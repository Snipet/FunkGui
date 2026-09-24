// FUNKGUI_TEST name=fg.gallery.lineedit timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section lineedit"
//
// The "lineedit" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §5.8, §5.11; HR PresetPanel.cpp:317-327,
// 386-421, 505-535, 945-975; G6): text::LineEdit drawn the way a preset strip and a browser search field draw it.
//   - The name field: at rest the committed name, made printable (text::printable) and fitted with text::fitEllipsis;
//     a click starts an edit with the name pre-filled and selected (an accentDim rectangle behind it), the caret is an
//     accent bar after the text before it (steady here: HR blinks it by alpha, which a settled frame cannot show),
//     Return commits a non-empty text and Escape cancels, as a product decides (LineEdit leaves both keys to it).
//   - The search field: a click focuses it; typing filters nothing here; empty, it shows SEARCH in ink32.
//   - A footer and three a11y items give the committed name, the edit buffer with its caret, and the search text.
// States: rest, edit (the pre-filled name selected), typed ("Keep" replaces the selection), commit ("Keep", Return),
// cancel ("x", Escape: the name is kept), caret (Left drops the selection to the start, then "X"), word (Right, then
// Alt-Backspace deletes "02"), line (Right, Cmd-Backspace), long (a name wider than the field: fitted with U+2026),
// printable (a name with characters the atlas cannot draw: shown as '?'), search (a click, then "bus"). The line above
// registers the test; the tools glob compiles this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/LineEdit.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/FocusRing.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        constexpr Rect  kName{ 16, 12, 300, 32 }, kSearch{ 16, 56, 200, 22 };
        constexpr float kFooterY = 96.0f, kPad = 6.0f;
        constexpr int   kMaxName = 40, kMaxSearch = 48;
        constexpr uint32_t kIdName = 900, kIdEdit = 901, kIdSearch = 902;

        enum class Status { none, editing, committed, cancelled };

        class LineEditSection final : public Section
        {
        public:
            LineEditSection() : Section(480, 120) {}

            void setName(const char* s) { name_ = text::printable(s); }

            bool wantsFullRate() const override { return dirty_; }
            void tick(float) override { dirty_ = false; }

            void pointerDown(const PointerEvent& e) override
            {
                const Point p{ e.x, e.y };
                searchFocus_ = false;
                if (kName.contains(p))
                {
                    if (!editing_)
                    {
                        edit_.set(name_, kMaxName);      // HR beginEdit: pre-filled and selected
                        editing_ = true;
                        status_ = Status::editing;
                    }
                }
                else
                {
                    if (editing_)
                        finish(false);                   // a click elsewhere cancels the edit
                    searchFocus_ = kSearch.contains(p);
                }
                dirty_ = true;
            }

            bool key(const KeyEvent& e) override
            {
                dirty_ = true;
                if (editing_)
                {
                    if (e.key == Key::enter)
                        finish(true);
                    else if (e.key == Key::escape)
                        finish(false);
                    else
                        edit_.key(e, kMaxName);
                    return true;                         // an edit owns the keyboard (HR)
                }
                if (searchFocus_)
                {
                    if (e.key == Key::enter || e.key == Key::escape)
                    {
                        searchFocus_ = false;
                        return true;
                    }
                    return search_.key(e, kMaxSearch);
                }
                return false;
            }

            Cursor cursor() const override { return Cursor::normal; }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                A11yItem name;
                name.id = kIdName;
                name.role = A11yRole::staticText;
                name.bounds = kName;
                name.title = "PRESET";
                name.value = name_;
                name.readOnly = true;
                out.push_back(name);

                A11yItem edit;
                edit.id = kIdEdit;
                edit.role = A11yRole::staticText;
                edit.bounds = kName;
                edit.title = "EDIT";
                edit.value = editing_ ? "[" + edit_.buffer + "] caret " + std::to_string(edit_.caret)
                                            + (edit_.allSelected ? " selected" : "")
                                      : statusText();
                edit.readOnly = true;
                out.push_back(edit);

                A11yItem search;
                search.id = kIdSearch;
                search.role = A11yRole::staticText;
                search.bounds = kSearch;
                search.title = "SEARCH";
                search.value = "[" + search_.buffer + "]" + (searchFocus_ ? " focused" : "");
                search.readOnly = true;
                out.push_back(search);
            }

            void draw(Canvas& c, const Theme& th) override
            {
                drawName(c, th);
                drawSearch(c, th);
                const Canvas::Scope s(c, tags::hint, false);
                char line[160];
                std::snprintf(line, sizeof line, "NAME %s   %s", name_.c_str(), statusText());
                char fitted[192];
                text::fitEllipsis(FontService::get().atlas(), line, type::kLabel, 448.0f, fitted, sizeof fitted);
                c.text(fitted, 16.0f, kFooterY, type::kLabel, th.ink32);
            }

        private:
            void finish(bool commit)
            {
                if (commit && !edit_.buffer.empty())
                {
                    name_ = edit_.buffer;
                    status_ = Status::committed;
                }
                else
                    status_ = Status::cancelled;
                editing_ = false;
            }

            const char* statusText() const
            {
                switch (status_)
                {
                    case Status::editing:   return "EDITING";
                    case Status::committed: return "COMMITTED";
                    case Status::cancelled: return "CANCELLED";
                    case Status::none:      break;
                }
                return "";
            }

            void drawName(Canvas& c, const Theme& th) const
            {
                const float x0 = kName.x + kPad, maxW = kName.w - 2.0f * kPad;
                const float top = c.capCentreTop(kName.centreY(), type::kLatch);
                c.hairlineH(kName.x, kName.bottom(), kName.w, th.ink16);
                if (!editing_)
                {
                    char fitted[128];
                    text::fitEllipsis(FontService::get().atlas(), name_.c_str(), type::kLatch, maxW, fitted,
                                      sizeof fitted);
                    c.text(fitted, x0, top, type::kLatch, th.ink100);
                    return;
                }
                const float w = c.textWidth(edit_.buffer.c_str(), type::kLatch);
                if (edit_.allSelected)
                    c.rrect(x0 - 2.0f, kName.centreY() - 9.0f, w + 4.0f, 18.0f, 1.0f, th.accentDim);   // HR :519-520
                c.text(edit_.buffer.c_str(), x0, top, type::kLatch, th.ink100);
                const std::string before = edit_.buffer.substr(0, static_cast<size_t>(edit_.caret));
                const float cx = x0 + c.textWidth(before.c_str(), type::kLatch);
                c.rrect(cx + 1.0f, kName.centreY() - 7.0f, 1.5f, 14.0f, 0.0f, th.accent);           // HR :522-525
            }

            void drawSearch(Canvas& c, const Theme& th) const
            {
                const float x0 = kSearch.x + kPad;
                const float top = c.capCentreTop(kSearch.centreY(), type::kLabel);
                c.rrect(kSearch.x, kSearch.y, kSearch.w, kSearch.h, 0.0f, th.ink16);
                if (search_.buffer.empty())
                    c.text("SEARCH", x0, top, type::kLabel, th.ink32);
                else
                    c.text(search_.buffer.c_str(), x0, top, type::kLabel, th.ink100);
                if (searchFocus_)
                {
                    const std::string before = search_.buffer.substr(0, static_cast<size_t>(search_.caret));
                    const float cx = x0 + c.textWidth(before.c_str(), type::kLabel);
                    c.rrect(cx + 1.0f, kSearch.centreY() - 6.0f, 1.5f, 12.0f, 0.0f, th.accent);
                    drawFocusRing(c, kSearch.expanded(2.0f), th.accent);
                }
            }

            std::string    name_ = "Drum Bus 02";
            text::LineEdit edit_, search_;
            bool           editing_ = false, searchFocus_ = false, dirty_ = false;
            Status         status_ = Status::none;
        };

        constexpr float kNameX = 120.0f, kNameY = 28.0f;

        void clickName(HeadlessHost& h) { h.click(kNameX, kNameY); }

        const Registration kLineEdit{ SectionInfo{
            "lineedit",
            [] { return std::make_unique<LineEditSection>(); },
            {
                State{ "rest", {} },
                State{ "edit", [](HeadlessHost& h, Panel&) { clickName(h); } },
                State{ "typed",
                       [](HeadlessHost& h, Panel&) {
                           clickName(h);
                           h.keys("K,e,e,p");
                       } },
                State{ "commit",
                       [](HeadlessHost& h, Panel&) {
                           clickName(h);
                           h.keys("K,e,e,p,return");
                       } },
                State{ "cancel",
                       [](HeadlessHost& h, Panel&) {
                           clickName(h);
                           h.keys("x,escape");
                       } },
                State{ "caret",
                       [](HeadlessHost& h, Panel&) {
                           clickName(h);
                           h.keys("left,X");
                       } },
                State{ "word",
                       [](HeadlessHost& h, Panel&) {
                           clickName(h);
                           h.keys("right,alt+backspace");
                       } },
                State{ "line",
                       [](HeadlessHost& h, Panel&) {
                           clickName(h);
                           h.keys("right,cmd+backspace");
                       } },
                State{ "long",
                       [](HeadlessHost&, Panel& p) {
                           static_cast<LineEditSection&>(p).setName(
                               "Mastering Chain For The Loudest Records Ever Pressed To Vinyl");
                       } },
                State{ "printable",
                       [](HeadlessHost&, Panel& p) {
                           static_cast<LineEditSection&>(p).setName("Caf\xC3\xA9 \xE2\x98\x83 Bus");
                       } },
                State{ "search",
                       [](HeadlessHost& h, Panel&) {
                           h.click(60.0f, 66.0f);
                           h.keys("b,u,s");
                       } },
            } } };
    }
}
