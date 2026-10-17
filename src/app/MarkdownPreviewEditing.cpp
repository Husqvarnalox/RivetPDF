// SPDX-License-Identifier: MPL-2.0
// WYSIWYG editing in the Rendered view (ADR-0031): caret, selection, keys,
// mouse and the translation of every gesture into one source edit. The
// source stays authoritative; see MarkdownPreviewView.hpp for the behaviour.
#include "app/MarkdownPreviewView.hpp"
#include "ui/Theme.hpp"

#include "markdown/Grapheme.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rivet::app {

namespace ed = markdown::editing;

namespace {

constexpr double kCaretWidth = 1.5;

bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\n'; }

std::size_t floorBoundary(std::string_view s, std::size_t offset) {
    offset = std::min(offset, s.size());
    while (offset > 0 && offset < s.size() && (static_cast<unsigned char>(s[offset]) & 0xC0) == 0x80) --offset;
    return offset;
}

bool looksLikeUrl(std::string_view text) {
    return text.starts_with("http://") || text.starts_with("https://") || text.starts_with("mailto:");
}

} // namespace

// ============================================================ target / state

void MarkdownPreviewView::setEditTarget(MarkdownTabState* state) {
    if (state == editState_) return;
    editState_ = state;
    clearEditState();
    if (editState_ != nullptr) knownRevision_ = editState_->revision();
    invalidate();
}

void MarkdownPreviewView::clearEditState() {
    anchor_ = caret_ = 0;
    preferredX_.reset();
    lastCommand_ = nullptr;
    trackValid_ = false;
    pendingKeys_.clear();
    journal_.clear();
    journalComplete_ = true;
    optimisticText_.clear();
    revealPending_ = false;
    editDragging_ = false;
    caretVisible_ = true;
}

bool MarkdownPreviewView::docFresh() const {
    return editState_ != nullptr && document_ != nullptr && revision_ == editState_->revision();
}

bool MarkdownPreviewView::layoutFresh() const {
    return docFresh() && layout_ != nullptr && !layoutDirty_ && !replaced_;
}

const markdown::CaretMap* MarkdownPreviewView::caretMap() const {
    if (layout_ == nullptr) return nullptr;
    if (caretMap_ == nullptr || caretMap_->layout() != layout_.get()) {
        caretMap_ = std::make_unique<markdown::CaretMap>(*layout_);
    }
    return caretMap_.get();
}

void MarkdownPreviewView::editTargetChanged() {
    if (editState_ == nullptr || inOwnEdit_) return;
    const std::uint64_t rev = editState_->revision();
    if (rev == knownRevision_) return;
    const MarkdownTabState::SourceChange& change = editState_->lastChange();
    if (change.revision == rev && knownRevision_ + 1 == rev) {
        journal_.push_back(JournalEntry{change.offset, change.removed, change.inserted, rev});
        // Undo/redo or the other pane: the caret goes to the end of the change.
        anchor_ = caret_ = change.offset + change.inserted;
    } else {
        journalComplete_ = false;
        const std::string& src = editState_->source();
        anchor_ = floorBoundary(src, anchor_);
        caret_ = floorBoundary(src, caret_);
    }
    knownRevision_ = rev;
    lastCommand_ = nullptr;
    trackValid_ = false;
    optimisticText_.clear();
    preferredX_.reset();
    invalidate();
}

// Maps an offset of the current source back into the snapshot the layout
// shows, through the changes made since (nullopt when they are not known).
std::optional<std::size_t> MarkdownPreviewView::toSnapshot(std::size_t offset) const {
    if (editState_ == nullptr || editState_->revision() == revision_) return offset;
    if (!journalComplete_) return std::nullopt;
    std::uint64_t expect = editState_->revision();
    for (auto it = journal_.rbegin(); it != journal_.rend(); ++it) {
        if (it->revision != expect) return std::nullopt;
        if (offset >= it->offset + it->inserted) {
            offset = offset - it->inserted + it->removed;
        } else if (offset > it->offset) {
            offset = it->offset;
        }
        --expect;
    }
    if (expect != revision_) return std::nullopt;
    return std::min(offset, source_.size());
}

// ================================================================= selection

void MarkdownPreviewView::setSelectionInternal(std::size_t anchor, std::size_t caret, bool notify) {
    if (editState_ == nullptr) return;
    const std::string& src = editState_->source();
    anchor = floorBoundary(src, anchor);
    caret = floorBoundary(src, caret);
    const bool changed = anchor != anchor_ || caret != caret_;
    anchor_ = anchor;
    caret_ = caret;
    restartBlink();
    invalidate();
    if (changed && notify && editHooks_.onCaretChanged) editHooks_.onCaretChanged(anchor_, caret_);
}

void MarkdownPreviewView::setCaretFromSource(std::size_t anchor, std::size_t caret, bool reveal) {
    if (editState_ == nullptr) return;
    trackValid_ = false;
    preferredX_.reset();
    setSelectionInternal(anchor, caret, false);
    if (reveal) {
        revealPending_ = true;
        if (layoutFresh()) revealCaretNow();
    }
}

void MarkdownPreviewView::moveCaret(std::size_t offset, bool extend) {
    trackValid_ = false;
    setSelectionInternal(extend ? anchor_ : offset, offset, true);
    revealCaretNow();
}

std::optional<markdown::TextPosition> MarkdownPreviewView::caretPosition() const {
    const markdown::CaretMap* map = caretMap();
    if (map == nullptr) return std::nullopt;
    const std::optional<std::size_t> snap = toSnapshot(caret_);
    if (!snap) return std::nullopt;
    return map->positionOf(*snap);
}

// The rendered content the selection covers, as a source range without the
// markup at its edges (Bold on rendered "bold" in "x **bold**" sees "bold").
std::pair<std::size_t, std::size_t> MarkdownPreviewView::contentSelection() const {
    std::size_t a = std::min(anchor_, caret_);
    std::size_t b = std::max(anchor_, caret_);
    const markdown::CaretMap* map = caretMap();
    if (a == b || map == nullptr || !layoutFresh()) return {a, b};
    const std::optional<markdown::TextPosition> pa = map->positionOf(a);
    const std::optional<markdown::TextPosition> pb = map->positionOf(b);
    if (!pa || !pb || !(*pa < *pb)) return {a, b};
    if (const auto next = map->next(*pa)) {
        if (const auto first = map->charBefore(*next); first && first->start >= a && first->start < b) a = first->start;
    }
    if (const auto last = map->charBefore(*pb); last && last->end <= b && last->end > a) b = last->end;
    return {a, b};
}

std::string MarkdownPreviewView::selectedEditText() const {
    if (editState_ == nullptr || anchor_ == caret_) return {};
    const std::size_t a = std::min(anchor_, caret_);
    const std::size_t b = std::max(anchor_, caret_);
    const markdown::CaretMap* map = caretMap();
    if (map != nullptr && layoutFresh()) {
        const auto pa = map->positionOf(a);
        const auto pb = map->positionOf(b);
        if (pa && pb && *pa < *pb) {
            std::string text = layout_->selectedText(*pa, *pb);
            if (!text.empty()) return text;
        }
    }
    // Nothing rendered there (an image block): the source itself.
    return editState_->source().substr(a, b - a);
}

// ==================================================================== edits

bool MarkdownPreviewView::commitEdit(const ed::Edit& edit, EditKind kind) {
    if (editState_ == nullptr) return false;
    if (!edit.changesText()) {
        if (edit.anchor != anchor_ || edit.caret != caret_) setSelectionInternal(edit.anchor, edit.caret, true);
        return false;
    }
    if (editState_->isEditingLocked()) {
        setStatus("A save is in progress — editing is paused");
        return false;
    }
    const std::string& src = editState_->source();
    if (edit.offset > src.size() || edit.removeLength > src.size() - edit.offset) return false;

    bool merge = false;
    const bool mergeable = kind == EditKind::Typing || kind == EditKind::Backspace || kind == EditKind::Delete;
    if (mergeable && trackValid_ && lastKind_ == kind && anchor_ == caret_ && caret_ == lastCaretAfter_ &&
        lastCommand_ != nullptr && editState_->commands().stateId() == lastStateId_ &&
        editState_->revision() == lastRevision_ && editState_->isDirty()) {
        merge = true;
        // A word typed after whitespace starts a new undo step (as in the Source view).
        if (kind == EditKind::Typing && edit.offset > 0 && !edit.insert.empty() && isBlank(src[edit.offset - 1]) &&
            !isBlank(edit.insert.front())) {
            merge = false;
        }
    }
    struct Guard {
        bool& flag;
        explicit Guard(bool& f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
    bool done = false;
    {
        const Guard guard(inOwnEdit_);
        if (merge) done = lastCommand_->tryMerge(edit.offset, edit.removeLength, edit.insert);
        if (!done) {
            std::string_view name = "Edit Text";
            switch (kind) {
            case EditKind::Typing: name = "Typing"; break;
            case EditKind::Backspace:
            case EditKind::Delete: name = "Delete"; break;
            case EditKind::NewLine: name = "New Line"; break;
            case EditKind::Paste: name = "Paste"; break;
            case EditKind::Cut: name = "Cut"; break;
            case EditKind::Format: name = "Format"; break;
            case EditKind::Other: break;
            }
            auto command = std::make_unique<TextEditCommand>(*editState_, edit.offset, edit.removeLength,
                                                             edit.insert, name);
            TextEditCommand* raw = command.get();
            if (!editState_->execute(std::move(command))) return false;
            lastCommand_ = raw;
            done = true;
        }
    }
    const std::uint64_t rev = editState_->revision();
    if (rev != revision_) {
        journal_.push_back(JournalEntry{edit.offset, edit.removeLength, edit.insert.size(), rev});
    }
    knownRevision_ = rev;
    lastStateId_ = editState_->commands().stateId();
    lastRevision_ = rev;
    lastKind_ = kind;
    preferredX_.reset();
    setSelectionInternal(edit.anchor, edit.caret, true);
    lastCaretAfter_ = caret_;
    trackValid_ = mergeable && anchor_ == caret_;
    revealPending_ = true;
    if (layoutFresh()) revealCaretNow();
    return true;
}

void MarkdownPreviewView::typeText(const std::string& text, EditKind kind) {
    if (editState_ == nullptr || text.empty()) return;
    const std::string& src = editState_->source();
    const std::size_t a = std::min(anchor_, caret_);
    const std::size_t b = std::max(anchor_, caret_);
    const bool multiline = text.find('\n') != std::string::npos || text.find('\r') != std::string::npos;
    const ed::Edit edit = multiline ? ed::pastePlainText(src, document_.get(), a, b, text)
                                    : ed::insertText(src, document_.get(), a, b, text);
    commitEdit(edit, multiline ? EditKind::Paste : kind);
}

// Typing while the document is still being parsed: compute the edit against
// the snapshot that IS parsed (at the caret mapped back into it) and move it
// forward through the changes made since. The old layout keeps showing; the
// typed text is drawn at the caret until the new layout lands.
bool MarkdownPreviewView::typeOptimistically(const std::string& text) {
    if (editState_ == nullptr || document_ == nullptr || anchor_ != caret_) return false;
    if (text.find('\n') != std::string::npos) return false;
    const std::optional<std::size_t> old = toSnapshot(caret_);
    if (!old) return false;
    const ed::Edit e = ed::insertText(source_, document_.get(), *old, *old, text);
    if (!e.changesText() || e.removeLength != 0) return false;
    // Forward through the journal: an insertion point at the burst position
    // continues after the text typed there already.
    std::size_t at = e.offset;
    for (const JournalEntry& j : journal_) {
        if (at >= j.offset + j.removed) at = at + j.inserted - j.removed;
        else if (at > j.offset) at = j.offset + j.inserted;
    }
    ed::Edit forward;
    forward.offset = at;
    forward.insert = e.insert;
    forward.anchor = forward.caret = at + e.insert.size();
    if (!commitEdit(forward, EditKind::Typing)) return false;
    if (!docFresh()) optimisticText_ += text;
    return true;
}

bool MarkdownPreviewView::applyFormat(MarkdownFormatCommand command) {
    if (editState_ == nullptr) return false;
    const std::string& src = editState_->source();
    ed::Edit edit;
    switch (command) {
    case MarkdownFormatCommand::Bold:
    case MarkdownFormatCommand::Italic:
    case MarkdownFormatCommand::Strikethrough:
    case MarkdownFormatCommand::Code:
    case MarkdownFormatCommand::Link: {
        const auto [a, b] = contentSelection();
        const ed::InlineFormat f = command == MarkdownFormatCommand::Bold     ? ed::InlineFormat::Bold
                                   : command == MarkdownFormatCommand::Italic ? ed::InlineFormat::Italic
                                   : command == MarkdownFormatCommand::Code   ? ed::InlineFormat::Code
                                   : command == MarkdownFormatCommand::Link   ? ed::InlineFormat::Link
                                                                              : ed::InlineFormat::Strikethrough;
        std::string url;
        if (f == ed::InlineFormat::Link) {
            const std::string_view selected = std::string_view(src).substr(a, b - a);
            if (looksLikeUrl(selected)) url.assign(selected);
            else if (editHooks_.linkTarget) url = editHooks_.linkTarget();
        }
        edit = ed::toggleInline(src, a, b, f, url);
        break;
    }
    default: {
        const auto block = static_cast<ed::BlockFormat>(static_cast<int>(command) -
                                                        static_cast<int>(MarkdownFormatCommand::Paragraph));
        edit = ed::applyBlockFormat(src, std::min(anchor_, caret_), std::max(anchor_, caret_), block);
        break;
    }
    }
    return commitEdit(edit, EditKind::Format);
}

bool MarkdownPreviewView::cutSelection() {
    if (editState_ == nullptr || anchor_ == caret_ || !docFresh()) return false;
    const std::string text = selectedEditText();
    if (env_.clipboard == nullptr || !env_.clipboard->setText(text)) {
        setStatus("Could not copy the selection");
        return false; // never lose the text
    }
    return commitEdit(ed::deleteRange(editState_->source(), document_.get(), std::min(anchor_, caret_),
                                      std::max(anchor_, caret_)),
                      EditKind::Cut);
}

bool MarkdownPreviewView::pasteFromClipboard() {
    if (editState_ == nullptr || env_.clipboard == nullptr || !docFresh()) return false;
    const std::string text = env_.clipboard->text();
    if (text.empty()) return false;
    const std::string& src = editState_->source();
    return commitEdit(ed::pastePlainText(src, document_.get(), std::min(anchor_, caret_), std::max(anchor_, caret_), text),
                      EditKind::Paste);
}

// ====================================================================== keys

namespace {

enum class KeyNeed : std::uint8_t { None, Document, Layout };

KeyNeed needOf(const ui::KeyEvent& event) {
    const bool primary = event.modifiers.command || event.modifiers.control;
    switch (event.key) {
    case ui::Key::Character:
        if (primary) {
            if (event.text == "a") return KeyNeed::None;
            if (event.text == "v") return KeyNeed::Document;
            return KeyNeed::Layout; // copy / cut use the rendered text
        }
        return KeyNeed::Document;
    case ui::Key::Space:
    case ui::Key::Plus:
    case ui::Key::Minus:
    case ui::Key::Enter:
    case ui::Key::Tab: return KeyNeed::Document;
    default: return KeyNeed::Layout;
    }
}

std::string typedText(const ui::KeyEvent& event) {
    switch (event.key) {
    case ui::Key::Space: return event.text.empty() ? std::string(" ") : event.text;
    case ui::Key::Plus: return !event.text.empty() ? event.text : std::string(event.modifiers.shift ? "+" : "=");
    case ui::Key::Minus: return event.text.empty() ? std::string("-") : event.text;
    case ui::Key::Character: {
        if (event.text.empty()) return {};
        const auto first = static_cast<unsigned char>(event.text.front());
        if ((first < 0x20 && first != '\t') || first == 0x7F) return {};
        // NSEvent function-key private-use code points (U+F700..U+F8FF).
        if (event.text.size() >= 3 && first == 0xEF && static_cast<unsigned char>(event.text[1]) >= 0x9C &&
            static_cast<unsigned char>(event.text[1]) <= 0xA3) {
            return {};
        }
        return event.text;
    }
    default: return {};
    }
}

} // namespace

bool MarkdownPreviewView::onEditKey(const ui::KeyEvent& event) {
    const bool primary = event.modifiers.command || event.modifiers.control;
    if (event.key == ui::Key::Escape || event.key == ui::Key::Unknown) return false;
    if (primary) {
        // The clipboard / select-all set is ours; every other shortcut
        // (undo, save, formatting, modes...) belongs to the shell.
        const bool clipboard = event.key == ui::Key::Character && !event.modifiers.option &&
                               (event.text == "a" || event.text == "c" || event.text == "x" || event.text == "v");
        // Cmd+arrows/Home/End move to line and document edges (as in the Source editor).
        const bool navigation = event.key == ui::Key::Left || event.key == ui::Key::Right ||
                                event.key == ui::Key::Up || event.key == ui::Key::Down ||
                                event.key == ui::Key::Home || event.key == ui::Key::End;
        const bool ours = clipboard || navigation;
        if (!ours) return false;
    } else if (event.key == ui::Key::Character && typedText(event).empty()) {
        return false;
    }
    event.accepted = true;
    const KeyNeed need = needOf(event);
    const bool ready = need == KeyNeed::None || (need == KeyNeed::Document ? docFresh() : layoutFresh());
    if (pendingKeys_.empty() && ready) {
        processEditKey(event);
        return true;
    }
    // Typing never waits for a background parse.
    if (pendingKeys_.empty() && !primary && need == KeyNeed::Document && event.key != ui::Key::Enter &&
        event.key != ui::Key::Tab) {
        const std::string text = typedText(event);
        if (!text.empty() && typeOptimistically(text)) return true;
    }
    pendingKeys_.push_back(event);
    invalidate();
    return true;
}

void MarkdownPreviewView::drainPendingKeys() {
    while (!pendingKeys_.empty()) {
        const ui::KeyEvent& next = pendingKeys_.front();
        const KeyNeed need = needOf(next);
        const bool ready = need == KeyNeed::None || (need == KeyNeed::Document ? docFresh() : layoutFresh());
        if (!ready) return;
        const ui::KeyEvent event = next;
        pendingKeys_.pop_front();
        processEditKey(event);
    }
}

bool MarkdownPreviewView::processEditKey(const ui::KeyEvent& event) {
    if (editState_ == nullptr) return false;
    const ui::ModifierFlags& mods = event.modifiers;
    const bool primary = mods.command || mods.control;
    const bool shift = mods.shift;
    const std::string& src = editState_->source();
    const markdown::CaretMap* map = caretMap();

    if (primary && event.key == ui::Key::Character) {
        if (event.text == "a") {
            setSelectionInternal(0, src.size(), true);
        } else if (event.text == "c") {
            copySelection();
        } else if (event.text == "x") {
            cutSelection();
        } else if (event.text == "v") {
            pasteFromClipboard();
        }
        return true;
    }

    const bool vertical = event.key == ui::Key::Up || event.key == ui::Key::Down || event.key == ui::Key::PageUp ||
                          event.key == ui::Key::PageDown;
    if (!vertical) preferredX_.reset();
    const std::size_t selA = std::min(anchor_, caret_);
    const std::size_t selB = std::max(anchor_, caret_);

    // Neighbouring stops of a caret that is not on one (a blank line opened by Enter).
    const auto here = [&]() -> std::optional<markdown::TextPosition> { return caretPosition(); };
    const auto leftOf = [&](markdown::TextPosition p) -> std::optional<markdown::TextPosition> {
        if (map->sourceOf(p) < caret_) return p;
        if (mods.command) return map->lineStart(p);
        if (mods.option) return map->wordLeft(p);
        return map->previous(p);
    };
    const auto rightOf = [&](markdown::TextPosition p) -> std::optional<markdown::TextPosition> {
        if (map->sourceOf(p) > caret_) return p;
        if (mods.command) return map->lineEnd(p);
        if (mods.option) return map->wordRight(p);
        return map->next(p);
    };

    switch (event.key) {
    case ui::Key::Left:
    case ui::Key::Right: {
        const bool left = event.key == ui::Key::Left;
        if (!shift && selA != selB && !mods.command && !mods.option) {
            moveCaret(left ? selA : selB, false);
            break;
        }
        if (map == nullptr) break;
        const auto p = here();
        if (!p) break;
        const auto target = left ? leftOf(*p) : rightOf(*p);
        if (target) moveCaret(map->sourceOf(*target), shift);
        break;
    }
    case ui::Key::Up:
    case ui::Key::Down:
    case ui::Key::PageUp:
    case ui::Key::PageDown: {
        if (map == nullptr || map->empty()) break;
        const bool up = event.key == ui::Key::Up || event.key == ui::Key::PageUp;
        if (mods.command && (event.key == ui::Key::Up || event.key == ui::Key::Down)) {
            moveCaret(map->sourceOf(up ? *map->first() : *map->last()), shift);
            break;
        }
        const auto p = here();
        if (!p) break;
        // A caret on a blank line between blocks (a gap) is drawn at the
        // left edge of that line; its neighbours are the stops around it.
        const std::size_t at = map->sourceOf(*p);
        const bool gap = at != caret_;
        double x = 0.0;
        if (preferredX_) x = *preferredX_;
        else if (const std::optional<core::Rect> r = gap ? caretDocRect() : std::nullopt) x = r->minX();
        else x = layout_->xForPosition(*p, measurer_);
        long lines = 1;
        if (event.key == ui::Key::PageUp || event.key == ui::Key::PageDown) {
            const double lineH = typography_.bodySize * typography_.lineHeightFactor;
            lines = std::max(1L, static_cast<long>(bounds().size.height * 0.9 / lineH));
            scrollBy(up ? -bounds().size.height * 0.9 : bounds().size.height * 0.9);
        }
        markdown::TextPosition from = *p;
        if (gap && (at < caret_) != up) {
            // The snapped stop lies on the far side: the first line in the
            // direction of travel is the one right next to the gap.
            const std::optional<markdown::TextPosition> beyond = up ? map->previous(*p) : map->next(*p);
            if (beyond) from = *beyond;
        }
        markdown::TextPosition target = map->onLineAtX(map->lineOf(from), x, measurer_);
        if (gap) {
            if (lines > 1) target = map->vertical(target, x, up ? -(lines - 1) : lines - 1, measurer_);
        } else {
            target = map->vertical(*p, x, up ? -lines : lines, measurer_);
        }
        moveCaret(map->sourceOf(target), shift);
        preferredX_ = x;
        break;
    }
    case ui::Key::Home:
    case ui::Key::End: {
        if (map == nullptr || map->empty()) break;
        const bool home = event.key == ui::Key::Home;
        if (mods.command) {
            moveCaret(map->sourceOf(home ? *map->first() : *map->last()), shift);
            break;
        }
        const auto p = here();
        if (p) moveCaret(map->sourceOf(home ? map->lineStart(*p) : map->lineEnd(*p)), shift);
        break;
    }
    case ui::Key::Backspace:
    case ui::Key::Delete: {
        const bool forward = event.key == ui::Key::Delete;
        const EditKind kind = forward ? EditKind::Delete : EditKind::Backspace;
        if (selA != selB) {
            commitEdit(ed::deleteRange(src, document_.get(), selA, selB), kind);
            break;
        }
        if (map == nullptr) break;
        if (mods.option || mods.command) {
            const auto p = here();
            if (!p) break;
            const auto other = forward ? (mods.command ? std::optional(map->lineEnd(*p)) : map->wordRight(*p))
                                       : (mods.command ? std::optional(map->lineStart(*p)) : map->wordLeft(*p));
            if (!other) break;
            const std::size_t o = map->sourceOf(*other);
            if (o != caret_) commitEdit(ed::deleteRange(src, document_.get(), std::min(o, caret_), std::max(o, caret_)), kind);
            break;
        }
        const ed::DeleteResult res = ed::deleteCharacter(src, document_.get(), *map, caret_, forward);
        if (res.selectIsland) {
            trackValid_ = false;
            setSelectionInternal(res.selectIsland->start, res.selectIsland->end, true);
        } else if (!res.blocked) {
            commitEdit(res.edit, kind);
        }
        break;
    }
    case ui::Key::Enter:
        commitEdit(shift ? ed::insertLineBreak(src, document_.get(), selA, selB)
                         : ed::insertParagraphBreak(src, document_.get(), selA, selB),
                   EditKind::NewLine);
        break;
    case ui::Key::Tab: {
        const ed::Context ctx = ed::contextAt(document_.get(), selA);
        if (ctx.editableCode) typeText("    ", EditKind::Typing);
        break;
    }
    case ui::Key::Space:
    case ui::Key::Plus:
    case ui::Key::Minus:
    case ui::Key::Character: {
        const std::string text = typedText(event);
        if (!text.empty()) typeText(text, EditKind::Typing);
        break;
    }
    default: break;
    }
    return true;
}

// ===================================================================== mouse

bool MarkdownPreviewView::onEditMouse(const ui::PointerEvent& event) {
    switch (event.type) {
    case ui::PointerEventType::Down: {
        if (event.button != 1 || !bounds().contains(event.position)) return false;
        event.accepted = true;
        const auto now = std::chrono::steady_clock::now();
        const double dist = std::hypot(event.position.x - lastClickPos_.x, event.position.y - lastClickPos_.y);
        editClickCount_ = (editClickCount_ > 0 && now - lastClickTime_ < std::chrono::milliseconds(450) && dist < 5.0)
                              ? editClickCount_ % 3 + 1
                              : 1;
        lastClickTime_ = now;
        lastClickPos_ = event.position;
        const markdown::HitResult hit = hitAt(event.position);
        // Cmd/Ctrl+click follows a link; a plain click edits.
        if ((event.modifiers.command || event.modifiers.control) && editClickCount_ == 1) {
            const std::string url = linkUrlAt(hit);
            if (!url.empty()) {
                activateLink(url);
                return true;
            }
        }
        if (!layoutFresh()) return true;
        const markdown::CaretMap* map = caretMap();
        // An image block is selected as a whole.
        const double docY = event.position.y + scrollY_;
        const std::size_t bi = layout_->blockIndexAtY(docY);
        if (bi < layout_->blocks.size()) {
            const markdown::LayoutBlock& b = layout_->blocks[bi];
            if (b.kind == markdown::LayoutBlockKind::Image && b.rect.contains(core::Point{event.position.x, docY})) {
                trackValid_ = false;
                setSelectionInternal(b.source.start, b.source.end, true);
                editDragging_ = false;
                return true;
            }
        }
        if (!hit.valid || map == nullptr) return true;
        const std::optional<markdown::TextPosition> pos = map->snap(hit.position);
        if (!pos) return true;
        const std::size_t offset = map->sourceOf(*pos);
        editDragging_ = true;
        preferredX_.reset();
        trackValid_ = false;
        if (editClickCount_ == 2) {
            const auto [a, b] = map->wordAt(*pos);
            dragWordBegin_ = map->sourceOf(a);
            dragWordEnd_ = map->sourceOf(b);
            setSelectionInternal(dragWordBegin_, dragWordEnd_, true);
        } else if (editClickCount_ == 3) {
            const markdown::LayoutBlock& block = layout_->blocks[map->blockOf(*pos)];
            const auto first = map->positionOf(block.source.start);
            const auto last = map->positionOf(block.source.end);
            dragWordBegin_ = first ? map->sourceOf(*first) : offset;
            dragWordEnd_ = last ? map->sourceOf(*last) : offset;
            setSelectionInternal(dragWordBegin_, dragWordEnd_, true);
        } else if (event.modifiers.shift) {
            setSelectionInternal(anchor_, offset, true);
        } else {
            setSelectionInternal(offset, offset, true);
        }
        return true;
    }
    case ui::PointerEventType::Move: {
        if (event.button == 0) {
            editDragging_ = false;
            const markdown::HitResult hit = hitAt(event.position);
            const std::uint32_t link = (!linkUrlAt(hit).empty()) ? hit.link : 0u;
            if (link != hoverLink_) {
                hoverLink_ = link;
                invalidate();
            }
            return false;
        }
        if (!editDragging_ || !layoutFresh()) return editDragging_;
        const double h = bounds().size.height;
        if (event.position.y < 0.0) {
            scrollBy(-std::min(40.0, 4.0 - event.position.y * 0.5));
        } else if (event.position.y > h) {
            scrollBy(std::min(40.0, 4.0 + (event.position.y - h) * 0.5));
        }
        const markdown::CaretMap* map = caretMap();
        const markdown::HitResult hit = hitAt(event.position);
        if (!hit.valid || map == nullptr) return true;
        const std::optional<markdown::TextPosition> pos = map->snap(hit.position);
        if (!pos) return true;
        std::size_t offset = map->sourceOf(*pos);
        if (editClickCount_ == 2) {
            const auto [a, b] = map->wordAt(*pos);
            if (map->sourceOf(a) < dragWordBegin_) {
                setSelectionInternal(dragWordEnd_, map->sourceOf(a), true);
            } else {
                setSelectionInternal(dragWordBegin_, std::max(map->sourceOf(b), dragWordEnd_), true);
            }
        } else if (editClickCount_ == 1) {
            setSelectionInternal(anchor_, offset, true);
        }
        event.accepted = true;
        return true;
    }
    case ui::PointerEventType::Up:
        if (!editDragging_) return false;
        editDragging_ = false;
        event.accepted = true;
        return true;
    default: return false;
    }
}

// ============================================================== caret / paint

std::optional<core::Rect> MarkdownPreviewView::caretDocRect() const {
    if (editState_ == nullptr || layout_ == nullptr) return std::nullopt;
    const std::optional<std::size_t> snap = toSnapshot(caret_);
    if (!snap) return std::nullopt;
    const markdown::CaretMap* map = caretMap();
    const double lineH = typography_.bodySize * typography_.lineHeightFactor;
    std::optional<markdown::TextPosition> pos = map != nullptr ? map->positionOf(*snap) : std::nullopt;
    bool gap = !pos;
    if (pos && map->sourceOf(*pos) != *snap) {
        // Off the rendered text: on a blank line of its own (opened by Enter,
        // an empty list item) the caret is drawn there.
        gap = ed::scanLinePrefix(source_, *snap).blank();
    }
    if (!gap) {
        const markdown::LayoutRun& run = layout_->runs[pos->run];
        const markdown::LayoutLine& line = layout_->lines[run.line];
        double x = layout_->xForPosition(*pos, measurer_);
        const std::size_t bi = map->blockOf(*pos);
        if (bi < layout_->blocks.size() && layout_->blocks[bi].overflowsHorizontally()) x -= blockScrollX(bi);
        return core::Rect{x, line.top, kCaretWidth, line.height};
    }
    const markdown::LayoutBlock* prev = nullptr;
    const markdown::LayoutBlock* next = nullptr;
    for (const markdown::LayoutBlock& b : layout_->blocks) {
        if (b.source.end <= *snap) prev = &b;
        else if (b.source.start > *snap) {
            next = &b;
            break;
        }
    }
    const double x = next != nullptr ? next->rect.minX() : prev != nullptr ? prev->rect.minX() : typography_.pageMarginX;
    double y = typography_.pageMarginY;
    if (prev != nullptr) y = prev->rect.maxY() + typography_.paragraphGap * 0.5;
    else if (next != nullptr) y = std::max(0.0, next->rect.minY() - lineH);
    return core::Rect{x, y, kCaretWidth, lineH};
}

std::optional<core::Rect> MarkdownPreviewView::caretRect() const {
    const std::optional<core::Rect> r = caretDocRect();
    if (!r) return std::nullopt;
    return core::Rect{r->minX(), r->minY() - scrollY_, r->size.width, r->size.height};
}

void MarkdownPreviewView::revealCaretNow() {
    revealPending_ = false;
    const std::optional<core::Rect> r = caretDocRect();
    if (!r) return;
    const double viewH = bounds().size.height;
    const double margin = std::min(8.0, viewH / 4.0);
    if (r->minY() < scrollY_ + margin) {
        setScrollY(r->minY() - margin);
    } else if (r->maxY() > scrollY_ + viewH - margin) {
        setScrollY(r->maxY() - viewH + margin);
    }
    // A caret inside a horizontally overflowing code block scrolls the block.
    if (const std::optional<markdown::TextPosition> pos = caretPosition(); pos && caretMap() != nullptr) {
        const std::size_t bi = caretMap()->blockOf(*pos);
        if (bi < layout_->blocks.size() && layout_->blocks[bi].overflowsHorizontally()) {
            const markdown::LayoutBlock& block = layout_->blocks[bi];
            const double x = layout_->xForPosition(*pos, measurer_);
            const double off = blockScrollX(bi);
            if (x < block.rect.minX() + off + 8.0 || x > block.rect.maxX() + off - 24.0) {
                scrollBlockBy(bi, (x - block.rect.minX() - block.rect.size.width / 2.0) - off);
            }
        }
    }
}

void MarkdownPreviewView::restartBlink() {
    caretVisible_ = true;
    caretMoved_ = std::chrono::steady_clock::now();
    scheduleBlink();
}

void MarkdownPreviewView::scheduleBlink() {
    if (blinkPending_ || tuning_.caretBlink.count() <= 0 || env_.dispatcher == nullptr || editState_ == nullptr ||
        !isFocused()) {
        return;
    }
    // A main-thread timer (QA bug 15: the blink used to sleep on the worker
    // pool, keeping one pool thread busy while an editor was focused).
    const std::weak_ptr<int> alive = alive_;
    const auto delay = tuning_.caretBlink;
    blinkPending_ = env_.dispatcher->postDelayed(delay, [alive, this, delay] {
        if (!alive.lock()) return;
        blinkPending_ = false;
        if (editState_ == nullptr || !isFocused()) {
            caretVisible_ = true;
            return;
        }
        if (std::chrono::steady_clock::now() - caretMoved_ >= delay) caretVisible_ = !caretVisible_;
        invalidate();
        scheduleBlink();
    });
}

void MarkdownPreviewView::paintEditSelection(ui::PaintContext& context) const {
    if (editState_ == nullptr || anchor_ == caret_ || layout_ == nullptr) return;
    const std::optional<std::size_t> a = toSnapshot(std::min(anchor_, caret_));
    const std::optional<std::size_t> b = toSnapshot(std::max(anchor_, caret_));
    const markdown::CaretMap* map = caretMap();
    if (!a || !b || map == nullptr) return;
    const ui::Color color = isFocused() ? palette().selection : palette().selectionInactive;
    const auto pa = map->positionOf(*a);
    const auto pb = map->positionOf(*b);
    if (pa && pb && *pa < *pb) {
        for (const core::Rect& r : layout_->selectionRects(*pa, *pb, measurer_)) fillDocRect(context, r, color);
    }
    // Islands inside the selection (an image block) highlight as a whole.
    for (const markdown::LayoutBlock& block : layout_->blocks) {
        if (!block.readOnlyIsland() || block.source.start < *a || block.source.end > *b) continue;
        fillDocRect(context, block.rect, color);
    }
}

void MarkdownPreviewView::paintEditing(ui::PaintContext& context) const {
    if (editState_ == nullptr || !isFocused() || anchor_ != caret_) return;
    if (!revealPending_ && !caretVisible_ && optimisticText_.empty()) return;
    const std::optional<core::Rect> r = caretDocRect();
    if (!r) return;
    double x = r->minX();
    const double top = r->minY() - scrollY_;
    if (!optimisticText_.empty() && !docFresh()) {
        // Typed while the parse runs: show it where it will appear.
        markdown::TextStyle style;
        if (const auto pos = caretPosition()) style = layout_->runs[pos->run].style;
        const ui::Font font = fontFor(style);
        const double lh = measurer_.lineHeight(style);
        const double width = context.measureText(optimisticText_, font).width;
        context.fillRect(core::Rect{x, top, width, r->size.height}, palette().background);
        context.drawText(optimisticText_, core::Rect{x, top + (r->size.height - lh) / 2.0, width + 8.0, lh}, font,
                         colorFor(style), ui::TextAlign::Left);
        x += width;
    }
    if (caretVisible_) context.fillRect(core::Rect{x, top, r->size.width, r->size.height}, palette().text);
}

} // namespace rivet::app
