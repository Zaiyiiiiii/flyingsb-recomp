/* 宿主服务：来宾内存、虚拟文件系统、API 注册、日志。与具体平台（SDL 等）无关。 */
#pragma once
#include "rt.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 游戏配置（hooks.c 的"遊戲設定"用） ---- */
int host_ini_get(const char *sec, const char *key, char *out, size_t n);   /* 读 FlyingSB.INI，没有该键返回 0 */
void host_ini_set(const char *sec, const char *key, const char *val);   /* 写游戏的 FlyingSB.INI（落到存档目录） */

/* ---- 日志与致命错误 ---- */
void host_log(const char *fmt, ...);
RT_NORETURN void host_fatal(const char *fmt, ...);
RT_NORETURN void host_exit(int code);
extern int g_trace_api;      /* 打印每次 API 调用 */
extern int g_res_w, g_res_h;   /* --resolution 覆盖游戏读到的屏幕宽高，0 表示不覆盖 */
extern int g_skip_video;     /* 开发辅助：AVI 打开一律失败，跳过片头视频 */
extern int g_video_playing;  /* 当前打开着的 AVI 文件数；>0 时手柄的常用按钮都当作 Esc 以跳过视频 */

/* ---- 来宾内存 ---- */
int gm_init(void);                                   /* 保留地址空间并映射低端区域 */
uint32_t gm_alloc(uint32_t size);                    /* 16 字节对齐，不清零 */
uint32_t gm_calloc(uint32_t size);
void gm_free(uint32_t addr);
uint32_t gm_realloc(uint32_t addr, uint32_t size);
uint32_t gm_size(uint32_t addr);
uint32_t gm_valloc(uint32_t size);                   /* 页对齐、清零，用于 VirtualAlloc */
int gm_vfree(uint32_t addr);
uint32_t gm_vsize(uint32_t addr);
#define GP(a) ((void *)(g_mem + (a)))                /* 来宾地址 -> 宿主指针 */
#define GSTR(a) ((const char *)(g_mem + (a)))
uint32_t gm_strdup(const char *s);                   /* 在来宾内存里复制一份 C 字符串 */

/* ---- 虚拟文件系统：游戏目录只读，写入落到用户数据目录 ---- */
void vfs_init(const char *game_dir, const char *save_dir);
const char *vfs_game_dir(void);
const char *vfs_save_dir(void);
/* 把来宾传来的路径（可能含反斜杠/大小写不一致）解析为宿主路径；writing 为真时解析到存档目录 */
int vfs_resolve(const char *guest_path, int writing, char *out, size_t outsz);
FILE *vfs_fopen(const char *guest_path, const char *mode);
int vfs_exists(const char *guest_path);
/* 读取 vfs_resolve 得到的主机路径。Web 版里游戏目录的路径带 WEB_GAME_PFX 前缀，经 HTTP 向服务器读取 */
FILE *host_fopen(const char *host_path, const char *mode);
#define WEB_GAME_PFX "@game/"
/* 游戏目录不在本地磁盘上的平台：Web 经 HTTP 向服务器读，Android 从 APK 的 assets 读。两者共用清单 + FILE* 包装 */
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
#define HOST_REMOTE_GAME 1
#endif
int web_manifest_load(void);
int web_manifest_find(const char *rel, char *out, size_t outsz);
FILE *web_remote_open(const char *actual_rel);

/* ---- API 注册 ---- */
typedef struct { const char *name; RtHostFn fn; } ApiEntry;
void api_register(const ApiEntry *tbl, int n);
RtHostFn api_lookup(const char *name);
#define API_FN(name) static void api_##name(void)
#define API_ENTRY(name) { #name, api_##name }
void api_register_kernel32(void);
void api_register_user32(void);
void api_register_gdi32(void);
void api_register_ddraw(void);
void api_register_dsound(void);
void api_register_winmm(void);
void api_register_avi(void);
void api_register_misc(void);

/* 来宾 stdcall 参数辅助 */
#define ARGP(i) GP(ARG(i))
#define ARGS(i) GSTR(ARG(i))

/* ---- PE 资源（DLL 容器） ---- */
typedef struct PeModule PeModule;
PeModule *pe_open(const char *host_path);
void pe_close(PeModule *m);
/* 以数字 id 或字符串名查找资源；返回数据在文件内偏移与大小 */
int pe_find_resource(PeModule *m, uint32_t type_id, const char *type_name, uint32_t name_id, const char *name_str, uint32_t *file_off, uint32_t *size);
int pe_read(PeModule *m, uint32_t off, uint32_t size, void *dst);
void pe_dump(PeModule *m);
const char *pe_path(PeModule *m);

void vfs_set_cwd(const char *rel);
const char *vfs_get_cwd(void);

#ifdef _WIN32
#include <string.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

/* ---- 加载器 ---- */
int loader_load(const char *exe_path);
void loader_run(void);
extern int g_exit_requested;

/* ---- SHA-256 ---- */
void sha256_hex(const uint8_t *data, size_t len, char out[65]);

#ifdef __cplusplus
}
#endif
