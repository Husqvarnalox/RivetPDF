#include "render/PageLayout.hpp"

#include <algorithm>
#include <cassert>

namespace rivet::render {

void PageLayout::setPages(std::vector<PageInfo> pages) {
    pages_ = std::move(pages);
    rebuild();
}

void PageLayout::setPageGapPoints(double gap) {
    gap_ = std::max(0.0, gap);
    rebuild();
}

void PageLayout::setPageMarginPoints(double margin) {
    margin_ = std::max(0.0, margin);
    rebuild();
}

void PageLayout::rebuild() {
    frames_.clear();
    contentSize_ = core::Size{};
    if (pages_.empty()) return;

    double widest = 0.0;
    double totalHeight = 0.0;
    for (const PageInfo& info : pages_) {
        widest = std::max(widest, info.sizePoints.width);
        totalHeight += info.sizePoints.height;
    }
    contentSize_ = core::Size{widest + 2.0 * margin_,
                              totalHeight + gap_ * static_cast<double>(pages_.size() - 1) + 2.0 * margin_};

    frames_.reserve(pages_.size());
    double y = margin_;
    for (const PageInfo& info : pages_) {
        const double x = margin_ + (widest - info.sizePoints.width) / 2.0;
        frames_.emplace_back(core::Point{x, y}, info.sizePoints);
        y += info.sizePoints.height + gap_;
    }
}

core::Rect PageLayout::pageFramePoints(std::size_t index) const {
    assert(index < frames_.size());
    return frames_[index];
}

std::optional<std::size_t> PageLayout::pageIndexAt(const core::Point& contentPoint) const {
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].contains(contentPoint)) return i;
    }
    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>>
PageLayout::visiblePageRange(const core::Rect& contentRectPoints) const {
    std::optional<std::size_t> first;
    std::size_t last = 0;
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].intersects(contentRectPoints)) {
            if (!first) first = i;
            last = i;
        }
    }
    if (!first) return std::nullopt;
    return std::pair<std::size_t, std::size_t>{*first, last};
}

double PageLayout::pageTopOffsetPoints(std::size_t index) const {
    assert(index < frames_.size());
    return frames_[index].origin.y;
}

std::optional<std::size_t> PageLayout::currentPageIndex(const core::Rect& contentRectPoints) const {
    if (pages_.empty()) return std::nullopt;

    // 1. Page containing the viewport center.
    const std::optional<std::size_t> atCenter = pageIndexAt(contentRectPoints.center());
    if (atCenter.has_value()) return atCenter;

    // 2. Largest visible area among pages intersecting the rect (ties ->
    //    lower index). Intersection of an intersecting page and the rect has
    //    positive area by visiblePageRange's contract, so the maximum is a
    //    real page, not a degenerate sliver.
    const std::optional<std::pair<std::size_t, std::size_t>> range =
        visiblePageRange(contentRectPoints);
    if (range.has_value()) {
        std::size_t best = range->first;
        double bestArea = 0.0;
        for (std::size_t i = range->first; i <= range->second; ++i) {
            const double area = frames_[i].intersection(contentRectPoints).area();
            if (area > bestArea) {
                bestArea = area;
                best = i;
            }
        }
        return best;
    }

    // 3. Rect intersects no page: nearest end page (first when above the
    //    column, last when below).
    if (contentRectPoints.maxY() <= frames_.front().origin.y) return 0;
    return frames_.size() - 1;
}

} // namespace rivet::render
