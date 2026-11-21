// OpenGL ES bridge shared by the generated thunks (rt_gl_gen.cpp) and rt_gl.cpp.
#pragma once
#include "rt.h"
#include <vector>
#include <string>
typedef int GLint; typedef unsigned GLenum; typedef float GLfloat; typedef unsigned char GLboolean; typedef unsigned GLuint;

static inline const void *gl_in(uint32_t p) { return p ? (const void *)GP(p) : nullptr; }
const void *gl_attrib_ptr(uint32_t p);     // offset into the bound ARRAY_BUFFER, or client memory
const void *gl_index_ptr(uint32_t p);      // offset into the bound ELEMENT_ARRAY_BUFFER, or client memory
void gl_missing(const char *name);
void gl_load_all(void *(*get)(const char *));
extern int gl_debug;
extern int gl_trace_on;
void gl_trace(const char *name, CPU *c, int nargs);
void gl_check(const char *name);
#define GL_CHECK(n) do { if (__builtin_expect(gl_debug, 0)) gl_check(n); } while (0)

struct GuestStrings {                      // glShaderSource(shader, count, const char **strings, const int *lengths)
    std::vector<const char *> ptrs; std::vector<GLint> lens; bool has_lens;
    GuestStrings(uint32_t count, uint32_t strs, uint32_t lenp) : has_lens(lenp != 0) {
        for (uint32_t k = 0; k < count; k++) {
            ptrs.push_back((const char *)GP(R32(strs + 4 * k)));
            if (lenp) lens.push_back((GLint)R32(lenp + 4 * k));
        }
    }
    const GLint *lens_ptr() const { return has_lens ? lens.data() : nullptr; }
};
