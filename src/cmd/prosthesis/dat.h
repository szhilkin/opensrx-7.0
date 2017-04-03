enum { Nfid = 100, Nreq = 40, Nproc=40 };

typedef struct 	Fid 	Fid;		/* file ids for 9p */
typedef struct 	Req	Req;		/* 9p request and response */
typedef struct 	Dirtab	Dirtab;
typedef	struct	Proc	Proc;
typedef	struct	Cmd	Cmd;

struct	Fid
{
	int 	used;
	ulong	fid;
	int	path;
	int	open;
	uchar *data;
	int len;
};

struct	Req
{
	int	busy;		/* Req busy with request */
	ulong	pid;		/* Pid of slave process executing the transaction */
	int	flushtag;	/* Tag on which to reply to flush */
	int	canint;
	Fid	*f;		/* fid */
	Fcall	t;		/* T msg */
	Fcall	r;		/* R msg */
	uchar	b[8192+IOHDRSZ];
};

struct	Dirtab
{
	char	*name;
	int	perm;
	vlong	length;
	void	(*read)(Req *);
	void	(*write)(Req *);
	void	(*clunk)(Req *);
	char	*opscrpt;
	char	*rdscrpt;
	char	*wrscrpt;
	int	nopen;		/* nuber of times file is open */
};

struct	Proc
{
	Proc	*next;
	int	pid;
	int	busy;
};

struct	Cmd
{
	int	code;
	char	*name;
};

#define DEBUG		if(!dbg){}else fprint

void	publish(void);
void	srv(void);
void	respond(Req *, char *);
void	devdir(Qid, char *, vlong, char *, long, Dir *);
void	doflush(Req*);
void	doclunk(Req*);
void	dowalk(Req*);
void	doread(Req*);
void	dowrite(Req*);
void	doopen(Req*);
void	doattach(Req *);
void	dostat(Req *);
int	newf(int, int);
Fid*	findf(int);
int	clunkf(int);
void	mkqid(Qid *, vlong, ulong, int);
int	dirwalk(char *);
void	fatal(char*);
void	doscript(Req *);
void	blockingslave(void);
void	flushaction(void *, char *);
void	slaveread(Req *);
void	slavewrite(Req *);
void	slaveopen(Req *);
int	cmdcode(char *, Cmd *);
char	*shell(char *);

int	fd;		/* we server 9p requests from this fd */
int	chatty;
char	*srvname;
char	*mtpt;
Fid	ftab[Nfid];
int	nfids;
Dirtab	dir[];		/* defined in handler file */
char	*user;
char	*system;
ulong	starttime;
Req	*workq;		/* work being done */
Proc	*proclist;	/* processes out there helping */
int	ndir;		/* number of dir entries */
int	dbg;
