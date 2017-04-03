#include <u.h>
#include <libc.h>
#include <ctype.h>
#include "aoe.h"

enum {
	Blksize = 8192,
};

void
dotest(Aoedev *dev, int fd, uvlong len)
{
	static uchar buf[Blksize];
	uvlong off;
	int n;

	off = 0;
	while (len > 0) {
		n = aoeread(dev, buf, sizeof buf, off);
		if (n < 0)
			sysfatal("error: can't read from target: %r");
		if (pwrite(fd, buf, n, off) != n)
			sysfatal("error: can't write to device: %r");
		off += n;
		len -= n;
	}
}

Aoedev *
gettarg(void)
{
	int i;

	/*
	 * Return the first CacheMotion target we find.  This requires
	 * the shelf be configured for loopback for reliable operation.
	 */
	aoediscover();
	for (i = 0; i < ndevs; ++i)
		if (strcmp(devs[i].model, "NVWC") == 0)
			return &devs[i];
	return nil;
}

void
usage(void)
{
	fprint(2, "usage: cmaoetest\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	Aoedev *dev;
	int fd;
	Dir *d;

	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if (argc != 0)
		usage();
	if (aoeinit(0, nil) < 0)
		sysfatal("error: can't initialize aoe");
	dev = gettarg();
	if (dev == nil)
		sysfatal("error: can't find target");
	fd = open("#S/sdS0/data", OWRITE);
	if (fd < 0)
		sysfatal("error: can't open: %r");
	d = dirfstat(fd);
	dotest(dev, fd, d->length);
	exits(nil);
}
