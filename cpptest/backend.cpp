#include "map_locator.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include <opencv2/core/ocl.hpp>

#ifdef _WIN32
#define BGI_API extern "C" __declspec(dllexport)
#else
#define BGI_API extern "C"
#endif

struct BgiPosition {
    double x, y, image_x, image_y, error, similarity;
    int floor, method, inliers, matches, template_size;
    char layer[96];
};

struct BgiResult {
    BgiPosition position;
    int found, phase, failure;
};

namespace {

thread_local std::string error;

bgi::Position import_position(const BgiPosition& p) {
    bgi::Position result;
    result.x = p.x;
    result.y = p.y;
    result.image_x = p.image_x;
    result.image_y = p.image_y;
    result.error = p.error;
    result.similarity = p.similarity;
    result.floor = p.floor;
    result.method = static_cast<bgi::Method>(p.method);
    result.inliers = p.inliers;
    result.matches = p.matches;
    result.template_size = p.template_size;
    result.layer.assign(p.layer, std::find(std::begin(p.layer), std::end(p.layer), '\0'));
    return result;
}

BgiPosition export_position(const bgi::Position& p) {
    BgiPosition result{p.x, p.y, p.image_x, p.image_y, p.error, p.similarity, p.floor,
                       static_cast<int>(p.method), p.inliers, p.matches, p.template_size, {}};
    std::memcpy(result.layer, p.layer.data(), std::min(p.layer.size(), sizeof(result.layer) - 1));
    return result;
}

}

BGI_API void bgi_configure(int threads) noexcept {
    cv::setNumThreads(std::max(1, threads));
    cv::ocl::setUseOpenCL(false);
}

BGI_API const char* bgi_last_error() noexcept {
    return error.c_str();
}

BGI_API void* bgi_open_cached(const char* assets, int method, int workers, int floor, bool use_floor,
                             const char* layers, int min_inliers, double inlier_ratio, double max_error,
                             const double* circle, double inset, const char* cache) noexcept {
    try {
        if (!assets || method < 0 || method > 2 || workers < 1) throw std::invalid_argument("invalid engine settings");
        bgi::Config config;
        config.method = static_cast<bgi::Method>(method);
        config.workers = static_cast<unsigned>(workers);
        if (use_floor) config.floor = floor;
        config.min_inliers = min_inliers;
        config.min_inlier_ratio = inlier_ratio;
        config.max_error = max_error;
        config.inset = inset;
        if (cache && *cache) config.index_cache = std::filesystem::u8path(cache);
        if (circle) {
            config.circle = bgi::Circle{static_cast<float>(circle[0]), static_cast<float>(circle[1]),
                                        static_cast<float>(circle[2])};
        }
        const std::string ids = layers ? layers : "";
        for (std::size_t begin = 0, end; begin < ids.size(); begin = end + 1) {
            end = ids.find(',', begin);
            if (end == std::string::npos) end = ids.size();
            if (end > begin) config.layers.push_back(ids.substr(begin, end - begin));
        }
        return new bgi::LocateEngine(std::filesystem::u8path(assets), std::move(config));
    } catch (const std::exception& failure) {
        error = failure.what();
        return nullptr;
    }
}

BGI_API void* bgi_open(const char* assets, int method, int workers, int floor, bool use_floor,
                     const char* layers, int min_inliers, double inlier_ratio, double max_error,
                     const double* circle, double inset) noexcept {
    return bgi_open_cached(assets, method, workers, floor, use_floor, layers, min_inliers, inlier_ratio, max_error,
                           circle, inset, nullptr);
}

BGI_API void bgi_close(void* handle) noexcept {
    delete static_cast<bgi::LocateEngine*>(handle);
}

BGI_API int bgi_set_video_retry_limits(void* handle, int local, int regional) noexcept {
    try {
        if (!handle) throw std::invalid_argument("invalid locator handle");
        static_cast<bgi::LocateEngine*>(handle)->set_video_retry_limits(local, regional);
        return 0;
    } catch (const std::exception& failure) {
        error = failure.what();
        return -1;
    }
}

BGI_API int bgi_video_retry_progress(void* handle, int phase) noexcept {
    return handle ? static_cast<bgi::LocateEngine*>(handle)->video_retry_progress(static_cast<bgi::Phase>(phase)) : 0;
}

BGI_API int bgi_locate_with_scene(void* handle, const unsigned char* data, int width, int height,
                                std::size_t stride, int input, int search, const BgiPosition* near,
                                void (*notify)(int), BgiResult* result, double* map_radius,
                                const char* prior_scene, char* scene, std::size_t scene_capacity) noexcept {
    try {
        if (!handle || !data || !result || width <= 0 || height <= 0 || stride < static_cast<std::size_t>(width) * 3 ||
            input < 0 || input > 3 || search < 0 || search > static_cast<int>(bgi::Search::tile)) {
            throw std::invalid_argument("invalid image buffer or matching mode");
        }
        const cv::Mat frame(height, width, CV_8UC3, const_cast<unsigned char*>(data), stride);
        auto anchor = near ? std::optional{import_position(*near)} : std::nullopt;
        if (anchor && prior_scene) anchor->scene = prior_scene;
        std::function<void(bgi::Phase)> callback;
        if (notify) callback = [&](auto phase) { notify(static_cast<int>(phase)); };
        const auto match = static_cast<bgi::LocateEngine*>(handle)->locate(
            frame, static_cast<bgi::Input>(input), static_cast<bgi::Search>(search), callback, anchor ? &*anchor : nullptr);
        *result = {};
        result->found = match.position.has_value() ? 1 : 0;
        result->phase = static_cast<int>(match.phase);
        result->failure = static_cast<int>(match.failure);
        if (match.position) result->position = export_position(*match.position);
        if (map_radius) *map_radius = match.position ? match.position->map_radius : 0;
        if (scene && scene_capacity) {
            const auto name = match.position ? std::string_view(match.position->scene) : std::string_view{};
            const auto count = std::min(name.size(), scene_capacity - 1);
            if (count) std::memcpy(scene, name.data(), count);
            scene[count] = '\0';
        }
        return 0;
    } catch (const std::exception& failure) {
        error = failure.what();
        return -1;
    }
}

BGI_API int bgi_locate_with_radius(void* handle, const unsigned char* data, int width, int height,
                                 std::size_t stride, int input, int search, const BgiPosition* near,
                                 void (*notify)(int), BgiResult* result, double* map_radius) noexcept {
    return bgi_locate_with_scene(handle, data, width, height, stride, input, search, near, notify,
                                 result, map_radius, nullptr, nullptr, 0);
}

BGI_API int bgi_locate(void* handle, const unsigned char* data, int width, int height, std::size_t stride,
                     int input, int search, const BgiPosition* near, void (*notify)(int),
                     BgiResult* result) noexcept {
    return bgi_locate_with_radius(handle, data, width, height, stride, input, search, near, notify, result, nullptr);
}
