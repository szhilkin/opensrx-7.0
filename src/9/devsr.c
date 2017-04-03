/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */

#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"pool.h"
#include	"../ip/ip.h"
#include	"../port/error.h"
#include	"../port/netif.h"
#include	"aoe.h"
#include	"ec.h"
#include	"etherif.h"
#include	"../port/iofilter.h"
#include	<ctype.h>
#include	<rawrd.h>

#define errorstr errorf
#define dprint(...)	if(debugoff) USED(debugoff); else print(__VA_ARGS__)
#define tprint(...)	if(traceoff) USED(traceoff); else print(__VA_ARGS__)
#define iskaddr(a)	((uintptr)a >= KZERO)
#define	TKDELTA2MS(x)	TK2MS(Ticks - (x))

#define DEVBAD(d) ((d)->flags & (Dfailed|Drepl|Dmissing))
#define BUFBAD(b) ((b)->d == nil || (b)->flags & (Binval|Bfailed|Bghost))
#define BUFBUSY(bp) ((bp)->flags & Bio || (bp)->ref > 0)

#define BLK2AUX(bp) ((Blkaux*) ((bp)->auxspc))

#define Lba28max 0x0FFFFFFFLL
#define Lba48max 0x0000FFFFFFFFFFFFLL

#define DEVF "devfault"
#define LBMODEL "Coraid EtherDrive Storage"

#define Ebadmac "invalid mac"
#define Ebadmagic "invalid magic"
#define Ebadluncfg "invalid lun config"
#define Ebadraid "invalid raid type"
#define Enoctl "unrecognized ctl"
#define Eusage "usage"

/*
 * Magic is used to indicate changes in layout.  By default we use
 * the most recent magic for new LUNs.  Changes to this heuristic
 * should be considered carefully as it impacts customers directly.
 */

#define Srmagic00 "coraid00"
#define Srmagic01 "coraid01"

/*
 * Globals are defined throughout the file.  Code segments
 * are demarcated by CSS.  References between comments
 * and code they describe are with references set in parens.
 * Just search for the reference string.
 */

enum {
	/* defaults */
	Nshelfunset= -1,
	Nbcachepercent= 90,
	Nrdevs= 36,
	Nraids= 4,
	Nbsize= 64*1024,
	Nbskip= 1,			/* interleave on Nbskip*Nbsize boundaries */
	Nstride= 64*1024,
	Npathasz= 16*1024,
	Nnets= 16,
	Nmacs= 64,
	Nsrr= 2048,
	Nmaxscnt= 16,

	Nspares= 16,
	Ncaches = 16,
	Ncblocks = 12*(1<<20),          /* 12 million blocks */
	Nelvpass= 512,
	Nvlanmax= 4094,
	Ngiveup = 100000,	// getbuf attempts before going Postal Service

	/* below are the primary performance tunables */

	/* these are carefully hand selected.  know what you're changing. */
	Nhwidth= 8,
	Nhbufs= 47,
	Nbufs= Nhbufs * Nhwidth,

	Npglh= 64,
	Npreget= 1,
	Ndp= 16,
	Nbufcntperdisk= 16,
	Nlbwperdisk= Nbufcntperdisk,
	Nbufcntmax= 512,
	Nlbwmax= Nbufcntmax+Nlbwperdisk,
	Nrlhash= 511,
	Nrlrate= 128*1024*1024,		/* 128MB/s rate limit on recon/init when lun in use */

	Nelvdelaygap= 128*1024,
	Nelvdelayms= 2,
	Nelvread= 32,			/* 2:1 reads:writes */
	Nelvwrite= 16,
	Nelvdread= 16,			/* 1:1 reads:writes */
	Nelvdwrite= 16,
	Nelvdeadms= 500,
	Niodeadsecs= 35,
	Niosvchash= 511,
	Ndpintrthresh= 5,		/* at this intr load, migrate dproc group to another core */
	Niovec= 1,

	/* do not change these.  ever. */
	Nlunlabelsz= 16,
	Nmodelsz= 40,
	Nfwversz= 8,
	Nserialsz= 20,
	Nconflen= 1024,
	Nsavefailed= 3,
	Nmagic= 8,

	FWV= 0xace0,

	/* buf flags */
	Bread= 1<<0,
	Bmod= 1<<1,
	Bimm= 1<<2,
	Bghost= 1<<3,
	Bfailed= 1<<4,
	Bio= 1<<5,
	Bsync= 1<<6,
	Brecon= 1<<7,
	Brefreshed= 1<<8,
	Bwrite= 1<<9,
	Bpreget= 1<<10,
	Binval= 1<<11,

	/* lun flags */
	Lnobuf= 1<<0,
	Lcached0= 1<<1,
	Lcached1= 1<<2,
	Lfnil= 1<<3,
	Lnoguard= 1<<4,
	Lflushcache= 1<<5,

	/* raid flags */
	Rclean= 1<<0,
	Rdegraded= 1<<1,
	Rfailed= 1<<2,
	Rrecovering= 1<<3,
	Riniting= 1<<4,
	Rscrubbing= 1<<5,
	Rstop= 1<<6,
	Rneedinit= 1<<7,
	R2degraded= 1<<8,

	/* dev flags */
	Dclean= 1<<0,
	Dfailed= 1<<1,
	Drepl= 1<<2,
	Dmissing= 1<<3,
	Dnospare= 1<<4,
	Dspare= 1<<5,
	Dreplacing= 1<<6,
	Dcache= 1<<7,
	Dcount= 8,

	Kdie= 1<<0,
	Kckdb= 1<<1,
	Kdp= 1<<2,

	/* aoe cmd extensions */
	ACEmask= 0xf0,
	ACEredirect= 0xf1,

	/* rrkey access checking */
	Aread= 0,
	Awrite,
	Ares,

	/* lun cache aligned stats */
	LCaslbw	= 0,
	LCasoverlap,
	LCasiodup,
	LCasoflow,
	LCaselvwait,
	LCaselvq,
	NLcas,

	/* Net states */
	SNfree	= 0,
	SNiniting,
	SNactive,
	SNclosing,

	/* cache */
	Ncache = 3,

	ETsuper = 0xff,
	ETcachemeta = 0xfe,
	ETunknown = 0,
	ETdirect = 1,
	ETdata,
	ETmaxuser = 0x7f,

	EFvalid = 1<<8,
	EFdirty = 1<<9,
	EFnf = 1<<10,
	EFrdbuf = 0<<11,		/* strictly a read buffer; writes invalidate */
	EFaside = 1<<11,		/* treat as a write-redundant buffer instead of full cache */
	EFwback = 2<<11,		/* normal write-back cache */
	EFwthrough = 3<<11,		/* normal write-through cache */
	EFctype = 3<<11,
	EFauto = 1<<13,			/* automatically use the next cache level for backing I/O */
	EFbypass = 1<<14,		/* use the bypass list */
	EFquick = 1<<15,		/* always use quick-start in bypass */

	/* elv states */
	ESread = 0,
	ESwrite,

	/* elv iomodes */
	EMspin = 0,		/* spin: sort i/o to optimize heads until deadline */
	EMssd,			/* ssd: always perform in fifo mode */

	/* emulate lun flags */
	EE4kn = 1<<0,
};

typedef struct Ataregs Ataregs;
typedef struct Blkaux Blkaux;
typedef struct Buf Buf;
typedef struct Castat Castat;
typedef struct Elv Elv;
typedef struct Elvgrp Elvgrp;
typedef struct Elvlist Elvlist;
typedef struct Iosvc Iosvc;
typedef struct Lun Lun;
typedef struct Net Net;
typedef struct Raid Raid;
typedef struct Rdev Rdev;
typedef struct Rlcache Rlcache;
typedef struct Rowlock Rowlock;
typedef struct Srconfig Srconfig;
typedef struct Srlconfig Srlconfig;

struct Elvlist {
	Elv *head;
	Elv *tail;
};

struct Elv {
	Lock;
	Rdev *d;
	int mode;
	ulong tout;
	uchar *db;
	ulong len;
	ulong res;
	uvlong off;
	Buf *bp;

	ulong inserttk;
	Proc *head;		/* elvwait list */
	Proc *tail;
	Elv *ionext;		/* read/write io list */
	Elv *ioprev;
	Elv *dnext;		/* deadline list */
	Elv *dprev;
	Elv *vnext;		/* for group i/o, multidisk scatter */
	uvlong gen;		/* general use value */
	int done;
};

struct Elvgrp {
	Elv *head, *tail;
};

struct Buf {
	Elv;
	Lock *clk;		/* cache line lock */
	Buf *next;
	uvlong bno;
	uvlong lastbno;
	ushort flags;
	ushort ref;
	ulong getts;
	ulong putts;
	ulong modts;
	ulong tss;		/* time stamp sequence - for buffers stamped in same µs */
	long dbidx;
};

struct Rdev {
	Chan *c;		/* channel we've opened to communicate with disk */
	int flags;
	long ndp;		/* child proc count */
	int dno;		/* disk number of raid (derivable, but here for ease) */
	int badspare;
	int savefailed;		/* save config failed */
	int failfast;		/* set when drive IO exceeds threshold */
	uvlong length;		/* total raid-useable size */
	int rm;
	Raid *raid;		/* who's your daddy? */
	char rdpath[16];	/* eg, #‡/0 */
	char path[64];		/* eg, /raiddev/0/data */
	char name[64];		/* eg, 21.0 */
	int slot;		/* eg, slot 2 in 21.2 */
	Iofilter filter;
	uvlong row;		/* rebuild row */
	int dpmachno;

	/* buffer cache */
	uvlong pregetlh[Npglh];	/* preget lookaside hash */
	Lock buflks[Nhbufs];	/* one per hash line */
	Buf bufs[Nbufs];

	/*
	 * [rw]head holds the full read/write i/o lists, sorted by offset.
	 * 
	 * ahead holds the active i/o list, sorted by offset.
	 * 
	 * every node in [rw]head also exists in dhead, sorted by
	 * deadline timeout.
	 */
	Lock elvlock;
	Proc *dp;
	Elvlist dlist;
	Elvlist rlist;
	Elvlist wlist;
	Elv *ahead;
	int writing;
	int reading;
	int closed;
	int dpworking;
	ulong deadraised;
	ulong rrandom;
	ulong wrandom;
	uvlong sectors;
	ulong secsize;
	ulong physecsize;
	ushort rpm;
	ushort iomode;
	ushort elvmode;
	uvlong lastroff;	/* end of last read request serviced for seq detection */
	uvlong lastwoff;	/* end of last write request serviced for seq detection */

	Castat *tss;
};

typedef enum Rtype {
	RTfnil= -5,
	RTnil= -4,
	RTraw= -3,
	RTjbod= -2,
	RTraidl= -1,
	RTraid0= 0,
	RTraid1= 1,
	RTraid5= 5,
	RTraid6rs= 6,
	RTraid10= 10,
} Rtype;

struct Raid {
	RWSlock;
	int flags;
	int rno;
	Lun *lblade;		/* who's your daddy? */
	uvlong length;		/* of entire raid */
	uvlong mindlen;		/* of a single disk in raid (for replacement) */
	uvlong doff;		/* device offset (cf. raidsizes) */
	uvlong roff;		/* rebuild offset */
	Rtype type;
	int ndevs;		/* number of devices */
	Rdev *devs;
	ulong lastaccess;
	ulong lastscrub;
	Iofilter filter;
	long nrecovery;		/* number of ongoing recoveries */
};

struct Rowlock {
	Rowlock *next;
	Rlcache *cache;
	Raid *r;
	uvlong row;
	int ref;
	void *aux;
	IOLock;
};

struct Rlcache {
	Lock;
	Rowlock *free;
	Rowlock *hash[Nrlhash];
};

typedef enum Lstate {
	Loff,
	Lon,
} Lstate;

struct Lun {
	RWSlock;
	Ref;
	Lun *next;
	int slot; 
	int nraids;
	Raid raids[Nraids];
	uvlong length;
	ulong secsize;
	ushort physecsize;
	ushort rpm;
	Lstate line;
	uchar macs[Nmacs*6];		/* mask macs */
	int nmacs;
	uchar srr[Nsrr*6];		/* reserve/release macs */
	int nsrr;
	int anyread;			/* macs not in srr list can read */
	ushort vlan;
	uchar flags;
	uchar fcpri;
	uchar fcminpct;
	char magic[Nmagic];
	int hasmagic;
	int ver;
	int nconfig;
	char config[Nconflen];		/* config may contain nulls, so we need nconfig */
	char model[Nmodelsz + 1];	/* These are all strings, so we need an extra byte */
	char fwver[Nfwversz + 1];
	char serial[Nserialsz + 1];
	char lunlabel[Nlunlabelsz + 1];
	Iofilter rfilt, wfilt;
	int bufcnt;
	int soff;
	uint fsunit;			/* unique id for namespace access */
	int zerocfg;			/* rmlb/ref removal side effect */
	long nworker;			/* number of lbworker procs */
	Lock iosvclock;
	Iosvc *iosvchash[Niosvchash];
	Iosvc *iosvcfree;
	Lock iolock;
	Proc *iop;
	Block *iohead, *iotail;
	int closed;
	int iocnt;
	int iolim;
	ulong emuflags;         /* emu flags facing initiator */
	Rlcache rlcache;

	Castat *castats;		/* cache aligned stats for atomic ops */
};

enum {
	Sfree,
	Sinuse,
	Sdying,
	Sdead,
};

struct Net {
	long ref;
	Net *next;
	Chan *dc;
	uchar ea[6];
	uchar maxscnt;
	uchar state;
	Chan *cc;
	XQueue *inq;
	XQueue *outq;
	Proc *outproc;
	Lock outlk;
	char path[58];
};

struct Srconfig {
	char magic[Nmagic];
	uchar length[2];
	uchar sectoralign[512 -Nmagic -2];
	char config[Nconflen];
};

/*
 * The prior version of the SRX code accidentally
 * used long arithmetic on the 2 byte length in the structure and
 * quietly worked because zero byte padding followed the length
 * element.  Length is increased to 4 for backwards compatibility.
 */
struct Srlconfig {
	char magic[Nmagic];
	uchar length[4];
	uchar serial[Nserialsz];
	uchar lunlabel[Nlunlabelsz]; 
	uchar vlan[2];
	uchar flags;
	uchar fcpri;
	uchar fcminpct;
	uchar sectoralign[512 -Nmagic -4 -Nserialsz -Nlunlabelsz -2 -1 -1 -1];
	char config[Nconflen];
};

struct Ataregs {
	uchar cmd;	/* input */
	uchar sectors;
	uchar feature;

	uvlong lba;	/* both */

	uchar err;	/* output */
	uchar status;
};

struct Iosvc {
	Iosvc *next;
	Block *bp;
	Aoeata *ah;
	int hash;
	int targ;
	uvlong start;
	uvlong end;
};

struct Blkaux {
	Net *net;
	Iosvc *svc;
	void (*freefn)(Block*);
	Lun *lb;	/* provide Block to Lun mapping */
};

/* cache aligned stat for atomic ops */
struct Castat {
	long	stat;
	uchar	pad[CACHELINESZ - sizeof (long)];
};

/*
 * add functions at the top/bottom, highlight the below section in
 * its entirety, then middle click highlight the following pipe
 * and sort line.
 * | sort -d +2
 */
static void addcache(char *disk, char *percent);
static void attachcache(char *, char *, char *);
static long blbrstat(Chan *, Lun *, void *, long, uvlong);
static uvlong bno2std(uvlong);
static int bpolder(Buf *, Buf *);
static long bufstats(void *, ulong);
static void buildparity2(void *);
static void buildparity(void *);
static void cacheflags(int, int);
static void cacheprio(Lun *,int, char *, char *);
static int candirect(Raid *);
static int cansave(Raid *);
static int cansavelun(Lun *);
static int ckonoff(char *);
static int creadonce(char *, void *, int);
static void defunct(Rdev *);
static int devflush(Rdev *);
static void rdevgeometry(Rdev *);
static Rdev *dmirror(Rdev *);
static void dpmachno(Rdev *);
static void dpswitchck(Rdev *);
static void dropcache(void);
static int drw(Lun *, void *, long, uvlong, int);
static char *dstate(Rdev *, int save);
static void dstatechg(Rdev *);
static void dstateinit(void);
static long dstateread(void *, long);
static void dsync(Rdev *);
static void ecreplay(int);
static void emulate4kn(Lun *, int);
static void fail(Lun *, char *);
static void faildev(Rdev *d);
static void failpath(char *);
static void fastfaildevices(Raid *);
static void fcache(char *);
static void fcache0(char *);
static long fcbackio(int, void *, long, vlong, int, int);
static void filterproc(void *);
static void flushraid(Raid *, int);
static long fnv1al(char *);
static void formatcache(char *, char *, char *, char *, char *);
static void formatcachex(int, char *, int, vlong, vlong);
static char *getcookie(void);
static int getdf(Raid *, Rdev **, uvlong);
static long getiomode(Lun *, void *, long);
static uvlong getlba(Aoeata *);
static void getlun(Lun *);
static long getlunlabel(Lun *, void*, long);
static long getmodel(Lun *, void *, long);
static long getndp(Lun *);
static void getnet(Net *);
static long getres(Lun *, void *, long);
static long getserial(Lun *, void *, long);
static long getvlan(Lun *, void *, long);
static void grow(Lun *, int, char *argv[]);
static void iodraindev(Rdev *, int);
static int isdirect(Raid *, int);
static int islbfailed(Lun *);
static int isparity(Rdev *, uvlong);
static int isredundant(Raid *);
static void kickthespares(void *);
static int krloadrr(Lun *);
static int krloadrr0(Lun *);
static int krreset(Lun *);
static int krreg(Lun *, Kreg *);
static int krset(Lun *, Kset *);
static int krstat(Lun *, Block *);
static void lbraid(int, char *argv[]);
static long lbrstat(Lun *, void *, long);
static long lbsetlunlabel(Lun *, void *, long);
static long lbsetserial(Lun *, void *, long);
static long lbsetvlan(Lun *, void *, long);
static long lbstat(Lun *, void *, long);
static void lbworker(void *);
static void loadlun(Lun *, int);
static int lrw(Lun *, void *, long, uvlong, int, int);
static void lsync(Lun *);
static void flushcache(Lun *lb, int on);
static void makedev(Rdev *, char *);
static void markcleanall(void);
static void mask(Lun *, char *);
static void netinput(void *, Block *);
static char *netname(Net *);
static void netreader(void *);
static Block *netread(void);
static void netwriter(void *);
static uvlong nextbno(Rdev *, uvlong);
static void nextfreeb(Next *);
static void preget(Rdev *, uvlong);
static uvlong prevbno(Rdev *, uvlong);
static void putlun(Lun *);
static void putnet(Net *);
static int raidshield(Raid *, uvlong);
static int raidshield0(Raid *, uvlong);
static int rbufio(Elv *);
static int rbufiotime(Elv *, IOchunk *, int, ulong *);
static long rdmasks(Lun *, void *, long);
static char *readwrite(int);
static void replace(Lun *, char *, char *);
static void resetlun(Lun *);
static void resetmodelfw(Lun *);
static void setlbgeometry(Lun *);
static void idputgeometry(Lun *, ushort *);
static void resetserial(Lun *);
static void resetts(Rdev *, ulong);
static void restorecache(char *disk);
static int rgetbuf(Rdev *, uvlong, Buf **, int);
static long rlbctl(Lun *, void *, long);
static long rlbioq(Lun *, void *, long);
static long rlbrowlocks(Lun *, void *, long);
static void rmfcache(int, char *[]);
static void rmfcache0(char *);
static void rmlb(Lun *, int, char **);
static void rmmask(Lun *, char *);
static void rmnl(char *);
static void rmspare(int, char *argv[]);
static long rootctl(void *, long);
static long rootheap(void *, long);
static long rootios(void *, long);
static long rootstat(void *, long);
static Chan *rrfsopen(int, char *, int);
static int rrok(Lun *, uchar *, int);
static void rsr6_tabinit(void);
static void rsync(Raid *);
static char *rt2s(Rtype);
static void savelun(Lun *);
static void save(void);
static void scanproc(void *);
static void schedsave(void);
static void scrub(void *);
static void setbaux(Block *, Net *, Lun *);
static void setcache(Lun *, int, int);
static void setdevslotattrs(Rdev *);
static long setiomode(Lun *, void *, long);
static void setline(Lun *, Lstate);
static long setlunlabel(Lun *, void *, long);
static long setmodel(Lun *, void *, long);
static void setnobuf(Lun *, int);
static void setnoguard(Lun *, int);
static long setres(Lun *, void *, long);
static void setrflags(Raid *);
static void setrrfs(char *);
static long setserial(Lun *, void *, long);
static long setvlan(Lun *, void *, long);
static long showfcache(void *, long);
static long showspares(void *, long);
static int sigvalid(void);
static int skipbno(Rdev *, uvlong);
static int skipmirror(Rdev *, uvlong);
static void spare(char *);
static long srctl(void *, long);
static void srinit(void);
static void startether(char *);
static uvlong std2bno(uvlong, uvlong);
static void stopallether(void);
static void stopether(char *);
static void sync(void);
static void tryfail(Rdev *);
static void undbuf(Buf *);
static void undbuf0(Buf *);
static void unusepath(char *);
static int usepath(char *);
static long wlbctl(Lun *, void *, long);
static void wonkdev(Rdev *, int);
static int validate(char *mac);
static int shelfslotfmt(Fmt *fmt);
static int parsedrive(char *);
static int ckpath(char *);
extern Dev srdevtab;

static int asmxor;
static int buildparityloop;
static int debugoff = 1;
static int traceoff = 1;
static int dirtysecs = 5;
static int maxdsecs = 30;
static Ref scanref;
static int nbsize = Nbsize;
static int nbskip = Nbskip;
static int npreget = Npreget;
static ulong rlrate = Nrlrate;
static int shelf = Nshelfunset;
static int directio = 1;
static int syncio;
static int scrubrl = 1;
static long ndprocs = Ndp;
static int iosamp = 1;
static int parityprespec = 0;
static ulong pgrows = 16;
static ulong paritypg = 0;
static ulong raidpg = 0;
static ulong raidpgp = 1;
static ulong raidpgpeek;
static ulong rgbmodbias = 1;
static int raidresize;
static ulong nelvread = Nelvread;
static ulong nelvwrite = Nelvwrite;
static ulong nelvdread = Nelvdread;
static ulong nelvdwrite = Nelvdwrite;
static ulong nelvdeadms = Nelvdeadms;
static ulong niodeadsecs = Niodeadsecs;
static ulong rspdelayms;
static int ecpresent;
static int ecattached[Ncache];
static int ecblk;
static uchar *cachelist;
static Ref units;
static Castat *gcastats;
static ulong getbufwaits;
static ulong getbufs;
static ulong pregetbiomods;
static ulong pregetbios;
static ulong pregethits;
static ulong pregetlinefulls;
static ulong nomatches;
static ulong noputdbufs;
static ulong linefulls;
static ulong rlinefulls;
static ulong giveups;
static int ngiveup = Ngiveup;
static ulong devreads;
static ulong devwrites;
static ulong bpmodio;
static ulong bpmodioR;
static ulong bpmodioW;
static ulong bpfltoosoon;
static ulong cachehits;
static ulong collisions;
static ulong deadraised;
static ulong elvdelayms = Nelvdelayms;
static ulong elvdelaygap = Nelvdelaygap;
static ulong nbufcnt;
static ulong bufcntmax = Nbufcntmax;
static ulong bufcntperdisk = Nbufcntperdisk;
static ulong lbwperdisk = Nlbwperdisk;
static ulong lbwmax = Nlbwmax;
static ulong dpintrthresh = Ndpintrthresh;
static ulong niovec = Niovec;

static long dprocactive;
static long lbwactive;

static uchar bcastmac[6] = "\xff\xff\xff\xff\xff\xff";

static Srconfig zeroconfig;
static Rendez scanrendez;

static uchar gflog[256];
static uchar gfilog[256];
static uchar gfinv[256];
static uchar gfexi[256];
static uchar gfmul[256][256];
static ulong ecseqio;
static ulong ecrandio;
static int lookback=3;

static uchar Konekey[8] = "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF";

static struct {
	QLock;
	Chan *rootc;
	char mnt[64];
	char buf[16384];
	char *arg[2048];
} rrfs;

static struct {
	RWlock;
	Net *head;
} nets;

static struct {
	RWSlock;
	Lun *head;
} lblades;

static struct {
	QLock;
	int ndevs;
	Rdev devs[Nspares];
} spares;

static struct {
	QLock;
	int ndevs;
	Rdev devs[Ncaches];
} caches;

#define TYPE(qid) ((ulong) (qid).path & Masktype)
#define UNIT(qid) (((ulong) (qid).path >> Shiftunit) & Maskunit)
#define QID(unit, type) (((unit) << Shiftunit) | (type))
#define DC srdevtab.dc

enum {
	Qtopdir= 1,
	Qbase,
	Qctl= Qbase, Qstat, Qios, Qdstate, Qspares, Qbufstats, Qecchk, Qcachelist, Qfcache, Qtopend,

	Qlbdir,
	Qlbbase,
	Qlbctl= Qlbbase, Qlbstat, Qlbrstat, Qlbmasks, Qlbiomode, Qlbconfig, Qlbmagic, Qlbmodel, Qlbserial, Qlbdata, Qlbiostats, Qlblunlabel, Qlbvlan, Qlbrls, Qlbioq, Qlbend,

	Qtopfiles= Qtopend - Qbase,

	/*
	 * LogN is the Log₂(maximum number of N values),
	 * or the # bits to reserve for N in qid.path:
 	 * [    Unit [31-8]    |  Type [7-0] ]
	 */
	Logtype= 8,
	Masktype= (1<<Logtype)-1,
	Logunit= 32-Logtype,
	Maskunit= (1<<Logunit)-1,
	Shiftunit= Logtype,
};

static char *dstatename[Dcount] = {
	"clean",
	"failed",
	"repl",
	"missing",
	"nospare",
	"spare",
	"replacing",
	"cache",
};

/*
 * must match cmd/src/slotled.c coros/src/9/port/ibpi.c
 * botch: should be able to mak disk states -> led states
 */
static char *ledactions[Dcount] = {
	"reset",		/* Dclean; and default */
	"fault",		/* Dfailed */
	"rebuild",		/* Drepl */
	nil,			/* Dmissing */
	"reset",		/* Dnospare */
	"spare",		/* Dspare */
	"reset",		/* Dreplacing */
	"reset",		/* Dcache */
};
static uint ledenabled = ~Dspare;	/* blinking spare disliked */

/* CSS begin misc */

#pragma varargck type "S" Rdev* 
static int
shelfslotfmt(Fmt *fmt)
{
	Rdev *r;
	
	r = va_arg(fmt->args, Rdev *);
	return fmtprint(fmt, "shelf:n:%d slot:n:%d", shelf, r->slot);
}

static void
srinit(void)
{
	char *p;

	fmtinstall('Z', filterfmt);
	fmtinstall('S', shelfslotfmt);
	fmtinstall('H', encodefmt);

	if (p = getconf("*srndprocs"))
		ndprocs = strtol(p, 0, 10);
	if (p = getconf("*srlbwmax"))
		lbwmax = strtol(p, 0, 10);
	if (p = getconf("*debug"))
	if (strcmp(p, "0") != 0)
		debugoff = 0;

	rsr6_tabinit();
	dstateinit();
	kproc("srscanproc", scanproc, nil);
	kproc("kickthespares", kickthespares, nil);
}

static void
iosamples(char *s)
{
	ulong n;

	n = strtoul(s, 0, 0);
	if (!isdigit(*s) || n < 1 || n > Niosamples)
		errorstr("invalid sample spec %s [1-%d]", s, Niosamples);
	iosamp = n;
}

static long
iostats(Lun *lb, char *op, int len)
{
	char *p = op;
	char *e = op + len;

	p = seprint(p, e, "%*Z %*Z\n", iosamp, &lb->rfilt, iosamp, &lb->wfilt);
	return p - op;
}

/*
 * This will replace the vlan tag on a frame if needed.
 * Called after all modifications to a Block are complete.
 */
static int
replacevlan(Block *bp)
{
	ulong vlan;

	if (bp->vlan == 0) 
		return 0;
	vlan = 0x8100<<16 |  bp->vlan;
	if (bp->rp - 4 < bp->base) {
		print("replacevlan rp - 4 < base\n");
		return -1;
	}
	bp->rp -= 4;
	memmove(bp->rp, bp->rp + 4, 12);
	hnputl(bp->rp + 12, vlan);
	return 0;
}

/* CSS begin fs interface */

static Lun *
unit2lb(ulong unit)
{
	Lun *lb;

	spinrlock(&lblades);
	for (lb=lblades.head; lb; lb=lb->next)
		if (lb->fsunit == unit) {
			getlun(lb);
			break;
		}
	spinrunlock(&lblades);
	return lb;
}

static void
getlun(Lun *lb)
{
	incref(lb);
}

static void
freerllist(Rowlock *list)
{
	Rowlock *p;

	while (p = list) {
		list = p->next;
		free(p);
	}
}

static void
flushrlcache(Rlcache *c)
{
	int i;

	for (i = 0; i < Nrlhash; i++) {
		if (c->hash[i] == nil)
			continue;
		dprint("warning: rlhash %d not empty\n", i);
		freerllist(c->hash[i]);
		c->hash[i] = nil;
	}
	freerllist(c->free);
	c->free = nil;
}

static int
setfailfast(Rdev *d)
{
	if (CASW(&d->failfast, (void*)0, (void*)1) != 0) { 	/* i am the first */
		dprint("setfailfast: iodraindev %s\n", d->path);
		iodraindev(d, 1);
		return 1;
	}
	return 0;
}

static void
putlun(Lun *lb)
{
	Iosvc *p, *q;
	Raid *r, *e;

	if (decref(lb) != 0)
		return;
	dprint("putlun removing lun %d\n", lb->slot);

	if (ecpresent && (lb->flags & Lcached0))
		setcache(lb, 0, 0);
	if (lb->flags & Lcached1)
		setcache(lb, 1, 0);
	delfilter(&lb->rfilt);
	delfilter(&lb->wfilt);
	r = lb->raids;
	e = r + lb->nraids;
	for (; r<e; r++)
		flushraid(r, lb->zerocfg);
	krreset(lb);
	q = lb->iosvcfree;
	while (p = q) {
		q = q->next;
		free(p);	
	}
	flushrlcache(&lb->rlcache);
	freeblist(lb->iohead);
	free(lb->castats);
	free(lb);
}

static void
lbwakeup(Lun *lb, int all)
{
	Proc *p;

	while (p = lb->iop) {
		lb->iop = p->qnext;
		ready(p);
		if (!all)
			break;
	}
}

static void
lbioclose(Lun *lb)
{
	lock(&lb->iolock);
	lbwakeup(lb, ++lb->closed);
	unlock(&lb->iolock);
}

static Block *
lbnextio(Lun *lb)
{
	Block *bp;

loop:
	ilock(&lb->iolock);
	if (lb->closed) {
		iunlock(&lb->iolock);
		return nil;
	}
	bp = lb->iohead;
	if (bp == nil) {
		/* nothing to do */
		up->qnext = lb->iop;
		lb->iop = up;
		up->state = Queueing;
		up->qpc = getcallerpc(&lb);
		iunlock(&lb->iolock);
		sched();
		goto loop;
	}
	lb->iocnt--;
	lb->iohead = bp->next;
	bp->next = nil;
	iunlock(&lb->iolock);
	return bp;
}

static int
lbiopass(Lun *lb, Block *bp)
{
	Proc *p;

	ilock(&lb->iolock);
	if (lb->closed) {
		iunlock(&lb->iolock);
		return -1;
	}
	if (lb->iocnt >= lb->iolim) {
		iunlock(&lb->iolock);
		ainc(&lb->castats[LCasoflow].stat);
		return -1;
	}
	if (lb->iohead == nil)
		lb->iohead = bp;
	else
		lb->iotail->next = bp;
	lb->iotail = bp;
	lb->iocnt++;
	if (p = lb->iop)
		lb->iop = p->qnext;
	iunlock(&lb->iolock);
	if (p != nil)
		ready(p);
	return 0;
}

static Chan *
srattach(char *spec)
{
	Chan *c;
	static QLock l;
	static int inited;

	qlock(&l);
	if (waserror()) {
		qunlock(&l);
		nexterror();
	}
	if (!inited) {
		srinit();
		inited++;
	}
	poperror();
	qunlock(&l);

	if (*spec != '\0')
		error(Eperm);

	c = devattach(DC, spec);
	mkqid(&c->qid, Qtopdir, 0, QTDIR);
	return c;
}

static int
lbgen(Chan *c, ulong type, Dir *db)
{
	Qid q;
	char *p;
	int perm = 0444;
	uvlong size = 0;
	Lun *lb;

	switch (type) {
	default:
		return -1;
	case Qlbctl:
		p = "ctl";
		perm = 0666;
		break;
	case Qlbdata:
		p = "data";
		perm = 0666;
		if (lb = unit2lb(UNIT(c->qid))){
			size = lb->length - lb->soff;
			putlun(lb);
		}
		break;
	case Qlblunlabel:
		p = "label";
		perm = 0666;
		break;
	case Qlbvlan:
		p = "vlan";
		perm = 0666;
		break;
	case Qlbstat:
		p = "stat";
		break;
	case Qlbrstat:
		p = "raidstat";
		break;
	case Qlbmasks:
		p = "masks";
		if (lb = unit2lb(UNIT(c->qid))){
			size = lb->nmacs * (Eaddrlen*2 + 1);
			putlun(lb);
		}
		break;
	case Qlbiomode:
		p = "iomode";
		perm = 0666;
		break;
	case Qlbconfig:
		p = "config";
		perm = 0666;
		break;
	case Qlbmagic:
		p = "magic";
		perm = 0444;
		break;
	case Qlbmodel:
		p = "model";
		perm = 0666;
		break;
	case Qlbserial:
		p = "serial";
		perm = 0666;
		break;
	case Qlbiostats:
		p = "iostats";
		perm = 0444;
		break;
	case Qlbrls:
		p = "rowlocks";
		perm = 0444;
		break;
	case Qlbioq:
		p = "ioq";
		perm = 0444;
		break;
	}
	mkqid(&q, QID(UNIT(c->qid), type), 0, QTFILE);
	devdir(c, q, p, size, eve, perm, db);
	return 1;
}

static int
topgen(Chan *c, ulong type, Dir *db)
{
	Qid q;
	char *p;
	int perm = 0444;
	uvlong size = 0;

	switch (type) {
	default:
		return -1;
	case Qctl:
		p = "ctl";
		perm = 0666;
		break;
	case Qstat:
		p = "stat";
		break;
	case Qios:
		p = "ios";
		break;
	case Qbufstats:
		p = "bufstats";
		break;
	case Qecchk:
		p = "cachecheck";
		break;
	case Qcachelist:
		p = "cachelist";
		break;
	case Qfcache:
		p = "fcache";
		break;
	case Qdstate:
		p = "dstate";
		break;
	case Qspares:
		p = "spares";
		break;
	}
	mkqid(&q, type, 0, QTFILE);
	devdir(c, q, p, size, eve, perm, db);
	return 1;
}

static int
srgen(Chan *c, char *, Dirtab *, int, int s, Dir *dir)
{
	Qid q;
	Lun *lb;

	if (s == DEVDOTDOT) {
		mkqid(&q, Qtopdir, 0, QTDIR);
		sprint(up->genbuf, "#%C", DC);
		devdir(c, q, up->genbuf, 0, eve, 0555, dir);
		return 1;
	}
	switch (TYPE(c->qid)) {
	default:
		dprint("srgen: unknown type %ld\n", TYPE(c->qid));
		return -1;
	case Qtopdir:
		if (s < Qtopfiles)
			return topgen(c, Qbase + s, dir);
		s -= Qtopfiles;
		if (s > units.ref)
			return -1;
		lb = unit2lb(s);
		if (lb == nil)		/* holes are possible */
			return 0;
		mkqid(&q, QID(s, Qlbdir), 0, QTDIR);
		snprint(up->genbuf, sizeof up->genbuf, "%d", lb->slot);
		putlun(lb);
		devdir(c, q, up->genbuf, 0, eve, 0555, dir);
		return 1;
	case Qctl:
	case Qstat:
	case Qios:
	case Qbufstats:
	case Qecchk:
	case Qcachelist:
	case Qfcache:
	case Qdstate:
	case Qspares:
		return topgen(c, TYPE(c->qid), dir);
	case Qlbdir:
		return lbgen(c, Qlbbase + s, dir);
	case Qlbctl:
	case Qlbstat:
	case Qlbrstat:
	case Qlbmasks:
	case Qlbconfig:
	case Qlbmagic:
	case Qlbiomode:
	case Qlbmodel:
	case Qlbserial:
	case Qlblunlabel:
	case Qlbvlan:
	case Qlbdata:
	case Qlbiostats:
	case Qlbrls:
	case Qlbioq:
		return lbgen(c, TYPE(c->qid), dir);
	}
}

static Walkqid *
srwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, nil, 0, srgen);
}

static int
srstat(Chan *c, uchar *db, int n)
{
	return devstat(c, db, n, nil, 0, srgen);
}

static Chan *
sropen(Chan *c, int omode)
{
	Lun *l;
	uchar *q;
	char *p, *e, *chkbuf;

	if (TYPE(c->qid) == Qecchk) {
		c->aux = eccheck();
	}
	if (TYPE(c->qid) == Qcachelist) {
		chkbuf = malloc(4096);
		if (!chkbuf)
			error("error: malloc failure");
		c->aux = chkbuf;
		if (!cachelist)
			chkbuf[0] = 0;
		else {
			p = chkbuf;
			e = chkbuf + 4096;
			p = seprint(p, e, "LUN   SERIAL\n");
			for (q = cachelist; q < cachelist + ecblk - 1; q +=  Nserialsz) {
				spinrlock(&lblades);
				for (l = lblades.head; l; l = l->next)
					if (memcmp(q, l->serial, Nserialsz) == 0)
						break;
				spinrunlock(&lblades);
				if (l)
					p = seprint(p, e, "%-3d   %s\n", l->slot, l->serial);
			}
		}
	}
	return devopen(c, omode, 0, 0, srgen);
}

static void
srclose(Chan *c)
{
	if (TYPE(c->qid) == Qecchk || TYPE(c->qid) == Qcachelist)
		free(c->aux);
}

static long
ecchkrd(char *chkbuf, void *db, long len, uvlong off)
{
	long n;

	if (!chkbuf)
		error("error: internal error--no buffer");
	n = strlen(chkbuf) - off;
	if (n < 0)
		n = 0;
	else if (len < n)
		n = len;
	if (n)
		strncpy(db, chkbuf + off, n);
	return n;
}

static long
cachelistrd(char *chkbuf, void *db, long len, uvlong off)
{
	long n;

	if (!chkbuf)
		error("error: internal error--no buffer");
	n = strlen(chkbuf) - off;
	if (n < 0)
		n = 0;
	else if (len < n)
		n = len;
	if (n)
		strncpy(db, chkbuf + off, n);
	return n;
}

static long
topread(Chan *c, void *db, long len, uvlong off)
{
	switch(TYPE(c->qid)) {
	default:
		error(Eperm);
	case Qtopdir:
		return devdirread(c, db, len, 0, 0, srgen);
	case Qctl:
		return rootctl(db, len);
	case Qstat:
		return rootstat(db, len);
	case Qios:
		return rootios(db, len);
	case Qdstate:
		return dstateread(db, len);
	case Qspares:
		return showspares(db, len);
	case Qbufstats:
		return bufstats(db, len);
	case Qecchk:
		return ecchkrd(c->aux, db, len, off);
	case Qcachelist:
		return cachelistrd(c->aux, db, len, off);
	case Qfcache:
		return showfcache(db, len);
	}
}

static long
lbread(Lun *lb, Chan *c, void *db, long len, uvlong off)
{
	void *buf;
	int n;
	
	switch(TYPE(c->qid)) {
	default:
		error(Eperm);
	case Qlbdir:
		return devdirread(c, db, len, 0, 0, srgen);
	case Qlbctl:
		return rlbctl(lb, db, len);
	case Qlbstat:
		return lbstat(lb, db, len);
	case Qlbrstat:
		return blbrstat(c, lb, db, len, off);
	case Qlbrls:
		return rlbrowlocks(lb, db, len);
	case Qlbioq:
		return rlbioq(lb, db, len);
	case Qlbmasks:
		return rdmasks(lb, db, len);
	case Qlbiomode:
		return getiomode(lb, db, len);
	case Qlbconfig:
		if (len > lb->nconfig)
			len = lb->nconfig;
		memmove(db, lb->config, len);
		return len;
	case Qlbmagic:
		if (len > Nmagic)
			len = Nmagic;
		memmove(db, lb->magic, len);
		return len;
	case Qlbmodel:
		return getmodel(lb, db, len);
	case Qlbserial:
		return getserial(lb, db, len);
	case Qlbdata:
		if (iskaddr(db))
			return lrw(lb, db, len, off, OREAD, syncio);
		buf = smalloc(len);
		if (waserror()) {
			free(buf);
			nexterror();
		}
		n = lrw(lb, buf, len, off, OREAD, syncio);
		memmove(db, buf, len);
		poperror();
		free(buf);
		return n;
	case Qlbiostats:
		return iostats(lb, db, len);
	case Qlblunlabel:
		return getlunlabel(lb, db, len);
	case Qlbvlan:
		return getvlan(lb, db, len);
	}
}

static int
canroff(int t)
{
	switch (t) {
	case Qlbdata:
	case Qdstate:
	case Qlbrstat:
	case Qecchk:
	case Qcachelist:
		return 1;
	}
	return 0;
}

static long
rlbrowlocks(Lun *lb, void *db, long len)
{
	Rowlock *rl;
	char *p, *ep, *buf;
	int i;
	IOUnit *u;
  
	p = buf = malloc(READSTR*2);
	if (p == nil)
		error("out of memory");
	if (waserror()) {
		free(buf);
		nexterror();
	}
	ep = p + READSTR*2;
	lock(&lb->rlcache);
	for (i=0; i<Nrlhash; i++) {
		rl = lb->rlcache.hash[i];
		for (; rl; rl = rl->next) {
			p = seprint(p, ep, "%02d rno=%d row=%lld ref=%d\n", i, rl->r->rno, rl->row, rl->ref);
			lock(&rl->use);
			u = rl->holdhead;
			if (u == nil)
				p = seprint(p, ep, "\tno holder\n");
			else
				p = seprint(p, ep, "\th mode=%d off=%lld end=%lld pid=%ld\n",
					u->mode, u->off, u->end, u->p->pid);
			for (u = rl->whead; u; u = u->next) {
				p = seprint(p, ep, "\tw mode=%d off=%lld end=%lld pid=%ld\n",
					u->mode, u->off, u->end, u->p->pid);
			}
			unlock(&rl->use);
		}
	}
	unlock(&lb->rlcache);
	if (p - buf < len)
		len = p - buf;
	memmove(db, buf, len);
	poperror();
	free(buf);
	return len;
}

static long
rlbioq(Lun *lb, void *db, long len)
{
	char *p, *ep, *buf;
	Block *bp;
	Aoeata *ah;

	p = buf = malloc(READSTR*2);
	if (p == nil)
		error("out of memory");
	if (waserror()) {
		free(buf);
		nexterror();
	}
	ep = p + READSTR*2;
	ilock(&lb->iolock);
	for (bp = lb->iohead; bp; bp = bp->next) {
		ah = (Aoeata *)bp->rp;
		if (ah->cmd != ACata)
			continue;
		p = seprint(p, ep, "%E %lld %d\n", ah->dst, getlba(ah), ah->scnt);
	}
	iunlock(&lb->iolock);
	if (p - buf < len)
		len = p - buf;
	memmove(db, buf, len);
	poperror();
	free(buf);
	return len;
}

static long
rootios(void *db, long len)
{
	Iosvc *s;
	char *p, *ep, *buf;
	int i;
	Lun *lb;
  
	p = buf = malloc(READSTR*2);
	if (p == nil)
		error("out of memory");
	if (waserror()) {
		free(buf);
		nexterror();
	}
	ep = p + READSTR*2;
	for (lb=lblades.head; lb ; lb=lb->next) {
		ilock(&lb->iosvclock);
		for (i = 0; i < Niosvchash; i++) {
			s = lb->iosvchash[i];
			while (s) {
				p = seprint(p, ep, "%d.%d %d %lld\n", s->targ>>8, s->targ&0xff, i, s->start);
				s = s->next;
			}
		}
		iunlock(&lb->iosvclock);
	}
	if (p - buf < len)
		len = p - buf;
	memmove(db, buf, len);
	poperror();
	free(buf);
	return len;
}

static long
srread(Chan *c, void *db, long len, vlong off)
{
	int t;
	Lun *lb;

	t = TYPE(c->qid);
	if (off && canroff(t) == 0)
		return 0;
	if (t < Qtopend)
		return topread(c, db, len, off);
	else if (t >= Qlbend)
		error("unknown file");
	/* lb file */
	lb = unit2lb(UNIT(c->qid));
	if(lb == nil)
		error("lb gone");
	if(waserror()){
		putlun(lb);
		nexterror();
	}
	len = lbread(lb, c, db, len, off);
	poperror();
	putlun(lb);
	return len;
}

static long
topwrite(Chan *c, void *db, long len, uvlong)
{
	switch (TYPE(c->qid)) {
	default:
		error(Eperm);
	case Qctl:
		return srctl(db, len);
	}
}

static long
lbwrite(Lun *lb, Chan *c, void *db, long len, uvlong off)
{
	void *buf;
	int n;

	switch (TYPE(c->qid)) {
	default:
		error(Eperm);
	case Qlbctl:
		return wlbctl(lb, db, len);
	case Qlbiomode:
		return setiomode(lb, db, len);
	case Qlbconfig:
		if (len > nelem(lb->config))
			len = nelem(lb->config);
		lb->nconfig = len;
		memmove(lb->config, db, lb->nconfig);
		schedsave();
		return lb->nconfig;
	case Qlbmodel:
		return setmodel(lb, db, len);
	case Qlbserial:
		return lbsetserial(lb, db, len);
	case Qlbdata:
		if (iskaddr(db))
			return lrw(lb, db, len, off, OWRITE, syncio);
		buf = smalloc(len);
		if (waserror()) {
			free(buf);
			nexterror();
		}
		memmove(buf, db, len);
		n = lrw(lb, buf, len, off, OWRITE, syncio);
		poperror();
		free(buf);
		return n;
	case Qlblunlabel:
		return lbsetlunlabel(lb, db, len);
	case Qlbvlan:
		return lbsetvlan(lb, db, len);
	}
}

static long
srwrite(Chan *c, void *db, long len, vlong off)
{
	int t;
	Lun *lb;

	t = TYPE(c->qid);
	if (off && t != Qlbdata)
		return 0;
	if (t < Qtopend)
		return topwrite(c, db, len, off);
	else if (t >= Qlbend)
		error("unknown file");
	/* lb file */
	lb = unit2lb(UNIT(c->qid));
	if (lb == nil)
		error("lb gone");
	if (waserror()) {
		putlun(lb);
		nexterror();
	}
	len = lbwrite(lb, c, db, len, off);
	poperror();
	putlun(lb);
	return len;
}

static long
lbsetserial(Lun *lb, void *db, long len)
{
	len = setserial(lb, db, len);
	schedsave();
	return len;
}

static long
setserial(Lun *lb, void *db, long len)
{
	if (len > Nserialsz)
		len = Nserialsz;
	memmove(lb->serial, db, len);
	lb->serial[len] = 0;
	return len;
}

static long
lbsetlunlabel(Lun *lb, void *db, long len)
{
	len = setlunlabel(lb, db, len);
	schedsave();
	return len;
}

static long
setlunlabel(Lun *lb, void *db, long len)
{
	char *b = db;

	while (len && (b[len - 1] == '\n' || b[len - 1]== '\r'))
		len--;
	if (len > Nlunlabelsz)
		errorstr("label length limited to %d characters", Nlunlabelsz);

	memmove(lb->lunlabel, db, len);
	lb->lunlabel[len] = '\0';
	if (lb->lunlabel[0])
		jskevent("msg:s:'LUN %d set label to %s' sev:n:5 tag:s:SRX_LUN_SETLABEL shelf:n:%d lun:n:%d label:s:%s",
			lb->slot, lb->lunlabel, shelf, lb->slot, lb->lunlabel);
	else
		jskevent( "msg:s:'LUN %d unset label' sev:n:5 tag:s:SRX_LUN_UNSETLABEL shelf:n:%d lun:n:%d",
			lb->slot, shelf, lb->slot);	

	return len;
}

static long
lbsetvlan(Lun *lb, void *db, long len)
{
	len = setvlan(lb, db, len);
	schedsave();
	return len;
}

static long
setvlan(Lun *lb, void *db, long len)
{
	char  *e;
	ulong vlan;
	enum { Nvlansz = 4, };
	char s[Nvlansz+1];

	if (len == 0 ||  len > Nvlansz || isdigit(((uchar *)db)[0]) == 0)
		error("vlan value invalid");
	memmove(s, db, len); 
	s[len] = '\0';
	vlan = strtoul(s, &e, 0);
	if (*e != 0) 
		error("vlan value invalid");
	if (vlan > Nvlanmax) 
		errorstr("vlan may not be greater than %d", Nvlanmax);
	if (vlan != lb->vlan) {
		if (vlan)
			jskevent( "msg:s:'LUN %d set VLAN %uld' sev:n:5"
				" tag:s:SRX_LUN_SET_VLAN shelf:n:%d lun:n:%d vlan:n:%uld",
				lb->slot, vlan, shelf, lb->slot, vlan);
		else
			jskevent( "msg:s:'LUN %d UNSET VLAN' sev:n:5"
				" tag:s:SRX_LUN_UNSET_VLAN shelf:n:%d lun:n:%d",
				lb->slot, shelf, lb->slot);	
	}
	lb->vlan = vlan;
	return len;
}

static void
setnobuf0(Lun *lb, int on)
{
	if (on) {
		if ((lb->flags & Lnobuf) == 0)
			jskevent("msg:s:'LUN %d iomode random'"
				" sev:n:5 tag:s:SRX_IOMODE_RANDOM shelf:n:%d lun:n:%d",
				lb->slot, shelf, lb->slot );
		lb->flags |= Lnobuf;
	} else {
		if ((lb->flags & Lnobuf) != 0)
			jskevent("msg:s:'LUN %d iomode sequential' "
				"sev:n:5 tag:s:SRX_IOMODE_SEQUENTIAL shelf:n:%d lun:n:%d",
				lb->slot, shelf, lb->slot );
		lb->flags &= ~Lnobuf;
	}
}

static void
setnobuf(Lun *lb, int on)
{
	Raid *r, *re;

	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	r = lb->raids;
	re = r + lb->nraids;
	for (; r < re; ++r)
		if (candirect(r))
			goto found;
	error(Ebadraid);
found:
	poperror();
	spinrunlock(lb);

	setnobuf0(lb, on);
	schedsave();
}

static void
setnoguard(Lun *lb, int on)
{
	if (on)
		lb->flags |= Lnoguard;
	else
		lb->flags &= ~Lnoguard;

	schedsave();
}

static long
setiomode(Lun *lb, void *db, long len)
{
	char buf[32+1];

	if (len > sizeof buf - 1)
		len = sizeof buf - 1;
	memmove(buf, db, len);
	buf[len] = '\0';
	rmnl(buf);

	if (strcmp(buf, "random") == 0)
		setnobuf(lb, 1);
	else if (strcmp(buf, "sequential") == 0)
		setnobuf(lb, 0);
	else
		error("invalid mode");	
	return len;		
}

static long
setmodel(Lun *lb, void *db, long len)
{
	if (len > Nmodelsz)
		len = Nmodelsz;
	memmove(lb->model, db, len);
	lb->model[len] = 0;
	return len;
}

static long
getserial(Lun *lb, void *db, long len)
{
	return snprint(db, len, "%s", lb->serial);
}

static long
getlunlabel(Lun *lb, void *db, long len)
{
	return snprint(db, len, "%s", lb->lunlabel);
}

static long
getvlan(Lun *lb, void *db, long len)
{
	return snprint(db, len, "%d", lb->vlan);
}

static long
getiomode(Lun *lb, void *db, long len)
{
	if (lb->flags & Lnobuf)
		return snprint(db, len, "random");
	else
		return snprint(db, len, "sequential");
}

static long
getmodel(Lun *lb, void *db, long len)
{
	return snprint(db, len, "%s", lb->model);
}

/* CSS fs support code */

void
ledcfg(Cmdbuf *cb)
{
	int i, j, k;
	Lun *l;
	Raid *r;
	Rdev *d;

	for(i = 0; i < Dcount; i++)
		if(cistrcmp(cb->f[1], dstatename[i]) == 0)
			goto ok;
	cmderror(cb, "bad state");
ok:
	if(cistrcmp(cb->f[2], "on") == 0)
		ledenabled |= 1<<i;
	else
		ledenabled &= ~(1<<i);

	/* force changes */
	qlock(&spares);
	for(j = 0; j < spares.ndevs; j++){
		d = spares.devs+j;
		if(d->flags & 1<<i)
			dstatechg(d);
	}
	qunlock(&spares);

	spinrlock(&lblades);
	for(l = lblades.head; l != nil; l = l->next){
		spinrlock(l);
		for(j = 0; j < l->nraids; j++){
			r = l->raids + j;
			spinrlock(r);
			for(k = 0; k < r->ndevs; k++){
				d = r->devs + k;
				if(d->flags & 1<<i)
					dstatechg(d);
			}
			spinrunlock(r);
		}
		spinrunlock(l);
	}
	spinrunlock(&lblades);
}

static int
setshelf(int argc, char *argv[])
{
	int old, new, rflag;
	char *p, *rstr = "-r";

	rflag = 0;
	for (; argc > 0; argc--, argv++)
		if (strcmp(*argv, rstr) == 0)
			rflag = 1;
		else
			break;
	if (argc != 1)
		errorstr("usage: shelf shelfno");
	old = shelf;
	new = strtoul(argv[0], &p, 0);
	if (*p != 0)
		errorstr("invalid shelf number %s", argv[0]);
	shelf = new;
	if (rflag == 0)
	if (old != shelf)
		jskevent("msg:s:'shelf id set to %d' sev:n:5"
			" tag:s:SRX_SHELF_ID_SET shelf:n:%d oldshelf:n:%d",
			shelf, shelf, old);
	return shelf;
}

enum {
	CMsrctl= 0,
	CMasync= CMsrctl,
	CMasmxor,
	CMbufcnt,
	CMbufcntmax,
	CMbufcntperdisk,
	CMbuildparityloop,
	CMdebug,
	CMelvdelaygap,
	CMelvdelayms,
	CMtrace,
	CMdirectio,
	CMdirtysecs,
	CMdpintrthresh,
	CMecadd,
	CMecattach,
	CMecclose,
	CMecflag,
	CMecream,
	CMecreplay,
	CMecrestore,
	CMfcache,
	CMrmfcache,
	CMmaxdsecs,
	CMlbraid,
	CMlbwperdisk,
	CMlbwmax,
	CMmarkclean,
	CMndprocs,
	CMpreget,
	CMresetec,
	CMlookback,
	CMrlrate,
	CMrmspare,
	CMsave,
	CMscrubrl,
	CMshelf,
	CMspare,
	CMstartether,
	CMstopallether,
	CMstopether,
	CMsync,
	CMdropcache,
	CMiosamples,
	CMppspec,
	CMpgrows,
	CMparitypg,
	CMraidpg,
	CMraidpgp,
	CMraidpgpeek,
	CMrgbmodbias,
	CMnelvread,
	CMnelvwrite,
	CMnelvdread,
	CMnelvdwrite,
	CMnelvdeadms,
	CMniodeadsecs,
	CMniovec,
	CMraidresize,
	CMrrfs,
	CMrspdelayms,
	CMledcfg,
	CMnbsize,
	CMnbskip,
	CMfailpath,
	CMngiveup,

	CMlbctl,
	CMfail= CMlbctl,
	CMflushcache,
	CMgrow,
	CMkrreset,
	CMkrloadrr,
	CMmask,
	CMoffline,
	CMonline,
	CMremove,
	CMreplace,
	CMresetserial,
	CMrmmask,
	CMstopallraid,
	CMstopraid,
	CMunfail,
	CMcache,
	CMcacheprio,
	CMemulate4kn,
	CMflags,
	CMnobuf,
	CMnoguard,
	CMrmlun,

	CMlastctl,

	Nsrctl= CMlbctl,
	Nlbctl= CMlastctl - CMlbctl,
};

static Cmdtab cmds[] = {
[CMasync]		CMasync,		"async",		2,
[CMasmxor]		CMasmxor,		"asmxor",		2,
[CMbufcntmax]		CMbufcntmax,		"bufcntmax",		2,
[CMbufcntperdisk]	CMbufcntperdisk,	"bufcntperdisk",	2,
[CMbufcnt]		CMbufcnt,		"bufcnt",		2,
[CMbuildparityloop]	CMbuildparityloop,	"buildparityloop",	2,
[CMdebug]		CMdebug,		"debug",		2,
[CMelvdelaygap]		CMelvdelaygap,		"elvdelaygap",		2,
[CMelvdelayms]		CMelvdelayms,		"elvdelayms",		2,
[CMtrace]		CMtrace,		"trace",		2,
[CMdirectio]		CMdirectio,		"directio",		2,
[CMdirtysecs]		CMdirtysecs,		"dirtysecs",		2,
[CMdpintrthresh]	CMdpintrthresh,		"dpintrthresh",		2,
[CMecadd]		CMecadd,		"ecadd",		3,
[CMecattach]		CMecattach,		"ecattach",		4,
[CMecclose]		CMecclose,		"ecclose",		2,
[CMecflag]		CMecflag,		"ecflag",		3,
[CMecream]		CMecream,		"ecream",		6,
[CMecreplay]		CMecreplay,		"ecreplay",		2,
[CMecrestore]		CMecrestore,		"ecrestore",		2,
[CMfcache]		CMfcache,		"fcache",		2,
[CMrmfcache]		CMrmfcache,		"rmfcache",		0,
[CMmaxdsecs]		CMmaxdsecs,		"maxdsecs",		2,
[CMlbraid]		CMlbraid,		"lbraid",		0,
[CMlbwmax]		CMlbwmax,		"lbwmax",		2,
[CMlbwperdisk]		CMlbwperdisk,		"lbwperdisk",		2,
[CMmarkclean]		CMmarkclean,		"markclean",		1,
[CMndprocs]		CMndprocs,		"ndprocs",		2,
[CMpreget]		CMpreget,		"preget",		2,
[CMresetec]		CMresetec,		"resetec",		1,
[CMlookback]		CMlookback,		"lookback",		2,
[CMrlrate]		CMrlrate,		"rlrate",		2,
[CMrmspare]		CMrmspare,		"rmspare",		0,
[CMsave]		CMsave,			"save",			1,
[CMscrubrl]		CMscrubrl,		"scrubrl",		2,
[CMshelf]		CMshelf,		"shelf",		0,
[CMspare]		CMspare,		"spare",		2,
[CMstartether]		CMstartether, 		"startether", 		2,
[CMstopallether]	CMstopallether,		"stopallether",		1,
[CMstopether]		CMstopether,		"stopether",		2,
[CMsync]		CMsync,			"sync",			1,
[CMdropcache]		CMdropcache,		"dropcache",		1,
[CMiosamples]		CMiosamples,		"iosamples",		2,
[CMppspec]		CMppspec,		"parityprespec",	2,
[CMpgrows]		CMpgrows,		"pgrows",		2,
[CMparitypg]		CMparitypg,		"bpparitypg",		2,
[CMraidpg]		CMraidpg,		"raidpg", 		2,
[CMraidpgp]		CMraidpgp,		"raidpgp", 		2,
[CMraidpgpeek]		CMraidpgpeek,		"raidpgpeek", 		2,
[CMrgbmodbias]		CMrgbmodbias,		"rgbmodbias", 		2,
[CMnelvread]		CMnelvread,		"nelvread", 		2,
[CMnelvwrite]		CMnelvwrite,		"nelvwrite",		2,
[CMnelvdread]		CMnelvdread,		"nelvdread",		2,
[CMnelvdwrite]		CMnelvdwrite,		"nelvdwrite",		2,
[CMnelvdeadms]		CMnelvdeadms,		"nelvdeadms",		2,
[CMniodeadsecs]		CMniodeadsecs,		"iodeadsecs",		2,
[CMniovec]		CMniovec,		"niovec",		2,
[CMraidresize]		CMraidresize,		"raidresize",		2,
[CMrrfs]		CMrrfs,			"rrfs",			2,
[CMrspdelayms]		CMrspdelayms,		"rspdelayms",		2,
[CMledcfg]		CMledcfg,		"ledcfg",		3,
[CMnbsize]		CMnbsize,		"nbsize",		2,
[CMnbskip]		CMnbskip,		"nbskip",		2,
[CMfailpath]		CMfailpath,		"failpath",		2,
[CMngiveup]		CMngiveup,		"ngiveup",		2,

[CMfail]		CMfail,			"fail",			2,
[CMflushcache]		CMflushcache,		"flushcache",		2,
[CMgrow]		CMgrow,			"grow",			0,
[CMkrreset]		CMkrreset,		"krreset",		1,
[CMmask]		CMmask,			"mask",			2,
[CMoffline]		CMoffline,		"offline",		1,
[CMonline]		CMonline,		"online",		1,
[CMremove]		CMremove,		"remove",		0,
[CMreplace]		CMreplace,		"replace",		3,
[CMresetserial]		CMresetserial,		"resetserial",		1,
[CMrmmask]		CMrmmask,		"rmmask",		2,
[CMstopallraid]		CMstopallraid,		"stopallraid",		1,
[CMstopraid]		CMstopraid,		"stopraid",		2,
[CMunfail]		CMunfail,		"unfail",		2,
[CMcache]		CMcache,		"cache",		3,
[CMcacheprio]		CMcacheprio,		"cacheprio",		4,
[CMemulate4kn]		CMemulate4kn,		"emulate4kn",		2,
[CMflags]		CMflags,		"flags",		2,
[CMnobuf]		CMnobuf,		"nobuf",		2,
[CMnoguard]		CMnoguard,		"noguard",		2,
[CMrmlun]		CMrmlun,		"rmlun",		0,
[CMkrloadrr]		CMkrloadrr,		"krloadrr",		1,
};

static int
ckonoff(char *p)
{
	if (strcmp(p, "on") == 0)
		return 1;
	if (strcmp(p, "off") == 0)
		return 0;
	errorstr("incorrect directive: %s", p);
	return -1;
}

static char *
onoff(int x)
{
	return x ? "on" : "off";
}

static long
srctl(void *db, long n)
{
	Cmdbuf *cb;
	Cmdtab *ct;

	notedefer();
	if (waserror()) {
		noteallow();
		nexterror();
	}
	cb = parsecmd(db, n);
	if (waserror()) {
		free(cb);
		nexterror();
	}
	ct = lookupcmd(cb, cmds+CMsrctl, Nsrctl);
	switch (ct->index) {
	case CMlbraid:
	case CMrmspare:
	case CMsave:
	case CMspare:
		if (shelf == Nshelfunset)		/* help first time users */
			error("shelf address is unset");
	}
	switch (ct->index) {
	default:
		cmderror(cb, Enoctl);
	case CMasync:
		syncio = ckonoff(cb->f[1]) ? 0 : Bimm;
		break;
	case CMasmxor:
		asmxor = ckonoff(cb->f[1]);
		break;
	case CMbufcnt:
		nbufcnt = atoi(cb->f[1]);
		break;
	case CMbuildparityloop:
		buildparityloop = ckonoff(cb->f[1]);
		break;
	case CMdebug:
		debugoff = !ckonoff(cb->f[1]);
		break;
	case CMelvdelaygap:
		elvdelaygap = atoi(cb->f[1]);
		break;
	case CMelvdelayms:
		elvdelayms = atoi(cb->f[1]);
		break;
	case CMtrace:
		traceoff = !ckonoff(cb->f[1]);
		break;
	case CMdirectio:
		directio = ckonoff(cb->f[1]);
		break;
	case CMdirtysecs:
		dirtysecs = atoi(cb->f[1]);
		break;
	case CMdpintrthresh:
		dpintrthresh = strtoul(cb->f[1], 0, 0);
		break;
	case CMecadd:
		addcache(cb->f[1], cb->f[2]);
		break;
	case CMecattach:
		attachcache(cb->f[1], cb->f[2], cb->f[3]);
		break;
	case CMecclose:
		ecattached[atoi(cb->f[1])] = 0;
		ecclose(atoi(cb->f[1]));
		break;
	case CMecflag:
		cacheflags(atoi(cb->f[1]), atoi(cb->f[2]));
		break;
	case CMecream:
		formatcache(cb->f[1], cb->f[2], cb->f[3], cb->f[4], cb->f[5]);
		break;
	case CMecreplay:
//		if (!ecattached[0])
//			error("error: No write buffer attached");
		ecreplay(atoi(cb->f[1]));
		break;
	case CMecrestore:
		restorecache(cb->f[1]);
		break;
	case CMfcache:
		fcache(cb->f[1]);
		break;
	case CMrmfcache:
		rmfcache(cb->nf-1, cb->f+1);
		break;
	case CMmaxdsecs:
		maxdsecs = atoi(cb->f[1]);
		break;
	case CMlbraid:
		lbraid(cb->nf-1, cb->f+1);
		break;
	case CMlbwmax:
		lbwmax = strtoul(cb->f[1], 0, 10);
		break;
	case CMbufcntmax:
		bufcntmax = atoi(cb->f[1]);
		break;
	case CMbufcntperdisk:
		bufcntperdisk = atoi(cb->f[1]);
		break;
	case CMlbwperdisk:
		lbwperdisk = atoi(cb->f[1]);
		break;
	case CMmarkclean:
		markcleanall();
		break;
	case CMndprocs:
		ndprocs = atoi(cb->f[1]);
		break;
	case CMpreget:
		npreget = atoi(cb->f[1]);
		break;
	case CMresetec:
		ecseqio = ecrandio = 0;
		break;
	case CMlookback:
		lookback = atoi(cb->f[1]);
		break;
	case CMrlrate:
		rlrate = strtoul(cb->f[1], 0, 0);
		break;
	case CMrmspare:
		rmspare(cb->nf-1, cb->f+1);
		break;
	case CMsave:
		save();
		break;
	case CMscrubrl:
		scrubrl = ckonoff(cb->f[1]);
		break;
	case CMshelf:
		shelf = setshelf(cb->nf-1, cb->f+1);
		break;
	case CMspare:
		spare(cb->f[1]);
		break;
	case CMstartether:
		startether(cb->f[1]);
		break;
	case CMstopallether:
		stopallether();
		break;
	case CMstopether:
		stopether(cb->f[1]);
		break;
	case CMsync:
		sync();
		break;
	case CMdropcache:
		dropcache();
		break;
	case CMiosamples:
		iosamples(cb->f[1]);
		break;
	case CMppspec:
		parityprespec = ckonoff(cb->f[1]);
		break;
	case CMpgrows:
		pgrows = strtoul(cb->f[1], 0, 0);
		break;
	case CMparitypg:
		paritypg = ckonoff(cb->f[1]);
		break;
	case CMraidpg:
		raidpg = ckonoff(cb->f[1]);
		break;
	case CMraidpgp:
		raidpgp = ckonoff(cb->f[1]);
		break;
	case CMraidpgpeek:
		raidpgpeek = ckonoff(cb->f[1]);
		break;
	case CMrgbmodbias:
		rgbmodbias = ckonoff(cb->f[1]);
		break;
	case CMnelvread:
		nelvread = atoi(cb->f[1]);
		break;
	case CMnelvwrite:
		nelvwrite = atoi(cb->f[1]);
		break;
	case CMnelvdread:
		nelvdread = atoi(cb->f[1]);
		break;
	case CMnelvdwrite:
		nelvdwrite = atoi(cb->f[1]);
		break;
	case CMnelvdeadms:
		nelvdeadms = atoi(cb->f[1]);
		break;
	case CMniodeadsecs:
		niodeadsecs = atoi(cb->f[1]);
		break;
	case CMniovec:
		niovec = atoi(cb->f[1]);
		break;
	case CMraidresize:
		raidresize = ckonoff(cb->f[1]);
		break;
	case CMrrfs:
		setrrfs(cb->f[1]);
		break;
	case CMnbsize:
		/*
		 *  This is an extremely dangerous knob! All LUNs should
		 * be offline; argument should be less than Nstride.
		 */
		nbsize = atoi(cb->f[1]);
		break;
	case CMnbskip:
		nbskip = atoi(cb->f[1]);
		break;
	case CMrspdelayms:
		rspdelayms = atoi(cb->f[1]);
		break;
	case CMledcfg:
		ledcfg(cb);
		break;
	case CMfailpath:
		failpath(cb->f[1]);
		break;
	case CMngiveup:
		ngiveup = atoi(cb->f[1]);
		break;
	}
	poperror();
	free(cb);
	poperror();
	noteallow();
	return n;
}

static void
stopraid(Lun *lb)
{
	Raid *r, *re;

	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	r = lb->raids;
	re = r + lb->nraids;
	for (; r<re; r++) {
		spinwlock(r);
		r->flags |= Rstop;
		spinwunlock(r);
		while ((r->flags & (Rscrubbing|Riniting)) || r->nrecovery > 0)
			tsleep(&up->sleep, return0, 0, 100);
	}
	poperror();
	spinrunlock(lb);
}

static char *
sepdpstats(char *p, char *e, Lun *lb)
{
	int i;
	Raid *r;

	p = seprint(p, e, "dpworking:");
	r = &lb->raids[0];
	for (i = 0; i < r->ndevs; i++)
		p = seprint(p, e, " %ud", r->devs[i].dpworking);
	p = seprint(p, e, "\ndprrandom:");
	for (i = 0; i < r->ndevs; i++)
		p = seprint(p, e, " %lud", r->devs[i].rrandom);
	p = seprint(p, e, "\ndpwrandom:");
	for (i = 0; i < r->ndevs; i++)
		p = seprint(p, e, " %lud", r->devs[i].wrandom);
	p = seprint(p, e, "\ndpiomode:");
	for (i = 0; i < r->ndevs; i++)
		p = seprint(p, e, " %ud", r->devs[i].iomode);
	return seprint(p, e, "\n");
}

static long
rlbctl(Lun *lb, void *db, long n)
{
	char *p, *ep;
	
	p = (char *) db;
	ep = p + n;
	p = seprint(p, ep, "geometry: %llud %lud\n", (lb->length-lb->soff)/lb->secsize, lb->secsize);
	p = seprint(p, ep, "physectorsize: %d\n", lb->physecsize);
	p = seprint(p, ep, "rpm: %d\n", lb->rpm);
	p = seprint(p, ep, "flushcache: %s\n", (lb->flags & Lflushcache) ? "enabled" : "disabled");
	p = seprint(p, ep, "flags: %d\n", lb->flags);
	p = seprint(p, ep, "nobuf: %s\n", onoff(lb->flags & Lnobuf));
	p = seprint(p, ep, "fcpri: %d\n", lb->fcpri);
	p = seprint(p, ep, "fcminpct: %d\n", lb->fcminpct);
	p = seprint(p, ep, "noguard: %s\n", onoff(lb->flags & Lnoguard));
	p = seprint(p, ep, "nworker: %ld\n", lb->nworker);
	p = seprint(p, ep, "iocnt: %d\n", lb->iocnt);
	p = seprint(p, ep, "ndp: %ld\n", getndp(lb));
	p = seprint(p, ep, "elvq: %ld\n", lb->castats[LCaselvq].stat);
	p = seprint(p, ep, "elvwait: %ld\n", lb->castats[LCaselvwait].stat);
	p = seprint(p, ep, "iooverlaps: %lud\n", lb->castats[LCasoverlap].stat);
	p = seprint(p, ep, "iodups: %lud\n", lb->castats[LCasiodup].stat);
	p = seprint(p, ep, "oflows: %lud\n", lb->castats[LCasoflow].stat);
	p = seprint(p, ep, "lbworking: %lud\n", lb->castats[LCaslbw].stat);
	p = sepdpstats(p, ep, lb);
	return p - (char *) db;
}

static long
wlbctl(Lun *lb, void *db, long n)
{
	Cmdbuf *cb;
	Cmdtab *ct;

	notedefer();
	if (waserror()) {
		noteallow();
		nexterror();
	}
	cb = parsecmd(db, n);
	if (waserror()) {
		free(cb);
		nexterror();
	}
	ct = lookupcmd(cb, cmds+CMlbctl, Nlbctl);
	switch (ct->index) {
	default:
		cmderror(cb, Enoctl);
	case CMfail:
		fail(lb, cb->f[1]);
		break;
	case CMflushcache:
		flushcache(lb, ckonoff(cb->f[1]));
		break;
	case CMgrow:
		grow(lb, cb->nf-1, cb->f+1);
		break;
	case CMkrreset:
		krreset(lb);
		/* fall through to loadrr */
	case CMkrloadrr:
		krloadrr(lb);
		break;
	case CMmask:
		mask(lb, cb->f[1]);
		break;
	case CMoffline:
		setline(lb, Loff);
		break;
	case CMonline:
		setline(lb, Lon);
		break;
	case CMremove:
	case CMrmlun:
		rmlb(lb, cb->nf - 1, cb->f + 1);
		break;
	case CMreplace:
		ckpath(cb->f[2]);
		replace(lb, cb->f[1], cb->f[2]);
		break;
	case CMresetserial:
		resetserial(lb);
		schedsave();
		break;
	case CMrmmask:
		rmmask(lb, cb->f[1]);
		break;
	case CMstopallraid:
	case CMstopraid:
		stopraid(lb);
		break;
	case CMunfail:
		replace(lb, cb->f[1], nil);
		break;
	case CMcache:
		setcache(lb, atoi(cb->f[1]), ckonoff(cb->f[2]));
		break;
	case CMcacheprio:
		cacheprio(lb, atoi(cb->f[1]), cb->f[2], cb->f[3]);
		break;
	case CMemulate4kn:
		emulate4kn(lb, ckonoff(cb->f[1]));
		break;
	case CMflags:
		lb->flags = atoi(cb->f[1]);
		break;
	case CMnobuf:
		setnobuf(lb, ckonoff(cb->f[1]));
		break;
	case CMnoguard:
		setnoguard(lb, ckonoff(cb->f[1]));
		break;
	}
	poperror();
	free(cb);
	poperror();
	noteallow();
	return n;
}

static long
rootctl(void *db, long len)
{
	char *p, *ep;
	Net *np;
	
	p = (char *) db;
	ep = p + len;
	p = seprint(p, ep, "shelf: %d\n", shelf);
	p = seprint(p, ep, "ethers:");
	rlock(&nets);
	for (np = nets.head; np; np = np->next)
		if (np->state == SNactive)
			p = seprint(p, ep, " %s", np->path);
	runlock(&nets);
	p = seprint(p, ep, "\n");
	p = seprint(p, ep, "asmxor: %s\n", onoff(asmxor));
	p = seprint(p, ep, "async: %s\n", onoff(syncio != Bimm));
	p = seprint(p, ep, "bufcnt: %lud\n", nbufcnt);
	p = seprint(p, ep, "bufcntmax: %lud\n", bufcntmax);
	p = seprint(p, ep, "bufcntperdisk: %lud\n", bufcntperdisk);
	p = seprint(p, ep, "buildparityloop: %s\n", onoff(buildparityloop));
	p = seprint(p, ep, "debug: %s\n", onoff(!debugoff));
	p = seprint(p, ep, "elvdelaygap: %lud\n", elvdelaygap);
	p = seprint(p, ep, "elvdelayms: %lud\n", elvdelayms);
	p = seprint(p, ep, "trace: %s\n", onoff(!traceoff));
	p = seprint(p, ep, "directio: %s\n", onoff(directio));
	p = seprint(p, ep, "dirtysecs: %d\n", dirtysecs);
	p = seprint(p, ep, "preget: %d\n", npreget);
	p = seprint(p, ep, "maxdsecs: %d\n", maxdsecs);
	p = seprint(p, ep, "rlrate: %uld\n", rlrate);
	p = seprint(p, ep, "scrubrl: %s\n", onoff(scrubrl));
	p = seprint(p, ep, "iosamples: %d\n", iosamp);
	p = seprint(p, ep, "pgrows: %lud\n", pgrows);
	p = seprint(p, ep, "bpparitypg: %s\n", onoff(paritypg));
	p = seprint(p, ep, "raidpg: %s\n", onoff(raidpg));
	p = seprint(p, ep, "raidpgp: %s\n", onoff(raidpgp));
	p = seprint(p, ep, "raidpgpeek: %s\n", onoff(raidpgpeek));
	p = seprint(p, ep, "rgbmodbias: %s\n", onoff(rgbmodbias));
	p = seprint(p, ep, "parityprespec: %s\n", onoff(parityprespec));
	p = seprint(p, ep, "raidresize: %s\n", onoff(raidresize));
	p = seprint(p, ep, "rrfs: %s %p\n", rrfs.mnt, rrfs.rootc);
	p = seprint(p, ep, "nelvread: %ld\n", nelvread);
	p = seprint(p, ep, "nelvwrite: %ld\n", nelvwrite);
	p = seprint(p, ep, "nelvdread: %ld\n", nelvdread);
	p = seprint(p, ep, "nelvdwrite: %ld\n", nelvdwrite);
	p = seprint(p, ep, "nelvdeadms: %ld\n", nelvdeadms);
	p = seprint(p, ep, "niodeadsecs: %ld\n", niodeadsecs);
	p = seprint(p, ep, "rspdelayms: %ld\n", rspdelayms);
	p = seprint(p, ep, "nbsize: %d\n", nbsize);
	p = seprint(p, ep, "nbskip: %d\n", nbskip);
	p = seprint(p, ep, "lbwmax: %lud\n", lbwmax);
	p = seprint(p, ep, "lbwperdisk: %lud\n", lbwperdisk);
	p = seprint(p, ep, "ndprocs: %lud\n", ndprocs);
	p = seprint(p, ep, "ecseqio: %lud\n", ecseqio);
	p = seprint(p, ep, "ecrandio: %lud\n", ecrandio);
	p = seprint(p, ep, "lookback: %d\n", lookback);
	p = seprint(p, ep, "dpintrthresh: %lud\n", dpintrthresh);
	p = seprint(p, ep, "niovec: %lud\n", niovec);
	p = seprint(p, ep, "ngiveup: %d\n", ngiveup);
	return p - (char *) db;
}

static long
rootstat(void *db, long len)
{
	char *p, *ep;

	p = (char *) db;
	ep = p + len;
	p = seprint(p, ep, "bpfltoosoon: %lud\n", bpfltoosoon);
	p = seprint(p, ep, "bpmodio: %lud\n", bpmodio);
	p = seprint(p, ep, "bpmodioR: %lud\n", bpmodioR);
	p = seprint(p, ep, "bpmodioW: %lud\n", bpmodioW);
	p = seprint(p, ep, "cachehits: %lud\n", cachehits);
	p = seprint(p, ep, "collisions: %lud\n", collisions);
	p = seprint(p, ep, "devwrites: %lud\n", devwrites);
	p = seprint(p, ep, "devreads: %lud\n", devreads);
	p = seprint(p, ep, "scanref: %ld\n", scanref.ref);
	p = seprint(p, ep, "nhbufs: %d\n", Nhbufs);
	p = seprint(p, ep, "nbufs: %d\n", Nbufs);
	p = seprint(p, ep, "linefulls: %lud\n", linefulls);
	p = seprint(p, ep, "rlinefulls: %lud\n", rlinefulls);
	p = seprint(p, ep, "giveups: %lud\n", giveups);
	p = seprint(p, ep, "pregetbios: %lud\n", pregetbios);
	p = seprint(p, ep, "pregetbiomods: %lud\n", pregetbiomods);
	p = seprint(p, ep, "pregethits: %lud\n", pregethits);
	p = seprint(p, ep, "pregetlinefulls: %lud\n", pregetlinefulls);
	p = seprint(p, ep, "nomatches: %lud\n", nomatches);
	p = seprint(p, ep, "noputdbufs: %lud\n", noputdbufs);
	p = seprint(p, ep, "getbufs: %lud\n", getbufs);
	p = seprint(p, ep, "getbufwaits: %lud\n", getbufwaits);
	p = seprint(p, ep, "deadraised: %lud\n", deadraised);
	return p - (char *) db;
}

static long
lbstat(Lun *lb, void *db, long len)
{
	char *p, *ep;

	p = db;
	ep = p + len;
	p = seprint(p, ep, "%llud %sline cm%s fc%s\n",
		lb->length - lb->soff, onoff(lb->line == Lon), (lb->flags & Lcached0)? "enabled" : "disabled",
		(lb->flags & Lcached1)? "enabled" : "disabled");
	return p - (char *) db;
}

static char *
rateprint(char *p, char *ep, Raid *r, uvlong row, Iofilter *f)
{
	uvlong b;
	ulong n;

	if ((n = filtersum(f, &b, 0, 0)) > 0)
	switch (r->type) {
	case RTraid5:
	case RTraid6rs:
		p = seprint(p, ep, " %lld/%lld:%lld", row * r->ndevs * nbsize, r->mindlen * r->ndevs, b / n);
		break;
	case RTraid1:
	case RTraid10:
		p = seprint(p, ep, " %lld/%lld:%lld", row * 2 * nbsize, r->mindlen * 2, b / (uvlong) n);
	}
	return p;
}

/* enters with spinrlock(r) held */
static int
rstate(Raid *r, void *db, long n)
{
	char *p, *ep;

	p = (char *) db;
	ep = p + n;

	if (r->flags & Rfailed)
		p = seprint(p, ep, "failed,");
	/*
	 * both initing and needinit can be set, but we only
	 * care about reporting one.
	 */
	if (r->flags & Riniting)
		p = seprint(p, ep, "initing,");
	else if (r->flags & Rneedinit)
		p = seprint(p, ep, "needinit,");
	if (r->flags & Rrecovering)
		p = seprint(p, ep, "recovering,");
	if (r->flags & Rstop)
		p = seprint(p, ep, "stop,");
//	if (r->flags & Rclean)
//		p = seprint(p, ep, "clean,");
	if (r->flags & Rdegraded)
		p = seprint(p, ep, "degraded,");
	if (p != db) {
		p--;
		*p = '\0';
	} else
		p = seprint(p, ep, "normal");
	if (r->flags & Riniting)
		p = rateprint(p, ep, r, r->roff, &r->filter);
	return p - (char *) db;
}

static int
drstate(Rdev *d, char *op, long n)
{
	Raid *r;
	char *p = op;

	if (!(d->flags & Dreplacing))
		return 0;
	r = d->raid;
	p = rateprint(p, p+n, r, d->row, &d->filter);
	return p - op;
}

enum {
	Nb= 4096,
	Nlbrbuf= 8,
	Mugh= 0xdefec8,
};
struct {
	void *ver;
	char buf[Nb];
	long len;
} lbrbuf[Nlbrbuf];

/* this is ... so bad. */
static long
blbrstat(Chan *c, Lun *lb, void *db, long len, uvlong off)
{
	uint i, dlen, ugh;
	static QLock q;
	static uint ver;

	qlock(&q);
	if (waserror()) {
		qunlock(&q);
		nexterror();
	}
	i = (uint) c->aux;
	ugh = i / Nlbrbuf;
	i %= Nlbrbuf;
	if (ugh != Mugh || lbrbuf[i].ver != c->aux) {
		if (off)
			error("phase error");
		i = ver++ % Nlbrbuf;
		lbrbuf[i].ver = c->aux = (void *) (Mugh*Nlbrbuf + i);
		lbrbuf[i].len = lbrstat(lb, lbrbuf[i].buf, Nb);
	}
	dlen = lbrbuf[i].len;
	if (off >= dlen)
		len = 0;
	else if (off + len > dlen)
		len = dlen - off;
	memmove(db, &lbrbuf[i].buf[off], len);
	poperror();
	qunlock(&q);
	return len;
}

static long
lbrstat(Lun *lb, void *db, long len)
{
	Raid *r, *re;
	Rdev *d, *de;
	char *p, *ep;
	char buf[64];

	p = (char *) db;
	ep = p + len;

	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	r = lb->raids;
	re = r + lb->nraids;
	for (; r<re; r++) {
		spinrlock(r);
		if (waserror()) {
			spinrunlock(r);
			nexterror();
		}
		len = rstate(r, buf, sizeof buf - 1);
		buf[len] = 0;
		p = seprint(p, ep, "%d.%d %s %d %lld %lld %s\n",
			lb->slot, r->rno, rt2s(r->type), r->ndevs,
			r->length, r->mindlen, buf);
		d = r->devs;
		de = d + r->ndevs;
		for (; d<de; d++) {
			len = drstate(d, buf, sizeof buf - 1);
			buf[len] = 0;
			p = seprint(p, ep, "%d.%d.%d %lld %s %s %s\n",
				lb->slot, r->rno, d->dno, d->length, dstate(d, 0), d->path, buf);
		}
		poperror();
		spinrunlock(r);
	}
	poperror();
	spinrunlock(lb);
	return p - (char *) db;
}

static long
getndp(Lun *lb)
{
	Raid *r, *re;
	Rdev *d, *de;
	long ndp;

	ndp = 0;
	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	r = lb->raids;
	re = r + lb->nraids;
	for (; r<re; r++) {
		spinrlock(r);
		if (waserror()) {
			spinrunlock(r);
			nexterror();
		}
		d = r->devs;
		de = d + r->ndevs;
		for (; d<de; d++)
			ndp += d->ndp;
		poperror();
		spinrunlock(r);
	}
	poperror();
	spinrunlock(lb);
	return ndp;
}

static void
spare(char *path)
{
	Rdev *d, *nd;

	ckpath(path);
	qlock(&spares);
	nd = spares.devs + spares.ndevs;
	for (d = spares.devs; d < nd; d++)
		if (strncmp(d->path, path, sizeof d->path) == 0) {
			qunlock(&spares);
			return;
		}
	if (waserror()) {
		qunlock(&spares);
		nexterror();
	}
	if (spares.ndevs >= Nspares)
		error("too many spares");
	memset(d, 0, sizeof *d);
	makedev(d, path);
	if (d->flags & (Dfailed|Dmissing)) {
		wonkdev(d, 0);
		error("spare devices cannot be failed or missing");
	}
	d->flags |= Dspare;
	dstatechg(d);
	d->raid = nil;
	spares.ndevs++;
	poperror();
	qunlock(&spares);
	jskevent("msg:s:'Drive %s added to spare pool'"
		" sev:n:5 tag:s:SRX_SPARE_ADD shelf:n:%d slot:n:%d", 
		d->name, shelf, d->slot);
	schedsave();
}

static int
parsedrive(char *s)
{
	char *r, *p;
	int n;

	r = p = nil;
	while (s = strchr(s, '/')) {
		r = p;
		p = s++;
	}
	if (r == nil || isdigit(*++r) == 0)
		return -1;
	n = strtol(r, &r, 10);
	if (*r != '/')
		return -1;
	return n;
}

static int
ckpath(char *path)
{
	Chan *c;
	int n;

	n = parsedrive(path);
	if (n < 0)
		errorstr("%s is not a valid drive path", path);
	if (waserror())
		errorstr("drive %d.%d does not exist", shelf, n);
	c = namec(path, Aopen, OREAD, 0);
	cclose(c);
	poperror();
	return n;
}

static void
rmspare(int argc, char *argv[])
{
	Rdev *d, *e;
	char *path;
	int n, zerocfg = 1;

	switch (argc) {
	case 1:
		path = argv[0];
		break;
	case 2:
		if (!strcmp(*argv, "-s")) {	/* save config string */
			path = argv[1];
			zerocfg = 0;
			break;
		}
	default:
		error("usage: rmspare [-s] /path/to/dev");
		return;
	}

	n = ckpath(path);
	qlock(&spares);
	if (waserror()) {
		qunlock(&spares);
		nexterror();
	}
	d = spares.devs;
	e = d + spares.ndevs;
	for (; d<e; d++)
		if (!strcmp(path, d->path))
			break;
	if (d == e)
		errorstr("%d.%d is not a spare", shelf, n);
	d->flags &= ~Dspare;
	dstatechg(d);
	wonkdev(d, zerocfg);
	jskevent("msg:s:'Drive %s removed from spare pool'"
		" sev:n:5 tag:s:SRX_SPARE_REMOVE shelf:n:%d slot:n:%d", 
		d->name, shelf, d->slot);
	if (--spares.ndevs)
		*d = *(e-1);
	poperror();
	qunlock(&spares);
}

static int
getspare(Rdev *rd, uvlong len)
{
	Rdev *d, *e, *q=nil, *s=nil;

	qlock(&spares);
	d = spares.devs;
	e = d + spares.ndevs;
	for (; d<e; d++) {
		if (len < d->length) {
			if (!q || d->length < q->length)	/* find best fit */
				q = d;
			if (!d->badspare)
			if (!s || d->length < s->length)	/* find best safe spare */
				s = d;
		}
	}
	if (q) {
		if (q->badspare && s) {
			if (q->length != s->length)
				print("spare %s is best fit, but is skipped due to check-in failures\n", q->name);
			q = s;
		}
		*rd = *q;
		*q = *(e-1);
		spares.ndevs--;
		rd->flags &= ~Dspare;
		dstatechg(rd);
	}
	qunlock(&spares);
	return q != nil;
}

static void
markcleanone(Lun *lb)
{
	Raid *r, *re;
	Rdev *d, *de;

	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	r = lb->raids;
	re = r + lb->nraids;
	for (; r<re; r++) {
		if (r->flags & (/*Rneedinit|Riniting|*/Rfailed|Rrecovering))
			continue;
		spinwlock(r);
		if (waserror()) {
			spinwunlock(r);
			nexterror();
		}
		d = r->devs;
		de = d + r->ndevs;
		for (; d<de; d++)
			if (!DEVBAD(d))
				d->flags |= Dclean;
		setrflags(r);
		poperror();
		spinwunlock(r);
	}
	poperror();
	spinrunlock(lb);
}

static void
markcleanall(void)
{
	Lun *lb;

	spinrlock(&lblades);
	lb = lblades.head;
	for (; lb; lb=lb->next) {
		markcleanone(lb);
	}
	spinrunlock(&lblades);
}

/* Check if the lun is configured as raidtype update. */
static int
isupdatelun(Lun *lb)
{
	if (lb->raids[0].type == RTraw)
	if (cistrstr(lb->raids[0].devs[0].path, "update"))
		return 1;
	return 0;
}

static void
wipeupdate(void)
{
	int n;
	Chan *c;
	char buf[64];

	/* Can't rely on namespace from the kernel */
	snprint(buf, sizeof buf, "/n/sys/ctl");
	if (waserror()) {
		dprint("wipeupdate: unable to open %s: %s\n", buf, up->errstr);
		return;
	}
	c = namec(buf, Aopen, OWRITE, 0);
	poperror();
	if (waserror()) {
		dprint("wipeupdate: unable to write to chan for %s: %s\n", buf,
		       up->errstr);
		cclose(c);
		return;
	}
	n = devtab[c->type]->write(c, "updatewipe", 10, 0);
	if (n != 10)
		errorf("write returned %d", n);
	poperror();
	cclose(c);	
}

/*
 * entering this function the Lun has a reference count for the
 * fs call above us.
 */
static void
rmlb(Lun *lb, int argc, char **argv)
{
	int i, sflag, isupd;
	Lun *l, **ll;
	enum {
		Nms	= 100,
		Nwarn	= 5*1000 / Nms,
		Newait	= 30*1000 / Nms,
	};

	sflag = 0;
	switch (argc) {
	case 0:
		break;
	case 1:
		if (strcmp(*argv, "-s") == 0) {
			sflag++;
			break;
		}
		/* fall thru */
	default:
		error(Eusage);
	}

	isupd = isupdatelun(lb);
	
	if (sflag) {
		if (!waserror()) {
			markcleanone(lb);
			save();
			poperror();
		} else
			print("Warning: failure marking devices clean for LUN %d ejection\n", lb->slot);
	}

	/* halt ingress of packets from netreaders by removing from lblades list */
	spinwlock(&lblades);
	ll = &lblades.head;
	for (; l=*ll; ll=&l->next)
		if (l == lb) {
			*ll = l->next;
			break;
		}
	spinwunlock(&lblades);
	if (l == nil) {
		/* this should not be possible */
		errorstr("LUN %d not found", lb->slot);
	}

	/* drop list ref cnt */
	putlun(lb);

	/* stop async i/o for parity, disk rebuild, scrub, etc */
	stopraid(lb);

	/*
	 * Close queue freeing any open blocks and kicking out lbworkers.
	 * Wait for ref cnt to drop to one, the ref for the fs write operation that
	 * called us.  If the fs ref handling code changes, this must as well.
	 */
	lbioclose(lb);
	for (i=0; lb->ref != 1 && i < Newait; i++) {
		if (i && (i % Nwarn) == 0)
			print("rmlb: ref %ld; waiting\n", lb->ref);
		if (!waserror()) {
			tsleep(&up->sleep, return0, 0, Nms);
			poperror();
		}
	}
	if (i == Newait)
		print("rmlb: ref %ld; continuing without waiting\n", lb->ref);
	else if (i >= Nwarn)
		print("rmlb: release successful\n");

	if (sflag) {
		jskevent("msg:s:'Ejecting lun %d' sev:n:5 tag:s:SRX_LUN_EJECTED shelf:n:%d lun:n:%d", 
			lb->slot, shelf, lb->slot);
	} else
		jskevent("msg:s:'Removing lun %d' sev:n:5 tag:s:SRX_LUN_REMOVED shelf:n:%d lun:n:%d", 
			lb->slot, shelf, lb->slot);

	lb->zerocfg = !sflag;
	/*
	 * RAID state will be synced to drives and LUN will be freed when final ref is released via
	 * putlun in srwrite.
	 */
	
	if (isupd)
		wipeupdate();
}

static long
showspares(void *db, long len)
{
	char *buf, *cp, *ep;
	Rdev *d, *e;

	cp = buf = smalloc(READSTR);
	*cp = 0;
	ep = buf + READSTR;
	if (waserror()) {
		free(buf);
		nexterror();
	}
	qlock(&spares);
	if (waserror()) {
		qunlock(&spares);
		nexterror();
	}
	d = spares.devs;
	e = d + spares.ndevs;
	for (; d<e; d++)
		cp = seprint(cp, ep, "%s %lld\n", d->path, d->length);
	poperror();
	qunlock(&spares);
	len = readstr(0, db, len, buf);
	poperror();
	free(buf);
	return len;
}

static long
rdmasks(Lun *lb, void *db, long len)
{
	char *buf, *p, *e;
	uchar *mp, *me;

	p = buf = smalloc(READSTR);
	*p = 0;
	e = buf + READSTR;
	if (waserror()) {
		free(buf);
		nexterror();
	}
	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	mp = lb->macs;
	me = mp + lb->nmacs*6;
	for (; mp<me; mp+=6)
		p = seprint(p, e, "%E\n", mp);
	poperror();
	spinrunlock(lb);
	len = readstr(0, db, len, buf);
	poperror();
	free(buf);
	return len;
}

static void
setrrfs(char *mnt)
{
	Chan *c;

	qlock(&rrfs);
	if (waserror()) {
		qunlock(&rrfs);
		nexterror();
	}
	c = namec(mnt, Aaccess, OREAD, 0);
	if (c == nil)
		errorstr("unable to namec %s", mnt);
	if (rrfs.rootc)
		cclose(rrfs.rootc);
	rrfs.rootc = c;
	snprint(rrfs.mnt, sizeof rrfs.mnt, mnt);
	poperror();
	qunlock(&rrfs);
}

/* CSS buffer cache interface */

static void
elvreset(Elv *v)
{
	v->ionext = v->ioprev = nil;
	v->head = v->tail = nil;
	v->done = 0;
	v->vnext = nil;
}

static void
elvinit(Elv *v, Rdev *d, void *db, ulong len, uvlong off, int mode)
{
	memset(v, 0, sizeof *v);
	v->mode = mode;
	v->off = off;
	v->db = db;
	v->len = len;
	v->res = 0;
	v->d = d;
}

/* called with elvlock held */
static void
elvwakeup(Rdev *d, int all)
{
	Proc *p;

	while (p = d->dp) {
		d->dp = p->qnext;
		ready(p);
		if (!all)
			break;
	}
}

static void
elvlappend(Elvlist *list, Elv *v)
{
	if (list->head != nil) {
		v->ioprev = list->tail;
		list->tail->ionext = v;
	} else
		list->head = v;
	list->tail = v;
}

static void
elvlinsert(Elvlist *list, Elv *v)
{
	Elv **pp, *f, *p, *r;

	/*
	 * Use forward and reverse traversal pointers to
	 * find the insertion point.  For truly sequential
	 * I/O, we should find our insertion point at the tail.
	 */
	pp = &list->head;
	p = nil;
	r = list->tail;
	for (; f = *pp; pp = &f->ionext) {
		if (f->off > v->off)
			break;
		if (r->off <= v->off) {
			p = r;
			pp = &r->ionext;
			f = *pp;
			break;
		}
		r = r->ioprev;
		p = f;
	}
	if (v->ionext = f)
		f->ioprev = v;
	else
		list->tail = v;
	v->ioprev = p;
	*pp = v;
}

static void
elvinsert(Elv *v)
{
	Proc *p;
	Rdev *d;
	Elvlist *list;

	d = v->d;
	v->dnext = v->ionext = v->dprev = v->ioprev = nil;
	ilock(&d->elvlock);
	v->inserttk = Ticks;

	/* first append to deadline list */
	list = &d->dlist;
	if (list->head != nil) {
		v->dprev = list->tail;
		list->tail->dnext = v;
	} else
		list->head = v;
	list->tail = v;

	/*
	 * Now determine whether to append to i/o list, or insert.
	 * For SSDs, simply append and let the drive sort it
	 * out.  If we're trying to optimize sequential for spinning
	 * disks (or sequential iomode is enabled on non-spinning),
	 * perform sorted insert.
	 */
	list = v->mode == OWRITE ? &d->wlist : &d->rlist;
	if (d->iomode == EMspin || (d->raid->lblade->flags & Lnobuf) == 0)
		elvlinsert(list, v);
	else
		elvlappend(list, v);

	if (p = d->dp)
		d->dp = p->qnext;
	iunlock(&d->elvlock);
	if (p)
		ready(p);
	ainc(&d->raid->lblade->castats[LCaselvq].stat);
}

static void
elvwait(Elv *v)
{
	Rdev *d;
	Raid *r;
	Lun *lb;

	d = v->d;
	r = d->raid;
	lb = r->lblade;
	ilock(v);
	if (v->done) {
		iunlock(v);
		return;
	}
	if (v->head == nil)
		v->head = up;
	else
		v->tail->qnext = up;
	v->tail = up;
	up->qnext = nil;
	up->state = Queueing;
	up->qpc = getcallerpc(&v);
	iunlock(v);
	ainc(&lb->castats[LCaselvwait].stat);
	sched();
	adec(&lb->castats[LCaselvwait].stat);
}

static void
elvdone(Elv *v)
{
	Proc *p, *q;

	ilock(v);
	if (v->done)
		panic("elvdone");
	q = v->head;
	v->head = nil;
	v->done = 1;
	iunlock(v);

	/*
	 * This is tricky; we cannot reference the Elv once it has
	 * been marked as done as it may be living on the stack.
	 */
	while (p = q) {
		q = p->qnext;
		ready(p);
	}
}

static void
elvgrpadd(Elvgrp *g, Elv *v)
{
	if (g->head == nil)
		g->head = v;
	else
		g->tail->vnext = v;
	g->tail = v;
}

static void
elvgrpinsert(Elvgrp *g)
{
	Elv *v;

	v = g->head;
	while (v) {
		elvinsert(v);
		v = v->vnext;
	}
}

static void
elvgrpwait(Elvgrp *g)
{
	Elv *v;

	v = g->head;
	while (v) {
		elvwait(v);
		v = v->vnext;
	}
}

/* remove from deadline list */
static void
deadunlink(Elv *x)
{
	Rdev *d;

	d = x->d;
	if (x->dprev == nil)
		d->dlist.head = x->dnext;
	else
		x->dprev->dnext = x->dnext;
	if (x->dnext == nil)
		d->dlist.tail = x->dprev;
	else
		x->dnext->dprev = x->dprev;
}

static void
iounlink(Elv *x)
{
	Elv **h, **t;

	if (x->mode == OWRITE) {
		h = &x->d->wlist.head;
		t = &x->d->wlist.tail;
	} else {
		h = &x->d->rlist.head;
		t = &x->d->rlist.tail;
	}
	if (x->ioprev == nil)
		*h = x->ionext;
	else
		x->ioprev->ionext = x->ionext;
	if (x->ionext == nil)
		*t = x->ioprev;
	else
		x->ionext->ioprev = x->ioprev;
}

/* check deadline list and move any that are expired to the active list */
static Elv *
raisethedead(Rdev *d)
{
	ulong m, n;
	Elv *x, *y, **xx;

	m = Ticks;
	for (x = d->dlist.head; x; x = x->dnext) {
		n = TK2MS(m - x->inserttk);
		if (n < nelvdeadms)
			break;

		deadraised++;

		/* remove from deadline, io lists */
		deadunlink(x);
		iounlink(x);

		/* insert into active list */
		xx = &d->ahead;
		for (; y=*xx; xx = &y->ionext)
			if (y->off > x->off)
				break;
		x->ionext = y;
		*xx = x;
	}

	return d->ahead;
}

/* called with elvlock held */
static void
dpwait(Rdev *d)
{
	d->dpworking--;
	up->qnext = d->dp;
	d->dp = up;
	up->state = Queueing;
	up->qpc = getcallerpc(&d);
	iunlock(&d->elvlock);
	sched();
	ilock(&d->elvlock);
	d->dpworking++;
}

/* called with ilock(&d->elvlock) held, protecting d->dpmachno */
static void
dpswitchck(Rdev *d)
{
	for (;;) {
		if (d->dpmachno == m->machno) {
			/*
			 * if interrupt load goes above dpintrthresh, find another core
			 */
			if ((m->perf.avg_inintr*100) / m->perf.period < dpintrthresh)
				return;
			dpmachno(d);
		}
		procwired(up, d->dpmachno);
		iunlock(&d->elvlock);
		yield();
		ilock(&d->elvlock);
	}
}

static int
elvpullup(Rdev *d)
{
	int n, cnt, *acct;
	ulong nread, nwrite;
	uvlong *lasteoff;
	Elvlist *list;
	Elv **xx, *x;

	/* Alternate reading/writing up to our pre-established limits. */
	acct = nil;
	switch (d->elvmode) {
	default:
	case ESread:
r:		nread = isdirect(d->raid, OREAD) ? nelvdread : nelvread;
		list = &d->rlist;
		if (d->reading < nread) {
			if (list->head != nil) {
				if (d->wlist.head != nil)
					acct = &d->reading;
				cnt = nread - d->reading;
				lasteoff = &d->lastroff;
				break;
			} else if (0 && TKDELTA2MS(d->wlist.head->inserttk) < elvdelayms)
				return 0;
		}
		d->elvmode = ESwrite;
		d->writing = 0;
		/* fall thru */
	case ESwrite:
		nwrite = isdirect(d->raid, OWRITE) ? nelvdwrite : nelvwrite;
		list = &d->wlist;
		if (d->writing < nwrite) {
			if (list->head != nil) {
				if (d->rlist.head != nil)
					acct = &d->writing;
				cnt = nwrite - d->writing;
				lasteoff = &d->lastwoff;
				break;
			} else if (0 && TKDELTA2MS(d->rlist.head->inserttk) < elvdelayms)
				return 0;
		}
		/* fall thru */
		d->elvmode = ESread;
		d->reading = 0;
		goto r;
	}
	x = list->head;
	if (d->iomode == EMspin) {
		n = x->off - *lasteoff;
		if (n > 0 && n < elvdelaygap)		/* delay for sequential workload */
		if (TKDELTA2MS(x->inserttk) < elvdelayms)
			return 0;
	}
	if (*lasteoff != x->off) {
		if (x->mode == OWRITE)
			d->wrandom++;
		else
			d->rrandom++;
	}
	n = 0;
	do {
		deadunlink(x);
		*lasteoff = x->off + x->len;
		xx = &x->ionext;
		x = *xx;
		n++;
	} while (n < cnt && x && (d->iomode == EMssd || *lasteoff == x->off));
	d->ahead = list->head;
	*xx = nil;	/* set end of ahead list */
	if (list->head = x)
		x->ioprev = nil;
	if (acct)
		*acct += n;
	return n;
}

static Elv *
elvnextio(Rdev *d, int maxio)
{
	Elv *v, *vx, **vv;
	uvlong xoff;
loop:
	ilock(&d->elvlock);
	for (;;) {
		if (d->closed) {
			iunlock(&d->elvlock);
			return nil;
		}
		if (d->iomode == EMspin)
			dpswitchck(d);
		if (d->ahead)
			goto out;
		if (d->dlist.head != nil)
			break;
		dpwait(d);
	}

	/*
	 * Determine if the deadline list head needs to be
	 * serviced.  Returns ahead which is != nil if any
	 * Elv items need immediate priority.
	 */
	if (raisethedead(d) != nil)
		goto out;

	/*
	 * Otherwise we need to pull up some i/o from our read/write
	 * lists onto our active head.
	 */
	if (elvpullup(d) == 0) {		/* forced delay yield */
		iunlock(&d->elvlock);
		yield();
		goto loop;
	}
out:
	v = d->ahead;
	vv = &v->ionext;
	xoff = v->off + v->len;
	while (--maxio > 0 && (vx = *vv) && vx->off == xoff) {
		xoff = vx->off + vx->len;
		vv = &vx->ionext;
	}
	d->ahead = *vv;
	*vv = nil;

	iunlock(&d->elvlock);
	adec(&d->raid->lblade->castats[LCaselvq].stat);
	return v;
}

static void
elvclose(Rdev *d)
{
	ilock(&d->elvlock);
	d->closed = 1;
	elvwakeup(d, 1);
	iunlock(&d->elvlock);
}

static int
dpwlockbail(void *vp)
{
	Rdev *d;

	d = vp;
	return d->closed || (d->flags & Dfailed);
}

/*
 * (*kpc) ok - this is shifty.  The problem here is that we can
 * have a deadlock problem since we queue all i/o up for dprocs.
 * The dprocs have to be able to call raidshield in order to
 * correct errors on prefetch i/o, but if they do then we can get
 * in a situation where the dprocs wait forever to obtain a wlock
 * on a raid because the lbworkers have it rlocked waiting for
 * dprocs to finish i/o for them.  This bit of shiftiness fixes
 * the problem by guaranteeing a dproc is always available to do
 * i/o.  It could be a recursive nightmare ...  but what are the
 * odds?  (famous last words)
 */
static void
dproc(void *vp)
{
	Raid *r;
	Rdev *d;
	Elv *v;
	uvlong roff;
	ulong flags, iotime;
	IOchunk *vec;
	int maxvec;

	ainc(&dprocactive);
	maxvec = niovec;
	d = vp;
	r = d->raid;
	while ((vec = malloc(sizeof *vec * maxvec)) == nil) {
		tsleep(&up->sleep, return0, 0, 1000);
		if (d->closed)
			goto out;
	}
	while ((v = elvnextio(d, maxvec)) != nil) {
		roff = v->off - r->doff; /* roff is the offset in raid, i.e disk offset - disk config area */
		flags = rbufiotime(v, vec, maxvec, &iotime);
		USED(flags);
		if (niodeadsecs && iotime > niodeadsecs)
		if ((d->flags & Dfailed) == 0)
		if (setfailfast(d) != 0) {	/* i am the first */
			jskevent( "msg:s:'I/O response time of %uld secs exceeded threshold of %uld secs on device %s.  Going to fail device.'"
				" sev:n:2 tag:s:SRX_DRIVE_IO_RESPONSE_THRESHOLD_EXCEEDED"
				" %S lun:n:%d raidpart:n:%d drivepart:n:%d",
				iotime, niodeadsecs, d->name, d, r->lblade->slot, r->rno, d->dno);	
			ainc(&d->ndp);
			kproc(up->text, dproc, d);	/* clone myself */
			snprint(up->errstr, ERRMAX, "%s:%p:%ulld", DEVF, d, roff);
			if (spinwlockbailfn(r, dpwlockbail, d) != 0) {
				faildev(d);
				setrflags(r);
				spinwunlock(r);
				schedsave();
			}
			break;
		}
#ifdef notdef
		if ((flags & (Bfailed|Bghost)) == Bfailed) {	/* (*kpc) */
			ainc(&d->ndp);
			kproc(up->text, dproc, d);	/* clone myself */
			snprint(up->errstr, ERRMAX, "%s:%p:%ulld", DEVF, d, roff);
			if (spinwlockbailfn(r, dpwlockbail, d) != 0) {
				raidshield0(r, roff/nbsize);
				spinwunlock(r);
			}
			break;
		}
#endif
	}
out:	adec(&d->ndp);
	free(vec);
	adec(&dprocactive);
	pexit(nil, 1);
}

static Elv *
bp2elv(Buf *bp)
{
	Elv *v = bp;
	Rdev *d = bp->d;
	Raid *r = d->raid;

	v->mode = (bp->flags & Bmod) ? OWRITE : OREAD;
	v->off = nbsize*bp->bno + r->doff;
	v->db = bp->db;
	v->len = nbsize;
	v->res = 0;
	v->bp = bp;
	return v;
}

/* called with cache line lock held */
static void
bufio(Buf *bp)
{
	Elv *v;

	bp->flags |= Bio;
	v = bp2elv(bp);
	elvreset(v);
	elvinsert(v);
}

static int
elv2iovec(Elv *v, IOchunk *vec, int nvec, long *len)
{
	int n;
	long tlen;

	tlen = 0;
	for (n = 0; v && n < nvec; v = v->ionext, n++) {
		vec->addr = v->db;
		tlen += vec->len = v->len;
		vec++;
	}
	assert(v == nil);
	*len = tlen;
	return n;
}

static int
elvcomplete(Elv *v, long success)
{
	int flags;
	Buf *bp;

	flags = 0;
	v->res = success ? v->len : 0;
	if (bp = v->bp) {
		lock(bp->clk);
		bp->flags &= ~Bfailed;
		if (success == 0)
			bp->flags |= Bfailed;
		flags |= bp->flags & (Bfailed|Bghost);
		bp->flags &= ~(Bio|Bsync|Bimm|Bmod);
		bp->modts = 0;
		elvdone(v);
		unlock(bp->clk);
	} else
		elvdone(v);
	return flags;
}
		
static int
rbufiotime(Elv *v, IOchunk *vec, int nvec, ulong *iotime)
{
	int n;
	Chan *c;
	Rdev *d;
	int flags;
	ulong lastio;
	long len;
	Elv *vx;

	n = 0;
	len = -1;
	d = v->d;
	if (d->failfast)
		goto out;
	lastio = Ticks;
	if (!waserror()) {
		c = d->c;
		nvec = elv2iovec(v, vec, nvec, &len);
		if (v->mode == OWRITE) {
			devwrites++;
			n = devtab[c->type]->writev(c, vec, nvec, len, v->off);
		} else {
			devreads++;
			n = devtab[c->type]->readv(c, vec, nvec, len, v->off);
		}
		poperror();
	}
	if (iotime)
		*iotime = TK2SEC(Ticks - lastio);
out:
	flags = 0;
	while (vx = v) {
		v = vx->ionext;
		flags |= elvcomplete(vx, n == len);	/* all or nothing for now */
	}
	return flags;
}

static int
rbufio(Elv *v)
{
	IOchunk vec;

	return rbufiotime(v, &vec, 1, nil);
}

static void
checkdbuf(void *vp)
{
	Rdev *d;
	Buf *b, *be;
	int i;
	ulong n, lastn, lastsweep[Nhbufs];
	Lock *lk;
	enum { Nlockms	= 3*1000, };

	d = vp;
	lastn = 0;
	memset(lastsweep, 0, sizeof lastsweep);

	while (waserror())
		dprint("checkdbuf: %s\n", up->errstr);
loop:
	tsleep(&up->sleep, return0, 0, 1000);

	/*
	 * If the elevator has closed, we should take a hike.
	 */
	if (d->closed) {
		poperror();
		adec(&d->ndp);
		pexit("", 1);
	}
	n = Ticks;
	if (n < lastn) {
		/*
		 * btime wrapped.  reset all timestamps in buffer
		 * cache.  it's too hard to get right.
		 */
		resetts(d, n);
		goto loop;
	}
	for (i = 0; i < Nhbufs; i++) {
		lk = d->buflks + i;
		if (TK2MS(n - lastsweep[i]) > Nlockms)
			lock(lk);
		else if (!canlock(lk))
			continue;
		if (waserror()) {
			unlock(lk);
			nexterror();
		}
		b = d->bufs + i*Nhwidth;
		be = b + Nhwidth;
		for (; b<be; b++) {
			if (!BUFBAD(b))
			if (b->flags & Bmod)
			if (TK2MS(n - b->putts) >= (dirtysecs*1000))
			if (!BUFBUSY(b))
				bufio(b);
		}
		lastsweep[i] = n;
		poperror();
		unlock(lk);
	}
	goto loop;
}

/* drop the buffer cache blocks that are not busy or dirty */
static void
dropcache(void)
{
	Lun *lb;
	Raid *r, *re;
	Rdev *d, *de;
	Buf *b, *be;
	int i;

	spinrlock(&lblades);
	lb = lblades.head;
	for (; lb; lb=lb->next) {
		spinrlock(lb);
		r = lb->raids;
		re = r + lb->nraids;
		for (; r<re; r++) {
			spinrlock(r);
			d = r->devs;
			de = d + r->ndevs;
			for (; d<de; d++) {
				for (i=0; i < Nhbufs; i++) {
					b = d->bufs + i*Nhwidth;
					be = b + Nhwidth;
					lock(&d->buflks[i]);
					for (; b<be; b++) {
						if (!BUFBUSY(b))
						if (!(b->flags & Bmod))
							b->d = nil;
					}
					unlock(&d->buflks[i]);
				}
			}
			spinrunlock(r);
		}
		spinrunlock(lb);
	}
	spinrunlock(&lblades);
}

static void
emulate4kn(Lun *lb, int on)
{
	lb->emuflags = on ? lb->emuflags|EE4kn : lb->emuflags&~EE4kn;
	setlbgeometry(lb);
}

static void
sync(void)
{
	Lun *lb;

	spinrlock(&lblades);
	if (waserror()) {
		spinrunlock(&lblades);
		nexterror();
	}
	for (lb=lblades.head; lb; lb=lb->next)
		lsync(lb);
	poperror();
	spinrunlock(&lblades);
}

static void
flushcache(Lun *lb, int on)
{
	spinwlock(lb);
	if (on)
		lb->flags |= Lflushcache;
	else
		lb->flags &= ~Lflushcache;
	spinwunlock(lb);
	schedsave();
}

static void
lsync(Lun *lb)
{
	Raid *r, *e;

	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	r = lb->raids;
	e = r + lb->nraids;
	for (; r<e; r++)
		rsync(r);
	poperror();
	spinrunlock(lb);
}

static void
rsync(Raid *r)
{
	Rdev *d, *e;

	spinwlock(r);
	if (waserror()) {
		spinwunlock(r);
		nexterror();
	}
	d = r->devs;
	e = d + r->ndevs;
	for (; d<e; d++)
		dsync(d);
	poperror();
	spinwunlock(r);
}

static void
dsync(Rdev *d)
{
	Buf *bp, *e;
	Lock *lk;
	int i;

	bp = d->bufs;
	e = bp + Nbufs;
	for (i=0; i<Nhbufs; i++) {
		bp = d->bufs + i*Nhwidth;
		e = bp + Nhwidth;
		lk = d->buflks + i;
		lock(lk);
		if (waserror()) {
			unlock(lk);
			nexterror();
		}
		for (; bp<e; bp++) {
			if (!BUFBAD(bp))
			if (!BUFBUSY(bp))
			if (bp->flags & Bmod) {
				bp->flags |= Bsync;
				bufio(bp);
			}
		}
		poperror();
		unlock(lk);
	}
	for (bp=d->bufs; bp<e; bp++)
		while (bp->flags & Bsync)
			tsleep(&up->sleep, return0, 0, 10);
	devflush(d);
}

static char *
bpdump(Buf *bp, char *p, char *e)
{
	for (; bp; bp=bp->next)
		p = seprint(p, e, "[%c %lld]%c", (bp->flags & Bmod) ? 'w' : 'r', bp->bno, bp->next ? ' ' : '\n');
	return p;
}

/*
 * Due to not acquiring cache line locks these stats are best effort and
 * do not reflect a complete snapshot of the cache state when read.
 */
static long
bufstats(void *db, ulong len)
{
	Lun *lb;
	Raid *r, *re;
	Rdev *d, *de;
	Buf *b, *be;
	char *p, *ep;
	int i;
	struct {
		ulong n;
		char nm[16];
	} stats[] = {
		0, "Bread",		/* 0 */
		0, "Bmod",
		0, "Bimm",
		0, "Bghost",
		0, "Bfailed",		/* 4 */
		0, "Bio",
		0, "Bsync",
		0, "Brecon",
		0, "Brefreshed",	/* 8 */
		0, "Bwrite",
		0, "Bpreget",
		0, "ref",
		0, "nodev",		/* 12 */
	};

	p = db;
	ep = p + len;
	spinrlock(&lblades);
	for (lb=lblades.head; lb; lb=lb->next) {
		spinrlock(lb);
		r = lb->raids;
		re = r + lb->nraids;
		for (; r<re; r++) {
			spinrlock(r);
			d = r->devs;
			de = d + r->ndevs;
			for (; d<de; d++) {
				b = d->bufs;
				be = b + Nbufs;
				for (; b<be; b++) {
					for (i=0; i<=10; i++)
						if (b->flags & 1<<i)
							stats[i].n++;
					if (b->ref)
						stats[11].n++;
					if (b->d == nil)
						stats[12].n++;
				}
				p = seprint(p, ep, "%s:\n", d->name);
				for (i=0; i<nelem(stats); i++) {
					p = seprint(p, ep, "%s=%ld%c",
						stats[i].nm, stats[i].n, i && i%4 == 0 ? '\n' : ' ');
					stats[i].n = 0;
				}
			}
			spinrunlock(r);
		}
		spinrunlock(lb);
	}
	spinrunlock(&lblades);
	return p - (char *) db;
}

/*
 * returns 1 if n is prime
 * used for adjusting lengths
 * of hashing things.
 * there is no need to be clever
 */
static int
prime(long n)
{
	long i;

	if ((n % 2) == 0)
		return 0;
	for (i = 3;; i += 2) {
		if ((n % i) == 0)
			return 0;
		if (i*i >= n)
			return 1;
	}
}

/*
 * Return the largest power of 2 less than or equal to v.
 * Please give me a better function name and obviate this comment.  :)
 */
static uvlong
base2floor(uvlong v)
{
	int i;

	if (v == 0)
		return 0;
	for (i=0; v>1; i++)
		v >>= 1;
	return 1<<i;
}

static void
resetts(Rdev *d, ulong ts)
{
	Buf *bp, *e;

	bp = d->bufs;
	e = bp + Nbufs;
	for (; bp<e; bp++)
		bp->getts = bp->putts = bp->modts = ts;
}

static void
putbuf(Buf *bp)
{
	Rdev *d;

	d = bp->d;

	assert(bp->ref > 0);
	lock(bp->clk);
	if (waserror()) {
		unlock(bp->clk);
		nexterror();
	}
	do {
		if (--bp->ref > 0)
			break;
		bp->putts = Ticks;
		bp->tss = ainc((long*)&d->tss->stat);
		if (BUFBAD(bp)) {
			if ((bp->flags & (Bghost|Binval|Bfailed)) == Bghost)
				break;		/* leave ghost buffers for future lookups */
			bp->d = nil;
			bp->flags = 0;
			break;
		}
		if (bp->flags & Bmod)
		if ((bp->flags & Bimm) || TK2MS(Ticks - bp->modts) > (maxdsecs*1000))
			bufio(bp);
	} while (0);
	poperror();
	unlock(bp->clk);
}

#define LTS(bp) ((bp)->getts > (bp)->putts ? (bp)->getts : (bp)->putts)

/* return which bp arg is older.  If timestamps are the same, use counter. */
static int
bpolder(Buf *rp, Buf *bp)
{
	ulong rps, bps;

	rps = LTS(rp);
	bps = LTS(bp);
	if (rps == bps)
		return bp->tss < rp->tss;
	return bps < rps;
}

/*
 * rgetbuf fetch status.  Either the requested buffer is found, or we
 * have elected one to relabel, or we can't find any to relabel and
 * must ask the user to try again.
 */
enum {
	Sfound,
	Srelabel,
	Sagain,
	Snotfound,
};

/* identify the buffer cache lock for the device block no */
static Lock *
bno2clk(Rdev *d, uvlong bno)
{
	return d->buflks + bno%Nhbufs;
}

/*
 * This is done without the lock for the cache line intentionally.
 * We can walk the cache line without the lock due to the array
 * management for the cache line (and no reordering), and since
 * we don't need a handle to the buffer it doesn't matter if
 * we catch it in an inconsistent state.  It would be inconsistent
 * anyway since a lock, peek, unlock still leaves a chance for the
 * buffer to vanish once we unlock.
 */
static int
dcpeek(Rdev *d, uvlong bno)
{
	return rgetbuf(d, bno, nil, 1);
}

/*
 * Look in the cache for the buf requested.  If not found, return the
 * oldest unused buf in the hash line that can be relabed.  The returned
 * buf may need to be written out.  
 * 
 * quick check is really just a way to know if something is in the
 * cache without screwing with the timestamps.  Used by prespec.
 *
 * Called with lock for cache line held.
 */
static int
rgetbuf(Rdev *d, uvlong bno, Buf **bpp, int quick)
{
	Buf *bp, *rp, *ibp, *ep, *owp, *orp;
	int h, s, nmod;

	h = bno % Nhbufs;
	bp = ibp = d->bufs + h*Nhwidth;
	ep = bp + Nhwidth;
	s = Sfound;
	for (; bp<ep; bp++) {
		/* can't use BUFBAD because we want to find Bghost buffers */
		if (bp->d == nil || bp->bno != bno || (bp->flags & (Binval | Bfailed)))
			continue;
		if (quick) {
			if (bpp)
				*bpp = bp;
			return s;
		}
		goto found;
	}
	if (quick)
		return Snotfound;
	s = Srelabel;
	rp = owp = orp = nil;
	nmod = 0;
	for (bp--; bp>=ibp; bp--) {	/* not found -- select one for relabel */
		if (bp->lastbno == bno)
			bpfltoosoon++;
		if (BUFBUSY(bp))
			continue;
		if (bp->d == nil) {	/* buffer unassigned.  Run with it. */
			rp = bp;
			break;
		}
		if (bp->flags & Bmod) {
			nmod++;
			if (owp == nil || bpolder(owp, bp))
				owp = bp;
		} else if (orp == nil || bpolder(orp, bp))
			orp = bp;
	}
	if (rp == nil) {
		if (orp == nil && owp == nil) 	/* bugger.  we're full. */
			return Sagain;
		rp = orp;
		if (rp != nil) {
			if (!rgbmodbias && owp)
			if (bpolder(rp, owp))
				rp = owp;
		} else
			rp = owp;
	}
	if (owp && rp != owp)
	if (nmod >= Nhwidth/2)
	if (!BUFBAD(owp))
		bufio(owp);	/* flush the oldest unused dirty buffer if we're over half dirty */
	bp = rp;
found:
	bp->getts = Ticks;
	bp->tss = ainc((long*)&d->tss->stat);
	*bpp = bp;
	return s;
}

static uvlong
numblocks(Rdev *d)
{
	return d->raid->mindlen / nbsize;
}

static void
prespec(Rdev *d, uvlong bno)
{
	uvlong pbno, nbno;

	if (bno == 0 || bno >= numblocks(d))
		return;
	pbno = prevbno(d, bno);
	if (dcpeek(d, pbno) != Sfound)
		return;
	nbno = nextbno(d, bno);
	if (dcpeek(d, nbno) == Sfound)
		return;
	preget(d, nbno);
}

static void
preget(Rdev *d, uvlong bno)
{
	Buf *bp;
	Lock *lk;
	uvlong *vn;

	if (bno >= numblocks(d))
		return;
	vn = d->pregetlh + bno%Npglh;
	if (*vn == bno)		/* lock contention optimization */
		return;
	lk = bno2clk(d, bno);
	lock(lk);
	if (*vn == bno)		/* lock contention optimization */
		goto end;
loop:
	switch (rgetbuf(d, bno, &bp, 0)) {
	case Sagain:
		pregetlinefulls++;
		break;
	case Sfound:
		pregethits++;
		if (bp->flags & Bio) {
			pregetbios++;
			if (bp->flags & Bmod)
				pregetbiomods++;
		}
		break;
	case Srelabel:
		if (!BUFBAD(bp))
		if (bp->flags & Bmod) {
			collisions++;
			bufio(bp);
			goto loop;
		}
//dprint("pr%d %lld %lld\n", d->dno, bp->bno, bno);
		if (bp->d)
		if (bp->d != d)
			print("whoa! - %d != %d\n", ((Rdev *)bp->d)->dno, d->dno);
		bp->d = d;
		bp->flags = 0;
		bp->modts = 0;
		bp->lastbno = bp->bno;
		bp->bno = bno;
		bufio(bp);
		*vn = bno;
	}
end:
	unlock(lk);
}

static void
invalbuf(Rdev *d, uvlong bno)
{
	Buf *bp;
	Lock *lk;

	lk = bno2clk(d, bno);
	lock(lk);
	if (rgetbuf(d, bno, &bp, 1) == Sfound) {
		if (BUFBUSY(bp))
			bp->flags |= Binval;
		else {
			bp->d = nil;	/* take away the d and it looks unassigned (like startup) */
			bp->flags = 0;
		}
	}
	unlock(lk);
}

static void
invalbufs(Rdev *d, uvlong off, long len)
{
	uvlong bno;
	long boff;
	long bcnt;

	bno = off / nbsize;
	boff = off % nbsize;
	bcnt = nbsize - boff;
	invalbuf(d, bno);
	if (bcnt < len)
		invalbuf(d, bno+1);
}

static char *Etoolong = "too long in getbuf";

static Buf *
getbuf(Rdev *d, uvlong bno, int flags)
{
	Buf *bp;
	int c, pg, i;
	Lock *lk;

	c = 0;
	getbufs++;
again:
	lk = bno2clk(d, bno);
	lock(lk);
	if (waserror()) {
		unlock(lk);
		nexterror();
	}
reget:
	switch (rgetbuf(d, bno, &bp, 0)) {
	case Sagain:
		if (c == 0) {
			c++;
			rlinefulls++;
		} else if (c++ > ngiveup) {
			giveups++;
			error(Etoolong);
		}
		linefulls++;
		poperror();
		unlock(lk);
		yield();
		goto again;
	case Srelabel:
		if (c == 0) {
			c++;
			if (flags & Bread)
				nomatches++;
		}
		if (!BUFBAD(bp))
		if (bp->flags & Bmod) {	/* flush the modified buffer before reusing */
			collisions++;
			bufio(bp);
			goto reget;
		}
		bp->ref++;
		if (bp->d)
		if (bp->d != d)
			print("whoa! - %d != %d\n", ((Rdev *)bp->d)->dno, d->dno);
		bp->d = d;
		bp->flags = 0;
		bp->modts = 0;
		bp->lastbno = bp->bno;
		bp->bno = bno;
		if (flags & Bread)
			bufio(bp);
		break;
	case Sfound:
		bp->ref++;
		cachehits++;
	}

	/*
	 * If the device is bad then don't bother prefetching.  This avoids a loop
	 * where we continually prefetch data on a bad disk (async proc calls
	 * raidshield, which triggers async prefetch, which calls raidshield, ...)
	 */
	pg = 0;
	if (!DEVBAD(d))
	if ((flags & (Bread|Bmod)) == Bread)
	if ((bp->flags & (Bpreget|Bio)) == Bio)	/* not prefetched and we will wait below */
	if (!(d->raid->flags & Rdegraded)) {
		bp->flags |= Bpreget;
		pg = npreget;
	}

	/*
	 * If the Buf is currently under i/o, go ahead and do any
	 * prefetching here.
	 */
	if (bp->flags & Bio) {
		if (bp->flags & Bmod)
			bpmodio++;
		unlock(bp->clk);
		for (i = 0; i < pg; i++)
			prespec(d, bno+i);
		elvwait(bp2elv(bp));
		lock(bp->clk);
	}

	if ((bp->flags & Bghost) == 0)
	if (flags & Bread) {
		if ((bp->flags & Bfailed)) {
			bp->ref--;
			errorstr("%s:%p:%ulld", DEVF, d, bno*nbsize);
		}
		if (flags & Bmod)
		if (bp->modts == 0)
			bp->modts = Ticks;
		bp->flags |= (flags & (Bmod|Bimm));
	} else
		bp->flags |= Bghost;

	poperror();
	unlock(lk);
	return bp;
}

/* CSS begin raid interface */

static uchar
gfmult(uchar a, uchar b)
{
	int m;

	if (a == 0 || b == 0)
		return 0;
	m = gflog[a] + gflog[b];
	if (m >= 255)
		m -= 255;
	return gfilog[m];
}

static uchar
gfexp(uchar a, int e)
{
	uchar r = 1;

	while (e) {
		if (e & 1)
			r = gfmul[r][a];
		a = gfmul[a][a];
		e >>= 1;
	}
	return r;
}

static void
rsr6_tabinit(void)
{
	uchar b;
	int i, j;

	/* generate gflog table used by gfmult and gfilog table used at run-time */
	b = 1;
	for (i=0; i<256; i++) {
		gflog[b] = i;
		gfilog[i] = b;
		b = (b << 1) ^ (b & 0x80 ? 0x1d : 0);
	}
	/* calc multiplication tables */
	for (i=0; i<256; i++)
	for (j=0; j<256; j++)
		gfmul[i][j] = gfmult(i, j);
	/* calc x^-1 == x^-254 table */
	for (i=0; i<256; i++)
		gfinv[i] = gfexp(i, 254);
	/* calc exponent-xor-inverse table -- the latter half of the A,B functions */
	/* (g^z + {01})^-1 where z is y-x */
	for (i=0; i<256; i++)
		gfexi[i] = gfinv[gfilog[i] ^ 1];
}

/*
 * Called with r wlocked.
 * this function is not permitted to throw an error as it is used
 * in error handling.
 */
static void
setrflags(Raid *r)
{
	Rdev *d, *e;
	int bad, clean, flags;

	bad = clean = 0;
	flags = r->flags;
	r->flags &= ~(Rdegraded|R2degraded);
	d = r->devs;
	e = d + r->ndevs;
	for (; d<e; d++) {
		if (d->flags & Dclean)
			clean++;
		if (!DEVBAD(d))
			continue;
		switch (r->type) {
		case RTnil:
		case RTfnil:
		case RTraw:
		case RTjbod:
		case RTraidl:
		case RTraid0:
			break;
		case RTraid1:
		case RTraid10:
			if (DEVBAD(dmirror(d)))
				break;
			bad++;
			continue;
		case RTraid5:
			if (++bad > 1 || (r->flags & Riniting))
				break;
			continue;
		case RTraid6rs:
			if (++bad > 2 || (r->flags & Riniting))
				break;
			continue;
		default:
			print("setrflags: %d is not a raid type!", r->type);
		}
		r->flags |= Rfailed;
		if ((flags & Rfailed) == 0)	/* freshly failed */
			jskevent( "msg:s:'unrecoverable failure on raid %d.%d'"
				" sev:n:1 tag:s:SRX_RAID_FAILURE shelf:n:%d lun:n:%d raidpart:n:%d",
				r->lblade->slot, r->rno, shelf, r->lblade->slot, r->rno);	
		return;
	}
	if (clean == r->ndevs - bad)
		r->flags |= Rclean;
	if (bad--) {
		r->flags |= Rdegraded;
		if (r->type == RTraid6rs && bad)
			r->flags |= R2degraded;
	}
}

static Rtype
raidtype(char *rtype)
{
	if (!strcmp(rtype, "nil"))
		return RTnil;
	if (!strcmp(rtype, "fnil"))
		return RTfnil;
	if (!strcmp(rtype, "raw"))
		return RTraw;
	if (!strcmp(rtype, "jbod"))
		return RTjbod;
	if (!strcmp(rtype, "raidL") || !strcmp(rtype, "raidl"))
		return RTraidl;
	if (!strcmp(rtype, "raid0"))
		return RTraid0;
	if (!strcmp(rtype, "raid1"))
		return RTraid1;
	if (!strcmp(rtype, "raid5"))
		return RTraid5;
	if (!strcmp(rtype, "raid6rs"))
		return RTraid6rs;
	if (!strcmp(rtype, "raid10"))
		return RTraid10;
	errorstr("unsupported raid type %s", rtype);
	return 0xbadcc;
}

static void
ndevtypesanity(int ndevs, Rtype type)
{
	if (ndevs > Nrdevs)
		error("too many devices");
	if (ndevs <= 0)
		error("no devices");
	if ((type == RTraw && ndevs != 1)
	|| (type == RTjbod && ndevs > 1)
	|| (type == RTraid1 && ndevs != 2)
	|| (type == RTraid5 && ndevs < 2)
	|| (type == RTraid6rs && ndevs < 3)
	|| (type == RTraid10 && (ndevs & 1)))
		error("wrong number of devices");
}

static void
magicsanity(Raid *r, int argc, char **argv)
{
	Lun *lb;
	char name[KNAMELEN+1], *p;
	uchar buf[512];
	int n;

	if (!cansave(r))
		return;
	lb = r->lblade;
	if (lb->hasmagic)
		return;
	for (; argc > 0; argc--, argv++) {
		if (strcmp(*argv, "missing") == 0)
			continue;
		strncpy(name, *argv, sizeof name);
		p = strchr(name, ':');	/* rrestore */
		if (p != nil)
			*p = '\0';
		n = creadonce(name, buf, sizeof buf);
		if (n < sizeof buf)
			errorstr("can't read from drive %d.%d", shelf, parsedrive(name));
		if (memcmp(buf, Srmagic00, Nmagic) == 0)
		if (memcmp(lb->magic, buf, Nmagic) != 0)
			error(Ebadmagic);
	}
}

/*
 * In creating a raid we are given the path to the data file for each
 * device we use as a raid element.  This path is never actually specified
 * by the user and is derived based on the shelf.slot user interface
 * specification.  For event messages we'd like to generate names that
 * the user can relate to so we translate the path back to the disk name
 * they expect.  Eventually this will get pushed out of this code to a
 * higher error/alarm handling level, but for now we hack it in here.
 * 
 * Parsedrive assumes the disk path is of the form '/mntpt/[0-9]+/data'.
 * If it doesn't find this and can't determine the slot, we set the name
 * to the disk path.
 * 
 */
static void
setdevslotattrs(Rdev *d)
{
	d->slot = parsedrive(d->path);
	if (d->slot != -1) {
		snprint(d->name, nelem(d->name), "%d.%d", shelf, d->slot);
		snprint(d->rdpath, sizeof d->rdpath, "#‡/%d", d->slot);
	} else {
		snprint(d->name, nelem(d->name), d->path);
		d->rdpath[0] = 0;
	}
}

static void
devcachefree(Rdev *d)
{
	free(d->bufs[0].db);		/* frees entire cache */
	free(d->tss);
	d->tss = nil;
	memset(d->bufs, 0, sizeof d->bufs);
	memset(d->buflks, 0, sizeof d->buflks);
}

static void
devcacheinit(Rdev *d)
{
	Buf *bp;
	uchar *alloc;
	int i;

	alloc = d->bufs[0].db;		/* memory cache from previous alloc */
	memset(d->bufs, 0, sizeof d->bufs);
	memset(d->buflks, 0, sizeof d->buflks);

	if (alloc == nil) {
		alloc = mallocalign(nbsize*Nbufs, BY2PG, 0, 0);
		d->tss = mallocalign(sizeof (Castat), CACHELINESZ, 0, 0);
		if (alloc == nil || d->tss == nil) {
			free(alloc);
			free(d->tss);
			errorstr("cannot alloc cache for %s", d->name);
		}
	}

	for (i = 0; i < Nbufs; i++) {
		bp = d->bufs + i;
		bp->clk = d->buflks + i/Nhwidth;
		bp->dbidx = -1;
		bp->db = alloc;
		alloc += nbsize;
	}
}

static void
dpmachno(Rdev *d)
{
	static long machnos;

	d->dpmachno = ainc(&machnos) % conf.nmach;
}

static void
devkpinit(Rdev *d)
{
	int i;
	int ndp;

	ndp = ndprocs;
	d->ndp = ndp+1;	/* checkdbuf */
	snprint(up->genbuf, sizeof up->genbuf, "checkdbuf_%s", d->name);
	kproc(up->genbuf, checkdbuf, d);
	d->dpworking = ndp;
	dpmachno(d);
	for (i=0; i<ndp; i++) {
		sprint(up->genbuf, "dproc%d_%s", i, d->name);
		kproc(up->genbuf, dproc, d);
	}
}

static int
devrdopen(Rdev *d, Rawrd *r)
{
	int ret;
	char errbuf[ERRMAX]; // save errstr clobbered by rawrdopen on SAS

	strncpy(errbuf, up->errstr, ERRMAX - 1);
	errbuf[ERRMAX - 1] = '\0';
	strncpy(r->path, d->rdpath, sizeof r->path - 1);
	strncpy(r->name, d->name, sizeof r->name);

	ret = rawrdopen(r);
	if (ret >= 0 && strcmp(errbuf, up->errstr) != 0) {
		strncpy(up->errstr, errbuf, ERRMAX - 1);
		up->errstr[ERRMAX - 1] = '\0';
	}
	return ret;
}

static int
devflush(Rdev *d)
{
	int rv;
	Rawrd *r;

	r = mallocz(sizeof *r, 1);
	if (r == nil) {
		snprint(up->errstr, sizeof up->errstr, "malloc failure");
		return -1;
	}
	if (devrdopen(d, r) < 0) {
		free(r);
		return -1;
	}
	rv = rawrdflushcache(r);
	if (rv < 0)
		print("warning: flush cache of %s failed: %s\n", r->name, up->errstr);
	rawrdclose(r);
	free(r);
	return rv;
}

static void
getrpm(Rdev *d, int type, uchar *buf)
{
	ushort rr;

	rr = 0;
	if (type == Tata)
		rr = legets(&buf[217*2]);
	else if (type == Tscsi)
		rr = begets(&buf[4]);
	if (rr == 0x1 || (rr >= 0x0401 && rr < 0xffff))
		d->rpm = rr;
}

static void
rdevgeometry(Rdev *d)
{
	Drivegeometry g;
	Rawrd *xd;

	d->secsize = d->physecsize = 512;
	d->iomode = EMspin;
	d->rpm = 0;	// '0' rpm = not reported
	xd = mallocz(sizeof *xd, 1);
	if (xd == nil) {
		snprint(up->errstr, sizeof up->errstr, "malloc failure");
		return;
	}
	if (devrdopen(d, xd) < 0) {
		free(xd);
		return;
	}
	if (rawrdgeometry(xd, &g) != 0)
		goto done;
	d->secsize = g.secsize;
	d->physecsize = g.physecsize;
	d->rpm = g.rpm;
	if (d->rpm == 1)
		d->iomode = EMssd;
done:
	rawrdclose(xd);
	free(xd);
}

static void
makedev(Rdev *d, char *path)
{
	char *p;
	uchar buf[128];	/* cf. port/devfs.c: "old DIRLEN plus a little" */
	Chan *c;
	Dir dir;
	int n;
	static Ref ids;

	d->flags = 0;
	if (p = strchr(path, ':')) {
		*p++ = 0;
		if (*p == 'c')
			d->flags |= Dclean;
		else if (*p == 'r')
			d->flags |= Drepl;
		else if (*p == 'f')
			d->flags |= Dfailed;
	}
	strncpy(d->path, path, sizeof d->path);
	setdevslotattrs(d);
	d->length = 0;
	d->failfast = 0;
	d->savefailed = 0;
	d->c = nil;
	devcacheinit(d);
	if (!strcmp(path, "missing")) {
		d->flags |= Dmissing|Dfailed;
		return;
	}
	if (waserror()) {
		devcachefree(d);
		nexterror();
	}
	if (usepath(path) == 0)
		errorstr("%s already in use", d->name);
	if (waserror()) {
		unusepath(path);
		nexterror();
	}
	c = namec(path, Aopen, ORDWR, 0);
	if (c == nil)
		errorstr("unable to namec %s", path);
	n = devtab[c->type]->stat(c, buf, sizeof buf);
	convM2D(buf, n, &dir, nil);
	d->length = dir.length;
	d->c = c;
	rdevgeometry(d);
	poperror();
	poperror();
}

static Rdev *
dmirror(Rdev *d)
{
	int nd = d->raid->ndevs;
	Rdev *rd = d->raid->devs;

	return rd + ((d-rd) + nd/2) % nd;
}

static int
skipmirror(Rdev *d, uvlong bno)
{
	Rdev *rd;

	if (!nbskip)
		return 0;
	rd = d->raid->devs;
	return (d-rd) & 1 ^ bno & nbskip;
}

static void
raidsizes(Raid *r, uvlong *mindlen, uvlong *raidlen, uvlong *raiddoff)
{
	Lun *lb;
	Rdev *d, *e;
	uvlong doff, nstd, len, n;

	lb = r->lblade;
	doff = 0, n = 0, nstd = 0, len = 0;
	if (cansave(r)) {
		if (lb->ver == 1)
			doff = 128*512;
		else if (lb->ver == 0)
			doff = sizeof (Srconfig);
	}
	if (raiddoff)
		*raiddoff = doff;
	d = r->devs;
	e = d + r->ndevs;
	for (; d<e; d++) {
		n = (d->length - doff) / Nstride;
		if (n)
		if (nstd == 0 || n < nstd)
			nstd = n;
		len += n * Nstride;
	}
	n = nstd * Nstride;
	if (mindlen)
		*mindlen = n;
	if (raidlen == nil)
		return;
	switch (r->type) {
	case RTnil:
	case RTfnil:
		return;
	case RTraw:
	case RTjbod:
	case RTraidl:
		*raidlen = len;
		break;
	case RTraid0:
		*raidlen = n * r->ndevs;
		break;
	case RTraid5:
		*raidlen = n * (r->ndevs - 1);
		break;
	case RTraid6rs:
		*raidlen = n * (r->ndevs - 2);
		break;
	case RTraid1:
	case RTraid10:
		*raidlen = n * (r->ndevs / 2);
		break;
	default:
		dprint("raidsizes: unknown raid type %d\n", r->type);
	}
}

static struct {
	QLock;
	char patha[Npathasz];
} paths;

static void
unusepath(char *path)
{
	char *p, *r;

	qlock(&paths);
	p = strstr(paths.patha, path);
	if (p) {
		r = strchr(p, ':');
		if (r)
			strcpy(p, r+1);
		else
			*p ='\0';
	}
	qunlock(&paths);
}

static int
usepath(char *path)
{
	int n;

	qlock(&paths);
	if (strstr(paths.patha, path)) {
		qunlock(&paths);
		return 0;
	}
	n = strlen(path) + 1;
	if (strlen(paths.patha) + n >= Npathasz) {
		qunlock(&paths);
		error("out of path tracking space");
	}
	strcat(paths.patha, ":");
	strcat(paths.patha, path);
	qunlock(&paths);
	return 1;
}

static void
initparity(Lun *lb, Raid *r)		/* only for non-cached LUNs */
{
	if (lb->flags & Lcached0)
		return;
	if (!(r->flags & Rneedinit))
		return;

	assert(r->type == RTraid5 || r->type == RTraid6rs);

	r->roff = 0;
	if ((r->flags & (Rfailed|Rdegraded)) == 0) {
		r->flags |= Riniting;
		sprint(up->genbuf, "buildparity%d.%d", lb->slot, r->rno);
		kproc(up->genbuf, buildparity, r);
	}
}

static void
cacheparity(int spawn)		/* start parity on cached LUNs */
{
	Lun *l;
	Raid *r;
	Rdev *d, *e;
	int i;

	spinrlock(&lblades);
	for (l = lblades.head; l; l = l->next) {
		spinrlock(l);
		if (l->flags & Lcached0)
			for (i = 0, r = l->raids; i < l->nraids; ++i, ++r) {
				spinwlock(r);
				r->flags &= ~Riniting;
				d = r->devs;
				e = d + r->ndevs;
				for (; d < e && !(d->flags & (Drepl|Dreplacing)); ++d) ;
				if (d >= e)
					r->flags &= ~Rrecovering;
				setrflags(r);
				if ((r->type == RTraid5 || r->type == RTraid6rs)
						&& (r->flags & (Rneedinit|Rfailed|Rdegraded)) == Rneedinit) {
					r->flags |= Riniting;
					if (spawn) {
						sprint(up->genbuf, "buildparity%d.%d", l->slot, r->rno);
						kproc(up->genbuf, buildparity, r);
					}
				}
				spinwunlock(r);
			}
		spinrunlock(l);
	}
	spinrunlock(&lblades);
}

static void
cfgsanity(Raid *r)
{
	Rdev *d, *e;
	Srconfig *s;
	char *toks[8], *p;
	uvlong nrows, row;
	int n;

	d = r->devs;
	e = d + r->ndevs;
	for (; d<e && (!d || !d->c); d++) ;
	if (d == e)
		error("bad config-no dev");
	s = mallocz(sizeof (Srconfig), 1);
	if (!s)
		error("no memory");
	if (waserror()) {
		free(s);
		nexterror();
	}
	devtab[d->c->type]->read(d->c, s, sizeof (Srconfig), 0);
	n = tokenize(s->config, toks, 8);
	if (n < 7)
		error("bad config");
	p = strchr(toks[1], '.');
	if (!p)
		error("bad config-raid number");
	if (atoi(p+1) != r->rno)
		error("bad config");
	if (atoi(toks[2]) != r->ndevs)
		error("bad config-number of devices");
	nrows = r->mindlen / nbsize;
	row = strtoull(toks[5], nil, 0);
	r->roff = nrows - row;
	poperror();
	free(s);
}

static Raid *
addraid(Lun *lb, int rflag, int argc, char *argv[])
{
	Raid *r; 
	Rdev *d;
	int clean;

	if (lb->nraids >= Nraids)
		errorstr("too many raids for lun %d", lb->slot);
	r = lb->raids + lb->nraids;
	if (waserror()) {
		flushraid(r, 0);
		nexterror();
	}
	memset(r, 0, sizeof *r);
	r->lblade = lb;
	r->rno = lb->nraids;
	r->nrecovery = 0;
	r->type = raidtype(*argv);
	switch (r->type) {
	case RTnil:
	case RTfnil:
		r->length = (uvlong)8*1000*1000*1000;	/* 8 gig */
		setrflags(r);	/* r doesn't really exist until X, no bother wlocking */
		r->mindlen = 0;
		r->doff = 0;
		lb->nraids++;	/* X: r is accessible now */
		poperror();
		return r;
	}
	if (r->type == RTraidl && argc == 1)
		r->type = RTjbod;		/* silent "upgrade" */
	argc--, argv++;
	ndevtypesanity(argc, r->type);		/* argc == ndevs */
	magicsanity(r, argc, argv);
	r->devs = mallocz(sizeof (Rdev) * argc, 1);
	if (r->devs == nil)
		error("unable to alloc rdevs for raid");
	clean = 1;
	d = r->devs;
	for (; argc > 0; argc--, argv++, d++) {
		d->dno = r->ndevs++;
		d->raid = r;
		makedev(d, *argv);
		if ((d->flags & Dclean) == 0)
			clean = 0;
		if ((d->flags & Dmissing) == 0)
			devkpinit(d);
		dstatechg(d);
	}
	raidsizes(r, &r->mindlen, &r->length, &r->doff);
	if (clean) {
		r->flags |= Rclean;
		r->roff = r->mindlen / nbsize;
	} else if (r->type == RTraid5 || r->type == RTraid6rs)
		r->flags |= Rneedinit;
	if (rflag)
		cfgsanity(r);
	setrflags(r);	/* r doesn't really exist until X, no bother wlocking */
	if (isredundant(r))
	if (!(r->flags & Rfailed)) {
		r->flags |= Rscrubbing;
		sprint(up->genbuf, "R%d.%dscrub", lb->slot, r->rno);
		kproc(up->genbuf, scrub, r);
	}
	lb->nraids++;	/* X: r is accessible now */
	poperror();
	return r;
}

static void
wonkbufs(Rdev *d)
{
	Buf *bp, *e;
	int i;

	ilock(&d->elvlock);
	d->dlist.head = d->rlist.head = d->wlist.head = d->ahead = nil;
	d->reading = d->writing = d->closed = 0;
	iunlock(&d->elvlock);
	for (i = 0; i < Nhbufs; i++) {
		bp = d->bufs + i*Nhwidth;
		e = bp + Nhwidth;
		lock(&d->buflks[i]);
		for (; bp<e; bp++) {
			if (bp->d != d)
				continue;
			bp->flags = Bghost;
		}
		unlock(&d->buflks[i]);
	}
}

static void
iodraindev(Rdev *d, int on)
{
	int n, wlen;
	Chan *c;
	char buf[64], *cp, *cmd;

	buf[sizeof buf - 1] = 0;
	strncpy(buf, d->path, sizeof buf - 1);

	/* find slot # */
	cp = strrchr(buf, '/');
	if (cp == nil || cp == buf)
		return;
	*cp-- = 0;
	if (!isdigit(*cp))
		return;
	while (cp >= buf && isdigit(*cp))
		cp--;
	n = atoi(++cp);

	/* fetch from devrd.  Can't rely on namespace from the kernel */
	snprint(buf, sizeof buf, "#‡/%d/ctl", n);
	if (waserror()) {
		dprint("iodraindev: unable to open %s: %s\n", buf, up->errstr);
		return;
	}
	c = namec(buf, Aopen, OWRITE, 0);
	poperror();
	if (waserror()) {
		dprint("iodraindev: unable to write to chan for %s: %s\n", buf, up->errstr);
		cclose(c);
		return;
	}
	cmd = (on) ? "iodrain on" : "iodrain off";
	wlen = strlen(cmd);
	dprint("iodraindev: %s %s\n", buf, cmd);
	n = devtab[c->type]->write(c, cmd, wlen, 0);
	if (n != wlen)
		errorf("write returned %d", n);
	poperror();
	cclose(c);	
}

static void
wonkdev(Rdev *d, int zerocfg)
{
	if (d->c != nil) {
		elvclose(d);
		iodraindev(d, 1);
		while (d->ndp > 0) {	/* wait for kproc(s) to die */
			elvclose(d);	/* die, kproc(s), die! */
			yield();
		}
		iodraindev(d, 0);
		if (!waserror()) {
			if (zerocfg)
			if (d->raid)
				defunct(d);
			else	/* spare */
				devtab[d->c->type]->write(d->c, &zeroconfig, sizeof zeroconfig, 0);
			cclose(d->c);
			d->c = nil;
			poperror();
		}
		unusepath(d->path);
	}
	wonkbufs(d);
	devcachefree(d);
}

static void
flushraid(Raid *r, int zerocfg)
{
	Rdev *d, *e;

	switch (r->type) {
	case RTraw:
	case RTnil:
	case RTfnil:
		zerocfg = 0;
	}
	spinwlock(r);
	r->flags |= Rstop;
	spinwunlock(r);
	while ((r->flags & (Rscrubbing|Riniting)) || r->nrecovery > 0)
		tsleep(&up->sleep, return0, 0, 100);
	if (r->devs == nil)
		return;
	rsync(r);
	d = r->devs;
	e = d + r->ndevs;
	for (; d<e; d++) {
		d->flags = 0;
		dstatechg(d);
		wonkdev(d, zerocfg);
	}
	r->ndevs = 0;
	free(r->devs);
}

static void
grow(Lun *lb, int argc, char *argv[])
{
	Raid *r;

	spinwlock(lb);
	if (waserror()) {
		spinwunlock(lb);
		nexterror();
	}
	r = addraid(lb, 0, argc, argv);
	initparity(lb, r);
	lb->length += r->length;
	poperror();
	spinwunlock(lb);
	schedsave();
}

static int
islbfailed(Lun *lb)
{
	Raid *r, *e;

	spinrlock(lb);
	r = lb->raids;
	e = r + lb->nraids;
	for (; r<e; r++)
		if (r->flags & Rfailed)
			break;
	spinrunlock(lb);
	return r != e;
}

/*
 * Do what in userland would be:
 *  `{grep aoetarget /dev/sdS0/ctl | sed -e 's/aoetarget [0-9]*.\([0-9]\)$/\1/'}
 *
 * Ugly: assumes that our cache is necessarily a slurpee.
 */
static int
slrpid(void)
{
	char *buf, *p;
	int n;

	buf = malloc(8193);
	if (!buf)
		error("memory allocation failure");
	if (waserror()) {
		free(buf);
		return -1;
	}
	n = chancat("#S/sdS0/ctl", buf, 8192);
	if (n <= 0)
		nexterror();
	p = strstr(buf, "aoetarget ");
	n = -1;
	if (p) {
		p += 10;
		while(*p && isdigit(*p))
			++p;
		if (*p == '.')
			n = atoi(p+1);
	}
	poperror();
	free(buf);
	return n;
}

static int
isssdraid(Raid *r)
{
	Rdev *d, *e;

	d = r->devs;
	e = d + r->ndevs;
	for (; d<e; d++)
		if (d->iomode != EMssd)
			return 0;
	return 1;
}

static void
lbraid(int argc, char *argv[])
{
	Lun *lb, *l;
	Raid *r;
	int force, restore;
	int ver, slot, i, n;
	char *ep;
	char buf[64];

	ver = -1;
	force = 0;
	restore = 0;
	while (argc > 0) {
		if (strcmp(*argv, "-f") == 0)
			force = 1;
		else if (strcmp(*argv, "-r") == 0)
			restore = 1;
		else if (strcmp(*argv, "-V") == 0) {
			argc--, argv++;
			if (argc < 1)
				error(Eusage);
			ver = strtoul(*argv, &ep, 0);
			if (*ep != '\0')
				error(Eusage);
			if (ver < 0 || ver > 1)
				error(Eusage);
		} else
			break;
		argc--, argv++;
	}
	if (argc < 2)
		error(Eusage);
	slot = strtoul(*argv, &ep, 0);
	if (slot >= 255 || *ep != '\0')
		error("LUN must be an integer between 0 and 254 inclusive");
	spinwlock(&lblades);
/*1*/	if (waserror()) {
		spinwunlock(&lblades);
		nexterror();
	}
	for (l=lblades.head; l; l=l->next) {
		if (l->slot == slot)
			error("lun already exists");
	}
	if (slot == slrpid())
		error("lun already exists in CacheMotion");
	lb = mallocz(sizeof *lb, 1);
	if (!lb)
		error("lb allocation failure");
	lb->castats = mallocalign(sizeof (Castat) * NLcas, CACHELINESZ, 0, 0);
	if (lb->castats == nil) {
		free(lb);
		error("lb allocation failure");
	}
/*2*/	if (waserror()) {
		free(lb->castats);
		free(lb);
		nexterror();
	}
	switch (ver) {
	case 0:
		memmove(lb->magic, Srmagic00, Nmagic);
		lb->soff = sizeof (Srlconfig);
		lb->hasmagic = 1;
		lb->ver = 0;
		break;
	case 1:
		memmove(lb->magic, Srmagic01, Nmagic);
		lb->hasmagic = 1;
		lb->ver = 1;
		break;
	case -1:	/* default to latest */
		memmove(lb->magic, Srmagic01, Nmagic);
		lb->ver = 1;
	}
	lb->slot = slot;
	r = addraid(lb, restore, argc-1, argv+1);
/*3*/	if (waserror()) {
		flushraid(r, 0);
		nexterror();
	}
	if (r->ndevs==0  && 200+lbwactive+dprocactive+lbwperdisk > conf.nproc)
		error("too few processes available for new nil lun");
	lb->length = r->length;
	n = r->ndevs * bufcntperdisk;
	/*
	 * Normally bufcntperdisk is nailed to lbwperdisk in order to avoid
	 * out of order problems in the pipeline processing from netreader
	 * to lbworkers to dprocs.  We have an i/o elevator with delay to
	 * accomodate some out of order, but if all the lbworkers block
	 * waiting on their submitted i/o and can't pull the additional requests
	 * from the lun queue (which is the case when bufcnt >> nlbworkers)
	 * then we undo our sorting and sequential i/o on spinning disks with
	 * multiple ingest interfaces performs lower due to looking random
	 * at the drive.
	 *
	 * If we're all SSD, though, we don't care about this problem and the
	 * higher bufcnt results in increased performance.
	 */
	if (isssdraid(r))
		n *= 2;
	if (n > bufcntmax || r->type == RTnil || r->type == RTfnil)
		n = bufcntmax;
	lb->bufcnt = n;
	/*
	 * set queue depth to bufcnt*8 to support multiple initiators
	 * concurrently before we congest.
	 */
	lb->iolim = n*8;
	if (restore)
		loadlun(lb, force);
	initparity(lb, r);
	if (!restore) {
		resetlun(lb);
		switch (r->type) {
		case RTnil:
		case RTfnil:
			break;
		case RTraw:
			break;
		default:
			if (ecpresent)
				setcache(lb, 0, 1);
			if (ecattached[1])
				setcache(lb, 1, 1);
		}
	}
	if ((lb->flags & Lcached1) && (lb->fcpri || lb->fcminpct))
		ecpriority(1, lb->slot, lb->fcpri, lb->fcminpct);
	resetmodelfw(lb);
	setlbgeometry(lb);
	krloadrr(lb);
	lb->fsunit = incref(&units);
	addfilter(&lb->rfilt);
	addfilter(&lb->wfilt);
	if (r->ndevs == 0)	/* [f]nil */
		n = lbwperdisk;
	else {
		n = r->ndevs * lbwperdisk;
		if (n > lbwmax)
			n = lbwmax;
	}
	/* one ref per lbworker; one for lblades list */
	lb->ref = n + 1;
	lb->nworker = n;
	lb->next = lblades.head;
	lblades.head = lb;
	for (i = 0; i < n; i++) {
		snprint(buf, sizeof buf, "lbworker%d_%d", lb->slot, i);
		kproc(buf, lbworker, lb);
	}
	if (restore == 0)
		jskevent( "msg:s:'LUN %d Type %s Created'"
			" sev:n:5 tag:s:SRX_LUN_CREATED shelf:n:%d lun:n:%d type:s:%s",
			lb->slot, rt2s(r->type), shelf, lb->slot, rt2s(r->type));	
	spinwunlock(&lblades);
/*3*/	poperror();
/*2*/	poperror();
/*1*/	poperror();
	schedsave();
}

static void
bcastqc(Lun *lb)
{
	Net *np;
	Block *b, *bp;
	Aoeqc *qh;
	int i, len;

	len = sizeof *qh + Nconflen;
	bp = allocb(len);
	if (bp == nil)
		error("allocb failure");
	if (waserror()) {
		freeb(bp);
		nexterror();
	}
	memset(bp->wp, 0, len);
	qh = (Aoeqc *) bp->wp;
	len = lb->nconfig;
	memmove(qh+1, lb->config, len);
	memset(qh->dst, 0xff, 6);
	hnputs(qh->type, ETAOE);
	qh->verflags = AFrsp | (Aoever<<4);
	hnputs(qh->major, shelf);
	qh->minor = lb->slot;
	qh->cmd = ACconfig;
	hnputl(qh->tag, 0);
	if (nbufcnt)
		hnputs(qh->bufcnt, nbufcnt);
	else
		hnputs(qh->bufcnt, lb->bufcnt);
	hnputs(qh->fwver, FWV);
	qh->verccmd = Aoever << 4;
	hnputs(qh->cslen, len);
	len += sizeof *qh;
	if (len < ETHERMINTU)
		len = ETHERMINTU;
	bp->wp += len;
	/* We will send 802.1Q frames on ALL ports if lb has a vlan */
	bp->vlan = lb->vlan;
	rlock(&nets);
	if (waserror()) {
		runlock(&nets);
		nexterror();
	}
	for (np = nets.head; np; np = np->next) {
		qh->scnt = np->maxscnt;
		memmove(qh->src, np->ea, 6);
		if (lb->nmacs == 0) {
			b = copyblock(bp, BLEN(bp));
			if (b == nil)
				error("copyblock failure");
			if (replacevlan(b) == -1) {
				freeb(b);
				error("bcastvlan error");
			}
			devtab[np->dc->type]->bwrite(np->dc, b, 0);
			continue;
		}
		for (i=0; i<lb->nmacs; i++) {
			memmove(qh->dst, &lb->macs[i*6], 6);
			b = copyblock(bp, BLEN(bp));
			if (!b)
				error("copyblock failure");
			if (replacevlan(b) == -1) {
				freeb(b);
				error("bcastvlan error");
			}
			devtab[np->dc->type]->bwrite(np->dc, b, 0);
		}
	}
	poperror();
	runlock(&nets);
	poperror();
	freeb(bp);
}

static int
cacheavail(Lun *lb)
{
	uchar *p;

	if (!(lb->flags & Lcached0))
		return 1;
	if (!ecattached[0] || !cachelist)
		return 0;
	for (p = cachelist; p < cachelist + ecblk - 1; p += Nserialsz) {
		if (memcmp(p, lb->serial, Nserialsz) == 0)
			return 1;
	}
	return 0;
}

static int
sigvalid(void)
{
	Chan *c;

	c = namec("#§/sig/flash", Aaccess, 0, 0);
	if (c == nil)
		return 0;
	cclose(c);
	return 1;
}

/*
 * This is only called by CMonline CMoffline.  To use this in another context,
 * we might need to rethink the lbfailed error message.
 */ 
static void
setline(Lun *lb, Lstate s)
{
	switch (s) {
	case Lon:
		if (lb->line == Lon)
			break;
		if (lb->raids[0].type != RTraw)	/* permit traffic to update lblades */
		if (sigvalid() == 0) {
			print("Flash module is unsigned.  AoE traffic flow prohibited for LUN %d.\n", lb->slot);
			return;
		}
		if (!cacheavail(lb))
			errorstr("cannot online LUN %d without cache", lb->slot);
		lb->line = Lon;
		bcastqc(lb);
		jskevent("msg:s:'LUN %d.%d online' sev:n:5 tag:s:SRX_LUN_ONLINE shelf:n:%d lun:n:%d",
			shelf, lb->slot, shelf, lb->slot);
		break;
	case Loff:
		if (lb->line == Loff)
			break;
		lb->line = Loff;
		jskevent("msg:s:'LUN %d.%d offline' sev:n:5 tag:s:SRX_LUN_OFFLINE shelf:n:%d lun:n:%d",
			shelf, lb->slot, shelf, lb->slot);
		break;
	}
}

static void
resetlun(Lun *lb)
{
	int n;
	Raid *r;

	r = &lb->raids[0];
	lb->nconfig = 0;
	setlunlabel(lb, "", 0); 
	resetserial(lb);
	lb->vlan = 0;
	lb->flags = 0;
	/*
	 * If we're creating an SSD RAID LUN always select random
	 * iomode if the RAID can sustain it.
	 */
	if (isssdraid(r))
	if (candirect(r))
		goto r;
	switch (r->type) {
	case RTraid1:
	case RTraid10:
r:		setnobuf0(lb, 1);
		break;
	}
	if (!cansavelun(lb))
		return;
	n = 0;
	if (lb->ver == 1)
		n = drw(lb, &zeroconfig, sizeof zeroconfig, 64*512, OWRITE);
	else if (lb->ver == 0)
		n = lrw(lb, &zeroconfig, sizeof zeroconfig, 0, OWRITE, Bimm);
	if (n < sizeof zeroconfig)
		print("resetlun: short write to lun %d\n", lb->slot);
}

static long
fnv1al(char *s)
{
	long h;

	h = 2166136261;		/* offset_basis */
	for (; *s; s++) {
		h ^= *s;
		h *= 16777619;	/* FNV_prime */
	}
	return h;
}

static char *
getcookie(void)
{
	int n;
	char buf[256], *p, *d, *e;

	n = chancat("#§/stat", buf, sizeof buf - 1);
	if (n < 0)
		return nil;
	buf[n] = 0;
	for (e = buf; e;) {
		p = e;
		if (e = strchr(p, '\n'))	/* line at a time */
			*e++ = 0;
		d = strchr(p, ':');		/* delimiter */
		if (d == nil)
			continue;
		*d++ = 0;
		if (strcmp(p, "cookie") == 0) {
			while (isspace(*d))	/* skip potential readability ws */
				d++;
			return strdup(d);
		}
	}
	return nil;
}

static void
resetserial(Lun *lb)
{
	char *p;

	p = getcookie();
	if (p == nil) {
		p = smprint("8675309");
		if (p == nil)
			error("memory failure");
	}
	snprint(lb->serial, sizeof lb->serial, "%08ulX-%02uX-%08ulX",
	    fnv1al(p), lb->slot, rtctime());
	free(p);
}

static int
diskmodelfw(Lun *lb)
{
	char buf[1024], *p, *q, *r, *m, *f;
	int n;

	snprint(buf, sizeof buf, "%s", lb->raids[0].devs[0].path);
	p = strstr(buf, "/data");
	if (p == nil)
		return -1;
	snprint(p, &buf[sizeof buf] - p, "%s", "/stat");
	n = creadonce(buf, buf, sizeof buf);
	if (n < 0)
		return -1;
	p = r = buf;
	m = f = nil;
	for (; r; p = r + 1) {
		r = strchr(p, '\n');
		if (r)
			*r = 0;
		q = strchr(p, ':');
		if (q == nil)
			continue;
		q++;
		while (isspace(*q))
			q++;
		if (*q == 0)
			continue;
		if (cistrncmp(p, "model", 5) == 0)
			m = q;
		else if (cistrncmp(p, "fw", 2) == 0)
			f = q;
	}
	if (m == nil || f == nil)
		return -1;
	snprint(lb->model, sizeof lb->model, "%s", m);
	snprint(lb->fwver, sizeof lb->fwver, "%s", f);
	return 0;
}

static void
getprod(char *p, char *e)
{
	int n;

	p = seprint(p, e, "EtherDrive ");
	
	n = creadonce("#c/model", p, e-p-1);
	if (n < 0)
		return;
	p[n] = '\0';
	rmnl(p);
	/* neuter model down to product only */
	for (; p<e; p++) {
		if (!isalpha(*p)) {
			*p = '\0';
			break;
		}
	}
}

static void
idputgeometry(Lun *lb, ushort *buf)
{
	uchar *id;
	ushort phyword, tmp, exp;

	id = (uchar *)(buf+117);
	leputl(id, lb->secsize>>1); // secsize in Nwords

	phyword = 0x4000;
	exp = 0;
	if ((lb->secsize>>1) > 256)
		phyword |= 1 << 12;
	if ((lb->physecsize > lb->secsize) && (lb->physecsize % lb->secsize == 0)) {
		phyword |= 1 << 13;
		tmp = lb->physecsize/lb->secsize;
		while (tmp >>= 1)
			exp++;
		phyword |= exp;		// physecsize = 2^exp*secsize
	}
	id = (uchar *)(buf+106);
	leputs(id, phyword);
	id = (uchar *)(buf+217);
	leputs(id, lb->rpm);
}

static void
setlbgeometry(Lun *lb)
{
	Raid *r, *re;
	Rdev *d, *de;
	ulong maxsecsize;
	ushort maxphysecsize;

	spinrlock(lb);
	lb->rpm = 0;
	maxsecsize = maxphysecsize = (lb->emuflags & EE4kn) ? 4096 : 512;
	r = lb->raids;
	re = r + lb->nraids;
	for (; r<re; r++) {
		spinrlock(r);
		d = r->devs;
		if (r->type == RTjbod)
			lb->rpm = d->rpm;
		de = d + r->ndevs;
		for (; d<de; d++) {
			if (d->secsize > maxsecsize)
				maxsecsize = d->secsize;
			if (d->physecsize > maxphysecsize)
				maxphysecsize = d->physecsize;
		}
		spinrunlock(r);
	}
	spinrunlock(lb);

	lb->secsize = maxsecsize;
	lb->physecsize = maxphysecsize;
}

static void
srmodelfw(Lun *lb)
{
	char buf[40+1];

	memset(buf, 0, sizeof buf);
	if (cistrstr(lb->raids[0].devs[0].path, "update"))
		strcpy(buf, "Update");
	else
		getprod(buf, buf+sizeof buf-1);
	snprint(lb->model, sizeof lb->model, "Coraid %s", buf);
	snprint(lb->fwver, sizeof lb->fwver, "V%d.%d\n", FWV >> 24, FWV & 0xffffff);
}

static void
resetmodelfw(Lun *lb)
{
	switch (lb->raids[0].type) {
	case RTnil:
	case RTfnil:
		return;
	case RTjbod:
	case RTraw:
		if (diskmodelfw(lb) != -1)
			return;
		/* fall thru */
	default:
		srmodelfw(lb);
	}
}

static int
addmac(Lun *lb, uchar *ea)
{
	uchar *p, *e;

	spinwlock(lb);
	p = lb->macs;
	e = p + lb->nmacs*6;
	for (; p<e; p += 6) {
		if (!memcmp(p, ea, 6)) {
			spinwunlock(lb);
			return 2;
		}
	}
	if (lb->nmacs >= Nmacs) {
		spinwunlock(lb);
		return 0;
	}
	memmove(p, ea, 6);
	lb->nmacs++;
	spinwunlock(lb);
	return 1;
}

static int
validate(char *mac)
{
	int i;
	uchar byte0;

	for (i=0; i < 2*Eaddrlen; i++)
		if(!isxdigit(mac[i]))
			return -1;
	dec16(&byte0, 1, mac, 2);
	if (byte0 & 1) /* Check for multicast MAC */
		return -1;
	return 0;	
}

static void
mask(Lun *lb, char *addr)
{
	uchar ea[6];
	int res;

	if (validate(addr) < 0)
		error(Ebadmac);
	if (parseether(ea, addr) < 0)
		error(Ebadmac);
	res = addmac(lb, ea);
	if (res == 0)
		error("mask list full");
	jskevent("msg:s:'LUN %d Add Mask %s' sev:n:5 tag:s:SRX_LUN_ADDMASK shelf:n:%d lun:n:%d mask:s:%s",
		lb->slot, addr, shelf, lb->slot, addr);
	if (res == 1)
		schedsave();
}

static int
rmmac(Lun *lb, uchar *ea)
{
	uchar *p, *e;

	spinwlock(lb);
	p = lb->macs;
	e = p + lb->nmacs*6;
	for (; p<e; p+=6)
		if (!memcmp(p, ea, 6)) {
			memmove(p, p+6, e-p-6);
			lb->nmacs--;
			spinwunlock(lb);
			jskevent("msg:s:'LUN %d Del Mask %E' sev:n:5"
				" tag:s:SRX_LUN_DELMASK shelf:n:%d lun:n:%d mask:s:%E",
				lb->slot, ea, shelf, lb->slot, ea);
			return 1;
		}
	spinwunlock(lb);
	return 0;
}

static void
rmmask(Lun *lb, char *addr)
{
	uchar ea[6];

	if (validate(addr) < 0)
		error(Ebadmac);
	if (parseether(ea, addr) < 0)
		error(Ebadmac);
	if (rmmac(lb, ea))
		schedsave();
}

static void
addcache(char *disk, char *op)
{
	int offset = 1 << 16;
	uvlong len;
	Rdev *d, *e;
	int pct;
	char *ep;

	pct = strtol(op, &ep, 10);
	if (*ep != 0 || pct < 0 || pct > 100)
		error("over-provisioning percentage must be in the range 0 to 100");
	ckpath(disk);
	fcache0(disk);
	d = caches.devs;
	e = d + caches.ndevs;
	for (; d < e; ++d) {
		if (strcmp(disk, d->path) == 0)
			break;
	}
	len = (d->length * (100 - pct))/100;
	if (waserror()) {
		rmfcache0(disk);
		nexterror();
	}
	formatcachex(1, disk, 0, offset, len);
	poperror();
	jskevent("msg:s:'Drive %s added to EtherFlash Cache'"
		" sev:n:5 tag:s:SRX_FC_ADD shelf:n:%d slot:n:%d", 
		d->name, shelf, d->slot);
	schedsave();
}

static void
attachcache(char *level, char *disk, char *offset)
{
	Chan *rdc;
	Lun *l;
	uchar *p, *e;
	char *er;
	int lev, dobuf;
	uchar zeros[Nserialsz];

	if (!cachelist)
		cachelist = mallocz(8192, 1);
	if (!cachelist)
		error("memory failure in cache");
	lev = strtoul(level, &er, 10);
	if (level == nil || lev >= Ncache || *er != '\0')
		error("invalid cache level");
	if (lev == 0) {
		ecpresent = 1;
		ecblk = ecattach(lev, disk, strtoll(offset, nil, 10), 0, nil, nil);
	}
	else {
		/*
		 * Hack of extreme ugliness
		 */
		dobuf = 0;
		if (!ecattached[lev]) {
			rdc = namec("#ℝ/ctl", Aopen, OWRITE, 0);
			if (rdc) {
				if (waserror()) {
					cclose(rdc);
					nexterror();
				}
				devtab[rdc->type]->write(rdc, "size 67108864", 13, 0);
				if (ecream(lev, "#ℝ/data", 131072, 0, 67108864, 1, EFrdbuf | EFauto | EFbypass | EFquick) >= 0) {
					dobuf = 1;
					lev++;
				}
				poperror();
				cclose(rdc);
			}
		}
		else
			++lev;
		if (ecattach(lev, disk, strtoll(offset, nil, 10), 1,nil, fcbackio) < 0)
			error("cache attach failed");
		ecattached[lev] = 1;
		if (dobuf) {
			if (ecattach(lev-1, "#ℝ/data", 0, 0, nil, nil) < 0)
				error("cache attach failed");
			ecattached[lev-1] = 1;
		}
		return;
	}

	if (ecblk >= 0
			&& ecreadl(lev, 0, 0, cachelist, ecblk, ETdirect) == ecblk) {
		spinrlock(&lblades);
		e = cachelist + ecblk - 1 - Nserialsz;
		for (l = lblades.head; l; l = l->next) {
			if (l->flags & Lcached0) {
				for (p = cachelist; p < e && memcmp(p, l->serial, Nserialsz); p += Nserialsz) ;
				if (p >= e)
					break;
			}
		}
		if (l) {
			spinrunlock(&lblades);
			error("cache mismatch");
		}
		memset(zeros, 0, Nserialsz);
		for (p = cachelist; p < e; p += Nserialsz) {
			if (memcmp(p, zeros, Nserialsz) == 0)
				continue;
			for (l = lblades.head; l && memcmp(p, l->serial, Nserialsz); l = l->next) ;
			if (!l || !(l->flags & Lcached0))
				break;
		}
		spinrunlock(&lblades);
		if (p < e) {
			error("cache mismatch");
		}
		ecattached[lev] = 1;
	}
	else
		error(up->errstr);
}

static void
cacheflags(int lev, int f)
{
	if (lev >= Ncache)
		error("error: invalid cache level");
	if (!cachelist)
		cachelist = mallocz(8192, 1);
	cachelist[ecblk-1] = f;
	if (ecwrite(0, 0, cachelist, ecblk, ETdirect) != ecblk) {
		ecclose(0);
		ecattached[0] = 0;
		error("error: cache flag update failed");
	}
}

static void
cacheprio(Lun *lb, int lev, char *priority, char *minpct)
{
	int pri, minws;
	char *e;

	pri = strtol(priority, &e, 10);
	if (*e != 0 || pri < 0 || pri > 100)
		error("pri must be in the range 0 to 100");
	minws = strtol(minpct, &e, 10);
	if (*e != 0 || minws < 0 || minws > 100)
		error("minpct must be in the range 0 to 100");
	lb->fcpri = pri;
	lb->fcminpct = minws;
	schedsave();
	ecpriority(lev, lb->slot, pri, minws);
}

static void
formatcachex(int level, char *disk, int blksize, vlong offset, vlong length)
{
	Chan *rdc;
	Lun *l;
	uchar *p;
	uvlong len;
	int bsize, dobuf, i;

	if (!cachelist)
		cachelist = mallocz(8192, 1);
	if (!cachelist)
		error("memory failure in cache");
	if (level >= Ncache)
		error("invalid cache level");
	if (level == 0) {
		/* For now we only support one device at this level, so close in case it's open */
		ecclose(level);
		ecblk = blksize;
		ecattached[0] = 0;
		if (ecream(0, disk, ecblk, offset, length, 1, EFaside | EFauto) < 0) {
			error("ream failed");
		}
		if (ecattach(level, disk, offset, 0, nil, nil) < 0) {
			error("attach failed");
		}
		ecpresent = 1;
		memset(cachelist, 0, ecblk);
		p = cachelist;
		spinrlock(&lblades);
		for (l = lblades.head; l; l = l->next) {
			if (l->flags & Lcached0) {
				memmove(p, l->serial, Nserialsz);
				p += Nserialsz;
			}
		}
		spinrunlock(&lblades);
		if (ecwritel(level, 0, 0, cachelist, ecblk, ETdirect) != ecblk) {
			ecattached[level] = 0;
			ecclose(level);
			error("ream cachelist failed");
		}
		ecattached[level] = 1;
	}
	else {
		/*
		 * Hack of extreme ugliness
		 */
		dobuf = 0;
		if (!ecattached[level]) {
			rdc = namec("#ℝ/ctl", Aopen, OWRITE, 0);
			if (rdc) {
				if (waserror()) {
					cclose(rdc);
					nexterror();
				}
				devtab[rdc->type]->write(rdc, "size 67108864", 13, 0);
				if (ecream(level, "#ℝ/data", 131072, 0, 67108864, 1,
						EFrdbuf | EFauto | EFbypass | EFquick) >= 0) {
					dobuf = 1;
					level++;
				}
				poperror();
				cclose(rdc);
			}
		}
		else
			++level;
		len = length / Ncblocks;
		for (i = 0; i < 64 && len != 0; ++i, len >>= 1) ;
		if (i < 17)
			i = 17;
		bsize = (uvlong)1 << i;
		if (bsize <= 0)
			error("unusable cache size");
		if (ecream(level, disk, bsize, offset, length, 0, EFrdbuf | EFbypass) < 0)
			error("ream failed");
		if (ecattach(level, disk, offset, 0, nil, fcbackio) < 0)
			error("attach failed");
		ecattached[level] = 1;
		if (dobuf) {
			if (ecattach(level-1, "#ℝ/data", 0, 0, nil, nil) < 0)
				error("attach failed");
			ecattached[level-1] = 1;
		}
	}
}

static void
formatcache(char *level, char *disk, char *blksize, char *offset, char *length)
{
	formatcachex(atoi(level), disk, atoi(blksize), strtoll(offset, nil, 10),
		strtoull(length, nil, 10));
}

static void
setcache(Lun *lb, int lev, int cached)
{
	Chan *ch;
	uchar *p;
	uchar zeros[Nserialsz];

	if (lev >= Ncache)
		error("invalid cache level");
	if (lev == 0) {
		if (cached && !ecpresent)
			error("cannot enable caching with no cache present");
		if (ecattached[lev] && cachelist) {
			if (cached) {
				for (p = cachelist; p < cachelist + ecblk - 1; p += Nserialsz) {
					if (memcmp(p, lb->serial, Nserialsz) == 0)
						goto inlist;
				}
				memset(zeros, 0, Nserialsz);
				for (p = cachelist; p < cachelist + ecblk - 1; p += Nserialsz) {
					if (memcmp(p, zeros, Nserialsz) == 0)
						break;
				}
				if (p >= cachelist + ecblk - 1)
					error("no more LUNs can be cached");
				memmove(p, lb->serial, Nserialsz);
			}
			else {
				for (p = cachelist; p < cachelist + ecblk - 1; p += Nserialsz) {
					if (memcmp(p, lb->serial, Nserialsz) == 0) {
						memset(p, 0, Nserialsz);
						break;
					}
				}
			}
			if (ecwrite(0, 0, cachelist, ecblk, ETdirect) != ecblk) {
				if (ecattached[0]) {
					ecattached[0] = 0;
					print("cache failure: switching to synchronous operation\n");
					ecpoison(0);
					ecclose(0);
					syncio = Bimm;
					if (waserror()) {
						print("failed to set wrcache off\n");
						nexterror();
					}
					ch = namec("#‡/ctl", Aopen, OWRITE, 0);
					if (waserror()) {
						cclose(ch);
						nexterror();
					}
					devtab[ch->type]->write(ch, "wrcache off", 11, 0);
					cclose(ch);
					poperror();
					poperror();
					sync();
					error("failed to write updated cache list");
				}
			}
		}
		else
			print("Changing cache status of LUN with no cache present\n");
inlist:
		if (cached) {
			if ((lb->flags & Lcached0) == 0)
				jskevent( "msg:s:'LUN %d CacheMotion Enabled'"
					" sev:n:5 tag:s:SRX_CM_LUN_ENABLE shelf:n:%d lun:n:%d",
					lb->slot, shelf, lb->slot);
			lb->flags |= Lcached0;
		} else {
			if ((lb->flags & Lcached0) == Lcached0)
				jskevent( "msg:s:'LUN %d CacheMotion Disabled'"
					" sev:n:5 tag:s:SRX_CM_LUN_DISABLE shelf:n:%d lun:n:%d",
					lb->slot, shelf, lb->slot);
			lb->flags &= ~Lcached0;
		}
	}
	else {
		if (cached) {
			lb->flags |= Lcached1;
			if (lb->fcpri || lb->fcminpct)
				ecpriority(1, lb->slot, lb->fcpri, lb->fcminpct);
			jskevent("msg:s:'LUN %d EtherFlash Cache enabled'"
				" sev:n:5 tag:s:SRX_FC_LUN_ENABLE shelf:n:%d lun:n:%d", 
				lb->slot, shelf, lb->slot);
		}
		else {
			lb->flags &= ~Lcached1;
			jskevent("msg:s:'LUN %d EtherFlash Cache disabled'"
				" sev:n:5 tag:s:SRX_FC_LUN_DISABLE shelf:n:%d lun:n:%d", 
				lb->slot, shelf, lb->slot);
		}
	}
	schedsave();
	if (!cached) {
/*
		I may still want to do it this way with full r/w caches.  But for
		a write-aside level 0 and a read buffer level 1, we only need
		to invalidate the one we're disabling.

		if (ecattached[0])
			ecinval(0, lb->slot);
		if (ecattached[1])
			ecinval(1, lb->slot);
*/
		if (ecattached[lev]) {
			ecinval(lev, lb->slot);
			if (lev == 1)
				ecinval(2, lb->slot);
		}
	}
	else {
		if (ecattached[lev]) {
			ecreclaim(lev, lb->slot);
			if (lev == 1)
				ecreclaim(2, lb->slot);
		}
	}
}
static void
restorecache(char *disk)
{
	Rdev *d, *e;

	ckpath(disk);
	d = caches.devs;
	e = d + caches.ndevs;
	for (; d < e; d++)
		if (strcmp(disk, d->path) == 0)
			return;
	fcache0(disk);
	if (waserror()) {
		print("Failed to restore cache %s: %s\n", d->name, up->errstr);
		rmfcache0(disk);
		nexterror();
	}
	attachcache("1", disk, "65536");
	poperror();
	schedsave();
}

static void
rmfcachedev(Rdev *d)
{
	Lun *l;

	if (ecclosedev(2, d->path)) {
		ecattached[1] = 0;
		ecattached[2] = 0;
		spinrlock(&lblades);
		for (l = lblades.head; l; l = l->next)
			l->flags &= ~Lcached1;
		spinrunlock(&lblades);
		ecclose(1);
		ecclose(2);
	}
	d->flags &= ~Dcache;
	dstatechg(d);
	wonkdev(d, 1);
}

static void
rmfcache0(char *path)
{
	Rdev *d, *e;

	qlock(&caches);
	if (waserror()) {
		qunlock(&caches);
		nexterror();
	}
	d = caches.devs;
	e = d + caches.ndevs;
	for (; d < e; ++d) {
		if (strcmp(path, d->path) == 0) {
			rmfcachedev(d);
			if (--caches.ndevs)
				*d = *(e-1);
			break;
		}
	}
	poperror();
	qunlock(&caches);
}

static void
rmfcache(int argc, char *argv[])
{
	Rdev *d, *e;
	int n;

	if (argc > 1) {
		error("usage: rmfcache [device]");
		return;
	}
	n = argc ? ckpath(argv[0]) : 0;
	qlock(&caches);
	if (waserror()) {
		qunlock(&caches);
		nexterror();
	}
	d = caches.devs;
	e = d + caches.ndevs;
	for (; d < e;) {
		if (argc && strcmp(argv[0], d->path)) {
			d++;
			continue;
		}
		rmfcachedev(d);
		jskevent("msg:s:'Drive %s removed from EtherFlash Cache'"
			" sev:n:5 tag:s:SRX_FC_REMOVE shelf:n:%d slot:n:%d", 
			d->name, shelf, d->slot);
		if (--caches.ndevs)
			*d = *(e-1);
		if (argc)
			break;
		e = caches.devs + caches.ndevs;
	}
	if (argc && d == e)
		errorstr("Drive %d.%d is not part of EtherFlash Cache", shelf, n);
	poperror();
	qunlock(&caches);
	schedsave();
}

static void
fcache0(char *path)
{
	Rdev *d;

	qlock(&caches);
	if (waserror()) {
		qunlock(&caches);
		nexterror();
	}
	if (caches.ndevs >= Ncaches)
		error("too many caches");
	d = caches.devs + caches.ndevs;
	makedev(d, path);
	if (d->flags & (Dfailed|Dmissing)) {
		wonkdev(d, 0);
		error("cache devices cannot be failed or missing");
	}
	d->flags |= Dcache;
	devcachefree(d);
	dstatechg(d);
	d->raid = nil;
	caches.ndevs++;
	poperror();
	qunlock(&caches);
}

static void
fcache(char *path)
{
	fcache0(path);
	schedsave();
}

static long
showfcache(void *db, long len)
{
	char *buf, *cp, *ep;
	Rdev *d, *e;

	cp = buf = smalloc(READSTR);
	*cp = 0;
	ep = buf + READSTR;
	if (waserror()) {
		free(buf);
		nexterror();
	}
	qlock(&caches);
	if (waserror()) {
		qunlock(&caches);
		nexterror();
	}
	d = caches.devs;
	e = d + caches.ndevs;
	for (; d<e; d++)
		cp = seprint(cp, ep, "%s %lld\n", d->path, d->length);
	poperror();
	qunlock(&caches);
	len = readstr(0, db, len, buf);
	poperror();
	free(buf);
	return len;
}

static void
parserspec(char *rspec, Lun *lb, Raid **rr, Rdev **rd)
{
	char *buf, *p;
	ulong rno, dno;
	Raid *r;

	buf = strdup(rspec);
	if (buf == nil)
		errorstr("parserspec: can't allocate for rspec %s", rspec);
	if (waserror()) {
		free(buf);
		snprint(up->errstr, ERRMAX, "invalid raid.dev specification %s", rspec);
		nexterror();
	}
	p = strchr(buf, '.');
	if (!p)
		nexterror();
	*p++ = '\0';
	rno = strtoul(buf, 0, 0);
	dno = strtoul(p, 0, 0);
	r = lb->raids + rno;
	if (rno >= lb->nraids || dno >= r->ndevs)
		nexterror();
	if (rr)
		*rr = r;
	if (rd)
		*rd = r->devs + dno;
	poperror();
	free(buf);
}

static void
failguard(Raid *r, Rdev *rd)
{
	Lun *lb;
	Rdev *d, *e;
	int bad;
	
	lb = r->lblade;
	if (lb->flags & Lnoguard)
		return;
	if (!isredundant(r))
		errorstr("raid %d.%d is not redundant and would not sustain failure", lb->slot, r->rno);
	if (r->flags & Rfailed)
		errorstr("raid %d.%d is failed", lb->slot, r->rno);
	if (r->flags & (Riniting|Rneedinit))
		errorstr("raid %d.%d has uninitialized parity", lb->slot, r->rno);
	if (rd->flags & Dfailed)
		errorstr("raid device %d.%d.%d is already failed", lb->slot, r->rno, rd->dno);
	spinrlock(r);
	if (waserror()) {
		spinrunlock(r);
		nexterror();
	}
	switch (r->type) {
	case RTraid10:
	case RTraid1:
		if (DEVBAD(dmirror(rd)))
rfail:			errorstr("failing %d.%d.%d would fail raid", lb->slot, r->rno, rd->dno);
		break;
	case RTraid5:
	case RTraid6rs:
		bad = 0;
		d = r->devs;
		e = d + r->ndevs;
		for (; d<e; d++) {
			if (d != rd)
			if (DEVBAD(d))
				bad++;
		}
		if (bad--)
		if (r->type == RTraid5 || bad)
			goto rfail;
		break;
	}
	poperror();
	spinrunlock(r);
}

static void
faildev(Rdev *d)
{
	Lun *lb;
	Raid *r;

	r = d->raid;
	lb = r->lblade;
	if (d->flags & Dfailed)	/* device is already failed */
		return;
	d->flags |= Dfailed;
	iodraindev(d, 0);
	coherence();
	d->failfast = 0;
	dstatechg(d);
	wonkbufs(d);
	jskevent("msg:s:'drive %s (device %d.%d.%d) has failed'"
		 " sev:n:1 tag:s:SRX_DRIVE_FAILURE %S"
		 " lun:n:%d raidpart:n:%d drivepart:n:%d", d->name,
		 lb->slot, r->rno, d->dno, d, lb->slot, r->rno, d->dno);
}

static void
tryfail(Rdev *d)
{
	Raid *r;

	r = d->raid;
	failguard(r, d);
	spinwlock(r);
	if (waserror()) {
		spinwunlock(r);
		nexterror();
	}
	faildev(d);
	setrflags(r);
	poperror();
	spinwunlock(r);
}

static void
fail(Lun *lb, char *rspec)
{
	Rdev *d;
	Raid *r;

	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	parserspec(rspec, lb, &r, &d);
	tryfail(d);
  	poperror();
  	spinrunlock(lb);
  	schedsave();
}

static void
failpath(char *path)
{
	Lun *lb;
	Raid *r, *re;
	Rdev *d, *de, *dp;

	dp = nil;
	spinrlock(&lblades);
	if (waserror()) {
		spinrunlock(&lblades);
		nexterror();
	}
	lb = lblades.head;
	for (; !dp && lb; lb = lb->next) {
		spinrlock(lb);
		if (waserror()) {
			spinrunlock(lb);
			nexterror();
		}
		r = lb->raids;
		re = r + lb->nraids;
		for (; !dp && r < re; r++) {
			spinrlock(r);
			d = r->devs;
			de = d + r->ndevs;
			for (; !dp && d < de; d++) 
				if (strcmp(d->path, path) == 0)
					dp = d;
			spinrunlock(r);
			if (dp) {
				tryfail(dp);
				schedsave();
			}
		}
		poperror();
		spinrunlock(lb);
	}
	poperror();
	spinrunlock(&lblades);
}

static Rowlock *
getrowlock(Rlcache *c, Raid *r, uvlong row)
{
	Rowlock **rh, *rl;

	rh = c->hash + row%Nrlhash;
	lock(c);
	if (waserror()) {
		unlock(c);
		nexterror();
	}
	rl = *rh;
	for (; rl; rl=rl->next) {
		if (rl->r == r)
		if (rl->row == row) {
			rl->ref++;
			goto out;
		}
	}
	rl = c->free;
	if (rl == nil) {
		rl = mallocz(sizeof *rl, 1);
		if (rl == nil) {
			/*
			 * we really have no good recourse here.
			 * bail and hope for the best.
			 */
			error("out of memory");
		}
	}
	c->free = rl->next;
	rl->r = r;
	rl->row = row;
	rl->cache = c;
	rl->next = *rh;
	*rh = rl;
	rl->ref = 1;
	rl->aux = nil;
out:
	poperror();
	unlock(c);
	return rl;
}

static void
putrowlock(Rowlock *rrl)
{
	Rowlock **rh, *rl;
	Rlcache *c;

	c = rrl->cache;
	if (c == nil)
		error("devsr putrowlock with invalid cache");
	rh = c->hash + rrl->row%Nrlhash;
	lock(c);
	rl = *rh;
	for (; rl; rh=&rl->next, rl=*rh) {
		if (rl != rrl)
			continue;
		if (--rl->ref == 0) {
			*rh = rl->next;
			rl->next = c->free;
			c->free = rl;
		}
		unlock(c);
		return;
	}
	unlock(c);
	error("putrowlock on unused rowlock!");
}

static void *
rowlock(Raid *r, uvlong row, long len, uvlong off, int mode)
{
	Rowlock *rl;
	IOUnit *u;

	rl = getrowlock(&r->lblade->rlcache, r, row);
	u = iolock(rl, len, off, mode);
	u->aux = rl;
	return u;
}

static void
rowunlock(void *vp)
{
	IOUnit *u;
	Rowlock *rl;

	u = (IOUnit *)vp;
	rl = (Rowlock *)u->aux;
	iounlock(rl, u);
	putrowlock(rl);
}

/*
 * Direct I/O is gated by the isdirect function. All RAID
 * types other than RAID5/RAID6rs are supported as they do not
 * require read/modify/write for updates. Writes are always
 * direct, however reads may be buffered for sequential I/O.
 */
static int
candirect(Raid *r)
{
	if (!directio)
		return 0;
	switch (r->type) {
	case RTnil:
	case RTfnil:
	case RTraw:
	case RTjbod:
	case RTraidl:
		if (r->ndevs > 1)
			break;
		/* fall thru */
	case RTraid0:
	case RTraid1:
	case RTraid10:
		return 1;
	}
	return 0;
}

static int
isdirect(Raid *r, int mode)
{
	Lun *lb;

	if (!candirect(r))
		return 0;	/* never... */
	if (mode == OWRITE)
		return 1;	/* always! */
	lb = r->lblade;
	return lb->flags & Lnobuf;
}

static int
isredundant(Raid *r)
{
	switch (r->type) {
	case RTnil:
	case RTfnil:
	case RTraw:
	case RTjbod:
	case RTraidl:
	case RTraid0:
		return 0;
	case RTraid1:
	case RTraid5:
	case RTraid6rs:
	case RTraid10:
		return 1;
	default:
		dprint("isredundant: %d is not a raid type!\n", r->type);
	}
	return 0;
}

static void
replaceguard(Raid *r, Rdev *rd, int unfail)
{
	Lun *lb;

	lb = r->lblade;
	if (lb->flags & Lnoguard)
		return;
	if (!isredundant(r))
		errorstr("raid %d.%d is not redundant and cannot rebuild a drive", lb->slot, r->rno);
	if (r->flags & Rfailed)
		errorstr("raid %d.%d is failed", lb->slot, r->rno);
	if (r->flags & Riniting)
		errorstr("raid %d.%d is initializing parity", lb->slot, r->rno);
	if (!(rd->flags & Dfailed))
		errorstr("device %d.%d.%d is not failed", lb->slot, r->rno, rd->dno);
	if (unfail && (rd->flags & Dmissing))
		errorstr("can't unfail missing device %d.%d.%d", lb->slot, r->rno, rd->dno);
}

/*
 * This is all but a complete copy of what's in save, but at this
 * point I don't think there's anything to be gained by generalizing the
 * interface for saving config to a disk in a raid.  This is a special
 * case where we want to set the disk as defunct because we're replacing
 * it and want to leave some post-mortem state about what happened.
 * By labeling it as defunct (as opposed to leaving the config string
 * as-is) we guarantee that the software will never pick this up
 * as a valid element in the future.
 */
static void
defunct(Rdev *d)
{
	Srconfig *s;
	char *p, *e;
	Lun *lb;
	Raid *r;
	ulong ts;
	int i;

	if (d->c == nil)
		return;
	r = d->raid;
	lb = r->lblade;
	s = mallocz(sizeof *s, 1);
	if (!s)
		error("defunct malloc error");
	if (!waserror()) {
		ts = seconds();
		memmove(s->magic, lb->magic, Nmagic);
		p = s->config;
		e = p + Nconflen - 1;
		p = seprint(p, e, "%d %d.%d.%d %d %s %s 0 %lud",
			shelf, lb->slot, r->rno, d->dno,
			r->ndevs, rt2s(r->type), "defunct", ts);
		for (i=0; i<lb->nmacs; i++)
			p = seprint(p, e, " %E", &lb->macs[i*6]);
		hnputs(s->length, p - s->config);
		devtab[d->c->type]->write(d->c, s, sizeof *s, 0);
		poperror();
	}
	free(s);
}

static void
ckmindlen(Raid *r, Rdev *rd)
{
	uvlong mind, rlen;
	Lun *lb;

	lb = r->lblade;
	if (r->mindlen == 0)
		return;
	if (rd->length < r->mindlen + r->doff) {
		wonkdev(rd, 1);
		errorstr("%s is not large enough for raid", rd->name);
	}
	/*
	 * new disk is bigger than minimum length, let's handle this best
	 * we can.
	 */
	raidsizes(r, &mind, &rlen, nil);
	if (mind > r->mindlen) {
		if (!raidresize || lb->nraids - 1 != r->rno) {	/* only expand last raid, if permitted */
			wonkdev(rd, 1);
			errorstr("%s would grow raid to larger size", rd->name);
		}
		print("growing raid to accomodate additional space provided by replacing drive %s (device %d.%d.%d)\n",
			rd->name, lb->slot, r->rno, rd->dno);
		r->mindlen = mind;
		r->length = rlen;
	}
}

static void
replace(Lun *lb, char *rspec, char *rdpath)
{
	Raid *r;
	Rdev *d;
	int unfail = rdpath == nil;

	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	parserspec(rspec, lb, &r, &d);
	replaceguard(r, d, unfail);
	rsync(r);
	spinwlock(r);
	if (waserror()) {
		spinwunlock(r);
		nexterror();
	}
	if (unfail)
		rdpath = d->path;
	else
		defunct(d);
	wonkdev(d, 0);
	if (waserror()) {
		makedev(d, "missing");
		setrflags(r);
		nexterror();
	}
	makedev(d, rdpath);
	ckmindlen(r, d);
	if ((d->flags & Dmissing) == 0)
		devkpinit(d);
	poperror();
	if (isredundant(r))
		d->flags |= Drepl;
	dstatechg(d);
	setrflags(r);
	poperror();
	spinwunlock(r);
	poperror();
	spinrunlock(lb);
	schedsave();
}

/*
#define xor xor_asm
#define xor2 xor2_asm
*/

#define xor xor_c
#define xor2 xor2_c

/* len must be a multiple of 4 */
static void *
xor_c(void *vd, void *vs, ulong len)
{
	ulong *d, *s;
	int nl, n;

	if (len & 3)
		print("error: len & 3!\n");
	d = vd;
	s = vs;
	nl = len >> 2;
	n = (nl+15)/16;
	if (n | nl)
	switch (nl % 16) {
	case 0:	do {	*d++ ^= *s++;
	case 15:		*d++ ^= *s++;
	case 14:		*d++ ^= *s++;
	case 13:		*d++ ^= *s++;
	case 12:		*d++ ^= *s++;
	case 11:		*d++ ^= *s++;
	case 10:		*d++ ^= *s++;
	case 9:		*d++ ^= *s++;
	case 8:		*d++ ^= *s++;
	case 7:		*d++ ^= *s++;
	case 6:		*d++ ^= *s++;
	case 5:		*d++ ^= *s++;
	case 4:		*d++ ^= *s++;
	case 3:		*d++ ^= *s++;
	case 2:		*d++ ^= *s++;
	case 1:		*d++ ^= *s++;
			} while (--n > 0);
	}
	return vd;
}

/*
 * if vs/vss == nil, vd ^= vss/vs.  If both vs and vss are nil, we're an 
 * expensive memset(0).  len must be a multiple of 4.
 */
static void *
xor2_c(void *vd, void *vs, void *vss, ulong len)
{
	ulong *d, *s, *ss;
	int nl, n;

	d = vd;
	if ((s = vs) == nil)
		s = vd;
	if ((ss = vss) == nil)
		ss = vd;
	nl = len >> 2;
	n = (nl+15)/16;
	if (n | nl)
	switch (nl % 16) {
	case 0:	do {	*d++ = *s++ ^ *ss++;
	case 15:		*d++ = *s++ ^ *ss++;
	case 14:		*d++ = *s++ ^ *ss++;
	case 13:		*d++ = *s++ ^ *ss++;
	case 12:		*d++ = *s++ ^ *ss++;
	case 11:		*d++ = *s++ ^ *ss++;
	case 10:		*d++ = *s++ ^ *ss++;
	case 9:		*d++ = *s++ ^ *ss++;
	case 8:		*d++ = *s++ ^ *ss++;
	case 7:		*d++ = *s++ ^ *ss++;
	case 6:		*d++ = *s++ ^ *ss++;
	case 5:		*d++ = *s++ ^ *ss++;
	case 4:		*d++ = *s++ ^ *ss++;
	case 3:		*d++ = *s++ ^ *ss++;
	case 2:		*d++ = *s++ ^ *ss++;
	case 1:		*d++ = *s++ ^ *ss++;
			} while (--n > 0);
	}
	return vd;
}

typedef struct Tbuf Tbuf;
struct Tbuf {
	Buf b;
	uchar db[Nstride];	/* nbsize never exceeds Nstride */
	Tbuf *next;
};

struct {
	Lock;
	Tbuf *free;
} tbufs;

/* temporary buffers for raid6. */

static Buf *
alloctbuf(void)
{
	Tbuf *p;

	lock(&tbufs);
	p = tbufs.free;
	if (p == nil)
		p = mallocz(sizeof *p, 1);
	p->b.db = p->db;
	tbufs.free = p->next;
	unlock(&tbufs);
	return (Buf *) p;
}

static void
freetbuf(Buf *bp)
{
	Tbuf *p = (Tbuf *) bp;

	lock(&tbufs);
	p->next = tbufs.free;
	tbufs.free = p;
	unlock(&tbufs);
}

static void
raid6pq(Raid *r, uvlong bno, Rdev **pd, Rdev **qd)
{
	int nd = r->ndevs;
	uvlong std = bno2std(bno);

	*qd = r->devs + (nd-1) - (std%nd);
	*pd = r->devs + (((nd-2) - (std%nd))+nd) % nd;
}

static uvlong
raid6row(Raid *r, uvlong row)
{
	return row / (r->ndevs - 2);
}

static void
qmulxor(void *dba, void *dbb, long len)
{
	ulong *a, *b, *e, vv, v;

	if (len & 3)
		error("qmulxor: len misaligned");
	a = (ulong *) dba;
	b = (ulong *) dbb;
	e = a + len/sizeof *a;
	for (; a<e; a++, b++) {
		v = *a;
		vv = (v<<1) & 0xfefefefe;

		/* mask(v) */
		v &= 0x80808080;
		v = (v<<1) - (v>>7);

		v &= 0x1d1d1d1d;
		*a = vv ^ v ^ *b;
	}
}

static void
rsr6_Q(Buf *bp, Raid *r, uvlong row, Rdev *p, Rdev *q)
{
	Rdev *d, *e;
	Buf *bp2;
	int n;

	n = 0;
	d = r->devs + r->ndevs - 1;
	e = r->devs - 1;
	for (; d>e; d--) {
		if (d == q || d == p)
			continue;
		bp2 = getbuf(d, row, Bread);
		if (n)
			qmulxor(bp->db, bp2->db, nbsize);
		else {
			memmove(bp->db, bp2->db, nbsize);
			n++;
		}
		putbuf(bp2);
	}
}

/*
 * p and q are basically a 2 element skip list for this function.
 * calculate the xor parity skipping these two devices.
 */
static void
rsr6_P(Buf *bp, Raid *r, uvlong row, Rdev *p, Rdev *q)
{
	Rdev *d, *e;
	Buf *bp2;
	int n;

	n = 0;
	d = r->devs;
	e = d + r->ndevs;
	for (; d<e; d++) {
		if (d == p || d == q)
			continue;
		bp2 = getbuf(d, row, Bread);
		if (n)
			xor(bp->db, bp2->db, nbsize);
		else {
			memmove(bp->db, bp2->db, nbsize);
			n++;
		}
		putbuf(bp2);
	}
}

static void
rsr6_mul(void *dbd, void *dbs, uchar *tab)
{
	uchar *d, *s, *e;

	d = dbd;
	if ((s=dbs) == nil)
		s = dbd;
	e = d + nbsize;
	for (; d<e; d++, s++)
		*d = tab[*s];
}

/* map disk dno down to index for mul math to work */
static int
rsr6_didx(int n, int p, int q)
{
	int on = n;

	if (on > p)
		n--;
	if (on > q)
		n--;
	return n;
}

/*
 * the math in this function differs slightly from that documented by hpa in his
 * "mathematics of raid6" paper.  This approach uses his Linux raid6 implementation
 * instead which appears to recover erasures just as effectively with two less
 * multiplication mappings.
 * 
 * Dp = bpp;
 * Dq = bpq;
 * Pxy = tp;
 * Qxy = tq;
 * Dx = dbx;
 * Dy = dby;
 * 
 * A = gfinv[ gfilog[ y-x ] ^ 1 ];
 * B = gfmul[ gfinv[ gfilog[x] ^ gfilog[y] ] ];
 * Dy = gfmul[A][ Dp ^ Pxy ] ^ gfmul[B][ Dq ^ Qxy ];
 * Dx = Dy ^ Dp ^ Pxy;
 */
static
void rsr6_hpaAB(void *dbx, void *dby, void *tp, void *tq, int x, int y)
{
	uchar *a, *b;

	a = gfmul[ gfinv[ gfilog[y-x] ^ 1 ] ];
	b = gfmul[ gfinv[ gfilog[x] ^ gfilog[y] ] ];
	rsr6_mul(tq, nil, b);
	rsr6_mul(dbx, tp, a);				/* use x as temporary buffer to calculate y */
	xor2(dby, tq, dbx, nbsize);
	xor2(dbx, dby, tp, nbsize);			/* because we overwrite it here */
}

static void
rsr6_PD(Buf *pbp, Buf *dbp, Raid *r, uvlong row, Rdev *pd, Rdev *qd)
{
	Buf *qbp;
	Rdev *dd;
	int n;

	dd = dbp->d;

	/* calc Q' with D buf set to 0 */
	memset(dbp->db, 0, nbsize);
	rsr6_Q(pbp, r, row, pd, qd);

	/* Q'' = Q' ^ Q */
	qbp = getbuf(qd, row, Bread);
	xor(pbp->db, qbp->db, nbsize);
	putbuf(qbp);

	/* mul Q'' to get missing D */
	n = rsr6_didx(dd->dno, pd->dno, qd->dno);
	rsr6_mul(dbp->db, pbp->db, gfmul[gfinv[gfilog[n]]]);
	/* dbp now reconstructed */

	/* now recalc P using good D */
	rsr6_P(pbp, r, row, pd, qd);
}

static void
rsr6_DP(Buf *dbp, Buf *pbp, Raid *r, uvlong row, Rdev *pd, Rdev *qd)
{
	rsr6_PD(pbp, dbp, r, row, pd, qd);
}

static void
rsr6_QD(Buf *qbp, Buf *dbp, Raid *r, uvlong row, Rdev *pd, Rdev *qd)
{
	/* calculate D from parity */
	rsr6_P(dbp, r, row, dbp->d, qd);

	/* calculate Q using new D */
	rsr6_Q(qbp, r, row, pd, qd);
}

static void
rsr6_DQ(Buf *dbp, Buf *qbp, Raid *r, uvlong row, Rdev *pd, Rdev *qd)
{
	rsr6_QD(qbp, dbp, r, row, pd, qd);
}

static void
rsr6_DD(Buf *bp0, Buf *bp1, Raid *r, uvlong row, Rdev *pd, Rdev *qd)
{
	Buf *bpp, *bpq, *sb, *tp, *tq;
	int x, y, s;
	Rdev *d0, *d1;

	tp = alloctbuf();
	if (waserror()) {
		freetbuf(tp);
		nexterror();
	}
	tq = alloctbuf();
	if (waserror()) {
		freetbuf(tq);
		nexterror();
	}

	/* calculate P' and Q' with both D set to 0 */
	memset(bp0->db, 0, nbsize);
	memset(bp1->db, 0, nbsize);
	rsr6_P(tp, r, row, pd, qd);
	rsr6_Q(tq, r, row, pd, qd);

	d0 = bp0->d;
	d1 = bp1->d;
	x = rsr6_didx(d0->dno, pd->dno, qd->dno);
	y = rsr6_didx(d1->dno, pd->dno, qd->dno);
	if (x > y) {		/* get them in the right order for the math */
		s = x; x = y; y = s;
		sb = bp0; bp0 = bp1; bp1 = sb;
	}
	bpp = getbuf(pd, row, Bread);
	xor(tp->db, bpp->db, nbsize);
	putbuf(bpp);
	bpq = getbuf(qd, row, Bread);
	xor(tq->db, bpq->db, nbsize);
	putbuf(bpq);
	rsr6_hpaAB(bp0->db, bp1->db, tp->db, tq->db, x, y);
	poperror();
	freetbuf(tq);
	poperror();
	freetbuf(tp);
}	

static void
rsr6_PQ(Buf *pbp, Buf *qbp, Raid *r, uvlong row, Rdev *pd, Rdev *qd)
{
	rsr6_P(pbp, r, row, pd, qd);
	rsr6_Q(qbp, r, row, pd, qd);
}

static void
rsr6_QP(Buf *qbp, Buf *pbp, Raid *r, uvlong row, Rdev *pd, Rdev *qd)
{
	rsr6_PQ(pbp, qbp, r, row, pd, qd);
}

static void
brecon(Buf *bp, ulong flags)
{
	Rdev *d = bp->d;

	lock(bp->clk);
	if (!(d->flags & (Dmissing|Dfailed))) {
		bp->flags &= ~Bghost;
		bp->flags |= flags;
		if (flags & Bmod)
			bp->modts = Ticks;
	}
	bp->flags &= ~Bfailed;
	bp->flags |= Brecon;
	unlock(bp->clk);
}

static char *Ebadness = "too many bad devices";

/* blech.  This is written very defensively so that if we have
 * a secondary failure while we're recovering a single disk failure block,
 * we'll sustain it.  By fetching all the row blocks up front, we can
 * determine what we need to recover.
 */
static void
rsr6_recover(Buf *bp, uvlong bno)
{
	Rdev *xd, *p, *q, *d, *e, *rd;
	Raid *r;
	Buf *bp2;
	void (*dfr)(Buf *, Buf *, Raid *, uvlong, Rdev *, Rdev *);
	Buf *rowbufs[Nrdevs];
	int i;

	rd = bp->d;
	r = rd->raid;
	memset(rowbufs, 0, sizeof rowbufs);
	if (waserror()) {
		for (i=0; i<r->ndevs; i++)
			if (rowbufs[i])
				putbuf(rowbufs[i]);
		nexterror();
	}
	raid6pq(r, bno, &p, &q);

	/* 1 disk failure or 2 disk? */
	xd = nil;
	d = r->devs;
	e = d + r->ndevs;
	for (; d<e; d++) {
		if (d == rd)
			continue;
		if (DEVBAD(d)) {
			if (xd)
				error(Ebadness);
			xd = d;
		} else if (waserror()) {
			if (strcmp(up->errstr, Etoolong) == 0)
				nexterror();
			if (xd)
				error(Ebadness);
			xd = d;
		} else {
			rowbufs[d-r->devs] = getbuf(d, bno, Bread);
			poperror();
		}
	}

	/* single failure */
	if (xd == nil) {
		xd = rd != q ? q : p;
		bp2 = rowbufs[xd - r->devs];
		if ((r->flags & Rneedinit) == 0 || (bp2 && (bp2->flags & Brecon))) {
			if (rd == q)
				rsr6_Q(bp, r, bno, p, q);
			else if (rd == p)
				rsr6_P(bp, r, bno, p, q);
			else
				rsr6_P(bp, r, bno, rd, q);	/* skip data disk rd */
			goto e;
		}
		/*
		 * this deserves a note.  if the raid6 parity is in a good state (!Rneedinit) it's
		 * ok to continue with a single disk failure reconstruction.  If it's not, we
		 * need to ensure both parity (P,Q) are consistent.  If rd is P or any D,
		 * then P will be set correctly and we need to ensure Q is correct.  If rd
		 * is Q, then we need to ensure P is correct.  We set xd appropriately above
		 * and if we have to fall back to double failure recovery we fall thru here.
		 */
	}

	/* double failure recovery */
	if (rd == p)
		dfr = xd == q ? rsr6_PQ : rsr6_PD;
	else if (rd == q)
		dfr = xd == p ? rsr6_QP : rsr6_QD;
	else if (xd == p)
		dfr = rsr6_DP;
	else if (xd == q)
		dfr = rsr6_DQ;
	else
		dfr = rsr6_DD;

	bp2 = getbuf(xd, bno, 0);
	if (waserror()) {
		putbuf(bp2);
		nexterror();
	}
	dfr(bp, bp2, r, bno, p, q);
	/*
	 * if the device isn't bad, then this block *just* failed - set Bmod to get it
	 * flushed to disk.  Otherwise let rebuild deal with it directly.
	 */
	brecon(bp2, DEVBAD(xd) ? 0 : Bmod);
	poperror();
	putbuf(bp2);
e:
	poperror();
	for (i=0; i<r->ndevs; i++)
		if (rowbufs[i])
			putbuf(rowbufs[i]);
}

static Buf *
_recovered(Rdev *rd, uvlong bno, int flags)	/* no rowlock */
{
	Buf *bp, *bp2;
	Rdev *d, *e;
	Raid *r;
	void *(*f)(void *, void *, ulong);

	r = rd->raid;
	bp = getbuf(rd, bno, 0);
	if (bp->flags & Brecon)
		goto recon;
	if (waserror()) {
		putbuf(bp);
		nexterror();
	}
	switch (r->type) {
	case RTraid1:
	case RTraid10:
		d = dmirror(rd);
		if (DEVBAD(d))
			error("too many bad devices");
		bp2 = getbuf(d, bno, Bread);
		memmove(bp->db, bp2->db, nbsize);
		putbuf(bp2);
		break;
	case RTraid5:
		d = r->devs;
		e = d + r->ndevs;
		f = memmove;
		for (; d<e; d++) {
			if (d != rd) {
				if (DEVBAD(d))
					error("too many bad devices");
				bp2 = getbuf(d, bno, Bread);
				f(bp->db, bp2->db, nbsize);
				f = xor;
				putbuf(bp2);
			}
		}
		break;
	case RTraid6rs:
		rsr6_recover(bp, bno);
		break;
	}
	poperror();
recon:
	brecon(bp, flags);
	return bp;
}

static Buf *
recovered(Rdev *rd, uvlong bno, int flags)	/* fetch block in spite of a failure */
{
	Buf *bp;
	Rowlock *rl;
	Raid *r;

	r = rd->raid;
	if (!isredundant(r))
		error("can't recover from non-redundant raid");
	rl = rowlock(r, bno, 0, 0, OEXCL);	/* bno is offset into disk, NOT raid. */
	while (waserror()) {
		rowunlock(rl);
		if (strcmp(up->errstr, Etoolong) != 0)
			nexterror();
		yield();
		*up->errstr = 0;
		rl = rowlock(r, bno, 0, 0, OEXCL);
	}
	bp = _recovered(rd, bno, flags);
	poperror();
	rowunlock(rl);
	return bp;
}

/*
 * translate bno in raid into bno on a disk.
 * If parity, get the parity disk associated with this raid bno.
 * parity == 1, get P; o/w != 0, get Q
 */
static void
raidxlate(Raid *r, uvlong bno, Rdev **dd, uvlong *dbno, int parity)
{
	int disk, nd;
	Lun *lb;
	Rdev *d;
	uvlong std, xstd, xbno, b = 0;

	std = bno2std(bno);		/* offset in std into raid */

	lb = r->lblade;
	nd = r->ndevs;
	if (parity && !isredundant(r))
		errorstr("raid type %d is not redundant", r->type);
	switch (r->type) {
	default:
		errorstr("no such raid type %d", r->type);
	case RTraw:
	case RTjbod:
	case RTraidl:
		for (disk=0; disk<nd; disk++) {
			b = r->devs[disk].length / Nstride;
			if (std < b)
				break;
			std -= b;
		}
		if (disk == nd)
			errorstr("bno %llud past end of raid on %d", bno, lb->slot);
		xbno = std2bno(std, bno);
		d = r->devs + disk;
		break;
	case RTraid0:
		xstd = std / nd;
		xbno = std2bno(xstd, bno);
		disk = std % nd;
		d = r->devs + disk;
		break;
	case RTraid5:
		xstd = std / (nd - 1);
		if (parity) {
			disk = nd - 1;
			disk -= xstd % nd;
		} else
			disk = std % nd;
		xbno = std2bno(xstd, bno);
		d = r->devs + disk;
		break;
	case RTraid6rs:
		xstd = raid6row(r, std);
		switch (parity) {
		case 0:	/* data */
			disk = (std+xstd) % nd;
			break;
		case 1:	/* P */
			disk = nd - 2 - xstd%nd;
			disk += nd;
			disk %= nd;
			break;
		default:	/* Q */
			disk = nd - 1 - xstd%nd;
			break;
		}
		xbno = std2bno(xstd, bno);
		d = r->devs + disk;
		break;
	case RTraid1:
	case RTraid10:
		xstd = std / (r->ndevs / 2);
		xbno = std2bno(xstd, bno);
		disk = std % (r->ndevs / 2);
		d = r->devs + disk;
		if (parity)
			d = dmirror(d);
		break;
	}
	if (xbno > bno)
		errorstr("raidxlate map error: bno=%ulld xbno=%ulld std=%ulld t=%d",
			bno, xbno, std, r->type);
	tprint("raidxlate: %d.%d %s std/xstd: %ulld/%ulld bno/xbno: %ulld/%ulld drive/nd: %d/%d parity: %d\n",
		lb->slot, r->rno, rt2s(r->type), std, xstd, bno, xbno, disk, nd, parity);
	if (dd)
		*dd = d;
	if (dbno)
		*dbno = xbno;
}

/* getbuf for raids */
static Buf *
getraid(Raid *r, uvlong bno, int flags, int parity)
{
	Rdev *d;

	raidxlate(r, bno, &d, &bno, parity);
	if (DEVBAD(d))
		return recovered(d, bno, flags);
	return getbuf(d, bno, flags);
}

static void
pregetraid(Raid *r, uvlong bno, int parity)
{
	Rdev *d;

	raidxlate(r, bno, &d, &bno, parity);
	if (DEVBAD(d))
		return;
	preget(d, bno);
}

static void
soilraid(Raid *r)
{
	Rdev *d, *e;

	spinwlock(r);
	d = r->devs;
	e = d + r->ndevs;
	for (; d<e; d++)
		d->flags &= ~Dclean;
	r->flags &= ~Rclean;
	spinwunlock(r);
	schedsave();
}

static void
kickthespares(void *)
{
	Rdev *d, *e;
	uint n;
	char buf[512];
	enum { Nminday= 24*60, Nminsixh= 6*60};
loop:
	tsleep(&up->sleep, return0, 0, 60*1000);
	qlock(&spares);
	d = spares.devs;
	e = d + spares.ndevs;
	for (; d<e; d++) {
		n = 0;
		if (!waserror()) {
			n = devtab[d->c->type]->read(d->c, buf, sizeof buf, 0);
			poperror();
		}
		if (n != sizeof buf) {
			n = d->badspare++;
			/* first 3 minutes, then every 6 hours for the first day, then once a day */
			if (n < 3 || (n < Nminday && n%Nminsixh == 0) || n%Nminday == 0) {
				jskevent("msg:s:'Warning: spare drive %s is not responding to periodic check.'"
			         	 " sev:n:2 tag:s:SRX_SPARE_OFFLINE %S", d->name, d);
			}
		} else if (d->badspare) {
			jskevent("msg:s:'Spare drive %s is now responding.'"
				 " sev:n:5 tag:s:SRX_SPARE_ONLINE %S", d->name, d);
			d->badspare = 0;
			schedsave();		/* something tells me the user will "solve" this by swapping the disk. */
		}
	}
	qunlock(&spares);
	goto loop;
}

static void
scrub(void *vp)
{
	Raid *r = vp;
	Rdev *d, *e;
	Buf *bp;
	int n, bkps, dgfl, nbps;
	uvlong nblks, blk;

	dgfl = Rdegraded;
	if (r->type == RTraid6rs)
		dgfl = R2degraded;
	nblks = r->mindlen / nbsize;
	if (nblks == 0) {
		dprint("scrub: size of device is zero.  scrubber exiting.\n");
stop:
		spinwlock(r);
		r->flags &= ~Rscrubbing;
		spinwunlock(r);
		pexit("scrub exiting", 1);
	}
	blk = nrand(nblks);
	nbps = (1024*1024*16)/nbsize;
	n = bkps = (nbps / r->ndevs);
loop:
	if (r->flags & Rstop)
		goto stop;
	if (scrubrl)
	if (TK2SEC(Ticks - r->lastaccess) < 60 || n-- == 0) {
		tsleep(&up->sleep, return0, 0, 1000);
		n = bkps;
	}
	spinrlock(r);
	if (!(r->flags & (Rfailed|Riniting|Rrecovering|dgfl))) {
		if (!waserror()) {
			d = r->devs;
			e = d + r->ndevs;
			for (; d<e; d++) {
				if (DEVBAD(d))
					continue;
				bp = getbuf(d, blk, Bread);
				putbuf(bp);
			}
			poperror();
		} else {
			spinrunlock(r);
			if (strcmp(up->errstr, Etoolong) != 0)
				raidshield(r, blk);
			*up->errstr = 0;
			goto loop;
		}
	}
	blk++;
	blk %= nblks;
	spinrunlock(r);
	goto loop;
}

static void
recoverdisk(void *vp)
{
	Raid *r;
	Lun *lb;
	Rdev *d, *e, *fd;
	uvlong nblks, blk;
	ulong bpr;		/* bytes per row */
	long ratelimit = 0;
	Rowlock *rl;
	int aborted;
	int flags;

	aborted = 0;
	flags = 0;
	d = vp;
	r = d->raid;
	lb = r->lblade;
	switch (r->type) {
	default:
		print("can't recover on raid %d.%d of type %d\n",
			lb->slot, r->rno, r->type);
		break;
	case RTraid6rs:
	case RTraid5:
		zfilter(&d->filter);
		addfilter(&d->filter);
		spinrlock(r);
		ainc(&r->nrecovery);
		jskevent("msg:s:'beginning recovery of drive %s (device %d.%d.%d)'"
			 " sev:n:2 tag:s:SRX_DRIVE_RECOVERY_STARTED %S"
			 " lun:n:%d raidpart:n:%d drivepart:n:%d", d->name,
		 	 lb->slot, r->rno, d->dno, d, lb->slot, r->rno, d->dno);
		nblks = r->mindlen / nbsize;
		bpr = nbsize * r->ndevs;
		if (!waserror()) {
			for (blk=0; blk<nblks; blk++, spinrlock(r)) {
				d->row = blk;
				if (r->flags & Rstop) {
					print("recovery suspended: drive %s (device %d.%d.%d)\n",
						d->name, lb->slot, r->rno, d->dno);
					spinrunlock(r);
					delfilter(&d->filter);
					poperror();
					goto exit;
				}
				if ((r->flags & Rfailed) | (d->flags & Dfailed))
					error("recover failed");
				putbuf(recovered(d, blk, Bmod));
				spinrunlock(r);
				incfilter(&d->filter, bpr, 0);

				if (rlrate)
				if (TK2SEC(Ticks - r->lastaccess) < 60) {
					ratelimit -= bpr;
					if (ratelimit < 0) {
						tsleep(&up->sleep, return0, 0, 1000);
						ratelimit = rlrate;
					}
				}
			}
			poperror();
			flags = Rneedinit;
			print("recovery complete: drive %s (device %d.%d.%d)\n",
				d->name, lb->slot, r->rno, d->dno);
		} else {
			jskevent("msg:s:'aborted recovery of drive %s (device %d.%d.%d: %s)'"
				 " sev:n:2 tag:s:SRX_DRIVE_RECOVERY_ABORTED %S"
				 " lun:n:%d raidpart:n:%d drivepart:n:%d", d->name,
			  	 lb->slot, r->rno, d->dno, up->errstr, d, lb->slot, r->rno, d->dno);
			aborted = 1;
		}
		break;
	case RTraid1:
	case RTraid10:
		zfilter(&d->filter);
		addfilter(&d->filter);
		spinrlock(r);
		ainc(&r->nrecovery);
		jskevent("msg:s:'beginning recovery of drive %s (device %d.%d.%d)'"
			 " sev:n:2 tag:s:SRX_DRIVE_RECOVERY_STARTED %S"
			 " lun:n:%d raidpart:n:%d drivepart:n:%d", d->name,
			 lb->slot, r->rno, d->dno, d, lb->slot, r->rno, d->dno);
		bpr = nbsize * 2;
		nblks = r->mindlen / nbsize;
		if (!waserror()) {
			for (blk=0; blk<nblks; blk++, spinrlock(r)) {
				d->row = blk;
				if (r->flags & Rstop) {
					print("recovery suspended: drive %s (device %d.%d.%d)\n",
						d->name, lb->slot, r->rno, d->dno);
					spinrunlock(r);
					delfilter(&d->filter);
					poperror();
					goto exit;
				}
				if ((r->flags & Rfailed) | (d->flags & Dfailed))
					error("recover failed");
				rl = rowlock(r, blk, 0, 0, OEXCL);
				while (waserror()) {
					rowunlock(rl);
					if (strcmp(up->errstr, Etoolong) != 0)
						nexterror();
					yield();
					*up->errstr = 0;
					rl = rowlock(r, blk, 0, 0, OEXCL);
				}
				putbuf(_recovered(d, blk, Bmod|Bimm));
				poperror();
				rowunlock(rl);
				spinrunlock(r);
				incfilter(&d->filter, bpr, 0);

				if (rlrate)
				if (TK2SEC(Ticks - r->lastaccess) < 60) {
					ratelimit -= bpr;
					if (ratelimit < 0) {
						tsleep(&up->sleep, return0, 0, 1000);
						ratelimit = rlrate;
					}
				}
			}
			poperror();
			print("recovery complete: drive %s (device %d.%d.%d)\n",
				d->name, lb->slot, r->rno, d->dno);
		} else {
			jskevent("msg:s:'aborted recovery of drive %s (device %d.%d.%d: %s)'"
				 " sev:n:2 tag:s:SRX_DRIVE_RECOVERY_ABORTED %S"
				 " lun:n:%d raidpart:n:%d drivepart:n:%d",
				 d->name, lb->slot, r->rno, d->dno,
				 up->errstr, d, lb->slot, r->rno, d->dno);
			aborted = 1;
		}
	}
	spinrunlock(r);
	delfilter(&d->filter);
	spinwlock(r);
	d->flags &= ~(Drepl|Dreplacing);
	if (aborted) {
		if (getdf(r, &fd, blk) == 0) {
			dprint("cannot determine failed drive - failing rebuild drive.\n");
			fd = d;
		} else if (fd != d)
			faildev(d);
		faildev(fd);		
	}
	else {
		dstatechg(d);		
	}
	setrflags(r);
	r->flags &= ~flags;
	d = r->devs;
	e = d + r->ndevs;
	for (; d<e; d++)
		if (d->flags & Dreplacing)
			break;
	if (d == e)
		r->flags &= ~Rrecovering;
	spinwunlock(r);
exit:
	schedsave();	/* schedule scanproc to run now that recoverdisk has completed. */
	adec(&r->nrecovery);
	pexit(up->errstr, 1);
}

static void
raidparity(Raid *r, uvlong bno)
{
	Rdev *p, *q;
	int nd;
	uvlong std;

	std = bno2std(bno);
	switch (r->type) {
	case RTraid6rs:
		raid6pq(r, bno, &p, &q);
		putbuf(recovered(p, bno, Bmod|Bimm));
		putbuf(recovered(q, bno, Bmod|Bimm));
		break;
	case RTraid5:
		nd = r->ndevs;
		p = r->devs + (nd - 1 - std%nd);
		putbuf(recovered(p, bno, Bmod|Bimm));
	}
}

static int
isparity(Rdev *d, uvlong bno)
{
	int nd, x;
	Raid *r;
	uvlong std;

	std = bno2std(bno);
	r = d->raid;
	nd = r->ndevs;
	switch(r->type) {
	case RTraid5:
		x = (nd-1) - std % nd;
		return x == d->dno;
	case RTraid6rs:
		x = (nd-2) - std % nd;
		x += nd;
		return (x%nd == d->dno || (x+1)%nd == d->dno);
	default:
		return 0;
	}
}

static int
skipbno(Rdev *d, uvlong bno)
{
	Raid *r;

	r = d->raid;
	switch (r->type) {
	case RTraid5:
	case RTraid6rs:
		if (!parityprespec)
			return isparity(d, bno);
		return 0;
	case RTraid1:
	case RTraid10:
		if (isdirect(r, OREAD))
			return skipmirror(d, bno);
		/* fall thru */
	default:
		return 0;
	}
}

static uvlong
nextbno(Rdev *d, uvlong bno)
{				
	while(skipbno(d, ++bno))
		;
	return bno;
}

static uvlong
prevbno(Rdev *d, uvlong bno)
{
	while(skipbno(d, --bno))
		;
	return bno;
}

/* used by raid5, raid6rs, bno is the row to prefetch for future use */
static void
pregetblkrow(Raid *r, uvlong bno, int parity)
{
	Rdev *od, *d, *e;

	d = od = r->devs + up->pid%r->ndevs;
	e = r->devs + r->ndevs;
	do {
		if (++d >= e)
			d = r->devs;
		if (!parity && isparity(d, bno))
			continue;
		if (DEVBAD(d))
			continue;
		if (!raidpgpeek || dcpeek(d, bno) != Sfound)
			preget(d, bno);
	} while (d != od);
}

static void
buildparity(void *vp)
{
	Raid *r;
	Lun *lb;
	vlong row, nrows;
	ulong bpr;	/* bytes per row */
	int flags, i;
	long ratelimit = 0;

	r = vp;
	lb = r->lblade;
	switch(r->type) {
	case RTraid5:
	case RTraid6rs:
		nrows = r->mindlen / nbsize;
		break;
	default:
		dprint("unknown raid type for buildparity\n");
		nrows = 0;
	}
	bpr = nbsize * r->ndevs;
loop:
	jskevent("msg:s:'beginning building parity: %d.%d' sev:n:4 tag:s:SRX_PARITY_STARTED"
		 " shelf:n:%d lun:n:%d", lb->slot, r->rno, lb->slot, r->rno); 
	zfilter(&r->filter);
	addfilter(&r->filter);
	row = r->roff;
	spinrlock(r);
	while (waserror()) {
		spinrunlock(r);
		raidshield(r, row);	/* will spinwlock(r), gotta let the rlock go. */
		spinrlock(r);
	}
	for (; row<nrows; row++, spinrlock(r)) {
		r->roff = row;
		if (r->flags & (Rfailed|Rdegraded|Rstop))
			break;
		if (row%pgrows == 0)
		if (row+pgrows < nrows)
			for (i = 0; i < pgrows; i++)
				pregetblkrow(r, row+pgrows+i, paritypg || r->type == RTraid6rs);
		raidparity(r, row);
		spinrunlock(r);
		incfilter(&r->filter, bpr, 0);

		if (row % 100000 == 0)
			schedsave();

		if (rlrate)
		if (TK2SEC(Ticks - r->lastaccess) < 60) {
			ratelimit -= bpr;
			if (ratelimit < 0) {
				tsleep(&up->sleep, return0, 0, 1000);
				ratelimit = rlrate;
			}
		}
	}
	poperror();
	spinrunlock(r);
	delfilter(&r->filter);
	flags = 0;
	if (row == nrows) {
		jskevent("msg:s:'building parity complete: %d.%d' sev:n:5 tag:s:SRX_PARITY_COMPLETED"
			 " shelf:n:%d lun:n:%d", lb->slot, r->rno, lb->slot, r->rno); 
		if (buildparityloop) {
			r->roff = 0;
			goto loop;
		}
		r->roff = nrows;
		flags = Rneedinit;
	} else {
		jskevent("msg:s:'building parity aborted: %d.%d' sev:n:2 tag:s:SRX_PARITY_ABORTED"
			 " shelf:n:%d lun:n:%d", lb->slot, r->rno, lb->slot, r->rno); 
	}
	spinwlock(r);
	r->flags &= ~(Riniting | flags);
	spinwunlock(r);
	schedsave();
	pexit("", 1);
}

static void
schedsave(void)
{
	incref(&scanref);
	wakeup(&scanrendez);
}

static int
scanready(void *)
{
	return scanref.ref;
}

/*
 * lazy.  was on the stack and Rdev got big enough to cause a problem.
 * I just don't feel like rewriting the function.
 */
static Rdev scandev;

static void
scanproc(void *)
{
	Raid *r, *re;
	Lun *lb;
	Rdev *d, *de, *rd = &scandev;
loop:
	if (xchgl((ulong*)&scanref.ref, 0) != 0)
	if (!waserror()) {
		save();
		poperror();
	}
	spinrlock(&lblades);
	lb = lblades.head;
	for (; lb; lb=lb->next) {
		spinrlock(lb);
		r = lb->raids;
		re = r + lb->nraids;
		for (; r<re; r++) {
			spinwlock(r);
			setrflags(r);
			if ((r->flags & (Rfailed|Rstop|Rdegraded|Riniting)) != Rdegraded) {
				spinwunlock(r);
				continue;
			}
			d = r->devs;
			de = d + r->ndevs;
			for (; d<de; d++) {
				if (d->flags & (Dfailed|Dmissing)) {
					/*
					 * We found a failed disk and need to allocate a spare
					 * to begin recovering, however there is already a recovery
					 * going on. This can lead to a deadlock where we are waiting
					 * for the replace to finish, but recoverdisk is waiting on a
					 * wlock to fail the dev.  So we will continue here, and when
					 * recoverdisk finishes it will reschedule scanproc to run again.
					 */
					 if (d->flags & Dreplacing)
						continue;
					 if (getspare(rd, r->mindlen + r->doff)) {
						rd->raid = d->raid;
						rd->dno = d->dno;
						defunct(d);
						wonkdev(d, 0);
						*d = *rd;
						devcacheinit(d);	/* reset devcache pointers to this dev */
						d->flags |= Drepl;
						devkpinit(d);
						dstatechg(d);
						incref(&scanref);
						jskevent("msg:s:'spare %s allocated for %d.%d.%d'"
							 " sev:n:4 tag:s:SRX_SPARE_ALLOCATED"
							 " %S lun:n:%d raidpart:n:%d drivepart:n:%d",
							 d->name, lb->slot, r->rno, d->dno, d,
							 lb->slot, r->rno, d->dno); 
					} else if (!(d->flags & Dnospare)) {
						d->flags |= Dnospare;
						jskevent("msg:s:'no spare large enough for drive"
							 " %s (device %d.%d.%d)' sev:n:2"
							 " tag:s:SRX_SPARE_SIZE_FAILURE %S"
							 " lun:n:%d raidpart:n:%d drivepart:n:%d",
							 d->name, lb->slot, r->rno, d->dno,
							 d, lb->slot, r->rno, d->dno); 
					}
				} else if ((d->flags & (Drepl|Dreplacing)) == Drepl) {
					r->flags |= Rrecovering;
					d->flags |= Dreplacing;
					sprint(up->genbuf, "recover%d.%d", lb->slot, r->rno);
					kproc(up->genbuf, recoverdisk, d);
				}
			}
			spinwunlock(r);
		}
		spinrunlock(lb);
	}
	spinrunlock(&lblades);
	sleep(&scanrendez, scanready, 0);
	goto loop;
}

static int
refreshbno(Rdev *d, uvlong bno)
{
	Buf *bp;
	int n;
	Raid *r;
	void *rl;
	Elv *v;

	r = d->raid;
	rl = rowlock(r, bno, 0, 0, OEXCL);
	while (waserror()) {
		rowunlock(rl);
		if (strcmp(up->errstr, Etoolong) != 0) {
			dprint("refreshbno Bfail\n");
			return Bfailed;
		}
		yield();
		*up->errstr = 0;
		rl = rowlock(r, bno, 0, 0, OEXCL);
	}
	bp = _recovered(d, bno, Bmod);
	poperror();

	if (bp->flags & Brefreshed) {
		rowunlock(rl);
		putbuf(bp);
		return Brefreshed;
	}
	lock(bp->clk);
	bp->flags |= Bio;
	unlock(bp->clk);
	v = bp2elv(bp);
	elvreset(v);
	n = rbufio(v);			/* writes out buffer */
	v = bp2elv(bp);
					/* XXX need device cache flush here */
	elvreset(v);
	n |= rbufio(v);			/* reads it back in */
	lock(bp->clk);
	bp->flags |= n & (Bghost|Bfailed) ? Bghost : Brefreshed;
	unlock(bp->clk);
	putbuf(bp);
	rowunlock(rl);
	return n;
}

static int
getdf0(Raid *r, Rdev **rd, uvlong *bno)
{
	char buf[ERRMAX];
	char *argv[16];
	int argc;
	Rdev *d;

	strncpy(buf, up->errstr, ERRMAX-1);
	buf[ERRMAX-1] = 0;
	argc = getfields(buf, argv, nelem(argv), 1, ":");
	if (argc < 3 || strcmp(argv[0], DEVF)) {
		dprint("getdf0: %s fmt error\n", up->errstr);
		return -1;
	}
	d = *rd = (Rdev *) strtoull(argv[1], 0, 16);
	if (d < r->devs || d >= &r->devs[r->ndevs]) {
		dprint("getdf0 device mismatch: r=%#p d=%#p r->devs=%#p r->ndevs=%d\n",
			r, d, r->devs, r->ndevs);
		return -1;
	}
	*bno = strtoll(argv[2], 0, 0) / nbsize;
	return 0;
}

static int
getdf(Raid *r, Rdev **rd, uvlong rbno)
{
	uvlong bno;

	if (getdf0(r, rd, &bno) < 0)
		return 0;
	if (rbno != bno) {
		dprint("getdf bno mismatch: r=%#p d=%#p rbno=%llud bno=%llud\n",
			r, *rd, rbno, bno);
		return 0;
	}
	return 1;
}

static int
dfcheck(Raid *r, Rdev *fd)
{
	Rdev *d, *e, *xd;

	switch (r->type) {
	default:
		return 1;
	case RTraid10:
	case RTraid1:
		if (DEVBAD(dmirror(fd)))
			return 1;
		break;
	case RTraid5:
		d = r->devs;
		e = d + r->ndevs;
		for (; d<e; d++)
			if (d != fd && DEVBAD(d))
				return 1;
		break;
	case RTraid6rs:
		xd = nil;
		d = r->devs;
		e = d + r->ndevs;
		for (; d<e; d++) {
			if (d != fd && DEVBAD(d)) {
				if (xd)
					return 1;
				xd = d;
			}
		}
	}
	return 0;
}

/* called with wlock(r) held */
static int
raidshield0(Raid *r, uvlong blk)
{
	int n;
	Lun *lb;
	Rdev *d, *e;

	lb = r->lblade;
	dprint("raidshield %d.%d %ulld: %s\n", lb->slot, r->rno, blk, up->errstr);
	if (r->flags & Rfailed)
		return 0;
	n = Bfailed;
	if (getdf(r, &d, blk) == 0) {
		dprint("raidshield cannot determine drive fault from errstr %q.  "
			"Attempting old style lookup\n", up->errstr);
		d = r->devs;
		e = d + r->ndevs;
		/* find problematic disk */
		if (!waserror()) {
			for (; d<e; d++)
				putbuf(getbuf(d, blk, Bread));
			poperror();
			/*
			 * This is not an error.  In order to handle errors for async I/O,
			 * parity initialization, and normal access, all must be able to
			 * call raidshield.  If things happen just right, more than one of
			 * these users will note the failure and call raidshield.  One of
			 * them will fix the block, and the other(s) will end up here.
			 * 
			 * We could change all I/O to go through the async procs and call
			 * them I/O procs to eliminate this case and simplify some other
			 * things.  This gets us back to the prior design, though, and I'm
			 * not sure if that's a good thing.  There's a benefit in
			 * permitting the lbworker to go grab the block it wants instead of
			 * putting the request on a list and waiting for an I/O proc to be
			 * available to do it for him.
			 */
			dprint("raidshield enacted for blk %lld on raid %d.%d, but no errors found\n",
				blk, lb->slot, r->rno);
			return 1;
		}
	}
	if (d->flags & Dfailed)		/* can't fix a failed disk */
		return 0;
	if (!isredundant(r)) {
		print("raidshield: i/o error on non-redundant drive %s (device %d.%d.%d) at block %llud\n",
			d->name, lb->slot, r->rno, d->dno, blk);
		return 0;
	}
	if (!dfcheck(r, d)) {
		if (!waserror()) {
			n = refreshbno(d, blk);
			poperror();
		} else
			n = Bfailed;
		switch (n) {
		case Brefreshed:	/* already fixed */
			return 1;
		case 0:			/* success */
			jskevent("msg:s:'raidshield: corrected error on drive %s (device %d.%d.%d)"
				 " at block %llud' sev:n:4 tag:s:SRX_DRIVE_IO_ERROR_CORRECTED"
				 " %S lun:n:%d raidpartpart:n:%d drivepart:n:%d block:n:%llud", d->name,
				 lb->slot, r->rno, d->dno, blk, d, lb->slot, r->rno, d->dno, blk); 
			return 1;
		}
		dprint("raidshield: refreshbno %ux\n", n);
	}
	jskevent("msg:s:'raidshield: unrecoverable%serror on drive %s (device %d.%d.%d) at block %llud'"
		 " sev:n:2 tag:s:SRX_DRIVE_IO_ERROR %S"
		 " lun:n:%d raidpart:n:%d drivepart:n:%d block:n:%llud", !n ? " revalidation " : " ",
		 d->name, lb->slot, r->rno, d->dno, blk,
		 d, lb->slot, r->rno, d->dno, blk); 
	faildev(d);
	setrflags(r);
	schedsave();
	return 0;
}

static int
raidshield(Raid *r, uvlong blk)
{
	int fixed;

	spinwlock(r);
	fixed = raidshield0(r, blk);
	spinwunlock(r);
	return fixed;
}

static void
nr_wr(Raid *r, uvlong rbno, void *db, ulong boff, ulong blen, ulong flags)
{
	Buf *bp;

	bp = getraid(r, rbno, flags, 0);
	if (waserror()) {
		putbuf(bp);
		nexterror();
	}
	memmove(bp->db+boff, db, blen);
	poperror();
	putbuf(bp);
}

static void
raid5_wr(Raid *r, uvlong rbno, void *db, ulong boff, ulong blen, ulong flags)
{
	Buf *bp, *pbp;
	void *pdb, *bdb;
	Rowlock *rl;

	bp = getraid(r, rbno, flags, 0);
	if (waserror()) {
		putbuf(bp);
		nexterror();
	}
	pbp = getraid(r, rbno, flags, 1);
	if (waserror()) {
		putbuf(pbp);
		nexterror();
	}
	rl = rowlock(r, bp->bno, blen, boff, OWRITE);
	if (waserror()) {
		rowunlock(rl);
		nexterror();
	}
	bdb = bp->db + boff;
	pdb = pbp->db + boff;
	xor(bdb, db, blen);
	xor(pdb, bdb, blen);
	memmove(bdb, db, blen);
	poperror();
	rowunlock(rl);
	poperror();
	putbuf(pbp);
	poperror();
	putbuf(bp);
}

static void
raid1x_wr(Raid *r, uvlong rbno, void *db, ulong boff, ulong blen, ulong flags)
{
	Buf *bp, *pbp;
	Rowlock *rl;

	bp = getraid(r, rbno, flags, 0);
	if (waserror()) {
		putbuf(bp);
		nexterror();
	}
	pbp = getraid(r, rbno, flags, 1);
	if (waserror()) {
		putbuf(pbp);
		nexterror();
	}
	rl = rowlock(r, bp->bno, blen, boff, OWRITE);
	if (waserror()) {
		rowunlock(rl);
		nexterror();
	}
	memmove(pbp->db+boff, db, blen);
	memmove(bp->db+boff, db, blen);
	poperror();
	rowunlock(rl);
	poperror();
	putbuf(pbp);
	poperror();
	putbuf(bp);
}

static Buf *
xgetbuf(Rdev *d, uvlong bno, ulong flags)
{
	if (DEVBAD(d))
		return recovered(d, bno, flags);
	return getbuf(d, bno, flags);
}

static uvlong
std2bno(uvlong std, uvlong rbno)
{
	uvlong bno;

	bno = std * (Nstride/nbsize);
	bno += rbno % (Nstride/nbsize);
	return bno;
}

static uvlong
bno2std(uvlong blk)
{
	return (blk*nbsize) / Nstride;
}

/*
 * here we must not return until we've at least called
 * putbuf on every buf in the row.  I'm not sure putbuf
 * can throw an error, but if it does we can't lose
 * refs to bufs.
 */
static void
putrowbufs(Buf **rowbufs, int nrb)
{
	Buf **ob, **b, **e;

	b = ob = rowbufs + up->pid%nrb;
	e = rowbufs + nrb;
	while (waserror())
		;
	do {
		if (++b >= e)
			b = rowbufs;
		if (*b)
			putbuf(*b);
	} while (b != ob);
	poperror();
}

static void
rsr6_wr(Raid *r, uvlong rbno, void *db, ulong boff, ulong blen, ulong flags)
{
	Rdev *p, *q, *dd, *d;
	void *qdb, *pdb, *ddb, *xdb;
	Buf *rowbufs[Nrdevs];
	uvlong row;
	int i, n;
	Rowlock *rl;

	raidxlate(r, rbno, &dd, &row, 0);
	raid6pq(r, row, &p, &q);
	pdb = ddb = qdb = nil;
	memset(rowbufs, 0, sizeof (Buf *) * r->ndevs);
/*1*/	if (waserror()) {
		putrowbufs(rowbufs, r->ndevs);
		nexterror();
	}
	/*
	 * Unlike pregetblkrow and putrowbufs acquisition here cannot use
	 * the up->pid lock distribution trick as hard acquisition of buffers
	 * in the row must be acquired in order to avoid deadlock on random
	 * i/o due to full cache lines.
	 */

	for (i = 0; i < r->ndevs; i++) {
		d = r->devs + i;
		rowbufs[i] = xgetbuf(d, row, (d == q || d == p || d == dd) ? flags : Bread);
		if (d == q)
			qdb = rowbufs[i]->db + boff;
		else if (d == dd)
			ddb = rowbufs[i]->db + boff;
		else if (d == p)
			pdb = rowbufs[i]->db + boff;
	}
	if (qdb == nil || ddb == nil || pdb == nil)
		error("what happened to PDQ?");

	/*
	 * OK, now we should have all the bufs for the row in hand.
	 * Lock the row and do the math.
	 */
	rl = rowlock(r, row, blen, boff, OWRITE);
/*2*/	if (waserror()) {
		rowunlock(rl);
		nexterror();
	}
	n = 0;
	/* calculate new Q */
	for (i = r->ndevs - 1; i >= 0; i--) {
		d = r->devs + i;
		if (d == q || d == p)
			continue;
		xdb = (d == dd) ? db : rowbufs[i]->db + boff;
		if (n)
			qmulxor(qdb, xdb, blen);
		else {
			memmove(qdb, xdb, blen);
			n++;
		}
	}

	/* update P and data */
	xor(ddb, db, blen);
	xor(pdb, ddb, blen);
	memmove(ddb, db, blen);

/*2*/	poperror();
	rowunlock(rl);
/*1*/	poperror();
	putrowbufs(rowbufs, r->ndevs);
}

static long
drawrw(Raid *r, char *db, long len, uvlong off, int mode)
{
	Rdev *d;
	uvlong n;
	Elv v;

	n = r->length;
	if (off + len > n)
		len = n - off;
	d = r->devs;
	elvinit(&v, d, db, len, off + r->doff, mode);
	elvinsert(&v);
	elvwait(&v);
	if (v.res != len)
		errorstr("%s:%p:%ulld", DEVF, d, off);
	if (mode == OWRITE)
		invalbufs(d, off, len);
	return len;
}

static long
dmirrorrw(Raid *r, char *db, long len, uvlong off, int mode)
{
	Rdev *d0, *d1;
	uvlong bno, xbno, doff, n;
	long bcnt, boff, toff, cnt;
	void *rls[2], **rl;
	Elv elv[4], *v, *ve;
	Elvgrp g;

	cnt = len;
	n = r->length;
	if (off + len > n)
		cnt = n - off;
loop:
	rl = rls;
	v = elv;
	ve = v + nelem(elv);
	memset(&g, 0, sizeof g);
	while (cnt > 0 && v < ve) {
		bno = off / nbsize;
		raidxlate(r, bno, &d0, &xbno, 0);
		boff = off % nbsize;		/* buffer offset */
		doff = nbsize*xbno + boff;	/* device offset */
		toff = doff % Nstride;		/* stride offset */
		bcnt = Nstride - toff;
		if (bcnt > cnt)
			bcnt = cnt;
		*rl++ = rowlock(r, xbno, bcnt, off, mode);
		switch (mode) {
		case OWRITE:
			n = DEVBAD(d0);
			if (n && n != Drepl) {
				invalbuf(d0, xbno);
				goto d1;
			}
			elvinit(v, d0, db, bcnt, doff + r->doff, mode);
			v->gen = xbno;
			elvgrpadd(&g, v);
			v++;
d1:
			d1 = dmirror(d0);
			n = DEVBAD(d1);
			if (n && n != Drepl) {
				invalbuf(d1, xbno);
				break;
			}
			elvinit(v, d1, db, bcnt, doff + r->doff, mode);
			v->gen = xbno;
			elvgrpadd(&g, v);
			v++;
			break;
		default:
			/*
			 * We interleave I/O based on the number of cache blocks
			 * specified by nbskip.  This impacts sequential I/O as most
			 * disks suffer a skip penalty; its use here is indicated for
			 * random workloads.
			 */
			if (DEVBAD(d0) || skipmirror(d0, xbno)) {
				d1 = dmirror(d0);
				if (!DEVBAD(d1))
					d0 = d1;
			}
			elvinit(v, d0, db, bcnt, doff + r->doff, mode);
			v->gen = xbno;
			elvgrpadd(&g, v);
			v++;
			break;
		}
		off += bcnt;
		db += bcnt;
		cnt -= bcnt;
	}
	ve = v;
	elvgrpinsert(&g);
	elvgrpwait(&g);
	if (mode == OWRITE)
		for (v = elv; v < ve; v++)
			invalbuf(v->d, v->gen);
	while (--rl >= rls)
		rowunlock(*rl);
	for (v = elv; v < ve; v++) {
		if (v->res != v->len)
			errorstr("%s:%p:%ulld", DEVF, v->d, v->off - r->doff);
	}
	if (cnt > 0)
		goto loop;
	return len - cnt;
}

static long
dstriperw(Raid *r, char *db, long len, uvlong off, int mode)
{
	Rdev *d;
	uvlong bno, xbno, doff, n;
	long bcnt, boff, toff, cnt;
	Elv elv[4], *v, *ve;
	Elvgrp g;

	cnt = len;
	n = r->length;
	if (off + len > n)
		cnt = n - off;
loop:
	v = elv;
	ve = elv + nelem(elv);
	memset(&g, 0, sizeof g);
	while (cnt > 0 && v < ve) {
		bno = off / nbsize;
		raidxlate(r, bno, &d, &xbno, 0);
		boff = off % nbsize;		/* buffer offset */
		doff = nbsize*xbno + boff;	/* device offset */
		toff = doff % Nstride;		/* stride offset */
		bcnt = Nstride - toff;
		if (bcnt >= cnt)
			bcnt = cnt;
		elvinit(v, d, db, bcnt, doff + r->doff, mode);
		v->gen = xbno;
		elvgrpadd(&g, v);
		v++;
		off += bcnt;
		db += bcnt;
		cnt -= bcnt;
	}
	ve = v;
	elvgrpinsert(&g);
	elvgrpwait(&g);
	for (v = elv; v < ve; v++) {
		if (v->res != v->len)
			errorstr("%s:%p:%ulld", DEVF, v->d, v->off - r->doff);
		if (mode == OWRITE)
			invalbuf(v->d, v->gen);
	}
	if (cnt > 0)
		goto loop;
	return len - cnt;
}

static int
realrw0(Raid *r, char *db, long len, uvlong off, int mode, int bflags)
{
	Buf *bp;
	uvlong n, std, bno, offarg;
	long bcnt, boff, cnt;
	void *rl;
	Rdev *d;
	int flags;

	offarg = off;
	flags = bflags|Bread;
	if (mode == OWRITE)
		flags |= Bmod;
	bno = 0;
	if (waserror()) {		/* L0 */
		if (strcmp(up->errstr, Etoolong) == 0)
			nexterror();
		dprint("realrw error: %s\n", up->errstr);
		if (getdf0(r, &d, &bno) < 0) {
			if (bno == 0)			/* direct i/o? */
				bno = off / nbsize;
			raidxlate(r, bno, &d, &bno, 0);	/* find the device and device bno (row) */
		}
		if (d->failfast == 0)
		if (raidshield(r, bno) == 0)
		if (isredundant(r) == 0)
			return 0;	
		nexterror();
	}
	spinrlock(r);
	if (r->flags & Rfailed) {
		poperror();		/* L0 */
		spinrunlock(r);
		return 0;
	}
	if (waserror()) {		/* L1 */
		spinrunlock(r);
		nexterror();
	}
	if (isdirect(r, mode)) {
		switch (r->type) {
		case RTnil:
		case RTfnil:
		case RTraw:
		case RTjbod:
		case RTraidl:
			len = drawrw(r, db, len, off, mode);
			break;
		case RTraid1:
		case RTraid10:
			len = dmirrorrw(r, db, len, off, mode);
			break;
		case RTraid0:
			len = dstriperw(r, db, len, off, mode);
			break;
		}
		poperror();		/* L1 */
		spinrunlock(r);
		poperror();		/* L0 */
		goto e;
	}
	cnt = len;
	n = r->length;
	if (off + cnt > n)
		cnt = n - off;
	while (cnt > 0) {
		bno = off / nbsize;
		if (raidpgp)
		if (mode == OWRITE && isredundant(r)) {
			if (r->type == RTraid6rs) {
				std = bno2std(bno);		/* raid offset in stride chunks */
				std = raid6row(r, std);		/* stride row (ie, stride disk offset) */
				pregetblkrow(r, std2bno(std, bno), 1);
			} else
				pregetraid(r, bno, 1);
		}
		boff = off % nbsize;
		bcnt = nbsize - boff;
		if (bcnt < cnt) {		/* block split - prefetch next block */
			if (raidpg)
				pregetraid(r, bno+1, 0);
		} else
			bcnt = cnt;

		switch (mode) {
		case OWRITE:
			switch (r->type) {
			default:
				nr_wr(r, bno, db, boff, bcnt, flags);
				break;
			case RTraid5:
				raid5_wr(r, bno, db, boff, bcnt, flags);
				break;
			case RTraid1:
			case RTraid10:
				raid1x_wr(r, bno, db, boff, bcnt, flags);
				break;
			case RTraid6rs:
				rsr6_wr(r, bno, db, boff, bcnt, flags);
				break;
			}
			break;
		default:
			bp = getraid(r, bno, flags, 0);
			if (waserror()) {		/* L2 */
				putbuf(bp);
				nexterror();
			}
			if (isredundant(r)) {
				rl = rowlock(r, bp->bno, bcnt, off, mode);
				memmove(db, bp->db+boff, bcnt);
				rowunlock(rl);
			} else
				memmove(db, bp->db+boff, bcnt);
			poperror();	/* L2 */
			putbuf(bp);
		}
		off += bcnt;
		db += bcnt;
		cnt -= bcnt;
	}
	poperror();			/* L1 */
	spinrunlock(r);
	poperror();			/* L0 */
	len -= cnt;
e:
	if (r->flags & Rclean)
	if (mode == OWRITE)
	if (offarg != 0 || r->lblade->ver != 0)
		soilraid(r);
	return len;
}

// returning anything != len causes an aoe ata error to be generated.
static int
realrw(Raid *r, char *db, long len, uvlong off, int mode, int bflags)
{
	int ret;

	while (waserror()) {
		if (strcmp(up->errstr, Etoolong) == 0) {
			*up->errstr = 0;
			continue;
		}
		nexterror();
	}
	ret = realrw0(r, db, len, off, mode, bflags);
	poperror();
	return ret;
}

#define SMASK ((1<<9) - 1)

static int
lrw(Lun *lb, void *db, long len, uvlong off, int mode, int bflags)
{
	Raid *r, *e;
	uvlong roff = 0, rend;
	long rcount, cnt;

	if ((off | len) & SMASK)
		error("offset and length must be a multiple of 512");
	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	switch (lb->raids[0].type) {
	case RTnil:
	case RTfnil:
		poperror();
		spinrunlock(lb);
		return len;
	}
	if (off+len > lb->length)
		len = lb->length - off;
	if (off > lb->length || len == 0) {
		poperror();
		spinrunlock(lb);
		return 0;
	}
	r = lb->raids;
	e = r + lb->nraids;
	rend =  r->length;
	while (rend < off) {
		roff = rend;
		if (++r >= e)
			panic("raid traversal error");
		rend = roff + r->length;
	}
	cnt = len;
	if (off + cnt > rend)
		cnt -= (off + cnt) - rend;
	/*
	 * Don't count reading lblade config (off==0) as using raid.  UGLY.  The
	 * config is stored in the first sizeof (Srlconfig) / 512 sectors
	 * and is read by any system on aoe query config read.  On linux,
	 * this happens once a minute, triggered by the periodic
	 * discovery timer.
	 */
	if (off)
		r->lastaccess = Ticks;
	rcount = realrw(r, db, cnt, off - roff, mode, bflags);
	if (rcount == cnt)
	if (cnt < len) {
		db = (uchar *) db + cnt;
		if (++r >= e)
			panic("raid traversal error");
		r->lastaccess = Ticks;
		rcount += realrw(r, db, len - cnt, 0, mode, bflags);
	}
	poperror();
	spinrunlock(lb);
	return rcount;
}

static void
fixupparity(Lun *l, uvlong lba)
{
	static Raid *lastr = nil;
	static uvlong lastbno = 0;
	Raid *r, *e;
	Rdev *d, *d2, *ed;
	uvlong roff = 0, rend;
	uvlong off, bno;
	int errs, sflags;

	r = l->raids;
	e = r + l->nraids;
	rend =  r->length;
	off = lba * 512;
	while (rend < off) {
		roff = rend;
		if (++r >= e)
			panic("raid traversal error");
		rend = roff + r->length;
	}
	if (r->type != RTraid5 && r->type != RTraid6rs)
		return;
	if (r->flags & (Rfailed|Rstop))
		return;
	raidxlate(r, off / nbsize, nil, &bno, 1);
	if (r != lastr || bno != lastbno) {
		spinwlock(r);
		errs = 0;
		sflags = r->flags;
		r->flags |= Riniting;
		while (waserror()) {
			spinwunlock(r);
			if (errs++ > 5 || (r->flags & Rfailed)) {
				r->flags = sflags;
				return;
			}
			raidshield(r, bno);
			spinwlock(r);
		}
		if (!(r->flags & Rdegraded))
			raidparity(r, bno);
		else {
			d = r->devs;
			ed = d + r->ndevs;
			for (; d < ed && !(d->flags & (Drepl|Dreplacing)); ++d) ;
			if (d < ed) {
				if (bno >= d->row)
					putbuf(recovered(d, bno, Bmod));
				for (d2 = d+1; d2 < ed && !(d2->flags & (Drepl|Dreplacing)); ++d2) ;
				if (d2 < ed) {
					if (bno >= d2->row)
						putbuf(recovered(d2, bno, Bmod));
				}
			}
		}
		lastr = r;
		lastbno = bno;
		r->flags = sflags;
		poperror();
		spinwunlock(r);
	}
}

enum {
	Nrpworker = 4,
};

typedef struct Replaywork Replaywork;
struct Replaywork {
	Lun *l;
	uchar *buf;
	int owner;
	uvlong lba;
	ulong mask;
	int dev;
	int typ;
} rpwork[Nrpworker];

static void
rpworker(void *a)
{
	Replaywork *rpw;
	int start, end;

	rpw = a;
	if (waserror()) {
		rpw->owner = -1;
		pexit("", 1);
	}
	while (1) {
		if (rpw->owner == 2) {
			poperror();
			rpw->owner = 0;
			pexit("", 1);
		}
		if (rpw->owner == 1) {
			fixupparity(rpw->l, rpw->lba);
			start = 0;
			while (rpw->mask) {
				for (; !(rpw->mask & 1); rpw->mask >>= 1, ++start) ;
				for (end = start; rpw->mask & 1; rpw->mask >>= 1, ++end) ;
				if (lrw(rpw->l, rpw->buf + (start << 9), (end - start) << 9,
					(rpw->lba + start) << 9, OWRITE, syncio) < 0) {
					nexterror();
				}
				start = end;
			}
			rpw->owner = 0;
		}
		yield();
	}
}

static void
ecreplay(int force)
{
	Lun *l;
	uchar *buf;
	uvlong lba;
	ulong mask;
	int i, n, dev, typ, fail;
	int spawn;

	spawn = 0;
	if (force == 0 && (!cachelist || cachelist[ecblk -1] == 0)) {
		spawn = 1;
		spinrlock(&lblades);
		for (l = lblades.head; l; l = l->next) {
			if (l->flags & Lcached0) {
				for (n = 0; n < l->nraids; ++n) {
					if (!(l->raids[n].flags & Rclean))
						break;
				}
				if (n < l->nraids)
					break;
			}
		}
		spinrunlock(&lblades);
		if (l == nil) {
			cacheparity(1);
			return;
		}
		print("shutdown not clean.\nRestoring CacheMotion data ... ");
	}
	if (cachelist && cachelist[ecblk-1]) {
		spawn = 1;
		print("restore forced\nRestoring CacheMotion data ... ");
	}
	spinrlock(&lblades);
	for (l = lblades.head; l; l = l->next) {
		if (l->flags & Lcached0)
			for (i = 0; i < l->nraids; ++i)
				l->raids[i].flags |= Rrecovering;
	}
	spinrunlock(&lblades);
	if (waserror()) {
		cacheparity(spawn);
		nexterror();
	}
	for (i = 0; i < Nrpworker; ++i) {
		rpwork[i].buf = malloc(ecblk);
		rpwork[i].owner = 0;
		kproc("rpworker", rpworker, &rpwork[i]);
	}
	fail = 0;
	ecreadopen(0);
	if (!(buf = malloc(ecblk)))
		error("error: malloc failure");
	if (waserror()) {
		free(buf);
		nexterror();
	}
	while ((n = ecrdback(0, &dev, &lba, buf, ecblk, &typ, &mask)) > 0) {
		spinrlock(&lblades);
		for (l = lblades.head; l && l->slot != dev; l = l->next) ;
		spinrunlock(&lblades);
		if (l == nil)
			errorstr("error: No lun %d", dev);
		if (!(l->flags & Lcached0))
			continue;
		while (1) {
			for (i = 0; i < Nrpworker && rpwork[i].owner != -1; ++i) ;
			if (i < Nrpworker) {
				fail = 1;
				goto bail;
			}
			for (i = 0; i < Nrpworker && rpwork[i].owner != 0; ++i) ;
			if (i < Nrpworker)
				break;
			yield();
		}
		rpwork[i].lba = lba;
		rpwork[i].l = l;
		rpwork[i].dev = dev;
		rpwork[i].typ = typ;
		rpwork[i].mask = mask;
		memmove(rpwork[i].buf, buf, ecblk);
		rpwork[i].owner = 1;
	}
bail:
	while (1) {
		for (i = 0; i < Nrpworker; ++i) {
			if (rpwork[i].owner == -1)
				fail = 1;
			else if (rpwork[i].owner != 0)
				break;
		}
		if (i >= Nrpworker)
			break;
		yield();
	}
	for (i = 0; i < Nrpworker; ++i) {
		free(rpwork[i].buf);
		rpwork[i].owner = 2;
	}
	poperror();
	free(buf);
	poperror();
	cacheparity(spawn);
	if (fail)
		error("write failure in replay");
	if (n < 0)
		nexterror();
	if (cachelist && cachelist[ecblk-1]) {
		cachelist[ecblk-1] = 0;
		ecwrite(0, 0, cachelist, ecblk, ETdirect);
	}
	sync();
}

/*
 * Sometimes we need direct access to the disks in the LUN.  This
 * function will perform best-effort reads/writes to each Rdev in each
 * Raid.  This is used primarily with newer metadata layouts.
 */
static int
drw(Lun *lb, void *db, long len, uvlong off, int mode)
{
	Raid *r, *re;
	Rdev *d, *de;
	int n, savefailed;

	if ((off | len) & SMASK)
		error("offset and length must be a multiple of 512");
	n = 0;
	spinrlock(lb);
	if (waserror()) {
		spinrunlock(lb);
		nexterror();
	}
	r = lb->raids;
	re = r + lb->nraids;
	for (; r < re; ++r) {
		savefailed = 0;
		spinrlock(r);
		if (waserror()) {
			spinrunlock(r);
			nexterror();
		}
		d = r->devs;
		de = d + r->ndevs;
		for (; d < de; ++d) {
			if (d->savefailed == Nsavefailed)
				continue;
			if (d->c == nil)
				continue;
			if (!waserror()) {
				if (mode == OREAD)
					n = devtab[d->c->type]->read(d->c, db, len, off);
				else /* OWRITE */
					n = devtab[d->c->type]->write(d->c, db, len, off);
				poperror();
				if (n < len) {
					print("warning: short %s on drive %s lun %d.%d.%d\n",
					    readwrite(mode), d->name, lb->slot, r->rno, d->dno);
					continue;
				}
				if (mode == OREAD) {
					poperror();
					spinrunlock(r);
					goto done;
				}
			} else {
				if (d->savefailed == 0)
					print("warning: %s error on drive %s lun %d.%d.%d\n",
					    readwrite(mode), d->name, lb->slot, r->rno, d->dno);
				setfailfast(d);
				d->savefailed++;
				savefailed++;
			}
		}
		poperror();
		spinrunlock(r);
		if (savefailed)
			fastfaildevices(r);
	}
done:
	poperror();
	spinrunlock(lb);
	return n;
}

/* CSS begin aoe support */

#ifdef notdef
TBI
  static ushort ident[256] = {
+ 	[2] 0xc837,	/* identify all valid; no set feat required */
  	[47] 0x8000,
  	[49] 0x0200,
  	[50] 0x4000,
- 	[83] 0x5400,
- 	[84] 0x4000,
- 	[86] 0x1400,
- 	[87] 0x4000,
+ 	[80] 0x01f0,	/* ata version 8 */
+ 	[82] 0x4000,	/* nop */
+ 	[83] 0x7400,	/* supported: 1<<14 | flush cache/ext, 48bit */
+ 	[84] 0x4000,	/* supported: */
+ 	[85] 0x4000,	/* supported: nop */
+ 	[86] 0xb400,	/* enabled: flush cache/ext, 48bit */
+ 	[87] 0x4000,	/* should set 0x100: wwn */
  	[93] 0x400b,
+ 	[119] 0x4000,
+ 	[120] 0x4000,
  };
#endif

static ushort ident[256] = {
	[47] 0x8000,
	[49] 0x0200,
	[50] 0x4000,
	[83] 0x5400,
	[84] 0x4000,
	[86] 0x1400,
	[87] 0x4000,
	[93] 0x400b,
};

static void
setfld(ushort *a, int idx, int len, char *str)	/* set field in ident */
{
	uchar *p;

	p = (uchar *)(a+idx);
	while (len > 0) {
		if (*str == 0)
			p[1] = ' ';
		else
			p[1] = *str++;
		if (*str == 0)
			p[0] = ' ';
		else
			p[0] = *str++;
		p += 2;
		len -= 2;
	}
}

/* ident fields are little endian */
static void
setlba28(ushort *sp, uvlong lba)
{
	uchar *p = (uchar *) (sp+60);
	int i;

	for (i=0; i<4; i++)
		*p++ = lba >> i*8;
}

static void
setlba48(ushort *sp, uvlong lba)
{
	uchar *p = (uchar *) (sp+100);
	int i;

	for (i=0; i<8; i++)
		*p++ = lba >> i*8;
}

static uvlong
getlba(Aoeata *ah)
{
	uchar *p;
	int i;
	uvlong u;

	p = ah->lba;
	u = 0;
	for (i = 0; i < 6; i++)
		u |= (uvlong)(*p++) << (i*8);
	return u;
}

static void
putlba(Aoeata *ah, uvlong lba)
{
	int i;

	for (i=0; i<6; i++)
		ah->lba[i] = lba >> (i*8);
}

static int
iohash(int targ, uvlong lba)
{
	return (targ * lba) % Niosvchash;
}

static int
iostart(Block *bp)
{
	int n, h, targ;
	uvlong start, end;
	uvlong emin, smax;
	Iosvc *p;
	Aoeata *ah;
	Blkaux *baux, *xbaux;
	Lun *lb;

	baux = BLK2AUX(bp);
	ah = (Aoeata *)bp->rp;
	if (ah->cmd != 0)
		return 0;
	n = 0;
	start = getlba(ah);
	end = start + ah->scnt;
	targ = (nhgets(ah->major) << 8) | ah->minor;

	switch (ah->cmdstat) {
	case 0x30:	/* write */
	case 0x34:
		n = ah->scnt << 9;
	case 0x20:	/* read */
	case 0x24:
		break;
	case 0xe7:	/* flush cache */
	case 0xea:
		start = 0;
		end = 0;
		break;
	default:
		return 0;
	}
	lb = baux->lb;
	h = iohash(targ, start);
	ilock(&lb->iosvclock);
	if (waserror()) {
		iunlock(&lb->iosvclock);
		return -1;
	}
	for (p = lb->iosvchash[h]; p; p = p->next) {
		if (targ != p->targ)
			continue;
		if (start == p->start)
		if (end == p->end)
		if (ah->cmdstat == p->ah->cmdstat)
		if (memcmp(ah+1, p->ah+1, n) == 0) {
			/*
			 * This is an exact match, which means it's probably
			 * a retransmit.  Update the msg we'll use to respond
			 * so that the dst and tag are set to this latter message.
			 *
			 * note this also means we *must* iofini before we
			 * rewrite the dst,src header for response.
			 */
			memmove(p->ah->tag, ah->tag, sizeof ah->tag);
			memmove(p->ah->src, ah->src, sizeof ah->src);
			xbaux = BLK2AUX(p->bp);
			xbaux->net = baux->net;
			ainc(&lb->castats[LCasiodup].stat);
			nexterror();
		}
		/*
		 * ok, not an exact match but check for overlap since
		 * that's probably bad, too.
		 */
		emin = MIN(end, p->end);
		smax = MAX(start, p->start);
		if (smax < emin) {
//			dprint("LUN %d overlap i/o s0=%lld s1=%lld e0=%lld e1=%lld\n",
//				lb->slot, p->start, start, p->end, end);
			ainc(&lb->castats[LCasoverlap].stat);
		}
	}
	p = lb->iosvcfree;
	if (p == nil)
		p = mallocz(sizeof *p, 1);
	lb->iosvcfree = p->next;
	p->next = lb->iosvchash[h];
	lb->iosvchash[h] = p;
	p->bp = bp;
	p->ah = ah;
	p->targ = targ;
	p->start = start;
	p->end = end;
	p->hash = h;
	baux->svc = p;
	poperror();
	iunlock(&lb->iosvclock);
	return 0;
}

static void
iofini(Block *bp)
{
	Iosvc *p, *rp, **pp;
	Blkaux *baux;
	Lun *lb;

	baux = BLK2AUX(bp);
	rp = baux->svc;
	if (rp == nil)
		return;
	lb = baux->lb;
	baux->svc = nil;
	ilock(&lb->iosvclock);
	pp = &lb->iosvchash[rp->hash];
	for (; p = *pp; pp = &p->next)
		if (p == rp) {
			*pp = p->next;
			p->next = nil;
			break;
		}
	if (p == nil)
		dprint("iofini: error: command already completed\n");
	else {
		rp->next = lb->iosvcfree;
		lb->iosvcfree = rp;
	}
	iunlock(&lb->iosvclock);
}

/*
 * We assume here that lblades is already locked.
 */
static long
fcbackio(int dev, void *a, long count, vlong offset, int, int mode)
{
	Lun *l;

	for (l = lblades.head; l && l->slot != dev; l = l->next) ;
	if (!l) {
		snprint(up->errstr, ERRMAX, "error: No LUN %d", dev);
		return -1;
	}
	return lrw(l, a, count, offset, mode, syncio);
}

static int
isseqio(Lun *l, uvlong lba, Raid *r, uvlong bigbno)
{
	Rdev *d;
	uvlong dbno;
	int i;

	if (bigbno < lookback)
		return 0;
	for (i=1; i<=lookback; i++) {
		raidxlate(r, bigbno-i, &d, &dbno, 0);
		if (dcpeek(d, dbno) != Sfound)
		if (ecage(1, l->slot, lba, -i) > 1000*i)
		if (ecage(2, l->slot, lba, -i) > 1000*i)
			return 0;
	}
	return 1;
}

static int
cachedlrw(Lun *l, void *a, int len, uvlong rlba, int mode)
{
	Chan *ch;
	Raid *r, *e;
	Rdev *d;
	uvlong roff, rend;
	uvlong lba, dbno, bigbno;
	int n;

	lba = rlba + l->soff / 512;
	if (mode == OWRITE) {
		n = lrw(l, a, len, lba << 9, mode, syncio);
		if (n == len && (l->flags & Lcached0) && ecattached[0]) {
			if (ecwrite(l->slot, lba, a, len, ETdata) != len) {
				if (ecattached[0]) {
					ecattached[0] = 0;
					print("cache failure: switching to synchronous operation\n");
					ecpoison(0);
					ecclose(0);
					syncio = Bimm;
					if (waserror()) {
						print("failed to set wrcache off\n");
						nexterror();
					}
					ch = namec("#‡/ctl", Aopen, OWRITE, 0);
					devtab[ch->type]->write(ch, "wrcache off", 11, 0);
					cclose(ch);
					sync();
					poperror();
				}
			}
		}
		if ((l->flags & Lcached1) && ecattached[1]) {
			ecwritel(1, l->slot, lba, a, len, ETdata);		/* to invalidate it */
			ecwritel(2, l->slot, lba, a, len, ETdata);		/* to invalidate it */
		}
	}
	else if ((l->flags & Lcached1) && ecattached[1]) {
		roff = 0;
		r = l->raids;
		e = r + l->nraids;
		rend =  r->length;
		while (rend < (lba << 9)) {
			roff = rend;
			if (++r >= e)
				panic("raid traversal error");
			rend = roff + r->length;
		}
		bigbno = (lba * 512 - roff) / nbsize;
		raidxlate(r, bigbno, &d, &dbno, 0);
		if (dcpeek(d, dbno) == Snotfound) {
			if (isseqio(l, lba, r, bigbno)) {
				ecseqio++;
				n = lrw(l, a, len, lba << 9, mode, syncio);
			} else {
				ecrandio++;
				n = ecreadl(1, l->slot, lba, a, len, ETdata);
				if (n == -1)
					n = lrw(l, a, len, lba << 9, mode, syncio);
			}
		} else
			n = lrw(l, a, len, lba << 9, mode, syncio);
	}
	else
		n = lrw(l, a, len, lba << 9, mode, syncio);
	return n;
}

static int
atacmd(Lun *lb, Ataregs *r, void *db, ulong dlen)
{
	uvlong lba, size, n, tod, tode;
	int mode, len;
	ushort *ip;
	Iofilter *f;
	enum Atabits {
		IDNF= 1<<4, ABRT= 1<<2,				/* err bits */
		BSY= 1<<7, DRDY= 1<<6, DF= 1<<5, ERR= 1<<0,	/* status bits */
	};

	size = lb->length / 512;
	lba = r->lba + lb->soff / 512;
	switch (r->cmd) {
	default:
		r->status = DRDY | ERR;
		r->err = ABRT;
		return 0;
	case 0xe7:		/* flush cache */
	case 0xea:
		if (lb->flags & Lflushcache)
			lsync(lb);
		r->err = 0;
		r->status = DRDY;
		return 0;
	case 0xec:		/* identify device */
		if (islbfailed(lb)) {
			r->err = ABRT;
			r->status = DRDY | DF | ERR;
			return 0;
		}
		ip = (ushort *) db;
		memmove(ip, ident, sizeof ident);
		setfld(ip, 27, 40, lb->model);
		setfld(ip, 23, 8, lb->fwver);
		setfld(ip, 10, 20, lb->serial);
		idputgeometry(lb, ip);
		n = size -= lb->soff / 512;
		if (n & ~Lba28max)
			n &= ~Lba28max;
		n = n ? Lba28max : size;
		setlba28(ip, n);
		setlba48(ip, size);
		r->err = 0;
		r->status = DRDY;
		return 512;
	case 0x20:		/* read sectors */
	case 0x30:		/* write sectors */
		lba &= Lba28max;
		break;
	case 0x24:		/* read sectors ext */
	case 0x34:		/* write sectors ext */
		lba &= Lba48max;
		break;
	}
	if (lba + r->sectors > size) {
		r->err = IDNF;
		r->status = DRDY | ERR;
		return 0;
	}
	if (r->cmd == 0x20 || r->cmd == 0x24) {
		mode = OREAD;
		f = &lb->rfilt;
	} else {
		mode = OWRITE;
		f = &lb->wfilt;
	}
	len = r->sectors << 9;
	if (len > dlen)
		error("atacmd: request too large");

	tod = MACHP(0)->ticks;	
	if (lb->flags & (Lcached0 | Lcached1))
		n = cachedlrw(lb, db, len, r->lba, mode);
	else
		n = lrw(lb, db, len, lba << 9, mode, syncio);
	if (n != len) {
		r->err = ABRT;
		r->status = DF | ERR;
		r->lba += (len-n)/512;
		return 0;
	}
	tode = MACHP(0)->ticks;
	tod = (tode - tod) * 1000;
	incfilter(f, n, tod);

	r->lba += r->sectors;
	r->sectors = 0;
	r->status = DRDY;
	r->err = 0;
	return (mode == OREAD) ? len : 0;
}

static void
loadlun(Lun *lb, int force)
{
	Srlconfig *s;
	long n;

	if (!cansavelun(lb))
		error(Ebadraid);
	s = smalloc(sizeof *s);
	if (waserror()) {
		free(s);
		nexterror();
	}
	n = 0;
	if (lb->ver == 1)
		n = drw(lb, s, sizeof *s, 64*512, OREAD);
	else if (lb->ver == 0)
		n = lrw(lb, s, sizeof *s, 0, OREAD, syncio);
	if (n < sizeof *s)
		print("loadlun: short read from lun %d\n", lb->slot);
	if (force ||	/* puppy dogs and rainbows are assumed */
	    lb->ver == 1 || lb->ver == 0) {
		n = nhgetl(s->length);
		if (n > sizeof s->config)
			n = sizeof s->config;
		if (n < 0) {
			print("loadlun: read %ld bytes from lun %d\n", n, lb->slot);
			n = sizeof s->config;
		}
		lb->nconfig = n;
		memmove(lb->config, s->config, lb->nconfig);
		setserial(lb, s->serial, Nserialsz);
		setlunlabel(lb, s->lunlabel, Nlunlabelsz); 
		lb->vlan = nhgets(s->vlan);
		lb->flags = s->flags;
		lb->fcpri = s->fcpri;
		lb->fcminpct = s->fcminpct;
		if (lb->vlan > Nvlanmax) {
			print("loadlun: invalid vlan %.3ux; ignored\n", lb->vlan);
			lb->vlan = 0xfff;
		}
	} else
		error(Ebadluncfg);
	poperror();
	free(s);
}

static void
savelun(Lun *lb)
{
	Srlconfig *s;
	int n;

	if (!cansavelun(lb))
		return;
	s = smalloc(sizeof *s);
	memset(s, 0, sizeof *s);
	memmove(s->magic, lb->magic, Nmagic);
	hnputl(s->length, lb->nconfig);
	memmove(s->config, lb->config, lb->nconfig);
	memmove(s->serial, lb->serial, Nserialsz);
	memmove(s->lunlabel, lb->lunlabel, Nlunlabelsz);
	hnputs(s->vlan, lb->vlan);
	s->flags = lb->flags;
	s->fcpri = lb->fcpri;
	s->fcminpct = lb->fcminpct;
	n = 0;
	if (lb->ver == 1) {
		if (!waserror()) {
			n = drw(lb, s, sizeof *s, 64*512, OWRITE);
			poperror();
		}
	} else if (lb->ver == 0) {
		if (!waserror()) {
			n = lrw(lb, s, sizeof *s, 0, OWRITE, Bimm);
			poperror();
		}
	}
	if (n < sizeof *s)
		print("savelun: short write to lun %d\n", lb->slot);
	free(s);
}

#define QCMD(x) ((x)->verccmd & 0xf)
#define BUSE(b) ((b)->lim - (b)->rp)

static Block *
aoeqc(Lun *lb, Block *bp)
{
	Aoeqc *qh;
	int reqlen, len;
	uchar curcfg[Nconflen];
	uchar *cfg, *p;

	qh = (Aoeqc *) bp->rp;
	reqlen = nhgets(qh->cslen);
	cfg = (uchar *) (qh+1);
	if (QCMD(qh) != AQCread)
	if (reqlen > Nconflen)
		return nil;		/* if you can't play nice ... */
	if (BUSE(bp) - sizeof *qh < Nconflen) {
		dprint("aoeqc: unable to fit my config into request buf!\n");
		return nil;
	}
	len = lb->nconfig;
	memmove(curcfg, lb->config, lb->nconfig);
	switch (QCMD(qh)) {
	case AQCtest:
		if (reqlen != len)
			return nil;
		/* fall thru */
	case AQCprefix:
		if (reqlen > len)
			return nil;
		if (memcmp(curcfg, cfg, reqlen))
			return nil;
		/* fall thru */
	case AQCread:
		break;
	case AQCset:
		if (len)
		/* fall thru */
	case AQCtar:
		if (len != reqlen || memcmp(curcfg, cfg, reqlen)) {
			qh->verflags |= AFerr;
			qh->error = AEcfg;
			break;
		}
		if (QCMD(qh) == AQCtar) {
			p = cfg + reqlen;
			reqlen = nhgets(p);
			if (reqlen > Nconflen)
				return nil;
			p += sizeof(short);
			cfg = p;
		}
		/* fall thru */
	case AQCfset:
		memmove(lb->config, cfg, reqlen);
		lb->nconfig = reqlen;
		schedsave();
		bcastqc(lb);
		len = reqlen;
		memmove(curcfg, cfg, len);
		break;
	default:
		qh->verflags |= AFerr;
		qh->error = AEarg;
	}
	memmove(qh+1, curcfg, len);
	hnputs(qh->cslen, len);
	if (nbufcnt)
		hnputs(qh->bufcnt, nbufcnt);
	else
		hnputs(qh->bufcnt, lb->bufcnt);
	qh->scnt = BLK2AUX(bp)->net->maxscnt;
	hnputs(qh->fwver, FWV);
	qh->verccmd = QCMD(qh) | (Aoever<<4);
	bp->wp = bp->rp + sizeof *qh + len;
	return bp;
}

static Block *
fnilio(Block *bp)
{
	Aoeata *ah;
	Net *np;
	Chan *dc;
	Blkaux *baux;

	baux = BLK2AUX(bp);
	np = baux->net;
	ah = (Aoeata *) bp->rp;
	if (ah->scnt > np->maxscnt) {
		ah->verflags |= AFerr;
		ah->error = AEarg;
		goto rsp;
	}
	if (ah->cmd != ACata)
		return bp;
	switch (ah->cmdstat) {
	case 0x20:	/* read */
	case 0x24:
		bp->wp = bp->rp + sizeof *ah + 512*ah->scnt;
		break;
	case 0x30:	/* write */
	case 0x34:
		bp->wp = bp->rp + sizeof *ah;
		break;
	default:
		return bp;
	}
	ah->scnt = 0;
	ah->errfeat = 0;
	ah->cmdstat = 1<<6;	/* DRDY */
rsp:	memmove(ah->dst, ah->src, 6);
	memmove(ah->src, np->ea, 6);
	ah->verflags |= AFrsp;
	if (BLEN(bp) < ETHERMINTU)
		bp->wp = bp->rp + ETHERMINTU;
	dc = np->dc;
	baux->net = nil;
	bp->free = baux->freefn;
	devtab[dc->type]->bwrite(dc, bp, 0);
	return nil;
}

static Block *
aoeata(Lun *lb, Block *bp)
{
	int n;
	Ataregs r;
	Aoeata *ah;

	ah = (Aoeata *) (bp->rp);
	if (islbfailed(lb)) {
		ah->verflags |= AFerr;
		ah->error = AEdev;
		return bp;
	}
	if (ah->scnt > BLK2AUX(bp)->net->maxscnt) {
		ah->verflags |= AFerr;
		ah->error = AEarg;
		return bp;
	}
	if (ah->cmdstat == 0x30 || ah->cmdstat == 0x34)
		if (ah->scnt * Aoesectsz > BLEN(bp) - sizeof (Aoeata)) {
			ah->verflags |= AFerr;
			ah->error = AEarg;
			return bp;
		}
	if (ah->cmdstat != 0xec)
	if (!rrok(lb, ah->src, (ah->aflags & AAFwrite) ? Awrite : Aread)) {
		ah->error = AEres;
		ah->verflags |= AFerr;
		return bp;
	}
	r.lba = getlba(ah);
	r.sectors = ah->scnt;
	r.feature = ah->errfeat;
	r.cmd = ah->cmdstat;
	n = atacmd(lb, &r, ah+1, BUSE(bp) - sizeof *ah);
	ah->scnt = r.sectors;
	ah->errfeat = r.err;
	ah->cmdstat = r.status;
	putlba(ah, r.lba);
	bp->wp = bp->rp + sizeof *ah + n;
	return bp;
}

static int
rrok(Lun *lb, uchar *m, int type)
{
	int i = lb->nsrr;

	if (i == 0 || (lb->anyread && type == Aread))
		return 1;
	while (--i >= 0)
		if (memcmp(m, &lb->srr[i*6], 6) == 0)
			return 1;
	return 0;
}

static Block *
aoeresrel(Lun *lb, Block *bp)
{
	Aoesrr *sh;
	uchar *m;
	int i, n;
	enum { Naoesrr= 26, };
	Chan *c;
	char *p, *e;

	sh = (Aoesrr *) bp->rp;
	m = bp->rp + Naoesrr;
	if (sh->rcmd == 1 || sh->rcmd == 2) { /* set or force command */
		n = sh->nmacs * 6;
		if (bp->wp < m + n)
			goto err;
		if (sh->nmacs == lb->nsrr) /* check for duplicate requests */
		if (memcmp(m, lb->srr, n) == 0)
			goto done;
		qlock(&rrfs);
		if (waserror()) {
			qunlock(&rrfs);
			dprint("aoeresrel: %s\n", up->errstr);
			goto err;
		}
		if (sh->rcmd == 1) /* set request, check for access */
			if (!rrok(lb, sh->src, Ares)) { /* Set the error and send current reservation */
				poperror();
				qunlock(&rrfs);
				sh->error = AEres;
				sh->verflags |= AFerr;
				goto done;
			}
		c = rrfsopen(lb->slot, "ctl", OWRITE);
		if (waserror()) {
			cclose(c);
			nexterror();
		}
		if (devtab[c->type]->write(c, "reset", 5, 0) != 5)
			error("reset write failure");
		if (sh->nmacs > 0) {
			p = rrfs.buf;
			e = p + sizeof rrfs.buf;
			p = seprint(p, e, "register %.08H", Konekey);
			for (i = 0; i < sh->nmacs; i++)
				p = seprint(p, e, " %E", m + i*6);
			n = p - rrfs.buf;
			if (devtab[c->type]->write(c, rrfs.buf, n, 0) != n)
				error("register write failure");
			n = snprint(rrfs.buf, sizeof rrfs.buf, "reserve 0 %.08H", Konekey);
			if (devtab[c->type]->write(c, rrfs.buf, n, 0) != n)
				error("reserve write failure");
		}
		poperror();
		cclose(c);
		krloadrr0(lb);
		poperror();
		qunlock(&rrfs);
	} else if (sh->rcmd == 0) { /* read request */
		/* Nothing to do */
	} else {
err:		sh->error = AEarg;
		sh->verflags |= AFerr;
		return bp;
	}
done:
	sh->nmacs = lb->nsrr;
	n = lb->nsrr * 6;
	memmove(m, lb->srr, n);
	bp->wp = bp->rp + Naoesrr + n;
	return bp;
}


static int
krreset(Lun *lb)
{
	Chan *c;

	qlock(&rrfs);
	if (waserror()) {
		qunlock(&rrfs);
		dprint("krreset: %s\n", up->errstr);
		return -1;
	}
	c = rrfsopen(lb->slot, "ctl", OWRITE);
	if (waserror()) {
		cclose(c);
		nexterror();
	}
	if (devtab[c->type]->write(c, "reset", 5, 0) != 5)
		error("write failure");
	poperror();
	cclose(c);
	poperror();
	qunlock(&rrfs);
	return 0;
}
	
static int
krreg(Lun *lb, Kreg *krh)
{
	Chan *c;
	char *p, *e;
	uchar *m;
	int i, n, nm;

	qlock(&rrfs);
	if (waserror()) {
		qunlock(&rrfs);
		dprint("krreg: %s\n", up->errstr);
		return -1;
	}
	c = rrfsopen(lb->slot, "ctl", OWRITE);
	if (waserror()) {
		cclose(c);
		nexterror();
	}
	nm = krh->nmacs;
	p = rrfs.buf;
	e = p + sizeof rrfs.buf;
	p = seprint(p, e, "register %.016ullx", nhgetv(krh->key));
	m = krh->macs;
	for (i = 0; i < nm; i++)
		p = seprint(p, e, " %E", m + i*6);
	n = p - rrfs.buf;
	if (devtab[c->type]->write(c, rrfs.buf, n, 0) != n)
		error("write failure");
	poperror();
	cclose(c);
	poperror();
	qunlock(&rrfs);
	return 0;
}

static int
krset(Lun *lb, Kset *ksh)
{
	Chan *c;
	char buf[32];
	int n, rv;

	qlock(&rrfs);
	if (waserror()) {
		qunlock(&rrfs);
		dprint("krset: %s\n", up->errstr);
		return -1;
	}
	c = rrfsopen(lb->slot, "ctl", OWRITE);
	if (waserror()) {
		cclose(c);
		nexterror();
	}
	rv = 0;
	n = snprint(buf, sizeof buf, "reserve %d %.016ullx", ksh->rtype, nhgetv(ksh->key));
	if (devtab[c->type]->write(c, buf, n, 0) != n) {
		print("krset devtab write failure: %s\n", up->errstr);
		rv = -1;
	}
	poperror();
	cclose(c);
	poperror();
	qunlock(&rrfs);
	return rv;
}

static int
krreplace(Lun *lb, Kreplace *krh)
{
	Chan *c;
	char buf[128];
	int n, rv;

	qlock(&rrfs);
	if (waserror()) {
		qunlock(&rrfs);
		dprint("krreplace: %s\n", up->errstr);
		return -1;
	}
	c = rrfsopen(lb->slot, "ctl", OWRITE);
	if (waserror()) {
		cclose(c);
		nexterror();
	}
	rv = 0;
	if (krh->rflags & KRnopreempt)
		n = snprint(buf, sizeof buf, "replace %d %.016ullx %.016ullx",
			krh->rtype, nhgetv(krh->targkey), nhgetv(krh->replkey));
	else
		n = snprint(buf, sizeof buf, "preempt %d %.016ullx %.016ullx",
			krh->rtype, nhgetv(krh->targkey), nhgetv(krh->replkey));
	if (devtab[c->type]->write(c, buf, n, 0) != n) {
		print("krreplace devtab write failure: %s\n", up->errstr);
		rv = -1;
	}
	poperror();
	cclose(c);
	poperror();
	qunlock(&rrfs);
	return rv;
}

static int
krstat(Lun *lb, Block *bp)
{
	Chan *c;
	Kresp *krh;
	int i, n, nk;
	uchar *p;

	qlock(&rrfs);
	if (waserror()) {
		qunlock(&rrfs);
		dprint("krstat: %s\n", up->errstr);
		return -1;
	}
	c = rrfsopen(lb->slot, "stat", OREAD);
	if (waserror()) {
		cclose(c);
		nexterror();
	}
	n = devtab[c->type]->read(c, rrfs.buf, sizeof rrfs.buf - 1, 0);
	if (n <= 0)
		error("error reading krstat file");
	rrfs.buf[n] = '\0';
	n = tokenize(rrfs.buf, rrfs.arg, nelem(rrfs.arg));
	if (n < 3)
		error("error parsing krstat result");
	i = 0;
	krh = (Kresp *) bp->rp;
	krh->rtype = strtoul(rrfs.arg[i++], 0, 10);
	hnputl(krh->gencnt, strtoul(rrfs.arg[i++], 0, 10));
	hnputv(krh->owner, strtoull(rrfs.arg[i++], 0, 16));
	nk = 0;
	p = krh->keys;
	for (; i < n; i++, nk++) {
		hnputv(p, strtoull(rrfs.arg[i], 0, 16));
		p += 8;
	}
	hnputs(krh->nkeys, nk);
	memset(krh->res, 0, sizeof krh->res);
	bp->wp = p;
	poperror();
	cclose(c);
	poperror();
	qunlock(&rrfs);
	return 0;
}

static int
krloadrr0(Lun *lb)
{
	Chan *c;
	int i, j, n;

	if (waserror()) {
		dprint("krloadrr: %s\n", up->errstr);
		return -1;
	}
	c = rrfsopen(lb->slot, "macs", OREAD);
	if (waserror()) {
		cclose(c);
		nexterror();
	}
	rrfs.buf[0] = 0;
	n = devtab[c->type]->read(c, rrfs.buf, sizeof rrfs.buf - 1, 0);
	if (n <= 0) {	/* no reservation */
		lb->nsrr = 0;
		lb->anyread = 0;
		goto done;
	}
	rrfs.buf[n] = '\0';
	n = tokenize(rrfs.buf, rrfs.arg, nelem(rrfs.arg));
	if (n < 2)
		error("bad parsing krloadrr file");
	lb->anyread = atoi(rrfs.arg[0]);
	lb->nsrr = 0;
	for (j = 0, i = 1; i < n; i++, j++) {
		if (parseether(&lb->srr[j*6], rrfs.arg[i]) < 0) {
			print("warning: error parsing rrfs ether %s\n", rrfs.arg[i]);
			memset(&lb->srr[j*6], 0, 6);
		}
		lb->nsrr++;
	}
done:	poperror();
	cclose(c);
	poperror();
	return 0;
}

static int
krloadrr(Lun *lb)
{
	int n;

	qlock(&rrfs);
	n = krloadrr0(lb);
	qunlock(&rrfs);
	return n;
}

static Block *
aoekresrel(Lun *lb, Block *bp)
{
	Aoekrr *kh;
	int rv;
	enum { Nkresp = 24+24 };

	kh = (Aoekrr *)bp->rp;
	switch (kh->rcmd) {
	default:
		kh->error = AEarg;
		kh->verflags |= AFerr;
		return bp;
	case AKstat:
		goto resstat;
	case AKreset:
		rv = krreset(lb);
		break;
	case AKreg:
		rv = krreg(lb, (Kreg *)kh);
		break;
	case AKset:
		if (rrok(lb, kh->src, Ares) == 0)
			goto reserr;
		rv = krset(lb, (Kset *)kh);
		break;
	case AKreplace:
		rv = krreplace(lb, (Kreplace *)kh);
		break;
	}
	if (rv < 0) {
reserr:		kh->error = AEres;
		kh->verflags |= AFerr;
		return bp;
	}
	krloadrr(lb);
resstat:
	if (krstat(lb, bp) < 0)
		error("krstat failed - this shouldn't happen");
	return bp;
}

static Block *
aoemask(Lun *lb, Block *bp)
{
	Aoemask *mh;
	Mdir *md, *mdi, *mde;
	int i;

	mh = (Aoemask *) (bp->rp);
	if (mh->rid != 0)
		return nil;
	md = mdi = (Mdir *) (mh+1);
	switch (mh->cmd) {
	case Medit:
		mde = md + mh->nmacs;
		for (; md<mde; md++) {
			switch (md->cmd) {
			case MDdel:
				rmmac(lb, md->mac);
				continue;
			case MDadd:
				if (addmac(lb, md->mac) == 0) {
					mh->merror = MEfull;
					mh->nmacs = md - mdi;
					schedsave();
					return bp;
				}
			case MDnop:
				continue;
			default:
				mh->merror = MEbaddir;
				mh->nmacs = md - mdi;
				schedsave();
				return bp;
			}
		}
		schedsave();
		/* success.  fall thru to return list */
	case Mread:
		md = mdi;
		spinrlock(lb);
		for (i=0; i<lb->nmacs; i++) {
			md->res = md->cmd = 0;
			memmove(md->mac, &lb->macs[i*6], 6);
			md++;
		}
		mh->merror = 0;
		mh->nmacs = lb->nmacs;
		bp->wp = bp->rp + sizeof *mh + sizeof *md * lb->nmacs;
		spinrunlock(lb);
		break;
	default:
		mh->verflags |= AFerr;
		mh->error = AEarg;
	}
	return bp;
}

static void
doaoe(Lun *lb, Block *bp)
{
	Block *rbp;
	Aoehdr *h;
	Net *np;
	Blkaux *baux;

	if (waserror()) {
		freeb(bp);
		nexterror();
	}
again:
	h = (Aoehdr *) bp->rp;
	switch (h->cmd) {
	case ACata:
		rbp = aoeata(lb, bp);
		break;
	case ACconfig:
		rbp = aoeqc(lb, bp);
		break;
	case ACEmask:
	case ACmask:
		rbp = aoemask(lb, bp);
		break;
	case ACEredirect:
		bp->rp += sizeof *h;
		if (bp->rp >= bp->wp)
			error("block rp exceeded wp");
		goto again;
	case ACresrel:
		rbp = aoeresrel(lb, bp);
		break;
	case ACkresrel:
		rbp = aoekresrel(lb, bp);
		break;
	default:
		rbp = bp;
		bp->wp = bp->rp + ETHERMINTU;
		h->verflags |= AFerr;
		h->error = AEcmd;
		break;
	}
	poperror();
	if (rbp == nil) {
		freeb(bp);
		return;
	}
	baux = BLK2AUX(bp);
	np = baux->net;
	bp->free = baux->freefn;
	iofini(bp);
	memmove(h->dst, h->src, 6);
	memmove(h->src, np->ea, 6);
	/*
	 * Optionally set the major, minor to allow redirect to specify
	 * an alternate response shelf, slot.
	 */
	if (nhgets(h->major) == 0xffff)
		hnputs(h->major, shelf);
	if (h->minor == 0xff)
		h->minor = lb->slot;
	h->verflags |= AFrsp;
	if (BLEN(bp) < ETHERMINTU)
		bp->wp = bp->rp + ETHERMINTU;
	if (replacevlan(bp) == -1) {
		freeb(bp);
		return;
	}
	if (rspdelayms)
		tsleep(&up->sleep, return0, 0, rspdelayms);
	mpscqput(np->outq, bp);
}

static int
maskok(Lun *lb, uchar *ea)
{
	uchar *p, *e;
	int ok;

	if (!lb->nmacs)
		return 1;
	spinrlock(lb);
	p = lb->macs;
	e = p + lb->nmacs*6;
	ok = 0;
	if (!waserror()) {
		for (; !ok && p<e; p+=6)
			ok = memcmp(p, ea, 6) == 0;
		poperror();
	}
	spinrunlock(lb);
	return ok;
}

#define BPTOH(bp) ((Aoehdr *) (bp)->rp)
#define BCAST(minor) ((minor) == 0xff)

/* aoeinput is responsible for freeing bp if it cannot be consumed */
static void
aoeinput(Block *bp)
{
	Block *obp, *nbp;
	int m;
	Lun *lb;
	Blkaux *baux;

	m = BPTOH(bp)->minor;
	obp = BCAST(m) ? bp : nil;	/* (obp == broadcast) */
	baux = BLK2AUX(bp);
	spinrlock(&lblades);
	if (waserror()) {
		spinrunlock(&lblades);
		if (bp)
			freeb(bp);
		nexterror();
	}
	for (lb=lblades.head; lb && bp; lb=lb->next) {
		/* ignore vlan priority bits */
		if (lb->vlan != (bp->vlan & 0xfff))
			continue;
		if (!obp && lb->slot != m)
			continue;
		if (!maskok(lb, BPTOH(bp)->src) || lb->line != Lon)
			continue;
		baux->lb = lb;
		if (obp != nil) {
			nbp = copyblock(bp, BUSE(bp));
			setbaux(nbp, baux->net, lb);
		} else
			nbp = bp;
		bp = obp;
		if (lb->raids[0].type == RTfnil) {
			nbp = fnilio(nbp);
			if (nbp == nil)
				continue;
		}
		if (iostart(nbp) < 0) {
			freeb(nbp);
			continue;
		}
		if (lbiopass(lb, nbp) < 0) {
			iofini(nbp);
			freeb(nbp);
		}
	}
	poperror();
	spinrunlock(&lblades);
	if (bp)
		freeb(bp);
}

static void
lbworker(void *vp)
{
	Lun *lb;
	Block *bp;
	long *llbw;

	lb = vp;
	ainc(&lbwactive);
	while (waserror())
		dprint("lbworker: %s\n", up->errstr);
	for (;;) {
		/*
		 * labassert permits us to catch mismanaged waserror/poperror
		 * in the i/o path.
		 */
		labassert(1);
		bp = lbnextio(lb);
		if (bp == nil)
			break;
		llbw = &lb->castats[LCaslbw].stat;
		ainc(llbw);
		if (waserror()) {
			adec(llbw);
			nexterror();
		}
		doaoe(lb, bp);
		poperror();
		adec(llbw);
	}
	poperror();
	adec(&lb->nworker);
	adec(&lbwactive);
	putlun(lb);
	pexit("", 1);
}

/* CSS network serving support */

static int
creadonce(char *path, void *db, int n)
{
	Chan *c;

	c = nil;
	if (waserror()) {
		if (c != nil)
			cclose(c);
		return -1;
	}
	c = namec(path, Aopen, OREAD, 0);
	n = devtab[c->type]->read(c, db, n, 0);
	poperror();
	cclose(c);
	return n;
}

/* called by stopether and stopallether, np must be pulled from nets list */
static void
closeether(Net *np)
{
	int i;
	char *name;
	enum {
		Nms	= 100,
		Nwarn	= 5*1000 / Nms,
		Newait	= 30*1000 / Nms,
	};

	name = netname(np);

	/* kick netwriter */
	mpscqput(np->outq, allocb(0));

	/* close input and kick netreader */
	devtab[np->cc->type]->write(np->cc, "iqclose", 7, 0);
	mpscqput(np->inq, allocb(0));	

	for (i=0; np->ref != 1 && i < Newait; i++) {
		if (i && (i % Nwarn) == 0)
			print("dropether: %s ref %ld; waiting\n", name, np->ref);
		tsleep(&up->sleep, return0, 0, Nms);
	}
	if (i == Newait) {
		print("dropether: %s ref %ld; continuing without waiting\n",
			name, np->ref);
		/*
		 * Returning here leaves the net in state SNclosing and
		 * it cannot be reused regardless if the netreader/netwriter
		 * eventually return.
		 */
		return;
	} else if (i >= Nwarn)
		print("dropether: %s release success\n", name);

	/* final cleanup */
	mpscqflush(np->inq, nextfreeb);
	mpscqflush(np->outq, nextfreeb);
	
	np->state = SNfree;
}

static void
stopallether(void)
{
	Net *n;

	for (;;) {
		wlock(&nets);
		for (n = nets.head; n; n = n->next) {
			if (n->state == SNactive) {
				n->state = SNclosing;
				break;
			}
		}
		wunlock(&nets);
		if (n == nil)
			return;
		closeether(n);
	}
}

static void
getnet(Net *np)
{
	ainc(&np->ref);
}

static void
putnet(Net *np)
{
	int n;

	n = adec(&np->ref);
	if (n < 0)
		panic("negative ref count on net %s\n", np->path);
	if (n != 0)
		return;

	/* As this shouldn't happen, log it.  panic? */
	dprint("putnet: closing net %s\n", np->path);

	if (np->dc != nil)
		cclose(np->dc);
	if (np->cc != nil)
		cclose(np->cc);
	free(np);
}

static int
cansave(Raid *r)
{
	switch (r->type) {
	case RTnil:
	case RTfnil:
	case RTraw:
		return 0;
	}
	return 1;
}

static int
cansavelun(Lun *lb)
{
	Raid *r, *re;

	spinrlock(lb);
	r = lb->raids;
	re = r + lb->nraids;
	for (; r < re; ++r)
		if (!cansave(r)) {
			spinrunlock(lb);
			return 0;
		}
	spinrunlock(lb);
	return 1;
}

static char *
readwrite(int mode)
{
	if (mode == OREAD)
		return "read";
	else /* OWRITE */
		return "write";
}

static void
rmnl(char *s)
{
	for (;;) {
		switch (*s) {
		case '\r':
		case '\n':
			*s = '\0';
			/* fall thru */
		case '\0':
			return;
		}
		s++;
	}
}

static void
stopether(char *ether)
{
	Net *n, **nn;
	char *p;

	wlock(&nets);
	if (waserror()) {
		wunlock(&nets);
		nexterror();
	}
	for (nn = &nets.head; n = *nn; nn = &n->next) {
		p = strstr(n->path, ether);
		if (p && strcmp(p, ether) == 0) {
			if (n->state != SNactive)
				errorstr("ether %s not active", ether);
			n->state = SNclosing;
			break;
		}
	}
	poperror();
	wunlock(&nets);
	if (n == nil)
		errorstr("ether %s not found", ether);
	closeether(n);
}

static Net *
newnet(char *ether)
{
	int n;
	enum { Nbuf= 64, };
	char buf[Nbuf];
	char *args[3];
	Net *np;

	np = malloc(sizeof *np);
	if (np == nil)
		error("allocation failure");
	if (waserror()) {
		if (np->dc) cclose(np->dc);
		if (np->cc) cclose(np->cc);
		np->dc = np->cc = nil;
		free(np);
		nexterror();
	}
	snprint(np->path, nelem(np->path), "%s%s",
		ether[0] == '/' ? "" : "/net/", ether);

	/* get addr */
	snprint(buf, Nbuf, "%s/addr", np->path);
	n = creadonce(buf, buf, Nbuf-1);
	if (n < 0)
		errorstr("error reading addr for %s", np->path);
	buf[n] = '\0';
	if (parseether(np->ea, buf) < 0)
		error(Ebadmac);

	/* get mtu */
	snprint(buf, Nbuf, "%s/mtu", np->path);
	n = creadonce(buf, buf, Nbuf-1);
	if (n < 0)
		errorstr("error reading mtu for %s", np->path);
	buf[n] = '\0';
	n = tokenize(buf, args, nelem(args));
	if (n < 3)
		errorstr("malformed mtu for %s", np->path);
	n = strtoul(args[2], 0, 0);
	if (n <= ETHERMAXTU)
		n = ETHERMAXTU;
	np->maxscnt = (n - sizeof (Aoeata)) / 512;
	if (np->maxscnt > Nmaxscnt)
		np->maxscnt = Nmaxscnt;

	np->inq = mpscqopen();
	if (np->inq == nil)
		errorstr("failure opening mpsc inq for %s", np->path);
	np->outq = mpscqopen();
	if (np->outq == nil)
		errorstr("failure opening mpsc outq for %s", np->path);

	snprint(buf, Nbuf, "%s!0x88a2", np->path);
	np->dc = chandial(buf, nil, nil, &np->cc);
	if (np->dc == nil || np->cc == nil)
		errorstr("chandial failure opening %s", buf);

	/* enable 802.1Q vlan processing on this ethertype - blech */
	devtab[np->cc->type]->write(np->cc, "vlan", 4, 0);

	netifbypass(etherc2e(np->dc), np->dc, netinput, np->inq);

	np->state = SNiniting;

	poperror();
	return np;
}

static void
startether(char *ether)
{
	Net *np;
	char *p;

	wlock(&nets);
	if (waserror()) {
		wunlock(&nets);
		nexterror();
	}
	for (np = nets.head; np; np = np->next) {
		p = strstr(np->path, ether);
		if (p && strcmp(p, ether) == 0) {
			if (np->state != SNfree)
				errorstr("ether %s in use", ether);
			np->state = SNiniting;
			break;
		}
	}
	if (np == nil) {
		np = newnet(ether);	/* succeeds or throws error */
		getnet(np);
		np->next = nets.head;
		nets.head = np;
	} else {
		/* flush reader/writer queues */
		mpscqflush(np->inq, nextfreeb);
		mpscqflush(np->outq, nextfreeb);

		/* reopen input queue */
		devtab[np->cc->type]->write(np->cc, "iqreopen", 8, 0);
	}

	poperror();
	wunlock(&nets);

	getnet(np);	/* for netreader */
	snprint(up->genbuf, sizeof up->genbuf, "netread_%s", ether);
	kproc(up->genbuf, netreader, np);
	getnet(np);	/* for netwriter */
	snprint(up->genbuf, sizeof up->genbuf, "netwrite_%s", ether);
	kproc(up->genbuf, netwriter, np);

	np->state = SNactive;
}

/*
 * If this is an 802.1Q vlan trunk frame move Dest and Src MAC four bytes into the frame
 * and bump bp->rp. This will allow the rest of the SRX code to ignore vlan offsets.
 */
static void
stripvlan(Block *bp)
{
	ushort type;
	Etherpkt *pkt;

	pkt = (Etherpkt*)bp->rp;
	type = pkt->type[0]<<8 | pkt->type[1];
	bp->vlan = 0;
	if (type == 0x8100) {
		bp->vlan = nhgets(bp->rp + 14);
		memmove(bp->rp + 4, bp->rp, 12);
		bp->rp += 4;	
	}
}

static void
srfreeb(Block *bp)
{
	iofini(bp);
	bp->free = BLK2AUX(bp)->freefn;
	freeb(bp);
}

#define aoever(h) (h->verflags >> 4)

static char *
netname(Net *np)
{
	char *p;

	p = strrchr(np->path, '/');
	if (p == nil)
		return "unknown";
	return ++p;
}

static void
setbaux(Block *bp, Net *np, Lun *lb)
{
	Blkaux *baux;

	baux = BLK2AUX(bp);
	baux->svc = nil;
	baux->net = np;
	baux->lb = lb;
	baux->freefn = bp->free;
	bp->free = srfreeb;
}

static void
netwriter(void *vp)
{
	Net *np;
	Block *bp;
	Chan *dc;

	np = vp;
	dc = np->dc;
	while (waserror())
		dprint("netwriter %s: %r\n", netname(np));
	for (;;) {
		if (np->state == SNiniting) {
			yield();
			continue;
		} else if (np->state != SNactive)
			break;
		bp = (Block*)((uintptr)mpscqget(np->outq) - offsetof(Block, next));
		if (BLEN(bp) == 0) {
			dprint("netwriter %s: nil read\n", netname(np));
			freeb(bp);
			break;
		}
		if (!waserror()) {
			devtab[dc->type]->bwrite(dc, bp, 0);
			poperror();
		}
	}
	poperror();
	putnet(np);
	pexit("", 1);
}

static void
nextfreeb(Next *p)
{
	freeb((Block*)((uintptr)p - offsetof(Block, next)));
}

static void
netinput(void *vp, Block *bp)
{
	XQueue *q;

	q = vp;
	mpscqput(q, bp);
}

/* enters with reference to np already set */
static void
netreader(void *vp)
{
	Net *np;
	Block *bp;
	ushort sh;
	Aoehdr *h;

	np = vp;
	while (waserror())
		dprint("netreader %s: %r\n", netname(np));
	for (;;) {
		if (np->state == SNiniting) {
			yield();
			continue;
		} else if (np->state != SNactive)
			break;
		bp = (Block*)((uintptr)mpscqget(np->inq) - offsetof(Block, next));
		if (BLEN(bp) == 0) {
			dprint("netreader %s: nil read\n", netname(np));
			freeb(bp);
			break;
		}
		stripvlan(bp);
		h = BPTOH(bp);
		sh = nhgets(h->major);
		if (shelf != Nshelfunset)
		if (!(h->verflags & AFrsp))
		if (aoever(h) == 1)
		if (sh == shelf || sh == 0xffff)
		if (memcmp(h->src, bcastmac, 6)) {
			setbaux(bp, np, nil);
			aoeinput(bp);
			continue;
		}
		freeb(bp);
	}
	poperror();
	putnet(np);
	pexit("", 1);
}

/* CSS begin dstate file support */

enum {
	Ndsline= ERRMAX,
	Ndssz= 128 * Ndsline,
};

static struct {
	char buf[Ndssz];
	char *rp;
	char *wp;
	Lock;
	QLock;
	Rendez;
} dsdata;

static int
dstateready(void *)
{
	return *dsdata.rp;
}

static long
dstateread(void *db, long len)
{
	char *p, *buf;

	buf = smalloc(Ndsline);
	qlock(&dsdata);
	if (waserror()) {
		qunlock(&dsdata);
		free(buf);
		nexterror();
	}
	for (;;) {
		lock(&dsdata);
		p = dsdata.rp;
		if (*p)
			break;
		unlock(&dsdata);
		sleep(&dsdata, dstateready, nil);
	}
	if (len > *p)
		len = *p;
	memmove(buf, p+1, len);
	*p = 0;
	dsdata.rp = p += Ndsline;
	if (p >= dsdata.buf + sizeof dsdata.buf)
		dsdata.rp = dsdata.buf;
	unlock(&dsdata);
	memmove(db, buf, len);
	poperror();
	free(buf);
	qunlock(&dsdata);
	return len;
}

static int
dstatelog(char *fmt, ...)
{
	int dragrp, n;
	va_list arg;
	char *p;

	lock(&dsdata);
	p = dsdata.wp;
	dragrp = *p++;
	va_start(arg, fmt);
	n = vsnprint(p, Ndsline-1, fmt, arg);
	va_end(arg);
	*--p = n;
	p = dsdata.wp += Ndsline;
	if (p >= dsdata.buf + sizeof dsdata.buf)
		p = dsdata.wp = dsdata.buf;
	if (dragrp)
		dsdata.rp = p;
	unlock(&dsdata);
	wakeup(&dsdata);
	return n;
}

static void
dstateinit(void)
{
	dsdata.wp = dsdata.rp = dsdata.buf;
}

static void
dstatechg(Rdev *d)
{
	char *s;
	int i;

	for(i = 0; i < Dcount; i++)
		if(d->flags & 1<<i)
			goto ok;
//	return;
	i = 0;
ok:
	s = ledactions[i];
	if(s == nil)
		return;
	if((ledenabled & 1<<i) == 0)
		s = "reset";
	dstatelog("%s %s\n", d->path, s);
}

/* CSS disk save  */

static char *
rt2s(Rtype type)
{
	switch (type) {
	case RTnil:
		return "nil";
	case RTfnil:
		return "fnil";
	case RTraw:
		return "raw";
	case RTjbod:
		return "jbod";
	case RTraidl:
		return "raidL";
	case RTraid0:
		return "raid0";
	case RTraid1:
		return "raid1";
	case RTraid5:
		return "raid5";
	case RTraid6rs:
		return "raid6rs";
	case RTraid10:
		return "raid10";
	default:
		return "unknown";
	}
}

static char *
dstate(Rdev *d, int save)
{
	if (d->flags & Dfailed)
		return "failed";
	if (d->flags & Drepl)
		return "replaced";
	if (save && (d->flags & Dclean)) {
		if (d->raid->flags & Rneedinit)
			return "needinit";
		else
			return "clean";
	}
	return "normal";
}

static void
fastfaildevices(Raid *r)
{
	Rdev *d, *de;

	spinwlock(r);
	d = r->devs;
	de = d + r->ndevs;
	for (; d<de; d++) {
		if (d->failfast) {
			faildev(d);
			setrflags(r);
		}
	}
	spinwunlock(r);
}

static void
save(void)
{
	Lun *lb;
	Raid *r, *re;
	Rdev *d, *de;
	char *p, *e;
	Srconfig *s;
	uvlong nrows;
	ulong ts;
	int savefailed;
	int i;

	s = mallocz(sizeof *s, 1);
	if (!s)
		error("save malloc error");
	ts = seconds();
	p = s->config;
	e = p + Nconflen - 1;
	spinrlock(&lblades);
	lb = lblades.head;
	for (; lb; lb=lb->next) {
		spinrlock(lb);
		r = lb->raids;
		re = r + lb->nraids;
		memmove(s->magic, lb->magic, Nmagic);
		for (; r<re; r++) {
			if (!cansave(r))
				continue;
			savefailed = 0;
			spinrlock(r);
			nrows = r->mindlen / nbsize;
			d = r->devs;
			de = d + r->ndevs;
			for (; d<de; d++) {
				if (d->savefailed == Nsavefailed)
					continue;
				if (d->c == nil)
					continue;
				p = s->config;
				p = seprint(p, e, "%d %d.%d.%d %d %s %s %ulld %lud",
					shelf, lb->slot, r->rno, d->dno,
					r->ndevs, rt2s(r->type), dstate(d, 1), nrows - r->roff, ts);
				for (i=0; i<lb->nmacs; i++)
					p = seprint(p, e, " %E", &lb->macs[i*6]);
				hnputs(s->length, p - s->config);
				if (!waserror()) {
					devtab[d->c->type]->write(d->c, s, sizeof *s, 0);
					poperror();
 				} else {
					if (d->savefailed == 0)
						jskevent( "msg:s:'warning: config save error for drive %s (device %d.%d.%d); failing device.'"
							" sev:n:2 tag:s:SRX_DRIVE_CONFIG_SAVE_ERROR"
							" %S lun:n:%d raidpart:n:%d drivepart:n:%d",
							d->name, lb->slot, r->rno, d->dno, d, lb->slot, r->rno, d->dno);
 					setfailfast(d);
					d->savefailed++;
 					savefailed++;
 				}
			}
			spinrunlock(r);
 			if (savefailed)
 				fastfaildevices(r);
		}
		spinrunlock(lb);
		savelun(lb);
	}
	spinrunlock(&lblades);

	qlock(&spares);
	d = spares.devs;
	de = d + spares.ndevs;
	memmove(s->magic, Srmagic01, Nmagic);	/* default to latest */
	for (; d<de; d++) {
		p = seprint(s->config, e, "%d spare", shelf);
		hnputs(s->length, p - s->config);
		if (!waserror()) {
			devtab[d->c->type]->write(d->c, s, sizeof *s, 0);
			poperror();
		} else
			print("warning: config save error for spare %s\n", d->name);
	}
	qunlock(&spares);
	qlock(&caches);
	d = caches.devs;
	de = d + caches.ndevs;
	memmove(s->magic, Srmagic01, Nmagic);	/* default to latest */
	for (; d<de; d++) {
		p = seprint(s->config, e, "%d cache", shelf);
		hnputs(s->length, p - s->config);
		if (!waserror()) {
			devtab[d->c->type]->write(d->c, s, sizeof *s, 0);
			poperror();
		} else
			print("warning: config save error for cache %s\n", d->name);
	}
	qunlock(&caches);
	free(s);
}

/* called holding rrfs qlock.  function returns a valid chan or throws an error */
static Chan *
rrfsopen(int lun, char *file, int omode)
{
	char *warr[2];
	char buf[16];
	Chan *c;

	if (rrfs.rootc == nil)
		error("rrfs not initialized");
	snprint(buf, sizeof buf, "%d", lun);
	warr[0] = buf;
	warr[1] = file;
	c = cclone(rrfs.rootc);
	if (waserror()) {
		cclose(c);
		nexterror();
	}
	if (walk(&c, warr, 2, 1, nil) < 0)
		errorstr("walk %s/%s", warr[0], warr[1]);
	c = devtab[c->type]->open(c, omode);
	if (c == nil)
		error("nil open");
	poperror();
	return c;
}

Dev srdevtab = {
	L'√',		/* alt-s-r */
	"sr",

	devreset,
	devinit,
	devshutdown,
	srattach,
	srwalk,
	srstat,
	sropen,
	devcreate,
	srclose,
	srread,
	devreadv,
	devbread,
	srwrite,
	devwritev,
	devbwrite,
	devremove,
	devwstat,
	devpower,
	devconfig,
};
