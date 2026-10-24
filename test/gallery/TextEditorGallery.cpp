// FUNKGUI_TEST name=fg.gallery.texteditor timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section texteditor"
//
// The "texteditor" gallery section (widgets/TextEditor.h, text/Highlighter.h): two editors the way a product shows a
// script field and a notes field.
//   - CODE: EditorFont::code with line numbers, a KeywordHighlighter over KeywordRules::cLike() and the default
//     palette, over a ten-line C-like sample that overflows its box (so the scrollbar shows). A click focuses it: the
//     focus ring, the caret, the caret row's band and its line number in ink70.
//   - NOTES: EditorFont::prose, no gutter, word wrap, a placeholder, over a sentence that wraps into three rows.
//   - A footer in kLabel names the focused editor and its caret (1-based line and column) and selection.
// States: rest; focus (a click in CODE on line 5, column 11); typed (a click at the end of line 3, then "int n = 3;"
// highlighted as it is typed); select (a drag over "powf(10.0f, db / 20.0f)" on line 6); selectall (Cmd-A); keys
// (Down, End, Shift-Alt-Left from line 5: the last word selected by the keys); scroll (two notches of the wheel down
// over CODE); undo (as typed, then Cmd-Z); prose (a click at the end of NOTES, then " Then mix." wrapping); empty
// (NOTES cleared: the placeholder). The line above registers the test; the tools glob compiles this file into
// FunkGuiGalleryProbe, and the gallery app and the web gallery take it by theirs.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/Highlighter.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/TextEditor.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        constexpr Rect  kCode{ 16.0f, 12.0f, 300.0f, 150.0f }, kNotes{ 16.0f, 174.0f, 300.0f, 70.0f };
        constexpr float kFooterX = 332.0f, kFooterY = 20.0f, kFooterStep = 16.0f;
        constexpr uint32_t kIdCode = 910, kIdNotes = 911, kIdFooter = 912;

        constexpr const char* kSample =
            "// Gain stage\n"
            "#include \"dsp.h\"\n"
            "\n"
            "float gain(float x, float db)\n"
            "{\n"
            "    const float g = powf(10.0f, db / 20.0f);   /* linear */\n"
            "    if (x > 1.0f)\n"
            "        return 1.0f;\n"
            "    return x * g;\n"
            "}";

        constexpr const char* kNote =
            "Warm bus patch: a slow attack, a little drive on the return and the high shelf two dB down.";

        class TextEditorSection final : public Section
        {
        public:
            TextEditorSection() : Section(480, 256)
            {
                code_.setBounds(kCode);
                code_.setFont(EditorFont::code);
                code_.setHighlighter(std::make_unique<text::KeywordHighlighter>(text::KeywordRules::cLike()));
                code_.setPalette(TextEditor::defaultPalette());
                TextEditorOptions co = code_.options();
                co.a11yTitle = "CODE";
                code_.setOptions(co);
                code_.setText(kSample);

                notes_.setBounds(kNotes);
                TextEditorOptions no;
                no.style = editorFont(EditorFont::prose);
                no.lineNumbers = false;
                no.wordWrap = true;
                no.placeholder = "Notes";
                no.a11yTitle = "NOTES";
                notes_.setOptions(no);
                notes_.setText(kNote);
            }

            // A point a click lands on to put the caret at p in the code or the notes editor.
            Point at(bool code, TextPos p)
            {
                TextEditor& e = code ? code_ : notes_;
                const TextPos c = e.caret(), a = e.anchor();
                e.setCaret(p);
                const Point q = e.caretPoint();
                e.select(a, c);
                return { q.x + 1.0f, q.y + e.rowHeight() * 0.5f };
            }

            void clearNotes() { notes_.setText(""); }

            bool wantsFullRate() const override { return dirty_; }
            void tick(float dt) override
            {
                code_.tick(dt);
                notes_.tick(dt);
                dirty_ = false;
            }

            void pointerDown(const PointerEvent& e) override
            {
                const Point p{ e.x, e.y };
                if (code_.contains(p))
                    notes_.setFocused(false);
                else if (notes_.contains(p))
                    code_.setFocused(false);
                else
                {
                    code_.setFocused(false);
                    notes_.setFocused(false);
                }
                code_.pointerDown(e);
                notes_.pointerDown(e);
                dirty_ = true;
            }
            void pointerDrag(const PointerEvent& e) override
            {
                code_.pointerDrag(e);
                notes_.pointerDrag(e);
                dirty_ = true;
            }
            void pointerUp(const PointerEvent& e) override
            {
                code_.pointerUp(e);
                notes_.pointerUp(e);
            }
            void doubleClick(const PointerEvent& e) override
            {
                code_.doubleClick(e);
                notes_.doubleClick(e);
                dirty_ = true;
            }
            bool wheel(const WheelEvent& e) override
            {
                dirty_ = true;
                return code_.wheel(e) || notes_.wheel(e);
            }
            bool key(const KeyEvent& e) override
            {
                dirty_ = true;
                if (e.key == Key::escape)
                {
                    code_.setFocused(false);
                    notes_.setFocused(false);
                    return true;
                }
                return code_.key(e) || notes_.key(e);
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                A11yItem code;
                code_.accessibility(code);
                code.id = kIdCode;
                out.push_back(code);
                A11yItem notes;
                notes_.accessibility(notes);
                notes.id = kIdNotes;
                out.push_back(notes);
                A11yItem footer;
                footer.id = kIdFooter;
                footer.role = A11yRole::staticText;
                footer.bounds = { kFooterX, kFooterY - 8.0f, 132.0f, 48.0f };
                footer.title = "FOCUS";
                footer.value = status();
                footer.readOnly = true;
                out.push_back(footer);
            }

            void draw(Canvas& c, const Theme& th) override
            {
                code_.draw(c, th, code_.focused());
                notes_.draw(c, th, notes_.focused());
                const Canvas::Scope s(c, tags::hint, false);
                const auto& atlas = FontService::get().atlas();
                char fitted[96];
                text::fitEllipsis(atlas, status().c_str(), type::kLabel, 132.0f, fitted, sizeof fitted);
                c.text(fitted, kFooterX, kFooterY, type::kLabel, th.ink52);
                char line[64];
                std::snprintf(line, sizeof line, "CODE %d LINES", code_.lineCount());
                c.text(line, kFooterX, kFooterY + kFooterStep, type::kMicro, th.ink32);
                std::snprintf(line, sizeof line, "NOTES %d ROWS", notes_.rowCount());
                c.text(line, kFooterX, kFooterY + 2.0f * kFooterStep, type::kMicro, th.ink32);
            }

        private:
            std::string status() const
            {
                const TextEditor* e = code_.focused() ? &code_ : notes_.focused() ? &notes_ : nullptr;
                if (e == nullptr)
                    return "NO FOCUS";
                char buf[64];
                const int sel = e->hasSelection() ? static_cast<int>(e->selectedText().size()) : 0;
                std::snprintf(buf, sizeof buf, "%s L%d C%d%s%d", e == &code_ ? "CODE" : "NOTES",
                              e->caret().line + 1, e->caret().col + 1, sel > 0 ? " SEL " : "", sel);
                std::string s = buf;
                if (sel == 0)
                    s.resize(s.size() - 1);              // no "SEL": drop the trailing 0
                return s;
            }

            TextEditor code_{ kIdCode }, notes_{ kIdNotes };
            bool       dirty_ = false;
        };

        TextEditorSection& sec(Panel& p) { return static_cast<TextEditorSection&>(p); }

        void clickCode(HeadlessHost& h, Panel& p, TextPos at)
        {
            const Point q = sec(p).at(true, at);
            h.click(q.x, q.y);
        }

        const Registration kTextEditor{ SectionInfo{
            "texteditor",
            [] { return std::make_unique<TextEditorSection>(); },
            {
                State{ "rest", {} },
                State{ "focus", [](HeadlessHost& h, Panel& p) { clickCode(h, p, { 4, 10 }); } },
                State{ "typed",
                       [](HeadlessHost& h, Panel& p) {
                           clickCode(h, p, { 2, 0 });
                           h.keys("i,n,t,space,n,space,=,space,3,;");
                       } },
                State{ "select",
                       [](HeadlessHost& h, Panel& p) {
                           const Point a = sec(p).at(true, { 5, 20 }), b = sec(p).at(true, { 5, 43 });
                           h.drag(a.x, a.y, b.x, b.y, 6);
                       } },
                State{ "selectall",
                       [](HeadlessHost& h, Panel& p) {
                           clickCode(h, p, { 0, 3 });
                           h.keys("cmd+a");
                       } },
                State{ "keys",
                       [](HeadlessHost& h, Panel& p) {
                           h.setCommandKeyIsMeta(true);
                           clickCode(h, p, { 4, 0 });
                           h.keys("down,end,shift+alt+left");
                       } },
                State{ "scroll",
                       [](HeadlessHost& h, Panel&) {
                           h.wheel(kCode.centreX(), kCode.centreY(), -50.0f / 256.0f);
                           h.wheel(kCode.centreX(), kCode.centreY(), -50.0f / 256.0f);
                       } },
                State{ "undo",
                       [](HeadlessHost& h, Panel& p) {
                           clickCode(h, p, { 2, 0 });
                           h.keys("i,n,t,space,n,space,=,space,3,;,cmd+z");
                       } },
                State{ "prose",
                       [](HeadlessHost& h, Panel& p) {
                           const Point q = sec(p).at(false, { 0, 91 });
                           h.click(q.x, q.y);
                           h.keys("space,T,h,e,n,space,m,i,x,.");
                       } },
                State{ "empty",
                       [](HeadlessHost& h, Panel& p) {
                           sec(p).clearNotes();
                           const Point q = sec(p).at(false, { 0, 0 });
                           h.click(q.x, q.y);
                       } },
            } } };
    }
}
