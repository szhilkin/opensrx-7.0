#include <u.h>
#include <libc.h>
#include <fcall.h>
#include "dat.h"

char	*srvname = "prosthesis";

void
usage(void)
{
	fprint(2, "usage: %s [-d][srvname [mtpt]]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	ARGBEGIN {
	case 'd':
		chatty++;
		dbg++;
		break;
	default:
		usage();
		break;
	} ARGEND
	if (argc-- > 0)
		srvname = *argv++;
	if (argc > 0)
		mtpt = *argv;
		
	fmtinstall('D', dirfmt);
	fmtinstall('F', fcallfmt);
	fmtinstall('M', dirmodefmt);

	if (chatty)
		print("%s started\n", argv0);
	starttime = time(nil);
	user = getuser();
	system = sysname();
	if (strlen(system) == 0)
		system = user;
	workq = malloc(Nreq * sizeof (Req));
	if (workq == nil)
		fatal("no memory for workq");
	memset(workq, 0, Nreq * sizeof (Req));
	publish();
	if (chatty)
		print("published\n");
	srv();
	exits(nil);
}

void
publish(void)
{
	char buf[32];
	int p[2], s;
	
	if (pipe(p) < 0)
		sysfatal("pipe:%r");
	snprint(buf, sizeof buf, "/srv/%s", srvname);
	s = create(buf, OWRITE, 0666);
	if (s < 0)
		sysfatal("failed create in /srv");
	fprint(s, "%d", p[0]);
	close(s);
	if (mtpt)
	if (mount(p[0], -1, mtpt, MAFTER, "") < 0)
		sysfatal("mount faled: %r");
	close(p[0]);
	fd = p[1];
}

Req *
getreq(void)
{
	int i;
	static ap;
	Req *rq;
	
	for (i = 0; i < Nreq; i++) {
		if (++ap >= Nreq)
			ap = 0;
		if (workq[ap].busy == 0)
			break;
	}
	if (i >= Nreq)
		fatal("no more work buffers");
	rq = &workq[ap];
	rq->pid = 0;
	rq->flushtag = NOTAG;
	rq->busy = 1;
	return rq;
}

void
srv(void)
{
	Req *rq;
	int n;
	
	for (;;) {
		rq = getreq();
		if (chatty)
			print("read9pmsg -> ");
		n = read9pmsg(fd, rq->b, sizeof rq->b);
		if (chatty)
			print("%d\n", n);
		if (n <= 0)
			sysfatal("read9pmsg:%r");
		n = convM2S(rq->b, n, &rq->t);
		if (n == 0) {
			print("bogus message\n");
			continue;
		}
		if (chatty)
			print("%F\n", &rq->t);
		rq->r.type = rq->t.type + 1;
		rq->r.fid = rq->t.fid;
		rq->r.tag = rq->t.tag;
		switch (rq->t.type) {
		case Tversion:
			rq->r.version = VERSION9P;
			rq->r.msize = rq->t.msize;
			respond(rq, nil);
			break;
		case Tattach:
			doattach(rq);
			break;
		case Tauth:
			respond(rq, "can't auth");
			break;
		case Tflush:
			doflush(rq);
			break;
		case Tremove:
			respond(rq, "can't remove");
			break;
		case Tclunk:
			doclunk(rq);
			break;
		case Twalk:
			dowalk(rq);
			break;
		case Topen:
			doopen(rq);
			break;
		case Tcreate:
			respond(rq, "can't create");
			break;
		case Tread:
			doread(rq);
			break;
		case Twrite:
			dowrite(rq);
			break;
		case Tstat:
			dostat(rq);
			break;
		case Twstat:
			respond(rq, "can't wstat");
			break;
		default:
			respond(rq, "unknown message type");
			break;
		}
	}
}

void
respond(Req *rq, char *err)
{
	int n;

	rq->r.tag = rq->t.tag;
	rq->r.fid = rq->t.fid;
	if (err) {
		rq->r.type = Rerror;
		rq->r.ename = err;
	} else
		rq->r.type = rq->t.type + 1;
	if (chatty)
		print("%F\n", &rq->r);
	n = convS2M(&rq->r, rq->b, sizeof rq->b);
	assert(n > 0);
	write(fd, rq->b, n);
	rq->busy = 0;
}

void
doattach(Req *rq)
{
	rq->r.qid.type = QTDIR;
	rq->r.qid.path = 0;
	rq->r.qid.vers = 0;
	newf(rq->t.fid, 0);
	rq->r.iounit = 8*1024;
	respond(rq, nil);
}

void
doflush(Req *rq)
{
	Req *w, *e;

	e = &workq[Nreq];
	for (w = workq; w < e; w++) {
		if (w->t.tag == rq->t.oldtag) {
			DEBUG(2, "\tQ busy %d pid %p can %d\n", w->busy, w->pid, w->canint);
			if(w->busy && w->pid) {
				w->flushtag = rq->t.tag;
				DEBUG(2, "\tset flushtag %d\n", rq->t.tag);
				if (w->canint)
					postnote(PNPROC, w->pid, "flush");
				rq->busy = 0;
				return;
			}
		}
	}
	respond(rq, nil);
	DEBUG(2, "\tflush reply\n");
	rq->busy = 0;
}

void
doclunk(Req *rq)
{
	Fid *f;
	Dirtab *d;
	
	if (f = findf(rq->t.fid))  {
		nfids--;
		f->used = 0;
		d = dir + f->path;
		rq->f = f;
		if (d->clunk) {
			d->clunk(rq);
			return;
		}
		respond(rq, nil);
	} else
		respond(rq, "can't find fid");
}

/*
 * this walk is special since our device only has three entries in the directory
 * and none of them is a directory.
 */
 
void
dowalk(Req *rq)
{
	int path, i;
	Qid *q;
	Fid *f, *nf;
	char *w;
	
	rq->r.nwqid = 0;
	f = findf(rq->t.fid);
	if (f == nil) {
		respond(rq, "unknown fid");
		return;
	}
	nf = findf(rq->t.newfid);
	if (rq->t.nwname == 0) {
		if (nf) {
			respond(rq, "bad walk");
			return;
		}
		newf(rq->t.newfid, f->path);
		respond(rq, nil);
		return;
	}
	path = -1;
	for (i = 0; i < rq->t.nwname; i++) {
		w = rq->t.wname[i];
		if (strcmp(w, "..") == 0)
			path = 0;
		else
			path = dirwalk(w);
		if (path == -1) {
			respond(rq, "walk failed");
			return;
		}
		q = rq->r.wqid + i;	// ???
		q->type = path == 0 ? QTDIR : QTFILE;
		q->path = path;
		q->vers = 0;
		rq->r.nwqid++;
	}
	if (rq->r.nwqid < rq->t.nwname) {
		respond(rq, nil);
		return;
	}
	if (rq->t.fid != rq->t.newfid) {
		if (nf) {
			respond(rq, "duplicate fid");
			return;
		}
		newf(rq->t.newfid, path);
	} else {
		if (nf == nil) {
			respond(rq, "fid unknown");
			return;
		}
		nf->path = path;
	}
	respond(rq, nil);
}

int
newf(int fid, int path)	/* add new fid into table */
{
	Fid *f, *q;

	q = nil;
	for (f = ftab; f < &ftab[Nfid]; f++)
		if (!f->used)
			q = f;
		else if (f->fid == fid)
			return 0;
	if (q) {
		q->used++;
		q->fid = fid;
		q->path = path;
		q->open = -1;
		q->data = nil;
		q->len = 0;
		nfids++;
		return 1;
	}
	return 0;
}

Fid *
findf(int fid)	/* look up a fid in the table */
{
	Fid *f;

	for (f = ftab; f < &ftab[Nfid]; f++)
		if (f->used && f->fid == fid)
			return f;
	return nil;
}

int
clunkf(int fid)	/* toss a fid */
{
	Fid *f;
	
	for (f = ftab; f < &ftab[Nfid]; f++)
		if (f->used && f->fid == fid) {
			nfids--;
			f->used = 0;
			return 1;
		}
	return 0;
}

void
mkqid(Qid *q, vlong path, ulong vers, int type)
{
	q->type = type;
	q->vers = vers;
	q->path = path;
}

void
devdir(Qid qid, char *n, vlong length, char *user, long perm, Dir *db)
{
	db->name = n;
	db->qid = qid;
	db->type = 0;
	db->dev = 0;
	db->mode = perm;
	db->mode |= qid.type << 24;
	db->atime = time(nil);
	db->mtime = starttime;
	db->length = length;
	db->uid = user;
	db->gid = system;
	db->muid = user;
}

void
fatal(char *s)
{
	Proc *p;
	
	fprint(2, "prosthesis: %s: %r\n", s);
	for (p = proclist; p; p = p->next)
		postnote(PNPROC, p->pid, "exit");
	exits("fatal");
}
