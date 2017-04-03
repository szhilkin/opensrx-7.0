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
lunlabelwrite(char *lun, char *name)
{
	char *b;
	int n;

	b = mustsmprint("/raid/%s/label", lun);
	n = srxwritefile(b, name, nil);
	free(b);
	return n;
}

int
rmlunlabel(char *lun)
{
	char *b;
	int n;

	b = mustsmprint("/raid/%s/label", lun);
	n = srxtruncatefile(b);
	free(b);
	return n;
}
