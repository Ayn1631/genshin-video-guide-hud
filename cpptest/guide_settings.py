"""Windows settings window for the live guide."""

from __future__ import annotations

import ctypes as ct
from ctypes import wintypes as wt
import json
import math
import os
from pathlib import Path
from queue import Empty, Queue
import re
import subprocess
import sys
from threading import Thread
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageTk

from run_hud import DEFAULT_LOCAL_MISSES, DEFAULT_REGION_MISSES, FROZEN, ROOT, guide
from window_video_shot import FULL_WINDOW, WindowsEdgeVideo, window_video_rect


OUTPUT = ROOT / "output"
SETTINGS = OUTPUT / "guide-settings.json"
LOG = OUTPUT / "game-video-guide.log"
ERROR_LOG = OUTPUT / "game-video-guide-error.log"
EDGE_MODES = {
    "远程模式 · 支持最小化": "remote",
    "唤醒模式 · Edge 窗口需还原": "wake",
    "Windows 捕获 · 无需 Edge 调试": "windows",
}
MATCH_METHODS = {"自动 · 先 SIFT 后模板": "auto", "仅 SIFT": "sift", "仅模板": "template"}
FULL_WINDOW_SETTING = "0 0 1 1"
DEFAULTS = {
    "local_misses": DEFAULT_LOCAL_MISSES, "region_misses": DEFAULT_REGION_MISSES,
    "video_fps": 10, "player_fps": 10, "arrival_distance": guide._ARRIVAL_DISTANCE,
    "edge_mode": next(iter(EDGE_MODES)), "circle_x": 94.5, "circle_y": 90.5,
    "circle_radius": 95.1, "inset": 11.4, "video_pattern": r"bilibili\.com",
    "map_workers": 4, "opencv_threads": 2, "index_cache": True, "detailed_hud": False,
    "window_video_rect": FULL_WINDOW_SETTING,
    "video_method": next(iter(MATCH_METHODS)), "game_title": "原神", "assets": str(guide._ASSETS),
}


def load_settings(path: Path = SETTINGS) -> dict:
    if not path.exists():
        return DEFAULTS.copy()
    values = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(values, dict):
        raise ValueError("设置文件需要包含参数对象")
    return {key: values.get(key, default) for key, default in DEFAULTS.items()}


def build_arguments(values: dict) -> list[str]:
    def number(key: str, label: str, minimum: float, maximum: float, *, integer: bool = False):
        try:
            parsed = int(values[key]) if integer else float(values[key])
        except (TypeError, ValueError):
            raise ValueError(f"{label}需要填写{'整数' if integer else '数字'}") from None
        if not math.isfinite(parsed) or not minimum <= parsed <= maximum:
            raise ValueError(f"{label}需要在 {minimum:g} 至 {maximum:g} 之间")
        return parsed

    local = number("local_misses", "局部连续失败帧数", 1, 120, integer=True)
    regional = number("region_misses", "区域连续失败次数", 1, 120, integer=True)
    video_fps = number("video_fps", "攻略取帧频率", 0.1, 120)
    player_fps = number("player_fps", "玩家取帧频率", 0.1, 120)
    arrival = number("arrival_distance", "到达判定距离", 0, 10000)
    x = number("circle_x", "圆心 X", 0, 639)
    y = number("circle_y", "圆心 Y", 0, 359)
    radius = number("circle_radius", "半径", 1, 320)
    inset = number("inset", "内缩", 0, 319)
    half = radius - inset
    if half < 1 or half >= min(x, y, 640 - x, 360 - y):
        raise ValueError("内缩后的固定取帧区域须在 640×360 画面内")
    workers = number("map_workers", "地图块并发数", 1, 32, integer=True)
    threads = number("opencv_threads", "OpenCV 线程数", 1, 64, integer=True)
    try:
        mode = EDGE_MODES[values["edge_mode"]]
        method = MATCH_METHODS[values["video_method"]]
    except KeyError:
        raise ValueError("请选择有效的 Edge 模式和匹配算法") from None
    pattern = values["video_pattern"].strip()
    try:
        re.compile(pattern)
    except re.error as error:
        raise ValueError(f"攻略标签筛选表达式有误：{error}") from None
    title = values["game_title"].strip()
    assets = Path(values["assets"]).expanduser()
    if not title or not values["assets"].strip() or not assets.is_dir():
        raise ValueError("请填写游戏窗口标题并选择有效的地图资源目录")
    if not guide.paimon_icon_path(assets).is_file():
        raise ValueError("未找到派蒙菜单图标。请选择完整 BetterGI 安装下的 Assets/Map/Teyvat 目录。")

    args = [
        "--edge-existing", "--follow-video", "--edge-mode", mode,
        "--local-misses", str(local), "--region-misses", str(regional),
        "--video-fps", str(video_fps), "--player-fps", str(player_fps),
        "--arrival-distance", str(arrival), "--video-circle", str(x), str(y), str(radius),
        "--video-inset", str(inset), "--map-workers", str(workers), "--opencv-threads", str(threads),
        "--video-method", method, "--game-title", title, "--assets", str(assets.resolve()),
    ]
    if pattern:
        args.extend(("--video-pattern", pattern))
    if mode == "windows":
        rect = window_video_rect(values.get("window_video_rect", FULL_WINDOW_SETTING).split())
        args.extend(("--window-video-rect", *map(str, rect)))
    if values.get("detailed_hud", False):
        args.append("--detailed-hud")
    if not values["index_cache"]:
        args.append("--no-index-cache")
    return args


class HudWindows:
    def __init__(self) -> None:
        self.user32 = ct.WinDLL("user32", use_last_error=True)
        self.callback = ct.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        self.user32.EnumWindows.argtypes = (self.callback, wt.LPARAM)
        self.user32.GetClassNameW.argtypes = (wt.HWND, wt.LPWSTR, ct.c_int)
        self.user32.PostMessageW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

    def find(self) -> list[int]:
        handles = []

        @self.callback
        def collect(hwnd, _data):
            name = ct.create_unicode_buffer(128)
            self.user32.GetClassNameW(hwnd, name, len(name))
            if re.fullmatch(r"BetterGIVideoGuide_[0-9]+", name.value):
                handles.append(hwnd)
            return True

        self.user32.EnumWindows(collect, 0)
        return handles

    def request_stop(self, hwnd: int) -> None:
        if not self.user32.PostMessageW(hwnd, 0x0312, 2, 0):
            raise ct.WinError(ct.get_last_error())


class SettingsWindow:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.windows = HudWindows()
        self.process: subprocess.Popen | None = None
        self.error_offset = 0
        self.busy = False
        self.preview_pending = False
        root.title("攻略 HUD 设置")
        root.geometry("680x525")
        root.minsize(635, 485)
        root.option_add("*Font", ("Microsoft YaHei UI", 10))
        self.status = tk.StringVar(value="已检测到运行中的 HUD" if self.windows.find() else "HUD 未运行")
        try:
            values = load_settings()
        except (OSError, ValueError) as error:
            values = DEFAULTS
            self.status.set(f"设置文件读取失败：{error}")
        self.vars = {
            key: (tk.BooleanVar(value=value) if isinstance(DEFAULTS[key], bool) else tk.StringVar(value=value))
            for key, value in values.items()
        }

        content = ttk.Frame(root, padding=(22, 16))
        content.pack(fill="both", expand=True)
        ttk.Label(content, text="攻略 HUD 设置", font=("Microsoft YaHei UI", 17, "bold")).pack(anchor="w", pady=(0, 12))
        tabs = ttk.Notebook(content)
        tabs.pack(fill="both", expand=True)
        tracking = self.add_tab(tabs, "重试与帧率")
        self.entry(tracking, 0, "local_misses", "区域搜索前的局部失败帧数")
        self.entry(tracking, 1, "region_misses", "全图重试前的区域失败次数")
        self.entry(tracking, 2, "video_fps", "攻略取帧（帧/秒）")
        self.entry(tracking, 3, "player_fps", "玩家取帧（帧/秒）")
        self.entry(tracking, 4, "arrival_distance", "到达判定（地图单位）")
        self.hud_rate = tk.StringVar()
        ttk.Label(tracking, textvariable=self.hud_rate).grid(row=5, column=0, columnspan=3, sticky="w", pady=(8, 0))
        for key in ("video_fps", "player_fps"):
            self.vars[key].trace_add("write", self.update_hud_rate)
        self.update_hud_rate()
        ttk.Checkbutton(tracking, text="详细模式 · 在游戏左侧显示实测帧率和耗时",
                        variable=self.vars["detailed_hud"]).grid(row=6, column=0, columnspan=2, sticky="w", pady=(10, 0))

        edge = self.add_tab(tabs, "Edge 和小地图")
        self.choice(edge, 0, "edge_mode", "Edge 取帧模式", EDGE_MODES)
        self.edge_help = tk.StringVar()
        ttk.Label(edge, textvariable=self.edge_help, wraplength=560).grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(1, 6))
        self.entry(edge, 2, "circle_x", "小地图圆心 X")
        self.entry(edge, 3, "circle_y", "小地图圆心 Y")
        self.entry(edge, 4, "circle_radius", "小地图半径")
        self.entry(edge, 5, "inset", "取帧内缩（按 640 像素宽）")
        self.pattern_label = self.entry(edge, 6, "video_pattern", "攻略标签筛选（正则）")
        self.video_rect_controls = ttk.Frame(edge)
        self.video_rect_controls.grid(row=7, column=0, columnspan=3, sticky="w", pady=(7, 0))
        self.rect_note = tk.StringVar()
        ttk.Label(self.video_rect_controls, textvariable=self.rect_note).pack(side="left", padx=(0, 10))
        self.preview_button = ttk.Button(self.video_rect_controls, text="框选视频画面…", command=self.pick_video_rect)
        self.preview_button.pack(side="left")
        ttk.Button(self.video_rect_controls, text="全屏视频用整窗", command=self.reset_video_rect).pack(side="left", padx=8)
        for key in ("edge_mode", "window_video_rect"):
            self.vars[key].trace_add("write", self.update_edge_mode)
        self.update_edge_mode()

        advanced = self.add_tab(tabs, "高级")
        self.choice(advanced, 0, "video_method", "攻略匹配算法", MATCH_METHODS)
        self.entry(advanced, 1, "map_workers", "地图块并发数")
        self.entry(advanced, 2, "opencv_threads", "OpenCV 线程数")
        self.entry(advanced, 3, "game_title", "游戏窗口标题")
        self.entry(advanced, 4, "assets", "地图资源目录")
        ttk.Button(advanced, text="浏览", command=self.pick_assets).grid(row=4, column=2, padx=(8, 0))
        ttk.Checkbutton(advanced, text="保存 SIFT 索引缓存", variable=self.vars["index_cache"]).grid(
            row=5, column=0, columnspan=2, sticky="w", pady=8)

        footer = ttk.Frame(content)
        footer.pack(fill="x", pady=(12, 0))
        ttk.Button(footer, text="恢复默认", command=self.restore_defaults).pack(side="left")
        ttk.Button(footer, text="仅保存", command=self.save).pack(side="left", padx=8)
        self.apply_button = ttk.Button(footer, text="应用并启动 HUD", command=self.apply)
        self.apply_button.pack(side="right")
        self.stop_button = ttk.Button(footer, text="停止 HUD", command=self.stop)
        self.stop_button.pack(side="right", padx=8)
        ttk.Label(content, textvariable=self.status, wraplength=570).pack(anchor="w", pady=(8, 0))
        self.root.after(500, self.monitor)

    @staticmethod
    def add_tab(tabs, name):
        tab = ttk.Frame(tabs, padding=(14, 10))
        tab.columnconfigure(1, weight=1)
        tabs.add(tab, text=name)
        return tab

    def entry(self, tab, row, key, label):
        title = ttk.Label(tab, text=label)
        title.grid(row=row, column=0, sticky="w", padx=(0, 16), pady=5)
        ttk.Entry(tab, textvariable=self.vars[key]).grid(row=row, column=1, sticky="ew", pady=5)
        return title

    def choice(self, tab, row, key, label, choices):
        ttk.Label(tab, text=label).grid(row=row, column=0, sticky="w", padx=(0, 16), pady=5)
        ttk.Combobox(tab, textvariable=self.vars[key], values=tuple(choices), state="readonly").grid(
            row=row, column=1, sticky="ew", pady=5)

    def update_hud_rate(self, *_args):
        try:
            rates = [float(self.vars[key].get()) for key in ("video_fps", "player_fps")]
            if not all(math.isfinite(rate) and rate > 0 for rate in rates):
                raise ValueError
        except ValueError:
            self.hud_rate.set("HUD 刷新频率 · 等待有效取帧频率")
        else:
            self.hud_rate.set(f"HUD 刷新频率 · {max(rates) + 5:g} 帧/秒（两路较快值 ＋ 5）")

    def pick_assets(self):
        directory = filedialog.askdirectory(initialdir=self.vars["assets"].get(), parent=self.root)
        if directory:
            self.vars["assets"].set(directory)

    def update_edge_mode(self, *_args):
        mode = EDGE_MODES.get(self.vars["edge_mode"].get())
        help_text = {
            "remote": "复用原 Edge。允许本地调试后，最小化仍可读视频。",
            "wake": "复用原 Edge。需要本地调试授权，且攻略小地图须在已还原窗口的视口内。",
            "windows": "复用原 Edge，无需调试授权。让攻略在窗口内播放，普通网页请先框选视频画面。",
        }
        self.edge_help.set(help_text.get(mode, ""))
        self.pattern_label.configure(text="攻略窗口标题（正则）" if mode == "windows" else "攻略标签筛选（正则）")
        if mode == "windows":
            self.video_rect_controls.grid()
            try:
                rect = window_video_rect(self.vars["window_video_rect"].get().split())
            except ValueError:
                self.rect_note.set("视频范围无效")
            else:
                self.rect_note.set("视频范围：整窗" if rect == FULL_WINDOW else "视频范围：已框选")
        else:
            self.video_rect_controls.grid_remove()

    def reset_video_rect(self):
        self.vars["window_video_rect"].set(FULL_WINDOW_SETTING)

    def pick_video_rect(self):
        if self.preview_pending:
            return
        title = self.vars["video_pattern"].get().strip()
        result = Queue()

        def capture():
            try:
                with WindowsEdgeVideo(title_pattern=title) as edge:
                    result.put(Image.fromarray(edge.snapshot()))
            except Exception as error:
                result.put(error)

        def wait():
            try:
                preview = result.get_nowait()
            except Empty:
                self.root.after(50, wait)
                return
            self.preview_pending = False
            self.preview_button.configure(state="normal")
            if isinstance(preview, Exception):
                self.status.set(str(preview))
            elif EDGE_MODES.get(self.vars["edge_mode"].get()) == "windows":
                self.status.set("在预览中拖动框选视频画面")
                self.show_video_rect_preview(preview)

        self.preview_pending = True
        self.preview_button.configure(state="disabled")
        self.status.set("正在截取一次 Edge 窗口作为预览…")
        Thread(target=capture, daemon=True).start()
        self.root.after(50, wait)

    def show_video_rect_preview(self, preview: Image.Image):
        preview.thumbnail((round(self.root.winfo_screenwidth() * 0.84),
                           round(self.root.winfo_screenheight() * 0.71)), Image.Resampling.LANCZOS)
        width, height = preview.size
        dialog = tk.Toplevel(self.root)
        dialog.title("框选攻略视频画面")
        dialog.transient(self.root)
        dialog.resizable(False, False)
        ttk.Label(dialog, text="从视频画面的左上角拖到右下角，沿画面边缘框选。", padding=10).pack(anchor="w")
        canvas = tk.Canvas(dialog, width=width, height=height, highlightthickness=0, cursor="crosshair")
        canvas.pack()
        picture = ImageTk.PhotoImage(preview, master=dialog)
        canvas.image = picture
        canvas.create_image(0, 0, anchor="nw", image=picture)
        outline = canvas.create_rectangle(0, 0, 0, 0, outline="#5cf5ff", width=2)
        anchor = None

        def begin(event):
            nonlocal anchor
            anchor = (min(width, max(0, event.x)), min(height, max(0, event.y)))
            drag(event)

        def drag(event):
            if anchor is not None:
                canvas.coords(outline, *anchor, min(width, max(0, event.x)), min(height, max(0, event.y)))

        def accept():
            x1, y1, x2, y2 = canvas.coords(outline)
            left, top = min(x1, x2), min(y1, y2)
            w, h = abs(x2 - x1), abs(y2 - y1)
            if min(w, h) < 12:
                messagebox.showerror("视频画面", "请在预览中框出整个视频画面", parent=dialog)
                return
            values = (left / width, top / height, w / width, h / height)
            self.vars["window_video_rect"].set(" ".join(map(str, values)))
            self.status.set("已框选视频画面，点击保存或应用")
            dialog.destroy()

        canvas.bind("<Button-1>", begin)
        canvas.bind("<B1-Motion>", drag)
        canvas.bind("<ButtonRelease-1>", drag)
        buttons = ttk.Frame(dialog, padding=8)
        buttons.pack(fill="x")
        ttk.Button(buttons, text="使用此区域", command=accept).pack(side="right")
        ttk.Button(buttons, text="取消", command=dialog.destroy).pack(side="right", padx=8)

    def restore_defaults(self):
        for key, value in DEFAULTS.items():
            self.vars[key].set(value)
        self.status.set("已恢复默认值，点击保存或应用")

    def save(self):
        values = {key: variable.get() for key, variable in self.vars.items()}
        try:
            args = build_arguments(values)
            SETTINGS.parent.mkdir(parents=True, exist_ok=True)
            temporary = SETTINGS.with_suffix(".tmp")
            temporary.write_text(json.dumps(values, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            temporary.replace(SETTINGS)
        except (OSError, ValueError) as error:
            messagebox.showerror("无法保存设置", str(error), parent=self.root)
            return None
        self.status.set("设置已保存")
        return args

    def apply(self):
        args = self.save()
        if args is not None:
            self.stop_then(lambda: self.start(args))

    def stop(self):
        self.stop_then(lambda: self.status.set("HUD 已停止"))

    def set_busy(self, value: bool):
        self.busy = value
        for button in (self.apply_button, self.stop_button):
            button.configure(state="disabled" if value else "normal")

    def stop_then(self, action):
        self.set_busy(True)
        self.status.set("等待现有 HUD 停止…")
        deadline = time.monotonic() + 40
        signalled = set()

        def check():
            try:
                handles = self.windows.find()
                for hwnd in handles:
                    if hwnd not in signalled:
                        self.windows.request_stop(hwnd)
                        signalled.add(hwnd)
            except OSError as error:
                self.status.set(f"无法停止 HUD：{error}")
                self.set_busy(False)
                return
            running = self.process is not None and self.process.poll() is None
            if not handles and not running:
                self.process = None
                self.set_busy(False)
                action()
            elif time.monotonic() >= deadline:
                self.set_busy(False)
                self.status.set("HUD 仍在结束取帧，请稍后重试")
            else:
                self.root.after(150, check)

        check()

    def start(self, args: list[str]):
        executable = Path(sys.executable)
        if FROZEN:
            command = [str(executable), *args]
        else:
            if executable.name.lower() == "pythonw.exe":
                executable = executable.with_name("python.exe")
            command = [str(executable), "-B", "-X", "utf8", "-u", str(ROOT / "run_hud.py"), *args]
        try:
            with LOG.open("ab") as log, ERROR_LOG.open("ab") as errors:
                self.error_offset = errors.tell()
                environment = dict(os.environ, PYINSTALLER_RESET_ENVIRONMENT="1") if FROZEN else None
                self.process = subprocess.Popen(command, cwd=ROOT if FROZEN else ROOT.parent,
                                                stdout=log, stderr=errors, env=environment,
                                                creationflags=subprocess.CREATE_NO_WINDOW)
        except OSError as error:
            self.status.set(f"无法启动 HUD：{error}")
            return
        mode = args[args.index("--edge-mode") + 1]
        self.status.set("HUD 正在启动，正在连接 Windows 捕获" if mode == "windows"
                        else "HUD 正在启动，必要时在原 Edge 允许本地连接")

    def monitor(self):
        if not self.busy and self.process is not None:
            code = self.process.poll()
            if code is not None:
                self.process = None
                if code == 0:
                    self.status.set("HUD 已停止")
                else:
                    with ERROR_LOG.open("rb") as errors:
                        errors.seek(self.error_offset)
                        lines = errors.read().decode("utf-8", errors="replace").strip().splitlines()
                    self.status.set(lines[-1] if lines else f"HUD 退出（代码 {code}）")
            elif self.windows.find():
                if self.status.get().startswith("HUD 正在启动"):
                    self.status.set("HUD 已运行")
        self.root.after(500, self.monitor)


def main():
    root = tk.Tk()
    SettingsWindow(root)
    root.mainloop()


if __name__ == "__main__":
    main()
