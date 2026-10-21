"""从原版 exe 的资源里提取程序图标，供各平台构建引用（不进仓库）。

输出到 <out>/ 下：
  flyingsb.ico / flyingsb.rc            Windows exe 图标
  res/mipmap-xxxhdpi/ic_launcher.png    Android 启动图标
"""
import os
import struct
import zlib

import pefile

ANDROID_SIZE = 192


def _resources(pe, type_id):
    """返回 {资源 id: 数据}，只取第一种语言。"""
    out = {}
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.struct.Id != type_id:
            continue
        for r in t.directory.entries:
            d = r.directory.entries[0].data.struct
            out[r.struct.Id] = pe.get_data(d.OffsetToData, d.Size)
    return out


def _decode_dib(data):
    """解码图标里的 DIB（高度是图像加掩码的两倍），返回 (宽, 高, 自上而下的 RGBA 行列表)。"""
    size, w, h2, _planes, bpp, _comp, _img, _xp, _yp, clr_used = struct.unpack_from("<IiiHHIIiiI", data, 0)
    h = h2 // 2
    npal = clr_used or ((1 << bpp) if bpp <= 8 else 0)
    pal = [struct.unpack_from("<BBBB", data, size + 4 * i) for i in range(npal)]
    pix_off = size + 4 * npal
    stride = ((w * bpp + 31) // 32) * 4
    mask_off = pix_off + stride * h
    mask_stride = ((w + 31) // 32) * 4
    rows = []
    has_alpha = False
    for y in range(h):
        src = pix_off + stride * (h - 1 - y)
        mrow = mask_off + mask_stride * (h - 1 - y)
        row = []
        for x in range(w):
            if bpp == 32:
                b, g, r, a = data[src + 4 * x: src + 4 * x + 4]
                has_alpha |= a != 0
            elif bpp == 24:
                b, g, r = data[src + 3 * x: src + 3 * x + 3]
                a = 255
            elif bpp == 8:
                b, g, r, _ = pal[data[src + x]]
                a = 255
            elif bpp == 4:
                v = data[src + x // 2]
                b, g, r, _ = pal[(v >> 4) if x % 2 == 0 else (v & 15)]
                a = 255
            else:
                raise ValueError("不支持的图标位深 %d" % bpp)
            row.append([r, g, b, a])
        if bpp != 32 or not has_alpha:
            for x in range(w):
                if data[mrow + x // 8] >> (7 - x % 8) & 1:
                    row[x][3] = 0
        rows.append(row)
    return w, h, rows


def _png(rgba_rows):
    h, w = len(rgba_rows), len(rgba_rows[0])
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in rgba_rows)

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def _scale(rows, w, h, size):
    return [[rows[y * h // size][x * w // size] for x in range(size)] for y in range(size)]


def extract(exe, out):
    pe = pefile.PE(exe)
    if not hasattr(pe, "DIRECTORY_ENTRY_RESOURCE"):
        return False
    groups = _resources(pe, pefile.RESOURCE_TYPE["RT_GROUP_ICON"])
    icons = _resources(pe, pefile.RESOURCE_TYPE["RT_ICON"])
    if not groups:
        return False
    grp = groups[min(groups)]
    count = struct.unpack_from("<H", grp, 4)[0]
    entries = []   # (宽, 高, 色数, 保留, 平面, 位深, 数据)
    for i in range(count):
        w, h, colors, res, planes, bpp, _size, icon_id = struct.unpack_from("<BBBBHHIH", grp, 6 + 14 * i)
        if icon_id in icons:
            entries.append((w, h, colors, res, planes, bpp, icons[icon_id]))
    if not entries:
        return False

    os.makedirs(os.path.join(out, "res", "mipmap-xxxhdpi"), exist_ok=True)
    ico = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for w, h, colors, res, planes, bpp, data in entries:
        ico += struct.pack("<BBBBHHII", w, h, colors, res, planes, bpp, len(data), offset)
        offset += len(data)
    ico += b"".join(e[6] for e in entries)
    with open(os.path.join(out, "flyingsb.ico"), "wb") as fh:
        fh.write(ico)
    with open(os.path.join(out, "flyingsb.rc"), "w") as fh:
        fh.write('1 ICON "flyingsb.ico"\n')

    best = max(entries, key=lambda e: (e[0] or 256, e[5]))
    w, h, rows = _decode_dib(best[6])
    with open(os.path.join(out, "res", "mipmap-xxxhdpi", "ic_launcher.png"), "wb") as fh:
        fh.write(_png(_scale(rows, w, h, ANDROID_SIZE)))
    return True
