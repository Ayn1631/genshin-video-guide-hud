#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <opencv2/core.hpp>

namespace guide {

struct GameWindow {
    HWND hwnd;
    RECT bounds;
};

std::optional<GameWindow> foreground_game(const std::wstring& title, const std::vector<HWND>& excluded);
std::optional<RECT> game_minimap_rect(const RECT& game_client);

class GameSource {
public:
    explicit GameSource(const std::filesystem::path& assets);
    ~GameSource();
    cv::Mat read(const GameWindow& game);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::optional<double> camera_heading(const cv::Mat& minimap);

}
