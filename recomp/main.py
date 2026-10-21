"""静态重编译驱动：读取用户提供的原版 FlyingSB.exe，生成可移植的 C 源文件。

用法： python main.py <FlyingSB.exe> <输出目录> [--extra-roots file]
"""
import argparse
import os
import sys

from image import Image
from discover import Discovery
from lift import Lifter

PER_FILE = 120


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--extra-roots", default=None)
    ap.add_argument("--hooks", default=None, help="每行 '地址(十六进制) C函数名'：在该地址的指令前插入对手写函数的调用")
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    img = Image(args.exe)
    extra = []
    if args.extra_roots and os.path.exists(args.extra_roots):
        for line in open(args.extra_roots, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if line:
                extra.append(int(line, 16))
    disc = Discovery(img, extra)
    funcs = disc.run()
    lifter = Lifter(img, funcs)
    if args.hooks and os.path.exists(args.hooks):
        for line in open(args.hooks, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if line:
                parts = line.split()
                tgt = int(parts[2], 16) if len(parts) > 2 else None
                lifter.hooks[int(parts[0], 16)] = (parts[1], tgt)

    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):
        if fn.startswith("gen_") and (fn.endswith(".c") or fn.endswith(".h")):
            os.remove(os.path.join(args.out, fn))

    entries = sorted(funcs)
    chunk = []
    idx = 0

    def flush():
        nonlocal chunk, idx
        if not chunk:
            return
        with open(os.path.join(args.out, "gen_%03d.c" % idx), "w", encoding="utf-8", newline="\n") as fh:
            fh.write('#include "rt.h"\n#include "gen_decl.h"\n\n')
            fh.write("\n\n".join(chunk))
            fh.write("\n")
        chunk = []
        idx += 1

    for e in entries:
        text, _ = lifter.lift_function(funcs[e])
        chunk.append(text)
        if len(chunk) >= PER_FILE:
            flush()
    flush()

    with open(os.path.join(args.out, "gen_decl.h"), "w", newline="\n") as fh:
        fh.write("#pragma once\n#include <stdint.h>\n")
        for e in entries:
            fh.write("void f_%08X(void);\n" % e)
        for name, tgt in sorted(set(lifter.hooks.values())):
            fh.write("%s %s(void);\n" % ("void" if tgt is None else "int", name))   # 钩子在 runtime/hooks.c 手写实现
    with open(os.path.join(args.out, "gen_table.c"), "w", newline="\n") as fh:
        fh.write('#include "rt.h"\n#include "gen_decl.h"\n\n')
        fh.write("const RtFuncEntry g_func_table[] = {\n")
        for e in entries:
            fh.write("  {0x%08Xu, f_%08X},\n" % (e, e))
        fh.write("};\nconst int g_func_count = %d;\n" % len(entries))
        fh.write("const uint32_t g_image_base = 0x%Xu;\n" % img.base)
        fh.write("const uint32_t g_image_size = 0x%Xu;\n" % img.size)
        fh.write("const uint32_t g_entry = 0x%Xu;\n" % img.entry)
        fh.write("const RtImportSlot g_imports[] = {\n")
        for a, (dll, name) in sorted(img.imports.items()):
            fh.write('  {0x%Xu, "%s", "%s"},\n' % (a, dll, name))
        fh.write("};\nconst int g_import_count = %d;\n" % len(img.imports))
        # 校验：原版 .text 的哈希，运行时用来确认用户提供的是匹配的原版可执行文件
        import hashlib
        h = hashlib.sha256(bytes(img.data[img.text_lo - img.base : img.text_hi - img.base])).hexdigest()
        fh.write('const char g_text_sha256[] = "%s";\n' % h)

    from icon import extract
    if not extract(args.exe, args.out):
        print("警告: exe 里没有图标资源")

    ninsn = sum(len(f.insns) for f in funcs.values())
    print("functions: %d, instructions: %d, files: %d" % (len(entries), ninsn, idx))
    if lifter.unsupported:
        print("UNSUPPORTED:")
        for k, v in sorted(lifter.unsupported.items(), key=lambda kv: -len(kv[1])):
            print("  %-20s x%d  e.g. %s" % (k, len(v), ", ".join("%X" % x for x in v[:4])))


if __name__ == "__main__":
    main()
