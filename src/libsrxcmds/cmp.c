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

/*
  * sparestrcmp is used to sort spareStr based on the name argument in the structure
  * the name must be a numerical string
  */
int
sparestrcmp(void *sa, void *sb)
{
	spareStr *inta, *intb;
	int a, b;

	inta = (spareStr *)sa;
	intb = (spareStr *)sb;
	a = atoi(inta->name);
	b = atoi(intb->name);
	if (a == b)
		return 0;
	return a < b ? -1 : 1;
}

/*
  * this cmp function is used in qsort and it only
  * sorts numeric directores when non-numeric and numeric directories
  * coexist. All non-numeric directoies will be dumped in a single bucket
  * without being sorted
  */
int
dirintcmp(void *d1, void *d2)
{
	Dir *aa, *bb;
	int a, b;

	aa = (Dir *)d1;
	bb = (Dir *)d2;
	if (!isdigit(aa->name[0])) {
		if (isdigit(bb->name[0]))
			return -1;
		else
			return 0;
	} else if (!isdigit(bb->name[0]))
		return 1;
	a = atoi(aa->name);
	b = atoi(bb->name);
	if (a == b)
		return 0;
	return a < b ? -1 : 1;
}
