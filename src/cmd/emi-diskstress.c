// emi-diskstress

// sleep 1
// open the given file, keep reading until an error occurs.
// close the file, repeat

#include <u.h>
#include <libc.h>

char buf[65536];

void
stress(char *path) {
	int fd;
	long v;

	fd = open(path, OREAD);
	if (fd < 0)  {
		fprint(2,"emi-diskstress: %r");
		return;
	}
	while (1) {
		v = read(fd, buf, sizeof buf);
		if (v <= 0)  {
			break;
		}
	}
	close(fd);
}

void
main(int argc, char **argv) {
	
	if (argc != 2) {
		fprint(2,"emi-diskstress /raiddev/N/data");
		exits("usage");
	}
	argv++;
	while (1) {
		sleep(1000);
		stress(*argv);
	}
}
