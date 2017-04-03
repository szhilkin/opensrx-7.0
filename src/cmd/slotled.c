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


void
usage(void)
{
	fprint(2,"usage: %s [ slot ... ]\n", argv0);
	exits("usage");
}

void
printled(char *led)
{
	char buf[32];
	static prthdr = 0;

	if (readfile(buf, sizeof buf , "/raiddev/%s/led", led) < 0) {
		print("error: cannot access LED information in slot %s\n", led);
	} else {
		if (prthdr == 0) {
			prthdr = 1;
			print("%-5s %9s\n", "SLOT", "STATE");
		}
		print("%-5s %9s\n", led, buf);
	}
}


void
printleds(void)
{
	int i, n;
	Dir *dp;

	n = numfiles("/raiddev", &dp);
	if (n < 0)
		errfatal("cannot access drive information");
	qsort(dp, n, sizeof *dp, dirintcmp);
	for (i = 0; i < n; i++) {
		if (isdigit(dp[i].name[0]))
			printled(dp[i].name);
	}
	free(dp);
}

void
main(int argc, char **argv)
{
	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if (argc == 0) {
		printleds();
		exits(nil);
	}
	while (argc-- > 0) {
		if (isslot(*argv) == 0) {
			werrstr("slot %s does not exist", *argv++);
			errskip(argc, argv);
		}
		printled(*argv++);
	}
	exits(nil);
}
