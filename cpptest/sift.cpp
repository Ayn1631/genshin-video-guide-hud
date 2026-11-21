#include "detail.hpp"
#include "index_cache.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include <opencv2/calib3d.hpp>
#include <opencv2/flann.hpp>
#include <opencv2/imgcodecs.hpp>

namespace bgi::detail {
namespace {

struct KeypointData {
    float x, y, size, angle, response;
    std::int32_t octave, class_id;
};
static_assert(sizeof(KeypointData) == 28);

struct FeatureLayer {
    struct RegionIndex {
        std::once_flag initialize;
        std::vector<int> ids;
        std::unique_ptr<cv::flann::Index> index;
    };

    int floor;
    const MapScene* scene;
    std::filesystem::path keypoints_file;
    std::filesystem::path descriptors_file;
    std::filesystem::path image_file;
    std::once_flag load_once;
    std::vector<cv::Point2f> points;
    std::vector<int> ordered_ids;
    cv::Mat descriptors;
    std::optional<IndexKey> index_key;
    std::unique_ptr<cv::flann::Index> global_index;
    std::once_flag initialize_global;
    std::map<const Country*, RegionIndex> regional_indices;
    std::mutex regions_mutex;

    void load() {
        std::call_once(load_once, [&] {
            if (!image_file.empty()) {
                auto image = read_image(image_file, cv::IMREAD_GRAYSCALE);
                std::vector<cv::KeyPoint> keypoints;
                cv::SIFT::create(15000, 3, 0.03)->detectAndCompute(image, {}, keypoints, descriptors);
                for (const auto& keypoint : keypoints) points.push_back(keypoint.pt);
                order_points();
                index_key = identify_index(descriptors, points.data(), points.size() * sizeof(cv::Point2f));
                return;
            }
            const auto size = std::filesystem::file_size(keypoints_file);
            if (size == 0 || size % sizeof(KeypointData) != 0 ||
                size / sizeof(KeypointData) > static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
                throw std::runtime_error("invalid BetterGI keypoint file: " + keypoints_file.u8string());
            }
            std::vector<KeypointData> file_points(static_cast<std::size_t>(size / sizeof(KeypointData)));
            std::ifstream keypoints(keypoints_file, std::ios::binary);
            if (!keypoints.read(reinterpret_cast<char*>(file_points.data()), static_cast<std::streamsize>(size))) {
                throw std::runtime_error("cannot read BetterGI keypoint file");
            }
            const auto encoded = read_image(descriptors_file, cv::IMREAD_GRAYSCALE);
            if (encoded.rows != static_cast<int>(file_points.size()) || encoded.cols != 128) {
                throw std::runtime_error("BetterGI keypoint count does not equal the descriptor row count");
            }
            std::vector<cv::Point2f> xy;
            xy.reserve(file_points.size());
            for (const auto& point : file_points) {
                if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                    throw std::runtime_error("BetterGI map contains an invalid keypoint");
                }
                xy.emplace_back(point.x, point.y);
            }
            index_key = identify_index(encoded, file_points.data(), file_points.size() * sizeof(KeypointData));
            encoded.convertTo(descriptors, CV_32F);
            points = std::move(xy);
            order_points();
        });
    }

    void order_points() {
        ordered_ids.resize(points.size());
        std::iota(ordered_ids.begin(), ordered_ids.end(), 0);
        std::sort(ordered_ids.begin(), ordered_ids.end(), [&](int a, int b) { return points[a].x < points[b].x; });
    }

    double units_scale() const { return scene->rank / 1024.0; }

    RegionIndex& index_for(const Country& country, const std::filesystem::path& cache) {
        RegionIndex* region;
        {
            std::lock_guard lock(regions_mutex);
            region = &regional_indices.try_emplace(&country).first->second;
        }
        std::call_once(region->initialize, [&] {
            std::vector<int> ids;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto p = (scene->origin - cv::Point2d(points[i])) / units_scale();
                if (country.contains(p)) ids.push_back(static_cast<int>(i));
            }
            if (ids.size() < 16) return;
            cv::Mat subset(static_cast<int>(ids.size()), descriptors.cols, descriptors.type());
            for (int i = 0; i < subset.rows; ++i) descriptors.row(ids[i]).copyTo(subset.row(i));
            const auto key = identify_index(subset, ids.data(), ids.size() * sizeof(int));
            region->index = load_or_build_index(subset, cache, key, floor);
            region->ids = std::move(ids);
        });
        return *region;
    }

    std::vector<int> nearby(const Position& prior) const {
        const auto local_radius = 350 * units_scale();
        const auto begin = std::lower_bound(ordered_ids.begin(), ordered_ids.end(), prior.image_x - local_radius,
                                            [&](int id, double x) { return points[id].x < x; });
        const auto end = std::upper_bound(begin, ordered_ids.end(), prior.image_x + local_radius,
                                          [&](double x, int id) { return x < points[id].x; });
        std::vector<int> ids;
        for (auto it = begin; it != end; ++it) {
            if (std::abs(points[*it].y - prior.image_y) <= local_radius) ids.push_back(*it);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }
};

struct FeatureAtlas {
    std::vector<std::unique_ptr<FeatureLayer>> layers;
};

std::shared_ptr<FeatureAtlas> atlas_for(const std::filesystem::path& assets) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::weak_ptr<FeatureAtlas>> registry;
    const auto root = std::filesystem::weakly_canonical(assets);
    std::lock_guard lock(mutex);
    if (auto reused = registry[root].lock()) return reused;
    auto atlas = std::make_shared<FeatureAtlas>();
    for (const auto& [directory, scene] : scene_assets(root)) {
      const auto prefix = std::string(scene->name) + "_";
      const auto rank = std::to_string(scene->rank);
      const auto suffix = "_" + rank + "_SIFT.kp.bin";
      const auto first = atlas->layers.size();
      for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto filename = entry.path().filename().u8string();
        const auto prefix_size = prefix.size();
        const auto suffix_size = suffix.size();
        if (filename.size() <= prefix_size + suffix_size ||
            filename.compare(0, prefix_size, prefix) != 0 ||
            filename.compare(filename.size() - suffix_size, suffix_size, suffix) != 0) {
            continue;
        }
        const auto number = filename.substr(prefix_size, filename.size() - prefix_size - suffix_size);
        std::size_t parsed = 0;
        const int floor = std::stoi(number, &parsed);
        if (parsed != number.size()) continue;
        const auto descriptions = directory / (prefix + number + "_" + rank + "_SIFT.mat.png");
        if (!std::filesystem::is_regular_file(descriptions)) continue;
        auto layer = std::make_unique<FeatureLayer>();
        layer->floor = floor;
        layer->scene = scene;
        layer->keypoints_file = entry.path();
        layer->descriptors_file = descriptions;
        atlas->layers.push_back(std::move(layer));
      }
      if (atlas->layers.size() == first && std::string_view(scene->name) != "Teyvat") {
          const auto image = directory / (prefix + "0_" + rank + ".png");
          if (std::filesystem::is_regular_file(image)) {
              auto layer = std::make_unique<FeatureLayer>();
              layer->floor = 0;
              layer->scene = scene;
              layer->image_file = image;
              atlas->layers.push_back(std::move(layer));
          }
      }
      std::sort(atlas->layers.begin() + first, atlas->layers.end(), [](const auto& a, const auto& b) {
          return a->floor < b->floor;
      });
    }
    registry[root] = atlas;
    return atlas;
}

std::optional<Position> verify(const FeatureLayer& layer, const Config& config, const Query& query,
                               const std::vector<std::vector<cv::DMatch>>& pairs,
                               const std::vector<int>* ids) {
    std::vector<cv::Point2f> source, target;
    std::unordered_set<int> used;
    for (const auto& pair : pairs) {
        if (pair.size() < 2 || pair[0].distance >= 0.73 * pair[1].distance) continue;
        const auto& nearest = pair[0];
        const int id = ids ? (*ids)[nearest.trainIdx] : nearest.trainIdx;
        if (!used.insert(id).second) continue;
        source.push_back(query.keypoints[nearest.queryIdx].pt);
        target.push_back(layer.points[id]);
    }
    const auto count = static_cast<int>(source.size());
    if (count < config.min_inliers) return {};

    cv::Mat inliers;
    const auto pose = cv::estimateAffinePartial2D(source, target, inliers, cv::RANSAC, 7, 5000, 0.995, 12);
    if (pose.empty() || inliers.empty()) return {};
    const int inlier_count = cv::countNonZero(inliers);
    if (inlier_count < config.min_inliers || inlier_count < count * config.min_inlier_ratio) return {};
    const auto* row_x = pose.ptr<double>(0);
    const auto* row_y = pose.ptr<double>(1);
    const auto scale = std::hypot(row_x[0], row_y[0]);
    if (!std::isfinite(scale) || scale < 0.2 || scale > 30) return {};

    const auto project = [&](cv::Point2f p) {
        return cv::Point2d(row_x[0] * p.x + row_x[1] * p.y + row_x[2],
                          row_y[0] * p.x + row_y[1] * p.y + row_y[2]);
    };
    cv::Point2f low(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    cv::Point2f high(-low.x, -low.y);
    double squared_error = 0;
    for (int i = 0; i < count; ++i) {
        if (!inliers.at<uchar>(i)) continue;
        const auto delta = project(source[i]) - cv::Point2d(target[i]);
        squared_error += delta.dot(delta);
        low.x = std::min(low.x, source[i].x);
        low.y = std::min(low.y, source[i].y);
        high.x = std::max(high.x, source[i].x);
        high.y = std::max(high.y, source[i].y);
    }
    const double rms = std::sqrt(squared_error / inlier_count);
    if (std::min(high.x - low.x, high.y - low.y) < 22 || !std::isfinite(rms) || rms > config.max_error) {
        return {};
    }
    const auto center = project(query.center);
    if (!std::isfinite(center.x) || !std::isfinite(center.y)) return {};
    Position result;
    result.image_x = center.x;
    result.image_y = center.y;
    result.x = (layer.scene->origin.x - center.x) / layer.units_scale();
    result.y = (layer.scene->origin.y - center.y) / layer.units_scale();
    result.scene = layer.scene->name;
    result.method = Method::sift;
    result.floor = layer.floor;
    result.inliers = inlier_count;
    result.matches = count;
    result.error = rms;
    result.map_radius = std::min(query.center.x, query.center.y) * scale / layer.units_scale();
    return result;
}

}

struct Sift::Impl {
    std::shared_ptr<FeatureAtlas> atlas;
    Config config;
    cv::Ptr<cv::SIFT> extract = cv::SIFT::create(2500);
    cv::BFMatcher local_matcher{cv::NORM_L2};
};

Sift::Sift(const std::filesystem::path& assets, Config config)
    : impl_(std::make_unique<Impl>()) {
    impl_->atlas = atlas_for(assets);
    impl_->config = std::move(config);
    impl_->extract = cv::SIFT::create(2500, 3, 0.03);
}

Sift::~Sift() = default;

bool Sift::available() const {
    return std::any_of(impl_->atlas->layers.begin(), impl_->atlas->layers.end(), [&](const auto& layer) {
        return !impl_->config.floor || layer->floor == *impl_->config.floor;
    });
}

std::optional<Position> Sift::locate(const Query& query, bool local, const Position* near, const SearchScope* scope) const {
    if (local && !near) return {};
    if (!query.extracted) {
        impl_->extract->detectAndCompute(query.gray, query.mask, query.keypoints, query.descriptors);
        query.extracted = true;
    }
    const auto& config = impl_->config;
    if (query.descriptors.rows < config.min_inliers) return {};

    std::optional<Position> best;
    for (auto& layer : impl_->atlas->layers) {
        if (best && best->scene != layer->scene->name) break;
        if ((config.floor && layer->floor != *config.floor) ||
            (local && (near->floor != layer->floor || near->scene != layer->scene->name)) ||
            (scope && scope->scene != layer->scene->name)) continue;
        layer->load();
        if (layer->descriptors.rows < 2) continue;
        std::vector<std::vector<cv::DMatch>> pairs;
        std::vector<int> ids;
        const std::vector<int>* matches_in = nullptr;
        if (local) {
            ids = layer->nearby(*near);
            if (ids.size() < 16) continue;
            cv::Mat window(static_cast<int>(ids.size()), layer->descriptors.cols, CV_32F);
            for (int i = 0; i < window.rows; ++i) {
                layer->descriptors.row(ids[i]).copyTo(window.row(i));
            }
            impl_->local_matcher.knnMatch(query.descriptors, window, pairs, 2);
            matches_in = &ids;
        } else {
            cv::flann::Index* index;
            int index_rows = layer->descriptors.rows;
            if (scope && scope->country) {
                auto& regional = layer->index_for(*scope->country, config.index_cache);
                if (!regional.index) continue;
                index = regional.index.get();
                matches_in = &regional.ids;
                index_rows = static_cast<int>(regional.ids.size());
            } else {
                std::call_once(layer->initialize_global, [&] {
                    layer->global_index = load_or_build_index(layer->descriptors, config.index_cache,
                                                              layer->index_key, layer->floor);
                });
                index = layer->global_index.get();
            }
            cv::Mat neighbors, distances;
            // The OpenCV KD-tree search uses a per-thread heap and reads the published index.
            index->knnSearch(query.descriptors, neighbors, distances, 2, cv::flann::SearchParams(96));
            pairs.resize(static_cast<std::size_t>(neighbors.rows));
            for (int row = 0; row < neighbors.rows; ++row) {
                for (int col = 0; col < neighbors.cols; ++col) {
                    const int id = neighbors.at<int>(row, col);
                    if (id >= 0 && id < index_rows) {
                        // FLANN returns squared L2, while the ratio test uses L2.
                        pairs[row].emplace_back(row, id, std::sqrt(distances.at<float>(row, col)));
                    }
                }
            }
        }
        auto candidate = verify(*layer, config, query, pairs, matches_in);
        if (candidate && scope && scope->country && !scope->country->contains({candidate->x, candidate->y})) continue;
        const auto priority = [](const Position& p) {
            return std::tuple{p.inliers, static_cast<double>(p.inliers) / p.matches, -p.error};
        };
        if (candidate && (!best || priority(*candidate) > priority(*best))) best = std::move(candidate);
    }
    return best;
}

}
