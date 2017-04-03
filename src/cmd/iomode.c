/*
 * Copyright © 2011 Coraid, Inc.
 * All rights reserved.
 */

#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <libcutil.h>

#include "srxcmds.h"

void
printiomode(char *lunid)
{
	char mode[24];
	static int hdr = 0;

	if (readfile(mode, sizeof mode, "/raid/%s/iomode", lunid) < 0) {
		print("error: LUN %s not present\n", lunid);
		return;
	}
	if (!hdr++)
		print("%-5s %10s\n", "LUN", "MODE");
	print("%-5s %10s\n", lunid, mode);
}

void
printiomodes(void)
{
	int i, n;
	Dir *dp;

	n = numfiles("/raid", &dp);
	if (n < 0)
		errfatal("failure accessing LUN info");
	qsort(dp, n, sizeof *dp, dirintcmp);
	for (i = 0; i < n; i++) {
		if (isdigit(dp[i].name[0]))
			printiomode(dp[i].name);
	}
	free(dp);
}

void
usage(void)
{
	fprint(2, "usage: iomode [ LUN ... ]\n");
	exits("usage");
}

void 
main(int argc, char **argv)
{
	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if (argc == 0)
		printiomodes();
	else {
		while (argc-- > 0)
			printiomode(*argv++);
	}
	exits(nil);
}
