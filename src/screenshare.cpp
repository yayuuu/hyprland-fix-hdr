#include "../modules.hpp"
#include "capture-shader.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/protocols/ColorManagement.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace NColorManagement;
using namespace Render;
using Hyprutils::Utils::CScopeGuard;
using TextureData = GL::CHyprOpenGLImpl::STextureRenderData;

namespace {
HANDLE pluginHandle = nullptr;
CFunctionHook *getTextureHook = nullptr, *renderToFBHook = nullptr, *renderTextureHook = nullptr, *preDrawSurfaceHook = nullptr, *getShaderHook = nullptr;

struct SDRCopy {
    WP<ITexture> source, copy;
};
std::unordered_map<ITexture*, SDRCopy> sdrCopies;
std::unordered_set<ITexture*> scalarAlphaTextures, pixelAlphaTextures;
bool correctPixelAlpha = false, correctScalarAlpha = false;

enum class Capture { NONE, MIRROR, PRIMARY };
struct Source {
    PImageDescription description;
    SP<ITexture> copy;
    bool displayAdjusted = false;
};
struct Draw {
    CRenderContext& ctx;
    Source source, blur;
    Capture capture = Capture::NONE;
    bool captureSource = false, captureBlur = false;
};
Draw* currentDraw = nullptr;
struct ShaderKey {
    SShaderVariant variant;
    Capture capture;
    bool alpha, scalar, source, blur;
    auto operator<=>(const ShaderKey&) const = default;
};
std::map<ShaderKey, SP<CShader>> shaders;
uint64_t copyDraws = 0, tonemapDraws = 0, shaderFailures = 0;

bool isHDR(PImageDescription description) {
    if (!description)
        return false;
    const auto tf = description->value().transferFunction;
    return tf == CM_TRANSFER_FUNCTION_LINEAR || tf == CM_TRANSFER_FUNCTION_EXT_LINEAR || tf == CM_TRANSFER_FUNCTION_ST2084_PQ || tf == CM_TRANSFER_FUNCTION_HLG;
}

bool hdrTarget(CRenderContext& ctx) {
    return ctx.m_mode == RENDER_MODE_NORMAL && !ctx.m_renderingCapture && ctx.m_data.pMonitor && ctx.m_data.pMonitor->inHDR() &&
        ctx.m_data.currentFB && isHDR(ctx.m_data.currentFB->imageDescription());
}

void refreshMonitors() {
    if (!State::monitorState())
        return;
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor)
            continue;
        monitor->m_blurFBDirty = true;
        monitor->m_forceFullFrames = std::max(monitor->m_forceFullFrames, 2);
        if (const auto resources = monitor->resources())
            resources->invalidateMirrorFB();
        monitor->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_RENDER_MONITOR);
    }
}

// Remember native framebuffer/MRT pairs without keeping either texture alive.
SP<ITexture> hkGetTexture(IFramebuffer* framebuffer) {
    auto source = reinterpret_cast<SP<ITexture> (*)(IFramebuffer*)>(getTextureHook->m_original)(framebuffer);
    if (!source)
        return source;
    const auto copy = framebuffer->getMirrorTexture();
    if (copy && copy != source && copy->m_imageDescription && !isHDR(copy->m_imageDescription)) {
        if (!sdrCopies.contains(source.get()) && sdrCopies.size() >= 64)
            std::erase_if(sdrCopies, [](const auto& item) { return item.second.source.expired() || item.second.copy.expired(); });
        sdrCopies[source.get()] = {source, copy};
    } else
        sdrCopies.erase(source.get());
    return source;
}

SP<ITexture> sdrCopy(const SP<ITexture>& texture) {
    const auto found = sdrCopies.find(texture.get());
    if (found == sdrCopies.end())
        return nullptr;
    const auto source = found->second.source.lock();
    const auto copy = found->second.copy.lock();
    if (source != texture || !copy || !copy->m_imageDescription || isHDR(copy->m_imageDescription) || copy->m_size != texture->m_size) {
        sdrCopies.erase(found);
        return nullptr;
    }
    return copy;
}

PImageDescription sourceDescription(CRenderContext& ctx, const SP<ITexture>& texture, const TextureData& data) {
    if (texture->m_imageDescription)
        return texture->m_imageDescription;
    if (ctx.m_data.surface && ctx.m_data.surface->m_colorManagement)
        return CImageDescription::from(ctx.m_data.surface->m_colorManagement->imageDescription());
    if (data.cmBackToSRGB || data.finalMonitorCM)
        return g_pHyprRenderer->workBufferImageDescription(ctx);
    return getDefaultImageDescription();
}

void setCaptureUniforms(CShader& shader, const Draw& draw) {
    const auto program = shader.program();
    const auto uniform = [&](const char* name) { return glGetUniformLocation(program, name); };
    const auto monitor = draw.ctx.m_data.pMonitor;
    const auto target = draw.capture == Capture::MIRROR ? draw.ctx.m_data.currentFB->getMirrorTexture()->m_imageDescription : draw.ctx.m_data.currentFB->imageDescription();
    glUniform1i(uniform("hdrfixSDRCopy"), 3);
    glUniform1i(uniform("hdrfixBlurCopy"), 4);
    glUniform1i(uniform("hdrfixOutputTF"), target ? target->value().transferFunction : CM_TRANSFER_FUNCTION_SRGB);
    glUniform2f(uniform("hdrfixSDRRange"), std::max(0.F, monitor->m_sdrMinLuminance), std::max(1.F, float(monitor->m_sdrMaxLuminance)));
    for (int i = 0; i < 2; ++i) {
        if (!(i == 0 ? draw.captureSource : draw.captureBlur))
            continue;
        const auto& source = i == 0 ? draw.source : draw.blur;
        const auto& description = source.description->value();
        const auto indexed = [&](const char* name) { return uniform(std::format("{}[{}]", name, i).c_str()); };
        glUniform1i(indexed("hdrfixUseCopy"), !!source.copy);
        glUniform1i(indexed("hdrfixSourceTF"), description.transferFunction);
        glUniform1i(indexed("hdrfixCopyTF"), source.copy ? source.copy->m_imageDescription->value().transferFunction : CM_TRANSFER_FUNCTION_SRGB);
        const bool nits = description.transferFunction == CM_TRANSFER_FUNCTION_LINEAR;
        glUniform2f(indexed("hdrfixSourceRange"), nits ? 0.F : description.getTFMinLuminance(), nits ? 1.F : description.getTFMaxLuminance());
        glUniform2f(indexed("hdrfixInverseMods"), source.displayAdjusted && monitor->m_sdrBrightness > 0 ? 1.F / monitor->m_sdrBrightness : 1.F,
                    source.displayAdjusted && monitor->m_sdrSaturation > 0 ? 1.F / monitor->m_sdrSaturation : 1.F);
        const auto settings = g_pHyprRenderer->getCMSettings(draw.ctx, source.description, DEFAULT_SRGB_IMAGE_DESCRIPTION, nullptr, false);
        const auto& matrix = settings.convertMatrix;
        const GLfloat conversion[] = {float(matrix[0][0]), float(matrix[1][0]), float(matrix[2][0]), float(matrix[0][1]), float(matrix[1][1]),
                                      float(matrix[2][1]), float(matrix[0][2]), float(matrix[1][2]), float(matrix[2][2])};
        glUniformMatrix3fv(indexed("hdrfixToSRGB"), 1, GL_FALSE, conversion);
        if (source.copy) {
            GL::g_pHyprOpenGL->setActiveTexture(GL_TEXTURE3 + i);
            source.copy->bind();
            source.copy->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            source.copy->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            GL::g_pHyprOpenGL->setActiveTexture(GL_TEXTURE0);
            ++copyDraws;
        } else
            ++tonemapDraws;
    }
}

WP<CShader> hkGetShader(void* self, ePreparedFragmentShader frag, const SShaderVariant& variant) {
    const auto original = [&] { return reinterpret_cast<WP<CShader> (*)(void*, ePreparedFragmentShader, const SShaderVariant&)>(getShaderHook->m_original)(self, frag, variant); };
    const bool captureSource = currentDraw && currentDraw->captureSource && currentDraw->capture != Capture::NONE;
    const bool captureBlur = currentDraw && currentDraw->captureBlur && currentDraw->capture != Capture::NONE && (variant.features & SH_FEAT_BLUR);
    const Capture capture = captureSource || captureBlur ? currentDraw->capture : Capture::NONE;
    const bool alpha = correctPixelAlpha && (variant.features & SH_FEAT_RGBA);
    const bool scalar = correctScalarAlpha;
    if (frag != SH_FRAG_SURFACE || (!alpha && !scalar && capture == Capture::NONE))
        return original();
    const ShaderKey key{variant, capture, alpha, scalar, captureSource, captureBlur};
    auto found = shaders.find(key);
    if (found == shaders.end()) {
        try {
            auto nativeVariant = variant;
            if (capture == Capture::PRIMARY && captureSource)
                nativeVariant.features &= ~(SH_FEAT_CM | SH_FEAT_TONEMAP | SH_FEAT_ALT_TONEMAP | SH_FEAT_SDR_MOD | SH_FEAT_ICC);
            auto source = HDRFix::patchSurfaceShader(g_pShaderLoader->getVariantSource(frag, nativeVariant), alpha, captureSource && capture == Capture::PRIMARY,
                                                     captureSource && capture == Capture::MIRROR, captureBlur, variant.features & SH_FEAT_MIRROR, scalar);
            auto shader = makeShared<CShader>();
            if (!shader->createProgram(GL::g_pHyprOpenGL->m_shaders->TEXVERTSRC, source, true, false))
                throw std::runtime_error("Cannot compile HDR texture shader");
            found = shaders.emplace(key, shader).first;
        } catch (const std::exception& error) {
            ++shaderFailures;
            Log::logger->log(Log::ERR, "[hyprland-fix-hdr] {}", error.what());
            // Cache failure too; retry only after a reload or native shader rebuild.
            found = shaders.emplace(key, nullptr).first;
        }
    }
    if (!found->second)
        return original();
    auto shader = found->second;
    GL::g_pHyprOpenGL->useShader(shader);
    if (capture != Capture::NONE) {
        auto uniforms = *currentDraw;
        uniforms.captureSource = captureSource;
        uniforms.captureBlur = captureBlur;
        setCaptureUniforms(*shader, uniforms);
    }
    return shader;
}

WP<CShader> hkRenderToFB(void* self, CRenderContext& ctx, SP<ITexture> texture, const TextureData& data, eTextureType type, const CBox& box) {
    Draw draw{.ctx = ctx, .source = {.description = sourceDescription(ctx, texture, data), .copy = sdrCopy(texture)}, .blur = {}};
    if (data.blurredBG)
        draw.blur = {data.blurredBG->m_imageDescription, sdrCopy(data.blurredBG), true};
    if (ctx.m_data.pMonitor && ctx.m_data.pMonitor->inHDR() && !data.finalMonitorCM) {
        const bool mirror = ctx.m_data.currentFB->getMirrorTexture() && ctx.m_data.pMonitor->needsUnmodifiedCopy();
        const auto target = ctx.m_data.currentFB->imageDescription();
        draw.captureSource = isHDR(draw.source.description);
        draw.captureBlur = data.blur && isHDR(draw.blur.description);
        if (draw.captureSource || draw.captureBlur)
            draw.capture = mirror ? Capture::MIRROR : target && !isHDR(target) ? Capture::PRIMARY : Capture::NONE;
        // Leave the completed monitor copy to Hyprland, including its behavior
        // when the user disables keep_unmodified_copy. Only reuse its SDR copy.
        if (draw.capture == Capture::PRIMARY && !draw.source.copy && ctx.m_data.mainFB && texture == ctx.m_data.mainFB->getTexture())
            draw.capture = Capture::NONE;
        if (draw.source.copy == ctx.m_data.currentFB->getMirrorTexture())
            draw.source.copy.reset(); // Never sample an attachment being written.
        if (draw.blur.copy == ctx.m_data.currentFB->getMirrorTexture())
            draw.blur.copy.reset();
        // Only compositor-owned HDR buffers contain the monitor's SDR adjustment.
        draw.source.displayAdjusted = texture->m_imageDescription &&
            (draw.source.description->value().transferFunction == CM_TRANSFER_FUNCTION_LINEAR || draw.source.description->value().transferFunction == CM_TRANSFER_FUNCTION_EXT_LINEAR) &&
            !ctx.m_data.surface;
    }
    const auto previous = currentDraw;
    currentDraw = &draw;
    const CScopeGuard restore([&] { currentDraw = previous; });
    return reinterpret_cast<WP<CShader> (*)(void*, CRenderContext&, SP<ITexture>, const TextureData&, eTextureType, const CBox&)>(renderToFBHook->m_original)(self, ctx, texture, data, type, box);
}

void hkPreDrawSurface(void* self, CRenderContext& ctx, WP<CSurfacePassElement> element, const CRegion& damage) {
    const auto original = [&] { reinterpret_cast<void (*)(void*, CRenderContext&, WP<CSurfacePassElement>, const CRegion&)>(preDrawSurfaceHook->m_original)(self, ctx, element, damage); };
    if (!element || !hdrTarget(ctx)) {
        original();
        return;
    }
    const auto surface = Desktop::View::CWLSurface::fromResource(element->m_data.surface);
    const float alpha = element->m_data.alpha * element->m_data.fadeAlpha * (surface ? surface->m_alphaModifier * surface->m_overallOpacity : 1.F);
    const auto texture = element->m_data.texture;
    const bool scalar = texture && alpha > 0.F && alpha < 1.F;
    const bool pixel = texture && texture->m_type == TEXTURE_RGBA && !texture->m_opaque && alpha >= 0.999F;
    if (scalar)
        scalarAlphaTextures.insert(texture.get());
    if (pixel)
        pixelAlphaTextures.insert(texture.get());
    const CScopeGuard restore([&] {
        if (scalar) scalarAlphaTextures.erase(texture.get());
        if (pixel) pixelAlphaTextures.erase(texture.get());
    });
    original();
}

void hkRenderTexture(void* self, CRenderContext& ctx, SP<ITexture> texture, const CBox& box, const TextureData& data) {
    const bool correct = hdrTarget(ctx) && !data.discardActive;
    const bool previous = correctPixelAlpha;
    const bool previousScalar = correctScalarAlpha;
    correctPixelAlpha = correct && pixelAlphaTextures.contains(texture.get());
    correctScalarAlpha = correct && scalarAlphaTextures.contains(texture.get()) && data.a > 0.F && data.a < 1.F;
    const CScopeGuard restore([&] { correctPixelAlpha = previous; correctScalarAlpha = previousScalar; });
    reinterpret_cast<void (*)(void*, CRenderContext&, SP<ITexture>, const CBox&, const TextureData&)>(renderTextureHook->m_original)(self, ctx, texture, box, data);
}

void hook(CFunctionHook*& destination, const std::string& name, const std::string& signature, void* replacement) {
    void* address = nullptr;
    for (const auto& match : HyprlandAPI::findFunctionsByName(pluginHandle, name)) {
        if (match.demangled.find(signature) == std::string::npos)
            continue;
        if (address)
            throw std::runtime_error("Ambiguous HDR hook: " + name);
        address = match.address;
    }
    if (!address)
        throw std::runtime_error("Missing HDR hook: " + name);
    destination = HyprlandAPI::createFunctionHook(pluginHandle, address, replacement);
    if (!destination || !destination->hook())
        throw std::runtime_error("Cannot install HDR hook: " + name);
}
}

void HDRFix::initScreenshare(HANDLE handle) {
    pluginHandle = handle;
    hook(getTextureHook, "getTexture", "Render::IFramebuffer::getTexture()", reinterpret_cast<void*>(hkGetTexture));
    hook(renderToFBHook, "renderToFBInternal", "CHyprOpenGLImpl::renderToFBInternal(", reinterpret_cast<void*>(hkRenderToFB));
    hook(renderTextureHook, "renderTextureInternal", "CHyprOpenGLImpl::renderTextureInternal(", reinterpret_cast<void*>(hkRenderTexture));
    hook(preDrawSurfaceHook, "preDrawSurface", "Render::IElementRenderer::preDrawSurface(", reinterpret_cast<void*>(hkPreDrawSurface));
    hook(getShaderHook, "getShaderVariant", "CHyprOpenGLImpl::getShaderVariant(Render::ePreparedFragmentShader, Render::SShaderVariant const&)", reinterpret_cast<void*>(hkGetShader));
    refreshMonitors();
}

void HDRFix::exitScreenshare() {
    for (auto* hook : {&getShaderHook, &preDrawSurfaceHook, &renderTextureHook, &renderToFBHook, &getTextureHook}) {
        if (!*hook)
            continue;
        (*hook)->unhook();
        HyprlandAPI::removeFunctionHook(pluginHandle, *hook);
        *hook = nullptr;
    }
    shaders.clear();
    sdrCopies.clear();
    scalarAlphaTextures.clear();
    pixelAlphaTextures.clear();
    correctPixelAlpha = false;
    correctScalarAlpha = false;
    currentDraw = nullptr;
    refreshMonitors();
}

std::string HDRFix::screenshareStatus() {
    return std::format("{{\"version\":\"{}\",\"native_unmodified_copy\":true,\"per_texture_capture\":true,\"alpha_fixes\":true,\"alpha_scope\":\"monitor\",\"sdr_copy_draws\":{},\"tonemap_draws\":{},\"shader_failures\":{}}}", VERSION, copyDraws, tonemapDraws, shaderFailures);
}
