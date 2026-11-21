"""从攻略帧左上角提取原神小地图候选区域。"""

from __future__ import annotations

from dataclasses import dataclass
from math import ceil

import cv2
import numpy as np


@dataclass(frozen=True)
class MinimapCircle:
    """相对于去黑边后的视频内容，按 640 像素宽的比例表示。"""

    x: float
    y: float
    radius: float
    detected: bool = True

    def crop(self, frame: np.ndarray) -> np.ndarray:
        scale = frame.shape[1] / 640
        # 视频里的圆边可能叠着方位标和剪辑装饰，按比例内缩后再匹配地形。
        half = max(1, round(self.radius * scale * 0.88))
        crop = cv2.getRectSubPix(frame, (2 * half + 1, 2 * half + 1), (self.x * scale, self.y * scale))
        if min(crop.shape[:2]) < 100:
            return cv2.resize(crop, (212, 212), interpolation=cv2.INTER_CUBIC)
        return crop

    def overlaps(self, other: MinimapCircle) -> bool:
        tolerance = max(5.0, min(self.radius, other.radius) * 0.16)
        return (abs(self.x - other.x) < tolerance and abs(self.y - other.y) < tolerance
                and abs(self.radius - other.radius) < tolerance)


def video_content(frame: np.ndarray) -> np.ndarray:
    """去掉视频画幅本身的纯黑边。"""
    height, width = frame.shape[:2]
    dark = cv2.inRange(frame, (0, 0, 0), (12, 12, 12))
    row_sums = cv2.reduce(dark, 1, cv2.REDUCE_SUM, dtype=cv2.CV_32S).ravel()
    column_sums = cv2.reduce(dark, 0, cv2.REDUCE_SUM, dtype=cv2.CV_32S).ravel()
    rows = np.flatnonzero(row_sums < 255 * width * 0.98)
    columns = np.flatnonzero(column_sums < 255 * height * 0.98)
    if not rows.size or not columns.size:
        return frame
    top, bottom = int(rows[0]), int(rows[-1]) + 1
    left, right = int(columns[0]), int(columns[-1]) + 1
    if (bottom - top) < height * 0.55 or (right - left) < width * 0.55:
        return frame
    return frame[top:bottom, left:right]


def has_video_minimap(crop: np.ndarray) -> bool:
    """验证候选裁剪里是否仍有小地图，沿用原圆心。"""
    normalized = cv2.resize(crop, (168, 168), interpolation=cv2.INTER_AREA)
    hsv = cv2.cvtColor(normalized[54:114, 54:114], cv2.COLOR_BGR2HSV)
    marker = cv2.inRange(hsv, (77, 120, 162), (110, 255, 255))
    _, _, stats, centers = cv2.connectedComponentsWithStats(marker)
    for (_, _, width, height, area), center in zip(stats[1:], centers[1:]):
        if (min(width, height) >= 6 and max(width, height) <= 34 and 30 <= area <= 560
                and np.linalg.norm(center - 29.5) <= 13):
            return True
    # 中心标记被覆盖时，检查现有圆心附近是否仍有圆边。
    gray = cv2.medianBlur(cv2.cvtColor(normalized, cv2.COLOR_BGR2GRAY), 5)
    rims = cv2.HoughCircles(gray, cv2.HOUGH_GRADIENT, 1, 15,
                           param1=85, param2=20, minRadius=72, maxRadius=111)
    return rims is not None and bool(np.any(np.linalg.norm(rims[0, :, :2] - 83.5, axis=1) <= 10))


def find_minimap_circles(frame: np.ndarray, standard: MinimapCircle | None = None) -> list[MinimapCircle]:
    """按可见圆环和中心蓝色玩家标记排序，再补充贴边的大圆。"""
    height, width = frame.shape[:2]
    scale = 640 / width
    top_left = frame[:min(height, ceil(248 / scale)), :min(width, ceil(285 / scale))]
    roi = cv2.resize(top_left, (round(top_left.shape[1] * scale), round(top_left.shape[0] * scale)),
                     interpolation=cv2.INTER_AREA)[:248, :285]
    if min(roi.shape[:2]) < 45:
        return []
    grey = cv2.medianBlur(cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY), 5)
    raw = cv2.HoughCircles(grey, cv2.HOUGH_GRADIENT, 1, 15,
                           param1=90, param2=24, minRadius=19, maxRadius=134)

    def visible_arc(circle: MinimapCircle) -> bool:
        x, y = circle.x, circle.y
        return min(x, y, roi.shape[1] - x, roi.shape[0] - y) >= 0.63 * circle.radius

    # Hough 输出按投票强度排序。蓝色箭头可帮助区分地形内的圆形装饰。
    ranked = []
    if raw is not None:
        blue, green, red = roi.astype(np.int16).transpose(2, 0, 1)
        cyan = (blue - red > 35) & (green - red > 35) & (green > 110)
        for index, (x, y, radius) in enumerate(raw[0, :32]):
            circle = MinimapCircle(float(x), float(y), float(radius))
            if not visible_arc(circle):
                continue
            center = max(3, round(radius * 0.13))
            xx, yy = round(x), round(y)
            patch = cyan[yy - center:yy + center + 1, xx - center:xx + center + 1]
            score = min(float(patch.mean()), 0.45) - index * 0.024
            ranked.append((score, circle))
    candidates: list[MinimapCircle] = []
    for _, circle in sorted(ranked, key=lambda item: item[0], reverse=True):
        if not any(circle.overlaps(existing) for existing in candidates):
            candidates.append(circle)
        if len(candidates) == 2:
            break
    large = cv2.HoughCircles(grey, cv2.HOUGH_GRADIENT, 1, 12,
                             param1=70, param2=20, minRadius=44, maxRadius=134)
    if large is not None:
        clipped = MinimapCircle(*map(float, large[0, 0]))
        if (min(clipped.x, clipped.y) < clipped.radius and visible_arc(clipped)
                and not any(clipped.overlaps(circle) for circle in candidates)):
            candidates.insert(0, clipped)
    if standard is not None and not any(standard.overlaps(circle) for circle in candidates):
        candidates.append(standard)
    return candidates
