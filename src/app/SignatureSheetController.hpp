// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ShellContext.hpp"
#include "app/SignatureStore.hpp"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfContent.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rivet::ui {
class Button;
class Container;
class TextField;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

class SignaturePad;
class TextLabel;

// The signature sheet (Annotate > Sign... / Manage Signatures..., ADR-0025):
// a panel centered over the viewport, built from rivet_ui widgets.
//
//   Draw   - the pad captures strokes (Clear empties it);
//   Type   - a name typed into a field, previewed in the bundled serif;
//   Image  - a PNG/JPEG chosen with IFileDialog::openImage (<= 2 MiB),
//            decoded by the platform IImageDecoder and scaled to at most
//            pdf::kMaxSignatureImageSide;
//   Saved  - the stored signatures (most recent first): previous/next,
//            Delete.
//
// Done turns the current signature into an appearance, remembers a NEW one
// in the SignatureStore (at most 8; the oldest is dropped) and hands it to
// `onChosen` (the shell arms the Sign tool with it); Cancel / Esc close the
// sheet. The store is loaded on the first open; a corrupt store file is
// ignored with a status message (its content is never logged).
//
// Main thread only (the image is read and decoded synchronously after the
// modal file dialog; both are bounded).
class SignatureSheetController {
public:
    static constexpr double kWidth = 480.0;
    static constexpr double kHeight = 318.0;
    static constexpr std::uint32_t kMaxDecodeSide = 8192;

    enum class Mode { Draw, Type, Image, Saved };

    // Builds the (hidden) panel into `parent` (appended as its next child).
    SignatureSheetController(ShellContext& context, ui::Widget& parent);
    ~SignatureSheetController();

    SignatureSheetController(const SignatureSheetController&) = delete;
    SignatureSheetController& operator=(const SignatureSheetController&) = delete;

    void setOnChosen(std::function<void(pdf::PdfSignatureAppearancePtr)> onChosen) {
        onChosen_ = std::move(onChosen);
    }
    void setInkColor(const pdf::PdfColor& color);

    // Shows the sheet: `manage` opens the saved list, otherwise the saved
    // list when there are saved signatures, else Draw.
    void open(bool manage);
    void close();
    bool visible() const { return visible_; }
    // Esc while open: closes. False when closed.
    bool handleEscape();

    void layout(const core::Rect& viewportFrame);

    Mode mode() const { return mode_; }
    void setMode(Mode mode);
    // Done: the current signature -> onChosen (and the store). False (with a
    // status message) when there is nothing usable yet.
    bool done();
    void clear();
    // Saved mode.
    std::size_t savedIndex() const { return savedIndex_; }
    void showSaved(std::size_t index);
    void deleteSaved();
    // Image mode: asks for a file and loads it (also used by the Image button).
    void chooseImage();
    // Image mode from bytes already read (tests; chooseImage's second half).
    bool loadImage(std::vector<std::uint8_t> encoded);

    SignatureStore& store();
    SignaturePad& pad() { return *pad_; }
    ui::TextField& nameField() { return *nameField_; }

    // A decoded BGRA image as a signature bitmap: scaled down (area average,
    // premultiplied) so neither side exceeds `maxSide`; null for invalid
    // input or an allocation failure.
    static std::shared_ptr<const core::Bitmap> signatureBitmap(const pdf::PdfImageData& image,
                                                               std::uint32_t maxSide = pdf::kMaxSignatureImageSide);

private:
    void ensureStoreLoaded();
    void refresh();
    std::optional<pdf::PdfSignatureAppearance> currentAppearance() const;
    std::shared_ptr<const core::Bitmap> decode(std::span<const std::uint8_t> encoded) const;
    pdf::PdfSignatureAppearancePtr savedAppearance(std::size_t index) const;

    ShellContext& context_;
    std::function<void(pdf::PdfSignatureAppearancePtr)> onChosen_;
    std::unique_ptr<SignatureStore> store_;
    bool storeLoaded_ = false;
    bool visible_ = false;
    Mode mode_ = Mode::Draw;
    std::size_t savedIndex_ = 0;
    std::vector<std::uint8_t> imageBytes_;
    std::shared_ptr<const core::Bitmap> image_;
    core::Rect viewportFrame_;
    pdf::PdfColor inkColor_{0.05F, 0.12F, 0.45F};

    // Raw pointers into widgets owned by the parent's tree.
    ui::Container* panel_ = nullptr;
    TextLabel* title_ = nullptr;
    TextLabel* info_ = nullptr;
    ui::Button* modeButtons_[4] = {nullptr, nullptr, nullptr, nullptr};
    SignaturePad* pad_ = nullptr;
    ui::TextField* nameField_ = nullptr;
    ui::Button* previous_ = nullptr;
    ui::Button* next_ = nullptr;
    ui::Button* delete_ = nullptr;
    ui::Button* clear_ = nullptr;
};

} // namespace rivet::app
