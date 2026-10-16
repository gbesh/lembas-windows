"""Lembas cursor set: parchment-and-ink pointers, with the One Ring as the busy cursor.

Writes classic Windows .cur files (multi-size 32/48/64/96 px, 32-bit DIBs with alpha)
and animated .ani files. Usage: python make_cursors.py <out_dir>
"""
import io
import math
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

INK = (31, 22, 13, 255)
PARCH = (243, 232, 205, 255)
GOLD = (217, 174, 84, 255)
GOLD_D = (150, 108, 40, 255)
RED = (154, 42, 28, 255)
SIZES = [32, 48, 64, 96]
SS = 8  # supersampling


def canvas(size):
    return Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))


def finish(img, size):
    return img.resize((size, size), Image.LANCZOS)


def pts(points, size, ox=0.0, oy=0.0, s=1.0):
    k = size * SS / 32.0
    return [((x * s + ox) * k, (y * s + oy) * k) for x, y in points]


ARROW = [(1, 1), (1, 23.5), (6.2, 18.6), (9.8, 27.4), (13.2, 26), (9.7, 17.6), (16.6, 17.6)]


def arrow(d, size, outline=1.8):
    k = size * SS / 32.0
    d.polygon(pts(ARROW, size), fill=PARCH, outline=INK, width=max(1, int(outline * k)))
    # gold edge just inside the left side
    d.line(pts([(3.0, 5.0), (3.0, 19.0)], size), fill=GOLD, width=max(1, int(1.1 * k)))


def star(d, size, cx, cy, r, fill=GOLD, edge=INK):
    p = []
    for i in range(8):
        a = -math.pi / 2 + i * math.pi / 4
        rr = r if i % 2 == 0 else r * 0.28
        p.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    k = size * SS / 32.0
    d.polygon(pts(p, size), fill=fill, outline=edge, width=max(1, int(0.9 * k)))


def ring(img, size, cx, cy, r, glint_angle, thick=3.2):
    """The One Ring seen face-on, with a travelling glint."""
    k = size * SS / 32.0
    d = ImageDraw.Draw(img)
    box = [(cx - r) * k, (cy - r) * k, (cx + r) * k, (cy + r) * k]
    d.ellipse(box, outline=INK, width=int((thick + 1.6) * k))
    d.ellipse(box, outline=GOLD_D, width=int(thick * k))
    inner = [(cx - r + 0.6) * k, (cy - r + 0.6) * k, (cx + r - 0.6) * k, (cy + r - 0.6) * k]
    d.ellipse(inner, outline=GOLD, width=int((thick - 1.2) * k))
    # glint
    g = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(g)
    a0 = math.degrees(glint_angle)
    gd.arc(inner, a0 - 26, a0 + 26, fill=(255, 246, 214, 255), width=int((thick - 1.0) * k))
    g = g.filter(ImageFilter.GaussianBlur(0.8 * k))
    img.alpha_composite(g)


def double_arrow(d, size, angle):
    """A two-headed arrow through the centre, rotated by angle (degrees)."""
    shape = [(-12, 0), (-7, -5), (-7, -2), (7, -2), (7, -5), (12, 0), (7, 5), (7, 2), (-7, 2), (-7, 5)]
    a = math.radians(angle)
    rot = [(16 + x * math.cos(a) - y * math.sin(a), 16 + x * math.sin(a) + y * math.cos(a)) for x, y in shape]
    k = size * SS / 32.0
    d.polygon(pts(rot, size), fill=PARCH, outline=INK, width=max(1, int(1.5 * k)))


def draw_frame(kind, size, t=0.0):
    img = canvas(size)
    d = ImageDraw.Draw(img)
    k = size * SS / 32.0
    if kind == "arrow":
        arrow(d, size)
    elif kind == "link":
        arrow(d, size)
        star(d, size, 22.5, 22.5, 7.5)
    elif kind == "help":
        arrow(d, size)
        d.ellipse([16.5 * k, 16.5 * k, 29.5 * k, 29.5 * k], fill=PARCH, outline=INK, width=max(1, int(1.2 * k)))
        d.arc([19.5 * k, 18.8 * k, 26.5 * k, 24.5 * k], 180, 40, fill=RED, width=int(1.6 * k))
        d.line([23 * k, 24.2 * k, 23 * k, 25.6 * k], fill=RED, width=int(1.6 * k))
        d.ellipse([22.2 * k, 26.6 * k, 23.8 * k, 28.2 * k], fill=RED)
    elif kind == "busy":
        ring(img, size, 16, 16, 11, t * 2 * math.pi)
    elif kind == "working":
        arrow(d, size)
        ring(img, size, 23, 23, 6.3, t * 2 * math.pi, thick=2.2)
    elif kind == "ibeam":
        for w, col in ((3.4, PARCH), (1.6, INK)):
            lw = max(1, int(w * k))
            d.line([16 * k, 6 * k, 16 * k, 26 * k], fill=col, width=lw)
            d.line([12 * k, 5 * k, 20 * k, 5 * k], fill=col, width=lw)
            d.line([12 * k, 27 * k, 20 * k, 27 * k], fill=col, width=lw)
    elif kind == "cross":
        for w, col in ((3.2, PARCH), (1.4, INK)):
            lw = max(1, int(w * k))
            d.line([16 * k, 3 * k, 16 * k, 13 * k], fill=col, width=lw)
            d.line([16 * k, 19 * k, 16 * k, 29 * k], fill=col, width=lw)
            d.line([3 * k, 16 * k, 13 * k, 16 * k], fill=col, width=lw)
            d.line([19 * k, 16 * k, 29 * k, 16 * k], fill=col, width=lw)
        d.ellipse([15 * k, 15 * k, 17 * k, 17 * k], fill=RED)
    elif kind == "no":
        box = [5 * k, 5 * k, 27 * k, 27 * k]
        d.ellipse(box, outline=PARCH, width=int(5.4 * k))
        d.ellipse(box, outline=RED, width=int(3.6 * k))
        d.line([9.5 * k, 9.5 * k, 22.5 * k, 22.5 * k], fill=PARCH, width=int(5.4 * k))
        d.line([9.5 * k, 9.5 * k, 22.5 * k, 22.5 * k], fill=RED, width=int(3.4 * k))
    elif kind == "ns":
        double_arrow(d, size, 90)
    elif kind == "we":
        double_arrow(d, size, 0)
    elif kind == "nwse":
        double_arrow(d, size, 45)
    elif kind == "nesw":
        double_arrow(d, size, -45)
    elif kind == "move":
        double_arrow(d, size, 0)
        double_arrow(d, size, 90)
        d.ellipse([14 * k, 14 * k, 18 * k, 18 * k], fill=GOLD, outline=INK)
    elif kind == "up":
        shape = [(16, 3), (24, 13), (19, 13), (19, 29), (13, 29), (13, 13), (8, 13)]
        d.polygon(pts(shape, size), fill=PARCH, outline=INK, width=max(1, int(1.5 * k)))
    elif kind == "pen":
        shape = [(4, 28), (6, 21), (22, 5), (27, 10), (11, 26)]
        d.polygon(pts(shape, size), fill=PARCH, outline=INK, width=max(1, int(1.4 * k)))
        d.line(pts([(6, 21), (11, 26)], size), fill=INK, width=max(1, int(1.2 * k)))
        d.polygon(pts([(4, 28), (5.2, 24), (8, 26.8)], size), fill=INK)
        d.line(pts([(19, 8), (24, 13)], size), fill=GOLD, width=max(1, int(1.6 * k)))
    return finish(img, size)


HOT = {  # hotspot in 32-px units
    "arrow": (1, 1), "link": (1, 1), "help": (1, 1), "working": (1, 1), "busy": (16, 16),
    "ibeam": (16, 16), "cross": (16, 16), "no": (16, 16), "ns": (16, 16), "we": (16, 16),
    "nwse": (16, 16), "nesw": (16, 16), "move": (16, 16), "up": (16, 3), "pen": (4, 28),
}


def dib(img):
    """32-bit BGRA DIB + 1-bit AND mask, as stored inside .ico/.cur."""
    w, h = img.size
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = img.tobytes("raw", "BGRA")
    rows = [px[y * w * 4:(y + 1) * w * 4] for y in range(h)]
    xor = b"".join(reversed(rows))
    stride = ((w + 31) // 32) * 4
    andmask = bytearray()
    a = img.getchannel("A").tobytes()
    for y in reversed(range(h)):
        row = bytearray(stride)
        for x in range(w):
            if a[y * w + x] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        andmask += row
    return header + xor + bytes(andmask)


def cur_bytes(frames_by_size, hot):
    """frames_by_size: list of (size, Image)."""
    n = len(frames_by_size)
    out = io.BytesIO()
    out.write(struct.pack("<HHH", 0, 2, n))
    data = [dib(im) for _, im in frames_by_size]
    offset = 6 + 16 * n
    for (size, _), blob in zip(frames_by_size, data):
        hx = round(hot[0] * size / 32)
        hy = round(hot[1] * size / 32)
        out.write(struct.pack("<BBBBHHII", size if size < 256 else 0, size if size < 256 else 0, 0, 0, hx, hy, len(blob), offset))
        offset += len(blob)
    for blob in data:
        out.write(blob)
    return out.getvalue()


def ani_bytes(frames, rate_jiffies):
    """frames: list of .cur byte strings."""
    def chunk(tag, payload):
        pad = b"\0" if len(payload) % 2 else b""
        return tag + struct.pack("<I", len(payload)) + payload + pad
    anih = struct.pack("<IIIIIIIII", 36, len(frames), len(frames), 0, 0, 0, 0, rate_jiffies, 1)
    fram = b"fram" + b"".join(chunk(b"icon", f) for f in frames)
    body = b"ACON" + chunk(b"anih", anih) + chunk(b"LIST", fram)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    statics = {
        "Lembas_Arrow.cur": "arrow", "Lembas_Link.cur": "link", "Lembas_Help.cur": "help",
        "Lembas_Text.cur": "ibeam", "Lembas_Precision.cur": "cross", "Lembas_Unavailable.cur": "no",
        "Lembas_SizeNS.cur": "ns", "Lembas_SizeWE.cur": "we", "Lembas_SizeNWSE.cur": "nwse",
        "Lembas_SizeNESW.cur": "nesw", "Lembas_Move.cur": "move", "Lembas_Alternate.cur": "up",
        "Lembas_Handwriting.cur": "pen",
    }
    for name, kind in statics.items():
        (out / name).write_bytes(cur_bytes([(s, draw_frame(kind, s)) for s in SIZES], HOT[kind]))
    for name, kind in (("Lembas_Busy.ani", "busy"), ("Lembas_Working.ani", "working")):
        frames = []
        n = 12
        for i in range(n):
            frames.append(cur_bytes([(s, draw_frame(kind, s, i / n)) for s in (32, 48, 64)], HOT[kind]))
        (out / name).write_bytes(ani_bytes(frames, 4))
    # preview sheet
    kinds = list(statics.values()) + ["busy", "working"]
    sheet = Image.new("RGBA", (len(kinds) * 72, 160), (60, 66, 84, 255))
    light = Image.new("RGBA", (len(kinds) * 72, 80), (240, 236, 226, 255))
    sheet.paste(light, (0, 80))
    for i, kd in enumerate(kinds):
        im = draw_frame(kd, 64, 0.1)
        sheet.alpha_composite(im, (i * 72 + 4, 8))
        sheet.alpha_composite(im, (i * 72 + 4, 88))
    sheet.save(out / "preview.png")
    print("cursors written to", out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "cursors")
