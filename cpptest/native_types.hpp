#pragma once

#include <windows.h>

#include "map_locator.hpp"

#include <algorithm>

namespace guide {

enum class CaptureMode { remote, wake, windows };

struct NormalizedRect {
    double x = 0;
    double y = 0;
    double width = 1;
    double height = 1;
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
    std::wstring edge_pattern = L"bilibili\\.com";
    CaptureMode capture_mode = CaptureMode::remote;
    NormalizedRect video_rect;
    bgi::Circle circle{94.5f, 90.5f, 95.1f, false};
    double inset = 11.4;
    int video_fps = 10;
    int player_fps = 10;
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
void save_settings(const std::filesystem::path& root, const GuideSettings& settings);
bool valid_assets(const std::filesystem::path& directory);
std::wstring setting_number(double value);
std::optional<double> parse_setting_number(const std::wstring& value);
int run_hud_worker(HINSTANCE instance, const GuideSettings& settings,
                   const std::filesystem::path& root, HANDLE stop_event);

}
