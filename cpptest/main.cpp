#include "map_locator.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include <opencv2/core/ocl.hpp>
#include <opencv2/imgcodecs.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using Path = std::filesystem::path;
using Clock = std::chrono::steady_clock;

Path binary_directory(const Path& argument) {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length > 0 && length < path.size()) {
        path.resize(length);
        return Path(path).parent_path();
    }
#endif
    return std::filesystem::absolute(argument).parent_path();
}

void usage() {
    std::cout << "map_locate --assets <directory> [--input video|minimap|game]\n"
                 "           [--method auto|sift|template] [--layer <id>] [--floor <number>]\n"
                 "           [--workers <N>] [--cv-threads <N>] [--repeat <N>] [--no-cache]\n"
                 "           [--index-cache-dir <directory> | --no-index-cache]\n"
                 "           [--circle X Y R] [--inset <pixels>] <image> [<image> ...]\n";
}

int integer(const std::string& text) {
    std::size_t used = 0;
    const int result = std::stoi(text, &used);
    if (used != text.size()) throw std::invalid_argument("invalid integer: " + text);
    return result;
}

void report(const Path& image, const bgi::Result& result, double milliseconds) {
    std::cout << std::fixed << std::setprecision(4) << "{\"image\":" << std::quoted(image.u8string())
              << ",\"phase\":\"" << bgi::name(result.phase) << "\",\"ms\":" << milliseconds
              << ",\"ok\":" << (result.position ? "true" : "false");
    if (result.position) {
        const auto& p = *result.position;
        std::cout << ",\"x\":" << p.x << ",\"y\":" << p.y << ",\"floor\":" << p.floor
                  << ",\"method\":\"" << bgi::name(p.method) << '\"'
                  << ",\"scene\":" << std::quoted(p.scene);
        if (p.method == bgi::Method::sift) {
            std::cout << ",\"inliers\":" << p.inliers << ",\"matches\":" << p.matches << ",\"rms\":" << p.error;
        } else {
            std::cout << ",\"layer\":" << std::quoted(p.layer) << ",\"similarity\":" << p.similarity;
        }
    } else {
        std::cout << ",\"reason\":\"" << bgi::name(result.failure) << '\"';
    }
    std::cout << "}" << std::endl;
}

int run(const std::vector<Path>& arguments, const Path& cache_directory) {
    Path assets;
    std::vector<Path> images;
    bgi::Config config;
    config.index_cache = cache_directory;
    auto kind = bgi::Input::video;
    int repeats = 1;
    int threads = 2;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto arg = arguments[i].u8string();
        const auto value = [&]() {
            if (i + 1 >= arguments.size()) throw std::invalid_argument("missing value for " + arg);
            return arguments[++i];
        };
        if (arg == "--help") { usage(); return 0; }
        if (arg == "--assets") assets = value();
        else if (arg == "--layer") config.layers.push_back(value().u8string());
        else if (arg == "--floor") config.floor = integer(value().u8string());
        else if (arg == "--workers") {
            const auto n = integer(value().u8string());
            if (n < 1 || n > 32) throw std::invalid_argument("--workers must be 1 to 32");
            config.workers = static_cast<unsigned>(n);
        } else if (arg == "--cv-threads") threads = integer(value().u8string());
        else if (arg == "--repeat") repeats = integer(value().u8string());
        else if (arg == "--no-cache") config.reuse_frames = false;
        else if (arg == "--index-cache-dir") config.index_cache = value();
        else if (arg == "--no-index-cache") config.index_cache.clear();
        else if (arg == "--inset") config.inset = std::stod(value().u8string());
        else if (arg == "--circle") {
            const float x = std::stof(value().u8string());
            const float y = std::stof(value().u8string());
            const float radius = std::stof(value().u8string());
            config.circle = bgi::Circle{x, y, radius};
        } else if (arg == "--input") {
            const auto mode = value().u8string();
            if (mode == "video") kind = bgi::Input::video;
            else if (mode == "minimap") kind = bgi::Input::minimap;
            else if (mode == "game") kind = bgi::Input::game;
            else throw std::invalid_argument("invalid input mode " + mode);
        } else if (arg == "--method") {
            const auto method = value().u8string();
            if (method == "sift") config.method = bgi::Method::sift;
            else if (method == "template") config.method = bgi::Method::map_template;
            else if (method != "auto") throw std::invalid_argument("invalid matching method " + method);
        } else if (arg.compare(0, 2, "--") == 0) {
            throw std::invalid_argument("unknown option " + arg);
        } else {
            images.push_back(arguments[i]);
        }
    }
    if (assets.empty() || images.empty() || repeats < 1 || threads < 1 || threads > 64) {
        usage();
        return 1;
    }
    if (kind == bgi::Input::game) {
        config.min_inliers = 9;
        config.min_inlier_ratio = 0.27;
        config.max_error = 6;
    }
    cv::setNumThreads(threads);
    cv::ocl::setUseOpenCL(false);
    bgi::LocateEngine engine(assets, config);
    int exit_code = 0;
    for (const auto& path : images) {
        const auto frame = bgi::read_image(path, cv::IMREAD_COLOR);
        for (int i = 0; i < repeats; ++i) {
            const auto start = Clock::now();
            const auto result = engine.locate(frame, kind);
            const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            report(path, result, elapsed);
            if (!result.position) exit_code = 2;
        }
    }
    return exit_code;
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    try {
        std::vector<Path> arguments;
        for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        const auto cache_directory = binary_directory(Path(argv[0])) / "index-cache";
        return run(arguments, cache_directory);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
