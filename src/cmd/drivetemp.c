/*
 * Copyright © 2014 Coraid, Inc.
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
	fprint(2,"usage: %s [ shelf.slot ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *drive;
	char cmd[256];
	int nlines;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc == 0) {
		snprint(cmd, sizeof cmd, "drives -t");
		shellcmd(cmd);
	}
	nlines = 0;
	while (argc--) {
		if (nlines == 0)
			snprint(cmd, sizeof cmd, "drives -t %s", *argv);
		else
			snprint(cmd, sizeof cmd, "drives -tq %s", *argv);
		drive = parseshelfdotslot(*argv, usage, nil, 0);
		if (drive == nil)
			print("error: %r\n");
		else if (shellcmd(cmd) < 0)
			print("error: %r\n");
		else
			nlines++;
		argv++;
	}
	exits(nil);
}
