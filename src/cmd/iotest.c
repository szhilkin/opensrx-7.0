// Copyright © 2010 Coraid, Inc.
// All rights reserved.
#include <u.h>
#include <libc.h>
#include <ctype.h>

#define dprint(...) if (dflag == 0) USED(dflag); else fprint(2, __VA_ARGS__);

enum {
	Nmaxbsz= 1<<22,
	Ndbsz= 1<<16,
};
int iosecs = 30;
int iofd;
int iosz = Ndbsz;
uchar iobuf[Nmaxbsz];
int dflag;

void
usage(void)
{
	fprint(2, "usage: %s /path/to/data\n", argv0);
	exits(0);
}

void
io(char *op)
{
	ulong s, cs;
	char *lp, *p;
	ulong boff, nr, nw;
	int n, rel, nloop;

	s = time(0);
	nr = nw = boff = cs = 0;
	lp = nil;
	nloop = -1;
	for (p=op; *p && cs < iosecs; cs = time(0) - s, p++) {
		n = rel = 0;
		if (*p == '-') {
			p++;
			rel = -1;
		}
		if (*p == '+') {
			p++;
			rel = 1;
		}
		// I'd like to permit baseless conversion, but something like 0x100br stymies that.
		// should we restrict a-f as state machine chars to permit this?
		if (isdigit(*p))
			n = strtol(p, &p, 10);
loop:
		switch(*p) {
		case 'm':
			n *= 1024;
		case 'k':
			n *= 1024;
			p++;
			goto loop;
		case 'd':
			dflag = n;
			break;
		case 'b':		// set blocksize
			if (n <= 0 || n > Nmaxbsz) {
				fprint(2, "error: block size %d inappropriate [0-%d]\n", n, Nmaxbsz);
				return;
			}
			iosz = n;
			break;
		case 'w':
			do {
				dprint("w %ld\n", boff);
				pwrite(iofd, iobuf, iosz, (vlong)iosz*boff);
				boff++;
				nw++;
			} while (--n > 0);
			break;
		case 'r':
			do {
				dprint("r %ld\n", boff);
				pread(iofd, iobuf, iosz, (vlong)iosz*boff);
				boff++;
				nr++;
			} while (--n > 0);
			break;
		case 's':		// skip blocks
			boff += n ? n : 1;
			break;
		case 't':		// set timeout
			iosecs = n;
			break;
		case 'o':		// set offset
			if (rel)
				boff += n*rel;
			else
				boff = n;
			break;
		case ':':		// beginning of loop
			nloop = n ? n : -1;
			lp = p;
			break;
		case ';':		// end of loop
			if (lp == nil) {
				fprint(2, "error: end of loop (';') with no beginning\n");
				exits("badloop");
			}
			if (nloop < 0 || --nloop > 0)
				p = lp;
			break;
		case ' ':	// skip for formatting
			break;
		default:
			fprint(2, "unknown directive '%c' - bailing\n", *p);
			return;
		}
	}
	print("%d:%lds %ldr/%ldw %ldrps/%ldwps %ldrKBps/%ldwKBps\n", iosz, cs, nr, nw, nr/iosecs, nw/iosecs, nr/iosecs*iosz/1024, nw/iosecs*iosz/1024);
}

int
readln(int fd, char *buf, int len)
{
	int n;
	char ch;

	n = 0;
	while(n < len - 1) {
		if(read(fd, &ch, 1) != 1)
			exits(0);
		if((buf[n] = ch) == '\n')
			break;
		n++;
	}
	buf[n] = 0;
	return n;
}

void
main(int argc, char *argv[])
{
	char buf[64];

	ARGBEGIN {
	case 'd':
		dflag++;
		break;
	} ARGEND

	if (argc != 1)
		usage();

	iofd = open(*argv, ORDWR);
	if (iofd < 0) {
		fprint(2, "can't open %s: %r\n", *argv);
		exits("badf");
	}
	for (;;) {
		print("%s> ", argv0);
		if (readln(0, buf, sizeof buf) > 0)
			io(buf);
	}
}
