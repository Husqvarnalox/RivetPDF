// SPDX-License-Identifier: MPL-2.0
// The signature sheet (ADR-0025): Draw / Type / Image / Saved -> Done arms a
// signature and remembers it in the store; corrupt store files are ignored
// with a status message; images are bounded and scaled down.
#include "RivetTest.h"

#include "fakes/FakePageDocument.hpp"

#include "app/DocumentWorkspace.hpp"
#include "app/ShellContext.hpp"
#include "app/SignaturePad.hpp"
#include "app/SignatureSheetController.hpp"
#include "app/SignatureStore.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "platform/AppDataLocation.hpp"
#include "platform/ImageDecoder.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextField.hpp"
#include "ui/UiTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace rivet;
using app::SignatureSheetController;
using app::SignatureStore;
using Mode = SignatureSheetController::Mode;
namespace fs = std::filesystem;

namespace {

class NoDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()>) override {}
};

class TempAppData final : public platform::IAppDataLocation {
public:
    fs::path dir;
    core::Result<fs::path> directory() const override { return dir; }
};

// Answers decodeBgra with a solid BGRA image of the configured size.
class SolidDecoder final : public platform::IImageDecoder {
public:
    std::uint32_t width = 40;
    std::uint32_t height = 20;
    bool fail = false;
    mutable int calls = 0;
    core::Result<pdf::PdfImageData> decode(std::span<const std::uint8_t>) const override {
        ++calls;
        if (fail) return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "bad", "test"));
        pdf::PdfImageData data;
        data.format = pdf::PdfImageData::Format::Bgra;
        data.width = width;
        data.height = height;
        data.stride = width * 4;
        data.bytes.assign(static_cast<std::size_t>(data.stride) * height, std::uint8_t{0xFF});
        return data;
    }
};

class ImageDialog final : public platform::IFileDialog {
public:
    fs::path image;
    core::Result<fs::path> openPdf() override {
        return std::unexpected(core::makeError(core::ErrorCode::Cancelled, "cancel", "test"));
    }
    core::Result<fs::path> openImage() override {
        if (image.empty()) return std::unexpected(core::makeError(core::ErrorCode::Cancelled, "cancel", "test"));
        return image;
    }
};

fs::path freshDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / (std::string{"rivet-sigsheet-"} + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

struct Sheet {
    test::FakePageEngine engine{1};
    core::TaskScheduler scheduler{1};
    NoDispatcher dispatcher;
    app::DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    platform::ShellServices services;
    TempAppData appData;
    SolidDecoder decoder;
    ImageDialog dialog;
    std::unique_ptr<ui::Container> root = std::make_unique<ui::Container>();
    ui::PdfViewport* viewport = nullptr;
    ui::Widget* focused = nullptr;
    std::vector<std::string> status;
    std::unique_ptr<app::ShellContext> context;
    std::unique_ptr<SignatureSheetController> sheet;
    std::vector<pdf::PdfSignatureAppearancePtr> chosen;

    explicit Sheet(const fs::path& dir) {
        appData.dir = dir;
        services.mainDispatcher = &dispatcher;
        services.appData = &appData;
        services.imageDecoder = &decoder;
        services.fileDialog = &dialog;
        auto vp = std::make_unique<ui::PdfViewport>();
        viewport = vp.get();
        viewport->setFrame(core::Rect{0.0, 0.0, 800.0, 600.0});
        root->addChild(std::move(vp));
        context = std::make_unique<app::ShellContext>(app::ShellContext{
            workspace,
            services,
            *viewport,
            [this](std::string message) { status.push_back(std::move(message)); },
            [this](ui::Widget* widget) {
                if (focused == widget) return;
                if (focused != nullptr) focused->setFocused(false);
                focused = widget;
                if (focused != nullptr) focused->setFocused(true);
            },
            [] {},
        });
        sheet = std::make_unique<SignatureSheetController>(*context, *root);
        sheet->layout(viewport->frame());
        sheet->setOnChosen([this](pdf::PdfSignatureAppearancePtr signature) { chosen.push_back(std::move(signature)); });
    }

    void stroke(double x0, double y0, double x1, double y1) {
        ui::PointerEvent down;
        down.type = ui::PointerEventType::Down;
        down.button = 1;
        down.position = core::Point{x0, y0};
        sheet->pad().onMouse(down);
        ui::PointerEvent move = down;
        move.type = ui::PointerEventType::Move;
        move.position = core::Point{(x0 + x1) / 2.0, (y0 + y1) / 2.0};
        sheet->pad().onMouse(move);
        move.position = core::Point{x1, y1};
        sheet->pad().onMouse(move);
        ui::PointerEvent up = move;
        up.type = ui::PointerEventType::Up;
        sheet->pad().onMouse(up);
    }
};

} // namespace

RIVET_TEST(signatureSheetDrawDoneArmsInkAndPersistsIt) {
    const fs::path dir = freshDir("draw");
    {
        Sheet s(dir);
        s.sheet->open(false);
        CHECK(s.sheet->visible());
        CHECK(s.sheet->mode() == Mode::Draw); // nothing saved yet
        CHECK(!s.sheet->done());              // empty pad
        CHECK(!s.status.empty() && s.status.back() == "Draw a signature first");
        CHECK(s.chosen.empty());

        s.stroke(20.0, 100.0, 300.0, 40.0);
        CHECK(s.sheet->pad().strokes().size() == 1u);
        CHECK(s.sheet->done());
        CHECK(!s.sheet->visible());
        CHECK(s.chosen.size() == 1u);
        CHECK(s.chosen[0]->source == pdf::PdfSignatureSource::Ink);
        CHECK(pdf::isValidSignature(*s.chosen[0]));
        CHECK(fs::exists(dir / SignatureStore::kFileName));
    }
    {
        // A new sheet reads the stored signature and opens on the saved list.
        Sheet s(dir);
        s.sheet->open(false);
        CHECK(s.sheet->mode() == Mode::Saved);
        CHECK(s.sheet->store().entries().size() == 1u);
        CHECK(s.sheet->done());
        CHECK(s.chosen.size() == 1u && s.chosen[0]->source == pdf::PdfSignatureSource::Ink);
        CHECK(s.sheet->store().entries().size() == 1u); // reuse does not duplicate
    }
    fs::remove_all(dir);
}

RIVET_TEST(signatureSheetTypedNameAndSavedDelete) {
    const fs::path dir = freshDir("type");
    Sheet s(dir);
    s.sheet->open(false);
    s.sheet->setMode(Mode::Type);
    CHECK(s.focused == &s.sheet->nameField());
    CHECK(!s.sheet->done());
    s.sheet->nameField().setText("Jane Doe");
    CHECK(s.sheet->done());
    CHECK(s.focused == nullptr); // focus released on close
    CHECK(s.chosen.size() == 1u);
    CHECK(s.chosen[0]->source == pdf::PdfSignatureSource::Text);
    CHECK(s.chosen[0]->text == "Jane Doe");

    // Closing cleared the personal data from the field.
    s.sheet->open(false);
    s.sheet->setMode(Mode::Type);
    CHECK(s.sheet->nameField().text().empty());
    s.sheet->nameField().setText("J. D.");
    CHECK(s.sheet->done());
    CHECK(s.sheet->store().entries().size() == 2u);
    CHECK(s.sheet->store().entries()[0].text == "J. D."); // most recent first

    s.sheet->open(true);
    CHECK(s.sheet->mode() == Mode::Saved);
    s.sheet->showSaved(5); // clamped
    CHECK(s.sheet->savedIndex() == 1u);
    s.sheet->deleteSaved();
    CHECK(s.sheet->store().entries().size() == 1u);
    CHECK(s.sheet->savedIndex() == 0u);
    s.sheet->deleteSaved();
    CHECK(s.sheet->store().entries().empty());
    CHECK(!s.sheet->done());
    CHECK(s.sheet->handleEscape());
    CHECK(!s.sheet->visible());
    CHECK(!s.sheet->handleEscape());

    SignatureStore reread(dir / SignatureStore::kFileName);
    CHECK(reread.load() == SignatureStore::LoadResult::Loaded);
    CHECK(reread.entries().empty());
    fs::remove_all(dir);
}

RIVET_TEST(signatureSheetIgnoresACorruptStoreWithAStatus) {
    const fs::path dir = freshDir("corrupt");
    fs::create_directories(dir);
    {
        std::ofstream out(dir / SignatureStore::kFileName, std::ios::binary);
        out << "RVSG garbage that is not a valid store";
    }
    Sheet s(dir);
    s.sheet->open(false);
    CHECK(s.sheet->mode() == Mode::Draw);
    CHECK(s.sheet->store().entries().empty());
    CHECK(!s.status.empty() && s.status.back() == "Saved signatures could not be read and were ignored");
    // Saving a new one replaces the corrupt file.
    s.sheet->setMode(Mode::Type);
    s.sheet->nameField().setText("A");
    CHECK(s.sheet->done());
    SignatureStore reread(dir / SignatureStore::kFileName);
    CHECK(reread.load() == SignatureStore::LoadResult::Loaded);
    CHECK(reread.entries().size() == 1u);
    fs::remove_all(dir);
}

RIVET_TEST(signatureSheetImageIsBoundedDecodedAndScaled) {
    const fs::path dir = freshDir("image");
    Sheet s(dir);
    s.sheet->open(false);
    s.decoder.width = 3000;
    s.decoder.height = 1500;
    CHECK(s.sheet->loadImage(std::vector<std::uint8_t>(16, std::uint8_t{1})));
    CHECK(s.sheet->mode() == Mode::Image);
    CHECK(s.sheet->done());
    CHECK(s.chosen.size() == 1u);
    const auto& image = s.chosen[0]->image;
    CHECK(s.chosen[0]->source == pdf::PdfSignatureSource::Image);
    CHECK(image != nullptr);
    CHECK(image->width() == pdf::kMaxSignatureImageSide);
    CHECK(image->height() == pdf::kMaxSignatureImageSide / 2);
    CHECK(s.sheet->store().entries().size() == 1u);
    CHECK(s.sheet->store().entries()[0].image.size() == 16u);

    // Too large / undecodable / empty are refused with a status.
    CHECK(!s.sheet->loadImage(std::vector<std::uint8_t>(SignatureStore::kMaxImageBytes + 1, std::uint8_t{1})));
    s.decoder.fail = true;
    CHECK(!s.sheet->loadImage(std::vector<std::uint8_t>(16, std::uint8_t{1})));
    CHECK(s.status.back() == "The image could not be decoded (PNG or JPEG)");
    CHECK(!s.sheet->loadImage({}));

    // The file chooser path: cancelled is silent, a real file loads.
    s.decoder.fail = false;
    s.sheet->open(false);
    const std::size_t before = s.status.size();
    s.sheet->chooseImage();
    CHECK(s.status.size() == before);
    s.dialog.image = dir / "sig.png";
    {
        std::ofstream out(s.dialog.image, std::ios::binary);
        out << "not really a png";
    }
    s.decoder.width = 40;
    s.decoder.height = 20;
    s.sheet->chooseImage();
    CHECK(s.sheet->mode() == Mode::Image);
    CHECK(s.sheet->done());
    CHECK(s.chosen.back()->image != nullptr && s.chosen.back()->image->width() == 40u);
    fs::remove_all(dir);
}

RIVET_TEST(signatureBitmapAveragesAndRejectsInvalidInput) {
    pdf::PdfImageData image;
    image.format = pdf::PdfImageData::Format::Bgra;
    image.width = 4;
    image.height = 2;
    image.stride = 16;
    image.bytes.assign(32, std::uint8_t{0});
    // Left half opaque white, right half transparent.
    for (std::uint32_t y = 0; y < 2; ++y) {
        for (std::uint32_t x = 0; x < 2; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) image.bytes[y * 16 + x * 4 + c] = 0xFF;
        }
    }
    const auto half = SignatureSheetController::signatureBitmap(image, 2);
    CHECK(half != nullptr);
    CHECK(half->width() == 2u && half->height() == 1u);
    const auto* px = reinterpret_cast<const std::uint8_t*>(half->data());
    CHECK(px[3] == 0xFF); // left pixel opaque
    CHECK(px[7] == 0x00); // right pixel transparent

    const auto same = SignatureSheetController::signatureBitmap(image, 1024);
    CHECK(same != nullptr && same->width() == 4u && same->height() == 2u);

    pdf::PdfImageData bad = image;
    bad.bytes.resize(8);
    CHECK(SignatureSheetController::signatureBitmap(bad) == nullptr);
    bad = image;
    bad.format = pdf::PdfImageData::Format::Jpeg;
    CHECK(SignatureSheetController::signatureBitmap(bad) == nullptr);
    CHECK(SignatureSheetController::signatureBitmap(pdf::PdfImageData{}) == nullptr);
}
