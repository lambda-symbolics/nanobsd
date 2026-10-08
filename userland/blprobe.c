/* blprobe SECONDS [INTERVAL_US]: watch the panel backlight PWM (PCH, controller 0)
 * and the pipe A link M (refresh) through the i915 MMIO BAR, printing a line
 * with a timestamp whenever any of them changes. */
#include <sys/mman.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#define BAR 0x603c000000ULL
static volatile uint8_t *map(int fd, uint64_t off) {
    void *m = mmap(0, 0x1000, PROT_READ, MAP_SHARED, fd, BAR + (off & ~0xfffULL));
    if (m == MAP_FAILED) { perror("mmap"); exit(3); }
    return (volatile uint8_t *)m + (off & 0xfff);
}
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    int us = argc > 2 ? atoi(argv[2]) : 4000;
    int fd = open("/dev/mem", O_RDONLY);
    if (fd < 0) { perror("/dev/mem"); return 2; }
    volatile uint32_t *ctl  = (volatile uint32_t *)map(fd, 0xC8250);
    volatile uint32_t *freq = (volatile uint32_t *)map(fd, 0xC8254);
    volatile uint32_t *duty = (volatile uint32_t *)map(fd, 0xC8258);
    volatile uint32_t *linkm = (volatile uint32_t *)map(fd, 0x60040);
    volatile uint32_t *pp   = (volatile uint32_t *)map(fd, 0xC7200); /* PP_STATUS */
    uint32_t pc = ~0u, pf = ~0u, pd = ~0u, pm = ~0u, ps = ~0u;
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (;;) {
        uint32_t c = *ctl, f = *freq, d = *duty, m = *linkm, s = *pp & 0x80000000u;
        clock_gettime(CLOCK_MONOTONIC, &t);
        double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
        if (c != pc || f != pf || d != pd || m != pm || s != ps) {
            time_t now = time(NULL); struct tm *tm = localtime(&now);
            printf("%02d:%02d:%02d %9.3f ctl=%08x freq=%u duty=%u (%.1f%%) linkM=%05x panel=%s\n",
                   tm->tm_hour, tm->tm_min, tm->tm_sec, el, c, f, d,
                   f ? 100.0 * d / f : 0.0, m, s ? "on" : "off");
            pc = c; pf = f; pd = d; pm = m; ps = s;
        }
        if (el >= secs) break;
        usleep(us);
    }
    return 0;
}
