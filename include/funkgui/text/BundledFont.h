#pragma once

#include <FunkGuiFonts.h>
#include <cstddef>

namespace funkgui
{
    // The single place that names the shipping typeface.
    //
    // The plugin and the offscreen harnesses both bake the atlas, and a canvas
    // dump records atlas UVs rather than characters — so if the two ever bake
    // different faces the harness renders the right geometry with the wrong
    // glyphs and every specimen it produces is quietly wrong. They read the
    // face from here so they cannot diverge.
    struct BundledFont
    {
        static const void* data() { return funkguifonts::JetBrainsMonoRegularsubset_ttf; }
        static size_t      size() { return static_cast<size_t>(funkguifonts::JetBrainsMonoRegularsubset_ttfSize); }
        static const char* name() { return "JetBrains Mono Regular"; }
    };
}
