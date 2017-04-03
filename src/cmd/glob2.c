/*
 * expand i[-j].k[-l](,....)[:suffix]
 * Copyright © 2010 Coraid, Inc.
 * coraid, inc. all rights reserved
 */
#include <u.h>
#include <libc.h>
#include <bio.h>

Biobuf	o;

void
syn(void)
{
	exits("syntax");
}

int
ret0(void)
{
	return 0;
}

int
shelfno(void)
{
	char buf[10];
	int fd, n;
	static int s, once;

	if(once != 0)
		return s;
	once = 1;
	s = -1;
	fd = open("/n/kfs/srx/shelf", OREAD);
	n = read(fd, buf, sizeof buf - 1);
	if(n > 0){
		buf[n] = 0;
		s = atoi(buf);
	}
	close(fd);
	return s;
}

int
cvtname(char *s)
{
	char *p;
	int n;

	n = strtol(s, &p, 0);
	if(*p != 0)
		return -1;
	return n;
}

int
maxslot(void)
{
	int i, n, v, fd;
	Dir *d;
	static int m, once;

	if(once != 0)
		return m;
	once = 1;
	fd = open("/raiddev", OREAD);
	if(fd == -1)
		return m;
	d = nil;
	n = dirreadall(fd, &d);
	for(i = 0; i < n; i++){
		v = cvtname(d[i].name);
		if(v > m)
			m = v;
	}
	free(d);
	return m;			
}

int
up(int i)
{
	char *f[2], buf[64], *s;
	int isup;
	Biobuf *b;

	snprint(buf, sizeof buf, "/raiddev/%d/ctl", i);
	b = Bopen(buf, OREAD);
	if(b == nil)
		return 0;
	isup = 0;
	for(; s = Brdstr(b, '\n', 1); free(s))
		if(strncmp(s, "state", 5) == 0)
		if(gettokens(s, f, nelem(f), " \t:") == 2)
		if(strcmp(f[1], "up") == 0)
			isup = 1;
	Bterm(b);
	return isup;
}

void
spares(int n, int *a)
{
	char *f[3], *s, *p;
	int i;
	Biobuf *b;

	b = Bopen("/raid/spares", OREAD);
	if(b == nil)
		return;
	for(; s = Brdstr(b, '\n', 1); free(s))
		if(gettokens(s, f, nelem(f), "/") == 3){
			i = strtoul(f[1], &p, 0);
			if(*p == 0 && i < n)
				a[i] = 0;
		}
	Bterm(b);
}

int
raids(int r, int n, int *a)
{
	char buf[64], *f[6], *g[3], *s, *p;
	int i;
	Biobuf *b;

	snprint(buf, sizeof buf, "/raid/%d/raidstat", r);
	b = Bopen(buf, OREAD);
	if(b == nil)
		return -1;
	for(; s = Brdstr(b, '\n', 1); free(s))
		if(gettokens(s, f, nelem(f), " ") == 4)
		if(gettokens(f[3], g, nelem(g), "/") == 3){
			i = strtoul(g[1], &p, 0);
			if(*p == 0 && i < n)
				a[i] = 0;
		}
	Bterm(b);
	return 0;
}

int
slotarray(int **ar)
{
	int *a, n, i;

	n = maxslot();
	a = malloc(n * sizeof *a);
	for(i = 0; i < n; i++)
		a[i] = 1;

	for(i = 0; i < n; i++)
		if(!up(i))
			a[i] = 0;
	spares(n, a);
	for(i = 0; raids(i, n, a) != -1; i++)
		;
	*ar = a;
	return n;
}

char*
dash(char *s, int *i, int *j, int (*min)(void), int (*max)(void))
{
	char *p;
	int t;

	*i = strtol(s, &p, 0);
	if(p == s)
		*i = min();
	if(*p != '-'){
		*j = *i;
		return p;
	}
	s = p + 1;
	*j = strtol(s, &p, 0);
	if(p == s)
		*j = max();
	if(*j < *i){
		t = *i;
		*i = *j;
		*j = t;
	}
	return p;
}

void
dot(char *s, char *x)
{
	char *suffix;
	int a, b, i, j, k, l, *y;

	s = dash(s, &i, &j, shelfno, shelfno);
	if(*s != '.'){
		if(strcmp(s, "all") != 0)
			syn();
	}else
		s++;
	if(x)
		suffix = smprint(":%s", x);
	else
		suffix = smprint("");
	if(strcmp(s, "all") == 0){
		l = slotarray(&y);
		for(a = i; a <= j; a++)
			for(b = 0; b < l; b++)
				if(y[b])
					Bprint(&o, "%d.%d%s ", a, b, suffix);
		Bprint(&o, "\n");
		free(y);
	}else for(;;){
		s = dash(s, &k, &l, ret0, maxslot);
		for(a = i; a <= j; a++)
			for(b = k; b <= l; b++)
				Bprint(&o, "%d.%d%s ", a, b, suffix);
		Bprint(&o, "\n");
		if(*s == 0)
			break;
		if(*s != ',')
			syn();
		s++;
	}
	free(suffix);
}

void
suffix(char *s)
{
	char *f[2];

	memset(f, 0, sizeof f);
	gettokens(s, f, nelem(f), ":");
	dot(f[0], f[1]);
}

void
main(int argc, char **argv)
{
	ARGBEGIN{
	default:
		syn();
	}ARGEND

	Binit(&o, 1, OWRITE);
	for(; *argv; argv++)
		suffix(*argv);
	Bterm(&o);
	exits("");
}
