=======================================================
oceanNES — a cycle-accurate NES emulator for Intel Macs
=======================================================

oceanNES is a native macOS app (Cocoa + Core Audio, no dependencies) built around an
emulator core that models the NES at the level of individual clock cycles:

- CPU (2A03): every bus access is its own cycle, including all dummy reads and
  writes, exact interrupt polling (CLI/SEI/PLP latency, branch quirks, NMI/IRQ/BRK
  hijacking), all unofficial opcodes (including SHA/SHX/SHY/TAS behaviour under DMA),
  OAM and DMC DMA with halt/dummy/alignment cycles, DMA aborts, and DMA bus
  conflicts with the internal APU/controller registers. Internal and external data
  buses are modelled separately.
- PPU (2C02): dot-by-dot rendering that models the chip's internals: two-dot
  memory accesses through the external address latch (so $2006/$2007 accesses
  mid-render corrupt fetches exactly as on hardware), the $2007 read/write state
  machines, background shifters with their serial inputs, the secondary-OAM address
  counter and its overflow flag, the OAM buffer seen through $2004, per-sprite X
  counters that keep running while rendering is off, sprites on scanline 0, OAM
  row corruption, the composite-PPU odd-frame sprite quirk, delayed $2001 effects,
  exact VBlank/NMI races, odd-frame dot skip and open-bus decay.
- APU: frame counter with exact $4017 timing, length-counter reload/halt
  ordering, DMC with APU-clock-aligned timer and start/stop delays, the hardware
  non-linear mixer and the NES's analog filter chain. VRC6 and Sunsoft 5B expansion
  audio.
- Mixed master clock: CPU and PPU run interleaved on a shared master clock
  (12 / 4 ticks), so mid-instruction PPU state is exact.

ACCURACY RESULTS
----------------

  - blargg CPU: instr_test-v5, instr_misc, instr_timing, cpu_timing_test6,
    cpu_dummy_reads/writes, cpu_exec_space, cpu_interrupts_v2, cpu_reset,
    branch_timing
      all pass
  - blargg PPU: ppu_vbl_nmi, vbl_nmi_timing, ppu_open_bus, ppu_read_buffer,
    oam_read, oam_stress, sprite_hit_tests, sprite_overflow_tests, 2005 PPU tests
      all pass
  - blargg APU: apu_test, apu_reset, 2005 APU tests (incl. len/IRQ timing)
      all pass
  - DMA: dmc_dma_during_read4, sprdma_and_dmc_dma (+512)
      all pass
  - Mappers: mmc3_test_2 (MMC3 rev B)
      all pass
  - 108 automated test ROMs total
      108 / 108
  - AccuracyCoin (147 tests, the strictest current suite)
      147 / 147 — reports "Composite PPU Detected", like a real NES

INSTALLING
----------

1. Unzip and drag oceanNES.app into Applications.
2. The app isn't notarized by Apple, so the first launch needs one extra step:
   - macOS 15 (Sequoia) and newer: double-click oceanNES, click Done, then open
     System Settings → Privacy & Security, scroll down and click Open Anyway.
   - macOS 14 and older: right-click (or Control-click) oceanNES → Open → Open.
   - Or in Terminal: xattr -dr com.apple.quarantine /Applications/oceanNES.app

Requires an Intel Mac with macOS 10.13 or newer. (It also runs on Apple Silicon
through Rosetta.)

USING IT
--------

Open a ROM with ⌘O, by dragging a .nes or .zip file onto the window, or by
double-clicking a .nes file.

Default keys (all changeable in oceanNES → Configure Controls…, ⌘,):

                      Player 1              Player 2
  D-pad               Arrow keys            I J K L
  A / B               X / Z                 H / G
  Turbo A / Turbo B   S / A                 (unset)
  Start / Select      Return / Left Shift   Y / T

Controllers (USB and Bluetooth)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Pair Bluetooth controllers in System Settings → Bluetooth (hold the pad's
pairing button), or plug them in by USB; oceanNES picks them up immediately, also while
running. Supported:

- Anything macOS's GameController framework supports (Xbox, PlayStation
  DualShock 4 / DualSense, Switch Pro, MFi; which ones depends on the macOS version).
- Every other USB or Bluetooth gamepad or joystick through raw HID: 8BitDo in
  D-input / Switch mode, generic Bluetooth pads, SNES-style USB pads, and Xbox and
  PlayStation pads on older macOS versions that don't support them natively.

The first controller is Player 1, the second Player 2. Configure Controls lets you:

- choose which controller each player uses (or keyboard only),
- remap every button by clicking it and pressing the key or controller button
  (Esc cancels, Delete clears); controller layouts are saved per controller model,
- use Turbo A / Turbo B (15 presses a second),
- let the left stick move the D-pad (on by default).

Default controller layout: bottom face button = B, right face button = A,
left = Turbo B, top = Turbo A, Select/View/Share = Select, Start/Menu/Options = Start.

- Tab (hold) — fast-forward
- ⌘P pause, ⌘R reset, ⇧⌘R power cycle, ⌥⌘P power on/off, ⌘G Game Genie codes
- ⌘S / ⌘L save / load state, Ctrl-1…9 choose the slot
- ⌘1…⌘5 window size, Ctrl-⌘F full screen
- ⇧⌘P screenshot (saved to Pictures)
- ⌥⌘1…⌥⌘5 video filter, ⌘T next filter, ⌥⌘C adjust colours

VIDEO: 4:3, CRT, NTSC COMPOSITE AND PALETTES
--------------------------------------------

Every game is shown at 4:3 by default, the shape it had on a TV
(View → Aspect Ratio also offers 8:7 pixel aspect and square pixels).

View → Video Filter

  - Sharp Pixels
      Crisp pixels that stay even at any window size (default)
  - Smooth
      Soft bilinear scaling
  - CRT
      Electron-beam scanlines (bright lines bloom wider), phosphor mask, glow,
      screen curvature, gamma-correct blending
  - NTSC Composite
      Rebuilds the NES's actual composite video signal (8 samples per pixel,
      correct colour-carrier phase per line and per frame) and decodes it like a
      TV: colour bleed, dot crawl and the rainbow artifacts some games rely on
  - NTSC Composite + CRT
      Both: the composite signal shown on a simulated CRT

View → CRT Settings: curvature on/off, scanlines light/medium/strong, mask
none / aperture grille / slot mask.

Palette menu

- Built-in NTSC palette: generated from the same 2C02 signal model as the
  composite filter, including all 8 colour-emphasis variants.
- Load Palette File (.pal): any standard palette. 64-colour files (192 bytes)
  get emphasis variants derived automatically; 512-colour files (1536 bytes) are used
  as-is. You can also drag a .pal file onto the window. The palette is remembered.
- Adjust Colors: hue, saturation, brightness and contrast sliders, live.
  They work with every palette and filter.
- Export Palette: saves the current palette (with your adjustments) as a
  512-colour .pal file.

The filters run on the GPU through Metal. On Macs without Metal (2011 and older) the
app falls back to Sharp/Smooth, and the palette options still work.

Battery saves (Zelda, Final Fantasy, etc.) are written automatically to
~/Library/Application Support/oceanNES/Saves; save states go to .../oceanNES/States.

REALISTIC CARTRIDGE HANDLING
----------------------------

Turn on Emulation → Realistic Cartridge Handling to use oceanNES like the real
console. Opening a ROM (⌘O or drag and drop) inserts the cartridge with the power
off. The screen stays black until you press Emulation → Power On (⌥⌘P), which
powers the NES up from cold, just like flipping the switch. While the power is on you
can't eject or swap cartridges: turn it off first, then use File → Eject Cartridge
(⌘W) or insert a different game. Battery saves are written when you power off or eject.
With the option off (the default), a ROM starts as soon as you open it.

Just like a real NES, the cartridge doesn't always make good contact: each power-on has
a 30% chance the power light blinks and the screen flashes while the game refuses to
start. A cartridge that fails keeps failing on every power-on, even if you eject and
reinsert it, until you blow on it: power off, choose Emulation → Blow on Cartridge
(⌥⌘B) and wait 5 seconds. The next power-on is guaranteed to work.

GAME GENIE
----------

Emulation → Game Genie Codes… (⌘G) opens the code list for the current game.
Type a 6- or 8-letter code (several at once is fine, separated by spaces) and an
optional description, then press Add. Each code has its own on/off checkbox, and
"Enable Game Genie" switches them all. Codes are saved per game and come back
whenever you open it. Like the real Game Genie, a code replaces what the console
reads from the cartridge (8-letter codes only when the original byte matches), so
some take effect only after a reset.

GAME GENIE
----------

Emulation → Game Genie Codes… (⌘G) opens the code list for the current game.
Type a 6- or 8-letter code (e.g. SXIOPO) and an optional description, then press
Return. Several codes can be pasted at once, separated by spaces or +. Each code has
its own on/off switch, the list shows the address and value it patches, and
Enable Game Genie turns them all off at once. Codes are saved per game (by the
cartridge's checksum) and come back the next time you open it. Like the real device,
codes patch what the console reads from the cartridge. A few take effect only after
a reset (⌘R).

OVERCLOCK
---------

Emulation → Overclock removes slowdown in games that lag when the screen gets
busy (Gradius, Kirby's Adventure, Mega Man 2 and 3, Contra Force…). After the last
visible scanline, oceanNES adds extra blank scanlines before VBlank: the CPU and picture
chip keep working (so graphics updates still land correctly), while the sound chip and
cartridge timers wait. The game gets 25%, 50% or 100% more CPU
time per frame, and game speed, music pitch and screen effects stay exactly as they
were.
It's off by default (the accurate setting) and uses more of your Mac's CPU. A few
games that rely on exact timing may behave differently. Turn it off if anything looks
wrong.

TOOLS: RAM EDITOR AND CORRUPTOR
-------------------------------

Tools → RAM Editor… (⌥⌘E) is a live hex view of the game's memory, refreshed 20
times a second. Choose CPU RAM, cartridge (battery) RAM, nametable RAM, palette, sprite
OAM, CHR (graphics) or PRG-ROM. Bytes that just changed flash red. Click a byte and type
two hex digits to change it (arrow keys move, Page Up/Down jump). Freeze locks the
selected byte to its value every frame (shown in blue), and is handy for infinite lives
or health. Go to jumps to an address.

Tools → Corruptor… (⌥⌘K) corrupts the running game, for glitch art or chaos:

- Corrupt in real time with an intensity from 1 to 1000 bytes a second, or
  Corrupt Now for a single burst of 50.
- Targets: program ROM (code and data, crashes are likely), graphics (CHR tiles), CPU
  RAM (game state), background map (nametables) and sprites (OAM).
- Modes: random byte, flip one bit, or nudge a value by ±1.
- Restore ROM puts the original program and graphics back. Restore & Reset
  also turns corruption off and power cycles for a clean start.

Corruption only changes the copy in memory. Your ROM file is never modified. While a
game is corrupted, battery saves are not written, so a corrupted game can't damage your
save file. Saving resumes after Restore & Reset or reloading the game.

SOUND FILTER
------------

Emulation → Sound Filter picks how the console's analog output stage sounds:

  - NES (front-loader)
      The NES's real output filters: 90 Hz + 440 Hz high-pass, 14 kHz low-pass (default)
  - Famicom
      The Famicom's gentler chain: 37 Hz high-pass, 14 kHz low-pass, so fuller bass
  - NES + TV Speaker
      The NES filters plus a small CRT TV speaker: bass cut below ~150 Hz and a
      soft roll-off above ~4.5 kHz
  - Raw (no filter)
      The mixer output with only the DC offset removed
  - MIDI soundfont
      The music replayed on a General MIDI synthesizer (see below)

MIDI soundfont
~~~~~~~~~~~~~~

The MIDI options in the Sound Filter menu turn the game's music into live MIDI and
play it through macOS's built-in General MIDI synthesizer. Every frame, oceanNES reads
what
each NES sound channel is doing:

- The two square channels and the triangle become instrument notes on MIDI channels 1–3.
  Pitch slides and vibrato become pitch bends, and volume envelopes become expression.
- Noise hits become hi-hats, snares and kicks (chosen by pitch), and DMC samples become
  kick or snare hits, all on the drum channel.

Instrument sets: Chiptune (square leads + synth bass), Rock Band (guitars + bass),
Orchestra (trumpet, violin, acoustic bass) and Piano. Load SoundFont uses any
.sf2 or .dls soundfont instead of the built-in one, and oceanNES remembers it. Sound
effects play through the same instruments, which is part of the fun. Expansion-chip
audio
(VRC6, Sunsoft 5B) isn't converted.

SUPPORTED MAPPERS
-----------------

0 NROM · 1 MMC1 (incl. SUROM/SXROM) · 2 UxROM · 3 CNROM · 4 MMC3/MMC6 · 5 MMC5 (no
expansion audio) · 7 AxROM · 9 MMC2 · 10 MMC4 · 11 Color Dreams · 13 CPROM · 21/22/23/25
VRC2/VRC4 · 24/26 VRC6 (with audio) · 28 Action 53 · 33 Taito TC0190 · 34 BNROM/NINA-001
·
48 Taito TC0690 · 64 Tengen RAMBO-1 · 66 GxROM · 69 Sunsoft FME-7/5B (with audio) · 70 ·
71 Camerica · 78 Irem/Jaleco · 79 NINA-03/06 · 87 · 89 · 93 · 94 · 118 TxSROM · 119
TQROM ·
140 · 152 · 180 · 184 · 185 · 206 · 232

That covers the large majority of the licensed NES library. Not yet included:
PAL timing, Famicom Disk System, Namco 163 / VRC7 / MMC5 audio, Zapper.

BUILDING FROM SOURCE
--------------------

On a Mac with the Command Line Tools (xcode-select --install):

    make
    open oceanNES.app

A locally built copy never triggers the Gatekeeper prompt.

Layout: src/ is the portable C99 emulator core (cpu, ppu, apu, cart/mappers,
palette, rom/zip loading); mac/ is the Cocoa frontend and mac/shaders.metal
the video filters; tools/ has the headless
test runner used to verify accuracy (make -f Makefile.linux builds it on Linux/macOS,
then ./runtest rom.nes). tools/shadertest/ runs the Metal shaders on the CPU to
check the filters (the NTSC decode reproduces every palette colour to within ~1/255).

LICENSE
-------

oceanNES is released under the MIT License (see the LICENSE file).
