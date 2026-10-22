/*
 * recvd: file upload service for the data partition.
 *
 * One command per TCP connection. The header is a single text line:
 *
 *   PUT <size> <path>\n<size bytes>   store a file (temp file + fsync + rename)
 *   DEL <path>\n                      remove a file
 *   LS [<dir>]\n                      list a directory
 *   GET <path>\n                      fetch a file: "OK <size>\n<size bytes>"
 *   RESTART\n                         ask init to restart scsynth and midid
 *
 * Paths are relative to the data directory ([A-Za-z0-9._-/] only, no "..").
 * The server replies "OK ..." or "ERR <reason>" and closes the connection.
 * PUT synthdefs/<name>.scsyndef also loads the def into scsynth right away,
 * and DEL frees it.
 *
 * There's no authentication. It's meant for a cable between the Pi and a laptop.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "osc.h"

#define MAX_UPLOAD (1LL << 30)

static const char *data_dir = "/data";
static int osc_fd;

static void reply(int fd, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vdprintf(fd, fmt, ap);
	va_end(ap);
}

static int read_line(int fd, char *buf, size_t size)
{
	size_t n = 0;

	/* Byte at a time so we never read past the header into the payload. */
	while (n + 1 < size) {
		char c;

		if (read(fd, &c, 1) != 1)
			return -1;
		if (c == '\n') {
			if (n && buf[n - 1] == '\r')
				n--;
			buf[n] = 0;
			return 0;
		}
		buf[n++] = c;
	}
	return -1;
}

static int valid_path(const char *p)
{
	if (!*p || *p == '/' || strlen(p) > 200)
		return 0;
	for (const char *c = p; *c; c++)
		if (!isalnum((unsigned char)*c) && !strchr("._-/", *c))
			return 0;
	while (*p) {
		size_t len = strcspn(p, "/");

		if (len == 0 || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.'))
			return 0;
		p += len;
		if (*p == '/')
			p++;
	}
	return 1;
}

static int is_synthdef(const char *rel)
{
	size_t len = strlen(rel);

	return !strncmp(rel, "synthdefs/", 10) && len > 9 && !strcmp(rel + len - 9, ".scsyndef");
}

static void mkdir_parents(char *path)
{
	for (char *p = path + strlen(data_dir) + 1; (p = strchr(p, '/')); p++) {
		*p = 0;
		mkdir(path, 0755);
		*p = '/';
	}
}

static void sync_parent(const char *path)
{
	char dir[512];
	char *slash;
	int fd;

	snprintf(dir, sizeof dir, "%s", path);
	slash = strrchr(dir, '/');
	if (!slash)
		return;
	*slash = 0;
	fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

static int write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t w = write(fd, buf, len);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		buf += w;
		len -= w;
	}
	return 0;
}

static void do_put(int c, long long size, const char *rel)
{
	static char buf[65536];
	char path[512], tmp[520];
	long long left = size;
	int out;

	snprintf(path, sizeof path, "%s/%s", data_dir, rel);
	snprintf(tmp, sizeof tmp, "%s.part", path);
	mkdir_parents(path);
	out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		reply(c, "ERR %s\n", strerror(errno));
		return;
	}
	while (left > 0) {
		ssize_t r = read(c, buf, left < (long long)sizeof buf ? (size_t)left : sizeof buf);

		if (r <= 0) {
			close(out);
			unlink(tmp);
			reply(c, "ERR short upload, %lld bytes missing\n", left);
			return;
		}
		if (write_all(out, buf, r) < 0) {
			reply(c, "ERR %s\n", strerror(errno));
			close(out);
			unlink(tmp);
			return;
		}
		left -= r;
	}
	if (fsync(out) < 0 || close(out) < 0 || rename(tmp, path) < 0) {
		reply(c, "ERR %s\n", strerror(errno));
		unlink(tmp);
		return;
	}
	sync_parent(path);
	if (is_synthdef(rel)) {
		osc_msg m;

		osc_init(&m, "/d_load");
		osc_s(&m, path);
		osc_send(osc_fd, &m);
	}
	reply(c, "OK %lld\n", size);
}

static void do_del(int c, const char *rel)
{
	char path[512];

	snprintf(path, sizeof path, "%s/%s", data_dir, rel);
	if (unlink(path) < 0) {
		reply(c, "ERR %s\n", strerror(errno));
		return;
	}
	sync_parent(path);
	if (is_synthdef(rel)) {
		char name[256];
		osc_msg m;

		snprintf(name, sizeof name, "%s", strrchr(rel, '/') + 1);
		name[strlen(name) - 9] = 0;
		osc_init(&m, "/d_free");
		osc_s(&m, name);
		osc_send(osc_fd, &m);
	}
	reply(c, "OK\n");
}

static void do_ls(int c, const char *rel)
{
	char path[512];
	struct dirent *e;
	DIR *dir;

	snprintf(path, sizeof path, "%s/%s", data_dir, rel);
	dir = opendir(path);
	if (!dir) {
		reply(c, "ERR %s\n", strerror(errno));
		return;
	}
	while ((e = readdir(dir))) {
		char full[800];
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		snprintf(full, sizeof full, "%s/%s", path, e->d_name);
		if (stat(full, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode))
			reply(c, "%12s %s/\n", "-", e->d_name);
		else
			reply(c, "%12lld %s\n", (long long)st.st_size, e->d_name);
	}
	closedir(dir);
	reply(c, "OK\n");
}

static void do_get(int c, const char *rel)
{
	static char buf[65536];
	char path[512];
	struct stat st;
	ssize_t n;
	int fd;

	snprintf(path, sizeof path, "%s/%s", data_dir, rel);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		reply(c, "ERR %s\n", fd < 0 ? strerror(errno) : "not a file");
		if (fd >= 0)
			close(fd);
		return;
	}
	/* The size is fixed up front; a growing file (log.txt) is cut there. */
	reply(c, "OK %lld\n", (long long)st.st_size);
	for (off_t left = st.st_size; left > 0; left -= n) {
		n = read(fd, buf, left < (off_t)sizeof buf ? (size_t)left : sizeof buf);
		if (n <= 0 || write_all(c, buf, n) < 0)
			break;
	}
	close(fd);
}

static void handle(int c)
{
	char line[512], *cmd, *a1, *a2;

	if (read_line(c, line, sizeof line) < 0) {
		reply(c, "ERR bad header\n");
		return;
	}
	cmd = strtok(line, " ");
	a1 = strtok(NULL, " ");
	a2 = strtok(NULL, " ");
	if (!cmd) {
		reply(c, "ERR empty command\n");
		return;
	}

	if (!strcmp(cmd, "PUT") && a1 && a2) {
		char *end;
		long long size = strtoll(a1, &end, 10);

		if (*end || size < 0 || size > MAX_UPLOAD)
			reply(c, "ERR bad size\n");
		else if (!valid_path(a2))
			reply(c, "ERR bad path\n");
		else {
			fprintf(stderr, "recvd: PUT %s (%lld bytes)\n", a2, size);
			do_put(c, size, a2);
		}
	} else if (!strcmp(cmd, "DEL") && a1) {
		if (!valid_path(a1))
			reply(c, "ERR bad path\n");
		else {
			fprintf(stderr, "recvd: DEL %s\n", a1);
			do_del(c, a1);
		}
	} else if (!strcmp(cmd, "LS")) {
		if (a1 && !valid_path(a1))
			reply(c, "ERR bad path\n");
		else
			do_ls(c, a1 ? a1 : "");
	} else if (!strcmp(cmd, "GET") && a1) {
		if (!valid_path(a1))
			reply(c, "ERR bad path\n");
		else
			do_get(c, a1);
	} else if (!strcmp(cmd, "RESTART")) {
		/* Only our own init: on a laptop, PID 1 is systemd. */
		if (getppid() != 1)
			reply(c, "ERR not running under MIDICollider init\n");
		else if (kill(1, SIGHUP) < 0)
			reply(c, "ERR %s\n", strerror(errno));
		else {
			fprintf(stderr, "recvd: RESTART\n");
			reply(c, "OK\n");
		}
	} else {
		reply(c, "ERR unknown command\n");
	}
}

int main(int argc, char **argv)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
	int port = 9000, scport = 57110, one = 1, opt, ls;

	while ((opt = getopt(argc, argv, "d:p:s:")) != -1) {
		switch (opt) {
		case 'd': data_dir = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 's': scport = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: recvd [-d datadir] [-p port] [-s scsynth-port]\n");
			return 2;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	osc_fd = osc_open(scport);
	ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (ls < 0 || osc_fd < 0) {
		perror("recvd: socket");
		return 1;
	}
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	addr.sin_port = htons((uint16_t)port);
	if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(ls, 4) < 0) {
		perror("recvd: bind");
		return 1;
	}
	fprintf(stderr, "recvd: listening on port %d, data in %s\n", port, data_dir);

	for (;;) {
		struct timeval tv = { .tv_sec = 15 };
		int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);

		if (c < 0)
			continue;
		/* A stalled client must not block uploads forever. */
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		handle(c);
		close(c);
	}
}
