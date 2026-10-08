/*
 * posix_fault.h - MMIO trapping and crash reporting on POSIX hosts.
 *
 * On Windows the runtime protects a device's register aperture with
 * PAGE_NOACCESS and a vectored exception handler decodes the faulting x86-64
 * instruction (apu_mmio_hook.c, aci_mmio.c, nv2a_mmio_hook.c). This is the
 * POSIX counterpart: the same apertures are mprotect()ed to PROT_NONE and one
 * SIGSEGV handler decodes the faulting load or store -- x86-64 (Linux, the
 * Android emulator) or AArch64 (Android phones) -- and hands the access to
 * the device's own register read/write functions.
 *
 * Linux / Android only.
 */
#ifndef POSIX_FAULT_H
#define POSIX_FAULT_H

#if !defined(_WIN32)

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*pf_mmio_read_fn)(void *ud, uint32_t off, unsigned size);
typedef void     (*pf_mmio_write_fn)(void *ud, uint32_t off, uint64_t val, unsigned size);

/* Trap [host, host + len): every guest access becomes a read_fn / write_fn
 * call with the offset from `host`. `len` is rounded out to whole pages.
 * Returns 1 on success. Up to 16 apertures. */
int pf_mmio_register(void *host, size_t len, const char *name,
                     pf_mmio_read_fn read_fn, pf_mmio_write_fn write_fn, void *ud);

/* Called on a fault that is not a trapped device access, before the process
 * dies (the default action runs afterwards, so Android still writes its
 * tombstone). `what` names the signal; `addr` is the fault address. */
typedef void (*pf_crash_fn)(int sig, const char *what, void *addr, uintptr_t pc,
                            void **frames, int nframes);

/* Install the SIGSEGV/SIGBUS/SIGILL/SIGFPE handlers (idempotent). */
void pf_install(pf_crash_fn crash);

/* Number of device accesses emulated and decode failures (diagnostics). */
void pf_stats(unsigned long long *emulated, unsigned long long *failed);

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif /* POSIX_FAULT_H */
