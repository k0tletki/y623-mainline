/* userspace stand-ins for the kernel definitions the NVDLA firmware expects */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif
