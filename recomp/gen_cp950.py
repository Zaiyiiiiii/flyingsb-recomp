"""由 Python 标准库的 cp950 编解码器生成 Big5(CP950) -> Unicode 表，供非 Windows 平台使用。"""
import sys

out = sys.argv[1]
rows = []
for lead in range(0x81, 0xFF):
    row = []
    for trail in range(0x40, 0xFF):
        try:
            ch = bytes([lead, trail]).decode("cp950")
            row.append(ord(ch) if len(ch) == 1 else 0)
        except UnicodeDecodeError:
            row.append(0)
    rows.append(row)
with open(out, "w", newline="\n") as f:
    f.write("/* 由 recomp/gen_cp950.py 自动生成：CP950 双字节 -> Unicode。lead 0x81..0xFE, trail 0x40..0xFE */\n")
    f.write("#include <stdint.h>\n")
    f.write("const uint16_t cp950_table[126][191] = {\n")
    for row in rows:
        f.write("{" + ",".join("0x%X" % v for v in row) + "},\n")
    f.write("};\n")
