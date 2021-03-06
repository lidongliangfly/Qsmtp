#include <qremote/conn.h>
#include <qremote/qremote.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct ips *
smtproute(const char *a __attribute__ ((unused)), const size_t b __attribute__ ((unused)),
		unsigned int *c __attribute__ ((unused)))
{
	return NULL;
}

void
write_status(const char *str)
{
	puts(str);

	exit(EFAULT);
}

void
write_status_m(const char **strs, const unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count - 1; i++)
		fputs(strs[i], stdout);

	write_status(strs[count - 1]);
}

void
err_mem(const int doquit __attribute__ ((unused)))
{
	(void) doquit;

	write_status("Z4.3.0 Out of memory.");

	exit(ENOMEM);
}

void
getrhost(const struct ips *m __attribute__ ((unused)), const unsigned short idx __attribute__ ((unused)))
{
	abort();
}

int
main(void)
{
	struct {
		const char *input;
		const char *expect;
		unsigned int prio;
	} patterns[] = {
		{
			.input = "127.0.0.1",
			.expect = "::ffff:127.0.0.1",
			.prio = 0
		},
		{
			.input = "::1",
			.expect = "::1",
#ifdef IPV4ONLY
			.prio = MX_PRIORITY_USED
#else
			.prio = 0
#endif
		},
		{
			.input = NULL,
			.expect = NULL,
			.prio = 0
		}
	};
	int ret = 0;

	for (unsigned int i = 0; patterns[i].input != NULL; i++) {
		struct ips *mx = NULL;
		struct in6_addr addr;
		char buf[32];

		snprintf(buf, sizeof(buf), "[%s]", patterns[i].input);

		getmxlist(buf, &mx);
#ifdef IPV4ONLY
		sortmx(&mx);
#endif

		if (inet_pton(AF_INET6, patterns[i].expect, &addr) <= 0)
			exit(EFAULT);

		if (mx == NULL) {
			fprintf(stderr, "%s was not parsed\n",
					patterns[i].input);
			ret++;
			continue;
		}

		if ((mx->next != NULL) || (mx->count > 1)) {
			fprintf(stderr, "addr %u returned multiple IPs\n",
					i);
			ret++;
			freeips(mx);
			continue;
		}

		if (!IN6_ARE_ADDR_EQUAL(&addr, mx->addr)) {
			fprintf(stderr, "addr %u was not parsed to correct result\n",
					i);
			ret++;
			freeips(mx);
			continue;
		}

		if (patterns[i].prio != mx->priority) {
			fprintf(stderr, "addr %u: found priority %u, expected %u\n",
					i, mx->priority, patterns[i].prio);
			ret++;
			freeips(mx);
			continue;
		}

		freeips(mx);
	}

	return ret;
}
