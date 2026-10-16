// SPDX-License-Identifier: MPL-2.0
// Redaction against the real backend (ADR-0026): mark text and a form
// widget, Apply Redactions through the command pipeline, save with the real
// saver and reopen the file. The text is no longer extractable, the widget
// is gone from the page and its field's value is no longer in the file.
// Returns early only when no PDF backend is built in.
#include "RivetTest.h"

#include "PdfFixtures.hpp"
#include "fakes/AnnotationTestSupport.hpp"

#include "core/async/TaskScheduler.hpp"
#include "editor/ContentService.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/RedactionCommands.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfForms.hpp"
#include "pdf/PdfSystem.hpp"
#include "pdf/PdfText.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iterator>
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
                ("rivet-redact-rt-" + std::to_string(::getpid()) + "-" +
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

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

// formPdf() pages are 300 x 400 (display y = 400 - user y).
core::Rect userRect(double left, double bottom, double right, double top) {
    return core::Rect{left, 400.0 - top, right - left, top - bottom};
}

} // namespace

RIVET_TEST(redactionRoundTripRemovesTextWidgetAndFieldValue) {
    auto engine = pdf::createEngine();
    if (engine == nullptr || !engine->isAvailable()) return;
    TempDir dir;
    const fs::path path = dir / "redact.pdf";
    {
        const std::string bytes = pdffix::formPdf();
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    CHECK(readAll(path).find("(Alice)") != std::string::npos);

    core::TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    auto created = DocumentSession::create(*engine, scheduler, &dispatcher, path);
    CHECK(created.has_value());
    if (!created.has_value()) return;
    DocumentSession& session = **created;
    const core::PageId page = session.pageId(0);

    // Everything Apply needs is loaded lazily.
    session.forms().ensureLoaded();
    CHECK(settle(dispatcher, [&] {
        return session.forms().state() == FormService::State::Ready &&
               session.contentService().content(page)->loaded &&
               session.annotations().originals(page) != nullptr;
    }));

    // The "Form" heading and the "name" field (only widget: page 1,
    // (20,340)-(200,360), value "Alice").
    auto marks = addRedactionMarks(session, {{page, userRect(15, 380, 50, 395)}, {page, userRect(15, 335, 205, 365)}});
    CHECK(marks.has_value());
    if (!marks.has_value()) return;
    CHECK(session.execute(std::move(*marks)).has_value());
    auto apply = applyRedactions(session, {});
    CHECK(apply.has_value());
    if (!apply.has_value()) return;
    CHECK(session.execute(std::move(*apply)).has_value());
    CHECK(session.pageSnapshot()->formValues() != nullptr);

    auto job = makeSaveJob(session, path);
    CHECK(job.has_value());
    if (!job) return;
    auto result = runDocumentWrite(*engine, *job);
    CHECK(result.written.has_value());
    CHECK(result.rebase.has_value() && result.rebase->has_value());
    if (!result.written || !result.rebase || !result.rebase->has_value()) return;
    job->snapshot.reset();
    CHECK(session.rebaseOnto(std::move(**result.rebase)).has_value());

    // The value is not in the file any more (no object streams: a /V string
    // would be written verbatim).
    const std::string bytes = readAll(path);
    CHECK(bytes.find("(Alice)") == std::string::npos);
    CHECK(bytes.find("Alice") == std::string::npos);

    auto reopened = engine->openDocument(path, {});
    CHECK(reopened.has_value());
    if (!reopened.has_value()) return;
    const pdf::PdfDocument& document = **reopened;
    auto text = document.textPage(0);
    CHECK(text.has_value());
    if (text.has_value()) CHECK((*text)->text().find("Form") == std::string::npos);
    auto annotations = document.annotations(0);
    CHECK(annotations.has_value());
    if (annotations.has_value()) {
        for (const pdf::PdfPageAnnotation& item : (*annotations)->items) {
            const pdf::PdfBox& r = item.data.rect;
            CHECK(!(r.left < 205 && 15 < r.right && r.bottom < 365 && 335 < r.top));
        }
        // 9 entries before; the name widget is the only one in the area.
        CHECK_EQ((*annotations)->annotsCount, std::uint32_t{8});
    }
    auto form = document.formInfo();
    CHECK(form.has_value());
    if (form.has_value()) {
        const std::size_t index = (*form)->find("name");
        if (index != pdf::PdfFormInfo::npos) CHECK((*form)->fields[index].value.empty());
        // Other fields keep their values.
        const std::size_t city = (*form)->find("city");
        CHECK(city != pdf::PdfFormInfo::npos);
        if (city != pdf::PdfFormInfo::npos) CHECK_EQ((*form)->fields[city].value, std::string("Paris"));
    }
}

// A save rebases the session onto the written file and drops the undo
// history: undoing "Apply Redactions" afterwards cannot bring the redacted
// content back, and a second save writes none of it.
RIVET_TEST(redactionCannotBeUndoneAcrossASaveSoASecondSaveStaysRedacted) {
    auto engine = pdf::createEngine();
    if (engine == nullptr || !engine->isAvailable()) return;
    TempDir dir;
    const fs::path path = dir / "undo.pdf";
    {
        const std::string bytes = pdffix::formPdf();
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    core::TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    auto created = DocumentSession::create(*engine, scheduler, &dispatcher, path);
    CHECK(created.has_value());
    if (!created.has_value()) return;
    DocumentSession& session = **created;
    const core::PageId page = session.pageId(0);
    session.forms().ensureLoaded();
    CHECK(settle(dispatcher, [&] {
        return session.forms().state() == FormService::State::Ready &&
               session.contentService().content(page)->loaded &&
               session.annotations().originals(page) != nullptr;
    }));
    auto marks = addRedactionMarks(session, {{page, userRect(15, 335, 205, 365)}});
    CHECK(marks.has_value());
    if (!marks.has_value()) return;
    CHECK(session.execute(std::move(*marks)).has_value());
    auto apply = applyRedactions(session, {});
    CHECK(apply.has_value());
    if (!apply.has_value()) return;
    CHECK(session.execute(std::move(*apply)).has_value());

    const auto saveOnce = [&]() {
        auto job = makeSaveJob(session, path);
        CHECK(job.has_value());
        if (!job) return false;
        auto result = runDocumentWrite(*engine, *job);
        CHECK(result.written.has_value());
        if (!result.written || !result.rebase || !result.rebase->has_value()) return false;
        job->snapshot.reset();
        CHECK(session.rebaseOnto(std::move(**result.rebase)).has_value());
        return true;
    };
    if (!saveOnce()) return;
    CHECK(readAll(path).find("Alice") == std::string::npos);

    // Undo after the save: nothing to undo (history fenced by the rebase).
    CHECK(!session.undo());
    CHECK(session.pageSnapshot()->formValues() == nullptr);
    // Even if an edit makes the session dirty again, the written file stays clean.
    if (!saveOnce()) return;
    CHECK(readAll(path).find("Alice") == std::string::npos);
}
