/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */

#include <u.h>
#include <libc.h>
#include <fis.h>
#include <ctype.h>
#include <bio.h>
#include <libcutil.h>
#include <rawrd.h>
#include <rawrdufops.h>
#include "srxcmds.h"

enum{
	Maxdisk		= 256,
	Nconflen	= 1024,
	Nmagic		= 8,
};

typedef struct Dattrs Dattrs;
typedef struct Iostats Iostats;

struct Iostats {
	ulong ios;
	ulong avglat;
	ulong maxlat;
};

struct Dattrs {
	Sfis;
	int	ssd;
	uvlong	noted;
	uvlong sectors;
	char*	model;
	char*	fwver;
	char*	serial;
	char*	config;
	void*	udata;
	ushort	id[512/2];
	char	inq[255];
	Iostats rstats;
	Iostats wstats;
};

static Rawrd	disks[Maxdisk];
static int	ndisks;
static int	shelf;

int aflag;
int cflag;
int dflag;
int eflag;
int fcflag;
int iflag;
int jflag;
int lflag;
int nflag;
int ppflag;
int gflag;
int qflag;
int rflag;
int sflag;
int ssdhealth;
int tflag;
long nval;
int flcache;
int secerase;
int secunlock;
int secdisable;
int wflag;
int xflag;
int fflag;
int nlines;
static	char 	*drtab[Maxdisk];

typedef struct Srconfig Srconfig;
struct Srconfig {
	char	magic[Nmagic];
	uchar	length[4];
	uchar	sectoralign[512 -8 -4];
	char	config[Nconflen];
};

typedef struct AV AV;
struct AV {
	AV		*next;
	char		*attr;
	char		*value;
};

void
usage(void)
{
	fprint(2, "usage: %s [ -pcs ] [ shelf.slot ... ]\n", argv0);
	exits("usage");
}

void
freeavl(AV *list)
{
	AV *p;

	while (p = list) {
		list = p->next;
		free(p);
	}
}

int path2dno(char *path);

char *
mbstr(char *p, int n, ulong bytes)
{
	enum { K= 1000, M= K*K, };
	ulong m, k;

	m = bytes / M;
	k = bytes % M / K;
	snprint(p, n, "%uld.%03uldMB", m, k);
	return p;
}

AV *
readavl(char *file)
{
	char buf[2048];
	int fd, n;
	char *cp, *cp1, *cp2;
	AV *head = nil, *tail = nil, *avp;

	fd = open(file, OREAD);
	if (fd < 0) {
		fprint(2, "readavl: open(%s, OREAD) failure: %r\n", file);
		return nil;
	}
	n = read(fd, buf, sizeof buf - 2);
	if (n <= 0) {
		fprint(2, "readavl: read %s failure: %r\n", file);
		return nil;
	}
	close(fd);
	if (buf[n-1] != '\n')
		buf[n++] = '\n';
	buf[n] = 0;
	cp = buf;
	do {
		avp = mallocz(sizeof *avp, 1);
		while ((cp1 = strchr(cp, ':')) == nil) {
			cp1 = strchr(cp, '\n');
			if (cp1 == nil)
				return head;
			cp = ++cp1;
		}			
		*cp1++ = 0;
		while (*cp1 && isspace(*cp1))
			cp1++;
		cp2 = strchr(cp1, '\n');
		assert(cp2);
		*cp2++ = 0;
		avp->attr = strdup(cp);
		avp->value = strdup(cp1);
		if (head == nil)
			head = avp;
		else
			tail->next = avp;
		tail = avp;
		cp = cp2;
	} while (*cp);
	return head;
}

Srconfig *
readconfig(Rawrd *d)
{
	char buf[32];
	int fd, n;
	Srconfig *s;

	snprint(buf, sizeof buf, "%s/data", d->path);
	fd = open(buf, OREAD);
	if (fd < 0) {
		fprint(2, "readconfig: open(%s, OREAD) failure: %r\n", buf);
		return nil;
	}
	s = malloc(sizeof *s);
	if (s == nil)
		sysfatal("unable to malloc for config");
	n = read(fd, s, sizeof *s);
	close(fd);
	if (n != sizeof *s || memcmp(s->magic, "coraid01", Nmagic) != 0 &&
	    memcmp(s->magic, "coraid00", Nmagic) != 0) {
		free(s);
		s = nil;
	}
	return s;
}

char *
getav(AV *p, char *attr)		// return value of attr
{
	while (p) {
		if (strcmp(p->attr, attr) == 0)
			return p->value;
		p = p->next;
	}
	return "";
}

char *
getconfig(Rawrd *d)
{
	Srconfig *s;
	char *p;

	s = readconfig(d);
	if (s == nil)
		return "";
	p = malloc(Nconflen+1);
	if (p == nil)
		sysfatal("unable to malloc for config string");
	snprint(p, begetl(s->length), "%s", s->config);
	free(s);
	return p;
}

char *
getversion(Rawrd *d)
{
	Srconfig *s;
	char *p;

	s = readconfig(d);
	if (s == nil)
		return "";
	p = mallocz(3+1, 1);
	if (p == nil)
		sysfatal("unable to malloc for vers string");
	if (memcmp(s->magic, "coraid01", Nmagic) == 0)
		sprint(p, "1");
	else if (memcmp(s->magic, "coraid00", Nmagic) == 0)
		sprint(p, "0");
	free(s);
	return p;
}

static int
getstats(Rawrd *d)
{
	char buf[64], *p, *f[10];
	Biobuf *bp;
	Dattrs *a;

	a = d->aux;
	if (a == nil)
		return -1;
	snprint(buf, sizeof buf, "%s/iostats", d->path);
	bp = Bopen(buf, OREAD);
	if (bp == nil)
		return -1;
	if (bp != nil && (p = Brdline(bp, '\n'))) {
		p[Blinelen(bp)-1] = 0;
		if (getfields(p, f, sizeof(f), 1, ":/ ") != 10)
			return -1;
		a->rstats.ios = strtoull(f[1], 0, 0);
		a->rstats.avglat = strtoul(f[3],0,0)/1000;
		a->rstats.maxlat = strtoul(f[4],0,0)/1000;
		a->wstats.ios = strtoull(f[6], 0, 0);
		a->wstats.avglat = strtoul(f[8],0,0)/1000;
		a->wstats.maxlat = strtoul(f[9],0,0)/1000;
		Bterm(bp);
		return 0;
	}
	return -1;
}

int
alldisks(char *arg[], int narg)
{
	int i;

	if (narg > ndisks)
		narg = ndisks;
	for (i=0; i<narg; i++)
		arg[i] = strdup(disks[i].name);
	return narg;
}

char *
cstatus(Rawrd *d, int status)
{
	static char buf[ERRMAX];

	switch (status) {
	default:
	case Srsunk:
		if (d->estr[0]) {
			snprint(buf, sizeof buf, "unknown (%s)", d->estr);
			return buf;
		}
		return "unknown";
	case Srsdisabled:
		return "disabled";
	case Srsnormal:
		return "normal";
	case Srsthresh:
		return "threshold exceeded";
	}
}

char *
smartrs(Rawrd *d)
{
	int status;

	status = Srsunk;
	switch (d->type) {
	case Tata:
		rawrdatasmartrs(d, &status);
		break;
	case Tscsi:
		snprint(d->estr, sizeof d->estr, "smart not available for scsi");
		break;
	default:
		snprint(d->estr, sizeof d->estr, "drive type unknown");
	}
	return cstatus(d, status);
}

int
smartrd(Rawrd *d, uchar *sdata, int len)
{
	switch (d->type) {
	case Tata:
		return rawrdatasmartrd(d, sdata);
		break;
	case Tscsi:
		return rawrdscsismartrd(d, sdata, len);
		break;
	default:
		snprint(d->estr, sizeof d->estr, "drive type unknown");
	}
	return -1;
}

int
drivetemp(Rawrd *d, uchar *tdata, int len, uchar *temp)
{
	int rv;

	rv = -1;
	switch (d->type) {
	case Tata:
		rv = rawrdatatemp(d, tdata, len, temp);
		if (rv < 0)
			snprint(d->estr, sizeof d->estr, "not provided");
		break;
	case Tscsi:
		rv = rawrdscsitemp(d, tdata, len, temp);
		if (rv < 0)
			snprint(d->estr, sizeof d->estr, "not provided");
		break;
	default:
		snprint(d->estr, sizeof d->estr, "missing");
	}
	return rv;
}

int
dno(char *spec)
{
	char *p;
	int sh, sl;

	if (spec == nil)
		return -1;
	sh = atoi(spec);
	p = strchr(spec, '.');
	if (p == nil)
		return -1;
	sl = atoi(&p[1]);
	if (sh != shelf)
		return -1;		// issue diagnostic?
	return sl;
}

static char *
wstrim(char *s, char *e)
{
	for (; e > s; e--) {
		switch (*e) {
		case ' ':
		case '\t':
		case '\r':
		case '\n':
			*e = 0;
			continue;
		}
		break;
	}
	for (; s < e; s++) {
		switch (*s) {
		case ' ':
		case '\t':
		case '\r':
		case '\n':
			continue;;
		}
		break;
	}
	return (char *) s;
}

static char *
idstr(ushort *ident, int off, int len)
{
	int e;
	char buf[65], *cp;

	memset(buf, 0, sizeof buf);
	cp = buf;
	if (len > sizeof buf / 2)
		len = sizeof buf / 2;
	for (e=off+len; off<e; off++) {
		*cp++ = ident[off] >> 8;
		*cp++ = ident[off];
	}
	return strdup(wstrim(buf, --cp));
}

void
readinq(Dattrs *a)
{
	char *inq;
	char id[17];
	char vnd[9];
	char buf[26];

	inq = a->inq;
	memcpy(vnd, inq+8, 8), vnd[8] = 0;
	memcpy(id, inq+16, 16), id[16] = 0;
	snprint(buf, sizeof buf, "%s %s", wstrim(vnd, vnd+7), wstrim(id, id+15));
	a->model = strdup(buf);
	inq[36] = 0;
	a->fwver = strdup(wstrim(inq+32, inq+35));
}

int
ident(Rawrd *d)
{
	Dattrs *a;
	ushort *id;
	uvlong v;
	enum { Scmds1= 83, Slba28= 60, Slba48= 100, };
	uchar inquiry[64];
	Drivegeometry g;

	a = d->aux;
	switch (d->type) {
	case Tata:
		id = a->id;
		if (rawrdataidentify(d, (uchar *) id) == -1)
			return -1;
		a->serial = idstr(id, 10, 20-10);
		a->fwver = idstr(id, 23, 27-23);
		a->model = idstr(id, 27, 47-27);
		v = legetl((uchar*)&id[Slba28]);
		if (legets((uchar*)&id[Scmds1]) & (1<<10))	// lba48 supported
		if (v == (1<<28) - 1)
			v = legetv((uchar*)&id[Slba48]);
		a->sectors = v;
		a->ssd = legets((uchar*)&id[217]);
		break;
	case Tscsi:
		if (rawrdscsiinquiry(d, 0, (uchar *) a->inq, sizeof a->inq) != 0)
			return -1;
		readinq(a);
		rawrdscsireadcap(d, &g);
		a->sectors = g.sectors;
		/*  Some bad drives fail to respond to VPD 0xb1 */
		if (rawrdscsiinquiry(d, 0xb1, inquiry, sizeof inquiry) == 0 &&
			begets(inquiry+4) == 1)
			a->ssd = 1;
		break;
	default:
		return -1;
	}
	return 0;
}

void
fmtsz(Rawrd *d, char *p, int np)
{
	enum { KB= 1000, MB= KB*KB, GB= MB*KB, };
	Dir *dir;
	vlong sz;
	char buf[64];
	char *ast = "";
	Dattrs *a;

	a = d->aux;
	sz = a->sectors *512;
	snprint(buf, sizeof buf, "%s/data", d->path);
	if (dir = dirstat(buf)) {	// check for setsize modification
		if (dir->length != sz) {
			ast = "*";
			sz = dir->length;
		}
		free(dir);
	}
	snprint(p, np, "%s%lld.%03lld", ast, sz / GB, sz % GB / MB);
}

static int
smartenable(Rawrd *d, int dis)
{
	int n;

	n = -1;
	switch (d->type) {
	case Tata:
		n = rawrdatasmartenable(d, dis);
		break;
	case Tscsi:
		snprint(d->estr, sizeof d->estr, "smart not available for scsi");
		break;
	}
	return n;
}

int
smartrl(Rawrd *d)
{
	int i, j, k, l;
	uchar sdata[512], *pdata;

	switch (d->type) {
	case Tata:
		if (rawrdatasmartrl(d, sdata) == 0)
			break;
		print("\tsmart error log read failure: %s\n", d->estr);
		return -1;
	case Tscsi:
		print("\tsmart not available for scsi\n");
		return -1;
	default:
		return -1;
	}

	/* error log index.  If zero, there are no log entries */
	if (sdata[1] == 0) {
		print("\tno error log entries found\n");
		return 0;
	}
	print("\terror log entries found\n");
	pdata = &sdata[2];
	for (i = 4; i >= 0; --i) {
		l = (i + sdata[1]) % 5;
		j = 5;
		k = (l * 90) + (j * 12);

		if (pdata[k+1] == 0)
			continue;

		print("\tlifetime:%d  errors:%d count:%d  lba:%d dev:%02x status:%02x state:%02x\n",
			pdata[k+29]<<8 | pdata[k+28],
			pdata[k+1],
			pdata[k+2],
			pdata[k+5]<<16 | pdata[k+4]<<8 | pdata[k+3],
			pdata[k+6],
			pdata[k+7],
			pdata[k+27]);

		for  (--j; j >= 0; --j) {
			k = (l * 90) + (j * 12);

			if (pdata[k+1] == 0)
				continue;

			print ("\tts:%d tsv:%02x feat:%02x count:%d lba:%d dev:%02x command:%02x\n",
			pdata[k+11]<<24 | pdata[k+10]<<16 | pdata[k+9]<<8 | pdata[k+8],
			pdata[k],
			pdata[k+1],
			pdata[k+2],
			pdata[k+5]<<16 | pdata[k+4]<<8 | pdata[k+3],
			pdata[k+6],
			pdata[k+7]);				
		}
	}
	print("\terrcount:%d\n", sdata[453]<<8 | sdata[452]);
	return 0;
}

Rawrd *
atodisk(char *spec)
{
	int n;

	n = dno(spec);
	if (n < 0 || n >= ndisks) {
		fprint(2, "%s does not exist\n", spec);
		return nil;
	}
	return &disks[n];
}

char *
wcable(Rawrd *d)
{
	Dattrs *a;
	ushort *id;
	uchar mspg[32];
	int e;
	char *rs[] = { "disabled", "enabled", "unsupported" };

	a = d->aux;
	id = a->id;
	e = 2;
	switch (d->type) {
	case Tata:
		if ((legets((uchar*)&id[82]) & (1<<5)) == 0)
			break;
		e = (legets((uchar*)&id[85]) & (1<<5)) != 0;
		break;
	case Tscsi:
		if (rawrdscsimodesense(d, 8, mspg, sizeof mspg) != 0)
			break;
		e = (mspg[10] & (1<<2)) != 0;
		break;
	}
	return rs[e];
}

char *
spec(Rawrd *d)
{
	static char buf[16];
	Dattrs *a;
	ushort *id;
	int n, i;

	a = d->aux;
	id = a->id;
	switch (d->type) {
	case Tata:
		n = legets((uchar*)&id[80]);
		for (i=0; n>>=1; i++)
			;
		snprint(buf, sizeof buf, "ata-%d", i);
		break;
	case Tscsi:
		snprint(buf, sizeof buf, "scsi-?");
		break;
	}
	return buf;
}

void
addlinfo(Rawrd *d)
{
	Dattrs *a;
	AV *av, *oav;
	char buf[64];
	char *fmt = "%10s: %q\n";

	a = d->aux;
	av = (AV *) a->udata;
	for (; av; av=av->next)
		print(fmt, av->attr, av->value);
	print(fmt, "smart", smartrs(d));
	print(fmt, "wc", wcable(d));
	print(fmt, "spec", spec(d));

	snprint(buf, sizeof buf, "%s/stats", d->path);
	if (access(buf, AREAD) == 0) {
		oav = av = readavl(buf);
		for (; av; av=av->next)
			print(fmt, av->attr, av->value);
		freeavl(oav);
	}
}

char *
mode(Dattrs *a)
{
	static char m[64];
	AV *av;

	av = (AV *) a->udata;
	snprint(m, sizeof m, "%s %s", getav(av, "type"), getav(av, "link"));
	return m;
}
	
void
avlstat(Rawrd *d)
{
	Dattrs *a;
	char buf[64];

	a = d->aux;
	snprint(buf, sizeof buf, "%s/stat", d->path);
	a->udata = readavl(buf);
}

static void
rawinfo(Rawrd *d)
{
	Dattrs *a;

	if (ident(d) < 0)
		return;
	a = d->aux;
	switch (d->type) {
	case Tata:
		write(1, a->id, sizeof a->id);
		break;
	case Tscsi:
		write(1, a->inq, sizeof a->inq);
		break;
	}
}

static void
secureerase(Rawrd *d)
{
	print("erasing %s ... ", d->name);
	if (d->type != Tata) {
		fprint(2, "secure erase not available for scsi drive %s\n", d->name);
		return;
	}
	if (rawrdataerase(d) < 0)
		fprint(2, "unable to erase %s: %s\n", d->name, d->estr);
	else
		print("done\n");
}

static void
secureunlock(Rawrd *d)
{
	print("unlocking %s ... ", d->name);
	if (d->type != Tata) {
		fprint(2, "secure unlock not available for scsi drive %s\n", d->name);
		return;
	}
	if (rawrdataunlock(d) < 0)
		fprint(2, "unable to unlock %s: %s\n", d->name, d->estr);
	else
		print("done\n");
}

static void
flushcache(Rawrd *d)
{
	if (d->type != Tata && d->type != Tscsi) {
		fprint(2, "drive %s: missing - skipping\n", d->name);
		return;
	}
	fprint(2, "drive %s: flushing cache ... ", d->name);
	if (rawrdflushcache(d) < 0)
		fprint(2, "unable to flush cache: %r\n");
	else
		fprint(2, "done\n");
}

static void
securedisable(Rawrd *d)
{
	print("disabling %s ... ", d->name);
	if (d->type != Tata) {
		fprint(2, "secure disable not available for scsi drive %s\n", d->name);
		return;
	}
	if (rawrdatadisable(d) < 0)
		fprint(2, "unable to disable %s: %s\n", d->name, d->estr);
	else
		print("done\n");
}

void
seterc(Rawrd *d)
{
	Dattrs *a;
	ushort *id;
	ulong max = 65535*100;

	a = d->aux;
	id = a->id;
	if (nval < 0 || nval > max) {
		fprint(2, "Invalid ERC timeout %ld [0-%ld]\n", nval, max);
		exits("bad erc timeout");
	}
	switch(d->type) {
	case Tata:
		ident(d);
		if ((legets((uchar*)&id[206]) & (1<<3)) == 0) {
			fprint(2, "SCT command set not supported for %s\n", d->name);
			return;
		}
		if (rflag)
		if (rawrdataseterc(d, 0, nval) < 0)
			fprint(2, "error setting ERC read timer for %s: %r\n", d->name);
		if (wflag)
		if (rawrdataseterc(d, 1, nval) < 0)
			fprint(2, "error setting ERC write timer for %s: %r\n", d->name);
		break;
	case Tscsi:
		print("SCSI ERC not yet implemented - skipping %s\n", d->name);
		break;
	}
}

void
geterc(Rawrd *d)
{
	int rd, wr;
	ushort *id;

	id = ((Dattrs*)d->aux)->id;
	switch(d->type) {
	case Tata:
		ident(d);
		if ((legets((uchar*)&id[206]) & 1<<3) == 0) {
			fprint(2, "SCT command set not supported for %s\n", d->name);
			return;
		}
		if ((rd = rawrdatageterc(d, 0)) < 0) {
			fprint(2, "Error getting ERC read timeout for %s: %r\n", d->name);
			break;
		}
		if ((wr = rawrdatageterc(d, 1)) < 0) {
			fprint(2, "Error getting ERC write timeout for %s: %r\n", d->name);
			break;
		}
		if (!nlines++)
			print("%-8s %9s %9s\n", "DRIVE", "READ", "WRITE");
		print("%-8s %7dms %7dms\n", d->name, rd, wr);
		break;
	case Tscsi:
		print("SCSI ERC not yet implemented - skipping %s\n", d->name);
		break;
	}
}

void
derrstate(Rawrd *d)
{
	char *p;
	char *estr = "";

	p = getav(((Dattrs*)d->aux)->udata, "state");
	if (strcmp(p, "up") == 0)	// this would be weird.  print estr as well.
		estr = d->estr;
	print("%-8s %12s %s\n", d->name, p, estr);
}

void
loadspares(void)
{
	char *f[4], *s;
	int dno, n;
	Biobuf *bp;

	bp = Bopen("/raid/spares", OREAD);
	if (bp == nil)
		return;
	while (s = Brdline(bp, '\n')) {
		s[Blinelen(bp)-1] = '\0';
		n = tokenize(s, f, nelem(f));
		if (n < 2) {
			fprint(2, "loadspares format error\n");
			break;
		}
		dno = path2dno(f[0]);
			if (dno != -1 && dno < Maxdisk)
				drtab[dno] = "spare";
	}
	Bterm(bp);
}

void
loadcaches(void)
{
	char *f[4], *s;
	int dno, n;
	Biobuf *bp;

	bp = Bopen("/raid/fcache", OREAD);
	if (bp == nil)
		return;
	while(s = Brdline(bp, '\n')) {
		s[Blinelen(bp)-1] = '\0';
		n = tokenize(s, f, nelem(f));
		if (n < 2) {
			fprint(2, "loadcaches format error\n");
			break;
		}
		dno = path2dno(f[0]);
			if (dno != -1 && dno < Maxdisk)
				drtab[dno] = "cache";
	}
	Bterm(bp);
}

int
path2dno(char *path)
{
	char *p;

	if (strcmp(path, "missing") == 0)
		return -1;
	if (strstr(path, "update") != nil)
		return -1;
	/*
	 *  normal case - it's a raid device of form /raiddev/SLOT/data
	 *  pull the slot(drive).
	 */
	if (p = strstr(path, "/data")) {
		*p = '\0';
		if (p = strrchr(path, '/'))
			return  atoi(++p);
	}
	fprint(2, "path2dno format assumption failure\n");
	return -1;
}

void
loaddinuse(int lun)
{
	char *p, *f[10], path[64];
	int ndevs,dno,n,i;
	Biobuf *bp;

	snprint(path, sizeof path, "/raid/%d/raidstat", lun);
	bp = Bopen(path, OREAD);
	if (bp == nil)
		return;
	while (p = Brdline(bp, '\n')) {
		p[Blinelen(bp)-1] = '\0';
		n = tokenize(p, f, nelem(f));
		if (n < 6) {
			fprint(2, "loaddinuse raid format error\n");
			break;
		}
		ndevs = atoi(f[2]);
		for (i=0;  i<ndevs;  i++) {
			p = Brdline(bp, '\n');
			if (p == nil)
				break;
			p[Blinelen(bp)-1] = '\0';
			n = tokenize(p, f, nelem(f));
			if (n < 4)
				break;
			dno = path2dno(f[3]);
			if (dno != -1 && dno < Maxdisk)
				drtab[dno] = strdup(f[0]);
		}
		if (i  !=  ndevs) {
			fprint(2, "loaddinuse dev format error\n");
			break;
		}
	}
	Bterm(bp);
}

void
initdrtab(void)
{
	int fd, i, dno, n;
	Dir *dp;
	char *r;
	
	loadspares();
	loadcaches();
	fd = open("/raid", OREAD);
	if (fd < 0)
		return;
	n = dirreadall(fd, &dp);
	close(fd);
	for (i = 0; i < n; i++) {
		dno = strtol(dp[i].name,&r,0);
		if (*r == '\0' && dno < Maxdisk)
			loaddinuse(dno);
	}
	free(dp);
}

char *
getrole(char *s)
{
	int lun;
	char *p;

	lun = dno(s);
	if (lun < 0)
		return "";
	p = drtab[lun];
	if (p == nil)
		return "";
	return p;
}

static int
decoderpm(ushort rpm)
{
	switch (rpm) {
	case 1:
		rpm = 0;
		/* fall thru */
	default:
		if (rpm == 0 || rpm >= 0x0401 && rpm < 0xFFFF)
			return rpm;
		/* fall thru */
	case 0:
		return -1;
	}
}

static char*
decodeform(ushort ff)
{
	switch (ff) {
	case 1:
		return "5.25 inch";
	case 2:
		return "3.5 inch";
	case 3:
		return "2.5 inch";
	case 4:
		return "1.8 inch";
	case 5:
		return "< 1.8 inch";
	default:
		return "unknown";
	}
}

static void
jgeometry(Rawrd *d)
{
	Drivegeometry g;

	if (d->type != Tata && d->type != Tscsi)
		return;

	memset(&g, 0, sizeof g);
	rawrdgeometry(d, &g);
	print("rpm:%d\n", decoderpm(g.rpm));
	print("secsize:%uld\n", g.secsize);
	print("physecsize:%uld\n", g.physecsize);
	print("formfactor:%q\n", decodeform(g.ff));
}

static void
geometry(Rawrd *d)
{
	int ret, rpm;
	Drivegeometry g;
	char rpmbuf[16], secbuf[16];

	memset(&g, 0, sizeof g);
	if (d->type != Tata && d->type != Tscsi) {
		fprint(2, "%-8s %12s\n", d->name, "missing");
		return;
	}
	ret = rawrdgeometry(d, &g);
	if (ret == 0)
		snprint(secbuf, sizeof secbuf, "%uld/%uld", g.secsize, g.physecsize);
	else
		snprint(secbuf, sizeof secbuf, "unknown");

	rpm = decoderpm(g.rpm);
	if (rpm < 0)
		snprint(rpmbuf, sizeof rpmbuf, "unknown");
	else
		snprint(rpmbuf, sizeof rpmbuf, "%d", rpm);

	print("%-8s %12s %18s %35s\n", d->name, rpmbuf, decodeform(g.ff), secbuf);
}

static int
hasrole(char *s)
{
	int lun;

	lun = dno(s);
	if (lun < 0 || drtab[lun] == nil)
		return 0;
	return 1;
}

static int
ssdlife(char *model, uchar *sbuf)
{
	int n;

	/*
	 * Look for the specific models we know about, then
	 * fall back to looking for the SMART params we know
	 * irrespective of model
	 */
	if (strstr(model, "INTEL")) {
		for (n = 2; n < 30 * 12 + 2; n += 12)
			if (sbuf[n] == 0xe9)
				return sbuf[n+3];
	}
	if (strstr(model, "D2RSTK")) {
		for (n = 2; n < 30 * 12 + 2; n += 12)
			if (sbuf[n] == 0xe7)
				return sbuf[n+3];
	}
	/*
	 * SmartStorage Optimus reports percent life used
	 * in parameter f5, rather than percent life remaining.
	 */
	if (strstr(model, "TXA2D")) {
		for (n = 4; n < 512; n += sbuf[n+3] + 4)
			if (sbuf[n+1] == 0xf5)
				return 100 - sbuf[n+11];
 	}
	for (n = 2; n < 30 * 12 + 2; n += 12)
		if (sbuf[n] == 0xe7)		/* sandforce/lsi */
			return sbuf[n+3];
	for (n = 2; n < 30 * 12 + 2; n += 12)
		if (sbuf[n] == 0xe9)		/* intel */
			return sbuf[n+3];
	for (n = 2; n < 30 * 12 + 2; n += 12)
		if (sbuf[n] == 0xd1)		/* idilinx */
			return sbuf[n+3];
	return -1;
}

void
disksmain(int argc, char **argv)
{
	int i, n, life, rv;
	Rawrd *d;
	Dattrs *a;
	char buf[64], *p;
	uchar sbuf[512];
	uchar temp[512];

	for (i=0; i<argc; i++) {
		p = parseshelfdotslot(argv[i], usage, nil, 0);
		if (p == nil) {
			if ((sflag && !rflag && !lflag) || xflag)
				errskip(argc-i-1, &argv[i+1]);
			else {
				print("error: %r\n");
				continue;
			}
		}
		d = atodisk(argv[i]);
		if (d == nil || (ppflag && d->type == Tunk))
			continue;
		a = d->aux;
		avlstat(d);
		if (cflag) {
			if (!nlines++)
				print("%-8s %7s %s\n", "DRIVE", "VERSION", "CONFIG");
			print("%-8s %7s %s\n", d->name, getversion(d), getconfig(d));
		} else if (fcflag) {
			n = ident(d);
			if (n == -1)
				continue;
			if (strncmp("cache", getrole(d->name), 5) == 0) {
				if (!nlines++)
					print("%-8s %13s %10s %6s %6s %10s %6s %6s\n", "DRIVE", "SIZE (GB)",
						"READ", "AVG", "MAX", "WRITE", "AVG", "MAX");
				fmtsz(d, buf, sizeof buf);
				print("%-8s %13s", d->name, buf);
				if (getstats(d) == 0) {
					print(" %10s", mbstr(buf, sizeof buf, a->rstats.ios));
					print(" %4uldms %4uldms", a->rstats.avglat, a->rstats.maxlat);
					print(" %10s", mbstr(buf, sizeof buf, a->wstats.ios));
					print(" %4uldms %4uldms\n", a->wstats.avglat, a->wstats.maxlat);
				} else
					print("\n");
			}
		} else if (sflag) {
			if (rflag || wflag) {
				if (nflag)
					seterc(d);
				else
					geterc(d);
			} else if (eflag || dflag) {
				n = smartenable(d, eflag ? 0 : 1);
				if (n < 0)
					print("%s: %s\n", d->name, d->estr);
			} else {
				if (!nlines++)
					print("%-8s %s\n", "DRIVE", "STATUS");
				print("%-8s %s\n", d->name, smartrs(d));
			}
		} else if (ssdhealth) {
			ident(d);
			if (a->ssd != 1)
				continue;
			if (!nlines++)
				print("%-8s %12s\n", "DRIVE", "SSD Health");
			if (smartrd(d, sbuf, sizeof sbuf) < 0)
				derrstate(d);
			else {
				life = ssdlife(a->model, sbuf);
				if (life == -1)
					print("%-8s  %12s\n", d->name, "unknown");
				else
					print("%-8s  %12d%%\n", d->name, life);
			}
		} else if (tflag) {
			rv = drivetemp(d, sbuf, sizeof sbuf, temp);
			if (!qflag && !nlines++)
				print("%-8s %12s\n", "DRIVE", "TEMP");
			if (rv < 0)
				print("%-8s %12s\n", d->name, d->estr);
			else
				print("%-8s %11dC\n", d->name, *temp);
		} else if (iflag) {
			rawinfo(d);
		} else if (jflag) {
			if (nlines++)
				print("\n");
			print("drive:%q\n", d->name);
			n = ident(d);
			print("state:%q\n", getav(((Dattrs*)a)->udata, "state"));
			if (n == -1)
				continue;
			print("version:%q\n", getversion(d));
			print("config:%q\n", getconfig(d));
			if (a->serial)
				print("serial:%q\n", a->serial);
			print("sectors:%ulld\n", a->sectors);
			print("fwver:%q\n", a->fwver);
			print("model:%q\n", a->model);
			print("role:%q\n", getrole(d->name));
			print("smart:%q\n", smartrs(d));
			print("wc:%q\n", wcable(d));
			print("spec:%q\n", spec(d));
			print("ssd:%s\n", a->ssd == 1 ? "True" : "False");
			if (a->ssd == 1) {
				life = -1;
				if (smartrd(d, sbuf, sizeof sbuf) == 0)
					life = ssdlife(a->model, sbuf);
				print("ssdhealth:%d\n", life);
			}
			jgeometry(d);
			rv = drivetemp(d, sbuf, sizeof sbuf, temp);
			print("temp:%d\n", rv < 0 ? -1 : *temp);
			print("errorlog:");
			smartrl(d);
		} else if (lflag) {
			n = ident(d);
			if (!nlines++)
				print("%-8s %26s %8s\n", "DRIVE", "MODEL", "FIRMWARE");
			if (n == -1)
				derrstate(d);
			else {
				print("%-8s %26s %8s\n", d->name, a->model, a->fwver);
				smartrl(d);
			}
		} else if (xflag) {
			if (!fflag && hasrole(d->name)) {
				werrstr("drive %s has role %s", d->name, getrole(d->name));
				errskip(argc-i-1, &argv[i+1]);
			}
			if (secerase)
				secureerase(d);
			else if (secunlock)
				secureunlock(d);
			else if (secdisable)
				securedisable(d);
			else if (flcache)
				flushcache(d);
		} else if (gflag) {
			if (!nlines++)
				print("%-8s %12s %18s %35s\n",
					"DRIVE", "RPM", "FORMFACTOR", "SECTORSIZE(logical/physical)");
			geometry(d);
		} else {
			n = ident(d);
			if (!nlines++)
				print("%-8s %12s %8s %26s %8s %12s\n", "DRIVE", "SIZE (GB)", "ROLE", "MODEL", "FIRMWARE", "MODE");
			if (n == -1)
				derrstate(d);
			else {
				fmtsz(d, buf, sizeof buf);
				print("%-8s %12s %8s %26s %8s %12s\n", d->name, buf, getrole(d->name),
					a->model, a->fwver, mode(a));
				if (aflag)
					addlinfo(d);
			}
		}
	}
}

#define Enodrive "no drives found"
#define Ealloc "allocation failure"

static void
probe(void)
{
	int i;

	ndisks = rawrdprobe(getshelf(), disks, nelem(disks));
	if (ndisks <= 0) {
		fprint(2, "%s\n", Enodrive);
		exits(Enodrive);
	}
	for (i = 0; i < ndisks; i++) {
		disks[i].aux = mallocz(sizeof (Dattrs), 1);
		if (disks[i].aux == nil) {
			fprint(2, "%s\n", Ealloc);
			exits(Ealloc);
		}
	}
}

void
main(int argc, char **argv)
{
	int narg;
	char *arg[Maxdisk], *p;

	quotefmtinstall();
	ARGBEGIN {
	case 'a':
		aflag++;
		break;
	case 'c':
		cflag++;
	case 'C':
		fcflag++;
	case 'e':
		eflag++;
		break;
	case 'd':
		dflag++;
		break;
	case 'f':
		fflag++;
		break;
	case 'g':
		gflag++;
		break;
	case 'i':
		iflag++;
		break;
	case 'j':
		jflag++;
		break;
	case 'l':
		lflag++;
		break;
	case 'n':
		nflag++;
		nval = strtol(EARGF(usage()), 0, 0);
		break;
	case 'p':
		ppflag++;
		break;
	case 'q':
		qflag++;
		break;
	case 'r':
		rflag++;
		break;
	case 's':
		sflag++;
		break;
	case 'S':
		ssdhealth++;
		break;
	case 't':
		tflag++;
		break;
	case 'w':
		wflag++;
		break;
	case 'x':
		p = ARGF();
		if (p == nil)
			usage();
		xflag++;
		if (strcmp(p, "erase") == 0)
			secerase++;
		else if (strcmp(p, "unlock") == 0)
			secunlock++;
		else if (strcmp(p, "disable") == 0)
			secdisable++;
		else if (strcmp(p, "flush") == 0)
			flcache++;
		else {
			fprint(2, "unknown -x command: %s\n", p);
			usage();
		}
		break;
	default:
		usage();
	} ARGEND
	shelf = getshelf();
	initdrtab();
	probe();
	if (argc == 0) {
		narg = alldisks(arg, nelem(arg));
		disksmain(narg, arg);
	} else
		disksmain(argc, argv);
	exits(0);
}
