// SPDX-License-Identifier: MPL-2.0
#include "editor/FormService.hpp"

#include "editor/DocumentSession.hpp"

#include <algorithm>
#include <exception>

namespace rivet::editor {

FormService::FormService(DocumentSession& session)
    : session_(session), dispatcher_(session.mainDispatcher()), executor_(session.scheduler()) {}

FormService::~FormService() {
    alive_->store(false, std::memory_order_release);
    executor_.cancelPending();
    executor_.waitUntilIdle();
}

FormService::State FormService::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

pdf::PdfFormInfoPtr FormService::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

void FormService::ensureLoaded() {
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::NotLoaded) return;
        state_ = State::Loading;
        generation = generation_;
    }
    executor_.post([this, alive = alive_, document = session_.documentPtr(), generation]() mutable {
        pdf::PdfFormInfoPtr info;
        try {
            auto loaded = document->formInfo();
            if (loaded.has_value()) info = std::move(*loaded);
        } catch (const std::exception&) {
        } catch (...) {
        }
        // NotAvailable / load error: an empty, non-fillable form.
        if (info == nullptr) info = std::make_shared<const pdf::PdfFormInfo>();
        document.reset();
        if (dispatcher_ == nullptr) {
            complete(generation, std::move(info));
            return;
        }
        dispatcher_->post([this, alive, generation, info = std::move(info)]() mutable {
            if (!alive->load(std::memory_order_acquire)) return;
            complete(generation, std::move(info));
        });
    });
}

void FormService::complete(std::uint64_t generation, pdf::PdfFormInfoPtr info) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || state_ != State::Loading) return; // rebased meanwhile
        byPage_.clear();
        for (std::size_t f = 0; f < info->fields.size(); ++f) {
            const pdf::PdfFormField& field = info->fields[f];
            for (std::size_t w = 0; w < field.widgets.size(); ++w) {
                byPage_[field.widgets[w].pageIndex].push_back(PageWidget{field.widgets[w].annotIndex, f, w});
            }
        }
        for (auto& [page, widgets] : byPage_) {
            std::sort(widgets.begin(), widgets.end(),
                      [](const PageWidget& a, const PageWidget& b) { return a.annotIndex < b.annotIndex; });
        }
        info_ = std::move(info);
        state_ = State::Ready;
    }
    if (onChanged_) {
        const auto callback = onChanged_; // may replace itself
        callback();
    }
}

std::vector<std::uint32_t> FormService::hiddenWidgets(const PageModelSnapshot& snapshot,
                                                      const PageEntry& entry) const {
    const pdf::PdfFormValuesPtr& values = snapshot.formValues();
    if (values == nullptr || values->empty() || entry.source == nullptr || entry.source != snapshot.base()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (info_ == nullptr) return {};
    const auto page = byPage_.find(entry.sourcePageIndex);
    if (page == byPage_.end()) return {};
    std::vector<std::uint32_t> hidden;
    for (const PageWidget& widget : page->second) {
        if (values->contains(info_->fields[widget.field].name)) hidden.push_back(widget.annotIndex);
    }
    return hidden;
}

std::vector<FormWidgetView> FormService::widgets(core::PageId pageId) const {
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    const PageEntry* entry = snapshot->find(pageId);
    if (entry == nullptr || entry->source == nullptr || entry->source != snapshot->base()) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (info_ == nullptr) return {};
    const auto page = byPage_.find(entry->sourcePageIndex);
    if (page == byPage_.end()) return {};
    const pdf::PdfFormValuesPtr& values = snapshot->formValues();
    std::vector<FormWidgetView> views;
    views.reserve(page->second.size());
    for (const PageWidget& widget : page->second) {
        const pdf::PdfFormField& field = info_->fields[widget.field];
        const pdf::PdfBox& rect = field.widgets[widget.widget].rect;
        if (!rect.isValid()) continue;
        FormWidgetView view;
        view.field = widget.field;
        view.widget = widget.widget;
        view.displayRect = pdf::userBoxToDisplayRect(entry->view, rect);
        view.edited = values != nullptr && values->contains(field.name);
        views.push_back(view);
    }
    return views;
}

std::optional<pdf::PdfFormValue> FormService::currentValue(std::size_t field) const {
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    std::lock_guard<std::mutex> lock(mutex_);
    if (info_ == nullptr || field >= info_->fields.size()) return std::nullopt;
    const pdf::PdfFormField& described = info_->fields[field];
    if (const pdf::PdfFormValuesPtr& values = snapshot->formValues(); values != nullptr) {
        if (const auto it = values->find(described.name); it != values->end()) return it->second;
    }
    return pdf::originalValue(described);
}

void FormService::rebased() {
    bool reload = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        reload = state_ != State::NotLoaded; // someone uses the form: keep it current
        state_ = State::NotLoaded;
        info_ = nullptr;
        byPage_.clear();
    }
    if (reload) ensureLoaded();
}

void FormService::waitForLoads() {
    executor_.waitUntilIdle();
}

} // namespace rivet::editor
