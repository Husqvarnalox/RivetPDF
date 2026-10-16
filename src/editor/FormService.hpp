// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "editor/PageModel.hpp"

#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfForms.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rivet::editor {

class DocumentSession;

// One widget of a fillable form as the editor presents it: display space of
// the entry's CURRENT view.
struct FormWidgetView {
    std::size_t field = 0;  // index into PdfFormInfo::fields
    std::size_t widget = 0; // index into that field's widgets
    core::Rect displayRect; // axis-aligned display bounds of the widget rect
    bool edited = false;    // the field has an edited value (overlay-drawn)
};

// The AcroForm of a session's BASE document (ADR-0024):
//
//   - the field description (pdf::PdfFormInfo) is loaded lazily, once per
//     base, on a dedicated SerialExecutor stream (PDFium calls are gated in
//     the backend) and delivered on the main thread; a backend without form
//     support (or a load error) yields an empty, non-fillable form;
//   - the edited VALUES are not stored here: they live on the page model
//     (PageModelSnapshot::formValues, swapped by FormValuesCommand), so undo,
//     save and rebase follow the one command stack;
//   - widgets are listed only for entries presenting a page of the base
//     document: fields of pages imported from another document keep their
//     original appearance and are view-only.
//
// Threading: main thread only, except the load job (which only reads the
// captured document). Completions are dropped when the service died
// (liveness flag) or the base changed meanwhile (generation token).
class FormService {
public:
    enum class State : std::uint8_t { NotLoaded, Loading, Ready };

    explicit FormService(DocumentSession& session);
    ~FormService();

    FormService(const FormService&) = delete;
    FormService& operator=(const FormService&) = delete;

    // Fired (main thread) when the form description finished loading.
    void setOnChanged(std::function<void()> onChanged) { onChanged_ = std::move(onChanged); }

    // Starts the load when it has not started. Main thread.
    void ensureLoaded();
    State state() const;
    // The loaded description (null until Ready; an empty info when the
    // backend has no form support).
    pdf::PdfFormInfoPtr info() const;

    // The /Annots indices of `entry`'s widgets whose field has an edited
    // value in `snapshot` (the raster must not draw them; Rivet draws the
    // overlay). Empty for entries that do not present a base page.
    std::vector<std::uint32_t> hiddenWidgets(const PageModelSnapshot& snapshot, const PageEntry& entry) const;

    // Widgets of the page `pageId` of the current model, in /Annots order.
    std::vector<FormWidgetView> widgets(core::PageId pageId) const;

    // The field's current value: the edited one, else the stored one.
    std::optional<pdf::PdfFormValue> currentValue(std::size_t field) const;

    // The session switched its base document (save + rebase): drops the
    // description and loads the new base's lazily.
    void rebased();

    // Tests: blocks until the load stream is idle (the completion may still
    // be queued on the main dispatcher).
    void waitForLoads();

private:
    void complete(std::uint64_t generation, pdf::PdfFormInfoPtr info);

    struct PageWidget {
        std::uint32_t annotIndex = 0;
        std::size_t field = 0;
        std::size_t widget = 0;
    };

    DocumentSession& session_;
    core::IMainThreadDispatcher* dispatcher_ = nullptr;
    mutable std::mutex mutex_; // guards state_/info_/byPage_ (inline completions in tests)
    State state_ = State::NotLoaded;
    pdf::PdfFormInfoPtr info_;
    std::map<std::size_t, std::vector<PageWidget>> byPage_; // source page -> widgets
    std::uint64_t generation_ = 1;
    std::function<void()> onChanged_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    core::SerialExecutor executor_; // declared last: destroyed (drained) first
};

} // namespace rivet::editor
