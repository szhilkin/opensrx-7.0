#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

enum {
	Qroot = 0,
	Qctl,
	Qdata,
};

Dirtab ramroot[] = {
	{".", {Qroot, 0, QTDIR}, 0, DMDIR|0777},
	{"ctl", {Qctl}, 0, 0666},
	{"data", {Qdata}, 0, 0666},
};

static uchar *ramdisk;
static long disksize;

static Chan *
ramattach(char *spec)
{
	return devattach(L'ℝ', spec);
}

static Walkqid *
ramwalk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, ramroot, nelem(ramroot), devgen);
}

static int
ramstat(Chan *c, uchar *a, int n)
{
	return devstat(c, a, n, ramroot, nelem(ramroot), devgen);
}

static Chan *
ramopen(Chan *c, int mode)
{
	return devopen(c, mode, ramroot, nelem(ramroot), devgen);
}

static void
ramclose(Chan *)
{
}

static long
ramread(Chan *c, void *a, long n, vlong offset)
{
	switch (c->qid.path) {
	case Qroot:
		return devdirread(c, a, n, ramroot, nelem(ramroot), devgen);
		break;
	case Qctl:
		if (offset)
			return 0;
		return snprint(a, n, "size: %ld\n", disksize);
		break;
	case Qdata:
		if (ramdisk == nil)
			return 0;
		if (offset >= disksize)
			return 0;
		if (offset + n > disksize)
			n = disksize - offset;
		memmove(a, ramdisk + offset, n);
		return n;
		break;
	default:
		error("no file");
		break;
	}
	return 0;
}

static long
ramwrite(Chan *c, void *a, long n, vlong offset)
{
	char *p;

	switch (c->qid.path) {
	case Qroot:
		error("no write to directory");
		break;
	case Qctl:
		if (memcmp(a, "size ", 5) == 0) {
			p = a;
			p += 5;
			for (; *p == ' '; ++p) ;
			if (*p == 0)
				error("invalid control message");
			disksize = atoi(p);
			if (ramdisk) {
				free(ramdisk);
				ramdisk = nil;
			}
			if (disksize) {
				ramdisk = malloc(disksize);
				if (ramdisk == nil)
					error("memory allocation failure");
			}
			ramroot[2].length = disksize;
		}
		else
			error("unknown command");
		return n;
		break;
	case Qdata:
		if (ramdisk == nil)
			return 0;
		if (offset >= disksize)
			return 0;
		if (offset + n > disksize)
			n = disksize - offset;
		memmove(ramdisk + offset, a, n);
		return n;
		break;
	default:
		error("no file");
		break;
	}
	return 0;
}

Dev ramdevtab = {
	L'ℝ',		/* alt-R-R */
	"ram",

	devreset,
	devinit,
	devshutdown,
	ramattach,
	ramwalk,
	ramstat,
	ramopen,
	devcreate,
	ramclose,
	ramread,
	devreadv,
	devbread,
	ramwrite,
	devwritev,
	devbwrite,
	devremove,
	devwstat,
};
