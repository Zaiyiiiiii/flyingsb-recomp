/* AVI 解复用 + Cinepak / MS Video 1 视频解码 (无外部依赖) */
#pragma once
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AviFile AviFile;

typedef struct {
    uint32_t type;        /* 'vids' / 'auds' */
    uint32_t handler;     /* fccHandler */
    uint32_t scale, rate; /* 帧率 = rate/scale */
    uint32_t start, length;
    uint32_t sample_size;
    uint32_t suggested;
    int idx;              /* 流序号 */
    int fmt_len;
    uint8_t *fmt;         /* strf 内容 (BITMAPINFO 或 WAVEFORMATEX) */
    int width, height, bitcount;
} AviStreamInfo;

AviFile *avi_open(const char *host_path);
void avi_close(AviFile *f);
int avi_stream_count(AviFile *f);
const AviStreamInfo *avi_stream(AviFile *f, int i);

/* 视频：解码第 n 帧到 DIB（自底向上），bpp 为 8/16/24/32；返回行距。
 * out 至少 stride*height 字节。palette_out 在 8bpp 时填入 256 项 0x00RRGGBB。 */
int avi_decode_frame(AviFile *f, int stream, int n, int bpp, uint8_t *out, int *stride, uint32_t *palette_out);
/* 音频：按"样本块"读取，返回字节数 */
int avi_read_audio(AviFile *f, int stream, uint32_t start_sample, uint32_t nsamples, uint8_t *buf, uint32_t bufsize, uint32_t *samples_read);
int avi_dib_stride(int width, int bpp);
uint32_t avi_stream_samples(AviFile *f, int stream);   /* 视频=帧数，音频=样本块数 */

#ifdef __cplusplus
}
#endif
