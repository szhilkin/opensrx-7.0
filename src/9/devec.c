#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"pool.h"
#include	"../ip/ip.h"
#include	"../port/error.h"
#include	"../port/netif.h"
#include	"etherif.h"
#include  <ctype.h>
  
#include "../port/iofilter.h"
#include "aoe.h"

#define errorstr errorf

enum {
	ECmagic = 0x736c7270,
	ECversion = 2,

	ECminblk = 4096,
	ECmaxblk = 8192,
	ECisize = 32,
	ECipersec = 512/ECisize,

	ENht = 400009,
	ENbypht = 10007,

	/* Flags used only in the superblock */
	EFrdbuf = 0<<11,		/* strictly a read buffer; writes invalidate */
	EFaside = 1<<11,		/* treat as a write-redundant buffer instead of full cache */
	EFwback = 2<<11,		/* normal write-back cache */
	EFwthrough = 3<<11,		/* normal write-through cache */
	EFctype = 3<<11,
	EFauto = 1<<13,			/* automatically use the next cache level for backing I/O */
	EFbypass = 1<<14,		/* use the bypass list */
	EFquick = 1<<15,		/* always use quick-start in bypass */

	/* Flags used for both the superblock and regular blocks */
	EFvalid = 1<<8,
	EFdirty = 1<<9,
	EFnf = 1<<10,

	/* Flags and types used only for regular blocks */
	EFstale = 1<<13,
	EFloading = 1<<14,

	ETsuper = 0xff,
	ETcachemeta = 0xfe,
	ETunknown = 0,
	ETdirect = 1,
	ETmaxuser = 0x7f,

	Nlevel = 3,
	Ncdev = 16,
	Ncblocks = 12*(1<<20),     	/* 12 million blocks */

	CDfree = 0,
	CDalloc,
	CDactive,
};

typedef struct ECsuper ECsuper;
typedef struct ECindex ECindex;
typedef struct Bypblock Bypblock;
typedef struct Target Target;
typedef struct Cdev Cdev;
typedef struct Cache Cache;
typedef struct Bgop Bgop;

struct ECsuper {
	ulong magic;
	ulong version;
	ulong blksize;		/* in bytes */
	ushort flags;
	uvlong size;		/* in EC blocks */
	uvlong frstdat;		/* in EC blocks */
};

struct ECindex {
	int target;
	uvlong bno;
	ushort flags;
	uchar cdev;
	ulong aticks, mticks;
	ulong secmask;
	ECindex *next;
	ECindex *nlru, *plru;
};

struct Bypblock {
	int target;
	uvlong bno;
	int hit;
	Bypblock *next, *prev;
	Bypblock *hnext;
};

struct Target {
	int target, pri;
	long whitrate;
	ulong incache, wssize, minws, sinceref;
	uvlong nio, nmiss;
	Target *next;
	char name[10];
};

struct Cdev {
	QLock mlock;
	int state;		/* of the slot - not the device */
	char *name;
	Chan *ch;
	long (*io)(Chan *, void *, long, vlong, int);
	Cache *parent;
	long sweeppid;
	ulong *lpmap;		/* logical -> physical */
	uvlong *seq;
	uvlong size;
	uvlong frstdat;
	ulong emptypos;
	uvlong base;		/* in bytes */
	ulong navail;
	ECindex *ecindex;
	ECindex *availhd, *availtl;
	uint werr, rerr;
	char fname[10];
};

struct Cache {
	RWlock;
	RWlock tablelock;
	Cdev cdev[Ncdev];
	long (*backio)(int, void *, long, vlong, int, int);
	int ncdev;
	ulong nmeta;
	ECsuper ecsuper;
	int iperblk, secperblk;

	int nstale;
	int nbyp;
	Bypblock *byphd, *byptl;
	Bypblock *bypht[ENbypht];
	ECindex *lruhd, *lrutl;
	ECindex *echt[ENht];
	Target *targhd;
	int ecrdy;
	int pos;
	uvlong rbstep, rbstart, rbend;
	long whitrate;
	uvlong nio, nmiss;
	int maxsearch;
};

long ecio(int, int, void *, long, vlong, int, int);
static int putecidx(Cache *, ECindex *);

extern ulong kerndate;

static Cache *cacheset;
static uvlong seq;

static int nfpass, nfree;

static ulong
hash(int targ, uvlong bno, ulong modulus)
{
	ulong seed, mid;
	uvlong sq;

	seed = targ & 0xff;
	seed |= (bno & 0xffffff) << 8;
	sq = (uvlong)seed * seed;
	mid = (sq >> 16) & 0xffffffff;
	return mid % modulus;
}

/* call with tablelock rlock */
static ECindex *
lkupht(Cache *ec, int targ, uvlong bno)
{
	ECindex *p;
	ulong idx, i;

	idx = hash(targ, bno, ENht);
	for (p = ec->echt[idx], i = 0; i < 1000 && p && (p->target != targ || p->bno != bno); p = p->next, ++i) ;
	if (i >= 1000) {
		return nil;
	}
	if (i > ec->maxsearch)
		ec->maxsearch = i;
	return p;
}

/* call with tablelock wlock */
static void
insht(Cache *ec, ECindex *p)
{
	ulong idx;

	idx = hash(p->target, p->bno, ENht);
	p->next = ec->echt[idx];
	ec->echt[idx] = p;
	p->mticks = p->aticks = Ticks;
}

/* call with tablelock wlock */
static void
rmht(Cache *ec, ECindex *p)
{
	ECindex *q;
	ulong idx;
	int i;

	idx = hash(p->target, p->bno, ENht);
	if (ec->echt[idx] == p)
		ec->echt[idx] = p->next;
	else {
		for (q = ec->echt[idx], i = 0; i < 1000 && q && q->next && q->next != p; q = q->next, ++i) ;
		if (i >= 1000) {
			return;
		}
		if (q && q->next == p)
			q->next = p->next;
	}
	p->next = nil;
}

/* call with tablelock rlock */
static Bypblock *
lkupbyp(Cache *ec, int targ, uvlong bno)
{
	Bypblock *p;
	ulong idx, i;

	idx = hash(targ, bno, ENbypht);
	for (p = ec->bypht[idx], i = 0; p && (p->target != targ || p->bno != bno); p = p->hnext, ++i) ;
	return p;
}

/* call with tablelock wlock */
static void
insbyp(Cache *ec, Bypblock *p)
{
	ulong idx;

	idx = hash(p->target, p->bno, ENbypht);
	p->hnext = ec->bypht[idx];
	ec->bypht[idx] = p;
	++ec->nbyp;
}

/* call with tablelock wlock */
static void
rmbyp(Cache *ec, Bypblock *p)
{
	Bypblock *q;
	ulong idx;

	idx = hash(p->target, p->bno, ENbypht);
	if (ec->bypht[idx] == p)
		ec->bypht[idx] = p->hnext;
	else {
		for (q = ec->bypht[idx]; q && q->hnext && q->hnext != p; q = q->hnext) ;
		if (q && q->hnext == p)
			q->hnext = p->hnext;
	}
	p->hnext = nil;
	--ec->nbyp;
}

/* call with tablelock wlock */
static void
addavail(Cache *ec, ECindex *p)
{
	Cdev *cd;

	cd = &ec->cdev[p->cdev];
	if (cd->state == CDfree)
		return;
	if (!cd->availhd)
		cd->availhd = p;
	else
		cd->availtl->next = p;
	cd->availtl = p;
	p->next = nil;
	++cd->navail;
}

/*
 * call with tablelock wlock
 * all blocks in list p must be from the same cdev
 */
static void
mergeavail(Cdev *cd, ECindex *p)
{
	ECindex *newhd, *newtl, *q;

	if (p == nil)
		return;
	if (cd->state == CDfree)
		return;
	for (q = p; q; q = q->next)
		++cd->navail;
	if (cd->availhd == nil) {
		cd->availhd = p;
		for (cd->availtl = p; cd->availtl->next; cd->availtl = cd->availtl->next) ;
		return;
	}
	if (cd->availhd < p) {
		newhd = newtl = cd->availhd;
		cd->availhd = newtl->next;
	}
	else {
		newhd = newtl = p;
		p = newtl->next;
	}
	newtl->next = nil;
	while (p && cd->availhd) {
		if (cd->availhd < p) {
			newtl->next = cd->availhd;
			newtl = newtl->next;
			cd->availhd = newtl->next;
		}
		else {
			newtl->next = p;
			newtl = newtl->next;
			p = newtl->next;
		}
		newtl->next = nil;
	}
	if (cd->availhd)
		newtl->next = cd->availhd;
	else
		newtl->next = p;
	while (newtl->next)
		newtl = newtl->next;
	cd->availhd = newhd;
	cd->availtl = newtl;
}

/* call with tablelock wlock, Cache rlock */
static ECindex *
getavail(Cache *ec)
{
	ECindex *p;
	Cdev *cd;
	int i;

	cd = &ec->cdev[0];
	for (i = 1; i < Ncdev; ++i)
		if (cd->state == CDactive && ec->cdev[i].navail > cd->navail)
			cd = &ec->cdev[i];
	
	if (!cd->availhd)
		return nil;
	p = cd->availhd;
	cd->availhd = p->next;
	if (!cd->availhd)
		cd->availtl = nil;
	p->next = nil;
	--cd->navail;
	return p;
}

/* call with tablelock wlock */
static void
rmavail(Cache *ec, ECindex *e)
{
	ECindex *p;
	Cdev *cd;

	cd = &ec->cdev[e->cdev];
	if (cd->state == CDfree)
		return;
	if (cd->availhd == nil)
		return;
	if (cd->availhd == e)
		cd->availhd = e->next;
	else {
		for (p = cd->availhd; p->next && p->next != e; p = p->next) ;
		if (p->next) {
			if (p->next == cd->availtl)
				cd->availtl = p;
			p->next = p->next->next;
		}
	}
	--cd->navail;
}

static int
idxless(Cache *ec, ECindex *e1, ECindex *e2)
{
	uvlong half;

	half = ec->ecsuper.size / 2;
	if (e1 > e2) {
		if (e1 - e2 < half)
			return 0;
		else
			return 1;
	}
	else {
		if (e2 - e1 < half)
			return 1;
		else
			return 0;
	}
}

/* call with tablelock wlock */
static Target *
gettarg(Cache *ec, int dev)
{
	Target *t;

	for (t = ec->targhd; t && t->target != dev; t = t->next) ;
	if (!t) {
		t = malloc(sizeof (Target));
		if (t == nil)
			return nil;
		t->target = dev;
		t->next = ec->targhd;
		t->pri = 70;
		t->minws = 1;
		t->wssize = 1;
		t->sinceref = 0;
		ec->targhd = t;
		snprint(t->name, 9, "%d", dev);
	}
	return t;
}

/* call with tablelock wlock */
static  void
rmlru(Cache *ec, ECindex *e)
{
	if (!e)
		return;
	if (ec->lruhd == e)
		ec->lruhd = e->nlru;
	else if (e->plru)
		e->plru->nlru = e->nlru;
	if (ec->lrutl == e)
		ec->lrutl = e->plru;
	else if (e->nlru)
		e->nlru->plru = e->plru;
	e->nlru = e->plru = nil;
}

/* call with tablelock wlock */
static void
inslru(Cache *ec, ECindex *e)
{
	ECindex *e2, *end;
	Cdev *cd;
	ulong blknum;

	if (!e)
		return;
	if (ec->ecsuper.blksize > 65536 || e == ec->lrutl + 1) {		/* small block sizes imply need for strong locality */
		e->nlru = nil;
		e->plru = ec->lrutl;
		if (ec->lrutl)
			ec->lrutl->nlru = e;
		else
			ec->lruhd = e;
		ec->lrutl = e;
		return;
	}
	cd = &ec->cdev[e->cdev];
	if (cd->state == CDfree)
		return;
	blknum = (e - cd->ecindex) / ec->iperblk;
	e2 = cd->ecindex + blknum * ec->iperblk;
	end = e2 + ec->iperblk;
	if (end > cd->ecindex + cd->size)
		end = cd->ecindex + cd->size;
	for (; e2 < end; ++e2) {
		if (e2 == e || e2->nlru) {
			if (e2 != e)
				rmlru(ec, e2);
			e2->nlru = nil;
			e2->plru = ec->lrutl;
			if (ec->lrutl)
				ec->lrutl->nlru = e2;
			else
				ec->lruhd = e2;
			ec->lrutl = e2;
		}
	}
}

/* call with tablelock wlock, ec rlock */
static ECindex *
getlru(Cache *ec)
{
	ECindex **lruset;
	ECindex *e, *es;
	Cdev *cd;
	Target *t, *tv;
	ulong blknum, limit;
	int i, j, n, allsame, max, x;

	if (!ec->lruhd) {
		return nil;
	}
	if (ec->ecsuper.blksize <= 65536 || (ec->ecsuper.flags & EFaside)) {		/* small block sizes imply need for strong locality */
	 	/*
		 * work on keeping some free ones all the time
		 * free as many as we can for a single putecidx
		 * call
	 	 */
		nfpass++;
		e = ec->lruhd;
		cd = &ec->cdev[e->cdev];
		lruset = malloc(ec->iperblk * sizeof (ECindex *));
		if (lruset == nil)
			return nil;
		blknum = (ulong)(e - cd->ecindex) / ec->iperblk;
		limit = ec->ecsuper.size / 1000;
		t = gettarg(ec, e->target);
		if (t == nil) {
			free(lruset);
			return nil;
		}
		for (n = 0, j = 0; n < limit && e && j < ec->iperblk; ++n, e = e->nlru) {
			if (blknum != (ulong)(e - cd->ecindex) / ec->iperblk)
				continue;
			nfree++;
			lruset[j++] = e;
			rmht(ec, e);
			e->flags &= ~EFvalid;
			e->flags |= EFnf;
			if (n > 0) {
				addavail(ec, e);
				assert(t->incache--);
			}
		}
		for (i = 0; i < j; ++i) {
			rmlru(ec, lruset[i]);
			if (lruset[i]->flags & EFnf)
				if (putecidx(ec, lruset[i]) < 0) {
					lruset[0]->flags &= ~EFvalid;
					addavail(ec, lruset[0]);
					assert(t->incache--);
					return nil;
				}
		}
		e = lruset[0];
		free(lruset);
		return e;
	}
	if (ec->nstale > 0) {
		es = nil;
		for (e = ec->lruhd; e && (e->flags & (EFvalid|EFstale|EFloading)) != EFstale; e = e->nlru)
			if (!es && (e->flags& (EFstale|EFloading)) == EFstale)
				es = e;
		if (e) {
			rmlru(ec, e);
			return e;
		}
		else if (es) {
			rmlru(ec, es);
			return es;
		}
	}
	allsame = 1;
	tv = ec->targhd;
	if (tv->next) {
		max = (100 * tv->incache) / tv->wssize;
		for (t = tv->next; t; t = t->next) {
			x = (100 * t->incache) / t->wssize;
			if (x != max) {
				allsame = 0;
				if (x > max) {
					tv = t;
					max = x;
				}
			}
		}
	}
	/*
	 * The checks for the EFloading flag are not strictly necessary,
	 * because we defer putting the block into the LRU list until after
	 * its loaded.  However, this provides an extra degree of protection
	 * against accidentally grabbing a block in the process of loading
	 * and re-assigning it before the loading is complete
	 */
	if (!allsame) {
		for (e = ec->lruhd; e && e->target != tv->target && (e->flags & EFloading); e = e->nlru) ;
		if (e) {
			rmlru(ec, e);
			return e;
		}
	}
	else if (ec->lruhd->flags & EFloading) {
		for (e = ec->lruhd; e && (e->flags & EFloading); e = e->nlru) ;
		if (e) {
			rmlru(ec, e);
			return e;
		}
		return nil;
	}
	else {
		e = ec->lruhd;
		ec->lruhd = e->nlru;
		if (ec->lruhd)
			ec->lruhd->plru = nil;
		else
			ec->lrutl = nil;
		e->nlru = e->plru = nil;
	}
	return e;
}

/* call with tablelock wlock */
static void
mvlru(Cache *ec, ECindex *e)
{
	if (ec->lrutl == e)
		return;
	rmlru(ec, e);
	inslru(ec, e);
	e->aticks = Ticks;
}

/* call with tablelock wlock */
static void
updstats(Cache *ec, int dev, int hit)
{
	Target *t, *t2;
	long wh, ws9, ws11, tpri, trate;
	int targrate;

	t = gettarg(ec, dev);
	if (t == nil)
		return;
	tpri = 0;
	trate = 0;
	for (t2 = ec->targhd; t2; t2 = t2->next) {
		tpri += t2->pri;
		trate += t2->whitrate;
		if (t2 == t)
			t->sinceref = 0;
		else if (t2->sinceref >= ec->ecsuper.size - ec->nmeta - t2->incache) {
			t2->sinceref = 0;
			t2->wssize = (99 * t2->wssize) / 100;
			if (t2->wssize == 0)
				t2->wssize = 1;
		}
		else
			t2->sinceref++;
	}
	ec->nio++;
	t->nio++;
	if (hit) {
		ec->whitrate = (999 * ec->whitrate + 500) / 1000 + 1000;
		t->whitrate = (999 * t->whitrate + 500) / 1000 + 1000;
	}
	else {
		ec->nmiss++;
		ec->whitrate = (999 * ec->whitrate + 500) / 1000;
		t->nmiss++;
		t->whitrate = (999 * t->whitrate + 500) / 1000;
		wh = (t->whitrate + 5000) / 10000;
		ws9 = (9 * t->wssize + 5) / 10;
		ws11 = (11 * t->wssize + 5) / 10;
		if (tpri > 0) {
			targrate = ((t->pri * trate) / tpri + 5000) / 10000;
			if (targrate < 10)
				targrate = 10;
			else if (targrate > 100)
				targrate = 100;
		}
		else
			targrate = 100;
		if (wh > targrate - 5 && wh < targrate + 5) {
			if (t->incache > ws11)
				t->wssize++;
			else if (t->incache < ws9 && t->wssize > t->minws)
				t->wssize--;
		}
		else {
			if (t->wssize > t->minws && t->incache <= ws11 && wh > targrate + 5)
				t->wssize--;
			else if (t->incache >= ws9 && wh < targrate - 5)
				t->wssize++;
		}
		if (t->wssize == 0)
			t->wssize = 1;
	}
}

static long
fakeedio(Chan *c, void *a, long count, vlong offset, int mode)
{
	int n;

	if (mode == OWRITE)
		n = devtab[c->type]->write(c, a, count, offset);
	else
		n = devtab[c->type]->read(c, a, count, offset);
	return n;
}

static ulong
blkcsum(Cache *ec, uchar *b)
{
	ulong *lp;
	ulong csum;
	int i, nl;

	csum = 0;
	lp = (ulong *)b;
	nl = (ec->ecsuper.blksize / sizeof (ulong)) - 1;
	for (i = 0; i < nl; ++i)
		csum += lp[i];
	return ~csum;
}

/* call with ec wlock */
static void
loadec(Cache *ec, int cdev, int validate)
{
	ECindex *d, *de, *de2;
	Target *t;
	uchar *p, *e, *tmpbuf;
	uvlong bseq, cdsize;
	vlong x;
	ulong blk;
	ulong magic, version;
	int i, nmeta;

	tmpbuf = nil;
	wlock(&ec->tablelock);
	if (waserror()) {
		free(tmpbuf);
		free(ec->cdev[cdev].ecindex);
		free(ec->cdev[cdev].lpmap);
		free(ec->cdev[cdev].seq);
		wunlock(&ec->tablelock);
		nexterror();
	}
	tmpbuf = malloc(ECminblk);
	if (!tmpbuf)
		error("memory allocation failure");
	if (ec->cdev[cdev].io == nil || ec->cdev[cdev].ch == nil)
		error("loading from non-attached cache");
	if (ec->cdev[cdev].io(ec->cdev[cdev].ch, tmpbuf, ECminblk, ec->cdev[cdev].base, OREAD) < 0) {
		ec->cdev[cdev].rerr++;
		error("superblock read failure");
	}
	cdsize = GBIT64(tmpbuf + 14);
	if (ec->ncdev > 0 && ((ec->ecsuper.size + cdsize) > Ncblocks))
		error("attach: cache device too large");
	ec->cdev[cdev].size = cdsize;
	ec->cdev[cdev].sweeppid = 0;
	ec->cdev[cdev].parent = ec;
	ec->cdev[cdev].frstdat = GBIT64(tmpbuf + 22);
	magic = GBIT32(tmpbuf);
	version = GBIT32(tmpbuf + 4);
	if (ec->ncdev == 0) {
		ec->lruhd = ec->lrutl = nil;
		ec->byphd = ec->byptl = nil;
		ec->targhd = nil;
		ec->nbyp = 0;
		memset(ec->bypht, 0, ENbypht * sizeof (Bypblock *));
		ec->nstale = 0;
		ec->ecsuper.magic = magic;
		ec->ecsuper.version = version;
		ec->ecsuper.blksize = GBIT32(tmpbuf + 8);
		ec->ecsuper.flags = GBIT16(tmpbuf + 12);
		ec->ecsuper.size = ec->cdev[cdev].size;
		ec->ecsuper.frstdat = ec->cdev[cdev].frstdat;
		ec->nmeta = ec->cdev[cdev].frstdat;
	}
	else {
		ec->ecsuper.size += ec->cdev[cdev].size;
		ec->nmeta += ec->cdev[cdev].frstdat;
	}
	ec->cdev[cdev].availhd = nil;
	ec->cdev[cdev].availtl = nil;
	ec->cdev[cdev].navail = 0;
	ec->iperblk = ec->ecsuper.blksize / ECisize - 1;
	ec->secperblk = ec->ecsuper.blksize / 512;
	if (magic != ECmagic || version != ECversion)
		errorstr("Wrong magic/version %ulx/%ulx",
			ec->ecsuper.magic, ec->ecsuper.version);
	else {
		nmeta = ec->cdev[cdev].frstdat - 2;
		if (nmeta <= 0)
			error("Corrupt cache superblock");
		ec->cdev[cdev].ecindex = malloc(ec->cdev[cdev].size * sizeof (ECindex));
		ec->cdev[cdev].lpmap = malloc(nmeta * sizeof (ulong));
		ec->cdev[cdev].seq = malloc(nmeta * sizeof (uvlong));
		if (ec->cdev[cdev].ecindex == nil || ec->cdev[cdev].lpmap == nil || ec->cdev[cdev].seq == nil)
			error("memory allocation failure");
		for (i = 0; i < nmeta; ++i)
			ec->cdev[cdev].lpmap[i] = -1;
		ec->cdev[cdev].emptypos = nmeta;
		de = ec->cdev[cdev].ecindex + ec->cdev[cdev].size;
		free(tmpbuf);
		tmpbuf = malloc(ec->ecsuper.blksize);
		if (!tmpbuf)
			error("memory allocation failure");
		e = tmpbuf + ec->iperblk * ECisize;
		if (ec->ncdev == 0)
			memset(ec->echt, 0, ENht * sizeof (ECindex *));
		for (i = 0; i < nmeta + 1; ++i) {
			x = (vlong)(i + 1) * ec->ecsuper.blksize;
			if (ec->cdev[cdev].io(ec->cdev[cdev].ch, tmpbuf, ec->ecsuper.blksize, x + ec->cdev[cdev].base, OREAD) < 0) {
				ec->cdev[cdev].rerr++;
				free(ec->cdev[cdev].ecindex);
				free(ec->cdev[cdev].lpmap);
				free(ec->cdev[cdev].seq);
				error("cache I/O error");
			}
			if (GBIT32(tmpbuf + ec->ecsuper.blksize - 4) != blkcsum(ec, tmpbuf)) {
				ec->cdev[cdev].emptypos = i;
				continue;
			}
			blk = GBIT32(tmpbuf + ec->ecsuper.blksize - 8);
			if (blk >= nmeta) {
				print("Bogus block ID in metadata block: skipping\n");
				continue;
			} 
			bseq = GBIT64(tmpbuf + ec->ecsuper.blksize - 16);
			if (ec->cdev[cdev].lpmap[blk] != -1) {
				if (bseq >= seq)
					seq = bseq + 1;
				if (bseq < ec->cdev[cdev].seq[blk]) {
					ec->cdev[cdev].emptypos = i;
					continue;
				}
				d = ec->cdev[cdev].ecindex + blk * ec->iperblk;
				de2 = d + ec->iperblk;
				if (de2 > de)
					de2 = de;
				for (; d < de2; ++d) {
					if (d->flags & EFvalid) {
						if (d->flags & EFstale)
							ec->nstale--;
						rmht(ec, d);
						rmlru(ec, d);
						if (d->target != -1) {
							t = gettarg(ec, d->target);
							if (t)
								assert(t->incache--);
						}
					}
					else
						rmavail(ec, d);
					d->flags = 0;
				}
				ec->cdev[cdev].emptypos = ec->cdev[cdev].lpmap[blk];
			}
			ec->cdev[cdev].seq[blk] = bseq;
			ec->cdev[cdev].lpmap[blk] = i;
			d = ec->cdev[cdev].ecindex + blk * ec->iperblk;
			for (p = tmpbuf; p < e  && d < de; p += ECisize, ++d) {
				d->target = GBIT32(p);
				d->bno = GBIT64(p + 4);
				d->flags = GBIT16(p + 12) & ~EFnf;
				d->aticks = GBIT32(p + 14);
				d->mticks = GBIT32(p + 18);
				d->secmask = GBIT32(p + 22);
				d->cdev = cdev;
				if (d - ec->cdev[cdev].ecindex >= ec->cdev[cdev].frstdat) {
					if (d->flags & EFvalid) {
						if (validate)
							d->flags |= EFstale;
						if (d->flags & EFstale)
							ec->nstale++;
						insht(ec, d);
						inslru(ec, d);
						t = gettarg(ec, d->target);
						if (t)
							t->incache++;
					}
					else {
						d->flags &= ~EFstale;
						addavail(ec, d);
					}
				}
			}
		}
		for (i = 0; i < nmeta; ++i)
			if (ec->cdev[cdev].lpmap[i] == -1)
				print("unset lpmap pointer: %d %d\n", nmeta, i);
	}
	for (t = ec->targhd; t; t = t->next) {
		t->wssize = t->incache;
		if (t->wssize == 0)
			t->wssize = 1;
	}
	poperror();
	free(tmpbuf);
	wunlock(&ec->tablelock);
}

static int
parsetarg(char *s, int *n, int *m)
{
	char *p, *q;

	p = strchr(s, '.');
	if (p == nil)
		return -1;
	for (q = s; q < p; ++q)
		if (!isdigit(*q))
			return -1;
	for (q = p+1; *q; ++q)
		if (!isdigit(*q))
			return -1;
	*n = atoi(s);
	*m = atoi(p+1);
	return 0;
}

static int
ecinit(Cache *ec, int cdev, char *disk)
{
	int n, m;

	wlock(ec);
	kstrdup(&ec->cdev[cdev].name, disk);
	ec->cdev[cdev].ecindex = nil;
	ec->cdev[cdev].lpmap = nil;
	ec->cdev[cdev].seq = nil;
	snprint(ec->cdev[cdev].fname, 9, "%d", cdev);
	if (strchr(ec->cdev[cdev].name, '/') || parsetarg(ec->cdev[cdev].name, &n, &m) != 0) {
		ec->cdev[cdev].ch = namec(ec->cdev[cdev].name, Aopen, ORDWR, 0);
		if (ec->cdev[cdev].ch == nil) {
			ec->cdev[cdev].io = nil;
			wunlock(ec);
			return -1;
		}
		ec->cdev[cdev].io = fakeedio;
	}
	else {
		snprint(up->errstr, ERRMAX, "AoE cache targets not yet supported");
		wunlock(ec);
		return -1;
/*
		for (i = 0; i < 30; ++i) {
			qlock(&slrplock);
			ec->targ = n << 8 | m;
			nblock = targlen(ec->targ) / ECblk;
			if (nblock > 0)
				break;
			ec->targ = -1;
			qunlock(&eclock);
			sleep(1000);
		}
		if (i >= 30) {
			ec->io = nil;
			snprunt(up->errstr, ERRMAX, "Slurpee target not available: %s\n", ec->name);
			ec->ecrdy = 1;
			fassemble();
			qunlock(&eclock);
			return -1;
		}
		else
			ec->io = edio;
*/
	}
	return 0;
}

/* call with tablelock wlock */
static void
closedev(Cdev *cd)
{
	ECindex *p, *e;
	Target *t;

	if (cd->ch == nil || cd->ecindex == nil)
		return;
	cd->state = CDalloc;
	cclose(cd->ch);
	cd->ch = nil;
	free(cd->lpmap);
	cd->lpmap = nil;
	free(cd->seq);
	cd->seq = nil;
	e = cd->ecindex + cd->size;
	for (p = cd->ecindex; p < e; ++p) {
		if (p->target != -1 && (p->flags & EFvalid)) {
			t = gettarg(cd->parent, p->target);
			if (t)
				assert(t->incache--);
		}
		rmht(cd->parent, p);
		rmlru(cd->parent, p);
	}
	free(cd->ecindex);
	cd->ecindex = nil;
	cd->navail = 0;
	cd->availhd = nil;
	cd->availtl = nil;
	cd->io = nil;
	free(cd->name);
	cd->name = nil;
	cd->state = CDfree;
}

/* return 1 if close last one, 0 otherwise */
int
ecclosedev(int lev, char *name)
{
	Cache *ec;
	Cdev *cd;
	int i;

	if (cacheset == nil)
		return 1;
	ec = &cacheset[lev];
	wlock(ec);
	for (i = 0, cd = ec->cdev; i < Ncdev; ++i, ++cd)
		if (cd->state != CDfree && cd->name && strcmp(name, cd->name) == 0)
			break;
	if (i < Ncdev) {
		ec->ecsuper.size -= cd->size;
		wlock(&ec->tablelock);
		closedev(cd);
		ec->ncdev--;
		wunlock(&ec->tablelock);
	}
	for (i = 0, cd = ec->cdev; i < Ncdev; ++i, ++cd)
		if (cd->state != CDfree)
			break;
	wunlock(ec);
	if (i < Ncdev)
		return 0;
	else {
		ec->ecrdy = 0;
		return 1;
	}
}

void
ecclose(int lev)
{
	Target *t1, *t2;
	Bypblock *b1, *b2;
	Cache *ec;
	int i;

	if (cacheset == nil)
		return;
	ec = &cacheset[lev];
	wlock(ec);
	ec->ecrdy = 0;
	wlock(&ec->tablelock);
	for (i = 0; i < Ncdev; ++i)
		if (ec->cdev[i].state != CDfree)
			closedev(&ec->cdev[i]);
	ec->ncdev = 0;
	for (b1 = ec->byphd; b1; b1 = b2) {
		b2 = b1->next;
		free(b1);
	}
	ec->byphd = ec->byptl = nil;
	ec->nbyp = 0;
	for (t1 = ec->targhd; t1; t1 = t2) {
		t2 = t1->next;
		free(t1);
	}
	ec->targhd = nil;
	wunlock(&ec->tablelock);
	wunlock(ec);
}

int
ecream(int lev, char *disk, int blksize, uvlong offset, uvlong length, int clear, int flags)
{
	ECsuper ecsuper;
	Cache *ec;
	Cdev *cd;
	uchar *p, *tmpbuf;
	uvlong i, j, k, nblock, nmeta;
	vlong x;
	ulong csum;
	int cdev;

	if (cacheset == nil)
		error("memory allocation failure");
	ec = &cacheset[lev];
	wlock(ec);
	if (ec->ncdev > 0) {
		blksize = ec->secperblk << 9;
		if ((ec->ecsuper.size + length/blksize) > Ncblocks) {
			wunlock(ec);
			error("ream : cache device too large");
		}
	}
	/* Refuse to ream a device currently in use */
	for (cdev = 0; cdev < Ncdev; ++cdev) {
		if (ec->cdev[cdev].state != CDfree && strcmp(disk, ec->cdev[cdev].name) == 0) {
			wunlock(ec);
			return -1;
		}
	}
	for (cdev = 0; cdev < Ncdev && ec->cdev[cdev].state != CDfree; ++cdev) ;
	if (cdev >= Ncdev) {
		wunlock(ec);
		return -1;
	}
	cd = &ec->cdev[cdev];
	cd->state = CDalloc;
	wunlock(ec);
	cd->sweeppid = 0;
	cd->parent = ec;
	if (ec->ncdev == 0) {
		if (cd->ch)
			ecclose(lev);
		wlock(&ec->tablelock);
		ec->ecrdy = 0;
		ec->nstale = 0;
		ec->nbyp = 0;
		ec->whitrate = 0;
		ec->nio = 0;
		ec->nmiss = 0;
		ec->maxsearch = 0;
		wunlock(&ec->tablelock);
	}
	if (ecinit(ec, cdev, disk) < 0) {
		cd->state = CDfree;
		return -1;
	}
	nblock = length / blksize;
	cd->base = offset;
	ecsuper.magic = ECmagic;
	ecsuper.version = ECversion;
	ecsuper.blksize = blksize;
	if (!ec->ecrdy)
		ec->ecsuper.blksize = ecsuper.blksize;
	ecsuper.flags = flags | EFvalid;
	ecsuper.size = nblock;
	tmpbuf = malloc(1000 * blksize);
	if (tmpbuf == nil)
		error("memory allocation failure");
	ec->iperblk = blksize / ECisize - 1;
	ec->secperblk = blksize / 512;
	nmeta = (nblock + ec->iperblk - 1) / ec->iperblk;
	cd->frstdat = nmeta + 2;
	ecsuper.frstdat = cd->frstdat;
	if (cd->io == nil || cd->ch == nil) {
		wunlock(ec);
		free(tmpbuf);
		cd->state = CDfree;
		return -1;
	}
	if (waserror()) {
		wunlock(ec);
		free(tmpbuf);
		cd->state = CDfree;
		return -1;
	}
	if (cd->io(cd->ch, tmpbuf, blksize, cd->base, OWRITE) < 0) {
		cd->werr++;
		error("cache I/O error");
	}
	seq = 0;
	for (j = 0, k = 1; j < cd->frstdat; ++k) {
		memset(tmpbuf, 0, blksize);
		for (i = 0, p = tmpbuf; i < ec->iperblk && j < cd->frstdat; ++i, ++j, p += ECisize) {
			PBIT32(p, -1);
			PBIT64(p+4, -1LL);
			PBIT16(p+12, EFvalid | (j == 0 ? ETsuper : ETcachemeta));
		}
		PBIT64(tmpbuf + blksize - 16, seq);
		seq++;
		PBIT32(tmpbuf + blksize - 8, k - 1);
		csum = blkcsum(ec, tmpbuf);
		PBIT32(tmpbuf + blksize - 4, csum);
		x = k * blksize;
		if (cd->io(cd->ch, tmpbuf, blksize, x + cd->base, OWRITE) < 0) {
			cd->werr++;
			error("cache I/O error");
		}
	}
	memset(tmpbuf, 0, blksize);
	for ( ; k < cd->frstdat - 1; ++k) {
		PBIT64(tmpbuf + blksize - 16, seq);
		seq++;
		PBIT32(tmpbuf + blksize - 8, k - 1);
		csum = blkcsum(ec, tmpbuf);
		PBIT32(tmpbuf + blksize - 4, csum);
		x = k * blksize;
		if (cd->io(cd->ch, tmpbuf, blksize, x + cd->base, OWRITE) < 0) {
			ec->cdev[cdev].werr++;
			error("cache I/O error");
		}
	}
	memset(tmpbuf, 0, blksize);
	x = k * blksize;
	if (cd->io(cd->ch, tmpbuf, blksize, x + cd->base, OWRITE) < 0)
		cd->werr++;
	++k;
	if (clear) {
		i = 1000;
		for (; k < nblock; k += 1000) {
			if (k + 1000 > nblock)
				i = nblock - k;
			x = k * blksize;
			if (cd->io(cd->ch, tmpbuf, i * blksize, x + cd->base, OWRITE) < 0)
				cd->werr++;
		}
	}
	memset(tmpbuf, 0, blksize);
	PBIT32(tmpbuf, ecsuper.magic);
	PBIT32(tmpbuf+4, ecsuper.version);
	PBIT32(tmpbuf+8, ecsuper.blksize);
	PBIT16(tmpbuf+12, ecsuper.flags);
	PBIT64(tmpbuf+14, ecsuper.size);
	PBIT64(tmpbuf+22, ecsuper.frstdat);
	if (cd->io(cd->ch, tmpbuf, blksize, cd->base, OWRITE) < 0) {
		cd->werr++;
		error("cache I/O error");
	}
	poperror();
	free(tmpbuf);
	cclose(cd->ch);
	cd->ch = nil;
	free(cd->lpmap);
	cd->lpmap = nil;
	free(cd->seq);
	cd->seq = nil;
	free(cd->ecindex);
	cd->ecindex = nil;
	cd->state = CDfree;
	wunlock(ec);
	return 0;
}

static long
fetchblock(Cache *ec, ECindex *p, int notinht, int typ, char *tmpbuf)
{
	Cdev *cd;
	vlong coff;
	long n;
	int dev, lev;

	dev = p->target;
	cd = &ec->cdev[p->cdev];
	lev = ec - cacheset;
	if (waserror()) {
		wlock(&ec->tablelock);
		rmht(ec, p);
		rmlru(ec, p);
		wunlock(&ec->tablelock);
		return -1;
	}
	n = -1;
	if ((ec->ecsuper.flags & EFauto) || ec->backio) {
		coff = (vlong)(p - cd->ecindex) * ec->ecsuper.blksize;
		if (lev < Nlevel - 1 && cacheset[lev+1].ecrdy && (ec->ecsuper.flags & EFauto))
			n = ecio(lev + 1, dev, tmpbuf, ec->ecsuper.blksize,
				p->bno * ec->ecsuper.blksize, typ, OREAD);
		else
			n = ec->backio(dev, tmpbuf, ec->ecsuper.blksize,
				p->bno * ec->ecsuper.blksize, typ, OREAD);
		if (n == ec->ecsuper.blksize) {
			if (!notinht && p->secmask && !(p->flags & EFstale)) {
				/* TBD - BLS */
				//print("Read collision on full cache p:%p t:%d b:%ulld f:%04x m:%08ulx\n", p, p->target, p->bno, p->flags, p->secmask);
			}
			if (waserror()) {
				cd->werr++;
				nexterror();
			}
			else {
				if (cd->state == CDactive)
					if (cd->io(cd->ch, tmpbuf, ec->ecsuper.blksize, coff + cd->base, OWRITE) < 0)
						error(up->errstr);
				poperror();
			}
		}
		else
			error("short read");
	}
	else
		error("no backing store");
	poperror();
	return n;
}

static void
sweeper(void *a)
{
	Rendez r;
	Cdev *cd;
	Cache *ec;
	ECindex *end, *last, *p;
	char *tmpbuf;
	int waittime, deferdec;

	cd = a;
	ec = cd->parent;
	last = cd->ecindex;
	end = cd->ecindex + cd->size;
	tmpbuf = malloc(ec->ecsuper.blksize);
	if (tmpbuf == nil)
		pexit("memory allocation failure", 1);
	waittime = 30000;
	memset(&r, 0, sizeof (Rendez));
	cd->sweeppid = up->pid;
	while (ec->ecrdy) {
		tsleep(&r, return0, nil, waittime);
		if (!ec->ecrdy || cd->sweeppid != up->pid)
			break;
		waittime = 60000;
		p = last;
		do {
			rlock(ec);
			if (ec->ecrdy == 0) {
				runlock(ec);
				break;
			}
			if (cd->state == CDfree) {
				runlock(ec);
				free(tmpbuf);
				pexit("", 1);
			}
			++p;
			if (p >= end)
				p = cd->ecindex;
			if (ec->ecrdy && p->flags & EFnf) {
				wlock(&ec->tablelock);
				putecidx(ec, p);
				wunlock(&ec->tablelock);
				last = p;
				waittime = 100;
				runlock(ec);
				break;
			}
			/*
			 * Yes, this looks odd, but I'm turning off the background stale
			 * block loading for now because the accesses it does fool the
			 * parity rebuild into thinking there's meaningful data activity
			 * which causes it to run slow, and that seems to be more troublesome
			 * than leaving blocks stale until the get accessed.
			 */
			if (1)
				runlock(ec);
			else {
				wlock(&ec->tablelock);
				if ((p->flags & (EFvalid|EFstale)) == (EFvalid|EFstale)) {
					if (!ec->ecrdy) {

						runlock(ec);
						break;
					}
					if (fetchblock(ec, p, 1, p->flags & ETmaxuser, tmpbuf) > 0) {
						deferdec = 1;
						p->flags &= ~EFstale;
						p->flags |= EFnf;
					}
					else {
						deferdec = 0;
						p->flags = EFnf;
					}
					last = p;
					waittime = 50;
					if (deferdec)
						ec->nstale--;
					wunlock(&ec->tablelock);
					runlock(ec);
					break;
				}
				else {
					wunlock(&ec->tablelock);
					runlock(ec);
				}
			}
		} while (p != last);
	}
	free(tmpbuf);
	pexit("", 1);
}

static void
eccdevfree(int level, int cdev)
{
	Cache *ec;
	Cdev *cd;

	if (level >= Nlevel || cdev >= Ncdev || cacheset == nil)
		return;
	ec = &cacheset[level];
	cd = &ec->cdev[cdev];
	cd->io = nil;
	cd->ch = nil;
	free(cd->name);
	cd->name = nil;
	cd->state = CDfree;
}

int
ecattach(int lev, char *disk, uvlong offset, int validate,
	long (*io)(Chan *, void *, long, vlong, int), long (*backio)(int, void *, long, vlong, int, int))
{
	Cache *ec;
	int cdev;
	Cdev *cd;

	if (cacheset == nil)
		error("memory allocation failure");
	ec = &cacheset[lev];
	if (ec->ncdev == 0)
		ec->ecrdy = 0;
	wlock(ec);
	for (cdev = 0; cdev < Ncdev && ec->cdev[cdev].state != CDfree; ++cdev) ;
	if (waserror()) {
		eccdevfree(lev, cdev);
		wunlock(ec);
		nexterror();
	}
	if (cdev >= Ncdev)
		error("max cache devices limit reached");
	cd = &ec->cdev[cdev];
	cd->state = CDalloc;
	wunlock(ec);
	if (ecinit(ec, cdev, disk) < 0)
		error("cache device init failed");
	if (io)
		cd->io = io;
	if (ec->ncdev == 0)
		ec->backio = backio;
	cd->base = offset;
	loadec(ec, cdev, validate);
	ec->ecrdy = 1;
	if ((ec->ecsuper.flags & EFctype) == EFrdbuf)
		kproc("sweeper", sweeper, &ec->cdev[cdev]);
	ec->ncdev++;
	cd->state = CDactive;
	poperror();
	wunlock(ec);
/*
 * We don't support this for the inital use in the SRX

	if (!(ec->ecsuper.flags & EFaside))
		wbproc();
*/
	return ec->ecsuper.blksize;
}

/*
 * If we don't think the block will be accessed again soon,
 * we might want to avoid the time to write it into the
 * cache or having to evict a block that's there.
 *
 * call with tablelock wlock
 */
static int
bypass(Cache *ec, int dev, void *, long count, vlong offset, int, int)
{
	Bypblock *b;
	uvlong bno;
	ulong navail;
	int i, quickstart;

	if (!(ec->ecsuper.flags & EFbypass))
		return 0;
	bno = offset / ec->ecsuper.blksize;
	b = lkupbyp(ec, dev, bno);
	if (!b) {
		/*
		 * We may need to play with this constant.  If we make
		 * it adaptive, we need to be sure not to accidentally
		 * implement ARC on which IBM has a patent application
		 * filed.
		 */
		if (ec->nbyp >= ec->ecsuper.size / 10) {
			b = ec->byphd;
			ec->byphd = b->next;
			if (ec->byphd)		/* This better be true if ec->nbyp > 1 */
				ec->byphd->prev = nil;
			else
				ec->byptl = nil;
			rmbyp(ec, b);
			free(b);
		}
		b = malloc(sizeof (Bypblock));
		if (b == nil)
			return 1;		/* safe to ignore this one */
		b->target = dev;
		b->bno = bno;
		b->hit = count;
		b->prev = ec->byptl;
		if (ec->byphd == nil)
			ec->byphd = b;
		else
			ec->byptl->next = b;
		ec->byptl = b;
		insbyp(ec, b);
		return 1;
	}
	for (i = 0, navail = 0; i < Ncdev; ++i)
		if (ec->cdev[i].state == CDactive)
			navail += ec->cdev[i].navail;
	quickstart = navail > ec->ecsuper.size / 10;
	if ((ec->ecsuper.flags & EFquick) || quickstart || offset % ec->ecsuper.blksize < b->hit) {
		b->hit += count;
		if (b->next)
			b->next->prev = b->prev;
		else
			ec->byptl = b->prev;
		if (b->prev)
			b->prev->next = b->next;
		else
			ec->byphd = b->next;
		rmbyp(ec, b);
		free(b);
		return 0;
	}
	b->hit += count;
	return 1;
}

long
ecio(int lev, int dev, void *a, long count, vlong offset, int typ, int mode)
{
	ECindex *p;
	Cache *ec;
	Target *t;
	Cdev *cd;
	char *tmpbuf;
	uvlong bno;
	vlong coff;
	ulong mask;
	ulong mytype;
	uint soff;
	int n, notinht, updmeta, decstale;
	char tmsg[ERRMAX];

	updmeta = 0;
	decstale = 0;
	if (lev >= Nlevel) {
		snprint(up->errstr, ERRMAX, "internal error: invalid level");
		return -1;
	}
	if (offset < 0) {
		snprint(up->errstr, ERRMAX, "Invalid offset");
		return -1;
	}
	if (cacheset == nil)
		error("memory allocation failure");
	ec = &cacheset[lev];
	rlock(ec);
	if (ec->ncdev == 0) {
		if (typ == ETdirect || (ec->ecsuper.flags & EFaside)) {
			if (up)
				snprint(up->errstr, ERRMAX, "%s operation on closed cache", typ == ETdirect ? "Direct" : "Write aside");
			runlock(ec);
			return -1;
		}
		else {
			runlock(ec);
			if (lev < Nlevel - 1 && cacheset[lev+1].ecrdy && (ec->ecsuper.flags & EFauto))
				return ecio(lev + 1, dev, a, count, offset, typ, mode);
			else if(ec->backio)
				return ec->backio(dev, a, count, offset, typ, mode);
			return -1;
		}
	}
	bno = offset / ec->ecsuper.blksize;
	soff = (offset >> 9) % ec->secperblk;
	if (typ == ETdirect) {
		offset += ec->cdev[0].frstdat * ec->ecsuper.blksize;
		bno += ec->cdev[0].frstdat;
		if (ec->cdev[0].ch == nil || ec->cdev[0].io == nil || ec->ecrdy == 0) {
			if (up)
				snprint(up->errstr, ERRMAX, "Operation on closed cache");
			runlock(ec);
			return count;
		}
		if (waserror()) {
			if (mode == OWRITE)
				ec->cdev[0].werr++;
			else
				ec->cdev[0].rerr++;
			n = -1;
		}
		else {
			if((n = ec->cdev[0].io(ec->cdev[0].ch, a, count, offset + ec->cdev[0].base, mode)) < 0)
				error(up->errstr);
			poperror();
		}
		p = ec->cdev[0].ecindex + bno;
		wlock(&ec->tablelock);
		if (p->flags & EFvalid) {
			if ((p->flags & 0xff) != typ) {
				if (up)
					snprint(up->errstr, ERRMAX, "wrong block type");
				wunlock(&ec->tablelock);
				runlock(ec);
				return -1;
			}
		}
		else {
			rmavail(ec, p);
			p->flags = (p->flags & ~0xff) | typ | EFvalid | EFnf;
		}
		if (mode == OWRITE && !(p->flags & EFdirty)) {
			p->flags |= (EFdirty|EFnf);
		}
		if (p->flags & EFnf)
			if (putecidx(ec, p) < 0)
				n = -1;
		wunlock(&ec->tablelock);
		runlock(ec);
		return n;
	}
	mytype = ec->ecsuper.flags & EFctype;
	if (mytype == EFrdbuf)
		mask = 0;
	else {
		n = soff + (count >> 9);
		mask = ((1 << soff) - 1) ^ ((1 << n) - 1);
	}
	notinht = 0;
	wlock(&ec->tablelock);
	p = lkupht(ec, dev, bno);
	if (!p) {
		if (mytype == EFrdbuf && mode == OWRITE) {
			wunlock(&ec->tablelock);
			runlock(ec);
			return count;
		}

		updstats(ec, dev, 0);
		if (mytype == EFaside && mode == OREAD) {
			wunlock(&ec->tablelock);
			runlock(ec);
			return -1;
		}

		if (bypass(ec, dev, a, count, offset, typ, mode)) {
			wunlock(&ec->tablelock);
			runlock(ec);
			if (lev < Nlevel - 1 && cacheset[lev+1].ecrdy && (ec->ecsuper.flags & EFauto))
				n = ecio(lev + 1, dev, a, count, offset, typ, mode);
			else if (ec->backio)
				n = ec->backio(dev, a, count, offset, typ, mode);
			else
				n = -1;
			return n;
		}

		notinht = 1;
		p = getavail(ec);
	}
	else {
		if (p->flags & EFstale)
			p->flags |= EFvalid;
		updstats(ec, dev, 1);
		if (p->flags & EFloading) {
			runlock(ec);
			wunlock(&ec->tablelock);
			if (lev < Nlevel - 1 && cacheset[lev+1].ecrdy && (ec->ecsuper.flags & EFauto))
				n = ecio(lev + 1, dev, a, count, offset, typ, mode);
			else if (ec->backio)
				n = ec->backio(dev, a, count, offset, typ, mode);
			else
				n = -1;
			return n;
		}
	}
	if (!p) {
		p = getlru(ec);
		if (p) {
			rmht(ec, p);
			if (p->target != -1) {
				t = gettarg(ec, p->target);
				if (t)
					assert(t->incache--);
			}
		}
	}
	if (!p) {
		runlock(ec);
		wunlock(&ec->tablelock);
		if (lev < Nlevel - 1 && cacheset[lev+1].ecrdy && (ec->ecsuper.flags & EFauto))
			n = ecio(lev + 1, dev, a, count, offset, typ, mode);
		else if (ec->backio)
			n = ec->backio(dev, a, count, offset, typ, mode);
		else
			n = -1;
		return n;
	}
	cd = &ec->cdev[p->cdev];
	if (notinht) {
		p->target = dev;
		p->bno = bno;
		insht(ec, p);
		t = gettarg(ec, dev);
		if (t)
			t->incache++;
		if (mytype == EFrdbuf && mode == OREAD)
			p->flags |= EFloading;
		else
			inslru(ec, p);
	}
	else if (mode == OWRITE && mytype == EFrdbuf) {
		if (p->flags & (EFvalid|EFstale)) {
			if (p->flags & EFstale)
				ec->nstale--;
			p->flags &= ~(EFvalid|EFstale);
			p->flags |= EFnf;
		}
		rmht(ec, p);
		rmlru(ec, p);
		addavail(ec, p);
		updmeta = 1;
		t = gettarg(ec, dev);
		if (t)
			assert(t->incache--);
	}
	else {
		mvlru(ec, p);
	}
	if ((p->flags & EFvalid) != 0 && notinht) {
		p->flags &= ~EFvalid;
		p->flags |= EFnf;
		if (mytype != EFrdbuf && putecidx(ec, p) < 0) {
			wunlock(&ec->tablelock);
			runlock(ec);
			return -1;
		}
	}
	if (!notinht && (p->flags & EFstale) && mytype == EFrdbuf && mode == OREAD)
		p->flags |= EFloading;
	wunlock(&ec->tablelock);
	if (cd->ch == nil || cd->io == nil || ec->ecrdy == 0) {
		if (up)
			snprint(up->errstr, ERRMAX, "Operation on closed cache");
		runlock(ec);
		return count;
	}
	coff = (vlong)(p - cd->ecindex) * ec->ecsuper.blksize + (soff << 9);
	if (mode == OWRITE) {
		if (mytype == EFrdbuf) {
			n = count;
		}
		else {
			if (waserror()) {
				cd->werr++;
				goto iofail;
			}
			n = cd->io(cd->ch, a, count, coff + cd->base, mode);
			if (n < 0)
				error(up->errstr);
			poperror();
			if (mytype == EFwthrough || mytype == EFaside) {
				if ((ec->ecsuper.flags & EFauto) && lev < Nlevel - 1 && cacheset[lev+1].ecrdy)
					ecio(lev + 1, dev, a, count, offset, typ, mode);
				else if (ec->backio)
					ec->backio(dev, a, count, coff, typ, mode);
			}
			wlock(&ec->tablelock);
			p->aticks = Ticks;
			if ((p->flags & EFvalid) == 0) {
				p->mticks = p->aticks;
				p->flags = EFvalid | EFdirty | EFnf | typ;
				p->secmask = mask;
				updmeta = 1;
			}
			else {
				p->mticks = p->aticks;
				if (!(p->flags & EFdirty) || (mask & ~p->secmask)) {
					p->secmask |= mask;
					p->flags |= EFnf | EFdirty;
					updmeta = 1;
				}
			}
			wunlock(&ec->tablelock);
		}
	}
	else {
		if (mytype == EFaside && (p->secmask & mask) != mask) {
			runlock(ec);
			return -1;
		}
		else if (!notinht && !(p->flags & EFstale) && (mytype == EFrdbuf || (p->secmask & mask) == mask)) {
			if (cd->ch == nil || cd->io == nil || ec->ecrdy == 0) {
				runlock(ec);
				return count;
			}
			if (waserror()) {
				cd->rerr++;
				goto iofail;
			}
			n = cd->io(cd->ch, a, count, coff + cd->base, mode);
			if (n < 0)
				error(up->errstr);
			poperror();
			p->aticks = Ticks;
		}
		else {
			tmpbuf = malloc(ec->ecsuper.blksize);
			if (tmpbuf == nil)
				goto iofail;
			n = fetchblock(ec, p, notinht, typ, tmpbuf);
			wlock(&ec->tablelock);
			p->flags &= ~EFloading;
			if (n < 0) {
				wunlock(&ec->tablelock);
				free(tmpbuf);
				goto iofail;
			}
			memmove(a, tmpbuf + (soff << 9), count);
			n = count;
			free(tmpbuf);
			/*
			 * Check to make sure we didn't have a problem that invalidated
			 * the cache block.
			 */
			if (lkupht(ec, dev, bno)) {
				if (p->nlru == nil && p->plru == nil && ec->lruhd != p)
					inslru(ec, p);
				p->aticks = Ticks;
				if (p->flags & EFstale) {
					p->flags &= ~EFstale;
					p->flags |= EFnf;
					decstale = 1;
				}
				if (notinht || p->secmask != ~0) {
					p->secmask = ~0;
					p->flags = EFvalid | EFnf | typ;
				}
			}
			wunlock(&ec->tablelock);
		}
	}
	if (decstale) {
		wlock(&ec->tablelock);
		ec->nstale--;
		wunlock(&ec->tablelock);
	}
	if (updmeta) {
		wlock(&ec->tablelock);
		if (putecidx(ec, p) < 0)
			n = -1;
		wunlock(&ec->tablelock);
	}
	runlock(ec);
	return n;

iofail:
	n = -1;
	if (mode == OREAD) {		// will need to flesh this out for write-back and through
		if (lev < Nlevel - 1 && cacheset[lev+1].ecrdy && (ec->ecsuper.flags & EFauto))
			n = ecio(lev + 1, dev, a, count, offset, typ, mode);
		else if (ec->backio)
			n = ec->backio(dev, a, count, offset, typ, mode);
	}
	if (notinht) {
		wlock(&ec->tablelock);
		p->flags &= ~EFvalid;
		addavail(ec, p);
		wunlock(&ec->tablelock);
	}
	if (n == -1 && up) {
		strcpy(tmsg, up->errstr);
		snprint(up->errstr, ERRMAX, "Cache I/O failure: %s.%ld: %s\n",
			cd->name, (p - cd->ecindex) * ec->ecsuper.blksize, tmsg);
	}
	runlock(ec);
	return n;
}

uvlong
ecage(int lev, int dev, uvlong lba, int offset)
{
	Cache *ec;
	uvlong off;
	ECindex *p;
	uvlong bno;

	if (cacheset == nil)
		return 0;
	ec = &cacheset[lev];
	off = lba << 9;
	bno = off / ec->ecsuper.blksize;
	if (bno < -offset)
		return -1;
	bno += offset;
	p = lkupht(ec, dev, bno);
	if (p == 0 || p->flags & (EFstale|EFloading))
		return -1;
	return TK2MS(Ticks - p->aticks);
}

int
ecpeek(int lev, int dev, uvlong lba, int dir)
{
	Cache *ec;
	uvlong off;
	ECindex *p;
	uvlong bno;

	if (cacheset == nil)
		return 0;
	ec = &cacheset[lev];
	off = lba << 9;
	bno = off / ec->ecsuper.blksize;
	bno += dir;
	p = lkupht(ec, dev, bno);
	if (p == 0 || p->flags & (EFstale|EFloading))
		return 0;
	return 1;
}

int
ecwritel(int lev, int dev, uvlong lba, void *a, int length, int typ)
{
	Cache *ec;
	uchar *p;
	uvlong off;
	int n, r, boff;

	if (cacheset == nil)
		error("memory allocation failure");
	ec = &cacheset[lev];
	off = lba << 9;
	boff = off % ec->ecsuper.blksize;
	p = a;
	while (length > 0) {
		n = length;
		if (n + boff > ec->ecsuper.blksize)
			n = ec->ecsuper.blksize - boff;
		if ((r = ecio(lev, dev, p, n, off, typ, OWRITE)) != n) {
			print("cache write failed: %s\n", up ? up->errstr : "kernel");
			return r;
		}
		off += n;
		length -= n;
		p += n;
		boff = 0;
	}
	return p - (uchar *)a;
}

int
ecwrite(int dev, uvlong lba, void *a, int length, int typ)
{
	return ecwritel(0, dev, lba, a, length, typ);
}

int
ecreadl(int lev, int dev, uvlong lba, void *a, int length, int typ)
{
	Cache *ec;
	uchar *p;
	uvlong off;
	int n, r, boff;

	if (cacheset == nil)
		error("memory allocation failure");
	ec = &cacheset[lev];
	off = lba << 9;
	boff = off % ec->ecsuper.blksize;
	p = a;
	while (length > 0) {
		n = length;
		if (n + boff > ec->ecsuper.blksize)
			n = ec->ecsuper.blksize - boff;
		if ((r = ecio(lev, dev, p, n, off, typ, OREAD)) != n)
			return r;
		off += n;
		length -= n;
		p += n;
		boff = 0;
	}
	return p - (uchar *)a;
}

int
ecread(int dev, uvlong lba, void *a, int length, int typ)
{
	return ecreadl(0, dev, lba, a, length, typ);
}

/*
 * If the big metadata lock becomes a bottleneck, we may need to go
 * to something like full journaling.  But for now serializing the metadata
 * writes makes things simpler.  We basically always have a single
 * journaling slot.  We write the new metadata block to it and the old
 * place where that block was located becomes the new journaling slot.
 *
 * Plus serializing the metadata writes isn't as bad as it might seem.
 * The longer we wait the greater the chances that this update will
 * have already been written as part of another update.  So between
 * reducing the number of writes and losing concurrency, it's looking
 * like it's about a wash.
 *
 * Call with tablelock wlock, ec rlock
 */
static int
putecidx(Cache *ec, ECindex *p)
{
	ECindex *base, *q, *e;
	Cdev *cd;
	uchar *s;
	uchar *blkbuf;
	ulong csum;
	ulong blknum, oblk;
	int n, failit;

	failit = 0;
	cd = &ec->cdev[p->cdev];
	qlock(&cd->mlock);
	if (cd->ch == nil || cd->ecindex == nil) {	/* the device got closed while we were waiting */
		qunlock(&cd->mlock);
		return 0;
	}
	if (!(p->flags & EFnf)) {		/* it got done while we were waiting on the lock */
		qunlock(&cd->mlock);
		return 0;	
	}
	blknum = (ulong)(p - cd->ecindex) / ec->iperblk;
	blkbuf = malloc(ec->ecsuper.blksize);
	if (!blkbuf) {
		print("malloc failure in cache\n");
		qunlock(&cd->mlock);
		return -1;
	}
	base = cd->ecindex + blknum * ec->iperblk;
	e = base + ec->iperblk;
	if (e > cd->ecindex + cd->size)
		e = cd->ecindex + cd->size;
	for (q = base, s = blkbuf; q < e; ++q, s += ECisize) {
		PBIT32(s, q->target);
		PBIT64(s + 4, q->bno);
		PBIT16(s + 12, q->flags & ~EFnf);
		PBIT32(s + 14, q->aticks);
		PBIT32(s + 18, q->mticks);
		PBIT32(s + 22, q->secmask);
	}
	oblk = cd->lpmap[blknum];
	cd->seq[blknum] = seq;
	PBIT64(blkbuf + ec->ecsuper.blksize - 16, seq);
	seq++;
	PBIT32(blkbuf + ec->ecsuper.blksize - 8, blknum);
	csum = blkcsum(ec, blkbuf);
	PBIT32(blkbuf + ec->ecsuper.blksize - 4, csum);
	if (cd->ch == nil || cd->io == nil || ec->ecrdy == 0) {
		if (up)
			snprint(up->errstr, ERRMAX, "Index operation on closed cache");
		qunlock(&cd->mlock);
		free(blkbuf);
		return 0;
	}
	if (waserror())
		n = -1;
	else {
		n = devtab[cd->ch->type]->write(cd->ch, blkbuf, ec->ecsuper.blksize,
			((vlong)cd->emptypos + 1) * ec->ecsuper.blksize + cd->base);
		poperror();
	}
	if (n < 0) {
		cd->werr++;
		/*
		 * We can't trust the metadata in the cache, so we'll try to poison
		 * the superblock so we don't try to use the metadata on the next
		 * attach
		 */
		memset(blkbuf, 0, ec->ecsuper.blksize);
		if (waserror()) {
			failit = 1;
			if ((ec->ecsuper.flags & EFctype) == EFaside)
				print("Failed to poison superblock.  May have cache corruption on next restart.\n");
		}
		else {
			if (devtab[cd->ch->type]->write(cd->ch, blkbuf, ec->ecsuper.blksize, 0) < 0)
				error(up->errstr);
			poperror();
		}
		if ((ec->ecsuper.flags & EFctype) == EFaside) {
			print("Closing cache device because of write errors\n");
			failit = 1;
		}
	}
	cd->lpmap[blknum] = cd->emptypos;
	cd->emptypos = oblk;
	for (q = base; q < e; ++q)
		q->flags &= ~EFnf;
	free(blkbuf);
	if (failit)
		closedev(cd);
	qunlock(&cd->mlock);
	return n;
}

void
ecreadopen(int lev)
{
	Cache *ec;
	ECindex *e, *ee;
	uvlong maxbno, minbno;
	int i, j, t;

	if (cacheset == nil)
		return;
	ec = &cacheset[lev];
	ec->pos = 0;
	e = ec->cdev[0].ecindex;
	ee = e + ec->cdev[0].size;
	minbno = 1ULL << 63;
	maxbno = 0;
	for (; e < ee; ++e) {
		t = e->flags & 0xff;
		if (!(e->flags & EFvalid) || t <= ETdirect || t > ETmaxuser)
			continue;
		if (e->bno > maxbno)
			maxbno = e->bno;
		if (e->bno < minbno)
			minbno = e->bno;
	}
	if (maxbno < minbno) {
		ec->rbstep = -1;
		return;
	}
	for (i = 0; i < 64 && minbno; ++i) {
		minbno >>= 1;
		maxbno >>= 1;
	}
	if (i == 0) {
		ec->rbstart = 0;
		ec->rbstep = 1;
	}
	else {
		ec->rbstart = 1ULL << (i - 1);
		ec->rbstep = ec->rbstart;
	}
	for (j = i; j < 64 && maxbno; ++j)
		maxbno >>= 1;
	ec->rbend = 1ULL << j;
	if (j - i > 6) {
		ec->rbstart = 0;
		ec->rbstep <<= j - i - 6;
	}
}

int
ecrdback(int lev, int *dev, uvlong *lba, void *a, int length, int *typ, ulong *mask)
{
	Cache *ec;
	ECindex *e;
	Cdev *cd;
	uvlong rbmask;
	int t, n;

	if (cacheset == nil)
		return 0;
	ec = &cacheset[lev];
	if (length < ec->ecsuper.blksize)
		return -1;
	rbmask = ~(ec->rbstep - 1);
	rlock(ec);
	while (1) {
		if (ec->rbstart > ec->rbend) {
			runlock(ec);
			return 0;
		}
		if (ec->pos == ec->ecsuper.size) {
			ec->pos = 0;
			ec->rbstart += ec->rbstep;
			continue;
		}
		e = &ec->cdev[0].ecindex[ec->pos];
		++ec->pos;
		t = e->flags & 0xff;
		if ((e->flags & EFvalid) && t > ETdirect && t <= ETmaxuser && (e->bno & rbmask) == ec->rbstart) {
			if (waserror()) {
				runlock(ec);
				return -1;
			}
			*dev = e->target;
			*lba = e->bno * ec->secperblk;
			*typ = t;
			*mask = e->secmask;
			cd = &ec->cdev[e->cdev];
			if (cd->ch == nil || cd->io == nil || ec->ecrdy == 0)
				error("cache not open");
			n = cd->io(cd->ch, a, ec->ecsuper.blksize, (uvlong)(e - cd->ecindex) * ec->ecsuper.blksize + cd->base, OREAD);
			poperror();
			runlock(ec);
			if (n < 0)
				cd->rerr++;
			return n;
		}
	}
}

void
ecinval(int lev, int dev)
{
	Bypblock *b, *b2;
	ECindex *e, *ee, *vhead, *vtail;
	Cdev *cd;
	Target *targ;
	Cache *ec;
	int i, t;

	if (cacheset == nil)
		return;
	ec = &cacheset[lev];
	rlock(ec);
	wlock(&ec->tablelock);
	targ = gettarg(ec, dev);
	for (i = 0; i < Ncdev; ++i) {
		cd = &ec->cdev[i];
		if (cd->state != CDactive)
			continue;
		/*
		 * Shouldn't be necessary, but if there is a valid block in the avail
		 * list, it will screw things up majorly for mergeavail
		 */
		for (e = cd->availhd; e; e = e->next)
			if (e->target == dev)
				e->flags &= ~EFvalid;
		ee = cd->ecindex + cd->size;
		vhead = vtail = nil;
		for (e = cd->ecindex; e < ee; ++e) {
			t = e->flags & 0xff;
			if ((e->flags & EFvalid) && t != ETsuper && t != ETcachemeta && t != ETdirect && e->target == dev) {
				if ((ec->ecsuper.flags & EFctype) == EFaside) {
					e->flags = EFnf;
					rmht(ec, e);
					rmlru(ec, e);
					if (vhead == nil)
						vhead = e;
					else
						vtail->next = e;
					vtail = e;
					e->next = nil;
				}
				else {
					/*
					 * The original intent here is to mark blocks we invalidate as
					 * stale allowing them to be quickly refreshed without having
					 * to make their way through the bypass list.  It's not entirely
					 * clear that's better than just removing the blocks and it add
					 * complication with potential pitfalls.  So for the time being
					 * we're going with just removing the blocks and may come back
					 * to stale marking in the future if we decide it's really better.
					 *
					if (!(e->flags & EFstale)) {
						ec->nstale++;
					}
					e->flags &= ~EFvalid;
					e->flags |= EFnf|EFstale;
					 */
					e->flags = EFnf;
					rmht(ec, e);
					rmlru(ec, e);
					addavail(ec, e);
				}
				if (targ)
					assert(targ->incache--);
			}
		}
		if ((ec->ecsuper.flags & EFctype) == EFaside) {
			mergeavail(cd, vhead);
			for (e = cd->ecindex; e < ee; ++e) {
				if (e->flags & EFnf) {
					putecidx(ec, e);
				}
			}
		}
	}
	for (b = ec->byphd; b; ) {
		b2 = b;
		b = b->next;
		if (b2->target == dev) {
			if (b2->next)
				b2->next->prev = b2->prev;
			else
				ec->byptl = b2->prev;
			if (b2->prev)
				b2->prev->next = b2->next;
			else
				ec->byphd = b2->next;
			rmbyp(ec, b2);
			free(b2);
		}
	}
	wunlock(&ec->tablelock);
	runlock(ec);
}

void
ecreclaim(int lev, int dev)
{
	ECindex *e;
	Cache *ec;

	if (cacheset == nil)
		return;
	ec = &cacheset[lev];
	if ((ec->ecsuper.flags & EFctype) == EFaside)
		return;
	wlock(&ec->tablelock);
	for (e = ec->lruhd; e; e = e->nlru) {
		if (e->target == dev && !(e->flags & EFvalid)) {
			e->flags |= (EFvalid | EFnf);
		}
	}
	wunlock(&ec->tablelock);
}

void
ecpoison(int lev)
{
	Cache *ec;
	char *tmpbuf;
	int i;

	if (cacheset == nil)
		return;
	ec = &cacheset[lev];
	rlock(ec);
	tmpbuf = malloc(ec->ecsuper.blksize);
	if (ec->ncdev == 0 || ec->ecrdy == 0 || tmpbuf == nil) {
		goto done;
	}
	if (waserror()) {
		print("error in cache superblock clear: %r\n");
		goto done;
	}
	for (i = 0; i < Ncdev; ++i)
		if (ec->cdev[i].state == CDactive)
			devtab[ec->cdev[i].ch->type]->write(ec->cdev[i].ch, tmpbuf, ec->ecsuper.blksize, 0);
	poperror();
done:
	runlock(ec);
	free(tmpbuf);
}

void
ecpriority(int lev, int dev, int pri, int respct)
{
	Cache *ec;
	Target *t;

	if (cacheset == nil)
		return;
	ec = &cacheset[lev];
	wlock(&ec->tablelock);
	t = gettarg(ec, dev);
	if (t) {
		t->minws = (respct * ec->ecsuper.size) / 100;
		if (t->minws == 0)
			t->minws = 1;
		if (t->wssize < t->minws)
			t->wssize = t->minws;
		if (pri >= 100)
			t->pri = 100;
		else if (pri <= 10)
			t->pri = 10;
		else
			t->pri = pri;
	}
	wunlock(&ec->tablelock);
}

char *
eccheck(void)
{
	ECindex ti, *tip, *eoo;
	Cache *ec;
	Cdev *cd;
	Bypblock *b;
	Target *targ;
	char *p, *e, *rbuf;
	uchar *ip, *tmpbuf;
	vlong ib, x;
	vlong size, frstdat;
	ulong magic, version, blksize, mask, lm;
	int cdev, flags, i, t, ns, nc, nd, ndd, ninc, noo, lev, len;

	if (cacheset == nil)
		error("memory allocation failure");
	/*
	 * This is a rather big overestimate of the amount of space we need,
	 * but it's not really worth it to track and realloc as we go.
	 */
	len = 4096;
	for (lev = 0; lev < Nlevel; ++lev) {
		for (targ = cacheset[lev].targhd; targ; targ = targ->next)
			len += 100;
	}
	rbuf = malloc(len+1);
	if (rbuf == nil)
		error("memory allocation failure");
	p = rbuf;
	e = rbuf + len;
	for (lev = 0; lev < Nlevel; ++lev) {
		ec = &cacheset[lev];
		if (lev > 0)
			p = seprint(p, e, "\n");
		p = seprint(p, e, "Cache level %d\n", lev);
		if (ec->ncdev == 0 || !ec->ecrdy) {
			continue;
		}
		tmpbuf = malloc(ec->ecsuper.blksize);
		if (tmpbuf == nil)
			error("memory allocation failure");
		p = seprint(p, e, "eccheck-maxsearch=%d  ", ec->maxsearch);
		p = seprint(p, e, "ncdev=%d  ", ec->ncdev);
		p = seprint(p, e, "nmiss=%ulld\n", ec->nmiss);
		p = seprint(p, e, "eccheck-checking superblock\n");
		rlock(ec);
		lm = 0;
		mask = (1 << ec->ecsuper.blksize / 512) - 1;
		if (mask == 0)
			mask = ~0;
		for (cdev = 0; cdev < Ncdev; ++cdev) {
			cd = &ec->cdev[cdev];
			if (cd->state == CDfree)
				continue;
			if (cd->name && cd->ch)
				p = seprint(p, e, "device %d: %s\n", cdev, cd->name);
			else {
				p = seprint(p, e, "device %d: Closed\n", cdev);
				continue;
			}
			if (waserror()) {
				p = seprint(p, e, "eccheck-found errors\n");
				continue;
			}
			if (cd->io(cd->ch, tmpbuf, ec->ecsuper.blksize, cd->base, OREAD) < 0) {
				cd->rerr++;
				p = seprint(p, e, "Cannot read from the cache");
				poperror();
				continue;
			}
			magic = GBIT32(tmpbuf);
			version = GBIT32(tmpbuf + 4);
			blksize = GBIT32(tmpbuf + 8);
			flags = GBIT16(tmpbuf + 12);
			size = GBIT64(tmpbuf + 14);
			frstdat = GBIT64(tmpbuf + 22);
			if (magic != ECmagic) {
				p = seprint(p, e, "eccheck-bad magic number: %04ulx\n", magic);
				poperror();
				continue;
			}
			if (version != ECversion) {
				p = seprint(p, e, "eccheck-wrong version: %uld\n", version);
				poperror();
				continue;
			}
			if (size < 0) {
				p = seprint(p, e, "eccheck-negative size: %lld\n", size);
				poperror();
				continue;
			}
			p = seprint(p, e, "%lld blocks in cache, first data block is %lld,  emptypos=%uld\n",
				size, frstdat, cd->emptypos);
			p = seprint(p, e, "eccheck-checking index blocks\n");
			ns = nc = nd = ndd = ninc = 0;
			for (ib = 1; ib < cd->frstdat; ++ib) {
				if (ib - 1 == cd->emptypos)
					continue;
				x = ib * ec->ecsuper.blksize;
				if (cd->io(cd->ch, tmpbuf, ec->ecsuper.blksize, x + cd->base, OREAD) < 0) {
					cd->rerr++;
					p = seprint(p, e, "failed to read index block %lld\n", ib);
					continue;
				}
				for (i = 0, ip = tmpbuf; i < ec->iperblk; ++i, ip += ECisize) {
					ti.target = GBIT32(ip);
					ti.bno = GBIT64(ip+4);
					ti.flags = GBIT16(ip+12);
					ti.aticks = GBIT32(ip+14);
					ti.mticks = GBIT32(ip+18);
					ti.secmask = GBIT32(ip+22);
					if (!(ti.flags & EFvalid))
						continue;
					t = ti.flags & 0xff;
					switch (t) {
					case ETunknown:
						break;
					case ETsuper:
						ns++;
						break;
					case ETcachemeta:
						nc++;
						break;
					case ETdirect:
						ndd++;
						break;
					default:
						if (t > ETdirect && t <= ETmaxuser)
							nd++;
						else
							p = seprint(p, e, "bad index type: block %lld entry %d type %02x\n", ib, i, t);
						if ((ti.secmask & mask) != mask)
							ninc++;
						lm = ti.secmask;
					}
				}
			}
			p = seprint(p, e, "version: %uld, blocksize: %uld, flags: %04x, ", version, blksize, flags);
			p = seprint(p, e, "write errors: %d, read errors: %d\n", cd->werr, cd->rerr);
			p = seprint(p, e, "%d super blocks, %d metadata blocks, %d direct blocks, %d data blocks\n", ns, nc, ndd, nd);
			if (ninc)
				p = seprint(p, e, "eccheck-%d incomplete data blocks %ulx %ulx\n", ninc, mask, lm);
			poperror();
		}
		if (ec->nio != 0)
			p = seprint(p, e, "hit rate %ulld%%  recent hit rate %ld%%\n",
				(100 * (ec->nio - ec->nmiss)) / ec->nio, (ec->whitrate + 5000) / 10000);
		rlock(&ec->tablelock);
		for (targ = ec->targhd; targ; targ = targ->next) {
			p = seprint(p, e, "target %d  in cache %uld  working set %uld",
				targ->target, targ->incache, targ->wssize);
			if (targ->nio != 0)
				p = seprint(p, e, "  hit rate %ulld%%  recent hit rate %ld%%\n",
					(100 * (targ->nio - targ->nmiss)) / targ->nio, (targ->whitrate + 5000) / 10000);
			else
				p = seprint(p, e, "\n");
		}
		p = seprint(p, e, "eccheck-no errors\n");
		ns = nc = nd = ndd = ninc = noo = 0;
		for (cdev = 0; cdev < Ncdev; ++cdev) {
			cd = &ec->cdev[cdev];
			if (cd->state == CDfree)
				continue;
			for (i = 0, tip = cd->ecindex; i < cd->size; ++i, ++tip) {
				if (!(tip->flags & EFvalid))
					continue;
				t = tip->flags & 0xff;
				switch (tip->flags & 0xff) {
				case ETunknown:
					break;
				case ETsuper:
					ns++;
					break;
				case ETcachemeta:
					nc++;
					break;
				case ETdirect:
					ndd++;
					break;
				default:
					if (t > ETdirect && t <= ETmaxuser)
						nd++;
					else
						p = seprint(p, e, "bad index type: entry %d type %02x\n", i, t);
					if ((tip->secmask & mask) != mask)
						ninc++;
				}
			}
		}
		t = 0;
		p = seprint(p, e, "eccheck-in memory\n");
		p = seprint(p, e, "eccheck-avail ");
		for (cdev = 0; cdev < Ncdev; ++cdev) {
			cd = &ec->cdev[cdev];
			if (cd->state == CDfree)
				continue;
			for (x = 0, tip = cd->availhd; tip; ++x, tip = tip->next) {
				if (tip->flags & EFnf)
					++t;
				if (tip->flags & EFvalid)
					p = seprint(p, e, "Block %ld is both available and valid?!?!?\n", tip - cd->ecindex);
				if (x > ec->ecsuper.size) {
					p = seprint(p, e, "\nCycle in available list\n");
					break;
				}
			}
			if (cd->availtl && cd->availtl->next != nil)
				p = seprint(p, e, "mangled\n");
			if (ec->lrutl && ec->lrutl->nlru != nil)
				p = seprint(p, e, "mangled\n");
			p = seprint(p, e, "%lld/%uld ", x, cd->navail);
		}
		eoo = nil;
		for (x = 0, tip = ec->lruhd; tip; ++x, tip = tip->nlru) {
			if (tip->flags & EFnf)
				++t;
			if (tip->nlru && idxless(ec, tip->nlru, tip)) {
				++noo;
				if (eoo == nil)
					eoo = tip;
			}
			if (x > ec->ecsuper.size) {
				p = seprint(p, e, "\nCycle in LRU list\n");
				break;
			}
		}
		p = seprint(p, e, "lru %lld  ", x);
		x = 0;
		for (i = 0; i < ENht; ++i)
			for (tip = ec->echt[i]; tip; ++x, tip = tip->next) ;
		for (b = ec->byphd, i = 0; b; b = b->next, ++i) ;
		runlock(&ec->tablelock);
		p = seprint(p, e, "hash %lld  stale %d  EFnf %d  ", x, ec->nstale, t);
		p = seprint(p, e, "bypass %d\n", i);
		p = seprint(p, e, "eccheck-%d super blocks, %d metadata blocks, %d direct blocks, %d data blocks\n", ns, nc, ndd, nd);
		p = seprint(p, e, "eccheck-%d incomplete data blocks %ulx %ulx  %d OO blocks %p\n", ninc, mask, lm, noo, eoo);
		runlock(ec);
		free(tmpbuf);
	}
	return rbuf;
}

static int fsinitted;

enum {
	Qtopdir = 0,
	Qlevdir,
	Qlstats,
	Qhashcnt,
	Qtargdir,
	Qtstats,
	Qdevdir,
	Qdstats,

	Levshift = 8,
	Classmask = ((1<<Levshift)-1),

	Ndent = 5,
	/*
	 * Positions in the level directory
	 */
	LDot = 0,
	LStats,
	LHashcnt,
	LDevices,
	LTargets,
};

#define path2lev(p) ((p)>>Levshift)
#define path2type(p) ((p)&Classmask)

static Dirtab topdir[Nlevel+1] = {
	".",		{Qtopdir, 0, QTDIR},		0,	DMDIR|0555,
};

static Dirtab ecldir[Nlevel][Ndent];

static void
ecdinit(void)
{
	if (cacheset != nil)
		free(cacheset);
	cacheset = malloc(Nlevel * sizeof (Cache));
}

static int
ecdgen(Chan *c, char *name, Dirtab *, int, int i, Dir *dp)
{
	Qid qid;
	Cache *ec;
	Target *t;
	int lev, j, k, which;

	if (cacheset == nil)
		return -1;
	lev = path2lev(c->qid.path);
	ec = &cacheset[lev];
	switch (path2type(c->qid.path)) {
	case Qtopdir:
		return devgen(c, name, topdir, nelem(topdir), i, dp);
	case Qlevdir:
	case Qlstats:
	case Qhashcnt:
		return devgen(c, name, ecldir[lev], Ndent, i, dp);
	case Qtargdir:
	case Qtstats:
		if (i == -1)
			return devgen(c, ".", ecldir[lev], Ndent, i, dp);
		else {
			rlock(&ec->tablelock);
			if (name) {
				if (name[0] < '0' || name[0] > '9') {
					runlock(&ec->tablelock);
					return -1;
				}
				which = atoi(name);
				for (t = ec->targhd; t && t->target != which; t = t->next) ;
			}
			else
				for (which = 0, t = ec->targhd; t && which < i; ++which, t = t->next) ;
			if (t == nil) {
				runlock(&ec->tablelock);
				return -1;
			}
			qid.path = Qtstats | lev << Levshift;
			qid.vers = 0;
			qid.type = QTFILE;
			devdir(c, qid, t->name, 0, eve, 0444, dp);
			runlock(&ec->tablelock);
			return 1;
		}
		break;
	case Qdevdir:
	case Qdstats:
		if (i == -1)
			return devgen(c, ".", ecldir[lev], Ndent, i, dp);
		else {
			rlock(&ec->tablelock);
			if (name) {
				if (name[0] < '0' || name[0] > '9') {
					runlock(&ec->tablelock);
					return -1;
				}
				which = atoi(name);
			}
			else
				which = i;
			for (j = 0, k = -1; j < Ncdev && k != which; ++j) {
				if (ec->cdev[j].state == CDactive)
					++k;
			}
			if (j >= Ncdev) {
				runlock(&ec->tablelock);
				return -1;
			}
			qid.path = Qdstats | lev << Levshift;
			qid.vers = 0;
			qid.type = QTFILE;
			devdir(c, qid, ec->cdev[k].fname, 0, eve, 0444, dp);
			runlock(&ec->tablelock);
			return 1;
		}
		break;
	}
	return -1;
}

static Walkqid *
ecdwalk(Chan *c, Chan *nc, char **name, int nname)
{
	int lev;

	lev = path2lev(c->qid.path);
	switch (path2type(c->qid.path)) {
	case Qtopdir:
		return devwalk(c, nc, name, nname, topdir, nelem(topdir), ecdgen);
	case Qlevdir:
		return devwalk(c, nc, name, nname, ecldir[lev], Ndent, ecdgen);
	case Qdevdir:
	case Qtargdir:
		return devwalk(c, nc, name, nname, nil, 0, ecdgen);
		break;
	}
	return nil;
}

static int
ecdstat(Chan *c, uchar *dp, int n)
{
	int lev, file;

	lev = path2lev(c->qid.path);
	file = path2type(c->qid.path);
	switch(file) {
	case Qtopdir:
	case Qlevdir:
		return devstat(c, dp, n, topdir, nelem(topdir), ecdgen);
	case Qlstats:
	case Qhashcnt:
	case Qdevdir:
	case Qtargdir:
		return devstat(c, dp, n, ecldir[lev], Ndent, ecdgen);
	case Qdstats:
	case Qtstats:
		return devstat(c, dp, n, nil, 0, ecdgen);
	}
	error(Enonexist);
	return -1;
}

static int
idxcnt(ECindex *p)
{
	int n;

	for (n = 0; p; p = p->next)
		n++;
	return n;
}

static int
lrucnt(ECindex *p)
{
	int n;

	for (n = 0; p; p = p->nlru)
		n++;
	return n;
}

static long
hashcnt(int lev, char *buf, long len, vlong off)
{
	char *p, *e;
	int i;
	Cache *ec;

	if (off & 7)
		errorstr("improper offset to hashcnt %lld", off);
	i = off / 8;
	len -= 1;	/* for NULL */
	len &= ~7;	/* mask excess to multiple for whole records */
	if (len <= 0)
		return 0;
	p = buf;
	e = p + len + 1;
	ec = &cacheset[lev];
	rlock(&ec->tablelock);
	for (; i < ENht && p+1 < e; i++)
		p = seprint(p, e, "%-7d\n", idxcnt(ec->echt[i]));
	runlock(&ec->tablelock);
	return p - buf;
}

static long
lstats(int lev, char *buf, long len, vlong off)
{
	Cache *ec;
	char *p, *e;
	vlong sum;
	int min, max, avg, lru, n, i;

	if (off)
		return 0;

	if (cacheset == nil)
		return 0;

	p = buf;
	e = p + len;
	p = seprint(p, e, "level: %d\n", lev);
	ec = &cacheset[lev];
	rlock(&ec->tablelock);
	p = seprint(p, e, "version: %uld\n", ec->ecsuper.version);
	p = seprint(p, e, "blocksize: %uld\n", ec->ecsuper.blksize);
	p = seprint(p, e, "flags: %ux\n", ec->ecsuper.flags);
	lru = lrucnt(ec->lruhd);
	p = seprint(p, e, "lru: %d\n", lru);
	min = -1;
	max = 0;
	sum = 0;
	for (i = 0; i < ENht; i++) {
		n = idxcnt(ec->echt[i]);
		if (min == -1 || min > n)
			min = n;
		if (max == 0 || max < n)
			max = n;
		sum += n;
	}
	avg = sum / ENht;
	p = seprint(p, e, "hsum: %lld\n", sum);
	p = seprint(p, e, "hmin: %d\n", min);
	p = seprint(p, e, "hmax: %d\n", max);
	p = seprint(p, e, "havg: %d\n", avg);
	if (ec->nio > 0) {
		p = seprint(p, e, "hitrate: %ulld\n", (100 * (ec->nio - ec->nmiss)) / ec->nio);
		p = seprint(p, e, "recenthitrate: %ld\n", (ec->whitrate + 5000) / 10000);
	}
	else {
		p = seprint(p, e, "hitrate: undefined\n");
		p = seprint(p, e, "recenthitrate: undefined\n");
	}
	p = seprint(p, e, "nbuffers: %ulld\n", ec->ecsuper.size);
	runlock(&ec->tablelock);
	return p - buf;	
}

static long
tstats(int lev, int which, char *buf, long len, vlong off)
{
	Cache *ec;
	Target *t;
	char *p, *e;

	if (off)
		return 0;

	if (cacheset == nil)
		return 0;

	p = buf;
	e = p + len;
	ec = &cacheset[lev];
	rlock(&ec->tablelock);
	for (t = ec->targhd; t && t->target != which; t = t->next) ;
	if (t == nil) {
		runlock(&ec->tablelock);
		return 0;
	}
	p = seprint(p, e, "target: %d\n", t->target);
	p = seprint(p, e, "priority: %d\n", t->pri);
	p = seprint(p, e, "minws: %uld\n", t->minws);
	p = seprint(p, e, "blocks: %uld\n", t->incache);
	p = seprint(p, e, "wss: %uld\n", t->wssize);
	if (t->nio > 0) {
		p = seprint(p, e, "hitrate: %ulld\n", (100 * (t->nio - t->nmiss)) / t->nio);
		p = seprint(p, e, "recenthitrate: %ld\n", (t->whitrate + 5000) / 10000);
	}
	else {
		p = seprint(p, e, "hitrate: undefined\n");
		p = seprint(p, e, "recenthitrate: undefined\n");
	}
	runlock(&ec->tablelock);
	return p - buf;	
}

static long
dstats(int lev, int which, char *buf, long len, vlong off)
{
	Cache *ec;
	Cdev *cd;
	char *p, *e;
	int i, k, cdev;

	if (off)
		return 0;

	if (cacheset == nil)
		return 0;

	p = buf;
	e = p + len;
	ec = &cacheset[lev];
	rlock(&ec->tablelock);
	cdev = 0;
	for (i = 0, k = 0; i < Ncdev; ++i) {
		if (ec->cdev[i].state == CDactive) {
			if (k == which) {
				cdev = i;
				break;
			}
			++k;
		}
	}
	if (i >= Ncdev) {
		runlock(&ec->tablelock);
		return 0;
	}
	cd = &ec->cdev[cdev];
	p = seprint(p, e, "device: %d\n", cdev);
	p = seprint(p, e, "file: %s\n", cd->name);
	p = seprint(p, e, "blocks: %ulld\n", cd->size);
	p = seprint(p, e, "metadata: %ulld\n", cd->frstdat - 1);
	p = seprint(p, e, "inuse: %ulld\n", cd->size - cd->frstdat - cd->navail);
	p = seprint(p, e, "available: %uld\n", cd->navail);
	p = seprint(p, e, "writeerrs: %ud\n", cd->werr);
	p = seprint(p, e, "readerrs: %ud\n", cd->rerr);
	runlock(&ec->tablelock);
	return p - buf;	
}

static char *
tailname(char *path)
{
	char *p;

	for (p = path + strlen(path) - 1; p > path && *p != '/'; --p) ;
	if (*p == '/')
		++p;
	return p;
}

static long
ecdread(Chan *c, void *buf, long n, vlong off)
{
	int lev, file;

	lev = path2lev(c->qid.path);
	file = path2type(c->qid.path);
	if (c->qid.path == Qtopdir)
		return devdirread(c, buf, n, topdir, nelem(topdir), devgen);
	switch (file) {
	case Qlevdir:
		return devdirread(c, buf, n, ecldir[lev], Ndent, devgen);
	case Qtargdir:
	case Qdevdir:
		return devdirread(c, buf, n, nil, 0, ecdgen);
	case Qhashcnt:
		return hashcnt(lev, buf, n, off);
	case Qlstats:
		return lstats(lev, buf, n, off);
	case Qdstats:
		return dstats(lev, atoi(tailname(c->path->s)), buf, n, off);
	case Qtstats:
		return tstats(lev, atoi(tailname(c->path->s)), buf, n, off);
	default:
		error(Enonexist);
	}
	return 0;
}

static long
ecdwrite(Chan *, void *, long, vlong)
{
	error(Eperm);
	return 0;
}

static Chan *
ecdopen(Chan *c, int omode)
{
	int lev, file;

	lev = path2lev(c->qid.path);
	file = path2type(c->qid.path);
	if (c->qid.path == Qtopdir || file == Qlevdir)
		return devopen(c, omode, topdir, nelem(topdir), devgen);
	else
		return devopen(c, omode, ecldir[lev], Ndent, devgen);
}

static void
ecdclose(Chan *)
{
}

static void
fsinit(void)
{
	int i, j;

	for (i = 0, j = 0; i < Nlevel; ++i) {
		/*
		 * Major kludge to skip in-memory buffer level
		 * Will go away when we have level naming
		 */
		if (i == 1)
			snprint(topdir[i+1].name, KNAMELEN, "buffer");
		else {
			snprint(topdir[i+1].name, KNAMELEN, "%d", j);
			j++;
		}
		topdir[i+1].qid.path = Qlevdir |  (i << Levshift);
		topdir[i+1].qid.vers = 0;
		topdir[i+1].qid.type = QTDIR;
		topdir[i+1].length = 0;
		topdir[i+1].perm = DMDIR|0555;
		strcpy(ecldir[i][LDot].name, ".");
		ecldir[i][LDot].qid.path = Qlevdir |  (i << Levshift);
		ecldir[i][LDot].qid.vers = 0;
		ecldir[i][LDot].qid.type = QTDIR;
		ecldir[i][LDot].length = 0;
		ecldir[i][LDot].perm = DMDIR|0555;
		strcpy(ecldir[i][LHashcnt].name, "hashcnt");
		ecldir[i][LHashcnt].qid.path = Qhashcnt |  (i << Levshift);
		ecldir[i][LHashcnt].qid.vers = 0;
		ecldir[i][LHashcnt].qid.type = QTFILE;
		ecldir[i][LHashcnt].length = 0;
		ecldir[i][LHashcnt].perm = 0444;
		strcpy(ecldir[i][LStats].name, "stats");
		ecldir[i][LStats].qid.path = Qlstats |  (i << Levshift);
		ecldir[i][LStats].qid.vers = 0;
		ecldir[i][LStats].qid.type = QTFILE;
		ecldir[i][LStats].length = 0;
		ecldir[i][LStats].perm = 0444;
		strcpy(ecldir[i][LDevices].name, "devices");
		ecldir[i][LDevices].qid.path = Qdevdir | (i << Levshift);
		ecldir[i][LDevices].qid.vers = 0;
		ecldir[i][LDevices].qid.type = QTDIR;
		ecldir[i][LDevices].length = 0;
		ecldir[i][LDevices].perm = DMDIR | 0555;
		strcpy(ecldir[i][LTargets].name, "targets");
		ecldir[i][LTargets].qid.path = Qtargdir | (i << Levshift);
		ecldir[i][LTargets].qid.vers = 0;
		ecldir[i][LTargets].qid.type = QTDIR;
		ecldir[i][LTargets].length = 0;
		ecldir[i][LTargets].perm = DMDIR | 0555;
	}
	fsinitted = 1;
}

static Chan *
ecdattach(char *spec)
{
	extern Dev ecdevtab;

	if (!fsinitted)
		fsinit();
	return devattach(ecdevtab.dc, spec);
}

Dev ecdevtab = {
	L'ℂ',		/* alt-C-C */
	"ethercache",

	devreset,
	ecdinit,
	devshutdown,
	ecdattach,
	ecdwalk,
	ecdstat,
	ecdopen,
	devcreate,
	ecdclose,
	ecdread,
	devreadv,
	devbread,
	ecdwrite,
	devwritev,
	devbwrite,
	devremove,
	devwstat,
};
