//============================================================================
// gl_stubs_web.cpp  (Emscripten / WebGL2 only)
//
// Desktop-GL entry points absent from WebGL2 / GLES3. The D3D9->GL layer calls a
// handful of fixed-function / desktop-only GL functions. Here they are mapped onto
// the nearest WebGL2 primitive, or made no-ops where the feature simply does not
// exist on the web (wireframe, fixed-function alpha test).
//============================================================================
#ifdef __EMSCRIPTEN__

#include <GLES3/gl3.h>
#include <cstdio>

extern "C" {

// Fixed-function alpha test does not exist in GLES3/WebGL2; it is emulated with
// `discard` in the translated fragment shaders (milestone 5). No-op the state
// setter so existing call sites link and run.
void glAlphaFunc(GLenum /*func*/, GLclampf /*ref*/) {}

// No polygon-fill mode (wireframe) in GLES3/WebGL2. No-op.
void glPolygonMode(GLenum /*face*/, GLenum /*mode*/) {}

// Single-target draw-buffer select -> the GLES3 array form. GL_BACK on the
// default framebuffer maps to GL_BACK; an FBO attachment passes straight through.
void glDrawBuffer(GLenum mode) {
    GLenum bufs[1] = { mode };
    glDrawBuffers(1, bufs);
}

// Occurrence query result: GLES3 exposes the unsigned variant only. Forward.
void glGetQueryObjectiv(GLuint id, GLenum pname, GLint *params) {
    GLuint tmp = 0;
    glGetQueryObjectuiv(id, pname, &tmp);
    if (params) *params = (GLint)tmp;
}

// No base-vertex draw in WebGL2. For milestone 2 we forward to plain
// glDrawElements; correct whenever BaseVertexIndex == 0 (the common path).
// NOTE: a non-zero base vertex is silently ignored here — full emulation (fold
// the base vertex into the bound attribute offsets) is a milestone-5 renderer
// task and is flagged in WEB_PORT_TRIAGE.md.
void glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                              const void *indices, GLint basevertex) {
    static int warned = 0;
    if (basevertex != 0 && !warned) {
        warned = 1;
        fprintf(stderr, "[web] glDrawElementsBaseVertex: basevertex=%d ignored "
                        "(WebGL2 has no base-vertex draw; see M5)\n", basevertex);
    }
    glDrawElements(mode, count, type, indices);
}

} // extern "C"

#endif // __EMSCRIPTEN__
