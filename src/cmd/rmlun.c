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

int fflag;

void
usage(void)
{
	fprint(2, "usage: %s LUN [ ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	int askret;

	ARGBEGIN {
	case 'f':
		fflag++;
		break;
	default:
		usage();
	} ARGEND
	if (argc == 0)
		usage();
	if (!fflag)
		askhdr(argc, argv);
	askret = -1;
	for (; argc-- > 0; argv++) {
		if (fflag) {
			lunctlwrite(*argv, "rmlun");
			continue;
		}
		if (!islun(*argv)) {
			werrstr("LUN %s does not exist", *argv);
			errskip(argc, argv + 1);
		}
		if (islunonline(*argv)) {
			werrstr("LUN %s needs to be offline before removal", *argv);
			errskip(argc, argv + 1);
		}
		if (askret != RespondAll)
			askret = ask(*argv, 0);
		if (lunctlwrite(*argv, "rmlun") < 0)
			errskip(argc, argv + 1);
	}
	exits(nil);
}
