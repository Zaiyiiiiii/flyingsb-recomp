/* 平台后端：SDL3（窗口、渲染、输入、手柄、音频）。 */
#include "platform.h"
#include "gamepad_map.h"
#include "touch_pad.h"
#include "host.h"
#include <SDL3/SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif
#include <stdio.h>
#include <string.h>

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static int g_tex_w, g_tex_h;
static int g_logical_w = 800, g_logical_h = 600;
static SDL_AudioStream *g_astream;
static PlatAudioFn g_afn;
static int g_afmt_channels = 2;
static int g_scale_linear = 1;
static int g_cursor_visible = 1;

/* 窗口坐标 -> 游戏逻辑坐标。有渲染器时用 SDL 的逻辑呈现换算；Web 没有渲染器，按窗口尺寸比例换算 */
static void win_to_logical(float wx, float wy, float *lx, float *ly) {
    if (g_ren) { SDL_RenderCoordinatesFromWindow(g_ren, wx, wy, lx, ly); return; }
    int ww = 0, wh = 0;
    if (g_win) SDL_GetWindowSize(g_win, &ww, &wh);
    *lx = ww > 0 ? wx * (float)g_logical_w / (float)ww : wx;
    *ly = wh > 0 ? wy * (float)g_logical_h / (float)wh : wy;
}

#ifdef __EMSCRIPTEN__
/* Web 键盘：由页面脚本（web/shell.html 的 Module.installInput）按 event.code 映射后经此环形队列投递，
 * 整页有效、不依赖 canvas 焦点、不受输入法影响。单生产者(主线程 JS)单消费者(本线程)。
 * 队列字: type<<24 | down<<16 | 值；type 0=按键(VK) 1=字符 2=松开所有键 3=窗口激活/失活 */
#define WEBKEY_N 256
static volatile uint32_t g_wk_ring[WEBKEY_N];
static volatile int g_wk_head, g_wk_tail;

static void web_input_drain(void) {
    int head = g_wk_head;
    while (head != __atomic_load_n(&g_wk_tail, __ATOMIC_ACQUIRE)) {
        uint32_t w = g_wk_ring[(uint32_t)head & (WEBKEY_N - 1)];
        head++;
        switch (w >> 24) {
        case 0: ev_key((int)(w & 0xFFFF), (int)((w >> 16) & 1)); break;
        case 1: ev_char(w & 0xFFFF); break;
        case 2: ev_release_keys(); break;
        case 3: ev_activate((int)((w >> 16) & 1)); break;
        default: break;
        }
        __atomic_store_n(&g_wk_head, head, __ATOMIC_RELEASE);
    }
}
#endif

#ifdef __EMSCRIPTEN__
extern int g_video_playing;   /* 页面脚本读它：播放片头视频时手柄的常用按钮都当作 Esc */
#endif

int plat_init(void) {
#ifdef __EMSCRIPTEN__
    MAIN_THREAD_ASYNC_EM_ASM({ Module.installInput($0, $1, $2, $3); }, g_wk_ring, &g_wk_head, &g_wk_tail, &g_video_playing);
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");   /* 触摸只用于虚拟按键，不再合成鼠标事件 */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");   /* 返回键只交给游戏（映射为 Esc），不触发系统的"返回/退到后台"及其动画 */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        return 0;
    }
    gp_init();
    return 1;
}

void plat_shutdown(void) {
    plat_audio_close();
    gp_shutdown();
    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; }
    if (g_ren) { SDL_DestroyRenderer(g_ren); g_ren = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
    SDL_Quit();
}

void plat_error_box(const char *title, const char *msg) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, g_win);
}

int plat_message_box(const char *title, const char *msg, int type) {
#ifdef __EMSCRIPTEN__
    /* 浏览器里没有原生消息框：在主线程用 confirm/alert */
    (void)title;
    if (type == 4 || type == 1) {
        int ok = MAIN_THREAD_EM_ASM_INT({ return confirm(UTF8ToString($0)) ? 1 : 0; }, msg);
        return type == 4 ? (ok ? 6 : 7) : (ok ? 1 : 2);
    }
    MAIN_THREAD_EM_ASM({ alert(UTF8ToString($0)); }, msg);
    return 1;
#endif
    SDL_MessageBoxButtonData btns[2];
    int n = 0;
    int ids[2];
    if (type == 4) {          /* MB_YESNO */
        btns[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT; btns[0].buttonID = 6; btns[0].text = "是 / Yes";
        btns[1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT; btns[1].buttonID = 7; btns[1].text = "否 / No";
        n = 2; ids[0] = 6; ids[1] = 7;
    } else if (type == 1) {   /* MB_OKCANCEL */
        btns[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT; btns[0].buttonID = 1; btns[0].text = "OK";
        btns[1].flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT; btns[1].buttonID = 2; btns[1].text = "Cancel";
        n = 2; ids[0] = 1; ids[1] = 2;
    } else {
        btns[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT; btns[0].buttonID = 1; btns[0].text = "OK";
        n = 1; ids[0] = 1;
    }
    SDL_MessageBoxData d;
    memset(&d, 0, sizeof d);
    d.flags = SDL_MESSAGEBOX_INFORMATION;
    d.window = g_win;
    d.title = title;
    d.message = msg;
    d.numbuttons = n;
    d.buttons = btns;
    int pressed = ids[0];
    if (!SDL_ShowMessageBox(&d, &pressed)) return ids[n - 1];
    return pressed;
}

int plat_create_window(int w, int h, const char *title) {
    if (g_win) {
        SDL_SetWindowTitle(g_win, title);
        return 1;
    }
    g_logical_w = w;
    g_logical_h = h;
#ifdef __EMSCRIPTEN__
    /* Web：游戏跑在永不让出的 pthread 里，OffscreenCanvas 的帧不会被提交，所以不建 SDL 渲染器；
     * SDL 窗口只负责输入事件，画面由 plat_present 交给主线程用 2D canvas 显示 */
    g_win = SDL_CreateWindow(title, w, h, 0);
    if (!g_win) { fprintf(stderr, "SDL_CreateWindow 失败: %s\n", SDL_GetError()); return 0; }
    /* 不启用 SDL 的文本输入：它会引入输入法相关的元素，键盘改由页面脚本处理 */
    return 1;
#endif
    SDL_WindowFlags wf = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
    wf |= SDL_WINDOW_FULLSCREEN;   /* 手机上始终全屏（隐藏状态栏和导航栏） */
#endif
    g_win = SDL_CreateWindow(title, w, h, wf);
    if (!g_win) { fprintf(stderr, "SDL_CreateWindow 失败: %s\n", SDL_GetError()); return 0; }
    g_ren = SDL_CreateRenderer(g_win, NULL);
    if (!g_ren) { fprintf(stderr, "SDL_CreateRenderer 失败: %s\n", SDL_GetError()); return 0; }
    SDL_SetRenderLogicalPresentation(g_ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    /* 游戏按已过去的毫秒数决定每次推进几个逻辑帧（上限 3），渲染线程不能被屏幕刷新卡住：
     * 屏幕常是 90/120/144Hz 或会变频，等垂直同步会让帧间隔忽长忽短，表现为动画跳帧、忽快忽慢。
     * 所有平台都不等同步，由游戏自己按 16ms 限帧（窗口由系统合成，不会撕裂） */
    SDL_SetRenderVSync(g_ren, 0);
#ifndef __ANDROID__
    SDL_StartTextInput(g_win);   /* 手机上不开：会弹出软键盘/输入法 */
#endif
    return 1;
}

void plat_set_title(const char *title) { if (g_win) SDL_SetWindowTitle(g_win, title); }

void plat_set_fullscreen(int on) {
#ifdef __EMSCRIPTEN__
    /* 全屏整个舞台容器（保持 4:3 居中），而不是只拉伸 canvas */
    MAIN_THREAD_ASYNC_EM_ASM({
        const st = document.getElementById('stage');
        if ($0) { if (!document.fullscreenElement) st.requestFullscreen().catch(() => {}); }
        else if (document.fullscreenElement) document.exitFullscreen();
    }, on != 0);
    return;
#endif
    if (g_win) SDL_SetWindowFullscreen(g_win, on != 0);
}

#ifdef __EMSCRIPTEN__
/* 游戏线程把最新一帧拷进共享缓冲，主线程在 requestAnimationFrame 里画它：
 * 画面跟显示器刷新对齐，游戏线程也不会因主线程忙而丢帧或被卡住 */
static uint32_t *g_web_buf;
static volatile int g_web_w, g_web_h, g_web_seq;   /* g_web_seq: 每提交一帧加 1，主线程只画序号变了的帧 */
static int g_web_started;
#endif

void plat_present(const uint32_t *pixels, int w, int h, int pitch) {
#ifdef __EMSCRIPTEN__
    (void)pitch;
    g_logical_w = w; g_logical_h = h;
    if (g_web_w != w || g_web_h != h || !g_web_buf) {
        /* 分辨率变化只发生在启动阶段 */
        g_web_buf = (uint32_t *)realloc(g_web_buf, (size_t)w * (size_t)h * 4);
        g_web_w = w; g_web_h = h;
    }
    memcpy(g_web_buf, pixels, (size_t)w * (size_t)h * 4);
    __atomic_add_fetch(&g_web_seq, 1, __ATOMIC_SEQ_CST);
    if (!g_web_started) {
        g_web_started = 1;
        MAIN_THREAD_ASYNC_EM_ASM({
            /* 注意：宏参数里不能有顶层逗号，所以这里每个声明单独成句 */
            const pBuf = $0 >> 2;
            const pW = $1 >> 2;
            const pH = $2 >> 2;
            const pSeq = $3 >> 2;
            const c = document.getElementById('canvas');
            const ctx = c.getContext('2d', { alpha: false });
            let img = null;
            let last = -1;
            const loop = () => {
                requestAnimationFrame(loop);
                const seq = Atomics.load(HEAP32, pSeq);
                if (seq === last) return;                                       /* 没有新帧 */
                last = seq;
                const w = Atomics.load(HEAP32, pW);
                const h = Atomics.load(HEAP32, pH);
                const ptr = Atomics.load(HEAPU32, pBuf);
                if (!ptr || w <= 0 || h <= 0) return;
                if (c.width !== w || c.height !== h) {
                    c.width = w; c.height = h;
                    document.getElementById('stage').style.setProperty('--arn', String(w / h));   /* 舞台按游戏画面的比例排版 */
                }
                if (!img || img.width !== w || img.height !== h) img = new ImageData(w, h);
                const s = new Uint32Array(HEAPU8.buffer, ptr, w * h);          /* 共享内存只读视图: 0x00RRGGBB */
                const d = new Uint32Array(img.data.buffer);                    /* 目标按字节 RGBA */
                for (let i = 0; i < s.length; i++) { const v = s[i]; d[i] = 0xFF000000 | ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >>> 16) & 0xFF); }
                ctx.putImageData(img, 0, 0);
            };
            requestAnimationFrame(loop);
        }, &g_web_buf, &g_web_w, &g_web_h, &g_web_seq);
    }
    return;
#endif
    if (!g_ren) return;
    if (!g_tex || g_tex_w != w || g_tex_h != h) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(g_tex, g_scale_linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        g_tex_w = w; g_tex_h = h;
        if (w != g_logical_w || h != g_logical_h) {
            g_logical_w = w; g_logical_h = h;
            SDL_SetRenderLogicalPresentation(g_ren, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        }
    }
    SDL_UpdateTexture(g_tex, NULL, pixels, pitch);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, NULL);
    tp_draw(g_ren);   /* 仅 Android：没有手柄时叠加虚拟按键 */
    SDL_RenderPresent(g_ren);
}

static void toggle_fullscreen(void) {
#ifdef __EMSCRIPTEN__
    int on = MAIN_THREAD_EM_ASM_INT({ return document.fullscreenElement ? 1 : 0; });
    plat_set_fullscreen(!on);
#else
    SDL_WindowFlags f = SDL_GetWindowFlags(g_win);
    plat_set_fullscreen(!(f & SDL_WINDOW_FULLSCREEN));
#endif
}

void plat_toggle_fullscreen(void) { toggle_fullscreen(); }

void plat_show_cursor(int show) {
    g_cursor_visible = show;
    if (show) SDL_ShowCursor(); else SDL_HideCursor();
}

void plat_set_cursor_pos(int x, int y) {
    if (!g_win || !g_ren) return;
    float wx, wy;
    SDL_RenderCoordinatesToWindow(g_ren, (float)x, (float)y, &wx, &wy);
    SDL_WarpMouseInWindow(g_win, wx, wy);
}

void plat_get_cursor_pos(int *x, int *y) {
    float wx, wy, lx = 0, ly = 0;
    SDL_GetMouseState(&wx, &wy);
    win_to_logical(wx, wy, &lx, &ly);
    *x = (int)lx; *y = (int)ly;
}

/* ---------------------------------------------------------------- 按键映射 SDL -> Win32 VK */
static int sdl_to_vk(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return 0x61 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_AC_BACK: return 0x1B;   /* Android 系统返回键 = Esc（取消/菜单/跳过片头） */
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    default: return 0;
    }
}

void plat_pump_events(void) {
#ifdef __EMSCRIPTEN__
    /* 游戏线程是不让出的阻塞循环：浏览器的键盘/鼠标/手柄回调被代理到本线程排队，
     * 必须主动处理，否则 SDL 永远收不到事件 */
    emscripten_current_thread_process_queued_calls();
    web_input_drain();
#endif
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
#ifdef __EMSCRIPTEN__
        /* 键盘/文本/焦点已由页面脚本处理；忽略 SDL 自己的，避免重复，也避开它依赖 canvas 焦点和输入法的问题 */
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_TEXT_INPUT ||
            e.type == SDL_EVENT_WINDOW_FOCUS_GAINED || e.type == SDL_EVENT_WINDOW_FOCUS_LOST) continue;
#endif
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            ev_close();
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int down = e.key.down;
            if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                toggle_fullscreen();
                break;
            }
            if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F11) {
                toggle_fullscreen();
                break;
            }
            int vk = sdl_to_vk(e.key.scancode);
#ifdef __ANDROID__
            if (down && !e.key.repeat) host_log("[键] 扫描码=%d 键码=%u -> VK=0x%02X", (int)e.key.scancode, (unsigned)e.key.key, vk);
#endif
            if (vk) ev_key(vk, down);
            break;
        }
        case SDL_EVENT_TEXT_INPUT: {
            const unsigned char *s = (const unsigned char *)e.text.text;
            for (; *s; s++) if (*s < 0x80) ev_char(*s);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            float lx, ly;
            win_to_logical(e.motion.x, e.motion.y, &lx, &ly);
            ev_mouse_move((int)lx, (int)ly);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            float lx, ly;
            win_to_logical(e.button.x, e.button.y, &lx, &ly);
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            ev_mouse_button(b, e.button.down, (int)lx, (int)ly);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            ev_mouse_wheel((int)(e.wheel.y * 120));
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            tp_event(&e);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: ev_activate(1); break;
        case SDL_EVENT_WINDOW_FOCUS_LOST: ev_release_keys(); ev_activate(0); break;
        default:
            gp_handle_event(&e);
            break;
        }
    }
    gp_update();
}

uint64_t plat_ticks_ms(void) { return SDL_GetTicks(); }
void plat_sleep_ms(uint32_t ms) {
    /* 短睡眠用精确延时：Windows 默认计时精度约 15ms，普通 Sleep(1) 会睡过头，帧间隔就会抖 */
    if (ms <= 4) SDL_DelayPrecise((Uint64)ms * 1000000u); else SDL_Delay(ms);
}

/* ---------------------------------------------------------------- 音频 */
static SDL_Mutex *g_amutex;

static void SDLCALL audio_cb(void *ud, SDL_AudioStream *stream, int additional, int total) {
    (void)ud; (void)total;
    int16_t buf[2048 * 2];
    while (additional > 0) {
        int frames = additional / (int)(sizeof(int16_t) * g_afmt_channels);
        if (frames > 2048) frames = 2048;
        if (frames <= 0) break;
        SDL_LockMutex(g_amutex);
        if (g_afn) g_afn(buf, frames); else memset(buf, 0, (size_t)frames * 4);
        SDL_UnlockMutex(g_amutex);
        SDL_PutAudioStreamData(stream, buf, frames * (int)sizeof(int16_t) * g_afmt_channels);
        additional -= frames * (int)sizeof(int16_t) * g_afmt_channels;
    }
}

int plat_audio_open(int rate, int channels, PlatAudioFn cb) {
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16;
    spec.channels = channels;
    spec.freq = rate;
    g_afmt_channels = channels;
    g_afn = cb;
    if (!g_amutex) g_amutex = SDL_CreateMutex();
    g_astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!g_astream) { fprintf(stderr, "音频设备打开失败: %s\n", SDL_GetError()); return 0; }
    SDL_ResumeAudioStreamDevice(g_astream);
    return 1;
}

void plat_audio_close(void) {
    if (g_astream) { SDL_DestroyAudioStream(g_astream); g_astream = NULL; }
}

void plat_audio_lock(void) { if (g_amutex) SDL_LockMutex(g_amutex); }
void plat_audio_unlock(void) { if (g_amutex) SDL_UnlockMutex(g_amutex); }

float plat_display_aspect(void) {
#ifdef __EMSCRIPTEN__
    double a = MAIN_THREAD_EM_ASM_DOUBLE({ return window.innerHeight > 0 ? window.innerWidth / window.innerHeight : 0; });
    return (float)a;
#else
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!m || m->w <= 0 || m->h <= 0) return 0.0f;
    return m->w >= m->h ? (float)m->w / (float)m->h : (float)m->h / (float)m->w;   /* 游戏横屏，不管系统报告的方向 */
#endif
}

const char *plat_base_dir(void) {
    const char *p = SDL_GetBasePath();
    return p ? p : "./";
}

const char *plat_exe_dir(void) { return SDL_GetBasePath(); }

const char *plat_pref_dir(void) {
#ifdef __ANDROID__
    /* 应用的外部私有目录（Android/data/<包名>/files/）：文件管理器和电脑都能看到，存档、配置放在这里，游戏数据在其下的 game/ */
    static char ext[1100];
    const char *e = SDL_GetAndroidExternalStoragePath();
    if (e) { snprintf(ext, sizeof ext, "%s/", e); return ext; }
#endif
    const char *p = SDL_GetPrefPath("flyingsb-recomp", "FlyingSB");
    return p ? p : ".";
}
