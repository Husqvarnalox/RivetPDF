#pragma once

#include "ui/Widget.hpp"

#import <AppKit/AppKit.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

@class RivetContentView;

namespace rivet::platform {

// C++ host object owned by RivetContentView; bridges widget-tree invalidation
// to AppKit repaints. requestRedraw() is thread-safe: render completion
// callbacks fire it from worker threads.
class ContentViewHost final : public rivet::ui::IRedrawSink {
public:
    // The view owns the host, so the weak back pointer stays valid for the
    // host's whole lifetime.
    explicit ContentViewHost(RivetContentView* __weak view);
    ~ContentViewHost();

    void requestRedraw() override;

private:
    RivetContentView* __weak view_;
};

} // namespace rivet::platform

// Flipped (top-left origin, y-down) content view hosting the Rivet widget
// tree. The view owns the AppKit <-> widget plumbing: painting via
// MacosPaintContext, event translation, and the thread-safe redraw sink. The
// widget tree itself stays owned by the app shell.
@interface RivetContentView : NSView

// Non-owning pointer to the shell's root widget. Installing it also
// propagates the redraw sink through the subtree and applies the current
// view bounds as the root frame (which runs the shell layout).
- (void)setRootWidget:(rivet::ui::Widget*)widget;

// The redraw sink to hand to rivet::platform::ShellServices.
- (rivet::ui::IRedrawSink*)redrawSink;

// Receives every keyDown translated to a ui::KeyEvent (keyDown is always on
// the main thread). Returning NO passes the original event to NSView's
// default handling.
- (void)setKeyHandler:(std::function<bool(const rivet::ui::KeyEvent&)>)handler;

// Files dropped on the view (Finder drag and drop): called on the main thread
// with the dropped file paths. Dropping is only accepted while a handler is
// installed.
- (void)setFileDropHandler:(std::function<void(const std::vector<std::filesystem::path>&)>)handler;

@end
