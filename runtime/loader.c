/* 加载器：把用户提供的原版 FlyingSB.exe 映射进来宾内存。
 * 重编译产物里只有代码；全部数据（.rdata/.data/.rsrc 以及作为数据读取的 .text）在运行时由用户的原版文件提供。 */
#include "host.h"
#include <stdlib.h>
#include <string.h>

int g_exit_requested;

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int loader_load(const char *exe_path) {
    FILE *fp = host_fopen(exe_path, "rb");
    if (!fp) { host_log("无法打开 %s", exe_path); return 0; }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc((size_t)fsz);
    if (!file || fread(file, 1, (size_t)fsz, fp) != (size_t)fsz) { fclose(fp); host_log("读取 %s 失败", exe_path); return 0; }
    fclose(fp);
    if (fsz < 0x200 || file[0] != 'M' || file[1] != 'Z') { host_log("不是有效的 PE 文件"); free(file); return 0; }
    uint32_t pe = u32(file + 0x3C);
    if (memcmp(file + pe, "PE\0\0", 4) != 0) { free(file); return 0; }
    int nsec = u16(file + pe + 6);
    uint32_t optsz = u16(file + pe + 20);
    const uint8_t *opt = file + pe + 24;
    uint32_t base = u32(opt + 28);
    uint32_t size_of_image = u32(opt + 56);
    uint32_t size_of_headers = u32(opt + 60);
    if (base != g_image_base || size_of_image != g_image_size) {
        host_log("原版可执行文件与重编译版本不符 (base=%08X size=%08X)", base, size_of_image);
        free(file);
        return 0;
    }
    memset(g_mem + base, 0, size_of_image);
    memcpy(g_mem + base, file, size_of_headers);
    const uint8_t *st = file + pe + 24 + optsz;
    for (int i = 0; i < nsec; i++, st += 40) {
        uint32_t va = u32(st + 12), raw = u32(st + 20), rawsz = u32(st + 16), vsz = u32(st + 8);
        uint32_t n = rawsz < vsz || vsz == 0 ? rawsz : vsz;
        if (raw + n > (uint32_t)fsz) n = (uint32_t)fsz - raw;
        memcpy(g_mem + base + va, file + raw, n);
    }
    free(file);

    /* 重编译代码是按“免光盘”版 exe 生成的。原版 exe 与它只差 4 个字节（光盘盘符检测的跳转和路径格式串），
     * 在内存里补上同样的字节，原版和免光盘版就都能运行；已经是补丁版的不受影响 */
    static const struct { uint32_t va; uint8_t orig[3], patched[3]; int n; } nocd[] = {
        { 0x0043B278u, { 0x0C }, { 0x59 }, 1 },
        { 0x00559E88u, { 0x25, 0x63, 0x3A }, { 0x2E, 0x5C, 0x2E }, 3 },
    };
    for (size_t i = 0; i < sizeof nocd / sizeof nocd[0]; i++)
        if (memcmp(g_mem + nocd[i].va, nocd[i].orig, (size_t)nocd[i].n) == 0)
            memcpy(g_mem + nocd[i].va, nocd[i].patched, (size_t)nocd[i].n);

    /* 用宿主函数填充导入地址表 */
    for (int i = 0; i < g_import_count; i++) {
        const RtImportSlot *s = &g_imports[i];
        RtHostFn fn = api_lookup(s->name);
        uint32_t thunk;
        if (fn) {
            thunk = rt_register_thunk(s->name, fn);
        } else {
            host_log("警告: 未实现的导入 %s!%s", s->dll, s->name);
            thunk = rt_register_thunk(s->name, NULL);
        }
        rt_wr32(g_mem, s->slot, thunk);
        if (g_trace_api) host_log("[iat] %08X <- %08X %s", s->slot, thunk, s->name);
    }
    return 1;
}

void loader_run(void) {
    /* TIB: fs:[0]=SEH 链(-1)，fs:[4]=栈顶，fs:[8]=栈底，fs:[0x18]=自指 */
    uint32_t tib = GUEST_TIB_BASE;
    memset(g_mem + tib, 0, 0x1000);
    rt_wr32(g_mem, tib + 0x00, 0xFFFFFFFFu);
    rt_wr32(g_mem, tib + 0x04, GUEST_STACK_TOP);
    rt_wr32(g_mem, tib + 0x08, GUEST_STACK_LOW);
    rt_wr32(g_mem, tib + 0x18, tib);
    memset(&G, 0, sizeof G);
    G.esp = GUEST_STACK_TOP - 64;
    rt_wr32(g_mem, G.esp, 0xFFFFFFF0u);       /* 入口函数的返回地址（不应返回） */
    fp_init();
    fp_cw = 0x027F;
    rt_call_ind(g_entry);
    host_exit((int)G.eax);
}
