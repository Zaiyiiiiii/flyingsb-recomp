#include "host.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#endif

#define VMEM_BASE GUEST_VMEM_BASE
#define VMEM_END  GUEST_ARENA_END

static uint32_t committed_low_end;
static uint32_t arena_committed;   /* 堆区已提交到的位置 */

static int commit_range(uint32_t addr, uint32_t size) {
#ifdef _WIN32
    return VirtualAlloc(g_mem + addr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    (void)addr; (void)size;
    return 1;
#endif
}

int gm_init(void) {
#ifdef _WIN32
    g_mem = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)GUEST_ARENA_END, MEM_RESERVE, PAGE_NOACCESS);
#elif defined(__EMSCRIPTEN__)
    g_mem = (uint8_t *)calloc(1, (size_t)GUEST_ARENA_END);
#else
    void *p = mmap(NULL, (size_t)GUEST_ARENA_END, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_mem = (p == MAP_FAILED) ? NULL : (uint8_t *)p;
#endif
    if (!g_mem) return 0;
    if (!commit_range(0, GUEST_THUNK_END)) return 0;
    committed_low_end = GUEST_THUNK_END;
    arena_committed = GUEST_ARENA_BASE;
    return 1;
}

/* ---------------------------------------------------------------- 堆分配器
 * 块头 8 字节：[size|flags][prev_size]。size 为含头的总大小(16 的倍数)，
 * bit0 = 本块在用，bit1 = 前一块在用。空闲块尾部 4 字节存 size，
 * 空闲块有效载荷前 8 字节存双向链表指针。块地址 ≡ 8 (mod 16)，载荷 16 对齐。 */
#define F_USED 1u
#define F_PREV_USED 2u
#define MIN_CHUNK 32u
#define NBINS 96

static uint32_t bins[NBINS];     /* 每个 bin 链表头（块地址，0 = 空） */
static uint32_t heap_start, top_chunk, top_size;
static int heap_ready;

static inline uint32_t ch_size(uint32_t c) { return rt_rd32(g_mem, c) & ~15u; }
static inline uint32_t ch_flags(uint32_t c) { return rt_rd32(g_mem, c) & 3u; }
static inline void ch_set(uint32_t c, uint32_t size, uint32_t flags) { rt_wr32(g_mem, c, size | flags); }
static inline uint32_t fl_next(uint32_t c) { return rt_rd32(g_mem, c + 8); }
static inline uint32_t fl_prev(uint32_t c) { return rt_rd32(g_mem, c + 12); }

static int bin_index(uint32_t size) {
    if (size < 1024 + 16) return (int)((size - MIN_CHUNK) >> 4);       /* 0..63 */
    int lg = 0;
    uint32_t s = size;
    while (s > 1) { s >>= 1; lg++; }                                    /* >= 10 */
    int idx = 64 + (lg - 10) * 2 + (int)((size >> (lg - 1)) & 1);
    return idx >= NBINS ? NBINS - 1 : idx;
}

static void bin_insert(uint32_t c) {
    uint32_t sz = ch_size(c);
    int b = bin_index(sz);
    uint32_t head = bins[b];
    rt_wr32(g_mem, c + 8, head);
    rt_wr32(g_mem, c + 12, 0);
    if (head) rt_wr32(g_mem, head + 12, c);
    bins[b] = c;
    rt_wr32(g_mem, c + sz - 4, sz);
}

static void bin_remove(uint32_t c) {
    uint32_t n = fl_next(c), p = fl_prev(c);
    if (p) rt_wr32(g_mem, p + 8, n); else bins[bin_index(ch_size(c))] = n;
    if (n) rt_wr32(g_mem, n + 12, p);
}

static void heap_init(void) {
    heap_start = GUEST_ARENA_BASE + 8;
    top_chunk = heap_start;
    top_size = 0;
    heap_ready = 1;
}

static int heap_grow(uint32_t need) {
    uint32_t want = (need + 0xFFFFFu) & ~0xFFFFFu;
    if (want < 0x400000u) want = 0x400000u;
    uint32_t cur_end = top_chunk + top_size;
    uint32_t limit = VMEM_BASE;
    if (cur_end - 8 + want > limit) return 0;
    if (!commit_range(arena_committed, want)) return 0;
    arena_committed += want;
    top_size += want;
    return 1;
}

uint32_t gm_alloc(uint32_t n) {
    if (!heap_ready) heap_init();
    uint32_t need = (n + 8 + 15) & ~15u;
    if (need < MIN_CHUNK) need = MIN_CHUNK;
    for (int b = bin_index(need); b < NBINS; b++) {
        for (uint32_t c = bins[b]; c; c = fl_next(c)) {
            uint32_t sz = ch_size(c);
            if (sz < need) continue;
            bin_remove(c);
            uint32_t pf = ch_flags(c) & F_PREV_USED;
            if (sz - need >= MIN_CHUNK) {
                uint32_t r = c + need;
                ch_set(c, need, F_USED | pf);
                ch_set(r, sz - need, F_PREV_USED);
                bin_insert(r);
            } else {
                ch_set(c, sz, F_USED | pf);
                uint32_t nx = c + sz;
                ch_set(nx, ch_size(nx), ch_flags(nx) | F_PREV_USED);
            }
            return c + 8;
        }
    }
    /* 从 top 切 */
    if (top_size < need + MIN_CHUNK) {
        if (!heap_grow(need + MIN_CHUNK - top_size)) return 0;
    }
    uint32_t c = top_chunk;
    uint32_t pf = (c == heap_start) ? F_PREV_USED : (rt_rd32(g_mem, c) & F_PREV_USED);
    ch_set(c, need, F_USED | pf);
    top_chunk = c + need;
    top_size -= need;
    ch_set(top_chunk, 0, F_PREV_USED);
    return c + 8;
}

uint32_t gm_size(uint32_t p) {
    return ch_size(p - 8) - 8;
}

void gm_free(uint32_t p) {
    if (!p) return;
    uint32_t c = p - 8;
    uint32_t sz = ch_size(c);
    uint32_t fl = ch_flags(c);
    if (!(fl & F_USED)) host_fatal("来宾堆: 重复释放 0x%08X", p);
    /* 与前一块合并 */
    if (!(fl & F_PREV_USED)) {
        uint32_t psz = rt_rd32(g_mem, c - 4);
        uint32_t pc = c - psz;
        bin_remove(pc);
        c = pc; sz += psz;
        fl = ch_flags(pc);
    }
    uint32_t nx = c + sz;
    if (nx == top_chunk) {
        /* 并入 top */
        top_size += sz;
        top_chunk = c;
        ch_set(c, 0, fl & F_PREV_USED);
        return;
    }
    if (!(ch_flags(nx) & F_USED)) {
        bin_remove(nx);
        sz += ch_size(nx);
        nx = c + sz;
    }
    ch_set(c, sz, fl & F_PREV_USED);
    bin_insert(c);
    ch_set(nx, ch_size(nx), ch_flags(nx) & ~F_PREV_USED);
}

uint32_t gm_calloc(uint32_t n) {
    uint32_t p = gm_alloc(n);
    if (p) memset(g_mem + p, 0, n);
    return p;
}

uint32_t gm_realloc(uint32_t p, uint32_t n) {
    if (!p) return gm_alloc(n);
    uint32_t old = gm_size(p);
    if (n <= old) return p;
    uint32_t q = gm_alloc(n);
    if (!q) return 0;
    memcpy(g_mem + q, g_mem + p, old);
    gm_free(p);
    return q;
}

uint32_t gm_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    uint32_t p = gm_alloc((uint32_t)n);
    if (p) memcpy(g_mem + p, s, n);
    return p;
}

/* ---------------------------------------------------------------- 虚拟内存区(VirtualAlloc)
 * 以 64KB 粒度的区间表管理 [VMEM_BASE, VMEM_END)，首次适配。 */
typedef struct { uint32_t base, size; int used; } VExt;
static VExt *vext;
static int vn, vcap;
static uint32_t vtop = VMEM_BASE;     /* 已提交到的位置 */

static void vins(int at, VExt e) {
    if (vn == vcap) { vcap = vcap ? vcap * 2 : 64; vext = (VExt *)realloc(vext, vcap * sizeof(VExt)); }
    memmove(vext + at + 1, vext + at, (vn - at) * sizeof(VExt));
    vext[at] = e; vn++;
}

uint32_t gm_valloc(uint32_t size) {
    size = (size + 0xFFFFu) & ~0xFFFFu;
    if (!size) size = 0x10000;
    for (int i = 0; i < vn; i++) {
        if (!vext[i].used && vext[i].size >= size) {
            uint32_t b = vext[i].base;
            if (vext[i].size > size) {
                VExt rest = { b + size, vext[i].size - size, 0 };
                vext[i].size = size;
                vins(i + 1, rest);
            }
            vext[i].used = 1;
            memset(g_mem + b, 0, size);
            return b;
        }
    }
    if ((uint64_t)vtop + size > VMEM_END) return 0;
    if (!commit_range(vtop, size)) return 0;
    VExt e = { vtop, size, 1 };
    vins(vn, e);
    vtop += size;
    return e.base;   /* 新提交内存本身为零 */
}

int gm_vfree(uint32_t addr) {
    for (int i = 0; i < vn; i++) {
        if (vext[i].base == addr && vext[i].used) {
            vext[i].used = 0;
            if (i + 1 < vn && !vext[i + 1].used) { vext[i].size += vext[i + 1].size; memmove(vext + i + 1, vext + i + 2, (vn - i - 2) * sizeof(VExt)); vn--; }
            if (i > 0 && !vext[i - 1].used) { vext[i - 1].size += vext[i].size; memmove(vext + i, vext + i + 1, (vn - i - 1) * sizeof(VExt)); vn--; }
            return 1;
        }
    }
    return 0;
}

uint32_t gm_vsize(uint32_t addr) {
    for (int i = 0; i < vn; i++) if (vext[i].used && addr >= vext[i].base && addr < vext[i].base + vext[i].size) return vext[i].base + vext[i].size - addr;
    return 0;
}
