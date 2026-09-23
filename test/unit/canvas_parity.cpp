// FUNKGUI_TEST name=fg.canvas.parity timeout=600 gpu=0
//
// fg.canvas.parity (= fg.recorder.roundtrip, 02 §3.11): a frame recorded by funkgui::Canvas -> PrimList::writeText
// (dump v2, 02 §3.8) -> PrimList::parseText gives back the same primitives bit for bit, the same frame info, axes and
// missing glyphs, and the same fingerprint (02 §3.9) under every option; writing the parsed frame again gives the same
// bytes. Also: HR's v1 dump parses (a literal line of SdfCanvas::end()'s format, and the recorded frame written in
// that format, whose legacy fingerprint equals the in-memory frame's); malformed dumps are refused; the fingerprint's
// rules (live primitives and text gamma left out, colours and tags not hashed, the 1/1024 px quantum, tag counts,
// HR's Rank-cap rule under legacyHr); the tag-name registry; FontService (one bake; atlasHash() is FontProbe's
// font.atlas); and the recorder's own contract (begin() keeps capacity, Scope restores, textWidth == text::width,
// missing-glyph accounting, snapping, the dpi guard). Spec rows only.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::Fingerprint;
using funkgui::FingerprintOptions;
using funkgui::PrimList;
using funkgui::Theme;

namespace
{
    constexpr funkgui::Tag kCurve = 300, kDot = 301, kUnnamed = 999;

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    // ---- the scene: every HR primitive, tags, a live scope, missing glyphs ------------------------------------------
    void drawScene(Canvas& c, const Theme& th, bool withLive, float nudge = 0.0f)
    {
        c.rrect(16.0f + nudge, 16.0f, 40.0f, 24.0f, 0.0f, th.ink16);
        c.rrect(64.25f, 16.5f, 40.0f, 24.0f, 6.0f, th.ink32, 1.5f, th.accent);
        c.rrect4(112.1f, 16.2f, 48.3f, 24.4f, 0.0f, 8.0f, 2.0f, 12.0f, th.accentDim, 0.0f, {}, 2.5f);
        {
            const Canvas::Scope track(c, funkgui::tags::slotTrack, false);
            c.hairlineH(16.0f, 60.3f, 200.0f, th.ink16);
            c.hairlineV(230.7f, 56.0f, 40.0f, th.ink16);
        }
        {
            const Canvas::Scope curve(c, kCurve, false);
            const float xs[] = { 250.0f, 270.5f, 290.0f, 310.25f };
            const float ys[] = { 104.0f, 74.0f, 96.5f, 70.0f };
            for (int k = 0; k + 1 < 4; ++k)
                c.segment(xs[k], ys[k], xs[k + 1], ys[k + 1], 1.25f, th.ink100, 0.5f);
        }
        {
            const Canvas::Scope unnamed(c, kUnnamed, false);
            c.segment(10.0f, 200.0f, 90.0f, 180.0f, 2.0f, th.ink70);
        }
        {
            const Canvas::Scope labels(c, funkgui::tags::slotLabel, false);
            c.text("THRESHOLD -12.5 dB", 16.0f, 120.0f, funkgui::type::kLabel, th.ink52);
            c.text("4:1", 300.0f, 120.0f, funkgui::type::kValueP, th.ink100, funkgui::Align::centre);
            c.text("\xC2\xB0\xC2\xB1\xC3\x97\xE2\x80\x93\xE2\x86\x92\xE2\x88\x9E", 390.0f, 150.0f,
                   funkgui::type::kCaption, th.ink70, funkgui::Align::right);
        }
        if (withLive)
        {
            const Canvas::Scope live(c, kDot, true);
            c.rrect(400.0f + nudge * 7.0f, 40.0f, 8.0f, 8.0f, 4.0f, th.signal);
            c.text("LIVE 0.37", 400.0f, 60.0f, funkgui::type::kMicro, th.ink100);
        }
    }

    funkgui::FrameInfo frameFor(const Theme& th, int themeIdx, float dpi)
    {
        funkgui::FrameInfo f;
        f.logicalW = 480;
        f.logicalH = 300;
        f.dpi = dpi;
        f.clear = th.ground;
        f.textGamma = th.textGamma;
        f.theme = themeIdx;
        f.seconds = 1.01666677f;
        f.frame = 61;
        f.dt = 1.0f / 60.0f;
        f.fixedClock = true;
        f.fullRate = true;
        return f;
    }

    // ---- comparisons ------------------------------------------------------------------------------------------------
    bool sameInfo(const funkgui::FrameInfo& a, const funkgui::FrameInfo& b)
    {
        return a.logicalW == b.logicalW && a.logicalH == b.logicalH && sameBits(a.dpi, b.dpi)
            && a.clear.r == b.clear.r && a.clear.g == b.clear.g && a.clear.b == b.clear.b && a.clear.a == b.clear.a
            && sameBits(a.textGamma, b.textGamma) && a.theme == b.theme && sameBits(a.seconds, b.seconds)
            && a.frame == b.frame && sameBits(a.dt, b.dt) && a.fixedClock == b.fixedClock
            && a.displayLinked == b.displayLinked && sameBits(a.fps, b.fps) && a.fullRate == b.fullRate
            && a.overflows == b.overflows;
    }

    bool sameMap(const funkgui::AxisMap& x, const funkgui::AxisMap& y)
    {
        return sameBits(x.px0, y.px0) && sameBits(x.px1, y.px1) && sameBits(x.v0, y.v0) && sameBits(x.v1, y.v1)
            && x.log == y.log;
    }

    bool sameList(const PrimList& a, const PrimList& b)
    {
        if (!sameInfo(a.info, b.info) || a.prims.size() != b.prims.size() || a.axes.size() != b.axes.size()
            || a.missingGlyphs != b.missingGlyphs || std::memcmp(a.missingFirst, b.missingFirst, sizeof a.missingFirst))
            return false;
        for (size_t i = 0; i < a.prims.size(); ++i)
            if (std::memcmp(&a.prims[i], &b.prims[i], sizeof(funkgui::Prim)) != 0)
                return false;
        for (size_t i = 0; i < a.axes.size(); ++i)
            if (a.axes[i].tag != b.axes[i].tag || a.axes[i].hasX != b.axes[i].hasX || a.axes[i].hasY != b.axes[i].hasY
                || !sameMap(a.axes[i].x, b.axes[i].x) || !sameMap(a.axes[i].y, b.axes[i].y))
                return false;
        return true;
    }

    bool sameFingerprint(const Fingerprint& a, const Fingerprint& b)
    {
        return a.geometry == b.geometry && a.text == b.text && a.statics == b.statics && a.live == b.live
            && a.texts == b.texts && a.rrects == b.rrects && a.segments == b.segments && a.areas == b.areas
            && sameBits(a.maxX, b.maxX) && sameBits(a.maxY, b.maxY) && a.tagCounts == b.tagCounts;
    }

    // writeText into memory.
    std::string dump(const PrimList& l, bool* ok = nullptr)
    {
        char* buf = nullptr;
        size_t len = 0;
        std::FILE* f = open_memstream(&buf, &len);
        const bool written = f != nullptr && l.writeText(f);
        const bool closed = f != nullptr && std::fclose(f) == 0;
        std::string s = buf != nullptr ? std::string(buf, len) : std::string();
        std::free(buf);
        if (ok != nullptr)
            *ok = written && closed;
        return s;
    }

    // The frame in HR's v1 format, exactly as the snapshot's SdfCanvas::end() prints it (gpu/SdfCanvas.cpp:290-319).
    std::string dumpV1(const PrimList& l)
    {
        std::string s;
        char line[512];
        std::snprintf(line, sizeof line, "clear %02x%02x%02x\n", l.info.clear.r, l.info.clear.g, l.info.clear.b);
        s += line;
        std::snprintf(line, sizeof line, "view %g %g dpi %g clock %s fps %.1f rate %s\n",
                      static_cast<double>(l.info.logicalW), static_cast<double>(l.info.logicalH),
                      static_cast<double>(l.info.dpi), "displaylink", 59.9, "idle");
        s += line;
        for (const funkgui::Prim& p : l.prims)
        {
            std::snprintf(line, sizeof line,
                          "p %g %g %g %g  c0 %08x c1 %08x  d0 %g %g %g %g  e0 %g %g  d1 %g %g %g %g  d2 %g %g %g %g\n",
                          double(p.x0), double(p.y0), double(p.x1), double(p.y1), p.c0, p.c1, double(p.d0[0]),
                          double(p.d0[1]), double(p.d0[2]), double(p.d0[3]), double(p.e0[0]), double(p.e0[1]),
                          double(p.d1[0]), double(p.d1[1]), double(p.d1[2]), double(p.d1[3]), double(p.d2[0]),
                          double(p.d2[1]), double(p.d2[2]), double(p.d2[3]));
            s += line;
        }
        return s;
    }

    bool refused(const char* text)
    {
        PrimList l;
        l.prims.resize(3);                               // parseText must also clear what was there
        return !PrimList::parseText(text, l) && l.prims.empty();
    }

    const char* kHeader = "funkgui-dump 2\nclear 16171a\nview 10 10 dpi 2\n";
    const char* kPrim = "p 1 2 3 4  c0 ffffffff c1 ffffffff  d0 1 2 3 4  e0 5 6  d1 0 0 0 0  d2 0 0 0 0";
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // the atlas bakes through JUCE's font stack
    T::Probe P("fg.canvas.parity", "", argc, argv);

    auto& fonts = funkgui::FontService::get();
    const funkgui::FontAtlasSdf& atlas = fonts.atlas();
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();

    // ---- FontService: one bake per process; atlasHash() is FontProbe's font.atlas (FNV-1a 64 of the R8 pixels) ------
    {
        const auto& px = atlas.pixels();
        const uint64_t probeHash = T::fnv1a(px.data(), px.size() * sizeof(px[0]));
        std::printf("font.atlas %016llx\n", static_cast<unsigned long long>(fonts.atlasHash()));
        P.eq("font.atlas_hash_is_fontprobes", fonts.atlasHash() == probeHash && probeHash != 0, 1);
        P.eq("font.one_bake", &funkgui::FontService::get() == &fonts && &fonts.atlas() == &atlas && atlas.baked()
                                  && atlas.usedEmbeddedFace(), 1);
    }

    // ---- tag names --------------------------------------------------------------------------------------------------
    {
        const funkgui::TagName product[] = { { kCurve, "OLD_NAME" }, { kDot, "OP_DOT" }, { 5, "NOT_MINE" },
                                             { 302, "bad name" }, { 303, "HINT" }, { 304, "OP_DOT" },
                                             { 305, nullptr } };
        funkgui::registerTagNames(product);
        const funkgui::TagName rename[] = { { kCurve, "TRANSFER_CURVE" } };
        funkgui::registerTagNames(rename);
        const auto is = [](const char* got, const char* want) { return got != nullptr && std::strcmp(got, want) == 0; };
        P.eq("tags.funkgui_names", is(funkgui::tagName(funkgui::tags::slotLabel), "SLOT_LABEL")
                                       && is(funkgui::tagName(funkgui::tags::hint), "HINT"), 1);
        P.eq("tags.product_names",
             is(funkgui::tagName(kCurve), "TRANSFER_CURVE") && is(funkgui::tagName(kDot), "OP_DOT"), 1);
        P.eq("tags.none_and_unknown", funkgui::tagName(0) == nullptr && funkgui::tagName(kUnnamed) == nullptr
                                          && funkgui::tagName(200) == nullptr && funkgui::tagName(302) == nullptr
                                          && funkgui::tagName(305) == nullptr, 1);
        P.eq("tags.taken_names_refused", funkgui::tagName(303) == nullptr && funkgui::tagName(304) == nullptr
                                             && funkgui::tagFromName("HINT") == funkgui::tags::hint, 1);
        P.eq("tags.funkgui_range_not_registrable", is(funkgui::tagName(5), "SLOT_CARET"), 1);
        P.eq("tags.from_name", funkgui::tagFromName("FOCUS_RING") == funkgui::tags::focusRing
                                   && funkgui::tagFromName("OP_DOT") == kDot
                                   && funkgui::tagFromName("TRANSFER_CURVE") == kCurve
                                   && funkgui::tagFromName("OLD_NAME") == 0 && funkgui::tagFromName("NOPE") == 0
                                   && funkgui::tagFromName("") == 0 && funkgui::tagFromName(nullptr) == 0, 1);
    }

    // ---- the recorder's contract ------------------------------------------------------------------------------------
    Canvas canvas(atlas);
    const Theme graphite = Theme::graphite();
    {
        canvas.begin(frameFor(graphite, 0, 0.0f));
        P.eq("canvas.dpi_guard", sameBits(canvas.dpi(), 1.0f), 1);
        canvas.begin(frameFor(graphite, 0, 2.0f));
        P.eq("canvas.snap", sameBits(canvas.snapY(10.3f), 10.5f) && sameBits(canvas.snapX(10.2f), 10.0f)
                                && sameBits(canvas.snapX(-0.3f), -0.5f), 1);

        P.eq("canvas.scope_restores", [&] {
            canvas.setTag(7);
            canvas.setLive(false);
            bool inner = false;
            {
                const Canvas::Scope a(canvas, kCurve, true);
                {
                    const Canvas::Scope b(canvas, funkgui::tags::hint, false);
                    inner = canvas.tag() == funkgui::tags::hint && !canvas.live();
                }
                inner = inner && canvas.tag() == kCurve && canvas.live();
            }
            const bool ok = inner && canvas.tag() == 7 && !canvas.live();
            canvas.setTag(0);
            return ok;
        }(), 1);

        bool widths = true;
        for (const char* s : { "THRESHOLD", "-12.5 dB", "0123456789", "\xC2\xB0 x \xE2\x88\x86", "", "  " })
            for (const auto& st : { funkgui::type::kLabel, funkgui::type::kDisplay, funkgui::type::kMicro })
                widths = widths && sameBits(canvas.textWidth(s, st), funkgui::text::width(atlas, s, st));
        P.eq("canvas.text_width_is_text_width", widths, 1);

        canvas.begin(frameFor(graphite, 0, 2.0f));
        canvas.text("A\xE2\x88\x86" "B\xE2\x86\xBA\xE2\x88\x86\xFF" "C\t", 0.0f, 0.0f, funkgui::type::kLabel,
                    graphite.ink100);
        const PrimList& m = canvas.end();
        P.eq("missing.count", canvas.missingGlyphs(), 5);
        P.eq("missing.list", m.missingFirst[0] == 0x2206u && m.missingFirst[1] == 0x21BAu
                                 && m.missingFirst[2] == 0xFFFDu && m.missingFirst[3] == 0x09u
                                 && m.missingFirst[4] == 0u, 1);
        P.eq("missing.glyphs_drawn", static_cast<int64_t>(m.prims.size()), 3);

        drawScene(canvas, graphite, true);
        const size_t cap = canvas.end().prims.capacity();
        canvas.begin(frameFor(graphite, 0, 2.0f));
        P.eq("canvas.begin_clears_keeps_capacity",
             canvas.end().prims.empty() && canvas.end().prims.capacity() == cap && canvas.missingGlyphs() == 0, 1);

        canvas.text("x", 0.0f, 0.0f, funkgui::type::kLabel, graphite.ink100);
        {
            const Canvas::Scope s(canvas, funkgui::tags::word, true);
            canvas.rrect(0.0f, 0.0f, 4.0f, 4.0f, 1.0f, graphite.ink100);
        }
        const auto& ps = canvas.end().prims;
        P.eq("canvas.emit_stamps", ps.size() == 2 && ps[0].tag == 0 && sameBits(ps[0].d2[3], 0.0f)
                                       && sameBits(ps[0].d2[2], 1.0f) && ps[1].tag == funkgui::tags::word
                                       && sameBits(ps[1].d2[3], 1.0f) && sameBits(ps[1].d2[2], 0.0f)
                                       && ps[1].reserved == 0, 1);
    }

    // ---- record -> write -> parse -> fingerprint, at two scales and both themes -------------------------------------
    const FingerprintOptions defaults, withLive{ false, true, 1.0f / 1024.0f, false },
        withGamma{ true, false, 1.0f / 1024.0f, false }, legacy{ true, true, 1.0f / 1024.0f, true };
    Fingerprint graphiteFp, paperFp, graphiteGammaFp, paperGammaFp;
    for (const int themeIdx : { 0, 1 })
        for (const float dpi : { 1.0f, 2.0f })
        {
            const Theme th = Theme::byIndex(themeIdx);
            const std::string key = std::string("roundtrip.theme") + std::to_string(themeIdx) + ".dpi"
                                  + std::to_string(static_cast<int>(dpi));
            funkgui::FrameInfo info = frameFor(th, themeIdx, dpi);
            info.overflows = themeIdx == 0 ? 3u : 0u;
            info.displayLinked = themeIdx == 1;
            info.fixedClock = themeIdx == 0;
            info.fps = themeIdx == 1 ? 59.9f : 0.0f;
            canvas.begin(info);
            drawScene(canvas, th, true);
            canvas.text("\xE2\x88\x86" "3", 10.0f, 280.0f, funkgui::type::kMicro, th.ink32);   // one missing glyph
            PrimList rec = canvas.end();
            funkgui::AxisRec ax;
            ax.tag = kCurve;
            ax.hasX = true;
            ax.x = { 250.0f, 310.25f, -42.0f, 6.0f, false };
            ax.hasY = true;
            ax.y = { 104.0f, 70.0f, 0.001f, 1000.0f, true };
            rec.axes.push_back(ax);
            funkgui::AxisRec half;
            half.tag = kUnnamed;
            half.hasY = true;
            half.y = { 1.0f, 2.0f, 3.0f, 4.0f, false };
            rec.axes.push_back(half);

            bool wrote = false;
            const std::string text = dump(rec, &wrote);
            PrimList back;
            const bool parsed = PrimList::parseText(text, back);
            P.eq(key + ".write", wrote, 1);
            P.eq(key + ".parse", parsed, 1);
            P.eq(key + ".bit_equal", parsed && sameList(rec, back), 1);
            bool fpEqual = true;
            for (const FingerprintOptions* o : { &defaults, &withLive, &withGamma, &legacy })
                fpEqual = fpEqual && sameFingerprint(funkgui::fingerprint(rec, *o), funkgui::fingerprint(back, *o));
            P.eq(key + ".fingerprint_equal", fpEqual, 1);
            P.eq(key + ".rewrite_identical", dump(back) == text, 1);
            P.eq(key + ".tags_spelled", text.find("  t TRANSFER_CURVE\n") != std::string::npos
                                            && text.find("  t 999\n") != std::string::npos
                                            && text.find("axis TRANSFER_CURVE x 250 310.25 -42 6 lin y 104 70 "
                                                         "0.00100000005 1000 log\n") != std::string::npos
                                            && text.find("axis 999 x - y 1 2 3 4 lin\n") != std::string::npos
                                            && text.find("glyphs missing 1 U+2206\n") != std::string::npos
                                            && (themeIdx != 0 || text.find("overflow 3\n") != std::string::npos), 1);
            if (dpi > 1.5f)
            {
                (themeIdx == 0 ? graphiteFp : paperFp) = funkgui::fingerprint(rec);
                (themeIdx == 0 ? graphiteGammaFp : paperGammaFp) = funkgui::fingerprint(rec, withGamma);
            }

            // HR's v1 of the same frame: parses, and the legacy fingerprint sees what HR's FrameRender saw.
            PrimList v1;
            P.eq(key + ".v1_parse", PrimList::parseText(dumpV1(rec), v1) && v1.prims.size() == rec.prims.size()
                                        && v1.info.displayLinked && !v1.info.fixedClock && !v1.info.fullRate
                                        && sameBits(v1.info.fps, 59.9f) && v1.info.logicalW == 480, 1);
            P.eq(key + ".legacy_fingerprint", sameFingerprint(funkgui::fingerprint(rec, legacy),
                                                               funkgui::fingerprint(v1, legacy)), 1);
        }

    // ---- fingerprint rules ------------------------------------------------------------------------------------------
    P.eq("fp.theme_invariant", sameFingerprint(graphiteFp, paperFp), 1);
    P.eq("fp.gamma_hashed_when_asked", graphiteGammaFp.text != paperGammaFp.text
                                           && graphiteGammaFp.geometry == paperGammaFp.geometry, 1);
    {
        const auto record = [&](bool withLiveElems, float nudge) {
            canvas.begin(frameFor(graphite, 0, 2.0f));
            drawScene(canvas, graphite, withLiveElems, nudge);
            return canvas.end();
        };
        const PrimList base = record(true, 0.0f);
        const Fingerprint fp = funkgui::fingerprint(base);
        const Fingerprint noLive = funkgui::fingerprint(record(false, 0.0f));
        // The live element is one rrect and the 8 glyphs of "LIVE 0.37".
        P.eq("fp.live_excluded", fp.geometry == noLive.geometry && fp.text == noLive.text && fp.live == 9
                                     && noLive.live == 0 && fp.statics == noLive.statics, 1);
        const Fingerprint liveIn = funkgui::fingerprint(base, withLive);
        P.eq("fp.live_included_when_asked", liveIn.geometry != fp.geometry && liveIn.text != fp.text
                                                && liveIn.statics == fp.statics + 9 && liveIn.live == 9, 1);
        P.eq("fp.counts", fp.rrects == 5 && fp.segments == 4 && fp.areas == 0 && fp.texts + 9 == fp.statics, 1);
        P.eq("fp.tag_counts", fp.tagCounts.size() == 4 && fp.tagCounts[0].first == funkgui::tags::slotLabel
                                  && fp.tagCounts[1].first == funkgui::tags::slotTrack
                                  && fp.tagCounts[1].second == 2 && fp.tagCounts[2].first == kCurve
                                  && fp.tagCounts[2].second == 3 && fp.tagCounts[3].first == kUnnamed, 1);
        P.eq("fp.quantum_absorbs_noise", sameFingerprint(fp, funkgui::fingerprint(record(true, 1.0e-5f))), 1);
        P.eq("fp.geometry_moves", funkgui::fingerprint(record(true, 0.01f)).geometry != fp.geometry, 1);

        PrimList recoloured = base;
        for (auto& p : recoloured.prims)
        {
            p.c0 ^= 0x00FF00FFu;
            p.c1 ^= 0xFF0000FFu;
            p.tag = static_cast<funkgui::Tag>(p.tag == 0 ? 0 : p.tag + 1);
        }
        const Fingerprint rc = funkgui::fingerprint(recoloured);
        P.eq("fp.colours_and_tags_not_hashed", rc.geometry == fp.geometry && rc.text == fp.text, 1);
        float maxX = 0.0f, maxY = 0.0f;
        for (const auto& p : base.prims)
            if (!sameBits(p.d2[3], 1.0f))
            {
                maxX = std::max(maxX, p.x1);
                maxY = std::max(maxY, p.y1);
            }
        P.eq("fp.extents", sameBits(fp.maxX, maxX) && sameBits(fp.maxY, maxY) && maxX > 300.0f, 1);

        // HR's Rank band under legacyHr: a 3 px bar inside y 165-295 is an "energy cap", skipped and counted.
        canvas.begin(frameFor(graphite, 0, 2.0f));
        canvas.rrect(100.0f, 200.0f, 20.0f, 3.0f, 0.0f, graphite.ink100);    // hh 1.5: a cap
        canvas.rrect(100.0f, 180.0f, 20.0f, 20.0f, 0.0f, graphite.ink100);   // hh 10: a stroke
        canvas.rrect(100.0f, 20.0f, 20.0f, 3.0f, 0.0f, graphite.ink100);     // outside the band
        const Fingerprint lg = funkgui::fingerprint(canvas.end(), legacy);
        P.eq("fp.legacy_rank_caps", lg.live == 1 && lg.statics == 2 && lg.text == 0, 1);
    }

    // ---- HR v1 literal and malformed input --------------------------------------------------------------------------
    {
        PrimList l;
        const bool ok = PrimList::parseText(
            "clear 16171a\r\nview 880 520 dpi 2 clock timer fps 0.0 rate full\r\n"
            "p 8.5 18.5 41.5 61.5  c0 04030201 c1 08070605  d0 -16.5 -21.5 15 20  e0 16.5 21.5  d1 5 5 5 5  "
            "d2 2 0 0 0\r\n", l);
        P.eq("v1.literal", ok && l.info.logicalW == 880 && l.info.logicalH == 520 && sameBits(l.info.dpi, 2.0f)
                               && !l.info.displayLinked && !l.info.fixedClock && l.info.fullRate
                               && l.info.clear.r == 0x16 && l.info.clear.b == 0x1a && l.info.clear.a == 255
                               && l.prims.size() == 1 && l.prims[0].c0 == 0x04030201u && l.prims[0].c1 == 0x08070605u
                               && sameBits(l.prims[0].x0, 8.5f) && sameBits(l.prims[0].e0[1], 21.5f)
                               && sameBits(l.prims[0].d2[0], 2.0f) && l.prims[0].tag == 0, 1);

        const std::string head = kHeader;
        const std::string ok2 = head + "glyphs missing 0\n" + kPrim + "  t NOT_A_KNOWN_NAME\n";
        PrimList u;
        P.eq("parse.unknown_tag_name_is_untagged", PrimList::parseText(ok2, u) && u.prims.size() == 1
                                                       && u.prims[0].tag == 0, 1);
        P.eq("parse.minimal_v2", PrimList::parseText(head, u) && u.prims.empty(), 1);

        const std::string bad[] = {
            "",                                                                       // nothing
            head.substr(0, head.find("view")),                                        // no view line
            "funkgui-dump 2\nview 10 10 dpi 2\n",                                      // no clear line
            "funkgui-dump 3\nclear 16171a\nview 10 10 dpi 2\n",                        // another version
            "clear 16171a\nview 10 10 dpi 2\nglyphs missing 0\n",                     // a v2 line in a v1 dump
            head + "view 10 10 dpi 2\n",                                              // repeated header line
            head + "bogus 1\n",                                                       // unknown keyword
            "funkgui-dump 2\nclear 16171a\nview 10 10\n",                              // view without dpi
            "funkgui-dump 2\nclear 16171a\nview 10 10 dpi 2 colour red\n",             // unknown view key
            "funkgui-dump 2\nclear 16171a\nview 10.5 10 dpi 2\n",                      // fractional size
            "funkgui-dump 2\nclear 16171g\nview 10 10 dpi 2\n",                        // bad hex
            "funkgui-dump 2\nclear 16171a\nview 10 10 dpi 2 clock quartz\n",           // bad clock
            head + "p 1 2 3  c0 ffffffff c1 ffffffff  d0 1 2 3 4  e0 5 6  d1 0 0 0 0  d2 0 0 0 0\n",   // 19 fields
            head + "p 1 2 3 4  c0 fffffffff c1 ffffffff  d0 1 2 3 4  e0 5 6  d1 0 0 0 0  d2 0 0 0 0\n",   // 9 digits
            head + "p 1 2 3 x  c0 ffffffff c1 ffffffff  d0 1 2 3 4  e0 5 6  d1 0 0 0 0  d2 0 0 0 0\n",   // bad float
            head + "p 1 2 3 4  c1 ffffffff c0 ffffffff  d0 1 2 3 4  e0 5 6  d1 0 0 0 0  d2 0 0 0 0\n",   // labels
            head + kPrim + "  t lower_case\n",                                        // bad tag name
            head + kPrim + "  t 70000\n",                                             // tag out of range
            head + kPrim + "  q X\n",                                                 // not a tag field
            head + "glyphs missing 1 U+0000\n",                                       // U+0 is not a codepoint
            head + "glyphs missing 9 U+0041 U+0042 U+0043 U+0044 U+0045 U+0046 U+0047 U+0048 U+0049\n",  // > 8 listed
            head + "glyphs missing -1\n",                                             // negative count
            head + "overflow x\n",                                                    // bad overflow
            head + "axis LEVEL x 1 2 3 4 cubic y -\n",                                 // bad mapping
            head + "axis LEVEL x 1 2 3 4 lin\n",                                      // missing y
            head + "axis LEVEL y - x -\n",                                            // wrong order
        };
        bool allRefused = true;
        for (size_t i = 0; i < std::size(bad); ++i)
            if (!refused(bad[i].c_str()))
            {
                allRefused = false;
                std::printf("  accepted malformed dump #%zu\n", i);
            }
        P.eq("parse.malformed_refused", allRefused, 1);

        PrimList c;
        c.prims.resize(5);
        c.axes.resize(2);
        c.missingGlyphs = 3;
        c.missingFirst[0] = 0x2206u;
        c.info.logicalW = 7;
        const size_t pc = c.prims.capacity();
        c.clear();
        P.eq("primlist.clear", c.prims.empty() && c.axes.empty() && c.missingGlyphs == 0 && c.missingFirst[0] == 0
                                   && c.info.logicalW == 0 && c.prims.capacity() == pc, 1);
        P.eq("primlist.write_null", PrimList{}.writeText(nullptr), 0);
    }
    return P.finish();
}
