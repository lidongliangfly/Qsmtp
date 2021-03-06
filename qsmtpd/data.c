/** \file data.c
 \brief receive and queue message data
 */

#define _STD_SOURCE
#define _GNU_SOURCE
#include <qsmtpd/qsdata.h>

#include <fmt.h>
#include <log.h>
#include <netio.h>
#include <qsmtpd/antispam.h>
#include <qsmtpd/qsmtpd.h>
#include <qsmtpd/queue.h>
#include <qsmtpd/syntax.h>
#include <tls.h>
#include <version.h>

#include <errno.h>
#include <openssl/ssl.h>
#include <string.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define MAXHOPS		100		/* maximum number of "Received:" lines allowed in a mail (loop prevention) */

size_t maxbytes;			/* the maximum allowed size of message data */
static char datebuf[35] = ">; ";		/* the date for the From- and Received-lines */
static const char *loop_logmsg = "mail loop}";
static const char *loop_netmsg = "554 5.4.6 too many hops, this message is looping\r\n";


static inline void
two_digit(char *buf, int num)
{
	*buf = '0' + (num / 10);
	*(buf + 1) = '0' + (num % 10);
}

/**
 * write RfC822 date information to buffer
 *
 * @param buf buffer to store string in, must have at least 32 bytes free
 *
 * exactly 31 bytes in buffer are filled, it will _not_ be 0-terminated
 */
static void
date822(char *buf)
{
	time_t ti;
	struct tm stm;
	const char *weekday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	const char *month[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	long tz;

	ti = time(NULL);
	tzset();
	tz = timezone / 60;
	localtime_r(&ti, &stm);
	memcpy(buf, weekday[stm.tm_wday], 3);
	memcpy(buf + 3, ", ", 2);
	two_digit(buf + 5, stm.tm_mday);
	buf[7] = ' ';
	memcpy(buf + 8, month[stm.tm_mon], 3);
	buf[11] = ' ';
	ultostr(1900 + stm.tm_year, buf + 12);
	buf[16] = ' '; /* this will fail after 9999, but that I'll fix then */
	two_digit(buf + 17, stm.tm_hour);
	buf[19] = ':';
	two_digit(buf + 20, stm.tm_min);
	buf[22] = ':';
	two_digit(buf + 23, stm.tm_sec);
	buf[25] = ' ';
	buf[26] = (tz <= 0) ? '+' : '-';
	if (tz < 0)
	    tz = -tz;
	if (stm.tm_isdst > 0) {
		two_digit(buf + 27, 1 + (tz / 60));
	} else {
		two_digit(buf + 27, tz / 60);
	}
	two_digit(buf + 29, tz % 60);
}

#define WRITE(buf,len) \
		do { \
			if ( (rc = write(queuefd_data, buf, len)) < 0 ) { \
				return rc; \
			} \
		} while (0)

#define WRITEL(str)		WRITE(str, strlen(str))

/**
 * @brief write Received header line
 * @param chunked if message was transferred using BDAT
 */
static int
write_received(const int chunked)
{
	/* There are several cases in this functions where 2 strings are used
	 * where one string is exactly the begin or end of the other. For
	 * code clarity those are both spelled out. The compiler will usually
	 * find this out anyway and will allocate space only for one of them,
	 * and hopefully merge both code paths and use an offset or something
	 * like that. */
	int rc;
	size_t i = (authhide && is_authenticated_client()) ? 1 : 0;
	const char afterprot[]     =  "\n\tfor <";	/* the string to be written after the protocol */
	const char afterprotauth[] = "A\n\tfor <";	/* the string to be written after the protocol for authenticated mails*/

	/* write "Received-SPF: " line */
	if (!is_authenticated_client() && (relayclient != 1)) {
		if ( (rc = spfreceived(queuefd_data, xmitstat.spf)) )
			return rc;
	}
	/* write the "Received: " line to mail header */
	WRITEL("Received: from ");
	if (xmitstat.remotehost.len && !authhide)
		WRITE(xmitstat.remotehost.s, xmitstat.remotehost.len);
	else
		WRITEL("unknown");

	if (!i) {
		WRITEL(" ([");
		WRITE(xmitstat.remoteip, strlen(xmitstat.remoteip));
		if (xmitstat.remoteport) {
			WRITEL("]:");
			WRITEL(xmitstat.remoteport);
		} else {
			WRITEL("]");
		}
		if (xmitstat.helostr.len) {
			WRITEL(" HELO ");
			WRITE(xmitstat.helostr.s, xmitstat.helostr.len);
		}
	}
	if (xmitstat.authname.len) {
		const char authstr[] = ") (auth=";
		WRITEL(authstr + i);
		WRITE(xmitstat.authname.s, xmitstat.authname.len);
	} else if (xmitstat.tlsclient != NULL) {
		const char authstr[] = ") (cert=";
		WRITEL(authstr + i);
		WRITEL(xmitstat.tlsclient);
	} else if ((xmitstat.remoteinfo != NULL) && !i) {
		WRITEL(") (ident=");
		WRITEL(xmitstat.remoteinfo);
	}
	WRITEL(")\n\tby ");
	WRITE(heloname.s, heloname.len);
	WRITEL(" (" VERSIONSTRING ") with ");
	if (!xmitstat.esmtp) {
		WRITEL("SMTP");
	} else if (!ssl) {
		if (chunked)
			WRITEL("(chunked) ESMTP");
		else
			WRITEL("ESMTP");
	} else {
		const char *cipher = SSL_get_cipher(ssl);
		if (chunked)
			WRITEL("(chunked ");
		else
			WRITEL("(");
		/* avoid trouble in case SSL returns a NULL string */
		if (cipher)
			WRITEL(cipher);
		WRITEL(" encrypted) ESMTPS");
	}
	/* add the 'A' to the end of ESMTP or ESMTPS as described in RfC 3848 */
	if (xmitstat.authname.len != 0) {
		WRITEL(afterprotauth);
	} else {
		WRITEL(afterprot);
	}
	WRITE(TAILQ_FIRST(&head)->to.s, TAILQ_FIRST(&head)->to.len);
	date822(datebuf + 3);
	datebuf[34] = '\n';
	WRITE(datebuf, 35);
	return 0;
}

#undef WRITE
#define WRITE(buf, len) \
		do { \
			if ( (rc = write(queuefd_data, buf, len)) < 0 ) { \
				goto err_write; \
			} \
		} while (0)

/**
 * @brief check if header lines violate RfC822
 * @param headerflags flags which headers were already found
 * @param hdrname the header found on error
 * @return if processing should continue
 * @retval 0 nothing special found
 * @retval 1 a known header was found
 * @retval -2 a duplicate header was found (hdrname is set)
 * @retval -8 unencoded 8 bit data was found
 */
static int
check_rfc822_headers(unsigned int *headerflags, const char **hdrname)
{
	const char *searchpattern[] = { "Date:", "From:", "Message-Id:", NULL };
	int j;

	for (j = linein.len - 1; j >= 0; j--) {
		if (((signed char)linein.s[j]) < 0)
			return -8;
	}

	for (j = 0; searchpattern[j] != NULL; j++) {
		if (!strncasecmp(searchpattern[j], linein.s, strlen(searchpattern[j]))) {
			if ((*headerflags) & (1 << j)) {
				*hdrname = searchpattern[j];
				return -2;
			} else {
				*headerflags |= (1 << j);
				return 1;
			}
		}
	}

	return 0;
}

static unsigned long msgsize;

static void log_recips(const char *reason)
{
#define LOG_BRACES "] ("
	char s[ULSTRLEN + sizeof(LOG_BRACES)] = LOG_BRACES;		/* msgsize */
	const char *logmail[] = { "rejected message to <", NULL, "> from <", MAILFROM,
					"> from IP [", xmitstat.remoteip, s, " bytes) {",
					reason, NULL };
	struct recip *l;

	ultostr(msgsize, s + strlen(LOG_BRACES));
#undef LOG_BRACES

	TAILQ_FOREACH(l, &head, entries) {
		if (l->ok) {
			logmail[1] = l->to.s;
			log_writen(LOG_INFO, logmail);
		}
	}
}

/**
 * handle DATA command and store data into queue
 *
 * @return 0 on success, else error code
 */
int
smtp_data(void)
{
	const char *logreason = NULL;
	char logreason_buf[64];
	int i, rc;
	unsigned int headerflags = 0;	/* Date: and From: are required in header,
					 * else message is bogus (RfC 2822, section 3.6).
					 * We also scan for Message-Id here.
					 * RfC 2821 says server SHOULD NOT check for this,
					 * but we let the user decide.*/
#define HEADER_HAS_DATE 0x1
#define HEADER_HAS_FROM 0x2
#define HEADER_HAS_MSGID 0x4
	const char *errmsg = NULL;
	char errbuf[96];			/* for dynamically constructed error messages */
	unsigned int hops = 0;		/* number of "Received:"-lines */

	msgsize = 0;

	if (!goodrcpt) {
		tarpit();
		return netwrite("554 5.1.1 no valid recipients\r\n") ? errno : EDONE;
	}

	sync_pipelining();

	if ( (i = queue_init()) )
		return i;

	if (netwrite("354 Start mail input; end with <CRLF>.<CRLF>\r\n")) {
		int e = errno;

		queue_reset();
		return e;
	}
#ifdef DEBUG_IO
	in_data = 1;
#endif

	if ( (rc = write_received(0)) )
		goto err_write;

	/* loop until:
	 * -the message is bigger than allowed
	 * -we reach the empty line between header and body
	 * -we reach the end of the transmission
	 */
	if (net_read(1))
		goto loop_data;
	/* write the data to mail */
	while (!((linein.len == 1) && (linein.s[0] == '.')) && (msgsize <= maxbytes) && (linein.len > 0) && (hops <= MAXHOPS)) {
		unsigned int offset = 0;
		if (linein.s[0] == '.') {
			/* write buffer beginning at [1], we do not have to check if the second character
			 * is also a '.', RfC 2821 says only we should discard the '.' beginning the line */
			offset = 1;
		} else {
			int flagr = 1;	/* if the line may be a "Received:" or "Delivered-To:"-line */

			if ((xmitstat.check2822 & 1) || submission_mode) {
				const char *hdrname;
				switch (check_rfc822_headers(&headerflags, &hdrname)) {
				case 0:
					break;
				case 1:
					flagr = 0;
					break;
				case -2: {
					const char *errtext = "550 5.6.0 message does not comply to RfC2822: "
							"more than one '";

					strcpy(logreason_buf, "more than one '");
					strcat(logreason_buf, hdrname);
					strcat(logreason_buf, "' in header}");
					logreason = logreason_buf;

					errmsg = errbuf;
					memcpy(errbuf, errtext, strlen(errtext));
					memcpy(errbuf + strlen(errtext), hdrname, strlen(hdrname));
					memcpy(errbuf + strlen(errtext) + strlen(hdrname), "'\r\n", 4);
					goto loop_data;
				}
				case -8:
					logreason = "8bit-character in message header}";
					errmsg = "550 5.6.0 message does not comply to RfC2822: "
							"8bit character in message header\r\n";
					goto loop_data;
				}
			}
			if (flagr) {
				if (!strncasecmp("Received:", linein.s, 9)) {
					if (++hops > MAXHOPS) {
						logreason = loop_logmsg;
						errmsg = loop_netmsg;
						goto loop_data;
					}
				} else if ((linein.len >= 20) && !strncmp("Delivered-To:", linein.s, 13)) {
					/* we write it exactly this way, noone else is allowed to
					 * change our header lines so we do not need to use strncasecmp
					 *
					 * The minimum length of 21 are a sum of:
					 * 13: Delivered-To:
					 * 1: ' '
					 * 1: at least 1 character localpart
					 * 1: @
					 * 1: at least 1 character domain name
					 * 1: '.'
					 * 2: at least 2 characters top level domain */
					struct recip *np;

					TAILQ_FOREACH(np, &head, entries) {
						if (np->ok && !strcmp(linein.s + 14, np->to.s)) {
							logreason = loop_logmsg;
							errmsg = "554 5.4.6 message is looping, found a \"Delivered-To:\" line with one of the recipients\r\n";
							goto loop_data;
						}
					}
				}
			}
		}
		WRITE(linein.s + offset, linein.len - offset);
		msgsize += linein.len + 2 - offset;
		WRITEL("\n");
		/* this has to stay here and can't be combined with the net_read before the while loop:
		 * if we combine them we add an extra new line for the line that ends the transmission */
		if (net_read(1))
			goto loop_data;
	}
	if (submission_mode) {
		if (!(headerflags & HEADER_HAS_DATE)) {
			WRITEL("Date: ");
			WRITE(datebuf + 3, 32);
		}
		if (!(headerflags & HEADER_HAS_FROM)) {
			WRITEL("From: <");
			WRITE(xmitstat.mailfrom.s, xmitstat.mailfrom.len);
			WRITEL(">\n");
		}
		if (!(headerflags & HEADER_HAS_MSGID)) {
			char timebuf[20];
			struct timeval ti;
			struct timezone tz = { .tz_minuteswest = 0, .tz_dsttime = 0 };
			size_t l;

			WRITEL("Message-Id: <");

			gettimeofday(&ti, &tz);
			ultostr((const unsigned long) ti.tv_sec, timebuf);
			l = strlen(timebuf);
			timebuf[l] = '.';
			ultostr(ti.tv_usec, timebuf + l + 1);
			l += 1 + strlen(timebuf + l + 1);
			WRITE(timebuf, l);
			WRITEL("@");
			WRITE(msgidhost.s, msgidhost.len);
			WRITEL(">\n");
		}
	} else if (xmitstat.check2822 & 1) {
		if (!(headerflags & HEADER_HAS_DATE)) {
			logreason = "no 'Date:' in header}";
			errmsg = "550 5.6.0 message does not comply to RfC2822: 'Date:' missing\r\n";
			goto loop_data;
		} else if (!(headerflags & HEADER_HAS_FROM)) {
			logreason = "no 'From:' in header}";
			errmsg = "550 5.6.0 message does not comply to RfC2822: 'From:' missing\r\n";
			goto loop_data;
		}
	}

	if (linein.len == 0) {
		/* if (linein.len) message has no body and we already are at the end */
		WRITEL("\n");
		msgsize += 2;
		if (net_read(1))
			goto loop_data;
		while (((linein.len != 1) || (linein.s[0] != '.')) && (msgsize <= maxbytes)) {
			unsigned int offset;

			if ((xmitstat.check2822 & 1) && !xmitstat.datatype) {
				for (i = linein.len - 1; i >= 0; i--)
					if (((signed char)linein.s[i]) < 0) {
						logreason = "8bit-character in message body}";
						errmsg = "550 5.6.0 message contains 8bit characters\r\n";
						goto loop_data;
					}
			}

			offset = (linein.s[0] == '.') ? 1 : 0;
			WRITE(linein.s + offset, linein.len - offset);
			msgsize += linein.len + 2 - offset;

			WRITEL("\n");
			if (net_read(1))
				goto loop_data;
		}
	}
	if (msgsize > maxbytes) {
		logreason = "message too big}";
		errno = EMSGSIZE;
		errmsg = NULL;
		goto loop_data;
	}

#ifdef DEBUG_IO
	in_data = 0;
#endif

	if (!queue_envelope(msgsize, 0))
		return queue_result();

err_write:
	rc = errno;
	queue_reset();
	freedata();

/* first check, then read: if the error happens on the last line nothing will be read here */
	while ((linein.len != 1) || (linein.s[0] != '.')) {
		if (net_read(1))
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
	case E2BIG:
	case ENOMEM:
		break;
	case EPIPE:
		log_write(LOG_ERR, "broken pipe to qmail-queue");
		rc = EDONE;
		break;
	case EINVAL:	/* This errors happen if client sends invalid data (e.g. bad <CRLF> sequences). */
		return netwrite("500 5.5.2 bad <CRLF> sequence\r\n") ? errno : EBOGUS;
	default:	/* normally none of the other errors may ever occur. But who knows what I'm missing here? */
		{
			const char *logmsg[] = {"error in DATA: ", strerror(rc), NULL};

			log_writen(LOG_ERR, logmsg);
			rc = EDONE; // will not be caught in main
		}
	}

	return rc;
loop_data:
	rc = errno;
	if (logreason == NULL) {
		switch (rc) {
		case EINVAL:
			logreason = "bad CRLF sequence}";
			errmsg = "500 5.5.2 bad <CRLF> sequence\r\n";
			break;
		case E2BIG:
			logreason = "too long SMTP line}";
			break;
		default:
			logreason = "read error}";
		}
	}
	queue_reset();
	/* eat all data until the transmission ends. But just drop it and return
	 * an error defined before jumping here */
	while ((linein.len != 1) || (linein.s[0] != '.')) {
		msgsize += linein.len + 2;
		if (linein.s[0] == '.')
			msgsize--;
		net_read(1);
	}

	log_recips(logreason);
	freedata();

#ifdef DEBUG_IO
	in_data = 0;
#endif
	if (errmsg)
		return netwrite(errmsg) ? errno : EDONE;
	return rc;
}

#ifdef CHUNKING
#define CHUNK_READ_SIZE (INCOMING_CHUNK_SIZE * 1024)
static int bdaterr;
static int lastcr;

/**
 * handle BDAT command and store data into queue
 *
 * @return 0 on success, else error code
 */
int
smtp_bdat(void)
{
	int rc;
#warning FIXME: loop detection missing
	unsigned int hops = 0;		/* number of "Received:"-lines */
	unsigned long long chunksize;
	char *more;

	if (!goodrcpt) {
		tarpit();
		return netwrite("554 5.1.1 no valid recipients\r\n") ? errno : EDONE;
	}

	if ((linein.s[5] < '0') || (linein.s[5] > '9'))
		return EINVAL;
	errno = 0;
	chunksize = strtoull(linein.s + 5, &more, 10);
	if ((errno == ERANGE) || (*more && (*more != ' ')))
		return EINVAL;
	if (*more && strcasecmp(more + 1, "LAST"))
		return EINVAL;

	if (comstate != 0x0800) {
		msgsize = 0;
		comstate = 0x0800;
		lastcr = 0;

		bdaterr = queue_init();

		if (!bdaterr)
			bdaterr = write_received(1);
	}

	while (chunksize > 0) {
		size_t chunk;
		char inbuf[CHUNK_READ_SIZE];

		if (chunksize >= sizeof(inbuf)) {
			/* read one byte less so no end-of-buffer checks need to be done */
			chunk = net_readbin(sizeof(inbuf) - 1, inbuf);
		} else {
			chunk = net_readbin(chunksize, inbuf);
		}
		if (chunk == (size_t) -1) {
			if (!bdaterr)
				bdaterr = errno;
			break;
		} else if (chunk) {
			char *pos = inbuf;	/**< current input position */
			size_t rlen;	/**< remaining length */
			char *cr = pos;	/**< current CR position */

			chunksize -= chunk;
			msgsize += chunk;
			/* if the last chunk ended in CR and there is no LF right here then keep the CR */
			if (lastcr && (inbuf[0] != '\n'))
				WRITEL("\r");
			lastcr = (inbuf[chunk - 1] == '\r');
			if (lastcr)
				/* ignore the trailing CR, it will be handled separately */
				chunk--;
			rlen = chunk;
			/* make sure this will never match in any CRLF checks at end of buffer */
			inbuf[chunk] = '\0';

			/* handle all CRLF-terminated lines */
			while ((rlen > 0) && (cr != NULL)) {
				cr = memchr(cr, '\r', rlen);
				while ((cr != NULL) && (cr[1] != '\n')) {
					const ptrdiff_t o = cr - pos;
					cr = memchr(cr + 1, '\r', rlen - o);
				}
				if ((cr != NULL) && (cr[1] == '\n')) {
					/* overwrite CR with LF to keep number of writes low
					 * then write it all out */
					const ptrdiff_t l = cr - pos + 1;

					cr[0] = '\n';
					WRITE(pos, l);
					rlen -= l + 1; /* skip the original LF */
					cr += 2;
					pos = cr;
				}
			}

			/* handle everything after the last CRLF (if any) */
			if ((*more != '\0') && lastcr && (chunksize == 0)) {
				/* If this is the final chunk and it ended in CR than add it back here.
				 * The last byte in the buffer was never used before so this can't cause
				 * an overflow. */
				pos[rlen++] = '\r';
			}
			WRITE(pos, rlen);
		}
	}

	if ((msgsize > maxbytes) && !bdaterr) {
		log_recips("message too big}");
		bdaterr = EMSGSIZE;
		freedata();
	}
	/* send envelope data if this is last chunk */
	if (*more && !bdaterr) {
		if (queue_envelope(msgsize, 1))
			goto err_write;

		return queue_result();
	}

	if (bdaterr) {
		if (queuefd_hdr >= 0)
			queue_reset();
		freedata();
	} else if (hops > MAXHOPS) {
		log_recips(loop_logmsg);
		freedata();
		if (netwrite(loop_netmsg)) {
			bdaterr = -errno;
		} else
			bdaterr = -EDONE;
	} else {
		/* This returns the size as given by the client. It has successfully been parsed as number.
		 * and the contents of this message do not really matter, so we can just reuse that. This
		 * will not be done in case of the LAST chunk, that will go through queue_envelope() above. */
		const char *bdatmess[] = {"250 2.5.0 ", linein.s + 5, " octets received", NULL};

		bdaterr = -net_writen(bdatmess);
	}

	return bdaterr;
err_write:
	rc = errno;
	queue_reset();
	freedata();

	switch (rc) {
	case ENOSPC:
	case EFBIG:
		rc = EMSGSIZE;
		/* fallthrough */
	case EMSGSIZE:
	case E2BIG:
	case ENOMEM:
		return rc;
	}

	if (netwrite("451 4.3.0 error writing mail to queue\r\n"))
		return errno;

	switch (rc) {
	case EPIPE:
		log_write(LOG_ERR, "broken pipe to qmail-queue");
		break;
	default:	/* normally none of the other errors may ever occur. But who knows what I'm missing here? */
		{
			const char *logmsg[] = { "error in BDAT: ", strerror(rc), NULL };

			log_writen(LOG_ERR, logmsg);
		}
	}

	return EDONE; // will not be caught in main
}
#endif
