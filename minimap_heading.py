"""从北朝上的游戏小地图读取摄像机扇形朝向。"""

from functools import lru_cache

import cv2
import numpy as np

from bettergi_map_localizer import _exclude_map_icons


def camera_heading(minimap: np.ndarray) -> float | None:
    """返回以北为 0°、顺时针增大的角度，扇形不清晰时返回 None。"""
    if min(minimap.shape[:2]) < 64:
        return None
    normalized = cv2.resize(minimap, (212, 212), interpolation=cv2.INTER_LINEAR)
    angle, confidence, contrast, separation = _color_sector(normalized)
    if confidence >= 0.2 and contrast >= 0.20 and separation >= 0.020:
        return angle
    return _edge_heading(normalized)


@lru_cache(maxsize=1)
def _polar_color_tables() -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    radii = np.linspace(19, 78, 60, dtype=np.float32)
    angles = np.deg2rad(np.arange(360, dtype=np.float32))[:, None]
    x = (78 + np.cos(angles) * radii).astype(np.float32)
    y = (78 + np.sin(angles) * radii).astype(np.float32)
    thresholds = [18.632, 20.157, 24.093, 34.617, 38.566, 41.94, 47.654,
                  51.087, 58.561, 63.925, 67.759, 71.77, 75.214]
    base_alpha = 255 / (229 + np.searchsorted(thresholds, radii))
    fan_alpha = 255 / (137 + 1.43 * radii)
    return x, y, base_alpha.astype(np.float32), fan_alpha


def _color_sector(normalized: np.ndarray) -> tuple[float, float, float, float]:
    # BetterGI 的彩色方法通过透明度补偿判断像素属于扇形内还是扇形外。
    # https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/MiniMap/CameraOrientationCalculator.cs
    center = normalized[28:184, 28:184]
    mask = np.full(center.shape[:2], 255, dtype=np.uint8)
    _exclude_map_icons(center, mask, 78)
    x, y, base_alpha, fan_alpha = _polar_color_tables()
    polar = cv2.remap(center, x, y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    valid = cv2.remap(mask, x, y, cv2.INTER_NEAREST, borderMode=cv2.BORDER_REPLICATE) != 0
    hue = cv2.cvtColor(polar, cv2.COLOR_BGR2HLS_FULL)[:, :, 0].astype(np.int32)
    gray = cv2.cvtColor(polar, cv2.COLOR_BGR2GRAY).astype(np.float32)
    outside = np.where(valid, gray * base_alpha, gray)
    inside = np.where(valid, ((gray - 255) * fan_alpha + 255) * base_alpha, gray)

    base_index = hue * 256
    outside_bin = base_index + np.clip(outside, 0, 255).astype(np.int32)
    inside_bin = base_index + np.clip(inside, 0, 255).astype(np.int32)
    outdoor = outside < 256
    indoor = (inside >= 0) & (inside < 256)
    outside_hist = np.bincount(outside_bin[outdoor], minlength=65536)
    inside_hist = np.bincount(inside_bin[indoor], minlength=65536)
    gain = inside_hist[inside_bin] - outside_hist[outside_bin]
    votes = np.where(gain > 0, 1.0, np.where(gain < 0, 0.0, 100 / 255))
    votes = np.where(outdoor, votes, 1.0)
    votes = np.where(inside < 0, -1.0, votes)
    votes = np.where(inside >= 256, 0.0, votes)

    sectors = votes.mean(axis=1)
    scores = np.convolve(np.pad(sectors, 45, mode="wrap"), np.full(91, 1 / 91), mode="valid")
    best = int(np.argmax(scores))
    outside_score = (360 * sectors.mean() - 91 * scores[best]) / 269
    contrast = scores[best] - outside_score
    # 对比远离最佳角度的次峰，过滤均匀画面或多个候选强度接近的情况。
    rivals = np.roll(scores, -best)[35:-34]
    return (best + 90) % 360, float(scores[best]), float(contrast), float(scores[best] - rivals.max())


def _edge_heading(normalized: np.ndarray) -> float | None:
    # BetterGI 的 GIA 方法在极坐标中匹配间隔约 90° 的两条扇形边。
    # https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Camera/CameraOrientationFromGia.cs
    gray = cv2.cvtColor(normalized, cv2.COLOR_BGR2GRAY)
    gray = cv2.GaussianBlur(gray, (3, 3), 0)
    polar = cv2.warpPolar(gray, (96, 360), (106, 106), 96, cv2.INTER_LINEAR | cv2.WARP_FILL_OUTLIERS)
    # 舍去中心角色箭头和外圈图标，保留摄像机亮区的直线边界。
    band = polar[:, 19:78].astype(np.float32)
    wrapped = np.concatenate((band[-1:], band, band[:1]))
    gradient = cv2.Scharr(wrapped, cv2.CV_32F, 0, 1)[1:-1]

    def edge_support(values: np.ndarray) -> np.ndarray:
        peaks = (values > np.roll(values, 1, axis=0)) & (values >= np.roll(values, -1, axis=0))
        votes = np.where(peaks, np.clip((values - 48) / 96, 0, 1), 0)
        # 边缘随半径和缩放产生少量量化偏差，每条半径在相邻角度只投一票。
        return np.maximum.reduce([np.roll(votes, offset, axis=0) for offset in range(-2, 3)]).sum(axis=1)

    leading = edge_support(gradient)
    trailing = edge_support(-gradient)
    paired = np.maximum.reduce([np.roll(trailing, -offset) for offset in range(87, 94)])
    scores = np.sqrt(leading * paired)
    best = int(np.argmax(scores))
    rival = np.roll(scores, -best)[15:-14].max()
    if min(leading[best], paired[best]) < 12 or scores[best] < max(17, 1.25 * rival):
        return None
    return (best + 45 + 90) % 360
