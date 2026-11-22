#include "native_types.hpp"
#include "native_video.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <regex>
#include <stdexcept>
#include <string_view>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using guide::GuideSettings;
using guide::NormalizedRect;
using Microsoft::WRL::ComPtr;

constexpr wchar_t settings_class[] = L"GenshinVideoGuideHUD.Settings";
constexpr wchar_t window_title[] = L"原神视频攻略 HUD 设置";
constexpr wchar_t preview_class[] = L"GenshinVideoGuideHUD.Preview";
constexpr int form_width = 768;
constexpr int form_height = 600;
constexpr std::array<const wchar_t*, 3> tab_names{L"重试与帧率", L"Edge 和小地图", L"高级"};

enum Control : int {
    assets = 100, browse, game_title, edge_mode, edge_filter,
    video_x, video_y, video_w, video_h, select_video, reset_video,
    circle_x, circle_y, circle_r, inset,
    video_fps, player_fps, local_misses, region_misses, arrival,
    method, workers, opencv_threads, cache_indexes, detailed, status,
    save, stop, start, settings_tabs, restore, edge_help, filter_label, hud_rate, hud_rate_caption,
    video_rect_status, hud_rate_formula
};

enum class Page { tracking, edge, advanced, shared };
enum class Font { body, note, section, title, value, count };
enum class ControlKind { plain, edit, combo, button };
enum class Status { info, running, error };

struct Layout {
    int x, y, width, height;

    RECT scaled(UINT dpi) const {
        return {MulDiv(x, dpi, 96), MulDiv(y, dpi, 96),
                MulDiv(x + width, dpi, 96), MulDiv(y + height, dpi, 96)};
    }
};

struct Palette {
    COLORREF canvas = RGB(241, 246, 249);
    COLORREF surface = RGB(255, 255, 255);
    COLORREF well = RGB(232, 244, 244);
    COLORREF border = RGB(211, 223, 230);
    COLORREF input_border = RGB(131, 151, 164);
    COLORREF ink = RGB(23, 42, 55);
    COLORREF muted = RGB(78, 99, 113);
    COLORREF accent = RGB(14, 105, 108);
    COLORREF accent_hover = RGB(12, 87, 91);
    COLORREF on_accent = RGB(255, 255, 255);
    COLORREF error = RGB(155, 48, 38);

    static Palette system() {
        Palette colors;
        HIGHCONTRASTW contrast{sizeof(contrast), 0, nullptr};
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
            && (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
            colors.canvas = colors.surface = colors.well = GetSysColor(COLOR_WINDOW);
            colors.border = colors.input_border = colors.ink = colors.muted = colors.error = GetSysColor(COLOR_WINDOWTEXT);
            colors.accent = colors.accent_hover = GetSysColor(COLOR_HIGHLIGHT);
            colors.on_accent = GetSysColor(COLOR_HIGHLIGHTTEXT);
        }
        return colors;
    }
};

void fill(HDC dc, RECT bounds, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void rounded(HDC dc, RECT bounds, int radius, COLORREF background, COLORREF border) {
    const auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(dc, border);
    SetDCBrushColor(dc, background);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
    SelectObject(dc, brush);
    SelectObject(dc, pen);
}

void draw_text(HDC dc, std::wstring_view text, RECT bounds, HFONT font, COLORREF color, UINT flags) {
    const auto previous = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &bounds, flags | DT_NOPREFIX);
    SelectObject(dc, previous);
}

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
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, preview_class, L"框选攻略视频画面",
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

    bool handle_key(MSG& message) const {
        if (!window_ || GetAncestor(message.hwnd, GA_ROOT) != window_) {
            return false;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            DestroyWindow(window_);
            return true;
        }
        return IsDialogMessageW(window_, &message) != FALSE;
    }

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
        case WM_CREATE: {
            constexpr auto button_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
            cancel_ = CreateWindowW(L"BUTTON", L"取消", button_style | BS_PUSHBUTTON,
                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
            button_ = CreateWindowW(L"BUTTON", L"使用此区域", button_style | BS_DEFPUSHBUTTON,
                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
            const auto font = reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT));
            SendMessageW(button_, WM_SETFONT, font, TRUE);
            SendMessageW(cancel_, WM_SETFONT, font, TRUE);
            return 0;
        }
        case WM_SIZE: {
            RECT client{};
            GetClientRect(window_, &client);
            const auto dpi = find_dpi(window_);
            const int header = px(36, dpi), footer = px(50, dpi), margin = px(12, dpi);
            const int width = std::max(1L, client.right - 2 * margin);
            const int height = std::max(1L, client.bottom - header - footer);
            const double scale = std::min(static_cast<double>(width) / pixels_.cols,
                                          static_cast<double>(height) / pixels_.rows);
            const LONG w = std::max(1L, std::lround(scale * pixels_.cols));
            const LONG h = std::max(1L, std::lround(scale * pixels_.rows));
            image_ = {(client.right - w) / 2, (height - h) / 2 + header,
                      (client.right - w) / 2 + w, (height - h) / 2 + header + h};
            const int y = client.bottom - footer + px(9, dpi);
            MoveWindow(cancel_, std::max(0L, client.right - px(212, dpi)), y,
                       px(80, dpi), px(32, dpi), TRUE);
            MoveWindow(button_, std::max(0L, client.right - px(122, dpi)), y,
                       px(110, dpi), px(32, dpi), TRUE);
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
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK && selection_.width >= 0.01 && selection_.height >= 0.01) {
                accepted_ = true;
                DestroyWindow(window_);
            } else if (LOWORD(wparam) == IDCANCEL) {
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
        const auto pen = CreatePen(PS_SOLID, 2, RGB(92, 245, 255));
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
        client.left += px(12, find_dpi(window_));
        client.bottom = px(36, find_dpi(window_));
        draw_text(dc, L"沿视频边缘拖动选框，框住完整的攻略画面。", client,
            static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)), GetSysColor(COLOR_WINDOWTEXT),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window_, &paint);
    }

    HWND window_{}, button_{}, cancel_{};
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

    ~SettingsWindow() { clear_fonts(); }

    bool open(int show) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.hInstance = instance_;
        cls.lpfnWndProc = procedure;
        cls.lpszClassName = settings_class;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        if (!RegisterClassExW(&cls)) {
            return false;
        }
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        POINT pointer{};
        GetCursorPos(&pointer);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const auto& work = monitor.rcWork;
        HWND window = CreateWindowExW(0, settings_class, window_title, style, CW_USEDEFAULT, CW_USEDEFAULT,
            form_width, form_height, nullptr, nullptr, instance_, this);
        if (!window) {
            return false;
        }
        fit_window(work, find_dpi(window), work.left, work.top, true);
        update_caption();
        ShowWindow(window, show);
        UpdateWindow(window);
        return true;
    }

    bool handle_key(MSG& message) {
        if (GetAncestor(message.hwnd, GA_ROOT) != window_) {
            return preview_ && preview_->handle_key(message);
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB && GetKeyState(VK_CONTROL) < 0) {
            const int count = static_cast<int>(tab_names.size());
            const int step = GetKeyState(VK_SHIFT) < 0 ? count - 1 : 1;
            select_page(static_cast<Page>((static_cast<int>(active_page_) + step) % count));
            SetFocus(item(settings_tabs));
            return true;
        }
        return IsDialogMessageW(window_, &message) != FALSE;
    }

private:
    struct Element {
        HWND window;
        Layout bounds;
        Page page;
        Font font;
        ControlKind kind;
        bool windows_only;
    };

    HFONT font(Font role) const {
        const auto handle = fonts_[static_cast<std::size_t>(role)];
        return handle ? handle : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }

    void clear_fonts() {
        for (auto& handle : fonts_) {
            if (handle) {
                DeleteObject(handle);
                handle = nullptr;
            }
        }
    }

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
            self->focus_field(error.id);
            self->set_status(error.message, Status::error);
        } catch (const std::exception&) {
            if (message == WM_CREATE) {
                return -1;
            }
            self->set_status(L"操作失败，请检查配置目录是否可写。", Status::error);
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
            } else if (HIWORD(wparam) == EN_CHANGE) {
                const int id = LOWORD(wparam);
                if (id == video_fps || id == player_fps) {
                    update_hud_rate();
                } else if (id >= video_x && id <= video_h) {
                    update_video_rect();
                }
            } else if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS) {
                redraw_field(reinterpret_cast<HWND>(lparam));
            }
            return 0;
        case WM_NOTIFY: {
            const auto* notification = reinterpret_cast<const NMHDR*>(lparam);
            if (notification->idFrom == settings_tabs && notification->code == TCN_SELCHANGE) {
                select_page(static_cast<Page>(TabCtrl_GetCurSel(notification->hwndFrom)));
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT state{};
            const auto dc = BeginPaint(window_, &state);
            paint_background(dc);
            EndPaint(window_, &state);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint_background(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM: {
            const auto* drawing = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (drawing->CtlType == ODT_BUTTON) {
                paint_button(*drawing);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
            return paint_control(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam));
        case WM_SYSCOLORCHANGE:
        case WM_SETTINGCHANGE:
            colors_ = Palette::system();
            update_caption();
            RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            break;
        case WM_LBUTTONDOWN:
            focus_at({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_TIMER:
            tick();
            return 0;
        case WM_DPICHANGED: {
            const auto& bounds = *reinterpret_cast<const RECT*>(lparam);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor);
            fit_window(monitor.rcWork, HIWORD(wparam), bounds.left, bounds.top, false);
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

    HWND add(Page page, const wchar_t* window_class, const wchar_t* label, DWORD style, int id, Layout bounds,
             Font face = Font::body, ControlKind kind = ControlKind::plain, bool windows_only = false) {
        const auto control = CreateWindowExW(0, window_class, label, WS_CHILD | style, 0, 0, 0, 0,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!control) {
            throw std::runtime_error("cannot create control");
        }
        elements_.push_back({control, bounds, page, face, kind, windows_only});
        return control;
    }

    void label(Page page, const wchar_t* text, Layout bounds, Font face = Font::body, int id = 0,
               bool windows_only = false) {
        add(page, L"STATIC", text, SS_CENTERIMAGE | SS_NOPREFIX, id, bounds, face, ControlKind::plain, windows_only);
    }

    void edit(Page page, int id, Layout bounds, bool windows_only = false) {
        add(page, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, id, bounds, Font::body, ControlKind::edit, windows_only);
    }

    void field(Page page, const wchar_t* text, int id, int x, int y, int width) {
        label(page, text, {x, y, width, 22});
        edit(page, id, {x, y + 26, width, 34});
    }

    void button(Page page, const wchar_t* name, int id, Layout bounds, bool windows_only = false) {
        const auto control = add(page, L"BUTTON", name,
            BS_OWNERDRAW | WS_TABSTOP, id, bounds, Font::body, ControlKind::button, windows_only);
        SetWindowSubclass(control, button_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    void checkbox(Page page, const wchar_t* name, int id, Layout bounds) {
        add(page, L"BUTTON", name, BS_AUTOCHECKBOX | WS_TABSTOP, id, bounds);
    }

    void combo(Page page, int id, Layout bounds, std::initializer_list<const wchar_t*> entries) {
        const HWND box = add(page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            id, bounds, Font::body, ControlKind::combo);
        for (const auto* entry : entries) {
            SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry));
        }
    }

    void controls() {
        constexpr auto shared = Page::shared;
        label(shared, L"攻略 HUD 设置", {94, 24, 590, 38}, Font::title);
        label(shared, L"关闭此窗口会退出 HUD 和后台取帧。", {96, 65, 592, 23}, Font::note);
        const HWND tabs = add(shared, WC_TABCONTROLW, L"", WS_TABSTOP | TCS_BUTTONS | TCS_FIXEDWIDTH,
                              settings_tabs, {32, 104, 704, 38});
        for (int index = 0; index < static_cast<int>(tab_names.size()); ++index) {
            TCITEMW tab{};
            tab.mask = TCIF_TEXT;
            tab.pszText = const_cast<wchar_t*>(tab_names[index]);
            TabCtrl_InsertItem(tabs, index, &tab);
        }
        SetWindowSubclass(tabs, tabs_proc, 1, reinterpret_cast<DWORD_PTR>(this));

        tracking_controls();
        edge_controls();
        advanced_controls();
        label(shared, L"HUD 未运行", {50, 527, 686, 24}, Font::note, status);
        button(shared, L"恢复默认", restore, {32, 556, 104, 36});
        button(shared, L"仅保存", save, {148, 556, 94, 36});
        button(shared, L"停止 HUD", stop, {462, 556, 104, 36});
        button(shared, L"应用并启动 HUD", start, {578, 556, 158, 36});
        EnableWindow(item(stop), FALSE);
        write_form(settings_);
    }

    void tracking_controls() {
        constexpr auto page = Page::tracking;
        label(page, L"取帧频率", {56, 165, 300, 24}, Font::section);
        field(page, L"攻略取帧（帧/秒）", video_fps, 56, 199, 200);
        field(page, L"玩家取帧（帧/秒）", player_fps, 280, 199, 200);
        label(page, L"HUD 刷新频率", {520, 196, 176, 21}, Font::note, hud_rate_caption);
        label(page, L"", {520, 216, 176, 29}, Font::value, hud_rate);
        label(page, L"两路较高值 + 5", {520, 245, 176, 18}, Font::note, hud_rate_formula);
        label(page, L"匹配与到达", {56, 295, 300, 24}, Font::section);
        field(page, L"区域搜索前的局部失败帧数", local_misses, 56, 330, 200);
        field(page, L"全图重试前的区域失败次数", region_misses, 280, 330, 200);
        field(page, L"到达判定（地图单位）", arrival, 504, 330, 208);
        checkbox(page, L"详细模式 · 在游戏左侧显示实测帧率和耗时", detailed, {56, 432, 580, 30});
    }

    void edge_controls() {
        constexpr auto page = Page::edge;
        label(page, L"Edge 取帧模式", {56, 165, 350, 24}, Font::section);
        combo(page, edge_mode, {56, 195, 440, 136},
              {L"远程模式 · 支持最小化", L"唤醒模式 · Edge 窗口需还原", L"Windows 捕获 · 无需 Edge 调试"});
        label(page, L"", {56, 231, 656, 22}, Font::note, edge_help);
        label(page, L"", {56, 264, 656, 22}, Font::body, filter_label);
        edit(page, edge_filter, {56, 289, 656, 34});

        label(page, L"固定小地图", {56, 348, 168, 24}, Font::section);
        label(page, L"按 640 × 360 视频坐标", {232, 349, 308, 22}, Font::note);
        field(page, L"圆心 X", circle_x, 56, 377, 148);
        field(page, L"圆心 Y", circle_y, 224, 377, 148);
        field(page, L"半径", circle_r, 392, 377, 148);
        field(page, L"取帧内缩", inset, 560, 377, 152);

        label(page, L"视频范围", {56, 443, 100, 18}, Font::note, 0, true);
        label(page, L"", {56, 464, 100, 30}, Font::body, video_rect_status, true);
        const std::array ids{video_x, video_y, video_w, video_h};
        const std::array names{L"左", L"上", L"宽", L"高"};
        for (int i = 0; i < 4; ++i) {
            const int x = 164 + 71 * i;
            label(page, names[i], {x, 443, 60, 18}, Font::note, 0, true);
            edit(page, ids[i], {x, 464, 60, 30}, true);
        }
        button(page, L"框选视频…", select_video, {460, 464, 126, 30}, true);
        button(page, L"全屏用整窗", reset_video, {598, 464, 114, 30}, true);
    }

    void advanced_controls() {
        constexpr auto page = Page::advanced;
        label(page, L"定位性能", {56, 165, 300, 24}, Font::section);
        label(page, L"攻略匹配算法", {56, 199, 256, 22});
        combo(page, method, {56, 225, 256, 136}, {L"自动 · 先 SIFT 后模板", L"仅 SIFT", L"仅模板"});
        field(page, L"地图块并发数", workers, 336, 199, 176);
        field(page, L"OpenCV 线程数", opencv_threads, 536, 199, 176);
        checkbox(page, L"保存 SIFT 索引缓存", cache_indexes, {56, 274, 420, 28});
        label(page, L"游戏窗口与地图资源", {56, 339, 400, 24}, Font::section);
        field(page, L"游戏窗口标题", game_title, 56, 374, 196);
        label(page, L"地图资源目录", {276, 374, 320, 22});
        edit(page, assets, {276, 400, 334, 34});
        button(page, L"浏览…", browse, {622, 400, 90, 34});
        label(page, L"选择 BetterGI 的 Assets/Map/Teyvat 目录。", {276, 443, 436, 22}, Font::note);
    }

    void write_form(const GuideSettings& settings) {
        SetDlgItemTextW(window_, assets, settings.assets.c_str());
        SetDlgItemTextW(window_, game_title, settings.game_title.c_str());
        SetDlgItemTextW(window_, edge_filter, settings.edge_pattern.c_str());
        SendDlgItemMessageW(window_, edge_mode, CB_SETCURSEL, static_cast<int>(settings.capture_mode), 0);
        SendDlgItemMessageW(window_, method, CB_SETCURSEL, static_cast<int>(settings.method), 0);
        write_number(circle_x, settings.circle.x);
        write_number(circle_y, settings.circle.y);
        write_number(circle_r, settings.circle.radius);
        write_number(inset, settings.inset);
        write_number(video_fps, settings.video_fps);
        write_number(player_fps, settings.player_fps);
        write_number(local_misses, settings.local_misses);
        write_number(region_misses, settings.region_misses);
        write_number(arrival, settings.arrival_distance);
        write_number(workers, settings.workers);
        write_number(opencv_threads, settings.opencv_threads);
        write_rect(settings.video_rect);
        CheckDlgButton(window_, cache_indexes, settings.cache_indexes ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window_, detailed, settings.detailed ? BST_CHECKED : BST_UNCHECKED);
        update_hud_rate();
        update_mode();
    }

    void resize(UINT dpi) {
        dpi_ = dpi;
        auto old_fonts = fonts_;
        const std::array sizes{14, 13, 16, 27, 20};
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            const int weight = i < static_cast<std::size_t>(Font::section) ? FW_NORMAL : FW_SEMIBOLD;
            fonts_[i] = CreateFontW(-px(sizes[i], dpi_), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Microsoft YaHei UI");
        }
        for (const auto& element : elements_) {
            SendMessageW(element.window, WM_SETFONT, reinterpret_cast<WPARAM>(font(element.font)), FALSE);
            auto bounds = element.bounds.scaled(dpi_);
            if (element.kind == ControlKind::edit) {
                InflateRect(&bounds, -px(10, dpi_), -px(6, dpi_));
            } else if (element.kind == ControlKind::combo) {
                SendMessageW(element.window, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(27, dpi_));
                SendMessageW(element.window, CB_SETITEMHEIGHT, 0, px(28, dpi_));
            }
            MoveWindow(element.window, bounds.left, bounds.top, bounds.right - bounds.left,
                       bounds.bottom - bounds.top, FALSE);
        }
        TabCtrl_SetItemSize(item(settings_tabs), px(151, dpi_), px(36, dpi_));
        for (auto old : old_fonts) {
            if (old) {
                DeleteObject(old);
            }
        }
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void fit_window(RECT work, UINT monitor_dpi, LONG left, LONG top, bool center) {
        if (IsRectEmpty(&work)) {
            work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
        RECT frame{};
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
        AdjustWindowRectExForDpi(&frame, style, FALSE, 0, monitor_dpi);
        const int chrome_width = frame.right - frame.left, chrome_height = frame.bottom - frame.top;
        const int max_width = work.right - work.left, max_height = work.bottom - work.top;
        const int dpi = std::max(48, std::min({static_cast<int>(monitor_dpi),
            (max_width - chrome_width) * 96 / form_width, (max_height - chrome_height) * 96 / form_height}));
        const int width = px(form_width, dpi) + chrome_width, height = px(form_height, dpi) + chrome_height;
        const LONG x = center ? work.left + (max_width - width) / 2
            : std::clamp(left, work.left, std::max(work.left, work.right - width));
        const LONG y = center ? work.top + (max_height - height) / 2
            : std::clamp(top, work.top, std::max(work.top, work.bottom - height));
        resize(dpi);
        SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void update_caption() {
        const COLORREF background = colors_.canvas, text = colors_.ink;
        DwmSetWindowAttribute(window_, DWMWA_CAPTION_COLOR, &background, sizeof(background));
        DwmSetWindowAttribute(window_, DWMWA_TEXT_COLOR, &text, sizeof(text));
    }

    const Element* element_for(HWND control) const {
        const auto found = std::find_if(elements_.begin(), elements_.end(), [control](const auto& element) {
            return element.window == control;
        });
        return found == elements_.end() ? nullptr : &*found;
    }

    bool is_visible(const Element& element, bool window_capture) const {
        return (element.page == Page::shared || element.page == active_page_)
            && (!element.windows_only || window_capture);
    }

    void refresh_visibility() {
        const bool window_capture = uses_window_capture();
        for (const auto& element : elements_) {
            ShowWindow(element.window, is_visible(element, window_capture) ? SW_SHOWNA : SW_HIDE);
        }
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void select_page(Page page) {
        if (page == Page::shared || static_cast<std::size_t>(page) >= tab_names.size()) {
            return;
        }
        active_page_ = page;
        TabCtrl_SetCurSel(item(settings_tabs), static_cast<int>(page));
        refresh_visibility();
    }

    void focus_field(int id) {
        const auto control = item(id);
        if (const auto* element = element_for(control)) {
            if (element->page != Page::shared && element->page != active_page_) {
                select_page(element->page);
            }
            SetFocus(control);
        }
    }

    void focus_at(POINT point) {
        const bool window_capture = uses_window_capture();
        for (const auto& element : elements_) {
            const auto bounds = element.bounds.scaled(dpi_);
            if (element.kind == ControlKind::edit && is_visible(element, window_capture) && PtInRect(&bounds, point)) {
                SetFocus(element.window);
                break;
            }
        }
    }

    void redraw_field(HWND control) {
        if (const auto* element = element_for(control); element && element->kind == ControlKind::edit) {
            const auto bounds = element->bounds.scaled(dpi_);
            InvalidateRect(window_, &bounds, FALSE);
        }
    }

    void paint_background(HDC dc) {
        RECT client{};
        GetClientRect(window_, &client);
        const auto box = [&](Layout bounds, int radius, COLORREF background, COLORREF outline) {
            rounded(dc, bounds.scaled(dpi_), px(radius, dpi_), background, outline);
        };
        const auto line = [&](int y) { fill(dc, Layout{56, y, 656, 1}.scaled(dpi_), colors_.border); };
        fill(dc, client, colors_.canvas);
        box({32, 147, 704, 366}, 16, colors_.surface, colors_.border);
        box({32, 32, 46, 46}, 15, colors_.accent, colors_.accent);
        const std::array<POINT, 4> needle{{
            {px(64, dpi_), px(43, dpi_)}, {px(58, dpi_), px(59, dpi_)},
            {px(46, dpi_), px(66, dpi_)}, {px(51, dpi_), px(50, dpi_)}}};
        const auto pen = SelectObject(dc, GetStockObject(NULL_PEN));
        const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, colors_.on_accent);
        Polygon(dc, needle.data(), static_cast<int>(needle.size()));
        SelectObject(dc, brush);
        SelectObject(dc, pen);

        switch (active_page_) {
        case Page::tracking:
            box({504, 190, 208, 78}, 12, colors_.well, colors_.well);
            line(280);
            line(414);
            break;
        case Page::edge:
            line(338);
            break;
        case Page::advanced:
            line(320);
            break;
        default:
            break;
        }
        const bool window_capture = uses_window_capture();
        const auto focus = GetFocus();
        for (const auto& element : elements_) {
            if (element.kind == ControlKind::edit && is_visible(element, window_capture)) {
                box(element.bounds, 10, colors_.surface, focus == element.window ? colors_.accent : colors_.input_border);
            }
        }
        const auto dot = status_ == Status::running ? colors_.accent
            : status_ == Status::error ? colors_.error : colors_.muted;
        box({33, 535, 8, 8}, 8, dot, dot);
    }

    LRESULT paint_control(HDC dc, HWND control) const {
        COLORREF background = colors_.surface, foreground = colors_.ink;
        const int id = GetDlgCtrlID(control);
        if (id == hud_rate || id == hud_rate_caption || id == hud_rate_formula) {
            background = colors_.well;
            foreground = id == hud_rate ? colors_.accent : colors_.muted;
        } else if (const auto* element = element_for(control)) {
            if (element->page == Page::shared) {
                background = colors_.canvas;
            }
            if (element->font == Font::note) {
                foreground = colors_.muted;
            }
        }
        if (id == status && status_ == Status::error) {
            foreground = colors_.error;
        }
        SetDCBrushColor(dc, background);
        SetBkColor(dc, background);
        SetTextColor(dc, foreground);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }

    void paint_button(const DRAWITEMSTRUCT& drawing) const {
        const auto dc = drawing.hDC;
        const auto bounds = drawing.rcItem;
        const auto* element = element_for(drawing.hwndItem);
        const bool primary = drawing.CtlID == start;
        const bool disabled = (drawing.itemState & ODS_DISABLED) != 0;
        const bool hot = hovered_ == drawing.hwndItem || (drawing.itemState & ODS_SELECTED);
        const auto surround = element && element->page != Page::shared ? colors_.surface : colors_.canvas;
        const auto background = primary ? (hot ? colors_.accent_hover : colors_.accent)
            : disabled ? colors_.canvas : hot ? colors_.well : colors_.surface;
        fill(dc, bounds, surround);
        rounded(dc, bounds, px(12, dpi_), background, primary ? background : colors_.border);
        draw_text(dc, text_of(drawing.hwndItem), bounds, font(Font::body),
                  primary ? colors_.on_accent : disabled ? colors_.muted : colors_.ink,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (drawing.itemState & ODS_FOCUS) {
            auto focus = bounds;
            InflateRect(&focus, -px(4, dpi_), -px(4, dpi_));
            DrawFocusRect(dc, &focus);
        }
    }

    void paint_tabs(HWND control, HDC dc) const {
        RECT bounds{};
        GetClientRect(control, &bounds);
        fill(dc, bounds, colors_.canvas);
        const int selected = TabCtrl_GetCurSel(control);
        const int focused = GetFocus() == control ? TabCtrl_GetCurFocus(control) : -1;
        for (int i = 0; i < static_cast<int>(tab_names.size()); ++i) {
            RECT tab{};
            if (!TabCtrl_GetItemRect(control, i, &tab)) {
                continue;
            }
            InflateRect(&tab, -px(1, dpi_), -px(1, dpi_));
            if (i == selected) {
                rounded(dc, tab, px(12, dpi_), colors_.well, colors_.border);
                fill(dc, {tab.left + px(16, dpi_), tab.bottom - px(3, dpi_),
                          tab.right - px(16, dpi_), tab.bottom - px(1, dpi_)}, colors_.accent);
            }
            draw_text(dc, tab_names[i], tab, font(i == selected ? Font::section : Font::body),
                      i == selected ? colors_.ink : colors_.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (i == focused) {
                InflateRect(&tab, -px(5, dpi_), -px(5, dpi_));
                DrawFocusRect(dc, &tab);
            }
        }
    }

    static LRESULT CALLBACK tabs_proc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR id, DWORD_PTR state) {
        const auto* self = reinterpret_cast<SettingsWindow*>(state);
        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(control, &paint);
            self->paint_tabs(control, dc);
            EndPaint(control, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            self->paint_tabs(control, reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_KEYDOWN:
        case WM_LBUTTONUP: {
            const auto result = DefSubclassProc(control, message, wparam, lparam);
            InvalidateRect(control, nullptr, FALSE);
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(control, tabs_proc, id);
            break;
        }
        return DefSubclassProc(control, message, wparam, lparam);
    }

    static LRESULT CALLBACK button_proc(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR id, DWORD_PTR state) {
        auto* self = reinterpret_cast<SettingsWindow*>(state);
        if (message == WM_MOUSEMOVE && self->hovered_ != control) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, control, 0};
            TrackMouseEvent(&track);
            if (self->hovered_) {
                InvalidateRect(self->hovered_, nullptr, FALSE);
            }
            self->hovered_ = control;
            InvalidateRect(control, nullptr, FALSE);
        } else if (message == WM_MOUSELEAVE) {
            if (self->hovered_ == control) {
                self->hovered_ = nullptr;
            }
            InvalidateRect(control, nullptr, FALSE);
        } else if (message == BM_SETSTYLE) {
            wparam = (wparam & ~static_cast<WPARAM>(BS_TYPEMASK)) | BS_OWNERDRAW;
        } else if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control, button_proc, id);
        }
        return DefSubclassProc(control, message, wparam, lparam);
    }

    bool uses_window_capture() const {
        return SendDlgItemMessageW(window_, edge_mode, CB_GETCURSEL, 0, 0) ==
            static_cast<int>(guide::CaptureMode::windows);
    }

    void update_mode() {
        const auto mode = SendDlgItemMessageW(window_, edge_mode, CB_GETCURSEL, 0, 0);
        const bool windows = uses_window_capture();
        if (!windows) {
            if (preview_stop_) {
                SetEvent(preview_stop_.get());
            }
            preview_.reset();
        }
        if (mode >= 0 && mode <= 2) {
            const std::array hints{
                L"复用当前 Edge。允许本地调试后，最小化仍可读取攻略视频。",
                L"需要本地调试授权。保持 Edge 还原，并让小地图留在可见区域。",
                L"保持攻略在已还原的 Edge 中播放。普通网页先框选视频画面。"};
            SetDlgItemTextW(window_, edge_help, hints[mode]);
        }
        SetDlgItemTextW(window_, filter_label, windows ? L"攻略窗口标题（正则）" : L"攻略标签筛选（正则）");
        EnableWindow(item(select_video), windows && !preview_frame_.valid() && !preview_);
        if (windows) {
            update_video_rect();
        }
        refresh_visibility();
    }

    void update_hud_rate() {
        const auto guide_rate = guide::parse_setting_number(text_of(item(video_fps)));
        const auto player_rate = guide::parse_setting_number(text_of(item(player_fps)));
        const auto valid = [](auto value) {
            return value && *value >= guide::min_capture_fps && *value <= guide::max_capture_fps;
        };
        const auto rate = valid(guide_rate) && valid(player_rate)
            ? guide::setting_number(guide::hud_fps(*guide_rate, *player_rate)) + L" 帧/秒" : L"等待有效帧率";
        SetDlgItemTextW(window_, hud_rate, rate.c_str());
    }

    void update_video_rect() {
        if (!uses_window_capture()) {
            return;
        }
        const wchar_t* text = L"范围无效";
        try {
            const auto rect = read_rect();
            text = rect.x == 0 && rect.y == 0 && rect.width == 1 && rect.height == 1 ? L"整窗" : L"已设定";
        } catch (const FieldError&) {}
        SetDlgItemTextW(window_, video_rect_status, text);
    }

    void set_status(const std::wstring& message, Status tone = Status::info) {
        status_ = tone;
        SetDlgItemTextW(window_, status, message.c_str());
        RECT indicator = Layout{32, 528, 14, 22}.scaled(dpi_);
        InvalidateRect(window_, &indicator, FALSE);
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

    void write_number(int id, double value) {
        SetDlgItemTextW(window_, id, guide::setting_number(value).c_str());
    }

    void write_rect(const NormalizedRect& rect) {
        write_number(video_x, rect.x);
        write_number(video_y, rect.y);
        write_number(video_w, rect.width);
        write_number(video_h, rect.height);
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
        try {
            settings.video_rect = read_rect();
        } catch (const FieldError&) {
            if (settings.capture_mode == guide::CaptureMode::windows) {
                throw;
            }
        }
        settings.circle.x = number(circle_x, 0, 639, L"圆心 X");
        settings.circle.y = number(circle_y, 0, 359, L"圆心 Y");
        settings.circle.radius = number(circle_r, 1, 320, L"半径");
        settings.inset = number(inset, 0, 319, L"取帧内缩");
        const double half = settings.circle.radius - settings.inset;
        const double x = settings.circle.x, y = settings.circle.y;
        if (half < 1 || half >= std::min({x, y, 640 - x, 360 - y})) {
            throw FieldError{inset, L"内缩后的小地图范围须落在 640 × 360 视频坐标内。"};
        }
        settings.video_fps = number(video_fps, guide::min_capture_fps, guide::max_capture_fps, L"攻略帧率");
        settings.player_fps = number(player_fps, guide::min_capture_fps, guide::max_capture_fps, L"玩家帧率");
        settings.local_misses = static_cast<int>(number(local_misses, 1, guide::max_retry_count, L"图块失败次数", true));
        settings.region_misses = static_cast<int>(number(region_misses, 1, guide::max_retry_count, L"区域失败次数", true));
        settings.arrival_distance = number(arrival, 0, guide::max_arrival_distance, L"到达距离");
        settings.workers = static_cast<int>(number(workers, 1, 32, L"搜索并发数", true));
        settings.opencv_threads = static_cast<int>(number(opencv_threads, 1, guide::max_opencv_threads, L"OpenCV 线程数", true));
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
            set_status(L"无法打开目录选择窗口。", Status::error);
            return;
        }
        dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
        dialog->SetTitle(L"选择 BetterGI 的 Assets/Map/Teyvat 目录");
        ComPtr<IShellItem> current;
        const auto current_path = text_of(item(assets));
        if (!current_path.empty() && SUCCEEDED(SHCreateItemFromParsingName(
            current_path.c_str(), nullptr, IID_PPV_ARGS(&current)))) {
            dialog->SetFolder(current.Get());
        }
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
        case restore: {
            auto defaults = guide::default_settings(root_);
            if (defaults.assets.empty()) {
                defaults.assets = text_of(item(assets));
            }
            write_form(defaults);
            set_status(L"已恢复默认值，可仅保存或应用并启动 HUD。");
            break;
        }
        case browse:
            choose_assets();
            break;
        case reset_video:
            write_rect({});
            set_status(L"视频范围已设为整窗。");
            break;
        case select_video: {
            if (!uses_window_capture() || preview_frame_.valid() || preview_) {
                break;
            }
            GuideSettings settings = settings_;
            settings.edge_pattern = read_filter();
            preview_stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!preview_stop_) {
                set_status(L"无法启动窗口预览。", Status::error);
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
            if (!guide::valid_assets(settings.assets)) {
                throw FieldError{assets, L"请先选择 BetterGI 的 Assets/Map/Teyvat 目录。"};
            }
            guide::save_settings(root_, settings);
            settings_ = std::move(settings);
            if (id == start) {
                const bool running = hud_.open(exe_);
                EnableWindow(item(stop), running);
                const bool windows = settings_.capture_mode == guide::CaptureMode::windows;
                set_status(running ? (windows ? L"HUD 已启动，正在连接 Windows 捕获。"
                                             : L"HUD 已启动，必要时在当前 Edge 允许本地连接。")
                                   : L"无法启动 HUD 进程，请重试。", running ? Status::running : Status::error);
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
            set_status(L"HUD 已退出，请检查运行库或地图目录后重试。", Status::error);
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
            if (!uses_window_capture()) {
                set_status(L"取帧模式已切换。");
            } else if (frame.image.empty()) {
                set_status(frame.status, Status::error);
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
    HWND hovered_{};
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    Page active_page_ = Page::tracking;
    Status status_ = Status::info;
    Palette colors_ = Palette::system();
    std::array<HFONT, static_cast<std::size_t>(Font::count)> fonts_{};
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
        const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES};
        InitCommonControlsEx(&controls);
        int result = 1;
        {
            SettingsWindow settings(instance, exe);
            if (settings.open(show)) {
                MSG message{};
                while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                    if (!settings.handle_key(message)) {
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
