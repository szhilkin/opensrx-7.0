
#define SS2TARG(sh, sl) ((sh)<<16 | (sl))
#define TARG2SH(targ) ((targ)>>16)
#define TARG2SL(targ) ((targ) & 0xff)

enum {
//	Ndevs	= 512,
	Ndevs	= 4096,
	Nconf	= 1024,
	Niomax	= 8*1024,

	Nmodel	= 16,
	Nserial	= 20,
	Nfw	= 8,
};

typedef struct Eth Eth;
typedef struct Aoepath Aoepath;
typedef struct Aoedev Aoedev;

struct Eth {
	int	fd;
	char	*name;
	uchar	ea[6];
	ulong 	iomax;
};

struct Aoepath {
	Eth	*eth;
	uchar	dst[6];
};

struct Aoedev {
	uchar	ident[512];
	char	model[Nmodel+1];
	char	serial[Nserial+1];
	char	firmware[Nfw+1];
	uvlong	length;
	ulong	targ;		/* sh << 16 | sl */
	uchar	conf[Nconf];
	int	nconf;
	int	wnd;		/* max outstanding command window */
	int	iomax;		/* max data per ata command */
	Aoepath;		/* later, an array */
};

#pragma varargck type "T" ulong
#pragma varargck type "S" uvlong

int aoeinit(int, char **);
void aoediscover(void);
long aoeread(Aoedev *, void *, long, uvlong);
long aoewrite(Aoedev *, void *, long, uvlong);
long aoeio(Aoedev *, void *, long, uvlong, int);
Aoedev *aoetarg2dev(ulong);
void aoestop(void);

Aoedev	devs[Ndevs];
int	ndevs;
int	aoetrace;
int	aoerexmit;
