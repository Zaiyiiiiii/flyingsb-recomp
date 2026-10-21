/* 触屏虚拟按键（仅 Android）：没有手柄接入时，在画面上叠加方向盘和按钮，触摸映射成原版的键盘输入。 */
#pragma once
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void tp_event(const SDL_Event *e);        /* 处理 SDL_EVENT_FINGER_*；其他事件忽略 */
void tp_draw(SDL_Renderer *ren);          /* 在已绘制好的游戏画面上叠加按键，调用后再 RenderPresent */

#ifdef __cplusplus
}
#endif
