// netcore: compiled-network loader/runner (see netcore.c)
#pragma once
#include <stdint.h>
extern uint32_t hd[16];		/* program header: arena, sizes, input cube, nout, output scales */
extern uint32_t (*outs)[6];	/* nout x (off, line, surf, c, h, w) */
extern int8_t *A;		/* arena (uncached) */
double now_ms(void);
int net_load(const char *path, int stop_after_op);	/* -1 = whole network */
int net_run(double *npu_ms, double *cpu_ms);		/* input already in the arena */
