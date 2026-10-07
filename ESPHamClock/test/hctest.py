#!/usr/bin/env python3
"""
hctest.py -- black-box menu test harness for HamClock.

Drives a HamClock purely through its RESTful web interface:
    set_touch?x=&y=      synthesize a tap
    get_capture.bmp      read the screen back (RGB565)
    get_config.txt       settings snapshot (to prove Cancel changes nothing)
    get_sys.txt          UpTime (to detect crashes / restarts)
    set_pane?PaneN=...   put a given plot choice into a pane slot

Nothing is compiled into HamClock and no source changes are needed. Menus are found
*visually*: after a tap the screen is diffed against a baseline and the popup's Ok/Cancel
buttons are located from their white borders; the menu is then dismissed through those
buttons, so no per-menu coordinates are required beyond "where do I tap to open it".

Sub-commands
    run       test every menu: chrome entry points + every pane choice's hot spots
    discover  grid-scan the screen to find tap spots that open menus (writes catalog.json)
    onta      regression test for the On the Air scroll snap-back / deferred-refresh behavior
    mock      run only the mock backend, so your own HamClock can use  -b 127.0.0.1:PORT

Either attach to a running HamClock (--url http://host:8080) or let the harness launch and
manage its own headless instance (--launch ./hamclock-web-800x480) with a seeded config, a
fixed clock and a mock backend. Pure standard library; Python 3.8+.
"""
import argparse
import array
import calendar
import html
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BASE_W, BASE_H = 800, 480       # layout all built-in coordinates are expressed in
WHITE = 0xFFFF                  # RGB565 white, menu borders
CELL = 8                        # volatile-mask granularity, pixels
MENU_RH = 11                    # menu row height at 800x480 (menu.cpp MENU_RH)
MENU_TO_S = 30                  # HamClock closes an untouched menu after this long
def utc_now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime())

# every plot choice HamClock can put in a pane (HamClock.h PLOTNAMES_*), plus X-Ray
PANE_CHOICES = [
    "VOACAP_DEDX", "DE_Wx", "DX_Cluster", "DX_Wx", "Solar_Flux", "Planetary_K", "Moon",
    "NOAA_SpcWx", "Sunspot_N", "X-Ray", "Rotator", "ENV_Temp", "ENV_Press", "ENV_Humid",
    "ENV_DewPt", "SDO", "Solar_Wind", "DRAP", "Countdown", "Contests", "Live_Spots", "Bz_Bt",
    "On_The_Air", "ADIF", "Aurora", "DXPeditions", "Disturbance", "Storms", "Nets", "Launches",
    "HF_Bands", "VHF_Cond", "Sat_Alerts", "Mesh_Mon", "Eclipse", "Nearby_APRS", "Balloons",
    "HamAlert", "Band_Act", "Marine_Wx", "Fire_Wx", "Quakes", "POTA_Sked",
]

# ----------------------------------------------------------------------------------------
# frames
# ----------------------------------------------------------------------------------------

_LUT = None


def _lut():
    global _LUT
    if _LUT is None:
        _LUT = [bytes((((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63,
                       (p & 31) * 255 // 31)) for p in range(65536)]
    return _LUT


class Diff:
    def __init__(self):
        self.n = 0
        self.bbox = None

    def add(self, x, y):
        self.n += 1
        if self.bbox is None:
            self.bbox = [x, y, x, y]
        else:
            b = self.bbox
            b[0] = min(b[0], x); b[1] = min(b[1], y); b[2] = max(b[2], x); b[3] = max(b[3], y)

    def __repr__(self):
        return "Diff(n=%d bbox=%s)" % (self.n, self.bbox)


class Frame:
    """a captured screen: RGB565 pixels, row major"""

    def __init__(self, w, h, pix):
        self.w, self.h, self.pix = w, h, pix

    @classmethod
    def from_bmp(cls, data):
        if data[:2] != b"BM":
            raise ValueError("capture is not a BMP")
        off = struct.unpack_from("<I", data, 10)[0]
        w, h = struct.unpack_from("<ii", data, 18)
        bpp = struct.unpack_from("<H", data, 28)[0]
        if bpp != 16:
            raise ValueError("expected 16 bpp capture, got %d" % bpp)
        top_down = h < 0
        h = abs(h)
        pix = array.array("H")
        pix.frombytes(data[off:off + w * h * 2])
        if sys.byteorder == "big":
            pix.byteswap()
        if not top_down:
            rows = [pix[y * w:(y + 1) * w] for y in range(h)]
            pix = array.array("H")
            for r in reversed(rows):
                pix.extend(r)
        return cls(w, h, pix)

    def row(self, y):
        return self.pix[y * self.w:(y + 1) * self.w]

    def copy(self):
        return Frame(self.w, self.h, array.array("H", self.pix))

    def diff(self, other, volatile=None, box=None, skip=None):
        """count differing pixels. volatile: set of (cy,cx) cells to ignore; box: only look
        inside (x0,y0,x1,y1); skip: list of boxes to ignore."""
        d = Diff()
        x_lo, y_lo, x_hi, y_hi = 0, 0, self.w - 1, self.h - 1
        if box:
            x_lo, y_lo = max(0, box[0]), max(0, box[1])
            x_hi, y_hi = min(self.w - 1, box[2]), min(self.h - 1, box[3])
        for y in range(y_lo, y_hi + 1):
            base = y * self.w
            a = self.pix[base + x_lo:base + x_hi + 1]
            b = other.pix[base + x_lo:base + x_hi + 1]
            if a == b:
                continue
            for i, (p, q) in enumerate(zip(a, b)):
                if p == q:
                    continue
                x = x_lo + i
                if volatile and ((y // CELL), (x // CELL)) in volatile:
                    continue
                if skip and any(s[0] <= x <= s[2] and s[1] <= y <= s[3] for s in skip):
                    continue
                d.add(x, y)
        return d

    def changed_cells(self, other):
        cells = set()
        for y in range(self.h):
            if self.row(y) == other.row(y):
                continue
            a, b = self.row(y), other.row(y)
            for x in range(self.w):
                if a[x] != b[x]:
                    cells.add((y // CELL, x // CELL))
        return cells

    def nonblack(self, box):
        n = 0
        for y in range(box[1], box[3] + 1):
            n += sum(1 for p in self.pix[y * self.w + box[0]:y * self.w + box[2] + 1] if p)
        return n

    def region_crc(self, box):
        c = 0
        for y in range(box[1], box[3] + 1):
            c = zlib.crc32(self.pix[y * self.w + box[0]:y * self.w + box[2] + 1].tobytes(), c)
        return c

    def draw_rect(self, box, color=0xF800):
        x0, y0, x1, y1 = [int(v) for v in box]
        for x in range(max(0, x0), min(self.w - 1, x1) + 1):
            for y in (y0, y1):
                if 0 <= y < self.h:
                    self.pix[y * self.w + x] = color
        for y in range(max(0, y0), min(self.h - 1, y1) + 1):
            for x in (x0, x1):
                if 0 <= x < self.w:
                    self.pix[y * self.w + x] = color

    def save_png(self, path):
        lut = _lut()
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)
            raw += b"".join(map(lut.__getitem__, self.row(y)))

        def chunk(tag, data):
            c = struct.pack(">I", len(data)) + tag + data
            return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        png += chunk(b"IEND", b"")
        with open(path, "wb") as f:
            f.write(png)


# ----------------------------------------------------------------------------------------
# HamClock REST client
# ----------------------------------------------------------------------------------------

class HCError(Exception):
    pass


class HamClock:
    def __init__(self, base, timeout=30):
        self.base = base.rstrip("/")
        self.timeout = timeout

    def get(self, path, tries=3):
        last = None
        for i in range(tries):
            try:
                with urllib.request.urlopen(self.base + path, timeout=self.timeout) as r:
                    return r.read()
            except (urllib.error.URLError, OSError, ConnectionError) as e:
                last = e
                time.sleep(0.5 * (i + 1))
        raise HCError("%s: %s" % (path, last))

    def text(self, path):
        return self.get(path).decode("utf-8", "replace")

    def touch(self, x, y):
        r = self.text("/set_touch?x=%d&y=%d" % (x, y))
        if "Web touch" not in r:
            raise HCError("set_touch refused: %s" % r.strip())

    def capture(self):
        return Frame.from_bmp(self.get("/get_capture.bmp"))

    def config(self):
        return self.text("/get_config.txt")

    def uptime(self):
        m = re.search(r"UpTime\s+(\d+)d(\d+):(\d+):(\d+)", self.text("/get_sys.txt"))
        if not m:
            raise HCError("no UpTime in get_sys.txt")
        d, h, mi, s = map(int, m.groups())
        return ((d * 24 + h) * 60 + mi) * 60 + s

    def utc_now(self):
        m = re.search(r"Clock_UTC\s+(\S+)T(\S+)\s", self.text("/get_time.txt"))
        if not m:
            raise HCError("no Clock_UTC")
        return calendar.timegm(time.strptime(m.group(1) + "T" + m.group(2), "%Y-%m-%dT%H:%M:%S"))

    def set_pane(self, n, value):
        return self.text("/set_pane?Pane%d=%s" % (n, value))

    def alive(self):
        try:
            self.uptime()
            return True
        except HCError:
            return False


# ----------------------------------------------------------------------------------------
# menu detection
# ----------------------------------------------------------------------------------------

def white_runs(after, before, minrun=24):
    """horizontal runs of white pixels in `after` that are mostly new relative to `before`"""
    runs = []
    w = after.w
    for y in range(after.h):
        a, b = after.row(y), before.row(y)
        if a == b:
            continue
        x = 0
        while x < w:
            if a[x] == WHITE:
                s = x
                while x < w and a[x] == WHITE:
                    x += 1
                if x - s >= minrun:
                    changed = sum(1 for i in range(s, x) if a[i] != b[i])
                    if changed * 2 >= x - s:
                        runs.append((y, s, x - 1))
            else:
                x += 1
    runs.sort()
    merged = []
    for r in runs:
        if merged and merged[-1][0] == r[0] and r[1] - merged[-1][2] <= 3:
            merged[-1] = (r[0], merged[-1][1], r[2])
        else:
            merged.append(r)
    return merged


class Menu:
    def __init__(self, ok, cancel, box):
        self.ok, self.cancel, self.box = ok, cancel, box     # (x0,y0,x1,y1) each, or None

    @staticmethod
    def center(b):
        return (b[0] + b[2]) // 2, (b[1] + b[3]) // 2

    def dismiss_point(self):
        """Cancel when the menu has one, else its (full width) Ok"""
        return self.center(self.cancel or self.ok)

    def ok_point(self):
        return self.center(self.ok)

    def __repr__(self):
        return "Menu(box=%s ok=%s cancel=%s)" % (self.box, self.ok, self.cancel)


def find_menu(after, before):
    """locate a HamClock popup menu that appeared between `before` and `after`, else None.
    Every menu ends in an Ok button (and usually a Cancel beside it), each a white-bordered
    box; the popup itself is enclosed by a longer white border above and below those."""
    runs = white_runs(after, before)
    if not runs:
        return None
    by_x = {}
    for y, x0, x1 in runs:
        by_x.setdefault((x0, x1), []).append(y)
    boxes = []
    for (x0, x1), ys in by_x.items():
        ys.sort()
        for i, y in enumerate(ys):
            nxt = [y2 for y2 in ys[i + 1:] if 8 <= y2 - y <= 26]
            if nxt and x1 - x0 >= 20:
                boxes.append((x0, y, x1, nxt[0]))
    if not boxes:
        return None
    lowest = max(b[3] for b in boxes)
    row = sorted(b for b in boxes if abs(b[3] - lowest) <= 1)
    ok = row[0]
    cancel = row[-1] if len(row) > 1 else None
    # outer border: a longer run just below the buttons, and the matching one above
    lo = (cancel or ok)[2]
    bottom = [(y, x0, x1) for (y, x0, x1) in runs
              if lowest < y <= lowest + 14 and x0 <= ok[0] and x1 >= lo]
    box = None
    if bottom:
        yb, xb0, xb1 = max(bottom, key=lambda r: r[2] - r[1])
        tops = [y for (y, x0, x1) in runs if x0 == xb0 and x1 == xb1 and y < yb]
        box = (xb0, max(tops) if tops else max(0, ok[1] - 30), xb1, yb)
    return Menu(ok, cancel, box)


# ----------------------------------------------------------------------------------------
# session: volatile calibration, settle, results
# ----------------------------------------------------------------------------------------

class Result:
    def __init__(self, name, group=""):
        self.name, self.group = name, group
        self.status = "PASS"
        self.notes = []
        self.shots = []
        self.menu_fp = None
        self.secs = 0.0

    def fail(self, msg):
        self.status = "FAIL"
        self.notes.append(msg)

    def warn(self, msg):
        if self.status == "PASS":
            self.status = "WARN"
        self.notes.append(msg)

    def skip(self, msg):
        self.status = "SKIP"
        self.notes.append(msg)

    def error(self, msg):
        self.status = "ERROR"
        self.notes.append(msg)


class Session:
    def __init__(self, hc, outdir, tol=60, managed=None, verbose=True):
        self.hc, self.outdir, self.tol = hc, outdir, tol
        self.managed = managed
        self.verbose = verbose
        self.volatile = set()
        self.scale = 1.0
        self.results = []
        self.settle_px = 12
        os.makedirs(os.path.join(outdir, "shots"), exist_ok=True)

    def log(self, msg):
        if self.verbose:
            print(msg, flush=True)

    def S(self, x, y):
        return int(round(x * self.scale)), int(round(y * self.scale))

    def tap(self, x, y, wait=0.45):
        sx, sy = self.S(x, y)
        self.hc.touch(sx, sy)
        time.sleep(wait)

    def tap_px(self, x, y, wait=0.45):
        """tap in real (already scaled) pixel coordinates"""
        self.hc.touch(int(x), int(y))
        time.sleep(wait)

    def init_geometry(self):
        f = self.hc.capture()
        self.scale = f.w / float(BASE_W)
        return f

    def calibrate(self, n=4, gap=0.8):
        """learn which screen cells keep changing on their own (clock seconds etc)"""
        prev = self.hc.capture()
        for _ in range(n - 1):
            time.sleep(gap)
            cur = self.hc.capture()
            self.volatile |= prev.changed_cells(cur)
            prev = cur
        return prev

    def settle(self, timeout=5.0, quiet=2, gap=0.3):
        prev = self.hc.capture()
        stable = 0
        t0 = time.time()
        while time.time() - t0 < timeout:
            time.sleep(gap)
            cur = self.hc.capture()
            if cur.diff(prev, self.volatile).n <= self.settle_px:
                stable += 1
            else:
                stable = 0
            prev = cur
            if stable >= quiet:
                return prev, True
        return prev, False

    def shot(self, res, label, frame, boxes=()):
        f = frame.copy()
        for b in boxes:
            if b:
                f.draw_rect(b, 0xF800)
        fn = "%s_%d_%s.png" % (re.sub(r"[^A-Za-z0-9]+", "_", res.name)[:60], len(self.results), label)
        f.save_png(os.path.join(self.outdir, "shots", fn))
        res.shots.append((label, "shots/" + fn))

    def liveness(self, res, up0):
        try:
            up = self.hc.uptime()
        except HCError as e:
            res.fail("HamClock not responding: %s" % e)
            self.recover(res)
            return False
        if up < up0:
            res.fail("HamClock restarted during the test (uptime %ds -> %ds)" % (up0, up))
            return False
        return True

    def recover(self, res):
        if self.managed:
            code = self.managed.exit_code()
            res.fail("HamClock process exited (code %s) -- see %s" % (code, self.managed.logpath))
            self.log("   restarting managed HamClock after crash")
            self.managed.restart()
            self.hc = HamClock(self.hc.base)
            self.calibrate()

    # ---- the core test: open a menu, inspect it, dismiss it, prove nothing changed ----

    def run_menu_test(self, name, taps, group="", pre=None, deep=False, ok_path=False,
                      expect_menu=True, ok_safe=True):
        res = Result(name, group)
        t0 = time.time()
        self.log("-- %s" % name)
        try:
            if pre:
                pre(res)
            if res.status == "SKIP":
                return self._done(res, t0)
            base, settled = self.settle()
            if not settled:
                res.warn("screen never settled before the tap (animated pane?); tolerance applies")
            cfg0 = self.hc.config()
            up0 = self.hc.uptime()

            for (x, y) in taps:
                self.tap(x, y)
            shown, _ = self.settle(timeout=4.0)
            menu = find_menu(shown, base)
            self.shot(res, "open", shown, [menu.box, menu.ok, menu.cancel] if menu else [])

            if not menu:
                if expect_menu:
                    res.fail("no popup menu detected after tapping %s" % (taps,))
                self._restore_if_changed(res, base, cfg0)
                self.liveness(res, up0)
                return self._done(res, t0)

            self._check_geometry(res, shown, menu)
            res.menu_fp = self._fingerprint(shown, menu)

            if deep:
                self._monkey(res, base, menu)
                shown2, _ = self.settle(timeout=3.0)
                menu2 = find_menu(shown2, base)
                menu = menu2 or menu

            # dismiss via Cancel (or Ok-only menus' Ok) and prove nothing was left behind
            self.tap_px(*menu.dismiss_point(), wait=0.6)
            after, settled = self.settle(timeout=5.0)
            d = after.diff(base, self.volatile)
            self.shot(res, "closed", after, [d.bbox] if d.bbox else [])
            if d.n > self.tol:
                res.fail("screen not restored after dismiss: %d px differ, bbox %s" % (d.n, d.bbox))
            cfg1 = self.hc.config()
            if cfg1 != cfg0:
                res.fail("config changed after Cancel: %s" % _cfg_delta(cfg0, cfg1))
            self.liveness(res, up0)

            if ok_path and ok_safe and res.status != "ERROR":
                self._ok_path(res, taps, base, cfg0, up0)
        except HCError as e:
            res.error("harness/HamClock error: %s" % e)
            self.recover(res)
        except Exception as e:                      # never let one test kill the run
            res.error("%s: %s" % (type(e).__name__, e))
        return self._done(res, t0)

    def _done(self, res, t0):
        res.secs = time.time() - t0
        self.results.append(res)
        self.log("   %s %s" % (res.status, "; ".join(res.notes)[:200]))
        return res

    def _restore_if_changed(self, res, base, cfg0):
        after, _ = self.settle(timeout=3.0)
        d = after.diff(base, self.volatile)
        if d.n > self.tol:
            res.warn("tap changed the screen by %d px without opening a menu (bbox %s)" % (d.n, d.bbox))

    def _check_geometry(self, res, shown, menu):
        W, H = shown.w, shown.h
        for nm, b in (("menu", menu.box), ("Ok", menu.ok), ("Cancel", menu.cancel)):
            if b and (b[0] < 0 or b[1] < 0 or b[2] >= W or b[3] >= H):
                res.fail("%s box %s extends past the %dx%d screen" % (nm, b, W, H))
        if menu.box is None:
            res.warn("menu outline not found; Ok/Cancel found at %s/%s" % (menu.ok, menu.cancel))
        elif menu.ok and not (menu.box[0] <= menu.ok[0] and menu.ok[2] <= menu.box[2]
                              and menu.box[1] <= menu.ok[1] and menu.ok[3] <= menu.box[3]):
            res.fail("Ok button %s not inside menu %s" % (menu.ok, menu.box))

    def _fingerprint(self, frame, menu):
        if not menu.box:
            return None
        b = menu.box
        return "%dx%d:%08x" % (b[2] - b[0], b[3] - b[1], frame.region_crc((b[0], b[1], b[2], b[3])))

    def _ok_path(self, res, taps, base, cfg0, up0):
        """re-open, press Ok without touching anything: config and screen must not change"""
        for (x, y) in taps:
            self.tap(x, y)
        shown, _ = self.settle(timeout=4.0)
        menu = find_menu(shown, base)
        if not menu:
            res.warn("Ok path: menu did not reopen")
            return
        self.tap_px(*menu.ok_point(), wait=0.8)
        after, _ = self.settle(timeout=6.0)
        d = after.diff(base, self.volatile)
        self.shot(res, "after_ok", after, [d.bbox] if d.bbox else [])
        if d.n > self.tol:
            res.warn("Ok with no edits changed the screen: %d px, bbox %s" % (d.n, d.bbox))
        cfg1 = self.hc.config()
        if cfg1 != cfg0:
            res.warn("Ok with no edits changed config: %s" % _cfg_delta(cfg0, cfg1))
        self.liveness(res, up0)

    def _monkey(self, res, base, menu):
        """tap every row of the open menu at three x positions. Nothing outside the menu may
        change, HamClock must stay alive, and the menu must stay open. Never taps Ok/Cancel."""
        if not menu.box:
            res.warn("deep: no menu outline, skipped")
            return
        box = list(menu.box)
        rh = max(8, int(MENU_RH * self.scale))
        taps = 0
        y = box[1] + rh // 2 + 1
        up0 = self.hc.uptime()
        while y < (menu.ok[1] - 2 if menu.ok else box[3] - 6):
            for fx in (0.12, 0.45, 0.78):
                x = int(box[0] + fx * (box[2] - box[0]))
                before = self.hc.capture()
                self.tap_px(x, y, wait=0.4)
                after = self.hc.capture()
                taps += 1
                m2 = find_menu(after, base)
                if m2 is None:
                    # a tap inside an open menu must not close it; let it settle and re-check
                    after, _ = self.settle(timeout=2.0)
                    m2 = find_menu(after, base)
                if m2 is None:
                    res.warn("deep: menu vanished after tap at (%d,%d)" % (x, y))
                    return
                nb = m2.box or tuple(box)
                outside_box = (min(box[0], nb[0]) - 3, min(box[1], nb[1]) - 3,
                               max(box[2], nb[2]) + 3, max(box[3], nb[3]) + 3)
                d = after.diff(before, self.volatile, skip=[outside_box])
                if d.n > self.tol:
                    res.fail("deep: tap at (%d,%d) changed %d px OUTSIDE the menu, bbox %s"
                             % (x, y, d.n, d.bbox))
                    self.shot(res, "deep_outside_%d_%d" % (x, y), after, [outside_box, d.bbox])
                if not self.hc.alive():
                    res.fail("deep: HamClock stopped responding after tap at (%d,%d)" % (x, y))
                    self.recover(res)
                    return
                if m2.box:
                    box = list(m2.box)
                    menu.ok, menu.cancel = m2.ok or menu.ok, m2.cancel
            y += rh
        self.liveness(res, up0)
        res.notes.append("deep: %d taps inside the menu" % taps) if res.status == "PASS" else None


def _cfg_delta(a, b):
    la, lb = a.splitlines(), b.splitlines()
    out = []
    for i in range(max(len(la), len(lb))):
        x = la[i].strip() if i < len(la) else ""
        y = lb[i].strip() if i < len(lb) else ""
        if x != y:
            out.append("'%s' -> '%s'" % (x, y))
    return "; ".join(out[:4]) or "(whitespace)"


# ----------------------------------------------------------------------------------------
# catalog of entry points
# ----------------------------------------------------------------------------------------

PANE_X0 = [None, 235, 405, 575]        # pane 1..3 left edge at 800x480
PANE_W = 160

# where to tap inside a pane to reach its menus: title (pane picker / pane-specific menu),
# the sub-title/count row, and a listing row
PANE_HOTSPOTS = [("title", 80, 12), ("subtitle", 80, 36), ("listing", 80, 62)]


def builtin_catalog():
    """chrome entry points that are the same on every 800x480 layout. `hctest.py discover`
    finds more (and corrects any that moved) and can write them to catalog.json."""
    cat = []
    cat.append({"name": "Callsign title", "taps": [[115, 25]], "group": "chrome"})
    cat.append({"name": "Pane 1 picker", "taps": [[320, 12]], "group": "chrome"})
    cat.append({"name": "Pane 2 picker", "taps": [[490, 12]], "group": "chrome"})
    cat.append({"name": "Pane 3 picker", "taps": [[655, 12]], "group": "chrome"})
    cat.append({"name": "NCDXF beacon box", "taps": [[770, 6]], "group": "chrome"})
    return cat


def load_catalog(path):
    if path and os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "catalog.json")
    if os.path.exists(here):
        with open(here) as f:
            return json.load(f)
    return builtin_catalog()


def pane_entries(slot=1, choices=None):
    out = []
    x0 = PANE_X0[slot]
    for label in (choices or PANE_CHOICES):
        for hname, dx, y in PANE_HOTSPOTS:
            out.append({"name": "Pane%d %s / %s" % (slot, label, hname),
                        "taps": [[x0 + dx, y]], "group": "pane:" + label,
                        "panes": {str(slot): label}})
    return out


# ----------------------------------------------------------------------------------------
# mock backend
# ----------------------------------------------------------------------------------------

class MockBackend:
    """serves just enough of HamClock's backend for the On the Air pane: onta.txt (with
    controllable, deterministic content) plus empty companion files; 404 for everything else"""

    PROGS = [("POTA", "US-%04d", 14), ("SOTA", "W7A/AN-%03d", 3), ("WWFF", "KFF-%04d", 3)]
    FREQS = [7074000, 7200000, 10130000, 14074000, 14250000, 18100000, 21200000, 24950000,
             28400000, 50125000, 14060000, 7030000]
    MODES = ["SSB", "CW", "FT8", "FM", "", "SSB", "CW"]

    def __init__(self, hamclock_now, n_spots=139, max_age_min=30, seed=1234, port=0):
        self.lock = threading.Lock()
        self.now0 = hamclock_now
        self.clock0 = time.time()
        self.seed = seed
        self.spots = []
        self.requests = []
        self.max_age_min = max_age_min
        self.add_spots(n_spots)
        outer = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                path = self.path.split("?")[0]
                outer.requests.append(path)
                body = outer.render(path)
                if body is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", port), H)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def hc_now(self):
        return int(self.now0 + (time.time() - self.clock0))

    def add_spots(self, n):
        rnd = random.Random(self.seed + len(self.spots))
        with self.lock:
            for _ in range(n):
                i = len(self.spots)
                prog, reffmt, wt = rnd.choices(self.PROGS, weights=[p[2] for p in self.PROGS])[0]
                call = "%s%d%s%s%s" % (rnd.choice("KWN"), rnd.randint(0, 9), rnd.choice("ABCDEFGHJKLMNPQRSTUVWXYZ"),
                                       rnd.choice("ABCDEFGHJKLMNPQRSTUVWXYZ"), rnd.choice("ABCDEFGHJKLMNPQRSTUVWXYZ"))
                ref = reffmt % rnd.randint(1, 999)
                age = rnd.randint(0, self.max_age_min * 60 // 2)
                self.spots.append((call, rnd.choice(self.FREQS) + rnd.randint(0, 20) * 100,
                                   self.hc_now() - age, rnd.choice(self.MODES),
                                   "FN%02d" % rnd.randint(0, 40), 28 + rnd.random() * 20,
                                   -120 + rnd.random() * 50, ref, prog))

    def render(self, path):
        # HamClock always asks for /ham/HamClock/<page>
        if path.startswith("/ham/HamClock"):
            path = path[len("/ham/HamClock"):]
        if path == "/ONTA/onta.txt":
            with self.lock:
                lines = ["%s,%d,%d,%s,%s,%.3f,%.3f,%s,%s" % s for s in self.spots]
            return ("\n".join(lines) + "\n").encode()
        if path in ("/ONTA/iota_spots.txt", "/ONTA/xonta_spots.txt", "/ONTA/onta_parks.txt"):
            return b"# none\n"
        if path == "/cty/cty_wt_mod-ll-dxcc.txt":
            return self.cty()
        return None

    _CTY = None

    @classmethod
    def cty(cls):
        """minimal country table so call2DXCC() works for US calls. HamClock insists the file
        is >= 800000 bytes, and skips '#' lines, so pad with comments. Lines must be grouped
        by first character (radix index) in sorted order."""
        if cls._CTY is None:
            rows = ["%s 39.8 -98.6 291" % p for p in
                    ["AA", "AB", "AC", "AD", "AE", "AF", "AG", "AH", "AI", "AJ", "AK", "AL",
                     "K", "N", "W"]]
            body = "# synthetic cty table for hctest\n" + "\n".join(rows) + "\n"
            pad = "# padding to satisfy the 800000 byte minimum size check ..........\n"
            body += pad * ((810000 - len(body)) // len(pad) + 1)
            cls._CTY = body.encode()
        return cls._CTY

    def stop(self):
        self.httpd.shutdown()


# ----------------------------------------------------------------------------------------
# managed HamClock process
# ----------------------------------------------------------------------------------------

class Eeprom:
    """HamClock's eeprom file: text, one '%08X %02X' line per byte; each item is a 0x5A cookie
    followed by its value, locations from doc/eeprom/hceeprom.csv"""

    def __init__(self, csv_path):
        self.items = {}
        with open(csv_path) as f:
            next(f)
            for line in f:
                parts = line.rstrip("\n").split(",", 4)
                if len(parts) >= 4 and parts[0].startswith("0x"):
                    self.items[parts[1]] = (int(parts[0], 16), int(parts[2]), parts[3])
        self.buf = bytearray(4096)

    def set(self, name, value):
        if name not in self.items:
            raise KeyError("%s not in eeprom csv" % name)
        addr, n, typ = self.items[name]
        if typ == "s":
            data = str(value).encode()[:n - 1].ljust(n, b"\0")
        elif typ == "f":
            data = struct.pack("<f", float(value))
        else:
            data = int(value).to_bytes(n, "little")
        self.buf[addr - 1] = 0x5A
        self.buf[addr:addr + n] = data

    def write(self, directory):
        with open(os.path.join(directory, "eeprom"), "w") as f:
            for a, v in enumerate(self.buf):
                f.write("%08X %02X\n" % (a, v))


class Managed:
    def __init__(self, binary, port=8080, backend=None, eeprom_csv=None, settings=None,
                 start_time=None, workdir=None, boot_timeout=180):
        self.binary = os.path.abspath(binary)
        self.port = port
        self.backend = backend
        self.settings = settings or {}
        # N.B. default is real UTC, NOT a fixed date: HamClock ages its cache files by comparing
        # its own clock with their real mtimes, so a fake clock stops caches from ever expiring
        self.start_time = start_time or utc_now_iso()
        self.boot_timeout = boot_timeout
        self.workdir = workdir or tempfile.mkdtemp(prefix="hctest_")
        self.logpath = os.path.join(self.workdir, "hamclock.log")
        if not eeprom_csv:
            for cand in (os.path.join(os.path.dirname(self.binary), "..", "doc", "eeprom", "hceeprom.csv"),
                         os.path.join(os.path.dirname(self.binary), "hceeprom.csv")):
                if os.path.exists(cand):
                    eeprom_csv = cand
                    break
        if not eeprom_csv:
            raise SystemExit("need --eeprom-csv (doc/eeprom/hceeprom.csv from the HamClock repo)")
        self.csv = eeprom_csv
        self.proc = None

    def start(self):
        for f in os.listdir(self.workdir):
            if f == "eeprom":
                os.unlink(os.path.join(self.workdir, f))
        ee = Eeprom(self.csv)
        ee.set("NV_CALLSIGN", "W1AW")
        ee.set("NV_DE_LAT", 41.7)
        ee.set("NV_DE_LNG", -72.7)
        ee.set("NV_DE_GRID", "FN31pr")
        for k, v in self.settings.items():
            ee.set(k, v)
        ee.write(self.workdir)
        cmd = [self.binary, "-k", "-s", self.start_time, "-d", self.workdir, "-o",
               "-e", str(self.port), "-w", "-1", "-r", "-1"]
        if self.backend:
            cmd += ["-b", self.backend]
        self.log = open(self.logpath, "w")
        self.proc = subprocess.Popen(cmd, stdout=self.log, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True,
                                     cwd=os.path.dirname(self.binary))
        t0 = time.time()
        while time.time() - t0 < self.boot_timeout:
            if self.proc.poll() is not None:
                raise SystemExit("HamClock exited during boot (code %s), see %s"
                                 % (self.proc.returncode, self.logpath))
            try:
                with open(self.logpath, errors="replace") as f:
                    if "Starting Arduino loop()" in f.read():
                        break
            except OSError:
                pass
            time.sleep(1.0)
        else:
            raise SystemExit("HamClock did not finish booting in %ds, see %s"
                             % (self.boot_timeout, self.logpath))
        hc = HamClock("http://127.0.0.1:%d" % self.port)
        for _ in range(30):
            if hc.alive():
                break
            time.sleep(1)
        time.sleep(4)

    def exit_code(self):
        return self.proc.poll() if self.proc else None

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def restart(self):
        self.stop()
        self.start()


# ----------------------------------------------------------------------------------------
# suites
# ----------------------------------------------------------------------------------------

def snapshot_panes(hc):
    panes = {}
    for line in hc.config().splitlines():
        m = re.match(r"Pane(\d)\s+(.*?)\s*$", line)
        if m:
            panes[int(m.group(1))] = m.group(2)
    return panes


def restore_panes(hc, panes):
    for n, v in sorted(panes.items()):
        try:
            hc.set_pane(n, v if v else "off")
        except HCError:
            pass


def make_pre(S, panes):
    """pre-step: put the requested choices into pane slots; skip the test if refused"""
    def pre(res):
        for slot, label in (panes or {}).items():
            r = S.hc.set_pane(int(slot), label)
            time.sleep(1.0)
            cfg = S.hc.config()
            if not re.search(r"Pane%s\s+.*%s" % (slot, re.escape(label)), cfg):
                res.skip("pane choice %s not available here (%s)" % (label, r.strip()[:80]))
                return
            S.settle(timeout=6.0)
    return pre


def suite_smoke(S):
    res = Result("smoke: REST + capture", "smoke")
    t0 = time.time()
    try:
        f = S.hc.capture()
        if (f.w, f.h) != (BASE_W, BASE_H):
            res.warn("build is %dx%d; coordinates are scaled by %.2f" % (f.w, f.h, f.w / BASE_W))
        if f.nonblack((0, 0, f.w - 1, f.h - 1)) < 1000:
            res.fail("screen is essentially blank")
        S.hc.config(); S.hc.uptime()
        S.shot(res, "screen", f)
    except Exception as e:
        res.error(str(e))
    S._done(res, t0)


def run_entries(S, entries, deep, ok_path):
    for e in entries:
        pre = make_pre(S, e.get("panes")) if e.get("panes") else None
        S.run_menu_test(e["name"], [tuple(t) for t in e["taps"]], group=e.get("group", ""),
                        pre=pre, deep=deep, ok_path=ok_path,
                        expect_menu=e.get("expect_menu", True), ok_safe=e.get("ok_safe", True))


# ----------------------------------------------------------------------------------------
# discover
# ----------------------------------------------------------------------------------------

def cmd_discover(S, args):
    regions = [(0, 0, 800, 148), (0, 148, 138, 480), (139, 148, 800, 172)]
    if args.include_map:
        regions.append((139, 172, 800, 480))
    found, actions = [], []
    step = args.step
    pts = []
    for (x0, y0, x1, y1) in regions:
        for y in range(y0 + step // 2, y1, step):
            for x in range(x0 + step // 2, x1, step):
                pts.append((x, y))
    S.log("discover: %d taps, step %d" % (len(pts), step))
    base, _ = S.settle()
    for i, (x, y) in enumerate(pts):
        sx, sy = S.S(x, y)
        S.hc.touch(sx, sy)
        time.sleep(0.5)
        frame = S.hc.capture()
        d = frame.diff(base, S.volatile)
        if d.n <= S.tol:
            continue
        frame, _ = S.settle(timeout=3.0)
        menu = find_menu(frame, base)
        if menu:
            fp = S._fingerprint(frame, menu)
            known = [f for f in found if f["fingerprint"] == fp]
            S.log("  (%d,%d): menu %s%s" % (x, y, menu.box, "  [same as %s]" % known[0]["taps"] if known else ""))
            if not known:
                found.append({"name": "Menu at (%d,%d)" % (x, y), "taps": [[x, y]],
                              "group": "discovered", "fingerprint": fp, "box": menu.box})
            S.tap_px(*menu.dismiss_point(), wait=0.6)
        else:
            S.log("  (%d,%d): non-menu change, %d px bbox %s" % (x, y, d.n, d.bbox))
            actions.append({"x": x, "y": y, "px": d.n, "bbox": d.bbox})
        base, _ = S.settle()
    out = {"menus": found, "non_menu_changes": actions}
    path = args.write or os.path.join(S.outdir, "discovered.json")
    with open(path, "w") as f:
        json.dump(out, f, indent=1)
    if args.write:
        cat = [{k: v for k, v in m.items() if k not in ("fingerprint", "box")} for m in found]
        with open(args.write, "w") as f:
            json.dump(cat, f, indent=1)
    S.log("discover: %d distinct menus; wrote %s" % (len(found), path))


# ----------------------------------------------------------------------------------------
# On the Air scroll regression
# ----------------------------------------------------------------------------------------

def test_onta_scroll(S, mock, slot, dwell, check_new, new_wait):
    x0 = int(PANE_X0[slot] * S.scale)
    sc = S.scale
    listing = (x0 + 1, int(47 * sc), x0 + int(150 * sc), int(146 * sc))
    age_col = [(x0 + int(147 * sc), int(40 * sc), x0 + int(160 * sc), int(148 * sc))]
    ctrl = (x0 + int(125 * sc), 0, x0 + int(159 * sc), int(32 * sc))
    new_sym = (x0 + int(5 * sc), int(17 * sc), x0 + int(28 * sc), int(28 * sc))
    down = (x0 + int(137 * sc), int(21 * sc))
    up = (x0 + int(137 * sc), int(9 * sc))

    res = Result("ONTA scroll: stays put at end of list (slot %d)" % slot, "onta")
    t0 = time.time()
    try:
        S.hc.set_pane(slot, "On_The_Air")
        # wait for the listing to populate
        for _ in range(90):
            f = S.hc.capture()
            if f.nonblack(listing) > 300:
                break
            time.sleep(1)
        else:
            res.error("On the Air pane never showed any spots (is the backend reachable?)")
            S.shot(res, "empty_pane", S.hc.capture(), [listing])
            return S._done(res, t0)
        S.settle(timeout=8)
        first = S.hc.capture()
        S.shot(res, "first_page", first, [listing])

        def lst_diff(a, b):
            return a.diff(b, S.volatile, box=listing, skip=age_col).n

        pages, prev, same = 1, first, 0
        wrapped = reached_end = False
        for _ in range(80):
            S.tap_px(*down, wait=0.7)
            cur = S.hc.capture()
            if lst_diff(cur, prev) <= 20:
                same += 1
                if same >= 2:
                    reached_end = True
                    break
            else:
                same = 0
                pages += 1
                if pages >= 4 and lst_diff(cur, first) <= 20:
                    wrapped = True              # back on page 1 without ever stopping at the end
                    break
            prev = cur
        last = S.hc.capture()
        S.shot(res, "last_page", last, [listing])
        res.notes.append("%d pages reached with the down arrow" % pages)
        if wrapped:
            res.fail("list wrapped back to the first page after %d pages instead of stopping at the end "
                     "(snap-back)" % pages)
            return S._done(res, t0)
        if not reached_end:
            res.fail("down arrow never ran out of pages in 80 presses (list keeps changing under it)")
            return S._done(res, t0)
        if pages < 3:
            res.warn("only %d pages -- too few spots to exercise paging" % pages)
        if lst_diff(first, last) <= 20:
            res.fail("down arrow never moved the list")

        time.sleep(dwell)
        later = S.hc.capture()
        n = lst_diff(later, last)
        c = later.diff(last, S.volatile, box=ctrl).n
        S.shot(res, "after_dwell", later, [listing])
        if n > 150 or c > 40:
            res.fail("list changed by itself %.0fs after paging to the end (%d listing px, %d arrow px): "
                     "it snapped back / refreshed" % (dwell, n, c))
        else:
            res.notes.append("held its position for %.0fs at the end of the list" % dwell)

        # one page back up, then dwell again: a scrolled-away view must never be refreshed under you
        S.tap_px(*up, wait=0.7)
        mid = S.hc.capture()
        time.sleep(dwell)
        later = S.hc.capture()
        n = lst_diff(later, mid)
        if n > 150:
            res.fail("list changed by itself %.0fs after scrolling up one page (%d px)" % (dwell, n))

        if check_new and mock is not None:
            held = S.hc.capture()
            quiet_new = held.nonblack(new_sym)
            mock.add_spots(6)
            S.log("   waiting up to %ds for the New indicator" % new_wait)
            seen = False
            t1 = time.time()
            while time.time() - t1 < new_wait:
                f = S.hc.capture()
                if abs(f.nonblack(new_sym) - quiet_new) > 15:
                    seen = True
                    break
                time.sleep(3)
            if not seen:
                res.fail("New indicator did not appear within %ds of new spots arriving" % new_wait)
            else:
                f = S.hc.capture()
                S.shot(res, "new_shown", f, [new_sym])
                if lst_diff(f, held) > 150:
                    res.fail("list changed under the user while New was pending (refresh not deferred)")
                S.tap_px(x0 + int(15 * sc), int(23 * sc), wait=1.0)
                after, _ = S.settle(timeout=40)
                S.shot(res, "after_new_tap", after, [listing])
                if after.nonblack(new_sym) > quiet_new + 15:
                    res.fail("New indicator still shown after tapping it")
                if lst_diff(after, held) <= 20:
                    res.fail("tapping New did not refresh/re-home the list")
                else:
                    res.notes.append("New appeared, list stayed put, tapping New refreshed it")
    except Exception as e:
        res.error("%s: %s" % (type(e).__name__, e))
    S._done(res, t0)


# ----------------------------------------------------------------------------------------
# report
# ----------------------------------------------------------------------------------------

def write_report(S, path_base, extra=""):
    rs = S.results
    counts = {}
    for r in rs:
        counts[r.status] = counts.get(r.status, 0) + 1
    fps = {r.menu_fp for r in rs if r.menu_fp}
    rows = []
    for r in rs:
        shots = " ".join('<a href="%s">%s</a>' % (p, html.escape(l)) for l, p in r.shots)
        rows.append('<tr class="%s"><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%.1fs</td></tr>' % (
            r.status, r.status, html.escape(r.name), html.escape("; ".join(r.notes)), shots, r.secs))
    page = """<!doctype html><meta charset=utf-8><title>HamClock menu test report</title>
<style>body{font:14px sans-serif;margin:1.5em}table{border-collapse:collapse;width:100%%}
td{border:1px solid #ccc;padding:3px 6px;vertical-align:top}.PASS td:first-child{background:#cfc}
.FAIL td:first-child,.ERROR td:first-child{background:#f99}.WARN td:first-child{background:#fe9}
.SKIP td:first-child{background:#ddd}</style>
<h2>HamClock menu test report</h2><p>%s &mdash; %d tests, <b>%d distinct menus</b> seen.</p>%s
<table><tr><th>Status</th><th>Test</th><th>Notes</th><th>Screens</th><th>Time</th></tr>%s</table>""" % (
        ", ".join("%s: %d" % kv for kv in sorted(counts.items())), len(rs), len(fps), extra, "\n".join(rows))
    with open(path_base + ".html", "w") as f:
        f.write(page)
    with open(path_base + ".json", "w") as f:
        json.dump([{"name": r.name, "group": r.group, "status": r.status, "notes": r.notes,
                    "menu": r.menu_fp, "secs": round(r.secs, 1)} for r in rs], f, indent=1)
    return counts, len(fps)


# ----------------------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------------------

def build_session(args, need_mock=False, settings=None):
    managed = mock = None
    outdir = args.outdir
    os.makedirs(outdir, exist_ok=True)
    if args.launch:
        if need_mock or args.mock:
            mock = MockBackend(calendar.timegm(time.gmtime()), port=args.mock_port)
            backend = "127.0.0.1:%d" % mock.port
        else:
            backend = args.backend
        managed = Managed(args.launch, port=args.port, backend=backend,
                          eeprom_csv=args.eeprom_csv, settings=settings,
                          start_time=getattr(args, "start_time", None))
        print("launching %s (boot can take ~40s)..." % args.launch, flush=True)
        managed.start()
        base_url = "http://127.0.0.1:%d" % args.port
    else:
        base_url = args.url
    hc = HamClock(base_url)
    if not hc.alive():
        raise SystemExit("cannot reach HamClock at %s" % base_url)
    S = Session(hc, outdir, tol=args.tol, managed=managed)
    S.init_geometry()
    S.log("calibrating volatile screen areas...")
    S.calibrate()
    return S, managed, mock


def main():
    ap = argparse.ArgumentParser(description="HamClock menu test harness",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--url", default="http://127.0.0.1:8080", help="running HamClock REST base URL")
        p.add_argument("--launch", metavar="BINARY", help="launch and manage a headless hamclock-web-* binary instead")
        p.add_argument("--port", type=int, default=8080, help="REST port for --launch")
        p.add_argument("--backend", help="backend host:port for --launch (default: none/unreachable)")
        p.add_argument("--mock", action="store_true", help="with --launch, point it at the mock backend")
        p.add_argument("--mock-port", type=int, default=0)
        p.add_argument("--start-time", help="ISO UTC start time for --launch (default: now)")
        p.add_argument("--eeprom-csv", help="doc/eeprom/hceeprom.csv (found automatically next to a repo checkout)")
        p.add_argument("--outdir", default="hctest_report")
        p.add_argument("--tol", type=int, default=60, help="pixels allowed to differ before/after a menu")

    r = sub.add_parser("run", help="test every menu"); common(r)
    r.add_argument("--suite", choices=["all", "chrome", "panes"], default="all")
    r.add_argument("--catalog", help="catalog.json from `discover` (default: ./catalog.json or built-in)")
    r.add_argument("--only", help="only tests whose name contains this text")
    r.add_argument("--panes", help="comma list of pane choices to test (default: all)")
    r.add_argument("--slot", type=int, default=1, choices=[1, 2, 3], help="pane slot to put each choice in")
    r.add_argument("--deep", action="store_true", help="also tap every row inside each menu")
    r.add_argument("--ok-path", action="store_true", help="also reopen and press Ok with no edits")

    d = sub.add_parser("discover", help="scan the screen for menu entry points"); common(d)
    d.add_argument("--step", type=int, default=20)
    d.add_argument("--include-map", action="store_true", help="also scan the map (taps there set DX!)")
    d.add_argument("--write", help="write a catalog.json from what is found")

    o = sub.add_parser("onta", help="On the Air scroll / deferred-refresh regression"); common(o)
    o.add_argument("--sort", choices=["band", "call", "org", "age"], default="band")
    o.add_argument("--slot", type=int, default=1, choices=[1, 2, 3])
    o.add_argument("--dwell", type=float, default=12.0, help="seconds to wait at each scroll position")
    o.add_argument("--new", action="store_true", help="also test the New indicator (waits up to --new-wait)")
    o.add_argument("--new-wait", type=int, default=150)

    m = sub.add_parser("mock", help="run only the mock backend")
    m.add_argument("--port", type=int, default=9000)

    args = ap.parse_args()

    if args.cmd == "mock":
        mb = MockBackend(calendar.timegm(time.gmtime()), port=args.port)
        print("mock backend on 127.0.0.1:%d  (start HamClock with -b 127.0.0.1:%d)" % (mb.port, mb.port))
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            return 0

    settings = None
    if args.cmd == "onta":
        args.mock = True
        settings = {"NV_ONTASORTBY": {"band": 0, "call": 1, "org": 2, "age": 3}[args.sort],
                    "NV_ONTA_MAXAGE": 60}
    S, managed, mock = build_session(args, need_mock=(args.cmd == "onta"), settings=settings)
    saved = snapshot_panes(S.hc)
    code = 0
    try:
        if args.cmd == "discover":
            cmd_discover(S, args)
        elif args.cmd == "onta":
            if not managed:
                print("note: attached to an existing HamClock; it needs On the Air data (>3 pages) "
                      "and --new will not work without the mock backend")
            test_onta_scroll(S, mock, args.slot, args.dwell, args.new and mock is not None, args.new_wait)
        else:
            suite_smoke(S)
            entries = []
            if args.suite in ("all", "chrome"):
                entries += load_catalog(args.catalog)
            if args.suite in ("all", "panes"):
                entries += pane_entries(args.slot, args.panes.split(",") if args.panes else None)
            if args.only:
                entries = [e for e in entries if args.only.lower() in e["name"].lower()]
            run_entries(S, entries, args.deep, args.ok_path)
    finally:
        try:
            if args.cmd != "discover" and S.hc.alive():
                restore_panes(S.hc, saved)
        except Exception:
            pass
        if args.cmd != "discover" or S.results:
            counts, nmenus = write_report(S, os.path.join(args.outdir, "report"))
            print("\nresults: %s; %d distinct menus; report: %s" % (
                ", ".join("%s %d" % kv for kv in sorted(counts.items())), nmenus,
                os.path.join(args.outdir, "report.html")))
            if counts.get("FAIL") or counts.get("ERROR"):
                code = 1
        if managed:
            managed.stop()
        if mock:
            mock.stop()
    return code


if __name__ == "__main__":
    sys.exit(main())
