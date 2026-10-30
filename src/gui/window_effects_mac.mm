#include "gui/window_effects.h"

#import <AppKit/AppKit.h>

#include <QQuickWindow>

static const NSInteger kEffectTag = 0x534b48;  // finds the view again when the material changes

bool window_effects_reduce_transparency_mac() { return [[NSWorkspace sharedWorkspace] accessibilityDisplayShouldReduceTransparency]; }

// A view behind Qt's own: NSVisualEffectView, or on macOS 26 and later NSGlassEffectView, looked up by name so the program
// still starts on older systems.
void window_effects_apply_mac(QQuickWindow *window, int kind) {
    NSView *view = reinterpret_cast<NSView *>(window->winId());
    NSWindow *ns = view.window;
    NSView *frame = view.superview;
    if (!ns || !frame) return;
    [[frame viewWithTag:kEffectTag] removeFromSuperview];
    ns.opaque = kind == 0;
    ns.backgroundColor = kind == 0 ? [NSColor windowBackgroundColor] : [NSColor clearColor];
    if (kind == 0) return;
    Class glass = kind == 2 ? NSClassFromString(@"NSGlassEffectView") : nil;
    NSView *effect;
    if (glass) {
        effect = [[glass alloc] initWithFrame:frame.bounds];
    } else {
        NSVisualEffectView *blur = [[NSVisualEffectView alloc] initWithFrame:frame.bounds];
        blur.material = NSVisualEffectMaterialUnderWindowBackground;
        blur.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        blur.state = NSVisualEffectStateFollowsWindowActiveState;
        effect = blur;
    }
    effect.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    @try {
        [effect setValue:@(kEffectTag) forKey:@"tag"];
    } @catch (NSException *) {
    }
    [frame addSubview:effect positioned:NSWindowBelow relativeTo:view];
}
