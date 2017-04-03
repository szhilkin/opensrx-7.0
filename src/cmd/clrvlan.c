/*
 *  Copyright © 2013 Coraid, Inc.
 *  All rights reserved.
 *  List vlans associated with luns
 */

#include <u.h>
#include <libc.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void)
{
	fprint(2, "usage: %s LUN [ ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char lunvlan[50];

	ARGBEGIN{
	default:
		usage();
	}ARGEND
	if (argc == 0)
		usage();
	while (argc-- > 0) {
		if(islun(*argv) == 0) {
			werrstr("LUN %s does not exist", *argv);
			errskip(argc, argv + 1);
		}
		snprint(lunvlan, sizeof lunvlan, "/raid/%s/vlan", *argv);
		if (writefile(lunvlan, "0") < 0)
			errskip(argc, argv + 1);
		argv++;
	}
	exits(nil);
}
