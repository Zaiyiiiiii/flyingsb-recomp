/* KERNEL32 子集：堆、虚拟内存、文件、INI、资源、模块、杂项。 */
#include "host.h"
#include "platform.h"
#include "codepage.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define INVALID_HANDLE ((uint32_t)-1)
static uint32_t g_last_error;
const char *g_cmdline_extra = "";

#define SET_ERR(e) (g_last_error = (e))

/* ================================================================ 堆 */
static uint32_t heap_handles[16];
static int nheaps;
#define HEAP_BASE_HANDLE 0x00D00000u

API_FN(HeapCreate) {
    heap_handles[nheaps & 15] = HEAP_BASE_HANDLE + (uint32_t)nheaps * 16;
    HOST_RET(HEAP_BASE_HANDLE + (uint32_t)nheaps++ * 16, 3);
}
API_FN(HeapDestroy) { HOST_RET(1, 1); }
API_FN(HeapAlloc) {
    uint32_t flags = ARG(1), size = ARG(2);
    uint32_t p = (flags & 8) ? gm_calloc(size ? size : 1) : gm_alloc(size ? size : 1);
    HOST_RET(p, 3);
}
API_FN(HeapReAlloc) {
    uint32_t flags = ARG(1), p = ARG(2), size = ARG(3);
    uint32_t old = p ? gm_size(p) : 0;
    uint32_t q = gm_realloc(p, size ? size : 1);
    if (q && (flags & 8) && size > old) memset(g_mem + q + old, 0, size - old);
    HOST_RET(q, 4);
}
API_FN(HeapFree) { gm_free(ARG(2)); HOST_RET(1, 3); }
API_FN(HeapSize) { uint32_t p = ARG(2); HOST_RET(p ? gm_size(p) : (uint32_t)-1, 3); }

/* ================================================================ 虚拟内存 */
API_FN(VirtualAlloc) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    uint32_t r;
    if (addr && !(type & 0x2000)) {
        /* 对已保留区域提交：我们的保留即已提交，直接返回 */
        r = addr;
    } else {
        r = gm_valloc(size);
        if (!r) SET_ERR(8);
    }
    HOST_RET(r, 4);
}
API_FN(VirtualFree) {
    uint32_t addr = ARG(0), type = ARG(2);
    int ok = 1;
    if (type & 0x8000) ok = gm_vfree(addr);
    HOST_RET(ok, 3);
}

/* ================================================================ 进程/杂项 */
API_FN(ExitProcess) { host_exit((int)ARG(0)); }
API_FN(TerminateProcess) { host_exit((int)ARG(1)); }
API_FN(GetVersion) { HOST_RET(0x0A280105u, 0); }
API_FN(GetLastError) { HOST_RET(g_last_error, 0); }
API_FN(GetCurrentProcess) { HOST_RET(0xFFFFFFFFu, 0); }
API_FN(GetCurrentThreadId) { HOST_RET(1, 0); }
API_FN(GetTickCount) { HOST_RET((uint32_t)plat_ticks_ms(), 0); }
API_FN(Sleep) { plat_sleep_ms(ARG(0)); HOST_RET(0, 1); }
API_FN(GetACP) { HOST_RET(950, 0); }
API_FN(GetOEMCP) { HOST_RET(950, 0); }
API_FN(SetHandleCount) { HOST_RET(ARG(0), 1); }
API_FN(UnhandledExceptionFilter) { HOST_RET(1, 1); }
API_FN(RaiseException) { host_fatal("来宾调用 RaiseException(code=0x%08X)", ARG(0)); }
API_FN(RtlUnwind) { host_fatal("来宾调用 RtlUnwind (不支持 SEH 展开)"); }

API_FN(GetSystemInfo) {
    uint32_t p = ARG(0);
    memset(GP(p), 0, 36);
    rt_wr32(g_mem, p + 4, 4096);
    rt_wr32(g_mem, p + 8, 0x10000);
    rt_wr32(g_mem, p + 12, 0x7FFEFFFF);
    rt_wr32(g_mem, p + 20, 1);
    rt_wr32(g_mem, p + 24, 586);
    rt_wr32(g_mem, p + 28, 0x10000);
    HOST_RET(0, 1);
}

API_FN(GetStartupInfoA) {
    uint32_t p = ARG(0);
    memset(GP(p), 0, 68);
    rt_wr32(g_mem, p, 68);
    HOST_RET(0, 1);
}

static uint32_t cmdline_addr;
API_FN(GetCommandLineA) {
    if (!cmdline_addr) {
        char buf[1024];
        snprintf(buf, sizeof buf, "\"C:\\FlyingSB\\FlyingSB.exe\"%s%s", *g_cmdline_extra ? " " : "", g_cmdline_extra);
        cmdline_addr = gm_strdup(buf);
    }
    HOST_RET(cmdline_addr, 0);
}

static uint32_t env_a, env_w;
API_FN(GetEnvironmentStrings) {
    if (!env_a) { env_a = gm_calloc(8); }
    HOST_RET(env_a, 0);
}
API_FN(GetEnvironmentStringsW) {
    if (!env_w) { env_w = gm_calloc(8); }
    HOST_RET(env_w, 0);
}
API_FN(FreeEnvironmentStringsA) { HOST_RET(1, 1); }
API_FN(FreeEnvironmentStringsW) { HOST_RET(1, 1); }

API_FN(GetLocalTime) {
    uint32_t p = ARG(0);
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    uint16_t v[8] = {(uint16_t)(tm->tm_year + 1900), (uint16_t)(tm->tm_mon + 1), (uint16_t)tm->tm_wday, (uint16_t)tm->tm_mday,
                     (uint16_t)tm->tm_hour, (uint16_t)tm->tm_min, (uint16_t)tm->tm_sec, (uint16_t)(plat_ticks_ms() % 1000)};
    for (int i = 0; i < 8; i++) rt_wr16(g_mem, p + i * 2, v[i]);
    HOST_RET(0, 1);
}

/* ================================================================ 模块与资源 */
#define MAX_MODS 32
typedef struct { char name[64]; PeModule *pe; uint32_t handle; } Mod;
static Mod mods[MAX_MODS];
static int nmods;
#define MOD_HANDLE_BASE 0x7A000000u
#define MOD_EXE 0x00400000u

static PeModule *exe_pe;

static PeModule *module_pe(uint32_t h) {
    if (h == 0 || h == MOD_EXE) {
        if (!exe_pe) {
            char path[2048];
            vfs_resolve("FlyingSB.exe", 0, path, sizeof path);
            exe_pe = pe_open(path);
        }
        return exe_pe;
    }
    for (int i = 0; i < nmods; i++) if (mods[i].handle == h) return mods[i].pe;
    return NULL;
}

static const char *base_name(const char *s) {
    const char *p = strrchr(s, '\\');
    const char *q = strrchr(s, '/');
    if (q > p) p = q;
    return p ? p + 1 : s;
}

API_FN(GetModuleHandleA) {
    uint32_t n = ARG(0);
    if (!n) { HOST_RET(MOD_EXE, 1); return; }
    const char *name = base_name(GSTR(n));
    if (!strcasecmp(name, "FlyingSB.exe") || !strcasecmp(name, "FlyingSB")) { HOST_RET(MOD_EXE, 1); return; }
    for (int i = 0; i < nmods; i++) if (!strcasecmp(mods[i].name, name)) { HOST_RET(mods[i].handle, 1); return; }
    if (!strncasecmp(name, "kernel32", 8)) { HOST_RET(0x7C800000u, 1); return; }
    HOST_RET(0, 1);
}

static uint32_t load_library(const char *path) {
    const char *name = base_name(path);
    for (int i = 0; i < nmods; i++) if (!strcasecmp(mods[i].name, name)) return mods[i].handle;
    if (nmods >= MAX_MODS) return 0;
    char hp[2048];
    int found = 0;
    if (!strchr(path, '\\') && !strchr(path, '/')) {
        /* 与 Windows 一致：先在应用程序目录里找，再找当前目录 */
        char rooted[300];
        snprintf(rooted, sizeof rooted, "C:\\FlyingSB\\%s", path);
        found = vfs_resolve(rooted, 0, hp, sizeof hp);
    }
    if (!found) found = vfs_resolve(path, 0, hp, sizeof hp);
    if (g_trace_api) host_log("[kernel32] LoadLibrary(%s) -> %s found=%d cwd=%s", path, hp, found, vfs_get_cwd());
    if (!found) {
        /* 系统 DLL：给一个伪句柄，GetProcAddress 走宿主注册表 */
        if (!strncasecmp(name, "kernel32", 8) || !strncasecmp(name, "user32", 6) || !strncasecmp(name, "gdi32", 5) ||
            !strncasecmp(name, "ddraw", 5) || !strncasecmp(name, "dsound", 6) || !strncasecmp(name, "winmm", 5)) {
            Mod *m = &mods[nmods];
            snprintf(m->name, sizeof m->name, "%s", name);
            m->pe = NULL;
            m->handle = MOD_HANDLE_BASE + (uint32_t)nmods * 0x10000;
            nmods++;
            return m->handle;
        }
        SET_ERR(126);
        return 0;
    }
    PeModule *pe = pe_open(hp);
    if (!pe) { SET_ERR(193); return 0; }
    Mod *m = &mods[nmods];
    snprintf(m->name, sizeof m->name, "%s", name);
    m->pe = pe;
    m->handle = MOD_HANDLE_BASE + (uint32_t)nmods * 0x10000;
    nmods++;
    host_log("LoadLibrary %s -> %08X", name, m->handle);
    return m->handle;
}

API_FN(LoadLibraryA) { HOST_RET(load_library(ARGS(0)), 1); }
API_FN(LoadLibraryExA) { HOST_RET(load_library(ARGS(0)), 3); }
API_FN(FreeLibrary) { HOST_RET(1, 1); }

API_FN(GetModuleFileNameA) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2);
    const char *s = "C:\\FlyingSB\\FlyingSB.exe";
    char tmp[128];
    if (h != 0 && h != MOD_EXE) {
        for (int i = 0; i < nmods; i++) if (mods[i].handle == h) { snprintf(tmp, sizeof tmp, "C:\\FlyingSB\\%s", mods[i].name); s = tmp; }
    }
    uint32_t len = (uint32_t)strlen(s);
    if (len >= n) len = n ? n - 1 : 0;
    memcpy(g_mem + buf, s, len);
    if (n) g_mem[buf + len] = 0;
    HOST_RET(len, 3);
}

static uint32_t getproc_cache[256];
static const char *getproc_names[256];
static int ngetproc;
API_FN(GetProcAddress) {
    uint32_t nm = ARG(1);
    if (nm < 0x10000) { HOST_RET(0, 2); return; }
    const char *name = GSTR(nm);
    for (int i = 0; i < ngetproc; i++) if (!strcmp(getproc_names[i], name)) { HOST_RET(getproc_cache[i], 2); return; }
    RtHostFn fn = api_lookup(name);
    uint32_t r = 0;
    if (fn && ngetproc < 256) {
        r = rt_register_thunk(strdup(name), fn);
        getproc_names[ngetproc] = strdup(name);
        getproc_cache[ngetproc++] = r;
    } else {
        host_log("GetProcAddress(%s) 未找到", name);
    }
    HOST_RET(r, 2);
}

/* ---- 资源 ---- */
typedef struct { PeModule *pe; uint32_t off, size, guest; } Res;
static Res *ress;
static int nres, capres;
#define RES_HANDLE_BASE 0x7B000000u

API_FN(FindResourceA) {
    uint32_t h = ARG(0), name = ARG(1), type = ARG(2);
    PeModule *pe = module_pe(h);
    uint32_t off, size;
    uint32_t tid = 0, nid = 0;
    const char *tn = NULL, *nn = NULL;
    if (type < 0x10000) tid = type; else { tn = GSTR(type); if (tn[0] == '#') { tid = (uint32_t)atoi(tn + 1); tn = NULL; } }
    if (name < 0x10000) nid = name; else { nn = GSTR(name); if (nn[0] == '#') { nid = (uint32_t)atoi(nn + 1); nn = NULL; } }
    if (!pe || !pe_find_resource(pe, tid, tn, nid, nn, &off, &size)) {
        if (g_trace_api) host_log("FindResource 失败 mod=%08X type=%s name=%s", h, tn ? tn : "#id", nn ? nn : "#id");
        SET_ERR(1813);
        HOST_RET(0, 3);
        return;
    }
    if (nres == capres) { capres = capres ? capres * 2 : 256; ress = (Res *)realloc(ress, capres * sizeof(Res)); }
    ress[nres].pe = pe; ress[nres].off = off; ress[nres].size = size; ress[nres].guest = 0;
    HOST_RET(RES_HANDLE_BASE + (uint32_t)nres++, 3);
}
API_FN(LoadResource) { HOST_RET(ARG(1), 2); }
API_FN(LockResource) {
    uint32_t h = ARG(0);
    int i = (int)(h - RES_HANDLE_BASE);
    if (i < 0 || i >= nres) { HOST_RET(0, 1); return; }
    Res *r = &ress[i];
    if (!r->guest) {
        r->guest = gm_alloc(r->size ? r->size : 1);
        if (!r->guest || !pe_read(r->pe, r->off, r->size, g_mem + r->guest)) host_fatal("读取资源失败 (%s)", pe_path(r->pe));
    }
    HOST_RET(r->guest, 1);
}
API_FN(SizeofResource) {
    int i = (int)(ARG(1) - RES_HANDLE_BASE);
    HOST_RET((i >= 0 && i < nres) ? ress[i].size : 0, 2);
}

/* ================================================================ 文件 */
#define MAX_FILES 256
typedef struct { FILE *fp; int used; char name[260]; } FileEnt;
static FileEnt files[MAX_FILES];
#define FILE_HANDLE_BASE 0x1000u

static FileEnt *file_get(uint32_t h) {
    if (h < FILE_HANDLE_BASE || h >= FILE_HANDLE_BASE + MAX_FILES) return NULL;
    FileEnt *f = &files[h - FILE_HANDLE_BASE];
    return f->used ? f : NULL;
}

API_FN(GetStdHandle) {
    uint32_t n = ARG(0);
    HOST_RET(n == (uint32_t)-10 ? 0x10 : n == (uint32_t)-11 ? 0x11 : 0x12, 1);
}
API_FN(SetStdHandle) { HOST_RET(1, 2); }

API_FN(CreateFileA) {
    const char *name = ARGS(0);
    uint32_t access = ARG(1), disp = ARG(4);
    int write = (access & 0x40000000u) != 0;
    char mode[8];
    int exists = vfs_exists(name);
    if (!write) {
        if (!exists) { SET_ERR(2); HOST_RET(INVALID_HANDLE, 7); return; }
        strcpy(mode, "rb");
    } else {
        switch (disp) {
        case 1: if (exists) { SET_ERR(80); HOST_RET(INVALID_HANDLE, 7); return; } strcpy(mode, "wb+"); break;
        case 2: strcpy(mode, "wb+"); break;
        case 3: if (!exists) { SET_ERR(2); HOST_RET(INVALID_HANDLE, 7); return; } strcpy(mode, "rb+"); break;
        case 4: strcpy(mode, exists ? "rb+" : "wb+"); break;
        case 5: if (!exists) { SET_ERR(2); HOST_RET(INVALID_HANDLE, 7); return; } strcpy(mode, "wb+"); break;
        default: strcpy(mode, "rb+");
        }
    }
    FILE *fp = vfs_fopen(name, mode);
    if (!fp) { SET_ERR(2); HOST_RET(INVALID_HANDLE, 7); return; }
    for (int i = 0; i < MAX_FILES; i++) {
        if (!files[i].used) {
            files[i].used = 1; files[i].fp = fp;
            snprintf(files[i].name, sizeof files[i].name, "%s", name);
            HOST_RET(FILE_HANDLE_BASE + (uint32_t)i, 7);
            return;
        }
    }
    fclose(fp);
    SET_ERR(4);
    HOST_RET(INVALID_HANDLE, 7);
}

API_FN(CloseHandle) {
    FileEnt *f = file_get(ARG(0));
    if (f) { fclose(f->fp); f->used = 0; }
    HOST_RET(1, 1);
}

API_FN(ReadFile) {
    FileEnt *f = file_get(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), pn = ARG(3);
    if (!f) { SET_ERR(6); HOST_RET(0, 5); return; }
    size_t r = fread(g_mem + buf, 1, n, f->fp);
    if (pn) rt_wr32(g_mem, pn, (uint32_t)r);
    HOST_RET(1, 5);
}

API_FN(WriteFile) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pn = ARG(3);
    if (h == 0x11 || h == 0x12) {
        fwrite(g_mem + buf, 1, n, stderr);
        if (pn) rt_wr32(g_mem, pn, n);
        HOST_RET(1, 5);
        return;
    }
    FileEnt *f = file_get(h);
    if (!f) { SET_ERR(6); HOST_RET(0, 5); return; }
    size_t w = fwrite(g_mem + buf, 1, n, f->fp);
    if (pn) rt_wr32(g_mem, pn, (uint32_t)w);
    HOST_RET(1, 5);
}

API_FN(SetFilePointer) {
    FileEnt *f = file_get(ARG(0));
    int32_t lo = (int32_t)ARG(1);
    uint32_t hip = ARG(2), method = ARG(3);
    if (!f) { SET_ERR(6); HOST_RET(INVALID_HANDLE, 4); return; }
    int64_t off = lo;
    if (hip) off |= (int64_t)(int32_t)rt_rd32(g_mem, hip) << 32;
    fseek(f->fp, (long)off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    long pos = ftell(f->fp);
    if (hip) rt_wr32(g_mem, hip, 0);
    HOST_RET((uint32_t)pos, 4);
}

API_FN(SetEndOfFile) {
    FileEnt *f = file_get(ARG(0));
    if (f) {
        fflush(f->fp);
        long pos = ftell(f->fp);
#ifdef _WIN32
        _chsize_s(_fileno(f->fp), pos);
#else
        if (ftruncate(fileno(f->fp), pos)) {}
#endif
    }
    HOST_RET(1, 1);
}
API_FN(FlushFileBuffers) { FileEnt *f = file_get(ARG(0)); if (f) fflush(f->fp); HOST_RET(1, 1); }
API_FN(GetFileType) { uint32_t h = ARG(0); HOST_RET(h < FILE_HANDLE_BASE ? 2 : 1, 1); }

API_FN(DeleteFileA) {
    char p[2048];
    vfs_resolve(ARGS(0), 1, p, sizeof p);
    int ok = remove(p) == 0;
    if (!ok) SET_ERR(2);
    HOST_RET(ok, 1);
}
API_FN(MoveFileA) {
    char a[2048], b[2048];
    vfs_resolve(ARGS(0), 0, a, sizeof a);
    vfs_resolve(ARGS(1), 1, b, sizeof b);
    int ok = rename(a, b) == 0;
    if (!ok) SET_ERR(2);
    HOST_RET(ok, 2);
}

API_FN(GetCurrentDirectoryA) {
    uint32_t n = ARG(0), buf = ARG(1);
    char tmp[600];
    const char *cwd = vfs_get_cwd();
    snprintf(tmp, sizeof tmp, "C:\\FlyingSB%s%s", *cwd ? "\\" : "", cwd);
    for (char *p = tmp; *p; p++) if (*p == '/') *p = '\\';
    uint32_t len = (uint32_t)strlen(tmp);
    if (len + 1 > n) { HOST_RET(len + 1, 2); return; }
    memcpy(g_mem + buf, tmp, len + 1);
    HOST_RET(len, 2);
}
API_FN(SetCurrentDirectoryA) {
    char rel[512];
    const char *p = ARGS(0);
    /* 仅记录相对于游戏根目录的路径 */
    if (!strncasecmp(p, "C:\\FlyingSB", 11)) p += 11;
    while (*p == '\\' || *p == '/') p++;
    snprintf(rel, sizeof rel, "%s", p);
    for (char *q = rel; *q; q++) if (*q == '\\') *q = '/';
    vfs_set_cwd(rel);
    HOST_RET(1, 1);
}

API_FN(GetDriveTypeA) { HOST_RET(3, 1); }
API_FN(GetVolumeInformationA) {
    uint32_t name = ARG(1), nsz = ARG(2), serial = ARG(3);
    if (name && nsz) { strncpy((char *)GP(name), "FLYINGSB", nsz); }
    if (serial) rt_wr32(g_mem, serial, 0x1234ABCDu);
    if (ARG(4)) rt_wr32(g_mem, ARG(4), 255);
    if (ARG(5)) rt_wr32(g_mem, ARG(5), 0);
    if (ARG(6) && ARG(7)) strncpy((char *)GP(ARG(6)), "FAT", ARG(7));
    HOST_RET(1, 8);
}

/* ================================================================ INI */
typedef struct { char *sec, *key, *val; } IniEnt;
typedef struct { IniEnt *e; int n, cap; } Ini;

static void ini_add(Ini *ini, const char *s, const char *k, const char *v) {
    for (int i = 0; i < ini->n; i++)
        if (!strcasecmp(ini->e[i].sec, s) && !strcasecmp(ini->e[i].key, k)) {
            free(ini->e[i].val); ini->e[i].val = strdup(v); return;
        }
    if (ini->n == ini->cap) { ini->cap = ini->cap ? ini->cap * 2 : 64; ini->e = (IniEnt *)realloc(ini->e, ini->cap * sizeof(IniEnt)); }
    ini->e[ini->n].sec = strdup(s); ini->e[ini->n].key = strdup(k); ini->e[ini->n].val = strdup(v);
    ini->n++;
}

static void ini_load(Ini *ini, const char *guest_path) {
    char hp[2048];
    vfs_resolve(guest_path, 0, hp, sizeof hp);
    FILE *fp = host_fopen(hp, "rb");
    if (!fp) return;
    char line[1024], sec[256] = "";
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l - 1] == '\r' || p[l - 1] == '\n' || p[l - 1] == ' ')) p[--l] = 0;
        if (!*p || *p == ';') continue;
        if (*p == '[') { char *e = strchr(p, ']'); if (e) { *e = 0; snprintf(sec, sizeof sec, "%s", p + 1); } continue; }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = p; size_t kl = strlen(k);
        while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
        ini_add(ini, sec, k, eq + 1);
    }
    fclose(fp);
}

static void ini_free(Ini *ini) {
    for (int i = 0; i < ini->n; i++) { free(ini->e[i].sec); free(ini->e[i].key); free(ini->e[i].val); }
    free(ini->e);
}

static void ini_save(Ini *ini, const char *guest_path) {
    char hp[2048];
    vfs_resolve(guest_path, 1, hp, sizeof hp);
    FILE *fp = fopen(hp, "wb");
    if (!fp) return;
    for (int i = 0; i < ini->n; i++) {
        if (i == 0 || strcasecmp(ini->e[i].sec, ini->e[i - 1].sec)) {
            /* 新节：把同节的条目一起输出（只在首次遇到该节时） */
            int seen = 0;
            for (int j = 0; j < i; j++) if (!strcasecmp(ini->e[j].sec, ini->e[i].sec)) seen = 1;
            if (seen) continue;
            fprintf(fp, "[%s]\r\n", ini->e[i].sec);
            for (int j = i; j < ini->n; j++)
                if (!strcasecmp(ini->e[j].sec, ini->e[i].sec)) fprintf(fp, "%s=%s\r\n", ini->e[j].key, ini->e[j].val);
            fprintf(fp, "\r\n");
        }
    }
    fclose(fp);
}

API_FN(GetPrivateProfileStringA) {
    const char *sec = ARG(0) ? ARGS(0) : "";
    const char *key = ARG(1) ? ARGS(1) : NULL;
    /* 画面比例：配置里 [VideoOption] ScreenRatio 为 4:3（原版 800x600）或 16:9（宽屏，高 600、宽 1066）；
     * 没配置时匹配屏幕比例：接近 4:3 的屏幕用原版，更宽的按屏幕比例取宽度（高仍为 600）。
     * 首次读配置时一次性取得，之后由游戏内"遊戲設定"菜单修改（改后需重启生效） */
    static int ratio_loaded;
    if (!ratio_loaded) {
        ratio_loaded = 1;
        char rv[16];
        Ini t = {0};
        ini_load(&t, ARGS(5));
        rv[0] = 0;
        for (int i = 0; i < t.n; i++)
            if (!strcasecmp(t.e[i].sec, "VideoOption") && !strcasecmp(t.e[i].key, "ScreenRatio")) snprintf(rv, sizeof rv, "%s", t.e[i].val);
        ini_free(&t);
        if (!strcmp(rv, "16:9")) { g_res_w = 1066; g_res_h = 600; }
        else if (strcmp(rv, "4:3")) {
            float a = plat_display_aspect();
            if (a >= 1.4f) {
                int w = (int)(600.0f * a + 0.5f) & ~1;
                g_res_w = w < 800 ? 800 : w > 1400 ? 1400 : w;
                g_res_h = 600;
            }
        }
        host_log("画面比例: 配置=%s 屏幕比例=%.3f -> 游戏分辨率 %dx%d", rv[0] ? rv : "(自动)", plat_display_aspect(), g_res_w ? g_res_w : 800, g_res_h ? g_res_h : 600);
    }
    /* 游戏读完 Resolution 并按预设写入屏幕宽高后，在下一次读配置时覆盖成宽屏尺寸 */
    static int res_pending;
    if (res_pending) {
        uint32_t w = (uint32_t)g_res_w, h = (uint32_t)g_res_h;
        memcpy(g_mem + 0x54005Cu, &w, 4);
        memcpy(g_mem + 0x540060u, &h, 4);
        /* 战斗(原 640x480)和过场(原 800x450)的视口是固定尺寸、居中显示：宽屏下把宽度扩到整屏，高度不变，
         * 这样两侧不留黑边，只保留上下黑边 */
        memcpy(g_mem + 0x53CA30u, &w, 4);
        memcpy(g_mem + 0x53CA38u, &w, 4);
        res_pending = 0;
    }
    if (g_res_w && key && !strcasecmp(sec, "VideoOption") && !strcasecmp(key, "Resolution")) res_pending = 1;
    const char *def = ARG(2) ? ARGS(2) : "";
    uint32_t buf = ARG(3), n = ARG(4);
    const char *file = ARGS(5);
    Ini ini = {0};
    ini_load(&ini, file);
    const char *val = def;
    if (key) {
        for (int i = 0; i < ini.n; i++)
            if (!strcasecmp(ini.e[i].sec, sec) && !strcasecmp(ini.e[i].key, key)) { val = ini.e[i].val; break; }
    }
    /* 宽屏：游戏主视口（视口模式 0）放大到整个屏幕；过场(800x450)和 640x480 模式是固定尺寸，不受影响 */
    char vp[16];
    if (g_res_w && key && !strcasecmp(sec, "ENVIRONMENT")) {
        if (!strcasecmp(key, "user_viewportx")) { snprintf(vp, sizeof vp, "%d", g_res_w); val = vp; }
        else if (!strcasecmp(key, "user_viewporty")) { snprintf(vp, sizeof vp, "%d", g_res_h); val = vp; }
    }
    size_t len = strlen(val);
    if (len + 1 > n) len = n ? n - 1 : 0;
    memcpy(g_mem + buf, val, len);
    if (n) g_mem[buf + len] = 0;
    ini_free(&ini);
    HOST_RET(len, 6);
}

/* 宿主侧读写游戏的 FlyingSB.INI（和游戏自己用的是同一份，写入落到存档目录） */
void host_ini_set(const char *sec, const char *key, const char *val) {
    Ini ini = {0};
    ini_load(&ini, "FlyingSB.INI");
    ini_add(&ini, sec, key, val);
    ini_save(&ini, "FlyingSB.INI");
    ini_free(&ini);
}

int host_ini_get(const char *sec, const char *key, char *out, size_t n) {
    Ini ini = {0};
    int found = 0;
    ini_load(&ini, "FlyingSB.INI");
    for (int i = 0; i < ini.n; i++)
        if (!strcasecmp(ini.e[i].sec, sec) && !strcasecmp(ini.e[i].key, key)) { snprintf(out, n, "%s", ini.e[i].val); found = 1; break; }
    ini_free(&ini);
    return found;
}

API_FN(WritePrivateProfileStringA) {
    const char *sec = ARG(0) ? ARGS(0) : "";
    const char *key = ARG(1) ? ARGS(1) : NULL;
    const char *val = ARG(2) ? ARGS(2) : "";
    const char *file = ARGS(3);
    if (key) {
        Ini ini = {0};
        ini_load(&ini, file);
        ini_add(&ini, sec, key, val);
        ini_save(&ini, file);
        ini_free(&ini);
    }
    HOST_RET(1, 4);
}

/* ================================================================ 代码页与字符串 */
API_FN(GetCPInfo) {
    uint32_t cp = ARG(0), p = ARG(1);
    memset(GP(p), 0, 20);
    if (cp == 950 || cp == 949 || cp == 936 || cp == 932) {
        rt_wr32(g_mem, p, 2);
        g_mem[p + 4] = '?';
        g_mem[p + 6] = 0x81; g_mem[p + 7] = 0xFE;
    } else {
        rt_wr32(g_mem, p, 1);
        g_mem[p + 4] = '?';
    }
    HOST_RET(1, 2);
}

API_FN(MultiByteToWideChar) {
    uint32_t cp = ARG(0), src = ARG(2), cb = ARG(3), dst = ARG(4), cch = ARG(5);
    const uint8_t *s = g_mem + src;
    int32_t n = (int32_t)cb;
    if (n < 0) n = (int32_t)strlen((const char *)s) + 1;
    uint32_t out = 0;
    for (int32_t i = 0; i < n;) {
        uint16_t w;
        int used = cp_mb_to_wc(cp, s + i, n - i, &w);
        if (dst) {
            if (out >= cch) { SET_ERR(122); HOST_RET(0, 6); return; }
            rt_wr16(g_mem, dst + out * 2, w);
        }
        out++; i += used;
    }
    HOST_RET(out, 6);
}

API_FN(WideCharToMultiByte) {
    uint32_t cp = ARG(0), src = ARG(2), cch = ARG(3), dst = ARG(4), cb = ARG(5), pdef = ARG(6), pused = ARG(7);
    int32_t n = (int32_t)cch;
    if (n < 0) { n = 0; while (rt_rd16(g_mem, src + n * 2)) n++; n++; }
    uint32_t out = 0;
    int used_def = 0;
    for (int32_t i = 0; i < n; i++) {
        uint16_t w = rt_rd16(g_mem, src + i * 2);
        uint8_t buf[4];
        int k = cp_wc_to_mb(cp, w, buf, &used_def);
        if (dst) {
            if (out + k > cb) { SET_ERR(122); HOST_RET(0, 8); return; }
            memcpy(g_mem + dst + out, buf, k);
        }
        out += k;
    }
    (void)pdef;
    if (pused) rt_wr32(g_mem, pused, used_def);
    HOST_RET(out, 8);
}

API_FN(GetStringTypeW) {
    uint32_t src = ARG(1), cch = ARG(2), out = ARG(3);
    int32_t n = (int32_t)cch;
    if (n < 0) { n = 0; while (rt_rd16(g_mem, src + n * 2)) n++; }
    for (int32_t i = 0; i < n; i++) {
        uint16_t w = rt_rd16(g_mem, src + i * 2);
        uint16_t t = 0;
        if (w < 128) {
            if (isupper(w)) t |= 1;
            if (islower(w)) t |= 2;
            if (isdigit(w)) t |= 4;
            if (isspace(w)) t |= 8;
            if (ispunct(w)) t |= 0x10;
            if (iscntrl(w)) t |= 0x20;
            if (w == ' ') t |= 0x40;
            if (isxdigit(w)) t |= 0x80;
            if (isalpha(w)) t |= 0x100;
        } else t = 0x100;
        rt_wr16(g_mem, out + i * 2, t);
    }
    HOST_RET(1, 4);
}
API_FN(GetStringTypeA) {
    uint32_t src = ARG(2), cch = ARG(3), out = ARG(4);
    int32_t n = (int32_t)cch;
    if (n < 0) n = (int32_t)strlen(GSTR(src));
    for (int32_t i = 0; i < n; i++) {
        uint8_t w = g_mem[src + i];
        uint16_t t = 0;
        if (w < 128) {
            if (isupper(w)) t |= 1;
            if (islower(w)) t |= 2;
            if (isdigit(w)) t |= 4;
            if (isspace(w)) t |= 8;
            if (ispunct(w)) t |= 0x10;
            if (iscntrl(w)) t |= 0x20;
            if (w == ' ') t |= 0x40;
            if (isxdigit(w)) t |= 0x80;
            if (isalpha(w)) t |= 0x100;
        }
        rt_wr16(g_mem, out + i * 2, t);
    }
    HOST_RET(1, 5);
}

API_FN(LCMapStringA) {
    uint32_t flags = ARG(1), src = ARG(2), cs = ARG(3), dst = ARG(4), cd = ARG(5);
    int32_t n = (int32_t)cs;
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (!cd) { HOST_RET(n, 6); return; }
    if ((uint32_t)n > cd) { SET_ERR(122); HOST_RET(0, 6); return; }
    for (int32_t i = 0; i < n; i++) {
        uint8_t c = g_mem[src + i];
        if (flags & 0x100) c = (uint8_t)tolower(c);
        else if (flags & 0x200) c = (uint8_t)toupper(c);
        g_mem[dst + i] = c;
    }
    HOST_RET(n, 6);
}
API_FN(LCMapStringW) {
    uint32_t flags = ARG(1), src = ARG(2), cs = ARG(3), dst = ARG(4), cd = ARG(5);
    int32_t n = (int32_t)cs;
    if (n < 0) { n = 0; while (rt_rd16(g_mem, src + n * 2)) n++; n++; }
    if (!cd) { HOST_RET(n, 6); return; }
    if ((uint32_t)n > cd) { SET_ERR(122); HOST_RET(0, 6); return; }
    for (int32_t i = 0; i < n; i++) {
        uint16_t c = rt_rd16(g_mem, src + i * 2);
        if (c < 128) { if (flags & 0x100) c = (uint16_t)tolower(c); else if (flags & 0x200) c = (uint16_t)toupper(c); }
        rt_wr16(g_mem, dst + i * 2, c);
    }
    HOST_RET(n, 6);
}

void api_register_kernel32(void) {
    static const ApiEntry t[] = {
        API_ENTRY(HeapCreate), API_ENTRY(HeapDestroy), API_ENTRY(HeapAlloc), API_ENTRY(HeapReAlloc), API_ENTRY(HeapFree), API_ENTRY(HeapSize),
        API_ENTRY(VirtualAlloc), API_ENTRY(VirtualFree), API_ENTRY(ExitProcess), API_ENTRY(TerminateProcess), API_ENTRY(GetVersion),
        API_ENTRY(GetLastError), API_ENTRY(GetCurrentProcess), API_ENTRY(GetCurrentThreadId), API_ENTRY(GetTickCount), API_ENTRY(Sleep),
        API_ENTRY(GetACP), API_ENTRY(GetOEMCP), API_ENTRY(SetHandleCount), API_ENTRY(UnhandledExceptionFilter), API_ENTRY(RaiseException),
        API_ENTRY(RtlUnwind), API_ENTRY(GetSystemInfo), API_ENTRY(GetStartupInfoA), API_ENTRY(GetCommandLineA),
        API_ENTRY(GetEnvironmentStrings), API_ENTRY(GetEnvironmentStringsW), API_ENTRY(FreeEnvironmentStringsA), API_ENTRY(FreeEnvironmentStringsW),
        API_ENTRY(GetLocalTime), API_ENTRY(GetModuleHandleA), API_ENTRY(LoadLibraryA), API_ENTRY(LoadLibraryExA), API_ENTRY(FreeLibrary),
        API_ENTRY(GetModuleFileNameA), API_ENTRY(GetProcAddress), API_ENTRY(FindResourceA), API_ENTRY(LoadResource), API_ENTRY(LockResource),
        API_ENTRY(SizeofResource), API_ENTRY(GetStdHandle), API_ENTRY(SetStdHandle), API_ENTRY(CreateFileA), API_ENTRY(CloseHandle),
        API_ENTRY(ReadFile), API_ENTRY(WriteFile), API_ENTRY(SetFilePointer), API_ENTRY(SetEndOfFile), API_ENTRY(FlushFileBuffers),
        API_ENTRY(GetFileType), API_ENTRY(DeleteFileA), API_ENTRY(MoveFileA), API_ENTRY(GetCurrentDirectoryA), API_ENTRY(SetCurrentDirectoryA),
        API_ENTRY(GetDriveTypeA), API_ENTRY(GetVolumeInformationA), API_ENTRY(GetPrivateProfileStringA), API_ENTRY(WritePrivateProfileStringA),
        API_ENTRY(GetCPInfo), API_ENTRY(MultiByteToWideChar), API_ENTRY(WideCharToMultiByte), API_ENTRY(GetStringTypeW), API_ENTRY(GetStringTypeA),
        API_ENTRY(LCMapStringA), API_ENTRY(LCMapStringW),
    };
    api_register(t, (int)(sizeof t / sizeof t[0]));
}
