# -*- coding: utf-8 -*-
"""从正在运行的 Microsoft Edge 抓取当前网页里 <video> 的画面。

两条路线，按精度从高到低：

1) CDP（Chrome DevTools Protocol）—— 精确，只截视频元素那一块
   Edge 必须以 --remote-debugging-port=<port> 启动。脚本从 http://127.0.0.1:<port>/json/list
   找到页面 target，用 Runtime.evaluate 读 <video> 的 getBoundingClientRect()，
   再用 Page.captureScreenshot(clip=...) 只截视频区域（不经过 canvas，所以不受跨域污染影响）。
   不受窗口遮挡、屏幕缩放、浏览器界面干扰；还能静音播放、seek 到指定秒数再截。

   复用现有 Edge 时，通过 edge://inspect/#remote-debugging 手动启用调试，
   再用 ExistingEdgeVideo 读取原窗口里的视频。launch_edge_debug() 用于独立测试配置。

2) Win32 窗口截图 —— 零配置，但截的是整个浏览器窗口
   PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT) 取窗口位图（窗口被遮挡也能取到），
   可用 crop=(x, y, w, h) 只保留视频所在区域。
   缺点：视频若走硬件覆盖层 / 窗口最小化时，视频区域会截成纯黑（实测 B 站页会这样），
   这种情况只能用 CDP 路线。

依赖: pip install pillow websockets   （pytest 用例见 test_edge_video_shot.py）

后台能力（不需要窗口在前台，也不抢焦点）:
    - headless=True（--headless=new）：完全不开窗口，实测 0.2~0.6s 一张，最省事。
    - 有头但窗口最小化：mode="canvas" 可靠（0.8s）；mode="clip" 要等合成器，首张 6~30s，
      偶发超时（已自动回退到 captureBeyondViewport 离屏渲染）。
    - 标签页在后台（别的标签在前台）：能截，但视频可能被 Chromium 节流成冻结帧，
      想拿指定时刻就配 at_time 先 seek。
    - 只有 capture_window() 要求窗口可见（最小化截不到），后台场景别用它。

可移植性（复制这一个 .py 就能在别的机器跑，不需要 dsh）:
    - 只用标准库 + Pillow + websockets；窗口截图是纯 ctypes 实现，不依赖 pywin32。
    - CDP 路线跨平台（Windows/macOS/Linux 都能连 Edge/Chrome 的调试端口）。
    - 窗口截图只支持 Windows（非 Windows 上会抛 EdgeShotError）。
    - Edge 可执行文件路径可用 EDGE_EXE 环境变量覆盖。
    - 实测 Python 3.11；注解全部字符串化（from __future__ import annotations），没用到 3.10 语法。

命令行:
    python edge_video_shot.py --mode window --out shot.png
    python edge_video_shot.py --mode video --port 9222 --play --at 60 --out frame.png
    python edge_video_shot.py --mode video --launch <URL> --headless --play --at 90 --frame-mode canvas --out frame.png
    python edge_video_shot.py --mode page --full --port 9222 --out page.png
"""

from __future__ import annotations

import argparse
import base64
import ctypes
import io
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from ctypes import wintypes as wt
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, Iterable, NoReturn, Optional, Sequence
from urllib.parse import urlsplit

from PIL import Image

if TYPE_CHECKING:
    from numpy import ndarray

VideoCircle = tuple[float, float, float]  # center and inner radius on a 640px-wide frame

DEFAULT_PORT = 9222
DEFAULT_PROFILE_DIR = "edge-video-shot-profile"

EDGE_EXE_PATHS = (
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files (x86)\Microsoft\Edge Beta\Application\msedge.exe",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
)
EDGE_EXE_NAMES = ("msedge", "microsoft-edge", "microsoft-edge-stable", "microsoft-edge-beta")


class EdgeShotError(RuntimeError):
    """截图链路上的可预期失败（找不到窗口 / 端口没开 / 页面没有 video 等）。"""


class VideoNotFound(EdgeShotError):
    """当前没有可用的攻略画面。"""

    def __init__(self, message: str, *, status: str = "等待 Edge 攻略视频出画面") -> None:
        super().__init__(message)
        self.status = status


class ExistingEdgeUnavailable(EdgeShotError):
    """现有 Edge 尚未开放或授权本地连接。"""


class _CanvasReadBlocked(EdgeShotError):
    """The video cannot be read through a canvas under its origin policy."""


@dataclass
class VideoShot:
    """一次截图的结果。"""

    image: Image.Image
    info: dict[str, Any]
    via: str                     # "cdp-clip" | "cdp-canvas" | "cdp-page" | "window"
    png: bytes

    def save(self, path: str | os.PathLike[str]) -> Path:
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(self.png)
        return p


@dataclass
class VideoArrayShot:
    bgr: ndarray
    info: dict[str, Any]
    via: str = "cdp-canvas-fast"


@dataclass(frozen=True)
class EdgeWindow:
    hwnd: int
    pid: int
    title: str
    rect: tuple[int, int, int, int]   # 物理像素 left, top, right, bottom

    @property
    def width(self) -> int:
        return self.rect[2] - self.rect[0]

    @property
    def height(self) -> int:
        return self.rect[3] - self.rect[1]

    def __str__(self) -> str:  # pragma: no cover - 仅用于日志
        return f"hwnd={self.hwnd} pid={self.pid} {self.width}x{self.height} {self.title!r}"


# --------------------------------------------------------------------------- #
# Win32：找 Edge 窗口 + PrintWindow 截图（纯 ctypes，不依赖 pywin32）
# --------------------------------------------------------------------------- #

_IS_WINDOWS = sys.platform == "win32"
_HAS_WIN32 = False

if _IS_WINDOWS:  # pragma: no branch - 非 Windows 上只保留 CDP 部分
    try:
        _user32 = ctypes.WinDLL("user32", use_last_error=True)
        _gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        _dwmapi = ctypes.WinDLL("dwmapi", use_last_error=True)

        _WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

        _user32.EnumWindows.argtypes = (_WNDENUMPROC, wt.LPARAM)
        _user32.EnumWindows.restype = wt.BOOL
        _user32.IsWindow.argtypes = (wt.HWND,)
        _user32.IsWindow.restype = wt.BOOL
        _user32.IsWindowVisible.argtypes = (wt.HWND,)
        _user32.IsWindowVisible.restype = wt.BOOL
        _user32.IsIconic.argtypes = (wt.HWND,)
        _user32.IsIconic.restype = wt.BOOL
        _user32.GetForegroundWindow.restype = wt.HWND
        _user32.GetClassNameW.argtypes = (wt.HWND, wt.LPWSTR, ctypes.c_int)
        _user32.GetWindowTextW.argtypes = (wt.HWND, wt.LPWSTR, ctypes.c_int)
        _user32.GetWindowThreadProcessId.argtypes = (wt.HWND, ctypes.POINTER(wt.DWORD))
        _user32.GetWindowRect.argtypes = (wt.HWND, ctypes.POINTER(wt.RECT))
        _user32.PrintWindow.argtypes = (wt.HWND, wt.HDC, wt.UINT)
        _user32.PrintWindow.restype = wt.BOOL
        _user32.GetDC.argtypes = (wt.HWND,)
        _user32.GetDC.restype = wt.HDC
        _user32.ReleaseDC.argtypes = (wt.HWND, wt.HDC)
        _user32.PostMessageW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

        _gdi32.CreateCompatibleDC.argtypes = (wt.HDC,)
        _gdi32.CreateCompatibleDC.restype = wt.HDC
        _gdi32.CreateCompatibleBitmap.argtypes = (wt.HDC, ctypes.c_int, ctypes.c_int)
        _gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
        _gdi32.SelectObject.argtypes = (wt.HDC, wt.HGDIOBJ)
        _gdi32.SelectObject.restype = wt.HGDIOBJ
        _gdi32.BitBlt.argtypes = (
            wt.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            wt.HDC, ctypes.c_int, ctypes.c_int, wt.DWORD,
        )
        _gdi32.BitBlt.restype = wt.BOOL
        _gdi32.GetDIBits.argtypes = (
            wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT,
        )
        _gdi32.GetDIBits.restype = ctypes.c_int
        _gdi32.DeleteObject.argtypes = (wt.HGDIOBJ,)
        _gdi32.DeleteDC.argtypes = (wt.HDC,)

        _kernel32.OpenProcess.argtypes = (wt.DWORD, wt.BOOL, wt.DWORD)
        _kernel32.OpenProcess.restype = wt.HANDLE
        _kernel32.QueryFullProcessImageNameW.argtypes = (
            wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD),
        )
        _kernel32.CloseHandle.argtypes = (wt.HANDLE,)

        _dwmapi.DwmGetWindowAttribute.argtypes = (wt.HWND, wt.DWORD, ctypes.c_void_p, wt.DWORD)
        _dwmapi.DwmGetWindowAttribute.restype = ctypes.c_long

        _HAS_WIN32 = True
    except Exception:  # pragma: no cover - 理论上只会在非 Windows 触发
        _HAS_WIN32 = False

_PW_RENDERFULLCONTENT = 0x00000002
_SRCCOPY = 0x00CC0020
_WM_CLOSE = 0x0010
_DWMWA_EXTENDED_FRAME_BOUNDS = 9
_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


class _BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wt.DWORD),
        ("biWidth", wt.LONG),
        ("biHeight", wt.LONG),
        ("biPlanes", wt.WORD),
        ("biBitCount", wt.WORD),
        ("biCompression", wt.DWORD),
        ("biSizeImage", wt.DWORD),
        ("biXPelsPerMeter", wt.LONG),
        ("biYPelsPerMeter", wt.LONG),
        ("biClrUsed", wt.DWORD),
        ("biClrImportant", wt.DWORD),
    ]


_DPI_READY = False


def enable_dpi_awareness() -> None:
    """让窗口坐标和截图都按物理像素算，避免 125%/150% 缩放下裁错位置。"""
    global _DPI_READY
    if _DPI_READY or not _HAS_WIN32:
        return
    _DPI_READY = True
    try:
        ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)  # PER_MONITOR_DPI_AWARE
    except Exception:
        try:
            _user32.SetProcessDPIAware()
        except Exception:
            pass


def _process_image_name(pid: int) -> str:
    handle = _kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(1024)
        size = wt.DWORD(len(buf))
        if _kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return buf.value
        return ""
    finally:
        _kernel32.CloseHandle(handle)


def _window_frame(hwnd: int) -> tuple[int, int, int, int]:
    """优先用 DWM 扩展边框：Windows 10+ 的 GetWindowRect 会把不可见的阴影算进去。"""
    rect = wt.RECT()
    try:
        if _dwmapi.DwmGetWindowAttribute(
            hwnd, _DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(rect), ctypes.sizeof(rect)
        ) == 0:
            return rect.left, rect.top, rect.right, rect.bottom
    except Exception:
        pass
    _user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect.left, rect.top, rect.right, rect.bottom


def list_edge_windows(*, include_minimized: bool = False) -> list[EdgeWindow]:
    """列出所有可见的 Edge 顶层窗口（Chrome_WidgetWin_1 且进程是 msedge.exe）。"""
    if not _HAS_WIN32:
        return []
    enable_dpi_awareness()
    found: list[EdgeWindow] = []

    @_WNDENUMPROC
    def _cb(hwnd, _lparam):  # noqa: ANN001 - ctypes 回调
        try:
            if not _user32.IsWindowVisible(hwnd):
                return True
            cls = ctypes.create_unicode_buffer(256)
            _user32.GetClassNameW(hwnd, cls, 256)
            if cls.value != "Chrome_WidgetWin_1":
                return True
            pid = wt.DWORD()
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            exe = _process_image_name(pid.value)
            if os.path.basename(exe).lower() != "msedge.exe":
                return True
            title = ctypes.create_unicode_buffer(512)
            _user32.GetWindowTextW(hwnd, title, 512)
            rect = _window_frame(hwnd)
            if ((rect[2] <= rect[0] or rect[3] <= rect[1])
                    and not (include_minimized and _user32.IsIconic(hwnd))):
                return True
            found.append(EdgeWindow(int(hwnd), int(pid.value), title.value, rect))
        except Exception:
            pass
        return True

    _user32.EnumWindows(_cb, 0)
    return found


def pick_edge_window(title_pattern: Optional[str] = None) -> EdgeWindow:
    """挑一个 Edge 窗口：有标题、面积最大的那个；title_pattern 可指定标题/正则。"""
    windows = list_edge_windows()
    if not windows:
        raise EdgeShotError("没有找到可见的 Microsoft Edge 窗口")
    if title_pattern:
        rx = re.compile(title_pattern, re.I)
        windows = [w for w in windows if rx.search(w.title)]
        if not windows:
            raise EdgeShotError(f"没有标题匹配 {title_pattern!r} 的 Edge 窗口")
    titled = [w for w in windows if w.title.strip()]
    pool = titled or windows
    return max(pool, key=lambda w: w.width * w.height)


def _read_bitmap(dc: int, bitmap: int, width: int, height: int) -> Image.Image:
    header = _BITMAPINFOHEADER()
    header.biSize = ctypes.sizeof(_BITMAPINFOHEADER)
    header.biWidth = width
    header.biHeight = -height        # 负高度 = 自上而下
    header.biPlanes = 1
    header.biBitCount = 32
    header.biCompression = 0         # BI_RGB
    buffer = ctypes.create_string_buffer(width * height * 4)
    lines = _gdi32.GetDIBits(dc, bitmap, 0, height, buffer, ctypes.byref(header), 0)
    if lines != height:
        raise EdgeShotError(f"GetDIBits 只取到 {lines}/{height} 行")
    return Image.frombuffer("RGBA", (width, height), buffer, "raw", "BGRA", 0, 1).convert("RGB")


def _is_blank(image: Image.Image) -> bool:
    return image.convert("L").getextrema() == (0, 0)


def capture_window(
    hwnd: Optional[int] = None,
    *,
    title_pattern: Optional[str] = None,
    crop: Optional[Sequence[int]] = None,
) -> VideoShot:
    """截 Edge 窗口。

    PrintWindow 走的是合成器快照，窗口被别的窗口挡住也能截到内容；
    最小化窗口取不到画面（返回全黑），此时会尝试屏幕 BitBlt。
    crop=(x, y, w, h) 按截图像素裁掉浏览器界面，只留视频区域。
    """
    if not _HAS_WIN32:
        raise EdgeShotError("窗口截图只在 Windows 上可用")
    enable_dpi_awareness()

    window = next((w for w in list_edge_windows() if w.hwnd == hwnd), None) if hwnd else None
    if window is None:
        window = pick_edge_window(title_pattern)
    hwnd = window.hwnd
    left, top, right, bottom = _window_frame(hwnd)
    width, height = right - left, bottom - top
    if width <= 0 or height <= 0:
        raise EdgeShotError(f"窗口尺寸无效: {width}x{height}")

    screen_dc = _user32.GetDC(None)
    if not screen_dc:
        raise EdgeShotError("GetDC(NULL) 失败")
    mem_dc = _gdi32.CreateCompatibleDC(screen_dc)
    bitmap = _gdi32.CreateCompatibleBitmap(screen_dc, width, height)
    old = _gdi32.SelectObject(mem_dc, bitmap)
    try:
        printed = _user32.PrintWindow(hwnd, mem_dc, _PW_RENDERFULLCONTENT)
        image = _read_bitmap(mem_dc, bitmap, width, height)
        if not printed or _is_blank(image):
            _gdi32.BitBlt(mem_dc, 0, 0, width, height, screen_dc, left, top, _SRCCOPY)
            fallback = _read_bitmap(mem_dc, bitmap, width, height)
            if not _is_blank(fallback) or _is_blank(image):
                image = fallback
    finally:
        _gdi32.SelectObject(mem_dc, old)
        _gdi32.DeleteObject(bitmap)
        _gdi32.DeleteDC(mem_dc)
        _user32.ReleaseDC(None, screen_dc)

    info: dict[str, Any] = {
        "window": str(window),
        "hwnd": hwnd,
        "title": window.title,
        "rect": list(window.rect),
        "size": list(image.size),
    }
    if crop:
        x, y, w, h = (int(v) for v in crop)
        box = (x, y, x + w, y + h)
        image = image.crop(box)
        info["crop"] = list(crop)
        info["size"] = list(image.size)

    buffer = io.BytesIO()
    image.save(buffer, format="PNG")
    return VideoShot(image=image, info=info, via="window", png=buffer.getvalue())


# --------------------------------------------------------------------------- #
# CDP：找到 <video> 并只截它那一块
# --------------------------------------------------------------------------- #

_JS_PROBE_VIDEO = r"""
(() => {
  const all = Array.from(document.querySelectorAll('video'));
  const usable = all.filter(v => v.readyState >= 2 && v.videoWidth > 0 && v.videoHeight > 0);
  const visible = usable.filter(v => {
    const r = v.getBoundingClientRect();
    const cs = getComputedStyle(v);
    return r.width > 2 && r.height > 2 &&
           cs.visibility !== 'hidden' && cs.display !== 'none' && parseFloat(cs.opacity) > 0.01;
  });
  const v = visible.find(x => !x.paused && !x.ended) || visible[0] || usable[0] || null;
  if (!v) {
    return {
      found: false,
      total: all.length,
      ready: usable.length,
      docVisibility: document.visibilityState,
      docFocused: document.hasFocus(),
      hint: all.length ? 'video 还没出画面 (readyState<2 或 videoWidth=0)' : '页面上没有 <video> 元素',
    };
  }
  const r = v.getBoundingClientRect();
  const cs = getComputedStyle(v);
  return {
    found: true,
    total: all.length,
    ready: usable.length,
    videoIndex: all.indexOf(v),
    src: v.currentSrc || v.src || '',
    currentTime: v.currentTime,
    duration: Number.isFinite(v.duration) ? v.duration : null,
    paused: v.paused,
    ended: v.ended,
    muted: v.muted,
    playbackRate: v.playbackRate,
    videoWidth: v.videoWidth,
    videoHeight: v.videoHeight,
    rect: { x: r.x, y: r.y, width: r.width, height: r.height },
    inViewport: r.bottom > 0 && r.right > 0 && r.top < innerHeight && r.left < innerWidth,
    objectFit: cs.objectFit,
    docVisibility: document.visibilityState,
    docFocused: document.hasFocus(),
    viewport: { width: innerWidth, height: innerHeight, dpr: devicePixelRatio, scrollX, scrollY },
  };
})()
"""

_JS_CANVAS_FRAME = r"""
(() => {
  const all = Array.from(document.querySelectorAll('video'));
  const usable = all.filter(v => v.readyState >= 2 && v.videoWidth > 0);
  const v = usable.find(x => !x.paused && !x.ended) || usable[0];
  if (!v) return { ok: false, reason: '没有可截图的 <video>' };
  const c = document.createElement('canvas');
  c.width = v.videoWidth;
  c.height = v.videoHeight;
  const ctx = c.getContext('2d');
  ctx.drawImage(v, 0, 0, c.width, c.height);
  try {
    return { ok: true, dataUrl: c.toDataURL('image/png') };
  } catch (e) {
    return { ok: false, reason: (e && e.name) + ': 跨域视频把 canvas 标记为污染，改用 mode="clip"' };
  }
})()
"""


_JS_FAST_CANVAS_FRAME = r"""
((circle, videoIndex) => {
  if (!['www.bilibili.com', 'bilibili.com'].includes(location.hostname)) return null;
  const started = performance.now();
  const videos = Array.from(document.querySelectorAll('video'));
  const ready = videos.filter(v => v.readyState >= 2 && v.videoWidth > 0 && v.videoHeight > 0);
  const video = videoIndex === undefined
    ? ready.find(v => !v.paused && !v.ended) || ready[0]
    : videos[videoIndex];
  if (!ready.includes(video)) return {found: false, total: videos.length};

  let x = 0, y = 0, width = video.videoWidth, height = video.videoHeight;
  let sample = null;
  if (circle) {
    const scale = width / 640;
    const [cx, cy, radius] = circle.map(value => value * scale);
    const half = Math.round(radius);
    x = Math.floor(cx) - half;
    y = Math.floor(cy) - half;
    width = height = 2 * half + 2;
    if (half < 1 || x < 0 || y < 0 || x + width > video.videoWidth || y + height > video.videoHeight) {
      return {found: true, invalidCircle: true};
    }
    sample = [cx - x, cy - y, half];
  }
  const key = Symbol.for('BetterGI.VideoGuide.canvas');
  let canvas = globalThis[key];
  if (!canvas) canvas = globalThis[key] = document.createElement('canvas');
  if (canvas.width !== width) canvas.width = width;
  if (canvas.height !== height) canvas.height = height;
  const currentTime = video.currentTime;
  try {
    canvas.getContext('2d', {alpha: false}).drawImage(video, x, y, width, height, 0, 0, width, height);
    const drawn = performance.now();
    const dataUrl = circle ? canvas.toDataURL('image/png') : canvas.toDataURL('image/jpeg', 0.98);
    return {found: true, dataUrl, currentTime, sample, drawMs: drawn - started,
            encodeMs: performance.now() - drawn};
  } catch (error) {
    // A tainted canvas stays unreadable until its backing store is reset.
    delete globalThis[key];
    const blocked = error.name === 'SecurityError';
    return {found: true, blocked, reason: blocked ? '跨域视频限制 canvas 取帧' : String(error)};
  }
})
"""


def _http_json(port: int, path: str, timeout: float = 5.0) -> Any:
    url = f"http://127.0.0.1:{int(port)}{path}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.URLError as exc:
        raise EdgeShotError(f"连不上 Edge 调试端口 {port}（{url}）: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise EdgeShotError(f"调试端口 {port} 返回的不是 JSON: {exc}") from exc


def debug_port_open(port: int = DEFAULT_PORT, timeout: float = 1.5) -> bool:
    try:
        _http_json(port, "/json/version", timeout=timeout)
        return True
    except EdgeShotError:
        return False


def wait_debug_port(port: int = DEFAULT_PORT, timeout: float = 25.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if debug_port_open(port, timeout=1.0):
            return True
        time.sleep(0.3)
    return False


def list_page_targets(port: int = DEFAULT_PORT, timeout: float = 5.0) -> list[dict[str, Any]]:
    """列出可截图的标签页（去掉 devtools:// 这类内部页面）。"""
    targets = _http_json(port, "/json/list", timeout=timeout)
    pages = []
    for target in targets or []:
        if target.get("type") != "page":
            continue
        url = target.get("url") or ""
        if url.startswith("devtools://") or (url.startswith("edge://") and "newtab" in url):
            continue
        pages.append(target)
    return pages


class CDPSession:
    """极简 CDP 客户端：一个 websocket，一个 call()，忽略事件。"""

    def __init__(self, ws_url: str, timeout: float = 15.0) -> None:
        try:
            from websockets.sync.client import connect
        except ImportError as exc:  # pragma: no cover
            raise EdgeShotError("需要 websockets: pip install websockets") from exc
        self._ws = connect(ws_url, max_size=None, open_timeout=timeout)
        self._id = 0
        self._timeout = timeout

    def close(self) -> None:
        try:
            self._ws.close()
        except Exception:
            pass

    def __enter__(self) -> "CDPSession":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def call(self, method: str, *, timeout: Optional[float] = None,
             _session_id: Optional[str] = None, **params: Any) -> dict[str, Any]:
        wait = self._timeout if timeout is None else timeout
        self._id += 1
        message_id = self._id
        payload = {"id": message_id, "method": method, "params": params}
        if _session_id is not None:
            payload["sessionId"] = _session_id
        self._ws.send(json.dumps(payload))
        while True:
            try:
                raw = self._ws.recv(timeout=wait)
            except TypeError:  # 老版本 websockets 的 recv 没有 timeout 参数
                raw = self._ws.recv()
            except TimeoutError as exc:
                raise EdgeShotError(f"{method} 超时（{wait}s）") from exc
            if isinstance(raw, (bytes, bytearray)):
                raw = bytes(raw).decode("utf-8", "replace")
            message = json.loads(raw)
            if message.get("id") != message_id:
                continue                     # 事件（Runtime.consoleAPICalled 之类）直接丢掉
            error = message.get("error")
            if error:
                raise EdgeShotError(f"{method} 失败: {error.get('message')} (code={error.get('code')})")
            return message.get("result") or {}

    def evaluate(self, expression: str, *, await_promise: bool = False,
                 _session_id: Optional[str] = None) -> Any:
        result = self.call(
            "Runtime.evaluate",
            _session_id=_session_id,
            expression=expression,
            returnByValue=True,
            awaitPromise=await_promise,
            userGesture=True,
        )
        details = result.get("exceptionDetails")
        if details:
            text = (details.get("exception") or {}).get("description") or details.get("text")
            raise EdgeShotError(f"页面脚本报错: {text}")
        return (result.get("result") or {}).get("value")


class _AttachedPage:
    def __init__(self, browser: CDPSession, session_id: str) -> None:
        self.browser = browser
        self.session_id = session_id

    def call(self, method: str, **params: Any) -> dict[str, Any]:
        return self.browser.call(method, _session_id=self.session_id, **params)

    def evaluate(self, expression: str, *, await_promise: bool = False) -> Any:
        return self.browser.evaluate(expression, await_promise=await_promise, _session_id=self.session_id)


def _probe(session: CDPSession | _AttachedPage) -> dict[str, Any]:
    return session.evaluate(_JS_PROBE_VIDEO) or {"found": False, "hint": "Runtime.evaluate 返回空"}


def _pick_video_session(
    port: int,
    url_pattern: Optional[str],
    timeout: float,
) -> tuple[CDPSession, dict[str, Any], dict[str, Any]]:
    pages = list_page_targets(port, timeout=timeout)
    if url_pattern:
        rx = re.compile(url_pattern, re.I)
        pages = [p for p in pages if rx.search(f"{p.get('title', '')} {p.get('url', '')}")]
    if not pages:
        raise VideoNotFound("调试端口上没有匹配的标签页" + (f"（url_pattern={url_pattern!r}）" if url_pattern else ""))

    notes: list[str] = []
    for page in pages:
        ws_url = page.get("webSocketDebuggerUrl")
        if not ws_url:
            continue
        session = CDPSession(ws_url, timeout=timeout)
        try:
            info = _probe(session)
        except Exception as exc:
            session.close()
            notes.append(f"{page.get('title', '')[:40]!r}: {exc}")
            continue
        if info.get("found"):
            return session, page, info
        session.close()
        notes.append(f"{page.get('title', '')[:40]!r}: {info.get('hint') or '没有 video'}")

    raise VideoNotFound("没有找到可截图的 <video>；逐个标签页的结果: " + "; ".join(notes))


_JS_SEEK = r"""
(async (target) => {
  const all = Array.from(document.querySelectorAll('video'));
  const usable = all.filter(v => v.readyState >= 1 && v.videoWidth > 0);
  const v = usable.find(x => !x.paused && !x.ended) || usable[0] || all[0];
  if (!v) return { ok: false, reason: '没有 <video>' };
  v.pause();
  if (Math.abs(v.currentTime - target) > 0.05) {
    await new Promise((resolve) => {
      const done = () => { v.removeEventListener('seeked', done); resolve(); };
      v.addEventListener('seeked', done);
      try { v.currentTime = target; } catch (e) { /* 直播流之类不可 seek */ }
      setTimeout(done, 3000);
    });
  }
  // 用 setTimeout 而不是 requestAnimationFrame：标签页在后台时 rAF 会被节流甚至不跑
  await new Promise((r) => setTimeout(r, 120));
  return { ok: true, currentTime: v.currentTime, duration: v.duration, paused: v.paused };
})(%s)
"""


_JS_PLAY = r"""
(async () => {
  const v = document.querySelector('video');
  if (!v) return { ok: false, reason: '没有 <video>' };
  if (MUTE) v.muted = true;
  const raced = await Promise.race([
    v.play().then(() => 'ok').catch((e) => 'err:' + (e && e.name)),
    new Promise((res) => setTimeout(() => res('timeout'), 8000)),
  ]);
  return {
    ok: raced === 'ok',
    result: raced,
    readyState: v.readyState,
    videoWidth: v.videoWidth,
    videoHeight: v.videoHeight,
    currentTime: v.currentTime,
    paused: v.paused,
  };
})()
"""


def _take_video_frame(session: CDPSession | _AttachedPage, page: dict[str, Any], info: dict[str, Any], *,
                      mode: str, scale: Optional[float] = None, trim_letterbox: bool = True,
                      at_time: Optional[float] = None, play: bool = False, mute: bool = True,
                      screenshot_timeout: float = 45.0) -> VideoShot:
    info = dict(info)
    info["page"] = {"title": page.get("title"), "url": page.get("url"), "id": page.get("id")}
    if play:
        played = session.evaluate(_JS_PLAY.replace("MUTE", "true" if mute else "false"), await_promise=True) or {}
        info["play"] = played
        if not played.get("ok"):
            raise EdgeShotError(f"让视频播放失败: {played.get('result') or played.get('reason')}")
        time.sleep(0.5)
    if at_time is not None:
        info["seeked"] = _seek_video(session, at_time)
    if mode == "canvas":
        return _canvas_shot(session, info)
    return _clip_shot(session, info, scale, trim_letterbox, screenshot_timeout)


def _seek_video(session: CDPSession | _AttachedPage, at_time: float) -> dict[str, Any]:
    seeked = session.evaluate(_JS_SEEK % float(at_time), await_promise=True) or {}
    if not seeked.get("ok"):
        raise EdgeShotError(f"seek 到 {at_time}s 失败: {seeked.get('reason')}")
    return seeked


class ExistingEdgeVideo:
    """复用用户在 edge://inspect 显式开放的原 Edge 会话。"""

    def __init__(self, user_data_dir: Optional[Path] = None, *,
                 pin_tab: bool = False, debug_port: int | None = None) -> None:
        local = os.environ.get("LOCALAPPDATA")
        default = Path(local) / "Microsoft" / "Edge" / "User Data" if local else None
        self._data_dir = user_data_dir if user_data_dir is not None else default
        self._browser: Optional[CDPSession] = None
        self._pages: dict[str, _AttachedPage] = {}
        self._target_id: Optional[str] = None
        self._pin_tab = pin_tab
        self._pinned: tuple[dict[str, Any], _AttachedPage] | None = None
        self._debug_port = debug_port
        self._retry_at = 0.0

    def __enter__(self) -> ExistingEdgeVideo:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def close(self) -> None:
        if self._browser is not None:
            self._browser.close()
        self._browser = None
        self._pages.clear()
        self._target_id = None
        self._pinned = None

    def _endpoint(self) -> str:
        if self._debug_port is not None:
            info = _http_json(self._debug_port, "/json/version", timeout=1.5)
            endpoint = urlsplit(info.get("webSocketDebuggerUrl") or "")
            if (endpoint.scheme == "ws" and endpoint.hostname in ("127.0.0.1", "localhost")
                    and endpoint.port == self._debug_port and endpoint.path.startswith("/devtools/browser/")):
                return endpoint.geturl()
            raise EdgeShotError(f"Edge 调试端口 {self._debug_port} 未提供本地浏览器连接")
        if self._data_dir is not None:
            try:
                lines = (self._data_dir / "DevToolsActivePort").read_text(encoding="utf-8").splitlines()
                port, path = int(lines[0]), lines[1].strip()
                if 1 <= port <= 65535 and re.fullmatch(r"/devtools/browser(?:/[a-zA-Z0-9_-]+)?", path):
                    return f"ws://127.0.0.1:{port}{path}"
            except (OSError, ValueError, IndexError):
                pass
        raise ExistingEdgeUnavailable("请在原 Edge 的 edge://inspect/#remote-debugging 开启远程调试")

    def _attach(self, browser: CDPSession, target_id: str) -> _AttachedPage:
        if target_id not in self._pages:
            attachment = browser.call("Target.attachToTarget", targetId=target_id, flatten=True)
            self._pages[target_id] = _AttachedPage(browser, attachment["sessionId"])
        return self._pages[target_id]

    def _detach(self, browser: CDPSession, target_id: str) -> None:
        page = self._pages.pop(target_id, None)
        if page is not None:
            try:
                browser.call("Target.detachFromTarget", sessionId=page.session_id)
            except EdgeShotError:
                pass

    @staticmethod
    def _capture_page(target: dict[str, Any], page: _AttachedPage, info: dict[str, Any],
                      mode: str, at_time: Optional[float], circle: VideoCircle | None) -> VideoShot | VideoArrayShot:
        if info.get("found"):
            if mode in ("canvas-fast", "canvas-region"):
                return _fast_canvas_shot(page, circle, at_time)
            if mode == "window-region":
                return _window_region_shot(target["targetId"], page, info, circle, at_time)
            return _take_video_frame(page, {**target, "id": target["targetId"]}, info, mode=mode, at_time=at_time)
        ExistingEdgeVideo._video_not_ready(info)

    @staticmethod
    def _video_not_ready(info: dict[str, Any]) -> NoReturn:
        status = "攻略播放器还没解码出画面" if info.get("total") else "当前攻略标签页没有视频播放器"
        hint = info.get("hint")
        raise VideoNotFound(f"{status}（{hint}）" if hint else status, status=status)

    def capture(self, *, url_pattern: Optional[str] = None, mode: str = "canvas",
                at_time: Optional[float] = None, circle: VideoCircle | None = None) -> VideoShot | VideoArrayShot:
        from websockets.exceptions import WebSocketException

        if mode not in ("canvas", "clip", "canvas-fast", "canvas-region", "window-region"):
            raise ValueError(f"不支持的 Edge 取帧方式 {mode}")
        if mode in ("canvas-region", "window-region") and circle is None:
            raise ValueError("固定区域取帧需要提供小地图圆")
        if mode not in ("canvas-region", "window-region"):
            circle = None
        if mode == "canvas-fast" and at_time is not None:
            mode = "canvas"
        if self._browser is None and time.monotonic() < self._retry_at:
            raise ExistingEdgeUnavailable("等待原 Edge 允许本地连接")
        try:
            if self._browser is None:
                self._browser = CDPSession(self._endpoint(), timeout=30.0)
            browser = self._browser
            if self._pinned is not None:
                target, page = self._pinned
                if mode in ("canvas-fast", "canvas-region"):
                    return _fast_canvas_shot(page, circle, at_time)
                info = page.evaluate(
                    "['www.bilibili.com', 'bilibili.com'].includes(location.hostname) && " + _JS_PROBE_VIDEO
                )
                if not info:
                    raise VideoNotFound("原攻略标签页没有 B 站播放器", status="原攻略标签页没有 B 站视频")
                return self._capture_page(target, page, info, mode, at_time, circle)
            result = browser.call("Target.getTargets")
            pages = []
            for target in result.get("targetInfos", []):
                url = urlsplit(target.get("url", ""))
                if target.get("type") != "page" or url.hostname not in ("www.bilibili.com", "bilibili.com"):
                    continue
                if url_pattern and not re.search(url_pattern, f"{target.get('title', '')} {url.geturl()}", re.I):
                    continue
                pages.append(target)
            active_ids = {target["targetId"] for target in pages}
            for gone in self._pages.keys() - active_ids:
                self._detach(browser, gone)
            if not pages:
                raise VideoNotFound("原 Edge 没有符合筛选条件的 B 站标签页", status="未找到 B 站攻略标签页")
            candidates = []
            for target in pages:
                target_id = target["targetId"]
                try:
                    page = self._attach(browser, target_id)
                    info = _probe(page)
                except EdgeShotError:
                    self._detach(browser, target_id)
                    continue
                if info.get("found") or info.get("docVisibility") == "visible" or info.get("docFocused"):
                    playing = info.get("found") and not info.get("paused", True) and not info.get("ended", False)
                    preference = (bool(info.get("docFocused")), info.get("docVisibility") == "visible",
                                  bool(playing), target_id == self._target_id)
                    candidates.append((preference, target, page, info))
            if candidates:
                _, target, page, info = max(candidates, key=lambda item: item[0])
                self._target_id = target["targetId"]
                if self._pin_tab and info.get("found"):
                    self._pinned = target, page
                return self._capture_page(target, page, info, mode, at_time, circle)
            raise VideoNotFound("原 Edge 的 B 站标签页暂未提供可读的视频信息",
                                status="暂时无法读取 Edge 播放状态")
        except (OSError, WebSocketException) as exc:
            self.close()
            self._retry_at = time.monotonic() + 8.0
            raise ExistingEdgeUnavailable("等待原 Edge 允许本地连接") from exc


def capture_video(
    port: int = DEFAULT_PORT,
    *,
    url_pattern: Optional[str] = None,
    mode: str = "clip",
    scale: Optional[float] = None,
    trim_letterbox: bool = True,
    at_time: Optional[float] = None,
    play: bool = False,
    mute: bool = True,
    timeout: float = 15.0,
    screenshot_timeout: float = 45.0,
    wait_ready: float = 0.0,
) -> VideoShot:
    """截当前标签页里正在播放的 <video>。

    mode="clip"   用 Page.captureScreenshot 截视频元素的屏幕区域（推荐，跨域视频也能用）。
    mode="canvas" 把当前帧画到 canvas 再导出：视频原生分辨率、不含播放器控件和网页弹幕。
                  实测 B 站能画（MSE 的 blob: 源算同源），只有把跨域 URL 直接当 src 的视频
                  才会因 canvas 污染失败。
    scale         额外倍率，默认 1.0。CDP 的输出宽度 = CSS 宽度 × devicePixelRatio × scale，
                  所以 scale=1.0 已经是设备像素（125% 缩放时 640 CSS px -> 800 px），
                  想再放大就传 2.0。
    trim_letterbox  True 时按 object-fit 去掉元素框里的黑边，只留画面本身。
    at_time       指定秒数：先暂停并 seek 到该时间点再截（攻略视频截某一刻用）。
    play          True 时先让视频播起来（CDP 的 userGesture 能绕过自动播放限制），
                  配合 at_time 用；mute=True 会先静音，避免突然出声。
    screenshot_timeout
                  Page.captureScreenshot 单独的超时，默认 45s。窗口最小化 / 标签页在后台时
                  合成器要先醒过来，实测第一张要 6~30s，之后 0.1s 一张。
    wait_ready    最多等这么多秒让视频出画面（刚打开页面时用）。

    后台能力：整条链路都不需要窗口在前台，也不需要抢焦点。窗口最小化、被别的窗口盖住、
    标签页在后台都能截；canvas 模式甚至不经过合成器，最小化时也是 0.1s 级别。
    """
    if not debug_port_open(port):
        raise EdgeShotError(
            f"端口 {port} 没有开调试接口。用 launch_edge_debug() 起一个带 "
            f"--remote-debugging-port={port} 的 Edge（默认 profile 不行，见 restart_edge_with_debug_port 的说明）。"
        )
    if mode not in ("clip", "canvas"):
        raise ValueError("mode 只能是 'clip' 或 'canvas'")

    deadline = time.time() + max(0.0, wait_ready)
    while True:
        try:
            session, page, info = _pick_video_session(port, url_pattern, timeout)
        except VideoNotFound:
            if time.time() >= deadline:
                raise
            time.sleep(0.4)
            continue
        try:
            return _take_video_frame(session, page, info, mode=mode, scale=scale,
                                     trim_letterbox=trim_letterbox, at_time=at_time, play=play,
                                     mute=mute, screenshot_timeout=screenshot_timeout)
        finally:
            session.close()


def _picture_rect(info: dict[str, Any]) -> tuple[float, float, float, float]:
    """视频画面在元素框里的实际区域（object-fit: contain 时会去掉黑边）。"""
    rect = info["rect"]
    box_x, box_y = float(rect["x"]), float(rect["y"])
    box_w, box_h = float(rect["width"]), float(rect["height"])
    video_w = float(info.get("videoWidth") or 0)
    video_h = float(info.get("videoHeight") or 0)
    fit = str(info.get("objectFit") or "fill").lower()
    if video_w <= 0 or video_h <= 0 or box_w <= 0 or box_h <= 0:
        return box_x, box_y, box_w, box_h
    if fit not in ("contain", "scale-down"):
        return box_x, box_y, box_w, box_h      # cover/fill：元素框本身就是画面
    scale = min(box_w / video_w, box_h / video_h)
    picture_w, picture_h = video_w * scale, video_h * scale
    return (
        box_x + (box_w - picture_w) / 2,
        box_y + (box_h - picture_h) / 2,
        picture_w,
        picture_h,
    )


def _clip_shot(
    session: CDPSession | _AttachedPage,
    info: dict[str, Any],
    scale: Optional[float],
    trim_letterbox: bool = True,
    screenshot_timeout: float = 45.0,
) -> VideoShot:
    viewport = info["viewport"]
    if trim_letterbox:
        x, y, width, height = _picture_rect(info)
    else:
        rect = info["rect"]
        x, y = float(rect["x"]), float(rect["y"])
        width, height = float(rect["width"]), float(rect["height"])

    # 与视口求交：视频被滚出屏幕一部分时，只截仍然可见的那块
    left = max(0.0, x)
    top = max(0.0, y)
    right = min(x + width, float(viewport["width"]))
    bottom = min(y + height, float(viewport["height"]))
    x, y = left, top
    width, height = right - left, bottom - top
    if width < 2 or height < 2:
        raise VideoNotFound("video 元素不在可视区域内（可能被滚动到屏幕外或被折叠）")

    clip = {"x": round(x), "y": round(y), "width": round(width), "height": round(height)}
    # CDP 的输出尺寸 = CSS 尺寸 × devicePixelRatio × clip.scale，
    # 所以 scale 保持 1.0 就是设备像素；传 dpr 会二次放大（图变糊）。
    clip["scale"] = max(0.05, float(scale) if scale is not None else 1.0)
    params: dict[str, Any] = {
        "format": "png",
        "clip": clip,
        "captureBeyondViewport": False,
        "fromSurface": True,
    }
    try:
        result = session.call("Page.captureScreenshot", timeout=screenshot_timeout, **params)
    except EdgeShotError as exc:
        if "超时" not in str(exc):
            raise
        # 窗口最小化 / 被完全遮挡时，合成器可能一直不吐帧。
        # captureBeyondViewport 走离屏渲染，通常还能出图（慢几秒）。
        try:
            result = session.call(
                "Page.captureScreenshot",
                timeout=screenshot_timeout,
                **{**params, "captureBeyondViewport": True},
            )
        except EdgeShotError:
            raise EdgeShotError(
                "截图超时：窗口很可能被最小化或完全遮挡。后台场景请改用 "
                "mode='canvas'（不经过合成器，最小化时也只要 1s 内），"
                "或者 launch_edge_debug(headless=True) 完全不开窗口。"
            ) from exc
    png = base64.b64decode(result["data"])
    image = Image.open(io.BytesIO(png))
    image.load()
    info = dict(info)
    info["clip"] = clip
    info["picture"] = [round(x), round(y), round(width), round(height)]
    info["size"] = list(image.size)
    return VideoShot(image=image, info=info, via="cdp-clip", png=png)


def _window_region_shot(target_id: str, page: _AttachedPage, info: dict[str, Any], circle: VideoCircle,
                        at_time: float | None) -> VideoArrayShot:
    import cv2
    import numpy as np

    started = time.perf_counter()
    window = page.browser.call("Browser.getWindowForTarget", targetId=target_id, timeout=2.0)
    if window.get("bounds", {}).get("windowState") == "minimized":
        status = "唤醒模式等待 Edge 退出最小化"
        raise VideoNotFound(status, status=status)
    cx, cy, half = circle
    x, y, width, height = _picture_rect(info)
    scale_x = width / 640
    scale_y = height / (640 * info["videoHeight"] / info["videoWidth"])
    left, top = x + (cx - half) * scale_x, y + (cy - half) * scale_y
    crop_width, crop_height = 2 * half * scale_x, 2 * half * scale_y
    viewport = info["viewport"]
    if (min(crop_width, crop_height) < 2 or min(left, top) < 0
            or left + crop_width > min(x + width, viewport["width"])
            or top + crop_height > min(y + height, viewport["height"])):
        status = "唤醒模式等待小地图进入 Edge 可视区域"
        raise VideoNotFound(status, status=status)

    seeked = _seek_video(page, at_time) if at_time is not None else None
    canvas_started = time.perf_counter()
    try:
        shot = _fast_canvas_shot(page, circle, None, video_index=info.get("videoIndex"))
    except _CanvasReadBlocked:
        # Chromium changes the live view size when a screenshot specifies clip.
        result = page.call("Page.captureScreenshot", format="jpeg", quality=95,
                           fromSurface=True, captureBeyondViewport=False, timeout=2.0)
        received = time.perf_counter()
        encoded = base64.b64decode(result["data"])
        image = cv2.imdecode(np.frombuffer(encoded, dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None:
            raise EdgeShotError("Edge 视口截图无法解码")
        sx = image.shape[1] / viewport["width"]
        sy = image.shape[0] / viewport["height"]
        x1, x2 = round(left * sx), round((left + crop_width) * sx)
        y1, y2 = round(top * sy), round((top + crop_height) * sy)
        frame = image[y1:y2, x1:x2].copy()
        if not frame.size:
            raise EdgeShotError("Edge 视口截图中的小地图区域为空")
        draw = encode = 0
        read = (received - started) * 1000
        decode = (time.perf_counter() - received) * 1000
        size_kib = len(encoded) / 1024
        current_time = info["currentTime"]
    else:
        frame = shot.bgr
        draw, encode, read, decode, size_kib = shot.info["capture_stats"]
        read += (canvas_started - started) * 1000
        current_time = shot.info["currentTime"]

    size = max(212, min(frame.shape[:2]))
    if frame.shape[:2] != (size, size):
        resize_started = time.perf_counter()
        frame = cv2.resize(frame, (size, size), interpolation=cv2.INTER_LINEAR)
        decode += (time.perf_counter() - resize_started) * 1000
    metadata = {
        "currentTime": seeked["currentTime"] if seeked is not None else current_time,
        "size": [size, size],
        "capture_stats": (draw, encode, read, decode, size_kib),
    }
    if seeked is not None:
        metadata["seeked"] = seeked
    return VideoArrayShot(frame, metadata, via="cdp-window-region")


def _canvas_shot(session: CDPSession | _AttachedPage, info: dict[str, Any]) -> VideoShot:
    result = session.evaluate(_JS_CANVAS_FRAME)
    if not result or not result.get("ok"):
        raise EdgeShotError((result or {}).get("reason") or "canvas 抓帧失败")
    data_url = result["dataUrl"]
    png = base64.b64decode(data_url.split(",", 1)[1])
    image = Image.open(io.BytesIO(png))
    image.load()
    info = dict(info)
    info["size"] = list(image.size)
    return VideoShot(image=image, info=info, via="cdp-canvas", png=png)


def _fast_canvas_shot(page: _AttachedPage, circle: VideoCircle | None, at_time: float | None, *,
                      video_index: int | None = None) -> VideoArrayShot:
    import cv2
    import numpy as np

    seeked = _seek_video(page, at_time) if at_time is not None else None
    started = time.perf_counter()
    args = json.dumps(circle, allow_nan=False)
    if video_index is not None:
        args += f", {int(video_index)}"
    result = page.evaluate(f"{_JS_FAST_CANVAS_FRAME}({args})")
    received = time.perf_counter()
    if not result:
        raise VideoNotFound("原攻略标签页没有 B 站播放器", status="原攻略标签页没有 B 站视频")
    if not result.get("found"):
        ExistingEdgeVideo._video_not_ready(result)
    if result.get("invalidCircle"):
        status = "固定小地图区域超出攻略画面"
        raise VideoNotFound(status, status=status)
    if result.get("blocked"):
        raise _CanvasReadBlocked(result["reason"])
    if "dataUrl" not in result:
        raise EdgeShotError(result.get("reason") or "canvas 快速取帧失败")
    encoded = base64.b64decode(result["dataUrl"].partition(",")[2])
    if not encoded:
        raise EdgeShotError("Edge 返回的攻略帧为空")
    bgr = cv2.imdecode(np.frombuffer(encoded, dtype=np.uint8), cv2.IMREAD_COLOR)
    if bgr is None:
        raise EdgeShotError("Edge 返回的攻略帧无法解码")
    if circle is not None:
        if result.get("sample") is None:
            raise EdgeShotError("Edge 未返回固定小地图区域")
        x, y, half = result["sample"]
        size = 2 * half + 1
        bgr = cv2.getRectSubPix(bgr, (size, size), (x, y))
        if size < 100:
            bgr = cv2.resize(bgr, (212, 212), interpolation=cv2.INTER_CUBIC)
    info = {
        "currentTime": result["currentTime"],
        "size": [bgr.shape[1], bgr.shape[0]],
        "capture_stats": (result["drawMs"], result["encodeMs"], (received - started) * 1000,
                          (time.perf_counter() - received) * 1000, len(encoded) / 1024),
    }
    if seeked is not None:
        info["seeked"] = seeked
    return VideoArrayShot(bgr, info, via="cdp-canvas-region" if circle is not None else "cdp-canvas-fast")


def capture_page(
    port: int = DEFAULT_PORT,
    *,
    url_pattern: Optional[str] = None,
    full_page: bool = False,
    timeout: float = 15.0,
) -> VideoShot:
    """截整页（full_page=True 时按文档高度滚出整页长图）。"""
    pages = list_page_targets(port, timeout=timeout)
    if url_pattern:
        rx = re.compile(url_pattern, re.I)
        pages = [p for p in pages if rx.search(f"{p.get('title', '')} {p.get('url', '')}")]
    if not pages:
        raise EdgeShotError("调试端口上没有匹配的标签页")
    page = pages[0]
    with CDPSession(page["webSocketDebuggerUrl"], timeout=timeout) as session:
        params: dict[str, Any] = {"format": "png", "captureBeyondViewport": bool(full_page)}
        if full_page:
            metrics = session.call("Page.getLayoutMetrics")
            size = metrics.get("cssContentSize") or metrics.get("contentSize") or {}
            params["clip"] = {
                "x": 0, "y": 0,
                "width": max(1, round(size.get("width", 1280))),
                "height": max(1, round(size.get("height", 800))),
                "scale": 1,
            }
        result = session.call("Page.captureScreenshot", timeout=45.0, **params)
    png = base64.b64decode(result["data"])
    image = Image.open(io.BytesIO(png))
    image.load()
    info = {"page": {"title": page.get("title"), "url": page.get("url")}, "size": list(image.size)}
    return VideoShot(image=image, info=info, via="cdp-page", png=png)


# --------------------------------------------------------------------------- #
# 让 Edge 带上调试端口
# --------------------------------------------------------------------------- #

def find_edge_exe() -> str:
    """找 Edge 可执行文件；EDGE_EXE 环境变量优先。"""
    override = os.environ.get("EDGE_EXE")
    if override:
        if os.path.isfile(override):
            return override
        raise EdgeShotError(f"EDGE_EXE 指向的文件不存在: {override}")
    for path in EDGE_EXE_PATHS:
        if os.path.isfile(path):
            return path
    import shutil

    for name in EDGE_EXE_NAMES:
        found = shutil.which(name)
        if found:
            return found
    raise EdgeShotError("没找到 Edge，请用 EDGE_EXE 环境变量指定 msedge 的完整路径")


def launch_edge_debug(
    port: int = DEFAULT_PORT,
    *,
    user_data_dir: Optional[str | os.PathLike[str]] = None,
    urls: Iterable[str] = (),
    wait: float = 25.0,
    headless: bool = False,
) -> subprocess.Popen:
    """起一个独立 profile 的 Edge（不动你现在的窗口），并等调试端口就绪。

    user_data_dir 给个固定目录就能复用同一套标签页（登录态也留着）；
    不传则用 %LOCALAPPDATA%\\edge-video-shot-profile。
    headless=True 用 --headless=new，完全不开窗口（后台跑、不抢前台）。
    """
    if debug_port_open(port):
        raise EdgeShotError(f"端口 {port} 已经有一个调试中的 Edge，直接 attach 即可")
    profile = Path(user_data_dir) if user_data_dir else Path(
        os.environ.get("LOCALAPPDATA", os.path.expanduser("~"))
    ) / DEFAULT_PROFILE_DIR
    profile.mkdir(parents=True, exist_ok=True)
    exe = os.environ.get("EDGE_EXE") or find_edge_exe()
    args = [
        exe,
        f"--remote-debugging-port={int(port)}",
        f"--user-data-dir={profile}",
        "--no-first-run",
        "--no-default-browser-check",
    ]
    if headless:
        args.append("--headless=new")
    args.extend(urls)
    proc = subprocess.Popen(args, close_fds=True)
    if not wait_debug_port(port, timeout=wait):
        raise EdgeShotError(f"Edge 起来了但 {port} 端口没响应，检查是否被占用或被策略禁用")
    return proc


def kill_edge(proc: subprocess.Popen, wait: float = 5.0) -> None:
    """结束 launch_edge_debug / restart_edge_with_debug_port 起的那个 Edge 进程树。"""
    if _IS_WINDOWS:
        subprocess.run(
            ["taskkill", "/F", "/T", "/PID", str(proc.pid)], capture_output=True, check=False
        )
    else:
        try:
            proc.terminate()
        except Exception:
            pass
    try:
        proc.wait(timeout=wait)
    except Exception:
        pass


def close_debug_edge(port: int = DEFAULT_PORT, timeout: float = 10.0) -> bool:
    """通过 CDP 的 Browser.close 关掉带调试端口的那个 Edge（不碰你自己开的窗口）。"""
    if not debug_port_open(port):
        return False
    version = _http_json(port, "/json/version", timeout=timeout)
    ws_url = version.get("webSocketDebuggerUrl")
    if not ws_url:
        raise EdgeShotError("拿不到 browser 级 websocket 地址")
    try:
        with CDPSession(ws_url, timeout=timeout) as session:
            session.call("Browser.close")      # 浏览器会直接退出，可能收不到响应
    except EdgeShotError:
        pass
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not debug_port_open(port, timeout=0.5):
            return True
        time.sleep(0.2)
    return False


def close_edge(wait: float = 20.0) -> int:
    """给所有 Edge 窗口发 WM_CLOSE（相当于点叉），返回关闭的窗口数。"""
    windows = list_edge_windows()
    for window in windows:
        _user32.PostMessageW(window.hwnd, _WM_CLOSE, 0, 0)
    deadline = time.time() + wait
    while time.time() < deadline and list_edge_windows():
        time.sleep(0.3)
    return len(windows)


def restart_edge_with_debug_port(
    port: int = DEFAULT_PORT,
    *,
    user_data_dir: Optional[str | os.PathLike[str]] = None,
    urls: Iterable[str] = (),
    wait: float = 30.0,
) -> subprocess.Popen:
    """给「正在用的那个 profile」开调试端口——现代 Edge 已经不允许了。

    Edge / Chromium 从 136 版起，用默认 profile 启动时会直接忽略
    --remote-debugging-port（安全限制，防止本机任意程序接管你的登录态）。
    实测 Edge 150：加上该参数后窗口能起来、会话能恢复，但 9222 端口始终不监听。
    所以这里不再假装能重启，只把结论和替代方案讲清楚：

    1. launch_edge_debug(port, user_data_dir=<固定目录>)：独立 profile 的 Edge，
       在那边登录一次 B 站，之后每次都用同一个 profile，登录态、标签页都还在。
    2. capture_window()：零配置的窗口截图，但视频可能落在硬件覆盖层上截成黑块。
    """
    if user_data_dir is None:
        raise EdgeShotError(
            "默认 profile 不能开远程调试（Edge/Chromium 136+ 的安全限制，"
            "--remote-debugging-port 会被忽略）。改用 launch_edge_debug(port, user_data_dir=...)，"
            "或者用 capture_window() 走窗口截图。"
        )
    return launch_edge_debug(port, user_data_dir=user_data_dir, urls=urls, wait=wait)


# --------------------------------------------------------------------------- #
# 命令行
# --------------------------------------------------------------------------- #

def _default_out(mode: str) -> Path:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return Path(__file__).with_name("output") / "edge_video_shot" / f"{mode}-{stamp}.png"


def configure_stdout() -> None:
    """让 print 在 GBK 控制台下不会因为标题里的零宽字符之类直接崩掉。

    Edge 的窗口标题里带 U+200B，cp936 编不出来，默认 print 会抛 UnicodeEncodeError。
    这里只放宽错误处理，不改编码，中文照样正常显示。
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except Exception:
            pass


def main(argv: Optional[Sequence[str]] = None) -> int:
    configure_stdout()
    parser = argparse.ArgumentParser(description="抓 Edge 当前网页里的视频画面")
    parser.add_argument("--mode", choices=("video", "window", "page"), default="video")
    parser.add_argument("--port", type=int, default=int(os.environ.get("EDGE_SHOT_PORT", DEFAULT_PORT)))
    parser.add_argument("--url", default=os.environ.get("EDGE_SHOT_URL"),
                        help="只截 URL/标题匹配该正则的标签页")
    parser.add_argument("--title", default=None, help="窗口截图时按标题正则挑窗口")
    parser.add_argument("--frame-mode", choices=("clip", "canvas"), default="clip")
    parser.add_argument("--keep-letterbox", action="store_true",
                        help="保留视频元素框里的黑边（默认按 object-fit 裁掉）")
    parser.add_argument("--crop", default=None, help="窗口截图裁剪: x,y,w,h")
    parser.add_argument("--full", action="store_true", help="--mode page 时截整页长图")
    parser.add_argument("--at", type=float, default=None,
                        help="--mode video 时先暂停并 seek 到该秒数再截")
    parser.add_argument("--play", action="store_true",
                        help="--mode video 时先静音播放起来（绕过自动播放限制）")
    parser.add_argument("--wait", type=float, default=0.0, help="等视频出画面的秒数")
    parser.add_argument("--launch", metavar="URL", default=None,
                        help="先起一个独立 profile 的 Edge 打开该 URL（后台跑，不抢焦点）")
    parser.add_argument("--headless", action="store_true",
                        help="配合 --launch：用 --headless=new，完全不开窗口")
    parser.add_argument("--keep-browser", action="store_true",
                        help="截完不关掉 --launch 起的 Edge")
    parser.add_argument("--out", default=None)
    args = parser.parse_args(argv)

    crop = tuple(int(v) for v in args.crop.split(",")) if args.crop else None
    launched = None
    try:
        if args.launch:
            launched = launch_edge_debug(
                args.port, urls=[args.launch], headless=args.headless, wait=45.0
            )
        if args.mode == "window":
            shot = capture_window(title_pattern=args.title, crop=crop)
        elif args.mode == "page":
            shot = capture_page(args.port, url_pattern=args.url, full_page=args.full)
        else:
            shot = capture_video(
                args.port,
                url_pattern=args.url,
                mode=args.frame_mode,
                trim_letterbox=not args.keep_letterbox,
                at_time=args.at,
                play=args.play,
                wait_ready=args.wait,
            )
    finally:
        if launched is not None and not args.keep_browser:
            close_debug_edge(args.port)

    out = Path(args.out) if args.out else _default_out(args.mode)
    shot.save(out)
    print(f"[{shot.via}] {shot.image.width}x{shot.image.height} -> {out}")
    print(json.dumps(shot.info, ensure_ascii=False, indent=2, default=str))
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
