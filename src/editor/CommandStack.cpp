#include "editor/CommandStack.hpp"

namespace rivet::editor {

CommandStack::CommandStack(std::size_t maxDepth) : maxDepth_(maxDepth) {}

bool CommandStack::execute(std::unique_ptr<Command> command) {
    if (!command) {
        return false;
    }
    if (!command->execute()) {
        return false; // failed commands never enter the history
    }
    undo_.push_back(std::move(command));
    trimToMaxDepth();
    redo_.clear();
    notifyChanged();
    return true;
}

bool CommandStack::undo() {
    if (undo_.empty()) {
        return false;
    }
    auto command = std::move(undo_.back());
    undo_.pop_back();
    if (!command->undo()) {
        // Leave the history exactly as it was.
        undo_.push_back(std::move(command));
        return false;
    }
    redo_.push_back(std::move(command));
    notifyChanged();
    return true;
}

bool CommandStack::redo() {
    if (redo_.empty()) {
        return false;
    }
    auto command = std::move(redo_.back());
    redo_.pop_back();
    if (!command->redo()) {
        redo_.push_back(std::move(command));
        return false;
    }
    undo_.push_back(std::move(command));
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

void CommandStack::clear() {
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
        undo_.pop_front(); // oldest history is what gets evicted
    }
}

void CommandStack::notifyChanged() {
    if (onChanged_) {
        onChanged_();
    }
}

} // namespace rivet::editor
