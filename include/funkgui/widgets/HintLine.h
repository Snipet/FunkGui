#pragma once

// The first-run hint, then the persistent spec line (02 §5.8; HR BgfxEditor.cpp:1556-1586). There are no tooltips
// anywhere: the footer line says what is under the hand. The hint shows for `seconds`, and the first pointer move cuts
// what is left to 0.4 s (HR :1668). A mouse-down with no mouse-move ever seen (a touch screen or a tablet) switches to
// always-chrome, where every slot shows its hover chrome (HR :1488). Probes start without the hint (02 §3.7 rule 6).
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G6 (src/widgets/HintLine.cpp).

namespace funkgui
{
    class Canvas;
    struct Theme;

    class HintLine
    {
    public:
        explicit HintLine(const char* firstRun, float seconds = 6.0f);

        void tick(float dt);
        void pointerMoved();                     // cuts the remaining hint to 0.4 s (HR :1668)
        void noteDown();                         // mouseDown with no mouseMove ever -> always-chrome (HR :1488)
        void noteMove();
        void skip();                             // no hint at all (probes and captures, 02 §3.7 rule 6)
        bool active() const;
        bool wantsFullRate() const;
        bool alwaysChrome() const;
        void draw(Canvas&, const Theme&, float x, float y, float maxW, const char* spec) const;   // kLabel ink32

    private:
        // Private state: completed by the implementing card (G6); not part of the frozen API.
        const char* firstRun_;
        float       left_;
        float       alpha_ = 1.0f;
        bool        moved_ = false;
        bool        alwaysChrome_ = false;
    };
}
