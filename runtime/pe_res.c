/* 读取 PE 文件(DLL)中的资源。按需从文件读取，不整体载入内存。 */
#include "host.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct { uint32_t va, vsize, raw, rawsize; } Sec;

struct PeModule {
    FILE *fp;
    char *path;
    Sec secs[32];
    int nsec;
    uint8_t *rsrc;          /* 资源目录所在节的内容 */
    uint32_t rsrc_rva, rsrc_size;
};

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

const char *pe_path(PeModule *m) { return m->path; }

PeModule *pe_open(const char *host_path) {
    FILE *fp = host_fopen(host_path, "rb");
    if (!fp) return NULL;
    uint8_t hdr[1024];
    if (fread(hdr, 1, sizeof hdr, fp) < 0x100 || hdr[0] != 'M' || hdr[1] != 'Z') { fclose(fp); return NULL; }
    uint32_t pe = u32(hdr + 0x3C);
    if (pe + 0x100 > sizeof hdr) { fclose(fp); return NULL; }
    if (memcmp(hdr + pe, "PE\0\0", 4) != 0) { fclose(fp); return NULL; }
    int nsec = u16(hdr + pe + 6);
    uint32_t optsz = u16(hdr + pe + 20);
    const uint8_t *opt = hdr + pe + 24;
    uint32_t rsrc_rva = 0, rsrc_size = 0;
    if (u16(opt) == 0x10B) { rsrc_rva = u32(opt + 96 + 16); rsrc_size = u32(opt + 96 + 20); }
    PeModule *m = (PeModule *)calloc(1, sizeof *m);
    m->fp = fp;
    m->path = strdup(host_path);
    const uint8_t *st = hdr + pe + 24 + optsz;
    for (int i = 0; i < nsec && i < 32 && (size_t)(st - hdr) + 40 <= sizeof hdr; i++, st += 40) {
        m->secs[i].vsize = u32(st + 8);
        m->secs[i].va = u32(st + 12);
        m->secs[i].rawsize = u32(st + 16);
        m->secs[i].raw = u32(st + 20);
        m->nsec++;
    }
    m->rsrc_rva = rsrc_rva;
    m->rsrc_size = rsrc_size;
    if (rsrc_rva) {
        for (int i = 0; i < m->nsec; i++) {
            Sec *s = &m->secs[i];
            if (rsrc_rva >= s->va && rsrc_rva < s->va + (s->vsize > s->rawsize ? s->vsize : s->rawsize)) {
                /* 只载入资源目录部分：目录树通常位于节的开头，限制读取到 2MB 或目录大小 */
                uint32_t off = s->raw + (rsrc_rva - s->va);
                uint32_t want = rsrc_size < 0x400000u ? rsrc_size : 0x400000u;
                m->rsrc = (uint8_t *)malloc(want);
                fseek(fp, (long)off, SEEK_SET);
                if (fread(m->rsrc, 1, want, fp) != want) { /* 允许短读 */ }
                break;
            }
        }
    }
    return m;
}

void pe_close(PeModule *m) {
    if (!m) return;
    if (m->fp) fclose(m->fp);
    free(m->rsrc);
    free(m->path);
    free(m);
}

static int name_eq(const uint8_t *rs, uint32_t nameoff, const char *s) {
    const uint8_t *p = rs + (nameoff & 0x7FFFFFFF);
    int len = u16(p);
    p += 2;
    if ((int)strlen(s) != len) return 0;
    for (int i = 0; i < len; i++) {
        if (toupper((unsigned char)s[i]) != toupper(u16(p + i * 2))) return 0;
    }
    return 1;
}

/* 在目录 dir 中查找一项：id 或字符串名。找到返回其 OffsetToData 字段，否则 0xFFFFFFFF。 */
static uint32_t dir_find(const uint8_t *rs, uint32_t dir, uint32_t id, const char *name) {
    const uint8_t *d = rs + dir;
    int nn = u16(d + 12), ni = u16(d + 14);
    const uint8_t *e = d + 16;
    if (name) {
        for (int i = 0; i < nn; i++, e += 8)
            if (name_eq(rs, u32(e), name)) return u32(e + 4);
    } else {
        e += nn * 8;
        for (int i = 0; i < ni; i++, e += 8)
            if (u32(e) == id) return u32(e + 4);
    }
    return 0xFFFFFFFFu;
}

static uint32_t dir_first(const uint8_t *rs, uint32_t dir) {
    const uint8_t *d = rs + dir;
    int n = u16(d + 12) + u16(d + 14);
    if (!n) return 0xFFFFFFFFu;
    return u32(d + 16 + 4);
}

int pe_find_resource(PeModule *m, uint32_t type_id, const char *type_name, uint32_t name_id, const char *name_str, uint32_t *file_off, uint32_t *size) {
    if (!m || !m->rsrc) return 0;
    const uint8_t *rs = m->rsrc;
    uint32_t t = dir_find(rs, 0, type_id, type_name);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u)) return 0;
    uint32_t n = dir_find(rs, t & 0x7FFFFFFF, name_id, name_str);
    if (n == 0xFFFFFFFFu) return 0;
    uint32_t leaf = n;
    if (n & 0x80000000u) {
        leaf = dir_first(rs, n & 0x7FFFFFFF);
        if (leaf == 0xFFFFFFFFu) return 0;
        if (leaf & 0x80000000u) return 0;
    }
    const uint8_t *de = rs + leaf;
    uint32_t rva = u32(de), sz = u32(de + 4);
    for (int i = 0; i < m->nsec; i++) {
        Sec *s = &m->secs[i];
        if (rva >= s->va && rva < s->va + s->rawsize) {
            *file_off = s->raw + (rva - s->va);
            *size = sz;
            return 1;
        }
    }
    return 0;
}

int pe_read(PeModule *m, uint32_t off, uint32_t size, void *dst) {
    if (fseek(m->fp, (long)off, SEEK_SET) != 0) return 0;
    return fread(dst, 1, size, m->fp) == size;
}

/* 枚举（调试用）：列出所有类型与名称 */
static void dump_dir(PeModule *m, const uint8_t *rs, uint32_t dir, int level, char *path) {
    const uint8_t *d = rs + dir;
    int nn = u16(d + 12), ni = u16(d + 14);
    const uint8_t *e = d + 16;
    for (int i = 0; i < nn + ni; i++, e += 8) {
        char label[128];
        uint32_t nm = u32(e), off = u32(e + 4);
        if (i < nn) {
            const uint8_t *p = rs + (nm & 0x7FFFFFFF);
            int len = u16(p); if (len > 100) len = 100;
            int k = 0;
            for (; k < len; k++) label[k] = (char)u16(p + 2 + k * 2);
            label[k] = 0;
        } else snprintf(label, sizeof label, "#%u", nm);
        char np[512];
        snprintf(np, sizeof np, "%s/%s", path, label);
        if (off & 0x80000000u) dump_dir(m, rs, off & 0x7FFFFFFF, level + 1, np);
        else host_log("res %s size=%u", np, u32(rs + off + 4));
    }
}

void pe_dump(PeModule *m) {
    if (m && m->rsrc) { char p[8] = ""; dump_dir(m, m->rsrc, 0, 0, p); }
}
