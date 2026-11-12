#pragma once

// F1: what FunkGui's GPU layer runs on, for a product's debug page. Read-only: gpuInfo() changes nothing, starts
// nothing and holds nothing. Message thread (bgfx's thread in FunkGui's single-threaded bgfx), like every other call
// that reads BgfxContext. No bgfx type appears here, so a debug page needs no bgfx include.

#include <cstdint>
#include <string>

namespace funkgui
{
    struct GpuInfo
    {
        // Whether bgfx runs now (an editor is open, or a GPU picture holds a headless context: gpu/GpuPicture.h).
        // While it does not, the fields that come from bgfx are empty or 0.
        bool        running = false;
        std::string renderer;                    // bgfx's name for it ("Metal", "Vulkan", "Direct3D 11"); empty
        uint16_t    vendorId = 0;                // the GPU's PCI vendor id (0x106b Apple, 0x10de NVIDIA, ...); 0
        uint16_t    deviceId = 0;                // and device id; 0
        uint32_t    maxTextureSize = 0;          // the largest texture side in px; 0
        uint32_t    transientVbBytes = 0;        // the transient vertex buffer bgfx was started with; 0
        // Whether the UI's primitives are drawn as instances now: bgfx made the instanced program and
        // BgfxContext::Config::instancing is on (gpu/BgfxContext.h, v0.26.0). False while bgfx is not running.
        bool        instancing = false;
        // Held whether or not bgfx runs: the picture textures alive on the GPU (canvas/Images.h; 0 while bgfx is not
        // running) and the custom-shader programs registered (gpu/ShaderLibrary.h set(), not yet removed).
        uint32_t    imageTextures = 0;
        uint32_t    customShaders = 0;
        bool        gpuPictures = false;         // gpuPicturesSupported(): this platform has GPU pictures
    };

    GpuInfo gpuInfo();
}
