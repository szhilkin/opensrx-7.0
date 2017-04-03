#include <u.h>
#include <libc.h>
#include <ctype.h>
#include "aoe.h"

enum {
//	Nwork = 8,
	Nwork = 1,
	Blksize = 8192,
};

enum {
	PSMmissing 	= 0,
	PSMcharging 	= 1<<0,
	PSMcharged	= 1<<1,
	PSMpwr		= 1<<2,
	PSMoff		= 1<<3,

	PSMsconly 		= PSMcharged,
	PSMpwrnosc		= PSMpwr, 
	PSMpwrcharging 		= PSMpwr | PSMcharging,
	PSMpwrsc 		= PSMpwr | PSMcharged,
};

static void checkempty(void);
static void startaoe(int);
static void initdialog(void);
static Aoedev *gettarg(void);
static void listslrp(void);
static void waitsc(void);
static void worker(Aoedev *, int, uvlong);

static QLock slock;
static uvlong copied;
static int pctdone;
static uvlong size;

static void
usage(void)
{
	fprint(2, "usage: %s\n", argv0);
	exits(nil);
}

void
main(int argc, char *argv[])
{
	Dir *d;
	Aoedev *dev;
	Waitmsg *w;
	char *ecdev = "#S/sdS0/data";
	char *badness;
	int i, n, fd, rfd, dfd;
	char buf[32];

	ARGBEGIN {
	case '?':
		usage();
		break;
	} ARGEND

	dfd = -1;
	if (argc != 0)
		usage();
	d = dirstat(ecdev);
	if (d == nil){
		fprint(2, "error: no CacheMotion card\n");
		exits(nil);
	}
	rfd = open("/raid/ctl", OWRITE);
	if (rfd < 0)
		sysfatal("could not open raid control");
	checkempty();
	initdialog();
	startaoe(rfd);
	dev = gettarg();
	if (dev == nil)
		goto reboot;
	dfd = open(ecdev, OWRITE);
	if (dfd < 0) {
		print("Couldn't open cache: %r\n");
		goto reboot;
	}
	d = dirfstat(dfd);
	size = d->length;
	free(d);
	waitsc();
	print("\rRecovering CacheMotion data ...   0%%");
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
	else if (badness)
		fprint(2, "%s\n", badness);
	else if (pctdone < 100)
		fprint(2, "CacheMotion did not complete successfully.\n");
	else {
		fd = create("/n/kfs/srx/shelf", OWRITE, 0666);
		if (fd >= 0) {
			fprint(fd, "%uld\n", dev->targ >> 16);
			close(fd);
		}
		print("\nTransfer drives from the failed appliance to the recovery appliance.\n");
		print("Always insert transferred drives in the same slots in the recovery appliance that they occupied in the failed appliance.\n");
		fd = open("/raid/ctl", OWRITE);
		if (fd >= 0) {
			if (fprint(fd, "ecattach 0 #S/sdS0/data 0") < 0) {
				rerrstr(buf, 32);
				if (!strstr(buf, "mismatch")) {
					close(fd);
					fprint(2, "Invalid cache image on failed shelf: %r\n");
					goto reboot;
				}
			}
			seek(fd, 0, 0);
			fprint(fd, "ecflag 0 1");
			close(fd);
		}
	}
reboot:
	if (dfd >= 0)
		close(dfd);
	if (rfd >= 0)
		close(rfd);
	do {
		print("Please type OK to reboot: ");
		n = read(0, buf, 31);
		buf[n] = 0;
	} while (cistrcmp(buf, "OK\n") != 0);
	execl("/rc/bin/reboot", "reboot", "-f", nil);
}

static void
checkempty(void)
{
	Dir *d;
	char *p;
	int fd, i, n, m;
	char buf[512];

	fd = open("/raiddev", OREAD);
	if (fd < 0)
		sysfatal("could not read raid device directory");
	n = dirreadall(fd, &d);
	close(fd);
	if (n <= 0)
		sysfatal("could not read raid device directory");
	for (i = 0; i < n; ++i) {
		for (p = d[i].name; *p && isdigit(*p); ++p) ;
		if (*p == 0) {
			p = smprint("/raiddev/%s/stat", d[i].name);
			fd = open(p, OREAD);
			free(p);
			m = read(fd, buf, 511);
			close(fd);
			buf[m] = 0;
			p = strstr(buf, "state: ");
			/*
			 * could catch either state: or sstate:, but that's ok because both say
			 * missing if there's no disk
			 */

			if (p && strncmp(p+7, "missing", 7) != 0)
				sysfatal("Drives are detected in recovery appliance.  There must be no drives in the recovery appliance.");
		}
	}
	free(d);
}

static void
initdialog(void)
{
	int i, fd, pid;
	char buf[32];

	/*
	 * Tech Pubs has asked to remove this because there's nothing to bring
	 * down if there aren't any disks in the box.  But I have a feeling we
	 * may come back and want some kind of gateway question at the
	 * beginning anyway.  So I'm leaving it just commented out.
	 *
	print("NVRAM card recovery requires bringing down the local storage service.\n");
	print("OK to continue [yes or no]? ");
	i = read(0, buf, 31);
	buf[i] = 0;
	if (cistrcmp(buf, "yes\n") != 0) {
		print("aborting\n");
		exits(nil);
	}
	*/

	fd = open("/tmp/cminit.pid", OREAD);
	if (fd >= 0) {
		i = read(fd, buf, 32);
		buf[i] = 0;
		pid = atoi(buf);
		close(fd);
		snprint(buf, 31, "/proc/%d/ctl", pid);
		fd = open(buf, OWRITE);
		if (fd >= 0) {
			fprint(fd, "kill");
			close(fd);
		}
	}
	fd = open("/raid/ctl", OWRITE);
	if (fd >= 0) {
		fprint(fd, "ecclose 0");
		close(fd);
	}
	do {
		print("Make sure that the recovery appliance is the same model as the failed appliance.\n");
		print("\nUse the following steps to prepare for recovery:\n\n");
		print("  1. Power: Power up the failed appliance\n");
		print("  2. Data: In general, connect CM1 on the failed appliance to ether1 on the\n");
		print("     recovery appliance.  Cabling strategies depend on your specific\n");
		print("     environment and can include: direct vs. over the SAN, or through a\n");
		print("     management network.\n");
		print("\nUse RJ-45 Cat 6 Ethernet cables.  For details, see SRX documentation.\n");
		print("\nAre the data cable connections established? [yes or no] ");
		read(0, buf, 31);
	} while (buf[0] != 'y');
	print("\nPlease note: It may take as much as 2 minutes for the recovery NVRAM\n");
	print("card to be available.\n\n");
}

static void
startaoe(int rfd)
{
	fprint(rfd, "stopallether");
	if (aoeinit(0, nil) < 0)
		sysfatal("Could not initialize AoE\n");
}

static Aoedev *
gettarg(void)
{
	Aoedev *d;
	char *p;
	ulong t;
	int i;
	char targ[32];

retry:
	do {
		listslrp();
		print("Specify a CacheMotion target from which to recover data (type q to quit, r to rescan): ");
		i = read(0, targ, 16);
		targ[i] = 0;
		if (targ[0] == 'q') {
			print("quitting\n");
			return nil;
		}
	} while (targ[0] == 'r');
	if (targ[i-1] == '\n')
		targ[i-1] = 0;
	p = strchr(targ, '.');
	if (!p) {
		print("Invalid target %s\n", targ);
		goto retry;
	}
	t = atoi(targ) << 16 | atoi(p+1);
	d = aoetarg2dev(t);
	if (d == nil) {
		print("No such target %s\n", targ);
		goto retry;
	}
	return d;
}

static void
listslrp(void)
{
	int i, m;

	print("Probing for CacheMotion targets ...\n");
	aoediscover();
	for (i = 0, m = 0; i < ndevs; ++i) {
		if (strcmp(devs[i].model, "NVWC") == 0) {
			print("%uld.%uld ", devs[i].targ >> 16, devs[i].targ & 0xffff);
			++m;
		}
	}
	print("\n%d targets found\n", m);
}

static void
waitsc(void)
{
	char *toks[128];
	int fd, i, n, pass, ntoks;
	char buf[8192];

	pass = 0;
	while(1) {
		fd = open("#S/sdS0/ctl", OREAD);
		if (fd >= 0) {
			n = read(fd, buf, 8192);
			buf[n] = 0;
			close(fd);
			ntoks = tokenize(buf, toks, 128);
			for (i = 0; i < ntoks && strcmp(toks[i], "psmstate"); ++i) ;
			if (i >= ntoks-1)
				fprint(2, "No cache power state\n");
			else if (atoi(toks[i+1]) == PSMpwrsc) {
				if (pass != 0)
					print("done\n");
				return;
			}
		}
		if (pass == 0)
			print("Waiting for the CacheMotion supercap to charge ... ");
		++pass;
		sleep(5000);
	}
}

static void
worker(Aoedev *dev, int dfd, uvlong start)
{
	uchar *wrkbuf;
	char *p;
	int n, d, len;

	wrkbuf = malloc(Blksize);
	len = Blksize;
	while (1) {
		n = 0;
		if (start * Blksize >= size)
			break;
		if (start * Blksize + len >= size)
			len = size - start * Blksize;
		for (d = 0; d < 5; ++d)
			if (n = aoeread(dev, wrkbuf, len, start * Blksize))
				break;
		if (n <= 0) {
			print("\nError reading from the target: %d: %r\n", n);
			break;
		}
		if (pwrite(dfd, wrkbuf, n, start * Blksize) != n) {
			p = smprint("CacheMotion write failure: %r");
			exits(p);
		}
		qlock(&slock);
		copied += n;
		d = (100LL * copied) / size;
		if (d != pctdone) {
			pctdone = d;
			print("\b\b\b\b%3d%%", pctdone);
		}
		qunlock(&slock);
		start += Nwork;
	}
	if (n != 0) {
		print("\nFailed to read block %ulld from target: %r\n", start);
		exits(nil);
	}
	exits(nil);
}
