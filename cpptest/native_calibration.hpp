#pragma once

#include "native_types.hpp"

namespace guide {

enum class CalibrationKind { video, minimap };

class CapturePreview {
public:
    CapturePreview(HINSTANCE instance, HWND parent, const cv::Mat& image, CalibrationKind kind,
                   const NormalizedRect& video, const std::optional<WindowMinimap>& minimap);
    ~CapturePreview();

    bool open() const;
    bool handle_key(MSG& message) const;
    CalibrationKind kind() const;
    const std::optional<WindowMinimap>& result() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
