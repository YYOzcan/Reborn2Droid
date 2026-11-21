# How Reborn2Droid works

## 1. Static recompilation (`recomp/recomp.py`)

Each 32-bit ARM library becomes portable C. Every guest function is a C function
`void U_<vaddr>(CPU *c)` that operates on a register file (`CPU`: r0–r15, NZCV, VFP/NEON
registers, exclusive monitor). Guest calls are host calls and guest returns are host returns,
so the C compiler sees ordinary functions and optimises them well.

Function discovery combines: exported symbols, `.ARM.exidx` unwind entries, `R_ARM_RELATIVE`
code pointers, direct call targets, PIC jump tables (`tbb/tbh`, `add pc, pc, rX, lsl #2`
with immediate or register bounds), ARMv4 indirect calls (`mov lr, pc; ldr pc, [...]`) and an
optional list of entry points found at run time (`profiles/<package>/roots_*.txt`, collected
with `RB_ENTRIES`). A fixpoint marks functions that never return.

Notable translation details:

* `ldrex/strex` become compare-and-swap against the value seen by `ldrex` – real atomics
  between host threads; `swp`, kernel user helpers (`__kuser_cmpxchg` at `0xffff0fc0`) and
  CP15 barriers are emulated.
* VFP is softfp; NaNs are canonicalised like ARM; `-ffp-contract=off` keeps rounding identical.
* Any instruction the recompiler cannot translate continues in the interpreter, so a gap is
  slow, never fatal.

## 2. Guest memory

The guest sees a 32-bit address space mapped 1:1 into a 4 GB host reservation (`g_mem`).
The libraries are loaded at fixed bases (`libunity` 0x10000000, `libmono` 0x11000000) with
their relocations applied; imports point at trap addresses that dispatch to the runtime.
The guest heap is a dlmalloc mspace inside the window; `mmap` returns heap blocks.

## 3. The interpreter (`runtime/interp.cpp`)

Mono's JIT writes ARM code at run time, which static recompilation cannot see. The
interpreter executes it (ARM + VFP, the instruction mix Mono 2.x emits). Calls from JIT code
into recompiled functions are host calls; returns are matched by `(lr, sp)` per
"activation". Mono's exception unwinding restores a register context and jumps into an outer
managed frame — the interpreter detects a return into an outer activation and unwinds the host
stack to it with `longjmp`. Null dereferences in JIT code raise a synchronous guest SIGSEGV with
a Linux/ARM `ucontext`, exactly what Mono's handler expects for `NullReferenceException`.

## 4. Threads and signals (`runtime/rt_thread.cpp`)

Each guest thread is a host thread with a guest stack. bionic's 4-byte `pthread_mutex_t`,
`pthread_cond_t` and `sem_t` hold tagged handles to host objects. Boehm GC stops the world
with signals; guest signals are delivered at safe points (function entries, interpreter
branches, every blocking wait) on a guest signal frame below the interrupted `sp`, so the GC
finds the saved registers on the stack it scans.

## 5. The Java side (`runtime/rt_jni.cpp`, `runtime/java_classes.cpp`)

The engine calls Java through JNI. The JNIEnv/JavaVM tables live in guest memory and trap into
a small object model (classes, methods, fields, strings, arrays, direct buffers, reflection,
generation-tagged references). On top of it the Java classes of the original APK are
reimplemented: `UnityPlayer`, `PlayerPrefs` (persisted to `shared_prefs.txt`),
`ReflectionHelper` (for `AndroidJavaObject` in C#), `UnityJavaRunnable`, `WWW` (offline),
`FMODAudioDevice` (pulled by SDL audio), plus lenient stubs for ad/analytics/gamepad SDKs.
Unknown methods of lenient classes are logged and return defaults.

## 6. Graphics (`runtime/gles1_emu.cpp`, `runtime/rt_gl.cpp`)

GLES calls are thunked from the guest ABI (softfp floats, guest pointers, client arrays vs.
buffer offsets). Games built for GLES 1.1 get a fixed-function pipeline generated as GLSL on
GLES 2.0: matrices, client arrays, all texture-environment modes including `GL_COMBINE` with
crossbar sources, alpha test, fog and per-vertex lighting. Modern phones' GLES 1.1 drivers are
thin, inconsistent layers (Unity's GUI text rendered as opaque boxes on several of them); the
emulation makes output identical everywhere and is faster.

The runtime reports a 16-bit, alpha-less framebuffer to the engine (what these games asked
for), which keeps Unity off its destination-alpha GUI path.

## 7. Host (`runtime/main_sdl.cpp`)

SDL2 provides the window, GL context, touch, accelerometer, audio and lifecycle. The host
drives the engine exactly like the original `UnityPlayer` Java class did: `nativeFile`,
`initJni`, `nativeInit`, `nativeRender` per frame, `unityAndroidInit` after the first frame,
pause/resume, orientation requests (`Activity.setRequestedOrientation`), back key.
