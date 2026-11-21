#include <windows.h>
#include <d3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace capture = winrt::Windows::Graphics::Capture;
namespace dx = winrt::Windows::Graphics::DirectX;
using winrt::Windows::Graphics::SizeInt32;

constexpr auto pixel_format = dx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int buffer_count = 2;
using Region = std::array<double, 4>;

struct FrameView {
    const std::uint8_t* pixels;
    int width;
    int height;
};

struct Apartment {
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};

std::string exception_message() {
    try {
        throw;
    } catch (winrt::hresult_error const& error) {
        return winrt::to_string(error.message());
    } catch (std::exception const& error) {
        return error.what();
    } catch (...) {
        return "Unknown Windows Graphics Capture error";
    }
}

class FrameReader {
public:
    FrameReader() {
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device_.put(), nullptr, context_.put()));
        winrt::com_ptr<::IInspectable> bridge;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            device_.as<IDXGIDevice>().get(), bridge.put()));
        capture_device = bridge.as<dx::Direct3D11::IDirect3DDevice>();
    }

    void request(Region const& region) {
        std::lock_guard lock(mutex_);
        region_ = region;
        error_.clear();
        pending_ = true;
    }

    int wait(int timeout_ms, FrameView& frame, std::string& error) {
        std::unique_lock lock(mutex_);
        if (!ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !pending_; })) {
            pending_ = false;
            return 1;
        }
        if (!error_.empty()) {
            error = error_;
            return -1;
        }
        frame = {pixels_.data(), width_, height_};
        return 0;
    }

    void on_frame(capture::Direct3D11CaptureFramePool const& pool) noexcept {
        std::lock_guard lock(mutex_);
        try {
            auto frame = pool.TryGetNextFrame();
            if (!frame) {
                return;
            }
            const auto size = frame.ContentSize();
            if (size.Width <= 0 || size.Height <= 0) {
                frame.Close();
                return;
            }
            if (size.Width != pool_size.Width || size.Height != pool_size.Height) {
                frame.Close();
                pool.Recreate(capture_device, pixel_format, buffer_count, size);
                pool_size = size;
                return;
            }
            if (pending_) {
                copy_region(frame, size);
            }
            frame.Close();
        } catch (...) {
            if (pending_) {
                error_ = exception_message();
            }
        }
        if (pending_) {
            pending_ = false;
            ready_.notify_one();
        }
    }

    dx::Direct3D11::IDirect3DDevice capture_device{nullptr};
    SizeInt32 pool_size{};

private:
    void copy_region(capture::Direct3D11CaptureFrame const& frame, SizeInt32 size) {
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> source;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), source.put_void()));

        D3D11_BOX box{};
        box.left = static_cast<UINT>(std::floor(region_[0] * size.Width));
        box.top = static_cast<UINT>(std::floor(region_[1] * size.Height));
        box.right = std::min(static_cast<UINT>(size.Width),
            static_cast<UINT>(std::ceil((region_[0] + region_[2]) * size.Width)));
        box.bottom = std::min(static_cast<UINT>(size.Height),
            static_cast<UINT>(std::ceil((region_[1] + region_[3]) * size.Height)));
        box.back = 1;
        const auto width = box.right - box.left;
        const auto height = box.bottom - box.top;
        if (!width || !height) {
            throw std::runtime_error("The capture region is empty");
        }

        if (!staging_ || width_ != static_cast<int>(width) || height_ != static_cast<int>(height)) {
            D3D11_TEXTURE2D_DESC descriptor{};
            descriptor.Width = width;
            descriptor.Height = height;
            descriptor.MipLevels = 1;
            descriptor.ArraySize = 1;
            descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            descriptor.SampleDesc.Count = 1;
            descriptor.Usage = D3D11_USAGE_STAGING;
            descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            winrt::com_ptr<ID3D11Texture2D> staging;
            winrt::check_hresult(device_->CreateTexture2D(&descriptor, nullptr, staging.put()));
            pixels_.resize(static_cast<std::size_t>(width) * height * 4);
            staging_ = std::move(staging);
            width_ = static_cast<int>(width);
            height_ = static_cast<int>(height);
        }

        context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, source.get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE map{};
        winrt::check_hresult(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &map));
        const auto* source_pixels = static_cast<const std::uint8_t*>(map.pData);
        const auto row_bytes = static_cast<std::size_t>(width) * 4;
        for (UINT row = 0; row < height; ++row) {
            std::memcpy(pixels_.data() + row * row_bytes,
                source_pixels + static_cast<std::size_t>(row) * map.RowPitch, row_bytes);
        }
        context_->Unmap(staging_.get(), 0);
    }

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11Texture2D> staging_;
    std::vector<std::uint8_t> pixels_;
    int width_ = 0;
    int height_ = 0;
    std::mutex mutex_;
    std::condition_variable ready_;
    Region region_{};
    bool pending_ = false;
    std::string error_;
};

class WindowCapture {
public:
    explicit WindowCapture(HWND hwnd) {
        if (!capture::GraphicsCaptureSession::IsSupported()) {
            throw std::runtime_error("Windows Graphics Capture is unavailable");
        }
        auto factory = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        capture::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(factory->CreateForWindow(hwnd,
            winrt::guid_of<capture::IGraphicsCaptureItem>(), winrt::put_abi(item)));
        reader_ = std::make_shared<FrameReader>();
        reader_->pool_size = item.Size();
        pool_ = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            reader_->capture_device, pixel_format, buffer_count, reader_->pool_size);
        session_ = pool_.CreateCaptureSession(item);
        try {
            session_.IsCursorCaptureEnabled(false);
        } catch (winrt::hresult_no_interface const&) {
            // Older Windows builds include the system cursor.
        }
        arrived_ = pool_.FrameArrived(winrt::auto_revoke,
            [reader = reader_](auto const& sender, auto const&) { reader->on_frame(sender); });
    }

    ~WindowCapture() {
        arrived_.revoke();
        try { session_.Close(); } catch (...) {}
        try { pool_.Close(); } catch (...) {}
    }

    int read(Region const& region, int timeout_ms, FrameView& frame, std::string& error) {
        reader_->request(region);
        if (!started_) {
            session_.StartCapture();
            started_ = true;
        }
        return reader_->wait(timeout_ms, frame, error);
    }

private:
    Apartment apartment_;
    std::shared_ptr<FrameReader> reader_;
    capture::Direct3D11CaptureFramePool pool_{nullptr};
    capture::GraphicsCaptureSession session_{nullptr};
    capture::Direct3D11CaptureFramePool::FrameArrived_revoker arrived_;
    bool started_ = false;
};

thread_local std::string last_error;
}

extern "C" {
__declspec(dllexport) void* bgi_capture_open(std::uintptr_t hwnd) {
    try {
        return new WindowCapture(reinterpret_cast<HWND>(hwnd));
    } catch (...) {
        last_error = exception_message();
        return nullptr;
    }
}

__declspec(dllexport) int bgi_capture_read(void* capture, double x, double y, double width, double height,
                                         int timeout_ms, FrameView* frame) {
    try {
        if (!capture || !frame || timeout_ms < 0 || !(x >= 0 && y >= 0 && width > 0 && height > 0 &&
            x + width <= 1.000000001 && y + height <= 1.000000001)) {
            throw std::invalid_argument("Invalid Windows capture region or timeout");
        }
        return static_cast<WindowCapture*>(capture)->read({x, y, width, height}, timeout_ms, *frame, last_error);
    } catch (...) {
        last_error = exception_message();
        return -1;
    }
}

__declspec(dllexport) void bgi_capture_close(void* capture) {
    delete static_cast<WindowCapture*>(capture);
}

__declspec(dllexport) const char* bgi_capture_error() {
    return last_error.c_str();
}
}
