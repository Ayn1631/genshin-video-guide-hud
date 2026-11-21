from argparse import Namespace
from pathlib import Path
from threading import Event, Thread
import time
from types import SimpleNamespace

import numpy as np
import pytest

import video_map_guide as guide
import window_video_shot as windows


@pytest.mark.parametrize(("video_fps", "player_fps", "expected"), ((18, 10, 23), (10, 22, 27), (10, 10, 15)))
def test_hud_fps_follows_faster_source(video_fps, player_fps, expected):
    assert guide._hud_fps(Namespace(video_fps=video_fps, player_fps=player_fps)) == expected


def test_hud_shows_distance_from_same_map_positions():
    player = guide.MapPoint(0, 0, 0)
    target = guide.MapPoint(-3, 4, 0)
    assert guide._compass_display(player, target, 0, "") == ("+37°", "", 37, 5)
    assert guide._compass_display(player, target, None, "")[3] == 5
    assert guide._compass_display(player, target, 0, "", arrival_distance=5) == ("目标附近", "", None, 5)
    assert guide._compass_display(player, None, 0, "")[3] is None
    assert guide._compass_display(player, guide.MapPoint(-3, 4, 0, scene="TheChasm"), 0, "")[3] is None


def wait_for(predicate):
    deadline = time.monotonic() + 3
    while not predicate() and time.monotonic() < deadline:
        time.sleep(0.005)
    assert predicate()


@pytest.mark.parametrize("blocked", ("player", "video"))
def test_slow_matcher_keeps_other_stream_live_and_skips_old_frames(monkeypatch, blocked):
    release = Event()
    entered = Event()
    frames = {"player": [], "video": []}
    matches = {"player": [], "video": []}

    def capture(source):
        index = len(frames[source]) + 1
        frames[source].append(index)
        return np.asarray(index)

    def match(source, frame):
        index = int(frame.item())
        matches[source].append(index)
        if source == blocked and len(matches[source]) == 2:
            entered.set()
            if not release.wait(5):
                raise RuntimeError("test did not release the matcher")
        return index

    class GameCapture:
        def grab_minimap(self):
            return capture("player")

    class VideoMatcher:
        maps = ()
        mode = "局部匹配"

        def __init__(self, *args, **kwargs):
            pass

        def locate_video(self, frame, *args, **kwargs):
            return guide.MapPoint.from_match(guide.Position(match("video", frame), 0, 0, 0, 0, 20, 20, 1, 0))

    class PlayerMatcher:
        def __init__(self, assets):
            pass

        def locate(self, frame, anchor, previous):
            return guide.Position(match("player", frame), 0, 0, 0, 0, 20, 20, 1, 0)

    monkeypatch.setattr(guide, "VideoTemplateMatcher", VideoMatcher)
    monkeypatch.setattr(guide, "GuidedPlayerMatcher", PlayerMatcher)
    monkeypatch.setattr(guide, "_read_video_frame", lambda *args, **kwargs: capture("video"))
    monkeypatch.setattr(guide.LiveGuide, "_capture_video", lambda self: self._capture_video_frames(None))
    args = Namespace(follow_video=True, console=True, player_fps=50, video_fps=50,
                     player_layer_id=None, video_layer_id=None, at=None, video_url=None, edge_existing=False,
                     assets=Path("."))
    session = guide.LiveGuide(args, GameCapture())
    other = "target" if blocked == "player" else "player"

    try:
        session.start()
        assert entered.wait(3)
        first = matches[blocked][1]
        published = getattr(session, other)
        wait_for(lambda: len(frames[blocked]) >= first + 4
                 and getattr(session, other) is not published and getattr(session, other).point is not None)
        assert len(matches[blocked]) == 2
        release.set()
        wait_for(lambda: len(matches[blocked]) >= 3)
        assert matches[blocked][2] >= first + 4
        assert session.error is None
    finally:
        release.set()
        session.close()


def test_player_tries_once_per_frame_and_follows_guide_tile_switch(monkeypatch):
    first = guide.TemplatePosition(10, 10, 0, "first", "first", 0.8)
    second = guide.TemplatePosition(100, 100, 0, "second", "second", 0.8)
    attempts = []

    class PlayerMatcher:
        def __init__(self, assets):
            pass

        def locate(self, frame, anchor, previous):
            attempts.append((int(frame.item()), anchor, previous))
            return {1: first, 2: None, 3: first, 4: second}[int(frame.item())]

    monkeypatch.setattr(guide, "GuidedPlayerMatcher", PlayerMatcher)
    session = guide.LiveGuide(Namespace(follow_video=True, assets=Path(".")), object())
    worker = Thread(target=session._track_player)
    worker.start()

    def frame(number):
        previous_reading = session.player
        session.player_frame = guide.CapturedPlayerFrame(time.monotonic(), np.asarray(number))
        session._player_ready.set()
        wait_for(lambda: session.player is not previous_reading)

    try:
        frame(0)
        assert attempts == []
        session._guide_anchor = first
        frame(1)
        frame(2)
        assert session.player.cached
        frame(3)
        session._guide_anchor = second
        frame(4)
        assert attempts == [(1, first, None), (2, first, first), (3, first, None), (4, second, None)]
        assert session.player.point.match == second
        assert not session.player.cached
    finally:
        session.close()
        worker.join(timeout=3)


def test_video_without_minimap_skips_global_search_after_local_miss(monkeypatch):
    screenshot = Path(__file__).parent / "assest" / "screenshot.png"
    if not screenshot.is_file():
        pytest.skip("提供本地攻略截图 assest/screenshot.png 后运行真实帧回归用例")
    frame = guide._read_image(screenshot)
    no_minimap = frame.copy()
    no_minimap[:179, :184] = frame[100:279, 240:424]
    monkeypatch.setattr(guide, "BetterGITemplateLocalizer", lambda *args, **kwargs: object())
    matcher = guide.VideoTemplateMatcher(Path("."))
    probes = []
    searches = []
    matcher.previous = guide.TemplatePosition(10, 10, 0, "tile", "tile", 0.8)

    def match(crop, *, search_global=True, diagnostics=None):
        probes.append(search_global)
        return None

    monkeypatch.setattr(matcher, "locate", match)
    monkeypatch.setattr(matcher, "_locate_features", lambda *args, **kwargs: None)
    args = Namespace(video_circle=(90.5, 87.5, 95.9), video_inset=12.0)
    for _ in range(2):
        with pytest.raises(guide.VideoMatchError, match="攻略画面未检测到小地图") as missing:
            matcher.locate_video(no_minimap, args, on_global=searches.append)
        assert not missing.value.retried

    assert searches == []
    assert probes == [False]
    with pytest.raises(guide.VideoMatchError) as visible:
        matcher.locate_video(frame, args, on_global=searches.append)
    assert visible.value.retried
    assert searches == [True]
    assert probes == [False, False, True]


def test_windows_capture_reuses_window_and_samples_only_projected_minimap(monkeypatch):
    state = {"ready": True, "minimized": False}
    events = []
    popup = windows.edge.EdgeWindow(1, 1, "是否允许远程调试?", (0, 0, 480, 240))
    browser = windows.edge.EdgeWindow(2, 1, "攻略标签组 - 个人 - Microsoft\u200b Edge", (0, 0, 1600, 900))
    user32 = SimpleNamespace(GetForegroundWindow=lambda: 1, IsWindow=lambda hwnd: hwnd == 2,
                             IsIconic=lambda hwnd: state["minimized"])

    def list_windows(*, include_minimized):
        assert include_minimized
        events.append("list")
        return [popup, browser]

    class Capture:
        def __init__(self, hwnd):
            events.append(hwnd)

        def read(self, rect, _timeout_ms):
            events.append(rect)
            if state["ready"]:
                frame = np.zeros((160, 160, 4), dtype=np.uint8)
                frame[:, :, 0] = 143
                return frame
            return None

        def close(self):
            events.append("close")

    monkeypatch.setattr(windows.edge, "_HAS_WIN32", True)
    monkeypatch.setattr(windows.edge, "_user32", user32, raising=False)
    monkeypatch.setattr(windows.edge, "list_edge_windows", list_windows)
    monkeypatch.setattr(windows, "_WindowCapture", Capture)
    args = Namespace(video_image=None, frame_mode="canvas-region", video_circle=(100, 90, 85),
                     video_inset=5, follow_video=True)
    with windows.WindowsEdgeVideo(video_rect=(0.25, 0.1, 0.5, 0.8)) as source:
        frame = guide._read_video_frame(args, None, existing_edge=source)
        assert frame.shape == (212, 212, 3)
        assert frame[106, 106].tolist() == [143, 0, 0]
        assert events[:2] == ["list", 2]
        assert events[-1] == pytest.approx((0.265625, 0.1222222222, 0.125, 0.3555555556))

        state["ready"] = False
        with pytest.raises(windows.edge.VideoNotFound, match="请让攻略继续播放"):
            guide._read_video_frame(args, None, existing_edge=source)
        assert events.count(2) == 1
        state["minimized"] = True
        with pytest.raises(windows.edge.VideoNotFound, match="请还原 Edge"):
            guide._read_video_frame(args, None, existing_edge=source)
        assert events[-1] == "close"
        state.update(ready=True, minimized=False)
        guide._read_video_frame(args, None, existing_edge=source)
        assert events.count("list") == 1 and events.count(2) == 2


def test_detailed_mode_tracks_live_search_and_keeps_completed_search_times(monkeypatch):
    args = Namespace(follow_video=True, console=False, detailed_hud=True, edge_mode="windows")
    session = guide.LiveGuide(args, object())
    details = session.timings
    details.video_capture_started = 20.0
    details.record_video_capture(20.05, (0, 0, 48, 2, 512), "windows", available=True)
    details.video_match = guide.MatchTiming((("匹配前处理", 20.06),), 20.0, 20.05)
    events = iter((20.1, 20.3))
    monkeypatch.setattr(guide.time, "perf_counter", lambda: next(events))
    session._publish_search("region")
    session._publish_search(True)
    title, rows = details.sections(21.0, args)[0]
    assert title == "攻略 · Windows 捕获"
    assert rows[0][0] == "FPS"
    assert dict(rows[1:]) == pytest.approx({"攻略取帧": 50, "等待与读回": 48, "像素转换": 2,
                                            "帧交接": 10, "匹配前处理": 40, "国家或独立地图匹配": 200,
                                            "全图重试 · 进行中": 700, "端到端": 1000})
    details.finish_video_match(21.3, "全图重试")
    details.video_match = guide.MatchTiming((("局部匹配", 21.5),), finished=21.51)
    rows = dict(details.sections(21.6, args)[0][1])
    assert rows["局部匹配"] == pytest.approx(10)
    assert rows["上次国家或独立地图匹配"] == pytest.approx(200)
    assert rows["上次全图重试"] == pytest.approx(1000)


def test_detailed_fps_counts_each_stream_and_expires_during_stalls(monkeypatch):
    now = 100.0
    monkeypatch.setattr(guide.time, "perf_counter", lambda: now)
    details = guide.GuideTimings()
    args = Namespace(follow_video=True)
    assert details.sections(102.9, args)[0][1][0] == ("FPS", "取帧 —  定位 —  坐标 —")

    for now in (100.25, 100.50, 101.00, 101.50, 102.00, 102.50):
        details.video_capture_started = now - 0.01
        details.record_video_capture(now, None, "remote", available=True)
    details.video_capture_started = 102.75
    details.record_video_capture(102.8, None, "remote", available=False)
    for now in (100.50, 101.50, 102.50):
        details.player_rates.record_capture(now)

    video = guide.LocationRate("攻略", details.video_rates)
    player = guide.LocationRate("玩家", details.player_rates)
    for now, published, reused in ((100.5, True, False), (101.5, False, False),
                                   (102.5, True, True), (103.0, True, False)):
        video.record(now - 0.01, published=published, reused=reused)
    for now, published in ((101.6, False), (102.6, True)):
        player.record(now - 0.01, published=published)

    video_section, player_section = details.sections(103.0, args)
    assert video_section[1][0] == ("FPS", "取帧 2.0  定位 1.3  坐标 0.7")
    assert player_section[1][0] == ("FPS", "取帧 1.0  定位 0.7  坐标 0.3")
    assert all(rows[0] == ("FPS", "取帧 0.0  定位 0.0  坐标 0.0")
               for _, rows in details.sections(106.0, args))
