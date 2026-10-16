// OpenGL function loading through SDL_GL_GetProcAddress.
#include "gl.h"
#include <SDL.h>
#include <cstdio>

#define CANOPY_GL_DEFINE(type, name) type name = nullptr;
CANOPY_GL_FUNCS(CANOPY_GL_DEFINE)
#undef CANOPY_GL_DEFINE

namespace canopy::gfx {

bool gl_load() {
    bool ok = true;
#define CANOPY_GL_LOAD(type, name)                                              \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                \
    if (!name) {                                                                \
        std::fprintf(stderr, "gl_load: missing GL function %s\n", #name);       \
        ok = false;                                                             \
    }
    CANOPY_GL_FUNCS(CANOPY_GL_LOAD)
#undef CANOPY_GL_LOAD
    return ok;
}

static const char* gl_error_name(GLenum e) {
    switch (e) {
    case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
    default: return "GL_ERROR";
    }
}

bool gl_check(const char* where) {
    if (!glGetError) return true;
    bool clean = true;
    for (int i = 0; i < 16; ++i) {   // bounded: a lost context returns errors forever
        GLenum e = glGetError();
        if (e == GL_NO_ERROR) break;
        clean = false;
        std::fprintf(stderr, "GL error %s (0x%04x) at %s\n", gl_error_name(e), (unsigned)e, where ? where : "?");
    }
    return clean;
}

} // namespace canopy::gfx
