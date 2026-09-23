#pragma once

// Semantic tags on recorded primitives (02 §3.2, §3.8). A tag is CPU-only (never uploaded): the dump writes it as
// "t <NAME>" after a primitive's 20 fields, the fingerprint counts primitives per tag, and probes find what they check
// by tag (a curve, a meter, a caret) instead of by position. 0 is untagged. FunkGui owns 1..255 (below); a product
// registers names for its own tags from 256 (FCompressor: Source/editor/Tags.h).

#include <cstdint>
#include <span>

namespace funkgui
{
    using Tag = uint16_t;

    namespace tags
    {
        inline constexpr Tag none         = 0;
        inline constexpr Tag slotLabel    = 1;    // SLOT_LABEL    a slot's label (kLabel)
        inline constexpr Tag slotValue    = 2;    // SLOT_VALUE    its value and unit
        inline constexpr Tag slotSub      = 3;    // SLOT_SUB      the sub-readout or brief line
        inline constexpr Tag slotTrack    = 4;    // SLOT_TRACK    the 1 px rule, dotted when locked, and its fill
        inline constexpr Tag slotCaret    = 5;    // SLOT_CARET    the caret (solid, hollow when derived, ghost)
        inline constexpr Tag slotNotch    = 6;    // SLOT_NOTCH    the Mode-default notch and soft notches
        inline constexpr Tag detentTick   = 7;    // DETENT_TICK   a stepped slot's cell ticks
        inline constexpr Tag detentLabel  = 8;    // DETENT_LABEL  a stepped slot's detent labels
        inline constexpr Tag word         = 9;    // WORD          an attached word (AUTO, EXT, LISTEN)
        inline constexpr Tag latch        = 10;   // LATCH         a latch toggle
        inline constexpr Tag cell         = 11;   // CELL          a segmented-selector cell
        inline constexpr Tag focusRing    = 12;   // FOCUS_RING    the keyboard focus ring
        inline constexpr Tag hint         = 13;   // HINT          the first-run hint / spec line

        inline constexpr Tag lastFunkGui  = 255;  // FunkGui's range is 1..lastFunkGui
        inline constexpr Tag firstProduct = 256;  // a product's tags start here
    }

    struct TagName
    {
        Tag         tag;
        const char* name;                          // the dump spelling: [A-Z0-9_]+, static storage
    };

    // FunkGui's own names, in tag order (the dump spellings of 02 §3.8).
    inline constexpr TagName kFunkGuiTagNames[] = {
        { tags::slotLabel, "SLOT_LABEL" },   { tags::slotValue, "SLOT_VALUE" },     { tags::slotSub, "SLOT_SUB" },
        { tags::slotTrack, "SLOT_TRACK" },   { tags::slotCaret, "SLOT_CARET" },     { tags::slotNotch, "SLOT_NOTCH" },
        { tags::detentTick, "DETENT_TICK" }, { tags::detentLabel, "DETENT_LABEL" }, { tags::word, "WORD" },
        { tags::latch, "LATCH" },            { tags::cell, "CELL" },                { tags::focusRing, "FOCUS_RING" },
        { tags::hint, "HINT" },
    };

    // A product's tag names (tags >= tags::firstProduct), registered once before the first dump is written or parsed;
    // the names must outlive every dump. Registering a tag again replaces its name. Message thread. Implemented with
    // the dump (G3).
    void registerTagNames(std::span<const TagName> names);

    // The dump name of a tag: FunkGui's own for 1..255, the registered product name for >= 256; nullptr for 0 and for
    // an unknown tag (the dump then writes the number). Implemented with the dump (G3).
    const char* tagName(Tag);

    // The inverse, for the dump parser: the tag named `name`, or 0 when no FunkGui or registered name matches.
    Tag tagFromName(const char* name);
}
