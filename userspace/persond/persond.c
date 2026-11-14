// persond: person + cat detector on the NPU (YOLOv5n, ml/compiler.py program).
//
// Reads the letterboxed 320x192 RGB frames camrtsp publishes in /tmp/detframe
// (camrtsp -P), runs the network, keeps the COCO classes "person" (0) and
// "cat" (15), and turns each into a debounced present/absent state:
//   /tmp/person.state   key=value lines, rewritten after every frame (atomic rename):
//                       <class>=0|1, <class>_since, <class>_count, <class>_score,
//                       <class>_boxes (x0,y0,x1,y1@score, 0..1 of the picture)
//   -u/-U URL           plain-HTTP GET when a person appears / is gone (e.g. the
//                       homebridge-camera-ffmpeg motion endpoint); -a/-A for a cat
//
//   persond [-m prog.nnp] [-f detframe] [-i interval_ms] [-t score] [-T cat_score]
//           [-n frames_on] [-H hold_s] [-u on_url] [-U off_url] [-a cat_on_url]
//           [-A cat_off_url] [-v]
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "netcore.h"

#define STATE_PATH	"/tmp/person.state"
#define NHEAD		3
#define NA		3
#define NO		85		/* x y w h obj + 80 COCO classes; person = class 0 */
#define MAXDET		64

struct detframe {			/* camrtsp.c */
	uint32_t magic, seq, w, h, src_w, src_h, box_x, box_y, box_w, box_h;
	uint64_t mono_ms;
	uint8_t rgb[];
};

static const float anchors[NHEAD][NA][2] = {
	{ { 10, 13 }, { 16, 30 }, { 33, 23 } },
	{ { 30, 61 }, { 62, 45 }, { 59, 119 } },
	{ { 116, 90 }, { 156, 198 }, { 373, 326 } },
};

struct det { float score, x0, y0, x1, y1; };	/* normalised to the picture (0..1) */

#define NCLS		2
struct cls {
	const char *name;
	int id;				/* COCO class */
	float thr;
	const char *on_url, *off_url;
	int present, streak, nd;
	time_t since;
	double last_seen;
	struct det dets[MAXDET];
};
static struct cls classes[NCLS] = {
	{ .name = "person", .id = 0 },
	{ .name = "cat", .id = 15 },
};

static int verbose;
static volatile sig_atomic_t stop;

static void logmsg(const char *fmt, ...)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	fprintf(stderr, "[%6ld.%03ld] ", (long)t.tv_sec, t.tv_nsec / 1000000);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---------------------------------------------------------------- decode */

static float sig_lut[NHEAD][256];	/* sigmoid(q * scale) for every int8 q */
static int8_t *hbuf[NHEAD];		/* cached copy of the surfaces we read */

static float iou(const struct det *a, const struct det *b)
{
	float ix = fminf(a->x1, b->x1) - fmaxf(a->x0, b->x0);
	float iy = fminf(a->y1, b->y1) - fmaxf(a->y0, b->y0);
	if (ix <= 0 || iy <= 0)
		return 0;
	float in = ix * iy;
	return in / ((a->x1 - a->x0) * (a->y1 - a->y0) + (b->x1 - b->x0) * (b->y1 - b->y0) - in);
}

static int cmp_score(const void *a, const void *b)
{
	float d = ((const struct det *)b)->score - ((const struct det *)a)->score;
	return (d > 0) - (d < 0);
}

/*
 * YOLOv5 head decode for class 0 only. Output channel ch of head i at (y, x)
 * is A[off + (ch / 8) * surf + y * line + x * 8 + ch % 8]; per anchor only
 * channels a*85 .. a*85+5 are needed, which span at most two surfaces: those
 * are copied out of the uncached arena once.
 */
static void decode(const struct detframe *fr)
{
	static struct det cand[NCLS][512];
	int n[NCLS] = { 0 };
	const int in_h = hd[11];
	float thr = classes[0].thr;
	for (int k = 1; k < NCLS; k++)
		if (classes[k].thr < thr)
			thr = classes[k].thr;
	int maxch = 0;
	for (int k = 0; k < NCLS; k++)
		if (5 + classes[k].id > maxch)
			maxch = 5 + classes[k].id;

	for (int i = 0; i < NHEAD; i++) {
		uint32_t off = outs[i][0], line = outs[i][1], surf = outs[i][2];
		int gh = outs[i][4], gw = outs[i][5];
		float stride = (float)in_h / gh;
		for (int a = 0; a < NA; a++) {
			int c0 = a * NO, s0 = c0 / 8, s1 = (c0 + maxch) / 8;
			for (int s = s0; s <= s1; s++)
				memcpy(hbuf[i] + (size_t)s * surf, A + off + (size_t)s * surf, surf);
#define V(ch, y, x) hbuf[i][(size_t)((ch) / 8) * surf + (size_t)(y) * line + (size_t)(x) * 8 + (ch) % 8]
			for (int y = 0; y < gh; y++)
				for (int x = 0; x < gw; x++) {
					float obj = sig_lut[i][(uint8_t)V(c0 + 4, y, x)];
					if (obj < thr)
						continue;
					for (int k = 0; k < NCLS; k++) {
						struct cls *C = &classes[k];
						if (obj < C->thr)
							continue;
						float sc = obj * sig_lut[i][(uint8_t)V(c0 + 5 + C->id, y, x)];
						if (sc < C->thr || n[k] == 512)
							continue;
						float tx = sig_lut[i][(uint8_t)V(c0 + 0, y, x)], ty = sig_lut[i][(uint8_t)V(c0 + 1, y, x)];
						float tw = sig_lut[i][(uint8_t)V(c0 + 2, y, x)], th = sig_lut[i][(uint8_t)V(c0 + 3, y, x)];
						float cx = (tx * 2 - 0.5f + x) * stride, cy = (ty * 2 - 0.5f + y) * stride;
						float w = 4 * tw * tw * anchors[i][a][0], h = 4 * th * th * anchors[i][a][1];
						/* letterbox -> picture coordinates, 0..1 */
						struct det *d = &cand[k][n[k]++];
						d->score = sc;
						d->x0 = fmaxf(0, (cx - w / 2 - fr->box_x) / fr->box_w);
						d->x1 = fminf(1, (cx + w / 2 - fr->box_x) / fr->box_w);
						d->y0 = fmaxf(0, (cy - h / 2 - fr->box_y) / fr->box_h);
						d->y1 = fminf(1, (cy + h / 2 - fr->box_y) / fr->box_h);
					}
				}
#undef V
		}
	}
	for (int c = 0; c < NCLS; c++) {
		struct det *out = classes[c].dets;
		qsort(cand[c], n[c], sizeof(cand[c][0]), cmp_score);
		int k = 0;
		for (int j = 0; j < n[c] && k < MAXDET; j++) {
			int keep = cand[c][j].x1 > cand[c][j].x0 && cand[c][j].y1 > cand[c][j].y0;
			for (int m = 0; m < k && keep; m++)
				if (iou(&cand[c][j], &out[m]) > 0.45f)
					keep = 0;
			if (keep)
				out[k++] = cand[c][j];
		}
		classes[c].nd = k;
	}
}

/* RGB888 letterboxed frame -> network input surface (u8 - 128, 3 of 8 channels) */
static void load_input(const struct detframe *fr)
{
	const int W = hd[10], H = hd[11];
	const uint32_t line = hd[8];
	static uint8_t row[320 * 8];
	for (int y = 0; y < H; y++) {
		const uint8_t *p = fr->rgb + (size_t)y * W * 3;
		for (int x = 0; x < W; x++) {
			uint8_t *o = row + x * 8;
			o[0] = p[0] ^ 0x80; o[1] = p[1] ^ 0x80; o[2] = p[2] ^ 0x80;	/* u8 - 128 */
			o[3] = o[4] = o[5] = o[6] = o[7] = 0;
			p += 3;
		}
		memcpy(A + hd[7] + (size_t)y * line, row, (size_t)W * 8);
	}
}

/* ------------------------------------------------------------------ HTTP */

/* GET http://host[:port]/path, 2 s timeouts; returns the HTTP status or -1 */
static int http_get(const char *url)
{
	char host[128], port[8] = "80", path[512] = "/";
	const char *p = url;
	if (strncmp(p, "http://", 7))
		return -1;
	p += 7;
	size_t hl = strcspn(p, ":/");
	if (hl == 0 || hl >= sizeof(host))
		return -1;
	memcpy(host, p, hl); host[hl] = 0; p += hl;
	if (*p == ':') {
		size_t pl = strcspn(++p, "/");
		if (pl == 0 || pl >= sizeof(port))
			return -1;
		memcpy(port, p, pl); port[pl] = 0; p += pl;
	}
	if (*p)
		snprintf(path, sizeof(path), "%s", p);

	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai;
	if (getaddrinfo(host, port, &hints, &ai))
		return -1;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), status = -1;
	if (fd < 0) { freeaddrinfo(ai); return -1; }
	if (connect(fd, ai->ai_addr, ai->ai_addrlen) && errno != EINPROGRESS)
		goto out;
	struct pollfd pf = { .fd = fd, .events = POLLOUT };
	int err = 0; socklen_t el = sizeof(err);
	if (poll(&pf, 1, 2000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) || err)
		goto out;
	char req[768];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
	if (write(fd, req, n) != n)
		goto out;
	char resp[64];
	pf.events = POLLIN;
	if (poll(&pf, 1, 2000) == 1 && (n = read(fd, resp, sizeof(resp) - 1)) > 12) {
		resp[n] = 0;
		if (!strncmp(resp, "HTTP/1.", 7))
			status = atoi(resp + 9);
	}
out:
	close(fd);
	freeaddrinfo(ai);
	return status;
}

/* ----------------------------------------------------------------- state */

static void write_state(double ms, unsigned frames)
{
	FILE *f = fopen(STATE_PATH ".new", "w");
	if (!f)
		return;
	for (int c = 0; c < NCLS; c++) {
		const struct cls *C = &classes[c];
		fprintf(f, "%s=%d\n%s_since=%ld\n%s_count=%d\n%s_score=%.2f\n%s_boxes=", C->name, C->present,
			C->name, (long)C->since, C->name, C->nd, C->name, C->nd ? C->dets[0].score : 0.0f, C->name);
		for (int i = 0; i < C->nd && i < 8; i++)
			fprintf(f, "%s%.3f,%.3f,%.3f,%.3f@%.2f", i ? ";" : "", C->dets[i].x0, C->dets[i].y0,
				C->dets[i].x1, C->dets[i].y1, C->dets[i].score);
		fputc('\n', f);
	}
	fprintf(f, "updated=%ld\nframes=%u\ninfer_ms=%.1f\n", (long)time(NULL), frames, ms);
	fclose(f);
	rename(STATE_PATH ".new", STATE_PATH);
}

static void on_signal(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	const char *prog = "/lib/npu/yolov5n.nnp";
	const char *DET_PATH = "/tmp/detframe";
	int interval = 250, frames_on = 2, hold = 10, opt;
	float thr = 0.45f, cat_thr = -1;

	while ((opt = getopt(argc, argv, "m:f:i:t:T:n:H:u:U:a:A:v")) != -1) {
		switch (opt) {
		case 'm': prog = optarg; break;
		case 'f': DET_PATH = optarg; break;
		case 'i': interval = atoi(optarg); break;
		case 't': thr = atof(optarg); break;
		case 'n': frames_on = atoi(optarg); break;
		case 'H': hold = atoi(optarg); break;
		case 'T': cat_thr = atof(optarg); break;
		case 'u': classes[0].on_url = optarg; break;
		case 'U': classes[0].off_url = optarg; break;
		case 'a': classes[1].on_url = optarg; break;
		case 'A': classes[1].off_url = optarg; break;
		case 'v': verbose++; break;
		default:
			fprintf(stderr, "usage: persond [-m prog.nnp] [-f detframe] [-i interval_ms (250)] [-t score (0.45)]\n"
				"               [-T cat_score (= -t)] [-n frames_on (2)] [-H hold_s (10)]\n"
				"               [-u person_on_url] [-U person_off_url] [-a cat_on_url] [-A cat_off_url] [-v]\n");
			return 1;
		}
	}
	if (interval < 50) interval = 50;
	if (frames_on < 1) frames_on = 1;
	classes[0].thr = thr;
	classes[1].thr = cat_thr > 0 ? cat_thr : thr;
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	setpriority(PRIO_PROCESS, 0, 10);	/* the stream comes first */

	if (net_load(prog, -1))
		return 1;
	if (hd[12] != NHEAD || hd[10] != 320 || hd[11] != 192) {
		logmsg("unexpected program: %u outputs, input %ux%u", hd[12], hd[10], hd[11]);
		return 1;
	}
	for (int i = 0; i < NHEAD; i++) {
		float sc;
		memcpy(&sc, &hd[13 + i], 4);
		if (!(sc > 0 && sc < 10)) {
			logmsg("program has no output scales (rebuild with ml/compiler.py)");
			return 1;
		}
		for (int q = 0; q < 256; q++)
			sig_lut[i][q] = 1 / (1 + expf(-(int8_t)q * sc));
		hbuf[i] = malloc((size_t)outs[i][2] * ((outs[i][3] + 7) / 8));
		if (!hbuf[i])
			return 1;
	}

	/* the frame file appears once camrtsp runs; reopen if camrtsp restarts */
	const struct detframe *fr = NULL;
	ino_t ino = 0;
	uint32_t last_seq = 0;
	for (int c = 0; c < NCLS; c++)
		classes[c].since = time(NULL);
	double sum_ms = 0, t_stat = now_ms();
	unsigned frames = 0, nstat = 0;
	size_t fsz = sizeof(struct detframe) + 320 * 192 * 3;
	static uint8_t copy[sizeof(struct detframe) + 320 * 192 * 3];

	logmsg("persond: %s, every %d ms, person >= %.2f, cat >= %.2f, on after %d frames, off after %d s",
	       prog, interval, classes[0].thr, classes[1].thr, frames_on, hold);
	write_state(0, 0);
	while (!stop) {
		struct stat st;
		if (stat(DET_PATH, &st) || (size_t)st.st_size < fsz) {
			sleep(1);
			continue;
		}
		if (!fr || st.st_ino != ino) {
			if (fr)
				munmap((void *)fr, fsz);
			int fd = open(DET_PATH, O_RDONLY);
			fr = fd < 0 ? NULL : mmap(NULL, fsz, PROT_READ, MAP_SHARED, fd, 0);
			if (fd >= 0)
				close(fd);
			if (fr == MAP_FAILED || !fr || fr->magic != 0x46544544 || fr->w != 320 || fr->h != 192) {
				if (fr && fr != MAP_FAILED)
					munmap((void *)fr, fsz);
				fr = NULL;
				sleep(1);
				continue;
			}
			ino = st.st_ino;
			last_seq = 0;
			logmsg("frames from %s (%ux%u sub-stream)", DET_PATH, fr->src_w, fr->src_h);
		}
		/* newest complete frame (seqlock) */
		uint32_t s1 = __atomic_load_n(&fr->seq, __ATOMIC_ACQUIRE);
		if ((s1 & 1) || s1 == last_seq) {
			usleep(20000);
			continue;
		}
		memcpy(copy, fr, fsz);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		if (__atomic_load_n(&fr->seq, __ATOMIC_ACQUIRE) != s1)
			continue;
		last_seq = s1;
		const struct detframe *f = (const struct detframe *)copy;

		double t0 = now_ms();
		load_input(f);
		if (net_run(NULL, NULL)) {
			logmsg("NPU run failed");
			return 2;
		}
		decode(f);
		double ms = now_ms() - t0, now = t0 / 1000;
		frames++;
		sum_ms += ms; nstat++;

		for (int c = 0; c < NCLS; c++) {
			struct cls *C = &classes[c];
			C->streak = C->nd ? C->streak + 1 : 0;
			if (C->nd)
				C->last_seen = now;
			if (!C->present && C->streak >= frames_on) {
				C->present = 1; C->since = time(NULL);
				logmsg("%s: yes (%d, best %.2f at %.2f,%.2f-%.2f,%.2f)", C->name, C->nd, C->dets[0].score,
				       C->dets[0].x0, C->dets[0].y0, C->dets[0].x1, C->dets[0].y1);
				if (C->on_url)
					logmsg("%s on hook: HTTP %d", C->name, http_get(C->on_url));
			} else if (C->present && now - C->last_seen >= hold) {
				C->present = 0; C->since = time(NULL);
				logmsg("%s: no", C->name);
				if (C->off_url)
					logmsg("%s off hook: HTTP %d", C->name, http_get(C->off_url));
			}
		}
		write_state(ms, frames);
		if (verbose > 1 || (verbose && (classes[0].nd || classes[1].nd)))
			logmsg("frame %u: %d person(s), %d cat(s), %.1f ms", s1 / 2, classes[0].nd, classes[1].nd, ms);
		if (now_ms() - t_stat > 600e3) {
			logmsg("status: %u frames, %.1f ms average", nstat, sum_ms / nstat);
			sum_ms = 0; nstat = 0; t_stat = now_ms();
		}
		/* pace: next frame no sooner than interval after this one started */
		double wait = interval - (now_ms() - t0);
		if (wait > 0)
			usleep((useconds_t)(wait * 1000));
	}
	for (int c = 0; c < NCLS; c++) {
		classes[c].present = 0; classes[c].nd = 0;
	}
	write_state(0, frames);
	return 0;
}
