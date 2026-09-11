/*
 * espnet_ct.c - userspace smoke test against /dev/espnet.
 *
 * Build:  make -C kmod/test
 * Run:    sudo ./espnet_ct            (needs the module loaded: insmod espnet_core.ko)
 *
 * Exercises the ioctl ABI (GET_STATE / SET_AP) and the byte FIFO, which is
 * the minimal kernel-<->user contract the whole project relies on.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "espnet.h"

static int failures = 0;

#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            fprintf(stderr, "FAIL: %s\n", msg);                   \
            failures++;                                           \
        } else {                                                  \
            printf("PASS: %s\n", msg);                            \
        }                                                         \
    } while (0)

int main(void)
{
    const char *dev = "/dev/espnet";
    struct espnet_state st;
    unsigned int on = 1;
    int fd;

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", dev, strerror(errno));
        fprintf(stderr, "(load the module first: insmod espnet_core.ko)\n");
        return 1;
    }

    memset(&st, 0, sizeof(st));
    if (ioctl(fd, ESPNET_IOCTL_GET_STATE, &st) < 0) {
        fprintf(stderr, "GET_STATE failed: %s\n", strerror(errno));
        return 1;
    }
    CHECK(strlen(st.ext_ssid) > 0, "state carries an extender SSID");

    if (ioctl(fd, ESPNET_IOCTL_SET_AP, &on) < 0) {
        fprintf(stderr, "SET_AP failed: %s\n", strerror(errno));
        return 1;
    }
    memset(&st, 0, sizeof(st));
    ioctl(fd, ESPNET_IOCTL_GET_STATE, &st);
    CHECK(st.ap_active == 1, "AP state persists through ioctls");

    /* byte FIFO round-trip */
    {
        const char payload[] = "{\"evt\":\"AP_UP\",\"rssi\":-70}\n";
        char    back[128] = {0};
        ssize_t wn = write(fd, payload, sizeof(payload) - 1);
        ssize_t rn = read(fd, back, sizeof(back) - 1);

        CHECK(wn == (ssize_t)(sizeof(payload) - 1), "FIFO accepts a write");
        CHECK(rn == (ssize_t)(sizeof(payload) - 1), "FIFO returns the bytes");
        CHECK(memcmp(payload, back, (size_t)rn) == 0, "FIFO is byte-exact");
    }

    close(fd);
    printf(failures ? "%d failure(s)\n" : "all espnet_ct checks passed\n",
           failures);
    return failures ? 1 : 0;
}