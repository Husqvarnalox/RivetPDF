// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "editor/ContentCommands.hpp"
#include "editor/ContentObjects.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace rivet::editor {
class DocumentSession;
}

namespace rivet::app {

// The seam between the content UI (ContentController) and the editor layer's
// content service and command factories. The production implementation
// (createEditorContentBackend) forwards to DocumentSession::contentService()
// and the editor/ContentCommands.hpp factories; tests substitute a fake that
// serves hand-made objects and simple undoable commands, so the whole
// interaction layer is testable without a PDF backend.
//
// Main thread only. Nothing here logs document text.
class ContentBackend {
public:
    virtual ~ContentBackend() = default;

    // --- Reading ----------------------------------------------------------------
    virtual editor::PageContentViewPtr content(editor::DocumentSession& session, core::PageId page) const = 0;
    virtual std::optional<editor::ContentObjectView> findObject(editor::DocumentSession& session, core::PageId page,
                                                                core::ObjectId id) const = 0;
    virtual std::optional<editor::TextBlockView> findBlock(editor::DocumentSession& session, core::PageId page,
                                                           core::ObjectId blockId) const = 0;
    // Topmost hit within `tolerancePoints` (display points); read-only
    // objects are hit too so the UI can explain why they cannot be edited.
    virtual std::optional<editor::ContentHit> hitTest(editor::DocumentSession& session, core::PageId page,
                                                      core::Point display, double tolerancePoints) const = 0;

    // --- Commands (the editor/ContentCommands.hpp factories) ------------------
    virtual core::Result<editor::ContentEdit> moveContent(editor::DocumentSession& session, core::PageId page,
                                                          std::vector<core::ObjectId> ids,
                                                          core::Point displayDelta) const = 0;
    virtual core::Result<editor::ContentEdit> deleteContent(editor::DocumentSession& session, core::PageId page,
                                                            std::vector<core::ObjectId> ids) const = 0;
    virtual core::Result<editor::ContentEdit> resizeContent(editor::DocumentSession& session, core::PageId page,
                                                            core::ObjectId id, core::Rect displayBounds) const = 0;
    virtual core::Result<editor::ContentEdit>
    replaceImage(editor::DocumentSession& session, core::PageId page, core::ObjectId id,
                 std::shared_ptr<const pdf::PdfImageData> image) const = 0;
    virtual core::Result<editor::ContentEdit> editTextBlock(editor::DocumentSession& session, core::PageId page,
                                                            core::ObjectId blockId,
                                                            editor::TextBlockPatch patch) const = 0;
    virtual core::Result<editor::ContentEdit> addTextBlock(editor::DocumentSession& session, core::PageId page,
                                                           editor::NewTextBlock block) const = 0;
    virtual core::Result<editor::ContentEdit> bringToFront(editor::DocumentSession& session, core::PageId page,
                                                           core::ObjectId id) const = 0;
    // Characters the bundled face cannot write (empty = all covered).
    virtual std::u32string uncoveredCodepoints(pdf::PdfBundledFont font, std::string_view utf8) const = 0;

    // --- Change notification ------------------------------------------------------
    // Installs / clears (null) the observer fired on the main thread when a
    // page's content finished loading. One observer per session.
    virtual void observe(editor::DocumentSession& session, std::function<void(core::PageId)> onChanged) const = 0;
};

// The production backend over the editor layer.
std::unique_ptr<ContentBackend> createEditorContentBackend();

} // namespace rivet::app
