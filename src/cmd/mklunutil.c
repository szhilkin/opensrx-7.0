/*
  * Copyright © 2013 Coraid, Inc.
  * All rights reserved.
  */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <libcutil.h>
#include "srxcmds.h"

extern void usage(void);

static char Ebadmagic[] = "invalid magic";
static char Ebadluncfg[] = "invalid lun config";
static char updatelunstr[] = "/n/sys/config/update";

char *
getupdatelun(void)
{
	Dir *dp;
	char buf[200], *retstr;
	int i, n;

	retstr = nil;
	n = numfiles("/raid", &dp);
	for (i = 0; i < n; i++) {
		if (isdigit(dp[i].name[0])) {
			if (readfile(buf, sizeof buf, "/raid/%s/raidstat", dp[i].name) < 0)
				errfatal("%r");
			else if (strstr(buf, updatelunstr)) {
				retstr = strdup(dp[i].name);
				break;
			}
		}
	}
	free(dp);
	return retstr;
}

int
getdevpath(int argc, char **argv, char *name, int len, int clean)
{
	char *drive, *suffix;
	char *pname, *ename;

	pname = name;
	ename = name + len;
	*pname = 0;
	while (argc-- > 0) {
		suffix = nil;
		if (cistrcmp(*argv, "missing") == 0) {
			pname = seprint(pname, ename, "missing%s", clean ? ":c" : "");
		} else {	
			drive = parseshelfdotslot(*argv, usage, &suffix, 1);
			if (drive) {
				if(!suffix && clean)	
					suffix = ":c";
			} else
				return -1;
			pname = seprint(pname, ename, "/raiddev/%s/data%s", drive, suffix ? suffix : "");
		}
		if (argc)
			pname = seprint(pname, ename, " ");	
		argv++;
	}
	return 0;
}

static int
mklunwrite(char *options, char *lun, char *raidtype, char *devices)
{
	int fd, ret, len;
	char str[1500], *e;

	/* Octal and hexidecimal numbers are invalid - but '0' is valid */
	strtoul(lun, &e, 10);
	if (*e != '\0' || (lun[0] == '0' && strlen(lun) > 1)) {
		werrstr("LUN %s name is invalid", lun);
		return -1;
	}
	len = snprint(str, sizeof str, "lbraid %s %s %s %s", options, lun, raidtype, devices);
	fd = open("/raid/ctl", OWRITE);
	if (fd < 0)
		return -1;
	ret = write(fd, str, len);
	close(fd);
	return ret;
}

int
makelun(char *options, char *lun, char *raidtype, int argc, char **argv, int clean, int noprompt)
{
	char devices[1500], newoptions[50];
	char *updatelunid;
	char errbuf[ERRMAX];
	char *po, *epo;

	if (strcmp(raidtype, "update") == 0) {
		switch(argc) {
		case 0:
			if (updatelunid = getupdatelun()) {
				werrstr("update LUN already active as LUN %s", updatelunid);
				return -1;
			}
			snprint(devices, sizeof devices, updatelunstr);
			break;
		default:
			if (getdevpath(argc, argv, devices, sizeof devices, clean) < 0)
				return -1;
			break;
		}
		snprint(raidtype, sizeof raidtype, "raw");
	} else if (getdevpath(argc, argv, devices, sizeof devices, clean) < 0)
			return -1;
	if (mklunwrite(options, lun, raidtype, devices) < 0)
		rerrstr(errbuf, ERRMAX);
	else
		return 0;

	/*error handling */
	if (strstr(errbuf, Ebadmagic)) {
		if (noprompt)
			return 0;
		po = newoptions;
		epo = po + sizeof newoptions;
		po = seprint(po, epo, options);
		print("warning: old LUN format detected.  Updating to a newer format will destroy\n");
		print("         existing data.  Please contact support if you are attempting to\n");
		print("         recover a failed LUN.  This operation cannot be reversed.\n");
		print("Would you like to update the LUN format?\n");
		switch (ask("", 1)) {
			case RespondYes:
				seprint(po, epo, "-V 1");
				break;
			case RespondNo:
				seprint(po, epo, "-V 0");
				break;
			case RespondQuit:
			default:
				werrstr("LUN creation aborted");
				return -1;	
		}
		memset(errbuf, 0, ERRMAX);
		errstr(errbuf, ERRMAX);
		return mklunwrite(newoptions, lun, raidtype, devices);
	} else if (strstr(errbuf, Ebadluncfg)) {
		print("error: invalid LUN format detected.  Please contact support if you\n");
		print("       are attempting to upgrade your appliance.\n");
		return 0;
	} else
		return -1;
}
