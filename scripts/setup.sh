#!/bin/bash
# Downloads and builds everything Reborn2Droid needs into ./tools (about 3 GB):
# Android NDK r27c, SDK command-line tools + platform 35 + build-tools 35, SDL2 2.32.10 (built for
# arm64-v8a and x86_64) and a Python venv with capstone + pyelftools. Needs: curl, unzip, cmake,
# ninja, clang, python3, a JDK 17+ (JAVA_HOME), and for Linux builds the SDL2 and zlib dev packages.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/tools"; mkdir -p "$T"; cd "$T"
: "${JAVA_HOME:?set JAVA_HOME to a JDK 17 or newer}"

if [ ! -d android-ndk-r27c ]; then
    echo "== Android NDK r27c"
    curl -fL -o ndk.zip https://dl.google.com/android/repository/android-ndk-r27c-linux.zip
    unzip -q ndk.zip && rm ndk.zip
fi
if [ ! -d android-sdk/cmdline-tools ]; then
    echo "== Android SDK command-line tools"
    mkdir -p android-sdk/cmdline-tools
    curl -fL -o clt.zip https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
    unzip -q clt.zip -d android-sdk/cmdline-tools && mv android-sdk/cmdline-tools/cmdline-tools android-sdk/cmdline-tools/latest && rm clt.zip
fi
yes | android-sdk/cmdline-tools/latest/bin/sdkmanager --sdk_root="$T/android-sdk" "platforms;android-35" "build-tools;35.0.0" >/dev/null
if [ ! -d SDL2-2.32.10 ]; then
    echo "== SDL2 2.32.10"
    curl -fL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz | tar xz
fi
for abi in arm64-v8a x86_64; do
    [ -f "sdl-$abi/libSDL2.so" ] && continue
    echo "== SDL2 for $abi"
    mkdir -p "sdl-$abi"
    (cd "sdl-$abi" && cmake ../SDL2-2.32.10 -G Ninja -DCMAKE_TOOLCHAIN_FILE="$T/android-ndk-r27c/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$abi -DANDROID_PLATFORM=24 -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TEST=OFF \
        -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" >/dev/null && ninja >/dev/null)
done
if [ ! -x venv/bin/python ]; then
    echo "== Python venv (capstone, pyelftools)"
    python3 -m venv venv && venv/bin/pip install -q capstone pyelftools
fi
echo "== tools ready in $T"
