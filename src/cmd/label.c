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
	fprint(2,"usage: %s name LUN ...\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *lun;
	char *name;

	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if (argc < 2)
		usage();
	name = *argv++;
	argc--;
	while (argc-- > 0) {
		lun = *argv++;
		if (islun(lun) == 0) {
			werrstr("LUN %s does not exist", lun);
			errskip(argc, argv);
		}
		if (lunlabelwrite(lun, name) < 0)
			errskip(argc, argv);
	}
	exits(nil);
}
