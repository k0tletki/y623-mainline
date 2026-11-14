#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nn.h"

#define CBUF_BANKS	32
#define CBUF_BANK_BYTES	4096	/* 512 entries x 8 bytes */

static void cube_fill(struct dla_data_cube *d, const struct nn_cube *c)
{
	uint32_t line = c->line ? c->line : (uint32_t)c->w * 8;
	uint32_t surf = c->surf ? c->surf : line * c->h;
	d->type = DLA_MEM_MC;
	d->address = c->buf;
	d->offset = c->offset;
	d->width = c->w; d->height = c->h; d->channel = c->c;
	d->line_stride = line; d->surf_stride = surf;
	d->size = surf * ((c->c + 7) / 8);
}

int nn_init(struct nn_net *n, int max_ops, int max_luts)
{
	memset(n, 0, sizeof(*n));
	n->max_ops = max_ops; n->max_luts = max_luts;
	n->dep = calloc(max_ops, sizeof(*n->dep));
	n->ops = calloc(max_ops, sizeof(*n->ops));
	n->surf = calloc(max_ops, sizeof(*n->surf));
	n->luts = calloc(max_luts ? max_luts : 1, sizeof(*n->luts));
	if (!n->dep || !n->ops || !n->surf || !n->luts) return -1;
	n->nbufs = 5;			/* 0 net, 1 ops, 2 surfaces, 3 deps, 4 luts */
	n->last_sdp = n->last_conv = -1;
	for (int i = 0; i < DLA_OP_NUM; i++) n->op_head[i] = -1;
	return 0;
}

int nn_add_buf(struct nn_net *n, struct nrt_buf *b)
{
	if (n->nbufs >= NN_MAX_BUFS) return -1;
	n->bufs[n->nbufs] = *b;
	return n->nbufs++;
}

int nn_add_lut(struct nn_net *n, const struct dla_lut_param *l)
{
	if (n->nluts >= n->max_luts) return -1;
	n->luts[n->nluts] = *l;
	return n->nluts++;
}

static void dep_init(struct dla_common_op_desc *d, int index, int type)
{
	memset(d, 0, sizeof(*d));
	d->index = index; d->roi_index = 0; d->op_type = type;
	for (int i = 0; i < DLA_OP_NUM; i++) d->consumers[i].index = -1;
	d->fused_parent.index = -1;
}

int nn_conv(struct nn_net *n, const struct nn_conv *c, int depends_on_prev)
{
	int ic = n->nops, is = n->nops + 1;
	if (is >= n->max_ops) return -1;
	uint16_t ho = (c->in.h + c->pad_t + c->pad_b - c->r) / c->stride + 1;
	uint16_t wo = (c->in.w + c->pad_l + c->pad_r - c->s) / c->stride + 1;
	if (ho != c->out.h || wo != c->out.w) {
		fprintf(stderr, "nn_conv: output %ux%u, expected %ux%u\n", c->out.w, c->out.h, wo, ho);
		return -1;
	}
	/* --- CONV */
	struct dla_conv_op_desc *co = &n->ops[ic].conv_op;
	struct dla_conv_surface_desc *cs = &n->surf[ic].conv_surface;
	memset(co, 0, sizeof(*co)); memset(cs, 0, sizeof(*cs));
	uint32_t atoms = (c->in.c + 7) / 8;
	uint32_t eps = c->in.w * atoms;			/* CBUF entries per input line */
	uint32_t dbank = (eps * c->in.h + 511) / 512;
	uint32_t wbank = (c->wsize + CBUF_BANK_BYTES - 1) / CBUF_BANK_BYTES;
	/* measured on the V831: input data in 16 or more banks comes out as garbage
	 * (weights can use up to the rest of the 32 banks) */
	if (dbank > 15 || dbank + wbank > CBUF_BANKS) {
		fprintf(stderr, "nn_conv: does not fit CBUF (%u data + %u weight banks)\n", dbank, wbank);
		return -1;
	}
	co->conv_mode = CONV_MODE_DIRECT;
	co->data_reuse = co->weight_reuse = 0;
	co->skip_data_rls = co->skip_weight_rls = 0;
	co->entry_per_slice = eps;
	co->data_format = FORMAT_FEATURE;
	co->pixel_mapping = MAP_PITCH_LINEAR;
	co->fetch_grain = 1;
	co->batch = 1;
	co->weight_format = WEIGHT_FORMAT_UNCOMPRESSED;
	co->data_bank = dbank;
	co->weight_bank = wbank;
	co->release = c->in.h;
	co->input_width_csc = c->in.w; co->input_height_csc = c->in.h; co->input_channel_csc = c->in.c;
	co->kernel_width_csc = c->s; co->kernel_height_csc = c->r; co->kernel_channel_csc = c->in.c;
	co->input_width_cmac = wo; co->input_height_cmac = ho;
	co->bytes_per_kernel = (uint32_t)c->r * c->s * c->in.c;
	co->mean_format = MEAN_FORMAT_DISABLE;
	co->conv_stride_x = co->conv_stride_y = c->stride;
	co->pad_x_left = c->pad_l; co->pad_x_right = c->pad_r;
	co->pad_y_top = c->pad_t; co->pad_y_bottom = c->pad_b;
	co->dilation_x = co->dilation_y = 1;
	co->pra_truncate = 0;
	co->in_precision = co->out_precision = PRECISION_INT8;
	co->pad_val = 0;
	co->out_cvt = c->conv_out_cvt;
	cs->weight_data = (struct dla_data_cube){ .type = DLA_MEM_MC, .address = c->wbuf, .offset = c->woff,
		.size = c->wsize, .width = c->s, .height = c->r, .channel = c->in.c };
	cube_fill(&cs->src_data, &c->in);
	cs->dst_data = (struct dla_data_cube){ .type = DLA_MEM_HW, .address = -1, .width = wo, .height = ho,
		.channel = c->k };
	/* --- SDP */
	struct dla_sdp_op_desc *so = &n->ops[is].sdp_op;
	struct dla_sdp_surface_desc *ss = &n->surf[is].sdp_surface;
	memset(so, 0, sizeof(*so)); memset(ss, 0, sizeof(*ss));
	so->src_precision = so->dst_precision = PRECISION_INT8;
	so->lut_index = c->act == ACTIVATION_LUT ? c->lut : -1;
	so->out_cvt = c->out_cvt;
	so->conv_mode = CONV_MODE_DIRECT;
	so->batch_num = 1;
	so->x1_op.enable = c->x1_type != SDP_OP_NONE || c->act != ACTIVATION_NONE;
	so->x1_op.alu_type = SDP_ALU_OP_SUM;
	so->x1_op.type = c->x1_type;
	so->x1_op.mode = SDP_OP_PER_KERNEL;
	so->x1_op.act = c->act == ACTIVATION_RELU ? ACTIVATION_RELU : ACTIVATION_NONE;
	so->x1_op.shift_value = c->x1_shift;
	so->x1_op.truncate = c->x1_truncate;
	so->x1_op.precision = c->x1_precision;
	so->x2_op.alu_type = so->y_op.alu_type = SDP_ALU_OP_SUM;
	if (c->act == ACTIVATION_LUT) {		/* LUT lives on the Y (EW) path */
		so->y_op.enable = 1;
		so->y_op.type = SDP_OP_NONE;
		so->y_op.act = ACTIVATION_LUT;
		so->y_op.mode = SDP_OP_PER_LAYER;
		so->y_op.precision = PRECISION_INT16;
	}
	ss->src_data = (struct dla_data_cube){ .type = DLA_MEM_HW, .address = -1, .width = wo, .height = ho,
		.channel = c->k };
	if (c->x1_type != SDP_OP_NONE) {
		uint32_t esz = c->x1_precision == PRECISION_INT8 ? 1 : 2;
		uint32_t n_el = c->x1_type == SDP_OP_BOTH ? 2 : 1;
		ss->x1_data = (struct dla_data_cube){ .type = DLA_MEM_MC, .address = c->x1_buf, .offset = c->x1_off,
			.size = c->k * esz * n_el, .width = 1, .height = 1, .channel = c->k,
			.line_stride = 8, .surf_stride = 8 };
	} else {
		ss->x1_data = (struct dla_data_cube){ .type = DLA_MEM_HW, .address = -1 };
	}
	ss->x2_data = ss->y_data = (struct dla_data_cube){ .type = DLA_MEM_HW, .address = -1 };
	cube_fill(&ss->dst_data, &c->out);
	/* --- dependencies (NVIDIA's fused CONV+SDP pattern) */
	dep_init(&n->dep[ic], ic, DLA_OP_CONV);
	dep_init(&n->dep[is], is, DLA_OP_SDP);
	n->dep[ic].dependency_count = 1;
	n->dep[ic].consumers[DLA_OP_SDP] = (struct dla_consumer){ .index = is, .event = DLA_EVENT_OP_PROGRAMMED };
	n->dep[is].dependency_count = 1;
	n->dep[is].fused_parent = (struct dla_consumer){ .index = ic, .event = DLA_EVENT_OP_ENABLED };
	/* NVIDIA's chaining (NN_L0_1_small): next op of each engine via the same-type
	 * consumer slot (PROGRAMMED); data edge previous SDP -> this CONV (COMPLETED).
	 * Every layer waits for the previous one: strictly serial, safe for any data
	 * dependency. */
	if (n->last_conv >= 0) {
		n->dep[n->last_conv].consumers[DLA_OP_CONV] = (struct dla_consumer){ .index = ic, .event = DLA_EVENT_OP_PROGRAMMED };
		n->dep[ic].dependency_count++;
	}
	if (n->last_sdp >= 0) {
		n->dep[n->last_sdp].consumers[DLA_OP_SDP] = (struct dla_consumer){ .index = is, .event = DLA_EVENT_OP_PROGRAMMED };
		n->dep[is].dependency_count++;
		if (depends_on_prev) {
			n->dep[n->last_sdp].consumers[DLA_OP_CONV] = (struct dla_consumer){ .index = ic, .event = DLA_EVENT_OP_COMPLETED };
			n->dep[ic].dependency_count++;
		}
	}
	if (n->op_head[DLA_OP_CONV] < 0) n->op_head[DLA_OP_CONV] = ic;
	if (n->op_head[DLA_OP_SDP] < 0) n->op_head[DLA_OP_SDP] = is;
	n->last_sdp = is;
	n->last_conv = ic;
	n->nops += 2;
	return ic;
}

int nn_add(struct nn_net *n, const struct nn_add *a)
{
	int is = n->nops;
	if (is >= n->max_ops) return -1;
	struct dla_sdp_op_desc *so = &n->ops[is].sdp_op;
	struct dla_sdp_surface_desc *ss = &n->surf[is].sdp_surface;
	memset(so, 0, sizeof(*so)); memset(ss, 0, sizeof(*ss));
	so->src_precision = so->dst_precision = PRECISION_INT8;
	so->lut_index = -1;
	so->out_cvt = a->out_cvt;
	so->conv_mode = CONV_MODE_DIRECT;
	so->batch_num = 1;
	so->x1_op = (struct dla_sdp_op){ .enable = 1, .alu_type = SDP_ALU_OP_SUM, .type = SDP_OP_ADD,
		.mode = SDP_OP_PER_POINT, .act = ACTIVATION_NONE, .precision = PRECISION_INT8 };
	so->x2_op.alu_type = so->y_op.alu_type = SDP_ALU_OP_SUM;
	cube_fill(&ss->src_data, &a->a);
	cube_fill(&ss->x1_data, &a->b);
	ss->x2_data = ss->y_data = (struct dla_data_cube){ .type = DLA_MEM_HW, .address = -1 };
	cube_fill(&ss->dst_data, &a->out);
	dep_init(&n->dep[is], is, DLA_OP_SDP);
	if (n->last_sdp >= 0) {		/* runs after the previous SDP has finished */
		n->dep[n->last_sdp].consumers[DLA_OP_SDP] = (struct dla_consumer){ .index = is, .event = DLA_EVENT_OP_COMPLETED };
		n->dep[is].dependency_count++;
	}
	if (n->op_head[DLA_OP_SDP] < 0) n->op_head[DLA_OP_SDP] = is;
	n->last_sdp = is;
	n->nops += 1;
	return is;
}

int nn_finalize(struct nn_net *n)
{
	size_t s_net = sizeof(struct dla_network_desc), s_ops = sizeof(n->ops[0]) * n->nops,
	       s_surf = sizeof(n->surf[0]) * n->nops, s_dep = sizeof(n->dep[0]) * n->nops,
	       s_lut = sizeof(n->luts[0]) * (n->nluts ? n->nluts : 1);
	size_t o[5], tot = 0;
	size_t sz[5] = { s_net, s_ops, s_surf, s_dep, s_lut };
	for (int i = 0; i < 5; i++) { o[i] = tot; tot += (sz[i] + 255) & ~255; }
	if (!n->tables.va && nrt_alloc(&n->tables, (uint32_t)((tot + 4095) & ~4095))) return -1;
	struct dla_network_desc *nw = n->tables.va;
	memset(nw, 0, sizeof(*nw));
	nw->operation_desc_index = 1; nw->surface_desc_index = 2; nw->dependency_graph_index = 3;
	nw->lut_data_index = n->nluts ? 4 : -1; nw->roi_array_index = -1; nw->surface_index = -1;
	nw->stat_list_index = -1;
	memcpy(nw->op_head, n->op_head, sizeof(nw->op_head));
	nw->num_rois = 1; nw->num_operations = n->nops; nw->num_luts = n->nluts;
	nw->num_addresses = n->nbufs; nw->input_layer = 0; nw->dynamic_roi = 0;
	memcpy((uint8_t *)n->tables.va + o[1], n->ops, s_ops);
	memcpy((uint8_t *)n->tables.va + o[2], n->surf, s_surf);
	memcpy((uint8_t *)n->tables.va + o[3], n->dep, s_dep);
	if (n->nluts) memcpy((uint8_t *)n->tables.va + o[4], n->luts, s_lut);
	for (int i = 0; i < 5; i++)
		n->bufs[i] = (struct nrt_buf){ .va = (uint8_t *)n->tables.va + o[i], .dma = n->tables.dma + o[i],
					       .size = (uint32_t)sz[i] };
	return 0;
}

int nn_run(struct nn_net *n, uint32_t *us)
{
	/* the firmware decrements dependency counts in the task's own dependency
	 * graph buffer: restore it before every run */
	memcpy(n->bufs[3].va, n->dep, sizeof(n->dep[0]) * n->nops);
	struct nrt_task t = { .bufs = n->bufs, .nbufs = n->nbufs };
	return nrt_run(&t, 2000, us);
}
