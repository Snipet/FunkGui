#pragma once

// Capture and diagnostics overrides (02 §5.1 "Diagnostics environment"), read once from the environment through
// funkgui::env() — FUNKGUI_ENV_PREFIX + the suffix below — at EditorHost construction, and never per frame (02 §3.7
// rule 4). It replaces the snapshot's process-global SdfCanvas::dumpNextFrameTo (02 §2.3): each EditorHost has its own.
//
//   CANVAS_DUMP=path, CANVAS_DUMP_AFTER=n   dump v2 of frame n; pins FramePump to its fallback clock
//   UI_THEME=0|1                            theme override, not persisted
//   UI_SCALE=s, UI_SCALE_AFTER=n            fake backing scale
//   UI_KEYS=spec                            key replay before the first frame (HR's grammar + home,end,pageup,pagedown)
//   UI_FIXED_DT=sec                         the Panel ticks with this dt, so a live capture equals a headless frame
//   A11Y_DUMP=path                          a11y dump after the first frame (a11yDumpLine format, 02 §5.6)
//   GPU_LOG=1                               log attach, retry, owns, scale and overflow events
//
// Declared in G2 (v0.2.0, frozen at FZ1); fromEnv() is implemented with EditorHost (G7).

#include <string>

namespace funkgui
{
    struct CaptureConfig
    {
        std::string canvasDump;                  // empty = no dump
        int         canvasDumpAfter = 0;         // frames to skip before the dumped one
        int         uiTheme = -1;                // -1 = follow UiPreferences
        float       uiScale = 0.0f;              // 0 = the real backing scale
        int         uiScaleAfter = 0;
        std::string uiKeys;                      // empty = no replay
        float       fixedDt = 0.0f;              // 0 = the frame clock's dt
        std::string a11yDump;                    // empty = no dump
        bool        gpuLog = false;

        // Every field from the environment; unset or unparsable variables keep the defaults above.
        static CaptureConfig fromEnv();
    };
}
