// SPDX-License-Identifier: MPL-2.0
// AcroForm round trip against the real backend (ADR-0024): fill fields
// through the command pipeline, save with the real saver, rebase (values
// cleared, rasters of edited pages refreshed, description reloaded), then
// reopen the file in a NEW session and compare. Returns early only when no
// PDF backend is built in.
#include "RivetTest.h"

#include "PdfFixtures.hpp"
#include "fakes/AnnotationTestSupport.hpp"

#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/FormCommands.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfForms.hpp"
#include "pdf/PdfSystem.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("rivet-form-rt-" + std::to_string(::getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    fs::path operator/(const char* name) const { return path_ / name; }

private:
    fs::path path_;
};

struct Opened {
    Opened(pdf::PdfEngine& engine, const fs::path& path) {
        auto created = DocumentSession::create(engine, scheduler, &dispatcher, path);
        CHECK(created.has_value());
        if (created.has_value()) session = std::move(*created);
    }

    pdf::PdfFormInfoPtr loadForm() {
        session->forms().ensureLoaded();
        CHECK(settle(dispatcher, [&] { return session->forms().state() == FormService::State::Ready; }));
        return session->forms().info();
    }

    bool set(const std::string& name, pdf::PdfFormValue value) {
        auto command = makeSetFormValue(session->pageModel(), *session->forms().info(), name, std::move(value));
        CHECK(command.has_value() && *command != nullptr);
        if (!command.has_value() || *command == nullptr) return false;
        return session->execute(std::move(*command)).has_value();
    }

    core::TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    std::unique_ptr<DocumentSession> session;
};

const pdf::PdfFormField* field(const pdf::PdfFormInfo& info, const std::string& name) {
    const std::size_t index = info.find(name);
    return index == pdf::PdfFormInfo::npos ? nullptr : &info.fields[index];
}

} // namespace

RIVET_TEST(formRoundTripFillSaveRebaseReopen) {
    auto engine = pdf::createEngine();
    if (engine == nullptr || !engine->isAvailable()) return;
    TempDir dir;
    const fs::path path = dir / "form.pdf";
    {
        const std::string bytes = pdffix::formPdf();
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    Opened opened(*engine, path);
    if (opened.session == nullptr) return;
    DocumentSession& session = *opened.session;
    const pdf::PdfFormInfoPtr info = opened.loadForm();
    CHECK(info != nullptr && info->fields.size() == 8 && info->fillable());

    CHECK(opened.set("name", pdf::PdfFormValue::ofText("Zoe")));
    CHECK(opened.set("agree", pdf::PdfFormValue::ofToggle("Yes")));
    CHECK(opened.set("color", pdf::PdfFormValue::ofToggle("Red")));
    CHECK(opened.set("fruits", pdf::PdfFormValue::ofChoice({2})));
    CHECK(opened.set("city", pdf::PdfFormValue::ofText("Oslo")));
    CHECK(session.isDirty());
    const std::uint64_t page0Raster = session.pageSnapshot()->at(0).rasterRevision;
    const std::uint64_t page1Raster = session.pageSnapshot()->at(1).rasterRevision;
    // Edits never mutate the live document: its description is unchanged.
    const auto live = session.document().formInfo();
    const auto* liveName = live.has_value() ? field(**live, "name") : nullptr;
    CHECK(liveName != nullptr && liveName->value == "Alice");

    // Save + rebase.
    auto job = makeSaveJob(session, path);
    CHECK(job.has_value());
    if (!job) return;
    CHECK(job->request.formValues != nullptr && job->request.formValues->size() == 5);
    auto result = runDocumentWrite(*engine, *job);
    CHECK(result.written.has_value());
    CHECK(result.rebase.has_value() && result.rebase->has_value());
    if (!result.written || !result.rebase || !result.rebase->has_value()) return;
    job->snapshot.reset();
    CHECK(session.rebaseOnto(std::move(**result.rebase)).has_value());
    session.markSaved();
    CHECK(!session.isDirty());
    CHECK(session.pageSnapshot()->formValues() == nullptr);
    // The written appearances replace the overlay: fresh rasters.
    CHECK(session.pageSnapshot()->at(0).rasterRevision != page0Raster);
    CHECK(session.pageSnapshot()->at(1).rasterRevision != page1Raster);
    // The description is reloaded for the new base (it was in use).
    CHECK(settle(opened.dispatcher, [&] { return session.forms().state() == FormService::State::Ready; }));
    const pdf::PdfFormInfoPtr rebased = session.forms().info();
    const auto* rebasedName = rebased != nullptr ? field(*rebased, "name") : nullptr;
    CHECK(rebasedName != nullptr && rebasedName->value == "Zoe");

    // A fresh session on the saved file sees the values.
    Opened reopened(*engine, path);
    if (reopened.session == nullptr) return;
    const pdf::PdfFormInfoPtr saved = reopened.loadForm();
    if (saved == nullptr) return;
    const pdf::PdfFormField* name = field(*saved, "name");
    CHECK(name != nullptr && name->value == "Zoe");
    const pdf::PdfFormField* agree = field(*saved, "agree");
    CHECK(agree != nullptr && pdf::originalValue(*agree) == pdf::PdfFormValue::ofToggle("Yes"));
    const pdf::PdfFormField* color = field(*saved, "color");
    CHECK(color != nullptr && pdf::originalValue(*color) == pdf::PdfFormValue::ofToggle("Red"));
    const pdf::PdfFormField* fruits = field(*saved, "fruits");
    CHECK(fruits != nullptr && pdf::originalValue(*fruits) == pdf::PdfFormValue::ofChoice({2}));
    const pdf::PdfFormField* city = field(*saved, "city");
    CHECK(city != nullptr && city->value == "Oslo");
    const pdf::PdfFormField* size = field(*saved, "size");
    CHECK(size != nullptr && size->value == "Small"); // untouched
}
