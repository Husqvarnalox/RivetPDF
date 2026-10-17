// SPDX-License-Identifier: MPL-2.0
#pragma once

// AcroForm value commands (ADR-0024). FormValuesCommand swaps the page
// model's document-level form values pointer (undo is O(1) in the number of
// fields) together with the raster revision of every page entry that
// presents a widget of a changed field (those widgets switch between the
// raster and the overlay). Undo/redo restore the exact revisions, so tiles
// rendered before the edit are reused after an undo.

#include "editor/Command.hpp"
#include "editor/PageModel.hpp"

#include "core/Error.hpp"
#include "pdf/PdfForms.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::editor {

class FormValuesCommand final : public Command {
public:
    // `affectedSourcePages`: pages of the BASE document whose widgets change
    // presentation (their entries get fresh raster revisions).
    FormValuesCommand(PageModel& model, pdf::PdfFormValuesPtr before, pdf::PdfFormValuesPtr after,
                      std::vector<std::size_t> affectedSourcePages, std::string_view name);

    std::string_view name() const override { return name_; }
    bool execute() override;
    bool undo() override;
    bool redo() override;
    std::optional<core::Error> failure() const override { return failure_; }

private:
    struct PageRaster {
        core::PageId id;
        std::uint64_t before = 0;
        std::uint64_t after = 0;
    };
    bool apply(const pdf::PdfFormValuesPtr& values, bool forward);

    PageModel& model_;
    pdf::PdfFormValuesPtr before_;
    pdf::PdfFormValuesPtr after_;
    std::vector<std::size_t> affected_;
    std::string_view name_;
    std::vector<PageRaster> rasters_;
    bool executed_ = false;
    std::optional<core::Error> failure_;
};

// Sets field `fieldName` of `info` (the model base's form) to `value` (an
// edit equal to the stored value removes the field's entry, so the overlay
// disappears). nullptr = nothing changes. InvalidArgument for an unknown
// field, a non-fillable form/field or an invalid value (validateFormValue).
core::Result<std::unique_ptr<Command>> makeSetFormValue(PageModel& model, const pdf::PdfFormInfo& info,
                                                         const std::string& fieldName, pdf::PdfFormValue value);

// Resets every fillable field to its default (pdf::resetFormValues: /DV,
// else the type's empty value; fields without a writable default keep their
// stored value) as one command. nullptr when the form already shows exactly
// that. Also used to decide whether Reset Form is enabled.
core::Result<std::unique_ptr<Command>> makeResetForm(PageModel& model, const pdf::PdfFormInfo& info);

} // namespace rivet::editor
