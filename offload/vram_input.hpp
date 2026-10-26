#pragma once
#include <dlfcn.h>

// This file is included after prepareOffloadTexture. All hooks run on the
// compositor's event-loop thread. Core-owned CGLTexture objects are used so
// protected buffers never retain a plugin vtable after unloading.
static CFunctionHook *g_vramCtorHook = nullptr, *g_vramCreateHook = nullptr;
static CFunctionHook *g_vramBindHook = nullptr, *g_vramUnbindHook = nullptr, *g_vramFeedbackHook = nullptr;
static thread_local bool g_vramConstructing = false;
static std::string g_vramError;

static bool isProtectedInput(Render::ITexture* texture) {
    if (!g_state) return false;
    auto* gl = dynamic_cast<Render::GL::CGLTexture*>(texture);
    if (!gl) return false;
    const auto found = g_state->vram.proxies.find(gl);
    return found != g_state->vram.proxies.end() && found->second.texture.lock().get() == texture;
}

static bool isNativeInput(Render::ITexture* texture) {
    if (!g_state) return false;
    auto* gl = dynamic_cast<Render::GL::CGLTexture*>(texture);
    if (!gl) return false;
    const auto found = g_state->vram.proxies.find(gl);
    return found != g_state->vram.proxies.end() && found->second.texture.lock().get() == texture && found->second.nativeLayout;
}

static std::string describeVramInput(Render::ITexture* texture) {
    const auto* gl = dynamic_cast<Render::GL::CGLTexture*>(texture);
    const auto found = g_state->vram.proxies.find(const_cast<Render::GL::CGLTexture*>(gl));
    if (found == g_state->vram.proxies.end()) return std::format("bufferTex={} untracked", (void*)texture);
    return std::format("bufferTex={} live={} native={} {}", (void*)texture, found->second.texture.lock().get() == texture, found->second.nativeLayout, found->second.layoutReason);
}

static void vramBufferConstructor(void* object, uint32_t id, wl_client* client, const Aquamarine::SDMABUFAttrs& attrs) {
    pid_t pid = 0; wl_client_get_credentials(client, &pid, nullptr, nullptr);
    const bool previous = g_vramConstructing;
    g_vramConstructing = g_state && g_state->vram.protectNew && pid == g_state->vram.pid && Offload::offloadRequested(pid);
    Render::CScopeGuard restore([&] { g_vramConstructing = previous; });
    using F = void (*)(void*, uint32_t, wl_client*, const Aquamarine::SDMABUFAttrs&);
    reinterpret_cast<F>(g_vramCtorHook->m_original)(object, id, client, attrs);
}

static SP<Render::ITexture> vramCreateTexture(void* renderer, const Aquamarine::SDMABUFAttrs& attrs, bool opaque) {
    if (!g_vramConstructing || !g_state || attrs.planes != 1 || attrs.modifier != DRM_FORMAT_MOD_LINEAR ||
        (attrs.format != DRM_FORMAT_XBGR16161616F && attrs.format != DRM_FORMAT_ABGR16161616F)) {
        using F = SP<Render::ITexture> (*)(void*, const Aquamarine::SDMABUFAttrs&, bool);
        return reinterpret_cast<F>(g_vramCreateHook->m_original)(renderer, attrs, opaque);
    }
    // Query mapping compatibility on the NVIDIA worker before touching EGL.
    // Ordinary shared/Intel buffers keep the native texture alias and fallback.
    const auto local = g_state->nv.client.inspectSource(attrs.fds[0]);
    if (local && !*local) {
        using F = SP<Render::ITexture> (*)(void*, const Aquamarine::SDMABUFAttrs&, bool);
        return reinterpret_cast<F>(g_vramCreateHook->m_original)(renderer, attrs, opaque);
    }
    // Never import the source FD into Intel EGL. A real core texture with a
    // safe one-pixel backing also makes teardown/failure paths harmless.
    uint8_t black[]{0, 0, 0, 255};
    auto proxy = makeShared<Render::GL::CGLTexture>(DRM_FORMAT_XBGR8888, black, 4, Vector2D{1, 1}, false, true);
    proxy->m_size = attrs.size;
    proxy->m_drmFormat = attrs.format;
    proxy->m_isSynchronous = false;
    proxy->m_type = Render::TEXTURE_RGBX;
    std::erase_if(g_state->vram.proxies, [](const auto& item) { return item.second.texture.expired(); });
    // Native image layout is selected explicitly for this producer. Padding
    // in its advertised row pitch does not turn an optimal image into linear.
    g_state->vram.proxies.emplace(proxy.get(), SState::SVramInput::SProxy{.texture = proxy, .savedDescription = {}, .savedCMCache = {}, .slot = {}, .bound = false, .nativeLayout = local.value_or(false) && g_state->vram.feedback && g_state->vram.nativeLayout, .layoutReason = {}});
    g_state->vram.proxies.at(proxy.get()).layoutReason = std::format("local={} feedback={} requestedNative={} tight={}", local ? (*local ? "yes" : "no") : "unknown", g_state->vram.feedback, g_state->vram.nativeLayout, attrs.strides[0] == attrs.size.x * 8);
    ++g_state->vram.protectedTextures;
    return proxy;
}

static void vramBind(Render::GL::CGLTexture* texture) {
    using F = void (*)(Render::GL::CGLTexture*);
    const auto original = reinterpret_cast<F>(g_vramBindHook->m_original);
    if (!g_state || g_state->vram.insideBind) { original(texture); return; }
    auto found = g_state->vram.proxies.find(texture);
    if (found == g_state->vram.proxies.end()) { original(texture); return; }
    auto& proxy = found->second;
    if (proxy.texture.expired()) { g_state->vram.proxies.erase(found); original(texture); return; }
    auto& state = g_state->vram;
    state.insideBind = true;
    Render::CScopeGuard recursion([&] { state.insideBind = false; });
    proxy.savedDescription = texture->m_imageDescription;
    proxy.bound = true;
    proxy.slot.reset();
    const auto owner = state.owner.lock();
    const auto surface = state.surface.lock();
    const auto monitor = owner ? owner->m_monitor.lock() : nullptr;
    if (owner && owner->m_isMapped && surface && surface->m_current.buffer && monitor &&
        g_state->enabled && g_state->nv.enabled) {
        CSurfacePassElement::SRenderData data{};
        data.surface = surface; data.texture = surface->m_current.texture; data.pWindow = owner;
        CSurfacePassElement element{data};
        if (auto ready = prepareOffloadTexture(element, monitor, false)) {
            proxy.slot = ready;
            state.safeTexture = g_state->nv.slots[*ready].texture;
        }
    }
    auto safe = state.safeTexture;
    if (safe && safe->ok()) {
        auto& rd = g_pHyprRenderer->m_renderData;
        if (surface && monitor && rd.currentFB && surface->m_colorManagement.valid()) {
            const auto source = NColorManagement::CImageDescription::from(surface->m_colorManagement->imageDescription());
            const auto target = rd.currentFB->imageDescription();
            if (target && source) {
                proxy.savedCMCache = g_pHyprRenderer->m_cmSettingsCache;
                const auto originalSettings = g_pHyprRenderer->getCMSettings(source, target, surface, true, monitor->m_sdrMinLuminance, monitor->m_sdrMaxLuminance, true);
                auto packedSettings = g_pHyprRenderer->getCMSettings(safe->m_imageDescription, target, surface, true, monitor->m_sdrMinLuminance, monitor->m_sdrMaxLuminance, true);
                packedSettings.needsTonemap = originalSettings.needsTonemap;
                packedSettings.tonemapMode = originalSettings.tonemapMode;
                packedSettings.maxLuminance = originalSettings.maxLuminance;
                packedSettings.dstMaxLuminance = originalSettings.dstMaxLuminance;
                for (auto& entry : g_pHyprRenderer->m_cmSettingsCache)
                    if (entry.srcDescId == safe->m_imageDescription->id() && entry.dstDescId == target->id()) entry.settings = packedSettings;
            }
        }
        texture->m_imageDescription = safe->m_imageDescription;
        texture->m_cachedStates.fill(std::nullopt);
        if (!proxy.slot) for (unsigned i = 0; i < g_state->nv.slots.size(); ++i)
            if (g_state->nv.slots[i].texture == safe) { proxy.slot = i; break; }
        // bind() precedes renderToFBInternal(), which reads this description.
        // Parameters set through the proxy apply to the currently bound result.
        safe->bind();
        ++state.fallbackBinds;
    } else {
        original(texture); // Safe Intel-owned pixel, never the VRAM source.
        ++state.blankBinds;
    }
}

static void vramUnbind(Render::GL::CGLTexture* texture) {
    using F = void (*)(Render::GL::CGLTexture*);
    reinterpret_cast<F>(g_vramUnbindHook->m_original)(texture);
    if (!g_state) return;
    auto found = g_state->vram.proxies.find(texture);
    if (found == g_state->vram.proxies.end() || !found->second.bound) return;
    auto& proxy = found->second;
    if (proxy.slot && g_state->nv.client.ready && g_state->nv.enabled && !g_state->nv.stopRequested) {
        auto fence = g_pHyprRenderer->createSyncFDManager();
        if (fence && fence->isValid()) g_state->nv.slots[*proxy.slot].readComplete.reset(fence->takeFd().take());
        else { glFinish(); requestOffloadStop(); }
    }
    if (proxy.savedCMCache) { g_pHyprRenderer->m_cmSettingsCache = std::move(*proxy.savedCMCache); proxy.savedCMCache.reset(); }
    texture->m_imageDescription = proxy.savedDescription;
    proxy.savedDescription.reset(); proxy.bound = false; proxy.slot.reset();
}

static bool sendVramFeedback(CLinuxDMABUFFeedbackResource* feedback) {
    if (!g_state || !g_state->vram.feedback || !feedback || !feedback->m_surface ||
        feedback->m_surface != g_state->vram.surface.lock() || !PROTO::linuxDma || !PROTO::linuxDma->m_formatTable)
        return false;
    auto device = g_state->vram.device;
    wl_array array{.size = sizeof(device), .alloc = 0, .data = &device};
    feedback->m_resource->sendMainDevice(&array);
    auto tranche = PROTO::linuxDma->m_formatTable->m_rendererTranche;
    tranche.device = device;
    feedback->sendTranche(tranche);
    feedback->m_resource->sendDone();
    feedback->m_lastFeedbackWasScanout = false;
    ++g_state->vram.feedbackSent;
    return true;
}

static void vramDefaultFeedback(CLinuxDMABUFFeedbackResource* feedback) {
    if (sendVramFeedback(feedback)) return;
    using F = void (*)(CLinuxDMABUFFeedbackResource*);
    reinterpret_cast<F>(g_vramFeedbackHook->m_original)(feedback);
}

static void removeVramHook(CFunctionHook*& hook) {
    if (!hook) return;
    hook->unhook(); HyprlandAPI::removeFunctionHook(g_handle, hook); hook = nullptr;
}

static bool installVramHook(CFunctionHook*& hook, const std::string& name, const std::string& signature, void* replacement, bool prefix = false) {
    if (hook) return true;
    std::set<void*> addresses;
    for (const auto& match : HyprlandAPI::findFunctionsByName(g_handle, name))
        if (prefix ? match.demangled.starts_with(signature) : match.demangled == signature) addresses.insert(match.address);
    // The API caches parallel nm/demangler output. Resolve these exact ABI
    // symbols directly when that cached listing lacks a texture method.
    if (addresses.empty()) {
        const char* symbol = signature == "Render::GL::CGLTexture::bind()" ? "_ZN6Render2GL10CGLTexture4bindEv" :
                             signature == "Render::GL::CGLTexture::unbind()" ? "_ZN6Render2GL10CGLTexture6unbindEv" : nullptr;
        if (symbol) if (auto* address = dlsym(RTLD_DEFAULT, symbol)) addresses.insert(address);
    }
    if (addresses.size() != 1) { g_vramError = "hook " + name + " has " + std::to_string(addresses.size()) + " addresses"; Log::logger->log(Log::ERR, "[hdr-vram] hook {}: {} addresses", name, addresses.size()); return false; }
    hook = HyprlandAPI::createFunctionHook(g_handle, *addresses.begin(), replacement);
    if (!hook || !hook->hook()) { g_vramError = "cannot hook " + name; Log::logger->log(Log::ERR, "[hdr-vram] hook failed: {}", name); removeVramHook(hook); return false; }
    return true;
}

static bool setVramInput(bool enable, bool native = false) {
    if (!g_state) return false;
    auto& state = g_state->vram;
    if (!enable) {
        state.feedback = false;
        if (PROTO::linuxDma) for (const auto& feedback : PROTO::linuxDma->m_feedbacks)
            if (feedback->m_surface && feedback->m_surface == state.surface.lock()) feedback->sendDefaultFeedback();
        // Keep protecting already-created source textures until they retire.
        return true;
    }
    if (!g_state->enabled || !g_state->nv.enabled || !g_state->nv.client.ready || !g_state->nv.flag ||
        !Offload::offloadRequested(g_state->nv.ownerPID) || g_state->nv.sourceSurface.expired() || !PROTO::linuxDma)
        { g_vramError = std::format("prerequisites enabled={} nv={} ready={} flag={} owner={} env={} surface={} protocol={}", g_state->enabled, g_state->nv.enabled, g_state->nv.client.ready, g_state->nv.flag, g_state->nv.ownerPID, Offload::offloadRequested(g_state->nv.ownerPID), !g_state->nv.sourceSurface.expired(), bool(PROTO::linuxDma)); return false; }
    if (state.feedback) { if (state.nativeLayout != native) { g_vramError = "Disable VRAM input before changing its layout"; return false; } return true; }
    dev_t device = 0;
    for (const auto& node : std::filesystem::directory_iterator("/sys/class/drm")) {
        const auto name = node.path().filename().string();
        if (!name.starts_with("renderD")) continue;
        std::ifstream vendor(node.path() / "device/vendor"); std::string id; vendor >> id;
        if (id != "0x10de") continue;
        struct stat info{};
        if (!stat((std::filesystem::path("/dev/dri") / name).c_str(), &info)) { device = info.st_rdev; break; }
    }
    if (!device) { g_vramError = "no NVIDIA render node"; return false; }
    if (!installVramHook(g_vramCtorHook, "CDMABuffer", "CDMABuffer::CDMABuffer(unsigned int, wl_client*, Aquamarine::SDMABUFAttrs const&)", reinterpret_cast<void*>(&vramBufferConstructor)) ||
        !installVramHook(g_vramCreateHook, "createTexture", "Render::GL::CHyprGLRenderer::createTexture(Aquamarine::SDMABUFAttrs const&, bool)", reinterpret_cast<void*>(&vramCreateTexture)) ||
        !installVramHook(g_vramBindHook, "bind", "Render::GL::CGLTexture::bind()", reinterpret_cast<void*>(&vramBind)) ||
        !installVramHook(g_vramUnbindHook, "unbind", "Render::GL::CGLTexture::unbind()", reinterpret_cast<void*>(&vramUnbind)) ||
        !installVramHook(g_vramFeedbackHook, "sendDefaultFeedback", "CLinuxDMABUFFeedbackResource::sendDefaultFeedback()", reinterpret_cast<void*>(&vramDefaultFeedback)))
        return false;
    state.pid = g_state->nv.ownerPID; state.owner = g_state->nv.owner; state.surface = g_state->nv.sourceSurface;
    state.device = device; state.protectNew = true; state.nativeLayout = native;
    if (g_state->nv.cachedSlot) state.safeTexture = g_state->nv.slots[*g_state->nv.cachedSlot].texture;
    state.feedback = true;
    const auto before = state.feedbackSent;
    for (const auto& feedback : PROTO::linuxDma->m_feedbacks) sendVramFeedback(feedback.get());
    if (before == state.feedbackSent) { g_vramError = "no matching surface feedback among " + std::to_string(PROTO::linuxDma->m_feedbacks.size()); setVramInput(false); return false; }
    return true;
}

// Runs on the existing idle timer, after ordinary offload has produced a
// materialized frame. This never creates an NVIDIA context on the desktop.
static void updateAutomaticVramInput() {
    if (!g_state || !g_state->vram.automatic || !g_state->enabled || !g_state->nv.enabled) return;
    auto& state = g_state->vram;
    const auto& nv = g_state->nv;
    const auto now = std::chrono::steady_clock::now();
    if (nv.stopRequested || !nv.client.ready || !nv.cachedSlot || !nv.slots[*nv.cachedSlot].texture ||
        now - nv.lastDraw > std::chrono::milliseconds(250) || now < state.retryAfter) return;
    if (state.feedback && state.surface.lock() == nv.sourceSurface.lock() && state.pid == nv.ownerPID) return;
    if (state.feedback) setVramInput(false);
    if (!setVramInput(true, true)) state.retryAfter = now + std::chrono::seconds(1);
}

static void stopVramInput() {
    if (!g_state) return;
    setVramInput(false);
    removeVramHook(g_vramFeedbackHook); removeVramHook(g_vramUnbindHook); removeVramHook(g_vramBindHook);
    removeVramHook(g_vramCreateHook); removeVramHook(g_vramCtorHook);
    g_state->vram.proxies.clear(); g_state->vram.safeTexture.reset();
}
