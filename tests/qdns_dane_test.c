#include <qdns_dane.h>

#include <qdns.h>

#include <assert.h>
#include <dns.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dns_transmit dns_resolve_tx;
static int err;

static char **passed_q;
static struct {
	const char *packet;
	size_t len;
	int ret;
} patterns[] = {
	/* too short packet */
	{
		.packet = "\0",
		.len = 1,
		.ret = -EINVAL
	},
	/* correct header length, no name */
	{
		.packet = "\0\0\0\0\0\0\0\0\0\0\0\0",
		.len = 12,
		.ret = -ENOTBLK
	},
	/* answers = 0 -> no entries */
	{
		.packet = "\0\0\0\0\0\0\0\0\0\0\0\0\0",
		.len = 13,
		.ret = 0
	},
	/* answers = 65535, but packet too short for any data */
	{
		.packet = "\0\0\0\0\0\0\xff\xff\0\0\0\0\0",
		.len = 13,
		.ret = -ENOTBLK
	},
	/* answers = 65535, but packet too short for subheader */
	{
		.packet = "\0\0\0\0\0\0\xff\xff\0\0\0\0\0\0\0\0\0\0\0",
		.len = 19,
		.ret = -EINVAL
	},
	/* answers = 1, one subpacket of data length 0, wrong type, wrong class */
	{
		.packet = "\0\0\0\0\0\0\0\1\0\0\0\0" /* header */
			"\0" /* first name */
			"\0\0\0\0" /* 4 more */
			"\0" /*name of subrecord */
			"\0\0\0\0\0\0\0\0\0\0", /* subheader */
		.len = 28,
		.ret = 0
	},
	/* answers = 1, one subpacket of data length 0, correct type, wrong class */
	{
		.packet = "\0\0\0\0\0\0\0\1\0\0\0\0" /* header */
			"\0" /* first name */
			"\0\0\0\0" /* 4 more */
			"\0" /*name of subrecord */
			"\0\64\0\0\0\0\0\0\0\0", /* subheader */
		.len = 28,
		.ret = 0
	},
	/* answers = 1, one subpacket of data length 0, correct type, correct class */
	{
		.packet = "\0\0\0\0\0\0\0\1\0\0\0\0" /* header */
			"\0" /* first name */
			"\0\0\0\0" /* 4 more */
			"\0" /*name of subrecord */
			"\0\64\0\1\0\0\0\0\0\0", /* subheader */
		.len = 28,
		.ret = -EINVAL
	},
	/* answers = 1, one subpacket of data length 65535, correct type, correct class */
	{
		.packet = "\0\0\0\0\0\0\0\1\0\0\0\0" /* header */
			"\0" /* first name */
			"\0\0\0\0" /* 4 more */
			"\0" /*name of subrecord */
			"\0\64\0\1\0\0\0\0\xff\xff", /* subheader */
		.len = 28,
		.ret = -EINVAL
	},
	/* answers = 1, one subpacket of data length 3, correct type, correct class */
	{
		.packet = "\0\0\0\0\0\0\0\1\0\0\0\0" /* header */
			"\0" /* first name */
			"\0\0\0\0" /* 4 more */
			"\0" /*name of subrecord */
			"\0\64\0\1\0\0\0\0\0\3", /* subheader */
		.len = 28,
		.ret = -EINVAL
	},
	/* answers = 1, one subpacket of data length 4, correct type, correct class */
	/* matching type says SHA2-256, for which the length is too small */
	{
		.packet = "\0\0\0\0\0\0\0\1\0\0\0\0" /* header */
			"\0" /* first name */
			"\0\0\0\0" /* 4 more */
			"\0" /*name of subrecord */
			"\0\64\0\1\0\0\0\0\0\4" /* subheader */
			"\1\1\1" /* TLSA header */
			"\0", /* TLSA data */
		.len = 32,
		.ret = -EINVAL
	},
	/* answers = 1, one subpacket of data length 36 */
	/* matching type says SHA2-256, for which the length is too large */
	{
		.packet = "\0\0\0\0\0\0\0\1\0\0\0\0" /* header */
			"\0" /* first name */
			"\0\0\0\0" /* 4 more */
			"\0" /*name of subrecord */
			"\0\64\0\1\0\0\0\0\0\44" /* subheader */
			"\1\1\1" /* TLSA header */
			"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", /* TLSA data */
		.len = 67,
		.ret = -EINVAL
	},
	/* answers = 2, one subpacket of data length 36, one of length 4 (invalid) */
	/* first matching type says SHA2-256 (valid) */
	{
		.packet = "\0\0\0\0\0\0\0\2\0\0\0\0" /* header */
			"\0" /* first name */
			"\0\0\0\0" /* 4 more */
			"\0" /*name of subrecord */
			"\0\64\0\1\0\0\0\0\0\43" /* subheader */
			"\1\1\1" /* TLSA header */
			"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" /* TLSA data */
			"\0" /*name of subrecord */
			"\0\64\0\1\0\0\0\0\0\3" /* subheader */
			"\1\1\1", /* TLSA header */
		.len = 80,
		.ret = -EINVAL
	}
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

int
dns_domain_fromdot(char **q, const char *host, unsigned int len)
{
	unsigned int idx;

	if (len != strlen(host))
		err++;

	assert(len > 3);

	if (strcmp(host + 3, "._tcp.foo.example.org") != 0) {
		err++;
		return 0;
	}

	if (host[0] != '_') {
		err++;
		return 0;
	}

	if ((host[1] < '0') || (host[1] > '9') || (host[2] < '0') || (host[2] > '9')) {
		err++;
		return 0;
	}

	idx = (host[1] - '1') * 10 + (host[2] - '0');

	if (idx < ARRAY_SIZE(patterns)) {
		dns_resolve_tx.packet = (char *)patterns[idx].packet;
		dns_resolve_tx.packetlen = patterns[idx].len;
	} else {
		dns_resolve_tx.packetlen = 0;
	}

	passed_q = q;

	return 1;
}

unsigned int
dns_packet_skipname(const char * c __attribute__ ((unused)), unsigned int len, unsigned int pos)
{
	if (len > pos)
		return pos + 1;

	/* this error code is junk, but is can easily be detected that
	 * this was the reason that the call failed */
	errno = ENOTBLK;
	return 0;
}

int
dns_resolve(const char *q, const char *type)
{
	assert(type != 0);
	assert(type[0] == 0);
	assert(type[1] == 52);
	assert(q == *passed_q);

	if (dns_resolve_tx.packetlen == 0) {
		errno = ENOMEM;
		return -1;
	} else {
		return 0;
	}
}

void
dns_domain_free(char **q)
{
	assert(*q == *passed_q);
}

void
dns_transmit_free(struct dns_transmit *t)
{
	assert(t == &dns_resolve_tx);
}

int
main(void)
{
	struct daneinfo *val = (struct daneinfo *)(uintptr_t)-1;
	unsigned short i;

	if (dnstlsa("foo.example.org", 48, NULL) != DNS_ERROR_LOCAL)
		err++;

	if (dnstlsa("foo.example.org", 48, &val) != DNS_ERROR_LOCAL)
		err++;

	if (val != NULL)
		err++;

	errno = 0;
	for (i = 0; i < ARRAY_SIZE(patterns); i++) {
		const int s = dnstlsa("foo.example.org", i + 10, NULL);
		const int r = dnstlsa("foo.example.org", i + 10, &val);

		if (r != s) {
			fprintf(stderr, "dnstlsa(x, %u, NULL) returned %i, but dnstlsa(x, %u, &val) returned %i\n",
					i + 10, s, i + 10, r);
			err++;
		}

		if (patterns[i].ret < 0) {
			if ((r != DNS_ERROR_LOCAL) || (errno != -patterns[i].ret)) {
				fprintf(stderr, "dnstlsa(x, %u, ...) returned %i/%i, but expected was %i/%i\n",
						i + 10, r, errno, DNS_ERROR_LOCAL, -patterns[i].ret);
				err++;
			}
		} else {
			if (r != patterns[i].ret) {
				fprintf(stderr, "dnstlsa(x, %u, ...) returned %i/%i, but expected was %i\n",
						i +  10, r, errno, patterns[i].ret);
				err++;
			}
		}

		if (r > 0) {
			int j;

			for (j = 0; j < r; j++)
				free(val[j].data);
			free(val);
		}
		errno = 0;
	}

	return err;
}