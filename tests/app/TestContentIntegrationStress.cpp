// SPDX-License-Identifier: MPL-2.0
// Phase 5 end to end (real engine): a bounded, seeded random sequence of
// content commands with undo/redo against a model of what must exist, then a
// save and a reopen. Deterministic (fixed seed, own generator). Bodies return
// early without a backend.
#include "ContentIntegrationKit.hpp"

#include <random>
#include <set>

using namespace rivet;
using namespace rivet::test::integ;
using core::ObjectId;
using core::Point;
using core::Rect;
using pdf::PdfContentObjectType;

namespace {

constexpr int kOperations = 200;
constexpr std::uint64_t kSeed = 0x5EED2025u;

// What must be on the page after a command: the ids of the non-text objects
// (images, paths) and of the text blocks.
struct Model {
    std::set<ObjectId> shapes;
    std::set<ObjectId> blocks;
    bool operator==(const Model&) const = default;
};

bool isShape(PdfContentObjectType type) {
    return type == PdfContentObjectType::Image || type == PdfContentObjectType::Path;
}

Model modelOf(const editor::PageContentView& view) {
    Model model;
    for (const auto& object : view.objects) {
        if (isShape(object.type)) model.shapes.insert(object.id);
    }
    for (const auto& block : view.blocks) model.blocks.insert(block.id);
    return model;
}

bool uniqueIds(const editor::PageContentView& view) {
    std::set<ObjectId> seen;
    for (const auto& object : view.objects) {
        if (!seen.insert(object.id).second) return false;
    }
    return true;
}

} // namespace

RIVET_TEST(integRandomContentOperationsKeepIdentityRevisionsAndValidity) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich("stress.pdf");
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    const auto first = rig.loaded(0);
    const auto first2 = rig.loaded(1);
    CHECK(first != nullptr && first2 != nullptr);
    if (first == nullptr || first2 == nullptr) return;
    auto& session = rig.session();
    const core::PageId page = rig.pageId(0);
    const Model initial = modelOf(*first);
    CHECK(!initial.shapes.empty() && !initial.blocks.empty());
    const std::size_t page2Objects = first2->objects.size();
    const PageStamp page1Stamp = rig.stamp(1);
    const PageStamp initialStamp = rig.stamp(0);

    // The model per undo-stack position: states[cursor] is the current one.
    std::vector<Model> states{initial};
    std::size_t cursor = 0;

    std::mt19937_64 rng(kSeed);
    const auto pick = [&](std::size_t bound) { return bound == 0 ? std::size_t{0} : static_cast<std::size_t>(rng() % bound); };
    const auto offset = [&] { return static_cast<double>(static_cast<int>(pick(61)) - 30); };
    static const char* const kWords[] = {"alpha", "Beta", "gamma delta", "Привет", "x", "Zürich 42", "tab\tstop", "Line\nTwo"};

    std::uint64_t lastContentRevision = initialStamp.content;
    std::size_t refusals = 0;
    std::size_t executed = 0;
    std::size_t undone = 0;
    std::size_t redone = 0;
    std::size_t evictions = 0;
    int addCounter = 0;

    try {
        for (int op = 0; op < kOperations; ++op) {
            const auto view = rig.loaded(0);
            CHECK(view != nullptr);
            if (view == nullptr) return;
            const Model current = modelOf(*view);
            if (!(current == states[cursor])) {
                std::fprintf(stderr, "model mismatch before op %d\n", op);
            }
            CHECK(current == states[cursor]);
            CHECK(uniqueIds(*view));
            CHECK_EQ(rig.depth(), cursor);

            const std::size_t roll = pick(100);
            core::Result<editor::ContentEdit> edit = std::unexpected(core::makeError(core::ErrorCode::Unsupported, "none"));
            Model next = current;
            bool isCommand = false;

            if (roll < 15 && cursor > 0) { // undo
                const PageStamp before = rig.stamp(0);
                CHECK(session.undo());
                --cursor;
                ++undone;
                const PageStamp after = rig.stamp(0);
                CHECK(after.content > before.content);
                CHECK(after.raster > before.raster);
                lastContentRevision = after.content;
            } else if (roll < 25 && cursor + 1 < states.size()) { // redo
                const PageStamp before = rig.stamp(0);
                CHECK(session.redo());
                ++cursor;
                ++redone;
                const PageStamp after = rig.stamp(0);
                CHECK(after.content > before.content);
                CHECK(after.raster > before.raster);
                lastContentRevision = after.content;
            } else {
                isCommand = true;
                std::vector<ObjectId> targets; // movable things: blocks and shapes
                for (const auto& block : view->blocks) targets.push_back(block.id);
                for (const auto& object : view->objects) {
                    if (isShape(object.type)) targets.push_back(object.id);
                }
                const std::size_t kind = roll < 55 ? 0 : roll < 70 ? 1 : roll < 80 ? 2 : roll < 92 ? 3 : 4;
                if (targets.empty() && kind != 3) {
                    // Nothing left to touch: add instead.
                }
                if (kind == 3 || targets.empty()) { // add text
                    editor::NewTextBlock block;
                    block.text = "Added " + std::to_string(addCounter++);
                    block.font = pick(2) == 0 ? pdf::PdfBundledFont::SansRegular : pdf::PdfBundledFont::SansBold;
                    block.fontSize = 8.0 + static_cast<double>(pick(20));
                    block.color = pdf::PdfColor{0.1F, 0.1F, 0.8F};
                    block.displayOrigin = Point{20.0 + static_cast<double>(pick(200)), 30.0 + static_cast<double>(pick(340))};
                    edit = editor::addTextBlock(session, page, std::move(block));
                    if (edit.has_value() && !edit->ids.empty()) next.blocks.insert(edit->ids.front());
                } else if (kind == 0) { // move
                    const ObjectId id = targets[pick(targets.size())];
                    edit = editor::moveContent(session, page, {id}, Point{offset(), offset()});
                } else if (kind == 1) { // edit text
                    if (!view->blocks.empty()) {
                        const auto& block = view->blocks[pick(view->blocks.size())];
                        editor::TextBlockPatch patch;
                        patch.text = std::string(kWords[pick(std::size(kWords))]);
                        if (pick(3) == 0) patch.fontSize = 6.0 + static_cast<double>(pick(30));
                        edit = editor::editTextBlock(session, page, block.id, std::move(patch));
                    }
                } else if (kind == 2) { // delete
                    const ObjectId id = targets[pick(targets.size())];
                    edit = editor::deleteContent(session, page, {id});
                    if (edit.has_value()) {
                        next.shapes.erase(id);
                        next.blocks.erase(id);
                    }
                } else { // resize an image
                    for (const auto& object : view->objects) {
                        if (object.type != PdfContentObjectType::Image) continue;
                        const double scale = 0.5 + static_cast<double>(pick(15)) / 10.0;
                        edit = editor::resizeContent(
                            session, page, object.id,
                            Rect{object.bounds.minX(), object.bounds.minY(), object.bounds.size.width * scale,
                                 object.bounds.size.height * scale});
                        break;
                    }
                }
            }

            if (isCommand) {
                if (!edit.has_value()) {
                    // Refused, or no suitable target (no image left to resize, ...): nothing changes.
                    ++refusals;
                    CHECK_EQ(rig.depth(), cursor);
                    CHECK(rig.stamp(0).content == lastContentRevision);
                } else {
                    const PageStamp before = rig.stamp(0);
                    const core::Status status = session.execute(std::move(edit->command));
                    CHECK(static_cast<bool>(status));
                    if (!status) return;
                    ++executed;
                    states.resize(cursor + 1); // a new command drops the redo tail
                    states.push_back(next);
                    ++cursor;
                    // The undo stack is bounded: the oldest entry is evicted.
                    if (cursor > session.commands().maxDepth()) {
                        states.erase(states.begin());
                        --cursor;
                        ++evictions;
                    }
                    const PageStamp after = rig.stamp(0);
                    CHECK(after.content > before.content);
                    CHECK(after.raster > before.raster);
                    lastContentRevision = after.content;
                }
            }

            // Invariants after every operation.
            CHECK(rig.stamp(1) == page1Stamp);
            CHECK(rig.stamp(0).annotations == initialStamp.annotations);
            const editor::PageEntry* entry = session.pageSnapshot()->find(page);
            CHECK(entry != nullptr);
            if (entry != nullptr && entry->contentEdits != nullptr) {
                const auto now = session.contentService().content(page);
                if (now != nullptr && now->loaded) {
                    CHECK(static_cast<bool>(pdf::validate(*entry->contentEdits, now->sourceObjectCount)));
                }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "exception: %s\n", e.what());
        CHECK(false);
        return;
    }
    std::fprintf(stderr, "[stress] executed=%zu undone=%zu redone=%zu skipped=%zu evicted=%zu depth=%zu\n", executed, undone,
                 redone, refusals, evictions, cursor);
    CHECK(executed > 40);
    CHECK(undone > 5);

    // Final state, then save and reopen.
    const auto last = rig.loaded(0);
    CHECK(last != nullptr);
    if (last == nullptr) return;
    CHECK(modelOf(*last) == states[cursor]);
    CHECK(session.pageSnapshot()->find(page) != nullptr);
    CHECK(rig.saveAndWait());
    {
        auto reopened = rig.reopen(session.path());
        CHECK(reopened != nullptr);
        if (reopened == nullptr) return;
        std::size_t images = 0;
        std::size_t paths = 0;
        const auto content = contentOf(*reopened, 0);
        CHECK(content != nullptr);
        if (content == nullptr) return;
        for (const auto& object : content->objects) {
            if (object.type == PdfContentObjectType::Image) ++images;
            if (object.type == PdfContentObjectType::Path) ++paths;
        }
        std::size_t expectedImages = 0;
        std::size_t expectedPaths = 0;
        for (const auto& object : last->objects) {
            if (object.type == PdfContentObjectType::Image) ++expectedImages;
            if (object.type == PdfContentObjectType::Path) ++expectedPaths;
        }
        CHECK_EQ(images, expectedImages);
        CHECK_EQ(paths, expectedPaths);
        CHECK(contains(textOf(*reopened, 1), "Page Two"));
        CHECK_EQ(annotationCount(*reopened, 0), std::size_t{2});
        CHECK(renderOf(*reopened, 0).isValid());
    }
    // The rebase kept every surviving identity, and page 2 is as it was.
    const auto rebased = rig.loaded(0);
    CHECK(rebased != nullptr);
    if (rebased != nullptr) {
        CHECK(modelOf(*rebased) == states[cursor]);
        CHECK(uniqueIds(*rebased));
    }
    const auto rebased2 = rig.loaded(1);
    CHECK(rebased2 != nullptr);
    if (rebased2 != nullptr) CHECK_EQ(rebased2->objects.size(), page2Objects);
    CHECK(!session.isDirty());
}
