/*
 * macOS frontend (Cocoa + CoreAudio + GameController).
 * Emulation runs on its own thread, paced by the audio device clock so
 * sound never crackles; video shows the most recent completed frame.
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <AudioToolbox/AudioToolbox.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include "../src/nes.h"
#include "../src/romfile.h"
#include "../src/midi.h"
#import "input.h"
#import "cheats.h"
#import "tools.h"

#define APP_NAME @"oceanNES"
#define OLD_APP_NAME @"Cycle"      /* earlier name: its save folder is moved over on first launch */
#define SAMPLE_RATE 48000.0
#define TARGET_BUFFER 1600      /* ~33 ms of queued audio */

/* ------------------------------------------------------------------ shared state */
Nes g_nes;                                 /* shared with tools.m */
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_running = false;      /* a ROM is loaded */
static atomic_bool g_paused = false;
static atomic_bool g_bg_paused = false;
static atomic_bool g_fast = false;
static atomic_bool g_quit = false;
static atomic_bool g_audio_ok = false;
static atomic_bool g_muted = false;
static atomic_int  g_frame_seq = 0;
static atomic_uint g_keys[2];              /* keyboard state per pad */
static atomic_uint g_pads[2];              /* game controller state per pad */
static atomic_bool g_allow_opposing = false;
/* lock-free triple buffer: the emulator owns g_write_buf, the UI owns
 * g_disp_buf, and g_ready_buf holds the newest finished frame (+NEW flag).
 * Frames are the PPU's raw 9-bit colour indices (colour + emphasis) plus the
 * colour-subcarrier phase, so the video filters can rebuild the real signal. */
static uint16_t g_idx[3][256 * 240];
static uint8_t g_phase[3];
/* current palette (UI thread only) */
static uint32_t g_pal[512];
static int g_pal_ver = 1;
static bool g_pal_custom = false;
#define BUF_NEW 4
static atomic_int g_ready_buf = 0;
static int g_write_buf = 1;
static int g_disp_buf = 2;

static inline uint8_t clean_dirs(uint8_t b) {
    if (atomic_load(&g_allow_opposing)) return b;
    if ((b & (BTN_LEFT | BTN_RIGHT)) == (BTN_LEFT | BTN_RIGHT)) b &= (uint8_t)~(BTN_LEFT | BTN_RIGHT);
    if ((b & (BTN_UP | BTN_DOWN)) == (BTN_UP | BTN_DOWN)) b &= (uint8_t)~(BTN_UP | BTN_DOWN);
    return b;
}

static double now_sec(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

/* ------------------------------------------------------------------ audio */
static AudioComponentInstance g_au;
static float g_last_sample;

/* ---- MIDI soundfont mode: the emulation thread turns the APU's channels into
 * MIDI messages (src/midi.c); the audio callback feeds them to macOS's General
 * MIDI synthesizer and plays its output instead of the NES sound */
static AudioComponentInstance g_synth;
static atomic_int g_midi_preset = -1;      /* -1 off, else index into kMidiPresets */
static atomic_bool g_midi_resync;          /* re-send programs and retrigger notes */
#define MIDI_RING 4096
static uint32_t g_midi_ring[MIDI_RING];
static atomic_uint g_midi_w, g_midi_r;
static const uint8_t kMidiPresets[][3] = {
    { 80, 80, 38 },     /* Chiptune: square lead, square lead, synth bass */
    { 30, 29, 33 },     /* Rock band: distortion guitar, overdriven guitar, finger bass */
    { 56, 40, 32 },     /* Orchestra: trumpet, violin, acoustic bass */
    { 0, 0, 32 },       /* Piano: grand piano x2, acoustic bass */
};
static NSString *const kMidiPresetNames[] = { @"Chiptune", @"Rock Band", @"Orchestra", @"Piano" };

static void midi_push(const MidiMsg *m, int k) {   /* emulation thread only */
    uint32_t w = atomic_load(&g_midi_w);
    for (int i = 0; i < k; i++) {
        if (((w + 1) & (MIDI_RING - 1)) == (atomic_load(&g_midi_r) & (MIDI_RING - 1))) break;   /* full: drop */
        g_midi_ring[w & (MIDI_RING - 1)] = m[i].status | (uint32_t)m[i].d1 << 8 | (uint32_t)m[i].d2 << 16;
        w++;
    }
    atomic_store(&g_midi_w, w);
}

/* emulation thread, after each frame, with g_lock held */
static void midi_emu_frame(void) {
    static MidiState ms;
    static int applied = -1;
    MidiMsg msg[MIDI_MAX_MSGS];
    int preset = atomic_load(&g_midi_preset);
    if (!g_synth) return;
    if (preset != applied || (preset >= 0 && atomic_exchange(&g_midi_resync, false))) {
        if (preset >= 0) { midi_init(&ms, &g_nes); midi_push(msg, midi_setup(&ms, kMidiPresets[preset], msg, MIDI_MAX_MSGS)); }
        else midi_push(msg, midi_silence(&ms, msg, MIDI_MAX_MSGS));
        applied = preset;
    }
    if (preset >= 0) midi_push(msg, midi_frame(&g_nes, &ms, msg, MIDI_MAX_MSGS));
}

static void synth_start(void) {
    AudioComponentDescription d = { kAudioUnitType_MusicDevice, kAudioUnitSubType_DLSSynth, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &g_synth) != noErr) { g_synth = NULL; return; }
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = SAMPLE_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mFramesPerPacket = 1; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
    f.mBytesPerFrame = 4; f.mBytesPerPacket = 4;
    UInt32 maxFrames = 4096;
    AudioUnitSetProperty(g_synth, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof maxFrames);
    if (AudioUnitSetProperty(g_synth, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &f, sizeof f) != noErr ||
        AudioUnitInitialize(g_synth) != noErr) {
        AudioComponentInstanceDispose(g_synth); g_synth = NULL;
    }
}

/* render the synth into L/R (audio thread) */
static void synth_render(float *L, float *R, UInt32 frames, bool playing) {
    static Float64 t;
    static bool hushed;
    if (playing) {
        hushed = false;
        uint32_t r = atomic_load(&g_midi_r), w = atomic_load(&g_midi_w);
        while (r != w) {
            uint32_t m = g_midi_ring[r & (MIDI_RING - 1)];
            MusicDeviceMIDIEvent(g_synth, m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF, 0);
            r++;
        }
        atomic_store(&g_midi_r, r);
    } else if (!hushed) {
        /* paused / powered off: stop held notes; the converter retriggers on resume */
        for (int c = 0; c < 16; c++) MusicDeviceMIDIEvent(g_synth, 0xB0 | c, 123, 0, 0);
        atomic_store(&g_midi_resync, true);
        hushed = true;
    }
    UInt32 done = 0;
    while (done < frames) {
        UInt32 n = frames - done > 4096 ? 4096 : frames - done;
        struct { AudioBufferList l; AudioBuffer second; } abl;
        abl.l.mNumberBuffers = 2;
        abl.l.mBuffers[0] = (AudioBuffer){ 1, n * 4, L + done };
        abl.second = (AudioBuffer){ 1, n * 4, R + done };
        AudioTimeStamp ts = {0};
        ts.mSampleTime = t; ts.mFlags = kAudioTimeStampSampleTimeValid;
        AudioUnitRenderActionFlags fl = 0;
        if (AudioUnitRender(g_synth, &fl, &ts, 0, n, &abl.l) != noErr) { memset(L + done, 0, n * 4); memset(R + done, 0, n * 4); }
        t += n; done += n;
    }
}

static OSStatus audio_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                         UInt32 bus, UInt32 frames, AudioBufferList *io) {
    float *out = (float *)io->mBuffers[0].mData;
    float *out2 = io->mNumberBuffers > 1 ? (float *)io->mBuffers[1].mData : NULL;
    int got = 0;
    bool playing = atomic_load(&g_running) && !atomic_load(&g_paused) && !atomic_load(&g_bg_paused);
    if (playing) {
        /* this callback is the ring's only consumer: it also trims excess
         * (e.g. while fast-forwarding) so latency stays bounded */
        static float scratch[4096];
        int avail = nes_audio_available(&g_nes);
        int excess = avail - (int)frames - TARGET_BUFFER * 2;
        while (excess > 0) { int k = excess > 4096 ? 4096 : excess; nes_read_audio(&g_nes, scratch, k); excess -= k; }
        got = nes_read_audio(&g_nes, out, (int)frames);
    }
    if (got > 0) g_last_sample = out[got - 1];
    for (int i = got; i < (int)frames; i++) { out[i] = g_last_sample; g_last_sample *= 0.995f; }
    if (g_synth && atomic_load(&g_midi_preset) >= 0 && out2) {
        /* the NES samples were still consumed above (they pace the emulator) */
        synth_render(out, out2, frames, playing);
        for (UInt32 i = 0; i < frames; i++) { out[i] *= 1.6f; out2[i] *= 1.6f; }
    } else if (out2) memcpy(out2, out, frames * sizeof(float));
    if (atomic_load(&g_muted)) { memset(out, 0, frames * sizeof(float)); if (out2) memset(out2, 0, frames * sizeof(float)); }
    return noErr;
}

static bool audio_start(void) {
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &g_au) != noErr) return false;
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = SAMPLE_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    f.mFramesPerPacket = 1; f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;   /* stereo, non-interleaved */
    f.mBytesPerFrame = 4; f.mBytesPerPacket = 4;
    synth_start();
    if (AudioUnitSetProperty(g_au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr) return false;
    AURenderCallbackStruct cb = { audio_cb, NULL };
    if (AudioUnitSetProperty(g_au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr) return false;
    if (AudioUnitInitialize(g_au) != noErr) return false;
    if (AudioOutputUnitStart(g_au) != noErr) return false;
    return true;
}

/* ------------------------------------------------------------------ emulation thread */
static void publish_frame(void) {
    memcpy(g_idx[g_write_buf], g_nes.ppu.fb, sizeof g_idx[0]);
    g_phase[g_write_buf] = g_nes.ppu.frame_phase;
    int prev = atomic_exchange(&g_ready_buf, g_write_buf | BUF_NEW);
    g_write_buf = prev & 3;
    atomic_fetch_add(&g_frame_seq, 1);
}

/* UI thread: grab the newest frame if there is one */
static void acquire_frame(void) {
    if (atomic_load(&g_ready_buf) & BUF_NEW) {
        int got = atomic_exchange(&g_ready_buf, g_disp_buf);
        g_disp_buf = got & 3;
    }
}

static void *emu_thread(void *arg) {
    (void)arg;
    double next = now_sec();
    double last_frame = now_sec();
    const double frame_time = 1.0 / 60.0988;
    while (!atomic_load(&g_quit)) {
        if (!atomic_load(&g_running) || atomic_load(&g_paused) || atomic_load(&g_bg_paused)) {
            usleep(4000); next = now_sec(); continue;
        }
        bool fast = atomic_load(&g_fast);
        if (atomic_load(&g_audio_ok) && !fast && now_sec() - last_frame < 0.1) {
            /* paced by the audio device: only emulate when the queue runs low */
            if (nes_audio_available(&g_nes) > TARGET_BUFFER) { usleep(1000); continue; }
            next = now_sec();
        } else {
            double t = now_sec();
            double step = fast ? frame_time / 4 : frame_time;
            if (t < next) { usleep((useconds_t)((next - t) * 1e6 > 2000 ? 1000 : (next - t) * 1e6)); continue; }
            next += step;
            if (t - next > 0.25) next = t;
        }
        pthread_mutex_lock(&g_lock);
        if (atomic_load(&g_running)) {
            /* turbo buttons: 15 presses a second (2 frames on, 2 off) */
            static unsigned turbo_frame;
            bool turbo_on = (turbo_frame++ & 2) == 0;
            for (int p = 0; p < 2; p++) {
                uint32_t st = atomic_load(&g_keys[p]) | atomic_load(&g_pads[p]);
                uint8_t b = (uint8_t)st;
                if (turbo_on && (st & TURBO_A_BIT)) b |= BTN_A;
                if (turbo_on && (st & TURBO_B_BIT)) b |= BTN_B;
                g_nes.pad_state[p] = clean_dirs(b);
            }
            tools_pre_frame(&g_nes);
            nes_run_frame(&g_nes);
            tools_post_frame(&g_nes);
            midi_emu_frame();
            publish_frame();
            last_frame = now_sec();
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* ------------------------------------------------------------------ paths */
static NSString *support_dir(NSString *sub) {
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *base = [[dirs firstObject] stringByAppendingPathComponent:APP_NAME];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        /* moved over from the app's earlier name, so saves and states carry over */
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *old = [[dirs firstObject] stringByAppendingPathComponent:OLD_APP_NAME];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:base] && [fm fileExistsAtPath:old isDirectory:&isDir] && isDir)
            [fm moveItemAtPath:old toPath:base error:nil];
    });
    NSString *dir = sub ? [base stringByAppendingPathComponent:sub] : base;
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return dir;
}

/* ------------------------------------------------------------------ video view */
enum { FILTER_SHARP, FILTER_SMOOTH, FILTER_CRT, FILTER_NTSC, FILTER_NTSC_CRT };
enum { ASPECT_4_3, ASPECT_8_7, ASPECT_SQUARE };

/* must match the structs in shaders.metal */
typedef struct { float enc_hue, dec_hue, sat, bri, con; int phase0, custom, pad; } NtscParamsC;
typedef struct { float src_size[2], out_size[2]; float v0, v1; int mode, mask; float curvature, scan, glow, pad; } OutParamsC;

@interface NesView : NSView
@property (nonatomic) CALayer *imageLayer;      /* fallback path (no Metal) */
@property (nonatomic, readonly) BOOL metal;
@property (nonatomic) int aspectMode;
@property (nonatomic) BOOL cropOverscan;
@property (nonatomic) int filter;
@property (nonatomic) BOOL crtCurvature;
@property (nonatomic) float crtScan;
@property (nonatomic) int crtMask;
@property (nonatomic) float hue, sat, bri, con;  /* colour adjustments */
- (void)showFrame;
- (void)redraw;
- (NSSize)baseSize;
- (void)relayoutImage;
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic) NSWindow *window;
@property (nonatomic) NesView *view;
@property (nonatomic) NSTextField *toast;
@property (nonatomic, copy) NSString *romPath;
@property (nonatomic, copy) NSString *romName;
@property (nonatomic) int stateSlot;
@property (nonatomic) NSMenu *recentMenu;
@property (nonatomic) NSMenu *slotMenu;
@property (nonatomic) NSData *savedWram;
@property (nonatomic, copy) NSString *pendingFile;
@property (nonatomic) NSMenuItem *customPalItem;
@property (nonatomic) NSPanel *colorPanel;
@property (nonatomic) NSArray<NSSlider *> *colorSliders;
- (void)keyEvent:(NSEvent *)e down:(BOOL)down;
- (void)flagsEvent:(NSEvent *)e;
- (BOOL)openROM:(NSString *)path;
- (BOOL)loadPaletteFile:(NSString *)path;
@end

static AppDelegate *g_app;
static const float kScan[3] = { 0.4f, 0.7f, 1.0f };

@implementation NesView {
    id<MTLDevice> _dev;
    id<MTLCommandQueue> _queue;
    CAMetalLayer *_ml;
    id<MTLRenderPipelineState> _pPal, _pNtsc, _pOut;
    id<MTLTexture> _idxTex, _palTex, _rgbTex, _ntscTex;
    dispatch_semaphore_t _inflight;
    int _palVer;
    uint32_t _rgb[256 * 240];
}

- (instancetype)initWithFrame:(NSRect)r {
    if ((self = [super initWithFrame:r])) {
        _sat = 1; _con = 1; _crtScan = 0.7f; _crtMask = 1; _crtCurvature = YES;
        if (![self setupMetal]) {
            self.layer = [CALayer layer];   /* layer-hosting: set the layer before wantsLayer */
            self.wantsLayer = YES;
            self.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
            _imageLayer = [CALayer layer];
            _imageLayer.magnificationFilter = kCAFilterNearest;
            _imageLayer.minificationFilter = kCAFilterLinear;
            _imageLayer.contentsGravity = kCAGravityResize;
            _imageLayer.actions = @{ @"contents": [NSNull null], @"bounds": [NSNull null], @"position": [NSNull null] };
            [self.layer addSublayer:_imageLayer];
        }
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    }
    return self;
}

- (id<MTLRenderPipelineState>)pipeline:(id<MTLLibrary>)lib frag:(NSString *)f format:(MTLPixelFormat)fmt {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs_quad"];
    d.fragmentFunction = [lib newFunctionWithName:f];
    d.colorAttachments[0].pixelFormat = fmt;
    if (!d.vertexFunction || !d.fragmentFunction) return nil;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) NSLog(@"oceanNES: pipeline %@ failed: %@", f, err);
    return p;
}

- (BOOL)setupMetal {
    if ([[NSUserDefaults standardUserDefaults] boolForKey:@"noMetal"]) return NO;
    _dev = MTLCreateSystemDefaultDevice();
    if (!_dev) return NO;
    NSString *path = [[NSBundle mainBundle] pathForResource:@"shaders" ofType:@"metal"];
    NSString *src = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
    if (!src) { NSLog(@"oceanNES: shaders.metal missing"); _dev = nil; return NO; }
    NSError *err = nil;
    id<MTLLibrary> lib = [_dev newLibraryWithSource:src options:nil error:&err];
    if (!lib) { NSLog(@"oceanNES: shader compile failed: %@", err); _dev = nil; return NO; }
    _pPal = [self pipeline:lib frag:@"fs_palette" format:MTLPixelFormatRGBA8Unorm];
    _pNtsc = [self pipeline:lib frag:@"fs_ntsc" format:MTLPixelFormatRGBA16Float];
    _pOut = [self pipeline:lib frag:@"fs_out" format:MTLPixelFormatBGRA8Unorm];
    if (!_pPal || !_pNtsc || !_pOut) { _dev = nil; return NO; }
    _queue = [_dev newCommandQueue];

    MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Uint width:256 height:240 mipmapped:NO];
    _idxTex = [_dev newTextureWithDescriptor:t];
    t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:512 height:1 mipmapped:NO];
    _palTex = [_dev newTextureWithDescriptor:t];
    t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:240 mipmapped:NO];
    t.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; t.storageMode = MTLStorageModePrivate;
    _rgbTex = [_dev newTextureWithDescriptor:t];
    t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:1024 height:240 mipmapped:NO];
    t.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; t.storageMode = MTLStorageModePrivate;
    _ntscTex = [_dev newTextureWithDescriptor:t];
    if (!_idxTex || !_palTex || !_rgbTex || !_ntscTex) { _dev = nil; return NO; }

    _ml = [CAMetalLayer layer];
    _ml.device = _dev;
    _ml.pixelFormat = MTLPixelFormatBGRA8Unorm;
    _ml.framebufferOnly = YES;
    _ml.opaque = YES;
    _ml.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    _ml.colorspace = cs; CGColorSpaceRelease(cs);
    self.layer = _ml;
    self.wantsLayer = YES;
    _inflight = dispatch_semaphore_create(1);
    _metal = YES;
    return YES;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }
- (NSSize)baseSize {
    double h = _cropOverscan ? 224.0 : 240.0;
    double w = _aspectMode == ASPECT_4_3 ? h * 4.0 / 3.0 : _aspectMode == ASPECT_8_7 ? 256.0 * 8.0 / 7.0 : 256.0;
    return NSMakeSize(w, h);
}
- (NSRect)imageRect {   /* in points, centred, letterboxed */
    NSSize b = [self baseSize];
    NSRect r = self.bounds;
    double scale = fmin(r.size.width / b.width, r.size.height / b.height);
    double w = floor(b.width * scale), h = floor(b.height * scale);
    return NSMakeRect(floor((r.size.width - w) / 2), floor((r.size.height - h) / 2), w, h);
}
- (void)layout { [super layout]; [self relayoutImage]; }
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self relayoutImage]; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self relayoutImage]; }
- (void)relayoutImage {
    if (_ml) {
        CGFloat sc = self.window ? self.window.backingScaleFactor : 1.0;
        _ml.contentsScale = sc;
        CGSize ds = CGSizeMake(MAX(1.0, self.bounds.size.width * sc), MAX(1.0, self.bounds.size.height * sc));
        if (!CGSizeEqualToSize(_ml.drawableSize, ds)) _ml.drawableSize = ds;
        [self redraw];
        return;
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _imageLayer.frame = NSRectToCGRect([self imageRect]);
    _imageLayer.magnificationFilter = _filter == FILTER_SHARP ? kCAFilterNearest : kCAFilterLinear;
    [CATransaction commit];
}

- (void)showFrame { acquire_frame(); [self redraw]; }

- (void)redraw {
    if (_ml) [self renderMetal]; else [self renderFallback];
}

- (void)renderFallback {
    int buf = g_disp_buf;
    int rows = _cropOverscan ? 224 : 240, top = _cropOverscan ? 8 : 0;
    for (int i = 0; i < 256 * rows; i++) _rgb[i] = g_pal[g_idx[buf][top * 256 + i] & 511];
    CFDataRef data = CFDataCreate(NULL, (const UInt8 *)_rgb, rows * 256 * 4);
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate(256, rows, 8, 32, 256 * 4, cs,
                                   kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, prov, NULL, false, kCGRenderingIntentDefault);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _imageLayer.contents = (__bridge id)img;
    [CATransaction commit];
    CGImageRelease(img); CGColorSpaceRelease(cs); CGDataProviderRelease(prov); CFRelease(data);
}

- (void)renderMetal {
    if (self.bounds.size.width < 1 || self.bounds.size.height < 1) return;
    /* one frame in flight: the textures below are rewritten every frame */
    if (dispatch_semaphore_wait(_inflight, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) != 0) return;
    int buf = g_disp_buf;
    [_idxTex replaceRegion:MTLRegionMake2D(0, 0, 256, 240) mipmapLevel:0 withBytes:g_idx[buf] bytesPerRow:256 * 2];
    if (_palVer != g_pal_ver) {
        uint8_t px[512 * 4];
        for (int i = 0; i < 512; i++) { px[i * 4] = (uint8_t)(g_pal[i] >> 16); px[i * 4 + 1] = (uint8_t)(g_pal[i] >> 8); px[i * 4 + 2] = (uint8_t)g_pal[i]; px[i * 4 + 3] = 255; }
        [_palTex replaceRegion:MTLRegionMake2D(0, 0, 512, 1) mipmapLevel:0 withBytes:px bytesPerRow:512 * 4];
        _palVer = g_pal_ver;
    }
    id<CAMetalDrawable> drawable = [_ml nextDrawable];
    if (!drawable) { dispatch_semaphore_signal(_inflight); return; }
    id<MTLCommandBuffer> cb = [_queue commandBuffer];
    BOOL ntsc = _filter == FILTER_NTSC || _filter == FILTER_NTSC_CRT;
    BOOL crt = _filter == FILTER_CRT || _filter == FILTER_NTSC_CRT;
    float full[4] = { -1, -1, 1, 1 };

    /* pass 1: palette lookup or composite simulation */
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = ntsc ? _ntscTex : _rgbTex;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ntsc ? _pNtsc : _pPal];
    [e setVertexBytes:full length:sizeof full atIndex:0];
    [e setFragmentTexture:_idxTex atIndex:0];
    [e setFragmentTexture:_palTex atIndex:1];
    if (ntsc) {
        NtscParamsC np;
        np.enc_hue = 3.8f;
        np.phase0 = g_phase[buf];
        np.pad = 0;
        if (g_pal_custom) {
            /* the custom palette already carries the colour adjustments */
            np.custom = 1; np.dec_hue = 3.8f; np.sat = 1.0f; np.bri = 0; np.con = 1;
        } else {
            np.custom = 0; np.dec_hue = 3.8f + _hue / 30.0f; np.sat = 0.6f * _sat; np.bri = _bri; np.con = _con;
        }
        [e setFragmentBytes:&np length:sizeof np atIndex:0];
    }
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];

    /* pass 2: scale onto the screen */
    CGFloat sc = _ml.contentsScale;
    NSRect ir = [self imageRect];
    double vx = ir.origin.x * sc, vw = ir.size.width * sc, vh = ir.size.height * sc;
    double vy = (self.bounds.size.height - NSMaxY(ir)) * sc;   /* Metal viewports start at the top */
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:_pOut];
    [e setViewport:(MTLViewport){ vx, vy, vw, vh, 0, 1 }];
    [e setVertexBytes:full length:sizeof full atIndex:0];
    [e setFragmentTexture:ntsc ? _ntscTex : _rgbTex atIndex:0];
    OutParamsC op;
    memset(&op, 0, sizeof op);
    op.src_size[0] = ntsc ? 1024 : 256; op.src_size[1] = 240;
    op.out_size[0] = (float)vw; op.out_size[1] = (float)vh;
    op.v0 = _cropOverscan ? 8.0f / 240.0f : 0.0f;
    op.v1 = _cropOverscan ? 232.0f / 240.0f : 1.0f;
    op.mode = crt ? 2 : (_filter == FILTER_SMOOTH ? 1 : 0);
    op.mask = crt ? _crtMask : 0;
    op.curvature = (crt && _crtCurvature) ? 0.06f : 0.0f;
    op.scan = _crtScan;
    op.glow = 0.6f;
    [e setFragmentBytes:&op length:sizeof op atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];

    dispatch_semaphore_t sem = _inflight;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) { dispatch_semaphore_signal(sem); }];
    [cb presentDrawable:drawable];
    [cb commit];
}

- (void)keyDown:(NSEvent *)e { [g_app keyEvent:e down:YES]; }
- (void)keyUp:(NSEvent *)e { [g_app keyEvent:e down:NO]; }
- (void)flagsChanged:(NSEvent *)e { [g_app flagsEvent:e]; }
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)s { return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)s {
    NSArray *urls = [[s draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    NSURL *u = [urls firstObject];
    if (!u) return NO;
    if ([[u.pathExtension lowercaseString] isEqualToString:@"pal"]) return [g_app loadPaletteFile:u.path];
    return [g_app openROM:u.path];
}
@end

/* ------------------------------------------------------------------ app */
@implementation AppDelegate {
    /* realistic cartridge handling */
    BOOL _badContact;        /* powered on, but the cart isn't making contact: screen blinks */
    BOOL _cleaned;           /* blown on since the last power-on: the next one works */
    NSString *_dirtyCart;    /* path of a cart that failed: it keeps failing until blown on */
    BOOL _blowing;
    int _blowLeft;
    NSTimer *_blinkTimer, *_blowTimer;
    BOOL _blinkOn;
    NSTimer *_displayTimer;
    int _lastSeq;
    double _lastSramCheck;
    pthread_t _thread;
    NSTimer *_toastTimer;
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    g_app = self;
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    [ud registerDefaults:@{ @"scale": @3, @"aspectMode": @(ASPECT_4_3), @"crop": @YES, @"filter": @(FILTER_SHARP),
                            @"crtCurvature": @YES, @"crtScan": @0.7, @"crtMask": @1,
                            @"hue": @0.0, @"sat": @1.0, @"bri": @0.0, @"con": @1.0,
                            @"pauseBG": @YES, @"opposing": @NO, @"audioFilter": @(AUDIO_FILTER_NES) }];
    g_nes.audio_filter = (int)[ud integerForKey:@"audioFilter"];
    g_nes.oc_lines = (int)[ud integerForKey:@"ocLines"];
    if ([ud objectForKey:@"midiPreset"]) atomic_store(&g_midi_preset, (int)[ud integerForKey:@"midiPreset"]);
    atomic_store(&g_allow_opposing, [ud boolForKey:@"opposing"]);
    self.stateSlot = 1;

    nes_set_sample_rate(&g_nes, SAMPLE_RATE);
    atomic_store(&g_audio_ok, audio_start());
    if (atomic_load(&g_midi_preset) > 3) atomic_store(&g_midi_preset, -1);
    if ([ud stringForKey:@"soundFont"]) [self applySoundFont:[ud stringForKey:@"soundFont"]];

    [self buildMenus];

    NSRect r = NSMakeRect(0, 0, 896, 672);
    self.window = [[NSWindow alloc] initWithContentRect:r
                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered defer:NO];
    self.window.title = APP_NAME;
    self.window.delegate = self;
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.backgroundColor = [NSColor blackColor];
    NSView *container = [[NSView alloc] initWithFrame:r];
    container.wantsLayer = YES;
    self.view = [[NesView alloc] initWithFrame:r];
    self.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.view.aspectMode = (int)[ud integerForKey:@"aspectMode"];
    self.view.cropOverscan = [ud boolForKey:@"crop"];
    self.view.filter = (int)[ud integerForKey:@"filter"];
    if (!self.view.metal && self.view.filter > FILTER_SMOOTH) self.view.filter = FILTER_SHARP;
    self.view.crtCurvature = [ud boolForKey:@"crtCurvature"];
    self.view.crtScan = [ud floatForKey:@"crtScan"];
    self.view.crtMask = (int)[ud integerForKey:@"crtMask"];
    self.view.hue = [ud floatForKey:@"hue"]; self.view.sat = [ud floatForKey:@"sat"];
    self.view.bri = [ud floatForKey:@"bri"]; self.view.con = [ud floatForKey:@"con"];
    [self rebuildPalette];
    [container addSubview:self.view];
    self.window.contentView = container;

    self.toast = [NSTextField labelWithString:@""];
    self.toast.font = [NSFont boldSystemFontOfSize:13];
    self.toast.textColor = [NSColor whiteColor];
    self.toast.backgroundColor = [NSColor colorWithWhite:0 alpha:0.6];
    self.toast.drawsBackground = YES;
    self.toast.hidden = YES;
    [container addSubview:self.toast positioned:NSWindowAbove relativeTo:self.view];

    [self applyScale:(int)[ud integerForKey:@"scale"]];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    [self clearScreen];

    pthread_create(&_thread, NULL, emu_thread, NULL);
    _displayTimer = [NSTimer timerWithTimeInterval:1.0 / 120.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:_displayTimer forMode:NSRunLoopCommonModes];

    __weak AppDelegate *weakSelf = self;
    [InputManager shared].notify = ^(NSString *msg) { [weakSelf showToast:msg]; };
    [[InputManager shared] start];
    [ToolsManager shared].notify = ^(NSString *msg) { [weakSelf showToast:msg]; };
    [ToolsManager shared].powerCycle = ^{ [weakSelf hardReset:nil]; };
    [CheatManager shared].apply = ^(const Cheat *codes, int count) {
        pthread_mutex_lock(&g_lock);
        memcpy(g_nes.cheats, codes, sizeof(Cheat) * (size_t)count);
        g_nes.num_cheats = count;
        pthread_mutex_unlock(&g_lock);
    };

    if (self.pendingFile) { [self openROM:self.pendingFile]; self.pendingFile = nil; }
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    for (NSUInteger i = 1; i < args.count; i++) {
        NSString *a = args[i];
        if (g_nes.loaded) break;
        if (![a hasPrefix:@"-"] && [[NSFileManager defaultManager] fileExistsAtPath:a]) { [self openROM:a]; break; }
    }
    if (!g_nes.loaded) [self showToast:@"Open a ROM with ⌘O, or drag a .nes / .zip file here"];
}

- (BOOL)application:(NSApplication *)app openFile:(NSString *)file {
    if (!self.window) { self.pendingFile = file; return YES; }   /* launched by opening a ROM */
    return [self openROM:file];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }
- (void)applicationWillTerminate:(NSNotification *)n {
    atomic_store(&g_quit, true);
    pthread_join(_thread, NULL);
    [self saveSRAM];
    if (g_au) { AudioOutputUnitStop(g_au); AudioComponentInstanceDispose(g_au); }
}
- (void)applicationDidResignActive:(NSNotification *)n {
    if ([[NSUserDefaults standardUserDefaults] boolForKey:@"pauseBG"]) atomic_store(&g_bg_paused, true);
    [[InputManager shared] clearKeys];
    atomic_store(&g_keys[0], 0); atomic_store(&g_keys[1], 0); atomic_store(&g_fast, false);
}
- (void)windowDidResignKey:(NSNotification *)n {
    if (n.object != self.window) return;
    [[InputManager shared] clearKeys];
    [self updateKeys];
    atomic_store(&g_fast, false);
}
- (void)applicationDidBecomeActive:(NSNotification *)n { atomic_store(&g_bg_paused, false); }

/* ---------------------------------------------------------------- menus */
- (NSMenuItem *)item:(NSString *)title action:(SEL)a key:(NSString *)k mods:(NSEventModifierFlags)m menu:(NSMenu *)menu {
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:a keyEquivalent:k ?: @""];
    if (k) it.keyEquivalentModifierMask = m;
    it.target = self;
    [menu addItem:it];
    return it;
}

- (void)buildMenus {
    NSMenu *bar = [NSMenu new];
    NSEventModifierFlags C = NSEventModifierFlagCommand, CS = NSEventModifierFlagCommand | NSEventModifierFlagShift;

    NSMenuItem *appItem = [NSMenuItem new]; [bar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:APP_NAME];
    [self item:[NSString stringWithFormat:@"About %@", APP_NAME] action:@selector(about:) key:nil mods:0 menu:appMenu];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [self item:@"Configure Controls…" action:@selector(configureControls:) key:@"," mods:C menu:appMenu];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *hide = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Hide %@", APP_NAME] action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:hide];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Quit %@", APP_NAME] action:@selector(terminate:) keyEquivalent:@"q"];
    [appMenu addItem:quit];
    appItem.submenu = appMenu;

    NSMenuItem *fileItem = [NSMenuItem new]; [bar addItem:fileItem];
    NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
    [self item:@"Open ROM…" action:@selector(openDocument:) key:@"o" mods:C menu:file];
    NSMenuItem *recent = [[NSMenuItem alloc] initWithTitle:@"Open Recent" action:nil keyEquivalent:@""];
    self.recentMenu = [[NSMenu alloc] initWithTitle:@"Open Recent"];
    recent.submenu = self.recentMenu;
    [file addItem:recent];
    [self rebuildRecent];
    [file addItem:[NSMenuItem separatorItem]];
    [self item:@"Close ROM" action:@selector(closeROM:) key:@"w" mods:C menu:file];
    [file addItem:[NSMenuItem separatorItem]];
    [self item:@"Save Screenshot" action:@selector(screenshot:) key:@"p" mods:CS menu:file];
    [self item:@"Show Save Files in Finder" action:@selector(showSaves:) key:nil mods:0 menu:file];
    fileItem.submenu = file;

    NSMenuItem *emuItem = [NSMenuItem new]; [bar addItem:emuItem];
    NSMenu *emu = [[NSMenu alloc] initWithTitle:@"Emulation"];
    [self item:@"Power On" action:@selector(togglePower:) key:@"p" mods:C | NSEventModifierFlagOption menu:emu];
    [self item:@"Blow on Cartridge" action:@selector(blowOnCart:) key:@"b" mods:C | NSEventModifierFlagOption menu:emu];
    [emu addItem:[NSMenuItem separatorItem]];
    [self item:@"Pause" action:@selector(togglePause:) key:@"p" mods:C menu:emu];
    [self item:@"Reset" action:@selector(softReset:) key:@"r" mods:C menu:emu];
    [self item:@"Power Cycle" action:@selector(hardReset:) key:@"r" mods:CS menu:emu];
    [emu addItem:[NSMenuItem separatorItem]];
    [self item:@"Save State" action:@selector(saveState:) key:@"s" mods:C menu:emu];
    [self item:@"Load State" action:@selector(loadState:) key:@"l" mods:C menu:emu];
    NSMenuItem *slot = [[NSMenuItem alloc] initWithTitle:@"State Slot" action:nil keyEquivalent:@""];
    self.slotMenu = [[NSMenu alloc] initWithTitle:@"State Slot"];
    for (int i = 1; i <= 9; i++) {
        NSMenuItem *it = [self item:[NSString stringWithFormat:@"Slot %d", i] action:@selector(selectSlot:)
                                key:[NSString stringWithFormat:@"%d", i] mods:NSEventModifierFlagControl menu:self.slotMenu];
        it.tag = i;
    }
    slot.submenu = self.slotMenu;
    [emu addItem:slot];
    [emu addItem:[NSMenuItem separatorItem]];
    [self item:@"Configure Controls…" action:@selector(configureControls:) key:nil mods:0 menu:emu];
    [self item:@"Game Genie Codes…" action:@selector(showCheats:) key:@"g" mods:C menu:emu];
    [emu addItem:[NSMenuItem separatorItem]];
    [self item:@"Realistic Cartridge Handling" action:@selector(toggleRealCart:) key:nil mods:0 menu:emu];
    [self item:@"Pause When in Background" action:@selector(togglePauseBG:) key:nil mods:0 menu:emu];
    [self item:@"Allow Left+Right / Up+Down" action:@selector(toggleOpposing:) key:nil mods:0 menu:emu];
    [self item:@"Mute" action:@selector(toggleMute:) key:@"m" mods:CS menu:emu];
    NSMenuItem *ocItem = [[NSMenuItem alloc] initWithTitle:@"Overclock" action:nil keyEquivalent:@""];
    NSMenu *ocm = [[NSMenu alloc] initWithTitle:@"Overclock"];
    struct { NSString *name; int lines; } oc[] = {
        { @"Off (original speed)", 0 }, { @"+25% CPU time", 66 }, { @"+50% CPU time", 131 }, { @"+100% CPU time", 262 } };
    for (int i = 0; i < 4; i++) [self item:oc[i].name action:@selector(chooseOverclock:) key:nil mods:0 menu:ocm].tag = oc[i].lines;
    [ocm addItem:[NSMenuItem separatorItem]];
    NSMenuItem *ocNote = [[NSMenuItem alloc] initWithTitle:@"Reduces slowdown; game speed and music stay the same" action:nil keyEquivalent:@""];
    ocNote.enabled = NO;
    [ocm addItem:ocNote];
    ocItem.submenu = ocm; [emu addItem:ocItem];
    NSMenuItem *snd = [[NSMenuItem alloc] initWithTitle:@"Sound Filter" action:nil keyEquivalent:@""];
    NSMenu *sndm = [[NSMenu alloc] initWithTitle:@"Sound Filter"];
    struct { NSString *name; int mode; } af[] = {
        { @"NES (front-loader)", AUDIO_FILTER_NES }, { @"Famicom", AUDIO_FILTER_FAMICOM },
        { @"NES + TV Speaker", AUDIO_FILTER_TV }, { @"Raw (no filter)", AUDIO_FILTER_RAW } };
    for (int i = 0; i < 4; i++) [self item:af[i].name action:@selector(chooseAudioFilter:) key:nil mods:0 menu:sndm].tag = af[i].mode;
    [sndm addItem:[NSMenuItem separatorItem]];
    NSMenuItem *mh = [[NSMenuItem alloc] initWithTitle:@"MIDI Soundfont" action:nil keyEquivalent:@""];
    mh.enabled = NO;
    [sndm addItem:mh];
    for (int i = 0; i < 4; i++)
        [self item:[NSString stringWithFormat:@"   MIDI: %@", kMidiPresetNames[i]] action:@selector(chooseMidi:) key:nil mods:0 menu:sndm].tag = i;
    [self item:@"   Load SoundFont (.sf2 / .dls)…" action:@selector(loadSoundFont:) key:nil mods:0 menu:sndm];
    [self item:@"   Use Built-in macOS Soundfont" action:@selector(builtinSoundFont:) key:nil mods:0 menu:sndm];
    snd.submenu = sndm; [emu addItem:snd];
    emuItem.submenu = emu;

    NSMenuItem *viewItem = [NSMenuItem new]; [bar addItem:viewItem];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    for (int i = 1; i <= 5; i++) {
        NSMenuItem *it = [self item:[NSString stringWithFormat:@"Size %dx", i] action:@selector(chooseScale:)
                                key:[NSString stringWithFormat:@"%d", i] mods:C menu:view];
        it.tag = i;
    }
    [view addItem:[NSMenuItem separatorItem]];
    NSMenuItem *asp = [[NSMenuItem alloc] initWithTitle:@"Aspect Ratio" action:nil keyEquivalent:@""];
    NSMenu *aspm = [[NSMenu alloc] initWithTitle:@"Aspect Ratio"];
    NSArray *aspNames = @[ @"4:3 (TV screen)", @"8:7 Pixel Aspect", @"Square Pixels (1:1)" ];
    for (int i = 0; i < 3; i++) [self item:aspNames[i] action:@selector(chooseAspect:) key:nil mods:0 menu:aspm].tag = i;
    asp.submenu = aspm; [view addItem:asp];
    [self item:@"Hide Overscan (top/bottom 8 lines)" action:@selector(toggleCrop:) key:nil mods:0 menu:view];
    [view addItem:[NSMenuItem separatorItem]];
    NSMenuItem *flt = [[NSMenuItem alloc] initWithTitle:@"Video Filter" action:nil keyEquivalent:@""];
    NSMenu *fltm = [[NSMenu alloc] initWithTitle:@"Video Filter"];
    NSArray *fltNames = @[ @"Sharp Pixels", @"Smooth", @"CRT", @"NTSC Composite", @"NTSC Composite + CRT" ];
    for (int i = 0; i < 5; i++)
        [self item:fltNames[i] action:@selector(chooseFilter:) key:[NSString stringWithFormat:@"%d", i + 1]
              mods:NSEventModifierFlagCommand | NSEventModifierFlagOption menu:fltm].tag = i;
    [fltm addItem:[NSMenuItem separatorItem]];
    [self item:@"Next Filter" action:@selector(nextFilter:) key:@"t" mods:C menu:fltm];
    flt.submenu = fltm; [view addItem:flt];
    NSMenuItem *crt = [[NSMenuItem alloc] initWithTitle:@"CRT Settings" action:nil keyEquivalent:@""];
    NSMenu *crtm = [[NSMenu alloc] initWithTitle:@"CRT Settings"];
    [self item:@"Screen Curvature" action:@selector(toggleCurvature:) key:nil mods:0 menu:crtm];
    [crtm addItem:[NSMenuItem separatorItem]];
    NSArray *scanNames = @[ @"Scanlines: Light", @"Scanlines: Medium", @"Scanlines: Strong" ];
    for (int i = 0; i < 3; i++) [self item:scanNames[i] action:@selector(chooseScan:) key:nil mods:0 menu:crtm].tag = i;
    [crtm addItem:[NSMenuItem separatorItem]];
    NSArray *maskNames = @[ @"Mask: None", @"Mask: Aperture Grille", @"Mask: Slot Mask" ];
    for (int i = 0; i < 3; i++) [self item:maskNames[i] action:@selector(chooseMask:) key:nil mods:0 menu:crtm].tag = i;
    crt.submenu = crtm; [view addItem:crt];
    [view addItem:[NSMenuItem separatorItem]];

    NSMenuItem *fs = [[NSMenuItem alloc] initWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    [view addItem:fs];
    viewItem.submenu = view;

    NSMenuItem *palItem = [NSMenuItem new]; [bar addItem:palItem];
    NSMenu *pal = [[NSMenu alloc] initWithTitle:@"Palette"];
    [self item:@"Built-in NTSC Palette" action:@selector(useBuiltinPalette:) key:nil mods:0 menu:pal];
    self.customPalItem = [self item:@"Custom Palette" action:@selector(useCustomPalette:) key:nil mods:0 menu:pal];
    [pal addItem:[NSMenuItem separatorItem]];
    [self item:@"Load Palette File (.pal)…" action:@selector(loadPalette:) key:nil mods:0 menu:pal];
    [self item:@"Export Palette (.pal)…" action:@selector(exportPalette:) key:nil mods:0 menu:pal];
    [pal addItem:[NSMenuItem separatorItem]];
    [self item:@"Adjust Colors…" action:@selector(showColorPanel:) key:@"c" mods:NSEventModifierFlagCommand | NSEventModifierFlagOption menu:pal];
    [self item:@"Reset Color Adjustments" action:@selector(resetColors:) key:nil mods:0 menu:pal];
    palItem.submenu = pal;

    NSMenuItem *toolsItem = [NSMenuItem new]; [bar addItem:toolsItem];
    NSMenu *tools = [[NSMenu alloc] initWithTitle:@"Tools"];
    [self item:@"RAM Editor…" action:@selector(showRamEditor:) key:@"e" mods:NSEventModifierFlagCommand | NSEventModifierFlagOption menu:tools];
    [self item:@"Corruptor…" action:@selector(showCorruptor:) key:@"k" mods:NSEventModifierFlagCommand | NSEventModifierFlagOption menu:tools];
    toolsItem.submenu = tools;

    NSMenuItem *winItem = [NSMenuItem new]; [bar addItem:winItem];
    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    [win addItem:[[NSMenuItem alloc] initWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"]];
    winItem.submenu = win;
    [NSApp setWindowsMenu:win];

    NSMenuItem *helpItem = [NSMenuItem new]; [bar addItem:helpItem];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    [self item:@"Keyboard Shortcuts…" action:@selector(showControls:) key:@"?" mods:C menu:help];
    helpItem.submenu = help;

    [NSApp setMainMenu:bar];
}

- (BOOL)validateMenuItem:(NSMenuItem *)it {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    SEL a = it.action;
    BOOL loaded = atomic_load(&g_running);
    if (a == @selector(togglePause:)) { it.title = atomic_load(&g_paused) ? @"Resume" : @"Pause"; return loaded; }
    BOOL real = [ud boolForKey:@"realCart"];
    if (a == @selector(togglePower:)) {
        it.title = [self isPowered] ? @"Power Off" : @"Power On";
        return [self isPowered] || g_nes.loaded;
    }
    if (a == @selector(blowOnCart:)) {
        it.title = _blowing ? [NSString stringWithFormat:@"Blowing on Cartridge… %d", _blowLeft] : @"Blow on Cartridge";
        return real && g_nes.loaded && ![self isPowered] && !_blowing;
    }
    if (a == @selector(toggleRealCart:)) { it.state = real ? NSControlStateValueOn : NSControlStateValueOff; return YES; }
    if (a == @selector(closeROM:)) { it.title = real ? @"Eject Cartridge" : @"Close ROM"; return g_nes.loaded; }
    if (a == @selector(softReset:) || a == @selector(hardReset:) || a == @selector(saveState:) || a == @selector(loadState:) ||
        a == @selector(screenshot:)) return loaded;
    if (a == @selector(selectSlot:)) { it.state = it.tag == self.stateSlot ? NSControlStateValueOn : NSControlStateValueOff; return YES; }
    if (a == @selector(chooseScale:)) { it.state = [ud integerForKey:@"scale"] == it.tag ? NSControlStateValueOn : NSControlStateValueOff; return YES; }
    if (a == @selector(chooseAspect:)) it.state = self.view.aspectMode == it.tag ? NSControlStateValueOn : NSControlStateValueOff;
    if (a == @selector(toggleCrop:)) it.state = self.view.cropOverscan ? NSControlStateValueOn : NSControlStateValueOff;
    if (a == @selector(chooseFilter:)) { it.state = self.view.filter == it.tag ? NSControlStateValueOn : NSControlStateValueOff; return it.tag <= FILTER_SMOOTH || self.view.metal; }
    if (a == @selector(toggleCurvature:)) { it.state = self.view.crtCurvature ? NSControlStateValueOn : NSControlStateValueOff; return self.view.metal; }
    if (a == @selector(chooseScan:)) { it.state = fabsf(self.view.crtScan - kScan[it.tag]) < 0.01f ? NSControlStateValueOn : NSControlStateValueOff; return self.view.metal; }
    if (a == @selector(chooseMask:)) { it.state = self.view.crtMask == it.tag ? NSControlStateValueOn : NSControlStateValueOff; return self.view.metal; }
    if (a == @selector(useBuiltinPalette:)) it.state = g_pal_custom ? NSControlStateValueOff : NSControlStateValueOn;
    if (a == @selector(useCustomPalette:)) {
        NSString *nm = [ud stringForKey:@"customPalName"];
        it.title = nm ? [NSString stringWithFormat:@"Custom: %@", nm] : @"Custom Palette (none loaded)";
        it.state = g_pal_custom ? NSControlStateValueOn : NSControlStateValueOff;
        return [ud dataForKey:@"customPal"] != nil;
    }

    if (a == @selector(togglePauseBG:)) it.state = [ud boolForKey:@"pauseBG"] ? NSControlStateValueOn : NSControlStateValueOff;
    if (a == @selector(toggleOpposing:)) it.state = atomic_load(&g_allow_opposing) ? NSControlStateValueOn : NSControlStateValueOff;
    if (a == @selector(chooseOverclock:)) it.state = g_nes.oc_lines == it.tag ? NSControlStateValueOn : NSControlStateValueOff;
    if (a == @selector(chooseAudioFilter:)) it.state = atomic_load(&g_midi_preset) < 0 && g_nes.audio_filter == it.tag ? NSControlStateValueOn : NSControlStateValueOff;
    if (a == @selector(chooseMidi:)) { it.state = atomic_load(&g_midi_preset) == it.tag ? NSControlStateValueOn : NSControlStateValueOff; return g_synth != NULL; }
    if (a == @selector(builtinSoundFont:)) { it.state = [ud stringForKey:@"soundFont"] ? NSControlStateValueOff : NSControlStateValueOn; return g_synth != NULL; }
    if (a == @selector(loadSoundFont:)) {
        NSString *sf = [ud stringForKey:@"soundFont"];
        it.title = sf ? [NSString stringWithFormat:@"   SoundFont: %@ — Load Another…", [sf lastPathComponent]] : @"   Load SoundFont (.sf2 / .dls)…";
        it.state = sf ? NSControlStateValueOn : NSControlStateValueOff;
        return g_synth != NULL;
    }
    if (a == @selector(toggleMute:)) it.state = atomic_load(&g_muted) ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

/* ---------------------------------------------------------------- toast */
- (void)showToast:(NSString *)msg {
    self.toast.stringValue = [NSString stringWithFormat:@"  %@  ", msg];
    [self.toast sizeToFit];
    NSRect b = self.view.bounds;
    self.toast.frame = NSMakeRect(12, b.size.height - self.toast.frame.size.height - 12, self.toast.frame.size.width, self.toast.frame.size.height);
    self.toast.autoresizingMask = NSViewMaxXMargin | NSViewMinYMargin;
    self.toast.hidden = NO;
    [_toastTimer invalidate];
    _toastTimer = [NSTimer scheduledTimerWithTimeInterval:2.5 repeats:NO block:^(NSTimer *t) { self.toast.hidden = YES; }];
}

/* ---------------------------------------------------------------- frame tick */
- (void)tick:(NSTimer *)t {
    int seq = atomic_load(&g_frame_seq);
    if (seq != _lastSeq) { _lastSeq = seq; [self.view showFrame]; }
    [self pollPads];
    double now = now_sec();
    if (now - _lastSramCheck > 5.0) { _lastSramCheck = now; [self saveSRAMIfChanged]; }
}

- (void)clearScreen {
    for (int b = 0; b < 3; b++) for (int i = 0; i < 256 * 240; i++) g_idx[b][i] = 0x0F;
    [self.view showFrame];
}

/* ---------------------------------------------------------------- input */
- (void)keyEvent:(NSEvent *)e down:(BOOL)down {
    if (e.modifierFlags & NSEventModifierFlagCommand) return;
    if (down && e.isARepeat) return;
    if (e.keyCode == 48) { atomic_store(&g_fast, down); return; }          /* Tab: fast-forward */
    if (e.keyCode == 53) {                                                  /* Esc: leave full screen */
        if (down && (self.window.styleMask & NSWindowStyleMaskFullScreen)) [self.window toggleFullScreen:nil];
        return;
    }
    if (down) [[InputManager shared] keyDown:e]; else [[InputManager shared] keyUp:e];
    [self updateKeys];
}

- (void)flagsEvent:(NSEvent *)e {
    [[InputManager shared] flagsChanged:e];
    [self updateKeys];
}

- (void)updateKeys {
    InputManager *im = [InputManager shared];
    atomic_store(&g_keys[0], [im keyBitsForPlayer:0]);
    atomic_store(&g_keys[1], [im keyBitsForPlayer:1]);
}

- (void)pollPads {
    InputManager *im = [InputManager shared];
    atomic_store(&g_pads[0], [im padBitsForPlayer:0]);
    atomic_store(&g_pads[1], [im padBitsForPlayer:1]);
}

/* ---------------------------------------------------------------- ROM management */
- (NSString *)baseName { return [[self.romPath lastPathComponent] stringByDeletingPathExtension]; }
- (NSString *)sramPath { return [support_dir(@"Saves") stringByAppendingPathComponent:[[self baseName] stringByAppendingPathExtension:@"sav"]]; }
- (NSString *)statePath:(int)slot {
    return [support_dir(@"States") stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.state%d", [self baseName], slot]];
}

- (void)saveSRAM {
    if (tools_sram_blocked()) return;      /* never let a corrupted game overwrite the save */
    if (!g_nes.loaded || !g_nes.cart.battery || !g_nes.cart.wram) return;
    pthread_mutex_lock(&g_lock);
    NSData *d = [NSData dataWithBytes:g_nes.cart.wram length:g_nes.cart.wram_size];
    pthread_mutex_unlock(&g_lock);
    [d writeToFile:[self sramPath] atomically:YES];
    self.savedWram = d;
}

- (void)saveSRAMIfChanged {
    if (!g_nes.loaded || !g_nes.cart.battery || !g_nes.cart.wram) return;
    pthread_mutex_lock(&g_lock);
    BOOL changed = !self.savedWram || self.savedWram.length != g_nes.cart.wram_size ||
                   memcmp(self.savedWram.bytes, g_nes.cart.wram, g_nes.cart.wram_size) != 0;
    pthread_mutex_unlock(&g_lock);
    if (changed) [self saveSRAM];
}

- (BOOL)openROM:(NSString *)path {
    char err[256] = "", inner[256];
    size_t len = 0;
    uint8_t *data = romfile_load(path.fileSystemRepresentation, &len, inner, sizeof inner, err, sizeof err);
    if (!data) { [self alert:@"Couldn't open ROM" info:[NSString stringWithUTF8String:err]]; return NO; }
    BOOL real = [[NSUserDefaults standardUserDefaults] boolForKey:@"realCart"];
    if (real && [self isPowered]) {
        /* like the real console: no swapping cartridges with the power on */
        free(data);
        NSBeep();
        [self showToast:@"Turn the power off first (⌥⌘P), then insert the new cartridge"];
        return NO;
    }
    [self saveSRAM];
    atomic_store(&g_running, false);
    pthread_mutex_lock(&g_lock);
    bool ok = nes_load(&g_nes, data, len, err, sizeof err);
    free(data);
    if (ok) {
        self.romPath = path;
        if (g_nes.cart.battery && g_nes.cart.wram) {
            NSData *s = [NSData dataWithContentsOfFile:[self sramPath]];
            if (s) memcpy(g_nes.cart.wram, s.bytes, MIN(s.length, (NSUInteger)g_nes.cart.wram_size));
            self.savedWram = [NSData dataWithBytes:g_nes.cart.wram length:g_nes.cart.wram_size];
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (!ok) { [self alert:@"Couldn't start this game" info:[NSString stringWithUTF8String:err]]; return NO; }
    self.romName = [[path lastPathComponent] stringByDeletingPathExtension];
    self.window.title = self.romName;
    [self.window setRepresentedURL:[NSURL fileURLWithPath:path]];
    [[CheatManager shared] loadForGame:[NSString stringWithFormat:@"%08X", g_nes.cart.crc] name:self.romName];
    [[ToolsManager shared] romChanged];
    atomic_store(&g_paused, false);
    atomic_store(&g_midi_resync, true);
    [self addRecent:path];
    [self.window makeFirstResponder:self.view];
    if (real) {
        _cleaned = NO;          /* a freshly inserted cart takes its chances */
        [self clearScreen];
        [self updateTitle];
        [self showToast:@"Cartridge inserted · press Power (⌥⌘P) to turn the NES on"];
        return YES;
    }
    atomic_store(&g_running, true);
    int gg = [[CheatManager shared] activeCount];
    [self showToast:[NSString stringWithFormat:@"Mapper %d%@%@", g_nes.cart.mapper, g_nes.cart.battery ? @" · battery save" : @"",
                     gg ? [NSString stringWithFormat:@" · %d Game Genie code%@ on", gg, gg == 1 ? @"" : @"s"] : @""]];
    return YES;
}

- (void)openDocument:(id)sender {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.allowedFileTypes = @[@"nes", @"NES", @"zip", @"ZIP"];
    p.allowsMultipleSelection = NO;
    if ([p runModal] == NSModalResponseOK) [self openROM:p.URL.path];
}

- (void)closeROM:(id)sender {
    if ([[NSUserDefaults standardUserDefaults] boolForKey:@"realCart"] && [self isPowered]) {
        NSBeep();
        [self showToast:@"Turn the power off first (⌥⌘P), then eject the cartridge"];
        return;
    }
    BOOL had = g_nes.loaded;
    [self saveSRAM];
    atomic_store(&g_running, false);
    pthread_mutex_lock(&g_lock);
    nes_unload(&g_nes);
    pthread_mutex_unlock(&g_lock);
    [[CheatManager shared] loadForGame:nil name:nil];
    [[ToolsManager shared] romChanged];
    self.romPath = nil;
    self.window.title = APP_NAME;
    [self.window setRepresentedURL:nil];
    [self clearScreen];
    if (had && [[NSUserDefaults standardUserDefaults] boolForKey:@"realCart"]) [self showToast:@"Cartridge ejected"];
}

/* ---- realistic cartridge handling: insert with the power off, then power on */
- (void)updateTitle {
    if (!g_nes.loaded) { self.window.title = APP_NAME; return; }
    BOOL off = [[NSUserDefaults standardUserDefaults] boolForKey:@"realCart"] && ![self isPowered];
    if (_badContact) self.window.title = [NSString stringWithFormat:@"%@ — Power light blinking", self.romName];
    else self.window.title = off ? [NSString stringWithFormat:@"%@ — Power Off", self.romName] : self.romName;
}
- (BOOL)isPowered { return atomic_load(&g_running) || _badContact; }

- (void)fillScreen:(uint16_t)c {
    for (int b = 0; b < 3; b++) for (int i = 0; i < 256 * 240; i++) g_idx[b][i] = c;
    [self.view showFrame];
}

- (void)powerOn {
    if ([[NSUserDefaults standardUserDefaults] boolForKey:@"realCart"]) {
        BOOL dirty = _dirtyCart && [_dirtyCart isEqualToString:self.romPath];
        BOOL fail = !_cleaned && (dirty || arc4random_uniform(100) < 30);
        _cleaned = NO;
        if (fail) _dirtyCart = [self.romPath copy];   /* stays dirty, even if ejected and reinserted */
        if (fail) {
            /* dirty contacts: the lockout chip keeps resetting the console, so the
             * screen blinks and the game never starts */
            _badContact = YES;
            _blinkOn = NO;
            [_blinkTimer invalidate];
            _blinkTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
                self->_blinkOn = !self->_blinkOn;
                [self fillScreen:self->_blinkOn ? 0x00 : 0x0F];
            }];
            [self fillScreen:0x00];
            [self updateTitle];
            [self showToast:@"The cartridge isn't making good contact. Power off, then Blow on Cartridge (⌥⌘B)"];
            return;
        }
    }
    pthread_mutex_lock(&g_lock);
    nes_power(&g_nes);      /* a real power-on: fresh RAM, every chip reset */
    if (g_nes.cart.battery && self.savedWram)
        memcpy(g_nes.cart.wram, self.savedWram.bytes, MIN(self.savedWram.length, (NSUInteger)g_nes.cart.wram_size));
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_paused, false);
    atomic_store(&g_running, true);
    [self updateTitle];
    [self.window makeFirstResponder:self.view];
    int gg = [[CheatManager shared] activeCount];
    [self showToast:[NSString stringWithFormat:@"Power on%@", gg ? [NSString stringWithFormat:@" · %d Game Genie code%@ on", gg, gg == 1 ? @"" : @"s"] : @""]];
}
- (void)powerOff {
    if (_badContact) {
        _badContact = NO;
        [_blinkTimer invalidate]; _blinkTimer = nil;
        [self clearScreen];
        [self updateTitle];
        [self showToast:@"Power off"];
        return;
    }
    [self saveSRAM];
    atomic_store(&g_running, false);
    [self clearScreen];
    [self updateTitle];
    [self showToast:@"Power off"];
}
- (void)togglePower:(id)s {
    if ([self isPowered]) { [self powerOff]; return; }
    if (!g_nes.loaded) { NSBeep(); [self showToast:@"No cartridge inserted. Open a ROM with ⌘O"]; return; }
    if (_blowing) { NSBeep(); [self showToast:@"Still blowing on the cartridge…"]; return; }
    [self powerOn];
}
- (void)blowOnCart:(id)s {
    if (!g_nes.loaded || _blowing) return;
    if ([self isPowered]) { NSBeep(); [self showToast:@"Turn the power off first (⌥⌘P)"]; return; }
    _blowing = YES;
    _blowLeft = 5;
    [self showToast:@"Blowing on the cartridge… 5"];
    _blowTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
        if (--self->_blowLeft > 0) { [self showToast:[NSString stringWithFormat:@"Blowing on the cartridge… %d", self->_blowLeft]]; return; }
        [t invalidate];
        self->_blowTimer = nil;
        self->_blowing = NO;
        self->_cleaned = YES;
        self->_dirtyCart = nil;
        [self showToast:@"Cartridge cleaned and reinserted. Press Power (⌥⌘P)"];
    }];
}

- (void)toggleRealCart:(id)s {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    BOOL v = ![ud boolForKey:@"realCart"];
    [ud setBool:v forKey:@"realCart"];
    if (!v && _badContact) [self powerOff];
    if (!v && g_nes.loaded && !atomic_load(&g_running)) [self powerOn];   /* leaving the mode: a waiting cart starts */
    [self updateTitle];
    [self showToast:v ? @"Realistic cartridge handling on: insert a cartridge, then press Power (⌥⌘P)" : @"Realistic cartridge handling off"];
}

- (void)addRecent:(NSString *)path {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    NSMutableArray *r = [[ud arrayForKey:@"recent"] ?: @[] mutableCopy];
    [r removeObject:path];
    [r insertObject:path atIndex:0];
    while (r.count > 12) [r removeLastObject];
    [ud setObject:r forKey:@"recent"];
    [self rebuildRecent];
}
- (void)rebuildRecent {
    [self.recentMenu removeAllItems];
    NSArray *r = [[NSUserDefaults standardUserDefaults] arrayForKey:@"recent"];
    for (NSString *p in r) {
        NSMenuItem *it = [self item:[[p lastPathComponent] stringByDeletingPathExtension] action:@selector(openRecent:) key:nil mods:0 menu:self.recentMenu];
        it.representedObject = p;
        it.toolTip = p;
    }
    if (r.count) [self.recentMenu addItem:[NSMenuItem separatorItem]];
    [self item:@"Clear Menu" action:@selector(clearRecent:) key:nil mods:0 menu:self.recentMenu];
}
- (void)openRecent:(NSMenuItem *)it { [self openROM:it.representedObject]; }
- (void)clearRecent:(id)s { [[NSUserDefaults standardUserDefaults] setObject:@[] forKey:@"recent"]; [self rebuildRecent]; }

/* ---------------------------------------------------------------- actions */
- (void)togglePause:(id)s {
    atomic_store(&g_paused, !atomic_load(&g_paused));
    [self showToast:atomic_load(&g_paused) ? @"Paused" : @"Resumed"];
}
- (void)softReset:(id)s {
    pthread_mutex_lock(&g_lock); nes_reset(&g_nes); pthread_mutex_unlock(&g_lock);
    [self showToast:@"Reset"];
}
- (void)hardReset:(id)s {
    [self saveSRAM];
    pthread_mutex_lock(&g_lock);
    nes_power(&g_nes);
    if (g_nes.cart.battery && self.savedWram) memcpy(g_nes.cart.wram, self.savedWram.bytes, MIN(self.savedWram.length, (NSUInteger)g_nes.cart.wram_size));
    pthread_mutex_unlock(&g_lock);
    [self showToast:@"Power cycled"];
}
- (void)selectSlot:(NSMenuItem *)it { self.stateSlot = (int)it.tag; [self showToast:[NSString stringWithFormat:@"State slot %d", self.stateSlot]]; }
- (void)saveState:(id)s {
    size_t n = nes_state_size();
    NSMutableData *d = [NSMutableData dataWithLength:n];
    pthread_mutex_lock(&g_lock);
    bool ok = nes_save_state(&g_nes, d.mutableBytes, n);
    pthread_mutex_unlock(&g_lock);
    if (ok && [d writeToFile:[self statePath:self.stateSlot] atomically:YES])
        [self showToast:[NSString stringWithFormat:@"Saved state %d", self.stateSlot]];
    else [self showToast:@"Couldn't save state"];
}
- (void)loadState:(id)s {
    NSData *d = [NSData dataWithContentsOfFile:[self statePath:self.stateSlot]];
    if (!d) { [self showToast:[NSString stringWithFormat:@"State %d is empty", self.stateSlot]]; return; }
    pthread_mutex_lock(&g_lock);
    bool ok = nes_load_state(&g_nes, d.bytes, d.length);
    pthread_mutex_unlock(&g_lock);
    [self showToast:ok ? [NSString stringWithFormat:@"Loaded state %d", self.stateSlot] : @"That state is from a different game or version"];
}
- (void)screenshot:(id)s {
    int buf = g_disp_buf;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:256 pixelsHigh:240 bitsPerSample:8
                              samplesPerPixel:3 hasAlpha:NO isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:256 * 3 bitsPerPixel:24];
    uint8_t *px = rep.bitmapData;
    for (int i = 0; i < 256 * 240; i++) { uint32_t c = g_pal[g_idx[buf][i] & 511]; px[i * 3] = (uint8_t)(c >> 16); px[i * 3 + 1] = (uint8_t)(c >> 8); px[i * 3 + 2] = (uint8_t)c; }
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSString *dir = [NSSearchPathForDirectoriesInDomains(NSPicturesDirectory, NSUserDomainMask, YES) firstObject];
    NSDateFormatter *f = [NSDateFormatter new]; f.dateFormat = @"yyyy-MM-dd 'at' HH.mm.ss";
    NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%@ %@.png", self.romName ?: APP_NAME, [f stringFromDate:[NSDate date]]]];
    [self showToast:[png writeToFile:path atomically:YES] ? @"Screenshot saved to Pictures" : @"Couldn't save screenshot"];
}
- (void)showSaves:(id)s { [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:support_dir(nil)]]; }
- (void)togglePauseBG:(id)s { NSUserDefaults *ud = [NSUserDefaults standardUserDefaults]; [ud setBool:![ud boolForKey:@"pauseBG"] forKey:@"pauseBG"]; }
- (void)toggleOpposing:(id)s {
    bool v = !atomic_load(&g_allow_opposing); atomic_store(&g_allow_opposing, v);
    [[NSUserDefaults standardUserDefaults] setBool:v forKey:@"opposing"];
}
- (void)chooseOverclock:(NSMenuItem *)it {
    pthread_mutex_lock(&g_lock);
    g_nes.oc_lines = (int)it.tag;
    pthread_mutex_unlock(&g_lock);
    [[NSUserDefaults standardUserDefaults] setInteger:it.tag forKey:@"ocLines"];
    [self showToast:it.tag ? [NSString stringWithFormat:@"Overclock: %@", it.title] : @"Overclock off"];
}
- (void)chooseAudioFilter:(NSMenuItem *)it {
    atomic_store(&g_midi_preset, -1);
    [[NSUserDefaults standardUserDefaults] setInteger:-1 forKey:@"midiPreset"];
    pthread_mutex_lock(&g_lock);
    g_nes.audio_filter = (int)it.tag;
    for (int i = 0; i < 4; i++) g_nes.af_state[i] = 0;
    pthread_mutex_unlock(&g_lock);
    [[NSUserDefaults standardUserDefaults] setInteger:it.tag forKey:@"audioFilter"];
    [self showToast:[NSString stringWithFormat:@"Sound: %@", it.title]];
}
- (void)chooseMidi:(NSMenuItem *)it {
    if (!g_synth) return;
    atomic_store(&g_midi_preset, (int)it.tag);
    [[NSUserDefaults standardUserDefaults] setInteger:it.tag forKey:@"midiPreset"];
    [self showToast:[NSString stringWithFormat:@"Sound: MIDI soundfont (%@)", kMidiPresetNames[it.tag]]];
}
- (BOOL)applySoundFont:(NSString *)path {
    if (!g_synth) return NO;
    OSStatus st;
    if (path) {
        CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:path];
        st = AudioUnitSetProperty(g_synth, kMusicDeviceProperty_SoundBankURL, kAudioUnitScope_Global, 0, &url, sizeof url);
    } else {
        /* back to the default GM bank: reloading the synth is the reliable way */
        st = noErr;
        NSString *def = @"/System/Library/Components/CoreAudio.component/Contents/Resources/gs_instruments.dls";
        if ([[NSFileManager defaultManager] fileExistsAtPath:def]) {
            CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:def];
            st = AudioUnitSetProperty(g_synth, kMusicDeviceProperty_SoundBankURL, kAudioUnitScope_Global, 0, &url, sizeof url);
        }
    }
    atomic_store(&g_midi_resync, true);
    return st == noErr;
}
- (void)loadSoundFont:(id)s {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.allowedFileTypes = @[@"sf2", @"SF2", @"dls", @"DLS"];
    if ([p runModal] != NSModalResponseOK) return;
    if (![self applySoundFont:p.URL.path]) { [self alert:@"Couldn't load that soundfont" info:@"oceanNES can use SoundFont 2 (.sf2) and DLS (.dls) files."]; return; }
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    [ud setObject:p.URL.path forKey:@"soundFont"];
    if (atomic_load(&g_midi_preset) < 0) { atomic_store(&g_midi_preset, 0); [ud setInteger:0 forKey:@"midiPreset"]; }
    [self showToast:[NSString stringWithFormat:@"SoundFont: %@", [p.URL.path lastPathComponent]]];
}
- (void)builtinSoundFont:(id)s {
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"soundFont"];
    [self applySoundFont:nil];
    [self showToast:@"SoundFont: built-in macOS General MIDI"];
}
- (void)toggleMute:(id)s { atomic_store(&g_muted, !atomic_load(&g_muted)); [self showToast:atomic_load(&g_muted) ? @"Muted" : @"Sound on"]; }

- (void)applyScale:(int)s {
    if (s < 1) s = 1;
    if (s > 6) s = 6;
    NSSize b = [self.view baseSize];
    if (self.window.styleMask & NSWindowStyleMaskFullScreen) return;
    NSRect f = self.window.frame;
    NSRect c = [self.window contentRectForFrameRect:f];
    double top = NSMaxY(c);
    c.size = NSMakeSize(round(b.width * s), round(b.height * s));
    c.origin.y = top - c.size.height;
    [self.window setFrame:[self.window frameRectForContentRect:c] display:YES animate:NO];
    self.window.contentMinSize = b;
    [self.view relayoutImage];
}
- (void)chooseScale:(NSMenuItem *)it {
    [[NSUserDefaults standardUserDefaults] setInteger:it.tag forKey:@"scale"];
    [self applyScale:(int)it.tag];
}
- (void)rescale { [self applyScale:(int)[[NSUserDefaults standardUserDefaults] integerForKey:@"scale"]]; [self.view redraw]; }
- (void)chooseAspect:(NSMenuItem *)it {
    self.view.aspectMode = (int)it.tag;
    [[NSUserDefaults standardUserDefaults] setInteger:it.tag forKey:@"aspectMode"];
    [self rescale];
}
- (void)toggleCrop:(id)s {
    self.view.cropOverscan = !self.view.cropOverscan;
    [[NSUserDefaults standardUserDefaults] setBool:self.view.cropOverscan forKey:@"crop"];
    [self rescale];
}

/* ---------------------------------------------------------------- video filters */
static NSString *const kFilterNames[5] = { @"Sharp Pixels", @"Smooth", @"CRT", @"NTSC Composite", @"NTSC Composite + CRT" };
- (void)setFilter:(int)f {
    if (!self.view.metal && f > FILTER_SMOOTH) { [self showToast:@"CRT and NTSC filters need a Metal-capable Mac"]; return; }
    self.view.filter = f;
    [[NSUserDefaults standardUserDefaults] setInteger:f forKey:@"filter"];
    [self.view relayoutImage];
    [self.view redraw];
    [self showToast:kFilterNames[f]];
}
- (void)chooseFilter:(NSMenuItem *)it { [self setFilter:(int)it.tag]; }
- (void)nextFilter:(id)s {
    int n = self.view.metal ? 5 : 2;
    [self setFilter:(self.view.filter + 1) % n];
}
- (void)toggleCurvature:(id)s {
    self.view.crtCurvature = !self.view.crtCurvature;
    [[NSUserDefaults standardUserDefaults] setBool:self.view.crtCurvature forKey:@"crtCurvature"];
    [self.view redraw];
}
- (void)chooseScan:(NSMenuItem *)it {
    self.view.crtScan = kScan[it.tag];
    [[NSUserDefaults standardUserDefaults] setFloat:self.view.crtScan forKey:@"crtScan"];
    [self.view redraw];
}
- (void)chooseMask:(NSMenuItem *)it {
    self.view.crtMask = (int)it.tag;
    [[NSUserDefaults standardUserDefaults] setInteger:it.tag forKey:@"crtMask"];
    [self.view redraw];
}

/* ---------------------------------------------------------------- palettes */
/* applies hue / saturation / brightness / contrast to an RGB colour (YIQ space) */
static uint32_t adjust_rgb(uint32_t c, double hue_deg, double sat, double bri, double con) {
    double r = ((c >> 16) & 255) / 255.0, g = ((c >> 8) & 255) / 255.0, b = (c & 255) / 255.0;
    double y = 0.299 * r + 0.587 * g + 0.114 * b;
    double i = 0.595716 * r - 0.274453 * g - 0.321263 * b;
    double q = 0.211456 * r - 0.522591 * g + 0.311135 * b;
    double a = hue_deg * M_PI / 180.0, ca = cos(a), sa = sin(a);
    double i2 = (i * ca - q * sa) * sat, q2 = (i * sa + q * ca) * sat;
    double rgb[3] = { y + 0.956 * i2 + 0.621 * q2, y - 0.272 * i2 - 0.647 * q2, y - 1.106 * i2 + 1.703 * q2 };
    uint32_t out = 0xFF000000u;
    for (int k = 0; k < 3; k++) {
        double v = (rgb[k] - 0.5) * con + 0.5 + bri;
        int n = (int)(v * 255.0 + 0.5); n = n < 0 ? 0 : n > 255 ? 255 : n;
        out |= (uint32_t)n << (16 - 8 * k);
    }
    return out;
}

/* turns a .pal file into 512 entries; 64-colour files get the emphasis
 * variants derived the usual way (non-emphasised channels dimmed) */
static BOOL parse_pal(NSData *d, uint32_t *out) {
    const uint8_t *p = d.bytes;
    if (d.length >= 1536) {
        for (int i = 0; i < 512; i++) out[i] = 0xFF000000u | (uint32_t)p[i * 3] << 16 | (uint32_t)p[i * 3 + 1] << 8 | p[i * 3 + 2];
        return YES;
    }
    if (d.length < 192) return NO;
    for (int e = 0; e < 8; e++) for (int c = 0; c < 64; c++) {
        double ch[3] = { p[c * 3], p[c * 3 + 1], p[c * 3 + 2] };
        for (int k = 0; k < 3; k++) {
            /* emphasis bit 0 = red, 1 = green, 2 = blue: other channels are attenuated */
            int others = e & ~(1 << k);
            if (others) ch[k] *= 0.816;
        }
        out[e * 64 + c] = 0xFF000000u | (uint32_t)(ch[0] + 0.5) << 16 | (uint32_t)(ch[1] + 0.5) << 8 | (uint32_t)(ch[2] + 0.5);
    }
    return YES;
}

- (void)rebuildPalette {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    NesView *v = self.view;
    NSData *d = [ud boolForKey:@"useCustomPal"] ? [ud dataForKey:@"customPal"] : nil;
    uint32_t base[512];
    if (d && parse_pal(d, base)) {
        g_pal_custom = true;
        for (int i = 0; i < 512; i++) g_pal[i] = adjust_rgb(base[i], v.hue, v.sat, v.bri, v.con);
    } else {
        g_pal_custom = false;
        palette_generate_ex(g_pal, v.hue, v.sat, v.bri, v.con);
    }
    g_pal_ver++;
    [v redraw];
}

- (BOOL)loadPaletteFile:(NSString *)path {
    NSData *d = [NSData dataWithContentsOfFile:path];
    uint32_t tmp[512];
    if (!d || !parse_pal(d, tmp)) {
        [self alert:@"Couldn't load palette" info:@"A .pal file must hold 64 colours (192 bytes) or 512 colours with emphasis (1536 bytes)."];
        return NO;
    }
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    [ud setObject:[d subdataWithRange:NSMakeRange(0, d.length >= 1536 ? 1536 : 192)] forKey:@"customPal"];
    [ud setObject:[[path lastPathComponent] stringByDeletingPathExtension] forKey:@"customPalName"];
    [ud setBool:YES forKey:@"useCustomPal"];
    [self rebuildPalette];
    [self showToast:[NSString stringWithFormat:@"Palette: %@", [[path lastPathComponent] stringByDeletingPathExtension]]];
    return YES;
}
- (void)loadPalette:(id)s {
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.allowedFileTypes = @[@"pal", @"PAL"];
    if ([p runModal] == NSModalResponseOK) [self loadPaletteFile:p.URL.path];
}
- (void)useBuiltinPalette:(id)s {
    [[NSUserDefaults standardUserDefaults] setBool:NO forKey:@"useCustomPal"];
    [self rebuildPalette];
    [self showToast:@"Palette: built-in NTSC"];
}
- (void)useCustomPalette:(id)s {
    [[NSUserDefaults standardUserDefaults] setBool:YES forKey:@"useCustomPal"];
    [self rebuildPalette];
}
- (void)exportPalette:(id)s {
    NSSavePanel *p = [NSSavePanel savePanel];
    p.allowedFileTypes = @[@"pal"];
    p.nameFieldStringValue = @"oceanNES.pal";
    if ([p runModal] != NSModalResponseOK) return;
    uint8_t b[1536];
    for (int i = 0; i < 512; i++) { b[i * 3] = (uint8_t)(g_pal[i] >> 16); b[i * 3 + 1] = (uint8_t)(g_pal[i] >> 8); b[i * 3 + 2] = (uint8_t)g_pal[i]; }
    BOOL ok = [[NSData dataWithBytes:b length:sizeof b] writeToURL:p.URL atomically:YES];
    [self showToast:ok ? @"Palette exported (512 colours)" : @"Couldn't export palette"];
}

/* colour adjustment panel: hue, saturation, brightness, contrast */
- (void)showColorPanel:(id)s {
    if (!self.colorPanel) {
        NSPanel *p = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 380, 214)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow
                                                  backing:NSBackingStoreBuffered defer:NO];
        p.title = @"Adjust Colors";
        p.releasedWhenClosed = NO;
        p.hidesOnDeactivate = YES;
        p.becomesKeyOnlyIfNeeded = YES;
        NSView *cv = p.contentView;
        NSArray *names = @[ @"Hue", @"Saturation", @"Brightness", @"Contrast" ];
        double mins[4] = { -45, 0, -0.3, 0.5 }, maxs[4] = { 45, 2, 0.3, 1.5 };
        NSMutableArray *sl = [NSMutableArray array];
        for (int i = 0; i < 4; i++) {
            double y = 170 - i * 38;
            NSTextField *l = [NSTextField labelWithString:names[i]];
            l.frame = NSMakeRect(16, y, 84, 20);
            [cv addSubview:l];
            NSSlider *sd = [NSSlider sliderWithValue:0 minValue:mins[i] maxValue:maxs[i] target:self action:@selector(colorSliderChanged:)];
            sd.frame = NSMakeRect(104, y, 260, 22);
            sd.continuous = YES;
            sd.tag = i;
            [cv addSubview:sd];
            [sl addObject:sd];
        }
        NSButton *reset = [NSButton buttonWithTitle:@"Reset" target:self action:@selector(resetColors:)];
        reset.frame = NSMakeRect(274, 12, 90, 30);
        [cv addSubview:reset];
        NSTextField *hint = [NSTextField labelWithString:@"Applies to every palette and filter."];
        hint.font = [NSFont systemFontOfSize:11];
        hint.textColor = [NSColor secondaryLabelColor];
        hint.frame = NSMakeRect(16, 18, 250, 18);
        [cv addSubview:hint];
        self.colorSliders = sl;
        self.colorPanel = p;
        NSRect wf = self.window.frame;
        [p setFrameTopLeftPoint:NSMakePoint(NSMaxX(wf) + 12, NSMaxY(wf))];
    }
    [self syncColorSliders];
    [self.colorPanel orderFront:nil];
}
- (void)syncColorSliders {
    NesView *v = self.view;
    double vals[4] = { v.hue, v.sat, v.bri, v.con };
    for (NSSlider *sd in self.colorSliders) sd.doubleValue = vals[sd.tag];
}
- (void)colorSliderChanged:(NSSlider *)sd {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    float val = sd.floatValue;
    switch (sd.tag) {
    case 0: self.view.hue = val; [ud setFloat:val forKey:@"hue"]; break;
    case 1: self.view.sat = val; [ud setFloat:val forKey:@"sat"]; break;
    case 2: self.view.bri = val; [ud setFloat:val forKey:@"bri"]; break;
    default: self.view.con = val; [ud setFloat:val forKey:@"con"]; break;
    }
    [self rebuildPalette];
}
- (void)resetColors:(id)s {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    for (NSString *k in @[ @"hue", @"sat", @"bri", @"con" ]) [ud removeObjectForKey:k];
    self.view.hue = 0; self.view.sat = 1; self.view.bri = 0; self.view.con = 1;
    [self syncColorSliders];
    [self rebuildPalette];
}

- (void)alert:(NSString *)title info:(NSString *)info {
    NSAlert *a = [NSAlert new];
    a.messageText = title; a.informativeText = info ?: @"";
    [a runModal];
}
- (void)about:(id)s {
    NSDictionary *opts = @{ @"ApplicationName": APP_NAME, @"Version": @"1.0",
        @"Credits": [[NSAttributedString alloc] initWithString:
            @"A cycle-accurate NES emulator.\n\nCPU, PPU, APU and DMA are emulated at the individual clock level and verified against the blargg test suites and AccuracyCoin."
            attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:11] }] };
    [NSApp orderFrontStandardAboutPanelWithOptions:opts];
}
- (void)showCheats:(id)s { [[CheatManager shared] showWindow]; }
- (void)showRamEditor:(id)s { [[ToolsManager shared] showRamEditor]; }
- (void)showCorruptor:(id)s { [[ToolsManager shared] showCorruptor]; }
- (void)configureControls:(id)s { [[InputManager shared] showConfigWindow]; }
- (void)showControls:(id)s {
    [self alert:@"Keyboard shortcuts"
           info:@"Buttons for both players (keyboard and controllers) are set in oceanNES → Configure Controls… (⌘,).\n\n"
                 "Tab (hold): fast-forward\n"
                 "⌘P pause · ⌘R reset · ⇧⌘R power cycle · ⌥⌘P power on/off\n"
                 "⌘S / ⌘L save / load state · Ctrl-1…9 choose slot\n"
                 "⌥⌘1…5 video filter · ⌘T next filter · ⌥⌘C adjust colours\n"
                 "⌘1…5 window size · Ctrl-⌘F full screen\n"
                 "⌘G Game Genie codes\n"
                 "⇧⌘P screenshot · ⇧⌘M mute"];
}

@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *d = [AppDelegate new];
        app.delegate = d;
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
