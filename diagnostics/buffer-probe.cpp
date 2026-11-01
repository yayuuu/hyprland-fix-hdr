#include <bits/stdc++.h>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/ColorManagement.hpp>
#include <hyprland/src/debug/HyprCtl.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/output/Monitor.hpp>
static HANDLE handle;static SP<SHyprCtlCommand> command;
APICALL EXPORT std::string PLUGIN_API_VERSION(){return HYPRLAND_API_VERSION;}
APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE h){
    if(std::string(__hyprland_api_get_hash())!=std::string(__hyprland_api_get_client_hash()))throw std::runtime_error("ABI mismatch");handle=h;
    command=HyprlandAPI::registerHyprCtlCommand(h,{.name="hdr-buffer-probe",.exact=true,.fn=[](eHyprCtlOutputFormat,std::string){
        std::ostringstream out;out<<"explicit_sync="<<g_pHyprRenderer->explicitSyncSupported()<<" expected_linear="<<int(NColorManagement::CM_TRANSFER_FUNCTION_EXT_LINEAR)<<'\n';
        for(const auto& m:State::monitorState()->monitors())out<<"monitor="<<m->m_name<<" TF="<<int(m->m_imageDescription->value().transferFunction)<<" HDR_PQ="<<(m->m_imageDescription->value().transferFunction==NColorManagement::CM_TRANSFER_FUNCTION_ST2084_PQ)<<'\n';
        auto query=reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
        if(query)for(uint32_t format:{uint32_t(DRM_FORMAT_ABGR16161616F),uint32_t(DRM_FORMAT_XBGR16161616F),uint32_t(DRM_FORMAT_ABGR2101010)}){
            EGLint count=0;if(!query(Render::GL::g_pHyprOpenGL->m_eglDisplay,format,0,nullptr,nullptr,&count))continue;
            std::vector<EGLuint64KHR> mods(count);std::vector<EGLBoolean> external(count);if(!query(Render::GL::g_pHyprOpenGL->m_eglDisplay,format,count,mods.data(),external.data(),&count))continue;
            for(int j=0;j<count;++j)out<<"format="<<std::hex<<format<<" modifier="<<mods[j]<<std::dec<<" external="<<external[j]<<'\n';
        }
        PROTO::compositor->forEachSurface([&](SP<CWLSurfaceResource> s){
            if(!s->m_current.buffer)return;auto attrs=s->m_current.buffer->dmabuf();if(!attrs.success)return;pid_t pid=0;wl_client_get_credentials(s->client(),&pid,nullptr,nullptr);out<<"pid="<<pid<<" buffer="<<attrs.size.x<<'x'<<attrs.size.y<<" fourcc="<<std::hex<<attrs.format<<" modifier="<<attrs.modifier<<std::dec<<" planes="<<attrs.planes;
            if(s->m_current.texture)out<<" opaque="<<s->m_current.texture->m_opaque<<" textureTF="<<(s->m_current.texture->m_imageDescription?int(s->m_current.texture->m_imageDescription->value().transferFunction):-1);
            out<<" surfaceTF="<<((s->m_colorManagement.valid() && s->m_colorManagement->hasImageDescription())?int(s->m_colorManagement->imageDescription().transferFunction):-1)<<'\n';
        });return out.str();}});
    return {"hdr-buffer-probe","Read-only buffer metadata diagnostic","daniel","1"};
}
APICALL EXPORT void PLUGIN_EXIT(){if(command)HyprlandAPI::unregisterHyprCtlCommand(handle,command);}
