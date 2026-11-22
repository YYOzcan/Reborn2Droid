// Host program (PC and Android): SDL window + GL context, then drives libunity exactly like the
// original com.unity3d.player.UnityPlayer Java class did (init sequence, GL render loop, input).
#include "rt.h"
#include "thread.h"
#include "jvm.h"
#include "platform.h"
#include "rt_gl.h"
#include "host.h"
#include "profile.h"
#include <SDL.h>
#include <deque>
#include <mutex>
#include <fstream>
#include <sstream>
#include <map>
#include <atomic>
#include <cmath>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <jni.h>
#include <sys/system_properties.h>
#endif

using namespace rt;

namespace java { jni::Ref activity(); jni::Ref motion_event(int, float, float, int, int64_t); jni::Ref surface(); void fmod_start34(); jni::Ref player(); jni::JV player_native(const char *n, const char *s, const jni::Args &a); bool player_has(const char *n, const char *s); jni::Ref prefs_object(); }
extern bool gl_desktop_es3;

// =================================================================== platform services
namespace plat {
int width = 1280, height = 720;
float dpi = 160;
std::string files_dir, cache_dir, data_dir, apk_path, package_name = RB_PACKAGE, language = "en_US", device_id;
bool is_android = false;
int android_sdk = 0;
static std::atomic<bool> quitting{false};
static std::mutex post_mu;
static std::deque<std::function<void()>> posted;

void open_url(const std::string &url) { logf("openURL %s", url.c_str()); if (!url.empty() && url.rfind("market://", 0) != 0) SDL_OpenURL(url.c_str()); }
void vibrate(int ms) { (void)ms; }
void keep_screen_on(bool on) { if (on) SDL_DisableScreenSaver(); else SDL_EnableScreenSaver(); }
void quit() { quitting = true; }
static int req_orient = RB_ORIENTATION;             // from the game's manifest
static SDL_Window *g_win;
int requested_orientation() { return req_orient; }
bool is_portrait() { return height > width; }
static bool portrait_value(int o) { return o == 1 || o == 7 || o == 9 || o == 12; }
void set_orientation(int o) {
    if (o == req_orient) return;
    req_orient = o;
    logf("orientation request %d", o);
#ifdef __ANDROID__
    JNIEnv *e = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    jclass k = e->GetObjectClass(act);
    jmethodID m = e->GetStaticMethodID(k, "requestOrientation", "(I)V");
    // Unity asks for fixed sides (1 portrait, 0 landscape): let the sensor pick the matching side
    int want = o == 1 ? 7 : o == 0 ? 6 : o;
    if (m) e->CallStaticVoidMethod(k, m, want);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(k); e->DeleteLocalRef(act);
#else
    if (g_win && !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
        int w, h; SDL_GetWindowSize(g_win, &w, &h);
        bool want_portrait = portrait_value(o);
        if (want_portrait != (h > w)) SDL_SetWindowSize(g_win, h, w);
    }
#endif
}
void post(std::function<void()> fn) { std::lock_guard<std::mutex> g(post_mu); posted.push_back(std::move(fn)); }
static void run_posted() {
    std::deque<std::function<void()>> q;
    { std::lock_guard<std::mutex> g(post_mu); q.swap(posted); }
    for (auto &f : q) f();
}

// ---- audio
static SDL_AudioDeviceID audio_dev;
static std::function<void(int16_t *, int)> audio_fill;
static void SDLCALL audio_cb(void *, Uint8 *stream, int len) {
    if (audio_fill) audio_fill((int16_t *)stream, len / 4);
    else memset(stream, 0, len);
}
void audio_start(int rate, std::function<void(int16_t *, int)> fill) {
    if (audio_dev) return;
    audio_fill = std::move(fill);
    SDL_AudioSpec want{}, have{};
    want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!audio_dev) { logf("audio: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(audio_dev, 0);
}
void audio_stop() { if (audio_dev) { SDL_CloseAudioDevice(audio_dev); audio_dev = 0; } }
void audio_pause(bool p) { if (audio_dev) SDL_PauseAudioDevice(audio_dev, p ? 1 : 0); }

// ---- prefs: "type key value" lines with escapes, saved atomically
namespace {
struct PV { char t; std::string s; };
std::recursive_mutex pmu;
std::map<std::string, PV> pmap;
bool ploaded = false, pdirty = false;
std::string pfile() { return files_dir + "/../shared_prefs.txt"; }
std::string esc(const std::string &s) { std::string r; for (char c : s) { if (c == '\\') r += "\\\\"; else if (c == '\n') r += "\\n"; else if (c == '\t') r += "\\t"; else r += c; } return r; }
std::string unesc(const std::string &s) { std::string r; for (size_t i = 0; i < s.size(); i++) { if (s[i] == '\\' && i + 1 < s.size()) { char n = s[++i]; r += n == 'n' ? '\n' : n == 't' ? '\t' : n; } else r += s[i]; } return r; }
// profile "pins": PlayerPrefs entries forced to a value (in-game currencies kept at their maximum)
struct Pin { const char *type, *key, *value; };
const Pin pins[] = RB_PREF_PINS;
const Pin *pin_of(const std::string &k) { for (const Pin *p = pins; p->key; p++) if (k == p->key) return p; return nullptr; }
void apply_pins() { for (const Pin *p = pins; p->key; p++) pmap[p->key] = PV{p->type[0], p->value}; }
void pload() {
    if (ploaded) return; ploaded = true;
    struct AtEnd { ~AtEnd() { apply_pins(); } } at_end;
    std::ifstream f(pfile()); std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find('\t'); if (a == std::string::npos || a != 1) continue;
        size_t b = line.find('\t', a + 1); if (b == std::string::npos) continue;
        pmap[unesc(line.substr(a + 1, b - a - 1))] = PV{line[0], unesc(line.substr(b + 1))};
    }
}
}
int Prefs::get_int(const std::string &k, int d) { std::lock_guard<std::recursive_mutex> g(pmu); pload(); auto it = pmap.find(k); return it != pmap.end() && it->second.t == 'i' ? atoi(it->second.s.c_str()) : d; }
float Prefs::get_float(const std::string &k, float d) { std::lock_guard<std::recursive_mutex> g(pmu); pload(); auto it = pmap.find(k); return it != pmap.end() && it->second.t == 'f' ? strtof(it->second.s.c_str(), nullptr) : d; }
std::string Prefs::get_string(const std::string &k, const std::string &d) { std::lock_guard<std::recursive_mutex> g(pmu); pload(); auto it = pmap.find(k); return it != pmap.end() && it->second.t == 's' ? it->second.s : d; }
bool Prefs::has(const std::string &k) { std::lock_guard<std::recursive_mutex> g(pmu); pload(); return pmap.count(k) != 0; }
void Prefs::set_int(const std::string &k, int v) { std::lock_guard<std::recursive_mutex> g(pmu); pload(); if (pin_of(k)) return; pmap[k] = PV{'i', std::to_string(v)}; pdirty = true; }
void Prefs::set_float(const std::string &k, float v) { std::lock_guard<std::recursive_mutex> g(pmu); pload(); if (pin_of(k)) return; char b[64]; snprintf(b, sizeof b, "%.9g", v); pmap[k] = PV{'f', b}; pdirty = true; }
void Prefs::set_string(const std::string &k, const std::string &v) { std::lock_guard<std::recursive_mutex> g(pmu); pload(); if (pin_of(k)) return; pmap[k] = PV{'s', v}; pdirty = true; }
void Prefs::remove(const std::string &k) { std::lock_guard<std::recursive_mutex> g(pmu); pload(); if (pin_of(k)) return; pmap.erase(k); pdirty = true; }
void Prefs::clear() { std::lock_guard<std::recursive_mutex> g(pmu); pload(); pmap.clear(); apply_pins(); pdirty = true; }
void Prefs::sync() {
    std::lock_guard<std::recursive_mutex> g(pmu);
    if (!pdirty) return;
    std::string tmp = pfile() + ".tmp";
    { std::ofstream f(tmp, std::ios::trunc); for (auto &kv : pmap) f << kv.second.t << '\t' << esc(kv.first) << '\t' << esc(kv.second.s) << '\n'; }
    ::rename(tmp.c_str(), pfile().c_str());
    pdirty = false;
}
}  // namespace plat

// =================================================================== GL loading
static bool g_es = false, g_emu = false;
namespace gles1 { bool init(void *(*get)(const char *)); void *lookup(const char *name, void *(*get)(const char *)); void set_program_cache(const std::string &path); }
extern "C" { typedef void (*GLfn)(void); }
static void my_glClearDepthf(float d) { typedef void (*F)(double); static F f = (F)SDL_GL_GetProcAddress("glClearDepth"); if (f) f(d); }
static void my_glDepthRangef(float a, float b) { typedef void (*F)(double, double); static F f = (F)SDL_GL_GetProcAddress("glDepthRange"); if (f) f(a, b); }
static void my_glFogx(unsigned p, int v) { typedef void (*F)(unsigned, int); static F f = (F)SDL_GL_GetProcAddress("glFogi"); if (f) f(p, v); }
#ifdef __ANDROID__
#include <dlfcn.h>
#endif
static void *gl_get_es2(const char *name) {
#ifdef __ANDROID__
    static void *lib = dlopen("libGLESv2.so", RTLD_NOW);
    if (lib) { if (void *q = dlsym(lib, name)) return q; }
#endif
    return SDL_GL_GetProcAddress(name);
}
static void *gl_get_emu(const char *name) { return gles1::lookup(name, gl_get_es2); }
static void *gl_get(const char *name) {
    if (g_emu) return gl_get_emu(name);
    if (RB_GLES_MODE != 1) return gl_get_es2(name);   // GLES 2 game on a GLES 2 context
#ifdef __ANDROID__
    // the emulator's GLES 1 translator has no glDeleteFramebuffersOES behind its dispatch stub
    static bool emu = [] { typedef const char *(*GS)(unsigned); void *l = dlopen("libGLESv1_CM.so", RTLD_NOW);
                           GS gs = l ? (GS)dlsym(l, "glGetString") : nullptr; const char *r = gs ? gs(0x1F01) : nullptr;
                           return r && strstr(r, "Emulator"); }();
    if (emu && (!strcmp(name, "glDeleteFramebuffersOES") || !strcmp(name, "glDeleteFramebuffers"))) return (void *)+[](int, const unsigned *) {};
    static void *lib = dlopen("libGLESv1_CM.so", RTLD_NOW);
    if (lib) { if (void *q = dlsym(lib, name)) return q; }
    // GLES 1.1 has many GLES 2 entry points only as OES extensions (framebuffers, separate blending,
    // mipmap generation): prefer those, because the driver's generic stub for an unsupported core name
    // can jump to nowhere
    std::string nm = name;
    if (nm.size() < 3 || (nm.compare(nm.size() - 3, 3, "OES") != 0 && nm.compare(nm.size() - 3, 3, "EXT") != 0)) {
        std::string o = nm + "OES";
        if (lib) { if (void *q = dlsym(lib, o.c_str())) { logf("GL %s -> %s (lib)", name, o.c_str()); return q; } }
        if (void *q = SDL_GL_GetProcAddress(o.c_str())) { logf("GL %s -> %s (egl)", name, o.c_str()); return q; }
        if (nm.find("Framebuffer") != std::string::npos || nm.find("Renderbuffer") != std::string::npos || nm.find("Mipmap") != std::string::npos ||
            nm.find("BlendEquation") != std::string::npos) return nullptr;
    }
#endif
    void *p = SDL_GL_GetProcAddress(name);
    if (p) return p;
    if (!g_es) {
        if (!strcmp(name, "glClearDepthf")) return (void *)my_glClearDepthf;
        if (!strcmp(name, "glDepthRangef")) return (void *)my_glDepthRangef;
        if (!strcmp(name, "glFogx")) return (void *)my_glFogx;
    }
    std::string n = name;
    for (const char *suf : {"OES", "EXT", "ARB"})
        if (n.size() > 3 && n.compare(n.size() - 3, 3, suf) == 0) { p = SDL_GL_GetProcAddress(n.substr(0, n.size() - 3).c_str()); if (p) return p; }
    return nullptr;
}

// =================================================================== Unity driving
static jni::JV P(const char *n, const char *s, const jni::Args &a = {}) { return java::player_native(n, s, a); }
// profile max_aspect: on wider screens the game gets a centred area of that aspect ratio (black side bars)
extern int gl_xoff; void gl_draw_bars(int surf_w, int surf_h);
static int surf_w, surf_h;
static void apply_aspect() {
    surf_w = plat::width; surf_h = plat::height; gl_xoff = 0;
    float ma = RB_MAX_ASPECT;
    if (ma > 0 && surf_h > 0 && surf_w > surf_h && (float)surf_w / surf_h > ma + 0.01f) {
        int gw = (int)(surf_h * ma + 0.5f) & ~1;
        gl_xoff = (surf_w - gw) / 2; plat::width = gw;
    }
}
using jni::JV;

// Unity 4.x Screen.SetResolution: the engine renders a w x h backbuffer at the bottom-left of the window;
// the host scales it up to the window before presenting
extern int gl_xoff;
static int bb_w, bb_h;
void host_set_backbuffer(int w, int h) { bb_w = w; bb_h = h; }
static void upscale_backbuffer(int W, int H) {
    if (bb_w <= 0 || bb_h <= 0 || (bb_w >= W && bb_h >= H)) return;
    typedef unsigned GLu; typedef int GLi;
    #define GLF(ret, name, ...) static auto name = (ret (*)(__VA_ARGS__))SDL_GL_GetProcAddress(#name)
    GLF(void, glGetIntegerv, GLu, GLi *); GLF(void, glGetBooleanv, GLu, unsigned char *);
    GLF(void, glGenTextures, GLi, GLu *); GLF(void, glBindTexture, GLu, GLu); GLF(void, glActiveTexture, GLu);
    GLF(void, glTexImage2D, GLu, GLi, GLi, GLi, GLi, GLi, GLu, GLu, const void *); GLF(void, glTexParameteri, GLu, GLu, GLi);
    GLF(void, glCopyTexSubImage2D, GLu, GLi, GLi, GLi, GLi, GLi, GLi, GLi);
    GLF(GLu, glCreateShader, GLu); GLF(void, glShaderSource, GLu, GLi, const char *const *, const GLi *); GLF(void, glCompileShader, GLu);
    GLF(GLu, glCreateProgram); GLF(void, glAttachShader, GLu, GLu); GLF(void, glBindAttribLocation, GLu, GLu, const char *);
    GLF(void, glLinkProgram, GLu); GLF(void, glUseProgram, GLu); GLF(GLi, glGetUniformLocation, GLu, const char *); GLF(void, glUniform1i, GLi, GLi);
    GLF(void, glBindBuffer, GLu, GLu); GLF(void, glBindFramebuffer, GLu, GLu); GLF(void, glViewport, GLi, GLi, GLi, GLi);
    GLF(void, glEnable, GLu); GLF(void, glDisable, GLu); GLF(void, glColorMask, unsigned char, unsigned char, unsigned char, unsigned char);
    GLF(void, glGetVertexAttribiv, GLu, GLu, GLi *); GLF(void, glGetVertexAttribPointerv, GLu, GLu, void **);
    GLF(void, glVertexAttribPointer, GLu, GLi, GLu, unsigned char, GLi, const void *);
    GLF(void, glEnableVertexAttribArray, GLu); GLF(void, glDisableVertexAttribArray, GLu); GLF(void, glDrawArrays, GLu, GLi, GLi);
    #undef GLF
    static GLu tex, prog; static int tw, th;
    if (!prog) {
        const char *vs = "attribute vec2 p; varying vec2 v; uniform vec2 s; void main(){ v = (p * 0.5 + 0.5) * s; gl_Position = vec4(p, 0.0, 1.0); }";
        const char *fs = "precision mediump float; varying vec2 v; uniform sampler2D t; void main(){ gl_FragColor = texture2D(t, v); }";
        GLu a = glCreateShader(0x8B31), b = glCreateShader(0x8B30);
        glShaderSource(a, 1, &vs, nullptr); glCompileShader(a); glShaderSource(b, 1, &fs, nullptr); glCompileShader(b);
        prog = glCreateProgram(); glAttachShader(prog, a); glAttachShader(prog, b); glBindAttribLocation(prog, 0, "p"); glLinkProgram(prog);
        glGenTextures(1, &tex);
    }
    // save what the engine may have cached
    GLi o_prog, o_unit, o_tex, o_ab, o_fb, o_vp[4]; unsigned char o_mask[4];
    glGetIntegerv(0x8B8D, &o_prog); glGetIntegerv(0x84E0, &o_unit); glGetIntegerv(0x8894, &o_ab); glGetIntegerv(0x8CA6, &o_fb);
    glGetIntegerv(0x0BA2, o_vp); glGetBooleanv(0x0C23, o_mask);
    unsigned caps[] = {0x0C11, 0x0BE2, 0x0B71, 0x0B44, 0x0B90, 0x8037};
    unsigned char o_caps[6]; for (int k = 0; k < 6; k++) glGetBooleanv(caps[k], &o_caps[k]);
    GLi a_en, a_size, a_type, a_norm, a_stride, a_buf; void *a_ptr = nullptr;
    glGetVertexAttribiv(0, 0x8622, &a_en); glGetVertexAttribiv(0, 0x8623, &a_size); glGetVertexAttribiv(0, 0x8625, &a_type);
    glGetVertexAttribiv(0, 0x886A, &a_norm); glGetVertexAttribiv(0, 0x8624, &a_stride); glGetVertexAttribiv(0, 0x889F, &a_buf);
    glGetVertexAttribPointerv(0, 0x8645, &a_ptr);
    glActiveTexture(0x84C0); glGetIntegerv(0x8069, &o_tex);
    // copy the backbuffer region to a texture and draw it over the whole window
    glBindFramebuffer(0x8D40, 0); glBindTexture(0x0DE1, tex);
    if (tw != bb_w || th != bb_h) {
        glTexImage2D(0x0DE1, 0, 0x1907, bb_w, bb_h, 0, 0x1907, 0x1401, nullptr); tw = bb_w; th = bb_h;
        glTexParameteri(0x0DE1, 0x2801, 0x2601); glTexParameteri(0x0DE1, 0x2800, 0x2601);
        glTexParameteri(0x0DE1, 0x2802, 0x812F); glTexParameteri(0x0DE1, 0x2803, 0x812F);
    }
    glCopyTexSubImage2D(0x0DE1, 0, 0, 0, gl_xoff, 0, bb_w, bb_h);
    for (int k = 0; k < 6; k++) glDisable(caps[k]);
    glColorMask(1, 1, 1, 1); glViewport(gl_xoff, 0, W, H);
    glUseProgram(prog); glUniform1i(glGetUniformLocation(prog, "t"), 0);
    static auto u2 = (void (*)(GLi, float, float))SDL_GL_GetProcAddress("glUniform2f");
    u2(glGetUniformLocation(prog, "s"), 1.0f, 1.0f);
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glBindBuffer(0x8892, 0); glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, 0x1406, 0, 0, quad);
    glDrawArrays(5, 0, 4);
    // restore
    glBindBuffer(0x8892, a_buf); glVertexAttribPointer(0, a_size, a_type, (unsigned char)a_norm, a_stride, a_ptr);
    if (!a_en) glDisableVertexAttribArray(0);
    glBindBuffer(0x8892, o_ab); glBindTexture(0x0DE1, o_tex); glActiveTexture(o_unit); glUseProgram(o_prog);
    glBindFramebuffer(0x8D40, o_fb); glViewport(o_vp[0], o_vp[1], o_vp[2], o_vp[3]);
    glColorMask(o_mask[0], o_mask[1], o_mask[2], o_mask[3]);
    for (int k = 0; k < 6; k++) if (o_caps[k]) glEnable(caps[k]);
}
static bool g_u4 = false;          // Unity 4.x player: MotionEvent objects, own EGL, nativeRender
static void touch(int id, float x, float y, int action, int64_t t_ms) {
    if (g_u4) { P("nativeInjectEvent", "(Landroid/view/InputEvent;)Z", {JV::O(java::motion_event(id, x, y, action, t_ms))}); return; }
    // nativeTouch(pointerId, x, y, action, eventTime, source=TOUCHSCREEN)
    P("nativeTouch", "(IFFIJI)V", {JV::I(id), JV::F(x), JV::F(y), JV::I(action), JV::J(t_ms), JV::I(4098)});
}

static std::string env_or(const char *k, const std::string &d) { const char *v = getenv(k); return v && *v ? v : d; }
static void mkdirs(const std::string &p) { std::string cur; for (size_t i = 0; i < p.size(); i++) { cur += p[i]; if (p[i] == '/' && i) ::mkdir(cur.c_str(), 0755); } ::mkdir(p.c_str(), 0755); }

#ifdef __ANDROID__
static std::string android_apk_path() {
    JNIEnv *e = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    jclass k = e->GetObjectClass(act);
    jmethodID m = e->GetMethodID(k, "getPackageCodePath", "()Ljava/lang/String;");
    jstring s = (jstring)e->CallObjectMethod(act, m);
    const char *c = e->GetStringUTFChars(s, nullptr); std::string r = c; e->ReleaseStringUTFChars(s, c);
    e->DeleteLocalRef(s); e->DeleteLocalRef(k); e->DeleteLocalRef(act);
    return r;
}
#endif

#ifdef __ANDROID__
#include <sched.h>
// the game thread belongs on the fastest cores (big.LITTLE phones otherwise park it on a little core)
static void pin_to_big_cores() {
    long best = 0; long freq[64] = {0}; int n = 0;
    for (int k = 0; k < 64; k++) {
        char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", k);
        FILE *f = fopen(p, "r"); if (!f) { if (k > 16) break; continue; }
        if (fscanf(f, "%ld", &freq[k]) != 1) freq[k] = 0;
        fclose(f); n = k + 1; if (freq[k] > best) best = freq[k];
    }
    if (!best) return;
    cpu_set_t set; CPU_ZERO(&set); int cnt = 0;
    for (int k = 0; k < n; k++) if (freq[k] >= best * 85 / 100) { CPU_SET(k, &set); cnt++; }
    // with a single prime core, also allow the next tier so we are not starved
    if (cnt < 2) for (int k = 0; k < n; k++) if (freq[k] >= best * 60 / 100) CPU_SET(k, &set);
    if (sched_setaffinity(0, sizeof set, &set) == 0) logf("game thread pinned to fast cores (max %ld kHz)", best);
}
// small status line drawn by the Java side (GameActivity.setOverlay)
static void android_overlay(const char *text) {
    JNIEnv *e = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    jclass k = e->GetObjectClass(act);
    jmethodID m = e->GetStaticMethodID(k, "setOverlay", "(Ljava/lang/String;)V");
    if (m) { jstring s = e->NewStringUTF(text); e->CallStaticVoidMethod(k, m, s); e->DeleteLocalRef(s); }
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(k); e->DeleteLocalRef(act);
}
#endif

__attribute__((visibility("default"))) int main(int argc, char **argv) {
    // ---- paths
#ifdef __ANDROID__
    plat::is_android = true;
    plat::data_dir = SDL_AndroidGetInternalStoragePath() ? std::string(SDL_AndroidGetInternalStoragePath()) + "/.." : "/data/data/" RB_PACKAGE;
    plat::apk_path = android_apk_path();
    plat::android_sdk = SDL_GetAndroidSDKVersion();
#else
    std::string home = env_or("HOME", "/tmp");
    plat::data_dir = env_or("RB_DATA", env_or("XDG_DATA_HOME", home + "/.local/share") + "/" RB_DATA_NAME);
    plat::apk_path = argc > 1 ? argv[1] : env_or("RB_APK", "");
    { char *rp = realpath(plat::apk_path.c_str(), nullptr); if (rp) { plat::apk_path = rp; free(rp); } }   // engines build paths from it
    if (plat::apk_path.empty()) { fprintf(stderr, "usage: %s <game.apk>\n", argv[0]); return 2; }
#endif
    plat::files_dir = plat::data_dir + "/files";
    plat::cache_dir = plat::data_dir + "/cache";
    mkdirs(plat::files_dir); mkdirs(plat::cache_dir);
    rt::data_dir = plat::data_dir; rt::apk_path = plat::apk_path; rt::cache_dir = plat::cache_dir;
    verbose = atoi(env_or("RB_VERBOSE", "0").c_str());
#ifdef __ANDROID__
    { char v[92] = {0};      // adb shell setprop debug.rb.verbose 1 / debug.rb.gldebug 1
      if (__system_property_get("debug.rb.verbose", v) > 0) verbose = atoi(v);
      extern int gl_debug; if (__system_property_get("debug.rb.gldebug", v) > 0) gl_debug = atoi(v); }
#endif
    if (const char *l = getenv("LANG")) { std::string s = l; size_t d = s.find('.'); if (d != std::string::npos) s = s.substr(0, d); if (s.size() >= 2 && s != "C" && s != "POSIX") plat::language = s; }
    plat::device_id = plat::Prefs::get_string("__device_id", "");
    if (plat::device_id.empty()) {
        char b[33]; srand((unsigned)time(nullptr) ^ (unsigned)(uintptr_t)&b);
        for (int i = 0; i < 16; i++) snprintf(b + 2 * i, 3, "%02x", rand() & 0xff);
        plat::device_id = b; plat::Prefs::set_string("__device_id", b); plat::Prefs::sync();
    }

    // ---- window + GL
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_SENSOR) != 0) fatal("SDL_Init: %s", SDL_GetError());
#ifdef __ANDROID__
    g_es = true; g_emu = RB_GLES_MODE == 1;           // GLES 2 / auto (Unity 4) games talk to the driver directly
    { char v[92] = {0}; if (__system_property_get("debug.rb.nativegles1", v) > 0 && atoi(v)) g_emu = false; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, g_emu || RB_GLES_MODE != 1 ? 2 : 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, g_emu || RB_GLES_MODE != 1 ? 0 : 1);
    Uint32 wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    if (RB_GLES_MODE != 1 && !getenv("RB_DESKTOPGL")) {   // GLES 2 game: a real ES 2.0 context (EGL), as on phones
        g_es = true;
        SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
        SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    } else if (getenv("RB_GLES2EMU")) {           // dev: GLES 2.0 context + our GLES 1.1 emulation, like on Android
        g_es = true; g_emu = true;
        SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
        SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    } else if (getenv("RB_ES1")) {                // dev: a real GLES 1.1 context on the desktop (EGL)
        g_es = true;
        SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
        SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    } else {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    }
    Uint32 wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (getenv("RB_FULLSCREEN")) wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 6); SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int ww = atoi(env_or("RB_W", "1280").c_str()), wh = atoi(env_or("RB_H", "720").c_str());
    SDL_Window *win = SDL_CreateWindow(RB_LABEL, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, wflags);
    if (!win) fatal("window: %s", SDL_GetError());
    plat::g_win = win;
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) fatal("GL context: %s", SDL_GetError());
    if (SDL_GL_MakeCurrent(win, ctx) != 0) fatal("GL make current: %s", SDL_GetError());
    { int maj = 0, mn = 0, prof = 0; SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &maj); SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &mn);
      SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &prof); logf("GL context %p (version %d.%d profile %d)", ctx, maj, mn, prof); }
    SDL_GL_SetSwapInterval(getenv("RB_NOVSYNC") ? 0 : 1);
    SDL_GL_GetDrawableSize(win, &plat::width, &plat::height);
    apply_aspect();
    { float d = 0; if (SDL_GetDisplayDPI(0, &d, nullptr, nullptr) == 0 && d > 0) plat::dpi = d; }
    if (g_emu && !gles1::init(gl_get_es2)) fatal("GLES 2.0 is required");
    if (g_emu) gles1::set_program_cache(plat::cache_dir + "/gles1_programs.txt");   // compile what earlier runs needed, now
    gl_load_all(gl_get);
#ifdef __ANDROID__
    { extern bool gl_emulate_constant; gl_emulate_constant = false;
      char v[92] = {0}; if (__system_property_get("debug.rb.glconst", v) > 0) gl_emulate_constant = atoi(v) != 0; }
#else
    { extern bool gl_emulate_constant; gl_emulate_constant = getenv("RB_GLCONST") != nullptr; }
#endif
    { typedef const char *(*GS)(unsigned); GS gs = (GS)gl_get("glGetString");
      logf("GL: %s / %s, %dx%d", gs ? gs(0x1F01) : "?", gs ? gs(0x1F02) : "?", plat::width, plat::height); }

#ifdef __ANDROID__
    pin_to_big_cores();
#endif
    // ---- guest
    // Boehm GC (inside libmono): start with a roomy heap so these small games rarely collect. The
    // phones they were made for had ~512 MB; collections every second or two are what made them stutter.
    mem_init();
    { extern void env_set(const std::string &, const std::string &);
      env_set("GC_INITIAL_HEAP_SIZE", env_or("RB_GC_HEAP", "201326592"));   // 192 MB
      env_set("GC_FREE_SPACE_DIVISOR", "2");
      if (getenv("RB_GC_STATS")) env_set("GC_PRINT_STATS", "1"); }
    main_thread_create(16u << 20);
    std::string libdir = env_or("RB_LIBDIR", "");
#ifdef __ANDROID__
    // the original ARM libraries are shipped as assets of our own APK; extract once
    extern std::string android_extract_libs();
    libdir = android_extract_libs();
#endif
    if (libdir.empty()) fatal("RB_LIBDIR (directory with the original libmono.so / libunity.so) is not set");
    Lib *mono = load_library((libdir + "/libmono.so").c_str(), MONO_BASE);
    Lib *unity = load_library((libdir + "/libunity.so").c_str(), UNITY_BASE);
    jni::init();
    java_classes_init();
    run_initializers(mono);
    run_initializers(unity);

    uint32_t onload = lib_sym(unity, "JNI_OnLoad");
    if (onload) logf("JNI_OnLoad -> 0x%x", call(onload, {jni::vm(), 0}));

    { rt::start_thread_dumper(); }
    // ---- UnityPlayer constructor and init(), in the original order
    g_u4 = java::player_has("nativeRecreateGfxState", "(Landroid/view/Surface;)V");
    bool u34 = false;
    if (g_u4) {
        // Unity 4.x: the engine owns EGL (rt_egl: bound to our context) and its expansion file comes as a 2nd nativeFile
        P("nativeFile", "(Ljava/lang/String;)V", {JV::O(jni::jstr(plat::apk_path))});
        P("initJni", "(Landroid/content/Context;)V", {JV::O(java::activity())});
        P("nativeInitWWW", "(Ljava/lang/Class;)V", {JV::O(jni::find_class("com/unity3d/player/WWW")->class_obj)});
        P("nativeSetDefaultDisplay", "(I)V", {JV::I(0)});
        P("nativeRecreateGfxState", "(Landroid/view/Surface;)V", {JV::O(java::surface())});
        P("nativeResize", "(IIII)V", {JV::I(plat::width), JV::I(plat::height), JV::I(plat::width), JV::I(plat::height)});
        P("nativeFocusChanged", "(Z)V", {JV::Z(true)});
        P("nativeResume", "()V");
    } else {
    P("nativeFile", "(Ljava/lang/String;)V", {JV::O(jni::jstr(plat::apk_path))});
    P("initJni", "()V");
    jni::call_native(jni::find_class("com/unity3d/player/PlayerPrefs"), java::prefs_object().get(), "InitPlayerPrefs", "()V", {});
    P("nativeInitWWW", "(Ljava/lang/Class;)V", {JV::O(jni::find_class("com/unity3d/player/WWW")->class_obj)});
    u34 = java::player_has("nativeInit", "(I[BIIIII)V");     // Unity 3.4: splash pixels passed in
    if (u34) {
        // nativeInit(glesMode, RGB565 splash, texW, texH, w, h, splashMode): a black 2x2 splash
        jni::Ref px = jni::new_prim_array('B', 8);
        P("nativeInit", "(I[BIIIII)V", {JV::I(RB_GLES_MODE), JV::O(px), JV::I(2), JV::I(2), JV::I(2), JV::I(2), JV::I(RB_SPLASH_MODE)});
        // the 3.4 Java player creates and starts the FMOD output thread itself
        java::fmod_start34();
    } else
    P("nativeInit", "(II)V", {JV::I(RB_GLES_MODE), JV::I(RB_SPLASH_MODE)});   // from the game's settings.xml
    P("nativeSetExtras", "(Landroid/os/Bundle;)V", {JV()});
    P("nativeRecreateGfxState", "()V");                        // onSurfaceCreated
    P("nativeResize", "(IIII)V", {JV::I(plat::width), JV::I(plat::height), JV::I(plat::width), JV::I(plat::height)});
    if (!u34) P("nativeFocusChanged", "(Z)V", {JV::Z(true)});
    }

    bool started = false;
    int frame = 0;
    bool show_fps = false;     // FPS readout in the corner: adb shell setprop debug.rb.fps 1
#ifdef __ANDROID__
    { char v[92] = {0}; if (__system_property_get("debug.rb.fps", v) > 0) show_fps = atoi(v) != 0; }
#endif
    bool mouse_down = false;
    uint32_t t0 = SDL_GetTicks();
    // accelerometer: real sensor on Android, keyboard tilt on PC
    SDL_Sensor *accel = nullptr;
    for (int i = 0; i < SDL_NumSensors(); i++) if (SDL_SensorGetDeviceType(i) == SDL_SENSOR_ACCEL) { accel = SDL_SensorOpen(i); break; }
    float tilt_x = 0, tilt_y = 0;
    const char *script = getenv("RB_SCRIPT");     // "frame:tap:x:y;frame:shot:file;frame:quit"
    std::string scr = script ? script : "";

    while (!plat::quitting) {
        SDL_Event ev;
        int64_t now = SDL_GetTicks();
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: plat::quitting = true; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    SDL_GL_GetDrawableSize(win, &plat::width, &plat::height);
                    apply_aspect();
                    P("nativeResize", "(IIII)V", {JV::I(plat::width), JV::I(plat::height), JV::I(plat::width), JV::I(plat::height)});
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) { if (!u34) P("nativeFocusChanged", "(Z)V", {JV::Z(true)}); }
                else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { if (!u34) P("nativeFocusChanged", "(Z)V", {JV::Z(false)}); }
                break;
            case SDL_APP_WILLENTERBACKGROUND:
                if (started) { if (u34) P("nativePause", "()V"); else P("nativePause", "()Z"); plat::audio_pause(true); plat::Prefs::sync(); }
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                if (started) { plat::audio_pause(false); P("nativeResume", "()V"); }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT && started) { mouse_down = true; touch(0, (float)ev.button.x * surf_w / std::max(1, ww) - gl_xoff, (float)ev.button.y * plat::height / std::max(1, wh), 0, now); }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT && mouse_down) { mouse_down = false; touch(0, (float)ev.button.x * surf_w / std::max(1, ww) - gl_xoff, (float)ev.button.y * plat::height / std::max(1, wh), 1, now); }
                break;
            case SDL_MOUSEMOTION:
                if (mouse_down) touch(0, (float)ev.motion.x * surf_w / std::max(1, ww) - gl_xoff, (float)ev.motion.y * plat::height / std::max(1, wh), 2, now);
                break;
            case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION: {
                if (!started) break;
                int act = ev.type == SDL_FINGERDOWN ? 0 : ev.type == SDL_FINGERUP ? 1 : 2;
                touch((int)(ev.tfinger.fingerId & 0xff), ev.tfinger.x * surf_w - gl_xoff, ev.tfinger.y * plat::height, act, now);
                break;
            }
            case SDL_KEYDOWN: case SDL_KEYUP: {
                bool down = ev.type == SDL_KEYDOWN;
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_AC_BACK || k == SDLK_ESCAPE) {
                    // KEYCODE_BACK = 4: nativeKeyState(keycode, unicode?, down, meta, repeat)
                    if (started && !ev.key.repeat) {
                        if (u34) P("nativeKeyState", "(IIZ)V", {JV::I(4), JV::I(0), JV::Z(down)});
                        else P("nativeKeyState", "(IIZII)V", {JV::I(4), JV::I(0), JV::Z(down), JV::I(0), JV::I(0)});
                    }
                } else if (k == SDLK_LEFT) tilt_x = down ? -4.0f : 0;
                else if (k == SDLK_RIGHT) tilt_x = down ? 4.0f : 0;
                else if (k == SDLK_UP) tilt_y = down ? -4.0f : 0;
                else if (k == SDLK_DOWN) tilt_y = down ? 4.0f : 0;
                else if (k == SDLK_F11 && down) {
                    Uint32 fl = SDL_GetWindowFlags(win);
                    SDL_SetWindowFullscreen(win, (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;
            }
            }
        }
        SDL_GetWindowSize(win, &ww, &wh);
        if (started) {
            float a[3] = {tilt_x, tilt_y, 9.81f};
            if (accel) {
                SDL_SensorUpdate(); SDL_SensorGetData(accel, a, 3);   // device-natural axes, like Android's SensorEvent (Unity rotates them itself)
            }
            if (!g_u4) P("nativeSensor", "(FFFJ)V", {JV::F(a[0]), JV::F(a[1]), JV::F(a[2]), JV::J(now * 1000000)});
        }
        plat::run_posted();
        if (!scr.empty()) {
            // test hooks
            size_t semi;
            while (!scr.empty()) {
                semi = scr.find(';'); std::string cmd = scr.substr(0, semi);
                int f = atoi(cmd.c_str()); if (f > frame) break;
                scr = semi == std::string::npos ? "" : scr.substr(semi + 1);
                std::vector<std::string> p; std::stringstream ss(cmd); std::string tok; while (std::getline(ss, tok, ':')) p.push_back(tok);
                if (p.size() >= 4 && p[1] == "down") touch(0, (float)atof(p[2].c_str()), (float)atof(p[3].c_str()), 0, now);
                if (p.size() >= 4 && p[1] == "up") touch(0, (float)atof(p[2].c_str()), (float)atof(p[3].c_str()), 1, now);
                if (p.size() >= 4 && p[1] == "tap") { touch(0, (float)atof(p[2].c_str()), (float)atof(p[3].c_str()), 0, now); touch(0, (float)atof(p[2].c_str()), (float)atof(p[3].c_str()), 1, now + 50); }
                else if (p.size() >= 2 && p[1] == "quit") plat::quitting = true;
                else if (p.size() >= 3 && p[1] == "shot") {
                    const int W = surf_w, H = surf_h;               // the whole window, side bars included
                    std::vector<uint8_t> px(W * H * 4);
                    typedef void (*RP)(int, int, int, int, unsigned, unsigned, void *);
                    ((RP)SDL_GL_GetProcAddress("glReadPixels"))(0, 0, W, H, 0x1908, 0x1401, px.data());
                    FILE *o = fopen(p[2].c_str(), "wb");
                    if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H);
                        for (int y = H - 1; y >= 0; y--) for (int x = 0; x < W; x++) fwrite(&px[(y * W + x) * 4], 1, 3, o);
                        fclose(o); logf("screenshot %s", p[2].c_str()); }
                }
            }
        }
        // ---- UnityPlayer.onDrawFrame
        { extern int gl_trace_on; static int trace_frame = getenv("RB_GLTRACE") ? atoi(getenv("RB_GLTRACE")) : -1;
#ifdef __ANDROID__
          static bool once = [] { char v[92] = {0}; if (__system_property_get("debug.rb.gltrace", v) > 0) trace_frame = atoi(v); return true; }(); (void)once;
#endif
          gl_trace_on = frame == trace_frame || (trace_frame < -1 && frame < -trace_frame); }
        bool ok = P("nativeRender", "()Z").z();
        if (!ok) { logf("nativeRender returned false: quitting"); break; }
        if (!started && g_u4) {
            started = true;
            jni::JClass *fk = jni::find_class("org/fmod/FMODAudioDevice");      // UnityPlayer.resume(): FMOD output
            jni::Ref dev = jni::new_obj(fk); jni::Args none;
            fk->methods["<init>()V"]->impl(dev.get(), none);
            fk->methods["start()V"]->impl(dev.get(), none);
            logf("game loop started after %u ms", SDL_GetTicks() - t0);
        }
        if (!started) {
            P("unityAndroidInit", "(Ljava/lang/String;Ljava/lang/String;)V", {JV::O(jni::jstr("assets/bin/")), JV::O(jni::jstr(plat::data_dir + "/lib"))});
            if (!u34) P("unityAndroidPrepareGameLoop", "()V");
            started = true;
            P("nativeResize", "(IIII)V", {JV::I(plat::width), JV::I(plat::height), JV::I(plat::width), JV::I(plat::height)});
            P("nativeResume", "()V");
            if (!u34) P("nativeFocusChanged", "(Z)V", {JV::Z(true)});
            // UnityPlayer.resume(): the FMOD output device
            jni::JClass *fk = jni::find_class("org/fmod/FMODAudioDevice");
            jni::Ref dev = jni::new_obj(fk);
            jni::Args none;
            fk->methods["<init>()V"]->impl(dev.get(), none);
            fk->methods["start()V"]->impl(dev.get(), none);
            logf("game loop started after %u ms", SDL_GetTicks() - t0);
        }
        upscale_backbuffer(plat::width, plat::height);
        gl_draw_bars(surf_w, surf_h);
        SDL_GL_SwapWindow(win);
        frame++;
        {
            static uint32_t fps_t = SDL_GetTicks(); static int fps_n = 0; fps_n++;
            uint32_t tnow = SDL_GetTicks();
            if (tnow - fps_t >= 1000) {
                double fps = fps_n * 1000.0 / (tnow - fps_t);
                if (getenv("RB_FPS")) logf("fps %.1f (interp steps %llu)", fps, (unsigned long long)interp::steps());
#ifdef __ANDROID__
                if (show_fps) { char b[64]; snprintf(b, sizeof b, "%.0f FPS", fps); android_overlay(b); }
#endif
                fps_t = tnow; fps_n = 0;
            }
        }
        if (frame % 600 == 0) plat::Prefs::sync();
    }
    plat::Prefs::sync();
    interp::prof_dump();
    { extern void hprof_dump(); hprof_dump(); }
    { extern void dump_static_entries(); dump_static_entries(); }
    if (started) P("nativeDone", "()V");
    plat::audio_stop();
    SDL_Quit();
    fflush(nullptr);
    std::_Exit(0);
}
