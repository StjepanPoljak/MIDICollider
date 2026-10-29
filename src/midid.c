/*
 * midid: MIDI to scsynth bridge.
 *
 * Reads raw MIDI from every /dev/snd/midiC*D* device (class-compliant USB
 * keyboards appear there through snd-usb-audio, so alsa-lib isn't needed)
 * and turns notes into scsynth nodes, sending OSC to localhost. For testing
 * without a keyboard, -m scans another directory (e.g. one with a FIFO).
 *
 * SynthDef convention (see tools/synthdefs.scd). Controls it may use:
 *   freq, note, vel (0..1), amp (vel squared), bend (semitones),
 *   gate (1 = held, 0 = release; the def must free itself),
 *   buf, rate (only when samples are configured),
 *   svel (0..1, the velocity the chosen sample was recorded at; vel/svel
 *   scales a velocity-layered sample between its neighbouring layers),
 *   plus any control named in a "cc" line of the config.
 * scsynth ignores controls a def doesn't have.
 *
 * Config: <data>/midid.conf, re-read whenever it changes (see data/midid.conf).
 *
 * Idle: after "idle" minutes without MIDI input and with no key held, we ask
 * init (SIGUSR1) to clock the CPUs down, and the next input asks it to clock
 * them up again (SIGUSR2). Only when started by init (MC_INIT_PID).
 *
 * Watchdog: if scsynth stops answering for 10 s without exiting, we ask init
 * to restart it (SIGRTMIN), again every minute until it answers.
 */
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "osc.h"

#define GROUP		1
#define NODE_FIRST	1000
#define MAX_DEVS	8
#define MAX_CC		32
#define MAX_SAMPLES	512
#define NAME		64
#define HUNG_AFTER	10.0		/* s without a reply before we call scsynth hung */
#define HUNG_RETRY	60.0		/* s before asking init again */
#define SIG_HUNG	SIGRTMIN	/* to init: scsynth is hung, restart it */

struct ccmap {
	int num;
	char param[32];
	float lo, hi;
	int exp;
};

struct sample {
	int note;
	int vel;		/* recorded at this velocity, 0 = any */
	char path[256];
};

static struct {
	char deflt[NAME];
	char synth[16][NAME];		/* per channel, "" = deflt */
	char program[128][NAME];
	struct ccmap cc[MAX_CC];
	int ncc;
	float bend_range;
	double idle;			/* seconds, 0 = never */
	struct sample sample[MAX_SAMPLES];
	int nsamples;
} cfg;

struct dev {
	int fd;
	char name[32];
	uint8_t status, data[2];
	int ndata, sysex;
};

static const char *data_dir = "/data";
static const char *dev_dir = "/dev/snd";
static char conf_path[512];
static int osc_fd;
static int server_up;
static double last_reply;		/* 0 = scsynth never answered */
static double asked_restart;		/* when we last reported scsynth hung */
static int32_t next_node = NODE_FIRST;
static pid_t init_pid;			/* 0 = not started by init: never idle */
static int idle;			/* init has clocked the CPUs down */
static double last_input;

static int32_t node[16][128];		/* sounding node per key, 0 = none */
static uint8_t sustained[16][128];	/* key released, held by the pedal */
static uint8_t pedal[16];
static float bend[16];			/* semitones */
static const char *cur_synth[16];	/* set by program change */
static float cc_value[MAX_CC];
static uint8_t cc_set[MAX_CC];

static struct dev devs[MAX_DEVS];
static int ndevs;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void tx(const osc_msg *m)
{
	/* ECONNREFUSED while scsynth is down is expected; the ping notices. */
	osc_send(osc_fd, m);
}

static void ctl(osc_msg *m, const char *name, float value)
{
	osc_s(m, name);
	osc_f(m, value);
}

static void n_set(int32_t id, const char *name, float value)
{
	osc_msg m;

	osc_init(&m, "/n_set");
	osc_i(&m, id);
	ctl(&m, name, value);
	tx(&m);
}

static const char *synth_for(int ch)
{
	if (cur_synth[ch])
		return cur_synth[ch];
	return cfg.synth[ch][0] ? cfg.synth[ch] : cfg.deflt;
}

static void load_config(void)
{
	char line[512];
	int ln = 0;
	FILE *f;

	memset(&cfg, 0, sizeof cfg);
	snprintf(cfg.deflt, sizeof cfg.deflt, "default");
	cfg.bend_range = 2;
	cfg.idle = 15 * 60;
	memset(cur_synth, 0, sizeof cur_synth);
	memset(cc_set, 0, sizeof cc_set);

	f = fopen(conf_path, "r");
	if (!f) {
		fprintf(stderr, "midid: %s: %s, using defaults\n", conf_path, strerror(errno));
		return;
	}
	while (fgets(line, sizeof line, f)) {
		char *argv[8], *hash = strchr(line, '#');
		int argc = 0;

		ln++;
		if (hash)
			*hash = 0;
		for (char *t = strtok(line, " \t\r\n"); t && argc < 8; t = strtok(NULL, " \t\r\n"))
			argv[argc++] = t;
		if (!argc)
			continue;

		if (!strcmp(argv[0], "synth") && argc == 2) {
			snprintf(cfg.deflt, NAME, "%s", argv[1]);
		} else if (!strcmp(argv[0], "synth") && argc == 3 && atoi(argv[1]) >= 1 && atoi(argv[1]) <= 16) {
			snprintf(cfg.synth[atoi(argv[1]) - 1], NAME, "%s", argv[2]);
		} else if (!strcmp(argv[0], "program") && argc == 3 && atoi(argv[1]) >= 0 && atoi(argv[1]) <= 127) {
			snprintf(cfg.program[atoi(argv[1])], NAME, "%s", argv[2]);
		} else if (!strcmp(argv[0], "cc") && argc >= 5 && cfg.ncc < MAX_CC) {
			struct ccmap *c = &cfg.cc[cfg.ncc++];

			c->num = atoi(argv[1]);
			snprintf(c->param, sizeof c->param, "%s", argv[2]);
			c->lo = strtof(argv[3], NULL);
			c->hi = strtof(argv[4], NULL);
			c->exp = argc > 5 && !strcmp(argv[5], "exp") && c->lo > 0 && c->hi > 0;
		} else if (!strcmp(argv[0], "bend") && argc == 2) {
			cfg.bend_range = strtof(argv[1], NULL);
		} else if (!strcmp(argv[0], "idle") && argc == 2) {
			cfg.idle = strtod(argv[1], NULL) * 60;
		} else if (!strcmp(argv[0], "sample") && (argc == 3 || argc == 4) && cfg.nsamples < MAX_SAMPLES) {
			struct sample *s = &cfg.sample[cfg.nsamples++];

			s->note = atoi(argv[1]);
			snprintf(s->path, sizeof s->path, "%s", argv[2]);
			s->vel = argc == 4 ? atoi(argv[3]) : 0;
		} else {
			fprintf(stderr, "midid: %s:%d: can't parse, ignored\n", conf_path, ln);
		}
	}
	fclose(f);
	fprintf(stderr, "midid: loaded %s (synth %s, %d cc, %d samples)\n",
		conf_path, cfg.deflt, cfg.ncc, cfg.nsamples);
}

/* Sends /b_allocRead for samples that aren't loaded yet (all of them with
 * fresh = 1) and frees the ones no longer configured. A big sample set takes
 * scsynth seconds to load, so a config edit only reloads what changed. */
static void load_buffers(int fresh)
{
	static char loaded[MAX_SAMPLES][256];
	static int nloaded;
	char path[768];
	osc_msg m;

	if (fresh)
		nloaded = 0;
	for (int i = 0; i < cfg.nsamples; i++) {
		if (i < nloaded && !strcmp(loaded[i], cfg.sample[i].path))
			continue;
		snprintf(loaded[i], sizeof loaded[i], "%s", cfg.sample[i].path);
		snprintf(path, sizeof path, "%s/%s", data_dir, cfg.sample[i].path);
		osc_init(&m, "/b_allocRead");
		osc_i(&m, i);
		osc_s(&m, path);
		tx(&m);
	}
	for (int i = cfg.nsamples; i < nloaded; i++) {
		osc_init(&m, "/b_free");
		osc_i(&m, i);
		tx(&m);
	}
	nloaded = cfg.nsamples;
}

/* The nearest sample in pitch; among equally near ones, the one recorded
 * nearest to this velocity. */
static int nearest_sample(int key, int vel)
{
	int best = -1, dist = 1000, vdist = 1000;

	for (int i = 0; i < cfg.nsamples; i++) {
		struct sample *s = &cfg.sample[i];
		int d = abs(key - s->note);
		int vd = s->vel ? abs(vel - s->vel) : 0;

		if (d < dist || (d == dist && vd < vdist)) {
			dist = d;
			vdist = vd;
			best = i;
		}
	}
	return best;
}

static void release(int ch, int key)
{
	if (node[ch][key])
		n_set(node[ch][key], "gate", 0);
	node[ch][key] = 0;
	sustained[ch][key] = 0;
}

static void all_off(int ch)
{
	for (int k = 0; k < 128; k++)
		release(ch, k);
}

static void note_on(int ch, int key, int vel)
{
	float v = vel / 127.0f;
	int32_t id;
	int s;
	osc_msg m;

	if (!server_up)
		return;
	if (node[ch][key])
		release(ch, key);
	id = next_node++;
	if (next_node == INT32_MAX)
		next_node = NODE_FIRST;

	osc_init(&m, "/s_new");
	osc_s(&m, synth_for(ch));
	osc_i(&m, id);
	osc_i(&m, 0);			/* add to head */
	osc_i(&m, GROUP);
	ctl(&m, "freq", 440.0f * powf(2.0f, (key - 69) / 12.0f));
	ctl(&m, "note", key);
	ctl(&m, "vel", v);
	ctl(&m, "amp", v * v);
	ctl(&m, "gate", 1);
	ctl(&m, "bend", bend[ch]);
	s = nearest_sample(key, vel);
	if (s >= 0) {
		ctl(&m, "buf", s);
		ctl(&m, "rate", powf(2.0f, (key - cfg.sample[s].note) / 12.0f));
		ctl(&m, "svel", cfg.sample[s].vel ? cfg.sample[s].vel / 127.0f : v);
	}
	for (int i = 0; i < cfg.ncc; i++)
		if (cc_set[i])
			ctl(&m, cfg.cc[i].param, cc_value[i]);
	tx(&m);

	node[ch][key] = id;
	sustained[ch][key] = 0;
}

static void note_off(int ch, int key)
{
	if (!node[ch][key])
		return;
	if (pedal[ch])
		sustained[ch][key] = 1;
	else
		release(ch, key);
}

static void control(int ch, int num, int val)
{
	if (num == 64) {
		pedal[ch] = val >= 64;
		if (!pedal[ch])
			for (int k = 0; k < 128; k++)
				if (sustained[ch][k])
					release(ch, k);
		return;
	}
	if (num == 120 || num == 123) {
		all_off(ch);
		return;
	}
	/* Mapped CCs are global: they apply to every channel's nodes. */
	for (int i = 0; i < cfg.ncc; i++) {
		struct ccmap *c = &cfg.cc[i];
		float t = val / 127.0f;

		if (c->num != num)
			continue;
		cc_value[i] = c->exp ? c->lo * powf(c->hi / c->lo, t) : c->lo + (c->hi - c->lo) * t;
		cc_set[i] = 1;
		if (server_up)
			n_set(GROUP, c->param, cc_value[i]);
	}
}

static void program(int ch, int p)
{
	if (!cfg.program[p][0]) {
		fprintf(stderr, "midid: program %d not mapped\n", p);
		return;
	}
	cur_synth[ch] = cfg.program[p];
	fprintf(stderr, "midid: channel %d -> %s\n", ch + 1, cur_synth[ch]);
}

static void pitch_bend(int ch, int value)
{
	bend[ch] = value / 8192.0f * cfg.bend_range;
	if (!server_up)
		return;
	for (int k = 0; k < 128; k++)
		if (node[ch][k])
			n_set(node[ch][k], "bend", bend[ch]);
}

static void dispatch(uint8_t status, uint8_t d1, uint8_t d2)
{
	int ch = status & 0x0f;

	switch (status & 0xf0) {
	case 0x80: note_off(ch, d1); break;
	case 0x90: if (d2) note_on(ch, d1, d2); else note_off(ch, d1); break;
	case 0xb0: control(ch, d1, d2); break;
	case 0xc0: program(ch, d1); break;
	case 0xe0: pitch_bend(ch, ((d2 << 7) | d1) - 8192); break;
	}
}

/* Ends idle on the first input, before the input is handled. */
static void input_seen(void)
{
	last_input = now();
	if (!idle)
		return;
	idle = 0;
	kill(init_pid, SIGUSR2);
	fprintf(stderr, "midid: input, waking up\n");
}

static int any_key_down(void)
{
	for (int ch = 0; ch < 16; ch++)
		for (int k = 0; k < 128; k++)
			if (node[ch][k])
				return 1;
	return 0;
}

static void check_idle(double t)
{
	if (idle || !init_pid || cfg.idle <= 0 || !server_up)
		return;
	if (t - last_input < cfg.idle || any_key_down())
		return;
	idle = 1;
	kill(init_pid, SIGUSR1);
	fprintf(stderr, "midid: no input for %g min, going idle\n", cfg.idle / 60);
}

static void midi_byte(struct dev *d, uint8_t b)
{
	int need;

	if (b >= 0xf8)			/* realtime: clock, active sensing... */
		return;
	input_seen();
	if (b == 0xf0) {
		d->sysex = 1;
		return;
	}
	if (b & 0x80) {
		d->sysex = 0;
		d->status = b < 0xf0 ? b : 0;	/* drop system common messages */
		d->ndata = 0;
		return;
	}
	if (d->sysex || !d->status)
		return;
	d->data[d->ndata++] = b;
	need = ((d->status & 0xf0) == 0xc0 || (d->status & 0xf0) == 0xd0) ? 1 : 2;
	if (d->ndata == need) {
		dispatch(d->status, d->data[0], d->data[1]);
		d->ndata = 0;		/* keep status for running status */
	}
}

static int warned(const char *name)
{
	static char seen[16][32];
	static int nseen;

	for (int i = 0; i < nseen; i++)
		if (!strcmp(seen[i], name))
			return 1;
	if (nseen < 16)
		snprintf(seen[nseen++], sizeof seen[0], "%.31s", name);
	return 0;
}

static void scan_devices(void)
{
	struct dirent *e;
	DIR *dir = opendir(dev_dir);

	if (!dir)
		return;
	while ((e = readdir(dir)) && ndevs < MAX_DEVS) {
		char path[300];
		int fd, open_already = 0;

		if (strncmp(e->d_name, "midiC", 5))
			continue;
		for (int i = 0; i < ndevs; i++)
			if (!strcmp(devs[i].name, e->d_name))
				open_already = 1;
		if (open_already)
			continue;
		snprintf(path, sizeof path, "%s/%s", dev_dir, e->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			if (!warned(e->d_name))
				fprintf(stderr, "midid: %s: %s\n", path, strerror(errno));
			continue;
		}
		memset(&devs[ndevs], 0, sizeof devs[ndevs]);
		devs[ndevs].fd = fd;
		snprintf(devs[ndevs].name, sizeof devs[ndevs].name, "%.31s", e->d_name);
		ndevs++;
		fprintf(stderr, "midid: opened %s\n", path);
	}
	closedir(dir);
}

static void close_dev(int i)
{
	fprintf(stderr, "midid: closed %s/%s\n", dev_dir, devs[i].name);
	close(devs[i].fd);
	devs[i] = devs[--ndevs];
	/* Whatever was held on the unplugged keyboard would hang forever. */
	for (int ch = 0; ch < 16; ch++)
		all_off(ch);
}

/* A fresh scsynth (first start, or restarted by init): create our group
 * and load the samples. */
static void setup_server(void)
{
	osc_msg m;

	fprintf(stderr, "midid: setting up scsynth\n");
	memset(node, 0, sizeof node);
	memset(sustained, 0, sizeof sustained);
	osc_init(&m, "/g_new");
	osc_i(&m, GROUP);
	osc_i(&m, 0);
	osc_i(&m, 0);
	tx(&m);
	load_buffers(1);
}

/* The once-a-second ping is "/g_queryTree <GROUP>": scsynth answers it right
 * away (/status would wait behind queued /b_allocReads), with the tree if
 * our group exists and a /fail if scsynth is fresh and needs setting up. */
static void handle_replies(void)
{
	static char buf[65536];
	ssize_t n;

	while ((n = recv(osc_fd, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
		const char *s[2], *addr = osc_parse(buf, n, s, 2);
		int fresh;

		if (!addr)
			continue;
		fresh = !strcmp(addr, "/fail") && s[0] && !strcmp(s[0], "/g_queryTree");
		if (fresh || !strcmp(addr, "/g_queryTree.reply")) {
			last_reply = now();
			if (!server_up)
				fprintf(stderr, "midid: scsynth is up\n");
			server_up = 1;
			if (fresh)
				setup_server();
		} else if (!strcmp(addr, "/fail")) {
			/* A node freed itself before we set it. */
			if (s[1] && strstr(s[1], "not found"))
				continue;
			fprintf(stderr, "midid: scsynth: %s: %s\n", s[0] ? s[0] : "?", s[1] ? s[1] : "?");
		}
	}
}

/* scsynth runs our ping from its audio thread, so no reply means its audio
 * has stopped. One that exits is restarted by init, but one that hangs
 * stays silent for good: ask init to restart it. Only once it has answered,
 * so a slow start isn't a hang. (Seen once after a night with scsynth paused
 * by SIGSTOP, which idle no longer does.) */
static void check_hung(double t)
{
	if (!init_pid || !last_reply || server_up)
		return;
	if (t - last_reply < HUNG_AFTER || t - asked_restart < HUNG_RETRY)
		return;
	asked_restart = t;
	kill(init_pid, SIG_HUNG);
	fprintf(stderr, "midid: no reply from scsynth for %.0f s, asking init to restart it\n",
		t - last_reply);
}

int main(int argc, char **argv)
{
	struct stat st;
	struct stat conf_st = { 0 };
	double last_tick = 0;
	int port = 57110, opt;

	while ((opt = getopt(argc, argv, "d:m:p:")) != -1) {
		switch (opt) {
		case 'd': data_dir = optarg; break;
		case 'm': dev_dir = optarg; break;
		case 'p': port = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: midid [-d datadir] [-m mididir] [-p scsynth-port]\n");
			return 2;
		}
	}
	snprintf(conf_path, sizeof conf_path, "%s/midid.conf", data_dir);
	if (getenv("MC_INIT_PID") && atoi(getenv("MC_INIT_PID")) == getppid())
		init_pid = getppid();
	last_input = now();
	osc_fd = osc_open(port);
	if (osc_fd < 0) {
		perror("midid: socket");
		return 1;
	}
	if (stat(conf_path, &conf_st) < 0)
		memset(&conf_st, 0, sizeof conf_st);
	load_config();

	for (;;) {
		struct pollfd pfd[MAX_DEVS + 1];
		double t = now();

		if (t - last_tick >= 1.0) {
			osc_msg m;

			last_tick = t;
			scan_devices();
			if (stat(conf_path, &st) < 0)
				memset(&st, 0, sizeof st);
			if (st.st_mtime != conf_st.st_mtime || st.st_size != conf_st.st_size ||
			    st.st_ino != conf_st.st_ino) {
				conf_st = st;
				load_config();
				if (server_up)
					load_buffers(0);
			}
			check_idle(t);
			osc_init(&m, "/g_queryTree");
			osc_i(&m, GROUP);
			osc_i(&m, 0);
			tx(&m);
			if (server_up && t - last_reply > 3.0) {
				fprintf(stderr, "midid: lost scsynth\n");
				server_up = 0;
			}
			check_hung(t);
		}

		pfd[0].fd = osc_fd;
		pfd[0].events = POLLIN;
		for (int i = 0; i < ndevs; i++) {
			pfd[i + 1].fd = devs[i].fd;
			pfd[i + 1].events = POLLIN;
		}
		if (poll(pfd, ndevs + 1, 250) < 0)
			continue;
		if (pfd[0].revents)
			handle_replies();
		/* Backwards, so close_dev() moving the last entry into i is safe. */
		for (int i = ndevs - 1; i >= 0; i--) {
			uint8_t buf[256];
			ssize_t r;

			if (!pfd[i + 1].revents)
				continue;
			r = read(devs[i].fd, buf, sizeof buf);
			if (r > 0) {
				for (ssize_t j = 0; j < r; j++)
					midi_byte(&devs[i], buf[j]);
			} else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
				close_dev(i);
			}
		}
	}
}
