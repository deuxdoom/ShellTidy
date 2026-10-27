"""Export the English GitHub social card from editable SVG (development only).

Optional tools: Pillow and resvg_py, as used by generate_icons.py.
All output is written to this project's assets directory.
"""
from io import BytesIO
from pathlib import Path
import xml.etree.ElementTree as ET
import os

from PIL import Image
import resvg_py

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
ET.register_namespace("", "http://www.w3.org/2000/svg")
icon = ET.fromstring((ASSETS / "shelltidy.svg").read_text(encoding="utf-8"))
icon.attrib.update(x="80", y="139", width="76", height="76")
icon_svg = ET.tostring(icon, encoding="unicode")

svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="640" viewBox="0 0 1280 640">
  <title>ShellTidy — Windows Context Menu Cleaner</title>
  <desc>English social card. Important content stays inside the template's 80-pixel safe margin.</desc>
  <defs>
    <linearGradient id="card-background" x1="0" y1="0" x2="1280" y2="640" gradientUnits="userSpaceOnUse">
      <stop stop-color="#FFFFFF"/><stop offset="1" stop-color="#F0F5FD"/>
    </linearGradient>
    <radialGradient id="card-glow"><stop stop-color="#CFE1FF" stop-opacity=".7"/><stop offset="1" stop-color="#EAF2FF" stop-opacity="0"/></radialGradient>
    <filter id="menu-shadow" x="-25%" y="-25%" width="150%" height="165%">
      <feDropShadow dx="0" dy="16" stdDeviation="18" flood-color="#385985" flood-opacity=".12"/>
      <feDropShadow dx="0" dy="2" stdDeviation="3" flood-color="#385985" flood-opacity=".06"/>
    </filter>
  </defs>
  <rect width="1280" height="640" fill="url(#card-background)"/>
  <ellipse cx="1034" cy="331" rx="390" ry="370" fill="url(#card-glow)"/>
  <path d="M0 606H1280" stroke="#DFE8F4"/>
  <g font-family="Segoe UI, Arial, sans-serif">
    <text x="80" y="103" font-size="13" font-weight="600" letter-spacing="2.4" fill="#356DE1">WINDOWS CONTEXT MENU CLEANER</text>
    {icon_svg}
    <text x="177" y="199" font-size="72" font-weight="700" letter-spacing="-2.8" fill="#1D2B42">ShellTidy</text>
    <text x="80" y="294" font-size="46" font-weight="600" letter-spacing="-1.3" fill="#1D2B42">A cleaner Windows</text>
    <text x="80" y="351" font-size="46" font-weight="600" letter-spacing="-1.3" fill="#356DE1">context menu.</text>
    <text x="80" y="402" font-size="23" fill="#687991">Remove stale Open With registrations.</text>

    <rect x="80" y="442" width="144" height="36" rx="7" fill="#ECF3FF"/>
    <text x="152" y="466" text-anchor="middle" font-size="14" font-weight="600" fill="#356DE1">Portable EXE</text>
    <rect x="235" y="442" width="179" height="36" rx="7" fill="#F5F8FC" stroke="#DCE5F1"/>
    <text x="324.5" y="466" text-anchor="middle" font-size="14" font-weight="600" fill="#60738D">Windows 10/11 x64</text>
    <rect x="425" y="442" width="133" height="36" rx="7" fill="#F5F8FC" stroke="#DCE5F1"/>
    <text x="491.5" y="466" text-anchor="middle" font-size="14" font-weight="600" fill="#60738D">MIT License</text>

    <g filter="url(#menu-shadow)">
      <rect x="816" y="164" width="384" height="326" rx="16" fill="#FFFFFF"/>
    </g>
    <rect x="816.5" y="164.5" width="383" height="325" rx="15.5" fill="none" stroke="#D4DFEF"/>
    <path d="M817 219H1199" stroke="#E5EBF4"/>
    <text x="841" y="199" font-size="19" font-weight="600" fill="#344762">Open With</text>
    <circle cx="1158" cy="191" r="2" fill="#99A8BD"/><circle cx="1167" cy="191" r="2" fill="#99A8BD"/><circle cx="1176" cy="191" r="2" fill="#99A8BD"/>

    <rect x="840" y="245" width="19" height="19" rx="4" fill="#ECF3FF" stroke="#AEC7F1"/>
    <text x="875" y="262" font-size="20" fill="#4C607C">Current app</text>
    <rect x="1092" y="241" width="82" height="29" rx="6" fill="#EDF7F3"/>
    <text x="1133" y="261" text-anchor="middle" font-size="14" font-weight="600" fill="#398974">Keep</text>

    <rect x="831" y="287" width="354" height="58" rx="8" fill="#F4F7FE"/>
    <rect x="840" y="307" width="19" height="19" rx="4" fill="#477DE3"/>
    <path d="M845 316L849 320L855 312" fill="none" stroke="#FFFFFF" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>
    <text x="875" y="324" font-size="20" fill="#4C607C">Old app path</text>
    <rect x="1092" y="303" width="82" height="29" rx="6" fill="#FCEFEB"/>
    <text x="1133" y="323" text-anchor="middle" font-size="14" font-weight="600" fill="#BD6259">Remove</text>

    <rect x="831" y="350" width="354" height="58" rx="8" fill="#F4F7FE"/>
    <rect x="840" y="370" width="19" height="19" rx="4" fill="#477DE3"/>
    <path d="M845 379L849 383L855 375" fill="none" stroke="#FFFFFF" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>
    <text x="875" y="387" font-size="20" fill="#4C607C">Unused entry</text>
    <rect x="1092" y="366" width="82" height="29" rx="6" fill="#FCEFEB"/>
    <text x="1133" y="386" text-anchor="middle" font-size="14" font-weight="600" fill="#BD6259">Remove</text>

    <path d="M840 428H1176" stroke="#E5EBF4"/>
    <text x="1008" y="461" text-anchor="middle" font-size="15" font-weight="600" fill="#6D7F97">Back up. Clean up. Restore.</text>
    <text x="80" y="544" font-size="18" fill="#7C8BA2">github.com/deuxdoom/ShellTidy</text>
    <text x="1200" y="544" text-anchor="end" font-size="16" font-weight="600" fill="#6D7F97">v0.0.2</text>
  </g>
</svg>
'''
(ASSETS / "social-preview.svg").write_text(svg, encoding="utf-8")
font_dir = Path(os.environ.get("SHELLTIDY_FONT_DIR", ROOT / "tools/runtime/fonts"))
fonts = [str(font_dir / name) for name in ("segoeui.ttf", "segoeuib.ttf", "seguisb.ttf") if (font_dir / name).is_file()]
assert fonts, "Run tools/setup-local-tools.ps1 first"
png = resvg_py.svg_to_bytes(svg_string=svg, width=1280, height=640,
                            skip_system_fonts=True, font_files=fonts)
image = Image.open(BytesIO(png)).convert("RGB")
assert image.size == (1280, 640)
destination = ASSETS / "social-preview.png"
image.save(destination, optimize=True)
assert destination.stat().st_size < 1_000_000
print(f"Created {destination}: 1280x640, {destination.stat().st_size:,} bytes; safe margin 80px")
