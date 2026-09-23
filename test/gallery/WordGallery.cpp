// FUNKGUI_TEST name=fg.gallery.word timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section word"
//
// The "word" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §5.5, §6.4, §8.1; F §3.5; K1 #23; G5):
// AttachedWord in each of its states, on the slots FCompressor gives words (AUTO on MAKEUP, EXT on DETECT, LISTEN on
// SC HPF), with the slot routing a Panel does (the word's hit is checked first, then the slot's; Tab visits a slot,
// then its word):
//   MAKEUP + AUTO at rest · MAKEUP + AUTO on (sub AUTO +4.1) · MAKEUP whose AUTO is n/a (hidden: the slot takes the
//   whole hit and its tag stays in the label row) · MAKEUP locked with AUTO locked (disabled, ink16; the FIXED tag
//   moves to the sub line) · DETECT + EXT disabled (no side-chain bus) · SC HPF + LISTEN with an extension tag, which
//   moves to the detent line and leaves no room for the labels (ticks only)
// plus a footer: the word's reason when it is disabled, else the spec line. States: rest, hover (ink70), pressed
// (armed, accent), toggled (a click commits on up), dragoff (down, drag off, up: nothing), refused (a click on a
// disabled word: the reason), keys (Tab to the first word, Return toggles it; focus ring). The line above registers
// the test; the tools glob compiles this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/widgets/AttachedWord.h>
#include <funkgui/widgets/RuleSlider.h>
#include <funkgui/widgets/ValueModel.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        class WordPort final : public ParamPort
        {
        public:
            WordPort(const char* id, float v) : id_(id), v_(v) {}
            float value01() const override { return v_; }
            float default01() const override { return 0.0f; }
            int   numSteps() const override { return 0x7fffffff; }
            void  beginGesture() override {}
            void  setValue01(float v) override { v_ = v; }
            void  endGesture() override {}
            const char* id() const override { return id_; }
            void* native() const override { return nullptr; }

        private:
            const char* id_;
            float       v_;
        };

        struct SlotDef
        {
            SlotGeom    geom;
            ValueState  state = ValueState::continuous;
            const char* label = "";
            const char* tag = nullptr;
            const char* reason = nullptr;
            const char* sub = nullptr;
            std::vector<Detent> detents;
            float       init = 0.5f;
            // the word
            const char* word = nullptr;
            bool        wordOn = false, wordEnabled = true, wordVisible = true;
            const char* wordReason = nullptr;
        };

        // MAKEUP (bipolar -24..+24 dB), DETECT / SC HPF stepped, or a locked MAKEUP.
        class SlotModel final : public ValueModel
        {
        public:
            SlotModel(const SlotDef& d, WordPort& p) : d_(d), port_(p) {}

            uint64_t key() const override { return std::bit_cast<uint32_t>(port_.value01()); }

            void view(ValueView& v) const override
            {
                const float h = port_.value01();
                v.state = d_.state;
                v.label = d_.label;
                v.tag = d_.tag;
                v.reason = d_.reason;
                if (d_.sub != nullptr)
                    std::snprintf(v.text.sub, sizeof v.text.sub, "%s", d_.sub);
                if (d_.state == ValueState::stepped)
                {
                    v.nDetents = static_cast<int>(d_.detents.size());
                    v.detents = d_.detents.data();
                    const int n = v.nDetents;
                    int i = static_cast<int>(std::lround(h * static_cast<float>(n - 1)));
                    i = i < 0 ? 0 : (i > n - 1 ? n - 1 : i);
                    v.detent = i;
                    v.track = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
                    std::snprintf(v.text.value, sizeof v.text.value, "%s", d_.detents[static_cast<size_t>(i)].label);
                    return;
                }
                v.bipolar = true;
                v.track = h;
                v.trackDefault = 0.5f;
                v.atDefault = ease::sameBits(h, 0.5f);
                const float db = -24.0f + 48.0f * h;
                std::snprintf(v.text.value, sizeof v.text.value, "%s%.1f", db > 0.0f ? "+" : "", static_cast<double>(db));
                std::snprintf(v.text.unit, sizeof v.text.unit, "DB");
            }

            ParamPort* port() override { return &port_; }
            float host01FromTrack(float t) const override { return t; }
            float defaultHost01() const override { return 0.5f; }

        private:
            const SlotDef& d_;
            WordPort&      port_;
        };

        class Toggle final : public WordModel
        {
        public:
            Toggle(const SlotDef& d, WordPort& p) : d_(d), port_(p) {}
            bool on() const override { return port_.value01() > 0.5f; }
            bool enabled() const override { return d_.wordEnabled; }
            const char* reason() const override { return d_.wordReason; }
            void set(bool v, GestureController& g) override { g.tap(port_, v ? 1.0f : 0.0f); }
            ParamPort* port() override { return &port_; }
            bool visible() const override { return d_.wordVisible; }

        private:
            const SlotDef& d_;
            WordPort&      port_;
        };

        struct Entry
        {
            Entry(const SlotDef& d, uint32_t id)
                : def(d), port(d.label, d.init), wordPort(d.word != nullptr ? d.word : "", d.wordOn ? 1.0f : 0.0f),
                  model(def, port), toggle(def, wordPort), slider(model, d.geom, id)
            {
                if (d.word != nullptr)
                {
                    word = std::make_unique<AttachedWord>(toggle, d.geom, d.word, id + 1);
                    slider.setWord(word.get());
                }
            }
            SlotDef    def;
            WordPort   port, wordPort;
            SlotModel  model;
            Toggle     toggle;
            RuleSlider slider;
            std::unique_ptr<AttachedWord> word;
        };

        constexpr float kCol = 128.0f, kLeft = 16.0f, kW = 112.0f, kRowP = 24.0f, kRowS = 120.0f, kFooter = 200.0f;

        SlotGeom at(int col, float top, SlotSize size = SlotSize::primary)
        {
            return { kLeft + kCol * static_cast<float>(col), top, kW, size };
        }

        std::vector<Detent> steps(std::initializer_list<const char*> names)
        {
            std::vector<Detent> d;
            const float n = static_cast<float>(names.size());
            float i = 0.0f;
            for (const char* name : names)
            {
                d.push_back({ i / (n - 1.0f), name, name });
                i += 1.0f;
            }
            return d;
        }

        std::vector<SlotDef> slots()
        {
            std::vector<SlotDef> v;
            SlotDef d;
            d = {}; d.geom = at(0, kRowP); d.label = "MAKEUP"; d.init = 0.5f; d.word = "AUTO";
            v.push_back(d);
            d = {}; d.geom = at(1, kRowP); d.label = "MAKEUP"; d.init = 0.625f; d.word = "AUTO"; d.wordOn = true;
            d.sub = "AUTO +4.1";
            v.push_back(d);
            d = {}; d.geom = at(2, kRowP); d.label = "MAKEUP"; d.tag = "+"; d.init = 0.5f; d.word = "AUTO";
            d.wordVisible = false;                       // automu is n/a in this Mode: hidden
            v.push_back(d);
            d = {}; d.geom = at(3, kRowP); d.state = ValueState::locked; d.label = "MAKEUP"; d.tag = "FIXED";
            d.reason = "OUTPUT GAIN IS FIXED IN THIS MODE"; d.sub = "UNITY"; d.word = "AUTO"; d.wordEnabled = false;
            d.wordReason = "AUTO MAKEUP IS FIXED IN THIS MODE";
            v.push_back(d);
            d = {}; d.geom = at(0, kRowS, SlotSize::secondary); d.state = ValueState::stepped; d.label = "DETECT";
            d.detents = steps({ "PEAK", "RMS", "PK+RMS" }); d.init = 0.5f; d.word = "EXT"; d.wordEnabled = false;
            d.wordReason = "NO SIDECHAIN BUS CONNECTED";
            v.push_back(d);
            d = {}; d.geom = at(1, kRowS, SlotSize::secondary); d.state = ValueState::stepped; d.label = "SC HPF";
            d.tag = "+"; d.detents = steps({ "OFF", "50", "100", "200", "350" }); d.init = 0.5f; d.word = "LISTEN";
            v.push_back(d);
            return v;
        }

        class WordSection final : public Section
        {
        public:
            WordSection() : Section(528, 224)
            {
                uint32_t id = 200;
                for (const SlotDef& d : slots())
                {
                    entries_.push_back(std::make_unique<Entry>(d, id));
                    id += 2;
                }
                for (size_t i = 0; i < entries_.size(); ++i)
                {
                    stops_.push_back({ static_cast<int>(i), false });
                    if (entries_[i]->word != nullptr && entries_[i]->word->visible())
                        stops_.push_back({ static_cast<int>(i), true });
                }
            }

            void tick(float dt) override
            {
                for (size_t i = 0; i < entries_.size(); ++i)
                {
                    Entry& e = *entries_[i];
                    const bool sliderFocus = ring_ && focus_ >= 0 && stops_[static_cast<size_t>(focus_)].entry == static_cast<int>(i)
                                          && !stops_[static_cast<size_t>(focus_)].word;
                    e.slider.tick(dt, hasPointer_ && e.slider.contains(pointer_), sliderFocus, false);
                    if (e.word)
                        e.word->tick(dt, hasPointer_ && e.word->visible() && e.word->contains(pointer_));
                }
                dirty_ = false;
            }

            bool wantsFullRate() const override
            {
                if (dirty_)
                    return true;
                for (const auto& e : entries_)
                    if (!e->slider.settled() || (e->word && !e->word->settled()))
                        return true;
                return false;
            }

            void pointerMove(const PointerEvent& e) override
            {
                pointer_ = { e.x, e.y };
                hasPointer_ = true;
                for (auto& en : entries_)
                {
                    if (en->slider.contains(pointer_))
                        en->slider.pointerMove(e);
                    else
                        en->slider.pointerExit();
                }
                dirty_ = true;
            }

            void pointerExit() override
            {
                hasPointer_ = false;
                for (auto& en : entries_)
                    en->slider.pointerExit();
                dirty_ = true;
            }

            void pointerDown(const PointerEvent& e) override
            {
                ring_ = false;
                pressed_ = hit({ e.x, e.y }, pressedWord_);
                if (pressed_ >= 0)
                {
                    touched_ = pressed_;
                    touchedWord_ = pressedWord_;
                    Entry& en = *entries_[static_cast<size_t>(pressed_)];
                    if (pressedWord_)
                        en.word->pointerDown(e, *gestures());
                    else
                        en.slider.pointerDown(e, *gestures(), *host());
                }
                dirty_ = true;
            }

            void pointerDrag(const PointerEvent& e) override
            {
                pointer_ = { e.x, e.y };
                if (pressed_ >= 0)
                {
                    Entry& en = *entries_[static_cast<size_t>(pressed_)];
                    if (pressedWord_)
                        en.word->pointerDrag(e);
                    else
                        en.slider.pointerDrag(e, *gestures());
                }
                dirty_ = true;
            }

            void pointerUp(const PointerEvent& e) override
            {
                if (pressed_ >= 0)
                {
                    Entry& en = *entries_[static_cast<size_t>(pressed_)];
                    if (pressedWord_)
                        en.word->pointerUp(e, *gestures());
                    else
                        en.slider.pointerUp(e, *gestures());
                }
                pressed_ = -1;
                dirty_ = true;
            }

            bool key(const KeyEvent& e) override
            {
                dirty_ = true;
                const int n = static_cast<int>(stops_.size());
                if (e.key == Key::tab)
                {
                    const int dir = e.mods.shift ? -1 : 1;
                    focus_ = focus_ < 0 ? (dir > 0 ? 0 : n - 1) : (focus_ + dir + n) % n;
                    ring_ = true;
                    touched_ = stops_[static_cast<size_t>(focus_)].entry;
                    touchedWord_ = stops_[static_cast<size_t>(focus_)].word;
                    return true;
                }
                if (focus_ < 0)
                    return false;
                const Stop& s = stops_[static_cast<size_t>(focus_)];
                Entry& en = *entries_[static_cast<size_t>(s.entry)];
                return s.word ? en.word->key(e, *gestures()) : en.slider.key(e, *gestures());
            }

            Cursor cursor() const override
            {
                bool word = false;
                const int i = hasPointer_ ? hit(pointer_, word) : -1;
                if (i < 0)
                    return Cursor::normal;
                const Entry& en = *entries_[static_cast<size_t>(i)];
                return word ? en.word->cursorAt(pointer_) : en.slider.cursorAt(pointer_);
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                for (const auto& e : entries_)
                {
                    A11yItem item;
                    e->slider.accessibility(item);
                    out.push_back(item);
                    if (e->word)
                        e->word->accessibility(out);
                }
            }

            void a11yAction(uint32_t id, A11yAction a, double value) override
            {
                for (auto& e : entries_)
                {
                    if (e->slider.a11yId() == id)
                        e->slider.a11yAction(a, value, *gestures());
                    else if (e->word)
                        e->word->a11yAction(id, a, *gestures());
                }
                dirty_ = true;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                for (size_t i = 0; i < entries_.size(); ++i)
                {
                    const Entry& e = *entries_[i];
                    const bool focused = ring_ && focus_ >= 0 && stops_[static_cast<size_t>(focus_)].entry == static_cast<int>(i);
                    const bool wordFocus = focused && stops_[static_cast<size_t>(focus_)].word;
                    e.slider.draw(c, th, focused && !wordFocus);
                    if (e.word)
                        e.word->draw(c, th, wordFocus);
                }
                drawFooter(c, th);
            }

        private:
            struct Stop
            {
                int  entry;
                bool word;
            };

            // The entry under p; `word` says whether its word (checked first) or its slot has the point.
            int hit(Point p, bool& word) const
            {
                for (size_t i = 0; i < entries_.size(); ++i)
                {
                    const Entry& e = *entries_[i];
                    if (e.word && e.word->visible() && e.word->contains(p))
                    {
                        word = true;
                        return static_cast<int>(i);
                    }
                    if (e.slider.contains(p))
                    {
                        word = false;
                        return static_cast<int>(i);
                    }
                }
                word = false;
                return -1;
            }

            void drawFooter(Canvas& c, const Theme& th) const
            {
                int src = pressed_;
                bool word = pressedWord_;
                if (src < 0 && hasPointer_)
                    src = hit(pointer_, word);
                if (src < 0)
                {
                    src = touched_;
                    word = touchedWord_;
                }
                if (src < 0)
                    return;
                const Entry& e = *entries_[static_cast<size_t>(src)];
                char line[256];
                if (word && e.word)
                {
                    const char* r = e.word->reason();
                    std::snprintf(line, sizeof line, "%s", r != nullptr ? r : (e.toggle.on() ? "AUTO ON   RETURN TOGGLES"
                                                                                               : "AUTO OFF   RETURN TOGGLES"));
                }
                else
                    e.slider.specLine(line, sizeof line);
                const Canvas::Scope s(c, tags::hint, false);
                c.text(line, kLeft, kFooter, type::kLabel, th.ink32);
            }

            std::vector<std::unique_ptr<Entry>> entries_;
            std::vector<Stop> stops_;
            Point pointer_{};
            bool  hasPointer_ = false, ring_ = false, dirty_ = false, pressedWord_ = false, touchedWord_ = false;
            int   pressed_ = -1, focus_ = -1, touched_ = -1;
        };

        // The first word's hit is {x+w-34, top-5, 38, 18}: its centre, and a point on the slot below it.
        constexpr float kWordX = kLeft + kW - 15.0f, kWordY = kRowP + 4.0f;

        const Registration kWord{ SectionInfo{
            "word",
            [] { return std::make_unique<WordSection>(); },
            {
                State{ "rest", {} },
                State{ "hover", [](HeadlessHost& h, Panel&) { h.move(kWordX, kWordY); } },
                State{ "pressed",
                       [](HeadlessHost&, Panel& p) {
                           PointerEvent e;
                           e.x = kWordX;
                           e.y = kWordY;
                           p.pointerMove(e);
                           p.pointerDown(e);
                       } },
                State{ "toggled", [](HeadlessHost& h, Panel&) { h.click(kWordX, kWordY); } },
                State{ "dragoff",
                       [](HeadlessHost& h, Panel&) { h.drag(kWordX, kWordY, kWordX - 60.0f, kWordY + 30.0f, 4); } },
                State{ "refused",
                       [](HeadlessHost& h, Panel&) { h.click(kLeft + kW - 10.0f, kRowS + 4.0f); } },
                State{ "keys", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,return"); } },
            } } };
    }
}
