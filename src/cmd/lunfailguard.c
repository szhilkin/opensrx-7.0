/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */

#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

static void
usage(void)
{
	fprint(2, "usage: %s [ LUN ... ]\n", argv0);
	exits("usage");
}

/*
 * This is tricky.  Since we have to support disabling the
 * failguard in a backward compatible manner, we 
 * must negate the requested state.
 */
char *
getfailguard(char *noguard)
{
	if (strcmp(noguard, "on") == 0)
		return "off";
	if (strcmp(noguard, "off") == 0)
		return "on";
	return "unknown";
}

void
lunfailguard(char *lun)
{
	int i, n;
	char buf[256], *toks[40];
	static int hdr = 0;

	if (readfile(buf, sizeof buf, "/raid/%s/ctl", lun)) {
		n = tokenize(buf, toks, nelem(toks));
		if (!hdr++)
			print("%-7s %10s\n", "LUN", "FAILGUARD");
		for (i = 0; i < n; i++)
			if (strcmp(toks[i], "noguard:") == 0)
				print("%-7s %10s\n", lun, getfailguard(toks[i+1]));
	}	
}

void
main(int argc, char **argv)
{
	Dir *dp;
	int i, n;
	char *lun;

	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if (argc == 0) {
		n = numfiles("/raid", &dp);
		if (n < 0)
			errfatal("failure accessing LUN info");
		qsort(dp, n, sizeof *dp, dirintcmp);
		for(i = 0; i < n; i++)
			if (isdigit(dp[i].name[0]))
				lunfailguard(&dp[i].name[0]);
		free(dp);
	}
	else {
		while (argc-- > 0) {
			lun = *argv++;
			if (islun(lun) == 0) {
				fprint(2, "error: LUN %s does not exist\n", lun);
				continue;
			}
			lunfailguard(lun);
		}
	}	
}

