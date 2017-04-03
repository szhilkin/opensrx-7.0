// Copyright © 2013 Coraid, Inc
// All rights reserved.

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

enum {
	Fsize,
	Fstate,
	Fcmstate,
	Ffcstate,
	Nfields,
};

static char Fstatenabled[] = "enabled";
static uvlong totalbuf;

static void
usage(void)
{
	fprint(2,"usage: %s\n", argv0);
	exits("usage");
}

static int
getfcinfo(void)
{
	Biobuf *bp;
	char *p, *f[3];

	bp = Bopen("/n/cpns/cache/1/stats", OREAD);
	if (bp == nil)
		return -1;
	while (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = 0;
		if (tokenize(p, f, nelem(f)) != 2)
			continue;
		if (strcmp(f[0], "nbuffers:") == 0)
			totalbuf = strtoull(f[1], 0, 0);
	}
	Bterm(bp);
	return 0;
}

static void
fclunstat(char *lun)
{
	enum { K= 1000, M= K*K, G= M*K};
	int n, i;
	char *p, *f[Nfields], path[64], buf[256], *toks[16];
	Biobuf *bp;
	static int hdr = 0;
	uvlong nbuf;

	snprint(path, sizeof path, "/raid/%s/stat", lun);
	bp = Bopen(path, OREAD);
	if (bp == nil)
		return;
	if (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = 0;
		n = tokenize(p, f, nelem(f));
		if (n != Nfields)
			goto end;
		if (readfile(buf, sizeof buf, "/raid/%s/ctl", lun) > 0) {
			n = tokenize(buf, toks, nelem(toks));
			if (!hdr++)
				print("%-7s %8s %4s %7s %7s %8s %9s\n", "LUN", "STATUS", "PRI",
					"MINPCT", "USEDPCT", "TOTALHIT", "RECENTHIT");
			print("%-7s %8s", lun, &f[Ffcstate][2]);
			for (i = 0; i < n; i++) {
				if (strcmp(toks[i], "fcpri:") == 0)
					print(" %4s", toks[i+1]);
				if (strcmp(toks[i], "fcminpct:") == 0)
					print(" %7s", toks[i+1]);
			}
		}
		if (strcmp(&f[Ffcstate][2], Fstatenabled) == 0) {
			if (readfile(buf, sizeof buf, "/n/cpns/cache/1/targets/%s", lun) > 0) {
				n = tokenize(buf, toks, nelem(toks));
				for (i = 0; i < n; i++) {
					if (strcmp(toks[i], "blocks:") == 0) {
						nbuf = strtoull(toks[i+1], 0, 0);
						if (totalbuf > 0) {
							nbuf = nbuf * 100 / totalbuf;
							print(" %7ulld", nbuf);
						} else
							print(" %7s", "");
					} else if (strcmp(toks[i], "hitrate:") == 0) {
						if (strcmp(toks[i+1], "undefined") != 0)
							print(" %7s%%", toks[i+1]);
						else
							print(" %7s%%", "0");
					} else if (strcmp(toks[i], "recenthitrate:") == 0) {
						if (strcmp(toks[i+1], "undefined") != 0)
							print(" %8s%%", toks[i+1]);
						else
							print(" %8s%%", "0");
					}
				}
			} else
				print(" %7s %7s%% %8s%%", "0", "0", "0");
		}
		print("\n");
	}
end:
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
	if (getfcinfo() != 0)
		errfatal("failed to get cache information");
	for(i = 0; i < n; i++)
		if (isdigit(dp[i].name[0]))
			fclunstat(dp[i].name);

	exits(nil);
}
