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

/* formatted str to ll */
vlong
fstrtoll(char *str)
{
	int mult  = 1000;
	vlong n;
	char *p;

	n = strtoll(str, &p, 0);
	if (p == str)
		goto error;
	switch (*p) {
	case 'g':
	case 'G':
		n *= mult;
	case 'm':
	case 'M':
		n *= mult;
	case 'K':
	case 'k':
		n *= mult;
		p++;
		break;
	case 'S':
	case 's':
		n *= 512;
	}
	if (*p != '\0')
error:
	sysfatal("invalid number format %s\n", str);
	return n;
}

char *
mustsmprint(char *fmt, ...)
{
	char *cp;
	va_list arg;

	va_start(arg, fmt);
	cp = vsmprint(fmt, arg);
	va_end(arg);
	if (cp == nil)
		sysfatal("smprint failed %r");
	return cp;
}
