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

static char *states[] = {
	"locate",
	"fault",
	"rebuild",
	"reset",
	"spare"
};

static void
usage(void)
{
	fprint(2, "usage: %s state slot [ ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *state, buf[24];
	int i;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc < 2)
		usage();

	state = nil;
	for (i = 0; i < nelem(states); ++i)
		if (strcmp(*argv, states[i]) == 0) {
			state = *argv++;
			argc--;
			break;
		}
	if (state == nil)
		errfatal("invalid state: %s", *argv);
	while (argc-- > 0) {
		if (isslot(*argv) == 0) {
			werrstr("slot %s does not exist", *argv++);
			errskip(argc, argv);
		}
		snprint(buf, sizeof buf, "/raiddev/%s/led", *argv++);
		if (writefile(buf, state) < 0)
			errskip(argc, argv);
	}
	exits(nil);
}
