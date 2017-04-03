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

int cflag;

void
usage(void)
{
	fprint(2,"usage: %s [size] shelf.slot ...\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *drive;
	vlong size;

	ARGBEGIN {
	case 'c':
		cflag++;
		break;
	default:
		usage();
	} ARGEND
	if (!cflag)
	if (argc < 2)
		usage();
	else {
		size = fstrtoll(*argv++);
		argc--;
	}
	if (argc == 0)
		usage();
	while (argc-- > 0) {
		drive = parseshelfdotslot(*argv++, usage, nil, 1);
		if (drive == nil)
			errskip(argc, argv);
		if (cflag) {
			if (drivectlwrite(drive, "setsize") < 0)
				errskip(argc, argv);
		} else {
			if (drivectlwrite(drive, "setsize %ulld", size) < 0)
				errskip(argc, argv);
		}
	}
	exits(nil);
}
