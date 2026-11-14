// gpioset LINE=VAL [LINE=VAL ...] [-t ms]  - drive gpiochip0 lines, hold for ms
// (default 0 = until killed), then release. Line numbers are global (PH8 = 232).
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	struct gpio_v2_line_request req = { 0 };
	struct gpio_v2_line_values vals = { 0 };
	int ms = 0, n = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-t") && i + 1 < argc) { ms = atoi(argv[++i]); continue; }
		unsigned l, v;
		if (sscanf(argv[i], "%u=%u", &l, &v) != 2 || n >= 16) { fprintf(stderr, "bad arg %s\n", argv[i]); return 1; }
		req.offsets[n] = l;
		if (v) vals.bits |= 1ULL << n;
		vals.mask |= 1ULL << n;
		n++;
	}
	if (!n) { fprintf(stderr, "usage: gpioset LINE=VAL... [-t ms]\n"); return 1; }
	int fd = open("/dev/gpiochip0", O_RDWR);
	if (fd < 0) { perror("gpiochip0"); return 1; }
	req.num_lines = n;
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	req.config.num_attrs = 1;
	req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	req.config.attrs[0].attr.values = vals.bits;
	req.config.attrs[0].mask = vals.mask;
	strcpy(req.consumer, "gpioset");
	if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) { perror("GET_LINE"); return 1; }
	if (ms) usleep(ms * 1000); else pause();
	return 0;
}
