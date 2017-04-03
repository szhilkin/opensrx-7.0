// Copyright © 2010 Coraid, Inc.
// All rights reserved.
#include <u.h>
#include <libc.h>
#include <ctype.h>

#define raidctl "/raid/ctl"
#define flashrl "/n/kfs/srx/ratelimit"
#define dprint(...) if (!dflag) USED(dflag); else fprint(2, __VA_ARGS__)

int dflag;
enum { KB= 1024, MB=KB*KB, GB= MB*KB, };

/* formatted str to ll */
vlong
fstrtoll(char *str)
{
	vlong n;
	char *p;

	n = strtoll(str, &p, 0);
	if (n < 0 || p == str)
		goto error;
	switch (*p) {
	case 'g':
	case 'G':
		n <<= 10;
	case 'm':
	case 'M':
		n <<= 10;
	case 'K':
	case 'k':
		n <<= 10;
		p++;
	}
	if (*p != '\0')
error:	  sysfatal("invalid number format %s\n", str);
	return n;
}

void
usage(void)
{
	fprint(2, "usage: %s [ limit ]\n", argv0);
	exits("usage");
}

void
setrate(vlong rate)
{
	int fd, n;
	char buf[128];

	snprint(buf, nelem(buf), "rlrate %lld\n", rate);
	fd = open(raidctl, OWRITE);
	if (fd < 0)
		sysfatal("cannot open raid ctl file: %r");
	n = strlen(buf);
	if (write(fd, buf, n) != n)
		sysfatal("raid ctl write failure: %r\n");
	snprint(buf, nelem(buf), "%lld", rate);
	n = strlen(buf);
	fd = create(flashrl, OWRITE, 0660);
	if (fd < 0 || write(fd, buf, n) != n)
		sysfatal("cannot save ratelimit to flash for persistent state");
	
}

/* AV functions duplicated in show.c.  Probably needs a library someday. */

typedef struct AV AV;
struct AV {
	AV		*next;
	char		*attr;
	char		*value;
};

void
listav(char *file, AV *p)
{
	dprint("listav: %s:\n", file);
	while (p) {
		dprint("\t%s--%s\n", p->attr, p->value);
		p = p->next;
	}
}

AV *
readavl(char *file)
{
	char buf[2048];
	int fd, n;
	char *cp, *cp1, *cp2;
	AV *head = nil, *tail = nil, *avp;

	fd = open(file, OREAD);
	if (fd < 0) {
		dprint("readavl: open(%s, OREAD) failure: %r\n", file);
		return nil;
	}
	n = read(fd, buf, sizeof buf);
	if (n <= 0) {
		dprint("readavl: read %s failure: %r\n", file);
		return nil;
	}
	close(fd);
	buf[n] = 0;
	cp = buf;
	do {
		avp = mallocz(sizeof *avp, 1);
		if (head == nil)
			head = avp;
		else
			tail->next = avp;
		tail = avp;
		cp1 = strchr(cp, ':');
		assert(cp1);
		*cp1++ = 0;
		while (*cp1 && isspace(*cp1))
			cp1++;
		cp2 = strchr(cp1, '\n');
		assert(cp2);
		*cp2++ = 0;
		avp->attr = strdup(cp);
		avp->value = strdup(cp1);
		cp = cp2;
	} while (*cp);
	listav(file, head);
	return head;
}

char *
getav(AV *p, char *attr)		// return value of attr
{
	while (p) {
		if (strcmp(p->attr, attr) == 0)
			return p->value;
		p = p->next;
	}
	return nil;
}

void
printrate(void)
{
	AV *avl;
	vlong rate;
	char *p;

	avl = readavl(raidctl);
	if (avl == nil)
		sysfatal("unable to read raid ctl avl");
	p = getav(avl, "rlrate");
	if (p == nil)
		sysfatal("unable to find rlrate in raid ctl");
	rate = fstrtoll(p);
	if (rate == 0)
		print("rate limiting is off\n");
	else if (rate < KB)
		print("%lld Bytes/s\n", rate);
	else if (rate < MB)
		print("%lld KB/s\n", rate/KB);
	else if (rate < 2 * (uvlong)GB)
		print("%lld MB/s\n", rate/MB);
	else
		print("%lld GB/s\n", rate/GB);
}

void
main(int argc, char *argv[])
{
	vlong rate;

	ARGBEGIN {
	case '?':
		usage();
		break;
	case 'd':
		dflag++;
		break;
	} ARGEND

	switch (argc) {
	case 0:
		printrate();
		break;
	case 1:
		if (!isdigit(*argv[0])) {
			fprint(2, "error: limit must begin with a digit\n");
			usage();
		}
		rate = fstrtoll(argv[0]);
		if (rate < 0 || rate > (vlong) 2*1024*1024*1024) {
			fprint(2, "error: invalid rate specification [0-2g]\n");
			usage();
		}
		setrate(rate);
		break;
	default:
		usage();
	}
	exits(0);
}
