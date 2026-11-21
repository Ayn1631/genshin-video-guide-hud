#pragma once

#include "map_locator.hpp"

#include <algorithm>
#include <array>
#include <opencv2/imgproc.hpp>

namespace bgi::detail {

struct MapScene {
    const char* name;
    int rank;
    cv::Point2d origin;
};

// Origins and feature ranks follow the corresponding BetterGI SceneBaseMap subclasses.
inline const std::array<MapScene, 7> scenes{{
    {"Teyvat", 2048, {32768, 24576}}, {"Enkanomiya", 1024, {2048, 2048}},
    {"TheChasm", 1024, {2048, 2048}}, {"SeaOfBygoneEras", 1024, {6144, 3072}},
    {"AncientSacredMountain", 1024, {2048, 2048}}, {"TempleOfSpace", 1024, {2048, 2048}},
    {"MoonCanon", 1024, {11264, 4096}},
}};

inline std::vector<std::pair<std::filesystem::path, const MapScene*>> scene_assets(const std::filesystem::path& root) {
    const auto resolved = std::filesystem::absolute(root).lexically_normal();
    const auto selected = resolved.filename().u8string();
    for (const auto& scene : scenes) {
        if (selected == scene.name && selected != "Teyvat") return {{resolved, &scene}};
    }
    std::vector<std::pair<std::filesystem::path, const MapScene*>> result{{resolved, &scenes.front()}};
    if (selected == "Teyvat") {
        for (auto it = scenes.begin() + 1; it != scenes.end(); ++it) {
            auto sibling = resolved.parent_path() / it->name;
            if (std::filesystem::is_directory(sibling)) result.emplace_back(std::move(sibling), &*it);
        }
    }
    return result;
}

struct Country {
    const char* name;
    std::vector<std::vector<cv::Point2f>> outlines;
    std::vector<cv::Rect2d> coverage;

    Country(const char* label, std::initializer_list<cv::Point2f> points,
            std::initializer_list<cv::Point2f> additional = {}) : name(label) {
        outlines.emplace_back(points);
        if (additional.size()) outlines.emplace_back(additional);
        // Approximate the whole country's extent from BetterGI waypoint coordinates, including a border margin.
        constexpr double margin = 450;
        for (const auto& outline : outlines) {
            const auto box = cv::boundingRect(outline);
            coverage.emplace_back(box.x - margin, box.y - margin, box.width + 2 * margin, box.height + 2 * margin);
        }
    }

    bool contains(cv::Point2d point) const {
        return std::any_of(coverage.begin(), coverage.end(), [&](const auto& bounds) { return bounds.contains(point); });
    }

    bool overlaps(const cv::Rect2d& box) const {
        return std::any_of(coverage.begin(), coverage.end(), [&](const auto& bounds) { return !(bounds & box).empty(); });
    }
};

inline const std::array<Country, 7>& countries() {
    static const std::array<Country, 7> areas{{
        {"Mondstadt", {{-218,4104},{-1639,2806},{-2004,1424},{-1337,824},{-489,850},{340,2570}}},
        {"Liyue", {{2612,2380},{2459,2547},{285,1657},{-1149,-415},{255,-905},{1120,-1002},{2018,-784},{2875,1484}}},
        {"Inazuma", {{-2599,-6697},{-581,-3838},{-769,-3564},{-4129,-1765},{-4590,-2588},{-4480,-4588},{-3275,-6133}}},
        {"Sumeru", {{6743,684},{5490,1431},{2187,478},{2069,114},{2237,-1686},{4799,-3195},{5710,-2862},{6815,380}}},
        {"Fontaine", {{4426,5090},{3619,4782},{2872,3504},{3085,2638},{3856,1121},{4321,1167},{5001,1651},{5199,3365},{4841,4804}}},
        {"Natlan", {{15200,1626},{13917,1524},{9182,-29},{7226,-1451},{7877,-2761},{11007,-3750},{11951,-3640},{15184,1406}}},
        {"Snezhnaya", {{8679,7024},{8819,8695},{7336,10171},{4170,10869},{1997,9351},{3099,8721},{6273,7651}},
                      {{11058,1591},{11158,2014},{10285,6340},{9523,6388},{8382,5739},{8835,2204},{9360,1423}}},
    }};
    return areas;
}

struct SearchScope {
    std::string scene;
    const Country* country = nullptr;
};

inline std::optional<SearchScope> region_for(const Position& point) {
    if (point.scene != "Teyvat") return SearchScope{point.scene};
    const Country* best = nullptr;
    double distance = -450;
    for (const auto& country : countries()) {
        for (const auto& outline : country.outlines) {
            const auto score = cv::pointPolygonTest(outline, {static_cast<float>(point.x), static_cast<float>(point.y)}, true);
            if (score > distance) {
                best = &country;
                distance = score;
            }
        }
    }
    return best ? std::optional<SearchScope>{{point.scene, best}} : std::nullopt;
}

}
