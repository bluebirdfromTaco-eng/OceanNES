/*
 * Tools: a live RAM editor (hex view of any memory region, edit by typing,
 * freeze bytes) and a real-time ROM/RAM corruptor.
 *
 * All emulator memory is touched with g_lock held. The emulation thread
 * calls tools_pre_frame / tools_post_frame around every frame to apply
 * frozen bytes and corruption.
 */
#import "tools.h"

/* ------------------------------------------------------------ memory regions */
enum { REG_RAM, REG_WRAM, REG_NT, REG_PAL, REG_OAM, REG_CHR, REG_PRG, REG_COUNT };
static NSString *const kRegionNames[REG_COUNT] = {
    @"CPU RAM ($0000–$07FF)", @"Cartridge RAM ($6000)", @"Nametable RAM (VRAM $2000)",
    @"Palette ($3F00)", @"Sprite OAM", @"CHR (graphics)", @"PRG-ROM (program)" };
static const uint32_t kRegionBase[REG_COUNT] = { 0x0000, 0x6000, 0x2000, 0x3F00, 0x0000, 0x0000, 0x0000 };

/* lock must be held */
static uint8_t *region_ptr(Nes *n, int r, uint32_t *len) {
    *len = 0;
    if (!n->loaded) return NULL;
    switch (r) {
    case REG_RAM: *len = 2048; return n->ram;
    case REG_WRAM: if (!n->cart.wram) return NULL; *len = n->cart.wram_size; return n->cart.wram;
    case REG_NT: *len = sizeof n->ppu.ciram; return n->ppu.ciram;
    case REG_PAL: *len = sizeof n->ppu.pal; return n->ppu.pal;
    case REG_OAM: *len = sizeof n->ppu.oam; return n->ppu.oam;
    case REG_CHR: if (!n->cart.chr) return NULL; *len = n->cart.chr_size; return n->cart.chr;
    case REG_PRG: if (!n->cart.prg) return NULL; *len = n->cart.prg_size; return n->cart.prg;
    default: return NULL;
    }
}

/* ------------------------------------------------------------ frame hooks */
typedef struct { int region; uint32_t off; uint8_t val; } Freeze;
#define MAX_FREEZE 256
static Freeze g_freeze[MAX_FREEZE];
static int g_nfreeze;

enum { CT_PRG = 1, CT_CHR = 2, CT_RAM = 4, CT_NT = 8, CT_OAM = 16 };
enum { CM_RANDOM, CM_BITFLIP, CM_STEP };
static struct {
    bool on;
    int targets, mode;
    double per_frame, acc;
    int burst;            /* one-off corruptions waiting to be applied */
    uint32_t rng;
    uint64_t count;       /* bytes corrupted since load / restore */
    bool dirty;           /* anything corrupted since load / restore */
} g_cor = { .rng = 0x9E3779B9u };

bool tools_sram_blocked(void) { return g_cor.dirty; }

static uint32_t rnd(void) {
    uint32_t x = g_cor.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_cor.rng = x;
}

static void apply_freezes(Nes *n) {
    for (int i = 0; i < g_nfreeze; i++) {
        uint32_t len; uint8_t *p = region_ptr(n, g_freeze[i].region, &len);
        if (p && g_freeze[i].off < len) p[g_freeze[i].off] = g_freeze[i].val;
    }
}

static void corrupt_one(Nes *n) {
    static const int map[5][2] = { { CT_PRG, REG_PRG }, { CT_CHR, REG_CHR }, { CT_RAM, REG_RAM }, { CT_NT, REG_NT }, { CT_OAM, REG_OAM } };
    int cand[5], nc = 0;
    for (int i = 0; i < 5; i++) if (g_cor.targets & map[i][0]) cand[nc++] = map[i][1];
    if (!nc) return;
    uint32_t len; uint8_t *p = region_ptr(n, cand[rnd() % (uint32_t)nc], &len);
    if (!p || !len) return;
    uint32_t off = rnd() % len;
    switch (g_cor.mode) {
    case CM_BITFLIP: p[off] ^= (uint8_t)(1u << (rnd() & 7)); break;
    case CM_STEP: p[off] = (uint8_t)(p[off] + ((rnd() & 1) ? 1 : -1)); break;
    default: p[off] = (uint8_t)rnd(); break;
    }
    g_cor.count++;
    g_cor.dirty = true;
}

void tools_pre_frame(Nes *n) {
    apply_freezes(n);
    if (g_cor.on) {
        g_cor.acc += g_cor.per_frame;
        while (g_cor.acc >= 1.0) { corrupt_one(n); g_cor.acc -= 1.0; }
    }
    while (g_cor.burst > 0) { corrupt_one(n); g_cor.burst--; }
}
void tools_post_frame(Nes *n) { apply_freezes(n); }

/* ------------------------------------------------------------ hex view */
@protocol HexSource
- (uint32_t)hexLength;
- (uint8_t)hexByte:(uint32_t)i;
- (uint32_t)hexBase;
- (BOOL)hexFrozen:(uint32_t)i;
- (double)hexChangeAge:(uint32_t)i;
- (void)hexWrite:(uint32_t)i value:(uint8_t)v;
- (void)hexSelectionChanged;
@end

@interface HexView : NSView
@property (nonatomic, weak) id<HexSource> source;
@property (nonatomic) NSInteger selected;
@property (nonatomic) int pending;          /* first typed nibble, -1 = none */
- (void)reloadSize;
- (void)scrollToSelection;
@end

@implementation HexView {
    NSFont *_font;
    CGFloat _cw, _rh;
}
- (instancetype)initWithFrame:(NSRect)r {
    if ((self = [super initWithFrame:r])) {
        _font = [NSFont userFixedPitchFontOfSize:12];
        _cw = [@"0" sizeWithAttributes:@{ NSFontAttributeName: _font }].width;
        _rh = 17;
        _selected = -1; _pending = -1;
    }
    return self;
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (CGFloat)byteX:(int)col { return 8 + 8 * _cw + col * 3 * _cw + (col >= 8 ? _cw : 0); }
- (void)reloadSize {
    uint32_t len = [self.source hexLength];
    NSUInteger rows = (len + 15) / 16;
    CGFloat w = [self byteX:16] + 12;
    NSSize s = NSMakeSize(MAX(w, self.enclosingScrollView.contentSize.width), MAX(rows * _rh + 8, self.enclosingScrollView.contentSize.height));
    [self setFrameSize:s];
    [self setNeedsDisplay:YES];
}
- (void)drawRect:(NSRect)dirty {
    [[NSColor textBackgroundColor] setFill];
    NSRectFill(dirty);
    id<HexSource> src = self.source;
    uint32_t len = [src hexLength];
    if (!len) {
        [@"No data (no game loaded, or this cartridge has none)" drawAtPoint:NSMakePoint(10, 8)
            withAttributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:12], NSForegroundColorAttributeName: [NSColor secondaryLabelColor] }];
        return;
    }
    NSInteger first = MAX(0, (NSInteger)((dirty.origin.y - 4) / _rh));
    NSInteger last = MIN((NSInteger)((len + 15) / 16) - 1, (NSInteger)((NSMaxY(dirty) - 4) / _rh));
    uint32_t base = [src hexBase];
    NSDictionary *addrAttr = @{ NSFontAttributeName: _font, NSForegroundColorAttributeName: [NSColor secondaryLabelColor] };
    for (NSInteger row = first; row <= last; row++) {
        CGFloat y = 4 + row * _rh;
        NSString *a = len + base > 0x10000 ? [NSString stringWithFormat:@"%06X", (unsigned)(base + row * 16)]
                                            : [NSString stringWithFormat:@"$%04X", (unsigned)(base + row * 16)];
        [a drawAtPoint:NSMakePoint(8, y) withAttributes:addrAttr];
        for (int col = 0; col < 16; col++) {
            uint32_t i = (uint32_t)(row * 16 + col);
            if (i >= len) break;
            CGFloat x = [self byteX:col];
            NSColor *fg = [NSColor textColor];
            NSFont *f = _font;
            if (i == (uint32_t)_selected) {
                [[NSColor selectedTextBackgroundColor] setFill];
                NSRectFill(NSMakeRect(x - 2, y - 1, 2 * _cw + 4, _rh));
            }
            if ([src hexFrozen:i]) { fg = [NSColor systemBlueColor]; f = [NSFont fontWithDescriptor:[_font.fontDescriptor fontDescriptorWithSymbolicTraits:NSFontDescriptorTraitBold] size:12] ?: _font; }
            else {
                double age = [src hexChangeAge:i];
                if (age < 1.0) fg = [[NSColor systemRedColor] blendedColorWithFraction:age ofColor:[NSColor textColor]] ?: [NSColor systemRedColor];
            }
            NSString *t = (i == (uint32_t)_selected && _pending >= 0) ? [NSString stringWithFormat:@"%X_", _pending]
                                                                      : [NSString stringWithFormat:@"%02X", [src hexByte:i]];
            [t drawAtPoint:NSMakePoint(x, y) withAttributes:@{ NSFontAttributeName: f, NSForegroundColorAttributeName: fg }];
        }
    }
}
- (void)mouseDown:(NSEvent *)e {
    [self.window makeFirstResponder:self];
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSInteger row = (NSInteger)((p.y - 4) / _rh);
    int col = -1;
    for (int c = 0; c < 16; c++) { CGFloat x = [self byteX:c]; if (p.x >= x - 3 && p.x < x + 2.5 * _cw) { col = c; break; } }
    uint32_t len = [self.source hexLength];
    if (row < 0 || col < 0 || (uint32_t)(row * 16 + col) >= len) return;
    self.selected = row * 16 + col;
    self.pending = -1;
    [self.source hexSelectionChanged];
    [self setNeedsDisplay:YES];
}
- (void)moveBy:(NSInteger)d {
    uint32_t len = [self.source hexLength];
    if (!len) return;
    NSInteger s = (_selected < 0 ? 0 : _selected) + d;
    if (s < 0) s = 0;
    if (s >= (NSInteger)len) s = len - 1;
    self.selected = s; self.pending = -1;
    [self scrollToSelection];
    [self.source hexSelectionChanged];
    [self setNeedsDisplay:YES];
}
- (void)scrollToSelection {
    if (_selected < 0) return;
    NSInteger row = _selected / 16;
    [self scrollRectToVisible:NSMakeRect(0, 4 + row * _rh - _rh, 10, 3 * _rh)];
}
- (void)keyDown:(NSEvent *)e {
    NSString *c = e.charactersIgnoringModifiers;
    unichar ch = c.length ? [c characterAtIndex:0] : 0;
    switch (ch) {
    case NSLeftArrowFunctionKey: [self moveBy:-1]; return;
    case NSRightArrowFunctionKey: [self moveBy:1]; return;
    case NSUpArrowFunctionKey: [self moveBy:-16]; return;
    case NSDownArrowFunctionKey: [self moveBy:16]; return;
    case NSPageUpFunctionKey: [self moveBy:-256]; return;
    case NSPageDownFunctionKey: [self moveBy:256]; return;
    case 27: self.pending = -1; [self setNeedsDisplay:YES]; return;
    default: break;
    }
    int h = -1;
    if (ch >= '0' && ch <= '9') h = ch - '0';
    else if (ch >= 'a' && ch <= 'f') h = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') h = ch - 'A' + 10;
    if (h < 0 || _selected < 0) { [super keyDown:e]; return; }
    if (_pending < 0) { self.pending = h; [self setNeedsDisplay:YES]; return; }
    [self.source hexWrite:(uint32_t)_selected value:(uint8_t)(_pending << 4 | h)];
    self.pending = -1;
    [self moveBy:1];
}
@end

/* ------------------------------------------------------------ manager */
@interface ToolsManager () <HexSource, NSWindowDelegate>
@end

@implementation ToolsManager {
    /* RAM editor */
    NSWindow *_ramWin;
    NSPopUpButton *_regionPopup;
    NSTextField *_gotoField, *_info;
    NSButton *_freezeBtn;
    HexView *_hex;
    NSTimer *_ramTimer;
    int _region;
    NSMutableData *_snap;
    double *_changed; uint32_t _changedLen;
    /* corruptor */
    NSWindow *_corWin;
    NSButton *_corOn, *_tgt[5];
    NSSlider *_rate;
    NSTextField *_rateLabel, *_corStatus;
    NSPopUpButton *_modePopup;
    NSTimer *_corTimer;
    NSData *_prgBackup, *_chrBackup;
}

+ (instancetype)shared {
    static ToolsManager *m;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ m = [ToolsManager new]; });
    return m;
}

- (void)say:(NSString *)s { if (self.notify) self.notify(s); }

- (void)romChanged {
    pthread_mutex_lock(&g_lock);
    g_nfreeze = 0;
    g_cor.count = 0; g_cor.dirty = false; g_cor.acc = 0; g_cor.burst = 0;
    _prgBackup = g_nes.loaded && g_nes.cart.prg ? [NSData dataWithBytes:g_nes.cart.prg length:g_nes.cart.prg_size] : nil;
    _chrBackup = g_nes.loaded && g_nes.cart.chr && !g_nes.cart.chr_is_ram ? [NSData dataWithBytes:g_nes.cart.chr length:g_nes.cart.chr_size] : nil;
    pthread_mutex_unlock(&g_lock);
    _snap = nil;
    [self refreshRam];
    [self refreshCorruptor];
}

/* ================================================================ RAM editor */
- (void)showRamEditor {
    if (!_ramWin) [self buildRam];
    [_ramWin makeKeyAndOrderFront:nil];
    [_ramWin makeFirstResponder:_hex];
    if (!_ramTimer) {
        _ramTimer = [NSTimer timerWithTimeInterval:1.0 / 20.0 target:self selector:@selector(refreshRam) userInfo:nil repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:_ramTimer forMode:NSRunLoopCommonModes];
    }
    [self refreshRam];
}

- (void)buildRam {
    const CGFloat W = 560, H = 560;
    _ramWin = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                            backing:NSBackingStoreBuffered defer:NO];
    _ramWin.title = @"RAM Editor";
    _ramWin.releasedWhenClosed = NO;
    _ramWin.delegate = self;
    _ramWin.contentMinSize = NSMakeSize(W, 300);
    _ramWin.contentMaxSize = NSMakeSize(W, 4000);
    NSView *cv = _ramWin.contentView;

    _regionPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(14, H - 40, 250, 28) pullsDown:NO];
    for (int i = 0; i < REG_COUNT; i++) [_regionPopup addItemWithTitle:kRegionNames[i]];
    _regionPopup.target = self; _regionPopup.action = @selector(regionChanged:);
    _regionPopup.autoresizingMask = NSViewMinYMargin;
    [cv addSubview:_regionPopup];
    NSTextField *gl = [NSTextField labelWithString:@"Go to:"];
    gl.frame = NSMakeRect(280, H - 35, 46, 20); gl.autoresizingMask = NSViewMinYMargin;
    [cv addSubview:gl];
    _gotoField = [[NSTextField alloc] initWithFrame:NSMakeRect(326, H - 38, 90, 24)];
    _gotoField.placeholderString = @"hex addr";
    _gotoField.font = [NSFont userFixedPitchFontOfSize:12];
    _gotoField.target = self; _gotoField.action = @selector(gotoAddr:);
    _gotoField.autoresizingMask = NSViewMinYMargin;
    [cv addSubview:_gotoField];
    _freezeBtn = [NSButton buttonWithTitle:@"Freeze" target:self action:@selector(toggleFreeze:)];
    _freezeBtn.frame = NSMakeRect(426, H - 40, 120, 28); _freezeBtn.autoresizingMask = NSViewMinYMargin;
    [cv addSubview:_freezeBtn];

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(14, 64, W - 28, H - 112)];
    sv.hasVerticalScroller = YES;
    sv.borderType = NSBezelBorder;
    sv.autoresizingMask = NSViewHeightSizable;
    _hex = [[HexView alloc] initWithFrame:NSMakeRect(0, 0, sv.contentSize.width, 100)];
    _hex.source = self;
    sv.documentView = _hex;
    [cv addSubview:sv];

    _info = [NSTextField labelWithString:@""];
    _info.frame = NSMakeRect(14, 38, W - 28, 20);
    _info.font = [NSFont userFixedPitchFontOfSize:12];
    [cv addSubview:_info];
    NSTextField *hint = [NSTextField labelWithString:@"Click a byte and type two hex digits to change it. Red = just changed, blue = frozen."];
    hint.frame = NSMakeRect(14, 14, W - 160, 18);
    hint.font = [NSFont systemFontOfSize:11];
    hint.textColor = [NSColor secondaryLabelColor];
    [cv addSubview:hint];
    NSButton *unfreeze = [NSButton buttonWithTitle:@"Unfreeze All" target:self action:@selector(unfreezeAll:)];
    unfreeze.frame = NSMakeRect(W - 136, 8, 122, 28);
    [cv addSubview:unfreeze];
    [_ramWin center];
    [_hex reloadSize];
}

- (void)windowWillClose:(NSNotification *)n {
    if (n.object == _ramWin) { [_ramTimer invalidate]; _ramTimer = nil; }
    if (n.object == _corWin) { [_corTimer invalidate]; _corTimer = nil; }
}

- (void)refreshRam {
    if (!_ramWin || !_ramWin.visible) return;
    pthread_mutex_lock(&g_lock);
    uint32_t len; uint8_t *p = region_ptr(&g_nes, _region, &len);
    BOOL resized = !_snap || _snap.length != len;
    if (resized) {
        _snap = [NSMutableData dataWithLength:len];
        free(_changed); _changed = calloc(len ? len : 1, sizeof(double)); _changedLen = len;
        if (p) memcpy(_snap.mutableBytes, p, len);
    } else if (p) {
        double now = [NSDate timeIntervalSinceReferenceDate];
        uint8_t *s = _snap.mutableBytes;
        for (uint32_t i = 0; i < len; i++) if (s[i] != p[i]) { s[i] = p[i]; _changed[i] = now; }
    }
    pthread_mutex_unlock(&g_lock);
    if (resized) { [_hex reloadSize]; if (_hex.selected >= (NSInteger)len) _hex.selected = -1; }
    [_hex setNeedsDisplay:YES];
    [self hexSelectionChanged];
}

- (void)regionChanged:(NSPopUpButton *)p {
    _region = (int)p.indexOfSelectedItem;
    _hex.selected = -1; _hex.pending = -1;
    _snap = nil;
    [self refreshRam];
    [_hex scrollPoint:NSZeroPoint];
}

- (void)gotoAddr:(NSTextField *)f {
    NSString *s = [[f.stringValue stringByReplacingOccurrencesOfString:@"$" withString:@""] stringByReplacingOccurrencesOfString:@"0x" withString:@""];
    unsigned v = 0;
    if (![[NSScanner scannerWithString:s] scanHexInt:&v]) { NSBeep(); return; }
    uint32_t base = [self hexBase], len = [self hexLength];
    uint32_t off = v >= base && v - base < len ? v - base : v;   /* accept either the address or an offset */
    if (off >= len) { NSBeep(); return; }
    _hex.selected = off; _hex.pending = -1;
    [_hex scrollToSelection];
    [_ramWin makeFirstResponder:_hex];
    [self hexSelectionChanged];
    [_hex setNeedsDisplay:YES];
}

- (int)freezeIndex:(uint32_t)off {
    for (int i = 0; i < g_nfreeze; i++) if (g_freeze[i].region == _region && g_freeze[i].off == off) return i;
    return -1;
}

- (void)toggleFreeze:(id)s {
    if (_hex.selected < 0) return;
    uint32_t off = (uint32_t)_hex.selected;
    pthread_mutex_lock(&g_lock);
    int i = [self freezeIndex:off];
    if (i >= 0) g_freeze[i] = g_freeze[--g_nfreeze];
    else if (g_nfreeze < MAX_FREEZE) {
        uint32_t len; uint8_t *p = region_ptr(&g_nes, _region, &len);
        if (p && off < len) g_freeze[g_nfreeze++] = (Freeze){ _region, off, p[off] };
    }
    pthread_mutex_unlock(&g_lock);
    [self hexSelectionChanged];
    [_hex setNeedsDisplay:YES];
}

- (void)unfreezeAll:(id)s {
    pthread_mutex_lock(&g_lock); g_nfreeze = 0; pthread_mutex_unlock(&g_lock);
    [self hexSelectionChanged];
    [_hex setNeedsDisplay:YES];
}

/* HexSource */
- (uint32_t)hexLength { return (uint32_t)_snap.length; }
- (uint8_t)hexByte:(uint32_t)i { return i < _snap.length ? ((const uint8_t *)_snap.bytes)[i] : 0; }
- (uint32_t)hexBase { return kRegionBase[_region]; }
- (BOOL)hexFrozen:(uint32_t)i { return [self freezeIndex:i] >= 0; }
- (double)hexChangeAge:(uint32_t)i {
    if (!_changed || i >= _changedLen || _changed[i] == 0) return 99;
    return [NSDate timeIntervalSinceReferenceDate] - _changed[i];
}
- (void)hexWrite:(uint32_t)i value:(uint8_t)v {
    pthread_mutex_lock(&g_lock);
    uint32_t len; uint8_t *p = region_ptr(&g_nes, _region, &len);
    if (p && i < len) {
        p[i] = v;
        int fi = [self freezeIndex:i];
        if (fi >= 0) g_freeze[fi].val = v;      /* editing a frozen byte changes what it's frozen to */
    }
    pthread_mutex_unlock(&g_lock);
    if (i < _snap.length) ((uint8_t *)_snap.mutableBytes)[i] = v;
    [_hex setNeedsDisplay:YES];
}
- (void)hexSelectionChanged {
    NSInteger s = _hex.selected;
    if (s < 0 || (NSUInteger)s >= _snap.length) {
        _info.stringValue = _snap.length ? [NSString stringWithFormat:@"%lu bytes · %d frozen", (unsigned long)_snap.length, g_nfreeze] : @"";
        _freezeBtn.title = @"Freeze"; _freezeBtn.enabled = NO;
        return;
    }
    uint8_t v = [self hexByte:(uint32_t)s];
    uint32_t addr = [self hexBase] + (uint32_t)s;
    char bits[9]; for (int b = 0; b < 8; b++) bits[b] = (v >> (7 - b)) & 1 ? '1' : '0'; bits[8] = 0;
    _info.stringValue = [NSString stringWithFormat:@"$%04X = $%02X  (%u, signed %d)  %s%@", addr, v, v, (int8_t)v, bits,
                                                   [self hexFrozen:(uint32_t)s] ? @"  · frozen" : @""];
    _freezeBtn.enabled = YES;
    _freezeBtn.title = [self hexFrozen:(uint32_t)s] ? @"Unfreeze" : @"Freeze";
}

/* ================================================================ corruptor */
static double rate_from_slider(double t) { return round(pow(10.0, t * 3.0)); }   /* 0..1 -> 1..1000 bytes/s */

- (void)showCorruptor {
    if (!_corWin) [self buildCorruptor];
    [self refreshCorruptor];
    [_corWin makeKeyAndOrderFront:nil];
    if (!_corTimer) {
        _corTimer = [NSTimer timerWithTimeInterval:0.25 target:self selector:@selector(refreshCorruptor) userInfo:nil repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:_corTimer forMode:NSRunLoopCommonModes];
    }
}

- (void)buildCorruptor {
    const CGFloat W = 440, H = 400;
    _corWin = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                            backing:NSBackingStoreBuffered defer:NO];
    _corWin.title = @"Corruptor";
    _corWin.releasedWhenClosed = NO;
    _corWin.delegate = self;
    NSView *cv = _corWin.contentView;
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    [ud registerDefaults:@{ @"corTargets": @(CT_CHR), @"corRate": @0.55, @"corMode": @(CM_RANDOM) }];

    _corOn = [NSButton checkboxWithTitle:@"Corrupt in real time" target:self action:@selector(corChanged:)];
    _corOn.font = [NSFont boldSystemFontOfSize:13];
    _corOn.frame = NSMakeRect(18, H - 40, 300, 24);
    [cv addSubview:_corOn];

    NSTextField *tl = [NSTextField labelWithString:@"Corrupt:"];
    tl.frame = NSMakeRect(18, H - 74, 100, 20);
    [cv addSubview:tl];
    NSArray *names = @[ @"Program ROM (code & data, crashes likely)", @"Graphics (CHR tiles)", @"CPU RAM (game state)",
                        @"Background map (nametables)", @"Sprites (OAM)" ];
    int bits = (int)[ud integerForKey:@"corTargets"];
    for (int i = 0; i < 5; i++) {
        _tgt[i] = [NSButton checkboxWithTitle:names[i] target:self action:@selector(corChanged:)];
        _tgt[i].frame = NSMakeRect(34, H - 100 - i * 24, 380, 22);
        _tgt[i].state = (bits >> i) & 1 ? NSControlStateValueOn : NSControlStateValueOff;
        [cv addSubview:_tgt[i]];
    }
    CGFloat y = H - 100 - 5 * 24 - 16;
    NSTextField *il = [NSTextField labelWithString:@"Intensity:"];
    il.frame = NSMakeRect(18, y, 80, 20);
    [cv addSubview:il];
    _rate = [NSSlider sliderWithValue:[ud doubleForKey:@"corRate"] minValue:0 maxValue:1 target:self action:@selector(corChanged:)];
    _rate.frame = NSMakeRect(96, y - 2, 200, 24);
    _rate.continuous = YES;
    [cv addSubview:_rate];
    _rateLabel = [NSTextField labelWithString:@""];
    _rateLabel.frame = NSMakeRect(304, y, 130, 20);
    [cv addSubview:_rateLabel];
    y -= 34;
    NSTextField *ml = [NSTextField labelWithString:@"Mode:"];
    ml.frame = NSMakeRect(18, y + 4, 80, 20);
    [cv addSubview:ml];
    _modePopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(94, y, 210, 28) pullsDown:NO];
    [_modePopup addItemsWithTitles:@[ @"Random byte", @"Flip one bit", @"Nudge value ±1" ]];
    [_modePopup selectItemAtIndex:[ud integerForKey:@"corMode"]];
    _modePopup.target = self; _modePopup.action = @selector(corChanged:);
    [cv addSubview:_modePopup];
    y -= 44;
    NSButton *burst = [NSButton buttonWithTitle:@"Corrupt Now ×50" target:self action:@selector(corBurst:)];
    burst.frame = NSMakeRect(14, y, 140, 30);
    [cv addSubview:burst];
    NSButton *restore = [NSButton buttonWithTitle:@"Restore ROM" target:self action:@selector(corRestore:)];
    restore.frame = NSMakeRect(156, y, 120, 30);
    [cv addSubview:restore];
    NSButton *restoreReset = [NSButton buttonWithTitle:@"Restore & Reset" target:self action:@selector(corRestoreReset:)];
    restoreReset.frame = NSMakeRect(278, y, 148, 30);
    [cv addSubview:restoreReset];
    _corStatus = [NSTextField wrappingLabelWithString:@""];
    _corStatus.frame = NSMakeRect(18, 10, W - 36, 44);
    _corStatus.font = [NSFont systemFontOfSize:11];
    _corStatus.textColor = [NSColor secondaryLabelColor];
    [cv addSubview:_corStatus];
    [_corWin center];
    [self applyCorruptorSettings];
}

- (void)applyCorruptorSettings {
    int bits = 0;
    for (int i = 0; i < 5; i++) if (_tgt[i].state == NSControlStateValueOn) bits |= 1 << i;
    double rate = rate_from_slider(_rate.doubleValue);
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    [ud setInteger:bits forKey:@"corTargets"];
    [ud setDouble:_rate.doubleValue forKey:@"corRate"];
    [ud setInteger:_modePopup.indexOfSelectedItem forKey:@"corMode"];
    pthread_mutex_lock(&g_lock);
    g_cor.on = _corOn.state == NSControlStateValueOn;
    g_cor.targets = bits;
    g_cor.mode = (int)_modePopup.indexOfSelectedItem;
    g_cor.per_frame = rate / 60.0988;
    pthread_mutex_unlock(&g_lock);
    _rateLabel.stringValue = [NSString stringWithFormat:@"%.0f byte%@ / second", rate, rate == 1 ? @"" : @"s"];
}

- (void)corChanged:(id)s {
    BOOL wasOn = g_cor.on;
    [self applyCorruptorSettings];
    if (s == _corOn) [self say:g_cor.on ? @"Corruptor on" : @"Corruptor off"];
    if (g_cor.on && !wasOn && !g_cor.targets) [self say:@"Pick at least one thing to corrupt"];
    [self refreshCorruptor];
}

- (void)corBurst:(id)s {
    [self applyCorruptorSettings];
    if (!g_cor.targets) { NSBeep(); [self say:@"Pick at least one thing to corrupt"]; return; }
    pthread_mutex_lock(&g_lock); g_cor.burst += 50; pthread_mutex_unlock(&g_lock);
}

- (BOOL)restoreRom {
    pthread_mutex_lock(&g_lock);
    BOOL ok = g_nes.loaded;
    if (ok && _prgBackup && _prgBackup.length == g_nes.cart.prg_size) memcpy(g_nes.cart.prg, _prgBackup.bytes, _prgBackup.length);
    if (ok && _chrBackup && _chrBackup.length == g_nes.cart.chr_size) memcpy(g_nes.cart.chr, _chrBackup.bytes, _chrBackup.length);
    g_cor.burst = 0; g_cor.acc = 0;
    pthread_mutex_unlock(&g_lock);
    return ok;
}
- (void)corRestore:(id)s {
    if (![self restoreRom]) return;
    [self say:@"ROM restored (RAM keeps its current contents; use Restore & Reset for a clean start)"];
}
- (void)corRestoreReset:(id)s {
    if (![self restoreRom]) return;
    /* turn real-time corruption off so the clean game stays clean */
    _corOn.state = NSControlStateValueOff;
    [self applyCorruptorSettings];
    /* power cycle while still marked dirty, so the possibly corrupted cartridge
     * RAM isn't saved; the power cycle reloads the last good battery save */
    if (self.powerCycle) self.powerCycle();
    pthread_mutex_lock(&g_lock); g_cor.dirty = false; g_cor.count = 0; pthread_mutex_unlock(&g_lock);
    [self say:@"ROM restored and power cycled"];
    [self refreshCorruptor];
}

- (void)refreshCorruptor {
    if (!_corWin) return;
    _corOn.state = g_cor.on ? NSControlStateValueOn : NSControlStateValueOff;
    NSMutableString *t = [NSMutableString string];
    if (!g_nes.loaded) [t appendString:@"Open a game to corrupt it. "];
    else [t appendFormat:@"%llu byte%@ corrupted since the game was loaded or restored. ", (unsigned long long)g_cor.count, g_cor.count == 1 ? @"" : @"s"];
    if (g_cor.dirty) [t appendString:@"Battery saves are paused until you use Restore & Reset or reload the game, so a corrupted game can't overwrite your save file."];
    else [t appendString:@"Corruption only changes the copy in memory: your ROM file is never modified."];
    _corStatus.stringValue = t;
}
@end
