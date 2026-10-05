// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownPreviewView.hpp"

#include "markdown/MarkdownSourceMap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

namespace rivet::app {

namespace {

constexpr std::size_t kNone = ~std::size_t{0};
constexpr double kAscentRatio = 0.8; // must match MarkdownPaintMeasurer
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr std::size_t kMaxAltBytes = 160;
constexpr std::size_t kLongRunBytes = 2048;

bool isWordByte(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' || u >= 0x80;
}

std::string truncatedUtf8(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) return std::string(text);
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
    return std::string(text.substr(0, cut)) + "…";
}

// RAII binding of the measurer to a paint context.
class MeasurerBinding {
public:
    MeasurerBinding(MarkdownPaintMeasurer& measurer, const ui::PaintContext& context) : measurer_(measurer) {
        measurer_.bind(&context);
    }
    ~MeasurerBinding() { measurer_.unbind(); }
    MeasurerBinding(const MeasurerBinding&) = delete;
    MeasurerBinding& operator=(const MeasurerBinding&) = delete;

private:
    MarkdownPaintMeasurer& measurer_;
};

} // namespace

// ============================================================ construction

MarkdownPreviewView::MarkdownPreviewView(MarkdownPreviewEnvironment environment, markdown::Typography typography,
                                         MarkdownPreviewPalette palette)
    : env_(std::move(environment)), typography_(typography), palette_(palette) {
    images_ = MarkdownImageStore::create(
        MarkdownImageStore::Environment{env_.dispatcher, env_.scheduler, env_.imageDecoder});
    images_->setOnChanged([this] {
        keepAnchorOnRelayout_ = true;
        layoutDirty_ = true;
        invalidate();
    });
    auto bar = std::make_unique<ui::ScrollBar>(ui::ScrollOrientation::Vertical);
    scrollBar_ = bar.get();
    scrollBar_->setOnScroll([this](double offset) { setScrollY(offset); });
    addChild(std::move(bar));
}

MarkdownPreviewView::~MarkdownPreviewView() {
    images_->setOnChanged({});
    scope_.closeAndWait();
}

void MarkdownPreviewView::layout() {
    scrollBar_->setFrame(core::Rect{bounds().size.width - ui::ScrollBar::kThickness, 0.0,
                                    ui::ScrollBar::kThickness, bounds().size.height});
    syncScrollBar();
}

// ================================================================ content

void MarkdownPreviewView::setDocumentSnapshot(std::shared_ptr<const markdown::MarkdownDocument> document,
                                              std::uint64_t revision, std::string source) {
    document_ = std::move(document);
    revision_ = revision;
    source_ = std::move(source);
    replaced_ = true;
    layoutDirty_ = true;
    blockScrollX_.clear();
    hoverLink_ = 0;
    pressLinkUrl_.clear();
    dragging_ = false;
    clearSelection();
    if (!document_) layout_.reset();
    invalidate();
}

void MarkdownPreviewView::clearDocument() {
    document_.reset();
    layout_.reset();
    revision_ = 0;
    source_.clear();
    scrollY_ = 0.0;
    layoutWidth_ = -1.0;
    layoutDirty_ = true;
    replaced_ = false;
    blockScrollX_.clear();
    hoverLink_ = 0;
    pressLinkUrl_.clear();
    dragging_ = false;
    clearSelection();
    query_.clear();
    matches_.clear();
    current_.reset();
    pendingSourceOffset_.reset();
    syncScrollBar();
    invalidate();
}

void MarkdownPreviewView::setBaseDirectory(std::filesystem::path directory) {
    images_->setBaseDirectory(std::move(directory));
    images_->clear();
    layoutDirty_ = true;
    keepAnchorOnRelayout_ = true;
    invalidate();
}

// ================================================================= layout

void MarkdownPreviewView::prepare(ui::PaintContext& context) {
    if (context.backingScale() != scale_) {
        scale_ = context.backingScale();
        measurer_.clear();
        measureCache_.clear();
        layoutDirty_ = true;
        keepAnchorOnRelayout_ = true;
    }
    const MeasurerBinding binding(measurer_, context);
    ensureLayout(bounds().size.width, false);
}

void MarkdownPreviewView::ensureLayout(double width, bool force) {
    if (!document_) {
        layout_.reset();
        return;
    }
    width = std::max(width, 1.0);
    const bool widthChanged = layout_ == nullptr || layout_->viewportWidth != width;
    if (!force && !layoutDirty_ && !widthChanged) return;
    if (!force && layout_ != nullptr && !layoutDirty_ && widthChanged && layoutCostMs_ > tuning_.slowLayoutMs &&
        env_.scheduler != nullptr && env_.dispatcher != nullptr) {
        if (std::chrono::steady_clock::now() - lastLayoutTime_ < tuning_.relayoutDebounce) {
            scheduleWake(); // keep painting the previous layout; relayout once the resize settles
            return;
        }
    }
    doRelayout(width);
}

MarkdownPreviewView::Anchor MarkdownPreviewView::captureAnchor() const {
    Anchor anchor;
    if (layout_ == nullptr || scrollY_ <= 0.0) return anchor;
    const std::size_t index = layout_->blockIndexAtY(scrollY_);
    if (index == kNone) return anchor;
    anchor.blockId = layout_->blocks[index].blockId;
    anchor.offset = scrollY_ - layout_->blocks[index].rect.origin.y;
    return anchor;
}

void MarkdownPreviewView::restoreAnchor(const Anchor& anchor) {
    if (anchor.blockId == markdown::MarkdownLayout::kNoBlock || layout_ == nullptr) return;
    const double y = layout_->yForBlockId(anchor.blockId);
    if (y >= 0.0) scrollY_ = y + anchor.offset;
}

void MarkdownPreviewView::doRelayout(double width) {
    const Anchor anchor = (!replaced_ && layout_ != nullptr) ? captureAnchor() : Anchor{};
    const bool sameShape = !replaced_ && layout_ != nullptr && layout_->viewportWidth == width;
    const std::size_t oldRuns = layout_ != nullptr ? layout_->runs.size() : 0;
    const std::size_t oldMatchCount = matches_.size();

    const auto started = std::chrono::steady_clock::now();
    layout_ = std::make_unique<markdown::MarkdownLayout>(markdown::layoutMarkdown(
        *document_, width, measurer_, images_.get(), typography_, &measureCache_));
    const auto finished = std::chrono::steady_clock::now();
    layoutCostMs_ = std::chrono::duration<double, std::milli>(finished - started).count();
    lastLayoutTime_ = finished;
    layoutWidth_ = width;
    ++layoutCount_;

    warmed_.assign(layout_->runs.size(), 0);
    selectionRectsDirty_ = true;
    if (hasSelection_ && !(sameShape && layout_->runs.size() == oldRuns)) clearSelection();
    hoverLink_ = 0;

    if (replaced_) {
        scrollY_ = std::clamp(scrollY_, 0.0, maxScrollY());
    } else {
        restoreAnchor(anchor);
    }
    scrollY_ = std::clamp(scrollY_, 0.0, maxScrollY());
    if (pendingSourceOffset_) {
        scrollY_ = std::clamp(markdown::previewYForSourceOffset(*layout_, *pendingSourceOffset_), 0.0, maxScrollY());
        pendingSourceOffset_.reset();
    }
    layoutDirty_ = false;
    keepAnchorOnRelayout_ = false;

    if (!query_.empty()) {
        refreshMatches(!replaced_);
        if (matches_.size() != oldMatchCount || replaced_) searchNotifyPending_ = true;
    }
    replaced_ = false;
    syncScrollBar();

    if (searchNotifyPending_) {
        searchNotifyPending_ = false;
        if (env_.dispatcher != nullptr) {
            const std::weak_ptr<int> alive = alive_;
            env_.dispatcher->post([this, alive] {
                if (alive.lock()) notifySearchChanged();
            });
        } else {
            notifySearchChanged();
        }
    }
}

void MarkdownPreviewView::scheduleWake() {
    if (wakePending_) return;
    std::optional<core::AsyncScope::Token> token = scope_.enter();
    if (!token) return;
    wakePending_ = true;
    const auto shared = std::make_shared<core::AsyncScope::Token>(std::move(*token));
    const std::weak_ptr<int> alive = alive_;
    core::IMainThreadDispatcher* dispatcher = env_.dispatcher;
    const auto delay = tuning_.relayoutDebounce;
    env_.scheduler->post([token = shared, alive, dispatcher, delay, this] {
        const auto until = std::chrono::steady_clock::now() + delay;
        while (std::chrono::steady_clock::now() < until && !token->cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (token->cancelled()) return;
        dispatcher->post([alive, this] {
            if (!alive.lock()) return;
            wakePending_ = false;
            invalidate();
        });
    });
}

// =============================================================== scrolling

double MarkdownPreviewView::maxScrollY() const {
    if (layout_ == nullptr) return 0.0;
    return std::max(0.0, layout_->contentHeight - bounds().size.height);
}

void MarkdownPreviewView::setScrollY(double y) {
    if (!std::isfinite(y)) return;
    y = std::clamp(y, 0.0, maxScrollY());
    if (y == scrollY_) return;
    scrollY_ = y;
    syncScrollBar();
    invalidate();
    if (onScrolled_) onScrolled_();
}

std::optional<std::size_t> MarkdownPreviewView::topSourceOffset() const {
    if (layout_ == nullptr || layoutDirty_ || replaced_) return std::nullopt;
    return markdown::sourceOffsetForPreviewY(*layout_, scrollY_);
}

void MarkdownPreviewView::scrollToSourceOffset(std::size_t offset) {
    if (layout_ == nullptr || layoutDirty_ || replaced_) {
        pendingSourceOffset_ = offset;
        return;
    }
    pendingSourceOffset_.reset();
    setScrollY(markdown::previewYForSourceOffset(*layout_, offset));
}

void MarkdownPreviewView::syncScrollBar() {
    const double contentHeight = layout_ != nullptr ? layout_->contentHeight : 0.0;
    scrollBar_->setExtents(bounds().size.height, contentHeight);
    scrollBar_->setOffset(scrollY_);
}

void MarkdownPreviewView::scrollToDocY(double y) { setScrollY(y); }

bool MarkdownPreviewView::scrollToAnchor(std::string_view anchor) {
    if (!document_ || layout_ == nullptr) return false;
    const markdown::HeadingEntry* heading = document_->findHeadingBySlug(anchor);
    if (heading == nullptr) return false;
    const double y = layout_->yForBlockId(heading->blockId);
    if (y < 0.0) return false;
    scrollToDocY(y - 8.0);
    return true;
}

const markdown::LayoutBlock* MarkdownPreviewView::overflowBlockAtY(double docY, std::size_t* index) const {
    if (layout_ == nullptr) return nullptr;
    const std::size_t i = layout_->blockIndexAtY(docY);
    if (i == kNone) return nullptr;
    const markdown::LayoutBlock& block = layout_->blocks[i];
    if (docY < block.rect.minY() || docY >= block.rect.maxY() || !block.overflowsHorizontally()) return nullptr;
    if (index != nullptr) *index = i;
    return &block;
}

double MarkdownPreviewView::blockScrollX(std::size_t blockIndex) const {
    if (layout_ == nullptr || blockIndex >= layout_->blocks.size()) return 0.0;
    const markdown::LayoutBlock& block = layout_->blocks[blockIndex];
    if (!block.overflowsHorizontally()) return 0.0;
    const auto it = blockScrollX_.find(block.blockId);
    if (it == blockScrollX_.end()) return 0.0;
    return std::clamp(it->second, 0.0, block.contentWidth + 8.0 - block.rect.size.width);
}

void MarkdownPreviewView::scrollBlockBy(std::size_t blockIndex, double dx) {
    if (layout_ == nullptr || blockIndex >= layout_->blocks.size() || !std::isfinite(dx)) return;
    const markdown::LayoutBlock& block = layout_->blocks[blockIndex];
    if (!block.overflowsHorizontally()) return;
    const double maxOffset = std::max(0.0, block.contentWidth + 8.0 - block.rect.size.width);
    const double next = std::clamp(blockScrollX(blockIndex) + dx, 0.0, maxOffset);
    if (next == blockScrollX(blockIndex)) return;
    blockScrollX_[block.blockId] = next;
    selectionRectsDirty_ = true;
    invalidate();
}

// ================================================================== links

MarkdownLinkDecision MarkdownPreviewView::activateLink(std::string_view url) {
    MarkdownLinkDecision decision = classifyMarkdownLink(url);
    switch (decision.kind) {
    case MarkdownLinkKind::Empty: break;
    case MarkdownLinkKind::External:
        // Re-checked here although classify already applied the policy: this is
        // the last gate before the platform opener.
        if (env_.openUrl && isAllowedExternalUrlScheme(decision.target)) {
            if (!env_.openUrl(decision.target)) setStatus("Could not open the link");
        } else {
            setStatus("Opening links is not available");
        }
        break;
    case MarkdownLinkKind::Anchor:
        if (!scrollToAnchor(decision.target)) setStatus("The link target was not found in this document");
        break;
    case MarkdownLinkKind::Local: setStatus("Links to local files are not opened"); break;
    case MarkdownLinkKind::Unsafe: setStatus("Blocked a link with an unsupported scheme"); break;
    }
    return decision;
}

std::string MarkdownPreviewView::linkUrlAt(const markdown::HitResult& hit) const {
    if (layout_ == nullptr || !hit.valid || !hit.onText || hit.link == 0 || hit.link > layout_->links.size()) {
        return {};
    }
    if (layout_->runs[hit.position.run].isImage) return {};
    return layout_->links[hit.link - 1].url;
}

// ============================================================== selection

std::string MarkdownPreviewView::selectedText() const {
    if (layout_ == nullptr || !hasSelection()) return {};
    return layout_->selectedText(std::min(selectionAnchor_, selectionFocus_),
                                 std::max(selectionAnchor_, selectionFocus_));
}

bool MarkdownPreviewView::copySelection() {
    const std::string text = selectedText();
    if (text.empty()) return false;
    if (env_.clipboard == nullptr) {
        setStatus("The clipboard is not available");
        return false;
    }
    if (!env_.clipboard->setText(text)) {
        setStatus("Could not copy the selection");
        return false;
    }
    return true;
}

bool MarkdownPreviewView::selectAll() {
    if (layout_ == nullptr || layout_->runs.empty()) return false;
    selectionAnchor_ = markdown::TextPosition{0, 0};
    const auto last = static_cast<std::uint32_t>(layout_->runs.size() - 1);
    selectionFocus_ = markdown::TextPosition{last, static_cast<std::uint32_t>(layout_->runs[last].text.size())};
    hasSelection_ = true;
    selectionRectsDirty_ = true;
    invalidate();
    return true;
}

void MarkdownPreviewView::clearSelection() {
    if (!hasSelection_ && selectionRects_.empty()) return;
    hasSelection_ = false;
    selectionAnchor_ = selectionFocus_ = markdown::TextPosition{};
    selectionRects_.clear();
    selectionRectsDirty_ = true;
    invalidate();
}

void MarkdownPreviewView::selectWordAt(const markdown::HitResult& hit) {
    const std::string& text = layout_->runs[hit.position.run].text;
    std::size_t at = std::min<std::size_t>(hit.position.offset, text.size());
    std::size_t start = at;
    std::size_t end = at;
    while (start > 0 && isWordByte(text[start - 1])) --start;
    while (end < text.size() && isWordByte(text[end])) ++end;
    selectionAnchor_ = markdown::TextPosition{hit.position.run, static_cast<std::uint32_t>(start)};
    selectionFocus_ = markdown::TextPosition{hit.position.run, static_cast<std::uint32_t>(end)};
    hasSelection_ = true;
}

void MarkdownPreviewView::selectBlockAt(const markdown::HitResult& hit) {
    const markdown::LayoutBlock& block = layout_->blocks[hit.block];
    std::uint32_t first = markdown::MarkdownLayout::kNoBlock;
    std::uint32_t last = markdown::MarkdownLayout::kNoBlock;
    for (std::uint32_t l = block.lineBegin; l < block.lineEnd; ++l) {
        const markdown::LayoutLine& line = layout_->lines[l];
        if (line.runEnd <= line.runBegin) continue;
        if (first == markdown::MarkdownLayout::kNoBlock) first = line.runBegin;
        last = line.runEnd - 1;
    }
    if (first == markdown::MarkdownLayout::kNoBlock) return;
    selectionAnchor_ = markdown::TextPosition{first, 0};
    selectionFocus_ = markdown::TextPosition{last, static_cast<std::uint32_t>(layout_->runs[last].text.size())};
    hasSelection_ = true;
}

markdown::HitResult MarkdownPreviewView::hitAt(core::Point local) const {
    if (layout_ == nullptr) return {};
    core::Point doc{local.x, local.y + scrollY_};
    markdown::HitResult hit = layout_->hitTest(doc, measurer_);
    if (hit.valid && hit.block < layout_->blocks.size()) {
        const double off = blockScrollX(hit.block);
        if (off != 0.0) hit = layout_->hitTest(core::Point{doc.x + off, doc.y}, measurer_);
    }
    return hit;
}

// ================================================================== input

bool MarkdownPreviewView::handleScroll(const ui::PointerEvent& event) {
    const double dx = event.scrollDelta.x;
    const double dy = event.scrollDelta.y;
    if (std::abs(dx) > std::abs(dy)) {
        std::size_t index = 0;
        if (overflowBlockAtY(event.position.y + scrollY_, &index) != nullptr) scrollBlockBy(index, dx);
    } else {
        scrollBy(dy);
    }
    event.accepted = true;
    return true;
}

bool MarkdownPreviewView::onMouse(const ui::PointerEvent& event) {
    if (Widget::onMouse(event)) return true; // the scroll bar
    if (layout_ == nullptr) return false;

    switch (event.type) {
    case ui::PointerEventType::Scroll:
        if (!bounds().contains(event.position)) return false;
        return handleScroll(event);

    case ui::PointerEventType::Down: {
        if (event.button != 1 || !bounds().contains(event.position)) return false;
        const auto now = std::chrono::steady_clock::now();
        const double dist = std::hypot(event.position.x - lastClickPos_.x, event.position.y - lastClickPos_.y);
        clickCount_ = (clickCount_ > 0 && now - lastClickTime_ < std::chrono::milliseconds(450) && dist < 5.0)
                          ? clickCount_ % 3 + 1
                          : 1;
        lastClickTime_ = now;
        lastClickPos_ = event.position;
        pressPos_ = event.position;
        pressMoved_ = false;
        dragging_ = true;
        event.accepted = true;

        const markdown::HitResult hit = hitAt(event.position);
        pressLinkUrl_ = linkUrlAt(hit);
        if (!hit.valid) {
            clearSelection();
            return true;
        }
        if (clickCount_ == 2) {
            selectWordAt(hit);
        } else if (clickCount_ == 3) {
            selectBlockAt(hit);
        } else if (event.modifiers.shift && hasSelection_) {
            selectionFocus_ = hit.position;
        } else {
            selectionAnchor_ = selectionFocus_ = hit.position;
            hasSelection_ = true;
        }
        selectionRectsDirty_ = true;
        invalidate();
        return true;
    }

    case ui::PointerEventType::Move: {
        if (event.button == 1 && dragging_) {
            if (std::hypot(event.position.x - pressPos_.x, event.position.y - pressPos_.y) > 3.0) {
                pressMoved_ = true;
            }
            // Edge auto-scroll: one step per move event beyond the edge.
            const double h = bounds().size.height;
            if (event.position.y < 0.0) {
                scrollBy(-std::min(40.0, 4.0 - event.position.y * 0.5));
            } else if (event.position.y > h) {
                scrollBy(std::min(40.0, 4.0 + (event.position.y - h) * 0.5));
            }
            const markdown::HitResult hit = hitAt(event.position);
            if (hit.valid && pressMoved_ && clickCount_ == 1) {
                selectionFocus_ = hit.position;
                hasSelection_ = true;
                selectionRectsDirty_ = true;
                invalidate();
            }
            event.accepted = true;
            return true;
        }
        if (event.button == 0) {
            dragging_ = false;
            const markdown::HitResult hit = hitAt(event.position);
            const std::uint32_t link = (!linkUrlAt(hit).empty()) ? hit.link : 0u;
            if (link != hoverLink_) {
                hoverLink_ = link;
                invalidate();
            }
        }
        return false;
    }

    case ui::PointerEventType::Up: {
        if (!dragging_) return false;
        dragging_ = false;
        event.accepted = true;
        if (!pressMoved_ && clickCount_ == 1 && !pressLinkUrl_.empty()) {
            const std::string url = linkUrlAt(hitAt(event.position));
            if (url == pressLinkUrl_) {
                if (hasSelection_) clearSelection();
                activateLink(url);
            }
        }
        return true;
    }

    case ui::PointerEventType::Exited:
        if (hoverLink_ != 0) {
            hoverLink_ = 0;
            invalidate();
        }
        return false;

    default: return false;
    }
}

bool MarkdownPreviewView::onKey(const ui::KeyEvent& event) {
    const double page = std::max(kArrowScrollPoints, bounds().size.height * 0.9);
    if (event.modifiers.command || event.modifiers.control) {
        if (event.key == ui::Key::Character && !event.modifiers.shift && !event.modifiers.option) {
            if (event.text == "c") {
                copySelection();
                event.accepted = true;
                return true;
            }
            if (event.text == "a") {
                selectAll();
                event.accepted = true;
                return true;
            }
        }
        return false;
    }
    switch (event.key) {
    case ui::Key::Up: scrollBy(-kArrowScrollPoints); break;
    case ui::Key::Down: scrollBy(kArrowScrollPoints); break;
    case ui::Key::PageUp: scrollBy(-page); break;
    case ui::Key::PageDown: scrollBy(page); break;
    case ui::Key::Space: scrollBy(event.modifiers.shift ? -page : page); break;
    case ui::Key::Home: setScrollY(0.0); break;
    case ui::Key::End: setScrollY(maxScrollY()); break;
    default: return false;
    }
    event.accepted = true;
    return true;
}

// ================================================================= search

void MarkdownPreviewView::notifySearchChanged() {
    if (onSearchChanged_) onSearchChanged_();
}

void MarkdownPreviewView::refreshMatches(bool keepCurrent) {
    const std::optional<std::size_t> old = current_;
    matches_.clear();
    current_.reset();
    if (layout_ == nullptr || query_.empty()) return;
    matches_ = findInLayout(*layout_, query_);
    if (matches_.empty()) return;
    if (keepCurrent && old && *old < matches_.size()) {
        current_ = old;
        return;
    }
    // First match at or below the top of the viewport (wraps to the first).
    current_ = 0;
    for (std::size_t i = 0; i < matches_.size(); ++i) {
        const markdown::LayoutRun& run = layout_->runs[matches_[i].begin.run];
        if (layout_->lines[run.line].top >= scrollY_) {
            current_ = i;
            break;
        }
    }
}

void MarkdownPreviewView::startSearch(std::string query) {
    query_ = std::move(query);
    refreshMatches(false);
    invalidate();
    notifySearchChanged();
}

void MarkdownPreviewView::stepMatch(int delta) {
    if (matches_.empty()) return;
    const std::size_t n = matches_.size();
    if (!current_) {
        current_ = delta > 0 ? 0 : n - 1;
    } else {
        current_ = (*current_ + (delta > 0 ? 1 : n - 1)) % n;
    }
    revealCurrentMatch();
    invalidate();
    notifySearchChanged();
}

void MarkdownPreviewView::revealCurrentMatch() {
    if (!current_ || layout_ == nullptr || *current_ >= matches_.size()) return;
    const FindMatch& match = matches_[*current_];
    const markdown::LayoutRun& run = layout_->runs[match.begin.run];
    const markdown::LayoutLine& line = layout_->lines[run.line];
    const double viewH = bounds().size.height;
    const double margin = std::min(24.0, viewH / 4.0);
    if (line.top < scrollY_ + margin || line.top + line.height > scrollY_ + viewH - margin) {
        setScrollY(line.top - viewH / 3.0);
    }
    // Horizontally overflowing block: bring the match into the block's box.
    const markdown::LayoutBlock& block = layout_->blocks[match.block];
    if (block.overflowsHorizontally()) {
        const double x = layout_->xForPosition(match.begin, measurer_);
        const double off = blockScrollX(match.block);
        if (x < block.rect.minX() + off || x > block.rect.maxX() + off - 24.0) {
            scrollBlockBy(match.block, (x - block.rect.minX() - 24.0) - off);
        }
    }
}

// =============================================================== painting

ui::Color MarkdownPreviewView::colorFor(const markdown::TextStyle& style) const {
    using markdown::TextKind;
    switch (style.kind) {
    case TextKind::H1:
    case TextKind::H2:
    case TextKind::H3:
    case TextKind::H4:
    case TextKind::H5:
    case TextKind::H6: return palette_.heading;
    case TextKind::Code: return palette_.codeText;
    case TextKind::Quote: return palette_.quoteText;
    case TextKind::Link: return palette_.link;
    default: return palette_.text;
    }
}

void MarkdownPreviewView::paintSelf(ui::PaintContext& context) const {
    // The layout is a cache validated against the paint context (scale, width);
    // refreshing it from a const paint is the intended lazy-relayout point.
    auto* self = const_cast<MarkdownPreviewView*>(this);
    self->prepare(context);
    const MeasurerBinding binding(measurer_, context);

    context.fillRect(bounds(), palette_.background);
    if (layout_ == nullptr) return;

    const double y0 = scrollY_;
    const double y1 = scrollY_ + bounds().size.height;
    const auto [b0, b1] = layout_->blockRangeForY(y0, y1);

    for (const std::size_t i : layout_->decorationsInRange(y0, y1)) paintDecoration(context, layout_->decorations[i]);
    paintSelectionAndMatches(context);
    for (std::size_t b = b0; b < b1; ++b) paintBlock(context, b);

    // Per-block horizontal scroll indicators.
    for (std::size_t b = b0; b < b1; ++b) {
        const markdown::LayoutBlock& block = layout_->blocks[b];
        if (!block.overflowsHorizontally()) continue;
        const double total = block.contentWidth + 8.0;
        const double maxOff = std::max(1.0, total - block.rect.size.width);
        const double thumb = std::max(24.0, block.rect.size.width * block.rect.size.width / total);
        const double x = block.rect.minX() + (block.rect.size.width - thumb) * (blockScrollX(b) / maxOff);
        context.fillRoundedRect(core::Rect{x, block.rect.maxY() - 6.0 - scrollY_, thumb, 3.0},
                                palette_.overflowIndicator, 1.5);
    }
}

void MarkdownPreviewView::paintDecoration(ui::PaintContext& context, const markdown::Decoration& d) const {
    using markdown::DecorationKind;
    const core::Rect view{d.rect.minX(), d.rect.minY() - scrollY_, d.rect.size.width, d.rect.size.height};
    switch (d.kind) {
    case DecorationKind::CodeBackground: context.fillRoundedRect(view, palette_.codeBackground, 4.0); break;
    case DecorationKind::QuoteBar: context.fillRect(view, palette_.quoteBar); break;
    case DecorationKind::Rule: context.fillRect(view, palette_.rule); break;
    case DecorationKind::ListBullet:
        context.fillRoundedRect(view, palette_.marker, std::min(view.size.width, view.size.height) / 2.0);
        break;
    case DecorationKind::ListNumber: {
        const double lh = measurer_.lineHeight(d.style);
        context.drawText(d.text, core::Rect{view.minX(), d.baseline - scrollY_ - kAscentRatio * lh,
                                            view.size.width + 6.0, lh},
                         fontFor(d.style), palette_.marker, ui::TextAlign::Left);
        break;
    }
    case DecorationKind::TaskCheckbox: {
        if (d.checked) {
            context.fillRoundedRect(view, palette_.link, 3.0);
            const double w = view.size.width;
            const double h = view.size.height;
            context.drawLine({view.minX() + 0.22 * w, view.minY() + 0.52 * h},
                             {view.minX() + 0.43 * w, view.minY() + 0.74 * h}, ui::Color::white(), 1.6);
            context.drawLine({view.minX() + 0.43 * w, view.minY() + 0.74 * h},
                             {view.minX() + 0.78 * w, view.minY() + 0.28 * h}, ui::Color::white(), 1.6);
        } else {
            context.strokeRect(view, palette_.marker, 1.2);
        }
        break;
    }
    case DecorationKind::ImagePlaceholder: {
        if (d.imageState == markdown::ImageState::Known) {
            if (const std::shared_ptr<const core::Bitmap> bitmap = images_->bitmap(d.url)) {
                context.drawBitmap(*bitmap, view);
                break;
            }
        }
        paintImagePlaceholder(context, d, view);
        break;
    }
    }
}

void MarkdownPreviewView::paintImagePlaceholder(ui::PaintContext& context, const markdown::Decoration& d,
                                                const core::Rect& viewRect) const {
    const std::string alt = d.text.empty() ? std::string("image") : truncatedUtf8(d.text, kMaxAltBytes);
    std::string label;
    if (d.imageState == markdown::ImageState::Blocked) {
        switch (images_->blockReason(d.url)) {
        case ImageBlockReason::Remote: label = "Remote image blocked: " + alt; break;
        case ImageBlockReason::TooLarge: label = "Image too large to display: " + alt; break;
        default: label = "Image unavailable: " + alt; break;
        }
    } else {
        label = "Loading image: " + alt;
    }
    context.fillRoundedRect(viewRect, palette_.placeholderBackground, 6.0);
    context.pushClip(viewRect);
    const ui::Font font{13.0, ui::Font::Weight::Regular, true, false};
    const core::Size text = context.measureText(label, font);
    context.drawText(label, core::Rect{8.0, 0.0, std::max(0.0, viewRect.size.width - 16.0), viewRect.size.height},
                     font, palette_.placeholderText,
                     text.width > viewRect.size.width - 16.0 ? ui::TextAlign::Left : ui::TextAlign::Center);
    context.popClip();
}

void MarkdownPreviewView::fillDocRect(ui::PaintContext& context, const core::Rect& docRect,
                                      const ui::Color& color) const {
    std::size_t index = 0;
    const markdown::LayoutBlock* block = overflowBlockAtY(docRect.center().y, &index);
    if (block == nullptr) {
        context.fillRect(core::Rect{docRect.minX(), docRect.minY() - scrollY_, docRect.size.width,
                                    docRect.size.height},
                         color);
        return;
    }
    const double off = blockScrollX(index);
    context.pushClip(core::Rect{block->rect.minX(), block->rect.minY() - scrollY_, block->rect.size.width,
                                block->rect.size.height});
    context.fillRect(core::Rect{docRect.minX() - block->rect.minX() - off, docRect.minY() - block->rect.minY(),
                                docRect.size.width, docRect.size.height},
                     color);
    context.popClip();
}

void MarkdownPreviewView::paintSelectionAndMatches(ui::PaintContext& context) const {
    const double y0 = scrollY_;
    const double y1 = scrollY_ + bounds().size.height;
    if (hasSelection()) {
        if (selectionRectsDirty_) {
            selectionRects_ = layout_->selectionRects(std::min(selectionAnchor_, selectionFocus_),
                                                      std::max(selectionAnchor_, selectionFocus_), measurer_);
            selectionRectsDirty_ = false;
        }
        for (const core::Rect& r : selectionRects_) {
            if (r.maxY() < y0 || r.minY() > y1) continue;
            fillDocRect(context, r, palette_.selection);
        }
    }
    if (!matches_.empty()) {
        const auto [b0, b1] = layout_->blockRangeForY(y0, y1);
        auto it = std::lower_bound(matches_.begin(), matches_.end(), static_cast<std::uint32_t>(b0),
                                   [](const FindMatch& m, std::uint32_t block) { return m.block < block; });
        for (; it != matches_.end() && it->block < b1; ++it) {
            const bool current = current_ && static_cast<std::size_t>(it - matches_.begin()) == *current_;
            for (const core::Rect& r : layout_->selectionRects(it->begin, it->end, measurer_)) {
                fillDocRect(context, r, current ? palette_.currentMatch : palette_.match);
            }
        }
    }
}

void MarkdownPreviewView::paintBlock(ui::PaintContext& context, std::size_t blockIndex) const {
    const markdown::LayoutBlock& block = layout_->blocks[blockIndex];
    const double y0 = scrollY_;
    const double y1 = scrollY_ + bounds().size.height;

    double ox = 0.0;
    double oy = -scrollY_;
    double clipLeft = -kInf;
    double clipRight = kInf;
    const bool overflow = block.overflowsHorizontally();
    if (overflow) {
        const double off = blockScrollX(blockIndex);
        context.pushClip(core::Rect{block.rect.minX(), block.rect.minY() - scrollY_, block.rect.size.width,
                                    block.rect.size.height});
        ox = -block.rect.minX() - off;
        oy = -block.rect.minY();
        clipLeft = block.rect.minX() + off - 32.0;
        clipRight = block.rect.maxX() + off + 32.0;
    }

    if (block.kind == markdown::LayoutBlockKind::Table) {
        const auto cellsBegin = layout_->tableCells.begin() + block.cellBegin;
        const auto cellsEnd = layout_->tableCells.begin() + block.cellEnd;
        auto it = std::partition_point(cellsBegin, cellsEnd, [&](const markdown::TableCellLayout& c) {
            return c.rect.maxY() < y0;
        });
        for (; it != cellsEnd && it->rect.minY() <= y1; ++it) {
            const core::Rect rect{it->rect.minX() + ox, it->rect.minY() + oy, it->rect.size.width,
                                  it->rect.size.height};
            if (it->header) context.fillRect(rect, palette_.tableHeaderBackground);
            context.strokeRect(rect, palette_.tableGrid, 1.0);
        }
        auto cell = std::partition_point(cellsBegin, cellsEnd, [&](const markdown::TableCellLayout& c) {
            return c.rect.maxY() < y0;
        });
        for (; cell != cellsEnd && cell->rect.minY() <= y1; ++cell) {
            if (cell->rect.maxX() < clipLeft || cell->rect.minX() > clipRight) continue;
            paintLines(context, block, cell->lineBegin, cell->lineEnd, ox, oy, clipLeft, clipRight);
        }
    } else if (block.lineEnd > block.lineBegin) {
        const auto linesBegin = layout_->lines.begin() + block.lineBegin;
        const auto linesEnd = layout_->lines.begin() + block.lineEnd;
        const auto first = std::partition_point(linesBegin, linesEnd, [&](const markdown::LayoutLine& l) {
            return l.top + l.height < y0;
        });
        const auto last = std::partition_point(first, linesEnd, [&](const markdown::LayoutLine& l) {
            return l.top <= y1;
        });
        paintLines(context, block, static_cast<std::uint32_t>(first - layout_->lines.begin()),
                   static_cast<std::uint32_t>(last - layout_->lines.begin()), ox, oy, clipLeft, clipRight);
    }

    if (overflow) context.popClip();
}

void MarkdownPreviewView::paintLines(ui::PaintContext& context, const markdown::LayoutBlock& block,
                                     std::uint32_t lineBegin, std::uint32_t lineEnd, double ox, double oy,
                                     double clipLeft, double clipRight) const {
    for (std::uint32_t l = lineBegin; l < lineEnd; ++l) {
        const markdown::LayoutLine& line = layout_->lines[l];
        if (line.x + line.width < clipLeft || line.x > clipRight) continue;
        for (std::uint32_t r = line.runBegin; r < line.runEnd; ++r) {
            const markdown::LayoutRun& run = layout_->runs[r];
            if (run.x + run.width < clipLeft || run.x > clipRight) continue;
            paintRun(context, run, block, r, ox, oy, clipLeft, clipRight);
        }
    }
}

void MarkdownPreviewView::paintRun(ui::PaintContext& context, const markdown::LayoutRun& run,
                                   const markdown::LayoutBlock& block, std::uint32_t runIndex, double ox,
                                   double oy, double clipLeft, double clipRight) const {
    if (run.text.empty()) return;
    if (runIndex < warmed_.size() && warmed_[runIndex] == 0) {
        measurer_.warm(run.text, run.style);
        warmed_[runIndex] = 1;
    }
    const double lh = measurer_.lineHeight(run.style);
    const double top = run.baseline + oy - kAscentRatio * lh;
    double x = run.x + ox;
    std::string_view text = run.text;

    // A very long monospace run (minified code line) is drawn only where it is
    // visible, estimating the visible byte range from the average advance.
    if (run.style.monospace && text.size() > kLongRunBytes && std::isfinite(clipLeft) && std::isfinite(clipRight)) {
        const double perByte = run.width / static_cast<double>(text.size());
        if (perByte > 0.0) {
            auto from = static_cast<std::size_t>(std::max(0.0, (clipLeft - run.x) / perByte));
            auto to = static_cast<std::size_t>(std::max(0.0, (clipRight - run.x) / perByte)) + 2;
            from = std::min(from, text.size());
            to = std::min(to, text.size());
            while (from > 0 && from < text.size() && (static_cast<unsigned char>(text[from]) & 0xC0u) == 0x80u) --from;
            while (to < text.size() && (static_cast<unsigned char>(text[to]) & 0xC0u) == 0x80u) ++to;
            text = text.substr(from, to - from);
            x += static_cast<double>(from) * perByte;
        }
    }

    const bool inlineCode = run.style.monospace && block.kind != markdown::LayoutBlockKind::Code &&
                            block.kind != markdown::LayoutBlockKind::Html;
    if (inlineCode) {
        context.fillRoundedRect(core::Rect{x - 2.0, top, run.width + 4.0, lh}, palette_.inlineCodeBackground, 3.0);
    }
    ui::Color color = colorFor(run.style);
    const bool link = run.link != 0 && !run.isImage;
    if (run.isImage) color = palette_.placeholderText;
    if (link && run.link == hoverLink_) color = palette_.linkHover;
    context.drawText(text, core::Rect{x, top, run.width + 8.0, lh}, fontFor(run.style), color, ui::TextAlign::Left);
    if (link) {
        context.drawLine({x, run.baseline + oy + 2.0}, {x + run.width, run.baseline + oy + 2.0}, color, 1.0);
    }
    if (run.style.strike) {
        const double y = run.baseline + oy - run.style.size * 0.3;
        context.drawLine({x, y}, {x + run.width, y}, color, 1.0);
    }
}

} // namespace rivet::app
