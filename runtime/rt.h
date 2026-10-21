/* 重编译运行时：来宾内存模型、寄存器、x87 与分派接口。
 * 生成的 gen_*.c 与手写的 API 层都只依赖本头文件。 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#ifdef _MSC_VER
#include <intrin.h>
#define RT_NORETURN __declspec(noreturn)
static inline uint32_t rt_bswap32(uint32_t v) { return _byteswap_ulong(v); }
static inline uint32_t rt_ctz(uint32_t v) { unsigned long i; _BitScanForward(&i, v); return i; }
static inline uint32_t rt_clz(uint32_t v) { unsigned long i; _BitScanReverse(&i, v); return 31 - i; }
#else
#define RT_NORETURN __attribute__((noreturn))
static inline uint32_t rt_bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t rt_ctz(uint32_t v) { return (uint32_t)__builtin_ctz(v); }
static inline uint32_t rt_clz(uint32_t v) { return (uint32_t)__builtin_clz(v); }
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 来宾地址空间布局 ---- */
#define GUEST_TIB_BASE    0x00300000u
#define GUEST_STACK_LOW   0x00100000u
#define GUEST_STACK_TOP   0x002F0000u
#define GUEST_THUNK_BASE  0x01000000u   /* 伪地址：指向宿主函数 */
#define GUEST_THUNK_END   0x01100000u
#ifdef __EMSCRIPTEN__
/* wasm 的线性内存是真实占用的，不能保留 2GB 地址：堆 256MB + 虚拟内存区 128MB，共约 402MB */
#define GUEST_ARENA_BASE  0x01200000u
#define GUEST_VMEM_BASE   0x11200000u
#define GUEST_ARENA_END   0x19200000u
#else
#define GUEST_ARENA_BASE  0x10000000u
#define GUEST_VMEM_BASE   0x40000000u
#define GUEST_ARENA_END   0x70000000u
#endif

/* ---- 寄存器 ---- */
typedef struct { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; } RtRegs;
extern RtRegs G;
extern uint8_t *g_mem;

#define REGS_DECL uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; uint8_t *const m = g_mem
#define REGS_LOAD() do { eax = G.eax; ecx = G.ecx; edx = G.edx; ebx = G.ebx; esp = G.esp; ebp = G.ebp; esi = G.esi; edi = G.edi; } while (0)
#define REGS_RELOAD() REGS_LOAD()
#define SPILL() do { G.eax = eax; G.ecx = ecx; G.edx = edx; G.ebx = ebx; G.esp = esp; G.ebp = ebp; G.esi = esi; G.edi = edi; } while (0)

/* ---- 内存访问 ---- */
static inline uint8_t  rt_rd8 (const uint8_t *m, uint32_t a) { return m[a]; }
static inline uint16_t rt_rd16(const uint8_t *m, uint32_t a) { uint16_t v; memcpy(&v, m + a, 2); return v; }
static inline uint32_t rt_rd32(const uint8_t *m, uint32_t a) { uint32_t v; memcpy(&v, m + a, 4); return v; }
static inline uint64_t rt_rd64(const uint8_t *m, uint32_t a) { uint64_t v; memcpy(&v, m + a, 8); return v; }
static inline void rt_wr8 (uint8_t *m, uint32_t a, uint8_t v)  { m[a] = v; }
static inline void rt_wr16(uint8_t *m, uint32_t a, uint16_t v) { memcpy(m + a, &v, 2); }
static inline void rt_wr32(uint8_t *m, uint32_t a, uint32_t v) { memcpy(m + a, &v, 4); }
static inline void rt_wr64(uint8_t *m, uint32_t a, uint64_t v) { memcpy(m + a, &v, 8); }
#define RD8(a)  rt_rd8(m, (a))
#define RD16(a) rt_rd16(m, (a))
#define RD32(a) rt_rd32(m, (a))
#define RD64(a) rt_rd64(m, (a))
#define WR8(a, v)  rt_wr8(m, (a), (v))
#define WR16(a, v) rt_wr16(m, (a), (v))
#define WR32(a, v) rt_wr32(m, (a), (v))
#define WR64(a, v) rt_wr64(m, (a), (v))
static inline float  rt_rdf32(const uint8_t *m, uint32_t a) { float v; memcpy(&v, m + a, 4); return v; }
static inline double rt_rdf64(const uint8_t *m, uint32_t a) { double v; memcpy(&v, m + a, 8); return v; }
static inline void rt_wrf32(uint8_t *m, uint32_t a, float v)  { memcpy(m + a, &v, 4); }
static inline void rt_wrf64(uint8_t *m, uint32_t a, double v) { memcpy(m + a, &v, 8); }
#define RDF32(a) rt_rdf32(m, (a))
#define RDF64(a) rt_rdf64(m, (a))
#define WRF32(a, v) rt_wrf32(m, (a), (v))
#define WRF64(a, v) rt_wrf64(m, (a), (v))

extern const uint8_t rt_parity[256];

/* ---- 字符串指令 ---- */
void rt_movs(uint8_t *m, uint32_t *esi, uint32_t *edi, uint32_t cnt, int w, int df);
void rt_stos(uint8_t *m, uint32_t *edi, uint32_t cnt, int w, int df, uint32_t val);
int rt_scas(uint8_t *m, uint32_t *edi, uint32_t *ecx, int w, int df, int rep, uint32_t acc, uint32_t *ra, uint32_t *rb);
int rt_cmps(uint8_t *m, uint32_t *esi, uint32_t *edi, uint32_t *ecx, int w, int df, int rep, uint32_t *ra, uint32_t *rb);

/* ---- x87 ---- */
extern double fp_st[8];
extern int fp_top;
extern uint16_t fp_cw, fp_swc;
#define ST(i) fp_st[(fp_top + (i)) & 7]
static inline void fp_push(double v) { fp_top = (fp_top - 1) & 7; fp_st[fp_top] = v; }
static inline double fp_pop(void) { double v = fp_st[fp_top]; fp_top = (fp_top + 1) & 7; return v; }
static inline void fp_xch(int i) { double t = ST(0); ST(0) = ST(i); ST(i) = t; }
static inline void fp_inc(void) { fp_top = (fp_top + 1) & 7; }
static inline void fp_dec(void) { fp_top = (fp_top - 1) & 7; }
static inline uint16_t fp_sw(void) { return (uint16_t)(fp_swc | (fp_top << 11)); }
static inline void fp_c2(int v) { fp_swc = (uint16_t)((fp_swc & ~0x400) | (v ? 0x400 : 0)); }
static inline void fp_clex(void) { }
void fp_init(void);
void fp_com(double a, double b);
void fp_xam(void);
void fp_prem(int ieee);
double fp_rint(double v);
int64_t fp_round(double v);
int64_t fp_trunc(double v);
double rt_ld80(const uint8_t *p);
void rt_st80(uint8_t *p, double v);
void rt_fsave(uint8_t *p);
void rt_frstor(const uint8_t *p);
void rt_fstenv(uint8_t *p);
void rt_fldenv(const uint8_t *p);

/* ---- 分派 ---- */
typedef struct { uint32_t addr; void (*fn)(void); } RtFuncEntry;
typedef struct { uint32_t slot; const char *dll; const char *name; } RtImportSlot;
extern const RtFuncEntry g_func_table[];
extern const int g_func_count;
extern const RtImportSlot g_imports[];
extern const int g_import_count;
extern const uint32_t g_image_base, g_image_size, g_entry;
extern const char g_text_sha256[];

void rt_call_ind(uint32_t addr);     /* 间接 call：返回地址已压栈 */
void rt_jmp_ind(uint32_t addr);      /* 间接 jmp（尾调用） */
RT_NORETURN void rt_unimpl(uint32_t addr, const char *msg);
RT_NORETURN void rt_divzero(void);
#define RT_UNIMPL(a, msg) rt_unimpl((a), (msg))
#define RT_DIVCHK(d) do { if (!(d)) rt_divzero(); } while (0)

/* ---- 宿主函数（API 层）注册 ----
 * 宿主函数没有参数：从来宾栈读取 stdcall 参数，结果放入 G.eax 并弹栈。 */
typedef void (*RtHostFn)(void);
uint32_t rt_register_thunk(const char *name, RtHostFn fn);   /* 返回伪地址 */
const char *rt_thunk_name(uint32_t addr);

/* 在宿主函数中调用来宾函数（回调，如窗口过程），参数按 stdcall 压栈，返回 eax */
uint32_t rt_guest_call(uint32_t addr, int nargs, const uint32_t *args);

/* 栈参数访问（在宿主函数内部） */
#define ARG(i) rt_rd32(g_mem, G.esp + 4 + 4 * (i))
#define HOST_RET(v, nargs) do { uint32_t ra_ = rt_rd32(g_mem, G.esp); (void)ra_; G.eax = (uint32_t)(v); G.esp += 4 + 4 * (nargs); } while (0)
#define HOST_RET_CDECL(v) do { G.eax = (uint32_t)(v); G.esp += 4; } while (0)

#ifdef __cplusplus
}
#endif
