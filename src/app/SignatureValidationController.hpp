// SPDX-License-Identifier: MPL-2.0
#pragma once

// Signature validation panel model (ADR-0048): verifies the saved file on a
// worker (offline: trust anchors configured in Rivet, evidence embedded in the
// file; no network) and hands the view display-ready rows.

#include "app/BackgroundRunner.hpp"
#include "app/DigitalSignatureController.hpp"
#include "app/DocumentSecurityHost.hpp"
#include "app/SecurityText.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rivet::app {

struct ValidationPanelModel {
    std::string documentTitle;
    std::vector<SignatureRowModel> rows;
    std::string summary;   // "2 signatures: 1 valid, 1 needs attention"
    std::string footnote;  // scope of the check (offline) and the legal disclaimer
};

class ISignatureValidationView {
public:
    virtual ~ISignatureValidationView() = default;
    virtual void showLoading(const std::string& title) = 0;
    virtual void showResult(const ValidationPanelModel& model) = 0;
    // No signatures / not saved / failed: replaces the list with one sentence.
    virtual void showMessage(const std::string& message) = 0;
    virtual void close() = 0;
};

class SignatureValidationController {
public:
    using StatusSink = std::function<void(std::string)>;
    using VerifyFn = std::function<pdf::sign::SignResult<std::vector<pdf::sign::PdfSignatureInfo>>(
        crypto::ByteView, crypto::CryptoProvider&, const pdf::sign::VerifyOptions&)>;
    using ReadFileFn = std::function<core::Result<PdfBytes>(const std::filesystem::path&)>;

    SignatureValidationController(IDocumentSecurityHost& host, ISignatureValidationView& view,
                                  SecurityServices services, core::TaskScheduler& scheduler,
                                  core::IMainThreadDispatcher& dispatcher, StatusSink setStatus);
    ~SignatureValidationController();

    SignatureValidationController(const SignatureValidationController&) = delete;
    SignatureValidationController& operator=(const SignatureValidationController&) = delete;

    void setVerifyFunction(VerifyFn fn) { verify_ = std::move(fn); }
    void setReadFileFunction(ReadFileFn fn) { readFile_ = std::move(fn); }

    // Menu command: validates the active document's saved file and shows the panel.
    void begin();
    void close();
    bool busy() const { return busy_; }

    static std::string summarize(const std::vector<SignatureRowModel>& rows);

private:
    struct Job;
    void finish(const std::shared_ptr<Job>& job);

    IDocumentSecurityHost& host_;
    ISignatureValidationView& view_;
    SecurityServices services_;
    StatusSink setStatus_;
    VerifyFn verify_;
    ReadFileFn readFile_;
    bool busy_ = false;
    std::shared_ptr<Job> job_;
    BackgroundRunner runner_;
};

} // namespace rivet::app
