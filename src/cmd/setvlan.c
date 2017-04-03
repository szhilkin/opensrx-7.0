/*
 *  Copyright © 2013 Coraid, Inc.
 *  All rights reserved.
 *  List vlans associated with luns
 */
#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void)
{
	fprint(2, "usage: %s vlanid LUN [ ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *vlan;
	char lunvlan[50];

	ARGBEGIN{
	default:
		usage();
	}ARGEND
	if (argc <= 1)
		usage();
	vlan = *argv;
	if (!isdigit(vlan[0]) || vlan[0] == '0' || atoi(vlan) > 4094)
		errfatal("vlanid must be in the range 1 to 4094");
	while (++argv, --argc) {
		if(islun(*argv) == 0) {
			werrstr("LUN %s does not exist", *argv);
			errskip(argc - 1, argv + 1);
		}
		snprint(lunvlan, sizeof lunvlan, "/raid/%s/vlan", *argv);
		if (writefile(lunvlan, "%s", vlan) < 0)
			errskip(argc - 1, argv + 1);
	}
	exits(nil);
}
