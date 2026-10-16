// SPDX-License-Identifier: MPL-2.0
#import "MacosAppearance.h"

#import <AppKit/AppKit.h>

#include <utility>

namespace rivet::platform {

struct MacosAppearance::State {
    core::IMainThreadDispatcher* dispatcher = nullptr;
    std::function<void()> callback;
};

} // namespace rivet::platform

// Forwards KVO / notifications to the C++ state on the main thread.
@interface RivetAppearanceObserver : NSObject {
@public
    std::weak_ptr<rivet::platform::MacosAppearance::State> state;
}
- (void)notifyChange;
@end

@implementation RivetAppearanceObserver
- (void)notifyChange {
    const std::shared_ptr<rivet::platform::MacosAppearance::State> current = state.lock();
    if (!current || current->dispatcher == nullptr) return;
    // Delivered on the main thread through the dispatcher; the state is
    // re-checked there (the appearance object may be gone by then).
    std::weak_ptr<rivet::platform::MacosAppearance::State> weak = current;
    current->dispatcher->post([weak] {
        const auto alive = weak.lock();
        if (!alive || !alive->callback) return;
        const std::function<void()> callback = alive->callback; // may replace itself
        callback();
    });
}
- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id>*)change
                       context:(void*)context {
    (void)keyPath;
    (void)object;
    (void)change;
    (void)context;
    [self notifyChange];
}
- (void)systemColorsChanged:(NSNotification*)notification {
    (void)notification;
    [self notifyChange];
}
@end

namespace rivet::platform {

MacosAppearance::MacosAppearance(core::IMainThreadDispatcher& dispatcher) : state_(std::make_shared<State>()) {
    state_->dispatcher = &dispatcher;
    // A forced theme also drives the window chrome (title bar, menus).
    if (const std::optional<bool> forced = themeOverrideFromEnvironment(); forced.has_value()) {
        NSApp.appearance = [NSAppearance appearanceNamed:*forced ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    }
    RivetAppearanceObserver* observer = [[RivetAppearanceObserver alloc] init];
    observer->state = state_;
    [NSApp addObserver:observer forKeyPath:@"effectiveAppearance" options:0 context:nullptr];
    [[NSNotificationCenter defaultCenter] addObserver:observer
                                             selector:@selector(systemColorsChanged:)
                                                 name:NSSystemColorsDidChangeNotification
                                               object:nil];
    observer_ = (__bridge_retained void*)observer;
}

MacosAppearance::~MacosAppearance() {
    RivetAppearanceObserver* observer = (__bridge_transfer RivetAppearanceObserver*)observer_;
    observer_ = nullptr;
    [NSApp removeObserver:observer forKeyPath:@"effectiveAppearance"];
    [[NSNotificationCenter defaultCenter] removeObserver:observer];
    state_->callback = nullptr;
}

bool MacosAppearance::isDark() const {
    if (const std::optional<bool> forced = themeOverrideFromEnvironment(); forced.has_value()) return *forced;
    NSAppearance* appearance = NSApp.effectiveAppearance;
    if (appearance == nil) return false;
    NSAppearanceName best =
        [appearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return [best isEqualToString:NSAppearanceNameDarkAqua];
}

std::optional<AccentColor> MacosAppearance::accentColor() const {
    NSColor* accent = [NSColor.controlAccentColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    if (accent == nil) return std::nullopt;
    return AccentColor{accent.redComponent, accent.greenComponent, accent.blueComponent};
}

void MacosAppearance::onChange(std::function<void()> callback) { state_->callback = std::move(callback); }

} // namespace rivet::platform
