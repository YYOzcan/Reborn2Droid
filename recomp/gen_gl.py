#!/usr/bin/env python3
"""Generate runtime/rt_gl_gen.cpp: guest (ARM softfp) -> host OpenGL ES thunks.

Argument kinds: i int/enum/uint, f float, p in/out pointer (guest -> host, 0 stays NULL),
A vertex-array pointer (offset when an ARRAY_BUFFER is bound), E index pointer (offset when an
ELEMENT_ARRAY_BUFFER is bound), s string, S string array (glShaderSource), x fixed/int passed as is.
Return kinds: v void, i int, b boolean.
"""
import sys

GL = """
v glActiveTexture i
v glAlphaFunc i f
v glAttachShader i i
v glBindAttribLocation i i s
v glBindBuffer i i
v glBindFramebuffer i i
v glBindFramebufferOES i i
v glBindRenderbuffer i i
v glBindRenderbufferOES i i
v glBindTexture i i
v glBlendEquationSeparate i i
v glBlendFunc i i
v glBlendFuncSeparate i i i i
v glBufferData i i p i
v glBufferSubData i i i p
i glCheckFramebufferStatus i
i glCheckFramebufferStatusOES i
v glClear i
v glClearColor f f f f
v glClearDepthf f
v glClearStencil i
v glClientActiveTexture i
v glColor4f f f f f
v glColorMask i i i i
v glColorPointer i i i A
v glCompileShader i
v glCompressedTexImage2D i i i i i i i p
v glCopyTexSubImage2D i i i i i i i i
i glCreateProgram
i glCreateShader i
v glCullFace i
v glDeleteBuffers i p
v glDeleteFramebuffers i p
v glDeleteFramebuffersOES i p
v glDeleteProgram i
v glDeleteRenderbuffers i p
v glDeleteRenderbuffersOES i p
v glDeleteShader i
v glDeleteTextures i p
v glDepthFunc i
v glDepthMask i
v glDisable i
v glDisableClientState i
v glDisableVertexAttribArray i
v glDrawArrays i i i
v glDrawElements i i i E
v glEnable i
v glEnableClientState i
v glEnableVertexAttribArray i
v glFinish
v glFlush
v glFogf i f
v glFogfv i p
v glFogx i i
v glFramebufferRenderbuffer i i i i
v glFramebufferRenderbufferOES i i i i
v glFramebufferTexture2D i i i i i
v glFramebufferTexture2DOES i i i i i
v glFrontFace i
v glGenBuffers i p
v glGenerateMipmap i
v glGenerateMipmapOES i
v glGenFramebuffers i p
v glGenFramebuffersOES i p
v glGenRenderbuffers i p
v glGenRenderbuffersOES i p
v glGenTextures i p
v glGetActiveAttrib i i i p p p p
v glGetActiveUniform i i i p p p p
i glGetAttribLocation i s
i glGetError
v glGetFloatv i p
v glGetFramebufferAttachmentParameterivOES i i i p
v glGetProgramInfoLog i i p p
v glGetProgramiv i i p
v glGetShaderInfoLog i i p p
v glGetShaderiv i i p
v glGetShaderSource i i p p
v glGetUniformfv i i p
v glGetUniformiv i i p
i glGetUniformLocation i s
v glGetVertexAttribiv i i p
v glHint i i
v glLightf i i f
v glLightfv i i p
v glLightModelf i f
v glLightModelfv i p
v glLinkProgram i
v glLoadIdentity
v glLoadMatrixf p
v glMaterialf i i f
v glMaterialfv i i p
v glMatrixMode i
v glNormalPointer i i A
v glPixelStorei i i
v glReadPixels i i i i i i p
v glRenderbufferStorage i i i i
v glRenderbufferStorageOES i i i i
v glScissor i i i i
v glShadeModel i
v glShaderSource i i S p
v glStencilFunc i i i
v glStencilFuncSeparate i i i i
v glStencilMask i
v glStencilOp i i i
v glStencilOpSeparate i i i i
v glTexCoordPointer i i i A
v glTexEnvf i i f
v glTexEnvfv i i p
v glTexEnvi i i i
v glTexImage2D i i i i i i i i p
v glTexParameterf i i f
v glTexParameteri i i i
v glTexSubImage2D i i i i i i i i p
v glUniform1fv i i p
v glUniform1i i i
v glUniform2fv i i p
v glUniform3fv i i p
v glUniform4fv i i p
v glUniform2iv i i p
v glUniform3iv i i p
v glUniform4iv i i p
v glUniformMatrix3fv i i i p
v glUniformMatrix4fv i i i p
v glUseProgram i
v glVertexAttribPointer i i i i i A
v glVertexPointer i i i A
v glViewport i i i i
v glDiscardFramebufferEXT i i p
v glBindVertexArrayOES i
v glDeleteVertexArraysOES i p
v glGenVertexArraysOES i p
v glLineWidth f
v glPolygonOffset f f
v glUniform1f i f
v glUniform2f i f f
v glUniform3f i f f f
v glUniform4f i f f f f
v glUniform1iv i i p
v glVertexAttrib4f i f f f f
v glVertexAttrib4fv i p
v glGetBooleanv i p
b glIsEnabled i
v glGetTexParameteriv i i p
v glTexParameteriv i i p
v glTexParameterfv i i p
v glValidateProgram i
v glDetachShader i i
v glBlendEquation i
v glBlendColor f f f f
v glDepthRangef f f
v glSampleCoverage f i
v glCopyTexImage2D i i i i i i i i
v glCompressedTexSubImage2D i i i i i i i i p
b glIsTexture i
b glIsBuffer i
b glIsFramebuffer i
b glIsRenderbuffer i
b glIsProgram i
b glIsShader i
v glGetRenderbufferParameteriv i i p
v glGetRenderbufferParameterivOES i i p
v glGetFramebufferAttachmentParameteriv i i i p
"""

# functions with hand-written implementations in rt_gl.cpp
MANUAL = {"glGetString", "glGetIntegerv", "glBindBuffer", "glMapBufferOES", "glUnmapBufferOES", "glGetVertexAttribPointerv",
          "glCompressedTexImage2D", "glDeleteBuffers", "glBlendFuncSeparate", "glBlendEquationSeparate", "glBlendEquation",
          "glTexEnvi", "glTexEnvfv", "glActiveTexture", "glColor4f", "glEnableClientState", "glDisableClientState", "glClientActiveTexture",
          "glViewport", "glScissor", "glBindFramebuffer", "glBindFramebufferOES"}

CT = {"i": "GLint", "f": "GLfloat", "p": "const void *", "A": "const void *", "E": "const void *", "s": "const char *",
      "S": "const char *const *", "x": "GLint"}

out = ['// generated by recomp/gen_gl.py', '#include "rt_gl.h"', 'using namespace rt;', '']
names = []
for line in GL.strip().splitlines():
    parts = line.split()
    ret, name, args = parts[0], parts[1], parts[2:]
    names.append(name)
    proto = ", ".join(CT[a] for a in args) or "void"
    rtype = {"v": "void", "i": "GLint", "b": "GLboolean"}[ret]
    out.append(f"typedef {rtype} (*PFN_{name})({proto});")
    out.append(f"static PFN_{name} p_{name};")
    out.append(f"void *gl_ptr_{name}() {{ return (void *)p_{name}; }}")
    if name in MANUAL:
        out.append("")
        continue
    body = []
    callargs = []
    for k, a in enumerate(args):
        if a == "f": callargs.append(f"argf(c, {k})")
        elif a == "i" or a == "x": callargs.append(f"(GLint)arg(c, {k})")
        elif a == "p": callargs.append(f"gl_in(arg(c, {k}))")
        elif a == "s": callargs.append(f"(const char *)gl_in(arg(c, {k}))")
        elif a == "A": callargs.append(f"gl_attrib_ptr(arg(c, {k}))")
        elif a == "E": callargs.append(f"gl_index_ptr(arg(c, {k}))")
        elif a == "S":
            body.append(f"  GuestStrings strs(arg(c, {k - 1}), arg(c, {k}), arg(c, {k + 1}));")
            callargs.append("strs.ptrs.data()")
        else: raise SystemExit(a)
    # glShaderSource: lengths array consumed by GuestStrings
    if name == "glShaderSource":
        callargs[3] = "strs.lens_ptr()"
    call = f"p_{name}({', '.join(callargs)})"
    out.append(f"IMP({name}) {{")
    out.append(f"  if (!p_{name}) {{ gl_missing(\"{name}\"); c->r[0] = 0; return; }}")
    out.extend(body)
    if ret == "v": out.append(f"  {call};")
    else: out.append(f"  c->r[0] = (uint32_t){call};")
    if name in ("glTexImage2D", "glTexSubImage2D", "glPixelStorei"):
        out.append(f"  if (gl_debug > 1) logf(\"{name}(%x, %x, %x, %x, %x, %x, %x, %x)\", arg(c,0), arg(c,1), arg(c,2), arg(c,3), arg(c,4), arg(c,5), arg(c,6), arg(c,7));")
    out.append(f"  if (__builtin_expect(gl_trace_on, 0)) gl_trace(\"{name}\", c, {len(args)});")
    out.append("  GL_CHECK(\"" + name + "\");")
    out.append("}")
    out.append("")

for n in sorted(MANUAL - set(names)):
    out.append(f"typedef void (*PFN_{n})(void);")
    out.append(f"static PFN_{n} p_{n};")
    out.append(f"void *gl_ptr_{n}() {{ return (void *)p_{n}; }}")
names += sorted(MANUAL - set(names))
out.append("void gl_load_all(void *(*get)(const char *)) {")
out.append("  std::string miss;")
for n in names:
    out.append(f"  p_{n} = (PFN_{n})get(\"{n}\"); if (!p_{n}) miss += \" {n}\";")
out.append("  if (!miss.empty()) logf(\"GL entry points not available:%s\", miss.c_str());")
out.append("}")
open(sys.argv[1], "w").write("\n".join(out) + "\n")
