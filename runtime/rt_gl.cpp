// Hand-written GL entry points: strings, buffer binding tracking, ETC1 on desktop GL ES 3 contexts.
#include "rt_gl.h"
#include <map>
#include <cstring>

using namespace rt;
#define A(i) arg(c, i)
#define RET(v) (c->r[0] = (uint32_t)(v))

void *gl_ptr_glGetString(); void *gl_ptr_glGetIntegerv(); void *gl_ptr_glBindBuffer(); void *gl_ptr_glMapBufferOES();
void *gl_ptr_glUnmapBufferOES(); void *gl_ptr_glGetVertexAttribPointerv(); void *gl_ptr_glCompressedTexImage2D();
void *gl_ptr_glDeleteBuffers(); void *gl_ptr_glGetError();

int gl_debug = getenv("RB_GLDEBUG") ? atoi(getenv("RB_GLDEBUG")) : 0;
int gl_trace_on = 0;
void gl_trace(const char *name, CPU *c, int n) {
    char b[256]; int k = snprintf(b, sizeof b, "GLT %s(", name);
    for (int i = 0; i < n && k < 200; i++) k += snprintf(b + k, sizeof b - k, i ? ", %x" : "%x", arg(c, i));
    logf("%s)", b);
}
bool gl_desktop_es3 = false;               // set by the host: ETC1 must be uploaded as ETC2
static uint32_t bound_array, bound_element;

const void *gl_attrib_ptr(uint32_t p) { return bound_array ? (const void *)(uintptr_t)p : gl_in(p); }
const void *gl_index_ptr(uint32_t p) { return bound_element ? (const void *)(uintptr_t)p : gl_in(p); }
void gl_missing(const char *name) {
    // (reported once per function)
    static std::map<std::string, int> seen;
    if (!seen[name]++) logf("GL function %s is not available", name);
}
void gl_check(const char *name) {
    typedef GLenum (*F)(void); static F ge = (F)gl_ptr_glGetError();
    GLenum e = ge ? ge() : 0;
    static std::map<std::string, int> seen;
    if (e && seen[name]++ < 5) logf("GL error 0x%x after %s", e, name);
}

IMP(glGetString) {
    if (gl_trace_on) gl_trace("glGetString", c, 4);
    typedef const char *(*F)(GLenum); F f = (F)gl_ptr_glGetString(); if (!f) { gl_missing("glGetString"); c->r[0] = 0; return; }
    GLenum name = A(0);
    static std::map<GLenum, uint32_t> cache;
    auto it = cache.find(name);
    if (it != cache.end()) { RET(it->second); return; }
    const char *s = f ? f(name) : nullptr;
    std::string v = s ? s : "";
    if (getenv("RB_FAKE_GLES1")) {          // dev: let the desktop build look like a phone's GLES 1.1 driver
        if (name == 0x1F00) v = "Qualcomm";
        if (name == 0x1F01) v = "Adreno (TM) 618";
        if (name == 0x1F02) v = "OpenGL ES-CM 1.1";
        if (name == 0x1F03) v = "GL_AMD_compressed_ATC_texture GL_AMD_compressed_3DC_texture GL_OES_compressed_ETC1_RGB8_texture "
                                  "GL_OES_framebuffer_object GL_OES_depth24 GL_OES_rgb8_rgba8 GL_OES_packed_depth_stencil "
                                  "GL_OES_texture_npot GL_OES_element_index_uint GL_EXT_texture_format_BGRA8888 "
                                  "GL_OES_blend_func_separate GL_OES_blend_equation_separate GL_OES_texture_env_crossbar "
                                  "GL_OES_texture_cube_map GL_OES_point_sprite GL_OES_draw_texture GL_EXT_discard_framebuffer ";
    }
    if (name == 0x1F03) {                   // GL_EXTENSIONS: hide what the bridge cannot do (mapped buffers, VAOs)
        std::string out, tok; size_t p = 0;
        v += " ";
        while ((p = v.find(' ')) != std::string::npos) {
            tok = v.substr(0, p); v = v.substr(p + 1);
            if (tok.empty() || tok == "GL_OES_mapbuffer" || tok == "GL_OES_vertex_array_object" || tok == "GL_EXT_map_buffer_range") continue;
            out += tok + " ";
        }
        if (gl_desktop_es3 && out.find("GL_OES_compressed_ETC1_RGB8_texture") == std::string::npos) out += "GL_OES_compressed_ETC1_RGB8_texture ";
        v = out;
    }
    uint32_t g = gstrdup(v.c_str());
    cache[name] = g;
    RET(g);
}
IMP(glGetIntegerv) {
    if (gl_trace_on) gl_trace("glGetIntegerv", c, 4);
    typedef void (*F)(GLenum, GLint *); F f = (F)gl_ptr_glGetIntegerv(); if (!f) { gl_missing("glGetIntegerv"); c->r[0] = 0; return; }
    GLenum pname = A(0);
    // colour depth as the game asked for it (16-bit, no alpha, settings.xml "32bit_display" = False): with a
    // 24/32-bit report Unity switches to a destination-alpha GUI path that phone window surfaces can't honour
    if (pname == 0x0D52 || pname == 0x0D54) { W32(A(1), 5); return; }   // GL_RED_BITS / GL_BLUE_BITS
    if (pname == 0x0D53) { W32(A(1), 6); return; }                      // GL_GREEN_BITS
    if (pname == 0x84E2) { GLint n = 0; f(pname, &n); W32(A(1), n > 4 ? 4 : n); return; }   // GL_MAX_TEXTURE_UNITS: as on 2014 GPUs
    if (pname == 0x0D55) { W32(A(1), 0); return; }                      // GL_ALPHA_BITS
    if (pname == 0x8894) { W32(A(1), bound_array); return; }        // GL_ARRAY_BUFFER_BINDING
    if (pname == 0x8895) { W32(A(1), bound_element); return; }      // GL_ELEMENT_ARRAY_BUFFER_BINDING
    if (pname == 0x86A2 && gl_desktop_es3) {                        // GL_NUM_COMPRESSED_TEXTURE_FORMATS: add ETC1
        GLint n = 0; f(pname, &n); W32(A(1), n + 1); return;
    }
    if (pname == 0x86A3 && gl_desktop_es3) {
        GLint n = 0; f(0x86A2, &n); std::vector<GLint> v(n + 1); f(pname, v.data()); v[n] = 0x8D64;
        memcpy(GPTR(A(1)), v.data(), 4 * (n + 1)); return;
    }
    GLint tmp[16] = {0};
    f(pname, tmp);
    int n = (pname == 0x0BA2 || pname == 0x0C10 || pname == 0x0C22 || pname == 0x0C23) ? 4 : (pname == 0x0D3A || pname == 0x846D || pname == 0x846E || pname == 0x0B70) ? 2 : 1;
    memcpy(GPTR(A(1)), tmp, 4 * n);
}
IMP(glBindBuffer) {
    if (gl_trace_on) gl_trace("glBindBuffer", c, 4);
    typedef void (*F)(GLenum, GLuint); F f = (F)gl_ptr_glBindBuffer(); if (!f) { gl_missing("glBindBuffer"); c->r[0] = 0; return; }
    if (A(0) == 0x8892) bound_array = A(1);
    else if (A(0) == 0x8893) bound_element = A(1);
    f(A(0), A(1));
}
IMP(glDeleteBuffers) {
    typedef void (*F)(GLint, const GLuint *); F f = (F)gl_ptr_glDeleteBuffers(); if (!f) { gl_missing("glDeleteBuffers"); c->r[0] = 0; return; }
    for (uint32_t k = 0; k < A(0); k++) {
        uint32_t b = R32(A(1) + 4 * k);
        if (b == bound_array) bound_array = 0;
        if (b == bound_element) bound_element = 0;
    }
    f((GLint)A(0), (const GLuint *)GPTR(A(1)));
}
IMP(glMapBufferOES) { RET(0); }
IMP(glUnmapBufferOES) { RET(0); }
IMP(glGetVertexAttribPointerv) {
    typedef void (*F)(GLuint, GLenum, void **); F f = (F)gl_ptr_glGetVertexAttribPointerv(); if (!f) { gl_missing("glGetVertexAttribPointerv"); c->r[0] = 0; return; }
    void *p = nullptr; f(A(0), A(1), &p);
    uintptr_t v = (uintptr_t)p;
    W32(A(2), (v >= (uintptr_t)g_mem && v < (uintptr_t)g_mem + 0x100000000ull) ? G32(p) : (uint32_t)v);
}
IMP(glCompressedTexImage2D) {
    if (gl_trace_on) gl_trace("glCompressedTexImage2D", c, 4);
    typedef void (*F)(GLenum, GLint, GLenum, GLint, GLint, GLint, GLint, const void *); F f = (F)gl_ptr_glCompressedTexImage2D(); if (!f) { gl_missing("glCompressedTexImage2D"); c->r[0] = 0; return; }
    GLenum fmt = A(2);
    if (gl_debug > 1) logf("glCompressedTexImage2D(level %u, fmt %x, %ux%u, size %u)", A(1), fmt, A(3), A(4), A(6));
    if (fmt == 0x8D64 && gl_desktop_es3) fmt = 0x9274;               // ETC1 -> ETC2 RGB8 (ETC1 is a subset)
    f(A(0), (GLint)A(1), fmt, (GLint)A(3), (GLint)A(4), (GLint)A(5), (GLint)A(6), gl_in(A(7)));
    GL_CHECK("glCompressedTexImage2D");
}

// Separate blending: Unity calls the GLES 2 entry points even on GLES 1.1 contexts. Plain glBlendFunc
// with the colour factors gives the same picture on screen and exists on every driver.
void *gl_ptr_glBlendFunc(); void *gl_ptr_glBlendEquationSeparate(); void *gl_ptr_glBlendEquation();
IMP(glBlendFuncSeparate) {
    if (gl_trace_on) gl_trace("glBlendFuncSeparate", c, 4);
    typedef void (*F)(GLenum, GLenum); F f = (F)gl_ptr_glBlendFunc(); if (!f) { gl_missing("glBlendFunc"); return; }
    f(A(0), A(1));
}
IMP(glBlendEquationSeparate) {
    if (gl_trace_on) gl_trace("glBlendEquationSeparate", c, 4);
    typedef void (*F)(GLenum); F f = (F)gl_ptr_glBlendEquation();
    if (A(0) == 0x8006) { if (f) f(0x8006); return; }          // GL_FUNC_ADD: the default
    if (f) f(A(0));
}
IMP(glBlendEquation) {
    typedef void (*F)(GLenum); F f = (F)gl_ptr_glBlendEquation();
    if (f) f(A(0));
}

// ---- GL_CONSTANT texture-combine source: emulated with the primary colour where the driver can't do it.
// Unity's GLES1 text/GUI shaders use "constantColor"; several GLES1-on-GLES2 driver layers (Android
// emulator, some Adreno/Mali builds) ignore it, which turns every glyph into an opaque box.
bool gl_emulate_constant = false;
void *gl_ptr_glTexEnvi(); void *gl_ptr_glTexEnvfv(); void *gl_ptr_glActiveTexture(); void *gl_ptr_glColor4f();
void *gl_ptr_glEnableClientState(); void *gl_ptr_glDisableClientState(); void *gl_ptr_glClientActiveTexture();
static int cur_unit;
static float env_color[8][4];
static float cur_color[4] = {1, 1, 1, 1};
static bool color_array;
static void set_color(const float *v) {
    typedef void (*F)(GLfloat, GLfloat, GLfloat, GLfloat); static F f = (F)gl_ptr_glColor4f();
    if (f) f(v[0], v[1], v[2], v[3]);
}
IMP(glActiveTexture) {
    if (gl_trace_on) gl_trace("glActiveTexture", c, 1);
    typedef void (*F)(GLenum); F f = (F)gl_ptr_glActiveTexture(); if (!f) { gl_missing("glActiveTexture"); return; }
    cur_unit = (A(0) - 0x84C0) & 7; f(A(0));
}
IMP(glClientActiveTexture) {
    if (gl_trace_on) gl_trace("glClientActiveTexture", c, 1);
    typedef void (*F)(GLenum); F f = (F)gl_ptr_glClientActiveTexture(); if (f) f(A(0));
}
IMP(glEnableClientState) {
    if (gl_trace_on) gl_trace("glEnableClientState", c, 1);
    typedef void (*F)(GLenum); F f = (F)gl_ptr_glEnableClientState(); if (!f) return;
    if (A(0) == 0x8076) color_array = true;
    f(A(0));
}
IMP(glDisableClientState) {
    if (gl_trace_on) gl_trace("glDisableClientState", c, 1);
    typedef void (*F)(GLenum); F f = (F)gl_ptr_glDisableClientState(); if (!f) return;
    if (A(0) == 0x8076) { color_array = false; }
    f(A(0));
}
IMP(glColor4f) {
    if (gl_trace_on) gl_trace("glColor4f", c, 4);
    for (int k = 0; k < 4; k++) cur_color[k] = argf(c, k);
    set_color(cur_color);
}
IMP(glTexEnvfv) {
    if (gl_trace_on) gl_trace("glTexEnvfv", c, 3);
    typedef void (*F)(GLenum, GLenum, const GLfloat *); F f = (F)gl_ptr_glTexEnvfv(); if (!f) { gl_missing("glTexEnvfv"); return; }
    if (A(1) == 0x2201) memcpy(env_color[cur_unit], GPTR(A(2)), 16);       // GL_TEXTURE_ENV_COLOR
    f(A(0), A(1), (const GLfloat *)GPTR(A(2)));
}
IMP(glTexEnvi) {
    if (gl_trace_on) gl_trace("glTexEnvi", c, 3);
    typedef void (*F)(GLenum, GLenum, GLint); F f = (F)gl_ptr_glTexEnvi(); if (!f) { gl_missing("glTexEnvi"); return; }
    GLenum pname = A(1); GLint v = (GLint)A(2);
    bool src = pname >= 0x8580 && pname <= 0x858A;                          // GL_SRC0_RGB .. GL_SRC2_ALPHA
    if (gl_emulate_constant && src && v == 0x8576 && !color_array) {         // GL_CONSTANT -> GL_PRIMARY_COLOR
        v = 0x8578;
        set_color(env_color[cur_unit]);
    }
    f(A(0), pname, v);
}
