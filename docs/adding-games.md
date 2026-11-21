# Bringing up a new game

1. **Check support**

   ```bash
   python3 reborn2droid.py info Game.apk
   ```

   Reborn2Droid currently handles Unity 3.x/4.x Mono games for `armeabi-v7a`. IL2CPP builds and other
   engines are rejected with a message.

2. **Build for Linux first** – iterating on a PC is much faster than on a phone:

   ```bash
   python3 reborn2droid.py build Game.apk linux
   RB_VERBOSE=1 RB_LIBDIR=work/<pkg>/libs work/<pkg>/build/linux/reborn2droid Game.apk
   ```

   * `JNI: unimplemented Java method X` – the game calls a Java method of a plugin or of its
     own activity. Add it to `runtime/java_classes.cpp` if the return value matters.
   * `import not implemented: name` – a libc/bionic function is missing; add an `IMP(name)` in
     `runtime/rt_libc.cpp` or `runtime/rt_posix.cpp`.
   * `interpreter: ... at <address>` – an instruction the interpreter does not decode yet.
   * crashes: rebuild with `DEBUG=1` to get the guest pc in crash reports, `RB_DUMP=10` for
     periodic thread dumps.

3. **Speed** – collect the library code the recompiler did not reach and feed it back:

   ```bash
   RB_PROF=1 RB_ENTRIES=entries.txt RB_LIBDIR=... reborn2droid Game.apk     # play a while, quit
   grep '^10' entries.txt | cut -d' ' -f1 > profiles/<pkg>/roots_libunity.txt
   grep '^11' entries.txt | cut -d' ' -f1 > profiles/<pkg>/roots_libmono.txt
   ```

   The recompiler skips jump-table slots automatically; review entries that are not function
   starts before committing them.

4. **Android**

   ```bash
   python3 reborn2droid.py build Game.apk          # arm64-v8a + x86_64 APK in work/<pkg>/out/
   adb install -r work/<pkg>/out/*.apk && adb logcat -s Reborn2Droid
   ```

5. Commit `profiles/<pkg>/` (no game files!) and add the game to the table in the README.
