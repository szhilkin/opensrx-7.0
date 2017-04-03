/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <libcutil.h>
#include <stdio.h>

static int bshelf = -1;
static char *shelf = "/n/kfs/srx/shelf";

int
getshelf(void)
{
	char buf[100], *cp;

	if (bshelf != -1)
		return bshelf;
	if (access(shelf, AEXIST) < 0)
		errfatal("shelf ID is not set");
	if (readfile(buf, sizeof buf, shelf) < 0)
		errfatal("cannot access shelf information");
	cp = buf;
	while (*cp == ' ')
		cp++;
	if (!isdigit(*cp))
		bshelf = -1;
	else
		bshelf = atoi(cp);
	return bshelf;
}
