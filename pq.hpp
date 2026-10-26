#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace PQLut {
constexpr uint32_t firstBits = (127 - 40) << 23;
constexpr uint32_t stepBits = 1u << 16;
constexpr int count = 40 * 128 + 1;
constexpr float minimum = 0x1p-40f;
inline double exact(double x) {
    const auto e = std::pow(std::clamp(x, 0.0, 1.0), 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * e) / (1 + 2392.0 / 128.0 * e), 2523.0 / 32.0);
}
inline std::vector<float> samples() {
    std::vector<float> result(count);
    for (int i = 0; i < count; ++i)
        result[i] = exact(std::bit_cast<float>(firstBits + i * stepBits));
    return result;
}
inline float approximate(float value, const std::vector<float>& lut, bool quantizeWeight = false) {
    const float x = std::clamp(value, 0.0f, 1.0f);
    const auto bits = std::bit_cast<uint32_t>(std::max(x, minimum));
    const float index = static_cast<float>(bits - firstBits) / stepBits;
    const int lo = std::min(static_cast<int>(index), count - 2);
    float fraction = index - lo;
    if (quantizeWeight)
        fraction = std::round(fraction * 256) / 256;
    float result = std::lerp(lut[lo], lut[lo + 1], fraction);
    if (x < minimum)
        result = std::lerp(static_cast<float>(exact(0)), lut.front(), x / minimum);
    return result;
}
inline const std::string glsl = R"GLSL(
uniform highp sampler2D hdrPQLut;
vec3 tfPQ(vec3 color) {
    vec3 x = clamp(color, vec3(0.0), vec3(1.0));
    highp uvec3 bits = floatBitsToUint(max(x, vec3(0.0000000000009094947017729282)));
    vec3 index = vec3(bits - uvec3(729808896u)) / 65536.0;
    vec3 uv = (index + 0.5) / 5121.0;
    vec3 y = vec3(textureLod(hdrPQLut, vec2(uv.r, 0.5), 0.0).r,
                  textureLod(hdrPQLut, vec2(uv.g, 0.5), 0.0).r,
                  textureLod(hdrPQLut, vec2(uv.b, 0.5), 0.0).r);
    return mix(vec3(0.0000007309559025783966), y,
               min(x / 0.0000000000009094947017729282, vec3(1.0)));
}
)GLSL";
}
