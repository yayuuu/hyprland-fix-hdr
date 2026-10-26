#define WLR_USE_UNSTABLE

#include <bits/stdc++.h>

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
#include "../offload/client.hpp"

static_assert(std::string_view{GIT_COMMIT_HASH} == "efb50993780079460b0cbed1363e2166a2de1d9f", "Build against Hyprland v0.56.2 headers");
static_assert(std::string_view{HDR_SINGLEPASS_AQ_VERSION}.substr(0, 4) == "0.15", "Build against Aquamarine 0.15.x headers");

static HANDLE g_handle = nullptr;
static CFunctionHook* g_shareHook = nullptr;
static CFunctionHook* g_passHook = nullptr;
static CFunctionHook* g_copyFBHook = nullptr;

struct SFrameRestore {
    PHLMONITOR monitor;
    std::optional<unsigned> offloadSlot;
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
    SP<CEventLoopTimer> offloadTimer;
    struct SRetainedInput {
        Offload::FD fence;
        CHLBufferReference input;
        wl_event_source* source = nullptr;
        SRetainedInput(Offload::FD&& fd, const CHLBufferReference& buffer) : fence(std::move(fd)), input(buffer) {}
        ~SRetainedInput() { if (source) wl_event_source_remove(source); }
    };
    std::vector<UP<SRetainedInput>> retainedInputs;
    struct SNVSlot {
        SP<Render::ITexture> texture;
        Offload::FD readComplete;
        Offload::FD outputFD, conversionComplete;
        uint32_t width = 0, height = 0, stride = 0, offset = 0;
        bool awaitingFence = false;
    };
    struct SNV {
        Offload::Client client;
        std::array<SNVSlot, 3> slots;
        PHLWINDOWREF owner;
        pid_t ownerPID = 0;
        bool flag = false;
        bool enabled = true;
        bool stopRequested = false;
        bool eager = true;
        bool earlyRelease = false;
        unsigned profileRemaining = 0;
        uint64_t eagerFrames = 0;
        unsigned nextSlot = 0;
        uint64_t frames = 0, reused = 0, windowFrames = 0, captureFrames = 0;
        WP<CWLSurfaceResource> sourceSurface;
        CHyprSignalListener sourceCommit, sourceDestroy, sourceQueued;
        uint64_t generation = 0, convertedGeneration = UINT64_MAX, convertedSource = 0;
        std::array<float, 16> convertedColor{};
        std::optional<unsigned> cachedSlot;
        Vector2D pendingSize;
        std::chrono::steady_clock::time_point sizeSince{};
        std::string reason = "disabled";
        std::string route = "idle";
        std::chrono::steady_clock::time_point seen{}, lastDraw{}, lastCommit{};
    } nv;
    struct SVramInput {
        bool feedback = false, protectNew = false, insideBind = false, nativeLayout = false;
        bool automatic = true;
        std::chrono::steady_clock::time_point retryAfter{};
        pid_t pid = 0;
        dev_t device = 0;
        WP<CWLSurfaceResource> surface;
        PHLWINDOWREF owner;
        SP<Render::ITexture> safeTexture;
        uint64_t protectedTextures = 0, fallbackBinds = 0, blankBinds = 0, feedbackSent = 0;
        struct SProxy {
            WP<Render::ITexture> texture;
            NColorManagement::PImageDescription savedDescription;
            std::optional<decltype(g_pHyprRenderer->m_cmSettingsCache)> savedCMCache;
            std::optional<unsigned> slot;
            bool bound = false, nativeLayout = false;
            std::string layoutReason;
        };
        std::map<Render::GL::CGLTexture*, SProxy> proxies;
        std::string layoutDiagnostic;
    } vram;
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
static bool tryOffloadPrimary(const CRegion& damage);
static void maintainOffload();
static void wrapOffloadSurfaces(bool subpass = false);
static void unwrapOffloadSurfaces(Render::CRenderPass* pass = nullptr);
static void preconvertCommittedFrame();
static bool isProtectedInput(Render::ITexture* texture);
static bool isNativeInput(Render::ITexture* texture);
static std::string describeVramInput(Render::ITexture* texture);
static void traceEvent(const char* event, PHLMONITOR monitor = nullptr, std::optional<Time::steady_tp> timestamp = std::nullopt) {
    if (!g_state || !g_state->tracing || g_state->traceRows.size() >= 1536) return;
    const auto elapsed = std::chrono::duration<double, std::milli>(timestamp.value_or(std::chrono::steady_clock::now()) - g_state->traceStart).count();
    const auto pointer = Pointer::mgr()->position();
    const auto& nv = g_state->nv;
    CBox damage;
    if (monitor && g_pHyprRenderer->m_renderMode == Render::RENDER_MODE_NORMAL)
        damage = g_pHyprRenderer->m_renderData.damage.getExtents();
    g_state->traceRows.push_back(std::format("[\"{}\",{:.4f},{},{},{},{:.1f},{:.1f},{:.0f},{:.0f},{:.0f},{:.0f},{},{}]",
        event, elapsed, nv.generation, nv.convertedGeneration, g_state->cursorGeneration,
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
static void retainCaptureInput(const Hyprutils::OS::CFileDescriptor& fence, const CHLBufferReference& input);
static void requestOffloadStop() {
    if (!g_state) return;
    g_state->nv.stopRequested = true;
    if (g_state->offloadTimer) g_state->offloadTimer->updateTimeout(std::chrono::milliseconds(1));
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

static unsigned nvidiaDescriptors() noexcept {
    try {
        unsigned count = 0;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fdinfo")) {
            std::ifstream file(entry.path());
            const std::string info{std::istreambuf_iterator<char>{file}, {}};
            std::error_code ec;
            const auto target = std::filesystem::read_symlink(std::filesystem::path("/proc/self/fd") / entry.path().filename(), ec);
            if (info.contains("nvidia") || (!ec && target.string().contains("/dev/nvidia"))) ++count;
        }
        return count;
    } catch (...) { return UINT_MAX; }
}


static void restoreFrameMetadata() {
    if (!g_state || !g_state->restore.output)
        return;
    auto& saved = g_state->restore;
    if (saved.offloadSlot) {
        auto& slot = g_state->nv.slots[*saved.offloadSlot];
        slot.awaitingFence = false;
        if (saved.monitor && saved.monitor->m_inFence.isValid())
            slot.readComplete.reset(saved.monitor->m_inFence.duplicate().take());
        else {
            // A failed native-fence export must not allow reuse before the
            // Intel read is finished. This is an error-only shutdown path.
            glFinish();
            requestOffloadStop();
        }
    }
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
static void drawPrimary(const CRegion& damage) {
    auto& saved = g_state->restore;
    auto& gl = *Render::GL::g_pHyprOpenGL;
    auto draw = [&] { g_pHyprRenderer->draw(saved.originalPass.at(saved.surfaceIndex).element, damage); };
    if (g_state->nv.enabled && g_state->shaderMode == "stock" && tryOffloadPrimary(damage)) return;
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
                if (g_state->nv.earlyRelease && saved.offloadSlot)
                    saved.surface->m_data.texture = g_state->nv.slots[*saved.offloadSlot].texture;
                g_pHyprRenderer->m_bBlockSurfaceFeedback = true;
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
    if (element.m_data.texture && !isProtectedInput(element.m_data.texture.get()) && element.m_data.texture->m_imageDescription)
        return element.m_data.texture->m_imageDescription;
    const auto& surface = element.m_data.surface;
    if (surface && surface->m_colorManagement.valid() && surface->m_colorManagement->hasImageDescription())
        return NColorManagement::CImageDescription::from(surface->m_colorManagement->imageDescription());
    return nullptr;
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

static void maintainOffload() {
    if (!g_state) return;
    auto& nv = g_state->nv;
    nv.client.reap();
    const auto owner = nv.owner.lock();
    const auto now = std::chrono::steady_clock::now();
    const bool haveTextures = std::ranges::any_of(nv.slots, [](const auto& slot) { return bool(slot.texture) || slot.outputFD.value >= 0 || slot.conversionComplete.value >= 0; });
    const bool idle = !g_state->enabled || !nv.enabled || !owner || !owner->m_isMapped ||
        now - nv.lastDraw > std::chrono::milliseconds(500);
    if (!nv.stopRequested && !(idle && nv.client.running()) && !(haveTextures && !nv.client.running())) {
        if (nv.client.running()) nv.client.checkReady();
        return;
    }
    nv.stopRequested = true;
    for (const auto& slot : nv.slots) {
        if (slot.awaitingFence) return;
        if (slot.readComplete.value >= 0) {
            pollfd p{slot.readComplete.value, POLLIN, 0};
            if (poll(&p, 1, 0) == 0) return;
        }
    }
    if (haveTextures && Render::GL::g_pHyprOpenGL) Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    for (auto& slot : nv.slots) { slot.texture.reset(); slot.readComplete.reset(); slot.outputFD.reset(); slot.conversionComplete.reset(); slot.awaitingFence = false; }
    nv.client.stop();
    if (!nv.client.running()) {
        nv.stopRequested = false; nv.owner.reset(); nv.ownerPID = 0; nv.flag = false; nv.nextSlot = 0;
        nv.sourceCommit.reset(); nv.sourceDestroy.reset(); nv.sourceQueued.reset(); nv.sourceSurface.reset(); nv.cachedSlot.reset();
        nv.convertedGeneration = UINT64_MAX; nv.route = "idle";
        if (nv.reason == "converted" || nv.reason == "starting") nv.reason = "idle";
    }
}

static bool materializeOffload(SState::SNVSlot& slot, NColorManagement::PImageDescription packed, Hyprutils::Math::eTransform transform) {
    auto fail = [&](const std::string& reason) { g_state->nv.reason = reason; g_state->nv.client.fail(reason); requestOffloadStop(); return false; };
    if (!slot.texture) {
        Aquamarine::SDMABUFAttrs output{}; output.success = true; output.size = {int(slot.width), int(slot.height)}; output.planes = 1;
        output.format = DRM_FORMAT_XBGR2101010; output.modifier = DRM_FORMAT_MOD_LINEAR;
        output.fds[0] = slot.outputFD.value; output.strides[0] = slot.stride; output.offsets[0] = slot.offset;
        slot.texture = g_pHyprRenderer->createTexture(output, true);
        if (!slot.texture || !slot.texture->ok()) return fail("cannot import shared RAM output");
    }
    if (slot.conversionComplete.value >= 0) {
        auto create = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        auto wait = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(eglGetProcAddress("eglWaitSyncKHR"));
        auto destroy = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        if (!create || !wait || !destroy) return fail("native fence operations unavailable");
        const EGLint syncAttrs[]{EGL_SYNC_NATIVE_FENCE_FD_ANDROID, slot.conversionComplete.value, EGL_NONE};
        const auto display = Render::GL::g_pHyprOpenGL->m_eglDisplay;
        auto sync = create(display, EGL_SYNC_NATIVE_FENCE_ANDROID, syncAttrs);
        if (sync == EGL_NO_SYNC_KHR) return fail("cannot import conversion fence");
        slot.conversionComplete.release();
        auto waited = wait(display, sync, 0); destroy(display, sync);
        if (!waited) return fail("cannot wait for conversion");
    }
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    slot.texture->m_imageDescription = packed;
    slot.texture->m_transform = transform;
    if (g_state->vram.pid > 0 && g_state->vram.pid == g_state->nv.ownerPID) g_state->vram.safeTexture = slot.texture;
    return true;
}

static std::optional<unsigned> prepareOffloadTexture(CSurfacePassElement& element, PHLMONITOR monitor, bool fullscreen, bool submissionOnly = false) {
    auto& nv = g_state->nv;
    auto& data = element.m_data;
    if (!g_state->enabled || !nv.enabled || !monitor || !data.texture || !data.texture->ok() || !data.pWindow || !data.surface || !data.surface->m_current.buffer || !g_pHyprRenderer->explicitSyncSupported()) return std::nullopt;
    static auto cmEnabled = CConfigValue<Config::INTEGER>("render:cm_enabled");
    if (!*cmEnabled || monitor->doesNoShaderCM()) return std::nullopt;
    const auto pid = data.pWindow->getPID();
    if ((nv.owner.lock() != data.pWindow || nv.ownerPID != pid) && !Offload::offloadRequested(pid)) return std::nullopt;
    if (nv.owner.lock() != data.pWindow || nv.ownerPID != pid) {
        if (nv.client.running()) { nv.reason = "owner changed"; requestOffloadStop(); return std::nullopt; }
        nv.owner = data.pWindow; nv.ownerPID = pid; nv.flag = Offload::offloadRequested(pid);
    }
    if (!nv.flag) { nv.reason = "client lacks render offload flag"; return std::nullopt; }
    auto attrs = data.surface->m_current.buffer->dmabuf();
    if (!attrs.success || attrs.planes != 1 || attrs.modifier != DRM_FORMAT_MOD_LINEAR || (fullscreen && attrs.size != monitor->m_pixelSize) ||
        (attrs.format != DRM_FORMAT_XBGR16161616F && attrs.format != DRM_FORMAT_ABGR16161616F) ||
        (attrs.format == DRM_FORMAT_ABGR16161616F && !data.texture->m_opaque)) {
        nv.reason = "unsupported DMA-BUF layout"; return std::nullopt;
    }
    const auto source = sourceDescription(element);
    using namespace NColorManagement;
    if (!source || source->value().icc.present || source->value().transferFunction != CM_TRANSFER_FUNCTION_EXT_LINEAR) return std::nullopt;
    // Keep content luminance/mastering metadata. Only the transport encoding
    // changes: ordinary Hyprland composition still owns tone mapping/blending.
    auto packedData = source->value();
    packedData.transferFunction = CM_TRANSFER_FUNCTION_ST2084_PQ;
    packedData.transferFunctionPower = 1.0F;
    packedData.primariesNameSet = true;
    packedData.primariesNamed = CM_PRIMARIES_BT2020;
    packedData.primaries = NColorPrimaries::BT2020;
    const auto packed = fullscreen ? monitor->m_imageDescription : CImageDescription::from(packedData);
    const auto settings = fullscreen ? g_pHyprRenderer->getCMSettings(source, packed, data.surface, true,
        monitor->m_sdrMinLuminance, monitor->m_sdrMaxLuminance, true) :
        g_pHyprRenderer->getCMSettings(source, packed, nullptr, false, -1, -1, false);
    if (settings.sourceTF != CM_TRANSFER_FUNCTION_EXT_LINEAR || settings.targetTF != CM_TRANSFER_FUNCTION_ST2084_PQ ||
        settings.needsTonemap || settings.needsSDRmod || settings.dstTFRange.max <= settings.dstTFRange.min) {
        nv.reason = "unsupported color transform"; return std::nullopt;
    }
    nv.seen = std::chrono::steady_clock::now();
    if (!submissionOnly) { nv.lastDraw = nv.seen; observeContentSurface(data.surface, monitor); }
    if (nv.stopRequested) { maintainOffload(); return std::nullopt; }
    if (nv.pendingSize != attrs.size) {
        nv.pendingSize = attrs.size; nv.sizeSince = nv.seen;
    }
    if (nv.client.running() && (nv.client.width < unsigned(attrs.size.x) || nv.client.height < unsigned(attrs.size.y))) {
        nv.reason = "output exceeds reserved capacity"; requestOffloadStop(); return std::nullopt;
    }
    if (!nv.client.running()) {
        if (nv.seen - nv.sizeSince < std::chrono::milliseconds(200)) { nv.reason = "waiting for stable buffer size"; return std::nullopt; }
        // Keep the same helper/storage across ordinary window-size changes.
        nv.client.start(std::max(attrs.size.x, monitor->m_pixelSize.x),
            std::max(attrs.size.y, monitor->m_pixelSize.y), pid);
        if (g_state->offloadTimer) g_state->offloadTimer->updateTimeout(std::chrono::milliseconds(250));
    }
    nv.client.checkReady();
    if (!nv.client.ready) { nv.reason = nv.client.reason; return std::nullopt; }
    const auto slotIndex = nv.nextSlot % nv.slots.size();
    auto& slot = nv.slots[slotIndex];
    if (slot.awaitingFence) { nv.reason = "waiting for Intel release fence"; return std::nullopt; }
    struct stat inode{};
    if (fstat(attrs.fds[0], &inode)) { nv.reason = "cannot identify source buffer"; return std::nullopt; }
    // Pass wrappers may already have replaced data.texture with our PQ output.
    // Layout belongs to the source DMA-BUF, never to that temporary draw view.
    Offload::Packet job{};
    if (isNativeInput(data.surface->m_current.buffer->m_texture.get())) job.fdFlags |= Offload::NATIVE_INPUT;
    if (g_state->vram.feedback && g_state->vram.nativeLayout && !(job.fdFlags & Offload::NATIVE_INPUT))
        g_state->vram.layoutDiagnostic = std::format("{}x{} stride={} source={} pass={} current={} ", attrs.size.x, attrs.size.y, attrs.strides[0], job.source, (void*)data.texture.get(), (void*)data.surface->m_current.texture.get()) + describeVramInput(data.surface->m_current.buffer->m_texture.get());
    job.width = attrs.size.x; job.height = attrs.size.y; job.format = attrs.format;
    job.stride = attrs.strides[0]; job.offset = attrs.offsets[0]; job.modifier = attrs.modifier;
    job.source = uint64_t(inode.st_ino) ^ (uint64_t(inode.st_dev) << 32); job.slot = slotIndex;
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row) job.color[column * 4 + row] = settings.convertMatrix[row][column];
    job.color[12] = settings.srcTFRange.min; job.color[13] = settings.srcTFRange.max - settings.srcTFRange.min;
    job.color[14] = settings.dstTFRange.min; job.color[15] = settings.dstTFRange.max - settings.dstTFRange.min;
    if (nv.sourceSurface.lock() != data.surface) {
        nv.sourceCommit.reset(); nv.sourceDestroy.reset(); nv.sourceQueued.reset(); nv.cachedSlot.reset();
        nv.sourceSurface = data.surface; ++nv.generation;
        nv.sourceCommit = data.surface->m_events.commit.listen([] { if (g_state) { ++g_state->nv.generation; g_state->nv.lastCommit = std::chrono::steady_clock::now(); traceEvent("source"); preconvertCommittedFrame(); } });
        nv.sourceQueued = data.surface->m_events.stateCommit2.listen([](WP<SSurfaceState> state) {
            if (g_state && g_state->tracing && !state.expired() && state->updated.bits.buffer)
                traceEvent(state->surfaceLocked ? "queued_fifo" : "queued");
        });
        nv.sourceDestroy = data.surface->m_events.destroy.listen([] { if (g_state) { g_state->nv.cachedSlot.reset(); requestOffloadStop(); } });
    }
    if (nv.cachedSlot && nv.convertedGeneration == nv.generation && nv.convertedSource == job.source && nv.convertedColor == job.color) {
        auto& cached = nv.slots[*nv.cachedSlot];
        if (!submissionOnly && !materializeOffload(cached, packed, data.texture->m_transform)) return std::nullopt;
        ++nv.reused; nv.reason = "reused committed frame";
        return nv.cachedSlot;
    }
    Offload::FD acquire;
    if (data.surface->m_current.acquire) {
        auto fd = data.surface->m_current.acquire.exportAsFD();
        if (!fd.isValid()) { nv.reason = "cannot export client acquire fence"; return std::nullopt; }
        acquire.reset(fd.take());
    } else {
        dma_buf_export_sync_file exportSync{}; exportSync.flags = DMA_BUF_SYNC_READ; exportSync.fd = -1;
        if (ioctl(attrs.fds[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exportSync)) {
            nv.reason = "cannot export implicit acquire fence"; return std::nullopt;
        }
        acquire.reset(exportSync.fd);
    }
    std::vector<int> descriptors{attrs.fds[0]};
    if (acquire.value >= 0) { job.fdFlags |= Offload::ACQUIRE; descriptors.push_back(acquire.value); }
    if (slot.readComplete.value >= 0) { job.fdFlags |= Offload::REUSE; descriptors.push_back(slot.readComplete.value); }
    const auto input = data.surface->m_current.buffer;
    if (nv.profileRemaining) { --nv.profileRemaining; job.fdFlags |= Offload::PROFILE; }
    auto retain = [input] {};
    Offload::Packet response; std::vector<Offload::FD> received;
    if (!nv.client.convert(job, descriptors, response, received, retain)) {
        nv.reason = nv.client.reason; requestOffloadStop(); return std::nullopt;
    }
    auto fail = [&](const std::string& reason) {
        nv.client.hold(retain); nv.client.fail(reason); nv.reason = reason; requestOffloadStop();
        return std::nullopt;
    };
    if (response.width != unsigned(attrs.size.x) || response.height != unsigned(attrs.size.y) || response.modifier != 0 ||
        response.format != DRM_FORMAT_XBGR2101010 || response.stride < attrs.size.x * 4)
        return fail("invalid output layout");
    if (slot.width != response.width || slot.height != response.height || slot.stride != response.stride || slot.offset != response.offset)
        slot.texture.reset(); // Reimport the persistent buffer at its new logical size.
    slot.outputFD = std::move(received[0]);
    slot.width = response.width; slot.height = response.height; slot.stride = response.stride; slot.offset = response.offset;
    slot.conversionComplete.reset();
    if (response.fdFlags & Offload::COMPLETE) slot.conversionComplete = std::move(received[1]);
    // Source lifetime must cover conversion even when no monitor draws this
    // commit (occlusion/newer commit). No GL commands run from a commit callback.
    if (slot.conversionComplete.value >= 0) {
        Hyprutils::OS::CFileDescriptor completion{dup(slot.conversionComplete.value)};
        if (!completion.isValid()) return fail("cannot retain source completion fence");
        for (const auto& releaser : input->m_syncReleasers) releaser->addSyncFileFd(completion);
        retainCaptureInput(completion, input);
    }
    if (!submissionOnly && !materializeOffload(slot, packed, data.texture->m_transform)) return std::nullopt;
    nv.cachedSlot = slotIndex; nv.convertedGeneration = nv.generation;
    nv.convertedSource = job.source; nv.convertedColor = job.color;
    ++nv.nextSlot; ++nv.frames; nv.reason = "converted";
    return slotIndex;
}

#include "../offload/vram_input.hpp"

static void preconvertCommittedFrame() {
    if (!g_state) return;
    auto& nv = g_state->nv;
    if (!nv.eager || !g_state->enabled || !nv.enabled || nv.stopRequested || !nv.client.ready ||
        std::chrono::steady_clock::now() - nv.lastDraw > std::chrono::milliseconds(250)) return;
    const auto owner = nv.owner.lock();
    const auto surface = nv.sourceSurface.lock();
    if (!owner || !owner->m_isMapped || !surface || !surface->m_current.buffer || !surface->m_current.texture) return;
    const auto monitor = owner->m_monitor.lock();
    if (!monitor) return;
    CSurfacePassElement::SRenderData data{}; data.surface = surface; data.texture = surface->m_current.texture; data.pWindow = owner;
    CSurfacePassElement element{data};
    const auto before = nv.frames;
    prepareOffloadTexture(element, monitor, false, true);
    if (nv.frames > before) ++nv.eagerFrames;
}

static bool tryOffloadPrimary(const CRegion& damage) {
    auto& saved = g_state->restore;
    const auto prepared = prepareOffloadTexture(*saved.surface, saved.monitor, true);
    if (!prepared) return false;
    auto& slot = g_state->nv.slots[*prepared];
    auto& data = saved.surface->m_data;
    auto original = data.texture;
    {
        const auto used = g_pHyprRenderer->m_usedAsyncBuffers.size();
        Render::CScopeGuard restore([&] {
            data.texture = original;
            if (g_state->nv.earlyRelease) g_pHyprRenderer->m_usedAsyncBuffers.resize(used);
        });
        data.texture = slot.texture;
        g_pHyprRenderer->draw(saved.originalPass.at(saved.surfaceIndex).element, damage);
    }
    slot.readComplete.reset(); slot.awaitingFence = true;
    saved.offloadSlot = *prepared;
    g_state->nv.route = "fullscreen";
    return true;
}

// Own and cancel capture input waiters explicitly. Event-loop callbacks whose
// code lives in the plugin must never survive PLUGIN_EXIT/dlclose.
static void retainCaptureInput(const Hyprutils::OS::CFileDescriptor& fence, const CHLBufferReference& input) {
    auto duplicate = fence.duplicate();
    pollfd p{duplicate.get(), POLLIN, 0};
    if (poll(&p, 1, 0) > 0) return;
    auto held = makeUnique<SState::SRetainedInput>(Offload::FD{duplicate.take()}, input);
    held->source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, held->fence.value, WL_EVENT_READABLE,
        [](int, uint32_t mask, void* data) -> int {
            if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) requestOffloadStop();
            std::erase_if(g_state->retainedInputs, [data](const auto& item) { return item.get() == data; });
            return 0;
        }, held.get());
    if (!held->source) {
        // A submission-only callback has no GL wait. Retain the source until
        // the worker has been reaped rather than finishing the GL context.
        g_state->nv.client.hold([input] {}); requestOffloadStop(); return;
    }
    g_state->retainedInputs.emplace_back(std::move(held));
}

// A pass-local wrapper is also used for window capture, which renders its own
// pass without RENDER_LAST_MOMENT/RENDER_POST. It never changes wl_surface state.
class COffloadSurfaceElement : public IPassElement {
  public:
    UP<IPassElement> original;
    bool subpass = false;
    explicit COffloadSurfaceElement(UP<IPassElement>&& element, bool early = false) : original(std::move(element)), subpass(early) {}
    CSurfacePassElement* surface() const { return dynamic_cast<CSurfacePassElement*>(original.get()); }
    bool needsLiveBlur() override { return original->needsLiveBlur(); }
    bool needsPrecomputeBlur() override { return original->needsPrecomputeBlur(); }
    bool undiscardable() override { return original->undiscardable(); }
    bool disableSimplification() override { return original->disableSimplification(); }
    std::optional<CBox> boundingBox() override { return original->boundingBox(); }
    CRegion opaqueRegion() override { return original->opaqueRegion(); }
    void discard() override { original->discard(); }
    const char* passName() override { return "CHDROffloadSurfaceElement"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::vector<UP<IPassElement>> draw() override {
        auto& rd = g_pHyprRenderer->m_renderData;
        const auto damage = rd.damage;
        auto* element = surface();
        const auto source = sourceDescription(*element);
        const auto prepared = prepareOffloadTexture(*element, rd.pMonitor.lock(), false);
        if (!prepared) { g_pHyprRenderer->draw(original, damage); return {}; }
        auto& slot = g_state->nv.slots[*prepared];
        auto texture = element->m_data.texture;
        {
            const auto used = g_pHyprRenderer->m_usedAsyncBuffers.size();
            Render::CScopeGuard restore([&] {
                element->m_data.texture = texture;
                if (g_state->nv.earlyRelease) g_pHyprRenderer->m_usedAsyncBuffers.resize(used);
            });
            element->m_data.texture = slot.texture;
            // Hyprland treats PQ -> SDR differently from scRGB -> SDR. Keep the
            // original content tone-map parameters while decoding packed PQ.
            // All primaries/TF ranges still describe the actual packed texture.
            const auto cache = g_pHyprRenderer->m_cmSettingsCache;
            const auto previousSurface = rd.surface;
            const auto previousWindow = rd.currentWindow;
            rd.surface = element->m_data.surface;
            rd.currentWindow = element->m_data.pWindow;
            Render::CScopeGuard restoreCM([&] {
                g_pHyprRenderer->m_cmSettingsCache = cache;
                rd.surface = previousSurface; rd.currentWindow = previousWindow;
            });
            const auto target = rd.currentFB->imageDescription();
            auto originalSettings = g_pHyprRenderer->getCMSettings(source, target, element->m_data.surface, true,
                rd.pMonitor->m_sdrMinLuminance, rd.pMonitor->m_sdrMaxLuminance, true);
            const auto packed = slot.texture->m_imageDescription;
            auto packedSettings = g_pHyprRenderer->getCMSettings(packed, target, element->m_data.surface, true,
                rd.pMonitor->m_sdrMinLuminance, rd.pMonitor->m_sdrMaxLuminance, true);
            packedSettings.needsTonemap = originalSettings.needsTonemap;
            packedSettings.tonemapMode = originalSettings.tonemapMode;
            packedSettings.maxLuminance = originalSettings.maxLuminance;
            packedSettings.dstMaxLuminance = originalSettings.dstMaxLuminance;
            for (auto& entry : g_pHyprRenderer->m_cmSettingsCache)
                if (entry.srcDescId == packed->id() && entry.dstDescId == target->id()) entry.settings = packedSettings;
            g_pHyprRenderer->draw(original, damage);
        }
        // Dedicated read fence works for normal composition AND independent
        // capture passes, including FULL_FAKE passes with no monitor in-fence.
        auto fence = g_pHyprRenderer->createSyncFDManager();
        if (fence && fence->isValid()) {
            if (g_pHyprRenderer->m_renderMode == Render::RENDER_MODE_FULL_FAKE && !g_state->nv.earlyRelease) {
                // FULL_FAKE skips the core's async-buffer release fence. Protect
                // the source from client reuse until our conversion has ended.
                const auto input = element->m_data.surface->m_current.buffer;
                for (const auto& releaser : input->m_syncReleasers) releaser->addSyncFileFd(fence->fd());
                retainCaptureInput(fence->fd(), input);
            }
            slot.readComplete.reset(fence->takeFd().take());
        } else { glFinish(); requestOffloadStop(); }
        ++g_state->nv.windowFrames;
        if (subpass) ++g_state->subpassFrames;
        g_state->nv.route = subpass ? "subpass composition" : (g_pHyprRenderer->m_renderMode == Render::RENDER_MODE_NORMAL ? "composition" : "window capture");
        if (g_pHyprRenderer->m_renderMode != Render::RENDER_MODE_NORMAL) ++g_state->nv.captureFrames;
        return {};
    }
};

static void wrapOffloadSurfaces(bool subpass) {
    if (!g_state || !g_state->enabled || !g_state->nv.enabled || !Render::GL::g_pHyprOpenGL) return;
    // Only the main transient render pass is owned here. Nested transformer
    // passes may outlive this frame and must not retain plugin vtables.
    if (&g_pHyprRenderer->currentPass() != &g_pHyprRenderer->m_renderPass) return;
    for (auto& item : g_pHyprRenderer->currentPass().m_passElements) {
        auto* surface = item.element ? dynamic_cast<CSurfacePassElement*>(item.element.get()) : nullptr;
        if (!surface || !surface->m_data.pWindow || !surface->m_data.texture) continue;
        const auto source = sourceDescription(*surface);
        if (!source || source->value().transferFunction != NColorManagement::CM_TRANSFER_FUNCTION_EXT_LINEAR) continue;
        item.element = makeUnique<COffloadSurfaceElement>(std::move(item.element), subpass);
    }
}

static void unwrapOffloadSurfaces(Render::CRenderPass* pass) {
    if (!pass) pass = &g_pHyprRenderer->currentPass();
    for (auto& item : pass->m_passElements)
        if (auto* wrapped = item.element ? dynamic_cast<COffloadSurfaceElement*>(item.element.get()) : nullptr) {
            auto original = std::move(wrapped->original);
            item.element = std::move(original);
        }
}

// Overview flushes its pass before RENDER_LAST_MOMENT, after adjusting child
// geometry. Intercept actual execution so those edits see ordinary elements.
// Wrappers are removed before returning; no persistent plugin-owned vtables.
using FRenderPass = CRegion (*)(Render::CRenderPass*, const CRegion&);
static CRegion renderOffloadPass(Render::CRenderPass* pass, const CRegion& damage) {
    auto original = reinterpret_cast<FRenderPass>(g_passHook->m_original);
    const bool main = g_state && g_pHyprRenderer && pass == &g_pHyprRenderer->m_renderPass;
    if (!main) return original(pass, damage);
    wrapOffloadSurfaces(true);
    Render::CScopeGuard restore([pass] { unwrapOffloadSurfaces(pass); });
    auto result = original(pass, damage);
    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (g_state->capturePacing && monitor) {
        for (const auto& item : pass->m_passElements) {
            if (item.discard || !item.element) continue;
            auto* surface = dynamic_cast<CSurfacePassElement*>(item.element.get());
            if (auto* wrapped = dynamic_cast<COffloadSurfaceElement*>(item.element.get())) surface = wrapped->surface();
            if (surface && surface->m_data.pWindow && surface->m_data.surface) observeContentSurface(surface->m_data.surface, monitor);
        }
    }
    return result;
}
static bool installPassHook() {
    std::vector<SFunctionMatch> matches;
    for (const auto& match : HyprlandAPI::findFunctionsByName(g_handle, "render"))
        if (match.demangled.find("Render::CRenderPass::render(") != std::string::npos) matches.push_back(match);
    if (matches.size() != 1) return false;
    g_passHook = HyprlandAPI::createFunctionHook(g_handle, matches[0].address, reinterpret_cast<void*>(&renderOffloadPass));
    return g_passHook && g_passHook->hook();
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
    if (monitor->m_name != g_state->outputName) { wrapOffloadSurfaces(); return; }
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
        wrapOffloadSurfaces();
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
    result += std::format(",\"offload_subpass_frames\":{},\"capture_observed_surfaces\":{}", g_state->subpassFrames, g_state->contentWatches.size());
    result += std::format(",\"capture_pacing\":{},\"capture_coalesced\":{},\"capture_timeouts\":{}", g_state->capturePacing, g_state->captureCoalesced, g_state->captureTimeouts);
    result += std::format(",\"offload_early_release\":{}", g_state->nv.earlyRelease);
    result += std::format(",\"offload_eager\":{},\"offload_eager_frames\":{}", g_state->nv.eager, g_state->nv.eagerFrames);
    result += std::format(",\"offload_pipelined\":{},\"offload_pipelined_active\":{}", g_state->nv.client.pipelined, g_state->nv.client.ready && g_state->nv.client.pipelinedActive);
    result += std::format(",\"offload_prefer_local_input\":{}", g_state->nv.client.preferLocalInput);
    result += std::format(",\"vram_input_auto\":{}", g_state->vram.automatic);
    result += std::format(",\"vram_layout_diagnostic\":\"{}\"", g_state->vram.layoutDiagnostic);
    result += std::format(",\"vram_input_requested\":{},\"vram_input_native\":{},\"vram_input_active\":{},\"vram_protected_textures\":{},\"vram_fallback_binds\":{},\"vram_blank_binds\":{}", g_state->vram.feedback, g_state->vram.nativeLayout, g_state->enabled && g_state->nv.enabled && !g_state->nv.stopRequested && g_state->nv.client.ready && g_state->nv.client.sourceDeviceLocal, g_state->vram.protectedTextures, g_state->vram.fallbackBinds, g_state->vram.blankBinds);
    result += std::format(",\"offload_pending_inputs\":{}", g_state->retainedInputs.size());
    result += std::format(",\"offload_route\":\"{}\"", g_state->nv.route);
    result += std::format(",\"offload_reused\":{},\"offload_window_frames\":{},\"offload_capture_frames\":{}", g_state->nv.reused, g_state->nv.windowFrames, g_state->nv.captureFrames);
    result += std::format(",\"plugin\":\"hyprland-fix-hdr\",\"version\":\"{}\",\"screenshare_fix\":true", HDRFix::VERSION);
    return result + std::format(",\"offload_enabled\":{},\"offload_frames\":{},\"offload_owner\":{},\"offload_flag\":{},\"offload_pid\":{},\"offload_worker\":{},\"offload_ready\":{},\"offload_reason\":\"{}\",\"offload_worker_reason\":\"{}\",\"nvidia_gpu_fds\":{}}}",
        g_state->nv.enabled, g_state->nv.frames, g_state->nv.ownerPID, g_state->nv.flag, g_state->nv.client.loggerPID,
        g_state->nv.client.workerPID, g_state->nv.client.ready, g_state->nv.reason, g_state->nv.client.reason, nvidiaDescriptors());
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
        { setVramInput(false); g_state->enabled = false; requestOffloadStop(); }
    else if (args == "offload-on") {
        g_state->enabled = g_state->nv.enabled = true;
        if (!g_state->nv.client.running()) g_state->nv.stopRequested = false;
        g_state->nv.reason = "waiting for flagged HDR window";
    } else if (args == "offload-off") {
        setVramInput(false);
        g_state->nv.enabled = false;
        g_state->nv.reason = "disabled";
        requestOffloadStop();
    }
    else if (args == "vram-input-auto") {
        g_state->vram.automatic = true;
        g_state->vram.retryAfter = {};
        if (g_state->offloadTimer && g_state->nv.client.running()) g_state->offloadTimer->updateTimeout(std::chrono::milliseconds(1));
    }
    else if (args == "vram-input-on" || args == "vram-input-native-on" || args == "vram-input-off") {
        if (!setVramInput(args != "vram-input-off", args == "vram-input-native-on")) return {.success = false, .error = g_vramError};
        g_state->vram.automatic = false;
    } else if (args == "worker-profile") {
        g_state->nv.profileRemaining = 72;
    } else if (args == "pipeline-on" || args == "pipeline-off") {
        const bool enable = args == "pipeline-on";
        if (g_state->nv.client.pipelined != enable) {
            g_state->nv.client.pipelined = enable;
            requestOffloadStop();
        }
    } else if (args == "local-input-on" || args == "local-input-off") {
        const bool enable = args == "local-input-on";
        if (g_state->nv.client.preferLocalInput != enable) {
            g_state->nv.client.preferLocalInput = enable;
            requestOffloadStop();
        }
    } else if (args == "eager-on" || args == "eager-off") {
        g_state->nv.eager = args == "eager-on";
    } else if (args == "early-release-on" || args == "early-release-off") {
        g_state->nv.earlyRelease = args == "early-release-on";
    } else if (args == "uniform-capture-on" || args == "uniform-capture-off") {
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
    if (args == "on" || args == "off" || args == "offload-on" || args == "offload-off")
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
    g_state->offloadTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer> self, void*) {
        maintainOffload();
        updateAutomaticVramInput();
        if (g_state && (g_state->nv.client.running() || g_state->nv.stopRequested)) self->updateTimeout(std::chrono::milliseconds(250));
    }, nullptr);
    g_pEventLoopManager->addTimer(g_state->offloadTimer);
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
    if (!installPassHook()) throw std::runtime_error("hyprland-fix-hdr: cannot hook render-pass execution");
    if (!setCapturePacing(true)) throw std::runtime_error("hyprland-fix-hdr: cannot enable independent capture pacing");
    g_state->closeListener = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
        if (g_state && g_state->vram.owner.lock() == w) { setVramInput(false); g_state->vram.safeTexture.reset(); g_state->vram.owner.reset(); g_state->vram.pid = 0; }
        if (g_state && g_state->nv.owner.lock() == w) requestOffloadStop();
    });
    g_state->stageListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage == RENDER_LAST_MOMENT) {
            const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
            if (monitor && monitor->m_name == g_state->outputName) traceEvent("render", monitor);
            considerFrame();
        }
        else if (stage == RENDER_POST)
            restoreFrameMetadata();
        else if (stage == RENDER_POST_WINDOW && g_pHyprRenderer->m_renderMode != Render::RENDER_MODE_NORMAL)
            wrapOffloadSurfaces();
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
    stopVramInput();
    g_state->enabled = false;
    if (g_passHook) { g_passHook->unhook(); HyprlandAPI::removeFunctionHook(g_handle, g_passHook); g_passHook = nullptr; }
    g_state->contentWatches.clear(); g_state->contentActivity.clear();
    setCapturePacing(false);
    if (g_copyFBHook) { g_copyFBHook->unhook(); HyprlandAPI::removeFunctionHook(g_handle, g_copyFBHook); g_copyFBHook = nullptr; }
    if (g_state->captureReadyTimer) g_pEventLoopManager->removeTimer(g_state->captureReadyTimer);
    if (g_state->captureTimer) g_pEventLoopManager->removeTimer(g_state->captureTimer);
    g_state->traceCommit.reset(); g_state->tracePresent.reset(); g_state->traceCursor.reset();
    restoreFrameMetadata();
    unwrapOffloadSurfaces();
    g_state->nv.sourceCommit.reset(); g_state->nv.sourceDestroy.reset(); g_state->nv.sourceQueued.reset();
    if (g_state->offloadTimer) { g_state->offloadTimer->cancel(); g_pEventLoopManager->removeTimer(g_state->offloadTimer); }
    g_state->closeListener.reset();
    if (Render::GL::g_pHyprOpenGL) {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        glFinish();
    }
    g_state->nv.client.finish();
    g_state->retainedInputs.clear();
    for (auto& slot : g_state->nv.slots) { slot.texture.reset(); slot.readComplete.reset(); slot.outputFD.reset(); slot.conversionComplete.reset(); }
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
