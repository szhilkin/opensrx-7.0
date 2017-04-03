// Copyright © 2010 Coraid, Inc.
// All rights reserved.
#include <u.h>
#include <libc.h>

int mult = 1000;

/* formatted str to ll */
vlong
fstrtoll(char *str)
{
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
error:		sysfatal("invalid number format %s\n", str);
	return n;
}

void
usage(void)
{
	fprint(2, "usage: fmtsz [-2] ...\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	ARGBEGIN {
	default:
		usage();
		break;
	case '2':
		mult = 1024;
		break;
	} ARGEND

	for (; argc; argc--, argv++)
		print("%lld%c", fstrtoll(*argv), argc ? ' ' : '\n');
	exits(0);
}
