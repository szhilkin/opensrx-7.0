// Copyright © 2010 Coraid, Inc.
// All rights reserved.
// restore.c: read raiddev configurations and emit commands to
//	create the raid sets
#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <ip.h>
#include "srxcmds.h"

#define dprint(...) if (!dflag) USED(dflag); else fprint(2, __VA_ARGS__)
#define DIRNAME "/raiddev"

enum {
	Nconflen	= 1024,
	Nmagic		= 8,
};

typedef struct Spare Spare;
typedef struct Cache Cache;
typedef struct Mask Mask;
typedef struct Device Device;
typedef struct Online Online;
typedef struct Srconfig Srconfig;

struct Spare {
	Spare	*next;
	char		*dev;
};

struct Cache {
	Cache	*next;
	char		*dev;
};

struct Mask {
	Mask	*next;
	int		slot;
	ulong	ts;
	char		*mask;
};

struct Device {
	Device	*next;
	Device	*prev;
	char		*dev;
	char		*raidtype;
	char		*version;
	int		ndevs;
	int		l, p, d;		// logical, part, device
	ulong	ts;			// timestamp
	char		*state;
	int		nrow;		// parity build row
};

struct Online {
	Online	*next;
	int		slot;
};

struct Srconfig {
	char	magic[Nmagic];
	uchar	length[4];
	uchar	sectoralign[512 -8 -4];
	char	config[Nconflen];
};

int dflag;		// force
Spare *spares;
Cache *caches;
Device *devices;
Mask *masks;
char	 *dirname = DIRNAME;
Online *onlines;
static int shelf, userenshelf;
int rshelf = -1;
int rslot = -1;
int newslot = -1;

Srconfig *
readconfig(char *name)
{
	char buf[64+1];
	int fd, n;
	Srconfig *s;

	if (parsedrive(name, 1, nil) == 0)
		return nil;
	snprint(buf, sizeof buf, "/raiddev/%s/data", name);
	fd = open(buf, OREAD);
	if (fd < 0) {
		dprint("readconfig: open(%s, OREAD) failure: %r\n", buf);
		return nil;
	}
	s = malloc(sizeof *s);
	if (s == nil)
		sysfatal("unable to malloc for config");
	n = read(fd, s, sizeof *s);
	close(fd);
	if (n != sizeof *s || memcmp(s->magic, "coraid01", Nmagic) != 0 &&
	    memcmp(s->magic, "coraid00", Nmagic) != 0) {
		dprint("readconfig: (%s) read failure: %r\n", name);
		free(s);
		s = nil;
	}
	return s;
}

char *
getconfig(char *name)
{
	Srconfig *s;
	char *p;

	s = readconfig(name);
	if (s == nil)
		return "";
	p = malloc(Nconflen+1);
	if (p == nil)
		sysfatal("unable to malloc for config string");
	snprint(p, nhgetl(s->length), "%s", s->config);
	free(s);
	return p;
}

char *
getversion(char *name)
{
	Srconfig *s;
	char *p;

	s = readconfig(name);
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

void
noteonline(int slot)
{
	Online *p;

	p = mallocz(sizeof *p, 1);
	p->slot = slot;
	p->next = onlines;
	onlines = p;
}

void
emitonlines(void)
{
	Online *p;
	
	for (p = onlines; p; p = p->next)
		print("online %d\n", p->slot);
}

int
getcfstr(char *name, char **f, int nf)
{
	int n;
	char *cfg;

	cfg = getconfig(name);
	if (cfg == nil)
		return 0;
	n = tokenize(cfg, f, nf);
	return n;	// memory leaked intentionally
}

void
showstr(int nf, char **f)
{
	while (nf-- > 0) {
		print("%s%c", *f, nf == 0 ? '\n' : ' ');
		f++;
	}
}

void
addspare(char *, int, char **f)	// add new spare device
{
	Spare *p;

	p = mallocz(sizeof *p, 1);
	p->next = spares;
	spares = p;
	p->dev = strdup(f[0]);
}

void
addcache(char *, int, char **f)	// add new cache device
{
	Cache *p;

	p = mallocz(sizeof *p, 1);
	p->next = caches;
	caches = p;
	p->dev = strdup(f[0]);
}

void
emitspares(void)	// write out the spare commands
{
	Spare *p;

	for (p = spares; p; p = p->next)
		print("mkspare %s\n", p->dev);
}

void
emitfcconfig(void)
{
	Cache *p;
	char *q, *r, *e;

	if (caches == nil)
		return;
	q = malloc(8192);
	e = q + 8192;
	r = seprint(q, e, "fcadd -r");
	for (p = caches; p; p = p->next)
		r = seprint(r, e, " %s", p->dev);
	print("%s\n", q);
	free(q);
}

void
setlpd(Device *p, char *lpd)
{
	char *cp;

	cp = strchr(lpd, '.');
	if (cp == nil)
		return;
	*cp++ = 0;
	p->l = atoi(lpd);
	lpd = cp;
	cp = strchr(lpd, '.');
	if (cp == nil)
		return;
	*cp++ = 0;
	p->p = atoi(lpd);
	p->d = atoi(cp);
}

void
addmasks(int nf, char **f, int slot)	// add masks to the list
{
	Mask *p, **q, *np;
	char *mask;

next:
	while (nf-- > 0) {
		mask = *f++;
		for (p = masks; p; p = p->next)
			if (p->slot == slot)
			if (strcmp(p->mask, mask) == 0)
				goto next;
		np = mallocz(sizeof *np, 1);
		np->slot = slot;
		np->mask = strdup(mask);
		q = &masks;
		for (p = masks; p; p = p->next) {
			if (slot == p->slot)
				break;
			q = &p->next;
		}
		np->next = *q;
		*q = np;
	}
}

void
printmasks(void)	// dump out the list of masks
{
	Mask *p;

	for (p = masks; p; p = p->next)
		print(" %3d %s\n", p->slot, p->mask);
}

void
rmmasks(int slot, ulong ts)	// kill masks for this slot
{
	Mask *p, **q;

	q = &masks;
	for (p = masks; p; p = p->next) {
		if (p->slot == slot && p->ts < ts) {
			*q = p->next;	// don't bother freeing struct
			continue;
		}
		q = &p->next;
	}
}

void
emitmasks(void)	// output the mask commands
{
	Mask *p;
	int oldslot;

	p = masks;
	if (p == nil)
		return;
	oldslot = p->slot;
	while (p) {
		print("mask +%s", p->mask);
		for (;;) {
			p = p->next;
			if (p == nil) {
				print(" %d\n", newslot >= 0 ? newslot : oldslot);
				break;
			}
			if (p->slot != oldslot) {
				print(" %d\n", newslot >= 0 ? newslot : oldslot);
				oldslot = p->slot;
				break;
			}
			print(" +%s", p->mask);
		}
		
	}
}

Device *
newdev(char *name, char **f)
{
	Device *p;

	p = mallocz(sizeof *p, 1);
	p->dev = strdup(f[0]);
	p->raidtype = strdup(f[4]);
	p->ndevs = atoi(f[3]);
	setlpd(p, f[2]);
	p->ts = atol(f[7]);
	p->state = strdup(f[5]);
	p->nrow = atoi(f[6]);
	p->version = getversion(name);
	return p;
}

void
printdev(Device *p)
{
	fprint(2, "dev %6s, version %s, raidtype %6s,", p->dev, p->version, p->raidtype);
	fprint(2, " ndevs %3d, %2d %2d %2d ,", p->ndevs, p->l, p->p, p->d);
	fprint(2, " ts %uld,", p->ts);
//	fprint(2, " nrow %8d,", p->nrow);
	fprint(2, " state %s\n", p->state);
}

void
printdevices(void)
{
	Device *p;

	for (p = devices; p; p = p->next)
		printdev(p);
}

int
cmpare(Device *q, Device *p)	// <0 if less, ==0 if eq, >0 if greater
{
	if (q->l < p->l)
		return -1;
	if (q->l > p->l)
		return 1;
	if (q->p < p->p)
		return -1;
	if (q->p > p->p)
		return 1;
	if (q->d < p->d)
		return -1;
	if (q->d > p->d)
		return 1;
	return 0;
}

Device *
finddev(Device *q)	// lookup in our list of devices
{
	Device *p;

	for (p = devices; p; p = p->next)
		if (cmpare(q, p) == 0)
			return p;
	return nil;
}

void
replace(Device *op, Device *np)	// replace old with new
{
	np->next = op->next;
	np->prev = op->prev;
	if (np->next)
		np->next->prev = np;
	if (np->prev)
		np->prev->next = np;
	else
		devices = np;
	free(op);
}

void
insert(Device *p)	// put this device in the list
{
	Device *q;

	q = devices;
	if (q == nil) {
		devices = p;
		return;
	}
	while (cmpare(p, q) >= 0) {	// find insertion point
		if (q->next == nil) {		// or add to tail
			q->next = p;
			p->prev = q;
			return;
		}
		q = q->next;
	}
	p->next = q;
	p->prev = q->prev;
	q->prev = p;
	if (p->prev == nil)
		devices = p;
	else
		p->prev->next = p;
}

void
adddevice(char *name, int nf, char **f)
{
	Device *dp, *ndp;

	ndp = newdev(name, f);
	if (strcmp(ndp->state, "defunct") == 0) {
		free(ndp);
		return;
	}
	dp = finddev(ndp);
	if (dp) {
		if (ndp->ts > dp->ts) {
			rmmasks(dp->l, dp->ts);
			replace(dp, ndp);
		} else {
			free(ndp);
			return;
		}
	} else
		insert(ndp);
	if (nf > 8) 
		addmasks(nf - 8, &f[8], ndp->l);
}


Device *
appendmissing(Device *p, int l, int part, int d, char *raidtype)
{
	Device *np;

	np = mallocz(sizeof *np, 1);
	np->dev = "missing";
	np->raidtype = strdup(raidtype);
	np->l = l;
	np->p = part;
	np->d = d;
	np->ndevs = p->ndevs;
	np->state = "normal";
	np->version = p->version;
	np->next = p->next;
	np->prev = p;
	p->next = np;
	if (np->next)
		np->next->prev = np;
	return np;
}

// remove span of devices inclusive
void
rmspan(Device *start, Device *p)	// careful not to bother p's next pointer
{
	if (start->prev)
		start->prev->next = p->next;
	else
		devices = p->next;
	if (p->next)
		p->next->prev = start->prev;
}

Device *
killlblade(int slot)	// kill all the nodes with slot; will be contiguious
{
	Device *p, *q;

	p = devices;
	while (p && p->l != slot)
		p = p->next;
	if (p == nil)
		return nil;
	q = p;
	while (q->next && q->next->l == slot)
		q = q->next;
	rmspan(p, q);
	rmmasks(slot, ~0);
	return q->next;
}

void
insertmissing(Device *p, int slot, int part, int dev, char *raidtype)
{
	Device *np;

	np = mallocz(sizeof *np, 1);
	np->dev = "missing";
	np->raidtype = strdup(raidtype);
	np->l = slot;
	np->p = part;
	np->d = dev;
	np->ndevs = p->ndevs;
	np->state = "normal";
	np->version = p->version;
	np->next = p;
	np->prev = p->prev;
	p->prev = np;
	if (np->prev)
		np->prev->next = np;
	else
		devices = np;
}

/* clean up spares, dev, mask lists for emit based on command line input. */
void
paredevs(void)
{
	Device *d, *nd;
	Mask *m, **mm;
	int rluns;

	if (rslot < 0)
		return;

	/* if any slot specifier, ditch the spare devices. Lazy leaky algorithm. */
	spares = nil;

	/* Similarly for caches */
	caches = nil;

	rluns = 0;
	for (nd=devices; d=nd; ) {
		nd = d->next;
		if (d->l != rslot) {
			rmspan(d, d);
			continue;
		}
		rluns++;
		if (newslot >= 0)
			d->l = newslot;
	}
	if (rluns == 0)
		fprint(2, "warning: no luns to restore with lun id %d\n", rslot);
	/* ditch masks not for rslot.  again, Lazy leaky algorithm */
	for (mm=&masks; m=*mm;) {
		if (m->slot != rslot)
			*mm = m->next;
		else
			mm = &m->next;
	}
}

/* go through list ditching devices in parts with inconsistent timestamps. */
void
tscheck(void)
{
	Device *p, *np;

	if (devices == nil)
		return;
	p = devices;
loop:
	np = p->next;
	if (np == nil)
		return;
	if (p->l == np->l && p->p == np->p) {
		if (p->ts > np->ts) {
			rmspan(np, np);
			goto loop;
		}
		if (p->ts < np->ts)
			rmspan(p, p);
	}
	p = np;
	goto loop;
}

void
mkmissing(void)		//  insert missing slugs where required
{
	Device *p, *q;
	char *rtype;
	int ol, op, od, ndevs, i;

	p = devices;
A:
	if (p == nil)
		return;
	ol = p->l;
	if (p->p != 0) {
		p = killlblade(ol);
		goto A;
	}
B:
	op = p->p;
	ndevs = p->ndevs;
	rtype = p->raidtype;
	for (i = 0; i < p->d; i++)
		insertmissing(p, ol, op, i, rtype);
	for (;;) {
		od = p->d;
		q = p;
		p = p->next;
		if (p == nil || p->p != op || p->l != ol)
			for (i = od+1; i < ndevs; i++)
				q = appendmissing(q, ol, op, i, rtype);
		if (p == nil)
			return;
		if (p->l != ol)
			goto A;
		if (p->d >= ndevs) {
			rmspan(p, p);
			continue;
		}
		if (p->p == op+1)
			goto B;
		if (p->p != op) {
			p = killlblade(ol);
			goto A;
		}
		for (i = od+1; i < p->d; i++)
			insertmissing(p, ol, op, i, rtype);
	}
}

char *
suffix(Device *p)
{
	if (strcmp(p->state, "clean") == 0)
		return ":c";
	else if (strcmp(p->state, "failed") == 0)
		return ":f";
	else if (strcmp(p->state, "replaced") == 0)
		return ":r";
	else if (strcmp(p->state, "normal") == 0 ||
		 strcmp(p->state, "needinit") == 0)
		return "";
	else
		return ":x";
}

void
emitmake(void)	// writeout make and grow commands
{
	Device *p;
	int l, part;
	char buf[8];

	p = devices;
	if (p == nil) {
		if (userenshelf)
			fprint(2, "warning: no luns to restore for shelf %d\n", rshelf);
		return;
	}
	while (p) {
		sprint(buf, "%d", p->l);
		if (islun(buf)) {
			p = p->next;
			continue;
		}
		print("mklun -r -I -V %s %d %s %s%s ", p->version, p->l, p->raidtype, p->dev, suffix(p));
		noteonline(p->l);
		l = p->l;
		do {
			part = p->p;
			for (p = p->next; p && p->p == part && p->l == l; p = p->next)
				print("%s%s ", p->dev, suffix(p));
			print("\n");
			if (p && p->l == l)
				print("grow %d %s %s%s ", p->l, p->raidtype, p->dev, suffix(p));
		} while (p && p->l == l);
	}
}		

void
ctlprint(char *fname, char *fmt, ...)
{
	char buf[2048], *out;
	va_list arg;
	int fd;

	fd = open(fname, OWRITE);
	if (fd == -1) {
		dprint("ctlprint: open: %r\n");
		return;
	}
	va_start(arg, fmt);
	out = vseprint(buf, buf+sizeof(buf), fmt, arg);
	write(fd, buf, out-buf);
	close(fd);
}

void
emit(void)		// write out all the commands
{
	emitspares();
	emitfcconfig();
	emitmake();
	emitmasks();
	emitonlines();
}

void
getdevs(void)
{
	int fd, nf, n;
	Dir *dp;
	enum { MAXF= 30, };
	char *f[MAXF];
	char name[64];

	fd = open(dirname, OREAD);
	if (fd < 0)
		sysfatal("open: %r");
	n = dirreadall(fd, &dp);
	if (n < 0)
		sysfatal("dirreadall error: %r");
	for (; n-- > 0; dp++) {
		if (!isdigit(dp->name[0]))
			continue;
		snprint(name, sizeof name, "%d.%s", shelf, dp->name);
		f[0] = strdup(name);
		nf = getcfstr(dp->name, f+1, nelem(f)-1);
		if (nf < 2)
			continue;
		nf++;	// for f[0]
		if (atoi(f[1]) != rshelf)
			continue;
		if (strcmp(f[2], "spare") == 0)
			addspare(dp->name, nf, f);
		else if (strcmp(f[2], "cache") == 0)
	/* FC is removed from 7.0.0  enable below when it is suported again. */
//			addcache(dp->name, nf, f);
			continue;
		else
			adddevice(dp->name, nf, f);
	}
	if (dflag)
		printdevices();
}

void
usage(void)
{
	fprint(2, "usage: rrestore [ oldshelfno [ oldlun [ newlun ] ] ]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *e;
	int badrange = 0;

	ARGBEGIN {
	default:
		usage();
	case 'D':	// dirname
		dirname = EARGF(usage());
		break;
	case 'd':	// debug
		dflag = 1;
		break;
	} ARGEND

	shelf = getshelf();
	switch (argc) {
	case 3:
		newslot = strtol(argv[2], &e, 10);
		if (*e != '\0' || isvalidlunrange(newslot) == 0) {
			fprint(2, "lun %d is not within valid range(0 <= 254)\n", newslot);
			badrange = 1;
		}
	case 2:
		rslot = strtol(argv[1], &e, 10);
		if (*e != '\0' || isvalidlunrange(rslot) == 0) {
			fprint(2, "lun %d is not within valid range(0 <= 254)\n", rslot);
			badrange = 1;
		}
	case 1:
		rshelf = strtol(argv[0], &e, 10);
		if (*e != '\0' || isvalidshelfrange(rshelf) == 0) {
			fprint(2, "shelf %d is not within valid range(0 <= 65534)\n", rshelf);
			badrange = 1;
		}
		userenshelf = 1;
		break;
	case 0:
		rshelf = shelf;
		break;
	default:
		usage();
	}
	if (badrange)
		exits("badrange");
	getdevs();
	tscheck();
	mkmissing();
	paredevs();
	emit();
	exits(nil);
}
