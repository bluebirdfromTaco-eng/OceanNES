# Builds oceanNES.app for Intel macOS (x86_64, macOS 10.13+).
# On a Mac: just run `make` (needs the Xcode Command Line Tools: xcode-select --install).
# Cross-building from Linux: make SDK=/path/to/MacOSX10.15.sdk CROSS=1

APP      = oceanNES.app
TARGET   = x86_64-apple-macos10.13
CORE     = src/cpu.c src/ppu.c src/apu.c src/cart.c src/nes.c src/palette.c src/romfile.c src/midi.c
CFLAGS   = -O2 -std=gnu11 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation
OBJCFLAGS= -O2 -fobjc-arc -Wall -Wno-unused-parameter
FRAMEWORKS = -framework Cocoa -framework QuartzCore -framework AudioToolbox -framework AudioUnit -framework CoreAudio -framework GameController -framework Metal -framework IOKit -lz

ifdef CROSS
  CC = clang -target $(TARGET) -isysroot $(SDK)
  LDEXTRA = -fuse-ld=lld
else
  CC = clang -target $(TARGET)
  LDEXTRA =
endif

all: $(APP)

build/cycle: $(CORE) mac/main.m mac/input.m mac/input.h mac/cheats.m mac/cheats.h mac/tools.m mac/tools.h src/nes.h src/romfile.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c src/cpu.c -o build/cpu.o
	$(CC) $(CFLAGS) -c src/ppu.c -o build/ppu.o
	$(CC) $(CFLAGS) -c src/apu.c -o build/apu.o
	$(CC) $(CFLAGS) -c src/cart.c -o build/cart.o
	$(CC) $(CFLAGS) -c src/nes.c -o build/nes.o
	$(CC) $(CFLAGS) -c src/palette.c -o build/palette.o
	$(CC) $(CFLAGS) -c src/romfile.c -o build/romfile.o
	$(CC) $(CFLAGS) -c src/midi.c -o build/midi.o
	$(CC) $(OBJCFLAGS) -c mac/main.m -o build/main.o
	$(CC) $(OBJCFLAGS) -c mac/input.m -o build/input.o
	$(CC) $(OBJCFLAGS) -c mac/cheats.m -o build/cheats.o
	$(CC) $(OBJCFLAGS) -c mac/tools.m -o build/tools.o
	$(CC) $(LDEXTRA) -o $@ build/*.o $(FRAMEWORKS)

$(APP): build/cycle mac/Info.plist mac/AppIcon.icns mac/shaders.metal
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	cp build/cycle $(APP)/Contents/MacOS/oceanNES
	cp mac/Info.plist $(APP)/Contents/Info.plist
	cp mac/AppIcon.icns $(APP)/Contents/Resources/AppIcon.icns
	cp mac/shaders.metal $(APP)/Contents/Resources/shaders.metal
	printf 'APPL????' > $(APP)/Contents/PkgInfo
	-command -v codesign >/dev/null && codesign --force --sign - $(APP)

clean:
	rm -rf build $(APP)

.PHONY: all clean
