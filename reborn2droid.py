#!/usr/bin/env python3
"""Reborn2Droid - bring 32-bit ARM Android games back to life on modern 64-bit phones (and Linux PCs).

    python3 reborn2droid.py build <game.apk> [targets...]     targets: android (default), linux, android-arm64, android-x86_64
    python3 reborn2droid.py info  <game.apk>                   show what Reborn2Droid sees in the APK

The game's 32-bit ARM engine libraries are statically recompiled to portable C, compiled for the
target, and run on a runtime that provides the Android/Java side the original code expects.
Everything a build produces lives in work/<package>/ (the APK you get is in work/<package>/out/).

Environment: TOOLS=<dir with android-ndk-*, SDL2-*, sdl-<abi> builds> (default ./tools, see scripts/setup.sh),
ANDROID_SDK_ROOT, JAVA_HOME, JOBS=n, OPT=-O2, DEBUG=1 (record the guest pc of every instruction).
"""
import os, sys, re, glob, json, shutil, zipfile, tempfile, subprocess, concurrent.futures as cf

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.abspath(os.environ.get("TOOLS", os.path.join(HERE, "tools")))
SDK = os.environ.get("ANDROID_SDK_ROOT", os.path.join(TOOLS, "android-sdk"))
JDK = os.environ.get("JAVA_HOME", "")
JOBS = int(os.environ.get("JOBS", os.cpu_count() or 4))
VENV_PY = os.path.join(TOOLS, "venv", "bin", "python")
PY = VENV_PY if os.path.exists(VENV_PY) else sys.executable
LIBS = {"U": ("libunity.so", 0x10000000), "M": ("libmono.so", 0x11000000)}
ABI_TARGET = {"arm64-v8a": "android-arm64", "x86_64": "android-x86_64"}

def die(msg): sys.exit("reborn2droid: " + msg)

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.stderr.write(" ".join(cmd)[:500] + "\n" + r.stdout[-4000:] + r.stderr[-8000:])
        raise SystemExit(1)
    return r

def stale(out, *deps):
    if not os.path.exists(out): return True
    t = os.path.getmtime(out)
    return any(os.path.getmtime(d) > t for d in deps if os.path.exists(d))

def find_tool(pattern):
    hits = sorted(glob.glob(os.path.join(TOOLS, pattern)))
    return hits[-1] if hits else None

def build_tools():
    bt = sorted(glob.glob(os.path.join(SDK, "build-tools", "*")))
    if not bt: die(f"no Android build-tools in {SDK} (run scripts/setup.sh)")
    return bt[-1]

# ================================================================= APK inspection -> profile
ORIENT = {"unspecified": -1, "landscape": 0, "portrait": 1, "user": 2, "behind": 3, "sensor": 4, "nosensor": 5,
          "sensorLandscape": 6, "sensorPortrait": 7, "reverseLandscape": 8, "reversePortrait": 9, "fullSensor": 10}

def inspect(apk):
    z = zipfile.ZipFile(apk)
    names = set(z.namelist())
    def big(p): return p in names and z.getinfo(p).file_size > 1 << 20
    libdir = None
    for d in ("assets/libs/armeabi-v7a/", "lib/armeabi-v7a/", "assets/libs/armeabi/", "lib/armeabi/"):
        if big(d + "libunity.so") and big(d + "libmono.so"): libdir = d; break
    if not libdir:
        if any(n.endswith("libil2cpp.so") for n in names): die("IL2CPP Unity games are not supported yet (only Mono builds)")
        die("this APK has no 32-bit ARM Unity/Mono engine (libunity.so + libmono.so) - not a supported game")
    aapt2 = os.path.join(build_tools(), "aapt2")
    badging = run([aapt2, "dump", "badging", apk]).stdout
    m = re.search(r"package: name='([^']+)' versionCode='([^']*)' versionName='([^']*)'", badging)
    pkg, vcode, vname = m.group(1), m.group(2) or "1", m.group(3) or "1.0"
    label = (re.search(r"application-label:'([^']*)'", badging) or re.search(r"label='([^']*)'", badging))
    label = label.group(1) if label else pkg
    icons = re.findall(r"application-icon-(\d+):'([^']+)'", badging)
    icon = max(icons, key=lambda x: int(x[0]))[1] if icons else None
    xml = run([aapt2, "dump", "xmltree", "--file", "AndroidManifest.xml", apk]).stdout
    activities = []
    for block in re.split(r"\n\s*E: activity", xml)[1:]:
        n = re.search(r'A: http://schemas.android.com/apk/res/android:name\(0x[0-9a-f]+\)="([^"]+)"', block)
        o = re.search(r'screenOrientation\(0x[0-9a-f]+\)=(\d+)', block)
        if n: activities.append((n.group(1), int(o.group(1)) if o else -1))
    act = "com.unity3d.player.UnityPlayerActivity"; orient = -1
    for n, o in activities:
        if "Proxy" in n or "Native" in n or "VideoPlayer" in n: continue
        if "UnityPlayer" in n or "Activity" in n: act, orient = n, o; break
    if orient == -1:
        orient = next((o for n, o in activities if o != -1), -1)
    gles, splash = 2, 1
    if "assets/bin/Data/settings.xml" in names:
        st = z.read("assets/bin/Data/settings.xml").decode("utf-8", "replace")
        g = re.search(r'name="gles_mode">(\d+)<', st); s = re.search(r'name="splash_mode">(\d+)<', st)
        gles = int(g.group(1)) if g else gles; splash = int(s.group(1)) if s else splash
    return dict(package=pkg, version_code=vcode, version_name=vname, label=label, icon=icon, libdir=libdir,
                activity=act.replace(".", "/"), orientation=orient, gles_mode=gles, splash_mode=splash)

def work_dir(prof): return os.path.join(HERE, "work", prof["package"])

def write_profile(prof, wd):
    data = re.sub(r"[^A-Za-z0-9]+", "", prof["label"]) or prof["package"]
    o = prof["orientation"] if prof["orientation"] >= 0 else 4
    h = (f'// generated by reborn2droid.py from {prof["package"]}\n#pragma once\n'
         f'#define RB_PACKAGE "{prof["package"]}"\n#define RB_LABEL {json.dumps(prof["label"])}\n'
         f'#define RB_DATA_NAME "{data}"\n#define RB_ACTIVITY "{prof["activity"]}"\n'
         f'#define RB_GLES_MODE {prof["gles_mode"]}\n#define RB_SPLASH_MODE {prof["splash_mode"]}\n#define RB_ORIENTATION {o}\n')
    p = os.path.join(wd, "profile.h")
    if not os.path.exists(p) or open(p).read() != h: open(p, "w").write(h)

# ================================================================= recompilation
def recompile(apk, prof, wd):
    libs = os.path.join(wd, "libs"); os.makedirs(libs, exist_ok=True)
    z = zipfile.ZipFile(apk)
    for pfx, (name, base) in LIBS.items():
        dst = os.path.join(libs, name)
        if not os.path.exists(dst): open(dst, "wb").write(z.read(prof["libdir"] + name))
    rc = os.path.join(HERE, "recomp/recomp.py")
    gen = os.path.join(wd, "gen")
    for pfx, (name, base) in LIBS.items():
        d = os.path.join(gen, pfx); os.makedirs(d, exist_ok=True)
        roots = os.path.join(HERE, "profiles", prof["package"], f"roots_{name[:-3]}.txt")
        stamp = os.path.join(d, ".stamp")
        if stale(stamp, os.path.join(libs, name), rc, roots):
            print(f"== recompiling {name} (ARM -> C)", flush=True)
            for f in glob.glob(os.path.join(d, "*.c")): os.remove(f)
            with open(os.path.join(wd, f"recomp_{pfx}.log"), "w") as log:
                cmd = [PY, rc, os.path.join(libs, name), d, pfx, hex(base)] + ([roots] if os.path.exists(roots) else [])
                subprocess.run(cmd, stdout=log, check=True)
            open(stamp, "w").close()
    imp = os.path.join(gen, "imports_gen.c")
    txts = [os.path.join(gen, p, f"{p}_imports.txt") for p in LIBS]
    if stale(imp, *txts, os.path.join(HERE, "recomp/gen_imports.py")):
        run([sys.executable, os.path.join(HERE, "recomp/gen_imports.py"), imp] + txts)
    glg = os.path.join(gen, "rt_gl_gen.cpp")
    if stale(glg, os.path.join(HERE, "recomp/gen_gl.py")):
        run([sys.executable, os.path.join(HERE, "recomp/gen_gl.py"), glg])

# ================================================================= compilation
def toolchain(target):
    if target == "linux":
        return ["clang"], ["clang++"], [], ["-lSDL2", "-lz", "-lpthread", "-ldl", "-rdynamic"], "reborn2droid", ["-I/usr/include/SDL2", "-D_REENTRANT"]
    ndk = find_tool("android-ndk-*")
    if not ndk: die(f"no Android NDK in {TOOLS} (run scripts/setup.sh)")
    abi = {v: k for k, v in ABI_TARGET.items()}[target]
    triple = {"arm64-v8a": "aarch64-linux-android", "x86_64": "x86_64-linux-android"}[abi]
    bin_ = os.path.join(ndk, "toolchains/llvm/prebuilt/linux-x86_64/bin")
    sdl_src = find_tool("SDL2-*"); sdl_lib = os.path.join(TOOLS, "sdl-" + abi)
    if not sdl_src or not os.path.exists(os.path.join(sdl_lib, "libSDL2.so")): die("SDL2 for Android missing (run scripts/setup.sh)")
    return [os.path.join(bin_, f"{triple}24-clang")], [os.path.join(bin_, f"{triple}24-clang++")], \
        ["-fPIC", "-fvisibility=hidden", "-fno-semantic-interposition"], \
        ["-shared", "-L" + sdl_lib, "-lSDL2", "-lz", "-llog", "-landroid", "-ldl", "-static-libstdc++", "-Wl,-z,max-page-size=16384",
         "-Wl,-Bsymbolic", "-Wl,--gc-sections", "-Wl,-O2"], "libmain.so", ["-I" + os.path.join(sdl_src, "include")]

def compile_target(wd, target):
    gen = os.path.join(wd, "gen")
    out = os.path.join(wd, "build", target + ("-dbg" if os.environ.get("DEBUG") else "")); obj = os.path.join(out, "obj")
    os.makedirs(obj, exist_ok=True)
    cc, cxx, extra, ldflags, exe, sdl_inc = toolchain(target)
    opt = os.environ.get("OPT", "-O2").split()
    dbg = ["-DRB_DEBUG"] if os.environ.get("DEBUG") else []
    inc = ["-I" + os.path.join(HERE, "runtime"), "-I" + wd]
    jobs = []
    gflags = cc + opt + ["-w", "-fno-strict-aliasing", "-ffp-contract=off", "-fwrapv", "-g0"] + dbg + inc + extra
    for pfx in LIBS:
        for f in sorted(glob.glob(os.path.join(gen, pfx, "*.c"))):
            jobs.append((gflags, f, os.path.join(obj, os.path.basename(f)[:-2] + ".o"),
                         [os.path.join(gen, pfx, f"{pfx}_decls.h"), os.path.join(HERE, "runtime/guest.h")]))
    jobs.append((cc + ["-O2", "-w"] + inc + extra, os.path.join(gen, "imports_gen.c"), os.path.join(obj, "imports_gen.o"), []))
    jobs.append((cc + ["-O2", "-w", "-DONLY_MSPACES=1", "-DUSE_LOCKS=1", "-DHAVE_MMAP=0", "-DHAVE_MORECORE=0", "-DMSPACES=1"] + extra,
                 os.path.join(HERE, "third_party/malloc.c"), os.path.join(obj, "dlmalloc.o"), []))
    rflags = cxx + ["-O2", "-g", "-std=c++17", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-fexceptions"] + dbg + inc + sdl_inc + extra
    hdrs = glob.glob(os.path.join(HERE, "runtime/*.h")) + [os.path.join(wd, "profile.h")]
    srcs = sorted(glob.glob(os.path.join(HERE, "runtime/*.cpp"))) + [os.path.join(gen, "rt_gl_gen.cpp")]
    for f in srcs:
        if os.path.basename(f) == "android.cpp" and not target.startswith("android"): continue
        jobs.append((rflags, f, os.path.join(obj, "rt_" + os.path.basename(f)[:-4] + ".o"), hdrs))
    todo = [j for j in jobs if stale(j[2], j[1], *j[3])]
    print(f"== {target}: compiling {len(todo)} of {len(jobs)} files", flush=True)
    def one(j):
        flags, src, o, _ = j
        run(flags + ["-c", src, "-o", o])
    with cf.ThreadPoolExecutor(JOBS) as ex:
        for k, _ in enumerate(ex.map(one, todo)):
            if (k + 1) % 25 == 0: print(f"   {k + 1}/{len(todo)}", flush=True)
    binary = os.path.join(out, exe)
    if stale(binary, *[j[2] for j in jobs]):
        run(cxx + ["-o", binary] + [j[2] for j in jobs] + ldflags)
    return binary

# ================================================================= Android packaging
MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{package}" android:versionCode="{version_code}" android:versionName="{version_name}-reborn2droid">
    <uses-sdk android:minSdkVersion="24" android:targetSdkVersion="35" />
    <uses-feature android:glEsVersion="0x00020000" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-feature android:name="android.hardware.sensor.accelerometer" android:required="false" />
    <uses-permission android:name="android.permission.VIBRATE" />
    <application android:label="{label}" android:icon="@drawable/rb_icon" android:allowBackup="true"
        android:extractNativeLibs="true" android:hardwareAccelerated="true" android:isGame="true"
        android:appCategory="game" android:resizeableActivity="false">
        <activity android:name="org.reborn2droid.GameActivity" android:exported="true"
            android:label="{label}" android:screenOrientation="{orientation}" android:launchMode="singleTask"
            android:theme="@android:style/Theme.NoTitleBar.Fullscreen"
            android:configChanges="mcc|mnc|locale|touchscreen|keyboard|keyboardHidden|navigation|orientation|screenLayout|uiMode|screenSize|smallestScreenSize|fontScale|layoutDirection|density">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
"""

def package_apk(apk, prof, wd, abis):
    bt = build_tools()
    jar = sorted(glob.glob(os.path.join(SDK, "platforms", "android-*", "android.jar")))
    if not jar: die(f"no Android platform in {SDK} (run scripts/setup.sh)")
    jar = jar[-1]
    jdk_bin = os.path.join(JDK, "bin") if JDK else ""
    javac = os.path.join(jdk_bin, "javac") if JDK else "javac"
    sdl_java = os.path.join(find_tool("SDL2-*"), "android-project/app/src/main/java")
    tmp = tempfile.mkdtemp(prefix="rb-apk-")
    try:
        res = os.path.join(tmp, "res/drawable"); os.makedirs(res)
        z = zipfile.ZipFile(apk)
        if prof["icon"] and prof["icon"].endswith(".png"): open(os.path.join(res, "rb_icon.png"), "wb").write(z.read(prof["icon"]))
        else: shutil.copy(os.path.join(HERE, "android/default_icon.png"), os.path.join(res, "rb_icon.png"))
        orient = {v: k for k, v in ORIENT.items()}.get(prof["orientation"], "sensor")
        man = os.path.join(tmp, "AndroidManifest.xml")
        from xml.sax.saxutils import escape
        open(man, "w").write(MANIFEST.format(package=prof["package"] + ".reborn2droid", version_code=prof["version_code"],
                                             version_name=escape(prof["version_name"]), label=escape(prof["label"]), orientation=orient))
        run([os.path.join(bt, "aapt2"), "compile", "--dir", os.path.join(tmp, "res"), "-o", os.path.join(tmp, "res.zip")])
        base = os.path.join(tmp, "base.apk")
        run([os.path.join(bt, "aapt2"), "link", "-o", base, "-I", jar, "--manifest", man, os.path.join(tmp, "res.zip")])
        srcs = [os.path.join(dp, f) for d in (sdl_java, os.path.join(HERE, "android/java")) for dp, _, fs in os.walk(d) for f in fs if f.endswith(".java")]
        cls = os.path.join(tmp, "classes"); os.makedirs(cls)
        run([javac, "-nowarn", "-source", "8", "-target", "8", "-cp", jar, "-d", cls] + srcs)
        env = dict(os.environ);
        if JDK: env.update(JAVA_HOME=JDK, PATH=jdk_bin + ":" + os.environ["PATH"])
        cjar = os.path.join(tmp, "classes.jar")
        run([os.path.join(jdk_bin, "jar") if JDK else "jar", "cf", cjar, "-C", cls, "."])
        dex = os.path.join(tmp, "dex"); os.makedirs(dex)
        run([os.path.join(bt, "d8"), "--release", "--min-api", "24", "--output", dex, cjar], env=env)
        unsigned = os.path.join(tmp, "unsigned.apk")
        with zipfile.ZipFile(base) as zb, zipfile.ZipFile(unsigned, "w") as zo:
            for it in zb.infolist(): zo.writestr(it, zb.read(it.filename))
            zo.write(os.path.join(dex, "classes.dex"), "classes.dex", zipfile.ZIP_DEFLATED)
            for abi in abis:
                zo.write(os.path.join(wd, "build", ABI_TARGET[abi], "libmain.so"), f"lib/{abi}/libmain.so", zipfile.ZIP_DEFLATED)
                zo.write(os.path.join(TOOLS, "sdl-" + abi, "libSDL2.so"), f"lib/{abi}/libSDL2.so", zipfile.ZIP_DEFLATED)
            for it in z.infolist():                      # the original game data, untouched
                if it.filename.startswith("assets/"): zo.writestr(it, z.read(it.filename), compress_type=it.compress_type)
        aligned = os.path.join(tmp, "aligned.apk")
        run([os.path.join(bt, "zipalign"), "-f", "-P", "16", "4", unsigned, aligned])
        ks = os.path.join(HERE, "work", "reborn2droid.keystore")
        if not os.path.exists(ks):
            run([os.path.join(jdk_bin, "keytool") if JDK else "keytool", "-genkeypair", "-keystore", ks, "-storepass", "android",
                 "-keypass", "android", "-alias", "reborn2droid", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname", "CN=Reborn2Droid"])
        outd = os.path.join(wd, "out"); os.makedirs(outd, exist_ok=True)
        name = re.sub(r"[^A-Za-z0-9]+", "", prof["label"]) or "game"
        out = os.path.join(outd, f"{name}-reborn2droid.apk")
        run([os.path.join(bt, "apksigner"), "sign", "--ks", ks, "--ks-pass", "pass:android", "--key-pass", "pass:android",
             "--out", out, aligned], env=env)
        return out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

# ================================================================= CLI
def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("build", "info"): sys.exit(__doc__)
    apk = os.path.abspath(sys.argv[2])
    prof = inspect(apk)
    if sys.argv[1] == "info":
        for k, v in prof.items(): print(f"{k:14} {v}")
        return
    targets = sys.argv[3:] or ["android"]
    wd = work_dir(prof); os.makedirs(wd, exist_ok=True)
    write_profile(prof, wd)
    print(f"== {prof['label']} ({prof['package']} {prof['version_name']}), engine libraries in {prof['libdir']}")
    recompile(apk, prof, wd)
    abis = []
    for t in targets:
        if t == "android": abis += ["arm64-v8a", "x86_64"]
        elif t in ABI_TARGET.values(): abis.append({v: k for k, v in ABI_TARGET.items()}[t])
        elif t == "linux":
            b = compile_target(wd, "linux")
            print(f"== linux: {b}\n   run: RB_LIBDIR={os.path.join(wd, 'libs')} {b} {apk}")
        else: die(f"unknown target {t}")
    for abi in abis: compile_target(wd, ABI_TARGET[abi])
    if abis:
        out = package_apk(apk, prof, wd, abis)
        print(f"== {out} ({os.path.getsize(out) >> 20} MB)")

if __name__ == "__main__":
    main()
