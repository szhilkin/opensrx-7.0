/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */

#include <u.h>
#include <libc.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void)
{
	fprint(2, "usage: %s mode LUN [ ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *mode;
	char filestr[50];

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc < 2)
		usage();
	mode = *argv;
	if (strcmp(mode, "random") != 0)
	if (strcmp(mode, "sequential") != 0)
		errfatal("invalid mode: %s", mode);
	while(++argv, --argc) {
		if (!islun(*argv)) {
			werrstr("LUN %s does not exist", *argv);
			errskip(argc - 1, argv + 1);
		}
		snprint(filestr, sizeof filestr, "/raid/%s/iomode", *argv);
		if (writefile(filestr, mode) < 0)
			errskip(argc - 1, argv + 1);
	}
}
