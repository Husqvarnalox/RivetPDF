// SPDX-License-Identifier: MPL-2.0
#include "ui/Icons.hpp"

#include "ui/Theme.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <utility>

namespace rivet::ui {
namespace {

using P = core::Point;

// Small builder over the 20x20 grid.
class Builder {
public:
    std::vector<IconShape> shapes;

    Builder& line(double x1, double y1, double x2, double y2) {
        Path p;
        p.moveTo({x1, y1});
        p.lineTo({x2, y2});
        return stroke(std::move(p));
    }
    Builder& poly(std::initializer_list<P> points, bool closed = false) {
        Path p;
        bool first = true;
        for (const P& point : points) {
            if (first) {
                p.moveTo(point);
                first = false;
            } else {
                p.lineTo(point);
            }
        }
        if (closed) p.close();
        return stroke(std::move(p));
    }
    Builder& rrect(double x, double y, double w, double h, double r, bool filled = false) {
        Path p;
        p.addRoundedRect(core::Rect{x, y, w, h}, r);
        return filled ? fill(std::move(p)) : stroke(std::move(p));
    }
    Builder& ellipse(double x, double y, double w, double h, bool filled = false) {
        Path p;
        p.addEllipse(core::Rect{x, y, w, h});
        return filled ? fill(std::move(p)) : stroke(std::move(p));
    }
    Builder& circle(double cx, double cy, double r, bool filled = false) {
        return ellipse(cx - r, cy - r, 2.0 * r, 2.0 * r, filled);
    }
    // Arc around (cx, cy) from a0 to a1 degrees (0 = +x, 90 = +y: clockwise
    // on screen), split into <= 90 degree cubic pieces.
    Builder& arc(double cx, double cy, double r, double a0, double a1) {
        Path p;
        const double total = a1 - a0;
        const int pieces = std::max(1, static_cast<int>(std::ceil(std::fabs(total) / 90.0)));
        const double step = total / pieces * std::numbers::pi / 180.0;
        double a = a0 * std::numbers::pi / 180.0;
        p.moveTo({cx + r * std::cos(a), cy + r * std::sin(a)});
        const double k = 4.0 / 3.0 * std::tan(step / 4.0);
        for (int i = 0; i < pieces; ++i) {
            const double b = a + step;
            const P start{cx + r * std::cos(a), cy + r * std::sin(a)};
            const P end{cx + r * std::cos(b), cy + r * std::sin(b)};
            const P c1{start.x - k * r * std::sin(a), start.y + k * r * std::cos(a)};
            const P c2{end.x + k * r * std::sin(b), end.y - k * r * std::cos(b)};
            p.cubicTo(c1, c2, end);
            a = b;
        }
        return stroke(std::move(p));
    }
    Builder& stroke(Path p) {
        shapes.push_back(IconShape{std::move(p), false});
        return *this;
    }
    Builder& fill(Path p) {
        shapes.push_back(IconShape{std::move(p), true});
        return *this;
    }
};

// Mirrors every point horizontally on the grid (redo from undo, ...).
std::vector<IconShape> mirrored(std::vector<IconShape> shapes) {
    for (IconShape& shape : shapes) {
        for (PathSegment& s : shape.path.segments) {
            s.p.x = 20.0 - s.p.x;
            s.c1.x = 20.0 - s.c1.x;
            s.c2.x = 20.0 - s.c2.x;
        }
    }
    return shapes;
}

std::vector<IconShape> undoShapes() {
    Builder b;
    Path p;
    p.moveTo({5.5, 8.0});
    p.lineTo({12.0, 8.0});
    p.cubicTo({14.8, 8.0}, {16.5, 9.8}, {16.5, 12.0});
    p.cubicTo({16.5, 14.2}, {14.8, 16.0}, {12.0, 16.0});
    p.lineTo({8.5, 16.0});
    b.stroke(std::move(p));
    b.poly({{8.5, 4.5}, {5.0, 8.0}, {8.5, 11.5}});
    return b.shapes;
}

std::vector<IconShape> rotateLeftShapes() {
    Builder b;
    b.arc(10.5, 11.0, 6.0, 180.0, 450.0);
    b.poly({{2.4, 8.6}, {4.5, 11.3}, {6.8, 8.8}});
    return b.shapes;
}

} // namespace

std::vector<IconShape> iconShapes(Icon icon) {
    Builder b;
    switch (icon) {
    case Icon::Open:
        b.poly({{2.5, 15.5}, {2.5, 4.5}, {7.5, 4.5}, {9.0, 6.5}, {17.5, 6.5}, {17.5, 15.5}}, true);
        b.line(2.5, 9.0, 17.5, 9.0);
        break;
    case Icon::Save:
        b.poly({{3.0, 12.0}, {3.0, 16.5}, {17.0, 16.5}, {17.0, 12.0}});
        b.line(10.0, 3.0, 10.0, 12.0);
        b.poly({{6.5, 8.5}, {10.0, 12.0}, {13.5, 8.5}});
        break;
    case Icon::Print:
        b.poly({{6.0, 7.5}, {6.0, 3.0}, {14.0, 3.0}, {14.0, 7.5}});
        b.poly({{6.0, 14.0}, {3.5, 14.0}, {2.5, 13.0}, {2.5, 8.5}, {3.5, 7.5}, {16.5, 7.5}, {17.5, 8.5},
                {17.5, 13.0}, {16.5, 14.0}, {14.0, 14.0}});
        b.rrect(6.0, 11.5, 8.0, 5.5, 0.5);
        b.circle(14.5, 10.0, 0.8, true);
        break;
    case Icon::Sidebar:
        b.rrect(2.5, 4.0, 15.0, 12.0, 2.0);
        b.line(7.5, 4.0, 7.5, 16.0);
        break;
    case Icon::ZoomIn:
        b.circle(8.5, 8.5, 5.5);
        b.line(12.6, 12.6, 17.0, 17.0);
        b.line(6.0, 8.5, 11.0, 8.5);
        b.line(8.5, 6.0, 8.5, 11.0);
        break;
    case Icon::ZoomOut:
        b.circle(8.5, 8.5, 5.5);
        b.line(12.6, 12.6, 17.0, 17.0);
        b.line(6.0, 8.5, 11.0, 8.5);
        break;
    case Icon::FitWidth:
        b.rrect(2.5, 4.5, 15.0, 11.0, 1.5);
        b.line(5.5, 10.0, 14.5, 10.0);
        b.poly({{7.5, 8.0}, {5.5, 10.0}, {7.5, 12.0}});
        b.poly({{12.5, 8.0}, {14.5, 10.0}, {12.5, 12.0}});
        break;
    case Icon::FitPage:
        b.rrect(4.5, 2.5, 11.0, 15.0, 1.5);
        b.line(10.0, 5.5, 10.0, 14.5);
        b.poly({{8.0, 7.5}, {10.0, 5.5}, {12.0, 7.5}});
        b.poly({{8.0, 12.5}, {10.0, 14.5}, {12.0, 12.5}});
        break;
    case Icon::ActualSize:
        b.rrect(2.5, 4.5, 15.0, 11.0, 1.5);
        b.poly({{5.8, 8.6}, {7.0, 7.8}, {7.0, 12.5}});
        b.poly({{12.8, 8.6}, {14.0, 7.8}, {14.0, 12.5}});
        b.circle(10.5, 8.8, 0.8, true);
        b.circle(10.5, 11.6, 0.8, true);
        break;
    case Icon::Search:
        b.circle(8.5, 8.5, 5.5);
        b.line(12.6, 12.6, 17.0, 17.0);
        break;
    case Icon::Previous:
    case Icon::ChevronUp: b.poly({{5.5, 12.5}, {10.0, 8.0}, {14.5, 12.5}}); break;
    case Icon::Next:
    case Icon::ChevronDown: b.poly({{5.5, 8.0}, {10.0, 12.5}, {14.5, 8.0}}); break;
    case Icon::ChevronLeft: b.poly({{12.0, 5.5}, {7.5, 10.0}, {12.0, 14.5}}); break;
    case Icon::ChevronRight: b.poly({{8.0, 5.5}, {12.5, 10.0}, {8.0, 14.5}}); break;
    case Icon::Undo: return undoShapes();
    case Icon::Redo: return mirrored(undoShapes());
    case Icon::RotateLeft: return rotateLeftShapes();
    case Icon::RotateRight: return mirrored(rotateLeftShapes());
    case Icon::Select:
        b.poly({{5.0, 3.0}, {5.0, 15.5}, {8.4, 12.4}, {10.8, 17.2}, {12.8, 16.2}, {10.5, 11.5}, {15.0, 11.2}}, true);
        break;
    case Icon::Hand:
        b.line(7.0, 10.0, 7.0, 4.8);
        b.line(9.6, 9.5, 9.6, 3.5);
        b.line(12.2, 9.5, 12.2, 4.5);
        b.line(14.8, 10.5, 14.8, 6.5);
        {
            Path p;
            p.moveTo({14.8, 10.5});
            p.lineTo({14.8, 12.5});
            p.cubicTo({14.8, 15.5}, {12.8, 17.5}, {10.0, 17.5});
            p.cubicTo({7.8, 17.5}, {6.6, 16.6}, {5.4, 14.8});
            p.lineTo({3.2, 11.4});
            p.cubicTo({2.6, 10.4}, {3.8, 9.4}, {4.8, 10.2});
            p.lineTo({7.0, 12.0});
            b.stroke(std::move(p));
        }
        break;
    case Icon::Highlight:
        b.poly({{11.0, 3.5}, {16.5, 9.0}, {10.0, 15.5}, {4.5, 10.0}}, true);
        b.poly({{4.5, 10.0}, {3.0, 14.5}, {5.5, 17.0}, {10.0, 15.5}});
        b.line(12.5, 17.0, 17.0, 17.0);
        break;
    case Icon::Underline: {
        Path p;
        p.moveTo({6.0, 3.5});
        p.lineTo({6.0, 10.0});
        p.cubicTo({6.0, 12.8}, {7.8, 14.5}, {10.0, 14.5});
        p.cubicTo({12.2, 14.5}, {14.0, 12.8}, {14.0, 10.0});
        p.lineTo({14.0, 3.5});
        b.stroke(std::move(p));
        b.line(5.0, 17.0, 15.0, 17.0);
        break;
    }
    case Icon::StrikeOut: {
        Path p;
        p.moveTo({13.8, 5.6});
        p.cubicTo({13.0, 4.2}, {11.7, 3.5}, {10.0, 3.5});
        p.cubicTo({7.6, 3.5}, {6.1, 4.8}, {6.1, 6.6});
        p.cubicTo({6.1, 8.3}, {7.4, 9.2}, {10.0, 9.8});
        p.moveTo({12.6, 11.2});
        p.cubicTo({13.5, 11.8}, {14.0, 12.6}, {14.0, 13.5});
        p.cubicTo({14.0, 15.4}, {12.3, 16.5}, {10.0, 16.5});
        p.cubicTo({8.0, 16.5}, {6.6, 15.8}, {5.8, 14.3});
        b.stroke(std::move(p));
        b.line(3.5, 10.0, 16.5, 10.0);
        break;
    }
    case Icon::Note:
        b.poly({{3.5, 3.5}, {16.5, 3.5}, {16.5, 12.5}, {12.5, 16.5}, {3.5, 16.5}}, true);
        b.poly({{16.5, 12.5}, {12.5, 12.5}, {12.5, 16.5}});
        b.line(6.5, 7.5, 13.5, 7.5);
        b.line(6.5, 10.5, 10.5, 10.5);
        break;
    case Icon::Ink: {
        Path p;
        p.moveTo({3.0, 13.5});
        p.cubicTo({4.8, 6.0}, {7.2, 6.0}, {8.2, 10.0});
        p.cubicTo({9.2, 14.0}, {11.2, 14.0}, {12.6, 9.0});
        p.cubicTo({13.6, 5.6}, {15.6, 5.6}, {17.0, 8.0});
        b.stroke(std::move(p));
        break;
    }
    case Icon::Rectangle: b.rrect(3.0, 5.0, 14.0, 10.0, 1.0); break;
    case Icon::Ellipse: b.ellipse(2.5, 4.5, 15.0, 11.0); break;
    case Icon::Line: b.line(4.0, 16.0, 16.0, 4.0); break;
    case Icon::Arrow:
        b.line(4.0, 16.0, 16.0, 4.0);
        b.poly({{9.5, 4.0}, {16.0, 4.0}, {16.0, 10.5}});
        break;
    case Icon::Stamp:
        b.rrect(7.5, 2.5, 5.0, 6.5, 2.5);
        b.line(10.0, 9.0, 10.0, 11.0);
        b.rrect(3.5, 11.0, 13.0, 3.5, 1.0);
        b.line(4.5, 17.0, 15.5, 17.0);
        break;
    case Icon::Sign: {
        b.poly({{12.5, 3.0}, {16.5, 7.0}, {9.5, 14.0}, {6.0, 14.5}, {6.5, 11.0}}, true);
        Path p;
        p.moveTo({2.5, 17.5});
        p.cubicTo({4.5, 16.0}, {5.5, 16.0}, {6.5, 17.0});
        p.cubicTo({7.5, 18.0}, {9.0, 17.6}, {10.5, 16.8});
        b.stroke(std::move(p));
        break;
    }
    case Icon::EditContent:
        b.rrect(2.5, 6.5, 9.0, 11.0, 1.5);
        b.poly({{14.0, 2.5}, {17.5, 6.0}, {10.0, 13.5}, {6.5, 13.5}, {6.5, 10.0}}, true);
        break;
    case Icon::AddText:
        b.line(3.5, 4.5, 12.5, 4.5);
        b.line(8.0, 4.5, 8.0, 16.0);
        b.line(12.5, 13.5, 17.5, 13.5);
        b.line(15.0, 11.0, 15.0, 16.0);
        break;
    case Icon::Crop:
        b.poly({{5.5, 2.5}, {5.5, 14.5}, {17.5, 14.5}});
        b.poly({{2.5, 5.5}, {14.5, 5.5}, {14.5, 17.5}});
        break;
    case Icon::Redact:
        b.rrect(2.5, 3.5, 15.0, 13.0, 1.5);
        b.rrect(5.0, 6.5, 10.0, 2.5, 0.5, true);
        b.rrect(5.0, 11.0, 6.5, 2.5, 0.5, true);
        break;
    case Icon::Form:
        b.rrect(2.5, 3.5, 15.0, 13.0, 1.5);
        b.rrect(5.0, 6.5, 3.5, 3.5, 0.8);
        b.line(10.5, 8.25, 15.0, 8.25);
        b.rrect(5.0, 12.0, 10.0, 2.0, 0.5);
        break;
    case Icon::Compare:
        b.rrect(2.5, 2.5, 8.5, 11.5, 1.0);
        b.rrect(9.0, 6.0, 8.5, 11.5, 1.0);
        break;
    case Icon::Ocr:
        b.poly({{2.5, 6.0}, {2.5, 2.5}, {6.0, 2.5}});
        b.poly({{14.0, 2.5}, {17.5, 2.5}, {17.5, 6.0}});
        b.poly({{17.5, 14.0}, {17.5, 17.5}, {14.0, 17.5}});
        b.poly({{6.0, 17.5}, {2.5, 17.5}, {2.5, 14.0}});
        b.line(6.5, 7.5, 13.5, 7.5);
        b.line(6.5, 10.0, 13.5, 10.0);
        b.line(6.5, 12.5, 10.5, 12.5);
        break;
    case Icon::MarkdownRendered:
    case Icon::Document:
        b.poly({{4.5, 2.5}, {11.5, 2.5}, {15.5, 6.5}, {15.5, 17.5}, {4.5, 17.5}}, true);
        b.poly({{11.5, 2.5}, {11.5, 6.5}, {15.5, 6.5}});
        if (icon == Icon::MarkdownRendered) {
            b.line(7.5, 10.0, 12.5, 10.0);
            b.line(7.5, 12.5, 12.5, 12.5);
            b.line(7.5, 15.0, 10.5, 15.0);
        }
        break;
    case Icon::MarkdownSource:
        b.poly({{7.0, 6.0}, {3.0, 10.0}, {7.0, 14.0}});
        b.poly({{13.0, 6.0}, {17.0, 10.0}, {13.0, 14.0}});
        b.line(11.2, 4.5, 8.8, 15.5);
        break;
    case Icon::MarkdownSplit:
        b.rrect(2.5, 4.0, 15.0, 12.0, 1.5);
        b.line(10.0, 4.0, 10.0, 16.0);
        break;
    case Icon::Close:
        b.line(5.5, 5.5, 14.5, 14.5);
        b.line(14.5, 5.5, 5.5, 14.5);
        break;
    case Icon::More:
        b.circle(4.5, 10.0, 1.4, true);
        b.circle(10.0, 10.0, 1.4, true);
        b.circle(15.5, 10.0, 1.4, true);
        break;
    case Icon::View: {
        Path p;
        p.moveTo({2.5, 10.0});
        p.cubicTo({5.0, 5.5}, {15.0, 5.5}, {17.5, 10.0});
        p.cubicTo({15.0, 14.5}, {5.0, 14.5}, {2.5, 10.0});
        p.close();
        b.stroke(std::move(p));
        b.circle(10.0, 10.0, 2.5);
        break;
    }
    case Icon::Bold: {
        Path p;
        p.moveTo({6.0, 3.5});
        p.lineTo({6.0, 16.5});
        p.lineTo({11.6, 16.5});
        p.cubicTo({13.6, 16.5}, {14.8, 15.2}, {14.8, 13.3});
        p.cubicTo({14.8, 11.4}, {13.6, 10.0}, {11.6, 10.0});
        p.lineTo({6.0, 10.0});
        p.moveTo({6.0, 10.0});
        p.lineTo({10.8, 10.0});
        p.cubicTo({12.6, 10.0}, {13.8, 8.8}, {13.8, 6.8});
        p.cubicTo({13.8, 4.8}, {12.6, 3.5}, {10.8, 3.5});
        p.lineTo({6.0, 3.5});
        b.stroke(std::move(p));
        break;
    }
    case Icon::Italic:
        b.line(12.0, 3.5, 8.0, 16.5);
        b.line(9.0, 3.5, 15.0, 3.5);
        b.line(5.0, 16.5, 11.0, 16.5);
        break;
    case Icon::Code:
        b.poly({{7.0, 6.5}, {3.5, 10.0}, {7.0, 13.5}});
        b.poly({{13.0, 6.5}, {16.5, 10.0}, {13.0, 13.5}});
        break;
    case Icon::Link:
        b.rrect(2.5, 7.0, 9.0, 6.0, 3.0);
        b.rrect(8.5, 7.0, 9.0, 6.0, 3.0);
        break;
    case Icon::Heading:
        b.line(5.0, 4.0, 5.0, 16.0);
        b.line(15.0, 4.0, 15.0, 16.0);
        b.line(5.0, 10.0, 15.0, 10.0);
        break;
    case Icon::BulletList:
        b.circle(4.5, 5.5, 1.2, true);
        b.circle(4.5, 10.0, 1.2, true);
        b.circle(4.5, 14.5, 1.2, true);
        b.line(8.0, 5.5, 16.5, 5.5);
        b.line(8.0, 10.0, 16.5, 10.0);
        b.line(8.0, 14.5, 16.5, 14.5);
        break;
    case Icon::NumberedList: {
        b.poly({{3.6, 4.0}, {4.8, 3.2}, {4.8, 7.8}});
        Path p;
        p.moveTo({3.4, 11.0});
        p.cubicTo({3.6, 9.6}, {6.0, 9.5}, {6.0, 11.0});
        p.cubicTo({6.0, 12.2}, {3.4, 13.0}, {3.4, 14.6});
        p.lineTo({6.2, 14.6});
        b.stroke(std::move(p));
        b.line(9.0, 5.5, 16.5, 5.5);
        b.line(9.0, 12.5, 16.5, 12.5);
        break;
    }
    case Icon::TaskList:
        b.rrect(3.0, 3.5, 5.0, 5.0, 1.0);
        b.poly({{4.2, 6.0}, {5.3, 7.1}, {7.2, 4.8}});
        b.rrect(3.0, 11.5, 5.0, 5.0, 1.0);
        b.line(10.5, 6.0, 16.5, 6.0);
        b.line(10.5, 14.0, 16.5, 14.0);
        break;
    case Icon::Quote:
        b.line(4.5, 4.0, 4.5, 16.0);
        b.line(8.5, 6.0, 16.0, 6.0);
        b.line(8.5, 10.0, 16.0, 10.0);
        b.line(8.5, 14.0, 13.0, 14.0);
        break;
    }
    return b.shapes;
}

std::string_view iconName(Icon icon) {
    static constexpr std::string_view kNames[] = {
        "open", "save", "print", "sidebar", "zoom-in", "zoom-out", "fit-width", "fit-page", "actual-size",
        "search", "previous", "next", "undo", "redo", "rotate-left", "rotate-right", "select", "hand",
        "highlight", "underline", "strike-out", "note", "ink", "rectangle", "ellipse", "line", "arrow", "stamp",
        "sign", "edit-content", "add-text", "crop", "redact", "form", "compare", "ocr", "markdown-rendered",
        "markdown-source", "markdown-split", "close", "chevron-left", "chevron-right", "chevron-up",
        "chevron-down", "more", "view", "bold", "italic", "code", "link", "heading", "bullet-list",
        "numbered-list", "task-list", "quote", "document",
    };
    static_assert(std::size(kNames) == kIconCount);
    return kNames[static_cast<std::size_t>(icon)];
}

void drawIcon(PaintContext& context, Icon icon, const core::Rect& box, const Color& color) {
    const double side = std::min(box.size.width, box.size.height);
    if (side <= 0.0) return;
    const double scale = side / 20.0;
    const double ox = box.minX() + (box.size.width - side) / 2.0;
    const double oy = box.minY() + (box.size.height - side) / 2.0;
    const auto map = [&](const P& p) { return P{ox + p.x * scale, oy + p.y * scale}; };
    for (IconShape& shape : iconShapes(icon)) {
        for (PathSegment& s : shape.path.segments) {
            s.p = map(s.p);
            s.c1 = map(s.c1);
            s.c2 = map(s.c2);
        }
        if (shape.fill) {
            context.fillPath(shape.path, color);
        } else {
            context.strokePath(shape.path, color, metrics::kIconStroke * scale, LineCap::Round, LineJoin::Round);
        }
    }
}

} // namespace rivet::ui
