#pragma once

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace guide {

struct DiagnosticSection {
    std::wstring title;
    std::wstring text;

    bool operator==(const DiagnosticSection& other) const noexcept {
        return title == other.title && text == other.text;
    }
};

struct HudView {
    std::wstring title;
    std::wstring detail;
    std::optional<double> offset;
    std::optional<double> distance;
    std::optional<POINT> marker;  // Absolute screen pixels.
    std::optional<double> marker_heading;  // Degrees clockwise from up.
    std::vector<DiagnosticSection> diagnostics;
};

class Hud {
public:
    explicit Hud(HINSTANCE instance);
    ~Hud();

    std::vector<HWND> windows() const;
    void show(const RECT& game, const HudView& view, bool restore_z_order = false);
    void hide() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace guide
