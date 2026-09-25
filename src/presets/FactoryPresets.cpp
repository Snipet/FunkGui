// The factory bank. Data, measured: Tools/BankProbe.cpp renders every row
// through the processor and fingerprints its impulse response, so any change
// to a number here shows up as a moved row — bump factoryBankRevision() with
// it, or the preset database keeps serving the old copy.
//
// How to read the columns (plain units, as PresetTypes.h stores them):
//
//   ord    Matrix Order as its CHOICE INDEX: 0 = 4 lines, 1 = 8, 2 = 16,
//          3 = 32. More lines, faster echo build-up (IrAnalyse: t_dense).
//          4 lines is audibly metallic, which only Special uses on purpose.
//   size   0..1. Scales every loop by 0.25 + 5.75 * size (FDNReverb.cpp
//          kSizeScaleMin/Max) around seeds of 743..4457 samples at 48 kHz;
//          the editor prints the resulting mean loop in ms. At Order 8 that
//          is 24 * (0.25 + 5.75 * size) ms: 13 ms at 0.05, 34 at 0.2, 75 at
//          0.5, 130 at 0.9; Order 32 reaches the longer seeds, about 54 *
//          (0.25 + 5.75 * size) ms. Rooms stay under ~0.25. Halls sit at
//          0.4..0.5, not higher: the first draft's 0.6..0.75 never became
//          diffuse (BankProbe dens: 458 ms, and never within 540 ms for
//          Large Hall) — at those loop lengths a hall reads as a canyon.
//   modHz  Mod Speed, Hz. modAmt  Mod Amount, samples of delay excursion
//          (peak; the editor shows 2 * amt / 48 ms peak-to-peak). Small
//          amounts (1..4) break up the metallic ring of a static FDN; large
//          ones (12..32) are an audible chorus, which is Modulated's job.
//   decay  0..1 exponential: RT60 = 0.2 * 150^decay seconds (FDNReverb.cpp
//          kRT60Min/Max). The value asked for is written in each row's
//          comment; BankProbe measures it back.
//   damp   0..1 in-loop one-pole, pole 0.05 + 0.87 * damp at 48 kHz. Higher
//          is darker: the HF tail dies faster than the broadband one.
//   mix    0..1, linear dry/wet. Set as an INSERT value (the editor's mix
//          lock keeps a send's 100 % across preset changes); ambient and
//          special presets sit wetter because that is what they are for.
//   pre    Pre-Delay, ms. Halls get a long one (the gap before the first
//          reflection is how a large room reads as large); plates none.
//   diff   Input diffusion, 0..1: smears the excitation once, before the
//          loop — softens the attack.
//   smear  In-loop diffusion, 0..1: diffuses on every circulation, so echo
//          density grows exponentially — the dense, flutter-free plate
//          texture. Plates use it high, with diff high.
//   bass   Bass Mult, RT60 multiplier below X-Over. Halls > 1 (the warm,
//          long low end of a big room), small rooms and plates <= 1.
//   xover  X-Over, Hz, for Bass Mult.
//   width  0..2, 1 = as encoded, 0 = mono, 2 = exaggerated.
//   loCut  Hz on the wet path, 0 = off. hiCut Hz on the wet path, 20000 = off.
//
// Every row states every column, defaults included: a row that relied on a
// parameter's default would change sound if that default were ever moved,
// and it would do so silently. Init alone is empty — "all defaults" is what
// Init means.
//
// UUIDs are version 4, generated once and fixed here: they are the identity
// the database, saved sessions and tags key on. Never reuse one for a
// different sound; retire a preset by deleting its row.
//
// Names are plain printable ASCII: the editor's font has no other glyphs.

#include "FactoryPresets.h"

namespace hrvb::presets
{
    namespace
    {
        struct Row
        {
            const char* uuid;
            const char* name;
            const char* category;
            const char* notes;
            float ord, size, modHz, modAmt, decay, damp, mix, pre, diff, smear,
                  bass, xover, width, loCut, hiCut;
        };

        // Order = the processor's parameter layout, which is the order
        // capture() lists them in.
        Preset make(const Row& r)
        {
            Preset p;
            p.uuid = r.uuid; p.name = r.name; p.category = r.category;
            // No author: the list already says FACTORY, and the browser's
            // search covers author — "HardwareReverb" there made typing "ha"
            // (for Hall) match every preset in the bank.
            p.author = {}; p.notes = r.notes; p.isFactory = true;
            p.params = {
                { "order",     r.ord   }, { "size",      r.size  },
                { "modSpeed",  r.modHz }, { "modAmount", r.modAmt },
                { "decay",     r.decay }, { "damp",      r.damp  },
                { "mix",       r.mix   }, { "preDelay",  r.pre   },
                { "diffusion", r.diff  }, { "smear",     r.smear },
                { "bassMult",  r.bass  }, { "xover",     r.xover },
                { "width",     r.width }, { "loCut",     r.loCut },
                { "hiCut",     r.hiCut },
            };
            return p;
        }

        Preset makeInit()
        {
            Preset p;
            p.uuid = "6d35c610-6939-4f03-bd7b-07471f79c8b4";
            p.name = "Init"; p.category = "Init"; p.author = {};
            p.notes = "Every control at its default: the state a new instance starts in.";
            p.isFactory = true;
            return p;
        }

        std::vector<Preset> build()
        {
            // decay column: RT60 asked for, in the comment.
            static const Row rows[] = {
                //   uuid, name, category, notes,
                // ord   size   modHz modAmt    decay     damp      mix     pre     diff    smear    bass xover   width  loCut  hiCut

                // ---- Room: small Size, short Decay, some diffusion so the
                //      first reflections do not flutter.
                { "9376e2dc-0f80-4930-bd6d-741f8d6af42f", "Small Room", "Room",
                  "A tight, natural room for adding space without distance.",
                     2, 0.08f, 0.400f, 1.00f, 0.1805f, 0.3851f, 0.2200f,   2.0f, 0.7000f, 0.0000f, 0.900f,  500, 0.900f,     0, 20000 },  // 0.5 s
                { "d349dacf-669a-480c-a7dc-9f050e674032", "Drum Room", "Room",
                  "Live, slightly bright room ambience for kits; low end kept short.",
                     2, 0.15f, 0.300f, 1.50f, 0.2738f, 0.2255f, 0.2000f,   5.0f, 0.6000f, 0.0000f, 0.800f,  400, 1.000f,    80, 20000 },  // 0.8 s
                { "729cf3dd-acb2-46e5-81bd-21022dc5cbf0", "Vocal Booth", "Room",
                  "A small, damped space: presence without an obvious tail.",
                     1, 0.04f, 0.500f, 0.50f, 0.0809f, 0.6000f, 0.1500f,   0.0f, 0.8000f, 0.0000f, 0.800f,  500, 0.800f,     0,  9000 },  // 0.3 s
                { "5c41f843-499f-419e-bb89-98e3f19a459b", "Live Room", "Room",
                  "A medium tracking room with a little more air.",
                     2, 0.25f, 0.300f, 2.00f, 0.3373f, 0.2872f, 0.2500f,   8.0f, 0.8000f, 0.2500f, 1.000f,  500, 1.100f,     0, 20000 },  // 1.1 s

                // ---- Chamber: dense, bright-ish, mid Size; smear for the
                //      smooth build of a reverberant stone room.
                { "966b6f1a-6938-4e8f-b584-144396180a23", "Echo Chamber", "Chamber",
                  "Classic studio chamber: dense and smooth, with a clean low end.",
                     2, 0.30f, 0.250f, 2.00f, 0.4122f, 0.1776f, 0.2500f,  12.0f, 0.9000f, 0.3000f, 1.000f,  500, 1.000f,   100, 20000 },  // 1.6 s
                { "65fad74f-d6b8-4656-9174-bcb8107c55f1", "Stone Chamber", "Chamber",
                  "A larger, brighter chamber with a hard, reflective character.",
                     3, 0.35f, 0.200f, 1.50f, 0.4781f, 0.0431f, 0.2500f,  15.0f, 1.0000f, 0.4000f, 1.100f,  500, 1.000f,     0, 20000 },  // 2.2 s

                // ---- Hall: long pre-delay, Bass Mult > 1, and Smear 0.3..0.35:
                //      at hall loop lengths the input diffuser alone left the
                //      tail grainy for 300+ ms; a little in-loop diffusion
                //      brought the density crossing to 275..359 ms (with the
                //      pre-delay) without the plate's texture.
                { "f02230bb-6666-45ab-b715-004da07d7c59", "Concert Hall", "Hall",
                  "A balanced concert hall for orchestral and acoustic sources.",
                     3, 0.40f, 0.250f, 3.00f, 0.4930f, 0.2352f, 0.2500f,  28.0f, 1.0000f, 0.3000f, 1.300f,  400, 1.100f,     0, 20000 },  // 2.4 s
                { "75e3f423-c541-4861-989d-cb768a5693ec", "Large Hall", "Hall",
                  "A big hall with a long pre-delay and a warm, lingering low end.",
                     3, 0.50f, 0.200f, 4.00f, 0.5680f, 0.2848f, 0.3000f,  40.0f, 1.0000f, 0.3500f, 1.400f,  350, 1.200f,    40, 20000 },  // 3.5 s
                { "ca654125-a79d-4a78-b4aa-78d90de899f7", "Dark Hall", "Hall",
                  "A heavily damped hall: the tail sits behind the source.",
                     2, 0.50f, 0.250f, 3.00f, 0.5406f, 0.6307f, 0.3000f,  35.0f, 0.7000f, 0.3000f, 1.500f,  500, 1.000f,     0,  6000 },  // 3.0 s
                { "08085b46-7c72-419e-a270-cb2e41a6d7b8", "Bright Hall", "Hall",
                  "An open, airy hall with the low end cut for busy mixes.",
                     3, 0.40f, 0.300f, 2.50f, 0.4584f, 0.0143f, 0.2200f,  22.0f, 0.8000f, 0.3000f, 1.150f,  500, 1.200f,   150, 20000 },  // 2.0 s

                // ---- Plate: high Smear and Diffusion, small Size, no
                //      pre-delay to speak of, low cut — a plate has no
                //      room's low end.
                { "161adba6-ebb1-44e1-82e4-2f9eee2e7b35", "Vocal Plate", "Plate",
                  "Smooth, dense plate with a short pre-delay to keep words clear.",
                     2, 0.20f, 0.800f, 2.00f, 0.4371f, 0.1359f, 0.2000f,  20.0f, 1.0000f, 0.8000f, 0.900f,  500, 1.200f,   150, 20000 },  // 1.8 s
                { "699c6db1-c223-4af0-b9a0-78cd0098d3fb", "Bright Plate", "Plate",
                  "A shimmering, undamped plate that adds sheen and sustain.",
                     3, 0.25f, 0.600f, 1.50f, 0.4967f, 0.0001f, 0.2000f,   0.0f, 1.0000f, 1.0000f, 0.800f,  500, 1.000f,   200, 20000 },  // 2.4 s
                { "90ea22a4-8867-4d9a-839a-84beae5ff70c", "Drum Plate", "Plate",
                  "A short, punchy plate for snares and toms.",
                     2, 0.15f, 0.500f, 1.00f, 0.3551f, 0.2333f, 0.1800f,   0.0f, 1.0000f, 0.7000f, 0.800f,  500, 1.000f,   180, 12000 },  // 1.2 s

                // ---- Ambient: long Decay with modulation, so the tail
                //      keeps moving instead of ringing on fixed modes.
                { "c53be265-7b5f-4ced-a617-594e5ce32258", "Cathedral", "Ambient",
                  "A vast stone space with a long, slow bloom and deep lows.",
                     3, 0.70f, 0.200f, 6.00f, 0.7347f, 0.3280f, 0.3500f,  60.0f, 1.0000f, 0.6000f, 1.400f,  300, 1.200f,     0, 20000 },  // 8 s
                { "a9f2c728-4942-423f-82db-15ab077115a6", "Endless Pad", "Ambient",
                  "A near-infinite wash for pads and drones. Mostly wet.",
                     3, 0.85f, 0.350f,12.00f, 0.9171f, 0.3724f, 0.5000f,   0.0f, 1.0000f, 1.0000f, 1.000f,  500, 1.250f,   100, 20000 },  // 20 s
                { "f895ded9-d155-4605-a44e-cd1be73b7a98", "Cloud", "Ambient",
                  "A soft, drifting cloud behind a long pre-delay.",
                     2, 0.70f, 0.600f,16.00f, 0.8199f, 0.3251f, 0.4000f,  80.0f, 1.0000f, 0.9000f, 1.000f,  500, 1.200f,     0,  8000 },  // 12 s

                // ---- Modulated: Mod Amount is the effect.
                { "cfa6aa93-4b5b-4601-942b-1441dcb90c8f", "Chorus Space", "Modulated",
                  "A wide, chorused room: the tail detunes as it decays.",
                     1, 0.45f, 1.200f,20.00f, 0.5041f, 0.3500f, 0.3000f,  10.0f, 0.6000f, 0.0000f, 1.000f,  500, 1.300f,     0, 20000 },  // 2.5 s
                { "9365b5ce-47f6-4186-88f4-93f4fc49993d", "Warble Tank", "Modulated",
                  "Deep, fast pitch wobble, like a worn tape of a spring tank.",
                     1, 0.30f, 3.500f,32.00f, 0.5979f, 0.5000f, 0.3500f,   0.0f, 0.5000f, 0.4000f, 1.000f,  500, 1.000f,     0, 20000 },  // 4 s
                { "85cc5efc-d245-4ab2-8187-cd8e08b27720", "Drift Hall", "Modulated",
                  "A hall whose tail slowly drifts in pitch, never quite settling.",
                     3, 0.45f, 0.150f,10.00f, 0.6405f, 0.1473f, 0.3000f,  30.0f, 1.0000f, 0.3000f, 1.200f,  400, 1.200f,     0, 20000 },  // 5 s

                // ---- Special: settings no real space has.
                { "89f44261-fc6b-46a0-bcdd-e74fe3dabe9a", "Metal Box", "Special",
                  "Four lines, no damping, no modulation: a ringing metallic resonator.",
                     0, 0.10f, 0.300f, 0.00f, 0.4021f, 0.0000f, 0.3000f,   0.0f, 0.0000f, 0.0000f, 1.000f,  500, 1.000f,     0, 20000 },  // 1.5 s
                { "27720307-fd37-458f-b37e-337ec6836bbe", "Tin Can", "Special",
                  "A tiny, band-limited box: lo-fi and nasal.",
                     0, 0.02f, 0.300f, 0.00f, 0.2193f, 0.0500f, 0.4000f,   0.0f, 0.0000f, 0.0000f, 1.000f,  500, 0.500f,   400,  5000 },  // 0.6 s
                { "642e7fdb-d734-4269-b66b-6667c66e74c2", "Super Wide", "Special",
                  "Exaggerated width: the tail spills past the speakers. Check it in mono.",
                     2, 0.40f, 0.300f, 3.00f, 0.3990f, 0.2211f, 0.3000f,  10.0f, 0.8000f, 0.0000f, 1.000f,  500, 2.000f,     0, 20000 },  // 1.5 s
                { "06bdba0b-e8d6-49dd-834d-755a857f8281", "Distant Slap", "Special",
                  "The longest pre-delay: a slap from far away, then a short room.",
                     2, 0.30f, 0.300f, 2.00f, 0.3544f, 0.3204f, 0.3000f, 200.0f, 0.8000f, 0.3000f, 1.000f,  500, 1.000f,     0, 20000 },  // 1.2 s
            };

            std::vector<Preset> bank;
            bank.push_back(makeInit());
            for (const auto& r : rows) bank.push_back(make(r));
            return bank;
        }
    }

    const std::vector<Preset>& factoryBank()
    {
        static const std::vector<Preset> bank = build();
        return bank;
    }

    // 1: the first measured bank (replaces the 5-row placeholder, revision 0).
    // 2: factory presets carry no author.
    // 3: re-voiced for per-second absorption (FDNReverb::recomputeFeedback).
    //    Damping used to act once per pass, so short loops were absorbed more
    //    per second than long ones and the bank had been voiced against that.
    //    Every preset that moved had its Damping and Decay bisected until its
    //    >4 kHz decay and broadband RT60 matched what revision 2 measured.
    //    Endless Pad was tuned on a 24 s render (its tail outlasts 8 s).
    //    Bright Plate's Damping reached zero 0.03 s short of its old HF decay.
    int factoryBankRevision() { return 3; }

    const Preset* findFactory(const juce::String& uuid)
    {
        for (const auto& p : factoryBank())
            if (p.uuid == uuid) return &p;
        return nullptr;
    }

    int factoryIndexOf(const juce::String& uuid)
    {
        const auto& b = factoryBank();
        for (size_t i = 0; i < b.size(); ++i)
            if (b[i].uuid == uuid) return (int) i;
        return -1;
    }
}
