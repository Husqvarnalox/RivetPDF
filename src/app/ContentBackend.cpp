// SPDX-License-Identifier: MPL-2.0
#include "app/ContentBackend.hpp"

#include "editor/ContentService.hpp"
#include "editor/DocumentSession.hpp"

#include <utility>

namespace rivet::app {

namespace {

class EditorContentBackend final : public ContentBackend {
public:
    editor::PageContentViewPtr content(editor::DocumentSession& session, core::PageId page) const override {
        return session.contentService().content(page);
    }
    std::optional<editor::ContentObjectView> findObject(editor::DocumentSession& session, core::PageId page,
                                                        core::ObjectId id) const override {
        return session.contentService().findObject(page, id);
    }
    std::optional<editor::TextBlockView> findBlock(editor::DocumentSession& session, core::PageId page,
                                                   core::ObjectId blockId) const override {
        return session.contentService().findBlock(page, blockId);
    }
    std::optional<editor::ContentHit> hitTest(editor::DocumentSession& session, core::PageId page,
                                              core::Point display, double tolerancePoints) const override {
        return session.contentService().hitTest(page, display, tolerancePoints, /*editableOnly=*/false);
    }

    core::Result<editor::ContentEdit> moveContent(editor::DocumentSession& session, core::PageId page,
                                                  std::vector<core::ObjectId> ids,
                                                  core::Point displayDelta) const override {
        return editor::moveContent(session, page, std::move(ids), displayDelta);
    }
    core::Result<editor::ContentEdit> deleteContent(editor::DocumentSession& session, core::PageId page,
                                                    std::vector<core::ObjectId> ids) const override {
        return editor::deleteContent(session, page, std::move(ids));
    }
    core::Result<editor::ContentEdit> resizeContent(editor::DocumentSession& session, core::PageId page,
                                                    core::ObjectId id, core::Rect displayBounds) const override {
        return editor::resizeContent(session, page, id, displayBounds);
    }
    core::Result<editor::ContentEdit> replaceImage(editor::DocumentSession& session, core::PageId page,
                                                   core::ObjectId id,
                                                   std::shared_ptr<const pdf::PdfImageData> image) const override {
        return editor::replaceImage(session, page, id, std::move(image));
    }
    core::Result<editor::ContentEdit> editTextBlock(editor::DocumentSession& session, core::PageId page,
                                                    core::ObjectId blockId,
                                                    editor::TextBlockPatch patch) const override {
        return editor::editTextBlock(session, page, blockId, std::move(patch));
    }
    core::Result<editor::ContentEdit> addTextBlock(editor::DocumentSession& session, core::PageId page,
                                                   editor::NewTextBlock block) const override {
        return editor::addTextBlock(session, page, std::move(block));
    }
    core::Result<editor::ContentEdit> bringToFront(editor::DocumentSession& session, core::PageId page,
                                                   core::ObjectId id) const override {
        return editor::bringToFront(session, page, id);
    }
    std::u32string uncoveredCodepoints(pdf::PdfBundledFont font, std::string_view utf8) const override {
        return editor::uncoveredCodepoints(font, utf8);
    }

    void observe(editor::DocumentSession& session, std::function<void(core::PageId)> onChanged) const override {
        session.setOnContentChanged(std::move(onChanged));
    }
};

} // namespace

std::unique_ptr<ContentBackend> createEditorContentBackend() { return std::make_unique<EditorContentBackend>(); }

} // namespace rivet::app
