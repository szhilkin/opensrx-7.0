/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

enum {
	Fname,
	Flun,
	Nfields,
};

void
usage(void)
{
	fprint(2,"usage: %s [ LUN ]\n", argv0);
	exits("usage");
}

void
cmlunidlist(void)
{
	Biobuf *bp;
	int n;
	char *p, *q, *f[Nfields];

	bp = Bopen("/dev/sdS0/ctl", OREAD);
	if (bp == nil)
		errfatal("failed to open /dev/sdS0/ctl");
	while (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = 0;
		if (strstr(p, "aoetarget") != nil) {
			n = tokenize(p, f, nelem(f));
			if (n == Nfields) {
				q = strchr(f[Flun], '.');
				print("%-7s\n", ++q);
				break;
			}
		}
	}
	Bterm(bp);
	return;
}

void
main(int argc, char **argv)
{
	char *lun;
	int shelf;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (!iscm())
		errfatal("no CacheMotion card");
	if (argc == 0) {
		cmlunidlist();
		return;
	}
	if (argc != 1)
		usage();
	shelf = getshelf();
	lun = *argv;
	if (islun(lun))
		errfatal("duplicate LUN id");
	if (cmctlwrite("target %d.%s", shelf, lun) < 0)
		errfatal("failed to set Cache Motion LUN");
	exits(nil);
}
