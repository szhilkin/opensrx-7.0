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

int
cmctlwrite(char *fmt, ...)
{
	int n;
	va_list arg;

	va_start(arg, fmt);
	n = srxwritefile("/dev/sdS0/ctl", fmt, arg);
	va_end(arg);
	return n;
}

int
lunctlwrite(char *lun, char *fmt, ...)
{
	char *b;
	int n;
	va_list arg;

	b = mustsmprint("/raid/%s/ctl", lun);
	va_start(arg, fmt);
	n = srxwritefile(b, fmt, arg);
	va_end(arg);
	free(b);
	return n;
}

int
drivectlwrite(char *drive, char *fmt, ...)
{
	char *b;
	int n;
	va_list arg;

	b = mustsmprint("/raiddev/%s/ctl", drive);
	va_start(arg, fmt);
	n = srxwritefile(b, fmt, arg);
	va_end(arg);
	free(b);
	return n;
}
