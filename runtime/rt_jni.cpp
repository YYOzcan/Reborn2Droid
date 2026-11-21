// Guest-facing JNI: JNIEnv / JavaVM function tables whose entries are runtime traps.
#include "jvm.h"
#include "thread.h"
#include "profile.h"
#include <mutex>
#include <unordered_map>
#include <cstring>

using namespace rt;
namespace jni {

// ================================================================ registry
static std::map<std::string, JClass *> &classes() { static std::map<std::string, JClass *> m; return m; }
static std::recursive_mutex mu;

Obj::~Obj() { if (arr_data) gfree(arr_data); delete fields; }

static JClass *class_class;
JClass *find_class(const std::string &name) {
    std::lock_guard<std::recursive_mutex> g(mu);
    auto it = classes().find(name);
    return it == classes().end() ? nullptr : it->second;
}
JClass *def_class(const std::string &name, const std::string &super, bool lenient) {
    std::lock_guard<std::recursive_mutex> g(mu);
    JClass *&k = classes()[name];
    if (!k) { k = new JClass(); k->name = name; }
    if (name != "java/lang/Object" && !super.empty()) k->super = def_class(super, name == "java/lang/Object" ? "" : "java/lang/Object");
    k->lenient = k->lenient || lenient;
    if (!k->class_obj) {
        k->class_obj = std::make_shared<Obj>();
        k->class_obj->as_class = k;
        if (class_class) k->class_obj->cls = class_class;
    }
    return k;
}
void def_method(JClass *k, const std::string &name, const std::string &sig, Impl impl, bool is_static) {
    std::lock_guard<std::recursive_mutex> g(mu);
    JMethod *m = new JMethod{k, name, sig, is_static, std::move(impl)};
    k->methods[name + sig] = m;
}
void def_static(JClass *k, const std::string &name, const std::string &sig, Impl impl) { def_method(k, name, sig, std::move(impl), true); }
JField *def_field(JClass *k, const std::string &name, const std::string &sig, bool is_static, JV v) {
    std::lock_guard<std::recursive_mutex> g(mu);
    JField *f = new JField{k, name, sig, is_static, v};
    k->fields[name] = f;
    return f;
}
static JMethod *lookup_method(JClass *k, const std::string &name, const std::string &sig, bool is_static, bool create_stub) {
    std::lock_guard<std::recursive_mutex> g(mu);
    for (JClass *c = k; c; c = c->super) {
        auto it = c->methods.find(name + sig);
        if (it != c->methods.end()) return it->second;
    }
    // natives registered by the guest are methods too
    for (JClass *c = k; c; c = c->super) {
        auto it = c->natives.find(name + sig);
        if (it != c->natives.end()) {
            JMethod *m = new JMethod{c, name, sig, is_static, nullptr};
            c->methods[name + sig] = m;
            return m;
        }
    }
    if (!create_stub) return nullptr;
    bool lenient = false;
    for (JClass *c = k; c; c = c->super) lenient |= c->lenient;
    if (!lenient) return nullptr;
    JMethod *m = new JMethod{k, name, sig, is_static, nullptr, true};
    k->methods[name + sig] = m;
    return m;
}
static JField *lookup_field(JClass *k, const std::string &name) {
    std::lock_guard<std::recursive_mutex> g(mu);
    for (JClass *c = k; c; c = c->super) {
        auto it = c->fields.find(name);
        if (it != c->fields.end()) return it->second;
    }
    return nullptr;
}

// ================================================================ objects
Ref new_obj(JClass *k) { Ref o = std::make_shared<Obj>(); o->cls = k; return o; }
Ref jstr(const std::string &s) { static JClass *k = def_class("java/lang/String"); Ref o = new_obj(k); o->str = s; return o; }
static uint32_t elem_size(char t) { return t == 'Z' || t == 'B' ? 1 : t == 'C' || t == 'S' ? 2 : t == 'J' || t == 'D' ? 8 : 4; }
Ref new_prim_array(char type, uint32_t n) {
    std::string name = std::string("[") + type;
    Ref o = new_obj(def_class(name));
    o->arr_type = type; o->arr_len = n;
    o->arr_data = gmalloc(n * elem_size(type) + 8);
    memset(GPTR(o->arr_data), 0, n * elem_size(type) + 8);
    return o;
}
Ref new_obj_array(JClass *elem, uint32_t n) {
    Ref o = new_obj(def_class("[L" + (elem ? elem->name : std::string("java/lang/Object")) + ";"));
    o->arr_type = 'L'; o->arr_len = n; o->objs.resize(n);
    return o;
}
Ref new_direct_buffer(uint32_t addr, uint32_t cap) {
    Ref o = new_obj(def_class("java/nio/ByteBuffer"));
    o->buf_addr = addr; o->buf_cap = cap;
    return o;
}
JV get_field(Obj *o, const std::string &name) {
    if (!o || !o->fields) return JV();
    auto it = o->fields->find(name);
    return it == o->fields->end() ? JV() : it->second;
}
void set_field(Obj *o, const std::string &name, JV v) {
    if (!o) return;
    if (!o->fields) o->fields = new std::map<std::string, JV>();
    (*o->fields)[name] = std::move(v);
}

// ================================================================ handles
// handle = 0x40000000 | index << 8 | generation: a stale DeleteLocalRef of a recycled slot is ignored
struct Slot { Ref o; uint8_t gen = 0; };
static std::vector<Slot> refs(1);
static std::vector<uint32_t> free_refs;
struct Frame { std::vector<uint32_t> locals; };
static thread_local std::vector<Frame> *frames;
static thread_local Ref pending_exc;

static uint32_t add_ref(const Ref &o, bool local) {
    if (!o) return 0;
    std::lock_guard<std::recursive_mutex> g(mu);
    uint32_t k;
    if (free_refs.size() > 4096) { k = free_refs.front(); free_refs.erase(free_refs.begin()); }   // FIFO reuse: slow recycling
    else { k = (uint32_t)refs.size(); refs.emplace_back(); }
    if (k >= (1u << 22)) fatal("JNI: too many references");
    refs[k].o = o; refs[k].gen++;
    uint32_t h = 0x40000000u | (k << 8) | refs[k].gen;
    if (local && frames && !frames->empty()) frames->back().locals.push_back(h);
    return h;
}
static void del_ref(uint32_t h) {
    if ((h & 0xc0000000u) != 0x40000000u) return;
    std::lock_guard<std::recursive_mutex> g(mu);
    uint32_t k = (h >> 8) & 0x3fffff;
    if (k < refs.size() && refs[k].o && refs[k].gen == (h & 0xff)) { refs[k].o.reset(); free_refs.push_back(k); }
}
uint32_t to_handle(const Ref &o) { return add_ref(o, true); }
Ref from_handle(uint32_t h) {
    if ((h & 0xc0000000u) != 0x40000000u) return nullptr;
    std::lock_guard<std::recursive_mutex> g(mu);
    uint32_t k = (h >> 8) & 0x3fffff;
    return k < refs.size() && refs[k].gen == (h & 0xff) ? refs[k].o : nullptr;
}
static void push_frame() { if (!frames) frames = new std::vector<Frame>(); frames->emplace_back(); }
static void pop_frame() {
    if (!frames || frames->empty()) return;
    for (uint32_t h : frames->back().locals) del_ref(h);
    frames->pop_back();
}
void throw_new(const std::string &cls, const std::string &msg) {
    Ref e = new_obj(def_class(cls, "java/lang/Throwable"));
    e->str = msg;
    pending_exc = e;
    if (verbose) logf("java exception %s: %s", cls.c_str(), msg.c_str());
}

// method / field ids: indices into tables (never freed)
static std::vector<JMethod *> mids(1);
static std::vector<JField *> fids(1);
static std::unordered_map<JMethod *, uint32_t> mid_of;
static std::unordered_map<JField *, uint32_t> fid_of;
static uint32_t method_id(JMethod *m) {
    if (!m) return 0;
    std::lock_guard<std::recursive_mutex> g(mu);
    auto it = mid_of.find(m); if (it != mid_of.end()) return it->second;
    mids.push_back(m); return mid_of[m] = 0x00200000u + 4 * (uint32_t)(mids.size() - 1);
}
static JMethod *method_of(uint32_t id) {
    std::lock_guard<std::recursive_mutex> g(mu);
    uint32_t k = (id - 0x00200000u) / 4; return k < mids.size() ? mids[k] : nullptr;
}
static uint32_t field_id(JField *f) {
    if (!f) return 0;
    std::lock_guard<std::recursive_mutex> g(mu);
    auto it = fid_of.find(f); if (it != fid_of.end()) return it->second;
    fids.push_back(f); return fid_of[f] = 0x00300000u + 4 * (uint32_t)(fids.size() - 1);
}
static JField *field_of(uint32_t id) {
    std::lock_guard<std::recursive_mutex> g(mu);
    uint32_t k = (id - 0x00300000u) / 4; return k < fids.size() ? fids[k] : nullptr;
}

// ================================================================ signatures / arguments
static std::vector<char> sig_args(const std::string &sig) {
    std::vector<char> r;
    for (size_t p = 1; p < sig.size() && sig[p] != ')'; p++) {
        char ch = sig[p];
        if (ch == 'L') { while (sig[p] != ';') p++; r.push_back('L'); }
        else if (ch == '[') { while (sig[p] == '[') p++; if (sig[p] == 'L') while (sig[p] != ';') p++; r.push_back('L'); }
        else r.push_back(ch);
    }
    return r;
}
static char sig_ret(const std::string &sig) { size_t p = sig.find(')'); return p == std::string::npos ? 'V' : sig[p + 1] == '[' ? 'L' : sig[p + 1]; }

enum Mode { VARARGS, VALIST, JVALUES };
static Args read_args(CPU *c, const std::string &sig, Mode mode, int first, uint32_t ptr) {
    Args a; std::vector<char> t = sig_args(sig);
    VA va{c, first, ptr, mode == VALIST};
    for (size_t k = 0; k < t.size(); k++) {
        char ch = t[k];
        JV v;
        if (mode == JVALUES) {
            uint32_t e = ptr + 8 * (uint32_t)k;
            if (ch == 'J' || ch == 'D') v.v = (uint64_t)R32(e) | ((uint64_t)R32(e + 4) << 32);
            else if (ch == 'L') v.o = from_handle(R32(e));
            else if (ch == 'Z' || ch == 'B') v.v = R8(e);
            else if (ch == 'C' || ch == 'S') v.v = R16(e);
            else v.v = R32(e);
        } else {
            if (ch == 'J' || ch == 'D') v.v = va.next64();
            else if (ch == 'F') { double d; uint64_t u = va.next64(); memcpy(&d, &u, 8); v = JV::F((float)d); }   // promoted
            else if (ch == 'L') v.o = from_handle(va.next32());
            else v.v = va.next32();
        }
        a.push_back(v);
    }
    return a;
}
static void set_ret(CPU *c, char rt_, const JV &v) {
    switch (rt_) {
    case 'V': return;
    case 'J': case 'D': ret64(c, v.v); return;
    case 'L': c->r[0] = to_handle(v.o); return;
    case 'Z': c->r[0] = v.z() ? 1 : 0; return;
    case 'B': c->r[0] = (uint32_t)(int32_t)(int8_t)v.v; return;
    case 'S': c->r[0] = (uint32_t)(int32_t)(int16_t)v.v; return;
    case 'C': c->r[0] = (uint16_t)v.v; return;
    default: c->r[0] = (uint32_t)v.v; return;
    }
}

// ================================================================ invoking methods (Java or guest native)
static uint32_t g_env, g_vm;
static JV invoke(JMethod *m, Obj *self, Args &a, JClass *k) {
    if (!m) return JV();
    if (m->impl) return m->impl(self, a);
    // a guest native?
    for (JClass *c = k ? k : m->cls; c; c = c->super) {
        auto it = c->natives.find(m->name + m->sig);
        if (it != c->natives.end()) return call_native(c, self, m->name, m->sig, a);
    }
    if (m->stub) {
        static std::map<std::string, int> seen;
        std::string key = m->cls->name + "." + m->name + m->sig;
        if (!seen[key]++) logf("JNI: unimplemented Java method %s", key.c_str());
        char r = sig_ret(m->sig);
        if (r == 'L' && m->sig.find(")Ljava/lang/String;") != std::string::npos) return JV::O(jstr(""));
        return JV();
    }
    logf("JNI: method without implementation %s.%s%s", m->cls->name.c_str(), m->name.c_str(), m->sig.c_str());
    return JV();
}

bool has_native(JClass *k, const std::string &name, const std::string &sig) {
    std::lock_guard<std::recursive_mutex> g(mu);
    for (JClass *c = k; c; c = c->super) if (c->natives.count(name + sig)) return true;
    // exported Java_<class>_<method>
    std::string sym = "Java_";
    for (char ch : k->name) sym += ch == '/' ? '_' : ch;
    sym += "_" + name;
    return sym_addr(sym.c_str()) != 0;
}

JV call_native(JClass *k, Obj *self, const std::string &name, const std::string &sig, const Args &a) {
    uint32_t fn = 0;
    {
        std::lock_guard<std::recursive_mutex> g(mu);
        for (JClass *c = k; c && !fn; c = c->super) { auto it = c->natives.find(name + sig); if (it != c->natives.end()) fn = it->second; }
    }
    if (!fn) {
        std::string sym = "Java_";
        for (char ch : k->name) sym += ch == '/' ? '_' : ch == '_' ? '_' : ch;
        sym += "_" + name;
        fn = sym_addr(sym.c_str());
    }
    if (!fn) { logf("JNI: native %s.%s%s not registered", k->name.c_str(), name.c_str(), sig.c_str()); return JV(); }
    CPU *c = cpu();
    push_frame();
    // AAPCS softfp: core registers r0-r3, then the stack; 64-bit values in even register pairs / 8-aligned slots
    uint32_t regs[4] = {0}; int ncrn = 0; std::vector<uint32_t> stack;
    auto put32 = [&](uint32_t v) { if (ncrn < 4) regs[ncrn++] = v; else stack.push_back(v); };
    auto put64 = [&](uint64_t v) {
        if (ncrn < 3) { if (ncrn & 1) ncrn++; regs[ncrn++] = (uint32_t)v; regs[ncrn++] = (uint32_t)(v >> 32); }
        else { ncrn = 4; if (stack.size() & 1) stack.push_back(0); stack.push_back((uint32_t)v); stack.push_back((uint32_t)(v >> 32)); }
    };
    put32(g_env);
    put32(self ? to_handle(self->shared_from_this()) : to_handle(k->class_obj));
    std::vector<char> t = sig_args(sig);
    for (size_t i = 0; i < t.size() && i < a.size(); i++) {
        char ch = t[i];
        if (ch == 'J' || ch == 'D') put64(a[i].v);
        else if (ch == 'L') put32(to_handle(a[i].o));
        else put32((uint32_t)a[i].v);
    }
    // the self handle above must refer to the real shared object when one exists
    uint32_t saved_sp = c->r[13], saved_lr = c->r[14];
    uint32_t sp = (c->r[13] - 16 - 4 * (uint32_t)stack.size()) & ~7u;
    for (size_t i = 0; i < stack.size(); i++) W32(sp + 4 * (uint32_t)i, stack[i]);
    c->r[13] = sp;
    for (int i = 0; i < 4; i++) c->r[i] = regs[i];
    c->r[14] = host_return_addr();
    call_addr(c, fn);
    JV r;
    char rt_ = sig_ret(sig);
    if (rt_ == 'J' || rt_ == 'D') r.v = (uint64_t)c->r[0] | ((uint64_t)c->r[1] << 32);
    else if (rt_ == 'L') r.o = from_handle(c->r[0]);
    else if (rt_ == 'Z') r.v = c->r[0] & 0xff;
    else r.v = c->r[0];
    c->r[13] = saved_sp; c->r[14] = saved_lr;
    pop_frame();
    if (pending_exc) { if (verbose) logf("JNI: exception left pending by native %s: %s", name.c_str(), pending_exc->str.c_str()); pending_exc.reset(); }
    return r;
}

// ================================================================ JNIEnv functions
#define A(i) arg(c, i)
#define RET(v) (c->r[0] = (uint32_t)(v))
using Fn = void (*)(CPU *);
static Fn table[233];
static const char *table_names[233];

static Ref O(uint32_t h) { return from_handle(h); }
static JClass *K(uint32_t h) { Ref o = from_handle(h); return o ? (o->as_class ? o->as_class : nullptr) : nullptr; }
static std::string cls_name(const std::string &n) {   // "java.lang.String" -> "java/lang/String"
    std::string r = n; for (char &ch : r) if (ch == '.') ch = '/'; return r;
}

static bool lenient_namespace(const std::string &n) {
    static std::string app = [] { std::string a = RB_PACKAGE; for (char &ch : a) if (ch == '.') ch = '/'; return a + "/"; }();
    static const char *plugins[] = {"com/prime31/", "net/gree/", "com/facebook/", "com/google/android/gms/", "com/google/ads/",
                                    "com/flurry/", "com/chartboost/", "com/tapjoy/", "com/playhaven/", "com/admob/", "com/millennialmedia/",
                                    "com/inmobi/", "com/revmob/", "com/heyzap/", "com/vungle/", "com/applovin/", "com/bda/", "com/openfeint/",
                                    "com/scoreloop/", "com/swarmconnect/", "com/papaya/", "com/unity3d/Plugin/", "com/android/vending/"};
    if (n.rfind(app, 0) == 0) return true;
    // the developer's own shared Java code next to the app package (com/vendor/framework/...)
    static std::string vendor = [] { std::string a = app; size_t k = a.find('/'); k = k == std::string::npos ? k : a.find('/', k + 1);
                                     std::string v = k == std::string::npos ? "" : a.substr(0, k + 1);
                                     return (v == "com/google/" || v == "com/android/" || v == "com/unity3d/" || v == "android/" || v == "java/") ? "" : v; }();
    if (!vendor.empty() && n.rfind(vendor, 0) == 0) return true;
    for (const char *p : plugins) if (n.rfind(p, 0) == 0) return true;
    return false;
}
JClass *stub_class(const std::string &n) {        // the game's own Java classes and plugin SDKs: logged stubs
    if (!lenient_namespace(n)) return nullptr;
    JClass *k = def_class(n, "java/lang/Object", true);
    logf("JNI: class %s is provided as a stub", n.c_str());
    return k;
}
static void j_GetVersion(CPU *c) { RET(0x00010006); }
static void j_FindClass(CPU *c) {
    std::string n = cls_name(GSTR(A(1)));
    JClass *k = find_class(n);
    if (!k && n[0] == '[') k = def_class(n);
    if (!k) k = stub_class(n);
    if (!k) { throw_new("java/lang/NoClassDefFoundError", n); if (verbose) logf("JNI FindClass(%s): not found", n.c_str()); RET(0); return; }
    if (verbose > 1) logf("JNI FindClass(%s)", n.c_str());
    RET(to_handle(k->class_obj));
}
static void j_GetSuperclass(CPU *c) { JClass *k = K(A(1)); RET(k && k->super ? to_handle(k->super->class_obj) : 0); }
static bool is_a(JClass *k, JClass *target) { for (; k; k = k->super) if (k == target) return true; return target && target->name == "java/lang/Object"; }
static void j_IsAssignableFrom(CPU *c) { RET(is_a(K(A(1)), K(A(2)))); }
static void j_Throw(CPU *c) { pending_exc = O(A(1)); RET(0); }
static void j_ThrowNew(CPU *c) { JClass *k = K(A(1)); throw_new(k ? k->name : "java/lang/Exception", A(2) ? GSTR(A(2)) : ""); RET(0); }
static void j_ExceptionOccurred(CPU *c) { RET(to_handle(pending_exc)); }
static void j_ExceptionDescribe(CPU *c) { if (pending_exc) logf("JNI exception: %s %s", pending_exc->cls ? pending_exc->cls->name.c_str() : "?", pending_exc->str.c_str()); (void)c; }
static void j_ExceptionClear(CPU *) { pending_exc.reset(); }
static void j_FatalError(CPU *c) { fatal("JNI FatalError: %s", GSTR(A(1))); }
static void j_PushLocalFrame(CPU *c) { push_frame(); RET(0); }
static void j_PopLocalFrame(CPU *c) { Ref keep = O(A(1)); pop_frame(); RET(to_handle(keep)); }
static void j_NewGlobalRef(CPU *c) { RET(add_ref(O(A(1)), false)); }
static void j_DeleteRef(CPU *c) { del_ref(A(1)); }
static void j_IsSameObject(CPU *c) { Ref a = O(A(1)), b = O(A(2)); RET(a.get() == b.get()); }
static void j_NewLocalRef(CPU *c) { RET(to_handle(O(A(1)))); }
static void j_EnsureLocalCapacity(CPU *c) { RET(0); }
static void j_AllocObject(CPU *c) { JClass *k = K(A(1)); RET(k ? to_handle(new_obj(k)) : 0); }
static void new_object(CPU *c, Mode mode) {
    JClass *k = K(A(1)); JMethod *m = method_of(A(2));
    if (!k || !m) { RET(0); return; }
    Ref o = new_obj(k);
    Args a = read_args(c, m->sig, mode, 3, mode == VARARGS ? 0 : A(3));
    if (m->impl) m->impl(o.get(), a);
    else if (!m->stub) invoke(m, o.get(), a, k);
    RET(to_handle(o));
}
static void j_NewObject(CPU *c) { new_object(c, VARARGS); }
static void j_NewObjectV(CPU *c) { new_object(c, VALIST); }
static void j_NewObjectA(CPU *c) { new_object(c, JVALUES); }
static void j_GetObjectClass(CPU *c) { Ref o = O(A(1)); RET(o && o->cls ? to_handle(o->cls->class_obj) : 0); }
static void j_IsInstanceOf(CPU *c) { Ref o = O(A(1)); RET(o ? is_a(o->cls, K(A(2))) : 1); }
static void get_method_id(CPU *c, bool is_static) {
    JClass *k = K(A(1)); std::string name = GSTR(A(2)), sig = GSTR(A(3));
    JMethod *m = k ? lookup_method(k, name, sig, is_static, true) : nullptr;
    if (!m) {
        throw_new("java/lang/NoSuchMethodError", (k ? k->name : std::string("?")) + "." + name + sig);
        if (verbose) logf("JNI GetMethodID %s.%s%s: not found", k ? k->name.c_str() : "?", name.c_str(), sig.c_str());
    } else if (verbose > 1) logf("JNI GetMethodID %s.%s%s", k->name.c_str(), name.c_str(), sig.c_str());
    RET(method_id(m));
}
static void j_GetMethodID(CPU *c) { get_method_id(c, false); }
static void j_GetStaticMethodID(CPU *c) { get_method_id(c, true); }

static void call_method(CPU *c, Mode mode, bool is_static, bool nonvirtual) {
    // (env, obj|cls, [cls,] mid, ...)
    int midx = nonvirtual ? 3 : 2;
    JMethod *m = method_of(A(midx));
    if (!m) { RET(0); return; }
    Args a = read_args(c, m->sig, mode, midx + 1, mode == VARARGS ? 0 : A(midx + 1));
    Ref self = is_static ? nullptr : O(A(1));
    JClass *k = is_static ? K(A(1)) : (self ? self->cls : nullptr);
    if (!is_static && !self && !m->is_static) {
        if (verbose) logf("JNI: %s.%s%s called on null", m->cls->name.c_str(), m->name.c_str(), m->sig.c_str());
        throw_new("java/lang/NullPointerException", m->name);
        set_ret(c, sig_ret(m->sig), JV());
        return;
    }
    if (verbose > 2) logf("JNI call %s.%s%s", m->cls->name.c_str(), m->name.c_str(), m->sig.c_str());
    // virtual dispatch: the object's own class may override
    if (!is_static && !nonvirtual && self && self->cls && self->cls != m->cls) {
        JMethod *o = lookup_method(self->cls, m->name, m->sig, false, false);
        if (o) m = o;
    }
    JV r = invoke(m, self.get(), a, k);
    set_ret(c, sig_ret(m->sig), r);
}
#define CALLS(T) \
    static void j_Call##T##Method(CPU *c) { call_method(c, VARARGS, false, false); } \
    static void j_Call##T##MethodV(CPU *c) { call_method(c, VALIST, false, false); } \
    static void j_Call##T##MethodA(CPU *c) { call_method(c, JVALUES, false, false); } \
    static void j_CallNonvirtual##T##Method(CPU *c) { call_method(c, VARARGS, false, true); } \
    static void j_CallNonvirtual##T##MethodV(CPU *c) { call_method(c, VALIST, false, true); } \
    static void j_CallNonvirtual##T##MethodA(CPU *c) { call_method(c, JVALUES, false, true); } \
    static void j_CallStatic##T##Method(CPU *c) { call_method(c, VARARGS, true, false); } \
    static void j_CallStatic##T##MethodV(CPU *c) { call_method(c, VALIST, true, false); } \
    static void j_CallStatic##T##MethodA(CPU *c) { call_method(c, JVALUES, true, false); }
CALLS(Object) CALLS(Boolean) CALLS(Byte) CALLS(Char) CALLS(Short) CALLS(Int) CALLS(Long) CALLS(Float) CALLS(Double) CALLS(Void)

static void get_field_id(CPU *c, bool is_static) {
    JClass *k = K(A(1)); std::string name = GSTR(A(2)), sig = GSTR(A(3));
    JField *f = k ? lookup_field(k, name) : nullptr;
    if (!f && k) {
        bool lenient = false; for (JClass *x = k; x; x = x->super) lenient |= x->lenient;
        if (lenient) { f = def_field(k, name, sig, is_static); logf("JNI: unknown field %s.%s %s (created)", k->name.c_str(), name.c_str(), sig.c_str()); }
    }
    if (!f) { throw_new("java/lang/NoSuchFieldError", name); if (verbose) logf("JNI GetFieldID %s.%s: not found", k ? k->name.c_str() : "?", name.c_str()); }
    RET(field_id(f));
}
static void j_GetFieldID(CPU *c) { get_field_id(c, false); }
static void j_GetStaticFieldID(CPU *c) { get_field_id(c, true); }
static void get_fieldv(CPU *c, bool is_static, char t) {
    JField *f = field_of(A(2)); if (!f) { RET(0); return; }
    JV v = is_static ? f->sval : get_field(O(A(1)).get(), f->name);
    set_ret(c, t, v);
}
static void set_fieldv(CPU *c, bool is_static, char t) {
    JField *f = field_of(A(2)); if (!f) return;
    JV v;
    if (t == 'L') v.o = O(A(3));
    else if (t == 'J' || t == 'D') { int i = 3; v.v = arg64(c, i); }
    else v.v = A(3);
    if (is_static) f->sval = v; else set_field(O(A(1)).get(), f->name, v);
}
#define FIELDS(T, ch) \
    static void j_Get##T##Field(CPU *c) { get_fieldv(c, false, ch); } \
    static void j_Set##T##Field(CPU *c) { set_fieldv(c, false, ch); } \
    static void j_GetStatic##T##Field(CPU *c) { get_fieldv(c, true, ch); } \
    static void j_SetStatic##T##Field(CPU *c) { set_fieldv(c, true, ch); }
FIELDS(Object, 'L') FIELDS(Boolean, 'Z') FIELDS(Byte, 'B') FIELDS(Char, 'C') FIELDS(Short, 'S') FIELDS(Int, 'I')
FIELDS(Long, 'J') FIELDS(Float, 'F') FIELDS(Double, 'D')

// ---- strings (UTF-8 inside, UTF-16 for the *Chars API)
static std::u16string to_u16(const std::string &s) {
    std::u16string r;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp; unsigned char ch = s[i];
        if (ch < 0x80) { cp = ch; i++; }
        else if ((ch >> 5) == 6 && i + 1 < s.size()) { cp = ((ch & 0x1f) << 6) | (s[i + 1] & 0x3f); i += 2; }
        else if ((ch >> 4) == 14 && i + 2 < s.size()) { cp = ((ch & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f); i += 3; }
        else if (i + 3 < s.size()) { cp = ((ch & 0x07) << 18) | ((s[i + 1] & 0x3f) << 12) | ((s[i + 2] & 0x3f) << 6) | (s[i + 3] & 0x3f); i += 4; }
        else { cp = '?'; i++; }
        if (cp >= 0x10000) { cp -= 0x10000; r += (char16_t)(0xD800 + (cp >> 10)); r += (char16_t)(0xDC00 + (cp & 0x3ff)); }
        else r += (char16_t)cp;
    }
    return r;
}
static std::string from_u16(const uint16_t *p, size_t n) {
    std::string r;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = p[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) { cp = 0x10000 + ((cp - 0xD800) << 10) + (p[i + 1] - 0xDC00); i++; }
        if (cp < 0x80) r += (char)cp;
        else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) { r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3f)); r += (char)(0x80 | (cp & 0x3f)); }
        else { r += (char)(0xF0 | (cp >> 18)); r += (char)(0x80 | ((cp >> 12) & 0x3f)); r += (char)(0x80 | ((cp >> 6) & 0x3f)); r += (char)(0x80 | (cp & 0x3f)); }
    }
    return r;
}
static void j_NewString(CPU *c) { std::vector<uint16_t> u(A(2)); memcpy(u.data(), GPTR(A(1)), 2 * u.size()); RET(to_handle(jstr(from_u16(u.data(), u.size())))); }
static void j_GetStringLength(CPU *c) { Ref o = O(A(1)); RET(o ? to_u16(o->str).size() : 0); }
static void j_GetStringChars(CPU *c) {
    Ref o = O(A(1)); std::u16string u = o ? to_u16(o->str) : std::u16string();
    uint32_t p = gmalloc(2 * u.size() + 2); memcpy(GPTR(p), u.data(), 2 * u.size()); W16(p + 2 * (uint32_t)u.size(), 0);
    if (A(2)) W8(A(2), 1);
    RET(p);
}
static void j_ReleaseStringChars(CPU *c) { gfree(A(2)); }
static void j_NewStringUTF(CPU *c) { RET(A(1) ? to_handle(jstr(GSTR(A(1)))) : 0); }
static void j_GetStringUTFLength(CPU *c) { Ref o = O(A(1)); RET(o ? o->str.size() : 0); }
static void j_GetStringUTFChars(CPU *c) { Ref o = O(A(1)); if (A(2)) W8(A(2), 1); RET(gstrdup(o ? o->str.c_str() : "")); }
static void j_ReleaseStringUTFChars(CPU *c) { gfree(A(2)); }
static void j_GetStringRegion(CPU *c) { Ref o = O(A(1)); std::u16string u = o ? to_u16(o->str) : std::u16string(); for (uint32_t k = 0; k < A(3) && A(2) + k < u.size(); k++) W16(A(4) + 2 * k, u[A(2) + k]); }
static void j_GetStringUTFRegion(CPU *c) {
    Ref o = O(A(1)); std::u16string u = o ? to_u16(o->str) : std::u16string();
    std::vector<uint16_t> part; for (uint32_t k = 0; k < A(3) && A(2) + k < u.size(); k++) part.push_back(u[A(2) + k]);
    std::string s = from_u16(part.data(), part.size()); memcpy(GPTR(A(4)), s.c_str(), s.size() + 1);
}

// ---- arrays
static void j_GetArrayLength(CPU *c) { Ref o = O(A(1)); RET(o ? o->arr_len : 0); }
static void j_NewObjectArray(CPU *c) {
    Ref a = new_obj_array(K(A(2)), A(1)); Ref init = O(A(3));
    for (auto &e : a->objs) e = init;
    RET(to_handle(a));
}
static void j_GetObjectArrayElement(CPU *c) { Ref a = O(A(1)); RET(a && A(2) < a->objs.size() ? to_handle(a->objs[A(2)]) : 0); }
static void j_SetObjectArrayElement(CPU *c) { Ref a = O(A(1)); if (a && A(2) < a->objs.size()) a->objs[A(2)] = O(A(3)); }
#define ARRAYS(T, ch) \
    static void j_New##T##Array(CPU *c) { RET(to_handle(new_prim_array(ch, A(1)))); } \
    static void j_Get##T##ArrayElements(CPU *c) { Ref a = O(A(1)); if (A(2)) W8(A(2), 0); RET(a ? a->arr_data : 0); } \
    static void j_Release##T##ArrayElements(CPU *) {} \
    static void j_Get##T##ArrayRegion(CPU *c) { Ref a = O(A(1)); if (a) memcpy(GPTR(A(4)), GPTR(a->arr_data + A(2) * elem_size(ch)), A(3) * elem_size(ch)); } \
    static void j_Set##T##ArrayRegion(CPU *c) { Ref a = O(A(1)); if (a) memcpy(GPTR(a->arr_data + A(2) * elem_size(ch)), GPTR(A(4)), A(3) * elem_size(ch)); }
ARRAYS(Boolean, 'Z') ARRAYS(Byte, 'B') ARRAYS(Char, 'C') ARRAYS(Short, 'S') ARRAYS(Int, 'I') ARRAYS(Long, 'J') ARRAYS(Float, 'F') ARRAYS(Double, 'D')
static void j_GetPrimitiveArrayCritical(CPU *c) { Ref a = O(A(1)); if (A(2)) W8(A(2), 0); RET(a ? a->arr_data : 0); }
static void j_ReleasePrimitiveArrayCritical(CPU *) {}
static void j_GetStringCritical(CPU *c) { j_GetStringChars(c); }
static void j_ReleaseStringCritical(CPU *c) { gfree(A(2)); }

// ---- natives, monitors, misc
static void j_RegisterNatives(CPU *c) {
    JClass *k = K(A(1)); uint32_t m = A(2), n = A(3);
    if (!k) { RET(-1); return; }
    std::lock_guard<std::recursive_mutex> g(mu);
    for (uint32_t i = 0; i < n; i++) {
        std::string name = GSTR(R32(m + 12 * i)), sig = GSTR(R32(m + 12 * i + 4));
        k->natives[name + sig] = R32(m + 12 * i + 8);
        if (verbose > 1) logf("JNI RegisterNatives %s.%s%s -> %s", k->name.c_str(), name.c_str(), sig.c_str(), sym_name(R32(m + 12 * i + 8)));
    }
    RET(0);
}
static void j_UnregisterNatives(CPU *c) { RET(0); }
static void j_MonitorEnter(CPU *c) { RET(0); }
static void j_MonitorExit(CPU *c) { RET(0); }
static void j_GetJavaVM(CPU *c) { W32(A(1), g_vm); RET(0); }
static void j_ExceptionCheck(CPU *c) { RET(pending_exc ? 1 : 0); }
static void j_NewDirectByteBuffer(CPU *c) { RET(to_handle(new_direct_buffer(A(1), A(2)))); }   // (env, addr, i64 cap): cap in r2:r3
static void j_GetDirectBufferAddress(CPU *c) { Ref o = O(A(1)); RET(o ? o->buf_addr : 0); }
static void j_GetDirectBufferCapacity(CPU *c) { Ref o = O(A(1)); ret64(c, o ? o->buf_cap : (uint64_t)-1); }
static void j_GetObjectRefType(CPU *c) { RET(1); }
static void j_FromReflectedMethod(CPU *c) { Ref o = O(A(1)); RET(o && o->as_method ? method_id(o->as_method) : 0); }
static void j_FromReflectedField(CPU *c) { Ref o = O(A(1)); RET(o && o->as_field ? field_id(o->as_field) : 0); }
static void j_ToReflectedMethod(CPU *c) {
    JMethod *m = method_of(A(2)); Ref o = new_obj(def_class("java/lang/reflect/Method")); o->as_method = m; RET(to_handle(o));
}
static void j_ToReflectedField(CPU *c) {
    JField *f = field_of(A(2)); Ref o = new_obj(def_class("java/lang/reflect/Field")); o->as_field = f; RET(to_handle(o));
}
static void j_DefineClass(CPU *c) { RET(0); }
static void j_unused(CPU *c) { fatal("JNI: unsupported function called from %s", sym_name(c->r[14])); }

// ---- JavaVM
static void vm_DestroyJavaVM(CPU *c) { RET(0); }
static void vm_AttachCurrentThread(CPU *c) { self_thread(); if (A(1)) W32(A(1), g_env); RET(0); }
static void vm_DetachCurrentThread(CPU *c) { RET(0); }
static void vm_GetEnv(CPU *c) { self_thread(); W32(A(1), g_env); RET(0); }

uint32_t vm() { return g_vm; }
uint32_t env() { return g_env; }

void init() {
    if (g_env) return;
    class_class = def_class("java/lang/Class");
    for (auto &kv : classes()) if (kv.second->class_obj) kv.second->class_obj->cls = class_class;
    const char *names[233] = {
        "reserved0", "reserved1", "reserved2", "reserved3", "GetVersion", "DefineClass", "FindClass", "FromReflectedMethod",
        "FromReflectedField", "ToReflectedMethod", "GetSuperclass", "IsAssignableFrom", "ToReflectedField", "Throw", "ThrowNew",
        "ExceptionOccurred", "ExceptionDescribe", "ExceptionClear", "FatalError", "PushLocalFrame", "PopLocalFrame", "NewGlobalRef",
        "DeleteGlobalRef", "DeleteLocalRef", "IsSameObject", "NewLocalRef", "EnsureLocalCapacity", "AllocObject", "NewObject",
        "NewObjectV", "NewObjectA", "GetObjectClass", "IsInstanceOf", "GetMethodID"};
    Fn f[233] = {nullptr};
    int i = 4;
    Fn first[] = {j_GetVersion, j_DefineClass, j_FindClass, j_FromReflectedMethod, j_FromReflectedField, j_ToReflectedMethod,
                  j_GetSuperclass, j_IsAssignableFrom, j_ToReflectedField, j_Throw, j_ThrowNew, j_ExceptionOccurred,
                  j_ExceptionDescribe, j_ExceptionClear, j_FatalError, j_PushLocalFrame, j_PopLocalFrame, j_NewGlobalRef,
                  j_DeleteRef, j_DeleteRef, j_IsSameObject, j_NewLocalRef, j_EnsureLocalCapacity, j_AllocObject, j_NewObject,
                  j_NewObjectV, j_NewObjectA, j_GetObjectClass, j_IsInstanceOf, j_GetMethodID};
    for (Fn x : first) f[i++] = x;
#define TRIPLE(P, T, S) f[i++] = j_##P##T##Method##S;
#define CALL_ALL(P) \
    f[i++] = j_##P##ObjectMethod; f[i++] = j_##P##ObjectMethodV; f[i++] = j_##P##ObjectMethodA; \
    f[i++] = j_##P##BooleanMethod; f[i++] = j_##P##BooleanMethodV; f[i++] = j_##P##BooleanMethodA; \
    f[i++] = j_##P##ByteMethod; f[i++] = j_##P##ByteMethodV; f[i++] = j_##P##ByteMethodA; \
    f[i++] = j_##P##CharMethod; f[i++] = j_##P##CharMethodV; f[i++] = j_##P##CharMethodA; \
    f[i++] = j_##P##ShortMethod; f[i++] = j_##P##ShortMethodV; f[i++] = j_##P##ShortMethodA; \
    f[i++] = j_##P##IntMethod; f[i++] = j_##P##IntMethodV; f[i++] = j_##P##IntMethodA; \
    f[i++] = j_##P##LongMethod; f[i++] = j_##P##LongMethodV; f[i++] = j_##P##LongMethodA; \
    f[i++] = j_##P##FloatMethod; f[i++] = j_##P##FloatMethodV; f[i++] = j_##P##FloatMethodA; \
    f[i++] = j_##P##DoubleMethod; f[i++] = j_##P##DoubleMethodV; f[i++] = j_##P##DoubleMethodA; \
    f[i++] = j_##P##VoidMethod; f[i++] = j_##P##VoidMethodV; f[i++] = j_##P##VoidMethodA;
    CALL_ALL(Call)                 // 34..63
    CALL_ALL(CallNonvirtual)       // 64..93
    f[i++] = j_GetFieldID;         // 94
    f[i++] = j_GetObjectField; f[i++] = j_GetBooleanField; f[i++] = j_GetByteField; f[i++] = j_GetCharField; f[i++] = j_GetShortField;
    f[i++] = j_GetIntField; f[i++] = j_GetLongField; f[i++] = j_GetFloatField; f[i++] = j_GetDoubleField;
    f[i++] = j_SetObjectField; f[i++] = j_SetBooleanField; f[i++] = j_SetByteField; f[i++] = j_SetCharField; f[i++] = j_SetShortField;
    f[i++] = j_SetIntField; f[i++] = j_SetLongField; f[i++] = j_SetFloatField; f[i++] = j_SetDoubleField;
    f[i++] = j_GetStaticMethodID;  // 113
    CALL_ALL(CallStatic)           // 114..143
    f[i++] = j_GetStaticFieldID;   // 144
    f[i++] = j_GetStaticObjectField; f[i++] = j_GetStaticBooleanField; f[i++] = j_GetStaticByteField; f[i++] = j_GetStaticCharField;
    f[i++] = j_GetStaticShortField; f[i++] = j_GetStaticIntField; f[i++] = j_GetStaticLongField; f[i++] = j_GetStaticFloatField;
    f[i++] = j_GetStaticDoubleField;
    f[i++] = j_SetStaticObjectField; f[i++] = j_SetStaticBooleanField; f[i++] = j_SetStaticByteField; f[i++] = j_SetStaticCharField;
    f[i++] = j_SetStaticShortField; f[i++] = j_SetStaticIntField; f[i++] = j_SetStaticLongField; f[i++] = j_SetStaticFloatField;
    f[i++] = j_SetStaticDoubleField;
    f[i++] = j_NewString; f[i++] = j_GetStringLength; f[i++] = j_GetStringChars; f[i++] = j_ReleaseStringChars;     // 163
    f[i++] = j_NewStringUTF; f[i++] = j_GetStringUTFLength; f[i++] = j_GetStringUTFChars; f[i++] = j_ReleaseStringUTFChars;
    f[i++] = j_GetArrayLength; f[i++] = j_NewObjectArray; f[i++] = j_GetObjectArrayElement; f[i++] = j_SetObjectArrayElement;  // 171
    f[i++] = j_NewBooleanArray; f[i++] = j_NewByteArray; f[i++] = j_NewCharArray; f[i++] = j_NewShortArray;               // 175
    f[i++] = j_NewIntArray; f[i++] = j_NewLongArray; f[i++] = j_NewFloatArray; f[i++] = j_NewDoubleArray;
    f[i++] = j_GetBooleanArrayElements; f[i++] = j_GetByteArrayElements; f[i++] = j_GetCharArrayElements; f[i++] = j_GetShortArrayElements;  // 183
    f[i++] = j_GetIntArrayElements; f[i++] = j_GetLongArrayElements; f[i++] = j_GetFloatArrayElements; f[i++] = j_GetDoubleArrayElements;
    f[i++] = j_ReleaseBooleanArrayElements; f[i++] = j_ReleaseByteArrayElements; f[i++] = j_ReleaseCharArrayElements; f[i++] = j_ReleaseShortArrayElements;
    f[i++] = j_ReleaseIntArrayElements; f[i++] = j_ReleaseLongArrayElements; f[i++] = j_ReleaseFloatArrayElements; f[i++] = j_ReleaseDoubleArrayElements;
    f[i++] = j_GetBooleanArrayRegion; f[i++] = j_GetByteArrayRegion; f[i++] = j_GetCharArrayRegion; f[i++] = j_GetShortArrayRegion;  // 199
    f[i++] = j_GetIntArrayRegion; f[i++] = j_GetLongArrayRegion; f[i++] = j_GetFloatArrayRegion; f[i++] = j_GetDoubleArrayRegion;
    f[i++] = j_SetBooleanArrayRegion; f[i++] = j_SetByteArrayRegion; f[i++] = j_SetCharArrayRegion; f[i++] = j_SetShortArrayRegion;  // 207
    f[i++] = j_SetIntArrayRegion; f[i++] = j_SetLongArrayRegion; f[i++] = j_SetFloatArrayRegion; f[i++] = j_SetDoubleArrayRegion;
    f[i++] = j_RegisterNatives; f[i++] = j_UnregisterNatives; f[i++] = j_MonitorEnter; f[i++] = j_MonitorExit; f[i++] = j_GetJavaVM;  // 215
    f[i++] = j_GetStringRegion; f[i++] = j_GetStringUTFRegion; f[i++] = j_GetPrimitiveArrayCritical; f[i++] = j_ReleasePrimitiveArrayCritical;  // 220
    f[i++] = j_GetStringCritical; f[i++] = j_ReleaseStringCritical; f[i++] = j_NewGlobalRef /* NewWeakGlobalRef */; f[i++] = j_DeleteRef;  // 224
    f[i++] = j_ExceptionCheck; f[i++] = j_NewDirectByteBuffer; f[i++] = j_GetDirectBufferAddress; f[i++] = j_GetDirectBufferCapacity;  // 228
    f[i++] = j_GetObjectRefType;   // 232
    if (i != 233) fatal("JNI table has %d entries", i);
    (void)names;
    uint32_t tbl = gstatic(233 * 4);
    for (int k = 0; k < 233; k++) {
        char nm[32]; snprintf(nm, sizeof nm, "JNIEnv#%d", k);
        W32(tbl + 4 * k, new_trap(f[k] ? f[k] : j_unused, k < 4 ? "JNI reserved" : "JNIEnv"));
    }
    g_env = gstatic(16); W32(g_env, tbl);
    Fn vf[8] = {j_unused, j_unused, j_unused, vm_DestroyJavaVM, vm_AttachCurrentThread, vm_DetachCurrentThread, vm_GetEnv, vm_AttachCurrentThread};
    uint32_t vt = gstatic(8 * 4);
    for (int k = 0; k < 8; k++) W32(vt + 4 * k, new_trap(vf[k], "JavaVM"));
    g_vm = gstatic(16); W32(g_vm, vt);
    (void)table; (void)table_names;
}
}  // namespace jni
