// netrun PROGRAM.nnp INPUT OUT [loops] [stop_after_op ARENA_DUMP]
// Runs a compiled network (ml/compiler.py) via netcore; writes the head outputs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netcore.h"

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: netrun prog.nnp input out [loops] [stop_op dump]\n"); return 1; }
	int loops = argc > 4 ? atoi(argv[4]) : 1;
	int stop = argc > 6 ? atoi(argv[5]) : -1;
	if (net_load(argv[1], stop)) return 1;
	FILE *f = fopen(argv[2], "rb");
	size_t isz = (size_t)hd[9] * 1;	/* one surface (8 channels) */
	int8_t *input = malloc(isz);	/* the arena slot is reused after layer 0: keep a copy */
	if (!f || fread(input, 1, isz, f) != isz) { fprintf(stderr, "bad input\n"); return 1; }
	fclose(f);
	double best = 1e9, npu_ms = 0, cpu_ms = 0;
	for (int l = 0; l < loops; l++) {
		memcpy(A + hd[7], input, isz);	/* a new frame */
		double t0 = now_ms(), tn, tc;
		if (net_run(&tn, &tc)) return 2;
		double t = now_ms() - t0;
		if (t < best) { best = t; npu_ms = tn; cpu_ms = tc; }
	}
	fprintf(stderr, "best of %d: %.2f ms (NPU steps %.2f ms, CPU ops %.2f ms)\n", loops, best, npu_ms, cpu_ms);
	if (stop >= 0) {
		FILE *o = fopen(argv[6], "wb"); fwrite(A, 1, hd[2], o); fclose(o);
	}
	FILE *o = fopen(argv[3], "wb");
	for (uint32_t i = 0; i < hd[12]; i++)
		fwrite(A + outs[i][0], 1, (size_t)outs[i][2] * ((outs[i][3] + 7) / 8), o);
	fclose(o);
	return 0;
}
