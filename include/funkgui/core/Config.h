#pragma once

// Product seams (FCompressor docs/design/02-funkgui-and-ui.md §1.8).
//
// FunkGui's sources compile inside each consumer target (INTERFACE sources, 02 §1.3), so product identity is a set of
// PRIVATE definitions that funkgui_configure_product(<target> PRODUCT … OBJC_PREFIX … ENV_PREFIX … PREFS_FOLDER …)
// puts on that target (cmake/FunkGuiTargets.cmake):
//
//   FUNKGUI_PRODUCT_NAME   string literal   "FCompressor"   fallback screen, logs
//   FUNKGUI_OBJC_PREFIX    identifier       Fcmp            root of the product's Objective-C class names
//   FUNKGUI_ENV_PREFIX     string literal   "FCMP_"         prepended by funkgui::env() (core/Env.h)
//   FUNKGUI_PREFS_FOLDER   string literal   "FCompressor"   ~/Library/Application Support/<folder>/preferences.settings
//
// A target that links FunkGui but never called funkgui_configure_product() fails to compile here, rather than
// colliding with another product at run time.

#if !defined(FUNKGUI_PRODUCT_NAME) || !defined(FUNKGUI_OBJC_PREFIX) || !defined(FUNKGUI_ENV_PREFIX) \
    || !defined(FUNKGUI_PREFS_FOLDER)
  #error "call funkgui_configure_product()"
#endif

#define FUNKGUI_DETAIL_CAT_(a, b) a##b
#define FUNKGUI_DETAIL_CAT(a, b)  FUNKGUI_DETAIL_CAT_(a, b)
#define FUNKGUI_DETAIL_STR_(x)    #x
#define FUNKGUI_DETAIL_STR(x)     FUNKGUI_DETAIL_STR_(x)

// The static Objective-C class <OBJC_PREFIX><Root>, e.g. FUNKGUI_OBJC_NAME(RenderView) -> FcmpRenderView. The snapshot's
// NativeSurface.mm and DisplayLink.mm declare their classes through it (G0's rename). Static names separate products,
// not binaries (K2 #16): G7 replaces them with juce::ObjCClass runtime names whose roots are
// FUNKGUI_OBJC_PREFIX_STR "RenderView_" and FUNKGUI_OBJC_PREFIX_STR "DisplayLinkTarget_" (02 §1.8).
#define FUNKGUI_OBJC_NAME(root) FUNKGUI_DETAIL_CAT(FUNKGUI_OBJC_PREFIX, root)

// FUNKGUI_OBJC_PREFIX as a string literal ("Fcmp"), for runtime class-name roots and diagnostics.
#define FUNKGUI_OBJC_PREFIX_STR FUNKGUI_DETAIL_STR(FUNKGUI_OBJC_PREFIX)

namespace funkgui::config
{
    // Each `X ""` fails to compile unless X is a string literal, so a mistyped definition is caught here.
    inline constexpr const char* kProductName = FUNKGUI_PRODUCT_NAME "";
    inline constexpr const char* kObjcPrefix  = FUNKGUI_OBJC_PREFIX_STR "";
    inline constexpr const char* kEnvPrefix   = FUNKGUI_ENV_PREFIX "";
    inline constexpr const char* kPrefsFolder = FUNKGUI_PREFS_FOLDER "";

    static_assert(sizeof(FUNKGUI_PRODUCT_NAME "") > 1, "FUNKGUI_PRODUCT_NAME must be a non-empty string literal");
    static_assert(sizeof(FUNKGUI_OBJC_PREFIX_STR "") > 1, "FUNKGUI_OBJC_PREFIX must be a non-empty identifier");
    static_assert(sizeof(FUNKGUI_ENV_PREFIX "") > 1, "FUNKGUI_ENV_PREFIX must be a non-empty string literal");
    static_assert(sizeof(FUNKGUI_PREFS_FOLDER "") > 1, "FUNKGUI_PREFS_FOLDER must be a non-empty string literal");
}
