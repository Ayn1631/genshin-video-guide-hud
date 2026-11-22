#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native_hud.hpp"

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace guide {
namespace {

using namespace Gdiplus;

constexpr int compass_width = 760;
constexpr int compass_height = 134;
constexpr int marker_size = 25;
constexpr int diagnostics_width = 312;
constexpr REAL diagnostics_padding = 12.0f;
constexpr REAL diagnostics_text_width = diagnostics_width - 2 * diagnostics_padding;
constexpr REAL diagnostics_heading_height = 24.0f;
constexpr REAL diagnostics_section_gap = 13.0f;
constexpr double radians_per_degree = 3.14159265358979323846 / 180.0;

[[noreturn]] void throw_last_error(const char* message) {
    const DWORD error = GetLastError();
    throw std::system_error(static_cast<int>(error), std::system_category(), message);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

class GdiPlus {
public:
    GdiPlus() {
        GdiplusStartupInput input;
        if (GdiplusStartup(&token_, &input, nullptr) != Ok) {
            throw std::runtime_error("Could not initialize GDI+");
        }
    }

    ~GdiPlus() { GdiplusShutdown(token_); }
    GdiPlus(const GdiPlus&) = delete;
    GdiPlus& operator=(const GdiPlus&) = delete;

private:
    ULONG_PTR token_ = 0;
};

class WindowClass {
public:
    explicit WindowClass(HINSTANCE instance)
        : instance_(instance ? instance : GetModuleHandleW(nullptr)),
          name_(L"BetterGIVideoGuideNative_" + std::to_wstring(reinterpret_cast<ULONG_PTR>(this))) {
        WNDCLASSW description{};
        description.lpfnWndProc = window_proc;
        description.hInstance = instance_;
        description.lpszClassName = name_.c_str();
        if (!RegisterClassW(&description)) {
            throw_last_error("Could not register the HUD window class");
        }
    }

    ~WindowClass() { UnregisterClassW(name_.c_str(), instance_); }
    WindowClass(const WindowClass&) = delete;
    WindowClass& operator=(const WindowClass&) = delete;

    HWND create() const {
        constexpr DWORD style = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                WS_EX_LAYERED | WS_EX_NOACTIVATE;
        HWND window = CreateWindowExW(style, name_.c_str(), L"攻略方向指引", WS_POPUP,
                                      0, 0, 0, 0, nullptr, nullptr, instance_, nullptr);
        if (!window) {
            throw_last_error("Could not create a HUD window");
        }
        return window;
    }

private:
    HINSTANCE instance_;
    std::wstring name_;
};

class Surface {
public:
    Surface() = default;
    ~Surface() { release(); }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    bool matches(int width, int height) const noexcept {
        return dc_ && width_ == width && height_ == height;
    }

    void resize(int width, int height) {
        release();
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) {
            throw std::runtime_error("Could not create a HUD device context");
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap_ || !pixels) {
            release();
            throw std::runtime_error("Could not allocate the HUD bitmap");
        }
        previous_ = SelectObject(dc_, bitmap_);
        if (!previous_ || previous_ == HGDI_ERROR) {
            previous_ = nullptr;
            release();
            throw std::runtime_error("Could not select the HUD bitmap");
        }
        pixels_ = static_cast<BYTE*>(pixels);
        width_ = width;
        height_ = height;
    }

    template <typename Content>
    void draw(const Content& content, void (*paint)(Graphics&, int, int, const Content&)) {
        std::memset(pixels_, 0, static_cast<std::size_t>(width_) * height_ * 4);
        // UpdateLayeredWindow reads premultiplied BGRA directly from the DIB.
        Bitmap bitmap(width_, height_, width_ * 4, PixelFormat32bppPARGB, pixels_);
        Graphics graphics(&bitmap);
        if (bitmap.GetLastStatus() != Ok || graphics.GetLastStatus() != Ok) {
            throw std::runtime_error("Could not draw to the HUD bitmap");
        }
        graphics.SetPageUnit(UnitPixel);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        paint(graphics, width_, height_, content);
        graphics.Flush(FlushIntentionSync);
    }

    void upload(HWND window, const RECT& bounds) const {
        POINT destination{bounds.left, bounds.top};
        SIZE dimensions{width_, height_};
        POINT origin{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(window, nullptr, &destination, &dimensions,
                                 dc_, &origin, 0, &blend, ULW_ALPHA)) {
            throw_last_error("Could not update a HUD window");
        }
    }

private:
    void release() noexcept {
        if (previous_) {
            SelectObject(dc_, previous_);
            previous_ = nullptr;
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
        if (dc_) {
            DeleteDC(dc_);
            dc_ = nullptr;
        }
        pixels_ = nullptr;
    }

    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    BYTE* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

template <typename Content>
class Layer {
public:
    explicit Layer(const WindowClass& window_class) : window_(window_class.create()) {}
    ~Layer() { DestroyWindow(window_); }
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    HWND window() const noexcept { return window_; }

    void show(const RECT& bounds, const Content& content, void (*paint)(Graphics&, int, int, const Content&)) {
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const bool resized = !surface_.matches(width, height);
        const bool changed = resized || !content_ || !(*content_ == content);
        if (changed) {
            content_.reset();
            bounds_.reset();
            if (resized) {
                surface_.resize(width, height);
            }
            surface_.draw(content, paint);
            content_.emplace(content);
        }
        if (!bounds_ || !EqualRect(&*bounds_, &bounds)) {
            surface_.upload(window_, bounds);
            bounds_ = bounds;
        }
        if (!visible_) {
            if (!SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
                throw_last_error("Could not show a HUD window");
            }
            visible_ = true;
        }
    }

    void hide() noexcept {
        if (visible_) {
            ShowWindow(window_, SW_HIDE);
            visible_ = false;
        }
    }

private:
    HWND window_;
    Surface surface_;
    std::optional<Content> content_;
    std::optional<RECT> bounds_;
    bool visible_ = false;
};

struct CompassContent {
    std::wstring title;
    std::wstring detail;
    std::wstring distance;
    std::optional<double> offset;

    bool operator==(const CompassContent& other) const noexcept {
        return title == other.title && detail == other.detail &&
               distance == other.distance && offset == other.offset;
    }
};

struct MarkerContent {
    POINT center;
    std::optional<double> heading;

    bool operator==(const MarkerContent& other) const noexcept {
        return center.x == other.center.x && center.y == other.center.y && heading == other.heading;
    }
};

bool finite(const std::optional<double>& value) {
    return value && std::isfinite(*value);
}

std::wstring format_distance(const std::optional<double>& distance) {
    if (!finite(distance)) {
        return {};
    }
    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << L"距 " << std::fixed << std::setprecision(1) << *distance;
    return text.str();
}

void text_format(StringFormat& format, StringAlignment alignment) {
    format.SetAlignment(alignment);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
}

void draw_text(Graphics& graphics, const std::wstring& text, const Font& font, RectF bounds,
               StringAlignment alignment, Color color, bool shadow = false) {
    if (text.empty() || bounds.Width <= 0 || bounds.Height <= 0) {
        return;
    }
    StringFormat format;
    text_format(format, alignment);
    if (shadow) {
        SolidBrush shadow_brush(Color(180, 13, 28, 47));
        RectF shadow_bounds = bounds;
        shadow_bounds.Offset(0.8f, 1.0f);
        graphics.DrawString(text.c_str(), -1, &font, shadow_bounds, &format, &shadow_brush);
    }
    SolidBrush brush(color);
    graphics.DrawString(text.c_str(), -1, &font, bounds, &format, &brush);
}

template <std::size_t N>
void fill_polygon(Graphics& graphics, const PointF (&points)[N], Color color) {
    SolidBrush brush(color);
    graphics.FillPolygon(&brush, points, static_cast<int>(N));
}

template <std::size_t N>
void outline_polygon(Graphics& graphics, const PointF (&points)[N], Color color, REAL width) {
    Pen pen(color, width);
    pen.SetLineJoin(LineJoinRound);
    graphics.DrawPolygon(&pen, points, static_cast<int>(N));
}

REAL curve(REAL x) {
    const REAL distance = (x - 380.0f) / 362.0f;
    return 106.0f - 41.0f * distance * distance;
}

REAL angle_x(double angle) {
    return 380.0f + static_cast<REAL>(320.0 * angle / 90.0);
}

void paint_compass_offset(Graphics& graphics, double angle) {
    const REAL x = angle_x(std::clamp(angle, -90.0, 90.0));
    const REAL y = curve(x);
    PointF marker[] = {{x, y - 13.0f}, {x + 9.0f, y}, {x, y + 13.0f}, {x - 9.0f, y}};
    if (std::abs(angle) > 90.0) {
        const REAL side = angle < 0.0 ? -1.0f : 1.0f;
        marker[0] = PointF(x - side * 7.0f, y - 10.0f);
        marker[1] = PointF(x + side * 7.0f, y);
        marker[2] = PointF(x - side * 7.0f, y + 10.0f);
        marker[3] = PointF(x - side * 2.0f, y);
    }
    outline_polygon(graphics, marker, Color(32, 255, 69, 78), 15.0f);
    outline_polygon(graphics, marker, Color(68, 255, 69, 78), 8.0f);
    fill_polygon(graphics, marker, Color(255, 255, 245, 241));
    outline_polygon(graphics, marker, Color(255, 255, 92, 96), 2.4f);
}

void paint_compass(Graphics& graphics, int width, int, const CompassContent& content) {
    const auto scale = static_cast<REAL>(width) / compass_width;
    graphics.ScaleTransform(scale, scale);

    const Color arc_colors[] = {Color(0, 216, 238, 255), Color(180, 216, 238, 255),
                                Color(240, 216, 238, 255), Color(180, 216, 238, 255),
                                Color(0, 216, 238, 255)};
    const REAL arc_stops[] = {0, 0.25f, 0.5f, 0.75f, 1};
    LinearGradientBrush gradient(PointF(18, 65), PointF(742, 65), arc_colors[0], arc_colors[4]);
    gradient.SetInterpolationColors(arc_colors, arc_stops, 5);
    Pen arc(&gradient, 2.2f);
    graphics.DrawBezier(&arc, 18.0f, 65.0f, 259.33f, 119.67f,
                        500.67f, 119.67f, 742.0f, 65.0f);

    Pen tick(Color(206, 234, 247, 255), 2.0f);
    for (const int angle : {-90, -60, -30, 30, 60, 90}) {
        const REAL x = angle_x(angle);
        const REAL y = curve(x);
        const REAL lean = (380.0f - x) / 210.0f;
        graphics.DrawLine(&tick, x + lean, y - 5.0f, x - lean, y + 5.0f);
    }
    const PointF pointer[] = {{373, 72}, {387, 72}, {380, 82}};
    const PointF center[] = {{380, 97}, {384, 106}, {380, 115}, {376, 106}};
    fill_polygon(graphics, pointer, Color(250, 249, 253, 255));
    fill_polygon(graphics, center, Color(233, 247, 253, 255));
    if (content.offset) {
        paint_compass_offset(graphics, *content.offset);
    }

    const bool angle_title = !content.title.empty() && content.title.back() == L'°';
    const FontFamily chinese(L"Microsoft YaHei UI");
    const FontFamily segoe(L"Segoe UI");
    const FontFamily* text_family = chinese.IsAvailable() ? &chinese : &segoe;
    const Font title_font(angle_title ? &segoe : text_family,
                          angle_title ? 36.0f : 20.0f, FontStyleRegular, UnitPixel);
    const Font detail_font(text_family, 15.0f, FontStyleRegular, UnitPixel);
    const auto title_width = content.distance.empty() ? 736.0f : 500.0f;
    const RectF title_bounds(380.0f - title_width / 2, 0, title_width, 50);
    draw_text(graphics, content.title, title_font, title_bounds, StringAlignmentCenter,
              Color(255, 250, 253, 255), true);
    if (!content.distance.empty()) {
        REAL title_right = 380.0f;
        if (!content.title.empty()) {
            StringFormat format;
            text_format(format, StringAlignmentCenter);
            RectF text_bounds;
            if (graphics.MeasureString(content.title.c_str(), -1, &title_font, title_bounds, &format,
                                       &text_bounds) == Ok) {
                title_right = text_bounds.GetRight();
            }
        }
        const REAL left = title_right + 10.0f;
        const Font distance_font(text_family, 18.0f, FontStyleRegular, UnitPixel);
        draw_text(graphics, content.distance, distance_font, RectF(left, 0, 752.0f - left, 50),
                  StringAlignmentNear, Color(244, 237, 248, 255), true);
    }
    draw_text(graphics, content.detail, detail_font, RectF(12, 42, 736, 24), StringAlignmentCenter,
              Color(244, 237, 248, 255), true);
}

void paint_marker(Graphics& graphics, int, int, const MarkerContent& content) {
    const REAL x = static_cast<REAL>(content.center.x) + 0.5f;
    const REAL y = static_cast<REAL>(content.center.y) + 0.5f;
    const Color red(255, 254, 62, 78);
    const Color border(255, 255, 237, 233);
    const Color shadow(210, 24, 18, 26);
    if (content.heading) {
        const double radians = *content.heading * radians_per_degree;
        const auto dx = static_cast<REAL>(std::sin(radians));
        const auto dy = static_cast<REAL>(-std::cos(radians));
        const REAL base_x = x + 4.5f * dx;
        const REAL base_y = y + 4.5f * dy;
        const PointF arrow[] = {
            {x + 10.5f * dx, y + 10.5f * dy},
            {base_x - 3.5f * dy, base_y + 3.5f * dx},
            {base_x + 3.5f * dy, base_y - 3.5f * dx},
        };
        outline_polygon(graphics, arrow, shadow, 2.5f);
        fill_polygon(graphics, arrow, red);
        outline_polygon(graphics, arrow, border, 1.0f);
    }
    SolidBrush shadow_brush(shadow);
    SolidBrush red_brush(red);
    Pen border_pen(border, 1.0f);
    graphics.FillEllipse(&shadow_brush, x - 5.5f, y - 5.5f, 11.0f, 11.0f);
    graphics.FillEllipse(&red_brush, x - 4.0f, y - 4.0f, 8.0f, 8.0f);
    graphics.DrawEllipse(&border_pen, x - 4.0f, y - 4.0f, 8.0f, 8.0f);
}

struct DiagnosticsText {
    const FontFamily chinese{L"Microsoft YaHei UI"};
    const FontFamily segoe{L"Segoe UI"};
    const FontFamily* family = chinese.IsAvailable() ? &chinese : &segoe;
    const Font heading{family, 15.0f, FontStyleBold, UnitPixel};
    const Font body{family, 13.0f, FontStyleRegular, UnitPixel};
    StringFormat format;

    DiagnosticsText() { format.SetTrimming(StringTrimmingNone); }
};

struct DiagnosticsLayout {
    std::vector<DiagnosticSection> sections;
    std::vector<REAL> text_heights;
    REAL height = 2 * diagnostics_padding;

    bool operator==(const DiagnosticsLayout& other) const noexcept {
        return sections == other.sections;
    }
};

DiagnosticsLayout measure_diagnostics(const std::vector<DiagnosticSection>& sections) {
    Bitmap bitmap(1, 1, PixelFormat32bppPARGB);
    Graphics graphics(&bitmap);
    graphics.SetPageUnit(UnitPixel);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    const DiagnosticsText style;
    DiagnosticsLayout layout;
    layout.sections = sections;
    layout.text_heights.reserve(sections.size());
    for (const auto& section : sections) {
        const RectF line_area(0, 0, diagnostics_text_width, 0);
        RectF text_bounds;
        if (graphics.MeasureString(section.text.c_str(), -1, &style.body, line_area,
                                   &style.format, &text_bounds) != Ok) {
            throw std::runtime_error("Could not measure HUD diagnostics");
        }
        const REAL text_height = std::ceil(text_bounds.Height) + 1.0f;
        layout.text_heights.push_back(text_height);
        layout.height += diagnostics_heading_height + text_height;
    }
    if (!sections.empty()) {
        layout.height += diagnostics_section_gap * static_cast<REAL>(sections.size() - 1);
    }
    return layout;
}

void paint_diagnostics(Graphics& graphics, int width, int height, const DiagnosticsLayout& layout) {
    const auto scale = static_cast<REAL>(width) / diagnostics_width;
    graphics.ScaleTransform(scale, scale);
    const REAL right = diagnostics_width - 1.0f;
    const REAL bottom = static_cast<REAL>(height) / scale - 1.0f;
    const REAL corner = std::min(16.0f, bottom - 1.0f);
    GraphicsPath path;
    path.AddArc(1.0f, 1.0f, corner, corner, 180.0f, 90.0f);
    path.AddArc(right - corner, 1.0f, corner, corner, 270.0f, 90.0f);
    path.AddArc(right - corner, bottom - corner, corner, corner, 0.0f, 90.0f);
    path.AddArc(1.0f, bottom - corner, corner, corner, 90.0f, 90.0f);
    path.CloseFigure();
    SolidBrush background(Color(221, 15, 25, 39));
    Pen border(Color(105, 157, 194, 224), 1.0f);
    graphics.FillPath(&background, &path);
    graphics.DrawPath(&border, &path);

    const DiagnosticsText style;
    const SolidBrush heading(Color(255, 174, 216, 250));
    const SolidBrush body(Color(245, 228, 241, 255));
    const Pen divider(Color(82, 157, 194, 224), 1.0f);
    auto y = diagnostics_padding;
    for (std::size_t i = 0; i < layout.sections.size(); ++i) {
        if (i != 0) {
            const REAL separator = y + diagnostics_section_gap / 2;
            graphics.DrawLine(&divider, diagnostics_padding, separator,
                              diagnostics_padding + diagnostics_text_width, separator);
            y += diagnostics_section_gap;
        }
        const auto& section = layout.sections[i];
        graphics.DrawString(section.title.c_str(), -1, &style.heading,
                            RectF(diagnostics_padding, y, diagnostics_text_width, diagnostics_heading_height),
                            &style.format, &heading);
        y += diagnostics_heading_height;
        const auto text_height = layout.text_heights[i];
        graphics.DrawString(section.text.c_str(), -1, &style.body,
                            RectF(diagnostics_padding, y, diagnostics_text_width, text_height),
                            &style.format, &body);
        y += text_height;
    }
}

std::optional<RECT> diagnostics_bounds(const RECT& game, int width, int height, REAL content_height) {
    const int margin = std::max(8, static_cast<int>(std::lround(height * 0.014)));
    const int offset = static_cast<int>(std::lround(height * 0.23));
    const int available_height = height - offset - margin;
    const double available_width = static_cast<double>(width - 2 * margin) / diagnostics_width;
    const double scale = std::min({1.3, std::max(0.7, height / 950.0), available_width,
                                  static_cast<double>(available_height) / content_height});
    const int panel_width = static_cast<int>(std::floor(diagnostics_width * scale));
    if (panel_width <= 0) {
        return std::nullopt;
    }
    const int panel_height = std::min(available_height, static_cast<int>(std::ceil(
        static_cast<double>(panel_width) * content_height / diagnostics_width)));
    return RECT{game.left + margin, game.top + offset, game.left + margin + panel_width,
                game.top + offset + panel_height};
}

}  // namespace

struct Hud::Impl {
    explicit Impl(HINSTANCE instance)
        : window_class(instance), compass(window_class), marker(window_class), diagnostics(window_class) {}

    GdiPlus gdiplus;
    WindowClass window_class;
    Layer<CompassContent> compass;
    Layer<MarkerContent> marker;
    Layer<DiagnosticsLayout> diagnostics;
    std::optional<DiagnosticsLayout> diagnostics_layout;
};

Hud::Hud(HINSTANCE instance) : impl_(std::make_unique<Impl>(instance)) {}
Hud::~Hud() = default;

std::vector<HWND> Hud::windows() const {
    return {impl_->compass.window(), impl_->marker.window(), impl_->diagnostics.window()};
}

void Hud::show(const RECT& game, const HudView& view) {
    const auto width = static_cast<long long>(game.right) - game.left;
    const auto height = static_cast<long long>(game.bottom) - game.top;
    if (width < 2 || height < 2 || width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max()) {
        hide();
        return;
    }
    const int top_padding = static_cast<int>(std::lround(height * 0.008));
    const int max_width = static_cast<int>(std::min(1020LL, (height - top_padding) * compass_width / compass_height));
    const int render_width = std::clamp(static_cast<int>(std::lround(width * 0.53)), 1, max_width);
    const int render_height = std::max(1, static_cast<int>(std::lround(
        static_cast<double>(render_width) * compass_height / compass_width)));
    const LONG left = game.left + (static_cast<LONG>(width) - render_width) / 2;
    const LONG top = game.top + top_padding;
    const RECT compass_rect{left, top, left + render_width, top + render_height};
    std::optional<double> angle;
    if (finite(view.offset)) {
        angle = std::clamp(std::round(*view.offset), -180.0, 180.0);
    }

    try {
        impl_->compass.show(compass_rect, {view.title, view.detail, format_distance(view.distance), angle},
                            paint_compass);
        if (view.marker && PtInRect(&game, *view.marker)) {
            const LONG x = view.marker->x;
            const LONG y = view.marker->y;
            const RECT marker_rect{
                x - std::min<LONG>(marker_size / 2, x - game.left),
                y - std::min<LONG>(marker_size / 2, y - game.top),
                x + std::min<LONG>(marker_size / 2 + 1, game.right - x),
                y + std::min<LONG>(marker_size / 2 + 1, game.bottom - y),
            };
            std::optional<double> heading;
            if (finite(view.marker_heading)) {
                heading = std::round(std::fmod(*view.marker_heading, 360.0) / 5.0) * 5.0;
            }
            impl_->marker.show(marker_rect, {{x - marker_rect.left, y - marker_rect.top}, heading}, paint_marker);
        } else {
            impl_->marker.hide();
        }

        if (view.diagnostics.empty()) {
            impl_->diagnostics.hide();
            impl_->diagnostics_layout.reset();
        } else {
            auto& layout = impl_->diagnostics_layout;
            if (!layout || layout->sections != view.diagnostics) {
                layout = measure_diagnostics(view.diagnostics);
            }
            const auto timing_rect = diagnostics_bounds(game, static_cast<int>(width), static_cast<int>(height),
                                                        layout->height);
            if (timing_rect) {
                impl_->diagnostics.show(*timing_rect, *layout, paint_diagnostics);
            } else {
                impl_->diagnostics.hide();
            }
        }
    } catch (...) {
        hide();
        throw;
    }
}

void Hud::hide() noexcept {
    impl_->compass.hide();
    impl_->marker.hide();
    impl_->diagnostics.hide();
}

}  // namespace guide
