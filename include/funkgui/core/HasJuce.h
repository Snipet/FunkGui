#pragma once

// FUNKGUI_HAS_JUCE (v0.12.0): whether this build of FunkGui has JUCE under it. It is FunkGui::core's interface
// definition, set from the CMake option FUNKGUI_WITH_JUCE: 1 by default, 0 for a JUCE-free core (the browser, or the
// native `nojuce` preset that keeps that core honest). The few places that differ read it with #if:
//
//   prefs/UiPreferences.h    file() and defaultFile(), and the JUCE include they need, exist only with JUCE
//   text/FontService         bakes the bundled face with JUCE, loads the committed bake without
//
// Everything else that needs JUCE is a source under src/juce/, which a JUCE-free build does not compile (src/nojuce/
// holds the counterparts). A translation unit that reaches a FunkGui header without the definition is treated as a
// JUCE build, as every build was before the option existed.

#if !defined(FUNKGUI_HAS_JUCE)
  #define FUNKGUI_HAS_JUCE 1
#endif
