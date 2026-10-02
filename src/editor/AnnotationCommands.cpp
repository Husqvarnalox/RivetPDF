// SPDX-License-Identifier: MPL-2.0
#include "editor/AnnotationCommands.hpp"

#include "editor/AnnotationGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <unordered_set>

namespace rivet::editor {
namespace {

core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "editor");
}

core::Error notFound(std::string message) {
    return core::makeError(core::ErrorCode::NotFound, std::move(message), "editor");
}

// A random version-4 style UUID for /NM of new annotations (main thread).
std::string makeName() {
    static std::mt19937_64 rng{std::random_device{}()};
    const std::uint64_t hi = rng();
    const std::uint64_t lo = rng();
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%08x-%04x-4%03x-%04x-%012llx", static_cast<unsigned>(hi >> 32),
                  static_cast<unsigned>((hi >> 16) & 0xFFFFU), static_cast<unsigned>(hi & 0xFFFU),
                  static_cast<unsigned>(0x8000U | ((lo >> 48) & 0x3FFFU)),
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return buffer;
}

pdf::PdfBox normalizedBox(pdf::PdfBox box) {
    if (box.left > box.right) std::swap(box.left, box.right);
    if (box.bottom > box.top) std::swap(box.bottom, box.top);
    return box;
}

pdf::PdfBox displayRectToBox(const pdf::PdfPageView& view, const core::Rect& rect) {
    return normalizedBox(geometry::rectToUserBox(view, rect));
}

bool finiteRect(const core::Rect& r) {
    return r.isFinite() && r.size.width > 0.0 && r.size.height > 0.0;
}

// The working copy of one page's state while a command is being built.
struct Working {
    PageAnnotationStatePtr beforeState;
    std::uint64_t beforeRaster = 0;
    pdf::PdfPageView view;
    std::vector<std::uint32_t> suppressed;
    std::vector<OverlayAnnotation> overlay;
    std::uint64_t raster = 0;
    bool rasterBumped = false;
};

class Builder {
public:
    explicit Builder(DocumentSession& session) : session_(session), snapshot_(session.pageSnapshot()) {}

    core::Result<Working*> page(core::PageId id) {
        if (const auto it = pages_.find(id); it != pages_.end()) return &it->second;
        const PageEntry* entry = snapshot_->find(id);
        if (entry == nullptr) return std::unexpected(notFound("page is not in the document"));
        Working working;
        working.beforeState = entry->annotations;
        working.beforeRaster = entry->rasterRevision;
        working.view = entry->view;
        working.raster = entry->rasterRevision;
        if (entry->annotations != nullptr) {
            working.suppressed = entry->annotations->suppressed;
            working.overlay = entry->annotations->overlay;
        }
        order_.push_back(id);
        return &pages_.emplace(id, std::move(working)).first->second;
    }

    void suppress(Working& w, std::uint32_t index) {
        const auto it = std::lower_bound(w.suppressed.begin(), w.suppressed.end(), index);
        if (it == w.suppressed.end() || *it != index) w.suppressed.insert(it, index);
    }

    // Hides an original (and its popup) from the raster: a fresh raster revision once per command.
    void hideOriginal(Working& w, const AnnotationService::Located& loc) {
        if (loc.index.has_value()) suppress(w, *loc.index);
        if (loc.popupIndex.has_value()) suppress(w, *loc.popupIndex);
        if (!w.rasterBumped) {
            w.raster = session_.pageModel().mintRasterRevision();
            w.rasterBumped = true;
        }
    }

    // Stores edited data for `loc` (an overlay item is replaced in place; an
    // original becomes an overlay item with the same id, on top).
    core::Status replaceData(const AnnotationService::Located& loc, pdf::PdfAnnotationData data) {
        pdf::normalizeAnnotation(data);
        if (!pdf::isWritableAnnotation(data)) return std::unexpected(invalid("the edited annotation is not valid"));
        auto working = page(loc.page);
        if (!working) return std::unexpected(working.error());
        Working& w = **working;
        const auto it = std::find_if(w.overlay.begin(), w.overlay.end(),
                                     [&](const OverlayAnnotation& item) { return item.id == loc.id; });
        if (it != w.overlay.end()) {
            it->data = std::move(data);
            return core::ok();
        }
        hideOriginal(w, loc);
        w.overlay.push_back(OverlayAnnotation{loc.id, std::move(data), std::nullopt});
        return core::ok();
    }

    core::Status removeAnnotation(const AnnotationService::Located& loc) {
        auto working = page(loc.page);
        if (!working) return std::unexpected(working.error());
        Working& w = **working;
        const auto it = std::find_if(w.overlay.begin(), w.overlay.end(),
                                     [&](const OverlayAnnotation& item) { return item.id == loc.id; });
        if (it != w.overlay.end()) {
            // An index the item holds in the file is already suppressed.
            w.overlay.erase(it);
            return core::ok();
        }
        hideOriginal(w, loc);
        return core::ok();
    }

    core::Status addOverlay(core::PageId pageId, OverlayAnnotation item) {
        auto working = page(pageId);
        if (!working) return std::unexpected(working.error());
        (*working)->overlay.push_back(std::move(item));
        return core::ok();
    }

    core::Result<std::vector<AnnotationStateCommand::Swap>> finish() {
        std::vector<AnnotationStateCommand::Swap> swaps;
        for (const core::PageId id : order_) {
            Working& w = pages_.at(id);
            if (w.overlay.size() > pdf::kMaxAnnotationsPerPage) {
                return std::unexpected(invalid("too many annotations on the page"));
            }
            PageAnnotationStatePtr after;
            if (!w.suppressed.empty() || !w.overlay.empty()) {
                auto state = std::make_shared<PageAnnotationState>();
                state->suppressed = std::move(w.suppressed);
                state->overlay = std::move(w.overlay);
                after = std::move(state);
            }
            swaps.push_back(AnnotationStateCommand::Swap{id, w.beforeState, w.beforeRaster, std::move(after),
                                                         w.raster});
        }
        return swaps;
    }

    DocumentSession& session() { return session_; }

private:
    DocumentSession& session_;
    PageSnapshotPtr snapshot_;
    std::map<core::PageId, Working> pages_;
    std::vector<core::PageId> order_;
};

core::Result<AnnotationEdit> build(Builder& builder, std::string name, std::vector<core::AnnotationId> ids) {
    auto swaps = builder.finish();
    if (!swaps) return std::unexpected(swaps.error());
    AnnotationEdit edit;
    edit.command = std::make_unique<AnnotationStateCommand>(builder.session().pageModel(), std::move(name),
                                                            std::move(*swaps));
    edit.ids = std::move(ids);
    return edit;
}

core::Result<AnnotationService::Located> locateChecked(DocumentSession& session, core::AnnotationId id,
                                                       unsigned cap, const char* what) {
    auto located = session.annotations().locate(id);
    if (!located.has_value()) return std::unexpected(notFound("annotation not found"));
    if ((annotationCaps(located->data.kind) & cap) == 0U) {
        return std::unexpected(invalid(std::string("annotation does not support ") + what));
    }
    return std::move(*located);
}

core::Status checkDistinct(const std::vector<core::AnnotationId>& ids) {
    if (ids.empty()) return std::unexpected(invalid("no annotations given"));
    std::unordered_set<core::AnnotationId> seen;
    for (const core::AnnotationId id : ids) {
        if (!seen.insert(id).second) return std::unexpected(invalid("annotation listed twice"));
    }
    return core::ok();
}

using Mutator = std::function<core::Result<std::optional<pdf::PdfAnnotationData>>(const AnnotationService::Located&)>;

// Edits each annotation of `ids` through `mutate` (nullopt = no change for
// that one); InvalidArgument when nothing changed.
core::Result<AnnotationEdit> editEach(DocumentSession& session, std::vector<core::AnnotationId> ids,
                                      unsigned cap, const char* what, std::string name, const Mutator& mutate) {
    if (const auto distinct = checkDistinct(ids); !distinct) return std::unexpected(distinct.error());
    Builder builder(session);
    std::vector<AnnotationService::Located> located;
    located.reserve(ids.size());
    for (const core::AnnotationId id : ids) {
        auto loc = locateChecked(session, id, cap, what);
        if (!loc) return std::unexpected(loc.error());
        located.push_back(std::move(*loc));
    }
    bool changed = false;
    for (const AnnotationService::Located& loc : located) {
        auto data = mutate(loc);
        if (!data) return std::unexpected(data.error());
        if (!data->has_value()) continue;
        if (const auto stored = builder.replaceData(loc, std::move(**data)); !stored) {
            return std::unexpected(stored.error());
        }
        changed = true;
    }
    if (!changed) return std::unexpected(invalid("the edit changes nothing"));
    return build(builder, std::move(name), std::move(ids));
}

void translate(pdf::PdfAnnotationData& data, double dx, double dy) {
    const auto shift = [&](pdf::PdfPoint& p) {
        p.x += dx;
        p.y += dy;
    };
    data.rect.left += dx;
    data.rect.right += dx;
    data.rect.bottom += dy;
    data.rect.top += dy;
    for (pdf::PdfQuad& q : data.quads) {
        shift(q.p1);
        shift(q.p2);
        shift(q.p3);
        shift(q.p4);
    }
    for (auto& stroke : data.inkStrokes) {
        for (pdf::PdfPoint& p : stroke) shift(p);
    }
    shift(data.lineStart);
    shift(data.lineEnd);
}

} // namespace

// --- Command ----------------------------------------------------------------

AnnotationStateCommand::AnnotationStateCommand(PageModel& model, std::string name, std::vector<Swap> swaps)
    : PageCommand(model), name_(std::move(name)), swaps_(std::move(swaps)) {}

bool AnnotationStateCommand::apply(bool forward) {
    std::vector<PageModel::AnnotationUpdate> updates;
    updates.reserve(swaps_.size());
    const PageSnapshotPtr snapshot = model_.snapshot();
    for (const Swap& swap : swaps_) {
        const PageEntry* entry = snapshot->find(swap.page);
        if (entry == nullptr) return fail(notFound("page is not in the document"));
        const PageAnnotationStatePtr& expectedState = forward ? swap.beforeState : swap.afterState;
        const std::uint64_t expectedRaster = forward ? swap.beforeRaster : swap.afterRaster;
        if (entry->annotations != expectedState || entry->rasterRevision != expectedRaster) {
            return fail(invalid("the page's annotations changed since the command was created"));
        }
        updates.push_back(PageModel::AnnotationUpdate{swap.page, forward ? swap.afterState : swap.beforeState,
                                                      forward ? swap.afterRaster : swap.beforeRaster});
    }
    return record(model_.updateAnnotations(updates));
}

bool AnnotationStateCommand::execute() { return apply(true); }
bool AnnotationStateCommand::undo() { return apply(false); }

// --- Factories -----------------------------------------------------------------

core::Result<AnnotationEdit> createAnnotations(DocumentSession& session, std::vector<AnnotationDraft> drafts) {
    if (drafts.empty()) return std::unexpected(invalid("no annotations given"));
    Builder builder(session);
    std::vector<core::AnnotationId> ids;
    ids.reserve(drafts.size());
    for (const AnnotationDraft& draft : drafts) {
        auto working = builder.page(draft.page);
        if (!working) return std::unexpected(working.error());
        const pdf::PdfPageView& view = (*working)->view;

        pdf::PdfAnnotationData data;
        data.kind = draft.kind;
        data.color = draft.style.color;
        data.interiorColor = draft.style.interiorColor;
        data.opacity = draft.style.opacity;
        data.borderWidth = draft.style.borderWidth;
        data.contents = draft.contents;
        data.author = draft.author;
        data.name = makeName();
        switch (draft.kind) {
        case pdf::PdfAnnotationKind::Highlight:
        case pdf::PdfAnnotationKind::Underline:
        case pdf::PdfAnnotationKind::StrikeOut:
            if (draft.quads.size() > pdf::kMaxQuadsPerAnnotation) return std::unexpected(invalid("too many quads"));
            for (const DisplayQuad& q : draft.quads) {
                data.quads.push_back(pdf::PdfQuad{geometry::toUser(view, q[0]), geometry::toUser(view, q[1]),
                                                  geometry::toUser(view, q[2]), geometry::toUser(view, q[3])});
            }
            break;
        case pdf::PdfAnnotationKind::Ink: {
            if (draft.strokes.size() > pdf::kMaxInkStrokes) return std::unexpected(invalid("too many strokes"));
            std::size_t total = 0;
            for (const auto& stroke : draft.strokes) {
                total += stroke.size();
                if (stroke.size() > pdf::kMaxInkPointsPerStroke || total > pdf::kMaxInkPointsTotal) {
                    return std::unexpected(invalid("too many ink points"));
                }
                std::vector<pdf::PdfPoint> mapped;
                mapped.reserve(stroke.size());
                for (const core::Point& p : stroke) mapped.push_back(geometry::toUser(view, p));
                data.inkStrokes.push_back(std::move(mapped));
            }
            break;
        }
        case pdf::PdfAnnotationKind::Line:
        case pdf::PdfAnnotationKind::Arrow:
            data.lineStart = geometry::toUser(view, draft.lineStart);
            data.lineEnd = geometry::toUser(view, draft.lineEnd);
            break;
        case pdf::PdfAnnotationKind::Note:
        case pdf::PdfAnnotationKind::Square:
        case pdf::PdfAnnotationKind::Circle:
        case pdf::PdfAnnotationKind::Stamp:
            if (!draft.rect.isFinite()) return std::unexpected(invalid("the annotation rect is not finite"));
            data.rect = displayRectToBox(view, draft.rect);
            if (draft.kind == pdf::PdfAnnotationKind::Stamp) {
                data.stampName = draft.stampName;
                data.rotation = (360 - core::rotationDegrees(view.rotation)) % 360;
            }
            break;
        case pdf::PdfAnnotationKind::Other:
            return std::unexpected(invalid("this annotation kind cannot be created"));
        }
        pdf::normalizeAnnotation(data);
        if (!pdf::isWritableAnnotation(data)) return std::unexpected(invalid("the annotation is not valid"));

        const core::AnnotationId id = session.pageModel().mintAnnotationId();
        ids.push_back(id);
        if (const auto added = builder.addOverlay(draft.page, OverlayAnnotation{id, std::move(data), std::nullopt});
            !added) {
            return std::unexpected(added.error());
        }
    }
    return build(builder, drafts.size() == 1 ? "Add Annotation" : "Add Annotations", std::move(ids));
}

core::Result<AnnotationEdit> deleteAnnotations(DocumentSession& session, std::vector<core::AnnotationId> ids) {
    if (const auto distinct = checkDistinct(ids); !distinct) return std::unexpected(distinct.error());
    Builder builder(session);
    std::vector<AnnotationService::Located> located;
    for (const core::AnnotationId id : ids) {
        auto loc = locateChecked(session, id, kCapDelete, "deleting");
        if (!loc) return std::unexpected(loc.error());
        located.push_back(std::move(*loc));
    }
    for (const AnnotationService::Located& loc : located) {
        if (const auto removed = builder.removeAnnotation(loc); !removed) return std::unexpected(removed.error());
    }
    return build(builder, ids.size() == 1 ? "Delete Annotation" : "Delete Annotations", std::move(ids));
}

core::Result<AnnotationEdit> moveAnnotation(DocumentSession& session, core::AnnotationId id, core::Point delta) {
    if (!delta.isFinite()) return std::unexpected(invalid("the move is not finite"));
    return editEach(session, {id}, kCapMove, "moving", "Move Annotation",
                    [&](const AnnotationService::Located& loc) -> core::Result<std::optional<pdf::PdfAnnotationData>> {
                        // The mapping is affine: the user-space delta is the same anywhere.
                        const core::Point origin = geometry::boxToDisplayRect(loc.view, loc.data.rect).center();
                        const pdf::PdfPoint a = geometry::toUser(loc.view, origin);
                        const pdf::PdfPoint b =
                            geometry::toUser(loc.view, core::Point{origin.x + delta.x, origin.y + delta.y});
                        pdf::PdfAnnotationData data = loc.data;
                        translate(data, b.x - a.x, b.y - a.y);
                        return std::optional<pdf::PdfAnnotationData>{std::move(data)};
                    });
}

core::Result<AnnotationEdit> resizeAnnotation(DocumentSession& session, core::AnnotationId id,
                                              core::Rect displayBounds) {
    if (!finiteRect(displayBounds)) return std::unexpected(invalid("the target bounds are not valid"));
    return editEach(
        session, {id}, kCapResize, "resizing", "Resize Annotation",
        [&](const AnnotationService::Located& loc) -> core::Result<std::optional<pdf::PdfAnnotationData>> {
            pdf::PdfAnnotationData data = loc.data;
            const pdf::PdfPageView& view = loc.view;
            switch (data.kind) {
            case pdf::PdfAnnotationKind::Square:
            case pdf::PdfAnnotationKind::Circle:
            case pdf::PdfAnnotationKind::Stamp:
                data.rect = displayRectToBox(view, displayBounds);
                break;
            case pdf::PdfAnnotationKind::Ink:
            case pdf::PdfAnnotationKind::Line:
            case pdf::PdfAnnotationKind::Arrow: {
                // Collect the points (display space).
                std::vector<core::Point> points;
                if (data.kind == pdf::PdfAnnotationKind::Ink) {
                    for (const auto& stroke : data.inkStrokes) {
                        for (const pdf::PdfPoint& p : stroke) points.push_back(geometry::toDisplay(view, p));
                    }
                } else {
                    points.push_back(geometry::toDisplay(view, data.lineStart));
                    points.push_back(geometry::toDisplay(view, data.lineEnd));
                }
                if (points.empty()) return std::unexpected(invalid("the annotation has no geometry"));
                double minX = points[0].x, maxX = points[0].x, minY = points[0].y, maxY = points[0].y;
                for (const core::Point& p : points) {
                    minX = std::min(minX, p.x);
                    maxX = std::max(maxX, p.x);
                    minY = std::min(minY, p.y);
                    maxY = std::max(maxY, p.y);
                }
                const core::Rect oldBounds = geometry::boxToDisplayRect(view, data.rect);
                // Margins between the point box and the drawn bounds stay constant.
                const double mLeft = std::max(0.0, minX - oldBounds.minX());
                const double mTop = std::max(0.0, minY - oldBounds.minY());
                const double mRight = std::max(0.0, oldBounds.maxX() - maxX);
                const double mBottom = std::max(0.0, oldBounds.maxY() - maxY);
                double newMinX = displayBounds.minX() + mLeft;
                double newMaxX = displayBounds.maxX() - mRight;
                double newMinY = displayBounds.minY() + mTop;
                double newMaxY = displayBounds.maxY() - mBottom;
                if (newMaxX < newMinX) newMinX = newMaxX = displayBounds.center().x;
                if (newMaxY < newMinY) newMinY = newMaxY = displayBounds.center().y;
                const auto map = [&](double v, double oldMin, double oldMax, double newMin, double newMax) {
                    if (oldMax - oldMin < 1e-9) return (newMin + newMax) / 2.0;
                    return newMin + (v - oldMin) / (oldMax - oldMin) * (newMax - newMin);
                };
                const auto remap = [&](const pdf::PdfPoint& p) {
                    const core::Point d = geometry::toDisplay(view, p);
                    return geometry::toUser(view, core::Point{map(d.x, minX, maxX, newMinX, newMaxX),
                                                              map(d.y, minY, maxY, newMinY, newMaxY)});
                };
                if (data.kind == pdf::PdfAnnotationKind::Ink) {
                    for (auto& stroke : data.inkStrokes) {
                        for (pdf::PdfPoint& p : stroke) p = remap(p);
                    }
                } else {
                    data.lineStart = remap(data.lineStart);
                    data.lineEnd = remap(data.lineEnd);
                }
                break;
            }
            default:
                return std::unexpected(invalid("annotation does not support resizing"));
            }
            return std::optional<pdf::PdfAnnotationData>{std::move(data)};
        });
}

core::Result<AnnotationEdit> setLineEndpoints(DocumentSession& session, core::AnnotationId id, core::Point start,
                                              core::Point end) {
    if (!start.isFinite() || !end.isFinite()) return std::unexpected(invalid("the endpoints are not finite"));
    return editEach(session, {id}, kCapResize, "resizing", "Edit Line",
                    [&](const AnnotationService::Located& loc) -> core::Result<std::optional<pdf::PdfAnnotationData>> {
                        if (loc.data.kind != pdf::PdfAnnotationKind::Line &&
                            loc.data.kind != pdf::PdfAnnotationKind::Arrow) {
                            return std::unexpected(invalid("annotation is not a line"));
                        }
                        pdf::PdfAnnotationData data = loc.data;
                        data.lineStart = geometry::toUser(loc.view, start);
                        data.lineEnd = geometry::toUser(loc.view, end);
                        return std::optional<pdf::PdfAnnotationData>{std::move(data)};
                    });
}

core::Result<AnnotationEdit> restyleAnnotations(DocumentSession& session, std::vector<core::AnnotationId> ids,
                                                StylePatch patch) {
    return editEach(
        session, std::move(ids), kCapRestyle, "restyling", "Change Style",
        [&](const AnnotationService::Located& loc) -> core::Result<std::optional<pdf::PdfAnnotationData>> {
            pdf::PdfAnnotationData data = loc.data;
            const pdf::PdfAnnotationKind kind = data.kind;
            const bool shape = kind == pdf::PdfAnnotationKind::Square || kind == pdf::PdfAnnotationKind::Circle;
            const bool stroked = shape || kind == pdf::PdfAnnotationKind::Ink ||
                                 kind == pdf::PdfAnnotationKind::Line || kind == pdf::PdfAnnotationKind::Arrow;
            if (patch.color.has_value()) data.color = *patch.color;
            if (patch.interiorColor.has_value() && shape) data.interiorColor = *patch.interiorColor;
            if (patch.opacity.has_value() && kind != pdf::PdfAnnotationKind::Note &&
                kind != pdf::PdfAnnotationKind::Stamp) {
                data.opacity = *patch.opacity;
            }
            if (patch.borderWidth.has_value() && stroked) data.borderWidth = *patch.borderWidth;
            pdf::normalizeAnnotation(data);
            pdf::PdfAnnotationData before = loc.data;
            pdf::normalizeAnnotation(before);
            if (data == before) return std::optional<pdf::PdfAnnotationData>{};
            return std::optional<pdf::PdfAnnotationData>{std::move(data)};
        });
}

core::Result<AnnotationEdit> editContents(DocumentSession& session, core::AnnotationId id, std::string utf8) {
    if (utf8.size() > pdf::kMaxContentsBytes) return std::unexpected(invalid("the text is too long"));
    return editEach(session, {id}, kCapEditContents, "editing text", "Edit Annotation Text",
                    [&](const AnnotationService::Located& loc) -> core::Result<std::optional<pdf::PdfAnnotationData>> {
                        if (loc.data.contents == utf8) return std::optional<pdf::PdfAnnotationData>{};
                        pdf::PdfAnnotationData data = loc.data;
                        data.contents = utf8;
                        return std::optional<pdf::PdfAnnotationData>{std::move(data)};
                    });
}

} // namespace rivet::editor
