/*
 *  Copyright © 2013 Coraid, Inc.
 *  All rights reserved.
 *  list.c: list all the raid stuff
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

typedef struct Driveinfo Driveinfo;
typedef struct Luninfo Luninfo;

struct Driveinfo {
	vlong dlength;
	char element[10];
	char dname[10];
	char state[20];
};

struct Luninfo {
	char lun[4];
	char label[20];
	char status[10];
	char type[10];
	vlong lbsize;
	char state[20];
	int ndevs;
	char errstr[50];
	Driveinfo *drives;
};

static int hdr, labelw, statew, aflag;

static void
getstate(char *dstate, char *sstate, int len)
{
	int l;

	if (strstr(sstate, "recovering,degraded"))
		strncpy(dstate, "recovering", len);
	else if (strstr(sstate, "failed,needinit,degraded"))
		strncpy(dstate, "failed,needinit", len);
	else
		strncpy(dstate, sstate, len);
	l = strlen(dstate);
	if (statew < l)
		statew = l;
}

static int
getlabel(char *lun, char *label, int len)
{
	int l;

	if (readfile(label, len, "/raid/%s/label", lun) < 0)
		return -1;
	l = strlen(label);
	if (labelw < l)
		labelw = l;
	return 0;
}


static int
lbstat(char *lun, vlong *length, char *state, int len)
{
	char stat[300], *argv[4], *e;
	int n;

	if (readfile(stat, sizeof stat, "/raid/%s/stat", lun) < 0)
		return -1;
	n = tokenize(stat, argv, nelem(argv));
	if (n < 2)
		return -1;
	*length = strtoull(argv[0], &e, 10);
	if (*e != '\0')
		return -1;
	strncpy(state, argv[1], len);
	return 0;
}

static void
path2name(char *path, char *dname, int len)
{
	char *p;

	if (!strcmp(path, "missing"))
		strncpy(dname, "missing", len);
	else if (strstr(path, "update"))
		strncpy(dname, "update", len);

	/*
	 *  normal case - it's a raid device of form /raiddev/SLOT/data
	 *  pull the lun and add the shelf.
	 */
	else if(p = strstr(path, "/data")) {
		*p = '\0';
		if(p = strrchr(path, '/'))
			sprint(dname, "%d.%s", getshelf(), p+1);
	} else
		strncpy(dname, path, len);
}

/* Assumes l points to a zero-ed buffer */
static int
getluninfo(char *lun, Luninfo *l)
{
	char path[50];
	char *p, *f[10];
	int n, ndevs, i;
	Biobuf *bp;
	Driveinfo *d;

	if (islun(lun) == 0) {
		sprint(l->errstr, "error: LUN %s does not exist\n", lun);
		return -1;
	}
	strncpy(l->lun, lun, sizeof(l->lun)-1);
	if (lbstat(lun, &l->lbsize, l->status, sizeof(l->status)-1) < 0) {
		sprint(l->errstr, "error: cannot get LUN %s status\n", lun);
		return -1;
	}
	if (getlabel(lun, l->label, sizeof l->label) < 0) {
		sprint(l->errstr, "error: failure to read label for LUN %s\n", lun);
		return -1;
	}
	snprint(path, sizeof path, "/raid/%s/raidstat", lun);
	bp = Bopen(path, OREAD);
	while (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = 0;
		n = tokenize(p, f, nelem(f));
		n -= 6;
		if (n < 0) {
			sprint(l->errstr, "error: LUN %s raidstat wrong format\n", lun);
			Bterm(bp);
			return -1;
		}
		strncpy(l->type, f[1], sizeof(l->type)-1);
		ndevs = atoi(f[2]);
		l->ndevs = ndevs;
		getstate(l->state, f[5], sizeof(l->state)-1);
		if (d = mallocz(sizeof (Driveinfo) * ndevs, 1))
			l->drives = d;
		else {
			sprint(l->errstr, "error: LUN %s cannot malloc\n", lun);
			Bterm(bp);
			return -1;
		}
		for (i=0; i<ndevs; i++) {
			p = Brdline(bp, '\n');
			p[Blinelen(bp)-1] = 0;
			n = tokenize(p, f, nelem(f));
			n -= 4;
			if (n < 0) {
				sprint(l->errstr, "error: LUN %s raidstat wrong format\n", lun);
				Bterm(bp);
				return -1;
			}
			d[i].dlength = strtoull(f[1], 0, 0);
			path2name(f[3], d[i].dname, sizeof(d[i].dname)-1);
			strncpy(d[i].element, f[0], sizeof(d[i].element)-1);
			getstate(d[i].state, f[2], sizeof(d[i].state)-1);
		}
		if (i != ndevs) {
			sprint(l->errstr, "error: LUN %s raidstat dev format error\n", lun);
			Bterm(bp);
			return -1;
		}
	}
	Bterm(bp);
	return 0;
}

static void
printluns(Luninfo *l, int count)
{
	int k, i;
	Driveinfo *d;

	if (hdr == 1) {
		if (aflag)
			print("%-3s %*s %8s %8s %10s %9s %7s %*s\n", "LUN", labelw + 1, "LABEL",
				"STATUS", "TYPE", "SIZE(GB)", "ELEMENT", "DRIVE", statew + 1, "STATE");
		else
			print("%-3s %*s %8s %8s %10s %*s\n", "LUN", labelw + 1, "LABEL",
				"STATUS", "TYPE", "SIZE(GB)", statew + 1, "STATE");
	}
	for (k = 0; k < count; k++) {
		if (l[k].errstr[0] != 0) {
			print("%s", l[k].errstr);
			continue;
		}
		if (aflag == 0) {
			print("%-3s %*s %8s %8s %10.3f %*s\n",
				l[k].lun, labelw + 1, l[k].label, l[k].status, l[k].type,
				l[k].lbsize / 1e9, statew + 1, l[k].state);
			continue;
		}
		print("%-3s %*s %8s %8s %10.3f %9s %7s %*s\n",
			l[k].lun, labelw + 1, l[k].label, l[k].status, l[k].type,
			l[k].lbsize / 1e9, "", "", statew + 1, l[k].state);
		d = l[k].drives;
		for (i=0; i<l[k].ndevs; i++)
			print("%-3s %*s %8s %8s %10.3f %9s %7s %*s\n", "", labelw + 1, "", "", "", d[i].dlength / 1e9,
				d[i].element, d[i].dname, statew + 1, d[i].state);
		free(d);
		l[k].drives = nil;
	}
	free(l);
}

static Luninfo *
getallluns(int *total)
{
	Dir *dp;
	int n, i, count, lunindex;
	Luninfo *luns;

	n = numfiles("/raid", &dp);
	if (n < 0)
		errfatal("%r");
	qsort(dp, n, sizeof *dp, dirintcmp);
	count = 0;
	for (i = 0; i < n; i++)
		if (isdigit(dp[i].name[0]))
			count++;
	luns = mallocz(sizeof (Luninfo) * count, 1);
	lunindex = 0;
	for (i = 0; i < n && lunindex < count; i++)
		if (isdigit(dp[i].name[0]))
			getluninfo(dp[i].name, &luns[lunindex++]);
	free(dp);
	*total = count;
	hdr = 1;
	return luns;
}

void
usage(void)
{
	fprint(2, "usage: %s [ -a ] [ LUN ... ]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	Luninfo *luns;
	int totallun;
	char *e;
	ulong lun;

	ARGBEGIN {
	case 'l':
	case 'a':
		aflag++;
		break;
	default:
		usage();
	} ARGEND
	labelw = 6;
	statew = 8;
	if (argc == 0)
		luns = getallluns(&totallun);
	else {
		luns = mallocz(sizeof (Luninfo) * argc, 1);
		totallun = argc;
		while (argc-- > 0) {
			lun = strtoul(*argv, &e, 10);
			if (lun >= 255 || *e != '\0')
				errfatal("LUN must be an integer between 0 and 254 inclusive");
			if (getluninfo(*argv++, &luns[totallun - argc - 1]) == 0)
				hdr = 1;
		}
	}
	if (totallun)
		printluns(luns, totallun);
	exits(nil);
}
