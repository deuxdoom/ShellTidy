"""Render fictional example data in ShellTidy's UI layout, never read user inventory."""
from io import BytesIO
from pathlib import Path
from html import escape
import os
import xml.etree.ElementTree as ET

from PIL import Image
import resvg_py

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "assets" / "screenshots"
DEST.mkdir(parents=True, exist_ok=True)
NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", NS)
FONT_DIR = Path(os.environ.get("SHELLTIDY_FONT_DIR", ROOT / "tools/runtime/fonts"))
FONTS = [str(FONT_DIR / name) for name in ("segoeui.ttf", "segoeuib.ttf", "seguisb.ttf", "malgun.ttf", "malgunbd.ttf")]
assert all(Path(file).is_file() for file in FONTS), "Run tools/setup-local-tools.ps1 first"


def text(x, y, value, size=12, color="#2D3A4C", weight=400, anchor="start"):
    return f'<text x="{x}" y="{y}" font-family="Segoe UI, Malgun Gothic, sans-serif" font-size="{size}" fill="{color}" font-weight="{weight}" text-anchor="{anchor}">{escape(value)}</text>'


def box(x, y, width, height, fill="#FFFFFF", stroke="#D6DCE5", radius=0):
    return f'<rect x="{x}" y="{y}" width="{width}" height="{height}" rx="{radius}" fill="{fill}" stroke="{stroke}"/>'


def checkbox(x, y, checked=False, disabled=False):
    fill = "#F0F2F5" if disabled else ("#1476D4" if checked else "#FFFFFF")
    stroke = "#C6CCD5" if disabled else ("#1476D4" if checked else "#8896A8")
    out = box(x, y, 13, 13, fill, stroke, 2)
    if checked:
        out += f'<path d="M{x+3} {y+6}l3 3 5-6" fill="none" stroke="#FFFFFF" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>'
    return out


def icon(name, x, y, size=14, opacity=1.0):
    node = ET.fromstring((ROOT / "assets/fluent" / f"{name}.svg").read_text(encoding="utf-8"))
    node.attrib.update(x=str(x), y=str(y), width=str(size), height=str(size), opacity=str(opacity))
    return ET.tostring(node, encoding="unicode")


def app_icon(x, y, size):
    node = ET.fromstring((ROOT / "assets/shelltidy.svg").read_text(encoding="utf-8"))
    node.attrib.update(x=str(x), y=str(y), width=str(size), height=str(size))
    return ET.tostring(node, encoding="unicode")


def combo(x, y, width, label):
    return box(x, y, width, 22, "#FFFFFF", "#D0D0D0", 3) + text(x + 7, y + 15, label, 11) + \
        f'<path d="M{x+width-15} {y+9}l4 4 4-4" fill="none" stroke="#6E6E6E"/>'


# All records below are invented names and paths. No registry or screenshot capture is used.
# (name, type, status, enabled, path, extensions, description, checked, tone)
MENUS = [
    ("Legacy Preview", "셸 확장", "경로 없음", "사용", r"C:\OldApps\Preview\menu.dll", "*", "", True, "problem"),
    ("OldViewer.exe", "연결 프로그램", "등록 없음", "", r"(등록 없음) Applications\OldViewer.exe", ".jpg .png", "", True, "problem"),
    ("Aster View로 열기", "셸 메뉴", "확인 필요", "사용", r"D:\Apps\AsterView\viewer.exe", "*", "", False, "normal"),
    ("Orbit Notes에 추가", "셸 메뉴", "정상", "사용 안 함", r"C:\Apps\OrbitNotes\notes.exe", "*", "Orbit Notes", False, "disabled"),
    ("Cloud Sync 공유", "셸 확장", "정상", "사용", r"C:\Apps\CloudSync\shellext.dll", "*", "Cloud Sync Shell", False, "normal"),
    ("Photo Tools", "최신 메뉴", "확인 필요", "사용", "ExplorerCommand · 조회 전용", ".jpg .png", "", False, "normal"),
    ("열기", "셸 메뉴", "시스템 보호", "", r"C:\Windows\explorer.exe", "Folder", "Windows Explorer", None, "protected"),
]


def frame(rows):
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="1416" height="960" viewBox="0 0 944 640">',
           '<title>ShellTidy UI preview using fictional example data</title>',
           '<desc>Constructed UI illustration. Names, paths and registrations are fictional.</desc>',
           box(.5, .5, 943, 639, "#F3F3F3", "#C9C9C9", 8),
           '<path d="M8 1H936Q943 1 943 8V30H1V8Q1 1 8 1" fill="#EEF2F8"/>',
           app_icon(10, 8, 15), text(32, 20, "ShellTidy", 11),
           text(840, 21, "−", 16), box(879, 11, 9, 9, "none", "#5F5F5F"),
           '<path d="M916 11l9 9m0-9-9 9" stroke="#5F5F5F"/>']
    x = 14
    for label in ["파일(F)", "편집(E)", "보기(V)", "옵션(O)", "도움말(H)"]:
        out.append(text(x, 45, label, 11, "#1B1B1B"))
        x += 52
    tools = [("refresh", "새로고침", 1.0), None, ("delete", "삭제", 1.0), ("disable", "사용 안 함", 1.0),
             ("enable", "사용", 0.35), None, ("restore", "복원", 1.0), ("backups", "백업", 1.0)]
    x = 14
    for item in tools:
        if item is None:
            out.append(f'<path d="M{x+4} 58V76" stroke="#D0D0D0"/>')
            x += 12
            continue
        name, label, opacity = item
        out += [icon(name, x, 60, 14, opacity), text(x + 18, 72, label, 11, "#1B1B1B" if opacity == 1 else "#A8A8A8")]
        x += 30 + len(label) * 11
    out += [box(480, 56, 230, 22, "#FFFFFF", "#D0D0D0", 3), text(487, 71, "검색 (Ctrl+F)", 11, "#8A8A8A"),
            combo(716, 56, 104, "모든 유형"), combo(826, 56, 108, "모든 상태")]
    out += [box(10, 86, 924, 424, "#FFFFFF", "#E0E0E0")]
    columns = [(10, "이름"), (170, "유형"), (256, "상태 ↑"), (330, "사용 여부"), (398, "명령 / 파일 경로"),
               (704, "확장자"), (790, "설명")]
    for x, label in columns:
        if x > 10:
            out.append(f'<path d="M{x} 87V108" stroke="#E5E5E5"/>')
        out.append(text(x + 8, 102, label, 11, "#4C4C4C"))
    out.append('<path d="M11 108H933" stroke="#EAEAEA"/>')
    for i, (name, kind, status, enabled, path, exts, desc, checked, tone) in enumerate(rows):
        y = 109 + i * 26
        fill = "#CCE8FF" if i == 0 else ("#FDE7E9" if tone == "problem" else None)
        if fill:
            out.append(box(11, y, 922, 26, fill, "none"))
        color = {"disabled": "#8A8A8A", "protected": "#707070"}.get(tone, "#1B1B1B")
        if checked is not None:
            out.append(checkbox(16, y + 7, checked))
        values = [(36, name), (178, kind), (264, status), (338, enabled), (406, path), (712, exts), (798, desc)]
        for vx, value in values:
            out.append(text(vx, y + 17, value, 11 if vx < 400 else 10.5, color))
    out += [box(10, 518, 924, 86, "#FFFFFF", "#E0E0E0")]
    info = ["Legacy Preview  |  셸 확장  |  경로 없음  |  사용",
            r"명령: C:\OldApps\Preview\menu.dll",
            "Only the ContextMenuHandlers reference is removed; shared CLSID is preserved / 핸들러 참조만 삭제, CLSID 보존",
            r"HKCU\Software\Classes\*\shellex\ContextMenuHandlers\LegacyPreview [key] [64-bit]"]
    for i, value in enumerate(info):
        out.append(text(20, 537 + i * 18, value, 10.5, "#323232"))
    checked = sum(1 for row in rows if row[7])
    out += ['<path d="M1 614H943" stroke="#E0E0E0"/>',
            text(20, 632, f"표시 {len(rows)} / {len(rows)} · 경로 없음 1 · 등록 없음 1 · 사용 안 함 1 · 체크 {checked}",
                 11, "#606060")]
    return out


def save(name, parts):
    source="".join(parts)+"</svg>"
    (DEST / f"{name}.svg").write_text(source, encoding="utf-8")
    png=resvg_py.svg_to_bytes(svg_string=source,width=1416,height=960,skip_system_fonts=True,font_files=FONTS)
    image=Image.open(BytesIO(png)).convert("RGB")
    image.save(DEST / f"{name}.png", optimize=True)
    print(f"Created fictional UI preview: {DEST / (name+'.png')}")


save("context-menus", frame(MENUS))
