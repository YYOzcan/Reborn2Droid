// libc / NDK pieces that later engine builds (Unity 4.x) import: wide-char locale functions, Android
// NativeActivity glue (unused: we drive the Java-style UnityPlayer path), properties, libgcc unwinder hooks.
#include "rt.h"
#include "platform.h"
#include <cstring>
#include <cmath>
#include <cwctype>
using namespace rt;
#define A(i) arg(c, i)
#define RET(v) (c->r[0] = (uint32_t)(v))
static const uint32_t WEOF_ = 0xffffffffu;

// ---- wide chars (bionic: 32-bit wchar_t, "C"/UTF-8 single-byte use)
IMP(btowc) { int32_t v = (int32_t)A(0); RET(v >= 0 && v < 0x80 ? (uint32_t)v : WEOF_); }
IMP(wctob) { uint32_t v = A(0); RET(v < 0x80 ? v : 0xffffffffu); }
IMP(mbrtowc) {          // (wchar_t *pwc, const char *s, size_t n, mbstate_t *)
    uint32_t pwc = A(0), s = A(1), n = A(2);
    if (!s) { RET(0); return; }
    if (!n) { RET((uint32_t)-2); return; }
    const uint8_t *p = (const uint8_t *)GPTR(s);
    uint32_t cp, len;
    if (p[0] < 0x80) { cp = p[0]; len = 1; }
    else if ((p[0] & 0xe0) == 0xc0) { cp = p[0] & 0x1f; len = 2; }
    else if ((p[0] & 0xf0) == 0xe0) { cp = p[0] & 0x0f; len = 3; }
    else if ((p[0] & 0xf8) == 0xf0) { cp = p[0] & 0x07; len = 4; }
    else { RET((uint32_t)-1); return; }
    if (len > n) { RET((uint32_t)-2); return; }
    for (uint32_t k = 1; k < len; k++) { if ((p[k] & 0xc0) != 0x80) { RET((uint32_t)-1); return; } cp = (cp << 6) | (p[k] & 0x3f); }
    if (pwc) W32(pwc, cp);
    RET(cp ? len : 0);
}
IMP(wcrtomb) {          // (char *s, wchar_t wc, mbstate_t *)
    uint32_t s = A(0), wc = A(1);
    if (!s) { RET(1); return; }
    uint8_t *p = (uint8_t *)GPTR(s);
    if (wc < 0x80) { p[0] = wc; RET(1); }
    else if (wc < 0x800) { p[0] = 0xc0 | (wc >> 6); p[1] = 0x80 | (wc & 0x3f); RET(2); }
    else if (wc < 0x10000) { p[0] = 0xe0 | (wc >> 12); p[1] = 0x80 | ((wc >> 6) & 0x3f); p[2] = 0x80 | (wc & 0x3f); RET(3); }
    else if (wc < 0x110000) { p[0] = 0xf0 | (wc >> 18); p[1] = 0x80 | ((wc >> 12) & 0x3f); p[2] = 0x80 | ((wc >> 6) & 0x3f); p[3] = 0x80 | (wc & 0x3f); RET(4); }
    else RET((uint32_t)-1);
}
// wctype ids are our own small integers
static const char *wct_names[] = {"", "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"};
IMP(wctype) {
    const char *n = GSTR(A(0));
    for (uint32_t k = 1; k < sizeof wct_names / sizeof *wct_names; k++) if (!strcmp(n, wct_names[k])) { RET(k); return; }
    RET(0);
}
IMP(iswctype) {
    uint32_t v = A(0), t = A(1);
    if (v >= 0x10000) { RET(0); return; }
    wint_t w = (wint_t)v; int r = 0;
    switch (t) {
    case 1: r = iswalnum(w); break; case 2: r = iswalpha(w); break; case 3: r = iswblank(w); break; case 4: r = iswcntrl(w); break;
    case 5: r = iswdigit(w); break; case 6: r = iswgraph(w); break; case 7: r = iswlower(w); break; case 8: r = iswprint(w); break;
    case 9: r = iswpunct(w); break; case 10: r = iswspace(w); break; case 11: r = iswupper(w); break; case 12: r = iswxdigit(w); break;
    }
    RET(r ? 1 : 0);
}
IMP(getwc) { RET(WEOF_); }
IMP(putwc) { RET(A(0)); }
IMP(ungetwc) { RET(WEOF_); }
IMP(wcscoll) {
    uint32_t a = A(0), b = A(1);
    for (;; a += 4, b += 4) { int32_t x = (int32_t)R32(a), y = (int32_t)R32(b); if (x != y) { RET(x < y ? -1 : 1); return; } if (!x) break; }
    RET(0);
}
IMP(wcsxfrm) {          // (dst, src, n): identity transform
    uint32_t d = A(0), s = A(1), n = A(2), len = 0;
    while (R32(s + 4 * len)) len++;
    if (d && n) { uint32_t k = 0; for (; k < len && k + 1 < n; k++) W32(d + 4 * k, R32(s + 4 * k)); W32(d + 4 * k, 0); }
    RET(len);
}
IMP(wcsftime) { if (A(0) && A(1)) W32(A(0), 0); RET(0); }
IMP(wmemchr) { uint32_t s = A(0), ch = A(1), n = A(2); for (uint32_t k = 0; k < n; k++) if (R32(s + 4 * k) == ch) { RET(s + 4 * k); return; } RET(0); }
IMP(wmemcmp) {
    uint32_t a = A(0), b = A(1), n = A(2);
    for (uint32_t k = 0; k < n; k++) { int32_t x = (int32_t)R32(a + 4 * k), y = (int32_t)R32(b + 4 * k); if (x != y) { RET(x < y ? -1 : 1); return; } }
    RET(0);
}
IMP(strcoll) { RET(strcmp(GSTR(A(0)), GSTR(A(1)))); }
IMP(strxfrm) {
    const char *s = GSTR(A(1)); uint32_t len = strlen(s), n = A(2);
    if (A(0) && n) { char *d = (char *)GPTR(A(0)); size_t k = std::min<size_t>(len, n - 1); memcpy(d, s, k); d[k] = 0; }
    RET(len);
}
IMP(strsep) {           // (char **stringp, const char *delim)
    uint32_t pp = A(0), s = R32(pp);
    if (!s) { RET(0); return; }
    char *str = (char *)GPTR(s); const char *delim = GSTR(A(1));
    char *e = strpbrk(str, delim);
    if (e) { *e = 0; W32(pp, s + (uint32_t)(e - str) + 1); } else W32(pp, 0);
    RET(s);
}
IMP(modff) { float ip; float r = ::modff(argf(c, 0), &ip); if (A(1)) W32(A(1), fbits(ip)); retf(c, r); }

// ---- system
IMP(__system_property_get) { if (A(1)) *(char *)GPTR(A(1)) = 0; RET(0); }
IMP(ptrace) { RET((uint32_t)-1); }
IMP(tgkill) { RET(0); }
IMP(__div0) { RET(0); }
IMP(__gnu_uldivmod_helper) {   // (u64 a, u64 b, u64 *rem)
    uint64_t a = ((uint64_t)c->r[1] << 32) | c->r[0], b = ((uint64_t)c->r[3] << 32) | c->r[2];
    uint32_t rp = R32(c->r[13]);
    uint64_t q = b ? a / b : 0, r = b ? a % b : 0;
    if (rp) { W32(rp, (uint32_t)r); W32(rp + 4, (uint32_t)(r >> 32)); }
    c->r[0] = (uint32_t)q; c->r[1] = (uint32_t)(q >> 32);
}

// ---- C++ exceptions (libgcc unwinder in Unity 4's libmain.so): not expected on the normal path
#define NOEXC(n) IMP(n) { fatal("C++ exception unwinding (%s) is not supported (called from %s)", #n, sym_name(c->r[14])); }
NOEXC(_Unwind_RaiseException) NOEXC(_Unwind_Resume) NOEXC(_Unwind_Resume_or_Rethrow) NOEXC(_Unwind_Backtrace)
NOEXC(__aeabi_unwind_cpp_pr0) NOEXC(__aeabi_unwind_cpp_pr1) NOEXC(__gnu_unwind_frame)
NOEXC(_Unwind_VRS_Get) NOEXC(_Unwind_VRS_Set) NOEXC(_Unwind_GetLanguageSpecificData) NOEXC(_Unwind_GetRegionStart)
NOEXC(_Unwind_GetDataRelBase) NOEXC(_Unwind_GetTextRelBase)
IMP(_Unwind_Complete) {}
IMP(_Unwind_DeleteException) {}

// ---- NativeActivity / NDK input, looper, sensors, window: Unity's native-activity path, never taken here
#define ZERO(n) IMP(n) { RET(0); }
ZERO(AInputEvent_getDeviceId) ZERO(AInputEvent_getSource) ZERO(AInputEvent_getType) ZERO(AInputQueue_attachLooper)
ZERO(AInputQueue_finishEvent) ZERO(AInputQueue_preDispatchEvent) ZERO(AKeyEvent_getAction) ZERO(AKeyEvent_getKeyCode)
ZERO(AKeyEvent_getMetaState) ZERO(ALooper_forThread) ZERO(ALooper_prepare) ZERO(AMotionEvent_getAction)
ZERO(AMotionEvent_getHistorySize) ZERO(AMotionEvent_getPointerCount) ZERO(AMotionEvent_getPointerId)
ZERO(ANativeWindow_setBuffersGeometry)
// the engine only checks the window handle and hands it back to (our) EGL
IMP(ANativeWindow_fromSurface) { static uint32_t w; if (!w) w = gstatic(64); RET(w); }
IMP(ANativeWindow_release) {}
IMP(ANativeWindow_acquire) {}
IMP(ANativeWindow_getWidth) { RET(plat::width); }
IMP(ANativeWindow_getHeight) { RET(plat::height); }
IMP(ANativeWindow_getFormat) { RET(4); }
ZERO(ASensorEventQueue_disableSensor) ZERO(ASensorEventQueue_enableSensor) ZERO(ASensorEventQueue_getEvents)
ZERO(ASensorEventQueue_hasEvents) ZERO(ASensorEventQueue_setEventRate) ZERO(ASensor_getMinDelay) ZERO(ASensor_getName)
ZERO(ASensor_getType) ZERO(ASensor_getVendor) ZERO(ASensorManager_createEventQueue) ZERO(ASensorManager_destroyEventQueue)
ZERO(ASensorManager_getDefaultSensor) ZERO(ASensorManager_getInstance) ZERO(ASensorManager_getSensorList)
ZERO(_Z28ANativeActivity_feedWatchdogd) ZERO(_Z29ANativeActivity_getInputQueuev) ZERO(_Z30ANativeActivity_enableWatchdogb)
ZERO(_Z42ANativeActivity_forwardMotionEventToDalvikP11AInputEventP7_JNIEnv) ZERO(_Z42ANativeActivity_setFirstLevelHasBeenLoadedb)
IMP(AInputQueue_getEvent) { RET((uint32_t)-1); }
IMP(ALooper_pollAll) { RET((uint32_t)-1); }          // ALOOPER_POLL_WAKE: nothing
IMP(AMotionEvent_getEventTime) { c->r[0] = c->r[1] = 0; }
IMP(AMotionEvent_getHistoricalEventTime) { c->r[0] = c->r[1] = 0; }
IMP(AMotionEvent_getX) { retf(c, 0); }
IMP(AMotionEvent_getY) { retf(c, 0); }
IMP(AMotionEvent_getHistoricalX) { retf(c, 0); }
IMP(AMotionEvent_getHistoricalY) { retf(c, 0); }
IMP(ASensor_getResolution) { retf(c, 0); }

// ---- EGL (Unity 4.x creates its own display/config/surface/context): every object maps onto the GL context
// the host already made current; buffers are swapped by the host after nativeRender().
static const uint32_t EGL_DPY = 1, EGL_CFG = 1, EGL_SURF = 2, EGL_CTX = 3;
IMP(eglGetDisplay) { RET(EGL_DPY); }
IMP(eglInitialize) { if (A(1)) W32(A(1), 1); if (A(2)) W32(A(2), 4); RET(1); }
IMP(eglTerminate) { RET(1); }
IMP(eglGetError) { RET(0x3000); }                       // EGL_SUCCESS
IMP(eglChooseConfig) {                                  // (dpy, attribs, configs, size, num)
    if (A(2) && A(3)) W32(A(2), EGL_CFG);
    if (R32(c->r[13])) W32(R32(c->r[13]), 1);
    RET(1);
}
IMP(eglGetConfigs) { if (A(1) && A(2)) W32(A(1), EGL_CFG); if (A(3)) W32(A(3), 1); RET(1); }
IMP(eglGetConfigAttrib) {                               // (dpy, cfg, attr, value*): our 565 + depth 16 window
    uint32_t v = 0;
    switch (A(2)) {
    case 0x3020: v = 16; break;  case 0x3021: v = 0; break;  case 0x3022: v = 5; break;  case 0x3023: v = 6; break;
    case 0x3024: v = 5; break;   case 0x3025: v = 16; break; case 0x3026: v = 0; break;  case 0x3027: v = 0x3038; break;
    case 0x3028: v = 1; break;   case 0x302E: v = 4; break;  case 0x3033: v = 4 | 1; break; case 0x3040: v = 4 | 1; break;
    case 0x3042: v = 4 | 1; break; case 0x3031: case 0x3032: v = 0; break;
    }
    if (A(3)) W32(A(3), v);
    RET(1);
}
IMP(eglCreateWindowSurface) { RET(EGL_SURF); }
IMP(eglCreatePbufferSurface) { RET(EGL_SURF + 8); }
IMP(eglDestroySurface) { RET(1); }
IMP(eglCreateContext) { RET(EGL_CTX); }
IMP(eglDestroyContext) { RET(1); }
IMP(eglMakeCurrent) { RET(1); }
IMP(eglSwapBuffers) { RET(1); }
IMP(eglSwapInterval) { RET(1); }
IMP(eglQuerySurface) {                                  // (dpy, surf, attr, value*)
    uint32_t v = A(2) == 0x3057 ? (uint32_t)plat::width : A(2) == 0x3056 ? (uint32_t)plat::height : 0;
    if (A(3)) W32(A(3), v);
    RET(1);
}
IMP(eglQueryString) { static uint32_t s; if (!s) { s = gstatic(64); strcpy((char *)GPTR(s), "1.4 Reborn2Droid"); } RET(s); }
IMP(eglGetCurrentContext) { RET(EGL_CTX); }
IMP(eglGetCurrentDisplay) { RET(EGL_DPY); }
IMP(eglGetCurrentSurface) { RET(EGL_SURF); }
IMP(eglBindAPI) { RET(1); }
IMP(eglWaitGL) { RET(1); }
IMP(eglWaitNative) { RET(1); }
IMP(eglReleaseThread) { RET(1); }
IMP(eglGetProcAddress) { RET(import_trap(GSTR(A(0)))); }
