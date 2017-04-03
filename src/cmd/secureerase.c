/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <stdio.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void)
{
	fprint(2,"usage: %s shelf.slot [ ... ]\n", argv0);
	exits(nil);
}

int force;

void
main(int argc, char **argv)
{
	char *drive;
	char cmd[256];
	char resp[256];
	int n;

	ARGBEGIN {
	case 'f':
		force++;
		break;
	default:
		usage();
	} ARGEND
	if (argc == 0)
		usage();

	if (!force) {
		print("warning: secure erase will destroy all data on all specified drives.  This operation cannot be reversed.\n");
	 	print("Would you like to secure erase the specified drives y/n? [N]:");
		if ((n = read(0, resp, sizeof resp)) <= 0)
			sysfatal("failed to read response");
		resp[n-1] = 0;	/* kill newline */
		if (cistrcmp(resp, "y") != 0)
			exits("noerase");
	}
	while (argc--) {
		drive = parseshelfdotslot(*argv++, usage, nil, 1);
		if (drive == nil)
			errskip(argc, argv);
		snprint(cmd, sizeof cmd, "drives -%sx erase %d.%s", force ? "f" : "",getshelf(), drive);
		if (shellcmd(cmd) < 0)
			errskip(argc, argv);
	}
	exits(nil);
}
