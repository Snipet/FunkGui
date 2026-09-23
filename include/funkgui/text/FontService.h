#pragma once

// The process-wide font (02 §3.4). Core, no GPU: the atlas is baked on the CPU from the bundled face
// (text/BundledFont.h) on the first atlas() call, and every Canvas, HeadlessHost and BgfxContext reads that one bake,
// so a headless frame and a live frame use the same glyph metrics and UVs. BgfxContext::createResources() uploads
// atlas().pixels() into its R8 texture (HR baked inside BgfxContext).
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G3 (src/text/FontService.cpp).

#include <funkgui/text/FontAtlasSdf.h>

#include <cstdint>

namespace funkgui
{
    class FontService
    {
    public:
        // The instance, created on first use and never destroyed (HR idiom: a plug-in binary is unloaded while
        // statics are torn down in an order nothing here controls). Message thread.
        static FontService& get();

        // The atlas; bakes from BundledFont on the first call (11–23 ms, A §0). Valid for the life of the process.
        // atlas().baked() is false when the embedded face could not be baked; text then draws nothing.
        const FontAtlasSdf& atlas();

        // The atlas was baked from the embedded face (== atlas().baked() && atlas().usedEmbeddedFace()).
        bool ok() const;

        // FNV-1a 64 of the R8 pixels, the same value FontProbe reports as font.atlas. 0 before the first bake.
        uint64_t atlasHash() const;

        FontService(const FontService&) = delete;
        FontService& operator=(const FontService&) = delete;

    private:
        FontService() = default;

        // Private state: completed by the implementing card (G3); not part of the frozen API.
        FontAtlasSdf atlas_;
        bool     attempted_ = false;
        uint64_t hash_ = 0;
    };
}
