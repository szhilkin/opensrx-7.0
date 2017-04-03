/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void)
{
	fprint(2,"usage: %s LUN.part.drive shelf.slot\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *lun, *part, *drive;
	char *slot;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc != 2)
		usage();
	if (parselundotpartdotdrive(*argv++, &lun, &part, &drive, usage) != 0)
		errfatal("%r");
	slot = parseshelfdotslot(*argv, usage, nil, 1);
	if (slot == nil)
		errfatal("%r");
	if (lunctlwrite(lun, "replace %s.%s /raiddev/%s/data", part, drive, slot) < 0)
		errfatal("%r");
	exits(nil);
}
