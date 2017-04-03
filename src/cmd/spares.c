// Copyright © 2010 Coraid, Inc.
// All rights reserved.

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

void
usage(void) 
{
	fprint(2,"usage: %s \n", argv0);
	exits("usage");
}

void
main(int argc, char **argv) 
{
	char buf[Maxbuf], *args[Maxargs], devname[20];
	spareStr spareslist[Maxargs/2];
	char *slash;
	uvlong size;
	int i, n, count, shelf;

	ARGBEGIN{
	default:
		usage();
	}ARGEND

	if (argc > 0)
		usage();

	if ((n = readfile(buf, Maxbuf, "/raid/spares")) < 0)
		errfatal("%r");

	if (n == 0)	/* file is empty */
		exits(nil);

	/* spare info always stored in pair: drive and size */
	if ((n = tokenize(buf, args, Maxargs)) < 0 || n & 1)
		errfatal("%r");
	shelf = getshelf();

	/* content needs to be sorted */
	count = 0;
	for (i = 0; i < n; count++) {
		spareslist[count].name = strpbrk(args[i++], "0123456789");
		slash = strchr(spareslist[count].name, '/');
		*slash = 0;
		spareslist[count].size = args[i++];
	}
	qsort(spareslist, count, sizeof(spareStr),  sparestrcmp);

	print("%-9s %10s\n", "DRIVE", "SIZE");
	for (i = 0; i < count; i++) {
		size = fstrtoll(spareslist[i].size);
		snprint(devname, sizeof devname, "%d.%s", shelf, spareslist[i].name);
		print("%-9s %8.3fGB\n", devname, size / 1e9);
	}
}
