#include "native_game.hpp"
#include "map_locator.hpp"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <unordered_set>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace guide {
namespace {

constexpr int minimap_size = 212;
constexpr int angles = 360;
constexpr int first_radius = 19;
constexpr int last_radius = 78;
constexpr int radius_count = last_radius - first_radius + 1;

class PhysicalPixels {
public:
    PhysicalPixels() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalPixels() {
        if (previous_) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }
    PhysicalPixels(const PhysicalPixels&) = delete;
    PhysicalPixels& operator=(const PhysicalPixels&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous_;
};

bool is_cloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

std::optional<RECT> foreground_bounds(HWND hwnd) {
    if (!hwnd || hwnd != GetForegroundWindow() || !IsWindowVisible(hwnd) || IsIconic(hwnd) || is_cloaked(hwnd)) {
        return std::nullopt;
    }
    RECT client{};
    POINT origin{};
    if (!GetClientRect(hwnd, &client) || IsRectEmpty(&client) || !ClientToScreen(hwnd, &origin)) {
        return std::nullopt;
    }
    OffsetRect(&client, origin.x, origin.y);
    return client;
}

bool is_covered(HWND hwnd, const RECT& bounds, const std::vector<HWND>& excluded) {
    std::unordered_set<HWND> visited{hwnd};
    for (HWND above = GetWindow(hwnd, GW_HWNDPREV); above && visited.insert(above).second;
         above = GetWindow(above, GW_HWNDPREV)) {
        if (!IsWindowVisible(above) || IsIconic(above) ||
            std::find(excluded.begin(), excluded.end(), above) != excluded.end()) {
            continue;
        }

        RECT frame{}, overlap{};
        if (FAILED(DwmGetWindowAttribute(above, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
            !GetWindowRect(above, &frame)) {
            continue;
        }
        if (IntersectRect(&overlap, &bounds, &frame) && !is_cloaked(above)) {
            return true;
        }
    }
    return false;
}

class MonitorCoverage {
public:
    explicit MonitorCoverage(const RECT& bounds)
        : remaining_(CreateRectRgnIndirect(&bounds)), monitor_(CreateRectRgn(0, 0, 0, 0)) {}
    ~MonitorCoverage() {
        if (monitor_) {
            DeleteObject(monitor_);
        }
        if (remaining_) {
            DeleteObject(remaining_);
        }
    }
    MonitorCoverage(const MonitorCoverage&) = delete;
    MonitorCoverage& operator=(const MonitorCoverage&) = delete;

    bool visible() {
        RECT bounds{};
        return remaining_ && monitor_ &&
            EnumDisplayMonitors(nullptr, nullptr, subtract_monitor, reinterpret_cast<LPARAM>(this)) &&
            GetRgnBox(remaining_, &bounds) == NULLREGION;
    }

private:
    static BOOL CALLBACK subtract_monitor(HMONITOR, HDC, LPRECT area, LPARAM data) {
        const auto& regions = *reinterpret_cast<MonitorCoverage*>(data);
        return SetRectRgn(regions.monitor_, area->left, area->top, area->right, area->bottom) &&
            CombineRgn(regions.remaining_, regions.remaining_, regions.monitor_, RGN_DIFF) != ERROR;
    }

    HRGN remaining_;
    HRGN monitor_;
};

class ScreenCapture {
public:
    explicit ScreenCapture(const RECT& bounds)
        : width_(bounds.right - bounds.left), height_(bounds.bottom - bounds.top), screen_(GetDC(nullptr)) {
        if (!screen_) {
            return;
        }
        memory_ = CreateCompatibleDC(screen_);
        if (!memory_) {
            return;
        }

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(memory_, &info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
        if (bitmap_) {
            previous_ = SelectObject(memory_, bitmap_);
        }
    }

    ~ScreenCapture() {
        if (previous_ && previous_ != HGDI_ERROR) {
            SelectObject(memory_, previous_);
        }
        if (memory_) {
            DeleteDC(memory_);
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
        }
        if (screen_) {
            ReleaseDC(nullptr, screen_);
        }
    }
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    cv::Mat read(const RECT& bounds) const {
        if (!previous_ || previous_ == HGDI_ERROR || !pixels_ ||
            !BitBlt(memory_, 0, 0, width_, height_, screen_, bounds.left, bounds.top, SRCCOPY) || !GdiFlush()) {
            return {};
        }
        cv::Mat frame;
        cv::cvtColor(cv::Mat(height_, width_, CV_8UC4, pixels_), frame, cv::COLOR_BGRA2BGR);
        return frame;
    }

private:
    int width_;
    int height_;
    HDC screen_;
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* pixels_ = nullptr;
};

cv::Mat find_paimon_icon(const std::filesystem::path& assets) {
    const auto icon = std::filesystem::path("GameTask/Common/Element/Assets/1920x1080/paimon_menu.png");
    auto directory = std::filesystem::absolute(assets);
    for (;;) {
        const auto candidate = directory / icon;
        if (std::filesystem::is_regular_file(candidate)) {
            return bgi::read_image(candidate, cv::IMREAD_COLOR);
        }
        const auto parent = directory.parent_path();
        if (parent.empty() || parent == directory) {
            return {};
        }
        directory = parent;
    }
}

struct ColorTables {
    cv::Mat x;
    cv::Mat y;
    std::array<float, radius_count> base_alpha{};
    std::array<float, radius_count> fan_alpha{};

    ColorTables() : x(angles, radius_count, CV_32F), y(angles, radius_count, CV_32F) {
        constexpr std::array<double, 13> thresholds{
            18.632, 20.157, 24.093, 34.617, 38.566, 41.94, 47.654,
            51.087, 58.561, 63.925, 67.759, 71.77, 75.214
        };
        for (int column = 0; column < radius_count; ++column) {
            const int radius = first_radius + column;
            const auto below = std::lower_bound(thresholds.begin(), thresholds.end(), radius) - thresholds.begin();
            base_alpha[column] = static_cast<float>(255.0 / (229 + below));
            fan_alpha[column] = 255.f / (137.f + 1.43f * radius);
        }
        for (int angle = 0; angle < angles; ++angle) {
            const float radians = angle * static_cast<float>(CV_PI / 180);
            for (int column = 0; column < radius_count; ++column) {
                const int radius = first_radius + column;
                x.at<float>(angle, column) = last_radius + std::cos(radians) * radius;
                y.at<float>(angle, column) = last_radius + std::sin(radians) * radius;
            }
        }
    }
};

cv::Mat map_mask(const cv::Mat& center) {
    cv::Mat mask(center.size(), CV_8U, cv::Scalar(255));
    cv::Mat icons(center.size(), CV_8U);
    bool has_color = false;
    for (int y = 0; y < center.rows; ++y) {
        const auto* pixels = center.ptr<cv::Vec3b>(y);
        auto* row = icons.ptr<uchar>(y);
        for (int x = 0; x < center.cols; ++x) {
            const auto& pixel = pixels[x];
            const int brightest = std::max({pixel[0], pixel[1], pixel[2]});
            const int spread = brightest - std::min({pixel[0], pixel[1], pixel[2]});
            has_color |= spread != 0;
            const bool gray_icon = spread == 0 && brightest >= 50 && brightest <= 127;
            const bool white_icon = 3 * brightest > 600 + 10 * std::min(6 * spread, 255 - brightest);
            row[x] = gray_icon || white_icon ? 255 : 0;
        }
    }
    if (has_color) {
        const auto kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {5, 5});
        cv::dilate(icons, icons, kernel);
        cv::morphologyEx(icons, icons, cv::MORPH_CLOSE, kernel);
        mask.setTo(0, icons);
    }
    return mask;
}

int wrap(int angle) {
    return (angle + angles) % angles;
}

using AngleScores = std::array<double, angles>;

double rival_score(const AngleScores& scores, int best, int separation) {
    double rival = 0;
    for (int offset = separation; offset <= angles - separation; ++offset) {
        rival = std::max(rival, scores[wrap(best + offset)]);
    }
    return rival;
}

std::optional<double> color_heading(const cv::Mat& normalized) {
    static const ColorTables tables;
    const auto center = normalized(cv::Rect(28, 28, 156, 156));
    cv::Mat polar, valid, hls, gray;
    cv::remap(center, polar, tables.x, tables.y, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    cv::remap(map_mask(center), valid, tables.x, tables.y, cv::INTER_NEAREST, cv::BORDER_REPLICATE);
    cv::cvtColor(polar, hls, cv::COLOR_BGR2HLS_FULL);
    cv::cvtColor(polar, gray, cv::COLOR_BGR2GRAY);

    // Color-sector method from BetterGI: compare color frequencies after removing either opacity model.
    std::vector<int> outside_hist(256 * 256), inside_hist(256 * 256);
    const auto compensate = [&](int angle, int column) {
        const int hue = hls.at<cv::Vec3b>(angle, column)[0];
        const float brightness = gray.at<uchar>(angle, column);
        float outside = brightness, inside = brightness;
        if (valid.at<uchar>(angle, column)) {
            outside *= tables.base_alpha[column];
            inside = ((brightness - 255.f) * tables.fan_alpha[column] + 255.f) * tables.base_alpha[column];
        }
        return std::array{hue * 256 + static_cast<int>(std::clamp(outside, 0.f, 255.f)),
                          hue * 256 + static_cast<int>(std::clamp(inside, 0.f, 255.f)),
                          outside >= 256.f ? 1 : 0, inside < 0.f ? -1 : static_cast<int>(inside >= 256.f)};
    };
    for (int angle = 0; angle < angles; ++angle) {
        for (int column = 0; column < radius_count; ++column) {
            const auto [outside, inside, outside_limit, inside_limit] = compensate(angle, column);
            outside_hist[outside] += outside_limit == 0;
            inside_hist[inside] += inside_limit == 0;
        }
    }

    AngleScores sectors{}, scores{};
    for (int angle = 0; angle < angles; ++angle) {
        for (int column = 0; column < radius_count; ++column) {
            const auto [outside, inside, outside_limit, inside_limit] = compensate(angle, column);
            if (inside_limit < 0) {
                sectors[angle] -= 1;
            } else if (inside_limit == 0) {
                const int gain = inside_hist[inside] - outside_hist[outside];
                if (outside_limit || gain > 0) {
                    sectors[angle] += 1;
                } else if (gain == 0) {
                    sectors[angle] += 100.0 / 255.0;
                }
            }
        }
        sectors[angle] /= radius_count;
    }
    for (int angle = 0; angle < angles; ++angle) {
        for (int offset = -45; offset <= 45; ++offset) {
            scores[angle] += sectors[wrap(angle + offset)];
        }
        scores[angle] /= 91;
    }
    const int best = static_cast<int>(std::max_element(scores.begin(), scores.end()) - scores.begin());
    const double outside = (std::accumulate(sectors.begin(), sectors.end(), 0.0) - 91 * scores[best]) / 269;
    if (scores[best] >= 0.2 && scores[best] - outside >= 0.20 && scores[best] - rival_score(scores, best, 35) >= 0.020) {
        return wrap(best + 90);
    }
    return std::nullopt;
}

AngleScores edge_support(const cv::Mat& gradient, float sign) {
    AngleScores support{};
    for (int column = 0; column < gradient.cols; ++column) {
        std::array<float, angles> votes{};
        for (int angle = 0; angle < angles; ++angle) {
            const float strength = sign * gradient.at<float>(angle, column);
            if (strength > sign * gradient.at<float>(wrap(angle - 1), column) &&
                strength >= sign * gradient.at<float>(wrap(angle + 1), column)) {
                votes[angle] = std::clamp((strength - 48.f) / 96.f, 0.f, 1.f);
            }
        }
        for (int angle = 0; angle < angles; ++angle) {
            float strongest = 0;
            for (int offset = -2; offset <= 2; ++offset) {
                strongest = std::max(strongest, votes[wrap(angle + offset)]);
            }
            support[angle] += strongest;
        }
    }
    return support;
}

std::optional<double> edge_heading(const cv::Mat& normalized) {
    cv::Mat gray, polar, band, wrapped, gradient;
    cv::cvtColor(normalized, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, {3, 3}, 0);
    cv::warpPolar(gray, polar, {96, angles}, {106.f, 106.f}, 96, cv::INTER_LINEAR | cv::WARP_FILL_OUTLIERS);
    polar.colRange(first_radius, last_radius).convertTo(band, CV_32F);
    cv::copyMakeBorder(band, wrapped, 1, 1, 0, 0, cv::BORDER_WRAP);
    cv::Scharr(wrapped, gradient, CV_32F, 0, 1);
    gradient = gradient.rowRange(1, angles + 1);

    // Radial edge method from BetterGI/GIA: the bright sector has two opposing gradients about 90° apart.
    const auto leading = edge_support(gradient, 1.f);
    const auto trailing = edge_support(gradient, -1.f);
    AngleScores paired{}, scores{};
    for (int angle = 0; angle < angles; ++angle) {
        for (int offset = 87; offset <= 93; ++offset) {
            paired[angle] = std::max(paired[angle], trailing[wrap(angle + offset)]);
        }
        scores[angle] = std::sqrt(leading[angle] * paired[angle]);
    }
    const int best = static_cast<int>(std::max_element(scores.begin(), scores.end()) - scores.begin());
    if (std::min(leading[best], paired[best]) < 12 || scores[best] < std::max(17.0, 1.25 * rival_score(scores, best, 15))) {
        return std::nullopt;
    }
    return wrap(best + 45 + 90);
}

}

std::optional<GameWindow> foreground_game(const std::wstring& title, const std::vector<HWND>& excluded) {
    const PhysicalPixels physical_pixels;
    const HWND hwnd = GetForegroundWindow();
    if (!hwnd || title.empty() || std::find(excluded.begin(), excluded.end(), hwnd) != excluded.end()) {
        return std::nullopt;
    }
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return std::nullopt;
    }
    std::wstring actual_title(static_cast<std::size_t>(length) + 1, L'\0');
    if (!GetWindowTextW(hwnd, actual_title.data(), length + 1) ||
        CompareStringOrdinal(actual_title.c_str(), -1, title.c_str(), -1, TRUE) != CSTR_EQUAL) {
        return std::nullopt;
    }
    const auto bounds = foreground_bounds(hwnd);
    if (!bounds || is_covered(hwnd, *bounds, excluded)) {
        return std::nullopt;
    }
    return GameWindow{hwnd, *bounds};
}

std::optional<RECT> game_minimap_rect(const RECT& game_client) {
    const auto width = game_client.right - game_client.left;
    const auto height = game_client.bottom - game_client.top;
    if (width <= 0 || height <= 0 || std::abs(static_cast<double>(width) / height - 16.0 / 9.0) > 0.02) {
        return std::nullopt;
    }
    const double scale = width / 1920.0;
    const auto size = cvRound(minimap_size * scale);
    const auto left = game_client.left + cvRound(62 * scale);
    const auto top = game_client.top + cvRound(19 * scale);
    if (size <= 0 || left + size > game_client.right || top + size > game_client.bottom) {
        return std::nullopt;
    }
    return RECT{left, top, left + size, top + size};
}

struct GameSource::Impl {
    explicit Impl(const std::filesystem::path& assets) : paimon(find_paimon_icon(assets)) {}

    cv::Mat paimon;
    cv::Mat scaled_paimon;
    int client_width = 0;

    bool has_paimon(const cv::Mat& frame, int width) {
        const double scale = width / 1920.0;
        if (client_width != width) {
            cv::resize(paimon, scaled_paimon,
                       {std::max(1, cvRound(paimon.cols * scale)), std::max(1, cvRound(paimon.rows * scale))},
                       0, 0, cv::INTER_LINEAR);
            client_width = width;
        }
        const int size = cvRound(112 * scale);
        if (size < scaled_paimon.cols || size < scaled_paimon.rows || size > frame.cols || size > frame.rows) {
            return false;
        }
        cv::Mat scores;
        cv::matchTemplate(frame(cv::Rect(0, 0, size, size)), scaled_paimon, scores, cv::TM_CCOEFF_NORMED);
        double score = 0;
        cv::minMaxLoc(scores, nullptr, &score);
        return score >= 0.65;
    }
};

GameSource::GameSource(const std::filesystem::path& assets) : impl_(std::make_unique<Impl>(assets)) {}
GameSource::~GameSource() = default;

cv::Mat GameSource::read(const GameWindow& game) {
    const PhysicalPixels physical_pixels;
    const auto client = foreground_bounds(game.hwnd);
    if (!client || !EqualRect(&*client, &game.bounds)) {
        return {};
    }
    const auto minimap = game_minimap_rect(game.bounds);
    if (!minimap) {
        return {};
    }
    RECT bounds = *minimap;
    if (!impl_->paimon.empty()) {
        bounds.left = game.bounds.left;
        bounds.top = game.bounds.top;
    }
    if (!MonitorCoverage(bounds).visible()) {
        return {};
    }
    const auto frame = ScreenCapture(bounds).read(bounds);
    const auto after = foreground_bounds(game.hwnd);
    if (frame.empty() || !after || !EqualRect(&*after, &game.bounds)) {
        return {};
    }
    if (!impl_->paimon.empty() && !impl_->has_paimon(frame, game.bounds.right - game.bounds.left)) {
        return {};
    }
    auto crop = frame(cv::Rect(minimap->left - bounds.left, minimap->top - bounds.top,
                               minimap->right - minimap->left, minimap->bottom - minimap->top));
    if (crop.cols < 100) {
        cv::resize(crop, crop, {minimap_size, minimap_size}, 0, 0, cv::INTER_LINEAR);
    }
    return crop;
}

std::optional<double> camera_heading(const cv::Mat& minimap) {
    if (minimap.depth() != CV_8U || std::min(minimap.cols, minimap.rows) < 64) {
        return std::nullopt;
    }
    cv::Mat bgr = minimap;
    if (minimap.channels() == 4) {
        cv::cvtColor(minimap, bgr, cv::COLOR_BGRA2BGR);
    } else if (minimap.channels() == 1) {
        cv::cvtColor(minimap, bgr, cv::COLOR_GRAY2BGR);
    } else if (minimap.channels() != 3) {
        return std::nullopt;
    }
    cv::Mat normalized;
    cv::resize(bgr, normalized, {minimap_size, minimap_size}, 0, 0, cv::INTER_LINEAR);
    if (const auto heading = color_heading(normalized)) {
        return heading;
    }
    return edge_heading(normalized);
}

}
