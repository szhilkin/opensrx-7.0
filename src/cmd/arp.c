/* 
 * Copyright © 2010 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <ip.h>

enum {
	Neth= 14,
	Narp= 24,
};

typedef struct Eth Eth;
struct Eth {
	uchar dst[6];
	uchar src[6];
	uchar type[2];
};

typedef struct Arp Arp;
struct Arp
{
	uchar hrd[2];
	uchar pro[2];
	uchar hln;
	uchar pln;
	uchar op[2];
	uchar sha[6];
	uchar spa[4];
	uchar tha[6];
	uchar tpa[4];
};

int dflag;
uchar cbuf[60];
uchar rbuf[60];

void
dumparp(uchar *p)
{
	Eth *e;
	Arp *a;

	e = (Eth *) p;
	a = (Arp *) &p[Neth];
	fprint(2, "dst=%E src=%E t=%ux\n", e->dst, e->src, nhgets(e->type));
	fprint(2, "hrd=%ux pro=%ux hln=%ux pln=%ux op=%ux\n",
		nhgets(a->hrd), nhgets(a->pro), a->hln, a->pln, nhgets(a->op));
	fprint(2, "sha=%E spa=%V tha=%E tpa=%V\n\n",
		a->sha, a->spa, a->tha, a->tpa);	
}

void
arp(int fd, char *ip, uchar *mac)
{
	Eth *e;
	Arp *a, *ar;


	e = (Eth *) cbuf;
	a = (Arp *) &cbuf[Neth];

	memset(e->dst, 0xff, 6);
	hnputs(e->type, 0x806);

	hnputs(a->hrd, 1);
	hnputs(a->pro, 0x800);
	a->hln = 6;
	a->pln = 4;
	hnputs(a->op, 1);
	memcpy(a->sha, mac, 6);
	v4parseip(a->tpa, ip);

	alarm(3*1000);
	write(fd, cbuf, sizeof cbuf);
	if (dflag)
		dumparp(cbuf);
	while (read(fd, rbuf, sizeof rbuf) > 0) {
		ar = (Arp *) &rbuf[Neth];
		if (nhgets(ar->op) != 2)
			continue;
		if (dflag)
			dumparp(rbuf);
		if (memcmp(ar->spa, a->tpa, 4) == 0)
			print("%E\n", ar->sha);
	}
	alarm(0);
}

int
subipok(char *p)
{
	char *end;
	long n;

	if (!p)
		return 0;

	n = strtol(p, &end, 10);
	if (end == p || *end)
		return 0;
	if (n > 255 || n < 0)
		return 0;
	return 1;
}

int
isipv4(char *ip)
{
	char buf[32];

	strncpy(buf, ip, sizeof buf);
	buf[sizeof buf - 1] = '\0';

	if (subipok(strtok(buf, ".")))
	if (subipok(strtok(nil, ".")))
	if (subipok(strtok(nil, ".")))
	if (subipok(strtok(nil, "\0")))
		return 1;
	return 0;	
}

void
usage(void)
{
	fprint(2, "usage: %s ether ip\n", argv0);
	exits("usage");
}
	
void
main(int argc, char *argv[])
{
	char buf[64];
	uchar mac[6];
	int fd;

	fmtinstall('E', eipfmt);
	fmtinstall('V', eipfmt);

	ARGBEGIN {
	case 'd':
		dflag++;
		break;
	default:
		usage();
	} ARGEND

	if (argc != 2)
		usage();
	snprint(buf, sizeof buf, "%s!0x806", argv[0]);
	fd = dial(buf, 0, 0, 0);
	if (fd < 0) {
		fprint(2, "cannot dial %s: %r\n", argv[0]);
		exits("dial");
	}
	if (myetheraddr(mac, argv[0]) < 0) {
		fprint(2, "cannot fetch addr for %s: %r\n", argv[0]);
		exits("addr");
	}
	if (!isipv4(argv[1])) {
		fprint(2, "%s is not an IPv4 address\n", argv[1]);
		exits("badip");
	}
	arp(fd, argv[1], mac);
}

