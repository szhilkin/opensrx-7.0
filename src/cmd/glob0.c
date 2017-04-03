// Copyright © 2010 Coraid, Inc.
// All rights reserved.
#include <u.h>
#include <libc.h>
#include <ctype.h>

int isdigits(char *);
void range(char *, char *, uint *, uint *);

// simple globing with single digits
void
main(int argc, char **argv)
{
	char *cp, *str;
	uint n, m;

	argc--;
	argv++;
	while (argc-- > 0) {
		str = *argv++;
		cp = strchr(str, '-');
		if (cp == nil) {
			if (!isdigits(str)) {
				fprint(2, "%s is not a valid value\n", str);
				exits("LUN");
			}
			print("%s\n", str);
			continue;
		}
		*cp++ = 0;
		range(str, cp, &n, &m);
		while (n <= m)
			print("%d\n", n++);
	}
	exits(nil);
}

int
isdigits(char *p)
{
	for (; *p; p++)
		if (!isdigit(*p))
			return 0;
	return 1;
}

void
range(char *cs, char *ce, uint *s, uint *e)
{
	if (*cs == 0 || *ce == 0 || !isdigits(cs) || !isdigits(ce)) {
			fprint(2, "invalid range format: %s-%s\n", cs, ce);
			exits("range");
	}
	*s = atoi(cs);
	*e = atoi(ce);
}
