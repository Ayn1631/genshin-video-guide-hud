#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace bgi {

enum class Method { automatic, sift, map_template };
enum class Input { video, minimap, game, video_minimap };
enum class Search { track, global, local, tile };
enum class Phase { none, local, global, retry, cached, region, wait_region, wait_global, wait_initial };
enum class Failure { none, black, no_circle, no_features, no_tiles, coarse, fine, ambiguous, no_minimap };

struct Circle {
    float x;
    float y;
    float radius;
    bool detected = true;
};

struct Config {
    Method method = Method::automatic;
    unsigned workers = 4;
    int min_inliers = 15;
    double min_inlier_ratio = 0.5;
    double max_error = 3.0;
    std::optional<int> floor;
    std::vector<std::string> layers;
    std::optional<Circle> circle;
    std::filesystem::path index_cache;
    double inset = 12.0;
    bool reuse_frames = true;
};

struct Position {
    double x = 0;
    double y = 0;
    int floor = 0;
    Method method = Method::automatic;
    double image_x = 0;
    double image_y = 0;
    int inliers = 0;
    int matches = 0;
    double error = 0;
    std::string layer;
    double similarity = 0;
    int template_size = 0;
    double map_radius = 0;
    std::string scene = "Teyvat";
};

struct Result {
    std::optional<Position> position;
    Phase phase = Phase::none;
    Failure failure = Failure::none;
};

class LocateEngine {
public:
    LocateEngine(std::filesystem::path assets, Config config);
    ~LocateEngine();
    LocateEngine(const LocateEngine&) = delete;
    LocateEngine& operator=(const LocateEngine&) = delete;

    Result locate(const cv::Mat& input, Input kind, Search search = Search::track,
                  const std::function<void(Phase)>& notify = {}, const Position* prior = nullptr);
    void set_video_retry_limits(int local, int regional);
    int video_retry_progress(Phase phase) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

cv::Mat read_image(const std::filesystem::path& file, int mode);
const char* name(Method method);
const char* name(Phase phase);
const char* name(Failure failure);

}
