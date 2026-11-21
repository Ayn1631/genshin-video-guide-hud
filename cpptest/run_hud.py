"""Run the existing Windows capture and HUD with the C++ map locator."""

from __future__ import annotations

import argparse
import ctypes as ct
import json
import os
from pathlib import Path
import sys
import time
import weakref

import cv2
import numpy as np

FROZEN = getattr(sys, "frozen", False)
ROOT = Path(sys.executable if FROZEN else __file__).resolve().parent
DEFAULT_LOCAL_MISSES = 3
DEFAULT_REGION_MISSES = 2
sys.path.insert(0, str(ROOT.parent))
import video_map_guide as guide


class NativePosition(ct.Structure):
    _fields_ = [
        ("x", ct.c_double), ("y", ct.c_double), ("image_x", ct.c_double), ("image_y", ct.c_double),
        ("error", ct.c_double), ("similarity", ct.c_double), ("floor", ct.c_int),
        ("method", ct.c_int), ("inliers", ct.c_int), ("matches", ct.c_int),
        ("template_size", ct.c_int), ("layer", ct.c_char * 96),
    ]

    @classmethod
    def from_python(cls, position):
        if isinstance(position, guide.Position):
            return cls(position.x, position.y, position.image_x, position.image_y,
                       position.reprojection_error, 0, position.floor, 1, position.inliers, position.matches)
        if isinstance(position, guide.TemplatePosition):
            return cls(x=position.x, y=position.y, floor=position.floor, method=2, similarity=position.similarity,
                       layer=position.layer_id.encode("utf-8"), template_size=position.template_size)
        return None

    def to_python(self, map_radius=None, scene="Teyvat"):
        if self.method == 1:
            return guide.Position(self.x, self.y, self.floor, self.image_x, self.image_y,
                                  self.inliers, self.matches, self.inliers / self.matches, self.error, map_radius, scene)
        layer = self.layer.decode("utf-8")
        return guide.TemplatePosition(self.x, self.y, self.floor, layer, layer, self.similarity,
                                      self.template_size, map_radius, scene)


class NativeResult(ct.Structure):
    _fields_ = [
        ("position", NativePosition), ("found", ct.c_int), ("phase", ct.c_int), ("failure", ct.c_int),
    ]


Progress = ct.CFUNCTYPE(None, ct.c_int)


class NativeLibrary:
    def __init__(self, path: Path, threads: int, workers: int, *, index_cache: Path | None = None,
                 cache_indexes: bool = True, local_misses: int = 1, region_misses: int = 1,
                 video_method: int = 0) -> None:
        self._dll_dir = os.add_dll_directory(str(path.parent)) if os.name == "nt" else None
        self.dll = dll = ct.CDLL(str(path))
        self.workers = workers
        self.local_misses = local_misses
        self.region_misses = region_misses
        self.video_method = video_method
        self._index_cache = str(index_cache or path.parent / "index-cache").encode("utf-8") if cache_indexes else None
        dll.bgi_configure.argtypes = [ct.c_int]
        dll.bgi_configure.restype = None
        dll.bgi_configure(threads)
        dll.bgi_last_error.argtypes = []
        dll.bgi_last_error.restype = ct.c_char_p
        dll.bgi_open_cached.argtypes = [
            ct.c_char_p, ct.c_int, ct.c_int, ct.c_int, ct.c_bool, ct.c_char_p,
            ct.c_int, ct.c_double, ct.c_double, ct.POINTER(ct.c_double), ct.c_double, ct.c_char_p,
        ]
        dll.bgi_open_cached.restype = ct.c_void_p
        dll.bgi_close.argtypes = [ct.c_void_p]
        dll.bgi_close.restype = None
        self.set_video_retry_limits = getattr(dll, "bgi_set_video_retry_limits", None)
        if self.set_video_retry_limits is not None:
            self.set_video_retry_limits.argtypes = [ct.c_void_p, ct.c_int, ct.c_int]
            self.set_video_retry_limits.restype = ct.c_int
            dll.bgi_video_retry_progress.argtypes = [ct.c_void_p, ct.c_int]
            dll.bgi_video_retry_progress.restype = ct.c_int
        elif (local_misses, region_misses) != (1, 1):
            raise ValueError("当前定位 DLL 不支持分级重试参数，请重新编译 C++ 定位库")
        dll.bgi_locate.argtypes = [
            ct.c_void_p, ct.c_void_p, ct.c_int, ct.c_int, ct.c_size_t,
            ct.c_int, ct.c_int, ct.POINTER(NativePosition), Progress, ct.POINTER(NativeResult),
        ]
        dll.bgi_locate.restype = ct.c_int
        self.locate_with_radius = getattr(dll, "bgi_locate_with_radius", None)
        if self.locate_with_radius is not None:
            self.locate_with_radius.argtypes = [*dll.bgi_locate.argtypes, ct.POINTER(ct.c_double)]
            self.locate_with_radius.restype = ct.c_int
        self.locate_with_scene = getattr(dll, "bgi_locate_with_scene", None)
        if self.locate_with_scene is not None:
            self.locate_with_scene.argtypes = [*dll.bgi_locate.argtypes, ct.POINTER(ct.c_double), ct.c_char_p,
                                               ct.POINTER(ct.c_char), ct.c_size_t]
            self.locate_with_scene.restype = ct.c_int

    def check(self, success: bool) -> None:
        if not success:
            message = self.dll.bgi_last_error().decode("utf-8", errors="replace")
            raise ValueError(message or "C++ map locator failed")

    def open(self, assets, *, method=0, floor=None, layers=(), player=False, circle=None, inset=12.0):
        circle_args = (ct.c_double * 3)(*circle) if circle is not None else None
        handle = self.dll.bgi_open_cached(
            str(assets).encode("utf-8"), method, self.workers, floor or 0, floor is not None,
            ",".join(layers).encode("utf-8"), 9 if player else 15, 0.27 if player else 0.5,
            6.0 if player else 3.0, circle_args, inset, self._index_cache,
        )
        self.check(bool(handle))
        return NativeEngine(self, handle)


class NativeEngine:
    def __init__(self, library: NativeLibrary, handle: int) -> None:
        self.library = library
        self.handle = handle
        self._close = weakref.finalize(self, library.dll.bgi_close, handle)

    def match(self, image: np.ndarray, *, kind: int, search: int = 0, prior=None, notify=None) -> NativeResult:
        if image.dtype != np.uint8 or image.ndim != 3 or image.shape[2] != 3:
            raise ValueError("C++ locator requires a BGR uint8 frame")
        height, width = image.shape[:2]
        if image.strides[2] != 1 or image.strides[1] != 3 or image.strides[0] < width * 3:
            image = np.ascontiguousarray(image)
        anchor = NativePosition.from_python(prior)
        callback = Progress() if notify is None else Progress(lambda phase: notify("region" if phase == 5 else phase == 3))
        result = NativeResult()
        arguments = (
            self.handle, image.ctypes.data, width, height, image.strides[0], kind, search,
            ct.byref(anchor) if anchor is not None else None, callback, ct.byref(result),
        )
        radius = ct.c_double()
        scene = ct.create_string_buffer(32)
        if self.library.locate_with_scene is not None:
            prior_scene = prior.scene.encode("utf-8") if anchor is not None else None
            status = self.library.locate_with_scene(*arguments, ct.byref(radius), prior_scene, scene, len(scene))
        elif self.library.locate_with_radius is not None:
            status = self.library.locate_with_radius(*arguments, ct.byref(radius))
        else:
            status = self.library.dll.bgi_locate(*arguments)
        self.library.check(status == 0)
        result.map_radius = radius.value or None
        result.scene = scene.value.decode("utf-8") or "Teyvat"
        return result


PHASE_NAMES = ("画面检查", "局部匹配", "全图匹配", "全图重试", "同帧复用", "国家或独立地图匹配",
               "等待区域搜索", "等待全图重试", "等待再次全图匹配")
FAILURE_NAMES = (
    "", "攻略取到的是黑屏帧", "未检测到小地图圆", "SIFT 特征未通过校验",
    "找到疑似小地图圆，全图筛选无候选", "找到疑似小地图圆，地图粗匹配未通过",
    "地图粗匹配通过，精匹配未通过", "地图精匹配有多个相近坐标", "攻略画面未检测到小地图",
)


class NativeVideoMatcher:
    maps = ()

    def __init__(self, library: NativeLibrary, assets, layers=None, *, diagnostics_dir=None, share_atlas=None) -> None:
        self.library = library
        self.assets = assets
        self.layers = layers or ()
        self.mode = PHASE_NAMES[0]
        self._engine: NativeEngine | None = None
        self._last_retried = False
        self._camera_heading: float | None = None
        self._diagnostics = ROOT / "output" if diagnostics_dir is not None else None
        self._last_report = None
        self._reported_at = 0.0

    def _get_engine(self, args=None) -> NativeEngine:
        if self._engine is None:
            self._engine = self.library.open(self.assets, method=self.library.video_method, layers=self.layers,
                                             circle=args.video_circle if args is not None else None,
                                             inset=args.video_inset if args is not None else 12.0)
            if self.library.set_video_retry_limits is not None:
                self.library.check(self.library.set_video_retry_limits(
                    self._engine.handle, self.library.local_misses, self.library.region_misses) == 0)
        return self._engine

    def locate(self, minimap):
        result = self._get_engine().match(minimap, kind=1, search=1)
        return result.position.to_python(result.map_radius, result.scene) if result.found else None

    def locate_video(self, frame, args, *, playhead=None, on_global=None):
        fixed = args.frame_mode == "canvas-region"
        engine = self._get_engine(None if fixed else args)
        result = engine.match(frame, kind=3 if fixed else 0, notify=on_global)
        self.mode = PHASE_NAMES[result.phase]
        if result.found:
            self._last_retried = False
            if fixed and result.phase != 4:
                self._camera_heading = guide.camera_heading(frame)
            sample = result.position.to_python(result.map_radius, result.scene)
            return guide.MapPoint.from_match(sample, heading=self._camera_heading if fixed else None)
        if result.phase != 4:
            self._last_retried = result.phase == 3
        if result.phase >= 6:
            count = self.library.dll.bgi_video_retry_progress(engine.handle, result.phase)
            limit = self.library.region_misses if result.phase == 7 else self.library.local_misses
            unit = "区域失败" if result.phase == 7 else "新帧失败"
            raise guide.VideoMatchError(f"{self.mode} · {unit} {count}/{limit}")
        reason = FAILURE_NAMES[result.failure]
        now = time.monotonic()
        if self._diagnostics is not None and (self._last_report != reason or now - self._reported_at > 15):
            self._last_report, self._reported_at = reason, now
            self._write_report(frame, playhead, reason, fixed)
        raise guide.VideoMatchError(reason, retried=self._last_retried)

    def _write_report(self, frame, playhead, reason, fixed):
        try:
            self._diagnostics.mkdir(parents=True, exist_ok=True)
            encoded, png = cv2.imencode(".png", frame)
            if not encoded:
                raise OSError("攻略帧无法编码")
            (self._diagnostics / "guide-video-diagnostic.png").write_bytes(png.tobytes())
            report = {"status": reason, "match_mode": self.mode, "time": playhead,
                      "frame_size": [frame.shape[1], frame.shape[0]], "backend": "C++",
                      "frame_kind": "minimap" if fixed else "video"}
            (self._diagnostics / "guide-video-diagnostic.json").write_text(
                json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        except (OSError, cv2.error) as error:
            print(f"无法保存攻略诊断帧：{error}", file=sys.stderr, flush=True)


class NativePlayerLocator:
    def __init__(self, engine: NativeEngine) -> None:
        self.engine = engine

    def locate(self, minimap, *, prior=None):
        if min(minimap.shape[:2]) < 100:
            minimap = cv2.resize(minimap, (212, 212), interpolation=cv2.INTER_LINEAR)
        result = self.engine.match(minimap, kind=1, search=0 if prior is not None else 1, prior=prior)
        return result.position.to_python(result.map_radius, result.scene) if result.found else None


class NativeGuidedPlayerMatcher:
    def __init__(self, library: NativeLibrary, assets: Path) -> None:
        self.engine = library.open(assets, player=True)

    def locate(self, minimap, anchor, previous):
        if min(minimap.shape[:2]) < 100:
            minimap = cv2.resize(minimap, (212, 212), interpolation=cv2.INTER_LINEAR)
        tile_only = isinstance(anchor, guide.TemplatePosition) and previous is None
        prior = (previous or anchor) if isinstance(anchor, guide.TemplatePosition) else anchor
        result = self.engine.match(minimap, kind=1, search=3 if tile_only else 2, prior=prior)
        return result.position.to_python(result.map_radius, result.scene) if result.found else None


def run(argv: list[str]) -> int:
    options = argparse.ArgumentParser(add_help=False)
    native_build = ROOT / "build-native" / "bgi_locator.dll"
    visual_studio_build = ROOT / "build" / "Release" / "bgi_locator.dll"
    options.add_argument("--native-dll", type=Path, default=native_build if native_build.is_file() else visual_studio_build)
    options.add_argument("--opencv-threads", type=int, default=2)
    options.add_argument("--map-workers", type=int, default=4)
    options.add_argument("--local-misses", type=int, default=DEFAULT_LOCAL_MISSES, help="区域搜索前的连续局部失败帧数")
    options.add_argument("--region-misses", type=int, default=DEFAULT_REGION_MISSES, help="全图重试前的连续区域失败次数")
    options.add_argument("--video-method", choices=("auto", "sift", "template"), default="auto")
    options.add_argument("--index-cache-dir", type=Path)
    options.add_argument("--no-index-cache", action="store_true")
    args, remaining = options.parse_known_args(argv)
    if not 1 <= args.opencv_threads <= 64 or not 1 <= args.map_workers <= 32:
        options.error("无效的 OpenCV 线程数或图块并发数")
    if not 1 <= args.local_misses <= 120 or not 1 <= args.region_misses <= 120:
        options.error("两级连续失败门槛须为 1 至 120")
    runtime = NativeLibrary(args.native_dll.resolve(), args.opencv_threads, args.map_workers,
                            index_cache=args.index_cache_dir, cache_indexes=not args.no_index_cache,
                            local_misses=args.local_misses, region_misses=args.region_misses,
                            video_method=("auto", "sift", "template").index(args.video_method))

    old_video = guide.VideoTemplateMatcher
    old_guided = guide.GuidedPlayerMatcher
    old_player = guide._make_localizer

    def create_player(config):
        return NativePlayerLocator(runtime.open(config.assets, method=2 if config.method == "template" else 0,
                                                player=True, layers=[config.layer_id] if config.layer_id else ()))

    try:
        guide.VideoTemplateMatcher = lambda *a, **kw: NativeVideoMatcher(runtime, *a, **kw)
        guide.GuidedPlayerMatcher = lambda assets: NativeGuidedPlayerMatcher(runtime, assets)
        guide._make_localizer = create_player
        print(f"攻略定位后端 C++，图块并发 {args.map_workers}，OpenCV 线程 {args.opencv_threads}", flush=True)
        print(f"攻略重试门槛 局部 {args.local_misses} 帧，区域 {args.region_misses} 次", flush=True)
        return guide.main(remaining, default_live_frame_mode="canvas-region")
    finally:
        guide.VideoTemplateMatcher = old_video
        guide.GuidedPlayerMatcher = old_guided
        guide._make_localizer = old_player


if __name__ == "__main__":
    if FROZEN:
        for stream in (sys.stdout, sys.stderr):
            if stream is not None:
                stream.reconfigure(encoding="utf-8", line_buffering=True)
    try:
        if sys.argv[1:] == ["--settings"] or (FROZEN and len(sys.argv) == 1):
            from guide_settings import main

            raise SystemExit(main())
        raise SystemExit(run(sys.argv[1:]))
    except (OSError, ValueError) as failure:
        print(str(failure), file=sys.stderr)
        raise SystemExit(1)
