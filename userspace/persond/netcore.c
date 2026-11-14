// netcore: load a compiled network (ml/compiler.py .nnp) and run it on the NPU.
// NPU ops grouped into firmware tasks, CPU ops (maxpool, 2x upsample) in between,
// on int8 NVDLA surfaces in one arena. Used by netrun (test CLI) and persond.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nn.h"
#include "netcore.h"

double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

enum { OP_CONV = 1, OP_ADD = 2, OP_MAXPOOL = 3, OP_UP2 = 4 };
struct lutp { int32_t p[7]; int16_t T[257]; };


static struct nrt_buf arena, x1s;
#define WCHUNK_MAX	(96 * 1024)	/* CMA is fragmented: no single 1.8 MB block */
#define NWCHUNK		56
static struct nrt_buf wch[NWCHUNK];
static uint32_t wch_start[NWCHUNK + 1];
static int nwch;
int8_t *A;		/* arena CPU pointer */

static void lut_fill(struct dla_lut_param *l, const struct lutp *s)
{
	memset(l, 0, sizeof(*l));
	memcpy(l->linear_only_table, s->T, sizeof(s->T));
	for (int i = 0; i < 65; i++) l->linear_exp_table[i] = 0x7fff;
	l->method = LUT_METHOD_LINEAR;
	l->linear_exp_offset.frac_bits = 30;
	l->linear_exp_start = (uint64_t)(int64_t)0x40000000; l->linear_exp_end = (uint64_t)(int64_t)0x7fffffff;
	l->linear_only_offset.frac_bits = s->p[2];
	l->linear_only_start = (uint64_t)(int64_t)s->p[0]; l->linear_only_end = (uint64_t)(int64_t)s->p[1];
	l->linear_only_underflow_slope.data_i = (struct dla_float_data){ .scale = s->p[3], .shifter = s->p[4] };
	l->linear_only_overflow_slope.data_i = (struct dla_float_data){ .scale = s->p[5], .shifter = s->p[6] };
	l->linear_exp_underflow_slope = l->linear_only_underflow_slope;
	l->linear_exp_overflow_slope = l->linear_only_overflow_slope;
	l->hybrid_priority = l->underflow_priority = l->overflow_priority = LUT_PRI_LINEAR_ONLY;
}

static struct nn_cube cube(const int32_t *c, int buf)
{	/* off, w, h, c, line, surf */
	return (struct nn_cube){ .buf = buf, .offset = c[0], .w = c[1], .h = c[2], .c = c[3], .line = c[4], .surf = c[5] };
}

/* CPU ops on surfaces (uncached arena: read each byte once into a local copy) */
static void cpu_maxpool(const int32_t *o)
{
	const int32_t *ic = o + 1, *oc = o + 7; int k = o[13], pad = o[14];
	int W = ic[1], H = ic[2], S = (ic[3] + 7) / 8;
	size_t n = (size_t)H * W * 8;
	int8_t *in = malloc(n), *out = malloc(n);
	for (int s = 0; s < S; s++) {
		for (int y = 0; y < H; y++) memcpy(in + (size_t)y * W * 8, A + ic[0] + (size_t)s * ic[5] + (size_t)y * ic[4], W * 8);
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++)
				for (int c = 0; c < 8; c++) {
					int m = -128;
					for (int dy = -pad; dy < k - pad; dy++) for (int dx = -pad; dx < k - pad; dx++) {
						int yy = y + dy, xx = x + dx;
						if (yy < 0 || yy >= H || xx < 0 || xx >= W) continue;
						int v = in[((size_t)yy * W + xx) * 8 + c];
						if (v > m) m = v;
					}
					out[((size_t)y * W + x) * 8 + c] = (int8_t)m;
				}
		for (int y = 0; y < H; y++) memcpy(A + oc[0] + (size_t)s * oc[5] + (size_t)y * oc[4], out + (size_t)y * W * 8, W * 8);
	}
	free(in); free(out);
}

static void cpu_up2(const int32_t *o)
{
	const int32_t *ic = o + 1, *oc = o + 7;
	int W = ic[1], H = ic[2], S = (ic[3] + 7) / 8;
	uint64_t *row = malloc((size_t)W * 8), *orow = malloc((size_t)W * 16);
	for (int s = 0; s < S; s++)
		for (int y = 0; y < H; y++) {
			memcpy(row, A + ic[0] + (size_t)s * ic[5] + (size_t)y * ic[4], W * 8);
			for (int x = 0; x < W; x++) orow[2 * x] = orow[2 * x + 1] = row[x];
			for (int dy = 0; dy < 2; dy++)
				memcpy(A + oc[0] + (size_t)s * oc[5] + (size_t)(2 * y + dy) * oc[4], orow, W * 16);
		}
	free(row); free(orow);
}

struct step { int npu; struct nn_net *net; const int32_t *op; int first, last; };

uint32_t hd[16];
uint32_t (*outs)[6];
static struct step *steps;
static int nsteps;
static uint32_t nops, last;

int net_load(const char *path, int stop)
{
	FILE *f = fopen(path, "rb");
		if (!f || fread(hd, 4, 16, f) != 16 || hd[0] != 0x31504e4e) { fprintf(stderr, "bad program\n"); return -1; }
	uint32_t arena_sz = hd[2], wsz = hd[3], x1sz = hd[4], nluts = hd[5], nout = hd[12];
	nops = hd[6];
	outs = malloc(nout * 24); fread(outs, 24, nout, f);
	struct lutp *luts = malloc(sizeof(*luts) * (nluts ? nluts : 1));
	for (uint32_t i = 0; i < nluts; i++) { fread(luts[i].p, 4, 7, f); fread(luts[i].T, 2, 257, f); }
	int32_t (*ops)[48] = malloc(nops * sizeof(*ops)); fread(ops, sizeof(*ops), nops, f);
	if (nrt_open()) return -1;
	if (nrt_alloc(&arena, (arena_sz + 4095) & ~4095u) || nrt_alloc(&x1s, (x1sz + 4095) & ~4095u)) {
		fprintf(stderr, "out of NPU memory (arena %u)\n", arena_sz); return -1;
	}
	A = arena.va;
	/* weights: chunks cut at op boundaries (a layer's weights stay in one chunk) */
	{
		uint32_t start = 0, end = 0;
		for (uint32_t i = 0; i <= nops; i++) {
			uint32_t ws = i < nops && ops[i][0] == OP_CONV ? ops[i][13] : 0;
			uint32_t we = i < nops && ops[i][0] == OP_CONV ? ops[i][13] + ops[i][14] : 0;
			if (i == nops || (we > start + WCHUNK_MAX && end > start && ws >= end)) {
				if (i == nops) end = wsz;
				if (nwch >= NWCHUNK || nrt_alloc(&wch[nwch], (end - start + 4095) & ~4095u)) {
					fprintf(stderr, "out of NPU memory (weights chunk %d, %u bytes)\n", nwch, end - start); return -1;
				}
				wch_start[nwch++] = start;
				start = end;
			}
			if (we > end) end = we;
		}
		wch_start[nwch] = wsz;
		for (int k = 0; k < nwch; k++) {
			uint32_t len = wch_start[k + 1] - wch_start[k];
			if (fread(wch[k].va, 1, len, f) != len) { fprintf(stderr, "short weights\n"); return -1; }
		}
	}
	if (fread(x1s.va, 1, x1sz, f) != x1sz) { fprintf(stderr, "short program\n"); return -1; }
	fclose(f);
	/* build steps */
	steps = calloc(nops, sizeof(*steps)); nsteps = 0;
	last = stop >= 0 && (uint32_t)stop < nops ? (uint32_t)stop + 1 : nops;
	for (uint32_t i = 0; i < last; ) {
		struct step *s = &steps[nsteps++];
		s->first = i; s->op = ops[i];
		if (ops[i][0] == OP_MAXPOOL || ops[i][0] == OP_UP2) { s->npu = 0; s->last = i++; continue; }
		s->npu = 1;
		struct nn_net *n = s->net = calloc(1, sizeof(*n));
		int chain_cap = getenv("NRT_CHAIN") ? NN_MAX_OPS : 4;
		if (nn_init(n, chain_cap, getenv("NRT_CHAIN") ? 64 : 1)) { fprintf(stderr, "no memory\n"); return -1; }
		int ba = nn_add_buf(n, &arena), bx = nn_add_buf(n, &x1s), bw0 = -1;
		for (int k = 0; k < nwch; k++) { int b = nn_add_buf(n, &wch[k]); if (!k) bw0 = b; }
		int lutmap[256]; for (int k = 0; k < 256; k++) lutmap[k] = -1;
		/* one NPU op per task unless NRT_CHAIN is set: consecutive ops of a unit
		 * inside one task use register group 1, which hung the V831 */
		int chain = getenv("NRT_CHAIN") != NULL;
		for (int first = 1; i < last && (ops[i][0] == OP_CONV || ops[i][0] == OP_ADD) && (chain || first); i++, first = 0) {
			const int32_t *o = ops[i];
			if (n->nops + 2 > n->max_ops) break;
			if (o[0] == OP_ADD) {
				struct nn_add a = { .a = cube(o + 1, ba), .b = cube(o + 7, ba), .out = cube(o + 13, ba),
					.out_cvt = { .scale = (int16_t)o[19], .truncate = (uint8_t)o[20], .enable = 1, .offset = o[21] } };
				if (nn_add(n, &a) < 0) return -1;
				continue;
			}
			/* conv: in(6) out(6) woff wsize k r s stride pt pb pl pr x1off sh t act lut cs ct co */
			const int32_t *q = o + 13;
			struct nn_conv c = { 0 };
			c.in = cube(o + 1, ba); c.out = cube(o + 7, ba);
			{	/* weights chunk holding this op's weights */
				int k = 0;
				while (k + 1 < nwch && (uint32_t)q[0] >= wch_start[k + 1]) k++;
				c.wbuf = bw0 + k; c.woff = q[0] - wch_start[k]; c.wsize = q[1];
			}
			c.k = q[2]; c.r = q[3]; c.s = q[4]; c.stride = q[5];
			c.pad_t = q[6]; c.pad_b = q[7]; c.pad_l = q[8]; c.pad_r = q[9];
			c.x1_type = SDP_OP_BOTH; c.x1_buf = bx; c.x1_off = q[10]; c.x1_precision = PRECISION_INT16;
			c.x1_shift = q[11]; c.x1_truncate = q[12];
			c.act = q[13]; c.lut = -1;
			if (c.act == ACTIVATION_LUT) {
				if (lutmap[q[14]] < 0) { struct dla_lut_param l; lut_fill(&l, &luts[q[14]]); lutmap[q[14]] = nn_add_lut(n, &l); }
				c.lut = lutmap[q[14]];
			}
			c.out_cvt = (struct dla_cvt_param){ .scale = (int16_t)q[15], .truncate = (uint8_t)q[16], .enable = 1, .offset = q[17] };
			c.conv_out_cvt = (struct dla_cvt_param){ .scale = 1, .truncate = 0, .enable = 1 };
			if (nn_conv(n, &c, 1) < 0) { fprintf(stderr, "op %u rejected\n", i); return -1; }
			if (n->nluts >= n->max_luts && chain) { i++; break; }
		}
		s->last = i - 1;
		if (nn_finalize(n)) return -1;
	}
	fprintf(stderr, "%d steps (%u ops), arena %u KB weights %u KB in %d chunks\n", nsteps, last, hd[2] / 1024, hd[3] / 1024, nwch);
	return 0;
}

int net_run(double *npu_ms, double *cpu_ms)
{
	double tn = 0, tc = 0;
		for (int k = 0; k < nsteps; k++) {
			double a = now_ms();
			if (steps[k].npu) {
				uint32_t us;
				int r = nn_run(steps[k].net, &us);
				if (r) { fprintf(stderr, "step %d (ops %d..%d) failed: %d\n", k, steps[k].first, steps[k].last, r); return -2; }
				tn += now_ms() - a;
			} else {
				if (steps[k].op[0] == OP_MAXPOOL) cpu_maxpool(steps[k].op); else cpu_up2(steps[k].op);
				tc += now_ms() - a;
			}
		}
	if (npu_ms) *npu_ms = tn;
	if (cpu_ms) *cpu_ms = tc;
	return 0;
}
