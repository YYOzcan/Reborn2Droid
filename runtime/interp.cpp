// ARM (A32) + VFP interpreter for guest code that has no static translation: Mono's JIT output, its
// trampolines, and any library code the recompiler could not reach.
//
// Model: an "activation" runs guest code until it returns to the address that was in lr when it was
// entered (with the same sp).  Calls to recompiled functions and imports are host calls.  Mono's
// exception unwinding restores a register context and jumps into an outer managed frame; when the
// interpreter sees a return into an *outer* activation it unwinds the host stack to it (longjmp).
#include "rt.h"
#include <cstring>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <vector>

using namespace rt;

namespace {
struct Activation {
    uint32_t stop_pc, stop_sp;   // stop_sp 0: do not check sp (entered mid-function)
    rb_jmp_buf jb;
    Activation *prev;
    CPU *c;
};
thread_local Activation *t_act;
thread_local uint64_t t_steps;

inline bool cond_ok(CPU *c, uint32_t cond) {
    switch (cond) {
    case 0: return c->z; case 1: return !c->z; case 2: return c->cf; case 3: return !c->cf;
    case 4: return c->n; case 5: return !c->n; case 6: return c->v; case 7: return !c->v;
    case 8: return c->cf && !c->z; case 9: return !c->cf || c->z;
    case 10: return c->n == c->v; case 11: return c->n != c->v;
    case 12: return !c->z && c->n == c->v; case 13: return c->z || c->n != c->v;
    default: return true;
    }
}

[[noreturn]] void bad(CPU *c, uint32_t pc, uint32_t insn, const char *what) {
    fatal("interpreter: %s at 0x%08x (%s): insn %08x  lr=0x%08x (%s)", what, pc, sym_name(pc), insn, c->r[14], sym_name(c->r[14]));
}

// memory with the guest null page (and other unmapped low memory) turned into guest SIGSEGV
struct Fault { uint32_t addr; };
inline void chk(uint32_t a) { if (__builtin_expect(a < 0x10000u, 0)) throw Fault{a}; }
inline uint32_t ld32(uint32_t a) { chk(a); return R32(a); }
inline uint32_t ld16(uint32_t a) { chk(a); return R16(a); }
inline uint32_t ld8(uint32_t a) { chk(a); return R8(a); }
inline void st32(uint32_t a, uint32_t v) { chk(a); W32(a, v); }
inline void st16(uint32_t a, uint32_t v) { chk(a); W16(a, v); }
inline void st8(uint32_t a, uint32_t v) { chk(a); W8(a, v); }

inline uint32_t expand_imm(CPU *c, uint32_t insn, uint32_t *carry) {
    uint32_t imm = insn & 0xff, rot = ((insn >> 8) & 0xf) * 2;
    uint32_t v = ror32(imm, rot);
    *carry = rot ? v >> 31 : c->cf;
    return v;
}

// register operand with shift (data processing); pc reads as pc+8 (+4 more for register-shifted forms)
inline uint32_t shifter(CPU *c, uint32_t insn, uint32_t pc, uint32_t *carry) {
    uint32_t rm = insn & 0xf, type = (insn >> 5) & 3;
    uint32_t v = rm == 15 ? pc + 8 : c->r[rm];
    if (insn & 0x10) {                                  // register-shifted register
        uint32_t rs = (insn >> 8) & 0xf;
        if (rm == 15) v = pc + 12;
        uint32_t n = c->r[rs] & 0xff;
        return shc(c, v, (int)type, n, carry);
    }
    uint32_t imm = (insn >> 7) & 0x1f;
    if (type == 3 && imm == 0) return shc(c, v, 4, 1, carry);           // rrx
    if ((type == 1 || type == 2) && imm == 0) imm = 32;
    return shc(c, v, (int)type, imm, carry);
}
inline uint32_t addr_offset_reg(CPU *c, uint32_t insn, uint32_t pc) {
    uint32_t rm = insn & 0xf, type = (insn >> 5) & 3, imm = (insn >> 7) & 0x1f;
    uint32_t v = rm == 15 ? pc + 8 : c->r[rm];
    if (type == 3 && imm == 0) return (v >> 1) | (c->cf << 31);
    if ((type == 1 || type == 2) && imm == 0) imm = 32;
    return sh(v, (int)type, imm, c->cf);
}

inline float S(CPU *c, uint32_t n) { return c->s[n]; }
inline double D(CPU *c, uint32_t n) { return c->d[n]; }

struct Ctl { uint32_t pc; bool branched; };

}  // namespace

namespace interp {
static bool prof_on = getenv("RB_PROF") != nullptr;
static std::mutex prof_mu;
static std::unordered_map<uint32_t, uint64_t> prof_hist;
static void prof_sample(uint32_t pc) { std::lock_guard<std::mutex> g(prof_mu); prof_hist[pc]++; }
void prof_dump() {
    if (!prof_on) return;
    std::lock_guard<std::mutex> g(prof_mu);
    std::vector<std::pair<uint64_t, uint32_t>> v; uint64_t tot = 0, stat = 0;
    for (auto &kv : prof_hist) { v.push_back({kv.second, kv.first}); tot += kv.second; if (is_static_code(kv.first)) stat += kv.second; }
    std::sort(v.rbegin(), v.rend());
    logf("interp profile: %llu samples, %.1f%% in library code", (unsigned long long)tot, tot ? 100.0 * stat / tot : 0.0);
    for (size_t k = 0; k < v.size() && k < 40; k++) logf("  %6.2f%% %s", 100.0 * v[k].first / tot, sym_name(v[k].second));
}

static void run(CPU *c, uint32_t pc, Activation *act);

// returns true if pc is the return point of this activation
static inline bool is_stop(const Activation *a, CPU *c, uint32_t pc) {
    return (pc & ~1u) == a->stop_pc && (a->stop_sp == 0 || c->r[13] == a->stop_sp);
}

// pc reached by an indirect branch: return/unwind/host call decisions. Returns false to continue at pc.
static bool on_indirect(CPU *c, uint32_t &pc, Activation *act) {
    if (is_stop(act, c, pc)) return true;
    if (is_static_code(pc)) {
        for (Activation *o = act->prev; o; o = o->prev)
            if (o->stop_sp && is_stop(o, c, pc)) {           // a return into an outer activation: unwind to it
                RB_LONGJMP(o->jb);
            }
    }
    return false;
}

void call(CPU *c, uint32_t pc) {
    Activation a;
    a.stop_pc = c->r[14] & ~1u; a.stop_sp = c->r[13]; a.prev = t_act; a.c = c;
    t_act = &a;
    if (RB_SETJMP(a.jb)) { t_act = a.prev; return; }
    run(c, pc & ~1u, &a);
    t_act = a.prev;
}

void resume(CPU *c, uint32_t pc, uint32_t stop) {
    Activation a;
    a.stop_pc = stop & ~1u; a.stop_sp = 0; a.prev = t_act; a.c = c;
    t_act = &a;
    if (RB_SETJMP(a.jb)) { t_act = a.prev; return; }
    run(c, pc & ~1u, &a);
    t_act = a.prev;
}

uint64_t steps() { return t_steps; }

// a direct or indirect call/jump to code with a host implementation: run it, continue at `ret`
static inline bool host_call(CPU *c, uint32_t target, uint32_t ret) {
    GuestFnPtr f = host_fn(target);
    if (!f) return false;
    c->r[14] = ret;
    f(c);
    return true;
}

static void vfp_data(CPU *c, uint32_t insn, uint32_t pc);
static void vfp_ldst(CPU *c, uint32_t insn, uint32_t pc);
static void vfp_xfer(CPU *c, uint32_t insn, uint32_t pc);

static void run(CPU *c, uint32_t pc, Activation *act) {
    for (;;) {
        uint32_t insn;
        try {
        for (;;) {
            if (__builtin_expect(c->sigpending, 0)) { c->r[15] = pc; guest_poll(c); pc = c->r[15]; }
            if (pc & 1) bad(c, pc, 0, "thumb code is not supported by the interpreter");
            insn = R32(pc);
            if ((++t_steps & 1023) == 0 && prof_on) prof_sample(pc);
            uint32_t cond = insn >> 28;
            uint32_t npc = pc + 4;
            if (cond == 0xf) {
                // unconditional space
                if ((insn & 0x0e000000) == 0x0a000000) {             // blx imm
                    int32_t off = ((int32_t)(insn << 8) >> 6) | ((insn >> 23) & 2);
                    uint32_t t = pc + 8 + off;
                    c->r[14] = npc;
                    if (host_call(c, t | 1, npc)) { pc = npc; continue; }
                    bad(c, pc, insn, "blx to thumb code");
                }
                if ((insn & 0xfffffff0) == 0xf57ff040 || (insn & 0xfffffff0) == 0xf57ff050 ||
                    (insn & 0xfffffff0) == 0xf57ff060) { __atomic_thread_fence(__ATOMIC_SEQ_CST); pc = npc; continue; }   // dsb dmb isb
                if (insn == 0xf57ff01f) { c->excl_addr = 0xffffffffu; pc = npc; continue; }  // clrex
                if ((insn & 0xfd70f000) == 0xf550f000 || (insn & 0xfd70f000) == 0xf510f000) { pc = npc; continue; }   // pld / pldw
                if ((insn & 0xff100010) == 0xf4000000 || (insn & 0xfe000000) == 0xf2000000) bad(c, pc, insn, "neon");
                bad(c, pc, insn, "unconditional insn");
            }
            if (!cond_ok(c, cond)) { pc = npc; continue; }
            uint32_t op1 = (insn >> 25) & 7;
            switch (op1) {
            case 0: case 1: {
                // data processing / misc / multiply / extra load-store
                bool imm = op1 == 1;
                if (!imm && (insn & 0x90) == 0x90) {
                    // multiply, swap, ldrex, extra load/store
                    uint32_t op = (insn >> 5) & 3;
                    if (op == 0) {
                        if ((insn & 0x0f8000f0) == 0x00000090 || (insn & 0x0fe000f0) == 0x00600090) {     // mul/mla/mls
                            uint32_t rd = (insn >> 16) & 15, ra = (insn >> 12) & 15, rm = (insn >> 8) & 15, rn = insn & 15;
                            uint32_t opc = (insn >> 21) & 0xf; uint32_t r;
                            if (opc == 0) r = c->r[rn] * c->r[rm];
                            else if (opc == 1) r = c->r[rn] * c->r[rm] + c->r[ra];
                            else if (opc == 3) r = c->r[ra] - c->r[rn] * c->r[rm];
                            else bad(c, pc, insn, "multiply");
                            c->r[rd] = r;
                            if (insn & (1u << 20)) { c->n = r >> 31; c->z = r == 0; }
                            pc = npc; continue;
                        }
                        if ((insn & 0x0f8000f0) == 0x00800090) {                                         // umull/umlal/smull/smlal
                            uint32_t hi = (insn >> 16) & 15, lo = (insn >> 12) & 15, rm = (insn >> 8) & 15, rn = insn & 15;
                            bool sgn = insn & (1u << 22), acc = insn & (1u << 21);
                            uint64_t r = sgn ? (uint64_t)((int64_t)(int32_t)c->r[rn] * (int64_t)(int32_t)c->r[rm])
                                             : (uint64_t)c->r[rn] * c->r[rm];
                            if (acc) r += ((uint64_t)c->r[hi] << 32) | c->r[lo];
                            c->r[lo] = (uint32_t)r; c->r[hi] = (uint32_t)(r >> 32);
                            if (insn & (1u << 20)) { c->n = (uint32_t)(r >> 63); c->z = r == 0; }
                            pc = npc; continue;
                        }
                        if ((insn & 0x0fb00ff0) == 0x01000090) {                                         // swp/swpb
                            uint32_t rn = (insn >> 16) & 15, rt = (insn >> 12) & 15, rt2 = insn & 15;
                            chk(c->r[rn]);
                            c->r[rt] = SWP(c, c->r[rn], c->r[rt2], (insn & (1u << 22)) ? 1 : 4);
                            pc = npc; continue;
                        }
                        if ((insn & 0x0f8000f0) == 0x01800090) {                                         // ldrex/strex family
                            uint32_t rn = (insn >> 16) & 15, rt = (insn >> 12) & 15, rt2 = insn & 15;
                            uint32_t sz = (insn >> 21) & 3; bool ld = insn & (1u << 20);
                            uint32_t a = c->r[rn]; chk(a);
                            if (ld) {
                                if (sz == 1) { uint64_t v = LDREXD(c, a); c->r[rt] = (uint32_t)v; c->r[rt + 1] = (uint32_t)(v >> 32); }
                                else c->r[rt] = LDREX(c, a, sz == 0 ? 4 : sz == 2 ? 1 : 2);
                            } else {
                                if (sz == 1) c->r[rt] = STREXD(c, a, ((uint64_t)c->r[rt2 + 1] << 32) | c->r[rt2]);
                                else c->r[rt] = STREX(c, a, c->r[rt2], sz == 0 ? 4 : sz == 2 ? 1 : 2);
                            }
                            pc = npc; continue;
                        }
                        bad(c, pc, insn, "multiply/sync");
                    }
                    // extra load/store: ldrh strh ldrsb ldrsh ldrd strd
                    {
                        bool P = insn & (1u << 24), U = insn & (1u << 23), I = insn & (1u << 22), W = insn & (1u << 21),
                             L = insn & (1u << 20);
                        uint32_t rn = (insn >> 16) & 15, rt = (insn >> 12) & 15;
                        uint32_t off = I ? (((insn >> 4) & 0xf0) | (insn & 0xf)) : c->r[insn & 15];
                        uint32_t base = rn == 15 ? pc + 8 : c->r[rn];
                        uint32_t off_a = U ? base + off : base - off;
                        uint32_t a = P ? off_a : base;
                        if (L) {
                            uint32_t v;
                            if (op == 1) v = ld16(a);
                            else if (op == 2) v = (uint32_t)(int32_t)(int8_t)ld8(a);
                            else v = (uint32_t)(int32_t)(int16_t)ld16(a);
                            if (!P || W) c->r[rn] = off_a;
                            c->r[rt] = v;
                        } else {
                            if (op == 1) st16(a, rt == 15 ? pc + 8 : c->r[rt]);
                            else if (op == 2) { uint32_t v1 = ld32(a), v2 = ld32(a + 4); if (!P || W) c->r[rn] = off_a; c->r[rt] = v1; c->r[rt + 1] = v2; pc = npc; continue; }   // ldrd
                            else { st32(a, c->r[rt]); st32(a + 4, c->r[rt + 1]); }                         // strd
                            if (!P || W) c->r[rn] = off_a;
                        }
                        if (L && rt == 15) { pc = c->r[15]; goto indirect; }
                        pc = npc; continue;
                    }
                }
                uint32_t opc = (insn >> 21) & 0xf; bool Sb = insn & (1u << 20);
                if (!Sb && opc >= 8 && opc <= 11) {
                    // misc: movw/movt, msr/mrs, bx, blx reg, clz, ...
                    if (imm) {
                        uint32_t rd = (insn >> 12) & 15, v = ((insn >> 4) & 0xf000) | (insn & 0xfff);
                        if (opc == 8) { c->r[rd] = v; pc = npc; continue; }                  // movw
                        if (opc == 10) { c->r[rd] = (c->r[rd] & 0xffff) | (v << 16); pc = npc; continue; }   // movt
                        if ((insn & 0x0ffff0ff) == 0x0320f000 || (insn & 0x0ffffff0) == 0x0320f0f0) { pc = npc; continue; }  // nop/yield/hints
                        bad(c, pc, insn, "msr imm");
                    }
                    uint32_t op2 = (insn >> 4) & 7;
                    if (op2 == 1 && opc == 9) {                                                  // bx
                        uint32_t t = c->r[insn & 15];
                        pc = t; goto indirect;
                    }
                    if (op2 == 3 && opc == 9) {                                                  // blx reg
                        uint32_t t = c->r[insn & 15];
                        c->r[14] = npc;
                        if (host_call(c, t, npc)) { pc = npc; continue; }
                        if (t & 1) bad(c, pc, insn, "blx reg to thumb");
                        pc = t; continue;
                    }
                    if (op2 == 1 && opc == 11) {                                                 // clz
                        c->r[(insn >> 12) & 15] = clz32(c->r[insn & 15]); pc = npc; continue;
                    }
                    if (op2 == 0 && (opc == 8 || opc == 10)) {                                   // mrs
                        uint32_t v = (c->n << 31) | (c->z << 30) | (c->cf << 29) | (c->v << 28) | 0x10;
                        c->r[(insn >> 12) & 15] = v; pc = npc; continue;
                    }
                    if (op2 == 0 && opc == 9) {                                                  // msr apsr, reg
                        uint32_t v = c->r[insn & 15];
                        if (insn & (1u << 19)) { c->n = v >> 31; c->z = (v >> 30) & 1; c->cf = (v >> 29) & 1; c->v = (v >> 28) & 1; }
                        pc = npc; continue;
                    }
                    if (op2 == 7 && opc == 9) bad(c, pc, insn, "bkpt");
                    if ((op2 & 4) == 0 && (insn & 0x80) && (insn & 0x10) == 0) {                // smulxy / smlaxy
                        uint32_t rd = (insn >> 16) & 15, ra = (insn >> 12) & 15, rm = (insn >> 8) & 15, rn = insn & 15;
                        int32_t a = (insn & 0x20) ? (int16_t)(c->r[rn] >> 16) : (int16_t)c->r[rn];
                        int32_t b = (insn & 0x40) ? (int16_t)(c->r[rm] >> 16) : (int16_t)c->r[rm];
                        if (opc == 11) c->r[rd] = (uint32_t)(a * b);
                        else if (opc == 8) c->r[rd] = (uint32_t)(a * b) + c->r[ra];
                        else bad(c, pc, insn, "dsp multiply");
                        pc = npc; continue;
                    }
                    bad(c, pc, insn, "misc");
                }
                uint32_t rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
                uint32_t carry, b = imm ? expand_imm(c, insn, &carry) : shifter(c, insn, pc, &carry);
                uint32_t a = rn == 15 ? pc + 8 : c->r[rn];
                if (!imm && rn == 15 && (insn & 0x10)) a = pc + 12;
                uint32_t r = 0; bool logical = false, write = true;
                switch (opc) {
                case 0: r = a & b; logical = true; break;
                case 1: r = a ^ b; logical = true; break;
                case 2: r = Sb ? addc(c, a, ~b, 1) : a - b; break;
                case 3: r = Sb ? addc(c, b, ~a, 1) : b - a; break;
                case 4: r = Sb ? addc(c, a, b, 0) : a + b; break;
                case 5: r = Sb ? addc(c, a, b, c->cf) : a + b + c->cf; break;
                case 6: r = Sb ? addc(c, a, ~b, c->cf) : a + ~b + c->cf; break;
                case 7: r = Sb ? addc(c, b, ~a, c->cf) : b + ~a + c->cf; break;
                case 8: r = a & b; logical = true; write = false; break;
                case 9: r = a ^ b; logical = true; write = false; break;
                case 10: addc(c, a, ~b, 1); write = false; Sb = false; break;
                case 11: addc(c, a, b, 0); write = false; Sb = false; break;
                case 12: r = a | b; logical = true; break;
                case 13: r = b; logical = true; break;
                case 14: r = a & ~b; logical = true; break;
                case 15: r = ~b; logical = true; break;
                }
                if (Sb && logical) { c->n = r >> 31; c->z = r == 0; c->cf = carry; }
                if (write) {
                    if (rd == 15) {
                        if (Sb) bad(c, pc, insn, "flag-setting write to pc");
                        pc = r; goto indirect;
                    }
                    c->r[rd] = r;
                }
                pc = npc; continue;
            }
            case 2: case 3: {
                if (op1 == 3 && (insn & 0x10)) {
                    // media instructions
                    uint32_t rd = (insn >> 12) & 15, rn = (insn >> 16) & 15, rm = insn & 15;
                    if ((insn & 0x0f8000f0) == 0x06800070 || (insn & 0x0fb000f0) == 0x06a00070 || (insn & 0x0fb000f0) == 0x06e00070
                        || (insn & 0x0fb00070) == 0x06b00070) {
                        // sxtb/sxth/uxtb/uxth (+ add forms), rotate
                        uint32_t op = (insn >> 20) & 7, rot = ((insn >> 10) & 3) * 8;
                        uint32_t v = ror32(c->r[rm], rot), r;
                        switch (op) {
                        case 2: r = (uint32_t)(int32_t)(int8_t)v; break;     // sxtb
                        case 3: r = (uint32_t)(int32_t)(int16_t)v; break;    // sxth
                        case 6: r = v & 0xff; break;                         // uxtb
                        case 7: r = v & 0xffff; break;                       // uxth
                        default: bad(c, pc, insn, "extend");
                        }
                        if (rn != 15) r += c->r[rn];
                        c->r[rd] = r; pc = npc; continue;
                    }
                    if ((insn & 0x0fe00070) == 0x07e00050 || (insn & 0x0fe00070) == 0x07a00050) {     // ubfx / sbfx
                        uint32_t lsb = (insn >> 7) & 31, w = ((insn >> 16) & 31) + 1;
                        uint32_t v = c->r[rm];
                        if (insn & (1u << 22)) c->r[rd] = (v >> lsb) & (w == 32 ? 0xffffffffu : ((1u << w) - 1));
                        else c->r[rd] = (uint32_t)(((int32_t)(v << (32 - lsb - w))) >> (32 - w));
                        pc = npc; continue;
                    }
                    if ((insn & 0x0fe00070) == 0x07c00010) {                                         // bfi / bfc
                        uint32_t lsb = (insn >> 7) & 31, msb = (insn >> 16) & 31;
                        uint32_t w = msb - lsb + 1, mask = (w == 32 ? 0xffffffffu : ((1u << w) - 1)) << lsb;
                        uint32_t src = rm == 15 ? 0 : c->r[rm];
                        c->r[rd] = (c->r[rd] & ~mask) | ((src << lsb) & mask); pc = npc; continue;
                    }
                    if ((insn & 0x0ff00ff0) == 0x06b00f30) { c->r[rd] = __builtin_bswap32(c->r[rm]); pc = npc; continue; }     // rev
                    if ((insn & 0x0ff00ff0) == 0x06b00fb0) { uint32_t v = c->r[rm]; c->r[rd] = ((v >> 8) & 0x00ff00ff) | ((v << 8) & 0xff00ff00); pc = npc; continue; }  // rev16
                    if ((insn & 0x0ff00ff0) == 0x06f00fb0) { c->r[rd] = (uint32_t)(int16_t)__builtin_bswap16((uint16_t)c->r[rm]); pc = npc; continue; }  // revsh
                    if ((insn & 0x0ff0f0f0) == 0x0710f010 || (insn & 0x0ff0f0f0) == 0x0730f010) {     // sdiv / udiv
                        uint32_t d = (insn >> 16) & 15, m = (insn >> 8) & 15, n = insn & 15;
                        if (insn & (1u << 21)) c->r[d] = c->r[m] ? c->r[n] / c->r[m] : 0;
                        else c->r[d] = c->r[m] ? (c->r[n] == 0x80000000u && c->r[m] == 0xffffffffu ? 0x80000000u
                                                  : (uint32_t)((int32_t)c->r[n] / (int32_t)c->r[m])) : 0;
                        pc = npc; continue;
                    }
                    if ((insn & 0x0fe00030) == 0x06e00010 || (insn & 0x0fe00030) == 0x06a00010) {     // usat / ssat
                        uint32_t sat = (insn >> 16) & 31, shimm = (insn >> 7) & 31;
                        uint32_t v = (insn & 0x40) ? (uint32_t)((int32_t)c->r[rm] >> (shimm ? shimm : 31)) : c->r[rm] << shimm;
                        c->r[rd] = (insn & (1u << 22)) ? usat(v, (int)sat) : ssat(v, (int)sat + 1);
                        pc = npc; continue;
                    }
                    if ((insn & 0x0ffffff0) == 0x07f000f0 || (insn & 0x0ff000f0) == 0x07f000f0) bad(c, pc, insn, "udf");
                    bad(c, pc, insn, "media");
                }
                // ldr/str/ldrb/strb
                bool P = insn & (1u << 24), U = insn & (1u << 23), B = insn & (1u << 22), W = insn & (1u << 21),
                     L = insn & (1u << 20);
                uint32_t rn = (insn >> 16) & 15, rt = (insn >> 12) & 15;
                uint32_t off = op1 == 2 ? (insn & 0xfff) : addr_offset_reg(c, insn, pc);
                uint32_t base = rn == 15 ? pc + 8 : c->r[rn];
                uint32_t off_a = U ? base + off : base - off;
                uint32_t a = P ? off_a : base;
                if (L) {
                    uint32_t v = B ? ld8(a) : ld32(a);
                    if (!P || W) c->r[rn] = off_a;
                    if (rt == 15) { pc = v; goto indirect; }
                    c->r[rt] = v;
                } else {
                    uint32_t v = rt == 15 ? pc + 8 : c->r[rt];
                    if (B) st8(a, v); else st32(a, v);
                    if (!P || W) c->r[rn] = off_a;
                }
                pc = npc; continue;
            }
            case 4: {
                // ldm / stm
                bool P = insn & (1u << 24), U = insn & (1u << 23), W = insn & (1u << 21), L = insn & (1u << 20);
                if (insn & (1u << 22)) bad(c, pc, insn, "ldm/stm user registers");
                uint32_t rn = (insn >> 16) & 15, list = insn & 0xffff;
                uint32_t n = (uint32_t)__builtin_popcount(list);
                uint32_t base = c->r[rn];
                uint32_t start = U ? (P ? base + 4 : base) : (P ? base - 4 * n : base - 4 * n + 4);
                uint32_t wb = U ? base + 4 * n : base - 4 * n;
                uint32_t a = start; uint32_t newpc = 0; bool topc = false;
                if (L) {
                    uint32_t vals[16];
                    for (int k = 0; k < 16; k++) if (list & (1u << k)) { vals[k] = ld32(a); a += 4; }
                    if (W) c->r[rn] = wb;
                    for (int k = 0; k < 15; k++) if (list & (1u << k)) c->r[k] = vals[k];
                    if (list & 0x8000) { newpc = vals[15]; topc = true; }
                } else {
                    for (int k = 0; k < 16; k++) if (list & (1u << k)) { st32(a, k == 15 ? pc + 8 : c->r[k]); a += 4; }
                    if (W) c->r[rn] = wb;
                }
                if (topc) { pc = newpc; goto indirect; }
                pc = npc; continue;
            }
            case 5: {
                // b / bl
                int32_t off = (int32_t)(insn << 8) >> 6;
                uint32_t t = pc + 8 + off;
                if (insn & (1u << 24)) {
                    c->r[14] = npc;
                    if (host_call(c, t, npc)) { pc = npc; continue; }
                    pc = t; continue;
                }
                if (is_static_code(t)) { uint32_t ret = c->r[14]; if (host_call(c, t, ret)) { pc = ret; goto indirect; } }
                pc = t; continue;
            }
            case 6: vfp_ldst(c, insn, pc); pc = npc; continue;
            case 7:
                if (insn & (1u << 24)) { c->r[15] = pc; guest_svc(c); pc = npc; continue; }
                if (((insn >> 8) & 0xe) != 0xa) bad(c, pc, insn, "coprocessor");
                if (insn & 0x10) vfp_xfer(c, insn, pc); else vfp_data(c, insn, pc);
                pc = npc; continue;
            }
            bad(c, pc, insn, "decode");
        indirect:
            if (on_indirect(c, pc, act)) return;
            {   // a jump (not a call) to a host-implemented function: tail call
                GuestFnPtr f = host_fn(pc);
                if (f) {
                    uint32_t ret = c->r[14];
                    f(c);
                    pc = ret;
                    if (on_indirect(c, pc, act)) return;
                }
            }
        }
        } catch (Fault &f) {
            c->r[15] = pc;
            deliver_fault(c, pc, f.addr);       // the guest handler may redirect execution
            pc = c->r[15];
            if (on_indirect(c, pc, act)) return;
            if (GuestFnPtr hf = host_fn(pc)) {
                uint32_t ret = c->r[14];
                hf(c);
                pc = ret;
                if (on_indirect(c, pc, act)) return;
            }
        }
    }
}

// ---------------------------------------------------------------- VFP
static void vfp_ldst(CPU *c, uint32_t insn, uint32_t pc) {
    uint32_t cp = (insn >> 8) & 0xf;
    if (cp != 10 && cp != 11) bad(c, pc, insn, "coprocessor load/store");
    bool dbl = cp == 11;
    if ((insn & 0x0fe00000) == 0x0c400000) {                       // vmov r,r <-> d / s,s
        uint32_t rt = (insn >> 12) & 15, rt2 = (insn >> 16) & 15, M = (insn >> 5) & 1, vm = insn & 15;
        bool to_core = insn & (1u << 20);
        if (dbl) {
            uint32_t dm = (M << 4) | vm;
            if (to_core) { c->r[rt] = c->su[2 * dm]; c->r[rt2] = c->su[2 * dm + 1]; }
            else { c->su[2 * dm] = c->r[rt]; c->su[2 * dm + 1] = c->r[rt2]; }
        } else {
            uint32_t sm = (vm << 1) | M;
            if (to_core) { c->r[rt] = c->su[sm]; c->r[rt2] = c->su[sm + 1]; }
            else { c->su[sm] = c->r[rt]; c->su[sm + 1] = c->r[rt2]; }
        }
        return;
    }
    bool P = insn & (1u << 24), U = insn & (1u << 23), W = insn & (1u << 21), L = insn & (1u << 20);
    uint32_t rn = (insn >> 16) & 15, D = (insn >> 22) & 1, vd = (insn >> 12) & 15;
    uint32_t reg = dbl ? (D << 4) | vd : (vd << 1) | D;
    uint32_t base = rn == 15 ? (pc + 8) & ~3u : c->r[rn];
    if ((insn & 0x0e000000) == 0x0c000000 && !(P && !W) ) {
        // vldm/vstm (incl. vpush/vpop)
        uint32_t imm8 = insn & 0xff;
        uint32_t n = dbl ? imm8 / 2 : imm8;
        uint32_t a = U ? base : base - imm8 * 4;
        for (uint32_t k = 0; k < n; k++) {
            if (dbl) {
                if (L) { uint32_t lo = ld32(a), hi = ld32(a + 4); c->su[2 * (reg + k)] = lo; c->su[2 * (reg + k) + 1] = hi; }
                else { st32(a, c->su[2 * (reg + k)]); st32(a + 4, c->su[2 * (reg + k) + 1]); }
                a += 8;
            } else {
                if (L) c->su[reg + k] = ld32(a); else st32(a, c->su[reg + k]);
                a += 4;
            }
        }
        if (W) c->r[rn] = U ? base + imm8 * 4 : base - imm8 * 4;
        return;
    }
    // vldr / vstr
    uint32_t off = (insn & 0xff) * 4;
    uint32_t a = U ? base + off : base - off;
    if (dbl) {
        if (L) { uint32_t lo = ld32(a), hi = ld32(a + 4); c->su[2 * reg] = lo; c->su[2 * reg + 1] = hi; }
        else { st32(a, c->su[2 * reg]); st32(a + 4, c->su[2 * reg + 1]); }
    } else {
        if (L) c->su[reg] = ld32(a); else st32(a, c->su[reg]);
    }
}

static void vfp_xfer(CPU *c, uint32_t insn, uint32_t pc) {
    uint32_t rt = (insn >> 12) & 15;
    if ((insn & 0x0fe00f10) == 0x0e000a10) {                       // vmov s <-> r
        uint32_t sn = (((insn >> 16) & 15) << 1) | ((insn >> 7) & 1);
        if (insn & (1u << 20)) c->r[rt] = c->su[sn]; else c->su[sn] = c->r[rt];
        return;
    }
    if ((insn & 0x0fff0f10) == 0x0ef10a10) {                       // vmrs
        if (rt == 15) { uint32_t f = c->fpscr; c->n = f >> 31; c->z = (f >> 30) & 1; c->cf = (f >> 29) & 1; c->v = (f >> 28) & 1; }
        else c->r[rt] = c->fpscr;
        return;
    }
    if ((insn & 0x0fff0f10) == 0x0ee10a10) { c->fpscr = c->r[rt]; return; }       // vmsr
    if ((insn & 0x0f900f10) == 0x0e000b10 || (insn & 0x0f900f10) == 0x0e100b10) {   // vmov d[x] <-> r (32-bit lane)
        uint32_t dn = (((insn >> 7) & 1) << 4) | ((insn >> 16) & 15), x = (insn >> 21) & 1;
        if (insn & (1u << 20)) c->r[rt] = c->su[2 * dn + x]; else c->su[2 * dn + x] = c->r[rt];
        return;
    }
    bad(c, pc, insn, "vfp transfer");
}

static void vfp_data(CPU *c, uint32_t insn, uint32_t pc) {
    bool dbl = ((insn >> 8) & 0xf) == 11;
    uint32_t D = (insn >> 22) & 1, N = (insn >> 7) & 1, M = (insn >> 5) & 1;
    uint32_t vd = (insn >> 12) & 15, vn = (insn >> 16) & 15, vm = insn & 15;
    uint32_t d = dbl ? (D << 4) | vd : (vd << 1) | D;
    uint32_t n = dbl ? (N << 4) | vn : (vn << 1) | N;
    uint32_t m = dbl ? (M << 4) | vm : (vm << 1) | M;
    uint32_t opc1 = (insn >> 20) & 0xb, opc3 = (insn >> 6) & 1;   // bits 23,21,20 and bit 6
    #define DOP(expr_s, expr_d) do { if (dbl) c->d[d] = dnan(expr_d); else c->s[d] = fnan(expr_s); } while (0)
    switch (opc1) {
    case 0x0: if (!opc3) DOP(c->s[d] + c->s[n] * c->s[m], c->d[d] + c->d[n] * c->d[m]);           // vmla
              else DOP(c->s[d] - c->s[n] * c->s[m], c->d[d] - c->d[n] * c->d[m]); return;          // vmls
    case 0x1: if (!opc3) DOP(-c->s[d] + c->s[n] * c->s[m], -c->d[d] + c->d[n] * c->d[m]);         // vnmls
              else DOP(-c->s[d] - c->s[n] * c->s[m], -c->d[d] - c->d[n] * c->d[m]); return;        // vnmla
    case 0x2: if (!opc3) DOP(c->s[n] * c->s[m], c->d[n] * c->d[m]);                                // vmul
              else DOP(-(c->s[n] * c->s[m]), -(c->d[n] * c->d[m])); return;                        // vnmul
    case 0x3: if (!opc3) DOP(c->s[n] + c->s[m], c->d[n] + c->d[m]);                                // vadd
              else DOP(c->s[n] - c->s[m], c->d[n] - c->d[m]); return;                              // vsub
    case 0x8: if (!opc3) { DOP(c->s[n] / c->s[m], c->d[n] / c->d[m]); return; } break;             // vdiv
    case 0xa: if (!opc3) DOP(c->s[d] + c->s[n] * c->s[m], c->d[d] + c->d[n] * c->d[m]);           // vfma
              else DOP(c->s[d] - c->s[n] * c->s[m], c->d[d] - c->d[n] * c->d[m]); return;          // vfms
    case 0xb: {
        if (!opc3) {                                                   // vmov immediate
            uint32_t imm8 = ((insn >> 12) & 0xf0) | (insn & 0xf);
            if (dbl) {
                uint64_t sgn = (uint64_t)(imm8 >> 7) << 63, b6 = (imm8 >> 6) & 1;
                uint64_t e = ((uint64_t)(!b6) << 10) | ((b6 ? 0xffull : 0) << 2) | ((imm8 >> 4) & 3);   // NOT(b6):b6*8:imm[5:4]
                uint64_t v = sgn | (e << 52) | ((uint64_t)(imm8 & 0xf) << 48);
                c->d[d] = fromd(v);
            } else {
                uint32_t b6 = (imm8 >> 6) & 1;
                uint32_t e = ((uint32_t)(!b6) << 7) | ((b6 ? 0x1fu : 0) << 2) | ((imm8 >> 4) & 3);
                uint32_t v = ((imm8 >> 7) << 31) | (e << 23) | ((imm8 & 0xf) << 19);
                c->su[d] = v;
            }
            return;
        }
        uint32_t opc2 = (insn >> 16) & 0xf, op7 = (insn >> 7) & 1;
        switch (opc2) {
        case 0x0: if (!op7) { if (dbl) c->d[d] = c->d[m]; else c->s[d] = c->s[m]; }               // vmov reg
                  else { if (dbl) c->d[d] = fabs(c->d[m]); else c->s[d] = fabsf(c->s[m]); } return;   // vabs
        case 0x1: if (!op7) { if (dbl) c->d[d] = -c->d[m]; else c->s[d] = -c->s[m]; }             // vneg
                  else { if (dbl) c->d[d] = dnan(sqrt(c->d[m])); else c->s[d] = fnan(sqrtf(c->s[m])); } return;   // vsqrt
        case 0x4: case 0x5: {                                                                     // vcmp(e)
            bool zero = opc2 == 5;
            if (dbl) fcmp(c, c->d[d], zero ? 0.0 : c->d[m]);
            else fcmp(c, c->s[d], zero ? 0.0 : c->s[m]);
            return;
        }
        case 0x7: if (op7) {                                                                      // vcvt f32 <-> f64
            if (dbl) { uint32_t sd = (vd << 1) | D; c->s[sd] = (float)c->d[m]; }
            else { uint32_t dd = (D << 4) | vd; c->d[dd] = (double)c->s[m]; }
            return; } break;
        case 0x8: {                                                                               // vcvt int -> float
            uint32_t sm = (vm << 1) | M; int32_t iv = (int32_t)c->su[sm];
            double v = op7 ? (double)iv : (double)(uint32_t)c->su[sm];
            if (dbl) c->d[d] = v; else c->s[d] = (float)v;
            return;
        }
        case 0xc: case 0xd: {                                                                     // vcvt float -> int
            uint32_t sd = (vd << 1) | D;
            double v = dbl ? c->d[m] : (double)c->s[m];
            bool rz = op7;                       // op7=1: round toward zero; 0: FPSCR rounding (nearest)
            uint32_t r;
            if (opc2 == 0xd) r = rz ? f2s(v) : f2sr(v);
            else r = rz ? f2u(v) : f2ur(v);
            c->su[sd] = r;
            return;
        }
        }
        break;
    }
    }
    #undef DOP
    bad(c, pc, insn, "vfp data");
}

}  // namespace interp
