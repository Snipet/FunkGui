#include "PresetManager.h"

#include <cstring>
#include "FactoryPresets.h"

#include <cmath>

// The per-instance half of the preset system. Three decisions shape it, each
// made against what the JUCE 8.0.4 wrappers actually do with a parameter
// write (build/_deps/juce-src/modules/juce_audio_plugin_client):
//
// 1. NO CHANGE GESTURES AROUND A PRESET LOAD. A gesture (begin/end) tells the
//    host "the user has hold of this control": a host in Touch or Latch
//    automation mode writes automation for exactly the parameters inside one.
//    Wrapping a load in fifteen gestures would stamp an automation point on
//    every parameter each time a preset is auditioned during a Latch pass.
//    Outside a gesture the wrappers already send the notification a
//    programmatic change should send:
//      AU   (juce_audio_plugin_client_AU_1.mm, audioProcessorParameterChanged
//           -> sendAUEvent) posts kAudioUnitEvent_ParameterValueChange alone,
//           which is the AU event for a value the user did not touch;
//      VST3 (juce_audio_plugin_client_VST3.cpp, paramChanged) calls
//           setParamNormalized + performEdit on the message thread, and on
//           any other thread only queues the value for the next process()
//           (audioProcessor->setParameterValue). Its beginGesture/endGesture
//           are no-ops off the message thread (`isThisTheMessageThread()`),
//           so a program change arriving from a host thread could not be
//           gestured even if we asked — wrapping only the editor's loads
//           would make the two paths notify differently for the same act.
//    JUCE's own program change is consistent with this: the VST3 wrapper
//    gestures the PROGRAM parameter (audioProcessorChanged, programChanged)
//    and leaves the parameters that program writes ungestured.
//
// 2. isModified COMPARES PLAIN VALUES WITHIN HALF AN INTERVAL. Both sides are
//    snapped to the parameter's grid before comparing: the live value
//    already is (RangedAudioParameter::convertFrom0to1 snaps), and a baseline
//    is captured from live values, so after apply() the two are the same
//    grid point and the comparison is exact — not "within 1e-4", which on a
//    0..20000 Hz range in normalised units is 2 Hz of real difference that
//    reads as unmodified, and on a 0..1 range with a 1e-4 interval is one
//    whole step. Half an interval is the widest tolerance that still sees a
//    one-step nudge. It is taken in plain units rather than normalised
//    because the interval is DEFINED in plain units: on a skewed range
//    (Mod Speed, Pre-Delay, Bass Mult, X-Over, Lo/Hi Cut) one step is a
//    different normalised distance at every point of the range, so a fixed
//    normalised tolerance is either blind at one end or trigger-happy at the
//    other. BankProbe nudges every parameter of every factory preset by one
//    step, both directions, and counts a miss.
//
//    The "order" parameter is an AudioParameterChoice: its plain value is the
//    CHOICE INDEX (0..3 for 4/8/16/32 lines), not the line count, and that
//    index is what a preset stores. The two bools (freeze, bypass) are not
//    preset parameters at all.
//
// 3. THREADS. apply() can arrive from a host's program call on whatever
//    thread the host uses (AU NewFactoryPresetSet, VST3 ProgramChangeParameter
//    ::setNormalized), while the editor reads current() and isModified()
//    every frame on the message thread. current_ sits behind lock_, which is
//    held only to copy a Preset in or out — never across a parameter write,
//    because a parameter write calls into the host synchronously
//    (AUEventListenerNotify, performEdit) and a host that answers by touching
//    the editor would otherwise find the message thread parked on our lock.
//    Whole applies are serialised against each other by applyLock_, so a host
//    program change landing mid-way through an editor load cannot leave half
//    of one preset and half of the other. The audio thread takes neither lock
//    and never calls in here: the processor reads the parameters' own atomics
//    and learns of a load through the onApplied callback (a flag).
//    What this cannot make realtime-safe is a host that issues its program
//    change FROM the audio thread: apply allocates (a Preset copy). No host
//    in the wrappers above does.

namespace hrvb::presets
{
    namespace
    {
        const juce::Identifier kPresetTag  { "PRESET" };
        const juce::Identifier kParamTag   { "PARAM" };
        const juce::Identifier kUuid       { "uuid" };
        const juce::Identifier kName       { "name" };
        const juce::Identifier kCategory   { "category" };
        const juce::Identifier kIsFactory  { "isFactory" };
        const juce::Identifier kMixLocked  { "mixLocked" };
        const juce::Identifier kFormatId   { "format" };
        const juce::Identifier kId         { "id" };
        const juce::Identifier kValue      { "value" };

        const juce::String kMixId { "mix" };
    }

    bool PresetManager::isPresetParameter(const juce::String& id)
    {
        return id != "bypass" && id != "freeze";
    }

    // "Plain value" everywhere below is the APVTS's raw value for the
    // parameter — the std::atomic<float> the processor pushes to the engine
    // and the number getStateInformation saves — and NOT the parameter
    // object's own plain value. The two agree after every write, but not on
    // a fresh instance: AudioParameterFloat starts holding its default
    // literal (Decay 0.6f, 0x3f19999a) while the APVTS adapter starts from
    // convertFrom0to1(getDefaultValue()), the default snapped to the grid
    // (6000 * 0.0001f, 0x3f199999). Reading the parameter object made a
    // fresh instance's Init capture differ by that ulp from Init loaded
    // after any other preset (BankProbe caught it: apply.roundtrip_bad 1,
    // Init / decay), while the engine had been running 0x3f199999 all along.
    // The raw value is the one the sound is made of. For the Choice
    // parameter it is the index, as a float.

    PresetManager::PresetManager(juce::AudioProcessorValueTreeState& s, std::function<void()> f)
        : apvts_(s), onApplied_(std::move(f))
    {
        for (auto* ap : apvts_.processor.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*>(ap))
                if (isPresetParameter(rp->getParameterID()))
                {
                    const auto& range = rp->getNormalisableRange();
                    Slot slot;
                    slot.param        = rp;
                    slot.raw          = apvts_.getRawParameterValue(rp->getParameterID());
                    slot.id           = rp->getParameterID();
                    slot.defaultPlain = rp->convertFrom0to1(rp->getDefaultValue());
                    // No parameter here is continuous, but a future one could
                    // be: a millionth of the span is then "equal" without
                    // being blind to any change a user could make.
                    slot.halfStep     = range.interval > 0.0f
                                      ? 0.5f * range.interval
                                      : 1.0e-6f * (range.end - range.start);
                    slots_.push_back(slot);
                }

        // A fresh instance is factory "Init": every parameter at its default,
        // which is what the APVTS just constructed. The baseline is the full
        // captured set rather than Init's empty list so that current().params
        // always names every preset parameter, whatever path set it.
        current_ = factoryBank().front();
        current_.params = liveValues();
    }

    std::vector<ParamValue> PresetManager::liveValues() const
    {
        std::vector<ParamValue> v;
        v.reserve(slots_.size());
        for (const auto& s : slots_)
            v.push_back({ s.id, s.raw->load() });
        return v;
    }

    Preset PresetManager::capture() const
    {
        Preset p = current();
        p.params = liveValues();
        return p;
    }

    void PresetManager::apply(const Preset& p)
    {
        const juce::ScopedLock al(applyLock_);
        const bool keepMix = mixLocked();

        for (const auto& s : slots_)
        {
            if (keepMix && s.id == kMixId) continue;
            const auto* v = p.find(s.id);
            // A value that is absent loads at the default (PresetTypes.h).
            // So does one that is not a number: a hand-edited or damaged file
            // could carry "nan", and snapToLegalValue passes NaN straight
            // through its comparisons into the parameter.
            const float plain = (v != nullptr && std::isfinite(v->value)) ? v->value : s.defaultPlain;
            const float norm  = s.param->convertTo0to1(plain);
            const float cur   = s.param->getValue();
            // Skip an unchanged value: every write is a host notification
            // (and, in some hosts, an undo entry), and auditioning presets
            // that share most of their settings should not send fifteen.
            // Bitwise, deliberately: "unchanged" means the host would be told
            // nothing new, and anything else is a write.
            if (std::memcmp(&norm, &cur, sizeof(float)) != 0)
                s.param->setValueNotifyingHost(norm);
        }

        // The baseline is what the parameters now HOLD, not what the preset
        // said: the two differ by the grid snap (a literal 0.15f becomes
        // 1500 * 0.0001f, one ulp away), and isModified must be exact the
        // moment a load finishes. With the mix locked, the baseline mix is
        // the kept live value, so unlocking later does not flag it.
        Preset baseline = p;
        baseline.params = liveValues();
        { const juce::ScopedLock sl(lock_); current_ = std::move(baseline); }
        ++revision_;

        // Last, so anything the callback triggers already sees the new
        // identity (the processor only raises its snap flag).
        if (onApplied_) onApplied_();
    }

    bool PresetManager::isModified() const
    {
        const auto base = current();
        const bool skipMix = mixLocked();
        for (const auto& s : slots_)
        {
            if (skipMix && s.id == kMixId) continue;
            const auto* v = base.find(s.id);
            const float want = (v != nullptr && std::isfinite(v->value))
                             ? s.param->getNormalisableRange().snapToLegalValue(v->value)
                             : s.defaultPlain;
            if (std::abs(s.raw->load() - want) > s.halfStep) return true;
        }
        return false;
    }

    Preset PresetManager::current() const
    {
        const juce::ScopedLock sl(lock_);
        return current_;
    }

    void PresetManager::setCurrent(const Preset& p)
    {
        { const juce::ScopedLock sl(lock_); current_ = p; }
        ++revision_;
    }

    uint64_t PresetManager::revision() const { return revision_.load(); }

    void PresetManager::setMixLocked(bool b)
    {
        // The lock changes what isModified compares, so the editor's
        // per-frame revision check must see it.
        if (mixLocked_.exchange(b) != b) ++revision_;
    }

    bool PresetManager::mixLocked() const { return mixLocked_.load(); }

    void PresetManager::writeState(juce::ValueTree& root) const
    {
        // Replace, never append: a tree that already carries a PRESET (one
        // recalled into the APVTS by an older build, say) would otherwise
        // write two, and readState would take whichever came first.
        for (int i = root.getNumChildren(); i-- > 0;)
            if (root.getChild(i).hasType(kPresetTag)) root.removeChild(i, nullptr);

        const auto c = current();
        juce::ValueTree t(kPresetTag);
        t.setProperty(kUuid,      c.uuid,      nullptr);
        t.setProperty(kName,      c.name,      nullptr);
        t.setProperty(kCategory,  c.category,  nullptr);
        t.setProperty(kIsFactory, c.isFactory, nullptr);
        t.setProperty(kMixLocked, mixLocked(), nullptr);
        t.setProperty(kFormatId,  kFormat,     nullptr);
        // Doubles in the tree: juce::var serialises a double with enough
        // digits to round-trip, and every float is exactly a double, so the
        // baseline comes back bit for bit (BankProbe: state.identity rows).
        for (const auto& pv : c.params)
        {
            juce::ValueTree v(kParamTag);
            v.setProperty(kId,    pv.id,             nullptr);
            v.setProperty(kValue, (double) pv.value, nullptr);
            t.appendChild(v, nullptr);
        }
        root.appendChild(t, nullptr);
    }

    void PresetManager::readState(const juce::ValueTree& root)
    {
        const auto t = root.getChildWithName(kPresetTag);
        Preset p;

        if (!t.isValid())
        {
            // A session saved before presets existed. Its sound is whatever
            // the recalled parameters say, and nothing names it, so the
            // identity is an unnamed preset (isValid() false; the editor
            // shows UNTITLED) whose baseline IS those values — it is not
            // "modified" from anything. The processor calls this after the
            // parameters are restored. The mix lock did not exist either:
            // off, deterministically, rather than whatever this instance
            // held before — the same rule setStateInformation applies to an
            // absent parameter.
            p.params = liveValues();
            mixLocked_.store(false);
        }
        else
        {
            p.uuid     = t.getProperty(kUuid).toString();
            p.name     = t.getProperty(kName).toString();
            p.category = t.getProperty(kCategory).toString();
            p.format   = (int) t.getProperty(kFormatId, kFormat);
            // The bank, not the blob, decides what is factory: a preset the
            // bank has since dropped is no longer one, and getCurrentProgram
            // must not answer with an index for it.
            p.isFactory = (bool) t.getProperty(kIsFactory, false) && findFactory(p.uuid) != nullptr;
            if (p.isFactory)
                if (const auto* f = findFactory(p.uuid))
                {
                    p.author = f->author;
                    p.notes  = f->notes;
                }
            for (const auto& v : t)
                if (v.hasType(kParamTag))
                    p.params.push_back({ v.getProperty(kId).toString(),
                                         (float) (double) v.getProperty(kValue) });
            mixLocked_.store((bool) t.getProperty(kMixLocked, false));
        }

        { const juce::ScopedLock sl(lock_); current_ = std::move(p); }
        ++revision_;
    }
}
