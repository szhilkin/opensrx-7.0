/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */

#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

static void
usage(void)
{
	fprint(2, "usage: %s LUN [ ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *lun;
	int force;

	force = 0;
	ARGBEGIN {
	case 'f':
		force++;
		break;
	default:
		usage();
	} ARGEND

	if (argc == 0)
		usage();
	while (argc-- > 0) {
		lun = *argv++;
		if (islun(lun) == 0) {
			werrstr("LUN %s does not exist", lun);
			errskip(argc, argv);
		}
		if (!force && islunonline(lun)) {
			werrstr("LUN %s needs to be offline before enabling lunfailguard\n", lun);
			errskip(argc, argv);
		}
		/*
		  * This is tricky.  Since we have to support disabling the
		  * failguard in a backward compatible manner, we 
		  * must negate the requested state.
		  */
		if (lunctlwrite(lun, "noguard off") < 0)
			errskip(argc, argv);
	}
	exits(nil);
}