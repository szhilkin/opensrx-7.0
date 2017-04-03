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
	}ARGEND
	if (argc == 0)
		usage();
	if (!iscm())
		errfatal("no CacheMotion card");
	while (argc-- > 0) {
		lun = *argv++;
		if (!islun(lun)) {
			werrstr("LUN %s does not exist", lun);
			errskip(argc, argv);
		}
		if (lunctlwrite(lun, "cache 0 on") < 0)
			errskip(argc, argv);
	}
	exits(nil);
}
