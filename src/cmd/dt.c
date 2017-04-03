// Copyright © 2010 Coraid, Inc.
// All rights reserved.
#include <u.h>
#include <libc.h>
#include <libcutil.h>
#include <bio.h>
#include "srxcmds.h"

enum {
	Nmib	= 256,
	Maxdisk	= 256,
};

int dusetab[Maxdisk];
int rflag, wflag, dflag, fflag, kflag, tflag;
int fd;
uchar buf[1024*1024];
uchar keybuf[512];
uvlong key;
uvlong offset;
int nmib = Nmib;

void
usage(void)
{
	fprint(2, "usage: dt [-rw] [-k key] [-t nsize] [-o offset] /path/to/file\n");
	exits("usage");
}

char *
rdwr(int w)
{
	return w ? "Write" : "Read";
}

int
path2dno(char *path)
{
	char *p;

	if (strcmp(path, "missing") == 0)
		return -1;
	if (strstr(path, "update") != nil)
		return -1;
	/*
	 *  normal case - it's a raid device of form /raiddev/SLOT/data
	 *  pull the slot(drive).
	 */
	if (p = strstr(path, "/data")) {
		*p = '\0';
		if (p = strrchr(path, '/'))
			return  atoi(++p);
	}
	fprint(2, "path2dno format assumption failure\n");
	return -1;
}

int
isinuse(int dno)
{
	return dusetab[dno];
}

void
loaddinuse(int lun)
{
	char *p, *f[10], path[64];
	int ndevs,dno,n,i;
	Biobuf *bp;

	snprint(path, sizeof path, "/raid/%d/raidstat", lun);
	bp = Bopen(path, OREAD);
	if (bp == nil)
		return;
	while (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = '\0';
		n = tokenize(p, f, nelem(f));
		if (n < 6) {
			fprint(2, "loaddinuse raid format error\n");
			break;
		}
		ndevs = atoi(f[2]);
		for (i=0;  i<ndevs;  i++) {
			p = Brdline(bp, '\n');
			if (p == nil)
				break;
			p[Blinelen(bp)-1] = '\0';
			n = tokenize(p, f, nelem(f));
			if (n < 4)
				break;
			dno = path2dno(f[3]);
			if (dno != -1 && dno < Maxdisk)
				dusetab[dno] = 1;
		}
		if (i  !=  ndevs) {
			fprint(2, "loaddinuse dev format error\n");
			break;
		}
	}
	Bterm(bp);
}

void
initdusetab(void)
{
	int fd, i, lunno, n;
	Dir *dp;
	Biobuf *bp;
	char *p, *r;
	
	fd = open("/raid", OREAD);
	if (fd < 0)
		return;
	n = dirreadall(fd, &dp);
	close(fd);
	for (i = 0; i < n; i++) {
		lunno = strtol(dp[i].name,&r,0);
		if (*r == '\0' && lunno < Maxdisk)
			loaddinuse(lunno);
	}

	free(dp);

	bp = Bopen("/raid/fcache", OREAD);
	if (bp == nil)
		return;
	while (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = '\0';
		for (r = p; *r && *r != ' '; ++r) ;
		*r = '\0';
		n = path2dno(p);
		if (n != -1 && n < Maxdisk)
			dusetab[n] = 1;
	}
	Bterm(bp);
}

void
initbufs(int w)
{
	Dir *dir;

	if (w)
		memset(buf, key, sizeof keybuf);
	else {
		memset(buf, 0x0, sizeof buf);
		memset(keybuf, key, sizeof keybuf);
	}
	if (tflag == 0) {
		if ((dir = dirfstat(fd)) == nil)
			sysfatal("cannot dirfstat\n");
		nmib = dir->length;
	}
}

void
iotest(uvlong off, int w)
{
	int n;
	vlong ns;
	uvlong o, i, bufsize;
	double d;

	if (kflag) {
		initbufs(w);
		off = 0;
		bufsize = sizeof keybuf;
	} else {
		bufsize = sizeof buf;
	}
	if (tflag)
		nmib >>= kflag ? 9 : 20;   // Sectors vs 1M chunks
	ns = nsec();
	for (i=0; i<nmib; i++) {
		o = off+i*bufsize;
		if (w)
			n = pwrite(fd, buf, bufsize, o);
		else
			n = pread(fd, buf, bufsize, o);
		if (n <= 0) {
			fprint(2, "%s error at offset %lld\n", rdwr(w), o);
			break;
		}
		if (kflag && w == 0) {
			if (memcmp(buf, keybuf, sizeof keybuf) != 0)
				errfatal("key mismatch at offset %lld, expected %lld, received %lld\n", o, key, legetv(buf));
		}
	}
	ns = nsec() - ns;
	d = i;
	d /= (double) ns / (1000*1000*1000);
	print("	%5s: %6.02f MiB/s\n", rdwr(w), d);
	if (dflag)
		print("		ns=%lld\n", ns);
}

/* formatted str to ll */
vlong
fstrtoll(char *str)
{
	vlong n;
	char *p;

	n = strtoll(str, &p, 10);
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
		break;
	default:		// assume sectors
		n <<= 9;
	}
	if (*p != '\0')
error:		sysfatal("invalid number format %s\n", str);
	return n;
}

void
main(int argc, char *argv[])
{
	int dno;
	char *e;

	ARGBEGIN {
	case 'd':
		dflag++;
		break;
	case 'f':
		fflag++;
		break;
	case 'o':
		offset = fstrtoll(EARGF(usage()));
		break;
	case 'r':
		rflag++;
		break;
	case 't':
		tflag++;
		nmib = fstrtoll(EARGF(usage()));
		break;
	case 'k':
		kflag++;
		key = strtoul(EARGF(usage()), &e, 10);
		if (*e != '\0' || key <= 0) {
			errfatal("invalid key value, input a non-zero positive interger");
		}
		break;
	case 'w':
		wflag++;
		break;
	default:
		usage();
	} ARGEND;
	if (argc != 1)
		usage();
	if ((fd = open(*argv, ORDWR)) < 0)
		sysfatal("cannot open %s for I/O\n", *argv);
	if ((rflag|wflag) == 0) {
		fprint(2, "At least one of -r (read test) or -w (write test) must be specified\n");
		usage();
	}
	initdusetab();
	if (rflag) {
		iotest(offset, 0);
		if (kflag)
			print("Data verification complete\n");
	}
	if (wflag) {
		dno = path2dno(*argv);
		if (dno == -1)
			sysfatal("path2dno failure");
		if (!fflag && isinuse(dno)) {
			fprint(2, "Device %d is in use, skipping write test\n", dno);
			exits("inuse");
		}
		iotest(offset, 1);
	}
	exits(0);
}
