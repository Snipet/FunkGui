// FUNKGUI_TEST name=fg.smoke.core timeout=900 gpu=0
//
// fg.smoke.core: a console consumer of FunkGui::core. Every Core source (src/{core,text,prefs,…}) compiles inside this
// target with the product seams funkgui_configure_product() gives it (PRODUCT FunkGui, OBJC_PREFIX FunkGui,
// ENV_PREFIX FUNKGUI_, PREFS_FOLDER FunkGui), and the checks below exercise what S0 makes real: the seams of
// core/Config.h, funkgui::env() (prefix, cache, envReload, stable pointers), the bundled face through FunkGuiFonts and
// the snapshot's FontAtlasSdf, and the theme tokens. Spec rows only: nothing here is drift-detected.

#include <funkgui/core/Config.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/BundledFont.h>
#include <funkgui/text/FontAtlasSdf.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstring>
#include <string_view>

namespace T = funkgui::test;

namespace
{
    bool same(const char* a, std::string_view b) { return a != nullptr && std::string_view(a) == b; }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // FontAtlasSdf rasterises through JUCE's font stack
    T::Probe P("fg.smoke.core", "", argc, argv);

    // ---- seams (02 §1.8) --------------------------------------------------------------------------------------------
    P.eq("seam.product", same(funkgui::config::kProductName, "FunkGui"), 1);
    P.eq("seam.objc_prefix", same(funkgui::config::kObjcPrefix, "FunkGui"), 1);
    P.eq("seam.env_prefix", same(funkgui::config::kEnvPrefix, "FUNKGUI_"), 1);
    P.eq("seam.prefs_folder", same(funkgui::config::kPrefsFolder, "FunkGui"), 1);
    P.eq("seam.objc_prefix_str", same(FUNKGUI_OBJC_PREFIX_STR, "FunkGui"), 1);

    // ---- funkgui::env(): FUNKGUI_ENV_PREFIX + name, cached on first use, re-read after envReload() ------------------
    T::unsetEnv("FUNKGUI_SMOKE_VALUE");
    funkgui::envReload();
    P.eq("env.unset_is_null", funkgui::env("SMOKE_VALUE") == nullptr, 1);
    T::setEnv("FUNKGUI_SMOKE_VALUE", "one");
    P.eq("env.cached_until_reload", funkgui::env("SMOKE_VALUE") == nullptr, 1);
    funkgui::envReload();
    const char* first = funkgui::env("SMOKE_VALUE");
    P.eq("env.reload_reads_again", same(first, "one"), 1);
    T::setEnv("FUNKGUI_SMOKE_VALUE", "two");
    funkgui::envReload();
    const char* second = funkgui::env("SMOKE_VALUE");
    P.eq("env.old_pointer_stays_valid", same(first, "one") && same(second, "two"), 1);
    P.eq("env.same_pointer_while_cached", funkgui::env("SMOKE_VALUE") == second, 1);
    P.eq("env.prefix_added_once", funkgui::env("FUNKGUI_SMOKE_VALUE") == nullptr, 1);
    T::unsetEnv("FUNKGUI_SMOKE_VALUE");

    // ---- the bundled face bakes through FunkGuiFonts (BundledFont.h is the one place that names it) -----------------
    funkgui::FontAtlasSdf atlas;
    const bool baked = atlas.bake(funkgui::BundledFont::data(), funkgui::BundledFont::size());
    P.eq("font.bundled_bakes", baked, 1);
    P.eq("font.embedded_face_used", atlas.usedEmbeddedFace(), 1);
    P.eq("font.atlas_texels", static_cast<int64_t>(atlas.pixels().size()),
         static_cast<int64_t>(funkgui::FontAtlasSdf::kAtlasW) * funkgui::FontAtlasSdf::kAtlasH);
    P.in("font.cap_height", atlas.capHeight(), 1.0, funkgui::FontAtlasSdf::kBasePx);
    P.eq("font.name", same(funkgui::BundledFont::name(), "JetBrains Mono Regular"), 1);

    // ---- theme tokens: values unchanged from the snapshot (02 §2.2) -------------------------------------------------
    constexpr funkgui::Theme graphite = funkgui::Theme::graphite();
    P.eq("theme.count", funkgui::Theme::kCount, 2);
    P.eq("theme.graphite.ground",
         (int64_t{ graphite.ground.r } << 16) | (int64_t{ graphite.ground.g } << 8) | graphite.ground.b, 0x16171A);
    P.eq("theme.index_1_is_paper", same(funkgui::Theme::name(1), "PAPER"), 1);

    return P.finish();
}
