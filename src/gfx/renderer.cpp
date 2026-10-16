// Batched 2D renderer: one VAO/VBO/IBO, one program, draw order preserved.
//
// The world->screen transform is applied on the CPU, so vertices are in framebuffer pixels
// and a transform change never forces a flush. Flushes happen on clip (scissor) changes,
// atlas texture changes, a full vertex buffer, and at end_frame.
// Blending is premultiplied (ONE, ONE_MINUS_SRC_ALPHA); vertex colors are straight alpha and
// get premultiplied in the fragment shader. Mask glyph coverage is raised to u_gamma (< 1)
// so light text on dark backgrounds does not look anemic.
#include "renderer.h"
#include "gl.h"
#include "shaders.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace canopy::gfx {

namespace {

constexpr int kMaxQuads = 1 << 16;   // per flush; 40 B * 4 verts * 65536 = 10.5 MB VBO
constexpr int kVboRing = 3;          // frames in flight before a VBO is rewritten

struct Vertex {
    float x, y;
    float u, v;            // texels (modes 1/2) or local SDF coords (modes 3-6)
    uint8_t rgba[4];
    float p[4];
    uint16_t mode, aux;    // aux: stroke thickness * 64
};
static_assert(sizeof(Vertex) == 40, "vertex layout");

enum Mode : uint16_t { M_SOLID = 0, M_MASK = 1, M_COLOR = 2, M_RBOX = 3, M_RBOX_STROKE = 4, M_WEDGE = 5, M_WEDGE_STROKE = 6 };

struct IRect { int x0, y0, x1, y1; };
struct FRect { float x0, y0, x1, y1; };

inline void pack_color(RGBA c, uint8_t out[4]) {
    auto q = [](float v) { return (uint8_t)(int)(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    out[0] = q(c.r); out[1] = q(c.g); out[2] = q(c.b); out[3] = q(c.a);
}

inline uint16_t aux_thickness(float t) { return (uint16_t)std::clamp((int)std::lround(t * 64.f), 0, 65535); }

GLuint compile(GLenum type, const char* src, std::string* err) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        if (err) *err = std::string("shader compile failed: ") + std::string(log, n);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// Decodes one UTF-8 code point; advances i. Invalid bytes decode as U+FFFD.
char32_t utf8_next(std::string_view s, size_t& i) {
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (n == 0) return 0xFFFD;
    char32_t cp = c & (0x3F >> n);
    for (int k = 0; k < n; ++k) {
        if (i >= s.size() || ((unsigned char)s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    }
    return cp;
}

// Box drawing arm table for U+2500..U+257F: "LRUD" with 0 none, 1 light, 2 heavy, 3 double.
// Entries for dashes/arcs/diagonals carry the base style and are special-cased by code point.
const char* const kArms[128] = {
    "1100", "2200", "0011", "0022", "1100", "2200", "0011", "0022", "1100", "2200", "0011", "0022", "0101", "0201", "0102", "0202", // 2500
    "1001", "2001", "1002", "2002", "0110", "0210", "0120", "0220", "1010", "2010", "1020", "2020", "0111", "0211", "0121", "0112", // 2510
    "0122", "0221", "0212", "0222", "1011", "2011", "1021", "1012", "1022", "2021", "2012", "2022", "1101", "2101", "1201", "2201", // 2520
    "1102", "2102", "1202", "2202", "1110", "2110", "1210", "2210", "1120", "2120", "1220", "2220", "1111", "2111", "1211", "2211", // 2530
    "1121", "1112", "1122", "2121", "1221", "2112", "1212", "2221", "2212", "2122", "1222", "2222", "1100", "2200", "0011", "0022", // 2540
    "3300", "0033", "0301", "0103", "0303", "3001", "1003", "3003", "0310", "0130", "0330", "3010", "1030", "3030", "0311", "0133", // 2550
    "0333", "3011", "1033", "3033", "3301", "1103", "3303", "3310", "1130", "3330", "3311", "1133", "3333", "0101", "1001", "1010", // 2560
    "0110", "0000", "0000", "0000", "1000", "0010", "0100", "0001", "2000", "0020", "0200", "0002", "1200", "0012", "2100", "0021", // 2570
};

} // namespace

static inline void set_vertex(Vertex& v, float x, float y, float u, float t, const uint8_t rgba[4], const float p[4], uint16_t mode, uint16_t aux) {
    v.x = x; v.y = y; v.u = u; v.v = t;
    std::memcpy(v.rgba, rgba, 4);
    v.p[0] = p[0]; v.p[1] = p[1]; v.p[2] = p[2]; v.p[3] = p[3];
    v.mode = mode; v.aux = aux;
}

struct Renderer::Impl {
    // Axis-aligned solid rectangle in screen pixels.
    static void emit_rect_px(Renderer& r, Impl& im, float x0, float y0, float x1, float y1, RGBA c) {
        if (x1 <= x0 || y1 <= y0) return;
        uint8_t col[4];
        pack_color(c, col);
        static const float zero[4] = {0, 0, 0, 0};
        Vertex* v = im.alloc_quad(r);
        set_vertex(v[0], x0, y0, 0, 0, col, zero, M_SOLID, 0);
        set_vertex(v[1], x1, y0, 0, 0, col, zero, M_SOLID, 0);
        set_vertex(v[2], x1, y1, 0, 0, col, zero, M_SOLID, 0);
        set_vertex(v[3], x0, y1, 0, 0, col, zero, M_SOLID, 0);
    }

    // Quad covering [x0,x1]x[y0,y1] in screen px with SDF local coords relative to (cx, cy).
    static void emit_sdf_quad(Renderer& r, Impl& im, float x0, float y0, float x1, float y1, float cx, float cy,
                                     const float p[4], uint16_t mode, uint16_t aux, RGBA c) {
        if (x1 <= x0 || y1 <= y0) return;
        uint8_t col[4];
        pack_color(c, col);
        Vertex* v = im.alloc_quad(r);
        set_vertex(v[0], x0, y0, x0 - cx, y0 - cy, col, p, mode, aux);
        set_vertex(v[1], x1, y0, x1 - cx, y0 - cy, col, p, mode, aux);
        set_vertex(v[2], x1, y1, x1 - cx, y1 - cy, col, p, mode, aux);
        set_vertex(v[3], x0, y1, x0 - cx, y1 - cy, col, p, mode, aux);
    }

    // Rounded rectangle given in screen px (SDF, anti-aliased). thickness > 0 => stroke.
    static void emit_rbox_px(Renderer& r, Impl& im, float x0, float y0, float x1, float y1, float radius, float thickness, RGBA c) {
        float hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
        if (hw <= 0 || hh <= 0) return;
        float cx = x0 + hw, cy = y0 + hh;
        radius = std::clamp(radius, 0.f, std::min(hw, hh));
        float p[4] = {hw, hh, radius, 0.f};
        Impl::emit_sdf_quad(r, im, x0 - 1, y0 - 1, x1 + 1, y1 + 1, cx, cy, p, thickness > 0 ? M_RBOX_STROKE : M_RBOX, aux_thickness(thickness), c);
    }

    // Anti-aliased capsule line in screen px (axis-aligned lines are snapped when snap is set).
    static void emit_line_px(Renderer& r, Impl& im, float x0, float y0, float x1, float y1, float t, RGBA c) {
        float dx = x1 - x0, dy = y1 - y0;
        float len = std::sqrt(dx * dx + dy * dy);
        if (im.snap && (dx == 0.f || dy == 0.f)) {
            float tt = std::max(1.f, std::round(t));
            if (dy == 0.f) {
                float ys = std::floor(y0 - tt * 0.5f + 0.5f);
                Impl::emit_rect_px(r, im, std::round(std::min(x0, x1)), ys, std::round(std::max(x0, x1)), ys + tt, c);
            } else {
                float xs = std::floor(x0 - tt * 0.5f + 0.5f);
                Impl::emit_rect_px(r, im, xs, std::round(std::min(y0, y1)), xs + tt, std::round(std::max(y0, y1)), c);
            }
            return;
        }
        if (len < 1e-4f) {
            Impl::emit_rbox_px(r, im, x0 - t * 0.5f, y0 - t * 0.5f, x0 + t * 0.5f, y0 + t * 0.5f, t * 0.5f, 0.f, c);
            return;
        }
        float ux = dx / len, uy = dy / len;        // along
        float nx = -uy, ny = ux;                   // across
        float hl = len * 0.5f + 1.f, ht = t * 0.5f + 1.f;   // AA margin
        float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
        uint8_t col[4];
        pack_color(c, col);
        float p[4] = {len * 0.5f, t * 0.5f, t * 0.5f, 0.f};
        Vertex* v = im.alloc_quad(r);
        // corners in the line frame: (-hl,-ht), (hl,-ht), (hl,ht), (-hl,ht)
        set_vertex(v[0], cx - ux * hl - nx * ht, cy - uy * hl - ny * ht, -hl, -ht, col, p, M_RBOX, 0);
        set_vertex(v[1], cx + ux * hl - nx * ht, cy + uy * hl - ny * ht, hl, -ht, col, p, M_RBOX, 0);
        set_vertex(v[2], cx + ux * hl + nx * ht, cy + uy * hl + ny * ht, hl, ht, col, p, M_RBOX, 0);
        set_vertex(v[3], cx - ux * hl + nx * ht, cy - uy * hl + ny * ht, -hl, ht, col, p, M_RBOX, 0);
    }


    GLuint vao = 0, ibo = 0, prog = 0;
    GLuint vbos[kVboRing] = {};
    int vbo_index = 0;
    GLint u_viewport = -1, u_tex = -1, u_tex_size = -1, u_gamma = -1;
    std::vector<Vertex> verts;
    int nq = 0;                         // quads in the pending batch
    GLuint batch_tex = 0;               // 0 => no texture needed yet
    Font* batch_font = nullptr;
    int batch_tex_size = 1;
    GLuint mip_tex = 0;                 // texture whose mipmaps were last generated
    int fb_w = 0, fb_h = 0;
    std::vector<FRect> clips;           // screen-space clip stack
    IRect scissor{0, 0, 0, 0};
    float gamma = 1.f / 1.2f;
    bool snap = true;                   // transform scale == 1: snap glyphs/lines to pixels
    bool gl_ok = false;

    GLsizeiptr vbo_offset = 0;          // ring-buffer write position (bytes)

    void set_attrib_pointers(GLsizeiptr base) {
        const GLsizei stride = sizeof(Vertex);
        auto off = [&](size_t o) { return (const void*)(uintptr_t)(base + (GLsizeiptr)o); };
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, off(offsetof(Vertex, x)));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, off(offsetof(Vertex, u)));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, off(offsetof(Vertex, rgba)));
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, off(offsetof(Vertex, p)));
        glVertexAttribIPointer(4, 2, GL_UNSIGNED_SHORT, stride, off(offsetof(Vertex, mode)));
    }

    Vertex* alloc_quad(Renderer& r) {
        if (nq >= kMaxQuads) r.flush();
        Vertex* v = &verts[(size_t)nq * 4];
        ++nq;
        return v;
    }
};

Renderer::Renderer() : impl_(new Impl) {}

Renderer::~Renderer() {
    Impl& im = *impl_;
    if (im.gl_ok && glDeleteBuffers) {
        glDeleteBuffers(kVboRing, im.vbos);
        if (im.ibo) glDeleteBuffers(1, &im.ibo);
        if (im.vao) glDeleteVertexArrays(1, &im.vao);
        if (im.prog) glDeleteProgram(im.prog);
    }
    delete impl_;
}

bool Renderer::init(std::string* err) {
    Impl& im = *impl_;
    if (!glCreateShader) {
        if (err) *err = "GL not loaded";
        return false;
    }
    GLuint vs = compile(GL_VERTEX_SHADER, shaders::kVertex, err);
    if (!vs) return false;
    GLuint fs = compile(GL_FRAGMENT_SHADER, shaders::kFragment, err);
    if (!fs) { glDeleteShader(vs); return false; }
    im.prog = glCreateProgram();
    glAttachShader(im.prog, vs);
    glAttachShader(im.prog, fs);
    glLinkProgram(im.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(im.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetProgramInfoLog(im.prog, sizeof log, &n, log);
        if (err) *err = std::string("program link failed: ") + std::string(log, n);
        return false;
    }
    im.u_viewport = glGetUniformLocation(im.prog, "u_viewport");
    im.u_tex = glGetUniformLocation(im.prog, "u_tex");
    im.u_tex_size = glGetUniformLocation(im.prog, "u_tex_size");
    im.u_gamma = glGetUniformLocation(im.prog, "u_gamma");

    glGenVertexArrays(1, &im.vao);
    glGenBuffers(kVboRing, im.vbos);
    glGenBuffers(1, &im.ibo);
    glBindVertexArray(im.vao);
    for (int i = 0; i < kVboRing; ++i) {
        glBindBuffer(GL_ARRAY_BUFFER, im.vbos[i]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(Vertex) * 4 * kMaxQuads), nullptr, GL_STREAM_DRAW);
    }
    for (int i = 0; i < 5; ++i) glEnableVertexAttribArray(i);
    im.set_attrib_pointers(0);

    // Static quad index buffer.
    std::vector<uint32_t> idx((size_t)kMaxQuads * 6);
    for (uint32_t q = 0; q < (uint32_t)kMaxQuads; ++q) {
        uint32_t b = q * 4;
        uint32_t* o = &idx[(size_t)q * 6];
        o[0] = b; o[1] = b + 1; o[2] = b + 2; o[3] = b; o[4] = b + 2; o[5] = b + 3;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, im.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(uint32_t)), idx.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);

    im.verts.resize((size_t)kMaxQuads * 4);
    glUseProgram(im.prog);
    glUniform1i(im.u_tex, 0);
    glUniform1f(im.u_gamma, im.gamma);
    im.gl_ok = gl_check("Renderer::init");
    return im.gl_ok;
}

void Renderer::begin_frame(int fb_w, int fb_h, RGBA clear) {
    Impl& im = *impl_;
    im.fb_w = fb_w;
    im.fb_h = fb_h;
    draw_calls_ = 0;
    quads_ = 0;
    im.nq = 0;
    im.vbo_offset = 0;
    im.vbo_index = (im.vbo_index + 1) % kVboRing;
    im.batch_tex = 0;
    im.batch_font = nullptr;
    xf_ = Transform{};
    im.snap = true;
    im.clips.clear();
    im.clips.push_back({0.f, 0.f, (float)fb_w, (float)fb_h});
    im.scissor = {0, 0, fb_w, fb_h};

    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(clear.r * clear.a, clear.g * clear.a, clear.b * clear.a, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, fb_w, fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(im.prog);
    glUniform2f(im.u_viewport, (float)fb_w, (float)fb_h);
    glUniform1f(im.u_gamma, im.gamma);
    glBindVertexArray(im.vao);
    glActiveTexture(GL_TEXTURE0);
}

void Renderer::end_frame() {
    flush();
    glBindVertexArray(0);
}

void Renderer::flush() {
    Impl& im = *impl_;
    if (im.nq == 0) return;
    using clk = std::chrono::steady_clock; auto T0 = clk::now(); bool regen_done = false;
    if (im.batch_tex) {
        glBindTexture(GL_TEXTURE_2D, im.batch_tex);
        bool regen = im.batch_font && im.batch_font->take_atlas_dirty();
        if (im.mip_tex != im.batch_tex) regen = true;
        if (regen) {
            glGenerateMipmap(GL_TEXTURE_2D);
            im.mip_tex = im.batch_tex; regen_done = true;
        }
        glUniform2f(im.u_tex_size, (float)im.batch_tex_size, (float)im.batch_tex_size);
    }
    // Upload: each frame appends into its own VBO of a kVboRing-deep ring, so the buffer being
    // written was last read by the GPU kVboRing frames ago and glBufferSubData does not stall.
    // Orphaning only happens if a single frame overflows its buffer.
    glBindBuffer(GL_ARRAY_BUFFER, im.vbos[im.vbo_index]);
    const GLsizeiptr cap = (GLsizeiptr)(sizeof(Vertex) * 4 * kMaxQuads);
    GLsizeiptr bytes = (GLsizeiptr)(sizeof(Vertex) * 4 * (size_t)im.nq);
    if (im.vbo_offset + bytes > cap) {
        glBufferData(GL_ARRAY_BUFFER, cap, nullptr, GL_STREAM_DRAW);   // orphan
        im.vbo_offset = 0;
    }
    auto T1 = clk::now();
    glBufferSubData(GL_ARRAY_BUFFER, im.vbo_offset, bytes, im.verts.data());
    auto T2 = clk::now();
    im.set_attrib_pointers(im.vbo_offset);
    glDrawElements(GL_TRIANGLES, (GLsizei)(im.nq * 6), GL_UNSIGNED_INT, nullptr);
    auto T3 = clk::now();
    auto ms = [](clk::time_point a, clk::time_point b){ return std::chrono::duration<double, std::milli>(b - a).count(); };
    if (getenv("CANOPY_FLUSH_DEBUG") && im.nq > 10000) std::fprintf(stderr, "flush %d q: pre %.2f (regen %d) sub %.2f draw %.2f\n", im.nq, ms(T0,T1), (int)regen_done, ms(T1,T2), ms(T2,T3));
    im.vbo_offset += bytes;
    ++draw_calls_;
    im.nq = 0;
}

void Renderer::set_transform(const Transform& t) {
    xf_ = t;
    impl_->snap = std::fabs(t.scale - 1.f) < 1e-6f;
}

void Renderer::push_clip(Rect r) {
    Impl& im = *impl_;
    FRect s{xf_.sx(r.x), xf_.sy(r.y), xf_.sx(r.x + r.w), xf_.sy(r.y + r.h)};
    const FRect& top = im.clips.back();
    s.x0 = std::max(s.x0, top.x0); s.y0 = std::max(s.y0, top.y0);
    s.x1 = std::min(s.x1, top.x1); s.y1 = std::min(s.y1, top.y1);
    if (s.x1 < s.x0) s.x1 = s.x0;
    if (s.y1 < s.y0) s.y1 = s.y0;
    im.clips.push_back(s);
    IRect sc{(int)std::floor(s.x0), (int)std::floor(s.y0), (int)std::ceil(s.x1), (int)std::ceil(s.y1)};
    if (std::memcmp(&sc, &im.scissor, sizeof sc) != 0) {
        flush();
        im.scissor = sc;
        glScissor(sc.x0, im.fb_h - sc.y1, std::max(0, sc.x1 - sc.x0), std::max(0, sc.y1 - sc.y0));
    }
}

void Renderer::pop_clip() {
    Impl& im = *impl_;
    if (im.clips.size() <= 1) return;
    im.clips.pop_back();
    const FRect& s = im.clips.back();
    IRect sc{(int)std::floor(s.x0), (int)std::floor(s.y0), (int)std::ceil(s.x1), (int)std::ceil(s.y1)};
    if (std::memcmp(&sc, &im.scissor, sizeof sc) != 0) {
        flush();
        im.scissor = sc;
        glScissor(sc.x0, im.fb_h - sc.y1, std::max(0, sc.x1 - sc.x0), std::max(0, sc.y1 - sc.y0));
    }
}

// ---- public primitives ---------------------------------------------------------------

void Renderer::fill_rect(Rect rc, RGBA c) {
    Impl::emit_rect_px(*this, *impl_, xf_.sx(rc.x), xf_.sy(rc.y), xf_.sx(rc.x + rc.w), xf_.sy(rc.y + rc.h), c);
    ++quads_;
}

void Renderer::fill_rect_gradient(Rect rc, RGBA top, RGBA bottom) {
    float x0 = xf_.sx(rc.x), y0 = xf_.sy(rc.y), x1 = xf_.sx(rc.x + rc.w), y1 = xf_.sy(rc.y + rc.h);
    if (x1 <= x0 || y1 <= y0) return;
    uint8_t ct[4], cb[4];
    pack_color(top, ct);
    pack_color(bottom, cb);
    static const float zero[4] = {0, 0, 0, 0};
    Vertex* v = impl_->alloc_quad(*this);
    set_vertex(v[0], x0, y0, 0, 0, ct, zero, M_SOLID, 0);
    set_vertex(v[1], x1, y0, 0, 0, ct, zero, M_SOLID, 0);
    set_vertex(v[2], x1, y1, 0, 0, cb, zero, M_SOLID, 0);
    set_vertex(v[3], x0, y1, 0, 0, cb, zero, M_SOLID, 0);
    ++quads_;
}

void Renderer::stroke_rect(Rect rc, RGBA c, float thickness) {
    float t = thickness * xf_.scale;
    float x0 = xf_.sx(rc.x), y0 = xf_.sy(rc.y), x1 = xf_.sx(rc.x + rc.w), y1 = xf_.sy(rc.y + rc.h);
    if (impl_->snap) {
        t = std::max(1.f, std::round(t));
        x0 = std::round(x0); y0 = std::round(y0); x1 = std::round(x1); y1 = std::round(y1);
    }
    Impl::emit_rect_px(*this, *impl_, x0, y0, x1, y0 + t, c);
    Impl::emit_rect_px(*this, *impl_, x0, y1 - t, x1, y1, c);
    Impl::emit_rect_px(*this, *impl_, x0, y0 + t, x0 + t, y1 - t, c);
    Impl::emit_rect_px(*this, *impl_, x1 - t, y0 + t, x1, y1 - t, c);
    quads_ += 4;
}

void Renderer::rounded_rect(Rect rc, float radius, RGBA fill) {
    Impl::emit_rbox_px(*this, *impl_, xf_.sx(rc.x), xf_.sy(rc.y), xf_.sx(rc.x + rc.w), xf_.sy(rc.y + rc.h), radius * xf_.scale, 0.f, fill);
    ++quads_;
}

void Renderer::rounded_rect_stroke(Rect rc, float radius, RGBA c, float thickness) {
    Impl::emit_rbox_px(*this, *impl_, xf_.sx(rc.x), xf_.sy(rc.y), xf_.sx(rc.x + rc.w), xf_.sy(rc.y + rc.h), radius * xf_.scale,
                 std::max(0.01f, thickness * xf_.scale), c);
    ++quads_;
}

void Renderer::line(float x0, float y0, float x1, float y1, RGBA c, float thickness) {
    Impl::emit_line_px(*this, *impl_, xf_.sx(x0), xf_.sy(y0), xf_.sx(x1), xf_.sy(y1), thickness * xf_.scale, c);
    ++quads_;
}

void Renderer::circle(float cx, float cy, float radius, RGBA c) {
    rounded_rect({cx - radius, cy - radius, 2 * radius, 2 * radius}, radius, c);
}

// ---- glyphs --------------------------------------------------------------------------

void Renderer::draw_glyph(Font& font, const Glyph& g, float x, float baseline_y, RGBA color, float scale) {
    Impl& im = *impl_;
    if (g.aw <= 0 || g.ah <= 0) return;
    GLuint tex = font.atlas_texture();
    if (tex != im.batch_tex) {
        if (im.nq) flush();
        im.batch_tex = tex;
        im.batch_font = &font;
    }
    im.batch_tex_size = font.atlas_size();
    float eff = xf_.scale * scale;
    float px, py, w, h;
    if (std::fabs(eff - 1.f) < 1e-6f) {
        px = std::round(xf_.sx(x)) + g.bearing_x;
        py = std::round(xf_.sy(baseline_y)) - g.bearing_y;
        w = (float)g.aw;
        h = (float)g.ah;
    } else {
        px = xf_.sx(x) + g.bearing_x * eff;
        py = xf_.sy(baseline_y) - g.bearing_y * eff;
        w = g.aw * eff;
        h = g.ah * eff;
    }
    uint8_t col[4];
    pack_color(color, col);
    static const float zero[4] = {0, 0, 0, 0};
    uint16_t mode = g.color ? M_COLOR : M_MASK;
    float u0 = (float)g.ax, v0 = (float)g.ay, u1 = (float)(g.ax + g.aw), v1 = (float)(g.ay + g.ah);
    Vertex* v = im.alloc_quad(*this);
    set_vertex(v[0], px, py, u0, v0, col, zero, mode, 0);
    set_vertex(v[1], px + w, py, u1, v0, col, zero, mode, 0);
    set_vertex(v[2], px + w, py + h, u1, v1, col, zero, mode, 0);
    set_vertex(v[3], px, py + h, u0, v1, col, zero, mode, 0);
    ++quads_;
}

float Renderer::draw_text(Font& font, float x, float baseline_y, std::string_view utf8, RGBA color, GlyphStyle style, float scale) {
    const FontMetrics& m = font.metrics();
    float pen = x;
    size_t i = 0;
    while (i < utf8.size()) {
        char32_t cp = utf8_next(utf8, i);
        if (Font::is_box_drawing(cp)) {
            Rect cell{pen, baseline_y - m.ascent * scale, m.cell_w * scale, m.cell_h * scale};
            draw_box_char(cp, cell, color, std::max(1.f, (float)m.underline_thickness) * scale);
            pen += m.cell_w * scale;
            continue;
        }
        const Glyph& g = font.glyph(cp, style);
        draw_glyph(font, g, pen, baseline_y, color, scale);
        pen += g.advance * scale;
    }
    return pen - x;
}

float Renderer::measure_text(Font& font, std::string_view utf8, GlyphStyle style, float scale) {
    const FontMetrics& m = font.metrics();
    float w = 0.f;
    size_t i = 0;
    while (i < utf8.size()) {
        char32_t cp = utf8_next(utf8, i);
        if (Font::is_box_drawing(cp)) { w += m.cell_w * scale; continue; }
        w += font.glyph(cp, style).advance * scale;
    }
    return w;
}

// ---- procedural box drawing ------------------------------------------------------------

void Renderer::draw_box_char(char32_t cp, Rect cell, RGBA color, float thickness) {
    Impl& im = *impl_;
    Renderer& R = *this;
    const bool snap = im.snap;
    // Cell in screen pixels.
    float X = xf_.sx(cell.x), Y = xf_.sy(cell.y);
    float X1 = xf_.sx(cell.x + cell.w), Y1 = xf_.sy(cell.y + cell.h);
    float T = thickness * xf_.scale;
    if (snap) { X = std::round(X); Y = std::round(Y); X1 = std::round(X1); Y1 = std::round(Y1); T = std::max(1.f, std::round(T)); }
    const float W = X1 - X, H = Y1 - Y;
    if (W <= 0 || H <= 0) return;
    const float cx = X + W * 0.5f, cy = Y + H * 0.5f;
    const float t = T;
    const float th = snap ? (T < 1.5f ? 3.f : 2.f * T) : (T < 1.5f ? 3.f * T : 2.f * T);
    auto thick = [&](int s) { return s == 2 ? th : t; };
    // Band start for a line of thickness tt centred on c.
    auto bs = [&](float c, float tt) { return snap ? std::floor(c - tt * 0.5f + 0.5f) : c - tt * 0.5f; };
    auto fill = [&](float x0, float y0, float x1, float y1) { Impl::emit_rect_px(R, im, x0, y0, x1, y1, color); ++quads_; };
    auto hband = [&](float x0, float x1, float yc, float tt) { float y = bs(yc, tt); fill(x0, y, x1, y + tt); };
    auto vband = [&](float y0, float y1, float xc, float tt) { float x = bs(xc, tt); fill(x, y0, x + tt, y1); };

    // Block elements -------------------------------------------------------------------
    if (cp >= 0x2580 && cp <= 0x259F) {
        auto rnd = [&](float v) { return snap ? std::round(v) : v; };
        float hw = rnd(W * 0.5f), hh = rnd(H * 0.5f);
        if (cp == 0x2580) { fill(X, Y, X1, Y + hh); return; }
        if (cp >= 0x2581 && cp <= 0x2588) { int n = (int)(cp - 0x2580); float h = std::max(1.f, rnd(H * n / 8.f)); fill(X, Y1 - h, X1, Y1); return; }
        if (cp >= 0x2589 && cp <= 0x258F) { int n = 8 - (int)(cp - 0x2588); float w = std::max(1.f, rnd(W * n / 8.f)); fill(X, Y, X + w, Y1); return; }
        if (cp == 0x2590) { fill(X1 - hw, Y, X1, Y1); return; }
        if (cp >= 0x2591 && cp <= 0x2593) {
            RGBA c = color;
            c.a *= (cp == 0x2591) ? 0.25f : (cp == 0x2592) ? 0.5f : 0.75f;
            Impl::emit_rect_px(R, im, X, Y, X1, Y1, c);
            ++quads_;
            return;
        }
        if (cp == 0x2594) { fill(X, Y, X1, Y + std::max(1.f, rnd(H / 8.f))); return; }
        if (cp == 0x2595) { fill(X1 - std::max(1.f, rnd(W / 8.f)), Y, X1, Y1); return; }
        // Quadrants: bit0 UL, bit1 UR, bit2 LL, bit3 LR
        static const uint8_t quad[10] = {0x4, 0x8, 0x1, 0xD, 0x9, 0x7, 0xB, 0x2, 0x6, 0xE};
        uint8_t q = quad[cp - 0x2596];
        if (q & 1) fill(X, Y, X + hw, Y + hh);
        if (q & 2) fill(X + hw, Y, X1, Y + hh);
        if (q & 4) fill(X, Y + hh, X + hw, Y1);
        if (q & 8) fill(X + hw, Y + hh, X1, Y1);
        return;
    }

    // Powerline ------------------------------------------------------------------------
    if (cp >= 0xE0B0 && cp <= 0xE0BF) {
        auto wedge = [&](float ox, float oy, float ax, float ay, float bx, float by, float ix, float iy, bool stroke) {
            // Two edges from origin O towards A and B; I is a point inside.
            auto normal = [&](float ex, float ey, float* nx, float* ny) {
                float l = std::sqrt(ex * ex + ey * ey);
                if (l < 1e-6f) { *nx = 0; *ny = 0; return; }
                float px = -ey / l, py = ex / l;
                if (px * (ix - ox) + py * (iy - oy) > 0) { px = -px; py = -py; }
                *nx = px; *ny = py;
            };
            float p[4];
            normal(ax - ox, ay - oy, &p[0], &p[1]);
            normal(bx - ox, by - oy, &p[2], &p[3]);
            Impl::emit_sdf_quad(R, im, X, Y, X1, Y1, ox, oy, p, stroke ? M_WEDGE_STROKE : M_WEDGE, aux_thickness(t), color);
            ++quads_;
        };
        auto halfcircle = [&](bool right, bool stroke) {
            float ccx = right ? X : X1;
            float r = std::min(W, H * 0.5f);
            float p[4] = {W, H * 0.5f, r, 0.f};
            float q0 = right ? X : X - 1, q1 = right ? X1 + 1 : X1;
            Impl::emit_sdf_quad(R, im, q0, Y - 1, q1, Y1 + 1, ccx, cy, p, stroke ? M_RBOX_STROKE : M_RBOX, aux_thickness(t), color);
            ++quads_;
        };
        switch (cp) {
        case 0xE0B0: wedge(X1, cy, X, Y, X, Y1, X, cy, false); break;
        case 0xE0B1: wedge(X1, cy, X, Y, X, Y1, X, cy, true); break;
        case 0xE0B2: wedge(X, cy, X1, Y, X1, Y1, X1, cy, false); break;
        case 0xE0B3: wedge(X, cy, X1, Y, X1, Y1, X1, cy, true); break;
        case 0xE0B4: halfcircle(true, false); break;
        case 0xE0B5: halfcircle(true, true); break;
        case 0xE0B6: halfcircle(false, false); break;
        case 0xE0B7: halfcircle(false, true); break;
        case 0xE0B8: wedge(X, Y, X1, Y1, X1, Y1, X, Y1, false); break;     // lower-left triangle
        case 0xE0B9: wedge(X, Y, X1, Y1, X1, Y1, X, Y1, true); break;
        case 0xE0BA: wedge(X1, Y, X, Y1, X, Y1, X1, Y1, false); break;     // lower-right
        case 0xE0BB: wedge(X1, Y, X, Y1, X, Y1, X1, Y1, true); break;
        case 0xE0BC: wedge(X1, Y, X, Y1, X, Y1, X, Y, false); break;       // upper-left
        case 0xE0BD: wedge(X1, Y, X, Y1, X, Y1, X, Y, true); break;
        case 0xE0BE: wedge(X, Y, X1, Y1, X1, Y1, X1, Y, false); break;     // upper-right
        case 0xE0BF: wedge(X, Y, X1, Y1, X1, Y1, X1, Y, true); break;
        default: break;
        }
        return;
    }

    if (cp < 0x2500 || cp > 0x257F) return;

    // Arcs -----------------------------------------------------------------------------
    if (cp >= 0x256D && cp <= 0x2570) {
        float cxm = bs(cx, t) + t * 0.5f, cym = bs(cy, t) + t * 0.5f;   // exact line centres
        float r = std::min(std::min(cxm - X, X1 - cxm), std::min(cym - Y, Y1 - cym));
        bool right = (cp == 0x256D || cp == 0x2570), down = (cp == 0x256D || cp == 0x256E);
        float ccx = right ? cxm + r : cxm - r;
        float ccy = down ? cym + r : cym - r;
        if (right) hband(ccx, X1, cy, t); else hband(X, ccx, cy, t);
        if (down) vband(ccy, Y1, cx, t); else vband(Y, ccy, cx, t);
        float p[4] = {r, r, r, 0.f};
        float qx0 = right ? ccx - r - 1 : ccx, qx1 = right ? ccx : ccx + r + 1;
        float qy0 = down ? ccy - r - 1 : ccy, qy1 = down ? ccy : ccy + r + 1;
        Impl::emit_sdf_quad(R, im, qx0, qy0, qx1, qy1, ccx, ccy, p, M_RBOX_STROKE, aux_thickness(t), color);
        ++quads_;
        return;
    }
    // Diagonals --------------------------------------------------------------------------
    if (cp >= 0x2571 && cp <= 0x2573) {
        if (cp != 0x2572) { Impl::emit_line_px(R, im, X1, Y, X, Y1, t, color); ++quads_; }
        if (cp != 0x2571) { Impl::emit_line_px(R, im, X, Y, X1, Y1, t, color); ++quads_; }
        return;
    }
    // Dashed -------------------------------------------------------------------------------
    if ((cp >= 0x2504 && cp <= 0x250B) || (cp >= 0x254C && cp <= 0x254F)) {
        int n = (cp >= 0x254C) ? 2 : (cp >= 0x2508) ? 4 : 3;
        bool heavy = (cp & 1) != 0;
        bool vertical = ((cp - 0x2504) & 2) != 0;
        float tt = heavy ? th : t;
        float len = vertical ? H : W;
        float slot = len / n, gap = slot / 3.f;
        for (int i = 0; i < n; ++i) {
            float a = i * slot + gap * 0.5f, b = (i + 1) * slot - gap * 0.5f;
            if (snap) { a = std::round(a); b = std::round(b); if (b <= a) b = a + 1; }
            if (vertical) vband(Y + a, Y + b, cx, tt); else hband(X + a, X + b, cy, tt);
        }
        return;
    }

    // Straight arms -----------------------------------------------------------------------
    const char* arms = kArms[cp - 0x2500];
    int L = arms[0] - '0', Rr = arms[1] - '0', U = arms[2] - '0', D = arms[3] - '0';
    bool hd = (L == 3 || Rr == 3), vd = (U == 3 || D == 3);
    const float o = 2.f * t;   // offset of double lines from the centre
    float ya = bs(cy - o, t), yb = bs(cy + o, t);      // double horizontal bands (top, bottom)
    float xa = bs(cx - o, t), xb = bs(cx + o, t);      // double vertical bands (left, right)

    if (!hd && !vd) {
        float tmax = 0.f;
        for (int s : {L, Rr, U, D}) if (s) tmax = std::max(tmax, thick(s));
        float csx0 = bs(cx, tmax), csx1 = csx0 + tmax, csy0 = bs(cy, tmax), csy1 = csy0 + tmax;
        if (L) hband(X, csx1, cy, thick(L));
        if (Rr) hband(csx0, X1, cy, thick(Rr));
        if (U) vband(Y, csy1, cx, thick(U));
        if (D) vband(csy0, Y1, cx, thick(D));
        return;
    }
    if (hd && !vd) {
        bool vp = U || D;
        float tv = thick(std::max(U, D));
        float xc = bs(cx, tv);
        float x0 = L ? X : (vp ? xc : cx), x1 = Rr ? X1 : (vp ? xc + tv : cx);
        fill(x0, ya, x1, ya + t);
        fill(x0, yb, x1, yb + t);
        if (U) vband(Y, D ? Y1 : ya + t, cx, thick(U));
        if (D) vband(U ? Y : yb, Y1, cx, thick(D));
        return;
    }
    if (vd && !hd) {
        bool hp = L || Rr;
        float tH = thick(std::max(L, Rr));
        float yc = bs(cy, tH);
        float y0 = U ? Y : (hp ? yc : cy), y1 = D ? Y1 : (hp ? yc + tH : cy);
        fill(xa, y0, xa + t, y1);
        fill(xb, y0, xb + t, y1);
        if (L) hband(X, Rr ? X1 : ((U && D) ? xa + t : xb + t), cy, thick(L));
        if (Rr) hband(L ? X : ((U && D) ? xb : xa), X1, cy, thick(Rr));
        return;
    }
    // Double in both axes.
    auto hline = [&](float y, bool outer) {
        if (!outer) {   // inner (or crossed): stop at the near vertical line
            if (L && Rr) { fill(X, y, xa + t, y + t); fill(xb, y, X1, y + t); }
            else if (L) fill(X, y, xa + t, y + t);
            else if (Rr) fill(xb, y, X1, y + t);
        } else {
            if (L && Rr) fill(X, y, X1, y + t);
            else if (L) fill(X, y, xb + t, y + t);
            else if (Rr) fill(xa, y, X1, y + t);
        }
    };
    auto vline = [&](float x, bool outer) {
        if (!outer) {
            if (U && D) { fill(x, Y, x + t, ya + t); fill(x, yb, x + t, Y1); }
            else if (U) fill(x, Y, x + t, ya + t);
            else if (D) fill(x, yb, x + t, Y1);
        } else {
            if (U && D) fill(x, Y, x + t, Y1);
            else if (U) fill(x, Y, x + t, yb + t);
            else if (D) fill(x, ya, x + t, Y1);
        }
    };
    hline(ya, D && !U);   // top line is outer only when the vertical goes down only
    hline(yb, U && !D);
    vline(xa, Rr && !L);
    vline(xb, L && !Rr);
}

} // namespace canopy::gfx
