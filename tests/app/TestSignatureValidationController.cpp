// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "SecurityTestKit.hpp"
#include "app/SignatureValidationController.hpp"
#include "core/async/TaskScheduler.hpp"

#include <algorithm>
#include <cstdio>

using namespace rivet;
using namespace rivet::app;
using namespace rivet::app::sectest;

namespace {

class FakeView final : public ISignatureValidationView {
public:
    int loading = 0, results = 0, messages = 0, closed = 0;
    std::string lastMessage;
    ValidationPanelModel model;
    void showLoading(const std::string&) override { ++loading; }
    void showResult(const ValidationPanelModel& m) override { ++results; model = m; }
    void showMessage(const std::string& m) override { ++messages; lastMessage = m; }
    void close() override { ++closed; }
};

pdf::sign::PdfSignatureInfo makeInfo(std::size_t index, crypto::SignatureState state) {
    pdf::sign::PdfSignatureInfo info;
    info.index = index;
    info.fieldName = "Sig" + std::to_string(index + 1);
    info.validation.state = state;
    info.level = pdf::sign::PadesLevel::BaselineB;
    info.signerName = "Jane Doe";
    return info;
}

struct Rig {
    TempDir dir{"validate"};
    QueueDispatcher dispatcher;
    core::TaskScheduler scheduler{2};
    FakeHost host;
    FakeView view;
    std::vector<std::string> statuses;
    std::unique_ptr<SignatureValidationController> controller;

    explicit Rig(std::vector<pdf::sign::PdfSignatureInfo> infos) {
        host.info = readyDocument(dir.path / "signed.pdf", true);
        SecurityServices services;
        services.provider = crypto::makeDefaultProvider();
        controller = std::make_unique<SignatureValidationController>(host, view, std::move(services), scheduler, dispatcher,
                                                                     [this](std::string s) { statuses.push_back(std::move(s)); });
        controller->setReadFileFunction([](const std::filesystem::path&) -> core::Result<PdfBytes> { return PdfBytes{'%'}; });
        controller->setVerifyFunction([infos](crypto::ByteView, crypto::CryptoProvider&, const pdf::sign::VerifyOptions&)
                                          -> pdf::sign::SignResult<std::vector<pdf::sign::PdfSignatureInfo>> { return infos; });
    }
    bool settle() { return dispatcher.pumpUntil([&] { return !controller->busy(); }); }
};

} // namespace

RIVET_TEST(validationShowsOneRowPerSignatureWithSummary) {
    Rig r({makeInfo(0, crypto::SignatureState::ValidTrusted), makeInfo(1, crypto::SignatureState::DocumentModified)});
    r.controller->begin();
    CHECK(r.view.loading == 1);
    CHECK(r.settle());
    CHECK_EQ(r.view.results, 1);
    CHECK_EQ(r.view.model.rows.size(), std::size_t{2});
    CHECK(r.view.model.summary.find("2 signatures") != std::string::npos);
    CHECK(r.view.model.summary.find("1 valid") != std::string::npos);
    CHECK(r.view.model.summary.find("1 needs attention") != std::string::npos);
    CHECK(r.view.model.rows[1].severity == Severity::Bad);
    CHECK(r.view.model.footnote.find("offline") != std::string::npos);
}

RIVET_TEST(validationNeverClaimsQualifiedForFileOnlyEvidence) {
    Rig r({makeInfo(0, crypto::SignatureState::ValidTrusted)});
    r.controller->begin();
    CHECK(r.settle());
    CHECK_EQ(r.view.model.rows.size(), std::size_t{1});
    for (const SignatureRowModel& row : r.view.model.rows) {
        std::string low = row.qualification;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (low.find("all technical indicators") != std::string::npos) {
            std::fprintf(stderr, "file-only row claims qualified: %s\n", row.qualification.c_str());
        }
        CHECK(low.find("all technical indicators") == std::string::npos);
        CHECK(!row.disclaimer.empty());
    }
}

RIVET_TEST(validationExplainsUnsignedMissingAndUnsavedDocuments) {
    Rig r({});
    r.host.info.signatures = SecurityDocumentInfo::Signatures::None;
    r.controller->begin();
    CHECK(r.view.lastMessage.find("no digital signatures") != std::string::npos);
    r.host.info.signatures = SecurityDocumentInfo::Signatures::Present;
    r.host.info.path.clear();
    r.controller->begin();
    CHECK(r.view.lastMessage.find("Save the document first") != std::string::npos);
    r.host.info.available = false;
    r.controller->begin();
    CHECK_EQ(r.view.messages, 3);
    CHECK_EQ(r.view.loading, 0);
}

RIVET_TEST(validationReportsUnreadableFileAndEmptyResult) {
    Rig r({});
    r.controller->begin();
    CHECK(r.settle());
    CHECK_EQ(r.view.results, 0);
    CHECK(r.view.lastMessage.find("No digital signatures were found") != std::string::npos);
    r.controller->setReadFileFunction([](const std::filesystem::path&) -> core::Result<PdfBytes> {
        return std::unexpected(core::makeError(core::ErrorCode::Io, "gone", "t"));
    });
    r.controller->begin();
    CHECK(r.settle());
    CHECK(r.view.lastMessage.find("could not be read") != std::string::npos);
}

RIVET_TEST(validationSummaryText) {
    CHECK_EQ(SignatureValidationController::summarize({}), std::string("0 signatures"));
    SignatureRowModel good;
    good.severity = Severity::Good;
    CHECK_EQ(SignatureValidationController::summarize({good}), std::string("1 signature: 1 valid"));
}
