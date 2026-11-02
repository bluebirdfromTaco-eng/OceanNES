/*
 * Input handling: keyboard, GameController-framework pads and raw HID pads,
 * plus the Configure Controls window. See input.h.
 *
 * Every controller is read through the same set of "virtual inputs":
 *   1-4    D-pad (or hat switch) up / down / left / right
 *   5-8    left stick up / down / left / right
 *   9-12   right stick up / down / left / right
 *   101+   face / shoulder / menu buttons (GameController: named buttons,
 *          raw HID: button n = 100 + n)
 * Bindings map each NES button to one virtual input and are stored per
 * controller model, so a pad remembers its layout whichever player uses it.
 */
#import "input.h"
#import <GameController/GameController.h>
#import <IOKit/hid/IOHIDManager.h>
#import <objc/message.h>
#include "../src/nes.h"

enum { ROW_UP, ROW_DOWN, ROW_LEFT, ROW_RIGHT, ROW_B, ROW_A, ROW_SELECT, ROW_START, ROW_TURBO_B, ROW_TURBO_A };
static NSString *const kRowNames[INPUT_ROWS] = { @"Up", @"Down", @"Left", @"Right", @"B", @"A", @"Select", @"Start", @"Turbo B", @"Turbo A" };
static const uint32_t kRowBits[INPUT_ROWS] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_B, BTN_A, BTN_SELECT, BTN_START, TURBO_B_BIT, TURBO_A_BIT };
static const int kDefaultPadMap[INPUT_ROWS] = { 1, 2, 3, 4, 101, 102, 109, 110, 103, 104 };

/* keyboard: keyCode + display name */
typedef struct { int code; const char *name; } KeyDef;
static const KeyDef kDefaultKeys[2][INPUT_ROWS] = {
    { {126, "↑"}, {125, "↓"}, {123, "←"}, {124, "→"}, {6, "Z"}, {7, "X"}, {56, "Left Shift"}, {36, "Return"}, {0, "A"}, {1, "S"} },
    { {34, "I"}, {40, "K"}, {38, "J"}, {37, "L"}, {5, "G"}, {4, "H"}, {17, "T"}, {16, "Y"}, {-1, ""}, {-1, ""} },
};

static NSString *special_key_name(int code) {
    switch (code) {
    case 36: return @"Return"; case 76: return @"Enter"; case 48: return @"Tab"; case 49: return @"Space";
    case 51: return @"Delete"; case 117: return @"Fwd Delete"; case 53: return @"Esc";
    case 123: return @"←"; case 124: return @"→"; case 125: return @"↓"; case 126: return @"↑";
    case 56: return @"Left Shift"; case 60: return @"Right Shift"; case 59: return @"Left Control"; case 62: return @"Right Control";
    case 58: return @"Left Option"; case 61: return @"Right Option"; case 55: return @"Left Command"; case 54: return @"Right Command";
    case 115: return @"Home"; case 119: return @"End"; case 116: return @"Page Up"; case 121: return @"Page Down";
    case 122: return @"F1"; case 120: return @"F2"; case 99: return @"F3"; case 118: return @"F4"; case 96: return @"F5"; case 97: return @"F6";
    case 98: return @"F7"; case 100: return @"F8"; case 101: return @"F9"; case 109: return @"F10"; case 103: return @"F11"; case 111: return @"F12";
    case 82: return @"Keypad 0"; case 83: return @"Keypad 1"; case 84: return @"Keypad 2"; case 85: return @"Keypad 3"; case 86: return @"Keypad 4";
    case 87: return @"Keypad 5"; case 88: return @"Keypad 6"; case 89: return @"Keypad 7"; case 91: return @"Keypad 8"; case 92: return @"Keypad 9";
    case 65: return @"Keypad ."; case 67: return @"Keypad *"; case 69: return @"Keypad +"; case 75: return @"Keypad /"; case 78: return @"Keypad -";
    default: return nil;
    }
}

/* modifier keys arrive as flagsChanged; these are the device-dependent bits */
static uint64_t modifier_mask(int code) {
    switch (code) {
    case 56: return 0x0002; case 60: return 0x0004;   /* shift L/R */
    case 59: return 0x0001; case 62: return 0x2000;   /* control L/R */
    case 58: return 0x0020; case 61: return 0x0040;   /* option L/R */
    case 55: return 0x0008; case 54: return 0x0010;   /* command L/R */
    default: return 0;
    }
}

static NSString *dir_name(int vid) {
    static NSString *const d[4] = { @"Up", @"Down", @"Left", @"Right" };
    int k = (vid - 1) & 3;
    if (vid <= 4) return [@"D-pad " stringByAppendingString:d[k]];
    if (vid <= 8) return [@"Left Stick " stringByAppendingString:d[k]];
    return [@"Right Stick " stringByAppendingString:d[k]];
}

/* ------------------------------------------------------------ pads */
@interface InputPad ()
@property (nonatomic, readwrite) BOOL isHID;
@end
@implementation InputPad
- (BOOL)active:(int)vid { return NO; }
- (NSString *)nameOfInput:(int)vid { return vid >= 1 && vid <= 12 ? dir_name(vid) : [NSString stringWithFormat:@"Button %d", vid - 100]; }
@end

static BOOL gc_button(id pad, NSString *key) {
    SEL s = NSSelectorFromString(key);
    if (![pad respondsToSelector:s]) return NO;
    GCControllerButtonInput *b = [pad valueForKey:key];
    return b ? b.pressed : NO;
}

@interface GCPad : InputPad
@property (nonatomic) GCController *gc;
@end
@implementation GCPad
- (BOOL)active:(int)vid {
    GCExtendedGamepad *x = self.gc.extendedGamepad;
    if (x) {
        switch (vid) {
        case 1: return x.dpad.up.pressed;
        case 2: return x.dpad.down.pressed;
        case 3: return x.dpad.left.pressed;
        case 4: return x.dpad.right.pressed;
        case 5: return x.leftThumbstick.yAxis.value > 0.5f;
        case 6: return x.leftThumbstick.yAxis.value < -0.5f;
        case 7: return x.leftThumbstick.xAxis.value < -0.5f;
        case 8: return x.leftThumbstick.xAxis.value > 0.5f;
        case 9: return x.rightThumbstick.yAxis.value > 0.5f;
        case 10: return x.rightThumbstick.yAxis.value < -0.5f;
        case 11: return x.rightThumbstick.xAxis.value < -0.5f;
        case 12: return x.rightThumbstick.xAxis.value > 0.5f;
        case 101: return x.buttonA.pressed;
        case 102: return x.buttonB.pressed;
        case 103: return x.buttonX.pressed;
        case 104: return x.buttonY.pressed;
        case 105: return x.leftShoulder.pressed;
        case 106: return x.rightShoulder.pressed;
        case 107: return x.leftTrigger.pressed;
        case 108: return x.rightTrigger.pressed;
        case 109: return gc_button(x, @"buttonOptions");
        case 110: return gc_button(x, @"buttonMenu");
        case 111: return gc_button(x, @"leftThumbstickButton");
        case 112: return gc_button(x, @"rightThumbstickButton");
        case 113: return gc_button(x, @"buttonHome");
        default: return NO;
        }
    }
    GCMicroGamepad *m = self.gc.microGamepad;
    if (m) {
        switch (vid) {
        case 1: return m.dpad.up.pressed;
        case 2: return m.dpad.down.pressed;
        case 3: return m.dpad.left.pressed;
        case 4: return m.dpad.right.pressed;
        case 101: return m.buttonA.pressed;
        case 103: return m.buttonX.pressed;
        default: return NO;
        }
    }
    return NO;
}
- (NSString *)nameOfInput:(int)vid {
    switch (vid) {
    case 101: return @"A / Cross (bottom)";
    case 102: return @"B / Circle (right)";
    case 103: return @"X / Square (left)";
    case 104: return @"Y / Triangle (top)";
    case 105: return @"L1 / LB";
    case 106: return @"R1 / RB";
    case 107: return @"L2 / LT";
    case 108: return @"R2 / RT";
    case 109: return @"Select / View / Share";
    case 110: return @"Start / Menu / Options";
    case 111: return @"L3 (left stick click)";
    case 112: return @"R3 (right stick click)";
    case 113: return @"Home";
    default: return [super nameOfInput:vid];
    }
}
@end

@interface HIDPad : InputPad {
@public
    uint32_t buttons;       /* HID buttons 1..32 */
    uint8_t dpad;           /* explicit D-pad usages: up/down/left/right bits */
    int hat;                /* -1 centred, 0..7 = N, NE, E ... NW */
    float axis[6];          /* X Y Z Rx Ry Rz, -1..1 */
}
@property (nonatomic) IOHIDDeviceRef device;
@end
@implementation HIDPad
- (instancetype)init { if ((self = [super init])) { hat = -1; self.isHID = YES; } return self; }
- (BOOL)hatHas:(int)dir {   /* dir: 0 up 1 down 2 left 3 right */
    if (hat < 0) return NO;
    switch (dir) {
    case 0: return hat == 7 || hat == 0 || hat == 1;
    case 1: return hat >= 3 && hat <= 5;
    case 2: return hat >= 5 && hat <= 7;
    default: return hat >= 1 && hat <= 3;
    }
}
- (BOOL)active:(int)vid {
    if (vid >= 1 && vid <= 4) return [self hatHas:vid - 1] || (dpad >> (vid - 1) & 1);
    if (vid >= 5 && vid <= 8) {
        switch (vid) {
        case 5: return axis[1] < -0.5f;
        case 6: return axis[1] > 0.5f;
        case 7: return axis[0] < -0.5f;
        default: return axis[0] > 0.5f;
        }
    }
    if (vid >= 9 && vid <= 12) {
        /* right stick is Z/Rz on most pads, Rx/Ry on some */
        float rx = fabsf(axis[2]) > fabsf(axis[3]) ? axis[2] : axis[3];
        float ry = fabsf(axis[5]) > fabsf(axis[4]) ? axis[5] : axis[4];
        switch (vid) {
        case 9: return ry < -0.5f;
        case 10: return ry > 0.5f;
        case 11: return rx < -0.5f;
        default: return rx > 0.5f;
        }
    }
    if (vid >= 101 && vid <= 132) return (buttons >> (vid - 101)) & 1;
    return NO;
}
@end

/* ------------------------------------------------------------ manager */
@interface InputManager () <NSWindowDelegate>
- (void)hidMatched:(IOHIDDeviceRef)d;
- (void)hidRemoved:(IOHIDDeviceRef)d;
- (void)hidValue:(IOHIDValueRef)v;
@end

static void hid_match_cb(void *ctx, IOReturn r, void *sender, IOHIDDeviceRef d) { [(__bridge InputManager *)ctx hidMatched:d]; }
static void hid_remove_cb(void *ctx, IOReturn r, void *sender, IOHIDDeviceRef d) { [(__bridge InputManager *)ctx hidRemoved:d]; }
static void hid_value_cb(void *ctx, IOReturn r, void *sender, IOHIDValueRef v) { [(__bridge InputManager *)ctx hidValue:v]; }

static int hid_int_prop(IOHIDDeviceRef d, CFStringRef key) {
    CFTypeRef t = IOHIDDeviceGetProperty(d, key);
    int v = 0;
    if (t && CFGetTypeID(t) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)t, kCFNumberIntType, &v);
    return v;
}

@implementation InputManager {
    IOHIDManagerRef _hid;
    NSMutableArray<InputPad *> *_pads;
    bool _keyDown[256];
    /* config window */
    NSWindow *_win;
    NSSegmentedControl *_playerSeg;
    NSPopUpButton *_devicePopup;
    NSButton *_stickCheck;
    NSTextField *_hint;
    NSButton *_keyBtn[INPUT_ROWS], *_padBtn[INPUT_ROWS];
    int _capRow, _capKind;             /* _capKind: 0 none, 1 key, 2 pad */
    id _capMonitor;
    NSTimer *_capTimer;
    NSMutableSet<NSNumber *> *_capBaseline;
    double _capStart;
}

+ (instancetype)shared {
    static InputManager *m;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ m = [InputManager new]; });
    return m;
}

- (NSArray<InputPad *> *)pads { return _pads; }

- (void)start {
    _pads = [NSMutableArray array];
    NSMutableDictionary *defs = [NSMutableDictionary dictionary];
    for (int p = 0; p < 2; p++) { defs[[NSString stringWithFormat:@"stick.p%d", p + 1]] = @YES; }
    [[NSUserDefaults standardUserDefaults] registerDefaults:defs];

    /* GameController framework: MFi, and (by macOS version) Xbox / PlayStation / Switch pads */
    for (GCController *c in [GCController controllers]) [self gcConnected:c quiet:YES];
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:[NSOperationQueue mainQueue]
                                                  usingBlock:^(NSNotification *n) { [self gcConnected:n.object quiet:NO]; }];
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidDisconnectNotification object:nil queue:[NSOperationQueue mainQueue]
                                                  usingBlock:^(NSNotification *n) { [self gcDisconnected:n.object]; }];

    /* raw HID: every other USB / Bluetooth gamepad or joystick */
    _hid = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    NSArray *match = @[
        @{ @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_GamePad) },
        @{ @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_Joystick) },
        @{ @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_MultiAxisController) },
    ];
    IOHIDManagerSetDeviceMatchingMultiple(_hid, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(_hid, hid_match_cb, (__bridge void *)self);
    IOHIDManagerRegisterDeviceRemovalCallback(_hid, hid_remove_cb, (__bridge void *)self);
    IOHIDManagerRegisterInputValueCallback(_hid, hid_value_cb, (__bridge void *)self);
    IOHIDManagerScheduleWithRunLoop(_hid, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDManagerOpen(_hid, kIOHIDOptionsTypeNone);
}

- (void)say:(NSString *)msg { if (self.notify) self.notify(msg); }
- (void)padsChanged { [self refreshWindow]; }

- (void)gcConnected:(GCController *)c quiet:(BOOL)quiet {
    for (InputPad *p in _pads) if ([p isKindOfClass:[GCPad class]] && ((GCPad *)p).gc == c) return;
    if (!c.extendedGamepad && !c.microGamepad) return;
    GCPad *p = [GCPad new];
    p.gc = c;
    p.name = c.vendorName.length ? c.vendorName : @"Game Controller";
    p.profileKey = c.extendedGamepad ? @"gc" : @"gc-micro";
    [_pads addObject:p];
    if (!quiet) [self say:[NSString stringWithFormat:@"Controller connected: %@", p.name]];
    [self padsChanged];
}
- (void)gcDisconnected:(GCController *)c {
    for (InputPad *p in [_pads copy])
        if ([p isKindOfClass:[GCPad class]] && ((GCPad *)p).gc == c) {
            [_pads removeObject:p];
            [self say:[NSString stringWithFormat:@"Controller disconnected: %@", p.name]];
        }
    [self padsChanged];
}

/* is this HID device already handled by the GameController framework? */
- (BOOL)gcHandles:(IOHIDDeviceRef)d {
    SEL sel = NSSelectorFromString(@"supportsHIDDevice:");
    if ([GCController respondsToSelector:sel])    /* macOS 11+ */
        return ((BOOL (*)(id, SEL, IOHIDDeviceRef))objc_msgSend)([GCController class], sel, d);
    NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
    if (v.majorVersion == 10 && v.minorVersion >= 15) {   /* 10.15 added Xbox One and DualShock 4 */
        int vid = hid_int_prop(d, CFSTR(kIOHIDVendorIDKey)), pid = hid_int_prop(d, CFSTR(kIOHIDProductIDKey));
        if (vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0)) return YES;
        if (vid == 0x045E && (pid == 0x02E0 || pid == 0x02EA || pid == 0x02FD || pid == 0x0B00 || pid == 0x0B05 ||
                              pid == 0x0B12 || pid == 0x0B13 || pid == 0x0B20 || pid == 0x0B22)) return YES;
    }
    return NO;
}

- (void)hidMatched:(IOHIDDeviceRef)d {
    for (InputPad *p in _pads) if (p.isHID && ((HIDPad *)p).device == d) return;
    if ([self gcHandles:d]) return;
    HIDPad *p = [HIDPad new];
    p.device = d;
    CFStringRef prod = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDProductKey));
    p.name = prod && CFGetTypeID(prod) == CFStringGetTypeID() ? (__bridge NSString *)prod : @"USB / Bluetooth Gamepad";
    p.profileKey = [NSString stringWithFormat:@"hid-%04x-%04x", hid_int_prop(d, CFSTR(kIOHIDVendorIDKey)), hid_int_prop(d, CFSTR(kIOHIDProductIDKey))];
    [_pads addObject:p];
    [self say:[NSString stringWithFormat:@"Controller connected: %@", p.name]];
    [self padsChanged];
}
- (void)hidRemoved:(IOHIDDeviceRef)d {
    for (InputPad *p in [_pads copy])
        if (p.isHID && ((HIDPad *)p).device == d) {
            [_pads removeObject:p];
            [self say:[NSString stringWithFormat:@"Controller disconnected: %@", p.name]];
        }
    [self padsChanged];
}
- (void)hidValue:(IOHIDValueRef)v {
    IOHIDElementRef el = IOHIDValueGetElement(v);
    IOHIDDeviceRef d = IOHIDElementGetDevice(el);
    HIDPad *pad = nil;
    for (InputPad *p in _pads) if (p.isHID && ((HIDPad *)p).device == d) { pad = (HIDPad *)p; break; }
    if (!pad) return;
    uint32_t page = IOHIDElementGetUsagePage(el), usage = IOHIDElementGetUsage(el);
    CFIndex iv = IOHIDValueGetIntegerValue(v);
    CFIndex lo = IOHIDElementGetLogicalMin(el), hi = IOHIDElementGetLogicalMax(el);
    if (page == kHIDPage_Button) {
        if (usage >= 1 && usage <= 32) {
            if (iv) pad->buttons |= 1u << (usage - 1); else pad->buttons &= ~(1u << (usage - 1));
        }
    } else if (page == kHIDPage_GenericDesktop) {
        if (usage >= kHIDUsage_GD_X && usage <= kHIDUsage_GD_Rz) {
            if (hi > lo) pad->axis[usage - kHIDUsage_GD_X] = (float)(2.0 * (double)(iv - lo) / (double)(hi - lo) - 1.0);
        } else if (usage == kHIDUsage_GD_Hatswitch) {
            CFIndex range = hi - lo + 1, pos = iv - lo;
            if (iv < lo || iv > hi) pad->hat = -1;
            else if (range == 8) pad->hat = (int)pos;
            else if (range == 4) pad->hat = (int)pos * 2;
            else pad->hat = -1;
        } else if (usage >= kHIDUsage_GD_DPadUp && usage <= kHIDUsage_GD_DPadLeft) {
            static const int bit[4] = { 0, 1, 3, 2 };   /* usages: up, down, right, left */
            int b = bit[usage - kHIDUsage_GD_DPadUp];
            if (iv) pad->dpad |= (uint8_t)(1 << b); else pad->dpad &= (uint8_t)~(1 << b);
        }
    }
}

/* ---------------------------------------------------------------- bindings */
- (NSString *)keyDefaultsName:(int)p { return [NSString stringWithFormat:@"keys.p%d", p + 1]; }

- (NSArray *)keyBindings:(int)p {
    NSArray *a = [[NSUserDefaults standardUserDefaults] arrayForKey:[self keyDefaultsName:p]];
    if (a.count == INPUT_ROWS) return a;
    NSMutableArray *d = [NSMutableArray array];
    for (int r = 0; r < INPUT_ROWS; r++)
        [d addObject:@{ @"c": @(kDefaultKeys[p][r].code), @"n": [NSString stringWithUTF8String:kDefaultKeys[p][r].name] }];
    return d;
}
- (void)setKeyBindings:(NSArray *)a player:(int)p { [[NSUserDefaults standardUserDefaults] setObject:a forKey:[self keyDefaultsName:p]]; }

- (NSArray<NSNumber *> *)padMap:(NSString *)profile {
    NSArray *a = [[NSUserDefaults standardUserDefaults] arrayForKey:[@"padmap." stringByAppendingString:profile]];
    if (a.count == INPUT_ROWS) return a;
    NSMutableArray *d = [NSMutableArray array];
    for (int r = 0; r < INPUT_ROWS; r++) [d addObject:@(kDefaultPadMap[r])];
    return d;
}
- (void)setPadMap:(NSArray *)a profile:(NSString *)profile {
    [[NSUserDefaults standardUserDefaults] setObject:a forKey:[@"padmap." stringByAppendingString:profile]];
}

- (NSString *)deviceSetting:(int)p {
    return [[NSUserDefaults standardUserDefaults] stringForKey:[NSString stringWithFormat:@"pad.p%d", p + 1]] ?: @"";
}
- (BOOL)stickMoves:(int)p { return [[NSUserDefaults standardUserDefaults] boolForKey:[NSString stringWithFormat:@"stick.p%d", p + 1]]; }

/* player -> controller: an explicitly chosen pad (matched by name), otherwise
 * the connected pads are handed out in order to players set to Automatic */
- (InputPad *)padForPlayer:(int)p {
    InputPad *explicitPad[2] = { nil, nil };
    BOOL isAuto[2];
    for (int i = 0; i < 2; i++) {
        NSString *s = [self deviceSetting:i];
        isAuto[i] = s.length == 0;
        if ([s isEqualToString:@"-"]) continue;
        if (!isAuto[i]) {
            for (InputPad *pad in _pads)
                if ([pad.name isEqualToString:s] && (i == 0 || pad != explicitPad[0])) { explicitPad[i] = pad; break; }
        }
    }
    if (!isAuto[p]) return explicitPad[p];
    NSMutableArray *rest = [_pads mutableCopy];
    for (int i = 0; i < 2; i++) if (explicitPad[i]) [rest removeObject:explicitPad[i]];
    int slot = (p == 1 && isAuto[0]) ? 1 : 0;
    return slot < (int)rest.count ? rest[slot] : nil;
}

- (uint32_t)padBitsForPlayer:(int)p {
    InputPad *pad = [self padForPlayer:p];
    if (!pad) return 0;
    NSArray<NSNumber *> *map = [self padMap:pad.profileKey];
    uint32_t bits = 0;
    for (int r = 0; r < INPUT_ROWS; r++) if ([pad active:map[r].intValue]) bits |= kRowBits[r];
    if ([self stickMoves:p]) {
        if ([pad active:5]) bits |= BTN_UP;
        if ([pad active:6]) bits |= BTN_DOWN;
        if ([pad active:7]) bits |= BTN_LEFT;
        if ([pad active:8]) bits |= BTN_RIGHT;
    }
    return bits;
}

- (uint32_t)keyBitsForPlayer:(int)p {
    NSArray *a = [self keyBindings:p];
    uint32_t bits = 0;
    for (int r = 0; r < INPUT_ROWS; r++) {
        int c = [a[r][@"c"] intValue];
        if (c >= 0 && c < 256 && _keyDown[c]) bits |= kRowBits[r];
    }
    return bits;
}

- (void)keyDown:(NSEvent *)e { if (e.keyCode < 256) _keyDown[e.keyCode] = true; }
- (void)keyUp:(NSEvent *)e { if (e.keyCode < 256) _keyDown[e.keyCode] = false; }
- (void)flagsChanged:(NSEvent *)e {
    uint64_t m = modifier_mask(e.keyCode);
    if (m) _keyDown[e.keyCode] = (e.modifierFlags & m) != 0;
}
- (void)clearKeys { memset(_keyDown, 0, sizeof _keyDown); }

/* ---------------------------------------------------------------- config window */
- (void)showConfigWindow {
    if (!_win) [self buildWindow];
    [self refreshWindow];
    [_win makeKeyAndOrderFront:nil];
}

- (NSTextField *)label:(NSString *)s frame:(NSRect)r bold:(BOOL)bold {
    NSTextField *l = [NSTextField labelWithString:s];
    l.frame = r;
    if (bold) l.font = [NSFont boldSystemFontOfSize:[NSFont systemFontSize]];
    [_win.contentView addSubview:l];
    return l;
}

- (void)buildWindow {
    const CGFloat W = 580, H = 510;
    _win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                         backing:NSBackingStoreBuffered defer:NO];
    _win.title = @"Configure Controls";
    _win.releasedWhenClosed = NO;
    _win.delegate = self;
    NSView *cv = _win.contentView;

    _playerSeg = [NSSegmentedControl segmentedControlWithLabels:@[ @"Player 1", @"Player 2" ]
                                                   trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(playerChanged:)];
    _playerSeg.selectedSegment = 0;
    [_playerSeg sizeToFit];
    _playerSeg.frame = NSMakeRect((W - _playerSeg.frame.size.width) / 2, H - 44, _playerSeg.frame.size.width, _playerSeg.frame.size.height);
    [cv addSubview:_playerSeg];

    [self label:@"Controller:" frame:NSMakeRect(20, H - 84, 90, 20) bold:NO];
    _devicePopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(110, H - 89, W - 130, 28) pullsDown:NO];
    _devicePopup.target = self; _devicePopup.action = @selector(deviceChanged:);
    [cv addSubview:_devicePopup];

    CGFloat y = H - 126;
    [self label:@"NES button" frame:NSMakeRect(20, y, 110, 20) bold:YES];
    [self label:@"Keyboard" frame:NSMakeRect(140, y, 170, 20) bold:YES];
    [self label:@"Controller" frame:NSMakeRect(320, y, 240, 20) bold:YES];
    y -= 32;
    for (int r = 0; r < INPUT_ROWS; r++) {
        [self label:kRowNames[r] frame:NSMakeRect(20, y + 4, 110, 20) bold:NO];
        NSButton *k = [NSButton buttonWithTitle:@"" target:self action:@selector(keyCellClicked:)];
        k.frame = NSMakeRect(136, y, 178, 28); k.tag = r;
        [cv addSubview:k]; _keyBtn[r] = k;
        NSButton *b = [NSButton buttonWithTitle:@"" target:self action:@selector(padCellClicked:)];
        b.frame = NSMakeRect(316, y, 248, 28); b.tag = r;
        [cv addSubview:b]; _padBtn[r] = b;
        y -= 30;
    }
    _stickCheck = [NSButton checkboxWithTitle:@"Left stick also moves the D-pad" target:self action:@selector(stickChanged:)];
    _stickCheck.frame = NSMakeRect(318, y + 2, 250, 22);
    [cv addSubview:_stickCheck];

    _hint = [NSTextField wrappingLabelWithString:@""];
    _hint.frame = NSMakeRect(20, 12, 400, 40);
    _hint.font = [NSFont systemFontOfSize:11];
    _hint.textColor = [NSColor secondaryLabelColor];
    [cv addSubview:_hint];
    NSButton *reset = [NSButton buttonWithTitle:@"Reset to Defaults" target:self action:@selector(resetDefaults:)];
    [reset sizeToFit];
    reset.frame = NSMakeRect(W - reset.frame.size.width - 18, 16, reset.frame.size.width, 30);
    [cv addSubview:reset];
    [_win center];
}

- (int)player { return (int)_playerSeg.selectedSegment; }

- (void)refreshWindow {
    if (!_win) return;
    int p = [self player];
    /* device popup */
    [_devicePopup removeAllItems];
    InputPad *cur = [self padForPlayer:p];
    NSString *autoTitle = cur && [self deviceSetting:p].length == 0
        ? [NSString stringWithFormat:@"Automatic (%@)", cur.name] : @"Automatic";
    [_devicePopup addItemWithTitle:autoTitle];
    _devicePopup.lastItem.representedObject = @"";
    [_devicePopup addItemWithTitle:@"None (keyboard only)"];
    _devicePopup.lastItem.representedObject = @"-";
    if (_pads.count) [_devicePopup.menu addItem:[NSMenuItem separatorItem]];
    NSMutableSet *seen = [NSMutableSet set];
    for (InputPad *pad in _pads) {
        if ([seen containsObject:pad.name]) continue;
        [seen addObject:pad.name];
        [_devicePopup addItemWithTitle:pad.name];
        _devicePopup.lastItem.representedObject = pad.name;
    }
    NSString *setting = [self deviceSetting:p];
    if (setting.length > 1 && ![seen containsObject:setting]) {
        [_devicePopup addItemWithTitle:[setting stringByAppendingString:@" (not connected)"]];
        _devicePopup.lastItem.representedObject = setting;
    }
    for (NSMenuItem *it in _devicePopup.itemArray)
        if ([it.representedObject isEqual:setting]) { [_devicePopup selectItem:it]; break; }

    /* binding cells */
    NSArray *keys = [self keyBindings:p];
    NSArray *map = cur ? [self padMap:cur.profileKey] : nil;
    for (int r = 0; r < INPUT_ROWS; r++) {
        if (_capKind == 1 && _capRow == r) _keyBtn[r].title = @"Press a key…";
        else {
            int c = [keys[r][@"c"] intValue];
            _keyBtn[r].title = c < 0 ? @"—" : keys[r][@"n"];
        }
        _padBtn[r].enabled = cur != nil;
        if (_capKind == 2 && _capRow == r) _padBtn[r].title = @"Press a button…";
        else if (!cur) _padBtn[r].title = @"—";
        else {
            int vid = [map[r] intValue];
            _padBtn[r].title = vid <= 0 ? @"—" : [cur nameOfInput:vid];
        }
    }
    _stickCheck.state = [self stickMoves:p] ? NSControlStateValueOn : NSControlStateValueOff;
    _stickCheck.enabled = cur != nil;
    if (_capKind == 1) _hint.stringValue = @"Press the key to use. Esc cancels, Delete clears the binding.";
    else if (_capKind == 2) _hint.stringValue = [NSString stringWithFormat:@"Press a button or move a stick on “%@”. Esc cancels, Delete clears.", cur.name];
    else if (!cur) _hint.stringValue = @"No controller for this player. Pair Bluetooth controllers in System Settings → Bluetooth, or plug one in by USB.";
    else _hint.stringValue = [NSString stringWithFormat:@"Click a box, then press the key or button to use. Controller bindings are saved for “%@” and reused whenever it's connected.", cur.name];
}

- (void)playerChanged:(id)s { [self endCapture]; [self refreshWindow]; }
- (void)deviceChanged:(NSPopUpButton *)pop {
    [self endCapture];
    NSString *v = pop.selectedItem.representedObject ?: @"";
    [[NSUserDefaults standardUserDefaults] setObject:v forKey:[NSString stringWithFormat:@"pad.p%d", [self player] + 1]];
    [self refreshWindow];
}
- (void)stickChanged:(NSButton *)b {
    [[NSUserDefaults standardUserDefaults] setBool:b.state == NSControlStateValueOn forKey:[NSString stringWithFormat:@"stick.p%d", [self player] + 1]];
}
- (void)resetDefaults:(id)s {
    [self endCapture];
    int p = [self player];
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    [ud removeObjectForKey:[self keyDefaultsName:p]];
    [ud removeObjectForKey:[NSString stringWithFormat:@"stick.p%d", p + 1]];
    InputPad *cur = [self padForPlayer:p];
    if (cur) [ud removeObjectForKey:[@"padmap." stringByAppendingString:cur.profileKey]];
    [self refreshWindow];
}

/* ---- capture */
- (void)endCapture {
    if (_capMonitor) { [NSEvent removeMonitor:_capMonitor]; _capMonitor = nil; }
    [_capTimer invalidate]; _capTimer = nil;
    _capKind = 0;
}
- (void)windowWillClose:(NSNotification *)n { [self endCapture]; }

- (void)beginCaptureRow:(int)r kind:(int)kind {
    [self endCapture];
    _capRow = r; _capKind = kind;
    __weak InputManager *weakSelf = self;
    _capMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskFlagsChanged handler:^NSEvent *(NSEvent *e) {
        InputManager *me = weakSelf;
        if (!me || !me->_capKind) return e;
        if (e.window != me->_win) return e;
        [me captureKeyEvent:e];
        return nil;
    }];
    if (kind == 2) {
        InputPad *pad = [self padForPlayer:[self player]];
        _capBaseline = [NSMutableSet set];
        for (int v = 1; v <= 132; v++) if ([pad active:v]) [_capBaseline addObject:@(v)];
        _capStart = [NSDate timeIntervalSinceReferenceDate];
        _capTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 120.0 target:self selector:@selector(pollCapture:) userInfo:nil repeats:YES];
    }
    [self refreshWindow];
}

- (void)keyCellClicked:(NSButton *)b { [self beginCaptureRow:(int)b.tag kind:1]; }
- (void)padCellClicked:(NSButton *)b { if ([self padForPlayer:[self player]]) [self beginCaptureRow:(int)b.tag kind:2]; }

- (void)captureKeyEvent:(NSEvent *)e {
    int code = e.keyCode;
    BOOL isFlags = e.type == NSEventTypeFlagsChanged;
    if (isFlags) {
        uint64_t m = modifier_mask(code);
        if (!m || !(e.modifierFlags & m)) return;       /* only on press */
        if (code == 55 || code == 54) return;            /* Command stays reserved for menu shortcuts */
    } else if (e.isARepeat) return;
    if (!isFlags && code == 53) { [self endCapture]; [self refreshWindow]; return; }   /* Esc */
    int p = [self player];
    if (!isFlags && (code == 51 || code == 117)) {       /* Delete clears */
        if (_capKind == 1) [self bindKey:-1 name:@"" player:p row:_capRow];
        else [self bindPad:0 row:_capRow];
        [self endCapture]; [self refreshWindow]; return;
    }
    if (_capKind != 1) return;
    if (code == 48) { NSBeep(); return; }                /* Tab is fast-forward */
    NSString *name = special_key_name(code);
    if (!name) {
        name = [e.charactersIgnoringModifiers uppercaseString];
        if (!name.length || [name characterAtIndex:0] < 32) name = [NSString stringWithFormat:@"Key %d", code];
    }
    [self bindKey:code name:name player:p row:_capRow];
    [self endCapture];
    [self refreshWindow];
}

- (void)bindKey:(int)code name:(NSString *)name player:(int)p row:(int)r {
    /* a key drives one button only: take it away from anything else using it */
    if (code >= 0)
        for (int q = 0; q < 2; q++) {
            NSMutableArray *a = [[self keyBindings:q] mutableCopy];
            BOOL changed = NO;
            for (int i = 0; i < INPUT_ROWS; i++)
                if ((q != p || i != r) && [a[i][@"c"] intValue] == code) { a[i] = @{ @"c": @(-1), @"n": @"" }; changed = YES; }
            if (changed) [self setKeyBindings:a player:q];
        }
    NSMutableArray *a = [[self keyBindings:p] mutableCopy];
    a[r] = @{ @"c": @(code), @"n": name ?: @"" };
    [self setKeyBindings:a player:p];
}

- (void)bindPad:(int)vid row:(int)r {
    InputPad *pad = [self padForPlayer:[self player]];
    if (!pad) return;
    NSMutableArray *m = [[self padMap:pad.profileKey] mutableCopy];
    if (vid > 0) for (int i = 0; i < INPUT_ROWS; i++) if (i != r && [m[i] intValue] == vid) m[i] = @0;
    m[r] = @(vid);
    [self setPadMap:m profile:pad.profileKey];
}

- (void)pollCapture:(NSTimer *)t {
    InputPad *pad = [self padForPlayer:[self player]];
    if (!pad || [NSDate timeIntervalSinceReferenceDate] - _capStart > 8.0) { [self endCapture]; [self refreshWindow]; return; }
    for (int v = 1; v <= 132; v++) {
        BOOL on = [pad active:v];
        NSNumber *k = @(v);
        if (!on) { [_capBaseline removeObject:k]; continue; }
        if ([_capBaseline containsObject:k]) continue;
        [self bindPad:v row:_capRow];
        [self endCapture];
        [self refreshWindow];
        return;
    }
}
@end
