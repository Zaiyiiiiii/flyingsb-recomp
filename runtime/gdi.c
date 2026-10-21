/* 软件 GDI 实现（GDI32 导入 + USER32 的 DrawText / GetDC 家族）。 */
#include "host.h"
#include "gdi.h"
#include "codepage.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#define OBJ_BASE 0x00B00000u
#define DC_BASE  0x00A00000u
#define MAX_OBJS 4096
#define MAX_DCS 64

enum { O_NONE, O_PEN, O_FONT, O_PAL, O_RGN, O_BRUSH };

typedef struct {
    int type;
    union {
        struct { int style, width; uint32_t color; } pen;
        struct { int height, width, weight, italic, underline, quality; char face[32]; } font;
        struct { uint32_t pal[256]; int n; } pal;
        struct { int l, t, r, b; } rgn;
    } u;
} Obj;

typedef struct {
    int used;
    GdiTarget t;
    uint32_t font, pen, pal, rgn;
    uint32_t textcolor, bkcolor;
    int bkmode, rop2;
    int px, py;
} DC;

static Obj *objs;
static DC dcs[MAX_DCS];
static uint32_t window_dc;
static GdiTarget window_target;
extern int ddraw_display_bpp(void);
extern int ddraw_display_width(void);
extern int ddraw_display_height(void);

static uint32_t obj_new(int type) {
    if (!objs) objs = (Obj *)calloc(MAX_OBJS, sizeof(Obj));
    for (int i = 1; i < MAX_OBJS; i++) {
        if (objs[i].type == O_NONE) { memset(&objs[i], 0, sizeof objs[i]); objs[i].type = type; return OBJ_BASE + (uint32_t)i; }
    }
    return 0;
}
static Obj *obj_get(uint32_t h) {
    if (!objs || h < OBJ_BASE || h >= OBJ_BASE + MAX_OBJS) return NULL;
    Obj *o = &objs[h - OBJ_BASE];
    return o->type ? o : NULL;
}
static DC *dc_get(uint32_t h) {
    if (h < DC_BASE || h >= DC_BASE + MAX_DCS) return NULL;
    DC *d = &dcs[h - DC_BASE];
    return d->used ? d : NULL;
}

int gdi_is_dc(uint32_t h) { return dc_get(h) != NULL; }

uint32_t gdi_new_dc(const GdiTarget *t) {
    for (int i = 0; i < MAX_DCS; i++) {
        if (!dcs[i].used) {
            memset(&dcs[i], 0, sizeof dcs[i]);
            dcs[i].used = 1;
            if (t) dcs[i].t = *t;
            dcs[i].textcolor = 0; dcs[i].bkcolor = 0xFFFFFF; dcs[i].bkmode = 2; dcs[i].rop2 = 13;
            return DC_BASE + (uint32_t)i;
        }
    }
    return 0;
}
void gdi_free_dc(uint32_t dc) { DC *d = dc_get(dc); if (d) d->used = 0; }
void gdi_set_window_target(const GdiTarget *t) {
    window_target = *t;
    DC *d = dc_get(window_dc);
    if (d) d->t = *t;
}
uint32_t gdi_window_dc(void) {
    if (!window_dc) window_dc = gdi_new_dc(&window_target);
    return window_dc;
}

/* ---------------------------------------------------------------- 调色板 */
static uint32_t sys_pal[256];
const uint32_t *gdi_system_palette(void) {
    static int init;
    if (!init) {
        init = 1;
        static const uint32_t lo[10] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0, 0xC0DCC0, 0xA6CAF0};
        static const uint32_t hi[10] = {0xFFFBF0, 0xA0A0A4, 0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
        for (int i = 0; i < 10; i++) { sys_pal[i] = lo[i]; sys_pal[246 + i] = hi[i]; }
    }
    return sys_pal;
}

/* ---------------------------------------------------------------- 像素写入 */
/* COLORREF 为 0x00BBGGRR；高字节 0x01 表示 PALETTEINDEX（低位是调色板索引），0x02 为 PALETTERGB */
static uint32_t colorref_to_rgb(const GdiTarget *t, uint32_t c) {
    if ((c >> 24) == 1) {
        const uint32_t *pal = (t && t->pal) ? t->pal : gdi_system_palette();
        return pal[c & 255] & 0xFFFFFF;
    }
    return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

static int nearest_index(const uint32_t *pal, uint32_t rgb) {
    int best = 0; int64_t bd = INT64_MAX;
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    for (int i = 0; i < 256; i++) {
        int pr = (pal[i] >> 16) & 255, pg = (pal[i] >> 8) & 255, pb = pal[i] & 255;
        int64_t d = (int64_t)(r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb);
        if (d < bd) { bd = d; best = i; if (!d) break; }
    }
    return best;
}

static uint32_t pack_mask(uint32_t v8, uint32_t mask) {
    if (!mask) return 0;
    int sh = 0; while (!((mask >> sh) & 1)) sh++;
    int bits = 0; while ((mask >> (sh + bits)) & 1) bits++;
    return ((v8 >> (8 - bits)) << sh) & mask;
}

static uint32_t unpack_mask(uint32_t px, uint32_t mask) {
    int sh = 0; while (!((mask >> sh) & 1)) sh++;
    int bits = 0; while ((mask >> (sh + bits)) & 1) bits++;
    uint32_t v = (px & mask) >> sh;
    return (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0));
}

static uint32_t target_pixel_from_rgb(const GdiTarget *t, uint32_t rgb);
static uint32_t target_pixel_from_colorref(const GdiTarget *t, uint32_t c) {
    if (t->bpp == 8 && (c >> 24) == 1) return c & 255;   /* PALETTEINDEX 直接取索引 */
    return target_pixel_from_rgb(t, colorref_to_rgb(t, c));
}

static uint32_t target_pixel_from_rgb(const GdiTarget *t, uint32_t rgb) {
    if (t->bpp == 8) return (uint32_t)nearest_index(t->pal ? t->pal : gdi_system_palette(), rgb);
    return pack_mask((rgb >> 16) & 255, t->rmask) | pack_mask((rgb >> 8) & 255, t->gmask) | pack_mask(rgb & 255, t->bmask);
}

static uint32_t target_get_rgb(const GdiTarget *t, int x, int y) {
    uint8_t *p = t->bits + (size_t)y * t->pitch + (size_t)x * (t->bpp / 8);
    if (t->bpp == 8) return (t->pal ? t->pal : gdi_system_palette())[*p];
    uint32_t px = t->bpp == 16 ? (uint32_t)(p[0] | p[1] << 8) : t->bpp == 32 ? (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24)
                                                                           : (uint32_t)(p[0] | p[1] << 8 | p[2] << 16);
    return (unpack_mask(px, t->rmask) << 16) | (unpack_mask(px, t->gmask) << 8) | unpack_mask(px, t->bmask);
}

static void target_put(const GdiTarget *t, int x, int y, uint32_t px) {
    if (!t->bits || x < 0 || y < 0 || x >= t->w || y >= t->h) return;
    uint8_t *p = t->bits + (size_t)y * t->pitch + (size_t)x * (t->bpp / 8);
    switch (t->bpp) {
    case 8: *p = (uint8_t)px; break;
    case 16: p[0] = (uint8_t)px; p[1] = (uint8_t)(px >> 8); break;
    case 24: p[0] = (uint8_t)px; p[1] = (uint8_t)(px >> 8); p[2] = (uint8_t)(px >> 16); break;
    case 32: p[0] = (uint8_t)px; p[1] = (uint8_t)(px >> 8); p[2] = (uint8_t)(px >> 16); p[3] = (uint8_t)(px >> 24); break;
    }
}

static int in_clip(DC *d, int x, int y) {
    Obj *r = obj_get(d->rgn);
    if (!r) return 1;
    return x >= r->u.rgn.l && x < r->u.rgn.r && y >= r->u.rgn.t && y < r->u.rgn.b;
}

/* 以 coverage(0..255) 混合前景色 */
static void blend_pixel(DC *d, int x, int y, uint32_t fg_rgb, uint32_t fg_px, int cov) {
    if (!d->t.bits || !in_clip(d, x, y)) return;
    if (cov >= 255 || d->t.bpp == 8) { target_put(&d->t, x, y, fg_px); return; }
    if (x < 0 || y < 0 || x >= d->t.w || y >= d->t.h) return;
    uint32_t bg = target_get_rgb(&d->t, x, y);
    int r = (((fg_rgb >> 16) & 255) * cov + ((bg >> 16) & 255) * (255 - cov)) / 255;
    int g = (((fg_rgb >> 8) & 255) * cov + ((bg >> 8) & 255) * (255 - cov)) / 255;
    int b = ((fg_rgb & 255) * cov + (bg & 255) * (255 - cov)) / 255;
    target_put(&d->t, x, y, target_pixel_from_rgb(&d->t, (uint32_t)(r << 16 | g << 8 | b)));
}

/* ---------------------------------------------------------------- 字体 */
/* 字体用 FreeType 渲染：12/16 像素时字体（宋体/新細明體等）自带手工点阵字形，FreeType 会直接取用，
 * 其余尺寸用轮廓渲染。faces[0] 为主字体，其后是后备字体（主字体缺字时依次尝试）。 */
#define MAX_FACES 3
static FT_Library ft_lib;
static struct { FT_Face face; unsigned char *data; char path[260]; int em; } faces[MAX_FACES];
static int nfaces;
static int font_ok, font_tried;

static int try_font(const char *path) {
    if (nfaces >= MAX_FACES) return 0;
    for (int i = 0; i < nfaces; i++) if (!strcmp(faces[i].path, path)) return 0;   /* 已加载 */
    FILE *fp = host_fopen(path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *buf = (n > 0) ? (unsigned char *)malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(buf); return 0; }
    fclose(fp);
    if (!ft_lib && FT_Init_FreeType(&ft_lib)) { free(buf); return 0; }
    FT_Face f;
    if (FT_New_Memory_Face(ft_lib, buf, (FT_Long)n, 0, &f)) { free(buf); return 0; }
    /* 必须是 Unicode 字体且包含常用汉字 */
    if (FT_Select_Charmap(f, FT_ENCODING_UNICODE) || !FT_Get_Char_Index(f, 0x4E2D)) { FT_Done_Face(f); free(buf); return 0; }
    faces[nfaces].face = f;
    faces[nfaces].data = buf;
    snprintf(faces[nfaces].path, sizeof faces[nfaces].path, "%s", path);
    faces[nfaces].em = 0;
    host_log("字体[%d]: %s (%s)", nfaces, path, f->family_name ? f->family_name : "?");
    nfaces++;
    return 1;
}

/* 在项目/程序旁的 fonts/ 目录里找 */
static void try_local_fonts(const char *name) {
    char p[1200];
    const char *base = plat_base_dir();
    static const char *up[] = { "", "../", "../../", "../../../" };
    for (size_t i = 0; i < sizeof up / sizeof up[0]; i++) {
        snprintf(p, sizeof p, "%s%sfonts/%s", base, up[i], name);
        if (try_font(p)) return;
    }
    snprintf(p, sizeof p, "fonts/%s", name);
    try_font(p);
}

int gdi_font_init(void) {
    if (font_tried) return font_ok;
    font_tried = 1;
    const char *env = getenv("FLYINGSB_FONT");
    if (env) try_font(env);
#ifdef HOST_REMOTE_GAME
    /* Web：由服务器在清单里以 @font、@font2、@font3 提供（主字体 + 后备）；Android：@font 是打进 APK 的 fonts/simsun.ttc */
    try_font(WEB_GAME_PFX "@font");
    try_font(WEB_GAME_PFX "@font2");
    try_font(WEB_GAME_PFX "@font3");
#ifdef __ANDROID__
    try_font("/system/fonts/NotoSansCJK-Regular.ttc");   /* 系统自带的中日韩字体，作为缺字后备 */
#endif
    if (nfaces > 0) return font_ok = 1;
    host_log("警告: 没有可用的中文字体（Web 请给服务器加 --font 或把 simsun.ttc 放进 fonts/ 目录；Android 请把 simsun.ttc 放进 fonts/ 后重新打包）。");
    return 0;
#else
    char cfg[1200];
    snprintf(cfg, sizeof cfg, "%s/font.txt", vfs_save_dir());
    FILE *fp = fopen(cfg, "r");
    if (fp) {
        char line[1024];
        if (fgets(line, sizeof line, fp)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            try_font(line);
        }
        fclose(fp);
    }
    try_local_fonts("simsun.ttc");   /* 项目 fonts/ 目录里的宋体优先；找不到再用系统字体 */
    /* 系统字体：宋体优先，其后是繁体覆盖较好的字体，作为主字体缺字时的后备 */
    static const char *cands[] = {
        "C:/Windows/Fonts/simsun.ttc", "C:/Windows/Fonts/msjh.ttc", "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/kaiu.ttf",
        "/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/STHeiti Medium.ttc", "/Library/Fonts/Arial Unicode.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", "/usr/share/fonts/TTF/NotoSansCJK-Regular.ttc",
    };
    for (size_t i = 0; i < sizeof cands / sizeof cands[0] && nfaces < MAX_FACES; i++) try_font(cands[i]);
    if (nfaces > 0) return font_ok = 1;
    host_log("警告: 找不到可用的中文字体。请把宋体 simsun.ttc 放进 fonts/ 目录，或在 %s/font.txt 写字体路径，或设置环境变量 FLYINGSB_FONT。", vfs_save_dir());
    return 0;
#endif
}

typedef struct { uint32_t key; unsigned char *bmp; int w, h, xo, yo; } Glyph;
#define GCACHE 16384
static Glyph *gcache;

static void face_set_size(int i, int em) {
    if (faces[i].em == em) return;
    FT_Set_Pixel_Sizes(faces[i].face, 0, (FT_UInt)em);
    faces[i].em = em;
}

/* 渲染单个字形到 g（覆盖率 0..255）。smooth=0 为单色（8 位色表面，对应 GDI 的无抗锯齿文字），
 * 字体在该字号有内嵌点阵时 FreeType 会直接取用点阵字形。 */
static void render_glyph(Glyph *g, int cp, int em, int smooth) {
    for (int i = 0; i < nfaces; i++) {
        FT_Face f = faces[i].face;
        FT_UInt gi = FT_Get_Char_Index(f, (FT_ULong)cp);
        if (!gi) continue;
        face_set_size(i, em);
        if (FT_Load_Glyph(f, gi, FT_LOAD_RENDER | (smooth ? FT_LOAD_TARGET_NORMAL : FT_LOAD_TARGET_MONO))) continue;
        FT_GlyphSlot s = f->glyph;
        int w = (int)s->bitmap.width, h = (int)s->bitmap.rows;
        g->xo = s->bitmap_left;
        g->yo = -s->bitmap_top;
        g->w = w; g->h = h;
        if (w <= 0 || h <= 0) { g->bmp = NULL; return; }
        g->bmp = (unsigned char *)malloc((size_t)w * (size_t)h);
        for (int y = 0; y < h; y++) {
            const unsigned char *row = s->bitmap.buffer + (ptrdiff_t)y * s->bitmap.pitch;
            for (int x = 0; x < w; x++) {
                int v;
                if (s->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) v = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
                else v = row[x];
                g->bmp[y * w + x] = (unsigned char)v;
            }
        }
        return;
    }
    g->bmp = NULL; g->w = g->h = g->xo = g->yo = 0;   /* 所有字体都缺这个字 */
}

static Glyph *glyph_get(int cp, int em, int smooth) {
    if (!gcache) gcache = (Glyph *)calloc(GCACHE, sizeof(Glyph));
    uint32_t key = ((uint32_t)cp << 12) ^ ((uint32_t)em << 1) ^ (uint32_t)(smooth != 0) ^ 0x80000000u;
    uint32_t h = (key * 2654435761u) >> 18;
    for (;;) {
        Glyph *g = &gcache[h & (GCACHE - 1)];
        if (!g->key) {
            g->key = key;
            render_glyph(g, cp, em, smooth);
            return g;
        }
        if (g->key == key) return g;
        h++;
    }
}

typedef struct { int height_px; int bold; int quality; } FontSel;

static void font_metrics(const Obj *f, int *px, int *ascent, int *descent) {
    int h = f ? f->u.font.height : -16;
    int em = h < 0 ? -h : h;
    if (em == 0) em = 16;
    if (!font_ok) { *px = em; *ascent = em * 8 / 10; *descent = em - *ascent; return; }
    face_set_size(0, em);
    const FT_Size_Metrics *m = &faces[0].face->size->metrics;   /* 26.6 定点；有内嵌点阵时取点阵的度量 */
    *px = em;
    *ascent = (int)((m->ascender + 63) >> 6);
    *descent = (int)((-m->descender + 63) >> 6);
    if (h > 0) {   /* 正数: 单元格高度 = 上升+下降 */
        int cell = *ascent + *descent;
        if (cell > 0 && cell != h) { *ascent = *ascent * h / cell; *descent = h - *ascent; }
    }
}

static int char_advance(int cp, int em) { return cp < 0x80 ? (em + 1) / 2 : em; }

/* 解码 Big5 字符串为码点数组 */
static int decode_text(const uint8_t *s, int len, int *out, int maxn) {
    int n = 0;
    for (int i = 0; i < len && n < maxn;) {
        uint16_t w;
        i += cp_mb_to_wc(950, s + i, len - i, &w);
        out[n++] = w;
    }
    return n;
}

static int text_width(const int *cps, int n, int em) {
    int w = 0;
    for (int i = 0; i < n; i++) w += char_advance(cps[i], em);
    return w;
}

static void draw_glyphs(DC *d, int x, int baseline, const int *cps, int n, int em, int bold, int aa) {
    uint32_t fg = colorref_to_rgb(&d->t, d->textcolor);
    uint32_t fgpx = target_pixel_from_colorref(&d->t, d->textcolor);
    if (g_trace_api && d->t.bpp == 8 && d->t.pal)
        host_log("[gdi] 文字 colorref=%06X -> rgb=%06X 索引=%u 调色板色=%06X", d->textcolor, fg, fgpx, d->t.pal[fgpx & 255]);
    for (int i = 0; i < n; i++) {
        int cp = cps[i];
        int adv = char_advance(cp, em);
        if (cp > 32 && font_ok) {
            Glyph *g = glyph_get(cp, em, aa);
            if (g->bmp) {
                for (int yy = 0; yy < g->h; yy++)
                    for (int xx = 0; xx < g->w; xx++) {
                        int cov = g->bmp[yy * g->w + xx];
                        if (!cov) continue;
                        /* 字形居中到固定宽度单元里 */
                        int gx = x + g->xo + xx + (adv - (int)(g->w + g->xo) > 2 ? 0 : 0);
                        int gy = baseline + g->yo + yy;
                        blend_pixel(d, gx, gy, fg, fgpx, cov);
                        if (bold) blend_pixel(d, gx + 1, gy, fg, fgpx, cov);
                    }
            }
        }
        x += adv;
    }
}

static void fill_rect_bg(DC *d, int l, int t, int r, int b) {
    if (d->bkmode != 2) return;
    uint32_t px = target_pixel_from_colorref(&d->t, d->bkcolor);
    for (int y = t; y < b; y++) for (int x = l; x < r; x++) if (in_clip(d, x, y)) target_put(&d->t, x, y, px);
}

/* DrawTextA 的核心（供 USER32.DrawTextA 与表面文字调用） */
int gdi_draw_text(uint32_t hdc, const uint8_t *text, int len, int *rc, uint32_t flags) {
    DC *d = dc_get(hdc);
    if (!d) return 0;
    gdi_font_init();
    Obj *f = obj_get(d->font);
    int em, asc, desc;
    font_metrics(f, &em, &asc, &desc);
    int line_h = asc + desc;
    if (len < 0) len = (int)strlen((const char *)text);
    int cps[4096];
    int n = decode_text(text, len, cps, 4096);
    int boxw = rc[2] - rc[0];
    /* 切行 */
    int starts[256], ends[256], nl = 0;
    int s = 0, wcur = 0;
    for (int i = 0; i <= n && nl < 255; i++) {
        int brk = 0;
        if (i == n) brk = 1;
        else if (!(flags & 0x20) && (cps[i] == '\n')) brk = 1;
        else if ((flags & 0x10) && !(flags & 0x20) && wcur + char_advance(cps[i], em) > boxw && i > s) { brk = 2; }
        if (brk) {
            starts[nl] = s; ends[nl] = i; nl++;
            if (brk == 1) { s = i + 1; wcur = 0; }
            else { s = i; wcur = char_advance(cps[i], em); }
            if (i == n) break;
        } else wcur += char_advance(cps[i], em);
    }
    if (nl == 0) { starts[0] = ends[0] = 0; nl = 1; }
    /* 去掉回车 */
    int maxw = 0;
    for (int i = 0; i < nl; i++) {
        int e = ends[i];
        while (e > starts[i] && cps[e - 1] == '\r') e--;
        ends[i] = e;
        int w = text_width(cps + starts[i], e - starts[i], em);
        if (w > maxw) maxw = w;
    }
    int total_h = nl * line_h;
    if (flags & 0x400) {   /* DT_CALCRECT */
        rc[2] = rc[0] + maxw;
        rc[3] = rc[1] + total_h;
        return total_h;
    }
    int y0 = rc[1];
    if (flags & 4) y0 = rc[1] + ((rc[3] - rc[1]) - total_h) / 2;
    else if (flags & 8) y0 = rc[3] - total_h;
    for (int i = 0; i < nl; i++) {
        int w = text_width(cps + starts[i], ends[i] - starts[i], em);
        int x0 = rc[0];
        if (flags & 1) x0 = rc[0] + (boxw - w) / 2;
        else if (flags & 2) x0 = rc[2] - w;
        int ly = y0 + i * line_h;
        fill_rect_bg(d, x0, ly, x0 + w, ly + line_h);
        int aa = f ? (f->u.font.quality != 3 && d->t.bpp != 8) : 0;
        draw_glyphs(d, x0, ly + asc, cps + starts[i], ends[i] - starts[i], em, f && f->u.font.weight >= 600, aa);
    }
    return total_h;
}

/* ---------------------------------------------------------------- GDI32 API */
API_FN(CreateFontIndirectA) {
    uint32_t p = ARG(0);
    uint32_t h = obj_new(O_FONT);
    Obj *o = obj_get(h);
    if (o) {
        o->u.font.height = (int32_t)rt_rd32(g_mem, p);
        o->u.font.width = (int32_t)rt_rd32(g_mem, p + 4);
        o->u.font.weight = (int32_t)rt_rd32(g_mem, p + 16);
        o->u.font.italic = g_mem[p + 20];
        o->u.font.underline = g_mem[p + 21];
        o->u.font.quality = g_mem[p + 26];
        memcpy(o->u.font.face, g_mem + p + 28, 31);
        if (g_trace_api) {
            char hex[100] = {0};
            for (int i = 0; i < 31 && g_mem[p + 28 + i]; i++) snprintf(hex + i * 3, 4, "%02X ", g_mem[p + 28 + i]);
            host_log("[gdi] CreateFont 高=%d 宽=%d 粗=%d 字符集=%u 品质=%u 字体名=[%s]", o->u.font.height, o->u.font.width, o->u.font.weight,
                     g_mem[p + 23], g_mem[p + 26], hex);
        }
    }
    HOST_RET(h, 1);
}

API_FN(CreatePen) {
    uint32_t h = obj_new(O_PEN);
    Obj *o = obj_get(h);
    if (o) { o->u.pen.style = (int)ARG(0); o->u.pen.width = (int)ARG(1); o->u.pen.color = ARG(2); }
    HOST_RET(h, 3);
}

API_FN(CreateRectRgnIndirect) {
    uint32_t p = ARG(0);
    uint32_t h = obj_new(O_RGN);
    Obj *o = obj_get(h);
    if (o) {
        o->u.rgn.l = (int32_t)rt_rd32(g_mem, p); o->u.rgn.t = (int32_t)rt_rd32(g_mem, p + 4);
        o->u.rgn.r = (int32_t)rt_rd32(g_mem, p + 8); o->u.rgn.b = (int32_t)rt_rd32(g_mem, p + 12);
    }
    HOST_RET(h, 1);
}

API_FN(CreatePalette) {
    uint32_t p = ARG(0);   /* LOGPALETTE: WORD version, WORD count, PALETTEENTRY[] */
    uint32_t h = obj_new(O_PAL);
    Obj *o = obj_get(h);
    if (o) {
        int n = rt_rd16(g_mem, p + 2);
        if (n > 256) n = 256;
        o->u.pal.n = n;
        for (int i = 0; i < n; i++) {
            const uint8_t *e = g_mem + p + 4 + i * 4;
            o->u.pal.pal[i] = (uint32_t)(e[0] << 16 | e[1] << 8 | e[2]);
        }
    }
    HOST_RET(h, 1);
}

API_FN(DeleteObject) {
    Obj *o = obj_get(ARG(0));
    if (o) o->type = O_NONE;
    HOST_RET(1, 1);
}

API_FN(SelectObject) {
    DC *d = dc_get(ARG(0));
    uint32_t h = ARG(1);
    Obj *o = obj_get(h);
    uint32_t old = 0;
    if (d && o) {
        switch (o->type) {
        case O_FONT: old = d->font; d->font = h; break;
        case O_PEN: old = d->pen; d->pen = h; break;
        case O_RGN: old = d->rgn; d->rgn = h; break;
        case O_PAL: old = d->pal; d->pal = h; break;
        }
    }
    HOST_RET(old, 2);
}

API_FN(SelectClipRgn) {
    DC *d = dc_get(ARG(0));
    if (d) d->rgn = ARG(1);
    HOST_RET(2, 2);
}

API_FN(SelectPalette) {
    DC *d = dc_get(ARG(0));
    uint32_t old = 0;
    if (d) { old = d->pal; d->pal = ARG(1); }
    HOST_RET(old, 3);
}

API_FN(SetBkColor) { DC *d = dc_get(ARG(0)); uint32_t o = d ? d->bkcolor : 0; if (d) d->bkcolor = ARG(1); HOST_RET(o, 2); }
API_FN(SetTextColor) { DC *d = dc_get(ARG(0)); uint32_t o = d ? d->textcolor : 0; if (d) d->textcolor = ARG(1); HOST_RET(o, 2); }
API_FN(SetBkMode) { DC *d = dc_get(ARG(0)); uint32_t o = d ? (uint32_t)d->bkmode : 0; if (d) d->bkmode = (int)ARG(1); HOST_RET(o, 2); }
API_FN(SetROP2) { DC *d = dc_get(ARG(0)); uint32_t o = d ? (uint32_t)d->rop2 : 0; if (d) d->rop2 = (int)ARG(1); HOST_RET(o, 2); }

API_FN(GetNearestPaletteIndex) {
    Obj *o = obj_get(ARG(0));
    uint32_t c = colorref_to_rgb(NULL, ARG(1));
    if (!o || o->type != O_PAL) { HOST_RET(0, 2); return; }
    uint32_t full[256] = {0};
    memcpy(full, o->u.pal.pal, sizeof o->u.pal.pal);
    int best = 0; int64_t bd = INT64_MAX;
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    for (int i = 0; i < o->u.pal.n; i++) {
        int pr = (full[i] >> 16) & 255, pg = (full[i] >> 8) & 255, pb = full[i] & 255;
        int64_t dd = (int64_t)(r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb);
        if (dd < bd) { bd = dd; best = i; }
    }
    HOST_RET(best, 2);
}

API_FN(GetSystemPaletteEntries) {
    uint32_t start = ARG(1), n = ARG(2), out = ARG(3);
    const uint32_t *sp = gdi_system_palette();
    if (out) {
        for (uint32_t i = 0; i < n && start + i < 256; i++) {
            uint32_t c = sp[start + i];
            g_mem[out + i * 4] = (uint8_t)(c >> 16); g_mem[out + i * 4 + 1] = (uint8_t)(c >> 8);
            g_mem[out + i * 4 + 2] = (uint8_t)c; g_mem[out + i * 4 + 3] = 0;
        }
    }
    HOST_RET(n, 4);
}

API_FN(SetSystemPaletteUse) { HOST_RET(1, 2); }

API_FN(GetDeviceCaps) {
    int idx = (int)ARG(1);
    int bpp = ddraw_display_bpp();
    uint32_t r = 0;
    switch (idx) {
    case 2: r = 1; break;                              /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: r = 320; break;
    case 6: r = 240; break;
    case 8: r = (uint32_t)ddraw_display_width(); break;
    case 10: r = (uint32_t)ddraw_display_height(); break;
    case 12: r = (uint32_t)bpp; break;                 /* BITSPIXEL */
    case 14: r = 1; break;                             /* PLANES */
    case 24: r = bpp == 8 ? 20 : (uint32_t)-1; break;  /* NUMCOLORS */
    case 38: r = 0x7E99 | (bpp == 8 ? 0x100 : 0); break; /* RASTERCAPS */
    case 88: case 90: r = 96; break;                   /* LOGPIXELSX/Y */
    case 104: r = bpp == 8 ? 256 : 0; break;           /* SIZEPALETTE */
    case 106: r = bpp == 8 ? 20 : 0; break;            /* NUMRESERVED */
    case 108: r = (uint32_t)(bpp == 8 ? 8 : bpp); break; /* COLORRES */
    default: r = 0;
    }
    HOST_RET(r, 2);
}

API_FN(SetPixel) {
    DC *d = dc_get(ARG(0));
    int x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    if (d && d->t.bits && in_clip(d, x, y)) target_put(&d->t, x, y, target_pixel_from_colorref(&d->t, ARG(3)));
    HOST_RET(ARG(3), 4);
}

API_FN(MoveToEx) {
    DC *d = dc_get(ARG(0));
    if (d) {
        if (ARG(3)) { rt_wr32(g_mem, ARG(3), (uint32_t)d->px); rt_wr32(g_mem, ARG(3) + 4, (uint32_t)d->py); }
        d->px = (int32_t)ARG(1); d->py = (int32_t)ARG(2);
    }
    HOST_RET(1, 4);
}

API_FN(LineTo) {
    DC *d = dc_get(ARG(0));
    int x1 = (int32_t)ARG(1), y1 = (int32_t)ARG(2);
    if (d && d->t.bits) {
        Obj *pen = obj_get(d->pen);
        uint32_t col = pen ? pen->u.pen.color : 0;
        int x0 = d->px, y0 = d->py;
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        uint32_t px = target_pixel_from_colorref(&d->t, col);
        for (;;) {
            if (in_clip(d, x0, y0)) {
                if (d->rop2 == 7) {   /* R2_XORPEN */
                    if (x0 >= 0 && y0 >= 0 && x0 < d->t.w && y0 < d->t.h) {
                        uint32_t cur = target_get_rgb(&d->t, x0, y0);
                        target_put(&d->t, x0, y0, target_pixel_from_rgb(&d->t, cur ^ colorref_to_rgb(&d->t, col)));
                    }
                } else target_put(&d->t, x0, y0, px);
            }
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    if (d) { d->px = x1; d->py = y1; }
    HOST_RET(1, 3);
}

/* ---------------------------------------------------------------- USER32 中与 DC 相关的部分 */
API_FN(GetDC) { HOST_RET(gdi_new_dc(&window_target), 1); }
API_FN(ReleaseDC) { gdi_free_dc(ARG(1)); HOST_RET(1, 2); }
API_FN(BeginPaint) {
    uint32_t ps = ARG(1);
    uint32_t dc = gdi_new_dc(&window_target);
    memset(GP(ps), 0, 64);
    rt_wr32(g_mem, ps, dc);
    rt_wr32(g_mem, ps + 12, (uint32_t)ddraw_display_width());
    rt_wr32(g_mem, ps + 16, (uint32_t)ddraw_display_height());
    HOST_RET(dc, 2);
}
API_FN(EndPaint) { gdi_free_dc(rt_rd32(g_mem, ARG(1))); HOST_RET(1, 2); }
API_FN(DrawTextA) {
    uint32_t hdc = ARG(0), text = ARG(1), rc = ARG(3), flags = ARG(4);
    int len = (int32_t)ARG(2);
    int r[4];
    for (int i = 0; i < 4; i++) r[i] = (int32_t)rt_rd32(g_mem, rc + i * 4);
    int h = gdi_draw_text(hdc, g_mem + text, len, r, flags);
    if (flags & 0x400) for (int i = 0; i < 4; i++) rt_wr32(g_mem, rc + i * 4, (uint32_t)r[i]);
    HOST_RET(h, 5);
}

void api_register_gdi32(void) {
    static const ApiEntry t[] = {
        API_ENTRY(CreateFontIndirectA), API_ENTRY(CreatePen), API_ENTRY(CreateRectRgnIndirect), API_ENTRY(CreatePalette),
        API_ENTRY(DeleteObject), API_ENTRY(SelectObject), API_ENTRY(SelectClipRgn), API_ENTRY(SelectPalette),
        API_ENTRY(SetBkColor), API_ENTRY(SetTextColor), API_ENTRY(SetBkMode), API_ENTRY(SetROP2),
        API_ENTRY(GetNearestPaletteIndex), API_ENTRY(GetSystemPaletteEntries), API_ENTRY(SetSystemPaletteUse),
        API_ENTRY(GetDeviceCaps), API_ENTRY(SetPixel), API_ENTRY(MoveToEx), API_ENTRY(LineTo),
        API_ENTRY(GetDC), API_ENTRY(ReleaseDC), API_ENTRY(BeginPaint), API_ENTRY(EndPaint), API_ENTRY(DrawTextA),
    };
    api_register(t, (int)(sizeof t / sizeof t[0]));
}
