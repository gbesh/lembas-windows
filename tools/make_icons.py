"""Lembas system icons for Windows.

Draws every icon as SVG, renders each one at all the sizes Windows asks for, writes
.ico files, and builds a resource-only "imageres.dll" with the same icon IDs as
Windows' own, for Windhawk's Resource Redirect mod (theme.ini included).

Usage: python make_icons.py <out_dir>
  out_dir/Icons/*.ico          the icons as plain .ico files (desktop icons, folders)
  out_dir/IconTheme/theme.ini  + imageres.dll   (Resource Redirect theme)
  out_dir/preview.png          contact sheet on light and dark
Needs: cairosvg, Pillow, llvm-rc and lld-link (for the DLL).
"""
import io
import math
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import cairosvg
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 256]

INK = "#2B1E12"
RED = "#9A2A1C"
GOLD = "#C9A04C"
GREEN = "#4E7A3A"

DEFS = """
<defs>
 <linearGradient id="parch" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FCF3DA"/><stop offset="1" stop-color="#E4CB92"/></linearGradient>
 <linearGradient id="page" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FFFBEF"/><stop offset="1" stop-color="#F1E2BC"/></linearGradient>
 <linearGradient id="leather" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#A3703E"/><stop offset="1" stop-color="#6E4424"/></linearGradient>
 <linearGradient id="wood" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#7A5230"/><stop offset=".5" stop-color="#B07F48"/><stop offset="1" stop-color="#7A5230"/></linearGradient>
 <linearGradient id="woodv" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#B98A52"/><stop offset="1" stop-color="#83582F"/></linearGradient>
 <linearGradient id="gold" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#F3DB93"/><stop offset="1" stop-color="#B8892F"/></linearGradient>
 <linearGradient id="door" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#6E9F52"/><stop offset="1" stop-color="#3A6328"/></linearGradient>
 <linearGradient id="hill" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#93B86A"/><stop offset="1" stop-color="#557F3C"/></linearGradient>
 <linearGradient id="leaf" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#8DB565"/><stop offset="1" stop-color="#4A7533"/></linearGradient>
 <linearGradient id="stone" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#C9B892"/><stop offset="1" stop-color="#8E7C58"/></linearGradient>
 <radialGradient id="orb" cx=".42" cy=".38" r=".7"><stop offset="0" stop-color="#5F86B8"/><stop offset=".45" stop-color="#22345E"/><stop offset="1" stop-color="#090D1C"/></radialGradient>
 <radialGradient id="glow"><stop offset="0" stop-color="#FFD58A" stop-opacity=".85"/><stop offset=".35" stop-color="#E9A23B" stop-opacity=".35"/><stop offset="1" stop-color="#E9A23B" stop-opacity="0"/></radialGradient>
 <radialGradient id="shield" cx=".5" cy=".45" r=".6"><stop offset="0" stop-color="#B8392A"/><stop offset="1" stop-color="#7A1E14"/></radialGradient>
</defs>"""


def svg(body, size):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 256 256">'
            f'{DEFS}{body}</svg>')


def star(cx, cy, r_out, r_mid, r_in, fill, sw, points=8):
    pts = []
    for i in range(points * 2):
        a = -math.pi / 2 + i * math.pi / points
        rr = r_out if i % 4 == 0 else (r_mid if i % 2 == 0 else r_in)
        if points != 8:
            rr = r_out if i % 2 == 0 else r_in
        pts.append(f"{cx + rr * math.cos(a):.1f},{cy + rr * math.sin(a):.1f}")
    return f'<polygon points="{" ".join(pts)}" fill="{fill}" stroke="{INK}" stroke-width="{sw * .7:.1f}" stroke-linejoin="round"/>'


# ---------------------------------------------------------------- emblems (drawn on a folder front, centre 128,154)
def em_quill(sw, small):
    s = (f'<path d="M166,114 C134,120 108,150 98,192 C118,170 146,150 170,120 Z" fill="#FFFBEF" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>'
         f'<path d="M98,192 L90,204" stroke="{INK}" stroke-width="{sw}" stroke-linecap="round"/>')
    if not small:
        s += f'<path d="M160,122 C136,138 118,160 104,184" stroke="{INK}" stroke-width="{sw * .5}" fill="none"/>'
        s += f'<path d="M112,200 q16,-9 34,-2 q12,4 22,-4" stroke="{RED}" stroke-width="{sw * .8}" fill="none" stroke-linecap="round"/>'
    return s


def em_down(sw, small):
    return (f'<polygon points="114,116 142,116 142,148 162,148 128,186 94,148 114,148" fill="{GREEN}" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>'
            f'<path d="M98,198 L158,198" stroke="{INK}" stroke-width="{sw * 1.1}" stroke-linecap="round"/>')


def em_mountain(sw, small):
    s = (f'<circle cx="156" cy="126" r="12" fill="{RED}" stroke="{INK}" stroke-width="{sw * .7}"/>'
         f'<polygon points="86,194 122,132 140,160 152,146 172,194" fill="#5B6F8A" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>')
    if not small:
        s += f'<polygon points="122,132 112,149 120,146 126,152 131,146" fill="#F4EEDF" stroke="{INK}" stroke-width="{sw * .5}" stroke-linejoin="round"/>'
    return s


def em_harp(sw, small):
    # pillar (100,200)->(100,116), neck curve to (166,130), soundbox back to (108,200)
    def neck_y(x):
        # quadratic from (100,116) via (132,92) to (166,130): solve t for x, then y
        a, b, c = 100 - 2 * 132 + 166, 2 * (132 - 100), 100 - x
        t = (-b + math.sqrt(b * b - 4 * a * c)) / (2 * a) if a else -c / b
        return (1 - t) ** 2 * 116 + 2 * (1 - t) * t * 92 + t * t * 130
    s = ''
    xs = (114, 126, 138, 150) if not small else (122, 142)
    for x in xs:
        bottom = 130 + (166 - x) * (70 / 58)
        s += f'<path d="M{x},{neck_y(x) + 3:.1f} L{x},{bottom - 3:.1f}" stroke="{INK}" stroke-width="{sw * .55}"/>'
    shape = "M100,200 L100,116 Q132,92 166,130 L108,200 Z"
    s = '<g transform="translate(0,6)">' + s
    s += (f'<path d="{shape}" fill="none" stroke="{INK}" stroke-width="{sw * 2.4}" stroke-linejoin="round"/>'
          f'<path d="{shape}" fill="none" stroke="url(#gold)" stroke-width="{sw * 1.2}" stroke-linejoin="round"/></g>')
    return s


def em_play(sw, small):
    return (f'<circle cx="128" cy="154" r="34" fill="{RED}" stroke="{INK}" stroke-width="{sw}"/>'
            f'<polygon points="118,136 148,154 118,172" fill="#FBF1D6" stroke="{INK}" stroke-width="{sw * .5}" stroke-linejoin="round"/>')


def em_door(sw, small):
    s = (f'<circle cx="128" cy="156" r="38" fill="url(#stone)" stroke="{INK}" stroke-width="{sw}"/>'
         f'<circle cx="128" cy="156" r="28" fill="url(#door)" stroke="{INK}" stroke-width="{sw * .8}"/>')
    if not small:
        s += ''.join(f'<path d="M{x},{156 - math.sqrt(max(0, 28 ** 2 - (x - 128) ** 2)) + 2:.1f} L{x},{156 + math.sqrt(max(0, 28 ** 2 - (x - 128) ** 2)) - 2:.1f}" stroke="#2F5226" stroke-width="{sw * .4}"/>' for x in (114, 128, 142))
    s += f'<circle cx="128" cy="156" r="{6 if not small else 8}" fill="url(#gold)" stroke="{INK}" stroke-width="{sw * .5}"/>'
    return s


def em_user(sw, small):
    return (f'<circle cx="128" cy="134" r="18" fill="#7A5634" stroke="{INK}" stroke-width="{sw}"/>'
            f'<path d="M92,198 Q92,160 128,160 Q164,160 164,198 Z" fill="#7A5634" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>')


def em_star(sw, small):
    return star(128, 154, 42, 22, 9, "url(#gold)", sw * 1.3)


def em_search(sw, small):
    return (f'<path d="M140,166 L164,192" stroke="{INK}" stroke-width="{sw * 3}" stroke-linecap="round"/>'
            f'<path d="M140,166 L164,192" stroke="#8A5A2E" stroke-width="{sw * 1.7}" stroke-linecap="round"/>'
            f'<circle cx="122" cy="146" r="26" fill="#DCE8EE" fill-opacity=".85" stroke="{INK}" stroke-width="{sw * 2.1}"/>'
            f'<circle cx="122" cy="146" r="26" fill="none" stroke="url(#gold)" stroke-width="{sw}"/>')


def em_sword(sw, small):
    return (f'<polygon points="128,112 136,124 136,176 120,176 120,124" fill="#E3E8EA" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>'
            f'<rect x="100" y="174" width="56" height="10" rx="4" fill="url(#gold)" stroke="{INK}" stroke-width="{sw * .8}"/>'
            f'<rect x="123" y="184" width="10" height="14" fill="#6E4424" stroke="{INK}" stroke-width="{sw * .6}"/>'
            f'<circle cx="128" cy="202" r="6" fill="url(#gold)" stroke="{INK}" stroke-width="{sw * .6}"/>')


def em_share(sw, small):
    return (f'<path d="M104,154 L152,128 M104,154 L152,180" stroke="{INK}" stroke-width="{sw * 1.2}"/>'
            + ''.join(f'<circle cx="{x}" cy="{y}" r="13" fill="{GREEN}" stroke="{INK}" stroke-width="{sw}"/>' for x, y in ((104, 154), (152, 128), (152, 180))))


def em_cloud(sw, small):
    return (f'<path d="M98,186 Q78,186 80,166 Q82,148 102,150 Q106,124 132,126 Q156,128 160,150 Q180,148 180,168 Q180,186 160,186 Z" '
            f'fill="#EEF3F6" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>')


def em_home(sw, small):
    return (f'<path d="M80,196 Q84,128 128,124 Q172,128 176,196 Z" fill="url(#hill)" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>'
            f'<circle cx="128" cy="172" r="20" fill="url(#door)" stroke="{INK}" stroke-width="{sw * .8}"/>'
            f'<circle cx="128" cy="172" r="4" fill="url(#gold)"/>'
            f'<path d="M80,196 L176,196" stroke="{INK}" stroke-width="{sw}" stroke-linecap="round"/>')


# ---------------------------------------------------------------- folders
def folder(emblem=None, open_=False):
    def draw(sw, small):
        s = (f'<path d="M18,60 Q18,46 32,46 L90,46 Q99,46 105,53 L116,66 L224,66 Q238,66 238,80 L238,206 Q238,220 224,220 L32,220 Q18,220 18,206 Z" '
             f'fill="url(#leather)" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>')
        if not small:
            s += f'<rect x="40" y="76" width="176" height="44" rx="4" fill="url(#page)" stroke="{INK}" stroke-width="{sw * .6}"/>'
            s += f'<path d="M58,90 L150,90 M58,102 L196,102" stroke="#8B7350" stroke-width="{sw * .45}"/>'
        if open_:
            s += (f'<path d="M44,104 L238,104 Q252,104 248,118 L226,208 Q223,220 210,220 L30,220 Q14,220 18,206 L32,116 Q34,104 44,104 Z" '
                  f'fill="url(#parch)" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>')
        else:
            s += f'<rect x="18" y="100" width="220" height="120" rx="13" fill="url(#parch)" stroke="{INK}" stroke-width="{sw}"/>'
            if not small:
                s += f'<rect x="30" y="111" width="196" height="98" rx="6" fill="none" stroke="{GOLD}" stroke-width="{sw * .55}"/>'
        if emblem:
            s += emblem(sw, small)
        return s
    return draw


# ---------------------------------------------------------------- standalone icons
def this_pc(sw, small):
    s = (f'<path d="M10,226 Q20,92 128,80 Q236,92 246,226 Z" fill="url(#hill)" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>'
         f'<circle cx="128" cy="166" r="62" fill="url(#stone)" stroke="{INK}" stroke-width="{sw}"/>'
         f'<circle cx="128" cy="166" r="48" fill="url(#door)" stroke="{INK}" stroke-width="{sw * .9}"/>')
    if not small:
        for x in (104, 120, 136, 152):
            h = math.sqrt(48 ** 2 - (x - 128) ** 2) - 3
            s += f'<path d="M{x},{166 - h:.1f} L{x},{166 + h:.1f}" stroke="#2F5226" stroke-width="{sw * .45}"/>'
        s += f'<path d="M40,118 q8,-10 16,0 M206,112 q8,-10 16,0" stroke="#3E6A2C" stroke-width="{sw * .6}" fill="none" stroke-linecap="round"/>'
    s += f'<rect x="10" y="222" width="236" height="12" rx="6" fill="#7A5634" stroke="{INK}" stroke-width="{sw * .8}"/>'
    s += f'<circle cx="128" cy="166" r="{9 if not small else 12}" fill="url(#gold)" stroke="{INK}" stroke-width="{sw * .6}"/>'
    return s


def chest(windows=False):
    def draw(sw, small):
        s = (f'<path d="M26,112 L26,86 Q26,46 128,46 Q230,46 230,86 L230,112 Z" fill="url(#woodv)" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>'
             f'<rect x="26" y="108" width="204" height="110" rx="10" fill="url(#wood)" stroke="{INK}" stroke-width="{sw}"/>')
        for x in (54, 188):
            s += f'<path d="M{x},50 L{x},216" stroke="{INK}" stroke-width="{sw * 2.6}"/><path d="M{x},50 L{x},216" stroke="url(#gold)" stroke-width="{sw * 1.5}"/>'
        if not small:
            s += ''.join(f'<path d="M30,{y} L226,{y}" stroke="#6A4627" stroke-width="{sw * .4}"/>' for y in (144, 182))
        s += f'<path d="M26,110 L230,110" stroke="{INK}" stroke-width="{sw}"/>'
        s += f'<rect x="106" y="92" width="44" height="50" rx="6" fill="url(#gold)" stroke="{INK}" stroke-width="{sw}"/>'
        if windows:
            s += star(128, 117, 16, 9, 4, "#FFF6DA", sw * .9)
        else:
            s += f'<circle cx="128" cy="112" r="6" fill="{INK}"/><path d="M128,114 L128,130" stroke="{INK}" stroke-width="{sw * 1.1}" stroke-linecap="round"/>'
        return s
    return draw


def parcel(sw, small):
    s = (f'<path d="M128,22 C206,54 226,150 128,232 C30,150 50,54 128,22 Z" fill="url(#leaf)" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>'
         f'<path d="M128,34 L128,222" stroke="#355A24" stroke-width="{sw * .7}"/>')
    if not small:
        for y in (70, 104, 166, 196):
            d = 26 if y < 128 else 22
            s += f'<path d="M128,{y} L{128 - d * 1.6:.0f},{y - d} M128,{y} L{128 + d * 1.6:.0f},{y - d}" stroke="#355A24" stroke-width="{sw * .4}"/>'
    s += (f'<path d="M58,132 Q128,148 198,132" stroke="{INK}" stroke-width="{sw * 2.4}" fill="none"/>'
          f'<path d="M58,132 Q128,148 198,132" stroke="#D9C089" stroke-width="{sw * 1.2}" fill="none"/>'
          f'<path d="M128,140 q-24,-26 -30,-6 q-2,14 30,6 q24,-26 30,-6 q2,14 -30,6" fill="#D9C089" stroke="{INK}" stroke-width="{sw * .7}"/>')
    return s


def shield(sw, small):
    s = (f'<circle cx="128" cy="128" r="106" fill="url(#gold)" stroke="{INK}" stroke-width="{sw}"/>'
         f'<circle cx="128" cy="128" r="88" fill="url(#shield)" stroke="{INK}" stroke-width="{sw * .8}"/>')
    if not small:
        for i in range(8):
            a = i * math.pi / 4
            s += f'<path d="M{128 + 32 * math.cos(a):.1f},{128 + 32 * math.sin(a):.1f} L{128 + 84 * math.cos(a):.1f},{128 + 84 * math.sin(a):.1f}" stroke="#E2B95A" stroke-width="{sw * .7}"/>'
    s += f'<circle cx="128" cy="128" r="30" fill="url(#gold)" stroke="{INK}" stroke-width="{sw}"/>'
    s += f'<circle cx="120" cy="120" r="8" fill="#FFF3C8" opacity=".8"/>'
    return s


def barrel(full=False):
    def draw(sw, small):
        s = (f'<path d="M58,64 Q40,144 58,224 L198,224 Q216,144 198,64 Z" fill="url(#wood)" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>')
        if not small:
            for x0 in (84, 110, 146, 172):
                bulge = (x0 - 128) * 0.18
                s += f'<path d="M{x0},68 Q{x0 + bulge:.1f},144 {x0},220" stroke="#6A4627" stroke-width="{sw * .5}" fill="none"/>'
        for y, bow in ((96, 6), (192, -6)):
            s += f'<path d="M{50 if y < 144 else 50},{y} Q128,{y + bow * 2} {206},{y}" stroke="{INK}" stroke-width="{sw * 2.8}" fill="none"/>'
            s += f'<path d="M{50},{y} Q128,{y + bow * 2} {206},{y}" stroke="#5E5A55" stroke-width="{sw * 1.5}" fill="none"/>'
        s += f'<ellipse cx="128" cy="64" rx="70" ry="16" fill="#3B2616" stroke="{INK}" stroke-width="{sw}"/>'
        if full:
            # a written page and a rolled scroll poking out of the barrel
            s += (f'<g transform="rotate(-12 100 60)"><rect x="76" y="14" width="50" height="58" rx="3" fill="url(#page)" stroke="{INK}" stroke-width="{sw * .8}"/>'
                  + ('' if small else f'<path d="M86,30 L116,30 M86,42 L116,42 M86,54 L108,54" stroke="#8B7350" stroke-width="{sw * .5}"/>') + '</g>'
                  f'<g transform="rotate(16 156 56)"><rect x="140" y="12" width="24" height="60" rx="11" fill="url(#parch)" stroke="{INK}" stroke-width="{sw * .8}"/>'
                  f'<ellipse cx="152" cy="15" rx="12" ry="5" fill="#E4CB92" stroke="{INK}" stroke-width="{sw * .6}"/></g>')
            # front lip of the barrel drawn over them
            s += f'<path d="M58,64 A70,16 0 0 0 198,64" fill="url(#wood)" stroke="{INK}" stroke-width="{sw}"/>'
        return s
    return draw


def palantir(sw, small):
    s = (f'<path d="M78,232 L92,186 L164,186 L178,232 Z" fill="url(#stone)" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>'
         f'<rect x="66" y="226" width="124" height="14" rx="5" fill="#8E7C58" stroke="{INK}" stroke-width="{sw * .8}"/>'
         f'<circle cx="128" cy="108" r="86" fill="url(#orb)" stroke="{INK}" stroke-width="{sw}"/>')
    if not small:
        s += f'<circle cx="134" cy="120" r="46" fill="url(#glow)"/>'
        s += f'<path d="M92,132 Q120,104 150,118 Q170,128 162,146" stroke="#9CC0E0" stroke-width="{sw * .5}" fill="none" opacity=".55"/>'
    s += f'<ellipse cx="100" cy="74" rx="22" ry="12" fill="#FFFFFF" opacity=".28" transform="rotate(-30 100 74)"/>'
    return s


def document(sw, small):
    s = (f'<path d="M54,22 L166,22 L210,66 L210,234 L54,234 Z" fill="url(#page)" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>'
         f'<path d="M166,22 L166,66 L210,66" fill="#E4CB92" stroke="{INK}" stroke-width="{sw}" stroke-linejoin="round"/>')
    if not small:
        s += f'<rect x="72" y="80" width="34" height="34" rx="3" fill="{RED}" stroke="{INK}" stroke-width="{sw * .6}"/>'
        s += f'<path d="M80,106 L89,86 L98,106 M84,99 L94,99" stroke="#F3DB93" stroke-width="{sw * .6}" fill="none" stroke-linejoin="round"/>'
        for y in (84, 98, 112):
            s += f'<path d="M116,{y} L190,{y}" stroke="#8B7350" stroke-width="{sw * .55}"/>'
        for y in (132, 150, 168, 186, 204):
            s += f'<path d="M72,{y} L{190 if y != 204 else 150},{y}" stroke="#8B7350" stroke-width="{sw * .55}"/>'
    return s


def magnifier(sw, small):
    return (f'<path d="M160,160 L222,222" stroke="{INK}" stroke-width="{sw * 5}" stroke-linecap="round"/>'
            f'<path d="M160,160 L222,222" stroke="#8A5A2E" stroke-width="{sw * 3}" stroke-linecap="round"/>'
            f'<circle cx="110" cy="110" r="72" fill="#DCE8EE" fill-opacity=".8" stroke="{INK}" stroke-width="{sw * 4}"/>'
            f'<circle cx="110" cy="110" r="72" fill="none" stroke="url(#gold)" stroke-width="{sw * 2.2}"/>'
            f'<ellipse cx="86" cy="84" rx="22" ry="12" fill="#FFFFFF" opacity=".6" transform="rotate(-35 86 84)"/>')


# ---------------------------------------------------------------- the set
ICONS = {
    "folder": folder(),
    "folder-open": folder(open_=True),
    "documents": folder(em_quill),
    "downloads": folder(em_down),
    "pictures": folder(em_mountain),
    "music": folder(em_harp),
    "videos": folder(em_play),
    "desktop": folder(em_door),
    "user": folder(em_user),
    "favorites": folder(em_star),
    "search-folder": folder(em_search),
    "games": folder(em_sword),
    "shared": folder(em_share),
    "cloud": folder(em_cloud),
    "home": folder(em_home),
    "this-pc": this_pc,
    "drive": chest(),
    "drive-windows": chest(windows=True),
    "removable": parcel,
    "disc": shield,
    "recycle-empty": barrel(),
    "recycle-full": barrel(full=True),
    "network": palantir,
    "document": document,
    "search": magnifier,
}

# imageres.dll icon IDs -> icon name (IDs checked against a Windows 11 icon theme)
IMAGERES = {
    "folder": [3, 143, 190, 198],
    "folder-open": [4, 6, 10],
    "documents": [112, 1002],
    "downloads": [184],
    "pictures": [113, 1003],
    "music": [108, 1004],
    "videos": [189, 1005],
    "desktop": [183, 1001, 1014],
    "user": [123, 178, 181, 1010, 1029],
    "favorites": [115, 1020, 1024],
    "search-folder": [18, 1025],
    "games": [186],
    "shared": [175, 185],
    "cloud": [1040, 1043],
    "home": [1023],
    "this-pc": [15, 109, 149, 150, 151, 195],
    "drive": [31, 32, 33, 35, 58, 75, 91],
    "drive-windows": [36],
    "removable": [28, 29, 42, 43, 44, 48, 60, 65, 96],
    "disc": [30, 37, 38, 39, 40, 41, 56, 61, 62, 63, 64, 85, 135, 136, 137, 138, 139, 140, 141, 142, 146, 155, 172, 173, 180],
    "recycle-empty": [55],
    "recycle-full": [54],
    "network": [25, 120, 179],
    "document": [2, 5, 9],
    "search": [8, 145, 177],
}


def stroke_for(size):
    return {16: 16, 20: 14, 24: 12, 32: 10, 40: 9, 48: 8}.get(size, 7)


def render(draw, size):
    small = size <= 24
    png = cairosvg.svg2png(bytestring=svg(draw(stroke_for(size), small), size).encode(), output_width=size, output_height=size)
    return Image.open(io.BytesIO(png)).convert("RGBA")


def write_ico(images, path):
    """images: list of RGBA Images. 256 stored as PNG, the rest as 32-bit DIBs."""
    entries, blobs = [], []
    for im in images:
        w, h = im.size
        if w >= 256:
            b = io.BytesIO()
            im.save(b, "PNG")
            data = b.getvalue()
        else:
            hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
            px = im.transpose(Image.FLIP_TOP_BOTTOM).tobytes("raw", "BGRA")
            mask_row = ((w + 31) // 32) * 4
            data = hdr + px + b"\0" * (mask_row * h)
        entries.append((w, h, data))
    out = io.BytesIO()
    out.write(struct.pack("<HHH", 0, 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for w, h, data in entries:
        out.write(struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    for _, _, data in entries:
        out.write(data)
    Path(path).write_bytes(out.getvalue())


def build_dll(icon_dir, out_dll, work):
    rc = ["LANGUAGE 0, 0"]
    for name, ids in IMAGERES.items():
        ico = (icon_dir / f"{name}.ico").resolve().as_posix()
        for i in ids:
            rc.append(f'{i} ICON "{ico}"')
    rc_path = work / "lembas-icons.rc"
    rc_path.write_text("\n".join(rc) + "\n")
    res = work / "lembas-icons.res"
    subprocess.run(["llvm-rc", "/fo", str(res), str(rc_path)], check=True)
    subprocess.run(["lld-link", "/dll", "/noentry", "/machine:x86", "/nodefaultlib", "/safeseh:no",
                    f"/out:{out_dll}", str(res)], check=True)


def main(out):
    out = Path(out)
    icons = out / "Icons"
    theme = out / "IconTheme"
    work = out / "_work"
    for d in (icons, theme, work):
        d.mkdir(parents=True, exist_ok=True)
    big = {}
    for name, draw in ICONS.items():
        ims = [render(draw, s) for s in SIZES]
        write_ico(ims, icons / f"{name}.ico")
        big[name] = ims[-3]  # 64 px for the preview
        big[name + "@16"] = ims[0]
        big[name + "@32"] = ims[3]
    build_dll(icons, theme / "imageres.dll", work)
    (theme / "theme.ini").write_text("[redirections]\r\n%SystemRoot%\\System32\\imageres.dll=imageres.dll\r\n")
    shutil.rmtree(work)
    for extra in theme.glob("*.lib"):
        extra.unlink()
    # preview: light and dark rows, 64 / 32 / 16 px
    names = list(ICONS)
    cw = 100
    sheet = Image.new("RGB", (cw * len(names) // 2 + 20, 2 * 2 * 120), "#F3E8CD")
    d = ImageDraw.Draw(sheet)
    d.rectangle([0, sheet.height // 2, sheet.width, sheet.height], fill="#0B1020")
    half = (len(names) + 1) // 2
    for band, y0 in ((0, 0), (1, sheet.height // 2)):
        for k, n in enumerate(names):
            row, col = divmod(k, half)
            x = 10 + col * cw
            y = y0 + 10 + row * 120
            sheet.paste(big[n], (x, y), big[n])
            sheet.paste(big[n + "@32"], (x + 66, y + 4), big[n + "@32"])
            sheet.paste(big[n + "@16"], (x + 66, y + 42), big[n + "@16"])
            d.text((x, y + 70), n, fill="#2B1E12" if band == 0 else "#EDE3CB")
    sheet.save(out / "preview.png")
    print("icons:", len(ICONS), "-> imageres ids:", sum(len(v) for v in IMAGERES.values()))


if __name__ == "__main__":
    main(sys.argv[1])
