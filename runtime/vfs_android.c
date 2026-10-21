/* Android 版游戏目录后端：游戏数据由启动页（LauncherActivity）导入到应用外部私有目录（Android/data/<包名>/files/game，文件管理器可见，也可以自己直接放进去）。
 * - 启动时递归扫描该目录得到清单，用于不区分大小写的路径匹配（和 Web 版同一套接口）；
 * - 读文件直接 fopen；
 * - 虚拟名 "@font" 对应包内 assets/fonts/simsun.ttc（宋体，随包带上）。 */
#define _GNU_SOURCE   /* funopen；必须先于任何系统头文件 */
#ifdef __ANDROID__
#include <stdio.h>
#include "host.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <SDL3/SDL.h>

static AAssetManager *asset_mgr(void) {
    static AAssetManager *m;
    if (m) return m;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!env || !act) return NULL;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID mid = (*env)->GetMethodID(env, cls, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject am = (*env)->CallObjectMethod(env, act, mid);
    m = AAssetManager_fromJava(env, (*env)->NewGlobalRef(env, am));
    (*env)->DeleteLocalRef(env, am);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return m;
}

typedef struct { char *path; char *lower; } MEntry;
static MEntry *man;
static int man_n, man_cap;
static char game_root[1024];

static void scan(const char *rel) {
    char dir[2048];
    snprintf(dir, sizeof dir, "%s/%s", game_root, rel);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[2048], full[2048];
        snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", game_root, sub);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { scan(sub); continue; }
        if (man_n == man_cap) { man_cap = man_cap ? man_cap * 2 : 1024; man = (MEntry *)realloc(man, man_cap * sizeof(MEntry)); }
        MEntry *m = &man[man_n++];
        m->path = strdup(sub);
        m->lower = strdup(sub);
        for (char *q = m->lower; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
    }
    closedir(d);
}

int web_manifest_load(void) {
    const char *base = SDL_GetAndroidExternalStoragePath();
    if (!base) return 0;
    snprintf(game_root, sizeof game_root, "%s/game", base);
    scan("");
    return man_n;
}

/* 不区分大小写查找；out 得到磁盘上的真实相对路径 */
int web_manifest_find(const char *rel, char *out, size_t outsz) {
    char low[1024];
    size_t i = 0;
    for (; rel[i] && i + 1 < sizeof low; i++) low[i] = (rel[i] >= 'A' && rel[i] <= 'Z') ? rel[i] + 32 : rel[i];
    low[i] = 0;
    for (int k = 0; k < man_n; k++)
        if (strcmp(man[k].lower, low) == 0) { snprintf(out, outsz, "%s", man[k].path); return 1; }
    return 0;
}

static int as_read(void *c, char *buf, int n) { return AAsset_read((AAsset *)c, buf, (size_t)n); }
static fpos_t as_seek(void *c, fpos_t off, int whence) { return (fpos_t)AAsset_seek64((AAsset *)c, (off64_t)off, whence); }
static int as_close(void *c) { AAsset_close((AAsset *)c); return 0; }

FILE *web_remote_open(const char *actual_rel) {
    if (!strcmp(actual_rel, "@font")) {
        AAssetManager *mgr = asset_mgr();
        if (!mgr) return NULL;
        AAsset *a = AAssetManager_open(mgr, "fonts/simsun.ttc", AASSET_MODE_RANDOM);
        if (!a) return NULL;
        FILE *f = funopen(a, as_read, NULL, as_seek, as_close);
        if (!f) AAsset_close(a);
        return f;
    }
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", game_root, actual_rel);
    return fopen(full, "rb");
}
#endif
