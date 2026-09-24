#import "RivetContentView.h"

#include "MacosPaintContext.h"
#include "core/geometry/Rect.hpp"

#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

namespace {

rivet::ui::ModifierFlags makeModifiers(NSEventModifierFlags flags) {
    rivet::ui::ModifierFlags result;
    result.shift = (flags & NSEventModifierFlagShift) != 0;
    result.control = (flags & NSEventModifierFlagControl) != 0;
    result.option = (flags & NSEventModifierFlagOption) != 0;
    result.command = (flags & NSEventModifierFlagCommand) != 0;
    return result;
}

// Physical keycodes -> ui::Key. Keycode mapping is USB-HID derived and stable
// across layouts for these keys.
rivet::ui::KeyEvent makeKeyEvent(NSEvent* event) {
    rivet::ui::KeyEvent result;
    result.modifiers = makeModifiers(event.modifierFlags);
    switch (event.keyCode) {
    case 123: result.key = rivet::ui::Key::Left; break;
    case 124: result.key = rivet::ui::Key::Right; break;
    case 125: result.key = rivet::ui::Key::Down; break;
    case 126: result.key = rivet::ui::Key::Up; break;
    case 116: result.key = rivet::ui::Key::PageUp; break;
    case 121: result.key = rivet::ui::Key::PageDown; break;
    case 115: result.key = rivet::ui::Key::Home; break;
    case 119: result.key = rivet::ui::Key::End; break;
    case 53: result.key = rivet::ui::Key::Escape; break;
    case 36: // Return
    case 76: // Keypad Enter
        result.key = rivet::ui::Key::Enter;
        break;
    case 48: result.key = rivet::ui::Key::Tab; break;
    case 51: result.key = rivet::ui::Key::Backspace; break;
    case 117: result.key = rivet::ui::Key::Delete; break;
    case 49: result.key = rivet::ui::Key::Space; break;
    default: {
        NSString* characters = event.characters;
        if (characters.length == 0) break; // stays Key::Unknown
        const std::string text = characters.UTF8String;
        // '=' is the unshifted '+' on US layouts; both zoom in.
        if (text == "=" || text == "+") {
            result.key = rivet::ui::Key::Plus;
        } else if (text == "-") {
            result.key = rivet::ui::Key::Minus;
        } else {
            result.key = rivet::ui::Key::Character;
            result.text = text;
        }
        break;
    }
    }
    return result;
}

} // namespace

namespace rivet::platform {

ContentViewHost::ContentViewHost(RivetContentView* __weak view) : view_(view) {}

ContentViewHost::~ContentViewHost() = default;

void ContentViewHost::requestRedraw() {
    if (view_ == nil) return;
    // Called from arbitrary threads (render callbacks). Hop to main before
    // touching the view; setNeedsDisplay coalesces into a single frame.
    dispatch_async(dispatch_get_main_queue(), ^ {
        [view_ setNeedsDisplay:YES];
    });
}

} // namespace rivet::platform

@interface RivetContentView ()
- (void)applyRootFrame;
- (void)refreshTrackingArea;
- (rivet::core::Point)localPointForEvent:(NSEvent*)event;
- (void)sendPointer:(rivet::ui::PointerEventType)type button:(int)button event:(NSEvent*)event;
@end

@implementation RivetContentView {
    std::unique_ptr<rivet::platform::ContentViewHost> host_;
    rivet::ui::Widget* rootWidget_;
    std::function<bool(const rivet::ui::KeyEvent&)> keyHandler_;
    NSTrackingArea* trackingArea_;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self != nil) {
        host_ = std::make_unique<rivet::platform::ContentViewHost>(self);
        [self refreshTrackingArea];
    }
    return self;
}

- (void)dealloc {
    // C++ ivars release through the implicit ObjC++ destructor; ARC handles
    // the object ivars.
}

#pragma mark View configuration

- (BOOL)isFlipped {
    // Rivet's logical space is top-left origin / y-down; a flipped view makes
    // the CGContext coordinates match 1:1.
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)refreshTrackingArea {
    if (trackingArea_ != nil) [self removeTrackingArea:trackingArea_];
    trackingArea_ = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:trackingArea_];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    [self refreshTrackingArea];
}

#pragma mark Shell wiring

- (void)setRootWidget:(rivet::ui::Widget*)widget {
    rootWidget_ = widget;
    if (rootWidget_ != nullptr) {
        // The tree was built before the sink existed; propagate it now.
        // Widgets added later inherit it from the root via addChild().
        rootWidget_->setRedrawSink(host_.get());
        [self applyRootFrame];
    }
    [self setNeedsDisplay:YES];
}

- (rivet::ui::IRedrawSink*)redrawSink {
    return host_.get();
}

- (void)setKeyHandler:(std::function<bool(const rivet::ui::KeyEvent&)>)handler {
    keyHandler_ = std::move(handler);
}

- (void)applyRootFrame {
    if (rootWidget_ == nullptr) return;
    const NSRect bounds = self.bounds;
    // View coordinates are root-local: the root frame is {0, 0, w, h}.
    rootWidget_->setFrame(rivet::core::Rect{
        rivet::core::Point{}, rivet::core::Size{bounds.size.width, bounds.size.height}});
}

#pragma mark Painting

- (void)drawRect:(NSRect)dirtyRect {
    if (rootWidget_ == nullptr) return;
    NSGraphicsContext* graphicsContext = [NSGraphicsContext currentContext];
    if (graphicsContext == nil) return;
    CGContextRef context = graphicsContext.CGContext;
    if (context == nullptr) return;
    const double scale = self.window != nil ? self.window.backingScaleFactor : 1.0;

    CGContextSaveGState(context);
    {
        rivet::platform::MacosPaintContext paintContext(context, scale);
        rootWidget_->paint(paintContext);
    }
    CGContextRestoreGState(context);
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    // Root layout runs inside setFrame(); repaint everything after a resize.
    [self applyRootFrame];
    [self setNeedsDisplay:YES];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self setNeedsDisplay:YES];
}

#pragma mark Pointer events

- (rivet::core::Point)localPointForEvent:(NSEvent*)event {
    // locationInWindow is nil-fromView = window coordinates.
    const NSPoint local = [self convertPoint:event.locationInWindow fromView:nil];
    return rivet::core::Point{local.x, local.y};
}

- (void)sendPointer:(rivet::ui::PointerEventType)type button:(int)button event:(NSEvent*)event {
    if (rootWidget_ == nullptr) return;
    rivet::ui::PointerEvent pointerEvent;
    pointerEvent.type = type;
    pointerEvent.position = [self localPointForEvent:event];
    pointerEvent.button = button;
    pointerEvent.modifiers = makeModifiers(event.modifierFlags);
    // View coordinates == root-local coordinates (root frame is {0,0,w,h}).
    rootWidget_->onMouse(pointerEvent);
}

- (void)mouseDown:(NSEvent*)event {
    [self sendPointer:rivet::ui::PointerEventType::Down button:1 event:event];
}

- (void)rightMouseDown:(NSEvent*)event {
    [self sendPointer:rivet::ui::PointerEventType::Down button:2 event:event];
}

- (void)mouseUp:(NSEvent*)event {
    [self sendPointer:rivet::ui::PointerEventType::Up button:1 event:event];
}

- (void)rightMouseUp:(NSEvent*)event {
    [self sendPointer:rivet::ui::PointerEventType::Up button:2 event:event];
}

- (void)mouseMoved:(NSEvent*)event {
    [self sendPointer:rivet::ui::PointerEventType::Move button:0 event:event];
}

- (void)mouseDragged:(NSEvent*)event {
    [self sendPointer:rivet::ui::PointerEventType::Move button:1 event:event];
}

- (void)rightMouseDragged:(NSEvent*)event {
    [self sendPointer:rivet::ui::PointerEventType::Move button:2 event:event];
}

- (void)otherMouseDragged:(NSEvent*)event {
    [self sendPointer:rivet::ui::PointerEventType::Move button:0 event:event];
}

- (void)scrollWheel:(NSEvent*)event {
    if (rootWidget_ == nullptr) return;

    double dx = 0.0;
    double dy = 0.0;
    if (event.hasPreciseScrollingDeltas) {
        dx = event.scrollingDeltaX;
        dy = event.scrollingDeltaY;
    } else {
        dx = event.deltaX * 10.0;
        dy = event.deltaY * 10.0;
    }

    // Delta mapping: AppKit's positive deltaY means "scroll up" (the view
    // moves toward the document top, i.e. the content visually moves DOWN;
    // e.g. a natural-scrolling two-finger flick up reports deltaY < 0 and
    // moves the content up). Rivet's scrollDelta convention is
    // "positive = content moves up", hence the y negation. The x axis is NOT
    // negated: positive AppKit deltaX already corresponds to content moving
    // left in Rivet's terms. Horizontal direction could not be exercised
    // headless; revisit against a physical trackpad if it feels inverted.
    rivet::ui::PointerEvent pointerEvent;
    pointerEvent.type = rivet::ui::PointerEventType::Scroll;
    pointerEvent.position = [self localPointForEvent:event];
    pointerEvent.scrollDelta = rivet::core::Point{dx, -dy};
    pointerEvent.modifiers = makeModifiers(event.modifierFlags);
    rootWidget_->onMouse(pointerEvent);
}

#pragma mark Keyboard events

- (void)keyDown:(NSEvent*)event {
    const rivet::ui::KeyEvent keyEvent = makeKeyEvent(event);
    bool handled = false;
    if (keyHandler_ != nullptr) handled = keyHandler_(keyEvent);
    // Unhandled keys keep AppKit's default behavior (navigation, beeps, ...).
    if (!handled) [super keyDown:event];
}

@end
