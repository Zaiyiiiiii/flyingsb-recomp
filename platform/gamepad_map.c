#include "gamepad_map.h"
#include "platform.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
/* 网页版：手柄由页面脚本直接读浏览器的 Gamepad API（标准布局，见 web/shell.html 的 PAD_MAP），
 * 不经过 SDL 的重新映射——它在浏览器里会把按钮错位。这里全部留空，避免双重输入。 */
void gp_init(void) {}
void gp_shutdown(void) {}
void gp_handle_event(const SDL_Event *e) { (void)e; }
void gp_update(void) {}
int gp_connected(void) { return 1; }
#else

/* 默认映射：按钮 -> Win32 虚拟键码。可通过用户目录下的 gamepad.ini 覆盖：
 *   每行  按钮名=VK十六进制，例如  south=0D
 * 特殊值 1000 表示"切换全屏"动作。 */
#define ACT_FULLSCREEN 0x1000

typedef struct { SDL_GamepadButton btn; const char *name; int vk; } Bind;

static Bind binds[] = {
    { SDL_GAMEPAD_BUTTON_DPAD_UP,    "dpad_up",    0x26 },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN,  "dpad_down",  0x28 },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT,  "dpad_left",  0x25 },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "dpad_right", 0x27 },
    { SDL_GAMEPAD_BUTTON_SOUTH,      "south",      0x0D },   /* A: 确认 */
    { SDL_GAMEPAD_BUTTON_EAST,       "east",       0x1B },   /* B: 取消 */
    { SDL_GAMEPAD_BUTTON_WEST,       "west",       0x20 },   /* X: 空格 */
    { SDL_GAMEPAD_BUTTON_NORTH,      "north",      0x4D },   /* Y: M 键（功能未确认） */
    { SDL_GAMEPAD_BUTTON_START,      "start",      0x1B },   /* Esc: 菜单 */
    { SDL_GAMEPAD_BUTTON_BACK,       "back",       0x0D },   /* Enter */
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  "lshoulder", 0x43 },  /* C: 切换队伍（反向） */
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "rshoulder", 0x44 },  /* D: 魔塔里切换两组队员 */
#ifdef __ANDROID__
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK,    "rstick",    0 },                /* Android 始终全屏，R3 不用 */
#else
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK,    "rstick",    ACT_FULLSCREEN },   /* 右摇杆按下: 全屏 */
#endif
};
#define NBINDS ((int)(sizeof binds / sizeof binds[0]))

#define MAX_PADS 4
static SDL_Gamepad *pads[MAX_PADS];
static int axis_dir[4];     /* 摇杆合成的方向键状态: 0左 1右 2上 3下 */
static const int dir_vk[4] = { 0x25, 0x27, 0x26, 0x28 };
static int16_t lx[MAX_PADS], ly[MAX_PADS];
static int vk_count[256];  /* 多个来源按下同一个键时的计数 */
static uint8_t hat_dir[MAX_PADS][4];         /* 十字帽(hat)上一帧的方向: 0左 1右 2上 3下 */
static uint8_t btn_down[MAX_PADS][NBINDS];   /* 每个按钮上一帧是否按着 */
static int btn_vk[MAX_PADS][NBINDS];         /* 按下时实际发出的键（松开时要对应松开它） */

static void press(int vk, int down) {
    if (vk <= 0 || vk >= 256) return;
    if (down) { if (vk_count[vk]++ == 0) ev_key(vk, 1); }
    else if (vk_count[vk] > 0) { if (--vk_count[vk] == 0) ev_key(vk, 0); }
}

/* 片头视频：原版只认 Esc 跳过。手柄上播放视频时，所有常用按钮都当作 Esc */
static int is_skip_button(SDL_GamepadButton b) {
    return b == SDL_GAMEPAD_BUTTON_SOUTH || b == SDL_GAMEPAD_BUTTON_EAST || b == SDL_GAMEPAD_BUTTON_WEST ||
           b == SDL_GAMEPAD_BUTTON_NORTH || b == SDL_GAMEPAD_BUTTON_START || b == SDL_GAMEPAD_BUTTON_BACK;
}

static void load_config(void) {
    char path[1024];
    snprintf(path, sizeof path, "%sgamepad.ini", plat_pref_dir());
    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* 写出一份带注释的默认配置，方便用户修改 */
        fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "; 手柄映射：按钮名=Win32虚拟键码(十六进制)。例如 0D=Enter 1B=Esc 20=空格 25..28=方向键；1000=切换全屏\n");
            for (int i = 0; i < NBINDS; i++) fprintf(fp, "%s=%02X\n", binds[i].name, binds[i].vk);
            fclose(fp);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == ';' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        for (int i = 0; i < NBINDS; i++) if (!strcmp(binds[i].name, line)) binds[i].vk = (int)strtol(eq + 1, NULL, 16);
    }
    fclose(fp);
}

void gp_init(void) { load_config(); }

void gp_shutdown(void) {
    for (int i = 0; i < MAX_PADS; i++) if (pads[i]) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; }
}

static int pad_slot(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++) if (pads[i] && SDL_GetGamepadID(pads[i]) == id) return i;
    return -1;
}

/* 松开某个手柄上所有按着的键（拔出或失去时） */
static void release_pad(int s) {
    for (int i = 0; i < NBINDS; i++) {
        if (btn_down[s][i]) { if (btn_vk[s][i] != ACT_FULLSCREEN) press(btn_vk[s][i], 0); btn_down[s][i] = 0; }
    }
    for (int d = 0; d < 4; d++) { if (hat_dir[s][d]) { press(dir_vk[d], 0); hat_dir[s][d] = 0; } }
    lx[s] = ly[s] = 0;
}

void gp_handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        for (int i = 0; i < MAX_PADS; i++) if (!pads[i]) {
            pads[i] = SDL_OpenGamepad(e->gdevice.which);
            if (pads[i]) {
                SDL_Joystick *j = SDL_GetGamepadJoystick(pads[i]);
                host_log("手柄接入: %s 按钮=%d 轴=%d 十字帽=%d", SDL_GetGamepadName(pads[i]) ? SDL_GetGamepadName(pads[i]) : "?",
                         SDL_GetNumJoystickButtons(j), SDL_GetNumJoystickAxes(j), SDL_GetNumJoystickHats(j));
            }
            break;
        }
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
        /* 接入了但没有 SDL 现成按键映射的手柄不会成为 gamepad，游戏收不到输入；记下型号和 GUID，便于补映射 */
        if (!SDL_IsGamepad(e->jdevice.which)) {
            char guid[40];
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(e->jdevice.which), guid, sizeof guid);
            const char *nm = SDL_GetJoystickNameForID(e->jdevice.which);
            host_log("手柄接入但没有标准映射，无法使用: %s  GUID=%s", nm ? nm : "?", guid);
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int s = pad_slot(e->gdevice.which);
        if (s >= 0) { release_pad(s); SDL_CloseGamepad(pads[s]); pads[s] = NULL; }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int s = pad_slot(e->gaxis.which);
        if (s < 0) break;
        if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) lx[s] = e->gaxis.value;
        else if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) ly[s] = e->gaxis.value;
        break;
    }
    default: break;   /* 按钮不看事件，改成在 gp_update 里直接读状态 */
    }
}

int gp_connected(void) {
    for (int i = 0; i < MAX_PADS; i++) if (pads[i]) return 1;
    return 0;
}

void gp_update(void) {
    /* 按钮：每帧直接读当前状态，比较上一帧得到按下/松开，不会因事件丢失或类型差异而漏掉十字键 */
    for (int s = 0; s < MAX_PADS; s++) {
        if (!pads[s]) continue;
        for (int i = 0; i < NBINDS; i++) {
            int d = SDL_GetGamepadButton(pads[s], binds[i].btn) ? 1 : 0;
            if (d && !btn_down[s][i]) {
                btn_down[s][i] = 1;
                int vk = binds[i].vk;
                if (g_video_playing && is_skip_button(binds[i].btn)) vk = 0x1B;
                btn_vk[s][i] = vk;
                if (vk == ACT_FULLSCREEN) plat_toggle_fullscreen(); else press(vk, 1);
            } else if (!d && btn_down[s][i]) {
                btn_down[s][i] = 0;
                if (btn_vk[s][i] != ACT_FULLSCREEN) press(btn_vk[s][i], 0);
            }
        }
    }

    /* 十字帽：不少手柄的十字键是以 hat 上报的，SDL 手柄映射里不一定有对应按钮，所以也直接读 hat */
    for (int s = 0; s < MAX_PADS; s++) {
        if (!pads[s]) continue;
        SDL_Joystick *j = SDL_GetGamepadJoystick(pads[s]);
        uint8_t h = 0;
        for (int k = 0, n = SDL_GetNumJoystickHats(j); k < n; k++) h |= SDL_GetJoystickHat(j, k);
        uint8_t now[4] = { (h & SDL_HAT_LEFT) != 0, (h & SDL_HAT_RIGHT) != 0, (h & SDL_HAT_UP) != 0, (h & SDL_HAT_DOWN) != 0 };
        for (int d = 0; d < 4; d++) {
            if (now[d] != hat_dir[s][d]) { hat_dir[s][d] = now[d]; press(dir_vk[d], now[d]); }
        }
    }

    int want[4] = {0, 0, 0, 0};
    for (int s = 0; s < MAX_PADS; s++) {
        if (!pads[s]) continue;
        /* 滞回死区：按下阈值 0.5，释放阈值 0.3 */
        int on = 16384, off = 9830;
        want[0] |= lx[s] < (axis_dir[0] ? -off : -on);
        want[1] |= lx[s] > (axis_dir[1] ? off : on);
        want[2] |= ly[s] < (axis_dir[2] ? -off : -on);
        want[3] |= ly[s] > (axis_dir[3] ? off : on);
    }
    for (int d = 0; d < 4; d++) {
        if (want[d] != axis_dir[d]) {
            axis_dir[d] = want[d];
            press(dir_vk[d], want[d]);
        }
    }
}
#endif   /* !__EMSCRIPTEN__ */
