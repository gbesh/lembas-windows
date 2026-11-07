"""Render the scene layers the Lembas phone launcher animates between, from scenes/index.html.

  <out>/scene_<look>_<season>_p_<layer>.webp   upright (1350x3000: the 1080x2400 design at 1.25x)
  <out>/scene_<look>_<season>_l_<layer>.webp   the panorama for tablets and landscape (2880x1080)

The back layer is opaque; the others are see-through. Usage:
  python render_phone_scenes.py <lembas-android>/app/src/main/assets/scenes [look ...]
"""
import asyncio
import io
import sys
from pathlib import Path

from PIL import Image
from playwright.async_api import async_playwright

SCENE = Path(__file__).resolve().parents[1] / "scenes" / "index.html"
SEASONS = ["spring", "summer", "autumn", "winter"]
LAYERS = {"shire": ["back", "mid", "front"], "redbook": ["back"], "starlit": ["back", "front"], "elven": ["back", "mid"],
          "lorien": ["back", "mid", "front"], "havens": ["back", "mid", "front"]}


def save(png: bytes, out: Path, opaque: bool):
    im = Image.open(io.BytesIO(png))
    if opaque:
        im.convert("RGB").save(out, "WEBP", quality=82, method=6)
    else:
        im.convert("RGBA").save(out, "WEBP", quality=84, alpha_quality=90, method=6)


async def render(out: Path, looks):
    out.mkdir(parents=True, exist_ok=True)
    async with async_playwright() as p:
        browser = await p.chromium.launch()
        tall = await browser.new_page(viewport={"width": 1080, "height": 2400}, device_scale_factor=1.25)
        wide = await browser.new_page(viewport={"width": 2880, "height": 1080}, device_scale_factor=1)
        for look in looks:
            for season in SEASONS:
                for fmt, page, tag in (("phone", tall, "p"), ("pc", wide, "l")):
                    for layer in LAYERS[look]:
                        await page.goto(f"{SCENE.as_uri()}?fmt={fmt}&theme={look}&season={season}&still=1&layer={layer}")
                        await page.wait_for_timeout(500)
                        png = await page.screenshot(omit_background=(layer != "back"))
                        save(png, out / f"scene_{look}_{season}_{tag}_{layer}.webp", layer == "back")
        await browser.close()


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "scenes")
    asyncio.run(render(out, sys.argv[2:] or list(LAYERS)))
