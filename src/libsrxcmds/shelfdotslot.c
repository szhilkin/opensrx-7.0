/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include <stdio.h>
#include "srxcmds.h"

static int bshelf = -1;

static char *
getpartdotdrive(char* lunstr)
{
	char *dot;

	if (dot = strchr(lunstr, '.')) {
		*dot = 0;
		return (dot + 1);
	}
	return nil;
}

int
parsedrive(char *name, int checkstate, char **rret)
{
	int fd, ret = 0;
	char *b, buf[100], *reason;

	reason = nil;
	b = mustsmprint("/raiddev/%s", name);
	fd = open(b, OREAD);
	if (fd > 0 && readfile(buf, sizeof buf, "%s/stat",  b) > 0)
		ret = 1;
	else
		reason = "does not exist";
	if (ret && checkstate && strstr(buf, "state: up") == 0) {
		ret = 0;
		reason = "is missing";
	}
	close(fd);
	free(b);
	if (rret)
		*rret = reason;
	return ret;
}

char *
parseshelfdotslot(char *shelfdotslot, void (*usage)(void), char **strsuffix, int checkstate)
{
	char *shelf, *dot, *drive, *suffix, *e, *reason;

	drive = nil;
	if (strsuffix)
		*strsuffix = nil;
	shelf = shelfdotslot;
	if (dot = strchr(shelf, '.')) {
		drive = dot + 1;
		*dot = 0;
	} else
		usage();
	if (isdigit(*shelf) == 0 || isdigit(*drive) == 0)
		usage();
	if (strtoul(shelf, &e, 10) != getshelf() || *e != '\0' ||
	    (shelf[0] == '0' && strlen(shelf) > 1)) {
		werrstr("wrong shelf id %s", shelf);
		return nil;
	}
	if (suffix = strchr(drive, ':')) {
		if (strsuffix)
			*strsuffix = strdup(suffix);
		*suffix = 0;
	}
	if (parsedrive(drive, checkstate, &reason) == 0) {
		werrstr("drive %s.%s %s", shelf, drive, reason);
		return nil;
	}
	sprint(shelfdotslot, "%s.%s", shelf, drive);
	return drive;
}

/*
 * check if part.drive belongs to lun
 * input:
 * 	lun
 * 	partdotdrive is in part.drive format
 * output:
 * 	0 not present
 *	1 present
 * expects parameters to already have been validated
 */
static int
ispartdotdriveinlun(char *lun, char *part, char *drive)
{
	char *buf;
	char filestr[50];
	Biobuf *bp;
	char *partdotdrive;
	int ret = 0;
 
	snprint(filestr, sizeof filestr, "/raid/%s/raidstat", lun);
	bp = Bopen(filestr, OREAD);
	if (!bp)
		return ret;
	partdotdrive = mustsmprint("%s.%s", part, drive);
	while (buf = Brdline(bp, '\n')) {
		buf[Blinelen(bp)-1] = 0;
		if (strstr(buf, partdotdrive)) {
			ret = 1;
			break;
		}
	}
	Bterm(bp);
	free(partdotdrive);
	return ret;
}

int
parselundotpartdotdrive(char *lpd, char **lun, char **part, char **drive, void (*usage)(void))
{
	char *f[4], *ptr;

	if (getfields(lpd, f, 4, 0, ".") != 3)
		usage();
	strtol(f[0], &ptr, 10);
	if (*f[0] == 0 || *ptr != 0)
		usage();
	strtol(f[1], &ptr, 10);
	if (*f[1] == 0 || *ptr != 0)
		usage();
	strtol(f[2], &ptr, 10);
	if (*f[2] == 0 || *ptr != 0)
		usage();
	if (islun(f[0]) == 0) {
		werrstr("LUN %s does not exist", f[0]);
		return -1;
	}
	if (ispartdotdriveinlun(f[0], f[1], f[2]) == 0) {
		werrstr("%s.%s does not exist", f[1], f[2]);
		return -1;
	}
	*lun = f[0];
	*part = f[1];
	*drive = f[2];
	return 0;
}
