// A small Java object model behind a guest-facing JNI (JNIEnv / JavaVM tables in guest memory).
// The Java side of the original APK (UnityPlayer, PlayerPrefs, FMODAudioDevice, plugins) is
// reimplemented in java_classes.cpp on top of this.
#pragma once
#include "rt.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jni {
struct JClass;
struct JMethod;
struct JField;

struct Obj : std::enable_shared_from_this<Obj> {
    JClass *cls = nullptr;
    std::string str;                         // java.lang.String (UTF-8)
    char arr_type = 0;                       // primitive array element type, or 'L'
    uint32_t arr_len = 0, arr_data = 0;      // primitive array storage lives in guest memory
    std::vector<std::shared_ptr<Obj>> objs;  // object array elements
    uint32_t buf_addr = 0, buf_cap = 0, buf_pos = 0;   // direct ByteBuffer
    std::map<std::string, struct JV> *fields = nullptr;
    JClass *as_class = nullptr;              // java.lang.Class
    JMethod *as_method = nullptr;            // java.lang.reflect.Method / Constructor
    JField *as_field = nullptr;              // java.lang.reflect.Field
    std::shared_ptr<void> peer;              // C++ state of a reimplemented class
    ~Obj();
};
using Ref = std::shared_ptr<Obj>;

struct JV {
    uint64_t v = 0; Ref o;
    static JV I(int32_t x) { JV r; r.v = (uint32_t)x; return r; }
    static JV Z(bool x) { JV r; r.v = x ? 1 : 0; return r; }
    static JV J(int64_t x) { JV r; r.v = (uint64_t)x; return r; }
    static JV F(float x) { JV r; uint32_t u; memcpy(&u, &x, 4); r.v = u; return r; }
    static JV D(double x) { JV r; memcpy(&r.v, &x, 8); return r; }
    static JV O(Ref x) { JV r; r.o = std::move(x); return r; }
    int32_t i() const { return (int32_t)(uint32_t)v; }
    int64_t j() const { return (int64_t)v; }
    bool z() const { return (v & 0xff) != 0; }
    float f() const { uint32_t u = (uint32_t)v; float x; memcpy(&x, &u, 4); return x; }
    double d() const { double x; memcpy(&x, &v, 8); return x; }
    std::string s() const { return o ? o->str : std::string(); }
};
using Args = std::vector<JV>;
using Impl = std::function<JV(Obj *self, Args &a)>;

struct JMethod { JClass *cls; std::string name, sig; bool is_static; Impl impl; bool stub = false; };
struct JField { JClass *cls; std::string name, sig; bool is_static; JV sval; };

struct JClass {
    std::string name;
    JClass *super = nullptr;
    std::map<std::string, JMethod *> methods;    // name + sig
    std::map<std::string, JField *> fields;      // name
    std::map<std::string, uint32_t> natives;     // name + sig -> guest function
    Ref class_obj;
    bool lenient = false;                        // unknown methods become logged stubs
};

// ---- registry
JClass *def_class(const std::string &name, const std::string &super = "java/lang/Object", bool lenient = false);
JClass *find_class(const std::string &name);     // nullptr if unknown
JClass *stub_class(const std::string &name);     // lenient stub for app/plugin namespaces, else nullptr
void def_method(JClass *k, const std::string &name, const std::string &sig, Impl impl, bool is_static = false);
void def_static(JClass *k, const std::string &name, const std::string &sig, Impl impl);
JField *def_field(JClass *k, const std::string &name, const std::string &sig, bool is_static, JV v = JV());

// ---- objects
Ref new_obj(JClass *k);
Ref jstr(const std::string &s);
Ref new_prim_array(char type, uint32_t n);
Ref new_obj_array(JClass *elem, uint32_t n);
Ref new_direct_buffer(uint32_t addr, uint32_t cap);
JV get_field(Obj *o, const std::string &name);
void set_field(Obj *o, const std::string &name, JV v);

// ---- calling guest natives from "Java"
JV call_native(JClass *k, Obj *self, const std::string &name, const std::string &sig, const Args &a);
bool has_native(JClass *k, const std::string &name, const std::string &sig);

// ---- guest-facing tables
uint32_t vm();                     // JavaVM*
uint32_t env();                    // JNIEnv*
uint32_t to_handle(const Ref &o);  // local reference for the guest (0 for null)
Ref from_handle(uint32_t h);
void init();
void throw_new(const std::string &cls, const std::string &msg);
}  // namespace jni

void java_classes_init();          // java_classes.cpp
