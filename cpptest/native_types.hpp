#pragma once

#include <windows.h>

#include "map_locator.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace guide {

enum class CaptureMode { remote, wake, windows };
enum class EdgeEndpointMode { automatic, directory, port };

inline constexpr double min_capture_fps = 0.1;
inline constexpr double max_capture_fps = 120;
inline constexpr int max_retry_count = 120;
inline constexpr double max_arrival_distance = 10000;
inline constexpr int max_opencv_threads = 64;

inline double hud_fps(double guide_fps, double player_fps) {
    return std::max(guide_fps, player_fps) + 5;
}

struct NormalizedRect {
    double x = 0;
    double y = 0;
    double width = 1;
    double height = 1;
};

struct VideoCircle {
    double x = 94.5;
    double y = 90.5;
    double radius = 95.1;
};

struct CaptureWindow {
    std::uintptr_t handle = 0;
    std::uint32_t process_id = 0;
    std::wstring executable;
    std::wstring window_class;
    std::wstring title;
};

struct WindowMinimap {
    NormalizedRect bounds;
    double inset_fraction = 0;
};

inline std::optional<NormalizedRect> bounded_video_rect(NormalizedRect rect) {
    constexpr double tolerance = 0.00000001;
    if (!(rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0
        && rect.x < 1 && rect.y < 1 && rect.x + rect.width <= 1 + tolerance
        && rect.y + rect.height <= 1 + tolerance)) {
        return std::nullopt;
    }
    rect.width = std::min(rect.width, 1 - rect.x);
    rect.height = std::min(rect.height, 1 - rect.y);
    return rect;
}

struct GuideSettings {
    std::filesystem::path assets;
    std::wstring game_title = L"原神";
    bool hide_when_covered = true;
    std::wstring edge_pattern = L"bilibili\\.com";
    CaptureMode capture_mode = CaptureMode::remote;
    EdgeEndpointMode edge_endpoint = EdgeEndpointMode::automatic;
    std::wstring edge_debug_directory;
    std::uint16_t edge_debug_port = 0;
    std::optional<CaptureWindow> capture_window;
    std::optional<WindowMinimap> window_minimap;
    NormalizedRect video_rect;
    VideoCircle circle;
    double inset = 11.4;
    double video_fps = 10;
    double player_fps = 10;
    int local_misses = 3;
    int region_misses = 2;
    double arrival_distance = 2;
    int workers = 4;
    int opencv_threads = 2;
    bool cache_indexes = true;
    bool detailed = false;
    bgi::Method method = bgi::Method::automatic;
};

GuideSettings load_settings(const std::filesystem::path& root);
GuideSettings default_settings(const std::filesystem::path& root);
void save_settings(const std::filesystem::path& root, const GuideSettings& settings);
bool valid_assets(const std::filesystem::path& directory);
std::wstring setting_number(double value);
std::optional<double> parse_setting_number(const std::wstring& value);
std::optional<std::uint16_t> parse_edge_debug_port(std::wstring_view value);
std::filesystem::path expand_edge_debug_directory(const std::wstring& value);
int run_hud_worker(HINSTANCE instance, const GuideSettings& settings,
                   const std::filesystem::path& root, HANDLE stop_event);

}
