"""Read the guide's minimap from a Windows Graphics Capture window session."""

from __future__ import annotations

import ctypes as ct
import math
from pathlib import Path
import re
import sys
import time

import cv2
import numpy as np

import edge_video_shot as edge


WindowRect = tuple[float, float, float, float]
FULL_WINDOW: WindowRect = (0.0, 0.0, 1.0, 1.0)
DEFAULT_WINDOW_TITLE = r"bilibili|哔哩哔哩|B站"
_NATIVE_ROOT = (Path(sys.executable).resolve().parent if getattr(sys, "frozen", False)
                else Path(__file__).resolve().parent / "cpptest")
_DLL = _NATIVE_ROOT / "build-native" / "bgi_window_capture.dll"


def window_video_rect(values) -> WindowRect:
    try:
        x, y, width, height = map(float, values)
    except (TypeError, ValueError):
        raise ValueError("视频画面区域需要四个数字 X Y W H") from None
    if (not all(map(math.isfinite, (x, y, width, height))) or min(x, y) < 0 or min(width, height) <= 0
            or max(x + width, y + height) > 1 + 1e-9):
        raise ValueError("视频画面区域必须在 Edge 窗口的 0 到 1 范围内")
    return x, y, width, height


def minimap_region(video: WindowRect, circle: edge.VideoCircle) -> WindowRect:
    x, y, width, height = video
    cx, cy, half = circle
    if not 0 < half <= min(cx, cy, 640 - cx, 360 - cy):
        raise ValueError("小地图固定取帧区域须在 640×360 画面内")
    return (x + width * (cx - half) / 640, y + height * (cy - half) / 360,
            width * 2 * half / 640, height * 2 * half / 360)


class _FrameView(ct.Structure):
    _fields_ = [("pixels", ct.POINTER(ct.c_uint8)), ("width", ct.c_int), ("height", ct.c_int)]


class _WindowCapture:
    def __init__(self, hwnd: int) -> None:
        if not _DLL.is_file():
            raise OSError(f"找不到 Windows 捕获组件 {_DLL.name}")
        dll = self._dll = ct.CDLL(str(_DLL))
        dll.bgi_capture_open.argtypes = (ct.c_size_t,)
        dll.bgi_capture_open.restype = ct.c_void_p
        dll.bgi_capture_read.argtypes = (ct.c_void_p, ct.c_double, ct.c_double, ct.c_double, ct.c_double,
                                         ct.c_int, ct.POINTER(_FrameView))
        dll.bgi_capture_read.restype = ct.c_int
        dll.bgi_capture_close.argtypes = (ct.c_void_p,)
        dll.bgi_capture_close.restype = None
        dll.bgi_capture_error.restype = ct.c_char_p
        self._handle = dll.bgi_capture_open(hwnd)
        if not self._handle:
            raise self._error()

    def _error(self) -> OSError:
        return OSError(self._dll.bgi_capture_error().decode("utf-8", errors="replace"))

    def close(self) -> None:
        self._dll.bgi_capture_close(self._handle)
        self._handle = None

    def read(self, region: WindowRect, timeout_ms: int) -> np.ndarray | None:
        view = _FrameView()
        result = self._dll.bgi_capture_read(self._handle, *region, timeout_ms, ct.byref(view))
        if result == 1:
            return None
        if result != 0:
            raise self._error()
        return np.ctypeslib.as_array(view.pixels, (view.height, view.width, 4))


class WindowsEdgeVideo:
    """Pin the original Edge window and read new frames without CDP."""

    def __init__(self, *, title_pattern: str | None = None, video_rect: WindowRect = FULL_WINDOW) -> None:
        self._default_title = title_pattern in (None, "", r"bilibili\.com")
        pattern = DEFAULT_WINDOW_TITLE if self._default_title else title_pattern
        self._title = re.compile(pattern, re.I)
        self._video_rect = window_video_rect(video_rect)
        self._window: edge.EdgeWindow | None = None
        self._source: _WindowCapture | None = None

    def __enter__(self) -> WindowsEdgeVideo:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def close(self) -> None:
        if self._source is not None:
            self._source.close()
            self._source = None

    def _find_window(self) -> edge.EdgeWindow:
        opened = edge.list_edge_windows(include_minimized=True)
        windows = [window for window in opened if self._title.search(window.title)]
        if not windows and self._default_title:
            # Tab groups can replace the video title with the group's name.
            main = [window for window in opened
                    if window.title.replace("\u200b", "").lower().endswith(" - microsoft edge")]
            if len(main) == 1:
                windows = main
        if not windows:
            raise edge.VideoNotFound(f"没有 Edge 窗口标题匹配 {self._title.pattern!r}",
                                     status="Windows 捕获等待 Edge 攻略窗口")
        foreground = edge._user32.GetForegroundWindow()
        return max(windows, key=lambda window: (not edge._user32.IsIconic(window.hwnd),
                                                 window.hwnd == foreground, window.width * window.height))

    def _read(self, region: WindowRect, timeout_ms: int) -> np.ndarray:
        if not edge._HAS_WIN32:
            raise edge.VideoNotFound("当前系统无法使用 Windows 窗口捕获", status="Windows 捕获不可用")
        user32 = edge._user32
        if self._window is None or not user32.IsWindow(self._window.hwnd):
            self.close()
            self._window = self._find_window()
        hwnd = self._window.hwnd
        if user32.IsIconic(hwnd):
            self.close()
            raise edge.VideoNotFound("请还原 Edge 攻略窗口", status="Windows 捕获等待 Edge 退出最小化")
        try:
            if self._source is None:
                self._source = _WindowCapture(hwnd)
            frame = self._source.read(region, timeout_ms)
        except OSError as error:
            self.close()
            raise edge.VideoNotFound(f"Windows 捕获失败：{error}", status="Windows 捕获失败，见日志") from error
        if frame is None:
            raise edge.VideoNotFound("请让攻略继续播放，必要时露出一部分 Edge 窗口",
                                     status="Windows 捕获未收到新帧")
        return frame

    def snapshot(self) -> np.ndarray:
        """Read one complete window image for calibrating the video rectangle."""
        return cv2.cvtColor(self._read(FULL_WINDOW, 1400), cv2.COLOR_BGRA2RGB)

    def capture(self, circle: edge.VideoCircle) -> edge.VideoArrayShot:
        started = time.perf_counter()
        bgra = self._read(minimap_region(self._video_rect, circle), 350)
        received = time.perf_counter()
        frame = cv2.cvtColor(bgra, cv2.COLOR_BGRA2BGR)
        size = max(212, min(frame.shape[:2]))
        if frame.shape[:2] != (size, size):
            frame = cv2.resize(frame, (size, size), interpolation=cv2.INTER_LINEAR)
        stats = (0, 0, (received - started) * 1000, (time.perf_counter() - received) * 1000, bgra.nbytes / 1024)
        return edge.VideoArrayShot(frame, {"size": [size, size], "capture_stats": stats}, via="windows-region")
