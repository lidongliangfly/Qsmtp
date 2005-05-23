#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include "netio.h"
#include "log.h"
#include "qsmtpd.h"
#include "antispam.h"
#include "version.h"
#include "tls.h"

#define MAXHOPS		100		/* maximum number of "Received:" lines allowed in a mail (loop prevention) */

static const char *noqueue = "451 4.3.2 can not connect to queue\r\n";
static int fd0[2], fd1[2];		/* the fds to communicate with qmail-queue */
static pid_t qpid;			/* the pid of qmail-queue */

static int
err_pipe(void)
{
	log_write(LOG_ERR, "cannot create pipe to qmail-queue");
	return netwrite(noqueue) ? errno : 0;
}

static int
err_fork(void)
{
	log_write(LOG_ERR, "cannot fork qmail-queue");
	return netwrite(noqueue) ? errno : 0;
}

static const char *qqbin;

static int
queue_init(void)
{
	int i;

	if (pipe(fd0)) {
		if ( (i = err_pipe()) )
			return i;
		return EDONE;
	}
	if (pipe(fd1)) {
		/* EIO on pipe operations? Shit just happens (although I don't know why this could ever happen) */
		while (close(fd0[0]) && (errno == EINTR));
		while (close(fd0[1]) && (errno == EINTR));
		if ( (i = err_pipe()) )
			return i;
		return EDONE;
	}

	/* DJB uses vfork at this point (qmail.c::open_qmail) which looks broken
	 * because he modifies data before calling execve */
	switch (qpid = fork()) {
		case -1:	if ( (i = err_fork()) )
					return i;
				return EDONE;
		case 0:		if (!qqbin) {
					qqbin = getenv("QMAILQUEUE");
					if (!qqbin) {
						qqbin = "bin/qmail-queue";
					}
				}
				while ( (i = close(fd0[1])) ) {
					if (errno != EINTR)
						_exit(120);
				}
				while ( (i = close(fd1[1])) ) {
					if (errno != EINTR)
						_exit(120);
				}
				if (dup2(fd0[0], 0) == -1)
					_exit(120);
				if (dup2(fd1[0], 1) == -1)
					_exit(120);
				/* no chdir here, we already _are_ there (and qmail-queue does it again) */
				execlp(qqbin, qqbin, NULL);
				_exit(120);
		default:	while (close(fd0[0]) && (errno == EINTR));
				while (close(fd1[0]) && (errno == EINTR));
	}

/* check if the child already returned, which means something went wrong */
	if (waitpid(qpid, NULL, WNOHANG)) {
		/* error here may just happen, we are already in trouble */
		while (close(fd0[1]) && (errno == EINTR));
		while (close(fd1[1]) && (errno == EINTR));
		if ( (i = err_fork()) )
			return i;
		return EDONE;
	}
	return 0;
}

#define WRITE(fd,buf,len)	if ( (rc = write(fd, buf, len)) < 0 ) \
					return rc

static int
queue_header(void)
{
	int fd = fd0[1];
	int rc;
	char datebuf[36];			/* the date for the Received-line */
	time_t ti;
	size_t i;
	const char *afterprot = "A\n\tfor <";	/* the string to be written after the protocol */

/* write the "Received: " line to mail header */
	WRITE(fd, "Received: from ", 15);
	if (xmitstat.remotehost.s) {
		WRITE(fd, xmitstat.remotehost.s, xmitstat.remotehost.len);
	} else {
		WRITE(fd, "unknown", 7);
	}
	WRITE(fd, " ([", 3);
	WRITE(fd, xmitstat.remoteip, strlen(xmitstat.remoteip));
	WRITE(fd, "]", 1);
	if (xmitstat.helostr.len) {
		WRITE(fd, " HELO ", 6);
		WRITE(fd, xmitstat.helostr.s, xmitstat.helostr.len);
	}
	if (xmitstat.authname.len) {
		WRITE(fd, ") (auth=", 8);
		WRITE(fd, xmitstat.authname.s, xmitstat.authname.len);
	} else if (xmitstat.remoteinfo) {
		WRITE(fd, ") (", 3);
		WRITE(fd, xmitstat.remoteinfo, strlen(xmitstat.remoteinfo));
	}
	WRITE(fd, ")\n\tby ", 6);
	WRITE(fd, heloname.s, heloname.len);
	WRITE(fd, " (" VERSIONSTRING ") with ", 9 + strlen(VERSIONSTRING));
	WRITE(fd, protocol, strlen(protocol));
	/* add the 'A' to the end of ESMTP or ESMTPS as described in RfC 3848 */
	i = xmitstat.authname.len ? 0 : 1;
	WRITE(fd, afterprot + i, 8 - i);
	WRITE(fd, head.tqh_first->to.s, head.tqh_first->to.len);
	ti = time(NULL);
	i = strftime(datebuf, sizeof(datebuf), ">; %a, %d %b %Y %H:%M:%S %z\n", localtime(&ti));
	WRITE(fd, datebuf, i);
/* write "Received-SPF: " line */
	if (!(xmitstat.authname.len || xmitstat.tlsclient) && (relayclient != 1)) {
		if ( (rc = spfreceived(fd, xmitstat.spf)) )
			return rc;
	}
	return 0;
}

#undef WRITE
#define WRITE(fd,buf,len)	if ( (rc = write(fd, buf, len)) < 0 ) \
					goto err_write

static int
queue_envelope(const unsigned long msgsize)
{
	char s[ULSTRLEN];		/* msgsize */
	char t[ULSTRLEN];		/* goodrcpt */
	char bytes[] = " bytes, ";
	const char *logmail[] = {"received ", "", "message to <", NULL, "> from <", MAILFROM,
					"> ", "from ip [", xmitstat.remoteip, "] (", s, bytes,
					NULL, " recipients)", NULL};
	char *authmsg = NULL;
	int rc, e;
	int fd = fd1[1];

	if (ssl)
		logmail[1] = "encrypted ";
	ultostr(msgsize, s);
	if (goodrcpt > 1) {
		ultostr(goodrcpt, t);
		logmail[12] = t;
	} else {
		bytes[6] = ')';
		bytes[7] = '\0';
		/* logmail[13] is already NULL so that logging will stop there */
	}
/* print the authname.s into a buffer for the log message */
	if (xmitstat.authname.len) {
		if (strcasecmp(xmitstat.authname.s, MAILFROM)) {
			authmsg = malloc(xmitstat.authname.len + 23);

			if (!authmsg)
				return errno;
			memcpy(authmsg, "> (authenticated as ", 20);
			memcpy(authmsg + 20, xmitstat.authname.s, xmitstat.authname.len);
			memcpy(authmsg + 20 + xmitstat.authname.len, ") ", 3);
			logmail[6] = authmsg;
		} else {
			logmail[6] = "> (authenticated) ";
		}
	}

/* write the envelope information to qmail-queue */

	/* write the return path to qmail-queue */
	WRITE(fd, "F", 1);
	WRITE(fd, MAILFROM, xmitstat.mailfrom.len + 1);

	while (head.tqh_first != NULL) {
		struct recip *l = head.tqh_first;

		logmail[3] = l->to.s;
		if (l->ok) {
			log_writen(LOG_INFO, logmail);
			WRITE(fd, "T", 1);
			WRITE(fd, l->to.s, l->to.len + 1);
		}
		TAILQ_REMOVE(&head, head.tqh_first, entries);
		free(l->to.s);
		free(l);
	}
	WRITE(fd, "", 1);
err_write:
	e = errno;
	while ( (rc = close(fd)) ) {
		if (errno != EINTR) {
			e = errno;
			break;
		}
	}
	while (head.tqh_first != NULL) {
		struct recip *l = head.tqh_first;

		TAILQ_REMOVE(&head, head.tqh_first, entries);
		free(l->to.s);
		free(l);
	}
	freedata();
	free(authmsg);
	errno = e;
	return rc;
}

static int
queue_result(void)
{
	int status;

	while(waitpid(qpid, &status, 0) == -1) {
		/* don't know why this could ever happen, but we want to be sure */
		if (errno == EINTR) {
			log_write(LOG_ERR, "waitpid(qmail-queue) went wrong");
			return netwrite("451 4.3.2 error while writing mail to queue\r\n") ? errno : EDONE;
		}
	}
	if (WIFEXITED(status)) {
		int exitcode = WEXITSTATUS(status);

		if (!exitcode) {
			return netwrite("250 2.5.0 accepted message for delivery\r\n") ? errno : 0;
		} else {
			char ec[ULSTRLEN];
			const char *logmess[] = {"qmail-queue failed with exitcode ", ec, NULL};
			const char *netmsg;

			ultostr(exitcode, ec);
			log_writen(LOG_ERR, logmess);
 
			/* stolen from qmail.c::qmail_close */
			switch(exitcode) {
				case 11: netmsg = "554 5.1.3 envelope address too long for qq\r\n"; break;
				case 31: netmsg = "554 5.3.0 mail server permanently rejected message\r\n"; break;
				case 51: netmsg = "451 4.3.0 qq out of memory\r\n"; break;
				case 52: netmsg = "451 4.3.0 qq timeout\r\n"; break;
				case 53: netmsg = "451 4.3.0 qq write error or disk full\r\n"; break;
				case 54: netmsg = "451 4.3.0 qq read error\r\n"; break;
/*				case 55: netmsg = "451 4.3.0 qq unable to read configuration\r\n"; break;*/
/*				case 56: netmsg = "451 4.3.0 qq trouble making network connection\r\n"; break;*/
				case 61: netmsg = "451 4.3.0 qq trouble in home directory\r\n"; break;
				case 63:
				case 64:
				case 65:
				case 66:
				case 62: netmsg = "451 4.3.0 qq trouble creating files in queue\r\n"; break;
/*				case 71: netmsg = "451 4.3.0 mail server temporarily rejected message\r\n"; break;
				case 72: netmsg = "451 4.4.1 connection to mail server timed out\r\n"; break;
				case 73: netmsg = "451 4.4.1 connection to mail server rejected\r\n"; break;
				case 74: netmsg = "451 4.4.2 communication with mail server failed\r\n"; break;*/
				case 91: /* this happens when the 'F' and 'T' are not correctly sent.
					  * This is either a bug in qq but most probably a bug here */
				case 81: netmsg = "451 4.3.0 qq internal bug\r\n"; break;
				default:
					if ((exitcode >= 11) && (exitcode <= 40))
						netmsg = "554 5.3.0 qq permanent problem\r\n";
				else
					netmsg = "451 4.3.0 qq temporary problem\r\n";
			}
			return netwrite(netmsg) ? errno : EDONE;
		}
	} else {
		log_write(LOG_ERR, "WIFEXITED(qmail-queue) went wrong");
		return netwrite("451 4.3.2 error while writing mail to queue\r\n") ? errno : EDONE;
	}
}

int
smtp_data(void)
{
	char s[ULSTRLEN];		/* msgsize */
	const char *logmail[] = {"rejected message to <", NULL, "> from <", MAILFROM,
					"> from ip [", xmitstat.remoteip, "] (", s, " bytes) {",
					NULL, NULL};
	int i, rc;
	unsigned long msgsize = 0, maxbytes;
	int fd;
	int flagdate = 0, flagfrom = 0;	/* Date: and From: are required in header,
					 * else message is bogus (RfC 2822, section 3.6).
					 * RfC 2821 says server SHOULD NOT check for this,
					 * but we let the user decide */
	const char *errmsg = NULL;
	unsigned int hops = 0;		/* number of "Received:"-lines */

	if (badbounce || !goodrcpt) {
		tarpit();
		return netwrite("554 5.1.1 no valid recipients\r\n") ? errno : EDONE;
	}

	if ( (i = queue_init()) )
		return i;

	if ( (rc = hasinput()) ) {
		while (close(fd0[1]) && (errno == EINTR));
		while (close(fd1[1]) && (errno == EINTR));
		while ((waitpid(qpid, NULL, 0) == -1) && (errno == EINTR));
		return rc;
	}

	if (netwrite("354 Start mail input; end with <CRLF>.<CRLF>\r\n")) {
		int e = errno;

		while (close(fd0[1]) && (errno == EINTR));
		while (close(fd1[1]) && (errno == EINTR));
		while ((waitpid(qpid, NULL, 0) == -1) && (errno == EINTR));
		return e;
	}
#ifdef DEBUG_IO
	in_data = 1;
#endif
	if (databytes) {
		maxbytes = databytes;
	} else {
		maxbytes = -1UL - 1000;
	}

	/* fd is now the file descriptor we are writing to. This is better than always
	 * calculating the offset to fd0[1] */
	fd = fd0[1];
	if ( (rc = queue_header()) )
		goto err_write;

	/* loop until:
	 * -the message is bigger than allowed
	 * -we reach the empty line between header and body
	 * -we reach the end of the transmission
	 */
	if (net_read())
		goto err_write;
/* write the data to mail */
	while (!((linelen == 1) && (linein[0] == '.')) && (msgsize <= maxbytes) && linelen && (hops <= MAXHOPS)) {

		if (linein[0] == '.') {
			/* write buffer beginning at [1], we do not have to check if the second character 
			 * is also a '.', RfC 2821 says only we should discard the '.' beginning the line */
			WRITE(fd, linein + 1, linelen - 1);
			msgsize += linelen + 1;
		} else {
			int flagr = 1;	/* if the line may be a "Received:" or "Delivered-To:"-line */

			if (xmitstat.check2822 & 1) {
				if (!strncasecmp("Date:", linein, 5)) {
					if (flagdate) {
						logmail[9] = "more than one 'Date:' in header}";
						errmsg = "550 5.6.0 message does not comply to RfC2822: "
								"more than one 'Date:'\r\n";
						goto loop_data;
					} else {
						flagdate = 1;
						flagr = 0;
					}
				} else if (!strncasecmp("From:", linein, 5)) {
					if (flagfrom) {
						logmail[9] = "more than one 'From:' in header}";
						errmsg = "550 5.6.0 message does not comply to RfC2822: "
								"more than one 'From:'\r\n";
						goto loop_data;
					} else {
						flagfrom = 1;
						flagr = 0;
					}
				}
				for (i = linelen - 1; i >= 0; i--) {
					if (linein[i] < 0) {
						logmail[9] = "8bit-character in message header}";
						errmsg = "550 5.6.0 message does not comply to RfC2822: "
								"8bit character in message header\r\n";
						goto loop_data;
					}
				}
			}
			if (flagr) {
				if (!strncasecmp("Received:", linein, 9)) {
					if (++hops > MAXHOPS) {
						logmail[9] = "mail loop}";
						errmsg = "554 5.4.6 too many hops, this message is looping\r\n";
						goto loop_data;
					}
				} else if ((linelen > 20) && !strncmp("Delivered-To:", linein, 13)) {
					/* we write it exactly this way, noone else is allowed to
					 * change our header lines so we do not need to use strncasecmp
					 *
					 * The minimum length of 21 are a sum of:
					 * 13: Delivered-To:
					 * 1: ' '
					 * 1: at least 1 character localpart
					 * 1: @
					 * 2: at least 2 characters domain name
					 * 1: '.'
					 * 2: at least 2 characters top level domain */
					struct recip *np;

					for (np = head.tqh_first; np != NULL; np = np->entries.tqe_next) {
						if (np->ok && !strcmp(linein + 14, np->to.s)) {
							logmail[9] = "mail loop}";
							errmsg = "554 5.4.6 message is looping, found a \"Delivered-To:\" line with one of the recipients\r\n";
							goto loop_data;
						}
					}
				}
			}

			/* write buffer beginning at [0] */
			WRITE(fd, linein, linelen);
			msgsize += linelen + 2;
		}
		WRITE(fd, "\n", 1);
		/* this has to stay here and can't be combined with the net_read before the while loop:
		 * if we combine them we add an extra new line for the line that ends the transmission */
		if (net_read())
			goto err_write;
	}
	if (xmitstat.check2822 & 1) {
		if (!flagdate) {
			logmail[9] = "no 'Date:' in header}";
			errmsg = "550 5.6.0 message does not comply to RfC2822: 'Date:' missing\r\n";
			goto loop_data;
		} else if (!flagfrom) {
			logmail[9] = "no 'From:' in header}";
			errmsg = "550 5.6.0 message does not comply to RfC2822: 'From:' missing\r\n";
			goto loop_data;
		}
	}
	if (!linelen) {
		/* if(linelen) message has no body and we already are at the end */
		WRITE(fd, "\n", 1);
		if (net_read())
			goto err_write;
		while (!((linelen == 1) && (linein[0] == '.')) && (msgsize <= maxbytes)) {
			int offset;

			if ((xmitstat.check2822 & 1) && !xmitstat.datatype) {
				for (i = linelen - 1; i >= 0; i--)
					if (linein[i] < 0) {
						logmail[9] = "8bit-character in message body}";
						errmsg = "550 5.6.0 message contains 8bit characters\r\n";
						goto loop_data;
					}
			}

			offset = (linein[0] == '.') ? 1 : 0;
			WRITE(fd, linein + offset, linelen - offset);
			msgsize += linelen + 2 - offset;

			WRITE(fd, "\n", 1);
			if (net_read())
				goto err_write;
		}
	}
	if (msgsize > maxbytes) {
		logmail[9] = "message too big}";
		rc = EMSGSIZE;
		errmsg = NULL;
		goto loop_data;
	}
	/* the message body is sent to qmail-queue. Close the file descriptor and send the envelope information */
	while (close(fd)) {
		if (errno != EINTR)
			goto err_write;
	}
	fd0[1] = 0;
	if (queue_envelope(msgsize))
		goto err_write;

#ifdef DEBUG_IO
	in_data = 0;
#endif
	commands[7].state = (0x008 << xmitstat.esmtp);
	return queue_result();
loop_data:
	while (close(fd0[1]) && (errno == EINTR));
	fd0[1] = 0;
	/* eat all data until the transmission ends. But just drop it and return
	 * an error defined before jumping here */
	do {
		msgsize += linelen + 2;
		if (linein[0] == '.')
			msgsize--;
		if (net_read())
			goto err_write;
	} while ((linelen != 1) && (linein[0] != '.'));
	while (close(fd1[1]) && (errno == EINTR));
	ultostr(msgsize, s);

	while (head.tqh_first != NULL) {
		struct recip *l = head.tqh_first;

		TAILQ_REMOVE(&head, head.tqh_first, entries);
		if (l->ok) {
			logmail[1] = l->to.s;
			log_writen(LOG_INFO, logmail);
		}
		free(l->to.s);
		free(l);
	}
	freedata();

#ifdef DEBUG_IO
	in_data = 0;
#endif
	if (errmsg)
		return netwrite(errmsg) ? errno : EDONE;
	return rc;
err_write:
	rc = errno;
	if (fd0[1]) {
		while (close(fd0[1]) && (errno == EINTR));
	}
	while (close(fd1[1]) && (errno == EINTR));
	freedata();

/* first check, then read: if the error happens on the last line nothing will be read here */
	while ((linelen != 1) || (linein[0] != '.')) {
		if (net_read())
			break;
	}

#ifdef DEBUG_IO
	in_data = 0;
#endif
	if ((rc == ENOSPC) || (rc == EFBIG)) {
		rc = EMSGSIZE;
	} else if ((errno != ENOMEM) && (errno != EMSGSIZE) && (errno != E2BIG) && (errno != EINVAL)) {
		if (netwrite("451 4.3.0 error writing mail to queue\r\n"))
			return errno;
	}
	switch (rc) {
		case EMSGSIZE:
		case ENOMEM:	return rc;
		case EPIPE:	log_write(LOG_ERR, "broken pipe to qmail-queue");
				return EDONE;
		case EINTR:	log_write(LOG_ERR, "interrupt while writing to qmail-queue");
				return EDONE;
		/* This errors happen if client sends invalid data (e.g. bad <CRLF> sequences). 
		 * Let them pass, this will kick the client some lines later. */
		case EINVAL:	return netwrite("500 5.5.2 bad <CRLF> sequence\r\n") ? errno : EBOGUS;
		case E2BIG:	return rc;
		/* normally none of the other errors may ever occur. But who knows what I'm missing here? */
		default:	{
					const char *logmsg[] = {"error in DATA: ", strerror(rc), NULL};

					log_writen(LOG_ERR, logmsg);
					return EDONE; // will not be caught in main
				}
	}
}
