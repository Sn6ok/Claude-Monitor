import io
import os
import sys
import json
import ctypes
from ctypes import wintypes
from datetime import datetime
from pathlib import Path

# DPI-awareness must be the very first action so SetCursorPos uses physical
# pixels, the same coordinate system the screenshot was taken in.
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)   # per-monitor DPI aware
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

BASE = Path(__file__).parent
LAST = BASE / "screenshots" / "last.json"

# Папка вузла в Claude Monitor. Якщо її немає — мод не встановлено,
# і журнал кліків просто не ведеться.
NODE_DIR = Path(os.environ.get("USERPROFILE", "")) / ".claude-monitor" / "nodes" / "claude-click"
LOG = NODE_DIR / "clicks.log"
LOG_KEEP = 200

INPUT_MOUSE = 0
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
PUL = ctypes.POINTER(ctypes.c_ulong)


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", PUL)]


class _INPUTunion(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", _INPUTunion)]


def die(msg):
    print("ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def send_mouse(flags):
    extra = ctypes.c_ulong(0)
    mi = MOUSEINPUT(0, 0, 0, flags, 0, ctypes.cast(ctypes.pointer(extra), PUL))
    inp = INPUT(INPUT_MOUSE, _INPUTunion(mi=mi))
    ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def clean_label(raw):
    """Підпис для картки вузла: один рядок без керівних символів."""
    if not raw:
        return ""
    text = " ".join(str(raw).split())
    text = "".join(c for c in text if c.isprintable())
    return text[:80]


def log_click(label, x, y):
    """Лишає слід для картки вузла.

    Помилка журналу ніколи не заважає кліку: клік — головне, журнал — ні.
    """
    try:
        if not NODE_DIR.is_dir():
            return

        line = "{}\t{}\t{},{}\n".format(
            datetime.now().isoformat(timespec="seconds"), label, x, y)
        with io.open(LOG, "a", encoding="utf-8") as handle:
            handle.write(line)

        # Журнал не росте нескінченно: картці потрібні лише останні кліки.
        if LOG.stat().st_size > 64 * 1024:
            lines = io.open(LOG, encoding="utf-8").readlines()[-LOG_KEEP:]
            io.open(LOG, "w", encoding="utf-8").writelines(lines)
    except Exception:
        pass


def main():
    if len(sys.argv) not in (3, 4):
        die('usage: python click.py X Y ["підпис"]')
    try:
        x = int(sys.argv[1])
        y = int(sys.argv[2])
    except ValueError:
        die("X and Y must be integers")

    # Третій аргумент — на що саме тиснемо. Він нічого не змінює в кліку,
    # але саме він потрапляє в чат застосунку: «Натискаю: Файл».
    label = clean_label(sys.argv[3] if len(sys.argv) == 4 else "")

    cur_w = ctypes.windll.user32.GetSystemMetrics(0)
    cur_h = ctypes.windll.user32.GetSystemMetrics(1)

    if LAST.exists():
        try:
            d = json.loads(LAST.read_text(encoding="utf-8"))
            image_w, image_h = int(d["image_w"]), int(d["image_h"])
            screen_w, screen_h = int(d["screen_w"]), int(d["screen_h"])
            scale_x, scale_y = float(d["scale_x"]), float(d["scale_y"])
        except Exception:
            die("cannot read screenshots/last.json, run screenshot.py again")
        if (cur_w, cur_h) != (screen_w, screen_h):
            die("screen resolution changed, run screenshot.py again")
    else:
        image_w, image_h = cur_w, cur_h
        scale_x = scale_y = 1.0

    if not (0 <= x < image_w and 0 <= y < image_h):
        die(f"point {x},{y} is outside screenshot {image_w}x{image_h}")

    sx = round(x * scale_x)
    sy = round(y * scale_y)

    ctypes.windll.user32.SetCursorPos(sx, sy)
    send_mouse(MOUSEEVENTF_LEFTDOWN)
    send_mouse(MOUSEEVENTF_LEFTUP)

    log_click(label, x, y)

    suffix = f" label={label}" if label else ""
    print(f"OK click image={x},{y} screen={sx},{sy}{suffix}")


if __name__ == "__main__":
    main()
