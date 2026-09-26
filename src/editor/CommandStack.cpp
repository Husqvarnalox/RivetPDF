#include "editor/CommandStack.hpp"

namespace rivet::editor {

CommandStack::CommandStack(std::size_t maxDepth) : maxDepth_(maxDepth) {}

bool CommandStack::execute(std::unique_ptr<Command> command) {
    if (!command) {
        lastError_.reset();
        return false;
    }
    if (!command->execute()) {
        lastError_ = command->failure();
        return false; // failed commands never enter the history
    }
    lastError_.reset();
    undo_.push_back(Entry{std::move(command), ++lastStateId_});
    trimToMaxDepth();
    redo_.clear();
    notifyChanged();
    return true;
}

bool CommandStack::undo() {
    if (undo_.empty()) {
        return false;
    }
    Entry entry = std::move(undo_.back());
    undo_.pop_back();
    if (!entry.command->undo()) {
        // Leave the history exactly as it was.
        lastError_ = entry.command->failure();
        undo_.push_back(std::move(entry));
        return false;
    }
    lastError_.reset();
    redo_.push_back(std::move(entry));
    notifyChanged();
    return true;
}

bool CommandStack::redo() {
    if (redo_.empty()) {
        return false;
    }
    Entry entry = std::move(redo_.back());
    redo_.pop_back();
    if (!entry.command->redo()) {
        lastError_ = entry.command->failure();
        redo_.push_back(std::move(entry));
        return false;
    }
    lastError_.reset();
    undo_.push_back(std::move(entry));
    trimToMaxDepth();
    notifyChanged();
    return true;
}

bool CommandStack::canUndo() const {
    return !undo_.empty();
}

bool CommandStack::canRedo() const {
    return !redo_.empty();
}

std::size_t CommandStack::depth() const {
    return undo_.size();
}

std::uint64_t CommandStack::stateId() const {
    return undo_.empty() ? baseStateId_ : undo_.back().stateId;
}

void CommandStack::clear() {
    baseStateId_ = stateId(); // the document stays where it is
    undo_.clear();
    redo_.clear();
    notifyChanged();
}

void CommandStack::setMaxDepth(std::size_t maxDepth) {
    maxDepth_ = maxDepth;
    trimToMaxDepth();
}

std::size_t CommandStack::maxDepth() const {
    return maxDepth_;
}

void CommandStack::setOnChanged(std::function<void()> callback) {
    onChanged_ = std::move(callback);
}

void CommandStack::trimToMaxDepth() {
    while (undo_.size() > maxDepth_) {
        // Oldest history is what gets evicted; the state it produced becomes
        // the bottom of the reachable history.
        baseStateId_ = undo_.front().stateId;
        undo_.pop_front();
    }
}

void CommandStack::notifyChanged() {
    if (onChanged_) {
        onChanged_();
    }
}

} // namespace rivet::editor
