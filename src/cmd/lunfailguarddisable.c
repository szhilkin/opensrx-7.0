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
	int force, askret;

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
	askret = -1;
	while (argc-- > 0) {
		lun = *argv++;
		if (islun(lun) == 0) {
			werrstr("LUN %s does not exist", lun);
			errskip(argc, argv);
		}
		if (!force && islunonline(lun)) {
			werrstr("LUN %s needs to be offline before disabling lunfailguard", lun);
			errskip(argc, argv);
		}
		if (!force && askret != RespondAll) {
			print("warning: disabling this feature will ignore basic sanity checks prior to\n");
			print("	failing a drive.  This can lead to loss of data and should\n");
			print("	only be used at the direction of Coraid TAC.\n");
			askhdr(argc+1, argv-1);
			askret = ask(lun, 0);
		}
		/*
		  * This is tricky.  Since we have to support disabling the
		  * failguard in a backward compatible manner, we 
		  * must negate the requested state.
		  */
		if (lunctlwrite(lun, "noguard on") < 0)
			errskip(argc, argv);
	}
	exits(nil);
}