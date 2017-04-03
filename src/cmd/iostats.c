/*
 * Copyright © 2013 Coraid Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <bio.h>
#include "srxcmds.h"

typedef struct Disk Disk;
typedef struct Lun Lun;
typedef struct Ios Ios;

enum {
	Ird,
	Iwr,
	Isz,

	Lavg=0,
	Ltot,
	Lmax,
	Lsz,
};

struct Ios {
	int ns; /* number samples */
	uvlong bytes;
	vlong lat[Lsz];
};

struct Disk {
	Disk *next;
	char *name;
	char *path;
	Ios ios[Isz];
};

struct Lun {
	char name[8];
	Ios ios[Isz];
	Disk *disks;
};

Lun *luns[255];
int dflag;
int lflag;
int xflag;
int iosamp = 3;

#define rmnt "/raid"
#define rdmnt "/raiddev"
#define dprint(...) do { if (xflag) fprint(2, __VA_ARGS__); } while (0)

// format namples:total bytes:total latency: total avg /max/min - lazy parse.
void
parseio(Ios *ios, char *str)
{
	int n;
	char *a[8];

	n = getfields(str, a, nelem(a), 1, ":/");
	if (n == 5) {
		ios->ns = atoi(a[0]);
		if (ios->ns == 0)
			exits ("No samples acquired");
		if (ios->ns != iosamp)
			dprint("warning: iosample smaller than requested [%d != %d]\n", ios->ns, iosamp);
		ios->bytes = strtoll(a[1], 0, 0);
		ios->lat[Ltot] = strtoll(a[2], 0, 0)  / 1000;
		ios->lat[Lavg] = strtoll(a[3], 0, 0)  / 1000;
		ios->lat[Lmax] = strtoll(a[4], 0, 0) / 1000;
	}
}

int
readiostats(char *f, Ios *ios)
{
	int fd, n;
	char buf[256];
	char *a[4];

	fd = open(f, OREAD);
	if (fd < 0) {
		dprint("can't open %s: %r\n", f);
		return -1;
	}
	n = read(fd, buf, sizeof buf - 1);
	if (n <= 0) {
		close(fd);
		dprint("can't read %s: %r\n", f);
		return -1;
	}
	buf[n] = 0;
	n = getfields(buf, a, nelem(a), 1, "\t\r\n ");
	if (n != 2) {
		close(fd);
		dprint("unknown iostats file format for %s\n", f);
		return -1;
	}
	parseio(&ios[Ird], a[0]);
	parseio(&ios[Iwr], a[1]);
	close(fd);
	return 0;
}

char *
iopath(char *path)
{
	char *p, *e;
	static char buf[64];
	Dir *dir;

	p = buf;
	e = p + sizeof buf - 1;
	seprint(p, e, "%s", path);
	p = strrchr(buf, '/');
	if (p != nil) {
		p = seprint(++p, e, "iostats");
		*p = 0;
		dir = dirstat(buf);
		if (dir == nil)
			dprint("warning: can't stat iostat path for drive %s\n", path);
		else {
			free(dir);
			path = buf;
		}
	}
	return strdup(path);
}

void
readdisks(Lun *lun)
{
	Biobuf *bp;
	char buf[4096], *p, *f[16];
	int n, nd;
	Disk *d, *xd = nil;

	snprint(buf, sizeof buf, "%s/%s/raidstat", rmnt, lun->name);
	bp = Bopen(buf, OREAD);
	if (bp == nil) {
		dprint("can't Bopen %s: %r\n", buf);
		return;
	}
	while (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = 0;
		n = tokenize(p, f, nelem(f));
		n -= 6;
		if (n < 0) {
			dprint("raidstat format error for %s: %d\n", buf, n);
			break;
		}
		nd = atoi(f[2]);		// get # of disks in raid - o/w ditch raid line.
		for (; nd>0; nd--) {
			p = Brdline(bp, '\n');
			if (p == nil)
				break;
			p[Blinelen(bp)-1] = 0;
			n = tokenize(p, f, nelem(f));
			n -= 4;
			if (n < 0)
				break;
			d = mallocz(sizeof *d, 1);
			d->name = strdup(f[0]);
			d->path = iopath(f[3]);
			d->next = xd;
			xd = d;
		}
	}
	Bterm(bp);

	while (d = xd) {
		xd = d->next;
		readiostats(d->path, d->ios);
		d->next = lun->disks;
		lun->disks = d;
	}
}

void
readlun(int lno)
{
	Lun *lun;
	char buf[64];

	snprint(buf, sizeof buf, rmnt "/%d/iostats", lno);
	lun = mallocz(sizeof *lun, 1);
	snprint(lun->name, sizeof lun->name, "%d", lno);
	if (readiostats(buf, lun->ios) < 0) {
		free(lun);
		return;
	}
	readdisks(lun);
	luns[lno] = lun;
}

void
readluns(int argc, char *argv[])
{
	ulong i, n;
	char lun[8];

	if (argc == 0) {
		for (i=0; i<nelem(luns); i++) {
			snprint(lun, sizeof lun, "%ld", i);
			if (islun(lun))
				readlun(i);
		}
		return;
	}
	for (; argc > 0; argc--, argv++) {
		if (islun(*argv)) {
			n = strtoul(*argv, nil, 10);
			readlun(n);
		}
	}
}

char *
mbstr(char *p, int n, ulong bytes)
{
	enum { K= 1000, M= K*K, };
	ulong m, k;

	m = bytes / M;
	k = bytes % M / K;
	snprint(p, n, "%uld.%03uldMB", m, k);
	return p;
}

void
printlun(Lun *lun)
{
	Ios *r;
	Ios *w;
	Disk *d;
	char *fmt = "%-3s %10s %10s %4lldms %4lldms %10s %4lldms %4lldms\n";
	char rbuf[16], wbuf[16];
	static int nh;

	if (!nh++) {
		print("%-3s %10s %10s %6s %6s %10s %6s %6s\n",
			"LUN", "ELEMENT", "READ", "AVG", "MAX",
			"WRITE", "AVG", "MAX");
	}
	if (lflag) {
		r = &lun->ios[Ird];
		w = &lun->ios[Iwr];
		print(fmt, lun->name, "",
			mbstr(rbuf, 16, r->bytes), r->lat[Lavg], r->lat[Lmax],
			mbstr(wbuf, 16, w->bytes), w->lat[Lavg], w->lat[Lmax]);
	}
	if (dflag == 0)
		return;
	for (d=lun->disks; d; d=d->next) {
		r = &d->ios[Ird];
		w = &d->ios[Iwr];
		print(fmt, "", d->name,
			mbstr(rbuf, 16, r->bytes), r->lat[Lavg], r->lat[Lmax],
			mbstr(wbuf, 16, w->bytes), w->lat[Lavg], w->lat[Lmax]);
	}
}

void
printluns(int argc, char *argv[])
{
	ulong i, n;
	if (argc == 0) {
		for (i = 0; i < nelem(luns); i++)
			if (luns[i])
				printlun(luns[i]);
		return;
	}
	for (; argc > 0; argc--, argv++)
		if (islun(*argv)) {
			n = strtoul(*argv, nil, 10);
			printlun(luns[n]);
		}
		else
			print("error: LUN %s does not exist\n", *argv);
}

static void
iosamples(char *s)
{
	ulong n;
	char *e;

	n = strtoul(s, &e, 10);
	if (*e != '\0') {
		fprint(2, "invalid sample spec %s\n", s);
		exits("bads");
	}
	iosamp = n;
}

void
usage(void)
{
	fprint(2, "usage: %s [ -dl ] [ -s secs ] [ LUN ... ]\n", argv0);
	exits("usage");
}


void
setiosamples(void)
{
	int fd, i;
	char *ctl[] = { rmnt "/ctl", rdmnt "/ctl" };
	char err[ERRMAX], *invalid = "invalid sample spec";

	for (i=0; i<nelem(ctl); i++) {
		fd = open(ctl[i], OWRITE);
		if (fd < 0) {
			fprint(2, "unable to open %s to set iosample: %r\n", ctl[i]);
			exits("iosampfail");
		}
		if (fprint(fd, "iosamples %d\n", iosamp) < 0) {
			rerrstr(err, sizeof err);
			if (strncmp(err, invalid, strlen(invalid)) == 0)
				fprint(2, "error: sample must be an integer between 1 and 32 inclusive\n");
			else
				fprint(2, "unable to set iosample for %s: %r\n", ctl[i]);
			exits("iosampfail");
		}
		close(fd);
	}
}

void
main(int argc, char *argv[])
{
	ARGBEGIN {
	case 'd':
		dflag++;
		break;
	case 'l':
		lflag++;
		break;
	case 'x':
		xflag++;
		break;
	case 's':
		iosamples(EARGF(usage()));
		break;
	default:
		usage();
	} ARGEND

	if (dflag == 0 && lflag == 0)
		dflag = lflag = 1;

	setiosamples();
	readluns(argc, argv);
	printluns(argc, argv);
	exits(0);
}
