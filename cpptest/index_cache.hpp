#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>

#include <opencv2/flann.hpp>

namespace bgi::detail {

using IndexKey = std::array<unsigned char, 32>;

std::optional<IndexKey> identify_index(const cv::Mat& descriptors, const void* keypoints, std::size_t keypoint_bytes);

std::unique_ptr<cv::flann::Index> load_or_build_index(const cv::Mat& descriptors,
                                                    const std::filesystem::path& directory,
                                                    const std::optional<IndexKey>& key, int floor);

}
