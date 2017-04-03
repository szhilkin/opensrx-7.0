// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// glob.c: simple shelf globing
#include <u.h>
#include <libc.h>
#include <ctype.h>

/*
	for each argument, expand i[-j].k[-l] into i.k, i.k+1,...i.l, i+1.k, ...j.l
	isolate :string and append to every expanded entry
*/

char *cp;

int
getnum(void)
{
	int a = 0;

	while (isdigit(*cp)) {
		a *= 10;
		a += *cp - '0';
		cp++;
	}
	return a;
}

void
syn(void)
{
	exits("syntax");
}

void
main(int argc, char **argv)
{
	int a, b, c, d, i, j;
	char suffix[3];

	suffix[0] = ':';
	suffix[2] = 0;
	for (argc--, argv++; argc > 0; argc--, argv++) {
		if (strncmp(*argv, "missing", 4) == 0) {
			print("missing\n");
			continue;
		}
		suffix[1] = 0;
		cp = *argv;
		if (!isdigit(*cp))
			syn();
		a = getnum();
		if (*cp == '-') {
			cp++;
			b = getnum();
		} else
			b = a;
		if (*cp != '.')
			syn();
		cp++;
		if (!isdigit(*cp))
			syn();
		c = getnum();
		if (*cp == '-') {
			cp++;
			d = getnum();
		} else
			d = c;
		if (*cp == ':') {
			suffix[1] = *++cp;
			cp++;
		}
		if (a > b || c > d || *cp == '.')
			syn();
		for (i = a; i <= b; i++)
			for (j = c; j <= d; j++)
				print("%d.%d%s\n", i, j, suffix[1] ? suffix : "");
	}
	exits(nil);
}
