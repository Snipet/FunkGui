// FUNKGUI_TEST name=fg.gpu.info timeout=300 gpu=1
//
// fg.gpu.info (F1; gpu/GpuInfo.h, core/Version.h): the debug information. Spec rows only.
//
// - version.*  funkgui::version() is FUNKGUI_VERSION, "MAJOR.MINOR.PATCH".
// - idle.*     with no GPU context (no editor, no picture): not running, no renderer, ids, sizes and instancing 0;
//              gpuPictures is gpuPicturesSupported(); asking starts nothing.
// - shaders.*  customShaders counts the programs registered: one more after shaders::set() takes this platform's test
//              binary (Metal's on macOS, SPIR-V on Linux; refused without a Metal device, then unchanged), one fewer
//              after remove().
// - live.*     where GPU pictures exist (macOS): while renderGpuPicture() holds a headless context, running, a renderer
//              name, a largest texture of at least 2048 px, the transient buffer BgfxContext reports (> 0), instancing
//              as BgfxContext has it, and the picture's texture among imageTextures; after the context is released,
//              not running again. Where there are none (Linux), or no GPU starts (a VM), these rows are skipped and
//              noted: a soft test there.

#include <juce_events/juce_events.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Images.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Version.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/GpuInfo.h>
#include <funkgui/gpu/GpuPicture.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>
#if defined(__APPLE__)
  #include <funkgui/shaders/fs_custom_test.mtl.h>
#elif defined(__linux__)
  #include <funkgui/shaders/fs_custom_test.spv.h>
#endif

#include <cstdint>
#include <cstdio>
#include <string>

namespace T = funkgui::test;
using funkgui::GpuInfo;

namespace
{
    constexpr uint32_t kShaderId = 0x0f1d0001u;
    constexpr uint32_t kImageId = 0x0f1d0002u;

    bool versionShaped(const std::string& v)
    {
        int dots = 0;
        bool digit = false;
        for (const char c : v)
        {
            if (c == '.')
            {
                if (!digit)
                    return false;
                ++dots;
                digit = false;
            }
            else if (c >= '0' && c <= '9')
                digit = true;
            else
                return false;
        }
        return dots == 2 && digit;
    }

    funkgui::PrimList frameWithImage()
    {
        funkgui::FrameInfo f;
        f.logicalW = 32;
        f.logicalH = 24;
        f.dpi = 1.0f;
        f.fixedClock = true;
        funkgui::Canvas canvas(funkgui::FontService::get().atlas());
        canvas.begin(f);
        canvas.rrect(2.0f, 2.0f, 28.0f, 20.0f, 3.0f, funkgui::Col{ 40, 80, 120, 255 });
        canvas.image(kImageId, funkgui::Rect{ 4.0f, 4.0f, 8.0f, 8.0f });
        return canvas.end();
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.gpu.info", "", argc, argv);
    {
        const juce::ScopedJuceInitialiser_GUI juceInit;   // the message thread (this one) and the font's bake

        // ---- version ----------------------------------------------------------------------------------------------
        const std::string v = funkgui::version();
        P.eq("version.is_project", v == FUNKGUI_VERSION_STRING, 1);
        P.eq("version.shape", versionShaped(v), 1);
        P.note("version", "\"" + v + "\"");

        // ---- idle -------------------------------------------------------------------------------------------------
        auto& ctx = funkgui::BgfxContext::get();
        const GpuInfo idle = funkgui::gpuInfo();
        P.eq("idle.not_running", idle.running, 0);
        P.eq("idle.empty", idle.renderer.empty() && idle.vendorId == 0 && idle.deviceId == 0
                               && idle.maxTextureSize == 0 && idle.transientVbBytes == 0 && !idle.instancing, 1);
        P.eq("idle.no_image_textures", idle.imageTextures, 0);
        P.eq("idle.pictures_supported", idle.gpuPictures == funkgui::gpuPicturesSupported(), 1);
        P.eq("idle.starts_nothing", ctx.valid(), 0);

        // ---- shaders ----------------------------------------------------------------------------------------------
        const uint32_t before = idle.customShaders;
       #if defined(__APPLE__) || defined(__linux__)
        {
           #if defined(__APPLE__)
            const void* bin = fs_custom_test_mtl;
            const auto size = static_cast<uint32_t>(sizeof fs_custom_test_mtl);
           #else
            const void* bin = fs_custom_test_spv;
            const auto size = static_cast<uint32_t>(sizeof fs_custom_test_spv);
           #endif
            std::string why;
            const bool taken = funkgui::shaders::set(kShaderId, bin, size, why);
            if (!taken)
                std::printf("INFO     shaders::set refused the test binary (%s): the count must stay\n", why.c_str());
            P.eq("shaders.counted", funkgui::gpuInfo().customShaders, before + (taken ? 1 : 0));
            funkgui::shaders::remove(kShaderId);
            P.eq("shaders.removed", funkgui::gpuInfo().customShaders, before);
        }
       #else
        P.note("shaders", "\"skipped: no test binary for this platform\"");
       #endif

        // ---- live -------------------------------------------------------------------------------------------------
        if (!funkgui::gpuPicturesSupported())
        {
            std::printf("INFO     no GPU pictures on this platform: no context to read, the live rows skipped\n");
            P.note("live", "\"skipped: no GPU pictures\"");
        }
        else
        {
            const uint8_t rgba[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
            funkgui::images::set(kImageId, 2, 2, rgba);
            const funkgui::PrimList list = frameWithImage();
            const funkgui::GpuPicture pic = funkgui::renderGpuPicture(list);
            if (!pic.ok() || !ctx.valid())
            {
                std::printf("INFO     no GPU context (%s): the live rows skipped\n", funkgui::toString(pic.status));
                P.note("live", "\"skipped: no GPU context\"");
            }
            else
            {
                const GpuInfo live = funkgui::gpuInfo();
                P.eq("live.running", live.running, 1);
                P.eq("live.renderer", !live.renderer.empty(), 1);
                P.eq("live.max_texture", live.maxTextureSize >= 2048, 1);
                P.eq("live.transient", live.transientVbBytes > 0 && live.transientVbBytes == ctx.transientVbBytes(), 1);
                P.eq("live.instancing", live.instancing == (ctx.instancing() && ctx.config().instancing), 1);
                P.eq("live.image_texture", live.imageTextures >= 1, 1);
                P.eq("live.pictures", live.gpuPictures, 1);
                P.note("renderer", "\"" + live.renderer + "\"");
                std::printf("INFO     %s, vendor 0x%04x device 0x%04x, max texture %u, transient %u B, instancing %d, "
                            "%u image textures, %u custom shaders\n",
                            live.renderer.c_str(), live.vendorId, live.deviceId, live.maxTextureSize,
                            live.transientVbBytes, live.instancing ? 1 : 0, live.imageTextures, live.customShaders);
            }
            funkgui::releaseGpuPictureContext();
            P.eq("live.released", funkgui::gpuInfo().running, 0);
            funkgui::images::remove(kImageId);
        }
    }
    return P.finish();
}
