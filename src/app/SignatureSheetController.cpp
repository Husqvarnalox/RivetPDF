// SPDX-License-Identifier: MPL-2.0
#include "app/SignatureSheetController.hpp"
#include "ui/Theme.hpp"

#include "app/SignaturePad.hpp"
#include "app/TextLabel.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/TextField.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

namespace rivet::app {

namespace {

// The sheet swallows every pointer event inside it: nothing reaches the page
// underneath (a click on its background must not place an annotation).
class SheetPanel final : public ui::Card {
public:
    bool onMouse(const ui::PointerEvent& event) override {
        (void)ui::Card::onMouse(event);
        return true;
    }
};

constexpr const char* kModeLabels[4] = {"Draw", "Type", "Image\xE2\x80\xA6", "Saved"};
constexpr double kModeWidths[4] = {64.0, 64.0, 76.0, 84.0};

} // namespace

SignatureSheetController::SignatureSheetController(ShellContext& context, ui::Widget& parent) : context_(context) {
    auto panel = std::make_unique<SheetPanel>();
    panel_ = panel.get();
    panel_->setBackground(&ui::Palette::panelBackground);
    parent.addChild(std::move(panel));

    auto title = std::make_unique<TextLabel>("Sign", ui::Font{13.0, ui::Font::Weight::Semibold}, &ui::Palette::textPrimary,
                                             ui::TextAlign::Left);
    title->setFrame(core::Rect{14.0, 8.0, 452.0, 22.0});
    title_ = title.get();
    panel_->addChild(std::move(title));

    double x = 14.0;
    for (std::size_t i = 0; i < 4; ++i) {
        auto button = std::make_unique<ui::Button>(kModeLabels[i]);
        button->setFrame(core::Rect{x, 36.0, kModeWidths[i], 26.0});
        x += kModeWidths[i] + 4.0;
        const auto mode = static_cast<Mode>(i);
        button->setOnClick([this, mode] {
            if (mode == Mode::Image) {
                chooseImage();
            } else {
                setMode(mode);
            }
        });
        modeButtons_[i] = button.get();
        panel_->addChild(std::move(button));
    }

    auto pad = std::make_unique<SignaturePad>();
    pad->setFrame(core::Rect{14.0, 70.0, 452.0, 160.0});
    pad->setOnChanged([this] { refresh(); });
    pad_ = pad.get();
    panel_->addChild(std::move(pad));

    auto field = std::make_unique<ui::TextField>("Type your name");
    nameField_ = field.get();
    nameField_->setOnFocusRequested([this] { context_.setFocus(nameField_); });
    nameField_->setOnTextChanged([this](const std::string&) { refresh(); });
    nameField_->setOnEnter([this] { (void)done(); });
    nameField_->setOnEscape([this] { close(); });
    panel_->addChild(std::move(field));

    auto previous = std::make_unique<ui::Button>("\xE2\x97\x80");
    previous_ = previous.get();
    previous_->setOnClick([this] {
        if (savedIndex_ > 0) showSaved(savedIndex_ - 1);
    });
    panel_->addChild(std::move(previous));
    auto next = std::make_unique<ui::Button>("\xE2\x96\xB6");
    next_ = next.get();
    next_->setOnClick([this] { showSaved(savedIndex_ + 1); });
    panel_->addChild(std::move(next));
    auto remove = std::make_unique<ui::Button>("Delete");
    delete_ = remove.get();
    delete_->setOnClick([this] { deleteSaved(); });
    panel_->addChild(std::move(remove));

    auto info = std::make_unique<TextLabel>("", ui::Font{12.0}, &ui::Palette::textSecondary, ui::TextAlign::Left);
    info_ = info.get();
    panel_->addChild(std::move(info));

    auto clearButton = std::make_unique<ui::Button>("Clear");
    clearButton->setFrame(core::Rect{14.0, 278.0, 70.0, 28.0});
    clearButton->setOnClick([this] { clear(); });
    clear_ = clearButton.get();
    panel_->addChild(std::move(clearButton));

    auto cancel = std::make_unique<ui::Button>("Cancel");
    cancel->setFrame(core::Rect{300.0, 278.0, 78.0, 28.0});
    cancel->setOnClick([this] { close(); });
    panel_->addChild(std::move(cancel));

    auto doneButton = std::make_unique<ui::Button>("Done");
    doneButton->setFrame(core::Rect{388.0, 278.0, 78.0, 28.0});
    doneButton->setOnClick([this] { (void)done(); });
    panel_->addChild(std::move(doneButton));

    panel_->setFrame(kHiddenFrame);
}

SignatureSheetController::~SignatureSheetController() = default;

SignatureStore& SignatureSheetController::store() {
    ensureStoreLoaded();
    return *store_;
}

void SignatureSheetController::ensureStoreLoaded() {
    if (storeLoaded_) return;
    storeLoaded_ = true;
    std::filesystem::path file;
    if (context_.services.appData != nullptr) {
        if (const auto directory = context_.services.appData->directory(); directory.has_value()) {
            file = *directory / SignatureStore::kFileName;
        }
    }
    store_ = std::make_unique<SignatureStore>(std::move(file));
    switch (store_->load()) {
    case SignatureStore::LoadResult::Loaded:
    case SignatureStore::LoadResult::Missing:
        break;
    case SignatureStore::LoadResult::Corrupt:
        context_.setStatus("Saved signatures could not be read and were ignored");
        break;
    case SignatureStore::LoadResult::Unreadable:
        context_.setStatus("Saved signatures could not be read");
        break;
    }
}

void SignatureSheetController::setInkColor(const pdf::PdfColor& color) {
    inkColor_ = color;
    pad_->setInkColor(color);
}

void SignatureSheetController::open(bool manage) {
    ensureStoreLoaded();
    visible_ = true;
    title_->setText(manage ? "Saved Signatures" : "Sign");
    savedIndex_ = 0;
    setMode((manage || !store_->entries().empty()) ? Mode::Saved : Mode::Draw);
    layout(viewportFrame_);
}

void SignatureSheetController::close() {
    if (!visible_) return;
    visible_ = false;
    if (nameField_->isFocused()) context_.setFocus(nullptr);
    // Nothing personal lingers in hidden widgets.
    nameField_->setText("");
    pad_->clear();
    pad_->setPreview(nullptr);
    imageBytes_.clear();
    image_.reset();
    panel_->setFrame(kHiddenFrame);
}

bool SignatureSheetController::handleEscape() {
    if (!visible_) return false;
    close();
    return true;
}

void SignatureSheetController::layout(const core::Rect& viewportFrame) {
    viewportFrame_ = viewportFrame;
    if (!visible_) return;
    panel_->setFrame(core::Rect{std::max(viewportFrame.origin.x, viewportFrame.center().x - kWidth / 2.0),
                                std::max(viewportFrame.origin.y, viewportFrame.center().y - kHeight / 2.0), kWidth,
                                kHeight});
}

void SignatureSheetController::setMode(Mode mode) {
    mode_ = mode;
    if (mode != Mode::Type && nameField_->isFocused()) context_.setFocus(nullptr);
    if (mode == Mode::Type) context_.setFocus(nameField_);
    if (mode == Mode::Saved) {
        ensureStoreLoaded();
        savedIndex_ = std::min(savedIndex_, store_->entries().empty() ? 0 : store_->entries().size() - 1);
    }
    refresh();
}

void SignatureSheetController::refresh() {
    for (std::size_t i = 0; i < 4; ++i) modeButtons_[i]->setActive(static_cast<std::size_t>(mode_) == i);
    const bool saved = mode_ == Mode::Saved;
    nameField_->setFrame(mode_ == Mode::Type ? core::Rect{14.0, 238.0, 452.0, 28.0} : kHiddenFrame);
    previous_->setFrame(saved ? core::Rect{14.0, 238.0, 36.0, 26.0} : kHiddenFrame);
    next_->setFrame(saved ? core::Rect{54.0, 238.0, 36.0, 26.0} : kHiddenFrame);
    delete_->setFrame(saved ? core::Rect{94.0, 238.0, 70.0, 26.0} : kHiddenFrame);
    info_->setFrame(mode_ == Mode::Type ? kHiddenFrame
                                        : core::Rect{saved ? 172.0 : 14.0, 238.0, saved ? 294.0 : 452.0, 26.0});
    clear_->setFrame(mode_ == Mode::Draw || mode_ == Mode::Type ? core::Rect{14.0, 278.0, 70.0, 28.0} : kHiddenFrame);

    switch (mode_) {
    case Mode::Draw:
        pad_->setMode(SignaturePad::Mode::Capture);
        pad_->setHint("Draw your signature here");
        info_->setText("Press and drag to sign; Clear starts over.");
        break;
    case Mode::Type: {
        pad_->setMode(SignaturePad::Mode::Preview);
        pad_->setHint("Type your name below");
        const auto ap = currentAppearance();
        pad_->setPreview(ap.has_value() ? std::make_shared<const pdf::PdfSignatureAppearance>(*ap) : nullptr);
        break;
    }
    case Mode::Image:
        pad_->setMode(SignaturePad::Mode::Preview);
        pad_->setHint("Choose a PNG or JPEG image");
        if (image_ != nullptr) {
            pdf::PdfSignatureAppearance ap;
            ap.source = pdf::PdfSignatureSource::Image;
            ap.image = image_;
            pad_->setPreview(std::make_shared<const pdf::PdfSignatureAppearance>(std::move(ap)));
            info_->setText(std::format("Image {} x {} pixels", image_->width(), image_->height()));
        } else {
            pad_->setPreview(nullptr);
            info_->setText("");
        }
        break;
    case Mode::Saved: {
        pad_->setMode(SignaturePad::Mode::Preview);
        const std::size_t count = store_ != nullptr ? store_->entries().size() : 0;
        pad_->setHint(count == 0 ? "No saved signatures yet" : "");
        pad_->setPreview(count == 0 ? nullptr : savedAppearance(savedIndex_));
        info_->setText(count == 0 ? std::string("Signatures you make are saved here (up to 8).")
                                  : std::format("Saved signature {} of {}", savedIndex_ + 1, count));
        break;
    }
    }
    panel_->invalidate();
}

std::shared_ptr<const core::Bitmap> SignatureSheetController::decode(std::span<const std::uint8_t> encoded) const {
    if (context_.services.imageDecoder == nullptr || encoded.empty()) return nullptr;
    platform::IImageDecoder::BgraLimits limits;
    limits.maxSide = kMaxDecodeSide;
    const auto decoded = context_.services.imageDecoder->decodeBgra(encoded, limits);
    if (!decoded.has_value()) return nullptr;
    return signatureBitmap(*decoded);
}

pdf::PdfSignatureAppearancePtr SignatureSheetController::savedAppearance(std::size_t index) const {
    if (store_ == nullptr || index >= store_->entries().size()) return nullptr;
    const SignatureStore::Entry& entry = store_->entries()[index];
    if (entry.source == pdf::PdfSignatureSource::Image) {
        auto bitmap = decode(entry.image);
        if (bitmap == nullptr) return nullptr;
        pdf::PdfSignatureAppearance ap;
        ap.source = pdf::PdfSignatureSource::Image;
        ap.image = std::move(bitmap);
        return std::make_shared<const pdf::PdfSignatureAppearance>(std::move(ap));
    }
    auto ap = SignatureStore::appearance(entry);
    return ap.has_value() ? std::make_shared<const pdf::PdfSignatureAppearance>(std::move(*ap)) : nullptr;
}

std::optional<pdf::PdfSignatureAppearance> SignatureSheetController::currentAppearance() const {
    switch (mode_) {
    case Mode::Draw:
        return pdf::makeInkSignature(pad_->strokes(), SignaturePad::kPenWidth);
    case Mode::Type: {
        pdf::PdfSignatureAppearance ap;
        ap.source = pdf::PdfSignatureSource::Text;
        ap.text = nameField_->text();
        if (!pdf::isValidSignature(ap)) return std::nullopt;
        return ap;
    }
    case Mode::Image: {
        if (image_ == nullptr) return std::nullopt;
        pdf::PdfSignatureAppearance ap;
        ap.source = pdf::PdfSignatureSource::Image;
        ap.image = image_;
        return ap;
    }
    case Mode::Saved: {
        const pdf::PdfSignatureAppearancePtr ap = savedAppearance(savedIndex_);
        if (ap == nullptr) return std::nullopt;
        return *ap;
    }
    }
    return std::nullopt;
}

bool SignatureSheetController::done() {
    const std::optional<pdf::PdfSignatureAppearance> appearance = currentAppearance();
    if (!appearance.has_value() || !pdf::isValidSignature(*appearance)) {
        switch (mode_) {
        case Mode::Draw: context_.setStatus("Draw a signature first"); break;
        case Mode::Type: context_.setStatus("Type a name first (one line, at most 256 bytes)"); break;
        case Mode::Image: context_.setStatus("Choose an image first"); break;
        case Mode::Saved: context_.setStatus("There is no saved signature to use"); break;
        }
        return false;
    }
    ensureStoreLoaded();
    // Remember a new signature (a saved one just moves to the front).
    std::optional<SignatureStore::Entry> entry;
    if (mode_ == Mode::Image) {
        SignatureStore::Entry image;
        image.source = pdf::PdfSignatureSource::Image;
        image.image = imageBytes_;
        entry = std::move(image);
    } else if (mode_ == Mode::Saved) {
        entry = store_->entries()[savedIndex_];
    } else {
        entry = SignatureStore::fromAppearance(*appearance);
    }
    if (entry.has_value() && store_->add(std::move(*entry)) && store_->persistent()) {
        if (const auto saved = store_->save(); !saved.has_value()) {
            context_.setStatus("The signature is used but could not be saved for later");
        }
    }
    auto chosen = std::make_shared<const pdf::PdfSignatureAppearance>(*appearance);
    close();
    if (onChosen_) onChosen_(std::move(chosen));
    return true;
}

void SignatureSheetController::clear() {
    if (mode_ == Mode::Draw) pad_->clear();
    if (mode_ == Mode::Type) nameField_->setText("");
    refresh();
}

void SignatureSheetController::showSaved(std::size_t index) {
    ensureStoreLoaded();
    if (store_->entries().empty()) {
        savedIndex_ = 0;
    } else {
        savedIndex_ = std::min(index, store_->entries().size() - 1);
    }
    refresh();
}

void SignatureSheetController::deleteSaved() {
    ensureStoreLoaded();
    if (!store_->remove(savedIndex_)) return;
    if (store_->persistent()) {
        if (const auto saved = store_->save(); !saved.has_value()) {
            context_.setStatus("The saved signatures could not be updated");
        }
    }
    showSaved(savedIndex_);
}

void SignatureSheetController::chooseImage() {
    if (context_.services.fileDialog == nullptr || context_.services.imageDecoder == nullptr) {
        context_.setStatus("Images cannot be chosen on this platform");
        return;
    }
    const auto chosen = context_.services.fileDialog->openImage();
    if (!chosen.has_value()) {
        if (chosen.error().code != core::ErrorCode::Cancelled) context_.setStatus("No image chooser is available");
        return;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(*chosen, ec);
    if (ec || size == 0) {
        context_.setStatus("The image could not be read");
        return;
    }
    if (size > SignatureStore::kMaxImageBytes) {
        context_.setStatus("The image is larger than 2 MB");
        return;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream in(*chosen, std::ios::binary);
    if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        context_.setStatus("The image could not be read");
        return;
    }
    (void)loadImage(std::move(bytes));
}

bool SignatureSheetController::loadImage(std::vector<std::uint8_t> encoded) {
    if (encoded.empty() || encoded.size() > SignatureStore::kMaxImageBytes) {
        context_.setStatus("The image is larger than 2 MB");
        return false;
    }
    auto bitmap = decode(encoded);
    if (bitmap == nullptr) {
        context_.setStatus("The image could not be decoded (PNG or JPEG)");
        return false;
    }
    imageBytes_ = std::move(encoded);
    image_ = std::move(bitmap);
    setMode(Mode::Image);
    return true;
}

std::shared_ptr<const core::Bitmap> SignatureSheetController::signatureBitmap(const pdf::PdfImageData& image,
                                                                              std::uint32_t maxSide) {
    if (image.format != pdf::PdfImageData::Format::Bgra || image.width == 0 || image.height == 0 || maxSide == 0 ||
        image.stride < static_cast<std::size_t>(image.width) * 4u ||
        image.bytes.size() < static_cast<std::size_t>(image.stride) * image.height) {
        return nullptr;
    }
    const double scale =
        std::min(1.0, static_cast<double>(maxSide) / static_cast<double>(std::max(image.width, image.height)));
    const auto w = static_cast<std::uint32_t>(std::max(1.0, std::floor(image.width * scale)));
    const auto h = static_cast<std::uint32_t>(std::max(1.0, std::floor(image.height * scale)));
    auto created = core::Bitmap::create(w, h);
    if (!created.has_value()) return nullptr;
    core::Bitmap out = std::move(*created);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t y0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * image.height / h);
        const std::uint32_t y1 =
            std::max(y0 + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(y + 1) * image.height / h));
        auto* row = reinterpret_cast<std::uint8_t*>(out.data()) + static_cast<std::size_t>(y) * out.stride();
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t x0 = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * image.width / w);
            const std::uint32_t x1 =
                std::max(x0 + 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(x + 1) * image.width / w));
            // Premultiplied sums so transparent pixels do not darken edges.
            double b = 0.0, g = 0.0, r = 0.0, a = 0.0;
            for (std::uint32_t sy = y0; sy < y1; ++sy) {
                const std::uint8_t* src = image.bytes.data() + static_cast<std::size_t>(sy) * image.stride;
                for (std::uint32_t sx = x0; sx < x1; ++sx) {
                    const std::uint8_t* p = src + static_cast<std::size_t>(sx) * 4u;
                    const double alpha = p[3] / 255.0;
                    b += p[0] * alpha;
                    g += p[1] * alpha;
                    r += p[2] * alpha;
                    a += alpha;
                }
            }
            const double n = static_cast<double>(x1 - x0) * static_cast<double>(y1 - y0);
            std::uint8_t* d = row + static_cast<std::size_t>(x) * 4u;
            if (a <= 0.0) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            d[0] = static_cast<std::uint8_t>(std::clamp(std::lround(b / a), 0L, 255L));
            d[1] = static_cast<std::uint8_t>(std::clamp(std::lround(g / a), 0L, 255L));
            d[2] = static_cast<std::uint8_t>(std::clamp(std::lround(r / a), 0L, 255L));
            d[3] = static_cast<std::uint8_t>(std::clamp(std::lround(a / n * 255.0), 0L, 255L));
        }
    }
    return std::make_shared<const core::Bitmap>(std::move(out));
}

} // namespace rivet::app
