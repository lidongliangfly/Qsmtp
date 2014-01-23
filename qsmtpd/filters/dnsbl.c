#include <qsmtpd/userfilters.h>

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include <qsmtpd/antispam.h>
#include "control.h"
#include "log.h"
#include "netio.h"
#include <qsmtpd/qsmtpd.h>
#include <qsmtpd/userconf.h>

int
cb_dnsbl(const struct userconf *ds, const char **logmsg, int *t)
{
	char **a;		/* array of domains and/or mailaddresses to block */
	int i;			/* counter of the array position */
	int rc = 0;		/* return code */
	const char *fnb;	/* filename of the blacklist file */
	const char *fnw;	/* filename of the whitelist file */
	char *txt = NULL;	/* TXT record of the rbl entry */

	if (connection_is_ipv4()) {
		fnb = "dnsbl";
		fnw = "whitednsbl";
	} else {
		fnb = "dnsblv6";
		fnw = "whitednsblv6";
	}

	*t = userconf_get_buffer(ds, fnb, &a, domainvalid, 1);
	if (*t < 0) {
		errno = -*t;
		return -1;
	} else if (*t == CONFIG_NONE) {
		return 0;
	}

	i = check_rbl(a, &txt);
	if (i >= 0) {
		int j, u;
		char **c;		/* same like **a, just for whitelist */

		u = userconf_get_buffer(ds, fnw, &c, domainvalid, 0);
		if (u < 0) {
			free(a);
			free(txt);
			errno = -u;
			return -1;
		} else if (u == CONFIG_NONE) {
			const char *netmsg[] = { "501 5.7.1 message rejected, you are listed in ",
						a[i], NULL, txt, NULL };
			const char *logmess[] = { "rejected message to <", THISRCPT, "> from <", MAILFROM,
						"> from IP [", xmitstat.remoteip, "] {listed in ", a[i], " from ",
						blocktype[*t], " dnsbl}", NULL };

			log_writen(LOG_INFO, logmess);
			if (txt)
				netmsg[2] = ", message: ";

			if ( ! (rc = net_writen(netmsg)) )
				rc = 1;
		} else {
			j = check_rbl(c, NULL);

			if (j >= 0) {
				const char *logmess[] = { "not rejected message to <", THISRCPT, "> from <", MAILFROM,
							"> from IP [", xmitstat.remoteip, "] {listed in ", a[i], " from ",
							blocktype[*t], " dnsbl, but whitelisted by ",
							c[i], " from ", blocktype[u], " whitelist}", NULL };
				log_writen(LOG_INFO, logmess);
			} else if (errno) {
				if (errno == EAGAIN) {
					*logmsg = "temporary DNS error on RBL lookup";
					rc = 4;
				} else {
					rc = j;
				}
			}
		}
	} if (errno) {
		if (errno == EAGAIN) {
			*logmsg = "temporary DNS error on RBL lookup";
			rc = 4;
		} else {
			rc = -1;
		}
	}

	free(a);
	free(txt);
	return rc;
}
