#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <syslog.h>
#include "antispam.h"
#include "usercallback.h"
#include "control.h"
#include "dns.h"
#include "log.h"
#include "netio.h"
#include "qsmtpd.h"

int
cb_namebl(const struct userconf *ds, char **logmsg, int *t)
{
	char *b;		/* buffer to read file into */
	char **a;		/* array of domains and/or mailaddresses to block */
	int i = 0;		/* counter of the array position */
	int rc;			/* return code */
	int fd;			/* file descriptor of the policy file */
	char *txt = NULL;	/* TXT record of the rbl entry */
	const char *netmsg[] = {"501 5.7.1 message rejected, you are listed in ",
				NULL, NULL, NULL, NULL};
	const char *logmess[] = {"rejected message to <", THISRCPT, "> from <", MAILFROM,
				"> from IP [", xmitstat.remoteip, "] {listed in ", NULL, " from ",
				blocktype[*t], " namebl}", NULL};
	int flagtemp = 0;	/* true at least one list failed with temporary error */
	char *fromdomain;

	if (!xmitstat.mailfrom.len)
		return 0;

	if ( (fd = getfileglobal(ds, "namebl", t)) < 0)
		return (errno == ENOENT) ? 0 : -1;

	if ( (rc = loadlistfd(fd, &b, &a, domainvalid)) < 0)
		return rc;
	
	if (!b)
		return 0;

	fromdomain = strchr(xmitstat.mailfrom.s, '@') + 1;

	rc = 1;
	/* Beware: rc has opposite meaning (0 == match) ! */
	while (a[i] && rc) {
		char *d = fromdomain;
		unsigned int alen = strlen(a[i]) + 1;

		while (d && rc) {
			unsigned int dlen = strlen(d);
			char blname[256];

			if (dlen + alen < 256) {
				memcpy(blname, d, dlen);
				blname[dlen++] = '.';
				memcpy(blname + dlen, a[i], alen);

				rc = ask_dnsa(blname, NULL);
				if (rc < 0) {
					goto out;
				} else if (!rc) {
					/* if there is any error here we just write the generic message to the client
					* so that's no real problem for us */
					dnstxt(&txt, blname);
				} else if (rc == 2) {
					flagtemp = 1;
				}
				/* ask_dnsa returns 0 on success, that means we have a match */
				
			}
			d = strchr(d, '.');
			if (d)
				d++;
		}
	}

	if (!rc) {
		logmess[7] = a[--i];
		log_writen(LOG_INFO, logmess);
		netmsg[1] = a[i];
		if (txt) {
			netmsg[2] = ", message: ";
			netmsg[3] = txt;
		}
		if (! (rc = net_writen(netmsg)) )
			rc = 1;
	} else if (flagtemp) {
		*logmsg = "temporary DNS error on RBL lookup";
		rc = 4;
	} else {
		rc = 0;
	}
out:
	free(a);
	free(b);
	free(txt);
	return rc;
}
