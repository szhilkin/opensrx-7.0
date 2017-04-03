/*
 * Copyright © 2013 Coraid Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void)
{
	fprint(2,"usage: %s interface [ ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *interface;

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc == 0)
		usage();
	while (argc-- > 0) {
		interface = *argv++;
		if (!isif(interface)) {
			fprint(2, "error: interface %s does not exist\n", interface);
			continue;
		}
		if (writefile("/dev/cecctl", "cecoff %s", interface) < 0)
			errskip(argc, argv);
		if (copy("/dev/ceccfg", "/n/kfs/srx/ceccfg") < 0)
			errskip(argc, argv);
	}
	exits(nil);
}
