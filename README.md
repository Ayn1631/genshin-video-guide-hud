# 原神视频攻略 HUD

从 B 站攻略视频的小地图获取目标位置，在原神窗口上显示相对视角的方向和距离。玩家与攻略位置分别追踪。目标进入玩家的小地图范围时，红点标出攻略角色的位置，视角可识别时也会显示朝向。

定位核心使用 C++ 和 OpenCV。Windows 窗口取帧使用 C++ 与 Windows Graphics Capture，设置、Edge 取帧和 HUD 由 Python 驱动。

## 快速开始

1. 在 Windows x64 上下载 [v1.0.0 Windows 发布包](https://github.com/Ayn1631/genshin-video-guide-hud/releases/tag/v1.0.0)中的 `GenshinVideoGuideHUD-1.0.0-windows-x64.zip`，完整解压到当前账户可写的目录。双击 `GenshinVideoGuideHUD.exe` 打开设置。包内带 Python 和 OpenCV 运行库，`_internal` 与 `build-native` 目录须与主程序放在一起。
2. 完整解压 [BetterGI 0.67.0](https://github.com/babalae/better-genshin-impact/releases/tag/0.67.0) 或准备已有的兼容安装。在 HUD 设置的「高级」中，把「地图资源目录」选为该安装下的 `Assets/Map/Teyvat`。请保留 BetterGI 的目录结构，程序也从同一个安装读取 `GameTask/Common/Element/Assets/1920x1080/paimon_menu.png`。使用其他地图时，请保留 `Assets/Map` 下的同级地图目录。BetterGI 可以保持关闭。
3. 在平时使用的 Microsoft Edge 打开 B 站攻略视频，选取画面中可见小地图的一段。在「Edge 和小地图」中选择取帧模式。圆心、半径和内缩以 640 像素宽的**视频内容**为基准，默认值适用于小地图在左上角放大的攻略布局。其他视频布局需要调整这些值。
4. 打开原神，使用 16:9 窗口，建议 1920×1080 或更高分辨率。将小地图设为「锁定方向」，回到可见小地图的游戏主界面。如果游戏窗口标题与默认的 `原神` 不同，在「高级」填入实际标题。点击「应用并启动 HUD」，然后切回原神。游戏被遮挡或离开前台时，指引随之隐藏。按 `Ctrl+Alt+F10` 或点击设置里的「停止 HUD」可结束指引。

如果启动时提示缺少 `MSVCP140.dll` 或 `VCRUNTIME140.dll`，安装微软提供的 x64 v14 运行库。[Microsoft 官方下载](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)

### 选择 Edge 取帧方式

| 模式 | 启动前准备 | 使用条件 |
| --- | --- | --- |
| 远程模式 | 在同一个 Edge 中打开 `edge://inspect/#remote-debugging`，开启「Allow remote debugging for this browser instance」。Edge 弹窗时允许本地连接。 | 直接读取攻略视频的小地图区域，Edge 可最小化。 |
| 唤醒模式 | 同样开启并允许本地调试。 | Edge 保持还原，小地图留在视口内。通常直接读取视频像素，遇到 canvas 跨域限制时才截取视口并在本地裁剪。 |
| Windows 捕获 | 进入「框选视频画面」，沿实际视频边界拖框。播放器填满窗口时可选择「全屏视频用整窗」。 | 通过 Windows 捕获原窗口并在 GPU 上裁剪小地图。该模式无需 Edge 调试授权。浏览器遮挡后仍需继续出帧，窗口尺寸或播放器布局变化后可能要重新框选。 |

Windows 捕获优先选择窗口标题含 `bilibili`、`哔哩哔哩` 或 `B站` 的 Edge 窗口。打开多个攻略窗口时，可在「攻略窗口标题（正则）」填入当前标题中特有的文字再框选。

在「重试与帧率」中开启「详细模式」，可在游戏左侧查看两路最近 3 秒的取帧、定位处理和坐标更新频率，以及取帧、匹配、视角识别等阶段的耗时。「仅保存」保存配置。「应用并启动 HUD」用当前参数启动指引。

首次攻略定位搜索所有可用地图。匹配后优先局部追踪，达到连续失败门槛时再扩大到所在国家或独立地图，最后全图搜索。玩家随攻略使用同一地图块。SIFT 索引可保存在 `build-native/index-cache`，后续启动时可复用。发布包的设置与日志位于主程序旁的 `output`，从源码运行时则位于 `cpptest/output`。

## 从源码构建

构建环境为 Windows x64、带 Tcl/Tk 的 64 位 Python 3.11、CMake 3.21 或更新版本、Visual Studio 2022 Build Tools 的「使用 C++ 的桌面开发」工作负载和 Windows 10/11 SDK。C++ 后端使用 OpenCV 4.10.0 x64 SDK。[OpenCV 4.10.0 官方发行包](https://github.com/opencv/opencv/releases/tag/4.10.0)

先克隆仓库，用 Python 3.11 x64 创建虚拟环境。若使用 `python` 命令，请先确认它指向同一版本。

```powershell
git clone https://github.com/Ayn1631/genshin-video-guide-hud.git
cd genshin-video-guide-hud
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

把 OpenCV 官方 Windows 包解压到 `cpptest/_deps`，确保 `cpptest/_deps/opencv/build/OpenCVConfig.cmake` 存在。以下示例假定 `cmake` 已加入 `PATH`。

```powershell
.\cpptest\build-msvc.cmd
.\.venv\Scripts\python.exe -B .\cpptest\run_hud.py --settings
```

也可向构建脚本显式传入 `cmake.exe` 和包含 `OpenCVConfig.cmake` 的 SDK 目录。然后按快速开始配置 BetterGI 地图目录与 Edge。

```powershell
.\cpptest\build-msvc.cmd 'C:\Tools\CMake\bin\cmake.exe' 'C:\SDKs\opencv\build'
```

构建产物在 `cpptest/build-native`。独立工具 `map_locate.exe` 可用 `--assets` 指定地图目录，给定图片路径后输出 JSON 定位结果。

```powershell
.\cpptest\build-native\map_locate.exe --assets 'D:\BetterGI\Assets\Map\Teyvat' 'D:\Pictures\guide.png'
```

要打出与 Release 同结构的 Windows ZIP，先完成 C++ 构建，再运行以下命令。打包会保留 Python 安装附带的许可证。产物和 SHA-256 校验文件默认生成到 `.release-work`。

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements-release.txt
.\.venv\Scripts\python.exe -B .\build_release.py --version 1.0.0
```

如果构建时用了自定义 SDK 目录，打包时也要提供它的许可证。

```powershell
.\.venv\Scripts\python.exe -B .\build_release.py --version 1.0.0 --opencv-license 'C:\SDKs\opencv\build\LICENSE'
```

## 致谢与源码出处

感谢 BetterGI 的维护者与贡献者。这个 HUD 借助 BetterGI 的地图资源、定位实现与视角识别思路，把游戏画面与原视频里的小地图放在同一坐标系。以下链接可以对照实现。[BetterGI 上游仓库](https://github.com/babalae/better-genshin-impact)

| 本项目 | BetterGI 参考 |
| --- | --- |
| `cpptest/sift.cpp`、`cpptest/map_scopes.hpp`、`bettergi_map_localizer.py` 中的 SIFT 资源读取、地图坐标与原点 | [TeyvatMap.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Maps/TeyvatMap.cs)、[SceneBaseMap.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Maps/Base/SceneBaseMap.cs) |
| `cpptest/template.cpp`、`cpptest/image.cpp` 中的地图模板与小地图图标遮罩 | [SceneBaseMapByTemplateMatch.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Maps/Base/SceneBaseMapByTemplateMatch.cs)、[MaskCalculator.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/MiniMap/MaskCalculator.cs) |
| `minimap_heading.py` 中的彩色扇区与极坐标视角估计 | [CameraOrientationCalculator.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/MiniMap/CameraOrientationCalculator.cs)、[CameraOrientationFromGia.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Camera/CameraOrientationFromGia.cs) |

源码与发行版采用 **GNU General Public License v3.0**。BetterGI 的原始实现同样按 GPL v3 授权。第三方运行库适用各自的许可证。地图资源、游戏图标以及攻略内容由用户本地提供。相关版权与依赖出处见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，本项目许可证全文见 [LICENSE](LICENSE)。
