#include <funkgui/gpu/GpuInfo.h>

#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/BgfxSink.h>
#include <funkgui/gpu/GpuPicture.h>

#include "ShaderLibraryDetail.h"

#include <bgfx/bgfx.h>

// F1 (gpu/GpuInfo.h). bgfx's renderer and caps are read only while BgfxContext says bgfx is initialised: before that,
// and after shutdown, bgfx::getCaps() and getRendererType() describe nothing.

namespace funkgui
{
    GpuInfo gpuInfo()
    {
        GpuInfo info;
        const BgfxContext& ctx = BgfxContext::get();
        info.running = ctx.valid();
        if (info.running)
        {
            info.renderer = bgfx::getRendererName(bgfx::getRendererType());
            if (const bgfx::Caps* caps = bgfx::getCaps())
            {
                info.vendorId = caps->vendorId;
                info.deviceId = caps->deviceId;
                info.maxTextureSize = caps->limits.maxTextureSize;
            }
            info.transientVbBytes = ctx.transientVbBytes();
            info.instancing = ctx.instancing() && ctx.config().instancing;
        }
        info.imageTextures = BgfxSink::imageTextureCount();
        info.customShaders = shaders::detail::programCount();
        info.gpuPictures = gpuPicturesSupported();
        return info;
    }
}
