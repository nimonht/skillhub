#version 440
// Liquid Glass inside the window, for the surfaces that float over the program's own content: toasts, menus, tooltips.
//
// What lies behind is seen through a lens: flat inside, bending over a bevel at the rim so the content there is pulled
// toward the middle, with a faint colour fringe where the bending is strongest. A light from the top left draws a bright
// line along the edges that face it and a weaker one on the opposite side, with a soft glow inside the rim and a shade
// on the far side for depth. The glass is frosted (a blur), a little more colourful than what is behind it, and tinted.
// Outside the shape a soft shadow falls a little downward.
//
// The item is larger than the shape by `pad` on every side, which is where the shadow goes. Written for GLSL 100 es and 120
// as well as 440: no derivatives, no texture lod, no array initialisers, no loop that depends on a uniform.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;      // of the whole item, shadow included
    float radius;   // of the shape's corners
    float strength; // 0 to 1: how much the glass bends and shines
    vec4 tint;      // colour and how much of it covers what is behind (Qt passes a colour premultiplied by its alpha)
    float pad;      // the margin around the shape that is left to the shadow
    float bevel;    // how far inside the rim the lens works
    float spec;     // how bright the edge lines are
};
layout(binding = 1) uniform sampler2D source;

float box(vec2 p, vec2 half_size, float r) {
    vec2 q = abs(p) - half_size + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

vec4 tap(vec2 uv, vec2 offset, vec2 texel) { return texture(source, uv + offset * texel); }

// thirteen taps in two rings: frosted, and cheap enough to draw under every toast and menu
vec4 frosted(vec2 uv, float reach) {
    vec2 texel = 1.0 / size;
    float a = reach;
    float b = reach * 2.0;
    vec4 sum = tap(uv, vec2(0.0), texel) * 2.0;
    sum += tap(uv, vec2(a, 0.0), texel) + tap(uv, vec2(-a, 0.0), texel);
    sum += tap(uv, vec2(a * 0.5, a * 0.866), texel) + tap(uv, vec2(-a * 0.5, a * 0.866), texel);
    sum += tap(uv, vec2(a * 0.5, -a * 0.866), texel) + tap(uv, vec2(-a * 0.5, -a * 0.866), texel);
    sum += tap(uv, vec2(b * 0.866, b * 0.5), texel) + tap(uv, vec2(-b * 0.866, b * 0.5), texel);
    sum += tap(uv, vec2(b * 0.866, -b * 0.5), texel) + tap(uv, vec2(-b * 0.866, -b * 0.5), texel);
    sum += tap(uv, vec2(0.0, b), texel) + tap(uv, vec2(0.0, -b), texel);
    return sum / 14.0;
}

void main() {
    vec2 p = (qt_TexCoord0 - 0.5) * size;
    vec2 half_shape = size * 0.5 - pad;
    float d = box(p, half_shape, radius);                 // negative inside the shape
    float inside = 1.0 - smoothstep(-0.75, 0.75, d);

    // the shadow: the shape moved down a little, blurred, and only where the shape is not
    float shade = 1.0 - smoothstep(-pad * 0.15, pad * 0.9, box(p - vec2(0.0, pad * 0.28), half_shape, radius));
    float shadow = shade * (1.0 - inside) * 0.34 * (0.4 + 0.6 * strength);

    // The direction out of the shape, from the slope of the distance. A shape with small corners has a normal that flips along the
    // diagonal of each corner, which would show as a mitred frame: the lens takes its bearings from a rounder shape inside it.
    float rb = min(max(radius, bevel * 0.9), min(half_shape.x, half_shape.y));
    float db = box(p, half_shape, rb);
    float e = 1.0;
    vec2 n = vec2(box(p + vec2(e, 0.0), half_shape, rb) - box(p - vec2(e, 0.0), half_shape, rb),
                  box(p + vec2(0.0, e), half_shape, rb) - box(p - vec2(0.0, e), half_shape, rb));
    n = n / max(length(n), 0.0001);

    // the lens: strongest at the rim, none beyond the bevel
    float t = clamp(-db / max(bevel, 1.0), 0.0, 1.0);
    float lens = (1.0 - t) * (1.0 - t);
    vec2 bend = -n * lens * (9.0 + 14.0 * strength) / size;

    vec2 uv = qt_TexCoord0 + bend;
    // The picture behind is premultiplied, and the program's panels are translucent when this glass is on: where it is not
    // opaque the tint stands in for the desktop, and a colour is never darkened by the alpha it was multiplied with.
    vec3 tint_rgb = tint.rgb / max(tint.a, 0.0001);
    vec4 back = frosted(uv, 2.0 + 2.0 * strength);
    vec3 c = mix(tint_rgb, back.rgb / max(back.a, 0.001), clamp(back.a, 0.0, 1.0));
    // a colour fringe, only where the lens bends: red from a little further in, blue from a little less
    float fringe = lens * lens * strength;
    vec4 red = texture(source, qt_TexCoord0 + bend * 1.35);
    vec4 blue = texture(source, qt_TexCoord0 + bend * 0.65);
    c.r = mix(c.r, red.a > 0.001 ? red.r / red.a : c.r, fringe);
    c.b = mix(c.b, blue.a > 0.001 ? blue.b / blue.a : c.b, fringe);

    // frosted glass is livelier than what is behind it
    float grey = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(grey), c, 1.0 + 0.45 * strength);
    c = mix(c, tint_rgb, tint.a);

    // light from the top left: a bright line on the edges that face it, a weaker one on the far side, and a glow inside the rim
    float facing = dot(n, normalize(vec2(-0.55, -0.83)));
    float edge = 1.0 - smoothstep(0.0, 1.7, -d);
    float glow = 1.0 - smoothstep(0.0, bevel * 0.7, -d);
    float lit = pow(max(facing, 0.0), 2.5);
    float far_side = pow(max(-facing, 0.0), 3.0);
    float line = edge * (0.10 + 0.9 * lit + 0.35 * far_side) * spec;
    c += vec3(1.0) * (line * 0.85 + glow * glow * (0.05 + 0.10 * lit) * spec);
    c *= 1.0 - 0.10 * far_side * glow * strength;          // the shade on the side away from the light

    vec4 glass = vec4(clamp(c, 0.0, 1.0), 1.0) * inside;
    fragColor = (glass + vec4(0.0, 0.0, 0.0, shadow)) * qt_Opacity;
}
