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

static void
notifyfunc(void *, char *s)
{
	if (strcmp(s, "interrupt") == 0)
		noted(NCONT);
	noted(NDFLT);
}

int
shellcmd(char *s)
{
	Waitmsg *w;
	char err[ERRMAX];
	char buf[80];
	char *argv[40];
	int argc, pid, ret;

	argc = tokenize(s, argv, nelem(argv));
	argv[argc] = nil;
	ret = 0;
	switch (pid = fork()) {
	case -1:
		sysfatal("fork failed");
	case 0:
		snprint(buf, sizeof buf, "/bin/%s", argv[0]);
		exec(buf, argv);
		fprint(2, "%r\n");
		exits("exec");
	}
	notify(notifyfunc);
loop:
	if ((w = wait()) && w->pid == pid) {
		if (w->msg && w->msg[0])
			ret = -1;
		free(w);
	} else {
		errstr(err, sizeof err);
		if (strcmp(err, "interrupted") == 0 || (w && w->pid != pid)) {
			free(w);
			goto loop;
		}
		sysfatal("wait failed");
	}
	return ret;
}
