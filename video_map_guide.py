"""用原神当前画面和攻略视频的一帧计算提瓦特地图上的方向。

用法：python video_map_guide.py --video-image assest/screenshot.png
实时指引使用 Ctrl+Alt+F8 更新目标，Ctrl+Alt+F10 关闭指引。
"""

from __future__ import annotations

import argparse
from collections import deque
from contextlib import nullcontext
import ctypes
from ctypes import wintypes
from dataclasses import asdict, dataclass, field, replace
import json
import math
from pathlib import Path
import re
from statistics import median
import sys
from threading import Event, Lock, Thread
import time
from typing import Callable
from urllib.parse import urlsplit

import cv2
import numpy as np

from bettergi_map_localizer import (
    BetterGIMapLocalizer,
    BetterGITemplateLocalizer,
    Position,
    TemplateMatchDiagnostics,
    TemplatePosition,
    TEYVAT_SCALE,
)
from edge_video_shot import (
    EdgeShotError, ExistingEdgeUnavailable, ExistingEdgeVideo, VideoArrayShot, VideoNotFound,
    capture_video, debug_port_open, enable_dpi_awareness, launch_edge_debug,
)
from video_minimap import MinimapCircle, find_minimap_circles, has_video_minimap, video_content
from minimap_heading import camera_heading
from win32_screen_region import ScreenRegion
from window_video_shot import FULL_WINDOW, WindowsEdgeVideo, minimap_region, window_video_rect


_WORKSPACE = Path(sys.executable if getattr(sys, "frozen", False) else __file__).resolve().parent
_ASSETS = _WORKSPACE.parent / "Assets" / "Map" / "Teyvat"
_PAIMON_ICON = Path("GameTask/Common/Element/Assets/1920x1080/paimon_menu.png")
_BGI_MINIMAP = (62, 19, 212, 212)
_VIDEO_SIZE = (640, 360)
_DEFAULT_VIDEO_INSET = 12.0
_CACHE_SECONDS = 0.9
_SIFT_BLOCK_RADIUS = 350.0
_JUMP_DISTANCE = 150 / TEYVAT_SCALE
_JUMP_AGREEMENT = 8.0
_SMOOTH_SECONDS = 0.25
_HEADING_SMOOTH_SECONDS = 0.07
_VIDEO_FPS = 2.0
_PLAYER_FPS = 10.0
_HUD_FPS_MARGIN = 5.0
_FPS_WINDOW_SECONDS = 3.0
_VIDEO_STALE_SECONDS = 4.0
_ARRIVAL_DISTANCE = 2.0
_DIRECTIONS = ("北", "东北", "东", "东南", "南", "西南", "西", "西北")
_ARROWS = "↑↗→↘↓↙←↖"
_SCENE_NAMES = {
    "Teyvat": "提瓦特", "Enkanomiya": "渊下宫", "TheChasm": "层岩巨渊地下矿区",
    "SeaOfBygoneEras": "旧日之海", "AncientSacredMountain": "远古圣山",
    "TempleOfSpace": "空之神殿", "MoonCanon": "月球",
}

Match = Position | TemplatePosition
Localizer = BetterGIMapLocalizer | BetterGITemplateLocalizer


@dataclass(frozen=True)
class MapPoint:
    x: float
    y: float
    floor: int
    map_radius: float | None = None
    camera_heading: float | None = None
    scene: str = "Teyvat"
    match: Match | None = field(default=None, repr=False, compare=False)

    @classmethod
    def from_match(cls, sample: Match, *, heading: float | None = None) -> MapPoint:
        radius = sample.map_radius
        if radius is not None and (not math.isfinite(radius) or radius <= 0):
            radius = None
        return cls(sample.x, sample.y, sample.floor, radius, heading, sample.scene, sample)

    def distance(self, other: MapPoint) -> float:
        return math.hypot(other.x - self.x, other.y - self.y)


def _same_guide_block(anchor: Match | None, candidate: Match | None) -> bool:
    if anchor is None or candidate is None or (anchor.scene, anchor.floor) != (candidate.scene, candidate.floor):
        return False
    if isinstance(anchor, TemplatePosition):
        return isinstance(candidate, TemplatePosition) and candidate.layer_id == anchor.layer_id
    return (isinstance(candidate, Position)
            and max(abs(candidate.x - anchor.x), abs(candidate.y - anchor.y)) <= _SIFT_BLOCK_RADIUS)


class PositionTracker:
    """用连续样本确认跳点，用短时保留和按时间加权减小指引抖动。"""

    def __init__(self) -> None:
        self.raw: Match | None = None
        self.point: MapPoint | None = None
        self.pending: MapPoint | None = None
        self.updated_at = -math.inf

    def is_jump(self, sample: Match) -> bool:
        if self.raw is None:
            return False
        return (
            sample.scene != self.raw.scene or sample.floor != self.raw.floor
            or math.hypot(sample.x - self.raw.x, sample.y - self.raw.y) > _JUMP_DISTANCE
        )

    def update(self, sample: Match | None, now: float) -> tuple[MapPoint | None, bool]:
        if sample is None:
            self.pending = None
            return self._cached(now)

        point = MapPoint.from_match(sample)
        jumped = self.is_jump(sample)
        if jumped:
            pending = self.pending
            if (pending is None or pending.scene != point.scene or pending.floor != point.floor
                    or pending.distance(point) > _JUMP_AGREEMENT):
                self.pending = point
                return self._cached(now)

        self.pending = None
        if self.point is not None and not jumped and now - self.updated_at < _CACHE_SECONDS:
            weight = -math.expm1(-(now - self.updated_at) / _SMOOTH_SECONDS)
            radius = point.map_radius
            if radius is not None and self.point.map_radius is not None:
                radius = self.point.map_radius + weight * (radius - self.point.map_radius)
            point = replace(
                point,
                x=self.point.x + weight * (point.x - self.point.x),
                y=self.point.y + weight * (point.y - self.point.y),
                map_radius=radius,
            )
        self.raw = sample
        self.point = point
        self.updated_at = now
        return point, False

    def _cached(self, now: float) -> tuple[MapPoint | None, bool]:
        if self.point is not None and now - self.updated_at < _CACHE_SECONDS:
            return self.point, True
        return None, False


def game_minimap_rect(width: int, height: int) -> tuple[int, int, int, int]:
    """复用 BGI 的 1080p 基准矩形，坐标相对于游戏客户区。"""
    if height <= 0 or abs(width / height - 16 / 9) > 0.02:
        raise ValueError(f"游戏画面需要标准 16:9 布局，当前为 {width}×{height}")
    scale = width / 1920
    x, y, w, h = (round(v * scale) for v in _BGI_MINIMAP)
    if w <= 0 or x + w > width or y + h > height:
        raise ValueError("游戏画面过小，无法裁取小地图")
    return x, y, w, h


def crop_game_minimap(image: np.ndarray) -> np.ndarray:
    x, y, w, h = game_minimap_rect(image.shape[1], image.shape[0])
    return image[y:y + h, x:x + w]


def crop_video_minimap(
    image: np.ndarray, circle: tuple[float, float, float], inset: float
) -> np.ndarray:
    """圈参数使用 640×360 视频内容区坐标。"""
    h, w = image.shape[:2]
    if h <= 0 or abs(w / h - 16 / 9) > 0.02:
        raise ValueError(f"攻略视频需要 16:9 的内容截图，当前为 {w}×{h}")
    cx, cy, r = circle
    mask_radius = r - inset
    if not all(map(math.isfinite, (*circle, inset))) or inset < 0 or mask_radius < 44:
        raise ValueError("无效的小地图圆或内缩距离")
    if mask_radius > min(cx, cy, _VIDEO_SIZE[0] - 1 - cx, _VIDEO_SIZE[1] - 1 - cy):
        raise ValueError("视频小地图内圈超出画面，请检查圆心和内缩距离")

    normalized = cv2.resize(image, _VIDEO_SIZE, interpolation=cv2.INTER_AREA)
    # 传入正方形并使用现有定位器默认的 88% 遮罩，得到要求的半径。
    half = round(mask_radius / 0.88)
    return cv2.getRectSubPix(normalized, (2 * half + 1, 2 * half + 1), (cx, cy))


def paimon_icon_path(assets: Path) -> Path:
    for parent in assets.resolve().parents:
        icon = parent / _PAIMON_ICON
        if icon.is_file():
            return icon
    return _WORKSPACE.parent / _PAIMON_ICON


class GameWindowCapture:
    def __init__(self, title: str, verify_main_ui: bool = False, *, assets: Path = _ASSETS) -> None:
        if sys.platform != "win32":
            raise RuntimeError("实时游戏窗口截图需要 Windows")
        enable_dpi_awareness()
        self.title = title
        self.hud_hwnds: tuple[int, ...] = ()
        self._paimon = None
        if verify_main_ui:
            icon = paimon_icon_path(assets)
            if not icon.is_file():
                raise FileNotFoundError("找不到派蒙菜单图标。请选用完整 BetterGI 安装下的 Assets/Map/Teyvat 目录。")
            self._paimon = _read_image(icon)
        self._screen = ScreenRegion()
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.dwm = ctypes.WinDLL("dwmapi")
        self.user32.FindWindowW.argtypes = (wintypes.LPCWSTR, wintypes.LPCWSTR)
        self.user32.FindWindowW.restype = wintypes.HWND
        self.user32.GetForegroundWindow.restype = wintypes.HWND
        self.user32.GetClientRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))
        self.user32.GetClientRect.restype = wintypes.BOOL
        self.user32.ClientToScreen.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.POINT))
        self.user32.ClientToScreen.restype = wintypes.BOOL
        self.user32.IsIconic.argtypes = (wintypes.HWND,)
        self.user32.IsIconic.restype = wintypes.BOOL
        self.user32.IsWindowVisible.argtypes = (wintypes.HWND,)
        self.user32.IsWindowVisible.restype = wintypes.BOOL
        self.user32.GetWindow.argtypes = (wintypes.HWND, wintypes.UINT)
        self.user32.GetWindow.restype = wintypes.HWND
        self.user32.GetWindowRect.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.RECT))
        self.user32.GetWindowRect.restype = wintypes.BOOL
        self.dwm.DwmGetWindowAttribute.argtypes = (wintypes.HWND, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD)
        self.dwm.DwmGetWindowAttribute.restype = wintypes.LONG

    def _is_covered(self, hwnd: int, bounds: tuple[int, int, int, int]) -> bool:
        left, top, width, height = bounds
        seen = {hwnd}
        while above := self.user32.GetWindow(hwnd, 3):  # GW_HWNDPREV
            if above in seen:
                break
            seen.add(above)
            hwnd = above
            if above in self.hud_hwnds or not self.user32.IsWindowVisible(above) or self.user32.IsIconic(above):
                continue
            rect = wintypes.RECT()
            if self.dwm.DwmGetWindowAttribute(above, 9, ctypes.byref(rect), ctypes.sizeof(rect)) != 0:
                if not self.user32.GetWindowRect(above, ctypes.byref(rect)):
                    continue
            if left >= rect.right or left + width <= rect.left or top >= rect.bottom or top + height <= rect.top:
                continue
            cloaked = wintypes.DWORD()
            self.dwm.DwmGetWindowAttribute(above, 14, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
            if not cloaked.value:
                return True
        return False

    def foreground_client(self) -> tuple[int, tuple[int, int, int, int]] | None:
        hwnd = self.user32.FindWindowW(None, self.title)
        if (not hwnd or hwnd != self.user32.GetForegroundWindow()
                or not self.user32.IsWindowVisible(hwnd) or self.user32.IsIconic(hwnd)):
            return None
        client = wintypes.RECT()
        origin = wintypes.POINT()
        if not self.user32.GetClientRect(hwnd, ctypes.byref(client)):
            return None
        if not self.user32.ClientToScreen(hwnd, ctypes.byref(origin)):
            return None
        bounds = (origin.x, origin.y, client.right, client.bottom)
        if client.right <= 0 or client.bottom <= 0 or self._is_covered(hwnd, bounds):
            return None
        return hwnd, bounds

    def grab_minimap(self) -> np.ndarray | None:
        window = self.foreground_client()
        if window is None:
            return None
        _, (left, top, width, height) = window
        try:
            x, y, w, h = game_minimap_rect(width, height)
        except ValueError:
            return None  # 窗口调整大小的过程中可能暂时偏离 16:9。
        try:
            if self._paimon is None:
                frame = self._screen.grab(left + x, top + y, w, h)
            else:
                frame = self._screen.grab(left, top, x + w, y + h)
        except OSError:
            return None
        if self.foreground_client() != window:
            return None
        if self._paimon is None:
            return frame

        scale = width / 1920
        icon_height, icon_width = self._paimon.shape[:2]
        dimensions = (max(1, round(icon_width * scale)), max(1, round(icon_height * scale)))
        icon = cv2.resize(self._paimon, dimensions, interpolation=cv2.INTER_LINEAR)
        search_size = round(112 * scale)
        roi = frame[:search_size, :search_size]
        if min(roi.shape[:2]) < max(dimensions):
            return None
        _, score, _, _ = cv2.minMaxLoc(cv2.matchTemplate(roi, icon, cv2.TM_CCOEFF_NORMED))
        if score < 0.65:
            return None
        return frame[y:y + h, x:x + w]


def _read_image(path: Path) -> np.ndarray:
    image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"无法读取图片 {path}")
    return image


class GuidedPlayerMatcher:
    def __init__(self, assets: Path) -> None:
        self.assets = assets
        self._sift: BetterGIMapLocalizer | None = None
        self._template: BetterGITemplateLocalizer | None = None
        self._tile: tuple[str, str, int] | None = None

    def locate(self, minimap: np.ndarray, anchor: Match, previous: Match | None) -> Match | None:
        if min(minimap.shape[:2]) < 100:
            minimap = cv2.resize(minimap, (212, 212), interpolation=cv2.INTER_LINEAR)
        if isinstance(anchor, TemplatePosition):
            tile = (anchor.scene, anchor.layer_id, anchor.floor)
            if tile != self._tile:
                assets = self.assets if anchor.scene == "Teyvat" else self.assets.parent / anchor.scene
                self._template = BetterGITemplateLocalizer(assets, floor=anchor.floor, layer_id=anchor.layer_id)
                self._tile = tile
            near = previous if isinstance(previous, TemplatePosition) else None
            return self._template.locate(minimap, near=near, search_global=near is None)
        if self._sift is None:
            self._sift = BetterGIMapLocalizer(self.assets, floor=None, local_radius=_SIFT_BLOCK_RADIUS)
        self._sift.last_position = anchor
        return self._sift.locate(minimap, search_global=False)


def _direction_text(player: MapPoint, target: MapPoint) -> str:
    if player.scene != target.scene:
        return f"攻略位于{_SCENE_NAMES.get(target.scene, target.scene)}"
    if player.floor != target.floor:
        return f"玩家在层 {player.floor}，目标在层 {target.floor}"
    distance = player.distance(target)
    if distance <= _ARRIVAL_DISTANCE:
        return f"目标附近  ·  {distance:.1f}"
    bearing = _bearing(player, target)
    index = round(bearing / 45) % len(_DIRECTIONS)
    return f"{_ARROWS[index]}  {_DIRECTIONS[index]} {round(bearing) % 360}°  ·  {distance:.1f}"


def _bearing(player: MapPoint, target: MapPoint) -> float:
    # BetterGI 的游戏坐标 X 向西递增，Y 向北递增。
    return math.degrees(math.atan2(player.x - target.x, target.y - player.y)) % 360


def minimap_screen_target(bounds: tuple[int, int, int, int], player: MapPoint,
                          target: MapPoint | None) -> tuple[int, int] | None:
    """将同层攻略目标投影到原神小地图的屏幕像素。"""
    radius = player.map_radius
    if (radius is None or radius <= 0 or target is None
            or player.scene != target.scene or player.floor != target.floor):
        return None
    dx, dy = player.x - target.x, player.y - target.y
    if math.hypot(dx, dy) > radius:
        return None
    left, top, width, height = bounds
    try:
        x, y, w, h = game_minimap_rect(width, height)
    except ValueError:
        return None
    pixels_per_unit = (min(w, h) - 1) / (2 * radius)
    return (round(left + x + (w - 1) / 2 + dx * pixels_per_unit),
            round(top + y + (h - 1) / 2 + dy * pixels_per_unit))


def _angle_delta(angle: float, reference: float) -> float:
    return (angle - reference + 180) % 360 - 180


def _update_heading(heading: float | None, heading_at: float, minimap: np.ndarray | None,
                    now: float) -> tuple[float | None, float]:
    if minimap is None:
        return None, -math.inf
    detected = camera_heading(minimap)
    elapsed = now - heading_at
    if detected is None:
        return (heading, heading_at) if elapsed < _CACHE_SECONDS else (None, -math.inf)
    if heading is not None and elapsed < _CACHE_SECONDS:
        weight = -math.expm1(-elapsed / _HEADING_SMOOTH_SECONDS)
        detected = (heading + weight * _angle_delta(detected, heading)) % 360
    return detected, now


def _compass_display(player: MapPoint | None, target: MapPoint | None,
                     heading: float | None, note: str, video_detail: str = "",
                     *, arrival_distance: float = _ARRIVAL_DISTANCE) -> tuple[str, str, int | None, float | None]:
    if target is None:
        return note or "正在获取攻略画面", video_detail, None, None
    if player is None:
        return "正在定位玩家", note, None, None
    if player.scene != target.scene:
        return "玩家与攻略在不同地图", f"目标 · {_SCENE_NAMES.get(target.scene, target.scene)}", None, None
    different_floor = player.floor != target.floor
    layer_note = f"水平引导 · 目标层 {target.floor}" if different_floor else ""
    distance = player.distance(target)
    if distance <= arrival_distance:
        if different_floor:
            return "水平位置已到达", layer_note, None, distance
        return "目标附近", note, None, distance

    bearing = _bearing(player, target)
    if heading is None:
        direction = _DIRECTIONS[round(bearing / 45) % len(_DIRECTIONS)]
        detail = " · ".join(filter(None, (f"地图{direction}", layer_note, note or "视角未定位")))
        return f"{round(bearing) % 360}°", detail, None, distance
    offset = round(_angle_delta(bearing, heading))
    angle = f"{offset:+d}°" if offset else "0°"
    if abs(offset) > 90:
        rear = "右后方" if offset > 0 else "左后方"
        return angle, " · ".join(filter(None, (rear, layer_note, note))), offset, distance
    return angle, " · ".join(filter(None, (layer_note, note))), offset, distance


def guidance(player: MapPoint, target: MapPoint, *, arrival_distance: float = _ARRIVAL_DISTANCE) -> str:
    if player.scene != target.scene or player.floor != target.floor:
        return _direction_text(player, target)
    dx, dy = target.x - player.x, target.y - player.y
    distance = player.distance(target)
    bearing = _bearing(player, target)
    label = _DIRECTIONS[round(bearing / 45) % len(_DIRECTIONS)]
    status = "已在目标附近" if distance <= arrival_distance else f"向{label} {round(bearing) % 360}°"
    return (
        f"我 ({player.x:.1f}, {player.y:.1f})  目标 ({target.x:.1f}, {target.y:.1f})  "
        f"向量 ({dx:+.1f}, {dy:+.1f})  距离 {distance:.1f}  {status}"
    )


def _make_localizer(args: argparse.Namespace) -> Localizer:
    if args.method == "sift":
        return BetterGIMapLocalizer(args.assets)
    return BetterGITemplateLocalizer(args.assets, layer_id=args.layer_id)


def _read_video_frame(args: argparse.Namespace, at: float | None, *, metadata: dict | None = None,
                      existing_edge: ExistingEdgeVideo | WindowsEdgeVideo | None = None) -> np.ndarray:
    if args.video_image is not None:
        video = _read_image(args.video_image)
    else:
        started = time.monotonic()
        if existing_edge is not None:
            circle = None
            if args.frame_mode == "canvas-region":
                x, y, radius = args.video_circle
                circle = (x, y, radius - args.video_inset)
            if isinstance(existing_edge, WindowsEdgeVideo):
                shot = existing_edge.capture(circle)
            else:
                mode = "window-region" if getattr(args, "edge_mode", "remote") == "wake" else args.frame_mode
                shot = existing_edge.capture(url_pattern=args.video_pattern, mode=mode, at_time=at, circle=circle)
        elif args.edge_existing:
            with ExistingEdgeVideo() as original:
                shot = original.capture(url_pattern=args.video_pattern, mode=args.frame_mode, at_time=at)
        else:
            launched = args.video_url is not None and not debug_port_open(args.edge_port)
            if launched:
                profile = _WORKSPACE / "output" / f"edge-guide-{args.edge_port}"
                launch_edge_debug(args.edge_port, user_data_dir=profile, urls=[args.video_url])
            shot = capture_video(
                args.edge_port, url_pattern=args.video_pattern,
                mode="canvas" if args.frame_mode == "canvas-fast" else args.frame_mode,
                at_time=at, timeout=6.0, wait_ready=16.0 if launched else 0.0,
            )
        if at is not None:
            actual = shot.info.get("seeked", {}).get("currentTime")
            if not isinstance(actual, (float, int)) or abs(actual - at) > 0.25:
                current = f"{actual:.2f} 秒" if isinstance(actual, (float, int)) else "未知"
                raise ValueError(f"视频还没跳到 {at:g} 秒，当前进度 {current}")
        if not args.follow_video:
            print(f"攻略取帧 {shot.via}，耗时 {time.monotonic() - started:.2f} 秒", flush=True)
        if metadata is not None:
            metadata.update(shot.info)
        if isinstance(shot, VideoArrayShot):
            return shot.bgr
        video = cv2.cvtColor(np.asarray(shot.image.convert("RGB")), cv2.COLOR_RGB2BGR)
    return video


def _video_circles(frame: np.ndarray) -> list[MinimapCircle]:
    height, width = frame.shape[:2]
    standard = None
    if abs(width / height - 16 / 9) < 0.025:
        x, y, diameter, _ = game_minimap_rect(*_VIDEO_SIZE)
        standard = MinimapCircle(x + diameter / 2, y + diameter / 2, diameter / 2, detected=False)
    return find_minimap_circles(frame, standard)


def _video_minimap_candidates(video: np.ndarray, args: argparse.Namespace):
    frame = video_content(video)
    if args.video_circle is not None:
        yield crop_video_minimap(frame, tuple(args.video_circle), args.video_inset)
    else:
        for circle in _video_circles(frame):
            yield circle.crop(frame)


def _read_video_minimap(args: argparse.Namespace, at: float | None, *, metadata: dict | None = None,
                       existing_edge: ExistingEdgeVideo | None = None) -> np.ndarray:
    frame = _read_video_frame(args, at, metadata=metadata, existing_edge=existing_edge)
    minimap = next(_video_minimap_candidates(frame, args), None)
    if minimap is None:
        raise ValueError("这帧攻略画面未找到小地图")
    return minimap


class VideoMatchError(ValueError):
    """攻略视频匹配失败的阶段和 HUD 提示。"""

    def __init__(self, reason: str, *, retried: bool = False) -> None:
        super().__init__(reason)
        self.retried = retried


class VideoTemplateMatcher:
    """攻略首次全图定位，优先验证 SIFT 特征，再查整块地图模板。"""

    def __init__(self, assets: Path, layers: list[str] | None = None, *,
                 diagnostics_dir: Path | None = None, share_atlas: BetterGIMapLocalizer | None = None) -> None:
        self.maps = (tuple(BetterGITemplateLocalizer(assets, layer_id=layer) for layer in dict.fromkeys(layers))
                     if layers else (BetterGITemplateLocalizer(assets, use_pyramid=True),))
        self.previous: TemplatePosition | None = None
        self._last_frame: np.ndarray | None = None
        self._last_result: TemplatePosition | None = None
        self._sift: BetterGIMapLocalizer | None = None
        self._sift_assets = assets if not layers else None
        self._share_atlas = share_atlas
        self._circle: MinimapCircle | None = None
        self._last_video: np.ndarray | None = None
        self._last_target: MapPoint | None = None
        self._last_failure: tuple[str, bool] | None = None
        self._diagnostics_dir = diagnostics_dir
        self._last_report: str | None = None
        self._reported_at = -math.inf
        self.mode = "画面检查"

    def locate(self, minimap: np.ndarray, *, search_global: bool = True,
               diagnostics: dict[str, TemplateMatchDiagnostics] | None = None) -> TemplatePosition | None:
        if (self._last_frame is not None and (diagnostics is None or self._last_result is not None)
                and np.array_equal(self._last_frame, minimap)):
            return self._last_result
        previous_id = self.previous.layer_id if self.previous else None
        ordered = sorted(self.maps, key=lambda candidate: candidate.tiles[0].layer_id != previous_id)
        near = None if search_global else self.previous
        for localizer in ordered:
            details = None
            if diagnostics is not None:
                key = localizer.tiles[0].layer_id if len(localizer.tiles) == 1 else f"all_{len(localizer.tiles)}"
                details = diagnostics.setdefault(key, TemplateMatchDiagnostics())
            sample = localizer.locate(minimap, near=near, search_global=search_global, diagnostics=details)
            if sample is None:
                continue
            if self.previous is None or self.previous.layer_id != sample.layer_id:
                print(f"攻略地图 {sample.name}（{sample.layer_id}）", flush=True)
            self.previous = sample
            self._last_frame = minimap
            self._last_result = sample
            return sample
        if search_global:
            self._last_frame = minimap
            self._last_result = None
        return None

    def _locate_features(self, crop: np.ndarray, circle: MinimapCircle | None, *,
                         local_first: bool = False) -> MapPoint | None:
        if self._sift_assets is not None:
            try:
                self._sift = BetterGIMapLocalizer(self._sift_assets, floor=None, min_inliers=15,
                                                  min_inlier_ratio=0.5, share_atlas=self._share_atlas)
            except (OSError, RuntimeError, ValueError) as error:
                print(f"攻略 SIFT 特征地图不可用：{error}", file=sys.stderr, flush=True)
            self._sift_assets = None
        if self._sift is None:
            return None
        prior = self._sift.last_position
        sample = self._sift.locate(crop, local_first=local_first, search_global=not local_first)
        if sample is None or sample.reprojection_error > 3:
            self._sift.last_position = prior
            return None
        if not local_first:
            print(f"攻略 SIFT 全图定位通过，内点 {sample.inliers}/{sample.matches}，"
                  f"重投影误差 {sample.reprojection_error:.2f} px", flush=True)
        self.previous = None
        self._last_frame = None
        self._last_result = None
        self._set_circle(circle)
        self._last_target = MapPoint.from_match(sample, heading=camera_heading(crop))
        return self._last_target

    def _set_circle(self, circle: MinimapCircle | None) -> None:
        if circle is not None and (self._circle is None or not circle.overlaps(self._circle)):
            print(f"攻略小地图自动检测 ({circle.x:.1f}, {circle.y:.1f})，半径 {circle.radius:.1f}", flush=True)
        self._circle = circle

    def _fail(self, status: str, video: np.ndarray,
              reports: list[tuple[MinimapCircle | None, dict[str, TemplateMatchDiagnostics]]],
              *, playhead: float | None, retried: bool = False) -> None:
        self._last_failure = status, retried
        now = time.monotonic()
        if self._diagnostics_dir is not None and (self._last_report != status or now - self._reported_at > 15):
            self._last_report, self._reported_at = status, now
            report = {
                "status": status,
                "match_mode": self.mode,
                "time": playhead,
                "frame_size": [video.shape[1], video.shape[0]],
                "sift_checked": bool(reports) and self._sift is not None,
                "candidates": [{"circle": asdict(circle) if circle else None,
                                "matches": {name: asdict(result) for name, result in matches.items()}}
                               for circle, matches in reports],
            }
            try:
                self._diagnostics_dir.mkdir(parents=True, exist_ok=True)
                encoded, png = cv2.imencode(".png", video)
                if not encoded:
                    raise OSError("攻略帧无法编码")
                (self._diagnostics_dir / "guide-video-diagnostic.png").write_bytes(png.tobytes())
                (self._diagnostics_dir / "guide-video-diagnostic.json").write_text(
                    json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
            except (OSError, cv2.error) as error:
                print(f"无法保存攻略诊断帧：{error}", file=sys.stderr, flush=True)
        raise VideoMatchError(status, retried=retried)

    def locate_video(self, video: np.ndarray, args: argparse.Namespace, *, playhead: float | None = None,
                     on_global: Callable[[bool], None] | None = None) -> MapPoint:
        self.mode = "画面检查"
        if self._last_video is not None and np.array_equal(self._last_video, video):
            self.mode = "同帧复用"
            if self._last_failure is not None:
                reason, retried = self._last_failure
                raise VideoMatchError(reason, retried=retried)
            if self._last_target is not None:
                return self._last_target
        retried = self.previous is not None or (self._sift is not None and self._sift.last_position is not None)
        self._last_video = video
        self._last_target = None
        self._last_failure = None
        reports = []
        thumbnail = cv2.resize(video, (96, 54), interpolation=cv2.INTER_AREA)
        if np.mean(thumbnail.max(axis=2) <= 12) >= 0.995:
            self._fail("攻略取到的是黑屏帧", video, reports, playhead=playhead)
        frame = video_content(video)
        manual_crop = (crop_video_minimap(frame, tuple(args.video_circle), args.video_inset)
                       if args.video_circle is not None else None)
        if retried and (self._circle is not None or manual_crop is not None):
            self.mode = "局部匹配"
            crop = manual_crop if manual_crop is not None else self._circle.crop(frame)
            if self.previous is None:
                point = self._locate_features(crop, self._circle, local_first=True)
                if point is not None:
                    return point
            else:
                sample = self.locate(crop, search_global=False)
                if sample is not None:
                    self._last_target = MapPoint.from_match(sample, heading=camera_heading(crop))
                    return self._last_target
        crops = ([(None, manual_crop)] if manual_crop is not None else
                 [(circle, circle.crop(frame)) for circle in _video_circles(frame)])
        crops = [(circle, crop) for circle, crop in crops if has_video_minimap(crop)]
        if not crops:
            self.mode = "画面检查"
            self._fail("攻略画面未检测到小地图", video, reports, playhead=playhead)
        self.mode = "全图重试" if retried else "全图匹配"
        if on_global is not None:
            on_global(retried)
        for circle, crop in crops:
            point = self._locate_features(crop, circle)
            if point is not None:
                return point
        for circle, crop in crops:
            details: dict[str, TemplateMatchDiagnostics] = {}
            sample = self.locate(crop, diagnostics=details)
            if sample is not None:
                self._set_circle(circle)
                self._last_target = MapPoint.from_match(sample, heading=camera_heading(crop))
                return self._last_target
            reports.append((circle, details))
        detected = [result for _, details in reports for result in details.values()]
        if any(result.ambiguous is not None for result in detected):
            status = "地图精匹配有多个相近坐标"
        elif any(result.coarse_peaks for result in detected):
            status = "地图粗匹配通过，精匹配未通过"
        elif any(result.atlas_candidates for result in detected):
            status = "找到疑似小地图圆，地图粗匹配未通过"
        else:
            status = "找到疑似小地图圆，全图筛选无候选"
        self._fail(status, video, reports, playhead=playhead, retried=retried)


def _locate_target(localizer: Localizer | VideoTemplateMatcher, minimap: np.ndarray) -> MapPoint:
    sample = (
        localizer.locate(minimap, local_first=False)
        if isinstance(localizer, BetterGIMapLocalizer) else localizer.locate(minimap)
    )
    if sample is None:
        raise ValueError("攻略帧未匹配到可信坐标")
    return MapPoint.from_match(sample, heading=camera_heading(minimap))


def _sample_video(args: argparse.Namespace, at: float | None,
                  localizer: VideoTemplateMatcher | None,
                  existing_edge: ExistingEdgeVideo | None = None) -> np.ndarray | MapPoint:
    if localizer is None:
        return (_read_video_minimap(args, at, existing_edge=existing_edge) if existing_edge is not None
                else _read_video_minimap(args, at))
    details = {}
    frame = _read_video_frame(args, at, metadata=details, existing_edge=existing_edge)
    return localizer.locate_video(frame, args, playhead=details.get("currentTime"))


@dataclass(frozen=True)
class PlayerReading:
    point: MapPoint | None = None
    updated_at: float = -math.inf
    cached: bool = False

    def current(self, now: float) -> MapPoint | None:
        return self.point if now - self.updated_at < _CACHE_SECONDS else None


@dataclass(frozen=True)
class TargetReading:
    point: MapPoint | None = None
    captured_at: float = -math.inf
    note: str = "正在后台取攻略帧"
    detail: str = ""

    def current(self, now: float, follow_video: bool) -> MapPoint | None:
        return self.point if not follow_video or now - self.captured_at <= _VIDEO_STALE_SECONDS else None


@dataclass(frozen=True)
class CapturedPlayerFrame:
    captured_at: float = -math.inf
    frame: np.ndarray | None = None
    heading: float | None = None
    capture_started: float = 0.0
    ready_at: float = 0.0


@dataclass(frozen=True)
class CapturedVideoFrame:
    captured_at: float
    elapsed: float
    frame: np.ndarray | None = None
    playhead: float | None = None
    error: Exception | None = None
    capture_started: float = 0.0


TimingRows = tuple[tuple[str, float | None], ...]


class PipelineRates:
    def __init__(self) -> None:
        self._started = time.perf_counter()
        self._captured: deque[float] = deque()
        self._processed: deque[float] = deque()
        self._updated: deque[float] = deque()
        self._lock = Lock()

    @staticmethod
    def _append(events: deque[float], finished: float) -> None:
        events.append(finished)
        cutoff = finished - _FPS_WINDOW_SECONDS
        while events and events[0] <= cutoff:
            events.popleft()

    def record_capture(self, finished: float) -> None:
        with self._lock:
            self._append(self._captured, finished)

    def record_location(self, finished: float, *, updated: bool) -> None:
        with self._lock:
            self._append(self._processed, finished)
            if updated:
                self._append(self._updated, finished)

    def row(self, now: float) -> tuple[str, str]:
        values = ("—", "—", "—")
        if now - self._started >= _FPS_WINDOW_SECONDS:
            cutoff = now - _FPS_WINDOW_SECONDS
            with self._lock:
                counts = [sum(cutoff < stamp <= now for stamp in events)
                          for events in (self._captured, self._processed, self._updated)]
            values = tuple(f"{count / _FPS_WINDOW_SECONDS:.1f}" for count in counts)
        return "FPS", f"取帧 {values[0]}  定位 {values[1]}  坐标 {values[2]}"


@dataclass(frozen=True)
class MatchTiming:
    phases: tuple[tuple[str, float], ...]
    capture_started: float = 0.0
    ready_at: float = 0.0
    finished: float | None = None

    def rows(self, now: float) -> TimingRows:
        end = self.finished if self.finished is not None else now
        rows = []
        if self.ready_at:
            rows.append(("帧交接", max(0.0, (self.phases[0][1] - self.ready_at) * 1000)))
        for index, (name, started) in enumerate(self.phases):
            last = index + 1 == len(self.phases)
            stop = end if last else self.phases[index + 1][1]
            rows.append((f"{name} · 进行中" if last and self.finished is None else name,
                         max(0.0, (stop - started) * 1000)))
        if self.capture_started:
            rows.append(("端到端", max(0.0, (end - self.capture_started) * 1000)))
        return tuple(rows)


@dataclass
class GuideTimings:
    video_capture_started: float = 0.0
    video_capture: TimingRows = (("攻略取帧", None),)
    video_match: MatchTiming | None = None
    video_search_history: TimingRows = ()
    player_capture: TimingRows = (("小地图取帧", None), ("视角识别", None))
    player_match: MatchTiming | None = None
    player_wait: str = "等待玩家小地图"
    video_rates: PipelineRates = field(default_factory=PipelineRates)
    player_rates: PipelineRates = field(default_factory=PipelineRates)

    def record_video_capture(self, finished: float, stats, mode: str, *, available: bool) -> None:
        if available:
            self.video_rates.record_capture(finished)
        label = "攻略取帧" if available else "攻略取帧失败"
        rows = [(label, (finished - self.video_capture_started) * 1000)]
        if stats is not None:
            draw, encode, read, decode, _ = stats
            if mode == "windows":
                rows.extend((("等待与读回", read), ("像素转换", decode)))
            else:
                canvas = mode == "remote" or draw or encode
                if canvas:
                    rows.extend((("视频绘制", draw), ("PNG 编码", encode)))
                rows.extend((("CDP 往返", read), ("PNG 解码" if canvas else "JPEG 解码", decode)))
        self.video_capture = tuple(rows)
        self.video_capture_started = 0.0

    def finish_video_match(self, finished: float, mode: str) -> None:
        clock = self.video_match
        if clock is None:
            return
        phases = clock.phases
        if mode and len(phases) == 1:
            phases = ((mode, phases[0][1]),)
        completed = replace(clock, phases=phases, finished=finished)
        if len(phases) > 1:
            rows = dict(completed.rows(finished))
            history = dict(self.video_search_history)
            history.update((name, rows[name]) for name, _ in phases[1:])
            self.video_search_history = tuple(history.items())
        self.video_match = completed

    def sections(self, now: float, args):
        mode = getattr(args, "edge_mode", "remote")
        if args.follow_video:
            labels = {"windows": "Windows 捕获", "wake": "唤醒模式", "remote": "远程模式"}
            video_title = f"攻略 · {labels[mode]}"
            active = self.video_capture_started
            if active and now - active >= 0.15:
                video_rows = (("攻略取帧 · 进行中", (now - active) * 1000),)
            else:
                video_rows = self.video_capture
            video_rows = (self.video_rates.row(now), *video_rows)
        else:
            video_title = "攻略"
            video_rows = ()
        video_match, player_match = self.video_match, self.player_match
        video_rows += video_match.rows(now) if video_match is not None else (("攻略定位", None),)
        current_phases = {name for name, _ in video_match.phases} if video_match is not None else set()
        video_rows += tuple((f"上次{name}", duration) for name, duration in self.video_search_history
                            if name not in current_phases)
        player_rows = player_match.rows(now) if player_match is not None else ((self.player_wait, None),)
        return ((video_title, video_rows), ("玩家", (self.player_rates.row(now), *self.player_capture, *player_rows)))


class LocationRate:
    def __init__(self, name: str, pipeline_rates: PipelineRates | None = None) -> None:
        self.name = name
        self.pipeline_rates = pipeline_rates
        self.reset()

    def reset(self) -> None:
        self.started: float | None = None
        self.processed = self.published = self.reused = 0
        self.latencies: list[tuple[float, float]] = []

    def record(self, started: float, *, published: bool, reused: bool = False, capture_started: float = 0.0) -> None:
        if self.started is None:
            self.started = started
        self.processed += 1
        self.published += int(published)
        self.reused += int(reused)
        finished = time.perf_counter()
        if self.pipeline_rates is not None:
            self.pipeline_rates.record_location(finished, updated=published and not reused)
        if published and not reused and capture_started:
            self.latencies.append(((finished - started) * 1000, (finished - capture_started) * 1000))
        elapsed = finished - self.started
        if elapsed >= 5:
            cache = f"，同帧复用 {self.reused / elapsed:.1f} 帧/秒" if self.name == "攻略" else ""
            latency = ""
            if self.latencies:
                match, total = map(median, zip(*self.latencies))
                latency = (f"，新坐标 {(self.published - self.reused) / elapsed:.1f} 帧/秒"
                           f"，中位定位 {match:.0f} ms，端到端 {total:.0f} ms")
            print(f"{self.name}定位实测 {elapsed:.1f} 秒，处理 {self.processed / elapsed:.1f} 帧/秒"
                  f"，发布坐标 {self.published / elapsed:.1f} 帧/秒{cache}{latency}", flush=True)
            self.reset()


def _hud_fps(args: argparse.Namespace) -> float:
    return max(args.video_fps, args.player_fps) + _HUD_FPS_MARGIN


class LiveGuide:
    """两路各自采样和定位，每路只交接最新一帧和最新结果。"""

    def __init__(self, args: argparse.Namespace, capture: GameWindowCapture) -> None:
        self.args = args
        self.capture = capture
        self.player = PlayerReading()
        self.target = TargetReading()
        self._guide_anchor: Match | None = None
        self.player_frame = CapturedPlayerFrame()
        self.timings = GuideTimings() if getattr(args, "detailed_hud", False) and not args.console else None
        self.error: str | None = None
        self.stop = Event()
        self.refresh = Event()
        self._player_ready = Event()
        self._video_ready = Event()
        self._latest_video: CapturedVideoFrame | None = None
        workers = [self._capture_player, self._track_player, self._track_video]
        if args.follow_video:
            workers.append(self._capture_video)
        self.workers = [Thread(target=self._guarded, args=(work,), name=work.__name__) for work in workers]

    def start(self) -> None:
        print(f"玩家取帧目标 {self.args.player_fps:g} 帧/秒，HUD 刷新目标 {_hud_fps(self.args):g} 帧/秒", flush=True)
        if self.args.follow_video:
            print(f"攻略取帧频率 {self.args.video_fps:g} 帧/秒", flush=True)
        for worker in self.workers:
            worker.start()

    def _guarded(self, work: Callable[[], None]) -> None:
        try:
            work()
        except Exception as error:
            self.error = str(error)
            self.stop.set()

    def _capture_player(self) -> None:
        next_frame = time.perf_counter()
        heading, heading_at = None, -math.inf
        samples = []
        timing = self.timings
        while not self.stop.wait(max(0.0, next_frame - time.perf_counter())):
            started = time.perf_counter()
            frame = self.capture.grab_minimap()
            grabbed = time.perf_counter()
            now = time.monotonic()
            if not self.args.console:
                heading, heading_at = _update_heading(heading, heading_at, frame, now)
            ready_at = 0.0
            if timing is not None:
                ready_at = time.perf_counter()
                if frame is not None:
                    timing.player_rates.record_capture(grabbed)
                label = "小地图取帧" if frame is not None else "未取得小地图"
                timing.player_capture = ((label, (grabbed - started) * 1000),
                                         ("视角识别", (ready_at - grabbed) * 1000 if frame is not None else None))
            self.player_frame = CapturedPlayerFrame(now, frame, heading, started, ready_at)
            self._player_ready.set()
            if frame is None:
                samples.clear()
            else:
                samples.append((grabbed, (grabbed - started) * 1000))
                if len(samples) == 40:
                    times, durations = zip(*samples)
                    fps = (len(times) - 1) / (times[-1] - times[0])
                    print(f"玩家小地图实际取帧 {fps:.1f} 帧/秒，中位取帧 {median(durations):.0f} ms", flush=True)
                    samples.clear()
            next_frame = max(next_frame + 1 / self.args.player_fps, time.perf_counter())

    def _track_player(self) -> None:
        localizer = GuidedPlayerMatcher(self.args.assets)
        tracker = PositionTracker()
        timing = self.timings
        rate = LocationRate("玩家", timing.player_rates if timing is not None else None)
        last = None
        previous = None
        while not self.stop.is_set():
            self._player_ready.wait()
            self._player_ready.clear()
            captured = self.player_frame
            if self.stop.is_set() or captured is last:
                continue
            last = captured
            started = time.perf_counter()
            anchor = self._guide_anchor
            can_match = captured.frame is not None and anchor is not None
            if timing is not None:
                if can_match:
                    timing.player_match = MatchTiming((("攻略图块匹配", started),),
                                                      captured.capture_started, captured.ready_at)
                else:
                    timing.player_match = None
                    timing.player_wait = "等待玩家小地图" if captured.frame is None else "等待攻略图块"
            if tracker.raw is not None and not _same_guide_block(anchor, tracker.raw):
                tracker = PositionTracker()
                previous = None
            sample = None
            if can_match:
                sample = localizer.locate(captured.frame, anchor, previous)
                current = self._guide_anchor
                if not _same_guide_block(current, sample):
                    sample = None
                if tracker.raw is not None and not _same_guide_block(current, tracker.raw):
                    tracker = PositionTracker()
            point, cached = tracker.update(sample, captured.captured_at)
            self.player = PlayerReading(point, tracker.updated_at, cached)
            previous = sample if point is not None and not cached else None
            if can_match:
                if timing is not None:
                    timing.player_match = replace(timing.player_match, finished=time.perf_counter())
                rate.record(started, published=point is not None and not cached, capture_started=captured.capture_started)
            else:
                rate.reset()

    def _capture_video(self) -> None:
        if getattr(self.args, "edge_mode", "remote") == "windows":
            source = WindowsEdgeVideo(title_pattern=self.args.video_pattern, video_rect=self.args.window_video_rect)
        else:
            port = None if self.args.edge_existing else self.args.edge_port
            source = ExistingEdgeVideo(pin_tab=True, debug_port=port)
        with source as existing:
            self._capture_video_frames(existing)

    def _capture_video_frames(self, existing: ExistingEdgeVideo | WindowsEdgeVideo) -> None:
        next_frame = time.perf_counter()
        at = self.args.at
        launch_url = self.args.video_url
        samples = []
        timing = self.timings
        edge_mode = getattr(self.args, "edge_mode", "remote")
        while not self.stop.wait(max(0.0, next_frame - time.perf_counter())):
            started = time.perf_counter()
            if timing is not None:
                timing.video_capture_started = started
            metadata = {}
            try:
                if launch_url is not None:
                    url, launch_url = launch_url, None
                    if not debug_port_open(self.args.edge_port):
                        profile = _WORKSPACE / "output" / f"edge-guide-{self.args.edge_port}"
                        launch_edge_debug(self.args.edge_port, user_data_dir=profile, urls=[url])
                frame = _read_video_frame(self.args, at, metadata=metadata, existing_edge=existing)
            except Exception as error:
                finished = time.perf_counter()
                if timing is not None:
                    timing.record_video_capture(finished, None, edge_mode, available=False)
                self._latest_video = CapturedVideoFrame(time.monotonic(), finished - started, error=error)
                self._video_ready.set()
            else:
                finished = time.perf_counter()
                stats = metadata.get("capture_stats")
                if timing is not None:
                    timing.record_video_capture(finished, stats, edge_mode, available=True)
                self._latest_video = CapturedVideoFrame(time.monotonic(), finished - started, frame,
                                                        metadata.get("currentTime"), capture_started=started)
                self._video_ready.set()
                if stats is not None:
                    samples.append((finished, (finished - started) * 1000, *stats))
                    if len(samples) == 20:
                        _, capture, draw, encode, read, decode, size = map(median, zip(*samples))
                        elapsed = samples[-1][0] - samples[0][0]
                        fps = (len(samples) - 1) / elapsed
                        if edge_mode == "windows":
                            print(f"攻略 Windows 捕获小地图 {fps:.1f} 帧/秒，中位取帧 {capture:.0f} ms，"
                                  f"等待和读回 {read:.0f} ms，像素转换 {decode:.1f} ms，单帧读回 {size:.0f} KiB", flush=True)
                        else:
                            scope = "小地图" if self.args.frame_mode == "canvas-region" else "整帧"
                            canvas = (f"（绘制 {draw:.0f} ms，编码 {encode:.0f} ms）"
                                      if edge_mode == "remote" or draw or encode else "")
                            print(f"攻略快速取帧 {scope} {fps:.1f} 帧/秒，中位取帧 {capture:.0f} ms，CDP 往返 {read:.0f} ms"
                                  f"{canvas}，解码 {decode:.0f} ms，单帧 {size:.0f} KiB", flush=True)
                        samples.clear()
            at = None
            next_frame = max(next_frame + 1 / self.args.video_fps, finished)

    def _publish_search(self, retry: bool | str) -> None:
        if retry == "region":
            note = "局部无法匹配，正在匹配所在国家或独立地图"
            phase = "国家或独立地图匹配"
        else:
            note = "正在全图重试" if retry else "正在全图匹配"
            phase = "全图重试" if retry else "全图匹配"
        if self.timings is not None and self.timings.video_match is not None:
            clock = self.timings.video_match
            self.timings.video_match = replace(clock, phases=(*clock.phases, (phase, time.perf_counter())))
        previous = self.target
        self.target = TargetReading(previous.point, previous.captured_at, note)
        if retry:
            print(note, flush=True)

    def _video_failure(self, error: Exception) -> TargetReading:
        if not self.args.follow_video:
            previous = self.target
            note = "更新失败，保留上个目标" if previous.point else "攻略定位失败，Ctrl+Alt+F8 重试"
            return TargetReading(previous.point, previous.captured_at, note)
        if isinstance(error, ExistingEdgeUnavailable):
            return TargetReading(note=str(error))
        if isinstance(error, VideoNotFound):
            return TargetReading(note=error.status)
        if isinstance(error, EdgeShotError):
            return TargetReading(note="Edge 取帧链路出错，见日志")
        if isinstance(error, VideoMatchError):
            if error.retried:
                return TargetReading(note="全图重试后无法匹配", detail=str(error))
            return TargetReading(note=str(error))
        return TargetReading(note="攻略定位发生错误，见日志")

    def _track_video(self) -> None:
        follow = self.args.follow_video
        use_template = follow or self.args.edge_existing or self.args.video_layer_id
        video = (VideoTemplateMatcher(self.args.assets, self.args.video_layer_id, diagnostics_dir=_WORKSPACE / "output")
                 if use_template else None)
        static_locator = _make_localizer(self.args) if video is None else None
        last_frame = None
        first = True
        last_failure = last_mode = ""
        timing = self.timings
        rate = LocationRate("攻略", timing.video_rates if timing is not None else None)
        connection = ExistingEdgeVideo() if self.args.edge_existing and not follow else nullcontext()
        with connection as existing:
            while not self.stop.is_set():
                captured = None
                if follow:
                    self._video_ready.wait()
                    self._video_ready.clear()
                    captured = self._latest_video
                    if captured is None or captured is last_frame:
                        continue
                    last_frame = captured
                elif not first:
                    self.refresh.wait()
                    self.refresh.clear()
                if self.stop.is_set():
                    break

                started = time.perf_counter()
                if timing is not None:
                    if captured is None:
                        timing.video_match = MatchTiming((("目标载入与定位", started),))
                    elif captured.error is not None:
                        timing.video_match = None
                    else:
                        preparation = "画面检查与局部尝试" if self._guide_anchor is not None else "匹配前处理"
                        timing.video_match = MatchTiming(((preparation, started),), captured.capture_started,
                                                         captured.capture_started + captured.elapsed)
                previous = self.target
                result = error = None
                try:
                    if captured is not None:
                        if captured.error is not None:
                            raise captured.error
                        result = video.locate_video(captured.frame, self.args, playhead=captured.playhead,
                                                    on_global=self._publish_search)
                    else:
                        self.target = TargetReading(previous.point, previous.captured_at, "正在取攻略帧")
                        result = _sample_video(self.args, self.args.at if first else None, video, existing)
                        if not isinstance(result, MapPoint):
                            result = _locate_target(static_locator, result)
                except Exception as failure:
                    error = failure
                first = False
                if self.stop.is_set():
                    break

                elapsed = mode = ""
                if captured is not None:
                    mode = video.mode if captured.error is None else "未开始匹配"
                    elapsed = f"，取帧 {captured.elapsed * 1000:.0f} ms，{mode} {(time.perf_counter() - started) * 1000:.0f} ms"
                if timing is not None:
                    timing.finish_video_match(time.perf_counter(), mode)
                if error is not None:
                    self.target = self._video_failure(error)
                    if captured is not None:
                        if captured.frame is not None:
                            rate.record(started, published=False)
                        else:
                            rate.reset()
                    if str(error) != last_failure:
                        print(f"攻略目标更新失败：{error}{elapsed}", file=sys.stderr, flush=True)
                        last_failure = str(error)
                    continue

                if result.match is not None:
                    if not _same_guide_block(self._guide_anchor, result.match):
                        block = (result.match.layer_id if isinstance(result.match, TemplatePosition)
                                 else "SIFT 局部特征")
                        print(f"玩家跟随攻略图块 {result.scene} / 层 {result.floor} / {block}", flush=True)
                    self._guide_anchor = result.match
                self.target = TargetReading(result, captured.captured_at if captured else time.monotonic(), "")
                if captured is not None:
                    rate.record(started, published=True, reused=mode == "同帧复用", capture_started=captured.capture_started)
                target = previous.point
                if (target is None or target.scene != result.scene or target.floor != result.floor
                        or target.distance(result) >= 2 or mode != last_mode):
                    angle = result.camera_heading
                    view = f"，攻略视角 {round(angle) % 360}°" if angle is not None else ""
                    print(f"攻略匹配成功 ({result.x:.2f}, {result.y:.2f}) 层 {result.floor}{elapsed}{view}", flush=True)
                last_failure, last_mode = "", mode

    def close(self) -> None:
        self.stop.set()
        self.refresh.set()
        self._player_ready.set()
        self._video_ready.set()
        for worker in self.workers:
            if worker.ident is not None:
                worker.join()


def _run_live(args: argparse.Namespace) -> int:
    capture = GameWindowCapture(args.game_title, verify_main_ui=True, assets=args.assets)
    session = LiveGuide(args, capture)
    hud = marker = timing_panel = None
    if not args.console:
        from video_guide_hud import GuideHud

        hud = GuideHud()
        try:
            marker = GuideHud(marker_only=True)
            if session.timings is not None:
                timing_panel = GuideHud(timing_only=True)
        except Exception:
            if marker is not None:
                marker.close()
            hud.close()
            raise
        capture.hud_hwnds = tuple(layer.hwnd for layer in (hud, marker, timing_panel) if layer is not None)
        print("Ctrl+Alt+F8 更新攻略目标，Ctrl+Alt+F10 关闭指引", flush=True)
    try:
        session.start()
        last_line = None
        next_render = time.perf_counter()
        next_timing = 0.0
        timing_bounds = None
        while not session.stop.wait(max(0.0, next_render - time.perf_counter())):
            now = time.monotonic()
            player_reading, target_reading = session.player, session.target
            player, target = player_reading.current(now), target_reading.current(now, args.follow_video)
            if player is not None and target is not None and not _same_guide_block(target.match, player.match):
                player = None
            window = capture.foreground_client()
            frame = session.player_frame
            has_minimap = frame.frame is not None and now - frame.captured_at < _CACHE_SECONDS
            if not has_minimap:
                player = None
            if hud is not None:
                refresh, quit_requested = hud.poll()
                if quit_requested:
                    break
                if refresh:
                    session.refresh.set()
                heading = frame.heading if window is not None and has_minimap else None
                note = target_reading.note or ("短时保留上次位置" if player_reading.cached else "")
                if window is not None and target is not None and not has_minimap:
                    title, detail, offset, distance = "游戏画面未检测到小地图", "", None, None
                else:
                    title, detail, offset, distance = _compass_display(
                        player, target, heading, note, target_reading.detail, arrival_distance=args.arrival_distance)
                bounds = window[1] if window is not None else None
                hud.show(bounds, title, detail, offset, distance=distance)
                point = (minimap_screen_target(bounds, player, target)
                         if bounds is not None and has_minimap and player is not None else None)
                marker.show_marker(point, target.camera_heading if target is not None else None)
                if timing_panel is not None and (now >= next_timing or bounds != timing_bounds):
                    sections = session.timings.sections(time.perf_counter(), args) if bounds is not None else ()
                    timing_panel.show_timings(bounds, sections)
                    timing_bounds = bounds
                    next_timing = now + 0.25
            else:
                if window is None:
                    line = f"等待「{args.game_title}」前台主界面"
                elif target is not None and not has_minimap:
                    line = "游戏画面未检测到小地图"
                elif player is None or target is None:
                    line = target_reading.note or "正在重新定位"
                else:
                    line = ("[短时保留] " if player_reading.cached else "") + guidance(
                        player, target, arrival_distance=args.arrival_distance)
                if line != last_line:
                    print(line, flush=True)
                    last_line = line
            next_render = max(next_render + 1 / _hud_fps(args), time.perf_counter())
        if session.error is not None:
            print(session.error, file=sys.stderr)
            return 1
        return 0
    finally:
        for layer in (timing_panel, marker, hud):
            if layer is not None:
                layer.close()
        session.close()


def _run_heading_only(args: argparse.Namespace) -> int:
    capture = GameWindowCapture(args.game_title, verify_main_ui=True, assets=args.assets)
    hud = None
    if not args.console:
        from video_guide_hud import GuideHud

        hud = GuideHud(refresh_enabled=False)
        capture.hud_hwnds = (hud.hwnd,)
        print("原神视角 HUD 已启动，Ctrl+Alt+F10 关闭", flush=True)

    heading, heading_at = None, -math.inf
    next_capture = 0.0
    has_minimap = False
    last_line = None
    try:
        while True:
            started = time.perf_counter()
            if hud is not None and hud.poll()[1]:
                return 0
            window = capture.foreground_client()
            now = time.monotonic()
            if window is None:
                heading, heading_at = None, -math.inf
                has_minimap = False
                next_capture = 0.0
                label, detail = "等待游戏窗口", ""
            else:
                if now >= next_capture:
                    minimap = capture.grab_minimap()
                    has_minimap = minimap is not None
                    heading, heading_at = _update_heading(heading, heading_at, minimap, now)
                    next_capture = now + 0.06
                if heading is None:
                    label, detail = ("正在识别视角", "") if has_minimap else ("等待游戏主界面", "")
                else:
                    direction = _DIRECTIONS[round(heading / 45) % len(_DIRECTIONS)]
                    label, detail = f"{round(heading) % 360}°", f"镜头朝{direction}"
            if hud is not None:
                hud.show(window[1] if window is not None and has_minimap else None, label, detail)
            else:
                line = f"{label}  {detail}".strip()
                if line != last_line:
                    print(line, flush=True)
                    last_line = line
            delay = 0.02 if window is not None else 0.05
            if hud is not None:
                delay = max(0.0, 1 / _hud_fps(args) - (time.perf_counter() - started))
            time.sleep(delay)
    finally:
        if hud is not None:
            hud.close()


def main(argv: list[str] | None = None, *, default_live_frame_mode: str = "canvas") -> int:
    parser = argparse.ArgumentParser(description="由后台攻略视频和原神小地图计算方向")
    parser.add_argument("--assets", type=Path, default=_ASSETS)
    parser.add_argument("--game-image", type=Path, help="用游戏完整客户区截图离线测试")
    parser.add_argument("--game-title", default="原神", help="游戏窗口标题")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--heading-only", action="store_true", help="只显示游戏镜头朝向")
    source.add_argument("--video-image", type=Path, help="用已有攻略帧作为目标")
    source.add_argument("--edge-existing", action="store_true", help="复用原 Edge。远程和唤醒模式需要本地调试授权")
    source.add_argument("--video-url", help="启动普通 Edge 并打开此网址，可复用已打开的匹配标签页")
    parser.add_argument("--follow-video", action="store_true", help="随视频画面刷新攻略目标")
    parser.add_argument("--detailed-hud", action="store_true", help="在实时攻略 HUD 左侧显示玩家和攻略的实测帧率与阶段耗时")
    parser.add_argument("--video-fps", type=float, default=_VIDEO_FPS, help="攻略取帧频率，默认 2 帧/秒")
    parser.add_argument("--player-fps", type=float, default=_PLAYER_FPS, help="玩家小地图取帧频率，默认 10 帧/秒")
    parser.add_argument("--arrival-distance", type=float, default=_ARRIVAL_DISTANCE,
                        help="判定目标附近的距离，单位为地图坐标")
    parser.epilog = "HUD 刷新频率自动取攻略和玩家取帧频率的较大值，再加 5 帧/秒。"
    parser.add_argument("--video-layer-id", action="append", help="攻略小地图候选图块，可重复指定")
    parser.add_argument("--player-layer-id", help="旧版启动参数，实时玩家定位以攻略图块为准")
    parser.add_argument("--edge-port", type=int, default=9222)
    parser.add_argument("--edge-mode", choices=("remote", "wake", "windows"), default="remote",
                        help="remote 支持最小化，wake 读取还原窗口内的视频，windows 无需调试授权")
    parser.add_argument("--video-pattern", help="Edge 攻略筛选正则，Windows 捕获按已打开的窗口标题筛选")
    parser.add_argument("--window-video-rect", type=float, nargs=4, default=FULL_WINDOW,
                        metavar=("X", "Y", "W", "H"), help="Windows 捕获的视频画面位置，按窗口宽高归一化")
    frame_modes = ("canvas", "canvas-fast", "clip")
    if default_live_frame_mode == "canvas-region":
        frame_modes += ("canvas-region",)
    parser.add_argument("--frame-mode", choices=frame_modes,
                        help=f"攻略帧取法，实时模式默认 {default_live_frame_mode}，单帧默认 canvas")
    parser.add_argument("--at", type=float, help="从后台视频指定秒数取目标帧")
    parser.add_argument("--video-circle", type=float, nargs=3, metavar=("X", "Y", "R"),
                        help="小地图圆心和半径，以 640 像素宽的原视频为基准，固定取帧时必填")
    parser.add_argument("--video-inset", type=float, default=_DEFAULT_VIDEO_INSET,
                        help="按 640 像素宽的基准从半径扣除的像素数")
    parser.add_argument("--method", choices=("sift", "template"), default="sift")
    parser.add_argument("--layer-id", help="模板匹配使用的图块 ID")
    parser.add_argument("--console", action="store_true", help="在终端输出实时指引")
    args = parser.parse_args(argv)
    args.frame_mode = args.frame_mode or (default_live_frame_mode if args.follow_video else "canvas")
    if args.edge_mode == "wake" and (not args.edge_existing or args.frame_mode != "canvas-region"):
        parser.error("唤醒模式需要 --edge-existing 和固定区域实时取帧")
    if args.edge_mode == "windows":
        if default_live_frame_mode != "canvas-region":
            parser.error("请用 cpptest/run_hud.py 启动 Windows 捕获模式")
        if (not args.follow_video or args.frame_mode != "canvas-region"
                or args.video_url is not None or args.at is not None):
            parser.error("Windows 捕获需要对已打开的 Edge 使用 --follow-video，无法指定视频地址或进度")
        try:
            args.window_video_rect = window_video_rect(args.window_video_rect)
            if args.video_pattern:
                re.compile(args.video_pattern)
        except (ValueError, re.error) as error:
            parser.error(str(error))
    if args.heading_only and args.game_image is not None:
        parser.error("--heading-only 使用实时游戏窗口")
    if args.follow_video and (args.heading_only or args.video_image is not None or args.game_image is not None):
        parser.error("--follow-video 使用实时游戏窗口及浏览器视频")
    if args.frame_mode == "canvas-region":
        if not args.follow_video or args.video_circle is None:
            parser.error("固定区域取帧需要 --follow-video 和 --video-circle X Y R")
        x, y, radius = args.video_circle
        if (not all(map(math.isfinite, (x, y, radius, args.video_inset)))
                or min(x, y) <= 0 or args.video_inset < 0 or radius <= args.video_inset):
            parser.error("小地图圆参数无效，半径需要大于内缩距离")
        if args.edge_mode == "windows":
            try:
                minimap_region(args.window_video_rect, (x, y, radius - args.video_inset))
            except ValueError as error:
                parser.error(str(error))
    for option, fps in (("--video-fps", args.video_fps), ("--player-fps", args.player_fps)):
        if not math.isfinite(fps) or fps <= 0:
            parser.error(f"{option} 需要正的有限帧率")
    if not math.isfinite(args.arrival_distance) or args.arrival_distance < 0:
        parser.error("--arrival-distance 需要非负的有限距离")
    if args.layer_id and args.method != "template":
        parser.error("--layer-id 适用于 --method template")
    if args.at is not None and (args.at < 0 or not math.isfinite(args.at)):
        parser.error("--at 需要非负的有限秒数")
    if args.video_url:
        url = urlsplit(args.video_url)
        if url.scheme not in ("http", "https") or not url.netloc:
            parser.error("--video-url 需要完整的 HTTP 或 HTTPS 视频地址")
        if args.video_pattern is None:
            args.video_pattern = re.escape(url.netloc + url.path.rstrip("/"))
    if args.video_pattern is None:
        args.video_pattern = r"bilibili\.com"

    cv2.setNumThreads(4)
    cv2.ocl.setUseOpenCL(False)
    try:
        if args.heading_only:
            return _run_heading_only(args)
        if args.game_image is None:
            return _run_live(args)
        video = _read_video_frame(args, args.at)
        localizer = _make_localizer(args)
        if args.video_layer_id:
            target = VideoTemplateMatcher(args.assets, args.video_layer_id).locate_video(video, args)
        else:
            for video_map in _video_minimap_candidates(video, args):
                try:
                    target = _locate_target(localizer, video_map)
                except ValueError:
                    continue
                break
            else:
                raise ValueError("这帧攻略的小地图没有匹配到可信坐标")
        print(f"攻略目标 ({target.x:.2f}, {target.y:.2f}) 层 {target.floor}", flush=True)
        minimap = crop_game_minimap(_read_image(args.game_image))
        if target.match is None:
            raise ValueError("攻略定位未提供图块")
        sample = GuidedPlayerMatcher(args.assets).locate(minimap, target.match, None)
        if sample is None or not _same_guide_block(target.match, sample):
            raise ValueError("游戏截图未在攻略图块内匹配到可信坐标")
        print(guidance(MapPoint.from_match(sample), target, arrival_distance=args.arrival_distance), flush=True)
        return 0
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
