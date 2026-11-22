#include "native_types.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <limits>
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

std::uint64_t read_unsigned(const std::filesystem::path& file, const wchar_t* key, std::uint64_t maximum) {
    const auto text = read_value(file, key, L"");
    if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos) {
        return 0;
    }
    try {
        const auto value = std::stoull(text);
        return value <= maximum ? value : 0;
    } catch (const std::out_of_range&) {
        return 0;
    }
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

std::optional<std::uint16_t> parse_edge_debug_port(std::wstring_view value) {
    if (value.empty() || value.size() > 5) {
        return std::nullopt;
    }
    unsigned port = 0;
    for (const wchar_t digit : value) {
        if (digit < L'0' || digit > L'9') {
            return std::nullopt;
        }
        port = port * 10 + static_cast<unsigned>(digit - L'0');
    }
    if (port == 0 || port > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(port);
}

std::filesystem::path expand_edge_debug_directory(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const auto input = value.size() >= 2 && value.front() == L'"' && value.back() == L'"'
        ? value.substr(1, value.size() - 2) : value;
    const DWORD required = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (required == 0 || required > 32768) {
        return {};
    }
    std::wstring expanded(required, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(input.c_str(), expanded.data(), required);
    if (written == 0 || written > required) {
        return {};
    }
    expanded.resize(written - 1);
    const std::filesystem::path directory(expanded);
    return directory.is_absolute() ? directory : std::filesystem::path{};
}

GuideSettings default_settings(const std::filesystem::path& root) {
    GuideSettings settings;
    settings.assets = find_assets(root);
    return settings;
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
    settings.edge_endpoint = static_cast<EdgeEndpointMode>(integer(L"EdgeEndpointMode", 0, 0, 2));
    settings.edge_debug_directory = read_value(file, L"EdgeDebugDirectory", L"");
    settings.edge_debug_port = parse_edge_debug_port(read_value(file, L"EdgeDebugPort", L"")).value_or(0);
    CaptureWindow target;
    target.handle = static_cast<std::uintptr_t>(read_unsigned(file, L"CaptureWindowHandle",
        std::numeric_limits<std::uintptr_t>::max()));
    target.process_id = static_cast<std::uint32_t>(read_unsigned(file, L"CaptureWindowProcess",
        std::numeric_limits<std::uint32_t>::max()));
    target.executable = read_value(file, L"CaptureWindowExecutable", L"");
    target.window_class = read_value(file, L"CaptureWindowClass", L"");
    target.title = read_value(file, L"CaptureWindowTitle", L"");
    if (!target.executable.empty() && !target.window_class.empty() && !target.title.empty()) {
        settings.capture_window = std::move(target);
    }
    if (integer(L"UseWindowMinimap", 0, 0, 1)) {
        const auto bounds = bounded_video_rect({
            number(L"WindowMinimapX", 0, 0, 1), number(L"WindowMinimapY", 0, 0, 1),
            number(L"WindowMinimapWidth", 0, 0, 1), number(L"WindowMinimapHeight", 0, 0, 1)});
        if (bounds) {
            settings.window_minimap = WindowMinimap{*bounds, number(L"WindowMinimapInset", 0, 0, 0.99)};
        }
    }
    settings.video_rect = {
        number(L"VideoX", 0, 0, 1), number(L"VideoY", 0, 0, 1),
        number(L"VideoWidth", 1, 0.01, 1), number(L"VideoHeight", 1, 0.01, 1)};
    settings.video_rect = bounded_video_rect(settings.video_rect).value_or(NormalizedRect{});
    settings.circle.x = number(L"CircleX", settings.circle.x, 0, 639);
    settings.circle.y = number(L"CircleY", settings.circle.y, 0, 359);
    settings.circle.radius = number(L"CircleRadius", settings.circle.radius, 1, 320);
    settings.inset = number(L"Inset", std::min(settings.inset, settings.circle.radius - 1),
                            0, settings.circle.radius - 1);
    settings.video_fps = number(L"VideoFps", settings.video_fps, min_capture_fps, max_capture_fps);
    settings.player_fps = number(L"PlayerFps", settings.player_fps, min_capture_fps, max_capture_fps);
    settings.local_misses = integer(L"LocalMisses", settings.local_misses, 1, max_retry_count);
    settings.region_misses = integer(L"RegionMisses", settings.region_misses, 1, max_retry_count);
    settings.arrival_distance = number(L"ArrivalDistance", settings.arrival_distance, 0, max_arrival_distance);
    settings.workers = integer(L"Workers", settings.workers, 1, 32);
    settings.opencv_threads = integer(L"OpenCvThreads", settings.opencv_threads, 1, max_opencv_threads);
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
    number(L"EdgeEndpointMode", static_cast<int>(settings.edge_endpoint));
    write(L"EdgeDebugDirectory", settings.edge_debug_directory);
    number(L"EdgeDebugPort", settings.edge_debug_port);
    if (const auto& target = settings.capture_window) {
        write(L"CaptureWindowHandle", std::to_wstring(target->handle));
        write(L"CaptureWindowProcess", std::to_wstring(target->process_id));
        write(L"CaptureWindowExecutable", target->executable);
        write(L"CaptureWindowClass", target->window_class);
        write(L"CaptureWindowTitle", target->title);
    }
    if (const auto& map = settings.window_minimap) {
        number(L"UseWindowMinimap", 1);
        number(L"WindowMinimapX", map->bounds.x);
        number(L"WindowMinimapY", map->bounds.y);
        number(L"WindowMinimapWidth", map->bounds.width);
        number(L"WindowMinimapHeight", map->bounds.height);
        number(L"WindowMinimapInset", map->inset_fraction);
    }
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
