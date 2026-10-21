#include "avi_codec.h"
#include <stdlib.h>
#include <string.h>

FILE *host_fopen(const char *host_path, const char *mode);   /* vfs.c：Web 版经 HTTP 读取游戏目录 */

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

typedef struct { uint32_t off, size, flags; uint32_t cum; int is_pc; } Chunk;

typedef struct { uint8_t y[4]; int8_t u, v; } CPEntry;
typedef struct { CPEntry v1[256], v4[256]; } CPBooks;

typedef struct {
    int codec;                 /* 0=raw/未知 1=cinepak 2=msvideo1 */
    int w, h, nbpp;            /* nbpp: cinepak 24, msvideo1 8/16 */
    uint8_t *native;           /* cinepak: RGB24 自上而下；msvideo1: 自底向上的原生像素 */
    int last;
    uint32_t pal[256];
    CPBooks books[32];
    uint8_t *tmp; size_t tmp_cap;
} VState;

typedef struct {
    AviStreamInfo info;
    Chunk *chunks;
    int nchunks, capchunks;
    int has_pc;
    VState *vs;
} Stream;

struct AviFile {
    FILE *fp;
    Stream st[8];
    int nst;
};

static int isxdigit_(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int avi_dib_stride(int width, int bpp) { return ((width * bpp + 31) / 32) * 4; }

static void add_chunk(Stream *s, uint32_t off, uint32_t size, int is_pc) {
    if (s->nchunks == s->capchunks) {
        s->capchunks = s->capchunks ? s->capchunks * 2 : 1024;
        s->chunks = (Chunk *)realloc(s->chunks, s->capchunks * sizeof(Chunk));
    }
    Chunk *c = &s->chunks[s->nchunks++];
    c->off = off; c->size = size; c->flags = 0; c->is_pc = is_pc; c->cum = 0;
    if (is_pc) s->has_pc = 1;
}

static void parse_hdrl(AviFile *f, long end) {
    uint8_t h[12];
    long pos = ftell(f->fp);
    while (pos + 8 <= end) {
        fseek(f->fp, pos, SEEK_SET);
        if (fread(h, 1, 8, f->fp) != 8) return;
        uint32_t id = rd32(h), sz = rd32(h + 4);
        if (id == FOURCC('L', 'I', 'S', 'T')) {
            if (fread(h + 8, 1, 4, f->fp) != 4) return;
            uint32_t lt = rd32(h + 8);
            if (lt == FOURCC('s', 't', 'r', 'l') && f->nst < 8) {
                Stream *s = &f->st[f->nst];
                memset(s, 0, sizeof *s);
                s->info.idx = f->nst;
                long p2 = pos + 12, e2 = pos + 8 + sz;
                while (p2 + 8 <= e2) {
                    fseek(f->fp, p2, SEEK_SET);
                    uint8_t ch[8];
                    if (fread(ch, 1, 8, f->fp) != 8) break;
                    uint32_t cid = rd32(ch), csz = rd32(ch + 4);
                    if (cid == FOURCC('s', 't', 'r', 'h') && csz >= 48) {
                        uint8_t sh[64];
                        fread(sh, 1, csz < 64 ? csz : 64, f->fp);
                        s->info.type = rd32(sh); s->info.handler = rd32(sh + 4);
                        s->info.scale = rd32(sh + 20); s->info.rate = rd32(sh + 24);
                        s->info.start = rd32(sh + 28); s->info.length = rd32(sh + 32);
                        s->info.suggested = rd32(sh + 36); s->info.sample_size = rd32(sh + 44);
                    } else if (cid == FOURCC('s', 't', 'r', 'f')) {
                        s->info.fmt = (uint8_t *)malloc(csz + 4);
                        memset(s->info.fmt, 0, csz + 4);
                        fread(s->info.fmt, 1, csz, f->fp);
                        s->info.fmt_len = (int)csz;
                    }
                    p2 += 8 + csz + (csz & 1);
                }
                if (s->info.type == FOURCC('v', 'i', 'd', 's') && s->info.fmt) {
                    s->info.width = (int32_t)rd32(s->info.fmt + 4);
                    s->info.height = (int32_t)rd32(s->info.fmt + 8);
                    if (s->info.height < 0) s->info.height = -s->info.height;
                    s->info.bitcount = rd16(s->info.fmt + 14);
                }
                f->nst++;
            }
        }
        pos += 8 + sz + (sz & 1);
    }
}

static void parse_movi(AviFile *f, long start, long end) {
    long pos = start;
    uint8_t h[12];
    while (pos + 8 <= end) {
        fseek(f->fp, pos, SEEK_SET);
        if (fread(h, 1, 8, f->fp) != 8) return;
        uint32_t id = rd32(h), sz = rd32(h + 4);
        if (id == FOURCC('L', 'I', 'S', 'T')) {
            parse_movi(f, pos + 12, pos + 8 + sz);
        } else {
            int d0 = h[0], d1 = h[1];
            int sidx = -1;
            if (isxdigit_(d0) && isxdigit_(d1)) {
                int a = d0 <= '9' ? d0 - '0' : (d0 | 32) - 'a' + 10;
                int b = d1 <= '9' ? d1 - '0' : (d1 | 32) - 'a' + 10;
                sidx = a * 16 + b;
            }
            if (sidx >= 0 && sidx < f->nst) add_chunk(&f->st[sidx], (uint32_t)pos + 8, sz, h[2] == 'p' && h[3] == 'c');
        }
        pos += 8 + sz + (sz & 1);
    }
}

AviFile *avi_open(const char *path) {
    FILE *fp = host_fopen(path, "rb");
    if (!fp) return NULL;
    uint8_t h[12];
    if (fread(h, 1, 12, fp) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4)) { fclose(fp); return NULL; }
    AviFile *f = (AviFile *)calloc(1, sizeof *f);
    f->fp = fp;
    long riff_end = 8 + (long)rd32(h + 4);
    long pos = 12;
    long movi_start = 0, movi_end = 0;
    while (pos + 12 <= riff_end) {
        fseek(fp, pos, SEEK_SET);
        if (fread(h, 1, 12, fp) != 12) break;
        uint32_t id = rd32(h), sz = rd32(h + 4), lt = rd32(h + 8);
        if (id == FOURCC('L', 'I', 'S', 'T') && lt == FOURCC('h', 'd', 'r', 'l')) {
            fseek(fp, pos + 12, SEEK_SET);
            parse_hdrl(f, pos + 8 + (long)sz);
        } else if (id == FOURCC('L', 'I', 'S', 'T') && lt == FOURCC('m', 'o', 'v', 'i')) {
            movi_start = pos + 12; movi_end = pos + 8 + (long)sz;
        }
        pos += 8 + (long)sz + (sz & 1);
    }
    if (movi_start) parse_movi(f, movi_start, movi_end);
    /* 音频累计样本数；视频帧数 */
    for (int i = 0; i < f->nst; i++) {
        Stream *s = &f->st[i];
        uint32_t cum = 0;
        uint32_t bal = s->info.type == FOURCC('a', 'u', 'd', 's') && s->info.fmt_len >= 14 ? rd16(s->info.fmt + 12) : 1;
        if (!bal) bal = 1;
        for (int k = 0; k < s->nchunks; k++) { s->chunks[k].cum = cum; cum += s->chunks[k].size / bal; }
        if (s->info.type == FOURCC('v', 'i', 'd', 's')) {
            int n = 0;
            for (int k = 0; k < s->nchunks; k++) if (!s->chunks[k].is_pc) n++;
            if (!s->info.length || (int)s->info.length > n) s->info.length = (uint32_t)n;
        }
    }
    /* idx1: 取关键帧标志 */
    fseek(fp, 0, SEEK_END);
    long fend = ftell(fp);
    pos = movi_end + (movi_end & 1);
    while (pos + 8 <= fend) {
        fseek(fp, pos, SEEK_SET);
        if (fread(h, 1, 8, fp) != 8) break;
        uint32_t id = rd32(h), sz = rd32(h + 4);
        if (id == FOURCC('i', 'd', 'x', '1')) {
            uint32_t n = sz / 16;
            uint8_t e[16];
            int cnt[8] = {0};
            for (uint32_t i = 0; i < n; i++) {
                if (fread(e, 1, 16, fp) != 16) break;
                int d0 = e[0], d1 = e[1];
                if (!isxdigit_(d0) || !isxdigit_(d1)) continue;
                int a = d0 <= '9' ? d0 - '0' : (d0 | 32) - 'a' + 10;
                int b = d1 <= '9' ? d1 - '0' : (d1 | 32) - 'a' + 10;
                int sidx = a * 16 + b;
                if (sidx < f->nst && cnt[sidx] < f->st[sidx].nchunks) {
                    f->st[sidx].chunks[cnt[sidx]].flags = rd32(e + 4);
                    cnt[sidx]++;
                }
            }
            break;
        }
        pos += 8 + sz + (sz & 1);
    }
    return f;
}

void avi_close(AviFile *f) {
    if (!f) return;
    for (int i = 0; i < f->nst; i++) {
        free(f->st[i].chunks); free(f->st[i].info.fmt);
        if (f->st[i].vs) { free(f->st[i].vs->native); free(f->st[i].vs->tmp); free(f->st[i].vs); }
    }
    fclose(f->fp);
    free(f);
}

int avi_stream_count(AviFile *f) { return f->nst; }
const AviStreamInfo *avi_stream(AviFile *f, int i) { return (i >= 0 && i < f->nst) ? &f->st[i].info : NULL; }

/* ---------------------------------------------------------------- Cinepak */
static inline uint8_t clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

static void cp_put_pixel(uint8_t *p, int y, int u, int v) {
    p[0] = clip8(y + 2 * v);
    p[1] = clip8(y - (u >> 1) - v);
    p[2] = clip8(y + 2 * u);
}

static void cp_draw2x2(VState *vs, int x, int y, const CPEntry *e, int dup) {
    /* dup=1: V1 模式，每个亮度值扩展成 2x2；dup=0: 直接 2x2 */
    uint8_t *img = vs->native;
    int w = vs->w, h = vs->h;
    if (dup) {
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                int px = x + i, py = y + j;
                if (px < w && py < h) cp_put_pixel(img + ((size_t)py * w + px) * 3, e->y[(j >> 1) * 2 + (i >> 1)], e->u, e->v);
            }
    } else {
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++) {
                int px = x + i, py = y + j;
                if (px < w && py < h) cp_put_pixel(img + ((size_t)py * w + px) * 3, e->y[j * 2 + i], e->u, e->v);
            }
    }
}

static int cinepak_decode(VState *vs, const uint8_t *d, size_t len) {
    if (len < 10) return 0;
    int flags = d[0];
    int nstrips = (d[8] << 8) | d[9];
    if (nstrips > 32) nstrips = 32;
    size_t pos = 10;
    int ytop = 0;
    for (int si = 0; si < nstrips; si++) {
        if (pos + 12 > len) return 0;
        size_t ssz = (size_t)((d[pos + 2] << 8) | d[pos + 3]);
        int sh = ((d[pos + 8] << 8) | d[pos + 9]) - ((d[pos + 4] << 8) | d[pos + 5]);
        CPBooks *bk = &vs->books[si];
        if (si > 0 && !(flags & 1)) memcpy(bk, &vs->books[si - 1], sizeof *bk);
        int y1 = ytop, y2 = ytop + sh;
        size_t cpos = pos + 12, cend = pos + ssz;
        if (cend > len) cend = len;
        while (cpos + 4 <= cend) {
            int cid = (d[cpos] << 8) | d[cpos + 1];
            size_t csz = (size_t)((d[cpos + 2] << 8) | d[cpos + 3]);
            if (csz < 4) break;
            const uint8_t *cd = d + cpos + 4;
            size_t cdl = csz - 4;
            if (cpos + csz > cend) cdl = cend - cpos - 4;
            const uint8_t *ce = cd + cdl;
            int id = cid >> 8;
            if (id >= 0x20 && id <= 0x27) {
                CPEntry *book = (id & 0x02) ? bk->v1 : bk->v4;
                int gray = (id & 0x04) != 0;
                int inter = (id & 0x01) != 0;
                int esz = gray ? 4 : 6;
                const uint8_t *p = cd;
                if (!inter) {
                    for (int i = 0; i < 256 && p + esz <= ce; i++, p += esz) {
                        memcpy(book[i].y, p, 4);
                        book[i].u = gray ? 0 : (int8_t)p[4];
                        book[i].v = gray ? 0 : (int8_t)p[5];
                    }
                } else {
                    uint32_t flag = 0, mask = 0;
                    for (int i = 0; i < 256; i++) {
                        mask >>= 1;
                        if (!mask) {
                            if (p + 4 > ce) break;
                            flag = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
                            p += 4; mask = 0x80000000u;
                        }
                        if (flag & mask) {
                            if (p + esz > ce) break;
                            memcpy(book[i].y, p, 4);
                            book[i].u = gray ? 0 : (int8_t)p[4];
                            book[i].v = gray ? 0 : (int8_t)p[5];
                            p += esz;
                        }
                    }
                }
            } else if (id >= 0x30 && id <= 0x32) {
                const uint8_t *p = cd;
                uint32_t flag = 0, mask = 0;     /* 块有无编码、V1/V4 选择共用同一条标志位流 */
                for (int y = y1; y < y2; y += 4) {
                    for (int x = 0; x < vs->w; x += 4) {
                        if ((id & 1) && !(mask >>= 1)) {
                            if (p + 4 > ce) goto strip_done;
                            flag = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
                            p += 4; mask = 0x80000000u;
                        }
                        if (!(id & 1) || (flag & mask)) {
                            if (!(id & 2) && !(mask >>= 1)) {
                                if (p + 4 > ce) goto strip_done;
                                flag = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
                                p += 4; mask = 0x80000000u;
                            }
                            if ((id & 2) || (~flag & mask)) {
                                if (p + 1 > ce) goto strip_done;
                                cp_draw2x2(vs, x, y, &bk->v1[*p++], 1);
                            } else if (flag & mask) {
                                if (p + 4 > ce) goto strip_done;
                                for (int q = 0; q < 4; q++) cp_draw2x2(vs, x + (q & 1) * 2, y + (q >> 1) * 2, &bk->v4[*p++], 0);
                            }
                        }
                    }
                }
            }
        strip_done:
            cpos += csz;
        }
        ytop = y2;
        pos += ssz;
    }
    return 1;
}

/* ---------------------------------------------------------------- MS Video 1 */
static void msv1_decode(VState *vs, const uint8_t *d, size_t len) {
    int w = vs->w, h = vs->h;
    int bw = w / 4, bh = h / 4;
    int bytes = vs->nbpp / 8;
    size_t pos = 0;
    int skip = 0;
    int total = bw * bh;
    uint8_t *img = vs->native;   /* 自底向上 */
    int stride = w * bytes;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            if (skip) { skip--; total--; continue; }
            if (pos + 2 > len) return;
            int a = d[pos++], b = d[pos++];
            if (a == 0 && b == 0 && total == 0) return;
            if ((b & 0xFC) == 0x84) { skip = ((b - 0x84) << 8) + a - 1; continue; }
            uint8_t *base = img + (size_t)(by * 4) * stride + (size_t)bx * 4 * bytes;
            uint16_t col[8];
            if (b < 0x80 || b >= 0x90) {
                int ncol = b < 0x80 ? 2 : 8;
                if (bytes == 1) { if (pos + ncol > len) return; for (int i = 0; i < ncol; i++) col[i] = d[pos++]; }
                else { if (pos + ncol * 2 > len) return; for (int i = 0; i < ncol; i++) { col[i] = rd16(d + pos); pos += 2; } }
                unsigned flags = (unsigned)(b << 8) | (unsigned)a;
                for (int py = 0; py < 4; py++) {
                    for (int px = 0; px < 4; px++, flags >>= 1) {
                        int ci = ncol == 2 ? ((flags & 1) ^ 1) : (((py & 2) << 1) + (px & 2) + ((flags & 1) ^ 1));
                        uint8_t *p = base + (size_t)py * stride + (size_t)px * bytes;
                        if (bytes == 1) *p = (uint8_t)col[ci]; else { p[0] = (uint8_t)col[ci]; p[1] = (uint8_t)(col[ci] >> 8); }
                    }
                }
            } else {
                uint16_t c = bytes == 1 ? (uint16_t)a : (uint16_t)((b << 8) | a);
                for (int py = 0; py < 4; py++)
                    for (int px = 0; px < 4; px++) {
                        uint8_t *p = base + (size_t)py * stride + (size_t)px * bytes;
                        if (bytes == 1) *p = (uint8_t)c; else { p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); }
                    }
            }
            total--;
        }
    }
}

/* ---------------------------------------------------------------- 视频帧 */
static VState *vstate(Stream *s) {
    if (s->vs) return s->vs;
    VState *vs = (VState *)calloc(1, sizeof *vs);
    vs->w = s->info.width; vs->h = s->info.height; vs->last = -1;
    uint32_t hd = s->info.handler;
    uint32_t cmp = s->info.fmt ? rd32(s->info.fmt + 16) : 0;
    uint32_t c = hd ? hd : cmp;
    uint32_t lc = c | 0x20202020;
    if (lc == FOURCC('c', 'v', 'i', 'd')) { vs->codec = 1; vs->nbpp = 24; }
    else if (lc == FOURCC('m', 's', 'v', 'c') || lc == FOURCC('c', 'r', 'a', 'm') || lc == FOURCC('w', 'h', 'a', 'm')) { vs->codec = 2; vs->nbpp = s->info.bitcount; }
    else { vs->codec = 0; vs->nbpp = s->info.bitcount; }
    if (vs->codec == 1) vs->nbpp = 24;
    vs->native = (uint8_t *)calloc(1, (size_t)vs->w * vs->h * (vs->nbpp / 8 ? vs->nbpp / 8 : 1) + 16);
    if (s->info.bitcount == 8 && s->info.fmt && s->info.fmt_len >= 40 + 4) {
        int n = (int)rd32(s->info.fmt + 32);
        if (n <= 0 || n > 256) n = 256;
        for (int i = 0; i < n && 40 + i * 4 + 4 <= s->info.fmt_len; i++) {
            const uint8_t *q = s->info.fmt + 40 + i * 4;
            vs->pal[i] = (uint32_t)(q[2] << 16 | q[1] << 8 | q[0]);
        }
    }
    s->vs = vs;
    return vs;
}

static int video_chunk(Stream *s, int n) {
    /* 第 n 个视频帧 = 第 n 个非调色板块 */
    if (!s->has_pc) return n < s->nchunks ? n : -1;
    int k = 0;
    for (int i = 0; i < s->nchunks; i++) {
        if (s->chunks[i].is_pc) continue;
        if (k == n) return i;
        k++;
    }
    return -1;
}

static int decode_one(AviFile *f, Stream *s, VState *vs, int n) {
    int ci = video_chunk(s, n);
    if (ci < 0) return 0;
    Chunk *c = &s->chunks[ci];
    if (c->size == 0) { vs->last = n; return 1; }   /* 空帧: 重复上一帧 */
    if (vs->tmp_cap < c->size) { vs->tmp_cap = c->size + 1024; vs->tmp = (uint8_t *)realloc(vs->tmp, vs->tmp_cap); }
    fseek(f->fp, (long)c->off, SEEK_SET);
    if (fread(vs->tmp, 1, c->size, f->fp) != c->size) return 0;
    if (vs->codec == 1) {
        cinepak_decode(vs, vs->tmp, c->size);
    } else if (vs->codec == 2) {
        msv1_decode(vs, vs->tmp, c->size);
    } else {
        /* 未压缩 DIB 帧 */
        size_t row = (size_t)avi_dib_stride(vs->w, vs->nbpp);
        size_t need = row * vs->h;
        memcpy(vs->native, vs->tmp, c->size < need ? c->size : need);
    }
    vs->last = n;
    return 1;
}

int avi_decode_frame(AviFile *f, int sidx, int n, int bpp, uint8_t *out, int *stride_out, uint32_t *pal_out) {
    if (sidx < 0 || sidx >= f->nst) return 0;
    Stream *s = &f->st[sidx];
    if (s->info.type != FOURCC('v', 'i', 'd', 's')) return 0;
    VState *vs = vstate(s);
    if (n < 0) n = 0;
    if ((uint32_t)n >= s->info.length) n = (int)s->info.length - 1;
    if (vs->last != n) {
        int from;
        if (vs->last >= 0 && n == vs->last + 1) from = n;
        else {
            from = 0;
            if (vs->codec == 1) {
                /* 回溯到最近的关键帧 */
                for (int k = n; k >= 0; k--) {
                    int ci = video_chunk(s, k);
                    if (ci >= 0 && (s->chunks[ci].flags & 0x10)) { from = k; break; }
                }
            } else if (vs->codec == 2) from = 0;
            if (vs->last >= 0 && vs->last < n && from <= vs->last) from = vs->last + 1;
        }
        for (int k = from; k <= n; k++) decode_one(f, s, vs, k);
    }
    int w = vs->w, h = vs->h;
    int ostride = avi_dib_stride(w, bpp);
    *stride_out = ostride;
    if (pal_out) memcpy(pal_out, vs->pal, sizeof vs->pal);
    /* 转换到目标格式 (DIB: 自底向上, BGR) */
    for (int y = 0; y < h; y++) {
        uint8_t *dst = out + (size_t)y * ostride;
        if (vs->codec == 1) {
            const uint8_t *src = vs->native + (size_t)(h - 1 - y) * w * 3;   /* cinepak 自上而下 */
            for (int x = 0; x < w; x++) {
                int r = src[x * 3], g = src[x * 3 + 1], b = src[x * 3 + 2];
                if (bpp == 24) { dst[x * 3] = (uint8_t)b; dst[x * 3 + 1] = (uint8_t)g; dst[x * 3 + 2] = (uint8_t)r; }
                else if (bpp == 32) { dst[x * 4] = (uint8_t)b; dst[x * 4 + 1] = (uint8_t)g; dst[x * 4 + 2] = (uint8_t)r; dst[x * 4 + 3] = 0; }
                else if (bpp == 16) { uint16_t v = (uint16_t)((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)); dst[x * 2] = (uint8_t)v; dst[x * 2 + 1] = (uint8_t)(v >> 8); }
                else if (bpp == 8) dst[x] = (uint8_t)((r >> 5) << 5 | (g >> 5) << 2 | (b >> 6));
            }
        } else {
            int nb = vs->nbpp / 8;
            const uint8_t *src = vs->native + (size_t)y * (vs->codec == 2 ? w * nb : avi_dib_stride(w, vs->nbpp));
            for (int x = 0; x < w; x++) {
                int r, g, b;
                if (vs->nbpp == 8) { uint32_t c = vs->pal[src[x]]; r = (c >> 16) & 255; g = (c >> 8) & 255; b = c & 255; }
                else if (vs->nbpp == 16) { uint16_t v = rd16(src + x * 2); r = ((v >> 10) & 31) << 3 | ((v >> 12) & 7); g = ((v >> 5) & 31) << 3 | ((v >> 7) & 7); b = (v & 31) << 3 | ((v >> 2) & 7); }
                else if (vs->nbpp == 24 || vs->nbpp == 32) { b = src[x * nb]; g = src[x * nb + 1]; r = src[x * nb + 2]; }
                else { r = g = b = 0; }
                if (bpp == vs->nbpp && (bpp == 8 || bpp == 16)) { memcpy(dst + x * nb, src + x * nb, (size_t)nb); continue; }
                if (bpp == 24) { dst[x * 3] = (uint8_t)b; dst[x * 3 + 1] = (uint8_t)g; dst[x * 3 + 2] = (uint8_t)r; }
                else if (bpp == 32) { dst[x * 4] = (uint8_t)b; dst[x * 4 + 1] = (uint8_t)g; dst[x * 4 + 2] = (uint8_t)r; dst[x * 4 + 3] = 0; }
                else if (bpp == 16) { uint16_t v = (uint16_t)((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)); dst[x * 2] = (uint8_t)v; dst[x * 2 + 1] = (uint8_t)(v >> 8); }
                else if (bpp == 8) dst[x] = (uint8_t)((r >> 5) << 5 | (g >> 5) << 2 | (b >> 6));
            }
        }
    }
    return 1;
}

uint32_t avi_stream_samples(AviFile *f, int sidx) {
    Stream *s = &f->st[sidx];
    if (s->info.type != FOURCC('a', 'u', 'd', 's')) return s->info.length;
    if (!s->nchunks) return 0;
    uint32_t bal = s->info.fmt_len >= 14 ? rd16(s->info.fmt + 12) : 1;
    if (!bal) bal = 1;
    Chunk *c = &s->chunks[s->nchunks - 1];
    return c->cum + c->size / bal;
}

/* ---------------------------------------------------------------- 音频 */
int avi_read_audio(AviFile *f, int sidx, uint32_t start, uint32_t nsamp, uint8_t *buf, uint32_t bufsize, uint32_t *samples_read) {
    if (sidx < 0 || sidx >= f->nst) return 0;
    Stream *s = &f->st[sidx];
    uint32_t bal = s->info.fmt_len >= 14 ? rd16(s->info.fmt + 12) : 1;
    if (!bal) bal = 1;
    uint32_t done = 0;
    uint32_t want = nsamp;
    if (buf && bufsize / bal < want) want = bufsize / bal;
    int lo = 0, hi = s->nchunks - 1, ci = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t cs = s->chunks[mid].cum, ce = cs + s->chunks[mid].size / bal;
        if (start < cs) hi = mid - 1; else if (start >= ce) lo = mid + 1; else { ci = mid; break; }
    }
    if (ci < 0) { *samples_read = 0; return 1; }
    uint32_t pos = start;
    while (ci < s->nchunks && done < want) {
        Chunk *c = &s->chunks[ci];
        uint32_t cs = c->cum, in_chunk = (c->size / bal) - (pos - cs);
        uint32_t take = want - done < in_chunk ? want - done : in_chunk;
        if (buf) {
            fseek(f->fp, (long)(c->off + (pos - cs) * bal), SEEK_SET);
            if (fread(buf + (size_t)done * bal, 1, (size_t)take * bal, f->fp) != (size_t)take * bal) break;
        }
        done += take; pos += take; ci++;
    }
    *samples_read = done;
    return 1;
}
