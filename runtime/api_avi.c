/* AVIFIL32 子集：由自带的 AVI 解复用 + Cinepak/MS Video 1 解码器实现 */
#include "host.h"
#include "avi_codec.h"
#include <stdlib.h>
#include <string.h>

#define AVIERR_OK 0
#define AVIERR_UNSUPPORTED 0x80044065u
#define AVIERR_BADPARAM 0x80044068u
#define AVIERR_FILEOPEN 0x80044070u
#define FCC_VIDS 0x73646976u
#define FCC_AUDS 0x73647561u

#define MAX_FILES 16
#define MAX_STREAMS 64
#define MAX_FRAMES 16
#define H_FILE 0x7C000000u
#define H_STREAM 0x7C100000u
#define H_FRAME 0x7C200000u

typedef struct { AviFile *f; int used; } AFile;
typedef struct { int file, sidx; int used; } AStream;
typedef struct { int stream; int bpp; uint32_t buf; uint32_t cap; int used; int dib_w, dib_h; } AFrame;

static AFile files[MAX_FILES];
static AStream streams[MAX_STREAMS];
static AFrame frames[MAX_FRAMES];

static AStream *stream_of(uint32_t h) {
    uint32_t i = h - H_STREAM;
    return (i < MAX_STREAMS && streams[i].used) ? &streams[i] : NULL;
}
static const AviStreamInfo *info_of(AStream *s) { return avi_stream(files[s->file].f, s->sidx); }

API_FN(AVIFileInit) { HOST_RET(0, 0); }
API_FN(AVIFileExit) { HOST_RET(0, 0); }

API_FN(AVIFileOpenA) {
    uint32_t ppf = ARG(0);
    const char *name = ARGS(1);
    char hp[2048];
    if (g_skip_video) { HOST_RET(AVIERR_FILEOPEN, 4); return; }
    if (!vfs_resolve(name, 0, hp, sizeof hp)) {
        host_log("AVIFileOpen: 找不到 %s", name);
        HOST_RET(AVIERR_FILEOPEN, 4);
        return;
    }
    for (int i = 0; i < MAX_FILES; i++) {
        if (!files[i].used) {
            files[i].f = avi_open(hp);
            if (!files[i].f) { host_log("AVIFileOpen: 无法解析 %s", hp); HOST_RET(AVIERR_FILEOPEN, 4); return; }
            files[i].used = 1;
            g_video_playing++;
            rt_wr32(g_mem, ppf, H_FILE + (uint32_t)i);
            if (g_trace_api) host_log("[avi] 打开 %s", hp);
            HOST_RET(AVIERR_OK, 4);
            return;
        }
    }
    HOST_RET(AVIERR_FILEOPEN, 4);
}

API_FN(AVIFileRelease) {
    uint32_t i = ARG(0) - H_FILE;
    if (i < MAX_FILES && files[i].used) {
        for (int k = 0; k < MAX_STREAMS; k++) if (streams[k].used && streams[k].file == (int)i) streams[k].used = 0;
        avi_close(files[i].f);
        files[i].used = 0;
        if (g_video_playing > 0) g_video_playing--;
    }
    HOST_RET(0, 1);
}

API_FN(AVIFileGetStream) {
    uint32_t i = ARG(0) - H_FILE;
    uint32_t pp = ARG(1), type = ARG(2), lparam = ARG(3);
    if (i >= MAX_FILES || !files[i].used) { HOST_RET(AVIERR_BADPARAM, 4); return; }
    AviFile *f = files[i].f;
    int seen = 0;
    for (int k = 0; k < avi_stream_count(f); k++) {
        const AviStreamInfo *si = avi_stream(f, k);
        if (type == 0 || si->type == type) {
            if (seen == (int)lparam) {
                for (int j = 0; j < MAX_STREAMS; j++) {
                    if (!streams[j].used) {
                        streams[j].used = 1; streams[j].file = (int)i; streams[j].sidx = k;
                        rt_wr32(g_mem, pp, H_STREAM + (uint32_t)j);
                        HOST_RET(AVIERR_OK, 4);
                        return;
                    }
                }
            }
            seen++;
        }
    }
    rt_wr32(g_mem, pp, 0);
    HOST_RET(AVIERR_UNSUPPORTED, 4);
}

API_FN(AVIStreamRelease) {
    AStream *s = stream_of(ARG(0));
    if (s) s->used = 0;
    HOST_RET(0, 1);
}

API_FN(AVIStreamInfoA) {
    AStream *s = stream_of(ARG(0));
    uint32_t p = ARG(1), sz = ARG(2);
    if (!s) { HOST_RET(AVIERR_BADPARAM, 3); return; }
    const AviStreamInfo *si = info_of(s);
    uint8_t tmp[140];
    memset(tmp, 0, sizeof tmp);
    #define W32(o, v) do { uint32_t vv = (uint32_t)(v); memcpy(tmp + (o), &vv, 4); } while (0)
    W32(0, si->type); W32(4, si->handler); W32(20, si->scale); W32(24, si->rate); W32(28, si->start);
    uint32_t len = avi_stream_samples(files[s->file].f, s->sidx);
    if (si->type == FCC_AUDS) {
        uint32_t bal = si->fmt_len >= 14 ? (uint32_t)(si->fmt[12] | si->fmt[13] << 8) : 1;
        W32(48, bal);
    }
    W32(32, len); W32(40, si->suggested);
    if (si->type == FCC_VIDS) { W32(60, si->width); W32(64, si->height); }
    #undef W32
    memcpy(g_mem + p, tmp, sz < sizeof tmp ? sz : sizeof tmp);
    HOST_RET(AVIERR_OK, 3);
}

API_FN(AVIStreamLength) {
    AStream *s = stream_of(ARG(0));
    if (!s) { HOST_RET((uint32_t)-1, 1); return; }
    HOST_RET(avi_stream_samples(files[s->file].f, s->sidx), 1);
}
API_FN(AVIStreamStart) { HOST_RET(0, 1); }

API_FN(AVIStreamTimeToSample) {
    AStream *s = stream_of(ARG(0));
    int32_t t = (int32_t)ARG(1);
    if (!s) { HOST_RET((uint32_t)-1, 2); return; }
    const AviStreamInfo *si = info_of(s);
    int64_t r = (int64_t)t * si->rate / ((int64_t)si->scale * 1000);
    HOST_RET((uint32_t)r, 2);
}
API_FN(AVIStreamSampleToTime) {
    AStream *s = stream_of(ARG(0));
    int32_t n = (int32_t)ARG(1);
    if (!s) { HOST_RET((uint32_t)-1, 2); return; }
    const AviStreamInfo *si = info_of(s);
    int64_t r = (int64_t)n * si->scale * 1000 / si->rate;
    HOST_RET((uint32_t)r, 2);
}

API_FN(AVIStreamReadFormat) {
    AStream *s = stream_of(ARG(0));
    uint32_t buf = ARG(2), pcb = ARG(3);
    if (!s) { HOST_RET(AVIERR_BADPARAM, 4); return; }
    const AviStreamInfo *si = info_of(s);
    uint32_t have = (uint32_t)si->fmt_len;
    if (!buf) { if (pcb) rt_wr32(g_mem, pcb, have); HOST_RET(AVIERR_OK, 4); return; }
    uint32_t cap = pcb ? rt_rd32(g_mem, pcb) : have;
    uint32_t n = cap < have ? cap : have;
    memcpy(g_mem + buf, si->fmt, n);
    if (pcb) rt_wr32(g_mem, pcb, n);
    HOST_RET(AVIERR_OK, 4);
}

API_FN(AVIStreamRead) {
    AStream *s = stream_of(ARG(0));
    uint32_t start = ARG(1), n = ARG(2), buf = ARG(3), cb = ARG(4), pbytes = ARG(5), psamples = ARG(6);
    if (!s) { HOST_RET(AVIERR_BADPARAM, 7); return; }
    const AviStreamInfo *si = info_of(s);
    if (si->type != FCC_AUDS) { HOST_RET(AVIERR_UNSUPPORTED, 7); return; }
    uint32_t bal = si->fmt_len >= 14 ? (uint32_t)(si->fmt[12] | si->fmt[13] << 8) : 1;
    uint32_t got = 0;
    if (!buf) {
        avi_read_audio(files[s->file].f, s->sidx, start, n, NULL, 0, &got);
        if (pbytes) rt_wr32(g_mem, pbytes, got * bal);
        if (psamples) rt_wr32(g_mem, psamples, got);
        HOST_RET(AVIERR_OK, 7);
        return;
    }
    avi_read_audio(files[s->file].f, s->sidx, start, n, g_mem + buf, cb, &got);
    if (pbytes) rt_wr32(g_mem, pbytes, got * bal);
    if (psamples) rt_wr32(g_mem, psamples, got);
    HOST_RET(AVIERR_OK, 7);
}

API_FN(AVIStreamGetFrameOpen) {
    AStream *s = stream_of(ARG(0));
    uint32_t wanted = ARG(1);
    if (!s) { HOST_RET(0, 2); return; }
    const AviStreamInfo *si = info_of(s);
    if (si->type != FCC_VIDS) { HOST_RET(0, 2); return; }
    int bpp = si->bitcount;
    if (bpp == 0) bpp = 24;
    if (wanted > 1) {
        int wb = (int)rt_rd16(g_mem, wanted + 14);
        if (wb == 8 || wb == 16 || wb == 24 || wb == 32) bpp = wb;
    }
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (!frames[i].used) {
            frames[i].used = 1; frames[i].stream = (int)(s - streams); frames[i].bpp = bpp;
            int stride = avi_dib_stride(si->width, bpp);
            uint32_t pal = bpp == 8 ? 1024 : 0;
            frames[i].cap = 40 + pal + (uint32_t)stride * (uint32_t)si->height;
            frames[i].buf = gm_calloc(frames[i].cap);
            if (g_trace_api) host_log("[avi] GetFrameOpen 流 %d %dx%d %dbpp", s->sidx, si->width, si->height, bpp);
            HOST_RET(H_FRAME + (uint32_t)i, 2);
            return;
        }
    }
    HOST_RET(0, 2);
}

API_FN(AVIStreamGetFrame) {
    uint32_t i = ARG(0) - H_FRAME;
    int32_t pos = (int32_t)ARG(1);
    if (i >= MAX_FRAMES || !frames[i].used) { HOST_RET(0, 2); return; }
    AFrame *fr = &frames[i];
    AStream *s = &streams[fr->stream];
    const AviStreamInfo *si = info_of(s);
    uint32_t pal[256];
    int stride = 0;
    uint32_t base = fr->buf;
    uint32_t hdr = 40 + (fr->bpp == 8 ? 1024u : 0u);
    if (!avi_decode_frame(files[s->file].f, s->sidx, pos, fr->bpp, g_mem + base + hdr, &stride, pal)) { HOST_RET(0, 2); return; }
    memset(GP(base), 0, 40);
    rt_wr32(g_mem, base, 40);
    rt_wr32(g_mem, base + 4, (uint32_t)si->width);
    rt_wr32(g_mem, base + 8, (uint32_t)si->height);
    rt_wr16(g_mem, base + 12, 1);
    rt_wr16(g_mem, base + 14, (uint16_t)fr->bpp);
    rt_wr32(g_mem, base + 20, (uint32_t)stride * (uint32_t)si->height);
    if (fr->bpp == 8) {
        rt_wr32(g_mem, base + 32, 256);
        for (int k = 0; k < 256; k++) {
            uint32_t c = pal[k];
            /* 若是从 24 位降到 8 位，则用 3-3-2 调色板 */
            if (si->bitcount != 8) { int r = (k >> 5) & 7, g = (k >> 2) & 7, b = k & 3; c = (uint32_t)(((r * 255 / 7) << 16) | ((g * 255 / 7) << 8) | (b * 255 / 3)); }
            g_mem[base + 40 + k * 4] = (uint8_t)c; g_mem[base + 40 + k * 4 + 1] = (uint8_t)(c >> 8);
            g_mem[base + 40 + k * 4 + 2] = (uint8_t)(c >> 16); g_mem[base + 40 + k * 4 + 3] = 0;
        }
    }
    HOST_RET(base, 2);
}

API_FN(AVIStreamGetFrameClose) {
    uint32_t i = ARG(0) - H_FRAME;
    if (i < MAX_FRAMES && frames[i].used) { gm_free(frames[i].buf); frames[i].used = 0; }
    HOST_RET(0, 1);
}

void api_register_avi(void) {
    static const ApiEntry t[] = {
        API_ENTRY(AVIFileInit), API_ENTRY(AVIFileExit), API_ENTRY(AVIFileOpenA), API_ENTRY(AVIFileRelease), API_ENTRY(AVIFileGetStream),
        API_ENTRY(AVIStreamRelease), API_ENTRY(AVIStreamInfoA), API_ENTRY(AVIStreamLength), API_ENTRY(AVIStreamStart),
        API_ENTRY(AVIStreamTimeToSample), API_ENTRY(AVIStreamSampleToTime), API_ENTRY(AVIStreamReadFormat), API_ENTRY(AVIStreamRead),
        API_ENTRY(AVIStreamGetFrameOpen), API_ENTRY(AVIStreamGetFrame), API_ENTRY(AVIStreamGetFrameClose),
    };
    api_register(t, (int)(sizeof t / sizeof t[0]));
}
