#include <windows.h>

/* __rdtsc: the x86 time-stamp counter (xbox_perf.c's cycle counts). On x86
 * the compiler has it; on ARM the virtual counter stands in -- another rate,
 * which xbox_perf.c measures against the clock as it does the TSC's. */
#if (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#elif defined(__aarch64__)
static inline unsigned long long __rdtsc(void)
{
    unsigned long long v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
#else
#include <time.h>
static inline unsigned long long __rdtsc(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}
#endif
