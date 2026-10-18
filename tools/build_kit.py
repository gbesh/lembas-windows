"""Assemble "Lembas for Windows" from the repo + rendered assets.

Usage: python build_kit.py <assets_dir> <out_dir>
  assets_dir must contain: wall4k/*.jpg, wall4k/living/, desktop/*.exe, cursors/, sounds/, fonts/, icons/  (made by the other scripts)
"""
import json
import os
import math
import shutil
import sys
import zipfile
from pathlib import Path

import yaml
from PIL import Image, ImageDraw, ImageFilter

REPO = Path(__file__).resolve().parents[2]  # (unused)
sys.path.insert(0, str(Path(__file__).resolve().parent))
WIN = Path(__file__).resolve().parents[1]   # the folder holding tools/, scripts/, windhawk/...
NAMES = {
    "shire": "Lembas - Shire Morning",
    "redbook": "Lembas - Red Book",
    "starlit": "Lembas - Starlit Night",
    "elven": "Lembas - Elven Silver",
}


WINDHAWK_MODS = {
    "1-taskbar-styler": ("windows-11-taskbar-styler", "Windows 11 Taskbar Styler"),
    "2-start-menu-styler": ("windows-11-start-menu-styler", "Windows 11 Start Menu Styler"),
    "3-notification-center-styler": ("windows-11-notification-center-styler", "Windows 11 Notification Center Styler"),
    "4-taskbar-clock": ("taskbar-clock-customization", "Taskbar Clock Customization"),
    "5-file-explorer-styler": ("windows-11-file-explorer-styler", "Windows 11 File Explorer Styler"),
    "6-icons-resource-redirect": ("icon-resource-redirect", "Resource Redirect"),
    "7-translucent-windows": ("translucent-windows", "Translucent Windows"),
    "8-settings-styler": ("windows-11-settings-styler", "Windows 11 Settings Styler"),
    "9-window-borders": ("window-border-customizer", "Window Border Customizer"),
    "11-file-operation-styler": ("file-operation-styler", "File Operation Styler"),
    "12-explorer-visual-tweaks": ("explorer-visual-tweaks-dark", "Explorer Visual Tweaks Dark"),
}


# Translucent Windows skips its window painting on ARM PCs, so it is only installed on x64.
WINDHAWK_ARCH = {"7-translucent-windows": "x64"}
# Mods that need no settings
WINDHAWK_EXTRA = [("explorer-glyphs-colorizer", "Explorer Glyphs Colorizer")]
# Mods earlier Lembas versions installed that this one removes
WINDHAWK_RETIRED = ["explorer-font-changer"]


def flatten(value, prefix=""):
    """Windhawk's own flattening: a.b, a[0], booleans stored as 0/1."""
    flat = {}
    if isinstance(value, dict):
        for k, v in value.items():
            flat.update(flatten(v, f"{prefix}.{k}" if prefix else k))
    elif isinstance(value, list):
        for i, v in enumerate(value):
            flat.update(flatten(v, f"{prefix}[{i}]"))
    elif isinstance(value, bool):
        flat[prefix] = int(value)
    elif value is None:
        flat[prefix] = ""
    else:
        flat[prefix] = value
    return flat


def emblem(path, size=128):
    """Start button: an eight-point star inside a gold ring."""
    ss = 8
    S = size * ss
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = S / 2
    r = S * 0.44
    d.ellipse([c - r, c - r, c + r, c + r], outline=(31, 22, 13, 255), width=int(S * 0.085))
    d.ellipse([c - r, c - r, c + r, c + r], outline=(201, 160, 76, 255), width=int(S * 0.06))
    r2 = r - S * 0.02
    d.ellipse([c - r2, c - r2, c + r2, c + r2], outline=(246, 227, 168, 255), width=int(S * 0.018))
    pts = []
    for i in range(16):
        a = -math.pi / 2 + i * math.pi / 8
        rr = S * 0.31 if i % 4 == 0 else (S * 0.17 if i % 2 == 0 else S * 0.07)
        pts.append((c + rr * math.cos(a), c + rr * math.sin(a)))
    d.polygon(pts, fill=(246, 236, 210, 255), outline=(31, 22, 13, 255), width=int(S * 0.012))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([c - S * .06, c - S * .06, c + S * .06, c + S * .06], fill=(255, 248, 230, 200))
    im.alpha_composite(glow.filter(ImageFilter.GaussianBlur(S * 0.03)))
    im.resize((size, size), Image.LANCZOS).save(path)


def home_art(wall_dir, out_dir):
    """Backgrounds for File Explorer's Home page, one per look, toned so text stays readable:
    the day scenes washed toward parchment, the night scenes deepened toward their ink."""
    from PIL import ImageEnhance
    tone = {"shire": ((242, 244, 228), 0.6, 0.75), "redbook": ((246, 236, 212), 0.6, 0.75),
            "starlit": ((8, 11, 26), 0.3, 1.0), "elven": ((9, 12, 15), 0.3, 1.0)}
    for key, (rgb, amount, colour) in tone.items():
        im = Image.open(wall_dir / f"{NAMES[key]}.jpg").convert("RGB").resize((1920, 1080), Image.LANCZOS)
        im = ImageEnhance.Color(im).enhance(colour)
        im = Image.blend(im, Image.new("RGB", im.size, rgb), amount)
        im.save(out_dir / f"home-{key}.jpg", quality=86)


def browser_theme(folder, title, wallpaper, colors, tints):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "images").mkdir(exist_ok=True)
    Image.open(wallpaper).convert("RGB").resize((1920, 1080), Image.LANCZOS).save(folder / "images" / "ntp.jpg", quality=88)
    manifest = {
        "manifest_version": 3,
        "name": title,
        "version": "1.0",
        "description": "Lembas theme for Chrome and Edge.",
        "theme": {
            "images": {"theme_ntp_background": "images/ntp.jpg"},
            "colors": colors,
            "tints": tints,
            "properties": {"ntp_background_alignment": "bottom", "ntp_background_repeat": "no-repeat"},
        },
    }
    (folder / "manifest.json").write_text(json.dumps(manifest, indent=2))


def zip_dir(src, dest, root_name=None):
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(src.rglob("*")):
            if p.is_file():
                arc = p.relative_to(src)
                if root_name:
                    arc = Path(root_name) / arc
                z.write(p, arc.as_posix())


def main(assets, out):
    assets = Path(assets)
    out = Path(out)
    if out.exists():
        shutil.rmtree(out)
    pay = out / "payload"
    pay.mkdir(parents=True)

    # Wallpapers
    shutil.copytree(assets / "wall4k", pay / "Wallpapers", ignore=shutil.ignore_patterns("living"))
    # Lembas Desktop: the living-desktop app (ARM64 and x64 builds) and the still scene layers it animates over
    live = pay / "Living"
    shutil.copytree(assets / "wall4k" / "living", live)
    for exe in sorted((assets / "desktop").glob("LembasDesktop-*.exe")):
        shutil.copy2(exe, live / exe.name)
    # Cursors, sounds, fonts
    shutil.copytree(assets / "cursors", pay / "Cursors", ignore=shutil.ignore_patterns("preview.png"))
    shutil.copytree(assets / "sounds", pay / "Sounds")
    shutil.copytree(assets / "fonts", pay / "Fonts")
    # Start button emblem
    (pay / "Images").mkdir()
    emblem(pay / "Images" / "start.png")
    home_art(assets / "wall4k", pay / "Images")
    # Windhawk: the base styles as YAML (readable, and paste-able by hand), plus one JSON set per
    # look that the installer and the sun task write straight into Windhawk with its command line
    import palettes
    shutil.copytree(WIN / "windhawk", pay / "Windhawk")
    yamls = sorted((WIN / "windhawk").glob("*.yaml"), key=lambda p: int(p.name.split("-")[0]))
    mods = [{"mod": WINDHAWK_MODS[f.stem][0], "name": WINDHAWK_MODS[f.stem][1],
             "arch": WINDHAWK_ARCH.get(f.stem, "any")} for f in yamls]
    mods += [{"mod": m, "name": n, "arch": "any"} for m, n in WINDHAWK_EXTRA]
    (pay / "Windhawk" / "mods.json").write_text(json.dumps({"install": mods, "retire": WINDHAWK_RETIRED}, indent=1), encoding="utf-8")
    (pay / "Windhawk" / "phases").mkdir()
    for phase in palettes.PHASES:
        styles = []
        for f in yamls:
            text = palettes.apply(f.read_text(encoding="utf-8"), phase)
            styles.append({"mod": WINDHAWK_MODS[f.stem][0], "settings": flatten(yaml.safe_load(text))})
        (pay / "Windhawk" / "phases" / f"{phase}.json").write_text(json.dumps(styles, indent=1, ensure_ascii=False), encoding="utf-8")
    looks = {k: {kk: v[kk] for kk in ("name", "light", "wallpaper", "accent")} for k, v in palettes.LOOKS.items()}
    (pay / "looks.json").write_text(json.dumps(looks, indent=1), encoding="utf-8")
    # Icons: plain .ico files and the Resource Redirect theme
    shutil.copytree(assets / "icons" / "Icons", pay / "Icons")
    shutil.copytree(assets / "icons" / "IconTheme", pay / "IconTheme")
    # Windows Terminal
    (pay / "Terminal").mkdir()
    shutil.copy(WIN / "terminal" / "lembas.json", pay / "Terminal" / "lembas.json")
    # Browser themes (Chrome / Edge)
    browser_theme(pay / "Browser" / "Lembas Starlit Night", "Lembas - Starlit Night", assets / "wall4k" / f"{NAMES['starlit']}.jpg", {
        "frame": [11, 16, 32], "frame_inactive": [8, 12, 24], "frame_incognito": [14, 19, 22],
        "toolbar": [22, 28, 50], "tab_text": [237, 227, 203], "tab_background_text": [185, 177, 156],
        "bookmark_text": [237, 227, 203], "ntp_background": [11, 16, 32], "ntp_text": [237, 227, 203],
        "ntp_link": [217, 188, 122], "omnibox_background": [12, 17, 34], "omnibox_text": [237, 227, 203],
        "toolbar_button_icon": [217, 188, 122], "toolbar_text": [237, 227, 203],
    }, {"buttons": [0.12, 0.55, 0.78]})
    browser_theme(pay / "Browser" / "Lembas Red Book", "Lembas - Red Book", assets / "wall4k" / f"{NAMES['redbook']}.jpg", {
        "frame": [222, 204, 164], "frame_inactive": [232, 219, 190], "frame_incognito": [70, 52, 32],
        "toolbar": [245, 235, 210], "tab_text": [43, 30, 18], "tab_background_text": [106, 82, 54],
        "bookmark_text": [43, 30, 18], "ntp_background": [243, 232, 205], "ntp_text": [43, 30, 18],
        "ntp_link": [154, 42, 28], "omnibox_background": [252, 246, 232], "omnibox_text": [43, 30, 18],
        "toolbar_button_icon": [154, 42, 28], "toolbar_text": [43, 30, 18],
    }, {"buttons": [0.02, 0.7, 0.36]})
    browser_theme(pay / "Browser" / "Lembas Shire Morning", "Lembas - Shire Morning", assets / "wall4k" / f"{NAMES['shire']}.jpg", {
        "frame": [148, 179, 115], "frame_inactive": [185, 207, 152], "frame_incognito": [47, 74, 42],
        "toolbar": [251, 244, 227], "tab_text": [47, 74, 42], "tab_background_text": [47, 74, 42],
        "bookmark_text": [47, 74, 42], "ntp_background": [238, 242, 226], "ntp_text": [47, 74, 42],
        "ntp_link": [46, 94, 58], "omnibox_background": [238, 240, 220], "omnibox_text": [47, 74, 42],
        "toolbar_button_icon": [47, 74, 42], "toolbar_text": [47, 74, 42],
    }, {"buttons": [0.28, 0.35, 0.3]})
    browser_theme(pay / "Browser" / "Lembas Elven Silver", "Lembas - Elven Silver", assets / "wall4k" / f"{NAMES['elven']}.jpg", {
        "frame": [14, 19, 22], "frame_inactive": [10, 13, 16], "frame_incognito": [20, 24, 30],
        "toolbar": [24, 31, 36], "tab_text": [230, 236, 238], "tab_background_text": [142, 154, 158],
        "bookmark_text": [214, 222, 224], "ntp_background": [14, 19, 22], "ntp_text": [214, 222, 224],
        "ntp_link": [169, 198, 214], "omnibox_background": [16, 21, 25], "omnibox_text": [230, 236, 238],
        "toolbar_button_icon": [214, 222, 224], "toolbar_text": [214, 222, 224],
    }, {"buttons": [0.55, 0.1, 0.85]})

    # Scripts: everything lives in payload\Tools so the installed copy can update, preview,
    # change look and uninstall itself; the kit root only has the two one-click launchers.
    (pay / "Tools").mkdir()
    for f in (WIN / "scripts").glob("*.ps1"):
        (pay / "Tools" / f.name).write_text(f.read_text(encoding="utf-8"), encoding="utf-8-sig")  # BOM for Windows PowerShell
    for name in ("Install Lembas.cmd", "Uninstall Lembas.cmd"):
        text = (WIN / "scripts" / name).read_text()
        (out / name).write_bytes(text.replace("\r\n", "\n").replace("\n", "\r\n").encode("ascii"))
    (pay / "version.txt").write_text(os.environ.get("LEMBAS_VERSION", "dev"))
    readme = (WIN / "README.txt").read_text(encoding="utf-8")
    (out / "README.txt").write_bytes(("﻿" + readme).replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))
    print("kit assembled at", out)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
