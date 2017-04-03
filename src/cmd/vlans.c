/*
 *  Copyright © 2013 Coraid, Inc.
 *  All rights reserved.
 *  List vlans associated with luns
 */

#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void)
{
	fprint(2, "usage: %s [ LUN ... ]\n", argv0);
	exits("usage");
}

void
printvlan(char *lun)
{
	char vlan[50];

	if(islun(lun) == 0)
		print("error: LUN %s does not exist\n", lun);
	else {
		if (readfile(vlan, sizeof vlan, "/raid/%s/vlan", lun) < 0)
			print("error: LUN %s %r\n", lun);
		else
			print("%-5s %9s\n", lun, (strcmp(vlan, "0") == 0) ? " " : vlan);
	}
}

void
main(int argc, char **argv)
{
	int n, i;
	Dir *dp;

	ARGBEGIN{
	default:
		usage();
		break;
	}ARGEND
	print("%-5s %9s\n", "LUN", "VLAN");
	if (argc == 0) {
		n = numfiles("/raid", &dp);
		if (n < 0)
			errfatal("cannot retrieve vlan info");
		qsort(dp, n, sizeof *dp, dirintcmp);
		for (i = 0; i < n; i++)
			if (isdigit(dp[i].name[0]))
				printvlan(dp[i].name);
		free(dp);
	} else
		while (argc-- > 0)
			printvlan(*argv++);
	exits(nil);
}
