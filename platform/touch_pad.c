#include "touch_pad.h"

#ifdef __ANDROID__
#include "platform.h"
#include "gamepad_map.h"
#include "host.h"
#include <math.h>
#include <string.h>

/* 按键含义与手柄一致（见 gamepad_map.c）：A=Enter 确认，B=Esc 取消/菜单，X=Space，Y=M，C/D=切换队伍 */
typedef struct { float x, y, r; int vk; const char *label; SDL_FColor tint; } Btn;
#define NBTN 6
static Btn btns[NBTN];
static float dp_x, dp_y, dp_r, dp_dead;   /* 方向盘默认位置（未触摸时的提示）、外半径、中心死区 */
static int lay_w, lay_h;

#define MAX_FINGERS 10
static struct { SDL_FingerID id; float x, y; int on; int joy; } fingers[MAX_FINGERS];
static int joy_active;                    /* 方向盘是否被某根手指占用 */
static float joy_ox, joy_oy;              /* 方向盘本次落点（圆心） */
static uint8_t cur[256];                  /* 当前已发给游戏的按下状态 */

/* 预先算好的单位圆，避免每帧调用三角函数 */
#define SEG 40
static float cs[SEG + 1], sn[SEG + 1];
static int trig_ready;

static void layout(int w, int h) {
    if (w == lay_w && h == lay_h) return;
    lay_w = w; lay_h = h;
    if (!trig_ready) {
        for (int i = 0; i <= SEG; i++) { cs[i] = cosf(6.2831853f * (float)i / SEG); sn[i] = sinf(6.2831853f * (float)i / SEG); }
        trig_ready = 1;
    }
    float s = (float)h * 0.105f, m = s * 0.6f;
    dp_r = s * 2.3f; dp_dead = s * 0.45f;
    dp_x = m + dp_r + s * 0.4f; dp_y = (float)h - m - dp_r;
    float ax = (float)w - m - s * 2.6f, ay = (float)h - m - s * 2.6f, o = s * 1.55f;
    btns[0] = (Btn){ ax,     ay + o, s, 0x0D, "A", { 0.36f, 0.85f, 0.45f, 1 } };   /* 下 */
    btns[1] = (Btn){ ax + o, ay,     s, 0x1B, "B", { 0.95f, 0.38f, 0.38f, 1 } };   /* 右 */
    btns[2] = (Btn){ ax - o, ay,     s, 0x20, "X", { 0.40f, 0.62f, 1.00f, 1 } };   /* 左 */
    btns[3] = (Btn){ ax,     ay - o, s, 0x4D, "Y", { 1.00f, 0.82f, 0.30f, 1 } };   /* 上 */
    btns[4] = (Btn){ (float)w - m - s * 3.1f, m + s * 0.9f, s * 0.85f, 0x43, "C", { 0.80f, 0.80f, 0.90f, 1 } };
    btns[5] = (Btn){ (float)w - m - s * 0.9f, m + s * 0.9f, s * 0.85f, 0x44, "D", { 0.80f, 0.80f, 0.90f, 1 } };
}

static int in_circle(float fx, float fy, float cx, float cy, float r) {
    float dx = fx - cx, dy = fy - cy;
    return dx * dx + dy * dy <= r * r;
}

static int hit_button(float fx, float fy) {
    for (int b = 0; b < NBTN; b++)
        if (in_circle(fx, fy, btns[b].x, btns[b].y, btns[b].r * 1.15f)) return b;
    return -1;
}

/* 根据所有手指的位置重新计算应按下的键，与上次的状态比较后发出按下/松开 */
static void apply(void) {
    uint8_t want[256];
    memset(want, 0, sizeof want);
    if (!gp_connected()) {
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (!fingers[i].on) continue;
            float fx = fingers[i].x, fy = fingers[i].y;
            if (g_video_playing) { want[0x1B] = 1; continue; }   /* 播放片头时，按住屏幕任意处都当作 Esc 跳过 */
            if (fingers[i].joy) {
                float dx = fx - joy_ox, dy = fy - joy_oy, d = sqrtf(dx * dx + dy * dy);
                if (d > dp_dead) {
                    if (dx >  0.38f * d) want[0x27] = 1;
                    if (dx < -0.38f * d) want[0x25] = 1;
                    if (dy >  0.38f * d) want[0x28] = 1;
                    if (dy < -0.38f * d) want[0x26] = 1;
                }
                continue;
            }
            int b = hit_button(fx, fy);
            if (b >= 0) want[btns[b].vk] = 1;
        }
    }
    for (int vk = 0; vk < 256; vk++) {
        if (want[vk] != cur[vk]) { cur[vk] = want[vk]; ev_key(vk, want[vk]); }
    }
}

void tp_event(const SDL_Event *e) {
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION &&
        e->type != SDL_EVENT_FINGER_UP && e->type != SDL_EVENT_FINGER_CANCELED) return;
    if (!lay_w) return;
    SDL_FingerID id = e->tfinger.fingerID;
    int slot = -1;
    for (int i = 0; i < MAX_FINGERS; i++) if (fingers[i].on && fingers[i].id == id) { slot = i; break; }
    if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) {
        if (slot >= 0) {
            if (fingers[slot].joy) joy_active = 0;
            fingers[slot].on = 0; fingers[slot].joy = 0;
        }
    } else {
        float fx = e->tfinger.x * (float)lay_w, fy = e->tfinger.y * (float)lay_h;
        if (slot < 0) {
            for (int i = 0; i < MAX_FINGERS; i++) if (!fingers[i].on) { slot = i; break; }
            if (slot >= 0) {
                fingers[slot].on = 1; fingers[slot].id = id; fingers[slot].joy = 0;
                /* 左半屏的空白处按下：方向盘就出现在落点（贴边时往里收，保证整个圆盘在屏幕内） */
                if (!joy_active && fx < (float)lay_w * 0.5f && hit_button(fx, fy) < 0) {
                    joy_active = 1; fingers[slot].joy = 1;
                    joy_ox = fminf(fmaxf(fx, dp_r * 0.6f), (float)lay_w - dp_r * 0.6f);
                    joy_oy = fminf(fmaxf(fy, dp_r * 0.6f), (float)lay_h - dp_r * 0.6f);
                }
            }
        }
        if (slot >= 0) { fingers[slot].x = fx; fingers[slot].y = fy; }
    }
    apply();
}

/* 圆环/圆盘：用预先算好的单位圆拼三角形，r_in 为 0 时是实心圆 */
static void ring(SDL_Renderer *ren, float cx, float cy, float r_out, float r_in, SDL_FColor c) {
    SDL_Vertex v[(SEG + 1) * 2];
    int idx[SEG * 6], n = 0;
    for (int i = 0; i <= SEG; i++) {
        v[i * 2 + 0] = (SDL_Vertex){ { cx + r_out * cs[i], cy + r_out * sn[i] }, c, { 0, 0 } };
        v[i * 2 + 1] = (SDL_Vertex){ { cx + r_in * cs[i], cy + r_in * sn[i] }, c, { 0, 0 } };
    }
    for (int i = 0; i < SEG; i++) {
        int a = i * 2, b = i * 2 + 1, c2 = i * 2 + 2, d = i * 2 + 3;
        idx[n++] = a; idx[n++] = b; idx[n++] = c2;
        idx[n++] = c2; idx[n++] = b; idx[n++] = d;
    }
    SDL_RenderGeometry(ren, NULL, v, (SEG + 1) * 2, idx, n);
}

static void fill_triangle(SDL_Renderer *ren, float x0, float y0, float x1, float y1, float x2, float y2, SDL_FColor c) {
    SDL_Vertex v[3] = { { { x0, y0 }, c, { 0, 0 } }, { { x1, y1 }, c, { 0, 0 } }, { { x2, y2 }, c, { 0, 0 } } };
    SDL_RenderGeometry(ren, NULL, v, 3, NULL, 0);
}

static SDL_FColor with_a(SDL_FColor c, float a) { c.a = a; return c; }

static void draw_stick(SDL_Renderer *ren, float ox, float oy, float kx, float ky, int live) {
    float a = live ? 1.0f : 0.55f;
    ring(ren, ox, oy, dp_r, 0, (SDL_FColor){ 0, 0, 0, 0.20f * a });                       /* 底盘 */
    ring(ren, ox, oy, dp_r, dp_r * 0.95f, (SDL_FColor){ 1, 1, 1, 0.45f * a });             /* 外圈 */
    float t = dp_r * 0.10f, g = dp_r * 0.78f;                                              /* 四向小箭头，按下方向变亮 */
    SDL_FColor w = { 1, 1, 1, 0.30f * a }, hot = { 1, 1, 1, 0.80f };
    fill_triangle(ren, ox, oy - g - t, ox - t * 1.3f, oy - g + t, ox + t * 1.3f, oy - g + t, cur[0x26] ? hot : w);
    fill_triangle(ren, ox, oy + g + t, ox - t * 1.3f, oy + g - t, ox + t * 1.3f, oy + g - t, cur[0x28] ? hot : w);
    fill_triangle(ren, ox - g - t, oy, ox - g + t, oy - t * 1.3f, ox - g + t, oy + t * 1.3f, cur[0x25] ? hot : w);
    fill_triangle(ren, ox + g + t, oy, ox + g - t, oy - t * 1.3f, ox + g - t, oy + t * 1.3f, cur[0x27] ? hot : w);
    float kr = dp_r * 0.38f;
    ring(ren, kx, ky, kr, 0, (SDL_FColor){ 1, 1, 1, (live ? 0.50f : 0.25f) });             /* 摇杆头 */
    ring(ren, kx, ky, kr, kr * 0.88f, (SDL_FColor){ 1, 1, 1, 0.85f * a });
}

void tp_draw(SDL_Renderer *ren) {
    if (gp_connected()) { if (lay_w) apply(); return; }   /* 手柄接入时不显示；并确保松开残留的触摸按键 */
    int w = 0, h = 0;
    if (!SDL_GetRenderOutputSize(ren, &w, &h) || w <= 0 || h <= 0) return;
    layout(w, h);

    int lw = 0, lh = 0;
    SDL_RendererLogicalPresentation lmode;
    SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &lmode);
    SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);   /* 按窗口像素坐标绘制 */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    /* 方向盘：没人按时在默认位置显示淡淡的提示；按下后出现在落点，摇杆头跟随手指（限制在圆盘内） */
    float ox = dp_x, oy = dp_y, kx = dp_x, ky = dp_y;
    if (joy_active) {
        ox = joy_ox; oy = joy_oy; kx = ox; ky = oy;
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (!fingers[i].on || !fingers[i].joy) continue;
            float dx = fingers[i].x - ox, dy = fingers[i].y - oy, d = sqrtf(dx * dx + dy * dy);
            float k = d > dp_r ? dp_r / d : 1.0f;
            kx = ox + dx * k; ky = oy + dy * k;
        }
    }
    draw_stick(ren, ox, oy, kx, ky, joy_active);

    for (int b = 0; b < NBTN; b++) {
        int down = cur[btns[b].vk];
        float r = btns[b].r * (down ? 0.93f : 1.0f), x = btns[b].x, y = btns[b].y;
        SDL_FColor t = btns[b].tint;
        ring(ren, x, y + r * 0.06f, r * 1.04f, 0, (SDL_FColor){ 0, 0, 0, 0.22f });         /* 阴影 */
        ring(ren, x, y, r, 0, with_a(t, down ? 0.62f : 0.30f));                            /* 底色 */
        ring(ren, x, y, r, r * 0.90f, with_a(t, down ? 1.0f : 0.80f));                     /* 边圈 */
        float k = r / 12.0f;   /* 调试字体是 8x8，放大到约 0.67 倍半径 */
        SDL_SetRenderScale(ren, k, k);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, down ? 255 : 225);
        SDL_RenderDebugText(ren, (x - 4.0f * k) / k, (y - 4.0f * k) / k, btns[b].label);
        SDL_SetRenderScale(ren, 1.0f, 1.0f);
    }

    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderLogicalPresentation(ren, lw, lh, lmode);
}

#else
void tp_event(const SDL_Event *e) { (void)e; }
void tp_draw(SDL_Renderer *ren) { (void)ren; }
#endif
