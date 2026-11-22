// The Java half of the original APK, reimplemented: Android framework bits that libunity touches,
// com.unity3d.player.* (UnityPlayer, PlayerPrefs, ReflectionHelper, WWW, UnityJavaRunnable),
// org.fmod.FMODAudioDevice, and the game's plugins (AdMob, Flurry, Moga, OpenFeint: all offline no-ops).
#include "jvm.h"
#include "platform.h"
#include "profile.h"
#include <cmath>
#include <mutex>
#include <thread>
#include <atomic>
#include <ctime>
#include <condition_variable>

using namespace jni;
using namespace rt;

namespace {
JV nul() { return JV(); }
JV str(const std::string &s) { return JV::O(jstr(s)); }

// ------------------------------------------------------------------ reflection
Ref reflect_method(JClass *k, const std::string &name, const std::string &sig, bool is_static) {
    if (!k) return nullptr;
    // ReflectionHelper's sig is the full JNI signature; find exact match or any overload by name
    JMethod *found = nullptr;
    for (JClass *c = k; c && !found; c = c->super) {
        auto it = c->methods.find(name + sig);
        if (it != c->methods.end()) found = it->second;
    }
    if (!found) {
        bool lenient = false; for (JClass *c = k; c; c = c->super) lenient |= c->lenient;
        if (!lenient) { logf("reflection: %s.%s%s not found", k->name.c_str(), name.c_str(), sig.c_str()); return nullptr; }
        found = new JMethod{k, name, sig, is_static, nullptr, true};
        k->methods[name + sig] = found;
    }
    Ref o = new_obj(def_class("java/lang/reflect/Method"));
    o->as_method = found;
    return o;
}
Ref reflect_field(JClass *k, const std::string &name, const std::string &sig, bool is_static) {
    if (!k) return nullptr;
    JField *f = nullptr;
    for (JClass *c = k; c && !f; c = c->super) { auto it = c->fields.find(name); if (it != c->fields.end()) f = it->second; }
    if (!f) {
        bool lenient = false; for (JClass *c = k; c; c = c->super) lenient |= c->lenient;
        if (!lenient) { logf("reflection: field %s.%s not found", k->name.c_str(), name.c_str()); return nullptr; }
        f = def_field(k, name, sig, is_static);
    }
    Ref o = new_obj(def_class("java/lang/reflect/Field"));
    o->as_field = f;
    return o;
}

// ------------------------------------------------------------------ UnityPlayer singleton
JClass *K_player, *K_activity, *K_fmod, *K_prefs, *K_www, *K_runnable;
Ref the_player, the_activity, the_prefs;

// ------------------------------------------------------------------ FMOD audio device
struct FmodDev {
    Ref self;
    std::atomic<bool> running{false};
    uint32_t buf = 0; uint32_t block = 0;
    std::vector<int16_t> pending; size_t pos = 0;
};
FmodDev *fmod_dev;
void fmod_fill(int16_t *out, int frames);
int fmod_info(int what);
// Unity 3.4: the Java player owns the FMOD output thread; FMOD reports its rate once the engine initialised it
void fmod34_start(Obj *s) {
    if (!fmod_dev) fmod_dev = new FmodDev();
    fmod_dev->self = s->shared_from_this();
    fmod_dev->running = true;
    std::thread([] {
        for (int k = 0; k < 6000 && fmod_dev->running; k++) {
            int rate = fmod_info(0);
            if (rate > 0) { logf("FMOD audio: %d Hz, block %d", rate, fmod_info(1)); plat::audio_start(rate, fmod_fill); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }).detach();
}
// Unity 3.5 registers fmodGetInfo/fmodProcess, Unity 3.4 the lower-case fmodgetinfo/fmodprocess
static const char *fmod_n_info() { static const char *n = has_native(K_fmod, "fmodGetInfo", "(I)I") ? "fmodGetInfo" : "fmodgetinfo"; return n; }
static const char *fmod_n_process() { static const char *n = has_native(K_fmod, "fmodProcess", "(Ljava/nio/ByteBuffer;)I") ? "fmodProcess" : "fmodprocess"; return n; }
int fmod_info(int what) { return call_native(K_fmod, fmod_dev->self.get(), fmod_n_info(), "(I)I", {JV::I(what)}).i(); }
void fmod_fill(int16_t *out, int frames) {
    FmodDev *d = fmod_dev;
    int need = frames * 2;
    int k = 0;
    while (k < need) {
        if (d->pos >= d->pending.size()) {
            if (!d->running || fmod_info(0) <= 0 || fmod_info(3) != 1) { memset(out + k, 0, 2 * (need - k)); return; }
            uint32_t block = (uint32_t)fmod_info(1);
            if (!block) { memset(out + k, 0, 2 * (need - k)); return; }
            if (block != d->block) { if (d->buf) gfree(d->buf); d->buf = gmalloc(block * 4); d->block = block; }
            Ref bb = new_direct_buffer(d->buf, block * 4);
            call_native(K_fmod, d->self.get(), fmod_n_process(), "(Ljava/nio/ByteBuffer;)I", {JV::O(bb)});
            d->pending.assign((int16_t *)GPTR(d->buf), (int16_t *)GPTR(d->buf) + block * 2);
            d->pos = 0;
        }
        size_t n = std::min<size_t>(need - k, d->pending.size() - d->pos);
        memcpy(out + k, d->pending.data() + d->pos, 2 * n);
        d->pos += n; k += (int)n;
    }
}
}  // namespace

// ================================================================== public helpers for the host loop
namespace java {
void fmod_start34() { Ref dev = new_obj(K_fmod); fmod34_start(dev.get()); static Ref keep; keep = dev; }
Ref player() { return the_player; }
JV player_native(const char *name, const char *sig, const Args &a) {
    return call_native(K_player, the_player.get(), name, sig, a);
}
bool player_has(const char *name, const char *sig) { return has_native(K_player, name, sig); }
}

static void def_android() {
    JClass *obj = def_class("java/lang/Object", "");
    def_method(obj, "<init>", "()V", [](Obj *, Args &) { return nul(); });
    def_method(obj, "toString", "()Ljava/lang/String;", [](Obj *s, Args &) { return str(s && s->cls ? s->cls->name : "null"); });
    def_method(obj, "hashCode", "()I", [](Obj *s, Args &) { return JV::I((int)((uintptr_t)s >> 4)); });
    def_method(obj, "equals", "(Ljava/lang/Object;)Z", [](Obj *s, Args &a) { return JV::Z(a[0].o.get() == s || (a[0].o && s && a[0].o->str == s->str && s->cls && s->cls->name == "java/lang/String")); });
    def_method(obj, "getClass", "()Ljava/lang/Class;", [](Obj *s, Args &) { return JV::O(s && s->cls ? s->cls->class_obj : nullptr); });
    JClass *string = def_class("java/lang/String");
    def_method(string, "<init>", "([B)V", [](Obj *s, Args &a) { if (a[0].o) s->str.assign((const char *)GPTR(a[0].o->arr_data), a[0].o->arr_len); return nul(); });
    def_method(string, "<init>", "([BLjava/lang/String;)V", [](Obj *s, Args &a) { if (a[0].o) s->str.assign((const char *)GPTR(a[0].o->arr_data), a[0].o->arr_len); return nul(); });
    def_method(string, "getBytes", "()[B", [](Obj *s, Args &) { Ref r = new_prim_array('B', (uint32_t)s->str.size()); memcpy(GPTR(r->arr_data), s->str.data(), s->str.size()); return JV::O(r); });
    def_method(string, "getBytes", "(Ljava/lang/String;)[B", [](Obj *s, Args &) { Ref r = new_prim_array('B', (uint32_t)s->str.size()); memcpy(GPTR(r->arr_data), s->str.data(), s->str.size()); return JV::O(r); });
    def_method(string, "toString", "()Ljava/lang/String;", [](Obj *s, Args &) { return JV::O(s->shared_from_this()); });
    def_method(string, "length", "()I", [](Obj *s, Args &) { return JV::I((int)s->str.size()); });
    JClass *cls = def_class("java/lang/Class");
    def_method(cls, "getName", "()Ljava/lang/String;", [](Obj *s, Args &) { std::string n = s->as_class ? s->as_class->name : ""; for (char &ch : n) if (ch == '/') ch = '.'; return str(n); });
    def_static(cls, "forName", "(Ljava/lang/String;)Ljava/lang/Class;", [](Obj *, Args &a) {
        std::string n = a[0].s(); for (char &ch : n) if (ch == '.') ch = '/';
        JClass *k = find_class(n);
        if (!k) k = stub_class(n);
        if (!k) { throw_new("java/lang/ClassNotFoundException", n); return nul(); }
        return JV::O(k->class_obj);
    });
    def_class("java/lang/Throwable");
    def_class("java/lang/Exception", "java/lang/Throwable");
    def_class("java/lang/RuntimeException", "java/lang/Exception");
    def_class("java/lang/Runnable");
    def_class("java/lang/reflect/Method");
    def_class("java/lang/reflect/Field");
    def_class("java/lang/reflect/Constructor");
    def_class("java/nio/ByteBuffer");
    // boxed primitives (ReflectionHelper / AndroidJavaObject conversions)
    struct Box { const char *name, *sig; char t; };
    for (Box b : {Box{"java/lang/Integer", "I", 'I'}, Box{"java/lang/Float", "F", 'F'}, Box{"java/lang/Boolean", "Z", 'Z'},
                  Box{"java/lang/Long", "J", 'J'}, Box{"java/lang/Double", "D", 'D'}, Box{"java/lang/Short", "S", 'S'},
                  Box{"java/lang/Byte", "B", 'B'}, Box{"java/lang/Character", "C", 'C'}}) {
        JClass *k = def_class(b.name, "java/lang/Number");
        std::string s = b.sig;
        def_method(k, "<init>", "(" + s + ")V", [](Obj *self, Args &a) { set_field(self, "value", a[0]); return nul(); });
        def_static(k, "valueOf", "(" + s + ")L" + std::string(b.name) + ";", [k](Obj *, Args &a) { Ref o = new_obj(k); set_field(o.get(), "value", a[0]); return JV::O(o); });
        const char *getters[] = {"intValue", "floatValue", "booleanValue", "longValue", "doubleValue", "shortValue", "byteValue", "charValue"};
        const char *gs[] = {"()I", "()F", "()Z", "()J", "()D", "()S", "()B", "()C"};
        char src = b.t;
        for (int g = 0; g < 8; g++) {
            char dst = gs[g][2];
            def_method(k, getters[g], gs[g], [src, dst](Obj *self, Args &) {
                JV v = get_field(self, "value");
                double x = src == 'F' ? v.f() : src == 'D' ? v.d() : src == 'J' ? (double)v.j() : (double)v.i();
                if (dst == 'F') return JV::F((float)x);
                if (dst == 'D') return JV::D(x);
                if (dst == 'J') return JV::J((int64_t)x);
                return JV::I((int32_t)x);
            });
        }
    }
    def_class("java/lang/Number");

    JClass *build = def_class("android/os/Build");
    def_field(build, "MODEL", "Ljava/lang/String;", true, str("Reborn2Droid"));
    def_field(build, "MANUFACTURER", "Ljava/lang/String;", true, str("Reborn2Droid"));
    def_field(build, "DEVICE", "Ljava/lang/String;", true, str("port"));
    def_field(build, "PRODUCT", "Ljava/lang/String;", true, str("port"));
    def_field(build, "BRAND", "Ljava/lang/String;", true, str("generic"));
    def_field(build, "BOARD", "Ljava/lang/String;", true, str("port"));
    def_field(build, "HARDWARE", "Ljava/lang/String;", true, str("port"));
    def_field(build, "CPU_ABI", "Ljava/lang/String;", true, str("armeabi-v7a"));
    def_field(build, "CPU_ABI2", "Ljava/lang/String;", true, str("armeabi"));
    def_field(build, "ID", "Ljava/lang/String;", true, str("AIRATTACK"));
    def_field(build, "DISPLAY", "Ljava/lang/String;", true, str("port"));
    def_field(build, "FINGERPRINT", "Ljava/lang/String;", true, str("generic/port/port:4.4/AIRATTACK/1:user/release-keys"));
    JClass *ver = def_class("android/os/Build$VERSION");
    def_field(ver, "SDK_INT", "I", true, JV::I(19));          // what the 2014 game expects to see
    def_field(ver, "RELEASE", "Ljava/lang/String;", true, str("4.4.2"));
    def_field(ver, "INCREMENTAL", "Ljava/lang/String;", true, str("1"));
    def_field(ver, "CODENAME", "Ljava/lang/String;", true, str("REL"));
    JClass *clock = def_class("android/os/SystemClock");
    def_static(clock, "uptimeMillis", "()J", [](Obj *, Args &) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return JV::J((int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000); });
    def_static(clock, "elapsedRealtime", "()J", [](Obj *, Args &) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return JV::J((int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000); });
    JClass *kcm = def_class("android/view/KeyCharacterMap");
    def_static(kcm, "load", "(I)Landroid/view/KeyCharacterMap;", [kcm](Obj *, Args &) { return JV::O(new_obj(kcm)); });
    def_method(kcm, "get", "(II)I", [](Obj *, Args &a) {
        int kc = a[0].i();
        if (kc >= 29 && kc <= 54) return JV::I('a' + kc - 29);       // KEYCODE_A..Z
        if (kc >= 7 && kc <= 16) return JV::I('0' + kc - 7);         // KEYCODE_0..9
        if (kc == 62) return JV::I(' ');
        return JV::I(0);
    });
    JClass *loc = def_class("java/util/Locale");
    def_static(loc, "getDefault", "()Ljava/util/Locale;", [loc](Obj *, Args &) { Ref o = new_obj(loc); o->str = plat::language; return JV::O(o); });
    def_method(loc, "toString", "()Ljava/lang/String;", [](Obj *s, Args &) { return str(s->str); });
    def_method(loc, "getLanguage", "()Ljava/lang/String;", [](Obj *s, Args &) { return str(s->str.substr(0, 2)); });
    def_method(loc, "getCountry", "()Ljava/lang/String;", [](Obj *s, Args &) { return str(s->str.size() > 3 ? s->str.substr(3, 2) : ""); });
    def_method(loc, "getISO3Language", "()Ljava/lang/String;", [](Obj *s, Args &) {
        static const char *t[][2] = {{"en", "eng"}, {"tr", "tur"}, {"de", "deu"}, {"fr", "fra"}, {"es", "spa"}, {"it", "ita"}, {"ja", "jpn"},
                                     {"ko", "kor"}, {"zh", "zho"}, {"ru", "rus"}, {"pt", "por"}, {"nl", "nld"}, {"pl", "pol"}, {"ar", "ara"}};
        std::string l = s->str.substr(0, 2);
        for (auto &e : t) if (l == e[0]) return str(e[1]);
        return str("eng");
    });
    def_method(loc, "getISO3Country", "()Ljava/lang/String;", [](Obj *s, Args &) { return str(s->str.size() > 3 ? s->str.substr(3, 2) : "USA"); });
    def_method(loc, "getDisplayLanguage", "()Ljava/lang/String;", [](Obj *s, Args &) { return str(s->str.substr(0, 2)); });
    def_method(loc, "getDisplayName", "()Ljava/lang/String;", [](Obj *s, Args &) { return str(s->str); });
    JClass *map = def_class("java/util/HashMap");
    def_method(map, "<init>", "()V", [](Obj *, Args &) { return nul(); });
    def_method(map, "<init>", "(I)V", [](Obj *, Args &) { return nul(); });
    def_method(map, "get", "(Ljava/lang/Object;)Ljava/lang/Object;", [](Obj *s, Args &a) { return get_field(s, a[0].s()); });
    def_method(map, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", [](Obj *s, Args &a) { set_field(s, a[0].s(), a[1]); return nul(); });
    def_class("java/util/Map");
    JClass *bundle = def_class("android/os/Bundle");
    def_method(bundle, "getInt", "(Ljava/lang/String;)I", [](Obj *s, Args &a) { return JV::I(get_field(s, a[0].s()).i()); });
    def_method(bundle, "getBoolean", "(Ljava/lang/String;)Z", [](Obj *s, Args &a) { return JV::Z(get_field(s, a[0].s()).z()); });
    def_method(bundle, "getString", "(Ljava/lang/String;)Ljava/lang/String;", [](Obj *s, Args &a) { return get_field(s, a[0].s()); });
    def_class("android/content/Context", "java/lang/Object", true);
    def_class("android/app/Activity", "android/content/Context", true);
    def_class("android/view/View", "java/lang/Object", true);
}

static void def_unity() {
    // ---- the game's activity class (from the APK manifest; games often subclass UnityPlayerActivity to add plugins)
    K_activity = def_class(RB_ACTIVITY, std::string(RB_ACTIVITY) == "com/unity3d/player/UnityPlayerActivity" ? "android/app/Activity"
                                                                     : "com/unity3d/player/UnityPlayerActivity", true);
    def_class("com/unity3d/player/UnityPlayerActivity", "android/app/Activity", true);
    def_class("com/unity3d/player/UnityPlayerNativeActivity", "android/app/Activity", true);
    def_class("com/unity3d/player/UnityPlayerNativeActivityMy", "com/unity3d/player/UnityPlayerNativeActivity", true);
    def_method(K_activity, "runOnUiThread", "(Ljava/lang/Runnable;)V", [](Obj *, Args &a) {
        Ref r = a[0].o;
        plat::post([r] {
            if (!r || !r->cls) return;
            Args none;
            JClass *k = r->cls;
            for (JClass *c = k; c; c = c->super) {
                auto it = c->methods.find("run()V");
                if (it != c->methods.end() && it->second->impl) { it->second->impl(r.get(), none); return; }
            }
            if (has_native(k, "run", "()V")) call_native(k, r.get(), "run", "()V", none);
        });
        return nul();
    });
    // Moga gamepad SDK: a controller that is never connected (getState* -> 0, key codes -> ACTION_UP)
    static JClass *moga = def_class("com/bda/controller/Controller", "java/lang/Object", true);
    static Ref moga_obj = new_obj(moga);
    def_method(moga, "getState", "(I)I", [](Obj *, Args &) { return JV::I(0); });
    def_method(moga, "getKeyCode", "(I)I", [](Obj *, Args &) { return JV::I(1); });
    def_method(moga, "getAxisValue", "(I)F", [](Obj *, Args &) { return JV::F(0); });
    def_method(moga, "init", "()Z", [](Obj *, Args &) { return JV::Z(true); });
    for (const char *sig : {"()Lcom/bda/controller/Controller;", "()Ljava/lang/Object;"})
        def_method(K_activity, "GetMogaController", sig, [](Obj *, Args &) { return JV::O(moga_obj); });
    def_method(K_activity, "SetFlurryUserId", "(Ljava/lang/String;)V", [](Obj *, Args &) { return nul(); });
    def_method(K_activity, "FlurryLogEvent", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", [](Obj *, Args &) { return nul(); });
    def_method(K_activity, "getPackageName", "()Ljava/lang/String;", [](Obj *, Args &) { return str(plat::package_name); });
    def_method(K_activity, "finish", "()V", [](Obj *, Args &) { plat::quit(); return nul(); });
    def_method(K_activity, "getApplicationContext", "()Landroid/content/Context;", [](Obj *, Args &) { return JV::O(the_activity); });
    the_activity = new_obj(K_activity);

    // ---- plugins: offline no-ops (servers and SDKs are long gone)
    JClass *admob = def_class("com/unity3d/Plugin/AdMobFacade", "java/lang/Object", true);
    for (const char *n : {"InitBannerAds", "SetupAds", "ShowAds", "RequestFreshAd", "HideAds", "ShowInterstitial", "LoadInterstitial"})
        def_static(admob, n, "(Landroid/app/Activity;)V", [](Obj *, Args &) { return nul(); });
    def_static(admob, "InitAdMob", "(Z)V", [](Obj *, Args &) { return nul(); });
    def_class("com/flurry/android/FlurryAgent", "java/lang/Object", true);

    // ---- UnityPlayer
    K_player = def_class("com/unity3d/player/UnityPlayer", "android/view/View", true);
    def_field(K_player, "currentActivity", "Landroid/app/Activity;", true, JV::O(the_activity));
    the_player = new_obj(K_player);
    auto M = [](const char *n, const char *s, Impl f) { def_method(K_player, n, s, std::move(f)); };
    M("getFilesDir", "()Ljava/lang/String;", [](Obj *, Args &) { return str(plat::files_dir); });
    M("getCacheDir", "()Ljava/lang/String;", [](Obj *, Args &) { return str(plat::cache_dir); });
    M("getPackageName", "()Ljava/lang/String;", [](Obj *, Args &) { return str(plat::package_name); });
    M("getCPUType", "()Ljava/lang/String;", [](Obj *, Args &) { return str("ARMv7 VFPv3 NEON"); });
    M("getDeviceUniqueIdentifier", "()Ljava/lang/String;", [](Obj *, Args &) { return str(plat::device_id); });
    M("getDisplaySize", "()[I", [](Obj *, Args &) {
        Ref a = new_prim_array('I', 2);
        W32(a->arr_data, std::max(plat::width, plat::height)); W32(a->arr_data + 4, std::min(plat::width, plat::height));
        return JV::O(a);
    });
    M("getScreenDPI", "()F", [](Obj *, Args &) { return JV::F(plat::dpi); });
    M("getScreenTimeout", "()I", [](Obj *, Args &) { return JV::I(60); });
    M("getTotalMemory", "()I", [](Obj *, Args &) { return JV::I(2048); });
    M("getInternetReachability", "()I", [](Obj *, Args &) { return JV::I(0); });
    M("getOrientation", "()I", [](Obj *, Args &) { return JV::I(plat::requested_orientation()); });
    M("getDeviceOrientation", "()I", [](Obj *, Args &) { return JV::I(plat::is_portrait() ? 1 : 0); });   // ActivityInfo values
    M("getNumCameras", "()I", [](Obj *, Args &) { return JV::I(0); });
    M("getIsGyroAvailable", "()Z", [](Obj *, Args &) { return JV::Z(false); });
    M("getIsGyroEnabled", "()Z", [](Obj *, Args &) { return JV::Z(false); });
    M("getGyroUpdateDelay", "()I", [](Obj *, Args &) { return JV::I(20000); });
    M("isCompassAvailable", "()Z", [](Obj *, Args &) { return JV::Z(false); });
    M("isCompassEnabled", "()Z", [](Obj *, Args &) { return JV::Z(false); });
    M("isFinishing", "()Z", [](Obj *, Args &) { return JV::Z(false); });
    M("hasWakeLock", "()Z", [](Obj *, Args &) { return JV::Z(true); });
    M("loadLibrary", "(Ljava/lang/String;)Z", [](Obj *, Args &) { return JV::Z(true); });
    M("getConnectedJoysticks", "()[I", [](Obj *, Args &) { return JV::O(new_prim_array('I', 0)); });
    M("getJoystickName", "(I)Ljava/lang/String;", [](Obj *, Args &) { return str(""); });
    M("getJoystickAxes", "(I)[I", [](Obj *, Args &) { return JV::O(new_prim_array('I', 0)); });
    M("showBuildSetup", "()Z", [](Obj *, Args &) { return JV::Z(false); });
    M("showRuntimeSetup", "()Z", [](Obj *, Args &) { return JV::Z(false); });
    M("openURL", "(Ljava/lang/String;)V", [](Obj *, Args &a) { plat::open_url(a[0].s()); return nul(); });
    M("vibrate", "(I)V", [](Obj *, Args &a) { plat::vibrate(a[0].i()); return nul(); });
    M("setWakeLock", "(Z)V", [](Obj *, Args &a) { plat::keep_screen_on(a[0].z()); return nul(); });
    M("kill", "()V", [](Obj *, Args &) { plat::quit(); return nul(); });
    M("quit", "()V", [](Obj *, Args &) { plat::quit(); return nul(); });
    M("getSettings", "()Landroid/os/Bundle;", [](Obj *, Args &) {
        Ref b = new_obj(def_class("android/os/Bundle"));
        set_field(b.get(), "gles_mode", JV::I(RB_GLES_MODE)); set_field(b.get(), "splash_mode", JV::I(RB_SPLASH_MODE));
        set_field(b.get(), "hide_status_bar", JV::Z(true)); set_field(b.get(), "default_aa", JV::I(0));
        return JV::O(b);
    });
    for (const char *n : {"setScreenSize", "setOrientation", "setHideInputField", "setGyroEnabled", "setCompassEnabled",
                          "setGyroUpdateDelay", "hideSoftInput", "startActivityIndicator", "stopActivityIndicator", "closeCamera"})
        (void)n;
    M("setScreenSize", "(II)V", [](Obj *, Args &) { return nul(); });
    M("setOrientation", "(I)V", [](Obj *, Args &a) { plat::set_orientation(a[0].i()); return nul(); });
    M("setHideInputField", "(Z)V", [](Obj *, Args &) { return nul(); });
    M("setGyroEnabled", "(Z)V", [](Obj *, Args &) { return nul(); });
    M("setCompassEnabled", "(Z)V", [](Obj *, Args &) { return nul(); });
    M("setGyroUpdateDelay", "(I)V", [](Obj *, Args &) { return nul(); });
    M("hideSoftInput", "()V", [](Obj *, Args &) { return nul(); });
    M("startActivityIndicator", "()V", [](Obj *, Args &) { return nul(); });
    M("stopActivityIndicator", "()V", [](Obj *, Args &) { return nul(); });
    M("closeCamera", "(I)V", [](Obj *, Args &) { return nul(); });
    M("showSoftInput", "(Ljava/lang/String;IZZZZLjava/lang/String;)V", [](Obj *, Args &) { return nul(); });
    M("showVideoPlayer", "(Ljava/lang/String;IIIZ)V", [](Obj *, Args &a) { logf("video playback requested (%s): skipped", a[0].s().c_str()); return nul(); });
    M("reportSoftInputStr", "(Ljava/lang/String;I)V", [](Obj *, Args &) { return nul(); });
    M("forwardMotionEventToDalvik", "(JJII[I[FIFFIIIII[J[F)V", [](Obj *, Args &) { return nul(); });
    M("onNativeKey", "(JJIIIIIIIIZ)Z", [](Obj *, Args &) { return JV::Z(false); });
    M("dispatchTouchEvent", "(IIIFFJI)Z", [](Obj *, Args &) { return JV::Z(true); });

    // ---- PlayerPrefs (instance methods on the PlayerPrefs object the native side keeps)
    K_prefs = def_class("com/unity3d/player/PlayerPrefs");
    using plat::Prefs;
    def_method(K_prefs, "GetInt", "(Ljava/lang/String;I)I", [](Obj *, Args &a) { return JV::I(Prefs::get_int(a[0].s(), a[1].i())); });
    def_method(K_prefs, "GetFloat", "(Ljava/lang/String;F)F", [](Obj *, Args &a) { return JV::F(Prefs::get_float(a[0].s(), a[1].f())); });
    def_method(K_prefs, "GetString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [](Obj *, Args &a) {
        if (!Prefs::has(a[0].s())) return a[1];
        return str(Prefs::get_string(a[0].s(), a[1].s()));
    });
    def_method(K_prefs, "HasKey", "(Ljava/lang/String;)Z", [](Obj *, Args &a) { return JV::Z(Prefs::has(a[0].s())); });
    def_method(K_prefs, "SetInt", "(Ljava/lang/String;I)Z", [](Obj *, Args &a) { Prefs::set_int(a[0].s(), a[1].i()); return JV::Z(true); });
    def_method(K_prefs, "SetFloat", "(Ljava/lang/String;F)Z", [](Obj *, Args &a) { Prefs::set_float(a[0].s(), a[1].f()); return JV::Z(true); });
    def_method(K_prefs, "SetString", "(Ljava/lang/String;Ljava/lang/String;)Z", [](Obj *, Args &a) { Prefs::set_string(a[0].s(), a[1].s()); return JV::Z(true); });
    def_method(K_prefs, "DeleteKey", "(Ljava/lang/String;)V", [](Obj *, Args &a) { Prefs::remove(a[0].s()); return nul(); });
    def_method(K_prefs, "DeleteAll", "()V", [](Obj *, Args &) { Prefs::clear(); return nul(); });
    def_method(K_prefs, "Sync", "()V", [](Obj *, Args &) { Prefs::sync(); return nul(); });
    the_prefs = new_obj(K_prefs);

    // ---- ReflectionHelper (AndroidJavaObject / AndroidJavaClass in managed code)
    JClass *rh = def_class("com/unity3d/player/ReflectionHelper");
    def_static(rh, "getMethodID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;", [](Obj *, Args &a) {
        return JV::O(reflect_method(a[0].o ? a[0].o->as_class : nullptr, a[1].s(), a[2].s(), a[3].z()));
    });
    def_static(rh, "getFieldID", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;", [](Obj *, Args &a) {
        return JV::O(reflect_field(a[0].o ? a[0].o->as_class : nullptr, a[1].s(), a[2].s(), a[3].z()));
    });
    def_static(rh, "getConstructorID", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;", [](Obj *, Args &a) {
        Ref m = reflect_method(a[0].o ? a[0].o->as_class : nullptr, "<init>", a[1].s(), false);
        return JV::O(m);
    });

    // ---- UnityJavaRunnable (AndroidJavaRunnable)
    K_runnable = def_class("com/unity3d/player/UnityJavaRunnable", "java/lang/Runnable");
    def_method(K_runnable, "<init>", "(I)V", [](Obj *s, Args &a) { set_field(s, "ptr", a[0]); return nul(); });
    def_method(K_runnable, "run", "()V", [](Obj *s, Args &) {
        call_native(K_runnable, s, "nativeRun", "(I)V", {get_field(s, "ptr")});
        return nul();
    });

    // ---- WWW: no network for this offline port; every request fails like a dead server would
    // java.lang.Thread: start() runs run() on a new host thread, join() waits for it
    struct JThread { std::mutex m; std::condition_variable cv; bool started = false, done = false; };
    JClass *thr = def_class("java/lang/Thread", "java/lang/Object", true);
    def_method(thr, "start", "()V", [](Obj *s, Args &) {
        auto st = std::make_shared<JThread>(); s->peer = st; st->started = true;
        Ref self = s->shared_from_this();
        std::thread([self, st] {
            Args none;
            for (JClass *c = self->cls; c; c = c->super) {
                auto it = c->methods.find("run()V");
                if (it != c->methods.end() && it->second->impl) { it->second->impl(self.get(), none); break; }
            }
            std::lock_guard<std::mutex> g(st->m); st->done = true; st->cv.notify_all();
        }).detach();
        return nul();
    });
    def_method(thr, "join", "()V", [](Obj *s, Args &) {
        auto st = std::static_pointer_cast<JThread>(s->peer);
        if (st) { std::unique_lock<std::mutex> lk(st->m); st->cv.wait(lk, [&] { return st->done; }); }
        return nul();
    });
    def_method(thr, "isAlive", "()Z", [](Obj *s, Args &) {
        auto st = std::static_pointer_cast<JThread>(s->peer);
        if (!st) return JV::Z(false);
        std::lock_guard<std::mutex> g(st->m); return JV::Z(st->started && !st->done);
    });
    def_method(thr, "interrupt", "()V", [](Obj *, Args &) { return nul(); });
    def_method(thr, "setPriority", "(I)V", [](Obj *, Args &) { return nul(); });
    def_method(thr, "run", "()V", [](Obj *, Args &) { return nul(); });

    // WWW: no network in this offline port; every request fails like an unreachable server would
    K_www = def_class("com/unity3d/player/WWW", "java/lang/Thread");
    def_method(K_www, "<init>", "(ILjava/lang/String;[BLjava/util/Map;)V", [](Obj *s, Args &a) {
        set_field(s, "id", a[0]); set_field(s, "url", a[1]);
        Args none;
        find_class("java/lang/Thread")->methods["start()V"]->impl(s, none);
        return nul();
    });
    def_method(K_www, "run", "()V", [](Obj *s, Args &) {
        int id = get_field(s, "id").i();
        if (verbose) logf("WWW %s -> offline", get_field(s, "url").s().c_str());
        call_native(K_www, nullptr, "errorCallback", "(ILjava/lang/String;)V", {JV::I(id), str("Could not resolve host")});
        return nul();
    });

    // ---- FMOD audio output
    K_fmod = def_class("org/fmod/FMODAudioDevice");
    def_method(K_fmod, "a", "()V", [](Obj *s, Args &) {          // Unity 3.4 (obfuscated start())
        fmod34_start(s);
        return nul();
    });
    def_method(K_fmod, "b", "()V", [](Obj *, Args &) { if (fmod_dev) fmod_dev->running = false; plat::audio_stop(); return nul(); });
    if (0) def_method(K_fmod, "unused", "()V", [](Obj *s, Args &) {
        if (!fmod_dev) fmod_dev = new FmodDev();
        fmod_dev->self = s->shared_from_this();
        fmod_dev->running = true;
        std::thread([] {                                          // FMOD initialises later on the game thread
            for (int k = 0; k < 2000 && fmod_dev->running; k++) {
                int rate = fmod_info(0);
                if (rate > 0) { logf("FMOD audio: %d Hz, block %d", rate, fmod_info(1)); plat::audio_start(rate, fmod_fill); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }).detach();
        return nul();
    });
    def_method(K_fmod, "<init>", "()V", [](Obj *s, Args &) {
        if (!fmod_dev) fmod_dev = new FmodDev();
        fmod_dev->self = s->shared_from_this();
        return nul();
    });
    def_method(K_fmod, "start", "()V", [](Obj *s, Args &) {
        if (!fmod_dev) { fmod_dev = new FmodDev(); }
        fmod_dev->self = s->shared_from_this();
        call_native(K_fmod, s, "fmodInitJni", "()I", {});
        fmod_dev->running = true;
        int rate = fmod_info(0);
        logf("FMOD audio: %d Hz, block %d", rate, fmod_info(1));
        plat::audio_start(rate > 0 ? rate : 44100, fmod_fill);
        return nul();
    });
    def_method(K_fmod, "stop", "()V", [](Obj *, Args &) { if (fmod_dev) fmod_dev->running = false; plat::audio_stop(); return nul(); });
    def_method(K_fmod, "isRunning", "()Z", [](Obj *, Args &) { return JV::Z(fmod_dev && fmod_dev->running); });
    def_method(K_fmod, "startAudioRecord", "(III)I", [](Obj *, Args &) { return JV::I(-1); });
    def_method(K_fmod, "stopAudioRecord", "()V", [](Obj *, Args &) { return nul(); });
}

void java_classes_init() {
    def_android();
    def_unity();
}

namespace java {
Ref prefs_object() { return the_prefs; }
}
