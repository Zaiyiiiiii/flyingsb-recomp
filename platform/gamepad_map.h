/* 现代手柄 -> 原版键盘/鼠标输入的映射（基于 SDL3 Gamepad API，支持 Xbox/PS/Switch/通用手柄）。 */
#pragma once
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gp_init(void);
void gp_shutdown(void);
void gp_handle_event(const SDL_Event *e);
void gp_update(void);   /* 每帧调用：摇杆死区/长按重复等 */
int gp_connected(void); /* 是否有手柄接入（Android 据此决定是否显示屏幕虚拟按键） */

#ifdef __cplusplus
}
#endif
