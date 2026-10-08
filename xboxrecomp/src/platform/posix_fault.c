/*
 * posix_fault.c - MMIO trapping and crash reporting on POSIX hosts.
 * See posix_fault.h.
 *
 * The faulting instruction is always one the C compiler emitted for a
 * MEM8/16/32/64() access in the translated code, so only plain loads, stores
 * and the read-modify-write / compare forms compilers fold such accesses into
 * have to be understood. The address comes from the fault itself (si_addr),
 * the decoder only needs the data register, the access size and the
 * instruction length.
 */
#if !defined(_WIN32)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "posix_fault.h"
#include "win32_compat.h"   /* CaptureStackBackTrace */

#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/mman.h>

/* ---- apertures ---------------------------------------------------------- */

typedef struct {
    uintptr_t        lo, hi;
    const char      *name;
    pf_mmio_read_fn  rd;
    pf_mmio_write_fn wr;
    void            *ud;
} PfRange;

#define PF_MAX_RANGES 16
static PfRange           s_ranges[PF_MAX_RANGES];
static volatile int      s_nranges;
static pf_crash_fn       s_crash;
static unsigned long long s_emulated, s_failed;

int pf_mmio_register(void *host, size_t len, const char *name,
                     pf_mmio_read_fn read_fn, pf_mmio_write_fn write_fn, void *ud)
{
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t lo = (uintptr_t)host & ~(uintptr_t)(pg - 1);
    uintptr_t hi = ((uintptr_t)host + len + (uintptr_t)pg - 1) & ~(uintptr_t)(pg - 1);
    int i = s_nranges;
    if (i >= PF_MAX_RANGES) return 0;
    if (mprotect((void *)lo, hi - lo, PROT_NONE) != 0) {
        fprintf(stderr, "  [FAULT] could not trap %s at %p (+%zu)\n", name, host, len);
        return 0;
    }
    /* The device sees offsets from `host`, which need not be page aligned. */
    s_ranges[i].lo = (uintptr_t)host;
    s_ranges[i].hi = (uintptr_t)host + len;
    s_ranges[i].name = name;
    s_ranges[i].rd = read_fn;
    s_ranges[i].wr = write_fn;
    s_ranges[i].ud = ud;
    __atomic_store_n(&s_nranges, i + 1, __ATOMIC_RELEASE);
    return 1;
}

void pf_stats(unsigned long long *emulated, unsigned long long *failed)
{
    if (emulated) *emulated = s_emulated;
    if (failed)   *failed = s_failed;
}

static const PfRange *find_range(uintptr_t a)
{
    int n = __atomic_load_n(&s_nranges, __ATOMIC_ACQUIRE), i;
    for (i = 0; i < n; i++)
        if (a >= s_ranges[i].lo && a < s_ranges[i].hi) return &s_ranges[i];
    return NULL;
}

static uint64_t dev_read(const PfRange *r, uintptr_t a, unsigned size)
{
    return r->rd ? r->rd(r->ud, (uint32_t)(a - r->lo), size) : 0;
}
static void dev_write(const PfRange *r, uintptr_t a, uint64_t v, unsigned size)
{
    if (r->wr) r->wr(r->ud, (uint32_t)(a - r->lo), v, size);
}

static uint64_t sext(uint64_t v, unsigned from_bytes);

static uint64_t size_mask(unsigned size)
{
    return size >= 8 ? ~0ull : ((1ull << (size * 8)) - 1);
}

static uint64_t sext(uint64_t v, unsigned from_bytes)
{
    unsigned s = 64 - from_bytes * 8;
    return (uint64_t)(((int64_t)(v << s)) >> s);
}

/* ======================================================================== */
/* x86-64                                                                    */
/* ======================================================================== */
#if defined(__x86_64__)

#define FL_CF 0x0001u
#define FL_PF 0x0004u
#define FL_ZF 0x0040u
#define FL_SF 0x0080u
#define FL_OF 0x0800u

static greg_t *x_reg(ucontext_t *uc, int r)
{
    static const int map[16] = {
        REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
        REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15
    };
    return &uc->uc_mcontext.gregs[map[r & 15]];
}

/* Read a register operand of `size` bytes (8-bit: AH..BH without REX). */
static uint64_t x_get(ucontext_t *uc, int r, unsigned size, int has_rex)
{
    if (size == 1 && !has_rex && r >= 4 && r < 8)
        return ((uint64_t)*x_reg(uc, r - 4) >> 8) & 0xFF;
    return (uint64_t)*x_reg(uc, r) & size_mask(size);
}

/* Write a register with x86 semantics: 32-bit writes zero the upper half,
 * 8/16-bit writes keep the rest. */
static void x_set(ucontext_t *uc, int r, unsigned size, int has_rex, uint64_t v)
{
    greg_t *g;
    if (size == 1 && !has_rex && r >= 4 && r < 8) {
        g = x_reg(uc, r - 4);
        *g = (greg_t)(((uint64_t)*g & ~0xFF00ull) | ((v & 0xFF) << 8));
        return;
    }
    g = x_reg(uc, r);
    if (size == 4)      *g = (greg_t)(v & 0xFFFFFFFFull);
    else if (size == 8) *g = (greg_t)v;
    else                *g = (greg_t)(((uint64_t)*g & ~size_mask(size)) | (v & size_mask(size)));
}

/* Length of ModRM + SIB + displacement; -1 for a register operand. */
static int x_modrm_len(const uint8_t *m)
{
    int mod = m[0] >> 6, rm = m[0] & 7, len = 1;
    if (mod == 3) return -1;
    if (rm == 4) {
        len++;
        if (mod == 0 && (m[1] & 7) == 5) len += 4;
    } else if (mod == 0 && rm == 5) {
        len += 4;                                  /* RIP-relative */
    }
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

static int parity8(uint64_t v)
{
    return !__builtin_parity((unsigned)(v & 0xFF));
}

static void x_flags(ucontext_t *uc, uint64_t res, unsigned size, int cf, int of, uint32_t keep)
{
    greg_t *fl = &uc->uc_mcontext.gregs[REG_EFL];
    uint64_t f = (uint64_t)*fl & ~(uint64_t)((FL_CF | FL_PF | FL_ZF | FL_SF | FL_OF) & ~keep);
    unsigned bits = size * 8;
    res &= size_mask(size);
    if (!(keep & FL_CF) && cf) f |= FL_CF;
    if (of) f |= FL_OF;
    if (res == 0) f |= FL_ZF;
    if ((res >> (bits - 1)) & 1) f |= FL_SF;
    if (parity8(res)) f |= FL_PF;
    *fl = (greg_t)f;
}

enum { OP_ADD, OP_OR, OP_ADC, OP_SBB, OP_AND, OP_SUB, OP_XOR, OP_CMP };

/* Apply ALU op `op` (x86 /digit numbering) and set the flags. */
static uint64_t x_alu(ucontext_t *uc, int op, uint64_t a, uint64_t b, unsigned size)
{
    uint64_t m = size_mask(size), r = 0, sign = 1ull << (size * 8 - 1);
    a &= m; b &= m;
    switch (op) {
    case OP_ADD: r = (a + b) & m;
        x_flags(uc, r, size, r < a, ((a ^ r) & (b ^ r) & sign) != 0, 0); break;
    case OP_SUB: case OP_CMP: r = (a - b) & m;
        x_flags(uc, r, size, a < b, ((a ^ b) & (a ^ r) & sign) != 0, 0); break;
    case OP_OR:  r = a | b; x_flags(uc, r, size, 0, 0, 0); break;
    case OP_AND: r = a & b; x_flags(uc, r, size, 0, 0, 0); break;
    case OP_XOR: r = a ^ b; x_flags(uc, r, size, 0, 0, 0); break;
    default: return a;   /* ADC/SBB: not emitted for MMIO */
    }
    return r;
}


static int emulate_x86(ucontext_t *uc, const PfRange *rg, uintptr_t addr)
{
    const uint8_t *ip = (const uint8_t *)uc->uc_mcontext.gregs[REG_RIP];
    int p = 0, has66 = 0, rex = 0, repf3 = 0, repf2 = 0;
    for (;;) {
        uint8_t b = ip[p];
        if (b == 0x66) has66 = 1;
        else if (b == 0xF3) repf3 = 1;
        else if (b == 0xF2) repf2 = 1;
        else if (b == 0xF0 || b == 0x2E || b == 0x3E || b == 0x26 ||
                 b == 0x36 || b == 0x64 || b == 0x65) { /* lock / segment */ }
        else break;
        p++;
    }
    if ((ip[p] & 0xF0) == 0x40) rex = ip[p++];
    int has_rex = rex != 0;
    int W = (rex >> 3) & 1, R = (rex >> 2) & 1;
    unsigned osz = W ? 8 : has66 ? 2 : 4;
    const uint8_t *op = ip + p;
    int n, reg, len;
    uint64_t v, m;

#define ADV(k) (uc->uc_mcontext.gregs[REG_RIP] += (greg_t)(p + (k)))
#define MODRM(o) do { n = x_modrm_len(op + (o)); if (n < 0) return 0; \
                      reg = ((op[o] >> 3) & 7) | (R << 3); } while (0)
#define IMMV(at, sz) ((sz) == 1 ? (uint64_t)(int64_t)*(const int8_t *)(at) : \
                      (sz) == 2 ? (uint64_t)(int64_t)*(const int16_t *)(at) : \
                                  (uint64_t)(int64_t)*(const int32_t *)(at))

    switch (op[0]) {
    case 0x88: case 0x89: {                                   /* mov m, r */
        unsigned s = op[0] == 0x88 ? 1 : osz;
        MODRM(1);
        dev_write(rg, addr, x_get(uc, reg, s, has_rex), s);
        ADV(1 + n); return 1;
    }
    case 0x8A: case 0x8B: {                                   /* mov r, m */
        unsigned s = op[0] == 0x8A ? 1 : osz;
        MODRM(1);
        x_set(uc, reg, s, has_rex, dev_read(rg, addr, s));
        ADV(1 + n); return 1;
    }
    case 0xC6: case 0xC7: {                                   /* mov m, imm */
        unsigned s = op[0] == 0xC6 ? 1 : osz, isz = s == 8 ? 4 : s;
        MODRM(1);
        dev_write(rg, addr, IMMV(op + 1 + n, isz) & size_mask(s), s);
        ADV(1 + n + (int)isz); return 1;
    }
    case 0x63: {                                              /* movsxd r, m32 */
        MODRM(1);
        v = dev_read(rg, addr, 4);
        x_set(uc, reg, osz, has_rex, W ? sext(v, 4) : v);
        ADV(1 + n); return 1;
    }
    case 0x84: case 0x85: {                                   /* test m, r */
        unsigned s = op[0] == 0x84 ? 1 : osz;
        MODRM(1);
        x_alu(uc, OP_AND, dev_read(rg, addr, s), x_get(uc, reg, s, has_rex), s);
        ADV(1 + n); return 1;
    }
    case 0x80: case 0x81: case 0x83: {                        /* alu m, imm */
        unsigned s = op[0] == 0x80 ? 1 : osz;
        unsigned isz = (op[0] == 0x81) ? (s == 8 ? 4 : s) : 1;
        int alu = (op[1] >> 3) & 7;
        n = x_modrm_len(op + 1); if (n < 0) return 0;
        uint64_t imm = IMMV(op + 1 + n, isz) & size_mask(s);
        v = dev_read(rg, addr, s);
        uint64_t r = x_alu(uc, alu, v, imm, s);
        if (alu != OP_CMP) dev_write(rg, addr, r, s);
        ADV(1 + n + (int)isz); return 1;
    }
    case 0xF6: case 0xF7: {                                   /* test m, imm */
        unsigned s = op[0] == 0xF6 ? 1 : osz, isz = s == 8 ? 4 : s;
        if (((op[1] >> 3) & 7) > 1) return 0;
        n = x_modrm_len(op + 1); if (n < 0) return 0;
        x_alu(uc, OP_AND, dev_read(rg, addr, s), IMMV(op + 1 + n, isz), s);
        ADV(1 + n + (int)isz); return 1;
    }
    case 0xFE: case 0xFF: {                                   /* inc / dec m */
        unsigned s = op[0] == 0xFE ? 1 : osz;
        int d = (op[1] >> 3) & 7;
        if (d > 1) return 0;
        n = x_modrm_len(op + 1); if (n < 0) return 0;
        v = dev_read(rg, addr, s);
        m = size_mask(s);
        uint64_t r = (d == 0 ? v + 1 : v - 1) & m, sign = 1ull << (s * 8 - 1);
        int of = d == 0 ? (r == sign) : (v == sign);
        x_flags(uc, r, s, 0, of, FL_CF);
        dev_write(rg, addr, r, s);
        ADV(1 + n); return 1;
    }
    case 0x0F:
        switch (op[1]) {
        case 0xB6: case 0xB7: case 0xBE: case 0xBF: {         /* movzx / movsx */
            unsigned s = (op[1] & 1) ? 2 : 1;
            MODRM(2);
            v = dev_read(rg, addr, s);
            if (op[1] >= 0xBE) v = sext(v, s);
            x_set(uc, reg, osz, has_rex, v & size_mask(osz));
            ADV(2 + n); return 1;
        }
        case 0x6E: case 0x7E: case 0x10: case 0x11: case 0xD6: {   /* SSE scalar moves */
            fpregset_t fp = uc->uc_mcontext.fpregs;
            unsigned s;
            int load;
            if (!fp) return 0;
            if (op[1] == 0x6E && has66)       { s = W ? 8 : 4; load = 1; }   /* movd/movq xmm, m */
            else if (op[1] == 0x7E && has66)  { s = W ? 8 : 4; load = 0; }   /* movd/movq m, xmm */
            else if (op[1] == 0x7E && repf3)  { s = 8; load = 1; }           /* movq xmm, m64 */
            else if (op[1] == 0xD6 && has66)  { s = 8; load = 0; }           /* movq m64, xmm */
            else if (op[1] == 0x10 && (repf3 || repf2)) { s = repf3 ? 4 : 8; load = 1; } /* movss/sd */
            else if (op[1] == 0x11 && (repf3 || repf2)) { s = repf3 ? 4 : 8; load = 0; }
            else return 0;
            MODRM(2);
            uint32_t *x = fp->_xmm[reg & 15].element;
            if (load) {
                uint64_t d = dev_read(rg, addr, s);
                x[0] = (uint32_t)d; x[1] = s == 8 ? (uint32_t)(d >> 32) : 0;
                x[2] = 0; x[3] = 0;   /* movss/movd/movq from memory zero the rest */
            } else {
                uint64_t d = x[0] | ((uint64_t)x[1] << 32);
                dev_write(rg, addr, d & size_mask(s), s);
            }
            ADV(2 + n); return 1;
        }
        default: return 0;
        }
    default:
        break;
    }

    /* alu r/m forms: 00-3B (op & 7 in 0..3) */
    if (op[0] < 0x40 && (op[0] & 7) <= 3) {
        int alu = op[0] >> 3, dir = (op[0] >> 1) & 1;      /* dir 1: reg = reg op m */
        unsigned s = (op[0] & 1) ? osz : 1;
        MODRM(1);
        uint64_t mv = dev_read(rg, addr, s), rv = x_get(uc, reg, s, has_rex);
        if (dir) {
            uint64_t r = x_alu(uc, alu, rv, mv, s);
            if (alu != OP_CMP) x_set(uc, reg, s, has_rex, r);
        } else {
            uint64_t r = x_alu(uc, alu, mv, rv, s);
            if (alu != OP_CMP) dev_write(rg, addr, r, s);
        }
        ADV(1 + n); return 1;
    }
    (void)len;
    return 0;
#undef ADV
#undef MODRM
#undef IMMV
}

static uintptr_t uc_pc(ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]; }
#define EMULATE emulate_x86

/* ======================================================================== */
/* AArch64                                                                   */
/* ======================================================================== */
#elif defined(__aarch64__)

struct pf_ctx_head { uint32_t magic, size; };
struct pf_fpsimd   { struct pf_ctx_head head; uint32_t fpsr, fpcr; __uint128_t vregs[32]; };
#define PF_FPSIMD_MAGIC 0x46508001u

static struct pf_fpsimd *a64_fpsimd(ucontext_t *uc)
{
    uint8_t *p = (uint8_t *)uc->uc_mcontext.__reserved;
    uint8_t *end = p + sizeof(uc->uc_mcontext.__reserved);
    while (p + sizeof(struct pf_ctx_head) <= end) {
        struct pf_ctx_head *h = (struct pf_ctx_head *)p;
        if (h->magic == 0 || h->size == 0) break;
        if (h->magic == PF_FPSIMD_MAGIC) return (struct pf_fpsimd *)p;
        p += h->size;
    }
    return NULL;
}

static int emulate_a64(ucontext_t *uc, const PfRange *rg, uintptr_t addr)
{
    uint32_t insn = *(const uint32_t *)(uintptr_t)uc->uc_mcontext.pc;
    unsigned size = insn >> 30, V = (insn >> 26) & 1, opc = (insn >> 22) & 3;
    unsigned Rn = (insn >> 5) & 31, Rt = insn & 31;
    int writeback = 0;
    int64_t imm9 = 0;

    if ((insn & 0x3B000000u) == 0x39000000u) {
        /* unsigned scaled immediate */
    } else if ((insn & 0x3B200C00u) == 0x38200800u) {
        /* register offset */
    } else if ((insn & 0x3B200000u) == 0x38000000u) {
        unsigned idx = (insn >> 10) & 3;               /* 00 unscaled, 01 post, 10 unpriv, 11 pre */
        if (idx == 1 || idx == 3) {
            writeback = 1;
            imm9 = (int64_t)((int32_t)(((insn >> 12) & 0x1FF) << 23) >> 23);
        }
    } else {
        return 0;                                       /* pairs, exclusives, atomics: not for MMIO */
    }

    if (!V) {
        unsigned bytes = 1u << size;
        uint64_t *x = (uint64_t *)uc->uc_mcontext.regs;
        if (opc == 0) {                                  /* STR* */
            uint64_t val = Rt == 31 ? 0 : x[Rt];
            dev_write(rg, addr, val & size_mask(bytes), bytes);
        } else if (opc == 1) {                           /* LDR* zero-extend */
            uint64_t val = dev_read(rg, addr, bytes) & size_mask(bytes);
            if (Rt != 31) x[Rt] = val;
        } else if (size == 3) {
            /* PRFM: a prefetch never reaches a device */
        } else if (opc == 2) {                           /* LDRS* to Xt */
            uint64_t val = sext(dev_read(rg, addr, bytes), bytes);
            if (Rt != 31) x[Rt] = val;
        } else {                                         /* LDRS* to Wt */
            if (size == 2) return 0;
            uint64_t val = sext(dev_read(rg, addr, bytes), bytes) & 0xFFFFFFFFull;
            if (Rt != 31) x[Rt] = val;
        }
    } else {
        struct pf_fpsimd *fp = a64_fpsimd(uc);
        unsigned bytes = (size == 0 && (opc & 2)) ? 16 : (1u << size);
        int load = opc & 1;
        if (!fp) return 0;
        if (bytes == 16) {
            if (load) {
                uint64_t lo = dev_read(rg, addr, 8), hi = dev_read(rg, addr + 8, 8);
                fp->vregs[Rt] = ((__uint128_t)hi << 64) | lo;
            } else {
                dev_write(rg, addr, (uint64_t)fp->vregs[Rt], 8);
                dev_write(rg, addr + 8, (uint64_t)(fp->vregs[Rt] >> 64), 8);
            }
        } else if (load) {
            fp->vregs[Rt] = (__uint128_t)(dev_read(rg, addr, bytes) & size_mask(bytes));
        } else {
            dev_write(rg, addr, (uint64_t)fp->vregs[Rt] & size_mask(bytes), bytes);
        }
    }

    if (writeback) {
        if (Rn == 31) uc->uc_mcontext.sp += (uint64_t)imm9;
        else uc->uc_mcontext.regs[Rn] += (uint64_t)imm9;
    }
    uc->uc_mcontext.pc += 4;
    return 1;
}


static uintptr_t uc_pc(ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.pc; }
#define EMULATE emulate_a64

#else
#error "posix_fault.c: add an instruction decoder for this CPU"
#endif

/* ======================================================================== */
/* signal handling                                                           */
/* ======================================================================== */

static const char *sig_name(int sig, int code)
{
    switch (sig) {
    case SIGSEGV: return "access violation (SIGSEGV)";
    case SIGBUS:  return "bus error (SIGBUS)";
    case SIGILL:  return "illegal instruction (SIGILL)";
    case SIGFPE:  return code == FPE_INTDIV ? "integer divide by zero (SIGFPE)"
                                            : "arithmetic exception (SIGFPE)";
    default:      return "fatal signal";
    }
}

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    int saved_errno = errno;

    if (sig == SIGSEGV || sig == SIGBUS) {
        const PfRange *rg = find_range((uintptr_t)si->si_addr);
        if (rg) {
            if (EMULATE(uc, rg, (uintptr_t)si->si_addr)) {
                __atomic_add_fetch(&s_emulated, 1, __ATOMIC_RELAXED);
                errno = saved_errno;
                return;
            }
            if (__atomic_add_fetch(&s_failed, 1, __ATOMIC_RELAXED) <= 20) {
                const uint8_t *b = (const uint8_t *)uc_pc(uc);
                fprintf(stderr, "[FAULT] %s: undecoded access at %p, pc %p: "
                        "%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        rg->name, si->si_addr, (void *)uc_pc(uc),
                        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            }
        }
    }

    {
        void *frames[32];
        int n = CaptureStackBackTrace(0, 32, frames, NULL);
        if (s_crash)
            s_crash(sig, sig_name(sig, si->si_code), si->si_addr, uc_pc(uc), frames, n);
    }
    /* Let the default action run (core dump, Android tombstone). */
    signal(sig, SIG_DFL);
    errno = saved_errno;
}

void pf_install(pf_crash_fn crash)
{
    static int done;
    struct sigaction sa;
    s_crash = crash;
    if (done) return;
    done = 1;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;   /* a device handler may itself fault */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}

#endif /* !_WIN32 */
