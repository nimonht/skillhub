#include "gui/window_effects.h"

#import <AppKit/AppKit.h>

#include <QDebug>
#include <QQuickWindow>

// finds the view again when the material changes. NSView's own `tag` is read-only: this is the identifier, which is not.
static NSString *const kEffectId = @"io.github.nimonht.skillhub.effect";

bool window_effects_reduce_transparency_mac() { return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceTransparency]; }

// macOS 26 and later: the class exists, and the window's glass is the system's. Looked up by name so the program builds with an
// older SDK and still starts on older systems.
bool window_effects_native_glass_mac() { return NSClassFromString(@"NSGlassEffectView") != nil; }

// A view behind Qt's own: NSVisualEffectView, or on macOS 26 and later NSGlassEffectView. The window takes the theme's appearance,
// so the glass is dark when the program is, whatever the system is: that keeps what shows through the panels within the greys
// that qml/Theme.qml (glassBackdropHi, glassBackdropLo) and tests/contrast.py count on.
void window_effects_apply_mac(QQuickWindow *window, int kind, bool dark) {
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    NSWindow *ns = view.window;
    NSView *frame = view.superview;
    if (!ns || !frame) return;
    for (NSView *old in [[frame.subviews copy] autorelease])   // every one: an earlier call may have left more than one
        if ([old.identifier isEqualToString:kEffectId]) [old removeFromSuperview];
    ns.opaque = kind == 0;
    ns.backgroundColor = kind == 0 ? [NSColor windowBackgroundColor] : [NSColor clearColor];
    ns.appearance = kind == 0 ? nil : [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    if (kind == 0) return;
    Class glass = kind == 2 ? NSClassFromString(@"NSGlassEffectView") : nil;
    NSView *effect;
    if (glass) {
        effect = [[glass alloc] initWithFrame:frame.bounds];
        // style 0 is the regular glass, which adapts to what is behind it. Set only where this system has the property.
        if ([effect respondsToSelector:NSSelectorFromString(@"setStyle:")]) {
            @try {
                [effect setValue:@(0) forKey:@"style"];
            } @catch (NSException *) {
            }
        }
        qInfo("window effect: NSGlassEffectView, %s appearance", dark ? "dark" : "light");
    } else {
        NSVisualEffectView *blur = [[NSVisualEffectView alloc] initWithFrame:frame.bounds];
        blur.material = NSVisualEffectMaterialUnderWindowBackground;
        blur.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        blur.state = NSVisualEffectStateFollowsWindowActiveState;
        effect = blur;
        qInfo("window effect: NSVisualEffectView%s", kind == 2 ? " (no NSGlassEffectView on this system)" : "");
    }
    effect.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    effect.identifier = kEffectId;
    [frame addSubview:effect positioned:NSWindowBelow relativeTo:view];
    [effect release];  // no ARC here, and the window holds it now
}
