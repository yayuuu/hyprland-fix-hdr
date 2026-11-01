#define WLR_USE_UNSTABLE

#include <bits/stdc++.h>
#include <dlfcn.h>
#include <hyprland/src/plugins/PluginSystem.hpp>

// Match the private-access convention used by the existing local plugins.
// No uprobes or OA counters. Optional capture pacing hooks share() only.
#define private public
#define protected public
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/protocols/LinuxDMABUF.hpp>
#include <hyprland/src/protocols/types/DMABuffer.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/output/MonitorResources.hpp>
#include <hyprland/src/managers/screenshare/ScreenshareManager.hpp>
#undef protected
#undef private

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/HyprCtl.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include "../modules.hpp"
#include <hyprland/src/protocols/ColorManagement.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/SyncFDManager.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/protocols/DRMSyncobj.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/dma-buf.h>

#include "../policy.hpp"
#include "../capture_wait.hpp"
#include "../pq.hpp"

static_assert(std::string_view{GIT_COMMIT_HASH} == "efb50993780079460b0cbed1363e2166a2de1d9f", "Build against Hyprland v0.56.2 headers");
static_assert(std::string_view{HDR_SINGLEPASS_AQ_VERSION}.substr(0, 4) == "0.15", "Build against Aquamarine 0.15.x headers");

static HANDLE g_handle = nullptr;
static CFunctionHook* g_shareHook = nullptr;
static CFunctionHook* g_copyFBHook = nullptr;

struct SFrameRestore {
    PHLMONITOR monitor;
    SP<Render::IFramebuffer> output;
    SP<Render::IFramebuffer> work;
    NColorManagement::PImageDescription description;
    CSurfacePassElement* surface = nullptr;
    CTexPassElement* cursor = nullptr;
    Render::CRenderPass* pass = nullptr;
    std::vector<Render::CRenderPass::SPassElementData> originalPass;
    size_t surfaceIndex = 0;
    std::optional<size_t> cursorIndex;
    bool blur = false;
};

struct SState {
    CHyprSignalListener stageListener;
    CHyprSignalListener closeListener;
    CHyprSignalListener traceCommit, tracePresent, traceCursor;
    bool tracing = false;
    bool capturePacing = false;
    bool uniformCapture = true;
    bool cachedCapture = false; // Experimental until delivery cadence is measured.
    uint64_t cachedCaptureCopies = 0;
    uint64_t captureCoalesced = 0, captureTimeouts = 0, subpassFrames = 0;
    struct SContentWatch {
        WP<CWLSurfaceResource> surface;
        PHLMONITORREF monitor;
        CHyprSignalListener commit;
        std::chrono::steady_clock::time_point lastDraw{};
    };
    std::map<CWLSurfaceResource*, UP<SContentWatch>> contentWatches;
    struct SMonitorActivity { PHLMONITORREF monitor; std::chrono::steady_clock::time_point lastCommit{}; };
    std::map<Monitor::CMonitor*, SMonitorActivity> contentActivity;
    SP<CEventLoopTimer> captureTimer;
    struct SCaptureWait { WP<Screenshare::CScreenshareFrame> frame; PHLMONITORREF monitor; };
    std::vector<SCaptureWait> captureWaits;
    SP<CEventLoopTimer> captureReadyTimer;
    std::vector<SCaptureWait> captureReady;
    uint64_t cursorGeneration = 0;
    std::chrono::steady_clock::time_point traceStart;
    std::vector<std::string> traceRows;
    SP<SHyprCtlCommand> command, legacyCommand;
    bool enabled = true;
    std::string shaderMode = "stock";
    std::string shaderError;
    GLuint pqTexture = 0;
    std::map<std::pair<std::string, Render::ShaderFeatureFlags>, SP<CShader>> fastShaders;
    uint64_t shaderFrames = 0;
    struct SProfileQuery { GLuint read = 0, convert = 0; };
    std::vector<SProfileQuery> pendingQueries;
    std::vector<std::pair<uint64_t, uint64_t>> profileSamples;
    int profileIssued = 0;
    std::string outputName = "DP-1";
    uint64_t considered = 0;
    uint64_t optimized = 0;
    uint64_t fallback = 0;
    std::string lastReason = "disabled";
    std::string lastInput = "unknown";
    std::string lastPassInfo = "not inspected";
    std::string textureInfo = "unknown";
    bool lastWasFast = false;
    size_t candidateIndex = 0;
    std::optional<size_t> candidateCursor;
    SFrameRestore restore;
};

static UP<SState> g_state;
static void traceEvent(const char* event, PHLMONITOR monitor = nullptr, std::optional<Time::steady_tp> timestamp = std::nullopt) {
    if (!g_state || !g_state->tracing || g_state->traceRows.size() >= 1536) return;
    const auto elapsed = std::chrono::duration<double, std::milli>(timestamp.value_or(std::chrono::steady_clock::now()) - g_state->traceStart).count();
    const auto pointer = Pointer::mgr()->position();
    CBox damage;
    if (monitor && g_pHyprRenderer->m_renderMode == Render::RENDER_MODE_NORMAL)
        damage = g_pHyprRenderer->m_renderData.damage.getExtents();
    g_state->traceRows.push_back(std::format("[\"{}\",{:.4f},{},{},{},{:.1f},{:.1f},{:.0f},{:.0f},{:.0f},{:.0f},{},{}]",
        event, elapsed, 0, 0, g_state->cursorGeneration,
        pointer.x, pointer.y, damage.x, damage.y, damage.w, damage.h,
        monitor ? monitor->m_output->needsFrame : false, monitor ? monitor->m_pendingFrame : false));
}
static void observeContentSurface(SP<CWLSurfaceResource> surface, PHLMONITOR monitor) {
    if (!surface || !monitor) return;
    auto& watches = g_state->contentWatches;
    std::erase_if(watches, [](const auto& item) { return item.second->surface.expired(); });
    std::erase_if(g_state->contentActivity, [](const auto& item) { return item.second.monitor.expired(); });
    auto& watch = watches[surface.get()];
    if (!watch) {
        watch = makeUnique<SState::SContentWatch>(); watch->surface = surface;
        watch->commit = surface->m_events.commit.listen([held = watch.get()] {
            if (!g_state || !g_state->capturePacing || held->surface.expired() || held->monitor.expired()) return;
            const auto now = std::chrono::steady_clock::now();
            if (now - held->lastDraw > std::chrono::milliseconds(250)) return;
            auto& activity = g_state->contentActivity[held->monitor.get()];
            activity.monitor = held->monitor; activity.lastCommit = now;
        });
    }
    watch->monitor = monitor; watch->lastDraw = std::chrono::steady_clock::now();
}
static uint64_t renderEngineNanoseconds() noexcept {
    try {
        uint64_t total = 0;
        std::unordered_set<std::string> seen;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fdinfo")) {
            std::ifstream file(entry.path());
            std::string line, driver, client, device;
            uint64_t render = 0;
            while (std::getline(file, line)) {
                std::istringstream fields(line);
                std::string key;
                fields >> key;
                if (key == "drm-driver:")
                    fields >> driver;
                else if (key == "drm-client-id:")
                    fields >> client;
                else if (key == "drm-pdev:")
                    fields >> device;
                else if (key == "drm-engine-render:")
                    fields >> render;
            }
            if (driver == "i915" && !client.empty() && seen.emplace(device + ":" + client).second)
                total += render;
        }
        return total;
    } catch (...) {
        return 0;
    }
}

static void restoreFrameMetadata() {
    if (!g_state || !g_state->restore.output)
        return;
    auto& saved = g_state->restore;
    saved.output->setImageDescription(saved.description);
    if (saved.surface)
        saved.surface->m_data.blur = saved.blur;
    if (saved.pass)
        saved.pass->m_passElements = std::move(saved.originalPass);
    saved = {};
}

static void replacePQ(std::string& source) {
    const auto pos = source.find("vec3 tfPQ(");
    if (pos == std::string::npos)
        throw std::runtime_error("PQ shader function is missing");
    auto end = source.find('{', pos);
    int depth = 1;
    while (++end < source.size() && depth) {
        if (source[end] == '{') ++depth;
        if (source[end] == '}') --depth;
    }
    if (depth)
        throw std::runtime_error("PQ shader function is incomplete");
    source.replace(pos, end - pos, PQLut::glsl);
}

// Called only inside an active GL render pass. Temporarily replace two shader
// cache entries only around this opaque scRGB -> PQ surface draw. Core shaders
// are restored before drawing the linear cursor patch or any other output.
static bool drawTransferredSurface(UP<IPassElement>& element,const CRegion& damage) {
    using DrawSurface=bool (*)(void*,const CRegion*,bool);
    for(const auto& plugin:g_pPluginSystem->getAllPlugins())
        if(auto symbol=dlsym(plugin->m_handle,"gpu_switch_draw_surface_v1"))
            return reinterpret_cast<DrawSurface>(symbol)(&element,&damage,true);
    return false;
}
static void drawPrimary(const CRegion& damage) {
    auto& saved = g_state->restore;
    auto& gl = *Render::GL::g_pHyprOpenGL;
    auto draw = [&] { g_pHyprRenderer->draw(saved.originalPass.at(saved.surfaceIndex).element, damage); };
    if (g_state->shaderMode == "stock" && drawTransferredSurface(saved.originalPass.at(saved.surfaceIndex).element,damage))return;
    if (g_state->shaderMode == "nearest" || g_state->shaderMode == "fetch") {
        const auto& surface = saved.surface->m_data.surface->m_current;
        const auto box = saved.surface->getTexBox();
        if (saved.surface->m_data.texture->m_size != g_pHyprRenderer->m_renderData.pMonitor->m_pixelSize ||
            surface.scale != 1 || surface.viewport.hasSource ||
            (surface.viewport.hasDestination && surface.viewport.destination != g_pHyprRenderer->m_renderData.pMonitor->m_pixelSize) ||
            surface.transform != WL_OUTPUT_TRANSFORM_NORMAL || box.pos() != Vector2D{} ||
            box.size() != g_pHyprRenderer->m_renderData.pMonitor->m_pixelSize) {
            g_state->shaderError = "Sampling optimization requires exact 1:1 mapping";
            draw();
            return;
        }
        if (g_state->shaderMode == "nearest") {
            const bool old = saved.surface->m_data.useNearestNeighbor;
            Render::CScopeGuard restore([&] { saved.surface->m_data.useNearestNeighbor = old; });
            saved.surface->m_data.useNearestNeighbor = true;
            draw();
            ++g_state->shaderFrames;
            return;
        }
    }
    if (g_state->shaderMode == "stock" || g_state->shaderMode == "profile" || !g_state->shaderError.empty() ||
        g_pHyprRenderer->m_renderData.pMonitor->m_imageDescription->value().transferFunction != NColorManagement::CM_TRANSFER_FUNCTION_ST2084_PQ) {
        draw();
        return;
    }
    GLint previousActive = GL_TEXTURE0, previousTexture = 0, previousSampler = 0, previousProgram = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActive);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glActiveTexture(GL_TEXTURE7);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegeri_v(GL_SAMPLER_BINDING, 7, &previousSampler);
    glBindSampler(7, 0);
    auto& variants = gl.m_shaders->fragVariants[Render::SH_FRAG_SURFACE];
    std::map<Render::ShaderFeatureFlags, std::optional<SP<CShader>>> originals;
    bool used = false;
    {
        Render::CScopeGuard restore([&] {
            for (auto& [flags, original] : originals) {
                if (original) variants[flags] = *original;
                else variants.erase(flags);
            }
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, previousTexture);
            glBindSampler(7, previousSampler);
            glActiveTexture(previousActive);
        });
        try {
            if (g_state->shaderMode == "lut" && !g_state->pqTexture) {
                const auto table = PQLut::samples();
                glGenTextures(1, &g_state->pqTexture);
                glBindTexture(GL_TEXTURE_2D, g_state->pqTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, PQLut::count, 1, 0, GL_RED, GL_FLOAT, table.data());
            }
            for (const auto flags : {Render::ShaderFeatureFlags(Render::SH_FEAT_CM), Render::ShaderFeatureFlags(Render::SH_FEAT_CM | Render::SH_FEAT_RGBA)}) {
                const auto key = std::pair{g_state->shaderMode, flags};
                if (!g_state->fastShaders.contains(key)) {
                    auto source = Render::g_pShaderLoader->getVariantSource(Render::SH_FRAG_SURFACE, flags);
                    // Match identifiers rather than preprocessor whitespace.
                    source = std::regex_replace(source, std::regex(R"(uniform\s+int\s+sourceTF\s*;)"), "const int sourceTF = 5;");
                    source = std::regex_replace(source, std::regex(R"(uniform\s+int\s+targetTF\s*;)"), "const int targetTF = 11;");
                    if (source.find("const int sourceTF = 5;") == std::string::npos || source.find("const int targetTF = 11;") == std::string::npos)
                        throw std::runtime_error("Cannot specialize transfer functions");
                    if (g_state->shaderMode == "lut") replacePQ(source);
                    if (g_state->shaderMode == "fetch")
                        source = std::regex_replace(source, std::regex(R"(texture\s*\(\s*tex\s*,\s*v_texcoord\s*\))"),
                            "texelFetch(tex, clamp(ivec2(v_texcoord * vec2(textureSize(tex, 0))), ivec2(0), textureSize(tex, 0) - 1), 0)");
                    auto shader = makeShared<CShader>();
                    if (!shader->createProgram(gl.m_shaders->TEXVERTSRC, source, true, true))
                        throw std::runtime_error("Specialized shader compilation failed");
                    if (g_state->shaderMode == "lut") {
                        glUseProgram(shader->program());
                        glUniform1i(glGetUniformLocation(shader->program(), "hdrPQLut"), 7);
                        glUseProgram(previousProgram);
                    }
                    g_state->fastShaders.emplace(key, shader);
                }
                if (const auto it = variants.find(flags); it != variants.end()) originals[flags] = it->second;
                else originals[flags] = std::nullopt;
                variants[flags] = g_state->fastShaders.at(key);
            }
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, g_state->pqTexture);
            glActiveTexture(previousActive);
            used = true;
        } catch (const std::exception& error) {
            g_state->shaderError = error.what();
            for (auto& [flags, original] : originals) {
                if (original) variants[flags] = *original;
                else variants.erase(flags);
            }
            originals.clear();
            glUseProgram(previousProgram);
        }
        draw();
        if (used) {
            for (const auto& [key, shader] : g_state->fastShaders)
                if (key.first == g_state->shaderMode && gl.m_currentProgram == shader->program()) {
                    ++g_state->shaderFrames;
                    break;
                }
        }
    }
}

static void collectProfile() {
    using Get64 = void (*)(GLuint, GLenum, GLuint64*);
    static const auto get64 = reinterpret_cast<Get64>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    if (!get64) return;
    std::erase_if(g_state->pendingQueries, [&](auto query) {
        GLuint available = 0;
        glGetQueryObjectuiv(query.convert, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) return false;
        GLuint64 read = 0, convert = 0;
        get64(query.read, GL_QUERY_RESULT, &read);
        get64(query.convert, GL_QUERY_RESULT, &convert);
        g_state->profileSamples.emplace_back(read, convert);
        const GLuint ids[]{query.read, query.convert};
        glDeleteQueries(2, ids);
        return true;
    });
    if (g_state->shaderMode == "profile" && g_state->profileSamples.size() >= 3)
        g_state->shaderMode = "stock";
}

static bool profileFrame(const CRegion& damage) {
    if (g_state->shaderMode != "profile" || g_state->profileIssued >= 3) return false;
    using Get64 = void (*)(GLuint, GLenum, GLuint64*);
    if (!reinterpret_cast<Get64>(eglGetProcAddress("glGetQueryObjectui64vEXT"))) {
        g_state->shaderError = "GPU timer queries unavailable";
        g_state->shaderMode = "stock";
        return false;
    }
    constexpr GLenum elapsed = 0x88BF; // GL_TIME_ELAPSED_EXT
    GLint active = 0;
    glGetQueryiv(elapsed, GL_CURRENT_QUERY, &active);
    if (active) return false;
    GLuint ids[2];
    glGenQueries(2, ids);
    auto& saved = g_state->restore;
    auto& gl = *Render::GL::g_pHyprOpenGL;
    g_pHyprRenderer->bindFB(saved.work);
    glBeginQuery(elapsed, ids[0]);
    g_pHyprRenderer->draw(saved.originalPass.at(saved.surfaceIndex).element, damage);
    glEndQuery(elapsed);
    g_pHyprRenderer->bindFB(saved.output);
    gl.blend(false);
    Render::GL::CHyprOpenGLImpl::STextureRenderData conversion{};
    conversion.damage = &damage;
    conversion.finalMonitorCM = true;
    glBeginQuery(elapsed, ids[1]);
    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    gl.renderTexture(saved.work->getTexture(), CBox{{0, 0}, monitor->m_pixelSize}, conversion);
    glEndQuery(elapsed);
    gl.blend(true);
    g_state->pendingQueries.push_back({ids[0], ids[1]});
    ++g_state->profileIssued;
    return true;
}

static void drawSinglepassFrame() {
    auto& saved = g_state->restore;
    auto& rd = g_pHyprRenderer->m_renderData;
    auto& gl = *Render::GL::g_pHyprOpenGL;
    const CRegion fullDamage = rd.damage;
    g_pHyprRenderer->bindFB(saved.output);
    collectProfile();
    if (!profileFrame(fullDamage))
        drawPrimary(fullDamage);

    if (saved.cursor && saved.cursorIndex) {
        // Reconstruct only the small cursor rectangle in the linear FP16 work
        // buffer. Blending the cursor directly in PQ would change its colours.
        auto work = saved.work;
        CRegion cursorDamage{saved.cursor->m_data.box};
        cursorDamage.expand(2).intersect(fullDamage);
        if (!cursorDamage.empty()) {
            g_pHyprRenderer->bindFB(work);
            rd.damage = cursorDamage;
            const bool oldFeedback = g_pHyprRenderer->m_bBlockSurfaceFeedback;
            const auto oldBuffers = g_pHyprRenderer->m_usedAsyncBuffers.size();
            const auto originalTexture = saved.surface->m_data.texture;
            {
                Render::CScopeGuard guard([&] {
                    g_pHyprRenderer->m_bBlockSurfaceFeedback = oldFeedback;
                    g_pHyprRenderer->m_usedAsyncBuffers.resize(oldBuffers);
                    saved.surface->m_data.texture = originalTexture;
                });
                g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
                if(!drawTransferredSurface(saved.originalPass.at(saved.surfaceIndex).element,cursorDamage))
                    g_pHyprRenderer->draw(saved.originalPass.at(saved.surfaceIndex).element, cursorDamage);
            }
            rd.damage = cursorDamage;
            g_pHyprRenderer->draw(saved.originalPass.at(*saved.cursorIndex).element, cursorDamage);
            g_pHyprRenderer->bindFB(saved.output);
            gl.blend(false);
            const auto monitor = rd.pMonitor.lock();
            Render::GL::CHyprOpenGLImpl::STextureRenderData conversion{};
            conversion.damage = &cursorDamage;
            conversion.finalMonitorCM = true;
            gl.renderTexture(work->getTexture(), CBox{{0, 0}, monitor->m_pixelSize}, conversion);
            gl.blend(true);
        }
    }
    g_pHyprRenderer->bindFB(saved.output);
    rd.damage = fullDamage;
    // Allow Hyprland's normal end-of-frame invalidation to retire the work FB.
    saved.work.reset();
}

class CSinglepassElement : public IPassElement {
  public:
    std::vector<UP<IPassElement>> draw() override {
        drawSinglepassFrame();
        return {};
    }
    bool needsLiveBlur() override { return false; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "CHDRSinglepassElement"; }
    ePassElementType type() override { return EK_CUSTOM; }
    bool undiscardable() override { return true; }
};

static bool covers(const CRegion& region, const CBox& output) {
    return CRegion{output}.subtract(region).empty();
}

static NColorManagement::PImageDescription sourceDescription(CSurfacePassElement& element) {
    if (element.m_data.texture && element.m_data.texture->m_imageDescription)
        return element.m_data.texture->m_imageDescription;
    const auto& surface = element.m_data.surface;
    if (surface && surface->m_colorManagement.valid() && surface->m_colorManagement->hasImageDescription())
        return NColorManagement::CImageDescription::from(surface->m_colorManagement->imageDescription());
    return NColorManagement::getDefaultImageDescription();
}

static bool identityInput(CSurfacePassElement& element, PHLMONITOR monitor) {
    using namespace NColorManagement;
    const auto source = sourceDescription(element);
    if (!source)
        return false;
    const auto work = monitor->workBufferImageDescription();
    const auto& src = source->value();
    const auto& dst = work->value();
    g_state->lastInput = tfToString(src.transferFunction);
    // A linear HDR input already in the linear work-buffer colour space can
    // be converted directly to output PQ without changing the colour pipeline.
    // PQ inputs, ICC, gamut changes and first-stage tone mapping fall back.
    if (src.transferFunction != CM_TRANSFER_FUNCTION_EXT_LINEAR || dst.transferFunction != CM_TRANSFER_FUNCTION_EXT_LINEAR || src.icc.present || dst.icc.present)
        return false;
    if (source->getPrimaries()->id() != work->getPrimaries()->id())
        return false;
    if (src.getTFMinLuminance() != dst.getTFMinLuminance() || src.getTFMaxLuminance() != dst.getTFMaxLuminance())
        return false;
    const auto maxSource = src.luminances.max > 0 ? src.luminances.max : src.luminances.reference;
    const auto maxWork = dst.luminances.max > 0 ? dst.luminances.max : 10000;
    return maxSource < maxWork * 1.01;
}

extern "C" __attribute__((visibility("default"))) void gpu_surface_presented_v1(void* rawSurface) {
    if(!g_state || !g_state->capturePacing || !rawSurface)return;
    const auto monitor=g_pHyprRenderer->m_renderData.pMonitor.lock();
    auto* surface=static_cast<CSurfacePassElement*>(rawSurface);
    if(monitor && surface->m_data.surface)observeContentSurface(surface->m_data.surface,monitor);
}

static CSurfacePassElement* eligibleSurface(PHLMONITOR monitor) {
    auto& pass = g_pHyprRenderer->currentPass();
    std::string names;
    for (const auto& item : pass.m_passElements) {
        if (!names.empty())
            names += ",";
        names += item.element ? item.element->passName() : "null";
        if (item.element && item.element->type() == EK_SURFACE) {
            const auto* s = dynamic_cast<CSurfacePassElement*>(item.element.get());
            if (s)
                names += std::format("[window={} main={} wh={}x{}]", bool(s->m_data.pWindow), s->m_data.mainSurface, s->m_data.w, s->m_data.h);
        }
    }
    g_state->lastPassInfo = std::format("{} elements: {}", pass.m_passElements.size(), names);
    if (&pass != &g_pHyprRenderer->m_renderPass || pass.m_passElements.empty())
        return nullptr;
    auto index = pass.m_passElements.size() - 1;
    g_state->candidateCursor.reset();
    const auto& top = pass.m_passElements.at(index).element;
    if (top && top->type() == EK_TEXTURE) {
        const auto* cursor = dynamic_cast<CTexPassElement*>(top.get());
        if (!cursor || index == 0 || cursor->m_data.tex != Pointer::mgr()->getCurrentCursorTexture() || cursor->m_data.blur || cursor->m_data.motionBlur.enabled ||
            cursor->m_data.box.w > 256 || cursor->m_data.box.h > 256 || cursor->m_data.useMirrorProjection)
            return nullptr;
        g_state->candidateCursor = index--;
    }
    g_state->candidateIndex = index;
    const auto& element = pass.m_passElements.at(index).element;
    if (!element || element->type() != EK_SURFACE)
        return nullptr;
    auto* surface = dynamic_cast<CSurfacePassElement*>(element.get());
    const auto solitary = monitor->m_solitaryClient.lock();
    if (!surface || !solitary || surface->m_data.pWindow != solitary || surface->m_data.popup || !surface->m_data.surface ||
        surface->m_data.surface != solitary->getSolitaryResource()) {
        g_state->lastPassInfo += " (no matching solitary surface)";
        return nullptr;
    }
    if (const auto source = sourceDescription(*surface))
        g_state->lastInput = NColorManagement::tfToString(source->value().transferFunction);
    const auto& bufferState = surface->m_data.surface->m_current;
    g_state->textureInfo = std::format("{}x{} type={} scale={} viewportSource={} viewportDest={}",
        surface->m_data.texture ? surface->m_data.texture->m_size.x : 0,
        surface->m_data.texture ? surface->m_data.texture->m_size.y : 0,
        surface->m_data.texture ? int(surface->m_data.texture->m_type) : -1,
        bufferState.scale, bufferState.viewport.hasSource, bufferState.viewport.hasDestination);
    if (bufferState.buffer) {
        const auto attrs = bufferState.buffer->dmabuf();
        if (attrs.success)
            g_state->textureInfo += std::format(" fourcc={:08x} modifier={:x} stride={} planes={}", attrs.format, attrs.modifier, attrs.strides[0], attrs.planes);
    }
    if (!surface->m_data.dontRound || surface->m_data.alpha != 1.F || surface->m_data.fadeAlpha != 1.F || !solitary->opaque()) {
        g_state->lastPassInfo += std::format(" (dontRound={} alpha={} fade={} opaque={})", surface->m_data.dontRound, surface->m_data.alpha, surface->m_data.fadeAlpha, solitary->opaque());
        return nullptr;
    }
    if (!surface->m_data.clipBox.empty() || !surface->m_data.texture || !surface->m_data.texture->ok()) {
        g_state->lastPassInfo += " (clipped or missing texture)";
        return nullptr;
    }
    const CBox box{{0, 0}, monitor->m_transformedSize / monitor->m_scale};
    if (!covers(surface->opaqueRegion(), box)) {
        g_state->lastPassInfo += " (opaque region does not cover output)";
        return nullptr;
    }
    // Wine may have an entirely occluded parent surface below its opaque HDR
    // subsurface. Only ordinary surfaces of this same window may be omitted.
    for (size_t i = 0; i < index; ++i) {
        const auto& below = pass.m_passElements.at(i).element;
        const auto* s = below ? dynamic_cast<CSurfacePassElement*>(below.get()) : nullptr;
        if (!s || s->m_data.pWindow != solitary || s->m_data.popup || below->undiscardable())
            return nullptr;
    }
    return surface;
}

static void considerFrame() {
    if (!g_state || !g_pHyprRenderer || !Render::GL::g_pHyprOpenGL)
        return;
    auto& rd = g_pHyprRenderer->m_renderData;
    const auto monitor = rd.pMonitor.lock();
    if (!monitor) return;
    if (monitor->m_name != g_state->outputName) { return; }
    ++g_state->considered;
    if (!g_state->enabled) {
        g_state->lastReason = "disabled";
        if (g_state->lastWasFast) {
            const CRegion full{CBox{{0, 0}, monitor->m_pixelSize}};
            g_pHyprRenderer->setDamage(full, full);
            g_state->lastWasFast = false;
        }
        return;
    }
    auto& gl = *Render::GL::g_pHyprOpenGL;
    auto* surface = eligibleSurface(monitor);
    static auto cmEnabled = CConfigValue<Config::INTEGER>("render:cm_enabled");
    const CBox outputBox{{0, 0}, monitor->m_pixelSize};
    const bool format10 = monitor->m_drmFormat == DRM_FORMAT_XRGB2101010 || monitor->m_drmFormat == DRM_FORMAT_XBGR2101010;
    Singlepass::SConditions c{
        .enabled = g_state->enabled,
        .targetOutput = true,
        .normalGLFrame = g_pHyprRenderer->m_renderMode == Render::RENDER_MODE_NORMAL && !gl.m_fakeFrame && gl.m_offloadedFramebuffer && rd.outFB && rd.mainFB && rd.currentFB == rd.mainFB,
        .hdrFP16 = monitor->inHDR() && monitor->useFP16() && format10 && *cmEnabled && !monitor->m_imageDescription->value().icc.present,
        .noCapture = !monitor->needsACopyFB() && monitor->m_mirrors.empty() && !(g_state->uniformCapture && Screenshare::mgr()->outputCopyFBState(monitor).sharingSessions > 0),
        .noPostprocessing = !g_pHyprRenderer->m_crashingInProgress && (!gl.m_finalScreenShader || gl.m_finalScreenShader->program() == 0) && rd.mouseZoomFactor == 1.F && rd.renderModif.modifs.empty() && monitor->m_scale == 1 && monitor->m_transform == WL_OUTPUT_TRANSFORM_NORMAL && !monitor->doesNoShaderCM() && monitor->m_ctm == Mat3x3::identity(),
        .oneOpaqueFullscreenSurface = surface != nullptr,
        .fullDamage = covers(rd.damage, outputBox) && covers(rd.finalDamage, outputBox),
        .identityInputConversion = surface && identityInput(*surface, monitor),
    };
    const auto reason = Singlepass::skipReason(c);
    if (!reason.empty()) {
        ++g_state->fallback;
        g_state->lastReason = reason;
        if (g_state->lastWasFast) {
            const CRegion full{outputBox};
            g_pHyprRenderer->setDamage(full, full);
            g_state->lastWasFast = false;
        }
        return;
    }

    // The render pass has only been queued at RENDER_LAST_MOMENT. Its sole,
    // fully opaque surface is now drawn into the real 10-bit output framebuffer.
    // renderToFBInternal reads that framebuffer's image description and performs
    // Hyprland's own scRGB -> monitor conversion. end() then omits its extra blit.
    auto& saved = g_state->restore;
    saved.monitor = monitor;
    saved.output = rd.outFB;
    saved.work = rd.mainFB;
    saved.description = rd.outFB->imageDescription();
    saved.surface = surface;
    saved.blur = surface->m_data.blur;
    saved.pass = &g_pHyprRenderer->currentPass();
    saved.surfaceIndex = g_state->candidateIndex;
    saved.cursorIndex = g_state->candidateCursor;
    if (saved.cursorIndex)
        saved.cursor = dynamic_cast<CTexPassElement*>(saved.pass->m_passElements.at(*saved.cursorIndex).element.get());
    saved.originalPass = std::move(saved.pass->m_passElements);
    Render::CRenderPass::SPassElementData optimizedPass{};
    optimizedPass.element = makeUnique<CSinglepassElement>();
    saved.pass->m_passElements.emplace_back(std::move(optimizedPass));
    for (size_t i = 0; i < saved.surfaceIndex; ++i) {
        saved.originalPass[i].discard = true;
        saved.originalPass[i].element->discard();
    }
    rd.outFB->setImageDescription(monitor->m_imageDescription);
    surface->m_data.blur = false; // An opaque fullscreen surface hides its backdrop.
    g_pHyprRenderer->bindFB(rd.outFB);
    rd.mainFB = rd.outFB;
    gl.m_offloadedFramebuffer = false;
    ++g_state->optimized;
    g_state->lastWasFast = true;
    g_state->lastReason = "single pass";
}

static std::string status(eHyprCtlOutputFormat format, std::string request) {
    if (!g_state)
        return "not initialized";
    std::string profile = "[";
    for (auto [read, convert] : g_state->profileSamples) {
        if (profile.size() > 1) profile += ",";
        profile += std::format("[{},{}]", read, convert);
    }
    profile += "]";
    auto result = std::format("{{\"enabled\":{},\"output\":\"{}\",\"considered\":{},\"optimized\":{},\"fallback\":{},\"last_reason\":\"{}\",\"last_input\":\"{}\",\"last_pass\":\"{}\",\"render_engine_ns\":{},\"shader_mode\":\"{}\",\"shader_frames\":{},\"shader_error\":\"{}\",\"texture\":\"{}\",\"profile_ns\":{}",
                       g_state->enabled, g_state->outputName, g_state->considered, g_state->optimized, g_state->fallback, g_state->lastReason, g_state->lastInput, g_state->lastPassInfo, renderEngineNanoseconds(), g_state->shaderMode, g_state->shaderFrames, g_state->shaderError, g_state->textureInfo, profile);
    result += std::format(",\"uniform_capture\":{}", g_state->uniformCapture);
    result += std::format(",\"cached_capture\":{},\"cached_capture_copies\":{}", g_state->cachedCapture, g_state->cachedCaptureCopies);
    result += std::format(",\"capture_observed_surfaces\":{},\"capture_pacing\":{},\"capture_coalesced\":{},\"capture_timeouts\":{}",g_state->contentWatches.size(),g_state->capturePacing,g_state->captureCoalesced,g_state->captureTimeouts);
    return result + std::format(",\"plugin\":\"hyprland-fix-hdr\",\"version\":\"{}\"}}",HDRFix::VERSION);
}

// share() normally adds stale mirror-FB damage and schedules a KMS redraw even
// if the game has not committed new content. For an actively updating offload
// client, let the next genuine output update build the mirror and satisfy the
// pending capture. Preserve damage that predates share(), including cursors and
// other windows. A 25 ms deadline restores normal damage if the game stops.
static void flushCaptureWaits() {
    if (!g_state) return;
    auto waits = std::move(g_state->captureWaits);
    g_state->captureWaits.clear();
    for (const auto& wait : waits) {
        // CScreenshareFrame is unique-owned: its weak pointer is not lockable.
        // This callback runs on the same event-loop thread as frame destruction.
        if (!pendingCaptureFrame(wait.frame)) continue;
        auto monitor = wait.monitor.lock();
        if (!monitor || !monitor->m_output) continue;
        ++g_state->captureTimeouts;
        g_pHyprRenderer->damageMonitor(monitor);
    }
}
using FShare = Screenshare::eScreenshareError (*)(Screenshare::CScreenshareFrame*, SP<IHLBuffer>, const CRegion&, Screenshare::FScreenshareCallback);
using FNeedsCopyFB = bool (*)(Monitor::CMonitor*);
static bool captureNeedsCopyFB(Monitor::CMonitor* monitor) {
    const auto original = reinterpret_cast<FNeedsCopyFB>(g_copyFBHook->m_original);
    if (original(monitor)) return true;
    // Maintain the mirror only while a real session is sharing this output.
    // This never schedules an output frame or opens an NVIDIA device.
    return g_state && g_state->cachedCapture && g_state->capturePacing &&
        monitor->m_name == g_state->outputName &&
        Screenshare::mgr()->outputCopyFBState(monitor->m_self.lock()).sharingSessions > 0;
}
static bool freshCaptureMirror(PHLMONITOR monitor) {
    return monitor && !monitor->m_renderingActive && !monitor->m_forceFullFrames &&
        monitor->resources()->hasMirrorFB() && monitor->resources()->pendingMirrorFBDamage().empty();
}
static void copyReadyCaptures() {
    if (!g_state) return;
    auto ready = std::move(g_state->captureReady);
    g_state->captureReady.clear();
    if (!g_state->cachedCapture || !g_state->capturePacing) return;
    for (const auto& wait : ready) {
        if (!pendingCaptureFrame(wait.frame)) continue;
        auto monitor = wait.monitor.lock();
        if (!freshCaptureMirror(monitor)) continue; // Ordinary commit/deadline will satisfy it.
        // Borrow a unique-owned frame on the compositor event-loop thread.
        // Defer until share() has returned and the protocol retained its buffer.
        // copy() keeps the native permission, cursor, color and completion paths.
        ++g_state->cachedCaptureCopies;
        traceEvent("capture_cached");
        wait.frame.get()->copy();
    }
}
static Screenshare::eScreenshareError pacedShare(Screenshare::CScreenshareFrame* frame, SP<IHLBuffer> buffer, const CRegion& damage, Screenshare::FScreenshareCallback callback) {
    auto original = reinterpret_cast<FShare>(g_shareHook->m_original);
    if (!g_state || !g_state->capturePacing || !frame->m_session || frame->m_isFirst ||
        (frame->m_session->m_type != Screenshare::SHARE_MONITOR && frame->m_session->m_type != Screenshare::SHARE_REGION))
        return original(frame, std::move(buffer), damage, std::move(callback));
    const auto monitor = frame->m_session->monitor();
    const auto activity = monitor ? g_state->contentActivity.find(monitor.get()) : g_state->contentActivity.end();
    if (!monitor || !monitor->m_output || monitor->m_renderingActive || monitor->m_forceFullFrames ||
        activity == g_state->contentActivity.end() ||
        std::chrono::steady_clock::now() - activity->second.lastCommit > std::chrono::milliseconds(15))
        return original(frame, std::move(buffer), damage, std::move(callback));
    const auto previousDamage = monitor->m_damage.m_current;
    const bool previousNeedsFrame = monitor->m_output->needsFrame;
    const auto result = original(frame, std::move(buffer), damage, std::move(callback));
    if (result != Screenshare::ERROR_NONE) return result;
    monitor->m_damage.m_current = previousDamage;
    monitor->m_output->needsFrame = previousNeedsFrame;
    ++g_state->captureCoalesced;
    traceEvent("capture_wait");
    std::erase_if(g_state->captureWaits, [](const auto& wait) { return !pendingCaptureFrame(wait.frame); });
    const bool arm = g_state->captureWaits.empty();
    g_state->captureWaits.push_back({frame->m_self, PHLMONITORREF{monitor}});
    if (g_state->cachedCapture && monitor->m_name == g_state->outputName &&
        frame->m_buffer->dmabuf().success && freshCaptureMirror(monitor)) {
        g_state->captureReady.push_back({frame->m_self, PHLMONITORREF{monitor}});
        g_state->captureReadyTimer->updateTimeout(std::chrono::milliseconds(0));
    }
    if (arm) g_state->captureTimer->updateTimeout(std::chrono::milliseconds(25));
    return result;
}
static bool setCapturePacing(bool enable) {
    if (enable && !g_shareHook) {
        std::vector<SFunctionMatch> matches;
        for (const auto& match : HyprlandAPI::findFunctionsByName(g_handle, "share"))
            if (match.demangled.find("Screenshare::CScreenshareFrame::share(") != std::string::npos) matches.push_back(match);
        if (matches.size() != 1) return false;
        g_shareHook = HyprlandAPI::createFunctionHook(g_handle, matches[0].address, reinterpret_cast<void*>(&pacedShare));
        if (!g_shareHook || !g_shareHook->hook()) {
            if (g_shareHook) HyprlandAPI::removeFunctionHook(g_handle, g_shareHook);
            g_shareHook = nullptr; return false;
        }
    }
    g_state->capturePacing = enable;
    if (!enable) {
        if (g_state->captureReadyTimer) g_state->captureReadyTimer->updateTimeout(std::nullopt);
        g_state->captureReady.clear();
        if (g_state->captureTimer) g_state->captureTimer->updateTimeout(std::nullopt);
        flushCaptureWaits();
        if (g_shareHook) {
            g_shareHook->unhook(); HyprlandAPI::removeFunctionHook(g_handle, g_shareHook); g_shareHook = nullptr;
        }
    }
    return true;
}

static void forceTargetRepaint() {
    if (!g_state || !g_pHyprRenderer || !State::monitorState())
        return;
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor || monitor->m_name != g_state->outputName)
            continue;
        monitor->m_forceFullFrames = std::max(monitor->m_forceFullFrames, 2);
        g_pHyprRenderer->damageMonitor(monitor);
        monitor->scheduleFrame(Aquamarine::IOutput::AQ_SCHEDULE_DAMAGE);
    }
}

static SDispatchResult dispatch(std::string args) {
    if (args == "on")
        g_state->enabled = true;
    else if (args == "off")
        g_state->enabled = false;
    else if (args == "uniform-capture-on" || args == "uniform-capture-off") {
        g_state->uniformCapture = args == "uniform-capture-on";
    } else if (args == "cached-capture-on" || args == "cached-capture-off") {
        g_state->cachedCapture = args == "cached-capture-on";
    } else if (args == "capture-pacing-on" || args == "capture-pacing-off") {
        if (!setCapturePacing(args == "capture-pacing-on")) return {.success = false, .error = "Cannot install unambiguous screenshare hook"};
    } else if (args == "reset") {
        g_state->considered = g_state->optimized = g_state->fallback = g_state->shaderFrames = 0;
    } else if (args == "profile") {
        if (!g_state->pendingQueries.empty()) return {.success = false, .error = "Queries still pending"};
        g_state->profileSamples.clear();
        g_state->profileIssued = 0;
        g_state->shaderMode = "profile";
        g_state->enabled = true;
        g_state->shaderError.clear();
    } else if (args == "stock" || args == "specialized" || args == "lut" || args == "nearest" || args == "fetch") {
        g_state->shaderMode = args;
        g_state->shaderError.clear();
    } else
        return {.success = false, .error = "Use hdrfix on|off|reset|stock|specialized|lut"};
    if (args == "on" || args == "off")
        forceTargetRepaint();
    Log::logger->log(Log::INFO, "[hyprland-fix-hdr:offload] {}", status(FORMAT_JSON, ""));
    return {};
}

static std::string controlRequest(eHyprCtlOutputFormat format, std::string request) {
    std::istringstream words(request);
    std::string name, action, extra;
    words >> name >> action >> extra;
    if (!extra.empty())
        return "error: unexpected extra arguments";
    if (action == "trace-read") {
        std::string rows = "[";
        for (const auto& row : g_state->traceRows) { if (rows.size() > 1) rows += ","; rows += row; }
        return rows + "]";
    }
    if (action == "trace-on") {
        g_state->traceCommit.reset(); g_state->tracePresent.reset(); g_state->traceCursor.reset();
        for (const auto& monitor : State::monitorState()->monitors()) {
            if (!monitor || monitor->m_name != g_state->outputName) continue;
            g_state->traceRows.clear(); g_state->traceRows.reserve(1536);
            g_state->traceStart = std::chrono::steady_clock::now(); g_state->tracing = true;
            g_state->traceCommit = monitor->m_events.commit.listen([weak = PHLMONITORREF{monitor}] { if (weak) traceEvent("commit", weak.lock()); });
            g_state->tracePresent = monitor->m_events.presented.listen([](const Time::steady_tp& when) { traceEvent("present", nullptr, when); });
            g_state->traceCursor = Pointer::mgr()->m_events.cursorChanged.listen([] { if (g_state) { ++g_state->cursorGeneration; traceEvent("cursor"); } });
            return "ok";
        }
        return "error: target monitor missing";
    }
    if (action == "trace-off") { g_state->tracing = false; return "ok"; }
    if (action.empty() || action == "status")
        return status(format, request);
    const auto result = dispatch(action);
    return result.success ? "ok" : "error: " + result.error;
}

void HDRFix::initOffload(HANDLE handle) {
    const std::string serverHash = __hyprland_api_get_hash();
    const std::string clientHash = __hyprland_api_get_client_hash();
    if (serverHash != clientHash)
        throw std::runtime_error(std::format("hyprland-fix-hdr: ABI mismatch: server={} client={}", serverHash, clientHash));
    g_handle = handle;
    g_state = makeUnique<SState>();
    g_state->captureTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { flushCaptureWaits(); }, nullptr);
    g_pEventLoopManager->addTimer(g_state->captureTimer);
    g_state->captureReadyTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) { copyReadyCaptures(); }, nullptr);
    g_pEventLoopManager->addTimer(g_state->captureReadyTimer);
    std::vector<SFunctionMatch> copyFBMatches;
    for (const auto& match : HyprlandAPI::findFunctionsByName(g_handle, "needsACopyFB"))
        if (match.demangled == "Monitor::CMonitor::needsACopyFB()") copyFBMatches.push_back(match);
    if (copyFBMatches.size() != 1) throw std::runtime_error("hyprland-fix-hdr: ambiguous copy-FB hook");
    g_copyFBHook = HyprlandAPI::createFunctionHook(g_handle, copyFBMatches[0].address, reinterpret_cast<void*>(&captureNeedsCopyFB));
    if (!g_copyFBHook || !g_copyFBHook->hook()) throw std::runtime_error("hyprland-fix-hdr: cannot hook copy-FB policy");
    if (!setCapturePacing(true)) throw std::runtime_error("hyprland-fix-hdr: cannot enable independent capture pacing");
    g_state->stageListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage == RENDER_LAST_MOMENT) {
            const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
            if (monitor && monitor->m_name == g_state->outputName) traceEvent("render", monitor);
            if(g_state->capturePacing && monitor)for(const auto& item:g_pHyprRenderer->currentPass().m_passElements)
                if(auto* surface=item.element?dynamic_cast<CSurfacePassElement*>(item.element.get()):nullptr;surface && surface->m_data.surface)
                    observeContentSurface(surface->m_data.surface,monitor);
            considerFrame();
        }
        else if (stage == RENDER_POST)
            restoreFrameMetadata();
        });
    if (!HyprlandAPI::addDispatcherV2(handle, "hdrfix", dispatch) ||
        !HyprlandAPI::addDispatcherV2(handle, "hdrsinglepass", dispatch))
        throw std::runtime_error("hyprland-fix-hdr: cannot register dispatcher");
    g_state->command = HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{.name = "hdrfix", .exact = false, .fn = controlRequest});
    g_state->legacyCommand = HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{.name = "hdrsinglepass", .exact = false, .fn = controlRequest});
    if (!g_state->command || !g_state->legacyCommand)
        throw std::runtime_error("hyprland-fix-hdr: cannot register status command");

}

void HDRFix::exitOffload() {
    if (!g_state)
        return;
    g_state->enabled = false;
    g_state->contentWatches.clear(); g_state->contentActivity.clear();
    setCapturePacing(false);
    if (g_copyFBHook) { g_copyFBHook->unhook(); HyprlandAPI::removeFunctionHook(g_handle, g_copyFBHook); g_copyFBHook = nullptr; }
    if (g_state->captureReadyTimer) g_pEventLoopManager->removeTimer(g_state->captureReadyTimer);
    if (g_state->captureTimer) g_pEventLoopManager->removeTimer(g_state->captureTimer);
    g_state->traceCommit.reset(); g_state->tracePresent.reset(); g_state->traceCursor.reset();
    restoreFrameMetadata();
    forceTargetRepaint();
    g_state->stageListener.reset();
    if (g_state->command)
        HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_state->command);
    if (g_state->legacyCommand) HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_state->legacyCommand);
    HyprlandAPI::removeDispatcher(g_handle, "hdrfix");
    HyprlandAPI::removeDispatcher(g_handle, "hdrsinglepass");
    if (Render::GL::g_pHyprOpenGL) {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        for (const auto query : g_state->pendingQueries) {
            const GLuint ids[]{query.read, query.convert};
            glDeleteQueries(2, ids);
        }
        g_state->fastShaders.clear();
        if (g_state->pqTexture) glDeleteTextures(1, &g_state->pqTexture);
    }
    g_state.reset();
}
