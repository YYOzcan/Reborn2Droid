"""gen_imports.py <out.c> <imports.txt>... : names of every imported function (bound to IMP() at start-up)."""
import sys
names = set()
for p in sys.argv[2:]:
    for line in open(p):
        n = line.strip().split("@")[0]
        if n: names.add(n)
names |= {"pthread_cond_signal", "pthread_self", "longjmp", "siglongjmp", "_longjmp", "dlerror", "read", "munmap", "syscall",
          "glFlush", "glLineWidth", "glPolygonOffset", "glBlendEquation", "glDiscardFramebufferEXT"}
names -= {"__sF", "__stack_chk_guard", "_ctype_", "_tolower_tab_", "_toupper_tab_", "environ", "__page_size", "data_start"}
names = sorted(names)
with open(sys.argv[1], "w") as f:
    f.write('#include "guest.h"\n')
    f.write("const char *const import_names[] = {\n")
    for n in names: f.write(f'  "{n}",\n')
    f.write("};\nconst unsigned import_count = %d;\n" % len(names))
    # imports without a runtime implementation resolve to the other guest library (libunity -> libmono)
    f.write("void dyn_import(CPU *c, unsigned idx);\n")
    for k, n in enumerate(names):
        f.write(f"__attribute__((weak)) void imp_{n}(CPU *c) {{ dyn_import(c, {k}); }}\n")
print(len(names), "imports")
