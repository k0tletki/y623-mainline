// SPDX-License-Identifier: GPL-2.0
/* Blink the Yi 2K Pro blue status LED (GPIO line 237, PH13, active low). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

#define DEFAULT_LINE   237
#define DEFAULT_COUNT  6
#define DEFAULT_DELAY  200

static int find_chip(unsigned int line, struct gpiochip_info *info)
{
    char path[32];
    int i;

    for (i = 0; i < 8; i++) {
        int fd;

        snprintf(path, sizeof(path), "/dev/gpiochip%d", i);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        memset(info, 0, sizeof(*info));
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, info) == 0 &&
            info->lines > line) {
            printf("ledblink: using %s (%s, %u lines)\n",
                   path, info->label[0] ? info->label : info->name,
                   info->lines);
            return fd;
        }
        close(fd);
    }
    return -1;
}

static int set_value(int fd, unsigned int value)
{
    struct gpio_v2_line_values values;

    memset(&values, 0, sizeof(values));
    values.mask = 1;
    values.bits = value ? 1 : 0;
    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
}

int main(int argc, char **argv)
{
    unsigned int line = DEFAULT_LINE;
    int count = DEFAULT_COUNT, delay = DEFAULT_DELAY;
    struct gpiochip_info info;
    struct gpio_v2_line_request req;
    int chip_fd, line_fd, i;

    if (argc > 1)
        line = strtoul(argv[1], NULL, 0);
    if (argc > 2)
        count = atoi(argv[2]);
    if (argc > 3)
        delay = atoi(argv[3]);

    chip_fd = find_chip(line, &info);
    if (chip_fd < 0) {
        fprintf(stderr, "ledblink: no gpiochip with line %u\n", line);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.offsets[0] = line;
    req.num_lines = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    strncpy(req.consumer, "ledblink", sizeof(req.consumer) - 1);

    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        fprintf(stderr, "ledblink: line %u request failed: %s\n",
                line, strerror(errno));
        close(chip_fd);
        return 1;
    }
    line_fd = req.fd;
    close(chip_fd);

    for (i = 0; i < count; i++) {
        set_value(line_fd, 1);
        usleep(delay * 1000);
        set_value(line_fd, 0);
        usleep(delay * 1000);
    }
    set_value(line_fd, 0); /* active low: leave the blue LED on */
    close(line_fd);
    printf("ledblink: OK (line %u, %d blinks)\n", line, count);
    return 0;
}
