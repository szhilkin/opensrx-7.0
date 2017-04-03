/*
 * Copyright © 2014 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <stdio.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void)
{
	fprint(2, "usage: %s [ -r readtimer ] [ -w writetimer] [ shelf.slot ... ]\n", argv0);
	exits(nil);
}

void
main(int argc, char **argv)
{
	char *drive, *rf, *wf, *dl, *d;
	char cmd[256];

	rf = wf = nil;
	ARGBEGIN {
	case 'r':
		rf = EARGF(usage());
		break;
	case 'w':
		wf = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND

	if (rf == nil && wf == nil) {		/* Display drives. */
		dl = strdup("drives -sr");
		while (argc--) {
			drive = parseshelfdotslot(*argv++, usage, nil, 1);
			if (drive == nil) {
				fprint(2, "error: %r\n");
				continue;
			}
			d = smprint("%s %d.%s", dl, getshelf(), drive);
			free(dl);
			dl = d;
		}
		if (shellcmd(dl) < 0)
			sysfatal("%r");
		exits(nil);
	}
						/* Set drives. See comment0. */
	while (argc--) {
		drive = parseshelfdotslot(*argv++, usage, nil, 1);
		if (drive == nil)
			errskip(argc, argv);
		if (rf) {
			snprint(cmd, sizeof cmd, "drives -srn %s %d.%s",
				rf, getshelf(), drive);
			if (shellcmd(cmd) < 0)
				errskip(argc, argv);
		}
		if (wf) {
			snprint(cmd, sizeof cmd, "drives -swn %s %d.%s",
				wf, getshelf(), drive);
			if (shellcmd(cmd) < 0)
				errskip(argc, argv);
		}
	}
	exits(nil);
}

/*

comment0:

The drives command does not support setting read and write at the same
time, so make separate calls per drive.

SRX EXPERTMODE# drives -srn 8000 -swn 9000 500.4
SRX EXPERTMODE# drives -sr 500.4
DRIVE         READ     WRITE
500.4       9000ms    9000ms
SRX EXPERTMODE# 

*/
