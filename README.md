# Reborn64

**Bring 32-bit ARM Android games back to life on modern 64-bit phones — and on Linux PCs.**

Many classic Android games stopped working years ago: they ship only 32-bit ARM code
(`armeabi-v7a`), target an ancient API level, and rely on loader tricks that crash on
Android 11+. New phones (Pixel 7 and later, most 2024+ flagships, Android 17 devices) cannot
run 32-bit code at all, and Android 14+ refuses to install apps built for very old API levels.

Reborn64 takes the original APK you own and produces a new, modern APK:

* the game's 32-bit ARM engine libraries are **statically recompiled to C** and compiled
  natively for **arm64-v8a** and **x86_64** (no emulator, no 32-bit userspace needed);
* code generated at run time (the Mono JIT's ARM output) runs on a small **ARM interpreter**;
* a runtime provides everything the original code expected from 32-bit bionic Android:
  libc/pthreads/signals, a **Java/JNI implementation** of the engine's Java side, audio, input,
  sensors, and **OpenGL ES 1.1 emulated on OpenGL ES 2.0** (consistent on every GPU);
* the result targets API 35, supports 16 KB page devices, and runs fully offline
  (ads, analytics and dead online services are stubbed out).

The same build also runs as a native **Linux** program.

| | |
|---|---|
| Supported engine | Unity 3.x / 4.x **Mono** builds for `armeabi-v7a` (`libunity.so` + `libmono.so`) |
| Output | Android 7.0 – 17+ (arm64-v8a, x86_64); Linux x86_64 |
| Verified games | **Air Attack HD** 1.5.1 (Art In Games, Unity 3.5.7) — full speed (60 fps) on a Galaxy A52 |

Reborn64 contains **no game code or data**. You provide your own APK; everything derived
from it stays in `work/` on your machine and must not be redistributed.

## Quick start

```bash
git clone https://github.com/YYOzcan/reborn64 && cd reborn64
export JAVA_HOME=/path/to/jdk-17-or-newer
scripts/setup.sh                                   # NDK, SDK, SDL2, Python venv -> ./tools (once)
python3 reborn64.py info  MyGame.apk               # check that the game is supported
python3 reborn64.py build MyGame.apk               # -> work/<package>/out/<Game>-reborn64.apk
python3 reborn64.py build MyGame.apk linux         # native Linux build (needs SDL2 + zlib dev packages)
```

Install the APK with `adb install` or copy it to the phone. It uses its own package name
(`<original>.reborn64`), so it can sit next to the broken original. Saves live in the app's
private storage.

The Linux build runs the game straight from the APK:

```bash
RB_LIBDIR=work/<package>/libs work/<package>/build/linux/reborn64 MyGame.apk
```

Keys: mouse = touch, arrow keys = tilt, Esc = Android back, F11 = fullscreen.

## How it works

```
original APK ──► libunity.so / libmono.so (32-bit ARM)
                       │  recomp/recomp.py  (capstone; ARM, Thumb-2, VFP, NEON subset)
                       ▼
                 ~30 000 C functions  ──► clang (NDK / host) ──► libmain.so / reborn64
                       │
 runtime/  ─ guest memory (4 GB window, 32-bit pointers), ELF loader + relocations
           ─ interp.cpp: ARM interpreter for Mono JIT code, exception unwinding across host frames
           ─ rt_thread.cpp: guest pthreads, sync objects, signals delivered at safe points (Boehm GC)
           ─ rt_libc / rt_posix: ~450 bionic functions on the host libc
           ─ rt_jni + java_classes: JNIEnv/JavaVM and the engine's Java classes (UnityPlayer, PlayerPrefs,
             FMOD AudioTrack output, WWW, reflection helpers, plugin stubs)
           ─ gles1_emu.cpp: GLES 1.1 fixed-function pipeline generated as GLSL on GLES 2.0
           ─ main_sdl.cpp: SDL2 host (window, GL context, touch, sensors, audio, lifecycle)
```

See [docs/how-it-works.md](docs/how-it-works.md) for the details and
[docs/adding-games.md](docs/adding-games.md) for bringing up a new game.

## Debugging switches

Linux (environment): `RB_VERBOSE=1|2|3`, `RB_FPS=1`, `RB_SCRIPT="300:tap:640:360;600:shot:a.ppm;900:quit"`,
`RB_PROF=1` (interpreter profile), `RB_HPROF=1` (host profile), `RB_ENTRIES=file` (collect code the
recompiler missed), `RB_DUMP=10` (periodic guest thread dumps), `RB_GLTRACE=frame`, `RB_GLES2EMU=1`
(use the GLES 1.1 emulation on the desktop too), `DEBUG=1` at build time (track the guest pc).

Android (`adb shell setprop`): `debug.rb.verbose`, `debug.rb.fps 1` (FPS overlay), `debug.rb.gldebug`,
`debug.rb.gltrace`, `debug.rb.nativegles1 1` (use the device's own GLES 1.1 driver). Logs: `adb logcat -s Reborn64`.

## License

MIT — see [LICENSE](LICENSE). SDL2 (zlib license) and dlmalloc (public domain) are used as-is.
Game names and trademarks belong to their owners; Reborn64 is not affiliated with them.
