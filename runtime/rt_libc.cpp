// libc: strings, stdlib, printf/scanf, ctype, math, time (bionic 32-bit ARM ABI on a 64-bit host).
#include "rt.h"
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <ctime>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <map>
#include <zlib.h>
#include "host.h"

using namespace rt;
#define A(i) arg(c, i)

enum { G_EPERM = 1, G_ENOENT = 2, G_EINTR = 4, G_EIO = 5, G_EBADF = 9, G_EAGAIN = 11, G_ENOMEM = 12, G_EACCES = 13,
       G_EEXIST = 17, G_ENOTDIR = 20, G_EISDIR = 21, G_EINVAL = 22, G_ENOSPC = 28, G_ESPIPE = 29, G_ENOTSOCK = 88,
       G_EAFNOSUPPORT = 97, G_ECONNREFUSED = 111, G_EINPROGRESS = 115 };
void rt::set_errno_from_host() { gerr(errno); }      // Linux and Android hosts share the guest's errno numbers
#define RET(v) (c->r[0] = (uint32_t)(v))

// ---------------------------------------------------------------- printf machinery
uint32_t VA::next32() {
    if (mem) { uint32_t v = R32(ap); ap += 4; return v; }
    return arg(c, idx++);
}
uint64_t VA::next64() {
    if (mem) { ap = (ap + 7) & ~7u; uint64_t v = (uint64_t)R32(ap) | ((uint64_t)R32(ap + 4) << 32); ap += 8; return v; }
    return arg64(c, idx);
}

std::string rt::gformat(const char *fmt, VA &va) {
    std::string out; char spec[64], buf[512];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { out += *p; continue; }
        const char *s = p++;
        if (*p == '%') { out += '%'; continue; }
        std::string flags;
        while (*p && strchr("-+ #0'", *p)) flags += *p++;
        std::string width, prec;
        if (*p == '*') { width = std::to_string((int32_t)va.next32()); p++; } else while (isdigit((unsigned char)*p)) width += *p++;
        if (*p == '.') { p++; prec = "."; if (*p == '*') { prec += std::to_string((int32_t)va.next32()); p++; } else while (isdigit((unsigned char)*p)) prec += *p++; }
        int lng = 0;   // 0 int, 1 long, 2 long long, -1 short, -2 char, 3 size/ptrdiff
        while (*p && strchr("hlLqjzt", *p)) {
            if (*p == 'h') lng = lng == -1 ? -2 : -1;
            else if (*p == 'l') lng = lng == 1 ? 2 : 1;
            else if (*p == 'q' || *p == 'L' || *p == 'j') lng = 2;
            else lng = 3;
            p++;
        }
        char conv = *p;
        if (!conv) break;
        std::string base = "%" + flags + width + prec;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            if (lng == 2) {
                uint64_t v = va.next64();
                snprintf(spec, sizeof spec, "%sll%c", base.c_str(), conv);
                snprintf(buf, sizeof buf, spec, v);
            } else {
                uint32_t v = va.next32();
                snprintf(spec, sizeof spec, "%s%c", base.c_str(), conv);
                if (conv == 'd' || conv == 'i') snprintf(buf, sizeof buf, spec, (int)(int32_t)(lng == -1 ? (int16_t)v : lng == -2 ? (int8_t)v : (int32_t)v));
                else snprintf(buf, sizeof buf, spec, (unsigned)(lng == -1 ? (uint16_t)v : lng == -2 ? (uint8_t)v : v));
            }
            out += buf; break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t u = va.next64(); double d; memcpy(&d, &u, 8);
            snprintf(spec, sizeof spec, "%s%c", base.c_str(), conv);
            snprintf(buf, sizeof buf, spec, d); out += buf; break;
        }
        case 's': {
            uint32_t sp = va.next32();
            const char *str = sp ? GSTR(sp) : "(null)";
            snprintf(spec, sizeof spec, "%ss", base.c_str());
            int n = snprintf(nullptr, 0, spec, str);
            std::string tmp(n + 1, 0); snprintf(&tmp[0], n + 1, spec, str); tmp.resize(n); out += tmp; break;
        }
        case 'p': snprintf(buf, sizeof buf, "0x%x", va.next32()); out += buf; break;
        case 'n': { uint32_t ptr = va.next32(); if (ptr) W32(ptr, (uint32_t)out.size()); break; }
        default: out.append(s, p + 1); break;
        }
    }
    return out;
}

static uint32_t put_string(uint32_t buf, uint32_t size, const std::string &s) {
    if (size) { size_t n = std::min<size_t>(s.size(), size - 1); memcpy(GPTR(buf), s.data(), n); W8(buf + n, 0); }
    return (uint32_t)s.size();
}
IMP(sprintf) { VA va{c, 2, 0, false}; std::string s = gformat(GSTR(A(1)), va); memcpy(GPTR(A(0)), s.c_str(), s.size() + 1); RET(s.size()); }
IMP(snprintf) { VA va{c, 3, 0, false}; RET(put_string(A(0), A(1), gformat(GSTR(A(2)), va))); }
IMP(vsnprintf) { VA va{c, 0, A(3), true}; RET(put_string(A(0), A(1), gformat(GSTR(A(2)), va))); }
IMP(sscanf) {
    const char *str = GSTR(A(0)), *fmt = GSTR(A(1)); VA va{c, 2, 0, false};
    int pos = 0, count = 0;
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            if (isspace((unsigned char)*p)) { while (isspace((unsigned char)str[pos])) pos++; p++; continue; }
            if (str[pos] != *p) break; pos++; p++; continue;
        }
        const char *s = p++;
        if (*p == '%') { if (str[pos] != '%') break; pos++; p++; continue; }
        bool suppress = false; if (*p == '*') { suppress = true; p++; }
        while (isdigit((unsigned char)*p)) p++;
        std::string len; while (*p && strchr("hlLqjzt", *p)) len += *p++;
        char conv = *p;
        if (conv == '[') { while (*p && *p != ']') p++; }
        p++;
        std::string piece(s, p); piece += "%n";
        int used = 0, got;
        if (suppress) { std::string pc = piece; got = sscanf(str + pos, pc.c_str(), &used); if (used == 0 && got < 0) break; pos += used; continue; }
        uint32_t dst = va.next32();
        if (strchr("dioux", conv) || conv == 'X') {
            if (len == "ll" || len == "q" || len == "j") { long long v = 0; got = sscanf(str + pos, piece.c_str(), &v, &used); if (got >= 1) { W32(dst, (uint32_t)v); W32(dst + 4, (uint32_t)((uint64_t)v >> 32)); } }
            else if (len == "h") { short v = 0; got = sscanf(str + pos, piece.c_str(), &v, &used); if (got >= 1) W16(dst, (uint16_t)v); }
            else if (len == "hh") { char v = 0; got = sscanf(str + pos, piece.c_str(), &v, &used); if (got >= 1) W8(dst, (uint8_t)v); }
            else { std::string pc = piece; size_t lp = pc.find('l'); if (len == "l" && lp != std::string::npos) pc.erase(lp, 1);
                   int v = 0; got = sscanf(str + pos, pc.c_str(), &v, &used); if (got >= 1) W32(dst, (uint32_t)v); }
        } else if (strchr("feEgGaA", conv)) {
            if (len == "l" || len == "L") { double v = 0; got = sscanf(str + pos, piece.c_str(), &v, &used); if (got >= 1) WV64(dst, v); }
            else { float v = 0; got = sscanf(str + pos, piece.c_str(), &v, &used); if (got >= 1) WVF(dst, v); }
        } else if (conv == 's' || conv == '[' || conv == 'c') {
            got = sscanf(str + pos, piece.c_str(), (char *)GPTR(dst), &used);
        } else if (conv == 'n') { W32(dst, pos); continue; }
        else break;
        if (got < 1) break;
        count++; pos += used;
    }
    RET(count == 0 && !*str ? -1 : count);
}

// ---------------------------------------------------------------- strings / ctype / stdlib
IMP(strlen) { RET(strlen(GSTR(A(0)))); }
IMP(strcmp) { RET(strcmp(GSTR(A(0)), GSTR(A(1)))); }
IMP(strncmp) { RET(strncmp(GSTR(A(0)), GSTR(A(1)), A(2))); }
static int lc(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }   // ASCII only (no Turkish-locale surprises)
static int ncase(const char *a, const char *b, size_t n) {
    for (size_t k = 0; k < n; k++) { int x = lc((unsigned char)a[k]), y = lc((unsigned char)b[k]); if (x != y) return x - y; if (!x) return 0; }
    return 0;
}
IMP(strcasecmp) { RET(ncase(GSTR(A(0)), GSTR(A(1)), (size_t)-1)); }
IMP(strncasecmp) { RET(ncase(GSTR(A(0)), GSTR(A(1)), A(2))); }
IMP(strcpy) { strcpy((char *)GPTR(A(0)), GSTR(A(1))); RET(A(0)); }
IMP(strncpy) { strncpy((char *)GPTR(A(0)), GSTR(A(1)), A(2)); RET(A(0)); }
IMP(strlcat) { const char *s = GSTR(A(1)); char *d = (char *)GPTR(A(0)); size_t sz = A(2), dl = strnlen(d, sz), sl = strlen(s);
               if (dl < sz) { size_t n = std::min(sl, sz - dl - 1); memcpy(d + dl, s, n); d[dl + n] = 0; } RET(dl + sl); }
IMP(strchr) { const char *r = strchr(GSTR(A(0)), (int)A(1)); RET(r ? G32(r) : 0); }
IMP(strrchr) { const char *r = strrchr(GSTR(A(0)), (int)A(1)); RET(r ? G32(r) : 0); }
IMP(strstr) { const char *r = strstr(GSTR(A(0)), GSTR(A(1))); RET(r ? G32(r) : 0); }
IMP(memchr) { const void *r = memchr(GPTR(A(0)), (int)A(1), A(2)); RET(r ? G32(r) : 0); }
IMP(memcmp) { RET(memcmp(GPTR(A(0)), GPTR(A(1)), A(2))); }
IMP(memcpy) { memcpy(GPTR(A(0)), GPTR(A(1)), A(2)); RET(A(0)); }
IMP(memmove) { memmove(GPTR(A(0)), GPTR(A(1)), A(2)); RET(A(0)); }
IMP(memset) { memset(GPTR(A(0)), (int)A(1), A(2)); RET(A(0)); }
IMP(bcopy) { memmove(GPTR(A(1)), GPTR(A(0)), A(2)); }
IMP(strdup) { RET(gstrdup(GSTR(A(0)))); }
IMP(strtok_r) {
    uint32_t s = A(0) ? A(0) : R32(A(2)); const char *delim = GSTR(A(1));
    if (!s) { RET(0); return; }
    while (R8(s) && strchr(delim, (int)R8(s))) s++;
    if (!R8(s)) { W32(A(2), s); RET(0); return; }
    uint32_t tok = s;
    while (R8(s) && !strchr(delim, (int)R8(s))) s++;
    if (R8(s)) { W8(s, 0); s++; }
    W32(A(2), s); RET(tok);
}
static void set_end(uint32_t endp, const char *base, char *e) { if (endp) W32(endp, G32(e ? e : base)); }
IMP(strtol) { char *e; long v = strtol(GSTR(A(0)), &e, A(2)); set_end(A(1), GSTR(A(0)), e);
              if (v > INT32_MAX) v = INT32_MAX; if (v < INT32_MIN) v = INT32_MIN; RET((int32_t)v); }
IMP(strtoul) { char *e; unsigned long v = strtoul(GSTR(A(0)), &e, A(2)); set_end(A(1), GSTR(A(0)), e); RET(v > UINT32_MAX ? UINT32_MAX : v); }
IMP(strtoll) { char *e; long long v = strtoll(GSTR(A(0)), &e, A(2)); set_end(A(1), GSTR(A(0)), e); ret64(c, (uint64_t)v); }
IMP(strtoull) { char *e; unsigned long long v = strtoull(GSTR(A(0)), &e, A(2)); set_end(A(1), GSTR(A(0)), e); ret64(c, v); }
IMP(strtod) { char *e; double v = strtod(GSTR(A(0)), &e); set_end(A(1), GSTR(A(0)), e); retd(c, v); }
IMP(atoi) { RET(atoi(GSTR(A(0)))); }
IMP(atoll) { ret64(c, (uint64_t)atoll(GSTR(A(0)))); }
static const char *gstrerror(int e) {
    switch (e) { case 0: return "Success"; case G_ENOENT: return "No such file or directory"; case G_EACCES: return "Permission denied";
    case G_EEXIST: return "File exists"; case G_EAGAIN: return "Try again"; case G_ECONNREFUSED: return "Connection refused";
    case G_EINPROGRESS: return "Operation now in progress"; case G_EINVAL: return "Invalid argument"; default: return "I/O error"; }
}
IMP(strerror) { static thread_local uint32_t buf; if (!buf) buf = gstatic(128); snprintf((char *)GPTR(buf), 128, "%s", gstrerror((int)A(0))); RET(buf); }
IMP(strerror_r) { snprintf((char *)GPTR(A(1)), A(2), "%s", gstrerror((int)A(0))); RET(0); }
IMP(basename) { const char *s = GSTR(A(0)); const char *r = strrchr(s, '/'); RET(G32(r ? r + 1 : s)); }
// ctype: "C" locale semantics on every host (bionic's ctype is ASCII-only)
static bool asc(uint32_t v) { return v < 128; }
IMP(tolower) { uint32_t v = A(0); RET(v >= 'A' && v <= 'Z' ? v + 32 : v); }
IMP(toupper) { uint32_t v = A(0); RET(v >= 'a' && v <= 'z' ? v - 32 : v); }
#define CT(f) IMP(f) { uint32_t v = A(0); RET(asc(v) && f((int)v) ? 1 : 0); }
CT(isalnum) CT(isalpha) CT(iscntrl) CT(islower) CT(isprint) CT(ispunct) CT(isspace) CT(isupper) CT(isxdigit)
// wide chars: bionic wchar_t is 32-bit (Windows' is 16-bit), so never hand guest strings to host wcs* functions
static bool bmp(uint32_t v) { return v < 0x10000; }
IMP(towlower) { uint32_t v = A(0); RET(bmp(v) ? (uint32_t)towlower((wint_t)v) : v); }
IMP(towupper) { uint32_t v = A(0); RET(bmp(v) ? (uint32_t)towupper((wint_t)v) : v); }
#define WCT(f) IMP(f) { uint32_t v = A(0); RET(bmp(v) && f((wint_t)v) ? 1 : 0); }
WCT(iswalpha) WCT(iswcntrl) WCT(iswdigit) WCT(iswlower) WCT(iswprint) WCT(iswpunct) WCT(iswspace) WCT(iswupper) WCT(iswxdigit)
IMP(wcslen) { uint32_t p = A(0), n = 0; while (R32(p + 4 * n)) n++; RET(n); }
IMP(wcscmp) {
    uint32_t a = A(0), b = A(1);
    for (;; a += 4, b += 4) { int32_t x = (int32_t)R32(a), y = (int32_t)R32(b); if (x != y) { RET(x < y ? -1 : 1); return; } if (!x) break; }
    RET(0);
}
IMP(wcsncpy) {
    uint32_t d = A(0), s = A(1), n = A(2), k = 0;
    for (; k < n && R32(s + 4 * k); k++) W32(d + 4 * k, R32(s + 4 * k));
    for (; k < n; k++) W32(d + 4 * k, 0);
    RET(A(0));
}
IMP(wmemcpy) { memcpy(GPTR(A(0)), GPTR(A(1)), (size_t)A(2) * 4); RET(A(0)); }
IMP(wmemmove) { memmove(GPTR(A(0)), GPTR(A(1)), (size_t)A(2) * 4); RET(A(0)); }
IMP(wmemset) { for (uint32_t k = 0; k < A(2); k++) W32(A(0) + 4 * k, A(1)); RET(A(0)); }

IMP(malloc) { RET(gmalloc(A(0))); }
IMP(calloc) { RET(gcalloc(A(0), A(1))); }
IMP(realloc) { RET(grealloc(A(0), A(1))); }
IMP(free) { gfree(A(0)); }
IMP(abort) { fatal("guest abort() from %s", sym_name(c->r[14])); }
IMP(exit) { logf("guest exit(%d)", (int)A(0)); fflush(nullptr); std::_Exit((int)A(0)); }
IMP(__assert2) { fatal("assert %s:%d %s: %s", GSTR(A(0)), (int)A(1), GSTR(A(2)), GSTR(A(3))); }
IMP(__stack_chk_fail) { fatal("stack smashing detected in %s", sym_name(c->r[14])); }
IMP(__cxa_atexit) { RET(0); }
IMP(__aeabi_atexit) { RET(0); }
IMP(__cxa_finalize) {}
IMP(__gnu_Unwind_Find_exidx) { RET(0); }
static std::atomic<uint64_t> rand48_state{0x1234ABCD330Eull};
IMP(lrand48) {
    uint64_t x = rand48_state.load(), n;
    do n = (x * 0x5DEECE66Dull + 0xB) & 0xFFFFFFFFFFFFull; while (!rand48_state.compare_exchange_weak(x, n));
    RET((uint32_t)(n >> 17));
}
IMP(srand48) { rand48_state = ((uint64_t)A(0) << 16) | 0x330E; }
IMP(alarm) { RET(0); }
IMP(setlocale) { static uint32_t s; if (!s) s = gstrdup("C"); RET(s); }
static thread_local uint32_t cmp_fn;
static int qsort_trampoline(const void *a, const void *b) { return (int)(int32_t)call(cmp_fn, {G32(a), G32(b)}); }
IMP(qsort) {
    uint32_t saved = cmp_fn; cmp_fn = A(3);
    qsort(GPTR(A(0)), A(1), A(2), qsort_trampoline);
    cmp_fn = saved;
}

// ---------------------------------------------------------------- math (softfp)
#define M1(f) IMP(f) { int i = 0; double x = argd(c, i); retd(c, f(x)); }
#define M2(f) IMP(f) { int i = 0; double x = argd(c, i); double y = argd(c, i); retd(c, f(x, y)); }
#define MF(f) IMP(f) { retf(c, f(argf(c, 0))); }
M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan) M1(round) M2(atan2) M2(pow)
MF(sinf) MF(cosf) MF(asinf) MF(acosf)

// ---------------------------------------------------------------- time
IMP(time) { uint32_t t = (uint32_t)::time(nullptr); if (A(0)) W32(A(0), t); RET(t); }
IMP(gettimeofday) { int64_t us = host::now_us(); if (A(0)) { W32(A(0), (uint32_t)(us / 1000000)); W32(A(0) + 4, (uint32_t)(us % 1000000)); } RET(0); }
IMP(clock_gettime) {
    int64_t ns = A(0) == 0 ? host::now_us() * 1000 : host::mono_ns();     // 0 = CLOCK_REALTIME, others monotonic
    W32(A(1), (uint32_t)(ns / 1000000000)); W32(A(1) + 4, (uint32_t)(ns % 1000000000)); RET(0);
}
static void tm_to_guest(const struct tm &t, uint32_t g, long gmtoff) {
    int v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int k = 0; k < 9; k++) W32(g + 4 * k, v[k]);
    W32(g + 36, (uint32_t)gmtoff); W32(g + 40, 0);
}
static struct tm tm_from_guest(uint32_t g) {
    struct tm t{}; int *f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int k = 0; k < 9; k++) *f[k] = (int)R32(g + 4 * k);
    return t;
}
IMP(localtime) { static thread_local uint32_t buf; if (!buf) buf = gstatic(44); time_t t = (int32_t)R32(A(0)); struct tm r; host::localtime(t, r); tm_to_guest(r, buf, host::gmtoff(t)); RET(buf); }
IMP(gmtime_r) { time_t t = (int32_t)R32(A(0)); struct tm r; host::gmtime(t, r); tm_to_guest(r, A(1), 0); RET(A(1)); }
IMP(mktime) { struct tm t = tm_from_guest(A(0)); time_t r = mktime(&t); tm_to_guest(t, A(0), host::gmtoff(r)); RET((uint32_t)r); }
IMP(strftime) { struct tm t = tm_from_guest(A(3)); RET(strftime((char *)GPTR(A(0)), A(1), GSTR(A(2)), &t)); }


// ======================================================================= additions for libunity / libmono
IMP(strcat) { strcat((char *)GPTR(A(0)), GSTR(A(1))); RET(A(0)); }
IMP(strncat) { strncat((char *)GPTR(A(0)), GSTR(A(1)), A(2)); RET(A(0)); }
IMP(strlcpy) { const char *s = GSTR(A(1)); size_t n = A(2), sl = strlen(s); if (n) { size_t k = std::min(sl, n - 1); memmove(GPTR(A(0)), s, k); W8(A(0) + k, 0); } RET(sl); }
IMP(strndup) { const char *s = GSTR(A(0)); size_t n = strnlen(s, A(1)); uint32_t p = gmalloc(n + 1); memcpy(GPTR(p), s, n); W8(p + n, 0); RET(p); }
IMP(strnlen) { RET(strnlen(GSTR(A(0)), A(1))); }
IMP(strpbrk) { const char *r = strpbrk(GSTR(A(0)), GSTR(A(1))); RET(r ? G32(r) : 0); }
IMP(strspn) { RET(strspn(GSTR(A(0)), GSTR(A(1)))); }
IMP(strcspn) { RET(strcspn(GSTR(A(0)), GSTR(A(1)))); }
IMP(strcasestr) {
    const char *h = GSTR(A(0)), *n = GSTR(A(1)); size_t nl = strlen(n);
    for (const char *p = h; *p; p++) if (!ncase(p, n, nl)) { RET(G32(p)); return; }
    RET(nl ? 0 : A(0));
}
IMP(memmem) {
    const uint8_t *h = (const uint8_t *)GPTR(A(0)), *n = (const uint8_t *)GPTR(A(2)); size_t hl = A(1), nl = A(3);
    if (!nl) { RET(A(0)); return; }
    for (size_t k = 0; k + nl <= hl; k++) if (!memcmp(h + k, n, nl)) { RET(A(0) + (uint32_t)k); return; }
    RET(0);
}
static thread_local uint32_t strtok_save;
IMP(strtok) {
    uint32_t s = A(0) ? A(0) : strtok_save; const char *delim = GSTR(A(1));
    if (!s) { RET(0); return; }
    while (R8(s) && strchr(delim, (int)R8(s))) s++;
    if (!R8(s)) { strtok_save = 0; RET(0); return; }
    uint32_t tok = s;
    while (R8(s) && !strchr(delim, (int)R8(s))) s++;
    if (R8(s)) { W8(s, 0); s++; strtok_save = s; } else strtok_save = 0;
    RET(tok);
}
IMP(atol) { RET(atoi(GSTR(A(0)))); }
IMP(abs) { RET((uint32_t)std::abs((int32_t)A(0))); }
IMP(labs) { RET((uint32_t)std::abs((int32_t)A(0))); }
IMP(div) { int32_t a = (int32_t)A(1), b = (int32_t)A(2); W32(A(0), b ? a / b : 0); W32(A(0) + 4, b ? a % b : 0); RET(A(0)); }  // struct return via r0
IMP(memalign) { RET(gmemalign(A(0), A(1))); }
IMP(posix_memalign) { uint32_t p = gmemalign(A(1) < 8 ? 8 : A(1), A(2)); W32(A(0), p); RET(p ? 0 : G_ENOMEM); }
IMP(valloc) { RET(gmemalign(4096, A(0))); }
IMP(malloc_usable_size) { RET(gmalloc_usable(A(0))); }
static thread_local uint32_t bs_fn;
static int bs_cmp(const void *a, const void *b) { return (int)(int32_t)call(bs_fn, {G32(a), G32(b)}); }
IMP(bsearch) {
    uint32_t saved = bs_fn; bs_fn = A(4);
    void *r = bsearch(GPTR(A(0)), GPTR(A(1)), A(2), A(3), bs_cmp);
    bs_fn = saved; RET(r ? G32(r) : 0);
}
IMP(rand) { RET((uint32_t)rand() & 0x7fffffff); }
IMP(srand) { srand(A(0)); }
IMP(random) { RET((uint32_t)rand() & 0x7fffffff); }
IMP(srandom) { srand(A(0)); }
IMP(_exit) { logf("guest _exit(%d)", (int)A(0)); fflush(nullptr); std::_Exit((int)A(0)); }
IMP(atexit) { RET(0); }
std::recursive_mutex &guard_mutex();
IMP(__cxa_guard_acquire) {
    // ARM EABI guard: 32-bit word, bit 0 = initialized. One global recursive lock serializes static init.
    if (R32(A(0)) & 1) { RET(0); return; }
    guard_mutex().lock();
    if (R32(A(0)) & 1) { guard_mutex().unlock(); RET(0); return; }
    RET(1);
}
IMP(__cxa_guard_release) { W32(A(0), 1); guard_mutex().unlock(); }
IMP(__cxa_guard_abort) { guard_mutex().unlock(); }
IMP(__cxa_pure_virtual) { fatal("pure virtual call from %s", sym_name(c->r[14])); }
IMP(__cxa_begin_cleanup) { fatal("C++ exception unwinding is not supported (from %s)", sym_name(c->r[14])); }
IMP(__cxa_type_match) { fatal("C++ exception matching is not supported (from %s)", sym_name(c->r[14])); }
IMP(__cxa_call_unexpected) { fatal("C++ unexpected exception (from %s)", sym_name(c->r[14])); }

// ---- stdio extras
IMP(printf) { VA va{c, 1, 0, false}; std::string s = gformat(GSTR(A(0)), va); fputs(s.c_str(), stdout); RET(s.size()); }
IMP(vprintf) { VA va{c, 0, A(1), true}; std::string s = gformat(GSTR(A(0)), va); fputs(s.c_str(), stdout); RET(s.size()); }
IMP(vsprintf) { VA va{c, 0, A(2), true}; std::string s = gformat(GSTR(A(1)), va); memcpy(GPTR(A(0)), s.c_str(), s.size() + 1); RET(s.size()); }
IMP(vasprintf) { VA va{c, 0, A(2), true}; std::string s = gformat(GSTR(A(1)), va); W32(A(0), gstrdup(s.c_str())); RET(s.size()); }
IMP(asprintf) { VA va{c, 2, 0, false}; std::string s = gformat(GSTR(A(1)), va); W32(A(0), gstrdup(s.c_str())); RET(s.size()); }
IMP(puts) { puts(GSTR(A(0))); RET(1); }
IMP(putchar) { putchar((int)A(0)); RET(A(0)); }
IMP(perror) { logf("perror: %s", A(0) ? GSTR(A(0)) : ""); }
IMP(__android_log_print) {
    VA va{c, 3, 0, false}; std::string s = gformat(GSTR(A(2)), va);
    logf("[%s] %s", A(1) ? GSTR(A(1)) : "?", s.c_str()); RET(1);
}
IMP(__android_log_vprint) {
    VA va{c, 0, A(3), true}; std::string s = gformat(GSTR(A(2)), va);
    logf("[%s] %s", A(1) ? GSTR(A(1)) : "?", s.c_str()); RET(1);
}
IMP(__android_log_write) { logf("[%s] %s", A(1) ? GSTR(A(1)) : "?", A(2) ? GSTR(A(2)) : ""); RET(1); }

// ---- math: softfp (doubles in r0:r1 / r2:r3, floats in r0/r1)
#define D1(f) IMP(f) { int i = 0; double x = argd(c, i); retd(c, ::f(x)); }
#define D2(f) IMP(f) { int i = 0; double x = argd(c, i); double y = argd(c, i); retd(c, ::f(x, y)); }
#define F1(f) IMP(f) { retf(c, ::f(argf(c, 0))); }
#define F2(f) IMP(f) { retf(c, ::f(argf(c, 0), argf(c, 1))); }
D1(exp) D1(log) D1(log10) D1(sqrt) D1(floor) D1(ceil) D1(sinh) D1(cosh) D1(tanh) D1(rint) D1(trunc) D1(fabs)
D2(fmod) D2(hypot)
F1(expf) F1(logf) F1(log10f) F1(sqrtf) F1(floorf) F1(ceilf) F1(sinhf) F1(coshf) F1(tanhf) F1(tanf) F1(atanf) F1(fabsf)
F1(roundf) F1(truncf)
F2(atan2f) F2(powf) F2(fmodf) F2(hypotf)
IMP(ldexp) { int i = 0; double x = argd(c, i); int e = (int)arg(c, i); retd(c, ::ldexp(x, e)); }
IMP(ldexpf) { retf(c, ::ldexpf(argf(c, 0), (int)A(1))); }
IMP(frexp) { int i = 0; double x = argd(c, i); int e; double r = ::frexp(x, &e); W32(arg(c, i), (uint32_t)e); retd(c, r); }
IMP(modf) { int i = 0; double x = argd(c, i); double ip; double r = ::modf(x, &ip); WV64(arg(c, i), ip); retd(c, r); }
IMP(__fpclassifyf) {
    float x = argf(c, 0);
    // bionic: FP_INFINITE 0x01, FP_NAN 0x02, FP_NORMAL 0x04, FP_SUBNORMAL 0x08, FP_ZERO 0x10
    int k = std::fpclassify(x);
    RET(k == FP_INFINITE ? 1 : k == FP_NAN ? 2 : k == FP_NORMAL ? 4 : k == FP_SUBNORMAL ? 8 : 0x10);
}
IMP(__fpclassifyd) {
    int i = 0; double x = argd(c, i); int k = std::fpclassify(x);
    RET(k == FP_INFINITE ? 1 : k == FP_NAN ? 2 : k == FP_NORMAL ? 4 : k == FP_SUBNORMAL ? 8 : 0x10);
}
IMP(__isinf) { int i = 0; RET(std::isinf(argd(c, i))); }
IMP(__isnan) { int i = 0; RET(std::isnan(argd(c, i))); }
IMP(isinf) { int i = 0; RET(std::isinf(argd(c, i))); }
IMP(isnan) { int i = 0; RET(std::isnan(argd(c, i))); }

// ---- time extras
IMP(clock) { RET((uint32_t)(host::mono_ns() / 1000)); }
IMP(clock_getres) { if (A(1)) { W32(A(1), 0); W32(A(1) + 4, 1); } RET(0); }
IMP(gmtime) { static thread_local uint32_t buf; if (!buf) buf = gstatic(44); time_t t = (int32_t)R32(A(0)); struct tm r; host::gmtime(t, r); tm_to_guest(r, buf, 0); RET(buf); }
IMP(localtime_r) { time_t t = (int32_t)R32(A(0)); struct tm r; host::localtime(t, r); tm_to_guest(r, A(1), host::gmtoff(t)); RET(A(1)); }

// ---- SHA1 (bionic exports the BSD SHA1 API): SHA1_CTX { u32 state[5]; u32 count[2]; u8 buffer[64]; }
namespace {
inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
void sha1_block(uint32_t st[5], const uint8_t *p) {
    uint32_t w[80];
    for (int k = 0; k < 16; k++) w[k] = (uint32_t)p[4 * k] << 24 | (uint32_t)p[4 * k + 1] << 16 | (uint32_t)p[4 * k + 2] << 8 | p[4 * k + 3];
    for (int k = 16; k < 80; k++) w[k] = rol(w[k - 3] ^ w[k - 8] ^ w[k - 14] ^ w[k - 16], 1);
    uint32_t a = st[0], b = st[1], cc = st[2], d = st[3], e = st[4];
    for (int k = 0; k < 80; k++) {
        uint32_t f, kk;
        if (k < 20) { f = (b & cc) | (~b & d); kk = 0x5A827999; }
        else if (k < 40) { f = b ^ cc ^ d; kk = 0x6ED9EBA1; }
        else if (k < 60) { f = (b & cc) | (b & d) | (cc & d); kk = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; kk = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + kk + w[k]; e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += cc; st[3] += d; st[4] += e;
}
}
IMP(SHA1Init) {
    uint32_t x = A(0); uint32_t init[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    for (int k = 0; k < 5; k++) W32(x + 4 * k, init[k]);
    W32(x + 20, 0); W32(x + 24, 0);
}
IMP(SHA1Update) {
    uint32_t x = A(0); const uint8_t *data = (const uint8_t *)GPTR(A(1)); uint32_t len = A(2);
    uint32_t st[5]; for (int k = 0; k < 5; k++) st[k] = R32(x + 4 * k);
    uint64_t bits = (uint64_t)R32(x + 20) | ((uint64_t)R32(x + 24) << 32);
    uint32_t have = (uint32_t)((bits >> 3) & 63);
    uint8_t *buf = (uint8_t *)GPTR(x + 28);
    bits += (uint64_t)len << 3;
    for (uint32_t k = 0; k < len; k++) {
        buf[have++] = data[k];
        if (have == 64) { sha1_block(st, buf); have = 0; }
    }
    for (int k = 0; k < 5; k++) W32(x + 4 * k, st[k]);
    W32(x + 20, (uint32_t)bits); W32(x + 24, (uint32_t)(bits >> 32));
}
IMP(SHA1Final) {
    uint32_t digest = A(0), x = A(1);
    uint64_t bits = (uint64_t)R32(x + 20) | ((uint64_t)R32(x + 24) << 32);
    uint8_t pad[72] = {0x80}; uint32_t have = (uint32_t)((bits >> 3) & 63);
    uint32_t padlen = have < 56 ? 56 - have : 120 - have;
    uint8_t lenb[8]; for (int k = 0; k < 8; k++) lenb[k] = (uint8_t)(bits >> (56 - 8 * k));
    // feed through SHA1Update semantics
    uint32_t tmp = gmalloc(padlen + 8); memcpy(GPTR(tmp), pad, padlen); memcpy(GPTR(tmp + padlen), lenb, 8);
    CPU c2 = *c; c2.r[0] = x; c2.r[1] = tmp; c2.r[2] = padlen + 8; imp_SHA1Update(&c2);
    gfree(tmp);
    for (int k = 0; k < 5; k++) { uint32_t v = R32(x + 4 * k); W8(digest + 4 * k, v >> 24); W8(digest + 4 * k + 1, v >> 16); W8(digest + 4 * k + 2, v >> 8); W8(digest + 4 * k + 3, v); }
}

// ---- data symbols provided by "libc"
namespace {
uint32_t make_ctype() {
    // bionic: const char *_ctype_ -> table[257] (index c+1); _U 1 _L 2 _N 4 _S 8 _P 0x10 _C 0x20 _X 0x40 _B 0x80
    uint32_t tbl = gstatic(257);
    for (int ch = 0; ch < 256; ch++) {
        uint8_t f = 0;
        if (ch < 128) {
            if (isupper(ch)) f |= 1; if (islower(ch)) f |= 2; if (isdigit(ch)) f |= 4; if (isspace(ch)) f |= 8;
            if (ispunct(ch)) f |= 0x10; if (iscntrl(ch)) f |= 0x20; if (isxdigit(ch) && !isdigit(ch)) f |= 0x40; if (ch == ' ') f |= 0x80;
        }
        W8(tbl + 1 + ch, f);
    }
    uint32_t p = gstatic(4); W32(p, tbl); return p;
}
uint32_t make_case(bool upper) {
    uint32_t tbl = gstatic(257 * 2);
    W16(tbl, 0xffff);
    for (int ch = 0; ch < 256; ch++) {
        int v = ch;
        if (upper && ch >= 'a' && ch <= 'z') v = ch - 32;
        if (!upper && ch >= 'A' && ch <= 'Z') v = ch + 32;
        W16(tbl + 2 + 2 * ch, (uint16_t)v);
    }
    uint32_t p = gstatic(4); W32(p, tbl); return p;
}
}
extern "C" uint32_t rt_sF();
extern "C" uint32_t rt_environ();
extern "C" uint32_t rt_data_symbol(const char *name) {
    static std::mutex mu; std::lock_guard<std::mutex> g(mu);
    static std::map<std::string, uint32_t> cache;
    auto it = cache.find(name); if (it != cache.end()) return it->second;
    uint32_t v = 0;
    if (!strcmp(name, "__sF")) v = rt_sF();
    else if (!strcmp(name, "__stack_chk_guard")) { v = gstatic(4); W32(v, 0x41a1c0deu); }
    else if (!strcmp(name, "_ctype_")) v = make_ctype();
    else if (!strcmp(name, "_tolower_tab_")) v = make_case(false);
    else if (!strcmp(name, "_toupper_tab_")) v = make_case(true);
    else if (!strcmp(name, "environ")) v = rt_environ();
    else if (!strcmp(name, "__page_size")) { v = gstatic(4); W32(v, 4096); }
    else return 0;
    cache[name] = v;
    return v;
}
std::recursive_mutex &guard_mutex() { static std::recursive_mutex m; return m; }
