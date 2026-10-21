#include "codepage.h"
#include <stdlib.h>
#include <string.h>

extern const uint16_t cp950_table[126][191];

static uint16_t *rev;   /* Unicode -> 双字节码，0 表示无 */

static void build_rev(void) {
    rev = (uint16_t *)calloc(65536, sizeof(uint16_t));
    for (int l = 0; l < 126; l++)
        for (int t = 0; t < 191; t++) {
            uint16_t u = cp950_table[l][t];
            if (u && !rev[u]) rev[u] = (uint16_t)(((l + 0x81) << 8) | (t + 0x40));
        }
}

int cp_mb_to_wc(uint32_t cp, const uint8_t *s, int avail, uint16_t *out) {
    uint8_t c = s[0];
    if (cp == 950 && c >= 0x81 && c <= 0xFE && avail >= 2) {
        uint8_t t = s[1];
        if (t >= 0x40) {
            uint16_t u = cp950_table[c - 0x81][t - 0x40];
            if (u) { *out = u; return 2; }
        }
        *out = 0x003F;
        return 2;
    }
    if (cp == 950 && c >= 0x80) {
        if (c == 0x80) { *out = 0x20AC; return 1; }
        *out = 0x003F;
        return 1;
    }
    *out = c;
    return 1;
}

int cp_wc_to_mb(uint32_t cp, uint16_t w, uint8_t *out, int *used_default) {
    if (w < 0x80) { out[0] = (uint8_t)w; return 1; }
    if (cp == 950) {
        if (!rev) build_rev();
        uint16_t b = rev[w];
        if (b) { out[0] = (uint8_t)(b >> 8); out[1] = (uint8_t)b; return 2; }
    } else if (w < 0x100) {
        out[0] = (uint8_t)w;
        return 1;
    }
    out[0] = '?';
    if (used_default) *used_default = 1;
    return 1;
}
