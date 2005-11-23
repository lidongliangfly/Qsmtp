/** \file qrdata.c
 \brief send message body to remote host

 This file contains the functions to send the message body to the remote host.
 Both DATA and BDAT modes are supported. In DATA mode the message will be recoded
 to quoted-printable if neccessary.
 */
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include "netio.h"
#include "qremote.h"
#include "qrdata.h"
#include "version.h"
#include "mime.h"
#include "log.h"
#include "qoff.h"

const char *successmsg[] = {NULL, " accepted ", NULL, "message", "", "", "./Remote host said: ", NULL};
int ascii;			/* if message is plain ASCII or not */
const char *msgdata;		/* message will be mmaped here */
q_off_t msgsize;		/* size of the mmaped area */
static int lastlf = 1;		/* set if last byte sent was a LF */

/**
 * check if buffer has to be recoded for SMTP transfer
 *
 * @param buf buffer to scan
 * @param len length of buffer
 * @return logical or of: 1 if buffer has 8bit characters, 2 if buffer contains line longer 998 chars
 */
int
need_recode(const char *buf, q_off_t len)
{
	int res = 0;
	int llen = 0;

	/* if buffer is too short we don't need to check for long lines */
	while (len-- > 0) {
		if (buf[len] <= 0) {
			res = 1;
			llen++;
			break;
		} else if ((buf[len] == '\r') || (buf[len] == '\n')) {
			llen = 0;
		} else {
			llen++;
		}
		if (llen > 998) {
			res = 2;
			break;
		}
	}
	if (res == 1) {
		/* only scan for long lines */
		while ((len-- > 0) && (llen <= 998)) {
			if ((buf[len] == '\r') || (buf[len] == '\n')) {
				llen = 0;
			} else {
				llen++;
			}
		}
		if (llen > 998) {
			res = 3;
		}
	} else if (res == 2) {
		/* only scan for 8bit characters now */
		while (len-- > 0) {
			if (buf[len] <= 0) {
				res = 3;
				break;
			}
		}
	}
	return res;
}

/**
 * send message body, only fix broken line endings if present
 *
 * @param buf buffer to send
 * @param len length of data in buffer
 *
 * lastlf will be set if last 2 bytes sent were CRLF
 */
static void
send_plain(const char *buf, const q_off_t len)
{
	char sendbuf[1205];
	unsigned int idx = 0;
	size_t chunk = 0;	/* size of the chunk to copy into sendbuf */
	q_off_t off = 0;
	int llen = 0;		/* flag if start of line */

	if (!len)
		return;

	while (off < len) {
		while (idx + (q_off_t) chunk < sizeof(sendbuf) - 5) {
			if (off + (q_off_t) chunk == len) {
				break;
			}
			switch (buf[off + chunk]) {
				case '\r':	{
							int last = (off + (q_off_t) ++chunk == len);
		
							llen = 0;
							if (!last && (buf[off + chunk] == '\n')) {
								chunk++;
							} else {
								memcpy(sendbuf + idx, buf + off, chunk);
								off += chunk;
								idx += chunk;
								sendbuf[idx++] = '\n';
								chunk = 0;
							}
							break;
						}
				case '\n':	{
							/* bare '\n' */
							memcpy(sendbuf + idx, buf + off, chunk);
							off += chunk + 1;
							idx += chunk;
							sendbuf[idx++] = '\r';
							sendbuf[idx++] = '\n';
							chunk = 0;
							llen = 0;
							break;
						}
				case '.':	if (!llen) {
							chunk++;
							memcpy(sendbuf + idx, buf + off, chunk);
							off += chunk;
							idx += chunk;
							sendbuf[idx++] = '.';
							chunk = 0;
							break;
						}
						/* fallthrough */
				default:	chunk++;
						llen = 1;
			}
		}
		if (chunk) {
			memcpy(sendbuf + idx, buf + off, chunk);
			off += chunk;
			idx += chunk;
			chunk = 0;
		}

		if (len != off) {
			netnwrite(sendbuf, idx);
			lastlf = (sendbuf[idx - 1] == '\n');
			idx = 0;
		}
	}
	lastlf = (sendbuf[idx - 1] == '\n');
	netnwrite(sendbuf, idx);
}

static void
recodeheader(void)
{
	char buf[64 + heloname.len + strlen(VERSIONSTRING)];

	memcpy(buf, "Content-Transfer-Encoding: quoted-printable (recoded by: ", 57);
	memcpy(buf + 57, VERSIONSTRING, strlen(VERSIONSTRING));
	memcpy(buf + 57 + strlen(VERSIONSTRING), " at ", 4);
	memcpy(buf + 61 + strlen(VERSIONSTRING), heloname.s, heloname.len);
	buf[sizeof(buf) - 3] = ')';
	buf[sizeof(buf) - 2] = '\r';
	buf[sizeof(buf) - 1] = '\n';
	netnwrite(buf, sizeof(buf));
}

/**
 * scan and recode header: fix Content-Transfer-Encoding, check for boundary
 *
 * @param buf buffer to scan
 * @param len length of buffer
 * @param boundary if this is a multipart message a pointer to the boundary-string is stored here
 * @param multipart will be set to 1 if this is a multipart message
 * @return offset of end of header
 *
 * \warning boundary will not be 0-terminated! Use boundary->len!
 */
static q_off_t
qp_header(const char *buf, const q_off_t len, cstring *boundary, int *multipart)
{
	q_off_t off, header = 0;
	cstring cenc, ctype;

	STREMPTY(cenc);
	STREMPTY(ctype);
/* scan header */

	/* check if this is an empty header */
	if (buf[0] == '\r') {
		header = ((len > 1) && (buf[1] == '\n')) ? 2 : 1;
	} else if (buf[0] == '\n') {
		header = 1;
	}
	off = header;

	/* first: find the newline between header and body */
	while (!header && (off < len)) {
		switch (buf[off]) {
			case '\r':	off++;
					if ((off < len) && (buf[off] == '\n'))
						off++;
					if (off == len)
						break;
					if ((buf[off] == '\r') || (buf[off] == '\n')) {
						header = off;
					}
					break;
			case '\n':	off++;
					if (off == len)
						break;
					if ((buf[off] == '\r') || (buf[off] == '\n')) {
						header = off;
					}
					break;
			case 'c':
			case 'C':	{
						q_off_t rest = len - off;

						if ((rest >= 12) && !strncasecmp(buf + off + 1, "ontent-Type:", 11)) {
							const char *cr = buf + off;

							ctype.len = getfieldlen(cr, len - off);
							if (ctype.len) {
								ctype.s = cr;
								off += ctype.len - 2;
							}
							break;
						} else if ((rest >= 25) &&
								!strncasecmp(buf + off + 1, "ontent-Transfer-Encoding:", 25)) {
							const char *cr = buf + off;
			
							cenc.len = getfieldlen(cr, len - off);
							if (cenc.len) {
								cenc.s = cr;
								off += cenc.len - 2;
							}
							break;
						}
						/* fallthrough */
					}
			default:	off++;
					while ((off < len) && (buf[off] != '\r') && (buf[off] != '\n')) {
						off++;
					}
		}
	}
	if (!header || (need_recode(buf, header) & 1)) {
		/* no empty line found: treat whole message as header. But this means we have
		 * 8bit characters in header which is a bug in the client that we can't handle */
		write(1, "D5.6.3 message contains unencoded 8bit data in message header\n", 63);
		exit(0);
	}

#warning FIXME: fold long header lines if (need_redode() & 3)

	if ((*multipart = is_multipart(&ctype, boundary)) > 0) {
		/* content is implicitely 7bit if no declaration is present */
		if (cenc.len) {
			send_plain(buf, cenc.s - buf);
			send_plain(cenc.s + cenc.len, buf + header - cenc.s - cenc.len);
		} else {
			send_plain(buf, header);
		}
	} else if (*multipart < 0) {
		write(1, "D5.6.3 syntax error in Content-Type message header\n", 52);
		exit(0);
	} else {
		if (cenc.len) {
			send_plain(buf, cenc.s - buf);
			recodeheader();
			send_plain(cenc.s + cenc.len, buf + header - cenc.s - cenc.len);
		} else {
			recodeheader();
			send_plain(buf, header);
		}
	}
	return header;
}

/**
 * recode buffer to quoted-printable and send it to remote host
 *
 * @param buf data to send
 * @param len length of buffer
 */
static void
recode_qp(const char *buf, q_off_t len)
{
	unsigned int idx = 0;
	char sendbuf[1280];
	size_t chunk = 0;	/* size of the chunk to copy into sendbuf */
	q_off_t off = 0;
	int llen = 0;		/* length of this line, needed for qp line break */

	while (off < len) {
		while (idx + (q_off_t) chunk < sizeof(sendbuf) - 11) {
			if (off + (q_off_t) chunk == len) {
				break;
			}

			if (buf[off + chunk] == '\r') {
				chunk++;
				llen = 0;
				if (buf[off + chunk] == '\n') {
					chunk++;
				} else {
					chunk++;
					memcpy(sendbuf + idx, buf + off, chunk);
					off += chunk;
					idx += chunk;
					sendbuf[idx++] = '\n';
					chunk = 0;
				}
				continue;
			} else if (buf[off + chunk] == '\n') {
				memcpy(sendbuf + idx, buf + off, chunk);
				off += chunk + 1;
				idx += chunk;
				chunk = 0;
				sendbuf[idx++] = '\r';
				sendbuf[idx++] = '\n';
				llen = 0;
				continue;
			}

			/* add soft line break to make sure encoded line length < 80 */
			if (llen > 72) {
				chunk++;
				memcpy(sendbuf + idx, buf + off, chunk);
				off += chunk;
				idx += chunk;
				chunk = 0;
				/* recode last character if it was whitespace */
				if (sendbuf[idx - 1] == '\t') {
					sendbuf[idx - 1] = '=';
					sendbuf[idx++] = '0';
					sendbuf[idx++] = '9';
				} else if (sendbuf[idx - 1] == ' ') {
					sendbuf[idx - 1] = '=';
					sendbuf[idx++] = '2';
					sendbuf[idx++] = '0';
				}
				sendbuf[idx++] = '=';
				sendbuf[idx++] = '\r';
				sendbuf[idx++] = '\n';
				llen = 0;
			}

			if (!llen && (buf[off + chunk] == '.')){
				chunk++;
				memcpy(sendbuf + idx, buf + off, chunk);
				off += chunk;
				idx += chunk;
				sendbuf[idx++] = '.';
				chunk = 0;
			} else if ((buf[off + chunk] == '\t') || (buf[off + chunk] == ' ')) {
				/* recode whitespace if a linebreak follows */
				if ((off + (q_off_t) chunk < len) &&
						((buf[off + chunk + 1] == '\r') || (buf[off + chunk + 1] == '\n'))) {
					memcpy(sendbuf + idx, buf + off, chunk);
					off += chunk;
					idx += chunk;
					sendbuf[idx++] = '=';
					if (buf[off] == '\t') {
						sendbuf[idx++] = '0';
						sendbuf[idx++] = '9';
					} else {
						sendbuf[idx++] = '2';
						sendbuf[idx++] = '0';
					}
					sendbuf[idx++] = '\r';
					sendbuf[idx++] = '\n';
					if (buf[++off] == '\r')
						off++;
					if ((off < len) && (buf[off] == '\n'))
						off++;
					llen = 0;
					chunk = 0;
				} else {
					chunk++;
					llen++;
				}
			} else if ((buf[off + chunk] < 32) || (buf[off + chunk] == '=') ||
							 (buf[off + chunk] > 126)) {
				const char hexchars[] = "0123456789ABCDEF";

				/* recode non-printable and non-ascii characters */
				memcpy(sendbuf + idx, buf + off, chunk);
				off += chunk;
				idx += chunk;
				chunk = 0;
				sendbuf[idx++] = '=';
				sendbuf[idx++] = hexchars[(buf[off] >> 4) & 0x0f];
				sendbuf[idx++] = hexchars[buf[off] & 0xf];
				llen +=3;
				off++;
			} else {
				llen++;
				chunk++;
			}
		}
		if (chunk) {
			memcpy(sendbuf + idx, buf + off, chunk);
			off += chunk;
			idx += chunk;
			chunk = 0;
		}

		if (len != off) {
			netnwrite(sendbuf, idx);
			lastlf = (sendbuf[idx - 1] == '\n');
			idx = 0;
		}
	}
	lastlf = (sendbuf[idx - 1] == '\n');
	netnwrite(sendbuf, idx);
}

/**
 * skip transport padding after boundaries (trailing whitespace and [CR]LF)
 *
 * @param buf buffer to encode
 * @param len length of buffer
 * @return number of bytes skipped
 */
static q_off_t
skip_tpad(const char *buf, const q_off_t len)
{
	q_off_t off = 0;

	while ((off < len) && ((buf[off] == ' ') || (buf[off] == '\t')))
		off++;
	if ((off < len) && (buf[off] == '\r'))
		off++;
	if ((off < len) && (buf[off] == '\n'))
		off++;
	return off;
}

/**
 * send message body, do quoted-printable recoding where needed
 *
 * @param buf buffer to encode
 * @param len length of buffer
 */
static void
send_qp(const char *buf, const q_off_t len)
{
	q_off_t off = 0;
	cstring boundary;
	int multipart;		/* set to one if this is a multipart message */

	off = qp_header(buf, len, &boundary, &multipart);

	if (!multipart) {
		recode_qp(buf + off, len - off);
	} else {
		q_off_t nextoff = find_boundary(buf + off, len - off, &boundary);
		int nr;
		int islast = 0;	/* set to one if MIME end boundary was found */

		if (!nextoff) {
			/* huh? message declared as multipart, but without any boundary? */
			/* add boundary */
			netnwrite("\r\n--", 4);
			netnwrite(boundary.s, boundary.len);
			netnwrite("\r\n", 2);
			/* add Content-Transfer-Encoding header and extra newline */
			recodeheader();
			netnwrite("\r\n", 2);
			/* recode body */
			recode_qp(buf + off, len - off);
			/* add end boundary */
			netnwrite("\r\n--", 4);
			netnwrite(boundary.s, boundary.len);
			netnwrite("--\r\n", 4);
			lastlf = 1;
			return;
		}

		/* check and send or discard MIME preamble */
		if ( (nr = need_recode(buf + off, nextoff)) ) {
			log_write(LOG_ERR, "discarding invalid MIME preamble");
			netnwrite("\r\ninvalid MIME preamble was dicarded.\r\n\r\n--", 43);
			netnwrite(boundary.s, boundary.len);
			off += nextoff;
		} else {
			send_plain(buf + off, nextoff);
			off += nextoff;
		}

		if (buf[off] == '-') {
			/* wow: end-boundary as first boundary. What next? Flying cows? */

			/* first: add normal boundary to make this a more or less usefull MIME message, then add an end boundary */
			netnwrite("\r\n\r\n--", 6);
			netnwrite(boundary.s, boundary.len);
			netnwrite("--", 2);
			islast = 1;
			off += 2;
		}

		off += skip_tpad(buf + off, len - off);
		netnwrite("\r\n", 2);

		while ((off < len) && !islast && (nextoff = find_boundary(buf + off, len - off, &boundary))) {
			q_off_t partlen = nextoff - boundary.len - 2;

			nr = need_recode(buf + off, partlen);
			if (!(smtpext & 0x008) || (nr & 2)) {
				send_qp(buf + off, partlen);
			} else {
				send_plain(buf + off, partlen);
			}
			netnwrite("--", 2);
			netnwrite(boundary.s, boundary.len);
			off += nextoff;
			if (buf[off] == '-') {
				/* this is end boundary */
				netnwrite("--", 2);
				off += 2;
				islast = 1;
			}
			off += skip_tpad(buf + off, len - off);

			if ((off == len) && !islast) {
				netnwrite("--\r\n", 4);
				lastlf = 1;
				return;
			}
			netnwrite("\r\n", 2);
			if (off == len)
				return;
		}

		/* Look if we have seen the final MIME boundary yet. If not, add it. */
		if (!islast) {
			netnwrite("\r\n--", 4);
			netnwrite(boundary.s, boundary.len);
			netnwrite("--\r\n", 4);
		}

		/* All normal MIME parts are processed now, what follow is the epilogue.
		 * Check if it needs recode. If it does, it is broken and can simply be
		 * discarded */
		if (need_recode(buf + off, len - off)) {
			log_write(LOG_ERR, "discarding invalid MIME epilogue");
			netnwrite("\r\ninvalid MIME epilogue has been discarded.\r\n", 45);
			lastlf = 1;
		} else {
			send_plain(buf + off, len - off);
		}
	}
}

void
send_data(void)
{
	int num;

	successmsg[2] = "";
	netwrite("DATA\r\n");
	if ( (num = netget()) != 354) {
		write(1, num >= 500 ? "D5" : "Z4", 2);
		write(1, ".3.0 remote host rejected DATA command: ", 40);
		write(1, linein + 4, linelen - 3);
		quit();
	}
#ifdef DEBUG_IO
	in_data = 1;
#endif

	if ((!(smtpext & 0x008) && (ascii & 1)) || (ascii & 2)) {
		successmsg[2] = "(qp recoded) ";
		send_qp(msgdata, msgsize);
	} else {
		send_plain(msgdata, msgsize);
	}
	if (lastlf) {
		netnwrite(".\r\n", 3);
	} else {
		netnwrite("\r\n.\r\n", 5);
	}

#ifdef DEBUG_IO
	in_data = 0;
#endif
	checkreply("KZD", successmsg, 1);
}

void
send_bdat(void)
{
	char chunklen[6];
	const char *netmsg[] = {"BDAT ", NULL, NULL, NULL};
	q_off_t off = 0;

	successmsg[2] = "chunked ";

#define CHUNKSIZE 15000
	netmsg[1] = "15000";
	while (msgsize - off > CHUNKSIZE) {
		net_writen(netmsg);
#ifdef DEBUG_IO
		in_data = 1;
#endif
		netnwrite(msgdata + off, CHUNKSIZE);
#ifdef DEBUG_IO
		in_data = 0;
#endif
		if (checkreply(" ZD", NULL, 0) != 250)
			quit();
		off += CHUNKSIZE;
	}
	ultostr((unsigned long) (msgsize - off), chunklen);
	netmsg[1] = chunklen;
	netmsg[2] = " LAST";
	net_writen(netmsg);
#ifdef DEBUG_IO
	in_data = 1;
#endif
	netnwrite(msgdata + off, msgsize - off);
#ifdef DEBUG_IO
	in_data = 0;
#endif
	checkreply("KZD", successmsg, 1);
}
