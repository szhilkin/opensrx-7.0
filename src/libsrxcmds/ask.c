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

static char *
stem(char *s)
{
	if (strncmp(s, "rm", 2) == 0 || strncmp(s, "un", 2) == 0)
		return s + 2;
	else
		return "LUN";
}

void
askhdr(int c, char **v)
{
	print("Request to %s %d %s%s: ", argv0, c, stem(argv0), c > 1 ? "s" : "");
	switch (c) {
	case 1:
		break;
	case 2:
		print("%s and ", v[0]);
		break;
	default:
		print("%s ... ", v[0]);
		break;
	}
	print("%s\n", v[c-1]);
}

int
ask(char *s, int quit)
{
	int n;
	char buf[256];

	if (quit)
		print("\'y\' to update to new format, \'n\' to create LUN with old format, or \'q\' to quit [q]: ");
	else
		print("\'n\' to cancel, \'a\' for all, or \'y\' to %s %s [n]: ", argv0, s);
	n = read(0, buf, sizeof buf);
	buf[n-1] = 0;
	if (cistrcmp(buf, "y") == 0)
		return RespondYes;
	else if (cistrcmp(buf, "a") == 0)
		return RespondAll;
	else if (cistrcmp(buf, "n") == 0 && quit)
		return RespondNo;
	else
		sysfatal("action canceled");
	return -1;
}
