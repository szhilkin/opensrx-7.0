/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <ctype.h>

enum {
	Bsz = 1<<16,
	Ssz = 1<<9,
};

char buf[Bsz];
char zbuf[Bsz];
char *name;
vlong soff;
int fflag;

void
seekordie(int fd, vlong off)
{
	if (seek(fd, off, 0) != off)
		sysfatal("%s: failed to seek to %lld\n", name, off);
}

void
fix(int fd, vlong offset, int length)
{
	vlong off;
	int len, pass = 0;

	fprint(2, "%s: block read error at offset %lld, sector %lld\n", name, offset, offset/Ssz);
	if (fflag) {
		fprint(2, "%s: zeroing out sectors %lld through %lld\n", name, offset/Ssz, (offset+length)/Ssz);
		seekordie(fd, offset);
		if (write(fd, zbuf, length) != length)
			sysfatal("%s: failed to zero out sectors %lld through %lld\n", name, offset/Ssz, (offset+length)/Ssz);
		return;
	}
verify:
	off = offset;
	len = length;
	seekordie(fd, off);
	for (; len > 0; len -= Ssz, off += Ssz) {
		if (read(fd, buf, Ssz) == Ssz)
			continue;
		if (pass)
			sysfatal("%s: failed to remap sector %lld\n", name, off/Ssz);
		seekordie(fd, off);
		fprint(2, "%s: zeroing out sector %lld\n", name, off/Ssz);
		if (write(fd, zbuf, Ssz) != Ssz)
			sysfatal("%s: failed to zero out sector %lld\n", name, off/Ssz);
	}
	if (pass)
		return;
	pass++;
	goto verify;
}

void
usage(void)
{
	fprint(2, "usage: %s [-s startsector] slot\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int fd, n;
	vlong fsz, off;
	Dir *dir;
	char *offset, *e;

	ARGBEGIN {
	case 's':
		offset = EARGF(usage());
		soff = strtoll(offset, &e, 10);
		if (*e != 0 || soff < 0 || soff*512 < 0)
			sysfatal("invalid sector offset %s\n", offset);
		fprint(2, "starting at sector offset %lld\n", soff);
		soff *= 512;
		break;
	case 'f':
		fflag++;
	} ARGEND;
	if (argc != 1)
		usage();
	name = *argv;
	snprint(buf, sizeof buf, "/raiddev/%s/data", name);
	fd = open(buf, ORDWR);
	if (fd < 0)
		sysfatal("cannot open %s\n", buf);
	dir = dirfstat(fd);
	if (dir == nil)
		sysfatal("cannot dirfstat %s\n", buf);
	if (soff > dir->length)
		sysfatal("invalid sector offset %lld\n", soff/512);
	fsz = dir->length;
	seekordie(fd, soff);
	off = soff;
	for (; off < fsz; off += n) {
		n = Bsz;
		if ((fsz - off) < Bsz)
			n = fsz - off;
		if (read(fd, buf, n) != n)
			fix(fd, off, n);
	}
	fprint(2, "%s: condition complete\n", name);
	exits(nil);
}
