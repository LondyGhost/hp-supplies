# make-icon.py — 生成 packaging/windows/hp-supplies.ico
# 设计复刻 data/hp-supplies.svg（打印机 + KCMY 四色条），与 Linux 版图标一致。
# 用法: python packaging/windows/make-icon.py
import os
from PIL import Image, ImageDraw

S = 8                      # 128 viewBox → 1024 超采样后缩到 256
W = 128 * S
INK   = (0x3c, 0x40, 0x43, 255)
PAPER = (0xe8, 0xea, 0xed, 255)
LED   = (0x34, 0xa8, 0x53, 255)
TONER = [(0x20, 0x21, 0x24, 255),   # K
         (0x00, 0xae, 0xef, 255),   # C
         (0xec, 0x00, 0x8c, 255),   # M
         (0xf4, 0xb4, 0x00, 255)]   # Y

im = Image.new('RGBA', (W, W), (0, 0, 0, 0))
d = ImageDraw.Draw(im)

def rrect(x, y, w, h, r, fill, outline=None, width=0):
    d.rounded_rectangle([x * S, y * S, (x + w) * S, (y + h) * S],
                        radius=r * S, fill=fill, outline=outline, width=width)

rrect(30, 26, 68, 24, 4, PAPER, INK, 4 * S)      # 进纸托盘
rrect(14, 50, 100, 44, 8, INK)                   # 机身
d.ellipse([(102 - 4) * S, (62 - 4) * S, (102 + 4) * S, (62 + 4) * S],
          fill=LED)                              # 电源指示灯
rrect(30, 70, 68, 40, 4, PAPER, INK, 4 * S)      # 出纸托盘
for i, x in enumerate((38, 52, 66, 80)):         # KCMY 四色耗材条
    rrect(x, 80, 11, 22, 2, TONER[i])

icon = im.resize((256, 256), Image.LANCZOS)
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hp-supplies.ico')
icon.save(out, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (48, 48),
                      (64, 64), (128, 128), (256, 256)])
print('saved:', out)
