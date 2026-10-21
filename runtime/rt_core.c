#include "rt.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

RtRegs G;
uint8_t *g_mem;

double fp_st[8];
int fp_top;
uint16_t fp_cw = 0x027F, fp_swc;

const uint8_t rt_parity[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
};

/* ------------------------------------------------------------------ 字符串指令 */
void rt_movs(uint8_t *m, uint32_t *esi, uint32_t *edi, uint32_t cnt, int w, int df) {
    uint32_t s = *esi, d = *edi;
    if (!df) {
        uint64_t len = (uint64_t)cnt * w;
        if (d + len <= s || s + len <= d) {
            memcpy(m + d, m + s, (size_t)len);
        } else {
            for (uint32_t i = 0; i < cnt; i++) memmove(m + d + i * w, m + s + i * w, w);
        }
        *esi = s + (uint32_t)len;
        *edi = d + (uint32_t)len;
    } else {
        for (uint32_t i = 0; i < cnt; i++) {
            memmove(m + d, m + s, w);
            s -= w; d -= w;
        }
        *esi = s; *edi = d;
    }
}

void rt_stos(uint8_t *m, uint32_t *edi, uint32_t cnt, int w, int df, uint32_t val) {
    uint32_t d = *edi;
    if (!df) {
        if (w == 1) {
            memset(m + d, (int)(val & 0xFF), cnt);
        } else if (w == 2) {
            uint16_t v = (uint16_t)val;
            for (uint32_t i = 0; i < cnt; i++) memcpy(m + d + i * 2, &v, 2);
        } else {
            for (uint32_t i = 0; i < cnt; i++) memcpy(m + d + i * 4, &val, 4);
        }
        *edi = d + cnt * w;
    } else {
        for (uint32_t i = 0; i < cnt; i++) {
            memcpy(m + d, &val, w);
            d -= w;
        }
        *edi = d;
    }
}

static uint32_t rd_w(const uint8_t *m, uint32_t a, int w) {
    uint32_t v = 0;
    memcpy(&v, m + a, w);
    return v;
}

int rt_scas(uint8_t *m, uint32_t *edi, uint32_t *ecx, int w, int df, int rep, uint32_t acc, uint32_t *ra, uint32_t *rb) {
    uint32_t d = *edi, c = *ecx;
    int ran = 0;
    int step = df ? -w : w;
    if (rep == 0) {
        *ra = acc; *rb = rd_w(m, d, w); d += step; *edi = d;
        return 1;
    }
    while (c) {
        uint32_t v = rd_w(m, d, w);
        *ra = acc; *rb = v;
        d += step; c--; ran = 1;
        int eq = ((acc ^ v) & (w == 4 ? 0xFFFFFFFFu : (1u << (w * 8)) - 1)) == 0;
        if (rep == 1 && !eq) break;   /* repe: 相等才继续 */
        if (rep == 2 && eq) break;    /* repne: 不等才继续 */
    }
    *edi = d; *ecx = c;
    return ran;
}

int rt_cmps(uint8_t *m, uint32_t *esi, uint32_t *edi, uint32_t *ecx, int w, int df, int rep, uint32_t *ra, uint32_t *rb) {
    uint32_t s = *esi, d = *edi, c = *ecx;
    int ran = 0;
    int step = df ? -w : w;
    uint32_t mask = (w == 4 ? 0xFFFFFFFFu : (1u << (w * 8)) - 1);
    if (rep == 0) {
        *ra = rd_w(m, s, w); *rb = rd_w(m, d, w);
        *esi = s + step; *edi = d + step;
        return 1;
    }
    while (c) {
        uint32_t a = rd_w(m, s, w), b = rd_w(m, d, w);
        *ra = a; *rb = b;
        s += step; d += step; c--; ran = 1;
        int eq = ((a ^ b) & mask) == 0;
        if (rep == 1 && !eq) break;
        if (rep == 2 && eq) break;
    }
    *esi = s; *edi = d; *ecx = c;
    return ran;
}

/* ------------------------------------------------------------------ x87 */
void fp_init(void) {
    fp_top = 0; fp_cw = 0x037F; fp_swc = 0;
    memset(fp_st, 0, sizeof fp_st);
}

void fp_com(double a, double b) {
    uint16_t s = fp_swc & ~0x4500;
    if (a != a || b != b) s |= 0x4500;
    else if (a > b) { }
    else if (a < b) s |= 0x0100;
    else s |= 0x4000;
    fp_swc = s;
}

void fp_xam(void) {
    double v = ST(0);
    uint16_t s = fp_swc & ~0x4700;
    if (signbit(v)) s |= 0x0200;
    if (v != v) s |= 0x0100;
    else if (isinf(v)) s |= 0x0500;
    else if (v == 0) s |= 0x4000;
    else s |= 0x0400;
    fp_swc = s;
}

void fp_prem(int ieee) {
    double a = ST(0), b = ST(1);
    double r = ieee ? remainder(a, b) : fmod(a, b);
    double q = trunc(a / b);
    int64_t qi = (int64_t)fabs(q);
    ST(0) = r;
    fp_swc = (uint16_t)(fp_swc & ~0x4700);
    if (qi & 1) fp_swc |= 0x0200;
    if (qi & 2) fp_swc |= 0x4000;
    if (qi & 4) fp_swc |= 0x0100;
}

double fp_rint(double v) {
    switch ((fp_cw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

int64_t fp_round(double v) {
    double r = fp_rint(v);
    if (!(r > -9.2233720368547758e18 && r < 9.2233720368547758e18)) return INT64_MIN;
    return (int64_t)r;
}

int64_t fp_trunc(double v) {
    if (!(v > -9.2233720368547758e18 && v < 9.2233720368547758e18)) return INT64_MIN;
    return (int64_t)v;
}

double rt_ld80(const uint8_t *p) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, p, 8); memcpy(&se, p + 8, 2);
    int sign = se >> 15, e = se & 0x7FFF;
    if (e == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (e == 0x7FFF) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double v = ldexp((double)mant, e - 16383 - 63);
    return sign ? -v : v;
}

void rt_st80(uint8_t *p, double v) {
    uint64_t mant = 0; uint16_t se = 0;
    if (v != v) { mant = 0xC000000000000000ull; se = 0x7FFF; }
    else if (isinf(v)) { mant = 0x8000000000000000ull; se = 0x7FFF | (v < 0 ? 0x8000 : 0); }
    else if (v == 0) { se = signbit(v) ? 0x8000 : 0; }
    else {
        int e; double f = frexp(fabs(v), &e);   /* f in [0.5,1) */
        mant = (uint64_t)ldexp(f, 64);
        se = (uint16_t)((e - 1 + 16383) | (v < 0 ? 0x8000 : 0));
    }
    memcpy(p, &mant, 8); memcpy(p + 8, &se, 2);
}

/* fnsave/frstor：采用自有布局（宿主私有，来宾只会原样回读） */
void rt_fsave(uint8_t *p) {
    memset(p, 0, 108);
    memcpy(p, &fp_cw, 2);
    uint16_t sw = fp_sw();
    memcpy(p + 4, &sw, 2);
    for (int i = 0; i < 8; i++) rt_st80(p + 28 + i * 10, ST(i));
    fp_init();
}
void rt_frstor(const uint8_t *p) {
    uint16_t sw;
    memcpy(&fp_cw, p, 2); memcpy(&sw, p + 4, 2);
    fp_top = (sw >> 11) & 7; fp_swc = sw & 0x4700;
    for (int i = 0; i < 8; i++) ST(i) = rt_ld80(p + 28 + i * 10);
}
void rt_fstenv(uint8_t *p) {
    memset(p, 0, 28);
    memcpy(p, &fp_cw, 2);
    uint16_t sw = fp_sw();
    memcpy(p + 4, &sw, 2);
}
void rt_fldenv(const uint8_t *p) {
    uint16_t sw;
    memcpy(&fp_cw, p, 2); memcpy(&sw, p + 4, 2);
    fp_top = (sw >> 11) & 7; fp_swc = sw & 0x4700;
}

/* ------------------------------------------------------------------ 分派 */
#define HT_BITS 15
#define HT_SIZE (1 << HT_BITS)
static RtFuncEntry ht[HT_SIZE];
static int ht_ready;

typedef struct { const char *name; RtHostFn fn; } Thunk;
static Thunk *thunks;
static int n_thunks, cap_thunks;

static void ht_build(void) {
    for (int i = 0; i < g_func_count; i++) {
        uint32_t h = (g_func_table[i].addr * 2654435761u) >> (32 - HT_BITS);
        while (ht[h].addr) h = (h + 1) & (HT_SIZE - 1);
        ht[h] = g_func_table[i];
    }
    ht_ready = 1;
}

static void (*lookup(uint32_t addr))(void) {
    uint32_t h = (addr * 2654435761u) >> (32 - HT_BITS);
    while (ht[h].addr) {
        if (ht[h].addr == addr) return ht[h].fn;
        h = (h + 1) & (HT_SIZE - 1);
    }
    return NULL;
}

uint32_t rt_register_thunk(const char *name, RtHostFn fn) {
    if (n_thunks == cap_thunks) {
        cap_thunks = cap_thunks ? cap_thunks * 2 : 256;
        thunks = (Thunk *)realloc(thunks, cap_thunks * sizeof(Thunk));
    }
    thunks[n_thunks].name = name;
    thunks[n_thunks].fn = fn;
    return GUEST_THUNK_BASE + (uint32_t)(n_thunks++) * 4;
}

const char *rt_thunk_name(uint32_t addr) {
    if (addr >= GUEST_THUNK_BASE && addr < GUEST_THUNK_BASE + (uint32_t)n_thunks * 4) return thunks[(addr - GUEST_THUNK_BASE) / 4].name;
    return NULL;
}

void rt_call_ind(uint32_t addr) {
    if (addr >= GUEST_THUNK_BASE && addr < GUEST_THUNK_END) {
        uint32_t i = (addr - GUEST_THUNK_BASE) / 4;
        if (i < (uint32_t)n_thunks) {
            if (!thunks[i].fn) host_fatal("来宾调用了未实现的 API: %s", thunks[i].name);
            if (g_trace_api) host_log("[api] %s@%08X ret=%08X (esp=%08X) 参数: %08X %08X %08X %08X", thunks[i].name, addr, rt_rd32(g_mem, G.esp), G.esp,
                                      rt_rd32(g_mem, G.esp + 4), rt_rd32(g_mem, G.esp + 8), rt_rd32(g_mem, G.esp + 12), rt_rd32(g_mem, G.esp + 16));
            thunks[i].fn();
            return;
        }
    }
    if (!ht_ready) ht_build();
    void (*f)(void) = lookup(addr);
    if (!f) {
        /* 没有对应的重编译函数：不退出，记一条日志后当作空函数返回 0（按 cdecl 只弹返回地址，参数个数未知）。
         * 同一地址只记一次；要彻底解决就把它加进 recomp/extra_roots.txt */
        static uint32_t seen[64];
        static int n_seen;
        int logged = 0;
        for (int i = 0; i < n_seen; i++) if (seen[i] == addr) { logged = 1; break; }
        if (!logged) {
            if (n_seen < 64) seen[n_seen++] = addr;
            host_log("[警告] 间接调用到未知来宾地址 0x%08X (esp=0x%08X)，已跳过", addr, G.esp);
        }
        G.eax = 0;
        G.esp += 4;
        return;
    }
    f();
}

void rt_jmp_ind(uint32_t addr) { rt_call_ind(addr); }

RT_NORETURN void rt_unimpl(uint32_t addr, const char *msg) {
    host_fatal("来宾代码 0x%08X 触发未实现路径: %s", addr, msg);
}

RT_NORETURN void rt_divzero(void) {
    host_fatal("来宾除零错误");
}

uint32_t rt_guest_call(uint32_t addr, int nargs, const uint32_t *args) {
    RtRegs saved = G;
    uint32_t esp = G.esp;
    for (int i = nargs - 1; i >= 0; i--) { esp -= 4; rt_wr32(g_mem, esp, args[i]); }
    esp -= 4; rt_wr32(g_mem, esp, 0xFFFFFFF0u);
    G.esp = esp;
    rt_call_ind(addr);
    uint32_t r = G.eax;
    G = saved;
    return r;
}
