/* 代码页转换：CP950(繁体中文 Big5) 与 ASCII/Latin-1。 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 从 s 解码一个字符，返回消耗的字节数（>=1） */
int cp_mb_to_wc(uint32_t cp, const uint8_t *s, int avail, uint16_t *out);
/* 把一个 UTF-16 码元编码为多字节，返回字节数（1 或 2）；找不到时写入 '?' 并置位 *used_default */
int cp_wc_to_mb(uint32_t cp, uint16_t w, uint8_t *out, int *used_default);

#ifdef __cplusplus
}
#endif
