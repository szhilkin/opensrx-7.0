/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */

#include <u.h>
#include <libc.h>

void
usage(void)
{
	fprint(2,"usage: %s\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{

	ARGBEGIN {
	default:
		usage();
	}ARGEND

	if (argc != 0)
		usage();

	execl("/bin/drives", "drives", "-C", nil);

	exits(nil);
}
