// SPDX-License-Identifier: MPL-2.0
#include "app/TextInteractionController.hpp"

#include "app/UrlPolicy.hpp"
#include "core/Log.hpp"
#include "editor/SelectionText.hpp"
#include "ui/PdfViewport.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace rivet::app {
namespace {

constexpr ui::Color kSelectionHighlight = ui::Color::rgba(0.30, 0.55, 1.0, 0.35);
constexpr ui::Color kSearchMatch = ui::Color::rgba(1.0, 0.85, 0.25, 0.45);
constexpr ui::Color kActiveSearchMatch = ui::Color::rgba(1.0, 0.60, 0.05, 0.65);

} // namespace

void TextInteractionController::bindTab(DocumentTab& tab) {
    tab.selection().setCallback([this] { context_.viewport.invalidate(); });
}

void TextInteractionController::warmPage(std::size_t pageIndex) {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return;
    const core::PageId pageId = tab->session()->pageId(pageIndex);
    tab->session()->textService().ensureTextPage(pageId);
    tab->session()->linkService().ensurePageLinks(pageId);
}

std::optional<std::uint32_t> TextInteractionController::charIndexAtPoint(std::size_t pageIndex,
                                                                        const core::Point& pagePoint) {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return std::nullopt;
    const std::shared_ptr<const pdf::PdfTextPage> page =
        tab->session()->textService().cachedTextPage(tab->session()->pageId(pageIndex));
    if (page == nullptr) return std::nullopt;
    return page->charIndexAtPoint(pagePoint);
}

void TextInteractionController::selectionDragBegan(std::size_t pageIndex, std::uint32_t charIndex,
                                                   bool shiftHeld) {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return;
    const editor::TextPosition position{tab->session()->pageId(pageIndex), charIndex};
    if (shiftHeld) {
        tab->selection().extendTo(position);
    } else {
        tab->selection().start(position);
    }
}

void TextInteractionController::selectionDragMoved(std::size_t pageIndex, std::uint32_t charIndex) {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return;
    tab->selection().setFocus(editor::TextPosition{tab->session()->pageId(pageIndex), charIndex});
}

void TextInteractionController::selectionDragEnded() {}

void TextInteractionController::selectionCleared() {
    if (DocumentTab* tab = context_.readyActiveTab(); tab != nullptr) tab->selection().clear();
}

std::optional<ui::ViewerLinkHit> TextInteractionController::linkAtPoint(std::size_t pageIndex,
                                                                       const core::Point& pagePoint) {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return std::nullopt;
    // Cached links only: the page's links load asynchronously (warmed on
    // page tracking); a cold page reports no links rather than blocking.
    const std::vector<pdf::PdfPageLink> links =
        tab->session()->linkService().cachedLinks(tab->session()->pageId(pageIndex));
    for (const pdf::PdfPageLink& link : links) {
        for (const core::Rect& rect : link.rects) {
            if (!rect.contains(pagePoint)) continue;
            ui::ViewerLinkHit hit;
            hit.kind = link.kind == pdf::PdfPageLink::Kind::Internal ? ui::ViewerLinkHit::Kind::Internal
                                                                    : ui::ViewerLinkHit::Kind::External;
            if (link.kind == pdf::PdfPageLink::Kind::Internal) {
                hit.pageIndex = link.destination.pageIndex;
                hit.hasPoint = link.destination.hasPoint;
                hit.point = link.destination.point;
            }
            hit.url = link.url;
            return hit;
        }
    }
    return std::nullopt;
}

std::vector<core::Rect> TextInteractionController::linkRects(std::size_t pageIndex) {
    std::vector<core::Rect> rects;
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return rects;
    const std::vector<pdf::PdfPageLink> links =
        tab->session()->linkService().cachedLinks(tab->session()->pageId(pageIndex));
    for (const pdf::PdfPageLink& link : links) {
        rects.insert(rects.end(), link.rects.begin(), link.rects.end());
    }
    return rects;
}

void TextInteractionController::linkActivated(const ui::ViewerLinkHit& hit) {
    if (hit.kind == ui::ViewerLinkHit::Kind::Internal) {
        navigateInternalDestination(hit.pageIndex, hit.point, hit.hasPoint);
        return;
    }
    openExternalUrl(hit.url);
}

// Selection + search highlights for one page, in page display space. The
// selection spans a page range resolved through the session's page ordering
// (PageId alone has no order); per page it clips to the selected character
// range. Painting uses cached text only - pages under the viewport are warm
// by construction (the viewport warms them on page tracking); a cold page
// simply gains its highlight when its text arrives.
std::vector<ui::OverlayRect> TextInteractionController::overlayRects(std::size_t pageIndex) {
    std::vector<ui::OverlayRect> rects;
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return rects;
    const editor::DocumentSession* session = tab->session();
    const core::PageId pageId = session->pageId(pageIndex);
    const std::shared_ptr<const pdf::PdfTextPage> page = session->textService().cachedTextPage(pageId);
    if (page == nullptr) return rects;

    // Search matches on this page (active match stronger).
    if (const editor::TextSearchController* search = tab->search(); search != nullptr) {
        const std::optional<std::size_t> active = search->currentIndex();
        const std::vector<editor::TextSearchController::Match> matches = search->matches();
        for (std::size_t m = 0; m < matches.size(); ++m) {
            if (matches[m].page != pageId) continue;
            for (const core::Rect& rect : page->rectsForRange(matches[m].startIndex, matches[m].count)) {
                rects.push_back(
                    ui::OverlayRect{rect, (active && *active == m) ? kActiveSearchMatch : kSearchMatch});
            }
        }
    }

    // Selection highlight: this page's slice of the ordered selection.
    if (!tab->selection().empty()) {
        const std::vector<editor::TextRange> ranges = editor::orderedSelectionRanges(
            tab->selection().selection(),
            [session](core::PageId id) { return session->pageIndexFor(id); },
            [session](std::size_t index) { return session->pageId(index); },
            editor::DocumentSession::kInvalidPage);
        for (const editor::TextRange& range : ranges) {
            if (range.page != pageId) continue;
            const std::uint32_t end = static_cast<std::uint32_t>(
                std::min<std::size_t>(range.end, page->charCount()));
            if (range.begin >= end) continue;
            for (const core::Rect& rect : page->rectsForRange(range.begin, end - range.begin)) {
                rects.push_back(ui::OverlayRect{rect, kSelectionHighlight});
            }
        }
    }
    return rects;
}

// Copy: the selection is snapshotted into per-page ranges in reading order
// and handed to the text service, which extracts EVERY page (cache or worker
// extraction - never a silent skip) and assembles the exact UTF-8 text off
// the main thread. The clipboard is written on the main thread when the
// result arrives; a failure is reported, never a partial copy. The delivery
// dies with the session's text service (tab closed -> no callback), and the
// shell - which owns this controller - outlives every session.
void TextInteractionController::copySelection() {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || tab->selection().empty()) return; // documented no-op
    if (context_.services.clipboard == nullptr) {
        context_.setStatus("No clipboard available on this platform backend");
        return;
    }
    editor::DocumentSession* session = tab->session();
    std::vector<editor::TextRange> ranges = editor::orderedSelectionRanges(
        tab->selection().selection(),
        [session](core::PageId id) { return session->pageIndexFor(id); },
        [session](std::size_t index) { return session->pageId(index); },
        editor::DocumentSession::kInvalidPage);
    if (ranges.empty()) return;
    if (ranges.size() > 1) context_.setStatus(std::format("Copying text from {} pages…", ranges.size()));
    session->textService().requestRangesText(std::move(ranges), [this](core::Result<std::string> text) {
        if (!text.has_value()) {
            context_.setStatus("Copy failed: " + core::describe(text.error()));
            return;
        }
        if (text->empty()) return;
        const core::Status copied = context_.services.clipboard->setText(*text);
        context_.setStatus(copied.has_value() ? std::format("Copied {} characters", text->size())
                                              : "Copy failed: " + core::describe(copied.error()));
    });
}

void TextInteractionController::navigateInternalDestination(std::size_t pageIndex,
                                                            const core::Point& targetPoint,
                                                            bool hasPoint) {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return;
    if (hasPoint) {
        // Center a small region around the target point, keeping the zoom.
        context_.viewport.revealContentRect(
            pageIndex, core::Rect{targetPoint.x - 50.0, targetPoint.y - 50.0, 100.0, 100.0});
    } else {
        context_.viewport.goToPage(pageIndex);
    }
}

// External URL policy: explicit user click (the viewport fires linkActivated
// only for real clicks) + the deliberate scheme allow-list in UrlPolicy.hpp.
void TextInteractionController::openExternalUrl(const std::string& url) {
    if (!isAllowedExternalUrlScheme(url)) {
        core::log::warning("blocked external link with a disallowed scheme");
        context_.setStatus("Blocked external link (unsupported scheme)");
        return;
    }
    if (context_.services.urlOpener == nullptr) {
        context_.setStatus("No URL opener available on this platform backend");
        return;
    }
    const core::Status opened = context_.services.urlOpener->openUrl(url);
    if (!opened.has_value()) context_.setStatus("Could not open link: " + core::describe(opened.error()));
}

} // namespace rivet::app
