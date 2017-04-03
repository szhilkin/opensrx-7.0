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

int sflag;

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
	int fflag;

	fflag = 0;
	ARGBEGIN {
	case 'f':
		fflag++;
		break;
	case 's':
		sflag++;
		break;
	default:
		usage();
	} ARGEND
	if (argc == 0)
		usage();
	while (argc-- > 0) {
		drive = parseshelfdotslot(*argv++, usage, nil, fflag ? 0 : 1);
		if (drive == nil)
			errskip(argc, argv);
		if (writefile("/raid/ctl", "rmspare %s /raiddev/%s/data",
				sflag ? "-s" : "", drive) < 0 )
			errskip(argc, argv);
	}
	exits(nil);
}
