#include "native_types.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace guide {
namespace {

constexpr wchar_t filename[] = L"guide-settings.ini";
constexpr wchar_t section[] = L"Guide";

std::wstring read_value(const std::filesystem::path& file, const wchar_t* key,
                        const std::wstring& fallback) {
    std::wstring result(32768, L'\0');
    const DWORD size = GetPrivateProfileStringW(section, key, fallback.c_str(),
        result.data(), static_cast<DWORD>(result.size()), file.c_str());
    result.resize(size);
    return result;
}

std::filesystem::path find_assets(const std::filesystem::path& root) {
    for (auto directory = root; !directory.empty(); directory = directory.parent_path()) {
        const auto candidate = directory / L"Assets" / L"Map" / L"Teyvat";
        if (valid_assets(candidate)) {
            return candidate;
        }
        if (directory == directory.root_path()) {
            break;
        }
    }
    return {};
}

}

bool valid_assets(const std::filesystem::path& directory) {
    if (directory.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(directory / L"Teyvat_0_2048_SIFT.kp.bin", error)
        || std::filesystem::is_regular_file(directory / L"template_match_studio_mapback.json", error);
}

std::wstring setting_number(double value) {
    std::wostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(9) << value;
    return output.str();
}

std::optional<double> parse_setting_number(const std::wstring& value) {
    std::wistringstream input(value);
    input.imbue(std::locale::classic());
    double number = 0;
    if (!(input >> number) || !std::isfinite(number)) {
        return std::nullopt;
    }
    input >> std::ws;
    return input.eof() ? std::optional<double>(number) : std::nullopt;
}

GuideSettings load_settings(const std::filesystem::path& root) {
    GuideSettings settings;
    const auto file = root / filename;
    const auto number = [&](const wchar_t* key, double fallback, double minimum, double maximum) {
        const auto value = parse_setting_number(read_value(file, key, L""));
        return value && *value >= minimum && *value <= maximum ? *value : fallback;
    };
    const auto integer = [&](const wchar_t* key, int fallback, int minimum, int maximum) {
        const double value = number(key, fallback, minimum, maximum);
        return value == std::floor(value) ? static_cast<int>(value) : fallback;
    };
    settings.assets = read_value(file, L"Assets", L"");
    if (settings.assets.empty()) {
        settings.assets = find_assets(root);
    } else if (settings.assets.is_relative()) {
        settings.assets = root / settings.assets;
    }
    settings.game_title = read_value(file, L"GameTitle", settings.game_title);
    settings.edge_pattern = read_value(file, L"EdgePattern", settings.edge_pattern);
    settings.capture_mode = static_cast<CaptureMode>(integer(L"CaptureMode", 0, 0, 2));
    settings.video_rect = {
        number(L"VideoX", 0, 0, 1), number(L"VideoY", 0, 0, 1),
        number(L"VideoWidth", 1, 0.01, 1), number(L"VideoHeight", 1, 0.01, 1)};
    settings.video_rect = bounded_video_rect(settings.video_rect).value_or(NormalizedRect{});
    settings.circle.x = static_cast<float>(number(L"CircleX", settings.circle.x, 0, 639));
    settings.circle.y = static_cast<float>(number(L"CircleY", settings.circle.y, 0, 359));
    settings.circle.radius = static_cast<float>(number(L"CircleRadius", settings.circle.radius, 10, 320));
    settings.inset = number(L"Inset", std::min(settings.inset, settings.circle.radius - 2.0),
                            0, settings.circle.radius - 2);
    settings.video_fps = integer(L"VideoFps", settings.video_fps, 1, 60);
    settings.player_fps = integer(L"PlayerFps", settings.player_fps, 1, 60);
    settings.local_misses = integer(L"LocalMisses", settings.local_misses, 1, 100);
    settings.region_misses = integer(L"RegionMisses", settings.region_misses, 1, 100);
    settings.arrival_distance = number(L"ArrivalDistance", settings.arrival_distance, 0, 1000);
    settings.workers = integer(L"Workers", settings.workers, 1, 32);
    settings.opencv_threads = integer(L"OpenCvThreads", settings.opencv_threads, 1, 32);
    settings.cache_indexes = integer(L"CacheIndexes", 1, 0, 1) == 1;
    settings.detailed = integer(L"Detailed", 0, 0, 1) == 1;
    settings.method = static_cast<bgi::Method>(integer(L"Method", 0, 0, 2));
    return settings;
}

void save_settings(const std::filesystem::path& root, const GuideSettings& settings) {
    const auto file = root / filename;
    const auto temp = root / (L"guide-settings-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    std::wstring content = L"\xFEFF[Guide]\r\n";
    const auto write = [&](const wchar_t* key, const std::wstring& value) {
        content.append(key).append(L"=").append(value).append(L"\r\n");
    };
    const auto number = [&](const wchar_t* key, double value) {
        write(key, setting_number(value));
    };
    write(L"Assets", settings.assets.wstring());
    write(L"GameTitle", settings.game_title);
    write(L"EdgePattern", settings.edge_pattern);
    number(L"CaptureMode", static_cast<int>(settings.capture_mode));
    number(L"VideoX", settings.video_rect.x);
    number(L"VideoY", settings.video_rect.y);
    number(L"VideoWidth", settings.video_rect.width);
    number(L"VideoHeight", settings.video_rect.height);
    number(L"CircleX", settings.circle.x);
    number(L"CircleY", settings.circle.y);
    number(L"CircleRadius", settings.circle.radius);
    number(L"Inset", settings.inset);
    number(L"VideoFps", settings.video_fps);
    number(L"PlayerFps", settings.player_fps);
    number(L"LocalMisses", settings.local_misses);
    number(L"RegionMisses", settings.region_misses);
    number(L"ArrivalDistance", settings.arrival_distance);
    number(L"Workers", settings.workers);
    number(L"OpenCvThreads", settings.opencv_threads);
    number(L"CacheIndexes", settings.cache_indexes);
    number(L"Detailed", settings.detailed);
    number(L"Method", static_cast<int>(settings.method));
    static_assert(sizeof(wchar_t) == 2, "Windows INI storage uses UTF-16LE");
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size() * sizeof(wchar_t)));
    output.close();
    if (!output) {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("settings write failed");
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temp.c_str());
        throw std::system_error(static_cast<int>(error), std::system_category());
    }
}

}
