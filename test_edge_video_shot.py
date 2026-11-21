# -*- coding: utf-8 -*-
"""pytest 用例：抓 Microsoft Edge 当前网页里正在播放的视频画面。

三种跑法：

    pytest pytest/test_edge_video_shot.py -v
        # 只跑「窗口截图」用例：零配置，截整个 Edge 窗口，任何情况下都能出图。

    $env:EDGE_SHOT_PORT = "9222"; pytest pytest/test_edge_video_shot.py -v
        # 加上「视频精确截图」用例：只截 <video> 元素那块，需要 Edge 带调试端口。
        # 注意 Edge/Chromium 136+ 用默认 profile 时 --remote-debugging-port 会被忽略，
        # 所以要用独立 profile 起一个带端口的 Edge（profile 固定，登录态/标签页会留着）：
        #   python -c "import edge_video_shot as m; m.launch_edge_debug(9222, urls=['https://www.bilibili.com/video/BV1sZ4n6MEHd/'])"

    $env:EDGE_SHOT_SELFCHECK = "1"; pytest pytest/test_edge_video_shot.py -v
        # 自检：临时起一个独立 profile 的 Edge + 本地测试页，不碰你正在用的窗口，
        # 用来验证 CDP 链路（clip / canvas 两种抓帧）本身是否正常。

环境变量：
    EDGE_SHOT_PORT         已开调试端口的 Edge 端口（默认 9222）
    EDGE_SHOT_URL          只截 URL/标题匹配该正则的标签页
    EDGE_SHOT_OUT          截图输出目录（默认 pytest/output/edge_video_shot）
    EDGE_SHOT_SELFCHECK    =1 时跑临时 Edge 自检
    EDGE_SHOT_SELFCHECK_PORT  自检用的端口（默认 9231）
"""

from __future__ import annotations

import base64
import json
import os
from pathlib import Path

import pytest

import edge_video_shot as evs

PORT = int(os.environ.get("EDGE_SHOT_PORT", evs.DEFAULT_PORT))
URL_PATTERN = os.environ.get("EDGE_SHOT_URL") or None
SELFCHECK = os.environ.get("EDGE_SHOT_SELFCHECK") == "1"
SELFCHECK_PORT = int(os.environ.get("EDGE_SHOT_SELFCHECK_PORT", "9231"))
OUT_DIR = Path(
    os.environ.get("EDGE_SHOT_OUT") or Path(__file__).with_name("output") / "edge_video_shot"
)

# 自检页：canvas 逐帧画图 -> captureStream -> <video> 播放。
# 不联网、无跨域，所以 clip 和 canvas 两条抓帧路径都能验证。
SELFCHECK_HTML = """<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<title>EDGE_SHOT_SELFCHECK</title>
<style>
  html, body { margin: 0; background: #0b0b0b; overflow: hidden; }
  video { display: block; margin: 60px auto; width: 640px; height: 360px; }
</style></head>
<body>
<canvas id="src" width="640" height="360" style="display:none"></canvas>
<video id="v" autoplay muted playsinline></video>
<script>
  const canvas = document.getElementById('src');
  const ctx = canvas.getContext('2d');
  let frame = 0;
  setInterval(() => {
    frame += 1;
    ctx.fillStyle = 'hsl(' + ((frame * 3) % 360) + ',70%,45%)';
    ctx.fillRect(0, 0, 640, 360);
    ctx.fillStyle = '#fff';
    ctx.font = 'bold 72px sans-serif';
    ctx.fillText('FRAME ' + frame, 80, 200);
  }, 40);
  const video = document.getElementById('v');
  video.srcObject = canvas.captureStream(25);
  video.play().catch(() => {});
</script>
</body></html>
"""


@pytest.fixture(scope="session")
def out_dir() -> Path:
    evs.configure_stdout()      # 标题里的零宽字符在 GBK 控制台下会炸 print
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    return OUT_DIR


def _save(shot: evs.VideoShot, out_dir: Path, name: str) -> Path:
    path = shot.save(out_dir / name)
    assert path.stat().st_size > 2000, f"截图文件太小，可能没截到内容: {path}"
    return path


@pytest.mark.parametrize("canvas_blocked", (False, True))
def test_wake_capture_crops_without_changing_viewport(canvas_blocked: bool) -> None:
    cv2 = pytest.importorskip("cv2")
    np = pytest.importorskip("numpy")
    color = (40, 90, 150)
    frame = np.full((170, 170, 3), color, dtype=np.uint8)
    _, png = cv2.imencode(".png", frame)
    viewport_frame = np.zeros((1200, 1800, 3), dtype=np.uint8)
    viewport_frame[94:428, 82:416] = color  # CSS viewport coordinates at 200% DPI
    _, jpeg = cv2.imencode(".jpg", viewport_frame)
    calls = []

    class Browser:
        state = "minimized"

        def evaluate(self, expression, **_kwargs):
            if expression.startswith(evs._JS_FAST_CANVAS_FRAME):
                calls.append(("video canvas", {}))
                assert expression.endswith("([94.5, 90.5, 83.7], 1)")
                if canvas_blocked:
                    return {"found": True, "blocked": True, "reason": "跨域视频限制 canvas 取帧"}
                data = base64.b64encode(png).decode("ascii")
                return {"found": True, "dataUrl": "data:image/png;base64," + data, "currentTime": 32,
                        "sample": [84, 84, 84], "drawMs": 1.5, "encodeMs": 2.5}
            return {"found": True, "currentTime": 31, "videoIndex": 1, "videoWidth": 640, "videoHeight": 360,
                    "rect": {"x": 30, "y": 40, "width": 640, "height": 360},
                    "viewport": {"width": 900, "height": 600, "scrollX": 10, "scrollY": 20}}

        def call(self, name, **params):
            calls.append((name, params))
            if name == "Browser.getWindowForTarget":
                assert params["targetId"] == "guide"
                return {"bounds": {"windowState": self.state}}
            assert canvas_blocked and name == "Page.captureScreenshot"
            assert "clip" not in params and params["captureBeyondViewport"] is False
            assert params["format"] == "jpeg"
            return {"data": base64.b64encode(jpeg).decode("ascii")}

    browser = Browser()
    source = evs.ExistingEdgeVideo(pin_tab=True)
    source._browser = browser
    source._pinned = {"targetId": "guide"}, evs._AttachedPage(browser, "attached")
    circle = (94.5, 90.5, 83.7)
    with pytest.raises(evs.VideoNotFound, match="等待 Edge 退出最小化"):
        source.capture(mode="window-region", circle=circle)
    assert [name for name, _ in calls] == ["Browser.getWindowForTarget"]
    browser.state = "normal"
    calls.clear()
    shot = source.capture(mode="window-region", circle=circle)
    assert shot.via == "cdp-window-region"
    expected = ["Browser.getWindowForTarget", "video canvas"]
    if canvas_blocked:
        expected.append("Page.captureScreenshot")
    assert [name for name, _ in calls] == expected
    size = 334 if canvas_blocked else 212
    assert shot.bgr.shape == (size, size, 3)
    assert shot.info["currentTime"] == (31 if canvas_blocked else 32)
    matches = np.max(np.abs(shot.bgr.astype(int) - color), axis=2) < 15
    assert matches.mean() > 0.97


def test_pinned_fast_capture_uses_one_cdp_call(monkeypatch) -> None:
    cv2 = pytest.importorskip("cv2")
    np = pytest.importorskip("numpy")
    frame = np.full((360, 640, 3), (15, 80, 210), dtype=np.uint8)
    _, jpeg = cv2.imencode(".jpg", frame)
    data_url = "data:image/jpeg;base64," + base64.b64encode(jpeg).decode("ascii")
    calls = []

    class Browser:
        def evaluate(self, expression, *, await_promise, _session_id):
            assert _session_id == "attached"
            calls.append(expression)
            if await_promise:
                return {"ok": True, "currentTime": 90}
            if expression.startswith(evs._JS_FAST_CANVAS_FRAME):
                circle = json.loads(expression.removeprefix(evs._JS_FAST_CANVAS_FRAME)[1:-1])
                if circle is not None:
                    cx, cy, radius = circle
                    half = round(radius)
                    x, y, size = int(cx) - half, int(cy) - half, 2 * half + 2
                    if x < 0 or y < 0:
                        return {"found": True, "invalidCircle": True}
                    _, png = cv2.imencode(".png", frame[y:y + size, x:x + size])
                    url = "data:image/png;base64," + base64.b64encode(png).decode("ascii")
                    return {"found": True, "currentTime": 90, "dataUrl": url, "sample": [cx - x, cy - y, half],
                            "drawMs": 1, "encodeMs": 2}
                return {"found": True, "currentTime": 91.2, "dataUrl": data_url, "drawMs": 1, "encodeMs": 2}
            return {"found": True}

    browser = Browser()
    source = evs.ExistingEdgeVideo(pin_tab=True)
    source._browser = browser
    source._pinned = {"targetId": "initial"}, evs._AttachedPage(browser, "attached")
    shot = source.capture(mode="canvas-fast")
    assert calls == [f"{evs._JS_FAST_CANVAS_FRAME}(null)"]
    assert shot.info["currentTime"] == 91.2
    assert shot.bgr.shape == frame.shape
    assert np.max(np.abs(shot.bgr.astype(int) - frame)) <= 2

    circle = (94.5, 90.5, 83.7)
    region_shot = source.capture(mode="canvas-region", circle=circle)
    assert len(calls) == 2
    assert region_shot.via == "cdp-canvas-region"
    assert np.array_equal(region_shot.bgr, cv2.getRectSubPix(frame, (169, 169), circle[:2]))
    at = source.capture(mode="canvas-region", circle=circle, at_time=90)
    assert at.info["seeked"]["currentTime"] == 90
    assert calls[-1] == f"{evs._JS_FAST_CANVAS_FRAME}({json.dumps(circle)})"
    with pytest.raises(evs.VideoNotFound, match="超出攻略画面"):
        source.capture(mode="canvas-region", circle=(1, 1, 40))
    assert len(calls) == 5

    def seek(page, target, info, *, mode, at_time):
        return mode, at_time

    monkeypatch.setattr(evs, "_take_video_frame", seek)
    assert source.capture(mode="canvas-fast", at_time=90) == ("canvas", 90)
    assert evs._JS_PROBE_VIDEO in calls[-1]


# --------------------------------------------------------------------------- #
# 1. 零配置：整个 Edge 窗口
# --------------------------------------------------------------------------- #

def test_capture_edge_window(out_dir: Path) -> None:
    """截当前 Edge 窗口（窗口被遮挡也能截；最小化时退化为屏幕截图）。"""
    try:
        window = evs.pick_edge_window(os.environ.get("EDGE_SHOT_TITLE") or None)
    except evs.EdgeShotError as exc:
        pytest.skip(f"没有可截的 Edge 窗口: {exc}")

    shot = evs.capture_window(window.hwnd)
    assert shot.via == "window"
    assert shot.image.size == (window.width, window.height)
    assert shot.image.convert("L").getextrema() != (0, 0), "整张图全黑，窗口可能最小化了"

    path = _save(shot, out_dir, "edge-window.png")
    print(f"\n窗口截图 {shot.image.width}x{shot.image.height} -> {path}")
    print(f"标题: {window.title}")


def test_capture_edge_window_cropped(out_dir: Path) -> None:
    """窗口截图 + 裁剪：只留视频区域（这里按窗口上半部分举例）。"""
    try:
        window = evs.pick_edge_window(os.environ.get("EDGE_SHOT_TITLE") or None)
    except evs.EdgeShotError as exc:
        pytest.skip(f"没有可截的 Edge 窗口: {exc}")

    crop = (0, 100, window.width, max(1, window.height // 2))
    shot = evs.capture_window(window.hwnd, crop=crop)
    assert shot.image.size == (crop[2], crop[3])
    _save(shot, out_dir, "edge-window-cropped.png")


# --------------------------------------------------------------------------- #
# 2. 精确：CDP 只截 <video>
# --------------------------------------------------------------------------- #

def test_capture_video_frame(out_dir: Path) -> None:
    """只截 <video> 元素那一块（Edge 需已开调试端口）。"""
    if not evs.debug_port_open(PORT):
        pytest.skip(
            f"端口 {PORT} 没有开调试接口；先运行 "
            "python -c \"import edge_video_shot as m; m.launch_edge_debug(9222, urls=['https://www.bilibili.com/'])\""
            "（默认 profile 开不了调试端口，见模块 docstring）"
        )
    try:
        shot = evs.capture_video(PORT, url_pattern=URL_PATTERN, mode="clip", wait_ready=8.0)
    except evs.VideoNotFound as exc:
        pytest.skip(f"当前标签页没有可截图的 <video>: {exc}")

    assert shot.via == "cdp-clip"
    assert shot.image.width >= 2 and shot.image.height >= 2
    assert shot.image.convert("L").getextrema() != (0, 0), "截到的是纯黑图，视频可能没在播"

    path = _save(shot, out_dir, "edge-video-frame.png")
    info = shot.info
    print(
        f"\n视频截图 {shot.image.width}x{shot.image.height} -> {path}"
        f"\n页面: {info['page']['title']} {info['page']['url']}"
        f"\n视频: {info['videoWidth']}x{info['videoHeight']} "
        f"t={info['currentTime']:.2f}/{info['duration']} paused={info['paused']}"
        f"\n元素区域: {info['rect']} clip={info['clip']}"
    )


def test_capture_video_frame_canvas(out_dir: Path) -> None:
    """canvas 抓帧：拿视频原生分辨率、无播放器控件；跨域视频会失败（预期行为）。"""
    if not evs.debug_port_open(PORT):
        pytest.skip(f"端口 {PORT} 没有开调试接口")
    try:
        shot = evs.capture_video(PORT, url_pattern=URL_PATTERN, mode="canvas", wait_ready=8.0)
    except evs.VideoNotFound as exc:
        pytest.skip(f"当前标签页没有可截图的 <video>: {exc}")
    except evs.EdgeShotError as exc:
        if "污染" in str(exc) or "taint" in str(exc).lower():
            pytest.skip(f"跨域视频无法用 canvas 抓帧，改用 mode='clip': {exc}")
        raise

    assert shot.via == "cdp-canvas"
    assert shot.image.size == (shot.info["videoWidth"], shot.info["videoHeight"])
    _save(shot, out_dir, "edge-video-frame-canvas.png")


# --------------------------------------------------------------------------- #
# 3. 自检：临时 Edge + 本地测试页（不碰现有窗口）
# --------------------------------------------------------------------------- #

@pytest.mark.skipif(not SELFCHECK, reason="设置 EDGE_SHOT_SELFCHECK=1 才跑自检")
def test_selfcheck_cdp_chain(tmp_path: Path, out_dir: Path) -> None:
    """起一个独立 profile 的 Edge 打开本地测试页，验证 clip / canvas 两条链路。"""
    if evs.debug_port_open(SELFCHECK_PORT):
        pytest.skip(f"端口 {SELFCHECK_PORT} 已被占用，换个 EDGE_SHOT_SELFCHECK_PORT")

    page = tmp_path / "selfcheck.html"
    page.write_text(SELFCHECK_HTML, encoding="utf-8")

    proc = evs.launch_edge_debug(
        SELFCHECK_PORT, user_data_dir=tmp_path / "profile", urls=[page.as_uri()]
    )
    try:
        clip = evs.capture_video(SELFCHECK_PORT, mode="clip", wait_ready=30.0)
        assert clip.image.width >= 300 and clip.image.height >= 200, clip.image.size
        assert clip.image.convert("L").getextrema() != (0, 0)
        assert clip.info["videoWidth"] == 640 and clip.info["videoHeight"] == 360
        _save(clip, out_dir, "selfcheck-clip.png")

        canvas = evs.capture_video(SELFCHECK_PORT, mode="canvas", wait_ready=10.0)
        assert canvas.image.size == (640, 360)
        _save(canvas, out_dir, "selfcheck-canvas.png")

        # 整页截图也顺带验证一下
        full = evs.capture_page(SELFCHECK_PORT, full_page=True)
        _save(full, out_dir, "selfcheck-page.png")
    finally:
        evs.kill_edge(proc)
