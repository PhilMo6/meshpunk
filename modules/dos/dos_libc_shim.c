// Small libc pieces the vendored core uses that the firmware's ELF host does
// not export (see host_exports[] in src/elf_host.cpp).
//
// Implementing them here keeps the DOS module a pure store app: no new host
// exports, so no firmware release and no _FW_API bump. Everything below is
// built from symbols that ARE exported (floor/ceil/fmod/gmtime/fseek/printf).

#include <stdio.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

extern void     host_sleep_ms(uint32_t ms);
extern uint32_t host_get_ticks_us(void);

// --- math ------------------------------------------------------------------

// fpu.c uses nearbyint() for x87 rounding-control mode 0, which is round to
// NEAREST-EVEN (their comment: "in C23 we can use roundeven() instead").
// round() would be wrong here — it rounds halves away from zero.
double nearbyint(double x)
{
    double f = floor(x);
    double diff = x - f;
    if (diff > 0.5) return f + 1.0;
    if (diff < 0.5) return f;
    return (fmod(f, 2.0) == 0.0) ? f : f + 1.0;   // exact .5 -> even neighbour
}

// x87 rounding-control mode 3: toward zero.
double trunc(double x)
{
    return (x < 0.0) ? ceil(x) : floor(x);
}

// --- time ------------------------------------------------------------------

// misc.c's CMOS clock. The host exports gmtime() but not the reentrant form;
// the module is single-threaded through this path, so copying out is enough.
struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
    struct tm *g = gmtime(timep);
    if (!g || !result) return 0;
    *result = *g;
    return result;
}

// --- stdio -----------------------------------------------------------------

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
}

void perror(const char *s)
{
    printf("[dos] %s: error\n", s ? s : "");
}

// i386.c and misc.c read CLOCK_MONOTONIC directly. Back it with the same
// microsecond clock get_uticks() uses, so every time source in the module
// agrees. host_get_ticks_us() is 32-bit and wraps about every 71 minutes;
// both callers only ever take differences, so the wrap is harmless.
int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    (void)clk_id;
    if (!tp) return -1;
    uint32_t us = host_get_ticks_us();
    tp->tv_sec  = (time_t)(us / 1000000u);
    tp->tv_nsec = (long)((us % 1000000u) * 1000u);
    return 0;
}

// --- posix -----------------------------------------------------------------

// Callers are yields: pc.c uses usleep(0) and i386.c usleep(1) to give other
// tasks a turn. Sub-millisecond requests become a plain yield rather than a
// full tick of sleep.
int usleep(useconds_t usec)
{
    host_sleep_ms(usec < 1000 ? 0 : usec / 1000);
    return 0;
}

// Reached only by the guest serial port (disabled: conf.enable_serial = 0) and
// the tuntap network backend (not built). Route the console fds to the log so
// anything unexpected is visible rather than silently dropped.
ssize_t write(int fd, const void *buf, size_t count)
{
    if (fd == 1 || fd == 2) {
        const char *p = (const char *)buf;
        for (size_t i = 0; i < count; i++) putchar(p[i]);
    }
    return (ssize_t)count;
}
