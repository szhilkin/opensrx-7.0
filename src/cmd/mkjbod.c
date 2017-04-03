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
	fprint(2,"usage: %s shelf.slot [ ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv) 
{
	char *drive, *device;

	ARGBEGIN{
	default:
		usage();
	}ARGEND
	if (argc == 0)
		usage();
	while (argc-- > 0) {
		device = strdup(*argv);
		if (!(drive = parseshelfdotslot(*argv, usage, nil, 1)))
			errskip(argc, argv + 1);
		if (makelun("", drive, "jbod", 1, &device, 0, 0) <0)
			errskip(argc, argv + 1);
		argv++;
	}
	exits(nil);
}
