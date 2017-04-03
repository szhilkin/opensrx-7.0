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
	fprint(2,"usage: %s LUN.part.drive\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *lun, *part, *drive;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc != 1)
		usage();
	if (parselundotpartdotdrive(*argv, &lun, &part, &drive, usage) != 0)
		errfatal("%r");
	if (lunctlwrite(lun, "fail %s.%s", part, drive) < 0)
		errfatal("%r");
	exits(nil);
}
