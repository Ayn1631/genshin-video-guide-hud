# 原神视频攻略 HUD 的 C++ 定位组件

用 SIFT 特征和地图模板把攻略截图、小地图或原神游戏截图换算成 BetterGI 地图坐标。仓库提供 C++17 源码、CMake 构建脚本和 Windows x64 原生二进制。

## 从 Release 快速开始

1. 下载 [v1.0.0 C++ 发布包](https://github.com/Ayn1631/genshin-video-guide-hud/releases/tag/v1.0.0)中的 `GenshinVideoGuideHUD-1.0.0-windows-x64-cpp.zip`，完整解压到可写目录。
2. 准备完整的 [BetterGI 0.67.0](https://github.com/babalae/better-genshin-impact/releases/tag/0.67.0) 或兼容版本的地图资源。定位时将 `--assets` 指向 `Assets/Map/Teyvat`。独立地图资源沿用 `Assets/Map` 下的同级目录。
3. 在解压目录打开 PowerShell，将以下地图目录和图片路径改为本机路径，然后运行。

```powershell
.\map_locate.exe --assets 'D:\BetterGI\Assets\Map\Teyvat' --input video 'D:\Pictures\guide.png'
```

命令逐帧输出 JSON，成功时包含 `x`、`y`、`floor`、`scene`、匹配方法和耗时。使用 `--input minimap` 处理已裁好的小地图，使用 `--input game` 处理 16:9 游戏截图。`--help` 可查看完整选项。

## 发布包内容

| 文件 | 用途 |
| --- | --- |
| `map_locate.exe` | 命令行小地图定位 |
| `bgi_locator.dll` | 供其他程序集成的定位接口，入口见 [`cpptest/backend.cpp`](cpptest/backend.cpp) |
| `bgi_window_capture.dll` | 供集成调用的 Windows Graphics Capture 取帧接口，入口见 [`cpptest/window_capture.cpp`](cpptest/window_capture.cpp) |
| `opencv_world4100.dll` | OpenCV 4.10.0 原生运行库 |

如果系统提示缺少 `MSVCP140.dll` 或 `VCRUNTIME140.dll`，请安装微软的 x64 v14 运行库。[Microsoft 官方下载](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/)

## 构建源码

需要 Visual Studio 2022 Build Tools 的「使用 C++ 的桌面开发」工作负载、Windows 10/11 SDK、CMake 3.21 或更新版本，以及 [OpenCV 4.10.0 x64 SDK](https://github.com/opencv/opencv/releases/tag/4.10.0)。

```powershell
git clone https://github.com/Ayn1631/genshin-video-guide-hud.git
cd genshin-video-guide-hud
```

将 OpenCV 的 Windows SDK 解压到 `cpptest/_deps`，使 `cpptest/_deps/opencv/build/OpenCVConfig.cmake` 存在。确认 `cmake` 已加入 `PATH`，然后运行构建脚本。

```powershell
.\cpptest\build-msvc.cmd
.\cpptest\build-native\map_locate.exe --help
```

也可以显式指定 CMake 和 SDK 路径。

```powershell
.\cpptest\build-msvc.cmd 'C:\Tools\CMake\bin\cmake.exe' 'C:\SDKs\opencv\build'
```

构建产物在 `cpptest/build-native`。分发时将上表四个二进制放在同一目录，并随附项目与 OpenCV 的许可证。

## 匹配和索引

- 默认模式优先使用 SIFT，随后尝试地图模板。可通过 `--method sift` 或 `--method template` 单独测试。
- `--circle X Y R` 配合 `--inset` 可手动指定攻略小地图的位置与半径，单位按 640 像素宽的视频内容计算。
- SIFT 的 FLANN 索引默认存到可执行文件旁的 `index-cache`。可用 `--index-cache-dir` 指定路径，也可用 `--no-index-cache` 禁用磁盘缓存。

## 致谢与源码出处

感谢 [BetterGI](https://github.com/babalae/better-genshin-impact) 的维护者和贡献者。本项目的 C++ 定位适配了它的地图资源格式、坐标规则和小地图预处理思路。

- [`cpptest/sift.cpp`](cpptest/sift.cpp) 和 [`cpptest/map_scopes.hpp`](cpptest/map_scopes.hpp) 参考地图的特征资源与坐标原点。[TeyvatMap.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Maps/TeyvatMap.cs) · [SceneBaseMap.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Maps/Base/SceneBaseMap.cs)
- [`cpptest/template.cpp`](cpptest/template.cpp) 和 [`cpptest/image.cpp`](cpptest/image.cpp) 参考模板地图格式及原神小地图图标遮罩。[SceneBaseMapByTemplateMatch.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/Maps/Base/SceneBaseMapByTemplateMatch.cs) · [MaskCalculator.cs](https://github.com/babalae/better-genshin-impact/blob/main/BetterGenshinImpact/GameTask/Common/Map/MiniMap/MaskCalculator.cs)

项目采用 [GNU General Public License v3.0](LICENSE)。第三方授权和对应源码链接见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
