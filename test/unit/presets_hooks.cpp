// FUNKGUI_TEST name=fg.presets.hooks timeout=120 gpu=0 links=presets
//
// fg.presets.hooks: PresetManager over a real juce::AudioProcessorValueTreeState with every PresetHooks member set
// (presets/PresetManager.h; FCompressor docs/design/01-core-contracts.md §9.2, K2 #23) —
//   ORDER     apply() runs beginApply, applyBefore, the preset parameters (absent or NaN -> default, unchanged values
//             not rewritten, no gestures), then onApplied, which already sees the new identity; nothing else is
//             touched; begin/applied always pair;
//   CAPTURE   capture() = identity + live raw values (bit for bit) + captureExtra;
//   MODIFIED  exact after apply, sensitive to one interval (and to a millionth of a continuous span);
//   MIX LOCK  PresetHooks::mixParameter survives loads, is never "modified" while locked, and unlocks clean;
//   STATE     writeState/readState round trip identity, baseline, attributes and the lock; the bank (findFactory), not
//             the blob, decides isFactory; a session without <PRESET> is an unnamed baseline of the live values;
//   DEFAULTS  with no hooks at all every RangedAudioParameter is a preset parameter and apply still works.
// HardwareReverb's BankProbe held its manager to the same apply/capture/isModified/mix-lock rules. Spec rows only.

#include <funkgui/presets/PresetManager.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
namespace FP = funkgui::presets;

namespace
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout l;
        l.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "gain", 1 }, "Gain",
                                                          juce::NormalisableRange<float>(-60.0f, 12.0f, 0.01f), 0.0f));
        l.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "ratio", 1 }, "Ratio",
                                                          juce::NormalisableRange<float>(1.0f, 20.0f), 4.0f));   // continuous
        l.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "mix", 1 }, "Mix",
                                                          juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 1.0f));
        l.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { "mode", 1 }, "Mode",
                                                           juce::StringArray { "clean", "fet-76", "opto-2a" }, 0));
        l.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { "bypass", 1 }, "Bypass", false));
        return l;
    }

    class Processor final : public juce::AudioProcessor
    {
    public:
        Processor() : apvts(*this, nullptr, "PARAMS", layout()) {}

        using juce::AudioProcessor::processBlock;

        const juce::String getName() const override { return "fg.presets.hooks"; }
        void prepareToPlay(double, int) override {}
        void releaseResources() override {}
        void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void getStateInformation(juce::MemoryBlock&) override {}
        void setStateInformation(const void*, int) override {}

        juce::RangedAudioParameter& param(const char* id) { return *apvts.getParameter(id); }
        float raw(const char* id) { return apvts.getRawParameterValue(id)->load(); }
        void setPlain(const char* id, float plain)
        {
            auto& p = param(id);
            p.setValueNotifyingHost(p.convertTo0to1(plain));
        }

        juce::AudioProcessorValueTreeState apvts;
    };

    // What the host and the product see, in order: "begin", "before:<modeId>", "v:<id>", "g:<id>" (a gesture),
    // "applied".
    class Log final : public juce::AudioProcessorParameter::Listener
    {
    public:
        explicit Log(Processor& p) : proc_(p)
        {
            for (auto* ap : proc_.getParameters()) ap->addListener(this);
        }
        ~Log() override
        {
            for (auto* ap : proc_.getParameters()) ap->removeListener(this);
        }

        void parameterValueChanged(int index, float) override { add("v:" + id(index)); }
        void parameterGestureChanged(int index, bool) override { add("g:" + id(index)); }

        void add(const std::string& e)
        {
            if (!events_.empty()) events_ += ' ';
            events_ += e;
        }
        std::string take()
        {
            std::string s;
            s.swap(events_);
            return s;
        }

    private:
        std::string id(int index)
        {
            auto* p = dynamic_cast<juce::RangedAudioParameter*>(proc_.getParameters()[index]);
            return p != nullptr ? p->getParameterID().toStdString() : "?";
        }

        Processor& proc_;
        std::string events_;
    };

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    FP::Preset factoryInit()
    {
        FP::Preset p;
        p.uuid = "00000000-0000-4000-8000-000000000001";
        p.name = "Init";
        p.author = "FunkGui";
        p.notes = "all defaults";
        p.isFactory = true;
        p.attributes = { { "modeId", "clean" } };
        return p;
    }

    FP::Preset factoryFet()
    {
        FP::Preset p;
        p.uuid = "00000000-0000-4000-8000-000000000002";
        p.name = "Fet Slam";
        p.category = "Drums";
        p.author = "FunkGui";
        p.notes = "fast";
        p.isFactory = true;
        p.params = { { "gain", -12.0f }, { "ratio", 8.0f } };      // mix absent: loads at its default
        p.attributes = { { "modeId", "fet-76" }, { "modeRev", "1" } };
        return p;
    }

    const FP::Preset* findFactory(const juce::String& uuid)
    {
        static const FP::Preset init = factoryInit(), fet = factoryFet();
        if (uuid == init.uuid) return &init;
        if (uuid == fet.uuid) return &fet;
        return nullptr;
    }

    // The product side, as FCompressor's P3 will write it: a batch counter around the load, `mode` set from the
    // modeId attribute before the values, the mode captured as attributes.
    struct Product
    {
        Product(Processor& p, Log& l) : proc(p), log(l) {}

        Processor& proc;
        Log& log;
        FP::PresetManager* mgr = nullptr;
        int depth = 0, maxDepth = 0, begins = 0, applieds = 0;
        bool identityAtApplied = false;
        uint64_t revisionAtApplied = 0;
        juce::String expectUuid;

        FP::PresetHooks hooks()
        {
            FP::PresetHooks h;
            h.isPresetParameter = [](const juce::String& id) { return id != "mode" && id != "bypass"; };
            h.beginApply = [this]
            {
                ++begins;
                maxDepth = juce::jmax(maxDepth, ++depth);
                log.add("begin");
            };
            h.applyBefore = [this](const FP::Preset& p)
            {
                const auto* a = p.attr("modeId");
                const juce::String key = a != nullptr ? a->value : juce::String("clean");     // missing -> clean
                log.add("before:" + key.toStdString());
                auto* choice = dynamic_cast<juce::AudioParameterChoice*>(&proc.param("mode"));
                const int index = juce::jmax(0, choice->choices.indexOf(key));
                if (choice->getIndex() != index)
                    proc.setPlain("mode", static_cast<float>(index));
            };
            h.onApplied = [this]
            {
                ++applieds;
                --depth;
                log.add("applied");
                identityAtApplied = mgr != nullptr && mgr->current().uuid == expectUuid;
                revisionAtApplied = mgr != nullptr ? mgr->revision() : 0;
            };
            h.captureExtra = [this](FP::Preset& p)
            {
                auto* choice = dynamic_cast<juce::AudioParameterChoice*>(&proc.param("mode"));
                p.setAttr("modeId", choice->getCurrentChoiceName());
                p.setAttr("modeRev", "1");
            };
            h.findFactory = &findFactory;
            h.initialPreset = [] { return factoryInit(); };
            h.mixParameter = "mix";
            return h;
        }
    };

    void expect(T::Probe& P, std::string_view key, Log& log, std::string_view want)
    {
        const std::string got = log.take();
        if (got != want)
            std::printf("INFO     %.*s: got \"%s\", want \"%.*s\"\n", static_cast<int>(key.size()), key.data(),
                        got.c_str(), static_cast<int>(want.size()), want.data());
        P.eq(key, got == want, 1);
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    T::Probe P("fg.presets.hooks", "", argc, argv);

    Processor proc;
    Log log(proc);
    Product product(proc, log);
    FP::PresetManager mgr(proc.apvts, product.hooks());
    product.mgr = &mgr;
    log.take();

    // ---- construction -----------------------------------------------------------------------------------------------
    P.eq("init.preset_parameters", mgr.isPresetParameter("gain") && mgr.isPresetParameter("ratio")
                                       && mgr.isPresetParameter("mix"), 1);
    P.eq("init.excluded_parameters", mgr.isPresetParameter("mode") || mgr.isPresetParameter("bypass")
                                         || mgr.isPresetParameter("nope"), 0);
    const auto c0 = mgr.current();
    P.eq("init.identity", c0.uuid == factoryInit().uuid && c0.name == "Init" && c0.isFactory, 1);
    P.eq("init.attributes", c0.attr("modeId") != nullptr && c0.attr("modeId")->value == "clean", 1);
    P.eq("init.baseline_is_live", c0.params.size() == 3 && c0.find("gain") != nullptr
                                      && sameBits(c0.find("gain")->value, proc.raw("gain")), 1);
    P.eq("init.not_modified", mgr.isModified(), 0);
    P.eq("init.no_hook_calls", product.begins + product.applieds, 0);

    // ---- ORDER ------------------------------------------------------------------------------------------------------
    const auto fet = factoryFet();
    product.expectUuid = fet.uuid;
    const auto rev0 = mgr.revision();
    mgr.apply(fet);
    expect(P, "order.apply", log, "begin before:fet-76 v:mode v:gain v:ratio applied");
    P.eq("order.identity_before_onapplied", product.identityAtApplied, 1);
    P.eq("order.revision_before_onapplied", static_cast<int64_t>(product.revisionAtApplied - rev0), 1);
    P.eq("order.pairs", product.begins == 1 && product.applieds == 1 && product.depth == 0, 1);
    P.eq("order.values", std::abs(proc.raw("gain") + 12.0f) < 1e-4f && std::abs(proc.raw("ratio") - 8.0f) < 1e-5f
                             && sameBits(proc.raw("mix"), 1.0f), 1);
    P.eq("order.mode_from_attribute", dynamic_cast<juce::AudioParameterChoice&>(proc.param("mode")).getIndex(), 1);
    P.eq("order.bypass_untouched", sameBits(proc.raw("bypass"), 0.0f), 1);
    P.eq("order.current", mgr.current().uuid == fet.uuid && mgr.current().attr("modeRev") != nullptr, 1);
    P.eq("order.not_modified", mgr.isModified(), 0);

    mgr.apply(fet);                                  // the same preset again: nothing to tell the host
    expect(P, "order.unchanged_not_rewritten", log, "begin before:fet-76 applied");

    FP::Preset sparse;
    sparse.uuid = "a1b2c3d4-0000-4000-8000-000000000010";
    sparse.name = "Sparse";
    sparse.params = { { "ratio", std::numeric_limits<float>::quiet_NaN() }, { "unknown", 3.0f } };
    product.expectUuid = sparse.uuid;
    mgr.apply(sparse);                               // no modeId -> clean; gain absent, ratio NaN -> defaults
    expect(P, "order.absent_and_nan_default", log, "begin before:clean v:mode v:gain v:ratio applied");
    P.eq("order.defaults", sameBits(proc.raw("gain"), 0.0f) && std::abs(proc.raw("ratio") - 4.0f) < 1e-5f, 1);
    P.eq("order.pairs_never_nested", product.begins == 3 && product.applieds == 3 && product.maxDepth == 1
                                         && product.depth == 0, 1);

    // ---- CAPTURE ----------------------------------------------------------------------------------------------------
    proc.setPlain("gain", -3.37f);
    proc.setPlain("mode", 2.0f);
    log.take();
    const auto cap = mgr.capture();
    P.eq("capture.identity", cap.uuid == sparse.uuid && cap.name == "Sparse", 1);
    P.eq("capture.raw_values", cap.params.size() == 3 && cap.find("gain") != nullptr
                                   && sameBits(cap.find("gain")->value, proc.raw("gain"))
                                   && sameBits(cap.find("mix")->value, proc.raw("mix")), 1);
    P.eq("capture.no_excluded", cap.find("mode") == nullptr && cap.find("bypass") == nullptr, 1);
    P.eq("capture.extra", cap.attr("modeId") != nullptr && cap.attr("modeId")->value == "opto-2a"
                              && cap.attr("modeRev") != nullptr, 1);
    P.eq("capture.silent", log.take().empty(), 1);

    // apply(capture()) is exact: no write, not modified, same values.
    product.expectUuid = cap.uuid;
    mgr.apply(cap);
    expect(P, "capture.reapply_silent", log, "begin before:opto-2a applied");
    P.eq("capture.reapply_not_modified", mgr.isModified(), 0);

    // ---- MODIFIED ---------------------------------------------------------------------------------------------------
    const float g = proc.raw("gain");
    proc.setPlain("gain", g + 0.01f);
    P.eq("modified.one_interval_up", mgr.isModified(), 1);
    proc.setPlain("gain", g - 0.01f);
    P.eq("modified.one_interval_down", mgr.isModified(), 1);
    proc.setPlain("gain", g);
    P.eq("modified.restored", mgr.isModified(), 0);
    const float r = proc.raw("ratio");
    proc.setPlain("ratio", r + 1.0e-3f);
    P.eq("modified.continuous", mgr.isModified(), 1);
    proc.setPlain("ratio", r);
    P.eq("modified.continuous_restored", mgr.isModified(), 0);

    // ---- MIX LOCK ---------------------------------------------------------------------------------------------------
    proc.setPlain("mix", 0.77f);
    const float kept = proc.raw("mix");
    auto rev = mgr.revision();
    mgr.setMixLocked(true);
    P.eq("mixlock.revision", static_cast<int64_t>(mgr.revision() - rev), 1);
    FP::Preset wet = fet;
    wet.params.push_back({ "mix", 0.3f });
    product.expectUuid = wet.uuid;
    mgr.apply(wet);
    P.eq("mixlock.kept", sameBits(proc.raw("mix"), kept), 1);
    P.eq("mixlock.not_modified", mgr.isModified(), 0);
    proc.setPlain("mix", 0.5f);
    P.eq("mixlock.move_not_modified", mgr.isModified(), 0);
    proc.setPlain("mix", kept);
    mgr.setMixLocked(false);
    P.eq("mixlock.unlock_clean", mgr.isModified(), 0);
    mgr.apply(wet);
    P.eq("mixlock.unlocked_loads", std::abs(proc.raw("mix") - 0.3f) <= 0.5e-4f, 1);
    rev = mgr.revision();
    mgr.setMixLocked(false);
    P.eq("mixlock.same_state_silent", mgr.revision() == rev, 1);

    // ---- STATE ------------------------------------------------------------------------------------------------------
    {
        mgr.setMixLocked(true);
        juce::ValueTree root("PARAMS");
        root.appendChild(juce::ValueTree("PRESET"), nullptr);          // a stale one: replaced, never duplicated
        mgr.writeState(root);
        int presets = 0;
        for (const auto& child : root) presets += child.hasType("PRESET") ? 1 : 0;
        P.eq("state.one_preset_child", presets, 1);
        const auto t = root.getChildWithName("PRESET");
        int attrs = 0, params = 0;
        for (const auto& child : t)
        {
            attrs += child.hasType("ATTR") ? 1 : 0;
            params += child.hasType("PARAM") ? 1 : 0;
        }
        P.eq("state.shape", params == 3 && attrs == 2 && static_cast<bool>(t.getProperty("mixLocked")), 1);

        // Through XML text, as a host stores it.
        const auto xml = root.createXml();
        const auto back = juce::ValueTree::fromXml(*xml);
        Processor other;
        Log log2(other);
        Product product2(other, log2);
        FP::PresetManager m2(other.apvts, product2.hooks());
        log2.take();
        const auto r0 = m2.revision();
        m2.readState(back);
        const auto a = mgr.current(), b = m2.current();
        bool paramsSame = a.params.size() == b.params.size();
        for (size_t i = 0; paramsSame && i < a.params.size(); ++i)
            paramsSame = a.params[i].id == b.params[i].id && sameBits(a.params[i].value, b.params[i].value);
        P.eq("state.identity", a.uuid == b.uuid && a.name == b.name && a.category == b.category, 1);
        P.eq("state.baseline_bit_exact", paramsSame, 1);
        P.eq("state.attributes", b.attributes.size() == 2 && b.attr("modeId") != nullptr
                                     && b.attr("modeId")->value == a.attr("modeId")->value, 1);
        P.eq("state.factory_from_bank", b.isFactory && b.author == "FunkGui" && b.notes == "fast", 1);
        P.eq("state.mix_lock", m2.mixLocked(), 1);
        P.eq("state.revision", static_cast<int64_t>(m2.revision() - r0), 1);
        P.eq("state.touches_no_parameter", log2.take().empty(), 1);

        // The bank decides: a saved factory flag for a uuid the bank no longer has is dropped.
        auto gone = back.createCopy();
        gone.getChildWithName("PRESET").setProperty("uuid", "00000000-0000-4000-8000-0000000000ff", nullptr);
        m2.readState(gone);
        P.eq("state.dropped_factory", m2.current().isFactory || m2.current().notes.isNotEmpty(), 0);

        // A session from before presets: unnamed, baseline = live values, lock off.
        other.setPlain("gain", -7.0f);
        m2.readState(juce::ValueTree("PARAMS"));
        const auto u = m2.current();
        P.eq("state.no_preset_unnamed", !u.isValid() && u.params.size() == 3 && !m2.mixLocked() && !m2.isModified(), 1);
        P.eq("state.no_preset_live", u.find("gain") != nullptr && sameBits(u.find("gain")->value, other.raw("gain")), 1);

        rev = m2.revision();
        m2.setCurrent(fet);
        P.eq("state.set_current", m2.revision() == rev + 1 && m2.current().uuid == fet.uuid, 1);
        mgr.setMixLocked(false);
    }

    // ---- DEFAULTS ---------------------------------------------------------------------------------------------------
    {
        Processor bare;
        FP::PresetManager m(bare.apvts, {});
        P.eq("defaults.every_parameter", m.isPresetParameter("gain") && m.isPresetParameter("mode")
                                             && m.isPresetParameter("bypass"), 1);
        P.eq("defaults.unnamed", !m.current().isValid() && m.current().params.size() == 5, 1);
        FP::Preset p;
        p.uuid = "a1b2c3d4-0000-4000-8000-000000000020";
        p.name = "Bare";
        p.params = { { "mode", 2.0f }, { "bypass", 1.0f }, { "gain", -6.0f } };
        m.apply(p);
        P.eq("defaults.apply", dynamic_cast<juce::AudioParameterChoice&>(bare.param("mode")).getIndex() == 2
                                   && sameBits(bare.raw("bypass"), 1.0f) && std::abs(bare.raw("gain") + 6.0f) < 1e-4f, 1);
        m.setMixLocked(true);                       // no mixParameter: the lock keeps nothing
        FP::Preset q = p;
        q.params = { { "mix", 0.25f } };
        m.apply(q);
        P.eq("defaults.lock_keeps_nothing", std::abs(bare.raw("mix") - 0.25f) <= 0.5e-4f, 1);
        P.eq("defaults.capture_no_extra", m.capture().attributes.empty(), 1);
        juce::ValueTree root("PARAMS");
        root.appendChild(juce::ValueTree("PRESET").setProperty("uuid", p.uuid, nullptr)
                             .setProperty("name", "Bare", nullptr).setProperty("isFactory", true, nullptr), nullptr);
        m.readState(root);
        P.eq("defaults.no_bank_no_factory", m.current().isFactory, 0);
    }

    return P.finish();
}
