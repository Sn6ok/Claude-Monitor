import sys
import json
import ctypes
from pathlib import Path

# DPI-awareness must be the very first action, before grabbing the screen,
# so the screenshot is taken in physical pixels (matches SetCursorPos in click.py).
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)   # per-monitor DPI aware
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

# User paths may contain Cyrillic; keep captured output stable.
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

MAX_W, MAX_H = 1280, 800
BASE = Path(__file__).parent
SHOTS = BASE / "screenshots"


def die(msg):
    print("ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def main():
    try:
        from PIL import Image, ImageGrab
    except ImportError:
        die("Pillow is not installed, run: pip install -r requirements.txt")

    out = Path(sys.argv[1]) if len(sys.argv) > 1 else SHOTS / "screen.png"

    try:
        img = ImageGrab.grab()   # primary monitor only, physical pixels
    except Exception as e:
        die("cannot grab screen: " + str(e))

    screen_w, screen_h = img.size
    ratio = min(MAX_W / screen_w, MAX_H / screen_h, 1.0)   # never upscale
    image_w = round(screen_w * ratio)
    image_h = round(screen_h * ratio)
    if ratio < 1.0:
        img = img.resize((image_w, image_h), Image.Resampling.LANCZOS)

    scale_x = screen_w / image_w
    scale_y = screen_h / image_h

    # Create folders (for the PNG target and for last.json in the project).
    out.parent.mkdir(parents=True, exist_ok=True)
    SHOTS.mkdir(parents=True, exist_ok=True)
    try:
        img.save(out, "PNG")
    except Exception as e:
        die("cannot save PNG: " + str(e))

    data = {
        "image": str(out.resolve()).replace("\\", "/"),
        "image_w": image_w, "image_h": image_h,
        "screen_w": screen_w, "screen_h": screen_h,
        "scale_x": scale_x, "scale_y": scale_y,
    }
    (SHOTS / "last.json").write_text(json.dumps(data, indent=2), encoding="utf-8")

    scale = round(scale_x, 3)
    print(f"OK {out.resolve()} image={image_w}x{image_h} "
          f"screen={screen_w}x{screen_h} scale={scale}")


if __name__ == "__main__":
    main()
