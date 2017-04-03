#include <u.h>
#include <libc.h>
#include <fcall.h>
#include "dat.h"

enum {
	CMhalt,
	CMreboot,
	CMupdate,

	Nhaltsecs =	3*1000,		/* halt/reboot delay for response */
};

void	dread(Req*);
void	nowrite(Req *);
void	rdctl(Req *);
void	wrctl(Req *);
void	rdsos(Req *);
void	rmsos(Req *);
void	rduptime(Req *);
/*
 * The dir is a list of pointers to functions that carry out
 * IO for the given files.  The qid.path is the index into the
 * table.  The first entry must be the directory entry.
 */

Dirtab	dir[] = {
	{ "/",		0777, 		0, dread, nowrite },
	{ "ctl",	0222, 		0, rdctl, wrctl },
	{ "shelf", 	0666,		0, nil, nil, nil, nil, "pshelf", "pshelf" },
	{ "remove",	0222,		0, nil, nil, nil, nil, nil, "premove" },
	{ "eject",	0222,		0, nil, nil, nil, nil, nil, "peject" },
	{ "sos",	0444,		0, rdsos, nil, rmsos, "sos", nil, nil },
	{ "uptime",	0444,		0, rduptime, nil, nil, nil, nil, nil },
};

Cmd	cmds[] = {
	{ CMhalt, 	"halt" },
	{ CMreboot, 	"reboot" },
	{ CMupdate, 	"update" },
	{ 0, nil },
};

int	ndir = nelem(dir);
int	updatefd = -1;			/* file descriptor for update file */

void
doread(Req *rq)
{
	Fid *f;
	Dirtab *d;

	f = findf(rq->t.fid);
	if (f == nil) {
		respond(rq, "unknown fid");
		return;
	}
	if (f->open == -1) {
		respond(rq, "not open");
		return;
	}
	rq->f = f;
	d = dir + f->path;
	if (d->read == nil) {
		if (d->rdscrpt == nil) {
			respond(rq, "bad read");
			return;
		}
		if (rq->t.offset != 0) {
			rq->r.count = 0;
			respond(rq, nil);
			return;
		}
		doscript(rq);
		return;
	}
	d->read(rq);
}

void
dowrite(Req *rq)
{
	Fid *f;
	Dirtab *d;
	
	f = findf(rq->t.fid);
	if (f == nil) {
		respond(rq, "unknown fid");
		return;
	}
	if (f->open == -1) {
		respond(rq, "not open");
		return;
	}
	rq->f = f;
	d = dir + f->path;
	if (d->write == nil) {
		if (d->wrscrpt == nil) {
			respond(rq, "bad write");
			return;
		}
		if (rq->t.offset != 0) {
			rq->r.count = 0;
			respond(rq, nil);
			return;
		}
		doscript(rq);
		return;
	}
	d->write(rq);
}

void
doopen(Req *rq)
{
	Fid *f;
	Dirtab *d;
	
	f = findf(rq->t.fid);
	if (f == nil)
		respond(rq, "no fid");
	rq->f = f;
	rq->r.qid.type = QTFILE;
	if (f->path == 0)
		rq->r.qid.type = QTDIR;
	rq->r.qid.path = f->path;
	rq->r.qid.vers = 0;
	rq->r.iounit = 512;
	f->open = rq->t.mode;

	d = dir + f->path;
	if (d->opscrpt) {
		doscript(rq);
		return;
	}
	respond(rq, nil);
}

/*
 * here we fork a child to fork another child to run a script
 * and wait for it all to complete.  We can get interrupted if
 * a Tflush if sent to the srver.
 */
 
void
doscript(Req *rq)
{
	int r;
	Proc *p;
	uintptr pid;
	static int nproc;

	for(;;) {
		for(p = proclist; p; p = p->next) {
			if(p->busy == 0) {
				rq->pid = p->pid;
				p->busy = 1;
				pid = (ulong)rendezvous((void*)p->pid, rq);
				if (pid != p->pid)
					fatal("rendezvous sync fail");
				return;
			}	
		}

		if(++nproc > Nproc)
			fatal("too many procs");

		r = rfork(RFPROC|RFMEM);
		if (r < 0)
			fatal("rfork");

		if (r == 0)
			blockingslave();

		p = malloc(sizeof(Proc));
		if (p == nil)
			fatal("out of memory");

		p->busy = 0;
		p->pid = r;
		p->next = proclist;
		proclist = p;
		rendezvous((void*)p->pid, p);
	}
}

void
blockingslave(void)
{
	Proc *m;
	int pid;
	Req *rq;

	notify(flushaction);
	pid = getpid();
	m = rendezvous((void*)pid, 0);
	for(;;) {
		rq = rendezvous((void*)pid, (void*)pid);
		if (rq == (void*)~0)			/* Interrupted */
			continue;
		DEBUG(2, "\tslave: %d %F b %d p %uld\n", pid, &rq->t, rq->busy, rq->pid);
		if(rq->flushtag != NOTAG)
			return;
		switch(rq->t.type) {
		case Topen:
			slaveopen(rq);
			break;
		case Tread:
			slaveread(rq);
			break;
		case Twrite:
			slavewrite(rq);
			break;
		default:
			respond(rq, "exportfs: slave type error");
		}
		if(rq->flushtag != NOTAG) {
			rq->t.type = Tflush;
			rq->t.tag = rq->flushtag;
			respond(rq, nil);
		}
		rq->busy = 0;	
		m->busy = 0;
	}
}

void
flushaction(void *a, char *cause)
{
	USED(a);
	if(strncmp(cause, "kill", 4) == 0)
		noted(NDFLT);

	noted(NCONT);
}

void
slaveopen(Req *rq)
{
	Waitmsg *w;
	char err[ERRMAX];
	int n, fds[2], len, datasz, error;
	char buf[8192];
	uchar *data;
	Dirtab *d;
	
	if (pipe(fds) < 0) {
		respond(rq, "slaveopen: pipe failed");
		return;
	}
	switch (fork()) {
	case -1:
		respond(rq, "slaveopen: fork failed");
		return;
	case 0:
		d = &dir[rq->f->path];
		close(fds[0]);
		dup(fds[1], 1);
		dup(fds[1], 2);
		close(fds[1]);
		snprint(buf, sizeof buf, "/bin/%s", d->opscrpt);
		execl(buf, buf, nil);
		exits("exec");
	}
	close(fds[1]);
	error = 0;
	len = 0;
	datasz = sizeof buf;
	data = malloc(datasz);
	if (data == nil) {
		close(fds[0]);
		rerrstr(err, ERRMAX);
		respond(rq, err);
		return;
	}
	for (;;) {
		n = read(fds[0], buf, sizeof buf);
		if (n < 0) {
			free(data);
			data = nil;
			len = 0;
			error = 1;
			break;
		}
		if (n == 0)
			break;
		if (len + n >= datasz) {
			datasz *= 2;
			data = realloc(data, datasz);
			if (data == nil) {
				len = 0;
				error = 1;
				break;
			}
		}
		memmove(data + len, buf, n);
		len += n;
	}
	do {
		w = wait();
		if (w == nil) {
			errstr(err, sizeof err);
			if (strcmp(err, "interrupted") == 0)
				continue;
			sysfatal("wait");
		}
	} while (0);
	rq->f->data = data;
	rq->f->len = len;
	rq->r.count = 0;
	rq->r.data = nil;
	if (*w->msg)
		respond(rq, w->msg);
	else if (error) {
		rerrstr(err, ERRMAX);
		respond(rq, err);
	} else
		respond(rq, nil);
	close(fds[0]);
	free(w);
}

void
slaveread(Req *rq)
{
	Waitmsg *w;
	char err[ERRMAX];
	int n, fds[2];
	char buf[8192], ditch[32];
	Dirtab *d;
	
	if (pipe(fds) < 0) {
		respond(rq, "slaveread: pipe failed");
		return;
	}
	switch (fork()) {
	case -1:
		respond(rq, "slaveread: fork failed");
		return;
	case 0:
		d = &dir[rq->f->path];
		close(fds[0]);
		dup(fds[1], 1);
		dup(fds[1], 2);
		close(fds[1]);
		snprint(buf, sizeof buf, "/bin/%s", d->rdscrpt);
		execl(buf, buf, nil);
		exits("exec");
	}
	close(fds[1]);
	n = readn(fds[0], buf, sizeof buf);
	rq->r.count = n;
	/* ignore rest of write */
	while (n > 0)
		n = read(fds[0], ditch, sizeof ditch);
	do {
		w = wait();
		if (w == nil) {
			errstr(err, sizeof err);
			if (strcmp(err, "interrupted") == 0)
				continue;
			sysfatal("wait");
		}
	} while (0);
	if (*w->msg) {
		rq->r.count = 0;
		respond(rq, w->msg);
	} else {
		rq->r.data = buf;
		respond(rq, nil);
	}
	close(fds[0]);
	free(w);
}

void
slavewrite(Req *rq)
{
	Waitmsg *w;
	char *argv[20], err[ERRMAX];
	int n;
	char buf[8192], b2[8192];
	Dirtab *d;
	char *cp;
	
	switch (fork()) {
	case -1:
		respond(rq, "slavewrite: fork failed");
		return;
	case 0:
		d = &dir[rq->f->path];
		snprint(b2, sizeof b2, "%s %s", d->wrscrpt, rq->t.data);
		DEBUG (2, "b2=\"%s\"\n", b2);
		n = tokenize(b2, argv, nelem(argv));
		argv[n] = nil;
		snprint(buf, sizeof buf, "/bin/%s", d->wrscrpt);
		exec(buf, argv);
		exits("exec");
	}
	do {
		w = wait();
		if (w == nil) {
			errstr(err, sizeof err);
			if (strcmp(err, "interrupted") == 0)
				continue;
			sysfatal("wait");
		}
	} while (0);
	if (*w->msg) {
		rq->r.count = 0;
		cp = strchr(w->msg, ':');
		if (cp != nil)
			respond(rq, ++cp);
	} else {
		rq->r.count = rq->t.count;
		respond(rq, nil);
	}
}

void
nowrite(Req *rq)
{
	respond(rq, "write not allowed");
}

void
rdctl(Req *rq)
{
	rq->r.count = 0;
	respond(rq, "read not supported");
}

void
rdsos(Req *rq)
{
	vlong count;
	Fid *f;

	f = findf(rq->t.fid);
	if (f == nil) {
		respond(rq, "no fid");
		return;
	}
	if (f->data == nil || f->len == 0) {
		respond(rq, "sos unavailable");
		return;
	}
	count = rq->t.count;
	if (count + rq->t.offset >= f->len)
		count = f->len - rq->t.offset;
	if (count < 0)
		count = 0;
	rq->r.count = count;
	rq->r.data = (char*)f->data + rq->t.offset;
	respond(rq, nil);
}

void
readbuf(Req *r, char *s, long n)
{
	r->r.count = r->t.count;
	if(r->t.offset >= n){
		r->r.count = 0;
		return;
	}
	if(r->t.offset+r->r.count > n)
		r->r.count = n - r->t.offset;
	r->r.data = s;
}

void
readstr(Req *r, char *s)
{
	readbuf(r, s, strlen(s));
}	

vlong
getuptime(void)
{
	char buf[512], *args[5];
	int fd, n;
	vlong ticks, hz;

	fd = open("#c/time", OREAD);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof buf);
	close(fd);
	if (n < 0)
		return -1;
	n = tokenize(buf, args, 5);
	if (n < 4) {
		werrstr("unknown file format");
		return -1;
	}
	ticks = strtoll(args[2], &args[2], 10);
	hz    = strtoll(args[3], &args[3], 10);
	if (*args[2] != 0 || *args[3] != 0 || hz == 0) {
		werrstr("unexpected chars");
		return -1;
	}
	return ticks / hz;
}

void
rduptime(Req *rq)
{
	char err[ERRMAX], buf[100];
	vlong up;

	up = getuptime();
	if (up < 0) {
		rerrstr(err, ERRMAX);
		respond(rq, err);
	} else {
		snprint(buf, sizeof buf, "%lld", up);
		readstr(rq, buf);
		respond(rq, nil);
	}
}

void
rmsos(Req *rq)
{
	if (rq->f->data)
		free(rq->f->data);
	rq->f->data = nil;
	rq->f->len = 0;
	respond(rq, nil);
}

int
cmdcode(char *c, Cmd *tab)
{
	while (tab->name) {
		if (strcmp(c, tab->name) == 0)
			return tab->code;
		tab++;
	}
	return -1;
}
	
void
wrctl(Req *rq)
{
	char *argv[10], *msg;
	
	rq->t.data[rq->t.count] = 0;
	tokenize(rq->t.data, argv, nelem(argv));
	rq->r.count = rq->t.count;
	switch (cmdcode(argv[0], cmds)) {
	case CMhalt:
		respond(rq, nil);
		sleep(Nhaltsecs);
		shell("halt -f");
		break;
	case CMreboot:
		respond(rq, nil);
reboot:
		sleep(Nhaltsecs);	
		shell("reboot -f");
		break;
	case CMupdate:
		msg = shell("pupdate");
		if (msg != nil) 
			respond(rq, "update file is invalid or cannot be read");
		else {
			respond(rq, nil);
			goto reboot;
		}
		free(msg);
		break;
	default:
		respond(rq, "bad command");
		return;
	}
}

char *
shell(char *s)
{
	Waitmsg *w;
	char err[ERRMAX];
	char buf[80];
	char *argv[10], *msg;
	int argc;
	void notifyf(void *, char *);

	argc = tokenize(s, argv, nelem(argv));
	argv[argc] = nil;
	msg = nil;
	switch (fork()) {
	case -1:
		fatal("fork");
	case 0:
		snprint(buf, sizeof buf, "/bin/%s", argv[0]);
		exec(buf, argv);
		fprint(2, "%r\n");
		exits("exec");
	}
	notify(notifyf);
loop:
	if (w = wait()) {
		if (w->msg && w->msg[0])
			msg = strdup(w->msg);
		free(w);
	} else {
		errstr(err, sizeof err);
		if (strcmp(err, "interrupted") == 0)
			goto loop;
		fatal("wait");
	}
	return msg;
}

void
notifyf(void *, char *s)
{
	if (strcmp(s, "interrupt") == 0)
		noted(NCONT);
	noted(NDFLT);
}

void
dread(Req *rq)
{
	uchar buf[8192], *p;
	int remain, len, n, i;
	Dir e;
	Qid q;
	Dirtab *d;
	
	p = buf;
	remain = sizeof buf;
	for (i = 1; i < ndir; i++) {
		d = dir + i;
		mkqid(&q, i, 0, 0);
		devdir(q, d->name, d->length, user, d->perm, &e);
		n = convD2M(&e, p, remain);
		if (n <= 0) {
			respond(rq, "dread failed");
			return;
		}
		p += n;
		remain -= n;
	}
	len = p - buf;
	len -= rq->t.offset;
	p = buf + rq->t.offset;
	if (len > rq->t.count)
		len = rq->t.count;
	if (len <= 0) {
		rq->r.count = 0;
		respond(rq, nil);
		return;
	}
	rq->r.data = (char *)p;
	rq->r.count = len;
	respond(rq, nil);
}

int
dirwalk(char *w)
{
	int i;
	
	for (i = 1; i < ndir; i++)
		if (strcmp(w, dir[i].name) == 0)
			return i;
	return -1;
}

void
dostat(Req *rq)
{
	Fid *f;
	int n;
	Dir e;
	uchar buf[8192];
	Dirtab *d;
	Qid q;

	f = findf(rq->t.fid);
	if (f == nil) {
		respond(rq, "unknown fid");
		return;
	}
	if (f->path >= ndir) {
		respond(rq, "bad path");
		return;
	}
	d = dir + f->path;
	if(f->path == 0)
		mkqid(&q, f->path, 0, QTDIR);
	else
		mkqid(&q, f->path, 0, QTFILE);
	devdir(q, d->name, d->length, user, d->perm, &e);
	n = convD2M(&e, buf, sizeof buf);
	if (n == 0) {
		respond(rq, "stat failed");
		return;
	}
	rq->r.stat = buf;
	rq->r.nstat = n;
	respond(rq, nil);
}

/***

name of file
perm
ptr to read function or nil
ptr to write function or nil
ptr to clunk function or nil
name of open script
name of read script
name of write script

The script's return code is nil for good and the errstr if failed.
Reads take the first 8k of stdout.
Writes have feed the data of the Twrite as stdin.
**/
