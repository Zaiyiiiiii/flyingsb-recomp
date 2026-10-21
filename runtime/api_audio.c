/* 音频：DirectSound(软件混音) + MCI(WAV 播放)，输出到平台音频后端。 */
#include "host.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void user32_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

#define OUT_RATE 44100
#define MAX_SRC 96

typedef struct {
    int used;
    int kind;                 /* 0=DirectSound 缓冲 1=MCI */
    const uint8_t *data;
    uint32_t size;            /* 字节 */
    int channels, bits;
    uint32_t rate;
    double pos;               /* 帧位置 */
    int playing, looping;
    int vol_mb;               /* DirectSound: 1/100 dB */
    int pan;
    uint32_t freq;            /* DirectSound: 0 表示原始 */
    float gain;               /* MCI: 0..1 */
    /* DirectSound 专用 */
    uint32_t guest_data, guest_obj;
    int refs;
    int primary;
    /* MCI 专用 */
    char alias[32];
    uint8_t *owned;
    uint32_t notify_hwnd;
    int pending_notify;
    int paused;
} Src;

static Src src[MAX_SRC];
static int audio_open;

static inline int frame_bytes(const Src *s) { return s->channels * (s->bits / 8); }
static inline uint32_t total_frames(const Src *s) { return s->size / (uint32_t)frame_bytes(s); }

static float db_gain(int mb) {
    if (mb <= -10000) return 0.0f;
    if (mb >= 0) return 1.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

static inline void get_frame(const Src *s, uint32_t f, float *l, float *r) {
    const uint8_t *p = s->data + (size_t)f * frame_bytes(s);
    if (s->bits == 8) {
        float a = ((int)p[0] - 128) * (1.0f / 128.0f);
        float b = s->channels == 2 ? ((int)p[1] - 128) * (1.0f / 128.0f) : a;
        *l = a; *r = b;
    } else {
        int16_t a, b;
        memcpy(&a, p, 2);
        if (s->channels == 2) memcpy(&b, p + 2, 2); else b = a;
        *l = a * (1.0f / 32768.0f); *r = b * (1.0f / 32768.0f);
    }
}

static void mix_cb(int16_t *out, int frames) {
    static float *acc;
    static int acc_cap;
    if (acc_cap < frames * 2) { acc = (float *)realloc(acc, (size_t)frames * 2 * sizeof(float)); acc_cap = frames * 2; }
    memset(acc, 0, (size_t)frames * 2 * sizeof(float));
    for (int i = 0; i < MAX_SRC; i++) {
        Src *s = &src[i];
        if (!s->used || !s->playing || s->paused || s->primary || !s->data) continue;
        uint32_t nf = total_frames(s);
        if (nf == 0) { s->playing = 0; continue; }
        double step = (double)(s->freq ? s->freq : s->rate) / OUT_RATE;
        float gl, gr;
        if (s->kind == 0) {
            float g = db_gain(s->vol_mb);
            gl = gr = g;
            if (s->pan > 0) gl *= db_gain(-s->pan);
            else if (s->pan < 0) gr *= db_gain(s->pan);
        } else gl = gr = s->gain;
        for (int k = 0; k < frames; k++) {
            uint32_t f0 = (uint32_t)s->pos;
            if (f0 >= nf) {
                if (s->looping) { s->pos -= nf; f0 = (uint32_t)s->pos; if (f0 >= nf) { s->pos = 0; f0 = 0; } }
                else { s->playing = 0; if (s->kind == 1 && s->notify_hwnd) s->pending_notify = 1; break; }
            }
            uint32_t f1 = f0 + 1;
            if (f1 >= nf) f1 = s->looping ? 0 : f0;
            float a = (float)(s->pos - f0);
            float l0, r0, l1, r1;
            get_frame(s, f0, &l0, &r0);
            get_frame(s, f1, &l1, &r1);
            acc[k * 2] += (l0 + (l1 - l0) * a) * gl;
            acc[k * 2 + 1] += (r0 + (r1 - r0) * a) * gr;
            s->pos += step;
        }
    }
    for (int k = 0; k < frames * 2; k++) {
        float v = acc[k];
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        out[k] = (int16_t)(v * 32767.0f);
    }
}

static void ensure_audio(void) {
    if (!audio_open) { audio_open = 1; plat_audio_open(OUT_RATE, 2, mix_cb); }
}

/* 在主线程里发送 MCI 完成通知（音频线程只置标志） */
void audio_poll(void) {
    for (int i = 0; i < MAX_SRC; i++) {
        Src *s = &src[i];
        if (s->used && s->kind == 1 && s->pending_notify) {
            s->pending_notify = 0;
            user32_post(s->notify_hwnd, 0x3B9, 1, (uint32_t)(i + 1));
        }
    }
}

static int src_alloc(void) {
    for (int i = 0; i < MAX_SRC; i++) if (!src[i].used) { memset(&src[i], 0, sizeof src[i]); src[i].used = 1; return i; }
    return -1;
}

/* ================================================================ DirectSound */
#define DS_OK 0
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_GENERIC 0x80004005u

static uint32_t vt_ds, vt_dsb;
static struct { uint32_t guest; int refs; } dsd;
typedef struct { uint32_t name; } Dummy;

static uint32_t new_obj(uint32_t vtbl, int idx) {
    uint32_t o = gm_calloc(16);
    rt_wr32(g_mem, o, vtbl);
    rt_wr32(g_mem, o + 4, (uint32_t)idx);
    return o;
}
static Src *buf_of(uint32_t obj) {
    if (!obj) return NULL;
    uint32_t i = rt_rd32(g_mem, obj + 4);
    return (i < MAX_SRC && src[i].used && src[i].kind == 0) ? &src[i] : NULL;
}

#define HR(v, n) HOST_RET((uint32_t)(v), (n))
#define METHOD(n) static void n(void)

METHOD(ds_QueryInterface) { rt_wr32(g_mem, ARG(2), 0); HR(0x80004002u, 3); }
METHOD(ds_AddRef) { dsd.refs++; HR(dsd.refs, 1); }
METHOD(ds_Release) { if (dsd.refs > 0) dsd.refs--; HR(dsd.refs, 1); }

static int create_buffer(uint32_t desc, uint32_t out, int dup_from) {
    uint32_t flags = rt_rd32(g_mem, desc + 4), bytes = rt_rd32(g_mem, desc + 8), wfx = rt_rd32(g_mem, desc + 16);
    int i = src_alloc();
    if (i < 0) return -1;
    Src *s = &src[i];
    s->kind = 0; s->refs = 1;
    s->primary = (flags & 1) != 0;
    s->guest_obj = new_obj(vt_dsb, i);
    s->vol_mb = 0; s->pan = 0;
    if (wfx) {
        s->channels = rt_rd16(g_mem, wfx + 2);
        s->rate = rt_rd32(g_mem, wfx + 4);
        s->bits = rt_rd16(g_mem, wfx + 14);
    } else { s->channels = 2; s->rate = 22050; s->bits = 16; }
    if (s->channels < 1) s->channels = 1;
    if (s->bits != 8 && s->bits != 16) s->bits = 16;
    if (!s->primary) {
        s->size = bytes;
        s->guest_data = gm_calloc(bytes ? bytes : 16);
        s->data = g_mem + s->guest_data;
        if (s->bits == 8) memset(g_mem + s->guest_data, 128, bytes);
    }
    rt_wr32(g_mem, out, s->guest_obj);
    (void)dup_from;
    return i;
}

METHOD(ds_CreateSoundBuffer) {
    ensure_audio();
    plat_audio_lock();
    int i = create_buffer(ARG(1), ARG(2), -1);
    plat_audio_unlock();
    if (g_trace_api && i >= 0) host_log("[dsound] 创建缓冲 #%d %dch %uHz %dbit %u 字节 primary=%d", i, src[i].channels, src[i].rate, src[i].bits, src[i].size, src[i].primary);
    HR(i < 0 ? DSERR_GENERIC : DS_OK, 4);
}
METHOD(ds_GetCaps) {
    uint32_t p = ARG(1);
    uint32_t sz = rt_rd32(g_mem, p);
    memset(GP(p), 0, sz < 96 ? sz : 96);
    rt_wr32(g_mem, p, sz);
    rt_wr32(g_mem, p + 4, 0x1F3u);       /* 常见能力位: PRIMARYMONO/STEREO/8BIT/16BIT, CONTINUOUSRATE... */
    rt_wr32(g_mem, p + 8, 8000); rt_wr32(g_mem, p + 12, 48000);
    HR(DS_OK, 2);
}
METHOD(ds_DuplicateSoundBuffer) {
    Src *o = buf_of(ARG(1));
    if (!o) { HR(DSERR_INVALIDPARAM, 3); return; }
    plat_audio_lock();
    int i = src_alloc();
    if (i < 0) { plat_audio_unlock(); HR(DSERR_GENERIC, 3); return; }
    Src *s = &src[i];
    *s = *o;
    s->used = 1; s->playing = 0; s->pos = 0; s->refs = 1;
    s->guest_obj = new_obj(vt_dsb, i);
    /* 共享数据(DuplicateSoundBuffer 语义) */
    s->guest_data = o->guest_data;
    s->data = o->data;
    s->owned = (uint8_t *)1;   /* 标记为副本，释放时不回收数据 */
    plat_audio_unlock();
    rt_wr32(g_mem, ARG(2), s->guest_obj);
    HR(DS_OK, 3);
}
METHOD(ds_SetCooperativeLevel) { HR(DS_OK, 3); }
METHOD(ds_Compact) { HR(DS_OK, 1); }
METHOD(ds_GetSpeakerConfig) { rt_wr32(g_mem, ARG(1), 4); HR(DS_OK, 2); }
METHOD(ds_SetSpeakerConfig) { HR(DS_OK, 2); }
METHOD(ds_Initialize) { HR(DS_OK, 2); }

METHOD(db_QueryInterface) { rt_wr32(g_mem, ARG(2), 0); HR(0x80004002u, 3); }
METHOD(db_AddRef) { Src *s = buf_of(ARG(0)); if (s) s->refs++; HR(s ? s->refs : 0, 1); }
METHOD(db_Release) {
    Src *s = buf_of(ARG(0));
    if (!s) { HR(0, 1); return; }
    if (--s->refs <= 0) {
        plat_audio_lock();
        s->playing = 0;
        s->used = 0;
        plat_audio_unlock();
        if (!s->owned) gm_free(s->guest_data);
        gm_free(s->guest_obj);
        HR(0, 1);
        return;
    }
    HR(s->refs, 1);
}
METHOD(db_GetCaps) {
    Src *s = buf_of(ARG(0));
    uint32_t p = ARG(1);
    memset(GP(p), 0, 20);
    rt_wr32(g_mem, p, 20);
    rt_wr32(g_mem, p + 4, 0xF0u);
    if (s) rt_wr32(g_mem, p + 8, s->size);
    HR(DS_OK, 2);
}
METHOD(db_GetCurrentPosition) {
    Src *s = buf_of(ARG(0));
    if (!s) { HR(DSERR_INVALIDPARAM, 3); return; }
    plat_audio_lock();
    uint32_t pos = (uint32_t)s->pos * (uint32_t)frame_bytes(s);
    plat_audio_unlock();
    if (s->size && pos >= s->size) pos %= s->size;
    if (ARG(1)) rt_wr32(g_mem, ARG(1), pos);
    if (ARG(2)) {
        uint32_t w = (pos + (uint32_t)(s->rate / 20) * (uint32_t)frame_bytes(s)) % (s->size ? s->size : 1);
        rt_wr32(g_mem, ARG(2), w);
    }
    HR(DS_OK, 3);
}
METHOD(db_GetFormat) {
    Src *s = buf_of(ARG(0));
    uint32_t p = ARG(1), sz = ARG(2), wr = ARG(3);
    if (s && p && sz >= 18) {
        memset(GP(p), 0, 18);
        rt_wr16(g_mem, p, 1); rt_wr16(g_mem, p + 2, (uint16_t)s->channels); rt_wr32(g_mem, p + 4, s->rate);
        rt_wr32(g_mem, p + 8, s->rate * (uint32_t)frame_bytes(s)); rt_wr16(g_mem, p + 12, (uint16_t)frame_bytes(s));
        rt_wr16(g_mem, p + 14, (uint16_t)s->bits);
    }
    if (wr) rt_wr32(g_mem, wr, 18);
    HR(DS_OK, 4);
}
METHOD(db_GetVolume) { Src *s = buf_of(ARG(0)); rt_wr32(g_mem, ARG(1), (uint32_t)(s ? s->vol_mb : 0)); HR(DS_OK, 2); }
METHOD(db_GetPan) { Src *s = buf_of(ARG(0)); rt_wr32(g_mem, ARG(1), (uint32_t)(s ? s->pan : 0)); HR(DS_OK, 2); }
METHOD(db_GetFrequency) { Src *s = buf_of(ARG(0)); rt_wr32(g_mem, ARG(1), s ? (s->freq ? s->freq : s->rate) : 0); HR(DS_OK, 2); }
METHOD(db_GetStatus) {
    Src *s = buf_of(ARG(0));
    uint32_t st = 0;
    if (s && s->playing) st |= 1 | (s->looping ? 4 : 0);
    rt_wr32(g_mem, ARG(1), st);
    HR(DS_OK, 2);
}
METHOD(db_Initialize) { HR(DS_OK, 3); }
METHOD(db_Lock) {
    Src *s = buf_of(ARG(0));
    uint32_t off = ARG(1), bytes = ARG(2), p1 = ARG(3), n1 = ARG(4), p2 = ARG(5), n2 = ARG(6), flags = ARG(7);
    if (!s) { HR(DSERR_INVALIDPARAM, 8); return; }
    if (flags & 2) bytes = s->size;     /* DSBLOCK_ENTIREBUFFER */
    if (flags & 1) off = 0;             /* DSBLOCK_FROMWRITECURSOR: 近似 */
    if (off >= s->size) off = 0;
    if (bytes > s->size) bytes = s->size;
    uint32_t first = bytes;
    if (off + bytes > s->size) first = s->size - off;
    rt_wr32(g_mem, p1, s->guest_data + off); rt_wr32(g_mem, n1, first);
    if (p2) rt_wr32(g_mem, p2, first < bytes ? s->guest_data : 0);
    if (n2) rt_wr32(g_mem, n2, bytes - first);
    HR(DS_OK, 8);
}
METHOD(db_Play) {
    Src *s = buf_of(ARG(0));
    uint32_t flags = ARG(3);
    if (!s) { HR(DSERR_INVALIDPARAM, 4); return; }
    ensure_audio();
    plat_audio_lock();
    s->playing = 1; s->looping = (flags & 1) != 0;
    plat_audio_unlock();
    HR(DS_OK, 4);
}
METHOD(db_SetCurrentPosition) {
    Src *s = buf_of(ARG(0));
    if (s) { plat_audio_lock(); s->pos = (double)(ARG(1) / (uint32_t)frame_bytes(s)); plat_audio_unlock(); }
    HR(DS_OK, 2);
}
METHOD(db_SetFormat) { HR(DS_OK, 2); }
METHOD(db_SetVolume) { Src *s = buf_of(ARG(0)); if (s) { plat_audio_lock(); s->vol_mb = (int32_t)ARG(1); plat_audio_unlock(); } HR(DS_OK, 2); }
METHOD(db_SetPan) { Src *s = buf_of(ARG(0)); if (s) { plat_audio_lock(); s->pan = (int32_t)ARG(1); plat_audio_unlock(); } HR(DS_OK, 2); }
METHOD(db_SetFrequency) {
    Src *s = buf_of(ARG(0));
    if (s) { plat_audio_lock(); s->freq = ARG(1) == 0 ? 0 : ARG(1); plat_audio_unlock(); }
    HR(DS_OK, 2);
}
METHOD(db_Stop) { Src *s = buf_of(ARG(0)); if (s) { plat_audio_lock(); s->playing = 0; plat_audio_unlock(); } HR(DS_OK, 1); }
METHOD(db_Unlock) { HR(DS_OK, 5); }
METHOD(db_Restore) { HR(DS_OK, 1); }

typedef struct { const char *name; RtHostFn fn; } VM;
static uint32_t make_vtbl(const VM *m, int n) {
    uint32_t v = gm_calloc((uint32_t)n * 4);
    for (int i = 0; i < n; i++) rt_wr32(g_mem, v + (uint32_t)i * 4, rt_register_thunk(m[i].name, m[i].fn));
    return v;
}

API_FN(DirectSoundCreate_ord1) {
    if (!vt_ds) {
        static const VM dsv[] = {
            {"IDirectSound::QueryInterface", ds_QueryInterface}, {"IDirectSound::AddRef", ds_AddRef}, {"IDirectSound::Release", ds_Release},
            {"IDirectSound::CreateSoundBuffer", ds_CreateSoundBuffer}, {"IDirectSound::GetCaps", ds_GetCaps},
            {"IDirectSound::DuplicateSoundBuffer", ds_DuplicateSoundBuffer}, {"IDirectSound::SetCooperativeLevel", ds_SetCooperativeLevel},
            {"IDirectSound::Compact", ds_Compact}, {"IDirectSound::GetSpeakerConfig", ds_GetSpeakerConfig},
            {"IDirectSound::SetSpeakerConfig", ds_SetSpeakerConfig}, {"IDirectSound::Initialize", ds_Initialize},
        };
        vt_ds = make_vtbl(dsv, (int)(sizeof dsv / sizeof dsv[0]));
        static const VM dbv[] = {
            {"IDirectSoundBuffer::QueryInterface", db_QueryInterface}, {"IDirectSoundBuffer::AddRef", db_AddRef}, {"IDirectSoundBuffer::Release", db_Release},
            {"IDirectSoundBuffer::GetCaps", db_GetCaps}, {"IDirectSoundBuffer::GetCurrentPosition", db_GetCurrentPosition},
            {"IDirectSoundBuffer::GetFormat", db_GetFormat}, {"IDirectSoundBuffer::GetVolume", db_GetVolume}, {"IDirectSoundBuffer::GetPan", db_GetPan},
            {"IDirectSoundBuffer::GetFrequency", db_GetFrequency}, {"IDirectSoundBuffer::GetStatus", db_GetStatus},
            {"IDirectSoundBuffer::Initialize", db_Initialize}, {"IDirectSoundBuffer::Lock", db_Lock}, {"IDirectSoundBuffer::Play", db_Play},
            {"IDirectSoundBuffer::SetCurrentPosition", db_SetCurrentPosition}, {"IDirectSoundBuffer::SetFormat", db_SetFormat},
            {"IDirectSoundBuffer::SetVolume", db_SetVolume}, {"IDirectSoundBuffer::SetPan", db_SetPan}, {"IDirectSoundBuffer::SetFrequency", db_SetFrequency},
            {"IDirectSoundBuffer::Stop", db_Stop}, {"IDirectSoundBuffer::Unlock", db_Unlock}, {"IDirectSoundBuffer::Restore", db_Restore},
        };
        vt_dsb = make_vtbl(dbv, (int)(sizeof dbv / sizeof dbv[0]));
    }
    if (!dsd.guest) dsd.guest = new_obj(vt_ds, 0);
    dsd.refs++;
    rt_wr32(g_mem, ARG(1), dsd.guest);
    ensure_audio();
    HOST_RET(DS_OK, 3);
}

/* ================================================================ MCI (WAV) */
static Src *mci_find(const char *alias) {
    for (int i = 0; i < MAX_SRC; i++) if (src[i].used && src[i].kind == 1 && !strcasecmp(src[i].alias, alias)) return &src[i];
    return NULL;
}

static int load_wav_file(const char *host_path, Src *s) {
    FILE *fp = host_fopen(host_path, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(buf); return 0; }
    fclose(fp);
    if (n < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { free(buf); return 0; }
    long p = 12;
    int have_fmt = 0;
    while (p + 8 <= n) {
        uint32_t csz = (uint32_t)(buf[p + 4] | buf[p + 5] << 8 | buf[p + 6] << 16 | (uint32_t)buf[p + 7] << 24);
        if (!memcmp(buf + p, "fmt ", 4) && csz >= 16) {
            s->channels = buf[p + 10] | buf[p + 11] << 8;
            s->rate = (uint32_t)(buf[p + 12] | buf[p + 13] << 8 | buf[p + 14] << 16 | (uint32_t)buf[p + 15] << 24);
            s->bits = buf[p + 22] | buf[p + 23] << 8;
            have_fmt = 1;
        } else if (!memcmp(buf + p, "data", 4)) {
            if (!have_fmt) { free(buf); return 0; }
            if (p + 8 + csz > (uint32_t)n) csz = (uint32_t)(n - p - 8);
            s->owned = buf;
            s->data = buf + p + 8;
            s->size = csz;
            return 1;
        }
        p += 8 + csz + (csz & 1);
    }
    free(buf);
    return 0;
}

static void ret_str(uint32_t buf, uint32_t len, const char *s) {
    if (!buf || !len) return;
    size_t l = strlen(s);
    if (l + 1 > len) l = len - 1;
    memcpy(g_mem + buf, s, l);
    g_mem[buf + l] = 0;
}

/* 把命令切成词；双引号内的空格保留 */
static int tokenize(char *cmd, char **tok, int max) {
    int n = 0;
    char *p = cmd;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            tok[n++] = ++p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            tok[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
    }
    return n;
}

static int find_kw(char **tok, int n, const char *kw) {
    for (int i = 0; i < n; i++) if (!strcasecmp(tok[i], kw)) return i;
    return -1;
}

API_FN(mciSendStringA) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s", ARG(0) ? ARGS(0) : "");
    uint32_t retbuf = ARG(1), retlen = ARG(2), hwnd = ARG(3);
    if (g_trace_api) host_log("[mci] %s", cmd);
    char *tok[32];
    char work[1024];
    snprintf(work, sizeof work, "%s", cmd);
    int n = tokenize(work, tok, 32);
    if (retbuf && retlen) g_mem[retbuf] = 0;
    if (n == 0) { HOST_RET(0, 4); return; }
    int notify = find_kw(tok, n, "notify") >= 0;
    const char *verb = tok[0];
    if (!strcasecmp(verb, "open") && n >= 2) {
        const char *file = tok[1];
        int ai = find_kw(tok, n, "alias");
        const char *alias = (ai >= 0 && ai + 1 < n) ? tok[ai + 1] : file;
        Src *old = mci_find(alias);
        if (old) { HOST_RET(0x122 /* MCIERR_DUPLICATE_ALIAS */, 4); return; }
        char hp[2048];
        vfs_resolve(file, 0, hp, sizeof hp);
        ensure_audio();
        plat_audio_lock();
        int i = src_alloc();
        plat_audio_unlock();
        if (i < 0) { HOST_RET(0x115, 4); return; }
        Src *s = &src[i];
        s->kind = 1; s->gain = 1.0f;
        snprintf(s->alias, sizeof s->alias, "%s", alias);
        if (!load_wav_file(hp, s)) {
            host_log("MCI: 无法打开 %s", hp);
            s->used = 0;
            HOST_RET(0x10B /* MCIERR_FILE_NOT_FOUND */, 4);
            return;
        }
        HOST_RET(0, 4);
        return;
    }
    if (!strcasecmp(verb, "close")) {
        if (n >= 2 && !strcasecmp(tok[1], "all")) {
            plat_audio_lock();
            for (int i = 0; i < MAX_SRC; i++) if (src[i].used && src[i].kind == 1) { src[i].playing = 0; src[i].used = 0; free(src[i].owned); }
            plat_audio_unlock();
            HOST_RET(0, 4);
            return;
        }
        Src *s = n >= 2 ? mci_find(tok[1]) : NULL;
        if (s) { plat_audio_lock(); s->playing = 0; s->used = 0; plat_audio_unlock(); free(s->owned); }
        HOST_RET(0, 4);
        return;
    }
    Src *s = n >= 2 ? mci_find(tok[1]) : NULL;
    if (!s) { HOST_RET(0x106 /* MCIERR_INVALID_DEVICE_NAME */, 4); return; }
    if (!strcasecmp(verb, "play")) {
        int fi = find_kw(tok, n, "from"), ti = find_kw(tok, n, "to");
        plat_audio_lock();
        if (fi >= 0 && fi + 1 < n) s->pos = atof(tok[fi + 1]) * s->rate / 1000.0;
        else if (!s->playing && s->pos >= total_frames(s)) s->pos = 0;
        (void)ti;
        s->playing = 1; s->paused = 0; s->looping = 0;
        s->notify_hwnd = notify ? hwnd : 0;
        plat_audio_unlock();
        HOST_RET(0, 4);
        return;
    }
    if (!strcasecmp(verb, "stop") || !strcasecmp(verb, "pause")) {
        plat_audio_lock();
        if (!strcasecmp(verb, "stop")) s->playing = 0; else s->paused = 1;
        plat_audio_unlock();
        HOST_RET(0, 4);
        return;
    }
    if (!strcasecmp(verb, "resume")) { plat_audio_lock(); s->paused = 0; plat_audio_unlock(); HOST_RET(0, 4); return; }
    if (!strcasecmp(verb, "seek")) {
        plat_audio_lock();
        if (find_kw(tok, n, "start") >= 0) s->pos = 0;
        else if (find_kw(tok, n, "end") >= 0) s->pos = total_frames(s);
        else { int ti = find_kw(tok, n, "to"); if (ti >= 0 && ti + 1 < n) s->pos = atof(tok[ti + 1]) * s->rate / 1000.0; }
        plat_audio_unlock();
        HOST_RET(0, 4);
        return;
    }
    if (!strcasecmp(verb, "setaudio")) {
        int vi = find_kw(tok, n, "volume");
        if (vi >= 0) {
            int ti = find_kw(tok, n, "to");
            if (ti >= 0 && ti + 1 < n) { plat_audio_lock(); s->gain = (float)atoi(tok[ti + 1]) / 1000.0f; if (s->gain > 1.0f) s->gain = 1.0f; plat_audio_unlock(); }
        }
        HOST_RET(0, 4);
        return;
    }
    if (!strcasecmp(verb, "set")) { HOST_RET(0, 4); return; }
    if (!strcasecmp(verb, "status") && n >= 3) {
        char out[64] = "";
        if (!strcasecmp(tok[2], "mode")) snprintf(out, sizeof out, "%s", s->playing ? (s->paused ? "paused" : "playing") : "stopped");
        else if (!strcasecmp(tok[2], "position")) snprintf(out, sizeof out, "%u", (unsigned)(s->pos * 1000.0 / s->rate));
        else if (!strcasecmp(tok[2], "length")) snprintf(out, sizeof out, "%u", (unsigned)((uint64_t)total_frames(s) * 1000 / s->rate));
        else if (!strcasecmp(tok[2], "ready")) snprintf(out, sizeof out, "true");
        else if (!strcasecmp(tok[2], "number")) snprintf(out, sizeof out, "1");
        ret_str(retbuf, retlen, out);
        HOST_RET(0, 4);
        return;
    }
    host_log("MCI: 未处理的命令 \"%s\"", cmd);
    HOST_RET(0, 4);
}

API_FN(mciGetErrorStringA) { ret_str(ARG(1), ARG(2), "MCI error"); HOST_RET(1, 3); }

void api_register_dsound(void) {
    static const ApiEntry t[] = { { "#1", api_DirectSoundCreate_ord1 } };
    api_register(t, 1);
}
void api_register_winmm(void) {
    static const ApiEntry t[] = { API_ENTRY(mciSendStringA), API_ENTRY(mciGetErrorStringA) };
    api_register(t, 2);
}
