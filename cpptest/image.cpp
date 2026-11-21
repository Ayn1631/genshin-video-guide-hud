#include "detail.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace bgi {

cv::Mat read_image(const std::filesystem::path& file, int mode) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("cannot read image: " + file.u8string());
    }
    const auto size = stream.tellg();
    if (size <= 0 || size > std::numeric_limits<int>::max()) {
        throw std::runtime_error("invalid image file size: " + file.u8string());
    }
    std::vector<uchar> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("cannot read complete image: " + file.u8string());
    }
    auto image = cv::imdecode(bytes, mode);
    if (image.empty()) {
        throw std::runtime_error("cannot decode image: " + file.u8string());
    }
    return image;
}

namespace detail {
namespace {

bool is_standard(const cv::Mat& frame, double tolerance) {
    return std::abs(static_cast<double>(frame.cols) / frame.rows - 16.0 / 9.0) < tolerance;
}

bool overlaps(const Circle& a, const Circle& b) {
    const auto tolerance = std::max(5.f, std::min(a.radius, b.radius) * 0.16f);
    return std::abs(a.x - b.x) < tolerance && std::abs(a.y - b.y) < tolerance &&
           std::abs(a.radius - b.radius) < tolerance;
}

cv::Mat dark_pixels(const cv::Mat& image) {
    cv::Mat dark;
    cv::inRange(image, cv::Scalar::all(0), cv::Scalar::all(12), dark);
    return dark;
}

void mask_icons(const cv::Mat& source, cv::Mat& mask, double radius) {
    std::vector<cv::Mat> channels;
    cv::split(source, channels);
    cv::Mat bright, dark, spread;
    cv::max(channels[0], channels[1], bright);
    cv::max(bright, channels[2], bright);
    cv::min(channels[0], channels[1], dark);
    cv::min(dark, channels[2], dark);
    cv::subtract(bright, dark, spread);
    if (cv::countNonZero(spread) == 0) {
        return;
    }

    cv::Mat icons(mask.size(), CV_8U);
    for (int y = 0; y < mask.rows; ++y) {
        const auto* b = bright.ptr<uchar>(y);
        const auto* s = spread.ptr<uchar>(y);
        auto* dst = icons.ptr<uchar>(y);
        for (int x = 0; x < mask.cols; ++x) {
            const auto gray_icon = s[x] == 0 && b[x] >= 50 && b[x] <= 127;
            // Equivalent to brightness / (10 + min(spread, (255 - brightness)/6)) > 20.
            const auto white_icon = 3 * b[x] > 600 + 10 * std::min(6 * s[x], 255 - b[x]);
            dst[x] = gray_icon || white_icon ? 255 : 0;
        }
    }
    const auto width = 2 * std::max(1, cvRound(radius * 0.025)) + 1;
    const auto kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {width, width});
    cv::dilate(icons, icons, kernel);
    cv::morphologyEx(icons, icons, cv::MORPH_CLOSE, kernel);
    mask.setTo(0, icons);
}

}

Query prepare_query(const cv::Mat& image) {
    if (image.depth() != CV_8U || std::min(image.cols, image.rows) < 100) {
        throw std::invalid_argument("minimap must be an 8-bit image of at least 100x100");
    }
    Query result;
    if (image.channels() == 3) {
        cv::cvtColor(image, result.gray, cv::COLOR_BGR2GRAY);
    } else if (image.channels() == 4) {
        cv::cvtColor(image, result.gray, cv::COLOR_BGRA2GRAY);
    } else if (image.channels() == 1) {
        result.gray = image;
    } else {
        throw std::invalid_argument("unsupported minimap color format");
    }
    result.center = {static_cast<float>(image.cols - 1) / 2.f, static_cast<float>(image.rows - 1) / 2.f};
    const double radius = std::min(result.center.x, result.center.y);
    const cv::Point center(cvRound(result.center.x), cvRound(result.center.y));
    result.mask = cv::Mat::zeros(image.size(), CV_8U);
    cv::circle(result.mask, center, cvRound(radius * 0.88), 255, cv::FILLED);
    cv::circle(result.mask, center, std::max(12, cvRound(radius * 0.14)), 0, cv::FILLED);
    if (image.channels() >= 3) {
        mask_icons(image, result.mask, radius);
    }
    return result;
}

cv::Mat video_content(const cv::Mat& frame) {
    const auto dark = dark_pixels(frame);
    cv::Mat rows, columns;
    cv::reduce(dark, rows, 1, cv::REDUCE_SUM, CV_32S);
    cv::reduce(dark, columns, 0, cv::REDUCE_SUM, CV_32S);
    const auto row_cutoff = 255 * frame.cols * 0.98;
    const auto column_cutoff = 255 * frame.rows * 0.98;
    auto top = 0;
    auto bottom = frame.rows;
    auto left = 0;
    auto right = frame.cols;
    while (top < bottom && rows.at<int>(top) >= row_cutoff) ++top;
    while (bottom > top && rows.at<int>(bottom - 1) >= row_cutoff) --bottom;
    while (left < right && columns.at<int>(0, left) >= column_cutoff) ++left;
    while (right > left && columns.at<int>(0, right - 1) >= column_cutoff) --right;
    if (bottom - top < frame.rows * 0.55 || right - left < frame.cols * 0.55) {
        return frame;
    }
    return frame(cv::Rect(left, top, right - left, bottom - top));
}

bool black_frame(const cv::Mat& frame) {
    cv::Mat thumbnail;
    cv::resize(frame, thumbnail, {96, 54}, 0, 0, cv::INTER_AREA);
    return cv::countNonZero(dark_pixels(thumbnail)) >= thumbnail.total() * 0.995;
}

bool has_video_minimap(const cv::Mat& crop) {
    cv::Mat normalized, hsv, marker;
    cv::resize(crop, normalized, {168, 168}, 0, 0, cv::INTER_AREA);
    // An isolated cyan player marker near the center is visible even when the rim is clipped.
    cv::cvtColor(normalized(cv::Rect(54, 54, 60, 60)), hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(77, 120, 162), cv::Scalar(110, 255, 255), marker);
    cv::Mat labels, stats, centers;
    const int count = cv::connectedComponentsWithStats(marker, labels, stats, centers);
    for (int i = 1; i < count; ++i) {
        const int width = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (std::min(width, height) >= 6 && std::max(width, height) <= 34 && area >= 30 && area <= 560 &&
            std::hypot(centers.at<double>(i, 0) - 29.5, centers.at<double>(i, 1) - 29.5) <= 13) {
            return true;
        }
    }

    // Overlays can cover the marker. Only accept a rim centered on the existing crop.
    cv::Mat gray;
    cv::cvtColor(normalized, gray, cv::COLOR_BGR2GRAY);
    cv::medianBlur(gray, gray, 5);
    std::vector<cv::Vec3f> rims;
    cv::HoughCircles(gray, rims, cv::HOUGH_GRADIENT, 1, 15, 85, 20, 72, 111);
    return std::any_of(rims.begin(), rims.end(), [](const auto& rim) {
        return std::hypot(rim[0] - 83.5, rim[1] - 83.5) <= 10;
    });
}

std::vector<Circle> find_circles(const cv::Mat& frame) {
    const double scale = 640.0 / frame.cols;
    const int w = std::min(frame.cols, static_cast<int>(std::ceil(285 / scale)));
    const int h = std::min(frame.rows, static_cast<int>(std::ceil(248 / scale)));
    const int sw = cvRound(w * scale);
    const int sh = cvRound(h * scale);
    if (std::min(sw, sh) < 45) {
        return {};
    }
    cv::Mat roi, gray;
    cv::resize(frame(cv::Rect(0, 0, w, h)), roi, {sw, sh}, 0, 0, cv::INTER_AREA);
    roi = roi(cv::Rect(0, 0, std::min(285, sw), std::min(248, sh)));
    cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);
    cv::medianBlur(gray, gray, 5);
    std::vector<cv::Vec3f> circles;
    cv::HoughCircles(gray, circles, cv::HOUGH_GRADIENT, 1, 15, 90, 24, 19, 134);

    const auto visible_arc = [&](const Circle& c) {
        return std::min({c.x, c.y, roi.cols - c.x, roi.rows - c.y}) >= 0.63 * c.radius;
    };
    std::vector<std::pair<double, Circle>> ranked;
    for (std::size_t i = 0; i < std::min<std::size_t>(32, circles.size()); ++i) {
        const auto& raw = circles[i];
        const Circle c{raw[0], raw[1], raw[2]};
        if (!visible_arc(c)) {
            continue;
        }
        const int extent = std::max(3, cvRound(c.radius * 0.13f));
        const int x = cvRound(c.x);
        const int y = cvRound(c.y);
        const cv::Rect counts = cv::Rect(x - extent, y - extent, 2 * extent + 1, 2 * extent + 1) &
                                cv::Rect(0, 0, roi.cols, roi.rows);
        int cyan = 0;
        for (int yy = counts.y; yy < counts.br().y; ++yy) {
            const auto* pixels = roi.ptr<cv::Vec3b>(yy);
            for (int xx = counts.x; xx < counts.br().x; ++xx) {
                const auto& p = pixels[xx];
                cyan += p[0] - p[2] > 35 && p[1] - p[2] > 35 && p[1] > 110;
            }
        }
        const double score = std::min(0.45, static_cast<double>(cyan) / counts.area()) - i * 0.024;
        ranked.emplace_back(score, c);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Circle> result;
    for (const auto& pair : ranked) {
        if (std::none_of(result.begin(), result.end(), [&](const auto& c) { return overlaps(c, pair.second); })) {
            result.push_back(pair.second);
        }
        if (result.size() == 2) break;
    }
    // Enlarged minimaps in edited videos can extend past the top or left edge.
    // Their visible rim is often too faint for the normal circle pass.
    std::vector<cv::Vec3f> large;
    cv::HoughCircles(gray, large, cv::HOUGH_GRADIENT, 1, 12, 70, 20, 44, 134);
    if (!large.empty()) {
        const auto& rim = large.front();
        const Circle clipped{rim[0], rim[1], rim[2]};
        if (std::min(clipped.x, clipped.y) < clipped.radius && visible_arc(clipped) &&
            std::none_of(result.begin(), result.end(), [&](const auto& c) { return overlaps(c, clipped); })) {
            result.insert(result.begin(), clipped);
        }
    }
    if (is_standard(frame, 0.025)) {
        const Circle standard{56.5f, 41.5f, 35.5f, false};
        if (std::none_of(result.begin(), result.end(), [&](const auto& c) { return overlaps(c, standard); })) {
            result.push_back(standard);
        }
    }
    return result;
}

cv::Mat crop_circle(const cv::Mat& frame, const Circle& circle) {
    const auto scale = frame.cols / 640.f;
    const auto half = std::max(1, cvRound(circle.radius * scale * 0.88f));
    cv::Mat result;
    cv::getRectSubPix(frame, {2 * half + 1, 2 * half + 1}, {circle.x * scale, circle.y * scale}, result);
    if (result.cols < 100) {
        cv::resize(result, result, {212, 212}, 0, 0, cv::INTER_CUBIC);
    }
    return result;
}

cv::Mat crop_manual(const cv::Mat& frame, const Circle& circle, double inset) {
    const double inner = circle.radius - inset;
    if (!is_standard(frame, 0.02) || !std::isfinite(circle.x) || !std::isfinite(circle.y) ||
        !std::isfinite(circle.radius) || !std::isfinite(inset) || inset < 0 || inner < 44 ||
        inner > std::min({circle.x, circle.y, 639.f - circle.x, 359.f - circle.y})) {
        throw std::invalid_argument("invalid manual minimap circle or inset");
    }
    cv::Mat normalized, result;
    cv::resize(frame, normalized, {640, 360}, 0, 0, cv::INTER_AREA);
    const auto half = cvRound(inner / 0.88);
    cv::getRectSubPix(normalized, {half * 2 + 1, half * 2 + 1}, {circle.x, circle.y}, result);
    return result;
}

cv::Mat crop_game(const cv::Mat& frame) {
    if (!is_standard(frame, 0.02)) {
        throw std::invalid_argument("game screenshot must use the standard 16:9 layout");
    }
    const double scale = frame.cols / 1920.0;
    const auto diameter = cvRound(212 * scale);
    const cv::Rect region(cvRound(62 * scale), cvRound(19 * scale), diameter, diameter);
    if (diameter <= 0 || (region & cv::Rect(0, 0, frame.cols, frame.rows)) != region) {
        throw std::invalid_argument("game screenshot is too small");
    }
    auto crop = frame(region);
    if (diameter < 100) {
        cv::Mat upsampled;
        cv::resize(crop, upsampled, {212, 212}, 0, 0, cv::INTER_LINEAR);
        return upsampled;
    }
    return crop;
}

}
}
