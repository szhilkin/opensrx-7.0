#include <u.h>
#include <libc.h>

#define BUFSZ	8192

void
usage(void)
{
	fprint(2, "usage: gen [-c value]\n");
	exits("usage");
}

void
main(int argc, char *argv[])
{
	int c;
	uchar buf[BUFSZ];

	c = 0xaa;
	ARGBEGIN {
	case 'c':
		c = atoi(EARGF(usage()));
		break;
	default:
		usage();
	} ARGEND

	memset(buf, c, sizeof buf);
	for (;;)
		write(1, buf, sizeof buf);
}
