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
	fprint(2,"usage: %s LUN [ ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *lun;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc < 1)
		usage();
	while (argc-- > 0) {
		lun = *argv++;
		if (islun(lun) == 0) {
			werrstr("LUN %s does not exist\n", lun);
			errskip(argc, argv);
		}
		if (lunctlwrite(lun, "flushcache off") < 0)
			errskip(argc, argv);
	}
	exits(nil);
}
