#include "WebServices.h"

#include <funkgui/core/Col.h>
#include <funkgui/text/BundledFont.h>

#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>
#include <emscripten/eventloop.h>

#include <cstdint>
#include <string>
#include <utility>

// WebServices (v0.13.0; FCompressor ADR-93, web Sprint C; WebServices.h): what src/gpu/HostServicesJuce does with a
// juce::PopupMenu and JUCE's clipboard, in a page. The rules are kept the way that file keeps them: one request is
// pending per object, it carries a serial, and an event of the page runs the Panel's callback only when the serial it
// brings is still the open request's. Here that check is what stands between a stale element and a callback: a menu's
// item elements keep their click handlers after the menu has left the document, so an item of a replaced, dismissed or
// let-go menu that is clicked later (a page script still holds it) reports a serial nothing is open under.
//
// Choices where the card is silent:
//
// - JavaScript calls C++ with two numbers, the serial and the id, never with a pointer: the open request is looked up
//   among the live objects (a list through Impl), so an event that arrives after its object was destroyed finds
//   nothing. The serial counts over all objects, so it names one request.
// - The callback runs inside the page's event handler (the item's click, the key, the press outside), which is what
//   lets a callback that copies text still be inside the user's gesture. The menu has left the document by then, so a
//   callback may show another. Should an event ever arrive while showMenu is still running (a focus listener of the
//   page that clicks an item when the menu takes the focus), it is posted to a timer instead (HostServices' rule:
//   never from inside the call that took it).
// - The look is funkgui::MenuLook's on JUCE's LookAndFeel_V4, in CSS px: ground, ink100 text (a disabled item at half
//   alpha), the highlighted row ink16, the bundled face at 14 px; V4's metrics for a 14 px font (rows of 18 px, the
//   highlight inset by 1 px, text 18 px in, a separator of 10 px with a 1 px line at 0.3 alpha, a border of 2 px) and
//   its tick shape. The window's edge is JUCE's outline off macOS (the text colour at 0.6 alpha) and a shadow: a page
//   has no native window to set the menu off from a canvas of the same ground. Two things are not JUCE's. The card's
//   14 px is the CSS font size, the em; JUCE's 14 is the face's height (ascent plus descent, 1.32 em for this face),
//   and V4 draws a row's text no taller than the row over 1.3, so the native glyphs are about two thirds this size.
//   And V2's faint stripe on every third row of the background (which V4 inherits) is left out: MenuLook says ground.
// - Placement is JUCE's for a target area: left edges aligned, under the anchor when it fits there or there is more
//   room under than over, else over it; then kept inside the window (the document's client box), to whole CSS px.
// - Every element is styled inline from `all: initial`, so the page's style sheets do not reach the menu and nothing
//   is added to them. The face is a FontFace made from the embedded data when the first object is constructed and
//   dropped with the last; it is in document.fonts only while a menu is showing. Where it cannot be made or loaded
//   the family list falls through to the system's monospaced face.
// - Keys go to the menu while it is open (a listener on the window, capturing), as they go to a native menu's window:
//   Up, Down, Return and Space, Escape; no other key reaches the canvas. A press outside the menu dismisses it and
//   goes on to what it hit, as with JUCE. The focus returns to the canvas when the menu held it.
// - A selector that matches no element is a host with nowhere to show a menu: refused, like the other three.
// - copyText: navigator.clipboard.writeText, its promise's rejection swallowed (the outcome arrives too late to
//   report); where that API is missing (a page that is not a secure context), document.execCommand('copy') on a
//   textarea that is in the document only for the call.

// One event of a menu: the request's serial and the chosen id, or 0 for a dismissal.
typedef void (*FunkGuiWebMenuEvent)(unsigned serial, int id);

// (EM_JS defines a C symbol: file scope. A body passes through the C preprocessor: no apostrophe in a comment, and no
// empty string between apostrophes.)
EM_JS_DEPS(funkgui_web_services_deps, "$UTF8ToString,$getWasmTableEntry");

// The bundled face for this module's menus: `users` is +1 for an object constructed, -1 for one destroyed.
EM_JS(void, funkgui_web_menu_face, (const unsigned char* data, int size, int users), {
    const state = Module['funkguiWebMenus'] || (Module['funkguiWebMenus'] = { menus: new Map(), face: null, users: 0 });
    state.users += users;
    if (users > 0 && !state.face) {
        try {
            const face = new FontFace('funkgui-menu', HEAPU8.slice(data, data + size));
            face.load().then(function () {}, function () {});
            state.face = face;
        } catch (e) {
            state.face = null;
        }
    }
    if (state.users <= 0 && state.face) {
        try { document.fonts.delete(state.face); } catch (e) {}
        state.face = null;
    }
});

// A menu's root element, not yet in the document; 0 when the selector names no element. Colours are 0xRRGGBBAA.
EM_JS(int, funkgui_web_menu_begin, (unsigned serial, const char* selector, unsigned ground, unsigned ink,
                                    unsigned highlight, FunkGuiWebMenuEvent event), {
    const state = Module['funkguiWebMenus'] || (Module['funkguiWebMenus'] = { menus: new Map(), face: null, users: 0 });
    const where = UTF8ToString(selector);
    let canvas = null;
    try { canvas = document.querySelector(where); } catch (e) {}
    if (!(canvas instanceof Element)) return 0;

    const css = (c, f) => 'rgba(' + (c >>> 24) + ', ' + ((c >>> 16) & 255) + ', ' + ((c >>> 8) & 255) + ', '
                              + ((c & 255) / 255 * f) + ')';
    const root = document.createElement('div');
    root.setAttribute('role', 'menu');
    root.setAttribute('data-funkgui-menu', String(serial >>> 0));
    root.tabIndex = -1;
    const s = root.style;
    s.all = 'initial';
    s.display = 'block';
    s.position = 'fixed';
    s.left = '0px';
    s.top = '0px';
    s.zIndex = '2147483647';
    s.boxSizing = 'border-box';
    s.padding = '1px 0';
    s.border = '1px solid ' + css(ink, 0.6);
    s.boxShadow = '0 4px 14px rgba(0, 0, 0, 0.35)';
    s.backgroundColor = css(ground, 1);
    s.color = css(ink, 1);
    s.fontFamily = '"funkgui-menu", ui-monospace, Menlo, Consolas, monospace';
    s.fontSize = '14px';
    s.lineHeight = '16px';
    s.whiteSpace = 'nowrap';
    s.cursor = 'default';
    s.userSelect = 'none';
    s.webkitUserSelect = 'none';
    s.overflowX = 'hidden';
    s.overflowY = 'auto';
    s.outline = 'none';

    const menu = { root: root, selector: where, items: [], active: -1, abort: null,
                   lit: css(highlight, 1), dim: css(ink, 0.5), faint: css(ink, 0.3) };
    menu.report = (id) => getWasmTableEntry(event)(serial, id);
    menu.setActive = (index) => {
        if (index === menu.active) return;
        if (menu.active >= 0) menu.items[menu.active].el.style.backgroundColor = 'transparent';
        menu.active = index;
        if (index >= 0) {
            menu.items[index].el.style.backgroundColor = menu.lit;
            root.setAttribute('aria-activedescendant', menu.items[index].el.id);
        } else {
            root.removeAttribute('aria-activedescendant');
        }
    };
    state.menus.set(serial, menu);
    return 1;
});

// One row, top to bottom. flags: bit 0 a separator, bit 1 enabled, bit 2 ticked.
EM_JS(void, funkgui_web_menu_add, (unsigned serial, int flags, int id, const char* label), {
    const menu = Module['funkguiWebMenus'].menus.get(serial);
    if (!menu) return;
    const el = document.createElement('div');
    const s = el.style;
    s.all = 'initial';
    s.display = 'block';
    s.boxSizing = 'border-box';
    if (flags & 1) {
        el.setAttribute('role', 'separator');
        s.height = '1px';
        s.margin = '4px 5px 5px 5px';
        s.backgroundColor = menu.faint;
        menu.root.appendChild(el);
        return;
    }
    const enabled = (flags & 2) !== 0;
    const ticked = (flags & 4) !== 0;
    const index = menu.items.length;
    el.id = 'funkgui-menu-' + (serial >>> 0) + '-' + index;
    el.setAttribute('role', ticked ? 'menuitemcheckbox' : 'menuitem');
    if (ticked) el.setAttribute('aria-checked', 'true');
    if (!enabled) el.setAttribute('aria-disabled', 'true');
    el.setAttribute('data-id', String(id));
    el.textContent = UTF8ToString(label);
    s.position = 'relative';
    s.height = '18px';
    s.padding = '0 21px 0 17px';
    s.borderTop = '1px solid transparent';
    s.borderBottom = '1px solid transparent';
    s.backgroundClip = 'padding-box';
    s.font = 'inherit';
    s.color = enabled ? 'inherit' : menu.dim;
    s.whiteSpace = 'inherit';
    s.cursor = 'inherit';
    if (ticked) {
        const ns = 'http://www.w3.org/2000/svg';
        const tick = document.createElementNS(ns, 'svg');
        tick.setAttribute('viewBox', '0 0 7.3236 10');
        tick.setAttribute('aria-hidden', 'true');
        const shape = document.createElementNS(ns, 'path');
        shape.setAttribute('d', 'M0 4.6474L1.4104 3.9422L3.3815 7.8844L1.971 7.8844L5.9132 0L7.3236 0.7052L2.6763 10Z');
        shape.setAttribute('fill', 'currentColor');
        tick.appendChild(shape);
        const t = tick.style;
        t.position = 'absolute';
        t.left = '7.4px';
        t.top = '3.1px';
        t.width = '7.2px';
        t.height = '9.8px';
        t.pointerEvents = 'none';
        el.appendChild(tick);
    }
    el.addEventListener('click', (e) => {
        e.preventDefault();
        e.stopPropagation();
        if (enabled) menu.report(id);
    });
    el.addEventListener('pointermove', () => menu.setActive(enabled ? index : -1));
    menu.items.push({ el: el, id: id, enabled: enabled });
    menu.root.appendChild(el);
});

// Into the document beside the anchor (x, y and height in the Panel px of a canvas `logicalWidth` of them wide), with
// the listeners that choose and dismiss; the menu takes the keyboard focus.
EM_JS(void, funkgui_web_menu_show, (unsigned serial, double ax, double ay, double ah, double logicalWidth), {
    const state = Module['funkguiWebMenus'];
    const menu = state.menus.get(serial);
    if (!menu) return;
    const root = menu.root;
    let canvas = null;
    try { canvas = document.querySelector(menu.selector); } catch (e) {}
    const box = canvas ? canvas.getBoundingClientRect() : { left: 0, top: 0, width: 0 };
    const scale = logicalWidth > 0 && box.width > 0 ? box.width / logicalWidth : 1;
    const page = document.documentElement;
    const viewW = page.clientWidth || window.innerWidth;
    const viewH = page.clientHeight || window.innerHeight;

    root.style.maxHeight = Math.max(40, viewH - 24) + 'px';
    if (state.face) {
        try { document.fonts.add(state.face); } catch (e) {}
    }
    (document.body || page).appendChild(root);
    const w = root.offsetWidth;
    const h = root.offsetHeight;
    const top = box.top + ay * scale;
    const bottom = top + ah * scale;
    const under = viewH - bottom;
    let x = box.left + ax * scale;
    let y = (h < under - 30 || under >= top) ? bottom : top - h;
    x = Math.max(1, Math.min(viewW - (w + 6), x));
    y = Math.max(1, Math.min(viewH - (h + 6), y));
    root.style.left = Math.round(x) + 'px';
    root.style.top = Math.round(y) + 'px';

    menu.abort = new AbortController();
    const signal = menu.abort.signal;
    window.addEventListener('pointerdown', (e) => {
        if (!(e.target instanceof Node) || !root.contains(e.target)) menu.report(0);
    }, { capture: true, signal: signal });
    window.addEventListener('blur', (e) => {
        if (e.target === window) menu.report(0);
    }, { signal: signal });
    window.addEventListener('resize', () => menu.report(0), { signal: signal });
    window.addEventListener('keydown', (e) => {
        e.stopPropagation();
        const key = e.key;
        const count = menu.items.length;
        if (key === 'Escape') {
            e.preventDefault();
            menu.report(0);
        } else if (key === 'ArrowDown' || key === 'ArrowUp') {
            e.preventDefault();
            const down = key === 'ArrowDown';
            let i = menu.active >= 0 ? menu.active : (down ? count - 1 : 0);
            for (let tries = 0; tries < count; ++tries) {
                i = (i + (down ? 1 : count - 1)) % count;
                if (menu.items[i].enabled) {
                    menu.setActive(i);
                    menu.items[i].el.scrollIntoView({ block: 'nearest' });
                    break;
                }
            }
        } else if (key === 'Enter' || key === ' ') {
            e.preventDefault();
            if (menu.active >= 0) menu.report(menu.items[menu.active].id);
        } else if (key === 'Tab') {
            e.preventDefault();
        }
    }, { capture: true, signal: signal });
    root.addEventListener('pointerleave', () => menu.setActive(-1));
    root.addEventListener('contextmenu', (e) => e.preventDefault());
    root.focus({ preventScroll: true });
});

// Out of the document, with its listeners on the window; the focus goes back to the canvas when the menu held it.
EM_JS(void, funkgui_web_menu_close, (unsigned serial), {
    const state = Module['funkguiWebMenus'];
    const menu = state && state.menus.get(serial);
    if (!menu) return;
    state.menus.delete(serial);
    if (menu.abort) menu.abort.abort();
    const focused = menu.root.contains(document.activeElement);
    menu.root.remove();
    if (state.menus.size === 0 && state.face) {
        try { document.fonts.delete(state.face); } catch (e) {}
    }
    if (focused) {
        try {
            const canvas = document.querySelector(menu.selector);
            if (canvas && canvas.focus) canvas.focus({ preventScroll: true });
        } catch (e) {
        }
    }
});

// 1 when a write to the clipboard was issued.
EM_JS(int, funkgui_web_copy_text, (const char* text, int size), {
    const value = UTF8ToString(text, size, true);
    try {
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(value).then(function () {}, function () {});
            return 1;
        }
    } catch (e) {
    }
    let area = null;
    let copied = 0;
    const focused = document.activeElement;
    try {
        area = document.createElement('textarea');
        area.value = value;
        area.readOnly = true;
        area.setAttribute('aria-hidden', 'true');
        area.style.position = 'fixed';
        area.style.left = '-9999px';
        area.style.top = '0px';
        area.style.opacity = '0';
        (document.body || document.documentElement).appendChild(area);
        area.select();
        copied = document.execCommand('copy') ? 1 : 0;
    } catch (e) {
        copied = 0;
    }
    if (area) area.remove();
    try {
        if (focused && focused.focus) focused.focus({ preventScroll: true });
    } catch (e) {
    }
    return copied;
});

namespace funkgui::web
{
    namespace
    {
        // HostServices' rule: an item that is not a separator, and an id > 0 on each (HostServicesJuce's menuIsValid).
        bool menuIsValid(const MenuRequest& request)
        {
            bool anyItem = false;
            for (const MenuItem& it : request.items)
            {
                if (it.separator)
                    continue;
                if (it.id <= 0)
                    return false;
                anyItem = true;
            }
            return anyItem;
        }

        unsigned packed(Col c) noexcept
        {
            return (static_cast<unsigned>(c.r) << 24) | (static_cast<unsigned>(c.g) << 16)
                 | (static_cast<unsigned>(c.b) << 8) | static_cast<unsigned>(c.a);
        }
    }

    // The pending request (HostServicesJuce's Pending), and the object's place among the live ones.
    struct WebServices::Impl
    {
        std::string  selector;
        MenuCallback menu;                           // the open request's callback
        uint32_t     serial = 0;                     // the open request's serial
        bool         open = false;
        bool         asking = false;                 // showMenu is putting the menu in the document
        Impl*        next = nullptr;

        inline static Impl*    live = nullptr;       // every object alive, newest first
        inline static uint32_t lastSerial = 0;       // over all of them

        // The object whose open request has this serial; null when it was dismissed, replaced or let go since, or its
        // object is gone.
        static Impl* find(uint32_t serial) noexcept
        {
            for (Impl* p = live; p != nullptr; p = p->next)
                if (p->open && p->serial == serial)
                    return p;
            return nullptr;
        }

        // The page's event: the item's click, a key, a dismissal.
        static void onEvent(unsigned serial, int id)
        {
            Impl* const p = find(serial);
            if (p == nullptr)
                return;
            if (p->asking)
            {
                emscripten_set_timeout(&Impl::later, 0.0, new std::pair<uint32_t, int>(serial, id));
                return;
            }
            p->open = false;
            const MenuCallback callback = std::exchange(p->menu, nullptr);
            funkgui_web_menu_close(serial);          // out of the document first: the callback may show another
            if (callback)
                callback(id > 0 ? id : 0);           // nothing of *p after this: the callback may end the host
        }

        static void later(void* event)
        {
            const std::pair<uint32_t, int> e = *static_cast<std::pair<uint32_t, int>*>(event);
            delete static_cast<std::pair<uint32_t, int>*>(event);
            onEvent(e.first, e.second);
        }
    };

    WebServices::WebServices(const char* canvasSelector) : impl_(std::make_unique<Impl>())
    {
        impl_->selector = canvasSelector != nullptr ? canvasSelector : "";
        impl_->next = Impl::live;
        Impl::live = impl_.get();
        funkgui_web_menu_face(static_cast<const unsigned char*>(BundledFont::data()),
                              static_cast<int>(BundledFont::size()), 1);
    }

    WebServices::~WebServices()
    {
        letGo();
        for (Impl** p = &Impl::live; *p != nullptr; p = &(*p)->next)
        {
            if (*p == impl_.get())
            {
                *p = impl_->next;
                break;
            }
        }
        funkgui_web_menu_face(nullptr, 0, -1);
    }

    bool WebServices::showMenu(const MenuRequest& request, MenuCallback done, float logicalWidth)
    {
        dismissMenus();                              // a second menu replaces the first, even when it is refused
        if (!menuIsValid(request))
            return false;
        Impl& p = *impl_;
        const uint32_t serial = ++Impl::lastSerial;
        const Theme& th = request.theme;
        if (funkgui_web_menu_begin(serial, p.selector.c_str(), packed(th.ground), packed(th.ink100), packed(th.ink16),
                                   &Impl::onEvent) == 0)
            return false;                            // no canvas to show it beside
        for (const MenuItem& it : request.items)
        {
            const int flags = (it.separator ? 1 : 0) | (it.enabled ? 2 : 0) | (it.checked ? 4 : 0);
            funkgui_web_menu_add(serial, flags, it.id, it.label.c_str());
        }

        p.serial = serial;
        p.menu = std::move(done);
        p.open = true;
        p.asking = true;
        const Rect& r = request.anchor;
        funkgui_web_menu_show(serial, static_cast<double>(r.x), static_cast<double>(r.y), static_cast<double>(r.h),
                              static_cast<double>(logicalWidth));
        p.asking = false;
        return true;
    }

    void WebServices::dismissMenus()
    {
        Impl& p = *impl_;
        if (!p.open)
            return;
        p.open = false;
        p.menu = nullptr;                            // dropped unrun
        funkgui_web_menu_close(p.serial);
    }

    bool WebServices::copyText(std::string_view utf8)
    {
        const std::string text(utf8);
        return funkgui_web_copy_text(text.c_str(), static_cast<int>(text.size())) != 0;
    }

    void WebServices::letGo()
    {
        dismissMenus();
    }

    bool WebServices::menuOpen() const noexcept
    {
        return impl_->open;
    }
}
