/* DirectDraw 的软件实现：表面在来宾内存里，Blt/Lock/GetDC 都在 CPU 上完成，
 * 主表面由 plat_present 送到现代图形后端显示。 */
#include "host.h"
#include "platform.h"
#include "gdi.h"
#include <stdlib.h>
#include <string.h>

#define DD_OK 0
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_NOPALETTEATTACHED 0x88760186u

#define DDSD_CAPS 0x1u
#define DDSD_HEIGHT 0x2u
#define DDSD_WIDTH 0x4u
#define DDSD_PITCH 0x8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_LPSURFACE 0x800u
#define DDSD_PIXELFORMAT 0x1000u

#define DDSCAPS_BACKBUFFER 0x4u
#define DDSCAPS_COMPLEX 0x8u
#define DDSCAPS_FLIP 0x10u
#define DDSCAPS_FRONTBUFFER 0x20u
#define DDSCAPS_OFFSCREENPLAIN 0x40u
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_SYSTEMMEMORY 0x800u
#define DDSCAPS_VIDEOMEMORY 0x4000u

#define DDBLT_COLORFILL 0x400u
#define DDBLT_DDFX 0x800u
#define DDBLT_KEYDEST 0x2000u
#define DDBLT_KEYSRC 0x8000u
#define DDBLT_KEYDESTOVERRIDE 0x4000u
#define DDBLT_KEYSRCOVERRIDE 0x10000u

#define DDCKEY_DESTBLT 2u
#define DDCKEY_SRCBLT 8u

/* ---------------------------------------------------------------- 数据结构 */
typedef struct { uint32_t c[256]; int n; int refs; int used; uint32_t guest; } Pal;   /* c: 0x00RRGGBB */

typedef struct {
    int used, refs;
    uint32_t guest;
    int w, h, bpp, pitch;
    uint32_t pixels;               /* 来宾地址 */
    uint32_t caps;
    uint32_t rmask, gmask, bmask;
    int is_primary;
    int attached_back;             /* 主表面的后备缓冲索引，-1 无 */
    int attached_front;            /* 后备缓冲指向的主表面 */
    int pal;                       /* 调色板索引，-1 无 */
    int has_src_ck, has_dst_ck;
    uint32_t src_ck_lo, src_ck_hi, dst_ck_lo, dst_ck_hi;
    uint32_t dc;
    int locked;
} Surf;

typedef struct { int used, refs; uint32_t guest; uint32_t hwnd; } Clipper;

#define MAX_SURFS 8192   /* 战斗里会创建大量小表面（精灵帧），512 不够 */
#define MAX_PALS 256
#define MAX_CLIPPERS 16

static Surf surfs[MAX_SURFS];
static Pal pals[MAX_PALS];
static Clipper clippers[MAX_CLIPPERS];

static struct { uint32_t guest; int refs; int w, h, bpp; uint32_t coop; uint32_t hwnd; int in_mode; } dd;
static uint32_t vt_dd, vt_surf, vt_pal, vt_clip;
static int primary_idx = -1;
static int dirty;
static int window_created;
static uint64_t last_present;
static uint32_t default_pal[256];

int ddraw_display_bpp(void) { return dd.in_mode ? dd.bpp : 32; }
int ddraw_display_width(void) { return dd.in_mode ? dd.w : 800; }
int ddraw_display_height(void) { return dd.in_mode ? dd.h : 600; }
int ddraw_window_created(void) { return window_created; }

static void ensure_window(void) {
    if (!window_created) {
        window_created = 1;
        plat_create_window(dd.w ? dd.w : 800, dd.h ? dd.h : 600, "FlyingSB");
    }
}

/* ---------------------------------------------------------------- COM 对象创建 */
static uint32_t new_obj(uint32_t vtbl, int idx) {
    uint32_t o = gm_calloc(16);
    rt_wr32(g_mem, o, vtbl);
    rt_wr32(g_mem, o + 4, (uint32_t)idx);
    return o;
}

static Surf *surf_of(uint32_t obj) {
    if (!obj) return NULL;
    uint32_t i = rt_rd32(g_mem, obj + 4);
    if (i >= MAX_SURFS || !surfs[i].used) return NULL;
    return &surfs[i];
}
static int surf_idx(Surf *s) { return (int)(s - surfs); }
static Pal *pal_of(uint32_t obj) {
    if (!obj) return NULL;
    uint32_t i = rt_rd32(g_mem, obj + 4);
    return (i < MAX_PALS && pals[i].used) ? &pals[i] : NULL;
}

static void set_masks(Surf *s, int bpp) {
    s->bpp = bpp;
    if (bpp == 16) { s->rmask = 0xF800; s->gmask = 0x07E0; s->bmask = 0x001F; }
    else if (bpp == 32 || bpp == 24) { s->rmask = 0xFF0000; s->gmask = 0xFF00; s->bmask = 0xFF; }
    else { s->rmask = s->gmask = s->bmask = 0; }
}

static int surf_live(void) {
    int n = 0;
    for (int i = 0; i < MAX_SURFS; i++) n += surfs[i].used;
    return n;
}

static int surf_alloc(int w, int h, int bpp, uint32_t caps) {
    for (int i = 0; i < MAX_SURFS; i++) {
        if (!surfs[i].used) {
            Surf *s = &surfs[i];
            memset(s, 0, sizeof *s);
            s->used = 1; s->refs = 1; s->w = w; s->h = h;
            set_masks(s, bpp);
            s->pitch = (w * bpp / 8 + 3) & ~3;
            s->caps = caps;
            s->attached_back = -1; s->attached_front = -1; s->pal = -1;
            s->pixels = gm_calloc((uint32_t)s->pitch * (uint32_t)h + 64);
            if (!s->pixels) {
                s->used = 0;
                host_log("[ddraw] 表面分配失败(来宾堆内存不足): %dx%dx%d，当前存活表面 %d 个", w, h, bpp, surf_live());
                return -1;
            }
            s->guest = new_obj(vt_surf, i);
            /* 存活表面数持续上涨说明游戏没有 Release 或我们的引用计数有泄漏：每翻倍记一条 */
            static int next_report = 256;
            if (i + 1 >= next_report) {   /* 槽位下标从 0 起、总取最小空位，所以下标不到阈值时存活数一定不到 */
                int live = surf_live();
                if (live >= next_report) { host_log("[ddraw] 存活表面数达到 %d（最近一次创建 %dx%dx%d）", live, w, h, bpp); next_report *= 2; }
            }
            return i;
        }
    }
    host_log("[ddraw] 表面槽位用尽(%d): 创建 %dx%dx%d 失败", MAX_SURFS, w, h, bpp);
    return -1;
}

static uint32_t *surf_palette_rgb(Surf *s) {
    if (s->pal >= 0 && pals[s->pal].used) return pals[s->pal].c;
    if (primary_idx >= 0 && surfs[primary_idx].pal >= 0) return pals[surfs[primary_idx].pal].c;
    return default_pal;
}

static void update_window_target(void) {
    GdiTarget t;
    memset(&t, 0, sizeof t);
    if (primary_idx >= 0) {
        Surf *p = &surfs[primary_idx];
        t.bits = g_mem + p->pixels; t.w = p->w; t.h = p->h; t.pitch = p->pitch; t.bpp = p->bpp;
        t.rmask = p->rmask; t.gmask = p->gmask; t.bmask = p->bmask;
        t.pal = p->bpp == 8 ? surf_palette_rgb(p) : NULL;
    }
    gdi_set_window_target(&t);
}

/* ---------------------------------------------------------------- 显示 */
static uint32_t *present_buf;
static size_t present_cap;

void ddraw_present(void) {
    if (g_trace_api) host_log("[ddraw] present primary=%d dirty=%d", primary_idx, dirty);
    if (primary_idx < 0) return;
    Surf *p = &surfs[primary_idx];
    size_t need = (size_t)p->w * p->h;
    if (present_cap < need) { present_buf = (uint32_t *)realloc(present_buf, need * 4); present_cap = need; }
    const uint8_t *src = g_mem + p->pixels;
    if (g_trace_api && p->bpp == 8) {
        const uint32_t *tp = surf_palette_rgb(p);
        size_t nzi = 0, nzp = 0;
        for (size_t i = 0; i < need; i++) nzi += src[i] != 0;
        for (int i = 0; i < 256; i++) nzp += tp[i] != 0;
        host_log("[ddraw] 主表面非零索引=%zu 调色板非零项=%zu pal=%d", nzi, nzp, p->pal);
    }
    if (p->bpp == 8) {
        const uint32_t *pal = surf_palette_rgb(p);
        for (int y = 0; y < p->h; y++) {
            const uint8_t *row = src + (size_t)y * p->pitch;
            uint32_t *out = present_buf + (size_t)y * p->w;
            for (int x = 0; x < p->w; x++) out[x] = pal[row[x]];
        }
    } else if (p->bpp == 16) {
        static uint32_t lut565[65536];
        static int lut_ready;
        if (!lut_ready) {
            for (uint32_t v = 0; v < 65536; v++) {
                uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
                lut565[v] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            }
            lut_ready = 1;
        }
        for (int y = 0; y < p->h; y++) {
            const uint16_t *row = (const uint16_t *)(src + (size_t)y * p->pitch);
            uint32_t *out = present_buf + (size_t)y * p->w;
            for (int x = 0; x < p->w; x++) out[x] = lut565[row[x]];
        }
    } else if (p->bpp == 32) {
        for (int y = 0; y < p->h; y++) memcpy(present_buf + (size_t)y * p->w, src + (size_t)y * p->pitch, (size_t)p->w * 4);
    } else {
        for (int y = 0; y < p->h; y++) {
            const uint8_t *row = src + (size_t)y * p->pitch;
            uint32_t *out = present_buf + (size_t)y * p->w;
            for (int x = 0; x < p->w; x++) out[x] = (uint32_t)(row[x * 3 + 2] << 16 | row[x * 3 + 1] << 8 | row[x * 3]);
        }
    }
    if (g_res_w) {
        /* 宽屏：游戏的视口矩形(0x742308 左 / 0x74230C 上 / 0x742310 右 / 0x742314 下)之外应是黑边，
         * 视口缩放时旧画面会残留在边缘，这里显示时一律涂黑 */
        int l = (int)rt_rd32(g_mem, 0x742308), t = (int)rt_rd32(g_mem, 0x74230C);
        int r = (int)rt_rd32(g_mem, 0x742310), b = (int)rt_rd32(g_mem, 0x742314);
        if (l >= 0 && t >= 0 && r > l && b > t && r <= p->w && b <= p->h) {
            for (int y = 0; y < p->h; y++) {
                uint32_t *out = present_buf + (size_t)y * p->w;
                if (y < t || y >= b) memset(out, 0, (size_t)p->w * 4);
                else { memset(out, 0, (size_t)l * 4); memset(out + r, 0, (size_t)(p->w - r) * 4); }
            }
        }
    }
    ensure_window();
    {   /* 开发辅助：FLYINGSB_DUMP=目录 时把部分帧存成 PPM，便于无人值守验证画面 */
        static const char *dump_dir; static int dump_init, frame_no, dumped;
        if (!dump_init) { dump_init = 1; dump_dir = getenv("FLYINGSB_DUMP"); }
        if (dump_dir && dumped < 200 && (frame_no % 20) == 0) {
            char path[1024];
            snprintf(path, sizeof path, "%s/frame_%04d.ppm", dump_dir, frame_no);
            FILE *fp = fopen(path, "wb");
            if (fp) {
                fprintf(fp, "P6\n%d %d\n255\n", p->w, p->h);
                for (size_t i = 0; i < need; i++) { uint8_t rgb[3] = { (uint8_t)(present_buf[i] >> 16), (uint8_t)(present_buf[i] >> 8), (uint8_t)present_buf[i] }; fwrite(rgb, 1, 3, fp); }
                fclose(fp);
                dumped++;
            }
        }
        frame_no++;
    }
    plat_present(present_buf, p->w, p->h, p->w * 4);
    last_present = plat_ticks_ms();
    dirty = 0;
}

void ddraw_present_if_dirty(void) {
    if (dirty && plat_ticks_ms() - last_present >= 12) ddraw_present();
}

/* ---------------------------------------------------------------- 像素辅助 */
static uint32_t pix_get(const Surf *s, int x, int y) {
    const uint8_t *p = g_mem + s->pixels + (size_t)y * s->pitch + (size_t)x * (s->bpp / 8);
    switch (s->bpp) {
    case 8: return *p;
    case 16: return (uint32_t)(p[0] | p[1] << 8);
    case 24: return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16);
    default: return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
    }
}
static void pix_put(const Surf *s, int x, int y, uint32_t v) {
    uint8_t *p = g_mem + s->pixels + (size_t)y * s->pitch + (size_t)x * (s->bpp / 8);
    switch (s->bpp) {
    case 8: *p = (uint8_t)v; break;
    case 16: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break;
    case 24: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); break;
    default: p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    }
}

static uint32_t to_rgb(const Surf *s, uint32_t v) {
    if (s->bpp == 8) return surf_palette_rgb((Surf *)s)[v & 255];
    if (s->bpp == 16) {
        uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
    return v & 0xFFFFFF;
}

static uint32_t from_rgb(const Surf *s, uint32_t rgb) {
    if (s->bpp == 8) {
        const uint32_t *pal = surf_palette_rgb((Surf *)s);
        int best = 0; int64_t bd = INT64_MAX;
        int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
        for (int i = 0; i < 256; i++) {
            int pr = (pal[i] >> 16) & 255, pg = (pal[i] >> 8) & 255, pb = pal[i] & 255;
            int64_t d = (int64_t)(r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb);
            if (d < bd) { bd = d; best = i; if (!d) break; }
        }
        return (uint32_t)best;
    }
    if (s->bpp == 16) return ((rgb >> 19) & 31) << 11 | ((rgb >> 10) & 63) << 5 | ((rgb >> 3) & 31);
    return rgb & 0xFFFFFF;
}

typedef struct { int l, t, r, b; } Rc;

static int read_rect(uint32_t p, Rc *r, int w, int h) {
    if (!p) { r->l = 0; r->t = 0; r->r = w; r->b = h; return 1; }
    r->l = (int32_t)rt_rd32(g_mem, p); r->t = (int32_t)rt_rd32(g_mem, p + 4);
    r->r = (int32_t)rt_rd32(g_mem, p + 8); r->b = (int32_t)rt_rd32(g_mem, p + 12);
    return 1;
}

/* ---------------------------------------------------------------- Blt 核心 */
static void do_blt(Surf *d, Rc dr, Surf *s, Rc sr, uint32_t flags, int mirror_lr, int mirror_ud, uint32_t fill, int has_fill,
                   int use_src_ck, int use_dst_ck, uint32_t sck_lo, uint32_t sck_hi, uint32_t dck_lo, uint32_t dck_hi) {
    int dw = dr.r - dr.l, dh = dr.b - dr.t;
    if (dw <= 0 || dh <= 0) return;
    if (has_fill) {
        int l = dr.l < 0 ? 0 : dr.l, t = dr.t < 0 ? 0 : dr.t;
        int r = dr.r > d->w ? d->w : dr.r, b = dr.b > d->h ? d->h : dr.b;
        if (r <= l || b <= t) return;
        for (int y = t; y < b; y++) {
            uint8_t *row = g_mem + d->pixels + (size_t)y * d->pitch + (size_t)l * (d->bpp / 8);
            int n = r - l;
            if (d->bpp == 8) memset(row, (int)(fill & 0xFF), (size_t)n);
            else if (d->bpp == 16) { uint16_t *q = (uint16_t *)row; uint16_t v = (uint16_t)fill; for (int x = 0; x < n; x++) q[x] = v; }
            else if (d->bpp == 32) { uint32_t *q = (uint32_t *)row; for (int x = 0; x < n; x++) q[x] = fill; }
            else for (int x = l; x < r; x++) pix_put(d, x, y, fill);
        }
        return;
    }
    int sw = sr.r - sr.l, sh = sr.b - sr.t;
    if (sw <= 0 || sh <= 0) return;
    /* 无缩放、无镜像、同格式、无色键: memmove 快路径，并处理裁剪 */
    if (sw == dw && sh == dh && !mirror_lr && !mirror_ud) {
        int dx0 = dr.l, dy0 = dr.t, sx0 = sr.l, sy0 = sr.t, w = dw, h = dh;
        if (dx0 < 0) { sx0 -= dx0; w += dx0; dx0 = 0; }
        if (dy0 < 0) { sy0 -= dy0; h += dy0; dy0 = 0; }
        if (sx0 < 0) { dx0 -= sx0; w += sx0; sx0 = 0; }
        if (sy0 < 0) { dy0 -= sy0; h += sy0; sy0 = 0; }
        if (dx0 + w > d->w) w = d->w - dx0;
        if (dy0 + h > d->h) h = d->h - dy0;
        if (sx0 + w > s->w) w = s->w - sx0;
        if (sy0 + h > s->h) h = s->h - sy0;
        if (w <= 0 || h <= 0) return;
        int same = (s == d);
        if (s->bpp == d->bpp && !use_src_ck && !use_dst_ck) {
            int bytes = w * (d->bpp / 8);
            if (same && sy0 < dy0) {
                for (int y = h - 1; y >= 0; y--)
                    memmove(g_mem + d->pixels + (size_t)(dy0 + y) * d->pitch + (size_t)dx0 * (d->bpp / 8),
                            g_mem + s->pixels + (size_t)(sy0 + y) * s->pitch + (size_t)sx0 * (s->bpp / 8), (size_t)bytes);
            } else {
                for (int y = 0; y < h; y++)
                    memmove(g_mem + d->pixels + (size_t)(dy0 + y) * d->pitch + (size_t)dx0 * (d->bpp / 8),
                            g_mem + s->pixels + (size_t)(sy0 + y) * s->pitch + (size_t)sx0 * (s->bpp / 8), (size_t)bytes);
            }
            return;
        }
        if (s->bpp == d->bpp && (d->bpp == 8 || d->bpp == 16 || d->bpp == 32)) {
            /* 同格式带色键：按像素宽度直接读写，省掉逐像素的 switch */
#define KEYED_ROWS(T) \
            for (int k = 0; k < h; k++) { \
                int yy = (same && sy0 < dy0) ? h - 1 - k : k; \
                const T *sp = (const T *)(g_mem + s->pixels + (size_t)(sy0 + yy) * s->pitch) + sx0; \
                T *dp = (T *)(g_mem + d->pixels + (size_t)(dy0 + yy) * d->pitch) + dx0; \
                for (int x = 0; x < w; x++) { \
                    uint32_t v = sp[x]; \
                    if (use_src_ck && v >= sck_lo && v <= sck_hi) continue; \
                    if (use_dst_ck) { uint32_t dv = dp[x]; if (dv < dck_lo || dv > dck_hi) continue; } \
                    dp[x] = (T)v; \
                } \
            }
            if (d->bpp == 8) { KEYED_ROWS(uint8_t) }
            else if (d->bpp == 16) { KEYED_ROWS(uint16_t) }
            else { KEYED_ROWS(uint32_t) }
#undef KEYED_ROWS
            return;
        }
        for (int k = 0; k < h; k++) {
            int yy = (same && sy0 < dy0) ? h - 1 - k : k;
            for (int x = 0; x < w; x++) {
                uint32_t v = pix_get(s, sx0 + x, sy0 + yy);
                if (use_src_ck && v >= sck_lo && v <= sck_hi) continue;
                if (use_dst_ck) { uint32_t dv = pix_get(d, dx0 + x, dy0 + yy); if (dv < dck_lo || dv > dck_hi) continue; }
                if (s->bpp != d->bpp) v = from_rgb(d, to_rgb(s, v));
                pix_put(d, dx0 + x, dy0 + yy, v);
            }
        }
        return;
    }
    /* 缩放/镜像：最近邻。同格式时预先算好每列的源下标，按像素宽度直接读写 */
    if (s->bpp == d->bpp && (d->bpp == 8 || d->bpp == 16 || d->bpp == 32) && dw <= 4096) {
        int xmap[4096];
        for (int x = 0; x < dw; x++) {
            int sx = (mirror_lr ? (dw - 1 - x) : x) * sw / dw + sr.l;
            xmap[x] = (dr.l + x < 0 || dr.l + x >= d->w || sx < 0 || sx >= s->w) ? -1 : sx;
        }
        int x0 = 0, x1 = dw;
        if (dr.l < 0) x0 = -dr.l;
        if (dr.l + dw > d->w) x1 = d->w - dr.l;
#define SCALED_ROWS(T) \
        for (int y = 0; y < dh; y++) { \
            int dy = dr.t + y; \
            if (dy < 0 || dy >= d->h) continue; \
            int sy = (mirror_ud ? (dh - 1 - y) : y) * sh / dh + sr.t; \
            if (sy < 0 || sy >= s->h) continue; \
            const T *sp = (const T *)(g_mem + s->pixels + (size_t)sy * s->pitch); \
            T *dp = (T *)(g_mem + d->pixels + (size_t)dy * d->pitch) + dr.l; \
            for (int x = x0; x < x1; x++) { \
                int sx = xmap[x]; \
                if (sx < 0) continue; \
                uint32_t v = sp[sx]; \
                if (use_src_ck && v >= sck_lo && v <= sck_hi) continue; \
                if (use_dst_ck) { uint32_t dv = dp[x]; if (dv < dck_lo || dv > dck_hi) continue; } \
                dp[x] = (T)v; \
            } \
        }
        if (d->bpp == 8) { SCALED_ROWS(uint8_t) }
        else if (d->bpp == 16) { SCALED_ROWS(uint16_t) }
        else { SCALED_ROWS(uint32_t) }
#undef SCALED_ROWS
        return;
    }
    for (int y = 0; y < dh; y++) {
        int dy = dr.t + y;
        if (dy < 0 || dy >= d->h) continue;
        int sy = (mirror_ud ? (dh - 1 - y) : y) * sh / dh + sr.t;
        if (sy < 0 || sy >= s->h) continue;
        for (int x = 0; x < dw; x++) {
            int dx = dr.l + x;
            if (dx < 0 || dx >= d->w) continue;
            int sx = (mirror_lr ? (dw - 1 - x) : x) * sw / dw + sr.l;
            if (sx < 0 || sx >= s->w) continue;
            uint32_t v = pix_get(s, sx, sy);
            if (use_src_ck && v >= sck_lo && v <= sck_hi) continue;
            if (use_dst_ck) { uint32_t dv = pix_get(d, dx, dy); if (dv < dck_lo || dv > dck_hi) continue; }
            if (s->bpp != d->bpp) v = from_rgb(d, to_rgb(s, v));
            pix_put(d, dx, dy, v);
        }
    }
}
