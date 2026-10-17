// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "pdf/PdfAnnotation.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace rivet::app {

// The canvas of the signature sheet (ADR-0025). In Capture mode a press and
// drag draws a stroke (widget-local points, y-down; capped at
// pdf::kMaxSignatureStrokes strokes and pdf::kMaxSignaturePoints points); in
// Preview mode it shows a finished signature appearance (typed, image or a
// saved one) fitted into the pad exactly as it will be placed. Main thread.
class SignaturePad final : public ui::Widget {
public:
    static constexpr double kPenWidth = 2.5;  // logical points
    static constexpr double kInset = 12.0;    // preview margin

    SignaturePad() = default;

    enum class Mode { Capture, Preview };
    void setMode(Mode mode);
    Mode mode() const { return mode_; }

    // Capture.
    const std::vector<std::vector<pdf::PdfPoint>>& strokes() const { return strokes_; }
    bool empty() const { return strokes_.empty(); }
    void clear();
    void setOnChanged(std::function<void()> onChanged) { onChanged_ = std::move(onChanged); }

    // Preview (null: an empty pad with the hint).
    void setPreview(pdf::PdfSignatureAppearancePtr signature);
    const pdf::PdfSignatureAppearancePtr& preview() const { return preview_; }

    void setInkColor(const pdf::PdfColor& color) { inkColor_ = color; }
    void setHint(std::string hint) { hint_ = std::move(hint); }

    bool onMouse(const ui::PointerEvent& event) override;
    void paintSelf(ui::PaintContext& context) const override;

private:
    void paintPreview(ui::PaintContext& context) const;

    Mode mode_ = Mode::Capture;
    std::vector<std::vector<pdf::PdfPoint>> strokes_;
    std::size_t points_ = 0;
    bool drawing_ = false;
    pdf::PdfSignatureAppearancePtr preview_;
    pdf::PdfColor inkColor_{0.05F, 0.12F, 0.45F};
    std::string hint_;
    std::function<void()> onChanged_;
};

} // namespace rivet::app
