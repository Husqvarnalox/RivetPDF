// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "editor/Command.hpp"
#include "editor/PageModel.hpp"

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rivet::editor {

// Page-structure commands over a PageModel (run through the session's
// CommandStack). Common contract:
//   - Transactional: execute/undo/redo validate everything first and then
//     apply all-or-nothing through ONE model mutation (one snapshot, one
//     change notification). A multi-page operation is ONE undo step.
//   - A failure leaves the model untouched; failure() reports why.
//   - Undo restores the exact previous ids, order and views (including each
//     page's contentRevision); redo re-applies the exact same result
//     (including the ids minted by the first execute - redo never mints).
//   - The model must outlive the command (the session declares the model
//     before its CommandStack).
class PageCommand : public Command {
public:
    std::optional<core::Error> failure() const override { return failure_; }

protected:
    explicit PageCommand(PageModel& model) : model_(model) {}

    // Records the outcome; returns whether it succeeded.
    bool record(const core::Status& status);
    template <typename T>
    bool record(const core::Result<T>& result) {
        if (result.has_value()) {
            failure_.reset();
            return true;
        }
        failure_ = result.error();
        return false;
    }
    bool fail(core::Error error);

    PageModel& model_;

private:
    std::optional<core::Error> failure_;
};

// Moves a set of pages as one block, preserving their relative (model)
// order, so that the first moved page lands at final index `destination`
// (0 <= destination <= pageCount - ids.size()).
class MovePagesCommand final : public PageCommand {
public:
    MovePagesCommand(PageModel& model, std::vector<core::PageId> ids, std::size_t destination);

    // Converts a drop GAP in the current order (0 = before the first page,
    // pageCount = after the last) into the `destination` this command takes.
    static std::size_t destinationForGap(const PageModelSnapshot& snapshot,
                                         std::span<const core::PageId> ids, std::size_t gap);

    std::string_view name() const override { return "Move Pages"; }
    bool execute() override;
    bool undo() override;

private:
    std::vector<core::PageId> ids_;
    std::size_t destination_;
    std::vector<std::pair<std::size_t, PageEntry>> previous_;
};

// Deletes a set of pages. Refuses (InvalidArgument) to delete every page.
class DeletePagesCommand final : public PageCommand {
public:
    DeletePagesCommand(PageModel& model, std::vector<core::PageId> ids);

    std::string_view name() const override { return "Delete Pages"; }
    bool execute() override;
    bool undo() override;

private:
    std::vector<core::PageId> ids_;
    std::vector<std::pair<std::size_t, PageEntry>> removed_;
};

// Rotates a set of pages by a multiple of 90 degrees (clockwise positive;
// +-90, +-180, +-270). Each page gets a fresh contentRevision.
class RotatePagesCommand final : public PageCommand {
public:
    RotatePagesCommand(PageModel& model, std::vector<core::PageId> ids, int degrees);

    std::string_view name() const override { return "Rotate Pages"; }
    bool execute() override;
    bool undo() override;
    bool redo() override;

private:
    std::vector<core::PageId> ids_;
    int degrees_;
    std::vector<PageModel::ViewUpdate> before_;
    std::vector<PageModel::ViewUpdate> after_;
};

// Duplicates a set of pages: each copy is inserted DIRECTLY AFTER its
// original (Preview-style), with a new id and the original's current view.
class DuplicatePagesCommand final : public PageCommand {
public:
    DuplicatePagesCommand(PageModel& model, std::vector<core::PageId> ids);

    std::string_view name() const override { return "Duplicate Pages"; }
    bool execute() override;
    bool undo() override;
    bool redo() override;

    // Ids minted by the first successful execute (parallel to the originals
    // in model order). Empty before.
    const std::vector<core::PageId>& createdIds() const { return createdIds_; }

private:
    std::vector<core::PageId> ids_;
    std::vector<std::pair<std::size_t, PageEntry>> placed_;
    std::vector<core::PageId> createdIds_;
};

// Inserts pages of another opened document (import/merge) as a block at
// `index` (0 <= index <= pageCount), each with a new id and its native view.
// The source documents are kept alive by the model entries (shared_ptr).
class InsertPagesCommand final : public PageCommand {
public:
    InsertPagesCommand(PageModel& model, std::vector<PageSource> pages, std::size_t index);

    std::string_view name() const override { return "Insert Pages"; }
    bool execute() override;
    bool undo() override;
    bool redo() override;

    const std::vector<core::PageId>& createdIds() const { return createdIds_; }

private:
    std::vector<PageSource> pages_;
    std::size_t index_;
    std::vector<std::pair<std::size_t, PageEntry>> placed_;
    std::vector<core::PageId> createdIds_;
};

// Sets the crop box (PDF user space of each page) of a set of pages, keeping
// their rotation. The box must be valid and lie within every page's media
// box. std::nullopt resets each page to its native crop box.
class CropPagesCommand final : public PageCommand {
public:
    CropPagesCommand(PageModel& model, std::vector<core::PageId> ids, std::optional<pdf::PdfBox> cropBox);

    std::string_view name() const override { return "Crop Pages"; }
    bool execute() override;
    bool undo() override;
    bool redo() override;

private:
    std::vector<core::PageId> ids_;
    std::optional<pdf::PdfBox> cropBox_;
    std::vector<PageModel::ViewUpdate> before_;
    std::vector<PageModel::ViewUpdate> after_;
};

} // namespace rivet::editor
