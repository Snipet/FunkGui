#include <funkgui/gpu/GpuPicture.h>

// GPU pictures without JUCE (v0.28.0): none. renderGpuPicture() draws on JUCE's message thread and posts to it from
// every other one (src/gpu/GpuPicture.cpp), and a build without JUCE has no such thread, so this build answers as a
// platform without GPU pictures does: unsupported, nothing drawn, bgfx not touched, and the product draws the frame
// with SoftRaster (gpu/GpuPicture.h, "Where there is no GPU picture"). A window's own frame is read back from the GPU
// by its host instead (desktop/DesktopHost.h capture()). Compiled only with FUNKGUI_WITH_JUCE=OFF (src/gpu/nojuce/**).

namespace funkgui
{
    GpuPicture renderGpuPicture(const PrimList&, const GpuPictureOptions&)
    {
        return {};                                   // Status::unsupported, an empty image
    }

    bool gpuPicturesSupported() noexcept
    {
        return false;
    }

    bool releaseGpuPictureContext(int)
    {
        return true;                                 // no hold remains: there never is one
    }

    bool gpuPictureContextHeld() noexcept
    {
        return false;
    }

    GpuPictureCounters gpuPictureCounters() noexcept
    {
        return {};
    }

    const char* toString(GpuPicture::Status status) noexcept
    {
        switch (status)
        {
            case GpuPicture::Status::ok:              return "ok";
            case GpuPicture::Status::unsupported:     return "unsupported";
            case GpuPicture::Status::noMessageThread: return "noMessageThread";
            case GpuPicture::Status::timedOut:        return "timedOut";
            case GpuPicture::Status::initFailed:      return "initFailed";
            case GpuPicture::Status::badSize:         return "badSize";
            case GpuPicture::Status::overflow:        return "overflow";
            case GpuPicture::Status::failed:          return "failed";
        }
        return "?";
    }
}
