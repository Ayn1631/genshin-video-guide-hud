#pragma once

#include "map_locator.hpp"
#include "map_scopes.hpp"

#include <opencv2/features2d.hpp>

namespace bgi::detail {

struct Query {
    cv::Mat gray;
    cv::Mat mask;
    cv::Point2f center;
    mutable cv::Mat descriptors;
    mutable std::vector<cv::KeyPoint> keypoints;
    mutable bool extracted = false;
};

Query prepare_query(const cv::Mat& image);
cv::Mat video_content(const cv::Mat& frame);
bool black_frame(const cv::Mat& frame);
bool has_video_minimap(const cv::Mat& crop);
std::vector<Circle> find_circles(const cv::Mat& frame);
cv::Mat crop_circle(const cv::Mat& frame, const Circle& circle);
cv::Mat crop_manual(const cv::Mat& frame, const Circle& circle, double inset);
cv::Mat crop_game(const cv::Mat& frame);

class Sift {
public:
    Sift(const std::filesystem::path& assets, Config config);
    ~Sift();
    std::optional<Position> locate(const Query& query, bool local, const Position* near,
                                   const SearchScope* scope = nullptr) const;
    bool available() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Templates {
public:
    Templates(const std::filesystem::path& assets, const Config& config);
    ~Templates();
    Result locate(const Query& query, const Position* near, const SearchScope* scope = nullptr,
                  const Position* tile = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
