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


def button(x, y, width, label, primary=False):
    name={"새로고침":"refresh","선택 항목 삭제":"delete","마지막 작업 복원":"restore","백업 목록":"backups"}.get(label)
    out=box(x, y, width, 27, "#EAF2FC" if primary else "#FFFFFF", "#8FB2DC" if primary else "#CDD4DF", 4)
    if name:
        icon=ET.fromstring((ROOT / "assets/fluent" / f"{name}.svg").read_text(encoding="utf-8"))
        icon.attrib.update(x=str(x+7),y=str(y+7),width="14",height="14")
        out+=ET.tostring(icon,encoding="unicode")
    return out+text(x+width/2+(7 if name else 0), y+18, label, 11, anchor="middle")


def app_icon(x, y, size):
    node = ET.fromstring((ROOT / "assets/shelltidy.svg").read_text(encoding="utf-8"))
    node.attrib.update(x=str(x), y=str(y), width=str(size), height=str(size))
    return ET.tostring(node, encoding="unicode")


# All records below are invented names and paths. No registry or screenshot capture is used.
MENUS = [
    ("Aster View로 열기", "셸 메뉴", "정상", r"C:\Apps\AsterView\viewer.exe", "HKCU · shared", False, False),
    ("Legacy Preview", "셸 확장", "경로 없음", r"C:\OldApps\Preview\menu.dll", "HKCU · shared", True, False),
    ("Orbit Notes", "셸 메뉴", "정상", r"C:\Apps\OrbitNotes\notes.exe", "HKCU · shared", False, False),
    ("Windows 기본 메뉴", "셸 메뉴", "시스템 보호", r"C:\Windows\explorer.exe", "HKLM · shared", False, True),
    ("Example Package", "최신/패키지 · 조회", "확인 필요", "패키지 COM 후보 · 조회 전용", "HKLM · shared", False, True),
]


def frame(rows):
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="1416" height="960" viewBox="0 0 944 640">',
           '<title>ShellTidy UI preview using fictional example data</title>',
           '<desc>Constructed UI illustration. Names, paths and registrations are fictional.</desc>',
           box(.5, .5, 943, 639, "#F8F9FB", "#BFC8D5", 8),
           '<path d="M8 1H936Q943 1 943 8V30H1V8Q1 1 8 1" fill="#F1F5FB"/>',
           app_icon(10, 8, 15), text(32, 20, "ShellTidy", 11),
           text(840, 21, "−", 16), box(879, 11, 9, 9, "none", "#627188"),
           '<path d="M916 11l9 9m0-9-9 9" stroke="#627188"/>',
           text(20, 74, "ShellTidy", 25, "#1D2B42", 600),
           text(21, 97, "불필요한 연결 프로그램과 우클릭 메뉴를 정리합니다.", 11, "#68788E")]
    for x, width, label, active in [(20, 111, "연결 프로그램", False), (131, 108, "우클릭 메뉴", True), (239, 49, "전체", False)]:
        out += [box(x, 114, width, 25, "#FFFFFF" if active else "#F0F2F6", "#D9DFE9"), text(x+width/2, 131, label, 11, anchor="middle")]
    out += [box(526, 114, 240, 25), text(535, 131, "프로그램, 경로, 확장자 검색", 10, "#93A0B1"),
            box(777, 114, 147, 25), text(786, 131, "전체 상태", 11), '<path d="M910 124l4 4 4-4" fill="none" stroke="#8190A4"/>',
            box(20, 150, 904, 291), box(21, 151, 902, 27, "#F7F9FC", "none")]
    columns = [20, 167, 288, 404, 811, 924]
    for x in columns[1:-1]:
        out += [f'<path d="M{x} 151V440" stroke="#EDF0F5"/>']
    for x, label in [(31,"프로그램 ↑"),(178,"유형"),(299,"상태"),(415,"실행 경로"),(821,"등록 위치")]:
        out += [text(x, 169, label, 11, "#56657A")]
    for i, (name, kind, status, path, source, checked, disabled) in enumerate(rows):
        y = 179+i*29
        if i == 1:
            out += [box(21, y, 902, 29, "#EDF4FD", "none")]
        color = "#8C96A5" if disabled else ("#AA554A" if status=="경로 없음" else "#354256")
        out += [checkbox(25, y+8, checked, disabled), text(44,y+21,name,11,color), text(178,y+21,kind,10,color),
                text(299,y+21,status,11,color), text(415,y+21,path,10,color), text(821,y+21,source,9,color)]
    out += [box(20, 454, 904, 91, "#FAFBFD", "#DCE2EA")]
    info = ["Legacy Preview | 경로 없음", r"명령: C:\OldApps\Preview\menu.dll", "형식: ContextMenuHandlers 참조만 삭제 · 공유 CLSID 보존", r"HKCU\Software\Classes\*\shellex\ContextMenuHandlers\LegacyPreview"]
    for i, value in enumerate(info):
        out += [text(28,472+i*18,value,10,"#65758B")]
    selected = sum(row[5] for row in rows)
    out += [text(20, 566, f"{len(rows)}개 표시 / 전체 {len(rows)}개  ·  선택 {selected}", 11, "#65758B"),
            checkbox(21, 579), text(41,590,"정리 완료 후 Windows 탐색기 자동 재시작",11),
            button(20,603,90,"새로고침"), button(120,603,127,"선택 항목 삭제",True),
            button(684,603,139,"마지막 작업 복원"),button(833,603,91,"백업 목록")]
    return out


def save(name, parts):
    source="".join(parts)+"</svg>"
    (DEST / f"{name}.svg").write_text(source, encoding="utf-8")
    png=resvg_py.svg_to_bytes(svg_string=source,width=1416,height=960,skip_system_fonts=True,font_files=FONTS)
    image=Image.open(BytesIO(png)).convert("RGB")
    image.save(DEST / f"{name}.png", optimize=True)
    print(f"Created fictional UI preview: {DEST / (name+'.png')}")


save("context-menus", frame(MENUS))
