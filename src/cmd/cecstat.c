// Copyright © 2013 Coraid, Inc.
// All rights reserved.

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>

#include <libcutil.h>

void
usage(void) 
{
	fprint(2,"usage: cecstat\n");
	exits("usage");
}

int jflag;

void
main(int argc, char **argv) 
{
	Dir *dp;
	int i, n, max_net = 0, min_net = 100, number;
	char *fmt = "%-10s %10s\n";
	char *np, buf[200], interface[10];

	ARGBEGIN {
	case 'j':
		jflag++;
		fmt = "%s:%s\n";
		break;
	default:
		usage();
	}ARGEND
	if (argc > 0)
		usage();
	n = numfiles("/net", &dp);
	if (n < 0)
		errfatal("cannot access interface info");
	for (i = 0; i < n; i++) {
		if (strstr(dp[i].name, "ether")) {
			np = strpbrk(dp[i].name, "0123456789");
			number = atoi(np);
			if (number > max_net)
				max_net = number;
			if (number < min_net)
				min_net = number;
		}
	}
	free(dp);
	if (readfile(buf, sizeof buf, "/dev/ceccfg") < 0)
		errfatal("cannot access cec configuration info");
	if (!jflag)
		print(fmt, "NAME", "CEC");
	for (i = min_net; i <= max_net; i++) {
		snprint(interface, sizeof interface, "ether%d", i);
		if (strstr(buf, interface))
			print(fmt, interface, "enabled");
		else
			print(fmt, interface, "disabled");
		
	}
	exits(nil);
}