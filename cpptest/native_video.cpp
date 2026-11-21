#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native_video.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <winhttp.h>
#include <combaseapi.h>
#include <wincrypt.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <locale>
#include <regex>
#include <sstream>
#include <string_view>
#include <tuple>
#include <vector>

struct BgiFrameView {
    const std::uint8_t* pixels;
    int width;
    int height;
};

extern "C" {
__declspec(dllimport) void* bgi_capture_open(std::uintptr_t hwnd);
__declspec(dllimport) int bgi_capture_read(void* capture, double x, double y, double width,
    double height, int timeout_ms, BgiFrameView* frame);
__declspec(dllimport) void bgi_capture_close(void* capture);
__declspec(dllimport) const char* bgi_capture_error();
}

namespace guide {
namespace {

VideoFrame waiting(std::wstring status) { return {{}, std::move(status)}; }

// WinRT caches activation factories. Keep their MTA alive across preview sessions
// and successive HUD reads, including when all capture workers have stopped.
void keep_mta_alive() {
    struct Lifetime {
        CO_MTA_USAGE_COOKIE cookie = nullptr;
        Lifetime() { winrt::check_hresult(CoIncrementMTAUsage(&cookie)); }
        ~Lifetime() { CoDecrementMTAUsage(cookie); }
    };
    static Lifetime lifetime;
}

bool valid_circle(const GuideSettings& settings) {
    const auto& c = settings.circle;
    const double half = c.radius - settings.inset;
    return std::isfinite(c.x) && std::isfinite(c.y) && settings.inset >= 0 && half > 0 &&
        half <= std::min({double(c.x), double(c.y), 640.0 - c.x, 360.0 - c.y});
}

NormalizedRect minimap_region(const GuideSettings& settings) {
    const auto& r = settings.video_rect;
    const auto& c = settings.circle;
    const double half = c.radius - settings.inset;
    return {r.x + r.width * (c.x - half) / 640, r.y + r.height * (c.y - half) / 360,
        r.width * 2 * half / 640, r.height * 2 * half / 360};
}

struct EdgeWindow {
    HWND hwnd;
    std::wstring title;
    long long area;
};

BOOL CALLBACK collect_edge(HWND hwnd, LPARAM param) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t class_name[128]{};
    GetClassNameW(hwnd, class_name, 128);
    if (wcscmp(class_name, L"Chrome_WidgetWin_1") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return TRUE;
    wchar_t executable[32768];
    DWORD length = ARRAYSIZE(executable);
    const bool found = QueryFullProcessImageNameW(process, 0, executable, &length) != FALSE;
    CloseHandle(process);
    if (!found) return TRUE;
    const wchar_t* last = wcsrchr(executable, L'\\');
    if (_wcsicmp(last ? last + 1 : executable, L"msedge.exe") != 0) return TRUE;
    std::wstring title(static_cast<size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    title.resize(GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size())));
    if (title.empty()) return TRUE;
    RECT r{};
    GetWindowRect(hwnd, &r);
    reinterpret_cast<std::vector<EdgeWindow>*>(param)->push_back(
        {hwnd, std::move(title), static_cast<long long>(r.right - r.left) * (r.bottom - r.top)});
    return TRUE;
}

HWND find_window(const std::wstring& pattern) {
    const bool use_default = pattern.empty() || pattern == LR"(bilibili\.com)";
    const std::wregex filter(use_default ? L"bilibili|哔哩哔哩|B站" : pattern, std::regex::icase);
    std::vector<EdgeWindow> windows;
    EnumWindows(collect_edge, reinterpret_cast<LPARAM>(&windows));
    const HWND foreground = GetForegroundWindow();
    const EdgeWindow* best = nullptr;
    const auto rank = [foreground](const EdgeWindow& w) {
        return std::make_tuple(!IsIconic(w.hwnd), w.hwnd == foreground, w.area);
    };
    HWND only_main = nullptr;
    unsigned main_count = 0;
    for (const auto& w : windows) {
        if (std::regex_search(w.title, filter) && (!best || rank(w) > rank(*best))) best = &w;
        auto title = w.title;
        title.erase(std::remove(title.begin(), title.end(), L'\u200b'), title.end());
        constexpr std::wstring_view suffix = L" - Microsoft Edge";
        if (title.size() >= suffix.size() && _wcsicmp(title.c_str() + title.size() - suffix.size(),
                suffix.data()) == 0) {
            ++main_count;
            only_main = w.hwnd;
        }
    }
    return best ? best->hwnd : (use_default && main_count == 1 ? only_main : nullptr);
}

class WindowSource {
public:
    ~WindowSource() { close(); }

    VideoFrame read(const GuideSettings& settings, const NormalizedRect& region, int timeout_ms,
                    HANDLE stop = nullptr) {
        const auto crop = bounded_video_rect(region);
        if (!crop) return waiting(L"窗口取帧区域超出画面");
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return waiting(L"取帧已停止");
        if (!IsWindow(hwnd_)) {
            close();
            hwnd_ = find_window(settings.edge_pattern);
        }
        if (!hwnd_) return waiting(L"Windows 捕获等待匹配的 Edge 攻略窗口");
        if (IsIconic(hwnd_)) {
            close();
            return waiting(L"Windows 捕获等待 Edge 退出最小化");
        }
        if (!handle_) {
            keep_mta_alive();
            handle_ = bgi_capture_open(reinterpret_cast<std::uintptr_t>(hwnd_));
        }
        BgiFrameView view{};
        int rc = -1;
        if (handle_) {
            int remaining = timeout_ms;
            do {
                if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return waiting(L"取帧已停止");
                const int slice = stop ? std::min(remaining, 200) : remaining;
                rc = bgi_capture_read(handle_, crop->x, crop->y, crop->width, crop->height, slice, &view);
                remaining -= slice;
            } while (rc == 1 && remaining > 0);
        }
        if (rc == 1) return waiting(L"Windows 捕获未收到新帧");
        if (rc != 0) {
            std::wstring message = L"Windows 捕获失败：";
            message += winrt::to_hstring(bgi_capture_error());
            close();
            return waiting(std::move(message));
        }
        cv::Mat bgr;
        cv::Mat bgra(view.height, view.width, CV_8UC4, const_cast<std::uint8_t*>(view.pixels));
        cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
        return {std::move(bgr), L"Windows 窗口捕获"};
    }

private:
    void close() {
        if (handle_) bgi_capture_close(handle_);
        handle_ = nullptr;
    }
    HWND hwnd_ = nullptr;
    void* handle_ = nullptr;
};

namespace json = winrt::Windows::Data::Json;
using Object = json::JsonObject;
using Clock = std::chrono::steady_clock;

struct VideoError {
    std::wstring message;
};

struct InternetCloser {
    void operator()(HINTERNET handle) const noexcept { WinHttpCloseHandle(handle); }
};
using Internet = std::unique_ptr<void, InternetCloser>;

void require_http(bool ok, DWORD error = 0) {
    if (!ok) {
        const auto code = error ? error : GetLastError();
        throw VideoError{L"Edge 本地连接失败（WinHTTP " + std::to_wstring(code) + L"）"};
    }
}

struct Operation {
    winrt::handle done{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    DWORD error = 0;
    WINHTTP_WEB_SOCKET_STATUS read{};
    std::array<char, 32768> buffer{};
    std::string outgoing;

    Operation() { if (!done) winrt::throw_last_error(); }
};

// Buffers live through the final callback, including cancellation in an outstanding receive.
void CALLBACK on_http(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD length) {
    auto* op = reinterpret_cast<Operation*>(context);
    if (!op) return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        delete op;
        return;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        op->error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        if (length == sizeof(WINHTTP_WEB_SOCKET_STATUS))
            op->read = *static_cast<WINHTTP_WEB_SOCKET_STATUS*>(info);
        else op->error = ERROR_INVALID_DATA;
        break;
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        break;
    default:
        return;
    }
    SetEvent(op->done.get());
}

std::pair<INTERNET_PORT, std::wstring> debug_endpoint() {
    wchar_t local[32768];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    if (n && n < ARRAYSIZE(local)) {
        std::ifstream in(std::filesystem::path(local) / L"Microsoft/Edge/User Data/DevToolsActivePort");
        std::string port, path;
        if (std::getline(in, port) && std::getline(in, path) &&
                std::regex_match(port, std::regex("[0-9]{1,5}")) &&
                std::regex_match(path, std::regex("/devtools/browser(/[a-zA-Z0-9_-]+)?"))) {
            const auto number = std::stoi(port);
            if (number > 0 && number <= 65535)
                return {static_cast<INTERNET_PORT>(number), std::wstring(path.begin(), path.end())};
        }
    }
    throw VideoError{L"请在原 Edge 的 edge://inspect/#remote-debugging 开启并允许本地调试"};
}

class BrowserConnection {
public:
    explicit BrowserConnection(HANDLE stop) : stop_(stop) {
        const auto endpoint = debug_endpoint();
        client_.reset(WinHttpOpen(L"GenshinVideoGuideHUD", WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
        require_http(bool(client_));
        const DWORD notifications = WINHTTP_CALLBACK_FLAG_HANDLES | WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS;
        require_http(WinHttpSetStatusCallback(client_.get(), on_http, notifications, 0) !=
            WINHTTP_INVALID_STATUS_CALLBACK);
        server_.reset(WinHttpConnect(client_.get(), L"127.0.0.1", endpoint.first, 0));
        require_http(bool(server_));
        Internet request(WinHttpOpenRequest(server_.get(), L"GET", endpoint.second.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
        require_http(bool(request));
        DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        require_http(WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
            &redirect_policy, sizeof(redirect_policy)) != FALSE);
        require_http(WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) != FALSE);
        auto context = std::make_unique<Operation>();
        DWORD_PTR ptr = reinterpret_cast<DWORD_PTR>(context.get());
        require_http(WinHttpSetOption(request.get(), WINHTTP_OPTION_CONTEXT_VALUE, &ptr, sizeof(ptr)) != FALSE);
        auto* handshake = context.release(); // Released by the request's HANDLE_CLOSING callback.
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        require_http(WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, ptr) != FALSE);
        wait(*handshake, deadline, L"等待原 Edge 允许本地调试连接超时");
        require_http(WinHttpReceiveResponse(request.get(), nullptr) != FALSE);
        wait(*handshake, deadline, L"等待原 Edge 允许本地调试连接超时");
        DWORD status = 0, size = sizeof(status);
        require_http(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE);
        if (status != 101) throw VideoError{L"Edge 尚未允许此应用连接原浏览器"};
        auto socket_op = std::make_unique<Operation>();
        socket_.reset(WinHttpWebSocketCompleteUpgrade(request.get(), reinterpret_cast<DWORD_PTR>(socket_op.get())));
        require_http(bool(socket_));
        op_ = socket_op.release();
    }

    Object call(const wchar_t* method, const Object& params, const winrt::hstring& session = {}) {
        const auto id = ++sequence_;
        Object request;
        request.SetNamedValue(L"id", json::JsonValue::CreateNumberValue(id));
        request.SetNamedValue(L"method", json::JsonValue::CreateStringValue(method));
        request.SetNamedValue(L"params", params);
        if (!session.empty()) request.SetNamedValue(L"sessionId", json::JsonValue::CreateStringValue(session));
        op_->outgoing = winrt::to_string(request.Stringify());
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        const DWORD sent = WinHttpWebSocketSend(socket_.get(), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
            op_->outgoing.data(), static_cast<DWORD>(op_->outgoing.size()));
        require_http(sent == NO_ERROR, sent);
        wait(*op_, deadline);
        std::string payload;
        for (;;) {
            DWORD rc = WinHttpWebSocketReceive(socket_.get(), op_->buffer.data(),
                static_cast<DWORD>(op_->buffer.size()), nullptr, nullptr);
            require_http(rc == NO_ERROR, rc);
            wait(*op_, deadline);
            const auto type = op_->read.eBufferType;
            if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE &&
                    type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                throw VideoError{L"Edge 已关闭本地连接"};
            if (payload.size() + op_->read.dwBytesTransferred > 8 * 1024 * 1024)
                throw VideoError{L"Edge 视频帧超过取帧大小限制"};
            payload.append(op_->buffer.data(), op_->read.dwBytesTransferred);
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
            const auto response = Object::Parse(winrt::to_hstring(payload));
            if (response.GetNamedNumber(L"id", 0.0) == id) {
                if (response.HasKey(L"error")) throw VideoError{L"Edge 未能执行取帧请求"};
                return response.GetNamedObject(L"result");
            }
            payload.clear(); // Ignore unrelated asynchronous DevTools notifications.
        }
    }

private:
    void wait(Operation& op, Clock::time_point deadline,
            const wchar_t* timeout_message = L"等待 Edge 返回画面超时") const {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        HANDLE handles[] = {op.done.get(), stop_};
        auto rc = WaitForMultipleObjects(stop_ ? 2 : 1, handles, FALSE,
            static_cast<DWORD>(std::max<long long>(0, remaining)));
        if (rc == WAIT_OBJECT_0 + 1) throw VideoError{L"取帧已停止"};
        if (rc != WAIT_OBJECT_0) throw VideoError{timeout_message};
        require_http(op.error == 0, op.error);
    }

    HANDLE stop_;
    Internet client_, server_, socket_;
    Operation* op_ = nullptr;
    int sequence_ = 0;
};

Object text_param(const wchar_t* name, const winrt::hstring& text) {
    Object params;
    params.SetNamedValue(name, json::JsonValue::CreateStringValue(text));
    return params;
}

json::IJsonValue evaluate(BrowserConnection& connection, const winrt::hstring& session,
        const std::wstring& expression) {
    auto params = text_param(L"expression", winrt::hstring(expression));
    params.SetNamedValue(L"returnByValue", json::JsonValue::CreateBooleanValue(true));
    const auto reply = connection.call(L"Runtime.evaluate", params, session);
    if (reply.HasKey(L"exceptionDetails")) throw VideoError{L"Edge 页面未能读取视频元素"};
    return reply.GetNamedObject(L"result").GetNamedValue(L"value");
}

// A fresh document must pass the hostname check even after a pinned tab navigates.
const wchar_t* select_video = LR"JS(
    if (!['www.bilibili.com','bilibili.com'].includes(location.hostname)) return null;
    const videos = Array.from(document.querySelectorAll('video')).filter(v =>
        v.readyState >= 2 && v.videoWidth > 0 && v.videoHeight > 0);
    const video = videos.find(v => !v.paused && !v.ended) || videos[0];
)JS";

std::wstring canvas_expression(const GuideSettings& s) {
    std::wostringstream js;
    js.imbue(std::locale::classic());
    js << L"(() => {" << select_video << LR"JS(
        if (!video) return {error:'攻略播放器尚未解码出画面'};
        const scale = video.videoWidth / 640;
        const [cx,cy,r] = [)JS" << s.circle.x << L"," << s.circle.y << L"," << (s.circle.radius - s.inset) <<
        LR"JS(].map(v => v * scale);
        const side = 2*r, x = cx-r, y = cy-r;
        if (x < 0 || y < 0 || x+side > video.videoWidth || y+side > video.videoHeight)
            return {error:'固定小地图区域超出攻略画面'};
        )JS";
    if (s.capture_mode == CaptureMode::wake) js << LR"JS(
        if (document.visibilityState !== 'visible') return {error:'唤醒模式等待攻略标签页可见'};
        const box = video.getBoundingClientRect(), style = getComputedStyle(video);
        let w = box.width, h = box.height;
        const rawW = video.videoWidth, rawH = video.videoHeight;
        if (!w || !h || style.visibility === 'hidden') return {error:'唤醒模式等待视频可见'};
        const contain = Math.min(w/rawW,h/rawH), cover = Math.max(w/rawW,h/rawH);
        if (style.objectFit !== 'fill') {
            const f = style.objectFit === 'cover' ? cover : style.objectFit === 'none' ? 1 :
                      style.objectFit === 'scale-down' ? Math.min(1,contain) : contain;
            w = rawW*f; h = rawH*f;
        }
        const pos = style.objectPosition.split(/\s+/);
        const offset = (p,gap) => p.endsWith('%') ? gap * parseFloat(p)/100 : parseFloat(p) || 0;
        const left = box.left + offset(pos[0],box.width-w) + x*w/rawW;
        const top = box.top + offset(pos[1] || '50%',box.height-h) + y*h/rawH;
        if (left < Math.max(0,box.left) || top < Math.max(0,box.top) ||
            left+side*w/rawW > Math.min(innerWidth,box.right) ||
            top+side*h/rawH > Math.min(innerHeight,box.bottom))
            return {error:'唤醒模式等待小地图进入 Edge 可视区域'};
        )JS";
    js << LR"JS(
        const key = Symbol.for('BetterGI.VideoGuide.canvas');
        const cv = globalThis[key] || (globalThis[key] = document.createElement('canvas'));
        const n = Math.max(212, Math.min(1024,Math.round(side)));
        if (cv.width !== n || cv.height !== n) cv.width = cv.height = n;
        try {
            cv.getContext('2d',{alpha:false}).drawImage(video,x,y,side,side,0,0,n,n);
            return {data:cv.toDataURL('image/png')};
        } catch (e) {
            delete globalThis[key];
            return {error:e.name === 'SecurityError' ? '视频跨域限制取帧，请选择 Windows 窗口捕获' : 'Edge 无法读取视频帧'};
        }
    })())JS";
    return js.str();
}

cv::Mat decode_png(const winrt::hstring& url) {
    std::wstring_view value(url);
    constexpr std::wstring_view prefix = L"data:image/png;base64,";
    if (value.substr(0, prefix.size()) != prefix) throw VideoError{L"Edge 返回的视频帧格式无效"};
    value.remove_prefix(prefix.size());
    DWORD size = 0;
    const auto n = static_cast<DWORD>(value.size());
    if (!n || !CryptStringToBinaryW(value.data(), n, CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr) || !size)
        throw VideoError{L"Edge 返回的视频帧编码无效"};
    std::vector<unsigned char> png(size);
    if (!CryptStringToBinaryW(value.data(), n, CRYPT_STRING_BASE64, png.data(), &size, nullptr, nullptr))
        throw VideoError{L"Edge 视频帧解码失败"};
    auto frame = cv::imdecode(png, cv::IMREAD_COLOR);
    if (frame.empty() || frame.rows != frame.cols) throw VideoError{L"Edge 未返回小地图方形画面"};
    return frame;
}

class RemoteSource {
public:
    VideoFrame read(const GuideSettings& s, HANDLE stop) {
        if (!connection_ && Clock::now() < retry_) return waiting(last_error_);
        try {
            keep_mta_alive();
            if (!connection_) connection_ = std::make_unique<BrowserConnection>(stop);
            if (session_.empty()) {
                if (pinned_.empty()) {
                    if (!select_target(s)) return waiting(L"等待符合筛选条件且已解码的 B 站攻略标签页");
                } else {
                    session_ = attach(pinned_);
                }
            }
            if (s.capture_mode == CaptureMode::wake) {
                const auto win = connection_->call(L"Browser.getWindowForTarget", text_param(L"targetId", pinned_));
                if (win.GetNamedObject(L"bounds").GetNamedString(L"windowState", L"") == L"minimized")
                    return waiting(L"唤醒模式等待 Edge 退出最小化");
            }
            if (expression_.empty()) expression_ = canvas_expression(s);
            const auto value = evaluate(*connection_, session_, expression_);
            if (value.ValueType() != json::JsonValueType::Object) return waiting(L"原攻略标签页没有 B 站视频");
            const auto result = value.GetObject();
            if (result.HasKey(L"error")) return waiting(std::wstring(result.GetNamedString(L"error")));
            return {decode_png(result.GetNamedString(L"data")),
                s.capture_mode == CaptureMode::wake ? L"Edge 唤醒取帧" : L"Edge 后台取帧"};
        } catch (const VideoError& e) {
            return disconnect(e.message);
        } catch (const winrt::hresult_error& e) {
            return disconnect(L"Edge 返回无效响应：" + std::wstring(e.message()));
        }
    }

private:
    winrt::hstring attach(const winrt::hstring& id) {
        auto params = text_param(L"targetId", id);
        params.SetNamedValue(L"flatten", json::JsonValue::CreateBooleanValue(true));
        return connection_->call(L"Target.attachToTarget", params).GetNamedString(L"sessionId");
    }

    bool select_target(const GuideSettings& s) {
        const auto filter = std::wregex(s.edge_pattern, std::regex::icase);
        static const std::wregex site(LR"(^https?://(www\.)?bilibili\.com(:[0-9]+)?(/|$))", std::regex::icase);
        const auto targets = connection_->call(L"Target.getTargets", Object{}).GetNamedArray(L"targetInfos");
        int best = -1;
        for (const auto& value : targets) {
            const auto t = value.GetObject();
            const std::wstring url(t.GetNamedString(L"url", L""));
            if (t.GetNamedString(L"type", L"") != L"page" || !std::regex_search(url, site)) continue;
            if (!s.edge_pattern.empty() &&
                !std::regex_search(std::wstring(t.GetNamedString(L"title", L"")) + L" " + url, filter)) continue;
            const auto id = t.GetNamedString(L"targetId");
            const auto session = attach(id);
            const std::wstring probe = L"(() => {" + std::wstring(select_video) +
                L"return !video ? null : (document.hasFocus()?4:0)+(document.visibilityState==='visible'?2:0)"
                L"+(!video.paused && !video.ended?1:0);})()";
            const auto score = evaluate(*connection_, session, probe);
            if (score.ValueType() == json::JsonValueType::Number && score.GetNumber() > best) {
                if (!session_.empty()) connection_->call(L"Target.detachFromTarget", text_param(L"sessionId", session_));
                best = static_cast<int>(score.GetNumber());
                pinned_ = id;
                session_ = session;
                if (best == 7) break;
            } else {
                connection_->call(L"Target.detachFromTarget", text_param(L"sessionId", session));
            }
        }
        return !session_.empty();
    }

    VideoFrame disconnect(std::wstring message) {
        connection_.reset();
        session_ = {};
        last_error_ = std::move(message);
        retry_ = Clock::now() + std::chrono::seconds(3);
        return waiting(last_error_);
    }

    std::unique_ptr<BrowserConnection> connection_;
    winrt::hstring pinned_, session_;
    std::wstring expression_, last_error_;
    Clock::time_point retry_{};
};

} // namespace

struct VideoSource::Impl {
    GuideSettings settings;
    HANDLE stop;
    WindowSource window;
    RemoteSource remote;

    Impl(const GuideSettings& options, HANDLE event) : settings(options), stop(event) {}
    VideoFrame read() {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return waiting(L"取帧已停止");
        if (!valid_circle(settings)) return waiting(L"固定小地图区域超出攻略画面");
        if (settings.capture_mode != CaptureMode::windows) return remote.read(settings, stop);
        if (!bounded_video_rect(settings.video_rect)) return waiting(L"窗口视频区域无效");
        auto frame = window.read(settings, minimap_region(settings), 350, stop);
        if (!frame.image.empty()) {
            const int size = std::max(212, std::min(frame.image.cols, frame.image.rows));
            if (frame.image.size() != cv::Size(size, size))
                cv::resize(frame.image, frame.image, {size, size}, 0, 0, cv::INTER_LINEAR);
        }
        return frame;
    }
};

VideoSource::VideoSource(const GuideSettings& settings, HANDLE stop_event)
    : impl_(std::make_unique<Impl>(settings, stop_event)) {}
VideoSource::~VideoSource() = default;

VideoFrame VideoSource::read() {
    try { return impl_->read(); }
    catch (const std::regex_error&) { return waiting(L"Edge 筛选正则表达式无效"); }
    catch (const winrt::hresult_error& e) { return waiting(std::wstring(e.message())); }
    catch (const std::exception& e) { return waiting(std::wstring(winrt::to_hstring(e.what()))); }
}

VideoFrame preview_edge(const GuideSettings& settings, int timeout_ms, HANDLE stop_event) {
    try { return WindowSource().read(settings, {0, 0, 1, 1}, timeout_ms, stop_event); }
    catch (const std::regex_error&) { return waiting(L"Edge 筛选正则表达式无效"); }
    catch (const winrt::hresult_error& e) { return waiting(std::wstring(e.message())); }
    catch (const std::exception& e) { return waiting(std::wstring(winrt::to_hstring(e.what()))); }
}

} // namespace guide
