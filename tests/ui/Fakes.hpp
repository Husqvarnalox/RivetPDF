#pragma once

// Test doubles shared by the UI tests: a recording PaintContext, a
// scriptable IRenderSource, and a counting redraw sink.

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"
#include "render/RenderSource.hpp"
#include "render/TileKey.hpp"
#include "ui/PaintContext.hpp"
#include "ui/Widget.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rivet::ui::testing {

// Recording PaintContext: captures every primitive with the exact arguments
// the widget passed (widget-local logical coordinates). backingScale is
// configurable; measureText approximates 7 px per character with height =
// font size, so preferredSize math stays deterministic.
class FakePaintContext final : public PaintContext {
public:
    struct FillRecord {
        core::Rect rect;
        Color color;
    };
    struct RoundedFillRecord {
        core::Rect rect;
        Color color;
        double cornerRadius = 0.0;
    };
    struct StrokeRecord {
        core::Rect rect;
        Color color;
        double strokeWidth = 0.0;
    };
    struct LineRecord {
        core::Point from;
        core::Point to;
        Color color;
        double strokeWidth = 0.0;
    };
    struct BitmapRecord {
        const core::Bitmap* bitmap = nullptr;
        core::Rect dest;
    };
    struct TextRecord {
        std::string text;
        core::Rect rect;
        Font font;
        Color color;
        TextAlign align = TextAlign::Left;
    };

    double backingScaleValue = 1.0;
    std::vector<FillRecord> fills;
    std::vector<RoundedFillRecord> roundedFills;
    std::vector<StrokeRecord> strokes;
    std::vector<LineRecord> lines;
    std::vector<BitmapRecord> bitmaps;
    std::vector<TextRecord> texts;
    int clipDepth = 0;

    double backingScale() const override { return backingScaleValue; }

    void pushClip(const core::Rect& /*logicalRect*/) override { ++clipDepth; }
    void popClip() override { --clipDepth; }

    void fillRect(const core::Rect& rect, const Color& color) override {
        fills.push_back(FillRecord{rect, color});
    }

    void fillRoundedRect(const core::Rect& rect, const Color& color, double cornerRadius) override {
        roundedFills.push_back(RoundedFillRecord{rect, color, cornerRadius});
    }

    void strokeRect(const core::Rect& rect, const Color& color, double strokeWidth) override {
        strokes.push_back(StrokeRecord{rect, color, strokeWidth});
    }

    void drawLine(core::Point from, core::Point to, const Color& color, double strokeWidth) override {
        lines.push_back(LineRecord{from, to, color, strokeWidth});
    }

    void drawBitmap(const core::Bitmap& bitmap, const core::Rect& destLogicalRect) override {
        bitmaps.push_back(BitmapRecord{&bitmap, destLogicalRect});
    }

    core::Size measureText(std::string_view text, const Font& font) const override {
        return core::Size{static_cast<double>(text.size()) * 7.0, font.size};
    }

    void drawText(std::string_view text, const core::Rect& rect, const Font& font,
                  const Color& color, TextAlign align) override {
        texts.push_back(TextRecord{std::string(text), rect, font, color, align});
    }
};

// Counting IRedrawSink.
class CountingRedrawSink final : public IRedrawSink {
public:
    int count = 0;

    void requestRedraw() override { ++count; }
};

} // namespace rivet::ui::testing

namespace rivet::render::testing {

// Scriptable IRenderSource: records every request (key, params, priority),
// serves cachedTile from an in-memory map, and can complete requests
// immediately with a small bitmap to exercise the invalidate-on-arrival path.
class FakeRenderSource final : public IRenderSource {
public:
    struct RequestRecord {
        RenderRequest request;
        RenderPriority priority = RenderPriority::Visible;
    };

    std::vector<RequestRecord> requests;
    std::unordered_map<TileKey, std::shared_ptr<const core::Bitmap>> tiles;
    bool completeImmediately = false;
    mutable std::uint64_t lastCacheRevision = 0;
    int cancelAllCount = 0;

    void requestRender(const RenderRequest& request, RenderPriority priority,
                       RenderCallback onDone) override {
        requests.push_back(RequestRecord{request, priority});
        if (!completeImmediately || !onDone) return;
        core::Result<core::Bitmap> bitmap = core::Bitmap::create(4, 4);
        if (bitmap.has_value()) {
            onDone(std::make_shared<const core::Bitmap>(std::move(*bitmap)));
        }
    }

    std::shared_ptr<const core::Bitmap> cachedTile(const TileKey& key,
                                                   std::uint64_t revision) const override {
        lastCacheRevision = revision;
        const auto it = tiles.find(key);
        return it == tiles.end() ? nullptr : it->second;
    }

    void cancelAll() override { ++cancelAllCount; }

    void insertTile(const TileKey& key, std::uint32_t width, std::uint32_t height) {
        core::Result<core::Bitmap> bitmap = core::Bitmap::create(width, height);
        if (bitmap.has_value()) {
            tiles.emplace(key, std::make_shared<const core::Bitmap>(std::move(*bitmap)));
        }
    }
};

} // namespace rivet::render::testing
