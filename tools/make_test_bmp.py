"""
造一张 160x80 的测试 BMP(24bpp),用来验证「宿��写盘 → 设备读盘 → 画到屏上」
这条通路。图案要一眼能认出来:左上红、右上绿、左下蓝、右下白,中间一个黑方块。

用法:
  python tools\\make_test_bmp.py E:\\SCRN.BMP
"""
import sys

from PIL import Image, ImageDraw

W, H = 160, 80


def build(path):
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    # 四象限
    d.rectangle([0, 0, W // 2 - 1, H // 2 - 1], fill=(255, 0, 0))       # 红
    d.rectangle([W // 2, 0, W - 1, H // 2 - 1], fill=(0, 255, 0))       # 绿
    d.rectangle([0, H // 2, W // 2 - 1, H - 1], fill=(0, 0, 255))       # 蓝
    d.rectangle([W // 2, H // 2, W - 1, H - 1], fill=(255, 255, 255))   # 白
    # 正中间一个黑方块:看得出有没有偏移/翻转
    d.rectangle([W // 2 - 20, H // 2 - 12, W // 2 + 20, H // 2 + 12], fill=(0, 0, 0))
    # 左上角一条灰阶:看得出渐变有没有反
    for i in range(40):
        v = int(i * 255 / 39)
        d.point((i, H - 1), fill=(v, v, v))

    # BMP 存成 24bpp(BITMAPINFOHEADER,BI_RGB)
    img.save(path, "BMP")
    return path


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else r"C:\Temp\SCRN.BMP"
    build(out)
    print("wrote", out)
