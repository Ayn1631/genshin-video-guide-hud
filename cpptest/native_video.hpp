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

VideoFrame preview_edge(const GuideSettings& settings, int timeout_ms = 1400, HANDLE stop_event = nullptr);

}
