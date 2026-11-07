// opengl: WGL context on a hidden window, then into a framebuffer object (so pixel ownership
// of the window doesn't matter): clear, a fixed-function quad, and a GLSL fullscreen pass,
// each read back with glReadPixels and compared pixel by pixel.
//
// On the Mac this is Wine's opengl32 on Zink over KosmicKrisp (or CGL, by compatdb rule); on
// Linux it is Mesa (llvmpipe under Xvfb).
#include "gfx.h"
#include <GL/gl.h>

// The few GL 2.0 / 3.0 entry points needed, fetched through wglGetProcAddress.
typedef char GLchar;
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GLFUNCS(X)                                                                                          \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) X(void, glBindFramebuffer, (GLenum, GLuint))             \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) X(void, glBindRenderbuffer, (GLenum, GLuint))           \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))                                      \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))                                    \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) X(GLuint, glCreateShader, (GLenum))                        \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))                        \
    X(void, glCompileShader, (GLuint)) X(void, glGetShaderiv, (GLuint, GLenum, GLint *))                     \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(GLuint, glCreateProgram, (void))   \
    X(void, glAttachShader, (GLuint, GLuint)) X(void, glLinkProgram, (GLuint))                               \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) X(void, glUseProgram, (GLuint))
#define DECL(ret, name, args) typedef ret(APIENTRY *name##_t) args; static name##_t p_##name;
GLFUNCS(DECL)

static uint8_t px[W * H * 4];
// glReadPixels rows go bottom-up; flip into the top-down layout check_pixels expects.
static void read_fbo() {
    static uint8_t raw[W * H * 4];
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, raw);
    for (int y = 0; y < H; y++) memcpy(px + y * W * 4, raw + (H - 1 - y) * W * 4, W * 4);
}

static GLuint shader(GLenum type, const char *src, char *log, size_t n) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, nullptr);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { p_glGetShaderInfoLog(s, (GLsizei)n, nullptr, log); return 0; }
    return s;
}

int main(int argc, char **argv) {
    ladder_begin("opengl", argc, argv);
    HWND wnd = gfx_window("ladder opengl");
    HDC dc = wnd ? GetDC(wnd) : nullptr;
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cAlphaBits = 8; pfd.iLayerType = PFD_MAIN_PLANE;
    int fmt = dc ? ChoosePixelFormat(dc, &pfd) : 0;
    HGLRC rc = nullptr;
    if (fmt && SetPixelFormat(dc, fmt, &pfd)) rc = wglCreateContext(dc);
    if (!check(rc && wglMakeCurrent(dc, rc), "context", "window %p, pixel format %d, context %p", (void *)wnd, fmt, (void *)rc))
        return ladder_done();
    info("renderer", "%s | %s | %s", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
         (const char *)glGetString(GL_VERSION));

    int missing = 0;
#define LOAD(ret, name, args) p_##name = (name##_t)(void *)wglGetProcAddress(#name); missing += !p_##name;
    GLFUNCS(LOAD)
    if (!check(missing == 0, "entry_points", "%d of the GL 2.0/3.0 functions missing from wglGetProcAddress", missing)) return ladder_done();

    GLuint fbo = 0, rbo = 0;
    p_glGenFramebuffers(1, &fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glGenRenderbuffers(1, &rbo); p_glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (!check(st == GL_FRAMEBUFFER_COMPLETE, "fbo", "64x64 RGBA8 framebuffer object status %04x", st)) return ladder_done();
    glViewport(0, 0, W, H);
    glDisable(GL_DITHER);

    // clear
    glClearColor(k_clear_color[0], k_clear_color[1], k_clear_color[2], k_clear_color[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    read_fbo();
    check_pixels("clear", px, W * 4, [](int, int) { return k_clear; }, false, "glClear -> glReadPixels");

    // fixed-function quad in a y-down pixel projection, edges on pixel boundaries
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glColor4ub(k_quad.r, k_quad.g, k_quad.b, k_quad.a);
    glBegin(GL_QUADS);
    glVertex2f(16, 16); glVertex2f(48, 16); glVertex2f(48, 48); glVertex2f(16, 48);
    glEnd();
    read_fbo();
    check_pixels("draw_fixed", px, W * 4, [](int x, int y) { return in_quad(x, y) ? k_quad : k_clear; }, false,
                 "immediate-mode quad");

    // GLSL fullscreen pass: the same quadrant pattern as the D3D programs (gl_FragCoord is y-up)
    const char *vs = "#version 110\nvoid main() { gl_Position = gl_Vertex; }\n";
    const char *fs = "#version 110\nvoid main() {\n"
                     "  gl_FragColor = vec4(gl_FragCoord.x < 32.0 ? 1.0 : 0.0, gl_FragCoord.y > 32.0 ? 1.0 : 0.0, 0.2, 1.0);\n}\n";
    char log[512] = "";
    GLuint v = shader(GL_VERTEX_SHADER, vs, log, sizeof log), f = v ? shader(GL_FRAGMENT_SHADER, fs, log, sizeof log) : 0;
    GLuint prog = 0; GLint linked = 0;
    if (v && f) {
        prog = p_glCreateProgram(); p_glAttachShader(prog, v); p_glAttachShader(prog, f); p_glLinkProgram(prog);
        p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    }
    if (check(linked, "glsl_build", "GLSL 1.10 program %s%s", linked ? "linked" : "failed: ", linked ? "" : log)) {
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        p_glUseProgram(prog);
        glRectf(-1, -1, 1, 1);
        p_glUseProgram(0);
        read_fbo();
        check_pixels("draw_glsl", px, W * 4, quadrants, false, "fullscreen GLSL pass");
    }
    GLenum err = glGetError();
    check(err == GL_NO_ERROR, "gl_error", "glGetError after all draws: %04x", err);

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    return ladder_done();
}
