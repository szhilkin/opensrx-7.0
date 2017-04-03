/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <ctype.h>

enum {
	MDunknown = 0,
	MDrestored,
	MDinvalid,
	MDwritten,
	MDbackup,
	MDbacking,
	MDrqbackup,
	MDrestoring,
	MDnoimage,
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

void async(char *);
void fail(char *);
void checktargid(void);
int waitmd(void);
int attachcache(void);
int checkclean(void);
int update(void);
int reamit(Dir *);
void monitor(void);

char *ctltoks[128];
char ctlbuf[8192];
int ntoks;

void
usage(void)
{
	fprint(2,"usage: %s\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	Dir *d;
	char *p;
	int fd, n, mdstate;
	char buf[ERRMAX];

	ARGBEGIN {
	default:
		usage();
	} ARGEND
	if (argc != 0)
		usage();
	if (access("#S/sdS0/ctl", AREAD|AWRITE))
		exits(nil);
	fd = open("/tmp/cminit.pid", OREAD);
	if (fd >= 0) {
		n = read(fd, buf, ERRMAX);
		buf[n] = '\0';
		close(fd);
		p = smprint("/proc/%d/status", atoi(buf));
		fd = open(p, OREAD);
		free(p);
		if (fd >= 0) {
			n = read(fd, buf, 6);
			buf[n] = '\0';
			close(fd);
			if (strcmp(buf, "cminit") == 0)
				exits(nil);
		}
	}
	d = dirstat("#S/sdS0/data");
	if (d == nil) {
		print("Resetting CacheMotion ... ");
		if (waitmd() == MDwritten) {
			print("backup ... ");
			fd = open("#S/sdS0/ctl", OWRITE);
			if (fd >= 0) {
				fprint(fd, "sdrambackup");
				close(fd);
			}
			waitmd();
			print("continuing reset ... ");
		}
		fd = open("#S/sdS0/ctl", OWRITE);
		if (fd >= 0) {
			fprint(fd, "reset");
			close(fd);
		}
		do {
			sleep(1000);
			d = dirstat("#S/sdS0/data");
		} while (d == nil);
		print("done.\n");
	}

	mdstate = waitmd();
	switch (mdstate) {
	case MDrestored:
	case MDwritten:
	case MDbackup:
	case MDnoimage:
		if (attachcache()) {
			if (!checkclean()) {
				fd = open("/raid/ctl", OWRITE);
				if (fd >= 0) {
					fprint(fd, "ecclose 0");
					close(fd);
				}
				exits("cache replay failed");
			}
		}
		else {
			buf[0] = 0;
			errstr(buf, ERRMAX);
			if (strstr(buf, "mismatch")) {
				fd = open("/raid/ctl", OWRITE);
				if (fd >= 0) {
					fprint(fd, "ecclose 0");
					close(fd);
				}
				fprint(2, "CacheMotion contents do not match installed LUNs\n");
				exits("cache attach failed");
			}
		}
		break;
	case MDinvalid:
		fprint(2, "Bad CacheMotion backup\n");
		exits("cache backup failure");
	default:
		fprint(2, "Got unexpected mdstate %d\n", mdstate);
		break;
	}
	if (!update())
		fail("cache firmware update failed");
	checktargid();
	if (!reamit(d))
		fail("cache format failed");
	monitor();
	exits(nil);
}

void
async(char *onoff)
{
	int fd;

	fd = open("/raid/ctl", OWRITE);
	if (fd >= 0) {
		fprint(fd, "async %s", onoff);
		close(fd);
	}
	fd = open("/raiddev/ctl", OWRITE);
	if (fd >= 0) {
		fprint(fd, "wrcache %s", onoff);
		close(fd);
	}
}

void
fail(char *s)
{
	async("off");
	exits(s);
}

void
checktargid(void)
{
	char *p;
	int n, fd, major, minor, shelf;
	char buf[11];

	fd = open("#S/sdS0/ctl", OREAD);
	if (fd < 0)
		return;
	n = read(fd, ctlbuf, 8191);
	ctlbuf[n] = 0;
	ntoks = tokenize(ctlbuf, ctltoks, 128);
	close(fd);
	for (n = 0; n < ntoks && strcmp(ctltoks[n], "aoetarget"); ++n) ;
	if (n >= ntoks-1)
		return;
	p = strchr(ctltoks[n+1], '.');
	if (p) {
		major = atoi(ctltoks[n+1]);
		minor = atoi(p+1);
	}
	else {
		major = 65535;
		minor = 255;
	}
	fd = open("/n/kfs/srx/shelf", OREAD);
	if (fd < 0)
		return;
	n = read(fd, buf, 10);
	close(fd);
	buf[n] = 0;
	shelf = atoi(buf);
	if (minor != 255 && shelf == major)
		return;
	if (minor == 255)
		minor = 254;
	fd = open("#S/sdS0/ctl", OWRITE);
	if (fd < 0)
		return;
	print("Defaulting CacheMotion target ID to %d.%d\n", shelf, minor);
	fprint(fd, "target %d.%d", shelf, minor);
	close(fd);
}

int
waitmd(void)
{
	int n, fd, mdstate, written;

	written = 0;
	while (1) {
		fd = open("#S/sdS0/ctl", OREAD);
		if (fd < 0)
			return -1;
		n = read(fd, ctlbuf, 8191);
		ctlbuf[n] = 0;
		ntoks = tokenize(ctlbuf, ctltoks, 128);
		close(fd);
		for (n = 0; n < ntoks && strcmp(ctltoks[n], "mdstate"); ++n) ;
		if (n >= ntoks-1)
			return -1;
		mdstate = atoi(ctltoks[n+1]);
		if (mdstate != MDbacking && mdstate != MDrestoring) {
			if (written)
				fprint(2, "done.\n");
			return mdstate;
		}
		if (!written) {
			fprint(2, "Waiting for CacheMotion %s ... ", (mdstate == MDbacking) ? "backup" : "restore");
			written = 1;
		}
		sleep(3000);
	}
}

int
attachcache(void)
{
	int fd, n;

	fd = open("/raid/ctl", OWRITE);
	if (fd < 0)
		return 0;
	n = fprint(fd, "ecattach 0 #S/sdS0/data 0");
	close(fd);
	if (n < 0) {
		return 0;
	}
	return 1;
}

int
checkclean(void)
{
	int rfd, n;

	fprint(2, "Checking for clean shutdown ... ");
	rfd = open("/raid/ctl", OWRITE);
	n = fprint(rfd, "ecreplay 0");
	fprint(2, "done.\n");
	close(rfd);
	if (n < 0) {
		fprint(2, "Replaying cache failed: %r\n");
		return 0;
	}
	return 1;
}

int
update(void)
{
	Dir *d;
	char *toks[128], *p;
	int fd, i, n;
	char buf[8192], buf2[32];

	fd = open("#S/sdS0/ctl", OREAD);
	if (fd >= 0) {
		n = readn(fd, buf, 8192);
		buf[n] = 0;
		close(fd);
		n = tokenize(buf, toks, 128);
		for (i = 0; i < n && strcmp(toks[i], "firmware"); ++i) ;
		if (i >= n-1) {
			fprint(2, "No cache firmware version\n");
			return 0;
		}
		fd = open("/app/arm/fwrev", OREAD);
		if (fd < 0)
			return 1;
		n = read(fd, buf2, 32);
		buf2[n] = 0;
		for (p = buf2 + n - 1; p > buf2 && isspace(*p); --p)
			*p = 0;
		if (strcmp(buf2, toks[i+1]) != 0) {
			fprint(2, "Updating cache firmware to version %s ... ", buf2);
			fd = open("#S/sdS0/ctl", OWRITE);
			if (fd >= 0) {
				fprint(fd, "mcdownload /app/arm/9slurpee");
				close(fd);
			}
			fprint(2, "done\nResetting CacheMotion ... ");
			fd = open("#S/sdS0/ctl", OWRITE);
			if (fd >= 0) {
				fprint(fd, "reset");
				close(fd);
			}
			do {
				sleep(1000);
				d = dirstat("#S/sdS0/data");
			} while (d == nil);
			fprint(2, "done\n");
		}
		return 1;
	}
	return 0;	
}

int
reamit(Dir *d)
{
	int fd, n;

	fprint(2, "Clearing CacheMotion ... ");
	fd = open("/raid/ctl", OWRITE);
	fprint(fd, "ecclose 0");
	close(fd);
	fd = open("/raid/ctl", OWRITE);
	n = fprint(fd, "ecream 0 #S/sdS0/data 4096 0 %lld", d->length);
	close(fd);
	fprint(2, "done.\n");
	if (n < 0) {
		fprint(2, "Clearing cache failed: %r\n");
		return 0;
	}
	return 1;
}

void
monitor(void)
{
	char *toks[128];
	int fd, i, j, n, pid, charge;
	char buf[8192];

	charge = 1;
	for (i = 0; i < ntoks && strcmp(ctltoks[i], "psmstate"); ++i) ;
	if (i >= ntoks-1) {
		fprint(2, "No cache power state!  Falling back to synchronous operation.\n");
		fail("no power state");
	}
	if (atoi(ctltoks[i+1]) == PSMpwrnosc) {
		charge = 0;
		fprint(2, "No supercap!  Falling back to synchronous operation.\n");
		async("off");
	}
	else if ((atoi(ctltoks[i+1]) & PSMcharged) == 0) {
		charge = 0;
		fprint(2, "Waiting for CacheMotion supercap to charge ...\n");
		async("off");
	}
	switch (pid = rfork(RFPROC|RFMEM|RFNOWAIT|RFFDG)) {
	case -1:
		fprint(2, "Failed to create child process: %r\n");
		fail("process creation failure");
		break;
	case 0:
		while (1) {
			sleep(10000);
			fd = open("#S/sdS0/ctl", OREAD);
			if (fd < 0)
				continue;
			n = read(fd, buf, 8192);
			buf[n] = 0;
			close(fd);
			ntoks = tokenize(buf, toks, 128);
			for (i = 0; i < ntoks && strcmp(toks[i], "eccsbcnt"); ++i) ;
			for (j = 0; j < ntoks && strcmp(toks[j], "eccdbcnt"); ++j) ;
			if (i < ntoks && atoi(toks[i+1]) != 0 || j < ntoks && atoi(toks[j+1]) != 0) {
				fprint(2, "Cache ECC error! Falling back to synchronous operation\n");
				remove("/tmp/cminit.pid");
				fail("off");
			}
			for (i = 0; i < ntoks && strcmp(toks[i], "psmstate"); ++i) ;
			if (i < ntoks) {
				switch (j = atoi(toks[i+1])) {
				case PSMsconly:
					fprint(2, "Impossible power state\n");
					remove("/tmp/cminit.pid");
					fail("off");
					break;
				case PSMpwrnosc:
				case PSMpwrcharging:
					if (charge) {
						charge = 0;
						fprint(2, "CacheMotion supercap %s!  Falling back to synchronous operation\n",
							j == PSMpwrnosc ? "missing" : "not charged");
						async("off");
					}
					break;
				case PSMpwrsc:
					if (!charge) {
						charge = 1;
						fprint(2, "CacheMotion supercap charged: returning to asynchronous operation\n");
						async("on");
					}
					break;
				}
			}
		}
		break;
	default:
		fd = create("/tmp/cminit.pid", OWRITE, 0664);
		if (fd >= 0) {
			fprint(fd, "%d", pid);
			close(fd);
		}
	}
}

