#pragma once

#include "ui/Widget.hpp"

namespace rivet::ui {

// Generic container: paints children and forwards input/hit-testing through
// the Widget defaults. layout() remains the default no-op, so children keep
// their manually assigned frames.
class Container : public Widget {
public:
    // Fills the background; fully transparent by default paints nothing.
    void paintSelf(PaintContext& context) const override {
        if (backgroundColor_.a > 0.0) context.fillRect(bounds(), backgroundColor_);
    }

    void setBackgroundColor(const Color& color) {
        if (backgroundColor_ == color) return;
        backgroundColor_ = color;
        invalidate();
    }
    const Color& backgroundColor() const { return backgroundColor_; }

private:
    Color backgroundColor_{0.0, 0.0, 0.0, 0.0}; // transparent by default
};

} // namespace rivet::ui
