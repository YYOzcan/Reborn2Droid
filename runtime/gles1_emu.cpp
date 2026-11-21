// OpenGL ES 1.1 on top of OpenGL ES 2.0: the fixed-function pipeline (matrices, client arrays,
// texture environments incl. GL_COMBINE, alpha test, fog, per-vertex lighting) is generated as GLSL.
//
// Why: GLES 1.1 drivers on today's phones are thin, rarely tested layers over GLES 2/3 and disagree
// with each other (Unity's GUI text came out as opaque boxes on Adreno and on the Android emulator).
// With this layer the picture only depends on GLES 2.0 behaviour, which every device gets right.
#include "rt.h"
#include <cstring>
#include <cmath>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>

namespace gles1 {
typedef unsigned GLenum; typedef int GLint; typedef unsigned GLuint; typedef float GLfloat; typedef int GLsizei;
typedef unsigned char GLboolean; typedef unsigned GLbitfield; typedef int GLfixed; typedef intptr_t GLintptr;

// ---------------------------------------------------------------- real GLES 2 entry points
#define ES2_FUNCS(X) \
    X(void, glEnable, (GLenum)) X(void, glDisable, (GLenum)) X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(void, glGetFloatv, (GLenum, GLfloat *)) X(const unsigned char *, glGetString, (GLenum)) \
    X(GLuint, glCreateShader, (GLenum)) X(void, glShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, char *)) X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) X(void, glBindAttribLocation, (GLuint, GLuint, const char *)) \
    X(void, glLinkProgram, (GLuint)) X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *)) X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const char *)) X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) X(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) X(void, glDisableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    X(void, glActiveTexture, (GLenum)) X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
    X(void, glCopyTexImage2D, (GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *)) X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glGenerateMipmap, (GLenum)) X(void, glBlendFunc, (GLenum, GLenum)) X(void, glClearDepthf, (GLfloat))

#define DECL(r, n, a) static r (*p_##n) a;
ES2_FUNCS(DECL)
#undef DECL
static bool active = false;

// ---------------------------------------------------------------- state
struct Mat { float m[16]; };
static Mat ident() { Mat r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static Mat mul(const Mat &a, const Mat &b) {
    Mat r{};
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
        r.m[c * 4 + rr] = s;
    }
    return r;
}
static bool is_ident(const Mat &a) { Mat i = ident(); return !memcmp(&a, &i, sizeof a); }

enum { MAXU = 4 };
struct Array { bool on = false; GLint size = 4; GLenum type = 0x1406; GLsizei stride = 0; const void *ptr = nullptr; GLuint buf = 0; };
struct TexEnv {
    GLenum mode = 0x2100;                                     // GL_MODULATE
    GLenum crgb = 0x2100, calpha = 0x2100;
    GLenum srgb[3] = {0x1702, 0x8578, 0x8576}, salpha[3] = {0x1702, 0x8578, 0x8576};   // TEXTURE, PREVIOUS, CONSTANT (GL defaults)
    GLenum orgb[3] = {0x300, 0x300, 0x302}, oalpha[3] = {0x302, 0x302, 0x302};
    float rgb_scale = 1, alpha_scale = 1;
    float color[4] = {0, 0, 0, 0};
};
struct Light {
    bool on = false;
    float amb[4] = {0, 0, 0, 1}, dif[4] = {0, 0, 0, 1}, spe[4] = {0, 0, 0, 1}, pos[4] = {0, 0, 1, 0};
    float spot_dir[3] = {0, 0, -1}, spot_exp = 0, spot_cut = 180, att[3] = {1, 0, 0};
};
static struct State {
    GLenum matrix_mode = 0x1700;
    Mat mv = ident(), proj = ident(), tex[MAXU];
    std::vector<Mat> mv_stack, proj_stack, tex_stack[MAXU];
    int unit = 0, client_unit = 0;
    bool tex_on[MAXU] = {false};
    GLuint bound_tex[MAXU] = {0};
    TexEnv env[MAXU];
    Array vtx, col, nrm, tc[MAXU];
    float color[4] = {1, 1, 1, 1}, normal[3] = {0, 0, 1}, texcoord[MAXU][4];
    bool alpha_test = false; GLenum alpha_func = 0x207; float alpha_ref = 0;
    bool fog = false; GLenum fog_mode = 0x800; float fog_density = 1, fog_start = 0, fog_end = 1, fog_color[4] = {0, 0, 0, 0};
    bool lighting = false, color_material = false, normalize = false, rescale = false, two_side = false;
    Light light[8];
    float model_ambient[4] = {0.2f, 0.2f, 0.2f, 1};
    float m_amb[4] = {0.2f, 0.2f, 0.2f, 1}, m_dif[4] = {0.8f, 0.8f, 0.8f, 1}, m_spe[4] = {0, 0, 0, 1}, m_emi[4] = {0, 0, 0, 1}, m_shine = 0;
    GLuint array_buf = 0;
} S;
static std::unordered_map<GLuint, GLenum> tex_format;    // texture -> base format (GL_ALPHA, GL_RGB, ...)

static Mat &cur_matrix() {
    if (S.matrix_mode == 0x1701) return S.proj;
    if (S.matrix_mode == 0x1702) return S.tex[S.unit];
    return S.mv;
}
static std::vector<Mat> &cur_stack() {
    if (S.matrix_mode == 0x1701) return S.proj_stack;
    if (S.matrix_mode == 0x1702) return S.tex_stack[S.unit];
    return S.mv_stack;
}

// ---------------------------------------------------------------- shader generation
struct Program {
    GLuint id = 0;
    GLint u_mvp, u_mv, u_nm, u_tm[MAXU], u_tex[MAXU], u_envc[MAXU], u_aref, u_fogc, u_fogp, u_color;
    GLint u_lamb[8], u_ldif[8], u_lspe[8], u_lpos[8], u_lspot[8], u_ldir[8], u_latt[8];
    GLint u_mamb, u_mdif, u_mspe, u_memi, u_mshine, u_gamb;
};
static std::unordered_map<std::string, Program *> programs;

static const char *src_expr(GLenum src, int unit, bool alpha_texture) {
    static thread_local char buf[64];
    switch (src) {
    case 0x1702: return "t";                        // GL_TEXTURE
    case 0x8576: snprintf(buf, sizeof buf, "u_envc%d", unit); return buf;   // GL_CONSTANT
    case 0x8577: return "v_color";                  // GL_PRIMARY_COLOR
    case 0x8578: return "p";                        // GL_PREVIOUS
    default:
        if (src >= 0x84C0 && src < 0x84C0 + MAXU) { snprintf(buf, sizeof buf, "t%u", src - 0x84C0); return buf; }   // crossbar
        return "p";
    }
    (void)alpha_texture;
}
static std::string operand(const char *v, GLenum op, bool alpha) {
    switch (op) {
    case 0x300: return std::string(v) + (alpha ? ".a" : ".rgb");                    // SRC_COLOR
    case 0x301: return alpha ? "(1.0 - " + std::string(v) + ".a)" : "(vec3(1.0) - " + std::string(v) + ".rgb)";
    case 0x302: return alpha ? std::string(v) + ".a" : "vec3(" + std::string(v) + ".a)";       // SRC_ALPHA
    default: return alpha ? "(1.0 - " + std::string(v) + ".a)" : "vec3(1.0 - " + std::string(v) + ".a)";
    }
}
static std::string combine(GLenum fn, const std::string a[3], bool alpha) {
    switch (fn) {
    case 0x1E01: return a[0];                                         // REPLACE
    case 0x2100: return a[0] + " * " + a[1];                          // MODULATE
    case 0x0104: return a[0] + " + " + a[1];                          // ADD
    case 0x8574: return a[0] + " + " + a[1] + (alpha ? " - 0.5" : " - vec3(0.5)");   // ADD_SIGNED
    case 0x8575: return "mix(" + a[1] + ", " + a[0] + ", " + a[2] + ")";               // INTERPOLATE
    case 0x84E7: return a[0] + " - " + a[1];                          // SUBTRACT
    case 0x86AE: case 0x86AF:                                         // DOT3_RGB / DOT3_RGBA
        return alpha ? "1.0" : "vec3(4.0 * dot(" + a[0] + " - vec3(0.5), " + a[1] + " - vec3(0.5)))";
    }
    return a[0] + " * " + a[1];
}

static std::string state_key() {
    std::string k;
    char b[160];
    for (int u = 0; u < MAXU; u++) {
        if (!S.tex_on[u]) { k += "-"; continue; }
        const TexEnv &e = S.env[u];
        GLenum fmt = tex_format.count(S.bound_tex[u]) ? tex_format[S.bound_tex[u]] : 0x1908;
        snprintf(b, sizeof b, "T%d:%x:%x:%x:%x,%x,%x:%x,%x,%x:%x,%x,%x:%x,%x,%x:%g:%g:%d:%d|", u, e.mode, fmt, e.crgb, e.srgb[0], e.srgb[1], e.srgb[2],
                 e.orgb[0], e.orgb[1], e.orgb[2], e.salpha[0], e.salpha[1], e.salpha[2], e.oalpha[0], e.oalpha[1], e.oalpha[2] | (e.calpha << 16),
                 e.rgb_scale, e.alpha_scale, S.tc[u].on ? S.tc[u].size : 0, is_ident(S.tex[u]) ? 0 : 1);
        k += b;
    }
    snprintf(b, sizeof b, "A%d:%x F%d:%x C%d N%d L%d:%d:%d:%d", S.alpha_test, S.alpha_func, S.fog, S.fog_mode, S.col.on, S.nrm.on,
             S.lighting, S.color_material, S.normalize || S.rescale, S.two_side);
    k += b;
    if (S.lighting) for (int i = 0; i < 8; i++) {
        if (!S.light[i].on) continue;
        snprintf(b, sizeof b, "|l%d:%d:%d", i, S.light[i].pos[3] != 0, S.light[i].spot_cut != 180);
        k += b;
    }
    return k;
}

static GLuint compile(GLenum type, const std::string &src) {
    GLuint s = p_glCreateShader(type);
    const char *c = src.c_str();
    p_glShaderSource(s, 1, &c, nullptr);
    p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, 0x8B81, &ok);
    if (!ok) { char log[2048]; p_glGetShaderInfoLog(s, sizeof log, nullptr, log); rt::logf("gles1 shader error: %s\n%s", log, src.c_str()); }
    return s;
}

static Program *build_program() {
    std::string vs = "precision highp float;\nattribute vec4 a_pos; attribute vec4 a_color; attribute vec3 a_normal;\n"
                     "attribute vec4 a_tc0, a_tc1, a_tc2, a_tc3;\nuniform mat4 u_mvp, u_mv; uniform mat3 u_nm; uniform vec4 u_color;\n"
                     "varying vec4 v_color; varying float v_fog;\n";
    std::string fs = "precision mediump float;\nvarying vec4 v_color; varying float v_fog;\nuniform float u_aref; uniform vec4 u_fogc;\n";
    for (int u = 0; u < MAXU; u++) {
        if (!S.tex_on[u]) continue;
        char b[256];
        snprintf(b, sizeof b, "uniform mat4 u_tm%d; varying vec4 v_tc%d;\n", u, u); vs += b;
        snprintf(b, sizeof b, "uniform sampler2D u_tex%d; uniform vec4 u_envc%d; varying vec4 v_tc%d;\n", u, u, u); fs += b;
    }
    vs += "uniform vec3 u_fogp;\n";
    if (S.lighting) {
        vs += "uniform vec4 u_gamb, u_mamb, u_mdif, u_mspe, u_memi; uniform float u_mshine;\n";
        for (int i = 0; i < 8; i++) if (S.light[i].on) {
            char b[256];
            snprintf(b, sizeof b, "uniform vec4 u_lamb%d, u_ldif%d, u_lspe%d, u_lpos%d, u_lspot%d; uniform vec3 u_ldir%d, u_latt%d;\n", i, i, i, i, i, i, i);
            vs += b;
        }
    }
    vs += "void main() {\n  vec4 eye = u_mv * a_pos;\n  gl_Position = u_mvp * a_pos;\n";
    vs += S.col.on ? "  vec4 base = a_color;\n" : "  vec4 base = u_color;\n";
    if (S.lighting) {
        vs += std::string("  vec3 n = u_nm * a_normal;\n") + (S.normalize || S.rescale ? "  n = normalize(n);\n" : "");
        vs += S.color_material ? "  vec4 mamb = base, mdif = base;\n" : "  vec4 mamb = u_mamb, mdif = u_mdif;\n";
        vs += "  vec3 c = u_memi.rgb + mamb.rgb * u_gamb.rgb;\n";
        for (int i = 0; i < 8; i++) if (S.light[i].on) {
            char b[1024];
            bool point = S.light[i].pos[3] != 0, spot = S.light[i].spot_cut != 180;
            std::string L = point ? "u_lpos" + std::to_string(i) + ".xyz - eye.xyz" : "normalize(u_lpos" + std::to_string(i) + ".xyz)";
            std::string pre = point ? "    float dist = length(L); L = L / dist; att = 1.0 / (u_latt" + std::to_string(i) + ".x + u_latt" +
                                          std::to_string(i) + ".y * dist + u_latt" + std::to_string(i) + ".z * dist * dist);\n" : "";
            std::string sp = spot ? "    { float sd = dot(-L, normalize(u_ldir" + std::to_string(i) + ")); att *= sd >= u_lspot" + std::to_string(i) +
                                        ".y ? pow(max(sd, 0.0), u_lspot" + std::to_string(i) + ".x) : 0.0; }\n" : "";
            snprintf(b, sizeof b,
                     "  { vec3 L = %s; float att = 1.0;\n%s%s"
                     "    float d = max(dot(n, L), 0.0);\n"
                     "    vec3 H = normalize(L + vec3(0.0, 0.0, 1.0));\n"
                     "    float s = d > 0.0 ? pow(max(dot(n, H), 0.0), max(u_mshine, 0.0001)) : 0.0;\n"
                     "    c += att * (mamb.rgb * u_lamb%d.rgb + d * mdif.rgb * u_ldif%d.rgb + s * u_mspe.rgb * u_lspe%d.rgb); }\n",
                     L.c_str(), pre.c_str(), sp.c_str(), i, i, i);
            vs += b;
        }
        vs += "  v_color = vec4(clamp(c, 0.0, 1.0), mdif.a);\n";
    } else vs += "  v_color = base;\n";
    for (int u = 0; u < MAXU; u++) {
        if (!S.tex_on[u]) continue;
        char b[160];
        std::string tc = S.tc[u].on ? "a_tc" + std::to_string(u) : "vec4(0.0, 0.0, 0.0, 1.0)";
        snprintf(b, sizeof b, "  v_tc%d = u_tm%d * %s;\n", u, u, tc.c_str());
        vs += b;
    }
    if (S.fog) {
        vs += "  float z = abs(eye.z);\n";
        if (S.fog_mode == 0x2601) vs += "  v_fog = clamp((u_fogp.z - z) / (u_fogp.z - u_fogp.y), 0.0, 1.0);\n";         // LINEAR
        else if (S.fog_mode == 0x801) vs += "  v_fog = clamp(exp(-(u_fogp.x * z) * (u_fogp.x * z)), 0.0, 1.0);\n";      // EXP2
        else vs += "  v_fog = clamp(exp(-u_fogp.x * z), 0.0, 1.0);\n";                                                // EXP
    } else vs += "  v_fog = 1.0;\n";
    vs += "}\n";

    fs += "void main() {\n  vec4 p = v_color;\n";
    // all texture samples first (crossbar sources may read any unit)
    for (int u = 0; u < MAXU; u++) {
        if (!S.tex_on[u]) continue;
        GLenum fmt = tex_format.count(S.bound_tex[u]) ? tex_format[S.bound_tex[u]] : 0x1908;
        char b[256];
        // GLES1 texture base formats: ALPHA -> (0,0,0,A), LUMINANCE -> (L,L,L,1), LUMINANCE_ALPHA, RGB -> (.., 1)
        const char *sw = fmt == 0x1906 ? "vec4(0.0, 0.0, 0.0, s.a)" : fmt == 0x1909 ? "vec4(s.rrr, 1.0)" : fmt == 0x190A ? "vec4(s.rrr, s.a)"
                       : fmt == 0x1907 ? "vec4(s.rgb, 1.0)" : "s";
        std::string coord = S.tc[u].size == 4 || (S.tc[u].on && S.tc[u].size > 2 && false) ? "v_tc" + std::to_string(u) + ".xy / v_tc" + std::to_string(u) + ".w"
                                                                                              : "v_tc" + std::to_string(u) + ".xy";
        snprintf(b, sizeof b, "  vec4 t%d; { vec4 s = texture2D(u_tex%d, %s); t%d = %s; }\n", u, u, coord.c_str(), u, sw);
        fs += b;
    }
    for (int u = 0; u < MAXU; u++) {
        if (!S.tex_on[u]) continue;
        const TexEnv &e = S.env[u];
        GLenum fmt = tex_format.count(S.bound_tex[u]) ? tex_format[S.bound_tex[u]] : 0x1908;
        std::string t = "t" + std::to_string(u), ec = "u_envc" + std::to_string(u);
        fs += "  { vec4 t = " + t + ";\n";
        switch (e.mode) {
        case 0x1E01:   // REPLACE
            if (fmt == 0x1906) fs += "    p = vec4(p.rgb, t.a);\n";
            else if (fmt == 0x1907 || fmt == 0x1909) fs += "    p = vec4(t.rgb, p.a);\n";
            else fs += "    p = t;\n";
            break;
        case 0x2101:   // DECAL
            fs += fmt == 0x1907 ? "    p = vec4(t.rgb, p.a);\n" : "    p = vec4(mix(p.rgb, t.rgb, t.a), p.a);\n"; break;
        case 0x0BE2:   // BLEND
            fs += "    p = vec4(mix(p.rgb, " + ec + ".rgb, t.rgb), p.a * t.a);\n"; break;
        case 0x0104:   // ADD
            fs += "    p = vec4(p.rgb + t.rgb, p.a * t.a);\n"; break;
        case 0x8570: { // COMBINE
            std::string ar[3], aa[3];
            for (int k = 0; k < 3; k++) {
                ar[k] = operand(src_expr(e.srgb[k], u, false), e.orgb[k], false);
                aa[k] = operand(src_expr(e.salpha[k], u, true), e.oalpha[k], true);
            }
            char sc[64];
            std::string rgb = "(" + combine(e.crgb, ar, false) + ")";
            std::string al = "(" + combine(e.calpha, aa, true) + ")";
            if (e.crgb == 0x86AF) al = rgb + ".r";                     // DOT3_RGBA writes alpha too
            snprintf(sc, sizeof sc, "%.3f", e.rgb_scale); std::string rs = sc;
            snprintf(sc, sizeof sc, "%.3f", e.alpha_scale); std::string as = sc;
            fs += "    p = clamp(vec4(" + rgb + " * " + rs + ", " + al + " * " + as + "), 0.0, 1.0);\n";
            break;
        }
        default:       // MODULATE
            if (fmt == 0x1906) fs += "    p = vec4(p.rgb, p.a * t.a);\n";
            else if (fmt == 0x1907 || fmt == 0x1909) fs += "    p = vec4(p.rgb * t.rgb, p.a);\n";
            else fs += "    p = p * t;\n";
        }
        fs += "  }\n";
    }
    if (S.alpha_test) {
        const char *cmp = "";
        switch (S.alpha_func) {
        case 0x200: cmp = "true"; break;                              // NEVER -> discard always
        case 0x201: cmp = "!(p.a < u_aref)"; break;   case 0x202: cmp = "!(p.a == u_aref)"; break;
        case 0x203: cmp = "!(p.a <= u_aref)"; break;  case 0x204: cmp = "!(p.a > u_aref)"; break;
        case 0x205: cmp = "!(p.a != u_aref)"; break;  case 0x206: cmp = "!(p.a >= u_aref)"; break;
        default: cmp = "false";                                       // ALWAYS
        }
        fs += std::string("  if (") + cmp + ") discard;\n";
    }
    fs += "  gl_FragColor = vec4(mix(u_fogc.rgb, p.rgb, v_fog), p.a);\n}\n";

    Program *pr = new Program();
    pr->id = p_glCreateProgram();
    p_glAttachShader(pr->id, compile(0x8B31, vs));
    p_glAttachShader(pr->id, compile(0x8B30, fs));
    const char *attrs[] = {"a_pos", "a_color", "a_normal", "a_tc0", "a_tc1", "a_tc2", "a_tc3"};
    for (int k = 0; k < 7; k++) p_glBindAttribLocation(pr->id, k, attrs[k]);
    p_glLinkProgram(pr->id);
    GLint ok = 0; p_glGetProgramiv(pr->id, 0x8B82, &ok);
    if (!ok) { char log[2048]; p_glGetProgramInfoLog(pr->id, sizeof log, nullptr, log); rt::logf("gles1 link error: %s", log); }
    auto U = [&](const std::string &n) { return p_glGetUniformLocation(pr->id, n.c_str()); };
    pr->u_mvp = U("u_mvp"); pr->u_mv = U("u_mv"); pr->u_nm = U("u_nm"); pr->u_aref = U("u_aref"); pr->u_fogc = U("u_fogc");
    pr->u_fogp = U("u_fogp"); pr->u_color = U("u_color");
    for (int u = 0; u < MAXU; u++) { pr->u_tm[u] = U("u_tm" + std::to_string(u)); pr->u_tex[u] = U("u_tex" + std::to_string(u)); pr->u_envc[u] = U("u_envc" + std::to_string(u)); }
    for (int i = 0; i < 8; i++) {
        std::string s = std::to_string(i);
        pr->u_lamb[i] = U("u_lamb" + s); pr->u_ldif[i] = U("u_ldif" + s); pr->u_lspe[i] = U("u_lspe" + s); pr->u_lpos[i] = U("u_lpos" + s);
        pr->u_lspot[i] = U("u_lspot" + s); pr->u_ldir[i] = U("u_ldir" + s); pr->u_latt[i] = U("u_latt" + s);
    }
    pr->u_mamb = U("u_mamb"); pr->u_mdif = U("u_mdif"); pr->u_mspe = U("u_mspe"); pr->u_memi = U("u_memi"); pr->u_mshine = U("u_mshine"); pr->u_gamb = U("u_gamb");
    p_glUseProgram(pr->id);
    for (int u = 0; u < MAXU; u++) if (pr->u_tex[u] >= 0) p_glUniform1i(pr->u_tex[u], u);
    return pr;
}

static Mat inverse3x3_transpose(const Mat &m, float out[9]) {
    float a = m.m[0], b = m.m[4], c = m.m[8], d = m.m[1], e = m.m[5], f = m.m[9], g = m.m[2], h = m.m[6], i = m.m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C; if (fabsf(det) < 1e-20f) det = 1;
    float inv = 1.0f / det;
    // inverse transpose == cofactor matrix / det (column-major for glUniformMatrix3fv)
    out[0] = A * inv; out[1] = B * inv; out[2] = C * inv;
    out[3] = -(b * i - c * h) * inv; out[4] = (a * i - c * g) * inv; out[5] = -(a * h - b * g) * inv;
    out[6] = (b * f - c * e) * inv; out[7] = -(a * f - c * d) * inv; out[8] = (a * e - b * d) * inv;
    return m;
}

static void setup_array(GLuint idx, const Array &a, bool normalized_default) {
    if (!a.on) { p_glDisableVertexAttribArray(idx); return; }
    p_glBindBuffer(0x8892, a.buf);
    bool norm = normalized_default && (a.type == 0x1401 || a.type == 0x1400);   // ubyte/byte colours
    p_glVertexAttribPointer(idx, a.size, a.type, norm, a.stride, a.ptr);
    p_glEnableVertexAttribArray(idx);
}

static void prepare_draw() {
    std::string key = state_key();
    Program *&pr = programs[key];
    if (!pr) { pr = build_program(); if (rt::verbose) rt::logf("gles1: program %zu for %s", programs.size(), key.c_str()); }
    p_glUseProgram(pr->id);
    Mat mvp = mul(S.proj, S.mv);
    p_glUniformMatrix4fv(pr->u_mvp, 1, 0, mvp.m);
    p_glUniformMatrix4fv(pr->u_mv, 1, 0, S.mv.m);
    if (pr->u_nm >= 0) { float nm[9]; inverse3x3_transpose(S.mv, nm); p_glUniformMatrix3fv(pr->u_nm, 1, 0, nm); }
    if (pr->u_color >= 0) p_glUniform4fv(pr->u_color, 1, S.color);
    for (int u = 0; u < MAXU; u++) {
        if (!S.tex_on[u]) continue;
        p_glUniformMatrix4fv(pr->u_tm[u], 1, 0, S.tex[u].m);
        if (pr->u_envc[u] >= 0) p_glUniform4fv(pr->u_envc[u], 1, S.env[u].color);
    }
    if (pr->u_aref >= 0) p_glUniform1f(pr->u_aref, S.alpha_ref);
    if (pr->u_fogc >= 0) p_glUniform4fv(pr->u_fogc, 1, S.fog_color);
    if (pr->u_fogp >= 0) { float fp[3] = {S.fog_density, S.fog_start, S.fog_end}; p_glUniform3fv(pr->u_fogp, 1, fp); }
    if (S.lighting) {
        p_glUniform4fv(pr->u_gamb, 1, S.model_ambient);
        p_glUniform4fv(pr->u_mamb, 1, S.m_amb); p_glUniform4fv(pr->u_mdif, 1, S.m_dif); p_glUniform4fv(pr->u_mspe, 1, S.m_spe);
        p_glUniform4fv(pr->u_memi, 1, S.m_emi); p_glUniform1f(pr->u_mshine, S.m_shine);
        for (int i = 0; i < 8; i++) {
            if (!S.light[i].on) continue;
            Light &l = S.light[i];
            p_glUniform4fv(pr->u_lamb[i], 1, l.amb); p_glUniform4fv(pr->u_ldif[i], 1, l.dif); p_glUniform4fv(pr->u_lspe[i], 1, l.spe);
            p_glUniform4fv(pr->u_lpos[i], 1, l.pos);
            float spot[4] = {l.spot_exp, cosf(l.spot_cut * 3.14159265f / 180.f), 0, 0};
            if (pr->u_lspot[i] >= 0) p_glUniform4fv(pr->u_lspot[i], 1, spot);
            if (pr->u_ldir[i] >= 0) p_glUniform3fv(pr->u_ldir[i], 1, l.spot_dir);
            if (pr->u_latt[i] >= 0) p_glUniform3fv(pr->u_latt[i], 1, l.att);
        }
    }
    // attributes (client arrays or buffer offsets, as recorded at pointer time)
    setup_array(0, S.vtx, false);
    if (S.col.on) setup_array(1, S.col, true); else p_glDisableVertexAttribArray(1);
    if (S.nrm.on) setup_array(2, S.nrm, false); else { p_glDisableVertexAttribArray(2); p_glVertexAttrib4f(2, S.normal[0], S.normal[1], S.normal[2], 0); }
    for (int u = 0; u < MAXU; u++) {
        if (S.tex_on[u] && S.tc[u].on) setup_array(3 + u, S.tc[u], false);
        else { p_glDisableVertexAttribArray(3 + u); p_glVertexAttrib4f(3 + u, S.texcoord[u][0], S.texcoord[u][1], S.texcoord[u][2], S.texcoord[u][3]); }
    }
    p_glBindBuffer(0x8892, S.array_buf);
}

// ---------------------------------------------------------------- GLES 1.1 API
static void e_glMatrixMode(GLenum m) { S.matrix_mode = m; }
static void e_glLoadIdentity() { cur_matrix() = ident(); }
static void e_glLoadMatrixf(const GLfloat *m) { memcpy(cur_matrix().m, m, 64); }
static void e_glMultMatrixf(const GLfloat *m) { Mat b; memcpy(b.m, m, 64); cur_matrix() = mul(cur_matrix(), b); }
static void e_glPushMatrix() { cur_stack().push_back(cur_matrix()); }
static void e_glPopMatrix() { auto &s = cur_stack(); if (!s.empty()) { cur_matrix() = s.back(); s.pop_back(); } }
static void e_glOrthof(float l, float r, float b, float t, float n, float f) {
    Mat o = ident(); o.m[0] = 2 / (r - l); o.m[5] = 2 / (t - b); o.m[10] = -2 / (f - n);
    o.m[12] = -(r + l) / (r - l); o.m[13] = -(t + b) / (t - b); o.m[14] = -(f + n) / (f - n);
    cur_matrix() = mul(cur_matrix(), o);
}
static void e_glTranslatef(float x, float y, float z) { Mat t = ident(); t.m[12] = x; t.m[13] = y; t.m[14] = z; cur_matrix() = mul(cur_matrix(), t); }
static void e_glScalef(float x, float y, float z) { Mat t = ident(); t.m[0] = x; t.m[5] = y; t.m[10] = z; cur_matrix() = mul(cur_matrix(), t); }

static bool ff_cap(GLenum cap, bool on) {
    if (cap == 0x0DE1) { S.tex_on[S.unit] = on; return true; }
    if (cap == 0x0BC0) { S.alpha_test = on; return true; }
    if (cap == 0x0B60) { S.fog = on; return true; }
    if (cap == 0x0B50) { S.lighting = on; return true; }
    if (cap >= 0x4000 && cap < 0x4008) { S.light[cap - 0x4000].on = on; return true; }
    if (cap == 0x0B57) { S.color_material = on; return true; }
    if (cap == 0x0BA1) { S.normalize = on; return true; }
    if (cap == 0x803A) { S.rescale = on; return true; }
    if (cap == 0x0B10 || cap == 0x0B20 || cap == 0x0BF2 || cap == 0x809D || cap == 0x809F || cap == 0x8861 ||
        (cap >= 0x3000 && cap < 0x3006) || cap == 0x0B57 || cap == 0x0D10 || cap == 0x0D11) return true;   // no GLES2 equivalent
    return false;
}
static void e_glEnable(GLenum cap) { if (!ff_cap(cap, true)) p_glEnable(cap); }
static void e_glDisable(GLenum cap) { if (!ff_cap(cap, false)) p_glDisable(cap); }
static GLboolean e_glIsEnabled(GLenum cap) {
    if (cap == 0x0DE1) return S.tex_on[S.unit];
    if (cap == 0x0BC0) return S.alpha_test;
    if (cap == 0x0B60) return S.fog;
    if (cap == 0x0B50) return S.lighting;
    return 0;
}
static Array *client_array(GLenum a) {
    switch (a) {
    case 0x8074: return &S.vtx; case 0x8076: return &S.col; case 0x8075: return &S.nrm;
    case 0x8078: return &S.tc[S.client_unit];
    }
    return nullptr;
}
static void e_glEnableClientState(GLenum a) { if (Array *x = client_array(a)) x->on = true; }
static void e_glDisableClientState(GLenum a) { if (Array *x = client_array(a)) x->on = false; }
static void e_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *p) { S.vtx.size = size; S.vtx.type = type; S.vtx.stride = stride; S.vtx.ptr = p; S.vtx.buf = S.array_buf; }
static void e_glColorPointer(GLint size, GLenum type, GLsizei stride, const void *p) { S.col.size = size; S.col.type = type; S.col.stride = stride; S.col.ptr = p; S.col.buf = S.array_buf; }
static void e_glNormalPointer(GLenum type, GLsizei stride, const void *p) { S.nrm.size = 3; S.nrm.type = type; S.nrm.stride = stride; S.nrm.ptr = p; S.nrm.buf = S.array_buf; }
static void e_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *p) {
    Array &a = S.tc[S.client_unit]; a.size = size; a.type = type; a.stride = stride; a.ptr = p; a.buf = S.array_buf;
}
static void e_glClientActiveTexture(GLenum t) { S.client_unit = (t - 0x84C0) % MAXU; }
static void e_glActiveTexture(GLenum t) { S.unit = (t - 0x84C0) % MAXU; p_glActiveTexture(t); }
static void e_glBindTexture(GLenum target, GLuint t) { if (target == 0x0DE1) S.bound_tex[S.unit] = t; p_glBindTexture(target, t); }
static void e_glBindBuffer(GLenum target, GLuint b) { if (target == 0x8892) S.array_buf = b; p_glBindBuffer(target, b); }
static void e_glColor4f(float r, float g, float b, float a) { S.color[0] = r; S.color[1] = g; S.color[2] = b; S.color[3] = a; }
static void e_glColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { e_glColor4f(r / 255.f, g / 255.f, b / 255.f, a / 255.f); }
static void e_glNormal3f(float x, float y, float z) { S.normal[0] = x; S.normal[1] = y; S.normal[2] = z; }
static void e_glMultiTexCoord4f(GLenum t, float s, float tt, float r, float q) { int u = (t - 0x84C0) % MAXU; S.texcoord[u][0] = s; S.texcoord[u][1] = tt; S.texcoord[u][2] = r; S.texcoord[u][3] = q; }
static void e_glAlphaFunc(GLenum f, float ref) { S.alpha_func = f; S.alpha_ref = ref; }
static void e_glShadeModel(GLenum) {}
static void e_glHint(GLenum, GLenum) {}
static void e_glPointSize(float) {}

static void env_param(GLenum pname, float v) {
    TexEnv &e = S.env[S.unit]; GLenum iv = (GLenum)v;
    switch (pname) {
    case 0x2200: e.mode = iv; break;
    case 0x8571: e.crgb = iv; break;      case 0x8572: e.calpha = iv; break;
    case 0x8580: e.srgb[0] = iv; break;   case 0x8581: e.srgb[1] = iv; break;   case 0x8582: e.srgb[2] = iv; break;
    case 0x8588: e.salpha[0] = iv; break; case 0x8589: e.salpha[1] = iv; break; case 0x858A: e.salpha[2] = iv; break;
    case 0x8590: e.orgb[0] = iv; break;   case 0x8591: e.orgb[1] = iv; break;   case 0x8592: e.orgb[2] = iv; break;
    case 0x8598: e.oalpha[0] = iv; break; case 0x8599: e.oalpha[1] = iv; break; case 0x859A: e.oalpha[2] = iv; break;
    case 0x8573: e.rgb_scale = v; break;  case 0x0D1C: e.alpha_scale = v; break;
    }
}
static void e_glTexEnvi(GLenum target, GLenum pname, GLint v) { if (target == 0x2300) env_param(pname, (float)v); }
static void e_glTexEnvf(GLenum target, GLenum pname, GLfloat v) { if (target == 0x2300) env_param(pname, v); }
static void e_glTexEnvx(GLenum target, GLenum pname, GLfixed v) {
    if (target != 0x2300) return;
    env_param(pname, (pname == 0x8573 || pname == 0x0D1C) ? v / 65536.f : (float)v);
}
static void e_glTexEnvfv(GLenum target, GLenum pname, const GLfloat *v) {
    if (target != 0x2300) return;
    if (pname == 0x2201) memcpy(S.env[S.unit].color, v, 16); else env_param(pname, v[0]);
}
static void e_glTexEnviv(GLenum target, GLenum pname, const GLint *v) {
    if (target != 0x2300) return;
    if (pname == 0x2201) for (int k = 0; k < 4; k++) S.env[S.unit].color[k] = v[k] / 2147483647.f; else env_param(pname, (float)v[0]);
}
static void fog_param(GLenum p, const float *v) {
    switch (p) {
    case 0x0B65: S.fog_mode = (GLenum)v[0]; break; case 0x0B62: S.fog_density = v[0]; break;
    case 0x0B63: S.fog_start = v[0]; break;        case 0x0B64: S.fog_end = v[0]; break;
    case 0x0B66: memcpy(S.fog_color, v, 16); break;
    }
}
static void e_glFogf(GLenum p, float v) { fog_param(p, &v); }
static void e_glFogfv(GLenum p, const float *v) { fog_param(p, v); }
static void e_glFogx(GLenum p, GLfixed v) { float f = p == 0x0B65 ? (float)v : v / 65536.f; fog_param(p, &f); }
static void transform4(const Mat &m, const float in[4], float out[4]) {
    for (int r = 0; r < 4; r++) out[r] = m.m[r] * in[0] + m.m[4 + r] * in[1] + m.m[8 + r] * in[2] + m.m[12 + r] * in[3];
}
static void light_param(GLenum li, GLenum p, const float *v) {
    if (li < 0x4000 || li >= 0x4008) return;
    Light &l = S.light[li - 0x4000];
    switch (p) {
    case 0x1200: memcpy(l.amb, v, 16); break; case 0x1201: memcpy(l.dif, v, 16); break; case 0x1202: memcpy(l.spe, v, 16); break;
    case 0x1203: transform4(S.mv, v, l.pos); break;                                         // GL_POSITION: eye space now
    case 0x1204: { float d[4] = {v[0], v[1], v[2], 0}, o[4]; transform4(S.mv, d, o); memcpy(l.spot_dir, o, 12); break; }
    case 0x1205: l.spot_exp = v[0]; break; case 0x1206: l.spot_cut = v[0]; break;
    case 0x1207: l.att[0] = v[0]; break; case 0x1208: l.att[1] = v[0]; break; case 0x1209: l.att[2] = v[0]; break;
    }
}
static void e_glLightf(GLenum l, GLenum p, float v) { light_param(l, p, &v); }
static void e_glLightfv(GLenum l, GLenum p, const float *v) { light_param(l, p, v); }
static void e_glLightModelf(GLenum p, float v) { if (p == 0x0B52) S.two_side = v != 0; }
static void e_glLightModelfv(GLenum p, const float *v) { if (p == 0x0B53) memcpy(S.model_ambient, v, 16); else if (p == 0x0B52) S.two_side = v[0] != 0; }
static void material(GLenum p, const float *v) {
    switch (p) {
    case 0x1200: memcpy(S.m_amb, v, 16); break; case 0x1201: memcpy(S.m_dif, v, 16); break;
    case 0x1602: memcpy(S.m_amb, v, 16); memcpy(S.m_dif, v, 16); break;                  // AMBIENT_AND_DIFFUSE
    case 0x1202: memcpy(S.m_spe, v, 16); break; case 0x1600: memcpy(S.m_emi, v, 16); break;
    case 0x1601: S.m_shine = v[0]; break;
    }
}
static void e_glMaterialf(GLenum, GLenum p, float v) { material(p, &v); }
static void e_glMaterialfv(GLenum, GLenum p, const float *v) { material(p, v); }

static void e_glDrawArrays(GLenum mode, GLint first, GLsizei count) { prepare_draw(); p_glDrawArrays(mode, first, count); }
static void e_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) { prepare_draw(); p_glDrawElements(mode, count, type, idx); }

// texture base formats feed the texture environment
static void note_format(GLenum fmt) { if (S.bound_tex[S.unit]) tex_format[S.bound_tex[S.unit]] = fmt; }
static void e_glTexImage2D(GLenum t, GLint lvl, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum type, const void *p) {
    if (t == 0x0DE1 && lvl == 0) note_format(fmt == 0x80E1 ? 0x1908 : fmt);
    p_glTexImage2D(t, lvl, ifmt == 0x80E1 ? 0x80E1 : ifmt, w, h, b, fmt, type, p);
}
static void e_glCompressedTexImage2D(GLenum t, GLint lvl, GLenum fmt, GLsizei w, GLsizei h, GLint b, GLsizei sz, const void *p) {
    if (t == 0x0DE1 && lvl == 0) {
        bool has_alpha = fmt == 0x8C02 || fmt == 0x8C03 || fmt == 0x8C8E || fmt == 0x87EE || fmt == 0x8C93 || fmt == 0x83F2 || fmt == 0x83F3;
        note_format(has_alpha ? 0x1908 : 0x1907);
    }
    p_glCompressedTexImage2D(t, lvl, fmt, w, h, b, sz, p);
}
static void e_glCopyTexImage2D(GLenum t, GLint lvl, GLenum fmt, GLint x, GLint y, GLsizei w, GLsizei h, GLint b) {
    if (t == 0x0DE1 && lvl == 0) note_format(fmt);
    p_glCopyTexImage2D(t, lvl, fmt, x, y, w, h, b);
}
static void e_glDeleteTextures(GLsizei n, const GLuint *t) { for (int k = 0; k < n; k++) tex_format.erase(t[k]); p_glDeleteTextures(n, t); }
static void e_glTexParameteri(GLenum t, GLenum p, GLint v) {
    if (p == 0x8191) return;                        // GL_GENERATE_MIPMAP: GLES2 uses glGenerateMipmap after upload (Unity uploads levels itself)
    p_glTexParameteri(t, p, v);
}
static void e_glTexParameterf(GLenum t, GLenum p, GLfloat v) { e_glTexParameteri(t, p, (GLint)v); }

static void e_glGetIntegerv(GLenum p, GLint *v) {
    switch (p) {
    case 0x84E2: *v = MAXU; return;                 // GL_MAX_TEXTURE_UNITS
    case 0x0D31: *v = 8; return;                    // GL_MAX_LIGHTS
    case 0x0D36: case 0x0D38: case 0x0D39: *v = 32; return;   // matrix stack depths
    case 0x0D32: *v = 6; return;                    // GL_MAX_CLIP_PLANES
    case 0x0BA0: *v = (GLint)S.matrix_mode; return;
    case 0x84E0: *v = 0x84C0 + S.unit; return;
    case 0x84E1: *v = 0x84C0 + S.client_unit; return;
    case 0x8069: *v = (GLint)S.bound_tex[S.unit]; return;
    }
    p_glGetIntegerv(p, v);
}
static void e_glGetFloatv(GLenum p, GLfloat *v) {
    if (p == 0x0BA6) { memcpy(v, S.mv.m, 64); return; }
    if (p == 0x0BA7) { memcpy(v, S.proj.m, 64); return; }
    if (p == 0x0BA8) { memcpy(v, S.tex[S.unit].m, 64); return; }
    p_glGetFloatv(p, v);
}
static const unsigned char *e_glGetString(GLenum name) {
    static std::string ext;
    if (name == 0x1F02) return (const unsigned char *)"OpenGL ES-CM 1.1 (GLES2 fixed-function emulation)";
    if (name == 0x1F03) {
        if (ext.empty()) {
            const char *e = (const char *)p_glGetString(0x1F03);
            ext = std::string(e ? e : "") + " GL_OES_framebuffer_object GL_OES_blend_func_separate GL_OES_blend_equation_separate "
                  "GL_OES_blend_subtract GL_OES_texture_env_crossbar GL_OES_texture_cube_map GL_OES_point_size_array";
        }
        return (const unsigned char *)ext.c_str();
    }
    return p_glGetString(name);
}
static void e_glClearDepthf(GLfloat d) { p_glClearDepthf(d); }

// ---------------------------------------------------------------- hooking
static std::map<std::string, void *> table;
bool init(void *(*get)(const char *)) {
#define LOAD(r, n, a) p_##n = (r(*) a)get(#n); if (!p_##n) { rt::logf("gles1 emulation: %s missing", #n); return false; }
    ES2_FUNCS(LOAD)
#undef LOAD
    for (int u = 0; u < MAXU; u++) { S.tex[u] = ident(); S.texcoord[u][3] = 1; }
    S.light[0].dif[0] = S.light[0].dif[1] = S.light[0].dif[2] = 1; S.light[0].spe[0] = S.light[0].spe[1] = S.light[0].spe[2] = 1;
#define T(n) table[#n] = (void *)e_##n; table[std::string(#n) + "OES"] = (void *)e_##n;
    T(glMatrixMode) T(glLoadIdentity) T(glLoadMatrixf) T(glMultMatrixf) T(glPushMatrix) T(glPopMatrix) T(glOrthof) T(glTranslatef)
    T(glScalef) T(glEnable) T(glDisable) T(glIsEnabled) T(glEnableClientState) T(glDisableClientState) T(glVertexPointer)
    T(glColorPointer) T(glNormalPointer) T(glTexCoordPointer) T(glClientActiveTexture) T(glActiveTexture) T(glBindTexture)
    T(glBindBuffer) T(glColor4f) T(glColor4ub) T(glNormal3f) T(glMultiTexCoord4f) T(glAlphaFunc) T(glShadeModel) T(glHint)
    T(glPointSize) T(glTexEnvi) T(glTexEnvf) T(glTexEnvx) T(glTexEnvfv) T(glTexEnviv) T(glFogf) T(glFogfv) T(glFogx) T(glLightf)
    T(glLightfv) T(glLightModelf) T(glLightModelfv) T(glMaterialf) T(glMaterialfv) T(glDrawArrays) T(glDrawElements)
    T(glTexImage2D) T(glCompressedTexImage2D) T(glCopyTexImage2D) T(glDeleteTextures) T(glTexParameteri) T(glTexParameterf)
    T(glGetIntegerv) T(glGetFloatv) T(glGetString) T(glClearDepthf)
#undef T
    active = true;
    rt::logf("GLES 1.1 fixed-function emulation on GLES 2.0 enabled");
    return true;
}
// the GLES 1.1 name: emulated entry point, or the GLES 2 core function (OES suffix dropped)
void *lookup(const char *name, void *(*get)(const char *)) {
    auto it = table.find(name);
    if (it != table.end()) return it->second;
    std::string n = name;
    if (n.size() > 3 && n.compare(n.size() - 3, 3, "OES") == 0) {
        std::string core = n.substr(0, n.size() - 3);
        auto it2 = table.find(core);
        if (it2 != table.end()) return it2->second;
        if (void *p = get(core.c_str())) return p;
    }
    return get(name);
}
bool enabled() { return active; }
}  // namespace gles1
