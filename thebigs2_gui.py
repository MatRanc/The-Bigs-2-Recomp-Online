#!/usr/bin/env python3
"""The Bigs 2 - launcher and settings editor.

Edits config/thebigs2.toml (the same file the game reads in portable mode) and
launches the game (the AppImage next to this script, or the dev build).

Run:  ./thebigs2_gui.py
Test: ./thebigs2_gui.py --check
"""

import json
import os
import random
import re
import socket
import threading
import urllib.request
import webbrowser
import subprocess
import sys
import tkinter as tk
import tkinter.font as tkfont
from tkinter import messagebox, ttk

# ROOT is the game folder (game, config/, saves/). The release launcher is a PyInstaller app that sits in that
# folder (on macOS the folder holding "The Bigs 2 Launcher.app"); its own files (theme, icons) are in RES.
FROZEN = getattr(sys, "frozen", False)
if FROZEN:
    _exe_dir = os.path.dirname(os.path.abspath(sys.executable))
    _app = _exe_dir.split(".app" + os.sep)[0] + ".app" if ".app" + os.sep in _exe_dir + os.sep else None
    ROOT = os.path.dirname(_app) if _app else _exe_dir
    RES = getattr(sys, "_MEIPASS", ROOT)
else:
    ROOT = RES = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(ROOT, "config", "thebigs2.toml")

# A vendored copy of the sv-ttk (Sun Valley) theme: the modern dark look used
# by the mlb16-online launcher, with no pip install needed. MIT licensed, see
# vendor/sv_ttk/LICENSE.
sys.path.insert(0, os.path.join(RES, "vendor"))
try:
    import sv_ttk
except Exception:  # pragma: no cover - only if the vendor folder is missing
    sv_ttk = None

# GUI-only preferences (theme choice) and an error log live beside the game's
# settings, so they stay in the portable folder without touching thebigs2.toml.
GUI_PREFS = os.path.join(ROOT, "config", "gui_prefs.json")
GUI_LOG = os.path.join(ROOT, "config", "gui.log")

THEME_ORDER = ("system", "light", "dark")
# Colour emoji icons (rendered from the system's Noto Color Emoji) for the
# header toggle; the text glyphs are only a fallback if the PNGs are missing.
THEME_ICON_DIR = os.path.join(RES, "assets", "theme-icons")
THEME_ICONS = {"system": "🌗", "light": "☀️", "dark": "🌙"}


def _try_run(cmd):
    """Run a short probe, returning stdout or "" on any failure."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=2)
        return (result.stdout or "").strip()
    except Exception:
        return ""


def _windows_theme():
    try:
        import winreg
        with winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as key:
            value, _ = winreg.QueryValueEx(key, "AppsUseLightTheme")
        return "light" if value else "dark"
    except Exception:
        return None


def _linux_theme():
    # Respect an explicit override first.
    env = os.environ.get("GTK_THEME", "").lower()
    if "dark" in env:
        return "dark"
    if "light" in env:
        return "light"
    # GNOME / GTK (color-scheme is the modern source, gtk-theme the old one).
    for key in ("color-scheme", "gtk-theme"):
        out = _try_run(["gsettings", "get", "org.gnome.desktop.interface", key]).lower()
        if "dark" in out:
            return "dark"
        if "light" in out:
            return "light"
    # KDE.
    for tool in ("kreadconfig6", "kreadconfig5"):
        out = _try_run([tool, "--group", "General", "--key", "ColorScheme"]).lower()
        if "dark" in out:
            return "dark"
        if "light" in out:
            return "light"
    return "dark"


_SYSTEM_THEME_CACHE = None


def _detect_system_theme_uncached():
    try:
        if sys.platform == "win32":
            return _windows_theme() or "dark"
        if sys.platform == "darwin":
            # `defaults` prints nothing (and errors) when the light theme is set.
            return "dark" if _try_run(
                ["defaults", "read", "-g", "AppleInterfaceStyle"]).lower() == "dark" else "light"
        return _linux_theme()
    except Exception:
        return "dark"


def detect_system_theme(refresh=False):
    """The desktop's light/dark preference; 'dark' if it can't be determined."""
    global _SYSTEM_THEME_CACHE
    if _SYSTEM_THEME_CACHE is None or refresh:
        _SYSTEM_THEME_CACHE = _detect_system_theme_uncached()
    return _SYSTEM_THEME_CACHE


def load_prefs():
    try:
        with open(GUI_PREFS, "r", encoding="utf-8") as fh:
            prefs = json.load(fh)
        return prefs if isinstance(prefs, dict) else {}
    except (OSError, ValueError):
        return {}


def save_prefs(**changes):
    prefs = load_prefs()
    prefs.update(changes)
    try:
        os.makedirs(os.path.dirname(GUI_PREFS), exist_ok=True)
        with open(GUI_PREFS, "w", encoding="utf-8") as fh:
            json.dump(prefs, fh, indent=1)
        return True
    except OSError:
        return False


def load_theme_pref():
    value = load_prefs().get("theme", "system")
    return value if value in THEME_ORDER else "system"


def save_theme_pref(theme):
    return save_prefs(theme=theme)


# -- online play --------------------------------------------------------------
# The game's own online mode, with a stand-in for the dead Xbox Live service.
# The host's game room listens on TCP 51000; the match itself (the game's input
# lockstep) uses UDP 1795. tools/run_pair.sh gets its env from online_env() too.
ONLINE_PORT = 51000
NAME_MAX = 8  # the game shows the player's id bytes as the name: 8 characters


def online_env(mode, name, peer_ip=""):
    """Environment variables the game needs for an online session."""
    if mode not in ("Host", "Join"):
        return {}
    env = {"TB2_FAKE_LIVE": "1", "TB2_FAKE_ACCOUNT": "1", "TB2_SKIP_ROSTER": "1",
           "TB2_SESSION_TICK": "1", "TB2_ACCOUNT_NAME": name}
    if mode == "Host":
        env.update(TB2_FAKE_HOST="1", TB2_HOST_PORT=str(ONLINE_PORT))
    else:
        env.update(TB2_FAKE_SEARCH="1", TB2_PEER_IP=peer_ip, TB2_PEER_PORT=str(ONLINE_PORT))
    return env


def check_name(name):
    """Error message for an unusable player name, or None."""
    if not name:
        return "Enter a player name."
    if len(name) > NAME_MAX:
        return f"Player names can be at most {NAME_MAX} characters."
    if not re.fullmatch(r"[A-Za-z0-9 _-]+", name):
        return "Use only letters, digits, space, _ and - in the player name."
    return None


def resolve_peer(addr):
    """The friend's address as an IPv4 string (names like a Tailscale machine name are looked up)."""
    addr = addr.strip()
    if not addr:
        raise ValueError("Enter the host's IP address.")
    try:
        return socket.gethostbyname(addr)
    except OSError:
        raise ValueError(f"Cannot find '{addr}'. Check the address (for Tailscale, the 100.x.y.z IP).")


def local_ips():
    """This machine's IPv4 addresses, Tailscale (100.x) first, without loopback."""
    ips = []
    try:  # the address used for outgoing traffic (UDP connect sends nothing)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            ips.append(s.getsockname()[0])
    except OSError:
        pass
    try:
        ips += [i[4][0] for i in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)]
    except OSError:
        pass
    ips += _try_run(["tailscale", "ip", "-4"]).split()
    if sys.platform == "darwin":  # the Mac app's CLI is not on PATH by default
        ips += _try_run(["/Applications/Tailscale.app/Contents/MacOS/Tailscale", "ip", "-4"]).split()
    elif os.name == "nt":
        exe = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Tailscale", "tailscale.exe")
        if os.path.isfile(exe):
            ips += _try_run([exe, "ip", "-4"]).split()
    seen = []
    for ip in ips:
        if ip and not ip.startswith("127.") and ip not in seen:
            seen.append(ip)
    return sorted(seen, key=lambda ip: not ip.startswith("100."))

GUIDE_URL = "https://github.com/MatRanc/The-Bigs-2-Recomp-Online/blob/HEAD/docs/PLAY_ONLINE.md"
TAILSCALE_GUIDE_URL = GUIDE_URL + "#set-up-tailscale"
HELP_URL = GUIDE_URL + "#cant-connect"
PUBLIC_IP_URL = "https://api.ipify.org"


def is_tailscale_ip(ip):
    """Tailscale hands out addresses from 100.64.0.0/10."""
    try:
        a, b = (int(x) for x in ip.split(".")[:2])
        return a == 100 and 64 <= b <= 127
    except ValueError:
        return False


def tailscale_ip():
    """This machine's Tailscale address, or None when Tailscale isn't running here."""
    return next((ip for ip in local_ips() if is_tailscale_ip(ip)), None)


def lan_ip():
    """This machine's address on the home network (192.168.x.y and the like), or None."""
    def private(ip):
        a, b = (int(x) for x in ip.split(".")[:2])
        return a == 10 or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168)
    return next((ip for ip in local_ips() if private(ip)), None)


def public_ip():
    """This network's public address, from an IP-echo service (only called when the user asks)."""
    with urllib.request.urlopen(PUBLIC_IP_URL, timeout=6) as r:
        ip = r.read(64).decode("ascii", "replace").strip()
    socket.inet_aton(ip)  # raises on anything that isn't an IPv4 address
    return ip


def default_player_name():
    return f"Player{random.randint(10, 99)}"


class AlreadyRunning(Exception):
    pass


def acquire_single_instance():
    """Hold an OS file lock for this process's lifetime. Raises AlreadyRunning
    when another launcher owns it. Returns the fd, or None if no lock file
    could be created (then we run without one)."""
    path = os.path.join(ROOT, "config", ".launcher.lock")
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    except OSError:
        return None
    try:
        if os.name == "nt":
            import msvcrt
            if os.fstat(fd).st_size == 0:
                os.write(fd, b"0")
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        raise AlreadyRunning
    return fd


def _show_already_running():
    try:
        root = tk.Tk()
        root.withdraw()
        messagebox.showinfo("The Bigs 2 Launcher",
                            "The launcher is already running.")
        root.destroy()
    except Exception:
        sys.stderr.write("thebigs2 launcher: already running\n")


# ---------------------------------------------------------------------------
# Settings description (mirrors src/settings.cpp's template).
# type: bool | int | str | tristate (scene_msaa only)
# options: for combo boxes (strings; for int combos they are parsed as int)
# ---------------------------------------------------------------------------

KEYBIND_LABELS = [
    ("a", "A"), ("b", "B"), ("x", "X"), ("y", "Y"),
    ("left_shoulder", "Left shoulder (LB)"), ("right_shoulder", "Right shoulder (RB)"),
    ("left_trigger", "Left trigger (LT)"), ("right_trigger", "Right trigger (RT)"),
    ("back", "Back / Select"), ("start", "Start"),
    ("lstick_up", "Left stick up"), ("lstick_down", "Left stick down"),
    ("lstick_left", "Left stick left"), ("lstick_right", "Left stick right"),
    ("lstick_press", "Left stick click"),
    ("rstick_up", "Right stick up"), ("rstick_down", "Right stick down"),
    ("rstick_left", "Right stick left"), ("rstick_right", "Right stick right"),
    ("rstick_press", "Right stick click"),
    ("dpad_up", "D-pad up"), ("dpad_down", "D-pad down"),
    ("dpad_left", "D-pad left"), ("dpad_right", "D-pad right"),
]

# The game's default keyboard bindings (src/settings.cpp's template), so
# Reset and a freshly written config match what the game ships with.
KEYBIND_DEFAULTS = {
    "a": "Semicolon,Space", "b": "Quote,Backspace", "x": "L", "y": "P",
    "left_shoulder": "1", "right_shoulder": "3",
    "left_trigger": "Q,I", "right_trigger": "E,O",
    "back": "Z,Tab", "start": "X,Return",
    "lstick_up": "W", "lstick_down": "S", "lstick_left": "A", "lstick_right": "D",
    "lstick_press": "F",
    "rstick_up": "Up", "rstick_down": "Down", "rstick_left": "Left",
    "rstick_right": "Right", "rstick_press": "K",
    "dpad_up": "Shift+Up", "dpad_down": "Shift+Down",
    "dpad_left": "Shift+Left", "dpad_right": "Shift+Right",
}

PAD_OPTIONS = ["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start",
               "LS", "RS", "Up", "Down", "Left", "Right", "None"]

RESOLUTIONS = ["desktop", "1280x720", "1600x900", "1920x1080", "2560x1440",
               "3840x2160", "1280x800"]

# A field's `requires` predicate gets the current widget values (as strings)
# and returns whether the setting is relevant. Irrelevant settings are faded
# out and disabled, like refresh rate without exclusive fullscreen.


def _is_fullscreen(v):
    return v.get("fullscreen") == "true"


def _is_exclusive(v):
    return v.get("fullscreen") == "true" and v.get("fullscreen_exclusive") == "true"


def _not_borderless(v):
    # Borderless fullscreen uses the desktop mode, so resolution is ignored.
    return not (v.get("fullscreen") == "true" and v.get("fullscreen_exclusive") != "true")


def _keyboard_on(v):
    return v.get("mnk_mode") == "true"


GROUPS = [
    ("Display", [
        {"key": "fullscreen", "label": "Fullscreen", "type": "bool", "default": True},
        {"key": "fullscreen_exclusive", "label": "Exclusive fullscreen", "type": "bool",
         "default": False, "requires": _is_fullscreen,
         "help": "Only when fullscreen is on. Off = borderless desktop mode."},
        {"key": "display_resolution", "label": "Resolution", "type": "str",
         "default": "desktop", "options": RESOLUTIONS, "requires": _not_borderless,
         "help": "'desktop' or WIDTHxHEIGHT. Window size, or the monitor mode in "
                 "exclusive fullscreen."},
        {"key": "display_refresh_rate", "label": "Refresh rate (Hz)", "type": "int",
         "default": 0, "min": 0, "max": 240, "requires": _is_exclusive,
         "help": "0 = desktop rate. Exclusive fullscreen only."},
        {"key": "monitor", "label": "Monitor", "type": "int", "default": 0,
         "min": 0, "max": 8, "requires": _is_fullscreen,
         "help": "0 = system default, 1 = first, 2 = second, ..."},
        {"key": "present_letterbox", "label": "Keep 16:9 (letterbox)", "type": "bool",
         "default": True},
    ]),
    ("Scaling", [
        {"key": "resolution_scale", "label": "Render scale", "type": "int",
         "default": 2, "min": 1, "max": 3,
         "help": "1 = 720p, 2 = 1440p (default), 3 = 4K. Higher is sharper and "
                 "slower. Use 1 with frame_rate = 60."},
        {"key": "present_effect", "label": "Upscale effect", "type": "str",
         "default": "bilinear", "options": ["bilinear", "cas", "fsr"]},
        {"key": "render_target_path_vulkan", "label": "EDRAM path", "type": "str",
         "default": "fsi", "options": ["fsi", "host"],
         "help": "fsi = accurate (needed for scale > 1). host = faster, but the "
                 "bat/cap/shoes get green stripes above 1x."},
        {"key": "anisotropic_override", "label": "Texture filtering", "type": "int",
         "default": 3, "min": -1, "max": 5,
         "help": "-1 = the game's own, 0 = off, 1-5 = forced anisotropic."},
        {"key": "scene_msaa", "label": "Scene MSAA (4x)", "type": "tristate",
         "default": None,
         "help": "Auto = on at 1x, off at 2x and up. On/Off forces it."},
    ]),
    ("Frame rate", [
        {"key": "frame_rate", "label": "Match frame rate", "type": "int",
         "default": 30, "options": ["30", "60"], "requires": lambda v: v.get("_online_mode") == "Offline",
         "help": "30 = the game's own cap. 60 is experimental; use with render "
                 "scale 1 and a fast machine."},
    ]),
    ("Controls", [
        {"key": "bind_quit", "label": "Quit hotkey", "type": "str", "default": "F10"},
        {"key": "bind_settings_menu", "label": "Settings menu hotkey", "type": "str",
         "default": "F4"},
    ]),
    ("Keyboard", [
        {"key": "mnk_mode", "label": "Keyboard plays as player 1", "type": "bool",
         "default": True},
    ] + [
        {"key": "keybind_" + k, "label": label, "type": "str",
         "default": KEYBIND_DEFAULTS.get(k, ""), "requires": _keyboard_on}
        for k, label in KEYBIND_LABELS
    ]),
    ("Controller", [
        {"key": "padbind_" + k, "label": label, "type": "str",
         "default": default, "options": PAD_OPTIONS}
        for k, label, default in [
            ("a", "A", "A"), ("b", "B", "B"), ("x", "X", "X"), ("y", "Y", "Y"),
            ("lb", "Left shoulder (LB)", "LB"), ("rb", "Right shoulder (RB)", "RB"),
            ("lt", "Left trigger (LT)", "LT"), ("rt", "Right trigger (RT)", "RT"),
            ("back", "Back / Select", "Back"), ("start", "Start", "Start"),
            ("ls", "Left stick click", "LS"), ("rs", "Right stick click", "RS"),
            ("up", "D-pad up", "Up"), ("down", "D-pad down", "Down"),
            ("left", "D-pad left", "Left"), ("right", "D-pad right", "Right"),
        ]
    ]),
]

ALL_FIELDS = [f for _, fs in GROUPS for f in fs]
FIELDS_BY_KEY = {f["key"]: f for f in ALL_FIELDS}

# Info text for each setting, shown by the launcher's info button on hover (based on
# the in-game settings menu's text in src/settings_menu.cpp, some more detailed here).
_PADBIND_INFO = ("The game button this controller button presses (for every controller; "
                 "the keyboard has its own keys). Applies after a restart.")
_KEYBIND_INFO = ("The keyboard key(s) for this controller button: one key or several "
                 "separated by commas (\"Q,I\"), with modifiers if needed (\"Shift+Up\"). "
                 "Key names: see thebigs2.toml.")
INFO = {
    "fullscreen": "On: the game fills the screen. Off: it runs in a window, sized by "
                  "Resolution below.",
    "fullscreen_exclusive": "Borderless uses the desktop's resolution and refresh rate; "
                            "exclusive switches the monitor to the mode below.",
    "display_resolution": "In a window: the window's size. In exclusive fullscreen: the mode the "
                          "monitor switches to (the closest it supports). 'desktop' uses your "
                          "desktop's resolution. This is the screen size, not how sharp the game "
                          "renders: that's Render scale.",
    "display_refresh_rate": "Exclusive fullscreen only; 0 keeps the desktop's rate. Menus run at "
                            "60 fps and matches at 30 (or 60), so 60 or 120 Hz move the smoothest. "
                            "144 or 165 Hz judder slightly unless you have VRR (G-Sync/FreeSync).",
    "monitor": "Which monitor the game opens on: 0 = the system's main one, 1 = the first, "
               "2 = the second, and so on.",
    "present_letterbox": "The game is 16:9. On other screens (ultrawide, 16:10 like the Steam "
                         "Deck) this adds black bars; off stretches the picture to fill.",
    "resolution_scale": "How sharp the game renders: 1x = the original 720p, 2x = 1440p, "
                        "3x = 4K. Higher is sharper and needs a faster GPU. Use 1x with 60 fps, "
                        "and on GPUs without fragment shader interlock (AMD on Linux, Steam Deck).",
    "render_target_path_vulkan": "FSI: accurate, needed at 2x and up with the game's MSAA. "
                                 "Host: faster. GPUs without fragment shader interlock "
                                 "always use host.",
    "anisotropic_override": "Sharpens the field and the stands at a distance. -1 = the game's "
                            "own, 0 = off, 1-5 = stronger.",
    "scene_msaa": "The game's anti-aliasing on the 3D scene. Automatic: on at 1x, off at "
                  "2x and up and on the Steam Deck. Off saves about half the GPU "
                  "emulation's CPU work.",
    "present_effect": "How the rendered picture is fitted to your screen. bilinear: smooth "
                      "(default). cas: a little sharper. fsr: AMD FSR 1 upscaling, sharper "
                      "edges when the render is smaller than the screen.",
    "frame_rate": "30 is the game's own. 60 is experimental: it needs a fast CPU and GPU "
                  "(1x on the desktop); go back to 30 if it stutters. Offline only: online "
                  "matches always run at 30, so both players stay in step.",
    "mnk_mode": "The keyboard plays as player 1's controller, together with the first pad.",
    "bind_quit": "Key that quits the game (controller: hold Back + Start for 2 seconds).",
    "bind_settings_menu": "Key that opens this menu (controller: hold Back + Y for a second).",
}
for _k in [f["key"] for f in ALL_FIELDS if f["key"].startswith("padbind_")]:
    INFO[_k] = _PADBIND_INFO
for _k in [f["key"] for f in ALL_FIELDS if f["key"].startswith("keybind_")]:
    INFO[_k] = _KEYBIND_INFO

# ---------------------------------------------------------------------------
# Minimal TOML-ish reading/writing that preserves the file's comments.
# ---------------------------------------------------------------------------

_ACTIVE = re.compile(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$")
_COMMENTED = re.compile(r"^(\s*)#\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$")


def _strip_inline_comment(raw):
    out, quote = [], None
    for i, ch in enumerate(raw):
        if quote:
            out.append(ch)
            if ch == quote and (i == 0 or raw[i - 1] != "\\"):
                quote = None
        else:
            if ch in "\"'":
                quote = ch
                out.append(ch)
            elif ch == "#":
                break
            else:
                out.append(ch)
    return "".join(out).strip()


def _parse_value(raw):
    raw = _strip_inline_comment(raw)
    if raw in ("true", "false"):
        return raw == "true"
    if len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in "\"'":
        body = raw[1:-1]
        return body.replace('\\"', '"').replace("\\\\", "\\")
    try:
        return int(raw)
    except ValueError:
        pass
    try:
        return float(raw)
    except ValueError:
        return raw


def read_config(text):
    values = {}
    for line in text.splitlines():
        m = _ACTIVE.match(line)
        if m:
            values.setdefault(m.group(2), _parse_value(m.group(3)))
    return values


def _quote(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _literal(value, ftype):
    if ftype == "bool":
        return "true" if value else "false"
    if ftype == "int":
        return str(int(value))
    if ftype == "tristate":
        return "true" if value else "false"
    return _quote(value)


def _replace_key(text, key, literal):
    """Replace the first active `key = ...` line, else uncomment a matching
    commented line, else append the key."""
    lines = text.splitlines(keepends=True)
    for i, line in enumerate(lines):
        m = _ACTIVE.match(line)
        if m and m.group(2) == key:
            lines[i] = f"{m.group(1)}{key} = {literal}\n"
            return "".join(lines)
    for i, line in enumerate(lines):
        m = _COMMENTED.match(line)
        if m and m.group(2) == key:
            lines[i] = f"{key} = {literal}\n"
            return "".join(lines)
    if text and not text.endswith("\n"):
        text += "\n"
    return text + f"{key} = {literal}\n"


def _comment_key(text, key, default_literal="true"):
    """Comment the key out (used by scene_msaa = Auto)."""
    lines = text.splitlines(keepends=True)
    for i, line in enumerate(lines):
        m = _ACTIVE.match(line)
        if m and m.group(2) == key:
            lines[i] = f"# {key} = {m.group(3).strip()}\n"
            return "".join(lines)
    for i, line in enumerate(lines):
        m = _COMMENTED.match(line)
        if m and m.group(2) == key:
            return text  # already commented
    if text and not text.endswith("\n"):
        text += "\n"
    return text + f"# {key} = {default_literal}\n"


def default_config_text():
    parts = ["# The Bigs 2 - settings (written by the launcher)\n"]
    for _name, fields in GROUPS:
        for f in fields:
            if f["type"] == "tristate":
                continue
            parts.append(f"{f['key']} = {_literal(f['default'], f['type'])}\n")
    return "".join(parts)


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class ScrollFrame(ttk.Frame):
    """A vertically scrollable frame."""

    def __init__(self, parent):
        super().__init__(parent)
        # Match the themed background instead of Tk's default white canvas.
        bg = ttk.Style().lookup("TFrame", "background") or "#1c1c1c"
        self.canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0, bg=bg)
        self.vsb = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.inner = ttk.Frame(self.canvas)
        self.inner_id = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")
        self.canvas.configure(yscrollcommand=self.vsb.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        self.vsb.pack(side="right", fill="y")
        self._can_scroll = False
        self._syncing = False
        self._vsb_visible = True
        self.inner.bind("<Configure>", self._on_configure)
        self.canvas.bind("<Configure>", self._on_canvas)
        # add="+" so every tab's handler stays bound; each one only scrolls when
        # the pointer is actually over its canvas.
        self.canvas.bind_all("<MouseWheel>", self._on_wheel, add="+")
        self.canvas.bind_all("<Button-4>", self._on_wheel_btn, add="+")
        self.canvas.bind_all("<Button-5>", self._on_wheel_btn, add="+")

    def _on_configure(self, _e):
        self._sync_scroll()

    def _on_canvas(self, e):
        try:
            self.canvas.itemconfigure(self.inner_id, width=e.width)
        except tk.TclError:
            pass
        self._sync_scroll()

    def _sync_scroll(self):
        """Only allow scrolling when the content is taller than the tab."""
        if self._syncing:
            return
        try:
            bbox = self.canvas.bbox("all")
            if bbox is None:
                return
            self._syncing = True
            try:
                content_h = bbox[3] - bbox[1]
                view_h = self.canvas.winfo_height()
                view_w = self.canvas.winfo_width()
                need = content_h > view_h + 1
                self._can_scroll = need
                if need:
                    self.canvas.configure(
                        scrollregion=(0, 0, view_w, max(content_h, view_h)))
                    if not self._vsb_visible:
                        self.vsb.pack(side="right", fill="y")
                        self._vsb_visible = True
                else:
                    self.canvas.configure(scrollregion=(0, 0, view_w, view_h))
                    self.canvas.yview_moveto(0)
                    if self._vsb_visible:
                        self.vsb.pack_forget()
                        self._vsb_visible = False
            finally:
                self._syncing = False
        except tk.TclError:
            pass

    def set_background(self, bg):
        try:
            self.canvas.configure(bg=bg)
        except tk.TclError:
            pass

    def _pointer_inside(self, e):
        try:
            # Only the visible tab may scroll; the others keep their old
            # geometry while hidden and would otherwise all match.
            if not self.winfo_ismapped() or not self.canvas.winfo_ismapped():
                return False
            x, y = self.canvas.winfo_rootx(), self.canvas.winfo_rooty()
            return (x <= e.x_root < x + self.canvas.winfo_width()
                    and y <= e.y_root < y + self.canvas.winfo_height())
        except tk.TclError:
            return False

    def _on_wheel(self, e):
        try:
            if not self._can_scroll or not self._pointer_inside(e):
                return
            step = -int(e.delta / 120) if e.delta else 0
            if step:
                self.canvas.yview_scroll(step, "units")
        except tk.TclError:
            pass

    def _on_wheel_btn(self, e):
        try:
            if not self._can_scroll or not self._pointer_inside(e):
                return
            self.canvas.yview_scroll(-1 if e.num == 4 else 1, "units")
        except tk.TclError:
            pass


class Launcher(tk.Tk):
    def __init__(self):
        # Give the window a real app name so the DE doesn't call it "Tk";
        # StartupWMClass in the .desktop entry matches this.
        super().__init__(className="thebigs2-launcher")
        self.title("The Bigs 2 Launcher")
        self.geometry("860x740")
        self.minsize(700, 540)
        self.vars = {}
        self.field_widgets = {}
        self._scrolls = []
        self.theme_pref = load_theme_pref()
        self.proc = None
        self._theme_images = self._load_theme_images()
        self._config_sig = None
        self._loaded_snapshot = None
        self._pending_sig = None
        self._external_change = False
        self._set_window_icon()

        self._apply_theme()
        self._build_header()
        self._build_tabs()
        self._build_footer()
        self.load()
        # Pick up changes the game (or a text editor) makes to the file.
        self.after(1000, self._poll_config)

    # -- theme -------------------------------------------------------------
    # The sv-ttk "Sun Valley" theme (as in the mlb16-online launcher), in the
    # desktop's light or dark mode by default, overridable with the sun/moon
    # button in the header. A clam fallback keeps the app usable if sv-ttk is
    # missing or fails to load.
    def _resolve_theme(self):
        pref = getattr(self, "theme_pref", "system")
        return detect_system_theme() if pref == "system" else pref

    def _load_theme_images(self):
        images = {}
        if not os.path.isdir(THEME_ICON_DIR):
            return images
        for name in THEME_ORDER:
            path = os.path.join(THEME_ICON_DIR, f"{name}.png")
            try:
                images[name] = tk.PhotoImage(file=path)
            except (OSError, tk.TclError) as e:
                self._log_error(f"Could not load theme icon {path}: {e}")
        return images

    def _set_window_icon(self):
        for name in ("bigs2-logo-128.png", "bigs2-logo.png",
                     "thebigs2-window.png", "thebigs2.png"):
            path = os.path.join(RES, "assets", name)
            try:
                if os.path.isfile(path):
                    self._window_icon = tk.PhotoImage(file=path)
                    self.iconphoto(True, self._window_icon)
                    return
            except (OSError, tk.TclError) as e:
                self._log_error(f"Could not set the window icon {path}: {e}")

    def _theme_icon_image(self):
        images = getattr(self, "_theme_images", {})
        return images.get(getattr(self, "theme_pref", "system"))

    def _theme_icon(self):
        return THEME_ICONS.get(getattr(self, "theme_pref", "system"), "◐")

    def _theme_tooltip(self):
        pref = getattr(self, "theme_pref", "system")
        if pref == "system":
            return f"Theme: System ({self._resolve_theme()}) - click to switch"
        return f"Theme: {pref.capitalize()} - click to switch"

    def cycle_theme(self):
        try:
            pref = getattr(self, "theme_pref", "system")
            self.theme_pref = THEME_ORDER[(THEME_ORDER.index(pref) + 1) % len(THEME_ORDER)]
            if self.theme_pref == "system":
                detect_system_theme(refresh=True)  # re-sync when going back to auto
            if not save_theme_pref(self.theme_pref):
                self._log_error("Theme changed, but the preference could not be saved.")
            self._apply_theme()
        except Exception as e:
            self._report_error("Could not change the theme", e)

    def _apply_theme(self):
        theme = self._resolve_theme()
        dark = theme == "dark"
        style = ttk.Style(self)
        try:
            if sv_ttk is not None:
                sv_ttk.set_theme(theme, self)
            else:
                self._apply_clam_theme(style, dark)
        except Exception as e:
            # A broken theme must never take the launcher down.
            self._log_error(f"sv-ttk theme '{theme}' failed: {e}")
            try:
                self._apply_clam_theme(style, dark)
            except Exception:
                pass

        # sv-ttk resets the styles, so (re)apply our own label palette.
        if dark:
            palette = ("#9a9a9a", "#4ea1ff", "#3fb950", "#5f5f5f", "#555555")
        else:
            palette = ("#666666", "#0a66d6", "#1a7f37", "#a6a6a6", "#b0b0b0")
        for name, color in zip(
                ("Hint.TLabel", "Link.TLabel", "Running.TLabel",
                 "Disabled.TLabel", "DisabledHint.TLabel"), palette):
            style.configure(name, foreground=color)
            # sv-ttk colours labels through a state map, which beats a plain configure.
            style.map(name, foreground=[("!disabled", color)])
        self._palette = palette

        # Drop the dotted focus ring drawn around a clicked tab.
        try:
            style.layout("TNotebook.Tab", [
                ("Notebook.tab", {"sticky": "nswe", "children": [
                    ("Notebook.padding", {"side": "top", "sticky": "nswe", "children": [
                        ("Notebook.label", {"side": "top", "sticky": ""}),
                    ]}),
                ]}),
            ])
        except tk.TclError:
            pass

        base = tkfont.Font(font=style.lookup("TLabel", "font") or "TkDefaultFont")
        family = base.cget("family")
        size = abs(int(base.cget("size"))) or 10
        self.font_title = tkfont.Font(family=family, size=size + 9, weight="bold")
        self.font_h1 = tkfont.Font(family=family, size=size + 3, weight="bold")

        bg = style.lookup("TFrame", "background") or ("#1c1c1c" if dark else "#f4f4f4")
        try:
            self.configure(bg=bg)
        except tk.TclError:
            pass
        for scroll in getattr(self, "_scrolls", []):
            scroll.set_background(bg)
        if hasattr(self, "theme_btn"):
            img = self._theme_icon_image()
            if img is not None:
                self.theme_btn.configure(image=img, text="")
            else:
                self.theme_btn.configure(text=self._theme_icon())
        # Re-apply the faded colours of gated fields for the new palette.
        # sv-ttk's <<ThemeChanged>> handler can repaint the palette afterwards,
        # so do it again once idle.
        self._update_gates()
        try:
            self.after_idle(self._update_gates)
        except tk.TclError:
            pass

    @staticmethod
    def _apply_clam_theme(style, dark):
        if dark:
            bg, card, fg = "#1c1c1c", "#242424", "#e8e8e8"
            border, active, accent, accent_hi, accent_dis = (
                "#3a3a3a", "#2e2e2e", "#3b82f6", "#2f6fd6", "#2a4a7a")
        else:
            bg, card, fg = "#f4f4f4", "#ffffff", "#1a1a1a"
            border, active, accent, accent_hi, accent_dis = (
                "#d0d0d0", "#e9e9e9", "#2563eb", "#1d4ed8", "#9db8e8")
        style.theme_use("clam")
        style.configure(".", background=bg, foreground=fg, fieldbackground=card,
                        bordercolor=border, lightcolor=card, darkcolor=card,
                        troughcolor=card, selectbackground=accent,
                        selectforeground="#ffffff", focuscolor=accent)
        style.configure("TFrame", background=bg)
        style.configure("TLabel", background=bg, foreground=fg)
        style.configure("TCheckbutton", background=bg, foreground=fg)
        style.configure("TNotebook", background=bg, borderwidth=0)
        style.configure("TNotebook.Tab", background=card, foreground=fg, padding=(14, 6))
        style.map("TNotebook.Tab", background=[("selected", bg)])
        style.configure("TEntry", fieldbackground=card, foreground=fg, insertcolor=fg)
        style.configure("TCombobox", fieldbackground=card, foreground=fg)
        style.configure("TSpinbox", fieldbackground=card, foreground=fg)
        style.configure("TButton", background=card, foreground=fg, padding=(12, 5))
        style.map("TButton", background=[("active", active)])
        style.configure("Accent.TButton", background=accent, foreground="#ffffff",
                        padding=(14, 6))
        style.map("Accent.TButton",
                  background=[("active", accent_hi), ("disabled", accent_dis)])

    # -- errors ------------------------------------------------------------
    def _log_error(self, text):
        import time
        try:
            os.makedirs(os.path.dirname(GUI_LOG), exist_ok=True)
            with open(GUI_LOG, "a", encoding="utf-8") as fh:
                fh.write(f"\n[{time.strftime('%Y-%m-%d %H:%M:%S')}] {text}\n")
        except Exception:
            pass
        try:
            sys.stderr.write(text + "\n")
        except Exception:
            pass

    def _report_error(self, title, exc):
        import traceback
        detail = traceback.format_exc() if isinstance(exc, BaseException) else str(exc)
        self._log_error(f"{title}: {detail}")
        try:
            messagebox.showerror(
                title, f"{exc}\n\nLogged to:\n{GUI_LOG}", parent=self)
        except tk.TclError:
            pass

    def report_callback_exception(self, exc, value, tb):
        """Tk would otherwise print callback errors to stderr and continue."""
        import traceback
        detail = "".join(traceback.format_exception(exc, value, tb))
        self._log_error("Unhandled UI error:\n" + detail)
        try:
            messagebox.showerror(
                "Unexpected error",
                f"Something went wrong:\n\n{value}\n\nLogged to:\n{GUI_LOG}",
                parent=self)
        except tk.TclError:
            pass

    def _attach_tooltip(self, widget, text_fn, wraplength=380):
        state = {"win": None}

        def show(_event=None):
            if state["win"] is not None:
                return
            try:
                bg = ttk.Style().lookup("TFrame", "background") or "#1c1c1c"
                tip = tk.Toplevel(widget)
                tip.wm_overrideredirect(True)
                tip.configure(bg=bg)
                try:
                    tip.attributes("-topmost", True)
                except tk.TclError:
                    pass
                frame = ttk.Frame(tip, padding=1)
                frame.pack()
                ttk.Label(frame, text=text_fn(), style="Hint.TLabel",
                          wraplength=wraplength, justify="left",
                          padding=(9, 6)).pack()
                tip.update_idletasks()
                tw, th = tip.winfo_reqwidth(), tip.winfo_reqheight()
                sw, sh = tip.winfo_screenwidth(), tip.winfo_screenheight()
                x = widget.winfo_rootx()
                y = widget.winfo_rooty() + widget.winfo_height() + 6
                if x + tw > sw - 8:
                    x = max(8, sw - tw - 8)
                if y + th > sh - 8:
                    y = max(8, widget.winfo_rooty() - th - 6)
                tip.geometry(f"+{x}+{y}")
                state["win"] = tip
            except tk.TclError:
                state["win"] = None

        def hide(_event=None):
            win = state["win"]
            state["win"] = None
            if win is not None:
                try:
                    win.destroy()
                except tk.TclError:
                    pass

        widget.bind("<Enter>", show, add="+")
        widget.bind("<Leave>", hide, add="+")
        widget.bind("<ButtonPress>", hide, add="+")

    # -- layout ------------------------------------------------------------
    def _build_header(self):
        bar = ttk.Frame(self, padding=(28, 20, 28, 0))
        bar.pack(fill="x")
        left = ttk.Frame(bar)
        left.pack(side="left")
        ttk.Label(left, text="The Bigs 2 Launcher", font=self.font_title).pack(anchor="w")
        ttk.Label(left, text="Settings & launch", style="Hint.TLabel").pack(
            anchor="w", pady=(2, 0))
        icon = self._theme_icon_image()
        if icon is not None:
            self.theme_btn = ttk.Button(bar, image=icon, padding=(6, 3),
                                        command=self.cycle_theme)
        else:
            self.theme_btn = ttk.Button(bar, text=self._theme_icon(), width=3,
                                        padding=(6, 3), command=self.cycle_theme)
        self.theme_btn.pack(side="right", padx=(14, 0), pady=(2, 0))
        self._attach_tooltip(self.theme_btn, self._theme_tooltip)
        self.path_label = ttk.Label(bar, text="", style="Hint.TLabel")  # not shown: a file path means nothing to players
        ttk.Separator(self).pack(fill="x", padx=28, pady=(16, 0))

    def _build_tabs(self):
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=True, padx=24, pady=(14, 10))
        self._build_online_tab()  # first tab; the game asks for the disc image itself on first start
        for name, fields in GROUPS:
            scroll = ScrollFrame(self.notebook)
            self._scrolls.append(scroll)
            self.notebook.add(scroll, text=name)
            body = ttk.Frame(scroll.inner, padding=(28, 22, 28, 30))
            body.pack(fill="both", expand=True)
            body.columnconfigure(1, weight=1)
            row = 0
            if name == "Scaling":
                ttk.Label(body, text="Game running slow? Lower Render scale first (the biggest effect), "
                          "then set Scene MSAA to Off. Hover ⓘ for what each setting does.",
                          style="Hint.TLabel", wraplength=560, justify="left").grid(
                    row=0, column=0, columnspan=2, sticky="w", pady=(0, 14))
                row = 1
            for i, f in enumerate(fields):
                row = self._add_field(body, row, f, first=(i == 0))
        # Re-evaluate which settings apply whenever a value changes.
        for var in [*self.vars.values(), self.online_mode]:
            var.trace_add("write", lambda *_: self._update_gates())
        self.after_idle(self._update_gates)

    def _build_online_tab(self):
        prefs = load_prefs()
        # Always open offline (nothing goes online unless picked this session); Tailscale is the default way.
        self.online_mode = tk.StringVar(value="Offline")
        self.online_net = tk.StringVar(value="Tailscale")
        self.online_name = tk.StringVar(value=prefs.get("online_name") or default_player_name())
        self.online_peer = tk.StringVar(value=prefs.get("online_peer", ""))
        self._public_ip = None
        scroll = ScrollFrame(self.notebook)
        self._scrolls.append(scroll)
        self.notebook.add(scroll, text="Online")
        self.online_tab = scroll
        body = ttk.Frame(scroll.inner, padding=(28, 22, 28, 30))
        body.pack(fill="both", expand=True)
        body.columnconfigure(1, weight=1)
        body.columnconfigure(0, minsize=150)  # keep the layout steady when rows hide
        pad = dict(padx=(0, 26), pady=(16, 0))

        def label(row, text):
            lbl = ttk.Label(body, text=text)
            lbl.grid(row=row, column=0, sticky="e", **pad)
            return lbl

        def radios(row, var, choices, stack=False):
            f = ttk.Frame(body)
            f.grid(row=row, column=1, sticky="w", pady=pad["pady"])
            for i, (value, text) in enumerate(choices):
                b = ttk.Radiobutton(f, text=text, value=value, variable=var)
                b.pack(anchor="w", pady=(6 if i else 0, 0)) if stack else b.pack(side="left", padx=(0, 18))
            return f

        self._links = []
        self._link_font = tkfont.Font(font=ttk.Style(self).lookup("TLabel", "font") or "TkDefaultFont")
        self._link_font.configure(underline=True)

        def link(parent, text, url):
            w = ttk.Label(parent, text=text, style="Link.TLabel", cursor="hand2",
                          foreground=self._field_colors()[2], font=self._link_font)
            w.bind("<Button-1>", lambda _e: webbrowser.open(url))
            self._links.append(w)
            return w

        label(0, "Play")
        radios(0, self.online_mode, (("Offline", "Offline"), ("Host", "Host a game"), ("Join", "Join a friend")))
        rows = {}
        rows["net"] = (label(1, "Connection"),
                       radios(1, self.online_net, (("LAN", "Same home network"),
                                                   ("Tailscale", "Over the internet (Tailscale, recommended)"),
                                                   ("Public", "Over the internet (advanced: port forwarding)")), stack=True))
        rows["net"][0].grid_configure(sticky="ne")  # label beside the first of the stacked options
        name_row = ttk.Frame(body)
        name_row.grid(row=3, column=1, sticky="w", pady=pad["pady"])
        ttk.Entry(name_row, textvariable=self.online_name, width=12).pack(side="left")
        ttk.Label(name_row, text=f"max {NAME_MAX} characters, different from your friend's",
                  style="Hint.TLabel").pack(side="left", padx=(10, 0))
        rows["name"] = (label(3, "Player name"), name_row)

        # Host: this machine's address (for the chosen connection) to send to the friend.
        addr_row = ttk.Frame(body)
        addr_row.grid(row=4, column=1, sticky="w", pady=pad["pady"])
        self._addr_label = ttk.Label(addr_row, text="")
        self._addr_label.pack(side="left")
        self._copy_btn = ttk.Button(addr_row, text="Copy", padding=(6, 1), command=self._copy_address)
        self._copy_btn.pack(side="left", padx=(12, 0))
        self._public_btn = ttk.Button(addr_row, text="Find my address", padding=(6, 1),
                                      command=self._fetch_public_ip)
        rows["addr"] = (label(4, "Your address"), addr_row)

        # Join: the host's address.
        peer_row = ttk.Frame(body)
        peer_row.grid(row=5, column=1, sticky="w", pady=pad["pady"])
        ttk.Entry(peer_row, textvariable=self.online_peer, width=20).pack(side="left")
        ttk.Button(peer_row, text="Paste", padding=(6, 1), command=self._paste_peer).pack(side="left", padx=(8, 0))
        rows["peer"] = (label(5, "Host's address"), peer_row)

        # One or two lines of help for the current choice, plus links.
        self._hint = ttk.Label(body, text="", style="Hint.TLabel", wraplength=540, justify="left")
        self._hint.grid(row=6, column=1, sticky="w", pady=(18, 0))
        links = ttk.Frame(body)
        links.grid(row=7, column=1, sticky="w", pady=(10, 0))
        self._get_ts = link(links, "Tailscale setup guide", TAILSCALE_GUIDE_URL)
        link(links, "How to play online", GUIDE_URL).pack(side="left", padx=(0, 18))
        link(links, "Can't connect?", HELP_URL).pack(side="left", padx=(0, 18))

        def show(key, on):
            for w in rows[key]:
                w.grid() if on else w.grid_remove()

        def gate(*_):
            mode, net = self.online_mode.get(), self.online_net.get()
            online = mode != "Offline"
            show("net", online)
            show("name", online)
            show("addr", mode == "Host")
            show("peer", mode == "Join")
            ts = tailscale_ip() if online and net == "Tailscale" else None
            self._get_ts.pack_forget()
            if mode == "Host":
                self._public_btn.pack_forget()
                self._copy_btn.pack_forget()
                addr = {"Tailscale": ts, "Public": self._public_ip, "LAN": lan_ip()}[net]
                missing = {"Tailscale": "Tailscale isn't running on this PC",
                           "LAN": "Couldn't find this PC's home-network address"}.get(net, "")
                self._addr_label.configure(text=addr or missing)
                self._copy_btn.configure(state="normal" if addr else "disabled")
                if net == "Public" and not addr:
                    self._public_btn.pack(side="left")
                else:
                    self._copy_btn.pack(side="left", padx=(12, 0))
            if online and net == "Tailscale":
                self._get_ts.pack(side="left", padx=(0, 18))
            self._hint.configure(text=self._online_hint(mode, net, ts))
            self._update_launch_label()

        self.online_mode.trace_add("write", gate)
        self.online_net.trace_add("write", gate)
        self._online_gate = gate
        gate()

    def _online_hint(self, mode, net, ts):
        if mode == "Offline":
            return "Play on this PC. To play against a friend, pick Host or Join."
        if mode == "Host":
            if net == "Tailscale":
                first = ("Send your address to your friend, then press Host Game." if ts else
                         "Tailscale isn't set up on this PC yet: follow the setup guide below (about 5 minutes).")
            elif net == "LAN":
                first = "Send this address to your friend. No router setup needed."
            else:
                first = "Forward the game's ports on your router first (see How to play online), then send your address."
            return first + "\nIn the game: ONLINE > Quick Match > Search, then press X (Create Game)."
        first = {"Tailscale": "Enter the address your host sent you (100.x.y.z with Tailscale).",
                 "Public": "Enter the address your host sent you (the host forwards the game's ports).",
                 "LAN": "Enter the host's home-network address (usually 192.168.x.y)."}[net]
        return first + "\nStart after the host. In the game: ONLINE > Quick Match > Search."

    def _update_launch_label(self):
        btn = getattr(self, "launch_btn", None)
        if btn is not None:
            btn.configure(text={"Host": "Host Game", "Join": "Join Game"}.get(self.online_mode.get(), "Play Offline"))

    def _copy_address(self):
        text = self._addr_label.cget("text")
        if text and " " not in text:
            self._copy_text(text, self._copy_btn)

    def _copy_text(self, text, button):
        if not text:
            return
        self.clipboard_clear()
        self.clipboard_append(text)
        button.configure(text="Copied!")
        self.after(1500, lambda: button.configure(text="Copy"))

    def _paste_peer(self):
        try:
            self.online_peer.set(self.clipboard_get().strip())
        except tk.TclError:
            pass

    def _fetch_public_ip(self):
        self._public_btn.configure(state="disabled", text="Asking...")
        result = {}

        def work():
            try:
                result["ip"] = public_ip()
            except Exception as e:  # offline, blocked, ...
                result["err"] = str(e)

        def done():
            if t.is_alive():
                self.after(200, done)
                return
            self._public_btn.configure(state="normal", text="Find my address")
            if "ip" in result:
                self._public_ip = result["ip"]
            else:
                messagebox.showerror("Public IP", "Could not look up your public IP:\n" + result["err"], parent=self)
            self._online_gate()

        t = threading.Thread(target=work, daemon=True)
        t.start()
        self.after(200, done)

    def _online_launch_env(self):
        """Env for the chosen online mode; None (after telling the user why) if it can't launch."""
        mode, net, name = self.online_mode.get(), self.online_net.get(), self.online_name.get().strip()
        if mode == "Offline":
            return {}

        def fail(msg):
            messagebox.showerror("Online", msg, parent=self)
            self.notebook.select(self.online_tab)
            return None

        err = check_name(name)
        if err:
            return fail(err)
        peer = ""
        if mode == "Join":
            raw = self.online_peer.get().strip()
            if not raw:
                return fail("Enter the host's address (ask your host for it).")
            try:
                peer = resolve_peer(raw)
            except ValueError:
                return fail(f"'{raw}' doesn't look like an address. Ask your host for theirs: with Tailscale it "
                            "looks like 100.x.y.z.")
            if net == "Tailscale" and not is_tailscale_ip(peer) and not messagebox.askyesno(
                    "Online", f"{peer} isn't a Tailscale address (those look like 100.x.y.z). Join anyway?",
                    parent=self):
                return None
        if net == "Tailscale" and not tailscale_ip() and not messagebox.askyesno(
                "Online", "Tailscale isn't running on this PC, so your friend can't reach you through it. "
                "Start anyway?", parent=self):
            return None
        save_prefs(online_name=name, online_peer=self.online_peer.get().strip())
        return online_env(mode, name, peer)

    def _add_field(self, parent, row, f, first=False):
        key, label = f["key"], f["label"]
        top = 0 if first else 16
        lbl = ttk.Label(parent, text=label)
        lbl.grid(row=row, column=0, sticky="e", pady=(top, 0), padx=(0, 26))

        var = tk.StringVar()
        self.vars[key] = var
        ftype = f["type"]
        widget_frame = ttk.Frame(parent)
        widget_frame.grid(row=row, column=1, sticky="ew", pady=(top, 0))

        extra = []
        normal_state = "normal"
        if ftype == "bool":
            w = ttk.Checkbutton(widget_frame, variable=var, onvalue="true", offvalue="false")
            w.pack(side="left")
        elif ftype == "tristate":
            w = ttk.Combobox(widget_frame, textvariable=var, state="readonly",
                             values=["Auto", "On", "Off"], width=10)
            w.pack(side="left")
            normal_state = "readonly"
        elif ftype == "int" and f.get("options"):
            w = ttk.Combobox(widget_frame, textvariable=var, state="readonly",
                             values=f["options"], width=10)
            w.pack(side="left")
            normal_state = "readonly"
        elif ftype == "int":
            w = ttk.Spinbox(widget_frame, textvariable=var, from_=f.get("min", -999),
                            to=f.get("max", 999), width=8)
            w.pack(side="left")
        elif ftype == "str" and f.get("options"):
            w = ttk.Combobox(widget_frame, textvariable=var, state="readonly",
                             values=f["options"], width=24)
            w.pack(side="left")
            normal_state = "readonly"
        else:
            w = ttk.Entry(widget_frame, textvariable=var)
            w.pack(side="left", fill="x", expand=True)

        # A small info button, with the game's own description on hover.
        info = None
        info_text = INFO.get(key)
        if info_text:
            info = ttk.Label(widget_frame, text="ⓘ", style="Link.TLabel", cursor="hand2")
            info.pack(side="left", padx=(10, 0))  # right beside its control
            self._attach_tooltip(info, lambda t=info_text: t)

        self.field_widgets[key] = {"widget": w, "label": lbl, "info": info,
                                   "extra": extra, "state": normal_state}
        return row + 1

    def _field_colors(self):
        """Label and info-button colours for the current theme."""
        style = ttk.Style(self)
        dark = self._resolve_theme() == "dark"
        normal = style.lookup("TLabel", "foreground", ("!disabled",)) or (
            "#fafafa" if dark else "#1c1c1c")
        faded = style.lookup("TLabel", "foreground", ("disabled",)) or (
            "#595959" if dark else "#a0a0a0")
        link = "#4ea1ff" if dark else "#0a66d6"
        return normal, faded, link

    def _set_field_enabled(self, key, enabled):
        info = self.field_widgets.get(key)
        if info is None:
            return
        try:
            info["widget"].configure(state=info["state"] if enabled else "disabled")
        except tk.TclError:
            pass
        for b in info["extra"]:
            try:
                b.configure(state="normal" if enabled else "disabled")
            except tk.TclError:
                pass
        try:
            normal, faded, link = self._field_colors()
            # The labels carry an explicit -foreground from the palette, so a
            # style swap alone wouldn't dim them; set the colour directly.
            info["label"].configure(foreground=normal if enabled else faded)
            if info["info"] is not None:
                info["info"].configure(foreground=link if enabled else faded)
        except tk.TclError:
            pass

    def _update_gates(self):
        try:
            values = {k: v.get() for k, v in self.vars.items()}
            values["_online_mode"] = self.online_mode.get() if hasattr(self, "online_mode") else "Offline"
            for f in ALL_FIELDS:
                req = f.get("requires")
                self._set_field_enabled(
                    f["key"], True if req is None else bool(req(values)))
        except tk.TclError:
            pass  # widgets are being torn down
        except Exception as e:
            self._log_error(f"Could not update enabled settings: {e}")
        self._recolor_online()

    def _recolor_online(self):
        """sv-ttk repaints every label on a theme change; put the hint/link colours back."""
        palette = getattr(self, "_palette", None)
        if not palette:
            return
        try:
            for w in getattr(self, "_links", ()):
                w.configure(foreground=palette[1])
            if hasattr(self, "_hint"):
                self._hint.configure(foreground=palette[0])
        except tk.TclError:
            pass

    def _build_footer(self):
        ttk.Separator(self).pack(fill="x", padx=28, pady=(6, 0))
        foot = ttk.Frame(self, padding=(28, 14, 28, 20))
        foot.pack(fill="x")
        self.launch_btn = ttk.Button(foot, text="Launch Game", style="Accent.TButton",
                                     command=self.launch)
        self.launch_btn.pack(side="right", padx=(10, 0))
        self._update_launch_label()
        ttk.Button(foot, text="Save", command=self.save).pack(side="right", padx=(0, 8))
        ttk.Button(foot, text="Reset", command=self.reset).pack(side="left")  # away from Save

    # -- data --------------------------------------------------------------
    def load(self):
        try:
            if os.path.isfile(CONFIG):
                with open(CONFIG, "r", encoding="utf-8") as fh:
                    text = fh.read()
            else:
                text = default_config_text()
                os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
                with open(CONFIG, "w", encoding="utf-8") as fh:
                    fh.write(text)
        except OSError as e:
            # Show the defaults and keep going; Save will report if it fails.
            self._report_error("Could not read the settings file", e)
            text = default_config_text()
        self._text = text
        values = read_config(text)

        def _hashas(key):
            for line in text.splitlines():
                m = _ACTIVE.match(line)
                if m and m.group(2) == key:
                    return True
            return False

        for f in ALL_FIELDS:
            key, var = f["key"], self.vars[f["key"]]
            if f["type"] == "tristate":
                if _hashas(key):
                    var.set("On" if values.get(key) else "Off")
                else:
                    var.set("Auto")
            elif key in values:
                v = values[key]
                if f["type"] == "bool":
                    var.set("true" if v else "false")
                else:
                    var.set(str(v))
            else:
                if f["type"] == "bool":
                    var.set("true" if f["default"] else "false")
                else:
                    var.set("" if f["default"] is None else str(f["default"]))
        self.path_label.config(text=CONFIG)
        self._update_gates()
        self._loaded_snapshot = {k: v.get() for k, v in self.vars.items()}
        self._config_sig = self._config_signature()
        self._pending_sig = None
        self._external_change = False

    def _config_signature(self):
        try:
            st = os.stat(CONFIG)
            return (st.st_mtime_ns, st.st_size)
        except OSError:
            return None

    def _is_dirty(self):
        if self._loaded_snapshot is None:
            return False
        return {k: v.get() for k, v in self.vars.items()} != self._loaded_snapshot

    def _poll_config(self):
        """Reload when the game (or an editor) changes the file. A change must
        be stable across two polls (so a write in progress is never read), and
        unsaved launcher edits are never clobbered silently."""
        if not self.winfo_exists():
            return
        try:
            sig = self._config_signature()
            if sig is None or sig == self._config_sig:
                self._pending_sig = None
            elif sig == self._pending_sig:
                self._pending_sig = None
                if self._is_dirty():
                    # Keep the launcher's edits; Save will ask before replacing
                    # the file's newer contents.
                    self._external_change = True
                else:
                    self.load()
            else:
                self._pending_sig = sig
        except Exception as e:
            self._log_error(f"Config auto-reload failed: {e}")
        try:
            self.after(1000, self._poll_config)
        except tk.TclError:
            pass

    def collect(self):
        """Widget values -> dict key -> (value, type)."""
        out = {}
        for f in ALL_FIELDS:
            key, ftype = f["key"], f["type"]
            raw = self.vars[key].get()
            if ftype == "bool":
                out[key] = (raw == "true", "bool")
            elif ftype == "tristate":
                out[key] = ({"Auto": None, "On": True, "Off": False}[raw], "tristate")
            elif ftype == "int":
                try:
                    out[key] = (int(raw), "int")
                except ValueError:
                    raise ValueError(f"'{f['label']}' must be a whole number (got '{raw}').")
            else:
                out[key] = (raw, "str")
        return out

    def save(self):
        try:
            data = self.collect()
        except ValueError as e:
            messagebox.showerror("Invalid setting", str(e), parent=self)
            return False
        if self._external_change:
            choice = messagebox.askyesnocancel(
                "Settings changed on disk",
                "The settings file was changed by the game (or another editor) "
                "while you had unsaved changes.\n\n"
                "Yes: replace those changes with yours.\n"
                "No: reload the file and discard your changes.\n"
                "Cancel: keep editing without saving.",
                parent=self)
            if choice is None:
                return False
            if not choice:
                self.load()  # discard the launcher's edits
                return False
        text = self._text
        for f in ALL_FIELDS:
            key, ftype = f["key"], f["type"]
            value, _ = data[key]
            if ftype == "tristate":
                if value is None:
                    text = _comment_key(text, key)
                else:
                    text = _replace_key(text, key, _literal(value, ftype))
            else:
                text = _replace_key(text, key, _literal(value, ftype))
        try:
            os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
            with open(CONFIG, "w", encoding="utf-8") as fh:
                fh.write(text)
        except OSError as e:
            self._report_error("Cannot save the settings", e)
            return False
        self._text = text
        self._loaded_snapshot = {k: v.get() for k, v in self.vars.items()}
        self._config_sig = self._config_signature()
        self._pending_sig = None
        self._external_change = False
        return True

    def reset(self):
        if not messagebox.askyesno("Reset", "Reset every setting on screen to the "
                                   "defaults? (Nothing is written until you Save.)",
                                   parent=self):
            return
        for f in ALL_FIELDS:
            var = self.vars[f["key"]]
            if f["type"] == "tristate":
                var.set("Auto")
            elif f["type"] == "bool":
                var.set("true" if f["default"] else "false")
            else:
                var.set("" if f["default"] is None else str(f["default"]))

    # -- launch ------------------------------------------------------------
    def find_launcher(self):
        if os.name == "nt":
            exe = os.path.join(ROOT, "thebigs2.exe")
            return exe if os.path.isfile(exe) else None
        preferred = os.path.join(ROOT, "TheBigs2-x86_64.AppImage")
        dev = self._dev_build()
        try:
            if os.path.isfile(preferred) and os.access(preferred, os.X_OK):
                # In a development checkout a fresher dev build wins over an old AppImage.
                if dev and os.path.getmtime(dev) > os.path.getmtime(preferred):
                    return dev
                return preferred
            names = sorted(os.listdir(ROOT))
        except OSError:
            names = []
        for name in names:
            if name.lower().endswith(".appimage"):
                path = os.path.join(ROOT, name)
                try:
                    if os.access(path, os.X_OK):
                        return path
                except OSError:
                    continue
        beside = os.path.join(ROOT, "thebigs2")  # macOS release zip: the game sits next to the launcher
        if os.path.isfile(beside) and os.access(beside, os.X_OK):
            return beside
        return self._dev_build()

    @staticmethod
    def _dev_build():
        for build in ("linux-amd64-release", "mac-release"):  # mac: packaging/build-macos.sh
            dev = os.path.join(ROOT, "out", "build", build, "thebigs2")
            try:
                if os.path.isfile(dev) and os.access(dev, os.X_OK):
                    return dev
            except OSError:
                pass
        return None

    def launch(self):
        if self.proc is not None and self.proc.poll() is None:
            messagebox.showinfo("Already running", "The game is already running.", parent=self)
            return
        if not self.save():
            return
        extra_env = self._online_launch_env()
        if extra_env is None:
            return
        launcher = self.find_launcher()
        if not launcher:
            messagebox.showerror(
                "No launcher found",
                "Could not find TheBigs2-x86_64.AppImage (or the dev build) in:\n" + ROOT,
                parent=self)
            return
        if os.name != "nt" and not os.access(launcher, os.X_OK):
            messagebox.showerror(
                "Not executable",
                f"The launcher is not executable:\n{launcher}\n\n"
                "Make it executable (chmod +x) and try again.",
                parent=self)
            return
        try:
            self.proc = subprocess.Popen([launcher], cwd=ROOT, env={**os.environ, **extra_env})
        except Exception as e:
            self._report_error("Cannot launch the game", e)
            return
        try:
            self.launch_btn.config(state="disabled")
        except tk.TclError:
            pass
        self.after(1000, self._poll_proc)

    def _poll_proc(self):
        if self.proc is None:
            return
        try:
            code = self.proc.poll()
        except Exception as e:
            self._log_error(f"Could not poll the game process: {e}")
            code = -1
        if code is None:
            self.after(1000, self._poll_proc)
            return
        self.proc = None
        try:
            self.launch_btn.config(state="normal")
        except tk.TclError:
            pass


def main():
    if "--online-env" in sys.argv:
        # tools/run_pair.sh: print the online env as KEY=VALUE lines.
        # Usage: thebigs2_gui.py --online-env Host|Join NAME [HOST_IP]
        args = sys.argv[sys.argv.index("--online-env") + 1:]
        mode, name = args[0], args[1]
        peer = resolve_peer(args[2]) if mode == "Join" else ""
        err = check_name(name)
        if err:
            sys.exit(err)
        print("\n".join(f"{k}={v}" for k, v in online_env(mode, name, peer).items()))
        return
    lock_fd = None
    if "--check" not in sys.argv:
        try:
            lock_fd = acquire_single_instance()
        except AlreadyRunning:
            _show_already_running()
            return
    try:
        app = Launcher()
    except Exception as e:
        # Tk itself failed (no display, broken theme, ...): explain on stderr
        # and, if a root exists, with a dialog.
        import traceback
        traceback.print_exc()
        try:
            root = tk.Tk()
            root.withdraw()
            messagebox.showerror(
                "The Bigs 2 launcher could not start",
                f"{e}\n\nRun it from a terminal to see the full error.")
            root.destroy()
        except Exception:
            pass
        return

    app._lock_fd = lock_fd  # keep the single-instance lock for the app's life

    if "--check" in sys.argv:
        # Headless sanity: print what we parsed, then quit.
        values = read_config(app._text)
        print("config:", CONFIG)
        print("theme preference:", app.theme_pref,
              "-> resolved:", app._resolve_theme())
        print("keys:", len(values))
        for k in ("fullscreen", "display_resolution", "resolution_scale",
                  "frame_rate", "render_target_path_vulkan"):
            print(f"  {k} = {values.get(k)!r}")
        print("launcher:", app.find_launcher())
        print("theme engine:", "sv-ttk" if sv_ttk is not None else "clam fallback")
        app.destroy()
        return
    try:
        app.mainloop()
    except Exception as e:
        app._report_error("The launcher stopped unexpectedly", e)


if __name__ == "__main__":
    main()
