#pragma once

// What a Panel may ask of its host (02 §3.5). EditorHost implements it over JUCE and the frame pump; HeadlessHost over
// a simulated clock and a call log; a Panel reaches it through attach() and GestureController.

#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace juce
{
    class Component;                                 // ownerComponent() only: this header stays JUCE-free
}

namespace funkgui
{
    class ParamPort;

    // ---- Web Sprint B additions (v0.12.0; FCompressor ADR-93) -------------------------------------------------------
    // What the service calls at the end of HostServices take: plain structs, every text UTF-8.

    // The services a host can offer: HostServices::services() is a mask of these bits.
    namespace hostservice
    {
        inline constexpr unsigned menus       = 1u << 0;   // showMenu, dismissMenus
        inline constexpr unsigned fileChooser = 1u << 1;   // chooseFiles
        inline constexpr unsigned clipboard   = 1u << 2;   // copyText
    }

    // One row of a menu: an item, or with `separator` a dividing line (its other fields are not read). Every member
    // of these structs has an initialiser, so `{ 1, "Save" }` and `{ .separator = true }` are both complete.
    struct MenuItem
    {
        int         id = 0;                          // > 0: what the callback receives when the user chooses this item
        std::string label{};
        bool        enabled = true;                  // false: shown, and cannot be chosen
        bool        checked = false;                 // ticked
        bool        separator = false;
    };

    // A flat menu (no submenus, no icons, no shortcut column).
    struct MenuRequest
    {
        std::vector<MenuItem> items{};               // top to bottom
        Rect  anchor{};                              // the menu opens beside this rectangle, in the Panel's own px
        Theme theme = Theme::graphite();             // the palette the menu borrows. The caller passes it: a product's
                                                     // Theme is not always Theme::byIndex(themeIndex())
    };

    // The chosen item's id, or 0 when the user dismissed the menu without choosing.
    using MenuCallback = std::function<void(int id)>;

    struct FileRequest
    {
        enum class Mode { open, openMany, save };    // one existing file; one or several; a file to write

        Mode        mode = Mode::open;
        std::string title{};                         // the chooser's title: "Import presets"
        std::string pattern{};                       // the files it offers, as wildcards: "*.fcmppreset"; several
                                                     // separated by ';'; empty = every file
        std::string suggestedName{};                 // save: the file name it starts with, extension included. The
                                                     // host drops the characters a file name cannot hold
    };

    // The chosen files as absolute paths; empty when the user cancelled. Mode::open and Mode::save give one path.
    using FilesCallback = std::function<void(const std::vector<std::string>& paths)>;

    class HostServices
    {
    public:
        virtual ~HostServices() = default;

        virtual void   setUnboundedDrag(bool on) = 0;    // enableUnboundedMouseMovement(true, true) in EditorHost
        virtual void   showParamMenu(ParamPort&, float x, float y) = 0;   // host context menu for a parameter
        virtual void   nudgeFullRate() = 0;              // FramePump::nudgeFullRate
        virtual double nowSeconds() const = 0;           // wall clock (EditorHost) or simulated (HeadlessHost)

        // A batch of writes to several parameters (K2 #23; G2 addition). GestureController::tapMany brackets its
        // writes with these, so the product can hold the audio thread on the previous parameter set until the whole
        // set is written (FCompressor: ProcessorFacade::beginBatch/endBatch, where endBatch raises the engine snap).
        // Calls nest, and every beginBatch is matched by one endBatch.
        virtual void   beginBatch() = 0;
        virtual void   endBatch() = 0;

        // ---- G7b additions (v0.7.1). Not pure: a host written before them keeps compiling and gets the defaults. ----

        // The index of the Theme the Panel's next draw() receives (Theme::byIndex; 0 .. Theme::kCount - 1). Valid from
        // attach() on and at any time: in tick(), in draw(), in an input handler. EditorHost: its <PREFIX>UI_THEME
        // capture override when one is set, else UiPreferences::theme(), which the next frame applies before it ticks
        // the Panel; so a theme cell that writes the preference reads the new index at once, and the frame after the
        // click ticks and draws with it (no matching the Theme's colours, no frame of lag). HeadlessHost: its
        // constructor's themeIdx (0 when that names no theme, as Theme::byIndex draws). Default: 0.
        virtual int themeIndex() const { return 0; }

        // The juce::Component that owns the Panel's window, to anchor a juce::PopupMenu
        // (PopupMenu::Options::withTargetComponent) or parent a juce::FileChooser. EditorHost returns itself, valid for
        // as long as the Panel is attached. nullptr when there is no window (HeadlessHost; the default): the Panel
        // then shows no menu and no chooser. Its coordinates are editor px (G7c): under a UI zoom, the Panel's
        // position (x, y) is (x, y) * ownerComponent()->getWidth() / width() there. Anchoring to the component itself
        // needs no conversion.
        virtual juce::Component* ownerComponent() { return nullptr; }

        // ---- G7c additions (v0.8.0; FCompressor ADR-68 UI zoom). Not pure: defaults 100 / no-op / empty. ------------
        //
        // A UI zoom scales the whole window uniformly (its size, the render density, input and accessibility
        // coordinates) while the Panel keeps its fixed logical size and draws, hit-tests and lists accessibility in
        // its own px: nothing a Panel receives or records changes with the zoom. These calls exist for a ZOOM control.

        // The zoom the window is drawn at, in percent. EditorHost: its <PREFIX>UI_ZOOM capture pin when one is set
        // (100 under CANVAS_DUMP without one); else 100 when zoomSteps() is empty; else the machine-wide preference (a
        // missing or unlisted value reads as EditorConfig::defaultZoomPercent) reduced to the largest step whose
        // window fits the user area of the editor's display. Like themeIndex(), it answers a setZoomPercent() at
        // once, and the next frame resizes the editor before it ticks the Panel. HeadlessHost: HeadlessHost::setZoom.
        virtual int zoomPercent() const { return 100; }

        // Choose a zoom step (a ZOOM cell's click). EditorHost ignores a value that is not one of zoomSteps(); it
        // stores any other as the machine-wide preference (EditorConfig::zoomPrefKey), and every open editor in the
        // process follows through UiPreferences::revision(), like the theme. A step that does not fit the display is
        // kept as the preference and drawn at the largest step that fits (zoomPercent() says which).
        virtual void setZoomPercent(int /*percent*/) {}

        // The steps a ZOOM control offers, in percent, ascending; empty when the host has no zoom (the default: the
        // window is the Panel's own size, exactly as before v0.8.0). Valid while the Panel is attached.
        virtual std::span<const int> zoomSteps() const { return {}; }

        // Whether choosing `percent` would draw at it: a listed step whose window fits the user area of the editor's
        // display (lead, v0.8.0), so a ZOOM control can mark the steps this display cannot show instead of letting a
        // click fall back silently. EditorHost: false for an unlisted value; true under a pin (never fitted), when no
        // display is known, and for the smallest step (drawn when nothing fits). HeadlessHost: setZoomFitLimit.
        // Default: true.
        virtual bool zoomFits(int /*percent*/) const { return true; }

        // ---- Web Sprint B additions (v0.12.0; FCompressor ADR-93). Not pure: the defaults refuse. -------------------
        //
        // A popup menu, a file chooser and the clipboard as plain calls (the request types are above the class), so a
        // Panel needs no JUCE and no ownerComponent() for them, a host without JUCE can serve them, and HeadlessHost
        // can script them. A host written before them keeps compiling and serves nothing.
        //
        // The rules every host keeps for the callbacks of showMenu and chooseFiles:
        // - a callback runs on the message thread, at most once, and never from inside the call that took it;
        // - it never runs after dismissMenus() (a menu's), after the host has let go of the Panel (EditorHost: its
        //   editor left its window; HeadlessHost: its destructor, before Panel::closeGestures()) or after the host is
        //   destroyed;
        // - a second showMenu replaces a menu still showing (even when the second is refused), a second chooseFiles a
        //   chooser still open: the replaced request's callback does not run. A menu and a chooser do not replace
        //   each other.
        // A callback that does not run is destroyed without being called, when it is dropped (a refused one: when the
        // call returns), so what it captured is released. One that runs may call showMenu or chooseFiles again. A
        // sub-view that can go before its Panel still guards what its callbacks touch, as with any deferred call.
        // A Panel must not call a service from its destructor, nor a sub-view from a destructor that runs with the
        // Panel's: the host may be partly or wholly gone by then (as for every other call of HostServices), and it
        // has dropped the callbacks already, so nothing is left to dismiss.

        // What this host serves: a mask of hostservice bits, so a view can disable a cell its host cannot serve. A
        // call for a service that is not reported refuses, as the defaults below do. EditorHost and HeadlessHost: all
        // three. Default: none (0).
        virtual unsigned services() const { return 0; }

        // Opens the menu beside request.anchor in request.theme's colours and returns true; `done` then runs once
        // with the chosen id, or with 0 when the user dismisses the menu. Returns false, and drops `done` unrun, when
        // the host shows no menus, the request has no item that is not a separator (no items at all, or separators
        // alone: nothing could be chosen, and no host opens such a menu), or an item that is not a separator has an
        // id <= 0.
        // EditorHost: a juce::PopupMenu with a funkgui::MenuLook of the theme, anchored to the rectangle on screen
        // under the UI zoom (the rectangle times the editor's width over the Panel's, to the nearest editor px).
        // HeadlessHost: nothing is shown; the request is logged and stays pending until the test answers it
        // (chooseMenuItem, cancelMenu), which is when `done` runs. Default: false.
        virtual bool showMenu(const MenuRequest& /*request*/, MenuCallback /*done*/) { return false; }

        // Closes the menu this host is showing, if any; its callback does not run. For a Panel whose menu no longer
        // means anything (the sub-view that asked is going away while the Panel stays). EditorHost: JUCE can only
        // dismiss every popup menu of the process, so it does that, and only while a menu of its own is showing.
        // HeadlessHost: the pending menu is dropped, and the call counted. Default: nothing.
        virtual void dismissMenus() {}

        // Opens a file chooser and returns true; `done` then runs once with the chosen paths, or with none when the
        // user cancels. Mode::save warns before an existing file is overwritten, and its path ends in the pattern's
        // extension when the pattern is a single "*.<ext>": the host replaces another extension or appends it
        // (compared without case), as a native save panel does. Returns false, and drops `done` unrun, when the host
        // has no chooser. EditorHost: a native juce::FileChooser parented on the editor, starting in the user's
        // Documents folder (Mode::save: at the suggested name there). HeadlessHost: nothing is shown; the request is
        // logged and stays pending until the test answers it (returnFiles, cancelFiles). Default: false.
        virtual bool chooseFiles(const FileRequest& /*request*/, FilesCallback /*done*/) { return false; }

        // Puts the text on the system clipboard; returns whether it was written. EditorHost: juce::SystemClipboard.
        // HeadlessHost: logged (log.lastCopy), no clipboard is touched, true. Default: false.
        virtual bool copyText(std::string_view /*utf8*/) { return false; }

        // Whether the platform's command key is Meta (Cmd, as on macOS) rather than Ctrl: which name a shortcut hint
        // prints, and which of Mods::cmd and Mods::ctrl alone means "command" (JUCE sets both for Ctrl where Ctrl is
        // the command key). EditorHost: the default, which is JUCE's own rule. HeadlessHost: the default until
        // setCommandKeyIsMeta(). Default: the platform this was compiled for (true on Apple's).
        virtual bool commandKeyIsMeta() const
        {
           #if defined(__APPLE__)
            return true;
           #else
            return false;
           #endif
        }
    };
}
