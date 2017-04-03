/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>

enum {
	Bsz = 1<<16,
	Ssz = 1<<9,
};

char buf[Bsz];
char zbuf[Ssz];
typedef struct Disk Disk;
struct Disk {
	char *name;
	int fd;
	Dir *dir;
} od, nd;

static int fflag;
static int sflag;

void
seekordie(Disk *d, vlong off)
{
	if (seek(d->fd, off, 0) != off)
		sysfatal("%s: failed to seek to %lld\n", d->name, off);
}

void
subrw(vlong offset, int length)
{
	vlong off;
	int len;
	char *wp;

	fprint(2, "%s: block read error at offset %lld, sector %lld\n", od.name, offset, offset/Ssz);
	off = offset;
	len = length;
	for (; len > 0; len -= Ssz, off += Ssz) {
		seekordie(&od, off);
		wp = buf;
		if (fflag || read(od.fd, buf, Ssz) != Ssz) {
			fprint(2, "%s: zeroing out sector %lld\n", nd.name, off/Ssz);
			wp = zbuf;
		}
		seekordie(&nd, off);
		if (write(nd.fd, wp, Ssz) != Ssz)
			sysfatal("%s: write failure at offset %lld\n", nd.name, off);
	}
}

void
dopen(Disk *d, int perm)
{
	snprint(buf, sizeof buf, "/raiddev/%s/data", d->name);
	d->fd = open(buf, perm);
	if (d->fd < 0)
		sysfatal("cannot open %s\n", buf);
	d->dir = dirfstat(d->fd);
	if (d->dir == nil)
		sysfatal("cannot dirfstat %s\n", buf);
	fprint(2, "%s size is %lld\n", d->name, d->dir->length);
}

int
nread(int fd, char *buf, int length)
{
	int i, n;
	int len = 0;

	for (i=16; i>0; i--) {
		n = read(fd, buf, length);
		if (n < 0)
			n = 0;
		len += n;
		length -= n;
		if (length <= 0 || fflag)
			break;
		sleep(100);
	}
	return len;
}

void
usage(void)
{
	fprint(2, "usage: %s [ -f ] [-s nsectors] oldslot newslot\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int n;
	vlong fsz, off;
	char *e;

	fsz = 0;
	ARGBEGIN {
	case 'f':
		fflag++;
		break;
	case 's':
		sflag++;
		fsz = strtoull(EARGF(usage()), &e, 10);
		if (*e != '\0' || fsz <= 0 ) {
			sysfatal("nsectors must be a valid positive integer");
		}
		fsz <<= 9;
		break;
	} ARGEND;
	if (argc != 2)
		usage();

	od.name = *argv++;
	dopen(&od, OREAD);
	nd.name = *argv;
	dopen(&nd, OWRITE);

	if (fsz == 0)
		fsz = od.dir->length;
	if (fsz > nd.dir->length)
		sysfatal("destination drive is smaller than buffer size being copied\n");
	seekordie(&od, 0);
	seekordie(&nd, 0);
	off = 0;
	for (; off < fsz; off += n) {
		n = Bsz;
		if ((fsz - off) < Bsz)
			n = fsz - off;
		if (nread(od.fd, buf, n) == n) {
			if (write(nd.fd, buf, n) != n)
				sysfatal("%s: write failure at offset %lld\n", nd.name, off);
		} else
			subrw(off, n);
	}
	fprint(2, "%s: extraction to %s complete\n", od.name, nd.name);
}
