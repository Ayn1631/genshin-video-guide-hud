#include "detail.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string_view>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace bgi::detail {
namespace {

using Path = std::filesystem::path;
const std::vector<int> scales{30, 34, 38, 42, 46, 49, 52, 55, 58, 62, 67, 74, 82, 92, 104, 118};

struct Tile {
    std::string id;
    std::string scene;
    int floor;
    double left, top, scale;
    Path coarse, fine;
    cv::Mat pyramid;
    cv::Rect2d bounds;
};

cv::Size webp_size(const Path& path) {
    std::array<unsigned char, 30> header{};
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(header.data()), header.size())) return {};
    const auto chunk = std::string_view(reinterpret_cast<const char*>(header.data() + 12), 4);
    const auto u24 = [&](int i) { return header[i] | (header[i + 1] << 8) | (header[i + 2] << 16); };
    if (chunk == "VP8X") return {u24(24) + 1, u24(27) + 1};
    if (chunk == "VP8L" && header[20] == 0x2f) {
        const auto dimensions = static_cast<std::uint32_t>(u24(21)) | (static_cast<std::uint32_t>(header[24]) << 24);
        return {static_cast<int>((dimensions & 0x3fff) + 1), static_cast<int>(((dimensions >> 14) & 0x3fff) + 1)};
    }
    if (chunk == "VP8 " && u24(23) == 0x2a019d) return {u24(26) & 0x3fff, u24(27) >> 8 & 0x3fff};
    return {};
}

struct Prepared {
    cv::Mat gray, mask;
    double radius;
};

struct Texture {
    int size;
    cv::Mat patch, mask, detail, detail_mask;
};

struct Peak {
    double score;
    std::size_t tile;
    int width;
    cv::Point point;
};

template <class Function>
void parallel_for(std::size_t count, unsigned concurrency, Function&& run) {
    const auto workers = std::min<std::size_t>(concurrency, count);
    if (workers <= 1) {
        for (std::size_t i = 0; i < count; ++i) run(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::future<void>> tasks;
    tasks.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
        tasks.push_back(std::async(std::launch::async, [&] {
            for (auto i = next.fetch_add(1); i < count; i = next.fetch_add(1)) run(i);
        }));
    }
    for (auto& task : tasks) task.get();
}

void read_scene_tiles(const Path& root, const MapScene& scene, const Config& config, std::vector<Tile>& tiles) {
    std::vector<Path> manifests;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() == ".json") manifests.push_back(entry.path());
    }
    std::sort(manifests.begin(), manifests.end());
    std::map<std::string, std::size_t> existing;
    for (const auto& path : manifests) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("cannot read map manifest: " + path.u8string());
        std::string json(std::istreambuf_iterator<char>(stream), {});
        if (json.compare(0, 3, "\xef\xbb\xbf") == 0) json.erase(0, 3);
        cv::FileStorage manifest("{\"tiles\":" + json + "}", cv::FileStorage::READ |
                                 cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
        const auto items = manifest["tiles"];
        if (!items.isSeq()) throw std::runtime_error("invalid map manifest: " + path.u8string());
        for (const auto& entry : items) {
            const auto id = static_cast<std::string>(entry["LayerId"]);
            const int floor = static_cast<int>(entry["Floor"]);
            if (config.floor && floor != *config.floor) continue;
            if (!config.layers.empty() && std::find(config.layers.begin(), config.layers.end(), id) == config.layers.end()) {
                continue;
            }
            if (id.empty() || Path(id).filename().u8string() != id) continue;
            const auto coarse = root / (id + "_color.webp");
            const auto extension = static_cast<int>(entry["IsOverSize"]) ? ".png" : ".webp";
            const auto fine = root / (id + "_gray" + extension);
            if (!std::filesystem::is_regular_file(coarse) || !std::filesystem::is_regular_file(fine)) continue;
            Tile tile{id, scene.name, floor, static_cast<double>(entry["Left"]), static_cast<double>(entry["Top"]),
                      static_cast<double>(entry["Scale"]), coarse, fine, {}, {}};
            if (!std::isfinite(tile.left) || !std::isfinite(tile.top) || !std::isfinite(tile.scale) || tile.scale <= 0) {
                throw std::runtime_error("invalid coordinates for map tile " + id);
            }
            const auto size = webp_size(coarse);
            if (!size.empty()) {
                const auto width = size.width * 5.0 / tile.scale;
                const auto height = size.height * 5.0 / tile.scale;
                tile.bounds = {tile.left - width, tile.top - height, width, height};
            }
            const auto inserted = existing.emplace(id, tiles.size());
            if (inserted.second) tiles.push_back(std::move(tile));
            else tiles[inserted.first->second] = std::move(tile);
        }
    }
}

std::vector<Tile> read_tiles(const Path& root, const Config& config) {
    std::vector<Tile> tiles;
    for (const auto& [directory, scene] : scene_assets(root)) read_scene_tiles(directory, *scene, config, tiles);
    for (const auto& id : config.layers) {
        if (std::none_of(tiles.begin(), tiles.end(), [&](const auto& tile) { return tile.id == id; })) {
            throw std::runtime_error("missing template map layer: " + id);
        }
    }
    return tiles;
}

cv::Mat texture(const cv::Mat& gray) {
    cv::Mat detail, blurred;
    constexpr auto border = cv::BORDER_DEFAULT | cv::BORDER_ISOLATED;
    cv::GaussianBlur(gray, detail, {3, 3}, 0.6, 0, border);
    detail.convertTo(detail, CV_32F);
    cv::GaussianBlur(detail, blurred, {}, 2, 0, border);
    return detail - blurred;
}

Prepared prepare(const Query& query) {
    const double radius = std::min(query.center.x, query.center.y);
    const auto size = 2 * cvRound(0.92 * radius) + 1;
    Prepared result;
    result.radius = radius;
    cv::getRectSubPix(query.gray, {size, size}, query.center, result.gray);
    cv::getRectSubPix(query.mask, {size, size}, query.center, result.mask);
    result.mask.convertTo(result.mask, CV_32F, 1.0 / 255.0);
    cv::circle(result.mask, {size / 2, size / 2}, cvRound(radius * 0.17), 0, cv::FILLED);
    return result;
}

Texture resize_query(const Prepared& query, int size) {
    Texture result{size, {}, {}, {}, {}};
    cv::resize(query.gray, result.patch, {size, size}, 0, 0, cv::INTER_AREA);
    cv::resize(query.mask, result.mask, {size, size}, 0, 0, cv::INTER_AREA);
    cv::compare(result.mask, 0.99, result.mask, cv::CMP_GE);
    cv::erode(result.mask, result.detail_mask, cv::Mat::ones(3, 3, CV_8U));
    result.detail = texture(result.patch);
    return result;
}

std::vector<Texture> query_scales(const Prepared& query, const std::vector<int>& sizes, double coverage) {
    std::vector<Texture> shapes;
    for (const int size : sizes) {
        auto item = resize_query(query, size);
        if (cv::countNonZero(item.detail_mask) >= size * size * coverage) shapes.push_back(std::move(item));
    }
    return shapes;
}

std::pair<double, cv::Point> texture_peak(const cv::Mat& ref, const Texture& sample) {
    cv::Mat scores, valid;
    cv::matchTemplate(ref, sample.detail, scores, cv::TM_CCORR_NORMED, sample.detail_mask);
    cv::inRange(scores, -1.001, 1.001, valid);
    double best = -1;
    cv::Point position;
    cv::minMaxLoc(scores, nullptr, &best, nullptr, &position, valid);
    if (position.x < 0) best = -1;
    return {best, position};
}

double level_score(const cv::Mat& first, const cv::Mat& second, const cv::Mat& mask) {
    double count = 0, a = 0, b = 0, aa = 0, bb = 0, ab = 0;
    for (int y = 0; y < mask.rows; ++y) {
        const auto* valid = mask.ptr<uchar>(y);
        const auto* px = first.ptr<uchar>(y);
        const auto* py = second.ptr<uchar>(y);
        for (int x = 0; x < mask.cols; ++x) {
            if (!valid[x]) continue;
            ++count;
            a += px[x];
            b += py[x];
            aa += px[x] * px[x];
            bb += py[x] * py[x];
            ab += px[x] * py[x];
        }
    }
    if (count < 2) return -1;
    const auto var_a = aa - a * a / count;
    const auto var_b = bb - b * b / count;
    if (var_a < count || var_b < count) return -1;
    return (ab - a * b / count) / std::sqrt(var_a * var_b);
}

}

struct Templates::Impl {
    unsigned workers;
    std::vector<Tile> tiles;
    std::vector<std::pair<Path, cv::Mat>> fine_cache;

    cv::Mat fine_image(const Path& path) {
        const auto found = std::find_if(fine_cache.begin(), fine_cache.end(), [&](const auto& item) {
            return item.first == path;
        });
        if (found != fine_cache.end()) {
            auto cached = std::move(*found);
            fine_cache.erase(found);
            fine_cache.push_back(std::move(cached));
        } else {
            auto image = read_image(path, cv::IMREAD_GRAYSCALE);
            if (fine_cache.size() == 2) fine_cache.erase(fine_cache.begin());
            fine_cache.emplace_back(path, std::move(image));
        }
        return fine_cache.back().second;
    }

    std::optional<Position> fine_match(const Tile& tile, const Prepared& query, std::map<int, Texture>& shapes,
                                       cv::Point center, int nominal, int range, const SearchScope* scope = nullptr) {
        const auto fine = fine_image(tile.fine);
        const int extent = nominal / 2 + std::max(18, cvRound(nominal * 0.14));
        const auto bounds = cv::Rect(center.x - extent, center.y - extent, extent * 2, extent * 2) &
                            cv::Rect(0, 0, fine.cols, fine.rows);
        if (bounds.empty()) return {};
        const auto region = fine(bounds);
        const auto detail = texture(region);
        std::optional<Position> best;
        for (int size = nominal - range; size <= nominal + range; size += 2) {
            if (size <= 0 || size > std::min(region.cols, region.rows)) continue;
            auto shape = shapes.find(size);
            if (shape == shapes.end()) {
                shape = shapes.emplace(size, resize_query(query, size)).first;
            }
            const auto& sample = shape->second;
            const auto [edge, point] = texture_peak(detail, sample);
            if (edge < 0.62) continue;
            const auto level = level_score(sample.patch, region(cv::Rect(point.x, point.y, size, size)), sample.mask);
            const auto similarity = 0.55 * level + 0.45 * edge;
            if (level < 0.72 || similarity < 0.73 || (best && similarity <= best->similarity)) continue;
            Position match;
            match.method = Method::map_template;
            match.floor = tile.floor;
            match.scene = tile.scene;
            match.layer = tile.id;
            match.image_x = bounds.x + point.x + size / 2.0;
            match.image_y = bounds.y + point.y + size / 2.0;
            match.x = tile.left - match.image_x / tile.scale;
            match.y = tile.top - match.image_y / tile.scale;
            if (scope && scope->country && !scope->country->contains({match.x, match.y})) continue;
            match.similarity = similarity;
            match.template_size = size;
            match.map_radius = query.radius * size / query.gray.cols / tile.scale;
            best = std::move(match);
        }
        return best;
    }

    std::vector<std::size_t> shortlist(const Prepared& query, const std::vector<std::size_t>& candidates) {
        std::vector<int> reduced;
        for (const auto size : scales) {
            const auto width = cvRound(size / 2.0);
            if (reduced.empty() || reduced.back() != width) reduced.push_back(width);
        }
        const auto shapes = query_scales(query, reduced, 0.12);
        std::vector<double> scores(tiles.size(), -1);
        parallel_for(candidates.size(), workers, [&](std::size_t slot) {
            const auto i = candidates[slot];
            auto& tile = tiles[i];
            if (tile.pyramid.empty()) {
                const auto gray = read_image(tile.coarse, cv::IMREAD_GRAYSCALE);
                cv::Mat smaller;
                cv::resize(gray, smaller, {std::max(1, cvRound(gray.cols / 2.0)), std::max(1, cvRound(gray.rows / 2.0))},
                           0, 0, cv::INTER_AREA);
                tile.pyramid = texture(smaller);
            }
            const auto limit = std::min(tile.pyramid.cols, tile.pyramid.rows);
            for (const auto& shape : shapes) {
                if (shape.size <= limit) scores[i] = std::max(scores[i], texture_peak(tile.pyramid, shape).first);
            }
        });
        std::vector<std::size_t> ranked;
        for (auto i : candidates) {
            if (scores[i] >= 0.53) ranked.push_back(i);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [&](auto a, auto b) { return scores[a] > scores[b]; });
        return ranked;
    }

    Result refine_group(const Prepared& query, const std::vector<Texture>& shapes, std::map<int, Texture>& fine_shapes,
                        const std::vector<std::size_t>& ranked, std::size_t first, std::size_t last,
                        const SearchScope* scope) {
        std::vector<std::vector<Peak>> groups(last - first);
        parallel_for(last - first, workers, [&](std::size_t slot) {
            const auto id = ranked[first + slot];
            const auto ref = texture(read_image(tiles[id].coarse, cv::IMREAD_GRAYSCALE));
            const int limit = std::min(ref.cols, ref.rows);
            for (const auto& shape : shapes) {
                if (shape.size > limit) continue;
                const auto [score, point] = texture_peak(ref, shape);
                if (score >= 0.57) groups[slot].push_back({score, id, shape.size, point});
            }
        });
        std::vector<Peak> peaks;
        for (const auto& group : groups) peaks.insert(peaks.end(), group.begin(), group.end());
        if (peaks.empty()) return {{}, Phase::global, Failure::coarse};
        std::stable_sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
        std::vector<Position> results;
        for (std::size_t i = 0; i < std::min<std::size_t>(peaks.size(), 18); ++i) {
            const auto& peak = peaks[i];
            const auto center = cv::Point(cvRound((peak.point.x + peak.width / 2.0) * 5),
                                          cvRound((peak.point.y + peak.width / 2.0) * 5));
            auto result = fine_match(tiles[peak.tile], query, fine_shapes, center, peak.width * 5, 18, scope);
            if (result) results.push_back(std::move(*result));
        }
        const auto best = std::max_element(results.begin(), results.end(), [](const auto& a, const auto& b) {
            return a.similarity < b.similarity;
        });
        if (best == results.end()) return {{}, Phase::global, Failure::fine};
        for (const auto& result : results) {
            if (best->similarity - result.similarity < 0.04 &&
                (best->scene != result.scene || best->floor != result.floor ||
                 std::hypot(best->x - result.x, best->y - result.y) > 20)) {
                return {{}, Phase::global, Failure::ambiguous};
            }
        }
        return {*best, Phase::global, Failure::none};
    }
};

Templates::Templates(const Path& assets, const Config& config)
    : impl_(std::make_unique<Impl>()) {
    impl_->workers = config.workers;
    impl_->tiles = read_tiles(assets, config);
}

Templates::~Templates() = default;

Result Templates::locate(const Query& raw, const Position* near, const SearchScope* scope, const Position* locked_tile) {
    const auto query = prepare(raw);
    auto& model = *impl_;
    std::map<int, Texture> fine_shapes;
    if (near) {
        const auto it = std::find_if(model.tiles.begin(), model.tiles.end(), [&](const auto& tile) {
            return tile.id == near->layer && tile.scene == near->scene;
        });
        if (it != model.tiles.end() && near->template_size > 18) {
            const auto center = cv::Point(cvRound((it->left - near->x) * it->scale), cvRound((it->top - near->y) * it->scale));
            auto result = model.fine_match(*it, query, fine_shapes, center, near->template_size, 4);
            if (result && result->similarity >= std::max(0.73, near->similarity - 0.04)) {
                return {std::move(result), Phase::local, Failure::none};
            }
        }
        return {{}, Phase::local, Failure::fine};
    }

    const auto shapes = query_scales(query, scales, 0.15);
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < model.tiles.size(); ++i) {
        const auto& tile = model.tiles[i];
        if (locked_tile && (locked_tile->layer != tile.id || locked_tile->scene != tile.scene ||
                            locked_tile->floor != tile.floor)) continue;
        if (scope && (scope->scene != tile.scene ||
                      (scope->country && !tile.bounds.empty() && !scope->country->overlaps(tile.bounds)))) continue;
        candidates.push_back(i);
    }
    if (candidates.empty()) return {{}, Phase::global, Failure::no_tiles};
    if (candidates.size() <= 12) return model.refine_group(query, shapes, fine_shapes, candidates, 0, candidates.size(), scope);
    const auto ranked = model.shortlist(query, candidates);
    if (ranked.empty()) return {{}, Phase::global, Failure::no_tiles};
    Result result{{}, Phase::global, Failure::coarse};
    for (std::size_t first = 0, batch = 12; first < ranked.size(); batch = 24) {
        const auto last = std::min(first + batch, ranked.size());
        auto attempt = model.refine_group(query, shapes, fine_shapes, ranked, first, last, scope);
        if (attempt.position || attempt.failure == Failure::ambiguous) return attempt;
        result.failure = std::max(result.failure, attempt.failure);
        first = last;
    }
    return result;
}

}
