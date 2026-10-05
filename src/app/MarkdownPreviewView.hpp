// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/MarkdownFind.hpp"
#include "app/MarkdownImageStore.hpp"
#include "app/MarkdownLinks.hpp"
#include "app/MarkdownPaintMeasurer.hpp"
#include "app/SearchTarget.hpp"
#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "markdown/MarkdownLayout.hpp"
#include "markdown/MarkdownModel.hpp"
#include "platform/Clipboard.hpp"
#include "platform/ImageDecoder.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/Widget.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rivet::app {

// Shell services the preview may use. All optional (null = feature off) and
// owned by the shell; they outlive the view.
struct MarkdownPreviewEnvironment {
    core::IMainThreadDispatcher* dispatcher = nullptr;
    core::TaskScheduler* scheduler = nullptr;
    const platform::IImageDecoder* imageDecoder = nullptr;
    platform::IClipboard* clipboard = nullptr;
    // Opens an already policy-checked http/https/mailto URL (user click only).
    std::function<core::Status(const std::string&)> openUrl;
    std::function<void(std::string)> setStatus;
};

struct MarkdownPreviewPalette {
    ui::Color background = ui::Color::white();
    ui::Color text = ui::Color::gray(0.13);
    ui::Color heading = ui::Color::gray(0.08);
    ui::Color quoteText = ui::Color::gray(0.38);
    ui::Color quoteBar = ui::Color::gray(0.78);
    ui::Color link = ui::Color::rgba(0.05, 0.36, 0.78, 1.0);
    ui::Color linkHover = ui::Color::rgba(0.0, 0.22, 0.55, 1.0);
    ui::Color codeText = ui::Color::gray(0.18);
    ui::Color codeBackground = ui::Color::gray(0.955);
    ui::Color inlineCodeBackground = ui::Color::gray(0.93);
    ui::Color rule = ui::Color::gray(0.8);
    ui::Color tableGrid = ui::Color::gray(0.8);
    ui::Color tableHeaderBackground = ui::Color::gray(0.94);
    ui::Color marker = ui::Color::gray(0.35);
    ui::Color placeholderBackground = ui::Color::gray(0.94);
    ui::Color placeholderText = ui::Color::gray(0.4);
    ui::Color selection = ui::Color::rgba(0.25, 0.5, 0.95, 0.3);
    ui::Color match = ui::Color::rgba(1.0, 0.85, 0.0, 0.45);
    ui::Color currentMatch = ui::Color::rgba(1.0, 0.55, 0.0, 0.6);
    ui::Color overflowIndicator = ui::Color::rgba(0.0, 0.0, 0.0, 0.25);
};

// The rendered Markdown view: paints a MarkdownLayout through the PaintContext
// (only the visible blocks), scrolls, selects, copies, finds, follows links
// and shows local images. No browser engine: layout is the pure
// markdown::layoutMarkdown, painting is plain fill/stroke/text primitives.
//
// Behaviour summary
//  - Vertical scroll: wheel/trackpad, the ScrollBar overlay, arrow/page/home/
//    end/space keys. Relayout (width change, image arrival) keeps the block at
//    the top of the viewport in place.
//  - Horizontal overflow (long code lines, wide tables) is handled PER BLOCK:
//    the block clips to its box and scrolls on its own with horizontal wheel
//    input (shift+wheel on a mouse) while the pointer is over it; a thin
//    indicator at the block's bottom edge shows the extent. The page itself
//    never scrolls horizontally.
//  - Resize: relayout is lazy (at the next paint). For documents whose last
//    relayout was slow, relayouts during a live resize are spaced at least
//    `Tuning::relayoutDebounce` apart; the previous layout keeps painting
//    meanwhile and a worker wake-up guarantees the trailing relayout.
//  - Links: click opens http/https/mailto via the URL opener (UrlPolicy),
//    "#slug" scrolls to the heading, relative/absolute file links and every
//    other scheme are never opened (a status message says so). There is no
//    pointer-cursor change: the UI layer has no cursor API; hovering a link
//    just darkens it.
//  - Images: see MarkdownImageStore. Blocks show the picture, or a placeholder
//    with the alt text while loading / when blocked or unreadable.
//  - Selection: click-drag, double-click word, triple-click block, shift+click
//    extend, Cmd/Ctrl+A all, Cmd/Ctrl+C copies the plain text. Cross-block.
//    A relayout that changes line breaks drops the selection.
//  - Search: ISearchTarget over the displayed text (ASCII case-insensitive).
//
// The document arrives as an immutable snapshot (setDocumentSnapshot): the
// view never parses. Main thread only.
class MarkdownPreviewView final : public ui::Widget, public ISearchTarget {
public:
    struct Tuning {
        double slowLayoutMs = 12.0; // a relayout slower than this makes resizes debounced
        std::chrono::milliseconds relayoutDebounce{120};
    };

    explicit MarkdownPreviewView(MarkdownPreviewEnvironment environment, markdown::Typography typography = {},
                                 MarkdownPreviewPalette palette = {});
    ~MarkdownPreviewView() override;

    // --- content ----------------------------------------------------------
    // Installs the parsed document of `revision`. `source` is the text it was
    // parsed from (kept for source-offset mapping). Keeps the scroll position
    // (clamped); drops selection and horizontal offsets.
    void setDocumentSnapshot(std::shared_ptr<const markdown::MarkdownDocument> document,
                             std::uint64_t revision, std::string source);
    // Drops the document (empty view) and everything derived from it.
    void clearDocument();
    std::uint64_t revision() const { return revision_; }
    const std::string& source() const { return source_; }
    const markdown::MarkdownDocument* document() const { return document_.get(); }
    // Directory relative image paths resolve against (the .md file's folder).
    void setBaseDirectory(std::filesystem::path directory);
    void setTuning(const Tuning& tuning) { tuning_ = tuning; }

    // Lays out (when needed) against `context` and caches glyph metrics. Paint
    // does this; tests and hit-testing before the first paint call it.
    void prepare(ui::PaintContext& context);
    // The current layout (null before the first prepare/paint or without a document).
    const markdown::MarkdownLayout* currentLayout() const { return layout_.get(); }
    std::size_t layoutCount() const { return layoutCount_; } // relayouts performed (diagnostics)
    MarkdownImageStore& imageStore() { return *images_; }

    // --- scrolling --------------------------------------------------------
    double scrollY() const { return scrollY_; }
    double maxScrollY() const;
    void setScrollY(double y);
    void scrollBy(double dy) { setScrollY(scrollY_ + dy); }
    // Scrolls so the heading with this slug ("#x" or "x") is at the top.
    bool scrollToAnchor(std::string_view anchor);
    // Horizontal offset of an overflowing block (by layout block index).
    double blockScrollX(std::size_t blockIndex) const;
    void scrollBlockBy(std::size_t blockIndex, double dx);
    // Visible y range in document space.
    double visibleTop() const { return scrollY_; }

    // --- links ------------------------------------------------------------
    // Applies the link policy to `url` as a user click would (opens/scrolls/
    // reports). Returns the decision (tests).
    MarkdownLinkDecision activateLink(std::string_view url);

    // --- selection and clipboard -----------------------------------------
    bool hasSelection() const { return hasSelection_ && selectionAnchor_ != selectionFocus_; }
    std::string selectedText() const;
    bool copySelection();
    bool selectAll();
    void clearSelection();

    // --- ISearchTarget ----------------------------------------------------
    void startSearch(std::string query) override;
    void cancelSearch() override {}
    void nextMatch() override { stepMatch(+1); }
    void previousMatch() override { stepMatch(-1); }
    const std::string& searchQuery() const override { return query_; }
    std::size_t matchCount() const override { return matches_.size(); }
    std::optional<std::size_t> currentMatch() const override { return current_; }
    bool searching() const override { return false; }
    void revealCurrentMatch() override;
    void setOnSearchResultsChanged(std::function<void()> onChanged) override {
        onSearchChanged_ = std::move(onChanged);
    }
    const std::vector<FindMatch>& matches() const { return matches_; }

    // --- Widget -----------------------------------------------------------
    bool wantsFocus() const override { return true; }
    void paintSelf(ui::PaintContext& context) const override;
    bool onMouse(const ui::PointerEvent& event) override;
    bool onKey(const ui::KeyEvent& event) override;
    void layout() override;

    static constexpr double kArrowScrollPoints = 40.0;

private:
    struct Anchor {
        std::uint32_t blockId = markdown::MarkdownLayout::kNoBlock;
        double offset = 0.0;
    };

    void ensureLayout(double width, bool force);
    void doRelayout(double width);
    Anchor captureAnchor() const;
    void restoreAnchor(const Anchor& anchor);
    void scheduleWake();
    void syncScrollBar();
    void notifySearchChanged();
    void refreshMatches(bool keepCurrent);
    void stepMatch(int delta);
    void scrollToDocY(double y);

    // Geometry helpers (document space <-> view space).
    const markdown::LayoutBlock* overflowBlockAtY(double docY, std::size_t* index = nullptr) const;
    ui::Font fontFor(const markdown::TextStyle& style) const { return MarkdownPaintMeasurer::fontFor(style); }
    ui::Color colorFor(const markdown::TextStyle& style) const;

    // Painting.
    void paintDecoration(ui::PaintContext& context, const markdown::Decoration& d) const;
    void paintBlock(ui::PaintContext& context, std::size_t blockIndex) const;
    void paintLines(ui::PaintContext& context, const markdown::LayoutBlock& block, std::uint32_t lineBegin,
                    std::uint32_t lineEnd, double ox, double oy, double clipLeft, double clipRight) const;
    void paintRun(ui::PaintContext& context, const markdown::LayoutRun& run, const markdown::LayoutBlock& block,
                  std::uint32_t runIndex, double ox, double oy, double clipLeft, double clipRight) const;
    void paintImagePlaceholder(ui::PaintContext& context, const markdown::Decoration& d,
                               const core::Rect& viewRect) const;
    void fillDocRect(ui::PaintContext& context, const core::Rect& docRect, const ui::Color& color) const;
    void paintSelectionAndMatches(ui::PaintContext& context) const;
    void warmVisibleRuns(std::size_t b0, std::size_t b1) const;

    // Input.
    markdown::HitResult hitAt(core::Point local) const;
    bool handleScroll(const ui::PointerEvent& event);
    void beginDrag(const ui::PointerEvent& event);
    void selectWordAt(const markdown::HitResult& hit);
    void selectBlockAt(const markdown::HitResult& hit);
    std::string linkUrlAt(const markdown::HitResult& hit) const;
    void setStatus(std::string text) const {
        if (env_.setStatus) env_.setStatus(std::move(text));
    }

    MarkdownPreviewEnvironment env_;
    markdown::Typography typography_;
    MarkdownPreviewPalette palette_;
    Tuning tuning_;

    std::shared_ptr<const markdown::MarkdownDocument> document_;
    std::uint64_t revision_ = 0;
    std::string source_;

    std::shared_ptr<MarkdownImageStore> images_;
    mutable MarkdownPaintMeasurer measurer_;
    markdown::MeasureCache measureCache_;
    std::unique_ptr<markdown::MarkdownLayout> layout_;
    double layoutWidth_ = -1.0;
    bool layoutDirty_ = true;
    bool keepAnchorOnRelayout_ = false; // set by image arrival
    bool replaced_ = false;             // a new snapshot is pending its first layout
    double scale_ = 0.0;
    double layoutCostMs_ = 0.0;
    std::chrono::steady_clock::time_point lastLayoutTime_{};
    bool wakePending_ = false;
    std::size_t layoutCount_ = 0;
    mutable std::vector<std::uint8_t> warmed_; // per run: advances recorded

    double scrollY_ = 0.0;
    ui::ScrollBar* scrollBar_ = nullptr;
    std::unordered_map<std::uint32_t, double> blockScrollX_; // by LayoutBlock::blockId

    // Selection.
    bool hasSelection_ = false;
    markdown::TextPosition selectionAnchor_;
    markdown::TextPosition selectionFocus_;
    mutable std::vector<core::Rect> selectionRects_;
    mutable bool selectionRectsDirty_ = true;
    bool dragging_ = false;
    int clickCount_ = 0;
    std::chrono::steady_clock::time_point lastClickTime_{};
    core::Point lastClickPos_;
    core::Point pressPos_;
    std::string pressLinkUrl_;
    bool pressMoved_ = false;
    std::uint32_t hoverLink_ = 0; // 1-based layout link index under the pointer

    // Search.
    std::string query_;
    std::vector<FindMatch> matches_;
    std::optional<std::size_t> current_;
    std::function<void()> onSearchChanged_;
    bool searchNotifyPending_ = false;

    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    core::AsyncScope scope_;
};

} // namespace rivet::app
