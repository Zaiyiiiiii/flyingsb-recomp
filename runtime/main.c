/* FlyingSB 静态重编译版 —— 启动入口 */
#include "host.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>   /* Android 上 main 要改名为 SDL_main，由 SDL 的 Java 层拉起 */
#endif

extern const char *g_cmdline_extra;
void host_log_open(const char *path);
extern int g_scale_nearest;

static void usage(void) {
    fprintf(stderr,
        "用法: flyingsb [选项]\n"
        "  --game <目录>    原版游戏目录（含 FlyingSB.exe 与全部资源，默认先找可执行文件旁的 game，再找当前目录的 ./game；也可用环境变量 FLYINGSB_GAME）\n"
        "  --save <目录>    存档/配置目录（默认用户数据目录）\n"
        "  --trace-api      打印来宾 API 调用\n"
        "  --skip-video     跳过 AVI 视频（开发调试用）\n"
        "  --cheat         以 /e2wantedcheatcode 启动（启用原版秘籍输入）\n");
}

int main(int argc, char **argv) {
    const char *game = getenv("FLYINGSB_GAME");
    const char *save = NULL;
    int game_given = game != NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) { game = argv[++i]; game_given = 1; }
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i];
        else if (!strcmp(argv[i], "--trace-api")) g_trace_api = 1;
        else if (!strcmp(argv[i], "--skip-video")) g_skip_video = 1;
        else if (!strcmp(argv[i], "--cheat")) g_cmdline_extra = "/e2wantedcheatcode";
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { fprintf(stderr, "未知参数: %s\n", argv[i]); usage(); return 2; }
    }

#ifdef __EMSCRIPTEN__
    /* PROXY_TO_PTHREAD 下 Module.arguments 不会传进 main，改读页面 URL 参数 ?trace ?skip-video ?cheat */
    int qf = MAIN_THREAD_EM_ASM_INT({
        const q = new URLSearchParams(location.search);
        return (q.has('trace') ? 1 : 0) | (q.has('skip-video') ? 2 : 0) | (q.has('cheat') ? 4 : 0);
    });
    fprintf(stderr, "[web] 页面参数标志=%d\n", qf);
    if (qf & 1) g_trace_api = 1;
    if (qf & 2) g_skip_video = 1;
    if (qf & 4) g_cmdline_extra = "/e2wantedcheatcode";
#endif
    if (!plat_init()) return 1;
    /* 没有指定游戏目录：先找可执行文件所在目录下的 game，再找当前工作目录下的 game */
    char gamebuf[1100];
    if (!game_given) {
        game = "game";
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
        const char *ed = plat_exe_dir();
        struct stat st;
        if (ed) {
            snprintf(gamebuf, sizeof gamebuf, "%sgame", ed);
            if (stat(gamebuf, &st) == 0 && (st.st_mode & S_IFDIR)) game = gamebuf;
        }
#endif
    }
    char savebuf[1024];
#ifdef __EMSCRIPTEN__
    if (!save) save = "/save";   /* 由页面脚本挂载 IndexedDB 持久化 */
#endif
    if (!save) { snprintf(savebuf, sizeof savebuf, "%s", plat_pref_dir()); save = savebuf; }
    vfs_init(game, save);
#ifdef __EMSCRIPTEN__
    if (web_manifest_load() <= 0) host_fatal("无法从服务器取得游戏文件清单（/api/manifest）。请确认用 Rust 服务器打开本页面。");
#elif defined(__ANDROID__)
    if (web_manifest_load() <= 0) host_fatal("没有找到游戏数据，请回到启动页重新选择游戏 ZIP 或目录。");
#endif
    char logpath[1200];
    snprintf(logpath, sizeof logpath, "%s/flyingsb.log", vfs_save_dir());
    host_log_open(logpath);

    char exe[1200];
    if (!vfs_resolve("FlyingSB.exe", 0, exe, sizeof exe) || !vfs_exists("FlyingSB.exe")) {
        host_fatal("在游戏目录 \"%s\" 中找不到 FlyingSB.exe。\n请用 --game 指向你自己拥有的原版游戏目录。", vfs_game_dir());
    }
#ifdef __EMSCRIPTEN__
    fprintf(stderr, "[web] 清单已载入，exe=%s\n", exe);
#endif
    if (!gm_init()) host_fatal("无法保留来宾地址空间");

    api_register_kernel32();
    api_register_user32();
    api_register_gdi32();
    api_register_ddraw();
    api_register_dsound();
    api_register_winmm();
    api_register_avi();
    api_register_misc();

    if (!loader_load(exe)) host_fatal("加载原版可执行文件失败: %s", exe);
    loader_run();
    return 0;
}
