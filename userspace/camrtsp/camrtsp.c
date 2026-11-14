// camrtsp - always-on camera -> cedrus-enc H.264 -> RTSP/RTP server
//
//   camrtsp [-c /dev/video0] [-s 1920x1080] [-f 30] [-q 30] [-g 60] [-p 554]
//           [-b coded_kib] [-v]
//
// Pipeline: VIN NV12 buffers are exported as DMABUF and queued straight into
// the encoder (no CPU copy). The pipeline runs continuously so a new client
// does not pay sensor/encoder start-up; on PLAY a keyframe is forced and the
// client starts at that IDR (SPS/PPS are prepended to every IDR).
//
// RTSP: OPTIONS, DESCRIBE, SETUP (RTP/AVP/TCP interleaved or RTP/AVP UDP),
// PLAY, TEARDOWN, GET_PARAMETER/SET_PARAMETER (keep-alive). Any path is
// accepted (e.g. rtsp://camera/main); paths starting with /sub get the
// low-resolution sub-stream (-S, default 640x360, from /dev/video1). H.264 RTP per RFC 6184, packetization
// mode 1 (single NAL / FU-A). RTP timestamps come from the camera
// start-of-frame timestamp. A client that cannot keep up skips frames until
// the next IDR instead of accumulating latency.
//
// Audio (track1): mono S16 from the codec's MIC1 via tinyalsa, sent as RTP
// L16 (big-endian PCM, PT 97, 20 ms packets) or G.711 (-E pcmu|pcma, 8 kHz). Both tracks use
// CLOCK_MONOTONIC-based RTP timestamps; RTCP sender reports every 5 s map
// them to one NTP clock for lip sync. -A disables audio.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <linux/videodev2.h>
#include <linux/gpio.h>
#include <tinyalsa/asoundlib.h>
#include <linux/sun8i-eise.h>

#define NCAM		2	/* camera capture buffers */
#define NDEW		2	/* extra camera buffers used as dewarp outputs */
#define NCODED		2
#define MAXCLIENTS	4
#define RTP_MTU		1400
#define TXBUF		(512 * 1024)	/* per TCP client */
#define RXBUF		4096

static int verbose;

static void logf_(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "[%5ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
#define LOG(...) logf_(__VA_ARGS__)

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
	return r;
}

static double mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- base64 */

static void b64(const uint8_t *in, size_t n, char *out)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;

	for (i = 0; i + 2 < n; i += 3) {
		*out++ = t[in[i] >> 2];
		*out++ = t[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
		*out++ = t[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
		*out++ = t[in[i + 2] & 63];
	}
	if (i < n) {
		*out++ = t[in[i] >> 2];
		if (i + 1 < n) {
			*out++ = t[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
			*out++ = t[(in[i + 1] & 15) << 2];
		} else {
			*out++ = t[(in[i] & 3) << 4];
			*out++ = '=';
		}
		*out++ = '=';
	}
	*out = 0;
}

/* --------------------------------------------------------------- clients */

enum { C_FREE, C_RTSP, C_PLAYING };

enum { T_VIDEO, T_AUDIO, NTRACKS };

struct track {
	int setup;		/* SETUP done for this track */
	int ch;			/* interleaved RTP channel (RTCP = ch + 1) */
	int udp_fd, rtcp_fd;	/* UDP RTP / RTCP sockets */
	struct sockaddr_in udp_dst, rtcp_dst;
	uint32_t ssrc;
	uint16_t seq;
	uint32_t pkts, octets;	/* for RTCP sender reports */
};

struct client {
	int state;
	int fd;
	struct sockaddr_in peer;
	char rx[RXBUF];
	size_t rxlen;
	uint8_t *tx;		/* pending TCP output */
	size_t txlen, txoff;
	int tcp_interleaved;
	struct track t[NTRACKS];
	uint32_t session;
	int vs;			/* video stream index (vstreams[]) */
	int wait_idr;		/* skip until next IDR */
	double last_seen, last_sr;
	unsigned frames, skipped, apkts, askipped;
};

static struct client clients[MAXCLIENTS];
static unsigned W = 1920, H = 1080, FPS = 30;

/* an encoded video stream: 0 = main (rtsp://cam/main), 1 = sub (rtsp://cam/sub) */
struct vstream {
	int idx;
	unsigned w, h;
	uint8_t sps[128], pps[64];
	size_t sps_len, pps_len;
	int efd;
	int idr_requested;
};
static struct vstream vstreams[2] = { { .idx = 0, .efd = -1 }, { .idx = 1, .efd = -1 } };
static int sub_on;		/* sub-stream running */
static int efd = -1;		/* main encoder (= vstreams[0].efd) */

/* stream named by an RTSP url: ".../sub..." -> sub-stream when it runs */
static int url_stream(const char *url)
{
	const char *q = strstr(url, "://");
	q = q ? strchr(q + 3, '/') : url;
	return sub_on && q && !strncmp(q, "/sub", 4) ? 1 : 0;
}

/* audio: mono S16 from the V831 codec, sent as RTP L16 (RFC 3551) */
static int audio_pt = 97;	/* 97 = L16 (dynamic), 0 = PCMU, 8 = PCMA */
static const char *aenc_name = "L16";
static unsigned arate = 16000;
static int audio_on;		/* capture running -> audio track in SDP */
#define APKT_MS		20

static void client_init(struct client *c)
{
	memset(c, 0, sizeof(*c));
	c->fd = -1;
	for (int i = 0; i < NTRACKS; i++)
		c->t[i].udp_fd = c->t[i].rtcp_fd = -1;
}

static void client_close(struct client *c, const char *why)
{
	if (c->state == C_FREE)
		return;
	LOG("client %s:%u closed (%s), %u frames sent, %u skipped, %u audio packets, %u dropped",
	    inet_ntoa(c->peer.sin_addr), ntohs(c->peer.sin_port), why, c->frames, c->skipped,
	    c->apkts, c->askipped);
	close(c->fd);
	for (int i = 0; i < NTRACKS; i++) {
		if (c->t[i].udp_fd >= 0)
			close(c->t[i].udp_fd);
		if (c->t[i].rtcp_fd >= 0)
			close(c->t[i].rtcp_fd);
	}
	free(c->tx);
	client_init(c);
	c->state = C_FREE;
}

/* Queue bytes on the TCP connection; returns -1 if the buffer is full. */
static int tcp_queue(struct client *c, const void *p, size_t n)
{
	if (c->txlen + n > TXBUF)
		return -1;
	memcpy(c->tx + c->txlen, p, n);
	c->txlen += n;
	return 0;
}

static void tcp_flush(struct client *c)
{
	while (c->txoff < c->txlen) {
		ssize_t r = send(c->fd, c->tx + c->txoff, c->txlen - c->txoff,
				 MSG_NOSIGNAL | MSG_DONTWAIT);
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			client_close(c, strerror(errno));
			return;
		}
		c->txoff += r;
	}
	if (c->txoff == c->txlen)
		c->txoff = c->txlen = 0;
	else if (c->txoff > TXBUF / 2) {
		memmove(c->tx, c->tx + c->txoff, c->txlen - c->txoff);
		c->txlen -= c->txoff;
		c->txoff = 0;
	}
}

static void rtsp_reply(struct client *c, int code, const char *status, int cseq,
		       const char *extra, const char *body)
{
	char buf[2048];
	int n = snprintf(buf, sizeof(buf), "RTSP/1.0 %d %s\r\nCSeq: %d\r\nServer: camrtsp\r\n%s",
			 code, status, cseq, extra ? extra : "");
	if (body)
		n += snprintf(buf + n, sizeof(buf) - n, "Content-Type: application/sdp\r\n"
			      "Content-Length: %zu\r\n\r\n%s", strlen(body), body);
	else
		n += snprintf(buf + n, sizeof(buf) - n, "\r\n");
	if (tcp_queue(c, buf, n) == 0)
		tcp_flush(c);
	if (verbose)
		LOG("-> %d %s", code, status);
}

static const char *hdr(const char *req, const char *name)
{
	size_t l = strlen(name);
	for (const char *p = req; (p = strchr(p, '\n')); ) {
		p++;
		if (!strncasecmp(p, name, l) && p[l] == ':') {
			p += l + 1;
			while (*p == ' ')
				p++;
			return p;
		}
	}
	return NULL;
}

static void request_idr(struct vstream *v)
{
	struct v4l2_control ctl = { .id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, .value = 1 };
	if (v->efd >= 0 && xioctl(v->efd, VIDIOC_S_CTRL, &ctl))
		LOG("force keyframe (stream %d): %s", v->idx, strerror(errno));
	v->idr_requested = 1;
}

static void handle_request(struct client *c, char *req)
{
	char method[32], url[256];
	const char *h;
	int cseq = 0;

	if (sscanf(req, "%31s %255s", method, url) != 2)
		return;
	if ((h = hdr(req, "CSeq")))
		cseq = atoi(h);
	if (verbose)
		LOG("<- %s %s", method, url);
	c->last_seen = mono();

	if (!strcmp(method, "OPTIONS")) {
		rtsp_reply(c, 200, "OK", cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, "
			   "GET_PARAMETER, SET_PARAMETER\r\n", NULL);
	} else if (!strcmp(method, "DESCRIBE")) {
		char sdp[1400], s64[200], p64[100], extra[300];
		int n;
		struct sockaddr_in me;
		socklen_t ml = sizeof(me);

		struct vstream *v = &vstreams[c->vs = url_stream(url)];

		if (!v->sps_len) {
			rtsp_reply(c, 503, "Service Unavailable", cseq, "Retry-After: 1\r\n", NULL);
			return;
		}
		getsockname(c->fd, (struct sockaddr *)&me, &ml);
		b64(v->sps + 4, v->sps_len - 4, s64);
		b64(v->pps + 4, v->pps_len - 4, p64);
		n = snprintf(sdp, sizeof(sdp),
			 "v=0\r\no=- %u 1 IN IP4 %s\r\ns=camrtsp\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
			 "a=control:*\r\na=range:npt=0-\r\n"
			 "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
			 "a=fmtp:96 packetization-mode=1;profile-level-id=%02X%02X%02X;"
			 "sprop-parameter-sets=%s,%s\r\n"
			 "a=framerate:%u\r\na=x-dimensions:%u,%u\r\na=control:track0\r\n",
			 (unsigned)time(NULL), inet_ntoa(me.sin_addr), v->sps[5], v->sps[6],
			 v->sps[7], s64, p64, FPS, v->w, v->h);
		if (audio_on)
			snprintf(sdp + n, sizeof(sdp) - n,
				 "m=audio 0 RTP/AVP %d\r\na=rtpmap:%d %s/%u/1\r\na=ptime:%d\r\n"
				 "a=recvonly\r\na=control:track1\r\n",
				 audio_pt, audio_pt, aenc_name, arate, APKT_MS);
		snprintf(extra, sizeof(extra), "Content-Base: %s/\r\n", url);
		rtsp_reply(c, 200, "OK", cseq, extra, sdp);
	} else if (!strcmp(method, "SETUP")) {
		char extra[300];
		const char *t = hdr(req, "Transport");
		const char *slash = strrchr(url, '/');
		int ti = slash && !strncmp(slash + 1, "track1", 6) ? T_AUDIO : T_VIDEO;
		struct track *tr = &c->t[ti];

		if (!t || (ti == T_AUDIO && !audio_on)) {
			rtsp_reply(c, t ? 404 : 461, t ? "Not Found" : "Unsupported Transport",
				   cseq, NULL, NULL);
			return;
		}
		if (!c->session) {
			c->session = (uint32_t)rand() ^ ((uint32_t)rand() << 16);
			c->vs = url_stream(url);	/* SETUP without DESCRIBE */
		}
		tr->ssrc = (uint32_t)rand() ^ ((uint32_t)rand() << 16);
		tr->seq = (uint16_t)rand();
		tr->pkts = tr->octets = 0;
		if (strstr(t, "TCP")) {
			const char *il = strstr(t, "interleaved=");
			c->tcp_interleaved = 1;
			tr->ch = il ? atoi(il + 12) : 2 * ti;
			snprintf(extra, sizeof(extra),
				 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08X\r\n"
				 "Session: %08X;timeout=60\r\n", tr->ch, tr->ch + 1, tr->ssrc, c->session);
		} else {
			const char *cp = strstr(t, "client_port=");
			int port = cp ? atoi(cp + 12) : 0, sport = 0;

			if (!port) {
				rtsp_reply(c, 461, "Unsupported Transport", cseq, NULL, NULL);
				return;
			}
			/* RTP on an even port, RTCP on the next one (clients filter on it) */
			for (int tries = 0; tries < 20 && !sport; tries++) {
				int p0 = 20000 + 2 * (rand() % 10000);
				struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(p0) };
				int f0 = tr->udp_fd >= 0 ? tr->udp_fd : socket(AF_INET, SOCK_DGRAM, 0);
				int f1 = tr->rtcp_fd >= 0 ? tr->rtcp_fd : socket(AF_INET, SOCK_DGRAM, 0);
				tr->udp_fd = f0;
				tr->rtcp_fd = f1;
				if (bind(f0, (struct sockaddr *)&a, sizeof(a)))
					goto again;
				a.sin_port = htons(p0 + 1);
				if (bind(f1, (struct sockaddr *)&a, sizeof(a)))
					goto again;
				sport = p0;
				break;
			again:
				close(f0);
				close(f1);
				tr->udp_fd = tr->rtcp_fd = -1;
			}
			if (!sport) {
				rtsp_reply(c, 500, "Internal Server Error", cseq, NULL, NULL);
				return;
			}
			c->tcp_interleaved = 0;
			tr->udp_dst = c->peer;
			tr->udp_dst.sin_port = htons(port);
			tr->rtcp_dst = c->peer;
			tr->rtcp_dst.sin_port = htons(port + 1);
			snprintf(extra, sizeof(extra),
				 "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d;ssrc=%08X\r\n"
				 "Session: %08X;timeout=60\r\n", port, port + 1, sport, sport + 1,
				 tr->ssrc, c->session);
		}
		tr->setup = 1;
		rtsp_reply(c, 200, "OK", cseq, extra, NULL);
	} else if (!strcmp(method, "PLAY")) {
		char extra[600], base[260];
		int n;
		size_t bl = strlen(url);

		/* RTP-Info urls are the aggregate url + "/trackN" */
		snprintf(base, sizeof(base), "%s", url);
		if (bl && base[bl - 1] == '/')
			base[bl - 1] = 0;
		n = snprintf(extra, sizeof(extra), "Session: %08X\r\nRange: npt=0.000-\r\nRTP-Info: ",
			     c->session);
		for (int i = 0, first = 1; i < NTRACKS; i++) {
			if (!c->t[i].setup)
				continue;
			n += snprintf(extra + n, sizeof(extra) - n, "%surl=%s/track%d;seq=%u",
				      first ? "" : ",", base, i, c->t[i].seq);
			first = 0;
		}
		snprintf(extra + n, sizeof(extra) - n, "\r\n");
		rtsp_reply(c, 200, "OK", cseq, extra, NULL);
		c->state = C_PLAYING;
		c->last_sr = 0;
		c->wait_idr = 1;
		request_idr(&vstreams[c->vs]);
		LOG("client %s:%u playing %s (%s)", inet_ntoa(c->peer.sin_addr),
		    ntohs(c->peer.sin_port), c->vs ? "sub" : "main",
		    c->tcp_interleaved ? "TCP" : "UDP");
	} else if (!strcmp(method, "TEARDOWN")) {
		char extra[64];
		snprintf(extra, sizeof(extra), "Session: %08X\r\n", c->session);
		rtsp_reply(c, 200, "OK", cseq, extra, NULL);
		client_close(c, "teardown");
	} else if (!strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER")) {
		char extra[64];
		snprintf(extra, sizeof(extra), "Session: %08X\r\n", c->session);
		rtsp_reply(c, 200, "OK", cseq, extra, NULL);
	} else {
		rtsp_reply(c, 501, "Not Implemented", cseq, NULL, NULL);
	}
}

static void client_read(struct client *c)
{
	ssize_t r = recv(c->fd, c->rx + c->rxlen, sizeof(c->rx) - c->rxlen - 1, MSG_DONTWAIT);
	if (r <= 0) {
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		client_close(c, r ? strerror(errno) : "eof");
		return;
	}
	c->rxlen += r;
	c->last_seen = mono();
	for (;;) {
		if (c->rxlen && c->rx[0] == '$') {	/* interleaved RTCP from client */
			if (c->rxlen < 4)
				return;
			size_t n = 4 + ((uint8_t)c->rx[2] << 8 | (uint8_t)c->rx[3]);
			if (c->rxlen < n)
				return;
			memmove(c->rx, c->rx + n, c->rxlen - n);
			c->rxlen -= n;
			continue;
		}
		c->rx[c->rxlen] = 0;
		char *end = strstr(c->rx, "\r\n\r\n");
		if (!end) {
			if (c->rxlen >= sizeof(c->rx) - 1)
				client_close(c, "request too long");
			return;
		}
		size_t n = end + 4 - c->rx;
		const char *cl = hdr(c->rx, "Content-Length");
		if (cl)
			n += atoi(cl);
		if (c->rxlen < n)
			return;
		char save = c->rx[n];
		c->rx[n] = 0;
		handle_request(c, c->rx);
		if (c->state == C_FREE)
			return;
		c->rx[n] = save;
		memmove(c->rx, c->rx + n, c->rxlen - n);
		c->rxlen -= n;
	}
}

/* ---------------------------------------------------------------- RTP out */

static int interleaved_send(struct client *c, int ch, const uint8_t *pkt, size_t len)
{
	uint8_t h[4] = { '$', ch, len >> 8, len };
	if (c->txlen + 4 + len > TXBUF)
		return -1;
	tcp_queue(c, h, 4);
	return tcp_queue(c, pkt, len);
}

static int rtp_send(struct client *c, int ti, int pt, const uint8_t *hdr2, size_t h2,
		    const uint8_t *p, size_t n, int marker, uint32_t ts)
{
	struct track *tr = &c->t[ti];
	uint8_t rtp[12 + 2 + RTP_MTU];
	size_t len = 12 + h2 + n;

	rtp[0] = 0x80;
	rtp[1] = (marker ? 0x80 : 0) | pt;
	rtp[2] = tr->seq >> 8;
	rtp[3] = tr->seq;
	rtp[4] = ts >> 24; rtp[5] = ts >> 16; rtp[6] = ts >> 8; rtp[7] = ts;
	rtp[8] = tr->ssrc >> 24; rtp[9] = tr->ssrc >> 16; rtp[10] = tr->ssrc >> 8; rtp[11] = tr->ssrc;
	if (h2)
		memcpy(rtp + 12, hdr2, h2);
	memcpy(rtp + 12 + h2, p, n);
	if (c->tcp_interleaved) {
		if (interleaved_send(c, tr->ch, rtp, len))
			return -1;
	} else {
		sendto(tr->udp_fd, rtp, len, MSG_DONTWAIT, (struct sockaddr *)&tr->udp_dst,
		       sizeof(tr->udp_dst));
	}
	tr->seq++;
	tr->pkts++;
	tr->octets += h2 + n;
	return 0;
}

/*
 * RTP timestamps of both tracks are CLOCK_MONOTONIC * clock rate, so a sender
 * report maps "now" to the same NTP time for both and the client can lip-sync.
 */
static double ntp_offset;	/* NTP seconds - CLOCK_MONOTONIC seconds */

static void rtcp_sr(struct client *c, int ti, unsigned rate, double now)
{
	struct track *tr = &c->t[ti];
	double ntp = now + ntp_offset;
	uint32_t sec = (uint32_t)(uint64_t)ntp;
	uint32_t frac = (uint32_t)((ntp - floor(ntp)) * 4294967296.0);
	uint32_t ts = (uint32_t)(uint64_t)(now * rate);
	uint32_t w[7] = { htonl(0x80c80006), htonl(tr->ssrc), htonl(sec), htonl(frac),
			  htonl(ts), htonl(tr->pkts), htonl(tr->octets) };

	if (c->tcp_interleaved)
		interleaved_send(c, tr->ch + 1, (uint8_t *)w, sizeof(w));
	else
		sendto(tr->rtcp_fd, w, sizeof(w), MSG_DONTWAIT, (struct sockaddr *)&tr->rtcp_dst,
		       sizeof(tr->rtcp_dst));
}

static void rtcp_tick(double now)
{
	for (int i = 0; i < MAXCLIENTS; i++) {
		struct client *c = &clients[i];
		if (c->state != C_PLAYING || now - c->last_sr < 5)
			continue;
		/* the first report goes out once both tracks are flowing */
		if (c->t[T_VIDEO].setup && !c->t[T_VIDEO].pkts)
			continue;
		c->last_sr = now;
		if (c->t[T_VIDEO].setup)
			rtcp_sr(c, T_VIDEO, 90000, now);
		if (c->t[T_AUDIO].setup && c->t[T_AUDIO].pkts)
			rtcp_sr(c, T_AUDIO, arate, now);
		if (c->tcp_interleaved)
			tcp_flush(c);
	}
}

/* Send one access unit (Annex-B) to a client. Returns -1 on TCP overflow. */
static int send_au(struct client *c, const uint8_t *au, size_t n, uint32_t ts)
{
	size_t i = 0;

	/* find NAL units */
	while (i + 3 < n) {
		size_t s, e;
		if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) s = i + 3;
		else if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) s = i + 4;
		else { i++; continue; }
		for (e = s; e + 2 < n; e++)
			if (au[e] == 0 && au[e + 1] == 0 && (au[e + 2] == 1 ||
			    (e + 3 < n && au[e + 2] == 0 && au[e + 3] == 1)))
				break;
		if (e + 2 >= n)
			e = n;
		while (e > s && au[e - 1] == 0)	/* trailing_zero_8bits */
			e--;
		int last = 1;
		for (size_t k = e; k + 2 < n; k++)
			if (au[k] == 0 && au[k + 1] == 0 && au[k + 2] == 1) { last = 0; break; }
		const uint8_t *nal = au + s;
		size_t nl = e - s;
		if (nl <= RTP_MTU) {
			if (rtp_send(c, T_VIDEO, 96, NULL, 0, nal, nl, last, ts))
				return -1;
		} else {
			uint8_t fu[2] = { (nal[0] & 0xe0) | 28, 0x80 | (nal[0] & 0x1f) };
			size_t off = 1;
			while (off < nl) {
				size_t chunk = nl - off > RTP_MTU - 2 ? RTP_MTU - 2 : nl - off;
				if (off + chunk == nl)
					fu[1] |= 0x40;
				if (rtp_send(c, T_VIDEO, 96, fu, 2, nal + off, chunk, last && off + chunk == nl, ts))
					return -1;
				fu[1] &= ~0x80;
				off += chunk;
			}
		}
		i = e;
	}
	return 0;
}

static void grab_params(struct vstream *v, const uint8_t *au, size_t n)
{
	for (size_t i = 0; i + 4 < n; i++) {
		if (!(au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1))
			continue;
		uint8_t t = au[i + 4] & 0x1f;
		size_t e = i + 4;
		while (e + 3 < n && !(au[e] == 0 && au[e + 1] == 0 && (au[e + 2] == 1 ||
		       (au[e + 2] == 0 && au[e + 3] == 1))))
			e++;
		if (e + 3 >= n)
			e = n;
		while (e > i + 4 && au[e - 1] == 0)
			e--;
		if (t == 7 && e - i <= sizeof(v->sps)) { memcpy(v->sps, au + i, e - i); v->sps_len = e - i; }
		if (t == 8 && e - i <= sizeof(v->pps)) { memcpy(v->pps, au + i, e - i); v->pps_len = e - i; }
		if (t == 5 || t == 1)
			break;
	}
}

static void deliver(struct vstream *v, const uint8_t *au, size_t n, int key, double sof)
{
	uint32_t ts = (uint32_t)(uint64_t)(sof * 90000.0);

	if (key)
		grab_params(v, au, n);
	for (int i = 0; i < MAXCLIENTS; i++) {
		struct client *c = &clients[i];
		if (c->state != C_PLAYING || !c->t[T_VIDEO].setup || c->vs != v->idx)
			continue;
		if (c->wait_idr && !key) {
			c->skipped++;
			continue;
		}
		if (c->tcp_interleaved && c->txlen > TXBUF / 4) {
			/* behind: drop until the next IDR rather than add latency */
			c->wait_idr = 1;
			c->skipped++;
			request_idr(v);
			continue;
		}
		size_t mark = c->txlen;
		if (send_au(c, au, n, ts)) {
			c->txlen = mark;	/* drop the partial AU */
			c->wait_idr = 1;
			c->skipped++;
			request_idr(v);
			continue;
		}
		if (c->wait_idr && verbose)
			LOG("client %s: first IDR", inet_ntoa(c->peer.sin_addr));
		c->wait_idr = 0;
		c->frames++;
		if (c->tcp_interleaved)
			tcp_flush(c);
	}
}

/* ------------------------------------------------------------------ audio */

static struct pcm *apcm;
static int amic_gain = -1;	/* "Mic1 Capture Volume" 0..31, -1 = leave */
static int16_t abuf[48000 * APKT_MS / 1000];
static unsigned afill, aframes;	/* frames in abuf, frames per packet */
static uint64_t asamples;	/* samples captured since the audio clock anchor */
static double aanchor;		/* CLOCK_MONOTONIC time of sample 0 */
static unsigned aresync;

static void audio_open(void)
{
	struct pcm_config cfg = {
		.channels = 1, .rate = arate, .format = PCM_FORMAT_S16_LE,
		.period_size = arate * APKT_MS / 1000, .period_count = 8,
		.start_threshold = 1,
	};

	aframes = cfg.period_size;
	if (amic_gain >= 0) {
		struct mixer *m = mixer_open(0);
		struct mixer_ctl *ctl = m ? mixer_get_ctl_by_name(m, "Mic1 Capture Volume") : NULL;
		if (!ctl || mixer_ctl_set_value(ctl, 0, amic_gain))
			LOG("audio: cannot set mic gain");
		if (m)
			mixer_close(m);
	}
	apcm = pcm_open(0, 0, PCM_IN | PCM_NONBLOCK, &cfg);
	if (!apcm || !pcm_is_ready(apcm)) {
		LOG("audio: %s; stream is video only", apcm ? pcm_get_error(apcm) : "no pcm");
		if (apcm)
			pcm_close(apcm);
		apcm = NULL;
		return;
	}
	if (pcm_start(apcm))
		LOG("audio start: %s", pcm_get_error(apcm));
	audio_on = 1;
	asamples = 0;
	aanchor = mono();
	LOG("audio: %u Hz mono %s, %u ms packets", arate, aenc_name, APKT_MS);
}

/* G.711 (ITU-T G.711 / Sun g711.c reference algorithm) */
static uint8_t ulaw(int16_t s16)
{
	int s = s16 >> 2, mask = 0xff, seg = 0;		/* 14-bit */
	if (s < 0) { s = -s; mask = 0x7f; }
	s += 33;					/* bias */
	if (s > 0x1fff) s = 0x1fff;
	for (int v = s >> 6; v; v >>= 1) seg++;
	return (uint8_t)(((seg << 4) | ((s >> (seg + 1)) & 0xf)) ^ mask);
}

static uint8_t alaw(int16_t s16)
{
	int s = s16 >> 3, mask = 0xd5, seg = 0;		/* 13-bit */
	if (s < 0) { s = -s - 1; mask = 0x55; }
	if (s > 0xfff) s = 0xfff;
	for (int v = s >> 5; v; v >>= 1) seg++;
	int aval = seg << 4 | ((seg ? s >> seg : s >> 1) & 0xf);
	return (uint8_t)(aval ^ mask);
}

static void audio_deliver(const int16_t *pcm, unsigned n, uint32_t ts)
{
	uint8_t be[sizeof(abuf)];
	size_t len;

	if (audio_pt == 0 || audio_pt == 8) {
		for (unsigned i = 0; i < n; i++)
			be[i] = audio_pt ? alaw(pcm[i]) : ulaw(pcm[i]);
		len = n;
	} else {
		for (unsigned i = 0; i < n; i++) {
			be[2 * i] = (uint16_t)pcm[i] >> 8;
			be[2 * i + 1] = pcm[i];
		}
		len = 2 * n;
	}
	for (int i = 0; i < MAXCLIENTS; i++) {
		struct client *c = &clients[i];
		if (c->state != C_PLAYING || !c->t[T_AUDIO].setup)
			continue;
		/* a congested TCP client loses audio rather than gaining latency */
		if (c->tcp_interleaved && c->txlen > TXBUF / 4) {
			c->askipped++;
			c->t[T_AUDIO].seq++;
			continue;
		}
		if (rtp_send(c, T_AUDIO, audio_pt, NULL, 0, be, len, 0, ts)) {
			c->askipped++;
			continue;
		}
		c->apkts++;
		if (c->tcp_interleaved)
			tcp_flush(c);
	}
}

static void audio_read(void)
{
	for (;;) {
		int r = pcm_readi(apcm, abuf + afill, aframes - afill);
		if (r <= 0) {
			if (errno != EAGAIN)
				LOG("audio read: %s", pcm_get_error(apcm));
			return;
		}
		afill += r;
		if (afill < aframes)
			continue;
		/*
		 * Audio clock = sample count since the anchor. The codec clock and
		 * CLOCK_MONOTONIC drift apart slowly; re-anchor when the gap to the
		 * arrival time exceeds 40 ms (also covers overruns).
		 */
		double now = mono();
		long delay = pcm_get_delay(apcm);
		double arrived = now - (delay > 0 ? delay : 0) / (double)arate;
		double t = aanchor + (double)asamples / arate;	/* first sample of abuf */
		double t_end = t + (double)aframes / arate;
		if (fabs(t_end - arrived) > 0.040) {
			if (verbose || aresync < 3)
				LOG("audio: clock resync by %.1f ms", (arrived - t_end) * 1e3);
			aresync++;
			aanchor = arrived - (double)aframes / arate;
			asamples = 0;
			t = aanchor;
		}
		audio_deliver(abuf, aframes, (uint32_t)(uint64_t)(t * arate));
		asamples += aframes;
		afill = 0;
	}
}

/* ------------------------------------------------------------ auto exposure */

static int ae_fd = -1, ae_target = 70, ae_flicker = 50, ae_gain_max = 1024;
static int ae_exp, ae_gain, ae_exp_max = 1336, ae_vts = 1340, ae_skip;
static double ae_mean = -1;
static const uint8_t *cam_y[NCAM + NDEW];
static unsigned cam_stride;

static int sd_get(unsigned id)
{
	struct v4l2_control c = { .id = id };
	return xioctl(ae_fd, VIDIOC_G_CTRL, &c) ? -1 : c.value;
}

static void sd_set(unsigned id, int v)
{
	struct v4l2_control c = { .id = id, .value = v };
	if (xioctl(ae_fd, VIDIOC_S_CTRL, &c))
		LOG("AE: S_CTRL %x=%d: %s", id, v, strerror(errno));
}

static void ae_open(void)
{
	char path[64], name[64];

	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof(path), "/sys/class/video4linux/v4l-subdev%d/name", i);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(name, sizeof(name), f) && !strncmp(name, "gc3003", 6)) {
			snprintf(path, sizeof(path), "/dev/v4l-subdev%d", i);
			ae_fd = open(path, O_RDWR);
		}
		fclose(f);
	}
	if (ae_fd < 0) {
		LOG("AE: no sensor subdev, auto exposure off");
		return;
	}
	struct v4l2_queryctrl q = { .id = V4L2_CID_EXPOSURE };
	if (!xioctl(ae_fd, VIDIOC_QUERYCTRL, &q))
		ae_exp_max = q.maximum;
	int vb = sd_get(V4L2_CID_VBLANK);
	if (vb >= 0)
		ae_vts = 1296 + vb;
	ae_exp = sd_get(V4L2_CID_EXPOSURE);
	ae_gain = sd_get(V4L2_CID_ANALOGUE_GAIN);
	LOG("AE: exposure %d/%d lines, gain %d/64, target %d, flicker %d Hz",
	    ae_exp, ae_exp_max, ae_gain, ae_target, ae_flicker);
}

/* Called for every camera frame with the index of the buffer just filled. */
static void ae_frame(unsigned idx)
{
	if (ae_fd < 0 || !cam_y[idx])
		return;
	if (ae_skip > 0) {	/* sensor applies new settings ~2 frames later */
		ae_skip--;
		return;
	}
	const uint8_t *y = cam_y[idx];
	unsigned n = 0, clip = 0, sum = 0;
	for (unsigned r = 1; r < 28; r++)
		for (unsigned c = 1; c < 48; c++) {
			unsigned v = y[(size_t)(r * H / 28) * cam_stride + c * W / 48];
			sum += v; n++;
			clip += v >= 250;
		}
	double mean = (double)sum / n, clipf = (double)clip / n;
	ae_mean = mean;
	{
		static unsigned dbg;
		if (verbose > 1 && !(dbg++ % 15))
			LOG("AE dbg: buf %u mean %.1f first px %u %u %u", idx, mean, y[0],
			    y[(size_t)(H / 2) * cam_stride + W / 2], y[(size_t)(H - 1) * cam_stride + W - 1]);
	}
	double err = ae_target / (mean < 1 ? 1 : mean);
	if (clipf > 0.01 && err > 0.85)
		err = 0.85;			/* protect highlights */
	if (err > 0.92 && err < 1.08 && clipf <= 0.01)
		return;				/* deadband */
	double f = err > 1 ? (err > 2 ? 2 : err) : (err < 0.5 ? 0.5 : err);
	f = 1 + (f - 1) * 0.7;			/* damping */
	double total = (double)ae_exp * ae_gain / 64.0 * f;
	double line_s = 1.0 / (FPS * (double)ae_vts);
	int flick = ae_flicker ? (int)(1.0 / (2 * ae_flicker) / line_s) : 0;	/* lines per half mains period */
	int exp = total > ae_exp_max ? ae_exp_max : (int)total;
	if (flick && exp >= flick)
		exp = exp / flick * flick;	/* no 100/120 Hz banding */
	if (exp < 4)
		exp = 4;
	int gain = (int)(total / exp * 64 + 0.5);
	if (gain < 64) gain = 64;
	if (gain > ae_gain_max) gain = ae_gain_max;
	if (exp != ae_exp)
		sd_set(V4L2_CID_EXPOSURE, exp);
	if (gain != ae_gain)
		sd_set(V4L2_CID_ANALOGUE_GAIN, gain);
	if (verbose)
		LOG("AE: mean %.0f clip %.1f%% -> exposure %d (%.1f ms) gain %.2fx", mean,
		    clipf * 100, exp, exp * line_s * 1e3, gain / 64.0);
	ae_exp = exp;
	ae_gain = gain;
	ae_skip = 2;
}

/* ---------------------------------------------------- AWB + day/night */

#define VIN_PARAM "/sys/module/sun8i_v831_vin/parameters/"
static int awb_on = 1, wb_r = 0x197, wb_b = 0x1cf;
static int night;			/* current mode */
static const char *dn_mode = "auto";	/* auto | day | night */
static int dn_night_thr = -1, dn_day_thr = -1;	/* light sensor raw thresholds */
/* image-based day/night (default, like the stock app): scene light L = mean luma
 * normalised to full exposure and unity gain */
static double dn_l_night = 8, dn_l_day = 40, dn_l_base = -1;
static int dn_cnt;
static int ccm_day = 3;
static double dn_last_switch;
static int light_fd = -1, ir_led_fd = -1;
static int probe_on;			/* night-mode ambient probe: IR LEDs off, measuring */
static double probe_t = -1e9, probe_next;

static void param_write(const char *name, int v)
{
	char path[128], buf[16];
	snprintf(path, sizeof(path), VIN_PARAM "%s", name);
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return;
	int n = snprintf(buf, sizeof(buf), "%d\n", v);
	if (write(fd, buf, n) < 0)
		LOG("write %s: %s", name, strerror(errno));
	close(fd);
}

static int param_read(const char *name, int def)
{
	char path[128], buf[16] = "";
	snprintf(path, sizeof(path), VIN_PARAM "%s", name);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return def;
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	return n > 0 ? atoi(buf) : def;
}

/* request gpiochip0 output lines, return line fd */
static int gpio_out(const unsigned *lines, int n, unsigned long long vals)
{
	struct gpio_v2_line_request req = { 0 };
	int fd = open("/dev/gpiochip0", O_RDWR), r;
	if (fd < 0)
		return -1;
	for (int i = 0; i < n; i++)
		req.offsets[i] = lines[i];
	req.num_lines = n;
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	req.config.num_attrs = 1;
	req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	req.config.attrs[0].attr.values = vals;
	req.config.attrs[0].mask = (1ULL << n) - 1;
	strcpy(req.consumer, "camrtsp");
	r = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req);
	close(fd);
	return r < 0 ? -1 : req.fd;
}

/* y623: IR-cut motor PH11 (day) / PH12 (night), IR LEDs PH8 */
#define GPIO_IRCUT_DAY	235
#define GPIO_IRCUT_NIGHT 236
#define GPIO_IR_LED	232

static void ircut_pulse(int to_night)
{
	unsigned l[2] = { GPIO_IRCUT_DAY, GPIO_IRCUT_NIGHT };
	int fd = gpio_out(l, 2, to_night ? 2 : 1);
	if (fd < 0) {
		LOG("ircut: %s", strerror(errno));
		return;
	}
	usleep(200000);
	struct gpio_v2_line_values v = { .bits = 0, .mask = 3 };
	ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
	usleep(20000);
	close(fd);
}

static void set_night(int on)
{
	if (on) {
		ircut_pulse(1);
		if (ir_led_fd < 0) {
			unsigned l = GPIO_IR_LED;
			ir_led_fd = gpio_out(&l, 1, 1);
		}
		param_write("ccm", 4);
	} else {
		if (ir_led_fd >= 0) {
			close(ir_led_fd);	/* releasing the line lets it fall back low */
			ir_led_fd = -1;
			unsigned l = GPIO_IR_LED;
			int fd = gpio_out(&l, 1, 0);
			if (fd >= 0)
				close(fd);
		}
		ircut_pulse(0);
		param_write("ccm", ccm_day);
		param_write("wb_red", wb_r);
		param_write("wb_blue", wb_b);
	}
	night = on;
	dn_last_switch = mono();
	probe_on = 0;
	probe_next = 0;
	LOG("day/night: %s", on ? "NIGHT (IR)" : "DAY");
}

static int light_read(void)
{
	char buf[16] = "";
	if (light_fd < 0)
		return -1;
	if (pread(light_fd, buf, sizeof(buf) - 1, 0) <= 0)
		return -1;
	return atoi(buf);
}

/* once per second */
static double scene_light(void)
{
	if (ae_fd < 0 || ae_exp <= 0 || ae_gain <= 0 || ae_mean < 0)
		return -1;
	return ae_mean * 64.0 * ae_exp_max / ((double)ae_exp * ae_gain);
}

/*
 * /tmp/camrtsp.status (rewritten every 5 s) for other tools, and a history
 * line in the log every 10 minutes to tune the day/night thresholds.
 */
static void status_tick(double now, int light)
{
	static double last_file, last_log = -1e9;
	if (now - last_file < 5)
		return;
	last_file = now;
	FILE *f = fopen("/tmp/camrtsp.status.new", "w");
	if (f) {
		fprintf(f, "light=%d\nexposure=%d\nexposure_max=%d\ngain=%d\nluma=%.0f\n"
			"scene_light=%.1f\nir_floor=%.1f\nnight=%d\nmode=%s\nnight_thr=%d\n"
			"day_thr=%d\nwb_red=%d\nwb_blue=%d\n",
			light, ae_exp, ae_exp_max, ae_gain, ae_mean, scene_light(), dn_l_base,
			night, dn_mode, dn_night_thr, dn_day_thr, wb_r, wb_b);
		fclose(f);
		rename("/tmp/camrtsp.status.new", "/tmp/camrtsp.status");
	}
	if (now - last_log >= 600) {
		last_log = now;
		LOG("status: scene light %.1f, exposure %d/%d, gain %.2fx, luma %.0f, adc %d, %s",
		    scene_light(), ae_exp, ae_exp_max, ae_gain / 64.0, ae_mean, light,
		    night ? "NIGHT" : "DAY");
	}
}

/* runtime override from onvifd (ONVIF IrCutFilter): "mode=auto|day|night" */
#define CTL_FILE "/tmp/camrtsp.ctl"
static void ctl_poll(void)
{
	static struct timespec seen;
	static char buf[16];
	struct stat st;
	if (stat(CTL_FILE, &st) || (st.st_mtim.tv_sec == seen.tv_sec &&
				    st.st_mtim.tv_nsec == seen.tv_nsec))
		return;
	seen = st.st_mtim;
	FILE *f = fopen(CTL_FILE, "r");
	char line[64];
	while (f && fgets(line, sizeof(line), f)) {
		char v[16];
		if (sscanf(line, "mode=%15s", v) == 1 &&
		    (!strcmp(v, "auto") || !strcmp(v, "day") || !strcmp(v, "night")) &&
		    strcmp(v, dn_mode)) {
			snprintf(buf, sizeof(buf), "%s", v);
			dn_mode = buf;
			dn_last_switch = 0;	/* auto may switch right away */
			LOG("day/night: mode %s (control file)", dn_mode);
		}
	}
	if (f)
		fclose(f);
}

/*
 * Ambient-light probe while in night mode: with the IR LEDs on, the picture
 * cannot tell IR from room light, and the day rule (4x the IR-only floor) is
 * out of reach for an evening lamp - once night, it stayed night all evening.
 * So now and then the LEDs go off for PROBE_S seconds (IR-cut filter stays
 * open, no motor click) and the scene light is measured without them.
 */
#define PROBE_S		3

static void ir_leds(int on)
{
	if (ir_led_fd < 0)
		return;
	struct gpio_v2_line_values v = { .bits = on ? 1 : 0, .mask = 1 };
	ioctl(ir_led_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

/* returns 1 while a probe is running (the caller skips its own decision) */
static int daynight_probe(double now, double L)
{
	if (probe_on) {
		if (now - probe_t < PROBE_S)
			return 1;
		probe_on = 0;
		int day = L > 2 * dn_l_night;
		LOG("day/night: probe without IR: scene light %.1f -> %s", L, day ? "DAY" : "stay NIGHT");
		if (day) {
			dn_cnt = 0;
			dn_l_base = -1;
			set_night(0);
			return 1;
		}
		ir_leds(1);
		probe_next = now + 1800;
		return 1;
	}
	if (!night || ir_led_fd < 0 || now - dn_last_switch < 120)
		return 0;
	if (!probe_next)
		probe_next = now;
	/* light beyond the IR floor hints at room light: probe sooner */
	if (dn_l_base > 0 && L > 1.15 * dn_l_base && probe_next > now + 600)
		probe_next = now + 600;
	if (now < probe_next)
		return 0;
	probe_on = 1;
	probe_t = now;
	ir_leds(0);
	return 1;
}

static void daynight_tick(void)
{
	static double last;
	double now = mono();
	if (now - last < 1.0)
		return;
	last = now;
	ctl_poll();
	status_tick(now, light_read());
	if (!strcmp(dn_mode, "day") || !strcmp(dn_mode, "night")) {
		int want = !strcmp(dn_mode, "night");
		if (want != night)
			set_night(want);
		return;
	}
	if (dn_night_thr < 0 || dn_day_thr < 0) {
		/*
		 * No light-sensor thresholds: decide from the picture.
		 * Night: L below dn_l_night for 20 s (AE at its limits).
		 * Day: with the IR LEDs on, L must stay for 60 s above both dn_l_day and
		 * 4x the IR-only floor (lowest L seen since switching to night), so
		 * the IR light itself and someone walking past do not flip it back.
		 */
		double L = scene_light();
		if (L < 0)
			return;
		if (daynight_probe(now, L))
			return;
		if (night && now - dn_last_switch > 10 && now - probe_t > PROBE_S + 3)
			dn_l_base = dn_l_base < 0 || L < dn_l_base ? L : dn_l_base;
		int cond = !night ? L < dn_l_night :
			   now - dn_last_switch > 10 && L > dn_l_day && L > 4 * dn_l_base;
		dn_cnt = cond ? dn_cnt + 1 : 0;
		if (now - dn_last_switch < 60 || dn_cnt < (night ? 60 : 20))
			return;
		LOG("day/night: scene light %.1f (night < %.0f, day > %.0f, IR floor %.1f)", L,
		    dn_l_night, dn_l_day, dn_l_base);
		dn_cnt = 0;
		dn_l_base = -1;
		set_night(!night);
		return;
	}
	int v = light_read();
	if (v < 0 || now - dn_last_switch < 30)
		return;
	/* the sensor reads higher in the dark (pull-up + photoresistor); thresholds
	 * are configurable, direction follows their order */
	int darker_is_higher = dn_night_thr > dn_day_thr;
	int is_dark = darker_is_higher ? v >= dn_night_thr : v <= dn_night_thr;
	int is_bright = darker_is_higher ? v <= dn_day_thr : v >= dn_day_thr;
	if (!night && is_dark)
		set_night(1);
	else if (night && is_bright)
		set_night(0);
}

static void daynight_init(void)
{
	light_fd = open("/sys/bus/iio/devices/iio:device0/in_voltage0_raw", O_RDONLY);
	wb_r = param_read("wb_red", wb_r);
	wb_b = param_read("wb_blue", wb_b);
	ccm_day = param_read("ccm", 3);
	if (ccm_day == 4)
		ccm_day = 3;
	night = -1;
	set_night(!strcmp(dn_mode, "night"));
	LOG("day/night: mode %s, light sensor %s (now %d), thresholds night %d day %d",
	    dn_mode, light_fd >= 0 ? "ok" : "missing", light_read(), dn_night_thr, dn_day_thr);
}

/* grey-world on near-neutral mid-tone pixels; called with each sampled frame */
static void awb_frame(const uint8_t *y, const uint8_t *uv)
{
	static unsigned cnt;
	if (!awb_on || night || ++cnt % 8)
		return;
	long su = 0, sv = 0, n = 0;
	for (unsigned r = 1; r < 27; r++)
		for (unsigned c = 1; c < 48; c++) {
			unsigned yy = r * H / 27, xx = c * W / 48;
			unsigned lum = y[(size_t)yy * cam_stride + xx];
			const uint8_t *p = uv + (size_t)(yy / 2) * cam_stride + (xx & ~1u);
			int u = p[0] - 128, v = p[1] - 128;
			if (lum < 40 || lum > 230 || abs(u) + abs(v) > 48)
				continue;
			su += u; sv += v; n++;
		}
	if (n < 50)
		return;
	double mu = (double)su / n, mv = (double)sv / n;	/* Cb, Cr offsets */
	if (fabs(mu) < 1.0 && fabs(mv) < 1.0)
		return;
	/* Cr > 0: too red -> lower red gain; Cb > 0: too blue -> lower blue gain */
	int nr = wb_r * (1.0 - 0.004 * mv), nb = wb_b * (1.0 - 0.004 * mu);
	if (nr < 0x100) nr = 0x100;
	if (nr > 0x400) nr = 0x400;
	if (nb < 0x100) nb = 0x100;
	if (nb > 0x400) nb = 0x400;
	if (nr != wb_r) { wb_r = nr; param_write("wb_red", wb_r); }
	if (nb != wb_b) { wb_b = nb; param_write("wb_blue", wb_b); }
	if (verbose > 1)
		LOG("AWB: Cb %+.1f Cr %+.1f (%ld px) -> R %#x B %#x", mu, mv, n, wb_r, wb_b);
}

/* --------------------------------------------------------------- pipeline */

/* ------------------------------------------------------- dewarp (EISE LDC) */

/*
 * Lens distortion correction on the EISE engine. Our own radial model:
 * src = c + (dst - c) / zoom * (1 + k1 r^2 + k2 r^4), r normalised to the half
 * diagonal. Negative k1 straightens the lens' barrel distortion (and crops
 * the outer field of view a little). Tables: per 32x32 output tile an 8x8
 * grid of source points in 1/16 px and the tile's source bounding box.
 */
static int dew_on;
static double dew_k1 = -0.15, dew_k2, dew_zoom = 1.0;
static double dew_fish;		/* > 0: equidistant fisheye focal length (px) */
static int dew_fd = -1;

static int dewarp_tables(unsigned w, unsigned h, uint8_t **mesh_out, uint8_t **bbox_out,
			 unsigned *tiles_out)
{
	unsigned tw = (w + 31) / 32, th = (h + 31) / 32, tiles = tw * th, bad = 0;
	int32_t *mesh = malloc(tiles * 0x200);
	uint32_t *bbox = calloc(tiles, 0x20);
	double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0, rn2 = cx * cx + cy * cy;

	if (!mesh || !bbox) {
		free(mesh);
		free(bbox);
		return -1;
	}
	for (unsigned ty = 0; ty < th; ty++)
		for (unsigned tx = 0; tx < tw; tx++) {
			unsigned t = ty * tw + tx;
			int32_t *m = mesh + t * 128;
			int xmin = 1 << 30, xmax = -1, ymin = 1 << 30, ymax = -1;
			for (int r = 0; r < 8; r++)
				for (int c = 0; c < 8; c++) {
					double x = tx * 32 + c * 31 / 7.0, y = ty * 32 + r * 31 / 7.0;
					double dx, dy, f;
					if (dew_fish > 0) {
						/*
						 * Fisheye (r_src = F * theta) to rectilinear
						 * (r_out = F * zoom * tan theta).
						 */
						double ru;
						dx = x - cx;
						dy = y - cy;
						ru = sqrt(dx * dx + dy * dy);
						f = ru > 1e-6 ? dew_fish * atan(ru / (dew_fish * dew_zoom)) / ru : 1 / dew_zoom;
					} else {
						dx = (x - cx) / dew_zoom;
						dy = (y - cy) / dew_zoom;
						double rr = (dx * dx + dy * dy) / rn2;
						f = 1 + dew_k1 * rr + dew_k2 * rr * rr;
					}
					double sx = cx + dx * f, sy = cy + dy * f;
					sx = sx < 0 ? 0 : sx > w - 1 ? w - 1 : sx;
					sy = sy < 0 ? 0 : sy > h - 1 ? h - 1 : sy;
					int ix = (int)(sx * 16), iy = (int)(sy * 16);
					m[(r * 8 + c) * 2] = ix;
					m[(r * 8 + c) * 2 + 1] = iy;
					if (r == 0 || r == 7 || c == 0 || c == 7) {
						int px = ix >> 4, py = iy >> 4;
						if (px < xmin) xmin = px;
						if (px > xmax) xmax = px;
						if (py < ymin) ymin = py;
						if (py > ymax) ymax = py;
					}
				}
			int xmax_e = (xmax + 1) & ~1;
			int wfit = ((xmax_e - (xmin & ~31)) + 0x21) & ~31;
			int hfit = ((ymax - (ymin & ~1)) + 3) & ~1;
			if (!(wfit <= 0x60 && hfit <= 0x40 && wfit * hfit <= 0x1800))
				bad++;
			bbox[t * 8] = (ymax & 0xfff) | (uint32_t)(ymin & 0xfff) << 16;
			bbox[t * 8 + 1] = (xmax_e & 0xfff) | (uint32_t)(xmin & 0xffe) << 16;
		}
	if (bad) {
		/* the split (four-box) tile form is not decoded yet */
		LOG("dewarp: %u tiles need split boxes; use a milder -L", bad);
		free(mesh);
		free(bbox);
		return -1;
	}
	*mesh_out = (uint8_t *)mesh;
	*bbox_out = (uint8_t *)bbox;
	*tiles_out = tiles;
	return 0;
}

static int dewarp_open(unsigned w, unsigned h)
{
	uint8_t *mesh, *bbox;
	unsigned tiles;
	double t0 = mono();

	if (dewarp_tables(w, h, &mesh, &bbox, &tiles))
		return -1;
	dew_fd = open("/dev/eise", O_RDWR | O_NONBLOCK);
	if (dew_fd < 0) {
		LOG("dewarp: /dev/eise: %s", strerror(errno));
		free(mesh);
		free(bbox);
		return -1;
	}
	struct eise_tables t = { .width = w, .height = h, .mesh_size = tiles * 0x200,
				 .bbox_size = tiles * 0x20, .mesh_ptr = (uintptr_t)mesh,
				 .bbox_ptr = (uintptr_t)bbox };
	int r = xioctl(dew_fd, EISE_IOC_SET_TABLES, &t);
	free(mesh);
	free(bbox);
	if (r) {
		LOG("dewarp: set tables: %s", strerror(errno));
		close(dew_fd);
		dew_fd = -1;
		return -1;
	}
	if (dew_fish > 0)
		LOG("dewarp: fisheye f %.0f px (%.0f deg across), zoom %.2f, %u tiles, tables in %.0f ms",
		    dew_fish, 2 * w / 2.0 / dew_fish * 180 / M_PI, dew_zoom, tiles, (mono() - t0) * 1e3);
	else
		LOG("dewarp: k1 %.3f k2 %.3f zoom %.2f, %u tiles, tables in %.0f ms", dew_k1, dew_k2,
		    dew_zoom, tiles, (mono() - t0) * 1e3);
	return 0;
}

static void set_ctrl(int fd, unsigned id, int val, const char *name);

/* ------------------------------------------------------------- sub-stream */

/*
 * Second, low-resolution H.264 stream from VIN channel 1 (/dev/video1, its own
 * scaler on the same ISP frame, no dewarp) through a second encoder context.
 */
#define NSUBCAM		3
#define NSUBCODED	2
static unsigned SW = 640, SH = 360;
static int scfd = -1, sdmafd[NSUBCAM];
static uint8_t *scmap[NSUBCODED];
static unsigned ssizeimage, sstride;
static unsigned sub_ncoded;

/*
 * Detector frames (-P ms): every so often one sub-stream frame is scaled into
 * DET_W x DET_H RGB, letterboxed like the YOLO training images (grey 114
 * bars), and published in /tmp/detframe for persond. Seqlock: seq is odd
 * while the frame is written. The camera buffers are read through their
 * dmabuf mmap (uncached): each source row is copied once into cached memory.
 */
#define DET_W		320
#define DET_H		192
#define DET_PATH	"/tmp/detframe"
struct detframe {
	uint32_t magic;			/* 'DETF' */
	volatile uint32_t seq;
	uint32_t w, h;			/* DET_W x DET_H, RGB888 */
	uint32_t src_w, src_h;		/* sub-stream size; the picture is centred */
	uint32_t box_x, box_y, box_w, box_h;	/* picture area inside the frame */
	uint64_t mono_ms;		/* CLOCK_MONOTONIC capture time */
	uint8_t rgb[DET_W * DET_H * 3];
};
static unsigned det_ms;			/* 0 = off */
static struct detframe *det;
static const uint8_t *smap[NSUBCAM];	/* camera buffers (dmabuf mmap) */
static double det_last;

static void det_open(void)
{
	int fd = open(DET_PATH ".new", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, sizeof(*det)) ||
	    (det = mmap(NULL, sizeof(*det), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		LOG("detframe: %s; detector frames off", strerror(errno));
		det = NULL; det_ms = 0;
		if (fd >= 0) close(fd);
		return;
	}
	close(fd);
	memset(det, 0, sizeof(*det));
	det->magic = 0x46544544;
	det->w = DET_W; det->h = DET_H; det->src_w = SW; det->src_h = SH;
	rename(DET_PATH ".new", DET_PATH);
	LOG("detector frames %ux%u every %u ms: %s", DET_W, DET_H, det_ms, DET_PATH);
}

static inline uint8_t clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* NV12 (SW x SH) -> letterboxed RGB, area average of the luma, chroma per box */
static void det_publish(const uint8_t *nv12, const struct timeval *ts)
{
	unsigned nw, nh;
	if (SW * DET_H > SH * DET_W) { nw = DET_W; nh = (SH * DET_W + SW / 2) / SW; }
	else { nh = DET_H; nw = (SW * DET_H + SH / 2) / SH; }
	unsigned dx = (DET_W - nw) / 2, dy = (DET_H - nh) / 2;
	static uint8_t yrow[4][1920], uvrow[1920];
	static unsigned sxs[DET_W + 1];
	for (unsigned x = 0; x <= nw; x++) sxs[x] = x * SW / nw;

	__atomic_store_n(&det->seq, det->seq + 1, __ATOMIC_RELEASE);	/* odd: writing */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	memset(det->rgb, 114, sizeof(det->rgb));
	for (unsigned oy = 0; oy < nh; oy++) {
		unsigned y0 = oy * SH / nh, y1 = (oy + 1) * SH / nh, ny = y1 - y0;
		if (ny < 1) ny = 1;
		if (ny > 4) ny = 4;
		for (unsigned k = 0; k < ny; k++)
			memcpy(yrow[k], nv12 + (size_t)(y0 + k) * sstride, SW);
		memcpy(uvrow, nv12 + (size_t)SH * sstride + (size_t)((y0 + y1) / 4) * sstride, SW);
		uint8_t *o = det->rgb + ((size_t)(oy + dy) * DET_W + dx) * 3;
		for (unsigned ox = 0; ox < nw; ox++) {
			unsigned x0 = sxs[ox], x1 = sxs[ox + 1], nx = x1 > x0 ? x1 - x0 : 1, sum = 0;
			if (nx > 4) nx = 4;
			for (unsigned k = 0; k < ny; k++)
				for (unsigned j = 0; j < nx; j++)
					sum += yrow[k][x0 + j];
			int Y = (int)(sum / (nx * ny)) - 16;
			unsigned cx = ((x0 + x1) / 2) & ~1u;
			int U = uvrow[cx] - 128, V = uvrow[cx + 1] - 128;
			/* BT.601 limited range */
			int c = 298 * (Y < 0 ? 0 : Y) + 128;
			o[0] = clip8((c + 409 * V) >> 8);
			o[1] = clip8((c - 100 * U - 208 * V) >> 8);
			o[2] = clip8((c + 516 * U) >> 8);
			o += 3;
		}
	}
	det->box_x = dx; det->box_y = dy; det->box_w = nw; det->box_h = nh;
	det->mono_ms = (uint64_t)ts->tv_sec * 1000 + ts->tv_usec / 1000;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	__atomic_store_n(&det->seq, det->seq + 1, __ATOMIC_RELEASE);	/* even: valid */
}

static int sub_fail(const char *what)
{
	LOG("sub-stream: %s: %s; sub-stream off", what, strerror(errno));
	if (scfd >= 0)
		close(scfd);
	if (vstreams[1].efd >= 0)
		close(vstreams[1].efd);
	scfd = vstreams[1].efd = -1;
	sub_on = 0;
	return -1;
}

static int sub_open(const char *cam, const char *encpath, unsigned qp, unsigned gop)
{
	struct vstream *v = &vstreams[1];
	int sefd;

	scfd = open(cam, O_RDWR | O_NONBLOCK);
	v->efd = sefd = open(encpath, O_RDWR | O_NONBLOCK);
	if (scfd < 0 || sefd < 0)
		return sub_fail("open");
	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	f.fmt.pix.width = SW; f.fmt.pix.height = SH;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	if (xioctl(scfd, VIDIOC_S_FMT, &f))
		return sub_fail("cam S_FMT");
	SW = f.fmt.pix.width; SH = f.fmt.pix.height;
	unsigned stride = sstride = f.fmt.pix.bytesperline;
	ssizeimage = f.fmt.pix.sizeimage;
	v->w = SW; v->h = SH;

	struct v4l2_format cf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	cf.fmt.pix.width = SW; cf.fmt.pix.height = SH;
	cf.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	cf.fmt.pix.sizeimage = 128 * 1024;
	if (xioctl(sefd, VIDIOC_S_FMT, &cf))
		return sub_fail("enc S_FMT capture");
	struct v4l2_format of = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	of.fmt.pix.width = SW; of.fmt.pix.height = SH;
	of.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	of.fmt.pix.bytesperline = stride;
	if (xioctl(sefd, VIDIOC_S_FMT, &of) || of.fmt.pix.bytesperline != stride ||
	    of.fmt.pix.sizeimage > ssizeimage)
		return sub_fail("enc S_FMT output");
	struct v4l2_streamparm sp = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	sp.parm.output.timeperframe.numerator = 1;
	sp.parm.output.timeperframe.denominator = FPS;
	xioctl(sefd, VIDIOC_S_PARM, &sp);
	set_ctrl(sefd, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "sub prepend_spspps");
	set_ctrl(sefd, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, qp, "sub i_qp");
	set_ctrl(sefd, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, qp, "sub p_qp");
	set_ctrl(sefd, V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
		 V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC, "sub entropy");
	set_ctrl(sefd, V4L2_CID_MPEG_VIDEO_GOP_CLOSURE, 1, "sub gop_closure");
	set_ctrl(sefd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "sub gop_size");

	/* the detector reads these frames: cached (non-coherent) buffers, see det_publish */
	struct v4l2_requestbuffers rb = { .count = NSUBCAM, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					  .memory = V4L2_MEMORY_MMAP,
					  .flags = det_ms ? V4L2_MEMORY_FLAG_NON_COHERENT : 0 };
	if (xioctl(scfd, VIDIOC_REQBUFS, &rb) || rb.count != NSUBCAM)
		return sub_fail("cam REQBUFS");
	if (det_ms && !(rb.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS))
		LOG("sub-stream: no cache hints in the driver: detector frames read uncached (slow)");
	for (int i = 0; i < NSUBCAM; i++) {
		struct v4l2_exportbuffer e = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = i,
					       .flags = O_RDWR };
		if (xioctl(scfd, VIDIOC_EXPBUF, &e))
			return sub_fail("cam EXPBUF");
		sdmafd[i] = e.fd;
		smap[i] = NULL;
		if (det_ms) {
			void *m = mmap(NULL, ssizeimage, PROT_READ, MAP_SHARED, e.fd, 0);
			if (m == MAP_FAILED)
				LOG("sub-stream: dmabuf mmap: %s; detector frames off", strerror(errno));
			else
				smap[i] = m;
		}
	}
	if (det_ms && smap[0] && SW <= 1920 && SH * 2 >= SW / 4)
		det_open();
	rb = (struct v4l2_requestbuffers){ .count = NSUBCAM, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
					   .memory = V4L2_MEMORY_DMABUF };
	if (xioctl(sefd, VIDIOC_REQBUFS, &rb) || rb.count < NSUBCAM)
		return sub_fail("enc REQBUFS output");
	rb = (struct v4l2_requestbuffers){ .count = NSUBCODED, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					   .memory = V4L2_MEMORY_MMAP };
	if (xioctl(sefd, VIDIOC_REQBUFS, &rb) || rb.count < NSUBCODED)
		return sub_fail("enc REQBUFS capture");
	for (int i = 0; i < NSUBCODED; i++) {
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 .memory = V4L2_MEMORY_MMAP, .index = i };
		if (xioctl(sefd, VIDIOC_QUERYBUF, &b))
			return sub_fail("enc QUERYBUF");
		scmap[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, sefd, b.m.offset);
		if (scmap[i] == MAP_FAILED || xioctl(sefd, VIDIOC_QBUF, &b))
			return sub_fail("enc coded buffers");
	}
	unsigned ty = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(sefd, VIDIOC_STREAMON, &ty))
		return sub_fail("enc STREAMON capture");
	ty = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (xioctl(sefd, VIDIOC_STREAMON, &ty))
		return sub_fail("enc STREAMON output");
	for (int i = 0; i < NSUBCAM; i++) {
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 .memory = V4L2_MEMORY_MMAP, .index = i };
		if (xioctl(scfd, VIDIOC_QBUF, &b))
			return sub_fail("cam QBUF");
	}
	ty = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(scfd, VIDIOC_STREAMON, &ty))
		return sub_fail("cam STREAMON");
	sub_on = 1;
	LOG("sub-stream %ux%u@%u qp %u: rtsp://<camera>/sub", SW, SH, FPS, qp);
	return 0;
}

/* camera -> encoder -> clients; called every loop iteration */
static void sub_process(void)
{
	int sefd = vstreams[1].efd;
	struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };

	while (!xioctl(scfd, VIDIOC_DQBUF, &b)) {
		if (det && smap[b.index] && mono() - det_last >= det_ms / 1000.0) {
			double t = det_last = mono();
			det_publish(smap[b.index], &b.timestamp);
			if (verbose && det->seq % 50 == 2) {
				static uint8_t tmp[1920];
				double t1 = mono();
				for (unsigned y = 0; y < SH * 3 / 2; y++) {
					memcpy(tmp, smap[b.index] + (size_t)y * sstride, SW);
					__asm__ volatile("" : : "r"(tmp) : "memory");
				}
				LOG("detframe %u: %.1f ms; full-frame row memcpy %.1f ms", det->seq / 2,
				    (t1 - t) * 1e3, (mono() - t1) * 1e3);
			}
		}
		struct v4l2_buffer e = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
					 .memory = V4L2_MEMORY_DMABUF, .index = b.index,
					 .bytesused = ssizeimage, .length = ssizeimage,
					 .timestamp = b.timestamp,
					 .flags = V4L2_BUF_FLAG_TIMESTAMP_COPY };
		e.m.fd = sdmafd[b.index];
		if (xioctl(sefd, VIDIOC_QBUF, &e)) {
			LOG("sub enc QBUF: %s", strerror(errno));
			xioctl(scfd, VIDIOC_QBUF, &b);
		}
		b = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	}
	struct v4l2_buffer e = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF };
	while (!xioctl(sefd, VIDIOC_DQBUF, &e)) {
		struct v4l2_buffer c = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 .memory = V4L2_MEMORY_MMAP, .index = e.index };
		xioctl(scfd, VIDIOC_QBUF, &c);
		e = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF };
	}
	struct v4l2_buffer k = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	while (!xioctl(sefd, VIDIOC_DQBUF, &k)) {
		double sof = k.timestamp.tv_sec + k.timestamp.tv_usec / 1e6;
		int key = !!(k.flags & V4L2_BUF_FLAG_KEYFRAME);
		if (!sub_ncoded++)
			LOG("sub-stream: first coded frame");
		if (key)
			vstreams[1].idr_requested = 0;
		deliver(&vstreams[1], scmap[k.index], k.bytesused, key, sof);
		xioctl(sefd, VIDIOC_QBUF, &k);
		k = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
	}
}

#define DIE(msg) do { LOG("%s: %s", msg, strerror(errno)); exit(1); } while (0)

static void set_ctrl(int fd, unsigned id, int val, const char *name)
{
	struct v4l2_control c = { .id = id, .value = val };
	if (xioctl(fd, VIDIOC_S_CTRL, &c))
		LOG("warn: S_CTRL %s=%d: %s", name, val, strerror(errno));
}

static int find_encoder(char *path, size_t len)
{
	for (int i = 0; i < 16; i++) {
		struct v4l2_capability cap = {0};
		snprintf(path, len, "/dev/video%d", i);
		int fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;
		int ok = !xioctl(fd, VIDIOC_QUERYCAP, &cap) &&
			 !strncmp((char *)cap.driver, "cedrus-enc", 10);
		close(fd);
		if (ok)
			return 0;
	}
	return -1;
}

int main(int argc, char **argv)
{
	char encpath[32] = "", *cam = "/dev/video0";
	unsigned qp = 30, gop = 60, coded_kib = 512, port = 554;
	int opt, want_audio = 1;
	double t0 = mono();

	signal(SIGPIPE, SIG_IGN);
	srand((unsigned)time(NULL) ^ getpid());
	while ((opt = getopt(argc, argv, "c:e:s:S:f:q:g:p:b:a:F:G:M:N:D:n:d:L:WAr:m:E:P:v")) != -1) {
		switch (opt) {
		case 'c': cam = optarg; break;
		case 'e': snprintf(encpath, sizeof(encpath), "%s", optarg); break;
		case 's': sscanf(optarg, "%ux%u", &W, &H); break;
		case 'S':
			if (sscanf(optarg, "%ux%u", &SW, &SH) != 2)
				SW = SH = 0;	/* -S 0: no sub-stream */
			break;
		case 'f': FPS = atoi(optarg); break;
		case 'q': qp = atoi(optarg); break;
		case 'g': gop = atoi(optarg); break;
		case 'p': port = atoi(optarg); break;
		case 'b': coded_kib = atoi(optarg); break;
		case 'a': ae_target = atoi(optarg); break;
		case 'F': ae_flicker = atoi(optarg); break;
		case 'G': ae_gain_max = atoi(optarg) * 64; break;
		case 'M': dn_mode = optarg; break;
		case 'N': dn_night_thr = atoi(optarg); break;
		case 'D': dn_day_thr = atoi(optarg); break;
		case 'n': dn_l_night = atof(optarg); break;
		case 'd': dn_l_day = atof(optarg); break;
		case 'W': awb_on = 0; break;
		case 'L':
			dew_on = 1;
			if (optarg[0] == 'f')
				sscanf(optarg + 1, "%lf,%lf", &dew_fish, &dew_zoom);
			else
				sscanf(optarg, "%lf,%lf,%lf", &dew_k1, &dew_k2, &dew_zoom);
			if (dew_zoom < 0.5 || dew_zoom > 4)
				dew_zoom = 1;
			break;
		case 'A': want_audio = 0; break;
		case 'r': arate = atoi(optarg); break;
		case 'm': amic_gain = atoi(optarg); break;
		case 'E':
			if (!strcasecmp(optarg, "pcmu")) { audio_pt = 0; aenc_name = "PCMU"; }
			else if (!strcasecmp(optarg, "pcma")) { audio_pt = 8; aenc_name = "PCMA"; }
			else { audio_pt = 97; aenc_name = "L16"; }
			break;
		case 'v': verbose++; break;
		case 'P': det_ms = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: camrtsp [-c cam] [-s WxH] [-S subWxH|0] [-f fps] [-q qp] [-g gop] "
				"[-p port] [-b coded_kib] [-a ae_target(0=off)] [-F 50|60|0] [-G max_gain_x]\n"
				"        [-M auto|day|night] [-n night_light -d day_light (scene light, default 8/40)]\n"
				"        [-N night_thr -D day_thr (light sensor raw, instead of -n/-d)] [-W (no AWB)]\n"
				"        [-L f<focal_px>[,zoom] | -L k1[,k2[,zoom]] (EISE lens correction: fisheye\n"
				"         model, e.g. f1100,0.95, or radial polynomial, e.g. -0.15)]\n"
				"        [-A (no audio)] [-E l16|pcmu|pcma] [-r audio_rate] [-m mic_gain 0..31]\n"
				"        [-P ms (detector frames from the sub-stream to " DET_PATH ")] [-v]\n");
			return 1;
		}
	}
	for (int i = 0; i < MAXCLIENTS; i++)
		client_init(&clients[i]);
	if (audio_pt != 97)
		arate = 8000;		/* G.711 is 8 kHz only */
	if (arate < 8000 || arate > 48000)
		arate = 16000;
	{
		struct timespec rt;
		clock_gettime(CLOCK_REALTIME, &rt);
		ntp_offset = rt.tv_sec + rt.tv_nsec / 1e9 - mono() + 2208988800.0;
	}

	/* RTSP listener first, so clients can connect while the camera starts */
	int lfd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in la = { .sin_family = AF_INET, .sin_port = htons(port) };
	if (bind(lfd, (struct sockaddr *)&la, sizeof(la)) || listen(lfd, 4))
		DIE("rtsp listen");
	fcntl(lfd, F_SETFL, O_NONBLOCK);

	if (!encpath[0] && find_encoder(encpath, sizeof(encpath))) {
		LOG("no cedrus-enc video device");
		return 1;
	}
	efd = vstreams[0].efd = open(encpath, O_RDWR | O_NONBLOCK);
	vstreams[0].w = W;
	vstreams[0].h = H;
	int cfd = open(cam, O_RDWR | O_NONBLOCK);
	if (efd < 0 || cfd < 0)
		DIE("open devices");

	struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	f.fmt.pix.width = W; f.fmt.pix.height = H;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	if (xioctl(cfd, VIDIOC_S_FMT, &f)) DIE("cam S_FMT");
	if (f.fmt.pix.width != W || f.fmt.pix.height != H) {
		LOG("camera gave %ux%u", f.fmt.pix.width, f.fmt.pix.height);
		return 1;
	}
	unsigned stride = f.fmt.pix.bytesperline, camsize = f.fmt.pix.sizeimage;

	struct v4l2_format cf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	cf.fmt.pix.width = W; cf.fmt.pix.height = H;
	cf.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	cf.fmt.pix.sizeimage = coded_kib * 1024;
	if (xioctl(efd, VIDIOC_S_FMT, &cf)) DIE("enc S_FMT capture");
	struct v4l2_format of = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	of.fmt.pix.width = W; of.fmt.pix.height = H;
	of.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	of.fmt.pix.bytesperline = stride;
	if (xioctl(efd, VIDIOC_S_FMT, &of)) DIE("enc S_FMT output");
	if (of.fmt.pix.bytesperline != stride || of.fmt.pix.sizeimage > camsize) {
		LOG("stride/size mismatch: enc %u/%u cam %u/%u", of.fmt.pix.bytesperline,
		    of.fmt.pix.sizeimage, stride, camsize);
		return 1;
	}
	struct v4l2_streamparm sp = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	sp.parm.output.timeperframe.numerator = 1;
	sp.parm.output.timeperframe.denominator = FPS;
	xioctl(efd, VIDIOC_S_PARM, &sp);
	set_ctrl(efd, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "prepend_spspps");
	set_ctrl(efd, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, qp, "i_qp");
	set_ctrl(efd, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, qp, "p_qp");
	set_ctrl(efd, V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
		 V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC, "entropy");
	set_ctrl(efd, V4L2_CID_MPEG_VIDEO_GOP_CLOSURE, 1, "gop_closure");
	set_ctrl(efd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "gop_size");

	if (dew_on && (dewarp_open(W, H) || stride % 32)) {
		LOG("dewarp: off (stream continues uncorrected)");
		dew_on = 0;
	}
	unsigned ncam_all = NCAM + (dew_on ? NDEW : 0);
	struct v4l2_requestbuffers rb = { .count = ncam_all, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					  .memory = V4L2_MEMORY_MMAP };
	if (xioctl(cfd, VIDIOC_REQBUFS, &rb) || rb.count != ncam_all) DIE("cam REQBUFS");
	int dmafd[NCAM + NDEW];
	for (unsigned i = 0; i < ncam_all; i++) {
		struct v4l2_exportbuffer e = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = i,
					       .flags = O_RDWR };
		if (xioctl(cfd, VIDIOC_EXPBUF, &e)) DIE("cam EXPBUF");
		dmafd[i] = e.fd;
		struct v4l2_buffer qb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					  .memory = V4L2_MEMORY_MMAP, .index = i };
		if (!xioctl(cfd, VIDIOC_QUERYBUF, &qb)) {
			void *m = mmap(NULL, qb.length, PROT_READ, MAP_SHARED, cfd, qb.m.offset);
			cam_y[i] = m == MAP_FAILED ? NULL : m;
		}
	}
	cam_stride = stride;
	if (ae_target)
		ae_open();
	daynight_init();
	rb = (struct v4l2_requestbuffers){ .count = NCAM, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
					   .memory = V4L2_MEMORY_DMABUF };
	if (xioctl(efd, VIDIOC_REQBUFS, &rb) || rb.count < NCAM) DIE("enc REQBUFS output");
	rb = (struct v4l2_requestbuffers){ .count = NCODED, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					   .memory = V4L2_MEMORY_MMAP };
	if (xioctl(efd, VIDIOC_REQBUFS, &rb) || rb.count < NCODED) DIE("enc REQBUFS capture");
	uint8_t *cmap[NCODED];
	for (int i = 0; i < NCODED; i++) {
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 .memory = V4L2_MEMORY_MMAP, .index = i };
		if (xioctl(efd, VIDIOC_QUERYBUF, &b)) DIE("enc QUERYBUF");
		cmap[i] = mmap(NULL, b.length, PROT_READ, MAP_SHARED, efd, b.m.offset);
		if (cmap[i] == MAP_FAILED) DIE("mmap coded");
		if (xioctl(efd, VIDIOC_QBUF, &b)) DIE("enc QBUF capture");
	}
	unsigned ty = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(efd, VIDIOC_STREAMON, &ty)) DIE("enc STREAMON capture");
	ty = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (xioctl(efd, VIDIOC_STREAMON, &ty)) DIE("enc STREAMON output");
	for (int i = 0; i < NCAM; i++) {
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					 .memory = V4L2_MEMORY_MMAP, .index = i };
		if (xioctl(cfd, VIDIOC_QBUF, &b)) DIE("cam QBUF");
	}
	ty = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(cfd, VIDIOC_STREAMON, &ty)) DIE("cam STREAMON");
	LOG("pipeline %ux%u@%u qp %u gop %u up in %.1f ms; rtsp://<camera>:%u/main",
	    W, H, FPS, qp, gop, (mono() - t0) * 1e3, port);
	if (want_audio)
		audio_open();
	if (SW && SH && !strcmp(cam, "/dev/video0"))
		sub_open("/dev/video1", encpath, qp, gop);

	unsigned ncoded = 0;
	int dew_busy = 0, dew_in = 0, dew_out = 0, dew_free[NDEW] = { 1, 1 };
	unsigned dew_dropped = 0, dew_errors = 0, dew_n = 0;
	double dew_t0 = 0, dew_us_sum = 0, dew_us_max = 0;
	struct timeval dew_ts = { 0 };
	double lat_sum = 0, lat_max = 0, last_stat = mono();
	unsigned lat_n = 0;

	for (;;) {
		struct pollfd pfd[7 + MAXCLIENTS];
		int np = 0, aidx = -1, didx = -1;
		pfd[np++] = (struct pollfd){ .fd = cfd, .events = POLLIN };
		pfd[np++] = (struct pollfd){ .fd = efd, .events = POLLIN | POLLOUT };
		int lidx = np;
		pfd[np++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		if (dew_on) {
			didx = np;
			pfd[np++] = (struct pollfd){ .fd = dew_fd, .events = POLLIN };
		}
		if (sub_on) {
			pfd[np++] = (struct pollfd){ .fd = scfd, .events = POLLIN };
			pfd[np++] = (struct pollfd){ .fd = vstreams[1].efd, .events = POLLIN | POLLOUT };
		}
		if (apcm) {
			aidx = np;
			pfd[np++] = (struct pollfd){ .fd = pcm_get_file_descriptor(apcm), .events = POLLIN };
		}
		int cidx[MAXCLIENTS];
		for (int i = 0; i < MAXCLIENTS; i++) {
			cidx[i] = -1;
			if (clients[i].state == C_FREE)
				continue;
			cidx[i] = np;
			pfd[np++] = (struct pollfd){ .fd = clients[i].fd,
				.events = POLLIN | (clients[i].txlen > clients[i].txoff ? POLLOUT : 0) };
		}
		if (poll(pfd, np, 1000) < 0 && errno != EINTR)
			DIE("poll");

		/* camera -> encoder (zero copy) */
		struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
		while (!xioctl(cfd, VIDIOC_DQBUF, &b)) {
			ae_frame(b.index);
			if (cam_y[b.index])
				awb_frame(cam_y[b.index], cam_y[b.index] + (size_t)cam_stride * H);
			if (dew_on) {
				/* camera buffer -> EISE -> a free dewarp buffer */
				int j = -1;
				for (int k = 0; k < NDEW; k++)
					if (dew_free[k]) { j = k; break; }
				struct eise_job job = { .in_fd = dmafd[b.index],
					.out_fd = j >= 0 ? dmafd[NCAM + j] : -1,
					.in_stride = stride, .out_stride = stride,
					.in_c_offset = stride * H, .out_c_offset = stride * H };
				if (dew_busy || j < 0 || xioctl(dew_fd, EISE_IOC_RUN, &job)) {
					if (!dew_busy && j >= 0)
						LOG("dewarp run: %s", strerror(errno));
					dew_dropped++;
					if (xioctl(cfd, VIDIOC_QBUF, &b)) DIE("cam QBUF");
				} else {
					dew_busy = 1;
					dew_free[j] = 0;
					dew_in = b.index;
					dew_out = j;
					dew_ts = b.timestamp;
					dew_t0 = mono();
				}
				b = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
				continue;
			}
			struct v4l2_buffer e = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
						 .memory = V4L2_MEMORY_DMABUF, .index = b.index,
						 .bytesused = of.fmt.pix.sizeimage, .length = camsize,
						 .timestamp = b.timestamp,
						 .flags = V4L2_BUF_FLAG_TIMESTAMP_COPY };
			e.m.fd = dmafd[b.index];
			if (xioctl(efd, VIDIOC_QBUF, &e)) DIE("enc QBUF output");
			b = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
		}
		/* dewarp finished (or stuck > 300 ms: WAIT resets the engine) */
		if (dew_busy && ((didx >= 0 && (pfd[didx].revents & POLLIN)) || mono() - dew_t0 > 0.3)) {
			struct eise_result res = { 0 };
			if (mono() - dew_t0 > 0.3)
				fcntl(dew_fd, F_SETFL, 0);	/* block in WAIT until reset */
			int r = xioctl(dew_fd, EISE_IOC_WAIT, &res);
			fcntl(dew_fd, F_SETFL, O_NONBLOCK);
			if (!(r < 0 && errno == EAGAIN)) {
				dew_busy = 0;
				struct v4l2_buffer c = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
							 .memory = V4L2_MEMORY_MMAP, .index = dew_in };
				if (xioctl(cfd, VIDIOC_QBUF, &c)) DIE("cam QBUF");
				if (r || res.timeout || (res.status & 6)) {
					LOG("dewarp failed: %s status %x err %x%s", r ? strerror(errno) : "ok",
					    res.status, res.error, res.timeout ? " (timeout)" : "");
					dew_free[dew_out] = 1;
					if (++dew_errors > 10) {
						LOG("dewarp: too many errors, exiting");
						exit(1);
					}
				} else {
					dew_us_sum += res.time_us;
					dew_n++;
					if (res.time_us > dew_us_max)
						dew_us_max = res.time_us;
					struct v4l2_buffer q = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
						.memory = V4L2_MEMORY_DMABUF, .index = dew_out,
						.bytesused = of.fmt.pix.sizeimage, .length = camsize,
						.timestamp = dew_ts, .flags = V4L2_BUF_FLAG_TIMESTAMP_COPY };
					q.m.fd = dmafd[NCAM + dew_out];
					if (xioctl(efd, VIDIOC_QBUF, &q)) DIE("enc QBUF output");
				}
			}
		}
		/* encoder done with a picture -> camera (or back to the dewarp pool) */
		struct v4l2_buffer e = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF };
		while (!xioctl(efd, VIDIOC_DQBUF, &e)) {
			struct v4l2_buffer c = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
						 .memory = V4L2_MEMORY_MMAP, .index = e.index };
			if (dew_on)
				dew_free[e.index] = 1;
			else if (xioctl(cfd, VIDIOC_QBUF, &c)) DIE("cam QBUF");
			e = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF };
		}
		/* coded frame -> clients */
		struct v4l2_buffer k = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
		while (!xioctl(efd, VIDIOC_DQBUF, &k)) {
			double sof = k.timestamp.tv_sec + k.timestamp.tv_usec / 1e6;
			int key = !!(k.flags & V4L2_BUF_FLAG_KEYFRAME);
			if (!ncoded++)
				LOG("first coded frame %.1f ms after start", (mono() - t0) * 1e3);
			if (key && vstreams[0].idr_requested) {
				vstreams[0].idr_requested = 0;
				if (verbose)
					LOG("IDR %.1f ms after SOF", (mono() - sof) * 1e3);
			}
			deliver(&vstreams[0], cmap[k.index], k.bytesused, key, sof);
			double lat = mono() - sof;
			lat_sum += lat; lat_n++;
			if (lat > lat_max) lat_max = lat;
			if (xioctl(efd, VIDIOC_QBUF, &k)) DIE("enc QBUF capture");
			k = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
		}

		if (sub_on)
			sub_process();
		if (aidx >= 0 && pfd[aidx].revents)
			audio_read();

		/* network */
		if (pfd[lidx].revents & POLLIN) {
			struct sockaddr_in pa;
			socklen_t pl = sizeof(pa);
			int fd = accept(lfd, (struct sockaddr *)&pa, &pl);
			if (fd >= 0) {
				struct client *c = NULL;
				for (int i = 0; i < MAXCLIENTS && !c; i++)
					if (clients[i].state == C_FREE)
						c = &clients[i];
				if (!c) {
					close(fd);
				} else {
					client_init(c);
					c->fd = fd;
					c->peer = pa;
					c->tx = malloc(TXBUF);
					c->state = C_RTSP;
					c->last_seen = mono();
					fcntl(fd, F_SETFL, O_NONBLOCK);
					setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
					LOG("client %s:%u connected", inet_ntoa(pa.sin_addr), ntohs(pa.sin_port));
				}
			}
		}
		daynight_tick();
		double now = mono();
		rtcp_tick(now);
		for (int i = 0; i < MAXCLIENTS; i++) {
			struct client *c = &clients[i];
			if (c->state == C_FREE || cidx[i] < 0)
				continue;
			if (pfd[cidx[i]].revents & (POLLERR | POLLHUP)) {
				client_close(c, "hangup");
				continue;
			}
			if (pfd[cidx[i]].revents & POLLIN)
				client_read(c);
			if (c->state != C_FREE && (pfd[cidx[i]].revents & POLLOUT))
				tcp_flush(c);
			/* UDP clients only talk RTSP for keep-alive */
			if (c->state != C_FREE && now - c->last_seen > 65 &&
			    !(c->state == C_PLAYING && c->tcp_interleaved))
				client_close(c, "timeout");
		}
		if (verbose && now - last_stat > 10 && lat_n) {
			LOG("%u frames, SOF->sent avg %.1f max %.1f ms", ncoded,
			    lat_sum / lat_n * 1e3, lat_max * 1e3);
			if (dew_on && dew_n)
				LOG("dewarp: avg %.1f max %.1f ms, %u frames dropped", dew_us_sum / dew_n / 1e3,
				    dew_us_max / 1e3, dew_dropped);
			dew_us_sum = dew_us_max = 0;
			dew_n = 0;
			lat_sum = lat_max = 0; lat_n = 0; last_stat = now;
		}
	}
}
