# Reborn2Droid

**Bring 32-bit ARM Android games back to life on modern 64-bit phones — and on Linux PCs.**

Many classic Android games stopped working years ago: they ship only 32-bit ARM code
(`armeabi-v7a`), target an ancient API level, and rely on loader tricks that crash on
Android 11+. New phones (Pixel 7 and later, most 2024+ flagships, Android 17 devices) cannot
run 32-bit code at all, and Android 14+ refuses to install apps built for very old API levels.

Reborn2Droid takes the original APK you own and produces a new, modern APK:

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
| Supported engine | Unity 3.4 – 4.3 **Mono** builds for `armeabi-v7a` / `armeabi-vfp` (`libunity.so` + `libmono.so`) |
| Output | Android 7.0 – 17+ (arm64-v8a, x86_64); Linux x86_64 |
| Verified games | **Air Attack HD** 1.5.1 (Art In Games, Unity 3.5.7, armeabi-v7a) — 60 fps on a Galaxy A52 |
| | **Battle Robots!** 1.5.1 (COLOPL, Unity 3.5.6, armeabi-vfp / ARMv5TE) — 60 fps, GC hitches removed |
| | **Sleepy Jack** 9012 (Silvertree Media, Unity 3.4.2, armeabi-v7a, GLES 2) — encrypted asset bundle handled by the profile |
| | **Calling All Mixels** 2.0.0 (Cartoon Network, Unity 4.3, armeabi-v7a) — 553 MB OBB merged into the APK (`--obb`) |

Reborn2Droid contains **no game code or data**. You provide your own APK; everything derived
from it stays in `work/` on your machine and must not be redistributed.

## Quick start

```bash
git clone https://github.com/YYOzcan/reborn2droid && cd reborn2droid
export JAVA_HOME=/path/to/jdk-17-or-newer
scripts/setup.sh                                   # NDK, SDK, SDL2, Python venv -> ./tools (once)
python3 reborn2droid.py info  MyGame.apk               # check that the game is supported
python3 reborn2droid.py build MyGame.apk               # -> work/<package>/out/<Game>-reborn2droid.apk
python3 reborn2droid.py build MyGame.apk linux         # native Linux build (needs SDL2 + zlib dev packages)
```

Install the APK with `adb install` or copy it to the phone. It uses its own package name
(`<original>.reborn2droid`), so it can sit next to the broken original. Saves live in the app's
private storage.

The Linux build runs the game straight from the APK:

```bash
RB_LIBDIR=work/<package>/libs work/<package>/build/linux/reborn2droid MyGame.apk
```

Keys: mouse = touch, arrow keys = tilt, Esc = Android back, F11 = fullscreen.

## How it works

```
original APK ──► libunity.so / libmono.so (32-bit ARM)
                       │  recomp/recomp.py  (capstone; ARM, Thumb-2, VFP, NEON subset)
                       ▼
                 ~30 000 C functions  ──► clang (NDK / host) ──► libmain.so / reborn2droid
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

## Game profiles

`profiles/<package>/` holds optional per-game data (never game files):

* `roots_libunity.txt`, `roots_libmono.txt` – extra recompiler entry points (speed);
* `prefs.txt` – PlayerPrefs values pinned while playing, e.g. `int Coin 999` keeps an
  in-game currency at its maximum (offline single-player games only);
* `prefs_<variant>.txt` – extra pins for an optional build variant:
  `reborn2droid.py build Game.apk --variant cheat` adds `prefs_cheat.txt` and produces a separate
  `…-cheat.apk` (same package, so it can replace the normal build and keep the save);
* `patches.txt` – `lib vaddr old new` 32-bit word patches applied to the engine libraries before
  recompilation (only when the old word matches), e.g. tuning the Boehm GC of old Mono builds;
* `ilpatch.txt` – `assembly recipe…` method-body replacements in the game's managed DLLs
  (`ilpatch/`, Mono.Cecil; needs the .NET SDK at build time), e.g. turning slow or APK-bound C# code
  into a plain read;
* `ilpatch_<variant>.txt` – extra IL recipes for `--variant <variant>` (e.g. Sleepy Jack's
  `ilpatch_cheat.txt` opens every world and level);
* `prepare.py` – `transform(zipfile) -> {entry: bytes}`: rewrites game assets (e.g. pre-decrypting
  bundles whose key depends on the original APK). With `ilpatch.txt`/`prepare.py` the build writes a
  patched copy of the game data, `work/<package>/game-data.apk`, used for Linux runs and the APK;
* `options.txt` – `key value` lines; `max_aspect 1.7778` keeps wider phones at 16:9 with side bars
  for games whose UI was laid out for 16:9.

Built in for every game: libgcc's soft-float and 64-bit helpers (`__aeabi_fadd`, `__aeabi_dmul`,
`__aeabi_f2iz`, …) exported by the engine libraries are replaced with host FPU code
(`runtime/softfloat_native.cpp`), which matters a lot for `armeabi` / `armeabi-vfp` builds.

Classes of the game's own Java package and of common plugin SDKs (ads, analytics, social,
billing, web views) are provided as logged stubs automatically, so their calls never crash the
game; online features simply report "not available".

See [docs/how-it-works.md](docs/how-it-works.md) for the details and
[docs/adding-games.md](docs/adding-games.md) for bringing up a new game.

## Debugging switches

Linux (environment): `RB_VERBOSE=1|2|3`, `RB_FPS=1`, `RB_SCRIPT="300:tap:640:360;400:down:x:y;405:up:x:y;600:shot:a.ppm;900:quit"`,
`RB_PROF=1` (interpreter profile), `RB_HPROF=1` (host profile), `RB_ENTRIES=file` (collect code the
recompiler missed), `RB_DUMP=10` (periodic guest thread dumps), `RB_GLTRACE=frame`, `RB_GLES2EMU=1`
(use the GLES 1.1 emulation on the desktop too), `DEBUG=1` at build time (track the guest pc).

Android (`adb shell setprop`): `debug.rb.verbose`, `debug.rb.fps 1` (FPS overlay), `debug.rb.gldebug`,
`debug.rb.gltrace`, `debug.rb.nativegles1 1` (use the device's own GLES 1.1 driver). Logs: `adb logcat -s Reborn2Droid`.

## License

MIT — see [LICENSE](LICENSE). SDL2 (zlib license) and dlmalloc (public domain) are used as-is.
Game names and trademarks belong to their owners; Reborn2Droid is not affiliated with them.
