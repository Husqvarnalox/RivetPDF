// SPDX-License-Identifier: MPL-2.0
#include "editor/FormCommands.hpp"

#include <algorithm>
#include <utility>

namespace rivet::editor {
namespace {

core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "editor");
}

void addPagesOf(const pdf::PdfFormField& field, std::vector<std::size_t>& pages) {
    for (const pdf::PdfFormWidget& widget : field.widgets) pages.push_back(widget.pageIndex);
}

void normalizePages(std::vector<std::size_t>& pages) {
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
}

} // namespace

FormValuesCommand::FormValuesCommand(PageModel& model, pdf::PdfFormValuesPtr before, pdf::PdfFormValuesPtr after,
                                     std::vector<std::size_t> affectedSourcePages, std::string_view name)
    : model_(model),
      before_(std::move(before)),
      after_(std::move(after)),
      affected_(std::move(affectedSourcePages)),
      name_(name) {
    normalizePages(affected_);
}

bool FormValuesCommand::apply(const pdf::PdfFormValuesPtr& values, bool forward) {
    std::vector<PageModel::RasterUpdate> updates;
    updates.reserve(rasters_.size());
    const PageModelSnapshot& snapshot = *model_.snapshot();
    for (const PageRaster& page : rasters_) {
        if (!snapshot.contains(page.id)) continue; // gone since (cannot happen on a linear stack)
        updates.push_back(PageModel::RasterUpdate{page.id, forward ? page.after : page.before});
    }
    const core::Status status = model_.updateForms(values, updates);
    if (!status.has_value()) {
        failure_ = status.error();
        return false;
    }
    failure_.reset();
    return true;
}

bool FormValuesCommand::execute() {
    if (executed_) return redo();
    if (model_.formValues() != before_) {
        failure_ = invalid("the form changed since the edit was prepared");
        return false;
    }
    rasters_.clear();
    const PageModelSnapshot& snapshot = *model_.snapshot();
    for (const PageEntry& entry : snapshot.entries()) {
        if (entry.source == nullptr || entry.source != snapshot.base()) continue;
        if (!std::binary_search(affected_.begin(), affected_.end(), entry.sourcePageIndex)) continue;
        rasters_.push_back(PageRaster{entry.id, entry.rasterRevision, 0});
    }
    for (PageRaster& page : rasters_) page.after = model_.mintRasterRevision();
    if (!apply(after_, true)) return false;
    executed_ = true;
    return true;
}

bool FormValuesCommand::undo() {
    return apply(before_, false);
}

bool FormValuesCommand::redo() {
    return apply(after_, true);
}

core::Result<std::unique_ptr<Command>> makeSetFormValue(PageModel& model, const pdf::PdfFormInfo& info,
                                                         const std::string& fieldName, pdf::PdfFormValue value) {
    if (!info.fillable()) return std::unexpected(invalid("the document's form cannot be filled"));
    const std::size_t index = info.find(fieldName);
    if (index == pdf::PdfFormInfo::npos) return std::unexpected(invalid("the form field does not exist"));
    const pdf::PdfFormField& field = info.fields[index];
    if (auto valid = pdf::validateFormValue(field, value); !valid.has_value()) {
        return std::unexpected(valid.error());
    }
    const pdf::PdfFormValuesPtr before = model.formValues();
    auto next = before != nullptr ? std::make_shared<pdf::PdfFormValues>(*before)
                                  : std::make_shared<pdf::PdfFormValues>();
    if (value == pdf::originalValue(field)) {
        next->erase(fieldName);
    } else {
        (*next)[fieldName] = std::move(value);
    }
    const bool unchanged = before != nullptr ? *next == *before : next->empty();
    if (unchanged) return std::unique_ptr<Command>{};
    std::vector<std::size_t> pages;
    addPagesOf(field, pages);
    pdf::PdfFormValuesPtr after = next->empty() ? nullptr : pdf::PdfFormValuesPtr(std::move(next));
    return std::unique_ptr<Command>(
        std::make_unique<FormValuesCommand>(model, before, std::move(after), std::move(pages), "Fill Form Field"));
}

core::Result<std::unique_ptr<Command>> makeResetForm(PageModel& model, const pdf::PdfFormInfo& info) {
    const pdf::PdfFormValuesPtr before = model.formValues();
    if (before == nullptr || before->empty()) return std::unique_ptr<Command>{};
    std::vector<std::size_t> pages;
    for (const auto& [name, value] : *before) {
        (void)value;
        const std::size_t index = info.find(name);
        if (index != pdf::PdfFormInfo::npos) addPagesOf(info.fields[index], pages);
    }
    return std::unique_ptr<Command>(
        std::make_unique<FormValuesCommand>(model, before, nullptr, std::move(pages), "Reset Form"));
}

} // namespace rivet::editor
