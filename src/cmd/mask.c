/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 * mac masking a LUN
*/
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

int phdr;

void
usage(void)
{
	fprint(2,"usage: mask [ {+|-}mac ... ] [ LUN ... ]\n");
	exits("usage");
}

int
mask(char *mac, char *lun)
{
	if (islun(lun) == 0) {
		werrstr("LUN %s does not exist", lun);
		return -2;
	}
	if (mac[0] == '+')
		return lunctlwrite(lun, "mask %s", mac + 1);
	else if (mac[0] == '-')
		return lunctlwrite(lun, "rmmask %s", mac + 1);
	else {
		werrstr("wrong format of mask");
		return -1;
	}
}

void
masks(int n, int c, char **v)
{
	int i, j;
	int res;

	if (n == c) {
		fprint(2, "error: no LUN specified\n");
		return;
	}

	for (i = 0; i < n; i++)
		for (j = n; j < c; j++) {
			res = mask(v[i], v[j]);
			if (res == -2)
				errskip(c - j - 1 , v + j +1);
			else if (res < 0)
				errskip(n - i - 1 , v + i + 1);
		}
}

void
printlun(char *lun)
{
	char buf[Maxbuf], *args[Maxargs];
	int i, j, n;

	if (islun(lun) == 0) {
		fprint(2, "LUN %s does not exist\n", lun);
		return;
	}
	if (readfile(buf, Maxbuf, "/raid/%s/masks", lun) < 0) {
		fprint(2, "error: LUN %s has no mask information\n", lun);
		return;
	}
	if ((n = tokenize(buf, args, Maxargs)) < 0) {
		fprint(2, "error: %s has wrong mask file format\n", lun);
		return;
	}
	if (phdr == 0) {
		print("%-9s %-s\n", "LUN", "MASK(S)");
		phdr++;
	}
	print("%-9s ", lun);
	for (i = 0; i < n; i += j) {
		if (i)
			print("%27s", " ");
		for (j = 0; i+j < n && j < 4; j++)
			print("%s ", args[i+j]);
		print("\n");
	}
	if (i == 0)
		print("\n");
}

void
printallluns(void)
{
	int i, n;
	Dir *dp;

	n = numfiles("/raid", &dp);
	if (n < 0)
		sysfatal("no LUN is present");
	qsort(dp, n, sizeof *dp, dirintcmp);
	for (i = 0; i < n; i++)
		if (isdigit(dp[i].name[0]))
			printlun(dp[i].name);

	free(dp);
}


int
nmac(int c, char **v)
{
	int i;

	for (i = 0; i < c; i++)
		if (*v[i] != '-' && *v[i] != '+')
			break;
	return i;
}

void
main(int argc, char **argv) 
{
	int n;

	if (argc == 1) {
		printallluns();
		exits(nil);
	}

	argv0 = *argv++;
	argc--;
	if (strcmp(*argv, "-?") == 0)
		usage();

	if ((n = nmac(argc, argv)) > 0)
		masks(n, argc, argv);
	else
		while (argc-- > 0)
			printlun(*argv++);
	exits(nil);
}
