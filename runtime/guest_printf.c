/* wsprintfA / wvsprintfA：从来宾的参数区读取可变参数的 printf 子集。 */
#include "host.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* argptr 为来宾地址，指向第一个可变参数（每个参数占 4 字节，64 位值占 8 字节） */
int guest_vsprintf(char *out, size_t outsz, const char *fmt, uint32_t argptr) {
    size_t o = 0;
#define PUT(c) do { if (o + 1 < outsz) out[o] = (char)(c); o++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        p++;
        if (*p == '%') { PUT('%'); continue; }
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
            else break;
        }
        int width = 0, prec = -1;
        if (*p == '*') { width = (int)rt_rd32(g_mem, argptr); argptr += 4; p++; if (width < 0) { left = 1; width = -width; } }
        else while (isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0');
        if (*p == '.') {
            p++; prec = 0;
            if (*p == '*') { prec = (int)rt_rd32(g_mem, argptr); argptr += 4; p++; }
            else while (isdigit((unsigned char)*p)) prec = prec * 10 + (*p++ - '0');
        }
        int lng = 0, shrt = 0;
        while (*p == 'l' || *p == 'h' || *p == 'I') {
            if (*p == 'l') lng++; else if (*p == 'h') shrt = 1;
            p++;
            if (*p == '6' && p[1] == '4') { lng = 2; p += 2; }
        }
        char tmp[80];
        const char *s = tmp;
        int len = 0;
        char c = *p;
        if (!c) break;
        switch (c) {
        case 'd': case 'i': {
            int32_t v = (int32_t)rt_rd32(g_mem, argptr); argptr += 4;
            if (shrt) v = (int16_t)v;
            int neg = v < 0;
            uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
            char d[16]; int n = 0;
            do { d[n++] = (char)('0' + u % 10); u /= 10; } while (u);
            while (n < prec) d[n++] = '0';
            int k = 0;
            if (neg) tmp[k++] = '-'; else if (plus) tmp[k++] = '+'; else if (space) tmp[k++] = ' ';
            while (n) tmp[k++] = d[--n];
            len = k;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint32_t v = rt_rd32(g_mem, argptr); argptr += 4;
            if (shrt) v &= 0xFFFF;
            char d[16]; int n = 0;
            unsigned base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do { d[n++] = dig[v % base]; v /= base; } while (v);
            while (n < prec) d[n++] = '0';
            int k = 0;
            if (alt && base == 16) { tmp[k++] = '0'; tmp[k++] = c; }
            while (n) tmp[k++] = d[--n];
            len = k;
            break;
        }
        case 'c': {
            tmp[0] = (char)rt_rd32(g_mem, argptr); argptr += 4;
            len = 1;
            break;
        }
        case 's': {
            uint32_t sp = rt_rd32(g_mem, argptr); argptr += 4;
            s = sp ? GSTR(sp) : "(null)";
            len = (int)strlen(s);
            if (prec >= 0 && len > prec) len = prec;
            break;
        }
        case 'p': {
            uint32_t v = rt_rd32(g_mem, argptr); argptr += 4;
            len = snprintf(tmp, sizeof tmp, "%08X", v);
            break;
        }
        default:
            tmp[0] = c; len = 1;
        }
        int pad = width > len ? width - len : 0;
        if (!left && pad) {
            if (zero && c != 's' && c != 'c') {
                int k = 0;
                if (len && (s[0] == '-' || s[0] == '+')) { PUT(s[0]); k = 1; }
                for (int i = 0; i < pad; i++) PUT('0');
                for (; k < len; k++) PUT(s[k]);
                continue;
            }
            for (int i = 0; i < pad; i++) PUT(' ');
        }
        for (int k = 0; k < len; k++) PUT(s[k]);
        if (left) for (int i = 0; i < pad; i++) PUT(' ');
    }
    if (outsz) out[o < outsz ? o : outsz - 1] = 0;
    return (int)o;
#undef PUT
}
