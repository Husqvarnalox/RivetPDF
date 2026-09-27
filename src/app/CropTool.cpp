// SPDX-License-Identifier: MPL-2.0
#include "app/CropTool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace rivet::app {
namespace {

constexpr ui::Color kDimColor = ui::Color::rgba(0.0, 0.0, 0.0, 0.35);
constexpr ui::Color kMediaOutline = ui::Color::rgba(0.25, 0.25, 0.25, 0.8);
constexpr ui::Color kCropOutline = ui::Color::rgba(0.20, 0.45, 0.95, 1.0);
constexpr ui::Color kHandleFill = ui::Color::white();
constexpr ui::Color kBarFill = ui::Color::rgba(0.15, 0.15, 0.15, 0.88);
constexpr ui::Color kBarText = ui::Color::white();
constexpr ui::Font kBarFont{12.0, ui::Font::Weight::Semibold};

bool isFiniteRect(const core::Rect& rect) { return rect.isFinite(); }

} // namespace

// --- CropFrame --------------------------------------------------------------

core::Rect CropFrame::mediaRect() const {
    return core::Rect{core::Point{}, pdf::displaySize(uncroppedView())};
}

core::Rect CropFrame::currentCropRect() const {
    return pdf::userBoxToDisplayRect(uncroppedView(), view.cropBox);
}

pdf::PdfBox CropFrame::toUserBox(const core::Rect& uncroppedRect) const {
    const core::Rect clamped = uncroppedRect.intersection(mediaRect());
    return pdf::displayRectToUserBox(uncroppedView(), clamped).intersection(mediaBox);
}

core::Rect CropFrame::fromUserBox(const pdf::PdfBox& box) const {
    return pdf::userBoxToDisplayRect(uncroppedView(), box);
}

// --- CropTool ---------------------------------------------------------------

void CropTool::begin(std::size_t pageIndex, const CropFrame& frame) {
    active_ = true;
    pageIndex_ = pageIndex;
    frame_ = frame;
    base_ = frame_.currentCropRect();
    cropRect_ = base_;
    dragHandle_ = Handle::None;
}

void CropTool::end() {
    active_ = false;
    dragHandle_ = Handle::None;
}

double CropTool::minWidth() const { return std::min(kMinSizePoints, frame_.mediaRect().size.width); }
double CropTool::minHeight() const { return std::min(kMinSizePoints, frame_.mediaRect().size.height); }

void CropTool::setCropRect(const core::Rect& rect) {
    if (!isFiniteRect(rect)) return;
    const core::Rect media = frame_.mediaRect();
    // Normalize, then clamp every edge into the media rect.
    double left = std::min(rect.minX(), rect.maxX());
    double right = std::max(rect.minX(), rect.maxX());
    double top = std::min(rect.minY(), rect.maxY());
    double bottom = std::max(rect.minY(), rect.maxY());
    left = std::clamp(left, media.minX(), media.maxX());
    right = std::clamp(right, media.minX(), media.maxX());
    top = std::clamp(top, media.minY(), media.maxY());
    bottom = std::clamp(bottom, media.minY(), media.maxY());
    // Minimum size: grow right/bottom first, then left/top, within the media.
    if (right - left < minWidth()) {
        right = std::min(media.maxX(), left + minWidth());
        left = right - minWidth();
    }
    if (bottom - top < minHeight()) {
        bottom = std::min(media.maxY(), top + minHeight());
        top = bottom - minHeight();
    }
    cropRect_ = core::Rect{left, top, right - left, bottom - top};
}

std::optional<core::Rect> CropTool::cropRectInViewport(const ui::ViewportToolHost& host) const {
    const std::optional<core::Rect> page = host.pageRectInViewport(pageIndex_);
    if (!page.has_value()) return std::nullopt;
    const double zoom = host.zoomFactor();
    // The rendered page's (0, 0) is base_.origin in uncropped space.
    return core::Rect{page->origin + (cropRect_.origin - base_.origin) * zoom, cropRect_.size * zoom};
}

std::optional<core::Point> CropTool::toUncropped(const ui::ViewportToolHost& host,
                                                 const core::Point& viewportPoint) const {
    const std::optional<core::Rect> page = host.pageRectInViewport(pageIndex_);
    const double zoom = host.zoomFactor();
    if (!page.has_value() || !(zoom > 0.0)) return std::nullopt;
    return (viewportPoint - page->origin) / zoom + base_.origin;
}

// Priority: corners, then edge midpoints, then the edge bands, then the
// interior (move). All distances in logical viewport points.
CropTool::Handle CropTool::handleAt(const ui::ViewportToolHost& host, const core::Point& point) const {
    if (!active_) return Handle::None;
    const std::optional<core::Rect> rect = cropRectInViewport(host);
    if (!rect.has_value()) return Handle::None;
    const double r = kHandleHitRadius;
    const auto near = [r](const core::Point& a, double x, double y) {
        return std::abs(a.x - x) <= r && std::abs(a.y - y) <= r;
    };
    const double x0 = rect->minX();
    const double x1 = rect->maxX();
    const double y0 = rect->minY();
    const double y1 = rect->maxY();
    const double xm = (x0 + x1) / 2.0;
    const double ym = (y0 + y1) / 2.0;
    if (near(point, x0, y0)) return Handle::TopLeft;
    if (near(point, x1, y0)) return Handle::TopRight;
    if (near(point, x1, y1)) return Handle::BottomRight;
    if (near(point, x0, y1)) return Handle::BottomLeft;
    if (near(point, xm, y0)) return Handle::Top;
    if (near(point, x1, ym)) return Handle::Right;
    if (near(point, xm, y1)) return Handle::Bottom;
    if (near(point, x0, ym)) return Handle::Left;
    const bool withinX = point.x >= x0 - r && point.x <= x1 + r;
    const bool withinY = point.y >= y0 - r && point.y <= y1 + r;
    if (withinY && std::abs(point.x - x0) <= r) return Handle::Left;
    if (withinY && std::abs(point.x - x1) <= r) return Handle::Right;
    if (withinX && std::abs(point.y - y0) <= r) return Handle::Top;
    if (withinX && std::abs(point.y - y1) <= r) return Handle::Bottom;
    if (rect->contains(point)) return Handle::Move;
    return Handle::None;
}

core::Rect CropTool::draggedRect(Handle handle, const core::Rect& start, const core::Point& delta) const {
    const core::Rect media = frame_.mediaRect();
    if (handle == Handle::Move) {
        const double x = std::clamp(start.minX() + delta.x, media.minX(),
                                    std::max(media.minX(), media.maxX() - start.size.width));
        const double y = std::clamp(start.minY() + delta.y, media.minY(),
                                    std::max(media.minY(), media.maxY() - start.size.height));
        return core::Rect{x, y, start.size.width, start.size.height};
    }
    const bool left = handle == Handle::TopLeft || handle == Handle::Left || handle == Handle::BottomLeft;
    const bool right = handle == Handle::TopRight || handle == Handle::Right || handle == Handle::BottomRight;
    const bool top = handle == Handle::TopLeft || handle == Handle::Top || handle == Handle::TopRight;
    const bool bottom =
        handle == Handle::BottomLeft || handle == Handle::Bottom || handle == Handle::BottomRight;
    double x0 = start.minX();
    double x1 = start.maxX();
    double y0 = start.minY();
    double y1 = start.maxY();
    if (left) x0 = std::clamp(x0 + delta.x, media.minX(), std::max(media.minX(), x1 - minWidth()));
    if (right) x1 = std::clamp(x1 + delta.x, std::min(media.maxX(), x0 + minWidth()), media.maxX());
    if (top) y0 = std::clamp(y0 + delta.y, media.minY(), std::max(media.minY(), y1 - minHeight()));
    if (bottom) y1 = std::clamp(y1 + delta.y, std::min(media.maxY(), y0 + minHeight()), media.maxY());
    return core::Rect{x0, y0, x1 - x0, y1 - y0};
}

CropTool::BarRects CropTool::barRects(const ui::ViewportToolHost& host) {
    const core::Rect bounds = host.viewportBounds();
    const double width = 3.0 * kBarButtonWidth + 4.0 * 6.0;
    const core::Rect bar{bounds.minX() + (bounds.size.width - width) / 2.0, bounds.minY() + 10.0, width,
                         kBarHeight};
    const auto button = [&](int i) {
        return core::Rect{bar.minX() + 6.0 + static_cast<double>(i) * (kBarButtonWidth + 6.0),
                          bar.minY() + 3.0, kBarButtonWidth, kBarHeight - 6.0};
    };
    return BarRects{bar, button(0), button(1), button(2)};
}

void CropTool::apply() {
    if (!active_) return;
    const pdf::PdfBox box = userCropBox();
    if (callbacks_.onApply) {
        auto callback = callbacks_.onApply; // the owner may end the tool
        callback(box);
    }
}

void CropTool::reset() {
    if (!active_) return;
    if (callbacks_.onReset) {
        auto callback = callbacks_.onReset;
        callback();
    }
}

void CropTool::cancel() {
    if (!active_) return;
    end();
    if (callbacks_.onCancel) {
        auto callback = callbacks_.onCancel;
        callback();
    }
}

// Modal while active: every button press/drag/release is consumed (text
// selection and links are suspended); hover moves pass through.
bool CropTool::onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) {
    if (!active_) return false;
    switch (event.type) {
    case ui::PointerEventType::Down: {
        if (event.button != 1) return true;
        const BarRects bar = barRects(host);
        if (bar.bar.contains(event.position)) {
            if (bar.apply.contains(event.position)) {
                apply();
            } else if (bar.reset.contains(event.position)) {
                reset();
            } else if (bar.cancel.contains(event.position)) {
                cancel();
            }
            return true;
        }
        const Handle handle = handleAt(host, event.position);
        const std::optional<core::Point> point = toUncropped(host, event.position);
        if (handle != Handle::None && point.has_value()) {
            dragHandle_ = handle;
            dragStartRect_ = cropRect_;
            dragStartPoint_ = *point;
        }
        return true;
    }
    case ui::PointerEventType::Move: {
        if (dragHandle_ == Handle::None) return event.button != 0;
        if (event.button != 1) { // released outside the view
            dragHandle_ = Handle::None;
            return false;
        }
        const std::optional<core::Point> point = toUncropped(host, event.position);
        if (point.has_value()) {
            cropRect_ = draggedRect(dragHandle_, dragStartRect_, *point - dragStartPoint_);
            host.requestRepaint();
        }
        return true;
    }
    case ui::PointerEventType::Up:
        dragHandle_ = Handle::None;
        host.requestRepaint();
        return true;
    default:
        return false;
    }
}

bool CropTool::onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) {
    if (!active_) return false;
    if (event.key == ui::Key::Enter) {
        apply();
        host.requestRepaint();
        return true;
    }
    if (event.key == ui::Key::Escape) {
        if (dragHandle_ != Handle::None) {
            // First Esc aborts the drag in progress.
            cropRect_ = dragStartRect_;
            dragHandle_ = Handle::None;
        } else {
            cancel();
        }
        host.requestRepaint();
        return true;
    }
    return false;
}

void CropTool::paint(const ui::ViewportToolHost& host, ui::PaintContext& context) const {
    if (!active_) return;
    const std::optional<core::Rect> page = host.pageRectInViewport(pageIndex_);
    if (!page.has_value()) return;
    const double zoom = host.zoomFactor();
    const auto toViewport = [&](const core::Rect& r) {
        return core::Rect{page->origin + (r.origin - base_.origin) * zoom, r.size * zoom};
    };
    const core::Rect media = toViewport(frame_.mediaRect());
    const core::Rect crop = toViewport(cropRect_);

    // Dim the media area outside the crop (four bands), outline both.
    context.fillRect(core::Rect{media.minX(), media.minY(), media.size.width,
                                std::max(0.0, crop.minY() - media.minY())},
                     kDimColor);
    context.fillRect(core::Rect{media.minX(), crop.maxY(), media.size.width,
                                std::max(0.0, media.maxY() - crop.maxY())},
                     kDimColor);
    context.fillRect(core::Rect{media.minX(), crop.minY(), std::max(0.0, crop.minX() - media.minX()),
                                crop.size.height},
                     kDimColor);
    context.fillRect(core::Rect{crop.maxX(), crop.minY(), std::max(0.0, media.maxX() - crop.maxX()),
                                crop.size.height},
                     kDimColor);
    context.strokeRect(media, kMediaOutline, 1.0);
    context.strokeRect(crop, kCropOutline, 2.0);

    const double half = kHandleSize / 2.0;
    const double xm = (crop.minX() + crop.maxX()) / 2.0;
    const double ym = (crop.minY() + crop.maxY()) / 2.0;
    const std::array<core::Point, 8> handles{
        core::Point{crop.minX(), crop.minY()}, core::Point{xm, crop.minY()},
        core::Point{crop.maxX(), crop.minY()}, core::Point{crop.maxX(), ym},
        core::Point{crop.maxX(), crop.maxY()}, core::Point{xm, crop.maxY()},
        core::Point{crop.minX(), crop.maxY()}, core::Point{crop.minX(), ym},
    };
    for (const core::Point& center : handles) {
        const core::Rect square{center.x - half, center.y - half, kHandleSize, kHandleSize};
        context.fillRect(square, kHandleFill);
        context.strokeRect(square, kCropOutline, 1.0);
    }

    const BarRects bar = barRects(host);
    context.fillRoundedRect(bar.bar, kBarFill, 6.0);
    context.drawText("Apply", bar.apply, kBarFont, kBarText, ui::TextAlign::Center);
    context.drawText("Reset", bar.reset, kBarFont, kBarText, ui::TextAlign::Center);
    context.drawText("Cancel", bar.cancel, kBarFont, kBarText, ui::TextAlign::Center);
}

} // namespace rivet::app
