/*
 * mahoganyctl FORM...: evaluate a Lisp form in the running Mahogany and print
 * the result. The socket is $MAHOGANY_SOCKET, else $XDG_RUNTIME_DIR/mahogany.sock,
 * else /var/run/user/<uid>/mahogany.sock.
 *   mahoganyctl '(lid-closed)'      mahoganyctl '(hrt:idle-inhibited-p)'
 */
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
	struct sockaddr_un sun = { .sun_family = AF_UNIX };
	const char *path = getenv("MAHOGANY_SOCKET");
	char def[sizeof(sun.sun_path)], buf[4096];
	if (argc < 2) { fprintf(stderr, "usage: mahoganyctl FORM...\n"); return 2; }
	if (!path) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		if (rt) snprintf(def, sizeof def, "%s/mahogany.sock", rt);
		else snprintf(def, sizeof def, "/var/run/user/%u/mahogany.sock", (unsigned)getuid());
		path = def;
	}
	strncpy(sun.sun_path, path, sizeof(sun.sun_path) - 1);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sun, sizeof sun) < 0) {
		fprintf(stderr, "mahoganyctl: cannot connect to %s\n", path); return 1;
	}
	struct timeval tv = { .tv_sec = 15 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	for (int i = 1; i < argc; i++) {
		if (i > 1) write(fd, " ", 1);
		write(fd, argv[i], strlen(argv[i]));
	}
	write(fd, "\n", 1);
	shutdown(fd, SHUT_WR);
	ssize_t n;
	int err = 0;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		if (n >= 6 && !strncmp(buf, "ERROR:", 6)) err = 1;
		fwrite(buf, 1, n, stdout);
	}
	close(fd);
	return err;
}
