#version 440
// Liquid Glass inside the window: what lies behind, blurred and bent toward the middle near the rim, with a slight colour
// split at the edge, a lift in saturation and a bright rim. The shape is a rounded rectangle.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float radius;
    float strength;
    vec4 tint;
};
layout(binding = 1) uniform sampler2D source;

float box(vec2 p, vec2 half_size, float r) {
    vec2 q = abs(p) - half_size + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

vec3 blurred(vec2 uv) {
    vec2 texel = 1.0 / size;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int ring = 0; ring < 3; ++ring) {
        float reach = 3.0 + 4.0 * float(ring);
        for (int i = 0; i < 8; ++i) {
            float a = float(i) * 0.7853981 + float(ring);
            float w = 1.0 / (1.0 + float(ring));
            sum += texture(source, uv + vec2(cos(a), sin(a)) * reach * texel).rgb * w;
            total += w;
        }
    }
    return sum / total;
}

void main() {
    vec2 p = (qt_TexCoord0 - 0.5) * size;
    float d = box(p, size * 0.5, radius);
    float inside = 1.0 - smoothstep(-1.0, 0.0, d);
    float rim = 1.0 - smoothstep(0.0, 14.0, -d);
    vec2 bend = -normalize(p + 0.0001) * rim * rim * 10.0 * strength / size;
    vec3 c = vec3(blurred(qt_TexCoord0 + bend * 1.08).r, blurred(qt_TexCoord0 + bend).g, blurred(qt_TexCoord0 + bend * 0.92).b);
    float grey = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(grey), c, 1.0 + 0.3 * strength);
    c = mix(c, tint.rgb, tint.a);
    c += rim * rim * 0.16 * strength;
    fragColor = vec4(c, 1.0) * inside * qt_Opacity;
}
