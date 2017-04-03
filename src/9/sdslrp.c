#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "ureg.h"
#include "../port/error.h"
#include "../port/sd.h"
#include "../port/netif.h"
#include <fis.h>

#define dprint(...)	if(debugoff) USED(debugoff); else print(__VA_ARGS__)

extern SDifc sdslrpifc;

enum {
	MDunknown = 0,
	MDrestored,
	MDinvalid,
	MDwritten,
	MDbackup,
	MDbacking,
	MDrqbackup,
	MDrestoring,
};

enum {
	Slrpmbmagic = 0x736c7270,
};

enum {						/* offsets into the identify info. */
	Iconfig		= 0,		/* general configuration */
	Ilcyl		= 1,		/* logical cylinders */
	Ilhead		= 3,		/* logical heads */
	Ilsec		= 6,		/* logical sectors per logical track */
	Iserial		= 10,		/* serial number */
	Ifirmware	= 23,		/* firmware revision */
	Imodel		= 27,		/* model number */
	Imaxrwm		= 47,		/* max. read/write multiple sectors */
	Icapabilities	= 49,	/* capabilities */
	Istandby	= 50,		/* device specific standby timer */
	Ipiomode	= 51,		/* PIO data transfer mode number */
	Ivalid		= 53,
	Iccyl		= 54,		/* cylinders if (valid&0x01) */
	Ichead		= 55,		/* heads if (valid&0x01) */
	Icsec		= 56,		/* sectors if (valid&0x01) */
	Iccap		= 57,		/* capacity if (valid&0x01) */
	Irwm		= 59,		/* read/write multiple */
	Ilba		= 60,		/* LBA size */
	Imwdma		= 63,		/* multiword DMA mode */
	Iapiomode	= 64,		/* advanced PIO modes supported */
	Iminmwdma	= 65,		/* min. multiword DMA cycle time */
	Irecmwdma	= 66,		/* rec. multiword DMA cycle time */
	Iminpio		= 67,		/* min. PIO cycle w/o flow control */
	Iminiordy	= 68,		/* min. PIO cycle with IORDY */
	Ipcktbr		= 71,		/* time from PACKET to bus release */
	Iserbsy		= 72,		/* time from SERVICE to !Bsy */
	Iqdepth		= 75,		/* max. queue depth */
	Imajor		= 80,		/* major version number */
	Iminor		= 81,		/* minor version number */
	Icsfs		= 82,		/* command set/feature supported */
	Icsfe		= 85,		/* command set/feature enabled */
	Iudma		= 88,		/* ultra DMA mode */
	Ierase		= 89,		/* time for security erase */
	Ieerase		= 90,		/* time for enhanced security erase */
	Ipower		= 91,		/* current advanced power management */
	Ilba48		= 100,		/* 48-bit LBA size (64 bits in 100-103) */
	Irmsn		= 127,		/* removable status notification */
	Isecstat	= 128,		/* security status */
	Icfapwr		= 160,		/* CFA power mode */
	Imediaserial	= 176,	/* current media serial number */
	Icksum		= 255,		/* checksum */
};

enum {						/* bit masks for capabilities identify info */
	Mdma		= 0x0100,	/* DMA supported */
	Mlba		= 0x0200,	/* LBA supported */
	Mnoiordy	= 0x0400,	/* IORDY may be disabled */
	Miordy		= 0x0800,	/* IORDY supported */
	Msoftrst	= 0x1000,	/* needs soft reset when Bsy */
	Mstdby		= 0x2000,	/* standby supported */
	Mqueueing	= 0x4000,	/* queueing overlap supported */
	Midma		= 0x8000,	/* interleaved DMA supported */
};

enum {					/* bit masks for supported/enabled features */
	Msmart		= 0x0001,
	Msecurity	= 0x0002,
	Mrmmedia	= 0x0004,
	Mpwrmgmt	= 0x0008,
	Mpkt		= 0x0010,
	Mwcache		= 0x0020,
	Mlookahead	= 0x0040,
	Mrelirq		= 0x0080,
	Msvcirq		= 0x0100,
	Mreset		= 0x0200,
	Mprotected	= 0x0400,
	Mwbuf		= 0x1000,
	Mrbuf		= 0x2000,
	Mnop		= 0x4000,
	Mmicrocode	= 0x0001,
	Mqueued		= 0x0002,
	Mcfa		= 0x0004,
	Mapm		= 0x0008,
	Mnotify		= 0x0010,
	Mstandby	= 0x0020,
	Mspinup		= 0x0040,
	Mmaxsec		= 0x0100,
	Mautoacoustic	= 0x0200,
	Maddr48		= 0x0400,
	Mdevconfov	= 0x0800,
	Mflush		= 0x1000,
	Mflush48	= 0x2000,
	Msmarterror	= 0x0001,
	Msmartselftest	= 0x0002,
	Mmserial	= 0x0004,
	Mmpassthru	= 0x0008,
	Mlogging	= 0x0020,
};

enum {
	/* can handle following ata cmds: */
	Crdsect = 0x20,
	Crdsectext = 0x24,
	Cwrsect = 0x30,
	Cwrsectext = 0x34,
	Cident = 0xec,
	Csetfeatures = 0xfe,

	Slrpbar1sz = 0x40000,
	Slrpbar1toring = 0x4000,
	Slrpmaxsgsperringentry = 0x100,
	Nmergebufs = 32,
	Ringtune = 16,
	Maxagg = 32,
};

enum {
	/* slrpbar1 cmd */
	Creset 		= 0x01,
	Cinit		= 0x02,
	Clistslrp		= 0x03,
	Cpridma		= 0x04,
	Cprpci		= 0x05,
	Cprcpu		= 0x06,
	Cidmaverbose	= 0x07,
	Cringverbose	= 0x08,
	Chmbverbose	= 0x09,
	Cidmardtst	= 0x0a,
	Cidmawrtst	= 0x0b,
	Cprocdump	= 0x0c,
	Cirqsdump	= 0x0d,
	Csdramdump	= 0x0e,
	Cupdatefw	= 0x0f,
	Cbackup		= 0x10,
	Crestore		= 0x11,
	Ci2cdevtest	= 0x12,
	Cgetpsmstate	= 0x13,
	Cintrinit		= 0x14,
	Cxmemmovetst	= 0x15,
	xxxCmemmovetst	= 0x16,
	Cslrpreboot	= 0x17,
	Cprgpio		= 0x18,
	Csetmpp		= 0x19,
	Cprxor		= 0x1a,
	Cprereset	= 0x1b,

	Ccmdtodo	= 1<<31,

	/* irq */
	Icmd		= 1<<1,
	Iring		= 1<<0,
	Istate	= 1<<2,
};

typedef struct Slrpstate Slrpstate;
typedef struct Slrpmailbox Slrpmailbox;
typedef struct Ringentry Ringentry;
typedef struct Slrpsgentry Slrpsgentry;
typedef struct Slrpreqhdr Slrpreqhdr;
typedef struct Hostmailbox Hostmailbox;
typedef struct Drive Drive;
typedef struct Ctlr Ctlr;
typedef struct Ring Ring;
typedef struct Slrpop Slrpop;
typedef struct Qelem Qelem;

#define SLRPSTATEMAGIC "SLRPF01"
struct Slrpstate {
	uchar	magic[8];
	ulong	state;
	ulong	dummy;
	ulong	mdstate;
	ulong	pcistate;     /* peers can be interested in this, host implicitely knows */
	uchar	mac0[6];
	uchar	mac1[6];
	int	sclvl;
	int	boardtemp;
	int	poe0;
	int	poe1;
	u32int	eccaddr;
	uint	eccsbcnt;
	uint	eccdbcnt;
	uchar	shelf[2];
	uchar	slot;
	char	serial[100];
	char	rev[30];
};

struct Slrpmailbox {
 	ulong magic;              /* 0 */
 	ulong status;             /* 4 */
 	ulong cmd;                /* 8 */
 	ulong ringoffset;         /* c */
 	ulong ringlen;            /* 10 */
 	ulong producer;           /* 14 */
 	ulong consumer;           /* 18 */
 	ulong sgoffset;           /* 1c */
 	ulong sglen;              /* 20 */
 	ulong irq;                /* 24 */
 	ulong irqmask;            /* 28 */
};

struct Ringentry {
	uchar sgnum;
	uchar sg[3];
	ulong target;
	ulong status;
	uchar reserved;
	uchar feature;
	uchar sectors;
	uchar cmd;
	uchar lba[8];
};

struct Slrpsgentry {
	ulong daddrlo;
	ulong daddrhi;
	ulong len;
};

struct Slrpop {
	Drive *d;
	int len;
	int dcount;
	uvlong lba;
	SDreq *r;
	Proc *pp;
	int pndx;
	Slrpop *sib;
	Rendez;
};

struct Qelem {
	Drive *d;
	int nblk;
	int dcount;
	int write;
	uvlong lba;
	Slrpop *head, *tail;
	Qelem *next, *prev;
};

struct Ring {
	int rdone;
	int inuse;
	int nblk;
	uchar *buf;

	SDreq *r[Slrpmaxsgsperringentry];
	Proc *pp[Slrpmaxsgsperringentry];
	int dcount[Slrpmaxsgsperringentry];
	Ctlr *ctlr;
	int	status;
};

struct Ctlr {
	int	irq;

	ulong smbpaddr;
	Slrpmailbox *smb;

	Pcidev*	pcidev;
	ulong msiaddrlo;
	ulong msiaddrhi;
	ulong msidata;
	SDev*	sdev;
	Drive*  drive;

	Queue *ioq;
	Rendez qrend;
	Rendez retrend;

 	ulong ringlen;            /* number of entries */
	Ringentry *ring;
	ulong ringpaddr;

 	ulong sglen;              /* number of entries */
	Slrpsgentry *sg;
	ulong sgpaddr;

	Slrpstate *sstate;

	int lastack;
	Ring *rhndl;
	int	done;
	ulong producer;

	/* stats */
	int ringfull;
	int collision;

	QLock;		/* serialize commands */
	Lock;			/* ring access */
};

struct Drive {
	ushort	info[256];		/* must be on 8 byte boundary */
	Ctlr*	ctlr;
	SDunit *unit;

	vlong	sectors;		/* total */
	int	secsize;		/* sector size */

	char	firmware[4*2 + 1];

	int	flags;			/* internal flags */
};

static int slrphandleringpoll(Ctlr *, int);
static int resetmb(Ctlr *);
static Qelem *qhead, *qtail;
static QLock sqlock;
static Rendez qrendez;
static int identgone;
static int wantreset;
static int debugoff = 1;

static uvlong totsec, nxfer, nsingles;

static int 
slrpruncmd(Ctlr *ctlr, ulong cmd)
{
	Slrpmailbox *smb;
	int rv;

	smb = ctlr->smb;
	smb->status = 0xffffffff;
	if ((smb->cmd & Ccmdtodo) == 0) {
		smb->cmd = cmd | Ccmdtodo;
		rv = 0;
	} else
		rv = -1;
	return rv;
}

static ulong 
slrppollcmddone(Ctlr *ctlr, ulong cmd)
{
	Slrpmailbox *smb;
	int i;
	ulong rv;
	int waitfor;

	smb = ctlr->smb;
	if (cmd == Cxmemmovetst) 
		waitfor = 40000;
	else
		waitfor = 1000;
	for (i=0; i<waitfor; i++) {
		if  (smb->cmd == cmd) 
			break;
		if (up != nil)
			tsleep(&up->sleep, return0, 0, 10);
		else
			delay(10);
	}
	if (cmd != Creset && i == waitfor)
		dprint("slrppollcmd timed out\n");
	rv = smb->status;
	if (cmd != Creset && rv == 0xffffffff)
		dprint("slrppollcmd status: %lux\n", rv);
	return rv;
}

static int
slrpdone(void* arg)
{
	return ((Ctlr*)arg)->done;
}

static int
slrprdone(void* arg)
{
	Ring *r;

	r = arg;
	return r->rdone && r->inuse;
}

static int
slrprdone2(void* arg)
{
	Slrpop *s;
	Ring *r;

	s = arg;
	if (s->pndx == -1)
		return 0;
	r = &s->d->ctlr->rhndl[s->pndx];
	return r->rdone && r->inuse;
}

static void 
slrprentryprint(Ringentry *r)
{
	uchar *u;

	dprint("sgnum %ud sg %x %x %x\n", r->sgnum, r->sg[0], r->sg[1], r->sg[2]);
	dprint("target %lux status %lux feature %ux sectors %ux\n", r->target, r->status, r->feature, r->sectors);
	u = r->lba;
	dprint("cmd %ux lba %.2ux %.2ux %.2ux %.2ux %.2ux %.2ux %.2ux %.2ux\n",
		r->cmd, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]);
}

static int
slrpidentify(Ctlr *ctlr, void* info)
{
	int pndx, rv;
	ulong sgndx;
	Slrpsgentry *sg, lsg;
	Slrpmailbox *smb;
	Ringentry *rentry, lrentry;

	if (identgone)
		return -1;
	smb = ctlr->smb;

	if (PADDR(info) & 0x07)
		print("mal-aligned info in identify\n");
	memset(info, 0, 512);
	lock(ctlr);
	pndx = ctlr->producer;

	if (((pndx + 1) % ctlr->ringlen) == smb->consumer) {
		unlock(ctlr);
		return -1;
	}
	
	memset(&lrentry, 0, sizeof(lrentry));
	rentry = &ctlr->ring[pndx];
	lrentry.sgnum = 1;
	sgndx = pndx * Slrpmaxsgsperringentry;
	lrentry.sg[0] = sgndx & 0xff;
	lrentry.sg[1] = sgndx >> 8;
	lrentry.sg[2] = sgndx >> 16;
	lrentry.target = 0;
	lrentry.status = 0xffffffff;
	lrentry.feature = 0;
	lrentry.sectors = 1;
	lrentry.cmd = Cident;
	lrentry.lba[3] = 0xa0;
	memmove(rentry, &lrentry, sizeof(lrentry));
	
	sg = &ctlr->sg[sgndx];

	memset(&lsg, 0, sizeof(Slrpsgentry));
	lsg.daddrlo = PADDR(info);
	lsg.daddrhi = 0;
	lsg.len = 512;
	memmove(sg, &lsg, sizeof(lsg));
	coherence();

	ctlr->rhndl[pndx].rdone = 0;
	ctlr->rhndl[pndx].inuse = 1;
	ctlr->rhndl[pndx].ctlr = ctlr;
	ctlr->done = 0;
	ctlr->producer = (pndx + 1) % ctlr->ringlen;
	smb->producer = ctlr->producer;
	unlock(ctlr);		/* fishy.  no interlock.  (but no disk, either) */

	ctlr->rhndl[pndx].pp[0] = up;
	tsleep(&up->sleep, slrpdone, ctlr, 2*1000);

	lock(ctlr);
	if (ctlr->producer == smb->consumer) 
		rv = 0;
	else {
		rv = -1;
		dprint("ident timed out %lux %lux\n", ctlr->producer, smb->consumer);
		identgone = 1;
	}
	ctlr->rhndl[pndx].inuse = 0;
	unlock(ctlr);

	return rv;
}

static int
slrpdmasetup(Qelem *qe)
{
	static uchar *xbuf;
	static int xndx;
	Ctlr *ctlr;
	Slrpop *op;
	Slrpmailbox *smb;
	Slrpsgentry *sg, lsg;
	Ring *rhndl;
	Ringentry *rentry, lrentry;
	uchar *p;
	ulong pndx;
	ulong sgndx;
	int i;

	if (xbuf == nil)
		xbuf = malloc(Nmergebufs * 1024 * 1024);
	ctlr = qe->d->ctlr;
	lock(ctlr);
	if (waserror()) {
		unlock(ctlr);
		return -2;
	}
	if (qe->nblk > 1 && qe->dcount * ctlr->drive->secsize > 1024 * 1024)
		error("internal error: combined write too large");
	smb = ctlr->smb;
	pndx = ctlr->producer;

	if (((pndx + 1 + ctlr->ringlen - ctlr->lastack) % ctlr->ringlen) > Ringtune) {
		ctlr->ringfull++;
		unlock(ctlr);
		poperror();
		return -1;
	}
	
	rhndl = &ctlr->rhndl[pndx];
	if (rhndl->inuse) {
		ctlr->collision++;
		unlock(ctlr);
		poperror();
		return -1;
	}

	memset(&lrentry, 0, sizeof(lrentry));
	rentry = &ctlr->ring[pndx];
	lrentry.sgnum = 1;
	sgndx = pndx * Slrpmaxsgsperringentry;
	lrentry.sg[0] = sgndx & 0xff;
	lrentry.sg[1] = sgndx >> 8;
	lrentry.sg[2] = sgndx >> 16;

	lrentry.target = 0;
	lrentry.status = 0xffffffff;
	lrentry.feature = 0;
	lrentry.sectors = 1;
	/*
	 * if the first one has already timed out drop the whole element
	 * and let the timeout handle the rest too
	 */
	if (qe->head->r == nil)
		error("no request");
	if (qe->head->r->write)
		lrentry.cmd = Cwrsect;
	else 
		lrentry.cmd = Crdsect;

	lrentry.lba[0] = qe->lba;
	lrentry.lba[1] = qe->lba >> 8;
	lrentry.lba[2] = qe->lba >> 16;
	lrentry.lba[3] = qe->lba >> 24;
	lrentry.lba[4] = qe->lba >> 32;
	lrentry.lba[5] = qe->lba >> 40;
	lrentry.sectors = qe->dcount;
	memmove(rentry, &lrentry, sizeof(lrentry));
	
	sg = &ctlr->sg[sgndx];
	/*
	 * Always use our internal bounce buffer.  That way if we have to time
	 * a request out, we won't accidentally touch anyone else's memory.
	 * The one exception is that when we're reaming, we clear 1000
	 * blocks at a time, so we have to use the buffer we're given then.
	 */
	if (qe->nblk == 1 && qe->head->len > 1024 * 1024) {
		lsg.daddrlo = PADDR(qe->head->r->data);
	}
	else {
		rhndl->buf = xbuf + xndx * 1024 * 1024;
		xndx = (xndx + 1) % Nmergebufs;
		lsg.daddrlo = PADDR(rhndl->buf);
	}
	lsg.daddrhi = 0;
	lsg.len = qe->dcount * qe->d->secsize;
	p = rhndl->buf;
	for (i = 0, op = qe->head; op; ++i, op = op->sib) {
		if (qe->write && op->len <= 1024 * 1024) {
			if (op->r && op->r->data)
				memmove(p, op->r->data, op->len);
			p += op->len;
		}
		rhndl->r[i] = op->r;
		rhndl->pp[i] = op->pp;
		rhndl->dcount[i] = op->dcount;
	}
	memmove(sg, &lsg, sizeof(lsg));
	coherence();

	rhndl->rdone = 0;
	rhndl->inuse = qe->nblk;
	rhndl->nblk = qe->nblk;
	rhndl->ctlr = ctlr;

	ctlr->producer = (pndx + 1) % ctlr->ringlen;
	smb->producer = ctlr->producer;
	unlock(ctlr);
	poperror();
	if (qe->nblk == 1) {
		nsingles++;
	}
	else {
		totsec += qe->dcount;
		nxfer++;
	}
	return pndx;
}

static int
nonempty(void *)
{
	if (qhead == nil)
		return 0;
	return 1;
}

static int
slrpringok(void *arg)
{
	Ctlr *ctlr;
	Slrpmailbox *smb;
	ulong pndx;
	int rv;

	ctlr = (Ctlr*)arg;
	lock(ctlr);
	smb = ctlr->smb;
	pndx = ctlr->producer;

	if (((pndx + 1 + ctlr->ringlen - smb->consumer) % ctlr->ringlen) > Ringtune || ctlr->rhndl[pndx].inuse) {
		rv = 0;
	} else {
		rv = 1;
	}
	unlock(ctlr);
	return rv;
}

static void
doreset(Ctlr *ctlr)
{
	Ring *r;
	Slrpmailbox *smb;
	int i;

	r = ctlr->rhndl;
	while (1) {
		for (i = 0; i < ctlr->ringlen && r[i].inuse == 0; ++i) ;
		if (i >= ctlr->ringlen)
			break;
		tsleep(&ctlr->qrend, return0, nil, 100);
	}
	slrpruncmd(ctlr, Creset);
	slrppollcmddone(ctlr, Creset);
	while(pcicfgr32(ctlr->pcidev, PciVID) != (0x5121<<16 | 0x1b52))
		delay(100);
	do {
		delay(1000);
	} while (resetmb(ctlr) < 0);
	while(1) {
		qlock(ctlr);
		smb = ctlr->smb;
		smb->ringoffset = PADDR(ctlr->sstate);
		smb->ringlen = 0;
		smb->sglen = ROUND(sizeof(Slrpstate), 8);
		slrpruncmd(ctlr, Cgetpsmstate);
		slrppollcmddone(ctlr, Cgetpsmstate);
		qunlock(ctlr);
		if (ctlr->sstate->mdstate != MDbacking && ctlr->sstate->mdstate != MDrestoring)
			break;
		delay(2000);
	}
	wantreset = 0;
}

static void
qreader(void *a)
{
	Qelem *qe;
//	Qelem *qemin;
	Ctlr *ctlr;
	Slrpop *op, *opn;
	int pndx;

	ctlr = a;
	while (waserror())
		dprint("error in qreader\n");
	while (1) {
		tsleep(&qrendez, nonempty, nil, 100);
		qlock(&sqlock);
		if (qhead == nil) {
			qunlock(&sqlock);
			continue;
		}
		for (qe = qhead; qe && qe->nblk > 1; qe = qe->next) ;
		if (qe) {
			if (qe->next)
				qe->next->prev = qe->prev;
			if (qe->prev)
				qe->prev->next = qe->next;
			if (qe == qhead)
				qhead = qe->next;
			if (qe == qtail)
				qtail = qe->prev;
		}
		else {
			qe = qhead;
			qhead = qe->next;
			if (qhead == nil)
				qtail = nil;
			else
				qhead->prev = nil;
		}
/*
		qemin = qhead;
		for (qe = qemin->next; qe; qe = qe->next)
			if (qe->dcount < qemin->dcount)
				qemin = qe;
		qe = qemin;
		if (qemin->next)
			qemin->next->prev = qemin->prev;
		if (qemin->prev)
			qemin->prev->next = qemin->next;
		if (qemin == qhead)
			qhead = qemin->next;
		if (qemin == qtail)
			qtail = qemin->prev;
*/
		qunlock(&sqlock);
		while (1) {
			if (wantreset)
				doreset(ctlr);
			pndx = slrpdmasetup(qe);
			if (pndx != -1)
				break;
			tsleep(&ctlr->qrend, slrpringok, ctlr, 10);
		}
		lock(ctlr);
		for (op = qe->head; op; op = opn) {
			opn = op->sib;
			if (op->pp)
				op->pndx = pndx;
			else
				free(op);
		}
		unlock(ctlr);
		free(qe);
	}
//	poperror();
}

static int
qmerge(Slrpop *p)
{
	Qelem *qe, *qe2;

	for (qe = qhead; qe; qe = qe->next) {
		if (qe->d == p->d && qe->write == p->r->write && p->lba + p->dcount == qe->lba) {
			if ((qe->dcount + p->dcount) * 512 >= Maxagg * 1024)
				continue;
			/* insert at front */
			p->sib = qe->head;
			qe->head = p;
			qe->lba = p->lba;
			qe->dcount += p->dcount;
			++qe->nblk;
			for (qe2 = qhead; qe2; qe2 = qe2->next) {
				if (qe2 == qe)
					continue;
				if (qe2->d == qe->d && qe2->write == qe->write && qe2->lba + qe2->dcount == qe->lba) {
					if ((qe2->dcount + qe->dcount) * 512 >= Maxagg * 1024)
						return 1;
					qe2->tail->sib = qe->head;
					qe->head = qe2->head;
					qe->lba = qe2->lba;
					qe->dcount += qe2->dcount;
					qe->nblk += qe2->nblk;
					if (qe2->next)
						qe2->next->prev = qe2->prev;
					if (qe2->prev)
						qe2->prev->next = qe2->next;
					if (qe2 == qhead)
						qhead = qe2->next;
					if (qe2 == qtail)
						qtail = qe2->prev;
					free(qe2);
					break;
				}
			}
			return 1;
		}
		if (qe->d == p->d && qe->write == p->r->write && p->lba == qe->lba + qe->dcount) {
			if ((qe->dcount + p->dcount) * 512 >= Maxagg * 1024)
				continue;
			/* append to end */
			qe->tail->sib = p;
			qe->tail = p;
			qe->dcount += p->dcount;
			++qe->nblk;
			for (qe2 = qhead; qe2; qe2 = qe2->next) {
				if (qe2 == qe)
					continue;
				if (qe2->d == qe->d && qe2->write == qe->write && qe->lba + qe->dcount == qe2->lba) {
					if ((qe2->dcount + qe->dcount) * 512 >= Maxagg * 1024)
						return 1;
					qe->tail->sib = qe2->head;
					qe->tail = qe2->tail;
					qe->dcount += qe2->dcount;
					qe->nblk += qe2->nblk;
					if (qe2->next)
						qe2->next->prev = qe2->prev;
					if (qe2->prev)
						qe2->prev->next = qe2->next;
					if (qe2 == qhead)
						qhead = qe2->next;
					if (qe2 == qtail)
						qtail = qe2->prev;
					free(qe2);
					break;
				}
			}
			return 1;
		}
	}
	return 0;
}

static Slrpop *
slrpgeniostart(Drive* drive, uvlong lba, int dcount, SDreq *r)
{
	Slrpop *op;
	Qelem *qe;

	op = malloc(sizeof (Slrpop));
	if (op == nil)
		error("memory allocation failure");
	op->d = drive;
	op->dcount = dcount;
	op->len = dcount * drive->secsize;
	op->lba = lba;
	op->r = r;
	op->pp = up;
	op->pndx = -1;
	op->sib = nil;

	qlock(&sqlock);
	if (!qmerge(op)) {
		qe = malloc(sizeof (Qelem));
		if (!qe) {
			qunlock(&sqlock);
			error("memory allocation failure");
		}
		qe->d = drive;
		qe->nblk = 1;
		qe->dcount = dcount;
		qe->write = r->write;
		qe->lba = lba;
		qe->head = op;
		qe->tail = op;
		qe->next = nil;
		qe->prev = qtail;
		if (qhead == nil)
			qhead = qe;
		else
			qtail->next = qe;
		qtail = qe;
	}
	qunlock(&sqlock);
	wakeup(&qrendez);
	return op;
}

static int
slrpgenio(Drive* drive, SDreq *r)
{
	Slrpop *op;
	Ctlr *ctlr;
	uvlong lba;
	long submitted;
	int count;
	int pndx;
	int st;

	if((st = sdfakescsi(r)) != SDnostatus
	|| (sdfakescsirw(r, &lba, &count, nil)) != SDnostatus)
		return st;
	ctlr = drive->ctlr;

	if (waserror())
		return SDeio;

	op = slrpgeniostart(drive, lba, count, r);

	while(waserror())
		dprint("error in slrpgenio\n");

	for (submitted = seconds(); !slrprdone2(op) && seconds() - submitted < 30; ) {
		tsleep(&up->sleep, slrprdone2, op, 1000);
		lock(ctlr);
		if (op->pndx >= 0 && ctlr->rhndl[op->pndx].rdone == 0 && ctlr->ring[op->pndx].status == 0) {
			dprint("done and not done: %d n:%d p:%uld p:%uld c:%uld l:%d\n",
				op->pndx, ctlr->rhndl[op->pndx].nblk, ctlr->producer, ctlr->smb->producer,
				ctlr->smb->consumer, ctlr->lastack);
			slrphandleringpoll(ctlr, op->pndx);
			unlock(ctlr);
			break;
		}
		unlock(ctlr);
	}
	poperror();
	lock(ctlr);      /* in case the intr happens while we are here */
	if (waserror()) {
		unlock(ctlr);
		nexterror();
	}
	pndx = op->pndx;
	if (pndx == -1) {
		/* signal that we won't ever touch op again and qreader should free it */
		op->pp = nil;
		/* and we can't trust the req struct after we return */
		op->r = nil;
		dprint("error: queue timeout in slrp driver ΔT=%ld p:%uld c:%uld l:%d\n",
			seconds() - submitted, ctlr->smb->producer, ctlr->smb->consumer, ctlr->lastack);
		error("queue timeout");
	}
	free(op);
	if (pndx == -2)
		nexterror();
	if (ctlr->rhndl[pndx].rdone == 0) {
		dprint("slrp operation timed out %d ΔT=%ld inuse:%d nblk:%d p:%uld c:%uld l:%d\n",
			pndx, seconds() - submitted, ctlr->rhndl[pndx].inuse, ctlr->rhndl[pndx].nblk,
			ctlr->smb->producer, ctlr->smb->consumer, ctlr->lastack);
		if (slrphandleringpoll(ctlr, pndx) < 0) {
			drive->unit->sectors = 0;		/* need to ident */
			dprint("slrp poll was unsuccessful %d\n", pndx);
			if (ctlr->rhndl[pndx].inuse == 0)
				dprint("attempting to decrement inuse past 0\n");
			else
				ctlr->rhndl[pndx].inuse--;
			unlock(ctlr);
			poperror();
			if (ctlr->rhndl[pndx].inuse == 0) {
				ctlr->rhndl[pndx].rdone = 1;
				wakeup(&ctlr->qrend);
			}
			poperror();
			sdsetsense(r, SDcheck, 6, 8, 1);
			return SDtimeout;
		}
	}
	if (ctlr->rhndl[pndx].inuse == 0)
		dprint("attempting to decrement inuse past 0\n");
	else
		ctlr->rhndl[pndx].inuse--;
	unlock(ctlr);
	poperror();
	if (ctlr->rhndl[pndx].inuse == 0)
		wakeup(&ctlr->qrend);

	if(ctlr->rhndl[pndx].status != 0) {
		dprint("ring status error %d:%x\n", pndx, ctlr->rhndl[pndx].status);
		poperror();
		return SDeio;
	}
	poperror();
	return SDok;
}

static int
slrprio(SDreq* r)
{
	Ctlr *ctlr;
	Drive *drive;
	SDunit *unit;
	int reqstatus, status;

	unit = r->unit;
	if((ctlr = unit->dev->ctlr) == nil || ctlr->drive == nil) {
		r->status = SDtimeout;
		dprint("slrprio cannot find drive");
		return SDtimeout;
	}
	drive = ctlr->drive;

retry:
	status = slrpgenio(drive, r);
	if(status == SDretry) {
		dprint("%s: retry\n", unit->name);
		goto retry;
	}
	if(status == SDok) {
		sdsetsense(r, SDok, 0, 0, 0);
	}
	else if(status == SDcheck && !(r->flags & SDnosense)) {
		dprint("got SDcheck; %s\n", unit->name);
		r->write = 0;
		r->cmd[0] = 0x03;
		r->cmd[1] = r->lun<<5;
		r->cmd[4] = sizeof(r->sense)-1;
		r->data = r->sense;
		r->dlen = sizeof(r->sense)-1;
		reqstatus = slrpgenio(drive, r);
		if(reqstatus == SDok) {
			r->flags |= SDvalidsense;
			sdsetsense(r, SDok, 0, 0, 0);
		}
	}
	r->status = status;
	return status;
}

static int
slrphandleringpoll(Ctlr *ctlr, int rndx)
{
	Drive *drive;
	Ring *rhndl;
	Ringentry *rentry;
	SDreq *r;
	int i;

	rentry = &ctlr->ring[rndx];
	if (rentry->status == 0xffffffff) {
		return -1; /* not done yet */
	}
	rhndl = &ctlr->rhndl[rndx];
	drive = ctlr->drive;
	if (rhndl->rdone == 0) {
		rhndl->status = rentry->status;
		rhndl->rdone = 1;
		for (i = 0; i < rhndl->nblk; ++i) {
			r = rhndl->r[i];
			if (r != nil) {
				r->rlen = rhndl->dcount[i] * drive->secsize;
				if (rhndl->pp[i])
					wakeup(&rhndl->pp[i]->sleep);
			}
		}
	}
	return 0;
}

static int
retirep(void *a)
{
	Ctlr *ctlr;

	ctlr = a;
	return ctlr->lastack != ctlr->smb->consumer;
}

static void
retire(void *a)
{
	Ctlr *ctlr;
	Slrpmailbox *smb;
	int cndx, lack, i;
	Drive *drive;
	Ring *rhndl;
	Ringentry *rentry;
	SDreq *r;
	uchar *p;
	ulong val;

	ctlr = a;
	smb = ctlr->smb;
	drive = ctlr->drive;

	while (1) {
		lock(ctlr);
		if (waserror()) {
			unlock(ctlr);
			continue;
		}
		while ((lack = ctlr->lastack) != (cndx = smb->consumer)) {
			ctlr->lastack = (ctlr->lastack + 1) % ctlr->ringlen;
			rhndl = &ctlr->rhndl[lack];
			if (rhndl->rdone == 0) {
				rhndl->rdone = 1;
			} else {
				dprint("*");
				//rhndl->inuse = 0;
				continue;   /* done by poll */
			}
			rentry = &ctlr->ring[lack];
			if ((val = rentry->status) != 0) {
				dprint("lack %x cndx %x %lux: ", lack, cndx, val);
				slrprentryprint(rentry);
			}
			rhndl->status = val;
			p = rhndl->buf;
			for (i = 0; i < rhndl->nblk; ++i) {
				r = rhndl->r[i];
				if (r != nil) {
					r->rlen = rhndl->dcount[i] * drive->secsize;
					if (r->data && !(r->write) && r->rlen <= 1024 * 1024) {
						memmove(r->data, p, r->rlen);
					}
				} else {
					dprint("lastack %x rhdnl %p no req\n", lack, rhndl);
				}
				p += rhndl->dcount[i] * drive->secsize;
			}
			rhndl->buf = nil;
			if (rhndl->inuse) {
				for (i = 0; i < rhndl->nblk; ++i) {
					if (rhndl->pp[i])
						wakeup(&rhndl->pp[i]->sleep);
					else {
						if (rhndl->inuse == 0)
							dprint("attempting to decrement insue past 0\n");
						else
							rhndl->inuse--;
					}
				}
			}
		}
		unlock(ctlr);
		poperror();
		if (!waserror()) {
			while (!retirep(ctlr))
				tsleep(&ctlr->retrend, retirep, ctlr, 250);
			poperror();
		}
	}
}

static void
slrpinterrupt(Ureg *, void *arg)
{
	Ctlr *ctlr;
	Slrpmailbox *smb;
	ulong irq;

	ctlr = (Ctlr*)arg;
	smb = ctlr->smb;
	irq = smb->irq;

	if (irq & Iring) {
		ctlr->done = 1;
		wakeup(&ctlr->retrend);
		irq &= ~Iring;
	}
		
	if (irq & Icmd)
		irq &= ~Icmd;

	if (irq & Istate)
		irq &= ~Istate;

	smb->irq = irq;
}

static void
slrpirqenable(Ctlr *ctlr)
{
	Slrpmailbox *smb;
	Pcidev *p;

	smb = ctlr->smb;
	smb->irq = 0;
	smb->irqmask = Iring | Icmd | Istate;
	p = ctlr->pcidev;
	intrenable(p->intl, slrpinterrupt, ctlr, ctlr->pcidev->tbdf, "slrp");
}

static int
fixbars(Ctlr *c)
{
	int i;
	struct {
		uintptr	bar;
		uintptr	size;
	} mem;
	Pcidev *p;

	p = c->pcidev;
	if(pcicfgr32(p, PciVID) != (0x5121<<16 | 0x1b52))
		return -1;

	for(i = 0; i < nelem(p->mem); i++){
		mem.bar = pcicfgr32(p, PciBAR0+4*i);
		mem.size = pcibarsize(p, PciBAR0+4*i);
		if(mem.bar != p->mem[i].bar ||
		mem.size != p->mem[i].size)
			goto fix;
	}
	c->msiaddrhi = pcicfgr32(p, 0x58);
	c->msiaddrlo = pcicfgr32(p, 0x54);
	c->msidata = pcicfgr32(p, 0x5c);
	return 0;

fix:
	for(i = 0; i < nelem(p->mem); i++){
		pcicfgw32(p, PciBAR0+4*i, p->mem[i].bar);
		mem.bar = pcicfgr32(p, PciBAR0+4*i);
		if(mem.bar == p->mem[i].bar)
			continue;
		return -1;
	}
	pcicfgw32(p, PciPCR, 0x00100147);
	pcicfgw32(p, 0x58, c->msiaddrhi);
	pcicfgw32(p, 0x54, c->msiaddrlo);
	pcicfgw32(p, 0x5c, c->msidata);
	pcicfgw8(p, 0x52, 0x81);
	pcicfgw8(p, PciINTL, c->irq);

	return 0;
}

static int
resetmb(Ctlr *c)
{
	int i;
	Slrpmailbox *m;

	if(fixbars(c) == -1)
		return -1;

	qlock(c);
	m = c->smb;
	for(i = 0;; i += 20){
		if(m->magic == Slrpmbmagic)
			break;
		if(i >= 2000){
			print("Invalid NVWC mailbox magic %.8lux\n", m->magic);
			qunlock(c);
			return -1;
		}
		delay(20);
	}
	slrpruncmd(c, Cinit);
	if (slrppollcmddone(c, Cinit) != 0) {
		qunlock(c);
		print("Cinit failed\n");
		return -1;
	}
	m->producer = c->producer = 0;
	m->irq = 0;
	m->irqmask = Iring | Icmd | Istate;
	qunlock(c);
	return 0;
}

static int
online0(Drive *d)
{
	vlong s;
	Sfis f;

	memset(&f, 0, sizeof f);
	if(slrpidentify(d->ctlr, d->info) == -1){
b1:
		d->unit->sectors = 0;
		d->sectors = 0;
		return -1;
	}
	s = idfeat(&f, d->info);
	if(s == -1)
		goto b1;
	d->sectors = s;
	d->secsize = idss(&f, d->info);
	return 0;
}

static int
slrponline(SDunit *unit)
{
	Ctlr *ctlr;
	Drive *drive;
	ushort *sp;
	uchar *p;
	int i;

	if(unit->sectors != 0)
		return 1;
	if((ctlr = unit->dev->ctlr) == nil || (drive = ctlr->drive) == nil
	|| resetmb(ctlr) == -1 || online0(drive) == -1)
		return 0;
	
	unit->inquiry[2] = 2;
	unit->inquiry[3] = 2;
	unit->inquiry[4] = sizeof(unit->inquiry)-4;
	p = &unit->inquiry[8];
	sp = &drive->info[Imodel];
	for(i = 0; i < 20; i++) {
		*p++ = *sp>>8;
		*p++ = *sp++;
	}

	p = (uchar*)drive->firmware;
	sp = &drive->info[Ifirmware];	
	for(i = 0; i < 4; i++) {
		*p++ = *sp>>8;
		*p++ = *sp++;
	}
	*p = 0;
	unit->sectors = drive->sectors;
	/*
	 * Normally, when a drive goes away, devsd will set the secsize to 0
	 * so we wouldn't have to worry about being so careful.  But here
	 * we are taking the drive away out from under devsd; it never knows
	 * the drive is gone.  As a result it continues to use secsize as a
	 * demoninator in divides and modulos, so letting it go to 0 is a
	 * bad thing.  At some later date, we should revisit this.
	 */
	if (drive->secsize)
		unit->secsize = drive->secsize;
	else if (unit->secsize == 0)
		unit->secsize = 512;
	return scsionline(unit);
}

static Drive *
slrpdrive(Ctlr *ctlr)
{
	Drive *drive;

	if((drive = malloc(sizeof(Drive))) == nil)
		return nil;
	drive->ctlr = ctlr;
	return drive;
}

static int 
slrpinit(Ctlr *ctlr)
{
	Slrpmailbox *smb;

	smb = ctlr->smb;

	resetmb(ctlr);			/* need ringlen */
	ctlr->ringlen = smb->ringlen;
	if(ctlr->ringlen == 0)
		ctlr->ringlen = 256;
	ctlr->ringpaddr = ctlr->smbpaddr + smb->ringoffset;
	ctlr->ring = vmappat(ctlr->ringpaddr, smb->ringlen * sizeof(Ringentry), PATWC);
	ctlr->rhndl = malloc(ctlr->ringlen * sizeof(Ring));

	ctlr->sglen = smb->sglen;
	ctlr->sgpaddr = ctlr->smbpaddr + smb->sgoffset;
	ctlr->sg = vmappat(ctlr->sgpaddr, smb->sglen * sizeof(Slrpsgentry), PATWC);
	return 0;
}

static void*
map(Pcidev *p, int bar, uint sz, uint flags)
{
	uintptr io;
	void *a;

	io = p->mem[bar].bar & ~0xf;
	if(flags != 0)
		a = vmappat(io, sz, flags);
	else
		a = vmap(io, sz);
	if(a == nil)
		print("sdslrp: can't vmap %#p\n", io);
	return a;
}

static void
unmap(void *a, uint sz)
{
	if(a != nil)
		vunmap(a, sz);
}

static SDev*
slrppnp(void)
{
	SDev* s;
	Pcidev *p;
	Ctlr *c;

	for(p = nil; p = pcimatch(p, 0x1b52, 0x5121);){
		if((p->mem[2].bar & ~0xf) == 0 || p->mem[2].size < Slrpbar1sz) {
			print("%T invalid bar addresses\n", p->tbdf);
			continue;
		}
		c = malloc(sizeof *c);
		if(c == nil)
			continue;
		s = malloc(sizeof *s);
		if(s == nil){
			free(c);
			continue;
		}
		c->pcidev = p;
		pcisetbme(p);
		s->ctlr = c;
		c->sdev = s;
		c->smbpaddr = p->mem[2].bar & ~0x0f;
		c->smb = map(p, 2, Slrpbar1sz, 0);

		c->sstate = mallocalign(ROUND(sizeof(Slrpstate), 8), 0x20, 0, 0);
		if (c->smb == nil || c->sstate == nil || slrpinit(c) < 0 || (c->drive = slrpdrive(c)) == nil) {
			unmap(c->smb, Slrpbar1sz);
			free(c->sstate);
			free(c);
			free(s);
			continue;
		}

		s->idno = 'S';
		s->ifc = &sdslrpifc;
		s->nunit = 1;

		sdadddevs(s);
	}
	return nil;
}

static int
slrpverify(SDunit *u)
{
	Ctlr *c;
	Drive *d;

	c = u->dev->ctlr;
	d = c->drive + u->subno;
	if(d->unit == nil)
		d->unit = u;
	scsiverify(u);			/* ignore result; xor hung? */
	return 1;
}

static int
slrpenable(SDev* sdev)
{
	Ctlr *ctlr;

	ctlr = sdev->ctlr;
	kproc("slrp", qreader, ctlr);
	kproc("slrp", retire, ctlr);
	ctlr->irq = ctlr->pcidev->intl;
	slrpirqenable(ctlr);
	return 1;
}

static int
slrpdisable(SDev *sdev)
{
	Ctlr *ctlr;

	ctlr = sdev->ctlr;
	intrdisable(ctlr->irq, slrpinterrupt, ctlr, ctlr->pcidev->tbdf, "slrp");
	return 0;
}

static int
slrprctl(SDunit* unit, char *p0, int l)
{
	char *p, *e;
	Ctlr *ctlr;
	Drive *drive;
	Slrpmailbox *smb;
	Slrpstate *s;

	if((ctlr = unit->dev->ctlr) == nil || ctlr->drive == nil)
		return 0;
	drive = ctlr->drive;

	qlock(ctlr);
	smb = ctlr->smb;
	smb->ringoffset = PADDR(ctlr->sstate);
	smb->ringlen = 0;
	smb->sglen = ROUND(sizeof(Slrpstate), 8);
	slrpruncmd(ctlr, Cgetpsmstate);
	slrppollcmddone(ctlr, Cgetpsmstate);
	qunlock(ctlr);

	p = p0;
	e = p+l;
	p = seprint(p, e, "config %4.4ux capabilities %4.4ux\n",
		drive->info[Iconfig], drive->info[Icapabilities]);
	if(drive->sectors) {
		p = seprint(p, e, "geometry %lld %d 0 0 0\n",
			drive->sectors, drive->secsize);
	}
	s = ctlr->sstate;
	p = seprint(p, e, "aoetarget %ud.%ud\n", s->shelf[0] | s->shelf[1]<<8, s->slot);
	p = seprint(p, e, "psmstate %lux\n", s->state);
	p = seprint(p, e, "pcistate %lux\n", s->pcistate);
	p = seprint(p, e, "mdstate %lux\n", s->mdstate);
	p = seprint(p, e, "sclvl %ux\n", s->sclvl);
	p = seprint(p, e, "boardtemp %ux\n", s->boardtemp);
	p = seprint(p, e, "poe0 %ux\n", s->poe0);
	p = seprint(p, e, "poe1 %ux\n", s->poe1);
	p = seprint(p, e, "firmware %s\n", drive->firmware);
	//if (nxfer > 0)
	//	p = seprint(p, e, "sectors per transfer %ulld %ulld %ulld\n", nsingles, nxfer, totsec / nxfer);
	p = seprint(p, e, "mac0 %E\n", s->mac0);
	p = seprint(p, e, "mac1 %E\n", s->mac1);
	p = seprint(p, e, "eccaddr %ux\n", s->eccaddr);
	p = seprint(p, e, "eccsbcnt %ux\n", s->eccsbcnt);
	p = seprint(p, e, "eccdbcnt %ux\n", s->eccdbcnt);
	p = seprint(p, e, "serial %s\n", s->serial);
	p = seprint(p, e, "rev %s\n", s->rev);

	return p-p0;
}

typedef struct Scmd Scmd;
struct Scmd {
	int	ncmd;
	int	arg;
	char	*cmd[8];
	int	scmd;
};
static Scmd cmdtab[] = {
//	2,	1,	{"verbose",	"idma", },	Cidmaverbose,
	2,	1,	{"verbose",	"ring", },		Cringverbose,
//	2,	1,	{"verbose",	"hmb", },		Chmbverbose,
	2,	0,	{"print",		"idma", },	Cpridma,
	2,	0,	{"print",		"pci", },		Cprpci,
	2,	0,	{"print",		"idma", },	Cpridma,
	2,	0,	{"print",		"cpu", },		Cprcpu,
	2,	0,	{"print",		"proc", },	Cprocdump,

	2,	0,	{"print",		"irqs", },		Cirqsdump,
	2,	0,	{"print",		"sdram", },	Csdramdump,
	2,	0,	{"print",		"xor", },		Cprxor,

	1,	0,	{"i2cdevtest",	},		Ci2cdevtest,
//	1,	0,	{"memmovetst",	},		Cxmemmovetst,
	1,	0,	{"reset",		},		Creset,
	1,	0,	{"sdrambackup",	},		Cbackup,
	1,	0,	{"sdramrestore",	},		Crestore,
};

static int
basiccmd(Ctlr *ctlr, Cmdbuf *cb)
{
	int i, j;
	ulong l;
	Scmd *s;

	for(i = 0;; i++){
		if(i == nelem(cmdtab))
			return -1;
		s = cmdtab + i;
		if(cb->nf != s->ncmd + s->arg
		|| cb->nf > nelem(s->cmd))
			continue;
		for(j = 0; j < s->ncmd; j++)
			if(strcmp(cb->f[j], s->cmd[j]) != 0)
				break;
		if(j == s->ncmd)
			break;
	}
	if(s->arg){
		l = strtol(cb->f[s->ncmd], 0, 0);
		if(l > 50000)
			error("invalid ring length");
		ctlr->smb->ringlen = l;		/* botch */
	}
	slrpruncmd(ctlr, s->scmd);
	slrppollcmddone(ctlr, s->scmd);
	if (s->scmd == Creset) {
		if (ctlr->drive != nil && ctlr->drive->unit != nil)
			ctlr->drive->unit->sectors = 0;
		identgone = 0;
		while(pcicfgr32(ctlr->pcidev, PciVID) != (0x5121<<16 | 0x1b52))
			tsleep(&up->sleep, return0, nil, 100);
	}
	return 0;
}

static int
slrpwctl(SDunit* unit, Cmdbuf* cb)
{
	Ctlr *ctlr;
	Ring *rhndl;
	Slrpmailbox *smb;
	int pndx;
	Ringentry *rentry;

	if((ctlr = unit->dev->ctlr) == nil || ctlr->drive == nil)
		return 0;

	smb = ctlr->smb;

	qlock(ctlr);
	if (waserror()) {
		qunlock(ctlr);
		nexterror();
	}
	if(basiccmd(ctlr, cb) == 0){
		/* all done */
	}
	else if(strcmp(cb->f[0], "debug") == 0) {
		if (strcmp(cb->f[1], "on") == 0)
			debugoff = 0;
		else
			debugoff = 1;
	}
	else if(strcmp(cb->f[0], "target") == 0) {
		char *s0, *s1;
		ulong maj, min;
		Ringentry lrentry;

		if(cb->nf != 2)
			error(Ebadctl);
		s0 = cb->f[1];
		if((s1 = strchr(cb->f[1], '.')) == nil)
			error(Ebadctl);
		s1[0] = 0;
		s1++;
		maj = strtoul(s0, 0, 0);
		min = strtoul(s1, 0, 0);
		if (maj >= 0xffff || min >= 0xff) 
			error("Invalid LUN id");
		lock(ctlr);
		if (waserror()) {
			unlock(ctlr);
			nexterror();
		}
		pndx = ctlr->producer;

		if (((pndx + 1) % ctlr->ringlen) == smb->consumer) 
			error(Enomem);

		rhndl = &ctlr->rhndl[pndx];
		memset(&lrentry, 0, sizeof(lrentry));
		rentry = &ctlr->ring[pndx];
		lrentry.sgnum = 0;
		lrentry.target = 0;
		lrentry.status = 0xffffffff;
		lrentry.feature = 0;
		lrentry.sectors = 1;
		lrentry.cmd = Csetfeatures;
		lrentry.lba[5] = 0x56;
		lrentry.lba[0] = min;
		lrentry.lba[1] = maj;
		lrentry.lba[2] = maj >> 8;
		memmove(rentry, &lrentry, sizeof(lrentry));
		coherence();

		rhndl->rdone = 0;
		rhndl->inuse = 1;
		rhndl->nblk = 1;
		rhndl->r[0] = nil;
		rhndl->pp[0] = up;
		ctlr->done = 0;
		ctlr->producer = (pndx + 1) % ctlr->ringlen;
		smb->producer = ctlr->producer;
		poperror();
		unlock(ctlr);
		if (up) {
			tsleep(&up->sleep, slrprdone, rhndl, 1000);
			if (rhndl->rdone == 0) {
				error(Etimedout);
			}
		} else {
			int i;
			for (i=0; i<10; i++) {
				delay(1000);
				if (ctlr->producer == smb->consumer)
					break;
			}
			if (ctlr->producer != smb->consumer) {
				error(Etimedout);
			} 
		}
		rhndl->inuse = 0;
	}
	else if(strcmp(cb->f[0], "mcdownload") == 0) {
		char *mcfn, *mcbuf;
		int i, n;
		ulong cs;
		Chan *ctl;

		mcfn = cb->f[1];
		if (mcfn == nil)
			error(Ebadctl);

		if ((mcbuf = mallocalign(1*MB, 0x20, 0, 0)) == nil) {
			print("failed to allocate buf\n");
			error(Enomem);
		}
		if(waserror()) {
			free(mcbuf);
			nexterror();
		}

		ctl = namec(mcfn, Aopen, OREAD, 0);
		if(waserror()) {
			print("read mc failed\n");
			cclose(ctl);
			nexterror();
		}
		n = devtab[ctl->type]->read(ctl, mcbuf, 1*MB, 0);
		cclose(ctl);
		cs = 0;
		for (i=0; i<n; i++) 
			cs += mcbuf[i];
		poperror();
		smb->ringoffset = PADDR(mcbuf);
		smb->ringlen = 0;
		smb->sglen = n;
		slrpruncmd(ctlr, Cupdatefw);
		slrppollcmddone(ctlr, Cupdatefw);
		poperror();
		free(mcbuf);
 	}
	else
		error(Ebadctl);

	qunlock(ctlr);
	poperror();
	return 0;
}

SDifc sdslrpifc = {
	"slrp",				/* name */

	slrppnp,			/* pnp */

	nil,			/* legacy */
	slrpenable,			/* enable */
	slrpdisable,			/* disable */

	slrpverify, 		// scsiverify,			/* verify */
	slrponline,			/* online */
	slrprio,				/* rio */
	slrprctl,			/* rctl */
	slrpwctl,			/* wctl */

	scsibio,			/* bio */
	nil,			/* probe */
	nil,			/* clear */
	nil,			/* rtopctl */
	nil,				/* wtopctl */

};
