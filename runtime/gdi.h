/* 软件 GDI：对 DirectDraw 表面（或无目标的窗口 DC）做文字/线条/像素绘制。 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *bits;            /* 宿主指针，指向像素首地址；为空表示没有可绘制目标 */
    int w, h, pitch, bpp;
    uint32_t rmask, gmask, bmask;
    const uint32_t *pal;      /* 8bpp: 256 项 0x00RRGGBB；其他: NULL */
} GdiTarget;

uint32_t gdi_new_dc(const GdiTarget *t);
void gdi_free_dc(uint32_t dc);
/* 窗口 DC 的目标可在显示模式确定后补上 */
void gdi_set_window_target(const GdiTarget *t);
uint32_t gdi_window_dc(void);
int gdi_is_dc(uint32_t h);

/* 系统调色板（8bpp 模式下 GetSystemPaletteEntries 使用） */
const uint32_t *gdi_system_palette(void);

/* 字体 */
int gdi_font_init(void);

#ifdef __cplusplus
}
#endif
