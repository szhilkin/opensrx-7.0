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
	char *drive;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc == 0)
		usage();
	while (argc-- > 0) {
		drive = parseshelfdotslot(*argv++, usage, nil, 1);
		if (drive == nil)
			errskip(argc, argv);
		if (writefile("/raid/ctl", "spare /raiddev/%s/data", drive) < 0)
			errskip(argc, argv);
	}
	exits(nil);
}
