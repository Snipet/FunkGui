// FUNKGUI_TEST name=fg.gallery.services timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section services"
//
// The "services" gallery section (Web Sprint B, v0.12.0; FCompressor ADR-93, docs/sprints/web-b.md "G-B"): what a
// Panel asks its host for through HostServices, with no JUCE of its own. Five cells and four lines that say what the
// host answered:
//
//   MENU       HostServices::showMenu under the cell: First, Second, a separator, a disabled item, and a label with
//              U+2014 and U+00B7 (UTF-8 through the host). The item chosen last is ticked (Second at first).
//   OPEN       chooseFiles, Mode::open, "*.txt"
//   OPEN MANY  chooseFiles, Mode::openMany, every file
//   SAVE       chooseFiles, Mode::save, "*.txt", suggested name "FunkGui: gallery?.txt" (the host drops ':' and '?';
//              a path typed without ".txt" comes back with it)
//   COPY       copyText: "FunkGui gallery copy <n>: µ — ü" (the text is not drawn: paste it somewhere)
//
//   HOST       services() as words, and commandKeyIsMeta() as CMD or CTRL
//   MENU       OPEN while the menu waits, then CHOSE <id> or DISMISSED; REFUSED when showMenu returned false
//   FILES      OPEN while the chooser waits, then the count and the file names, or CANCELLED; REFUSED likewise
//   COPY       WROTE <n> or REFUSED
//
// A cell whose service the host does not report is drawn in ink32 and does nothing. A popup click on MENU and the
// a11y showMenu action open the menu too. The callbacks hold a weak token of the section, as a product's sub-view
// does: the gallery Panel can replace its section while the host stays.
//
// Live (tools/GalleryApp: `FunkGuiGalleryApp --section services`): EditorHost serves all three over JUCE. Headless
// (this test): HeadlessHost logs the calls and each state's script answers them, so the frames and a11y lines below pin
// the Panel's side. Every state sets the command key (the default is the platform's, and the rows are not). States:
// rest; ctrl (the command key is Ctrl); hover (MENU under the pointer); menu-open (clicked, unanswered); menu-chosen
// (First: the tick moves); menu-dismissed; open (one file); open-many (three); open-cancelled; save (a path typed
// without the extension); copy. The line above registers the test; the tools glob compiles this file into
// FunkGuiGalleryProbe and FunkGuiGalleryApp.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/LineEdit.h>
#include <funkgui/text/TextFit.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        enum CellIndex { kMenu, kOpen, kOpenMany, kSave, kCopy, kCellCount };

        struct Cell
        {
            Rect        r;
            const char* label;
            const char* spoken;
            unsigned    service;
        };

        constexpr Cell kCell[kCellCount] = {
            { { 16.0f, 16.0f, 56.0f, 20.0f }, "MENU", "Menu", hostservice::menus },
            { { 80.0f, 16.0f, 56.0f, 20.0f }, "OPEN", "Open a file", hostservice::fileChooser },
            { { 144.0f, 16.0f, 96.0f, 20.0f }, "OPEN MANY", "Open files", hostservice::fileChooser },
            { { 248.0f, 16.0f, 56.0f, 20.0f }, "SAVE", "Save a file", hostservice::fileChooser },
            { { 312.0f, 16.0f, 56.0f, 20.0f }, "COPY", "Copy text", hostservice::clipboard },
        };

        constexpr float    kLeft = 16.0f, kValueX = 64.0f, kLineY = 52.0f, kLineStep = 16.0f, kLineW = 400.0f;
        constexpr uint32_t kIdCells = 100, kIdLines = 900;
        constexpr const char* kLineTitle[] = { "HOST", "MENU", "FILES", "COPY" };
        constexpr int      kThirdId = 4;

        // The file name of a path, as text the atlas draws.
        std::string fileName(const std::string& path)
        {
            const size_t slash = path.find_last_of("/\\");
            return text::printable(slash == std::string::npos ? std::string_view(path)
                                                              : std::string_view(path).substr(slash + 1));
        }

        class ServicesSection final : public Section
        {
        public:
            ServicesSection() : Section(480, 124) {}

            void tick(float) override { dirty_ = false; }
            bool wantsFullRate() const override { return dirty_; }

            void pointerMove(const PointerEvent& e) override
            {
                hovered_ = cellAt({ e.x, e.y });
                dirty_ = true;
            }

            void pointerExit() override
            {
                hovered_ = -1;
                dirty_ = true;
            }

            void pointerDown(const PointerEvent& e) override
            {
                const int cell = cellAt({ e.x, e.y });
                if (cell == kMenu || (cell >= 0 && !e.popup))
                    activate(cell);                  // a popup click opens the menu too, and does nothing elsewhere
            }

            Cursor cursor() const override
            {
                return hovered_ >= 0 && offered(hovered_) ? Cursor::pointingHand : Cursor::normal;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                for (int i = 0; i < kCellCount; ++i)
                {
                    A11yItem b;
                    b.id = kIdCells + static_cast<uint32_t>(i);
                    b.role = A11yRole::button;
                    b.bounds = kCell[i].r;
                    b.title = kCell[i].label;
                    b.description = kCell[i].spoken;
                    b.enabled = offered(i);
                    out.push_back(b);
                }
                for (int i = 0; i < 4; ++i)
                {
                    A11yItem t;
                    t.id = kIdLines + static_cast<uint32_t>(i);
                    t.role = A11yRole::staticText;
                    t.bounds = { kLeft, lineY(i), kLineW + kValueX - kLeft, 12.0f };
                    t.title = kLineTitle[i];
                    t.value = line(i);
                    t.readOnly = true;
                    out.push_back(t);
                }
            }

            uint32_t a11yRevision() const override { return 1; }

            void a11yAction(uint32_t id, A11yAction a, double) override
            {
                const int cell = static_cast<int>(id) - static_cast<int>(kIdCells);
                if (cell < 0 || cell >= kCellCount)
                    return;
                if (a == A11yAction::press || (a == A11yAction::showMenu && cell == kMenu))
                    activate(cell);
            }

            void draw(Canvas& c, const Theme& th) override
            {
                for (int i = 0; i < kCellCount; ++i)
                {
                    const Canvas::Scope s(c, tags::cell, false);
                    const Rect& r = kCell[i].r;
                    const bool on = offered(i), hot = on && hovered_ == i;
                    c.rrect(r.x, r.y, r.w, r.h, 3.0f, th.ink16.withAlpha(0.0f), 1.0f, hot ? th.ink70 : th.ink32);
                    c.text(kCell[i].label, r.centreX(), c.capCentreTop(r.centreY(), type::kCaption), type::kCaption,
                           !on ? th.ink32 : (hot ? th.ink100 : th.ink70), Align::centre);
                }
                const Canvas::Scope s(c, tags::hint, false);
                for (int i = 0; i < 4; ++i)
                {
                    c.text(kLineTitle[i], kLeft, lineY(i), type::kMicro, th.ink32);
                    char fitted[256];
                    text::fitEllipsis(FontService::get().atlas(), line(i).c_str(), type::kMicro, kLineW, fitted,
                                      sizeof fitted);
                    c.text(fitted, kValueX, lineY(i), type::kMicro, th.ink70);
                }
            }

        private:
            static float lineY(int i) { return kLineY + kLineStep * static_cast<float>(i); }

            static int cellAt(Point p)
            {
                for (int i = 0; i < kCellCount; ++i)
                    if (kCell[i].r.contains(p))
                        return i;
                return -1;
            }

            bool offered(int cell) const
            {
                return host() != nullptr && (host()->services() & kCell[cell].service) != 0u;
            }

            void activate(int cell)
            {
                if (cell < 0 || !offered(cell))
                    return;
                switch (cell)
                {
                    case kMenu:     showMenu(); break;
                    case kOpen:     choose(FileRequest::Mode::open, "OPEN", "Open a text file", "*.txt", ""); break;
                    case kOpenMany: choose(FileRequest::Mode::openMany, "OPEN MANY", "Open files", "", ""); break;
                    case kSave:
                        choose(FileRequest::Mode::save, "SAVE", "Save a text file", "*.txt", "FunkGui: gallery?.txt");
                        break;
                    case kCopy:     copy(); break;
                    default:        break;
                }
                changed();
            }

            void showMenu()
            {
                MenuRequest m;
                m.items.push_back({ 1, "First", true, ticked_ == 1 });
                m.items.push_back({ 2, "Second", true, ticked_ == 2 });
                m.items.push_back({ .separator = true });
                m.items.push_back({ 3, "Disabled", false });
                m.items.push_back({ kThirdId, "Third \xE2\x80\x94 UTF-8 \xC2\xB7 label", true, ticked_ == kThirdId });
                m.anchor = kCell[kMenu].r;
                m.theme = Theme::byIndex(host()->themeIndex());
                const std::weak_ptr<int> alive = alive_;
                const bool taken = host()->showMenu(m, [this, alive](int id) {
                    if (alive.expired())
                        return;
                    if (id > 0)
                        ticked_ = id;
                    menuLine_ = id > 0 ? "CHOSE " + std::to_string(id) : std::string("DISMISSED");
                    changed();
                });
                menuLine_ = taken ? "OPEN" : "REFUSED";
            }

            void choose(FileRequest::Mode mode, const char* what, const char* title, const char* pattern,
                        const char* suggested)
            {
                FileRequest r;
                r.mode = mode;
                r.title = title;
                r.pattern = pattern;
                r.suggestedName = suggested;
                const std::weak_ptr<int> alive = alive_;
                const std::string name = what;
                const bool taken = host()->chooseFiles(r, [this, alive, name](const std::vector<std::string>& paths) {
                    if (alive.expired())
                        return;
                    filesLine_ = name + (paths.empty() ? ": CANCELLED" : ": " + std::to_string(paths.size()));
                    for (const std::string& p : paths)
                        filesLine_ += "   " + fileName(p);
                    changed();
                });
                filesLine_ = name + (taken ? ": OPEN" : ": REFUSED");
            }

            void copy()
            {
                ++copies_;
                const std::string clip = "FunkGui gallery copy " + std::to_string(copies_)
                                         + ": \xC2\xB5 \xE2\x80\x94 \xC3\xBC";
                copyLine_ = host()->copyText(clip) ? "WROTE " + std::to_string(copies_) : std::string("REFUSED");
            }

            // A frame, soon: EditorHost idles between input events, and a callback is not one.
            void changed()
            {
                dirty_ = true;
                if (host() != nullptr)
                    host()->nudgeFullRate();
            }

            std::string line(int i) const
            {
                switch (i)
                {
                    case 0:
                    {
                        if (host() == nullptr)
                            return "-";
                        const unsigned s = host()->services();
                        std::string words;
                        if ((s & hostservice::menus) != 0u)       words += "MENUS   ";
                        if ((s & hostservice::fileChooser) != 0u) words += "FILE CHOOSER   ";
                        if ((s & hostservice::clipboard) != 0u)   words += "CLIPBOARD   ";
                        if (words.empty())                        words = "NO SERVICES   ";
                        return words + "COMMAND KEY " + (host()->commandKeyIsMeta() ? "CMD" : "CTRL");
                    }
                    case 1:  return menuLine_ + "   TICK ON " + std::to_string(ticked_);
                    case 2:  return filesLine_;
                    default: return copyLine_;
                }
            }

            std::string menuLine_ = "-", filesLine_ = "-", copyLine_ = "-";
            int  ticked_ = 2;                        // the menu's ticked item: the one chosen last
            int  copies_ = 0;
            int  hovered_ = -1;
            bool dirty_ = false;
            std::shared_ptr<int> alive_ = std::make_shared<int>(0);   // the callbacks check it: the section exists
        };

        // Cell centres: MENU 44, OPEN 108, OPEN MANY 192, SAVE 276, COPY 340; the row's centre line is y 26.
        constexpr float kY = 26.0f;

        const Registration kServices{ SectionInfo{
            "services",
            [] { return std::make_unique<ServicesSection>(); },
            {
                State{ "rest", [](HeadlessHost& h, Panel&) { h.setCommandKeyIsMeta(true); } },
                State{ "ctrl", [](HeadlessHost& h, Panel&) { h.setCommandKeyIsMeta(false); } },
                State{ "hover", [](HeadlessHost& h, Panel&) {
                          h.setCommandKeyIsMeta(true);
                          h.move(44.0f, kY);
                      } },
                State{ "menu-open", [](HeadlessHost& h, Panel&) {
                          h.setCommandKeyIsMeta(true);
                          h.click(44.0f, kY);
                      } },
                State{ "menu-chosen", [](HeadlessHost& h, Panel&) {
                          h.setCommandKeyIsMeta(true);
                          h.click(44.0f, kY);
                          h.chooseMenuItem("First");
                      } },
                State{ "menu-dismissed", [](HeadlessHost& h, Panel&) {
                          h.setCommandKeyIsMeta(true);
                          h.click(44.0f, kY);
                          h.cancelMenu();
                      } },
                State{ "open", [](HeadlessHost& h, Panel&) {
                          h.setCommandKeyIsMeta(true);
                          h.click(108.0f, kY);
                          h.returnFiles({ "/tmp/notes.txt" });
                      } },
                State{ "open-many", [](HeadlessHost& h, Panel&) {
                          h.setCommandKeyIsMeta(true);
                          h.click(192.0f, kY);
                          h.returnFiles({ "/tmp/a.txt", "/tmp/sub/b.md", "/tmp/c" });
                      } },
                State{ "open-cancelled", [](HeadlessHost& h, Panel&) {
                          h.setCommandKeyIsMeta(true);
                          h.click(108.0f, kY);
                          h.cancelFiles();
                      } },
                State{ "save", [](HeadlessHost& h, Panel&) {
                          h.setCommandKeyIsMeta(true);
                          h.click(276.0f, kY);
                          h.returnFiles({ "/tmp/out" });
                      } },
                State{ "copy", [](HeadlessHost& h, Panel&) {
                          h.setCommandKeyIsMeta(true);
                          h.click(340.0f, kY);
                      } },
            } } };
    }
}
