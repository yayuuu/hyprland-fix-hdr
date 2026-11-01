#pragma once

#include <stdexcept>
#include <string>

namespace HDRFix {
inline constexpr const char* CAPTURE_GLSL = R"GLSL(
uniform sampler2D hdrfixSDRCopy;
uniform sampler2D hdrfixBlurCopy;
uniform bool hdrfixUseCopy[2];
uniform int hdrfixSourceTF[2];
uniform int hdrfixCopyTF[2];
uniform int hdrfixOutputTF;
uniform vec2 hdrfixSourceRange[2];
uniform vec2 hdrfixSDRRange;
uniform vec2 hdrfixInverseMods[2];
uniform mat3 hdrfixToSRGB[2];

vec3 hdrfixDecode(vec3 rgb, int tf) {
    if (tf == 11) { // ST2084 / PQ
        vec3 e = pow(clamp(rgb, 0.0, 1.0), vec3(1.0 / 78.84375));
        return pow(max(e - 0.8359375, 0.0) / max(18.8515625 - 18.6875 * e, 0.000001), vec3(1.0 / 0.1593017578125));
    }
    if (tf == 13) { // HLG
        vec3 lo = rgb * rgb / 3.0;
        vec3 hi = (exp((rgb - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
        return mix(hi, lo, lessThanEqual(rgb, vec3(0.5)));
    }
    if (tf == 2)
        return pow(max(rgb, 0.0), vec3(2.2));
    if (tf == 9)
        return mix(pow(max((rgb + 0.055) / 1.055, 0.0), vec3(2.4)), rgb / 12.92, lessThanEqual(rgb, vec3(0.04045)));
    return rgb; // linear nits or scRGB
}

vec3 hdrfixEncode(vec3 rgb) {
    if (hdrfixOutputTF == 2)
        return pow(max(rgb, 0.0), vec3(1.0 / 2.2));
    return mix(1.055 * pow(max(rgb, 0.0), vec3(1.0 / 2.4)) - 0.055, rgb * 12.92, lessThanEqual(rgb, vec3(0.0031308)));
}

vec4 hdrfixCapture(vec4 source, vec2 uv, int index) {
    if (hdrfixUseCopy[index]) {
        vec4 copy = index == 0 ? texture(hdrfixSDRCopy, uv) : texture(hdrfixBlurCopy, uv);
        if (copy.a <= 0.0)
            return vec4(0.0);
        // A native SDR copy is already mapped: only match the output encoding.
        if (hdrfixCopyTF[index] == hdrfixOutputTF)
            return copy;
        return vec4(hdrfixEncode(hdrfixDecode(copy.rgb / copy.a, hdrfixCopyTF[index])) * copy.a, copy.a);
    }
    if (source.a <= 0.0)
        return vec4(0.0);
    vec3 rgb = hdrfixToSRGB[index] * hdrfixDecode(max(source.rgb / source.a, 0.0), hdrfixSourceTF[index]);
    rgb = rgb * (hdrfixSourceRange[index].y - hdrfixSourceRange[index].x) + hdrfixSourceRange[index].x;
    rgb *= hdrfixInverseMods[index].x;
    float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(y), rgb, hdrfixInverseMods[index].y);
    rgb = clamp((rgb - hdrfixSDRRange.x) / max(hdrfixSDRRange.y - hdrfixSDRRange.x, 0.001), 0.0, 1.0);
    return vec4(hdrfixEncode(rgb) * source.a, source.a);
}
)GLSL";

// Modify only the sampled surface and its SDR output. Native geometry, discard,
// clipping, tint, rounding, blur and the HDR output remain in Hyprland's shader.
inline std::string patchSurfaceShader(std::string source, bool correctAlpha, bool primaryCapture, bool mirrorCapture, bool blurCapture = false, bool mirrorOutput = false, bool correctScalarAlpha = false) {
    const auto mainPosition = source.find("void main()");
    const auto sample = source.find("vec4 pixColor =", mainPosition);
    const auto sampleEnd = source.find(';', sample);
    if (mainPosition == std::string::npos || sample == std::string::npos || sampleEnd == std::string::npos)
        throw std::runtime_error("HDR shader: missing native texture sample");
    const bool splitAlpha = mirrorOutput && (correctAlpha || correctScalarAlpha);
    std::string afterSample;
    if (mirrorCapture || splitAlpha)
        afterSample += "\nvec4 hdrfixSource = pixColor;\n";
    if (correctAlpha)
        afterSample += "\nif (pixColor.a > 0.0 && pixColor.a < 1.0) pixColor.a = 1.0 - pow(1.0 - pixColor.a, 1.5);\n";
    if (primaryCapture)
        afterSample += "\npixColor = hdrfixCapture(pixColor, v_texcoord, 0);\n";
    if (correctScalarAlpha)
        afterSample += "\nfloat hdrfixMonitorAlpha = 1.0 - pow(1.0 - clamp(alpha, 0.0, 1.0), 1.5);\n";
    source.insert(sampleEnd + 1, afterSample);
    const auto replace = [&](const std::string& from, const std::string& to) {
        const auto position = source.find(from, mainPosition);
        if (position == std::string::npos)
            throw std::runtime_error("HDR shader: missing " + from);
        source.replace(position, from.size(), to);
    };
    const std::string cmStart = "doColorManagement(pixColor, alpha,";
    const auto cmPosition = source.find(cmStart, mainPosition);
    const bool cm = cmPosition != std::string::npos;
    std::string originalCM;
    if (splitAlpha && !mirrorCapture && cm) {
        const auto end = source.find(");", cmPosition);
        if (end == std::string::npos)
            throw std::runtime_error("HDR shader: missing native color management call");
        originalCM = source.substr(cmPosition, end + 2 - cmPosition);
        originalCM.replace(0, cmStart.size(), "doColorManagement(hdrfixSource, alpha,");
    }
    if (correctScalarAlpha) {
        if (cm)
            replace(cmStart, "doColorManagement(pixColor, hdrfixMonitorAlpha,");
        else
            replace("pixColor *= alpha;", "pixColor *= hdrfixMonitorAlpha;");
    }
    if (mirrorCapture || splitAlpha) {
        const std::string assignment = cm ? "mirrorColor = pixColors[1];" : "mirrorColor = pixColor;";
        if (mirrorCapture)
            replace(assignment, "mirrorColor = hdrfixCapture(hdrfixSource, v_texcoord, 0)" + std::string(cm ? " * alpha;" : ";"));
        else if (cm)
            replace(assignment, "vec4 hdrfixMirrorColors[2] = " + originalCM + "\nmirrorColor = hdrfixMirrorColors[1];");
        else
            replace(assignment, "mirrorColor = hdrfixSource;");
    }
    if (splitAlpha) {
        // Native proportional mirror blur refers to the monitor result. Rebuild
        // its uncorrected alpha so the monitor correction cannot affect capture.
        const auto mirrorSection = source.find("fragColor = pixColor;", mainPosition);
        const auto proportional = source.find("1.0 - pixColor.a", mirrorSection);
        if (mirrorSection != std::string::npos && proportional != std::string::npos)
            source.replace(proportional, std::string("1.0 - pixColor.a").size(),
                           "1.0 - (mirrorColor.a + blurredPixColor.a * (1.0 - mirrorColor.a) * pixBlurAlphaMask * (1.0 - pow(1.0 - mirrorColor.a, 3.0)))");
    }
    if (blurCapture) {
        const std::string pixel = mirrorOutput ? "blurredMirrorColor" : "blurredPixColor";
        replace("vec4 " + pixel + " = texture(blurredBG, blurUV);", "vec4 " + pixel + " = hdrfixCapture(texture(blurredBG, blurUV), blurUV, 1);");
    }
    if (primaryCapture || mirrorCapture || blurCapture) {
        std::string helpers = CAPTURE_GLSL;
        if (source.find("motionBlurSample(", mainPosition) != std::string::npos) {
            const std::string sampleCopy = "texture(hdrfixSDRCopy, uv)";
            helpers.replace(helpers.find(sampleCopy), sampleCopy.size(),
                            "motionBlurSample(hdrfixSDRCopy, motionPrevBox, motionCurrBox, motionSourceBox, motionSourceTexOrigin, motionSourceTexSize, motionSamples, true)");
        }
        source.insert(mainPosition, helpers);
    }
    return source;
}
}
