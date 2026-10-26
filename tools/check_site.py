"""Check the buildless introduction page without external dependencies."""
from html.parser import HTMLParser
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}


class Check(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack = []
        self.ids = set()
        self.references = []
        self.scripts = []
        self.in_script = False

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            assert attrs["id"] not in self.ids, f"Duplicate id: {attrs['id']}"
            self.ids.add(attrs["id"])
        if tag == "img":
            assert "alt" in attrs, "Image missing alt"
        for name in ("href", "src"):
            if name in attrs:
                self.references.append(attrs[name])
        if tag not in VOID:
            self.stack.append(tag)
        self.in_script = tag == "script" or self.in_script

    def handle_endtag(self, tag):
        assert self.stack and self.stack[-1] == tag, f"Unbalanced tag: {tag}"
        self.stack.pop()
        if tag == "script":
            self.in_script = False

    def handle_data(self, data):
        if self.in_script:
            self.scripts.append(data)


page = Check()
page.feed((ROOT / "index.html").read_text(encoding="utf-8"))
assert not page.stack, f"Unclosed tags: {page.stack}"
for ref in page.references:
    if ref.startswith(("https://", "http://", "mailto:")):
        continue
    if ref.startswith("#"):
        assert ref == "#" or ref[1:] in page.ids, f"Missing anchor: {ref}"
    else:
        assert (ROOT / ref.split("#")[0]).is_file(), f"Missing local asset: {ref}"
subprocess.run(["node", "--check"], input="\n".join(page.scripts), text=True, check=True, cwd=ROOT)
print(f"PASS HTML structure, {len(page.references)} links/assets, image alternatives, JavaScript syntax")
