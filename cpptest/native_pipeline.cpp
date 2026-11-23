#include "native_types.hpp"
#include "native_game.hpp"
#include "native_hud.hpp"
#include "native_startup_log.hpp"
#include "native_video.hpp"

#include <opencv2/core/ocl.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <mutex>
#include <thread>

namespace guide {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto player_age = 900ms;
constexpr auto guide_age = 4s;
constexpr double degrees = 57.29577951308232;

bool stopped(HANDLE stop) {
    return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0;
}

template<class Operation>
void paced(HANDLE stop, double fps, Operation operation) {
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1 / fps));
    while (!stopped(stop)) {
        const auto due = Clock::now() + period;
        operation();
        const auto rest = std::chrono::ceil<std::chrono::milliseconds>(due - Clock::now());
        if (rest.count() > 0) {
            WaitForSingleObject(stop, static_cast<DWORD>(rest.count()));
        }
    }
}

struct Frame {
    cv::Mat image;
    Clock::time_point time;
    double capture_ms = 0;
    std::wstring status;
};

class LatestFrame {
public:
    void publish(Frame frame) {
        {
            const std::lock_guard lock(mutex_);
            latest_ = std::move(frame);
        }
        changed_.notify_one();
    }

    std::optional<Frame> take(HANDLE stop) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return latest_.has_value() || stopped(stop); });
        if (stopped(stop)) {
            return std::nullopt;
        }
        auto frame = std::move(latest_);
        latest_.reset();
        return frame;
    }

    void wake() {
        const std::lock_guard lock(mutex_);
        changed_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Frame> latest_;
};

struct Rate {
    std::deque<Clock::time_point> samples;

    void add(Clock::time_point now) {
        while (!samples.empty() && samples.front() < now - 3s) {
            samples.pop_front();
        }
        samples.push_back(now);
    }

    double at(Clock::time_point now) const {
        const auto first = std::lower_bound(samples.begin(), samples.end(), now - 3s);
        if (first == samples.end() || std::distance(first, samples.end()) < 2) {
            return 0;
        }
        return static_cast<double>(std::distance(first, samples.end()) - 1)
            / std::chrono::duration<double>(std::min(now, *first + 3s) - *first).count();
    }
};

struct Tracked {
    bgi::Position position;
    std::optional<double> heading;
    Clock::time_point time;
};

struct Stream {
    std::optional<Tracked> point;
    std::wstring capture_status;
    std::wstring match_status;
    Clock::time_point frame_at{};
    Rate captures, matches, locations;
    double capture_ms = 0, match_ms = 0, heading_ms = 0;

    void capture(const Frame& frame, bool detailed) {
        capture_status = frame.image.empty() ? frame.status : L"";
        if (!frame.image.empty()) {
            frame_at = frame.time;
            if (detailed) {
                captures.add(frame.time);
            }
        }
        capture_ms = frame.capture_ms;
    }
};

bool same_block(const bgi::Position& guide, const bgi::Position& player) {
    if (guide.scene != player.scene || guide.floor != player.floor || guide.method != player.method) {
        return false;
    }
    if (guide.method == bgi::Method::map_template) {
        return guide.layer == player.layer;
    }
    return std::max(std::abs(guide.x - player.x), std::abs(guide.y - player.y)) <= 350;
}

std::wstring stage(bgi::Phase phase) {
    using bgi::Phase;
    switch (phase) {
    case Phase::local: return L"当前图块";
    case Phase::global: return L"全图搜索";
    case Phase::retry: return L"全图重试";
    case Phase::region: return L"区域搜索";
    case Phase::cached: return L"沿用同帧结果";
    case Phase::wait_region: return L"当前图块未匹配";
    case Phase::wait_global: return L"区域未匹配";
    case Phase::wait_initial: return L"等待再次全图搜索";
    default: return {};
    }
}

std::wstring failure(bgi::Failure reason) {
    using bgi::Failure;
    switch (reason) {
    case Failure::black: return L"画面为黑屏";
    case Failure::no_circle: return L"找不到小地图范围";
    case Failure::no_minimap: return L"画面未检测到小地图";
    case Failure::no_features: return L"有效特征不足";
    case Failure::no_tiles: return L"地图图块未匹配";
    case Failure::coarse: return L"粗匹配未通过";
    case Failure::fine: return L"粗匹配通过，精匹配未通过";
    case Failure::ambiguous: return L"有多个相似地点";
    default: return {};
    }
}

std::wstring result_status(const bgi::Result& result, const bgi::LocateEngine& engine,
                           const GuideSettings& settings) {
    const auto label = stage(result.phase);
    switch (result.phase) {
    case bgi::Phase::wait_initial:
    case bgi::Phase::wait_region:
    case bgi::Phase::wait_global: {
        const int limit = result.phase == bgi::Phase::wait_global ? settings.region_misses : settings.local_misses;
        return label + L"  " + std::to_wstring(engine.video_retry_progress(result.phase))
             + L"/" + std::to_wstring(limit);
    }
    default:
        if (result.position) {
            return L"已定位  ·  " + label;
        }
        const auto cause = failure(result.failure);
        return label.empty() ? cause : label + L"未匹配  ·  " + cause;
    }
}

std::optional<double> smooth_angle(std::optional<double> measured, std::optional<double> old,
                                  double elapsed) {
    if (old && elapsed < 0.9) {
        if (!measured) {
            return old;
        }
        return *old - std::expm1(-elapsed / 0.07) * std::remainder(*measured - *old, 360.0);
    }
    return measured;
}

class PlayerTracker {
public:
    std::optional<Tracked> last;
    std::optional<bgi::Position> previous;

    void update(bgi::Position next, std::optional<double> heading, Clock::time_point now) {
        const auto raw = next;
        if (last && (last->position.scene != next.scene || last->position.floor != next.floor
            || std::hypot(last->position.x - next.x, last->position.y - next.y) > 75.0)) {
            if (!pending_ || pending_->scene != next.scene || pending_->floor != next.floor
                || std::hypot(pending_->x - next.x, pending_->y - next.y) > 8) {
                pending_ = std::move(next);
                previous.reset();
                return;
            }
        } else if (last && now - last->time < player_age) {
            const auto& old = last->position;
            const double weight = -std::expm1(-std::chrono::duration<double>(now - last->time).count() / 0.25);
            next.x = old.x + weight * (next.x - old.x);
            next.y = old.y + weight * (next.y - old.y);
            if (old.map_radius > 0 && next.map_radius > 0) {
                next.map_radius = old.map_radius + weight * (next.map_radius - old.map_radius);
            }
        }
        pending_.reset();
        previous = raw;
        last = Tracked{std::move(next), heading, now};
    }

    void miss() {
        pending_.reset();
        previous.reset();
    }

private:
    std::optional<bgi::Position> pending_;
};

std::wstring decimal(double value) {
    auto text = std::to_wstring(value);
    const auto point = text.find(L'.');
    if (point != std::wstring::npos) {
        text.resize(point + 2);
    }
    return text;
}

DiagnosticSection diagnostics(const std::wstring& name, const Stream& stream, Clock::time_point now) {
    DiagnosticSection section{name + L"流程",
        L"取帧  " + decimal(stream.captures.at(now)) + L"\u00A0FPS  ·  "
        + decimal(stream.capture_ms) + L"\u00A0ms\n"
        + L"匹配  " + decimal(stream.matches.at(now)) + L"\u00A0FPS  ·  "
        + decimal(stream.match_ms) + L"\u00A0ms\n"
        + L"定位  " + decimal(stream.locations.at(now)) + L"\u00A0FPS  ·  视角  "
        + decimal(stream.heading_ms) + L"\u00A0ms"};
    if (!stream.capture_status.empty()) {
        section.text += L"\n" + stream.capture_status;
    }
    if (!stream.match_status.empty()) {
        section.text += L"\n" + stream.match_status;
    }
    return section;
}

HudView make_view(const GuideSettings& settings, const RECT& bounds, const Stream& video,
                  const Stream& player) {
    const auto now = Clock::now();
    HudView view;
    if (settings.detailed) {
        view.diagnostics = {diagnostics(L"攻略", video, now), diagnostics(L"玩家", player, now)};
    }
    if (now - player.frame_at > player_age || !player.capture_status.empty()) {
        view.title = L"游戏画面未检测到小地图";
        view.detail = player.capture_status == view.title ? video.match_status : player.capture_status;
        return view;
    }
    if (!video.point || now - video.point->time > guide_age || now - video.frame_at > player_age) {
        view.title = L"等待攻略目标";
        view.detail = !video.capture_status.empty() ? video.capture_status : video.match_status;
        return view;
    }
    if (!player.point || now - player.point->time > player_age
        || !same_block(video.point->position, player.point->position)) {
        view.title = L"正在定位玩家";
        view.detail = player.match_status;
        return view;
    }

    const auto& target = video.point->position;
    const auto& here = player.point->position;
    const double dx = here.x - target.x;
    const double dy = target.y - here.y;
    const double distance = std::hypot(dx, dy);
    const double bearing = std::atan2(dx, dy) * degrees;
    view.distance = distance;
    if (here.map_radius > 0 && distance <= here.map_radius) {
        if (const auto map = game_minimap_rect(bounds)) {
            const double size = static_cast<double>(std::min(map->right - map->left, map->bottom - map->top) - 1);
            const double scale = size / (2 * here.map_radius);
            view.marker = POINT{std::lround((map->left + map->right - 1) / 2.0 + dx * scale),
                                std::lround((map->top + map->bottom - 1) / 2.0 - dy * scale)};
            view.marker_heading = video.point->heading;
        }
    }
    if (distance <= settings.arrival_distance) {
        view.title = L"目标附近";
    } else if (player.point->heading) {
        view.offset = std::remainder(bearing - *player.point->heading, 360.0);
        const long angle = std::lround(*view.offset);
        view.title = (angle > 0 ? L"+" : L"") + std::to_wstring(angle) + L"°";
        if (std::abs(*view.offset) > 90) {
            view.detail = *view.offset > 0 ? L"右后方" : L"左后方";
        }
    } else {
        view.title = std::to_wstring((std::lround(bearing) + 360) % 360) + L"°";
        view.detail = L"地图方位  ·  正在识别视角";
    }
    return view;
}

class Pipeline {
public:
    Pipeline(const GuideSettings& settings, std::filesystem::path root, HANDLE stop, std::vector<HWND> windows)
        : settings_(settings), root_(std::move(root)), stop_(stop), windows_(std::move(windows)) {}

    ~Pipeline() {
        SetEvent(stop_);
        video_frames_.wake();
        game_frames_.wake();
        for (auto& thread : threads_) {
            thread.join();
        }
    }

    void start() {
        threads_.reserve(4);
        launch(video_.capture_status, &Pipeline::capture_video);
        launch(video_.match_status, &Pipeline::match_video);
        launch(player_.capture_status, &Pipeline::capture_game);
        launch(player_.match_status, &Pipeline::match_player);
    }

    HudView view(const RECT& bounds) {
        const std::lock_guard lock(mutex_);
        return make_view(settings_, bounds, video_, player_);
    }

private:
    void launch(std::wstring& status, void (Pipeline::*run)()) {
        threads_.emplace_back([this, &status, run] {
            try {
                (this->*run)();
            } catch (...) {
                const std::lock_guard lock(mutex_);
                status = L"流程中止，请检查地图资源后重启 HUD";
            }
        });
    }

    bgi::Config config() const {
        bgi::Config result;
        result.method = settings_.method;
        result.workers = static_cast<unsigned>(settings_.workers);
        if (settings_.cache_indexes) {
            result.index_cache = root_ / L"index-cache";
        }
        return result;
    }

    void publish(LatestFrame& slot, Stream& stream, cv::Mat image, std::wstring status, Clock::time_point start) {
        const auto now = Clock::now();
        Frame frame{std::move(image), now, std::chrono::duration<double, std::milli>(now - start).count(),
                    std::move(status)};
        {
            const std::lock_guard lock(mutex_);
            stream.capture(frame, settings_.detailed);
        }
        slot.publish(std::move(frame));
    }

    void capture_video() {
        VideoSource source(settings_, stop_);
        paced(stop_, settings_.video_fps, [&] {
            const auto start = Clock::now();
            auto frame = source.read();
            publish(video_frames_, video_, std::move(frame.image), std::move(frame.status), start);
        });
    }

    void capture_game() {
        GameSource source(settings_.assets);
        paced(stop_, settings_.player_fps, [&] {
            const auto start = Clock::now();
            const auto game = foreground_game(settings_.game_title, windows_, settings_.hide_when_covered).game;
            auto image = game ? source.read(*game) : cv::Mat{};
            auto status = image.empty() ? L"游戏画面未检测到小地图" : L"";
            publish(game_frames_, player_, std::move(image), status, start);
        });
    }

    void match_video() {
        bgi::LocateEngine engine(settings_.assets, config());
        engine.set_video_retry_limits(settings_.local_misses, settings_.region_misses);
        while (auto frame = video_frames_.take(stop_)) {
            if (frame->image.empty()) {
                continue;
            }
            const auto start = Clock::now();
            const auto result = engine.locate(frame->image, bgi::Input::video_minimap, bgi::Search::track,
                [this](bgi::Phase phase) {
                    const std::lock_guard lock(mutex_);
                    video_.match_status = L"正在" + stage(phase);
                });
            const auto matched = Clock::now();
            const auto heading = result.position ? camera_heading(frame->image) : std::nullopt;
            const auto done = Clock::now();
            const std::lock_guard lock(mutex_);
            video_.match_status = result_status(result, engine, settings_);
            record(video_, start, matched, done, result.position.has_value());
            if (result.position) {
                video_.point = Tracked{*result.position, heading, frame->time};
            } else if (result.failure == bgi::Failure::black || result.failure == bgi::Failure::no_minimap) {
                video_.point.reset();
            }
        }
    }

    void match_player() {
        auto options = config();
        options.min_inliers = 9;
        options.min_inlier_ratio = 0.27;
        options.max_error = 6;
        std::unique_ptr<bgi::LocateEngine> engine;
        PlayerTracker tracker;
        Clock::time_point heading_at{};
        std::optional<double> heading;
        while (auto frame = game_frames_.take(stop_)) {
            if (frame->image.empty()) {
                tracker.miss();
                heading.reset();
                continue;
            }
            std::optional<Tracked> target;
            {
                const std::lock_guard lock(mutex_);
                target = video_.point;
            }
            if (!target || Clock::now() - target->time > guide_age) {
                tracker.miss();
                continue;
            }
            const auto start = Clock::now();
            if (!engine) {
                engine = std::make_unique<bgi::LocateEngine>(settings_.assets, options);
            }
            const auto& anchor = target->position;
            if (tracker.last && !same_block(anchor, tracker.last->position)) {
                tracker = PlayerTracker{};
            }
            const bool tracked = tracker.previous && frame->time - tracker.last->time < player_age;
            const bgi::Position& prior = tracked ? *tracker.previous : anchor;
            const auto search = !tracked && anchor.method == bgi::Method::map_template
                ? bgi::Search::tile : bgi::Search::local;
            auto result = engine->locate(frame->image, bgi::Input::minimap, search, {}, &prior);
            const auto matched = Clock::now();
            const auto measured = camera_heading(frame->image);
            const auto done = Clock::now();
            heading = smooth_angle(measured, heading, std::chrono::duration<double>(frame->time - heading_at).count());
            if (measured) {
                heading_at = frame->time;
            }
            const std::lock_guard lock(mutex_);
            const bool located = result.position && video_.point && same_block(video_.point->position, *result.position);
            if (located) {
                tracker.update(std::move(*result.position), heading, frame->time);
            } else {
                tracker.miss();
            }
            if (tracker.last) {
                player_.point = tracker.last;
                player_.point->heading = heading;
            } else {
                player_.point.reset();
            }
            player_.match_status = located ? L"已在攻略图块内定位" : L"攻略图块未匹配，等待下一帧";
            record(player_, start, matched, done, located);
        }
    }

    void record(Stream& stream, Clock::time_point start, Clock::time_point matched,
                Clock::time_point done, bool located) const {
        stream.match_ms = std::chrono::duration<double, std::milli>(matched - start).count();
        stream.heading_ms = std::chrono::duration<double, std::milli>(done - matched).count();
        if (settings_.detailed) {
            stream.matches.add(done);
            if (located) {
                stream.locations.add(done);
            }
        }
    }

    GuideSettings settings_;
    std::filesystem::path root_;
    HANDLE stop_;
    std::vector<HWND> windows_;
    LatestFrame video_frames_, game_frames_;
    Stream video_, player_;
    std::mutex mutex_;
    std::vector<std::thread> threads_;
};

std::wstring window_details(HWND window, bool include_game_title) {
    if (!window) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    std::wstring details = L" PID=" + std::to_wstring(pid);
    wchar_t styles[11]{};
    std::swprintf(styles, ARRAYSIZE(styles), L"0x%08lX", static_cast<unsigned long>(GetWindowLongPtrW(window, GWL_EXSTYLE)));
    details += L"，扩展样式=";
    details += styles;
    wchar_t window_class[256]{};
    if (GetClassNameW(window, window_class, ARRAYSIZE(window_class))) {
        details += L"，窗口类=";
        details += window_class;
    }
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return details;

    wchar_t path[MAX_PATH]{};
    DWORD length = ARRAYSIZE(path);
    const bool has_path = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!has_path) return details;

    const auto separator = std::wcsrchr(path, L'\\');
    const auto executable = separator ? separator + 1 : path;
    details += L"，进程=";
    details += executable;
    if (include_game_title && (_wcsicmp(executable, L"YuanShen.exe") == 0 ||
                               _wcsicmp(executable, L"GenshinImpact.exe") == 0)) {
        wchar_t title[256]{};
        if (GetWindowTextW(window, title, ARRAYSIZE(title))) {
            details += L"，实际游戏标题=";
            details += title;
        }
    }
    return details;
}

std::wstring hud_visibility_message(const ForegroundGame& foreground, const std::wstring& expected_title,
                                   HWND overlay) {
    switch (foreground.state) {
    case ForegroundState::no_window:
        return L"HUD 已隐藏。当前没有可用的前台窗口。";
    case ForegroundState::title_mismatch:
        return L"HUD 已隐藏。前台窗口标题与设置不一致，预期标题=" + expected_title +
               L"。" + window_details(foreground.related_window, true);
    case ForegroundState::unavailable:
        return L"HUD 已隐藏。游戏窗口已切离前台、最小化或客户区不可用。";
    case ForegroundState::covered:
        return L"HUD 已隐藏。检测到上层窗口与游戏区域重叠。" +
               window_details(foreground.related_window, false);
    case ForegroundState::ready:
        return IsWindowVisible(overlay) ? L"方向 HUD 已提交绘制，Windows 窗口已设为可见。"
                                        : L"游戏窗口通过检查，方向 HUD 窗口仍未显示。";
    }
    return {};
}

}

int run_hud_worker(HINSTANCE instance, const GuideSettings& settings,
                   const std::filesystem::path& root, HANDLE stop_event) {
    cv::setNumThreads(settings.opencv_threads);
    cv::ocl::setUseOpenCL(false);
    Hud hud(instance);
    const auto windows = hud.windows();
    write_hud_startup_log(root, L"HUD 窗口已创建，开始检查前台游戏窗口。");
    Pipeline pipeline(settings, root, stop_event, windows);
    pipeline.start();
    const auto interval = static_cast<DWORD>(std::ceil(1000 / hud_fps(settings.video_fps, settings.player_fps)));
    std::optional<ForegroundState> last_state;
    HWND last_window = nullptr;
    bool logged_z_order_restore = false;
    while (!stopped(stop_event)) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                SetEvent(stop_event);
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const auto foreground = foreground_game(settings.game_title, windows, settings.hide_when_covered);
        if (const auto& game = foreground.game) {
            hud.show(game->bounds, pipeline.view(game->bounds), foreground.hud_below_game);
            if (foreground.hud_below_game && !logged_z_order_restore) {
                write_hud_startup_log(root, L"检测到浮层排在游戏窗口下方，已重新设置桌面置顶。");
                logged_z_order_restore = true;
            }
        } else {
            hud.hide();
        }
        if (last_state != foreground.state || last_window != foreground.related_window) {
            write_hud_startup_log(root, hud_visibility_message(foreground, settings.game_title, windows.front()));
            last_state = foreground.state;
            last_window = foreground.related_window;
        }
        MsgWaitForMultipleObjects(1, &stop_event, FALSE, interval, QS_ALLINPUT);
    }
    hud.hide();
    return 0;
}

}
