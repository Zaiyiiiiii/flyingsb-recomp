"""把 FLYINGSB_DUMP 产生的 PPM 转成 PNG，便于查看。用法: python ppm2png.py <目录或文件> [缩放]"""
import sys, os
from PIL import Image
p = sys.argv[1]
files = [os.path.join(p, f) for f in sorted(os.listdir(p)) if f.endswith('.ppm')] if os.path.isdir(p) else [p]
for f in files:
    im = Image.open(f)
    im.save(f[:-4] + '.png')
print(len(files), 'converted')
