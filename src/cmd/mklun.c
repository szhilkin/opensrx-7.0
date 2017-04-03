/*
  * Copyright © 2013 Coraid, Inc.
  * All rights reserved.
  */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include <stdio.h>
#include "srxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s LUN raidtype [ shelf.slot ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv) 
{
	int clean, noprompt;
	char options[50], *po, *epo;
	char *lun, *raidtype, *format;

	clean = 0;
	noprompt = 0;
	po = options;
	epo = po + sizeof options;
	ARGBEGIN {
	case 'f':
		po = seprint(po, epo, "-f ");
		break;
	case 'r':
		po = seprint(po, epo, "-r ");
		break;
	case 'c':
		clean = 1;
		break;
	case 'I':
		noprompt = 1;
		break;
	case 'V':
		format = EARGF(usage());
		if (strcmp(format, "0") != 0 && strcmp(format, "1") != 0)
			errfatal("-V only accepts format 0 or 1");
		po = seprint(po, epo,  "-V %s ", format);
		break;
	default:
		usage();
	} ARGEND
	if (argc < 2)
		usage();
	lun = *argv++;
	raidtype = *argv++;
	argc -= 2;
	if (makelun(options, lun, raidtype, argc, argv, clean, noprompt) < 0)
		errfatal("%r");
	exits(nil);
}
