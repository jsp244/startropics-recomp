#!/bin/sh
# tools/macos/build.sh - builds and tests the macOS package on a Mac with Xcode's command line tools,
# CMake and Python 3: SDL2 as a static library for Apple silicon and Intel Macs (once, into
# SDL_PREFIX), the game for both kinds of Mac, its tests, the app and dist/startropics-macos.zip.
# Needs generated/ (make recomp); the tests also need your cartridge dump in rom/. Give the build
# its version as for Linux (see tools/linux/Dockerfile): ST_VERSION and ST_REPO.
# .github/workflows/macos.yml runs it on GitHub's Macs. The test pictures go to shots/.
set -e
cd "$(dirname "$0")/../.."
SDL_VERSION=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
SDL_PREFIX=${SDL_PREFIX:-$HOME/sdl2-$SDL_VERSION-universal}
J=$(sysctl -n hw.ncpu)
export MACOSX_DEPLOYMENT_TARGET=11.0

if [ ! -x "$SDL_PREFIX/bin/sdl2-config" ]; then
    echo "== SDL $SDL_VERSION, a static library for arm64 + x86_64, into $SDL_PREFIX"
    t=$(mktemp -d)
    curl -fsSL -o "$t/sdl.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
    echo "$SDL_SHA256  $t/sdl.tar.gz" | shasum -a 256 -c -
    tar -xzf "$t/sdl.tar.gz" -C "$t"
    cmake -S "$t/SDL2-$SDL_VERSION" -B "$t/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF
    cmake --build "$t/build" -j "$J"
    cmake --install "$t/build"
    rm -rf "$t"
fi
export PATH="$SDL_PREFIX/bin:$PATH"
SDL_STATIC_LIBS=$(sdl2-config --static-libs | sed -e 's/ -lm / /g' -e 's/ -lm$//')   # the test scripts add -lm themselves
M="make -j$J PYTHON=python3"

echo "== version ${ST_VERSION:-dev}, repository ${ST_REPO:-(none)}"
$M headless headless-recomp
$M BUILD=build-mac startropics
echo "== the menu's rebinding test"
SDL_LIBS="$SDL_STATIC_LIBS" CC=clang sh tests/run_ui_bind_test.sh | tail -n 1

rm -rf shots && mkdir -p shots
ROM=$(ls rom/*.nes 2>/dev/null | head -n 1 || true)
if [ -n "$ROM" ]; then
    ROM=$(cd "$(dirname "$ROM")" && pwd -P)/$(basename "$ROM")
    echo "== cosimulation (interpreter vs native code)"
    python3 tests/cosim.py --frames 3000
    echo "== the remastered music (the game's notes on the SNES sound chip)"
    ./build/nes-headless-recomp --rom "$ROM" --recomp --frames 900 --snes-music 0 --wav shots/music.wav --quiet
    python3 -c "import wave, array, math; w = wave.open('shots/music.wav'); a = array.array('h', w.readframes(w.getnframes())); print('remastered music: %d stereo samples, RMS %.0f' % (len(a) // 2, math.sqrt(sum(x * x for x in a) / len(a))))"
    echo "== self-tests without a display: SDL's dummy video and audio drivers, its software renderer"
    (
        # (not the offscreen driver: on a Mac SDL gives every window OpenGL, which offscreen has no library for)
        export SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy
        B=./build-mac/startropics
        $B "$ROM" --selftest 300 shots/selftest.png | grep selftest | tee shots/selftest.txt
        $B --trigger-test | tail -n 1
        $B --touch-test | tail -n 1
        $B "$ROM" --chord-test | tail -n 1
        $B "$ROM" --rewind-test 1200 | grep rewind-test
        $B "$ROM" --ui-shot shots/ui-offscreen.png 1 60 | grep ui-shot
        $B --update-check | tail -n 2 || true   # the Updates tab's check (needs the network)
        if /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
            echo "== the Intel half of the program, under Rosetta (the same settings)"
            /usr/bin/arch -x86_64 $B "$ROM" --selftest 300 shots/selftest-x86_64.png | grep selftest | tee shots/selftest-x86_64.txt
            /usr/bin/arch -x86_64 $B "$ROM" --rewind-test 600 | grep rewind-test
            # the frame the PPU drew (its palette numbers) must be the same bit for bit; the colours it turns
            # into could differ by a step if the NTSC palette's pow/cos/sin rounded differently in the two
            # halves' math libraries (runtime/palette.c)
            test "$(sed -n 's/.*; frame //p' shots/selftest.txt)" = "$(sed -n 's/.*; frame //p' shots/selftest-x86_64.txt)"
            echo "the same frame from both halves"
            if cmp -s shots/selftest.png shots/selftest-x86_64.png; then echo "and the same colours"
            else printf 'colours: '; python3 tools/macos/pngdiff.py shots/selftest.png shots/selftest-x86_64.png; fi
        else
            echo "== (this Mac has no Rosetta: the Intel half was not run)"
        fi
        echo "== the remastered graphics"
        sed 's/^textures=.*/textures=1/' build-mac/startropics.ini > build-mac/startropics.ini.new
        mv build-mac/startropics.ini.new build-mac/startropics.ini
        $B "$ROM" --game-shot shots/textures.png 900 | grep game-shot
    )
fi
rm -f build-mac/startropics.ini
rm -rf build-mac/saves

echo "== the app and the package"
$M BUILD=build-mac dist
APP="build-mac/app/StarTropics.app"
printf 'architectures: '; lipo -archs "$APP/Contents/MacOS/startropics"
echo "== the libraries it needs: macOS's own only"
LIBS=$(otool -L "$APP/Contents/MacOS/startropics" | grep -E '^[[:space:]]' | sort -u)   # (one list per architecture)
echo "$LIBS"
if echo "$LIBS" | grep -v -E '^[[:space:]]+(/usr/lib/|/System/Library/)'; then
    echo "a library that is not part of macOS (above)"; exit 1
fi
echo "== the oldest macOS it runs on"
otool -l "$APP/Contents/MacOS/startropics" | grep -E '^ +minos ' | sort -u

echo "== the package unpacked as the Finder does: signature, Info.plist, icon"
U=$(mktemp -d)
ditto -x -k dist/startropics-macos.zip "$U"
UAPP="$U/startropics/StarTropics.app"
ls "$U/startropics"
codesign --verify --deep --strict --verbose=2 "$UAPP"
plutil -lint "$UAPP/Contents/Info.plist"
iconutil -c iconset -o "$U/icon.iconset" "$UAPP/Contents/Resources/startropics.icns"
ls "$U/icon.iconset" | tr '\n' ' '; echo

if [ -n "$ROM" ]; then
    echo "== the app in a real window (Cocoa, Metal), as unpacked"
    BIN="$UAPP/Contents/MacOS/startropics"
    SUPPORT="$HOME/Library/Application Support/StarTropicsPort/StarTropics"
    rm -rf "$SUPPORT"
    "$BIN" "$ROM" --game-shot "$PWD/shots/game-window.png" 300 | grep -E 'renderer|pacing|game-shot'
    "$BIN" "$ROM" --ui-shot "$PWD/shots/ui-window.png" 1 60 | grep ui-shot
    perl -e 'alarm 60; exec @ARGV' "$BIN" --dialog-test
    test -f "$SUPPORT/startropics.ini" && echo "settings: in Application Support"
    echo "== opened by Launch Services with the ROM dropped on its icon, then closed as macOS closes it"
    rm -f "$SUPPORT/startropics.ini"
    open -a "$UAPP" "$ROM"
    for i in $(seq 1 30); do grep -q '^rom=' "$SUPPORT/startropics.ini" 2>/dev/null && break; sleep 1; done
    screencapture -x shots/screen.png 2>/dev/null || true
    pkill -TERM -x startropics || true
    for i in $(seq 1 20); do pgrep -x startropics >/dev/null || break; sleep 1; done
    grep '^rom=' "$SUPPORT/startropics.ini"
    test "$(sed -n 's/^rom=//p' "$SUPPORT/startropics.ini")" = "$ROM" && echo "the dropped ROM was loaded and remembered"
    echo "== nothing was written into the app"
    codesign --verify --deep --strict "$UAPP" && echo "signature still intact"
    rm -rf "$SUPPORT"
fi
rm -rf "$U"
ls -l dist/
