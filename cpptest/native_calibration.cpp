#include "native_calibration.hpp"

#include <uxtheme.h>
#include <windowsx.h>
#include <opencv2/imgproc.hpp>

#include <array>
#include <cmath>
#include <stdexcept>

namespace guide {
namespace {

constexpr wchar_t preview_class[] = L"GenshinVideoGuideHUD.Preview";
constexpr int zoom_out = 200, zoom_in = 201, zoom_fit = 202, inset_input = 203;
constexpr int center_x_input = 204, center_y_input = 205, radius_input = 206;
constexpr int field_label_offset = 100;
constexpr double minimum_diameter = 8;
constexpr COLORREF selection_color = RGB(92, 245, 255);

struct CircleField {
    int id;
    const wchar_t* label;
};

constexpr std::array<CircleField, 4> circle_fields{{
    {center_x_input, L"圆心 X"}, {center_y_input, L"圆心 Y"},
    {radius_input, L"半径"}, {inset_input, L"内缩"}
}};

enum class PointerAction { none, selecting, panning };

double center(double start, double size) {
    return start + size / 2;
}

LONG pixel(double value) {
    return std::lround(value);
}

}

struct CapturePreview::Impl {
    Impl(HINSTANCE instance, HWND parent, const cv::Mat& image, CalibrationKind kind,
         const NormalizedRect& video, const std::optional<WindowMinimap>& minimap)
        : kind_(kind) {
        cv::cvtColor(image, pixels_, cv::COLOR_BGR2BGRA);
        if (kind_ == CalibrationKind::video) {
            selection_ = to_pixels(video);
        } else if (minimap) {
            const auto bounds = to_pixels(minimap->bounds);
            const double radius = std::min(bounds.width, bounds.height) / 2;
            selection_ = {center(bounds.x, bounds.width) - radius, center(bounds.y, bounds.height) - radius,
                          2 * radius, 2 * radius};
            inset_ = radius * minimap->inset_fraction;
        }

        WNDCLASSEXW definition{};
        definition.cbSize = sizeof(definition);
        definition.hInstance = instance;
        definition.lpfnWndProc = procedure;
        definition.lpszClassName = preview_class;
        definition.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        RegisterClassExW(&definition);

        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &monitor)) {
            monitor.rcWork = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
        const auto& work = monitor.rcWork;
        const int width = std::min(1120L, work.right - work.left - 36);
        const int height = std::min(760L, work.bottom - work.top - 36);
        const auto title = is_circle() ? L"圈选小地图" : L"框选视频画面";
        constexpr DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
        if (!CreateWindowExW(WS_EX_TOOLWINDOW, preview_class, title, style,
            work.left + (work.right - work.left - width) / 2,
            work.top + (work.bottom - work.top - height) / 2, width, height,
            parent, nullptr, instance, this)) {
            throw std::runtime_error("capture preview window unavailable");
        }
        ShowWindow(window_, SW_SHOW);
    }

    ~Impl() {
        if (window_) DestroyWindow(window_);
        if (font_) DeleteObject(font_);
    }

    bool is_circle() const { return kind_ == CalibrationKind::minimap; }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        return self->handle(message, wparam, lparam);
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            buffered_paint_ = SUCCEEDED(BufferedPaintInit());
            controls();
            return 0;
        case WM_SIZE:
            layout();
            fit();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            const auto& area = *reinterpret_cast<const RECT*>(lparam);
            set_font();
            SetWindowPos(window_, nullptr, area.left, area.top, area.right - area.left, area.bottom - area.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {scaled(590), scaled(365)};
            return 0;
        }
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_COMMAND:
            command(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_MOUSEWHEEL: {
            POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(window_, &cursor);
            if (!PtInRect(&canvas_, cursor)) cursor = canvas_center();
            zoom(cursor, std::pow(1.25, GET_WHEEL_DELTA_WPARAM(wparam) / double(WHEEL_DELTA)));
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            begin({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, message == WM_LBUTTONDOWN);
            return 0;
        case WM_MOUSEMOVE:
            move({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            end({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, message == WM_LBUTTONUP);
            return 0;
        case WM_CAPTURECHANGED:
            if (action_ == PointerAction::selecting) restore_selection();
            action_ = PointerAction::none;
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window_, &paint);
            if (dc && !IsRectEmpty(&paint.rcPaint)) {
                HDC canvas = nullptr;
                const auto buffer = buffered_paint_
                    ? BeginBufferedPaint(dc, &paint.rcPaint, BPBF_COMPATIBLEBITMAP, nullptr, &canvas)
                    : nullptr;
                draw(buffer ? canvas : dc);
                if (buffer) EndBufferedPaint(buffer, TRUE);
            }
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            draw(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY: {
            const auto window = window_;
            window_ = nullptr;
            if (buffered_paint_) {
                BufferedPaintUnInit();
                buffered_paint_ = false;
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    int scaled(int logical) const {
        const auto dpi = GetDpiForWindow(window_);
        return MulDiv(logical, dpi ? static_cast<int>(dpi) : 96, 96);
    }

    HWND button(int id, const wchar_t* text) {
        return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            (id == IDOK ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), 0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    }

    void controls() {
        button(zoom_out, L"缩小");
        button(zoom_in, L"放大");
        button(zoom_fit, L"适应窗口");
        if (is_circle()) {
            for (const auto& field : circle_fields) {
                CreateWindowW(L"STATIC", field.label, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(field.id + field_label_offset)),
                    nullptr, nullptr);
                CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(field.id)), nullptr, nullptr);
            }
            write_circle_inputs();
            SetDlgItemTextW(window_, inset_input, setting_number(inset_).c_str());
        }
        button(IDCANCEL, L"取消");
        button(IDOK, L"使用此区域");
        set_font();
    }

    void set_font() {
        const auto font = CreateFontW(-scaled(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Microsoft YaHei UI");
        for (int id : {zoom_out, zoom_in, zoom_fit, IDCANCEL, IDOK}) {
            SendDlgItemMessageW(window_, id, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        }
        if (is_circle()) {
            for (const auto& field : circle_fields) {
                SendDlgItemMessageW(window_, field.id, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
                SendDlgItemMessageW(window_, field.id + field_label_offset, WM_SETFONT,
                                    reinterpret_cast<WPARAM>(font), FALSE);
            }
        }
        if (font_) DeleteObject(font_);
        font_ = font;
    }

    void layout() {
        RECT client{};
        GetClientRect(window_, &client);
        const auto place = [&](int id, int x, int y, int w, int h) {
            if (const auto control = GetDlgItem(window_, id)) {
                MoveWindow(control, x, y, scaled(w), scaled(h), TRUE);
            }
        };
        const int footer = client.bottom - scaled(43);
        place(zoom_out, scaled(14), scaled(39), 62, 27);
        place(zoom_in, scaled(84), scaled(39), 62, 27);
        place(zoom_fit, scaled(154), scaled(39), 94, 27);
        place(IDCANCEL, client.right - scaled(216), footer, 81, 30);
        place(IDOK, client.right - scaled(124), footer, 110, 30);
        if (is_circle()) {
            int x = 14;
            for (const auto& field : circle_fields) {
                place(field.id + field_label_offset, scaled(x), client.bottom - scaled(70), 82, 22);
                place(field.id, scaled(x), footer, 78, 28);
                x += 84;
            }
        }
        canvas_ = {scaled(14), scaled(77), client.right - scaled(14), client.bottom - scaled(is_circle() ? 102 : 78)};
        zoom_caption_ = {scaled(263), scaled(38), client.right - scaled(14), scaled(65)};
        const int info_top = is_circle() ? 98 : 72;
        info_area_ = {scaled(14), client.bottom - scaled(info_top), client.right - scaled(14),
                      client.bottom - scaled(info_top - 25)};
    }

    POINT canvas_center() const {
        return {(canvas_.left + canvas_.right) / 2, (canvas_.top + canvas_.bottom) / 2};
    }

    void fit() {
        if (IsRectEmpty(&canvas_)) return;
        fit_scale_ = std::min(double(canvas_.right - canvas_.left) / pixels_.cols,
                              double(canvas_.bottom - canvas_.top) / pixels_.rows);
        scale_ = fit_scale_;
        clamp_origin();
    }

    void clamp_origin() {
        const auto axis = [](double current, double length, LONG start, LONG finish) {
            return length <= finish - start ? (start + finish - length) / 2
                                             : std::clamp(current, finish - length, double(start));
        };
        origin_.x = axis(origin_.x, scale_ * pixels_.cols, canvas_.left, canvas_.right);
        origin_.y = axis(origin_.y, scale_ * pixels_.rows, canvas_.top, canvas_.bottom);
    }

    void zoom(POINT anchor, double factor) {
        if (action_ != PointerAction::none || IsRectEmpty(&canvas_)) return;
        const auto scale = std::clamp(scale_ * factor, fit_scale_, std::max(12.0, fit_scale_));
        if (scale == scale_) return;
        const auto image_point = source_point(anchor);
        scale_ = scale;
        origin_ = {anchor.x - scale_ * image_point.x, anchor.y - scale_ * image_point.y};
        clamp_origin();
        invalidate(canvas_);
        invalidate(zoom_caption_);
    }

    cv::Point2d source_point(POINT point) const {
        return {(point.x - origin_.x) / scale_, (point.y - origin_.y) / scale_};
    }

    cv::Rect2d to_pixels(const NormalizedRect& bounds) const {
        return {bounds.x * pixels_.cols, bounds.y * pixels_.rows,
                bounds.width * pixels_.cols, bounds.height * pixels_.rows};
    }

    RECT on_screen(const cv::Rect2d& bounds) const {
        return {pixel(origin_.x + scale_ * bounds.x), pixel(origin_.y + scale_ * bounds.y),
                pixel(origin_.x + scale_ * (bounds.x + bounds.width)),
                pixel(origin_.y + scale_ * (bounds.y + bounds.height))};
    }

    void begin(POINT pointer, bool left) {
        if (action_ != PointerAction::none || !PtInRect(&canvas_, pointer)) return;
        if (left) {
            anchor_ = source_point(pointer);
            if (anchor_.x < 0 || anchor_.y < 0 || anchor_.x >= pixels_.cols || anchor_.y >= pixels_.rows) return;
            previous_ = selection_;
            action_ = PointerAction::selecting;
        } else {
            pan_anchor_ = pointer;
            action_ = PointerAction::panning;
        }
        SetFocus(window_);
        SetCapture(window_);
        move(pointer);
    }

    void move(POINT pointer) {
        if (action_ == PointerAction::none) return;
        if (action_ == PointerAction::panning) {
            origin_.x += pointer.x - pan_anchor_.x;
            origin_.y += pointer.y - pan_anchor_.y;
            pan_anchor_ = pointer;
            clamp_origin();
        } else {
            auto point = source_point(pointer);
            if (is_circle()) {
                const auto radius = std::min({std::hypot(point.x - anchor_.x, point.y - anchor_.y),
                    anchor_.x, anchor_.y, pixels_.cols - anchor_.x, pixels_.rows - anchor_.y});
                selection_ = {anchor_.x - radius, anchor_.y - radius, 2 * radius, 2 * radius};
            } else {
                point.x = std::clamp(point.x, 0.0, double(pixels_.cols));
                point.y = std::clamp(point.y, 0.0, double(pixels_.rows));
                selection_ = {std::min(anchor_.x, point.x), std::min(anchor_.y, point.y),
                              std::abs(point.x - anchor_.x), std::abs(point.y - anchor_.y)};
            }
            if (!is_circle() || !notice_.empty()) invalidate(info_area_);
            notice_.clear();
        }
        invalidate(canvas_);
    }

    void restore_selection() {
        selection_ = previous_;
        invalidate(canvas_);
        if (!is_circle()) invalidate(info_area_);
    }

    void end(POINT pointer, bool left) {
        const auto expected = left ? PointerAction::selecting : PointerAction::panning;
        if (action_ != expected) return;
        move(pointer);
        action_ = PointerAction::none;
        ReleaseCapture();
        if (left) {
            if (selection_.width < minimum_diameter || selection_.height < minimum_diameter) {
                restore_selection();
            } else if (is_circle()) {
                write_circle_inputs();
            }
        }
    }

    std::optional<double> input_number(int id) const {
        wchar_t text[32]{};
        if (GetWindowTextLengthW(GetDlgItem(window_, id)) >= static_cast<int>(ARRAYSIZE(text))) {
            return std::nullopt;
        }
        GetDlgItemTextW(window_, id, text, ARRAYSIZE(text));
        return parse_setting_number(text);
    }

    cv::Rect2d circle_from_inputs() const {
        const auto x = input_number(center_x_input);
        const auto y = input_number(center_y_input);
        const auto radius = input_number(radius_input);
        if (!x || !y || !radius || *radius < minimum_diameter / 2) {
            return {};
        }
        const double limit = std::min({*x, *y, pixels_.cols - *x, pixels_.rows - *y});
        // The displayed coordinates are rounded by setting_number, so allow subpixel
        // rounding at an image boundary while keeping the actual circle inside it.
        if (limit < minimum_diameter / 2 || *radius > limit + 0.00001) {
            return {};
        }
        const double r = std::min(*radius, limit);
        return {*x - r, *y - r, 2 * r, 2 * r};
    }

    void write_circle_inputs() {
        syncing_circle_ = true;
        const auto set = [&](int id, double value) {
            const auto text = selection_.empty() ? L"" : setting_number(value);
            SetDlgItemTextW(window_, id, text.c_str());
        };
        set(center_x_input, center(selection_.x, selection_.width));
        set(center_y_input, center(selection_.y, selection_.height));
        set(radius_input, selection_.width / 2);
        syncing_circle_ = false;
    }

    void accept() {
        const auto bounded = bounded_video_rect({selection_.x / pixels_.cols, selection_.y / pixels_.rows,
            selection_.width / pixels_.cols, selection_.height / pixels_.rows});
        const double min_width = is_circle() ? minimum_diameter : std::max(minimum_diameter, pixels_.cols * 0.01);
        const double min_height = is_circle() ? minimum_diameter : std::max(minimum_diameter, pixels_.rows * 0.01);
        if (action_ != PointerAction::none || !bounded || selection_.width < min_width || selection_.height < min_height) {
            notice_ = is_circle() ? L"请设置圆心和半径。圆须完整位于原图内，半径至少 4 px。"
                                  : L"请框出完整的视频画面。";
            invalidate(info_area_);
            return;
        }
        double fraction = 0;
        if (is_circle()) {
            const auto inset = input_number(inset_input);
            if (!inset || *inset < 0 || *inset > selection_.width / 2 - 2) {
                notice_ = L"内缩须为非负像素，且至少保留 2 像素半径。";
                invalidate(info_area_);
                return;
            }
            fraction = *inset / (selection_.width / 2);
        }
        result_ = WindowMinimap{*bounded, fraction};
        DestroyWindow(window_);
    }

    void command(int id, int notification) {
        if (notification == EN_CHANGE) {
            if (syncing_circle_) return;
            if (id == center_x_input || id == center_y_input || id == radius_input) {
                selection_ = circle_from_inputs();
            } else if (id != inset_input) {
                return;
            }
            notice_.clear();
            invalidate(canvas_);
            invalidate(info_area_);
        } else if (notification == BN_CLICKED) {
            switch (id) {
            case zoom_out: zoom(canvas_center(), 0.8); break;
            case zoom_in: zoom(canvas_center(), 1.25); break;
            case zoom_fit:
                fit();
                invalidate(canvas_);
                invalidate(zoom_caption_);
                break;
            case IDCANCEL: DestroyWindow(window_); break;
            case IDOK: accept(); break;
            }
        }
    }

    void invalidate(const RECT& area) {
        if (window_) InvalidateRect(window_, &area, FALSE);
    }

    void draw_text(HDC dc, const std::wstring& text, RECT area, COLORREF color) const {
        if (text.empty() || !RectVisible(dc, &area)) return;
        const auto previous = SelectObject(dc, font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &area, DT_LEFT | DT_SINGLELINE |
                  DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, previous);
    }

    void draw_image(HDC dc) const {
        const auto saved = SaveDC(dc);
        IntersectClipRect(dc, canvas_.left, canvas_.top, canvas_.right, canvas_.bottom);
        SetDCBrushColor(dc, RGB(35, 51, 59));
        FillRect(dc, &canvas_, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        const auto area = on_screen({0, 0, double(pixels_.cols), double(pixels_.rows)});
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
        bitmap.bmiHeader.biWidth = pixels_.cols;
        bitmap.bmiHeader.biHeight = -pixels_.rows;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, scale_ >= 1 ? COLORONCOLOR : HALFTONE);
        StretchDIBits(dc, area.left, area.top, area.right - area.left, area.bottom - area.top,
                      0, 0, pixels_.cols, pixels_.rows, pixels_.data, &bitmap, DIB_RGB_COLORS, SRCCOPY);
        if (!selection_.empty()) draw_selection(dc);
        RestoreDC(dc, saved);
    }

    void draw_selection(HDC dc) const {
        auto bounds = on_screen(selection_);
        const auto pen = CreatePen(PS_SOLID, scaled(2), selection_color);
        const auto previous_pen = SelectObject(dc, pen);
        const auto previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (is_circle()) {
            Ellipse(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
            const int x = (bounds.left + bounds.right) / 2, y = (bounds.top + bounds.bottom) / 2, cross = scaled(6);
            MoveToEx(dc, x - cross, y, nullptr);
            LineTo(dc, x + cross, y);
            MoveToEx(dc, x, y - cross, nullptr);
            LineTo(dc, x, y + cross);
            if (const auto inset = input_number(inset_input); inset && *inset > 0 && *inset < selection_.width / 2) {
                InflateRect(&bounds, -pixel(*inset * scale_), -pixel(*inset * scale_));
                const auto inner = CreatePen(PS_DOT, 1, RGB(255, 239, 150));
                SelectObject(dc, inner);
                Ellipse(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
                SelectObject(dc, pen);
                DeleteObject(inner);
            }
        } else {
            Rectangle(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
        }
        SelectObject(dc, previous_brush);
        SelectObject(dc, previous_pen);
        DeleteObject(pen);
    }

    void draw(HDC dc) const {
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
        if (RectVisible(dc, &canvas_)) draw_image(dc);
        const auto ink = GetSysColor(COLOR_WINDOWTEXT);
        const auto hint = is_circle() ? L"直接输入坐标，或左键从圆心拖到圆边。滚轮缩放，右键平移。"
                                       : L"左键沿边缘框住整个视频画面。滚轮缩放，右键拖动平移。";
        draw_text(dc, hint, {scaled(14), scaled(8), client.right - scaled(14), scaled(32)}, ink);
        draw_text(dc, std::to_wstring(std::lround(scale_ * 100)) + L"%", zoom_caption_, ink);
        std::wstring info = notice_;
        if (info.empty()) {
            if (is_circle()) {
                info = L"原图 " + std::to_wstring(pixels_.cols) + L" × " + std::to_wstring(pixels_.rows)
                    + L" px   ·   以截图左上角为原点，参数均为原图像素";
            } else if (!selection_.empty()) {
                const auto w = setting_number(std::round(selection_.width * 10) / 10);
                info = L"视频区域  ·  " + w + L" × " + std::to_wstring(pixel(selection_.height)) + L" px";
            }
        }
        draw_text(dc, info, info_area_, ink);
    }

    CalibrationKind kind_;
    HWND window_{};
    HFONT font_{};
    cv::Mat pixels_;
    cv::Rect2d selection_, previous_;
    cv::Point2d origin_, anchor_;
    POINT pan_anchor_{};
    RECT canvas_{}, zoom_caption_{}, info_area_{};
    double scale_ = 1, fit_scale_ = 1, inset_ = 3;
    bool buffered_paint_ = false;
    bool syncing_circle_ = false;
    PointerAction action_ = PointerAction::none;
    std::wstring notice_;
    std::optional<WindowMinimap> result_;
};

CapturePreview::CapturePreview(HINSTANCE instance, HWND parent, const cv::Mat& image, CalibrationKind kind,
                               const NormalizedRect& video, const std::optional<WindowMinimap>& minimap)
    : impl_(std::make_unique<Impl>(instance, parent, image, kind, video, minimap)) {}

CapturePreview::~CapturePreview() = default;

bool CapturePreview::open() const { return impl_->window_ != nullptr; }

bool CapturePreview::handle_key(MSG& message) const {
    if (!open() || GetAncestor(message.hwnd, GA_ROOT) != impl_->window_) return false;
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
        DestroyWindow(impl_->window_);
        return true;
    }
    return IsDialogMessageW(impl_->window_, &message) != FALSE;
}

CalibrationKind CapturePreview::kind() const { return impl_->kind_; }

const std::optional<WindowMinimap>& CapturePreview::result() const { return impl_->result_; }

}
