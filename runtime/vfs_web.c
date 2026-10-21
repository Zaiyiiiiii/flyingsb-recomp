/* Web 版游戏目录后端：游戏数据不在本地磁盘，而是由 Rust 服务器（server/）提供。
 * - 启动时取 /api/manifest（每行 "大小\t相对路径"），用于不区分大小写的路径匹配；
 * - 读文件用同步 XHR + Range 请求（运行在 pthread 工作线程里，同步 XHR 在 Worker 中可用），
 *   再包装成 FILE*，让原有的 fread/fseek 代码无需改动。 */
#define _GNU_SOURCE   /* fopencookie；必须先于任何系统头文件 */
#ifdef __EMSCRIPTEN__
#include <stdio.h>
#include "host.h"
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

EM_JS(int, js_http_range, (const char *rel, double off, int len, uint8_t *dst), {
    const parts = UTF8ToString(rel).split('/').map(encodeURIComponent).join('/');
    const x = new XMLHttpRequest();
    x.open('GET', self.location.origin + '/game/' + parts, false);
    x.responseType = 'arraybuffer';
    x.setRequestHeader('Range', 'bytes=' + off + '-' + (off + len - 1));
    try { x.send(null); } catch (e) { return -1; }
    if (x.status !== 206 && x.status !== 200) return -1;
    let b = new Uint8Array(x.response);
    if (x.status === 200) b = b.subarray(off, off + len);   /* 服务器忽略了 Range */
    HEAPU8.set(b, dst);
    return b.length;
});

EM_JS(int, js_http_text, (const char *url, char *dst, int cap), {
    const x = new XMLHttpRequest();
    x.open('GET', self.location.origin + UTF8ToString(url), false);
    try { x.send(null); } catch (e) { return -1; }
    if (x.status !== 200) return -1;
    return stringToUTF8(x.responseText, dst, cap);
});

typedef struct { char *path; char *lower; int64_t size; } MEntry;
static MEntry *man;
static int man_n;

int web_manifest_load(void) {
    const int cap = 1 << 20;
    char *buf = (char *)malloc(cap);
    int n = js_http_text("/api/manifest", buf, cap);
    if (n < 0) { free(buf); return 0; }
    int count = 0;
    for (char *p = buf; *p;) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        char *tab = strchr(p, '\t');
        if (tab) {
            *tab = 0;
            man = (MEntry *)realloc(man, (man_n + 1) * sizeof(MEntry));
            MEntry *e = &man[man_n++];
            e->size = strtoll(p, NULL, 10);
            e->path = strdup(tab + 1);
            e->lower = strdup(tab + 1);
            for (char *q = e->lower; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
            count++;
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(buf);
    return count;
}

/* 不区分大小写查找；out 得到服务器上的真实相对路径 */
int web_manifest_find(const char *rel, char *out, size_t outsz) {
    char low[1024];
    size_t i = 0;
    for (; rel[i] && i + 1 < sizeof low; i++) low[i] = (rel[i] >= 'A' && rel[i] <= 'Z') ? rel[i] + 32 : rel[i];
    low[i] = 0;
    for (int k = 0; k < man_n; k++)
        if (strcmp(man[k].lower, low) == 0) { snprintf(out, outsz, "%s", man[k].path); return 1; }
    return 0;
}

static int64_t manifest_size(const char *actual) {
    for (int k = 0; k < man_n; k++) if (strcmp(man[k].path, actual) == 0) return man[k].size;
    return -1;
}

#define BLK (256 * 1024)
typedef struct {
    char rel[1024];
    int64_t size, pos;
    uint8_t *blk;
    int64_t blk_off;
    int blk_len;
} Remote;

static ssize_t rm_read(void *c, char *buf, size_t n) {
    Remote *r = (Remote *)c;
    size_t done = 0;
    while (done < n && r->pos < r->size) {
        if (!r->blk || r->pos < r->blk_off || r->pos >= r->blk_off + r->blk_len) {
            if (!r->blk) r->blk = (uint8_t *)malloc(BLK);
            int64_t want = r->size - r->pos;
            if (want > BLK) want = BLK;
            int got = js_http_range(r->rel, (double)r->pos, (int)want, r->blk);
            if (got <= 0) return done ? (ssize_t)done : -1;
            r->blk_off = r->pos; r->blk_len = got;
        }
        size_t avail = (size_t)(r->blk_off + r->blk_len - r->pos);
        size_t k = n - done < avail ? n - done : avail;
        memcpy(buf + done, r->blk + (r->pos - r->blk_off), k);
        r->pos += (int64_t)k; done += k;
    }
    return (ssize_t)done;
}

static int rm_seek(void *c, off_t *off, int whence) {
    Remote *r = (Remote *)c;
    int64_t np = whence == SEEK_SET ? *off : whence == SEEK_CUR ? r->pos + *off : r->size + *off;
    if (np < 0) return -1;
    r->pos = np; *off = (off_t)np;
    return 0;
}

static int rm_close(void *c) {
    Remote *r = (Remote *)c;
    free(r->blk); free(r);
    return 0;
}

FILE *web_remote_open(const char *actual_rel) {
    int64_t sz = manifest_size(actual_rel);
    if (sz < 0) return NULL;
    Remote *r = (Remote *)calloc(1, sizeof *r);
    snprintf(r->rel, sizeof r->rel, "%s", actual_rel);
    r->size = sz;
    cookie_io_functions_t io = { rm_read, NULL, rm_seek, rm_close };
    FILE *f = fopencookie(r, "rb", io);
    if (!f) { free(r); return NULL; }
    return f;
}
#endif
