// SPDX-License-Identifier: MPL-2.0
// ContentBarController: the properties strip of the content tools follows the
// tool and the selection (fake content backend, real controllers).
#include "RivetTest.h"

#include "ContentTestKit.hpp"

#include "app/ContentBarController.hpp"
#include "ui/Button.hpp"

#include <string>

using namespace rivet;
using app::ContentBarController;
using app::ContentController;
using app::ContentTool;
using core::Point;
using core::Rect;
using test::content::ContentShell;

namespace {

void click(ui::Button& button) {
    ui::PointerEvent event;
    event.button = 1;
    event.position = Point{5.0, 5.0};
    event.type = ui::PointerEventType::Down;
    button.onMouse(event);
    event.type = ui::PointerEventType::Up;
    button.onMouse(event);
}

struct Fixture {
    ContentShell shell;
    ContentBarController bar;
    Fixture() : bar(*shell.context, *shell.root, *shell.content) {
        CHECK(shell.open("bar") != nullptr);
        shell.addImage(1, Rect{100.0, 100.0, 200.0, 100.0});
        shell.addBlock(2, Rect{50.0, 300.0, 200.0, 20.0}, "Hello");
        bar.layout(Rect{0.0, 0.0, 900.0, 36.0});
    }
    bool shown(const std::string& label) {
        ui::Button* button = bar.button(label);
        return button != nullptr && bar.controlShown(*button);
    }
};

} // namespace

RIVET_TEST(contentBarIsHiddenWithoutATool) {
    Fixture f;
    CHECK_NEAR(f.bar.height(), 0.0, 1e-9);
    CHECK(!f.bar.visible());
    CHECK(!f.shown("Sans"));
    const int before = f.shell.relayouts;
    f.shell.content->setTool(ContentTool::SelectObject);
    CHECK_GT(f.shell.relayouts, before); // the shell is asked to re-lay out
    CHECK_NEAR(f.bar.height(), ContentBarController::kHeight, 1e-9);
    CHECK_NEAR(f.bar.height(), 36.0, 1e-9);
    f.shell.content->setTool(ContentTool::AddText);
    CHECK_NEAR(f.bar.height(), 36.0, 1e-9);
    f.shell.content->setTool(ContentTool::None);
    CHECK_NEAR(f.bar.height(), 0.0, 1e-9);
}

RIVET_TEST(contentBarSelectToolWithoutSelectionShowsTheHint) {
    Fixture f;
    f.shell.content->setTool(ContentTool::SelectObject);
    f.bar.layout(Rect{0.0, 0.0, 900.0, 36.0});
    CHECK(!f.shown("Sans"));
    CHECK(!f.shown("Delete"));
    CHECK(f.bar.hintText().find("Click an object") != std::string::npos);
    f.shell.content->setTool(ContentTool::AddText);
    CHECK(f.bar.hintText().find("Click the page") != std::string::npos);
}

RIVET_TEST(contentBarAddTextShowsFontControlsAndSwatchesAndChangesTheDefault) {
    Fixture f;
    f.shell.content->setTool(ContentTool::AddText);
    f.bar.layout(Rect{0.0, 0.0, 900.0, 36.0});
    for (const char* label : {"Sans", "Serif", "Mono", "B", "\xE2\x88\x92", "+"}) {
        CHECK(f.shown(label));
    }
    CHECK(!f.shown("Delete"));
    CHECK(!f.shown("Replace Image\xE2\x80\xA6"));

    CHECK(f.shell.content->displayedStyle().family == ContentController::FontFamily::Sans);
    click(*f.bar.button("Serif"));
    CHECK(f.shell.content->displayedStyle().family == ContentController::FontFamily::Serif);
    click(*f.bar.button("Mono"));
    CHECK(f.shell.content->displayedStyle().family == ContentController::FontFamily::Mono);
    click(*f.bar.button("Sans"));
    CHECK(f.shell.content->displayedStyle().family == ContentController::FontFamily::Sans);
    CHECK(f.bar.button("Sans")->active());
    click(*f.bar.button("+"));
    CHECK_NEAR(f.shell.content->displayedStyle().size, 13.0, 1e-9);
    CHECK_EQ(f.shell.fake->editCalls, 0); // no selection: only the default changes
}

RIVET_TEST(contentBarImageSelectionShowsReplaceDeleteAndPixelSize) {
    Fixture f;
    f.shell.content->setTool(ContentTool::SelectObject);
    f.shell.click(Point{150.0, 150.0});
    f.bar.layout(Rect{0.0, 0.0, 900.0, 36.0});
    CHECK(f.shown("Replace Image\xE2\x80\xA6"));
    CHECK(f.shown("Delete"));
    CHECK(f.shown("640 \xC3\x97 480 px"));
    CHECK(!f.shown("Sans"));
    CHECK(!f.shown("Edit Text"));
}

RIVET_TEST(contentBarTextSelectionShowsStyleAndEditControls) {
    Fixture f;
    f.shell.content->setTool(ContentTool::SelectObject);
    f.shell.click(Point{100.0, 310.0});
    f.bar.layout(Rect{0.0, 0.0, 900.0, 36.0});
    CHECK(f.shown("Sans"));
    CHECK(f.shown("Edit Text"));
    CHECK(f.shown("Delete"));
    CHECK(!f.shown("Front"));             // not a block Rivet wrote
    CHECK(!f.shown("Replace Image\xE2\x80\xA6"));
    f.shell.page().blocks.front().tag = 3;
    f.shell.content->setTool(ContentTool::SelectObject);
    f.shell.click(Point{100.0, 310.0});
    f.bar.layout(Rect{0.0, 0.0, 900.0, 36.0});
    CHECK(f.shown("Front"));
}
