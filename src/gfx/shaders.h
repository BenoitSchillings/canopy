#pragma once
// GLSL 3.30 core sources for the batched 2D renderer (one program for everything).
//
// Vertex modes (a_mode.x):
//   0 solid color            2 color glyph (premultiplied RGBA from atlas)
//   1 alpha-mask glyph       3 rounded-rect fill (SDF)   4 rounded-rect stroke (SDF ring)
//   5 wedge fill (intersection of two half planes through the local origin)
//   6 wedge stroke (two lines through the local origin)
// For SDF modes a_uv carries the fragment position relative to the shape center, in pixels;
// a_param is (half_w, half_h, radius, -) for 3/4 and (n1.x, n1.y, n2.x, n2.y) for 5/6;
// a_mode.y is the stroke thickness in 1/64 px.

namespace canopy::gfx::shaders {

static const char* kVertex = R"GLSL(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in vec4 a_param;
layout(location = 4) in ivec2 a_mode;
uniform vec2 u_viewport;
out vec2 v_uv;
out vec4 v_color;
flat out vec4 v_param;
flat out ivec2 v_mode;
void main() {
    v_uv = a_uv;
    v_color = a_color;
    v_param = a_param;
    v_mode = a_mode;
    gl_Position = vec4(a_pos.x * 2.0 / u_viewport.x - 1.0, 1.0 - a_pos.y * 2.0 / u_viewport.y, 0.0, 1.0);
}
)GLSL";

static const char* kFragment = R"GLSL(#version 330 core
in vec2 v_uv;
in vec4 v_color;
flat in vec4 v_param;
flat in ivec2 v_mode;
uniform sampler2D u_tex;
uniform vec2 u_tex_size;
uniform float u_gamma;      // exponent applied to mask coverage (< 1 thickens thin stems)
out vec4 o_color;

float sd_rbox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

void main() {
    int mode = v_mode.x;
    vec4 c = vec4(v_color.rgb * v_color.a, v_color.a);   // premultiply
    if (mode == 0) {
        o_color = c;
    } else if (mode == 1) {
        float a = texture(u_tex, v_uv / u_tex_size).a;
        a = pow(a, u_gamma);
        o_color = c * a;
    } else if (mode == 2) {
        o_color = texture(u_tex, v_uv / u_tex_size) * v_color.a;
    } else {
        float t = float(v_mode.y) / 64.0;
        float d;
        if (mode == 3) {
            d = sd_rbox(v_uv, v_param.xy, v_param.z);
        } else if (mode == 4) {
            d = abs(sd_rbox(v_uv, v_param.xy, v_param.z) + 0.5 * t) - 0.5 * t;
        } else {
            float d1 = dot(v_uv, v_param.xy);
            float d2 = dot(v_uv, v_param.zw);
            if (mode == 5) d = max(d1, d2);
            else d = min(abs(d1), abs(d2)) - 0.5 * t;
        }
        float cov = clamp(0.5 - d, 0.0, 1.0);
        o_color = c * cov;
    }
}
)GLSL";

} // namespace canopy::gfx::shaders
