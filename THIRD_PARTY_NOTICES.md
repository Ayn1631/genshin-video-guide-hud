# 第三方声明

## BetterGI

感谢 [BetterGI 开发者和贡献者](https://github.com/babalae/better-genshin-impact/graphs/contributors)。本项目的 v1.0.0 于 2026 年 11 月 21 日整理发布，将 BetterGI 的地图资产格式、坐标规则和遮罩、视角识别方法适配到 C++、Python 与攻略视频 HUD。[对应参考文件](README.md#致谢与源码出处) [BetterGI GPL v3 许可证](https://github.com/babalae/better-genshin-impact/blob/main/LICENSE)

BetterGI 地图、游戏界面图标与视频内容归各自权利人，使用时从本地 BetterGI 安装和 Edge 中打开的攻略获取。

## Windows 发布包的运行库

- OpenCV C++ SDK 4.10.0 及 `opencv-python-headless` 轮子。SDK 适用 Apache License 2.0，参考 SDK 许可证以及轮子随附的许可证和第三方声明。[OpenCV 4.10.0 源码及发行包](https://github.com/opencv/opencv/releases/tag/4.10.0) [Python 轮子构建源码](https://github.com/opencv/opencv-python/tree/4.10.0.84)
- CPython、Tcl 和 Tk 随发行包分发，用于运行界面和控制逻辑。[CPython 源码](https://github.com/python/cpython) [Tcl 源码](https://github.com/tcltk/tcl) [Tk 源码](https://github.com/tcltk/tk)
- NumPy、Pillow 和 websockets 随发行包分发，用于像素数组、界面绘制与 Edge 通信。[NumPy](https://github.com/numpy/numpy) [Pillow](https://github.com/python-pillow/Pillow) [websockets](https://github.com/python-websockets/websockets)
- PyInstaller bootloader 与运行期钩子用于打包，bootloader 适用其许可证中的发布例外。[PyInstaller 许可证](https://pyinstaller.org/en/stable/license.html) [PyInstaller 钩子源码](https://github.com/pyinstaller/pyinstaller-hooks-contrib)

发布包的 `licenses` 目录记录依赖版本与随附版权文件。当前发布包的对应项目源码和构建脚本由同一 Git 标签提供。[对应源码及发行版](https://github.com/Ayn1631/genshin-video-guide-hud/releases/tag/v1.0.0)
