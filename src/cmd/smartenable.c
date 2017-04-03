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
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *drive;
	char cmd[256];

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc == 0)
		usage();
	while (argc--) {
		snprint(cmd, sizeof cmd, "drives -se %s", *argv);
		drive = parseshelfdotslot(*argv, usage, nil, 1);
		if (drive == nil)
			errskip(argc, argv+1);
		if (shellcmd(cmd) < 0)
			errskip(argc, argv+1);
		argv++;
	}
	exits(nil);
}
