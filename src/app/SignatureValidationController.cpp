// SPDX-License-Identifier: MPL-2.0
#include "app/SignatureValidationController.hpp"

#include "crypto/Qualification.hpp"

#include <chrono>
#include <fstream>
#include <optional>
#include <utility>

namespace rivet::app {

namespace {

core::Result<PdfBytes> readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::unexpected(core::makeError(core::ErrorCode::Io, "cannot open the document", "app"));
    const std::streamoff size = in.tellg();
    if (size < 0) return std::unexpected(core::makeError(core::ErrorCode::Io, "cannot size the document", "app"));
    in.seekg(0);
    PdfBytes bytes(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::unexpected(core::makeError(core::ErrorCode::Io, "cannot read the document", "app"));
    }
    return bytes;
}

} // namespace

struct SignatureValidationController::Job {
    std::string title;
    std::filesystem::path path;
    bool dirty = false;
    crypto::CancelToken token;
    std::vector<SignatureRowModel> rows;
    std::optional<std::string> error;
};

SignatureValidationController::SignatureValidationController(IDocumentSecurityHost& host,
                                                             ISignatureValidationView& view, SecurityServices services,
                                                             core::TaskScheduler& scheduler,
                                                             core::IMainThreadDispatcher& dispatcher,
                                                             StatusSink setStatus)
    : host_(host), view_(view), services_(std::move(services)), setStatus_(std::move(setStatus)),
      verify_([](crypto::ByteView pdf, crypto::CryptoProvider& provider, const pdf::sign::VerifyOptions& options) {
          return pdf::sign::verifyPdfSignatures(pdf, provider, options);
      }),
      readFile_(&readWholeFile), runner_(scheduler, dispatcher) {}

SignatureValidationController::~SignatureValidationController() {
    if (job_ != nullptr) job_->token.cancel();
}

std::string SignatureValidationController::summarize(const std::vector<SignatureRowModel>& rows) {
    std::size_t good = 0;
    for (const SignatureRowModel& row : rows) {
        if (row.severity == Severity::Good) ++good;
    }
    const std::size_t other = rows.size() - good;
    std::string text = std::to_string(rows.size()) + (rows.size() == 1 ? " signature" : " signatures");
    if (rows.empty()) return text;
    text += ": " + std::to_string(good) + " valid";
    if (other != 0) text += ", " + std::to_string(other) + (other == 1 ? " needs attention" : " need attention");
    return text;
}

void SignatureValidationController::begin() {
    if (busy_) return;
    host_.commitPendingEdits();
    const SecurityDocumentInfo info = host_.activeDocument();
    if (!info.available) {
        view_.showMessage("Open a PDF document first.");
        return;
    }
    if (info.signatures == SecurityDocumentInfo::Signatures::None) {
        view_.showMessage("This document has no digital signatures.");
        return;
    }
    if (info.path.empty()) {
        view_.showMessage("Save the document first: signatures are checked on the saved file.");
        return;
    }
    if (!services_.provider || !services_.provider->isAvailable()) {
        view_.showMessage("This build of Rivet has no cryptographic backend, so signatures cannot be verified. "
                          "Their presence does not mean they are valid.");
        return;
    }
    auto job = std::make_shared<Job>();
    job->title = info.title;
    job->path = info.path;
    job->dirty = info.dirty;
    job_ = job;
    busy_ = true;
    view_.showLoading(info.title);
    auto services = services_;
    auto verify = verify_;
    auto readFile = readFile_;
    const bool started = runner_.post(
        [job, services, verify, readFile] {
            core::Result<PdfBytes> bytes = readFile(job->path);
            if (!bytes.has_value()) {
                job->error = "The document could not be read: " + bytes.error().message;
                return;
            }
            pdf::sign::VerifyOptions options;
            options.anchors = services.anchors.get();
            options.timestampAnchors = services.timestampAnchors.get();
            options.validationTime = std::chrono::system_clock::now();
            options.cancel = job->token;
            auto infos = verify(*bytes, *services.provider, options);
            if (!infos) {
                job->error = job->token.cancelled() ? std::string("cancelled") : describeSignError(infos.error());
                return;
            }
            const auto now = std::chrono::system_clock::now();
            for (const pdf::sign::PdfSignatureInfo& sig : *infos) {
                job->rows.push_back(makeSignatureRow(sig, services.accredited.get(), now));
            }
        },
        [this, job] { finish(job); });
    if (!started) {
        job_.reset();
        busy_ = false;
    }
}

void SignatureValidationController::finish(const std::shared_ptr<Job>& job) {
    busy_ = false;
    job_.reset();
    if (job->token.cancelled()) {
        view_.close();
        return;
    }
    if (job->error.has_value()) {
        setStatus_(*job->error);
        view_.showMessage(*job->error);
        return;
    }
    if (job->rows.empty()) {
        view_.showMessage("No digital signatures were found in the saved file.");
        return;
    }
    ValidationPanelModel model;
    model.documentTitle = job->title;
    model.rows = std::move(job->rows);
    model.summary = summarize(model.rows);
    model.footnote = "Checked offline against the trusted certificates configured in Rivet and the evidence stored in "
                     "the file; revocation status is not fetched from the network. ";
    if (job->dirty) model.footnote += "Unsaved changes are not included. ";
    model.footnote += std::string(crypto::kLegalDisclaimer);
    setStatus_(model.summary);
    view_.showResult(model);
}

void SignatureValidationController::close() {
    if (job_ != nullptr) job_->token.cancel();
    view_.close();
}

} // namespace rivet::app
