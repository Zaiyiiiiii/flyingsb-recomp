/* USER32：窗口、消息队列、输入、杂项。DC/文字相关在 gdi.c。 */
#include "host.h"
#include "platform.h"
#include "codepage.h"
#include <stdlib.h>
#include <string.h>

extern int guest_vsprintf(char *out, size_t outsz, const char *fmt, uint32_t argptr);
extern void ddraw_present_if_dirty(void);
extern int ddraw_display_width(void);
extern int ddraw_display_height(void);
extern int ddraw_window_created(void);

#define WIN_BASE 0x00010000u
#define MAX_WINS 64
#define MAX_CLASSES 32
#define MAX_MSGS 1024

enum { WM_NULL = 0, WM_CREATE = 1, WM_DESTROY = 2, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8,
       WM_PAINT = 0x0F, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_ACTIVATEAPP = 0x1C, WM_SETCURSOR = 0x20,
       WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102,
       WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
       WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208, WM_MOUSEWHEEL = 0x20A,
       WM_INITDIALOG = 0x110 };

static void big5_to_utf8(const char *in, char *out, size_t outsz);
typedef struct { char name[64]; uint32_t proc; uint32_t style; } Class;
typedef struct {
    int used;
    uint32_t proc;
    uint32_t style, exstyle;
    int x, y, w, h;
    uint32_t parent;
    uint32_t userdata;
    uint32_t extra[8];
    char title[128];
    int visible;
    char cls[64];
} Win;
typedef struct { uint32_t hwnd, msg, wp, lp, time; int x, y; } Msg;

static Class classes[MAX_CLASSES];
static int nclasses;
static Win wins[MAX_WINS];
static Msg queue[MAX_MSGS];
static int qhead, qtail;
static uint8_t keystate[256];
static uint8_t keytoggle[256];
static int cur_x, cur_y;
static int quit_posted;
static int cursor_count;
static uint32_t main_hwnd;
static int app_active = 1;

static struct { int used; int id; uint32_t proc; } hooks[8];

static Win *win_get(uint32_t h) {
    if (h < WIN_BASE || h >= WIN_BASE + MAX_WINS) return NULL;
    Win *w = &wins[h - WIN_BASE];
    return w->used ? w : NULL;
}

static void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    int next = (qtail + 1) % MAX_MSGS;
    if (next == qhead) return;   /* 队列满，丢弃 */
    Msg *m = &queue[qtail];
    m->hwnd = hwnd; m->msg = msg; m->wp = wp; m->lp = lp; m->time = (uint32_t)plat_ticks_ms(); m->x = cur_x; m->y = cur_y;
    qtail = next;
}

void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { post(hwnd, msg, wp, lp); }
extern void audio_poll(void);

/* ---------------------------------------------------------------- 平台事件入口 */
/* VK -> PC/AT 扫描码（lParam 位 16-23），ext 为扩展键标志（位 24）。游戏的键盘钩子靠它填按键状态表 */
static uint32_t vk_scan_lparam(int vk) {
    static const char row1[] = "QWERTYUIOP", row2[] = "ASDFGHJKL", row3[] = "ZXCVBNM";
    uint32_t sc = 0, ext = 0;
    if (vk >= 'A' && vk <= 'Z') {
        const char *p;
        if ((p = strchr(row1, vk))) sc = 0x10 + (uint32_t)(p - row1);
        else if ((p = strchr(row2, vk))) sc = 0x1E + (uint32_t)(p - row2);
        else if ((p = strchr(row3, vk))) sc = 0x2C + (uint32_t)(p - row3);
    } else if (vk >= '1' && vk <= '9') sc = 0x02 + (uint32_t)(vk - '1');
    else if (vk == '0') sc = 0x0B;
    else if (vk >= 0x70 && vk <= 0x79) sc = 0x3B + (uint32_t)(vk - 0x70);   /* F1..F10 */
    else switch (vk) {
    case 0x1B: sc = 0x01; break;
    case 0x08: sc = 0x0E; break;
    case 0x09: sc = 0x0F; break;
    case 0x0D: sc = 0x1C; break;
    case 0x10: sc = 0x2A; break;
    case 0x11: sc = 0x1D; break;
    case 0x12: sc = 0x38; break;
    case 0x20: sc = 0x39; break;
    case 0xBF: sc = 0x35; break;                       /* / */
    case 0x21: sc = 0x49; ext = 1; break;              /* PgUp */
    case 0x22: sc = 0x51; ext = 1; break;              /* PgDn */
    case 0x23: sc = 0x4F; ext = 1; break;              /* End */
    case 0x24: sc = 0x47; ext = 1; break;              /* Home */
    case 0x25: sc = 0x4B; ext = 1; break;
    case 0x26: sc = 0x48; ext = 1; break;
    case 0x27: sc = 0x4D; ext = 1; break;
    case 0x28: sc = 0x50; ext = 1; break;
    case 0x2D: sc = 0x52; ext = 1; break;
    case 0x2E: sc = 0x53; ext = 1; break;
    case 0x65: sc = 0x4C; break;                       /* 小键盘 5 */
    default: break;
    }
    return (sc << 16) | (ext << 24);
}

void ev_key(int vk, int down) {
    int wasdown = keystate[vk & 255];
    keystate[vk & 255] = down ? 0x80 : 0;
    if (down && !wasdown) keytoggle[vk & 255] ^= 1;
    if (!main_hwnd) return;
    int sys = (vk == 0x12) || (keystate[0x12] && vk != 0x12);
    uint32_t lp = 1 | vk_scan_lparam(vk) | (down ? 0 : 0xC0000000u) | (wasdown && down ? 0x40000000u : 0);
    post(main_hwnd, down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), (uint32_t)vk, lp);
}
void ev_char(uint32_t ch) { if (main_hwnd) post(main_hwnd, WM_CHAR, ch, 1); }
void ev_mouse_move(int x, int y) { cur_x = x; cur_y = y; if (main_hwnd) post(main_hwnd, WM_MOUSEMOVE, 0, (uint32_t)((y & 0xFFFF) << 16 | (x & 0xFFFF))); }
void ev_mouse_button(int b, int down, int x, int y) {
    cur_x = x; cur_y = y;
    if (!main_hwnd) return;
    static const uint32_t dn[3] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
    static const uint32_t up[3] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
    static const uint32_t mk[3] = {1, 2, 0x10};
    post(main_hwnd, down ? dn[b] : up[b], down ? mk[b] : 0, (uint32_t)((y & 0xFFFF) << 16 | (x & 0xFFFF)));
}
void ev_mouse_wheel(int delta) { if (main_hwnd) post(main_hwnd, WM_MOUSEWHEEL, (uint32_t)(delta << 16), (uint32_t)((cur_y & 0xFFFF) << 16 | (cur_x & 0xFFFF))); }
void ev_release_keys(void) {
    for (int vk = 1; vk < 256; vk++) if (keystate[vk]) ev_key(vk, 0);
}

void ev_activate(int active) {
    app_active = active;
    if (!main_hwnd) return;
    post(main_hwnd, WM_ACTIVATEAPP, (uint32_t)active, 1);
    post(main_hwnd, WM_ACTIVATE, active ? 1u : 0u, 0);
    post(main_hwnd, active ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
}
void ev_close(void) {
    if (main_hwnd) post(main_hwnd, WM_CLOSE, 0, 0);
    else { quit_posted = 1; }
}

/* ---------------------------------------------------------------- 窗口 */
static uint32_t call_proc(Win *w, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!w->proc) return 0;
    uint32_t args[4] = { hwnd, msg, wp, lp };
    return rt_guest_call(w->proc, 4, args);
}

API_FN(RegisterClassExA) {
    uint32_t p = ARG(0);
    if (nclasses >= MAX_CLASSES) { HOST_RET(0, 1); return; }
    Class *c = &classes[nclasses];
    c->style = rt_rd32(g_mem, p + 4);
    c->proc = rt_rd32(g_mem, p + 8);
    uint32_t nm = rt_rd32(g_mem, p + 40);
    snprintf(c->name, sizeof c->name, "%s", nm < 0x10000 ? "#atom" : GSTR(nm));
    nclasses++;
    HOST_RET(0xC000 + nclasses, 1);
}

API_FN(CreateWindowExA) {
    uint32_t exstyle = ARG(0), clsp = ARG(1), title = ARG(2), style = ARG(3);
    int x = (int32_t)ARG(4), y = (int32_t)ARG(5), w = (int32_t)ARG(6), h = (int32_t)ARG(7);
    uint32_t parent = ARG(8), param = ARG(11);
    const char *cls = clsp < 0x10000 ? "#atom" : GSTR(clsp);
    uint32_t proc = 0;
    for (int i = 0; i < nclasses; i++) if (!strcasecmp(classes[i].name, cls)) proc = classes[i].proc;
    int slot = -1;
    for (int i = 0; i < MAX_WINS; i++) if (!wins[i].used) { slot = i; break; }
    if (slot < 0) { HOST_RET(0, 12); return; }
    Win *win = &wins[slot];
    memset(win, 0, sizeof *win);
    win->used = 1; win->proc = proc; win->style = style; win->exstyle = exstyle;
    win->x = x == (int)0x80000000 ? 0 : x; win->y = y == (int)0x80000000 ? 0 : y;
    win->w = w == (int)0x80000000 ? 800 : w; win->h = h == (int)0x80000000 ? 600 : h;
    win->parent = parent;
    snprintf(win->cls, sizeof win->cls, "%s", cls);
    if (title) snprintf(win->title, sizeof win->title, "%s", GSTR(title));
    uint32_t hwnd = WIN_BASE + (uint32_t)slot;
    if (!parent && !main_hwnd) main_hwnd = hwnd;
    /* CREATESTRUCT */
    uint32_t cs = gm_calloc(64);
    rt_wr32(g_mem, cs, param);
    rt_wr32(g_mem, cs + 12, style); rt_wr32(g_mem, cs + 16, 0);
    rt_wr32(g_mem, cs + 20, (uint32_t)h); rt_wr32(g_mem, cs + 24, (uint32_t)w);
    rt_wr32(g_mem, cs + 28, (uint32_t)y); rt_wr32(g_mem, cs + 32, (uint32_t)x);
    rt_wr32(g_mem, cs + 36, title); rt_wr32(g_mem, cs + 40, clsp); rt_wr32(g_mem, cs + 44, exstyle);
    uint32_t r = call_proc(win, hwnd, WM_NCCREATE, 0, cs);
    if (r != 0 || !win->proc) {
        r = call_proc(win, hwnd, WM_CREATE, 0, cs);
        if (r == (uint32_t)-1) { win->used = 0; if (main_hwnd == hwnd) main_hwnd = 0; gm_free(cs); HOST_RET(0, 12); return; }
    }
    gm_free(cs);
    HOST_RET(hwnd, 12);
}

API_FN(DefWindowProcA) {
    uint32_t h = ARG(0), msg = ARG(1);
    uint32_t r = 0;
    switch (msg) {
    case WM_NCCREATE: r = 1; break;
    case WM_CLOSE: {
        Win *w = win_get(h);
        if (w) {
            uint32_t a[4] = { h, WM_DESTROY, 0, 0 };
            if (w->proc) rt_guest_call(w->proc, 4, a);
            if (h == main_hwnd) post(0, WM_QUIT, 0, 0);
            w->used = 0;
        }
        break;
    }
    case WM_SETCURSOR: r = 0; break;
    case WM_ERASEBKGND: r = 1; break;
    default: r = 0;
    }
    HOST_RET(r, 4);
}

API_FN(DestroyWindow) {
    uint32_t h = ARG(0);
    Win *w = win_get(h);
    if (w) {
        call_proc(w, h, WM_DESTROY, 0, 0);
        call_proc(w, h, WM_NCDESTROY, 0, 0);
        w->used = 0;
        if (h == main_hwnd) main_hwnd = 0;
    }
    HOST_RET(1, 1);
}

API_FN(ShowWindow) {
    uint32_t h = ARG(0), cmd = ARG(1);
    Win *w = win_get(h);
    int was = 0;
    if (w) {
        was = w->visible;
        w->visible = cmd != 0;
        if (w->visible && !was && h == main_hwnd) {
            char u8[512];
            big5_to_utf8(w->title, u8, sizeof u8);
            if (!ddraw_window_created()) plat_create_window(w->w > 0 ? w->w : 800, w->h > 0 ? w->h : 600, u8);
            else plat_set_title(u8);
            post(h, WM_ACTIVATEAPP, 1, 1);
            post(h, WM_ACTIVATE, 1, 0);
            post(h, WM_SETFOCUS, 0, 0);
            post(h, WM_PAINT, 0, 0);
        }
    }
    HOST_RET(was, 2);
}

API_FN(UpdateWindow) { HOST_RET(1, 1); }

API_FN(SetWindowPos) {
    Win *w = win_get(ARG(0));
    if (w) {
        uint32_t flags = ARG(6);
        if (!(flags & 2)) { w->x = (int32_t)ARG(2); w->y = (int32_t)ARG(3); }
        if (!(flags & 1)) { w->w = (int32_t)ARG(4); w->h = (int32_t)ARG(5); }
    }
    HOST_RET(1, 7);
}

API_FN(GetClientRect) {
    uint32_t rc = ARG(1);
    int w = ddraw_display_width(), h = ddraw_display_height();
    rt_wr32(g_mem, rc, 0); rt_wr32(g_mem, rc + 4, 0); rt_wr32(g_mem, rc + 8, (uint32_t)w); rt_wr32(g_mem, rc + 12, (uint32_t)h);
    HOST_RET(1, 2);
}
API_FN(ClientToScreen) { HOST_RET(1, 2); }
API_FN(AdjustWindowRectEx) { HOST_RET(1, 4); }
API_FN(GetMenu) { HOST_RET(0, 1); }
API_FN(GetDlgItem) { HOST_RET(0, 2); }

API_FN(GetWindowLongA) {
    Win *w = win_get(ARG(0));
    int idx = (int32_t)ARG(1);
    uint32_t r = 0;
    if (w) {
        if (idx == -16) r = w->style;
        else if (idx == -20) r = w->exstyle;
        else if (idx == -21) r = w->userdata;
        else if (idx == -4) r = w->proc;
        else if (idx >= 0 && idx < 32) r = w->extra[idx / 4];
    }
    HOST_RET(r, 2);
}

API_FN(GetWindowTextA) {
    Win *w = win_get(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2);
    size_t l = w ? strlen(w->title) : 0;
    if (l + 1 > n) l = n ? n - 1 : 0;
    if (w && n) { memcpy(g_mem + buf, w->title, l); g_mem[buf + l] = 0; }
    HOST_RET(l, 3);
}
API_FN(SetWindowTextA) {
    Win *w = win_get(ARG(0));
    if (w) { snprintf(w->title, sizeof w->title, "%s", ARGS(1)); if (ARG(0) == main_hwnd) { char u[512]; big5_to_utf8(w->title, u, sizeof u); plat_set_title(u); } }
    HOST_RET(1, 2);
}

/* ---------------------------------------------------------------- 消息循环 */
static int hook_dispatch(Msg *m) {
    if (m->msg != WM_KEYDOWN && m->msg != WM_KEYUP && m->msg != WM_SYSKEYDOWN && m->msg != WM_SYSKEYUP) return 0;
    for (int i = 0; i < 8; i++) {
        if (hooks[i].used && hooks[i].id == 2) {
            uint32_t a[3] = { 0, m->wp, m->lp };
            if (rt_guest_call(hooks[i].proc, 3, a)) return 1;
        }
    }
    return 0;
}

static uint32_t last_pump;
API_FN(PeekMessageA) {
    uint32_t msgp = ARG(0), remove = ARG(4);
    uint32_t now = (uint32_t)plat_ticks_ms();
    plat_pump_events();
    audio_poll();
    ddraw_present_if_dirty();
    (void)last_pump; last_pump = now;
    Msg m;
    if (quit_posted) {
        quit_posted = 0;
        m.hwnd = 0; m.msg = WM_QUIT; m.wp = 0; m.lp = 0; m.time = now; m.x = cur_x; m.y = cur_y;
    } else {
        if (qhead == qtail) {
            /* 队列空：游戏在空转等下一帧（它自己按 GetTickCount 限帧）。让出 1ms，别把一个核心占满，
             * 也避免空转和呈现/系统调度互相抢占造成帧间隔抖动 */
            plat_sleep_ms(1);
            HOST_RET(0, 5);
            return;
        }
        m = queue[qhead];
        if (remove & 1) qhead = (qhead + 1) % MAX_MSGS;
        if (remove & 1) {
            if (hook_dispatch(&m)) {
                /* 钩子吞掉该消息；继续取下一条 */
                HOST_RET(0, 5);
                return;
            }
        }
    }
    rt_wr32(g_mem, msgp, m.hwnd); rt_wr32(g_mem, msgp + 4, m.msg); rt_wr32(g_mem, msgp + 8, m.wp);
    rt_wr32(g_mem, msgp + 12, m.lp); rt_wr32(g_mem, msgp + 16, m.time);
    rt_wr32(g_mem, msgp + 20, (uint32_t)m.x); rt_wr32(g_mem, msgp + 24, (uint32_t)m.y);
    HOST_RET(1, 5);
}

API_FN(TranslateMessage) { HOST_RET(0, 1); }

API_FN(DispatchMessageA) {
    uint32_t msgp = ARG(0);
    uint32_t h = rt_rd32(g_mem, msgp);
    Win *w = win_get(h);
    uint32_t r = 0;
    if (w) r = call_proc(w, h, rt_rd32(g_mem, msgp + 4), rt_rd32(g_mem, msgp + 8), rt_rd32(g_mem, msgp + 12));
    HOST_RET(r, 1);
}

API_FN(PostMessageA) {
    uint32_t h = ARG(0), msg = ARG(1);
    if (msg == WM_QUIT) quit_posted = 1;
    else post(h, msg, ARG(2), ARG(3));
    HOST_RET(1, 4);
}

API_FN(SendMessageA) {
    uint32_t h = ARG(0);
    Win *w = win_get(h);
    uint32_t r = 0;
    if (w) r = call_proc(w, h, ARG(1), ARG(2), ARG(3));
    HOST_RET(r, 4);
}

API_FN(PostQuitMessage) { quit_posted = 1; HOST_RET(0, 1); }

API_FN(SetWindowsHookExA) {
    for (int i = 0; i < 8; i++) {
        if (!hooks[i].used) {
            hooks[i].used = 1; hooks[i].id = (int)ARG(0); hooks[i].proc = ARG(1);
            HOST_RET(0x00F00000u + (uint32_t)i, 4);
            return;
        }
    }
    HOST_RET(0, 4);
}
API_FN(UnhookWindowsHookEx) {
    uint32_t h = ARG(0) - 0x00F00000u;
    if (h < 8) hooks[h].used = 0;
    HOST_RET(1, 1);
}
API_FN(CallNextHookEx) { HOST_RET(0, 4); }

/* ---------------------------------------------------------------- 输入/光标 */
API_FN(GetKeyState) {
    uint32_t vk = ARG(0) & 255;
    uint32_t r = keystate[vk] ? 0x8000 : 0;
    if (keytoggle[vk]) r |= 1;
    /* 修饰键的左右变体 */
    if (vk == 0xA0 || vk == 0xA1) r = keystate[0x10] ? 0x8000 : 0;
    if (vk == 0xA2 || vk == 0xA3) r = keystate[0x11] ? 0x8000 : 0;
    if (vk == 0xA4 || vk == 0xA5) r = keystate[0x12] ? 0x8000 : 0;
    HOST_RET(r, 1);
}

API_FN(GetCursorPos) {
    int x, y;
    plat_get_cursor_pos(&x, &y);
    rt_wr32(g_mem, ARG(0), (uint32_t)x); rt_wr32(g_mem, ARG(0) + 4, (uint32_t)y);
    HOST_RET(1, 1);
}
API_FN(SetCursorPos) { plat_set_cursor_pos((int32_t)ARG(0), (int32_t)ARG(1)); cur_x = (int32_t)ARG(0); cur_y = (int32_t)ARG(1); HOST_RET(1, 2); }
API_FN(ShowCursor) {
    if (ARG(0)) cursor_count++; else cursor_count--;
    plat_show_cursor(cursor_count >= 0);
    HOST_RET((uint32_t)cursor_count, 1);
}
API_FN(SetCursor) { HOST_RET(0, 1); }
API_FN(SetCapture) { HOST_RET(0, 1); }
API_FN(ReleaseCapture) { HOST_RET(1, 0); }
API_FN(MessageBeep) { HOST_RET(1, 1); }

API_FN(GetSystemMetrics) {
    uint32_t i = ARG(0);
    uint32_t r = 0;
    switch (i) {
    case 0: case 16: r = (uint32_t)ddraw_display_width(); break;    /* SM_CXSCREEN / FULLSCREEN */
    case 1: case 17: r = (uint32_t)ddraw_display_height(); break;
    case 4: r = 19; break;
    case 5: case 6: r = 1; break;
    case 7: case 8: r = 3; break;
    case 15: r = 19; break;
    case 32: case 33: r = 4; break;
    default: r = 0;
    }
    HOST_RET(r, 1);
}

/* ---------------------------------------------------------------- 消息框 / 对话框 */
static void big5_to_utf8(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    const uint8_t *s = (const uint8_t *)in;
    int n = (int)strlen(in);
    for (int i = 0; i < n && o + 4 < outsz;) {
        uint16_t w;
        i += cp_mb_to_wc(950, s + i, n - i, &w);
        if (w < 0x80) out[o++] = (char)w;
        else if (w < 0x800) { out[o++] = (char)(0xC0 | (w >> 6)); out[o++] = (char)(0x80 | (w & 63)); }
        else { out[o++] = (char)(0xE0 | (w >> 12)); out[o++] = (char)(0x80 | ((w >> 6) & 63)); out[o++] = (char)(0x80 | (w & 63)); }
    }
    out[o] = 0;
}

API_FN(MessageBoxA) {
    char text[2048], cap[256];
    big5_to_utf8(ARG(1) ? ARGS(1) : "", text, sizeof text);
    big5_to_utf8(ARG(2) ? ARGS(2) : "", cap, sizeof cap);
    uint32_t type = ARG(3);
    host_log("MessageBox[%s]: %s (type=0x%X)", cap, text, type);
    int r = plat_message_box(cap, text, (int)(type & 0xF));
    HOST_RET(r, 4);
}

API_FN(DialogBoxParamA) {
    host_log("DialogBoxParamA 被调用 (模板 %08X)，当前不支持，按取消处理", ARG(1));
    HOST_RET(2, 5);
}
API_FN(EndDialog) { HOST_RET(1, 2); }

/* ---------------------------------------------------------------- 矩形辅助 */
static int32_t R(uint32_t p, int i) { return (int32_t)rt_rd32(g_mem, p + i * 4); }
static void W(uint32_t p, int i, int32_t v) { rt_wr32(g_mem, p + i * 4, (uint32_t)v); }

API_FN(SetRect) { for (int i = 0; i < 4; i++) W(ARG(0), i, (int32_t)ARG(1 + i)); HOST_RET(1, 5); }
API_FN(CopyRect) { memcpy(g_mem + ARG(0), g_mem + ARG(1), 16); HOST_RET(1, 2); }
API_FN(InflateRect) {
    uint32_t p = ARG(0); int dx = (int32_t)ARG(1), dy = (int32_t)ARG(2);
    W(p, 0, R(p, 0) - dx); W(p, 1, R(p, 1) - dy); W(p, 2, R(p, 2) + dx); W(p, 3, R(p, 3) + dy);
    HOST_RET(1, 3);
}
API_FN(OffsetRect) {
    uint32_t p = ARG(0); int dx = (int32_t)ARG(1), dy = (int32_t)ARG(2);
    W(p, 0, R(p, 0) + dx); W(p, 1, R(p, 1) + dy); W(p, 2, R(p, 2) + dx); W(p, 3, R(p, 3) + dy);
    HOST_RET(1, 3);
}
API_FN(IntersectRect) {
    uint32_t d = ARG(0), a = ARG(1), b = ARG(2);
    int32_t l = R(a, 0) > R(b, 0) ? R(a, 0) : R(b, 0), t = R(a, 1) > R(b, 1) ? R(a, 1) : R(b, 1);
    int32_t r = R(a, 2) < R(b, 2) ? R(a, 2) : R(b, 2), bt = R(a, 3) < R(b, 3) ? R(a, 3) : R(b, 3);
    if (l >= r || t >= bt) { W(d, 0, 0); W(d, 1, 0); W(d, 2, 0); W(d, 3, 0); HOST_RET(0, 3); return; }
    W(d, 0, l); W(d, 1, t); W(d, 2, r); W(d, 3, bt);
    HOST_RET(1, 3);
}
API_FN(PtInRect) {
    uint32_t p = ARG(0); int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    HOST_RET(x >= R(p, 0) && x < R(p, 2) && y >= R(p, 1) && y < R(p, 3), 3);
}

/* ---------------------------------------------------------------- printf 家族 */
API_FN(wsprintfA) {
    char buf[4096];
    int n = guest_vsprintf(buf, sizeof buf, ARGS(1), G.esp + 12);
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    memcpy(g_mem + ARG(0), buf, (size_t)n + 1);
    HOST_RET_CDECL(n);
}
API_FN(wvsprintfA) {
    char buf[4096];
    int n = guest_vsprintf(buf, sizeof buf, ARGS(1), ARG(2));
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    memcpy(g_mem + ARG(0), buf, (size_t)n + 1);
    HOST_RET(n, 3);
}

void api_register_user32(void) {
    static const ApiEntry t[] = {
        API_ENTRY(RegisterClassExA), API_ENTRY(CreateWindowExA), API_ENTRY(DefWindowProcA), API_ENTRY(DestroyWindow),
        API_ENTRY(ShowWindow), API_ENTRY(UpdateWindow), API_ENTRY(SetWindowPos), API_ENTRY(GetClientRect), API_ENTRY(ClientToScreen),
        API_ENTRY(AdjustWindowRectEx), API_ENTRY(GetMenu), API_ENTRY(GetDlgItem), API_ENTRY(GetWindowLongA), API_ENTRY(GetWindowTextA),
        API_ENTRY(SetWindowTextA), API_ENTRY(PeekMessageA), API_ENTRY(TranslateMessage), API_ENTRY(DispatchMessageA), API_ENTRY(PostMessageA),
        API_ENTRY(SendMessageA), API_ENTRY(PostQuitMessage), API_ENTRY(SetWindowsHookExA), API_ENTRY(UnhookWindowsHookEx),
        API_ENTRY(CallNextHookEx), API_ENTRY(GetKeyState), API_ENTRY(GetCursorPos), API_ENTRY(SetCursorPos), API_ENTRY(ShowCursor),
        API_ENTRY(SetCursor), API_ENTRY(SetCapture), API_ENTRY(ReleaseCapture), API_ENTRY(MessageBeep), API_ENTRY(GetSystemMetrics),
        API_ENTRY(MessageBoxA), API_ENTRY(DialogBoxParamA), API_ENTRY(EndDialog), API_ENTRY(SetRect), API_ENTRY(CopyRect),
        API_ENTRY(InflateRect), API_ENTRY(OffsetRect), API_ENTRY(IntersectRect), API_ENTRY(PtInRect), API_ENTRY(wsprintfA), API_ENTRY(wvsprintfA),
    };
    api_register(t, (int)(sizeof t / sizeof t[0]));
}
