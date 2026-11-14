/* nn: build NVDLA task descriptors (current firmware structs) for conv+SDP layers.
 * Buffers are registered in the address list; index 0..4 are the tables. */
#ifndef NN_H
#define NN_H
#include <dla_interface.h>
#include "nrt.h"

#define NN_MAX_OPS	512
#define NN_MAX_BUFS	64

struct nn_cube {		/* a feature cube in an int8 NVDLA surface buffer */
	int buf;		/* address list index */
	uint32_t offset;
	uint16_t w, h, c;
	uint32_t line, surf;	/* strides; 0 = packed (w*8, h*line) */
};

struct nn_conv {
	struct nn_cube in, out;
	int wbuf; uint32_t woff, wsize;		/* weights, NVDLA layout */
	uint16_t k, r, s;			/* kernels, kernel h/w */
	uint8_t stride, pad_l, pad_r, pad_t, pad_b;
	/* SDP x1 (BS): per-kernel ALU (add) and/or MUL operands from memory */
	int x1_type;				/* SDP_OP_NONE/ADD/MUL/BOTH */
	int x1_buf; uint32_t x1_off;
	uint8_t x1_precision;			/* PRECISION_INT8/INT16 operand size */
	uint8_t x1_shift, x1_truncate;		/* ALU operand << shift; MUL result >> truncate */
	uint8_t act;				/* ACTIVATION_NONE/RELU/LUT */
	int lut;				/* index into the LUT list, -1 none */
	struct dla_cvt_param out_cvt;		/* SDP output converter */
	struct dla_cvt_param conv_out_cvt;	/* CACC output (truncate) */
};

struct nn_net {
	struct nrt_buf bufs[NN_MAX_BUFS];	/* address list */
	int nbufs;
	struct nrt_buf tables;			/* one allocation for indices 0..4 */
	int nops, max_ops;
	struct dla_common_op_desc *dep;
	union dla_operation_container *ops;
	union dla_surface_container *surf;
	struct dla_lut_param *luts;
	int nluts, max_luts;
	int last_sdp;				/* op index of the previous SDP op */
	int last_conv;				/* op index of the previous CONV op */
	int16_t op_head[DLA_OP_NUM];
};

/* capacity: max_ops descriptors, max_luts LUTs (allocated here) */
int nn_init(struct nn_net *n, int max_ops, int max_luts);
int nn_add_buf(struct nn_net *n, struct nrt_buf *b);	/* returns address index */
int nn_add_lut(struct nn_net *n, const struct dla_lut_param *l);
int nn_conv(struct nn_net *n, const struct nn_conv *c, int depends_on_prev);
/* standalone SDP: out = sat8(cvt(a + b)) with b a per-point int8 tensor (residual add) */
struct nn_add {
	struct nn_cube a, b, out;
	struct dla_cvt_param out_cvt;
};
int nn_add(struct nn_net *n, const struct nn_add *a);
int nn_finalize(struct nn_net *n);			/* writes the tables */
int nn_run(struct nn_net *n, uint32_t *us);
#endif
