#include <u.h>
#include <libc.h>
#include <ip.h>
#include <bio.h>
#include <ctype.h>
#include "aoe.h"

enum {
	Neth	= 16,
	Nio	= 32,
	Npkt	= 9000,
	Nethbad	= 16,
	Ndisc	= 7*1000,
	Nioto	= 10,
	Nrxto	= 2,

	ACata	= 0,
	ACqc,

	AFawr	= 1<<0,
	AFerr	= 1<<2,
	AFrsp	= 1<<3,
	AFaerr	= 1<<0,
	AFaext	= 1<<6,
};

typedef struct Io Io;
typedef struct Aoehdr Aoehdr;
typedef struct Aoeata Aoeata;
typedef struct Aoeqc Aoeqc;

struct Aoehdr {
	uchar	d[6];
	uchar	s[6];
	uchar	t[2];
	uchar	vf;
	uchar	err;
	uchar	shelf[2];
	uchar	slot;
	uchar	proto;
	uchar	tag[4];
};

struct Aoeata {
	Aoehdr;
	uchar	aflags;
	uchar	fea;
	uchar	scnt;
	uchar	cmd;
	uchar	lba[8];
	uchar	data[1];
};

struct Aoeqc {
	Aoehdr;
	uchar	bcnt[2];
	uchar	fwver[2];
	uchar	maxsec;
	uchar	ccmd;
	uchar	len[2];
	uchar	conf[1];
};

struct Io {
	uchar	tx[Npkt];
	int	ntx;
	uchar	rx[Npkt];
	int	nrx;
	ulong	tag;
	ulong	tick;
	Rendez;
};

static long startio(Aoedev *, void *, long, uvlong, int);
static void tagproc(void);
static void ioinit(void);
static void ifinit(int, char **);
static void tickproc(void);
static void ethinit(int, char **);
static void ethrxinit(void);
static void ethsend(Eth *, void *, long);
static void ioproc(Io *);
static int raoeio(Io *, Aoedev *, void *, long, uvlong, int);
static void ethopenall(void);
static void ethopen(char *);
static int isethopen(char *);
static int checklink(char *);
static void rxproc(Eth *);
static void discinput(Eth *, Aoehdr *, int);
static void taginput(ulong, Aoehdr *, int);
static void identrx(Aoedev *, uchar *);
static Aoedev *newdev(Eth *, Aoeqc *);
static void identtx(Eth *, Aoedev *, uchar *);
static void getidstr(char *, void *, int, int);
static void aoeshow(char *, void *, int);
static int Tfmt(Fmt *f);
static int Sfmt(Fmt *f);

static Eth	ethers[Neth];
static int	nethers;
static Aoeqc	bcquery;
static ulong	ticks;
static int	discovering;
static int	inited;
static Biobuf	ebio;
static int	upid;
static struct {
	QLock;
	Rendez	work;
	Rendez	user;
	int	fail;
	Io	io[Nio];
	int	nio;
	int	working;
	ulong	tick;

	int	mode;
	uvlong	off;
	ulong	len;
	ulong	rlen;	/* return length */
	ulong	xoff;	/* offset in a */
	uchar	*a;
	Aoedev	*dev;
} io;

void
prethers(void)
{
	Eth *p, *e;

	fprint(2, "nethers: %d\n", nethers);
	p = ethers;
	e = p + nethers;
	for (; p<e; p++)
		fprint(2, "%d %s %E %ld\n", p->fd, p->name, p->ea, p->iomax);
}

int
aoeinit(int neth, char *eth[])
{
	static QLock q, e;
	int pfd[2];

	Binit(&ebio, 2, OWRITE);
	ethinit(neth, eth);
	qlock(&q);
	if (inited) {
		qunlock(&q);
		return 0;
	}
	if (pipe(pfd) < 0) {
		qunlock(&q);
		return -1;
	}
	switch (rfork(RFPROC|RFMEM|RFNOTEG|RFFDG)) {
	case -1:
		close(pfd[0]);
		close(pfd[1]);
		qunlock(&q);
		return -1;
	case 0:
		close(pfd[0]);
		fmtinstall('T', Tfmt);
		fmtinstall('S', Sfmt);
		fmtinstall('E', eipfmt);
		tagproc();
		ioinit();
		tickproc();
		ethrxinit();
		//aoediscover();

		inited = 1;
		qunlock(&q);
		read(pfd[1], pfd, 1);
		postnote(PNGROUP, getpid(), "kill");
		exits(0);
	}
	close(pfd[1]);
	qlock(&q);	/* qlock passed to child, wait for child to complete init */
	qunlock(&q);
	return inited ? 0 : -1;
}

void
aoediscover(void)
{
	Eth *p, *e;
	Aoeqc *qh;

	qlock(&io);

	qh = &bcquery;
	memset(qh->d, 0xff, 6);
	hnputs(qh->s, 0x88a2);
	qh->vf = 0x10;
	hnputs(qh->shelf, 0xffff);
	qh->slot = 0xff;
	qh->proto = 1;

	if (io.working != 0 || io.len != 0) {
		qunlock(&io);
		return;
	}
	qunlock(&io);
	ndevs = 0;
	memset(devs, 0, sizeof devs);
	discovering = 1;
	p = ethers;
	e = p + nethers;
	for (; p<e; p++)
		if (checklink(p->name) == 0)
			ethsend(p, qh, sizeof bcquery);
	sleep(Ndisc);
	discovering = 0;
}

long
aoeread(Aoedev *d, void *a, long len, uvlong off)
{
	return aoeio(d, a, len, off, OREAD);
}

long
aoewrite(Aoedev *d, void *a, long len, uvlong off)
{
	return aoeio(d, a, len, off, OWRITE);
}

long
aoeio(Aoedev *d, void *a, long len, uvlong off, int mode)
{
	long rv;

	if (inited == 0 || d->length == 0)
		return -1;
	qlock(&io);
	if (io.working != 0 || io.len != 0) {
		qunlock(&io);
		fprint(2, "startio: phase error: working=%d len=%uld\n", io.working, io.len);
		return -1;
	}
	io.fail = 0;
	io.mode = mode;
	io.off = off;
	io.len = len;
	io.rlen = 0;
	io.xoff = 0;
	io.a = a;
	io.dev = d;
	io.tick = ticks;
	rwakeupall(&io.work);
	rsleep(&io.user);
	rv = io.rlen;
	qunlock(&io);
	return rv;
}

static void
tagproc(void)
{
	Io *p, *e;

	switch (rfork(RFPROC|RFMEM|RFNOWAIT)) {
	case -1:
		fprint(2, "failure initializing tagproc: %r\n");
		return;
	case 0:
		break;
	default:
		return;
	}
	for (;;) {
		sleep(1000);
		qlock(&io);
		p = io.io;
		e = p + io.nio;
		for (; p<e; p++) {
			if (p->tag == 0)
				continue;
			if (ticks - io.tick > Nioto)
				io.fail = 1;
			if (io.fail) {
				p->tag = 0;
				rwakeup(p);
			}				
			if (ticks - p->tick > Nrxto) {
				p->tick = ticks;
				aoerexmit++;
				write(io.dev->eth->fd, p->tx, p->ntx);
			}
		}
		qunlock(&io);
	}
}

static void
ioinit(void)
{
	Io *p, *e;

	io.work.l = &io;
	io.user.l = &io;
	p = io.io;
	e = p + Nio;
	for (; p<e; p++) {
		p->l = &io;
		switch (rfork(RFPROC|RFMEM|RFNOWAIT)) {
		case -1:
			fprint(2, "failure initializing ioproc: %r\n");
			return;
		case 0:
			ioproc(p);
			exits(0);
		}
		io.nio++;
	}
}

static void
ioproc(Io *p)
{
	uvlong len, off;
	int mode, n;
	uchar *a;
	Aoedev *d;

	qlock(&io);
loop:
	while (io.len == 0 || io.working >= io.dev->wnd || io.fail)
		rsleep(&io.work);
	mode = io.mode;
	d = io.dev;
	off = io.off;
	a = io.a + io.xoff;
	len = io.len;
	if (len > d->iomax)
		len = d->iomax;
	io.off += len;
	io.xoff += len;
	io.len -= len;
	io.working++;
	n = raoeio(p, d, a, len, off, mode);
	if (n != len) {
		io.fail = 1;
		io.len = 0;
	} else
		io.rlen += n;
	if (--io.working == 0 && (io.len == 0 || io.fail))
		rwakeup(&io.user);
	goto loop;
}

static void
putlba(uchar *p, uvlong lba)
{
	int i;

	for (i=0; i<6; i++) {
		p[i] = lba;
		lba >>= 8;
	}
}

uvlong
getlba(void *a)
{
	uchar *p;
	int i;
	uvlong lba;
	
	p = a;
	lba = 0;
	for (i = 0; i < 6; i++)
		lba |= (vlong)(*p++) << (8*i);
	return lba;
}

/* this function called with qlock(&io) held */
static int
raoeio(Io *p, Aoedev *d, void *a, long len, uvlong off, int mode)
{
	Aoeata *ah;
	ulong tag;
	static ulong nxtag;

	do {
		tag = nxtag++;
	} while (tag == 0);
	ah = (Aoeata *) p->tx;
	memset(ah, 0, offsetof(Aoeata, data[0]));
	memmove(ah->d, d->dst, 6);
	ah->vf = 0x10;
	hnputs(ah->shelf, TARG2SH(d->targ));
	ah->slot = TARG2SL(d->targ);
	hnputl(ah->tag, tag);
	ah->aflags = AFaext;
	putlba(ah->lba, off / 512);
	ah->scnt = len / 512;
	if (mode == OWRITE) {
		ah->aflags |= AFawr;
		ah->cmd = 0x34;
		memmove(ah->data, a, len);
		p->ntx = offsetof(Aoeata, data[0]) + len;
	} else {
		ah->cmd = 0x24;
		p->ntx = offsetof(Aoeata, data[0]);
	}
	p->nrx = 0;
	p->tick = ticks;
	p->tag = tag;
	qunlock(&io);
	ethsend(d->eth, p->tx, p->ntx);
	qlock(&io);
	while (p->tag != 0)
		rsleep(p);
	ah = (Aoeata *) p->rx;
	if (p->nrx == 0)
		return 0;
	if (ah->vf & AFerr)
		return 0;
	if (ah->fea & AFaerr)
		return 0;
	if (mode == OREAD)
		memmove(a, ah->data, len);
	return len;
}

static void
ethinit(int neth, char *eth[])
{
	int i;

	if (eth != nil) {
		for (i=0; i<neth; i++)
			ethopen(eth[i]);
	} else
		ethopenall();
}

static void
ethrxinit(void)
{
	Eth *p, *e;

	p = ethers;
	e = p + nethers;
	for (; p<e; p++)
		rxproc(p);
}

static int
file2ul(char *file, ulong *u)
{
	char buf[128];
	int fd, n;

	fd = open(file, OREAD);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof buf - 1);
	if (n < 0) {
		close(fd);
		return -1;
	}
	buf[n] = 0;
	*u = strtoul(buf, 0, 0);
	return 0;
}

static void
mymtu(ulong *mtu, char *ether)
{
	char buf[32], *p, *e;

	e = buf + sizeof buf;
	p = seprint(buf, e, "%s%s", *ether == '/' ? "" : "/net/", ether);
	seprint(p, e, "/maxmtu");
	if (file2ul(buf, mtu) < 0) {
		fprint(2, "warning: unable to fetch mtu for %s, default to 1500\n", ether);
		*mtu = 1500;
	}
	if (*mtu > Npkt)
		*mtu = Npkt;
}

static void
ethopen(char *ether)
{
	char *p;
	Eth *e;
	char buf[128];
	ulong mtu;

	if (ether == nil)
		return;
	e = ethers + nethers;
	if (nethers >= Neth) {
		fprint(2, "skipping ether %s, too many interfaces\n", ether);
		return;
	}
	p = strrchr(ether, '/');
	if (p)
		ether = p;
	if (isethopen(ether))
		return;
	if (myetheraddr(e->ea, ether) < 0) {
		// fprint(2, "unable to source ether address of %s: %r\n", ether);
		return;
	}
	mymtu(&mtu, ether);
	e->iomax = (mtu - offsetof(Aoeata, data[0])) & ~511;
	snprint(buf, sizeof buf, "%s!0x88a2", ether);
	e->fd = dial(buf, 0, 0, 0);
	if (e->fd < 0) {
		fprint(2, "unable to dial AoE on ether %s: %r\n", ether);
		return;
	}
	e->name = strdup(ether);
	nethers++;
}

static void
ethopenall(void)
{
	int i;
	char buf[128];

	for (i=0; i<Neth; i++) {
		snprint(buf, sizeof buf, "ether%d", i);
		ethopen(buf);
	}
}

static int
isethopen(char *ether)
{
	Eth *p, *e;

	p = ethers;
	e = p + nethers;
	for (; p<e; p++)
		if (strcmp(p->name, ether) == 0)
			return 1;
	return 0;
}

static int
checklink(char *ether)
{
	char *toks[128];
	int fd, i, n;
	char buf[8192], fname[32];

	snprint(fname, 32, "/net/%s/stats", ether);
	fd = open(fname, OREAD);
	if (fd < 0)
		return -1;
	n = read(fd, buf, 8191);
	buf[n] = 0;
	n = tokenize(buf, toks, 128);
	for (i = 0; i < n && strcmp(toks[i], "link:"); ++i) ;
	if (i >= n)
		return 0;
	if (atoi(toks[i+1]) == 0)
		return -1;
	return 0;
}

static void
ethsend(Eth *p, void *m, long len)
{
	uchar *src;
	long n;

	if (len < 60)
		len = 60;
	src = (uchar *) m + 6;
	memmove(src, p->ea, 6);
	hnputs(src + 6, 0x88a2);
	aoeshow("Sent", m, len);
	n = write(p->fd, m, len);
	if (n != len)
		fprint(2, "ethsend failure: %s %ld != %ld: %r\n", p->name, n, len);
}

static void
tickproc(void)
{
	switch (rfork(RFMEM|RFPROC|RFNOWAIT)) {
	case -1:
		fprint(2, "failure initializing tickproc: %r\n");
		return;
	case 0:
		break;
	default:
		return;
	}
	for (;;) {
		sleep(1000);
		ticks++;
	}
}

static void
rxproc(Eth *e)
{
	char pkt[Npkt];
	ulong tag;
	int n, bad;
	Aoehdr *h;

	switch (rfork(RFPROC|RFMEM|RFNOWAIT)) {
	case -1:
		fprint(2, "unable to start rx proc for %s: %r\n", e->name);
		return;
	case 0:
		break;
	default:
		return;
	}
	bad = 0;
	h = (Aoehdr *) pkt;
	for (;;) {
		n = read(e->fd, pkt, sizeof pkt);
		if (n < 0) {
			if (++bad > Nethbad) {
				fprint(2, "rxproc bailing on %s: %r\n", e->name);
				exits(0);
			}
			continue;
		}
		bad = 0;
		if (nhgets(h->t) != 0x88a2)
			continue;
		if ((h->vf & AFrsp) == 0)
			continue;
		aoeshow("Received", pkt, n);
		tag = nhgetl(h->tag);
		if (tag == 0) {
			if (discovering)
				discinput(e, h, n);
		} else
			taginput(tag, h, n);
	}
}

static void
discinput(Eth *e, Aoehdr *h, int)
{
	Aoeata *ah;
	Aoeqc *qh;
	Aoedev *d;

	ah = (Aoeata *) h;
	qh = (Aoeqc *) h;
	d = aoetarg2dev(SS2TARG(nhgets(h->shelf), h->slot));
	switch (h->proto) {
	case ACata:
		/* assume ident */
		if (d == nil)
			return;
		identrx(d, ah->data);
		break;
	case ACqc:
		if (d)	/* got it - thanks */
			return;
		d = newdev(e, qh);
		if (d == nil)
			return;
		identtx(e, d, qh->s);
	}
}

static void
taginput(ulong tag, Aoehdr *h, int len)
{
	Io *p, *e;

	qlock(&io);
	p = io.io;
	e = p + io.nio;
	for (; p<e; p++) {
		if (p->tag != tag)
			continue;
		memmove(p->rx, h, len);
		p->nrx = len;
		p->tag = 0;
		rwakeup(p);
		break;
	}
	qunlock(&io);
}

static void
identtx(Eth *e, Aoedev *d, uchar *dst)
{
	uchar pkt[Npkt];
	int n;
	Aoeata *ah;

	memset(pkt, 0, sizeof pkt);
	ah = (Aoeata *) pkt;
	memmove(ah->d, dst, 6);
	ah->vf = 0x10;
	hnputs(ah->shelf, TARG2SH(d->targ));
	ah->slot = TARG2SL(d->targ);
	ah->scnt = 1;
	ah->cmd = 0xec;
	n = offsetof(Aoeata, data[0]);
	n += d->iomax;
	ethsend(e, pkt, n);
}

static uvlong
lhgetv(uchar *p)
{
	int i;
	uvlong v;

	v = 0;
	for (i=7; i>=0; i--) {
		v <<= 8;
		v |= p[i];
	}
	return v;
}

static void
identrx(Aoedev *d, uchar *ident)
{
	memmove(d->ident, ident, 512);
	getidstr(d->model, ident, 27, Nmodel);
	getidstr(d->serial, ident, 10, Nserial);
	getidstr(d->firmware, ident, 23, Nfw);
	d->length = lhgetv(&ident[100*2]) * 512;	/* word 100 */
}

static void
getidstr(char *buf, void *ident, int beg, int len)  /* beg in 16-bit words, len in bytes */
{
	ushort w, *s;
	char *p;

	s = ident;
	p = buf;
	while (len > 0) {
		w = s[beg++];
		*p++ = w>>8;
		*p++ = (uchar)w;
		len -= 2;
	}
	while (--p > buf && *p == ' ')
		;
	*++p = 0;
}

static Aoedev *
newdev(Eth *e, Aoeqc *qh)
{
	Aoedev *d;
	ushort sh, sl;

	if (ndevs >= Ndevs) {
		fprint(2, "warning: out of devices for %d.%d\n", nhgets(qh->shelf), qh->slot);
		return nil;
	}
	d = &devs[ndevs++];
	sh = nhgets(qh->shelf);
	sl = qh->slot;
	d->targ = SS2TARG(sh, sl);
	d->nconf = nhgets(qh->len);
	memmove(d->conf, qh->conf, d->nconf);
	d->wnd = nhgets(qh->bcnt);
	d->iomax = qh->maxsec * 512;
	if (d->iomax > Niomax)
		d->iomax = Niomax;
	if (d->iomax > e->iomax)
		d->iomax = e->iomax;
	d->eth = e;
	memmove(d->dst, qh->s, 6);
	return d;
}

Aoedev *
aoetarg2dev(ulong targ)
{
	Aoedev *d, *e;

	d = devs;
	e = d + ndevs;
	for (; d<e; d++)
		if (d->targ == targ)
			return d;
	return nil;
}

static void
dump(void *ap, int len)
{
	uchar *p = ap;
	int i, j;
	
	while (len > 0) {
		Bprint(&ebio, "%p: ", p);
		j = len < 16 ? len : 16;
		for (i = 0; i < j; i++) 
			Bprint(&ebio, "%2.2x ", p[i]);
		while (i < 16) {
			Bprint(&ebio, "   ");
			i++;
		}
		Bprint(&ebio, " *");
		for (i = 0; i < j; i++)
			if (' ' <= p[i] && p[i] <= '~')
				Bprint(&ebio, "%c", (char)p[i]);
			else
				Bprint(&ebio, ".");
		while (i < 16) {
			Bprint(&ebio, " ");
			i++;
		}
		Bprint(&ebio, "*");
		Bprint(&ebio, "\n");
		len -= j;
		p += j;
	}
}

static void
aoeshow(char *msg, void *pkt, int len)
{
	Aoehdr *h;
	Aoeata *ah;
	Aoeqc *qh;
	int n, i;

	if (aoetrace == 0)
		return;
	Bprint(&ebio, "%s\n", msg);
	h = (Aoehdr *) pkt;
	Bprint(&ebio, "\tether(s=%E d=%E pr=%04x ln=%d)\n",
		h->s, h->d, nhgets(h->t), len);
	Bprint(&ebio, "\taoe(ver=%d flag=%c%c%c%c, err=%d %d.%d cmd=%d tag=%ux)\n",
		h->vf >> 4 & 0xf, 
		h->vf & 0x8 ? 'R' : '-',
		h->vf & 0x4 ? 'E' : '-',
		h->vf & 0x2 ? '1' : '0',
		h->vf & 0x1 ? '1' : '0',
		h->err, nhgets(h->shelf), h->slot, h->proto, nhgetl(h->tag));
	switch (h->proto) {
	case ACata:
		ah = (Aoeata *) h;
		Bprint(&ebio, "\taoeata(aflag=-%c-%c--%c%c errfeat=%02x ",
			ah->aflags & 0x40 ? 'E' : '-',
			ah->aflags & 0x10 ? 'D' : '-',
			ah->aflags & 0x02 ? 'A' : '-',
			ah->aflags & 0x01 ? 'W' : '-',
			ah->fea);
		Bprint(&ebio, "sectors=%d cmdstat=%02x lba=%,lld)\n",
			ah->scnt, ah->cmd, getlba(ah->lba));
		//dump(ah->data, len - offsetof(Aoeata, data[0]));
		break;
	case ACqc:
		qh = (Aoeqc *) h;
		Bprint(&ebio, "\taoeqc(bc=%d, fw=%04x sc=%d ver=%d ccmd=%d len=%d cfg=",
			nhgets(qh->bcnt), nhgets(qh->fwver), 
			qh->maxsec, qh->ccmd >> 4 & 0xf,
			qh->ccmd & 0xf, nhgets(qh->len));
		n = nhgets(qh->len);
		if (n > 32)
			n = 32;
		for (i = 0; i < n; i++)
			if (isprint(qh->conf[i]))
				Bprint(&ebio, "%c", qh->conf[i]);
			else
				Bprint(&ebio, "\\x%02x", qh->conf[i]);
		Bprint(&ebio, ")\n");
				
	}
	Bprint(&ebio, "\n");
	Bflush(&ebio);
}

static int
Tfmt(Fmt *f)
{
	int t;
	char buf[64];
	
	t = va_arg(f->args, int);
	if (t == -1)
		return fmtprint(f, "unset");
	snprint(buf, sizeof buf, "%d.%d", TARG2SH(t), TARG2SL(t));
	if (f->flags & FmtLeft)
		return fmtprint(f, "%-9s", buf);
	else
		return fmtprint(f, "%9s", buf);
}

static int
Sfmt(Fmt *f)
{
	uvlong sz;
	ulong m, g;
	char buf[64];
	
	sz = va_arg(f->args, uvlong);
	if (sz == -1)
		return fmtprint(f, "0");
	m = sz / (1000*1000);
	g = m / 1000;
	m %= 1000;
	snprint(buf, sizeof buf, "%5ld.%03ldGB", g, m);
	if (f->prec)
		return fmtprint(f, "%*s", f->prec, buf);
	else
		return fmtprint(f, "%s", buf);	
}

void
aoestop(void)
{
	Io *p, *e;

	qlock(&io);
	io.fail = 1;
	p = io.io;
	e = p + io.nio;
	for (; p<e; p++) {
		p->tag = 0;
		rwakeup(p);
	}
	rwakeupall(&io.work);
	qunlock(&io);
}
