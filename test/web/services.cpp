// The browser check of FunkGui::web's services (v0.13.0; FCompressor ADR-93, docs/sprints/web-c.md G-E): the popup
// menu in the page's DOM and the clipboard (src/web/WebServices.h), and the preferences in localStorage
// (web/WebPrefs.h). Not a self-registered test (this file has no test line): node has no document. It needs no
// WebHost and draws nothing: a canvas, a WebServices, and the page's own script (services.html) to look at the
// document and to make events. test/web/services.cmake builds it, in the `web` preset only, into
// <build>/test/web/{services.html, funkgui_web_services.js, funkgui_web_services.wasm}; tools/web/check-page.mjs
// (CTest fg.web.services, label live) serves that directory on 127.0.0.1 and runs it in headless Chrome. Or serve it
// and open it:
//
//   python3 -m http.server 8137 --bind 127.0.0.1 --directory <build>/test/web      http://127.0.0.1:8137/services.html
//
// The verdict is machine-readable, as the sink page's: document.title is "RUNNING" until the end, then "PASS" or
// "FAIL: <the first thing that failed>"; every line goes to console.log and to the page's <pre>. PASS means all of:
//
//  1. The menu is in the document as the request has it: one role=menu element under <body>, one element per item in
//     order with its text, a tick (and aria-checked) on a checked item, aria-disabled on a disabled one, a separator;
//     the colours are the request's Theme (ground, ink100, a disabled item at half alpha, ink16 under the pointer),
//     the face is the bundled one, loaded from the embedded data, at 14 px; the page's own style sheet (services.html
//     has a rule for every div) reaches none of it; the rows have LookAndFeel_V4's heights.
//  2. It sits beside the anchor: under it, left edges aligned, at a canvas scale of 1 and of 1.5 (the request's
//     rectangle times the canvas's CSS width over the logical width, from the canvas's box), and inside the window
//     when the anchor is at its right and bottom edge (then over the anchor).
//  3. A click on an item runs the callback once, with its id, not inside showMenu; the menu has left the document,
//     the face has left document.fonts and the focus is the canvas's by then. A disabled item, the separator and a
//     second click on the same element give nothing.
//  4. Keys: Down and Up move the highlight over the items that can be chosen, round the ends; Return and Space choose
//     the highlighted item and do nothing without one; Escape gives 0. While the menu is open no key reaches the
//     canvas.
//  5. A press outside the menu, the window's blur and its resize give 0; a press inside it and an element's blur do
//     not dismiss it.
//  6. dismissMenus(), letGo() and the destructor close the menu, run nothing and release the callback; an item of
//     such a menu clicked afterwards runs nothing. After letGo() the object shows menus again.
//  7. A second menu replaces the first, whose callback is released unrun. THE SERIAL: an item of the replaced menu,
//     clicked while the second is open, runs neither callback and leaves the second open. A second menu that is
//     refused still closes the first.
//  8. The refusals: no items, separators alone, an id of 0 or below, and a selector that names no element. Each
//     returns false, releases the callback unrun and leaves the document as it was.
//  9. An item clicked while showMenu is still running (the page's focus handling, services.html's clickInsideShow)
//     runs the callback later, never inside the call. A callback may show another menu. Two objects: one's
//     dismissMenus() and destruction leave the other's menu alone.
// 10. Nothing is left: after every menu the document has the elements it had; after the last object, the module has
//     no face.
// 11. copyText() calls navigator.clipboard.writeText once with the text, UTF-8 intact, and returns true; a refusal
//     by the browser is no unhandled rejection. On a page without navigator.clipboard it copies through
//     document.execCommand on a textarea that holds the text selected, returns what the browser returned, and leaves
//     no element behind and the focus where it was. What the browser then did is printed, not judged: a script's
//     click is not a user's gesture, so Chrome may refuse the write and the reading back (see "by hand" below).
// 12. installLocalStoragePrefs(): UiPreferences writes <prefix><key> to localStorage as decimal text; a new backend
//     reads it back (a page loaded again); reload() follows another tab's write and removal, bumps revision() once,
//     and not again; keys of another prefix are not seen; a write the storage refuses is kept for the page and
//     survives reload(); a browser that forbids storage altogether gives the defaults and loses nothing held; null
//     means the product's prefix.
//
// By hand, after the verdict: a click on the canvas opens a menu at the pointer (a real gesture). "Copy this line"
// calls copyText() inside the item's click, and the page prints what the browser made of the write; paste anywhere
// to see the text.

#include "../../src/web/WebServices.h"

#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/web/WebPrefs.h>

#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using funkgui::Col;
using funkgui::MenuCallback;
using funkgui::MenuItem;
using funkgui::MenuRequest;
using funkgui::Rect;
using funkgui::Theme;
using funkgui::UiPreferences;
using funkgui::web::WebServices;

// ---- the page's side of the check, in JavaScript -------------------------------------------------------------------
typedef void (*FgPageClick)(double x, double y);

EM_JS_DEPS(fg_services_page_deps, "$UTF8ToString,$getWasmTableEntry");

EM_JS(void, fg_page_line, (const char* text), {
    const line = UTF8ToString(text);
    console.log(line);
    const pre = document.getElementById('funkgui-log');
    if (pre) pre.textContent += line + '\n';
});

EM_JS(void, fg_page_title, (const char* text), { document.title = UTF8ToString(text); });

// After the verdict: a click on the canvas goes to `click`, in the canvas's CSS px.
EM_JS(void, fg_page_by_hand, (FgPageClick click), {
    fg.canvas().addEventListener('click', (e) => getWasmTableEntry(click)(e.offsetX, e.offsetY));
});

namespace
{
    constexpr const char* kCanvas = "#funkgui-canvas";
    constexpr float kLogicalW = 480.0f;          // the canvas is 480 x 300 CSS px until a case scales it
    constexpr double kPollMs = 10.0, kWaitMs = 5000.0;
    constexpr const char* kPrefix = "fg.services.";

    // ---- output -----------------------------------------------------------------------------------------------------
    std::string firstFailure;                    // "" while everything passed

    __attribute__((format(printf, 1, 2))) void say(const char* format, ...)
    {
        char text[2048];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof text, format, args);
        va_end(args);
        fg_page_line(text);
    }

    void fail(const std::string& why)
    {
        say("FAIL     %s", why.c_str());
        if (firstFailure.empty())
            firstFailure = why;
    }

    // A claim the run makes: printed as it stands when it holds, the failure when it does not.
    bool check(bool ok, const std::string& claim)
    {
        if (ok)
            say("ok       %s", claim.c_str());
        else
            fail("it is not true that " + claim);
        return ok;
    }

    void verdict()
    {
        if (firstFailure.empty())
        {
            say("VERDICT  PASS");
            fg_page_title("PASS");
        }
        else
        {
            say("VERDICT  FAIL: %s", firstFailure.c_str());
            fg_page_title(("FAIL: " + firstFailure).c_str());
        }
    }

    // ---- the document, through the page's script (services.html's `fg`) ---------------------------------------------
    int js(const std::string& code)
    {
        return emscripten_run_script_int(code.c_str());
    }

    std::string jsText(const std::string& code)
    {
        const char* text = emscripten_run_script_string(code.c_str());
        return text != nullptr ? text : "";
    }

    double jsNumber(const std::string& code)
    {
        return std::strtod(jsText(code).c_str(), nullptr);
    }

    // `text` as a JavaScript string literal.
    std::string quoted(const std::string& text)
    {
        std::string out = "'";
        for (const char c : text)
        {
            if (c == '\\' || c == '\'')
                out += '\\';
            if (c == '\n')
                out += "\\n";
            else
                out += c;
        }
        return out + "'";
    }

    std::string item(int id)
    {
        return "fg.item(" + std::to_string(id) + ")";
    }

    int menusInDocument()
    {
        return js("fg.menus().length");
    }

    struct Box
    {
        double left = 0, top = 0, width = 0, height = 0;
    };

    Box boxOf(const std::string& element)
    {
        const std::string r = "(" + element + ").getBoundingClientRect()";
        return { jsNumber(r + ".left"), jsNumber(r + ".top"), jsNumber(r + ".width"), jsNumber(r + ".height") };
    }

    double rounded(double v)                     // JavaScript's Math.round
    {
        return std::floor(v + 0.5);
    }

    std::string rgb(Col c)
    {
        return "rgb(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ")";
    }

    std::string rgba(Col c, const char* alpha)
    {
        return "rgba(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ", " + alpha
             + ")";
    }

    // What the document holds besides the page's own elements: 0 menus, the children <body> started with, no face
    // registered, no textarea.
    int bodyAtStart = 0;

    bool documentIsClean()
    {
        return menusInDocument() == 0 && js("document.body.childElementCount") == bodyAtStart
            && js("fg.faces().length") == 0
            && js("document.querySelectorAll('textarea, [data-funkgui-menu]').length") == 0;
    }

    // ---- a callback that tells what happened to it ------------------------------------------------------------------
    struct Call
    {
        int  runs = 0;
        int  id = -1;
        bool insideShow = false;
    };

    bool inShowMenu = false;                     // a showMenu call is on the stack

    // The callback holds the only other reference to its Call: use_count() == 1 says it was destroyed.
    MenuCallback recording(const std::shared_ptr<Call>& call)
    {
        return [call](int id) {
            ++call->runs;
            call->id = id;
            call->insideShow = inShowMenu;
        };
    }

    bool show(WebServices& services, const MenuRequest& request, MenuCallback done, float logicalWidth = kLogicalW)
    {
        inShowMenu = true;
        const bool shown = services.showMenu(request, std::move(done), logicalWidth);
        inShowMenu = false;
        return shown;
    }

    bool released(const std::shared_ptr<Call>& call)
    {
        return call.use_count() == 1;
    }

    bool ranOnce(const std::shared_ptr<Call>& call, int id)
    {
        return call->runs == 1 && call->id == id && !call->insideShow && released(call);
    }

    std::string told(const std::shared_ptr<Call>& call)
    {
        return "(runs " + std::to_string(call->runs) + ", id " + std::to_string(call->id)
             + (call->insideShow ? ", inside showMenu" : "") + (released(call) ? ", released)" : ", still held)");
    }

    MenuItem separator()
    {
        MenuItem it;
        it.separator = true;
        return it;
    }

    // The menu of the first cases: every kind of row.
    MenuRequest fullMenu(const Theme& theme, Rect anchor)
    {
        MenuRequest r;
        r.items = { { 1, "Save" }, { 2, "Save As\xE2\x80\xA6" }, separator(), { 3, "Sidechain", true, true },
                    { 4, "Import", false }, { 5, "Locked", false, true } };
        r.anchor = anchor;
        r.theme = theme;
        return r;
    }
    constexpr const char* kFullRows = "menuitem|1|Save|-|-|-\n"
                                      "menuitem|2|Save As\xE2\x80\xA6|-|-|-\n"
                                      "---\n"
                                      "menuitemcheckbox|3|Sidechain|true|-|tick\n"
                                      "menuitem|4|Import|-|true|-\n"
                                      "menuitemcheckbox|5|Locked|true|true|tick";
    constexpr int kFullHeight = 5 * 18 + 10 + 4;     // V4: rows of 18, a separator of 10, a border of 2

    MenuRequest smallMenu(int firstId, const char* first, const char* second)
    {
        MenuRequest r;
        r.items = { { firstId, first }, { firstId + 1, second } };
        r.anchor = Rect{ 20.0f, 20.0f, 80.0f, 16.0f };
        return r;
    }

    // ---- the run ----------------------------------------------------------------------------------------------------
    struct Page
    {
        std::unique_ptr<WebServices> services;
        std::shared_ptr<Call>        call;       // the case that is waiting
        double                       waitedMs = 0.0;
        std::string                  copied;

        std::unique_ptr<WebServices> byHand;     // after the verdict
        int                          byHandTheme = 0;
        int                          byHandCopies = 0;
    };
    Page page;

    void structure();
    void afterInsideShow(void*);
    void clipboard();
    void afterWrite(void*);
    void afterRead(void*);
    void fallbackAndPreferences();
    void byHand();

    // ---- 1 (the face): the first menu is opened, and the run waits for the browser to load the face ----------------
    void afterFace(void*)
    {
        const std::string status = jsText("fg.faces().length === 1 ? fg.faces()[0].status : 'absent'");
        if (status == "loading" && page.waitedMs < kWaitMs)
        {
            page.waitedMs += kPollMs;
            emscripten_set_timeout(afterFace, kPollMs, nullptr);
            return;
        }
        check(status == "loaded", "the bundled face is a FontFace of the document while the menu shows, loaded from "
                                  "the embedded data (status " + status + " after "
                                      + std::to_string(static_cast<int>(page.waitedMs)) + " ms)");
        structure();
    }

    void start()
    {
        say("FunkGui::web: WebServices and the localStorage preferences (%s)",
            jsText("navigator.userAgent").c_str());
        bodyAtStart = js("document.body.childElementCount");
        js("fg.canvas().focus()");
        check(documentIsClean() && js("document.activeElement === fg.canvas()") == 1,
              "the page starts with no menu and no face, and the canvas has the focus");

        page.services = std::make_unique<WebServices>(kCanvas);
        check(documentIsClean() && !page.services->menuOpen(),
              "a WebServices that has shown nothing has put nothing in the document");

        page.call = std::make_shared<Call>();
        const bool shown = show(*page.services, fullMenu(Theme::graphite(), Rect{ 40.0f, 30.0f, 120.0f, 20.0f }),
                                recording(page.call));
        if (!check(shown && page.services->menuOpen() && page.call->runs == 0 && menusInDocument() == 1,
                   "showMenu returns true with the menu in the document, and its callback has not run "
                       + told(page.call)))
        {
            verdict();
            return;
        }
        page.waitedMs = 0.0;
        emscripten_set_timeout(afterFace, kPollMs, nullptr);
    }

    // ---- 1 to 5: one menu after another, every event made by the page's script -------------------------------------
    void structure()
    {
        WebServices& services = *page.services;
        const Theme graphite = Theme::graphite();
        const Theme paper = Theme::paper();

        // ---- 1. what the document holds (the menu start() opened)
        {
            const std::string rows = jsText("fg.rows()");
            check(rows == kFullRows && js("fg.menu().parentNode === document.body") == 1,
                  "the menu is one role=menu element under <body> with a row per item, in order:\n" + rows);
            const std::string ground = jsText("fg.style(fg.menu(), 'backgroundColor')");
            const std::string ink = jsText("fg.style(" + item(1) + ", 'color')");
            const std::string dim = jsText("fg.style(" + item(4) + ", 'color')");
            const std::string line = jsText("fg.style(fg.separator(), 'backgroundColor')");
            const std::string edge = jsText("fg.style(fg.menu(), 'borderTopColor')");
            check(ground == rgb(graphite.ground) && ink == rgb(graphite.ink100) && dim == rgba(graphite.ink100, "0.5")
                      && line == rgba(graphite.ink100, "0.3") && edge == rgba(graphite.ink100, "0.6"),
                  "its colours are the request's Theme: ground " + ground + ", text " + ink + ", a disabled item "
                      + dim + ", the separator " + line + ", the edge " + edge);
            const std::string family = jsText("fg.style(" + item(1) + ", 'fontFamily')");
            const std::string size = jsText("fg.style(" + item(1) + ", 'fontSize')");
            check(family.rfind("funkgui-menu", 0) == 0 || family.rfind("\"funkgui-menu\"", 0) == 0,
                  "the items are set in the bundled face first (font-family " + family + ")");
            check(size == "14px" && js("document.fonts.check('14px \"funkgui-menu\"') ? 1 : 0") == 1,
                  "at 14 CSS px, a size the document has the face for (font-size " + size + ")");
            const std::string theirs = jsText("['fontStyle', 'textTransform', 'marginTop', 'paddingTop'].map((p) => "
                                              "fg.style(" + item(1) + ", p)).join(' ') + ' / ' + "
                                              "['marginTop', 'paddingTop', 'fontStyle'].map((p) => "
                                              "fg.style(fg.menu(), p)).join(' ')");
            check(theirs == "normal none 0px 0px / 0px 1px normal",
                  "the page's rule for every div reaches neither the menu nor an item (" + theirs + ")");
            const int rowH = js(item(1) + ".offsetHeight"), menuH = js("fg.menu().offsetHeight");
            const double textIn = jsNumber("(() => { const r = document.createRange(); "
                                           "r.selectNodeContents(fg.item(1).firstChild); return "
                                           "r.getBoundingClientRect().left - fg.menu().getBoundingClientRect().left; "
                                           "})()");
            const double tickIn = jsNumber("fg.item(3).querySelector('svg').getBoundingClientRect().left - "
                                           "fg.menu().getBoundingClientRect().left");
            char metrics[256];
            std::snprintf(metrics, sizeof metrics, "the rows have LookAndFeel_V4's metrics for a 14 px font: a row "
                          "%d px, the menu %d px, text %.2f px in, the tick %.2f px in", rowH, menuH, textIn, tickIn);
            check(rowH == 18 && menuH == kFullHeight && std::fabs(textIn - 18.0) < 0.01
                      && std::fabs(tickIn - 8.4) < 0.01, metrics);
            check(js("document.activeElement === fg.menu()") == 1, "the menu has the keyboard focus");
        }

        // ---- 2. beside the anchor, at scale 1
        {
            const Box canvas = boxOf("fg.canvas()");
            const Box menu = boxOf("fg.menu()");
            const double wantLeft = rounded(canvas.left + 40.0), wantTop = rounded(canvas.top + 30.0 + 20.0);
            char text[256];
            std::snprintf(text, sizeof text, "the menu's corner is at (%.2f, %.2f), under the anchor's left end at "
                          "(%.2f, %.2f) on a canvas at (%.2f, %.2f) of scale 1", menu.left, menu.top, wantLeft,
                          wantTop, canvas.left, canvas.top);
            check(menu.left == wantLeft && menu.top == wantTop, text);
        }

        // ---- 1, 3. the pointer: the highlight, what cannot be chosen, the click
        {
            js("fg.hover(" + item(2) + ")");
            const std::string lit = jsText("fg.style(" + item(2) + ", 'backgroundColor')");
            const std::string unlit = jsText("fg.style(" + item(1) + ", 'backgroundColor')");
            check(lit == rgb(graphite.ink16) && unlit == "rgba(0, 0, 0, 0)" && js("fg.litId()") == 2,
                  "the item under the pointer is highlighted in ink16 (" + lit + "), and no other (" + unlit + ")");
            js("fg.hover(" + item(4) + ")");
            check(js("fg.litId()") == 0 && jsText("fg.style(" + item(4) + ", 'backgroundColor')") == "rgba(0, 0, 0, 0)"
                      && jsText("fg.style(" + item(2) + ", 'backgroundColor')") == "rgba(0, 0, 0, 0)",
                  "a disabled item under the pointer is not highlighted, and takes the highlight off");

            js(item(4) + ".click()");
            js(item(5) + ".click()");
            js("fg.separator().click()");
            js("fg.menu().click()");
            check(page.call->runs == 0 && services.menuOpen() && menusInDocument() == 1,
                  "a click on a disabled item, on the separator or on the menu's edge chooses nothing and leaves the "
                  "menu open " + told(page.call));

            const int keysBefore = js("fg.canvasKeys");
            const int prevented = js("fg.key(fg.canvas(), 'a')");
            check(js("fg.canvasKeys") == keysBefore && prevented == 0,
                  "a key pressed on the canvas while the menu is open does not reach the canvas's listener");

            js("fg.stale = " + item(2));
            js("fg.stale.click()");
            check(ranOnce(page.call, 2), "a click on an item runs the callback once with its id, from the page's "
                                         "event and not inside showMenu " + told(page.call));
            check(!services.menuOpen() && documentIsClean() && js("document.activeElement === fg.canvas()") == 1,
                  "the menu has left the document, the face has left document.fonts, and the canvas has the focus");
            js("fg.stale.click()");
            check(page.call->runs == 1, "the same element clicked again runs nothing");
            check(js("fg.key(fg.canvas(), 'a')") == 0 && js("fg.canvasKeys") == keysBefore + 1,
                  "with the menu closed the key reaches the canvas");
        }

        // ---- 2, 4. a canvas scaled by 1.5, the other theme, and the keyboard
        {
            js("fg.canvas().style.width = '720px'; fg.canvas().style.height = '450px'; 0");
            const auto call = std::make_shared<Call>();
            const bool shown = show(services, fullMenu(paper, Rect{ 100.0f, 40.0f, 60.0f, 16.0f }), recording(call));
            const Box canvas = boxOf("fg.canvas()");
            const Box menu = boxOf("fg.menu()");
            const double wantLeft = rounded(canvas.left + 100.0 * 1.5), wantTop = rounded(canvas.top + 56.0 * 1.5);
            char text[256];
            std::snprintf(text, sizeof text, "on a canvas %.0f CSS px wide for 480 logical px the menu's corner is at "
                          "(%.2f, %.2f), the anchor's times 1.5: (%.2f, %.2f)", canvas.width, menu.left, menu.top,
                          wantLeft, wantTop);
            check(shown && canvas.width == 720.0 && menu.left == wantLeft && menu.top == wantTop, text);
            const std::string ground = jsText("fg.style(fg.menu(), 'backgroundColor')");
            const std::string ink = jsText("fg.style(" + item(1) + ", 'color')");
            check(ground == rgb(paper.ground) && ink == rgb(paper.ink100) && js(item(1) + ".offsetHeight") == 18
                      && jsText("fg.style(" + item(1) + ", 'fontSize')") == "14px",
                  "in the request's other Theme (ground " + ground + ", text " + ink + "), at the same 14 px: the menu "
                  "does not scale with the canvas");

            const int enter = js("fg.key(document.activeElement, 'Enter')");
            check(call->runs == 0 && services.menuOpen() && enter == 1,
                  "Return with no item highlighted chooses nothing");
            std::string walk;
            for (const char* key : { "ArrowDown", "ArrowDown", "ArrowDown", "ArrowDown", "ArrowUp", "ArrowUp" })
                walk += std::to_string(js(std::string("fg.key(document.activeElement, '") + key + "')") == 1
                                           ? js("fg.litId()") : -1) + " ";
            check(walk == "1 2 3 1 3 2 ",
                  "Down, Down, Down, Down, Up, Up highlight the items that can be chosen, round the ends: " + walk);
            const std::string lit = jsText("fg.style(" + item(2) + ", 'backgroundColor')");
            check(lit == rgb(paper.ink16), "the highlight is the Theme's ink16 (" + lit + ")");
            js("fg.key(document.activeElement, 'Enter')");
            check(ranOnce(call, 2) && documentIsClean() && js("document.activeElement === fg.canvas()") == 1,
                  "Return chooses the highlighted item " + told(call));

            const auto space = std::make_shared<Call>();
            show(services, fullMenu(paper, Rect{ 100.0f, 40.0f, 60.0f, 16.0f }), recording(space));
            js("fg.key(document.activeElement, 'ArrowUp')");
            js("fg.key(document.activeElement, ' ')");
            check(ranOnce(space, 3), "Up from nothing highlights the last item that can be chosen, and Space chooses "
                                     "it " + told(space));
            js("fg.canvas().style.width = ''; fg.canvas().style.height = ''; 0");
        }

        // ---- 2, 4. kept inside the window; Escape
        {
            const Box canvas = boxOf("fg.canvas()");
            const double viewW = jsNumber("document.documentElement.clientWidth");
            const double viewH = jsNumber("document.documentElement.clientHeight");
            const auto call = std::make_shared<Call>();
            // The anchor's left end 20 px from the window's right edge, its bottom 10 px from the window's.
            const Rect anchor{ static_cast<float>(viewW - 20.0 - canvas.left),
                               static_cast<float>(viewH - 30.0 - canvas.top), 60.0f, 20.0f };
            show(services, fullMenu(graphite, anchor), recording(call));
            const Box menu = boxOf("fg.menu()");
            char text[320];
            std::snprintf(text, sizeof text, "a menu anchored at the window's right and bottom edge (%.0f x %.0f) is "
                          "inside it, over the anchor: (%.2f, %.2f) to (%.2f, %.2f), the anchor's top at %.2f", viewW,
                          viewH, menu.left, menu.top, menu.left + menu.width, menu.top + menu.height, viewH - 30.0);
            check(menu.left >= 0.0 && menu.left + menu.width <= viewW && menu.top >= 0.0
                      && menu.top + menu.height <= viewH && std::fabs(menu.top + menu.height - (viewH - 30.0)) <= 1.0
                      && std::fabs(menu.left + menu.width - (viewW - 6.0)) <= 1.0, text);
            const int escape = js("fg.key(document.activeElement, 'Escape')");
            check(ranOnce(call, 0) && escape == 1 && documentIsClean(), "Escape gives 0 " + told(call));
        }

        // ---- 5. the dismissals
        {
            const auto call = std::make_shared<Call>();
            show(services, fullMenu(graphite, Rect{ 40.0f, 30.0f, 120.0f, 20.0f }), recording(call));
            js("fg.press(" + item(1) + ")");
            js("fg.press(fg.separator())");
            js("fg.canvas().dispatchEvent(new FocusEvent('blur'))");
            check(call->runs == 0 && services.menuOpen() && menusInDocument() == 1,
                  "a press inside the menu and an element's blur do not dismiss it");
            js("fg.press(fg.canvas())");
            check(ranOnce(call, 0) && documentIsClean() && js("document.activeElement === fg.canvas()") == 1,
                  "a press outside the menu gives 0 " + told(call));

            const auto blurred = std::make_shared<Call>();
            show(services, smallMenu(1, "One", "Two"), recording(blurred));
            js("window.dispatchEvent(new Event('blur'))");
            check(ranOnce(blurred, 0) && documentIsClean(), "the window's blur gives 0 " + told(blurred));

            const auto resized = std::make_shared<Call>();
            show(services, smallMenu(1, "One", "Two"), recording(resized));
            js("window.dispatchEvent(new Event('resize'))");
            check(ranOnce(resized, 0) && documentIsClean(), "the window's resize gives 0 " + told(resized));
            const int keys = js("fg.canvasKeys");
            const int prevented = js("fg.key(fg.canvas(), 'Escape')");
            check(prevented == 0 && js("fg.canvasKeys") == keys + 1,
                  "the closed menu's listeners have left the window: Escape on the canvas is the canvas's again");
        }

        // ---- 6. dismissMenus, letGo: nothing runs, the callback is released, a stale item is nothing
        {
            const auto dismissed = std::make_shared<Call>();
            show(services, smallMenu(1, "One", "Two"), recording(dismissed));
            js("fg.stale = " + item(1));
            services.dismissMenus();
            check(dismissed->runs == 0 && released(dismissed) && !services.menuOpen() && documentIsClean()
                      && js("document.activeElement === fg.canvas()") == 1,
                  "dismissMenus() closes the menu, runs nothing and releases the callback " + told(dismissed));
            js("fg.stale.click(); fg.key(document.body, 'Escape'); fg.press(document.body)");
            services.dismissMenus();
            check(dismissed->runs == 0, "its item clicked afterwards, a key and a press run nothing");

            const auto let = std::make_shared<Call>();
            show(services, smallMenu(1, "One", "Two"), recording(let));
            js("fg.stale = " + item(2));
            services.letGo();
            services.letGo();
            js("fg.stale.click()");
            check(let->runs == 0 && released(let) && !services.menuOpen() && documentIsClean(),
                  "letGo() closes the menu, runs nothing and releases the callback, twice over " + told(let));
            const auto again = std::make_shared<Call>();
            const bool shown = show(services, smallMenu(1, "One", "Two"), recording(again));
            js(item(1) + ".click()");
            check(shown && ranOnce(again, 1), "after letGo() the object shows a menu again " + told(again));
        }

        // ---- 7. a second menu replaces the first; THE SERIAL
        {
            const auto first = std::make_shared<Call>();
            const auto second = std::make_shared<Call>();
            show(services, smallMenu(1, "First one", "First two"), recording(first));
            js("fg.stale = " + item(1));
            const bool shown = show(services, smallMenu(7, "Second seven", "Second eight"), recording(second));
            const std::string rows = jsText("fg.rows()");
            check(shown && first->runs == 0 && released(first) && menusInDocument() == 1
                      && rows == "menuitem|7|Second seven|-|-|-\nmenuitem|8|Second eight|-|-|-",
                  "a second menu replaces the first, whose callback is released unrun " + told(first));
            js("fg.stale.click()");
            check(first->runs == 0 && second->runs == 0 && services.menuOpen() && menusInDocument() == 1,
                  "an item of the replaced menu, clicked while the second is open, runs neither callback and leaves "
                  "the second open: first " + told(first) + ", second " + told(second));
            js(item(8) + ".click()");
            check(ranOnce(second, 8) && first->runs == 0 && documentIsClean(),
                  "the second menu's own item then runs its callback " + told(second));

            const auto closed = std::make_shared<Call>();
            const auto refused = std::make_shared<Call>();
            show(services, smallMenu(1, "One", "Two"), recording(closed));
            const bool empty = show(services, MenuRequest{}, recording(refused));
            check(!empty && closed->runs == 0 && released(closed) && refused->runs == 0 && released(refused)
                      && !services.menuOpen() && documentIsClean(),
                  "a second menu that is refused still closes the first; neither callback runs");
        }

        // ---- 8. the refusals
        {
            const auto refuses = [](WebServices& on, MenuRequest request, const char* what)
            {
                const auto call = std::make_shared<Call>();
                request.anchor = Rect{ 10.0f, 10.0f, 10.0f, 10.0f };
                const bool shown = show(on, request, recording(call));
                check(!shown && call->runs == 0 && released(call) && !on.menuOpen() && documentIsClean(),
                      std::string("a request with ") + what + " is refused: false, the callback released unrun, "
                          "nothing in the document " + told(call));
            };
            MenuRequest none;
            refuses(services, none, "no items");
            MenuRequest lines;
            lines.items = { separator(), separator() };
            refuses(services, lines, "separators alone");
            MenuRequest zero;
            zero.items = { { 1, "One" }, { 0, "Zero" } };
            refuses(services, zero, "an item whose id is 0");
            MenuRequest negative;
            negative.items = { { -3, "Minus three" }, { 2, "Two" } };
            refuses(services, negative, "an item whose id is below 0");

            WebServices nowhere("#funkgui-no-such-canvas");
            refuses(nowhere, smallMenu(1, "One", "Two"), "a canvas selector that names no element");
            WebServices malformed("##");
            refuses(malformed, smallMenu(1, "One", "Two"), "a malformed canvas selector");
        }

        // ---- 9. a callback may show another menu; two objects
        {
            const auto inner = std::make_shared<Call>();
            int outerId = -1;
            bool innerShown = false;
            show(services, smallMenu(1, "One", "Two"), [&](int id) {
                outerId = id;
                innerShown = show(services, smallMenu(7, "Seven", "Eight"), recording(inner));
            });
            js(item(2) + ".click()");
            const std::string rows = jsText("fg.menus().length === 1 ? fg.rows() : ''");
            check(outerId == 2 && innerShown && services.menuOpen() && inner->runs == 0
                      && rows == "menuitem|7|Seven|-|-|-\nmenuitem|8|Eight|-|-|-",
                  "a callback may show another menu: it is the one in the document");
            js(item(7) + ".click()");
            check(ranOnce(inner, 7) && documentIsClean(), "and that menu's item runs its callback " + told(inner));

            const auto mine = std::make_shared<Call>();
            const auto theirs = std::make_shared<Call>();
            show(services, smallMenu(1, "One", "Two"), recording(mine));
            {
                WebServices other(kCanvas);
                other.dismissMenus();
                other.letGo();
                const bool still = services.menuOpen() && menusInDocument() == 1;
                show(other, smallMenu(7, "Seven", "Eight"), recording(theirs));
                check(still && menusInDocument() == 2 && other.menuOpen() && services.menuOpen(),
                      "another object's dismissMenus() and letGo() leave this one's menu alone, and each shows its "
                      "own");
                js("fg.stale = " + item(7));
            }
            js("fg.stale.click()");
            check(theirs->runs == 0 && released(theirs) && mine->runs == 0 && services.menuOpen()
                      && menusInDocument() == 1 && jsText("fg.rows()") == "menuitem|1|One|-|-|-\nmenuitem|2|Two|-|-|-",
                  "the destructor closes its object's menu, runs nothing and releases the callback; its item clicked "
                  "afterwards finds no object; the other menu stays " + told(theirs));
            js(item(1) + ".click()");
            check(ranOnce(mine, 1) && documentIsClean(), "which then runs its own callback " + told(mine));
        }

        // ---- 9. an item clicked while showMenu is still running (continues in afterInsideShow)
        page.call = std::make_shared<Call>();
        js("fg.clickInsideShow(1)");
        const bool shown = show(services, smallMenu(1, "One", "Two"), recording(page.call));
        check(shown && page.call->runs == 0 && services.menuOpen() && menusInDocument() == 1,
              "an item clicked from inside showMenu (the page's focus handling) has run nothing when showMenu "
              "returns, and the menu is open " + told(page.call));
        page.waitedMs = 0.0;
        emscripten_set_timeout(afterInsideShow, kPollMs, nullptr);
    }

    void afterInsideShow(void*)
    {
        if (page.call->runs == 0 && page.waitedMs < kWaitMs)
        {
            page.waitedMs += kPollMs;
            emscripten_set_timeout(afterInsideShow, kPollMs, nullptr);
            return;
        }
        check(ranOnce(page.call, 1) && !page.services->menuOpen() && documentIsClean(),
              "its callback runs later, once, from the page's timer " + told(page.call) + " after "
                  + std::to_string(static_cast<int>(page.waitedMs)) + " ms");

        // ---- 6, 10. the destructor, and what is left when the last object has gone
        const auto call = std::make_shared<Call>();
        show(*page.services, smallMenu(1, "One", "Two"), recording(call));
        js("fg.stale = " + item(1));
        const bool faceHeld = js("Module['funkguiWebMenus'].face ? 1 : 0") == 1;
        page.services.reset();
        js("fg.stale.click(); fg.key(document.body, 'Escape'); fg.press(document.body); "
           "window.dispatchEvent(new Event('resize'))");
        check(call->runs == 0 && released(call) && documentIsClean(),
              "the destructor closes the menu, runs nothing and releases the callback; its item and the window's "
              "events afterwards reach nothing " + told(call));
        const std::string left = jsText("const s = Module['funkguiWebMenus']; s.menus.size + ' menus, ' + s.users + "
                                        "' users, face ' + (s.face ? 'held' : 'gone')");
        check(faceHeld && left == "0 menus, 0 users, face gone",
              "with the last object the module has dropped the face (" + left + ")");
        clipboard();
    }

    // ---- 11. the clipboard ------------------------------------------------------------------------------------------
    void clipboard()
    {
        page.services = std::make_unique<WebServices>(kCanvas);
        page.copied = "FunkGui \xE2\x88\x92" "12.5 dB \xC2\xB5s \"quoted\" and 'apostrophes'\nsecond line";
        if (js("fg.watchClipboard()") == 0)
        {
            say("note     this browser has no navigator.clipboard.writeText here: only the fallback is checked");
            fallbackAndPreferences();
            return;
        }
        const bool issued = page.services->copyText(page.copied);
        check(issued && js("fg.writes.length") == 1 && js("fg.writes[0] === " + quoted(page.copied)) == 1,
              "copyText() returns true and has called navigator.clipboard.writeText once with the text, UTF-8 intact");
        check(documentIsClean(), "and has put nothing in the document");
        page.waitedMs = 0.0;
        emscripten_set_timeout(afterWrite, kPollMs, nullptr);
    }

    void afterWrite(void*)
    {
        const std::string outcome = jsText("fg.outcome");
        if (outcome == "pending" && page.waitedMs < kWaitMs)
        {
            page.waitedMs += kPollMs;
            emscripten_set_timeout(afterWrite, kPollMs, nullptr);
            return;
        }
        // Not judged: a script's click is not a user's gesture, and a headless window may not have the focus.
        say("note     the browser's answer to that write, made outside a user's gesture: %s", outcome.c_str());
        if (outcome != "fulfilled")
        {
            fallbackAndPreferences();
            return;
        }
        js("fg.read()");
        page.waitedMs = 0.0;
        emscripten_set_timeout(afterRead, kPollMs, nullptr);
    }

    void afterRead(void*)
    {
        const std::string read = jsText("fg.readBack");
        if (read == "pending" && page.waitedMs < kWaitMs)
        {
            page.waitedMs += kPollMs;
            emscripten_set_timeout(afterRead, kPollMs, nullptr);
            return;
        }
        if (read.rfind("text:", 0) == 0)
            check(read.substr(5) == page.copied, "the clipboard, read back, holds the text that was copied");
        else
            say("note     the page may not read the clipboard back here: %s", read.c_str());
        fallbackAndPreferences();
    }

    // ---- 11 (the fallback) and 12 -----------------------------------------------------------------------------------
    void fallbackAndPreferences()
    {
        {
            js("fg.canvas().focus()");
            const bool hidden = js("fg.hideClipboard(true)") == 0;
            const bool copied = page.services->copyText(page.copied);
            const std::string exec = jsText("fg.execs.length === 1 ? fg.execs[0].command + ' ' + fg.execs[0].result : "
                                            "fg.execs.length + ' calls'");
            check(hidden && js("fg.execs.length") == 1 && js("fg.execs[0].selected === " + quoted(page.copied)) == 1
                      && exec == (copied ? "copy true" : "copy false"),
                  std::string("without navigator.clipboard, copyText() copies through document.execCommand on a "
                              "textarea that holds the text selected, and returns what the browser returned (")
                      + exec + ")");
            check(documentIsClean() && js("document.activeElement === fg.canvas()") == 1,
                  "the textarea has left the document and the focus is where it was");
            if (!copied)
                say("note     execCommand('copy') is refused outside a user's gesture here, so copyText() said false");
            check(js("fg.hideClipboard(false)") == 1, "(navigator.clipboard is back)");
        }

        auto& prefs = UiPreferences::get();
        const std::string zoomKey = std::string(kPrefix) + "uiZoom", themeKey = std::string(kPrefix) + "theme";
        const auto stored = [](const std::string& key) { return jsText("fg.stored(" + quoted(key) + ")"); };
        js("fg.clearStored('fg.'); fg.clearStored('FunkGui.servicesPage')");
        {
            const uint32_t rev = prefs.revision();
            funkgui::installLocalStoragePrefs(kPrefix);
            check(prefs.revision() == rev + 1 && prefs.theme() == 0 && prefs.getInt("uiZoom", 100, 100, 175) == 100,
                  "installLocalStoragePrefs() makes localStorage the store: an empty one is the defaults");
            prefs.setInt("uiZoom", 150);
            prefs.setTheme(1);
            check(stored(zoomKey) == "150" && stored(themeKey) == "1" && stored("uiZoom") == "(none)",
                  "a preference is written through to localStorage as decimal text under <prefix><key> ("
                      + zoomKey + " = " + stored(zoomKey) + ", " + themeKey + " = " + stored(themeKey) + ")");
            funkgui::installLocalStoragePrefs(kPrefix);      // the page loaded again: a new backend, the same storage
            check(prefs.theme() == 1 && prefs.getInt("uiZoom", 100, 100, 175) == 150,
                  "a new backend over the same storage reads both back");
        }
        {
            // Another tab of the same origin: a changed key, a removed key, and a key of another product.
            js("localStorage.setItem(" + quoted(zoomKey) + ", '125'); localStorage.removeItem(" + quoted(themeKey)
               + "); localStorage.setItem('fg.other.uiZoom', '175')");
            const uint32_t rev = prefs.revision();
            const int before = prefs.getInt("uiZoom", 100, 100, 175);
            prefs.reload();
            check(before == 150 && prefs.revision() == rev + 1 && prefs.getInt("uiZoom", 100, 100, 175) == 125
                      && prefs.theme() == 0,
                  "what another tab stored is not seen until reload(), which then follows it and bumps revision() "
                  "once");
            prefs.reload();
            check(prefs.revision() == rev + 1, "a reload() that finds nothing new bumps nothing");
        }
        {
            // A private window: setItem throws.
            js("fg.refuseWrites(true)");
            prefs.setInt("uiZoom", 175);
            prefs.setTheme(1);
            const uint32_t rev = prefs.revision();
            prefs.reload();
            check(stored(zoomKey) == "125" && stored(themeKey) == "(none)"
                      && prefs.getInt("uiZoom", 100, 100, 175) == 175 && prefs.theme() == 1 && prefs.revision() == rev,
                  "a write the storage refuses is kept for the page, and survives reload() (storage still "
                      + stored(zoomKey) + ", the page has " + std::to_string(prefs.getInt("uiZoom", 100, 100, 175))
                      + ")");
            js("fg.refuseWrites(false)");
            prefs.setInt("uiZoom", 100);
            check(stored(zoomKey) == "100", "once the storage takes writes again the next one is stored");
        }
        {
            // A browser that forbids storage: reading window.localStorage throws.
            js("fg.forbidStorage(true)");
            const uint32_t rev = prefs.revision();
            prefs.reload();
            const bool kept = prefs.revision() == rev && prefs.getInt("uiZoom", 150, 100, 175) == 100;
            funkgui::installLocalStoragePrefs(kPrefix);
            prefs.setInt("uiZoom", 125);
            prefs.reload();
            const bool memory = prefs.theme() == 0 && prefs.getInt("uiZoom", 100, 100, 175) == 125;
            js("fg.forbidStorage(false)");
            check(kept && memory && stored(zoomKey) == "100",
                  "where the browser forbids storage a reload() loses nothing held, and a new backend is the defaults "
                  "and keeps what is set for the page; nothing throws");
        }
        {
            funkgui::installLocalStoragePrefs(nullptr);
            prefs.setInt("servicesPageProbe", 7);
            check(stored("FunkGui.servicesPageProbe") == "7",
                  "a null prefix is the product's: FUNKGUI_PREFS_FOLDER and a dot (FunkGui.servicesPageProbe = "
                      + stored("FunkGui.servicesPageProbe") + ")");
            js("fg.clearStored('fg.'); fg.clearStored('FunkGui.servicesPage')");
            prefs.setBackend(nullptr);
        }

        page.services.reset();
        check(documentIsClean(), "at the end the document is as it started");
        verdict();
        byHand();
    }

    // ---- after the verdict: a real gesture ------------------------------------------------------------------------
    void byHandClick(double x, double y)
    {
        MenuRequest r;
        r.items = { { 1, "Copy this line" }, { 2, "Paper", true, page.byHandTheme == 1 }, separator(),
                    { 3, "Disabled", false }, { 4, "Close" } };
        r.anchor = Rect{ static_cast<float>(x), static_cast<float>(y), 0.0f, 0.0f };
        r.theme = Theme::byIndex(page.byHandTheme);
        const float logicalW = static_cast<float>(jsNumber("fg.canvas().getBoundingClientRect().width"));
        page.byHand->showMenu(r, [](int id) {
            say("by hand  the menu gave %d", id);
            if (id == 2)
                page.byHandTheme = 1 - page.byHandTheme;
            if (id != 1)
                return;
            const std::string text = "FunkGui services page, copy " + std::to_string(++page.byHandCopies);
            const bool issued = page.byHand->copyText(text);
            say("by hand  copyText(\"%s\") returned %s", text.c_str(), issued ? "true" : "false");
        }, logicalW);
    }

    void byHand()
    {
        page.byHand = std::make_unique<WebServices>(kCanvas);
        js("fg.onOutcome = () => { const line = 'by hand  the browser says the write was ' + fg.outcome; "
           "console.log(line); document.getElementById('funkgui-log').textContent += line + '\\n'; }");
        fg_page_by_hand(byHandClick);
        say("by hand  click the canvas for a menu at the pointer; its Copy item writes the clipboard inside the click");
    }
}

int main()
{
    start();
    return 0;
}
