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
	Fsize,
	Fstate,
	Fcmstate,
	Ffcstate,
	Nfields,
};

void
usage(void)
{
	fprint(2,"usage: %s\n", argv0);
	exits("usage");
}

static void
cmstat(char *lun)
{
	char *p, *f[Nfields], path[64];
	Biobuf *bp;
	int n;
	static int hdr = 0;

	snprint(path, sizeof path, "/raid/%s/stat", lun);
	bp = Bopen(path, OREAD);
	if (bp == nil)
		return;
	if (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = 0;
		n = tokenize(p, f, nelem(f));
		if (n == Nfields) {
			if (!hdr++)
				print("%-7s %8s\n", "LUN", "STATE");
			print("%-7s %8s\n", lun, &f[Fcmstate][2]);
		}
	}
	Bterm(bp);
	return;
}

void
main(int argc, char **argv)
{
	int i, n;
	Dir *dp;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc != 0)
		usage();
	if (!iscm())
		errfatal("no CacheMotion card");
	n = numfiles("/raid", &dp);
	if (n < 0)
		errfatal("failure accessing LUN info");
	qsort(dp, n, sizeof *dp, dirintcmp);
	for(i = 0; i < n; i++)
		if (isdigit(dp[i].name[0]))
			cmstat(dp[i].name);
	free(dp);
	exits(nil);
}
