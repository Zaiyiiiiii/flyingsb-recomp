/* 对生成代码的手写补丁（地址见 recomp/hooks.txt）：
 * 在游戏主菜单里新增一个"遊戲設定"按钮，进去是一页原版样式的菜单，选择画面比例 4:3 / 16:9。
 *
 * 主菜单(0x41FB36)是个状态机页面：按钮是 7 个"控件块"(0x7B8F40 起，每块 32 字节) + 可导航条目表(0x54CBB0 起，
 * 条目是页面编号)，每个按钮对应一个光标矩形(函数局部数组，每按钮 16 字节)。原版 6 个按钮 + 顶部的标题块；
 * 这里把标题块改成第 7 个按钮，次序为 …系統環境、遊戲設定、結束。
 *
 * 子菜单页复用"系統環境設定"页(页面 10，0x42FFD6)的框架：它是个按状态运行的页面函数
 * （1=创建并绘制 10/15=打开动画 20=交互 100=关闭动画 110=收尾）。从"遊戲設定"进入时，创建和交互两个状态
 * 换成我们的（内容、光标、按键），打开/关闭动画和收尾沿用原版，所以动画、音效、字体、面板样式都是原版的。 */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_SLOTS      0x54CBB0u   /* 可导航条目表，每项 4 字节（页面编号） */
#define MENU_COUNT      0x54CBD8u   /* 可导航条目数（原为 6） */
#define MENU_PANEL_H    0x54CBACu   /* 面板高度（原为 160） */
#define MENU_BLOCKS     0x7B8F40u   /* 控件块表 */
#define MENU_ROW        0x14        /* 按钮间距 */
#define GAMESET_SLOT    5           /* "遊戲設定"在菜单里的序号 */
#define PAGE_SYSTEM     10          /* "系統環境"页面编号，子菜单借用它 */
#define PAGE_EXIT       11

static uint32_t rd(uint32_t a) { return rt_rd32(g_mem, a); }
static void wr(uint32_t a, uint32_t v) { rt_wr32(g_mem, a, v); }
static uint32_t call(uint32_t addr, int n, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    uint32_t args[5] = { a0, a1, a2, a3, a4 };
    return rt_guest_call(addr, n, args);
}

static uint32_t label_gameset;      /* "遊戲設定"（Big5，来宾内存） */

/* 来宾内存里放一份 Big5 文字（只创建一次） */
static uint32_t gstr(const uint8_t *big5, size_t n) {
    char tmp[64];
    memcpy(tmp, big5, n);
    tmp[n] = 0;
    return gm_strdup(tmp);
}

/* ---------------------------------------------------------------- 主菜单 */

/* 每次执行主菜单页面函数、进入状态分派之前调用（0x41FE5C）：原版的一次性初始化已经完成，这里改写它的结果。
 * 改写是幂等的，重复执行无害。 */
void hook_menu_tables(void) {
    if (!label_gameset) {
        static const uint8_t t[] = { 0xB9, 0x43, 0xC0, 0xB8, 0xB3, 0x5D, 0xA9, 0x77 };   /* 遊戲設定 */
        label_gameset = gstr(t, sizeof t);
    }
    wr(MENU_SLOTS + 4 * GAMESET_SLOT, PAGE_SYSTEM);   /* 条目 5：遊戲設定（和"系統環境"一样打开页面 10，见 hook_sys_page） */
    wr(MENU_SLOTS + 4 * 6, PAGE_EXIT);                /* 条目 6：結束（原来在 5） */
    wr(MENU_SLOTS + 4 * 7, 9);                        /* 条目 7：原来排在 6 的标题条目，仍不可选 */
    wr(MENU_COUNT, 7);                                /* 可选按钮 6 → 7 个 */
    wr(MENU_PANEL_H, 160 + MENU_ROW);                 /* 只多一行：面板加高一个按钮间距，其余边距不变 */
}

/* 画菜单时的控件块表（0x4201F2 的 push 0x7B8F40 之后调用）：原表保持不动（原版每帧会往里写按钮颜色），
 * 另组一张 9 块的表给绘制函数：原来的 0..4 号按钮、"遊戲設定"、"結束"（比原位置下移一格）、"選單"标题、结束标记。
 * 按钮颜色（不可选时变灰）随原表对应序号走：序号 5、6 现在分别是"遊戲設定"、"結束"。 */
void hook_menu_draw(void) {
    static uint32_t tbl;
    if (!tbl) tbl = gm_calloc(9 * 32);
    for (int i = 0; i < 5; i++) memcpy(g_mem + tbl + 32 * i, g_mem + MENU_BLOCKS + 32 * i, 32);
    /* 5 号：遊戲設定 —— 占原来"結束"的位置和样式 */
    uint32_t o5 = MENU_BLOCKS + 5 * 32, o6 = MENU_BLOCKS + 6 * 32, n5 = tbl + 5 * 32, n6 = tbl + 6 * 32;
    memcpy(g_mem + n5, g_mem + o5, 32);
    wr(n5 + 0x1C, label_gameset);
    /* 6 号：結束 —— 同样式，下移一格；颜色取原表 6 号（原版按序号 6 写入的）*/
    memcpy(g_mem + n6, g_mem + o5, 32);
    wr(n6 + 0x04, rd(o5 + 0x04) + MENU_ROW);
    wr(n6 + 0x0C, rd(o5 + 0x0C) + MENU_ROW);
    wr(n6 + 0x14, rd(o6 + 0x14));
    wr(n6 + 0x18, rd(o6 + 0x18));
    wr(n6 + 0x1C, 0x54E528u);                         /* 原版的"結束"字符串 */
    /* 7 号：標題"選單"，位置、样式、文字都还是原来的；颜色固定为原版标题色 */
    memcpy(g_mem + tbl + 7 * 32, g_mem + o6, 32);
    wr(tbl + 7 * 32 + 0x14, rd(0x53C75Cu));
    wr(tbl + 7 * 32 + 0x18, rd(0x53C760u));
    /* 8 号：结束标记 */
    memcpy(g_mem + tbl + 8 * 32, g_mem + MENU_BLOCKS + 7 * 32, 32);
    wr(G.esp, tbl);                                   /* 把刚压栈的表指针换成我们的 */
}

/* 光标矩形(局部数组)算好之后调用（0x41FF9E）：第 5、6 个按钮的光标框 */
void hook_menu_cursor(void) {
    uint32_t ebp = G.ebp;
    int32_t w = (int32_t)rd(0x54CBA8u);
    int32_t cx = w / 2;
    wr(ebp - 0x24, (uint32_t)(cx - 0x2C)); wr(ebp - 0x20, 0x81); wr(ebp - 0x1C, (uint32_t)(cx + 0x2C)); wr(ebp - 0x18, 0x97);   /* 遊戲設定 */
    wr(ebp - 0x14, (uint32_t)(cx - 0x13)); wr(ebp - 0x10, 0x95); wr(ebp - 0x0C, (uint32_t)(cx + 0x13)); wr(ebp - 0x08, 0xAB);   /* 結束 */
}

static int gameset_mode;            /* 本次打开的"系統環境"页其实是"遊戲設定" */

/* 主菜单里按下确认（0x42077C）：记下选中的是不是"遊戲設定"。原有逻辑照常执行：菜单关闭并打开页面 10 */
void hook_menu_confirm(void) {
    uint32_t page = rd(G.ebp + 8);
    gameset_mode = rd(page + 0x1A0) == GAMESET_SLOT;
}

/* ---------------------------------------------------------------- 子菜单页 */
/* 版式参考"系統環境設定"页的“基本移動模式 …… 走/快步走”那一行：标签在左，当前值在右，左右键切换。
 * 面板收窄到够用即可：标题居中，左右边距一致(MARGIN)，光标框横跨整行，下面一行小提示，上下边距对称 */
#define PANEL_W   300
#define MARGIN    24                /* 标签/值离面板左右边的距离 */
#define TITLE_Y   0x1E              /* 标题中心（沿用原页标题的高度和字体） */
#define ROW_Y     0x52              /* 选项行中心 */
#define ROW_STEP  0x1C              /* 行距 */
#define NOTE_Y    (ROW_Y + ROW_STEP + 0x28)   /* 提示行中心 */
#define PANEL_H   (NOTE_Y + 0x0B + 0x12)
#define ROWS      2
#define POS_TABLE 0x7B95A0u         /* 系統環境页的光标位置表（每项 16 字节：x1 y1 x2 y2），我们临时借用第 0 项 */

static uint32_t table_addr;         /* 我们的文字表（来宾内存） */
static uint32_t val_buf;            /* 第 1 行右侧"当前值"的文字缓冲（来宾内存），切换时改写 */
static uint32_t val_buf2;           /* 第 2 行的 */
static uint8_t saved_pos[16 * ROWS];   /* 被借用的光标位置表前几项的原值 */
static int cur_ratio;               /* 0 = 4:3  1 = 16:9 */
static int cur_row;                 /* 光标所在行 */
static int enc_loaded, enc_on = 1;  /* 随机遇敌开关，存在 FlyingSB.INI 的 [ENVIRONMENT] random_encounter */

static int encounter_on(void) {
    if (!enc_loaded) {
        char v[16];
        enc_loaded = 1;
        if (host_ini_get("ENVIRONMENT", "random_encounter", v, sizeof v)) enc_on = atoi(v) != 0;
    }
    return enc_on;
}

/* 随机遇敌判定函数(0x49CEA2)的函数体入口：关闭时直接跳到"未遇敌"的出口(返回 0) */
int hook_encounter(void) { return !encounter_on(); }

/* 文字表项(0x18 字节)：x, y, 字体号, 颜色A, 颜色B, 文字；x 是左边起点，y 是行中心，相对面板左上角 */
static void put_entry(uint32_t e, int x, int y, int font, uint32_t ca, uint32_t cb, uint32_t text) {
    wr(e + 0, (uint32_t)x); wr(e + 4, (uint32_t)y); wr(e + 8, (uint32_t)font);
    wr(e + 12, ca); wr(e + 16, cb); wr(e + 20, text);
}

/* 右侧的值："4:3" / "16:9"（西文半角每字 8 像素宽）。右对齐到 PANEL_W - MARGIN，保证整个文字都在光标框里 */
static void set_value_text(void) {
    static const uint8_t on[] = { 0xB6, 0x7D, 0 }, off[] = { 0xC3, 0xF6, 0 };   /* 開 / 關 */
    const char *s = cur_ratio ? "16:9" : "4:3";
    size_t n = strlen(s);
    memcpy(g_mem + val_buf, s, n + 1);
    wr(table_addr + 0x30 + 0, (uint32_t)(PANEL_W - MARGIN - 8 * (int)n));
    memcpy(g_mem + val_buf2, enc_on ? on : off, 3);
    wr(table_addr + 0x60 + 0, (uint32_t)(PANEL_W - MARGIN - 16));   /* 全角字 16 像素宽 */
}

static void build_table(void) {
    /* Big5 文字 */
    static const uint8_t title[] = { 0xB9, 0x43, 0xC0, 0xB8, 0xB3, 0x5D, 0xA9, 0x77 };                                   /* 遊戲設定 */
    static const uint8_t label[] = { 0xB5, 0x65, 0xAD, 0xB1, 0xA4, 0xF1, 0xA8, 0xD2 };                                   /* 畫面比例 */
    static const uint8_t label2[] = { 0xC0, 0x48, 0xBE, 0xF7, 0xB9, 0x4A, 0xBC, 0xC4 };                                 /* 隨機遇敵 */
    static const uint8_t note[]  = { 0xA1, 0x5D, 0xB5, 0x65, 0xAD, 0xB1, 0xA4, 0xF1, 0xA8, 0xD2, 0xBB, 0xDD, 0xAD, 0xAB, 0xB1, 0xD2, 0xA5, 0xCD, 0xAE, 0xC4, 0xA1, 0x5E };   /* （畫面比例需重啟生效） */
    if (!table_addr) { table_addr = gm_calloc(0x18 * 7); val_buf = gm_calloc(24); val_buf2 = gm_calloc(8); }
    encounter_on();
    uint32_t na = rd(0x53C734u), nb = rd(0x53C738u);       /* 原页标题、标签、值用的都是这一对颜色 */
    /* 标题 4 个字、每字约 37 像素，水平居中 */
    put_entry(table_addr + 0x00, (PANEL_W - 4 * 37) / 2, TITLE_Y, 0x0E, na, nb, gstr(title, sizeof title));
    put_entry(table_addr + 0x18, MARGIN, ROW_Y, 4, na, nb, gstr(label, sizeof label));
    put_entry(table_addr + 0x30, 0, ROW_Y, 4, na, nb, val_buf);                         /* x 在 set_value_text 里按值的宽度定 */
    put_entry(table_addr + 0x48, MARGIN, ROW_Y + ROW_STEP, 4, na, nb, gstr(label2, sizeof label2));
    put_entry(table_addr + 0x60, 0, ROW_Y + ROW_STEP, 4, na, nb, val_buf2);
    /* 提示：22 字节 = 11 个全角字，每字 16 像素，居中 */
    put_entry(table_addr + 0x78, (PANEL_W - 11 * 16) / 2, NOTE_Y, 4, na, nb, gstr(note, sizeof note));
    wr(table_addr + 0x90, 0xFFFFFFFFu);   /* 结束标记 */
    set_value_text();
}

/* 光标框：横跨整行，比文字的左右边距再往外 8 像素，行高 0x16 */
static void set_row_rects(void) {
    for (int r = 0; r < ROWS; r++) {
        uint32_t e = POS_TABLE + 16u * (uint32_t)r;
        int y = ROW_Y + r * ROW_STEP;
        wr(e + 0, MARGIN - 8);            wr(e + 4, (uint32_t)(y - 0x0B));
        wr(e + 8, PANEL_W - MARGIN + 8);  wr(e + 12, (uint32_t)(y + 0x0B));
    }
}

/* 把光标精灵(0x7B88E8)移到第 row 项：和原版一样，传面板左上角 + 位置表矩形，16.16 定点 */
static void move_cursor(uint32_t page, int row) {
    int32_t ox = (int32_t)rd(page + 0x168) - (int32_t)rd(page + 0x178) / 2;
    int32_t oy = (int32_t)rd(page + 0x16C) - (int32_t)rd(page + 0x17C) / 2;
    uint32_t e = POS_TABLE + 16u * (uint32_t)row;
    call(rd(0x53C264u), 5, 0x7B88E8u,
         (uint32_t)(ox + (int32_t)rd(e + 0)) << 16, (uint32_t)(oy + (int32_t)rd(e + 4)) << 16,
         (uint32_t)(ox + (int32_t)rd(e + 8)) << 16, (uint32_t)(oy + (int32_t)rd(e + 12)) << 16);
    uint32_t cur = rd(0x7B8EACu);
    if (cur) wr(cur + 0x20, 0x14);
}

static void sfx(uint32_t id) { call(0x416311u, 1, id, 0, 0, 0, 0); }

/* 状态 1：创建并绘制 */
static void gameset_create(uint32_t page) {
    cur_ratio = g_res_w >= 1000 ? 1 : 0;   /* 当前生效的比例：宽屏(1066 起)算 16:9 */
    memcpy(saved_pos, g_mem + POS_TABLE, sizeof saved_pos);
    set_row_rects();
    build_table();   /* 同时按 cur_ratio 写好右侧的值文字 */
    wr(page + 0xF0, rd(0x54CA20u));
    wr(page + 0x178, PANEL_W);
    wr(page + 0x17C, PANEL_H);
    wr(page + 0x168, (uint32_t)((int32_t)rd(0x54005Cu) / 2));
    wr(page + 0x16C, (uint32_t)((int32_t)rd(0x540060u) / 2));
    wr(page + 0x180, 0x1999);
    wr(page + 0x184, 0x1999);
    cur_row = 0;
    wr(page + 0x1A0, 0);                   /* 光标从第一行开始 */
    wr(page + 0x20, 10);                   /* 下一状态：打开动画（原版处理） */
    call(0x41E3ECu, 4, page, PANEL_W, PANEL_H, table_addr, 0);   /* 画面板和文字 */
}

/* 状态 20：交互。按键来自窗口消息记录（和原版一样读这些全局变量） */
static void gameset_input(uint32_t page) {
    if (rd(0x7B8EFCu) != page || rd(0x7B8EF8u) == 0) return;
    uint32_t msg = rd(0x742CBCu), key = rd(0x742328u), lp = rd(0x742300u);
    if (msg != 0x100) return;
    int rep = (lp & 0x40000000u) != 0;      /* 按住不放产生的重复键：只有方向键接受 */
    if (!rep && (key == 0x5A || key == 0x1B || key == 0x58 || key == 0x0D || key == 0x61)) {   /* 取消 / 确认：和原页一样都是关闭 */
        sfx(0x143);
        wr(page + 0x20, 100);                                                 /* 关闭动画（原版处理），收尾回到主菜单 */
        return;
    }
    if (key == 0x26 || key == 0x68 || key == 0x28 || key == 0x62) {         /* 上 / 下：换行 */
        int row = (key == 0x26 || key == 0x68) ? (cur_row + ROWS - 1) % ROWS : (cur_row + 1) % ROWS;
        if (row != cur_row) {
            cur_row = row;
            wr(page + 0x1A0, (uint32_t)row);
            move_cursor(page, row);
            sfx(0x144);
        }
        return;
    }
    int want = -1;
    if (key == 0x25 || key == 0x64) want = 0;                                /* 左 */
    else if (key == 0x27 || key == 0x66) want = 1;                           /* 右 */
    if (want < 0) return;
    /* 和原页的"基本移動模式"一样：改完立即生效（立即保存），换右边的值文字，整页重画 */
    if (cur_row == 0 && want != cur_ratio) {
        cur_ratio = want;
        host_ini_set("VideoOption", "ScreenRatio", cur_ratio ? "16:9" : "4:3");
        host_log("画面比例已保存: %s（重启后生效）", cur_ratio ? "16:9" : "4:3");
    } else if (cur_row == 1 && want != enc_on) {
        enc_on = want;
        host_ini_set("ENVIRONMENT", "random_encounter", enc_on ? "1" : "0");
        host_log("随机遇敌: %s", enc_on ? "开" : "关");
    } else {
        return;
    }
    set_value_text();
    sfx(0x142);
    call(0x41E3ECu, 4, page, PANEL_W, PANEL_H, table_addr, 0);
}

/* 打开动画结束后原版会按页面的光标序号摆光标，位置表在 set_row_rects 里已经改好。
 * 钩子挂在"系統環境"页的状态分派之前（0x430874）：只在 gameset_mode 下接管创建(1)和交互(20)两个状态，
 * 返回非零跳到页面函数的公共出口(0x431CFE)；其余状态（打开/关闭动画、收尾）沿用原版。 */
int hook_sys_page(void) {
    if (!gameset_mode) return 0;
    uint32_t page = rd(G.ebp + 8);
    uint32_t st = rd(page + 0x20);
    if (st == 1) { gameset_create(page); return 1; }
    if (st == 20) { gameset_input(page); return 1; }
    if (st == 110) {                          /* 收尾：还原借用的位置表，之后正常的"系統環境"页不受影响 */
        memcpy(g_mem + POS_TABLE, saved_pos, sizeof saved_pos);
        gameset_mode = 0;
    }
    return 0;
}
