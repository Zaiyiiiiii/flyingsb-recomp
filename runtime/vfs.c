#include "host.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/types.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static char game_dir[1024];
static char save_dir[1024];

void vfs_init(const char *g, const char *s) {
    snprintf(game_dir, sizeof game_dir, "%s", g);
    snprintf(save_dir, sizeof save_dir, "%s", s);
    size_t n = strlen(game_dir);
    while (n > 1 && (game_dir[n - 1] == '/' || game_dir[n - 1] == '\\')) game_dir[--n] = 0;
    n = strlen(save_dir);
    while (n > 1 && (save_dir[n - 1] == '/' || save_dir[n - 1] == '\\')) save_dir[--n] = 0;
    MKDIR(save_dir);
}

const char *vfs_game_dir(void) { return game_dir; }
const char *vfs_save_dir(void) { return save_dir; }

#ifdef _WIN32
/* Windows 文件系统大小写不敏感，直接拼接 */
static int match_path(const char *root, const char *rel, char *out, size_t outsz, int create_dirs) {
    snprintf(out, outsz, "%s/%s", root, rel);
    if (create_dirs) {
        char tmp[2048];
        snprintf(tmp, sizeof tmp, "%s", out);
        for (char *p = tmp + strlen(root) + 1; *p; p++) {
            if (*p == '/') { *p = 0; MKDIR(tmp); *p = '/'; }
        }
    }
    struct stat st;
    return stat(out, &st) == 0;
}
#else
#ifdef HOST_REMOTE_GAME
/* Web / Android：游戏目录不在本地磁盘，按清单不区分大小写匹配 */
static int match_game(const char *rel, char *out, size_t outsz) {
    char actual[1024];
    if (web_manifest_find(rel, actual, sizeof actual)) { snprintf(out, outsz, WEB_GAME_PFX "%s", actual); return 1; }
    snprintf(out, outsz, WEB_GAME_PFX "%s", rel);
    return 0;
}
#endif
static int match_path(const char *root, const char *rel, char *out, size_t outsz, int create_dirs) {
    char cur[2048];
    snprintf(cur, sizeof cur, "%s", root);
    char comp[512];
    const char *p = rel;
    int exists = 1;
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != '/') n++;
        if (n == 0) { p++; continue; }
        memcpy(comp, p, n); comp[n] = 0;
        p += n;
        int last = (*p == 0);
        char found[512] = {0};
        int ok = 0;
        if (exists) {
            DIR *d = opendir(cur);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (strcasecmp(e->d_name, comp) == 0) { snprintf(found, sizeof found, "%s", e->d_name); ok = 1; if (strcmp(e->d_name, comp) == 0) break; }
                }
                closedir(d);
            }
        }
        if (!ok) { exists = 0; snprintf(found, sizeof found, "%s", comp); }
        size_t l = strlen(cur);
        snprintf(cur + l, sizeof cur - l, "/%s", found);
        if (!last && create_dirs && !exists) MKDIR(cur);
    }
    snprintf(out, outsz, "%s", cur);
    return exists;
}
#endif

/* 虚拟根 "C:\FlyingSB\"；cwd 为相对游戏目录的当前目录（以 / 分隔，不含首尾斜杠） */
#define VROOT "c:/flyingsb"
static char vcwd[512];

void vfs_set_cwd(const char *rel) { snprintf(vcwd, sizeof vcwd, "%s", rel); }
const char *vfs_get_cwd(void) { return vcwd; }

static void normalize(const char *in, char *out, size_t outsz) {
    char tmp[2048];
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < sizeof tmp; i++) tmp[j++] = (in[i] == '\\') ? '/' : in[i];
    tmp[j] = 0;
    const char *p = tmp;
    char rel[2048];
    size_t rl = strlen(VROOT);
    if (strncasecmp(p, VROOT, rl) == 0 && (p[rl] == '/' || p[rl] == 0)) {
        p += rl;
        while (*p == '/') p++;
        snprintf(rel, sizeof rel, "%s", p);
    } else if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        p += 2;
        while (*p == '/') p++;
        snprintf(rel, sizeof rel, "%s", p);
    } else if (p[0] == '/') {
        while (*p == '/') p++;
        snprintf(rel, sizeof rel, "%s", p);
    } else if (vcwd[0]) {
        snprintf(rel, sizeof rel, "%s/%s", vcwd, p);
    } else {
        snprintf(rel, sizeof rel, "%s", p);
    }
    /* 处理 . 和 .. */
    char *parts[64];
    int np = 0;
    char *save = NULL;
    (void)save;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", rel);
    for (char *t = strtok(buf, "/"); t && np < 64; t = strtok(NULL, "/")) {
        if (strcmp(t, ".") == 0 || !*t) continue;
        if (strcmp(t, "..") == 0) { if (np) np--; continue; }
        parts[np++] = t;
    }
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < np; i++) {
        o += snprintf(out + o, outsz - o, "%s%s", i ? "/" : "", parts[i]);
    }
}

int vfs_resolve(const char *guest_path, int writing, char *out, size_t outsz) {
    char rel[2048];
    normalize(guest_path, rel, sizeof rel);
    char p1[2048], p2[2048];
    int e1 = match_path(save_dir, rel, p1, sizeof p1, writing);
    if (writing) { snprintf(out, outsz, "%s", p1); return 1; }
    if (e1) { snprintf(out, outsz, "%s", p1); return 1; }
#ifdef HOST_REMOTE_GAME
    int e2 = match_game(rel, p2, sizeof p2);
#else
    int e2 = match_path(game_dir, rel, p2, sizeof p2, 0);
#endif
    snprintf(out, outsz, "%s", p2);
    return e2;
}

FILE *host_fopen(const char *host_path, const char *mode) {
#ifdef HOST_REMOTE_GAME
    if (!strncmp(host_path, WEB_GAME_PFX, strlen(WEB_GAME_PFX))) return web_remote_open(host_path + strlen(WEB_GAME_PFX));
#endif
    return fopen(host_path, mode);
}

FILE *vfs_fopen(const char *guest_path, const char *mode) {
    int writing = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    char path[2048];
    if (writing && strchr(mode, '+') && !strchr(mode, 'w')) {
        /* 读写打开已有文件：若只存在于游戏目录，先复制到存档目录 */
        char src[2048], dst[2048];
        vfs_resolve(guest_path, 0, src, sizeof src);
        vfs_resolve(guest_path, 1, dst, sizeof dst);
        if (strcmp(src, dst) != 0) {
            FILE *a = host_fopen(src, "rb");
            if (a) {
                FILE *b = fopen(dst, "wb");
                if (b) {
                    char buf[65536]; size_t n;
                    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
                    fclose(b);
                }
                fclose(a);
            }
        }
        snprintf(path, sizeof path, "%s", dst);
    } else {
        vfs_resolve(guest_path, writing, path, sizeof path);
    }
    return host_fopen(path, mode);
}

int vfs_exists(const char *guest_path) {
    char p[2048];
    if (!vfs_resolve(guest_path, 0, p, sizeof p)) return 0;
#ifdef HOST_REMOTE_GAME
    if (!strncmp(p, WEB_GAME_PFX, strlen(WEB_GAME_PFX))) return 1;   /* vfs_resolve 已按清单确认存在 */
#endif
    struct stat st;
    return stat(p, &st) == 0;
}
