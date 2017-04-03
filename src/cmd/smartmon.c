/*
 * Copyright © 2013 Coraid, Inc.
 * All rights reserved.
 */

#include <u.h>
#include <libc.h>
#include <fis.h>
#include <json.h>
#include <rawrd.h>
#include <rawrdufops.h>
#include "srxcmds.h"

static Rawrd disks[36];

void
faildisk(Rawrd *d)
{
	int fd;

	fd = open("/raid/ctl", OWRITE);
	if (fd < 0) {
		print("error: can't open /raid/ctl: %r\n");
		return;
	}
	/*
	  * Rawrd path takes the form #‡/lunno
	  * We need to convert it to /raiddev/lunno/data
	  */
	fprint(fd, "failpath /raiddev%s/data", strchr(d->path, '/'));
	close(fd);
}

void
run(void)
{
	int i, status, *noted, shelf;
	Rawrd *d;

	shelf = getshelf();
	for(i=0; i<nelem(disks); i++) {
		d = &disks[i];
		if (rawrddevinit(d, shelf, i) < 0)
			break;
		if (rawrdopen(d) < 0)
			continue;
		if (d->type == Tunk)
			continue;
		if (d->type != Tata) {
			rawrdclose(d);
			continue;
		}
		status = Srsunk;
		rawrdatasmartrs(d, &status);
		rawrdclose(d);
		noted = (int *)&d->aux;
		switch (status) {
		case Srsunk:
		case Srsnormal:
			*noted = 0;
			break;
		case Srsthresh:
			/* generate a diagnostic every hour */
			if (*noted % 60 == 0)
				jsevent("msg:s:'warning: drive %s reports SMART threshold exceeded condition'"
					" sev:n:5 tag:s:SRX_SMART_THRESHOLD_EXCEEDED shelf:n:%d slot:n:%d",
					d->name, atoi(d->name), i);
			faildisk(d);
			(*noted)++;
		}
	}
}

void
main(int, char **)
{
	for (;;) {
		 sleep(60*1000);
		run();
	}
}
