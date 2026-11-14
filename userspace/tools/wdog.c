// wdog [timeout_s] - daemonize and ping /dev/watchdog every second.
// A bus hang stops the pings -> SoC warm reset -> DRAM (ramoops) survives.
#include <fcntl.h>
#include <linux/watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	int t = argc > 1 ? atoi(argv[1]) : 5;
	int fd = open("/dev/watchdog", O_WRONLY);
	if (fd < 0) { perror("/dev/watchdog"); return 1; }
	if (ioctl(fd, WDIOC_SETTIMEOUT, &t)) perror("SETTIMEOUT");
	ioctl(fd, WDIOC_GETTIMEOUT, &t);
	printf("wdog: timeout %ds\n", t);
	fflush(stdout);
	if (fork()) return 0;
	setsid();
	close(0); close(1); close(2);
	for (;;) { ioctl(fd, WDIOC_KEEPALIVE, 0); sleep(1); }
}
