#version 450
// A separable Gaussian blur along one axis, with a vignette.

#define TAPS 5

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D image;
layout(set = 0, binding = 1) uniform Params {
    vec2 direction;     // (1, 0) or (0, 1), in texels
    float vignette;
} params;

const float weights[TAPS] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

float vignetteOf(vec2 p) {
    vec2 d = p - 0.5;
    return clamp(1.0 - dot(d, d) * params.vignette, 0.0, 1.0);
}

void main() {
    vec2 texel = 1.0 / vec2(textureSize(image, 0));
    vec3 sum = texture(image, uv).rgb * weights[0];
    for (int i = 1; i < TAPS; ++i) {
        vec2 offset = params.direction * texel * float(i);
        sum += texture(image, uv + offset).rgb * weights[i];
        sum += texture(image, uv - offset).rgb * weights[i];
    }
    color = vec4(sum * vignetteOf(uv), 1.0);
}
