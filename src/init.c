/*
 * init: PID 1 for MIDICollider.
 *
 * Sets up the bare minimum (proc, sys, dev, the two SD card partitions, the
 * CPU governor, a static IP on eth0) and then supervises three services:
 * scsynth, midid and recvd. A service that exits is restarted, with backoff
 * if it keeps dying. SIGHUP (sent by recvd's RESTART command) restarts
 * scsynth and midid, e.g. after uploading a new scsynth.args.
 *
 * Everything init and the services print goes to the console and, along
 * with the kernel log, to <data>/log.txt (the previous boot's is log.old.txt),
 * so a boot can be debugged without a serial cable: read the card on a
 * laptop, or fetch it with "tools/pi log". The green ACT LED switches to a
 * heartbeat once init is supervising.
 *
 * When not running as PID 1 it skips the system setup and only supervises,
 * which is handy for testing on a laptop: MC_DATA=./data ./build/init
 *
 * Kernel command line (cmdline.txt) parameters reach us as env variables:
 *   mc_ip=10.0.0.2/24   address for eth0, or "off"
 */
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BOOT_DEV	"/dev/mmcblk0p1"
#define DATA_DEV	"/dev/mmcblk0p2"
#define LOG_MAX		(4 << 20)	/* rotate log.txt beyond this */

struct service {
	const char *name;
	int rtprio;		/* SCHED_FIFO priority, 0 = normal scheduling */
	int restart_on_hup;
	pid_t pid;
	double started, next_start, backoff;
};

static struct service services[] = {
	{ .name = "scsynth", .rtprio = 70, .restart_on_hup = 1 },
	{ .name = "midid",   .rtprio = 60, .restart_on_hup = 1 },
	{ .name = "recvd" },
};
#define NSERVICES (sizeof services / sizeof services[0])

/* Used when <data>/scsynth.args doesn't exist. */
static const char *scsynth_defaults[] = {
	"-u", "57110", "-B", "127.0.0.1", "-R", "0",
	"-i", "0", "-o", "2", "-S", "48000", "-z", "64", "-Z", "128",
	NULL
};

static const char *data_dir;
static const char *net_ip;	/* pending eth0 address, NULL once done */
static int console_fd = -1;
static int log_rd = -1;		/* read end of our stdout/stderr pipe */
static pid_t logger_pid;
static double logger_next;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int wait_for(const char *path, int ms)
{
	struct timespec tick = { .tv_nsec = 50 * 1000000 };

	for (; ms > 0; ms -= 50) {
		if (access(path, F_OK) == 0)
			return 1;
		nanosleep(&tick, NULL);
	}
	return access(path, F_OK) == 0;
}

static void try_mount(const char *src, const char *dst, const char *type, unsigned long flags)
{
	if (mount(src, dst, type, flags, NULL) < 0)
		fprintf(stderr, "init: mount %s on %s: %s\n", src, dst, strerror(errno));
}

static void write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	if (write(fd, s, strlen(s)) < 0)
		fprintf(stderr, "init: %s: %s\n", path, strerror(errno));
	close(fd);
}

/* Bring an interface up, optionally assigning "a.b.c.d/prefix". */
static int if_config(const char *ifname, const char *cidr)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	int ok = 0;

	if (fd < 0)
		return 0;
	memset(&ifr, 0, sizeof ifr);
	snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
	if (cidr) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
		char addr[32];
		const char *slash = strchr(cidr, '/');
		int prefix = slash ? atoi(slash + 1) : 24;

		snprintf(addr, sizeof addr, "%.*s", slash ? (int)(slash - cidr) : (int)strlen(cidr), cidr);
		sin->sin_family = AF_INET;
		if (inet_pton(AF_INET, addr, &sin->sin_addr) != 1 || prefix < 0 || prefix > 32) {
			fprintf(stderr, "init: bad address %s\n", cidr);
			goto out;
		}
		if (ioctl(fd, SIOCSIFADDR, &ifr) < 0)
			goto fail;
		sin->sin_addr.s_addr = htonl(prefix ? ~0u << (32 - prefix) : 0);
		if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0)
			goto fail;
	}
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
		goto fail;
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
		goto fail;
	ok = 1;
	goto out;
fail:
	fprintf(stderr, "init: %s: %s\n", ifname, strerror(errno));
out:
	close(fd);
	return ok;
}

/* eth0 (lan78xx on the 3B+) shows up a few seconds after USB enumerates,
 * so it's configured from the main loop instead of delaying the services. */
static void poll_network(void)
{
	if (!net_ip || access("/sys/class/net/eth0", F_OK) < 0)
		return;
	if (if_config("eth0", net_ip))
		fprintf(stderr, "init: eth0 is %s\n", net_ip);
	net_ip = NULL;
}

static void log_open(const char *path, int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

/* The logger process: copies the pipe to the console and log.txt, adding
 * uptime stamps, and adds the kernel log (which reaches the console anyway). */
static void run_logger(void)
{
	char path[512], old[512], buf[4096];
	struct pollfd pfd[2] = { { .fd = log_rd, .events = POLLIN } };
	int out = -1, at_bol = 1;

	snprintf(path, sizeof path, "%s/log.txt", data_dir);
	snprintf(old, sizeof old, "%s/log.old.txt", data_dir);
	log_open(path, &out);
	pfd[1].fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	pfd[1].events = POLLIN;

	for (;;) {
		ssize_t n;

		if (poll(pfd, 2, -1) < 0)
			continue;
		if (pfd[0].revents) {
			n = read(log_rd, buf, sizeof buf);
			if (n > 0 && console_fd >= 0) {
				ssize_t w = write(console_fd, buf, n);

				(void)w;	/* no console: nothing to do */
			}
			for (ssize_t i = 0; i < n && out >= 0; ) {
				char *nl = memchr(buf + i, '\n', n - i);
				ssize_t len = nl ? nl - (buf + i) + 1 : n - i;

				if (at_bol) {
					struct timespec ts;

					clock_gettime(CLOCK_MONOTONIC, &ts);
					dprintf(out, "[%5ld.%06ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000);
				}
				if (write(out, buf + i, len) < 0)
					break;
				at_bol = nl != NULL;
				i += len;
			}
		}
		/* One record per read: "prio,seq,usec,flags;message\n key=value..." */
		while (pfd[1].revents && (n = read(pfd[1].fd, buf, sizeof buf - 1)) != 0) {
			unsigned long long us = 0;
			char *msg, *nl;

			if (n < 0) {
				if (errno == EPIPE)	/* overwritten before we got to it */
					continue;
				break;
			}
			buf[n] = 0;
			msg = strchr(buf, ';');
			if (!msg || out < 0)
				continue;
			if ((nl = strchr(++msg, '\n')))
				*nl = 0;
			sscanf(buf, "%*u,%*u,%llu", &us);
			dprintf(out, "[%5llu.%06llu] kernel: %s\n", us / 1000000, us % 1000000, msg);
		}
		if (out >= 0) {
			fdatasync(out);
			if (lseek(out, 0, SEEK_END) > LOG_MAX) {
				rename(path, old);
				log_open(path, &out);
			}
		}
	}
}

static void start_logger(void)
{
	pid_t pid = fork();

	if (pid == 0) {
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		run_logger();
	}
	logger_pid = pid > 0 ? pid : 0;
	logger_next = now() + 1;
}

static void led(const char *trigger)
{
	write_file("/sys/class/leds/ACT/trigger", trigger);
	write_file("/sys/class/leds/led0/trigger", trigger);
}

static void setup_system(void)
{
	static const char *dirs[] = { "/proc", "/sys", "/dev", "/tmp", "/boot", "/data" };
	char path[256], old[256];
	int fd_pair[2];

	for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
		mkdir(dirs[i], 0755);
	try_mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC);
	try_mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC);
	try_mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID);
	/* scsynth keeps its control buses in POSIX shared memory (shm_open) */
	mkdir("/dev/shm", 01777);
	try_mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV);
	try_mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV);

	/* The kernel only gives us a console if /dev/console was in the initramfs. */
	console_fd = open("/dev/console", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (console_fd >= 0) {
		dup2(console_fd, 0);
		dup2(console_fd, 1);
		dup2(console_fd, 2);
	}
	/* From here on stdout/stderr (inherited by the services) go into a pipe
	 * that the logger drains once /data is mounted. We keep the read end
	 * open, so writers never get EPIPE even while the logger restarts. */
	if (pipe2(fd_pair, O_CLOEXEC) == 0) {
		log_rd = fd_pair[0];
		dup2(fd_pair[1], 1);
		dup2(fd_pair[1], 2);
		close(fd_pair[1]);
	}
	fprintf(stderr, "init: MIDICollider starting\n");

	if (wait_for(BOOT_DEV, 5000))
		try_mount(BOOT_DEV, "/boot", "vfat", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC);
	else
		fprintf(stderr, "init: no %s\n", BOOT_DEV);
	if (wait_for(DATA_DEV, 1000))
		try_mount(DATA_DEV, "/data", "vfat", MS_NOATIME | MS_NOSUID | MS_NODEV | MS_NOEXEC);
	else
		fprintf(stderr, "init: no %s, uploads will be lost on reboot\n", DATA_DEV);
	if (log_rd >= 0) {
		snprintf(path, sizeof path, "%s/log.txt", data_dir);
		snprintf(old, sizeof old, "%s/log.old.txt", data_dir);
		rename(path, old);
		start_logger();
	}

	for (int cpu = 0; cpu < 4; cpu++) {
		snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
		write_file(path, "performance");
	}
	if_config("lo", NULL);
	net_ip = getenv("mc_ip") ? getenv("mc_ip") : "10.0.0.2/24";
	if (!strcmp(net_ip, "off"))
		net_ip = NULL;

	setenv("PATH", "/bin:/sbin:/usr/bin", 1);
	setenv("HOME", "/", 1);
}

/* One argument per line, so device names with spaces work. */
static void scsynth_argv(char **argv, int max)
{
	static char buf[4096];
	char path[512], line[512];
	size_t used = 0;
	int argc = 0;
	FILE *f;

	argv[argc++] = "scsynth";
	snprintf(path, sizeof path, "%s/scsynth.args", data_dir);
	f = fopen(path, "r");
	if (!f) {
		for (int i = 0; scsynth_defaults[i] && argc < max - 1; i++)
			argv[argc++] = (char *)scsynth_defaults[i];
		argv[argc] = NULL;
		return;
	}
	while (fgets(line, sizeof line, f) && argc < max - 1) {
		char *t = line, *end;
		size_t len;

		while (*t == ' ' || *t == '\t')
			t++;
		end = t + strlen(t);
		while (end > t && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
			*--end = 0;
		if (!*t || *t == '#')
			continue;
		len = strlen(t) + 1;
		if (used + len > sizeof buf)
			break;
		memcpy(buf + used, t, len);
		argv[argc++] = buf + used;
		used += len;
	}
	fclose(f);
	argv[argc] = NULL;
}

static void spawn(struct service *s)
{
	char *argv[64];
	pid_t pid;

	if (!strcmp(s->name, "scsynth")) {
		scsynth_argv(argv, 64);
	} else {
		argv[0] = (char *)s->name;
		argv[1] = "-d";
		argv[2] = (char *)data_dir;
		argv[3] = NULL;
	}

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "init: fork: %s\n", strerror(errno));
		s->next_start = now() + 1;
		return;
	}
	if (pid == 0) {
		sigset_t none;

		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		if (s->rtprio) {
			struct sched_param sp = { .sched_priority = s->rtprio };

			if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
				fprintf(stderr, "init: %s: no realtime priority: %s\n", s->name, strerror(errno));
		}
		execvp(argv[0], argv);
		fprintf(stderr, "init: exec %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	s->pid = pid;
	s->started = now();
	fprintf(stderr, "init: started %s (pid %d)\n", s->name, (int)pid);
}

static void reap(void)
{
	pid_t pid;
	int st;

	/* Also collects orphans that got reparented to us. */
	while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
		if (pid == logger_pid)
			logger_pid = 0;
		for (size_t i = 0; i < NSERVICES; i++) {
			struct service *s = &services[i];
			double t = now();

			if (s->pid != pid)
				continue;
			if (WIFSIGNALED(st))
				fprintf(stderr, "init: %s killed by signal %d\n", s->name, WTERMSIG(st));
			else
				fprintf(stderr, "init: %s exited with %d\n", s->name, WEXITSTATUS(st));
			s->pid = 0;
			if (t - s->started < 5) {
				s->backoff = s->backoff ? s->backoff * 2 : 1;
				if (s->backoff > 30)
					s->backoff = 30;
			} else {
				s->backoff = 0;
			}
			s->next_start = t + s->backoff;
		}
	}
}

int main(void)
{
	sigset_t set;

	data_dir = getenv("MC_DATA") ? getenv("MC_DATA") : "/data";
	if (getpid() == 1)
		setup_system();
	if (!getenv("SC_SYNTHDEF_PATH")) {
		char path[512];

		snprintf(path, sizeof path, "%s/synthdefs", data_dir);
		mkdir(path, 0755);
		setenv("SC_SYNTHDEF_PATH", path, 1);
	}

	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	sigaddset(&set, SIGHUP);
	sigprocmask(SIG_BLOCK, &set, NULL);
	if (getpid() == 1)
		led("heartbeat");

	/* PID 1 must never return: the kernel panics if it does. */
	for (;;) {
		double t = now(), wait = 1.0;
		struct timespec ts;

		poll_network();
		if (log_rd >= 0 && !logger_pid) {
			if (t >= logger_next)
				start_logger();
			else if (logger_next - t < wait)
				wait = logger_next - t;
		}
		for (size_t i = 0; i < NSERVICES; i++) {
			struct service *s = &services[i];

			if (s->pid)
				continue;
			if (t >= s->next_start)
				spawn(s);
			else if (s->next_start - t < wait)
				wait = s->next_start - t;
		}

		ts.tv_sec = (time_t)wait;
		ts.tv_nsec = (long)((wait - ts.tv_sec) * 1e9);
		if (sigtimedwait(&set, NULL, &ts) == SIGHUP) {
			fprintf(stderr, "init: SIGHUP, restarting services\n");
			for (size_t i = 0; i < NSERVICES; i++)
				if (services[i].restart_on_hup && services[i].pid)
					kill(services[i].pid, SIGTERM);
		}
		reap();
	}
}
