// macOS platform layer (Apple silicon, macOS 26 and later): an AppKit window and its input, OpenGL
// 4.6 through Mesa's Zink over KosmicKrisp (macos_gl.h), each frame shown in the window's
// CAMetalLayer. Objective-C++ with ARC (-fobjc-arc, CMakeLists.txt).
//
// The game's loop owns the main thread, as on the other platforms: no [NSApp run], pumpEvents()
// takes the queued events itself and hands them to AppKit (sendEvent:). The window's content view
// is backed by a CAMetalLayer; swapBuffers() reads the finished frame back from GL (macos_gl.cpp),
// copies the newest frame that has arrived into a Metal buffer and blits it into the layer's next
// drawable, which is presented at the display's refresh (vsync) or at once.
//
// Input: positions in pixels from the top-left corner of the content view. Letters follow the
// keyboard layout (charactersIgnoringModifiers, as the X11 layer's keysyms), the other keys their
// physical position (virtual key codes, the same on every layout); Option is Alt. Command is the
// system's: Command+Q quits, Command+V pastes in the game's fields (KEY_LCMD/KEY_RCMD), the other
// Command shortcuts type nothing and reach the game as the key alone. Text comes from the input context (interpretKeyEvents: dead keys, input methods).
#ifdef __APPLE__
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "platform.h"
#include "macos_gl.h"
#include "../core/log.h"
#include "../net/net_sys.h"

#include <crt_externs.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if !__has_feature(objc_arc)
#error "platform_cocoa.mm is compiled with ARC (-fobjc-arc)"
#endif

@interface ScacelithWindow : NSWindow
@end
@interface ScacelithView : NSView <NSTextInputClient>
@end
@interface ScacelithWindowDelegate : NSObject <NSWindowDelegate>
@end
@interface ScacelithAppDelegate : NSObject <NSApplicationDelegate>
@end

namespace plat {
namespace {

constexpr int kStagingBuffers = 3;            // Metal frames on their way at most
constexpr int64_t kStagingWaitNs = 250000000;  // 250 ms: a frame skipped if the GPU is stuck
constexpr double kPreciseScrollScale = 0.1;    // trackpad scrolling (points) to wheel notches
constexpr NSWindowStyleMask kWindowedStyle =
    NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

ScacelithWindow* g_window;
ScacelithView* g_view;
ScacelithWindowDelegate* g_windowDelegate;   // NSWindow and NSApplication keep their delegates weakly
ScacelithAppDelegate* g_appDelegate;
CAMetalLayer* g_layer;
id<MTLDevice> g_device;
id<MTLCommandQueue> g_queue;
id<MTLBuffer> g_staging[kStagingBuffers];
int g_stagingIndex;
dispatch_semaphore_t g_inflight;
CGColorSpaceRef g_colorSpace;   // the layer's (sRGB), kept for the process's life

bool g_appStarted;
int g_pixelW, g_pixelH;       // the content view in pixels
DisplayMode g_mode = DisplayMode::Windowed;   // the mode and windowed size last asked for
int g_windowedW, g_windowedH;
bool g_quit, g_captured, g_cursorVisible = true;
bool g_cursorHidden, g_dissociated;   // [NSCursor hide] and CGAssociateMouseAndMouseCursorPosition in effect
Input g_input;
timespec g_t0;
int g_keyOfCode[128];         // the key each key code went down as, released by its key up

NSString* ns(const char* s) {
    NSString* r = s ? [NSString stringWithUTF8String:s] : nil;
    return r ? r : @"";
}

std::string utf8(NSString* s) {
    const char* c = s ? s.UTF8String : nullptr;
    return c ? std::string(c) : std::string();
}

CGFloat backingScale() {
    if (g_window) return g_window.backingScaleFactor;
    NSScreen* screen = NSScreen.mainScreen;
    return screen ? screen.backingScaleFactor : 1.0;
}

// ---- Keys -----------------------------------------------------------------------------------------
void setKey(int k, bool down) {
    if (k <= 0 || k >= KEY_COUNT) return;
    if (down) g_input.keyPressed[k] = true;
    else if (g_input.keyDown[k]) g_input.keyReleased[k] = true;
    g_input.keyDown[k] = down;
}
void setButton(int b, bool down) {
    if (down && !g_input.mouseDown[b]) g_input.mousePressed[b] = true;
    if (!down && g_input.mouseDown[b]) g_input.mouseReleased[b] = true;
    g_input.mouseDown[b] = down;
}

// The virtual key codes (Carbon's kVK_ constants, HIToolbox/Events.h) of the keys that keep their
// meaning on every layout.
struct CodeKey {
    unsigned short code;
    int key;
};
const CodeKey kCodeKeys[] = {
    {0x1D, '0'}, {0x12, '1'}, {0x13, '2'}, {0x14, '3'}, {0x15, '4'}, {0x17, '5'}, {0x16, '6'}, {0x1A, '7'}, {0x1C, '8'},
    {0x19, '9'}, {0x31, KEY_SPACE}, {0x35, KEY_ESCAPE}, {0x24, KEY_ENTER}, {0x4C, KEY_ENTER},   // Return, keypad Enter
    {0x30, KEY_TAB}, {0x33, KEY_BACKSPACE}, {0x75, KEY_DELETE},                                   // Delete, forward delete
    {0x7B, KEY_LEFT}, {0x7C, KEY_RIGHT}, {0x7E, KEY_UP}, {0x7D, KEY_DOWN}, {0x73, KEY_HOME}, {0x77, KEY_END},
    {0x74, KEY_PAGEUP}, {0x79, KEY_PAGEDOWN},
    {0x38, KEY_LSHIFT}, {0x3C, KEY_RSHIFT}, {0x3B, KEY_LCTRL}, {0x3E, KEY_RCTRL}, {0x3A, KEY_LALT}, {0x3D, KEY_RALT},
    {0x37, KEY_LCMD}, {0x36, KEY_RCMD},
    {0x7A, KEY_F1}, {0x78, KEY_F2}, {0x63, KEY_F3}, {0x76, KEY_F4}, {0x60, KEY_F5}, {0x61, KEY_F6}, {0x62, KEY_F7},
    {0x64, KEY_F8}, {0x65, KEY_F9}, {0x6D, KEY_F10}, {0x67, KEY_F11}, {0x6F, KEY_F12},
};
// The letter keys' codes, by their place on an ANSI keyboard: the letters of a layout that has no
// Latin letters (Cyrillic, Greek...), as Windows' virtual keys give them.
const unsigned short kLetterCodes[26] = {0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
                                         0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06};

int mapKey(NSEvent* e) {
    const unsigned short code = e.keyCode;
    for (const CodeKey& k : kCodeKeys)
        if (k.code == code) return k.key;
    NSString* chars = e.charactersIgnoringModifiers;
    if (chars.length != 1) return KEY_UNKNOWN;
    const unichar c = [chars characterAtIndex:0];
    if (c >= 'a' && c <= 'z') return 'A' + (c - 'a');
    if (c >= 'A' && c <= 'Z') return c;
    if (c >= 0x80 && [[NSCharacterSet letterCharacterSet] characterIsMember:c])
        for (int i = 0; i < 26; ++i)
            if (kLetterCodes[i] == code) return 'A' + i;
    return KEY_UNKNOWN;
}

void keyEvent(NSEvent* e, bool down) {
    const unsigned short code = e.keyCode;
    int k;
    if (down) {
        k = mapKey(e);
        if (code < 128) g_keyOfCode[code] = k;
    } else {
        // The key it went down as: a layout or Shift changed in between does not leave it held.
        k = code < 128 && g_keyOfCode[code] > 0 ? g_keyOfCode[code] : mapKey(e);
        if (code < 128) g_keyOfCode[code] = 0;
    }
    setKey(k, down);
}

// Shift, Control and Option: a key of the pair went down or up. The device-dependent bits of the
// flags (IOKit's NX_DEVICE*KEYMASK) tell the left key from the right one.
void modifierEvent(NSEvent* e) {
    struct Modifier {
        unsigned short code;
        int key;
        NSEventModifierFlags flag;
        NSUInteger side, pair;
    };
    static const Modifier kModifiers[] = {
        {0x38, KEY_LSHIFT, NSEventModifierFlagShift, 0x0002, 0x0006}, {0x3C, KEY_RSHIFT, NSEventModifierFlagShift, 0x0004, 0x0006},
        {0x3B, KEY_LCTRL, NSEventModifierFlagControl, 0x0001, 0x2001}, {0x3E, KEY_RCTRL, NSEventModifierFlagControl, 0x2000, 0x2001},
        {0x3A, KEY_LALT, NSEventModifierFlagOption, 0x0020, 0x0060}, {0x3D, KEY_RALT, NSEventModifierFlagOption, 0x0040, 0x0060},
        {0x37, KEY_LCMD, NSEventModifierFlagCommand, 0x0008, 0x0018}, {0x36, KEY_RCMD, NSEventModifierFlagCommand, 0x0010, 0x0018},
    };
    const NSUInteger flags = NSUInteger(e.modifierFlags);
    for (const Modifier& m : kModifiers) {
        if (m.code != e.keyCode) continue;
        bool down;
        if (!(flags & m.flag)) down = false;
        else if (flags & m.pair) down = (flags & m.side) != 0;
        else down = !g_input.keyDown[m.key];   // no side bits (a remote keyboard): each event toggles
        setKey(m.key, down);
    }
}

// Typed text, UTF-16 to UTF-32, without control characters nor the private-use codes of AppKit's
// function keys.
void addText(NSString* s) {
    const NSUInteger n = s.length;
    const int capacity = int(sizeof(g_input.text) / sizeof(g_input.text[0]));
    for (NSUInteger i = 0; i < n; ++i) {
        uint32_t c = [s characterAtIndex:i];
        if (c >= 0xD800 && c < 0xDC00) {
            if (i + 1 >= n) break;
            const uint32_t lo = [s characterAtIndex:i + 1];
            if (lo < 0xDC00 || lo >= 0xE000) continue;
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
            ++i;
        } else if (c >= 0xDC00 && c < 0xE000) {
            continue;
        }
        if (c < 32 || (c >= 127 && c < 0xA0) || (c >= 0xF700 && c <= 0xF8FF)) continue;
        if (g_input.textCount < capacity) g_input.text[g_input.textCount++] = c;
    }
}

// ---- Pointer --------------------------------------------------------------------------------------
// The pointer's motion and (unless captured) its position, in pixels from the top-left corner.
void mouseEvent(NSEvent* e, bool dragging) {
    if (!g_view) return;
    const CGFloat scale = backingScale();
    const NSPoint p = [g_view convertPoint:e.locationInWindow fromView:nil];
    const NSRect b = g_view.bounds;
    const bool inside = p.x >= 0 && p.y >= 0 && p.x < b.size.width && p.y < b.size.height;
    // Not clipped at the screen's edges, and still there when the cursor is held in place (captured).
    g_input.mouseDX += float(e.deltaX * scale);
    g_input.mouseDY += float(e.deltaY * scale);
    if (g_captured) return;
    if (inside || dragging) {
        g_input.mouseX = float(p.x * scale);
        g_input.mouseY = float((b.size.height - p.y) * scale);
    }
    g_input.mouseInWindow = inside;
}

// The system cursor: hidden while captured, or over the window when the game asked; held in place
// while captured. Both undone while the window is not the key window (another application's turn).
void updateCursor() {
    const bool focus = g_window && g_window.isKeyWindow;
    const bool hide = focus && (g_captured || (!g_cursorVisible && g_input.mouseInWindow));
    if (hide != g_cursorHidden) {
        if (hide) [NSCursor hide];
        else [NSCursor unhide];
        g_cursorHidden = hide;
    }
    const bool dissociate = focus && g_captured;
    if (dissociate != g_dissociated) {
        CGAssociateMouseAndMouseCursorPosition(dissociate ? false : true);
        g_dissociated = dissociate;
    }
}

void focusChanged(bool focus) {
    if (!focus) {
        // Keys and buttons released in another application never come back as up events.
        for (int k = 0; k < KEY_COUNT; ++k) setKey(k, false);
        for (int b = 0; b < MOUSE_BUTTON_COUNT; ++b) setButton(b, false);
        std::fill(std::begin(g_keyOfCode), std::end(g_keyOfCode), 0);
    }
    updateCursor();
}

// ---- Window ---------------------------------------------------------------------------------------
void updateSize() {
    if (!g_view) return;
    const NSRect px = [g_view convertRectToBacking:g_view.bounds];
    g_pixelW = std::max(0, int(std::lround(px.size.width)));
    g_pixelH = std::max(0, int(std::lround(px.size.height)));
    const CGFloat scale = backingScale();
    if (g_layer && g_layer.contentsScale != scale) g_layer.contentsScale = scale;
}

// Drawn and shown: not in the Dock, the application not hidden.
bool windowShown() { return g_window && !g_window.isMiniaturized && !NSApp.isHidden; }

void applyDisplayMode(DisplayMode mode, int w, int h) {
    NSScreen* screen = g_window.screen ? g_window.screen : NSScreen.mainScreen;
    if (!screen) return;
    if (mode == DisplayMode::Borderless) {
        // The whole screen, the Dock and the menu bar hidden while the game is the active application.
        g_window.styleMask = NSWindowStyleMaskBorderless;
        [g_window setFrame:screen.frame display:YES];
        NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
    } else {
        NSApp.presentationOptions = NSApplicationPresentationDefault;
        g_window.styleMask = kWindowedStyle;
        // The size in pixels on this screen, within its visible part.
        const CGFloat scale = screen.backingScaleFactor > 0 ? screen.backingScaleFactor : 1.0;
        const NSRect visible = screen.visibleFrame;
        NSRect frame = [g_window frameRectForContentRect:NSMakeRect(0, 0, w / scale, h / scale)];
        frame.size.width = std::min(frame.size.width, visible.size.width);
        frame.size.height = std::min(frame.size.height, visible.size.height);
        frame.origin.x = visible.origin.x + (visible.size.width - frame.size.width) / 2;
        frame.origin.y = visible.origin.y + (visible.size.height - frame.size.height) / 2;
        [g_window setFrame:frame display:YES];
    }
    [g_window makeFirstResponder:g_view];
    updateSize();
}

void addMenuItem(NSMenu* menu, NSString* title, SEL action, NSString* key) {
    [menu addItem:[[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key]];
}

// The application: a regular one (Dock icon, menu bar), its menu (Command+Q is its only shortcut)
// and its delegate. Once, before the window or a message box.
void startApplication(const char* title) {
    if (g_appStarted) return;
    g_appStarted = true;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    g_appDelegate = [[ScacelithAppDelegate alloc] init];
    NSApp.delegate = g_appDelegate;
    [NSWindow setAllowsAutomaticWindowTabbing:NO];

    NSString* name = ns(title);
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:name];
    addMenuItem(appMenu, [@"About " stringByAppendingString:name], @selector(orderFrontStandardAboutPanel:), @"");
    [appMenu addItem:[NSMenuItem separatorItem]];
    addMenuItem(appMenu, [@"Hide " stringByAppendingString:name], @selector(hide:), @"");
    [appMenu addItem:[NSMenuItem separatorItem]];
    addMenuItem(appMenu, [@"Quit " stringByAppendingString:name], @selector(terminate:), @"q");
    appItem.submenu = appMenu;
    [bar addItem:appItem];
    NSMenuItem* windowItem = [[NSMenuItem alloc] init];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    addMenuItem(windowMenu, @"Minimize", @selector(performMiniaturize:), @"");
    addMenuItem(windowMenu, @"Zoom", @selector(performZoom:), @"");
    [windowMenu addItem:[NSMenuItem separatorItem]];
    addMenuItem(windowMenu, @"Bring All to Front", @selector(arrangeInFront:), @"");
    windowItem.submenu = windowMenu;
    [bar addItem:windowItem];
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = windowMenu;
    [NSApp finishLaunching];
}

// ---- Present --------------------------------------------------------------------------------------
// The frame read back from GL into the layer: a staging buffer (shared memory), then a blit into
// the next drawable, presented.
void present(const macgl::Frame& f) {
    if (dispatch_semaphore_wait(g_inflight, dispatch_time(DISPATCH_TIME_NOW, kStagingWaitNs)) != 0) {
        macgl::releaseFrame();
        return;
    }
    const NSUInteger bytes = NSUInteger(f.stride) * NSUInteger(f.height);
    id<MTLBuffer> buffer = g_staging[g_stagingIndex];
    if (!buffer || buffer.length < bytes) {
        // Written by the CPU only: write-combined.
        buffer = [g_device newBufferWithLength:bytes options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        g_staging[g_stagingIndex] = buffer;
    }
    if (!buffer) {
        macgl::releaseFrame();
        dispatch_semaphore_signal(g_inflight);
        return;
    }
    std::memcpy([buffer contents], f.pixels, bytes);
    macgl::releaseFrame();
    g_stagingIndex = (g_stagingIndex + 1) % kStagingBuffers;

    const CGSize size = CGSizeMake(f.width, f.height);
    if (!CGSizeEqualToSize(g_layer.drawableSize, size)) g_layer.drawableSize = size;
    id<CAMetalDrawable> drawable = [g_layer nextDrawable];
    id<MTLCommandBuffer> commands = drawable ? [g_queue commandBuffer] : nil;
    if (!commands) {
        dispatch_semaphore_signal(g_inflight);
        return;
    }
    id<MTLTexture> target = drawable.texture;
    const NSUInteger w = std::min<NSUInteger>(NSUInteger(f.width), target.width);
    const NSUInteger h = std::min<NSUInteger>(NSUInteger(f.height), target.height);
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:buffer
               sourceOffset:0
          sourceBytesPerRow:NSUInteger(f.stride)
        sourceBytesPerImage:bytes
                 sourceSize:MTLSizeMake(w, h, 1)
                  toTexture:target
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    [commands presentDrawable:drawable];
    dispatch_semaphore_t inflight = g_inflight;
    [commands addCompletedHandler:^(id<MTLCommandBuffer>) {
      dispatch_semaphore_signal(inflight);
    }];
    [commands commit];
}

}  // namespace
}  // namespace plat

// ---- AppKit classes ---------------------------------------------------------------------------------
@implementation ScacelithWindow
// A borderless window (the Borderless display mode) takes the keyboard too.
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
@end

@implementation ScacelithView {
    NSMutableAttributedString* _marked;   // the input method's text being composed
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _marked = [[NSMutableAttributedString alloc] init];
        self.wantsLayer = YES;
        // The layer's contents are the presented drawables only.
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
        const NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveAlways |
                                              NSTrackingInVisibleRect | NSTrackingEnabledDuringMouseDrag;
        [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect options:options owner:self userInfo:nil]];
    }
    return self;
}

- (CALayer*)makeBackingLayer {
    return plat::g_layer;
}
- (BOOL)wantsUpdateLayer {
    return YES;
}
- (void)updateLayer {
}
- (BOOL)isOpaque {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    plat::updateSize();
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    plat::updateSize();
}

// ---- Keyboard
- (void)keyDown:(NSEvent*)event {
    const bool shortcut = (event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0;
    const bool composing = _marked.length > 0;
    // Shortcuts type nothing (Control+V or Command+V pastes in the game's fields).
    if (!shortcut) [self interpretKeyEvents:@[ event ]];
    // A key the input method took for its composition (a dead key, a candidate list's arrows and
    // Return) is not the game's.
    if (!composing && _marked.length == 0) plat::keyEvent(event, true);
}
- (void)keyUp:(NSEvent*)event {
    plat::keyEvent(event, false);
}
- (void)flagsChanged:(NSEvent*)event {
    plat::modifierEvent(event);
}

// ---- NSTextInputClient
- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    NSString* text = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)string string] : (NSString*)string;
    plat::addText(text);
    [_marked deleteCharactersInRange:NSMakeRange(0, _marked.length)];
}
- (void)doCommandBySelector:(SEL)selector {
    // The keys (Return, arrows, Escape...) reach the game as keys: no beep, no action here.
}
- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    if ([string isKindOfClass:[NSAttributedString class]])
        _marked = [[NSMutableAttributedString alloc] initWithAttributedString:(NSAttributedString*)string];
    else
        _marked = [[NSMutableAttributedString alloc] initWithString:(NSString*)string];
}
- (void)unmarkText {
    [_marked deleteCharactersInRange:NSMakeRange(0, _marked.length)];
}
- (NSRange)selectedRange {
    return NSMakeRange(NSNotFound, 0);
}
- (NSRange)markedRange {
    return _marked.length > 0 ? NSMakeRange(0, _marked.length) : NSMakeRange(NSNotFound, 0);
}
- (BOOL)hasMarkedText {
    return _marked.length > 0;
}
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    return nil;
}
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    // The input method's window goes by the bottom-left corner of the view (the game draws no caret
    // the system knows of).
    const NSRect inWindow = [self convertRect:NSMakeRect(0, 0, 0, 0) toView:nil];
    return self.window ? [self.window convertRectToScreen:inWindow] : inWindow;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    return 0;
}

// ---- Pointer
- (void)mouseDown:(NSEvent*)event {
    plat::setButton(plat::MOUSE_LEFT, true);
    plat::mouseEvent(event, false);
}
- (void)mouseUp:(NSEvent*)event {
    plat::setButton(plat::MOUSE_LEFT, false);
    plat::mouseEvent(event, true);
}
- (void)rightMouseDown:(NSEvent*)event {
    plat::setButton(plat::MOUSE_RIGHT, true);
    plat::mouseEvent(event, false);
}
- (void)rightMouseUp:(NSEvent*)event {
    plat::setButton(plat::MOUSE_RIGHT, false);
    plat::mouseEvent(event, true);
}
- (void)otherMouseDown:(NSEvent*)event {
    if (event.buttonNumber == 2) plat::setButton(plat::MOUSE_MIDDLE, true);
    plat::mouseEvent(event, false);
}
- (void)otherMouseUp:(NSEvent*)event {
    if (event.buttonNumber == 2) plat::setButton(plat::MOUSE_MIDDLE, false);
    plat::mouseEvent(event, true);
}
- (void)mouseMoved:(NSEvent*)event {
    plat::mouseEvent(event, false);
}
- (void)mouseDragged:(NSEvent*)event {
    plat::mouseEvent(event, true);
}
- (void)rightMouseDragged:(NSEvent*)event {
    plat::mouseEvent(event, true);
}
- (void)otherMouseDragged:(NSEvent*)event {
    plat::mouseEvent(event, true);
}
- (void)scrollWheel:(NSEvent*)event {
    // Away from the user is +, as the system's scrolling direction setting has it.
    double dy = event.scrollingDeltaY;
    if (event.hasPreciseScrollingDeltas) dy *= plat::kPreciseScrollScale;
    plat::g_input.wheel += float(dy);
}
- (void)mouseEntered:(NSEvent*)event {
    plat::g_input.mouseInWindow = true;
    plat::updateCursor();
}
- (void)mouseExited:(NSEvent*)event {
    plat::g_input.mouseInWindow = false;
    plat::updateCursor();
}
@end

@implementation ScacelithWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    plat::g_quit = true;   // the game closes it itself (shutdown) once it has saved
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
    plat::focusChanged(true);
}
- (void)windowDidResignKey:(NSNotification*)notification {
    plat::focusChanged(false);
}
- (void)windowDidResize:(NSNotification*)notification {
    plat::updateSize();
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
    plat::updateSize();
}
- (void)windowDidChangeScreen:(NSNotification*)notification {
    plat::updateSize();
}
@end

@implementation ScacelithAppDelegate
// Command+Q, the Dock's Quit, a logout: the game's loop ends as with the window's close button.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    plat::g_quit = true;
    return NSTerminateCancel;
}
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app {
    return YES;
}
@end

namespace plat {

bool init(const WindowDesc& desc) {
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    @autoreleasepool {
        startApplication(desc.title);
        g_device = MTLCreateSystemDefaultDevice();
        if (!g_device) {
            LOGE("macOS: no Metal device");
            return false;
        }
        g_queue = [g_device newCommandQueue];
        g_inflight = dispatch_semaphore_create(kStagingBuffers);
        g_layer = [CAMetalLayer layer];
        g_layer.device = g_device;
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.framebufferOnly = NO;   // the blit's destination
        g_layer.opaque = YES;
        g_layer.maximumDrawableCount = 3;
        g_layer.displaySyncEnabled = desc.vsync ? YES : NO;
        // The frames are sRGB-encoded 8-bit values, as on the other platforms' framebuffers.
        if (!g_colorSpace) g_colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        g_layer.colorspace = g_colorSpace;

        g_mode = desc.mode;
        g_windowedW = desc.width;
        g_windowedH = desc.height;
        NSScreen* screen = NSScreen.mainScreen;
        const CGFloat scale = screen && screen.backingScaleFactor > 0 ? screen.backingScaleFactor : 1.0;
        const NSRect content = NSMakeRect(0, 0, std::max(1, desc.width) / scale, std::max(1, desc.height) / scale);
        g_window = [[ScacelithWindow alloc] initWithContentRect:content
                                                      styleMask:kWindowedStyle
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
        g_window.releasedWhenClosed = NO;
        g_window.restorable = NO;
        g_window.title = ns(desc.title);
        // Full screen is the game's own (the Borderless mode): the green button zooms.
        g_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
        g_windowDelegate = [[ScacelithWindowDelegate alloc] init];
        g_window.delegate = g_windowDelegate;
        g_view = [[ScacelithView alloc] initWithFrame:content];
        g_window.contentView = g_view;
        applyDisplayMode(desc.mode, desc.width, desc.height);
        updateSize();
        g_layer.contentsScale = backingScale();

        std::string why;
        if (!macgl::create(g_pixelW, g_pixelH, desc.debugContext, why)) {
            LOGE("cannot create a GL 4.6 core context: %s (Mesa's Zink over KosmicKrisp, see the log above)", why.c_str());
            NSApp.presentationOptions = NSApplicationPresentationDefault;
            return false;
        }
        LOGI("macOS: window %dx%d pixels (scale %.2f), %s", g_pixelW, g_pixelH, double(backingScale()),
             desc.mode == DisplayMode::Borderless ? "borderless" : "windowed");
        if (!desc.hidden) {
            [g_window makeKeyAndOrderFront:nil];
            [NSApp activate];
        }
    }
    return true;
}

void shutdown() {
    @autoreleasepool {
        if (!g_window) return;
        g_captured = false;
        g_cursorVisible = true;
        if (g_cursorHidden) [NSCursor unhide];
        if (g_dissociated) CGAssociateMouseAndMouseCursorPosition(true);
        g_cursorHidden = g_dissociated = false;
        NSApp.presentationOptions = NSApplicationPresentationDefault;
        macgl::destroy();
        // The frames still on their way: a queue runs its command buffers in order.
        if (id<MTLCommandBuffer> last = [g_queue commandBuffer]) {
            [last commit];
            [last waitUntilCompleted];
        }
        [g_window orderOut:nil];
        g_window.delegate = nil;
        g_window = nil;
        g_view = nil;
        for (int i = 0; i < kStagingBuffers; ++i) g_staging[i] = nil;
        g_layer = nil;
        g_queue = nil;
        g_device = nil;
    }
}

bool pumpEvents() {
    for (int k = 0; k < KEY_COUNT; ++k) g_input.keyPressed[k] = g_input.keyReleased[k] = false;
    for (int b = 0; b < MOUSE_BUTTON_COUNT; ++b) g_input.mousePressed[b] = g_input.mouseReleased[b] = false;
    g_input.mouseDX = g_input.mouseDY = 0;
    g_input.wheel = 0;
    g_input.textCount = 0;
    @autoreleasepool {
        for (;;) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (!e) break;
            [NSApp sendEvent:e];
            // AppKit keeps a key's up event to itself while Command is held: the key would stay down.
            if (e.type == NSEventTypeKeyUp && (e.modifierFlags & NSEventModifierFlagCommand) && g_window && g_window.isKeyWindow)
                [g_window sendEvent:e];
        }
        // FB 0 follows the window's size in pixels before the frame is drawn.
        if (windowShown() && g_pixelW > 0 && g_pixelH > 0 &&
            (g_pixelW != macgl::surfaceWidth() || g_pixelH != macgl::surfaceHeight()))
            macgl::resize(g_pixelW, g_pixelH);
        updateCursor();
    }
    return !g_quit;
}

void swapBuffers() {
    @autoreleasepool {
        if (!g_window) return;
        // Covered by other windows, or not shown: nothing to read back; the GPU is waited for so
        // that the loop does not queue frames faster than it draws them.
        if (!g_window.isVisible || !(g_window.occlusionState & NSWindowOcclusionStateVisible)) {
            macgl::finish();
            sleepMs(16);
            return;
        }
        macgl::endFrame();
        macgl::Frame frame;
        if (macgl::takeFrame(frame)) present(frame);
    }
}

void setVsync(bool on) {
    if (g_layer) g_layer.displaySyncEnabled = on ? YES : NO;
}

void setDisplayMode(DisplayMode mode, int w, int h) {
    // Unchanged (Options applied for another setting): the window keeps its size, as on Windows.
    if (mode == g_mode && (mode == DisplayMode::Borderless || (w == g_windowedW && h == g_windowedH))) return;
    g_mode = mode;
    if (mode == DisplayMode::Windowed) {
        g_windowedW = w;
        g_windowedH = h;
    }
    @autoreleasepool {
        if (g_window) applyDisplayMode(mode, g_windowedW, g_windowedH);
    }
}

int width() { return windowShown() ? macgl::surfaceWidth() : 0; }
int height() { return windowShown() ? macgl::surfaceHeight() : 0; }
bool hasFocus() { return g_window && g_window.isKeyWindow; }

double time() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec - g_t0.tv_sec) + double(t.tv_nsec - g_t0.tv_nsec) * 1e-9;
}
void sleepMs(int ms) { usleep(useconds_t(ms) * 1000); }

const Input& input() { return g_input; }
void setCursorVisible(bool visible) {
    g_cursorVisible = visible;
    updateCursor();
}
void setMouseCaptured(bool captured) {
    g_captured = captured;
    updateCursor();
}

// The core library's folders (net::sys): ~/Library/Application Support/scacelith/ for both.
std::string exeDirectory() { return net::sys::exeDirectory(); }
std::string userDataDirectory() { return net::sys::userDataDirectory(); }
std::string appDataDirectory() { return net::sys::appDataDirectory(); }

void messageBox(const char* title, const char* text, bool) {
    // Right-to-left text needs nothing more: the alert lays it out by its characters' direction.
    LOGE("%s: %s", title, text);
    @autoreleasepool {
        startApplication(title);
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = ns(title);
        alert.informativeText = ns(text);
        [alert addButtonWithTitle:@"OK"];
        [NSApp activate];
        [alert runModal];
    }
}

uint64_t randomSeed() { return (uint64_t(arc4random()) << 32) | uint64_t(arc4random()); }

std::string clipboardText() {
    @autoreleasepool {
        return utf8([[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString]);
    }
}

std::string systemLanguage() {
    @autoreleasepool {
        // The first of the user's preferred languages ("fr-FR", "zh-Hant-TW"), as Windows' UI language.
        return utf8(NSLocale.preferredLanguages.firstObject);
    }
}

std::vector<std::string> commandLine() {
    std::vector<std::string> args;
    const int argc = *_NSGetArgc();
    char** argv = *_NSGetArgv();
    for (int i = 1; i < argc; ++i) {
        // The process serial number that older versions of macOS passed to an application opened in the Finder.
        if (!argv[i] || std::strncmp(argv[i], "-psn_", 5) == 0) continue;
        args.push_back(argv[i]);
    }
    return args;
}

// ---- Saved games ---------------------------------------------------------------------------------
bool openInFileManager(const std::string& path) {
    if (path.empty()) return false;
    // A relative path starting with '-' would read as an option.
    const std::string arg = path[0] == '-' ? "./" + path : path;
    char* argv[] = {const_cast<char*>("/usr/bin/open"), const_cast<char*>(arg.c_str()), nullptr};
    const pid_t pid = net::sys::spawnProgram(argv);
    if (pid < 0) {
        LOGW("could not start open for %s", path.c_str());
        return false;
    }
    // Reaped on a thread of its own, so the menu never waits and no zombie stays behind.
    std::thread([pid]() {
        int status = 0;
        waitpid(pid, &status, 0);
    }).detach();
    return true;
}

}  // namespace plat
#endif
