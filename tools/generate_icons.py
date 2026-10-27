"""Regenerate committed PNG/ICO assets. Development only: Pillow + resvg_py.

python -m pip install --target F:\\temp\\ShellTidy\\icon-tools Pillow resvg_py
Set PYTHONPATH to that directory, then python tools/generate_icons.py.
The distributed EXE has no Python or SVG renderer dependency.
"""
from pathlib import Path
from io import BytesIO
import resvg_py
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"


def render(svg: Path, target: Path, size: int) -> Image.Image:
    source = svg.read_text(encoding="utf-8")
    png = resvg_py.svg_to_bytes(svg_string=source, width=size, height=size)
    image = Image.open(BytesIO(png)).convert("RGBA")
    image.save(target)
    return image


image = render(ASSETS / "shelltidy.svg", ASSETS / "shelltidy.png", 256)
image.save(ASSETS / "shelltidy.ico", sizes=[(n, n) for n in (16, 20, 24, 32, 48, 64, 128, 256)])
for name in ("refresh", "delete", "restore", "backups", "disable", "enable"):
    image = render(ASSETS / "fluent" / f"{name}.svg", ASSETS / "fluent" / f"{name}.png", 64)
    image.save(ASSETS / "fluent" / f"{name}.ico", sizes=[(n, n) for n in (16, 20, 24, 32, 48, 64)])
print("Generated app PNG/ICO and six Fluent UI button icons.")
