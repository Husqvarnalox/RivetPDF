// SPDX-License-Identifier: MPL-2.0
#include "editor/SelectionModel.hpp"

#include <utility>

namespace rivet::editor {

void SelectionModel::start(TextPosition position) {
    selection_.anchor = position;
    selection_.focus = position;
    active_ = true;
    if (onChanged_) onChanged_();
}

void SelectionModel::setFocus(TextPosition position) {
    if (!active_) {
        start(position);
        return;
    }
    if (selection_.focus == position) return;
    selection_.focus = position;
    if (onChanged_) onChanged_();
}

void SelectionModel::extendTo(TextPosition position) {
    if (!active_) {
        // No selection to extend from: anchor the new selection here.
        selection_.anchor = position;
        active_ = true;
    }
    selection_.focus = position;
    if (onChanged_) onChanged_();
}

void SelectionModel::clear() {
    if (!active_) return;
    active_ = false;
    selection_ = TextSelection{};
    if (onChanged_) onChanged_();
}

void SelectionModel::setCallback(std::function<void()> onChanged) {
    onChanged_ = std::move(onChanged);
}

} // namespace rivet::editor
