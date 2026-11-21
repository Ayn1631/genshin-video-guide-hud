#include "native_types.hpp"
#include "native_video.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <future>
#include <regex>
#include <stdexcept>

namespace {

using guide::GuideSettings;
using guide::NormalizedRect;
using Microsoft::WRL::ComPtr;

constexpr wchar_t settings_class[] = L"GenshinVideoGuideHUD.Settings";
constexpr wchar_t window_title[] = L"原神视频攻略 HUD 设置";
constexpr wchar_t preview_class[] = L"GenshinVideoGuideHUD.Preview";
constexpr int form_width = 702;
constexpr int form_height = 580;

enum Control : int {
    assets = 100, browse, game_title, edge_mode, edge_filter,
    video_x, video_y, video_w, video_h, select_video, reset_video,
    circle_x, circle_y, circle_r, inset,
    video_fps, player_fps, local_misses, region_misses, arrival,
    method, workers, opencv_threads, cache_indexes, detailed, status,
    save, stop, start
};

struct CloseHandleDeleter {
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};

using Handle = std::unique_ptr<void, CloseHandleDeleter>;

std::filesystem::path executable() {
    std::wstring result(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
    if (!size || size == result.size()) {
        throw std::runtime_error("exe path unavailable");
    }
    result.resize(size);
    return result;
}

std::wstring text_of(HWND control) {
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    const int length = GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    value.resize(length);
    const auto first = value.find_first_not_of(L" \t");
    return first == std::wstring::npos ? L"" : value.substr(first, value.find_last_not_of(L" \t") - first + 1);
}

int px(int logical, UINT dpi) {
    return MulDiv(logical, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

int find_dpi(HWND window) {
    const auto dpi = GetDpiForWindow(window);
    return dpi > 0 ? static_cast<int>(dpi) : USER_DEFAULT_SCREEN_DPI;
}

class HudProcess {
public:
    ~HudProcess() { close(); }

    bool open(const std::filesystem::path& exe) {
        close();
        const auto name = L"Local\\GenshinVideoGuideHUD.Stop." + std::to_wstring(GetCurrentProcessId());
        Handle stop_event(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
        Handle job(CreateJobObjectW(nullptr, nullptr));
        if (!stop_event || !job) {
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            return false;
        }
        auto command = L"\"" + exe.wstring() + L"\" --hud " + name;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED, nullptr, exe.parent_path().c_str(), &startup, &pi)) {
            return false;
        }
        Handle process(pi.hProcess);
        Handle thread(pi.hThread);
        if (!AssignProcessToJobObject(job.get(), process.get()) || ResumeThread(thread.get()) == DWORD(-1)) {
            TerminateProcess(process.get(), 1);
            WaitForSingleObject(process.get(), 450);
            return false;
        }
        stop_ = std::move(stop_event);
        job_ = std::move(job);
        process_ = std::move(process);
        return true;
    }

    bool exited() const {
        return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0;
    }

    void close() {
        if (process_) {
            SetEvent(stop_.get());
            WaitForSingleObject(process_.get(), 600);
            job_.reset();
            WaitForSingleObject(process_.get(), 400);
            process_.reset();
            stop_.reset();
        }
    }

private:
    Handle stop_, job_, process_;
};

class Preview {
public:
    Preview(HINSTANCE instance, HWND parent, const cv::Mat& image, NormalizedRect selection)
        : selection_(selection) {
        cv::cvtColor(image, pixels_, cv::COLOR_BGR2BGRA);
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.hInstance = instance;
        cls.lpfnWndProc = procedure;
        cls.lpszClassName = preview_class;
        cls.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassExW(&cls);
        MONITORINFO display{};
        display.cbSize = sizeof(display);
        GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &display);
        const auto& area = display.rcWork;
        const int width = std::min(1000L, area.right - area.left - 40);
        const int height = std::min(680L, area.bottom - area.top - 40);
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, preview_class, L"框选 Edge 窗口里的攻略视频",
            WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, area.left + (area.right - area.left - width) / 2,
            area.top + (area.bottom - area.top - height) / 2, width, height, parent, nullptr, instance, this);
        if (!window) {
            throw std::runtime_error("preview window unavailable");
        }
        ShowWindow(window, SW_SHOW);
    }

    ~Preview() {
        if (window_) {
            DestroyWindow(window_);
        }
    }

    bool open() const { return window_ != nullptr; }
    std::optional<NormalizedRect> result() const { return accepted_ ? std::optional(selection_) : std::nullopt; }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Preview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return self ? self->handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            button_ = CreateWindowW(L"BUTTON", L"使用选框", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(1), nullptr, nullptr);
            SendMessageW(button_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            return 0;
        case WM_SIZE: {
            RECT client{};
            GetClientRect(window_, &client);
            const int footer = px(42, find_dpi(window_));
            const int width = std::max(1L, client.right - 16);
            const int height = std::max(1L, client.bottom - footer - 16);
            const double scale = std::min(static_cast<double>(width) / pixels_.cols,
                                          static_cast<double>(height) / pixels_.rows);
            const LONG w = std::max(1L, std::lround(scale * pixels_.cols));
            const LONG h = std::max(1L, std::lround(scale * pixels_.rows));
            image_ = {(client.right - w) / 2, (height - h) / 2 + 8,
                      (client.right + w) / 2, (height - h) / 2 + 8 + h};
            MoveWindow(button_, std::max(0L, client.right - px(112, find_dpi(window_))),
                client.bottom - footer + 5, px(100, find_dpi(window_)), footer - 12, TRUE);
            InvalidateRect(window_, nullptr, TRUE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (PtInRect(&image_, point)) {
                dragging_ = true;
                anchor_ = point;
                previous_ = selection_;
                SetCapture(window_);
                select(point);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragging_) {
                select({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging_) {
                select({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                if (selection_.width < 0.01 || selection_.height < 0.01) {
                    selection_ = previous_;
                    InvalidateRect(window_, nullptr, FALSE);
                }
                dragging_ = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging_) {
                dragging_ = false;
                selection_ = previous_;
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wparam) == 1 && selection_.width >= 0.01 && selection_.height >= 0.01) {
                accepted_ = true;
                DestroyWindow(window_);
            }
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        case WM_NCDESTROY: {
            const auto window = window_;
            window_ = nullptr;
            return DefWindowProcW(window, message, wparam, lparam);
        }
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    void select(POINT point) {
        point.x = std::clamp(point.x, image_.left, image_.right);
        point.y = std::clamp(point.y, image_.top, image_.bottom);
        const auto w = static_cast<double>(image_.right - image_.left);
        const auto h = static_cast<double>(image_.bottom - image_.top);
        selection_ = {(std::min(point.x, anchor_.x) - image_.left) / w,
                      (std::min(point.y, anchor_.y) - image_.top) / h,
                      std::abs(point.x - anchor_.x) / w, std::abs(point.y - anchor_.y) / h};
        InvalidateRect(window_, &image_, FALSE);
    }

    void paint() {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window_, &paint);
        const LONG width = image_.right - image_.left, height = image_.bottom - image_.top;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = pixels_.cols;
        info.bmiHeader.biHeight = -pixels_.rows;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, image_.left, image_.top, width, height, 0, 0, pixels_.cols, pixels_.rows,
                      pixels_.data, &info, DIB_RGB_COLORS, SRCCOPY);
        const auto pen = CreatePen(PS_SOLID, 2, RGB(251, 77, 78));
        const auto previous_pen = SelectObject(dc, pen);
        const auto previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, image_.left + std::lround(width * selection_.x),
                  image_.top + std::lround(height * selection_.y),
                  image_.left + std::lround(width * (selection_.x + selection_.width)),
                  image_.top + std::lround(height * (selection_.y + selection_.height)));
        SelectObject(dc, previous_brush);
        SelectObject(dc, previous_pen);
        DeleteObject(pen);
        RECT client{};
        GetClientRect(window_, &client);
        client.left += 12;
        client.top = client.bottom - px(42, find_dpi(window_));
        client.right -= px(120, find_dpi(window_));
        SetBkMode(dc, TRANSPARENT);
        const auto previous_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, L"拖动选框，只框住视频画面。", -1, &client, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, previous_font);
        EndPaint(window_, &paint);
    }

    HWND window_{}, button_{};
    cv::Mat pixels_;
    NormalizedRect selection_, previous_;
    RECT image_{};
    POINT anchor_{};
    bool dragging_ = false, accepted_ = false;
};

struct FieldError {
    int id;
    std::wstring message;
};

class SettingsWindow {
public:
    SettingsWindow(HINSTANCE instance, std::filesystem::path exe)
        : instance_(instance), exe_(std::move(exe)), root_(exe_.parent_path()), settings_(guide::load_settings(root_)) {}

    ~SettingsWindow() {
        if (font_) {
            DeleteObject(font_);
        }
    }

    bool open(int show) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.hInstance = instance_;
        cls.lpfnWndProc = procedure;
        cls.lpszClassName = settings_class;
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        if (!RegisterClassExW(&cls)) {
            return false;
        }
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        const auto dpi = GetDpiForSystem();
        RECT size{0, 0, px(form_width, dpi), px(form_height, dpi)};
        AdjustWindowRectExForDpi(&size, style, FALSE, 0, dpi);
        HWND window = CreateWindowExW(0, settings_class, window_title, style, CW_USEDEFAULT, CW_USEDEFAULT,
            size.right - size.left, size.bottom - size.top, nullptr, nullptr, instance_, this);
        if (!window) {
            return false;
        }
        resize(find_dpi(window));
        ShowWindow(window, show);
        UpdateWindow(window);
        return true;
    }

    HWND window() const { return window_; }

private:
    struct Element { HWND window; int x, y, w, h; };

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        try {
            return self->handle(message, wparam, lparam);
        } catch (const FieldError& error) {
            SetFocus(self->item(error.id));
            self->set_status(error.message);
        } catch (const std::exception&) {
            if (message == WM_CREATE) {
                return -1;
            }
            self->set_status(L"操作失败，请检查配置目录是否可写。");
        }
        return 0;
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            controls();
            SetTimer(window_, 1, 90, nullptr);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED) {
                command(LOWORD(wparam));
            } else if (LOWORD(wparam) == edge_mode && HIWORD(wparam) == CBN_SELCHANGE) {
                update_mode();
            }
            return 0;
        case WM_TIMER:
            tick();
            return 0;
        case WM_DPICHANGED: {
            const auto& bounds = *reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window_, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                         bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
            resize(HIWORD(wparam));
            return 0;
        }
        case WM_CLOSE:
            KillTimer(window_, 1);
            if (preview_stop_) {
                SetEvent(preview_stop_.get());
            }
            hud_.close();
            preview_.reset();
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            hud_.close();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    HWND item(int id) const { return GetDlgItem(window_, id); }

    HWND add(const wchar_t* kind, const wchar_t* label, DWORD style, int id,
             int x, int y, int w, int h, DWORD extra = 0) {
        const auto control = CreateWindowExW(extra, kind, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!control) {
            throw std::runtime_error("cannot create control");
        }
        elements_.push_back({control, x, y, w, h});
        return control;
    }

    void label(const wchar_t* name, int x, int y, int width) {
        add(L"STATIC", name, SS_CENTERIMAGE, 0, x, y, width, 24);
    }

    void edit(int id, const std::wstring& value, int x, int y, int width) {
        add(L"EDIT", value.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, id, x, y, width, 25, WS_EX_CLIENTEDGE);
    }

    void field(const wchar_t* name, int id, double value, int x, int y) {
        label(name, x, y, 157);
        edit(id, guide::setting_number(value), x + 160, y, 110);
    }

    void button(const wchar_t* name, int id, int x, int y, int w, int h = 26, DWORD style = BS_PUSHBUTTON) {
        add(L"BUTTON", name, style | WS_TABSTOP, id, x, y, w, h);
    }

    void combo(int id, int selection, int x, int y, int width, std::initializer_list<const wchar_t*> entries) {
        const HWND box = add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, id, x, y, width, 148);
        for (const auto* entry : entries) {
            SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry));
        }
        SendMessageW(box, CB_SETCURSEL, selection, 0);
    }

    void controls() {
        label(L"地图资源目录", 22, 18, 110);
        edit(assets, settings_.assets.wstring(), 134, 18, 445);
        button(L"浏览…", browse, 590, 18, 90);
        label(L"游戏窗口名称", 22, 55, 110);
        edit(game_title, settings_.game_title, 134, 55, 202);
        label(L"Edge 筛选", 358, 55, 100);
        edit(edge_filter, settings_.edge_pattern, 465, 55, 215);
        label(L"攻略取帧方式", 22, 92, 110);
        combo(edge_mode, static_cast<int>(settings_.capture_mode), 134, 92, 345,
              {L"远程调试（可最小化 Edge）", L"唤醒模式（保持 Edge 还原）", L"Windows 窗口捕获（无需调试授权）"});
        button(L"框选视频…", select_video, 500, 92, 100);
        button(L"整窗", reset_video, 610, 92, 70);

        const auto rect = settings_.video_rect;
        label(L"视频区域 0–1", 22, 127, 112);
        label(L"左", 134, 127, 24);
        edit(video_x, guide::setting_number(rect.x), 157, 127, 86);
        label(L"上", 270, 127, 24);
        edit(video_y, guide::setting_number(rect.y), 293, 127, 86);
        label(L"宽", 406, 127, 24);
        edit(video_w, guide::setting_number(rect.width), 429, 127, 86);
        label(L"高", 542, 127, 24);
        edit(video_h, guide::setting_number(rect.height), 565, 127, 86);

        label(L"固定小地图（按 640 像素宽的视频坐标）", 22, 170, 385);
        label(L"圆心 X", 22, 200, 62);
        edit(circle_x, guide::setting_number(settings_.circle.x), 86, 200, 76);
        label(L"圆心 Y", 193, 200, 62);
        edit(circle_y, guide::setting_number(settings_.circle.y), 257, 200, 76);
        label(L"半径", 364, 200, 55);
        edit(circle_r, guide::setting_number(settings_.circle.radius), 420, 200, 76);
        label(L"内缩", 527, 200, 55);
        edit(inset, guide::setting_number(settings_.inset), 583, 200, 76);

        field(L"攻略取帧（帧/秒）", video_fps, settings_.video_fps, 22, 248);
        field(L"玩家取帧（帧/秒）", player_fps, settings_.player_fps, 358, 248);
        field(L"图块失败后搜区域", local_misses, settings_.local_misses, 22, 285);
        field(L"区域失败后搜全图", region_misses, settings_.region_misses, 358, 285);
        label(L"匹配方法", 22, 322, 123);
        combo(method, static_cast<int>(settings_.method), 148, 322, 171,
              {L"自动", L"SIFT", L"地图模板"});
        field(L"到达距离（地图单位）", arrival, settings_.arrival_distance, 358, 322);
        field(L"搜索并发数", workers, settings_.workers, 22, 359);
        field(L"OpenCV 线程数", opencv_threads, settings_.opencv_threads, 358, 359);
        button(L"保存地图索引", cache_indexes, 22, 403, 158, 26, BS_AUTOCHECKBOX);
        button(L"显示详细耗时与帧率", detailed, 190, 403, 220, 26, BS_AUTOCHECKBOX);
        CheckDlgButton(window_, cache_indexes, settings_.cache_indexes ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window_, detailed, settings_.detailed ? BST_CHECKED : BST_UNCHECKED);
        label(L"切回游戏后显示 HUD。关闭此配置窗口会退出 HUD 和取帧。", 22, 447, 650);
        add(L"STATIC", L"修改参数后，点「应用并启动 HUD」。", SS_LEFT, status, 22, 481, 655, 36);
        button(L"保存", save, 333, 529, 94, 30);
        button(L"停止 HUD", stop, 439, 529, 94, 30);
        button(L"应用并启动 HUD", start, 545, 529, 135, 30, BS_DEFPUSHBUTTON);
        EnableWindow(item(stop), FALSE);
        update_mode();
    }

    void resize(UINT dpi) {
        const auto font = CreateFontW(-px(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Microsoft YaHei UI");
        for (const auto& element : elements_) {
            SendMessageW(element.window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            MoveWindow(element.window, px(element.x, dpi), px(element.y, dpi),
                       px(element.w, dpi), px(element.h, dpi), TRUE);
        }
        if (font_) {
            DeleteObject(font_);
        }
        font_ = font;
    }

    void update_mode() {
        const bool windows = SendMessageW(item(edge_mode), CB_GETCURSEL, 0, 0) ==
            static_cast<int>(guide::CaptureMode::windows);
        for (const int id : {video_x, video_y, video_w, video_h, reset_video}) {
            EnableWindow(item(id), windows);
        }
        EnableWindow(item(select_video), windows && !preview_frame_.valid() && !preview_);
    }

    void set_status(const std::wstring& message) {
        SetDlgItemTextW(window_, status, message.c_str());
    }

    double number(int id, double min, double max, const wchar_t* name, bool integer = false) const {
        const auto value = guide::parse_setting_number(text_of(item(id)));
        if (!value || *value < min || *value > max || (integer && std::floor(*value) != *value)) {
            throw FieldError{id, std::wstring(name) + L"须填写 " + (integer ? L"整数 " : L"数值 ")
                + guide::setting_number(min) + L"–" + guide::setting_number(max) + L"。"};
        }
        return *value;
    }

    NormalizedRect read_rect() const {
        NormalizedRect rect{number(video_x, 0, 1, L"视频左边界"), number(video_y, 0, 1, L"视频上边界"),
                            number(video_w, 0.01, 1, L"视频宽度"), number(video_h, 0.01, 1, L"视频高度")};
        const auto bounded = guide::bounded_video_rect(rect);
        if (!bounded) {
            throw FieldError{video_w, L"视频区域超出窗口，请重新框选或调整比例。"};
        }
        return *bounded;
    }

    void write_rect(const NormalizedRect& rect) {
        const auto set = [&](int id, double value) { SetDlgItemTextW(window_, id, guide::setting_number(value).c_str()); };
        set(video_x, rect.x);
        set(video_y, rect.y);
        set(video_w, rect.width);
        set(video_h, rect.height);
    }

    GuideSettings read_form() const {
        GuideSettings settings = settings_;
        settings.assets = text_of(item(assets));
        if (!settings.assets.empty() && settings.assets.is_relative()) {
            settings.assets = (root_ / settings.assets).lexically_normal();
        }
        settings.game_title = text_of(item(game_title));
        if (settings.game_title.empty()) {
            throw FieldError{game_title, L"请填写游戏窗口名称。"};
        }
        settings.edge_pattern = read_filter();
        settings.capture_mode = static_cast<guide::CaptureMode>(SendMessageW(item(edge_mode), CB_GETCURSEL, 0, 0));
        if (settings.capture_mode == guide::CaptureMode::windows) {
            settings.video_rect = read_rect();
        }
        settings.circle.x = static_cast<float>(number(circle_x, 0, 639, L"圆心 X"));
        settings.circle.y = static_cast<float>(number(circle_y, 0, 359, L"圆心 Y"));
        settings.circle.radius = static_cast<float>(number(circle_r, 10, 320, L"半径"));
        settings.inset = number(inset, 0, settings.circle.radius - 2, L"内缩");
        const double half = settings.circle.radius - settings.inset;
        const double x = settings.circle.x, y = settings.circle.y;
        if (half > std::min({x, y, 640 - x, 360 - y})) {
            throw FieldError{circle_x, L"内缩后的小地图范围须落在 640 × 360 视频坐标内。"};
        }
        settings.video_fps = static_cast<int>(number(video_fps, 1, 60, L"攻略帧率", true));
        settings.player_fps = static_cast<int>(number(player_fps, 1, 60, L"玩家帧率", true));
        settings.local_misses = static_cast<int>(number(local_misses, 1, 100, L"图块失败次数", true));
        settings.region_misses = static_cast<int>(number(region_misses, 1, 100, L"区域失败次数", true));
        settings.arrival_distance = number(arrival, 0, 1000, L"到达距离");
        settings.workers = static_cast<int>(number(workers, 1, 32, L"搜索并发数", true));
        settings.opencv_threads = static_cast<int>(number(opencv_threads, 1, 32, L"OpenCV 线程数", true));
        settings.method = static_cast<bgi::Method>(SendMessageW(item(method), CB_GETCURSEL, 0, 0));
        settings.cache_indexes = IsDlgButtonChecked(window_, cache_indexes) == BST_CHECKED;
        settings.detailed = IsDlgButtonChecked(window_, detailed) == BST_CHECKED;
        return settings;
    }

    std::wstring read_filter() const {
        const auto filter = text_of(item(edge_filter));
        try {
            const std::wregex check(filter);
        } catch (const std::regex_error&) {
            throw FieldError{edge_filter, L"Edge 筛选正则表达式无效。"};
        }
        return filter;
    }

    void choose_assets() {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            set_status(L"无法打开目录选择窗口。");
            return;
        }
        dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        dialog->SetTitle(L"选择 BetterGI 的 Assets/Map/Teyvat 目录");
        ComPtr<IShellItem> result;
        PWSTR path{};
        if (SUCCEEDED(dialog->Show(window_)) && SUCCEEDED(dialog->GetResult(&result))
            && SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            SetDlgItemTextW(window_, assets, path);
            CoTaskMemFree(path);
        }
    }

    void command(int id) {
        switch (id) {
        case browse:
            choose_assets();
            break;
        case reset_video:
            write_rect({});
            break;
        case select_video: {
            GuideSettings settings = settings_;
            settings.edge_pattern = read_filter();
            preview_stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!preview_stop_) {
                set_status(L"无法启动窗口预览。");
                break;
            }
            set_status(L"正在获取 Edge 窗口预览，请保持窗口还原。");
            preview_frame_ = std::async(std::launch::async, [settings = std::move(settings), event = preview_stop_.get()] {
                return guide::preview_edge(settings, 1400, event);
            });
            update_mode();
            break;
        }
        case stop:
            hud_.close();
            EnableWindow(item(stop), FALSE);
            set_status(L"HUD 已停止。");
            break;
        case start:
        case save: {
            auto settings = read_form();
            if (id == start && !guide::valid_assets(settings.assets)) {
                throw FieldError{assets, L"请先选择 BetterGI 的 Assets/Map/Teyvat 目录。"};
            }
            guide::save_settings(root_, settings);
            settings_ = std::move(settings);
            if (id == start) {
                const bool running = hud_.open(exe_);
                EnableWindow(item(stop), running);
                set_status(running ? L"HUD 已启动，切回原神后显示。关闭此窗口即可退出。"
                                   : L"无法启动 HUD 进程，请关闭本程序后重试。");
            } else {
                set_status(L"配置已保存。点「应用并启动 HUD」即可应用到 HUD。");
            }
            break;
        }
        }
    }

    void tick() {
        if (hud_.exited()) {
            hud_.close();
            EnableWindow(item(stop), FALSE);
            set_status(L"HUD 已退出，请检查运行库或地图目录后重试。");
        }
        if (preview_ && !preview_->open()) {
            if (const auto rect = preview_->result()) {
                write_rect(*rect);
                set_status(L"已选定视频区域。");
            }
            preview_.reset();
            update_mode();
        }
        if (preview_frame_.valid() &&
            preview_frame_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto frame = preview_frame_.get();
            preview_stop_.reset();
            if (frame.image.empty()) {
                set_status(frame.status);
            } else {
                NormalizedRect rect;
                try {
                    rect = read_rect();
                } catch (const FieldError&) {}
                preview_ = std::make_unique<Preview>(instance_, window_, frame.image, rect);
                set_status(L"请在预览中框住攻略视频。");
            }
            update_mode();
        }
    }

    HINSTANCE instance_;
    std::filesystem::path exe_, root_;
    GuideSettings settings_;
    HWND window_{};
    HFONT font_{};
    std::vector<Element> elements_;
    HudProcess hud_;
    Handle preview_stop_;
    std::future<guide::VideoFrame> preview_frame_;
    std::unique_ptr<Preview> preview_;
};

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool internal = false;
    try {
        const auto exe = executable();
        int count = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        internal = args && count == 3 && std::wstring_view(args[1]) == L"--hud";
        if (internal) {
            Handle event(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, args[2]));
            LocalFree(args);
            if (!event) {
                return 1;
            }
            const auto settings = guide::load_settings(exe.parent_path());
            return guide::valid_assets(settings.assets)
                ? guide::run_hud_worker(instance, settings, exe.parent_path(), event.get()) : 1;
        }
        LocalFree(args);
        Handle singleton(CreateMutexW(nullptr, FALSE, L"Local\\GenshinVideoGuideHUD.Settings.v1"));
        if (!singleton) {
            throw std::runtime_error("settings instance unavailable");
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (const auto window = FindWindowW(settings_class, nullptr)) {
                ShowWindow(window, SW_RESTORE);
                SetForegroundWindow(window);
            }
            return 0;
        }
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        int result = 1;
        {
            SettingsWindow settings(instance, exe);
            if (settings.open(show)) {
                MSG message{};
                while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                    if (!IsDialogMessageW(settings.window(), &message)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            } else {
                MessageBoxW(nullptr, L"无法打开配置窗口。", window_title, MB_ICONERROR);
            }
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        return result == 0 ? 0 : 1;
    } catch (const std::exception&) {
        if (!internal) {
            MessageBoxW(nullptr, L"无法启动配置窗口。", window_title, MB_ICONERROR);
        }
        return 1;
    }
}
