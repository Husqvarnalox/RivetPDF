// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownSourcePane.hpp"

#include "ui/Utf8.hpp"

#include <algorithm>
#include <utility>

namespace rivet::app {

namespace {

std::string_view commandName(ui::SourceEditKind kind) {
    switch (kind) {
    case ui::SourceEditKind::Typing: return "Typing";
    case ui::SourceEditKind::Backspace:
    case ui::SourceEditKind::DeleteForward:
    case ui::SourceEditKind::DeleteWord:
    case ui::SourceEditKind::DeleteLine: return "Delete";
    case ui::SourceEditKind::Newline: return "New Line";
    case ui::SourceEditKind::Paste: return "Paste";
    case ui::SourceEditKind::Cut: return "Cut";
    case ui::SourceEditKind::Indent: return "Indent";
    case ui::SourceEditKind::Other: break;
    }
    return "Edit Text";
}

} // namespace

class MarkdownSourcePane::Sink final : public ui::ISourceEditSink {
public:
    explicit Sink(MarkdownSourcePane& pane) : pane_(pane) {}

    bool applyEdit(const ui::SourceEdit& edit) override {
        MarkdownTabState* state = pane_.state_;
        if (state == nullptr) return false;
        if (state->isEditingLocked()) {
            if (pane_.environment_.setStatus) pane_.environment_.setStatus("A save is in progress — editing is paused");
            return false;
        }
        struct Guard {
            bool& flag;
            explicit Guard(bool& f) : flag(f) { flag = true; }
            ~Guard() { flag = false; }
        } guard(pane_.inOwnEdit_);

        bool done = false;
        if (edit.mergeWithPrevious && pane_.lastCommand_ != nullptr &&
            state->commands().stateId() == pane_.lastStateId_ && state->revision() == pane_.lastRevision_ &&
            state->isDirty()) {
            done = pane_.lastCommand_->tryMerge(edit.offset, edit.removeLength, edit.insert);
        }
        if (!done) {
            auto command = std::make_unique<TextEditCommand>(*state, edit.offset, edit.removeLength,
                                                             std::string(edit.insert), commandName(edit.kind));
            TextEditCommand* raw = command.get();
            if (!state->execute(std::move(command))) return false;
            pane_.lastCommand_ = raw;
        }
        pane_.lastStateId_ = state->commands().stateId();
        pane_.lastRevision_ = state->revision();
        pane_.syncedRevision_ = state->revision();
        return true;
    }

private:
    MarkdownSourcePane& pane_;
};

class MarkdownSourcePane::Clipboard final : public ui::ITextClipboard {
public:
    explicit Clipboard(platform::IClipboard* clipboard) : clipboard_(clipboard) {}
    std::string text() const override { return clipboard_ != nullptr ? clipboard_->text() : std::string(); }
    bool setText(const std::string& text) override {
        return clipboard_ != nullptr && clipboard_->setText(text).has_value();
    }

private:
    platform::IClipboard* clipboard_;
};

MarkdownSourcePane::MarkdownSourcePane(MarkdownSourcePaneEnvironment environment)
    : environment_(std::move(environment)),
      sink_(std::make_unique<Sink>(*this)),
      clipboard_(std::make_unique<Clipboard>(environment_.services != nullptr ? environment_.services->clipboard
                                                                              : nullptr)) {
    auto editor = std::make_unique<ui::SourceEditor>();
    editor_ = editor.get();
    editor_->setSink(sink_.get());
    editor_->setClipboard(clipboard_.get());
    editor_->setOnFocusRequested([this] { focusEditor(); });
    addChild(std::move(editor));
}

MarkdownSourcePane::~MarkdownSourcePane() = default;

void MarkdownSourcePane::layout() { editor_->setFrame(bounds()); }

void MarkdownSourcePane::focusEditor() {
    if (environment_.setFocus) environment_.setFocus(editor_);
}

void MarkdownSourcePane::rememberView() {
    if (state_ == nullptr) return;
    const ui::SourceEditor::Selection sel = editor_->selection();
    const bool caretAtEnd = editor_->caretOffset() == sel.end;
    views_[state_] = ViewState{caretAtEnd ? sel.begin : sel.end, editor_->caretOffset(), editor_->topVisibleOffset()};
}

void MarkdownSourcePane::bind(MarkdownTabState* state) {
    if (state == state_) {
        if (state_ != nullptr) stateChanged();
        return;
    }
    rememberView();
    state_ = state;
    lastCommand_ = nullptr;
    if (state_ == nullptr) {
        editor_->setText({});
        return;
    }
    editor_->setText(state_->source());
    syncedRevision_ = state_->revision();
    const auto it = views_.find(state_);
    if (it != views_.end()) {
        editor_->setSelection(it->second.anchor, it->second.caret);
        editor_->scrollToOffset(it->second.topOffset);
    }
}

void MarkdownSourcePane::stateChanged() {
    if (inOwnEdit_ || state_ == nullptr) return;
    if (state_->revision() == syncedRevision_) return;
    resyncFromState();
}

// The state's source moved without the editor's knowledge (undo/redo): turn
// the difference into one replacement on the mirror and park the caret at the
// end of the changed region.
void MarkdownSourcePane::resyncFromState() {
    const std::string& source = state_->source();
    const std::string& current = editor_->buffer().text();
    syncedRevision_ = state_->revision();
    lastCommand_ = nullptr; // history moved: never merge into an old step

    const std::size_t common = std::min(source.size(), current.size());
    std::size_t prefix = static_cast<std::size_t>(
        std::mismatch(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(common), current.begin()).first -
        source.begin());
    prefix = ui::utf8::floorBoundary(source, prefix);
    std::size_t suffix = 0;
    while (suffix < common - prefix && source[source.size() - 1 - suffix] == current[current.size() - 1 - suffix]) {
        ++suffix;
    }
    // The suffix must start on a code-point boundary (in both strings alike).
    while (suffix > 0 && ui::utf8::isContinuationByte(source[source.size() - suffix])) --suffix;

    const std::size_t removeLength = current.size() - prefix - suffix;
    const std::size_t insertLength = source.size() - prefix - suffix;
    if (removeLength == 0 && insertLength == 0) return;
    editor_->applyExternalEdit(prefix, removeLength, std::string_view(source).substr(prefix, insertLength));
    editor_->setCaretOffset(prefix + insertLength);
}

std::unique_ptr<MarkdownSourcePane> createMarkdownSourcePane(MarkdownSourcePaneEnvironment environment) {
    return std::make_unique<MarkdownSourcePane>(std::move(environment));
}

} // namespace rivet::app
