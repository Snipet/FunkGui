#include <funkgui/canvas/SoftRaster.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

// funkgui::writePng (v0.12.0: moved here from src/canvas/SoftRaster.cpp, unchanged): JUCE's PNG encoder over the
// rasteriser's RGBA image. Compiled only with FUNKGUI_WITH_JUCE (src/juce/**); src/nojuce/WritePng.cpp is its
// counterpart without JUCE.

namespace funkgui
{
    bool writePng(const Image& img, const char* path)
    {
        if (path == nullptr || *path == 0 || img.w <= 0 || img.h <= 0
            || img.rgba.size() != static_cast<size_t>(img.w) * static_cast<size_t>(img.h) * 4u)
            return false;

        juce::Image out(juce::Image::ARGB, img.w, img.h, false);
        {
            const juce::Image::BitmapData bd(out, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < img.h; ++y)
                for (int x = 0; x < img.w; ++x)
                {
                    const uint8_t* s = &img.rgba[(static_cast<size_t>(y) * static_cast<size_t>(img.w)
                                                  + static_cast<size_t>(x)) * 4u];
                    bd.setPixelColour(x, y, juce::Colour(s[0], s[1], s[2], s[3]));   // premultiplies
                }
        }

        // Written beside the destination and renamed onto it once complete, so a reader never sees half a file.
        const juce::File dest = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(path));
        const juce::File tmp = dest.getSiblingFile(dest.getFileName() + ".partial");
        tmp.deleteFile();
        bool ok = false;
        {
            juce::FileOutputStream os(tmp);
            juce::PNGImageFormat png;
            ok = os.openedOk() && png.writeImageToStream(out, os);
            if (ok)
            {
                os.flush();
                ok = os.getStatus().wasOk();
            }
        }
        if (ok && std::rename(tmp.getFullPathName().toRawUTF8(), dest.getFullPathName().toRawUTF8()) == 0)
            return true;
        tmp.deleteFile();
        return false;
    }
}
