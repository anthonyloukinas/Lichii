"""Screen-state classification from screenshots, against reference crops in refs/.

Each reference is a small grayscale thumbnail of a fixed crop of a 1920x1080 frame (refs/<name>.png)
plus the crop box (refs/refs.json). A screenshot matches a reference when the mean absolute
difference of the same crop's thumbnail is below that reference's threshold. In-game is detected
from the heart HUD (red pixels in the top-left box) so it does not depend on the scene.

Rebuild references from a run's screenshots with:
    python3 -I tools/oot/harness/states.py make-refs title=<png> load_list=<png> load_confirm=<png>
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageStat

REFS = Path(__file__).resolve().parent / "refs"
SIZE = (1920, 1080)
THUMB = (64, 32)
# crop boxes (x0, y0, x1, y1) in 1920x1080 coordinates
BOXES = {
    "title": (560, 220, 1360, 640),          # the ZELDA / OCARINA OF TIME logo
    "load_list": (0, 0, 1920, 60),           # "Load" banner (list and confirm both have it)
    "load_confirm": (760, 640, 1160, 800),   # Load / Cancel buttons of the confirm dialog
}
HEARTS = (0, 0, 260, 110)                   # heart HUD, top-left
DEFAULT_THRESH = {"title": 25.0, "load_list": 8.0, "load_confirm": 10.0}


def _norm(img):
    img = img.convert("RGB")
    return img if img.size == SIZE else img.resize(SIZE)


def thumb(img, box):
    return _norm(img).crop(box).convert("L").resize(THUMB, Image.BILINEAR)


def mad(a, b):
    pa, pb = a.tobytes(), b.tobytes()
    return sum(abs(x - y) for x, y in zip(pa, pb)) / len(pa)


def load_refs():
    meta = json.loads((REFS / "refs.json").read_text()) if (REFS / "refs.json").exists() else {}
    refs = {}
    for name, m in meta.items():
        p = REFS / f"{name}.png"
        if p.exists():
            refs[name] = (tuple(m["box"]), float(m["threshold"]), Image.open(p).convert("L"))
    return refs


def hearts_score(img):
    c = _norm(img).crop(HEARTS).resize((130, 55))
    px = c.tobytes()
    n = sum(1 for i in range(0, len(px), 3) if px[i] > 170 and px[i + 1] < 90 and px[i + 2] < 90)
    return n


def features(img, refs):
    img = _norm(img)
    f = {"brightness": round(ImageStat.Stat(img.convert("L").resize((96, 54))).mean[0], 1),
         "hearts": hearts_score(img)}
    for name, (box, _th, ref) in refs.items():
        f[name] = round(mad(thumb(img, box), ref), 1)
    return f


def classify(img, refs=None):
    """-> (state, features). state in: black, title, load_confirm, load_list, ingame, unknown."""
    refs = load_refs() if refs is None else refs
    f = features(img, refs)
    if f["brightness"] < 8:
        return "black", f
    for name in ("title", "load_confirm", "load_list"):
        if name in refs and f[name] < refs[name][1]:
            if name == "load_confirm" and "load_list" in refs and f["load_list"] >= refs["load_list"][1]:
                continue
            return name, f
    if f["hearts"] >= 40:
        return "ingame", f
    return "unknown", f


def make_refs(pairs):
    REFS.mkdir(exist_ok=True)
    meta = json.loads((REFS / "refs.json").read_text()) if (REFS / "refs.json").exists() else {}
    for name, png in pairs:
        box = BOXES[name]
        thumb(Image.open(png), box).save(REFS / f"{name}.png")
        meta[name] = {"box": list(box), "threshold": DEFAULT_THRESH[name], "source": Path(png).name}
    (REFS / "refs.json").write_text(json.dumps(meta, indent=2) + "\n")


if __name__ == "__main__":
    if sys.argv[1:2] == ["make-refs"]:
        make_refs([a.split("=", 1) for a in sys.argv[2:]])
    else:
        refs = load_refs()
        for p in sys.argv[1:]:
            print(Path(p).name, *classify(Image.open(p), refs))
