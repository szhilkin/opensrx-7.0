/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 * LUN reservations
*/
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

char Knilkey[] = "0000000000000000";
char Konekey[] = "FFFFFFFFFFFFFFFF";

enum {
	Nkey = 16,
	Nmac = 12,
};

static void
usage(void)
{
	fprint(2, "usage: %s [ [ -c ] LUN ... ]\n", argv0);
	exits("usage");
}

static int
clearresvs(char *lun)
{
	char *b;
	int n, fd;

	if (!islun(lun)) {
		fprint(2, "error: LUN %s does not exist\n", lun);
		return -1;
	}
	n = -1;
	b = mustsmprint("/raid/%s/ctl", lun);
	fd = open(b, OWRITE);
	if (fd >= 0) {
		n = fprint(fd, "krreset");
		close(fd);
	}
	free(b);
	if (n < 0)
		fprint(2, "Failed to clear reservations for LUN %s: %r", lun);
	return n;
}

static void
printheader(void)
{
	print("%-9s %-5s %-16s %-16s %-12s\n", "LUN", "RTYPE", "OWNER", "REGISTRANTS", "MAC ADDRESSES");
}

static void
printoneline(char *lun, char *rtype, char *owner, char *key, char *mac)
{
	print("%-9s ", (lun)? lun : "");
	print("%-5s ", (rtype)? rtype : "");
	if (owner && cistrcmp(owner, Knilkey) != 0 && cistrcmp(owner, Konekey) != 0)
		print("%16s ", owner);
	else
		print("%16s ", "");
	if (key && cistrcmp(key, Konekey) != 0)
		print("%16s ", key);
	else
		print("%16s ", "");
	print("%.12s\n", (mac)? mac : "");
}

/* This function loads all the key->MACs mappings from /n/rr/<lun>/keys file
 * The format of the keys file is
 * 	<key1> <mac1> <mac2>...
 *	<key2> <mac3> <mac4>...
 *	...
 */
static char *
readkeys(char *lun)
{
	char *fkeys, *kbuf, *p;
	Biobuf *bkeys;
	int ksize, n;

	ksize = 512;
	fkeys = mustsmprint("/n/rr/%s/keys", lun);
	if ((kbuf = malloc(sizeof (char) * ksize)) == 0) {
allocerror:
		fprint(2, "error: out of memory");
		exits("out of memory");
		return nil;
	}
	if ((bkeys = Bopen(fkeys, OREAD)) == 0) {
		fprint(2, "could not read keys\n");
		free(kbuf);
		free(fkeys);
		return nil;
	}
	p = kbuf;
	while ((n = Bread(bkeys, p, 512)) == 512) {
		ksize += 512;
		if ((kbuf = realloc(kbuf, sizeof (char) * ksize)) == 0)
			goto allocerror;
		p = kbuf + ksize - 512;
	}
	kbuf[ksize - 512 + n] = 0;
	Bterm(bkeys);
	free(fkeys);
	return kbuf;
}

/* The printresvs function reads the /n/rr/<lun>/stats file to get the
 * current owner and the registered keys. It then looks for the MACs
 * corresponding to the keys and prints a line for each such entry. The
 * format of the /n/rr/<lun>/stats file is
 *	<rtype> <gen #> <owner key> <key1> <key2>...
 */
static void
printresvs(char *lun)
{
	Biobuf *bstat;
	char *sbuf, *kbuf, *p, *e;
	char *fstat;
	char *f[3], *keys, *rtype, *owner;
	int first, n;

	if (!islun(lun)) {
		fprint(2, "error: LUN %s does not exist\n", lun);
		return;
	}
	kbuf = readkeys(lun);
	if (kbuf == 0 || *kbuf == 0) {
		printoneline(lun, nil, nil, nil, nil);
		goto error2;
	}
	fstat = mustsmprint("/n/rr/%s/stat", lun);
	if ((bstat = Bopen(fstat, OREAD)) == nil) {
		fprint(2, "could not read reservations\n");
		goto error3;
	}
	sbuf = 0;
	if ((p = Brdline(bstat, '\n')) != 0) {
		n = Blinelen(bstat);
		sbuf = malloc(sizeof (char) * (n+1));
		strncpy(sbuf, p, n);
		sbuf[n] = 0;
		if (tokenize(sbuf, f, 3) != 3) {
			fprint(2, "error: unable to get information for LUN %s\n", lun);
			goto error4;
		}
		rtype = f[0];
		owner = f[2];
		keys = f[2] + strlen(f[2]) + 1;
		if (*keys == 0)
			printoneline(lun, nil, nil, nil, nil);
		first = 1;
		while (*keys != 0 && getfields(keys, f, 2, 1, "\n ") > 0) {
			p = strstr(kbuf, f[0]);
			p += Nkey + 1;
			e = strchr(p, '\n');
			while (p < e) {
				printoneline(lun, rtype, owner, f[0], p);
				if (first == 1)
					first = 0;
				f[0] = lun = rtype = owner = nil; /* marking them as NULL, so they aren't printed again */
				p += Nmac + 1;
			}
			keys = f[1];
		}
	}
error4:
	free(sbuf);
	Bterm(bstat);
error3:
	free(fstat);
error2:
	free(kbuf);
	return;
}

static void
printallresvs(void)
{
	int i, n;
	Dir *dp;

	n = numfiles("/raid", &dp);
	if (n < 0)
		return;

	printheader();
	qsort(dp, n, sizeof *dp, dirintcmp);
	for (i=0; i<n; i++)
		if (isdigit(dp[i].name[0]))
			printresvs(dp[i].name);
	free(dp);
}

void
main(int argc, char **argv) 
{
	int clear = 0;

	ARGBEGIN {
	default:
		usage();
	case 'c':
		clear = 1;
	} ARGEND

	if (argc == 0) {
		if (clear == 1)
			usage();
		else
			printallresvs();
	}
	else {
		if (clear == 1)
			while (argc-- > 0)
				clearresvs(*argv++);
		else {
			printheader();
			while (argc-- > 0)
				printresvs(*argv++);
		}
	}
	exits(nil);
}
