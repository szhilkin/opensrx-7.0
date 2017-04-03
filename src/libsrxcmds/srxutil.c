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

int
isif(char *interface)
{
	char *path;
	Dir  *d;
	int  ret;

	ret = 0;
	path = smprint("/net/%s", interface);
	d = dirstat(path);
	if (d != nil)
		ret = 1;
	free(path);
	free(d);
	return ret;
}

int
iscm(void)
{
	Dir  *d;
	int  ret;

	ret = 0;
	d = dirstat("#S/sdS0");
	if (d != nil)
		ret = 1;
	free(d);
	return ret;
}

int
isslot(char *slot)
{
	Dir *d;
	char path[32];
	int ret;
	char *e;

	ret = 0;
	strtoul(slot, &e, 10);
	if (*e != '\0')
		return ret;
	snprint(path, sizeof path, "/raiddev/%s", slot);
	d = dirstat(path);
	if (d != nil)
		ret = 1;
	free(d);
	return ret;
}

/* Return true if lun exists */
int
islun(char *name)
{
	int fd;
	char *b;

	b = mustsmprint("/raid/%s/raidstat", name);
	fd = open(b, OREAD);
	free(b);
	if(fd < 0)
		return 0;
	close(fd);
	return 1;
}

/* Return true if lun is online */
int
islunonline(char *name)
{
	char buf[100], *args[10];

	if (readfile(buf, sizeof buf, "/raid/%s/stat", name) < 0)
		return 0;
	tokenize(buf, args, 10);
	if (strcmp(args[1], "online") == 0)
		return 1;
	else
		return 0;
}
