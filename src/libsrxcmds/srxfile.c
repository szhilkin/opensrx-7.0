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

int
srxwritefile(char *file, char *fmt, va_list arg)
{
	int fd, n;

	fd = open(file, OWRITE);
	if (fd < 0)
		return -1;
	n = vfprint(fd, fmt, arg);
	close(fd);
	return n;
}

int
srxtruncatefile(char *file)
{
	int fd;
	int n;

	fd = open(file, OWRITE);
	if (fd < 0)
		return -1;
	n = pwrite(fd, "", 0, 0);
	close(fd);
	return n;
}
