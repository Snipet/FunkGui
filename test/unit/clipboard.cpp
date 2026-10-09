// FUNKGUI_TEST name=fg.clipboard timeout=120 gpu=0
//
// fg.clipboard (FSynth G1): HostServices::clipboardText(), the read beside copyText (v0.12.0), as a JUCE-free build
// sees it. Spec rows only; no JUCE, so it runs in every configuration.
//
// - default.*   a host written before the call (the pure virtuals only) compiles and reads an empty clipboard, and
//               services() is what it was: the read has no hostservice bit.
// - headless.*  HeadlessHost keeps one in-memory clipboard for both directions: setClipboard (the user copying
//               somewhere else) then clipboardText; copyText then clipboardText, with log.copies and log.lastCopy as
//               at v0.14.0; setClipboard is not a copy and leaves the log alone; UTF-8 and an embedded line break are
//               kept byte for byte; an empty text empties it; each host has its own.
// - panel.*     a Panel copies in one field and pastes in another through the host, as a product's views would.
// No test reads or writes the user's clipboard: EditorHost's side is one JUCE call (juce::SystemClipboard), tried by
// hand in a product's editor.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/LineEdit.h>

#include <cstdint>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::HeadlessHost;
using funkgui::HostServices;

namespace
{
    // A host as every host was before the read existed: the pure virtuals and nothing else.
    class PlainHost final : public HostServices
    {
    public:
        void   setUnboundedDrag(bool) override {}
        void   showParamMenu(funkgui::ParamPort&, float, float) override {}
        void   nudgeFullRate() override {}
        double nowSeconds() const override { return 0.0; }
        void   beginBatch() override {}
        void   endBatch() override {}
    };

    // Two text fields. The command key with 'c' copies the first field's text; with 'v' it pastes into the second.
    class TwoFields final : public funkgui::Panel
    {
    public:
        HostServices* host = nullptr;
        funkgui::text::LineEdit first, second;
        bool copied = false, pasted = false;

        void attach(HostServices& h) override { host = &h; }
        int  width() const override { return 160; }
        int  height() const override { return 60; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        bool key(const funkgui::KeyEvent& e) override
        {
            if (e.key != funkgui::Key::character || !e.mods.cmd)
                return false;
            if (e.ch == U'c')
                copied = host->copyText(first.buffer);
            else if (e.ch == U'v')
                pasted = second.paste(host->clipboardText(), 16);
            else
                return false;
            return true;
        }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.clipboard", "", argc, argv);
    namespace hs = funkgui::hostservice;

    // ---- the default: empty, and no service bit ---------------------------------------------------------------------
    {
        PlainHost plain;
        const HostServices& h = plain;
        P.eq("default.reads_empty", h.clipboardText().empty(), 1);
        P.eq("default.services_none", plain.services(), 0);
        P.eq("default.copy_refused", plain.copyText("x"), 0);
        P.eq("default.still_empty_after_copy", h.clipboardText().empty(), 1);
    }

    // ---- HeadlessHost: one clipboard, both directions ---------------------------------------------------------------
    {
        TwoFields panel;
        HeadlessHost host(panel);
        const HostServices& h = host;                    // as a Panel reaches it
        P.eq("headless.initially_empty", h.clipboardText().empty(), 1);
        P.eq("headless.services_unchanged", host.services(), hs::menus | hs::fileChooser | hs::clipboard);

        host.setClipboard("from another app");
        P.eq("headless.set_then_read", h.clipboardText() == "from another app", 1);
        P.eq("headless.set_is_not_a_copy", host.log.copies == 0 && host.log.lastCopy.empty(), 1);

        P.eq("headless.copy_taken", host.copyText("-12.5 dB"), 1);
        P.eq("headless.copy_then_read", h.clipboardText() == "-12.5 dB", 1);
        P.eq("headless.copy_logged", host.log.copies == 1 && host.log.lastCopy == "-12.5 dB", 1);

        host.setClipboard("later");
        P.eq("headless.set_replaces_copy", h.clipboardText() == "later", 1);
        P.eq("headless.log_keeps_last_copy", host.log.copies == 1 && host.log.lastCopy == "-12.5 dB", 1);

        // UTF-8, a tab and line breaks are kept as they are: making them printable is LineEdit::paste's.
        const std::string text = "na\xC3\xAFve\t\xE2\x80\x94 line 1\r\nline 2\n";
        host.setClipboard(text);
        P.eq("headless.utf8_kept", h.clipboardText() == text, 1);
        P.eq("headless.copy_utf8", host.copyText(text) && h.clipboardText() == text && host.log.lastCopy == text, 1);
        P.eq("headless.read_twice", h.clipboardText() == h.clipboardText(), 1);

        host.setClipboard("");
        P.eq("headless.set_empty", h.clipboardText().empty(), 1);
        host.copyText("x");
        host.copyText("");
        P.eq("headless.copy_empty", h.clipboardText().empty() && host.log.copies == 4 && host.log.lastCopy.empty(), 1);

        // Each host has its own clipboard: nothing is shared through the process, let alone the system.
        TwoFields otherPanel;
        HeadlessHost other(otherPanel);
        host.setClipboard("mine");
        P.eq("headless.per_host", other.clipboardText().empty() && host.clipboardText() == "mine", 1);
    }

    // ---- a Panel: copy in one field, paste in another ---------------------------------------------------------------
    {
        TwoFields panel;
        HeadlessHost host(panel);
        panel.first.set("Warm Pad", 16);
        host.keys("cmd+c");
        P.eq("panel.copied", panel.copied && host.log.lastCopy == "Warm Pad", 1);
        host.keys("cmd+v");
        P.eq("panel.pasted", panel.pasted && panel.second.buffer == "Warm Pad", 1);
        // What the user copied elsewhere, pasted at the caret.
        host.setClipboard(" 2");
        host.keys("cmd+v");
        P.eq("panel.pasted_from_elsewhere", panel.second.buffer == "Warm Pad 2", 1);
        // An empty clipboard pastes nothing.
        host.setClipboard("");
        panel.pasted = true;
        host.keys("cmd+v");
        P.eq("panel.empty_pastes_nothing", !panel.pasted && panel.second.buffer == "Warm Pad 2", 1);
    }

    return P.finish();
}
