// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ContentController.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/RedactionInteraction.hpp"
#include "app/RedactionLayer.hpp"
#include "app/ShellContext.hpp"

#include "core/Error.hpp"
#include "editor/Command.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace rivet::app {

// True redaction over the ACTIVE Ready PDF tab (ADR-0026): mark, review,
// apply.
//   - the Redact tool is ContentTool::Redact, i.e. part of the shell's ONE
//     tool state (choosing it suspends the annotation layer; choosing an
//     annotation or object tool ends it); while it is active a drag on a page
//     adds a mark, a click selects a mark and Delete removes it;
//   - Mark Selection for Redaction turns the text selection into marks (one
//     per line rect of the selection);
//   - Apply Redactions first builds the command (so a page the regeneration
//     probe refused, or anything still loading, is reported with its reason
//     and nothing happens), then asks for confirmation and whether the
//     document metadata should go too (unencrypted documents only), and runs
//     it as one undo step;
//   - Clear Marks removes every mark (one undo step).
// Marks are painted (red outlines) whatever the tool. Document text is never
// logged. Main thread only; the controller installs its layer on the
// viewport and removes it in the destructor.
class RedactionController final : public RedactionLayerClient {
public:
    RedactionController(ShellContext& context, ContentController& content);
    ~RedactionController() override;

    RedactionController(const RedactionController&) = delete;
    RedactionController& operator=(const RedactionController&) = delete;

    // Binds the active tab: ends a drag, clears the mark selection.
    void bindTab(DocumentTab* tab);

    // --- Menus ----------------------------------------------------------------
    bool canPerform(RedactCommand command) const;
    void perform(RedactCommand command);
    std::optional<bool> isChecked(RedactCommand command) const;

    // Esc priority hook for the shell: ends a drag or clears the mark
    // selection. False when there was nothing to do.
    bool handleEscape();

    // --- RedactionLayerClient (public: tests drive intents directly) ---------
    bool active() const override;
    RedactionInteraction& interaction() override { return interaction_; }
    const RedactionInteraction& interaction() const override { return interaction_; }
    std::optional<core::Size> pageSize(std::size_t page) const override;
    std::vector<core::Rect> marks(std::size_t page) const override;
    void applyIntent(const RedactionInteraction::Intent& intent) override;

private:
    DocumentTab* activeTab() const { return context_.readyActiveTab(); }
    bool textSelectionNonEmpty() const;
    // Runs a command on the active tab's session; reports failures as
    // "Redact: <reason>".
    bool run(core::Result<std::unique_ptr<editor::Command>> command);
    void markSelection();
    void applyMarks();
    void repaint();

    ShellContext& context_;
    ContentController& content_;
    RedactionInteraction interaction_;
    RedactionLayer layer_;
};

} // namespace rivet::app
