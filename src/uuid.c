#include "uuid.h"
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

void uuid_v4(char *buf) {
    uint8_t rnd[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        /* Fallback: use time + pid (not cryptographically random) */
        struct { long t; int p; } seed = { time(NULL), getpid() };
        memcpy(rnd, &seed, sizeof(seed));
        for (int i = sizeof(seed); i < 16; i++)
            rnd[i] = (uint8_t)(rand() & 0xff);
    } else {
        ssize_t n = 0;
        while (n < 16) {
            ssize_t r = read(fd, rnd + n, 16 - n);
            if (r < 0 && errno != EINTR) break;
            if (r > 0) n += r;
        }
        close(fd);
    }

    /* Set version 4 and variant bits */
    rnd[6] = (rnd[6] & 0x0f) | 0x40;
    rnd[8] = (rnd[8] & 0x3f) | 0x80;

    snprintf(buf, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        rnd[0],  rnd[1],  rnd[2],  rnd[3],
        rnd[4],  rnd[5],
        rnd[6],  rnd[7],
        rnd[8],  rnd[9],
        rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);
}