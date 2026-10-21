/* 平台抽象层：窗口、输入、音频、计时。由 platform/ 下的后端(SDL3)实现。 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生命周期 ---- */
int plat_init(void);
void plat_shutdown(void);
void plat_error_box(const char *title, const char *msg);
/* type: 0=MB_OK 1=MB_OKCANCEL 4=MB_YESNO；返回 Win32 的 IDOK(1)/IDCANCEL(2)/IDYES(6)/IDNO(7) */
int plat_message_box(const char *title, const char *msg, int type);

/* ---- 显示 ---- */
int plat_create_window(int w, int h, const char *title);
void plat_set_title(const char *title);
/* 提交一帧：32 位 0x00RRGGBB 像素，pitch 为每行字节数。会缩放并保持比例。 */
void plat_present(const uint32_t *pixels, int w, int h, int pitch);
void plat_show_cursor(int show);
void plat_set_cursor_pos(int x, int y);     /* 逻辑(游戏)坐标 */
void plat_get_cursor_pos(int *x, int *y);   /* 逻辑(游戏)坐标 */
void plat_set_fullscreen(int on);

/* ---- 事件 ---- */
void plat_pump_events(void);                /* 抽取系统事件并回调下面的 ev_* */
void plat_toggle_fullscreen(void);          /* 切换全屏（手柄按钮、F11、Alt+Enter 共用） */

/* 由 user32 层实现，供平台后端投递输入 */
void ev_key(int vk, int down);
void ev_char(uint32_t ch);
void ev_mouse_move(int x, int y);
void ev_mouse_button(int button, int down, int x, int y);   /* 0=左 1=右 2=中 */
void ev_mouse_wheel(int delta);
void ev_activate(int active);
void ev_release_keys(void);                 /* 失去焦点时：把所有按着的键当作已松开，避免回来后卡键 */
void ev_close(void);

/* ---- 计时 ---- */
uint64_t plat_ticks_ms(void);
void plat_sleep_ms(uint32_t ms);

/* ---- 音频 ---- */
typedef void (*PlatAudioFn)(int16_t *out, int frames);
int plat_audio_open(int rate, int channels, PlatAudioFn cb);   /* 立体声 s16 */
void plat_audio_close(void);
void plat_audio_lock(void);
void plat_audio_unlock(void);

/* ---- 路径 ---- */
const char *plat_pref_dir(void);            /* 每用户的可写目录（存档/配置） */
const char *plat_exe_dir(void);             /* 可执行文件所在目录（结尾带分隔符），取不到返回 NULL */
float plat_display_aspect(void);            /* 屏幕（Web 为浏览器窗口）的宽高比，取不到时为 0；没配置画面比例时据此匹配屏幕 */
const char *plat_base_dir(void);            /* 可执行文件所在目录（以斜杠结尾），用于找随程序放置的 fonts/ */

#ifdef __cplusplus
}
#endif
