#include <u.h>
#include <libc.h>
#include <ctype.h>
#include "aoe.h"

enum {
	//Nwork = 8,
	Nwork = 1,
	Blksize = 8192,
};

static QLock slock;
static uvlong copied;
static int pctdone;
static uvlong size;

static void startaoe(int);
static Aoedev *gettarg(void);
static void listslrp(void);
static void worker(Aoedev *, int, uvlong);

static void
usage(void)
{
	fprint(2, "usage: %s\n", argv0);
	exits(nil);
}

void
main(int argc, char *argv[])
{
	Aoedev *dev;
	Waitmsg *w;
	Dir *d;
	char *ecdev = "#S/sdS0/data";
	char *badness;
	int i, rfd, dfd;

	ARGBEGIN {
	case '?':
		usage();
		break;
	} ARGEND

	if (argc != 0)
		usage();
	rfd = open("/raid/ctl", OWRITE);
	if (rfd < 0)
		sysfatal("could not open raid control");
	startaoe(rfd);
	dev = gettarg();
	if (dev == nil) {
		print("Couldn't connect to CacheMotion: connect ether1 to CacheMotion card\n");
		exits (nil);
        }
	dfd = open(ecdev, OWRITE);
	if (dfd < 0) {
		print("Couldn't open cache: %r\n");
		exits (nil);
	}
	d = dirfstat(dfd);
	size = d->length;
	free(d);
	while (1) {
		copied = 0;
		pctdone = 0;
		print("\rStarting NVRAM network loop test...\n");
		for (i = 0; i < Nwork; ++i) {
			if (rfork(RFPROC|RFMEM) == 0)
				worker(dev, dfd, i);
		}
		badness = nil;
		for (i = 0; i < Nwork; ++i) {
			if ((w = wait()) == nil)
				break;
			if (w->msg && w->msg[0])
				badness = strdup(w->msg);
			free(w);
		}
		if (i < Nwork)
			fprint(2, "internal error: too few children\n");
		else if (badness) {
			fprint(2, "%s\n", badness);
			print("\nemi-cmstress failed\n");
		}
		else
			print("\nemi-cmstress passed\n");
	}
}

static void
startaoe(int rfd)
{
	char *eths[] = {"ether1"};

	fprint(rfd, "stopether /net/ether1");
	if (aoeinit(1, eths) < 0)
		sysfatal("Could not initialize AoE\n");
}

static Aoedev *
gettarg(void)
{
	Aoedev *d = nil;
	int i;
	int m;
	char targ[32];

        print("Probing for NVRAM targets ...\n");
        aoediscover();
        for (i = 0, m = 0; i < ndevs; ++i) {
            if (strcmp(devs[i].model, "NVWC") == 0) {
                d = &devs[i];
		++m;
                print("\n%d target found\n", m);
		break;
	    }
        }

	if (d == nil) {
		print("No such target %s\n", targ);
	}
	return d;
}

static void
worker(Aoedev *dev, int dfd, uvlong start)
{
	uchar *wrkbuf;
	int n, d, len;
	int test_size;

	wrkbuf = malloc(Blksize);
	len = Blksize;
	test_size = size/10;

	while (1) {
		if (start * Blksize >= test_size) {
         		print ("complete\n");
			break;
		}
		if (start * Blksize + len >= test_size)
			len = test_size - start * Blksize;
		if ((n = aoeread(dev, wrkbuf, Blksize, start * Blksize)) <= 0) {
			print ("aoeread doen\n");
			break;
		}
		if (pwrite(dfd, wrkbuf, n, start * Blksize) != n) {
			print("\nCache Motion write failure: %r\n");
			exits("could not write to cache");
		}
		qlock(&slock);
		copied += n;
		d = (100LL * copied) / test_size;
	        if (d != pctdone) {
	            pctdone = d;
	            //print("\b\b\b\b%3d%%", pctdone);
	        }
	        qunlock(&slock);
	        start += Nwork;
	}
	free(wrkbuf);
	exits(nil);
}
