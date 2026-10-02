#pragma once

// A web host's side of the services of panel/HostServices.h (FCompressor ADR-93, web Sprint C; v0.13.0): a popup menu
// in the page's DOM and the system clipboard, the browser's counterparts of what src/gpu/HostServicesJuce does with
// JUCE. Private to src/web (FunkGui::web, Emscripten only): WebHost owns one for its whole life and forwards
// HostServices::showMenu, dismissMenus and copyText to it; a product never includes this header.
//
// It was the seam between the two cards of Sprint C (WebHost calls it as declared; WebServices.cpp implements it), which
// is why the class is this small and everything else lies behind `Impl`.
//
// The rules are HostServices' own (panel/HostServices.h, "Web Sprint B additions"): a callback runs on the main
// thread, at most once, never inside the call that took it, and never after dismissMenus(), after letGo() or after
// this object is destroyed; a second showMenu replaces a menu still showing (even when the second is refused); a
// request with no items, with no item that is not a separator, or with a non-separator id <= 0 is refused and its
// callback dropped unrun. A file chooser is not served here: a web host reports no hostservice::fileChooser.

#include <funkgui/panel/HostServices.h>

#include <memory>
#include <string_view>

namespace funkgui::web
{
    class WebServices
    {
    public:
        // The canvas the Panel is drawn on, by CSS selector (the one the host's WebGlSink was given): a menu is
        // anchored to its box and returns the keyboard focus to it. The canvas is looked up at each call, not kept.
        explicit WebServices(const char* canvasSelector);
        ~WebServices();                                      // letGo()

        WebServices(const WebServices&) = delete;
        WebServices& operator=(const WebServices&) = delete;

        // HostServices::showMenu. `logicalWidth` is the Panel's width in its own px: the canvas's CSS width over it is
        // the CSS px per Panel px (the UI zoom times any page scaling), so request.anchor times that factor is the
        // anchor in CSS px from the canvas's top-left corner. The menu takes request.theme's colours, the bundled
        // face and the native menu's metrics (MenuLook over JUCE's LookAndFeel_V4), which do not follow the UI zoom.
        // True: the menu is open and `done` will run once, from a later event of the page, with the chosen id (an
        // item's click or key; a release over an item the pointer moved onto with the opening button still held, as
        // JUCE chooses) or with 0 (a press outside the menu, Escape, the window losing focus or being resized, a
        // scroll that moves the canvas). A press on the canvas that dismisses the menu goes no further, as the
        // native menu swallows it; a press elsewhere in the page still reaches its target. False: refused, `done`
        // dropped unrun.
        bool showMenu(const MenuRequest& request, MenuCallback done, float logicalWidth);

        // HostServices::dismissMenus: closes the menu this object is showing, if any; its callback does not run.
        void dismissMenus();

        // HostServices::copyText: navigator.clipboard.writeText, which the browser allows only during the handling of
        // a user's gesture (a host delivers input to its Panel inside the DOM event's handler for this reason), with
        // a fallback where that API is missing. True: the write was issued (its outcome arrives later, if at all).
        bool copyText(std::string_view utf8);

        // The host lets go of its Panel: the menu closes and its callback is dropped unrun. Idempotent; the
        // destructor calls it.
        void letGo();

        bool menuOpen() const noexcept;                      // a menu of this object's is showing (for the host's tests)

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
