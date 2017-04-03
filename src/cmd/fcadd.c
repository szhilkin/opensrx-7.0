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

int rflag;

void
usage(void)
{
	fprint(2,"usage: %s shelf.slot ...\n", argv0);
	exits("usage");
}

int
fcadd(char *drive, char *op)
{
	if (rflag) {
		if (writefile("/raid/ctl", "ecrestore /raiddev/%s/data", drive) < 0)
			if (writefile("/raid/ctl", "rmfcache /raiddev/%s/data", drive) < 0)
				errfatal("cannot remove EtherFlash Cache");
		return 0;
	}
	if (writefile("/raid/ctl", "ecadd  /raiddev/%s/data %s", drive, op) < 0)
		return -1;
	return 0;
}


void
main(int argc, char **argv)
{
	char *op;
	char *drive;

	op = "0";
	ARGBEGIN {
	case 'o':
		op = EARGF(usage());
		break;
	case 'r':
		rflag++;
		break;
	default:
		usage();
	} ARGEND
	if (argc == 0)
		usage();
	while (argc-- > 0) {
		drive = parseshelfdotslot(*argv++, usage, nil, 1);
		if (drive == nil)
			errskip(argc, argv);
		if (fcadd(drive,op) < 0)
			errskip(argc, argv);
	}
	exits(nil);
}
