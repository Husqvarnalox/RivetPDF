// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ComparisonTabState.hpp"
#include "ui/Button.hpp"
#include "ui/OutlinePanel.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/ViewerTextBridge.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rivet::app {

class TextLabel;

// The content of a comparison tab (ADR-0029): a header with Previous / Next
// buttons and a summary, a list of the differences on the left and the two
// documents side by side in their own PdfViewports. Deletions are painted
// red on the left, insertions green on the right (semi-transparent fills, so
// they read on any page color); extra pages tint the whole page.
//
// Scrolling is synchronized by page index plus the position within the page,
// so documents with different page sizes or zoom stay aligned. There is no
// text selection in the comparison viewports.
//
// The shell owns ONE view and rebinds it on every tab switch: bind(&state)
// for a comparison tab, bind(nullptr) otherwise (every pointer into the
// state is dropped then). The bound state outlives the binding. Main thread
// only.
class ComparisonHostView final : public ui::Widget {
public:
    struct Environment {
        // Keyboard focus routing (nullptr = the shell's default content route,
        // which forwards keys to this view).
        std::function<void(ui::Widget*)> setFocus;
    };

    static constexpr double kHeaderHeight = 36.0;
    static constexpr double kListWidth = 280.0;
    static constexpr double kCaptionHeight = 22.0;

    explicit ComparisonHostView(Environment environment);
    ~ComparisonHostView() override;

    void bind(ComparisonTabState* state);
    // The bound state's phase or result changed: rebuild the lists and labels
    // and attach the viewports once the documents are open.
    void refresh();
    ComparisonTabState* state() const { return state_; }

    // Change navigation (wraps around; no-op without differences).
    void nextChange();
    void previousChange();
    void selectChange(std::size_t index);
    std::optional<std::size_t> selectedChange() const { return selected_; }

    ui::PdfViewport& leftViewport() { return *leftViewport_; }
    ui::PdfViewport& rightViewport() { return *rightViewport_; }
    const ui::OutlinePanel& changeList() const { return *list_; }

    // The highlight rects painted over a page of one side (page display
    // points). Exposed for tests.
    std::vector<ui::OverlayRect> overlaysFor(bool leftSide, std::size_t pageIndex) const;

    bool onKey(const ui::KeyEvent& event) override;
    void layout() override;
    void paintSelf(ui::PaintContext& context) const override;

private:
    class SideBridge;

    void attachViewports();
    void detachViewports();
    void rebuildChanges();
    void updateLabels();
    void syncScroll(bool fromLeft);
    ui::PdfViewport& viewportOf(bool leftSide) { return leftSide ? *leftViewport_ : *rightViewport_; }
    const render::PageLayout* layoutOf(bool leftSide) const;

    Environment environment_;
    ComparisonTabState* state_ = nullptr;

    ui::Button* previousButton_ = nullptr;
    ui::Button* nextButton_ = nullptr;
    TextLabel* summary_ = nullptr;
    TextLabel* leftCaption_ = nullptr;
    TextLabel* rightCaption_ = nullptr;
    TextLabel* statusLabel_ = nullptr;
    ui::OutlinePanel* list_ = nullptr;
    ui::PdfViewport* leftViewport_ = nullptr;
    ui::PdfViewport* rightViewport_ = nullptr;
    std::unique_ptr<SideBridge> leftBridge_;
    std::unique_ptr<SideBridge> rightBridge_;

    // Which documents the viewports show now (null = nothing attached).
    const editor::DocumentSession* attachedLeft_ = nullptr;
    const editor::DocumentSession* attachedRight_ = nullptr;
    // Pages that exist on one side only (whole-page tint), indexed by page.
    std::vector<bool> leftExtraPages_;
    std::vector<bool> rightExtraPages_;
    std::optional<std::size_t> selected_;
    bool focusLeft_ = true;
    bool syncing_ = false;
};

} // namespace rivet::app
