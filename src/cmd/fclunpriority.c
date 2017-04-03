// Copyright © 2013 Coraid, Inc.
// All rights reserved.

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void)
{
	fprint(2,"usage: %s LUN pri [minpct]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc < 2 || argc > 3)
		usage();
	if (!islun(argv[0]))
		errfatal("LUN %s does not exist", argv[0]);
	if (lunctlwrite(argv[0], "cacheprio 1 %s %s", argv[1], argc == 3 ? argv[2] : "0") < 0)
		errfatal("%r");
	exits(nil);
}
