/*
 * Minimal OSC 1.0 encoding/decoding: just enough to talk to scsynth over a
 * connected UDP socket on localhost. Header-only, no allocation.
 */
#ifndef OSC_H
#define OSC_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define OSC_MAX 2048

typedef struct {
	char addr[64];
	char tags[64];		/* ",ifs..." without terminator */
	size_t ntags;
	unsigned char args[OSC_MAX];
	size_t nargs;
	int overflow;
} osc_msg;

/* OSC strings are NUL-terminated and padded to a multiple of 4 bytes. */
static inline size_t osc_padded(size_t len)
{
	return (len + 4) & ~(size_t)3;
}

static inline void osc_init(osc_msg *m, const char *addr)
{
	snprintf(m->addr, sizeof m->addr, "%s", addr);
	m->tags[0] = ',';
	m->ntags = 1;
	m->nargs = 0;
	m->overflow = 0;
}

static inline void osc_put(osc_msg *m, char tag, const void *data, size_t len, size_t padded)
{
	if (m->ntags >= sizeof m->tags || m->nargs + padded > sizeof m->args) {
		m->overflow = 1;
		return;
	}
	m->tags[m->ntags++] = tag;
	memcpy(m->args + m->nargs, data, len);
	memset(m->args + m->nargs + len, 0, padded - len);
	m->nargs += padded;
}

static inline void osc_i(osc_msg *m, int32_t v)
{
	uint32_t be = htonl((uint32_t)v);
	osc_put(m, 'i', &be, 4, 4);
}

static inline void osc_f(osc_msg *m, float v)
{
	uint32_t u;
	memcpy(&u, &v, 4);
	u = htonl(u);
	osc_put(m, 'f', &u, 4, 4);
}

static inline void osc_s(osc_msg *m, const char *s)
{
	size_t len = strlen(s);
	osc_put(m, 's', s, len, osc_padded(len));
}

static inline ssize_t osc_send(int fd, const osc_msg *m)
{
	unsigned char buf[sizeof m->addr + sizeof m->tags + OSC_MAX + 8];
	size_t n = 0, len;

	if (m->overflow)
		return -1;
	len = strlen(m->addr);
	memset(buf, 0, sizeof m->addr + sizeof m->tags + 8);
	memcpy(buf, m->addr, len);
	n = osc_padded(len);
	memcpy(buf + n, m->tags, m->ntags);
	n += osc_padded(m->ntags);
	memcpy(buf + n, m->args, m->nargs);
	n += m->nargs;
	return send(fd, buf, n, 0);
}

/*
 * Returns the address of a received message (NULL if malformed) and fills
 * str[0..nstr-1] with its leading string arguments, or NULL where absent.
 * scsynth's /fail carries the failed command and the error message there.
 */
static inline const char *osc_parse(const char *buf, size_t len, const char **str, int nstr)
{
	size_t alen, tlen, off;
	const char *tags;
	int found = 0;

	for (int i = 0; i < nstr; i++)
		str[i] = NULL;
	alen = strnlen(buf, len);
	if (alen == len)
		return NULL;
	off = osc_padded(alen);
	if (off >= len)
		return buf;
	tags = buf + off;
	tlen = strnlen(tags, len - off);
	if (tlen == len - off || tags[0] != ',')
		return buf;
	off += osc_padded(tlen);
	for (size_t i = 1; i < tlen && off < len && found < nstr; i++) {
		if (tags[i] == 's') {
			size_t slen = strnlen(buf + off, len - off);
			if (slen == len - off)
				break;
			str[found++] = buf + off;
			off += osc_padded(slen);
		} else if (tags[i] == 'i' || tags[i] == 'f') {
			off += 4;
		} else {
			break;
		}
	}
	return buf;
}

/* UDP socket connected to scsynth on localhost. */
static inline int osc_open(int port)
{
	struct sockaddr_in to = {
		.sin_family = AF_INET,
		.sin_port = htons((uint16_t)port),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&to, sizeof to) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

#endif
