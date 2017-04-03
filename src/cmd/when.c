/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
*/

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

enum {
	Fname,
	Nrecovfields = 5,
	Ninitfields = 7
};

enum {
	Fdone,
	Ftotal,
	Fbps,
	Nprogfields,
};

void
usage(void)
{
	fprint(2,"usage: %s\n", argv0);
	exits("usage");
}

void
when(char *lun)
{
	char *p, *f[Ninitfields], *pf[Nprogfields], path[64];
	Biobuf *bp;
	uvlong done, total, bps, left;
	double pct;
	int n, m, sec, min, hour;
	static int hdr = 0;

	snprint(path, sizeof path, "/raid/%s/raidstat", lun);
	bp = Bopen(path, OREAD);
	if (bp == nil)
		return;
	while (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = 0;
		n = tokenize(p, f, nelem(f));
		if (n == Ninitfields || n == Nrecovfields) {
			m = getfields(f[n-1], pf, nelem(pf), 1, "/:");
			if (m == Nprogfields) {
				if (n == Ninitfields)
					*strchr(f[Fname], '.') = 0;
				done = strtoull(pf[Fdone], 0, 0);
				total = strtoull(pf[Ftotal], 0, 0);
				bps = strtoull(pf[Fbps], 0, 0);
				left = total - done;
				pct  = (done * 100.0) / total;
				if (bps > 0)
					sec = left/bps;
				else
					sec = 0;
				min = sec/60;
				sec %= 60;
				hour = min/60;
				min %= 60;
				if (!hdr++)
					print("%-11s %11s %14s %18s\n", "LUN/ELEMENT", "COMPLETE(%)",
						"I/O RATE(KBPS)", "ESTIMATED TIME(h:m:s)");
				print("%-11s %11.2f %14g %15d:%02d:%02d\n", f[Fname], pct, bps / 1e3, hour, min, sec);
			}
		}
	}

	Bterm(bp);
	return;
}

void
main(int argc, char **argv)
{
	int i, n;
	Dir *dp;

	ARGBEGIN {
	default:
		usage();
	}ARGEND

	if (argc != 0)
		usage();

	n = numfiles("/raid", &dp);
	if (n < 0)
		errfatal("failure accessing LUN info");
	qsort(dp, n, sizeof *dp, dirintcmp);
	for(i = 0; i < n; i++)
		if (isdigit(dp[i].name[0]))
			when(dp[i].name);

	exits(nil);
}
