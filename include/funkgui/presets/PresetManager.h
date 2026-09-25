#pragma once

// One per plugin instance, owned by the processor. Applies a preset to the
// parameters, captures the parameters as a preset, and remembers which preset
// is loaded and what its values were — so the editor can say "modified", and
// a recalled session can say which preset it was built on. No database: this
// is safe from a host's program-change call on whatever thread that arrives.

#include "PresetTypes.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <functional>

namespace hrvb::presets
{
    class PresetManager
    {
    public:
        // onApplied runs after a preset's values are written; the processor
        // uses it to snap the engine's smoothers rather than glide.
        PresetManager(juce::AudioProcessorValueTreeState&, std::function<void()> onApplied);

        // Every parameter except bypass (the host's) and freeze (a
        // performance latch: loading a preset must never engage it).
        static bool isPresetParameter(const juce::String& id);

        Preset capture() const;          // current values, plain units, carrying the current identity
        void   apply(const Preset&);     // absent params load at their defaults; mix kept if mixLocked()
        bool   isModified() const;       // live values vs the loaded preset's
        Preset current() const;          // identity + baseline values (a copy; thread safe)
        void   setCurrent(const Preset&);// identity + baseline, values untouched (after a save or rename)
        uint64_t revision() const;       // bumps whenever current() changes, host program changes included

        void setMixLocked(bool);
        bool mixLocked() const;

        // Session state: a <PRESET> child with identity, the baseline values
        // and the mix lock. Read restores them without touching a parameter.
        void writeState(juce::ValueTree& root) const;
        void readState(const juce::ValueTree& root);

    private:
        // The preset parameters, in the processor's layout order, resolved
        // once: apply/capture/isModified run on the message thread per frame
        // (isModified) and on a host's program-change thread, and a
        // dynamic_cast per parameter per call bought nothing.
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

        juce::AudioProcessorValueTreeState& apvts_;
        std::function<void()> onApplied_;
        mutable juce::CriticalSection lock_;       // guards current_ only; never held across a host call
        juce::CriticalSection applyLock_;          // serialises whole applies against each other
        Preset current_;
        std::atomic<uint64_t> revision_{ 1 };
        std::atomic<bool> mixLocked_{ false };
    };
}
