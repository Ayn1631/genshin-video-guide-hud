# 原神视频攻略 HUD

从 Edge 的 B 站攻略视频和原神的小地图提取坐标，在游戏窗口上显示攻略方向和距离。目标进入玩家小地图范围时，在原神小地图上标出红色目标和可识别的攻略视角。仓库提供 C++17 源码及 Windows x64 发布包。

## 从 Release 快速开始

1. 下载 [GitHub Release](https://github.com/Ayn1631/genshin-video-guide-hud/releases/latest) 中的 Windows x64 C++ ZIP，完整解压到可写目录。
2. **双击 `GenshinVideoGuideHUD.exe` 打开配置。** 在「地图资源目录」选择 [BetterGI](https://github.com/babalae/better-genshin-impact/releases) 的 `Assets/Map/Teyvat`。独立地图资源沿用 `Assets/Map` 下的同级目录。
3. 在日常使用的 Edge 中播放 B 站攻略，再从下表选择取帧方式。固定小地图默认坐标以 640 × 360 的视频内容为基准。小地图位置有偏移时，在配置中调整圆心和半径。
4. 点击「应用并启动 HUD」，切回原神的 16:9 游戏主画面。HUD 跟随游戏窗口，在游戏切到后台、遮挡或最小化时隐藏。
5. **关闭配置窗口即可退出整个程序。** HUD 和后台取帧随之结束。「停止 HUD」可单独结束 HUD 和取帧，方便保留配置窗口。

| 取帧方式 | 使用方法 |
| --- | --- |
| 远程调试 | 在播放攻略的 Edge 中打开 `edge://inspect/#remote-debugging`，启用该实例的远程调试，按 Edge 提示允许本地连接。支持 Edge 最小化。 |
| 唤醒模式 | 使用同一个本地调试连接。保持 Edge 窗口还原、攻略标签页选中，视频区域在页面视口内。 |
| Windows 窗口捕获 | 保持 Edge 还原并选中攻略标签页。点「框选视频…」，在预览中框住视频画面。通过 Windows 窗口捕获读取该区域里的小地图，无需 Edge 调试授权。 |

配置保存在程序同目录的 `guide-settings.ini`。打开「显示详细耗时与帧率」，可以在游戏左侧查看玩家和攻略两条流程的取帧、匹配和视角耗时。

## 命令行定位

如需单独定位截图，在解压目录打开 PowerShell，将地图目录和图片路径改为本机路径后运行。

```powershell
.\map_locate.exe --assets 'D:\BetterGI\Assets\Map\Teyvat' --input video 'D:\Pictures\guide.png'
```

命令逐帧输出 JSON，成功时包含 `x`、`y`、`floor`、`scene`、匹配方法和耗时。使用 `--input minimap` 处理已裁好的小地图，使用 `--input game` 处理 16:9 游戏截图。`--help` 可查看完整选项。

## 发布包内容

| 文件 | 用途 |
| --- | --- |
| `GenshinVideoGuideHUD.exe` | 配置窗口与原生 HUD |
| `map_locate.exe` | 命令行小地图定位 |
| `bgi_locator.dll` | 供其他程序集成的定位接口，入口见 [`cpptest/backend.cpp`](cpptest/backend.cpp) |
| `bgi_window_capture.dll` | 供集成调用的 Windows Graphics Capture 取帧接口，入口见 [`cpptest/window_capture.cpp`](cpptest/window_capture.cpp) |
| `opencv_world4100.dll` | OpenCV 4.10.0 原生运行库 |

发布包自带 OpenCV 运行库。系统提示缺少 `MSVCP140.dll` 或 `VCRUNTIME140.dll` 时，安装微软的 x64 v14 运行库。[Microsoft 官方下载](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)

## 构建源码

需要 Visual Studio 2022 Build Tools 的「使用 C++ 的桌面开发」工作负载、Windows 10/11 SDK、CMake 3.21 或更新版本，以及 [OpenCV 4.10.0 x64 SDK](https://github.com/opencv/opencv/releases/tag/4.10.0)。

```powershell
git clone https://github.com/Ayn1631/genshin-video-guide-hud.git
cd genshin-video-guide-hud
```

将 OpenCV 的 Windows SDK 解压到 `cpptest/_deps`，使 `cpptest/_deps/opencv/build/OpenCVConfig.cmake` 存在。确认 `cmake` 已加入 `PATH`，然后运行构建脚本。

```powershell
.\cpptest\build-msvc.cmd
.\cpptest\build-native\GenshinVideoGuideHUD.exe
```

也可以显式指定 CMake 和 SDK 路径。

```powershell
.\cpptest\build-msvc.cmd 'C:\Tools\CMake\bin\cmake.exe' 'C:\SDKs\opencv\build'
```

构建产物在 `cpptest/build-native`。分发时将上表的 EXE 和 DLL 放在同一目录，并随附项目与 OpenCV 的许可证。

## 匹配和索引

- 默认模式优先使用 SIFT，随后尝试地图模板。可通过 `--method sift` 或 `--method template` 单独测试。
- `--circle X Y R` 配合 `--inset` 可手动指定攻略小地图的位置与半径，单位按 640 像素宽的视频内容计算。
- SIFT 的 FLANN 索引默认存到可执行文件旁的 `index-cache`。可用 `--index-cache-dir` 指定路径，也可用 `--no-index-cache` 禁用磁盘缓存。

## 致谢与源码出处

感谢 [BetterGI](https://github.com/babalae/better-genshin-impact) 的维护者和贡献者。本项目的 C++ 定位适配了它的地图资源格式、坐标规则和小地图预处理思路。

- [`cpptest/sift.cpp`](cpptest/sift.cpp) 和 [`cpptest/map_scopes.hpp`](cpptest/map_scopes.hpp) 参考地图的特征资源与坐标原点。[TeyvatMap.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Maps/TeyvatMap.cs) · [SceneBaseMap.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Maps/Base/SceneBaseMap.cs)
- [`cpptest/template.cpp`](cpptest/template.cpp) 和 [`cpptest/image.cpp`](cpptest/image.cpp) 参考模板地图格式及原神小地图图标遮罩。[SceneBaseMapByTemplateMatch.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Maps/Base/SceneBaseMapByTemplateMatch.cs) · [MaskCalculator.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/MiniMap/MaskCalculator.cs)

项目采用 [GNU General Public License v3.0](LICENSE)。第三方授权和对应源码链接见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
