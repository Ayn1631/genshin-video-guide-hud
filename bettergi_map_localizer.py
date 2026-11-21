# -*- coding: utf-8 -*-
"""BetterGI 小地图定位（独立 Python/OpenCV 适配实现）。

输入: cv2.imread() 得到的 BGR ndarray（建议只截取小地图）。
输出: BetterGI 路径追踪使用的提瓦特二维坐标 (x, y)，定位失败则返回 None。

依赖: pip install opencv-python numpy
资源: SIFT 使用预计算特征，模板匹配使用地图图块及其 JSON 清单。

重要: 此代码是对 BetterGI 数据格式和坐标规则的独立 Python 适配，
并非 BetterGI 官方原始定位模块，不能保证对每种地图层、缩放和 UI 都等效。
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import json
import math
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path
from threading import Lock
from typing import Optional, Sequence

import cv2
import numpy as np

# 依据 BetterGI 主分支 TeyvatMap.cs 和 SceneBaseMap.cs 的地图参数。
# GameMapLeftCols=15, GameMapUpRows=11, TeyvatMapImageBlockWidth=2048。
# 实际图像原点 ((15+1)*2048, (11+1)*2048)，单位缩放=2048/1024=2。
TEYVAT_ORIGIN = (32768.0, 24576.0)
TEYVAT_SCALE = 2.0

# OpenCV cv::KeyPoint 在 BetterGI 当前 Windows OpenCvSharp 格式中的常见布局：
# 5 个 float32（x,y,size,angle,response）+ 2 个 int32（octave,class_id）= 28 bytes。
# 资源升级导致的布局变化，加载时将检查文件长度与描述子行数，不进行错误猜测。
_KP28 = np.dtype([
    ("x", "<f4"), ("y", "<f4"), ("size", "<f4"),
    ("angle", "<f4"), ("response", "<f4"),
    ("octave", "<i4"), ("class_id", "<i4"),
])


@dataclass(frozen=True)
class Position:
    x: float                   # BetterGI 地图 X
    y: float                   # BetterGI 地图 Y（不是高度）
    floor: int                 # 地图层级
    image_x: float             # 2048 地图全局图像像素 X
    image_y: float             # 2048 地图全局图像像素 Y
    inliers: int               # 几何一致匹配数
    matches: int               # 特征筛选后匹配数
    inlier_ratio: float        # 几何一致率，不是统计学上的概率
    reprojection_error: float  # 匹配重投影误差，单位是地图大图像素
    map_radius: float | None = None  # 小地图半径，单位与 x、y 相同
    scene: str = "Teyvat"


@dataclass
class _Layer:
    floor: int
    xy: np.ndarray
    descriptors: np.ndarray
    matcher: Optional[cv2.FlannBasedMatcher] = None
    matcher_lock: Lock = field(default_factory=Lock, repr=False)


def _exclude_map_icons(image: np.ndarray, mask: np.ndarray, radius: float) -> None:
    """排除高亮和中性灰图标，参考 BetterGI 的 MaskCalculator。"""
    if image.ndim != 3:
        return
    blue, green, red, *_ = cv2.split(image)
    brightest = cv2.max(cv2.max(blue, green), red).astype(np.float32)
    darkest = cv2.min(cv2.min(blue, green), red).astype(np.float32)
    spread = brightest - darkest
    if not np.any(spread):
        return
    gray_icons = (spread == 0) & (brightest >= 50) & (brightest <= 127)
    normalized = brightest / (10 + np.minimum(spread, (255 - brightest) / 6))
    icons = (gray_icons | (normalized > 20)).astype(np.uint8)
    size = 2 * max(1, round(radius * 0.025)) + 1
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (size, size))
    icons = cv2.morphologyEx(cv2.dilate(icons, kernel), cv2.MORPH_CLOSE, kernel)
    mask[icons != 0] = 0


class BetterGIMapLocalizer:
    """使用 BetterGI 预计算 SIFT 资源定位 cv2 图像。

    参数:
      assets_dir: BetterGI 安装目录中的 Assets/Map/Teyvat 路径。
      floor: 默认 0（地表），None 则匹配所有存在的层；多层搜索更慢。
      min_inliers: RANSAC 至少需要的真实内点数量。
      ratio: Lowe 的 KNN 最近邻比率阈值，默认 0.73。
      local_radius: 连续帧局部搜索半径，单位为游戏地图坐标。
    """

    def __init__(
        self,
        assets_dir: str | Path,
        floor: int | None = 0,
        min_inliers: int = 9,
        ratio: float = 0.73,
        local_radius: float = 350.0,
        min_inlier_ratio: float = 0.27,
        share_atlas: BetterGIMapLocalizer | None = None,
    ) -> None:
        if not hasattr(cv2, "SIFT_create"):
            raise RuntimeError("当前 OpenCV 无 SIFT_create，请安装新版 opencv-python")
        if not (0.5 <= ratio <= 0.95):
            raise ValueError("ratio 应位于 [0.5, 0.95]")
        if min_inliers < 4:
            raise ValueError("min_inliers 不能小于 4")

        self.assets_dir = Path(assets_dir)
        self.sift = cv2.SIFT_create(nfeatures=2500, contrastThreshold=0.03)
        self.ratio = ratio
        self.min_inliers = min_inliers
        self.min_inlier_ratio = min_inlier_ratio
        self.local_radius = local_radius
        self._local_matcher = cv2.BFMatcher(cv2.NORM_L2)
        self.last_position: Optional[Position] = None
        shared = {}
        if share_atlas is not None and share_atlas.assets_dir == self.assets_dir:
            shared = {layer.floor: layer for layer in share_atlas.layers}
        self.layers = self._load_layers(floor, shared)

    def _load_layers(self, floor: int | None, shared: dict[int, _Layer]) -> list[_Layer]:
        if not self.assets_dir.is_dir():
            raise FileNotFoundError(f"BetterGI 地图资源目录不存在: {self.assets_dir}")

        candidates = sorted(self.assets_dir.glob("Teyvat_*_2048_SIFT.kp.bin"))
        if floor is not None:
            candidates = [p for p in candidates if p.name == f"Teyvat_{floor}_2048_SIFT.kp.bin"]
        if not candidates:
            raise FileNotFoundError(
                "缺少 BetterGI SIFT 地图文件。预期有 Teyvat_0_2048_SIFT.kp.bin "
                "和 Teyvat_0_2048_SIFT.mat.png；注意只有 webp 图片是不够的。"
            )

        result = []
        for kp_path in candidates:
            prefix = kp_path.name[:-len(".kp.bin")]
            level = int(prefix.split("_")[1])
            if level in shared:
                result.append(shared[level])
                continue
            png_path = self.assets_dir / (prefix + ".mat.png")
            if not png_path.is_file():
                raise FileNotFoundError(f"描述子文件缺失: {png_path}")

            data = np.fromfile(str(kp_path), dtype=_KP28)
            desc = cv2.imdecode(np.fromfile(str(png_path), dtype=np.uint8), cv2.IMREAD_GRAYSCALE)
            if desc is None:
                raise ValueError(f"不能读取描述子图片: {png_path}")
            if len(data) == 0 or desc.ndim != 2 or desc.shape != (len(data), 128):
                raise ValueError(
                    f"地图特征格式不匹配: {kp_path.name} 有 {len(data)} 个点，"
                    f"描述子形状为 {desc.shape}，预期 ({len(data)},128)。"
                    "可能地图资源版本不同或 KeyPoint 结构布局发生变化。"
                )

            xy = np.column_stack((data["x"], data["y"])).astype(np.float32)
            if not np.isfinite(xy).all():
                raise ValueError(f"地图关键点含非有限坐标: {kp_path}")
            # BetterGI 的 mat.png 是 8 位 SIFT 描述子图片，匹配时要转成 CV_32F。
            descriptors = np.ascontiguousarray(desc, dtype=np.float32)
            result.append(_Layer(level, xy, descriptors))

        return result

    @staticmethod
    def _prepare_query(image: np.ndarray, center: Optional[Sequence[float]]):
        if not isinstance(image, np.ndarray) or image.dtype != np.uint8:
            raise TypeError("image 必须是 uint8 的 cv2 ndarray（BGR、BGRA 或灰度）")
        if image.ndim == 2:
            gray = image
        elif image.ndim == 3 and image.shape[2] == 3:
            gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        elif image.ndim == 3 and image.shape[2] == 4:
            gray = cv2.cvtColor(image, cv2.COLOR_BGRA2GRAY)
        else:
            raise ValueError("image 格式应为灰度、BGR 或 BGRA")
        h, w = gray.shape
        if min(h, w) < 100:
            raise ValueError("小地图太小（宽高至少 100 像素）")

        if center is None:
            cx, cy = (w - 1) / 2, (h - 1) / 2
        else:
            if len(center) != 2:
                raise ValueError("center 应是 (x,y) 截图内玩家图标坐标")
            cx, cy = map(float, center)
        if not (0 <= cx < w and 0 <= cy < h):
            raise ValueError("center 不在输入图片范围内")
        r = min(cx, cy, w - 1 - cx, h - 1 - cy)
        if r < 40:
            raise ValueError("人物中心离图片边缘过近，无法构建有效小地图遮罩")

        # 屏蔽四个方向文字、圆形边框以及中央的玩家箭头。
        # 不自动对小地图旋转做校正：SIFT 对旋转有一定鲁棒性，但与 BetterGI 的
        # MaskCalculator+CameraOrientationCalculator 预处理并不等价。
        mask = np.zeros((h, w), np.uint8)
        cv2.circle(mask, (round(cx), round(cy)), round(r * 0.88), 255, -1)
        cv2.circle(mask, (round(cx), round(cy)), max(12, round(r * 0.14)), 0, -1)
        _exclude_map_icons(image, mask, r)
        return gray, mask, (cx, cy)

    def _train_matcher(self, layer: _Layer) -> cv2.FlannBasedMatcher:
        if layer.matcher is None:
            # FLANN K-D tree 用于高维 SIFT 描述子近邻检索。
            matcher = cv2.FlannBasedMatcher(dict(algorithm=1, trees=5), dict(checks=96))
            matcher.add([layer.descriptors])
            matcher.train()
            layer.matcher = matcher
        return layer.matcher

    def _match_layer(
        self,
        layer: _Layer,
        query_xy: np.ndarray,
        query_desc: np.ndarray,
        player_center: tuple[float, float],
        query_radius: float,
        local_first: bool,
        search_global: bool,
    ) -> Optional[Position]:
        # 已知上一帧时只比较附近范围，显著减少连续定位耗时。
        # 只有局部搜索不成功时才回退到整张特征地图。
        tests: list[tuple[np.ndarray, np.ndarray, bool]] = []
        prev = self.last_position
        if local_first and prev is not None and prev.floor == layer.floor:
            px, py = prev.image_x, prev.image_y
            radius_px = self.local_radius * TEYVAT_SCALE
            ids = np.flatnonzero(
                (np.abs(layer.xy[:, 0] - px) <= radius_px)
                & (np.abs(layer.xy[:, 1] - py) <= radius_px)
            )
            if len(ids) >= 16:
                tests.append((ids, layer.descriptors[ids], True))
        if search_global:
            tests.append((np.empty(0, dtype=np.int64), layer.descriptors, False))

        for idx, train_desc, is_local in tests:
            if is_local:
                # 局部特征较少，直接找最近邻可避开逐帧建索引。
                pairs = self._local_matcher.knnMatch(query_desc, train_desc, k=2)
            else:
                with layer.matcher_lock:
                    matcher = self._train_matcher(layer)
                    pairs = matcher.knnMatch(query_desc, k=2)

            good = []
            used_train = set()
            for pair in pairs:
                if len(pair) < 2:
                    continue
                a, b = pair
                if a.distance >= self.ratio * b.distance:
                    continue
                train_id = int(idx[a.trainIdx]) if is_local else a.trainIdx
                if train_id in used_train:
                    continue
                used_train.add(train_id)
                good.append((a.queryIdx, train_id))
            if len(good) < self.min_inliers:
                continue

            qidx = np.asarray([i for i, _ in good], dtype=np.int32)
            tidx = np.asarray([j for _, j in good], dtype=np.int32)
            qpts, mpts = query_xy[qidx], layer.xy[tidx]

            # SIFT 对旋转/尺度较稳，估计仅含旋转、平移和等比缩放的仿射变换。
            # 此变换把小地图截图坐标转换到 BetterGI 的全图 2048 像素坐标。
            affine, inlier_mask = cv2.estimateAffinePartial2D(
                qpts, mpts, method=cv2.RANSAC,
                ransacReprojThreshold=7.0, maxIters=5000,
                confidence=0.995, refineIters=12,
            )
            if affine is None or inlier_mask is None:
                continue
            ok = inlier_mask.ravel().astype(bool)
            n_inliers = int(ok.sum())
            share = n_inliers / len(good)
            if n_inliers < self.min_inliers or share < self.min_inlier_ratio:
                continue

            # 检查空间分布，避免图标/文字引起的密集误匹配。
            if np.ptp(qpts[ok], axis=0).min() < 22:
                continue
            A = affine[:, :2]
            scale = math.sqrt(float(A[0, 0] ** 2 + A[1, 0] ** 2))
            if not math.isfinite(scale) or not (0.2 <= scale <= 30.0):
                continue
            predicted = (qpts[ok] @ A.T) + affine[:, 2]
            rms = float(np.sqrt(np.mean(np.sum((predicted - mpts[ok]) ** 2, axis=1))))
            if not math.isfinite(rms) or rms > 6.0:
                continue

            image_xy = A @ np.asarray(player_center, np.float64) + affine[:, 2]
            mx, my = map(float, image_xy)
            if not (math.isfinite(mx) and math.isfinite(my)):
                continue
            # 使用源码中完整的 Teyvat 2048 地图校准，而不是截图像素坐标。
            x = (TEYVAT_ORIGIN[0] - mx) / TEYVAT_SCALE
            y = (TEYVAT_ORIGIN[1] - my) / TEYVAT_SCALE
            return Position(x, y, layer.floor, mx, my,
                            n_inliers, len(good), share, rms, query_radius * scale / TEYVAT_SCALE)
        return None

    def locate(
        self,
        image: np.ndarray,
        *,
        center: Optional[Sequence[float]] = None,
        crop: Optional[Sequence[int]] = None,
        local_first: bool = True,
        search_global: bool = True,
    ) -> Optional[Position]:
        """由小地图 cv2 图像计算 BetterGI 坐标。

        image: BGR ndarray（也接受灰度/BGRA）。
        center: 小地图玩家中心在「裁剪后图片」中的 (x,y)，默认几何中心。
        crop: 如果传入整张游戏截图，给出小地图 (left,top,width,height)。
        local_first: 已经有上次坐标时优先局部匹配。
        search_global: 允许匹配整张特征地图。设为 False 时只检查上次坐标附近。
        返回 Position 或 None；绝不把失败/像素坐标伪装为地图坐标。
        """
        if crop is not None:
            if len(crop) != 4:
                raise ValueError("crop 格式应是 (x,y,width,height)")
            x, y, w, h = map(int, crop)
            if x < 0 or y < 0 or w < 1 or h < 1 or y + h > image.shape[0] or x + w > image.shape[1]:
                raise ValueError("crop 超出图片范围")
            image = image[y:y + h, x:x + w]
        gray, mask, player_center = self._prepare_query(image, center)
        kps, descriptors = self.sift.detectAndCompute(gray, mask)
        if descriptors is None or len(kps) < self.min_inliers:
            return None
        query_xy = np.asarray([kp.pt for kp in kps], np.float32)
        descriptors = np.ascontiguousarray(descriptors, np.float32)
        cx, cy = player_center
        query_radius = min(cx, cy, gray.shape[1] - 1 - cx, gray.shape[0] - 1 - cy)

        guesses = []
        for layer in self.layers:
            result = self._match_layer(layer, query_xy, descriptors, player_center, query_radius,
                                       local_first, search_global)
            if result is not None:
                guesses.append(result)
        if not guesses:
            return None
        # 多地图层时优先选择稳定的内点数、几何误差。
        best = max(guesses, key=lambda p: (p.inliers, p.inlier_ratio, -p.reprojection_error))
        self.last_position = best
        return best

    def get_map_coords(self, image: np.ndarray, **kwargs) -> Optional[tuple[float, float]]:
        """仅返回用户最关心的 (x,y)，匹配失败则返回 None。"""
        result = self.locate(image, **kwargs)
        return None if result is None else (result.x, result.y)

    def reset(self) -> None:
        """传送后可重置历史坐标，避免使用旧位置局部匹配。"""
        self.last_position = None


@dataclass(frozen=True)
class TemplatePosition:
    x: float
    y: float
    floor: int
    layer_id: str
    name: str
    similarity: float
    template_size: int = 0
    map_radius: float | None = None
    scene: str = "Teyvat"


@dataclass
class TemplateMatchDiagnostics:
    atlas_candidates: int | None = None
    atlas_best: tuple[str, float] | None = None
    coarse_peaks: int = 0
    fine_edge: float | None = None
    fine_score: float | None = None
    ambiguous: tuple[TemplatePosition, TemplatePosition] | None = None


@dataclass(frozen=True)
class _TemplateTile:
    layer_id: str
    name: str
    floor: int
    left: float
    top: float
    scale: float
    coarse: Path
    fine: Path


def _read_gray(path: Path) -> np.ndarray:
    image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_GRAYSCALE)
    if image is None:
        raise ValueError(f"无法读取地图图块 {path}")
    return image


@lru_cache(maxsize=2)
def _read_fine_tile(path: Path) -> np.ndarray:
    return _read_gray(path)


def _texture(gray: np.ndarray) -> np.ndarray:
    smoothed = cv2.GaussianBlur(gray, (3, 3), 0.6).astype(np.float32)
    return smoothed - cv2.GaussianBlur(smoothed, (0, 0), 2.0)


def _resize_template(gray: np.ndarray, mask: np.ndarray, size: int):
    patch = cv2.resize(gray, (size, size), interpolation=cv2.INTER_AREA)
    valid = cv2.resize(mask, (size, size), interpolation=cv2.INTER_AREA) >= 0.99
    valid = valid.astype(np.uint8)
    detail = cv2.erode(valid, np.ones((3, 3), np.uint8))
    return patch, valid, _texture(patch), detail


def _texture_peak(image: np.ndarray, template: np.ndarray, mask: np.ndarray) -> tuple[float, tuple[int, int]]:
    scores = cv2.matchTemplate(image, template, cv2.TM_CCORR_NORMED, mask=mask)
    # 纯平坦区域可能导致归一化分母为零。筛掉非有限值和超出范围的分数后再取峰值。
    valid = cv2.inRange(scores, -1.001, 1.001)
    _, score, _, location = cv2.minMaxLoc(scores, mask=valid)
    return (score, location) if location[0] >= 0 else (-1.0, (-1, -1))


def _coarse_queries(gray: np.ndarray, mask: np.ndarray, sizes: tuple[int, ...], coverage: float):
    for width in sizes:
        _, _, detail, valid = _resize_template(gray, mask, width)
        if cv2.countNonZero(valid) >= width * width * coverage:
            yield width, detail, valid


class BetterGITemplateLocalizer:
    """用本地模板地图定位北朝上的小地图。默认搜索清单中的所有楼层。"""

    def __init__(self, assets_dir: str | Path, floor: int | None = None,
                 layer_id: str | None = None, *, use_pyramid: bool = False) -> None:
        assets = Path(assets_dir)
        tiles = {}
        for manifest in sorted(assets.glob("*.json")):
            for item in json.loads(manifest.read_text(encoding="utf-8-sig")):
                name = item["LayerId"]
                level = item["Floor"]
                if (layer_id is not None and name != layer_id) or (floor is not None and level != floor):
                    continue
                coarse = assets / f"{name}_color.webp"
                extension = ".png" if item.get("IsOverSize") else ".webp"
                fine = assets / f"{name}_gray{extension}"
                if coarse.is_file() and fine.is_file():
                    tiles[name] = _TemplateTile(
                        name, item.get("Name", name), level, item["Left"], item["Top"], item["Scale"], coarse, fine
                    )
        if not tiles:
            raise FileNotFoundError(f"没有找到符合条件的地图模板 {assets}")
        self.tiles = list(tiles.values())
        self._use_pyramid = use_pyramid
        self._pyramid: dict[Path, np.ndarray] = {}

    def locate(self, image: np.ndarray, *, center: Optional[Sequence[float]] = None,
               near: TemplatePosition | None = None, search_global: bool = True,
               diagnostics: TemplateMatchDiagnostics | None = None) -> Optional[TemplatePosition]:
        gray, mask, (cx, cy) = BetterGIMapLocalizer._prepare_query(image, center)
        radius = min(cx, cy, gray.shape[1] - 1 - cx, gray.shape[0] - 1 - cy)
        size = 2 * round(radius * 0.92) + 1
        gray = cv2.getRectSubPix(gray, (size, size), (cx, cy))
        mask = cv2.getRectSubPix(mask, (size, size), (cx, cy)).astype(np.float32) / 255
        # 临近图标的像素会参与纹理滤波，额外缩小中心有效区。
        cv2.circle(mask, (size // 2, size // 2), round(radius * 0.17), 0, -1)
        if near is not None and near.template_size > 18:
            tile = next((t for t in self.tiles if t.layer_id == near.layer_id), None)
            if tile is not None:
                cx = round((tile.left - near.x) * tile.scale)
                cy = round((tile.top - near.y) * tile.scale)
                nominal = near.template_size
                candidate = self._match_fine(tile, gray, mask, cx, cy, nominal, range(nominal - 4, nominal + 5, 2),
                                             query_radius=radius)
                if candidate is not None and candidate.similarity >= max(0.73, near.similarity - 0.04):
                    return candidate

        if not search_global:
            return None

        sizes = (30, 34, 38, 42, 46, 49, 52, 55, 58, 62, 67, 74, 82, 92, 104, 118)
        if self._use_pyramid and len(self.tiles) > 12:
            ranked = self._rank_tiles(gray, mask, sizes, diagnostics)
            if ranked:
                # 全图低分辨率筛选之后，在最相似的图块上恢复原始尺度精度。
                shortlist = ranked[:12]
                first_pass = diagnostics if diagnostics is not None else TemplateMatchDiagnostics()
                candidate = self._search_tiles(shortlist, gray, mask, sizes, radius, first_pass)
                if candidate is not None or first_pass.ambiguous is not None:
                    # 更低优先级的图块不能覆盖已发现的坐标歧义。
                    return candidate
                return self._search_tiles(ranked[12:36], gray, mask, sizes, radius, diagnostics)
            return None
        return self._search_tiles(self.tiles, gray, mask, sizes, radius, diagnostics)

    def _rank_tiles(self, gray: np.ndarray, mask: np.ndarray, sizes: tuple[int, ...],
                    diagnostics: TemplateMatchDiagnostics | None = None) -> list[_TemplateTile]:
        reduced = tuple(dict.fromkeys(round(width / 2) for width in sizes))
        templates = list(_coarse_queries(gray, mask, reduced, 0.12))

        def rank(tile: _TemplateTile) -> float:
            reference = self._pyramid.get(tile.coarse)
            if reference is None:
                coarse = _read_gray(tile.coarse)
                height, width = coarse.shape
                smaller = cv2.resize(coarse, (max(1, round(width / 2)), max(1, round(height / 2))),
                                     interpolation=cv2.INTER_AREA)
                reference = _texture(smaller)
                self._pyramid[tile.coarse] = reference
            return max((_texture_peak(reference, query, valid)[0]
                        for width, query, valid in templates if width <= min(reference.shape)), default=-1.0)

        with ThreadPoolExecutor(max_workers=2) as workers:
            scores = list(workers.map(rank, self.tiles))
        candidates = []
        for tile, best in zip(self.tiles, scores):
            if diagnostics is not None and (diagnostics.atlas_best is None or best > diagnostics.atlas_best[1]):
                diagnostics.atlas_best = tile.layer_id, best
            if best >= 0.53:
                candidates.append((best, tile))
        if diagnostics is not None:
            diagnostics.atlas_candidates = len(candidates)
        return [tile for _, tile in sorted(candidates, key=lambda item: item[0], reverse=True)]

    def _search_tiles(self, tiles: list[_TemplateTile], gray: np.ndarray, mask: np.ndarray,
                      sizes: tuple[int, ...], query_radius: float,
                      diagnostics: TemplateMatchDiagnostics | None = None) -> Optional[TemplatePosition]:
        templates = list(_coarse_queries(gray, mask, sizes, 0.15))
        if not tiles:
            return None

        def tile_peaks(tile: _TemplateTile):
            reference = _texture(_read_gray(tile.coarse))
            peaks = []
            for width, query, valid in templates:
                if width > min(reference.shape):
                    continue
                score, (px, py) = _texture_peak(reference, query, valid)
                if score >= 0.57:
                    peaks.append((score, tile, width, px, py))
            return peaks

        if len(tiles) == 1:
            peaks = tile_peaks(tiles[0])
        else:
            with ThreadPoolExecutor(max_workers=2) as workers:
                peaks = [peak for group in workers.map(tile_peaks, tiles) for peak in group]
        if diagnostics is not None:
            diagnostics.coarse_peaks += len(peaks)
        results = []
        for _, tile, width, px, py in sorted(peaks, key=lambda row: row[0], reverse=True)[:18]:
            candidate = self._refine(tile, gray, mask, width, px, py, query_radius, diagnostics)
            if candidate is not None:
                results.append(candidate)
        if not results:
            return None
        best = max(results, key=lambda result: result.similarity)
        for result in results:
            if best.similarity - result.similarity < 0.04 and (
                best.floor != result.floor or math.hypot(best.x - result.x, best.y - result.y) > 20
            ):
                if diagnostics is not None:
                    diagnostics.ambiguous = best, result
                return None
        return best

    @staticmethod
    def _refine(tile: _TemplateTile, gray: np.ndarray, mask: np.ndarray, width: int,
                px: int, py: int, query_radius: float,
                diagnostics: TemplateMatchDiagnostics | None = None) -> Optional[TemplatePosition]:
        # BetterGI 的粗图按 1/5 比例保存。
        zoom = 5
        nominal = width * zoom
        cx, cy = round((px + width / 2) * zoom), round((py + width / 2) * zoom)
        return BetterGITemplateLocalizer._match_fine(tile, gray, mask, cx, cy, nominal,
                                                     range(nominal - 18, nominal + 19, 2), diagnostics,
                                                     query_radius=query_radius)

    @staticmethod
    def _match_fine(tile: _TemplateTile, gray: np.ndarray, mask: np.ndarray,
                    cx: int, cy: int, nominal: int, sizes: range,
                    diagnostics: TemplateMatchDiagnostics | None = None, *,
                    query_radius: float) -> Optional[TemplatePosition]:
        fine = _read_fine_tile(tile.fine)
        extent = nominal // 2 + max(18, round(nominal * 0.14))
        left, top = max(0, cx - extent), max(0, cy - extent)
        ref = fine[top:cy + extent, left:cx + extent]
        if not ref.size:
            return None
        texture = _texture(ref)
        best = None
        for size in sizes:
            if size > min(ref.shape):
                continue
            patch, valid, detail, detail_mask = _resize_template(gray, mask, size)
            edge_score, (x, y) = _texture_peak(texture, detail, detail_mask)
            if diagnostics is not None and (diagnostics.fine_edge is None or edge_score > diagnostics.fine_edge):
                diagnostics.fine_edge = edge_score
            if edge_score < 0.62:
                continue
            usable = valid != 0
            p = patch[usable]
            q = ref[y:y + size, x:x + size][usable]
            if min(p.std(), q.std()) < 1:
                continue
            level_score = float(np.corrcoef(p, q)[0, 1])
            score = 0.55 * level_score + 0.45 * edge_score
            if diagnostics is not None and (diagnostics.fine_score is None or score > diagnostics.fine_score):
                diagnostics.fine_score = score
            if level_score < 0.72 or score < 0.73 or (best and score <= best.similarity):
                continue
            mx, my = left + x + size / 2, top + y + size / 2
            best = TemplatePosition(tile.left - mx / tile.scale, tile.top - my / tile.scale,
                                    tile.floor, tile.layer_id, tile.name, score, size,
                                    query_radius * size / gray.shape[1] / tile.scale)
        return best


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="BetterGI 地图视觉坐标定位")
    parser.add_argument("--assets", required=True, help="Assets/Map/Teyvat 目录")
    parser.add_argument("--image", required=True, help="已裁剪小地图图像")
    parser.add_argument("--method", choices=("sift", "template"), default="sift", help="定位方式")
    parser.add_argument("--floor", type=int, help="地图层级，SIFT 默认 0，模板默认搜索所有层")
    parser.add_argument("--layer-id", help="模板图块 ID，可用于缩小搜索范围")
    parser.add_argument("--center", type=float, nargs=2, metavar=("X", "Y"), default=None)
    args = parser.parse_args()
    img = cv2.imdecode(np.fromfile(args.image, dtype=np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        parser.error(f"无法读取图像: {args.image}")
    if args.method == "template":
        loc = BetterGITemplateLocalizer(args.assets, floor=args.floor, layer_id=args.layer_id)
    else:
        if args.layer_id is not None:
            parser.error("--layer-id 仅适用于 --method template")
        loc = BetterGIMapLocalizer(args.assets, floor=0 if args.floor is None else args.floor)
    print(loc.locate(img, center=args.center))
