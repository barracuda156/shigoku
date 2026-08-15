// mac_menu.mm — AppKit half of the menu seam (see mac_menu.hpp). Compiled
// only on macOS. No SDL here: menu items call a plain function pointer the SDL
// layer installs, so this file knows nothing about how the viewer pumps
// events.
//
// No ARC — manual retain/release, explicit NSAutoreleasePool, and the pre-10.12
// modifier-mask names when the SDK is old enough to only have those.

#import <Cocoa/Cocoa.h>

#include "mac_menu.hpp"

namespace {

shigoku::view::MenuHandler g_handler = nullptr;

void fire(shigoku::view::MenuCommand cmd) {
  if (g_handler != nullptr) g_handler(static_cast<int>(cmd));
}

#if defined(MAC_OS_X_VERSION_10_12) && \
    MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_12
const NSUInteger kCommandMask = NSEventModifierFlagCommand;
const NSUInteger kControlMask = NSEventModifierFlagControl;
#else
const NSUInteger kCommandMask = NSCommandKeyMask;
const NSUInteger kControlMask = NSControlKeyMask;
#endif

}  // namespace

// The target every item we own points at. One process-lifetime instance; it
// holds no state, it just turns selectors into commands.
@interface ShigokuViewMenuTarget: NSObject
- (void)shigokuToggleFullScreen:(id)sender;
- (void)shigokuZoomIn:(id)sender;
- (void)shigokuZoomOut:(id)sender;
- (void)shigokuZoomReset:(id)sender;
- (void)shigokuClose:(id)sender;
@end

@implementation ShigokuViewMenuTarget

- (void)shigokuToggleFullScreen:(id)sender
{
    (void) sender;
    fire(shigoku::view::MenuCommand::Fullscreen);
}

- (void)shigokuZoomIn:(id)sender
{
    (void) sender;
    fire(shigoku::view::MenuCommand::ZoomIn);
}

- (void)shigokuZoomOut:(id)sender
{
    (void) sender;
    fire(shigoku::view::MenuCommand::ZoomOut);
}

- (void)shigokuZoomReset:(id)sender
{
    (void) sender;
    fire(shigoku::view::MenuCommand::ZoomReset);
}

- (void)shigokuClose:(id)sender
{
    (void) sender;
    fire(shigoku::view::MenuCommand::Close);
}

// Our items are always live: the viewer has exactly one window and every
// command applies to it. Without this AppKit would still enable them (the
// target responds to the action), but saying so is cheaper than relying on it.
- (BOOL)validateMenuItem:(NSMenuItem*)item
{
    (void) item;
    return YES;
}

@end

namespace {

// Depth-first search for the first item wired to `action` — SDL puts its
// fullscreen item in the Window submenu, but nothing promises that, so look
// everywhere rather than at a fixed path.
NSMenuItem* findItemWithAction(NSMenu* menu, SEL action)
{
    for (NSInteger i = 0; i < [menu numberOfItems]; ++i)
    {
        NSMenuItem* item = [menu itemAtIndex:i];
        if ([item action] == action)
            return item;
        NSMenu* sub = [item submenu];
        if (sub != nil)
        {
            NSMenuItem* found = findItemWithAction(sub, action);
            if (found != nil)
                return found;
        }
    }
    return nil;
}

// Where a View menu belongs: just before Window, or at the end if there is no
// Window menu.
NSInteger viewMenuIndex(NSMenu* bar)
{
    for (NSInteger i = 0; i < [bar numberOfItems]; ++i)
    {
        NSMenu* sub = [[bar itemAtIndex:i] submenu];
        if (sub != nil && [[sub title] isEqualToString:@"Window"])
            return i;
    }
    return [bar numberOfItems];
}

void addItem(NSMenu* menu, NSString* title, SEL action, NSString* key,
             NSUInteger mask, id target)
{
    NSMenuItem* item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    [item setKeyEquivalentModifierMask:mask];
    [item setTarget:target];
}

}  // namespace

namespace shigoku::view {

void install_menu(MenuHandler cb)
{
    if (cb == nullptr)
        return;

    static BOOL installed = NO;
    if (installed)
        return;

    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    NSApplication* app = NSApp;
    if (app == nil)
    {
        // No NSApplication: either SDL has not initialized video yet or this
        // is a headless run (SDL_VIDEODRIVER=dummy). Nothing to attach to.
        [pool release];
        return;
    }

    g_handler = cb;
    ShigokuViewMenuTarget* target = [[ShigokuViewMenuTarget alloc] init];  // never released

    NSMenu* bar = [app mainMenu];
    if (bar == nil)
    {
        // Nothing built one (SDL only does when the app is unbundled). Ours is
        // then the whole menu bar, and like the target it lives as long as the
        // process — no release, no chance of handing AppKit a dead menu.
        bar = [[NSMenu alloc] init];
        [app setMainMenu:bar];
    }

    // SDL's own item, if it is there: same place, same shortcut, but pointing
    // at a selector that exists on every system version.
    NSMenuItem* sdlItem = findItemWithAction(bar, @selector(toggleFullScreen:));
    if (sdlItem != nil)
    {
        [sdlItem setTarget:target];
        [sdlItem setAction:@selector(shigokuToggleFullScreen:)];
    }

    // File menu — just "Close" at the standard Cmd+W. An unbundled app's
    // default menu (what SDL builds here) carries no File menu and thus no
    // item claims that shortcut at all, so without this Cmd+W is simply dead
    // rather than merely unbound to a visible item.
    NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem* fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"File"
                                                           action:nil
                                                    keyEquivalent:@""];
    [fileMenuItem setSubmenu:fileMenu];
    addItem(fileMenu, @"Close", @selector(shigokuClose:), @"w", kCommandMask, target);
    const NSInteger fileIdx = [bar numberOfItems] > 0 ? 1 : 0;
    [bar insertItem:fileMenuItem atIndex:fileIdx];
    [fileMenu release];
    [fileMenuItem release];

    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem* viewItem = [[NSMenuItem alloc] initWithTitle:@"View"
                                                     action:nil
                                              keyEquivalent:@""];
    [viewItem setSubmenu:viewMenu];

    if (sdlItem == nil)
    {
        // Nothing to repair (a bundled app with its own nib, or a future SDL):
        // carry the fullscreen item ourselves, at the usual Cmd+Ctrl+F.
        addItem(viewMenu, @"Toggle Full Screen", @selector(shigokuToggleFullScreen:),
                @"f", kCommandMask | kControlMask, target);
        [viewMenu addItem:[NSMenuItem separatorItem]];
    }

    // Cmd + / Cmd - / Cmd 0. The "+" equivalent is what AppKit draws as ⌘+;
    // the plain +/-/0 keys reach the same commands through the key table, so
    // the menu is a discovery surface, not the only way in.
    addItem(viewMenu, @"Zoom In", @selector(shigokuZoomIn:), @"+", kCommandMask, target);
    addItem(viewMenu, @"Zoom Out", @selector(shigokuZoomOut:), @"-", kCommandMask, target);
    addItem(viewMenu, @"Actual Size", @selector(shigokuZoomReset:), @"0", kCommandMask, target);

    [bar insertItem:viewItem atIndex:viewMenuIndex(bar)];
    [viewMenu release];
    [viewItem release];

    installed = YES;
    [pool release];
}

}  // namespace shigoku::view
