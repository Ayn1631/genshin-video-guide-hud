#pragma once

#include <windows.h>

#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace guide {

inline void write_hud_startup_log(const std::filesystem::path& root, std::wstring_view message,
                                  bool reset = false) noexcept {
    try {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t stamp[32]{};
        std::swprintf(stamp, 32, L"[%02u:%02u:%02u.%03u] ", static_cast<unsigned>(time.wHour),
                      static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
                      static_cast<unsigned>(time.wMilliseconds));
        std::wstring line(stamp);
        line.append(message).append(L"\r\n");
        const auto length = static_cast<int>(line.size());
        const int size = WideCharToMultiByte(CP_UTF8, 0, line.data(), length, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return;
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, line.data(), length, bytes.data(), size, nullptr, nullptr) != size) {
            return;
        }

        const auto path = root / L"hud-startup.log";
        const auto file = CreateFileW(path.c_str(), reset ? GENERIC_WRITE : FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            reset ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(file);
    } catch (...) {
        // Diagnostics must not prevent the HUD from starting.
    }
}

inline std::wstring hud_runtime_error(const std::exception& error) {
    const char* message = error.what();
    const int size = MultiByteToWideChar(CP_ACP, 0, message, -1, nullptr, 0);
    std::wstring text;
    if (size > 0) {
        text.resize(static_cast<std::size_t>(size));
        if (MultiByteToWideChar(CP_ACP, 0, message, -1, text.data(), size) == size) {
            text.pop_back();
        } else {
            text.clear();
        }
    }
    if (const auto system = dynamic_cast<const std::system_error*>(&error)) {
        text += L" (错误码 " + std::to_wstring(system->code().value()) + L")";
    }
    return text;
}

}
