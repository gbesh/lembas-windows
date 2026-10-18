"""Render the four Lembas scenes to 3840x2160 JPEG wallpapers (no clock), plus the still
layers the Lembas Desktop app animates between (living/<scene>-back.jpg, -mid.png, -front.png).

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

# Which still layers each scene has (the app draws the moving parts in between)
LAYERS = {"shire": ["back", "mid", "front"], "redbook": ["back", "front"], "starlit": ["back", "front"], "elven": ["back"]}


async def render(out: Path):
    out.mkdir(parents=True, exist_ok=True)
    async with async_playwright() as p:
        browser = await p.chromium.launch()
        page = await browser.new_page(viewport={"width": 1920, "height": 1080}, device_scale_factor=2)
        for key, name in NAMES.items():
            await page.goto(f"{SCENE.as_uri()}?theme={key}&still=1&clock=0")
            await page.wait_for_timeout(800)
            png = out / f"{key}.png"
            await page.screenshot(path=str(png))
            Image.open(png).convert("RGB").save(out / f"{name}.jpg", quality=93, optimize=True, subsampling=0)
            png.unlink()
        # Still layers for the living desktop
        live = out / "living"
        live.mkdir(exist_ok=True)
        for key, layers in LAYERS.items():
            for layer in layers:
                await page.goto(f"{SCENE.as_uri()}?theme={key}&still=1&clock=0&layer={layer}")
                await page.wait_for_timeout(500)
                png = live / f"{key}-{layer}.png"
                await page.screenshot(path=str(png), omit_background=(layer != "back"))
                if layer == "back":
                    Image.open(png).convert("RGB").save(live / f"{key}-back.jpg", quality=92, optimize=True, subsampling=0)
                    png.unlink()
                else:
                    Image.open(png).save(png, optimize=True)
        await browser.close()


if __name__ == "__main__":
    asyncio.run(render(Path(sys.argv[1] if len(sys.argv) > 1 else "wall4k")))
