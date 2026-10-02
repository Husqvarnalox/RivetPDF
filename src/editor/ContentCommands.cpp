// SPDX-License-Identifier: MPL-2.0
#include "editor/ContentCommands.hpp"

#include "editor/ContentGeometry.hpp"
#include "editor/TextBlocks.hpp"

#include "pdf/BundledFonts.hpp"
#include "pdf/PdfTextLayout.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace rivet::editor {
namespace {

core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "editor");
}

core::Error notFound(std::string message) {
    return core::makeError(core::ErrorCode::NotFound, std::move(message), "editor");
}

core::Error unsupported(std::string message) {
    return core::makeError(core::ErrorCode::Unsupported, std::move(message), "editor");
}

core::Error notAvailable(std::string message) {
    return core::makeError(core::ErrorCode::NotAvailable, std::move(message), "editor");
}

// Smallest extent (display points) a resized object may be given.
constexpr double kMinResizedExtent = 0.5;

bool finitePoint(const core::Point& p) { return std::isfinite(p.x) && std::isfinite(p.y); }

// Rotation + translation of an object's (similarity) text matrix: the block
// frame placement at its baseline origin.
core::Matrix rigidPartOf(const core::Matrix& m) {
    core::Matrix rigid = core::Matrix::rotation(std::atan2(m.b, m.a));
    rigid.tx = m.tx;
    rigid.ty = m.ty;
    return rigid;
}

// The page being edited: its snapshot entry, the resolved content and a
// working copy of the immutable edits. Every factory opens one, mutates the
// copy, validates and finishes it into a command.
class PageEditor {
public:
    // `requireFreshGeometry`: the factory reads object geometry from the
    // resolved view, so the view must be extracted for the CURRENT edits.
    // Other factories only need identities (registry) and the edits
    // themselves, so they accept the previous view while the backend
    // re-extracts after an edit (key-repeat nudges, back-to-back drags).
    static core::Result<PageEditor> open(DocumentSession& session, core::PageId page,
                                         bool requireFreshGeometry = false) {
        if (session.isEditingLocked()) {
            return std::unexpected(unsupported(session.editingLockReason().empty()
                                                   ? std::string("the document cannot be edited right now")
                                                   : session.editingLockReason()));
        }
        PageEditor editor(session, page);
        editor.snapshot_ = session.pageSnapshot();
        editor.entry_ = editor.snapshot_->find(page);
        if (editor.entry_ == nullptr) return std::unexpected(notFound("page is not in the document"));
        editor.content_ = session.contentService().content(page);
        if (editor.content_ == nullptr || (!editor.content_->loaded && requireFreshGeometry) ||
            (!editor.content_->loaded && editor.content_->objects.empty())) {
            return std::unexpected(notAvailable("the page content is still loading"));
        }
        if (editor.content_->truncated) {
            return std::unexpected(unsupported("the page has too many objects to edit"));
        }
        if (!editor.content_->regenerationSafe) {
            return std::unexpected(unsupported(editor.content_->regenerationIssue.empty()
                                                   ? std::string("the page content cannot be edited safely")
                                                   : editor.content_->regenerationIssue));
        }
        if (editor.entry_->contentEdits != nullptr) editor.work_ = *editor.entry_->contentEdits;
        editor.objectAt_.reserve(editor.content_->objects.size());
        for (std::size_t i = 0; i < editor.content_->objects.size(); ++i) {
            editor.objectAt_.emplace(editor.content_->objects[i].id, i);
        }
        return editor;
    }

    const pdf::PdfPageView& view() const { return entry_->view; }
    pdf::PdfPageContentEdits& work() { return work_; }
    const PageContentView& content() const { return *content_; }
    DocumentSession& session() { return session_; }

    const ContentObjectView* object(core::ObjectId id) const {
        const auto it = objectAt_.find(id);
        return it == objectAt_.end() ? nullptr : &content_->objects[it->second];
    }

    const TextBlockView* block(core::ObjectId id) const {
        for (const TextBlockView& candidate : content_->blocks) {
            if (candidate.id == id) return &candidate;
        }
        return nullptr;
    }

    const TextBlockView* blockOf(const ContentObjectView& object) const {
        if (object.block.value() == 0) return nullptr;
        return block(object.block);
    }

    // What a set of ids addresses, expanded: source objects moved or removed
    // individually and Rivet-edited blocks (identified by their edit tag).
    struct Targets {
        std::set<std::uint32_t> sourceIndices;
        std::set<std::uint64_t> blockTags;
    };

    core::Result<Targets> collect(const std::vector<core::ObjectId>& ids, ContentCapability need) {
        if (ids.empty()) return std::unexpected(invalid("nothing selected"));
        Targets targets;
        for (const core::ObjectId id : ids) {
            if (const TextBlockView* b = block(id)) {
                if (b->capability < need) return std::unexpected(unsupported(b->capabilityReason));
                if (const std::uint64_t tag = tagOf(*b); tag != 0) {
                    targets.blockTags.insert(tag);
                    continue;
                }
                for (const TextBlockLine& line : b->lines) {
                    for (const core::ObjectId member : line.members) {
                        const ContentObjectView* o = object(member);
                        if (o == nullptr || o->source.origin.kind != pdf::PdfContentOrigin::Kind::Source) {
                            return std::unexpected(invalid("the text block has no source objects"));
                        }
                        if (isRemoved(o->source.origin.sourceIndex)) {
                            return std::unexpected(notFound("the text block was deleted"));
                        }
                        targets.sourceIndices.insert(o->source.origin.sourceIndex);
                    }
                }
                continue;
            }
            const ContentObjectView* o = object(id);
            if (o == nullptr) return std::unexpected(notFound("object is not on the page"));
            if (const TextBlockView* owner = blockOf(*o); owner != nullptr) {
                if (const std::uint64_t tag = tagOf(*owner); tag != 0) {
                    if (owner->capability < need) return std::unexpected(unsupported(owner->capabilityReason));
                    targets.blockTags.insert(tag);
                    continue;
                }
            }
            if (o->capability < need) return std::unexpected(unsupported(o->capabilityReason));
            if (o->source.origin.kind != pdf::PdfContentOrigin::Kind::Source) {
                return std::unexpected(invalid("the object has no source object"));
            }
            if (isRemoved(o->source.origin.sourceIndex)) return std::unexpected(notFound("the object was deleted"));
            targets.sourceIndices.insert(o->source.origin.sourceIndex);
        }
        return targets;
    }

    // The (possibly new) object edit of source object `index`, keeping the
    // vector sorted.
    pdf::PdfObjectEdit& objectEdit(std::uint32_t index) {
        auto it = std::lower_bound(work_.objects.begin(), work_.objects.end(), index,
                                   [](const pdf::PdfObjectEdit& edit, std::uint32_t value) {
                                       return edit.sourceIndex < value;
                                   });
        if (it == work_.objects.end() || it->sourceIndex != index) {
            pdf::PdfObjectEdit fresh;
            fresh.sourceIndex = index;
            it = work_.objects.insert(it, std::move(fresh));
        }
        return *it;
    }

    void removeObject(std::uint32_t index) {
        pdf::PdfObjectEdit& edit = objectEdit(index);
        edit.remove = true;
        edit.transform.reset();
        edit.replaceImage.reset();
    }

    pdf::PdfTextBlockEdit* textBlock(std::uint64_t tag) {
        for (pdf::PdfTextBlockEdit& block : work_.textBlocks) {
            if (block.tag == tag) return &block;
        }
        return nullptr;
    }

    // The edit tag a block is addressed by. A view resolved before the
    // latest edit (stale, see open()) still shows a replaced source block
    // without its tag; the tag of a replaced block IS the block's id.
    std::uint64_t tagOf(const TextBlockView& block) {
        if (block.tag != 0) return block.tag;
        return textBlock(block.id.value()) != nullptr ? block.id.value() : 0;
    }

    // A source object already deleted by the current edits (a stale view
    // still shows it).
    bool isRemoved(std::uint32_t index) const {
        const auto it = std::find_if(work_.objects.begin(), work_.objects.end(),
                                     [index](const pdf::PdfObjectEdit& edit) { return edit.sourceIndex == index; });
        return it != work_.objects.end() && it->remove;
    }

    // Drops no-op object edits, validates against the source page and builds
    // the command (before = the entry's current edits, after = the copy).
    core::Result<ContentEdit> finish(std::string name, std::vector<core::ObjectId> ids) {
        std::erase_if(work_.objects, [](const pdf::PdfObjectEdit& edit) {
            return !edit.remove && !edit.transform.has_value() && edit.replaceImage == nullptr;
        });
        if (core::Status status = pdf::validate(work_, content_->sourceObjectCount); !status) {
            return std::unexpected(status.error());
        }
        pdf::PdfPageContentEditsPtr after;
        if (!work_.empty()) after = std::make_shared<const pdf::PdfPageContentEdits>(std::move(work_));
        std::vector<ContentEditsCommand::Swap> swaps;
        swaps.push_back(ContentEditsCommand::Swap{page_, entry_->contentEdits, std::move(after)});
        ContentEdit edit;
        edit.command = std::make_unique<ContentEditsCommand>(session_.pageModel(), std::move(name), std::move(swaps));
        edit.ids = std::move(ids);
        return edit;
    }

private:
    PageEditor(DocumentSession& session, core::PageId page) : session_(session), page_(page) {}

    DocumentSession& session_;
    core::PageId page_;
    PageSnapshotPtr snapshot_;
    const PageEntry* entry_ = nullptr;
    PageContentViewPtr content_;
    pdf::PdfPageContentEdits work_;
    std::unordered_map<core::ObjectId, std::size_t> objectAt_;
};

// The font size is changing: keep the line advance proportional.
void scaleLineAdvance(pdf::PdfTextBlockEdit& edit, double newSize) {
    if (edit.fontSize > 0.0) edit.lineAdvance *= newSize / edit.fontSize;
    edit.fontSize = newSize;
}

core::Status checkNumbers(const TextBlockPatch& patch) {
    if (patch.fontSize.has_value() && !(std::isfinite(*patch.fontSize) && *patch.fontSize >= pdf::kMinFontSize &&
                                         *patch.fontSize <= pdf::kMaxFontSize)) {
        return std::unexpected(invalid("the font size is out of range"));
    }
    if (patch.wrapWidth.has_value() && !(std::isfinite(*patch.wrapWidth) && *patch.wrapWidth >= 0.0)) {
        return std::unexpected(invalid("the wrap width is invalid"));
    }
    if (patch.text.has_value() && patch.text->size() > pdf::kMaxTextBlockBytes) {
        return std::unexpected(invalid("the text is too long"));
    }
    return core::ok();
}

} // namespace

// --- Command -----------------------------------------------------------------

ContentEditsCommand::ContentEditsCommand(PageModel& model, std::string name, std::vector<Swap> swaps)
    : PageCommand(model), name_(std::move(name)), swaps_(std::move(swaps)) {}

bool ContentEditsCommand::apply(bool forward) {
    const PageSnapshotPtr snapshot = model_.snapshot();
    for (const Swap& swap : swaps_) {
        const PageEntry* entry = snapshot->find(swap.page);
        if (entry == nullptr) return fail(notFound("page is not in the document"));
        const pdf::PdfPageContentEditsPtr& expected = forward ? swap.before : swap.after;
        if (entry->contentEdits != expected) {
            return fail(invalid("the page's content changed since the command was created"));
        }
    }
    std::vector<PageModel::ContentUpdate> updates;
    updates.reserve(swaps_.size());
    for (const Swap& swap : swaps_) {
        // Fresh revisions on every execute/undo/redo: text, search, links and
        // tiles of the page are invalidated, nothing is ever reused.
        updates.push_back(PageModel::ContentUpdate{swap.page, forward ? swap.after : swap.before,
                                                   model_.mintContentRevision(), model_.mintRasterRevision()});
    }
    return record(model_.updateContent(updates));
}

bool ContentEditsCommand::execute() { return apply(true); }
bool ContentEditsCommand::undo() { return apply(false); }

// --- Factories -----------------------------------------------------------------

core::Result<ContentEdit> moveContent(DocumentSession& session, core::PageId page, std::vector<core::ObjectId> ids,
                                      core::Point displayDelta) {
    if (!finitePoint(displayDelta)) return std::unexpected(invalid("the movement is not finite"));
    auto editor = PageEditor::open(session, page);
    if (!editor) return std::unexpected(editor.error());
    const auto translation = geometry::userTranslationForDisplayDelta(editor->view(), displayDelta);
    if (!translation.has_value()) return std::unexpected(invalid("the movement cannot be mapped to the page"));
    auto targets = editor->collect(ids, ContentCapability::MoveOnly);
    if (!targets) return std::unexpected(targets.error());

    for (const std::uint64_t tag : targets->blockTags) {
        pdf::PdfTextBlockEdit* block = editor->textBlock(tag);
        if (block == nullptr) return std::unexpected(invalid("the text block has no edit"));
        block->placement = *translation * block->placement;
    }
    for (const std::uint32_t index : targets->sourceIndices) {
        pdf::PdfObjectEdit& edit = editor->objectEdit(index);
        edit.transform = edit.transform.has_value() ? (*translation * *edit.transform) : *translation;
    }
    return editor->finish("Move", std::move(ids));
}

core::Result<ContentEdit> deleteContent(DocumentSession& session, core::PageId page, std::vector<core::ObjectId> ids) {
    auto editor = PageEditor::open(session, page);
    if (!editor) return std::unexpected(editor.error());
    auto targets = editor->collect(ids, ContentCapability::MoveOnly);
    if (!targets) return std::unexpected(targets.error());

    for (const std::uint64_t tag : targets->blockTags) {
        pdf::PdfPageContentEdits& work = editor->work();
        const auto it = std::find_if(work.textBlocks.begin(), work.textBlocks.end(),
                                     [tag](const pdf::PdfTextBlockEdit& block) { return block.tag == tag; });
        if (it == work.textBlocks.end()) return std::unexpected(invalid("the text block has no edit"));
        // The replaced originals must not come back.
        const std::vector<std::uint32_t> members = it->members;
        work.textBlocks.erase(it);
        for (const std::uint32_t member : members) editor->removeObject(member);
    }
    for (const std::uint32_t index : targets->sourceIndices) editor->removeObject(index);
    return editor->finish("Delete", std::move(ids));
}

core::Result<ContentEdit> resizeContent(DocumentSession& session, core::PageId page, core::ObjectId id,
                                        core::Rect displayBounds) {
    if (!displayBounds.isFinite() || displayBounds.size.width < kMinResizedExtent ||
        displayBounds.size.height < kMinResizedExtent) {
        return std::unexpected(invalid("the new size is out of range"));
    }
    auto editor = PageEditor::open(session, page, /*requireFreshGeometry=*/true);
    if (!editor) return std::unexpected(editor.error());
    const ContentObjectView* object = editor->object(id);
    if (object == nullptr) return std::unexpected(notFound("object is not on the page"));
    if (object->type == pdf::PdfContentObjectType::Text) {
        return std::unexpected(invalid("text blocks are resized through their wrap width"));
    }
    if (object->type != pdf::PdfContentObjectType::Image && object->type != pdf::PdfContentObjectType::Path) {
        return std::unexpected(invalid("only images and paths can be resized"));
    }
    if (object->capability < ContentCapability::MoveOnly) return std::unexpected(unsupported(object->capabilityReason));
    if (object->source.origin.kind != pdf::PdfContentOrigin::Kind::Source) {
        return std::unexpected(invalid("the object has no source object"));
    }
    if (editor->isRemoved(object->source.origin.sourceIndex)) return std::unexpected(notFound("the object was deleted"));
    const auto transform = geometry::userTransformForDisplayRects(editor->view(), object->bounds, displayBounds);
    if (!transform.has_value()) return std::unexpected(invalid("the object cannot be resized"));
    pdf::PdfObjectEdit& edit = editor->objectEdit(object->source.origin.sourceIndex);
    edit.transform = edit.transform.has_value() ? (*transform * *edit.transform) : *transform;
    return editor->finish("Resize", {id});
}

core::Result<ContentEdit> replaceImage(DocumentSession& session, core::PageId page, core::ObjectId id,
                                       std::shared_ptr<const pdf::PdfImageData> image) {
    if (image == nullptr) return std::unexpected(invalid("no image given"));
    auto editor = PageEditor::open(session, page);
    if (!editor) return std::unexpected(editor.error());
    const ContentObjectView* object = editor->object(id);
    if (object == nullptr) return std::unexpected(notFound("object is not on the page"));
    if (object->type != pdf::PdfContentObjectType::Image) return std::unexpected(invalid("the object is not an image"));
    if (object->capability < ContentCapability::FullyEditable) {
        return std::unexpected(unsupported(object->capabilityReason));
    }
    if (object->source.origin.kind != pdf::PdfContentOrigin::Kind::Source) {
        return std::unexpected(invalid("the object has no source object"));
    }
    if (editor->isRemoved(object->source.origin.sourceIndex)) return std::unexpected(notFound("the object was deleted"));
    editor->objectEdit(object->source.origin.sourceIndex).replaceImage = std::move(image);
    return editor->finish("Replace Image", {id});
}

core::Result<ContentEdit> editTextBlock(DocumentSession& session, core::PageId page, core::ObjectId blockId,
                                        TextBlockPatch patch) {
    if (!patch.text && !patch.fontSize && !patch.color && !patch.wrapWidth && !patch.font) {
        return std::unexpected(invalid("nothing to change"));
    }
    if (core::Status status = checkNumbers(patch); !status) return std::unexpected(status.error());
    // Emptying a block removes it (an empty text block has no lines).
    if (patch.text.has_value() && patch.text->empty()) return deleteContent(session, page, {blockId});

    auto editor = PageEditor::open(session, page);
    if (!editor) return std::unexpected(editor.error());
    const TextBlockView* block = editor->block(blockId);
    if (block == nullptr) return std::unexpected(notFound("text block is not on the page"));
    if (block->capability < ContentCapability::Replaceable) return std::unexpected(unsupported(block->capabilityReason));

    const std::uint64_t tag = editor->tagOf(*block);
    pdf::PdfTextBlockEdit* existing = tag != 0 ? editor->textBlock(tag) : nullptr;
    if (tag != 0 && existing == nullptr) return std::unexpected(invalid("the text block has no edit"));

    pdf::PdfTextBlockEdit edit;
    if (existing != nullptr) {
        edit = *existing;
    } else {
        // Replace in place (ADR-0015): the block's own objects become members.
        if (block->lines.empty() || block->lines.front().members.empty()) {
            return std::unexpected(invalid("the text block has no objects"));
        }
        std::set<std::uint32_t> members;
        for (const TextBlockLine& line : block->lines) {
            for (const core::ObjectId member : line.members) {
                const ContentObjectView* o = editor->object(member);
                if (o == nullptr || o->source.origin.kind != pdf::PdfContentOrigin::Kind::Source) {
                    return std::unexpected(invalid("the text block has no source objects"));
                }
                if (editor->isRemoved(o->source.origin.sourceIndex)) {
                    return std::unexpected(notFound("the text block was deleted"));
                }
                members.insert(o->source.origin.sourceIndex);
            }
        }
        const ContentObjectView* first = editor->object(block->lines.front().members.front());
        edit.tag = blockId.value();
        edit.members.assign(members.begin(), members.end());
        edit.text = block->text;
        edit.font.kind = pdf::PdfFontRef::Kind::FromObject;
        edit.font.sourceIndex = edit.members.front();
        edit.font.fallback = pdf::fallbackFor(block->font);
        edit.fontSize = block->fontSize;
        edit.color = block->color;
        edit.placement = rigidPartOf(first->source.matrix);
        edit.wrapWidth = block->lines.size() > 1 ? block->wrapWidth : 0.0;
        edit.lineAdvance = block->lineAdvance > 0.0 ? block->lineAdvance : kDefaultLineAdvanceFactor * block->fontSize;
        // The members' earlier moves are baked into the placement now.
        for (const std::uint32_t member : edit.members) {
            pdf::PdfObjectEdit& objectEdit = editor->objectEdit(member);
            objectEdit.transform.reset();
        }
    }

    if (patch.fontSize.has_value()) scaleLineAdvance(edit, *patch.fontSize);
    if (patch.text.has_value()) edit.text = std::move(*patch.text);
    if (patch.color.has_value()) edit.color = *patch.color;
    if (patch.wrapWidth.has_value()) edit.wrapWidth = *patch.wrapWidth;
    if (patch.font.has_value()) {
        if (edit.font.kind != pdf::PdfFontRef::Kind::Bundled) {
            return std::unexpected(unsupported("the font of existing text cannot be changed"));
        }
        edit.font.fallback = *patch.font;
    }

    // Characters the writing font cannot encode are refused up front. A
    // block in its own full embedded font is checked by the backend.
    const bool ownFontMayCover = edit.font.kind == pdf::PdfFontRef::Kind::FromObject && !block->font.subset &&
                                 !block->fontSubstituted && !block->font.standard14 && block->font.embedded;
    if (!ownFontMayCover && !uncoveredCodepoints(edit.font.fallback, edit.text).empty()) {
        return std::unexpected(invalid("the text contains characters that cannot be written"));
    }

    pdf::PdfPageContentEdits& work = editor->work();
    if (existing != nullptr) {
        *existing = std::move(edit);
    } else {
        work.textBlocks.push_back(std::move(edit));
    }
    return editor->finish("Edit Text", {blockId});
}

core::Result<ContentEdit> addTextBlock(DocumentSession& session, core::PageId page, NewTextBlock block) {
    if (block.text.empty()) return std::unexpected(invalid("the text is empty"));
    if (block.text.size() > pdf::kMaxTextBlockBytes) return std::unexpected(invalid("the text is too long"));
    if (!(std::isfinite(block.fontSize) && block.fontSize >= pdf::kMinFontSize && block.fontSize <= pdf::kMaxFontSize)) {
        return std::unexpected(invalid("the font size is out of range"));
    }
    if (!finitePoint(block.displayOrigin)) return std::unexpected(invalid("the position is not finite"));
    if (!(std::isfinite(block.displayWrapWidth) && block.displayWrapWidth >= 0.0)) {
        return std::unexpected(invalid("the wrap width is invalid"));
    }
    if (!uncoveredCodepoints(block.font, block.text).empty()) {
        return std::unexpected(invalid("the text contains characters that cannot be written"));
    }
    auto editor = PageEditor::open(session, page);
    if (!editor) return std::unexpected(editor.error());
    if (editor->work().textBlocks.size() >= pdf::kMaxTextBlocksPerPage) {
        return std::unexpected(invalid("too many text blocks on the page"));
    }

    pdf::PdfTextBlockEdit edit;
    edit.tag = session.pageModel().mintObjectId().value();
    edit.text = std::move(block.text);
    edit.font.kind = pdf::PdfFontRef::Kind::Bundled;
    edit.font.fallback = block.font;
    edit.fontSize = block.fontSize;
    edit.color = block.color;
    edit.placement = geometry::uprightPlacement(editor->view(), block.displayOrigin);
    edit.wrapWidth = block.displayWrapWidth;
    edit.lineAdvance = kDefaultLineAdvanceFactor * block.fontSize;
    const core::ObjectId id{edit.tag};
    editor->work().textBlocks.push_back(std::move(edit));
    return editor->finish("Add Text", {id});
}

core::Result<ContentEdit> bringToFront(DocumentSession& session, core::PageId page, core::ObjectId id) {
    auto editor = PageEditor::open(session, page);
    if (!editor) return std::unexpected(editor.error());
    const TextBlockView* block = editor->block(id);
    if (block == nullptr) {
        const ContentObjectView* object = editor->object(id);
        if (object == nullptr) return std::unexpected(notFound("object is not on the page"));
        block = editor->blockOf(*object);
    }
    const std::uint64_t tag = block == nullptr ? 0 : editor->tagOf(*block);
    if (tag == 0) {
        return std::unexpected(notAvailable("only text added with Rivet can be brought to the front"));
    }
    auto& blocks = editor->work().textBlocks;
    const auto it = std::find_if(blocks.begin(), blocks.end(),
                                 [&](const pdf::PdfTextBlockEdit& edit) { return edit.tag == tag; });
    if (it == blocks.end() || !it->members.empty()) {
        return std::unexpected(notAvailable("only text added with Rivet can be brought to the front"));
    }
    if (it + 1 == blocks.end()) return std::unexpected(notAvailable("the text is already in front"));
    std::rotate(it, it + 1, blocks.end());
    return editor->finish("Bring to Front", {block->id});
}

std::u32string uncoveredCodepoints(pdf::PdfBundledFont font, std::string_view utf8) {
    std::u32string missing;
    for (const char32_t codepoint : pdf::decodeUtf8(utf8)) {
        if (pdf::bundledFontCovers(font, codepoint)) continue;
        if (missing.find(codepoint) == std::u32string::npos) missing.push_back(codepoint);
    }
    return missing;
}

} // namespace rivet::editor
