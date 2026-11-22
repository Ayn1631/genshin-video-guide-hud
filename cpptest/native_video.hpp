#pragma once

#include "native_types.hpp"
#include <windows.h>
#include <memory>
#include <string>
#include <opencv2/core.hpp>

namespace guide {

struct VideoFrame {
    cv::Mat image;
    std::wstring status;
};

class VideoSource {
public:
    VideoSource(const GuideSettings& settings, HANDLE stop_event);
    ~VideoSource();
    VideoFrame read();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::vector<CaptureWindow> available_capture_windows();
VideoFrame preview_capture_window(const GuideSettings& settings, int timeout_ms = 1400, HANDLE stop_event = nullptr);

}
