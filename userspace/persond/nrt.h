/* nvdla-rt: NVIDIA's NVDLA firmware (BSD-3, fw/) on the V831 NPU from userspace via
 * /dev/npu (the sun8i-npu kernel driver). A task is an address list of buffers:
 * index 0 = struct dla_network_desc, the other tables and data by index. */
#ifndef NRT_H
#define NRT_H
#include <stdint.h>

struct nrt_buf {
	void *va;		/* CPU mapping (uncached, coherent) */
	uint64_t dma;		/* NPU (bus) address */
	uint32_t size;
	uint32_t handle;
};

struct nrt_task {
	struct nrt_buf *bufs;	/* address list */
	int nbufs;
};

int nrt_open(void);
void nrt_close(void);
int nrt_alloc(struct nrt_buf *b, uint32_t size);
void nrt_free(struct nrt_buf *b);
/* run one task to completion; returns 0 or a negative error */
int nrt_run(struct nrt_task *t, unsigned timeout_ms, uint32_t *elapsed_us);
uint32_t nrt_reg(uint32_t addr);
extern int nrt_verbose;
#endif
