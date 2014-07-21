/** \file conn.c
 \brief functions for establishing connection to remote SMTP server
 */

#include <qremote/conn.h>

#include <log.h>
#include <netio.h>
#include <qdns.h>
#include <qremote/client.h>
#include <qremote/greeting.h>
#include <qremote/qremote.h>
#include <qremote/starttlsr.h>

#include <errno.h>
#include <syslog.h>
#include <unistd.h>

void
connect_mx(struct ips *mx, const struct in6_addr *outip4, const struct in6_addr *outip6)
{
	/* for all MX entries we got: try to enable connection, check if the SMTP server wants us
	 * (sends 220 response) and EHLO/HELO succeeds. If not, try next. If none left, exit. */
	do {
		int flagerr = 0;
		int s;

		if (socketd >= 0)
			close(socketd);
		socketd = tryconn(mx, outip4, outip6);
		dup2(socketd, 0);
		getrhost(mx);

		s = netget(1);

		/* consume the rest of the replies */
		while (linein.s[3] == '-') {
			int t = netget(0);

			flagerr |= (s != t);
			if (t > 0)
				continue;

			/* if the reply was invalid in itself (i.e. parse error or such)
			 * we can't know what the remote server will do next, so break out
			 * and immediately send quit. Since the initial result of netget()
			 * must have been positive flagerr will always be set here. */
			break;
		}
		if ((s != 220) || (flagerr != 0)) {
			if (flagerr) {
				const char *dropmsg[] = {"invalid greeting from ", rhost, NULL};

				log_writen(LOG_WARNING, dropmsg);
			}
			quitmsg();
			continue;
		}

		flagerr = greeting();
		if (flagerr < 0) {
			quitmsg();
			continue;
		}

		smtpext = flagerr;

		if (smtpext & esmtp_starttls) {
			flagerr = tls_init();
			/* Local error, this would likely happen on the next host again.
			 * Since it's a local fault stop trying and hope it gets fixed. */
			if (flagerr < 0)
				net_conn_shutdown(shutdown_clean);

			if (flagerr != 0) {
				quitmsg();
				continue;
			}

			flagerr = greeting();

			if (flagerr < 0) {
				quitmsg();
				continue;
			} else {
				smtpext = flagerr;
			}
		}
		
	} while (socketd < 0);
}