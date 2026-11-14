// sdctl /dev/v4l-subdevN [exposure|gain|vblank [value]] - get/set sensor controls
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
int main(int argc, char **argv)
{
	static const struct { const char *n; unsigned id; } c[] = {
		{ "exposure", V4L2_CID_EXPOSURE }, { "gain", V4L2_CID_ANALOGUE_GAIN },
		{ "vblank", V4L2_CID_VBLANK }, { "hflip", V4L2_CID_HFLIP }, { "vflip", V4L2_CID_VFLIP } };
	if (argc < 2) { fprintf(stderr, "usage: sdctl dev [ctrl [value]]\n"); return 1; }
	int fd = open(argv[1], O_RDWR);
	if (fd < 0) { perror(argv[1]); return 1; }
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		if (argc > 2 && strcmp(argv[2], c[i].n)) continue;
		struct v4l2_control ctl = { .id = c[i].id };
		if (argc > 3) {
			ctl.value = atoi(argv[3]);
			if (ioctl(fd, VIDIOC_S_CTRL, &ctl)) perror("S_CTRL");
		}
		if (!ioctl(fd, VIDIOC_G_CTRL, &ctl)) printf("%s=%d\n", c[i].n, ctl.value);
	}
	return 0;
}
