/** @file reply.c
 * @brief functions to read and parse server replies
 *
 * This file contains the functions to read and parse server replies, as well
 * as some helper functions.
 */

#include <log.h>
#include <netio.h>
#include <qremote/qremote.h>

#include <errno.h>
#include <string.h>
#include <syslog.h>

static int
err_network(int error)
{
	const char *logmsg[] = { "connection to ", rhost, NULL, NULL };

	switch (error) {
	case ETIMEDOUT:
		logmsg[2] = " timed out";
		break;
	case ECONNRESET:
		logmsg[2] = " died";
		break;
	default:
		return error;
	}

	log_writen(LOG_ERR, logmsg);

	return error;
}

int
netget(const unsigned int terminate)
{
	if (net_read(terminate)) {
		switch (errno) {
		case ENOMEM:
			err_mem(1);
		case EINVAL:
		case E2BIG:
			break;
		case ECONNRESET:
		case ETIMEDOUT:
			return -err_network(errno);
		default:
			if (terminate) {
				const char *tmp[] = { "Z4.3.0 ", strerror(errno) };

				write_status_m(tmp, 2);
				net_conn_shutdown(shutdown_clean);
			} else {
				int r = -errno;
				quitmsg();
				return r;
			}
		}
	} else {
		if ((linein.len > 3) && ((linein.s[3] == ' ') || (linein.s[3] == '-'))) {
			int r = linein.s[0] - '0';
			int q = linein.s[1] - '0';

			if ((r >= 2) && (r <= 5) && (q >= 0) && (q <= 9)) {
				r = r * 10 + q;
				q = linein.s[2] - '0';
				if ((q >= 0) && (q <= 9))
					return r * 10 + q;
			}
		}
	}

	if (terminate) {
		/* if this fails we're already in bad trouble */
		/* Even if 5.5.2 is a permanent error don't use 'D' return code here,
		 * hope that this is just a hiccup on the other side and will get
		 * fixed soon. */
		write_status("Z5.5.2 syntax error in server reply");
		net_conn_shutdown(shutdown_clean);
	} else {
		return -EINVAL;
	}
}

/* This function is only in this file to allow err_network() to be static. */

void
dieerror(int error)
{
	err_network(error);

	switch (error) {
	case ETIMEDOUT:
		write_status("Z4.4.1 connection to remote server timed out");
		break;
	case ECONNRESET:
		write_status("Z4.4.1 connection to remote server died");
		break;
	}
	net_conn_shutdown(shutdown_abort);
}
