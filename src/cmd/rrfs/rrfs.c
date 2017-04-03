/*
 * Copyright © 2011 Coraid, Inc.
 * All rights reserved.
 */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <auth.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "list.h"

#pragma varargck type "K" uchar *
#pragma varargck type "M" uchar *

#define	QTOL(q)		((int)(q).path>>8 & 0xff)
#define	QTOT(q)		((int)(q).path & 0xff)
#define	PATH(l, t)	(~0<<16|(l)<<8|(t))
#define	QID(l, t, q)	((Qid){PATH(l, t), 0, (q)})

enum {
	Nluns	= 255,
	Nkey	= 8,
	Nmac	= 6,

	Maxname	= 3,	/* 0-999 */

	/* Qids */
	Qlun	= 0xff,
	Qctl	= 0,
	Qmacs,
	Qstat,
	Qkeys,

	/* RTypes */
	Rrwro	= 0,
	Rwro,
	Rrwso,
	Rwso,
	Rrwgo,
	Rwgo,
};

typedef struct Mac Mac;
typedef struct Key Key;
typedef struct Lun Lun;

struct Mac {
	uchar	mac[Nmac];
	Node	node;
};

struct Key {
	uchar	key[Nkey];
	List	macs;
	Node	node;
};

struct Lun {
	int	lun;
	char	name[Maxname+1];
	List	keys;
	Key	*owner;
	int	rtype;
	ulong	gencnt;
	Node	node;
	int	persist;
};

static char *file0;
static char *file1;
static int debug;
static int post;
static List luns;

static uchar nilkey[] = "\0\0\0\0\0\0\0\0";
static uchar Konekey[] = "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF";
static Qid root = {0, 0, QTDIR};

static char Ebadcmd[] = "bad cmd";
static char Ebadlun[] = "bad lun";
static char Ebadkey[] = "bad key";
static char Ebadmac[] = "bad mac";
static char Ebadrtype[] = "bad rtype";
static char Enotexist[] = "file does not exist";
static char Eperm[] = "permission denied";
static char Ertype[] = "rtype conflict";

char	*basename(char *);
int	convdec(char *);
uchar	*convkey(char *);
uchar	*convmac(char *);
int	convrtype(char *);
Lun	*createlun(int);
Lun	*findlun(int);
Lun	*nextlun(int);
Lun	*plumblun(char *);
void	prunelun(Lun *);
void	resetlun(Lun *);
Key	*createkey(uchar *);
Key	*findkey(Lun *, uchar *);
Key	*plumbkey(Lun *, uchar *);
void	resetkey(Key *);
Mac	*createmac(uchar *);
Mac	*plumbmac(Lun *, uchar *);
Mac	*prunemac(Lun *, uchar *);
void	incgen(Lun *);
int	groupowner(int);
int	singleowner(int);
int	reserved(Lun *);
int	wexcl(int);
void	dump(void);
void	restore(void);
void	dprint(char *, ...);

char *
basename(char *s)
{
	char *t;

	t = strrchr(s, '/');
	if (t == nil)
		return s;
	return t+1;
}

int
convdec(char *s)
{
	char *e;
	ulong l;

	l = strtoul(s, &e, 10);
	if (*e != '\0')
		return -1;
	return l;
}

uchar *
convkey(char *s)
{
	int len;
	uchar *p;

	len = strlen(s);
	if (len != Nkey*2)
		return nil;
	p = (uchar *)s;
	if (dec16(p, Nkey, s, len) != Nkey)
		return nil;
	return p;
}

uchar *
convmac(char *s)
{
	int len;
	uchar *p;

	len = strlen(s);
	if (len != Nmac*2)
		return nil;
	p = (uchar *)s;
	if (dec16(p, Nmac, s, len) != Nmac)
		return nil;
	return p;
}

int
convrtype(char *s)
{
	int rtype;

	rtype = convdec(s);
	switch (rtype) {
	case Rrwro:
	case Rwro:
	case Rrwso:
	case Rwso:
	case Rrwgo:
	case Rwgo:
		return rtype;
	default:
		return -1;
	}
}

Lun *
createlun(int lun)
{
	Lun *lp;

	lp = emalloc9p(sizeof *lp);
	memset(lp, 0, sizeof *lp);
	lp->lun = lun;
	lp->persist = 1; /* by default, reservations persist */
	sprint(lp->name, "%d", lun);
	listinit(&lp->keys, Key, node);
	return lp;
}

Lun *
findlun(int lun)
{
	Lun *lp;

	listforeach(&luns, lp) {
		if (lp->lun == lun)
			return lp;
	} listdone;
	return nil;
}

Lun *
nextlun(int n)
{
	int i;
	Lun *lp;

	i = 0;
	listforeach(&luns, lp) {
		if (i++ == n)
			return lp;
	} listdone;
	return nil;
}

Lun *
plumblun(char *s)
{
	int lun;
	Lun *lp, *p, *q;

	lun = convdec(s);
	if (lun < 0 || lun >= Nluns)
		return nil;
	lp = findlun(lun);
	if (lp == nil) {
		lp = createlun(lun);
		q = nil;
		listforeach(&luns, p) {
			if (p->lun > lun) {
				q = p;
				break;
			}
		} listdone;
		listinsert(&luns, lp, q);
	}
	return lp;
}

void
prunelun(Lun *lp)
{
	Key *kp;

	listforeach(&lp->keys, kp) {
		if (!listempty(&kp->macs))
			continue;
		if (singleowner(lp->rtype))
		if (kp == lp->owner) {
			lp->owner = nil;
			lp->rtype = 0;
		}
		listremove(&lp->keys, kp);
		free(kp);
	} listdone;	
}

void
resetlun(Lun *lp)
{
	Key *kp;

	listforeach(&lp->keys, kp) {
		resetkey(kp);
		listremove(&lp->keys, kp);
		free(kp);
	} listdone;
	lp->owner = nil;
	lp->rtype = 0;
	return;
}

Key *
createkey(uchar *key)
{
	Key *kp;

	kp = emalloc9p(sizeof *kp);
	memset(kp, 0, sizeof *kp);
	memcpy(kp->key, key, Nkey);
	listinit(&kp->macs, Key, node);
	return kp;
}

Key *
findkey(Lun *lp, uchar *key)
{
	Key *kp;

	listforeach(&lp->keys, kp) {
		if (memcmp(kp->key, key, Nkey) == 0)
			return kp;
	} listdone;
	return nil;
}

Key *
plumbkey(Lun *lp, uchar *key)
{
	Key *kp;

	kp = findkey(lp, key);
	if (kp == nil) {
		kp = createkey(key);
		listadd(&lp->keys, kp);
	}
	return kp;
}

void
resetkey(Key *kp)
{
	Mac *mp;
	
	listforeach(&kp->macs, mp) {
		listremove(&kp->macs, mp);
		free(mp);
	} listdone;
}

Mac *
createmac(uchar *mac)
{
	Mac *mp;

	mp = emalloc9p(sizeof *mp);
	memset(mp, 0, sizeof *mp);
	memcpy(mp->mac, mac, Nmac);
	return mp;
}

Mac *
plumbmac(Lun *lp, uchar *mac)
{
	Mac *mp;

	mp = prunemac(lp, mac);
	if (mp == nil)
		mp = createmac(mac);
	return mp;
}

Mac *
prunemac(Lun *lp, uchar *mac)
{
	Key *kp;
	Mac *mp;

	listforeach(&lp->keys, kp) {
		listforeach(&kp->macs, mp) {
			if (memcmp(mp->mac, mac, Nmac) == 0) {
				listremove(&kp->macs, mp);
				prunelun(lp);
				return mp;
			}
		} listdone;
	} listdone;
	return nil;
}

void
incgen(Lun *lp)
{
	if (!post)
		return;
	lp->gencnt++;
}

int
groupowner(int rtype)
{
	switch (rtype) {
	case Rrwgo:
	case Rwgo:
	case Rrwso:
	case Rwso:
		return 1;
	default:
		return 0;
	}
}

int
singleowner(int rtype)
{
	switch (rtype) {
	case Rrwro:
	case Rwro:
	case Rrwso:
	case Rwso:
		return 1;
	default:
		return 0;
	}
}

int
reserved(Lun *lp)
{
	if (singleowner(lp->rtype))
		return lp->owner != nil;
	else
		return !listempty(&lp->keys);
}

int
wexcl(int rtype)
{
	switch(rtype) {
	case Rwro:
	case Rwso:
	case Rwgo:
		return 1;
	default:
		return 0;
	}
}

int
cregister(Lun *lp, uchar *key, char **f, int nf)
{
	uchar *mac;
	Key *kp;
	Mac *mp;
	int i;

	if (key == nil) {
		werrstr(Ebadkey);
		return -1;
	}
	if (memcmp(key, nilkey, Nkey) == 0) {
		for (i = 0; i < nf; ++i) {
			mac = convmac(f[i]);
			if (mac == nil) {
				werrstr(Ebadmac);
				return -1;
			}
			mp = prunemac(lp, mac);
			if (mp == nil)
				continue;
			free(mp);
		}
	} else {
		for (i = 0; i < nf; ++i) {
			mac = convmac(f[i]);
			if (mac == nil) {
				werrstr(Ebadmac);
				return -1;
			}
			mp = plumbmac(lp, mac);
			kp = plumbkey(lp, key);
			listadd(&kp->macs, mp);
		}
	}
	incgen(lp);
	return 0;
}

int
reserve(Lun *lp, int rtype, uchar *key)
{
	Key *kp;

	if (rtype == -1) {
		werrstr(Ebadrtype);
		return -1;
	}
	if (key == nil) {
		werrstr(Ebadkey);
		return -1;
	}
	if (memcmp(key, nilkey, Nkey) == 0) {
		if (!reserved(lp))
			return 0;
		if (singleowner(lp->rtype))
			lp->owner = nil;
		lp->rtype = 0;
	} else {
		kp = findkey(lp, key);
		if (kp == nil) {
			werrstr(Ebadkey);
			return -1;
		}
		if (!reserved(lp)) {
			if (singleowner(rtype))
				lp->owner = kp;
			lp->rtype = rtype;
			return 0;
		}
		if (rtype != lp->rtype) {
			werrstr(Ertype);
			return -1;
		}
		if (singleowner(lp->rtype))
		if (kp != lp->owner) {
			werrstr(Ebadkey);
			return -1;
		}
	}
	return 0;
}

int
replace(Lun *lp, int rtype, uchar *tkey, uchar *rkey)
{
	Key *tkp, *rkp;

	if (rtype == -1) {
		werrstr(Ebadrtype);
		return -1;
	}
	if (tkey == nil) {
		werrstr(Ebadkey);
		return -1;
	}
	if (rkey == nil) {
		werrstr(Ebadkey);
		return -1;
	}
	tkp = findkey(lp, tkey);
	if (tkp == nil) {
		werrstr(Ebadkey);
		return -1;
	}
	if (memcmp(tkey, rkey, Nkey) == 0) /* check for no-op */
		return 0;
	rkp = findkey(lp, rkey);
	if (rkp != nil) {
		werrstr(Ebadkey);
		return -1;
	}
	memmove(tkp->key, rkey, Nkey);
	incgen(lp);
	return 0;
}

int
preempt(Lun *lp, int rtype, uchar *tkey, uchar *rkey)
{
	Key *tkp, *rkp, *kp;

	if (rtype == -1) {
		werrstr(Ebadrtype);
		return -1;
	}
	if (tkey == nil) {
		werrstr(Ebadkey);
		return -1;
	}
	if (rkey == nil) {
		werrstr(Ebadkey);
		return -1;
	}
	rkp = findkey(lp, rkey);
	if (rkp == nil) {
		werrstr(Ebadkey);
		return -1;
	}
	if (singleowner(lp->rtype)) {
		tkp = findkey(lp, tkey);
		if (tkp == nil) {
			werrstr(Ebadkey);
			return -1;
		}
		if (tkp == lp->owner) {
			if (singleowner(rtype))
				lp->owner = rkp;
			else
				lp->owner = nil;
			lp->rtype = rtype;
		}
		resetkey(tkp);
	} else {
		if (memcmp(tkey, nilkey, Nkey) == 0) {
			listforeach(&lp->keys, kp) {
				if (kp == rkp)
					continue;
				resetkey(kp);
			} listdone;
			if (singleowner(rtype))
				lp->owner = rkp;
			lp->rtype = rtype;
		} else {
			tkp = findkey(lp, tkey);
			if (tkp == nil) {
				werrstr(Ebadkey);
				return -1;
			}
			resetkey(tkp);
		}
	}
	prunelun(lp);
	incgen(lp);
	return 0;
}

int
reset(Lun *lp)
{
	resetlun(lp);
	incgen(lp);
	return 0;
}

enum {
	Register,
	Reserve,
	Replace,
	Reset,
	Preempt,
};

Cmdtab ctltab[] = {
	{ Register,	"register",	0 },
	{ Reserve,	"reserve",	3 },
	{ Replace,	"replace",	4 },
	{ Reset,	"reset",	1 },
	{ Preempt,	"preempt",	4 },
};

int
readmacs(Lun *lp, char *buf, int len)
{
	char *p, *e;
	Key *kp;
	Mac *mp;
	
	dprint("readmacs %d\n", lp->lun);
	if (!reserved(lp))
		return 0;
	p = buf;
	e = buf + len;
	p = seprint(p, e, "%d", wexcl(lp->rtype));
	if (groupowner(lp->rtype)) {
		listforeach(&lp->keys, kp) {
			listforeach(&kp->macs, mp) {
				p = seprint(p, e, " %M", mp->mac);
			} listdone;
		} listdone;
	} else {
		kp = lp->owner;
		listforeach(&kp->macs, mp) {
			p = seprint(p, e, " %M", mp->mac);
		} listdone;
	}
	p = seprint(p, e, "\n");
	return p-buf;
}

int
readstat(Lun *lp, char *buf, int len)
{
	char *p, *e;
	Key *kp;

	dprint("readstat %d\n", lp->lun);
	p = buf;
	e = buf + len;
	p = seprint(p, e, "%d %uld", lp->rtype, lp->gencnt);
	if (reserved(lp) && singleowner(lp->rtype))
		p = seprint(p, e, " %K", lp->owner->key);
	else
		p = seprint(p, e, " %K", nilkey);
	listforeach(&lp->keys, kp) {
		p = seprint(p, e, " %K", kp->key);
	} listdone;
	p = seprint(p, e, "\n");
	return p-buf;
}

int
readkeys(Lun *lp, char *buf, int len)
{
	char *p, *e;
	Key *kp;
	Mac *mp;

	p = buf;
	e = buf+len;
	listforeach(&lp->keys, kp) {
		p = seprint(p, e, "%K", kp->key);
		listforeach(&kp->macs, mp) {
			p = seprint(p, e, " %M", mp->mac);
		} listdone;
		p = seprint(p, e, "\n");
	} listdone;
	return p-buf;
}

int
ispersistent(uchar *key)
{
	return (memcmp(key, Konekey, Nkey) != 0);
}

int
writectl(Lun *lp, char *buf, int len)
{
	Cmdbuf *cb;
	Cmdtab *tab;
	int rv;
	uchar *key;

	dprint("writectl %d '%.*s'\n", lp->lun, utfnlen(buf, len), buf);
	cb = parsecmd(buf, len);
	if (cb == nil) {
		werrstr(Ebadcmd);
		return 0;
	}
	tab = lookupcmd(cb, ctltab, nelem(ctltab));
	if (tab == nil) {
		werrstr(Ebadcmd);
		free(cb);
		return 0;
	}
	rv = -1;
	switch (tab->index) {
	case Register:
		if (cb->nf < 3) {
			werrstr(Ebadcmd);
			break;
		}
		key = convkey(cb->f[1]);
		if ((lp->persist = ispersistent(key)) == 0)
		if (!post) /* skip initialization of non-persistent reservations */
			break;
		rv = cregister(lp, key, &cb->f[2], cb->nf-2);
		break;
	case Reserve:
		key = convkey(cb->f[2]);
		if ((lp->persist = ispersistent(key)) == 0)
		if (!post) /* skip initialization of non-persistent reservations */
			break;
		rv = reserve(lp, convrtype(cb->f[1]), key);
		break;
	case Preempt:
		rv = preempt(lp, convrtype(cb->f[1]), convkey(cb->f[2]), convkey(cb->f[3]));
		break;
	case Replace:
		rv = replace(lp, convrtype(cb->f[1]), convkey(cb->f[2]), convkey(cb->f[3]));
		break;
	case Reset:
		rv = reset(lp);
		break;
	}
	free(cb);
	if (rv != 0)
		return 0;
	if (post && lp->persist == 1)
		dump();
	return len;
}

typedef int Read(Lun *, char *, int);
typedef int Write(Lun *, char *, int);

static Read *readfns[] = {
	[Qctl]		= nil,
	[Qmacs]		= readmacs,
	[Qstat]		= readstat,
	[Qkeys]		= readkeys,
};

static Write *writefns[] = {
	[Qctl]		= writectl,
	[Qmacs]		= nil,
	[Qstat]		= nil,
	[Qkeys]		= nil,
};

void
dump(void)
{
	int fd0, fd1;
	Lun *lp;
	Key *kp;
	Mac *mp;
	Dir d;

	dprint("dump\n");
	fd0 = open(file0, OREAD|ORCLOSE);
	if (fd0 < 0) {
		fd0 = create(file0, OREAD|ORCLOSE, 0666);
		if (fd0 < 0) {
			fprint(2, "rrfs: can't create %s: %r\n", file0);
			return;
		}
	}
	fd1 = create(file1, OWRITE, 0666);
	if (fd1 < 0) {
		fprint(2, "rrfs: can't create %s: %r\n", file1);
		return;
	}
	listforeach(&luns, lp) {
		if (listempty(&lp->keys))
			continue;
		if (lp->persist == 0)
			continue;
		fprint(fd1, "%d 'reset'\n", lp->lun);
		listforeach(&lp->keys, kp) {
			fprint(fd1, "%d 'register %K", lp->lun, kp->key);
			listforeach(&kp->macs, mp) {
				fprint(fd1, " %M", mp->mac);
			} listdone;
			fprint(fd1, "'\n");
		} listdone;
		if (!reserved(lp))
			continue;
		fprint(fd1, "%d 'reserve %d ", lp->lun, lp->rtype);
		if (singleowner(lp->rtype))
			fprint(fd1, "%K", lp->owner->key);
		else {
			kp = listhead(&lp->keys);
			fprint(fd1, "%K", kp->key);
		}
		fprint(fd1, "'\n");
	} listdone;
	close(fd0);
	nulldir(&d);
	d.mode = DMEXCL|0666;
	d.name = basename(file0);
	if (dirfwstat(fd1, &d) == -1)
		fprint(2, "rrfs: can't wstat %s: %r\n", file1);
	close(fd1);
}

void
restore(void)
{
	Biobuf *bp;
	char *p, *f[2];
	int repair, nf;
	Lun *lp;

	dprint("restore\n");
	repair = 0;
	bp = Bopen(file0, OREAD);
	if (bp == nil) {
		bp = Bopen(file1, OREAD);
		if (bp == nil)
			return;
		fprint(2, "rrfs: repairing %s: %r\n", file0);
		repair++;
	}
	for (;;) {
		p = Brdline(bp, '\n');
		if (p == nil)
			break;
		p[Blinelen(bp)-1] = '\0';
		nf = tokenize(p, f, nelem(f));
		if (nf != nelem(f))
			continue;
		lp = plumblun(f[0]);
		if (lp == nil)
			continue;
		writectl(lp, f[1], strlen(f[1]));
	}
	Bterm(bp);
	if (repair)
		dump();
}

void
fsattach(Req *r)
{
	r->ofcall.qid = root;
	r->fid->qid = r->ofcall.qid;
	respond(r, nil);
}

int
fsdirgen(int n, Dir *d, void *arg)
{
	Lun *lp;

	d->atime = time(nil);
	d->mtime = d->atime;
	d->length = 0;
	if (n == -1) {
		d->qid = root;
		d->mode = DMDIR|0555;
		d->name = estrdup9p("/");
		goto done;
	}
	if (arg == nil) {
		lp = nextlun(n);
		if (lp == nil)
			return -1;
		n = Qlun;	/* dirread9p */
	} else
		lp = arg;
	switch (n) {
	case Qlun:
		d->qid = QID(lp->lun, Qlun, QTDIR);
		d->mode = DMDIR|0555;
		d->name = estrdup9p(lp->name);
		break;
	case Qctl:
		d->qid = QID(lp->lun, Qctl, QTFILE);
		d->mode = 0222;
		d->name = estrdup9p("ctl");
		break;
	case Qmacs:
		d->qid = QID(lp->lun, Qmacs, QTFILE);
		d->mode = 0444;
		d->name = estrdup9p("macs");
		break;
	case Qstat:
		d->qid = QID(lp->lun, Qstat, QTFILE);
		d->mode = 0444;
		d->name = estrdup9p("stat");
		break;
	case Qkeys:
		d->qid = QID(lp->lun, Qkeys, QTFILE);
		d->mode = 0444;
		d->name = estrdup9p("keys");
		break;
	default:
		return -1;
	}
done:
	d->uid = estrdup9p(getuser());
	d->gid = estrdup9p(d->uid);
	d->muid = estrdup9p(d->uid);
	return 0;
}

void
fsread(Req *r)
{
	Fid *fid;
	Lun *lp;
	Read *fn;

	fid = r->fid;
	if (fid->qid.type & QTDIR) {
		if (fid->qid.path == 0)
			dirread9p(r, fsdirgen, nil);
		else {
			lp = findlun(QTOL(fid->qid));
			if (lp == nil) {
				respond(r, Ebadlun);
				return;
			}
			dirread9p(r, fsdirgen, lp);
		}
		respond(r, nil);
		return;
	}
	lp = findlun(QTOL(fid->qid));
	if (lp == nil) {
		respond(r, Ebadlun);
		return;
	}
	fn = readfns[QTOT(fid->qid)];
	if (fn == nil) {
		respond(r, Eperm);
		return;
	}
	if (r->ifcall.offset != 0) {
		respond(r, nil);
		return;
	}
	r->ofcall.count = (*fn)(lp, r->ofcall.data, r->ifcall.count);
	respond(r, nil);
}

void
fswrite(Req *r)
{
	Fid *fid;
	Lun *lp;
	Write *fn;

	fid = r->fid;
	lp = findlun(QTOL(fid->qid));
	if (lp == nil) {
		respond(r, Ebadlun);
		return;
	}
	fn = writefns[QTOT(fid->qid)];
	if (fn == nil) {
		respond(r, Eperm);
		return;
	}
	r->ofcall.count = (*fn)(lp, r->ifcall.data, r->ifcall.count);
	if (r->ofcall.count == 0) {
		responderror(r);
		return;
	}
	respond(r, nil);
}

void
fsstat(Req *r)
{
	Fid *fid;
	Lun *lp;

	fid = r->fid;
	if (fid->qid.path == 0)
		fsdirgen(-1, &r->d, nil);
	else {
		lp = findlun(QTOL(fid->qid));
		if (lp == nil) {
			respond(r, Ebadlun);
			return;
		}
		fsdirgen(QTOT(fid->qid), &r->d, lp);
	}
	respond(r, nil);
}

char *
fswalk1(Fid *fid, char *name, Qid *qid)
{
	Lun *lp;

	if (fid->qid.path == 0) {
		if (strcmp(name, "..") == 0)
			return nil;
		lp = plumblun(name);
		if (lp == nil)
			return Ebadlun;
		*qid = QID(lp->lun, Qlun, QTDIR);
		return nil;
	}
	if (strcmp(name, "..") == 0) {
		*qid = root;
		return nil;
	}
	lp = findlun(QTOL(fid->qid));
	if (lp == nil)
		return Ebadlun;
	if (strcmp(name, "ctl") == 0)
		*qid = QID(lp->lun, Qctl, QTFILE);
	else if (strcmp(name, "macs") == 0)
		*qid = QID(lp->lun, Qmacs, QTFILE);
	else if (strcmp(name, "stat") == 0)
		*qid = QID(lp->lun, Qstat, QTFILE);
	else if (strcmp(name, "keys") == 0)
		*qid = QID(lp->lun, Qkeys, QTFILE);
	else
		return Enotexist;
	return nil;
}

Srv fs = {
	.attach	= fsattach,
	.read	= fsread,
	.write	= fswrite,
	.stat	= fsstat,
	.walk1	= fswalk1,
};

void
setfiles(char *s)
{
	static char ext[] = "jrn";

	file0 = estrdup9p(s);
	dprint("file0 '%s'\n", file0);
	file1 = emalloc9p(strlen(file0)+nelem(ext)+1);
	sprint(file1, "%s.%s", file0, ext);
	dprint("file1 '%s'\n", file1);
}

int
Kfmt(Fmt *f)
{
	uchar *p;

	p = va_arg(f->args, uchar *);
	return fmtprint(f, "%.*lH", Nkey, p);
}

int
Mfmt(Fmt *f)
{
	uchar *p;

	p = va_arg(f->args, uchar *);
	return fmtprint(f, "%.*lH", Nmac, p);
}

void
dprint(char *format, ...)
{
	static ulong l;
	va_list v;
	char *s;

	if (!debug)
		return;
	va_start(v, format);
	s = vsmprint(format, v);
	print("%uld %s", ++l, s); 
	va_end(v);
	free(s);
}

void
usage(void)
{
	fprint(2, "usage: rrfs [-Dd] [-s srvname] [-m mtpt] file\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *srvname;
	char *mtpt;

	srvname = nil;
	mtpt = nil;
	ARGBEGIN {
	case 'D':
		chatty9p++;
		break;
	case 'd':
		debug++;
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND
	if (argc != 1)
		usage();
	if (srvname == nil && mtpt == nil)
		mtpt = "/n/rr";
	setfiles(*argv);
	fmtinstall('H', encodefmt);
	fmtinstall('K', Kfmt);
	fmtinstall('M', Mfmt);
	listinit(&luns, Lun, node);
	restore();
	post++;
	dprint("post\n");
	postmountsrv(&fs, srvname, mtpt, MREPL);
	exits(nil);
}
