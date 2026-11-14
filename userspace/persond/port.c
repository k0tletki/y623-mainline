// Port layer for the NVDLA firmware: replaces kmd/port/linux/nvdla_core_callbacks.c.
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <dla_interface.h>
#include <dla_sched.h>
#include <dla_engine.h>
#include <nvdla_interface.h>
#include "nrt.h"
#include <linux/sun8i-npu.h>

#define GLB_S_INTR_STATUS	0x100c

int nrt_verbose;
static int npu_fd = -1;
static volatile uint32_t *regs;
static void *engine_ctx;
static struct dla_config cfg_small = { .atom_size = 8, .bdma_enable = false,
				       .rubik_enable = false, .weight_compress_support = false };

/* ---- firmware callbacks */
uint32_t dla_reg_read(void *driver_context, uint32_t addr) { (void)driver_context; return regs[addr / 4]; }
void dla_reg_write(void *driver_context, uint32_t addr, uint32_t reg)
{
	(void)driver_context;
	if (nrt_verbose > 1)
		fprintf(stderr, "  W %05x = %08x\n", addr, reg);
	regs[addr / 4] = reg;
}
uint32_t nrt_reg(uint32_t addr) { return regs[addr / 4]; }

static void vlog(const char *tag, int lvl, const char *fmt, va_list ap)
{
	if (nrt_verbose >= lvl) { fputs(tag, stderr); vfprintf(stderr, fmt, ap); }
}
void dla_debug(const char *str, ...) { va_list ap; va_start(ap, str); vlog("", 3, str, ap); va_end(ap); }
void dla_info(const char *str, ...) { va_list ap; va_start(ap, str); vlog("", 2, str, ap); va_end(ap); }
void dla_warn(const char *str, ...) { va_list ap; va_start(ap, str); vlog("dla warn: ", 0, str, ap); va_end(ap); }
void dla_error(const char *str, ...) { va_list ap; va_start(ap, str); vlog("dla error: ", 0, str, ap); va_end(ap); }
void *dla_memset(void *src, int ch, uint64_t len) { return memset(src, ch, len); }
void *dla_memcpy(void *dest, const void *src, uint64_t len) { return memcpy(dest, src, len); }
int64_t dla_get_time_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int32_t dla_get_dma_address(void *driver_context, void *task_data, int16_t index,
			    void *dst_ptr, uint32_t destination)
{
	struct nrt_task *t = task_data;
	(void)driver_context;
	if (index < 0 || index >= t->nbufs)
		return -EINVAL;
	if (destination == DESTINATION_PROCESSOR)
		*(uint64_t *)dst_ptr = (uint64_t)index;		/* CPU side: the index itself */
	else
		*(uint64_t *)dst_ptr = t->bufs[index].dma;
	return 0;
}

int32_t dla_data_read(void *driver_context, void *task_data, uint64_t src, void *dst,
		      uint32_t size, uint64_t offset)
{
	struct nrt_task *t = task_data;
	(void)driver_context;
	if (src >= (uint64_t)t->nbufs || offset + size > t->bufs[src].size)
		return -EINVAL;
	memcpy(dst, (uint8_t *)t->bufs[src].va + offset, size);
	return 0;
}

int32_t dla_data_write(void *driver_context, void *task_data, void *src, uint64_t dst,
		       uint32_t size, uint64_t offset)
{
	struct nrt_task *t = task_data;
	(void)driver_context;
	if (dst >= (uint64_t)t->nbufs || offset + size > t->bufs[dst].size)
		return -EINVAL;
	memcpy((uint8_t *)t->bufs[dst].va + offset, src, size);
	return 0;
}

/* ---- runtime API */
int nrt_open(void)
{
	if (getenv("NRT_VERBOSE")) nrt_verbose = atoi(getenv("NRT_VERBOSE"));
	npu_fd = open("/dev/npu", O_RDWR);
	if (npu_fd < 0) { perror("/dev/npu"); return -1; }
	void *m = mmap(NULL, NPU_REG_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, npu_fd, 0);
	if (m == MAP_FAILED) { perror("mmap npu regs"); return -1; }
	regs = m;
	ioctl(npu_fd, NPU_IOC_RESET);
	return dla_register_driver(&engine_ctx, NULL);
}

void nrt_close(void)
{
	if (regs) munmap((void *)regs, NPU_REG_SIZE);
	if (npu_fd >= 0) close(npu_fd);
	regs = NULL; npu_fd = -1;
}

int nrt_alloc(struct nrt_buf *b, uint32_t size)
{
	struct npu_alloc a = { .size = size };
	if (ioctl(npu_fd, NPU_IOC_ALLOC, &a)) { perror("NPU_IOC_ALLOC"); return -1; }
	void *va = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, npu_fd, (off_t)a.mmap_offset);
	if (va == MAP_FAILED) { perror("mmap buffer"); ioctl(npu_fd, NPU_IOC_FREE, &a.handle); return -1; }
	memset(va, 0, size);
	*b = (struct nrt_buf){ .va = va, .dma = a.dma_addr, .size = size, .handle = a.handle };
	return 0;
}

void nrt_free(struct nrt_buf *b)
{
	if (!b->va) return;
	munmap(b->va, b->size);
	ioctl(npu_fd, NPU_IOC_FREE, &b->handle);
	memset(b, 0, sizeof(*b));
}

int nrt_run(struct nrt_task *t, unsigned timeout_ms, uint32_t *elapsed_us)
{
	uint32_t done = 0;
	/* unit state does not fully clear between tasks (the public code resets after
	 * every operation too): start each task from reset */
	if (getenv("NRT_NO_RESET") == NULL)
		ioctl(npu_fd, NPU_IOC_RESET);
	int64_t t0 = dla_get_time_us();
	if (nrt_verbose > 1) fprintf(stderr, "---- task start\n");
	int ret = dla_execute_task(engine_ctx, t, &cfg_small);
	if (ret) { fprintf(stderr, "dla_execute_task: %d\n", ret); goto out; }
	while (!done) {
		/* interrupt line not wired yet: poll the GLB status */
		while (!regs[GLB_S_INTR_STATUS / 4]) {
			if (dla_get_time_us() - t0 > (int64_t)timeout_ms * 1000) {
				fprintf(stderr, "nrt: timeout, GLB status 0, task incomplete\n");
				ret = -ETIMEDOUT;
				goto out;
			}
		}
		dla_isr_handler(engine_ctx);
		ret = dla_process_events(engine_ctx, &done);
		if (ret) { fprintf(stderr, "dla_process_events: %d\n", ret); break; }
	}
out:
	if (elapsed_us) *elapsed_us = (uint32_t)(dla_get_time_us() - t0);
	dla_clear_task(engine_ctx);
	/*
	 * dla_clear_task() keeps each processor's last_group, so the next task would
	 * be programmed into register group 1 while the (reset) hardware consumes
	 * group 0 first. Start every task from group 0, like the hardware.
	 */
	{
		struct dla_engine *e = dla_get_engine();
		for (int i = 0; i < DLA_OP_NUM; i++) {
			e->processors[i].last_group = 1;
			e->processors[i].consumer_ptr = 0;
		}
	}
	if (ret) ioctl(npu_fd, NPU_IOC_RESET);
	return ret;
}
