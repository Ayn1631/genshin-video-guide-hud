"""透明方向罗盘和 Windows 指引快捷键。"""

from __future__ import annotations

import ctypes
from ctypes import wintypes as win
from functools import lru_cache
import math
import os
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont


_BASE_WIDTH = 760
_BASE_HEIGHT = 134
_ARC_LIMIT = 90
_ARC_HALF_SPAN = 320
_TARGET_MARKER_SIZE = 25
_TIMING_WIDTH = 286
_TIMING_ROW = 20
_TIMING_PAD = 10
_TIMING_GAP = 6
_REFRESH = 1
_EXIT = 2
_WM_HOTKEY = 0x0312
_WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, win.HWND, win.UINT, win.WPARAM, win.LPARAM)


def hud_layout(bounds: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    left, top, width, height = bounds
    hud_width = max(1, min(1020, round(width * 0.53)))
    return (left + (width - hud_width) // 2, top + round(height * 0.008),
            hud_width, max(1, round(hud_width * _BASE_HEIGHT / _BASE_WIDTH)))


@lru_cache(maxsize=24)
def _font(size: int, angle: bool) -> ImageFont.FreeTypeFont:
    name = "segoeui.ttf" if angle else "msyh.ttc"
    return ImageFont.truetype(str(Path(os.environ["WINDIR"]) / "Fonts" / name), size)


def render_compass(
    size: tuple[int, int], label: str, detail: str = "", offset: int | None = None, distance_text: str = "",
) -> Image.Image:
    """生成透明 RGBA 图层，offset 为目标相对视角的有符号角度。"""
    width, height = size
    scale = 2 * width / _BASE_WIDTH

    def point(x: float, y: float) -> tuple[float, float]:
        return x * scale, y * scale

    def curve(x: float) -> float:
        return 106 - 41 * ((x - 380) / 362) ** 2

    def angle_x(angle: int) -> float:
        return 380 + _ARC_HALF_SPAN * angle / _ARC_LIMIT

    background = Image.new("RGBA", (2 * width, 2 * height))
    arc = Image.new("L", background.size)
    ImageDraw.Draw(arc).line([point(x, curve(x)) for x in range(18, 743, 2)],
                             fill=240, width=max(1, round(2.2 * scale)), joint="curve")
    fade = Image.new("L", (background.width, 1))
    fade.putdata([round(255 * max(0.0, 1 - ((x / scale - 380) / 362) ** 2))
                  for x in range(background.width)])
    layer = Image.new("RGBA", background.size, (216, 238, 255))
    layer.putalpha(ImageChops.multiply(arc, fade.resize(background.size, Image.Resampling.NEAREST)))
    draw = ImageDraw.Draw(layer)

    for angle in (-90, -60, -30, 30, 60, 90):
        x = angle_x(angle)
        y = curve(x)
        lean = (380 - x) / 210
        draw.line((point(x + lean, y - 5), point(x - lean, y + 5)),
                  fill=(234, 247, 255, 206), width=max(1, round(2 * scale)))

    draw.polygon([point(373, 72), point(387, 72), point(380, 82)], fill=(249, 253, 255, 250))
    center_y = curve(380)
    draw.polygon([point(380, center_y - 9), point(384, center_y),
                  point(380, center_y + 9), point(376, center_y)],
                 fill=(247, 253, 255, 233))
    is_angle = label.endswith("°")
    title = point(380, 25)
    title_font = _font(max(1, round((36 if is_angle else 20) * scale)), is_angle)
    stroke = max(1, round(scale * 0.45))
    draw.text(title, label, font=title_font, fill=(250, 253, 255, 255), anchor="mm",
              stroke_width=stroke, stroke_fill=(13, 28, 47, 112))
    if distance_text:
        _, _, title_right, _ = draw.textbbox(title, label, font=title_font, anchor="mm", stroke_width=stroke)
        draw.text((title_right + 10 * scale, title[1]), distance_text,
                  font=_font(max(1, round(18 * scale)), False), fill=(237, 248, 255, 244),
                  anchor="lm", stroke_width=stroke, stroke_fill=(15, 30, 47, 155))
    if detail:
        draw.text(point(380, 54), detail, font=_font(max(1, round(15 * scale)), False),
                  fill=(237, 248, 255, 244), anchor="mm", stroke_width=max(1, round(scale * 0.55)),
                  stroke_fill=(15, 30, 47, 155))
    if offset is not None:
        rear = abs(offset) > _ARC_LIMIT
        x = angle_x(max(-_ARC_LIMIT, min(_ARC_LIMIT, offset)))
        y = curve(x)
        if rear:
            side = math.copysign(1, offset)
            marker = [(x - side * 7, y - 10), (x + side * 7, y), (x - side * 7, y + 10),
                      (x - side * 2, y)]
        else:
            marker = [(x, y - 13), (x + 9, y), (x, y + 13), (x - 9, y)]
        marker = [point(*p) for p in marker]
        glow = Image.new("L", background.size)
        ImageDraw.Draw(glow).polygon(marker, fill=245)
        glow = glow.filter(ImageFilter.GaussianBlur(7 * scale))
        halo = Image.new("RGBA", background.size, (255, 69, 78))
        halo.putalpha(glow)
        background.alpha_composite(halo)
        draw.polygon(marker, fill=(255, 245, 241, 255), outline=(255, 92, 96, 255),
                     width=max(1, round(2.4 * scale)))

    background.alpha_composite(layer)
    return background.resize(size, Image.Resampling.LANCZOS)


@lru_cache(maxsize=24)
def render_target_marker(size: tuple[int, int], heading: int | None = None) -> Image.Image:
    image = Image.new("RGBA", (size[0] * 2, size[1] * 2))
    draw = ImageDraw.Draw(image)
    cx, cy = (image.width - 1) / 2, (image.height - 1) / 2
    red, border, shadow = (254, 62, 78, 255), (255, 237, 233, 255), (24, 18, 26, 210)
    if heading is not None:
        radians = math.radians(heading)
        dx, dy = math.sin(radians), -math.cos(radians)
        base_x, base_y = cx + 9 * dx, cy + 9 * dy
        tip = (cx + 22 * dx, cy + 22 * dy)
        arrow = [tip, (base_x - 7 * dy, base_y + 7 * dx), (base_x + 7 * dy, base_y - 7 * dx)]
        draw.line([*arrow, tip], fill=shadow, width=5, joint="curve")
        draw.polygon(arrow, fill=red, outline=border, width=2)
    draw.ellipse((cx - 11, cy - 11, cx + 11, cy + 11), fill=shadow)
    draw.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=red, outline=border, width=2)
    return image.resize(size, Image.Resampling.LANCZOS)


def timing_layout(bounds, sections):
    left, top, width, height = bounds
    margin = max(8, round(height * 0.014))
    units = 2 * _TIMING_PAD + sum(_TIMING_ROW * (1 + len(rows)) for _, rows in sections)
    units += _TIMING_GAP * (len(sections) - 1)
    offset = round(height * 0.23)
    scale = min(1.3, max(0.7, height / 950), (height - offset - margin) / units,
                (width - 2 * margin) / _TIMING_WIDTH)
    return (left + margin, top + offset, max(1, round(_TIMING_WIDTH * scale)), max(1, round(units * scale)))


def render_timings(size, sections) -> Image.Image:
    width, height = size
    scale = width / _TIMING_WIDTH
    panel = Image.new("RGBA", size)
    draw = ImageDraw.Draw(panel)
    draw.rounded_rectangle((0, 0, width - 1, height - 1), radius=max(1, round(8 * scale)),
                           fill=(15, 25, 39, 221), outline=(157, 194, 224, 105))
    left = round(12 * scale)
    right = width - left
    y = _TIMING_PAD + _TIMING_ROW / 2
    title_font = _font(max(1, round(14 * scale)), False)
    label_font = _font(max(1, round(13 * scale)), False)
    value_font = _font(max(1, round(13 * scale)), True)
    for index, (title, rows) in enumerate(sections):
        if index:
            divider = round((y - (_TIMING_ROW + _TIMING_GAP) / 2) * scale)
            draw.line((left, divider, right, divider), fill=(171, 205, 235, 76))
        draw.text((left, y * scale), title, font=title_font, anchor="lm", fill=(172, 222, 255, 255))
        for label, reading in rows:
            y += _TIMING_ROW
            font = value_font
            if isinstance(reading, str):
                value = reading
                font = label_font
            elif reading is None:
                value = "—"
            elif reading >= 1000:
                value = f"{reading / 1000:.2f} s"
            else:
                value = f"{reading:.1f} ms"
            draw.text((left, y * scale), label, font=label_font, anchor="lm", fill=(228, 241, 255, 235))
            draw.text((right, y * scale), value, font=font, anchor="rm", fill=(248, 252, 255, 255))
        y += _TIMING_ROW + _TIMING_GAP
    return panel


class _WindowClass(ctypes.Structure):
    _fields_ = [
        ("style", win.UINT), ("procedure", _WNDPROC),
        ("class_extra", ctypes.c_int), ("window_extra", ctypes.c_int),
        ("instance", win.HINSTANCE), ("icon", win.HICON), ("cursor", win.HANDLE),
        ("background", win.HBRUSH), ("menu_name", win.LPCWSTR), ("name", win.LPCWSTR),
    ]


class _BitmapInfo(ctypes.Structure):
    _fields_ = [
        ("size", win.DWORD), ("width", win.LONG), ("height", win.LONG),
        ("planes", win.WORD), ("bit_count", win.WORD), ("compression", win.DWORD),
        ("size_image", win.DWORD), ("x_pixels_per_meter", win.LONG), ("y_pixels_per_meter", win.LONG),
        ("colors_used", win.DWORD), ("colors_important", win.DWORD),
    ]


class _Blend(ctypes.Structure):
    _fields_ = [("operation", win.BYTE), ("flags", win.BYTE), ("opacity", win.BYTE), ("alpha_format", win.BYTE)]


def _function(dll, name, restype, *argtypes):
    function = getattr(dll, name)
    function.restype = restype
    function.argtypes = argtypes
    return function


class GuideHud:
    """带逐像素透明的置顶窗口，保留游戏焦点和鼠标操作。"""

    def __init__(self, refresh_enabled: bool = True, *, marker_only: bool = False, timing_only: bool = False) -> None:
        u = self._user = ctypes.WinDLL("user32", use_last_error=True)
        g = self._gdi = ctypes.WinDLL("gdi32", use_last_error=True)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.hwnd = None
        self._registered_keys: list[int] = []
        self._visible = False
        self._last = None
        self._size = (0, 0)
        self._dc = None
        self._bitmap = None
        self._previous_bitmap = None
        self._bits = ctypes.c_void_p()
        class_name = "BetterGIVideoGuideMarker" if marker_only else "BetterGIVideoGuide"
        if timing_only:
            class_name = "BetterGIVideoGuideTiming"
        self._class_name = f"{class_name}_{os.getpid()}"
        self._instance = _function(kernel, "GetModuleHandleW", win.HMODULE, win.LPCWSTR)(None)
        self._procedure = _WNDPROC(self._handle_message)
        self._class_registered = False

        self._def_window = _function(u, "DefWindowProcW", ctypes.c_ssize_t,
                                    win.HWND, win.UINT, win.WPARAM, win.LPARAM)
        _function(u, "DestroyWindow", win.BOOL, win.HWND)
        _function(u, "UnregisterClassW", win.BOOL, win.LPCWSTR, win.HINSTANCE)
        _function(u, "UnregisterHotKey", win.BOOL, win.HWND, ctypes.c_int)
        _function(u, "ShowWindow", win.BOOL, win.HWND, ctypes.c_int)
        _function(u, "PeekMessageW", win.BOOL, ctypes.POINTER(win.MSG), win.HWND, win.UINT, win.UINT, win.UINT)
        _function(u, "TranslateMessage", win.BOOL, ctypes.POINTER(win.MSG))
        _function(u, "DispatchMessageW", ctypes.c_ssize_t, ctypes.POINTER(win.MSG))
        _function(u, "UpdateLayeredWindow", win.BOOL, win.HWND, win.HDC, ctypes.POINTER(win.POINT),
                  ctypes.POINTER(win.SIZE), win.HDC, ctypes.POINTER(win.POINT),
                  win.DWORD, ctypes.POINTER(_Blend), win.DWORD)
        _function(g, "CreateCompatibleDC", win.HDC, win.HDC)
        _function(g, "CreateDIBSection", win.HBITMAP, win.HDC, ctypes.POINTER(_BitmapInfo), win.UINT,
                  ctypes.POINTER(ctypes.c_void_p), win.HANDLE, win.DWORD)
        _function(g, "SelectObject", win.HGDIOBJ, win.HDC, win.HGDIOBJ)
        _function(g, "DeleteObject", win.BOOL, win.HGDIOBJ)
        _function(g, "DeleteDC", win.BOOL, win.HDC)

        try:
            description = _WindowClass(0, self._procedure, 0, 0, self._instance, None, None,
                                       None, None, self._class_name)
            register = _function(u, "RegisterClassW", win.ATOM, ctypes.POINTER(_WindowClass))
            if not register(ctypes.byref(description)):
                raise ctypes.WinError(ctypes.get_last_error())
            self._class_registered = True
            create = _function(u, "CreateWindowExW", win.HWND, win.DWORD, win.LPCWSTR, win.LPCWSTR,
                               win.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                               win.HWND, win.HMENU, win.HINSTANCE, win.LPVOID)
            # TOPMOST | TRANSPARENT | TOOLWINDOW | LAYERED | NOACTIVATE
            style = 0x00000008 | 0x00000020 | 0x00000080 | 0x00080000 | 0x08000000
            self.hwnd = create(style, self._class_name, "攻略方向指引", 0x80000000, 0, 0,
                               0, 0, None, None, self._instance, None)
            if not self.hwnd:
                raise ctypes.WinError(ctypes.get_last_error())
            if marker_only or timing_only:
                return
            register_key = _function(u, "RegisterHotKey", win.BOOL, win.HWND, ctypes.c_int, win.UINT, win.UINT)
            modifiers = 0x4000 | 0x0002 | 0x0001  # NOREPEAT | CTRL | ALT
            for identifier, key, name in ((_REFRESH, 0x77, "F8"), (_EXIT, 0x79, "F10")):
                if identifier == _REFRESH and not refresh_enabled:
                    continue
                if not register_key(self.hwnd, identifier, modifiers, key):
                    raise RuntimeError(f"无法注册 Ctrl+Alt+{name}，可用 --console 启动")
                self._registered_keys.append(identifier)
        except Exception:
            self.close()
            raise

    def _handle_message(self, hwnd, message, wparam, lparam):
        if message == 0x0084:  # WM_NCHITTEST
            return -1  # HTTRANSPARENT
        if message == 0x0021:  # WM_MOUSEACTIVATE
            return 3  # MA_NOACTIVATE
        return self._def_window(hwnd, message, wparam, lparam)

    def _resize_bitmap(self, width: int, height: int) -> None:
        self._release_bitmap()
        self._dc = self._gdi.CreateCompatibleDC(None)
        if not self._dc:
            raise ctypes.WinError(ctypes.get_last_error())
        header = _BitmapInfo(ctypes.sizeof(_BitmapInfo), width, -height, 1, 32)
        self._bitmap = self._gdi.CreateDIBSection(self._dc, ctypes.byref(header), 0,
                                                   ctypes.byref(self._bits), None, 0)
        if not self._bitmap:
            raise ctypes.WinError(ctypes.get_last_error())
        self._previous_bitmap = self._gdi.SelectObject(self._dc, self._bitmap)
        if not self._previous_bitmap:
            raise ctypes.WinError(ctypes.get_last_error())
        self._size = (width, height)

    def _release_bitmap(self) -> None:
        if self._previous_bitmap:
            self._gdi.SelectObject(self._dc, self._previous_bitmap)
            self._previous_bitmap = None
        if self._bitmap:
            self._gdi.DeleteObject(self._bitmap)
            self._bitmap = None
        if self._dc:
            self._gdi.DeleteDC(self._dc)
            self._dc = None

    def poll(self) -> tuple[bool, bool]:
        refresh, quit_requested = False, False
        message = win.MSG()
        while self._user.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
            if message.message == _WM_HOTKEY and message.hWnd == self.hwnd:
                refresh |= message.wParam == _REFRESH
                quit_requested |= message.wParam == _EXIT
            else:
                self._user.TranslateMessage(ctypes.byref(message))
                self._user.DispatchMessageW(ctypes.byref(message))
        return refresh, quit_requested

    def hide(self) -> None:
        if self._visible:
            self._user.ShowWindow(self.hwnd, 0)
            self._visible = False

    def show(self, bounds: tuple[int, int, int, int] | None, label: str, detail: str = "",
             offset: int | None = None, *, distance: float | None = None) -> None:
        if bounds is None:
            self.hide()
        else:
            distance_text = f"距 {distance:.1f}" if distance is not None else ""
            self._show(hud_layout(bounds), render_compass, (label, detail, offset, distance_text))

    def show_marker(self, point: tuple[int, int] | None, heading: float | None = None) -> None:
        if point is None:
            self.hide()
        else:
            size = _TARGET_MARKER_SIZE
            x, y = point
            rotation = round(heading / 5) * 5 % 360 if heading is not None else None
            self._show((x - size // 2, y - size // 2, size, size), render_target_marker, (rotation,))

    def show_timings(self, bounds, sections=()) -> None:
        if bounds is None:
            self.hide()
        else:
            self._show(timing_layout(bounds, sections), render_timings, (sections,))

    def _show(self, rect, render, content) -> None:
        x, y, width, height = rect
        state = (width, height, content)
        if self._last != (x, y, state):
            if self._size != (width, height):
                self._resize_bitmap(width, height)
            if self._last is None or self._last[2] != state:
                frame = render((width, height), *content)
                data = frame.convert("RGBa").tobytes("raw", "BGRa")
                ctypes.memmove(self._bits, data, len(data))
            dest, dimensions, origin = win.POINT(x, y), win.SIZE(width, height), win.POINT()
            blend = _Blend(0, 0, 255, 1)  # AC_SRC_OVER | AC_SRC_ALPHA
            if not self._user.UpdateLayeredWindow(self.hwnd, None, ctypes.byref(dest), ctypes.byref(dimensions),
                                                  self._dc, ctypes.byref(origin), 0, ctypes.byref(blend), 2):
                raise ctypes.WinError(ctypes.get_last_error())
            self._last = (x, y, state)
        if not self._visible:
            self._user.ShowWindow(self.hwnd, 4)  # SW_SHOWNOACTIVATE
            self._visible = True

    def close(self) -> None:
        if self.hwnd:
            for key in self._registered_keys:
                self._user.UnregisterHotKey(self.hwnd, key)
            self._user.DestroyWindow(self.hwnd)
            self.hwnd = None
        if self._class_registered:
            self._user.UnregisterClassW(self._class_name, self._instance)
            self._class_registered = False
        self._release_bitmap()
