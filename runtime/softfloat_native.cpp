// Native replacements for libgcc's ARM EABI soft-float / 64-bit helpers (softfp ABI: floats in r0/r1,
// doubles in r0:r1 / r2:r3). Older Unity builds (armeabi, armeabi-vfp) do most float math through
// these routines; running them as host FPU instructions is exact (IEEE round-to-nearest) and far faster.
#include "guest.h"
#include <cmath>
#include <cstdint>

static inline float F(uint32_t u) { return fromb(u); }
static inline double D(uint32_t lo, uint32_t hi) { return fromd(((uint64_t)hi << 32) | lo); }
static inline void RF(CPU *c, float f) { c->r[0] = fbits(fnan(f)); }
static inline void RD(CPU *c, double d) { uint64_t u = dbits(dnan(d)); c->r[0] = (uint32_t)u; c->r[1] = (uint32_t)(u >> 32); }
static inline void R64(CPU *c, uint64_t u) { c->r[0] = (uint32_t)u; c->r[1] = (uint32_t)(u >> 32); }
static inline double DA(CPU *c) { return D(c->r[0], c->r[1]); }
static inline double DB(CPU *c) { return D(c->r[2], c->r[3]); }
// flags as "cmp a, b" would leave them (Z: equal, C: a >= b or unordered)
static inline void cflags(CPU *c, double a, double b) {
    if (a != a || b != b) { c->z = 0; c->cf = 1; c->n = 0; c->v = 1; return; }
    c->z = a == b; c->cf = a >= b; c->n = a < b; c->v = 0;
}
static inline int64_t sat_ll(double x) {
    if (x != x) return 0;
    if (x >= 9223372036854775807.0) return INT64_MAX;
    if (x <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)x;
}
static inline uint64_t sat_ull(double x) {
    if (x != x || x <= 0) return 0;
    if (x >= 18446744073709551615.0) return UINT64_MAX;
    return (uint64_t)x;
}

extern "C" {
#define NF(n) void nat_##n(CPU *c)
// ---- single precision
NF(__aeabi_fadd) { RF(c, F(c->r[0]) + F(c->r[1])); }  NF(__addsf3) { nat___aeabi_fadd(c); }
NF(__aeabi_fsub) { RF(c, F(c->r[0]) - F(c->r[1])); }  NF(__subsf3) { nat___aeabi_fsub(c); }
NF(__aeabi_frsub) { RF(c, F(c->r[1]) - F(c->r[0])); }
NF(__aeabi_fmul) { RF(c, F(c->r[0]) * F(c->r[1])); }  NF(__mulsf3) { nat___aeabi_fmul(c); }
NF(__aeabi_fdiv) { RF(c, F(c->r[0]) / F(c->r[1])); }  NF(__divsf3) { nat___aeabi_fdiv(c); }
NF(__aeabi_fcmpeq) { c->r[0] = F(c->r[0]) == F(c->r[1]); }
NF(__aeabi_fcmplt) { c->r[0] = F(c->r[0]) < F(c->r[1]); }
NF(__aeabi_fcmple) { c->r[0] = F(c->r[0]) <= F(c->r[1]); }
NF(__aeabi_fcmpge) { c->r[0] = F(c->r[0]) >= F(c->r[1]); }
NF(__aeabi_fcmpgt) { c->r[0] = F(c->r[0]) > F(c->r[1]); }
NF(__aeabi_fcmpun) { float a = F(c->r[0]), b = F(c->r[1]); c->r[0] = a != a || b != b; }
NF(__aeabi_cfcmple) { cflags(c, F(c->r[0]), F(c->r[1])); }
NF(__aeabi_cfcmpeq) { cflags(c, F(c->r[0]), F(c->r[1])); }
NF(__aeabi_cfrcmple) { cflags(c, F(c->r[1]), F(c->r[0])); }
// libgcc comparison results: <0, 0, >0; "unordered" picks the value that makes the test false
static inline int32_t cmpf(double a, double b, int32_t unord) { return (a != a || b != b) ? unord : a < b ? -1 : a > b ? 1 : 0; }
NF(__cmpsf2) { c->r[0] = (uint32_t)cmpf(F(c->r[0]), F(c->r[1]), 1); }
NF(__eqsf2) { nat___cmpsf2(c); }  NF(__nesf2) { nat___cmpsf2(c); }
NF(__lesf2) { nat___cmpsf2(c); }  NF(__ltsf2) { nat___cmpsf2(c); }
NF(__gesf2) { c->r[0] = (uint32_t)cmpf(F(c->r[0]), F(c->r[1]), -1); }  NF(__gtsf2) { nat___gesf2(c); }
NF(__unordsf2) { float a = F(c->r[0]), b = F(c->r[1]); c->r[0] = a != a || b != b; }
NF(__aeabi_f2iz) { c->r[0] = f2s(F(c->r[0])); }       NF(__fixsfsi) { nat___aeabi_f2iz(c); }
NF(__aeabi_f2uiz) { c->r[0] = f2u(F(c->r[0])); }      NF(__fixunssfsi) { nat___aeabi_f2uiz(c); }
NF(__aeabi_f2lz) { R64(c, (uint64_t)sat_ll(F(c->r[0]))); }   NF(__fixsfdi) { nat___aeabi_f2lz(c); }
NF(__aeabi_f2ulz) { R64(c, sat_ull(F(c->r[0]))); }          NF(__fixunssfdi) { nat___aeabi_f2ulz(c); }
NF(__aeabi_i2f) { RF(c, (float)(int32_t)c->r[0]); }   NF(__floatsisf) { nat___aeabi_i2f(c); }
NF(__aeabi_ui2f) { RF(c, (float)c->r[0]); }           NF(__floatunsisf) { nat___aeabi_ui2f(c); }
NF(__aeabi_l2f) { RF(c, (float)(int64_t)(((uint64_t)c->r[1] << 32) | c->r[0])); }  NF(__floatdisf) { nat___aeabi_l2f(c); }
NF(__aeabi_ul2f) { RF(c, (float)(((uint64_t)c->r[1] << 32) | c->r[0])); }          NF(__floatundisf) { nat___aeabi_ul2f(c); }
NF(__aeabi_f2d) { RD(c, (double)F(c->r[0])); }        NF(__extendsfdf2) { nat___aeabi_f2d(c); }
// ---- double precision
NF(__aeabi_dadd) { RD(c, DA(c) + DB(c)); }  NF(__adddf3) { nat___aeabi_dadd(c); }
NF(__aeabi_dsub) { RD(c, DA(c) - DB(c)); }  NF(__subdf3) { nat___aeabi_dsub(c); }
NF(__aeabi_drsub) { RD(c, DB(c) - DA(c)); }
NF(__aeabi_dmul) { RD(c, DA(c) * DB(c)); }  NF(__muldf3) { nat___aeabi_dmul(c); }
NF(__aeabi_ddiv) { RD(c, DA(c) / DB(c)); }  NF(__divdf3) { nat___aeabi_ddiv(c); }
NF(__aeabi_dcmpeq) { c->r[0] = DA(c) == DB(c); }
NF(__aeabi_dcmplt) { c->r[0] = DA(c) < DB(c); }
NF(__aeabi_dcmple) { c->r[0] = DA(c) <= DB(c); }
NF(__aeabi_dcmpge) { c->r[0] = DA(c) >= DB(c); }
NF(__aeabi_dcmpgt) { c->r[0] = DA(c) > DB(c); }
NF(__aeabi_dcmpun) { double a = DA(c), b = DB(c); c->r[0] = a != a || b != b; }
NF(__aeabi_cdcmple) { cflags(c, DA(c), DB(c)); }
NF(__aeabi_cdcmpeq) { cflags(c, DA(c), DB(c)); }
NF(__aeabi_cdrcmple) { cflags(c, DB(c), DA(c)); }
NF(__cmpdf2) { c->r[0] = (uint32_t)cmpf(DA(c), DB(c), 1); }
NF(__eqdf2) { nat___cmpdf2(c); }  NF(__nedf2) { nat___cmpdf2(c); }
NF(__ledf2) { nat___cmpdf2(c); }  NF(__ltdf2) { nat___cmpdf2(c); }
NF(__gedf2) { c->r[0] = (uint32_t)cmpf(DA(c), DB(c), -1); }  NF(__gtdf2) { nat___gedf2(c); }
NF(__unorddf2) { double a = DA(c), b = DB(c); c->r[0] = a != a || b != b; }
NF(__aeabi_d2iz) { c->r[0] = f2s(DA(c)); }      NF(__fixdfsi) { nat___aeabi_d2iz(c); }
NF(__aeabi_d2uiz) { c->r[0] = f2u(DA(c)); }     NF(__fixunsdfsi) { nat___aeabi_d2uiz(c); }
NF(__aeabi_d2lz) { R64(c, (uint64_t)sat_ll(DA(c))); }  NF(__fixdfdi) { nat___aeabi_d2lz(c); }
NF(__aeabi_d2ulz) { R64(c, sat_ull(DA(c))); }          NF(__fixunsdfdi) { nat___aeabi_d2ulz(c); }
NF(__aeabi_i2d) { RD(c, (double)(int32_t)c->r[0]); }   NF(__floatsidf) { nat___aeabi_i2d(c); }
NF(__aeabi_ui2d) { RD(c, (double)c->r[0]); }           NF(__floatunsidf) { nat___aeabi_ui2d(c); }
NF(__aeabi_l2d) { RD(c, (double)(int64_t)(((uint64_t)c->r[1] << 32) | c->r[0])); }  NF(__floatdidf) { nat___aeabi_l2d(c); }
NF(__aeabi_ul2d) { RD(c, (double)(((uint64_t)c->r[1] << 32) | c->r[0])); }          NF(__floatundidf) { nat___aeabi_ul2d(c); }
NF(__aeabi_d2f) { RF(c, (float)DA(c)); }        NF(__truncdfsf2) { nat___aeabi_d2f(c); }
// ---- 64-bit integer helpers
NF(__aeabi_llsl) { uint64_t v = ((uint64_t)c->r[1] << 32) | c->r[0]; uint32_t n = c->r[2]; R64(c, n >= 64 ? 0 : v << n); }
NF(__aeabi_llsr) { uint64_t v = ((uint64_t)c->r[1] << 32) | c->r[0]; uint32_t n = c->r[2]; R64(c, n >= 64 ? 0 : v >> n); }
NF(__aeabi_lasr) { int64_t v = (int64_t)(((uint64_t)c->r[1] << 32) | c->r[0]); uint32_t n = c->r[2]; R64(c, (uint64_t)(n >= 64 ? v >> 63 : v >> n)); }
NF(__ashldi3) { nat___aeabi_llsl(c); }  NF(__lshrdi3) { nat___aeabi_llsr(c); }  NF(__ashrdi3) { nat___aeabi_lasr(c); }
NF(__aeabi_lmul) { uint64_t a = ((uint64_t)c->r[1] << 32) | c->r[0], b = ((uint64_t)c->r[3] << 32) | c->r[2]; R64(c, a * b); }
NF(__muldi3) { nat___aeabi_lmul(c); }
}
