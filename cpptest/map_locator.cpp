#include "detail.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <opencv2/imgproc.hpp>

namespace bgi {
namespace {

bool same_frame(const cv::Mat& a, const cv::Mat& b) {
    if (a.empty() || a.size() != b.size() || a.type() != b.type()) return false;
    const auto bytes = a.cols * a.elemSize();
    if (a.isContinuous() && b.isContinuous()) {
        return std::memcmp(a.data, b.data, a.total() * a.elemSize()) == 0;
    }
    for (int row = 0; row < a.rows; ++row) {
        if (std::memcmp(a.ptr(row), b.ptr(row), bytes) != 0) return false;
    }
    return true;
}

bool same_circle(const std::optional<Circle>& a, const std::optional<Circle>& b) {
    if (!a || !b) return !a && !b;
    return a->x == b->x && a->y == b->y && a->radius == b->radius;
}

}

struct LocateEngine::Impl {
    std::filesystem::path assets;
    Config config;
    std::unique_ptr<detail::Sift> sift;
    std::unique_ptr<detail::Templates> templates;
    std::optional<Position> position;
    std::optional<Circle> circle;
    cv::Mat last_frame;
    Input last_input = Input::video;
    Result last_result;
    int local_limit = 1, regional_limit = 1;
    int local_misses = 0, regional_misses = 0;
    bool tried_initial_search = false;

    detail::Templates& atlas() {
        if (!templates) templates = std::make_unique<detail::Templates>(assets, config);
        return *templates;
    }

    Result attempt_local(const detail::Query& query, const Position& prior) {
        if (prior.method == Method::sift && sift) {
            auto point = sift->locate(query, true, &prior);
            return point ? Result{std::move(point), Phase::local, Failure::none}
                         : Result{{}, Phase::local, Failure::no_features};
        }
        if (prior.method == Method::map_template && config.method != Method::sift) {
            return atlas().locate(query, &prior);
        }
        return {{}, Phase::local, Failure::no_features};
    }
};

LocateEngine::LocateEngine(std::filesystem::path assets, Config config) : impl_(std::make_unique<Impl>()) {
    if (config.workers < 1 || config.min_inliers < 4 || config.min_inlier_ratio <= 0 || config.min_inlier_ratio > 1 ||
        config.max_error <= 0) {
        throw std::invalid_argument("invalid matcher configuration");
    }
    if (!std::filesystem::is_directory(assets)) {
        throw std::invalid_argument("BetterGI map directory is missing: " + assets.u8string());
    }
    impl_->assets = std::move(assets);
    impl_->config = std::move(config);
    if (!impl_->config.layers.empty() && impl_->config.method == Method::automatic) {
        impl_->config.method = Method::map_template;
    }
    if (impl_->config.method != Method::map_template) {
        impl_->sift = std::make_unique<detail::Sift>(impl_->assets, impl_->config);
        if (!impl_->sift->available()) {
            if (impl_->config.method == Method::sift) throw std::runtime_error("SIFT map assets were not found");
            impl_->sift.reset();
        }
    }
}

LocateEngine::~LocateEngine() = default;

void LocateEngine::set_video_retry_limits(int local, int regional) {
    if (local < 1 || regional < 1) throw std::invalid_argument("retry limits must be positive");
    impl_->local_limit = local;
    impl_->regional_limit = regional;
    impl_->local_misses = impl_->regional_misses = 0;
    impl_->last_frame.release();
}

int LocateEngine::video_retry_progress(Phase phase) const {
    return phase == Phase::wait_global ? impl_->regional_misses : impl_->local_misses;
}

Result LocateEngine::locate(const cv::Mat& input, Input kind, Search search,
                            const std::function<void(Phase)>& notify, const Position* prior) {
    const bool is_video = kind == Input::video;
    const bool from_video = is_video || kind == Input::video_minimap;
    if (input.empty() || input.depth() != CV_8U || (from_video && input.channels() != 3)) {
        throw std::invalid_argument("input must be a nonempty 8-bit image");
    }
    auto& model = *impl_;
    const bool video_tracking = from_video && search == Search::track && !prior;
    const bool cache = video_tracking && model.config.reuse_frames;
    if (cache && model.last_input == kind && same_frame(input, model.last_frame)) {
        auto result = model.last_result;
        if (result.phase < Phase::wait_region) result.phase = Phase::cached;
        return result;
    }
    const auto finish = [&](Result result, std::optional<Circle> circle) {
        if (video_tracking && (result.position || result.failure == Failure::black ||
                               result.failure == Failure::no_minimap || result.phase == Phase::global ||
                               result.phase == Phase::retry)) {
            model.local_misses = model.regional_misses = 0;
        }
        if (result.position) {
            model.position = result.position;
            model.circle = circle;
        }
        if (cache) {
            input.copyTo(model.last_frame);
            model.last_input = kind;
            model.last_result = result;
        } else {
            model.last_frame.release();
        }
        return result;
    };

    if (from_video && detail::black_frame(input)) {
        return finish({{}, Phase::none, Failure::black}, {});
    }
    const cv::Mat frame = is_video ? detail::video_content(input) : kind == Input::game ? detail::crop_game(input) : input;
    if (search == Search::tile) {
        if (!prior || prior->method != Method::map_template || prior->layer.empty() ||
            model.config.method == Method::sift) {
            return {{}, Phase::local, Failure::no_tiles};
        }
        auto result = model.atlas().locate(detail::prepare_query(frame), nullptr, nullptr, prior);
        result.phase = Phase::local;
        return result;
    }
    const bool manual = is_video && model.config.circle.has_value();
    const auto crop = [&](const std::optional<Circle>& circle) {
        if (manual) return detail::crop_manual(frame, *model.config.circle, model.config.inset);
        return circle ? detail::crop_circle(frame, *circle) : frame;
    };
    const auto anchor = search == Search::global ? nullptr : prior ? prior : model.position ? &*model.position : nullptr;
    const auto old_circle = is_video && !manual ? model.circle : model.config.circle;
    std::optional<detail::Query> old_query;
    if (anchor && (old_circle || !is_video)) {
        old_query = detail::prepare_query(crop(old_circle));
        auto local = model.attempt_local(*old_query, *anchor);
        if (local.position || search == Search::local) return finish(std::move(local), old_circle);
    }
    if (search == Search::local) return finish({{}, Phase::local, Failure::no_features}, {});

    struct Candidate {
        std::optional<Circle> circle;
        cv::Mat crop;
        std::optional<detail::Query> query;
    };
    std::vector<Candidate> candidates;
    const auto add_candidate = [&](const std::optional<Circle>& circle) {
        auto minimap = crop(circle);
        if (!from_video || detail::has_video_minimap(minimap)) {
            candidates.push_back({circle, std::move(minimap), {}});
        }
    };
    if (!is_video || manual) {
        add_candidate(manual ? model.config.circle : std::nullopt);
    } else {
        for (const auto& circle : detail::find_circles(frame)) add_candidate(circle);
    }
    if (candidates.empty()) {
        return finish({{}, Phase::none, Failure::no_minimap}, {});
    }
    auto prepare = [&](Candidate& candidate) -> detail::Query& {
        if (!candidate.query) {
            if (old_query && same_circle(old_circle, candidate.circle)) {
                candidate.query = std::move(old_query);
                old_query.reset();
            } else {
                candidate.query = detail::prepare_query(candidate.crop);
            }
        }
        return *candidate.query;
    };
    const auto attempt = [&](Phase phase, const detail::SearchScope* scope) -> Result {
        if (notify) notify(phase);
        if (model.sift) {
            for (auto& candidate : candidates) {
                auto point = model.sift->locate(prepare(candidate), false, nullptr, scope);
                if (point) return finish({std::move(point), phase, Failure::none}, candidate.circle);
            }
        }
        Failure failure = Failure::no_tiles;
        if (model.config.method != Method::sift) {
            for (auto& candidate : candidates) {
                auto result = model.atlas().locate(prepare(candidate), nullptr, scope);
                if (result.position) {
                    result.phase = phase;
                    return finish(std::move(result), candidate.circle);
                }
                failure = std::max(failure, result.failure);
            }
        } else {
            failure = Failure::no_features;
        }
        return {{}, phase, failure};
    };
    if (video_tracking) {
        if (anchor || model.tried_initial_search) {
            if (model.local_misses < model.local_limit) ++model.local_misses;
            if (model.local_misses < model.local_limit) {
                return finish({{}, anchor ? Phase::wait_region : Phase::wait_initial, Failure::none}, {});
            }
        }
        if (!anchor) model.tried_initial_search = true;
    }
    if (anchor) {
        if (const auto scope = detail::region_for(*anchor)) {
            auto result = attempt(Phase::region, &*scope);
            if (result.position) return result;
            if (video_tracking && ++model.regional_misses < model.regional_limit) {
                result.phase = Phase::wait_global;
                return finish(std::move(result), {});
            }
        }
    }
    auto result = attempt(anchor ? Phase::retry : Phase::global, nullptr);
    if (result.position) return result;
    return finish(std::move(result), {});
}

const char* name(Method method) {
    switch (method) {
    case Method::sift: return "sift";
    case Method::map_template: return "template";
    default: return "auto";
    }
}

const char* name(Phase phase) {
    switch (phase) {
    case Phase::local: return "local";
    case Phase::global: return "global";
    case Phase::retry: return "retry";
    case Phase::cached: return "cached";
    case Phase::region: return "region";
    case Phase::wait_region: return "wait_region";
    case Phase::wait_global: return "wait_global";
    case Phase::wait_initial: return "wait_initial";
    default: return "frame";
    }
}

const char* name(Failure failure) {
    switch (failure) {
    case Failure::black: return "black_frame";
    case Failure::no_circle: return "no_circle";
    case Failure::no_features: return "sift_quality";
    case Failure::no_tiles: return "no_atlas_candidates";
    case Failure::coarse: return "coarse";
    case Failure::fine: return "fine";
    case Failure::ambiguous: return "ambiguous";
    case Failure::no_minimap: return "no_minimap";
    default: return "";
    }
}

}
