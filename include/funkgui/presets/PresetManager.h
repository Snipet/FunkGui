#pragma once

// One per plugin instance, owned by the processor. Applies a preset to the parameters, captures the parameters as a
// preset, and remembers which preset is loaded and what its values were — so the editor can say "modified", and a
// recalled session can say which preset it was built on. No database: this is safe from a host's program-change call on
// whatever thread that arrives. (HardwareReverb Source/presets/PresetManager.h; G8 replaced the product rules with
// PresetHooks, 01 §9.2.)

#include <funkgui/presets/PresetTypes.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace funkgui::presets
{
    // PresetManager's product seams (01 §9.2). Every member is optional; a null hook is skipped. Hooks run on the
    // thread that called the PresetManager function (apply(): the message thread, or a host's program-change thread).
    struct PresetHooks
    {
        // Which parameters a preset holds, asked once per parameter at construction. Null: every parameter (every
        // juce::RangedAudioParameter of the processor). HardwareReverb: all but "bypass" and "freeze"; FCompressor: the
        // Mode-filtered parameters (not "mode", which the "modeId" attribute carries).
        std::function<bool(const juce::String& id)> isPresetParameter;

        // apply()'s order, always exactly this (01 §9.2, K2 #23):
        //   1. beginApply()          FCompressor: processor.beginBatch()
        //   2. applyBefore(preset)   FCompressor: set `mode` from attr("modeId")
        //   3. every preset parameter is written; absent (or not finite) -> the parameter's default
        //   4. the baseline and identity become the applied preset's (current(), revision())
        //   5. onApplied()           FCompressor: processor.endBatch(), which raises the snap; HardwareReverb: the snap
        // beginApply and onApplied are called in pairs, so a product that opens a batch in one closes it in the other.
        std::function<void()>              beginApply;
        std::function<void(const Preset&)> applyBefore;
        std::function<void()>              onApplied;

        // capture() calls it last, on the preset it returns (FCompressor: setAttr("modeId"/"modeRev")).
        std::function<void(Preset&)>       captureExtra;

        // ---- additions to 01 §9.2 (HardwareReverb's behaviour, generalised) ----------------------------------------
        // The product's factory bank by uuid, or nullptr. readState() trusts a saved isFactory only when the bank
        // still has the uuid (and takes author and notes from it). Null: no preset is factory after a state load.
        std::function<const Preset*(const juce::String& uuid)> findFactory;

        // The identity of a fresh instance (HardwareReverb and FCompressor: the factory "Init"). Its params are
        // replaced by the live values. Null: an unnamed preset (isValid() false).
        std::function<Preset()> initialPreset;

        // The parameter setMixLocked(true) keeps across loads (HardwareReverb: "mix"). Empty: the lock keeps nothing.
        juce::String mixParameter;
    };

    class PresetManager
    {
    public:
        PresetManager(juce::AudioProcessorValueTreeState&, PresetHooks);

        // Whether `id` is one of the parameters this manager applies and captures (PresetHooks::isPresetParameter as
        // resolved at construction).
        bool isPresetParameter(const juce::String& id) const;

        Preset capture() const;          // current values, plain units, carrying the current identity; captureExtra
        void   apply(const Preset&);     // PresetHooks order; absent params load at their defaults; mix kept if locked
        bool   isModified() const;       // live values vs the loaded preset's (half an interval, plain units)
        Preset current() const;          // identity + baseline values (a copy; thread safe)
        void   setCurrent(const Preset&);// identity + baseline, values untouched (after a save or rename)
        uint64_t revision() const;       // bumps whenever current() changes, host program changes included

        void setMixLocked(bool);
        bool mixLocked() const;

        // Session state: a <PRESET> child with identity, the baseline values (PARAM), the attributes (ATTR) and the
        // mix lock. Read restores them without touching a parameter.
        void writeState(juce::ValueTree& root) const;
        void readState(const juce::ValueTree& root);

    private:
        // The preset parameters, in the processor's layout order, resolved once: apply/capture/isModified run on the
        // message thread per frame (isModified) and on a host's program-change thread, and a dynamic_cast per
        // parameter per call bought nothing.
        struct Slot
        {
            juce::RangedAudioParameter* param = nullptr;
            std::atomic<float>* raw = nullptr;   // the APVTS's plain value: what the engine and the saved state get
            juce::String id;
            float defaultPlain = 0.0f;   // the parameter's default, plain units
            float halfStep     = 0.0f;   // half the range's interval, plain units
        };
        std::vector<Slot> slots_;

        std::vector<ParamValue> liveValues() const;
        bool keeps(const Slot&, bool locked) const;

        juce::AudioProcessorValueTreeState& apvts_;
        PresetHooks hooks_;
        mutable juce::CriticalSection lock_;       // guards current_ only; never held across a host call
        juce::CriticalSection applyLock_;          // serialises whole applies against each other
        Preset current_;
        std::atomic<uint64_t> revision_{ 1 };
        std::atomic<bool> mixLocked_{ false };
    };
}
