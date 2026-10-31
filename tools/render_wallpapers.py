"""Render the four Lembas scenes, in all four seasons, from scenes/index.html:

  <out>/Lembas - <Look>.jpg                     the summer wallpaper (also used for the browser themes)
  <out>/Seasons/<season>/Lembas - <Look>.jpg    the wallpaper for each season (Lembas Sun picks one)
  <out>/living/<season>/<look>-back.jpg, -mid.png, -front.png
                                                the still layers the Lembas Desktop app animates between

Wallpapers are 5760x2160 panoramas (a 16:9 screen shows the middle 3840, an ultrawide more);
the living layers are 4320x1620 (the app scales them to each monitor).

Usage: python render_wallpapers.py <out_dir>
"""
import asyncio
import sys
from pathlib import Path

from PIL import Image
from playwright.async_api import async_playwright

SCENE = Path(__file__).resolve().parents[1] / "scenes" / "index.html"
NAMES = {
    "shire": "Lembas - Shire Morning",
    "redbook": "Lembas - Red Book",
    "starlit": "Lembas - Starlit Night",
    "elven": "Lembas - Elven Silver",
}
SEASONS = ["spring", "summer", "autumn", "winter"]
# Which still layers each scene has (the app draws the moving parts in between)
LAYERS = {"shire": ["back", "mid", "front"], "redbook": ["back"], "starlit": ["back", "front"], "elven": ["back", "mid"]}


def save_layer(png: Path, out: Path):
    im = Image.open(png).convert("RGBA")
    # flat colours: a 256-colour palette with alpha is a fraction of the size and looks the same
    q = im.quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    q.save(out, optimize=True)


async def render(out: Path):
    out.mkdir(parents=True, exist_ok=True)
    async with async_playwright() as p:
        browser = await p.chromium.launch()
        wide = await browser.new_page(viewport={"width": 2880, "height": 1080}, device_scale_factor=2)
        layer_page = await browser.new_page(viewport={"width": 2880, "height": 1080}, device_scale_factor=1.5)
        for season in SEASONS:
            sdir = out / "Seasons" / season
            sdir.mkdir(parents=True, exist_ok=True)
            ldir = out / "living" / season
            ldir.mkdir(parents=True, exist_ok=True)
            for key, name in NAMES.items():
                await wide.goto(f"{SCENE.as_uri()}?theme={key}&season={season}&still=1")
                await wide.wait_for_timeout(900)
                png = sdir / f"{key}.png"
                await wide.screenshot(path=str(png))
                Image.open(png).convert("RGB").save(sdir / f"{name}.jpg", quality=88, optimize=True, subsampling=0)
                png.unlink()
                if season == "summer":
                    (out / f"{name}.jpg").write_bytes((sdir / f"{name}.jpg").read_bytes())
                for layer in LAYERS[key]:
                    await layer_page.goto(f"{SCENE.as_uri()}?theme={key}&season={season}&still=1&layer={layer}")
                    await layer_page.wait_for_timeout(500)
                    png = ldir / f"{key}-{layer}.raw.png"
                    await layer_page.screenshot(path=str(png), omit_background=(layer != "back"))
                    if layer == "back":
                        Image.open(png).convert("RGB").save(ldir / f"{key}-back.jpg", quality=90, optimize=True, subsampling=0)
                    else:
                        save_layer(png, ldir / f"{key}-{layer}.png")
                    png.unlink()
        await browser.close()


if __name__ == "__main__":
    asyncio.run(render(Path(sys.argv[1] if len(sys.argv) > 1 else "wall4k")))
