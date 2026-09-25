// FUNKGUI_TEST name=fg.presets.file timeout=120 gpu=0 links=presets
//
// fg.presets.file: PresetFile (presets/PresetFile.h; FCompressor docs/design/01-core-contracts.md §9.2) — the XML shape
// comes from ProductConfig (root element, plugin="…", extension), ATTR children carry the attributes, tags and
// timestamps are never exported, floats round-trip bit for bit in any process locale, and every malformed file is
// refused with a message rather than loaded as zeros. The refusal list is HardwareReverb's PresetProbe FILES section,
// generalised. Spec rows only; every file lives in the test's sandbox (the working directory).

#include <funkgui/presets/PresetFile.h>
#include <funkgui/test/Harness.h>

#include <juce_core/juce_core.h>

#include <bit>
#include <cfloat>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
namespace FP = funkgui::presets;

namespace
{
    const FP::ProductConfig kConfig { "FunkGuiTest", ".fgtpreset", "FunkGuiTestPreset", "FUNKGUI_TEST_PRESETS_DB" };

    // Every non-ASCII literal goes through this: juce::String(const char*) assumes ASCII.
    juce::String U(const char* utf8) { return juce::String::fromUTF8(utf8); }

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    bool sameParams(const std::vector<FP::ParamValue>& a, const std::vector<FP::ParamValue>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].id != b[i].id || !sameBits(a[i].value, b[i].value)) return false;
        return true;
    }

    bool sameAttrs(const std::vector<FP::Attribute>& a, const std::vector<FP::Attribute>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].key != b[i].key || a[i].value != b[i].value) return false;
        return true;
    }

    FP::Preset samplePreset()
    {
        FP::Preset p;
        p.uuid      = "0f8fad5b-d9cb-469f-a165-70867728950e";
        p.name      = U("Äther \"Hall\" & <Room>");
        p.category  = "Hall";
        p.author    = "FunkGui";
        p.notes     = U("two lines\nwith\ttabs, 100% and ünïcødé");
        p.isFactory = true;                  // never carried by a file
        p.format    = FP::kFormat;
        p.params    = { { "thr", -24.0f },
                        { "ratio", 0.15f },
                        { "tiny", 7.038531e-26f },          // the one float strtod-then-narrow misses (HR, measured)
                        { "negzero", -0.0f },
                        { "denormal", std::numeric_limits<float>::denorm_min() },
                        { "max", FLT_MAX },
                        { "thr", 3.0f } };                  // a repeated id: the first wins
        p.createdMs = 1234567890123;
        p.modifiedMs = 1234567890124;
        p.lastUsedMs = 1234567890125;
        p.tags      = 0x15;
        p.attributes = { { "modeId", "fet-76" }, { "modeRev", "1" }, { "empty", "" }, { "text", U("ä \"q\" <&>") },
                         { "modeId", "clean" } };           // a repeated key: the first wins
        return p;
    }

    // The preset a file of samplePreset() must read back as.
    FP::Preset expectedFromFile()
    {
        FP::Preset p = samplePreset();
        p.isFactory = false;
        p.createdMs = p.modifiedMs = p.lastUsedMs = 0;
        p.tags = 0;
        p.params.pop_back();
        p.attributes.pop_back();
        return p;
    }

    bool samePreset(const FP::Preset& a, const FP::Preset& b)
    {
        return a.uuid == b.uuid && a.name == b.name && a.category == b.category && a.author == b.author
            && a.notes == b.notes && a.isFactory == b.isFactory && a.format == b.format && a.createdMs == b.createdMs
            && a.modifiedMs == b.modifiedMs && a.lastUsedMs == b.lastUsedMs && a.tags == b.tags
            && sameParams(a.params, b.params) && sameAttrs(a.attributes, b.attributes);
    }

    void refuse(T::Probe& P, std::string_view key, const juce::String& xml, const char* mustMention = nullptr)
    {
        juce::String error;
        const auto r = FP::PresetFile::fromXmlString(kConfig, xml, &error);
        const bool mentions = mustMention == nullptr || error.contains(mustMention);
        if (r.has_value() || error.isEmpty() || !mentions)
            std::printf("INFO     %.*s: loaded=%d error=\"%s\"\n", static_cast<int>(key.size()), key.data(),
                        r.has_value() ? 1 : 0, error.toRawUTF8());
        P.eq(key, !r.has_value() && error.isNotEmpty() && mentions, 1);
    }

    int64_t countChildren(const juce::XmlElement& x, const char* tag)
    {
        int64_t n = 0;
        for (auto* e : x.getChildWithTagNameIterator(tag))
            n += e != nullptr ? 1 : 0;
        return n;
    }

    juce::String wrap(const juce::String& attrs, const juce::String& children = {})
    {
        return "<FunkGuiTestPreset " + attrs + (children.isEmpty() ? juce::String("/>")
                                                                   : ">" + children + "</FunkGuiTestPreset>");
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.presets.file", "", argc, argv);

    const auto root = juce::File::getCurrentWorkingDirectory().getChildFile("presets_file");
    root.deleteRecursively();
    P.eq("sandbox.created", root.createDirectory().wasOk() ? 1 : 0, 1);

    // ---- ProductConfig ----------------------------------------------------------------------------------------------
    P.eq("config.valid", kConfig.isValid(), 1);
    {
        auto bad = [](auto edit) { FP::ProductConfig c = kConfig; edit(c); return c.isValid() ? 1 : 0; };
        P.eq("config.refuse.empty_name", bad([](FP::ProductConfig& c) { c.productName = ""; }), 0);
        P.eq("config.refuse.slash_name", bad([](FP::ProductConfig& c) { c.productName = "a/b"; }), 0);
        P.eq("config.refuse.dotdot_name", bad([](FP::ProductConfig& c) { c.productName = ".."; }), 0);
        P.eq("config.refuse.padded_name", bad([](FP::ProductConfig& c) { c.productName = " X"; }), 0);
        P.eq("config.refuse.ext_no_dot", bad([](FP::ProductConfig& c) { c.fileExtension = "fgt"; }), 0);
        P.eq("config.refuse.ext_space", bad([](FP::ProductConfig& c) { c.fileExtension = ".f g"; }), 0);
        P.eq("config.refuse.ext_dot_only", bad([](FP::ProductConfig& c) { c.fileExtension = "."; }), 0);
        P.eq("config.refuse.bad_root", bad([](FP::ProductConfig& c) { c.xmlRoot = "1Preset"; }), 0);
        P.eq("config.empty_env_ok", bad([](FP::ProductConfig& c) { c.dbEnvVar = ""; }), 1);

        FP::ProductConfig invalid = kConfig;
        invalid.xmlRoot = "not valid";
        juce::String error;
        P.eq("file.refuse.bad_config.to_xml", FP::PresetFile::toXmlString(invalid, samplePreset()).isEmpty(), 1);
        P.eq("file.refuse.bad_config.from_xml",
             !FP::PresetFile::fromXmlString(invalid, FP::PresetFile::toXmlString(kConfig, samplePreset()), &error)
                  && error.isNotEmpty(), 1);
        error.clear();
        P.eq("file.refuse.bad_config.write",
             !FP::PresetFile::write(invalid, samplePreset(), root.getChildFile("x.fgtpreset"), &error)
                  && error.isNotEmpty() && !root.getChildFile("x.fgtpreset").exists(), 1);
    }

    // ---- the shape --------------------------------------------------------------------------------------------------
    const auto xmlText = FP::PresetFile::toXmlString(kConfig, samplePreset());
    {
        const auto x = juce::XmlDocument::parse(xmlText);
        P.eq("shape.parses", x != nullptr, 1);
        if (x != nullptr)
        {
            P.eq("shape.root", x->hasTagName("FunkGuiTestPreset"), 1);
            P.eq("shape.plugin", x->getStringAttribute("plugin") == "FunkGuiTest", 1);
            P.eq("shape.format", x->getStringAttribute("format") == "1", 1);
            int personal = 0;
            for (const char* a : { "tags", "createdMs", "modifiedMs", "lastUsedMs", "created", "modified", "lastUsed",
                                   "isFactory", "factory" })
                personal += x->hasAttribute(a) ? 1 : 0;
            P.eq("shape.no_tags_or_timestamps", personal, 0);
            P.eq("shape.attr_count", countChildren(*x, "ATTR"), 4);
            P.eq("shape.param_count", countChildren(*x, "PARAM"), 6);
            P.eq("shape.attrs_first", x->getFirstChildElement() != nullptr
                                          && x->getFirstChildElement()->hasTagName("ATTR"), 1);
            P.eq("shape.first_attr_wins", x->getChildByAttribute("key", "modeId") != nullptr
                                              && x->getChildByAttribute("key", "modeId")->getStringAttribute("value")
                                                     == "fet-76", 1);
            P.eq("shape.first_param_wins", x->getChildByAttribute("id", "thr") != nullptr
                                               && x->getChildByAttribute("id", "thr")->getStringAttribute("value")
                                                      == "-24", 1);
        }
    }

    // ---- round trip -------------------------------------------------------------------------------------------------
    {
        juce::String error;
        const auto back = FP::PresetFile::fromXmlString(kConfig, xmlText, &error);
        P.eq("roundtrip.string.loads", back.has_value(), 1);
        P.eq("roundtrip.string.identical", back.has_value() && samePreset(*back, expectedFromFile()), 1);
        P.eq("roundtrip.string.no_tags", back.has_value() && back->tags == 0, 1);
        P.eq("roundtrip.string.no_timestamps",
             back.has_value() && back->createdMs == 0 && back->modifiedMs == 0 && back->lastUsedMs == 0, 1);
        P.eq("roundtrip.string.not_factory", back.has_value() && !back->isFactory, 1);
        P.eq("roundtrip.string.attr_empty_value", back.has_value() && back->attr("empty") != nullptr
                                                      && back->attr("empty")->value.isEmpty(), 1);

        const auto file = root.getChildFile(FP::PresetFile::safeFileName(samplePreset().name) + kConfig.fileExtension);
        error.clear();
        P.eq("roundtrip.file.write", FP::PresetFile::write(kConfig, samplePreset(), file, &error), 1);
        P.eq("roundtrip.file.extension", file.hasFileExtension(".fgtpreset"), 1);
        const auto fromFile = FP::PresetFile::read(kConfig, file, &error);
        P.eq("roundtrip.file.identical", fromFile.has_value() && samePreset(*fromFile, expectedFromFile()), 1);
        // Overwriting in place: replaceWithText, never a half-written file under the user's name.
        FP::Preset renamed = samplePreset();
        renamed.notes = "second";
        P.eq("roundtrip.file.overwrite", FP::PresetFile::write(kConfig, renamed, file, &error)
                                             && FP::PresetFile::read(kConfig, file)->notes == "second", 1);

        // Every float of a sweep round-trips bit for bit (to_chars shortest, strtof in the "C" locale).
        FP::Preset sweep;
        sweep.name = "sweep";
        uint32_t bits = 0x00000001u;
        int n = 0;
        for (; bits < 0x7f800000u; bits += 0x000fff1du, ++n)
            sweep.params.push_back({ "p" + juce::String(n), std::bit_cast<float>(bits) });
        const auto sweepBack = FP::PresetFile::fromXmlString(kConfig, FP::PresetFile::toXmlString(kConfig, sweep));
        P.eq("roundtrip.float_sweep", sweepBack.has_value() && sameParams(sweepBack->params, sweep.params), 1);
        P.ge("roundtrip.float_sweep.count", static_cast<double>(n), 2000.0);

        // The process locale does not reach the numbers: a host running under de_DE writes and reads "0.15".
        const char* de = std::setlocale(LC_ALL, "de_DE.UTF-8");
        if (de == nullptr) de = std::setlocale(LC_ALL, "de_DE");
        std::printf("INFO     locale for the de_DE rows: %s\n", de != nullptr ? de : "unavailable (C locale)");
        const auto deText = FP::PresetFile::toXmlString(kConfig, samplePreset());
        const auto deBack = FP::PresetFile::fromXmlString(kConfig, deText);
        std::setlocale(LC_ALL, "C");
        P.eq("locale.de.text_uses_dot", deText.contains("value=\"0.15\""), 1);
        P.eq("locale.de.roundtrip", deBack.has_value() && samePreset(*deBack, expectedFromFile()), 1);
    }

    // ---- accepted variations ----------------------------------------------------------------------------------------
    {
        const auto noPlugin = FP::PresetFile::fromXmlString(
            kConfig, wrap("format=\"1\" name=\"Hand\"", "<PARAM id=\"thr\" value=\"-3\"/>"));
        P.eq("accept.no_plugin_attribute", noPlugin.has_value() && noPlugin->find("thr") != nullptr, 1);

        const auto future = FP::PresetFile::fromXmlString(
            kConfig, wrap("format=\"1\" plugin=\"FunkGuiTest\" name=\"Has Future\"",
                          "<PARAM id=\"future_param\" value=\"0.9\"/><FUTURE x=\"1\"/><ATTR key=\"k\" value=\"v\"/>"));
        P.eq("accept.unknown_param_kept", future.has_value() && future->find("future_param") != nullptr, 1);
        P.eq("accept.unknown_child_skipped", future.has_value() && future->params.size() == 1
                                                 && future->attributes.size() == 1, 1);

        const auto newer = FP::PresetFile::fromXmlString(kConfig, wrap("format=\"2\" plugin=\"FunkGuiTest\" name=\"Future\""));
        P.eq("accept.newer_format_read_not_judged", newer.has_value() && newer->format == 2, 1);

        const auto spaced = FP::PresetFile::fromXmlString(
            kConfig, wrap("format=\" 1 \" name=\"  Padded  \" uuid=\" abc \"",
                          "<ATTR key=\" modeId \" value=\" keep spaces \"/><PARAM id=\" thr \" value=\" -3 \"/>"));
        P.eq("accept.trimmed_fields", spaced.has_value() && spaced->name == "Padded" && spaced->uuid == "abc"
                                          && spaced->find("thr") != nullptr, 1);
        P.eq("accept.attr_key_trimmed_value_kept", spaced.has_value() && spaced->attr("modeId") != nullptr
                                                       && spaced->attr("modeId")->value == " keep spaces ", 1);
    }

    // ---- refusals ---------------------------------------------------------------------------------------------------
    refuse(P, "refuse.other_plugin", wrap("format=\"1\" plugin=\"OtherVerb\" name=\"x\""), "OtherVerb");
    refuse(P, "refuse.other_root", "<OtherProductPreset format=\"1\" name=\"x\"/>", "FunkGuiTest");
    refuse(P, "refuse.no_format", wrap("plugin=\"FunkGuiTest\" name=\"x\""));
    refuse(P, "refuse.format_zero", wrap("format=\"0\" name=\"x\""));
    refuse(P, "refuse.format_text", wrap("format=\"one\" name=\"x\""));
    refuse(P, "refuse.no_name", wrap("format=\"1\" name=\"   \""));
    refuse(P, "refuse.nan", wrap("format=\"1\" name=\"x\"", "<PARAM id=\"size\" value=\"nan\"/>"));
    refuse(P, "refuse.inf", wrap("format=\"1\" name=\"x\"", "<PARAM id=\"size\" value=\"inf\"/>"));
    refuse(P, "refuse.overflow", wrap("format=\"1\" name=\"x\"", "<PARAM id=\"size\" value=\"1e39\"/>"));
    refuse(P, "refuse.not_a_number", wrap("format=\"1\" name=\"x\"", "<PARAM id=\"size\" value=\"abc\"/>"));
    refuse(P, "refuse.trailing_junk", wrap("format=\"1\" name=\"x\"", "<PARAM id=\"size\" value=\"0.5x\"/>"));
    refuse(P, "refuse.no_value", wrap("format=\"1\" name=\"x\"", "<PARAM id=\"size\"/>"));
    refuse(P, "refuse.no_id", wrap("format=\"1\" name=\"x\"", "<PARAM value=\"0.5\"/>"));
    refuse(P, "refuse.attr_no_key", wrap("format=\"1\" name=\"x\"", "<ATTR value=\"v\"/>"));
    refuse(P, "refuse.attr_blank_key", wrap("format=\"1\" name=\"x\"", "<ATTR key=\"  \" value=\"v\"/>"));
    refuse(P, "refuse.attr_no_value", wrap("format=\"1\" name=\"x\"", "<ATTR key=\"modeId\"/>"), "modeId");
    refuse(P, "refuse.dtd", "<!DOCTYPE FunkGuiTestPreset [<!ENTITY c \"cc\">]>"
                            "<FunkGuiTestPreset format=\"1\" name=\"&c;\"/>");
    refuse(P, "refuse.truncated", "<FunkGuiTestPreset format=\"1\" name=\"x\"><PARAM id=\"size\" val");
    refuse(P, "refuse.not_xml", "this is not xml at all");
    refuse(P, "refuse.too_large", wrap("format=\"1\" name=\"x\" notes=\""
                                       + juce::String::repeatedString("n", 300 * 1024) + "\""), "too large");

    // ---- write and read refusals ------------------------------------------------------------------------------------
    {
        juce::String error;
        FP::Preset nameless = samplePreset();
        nameless.name = "  ";
        P.eq("write.refuse.no_name", !FP::PresetFile::write(kConfig, nameless, root.getChildFile("n.fgtpreset"), &error)
                                         && error.isNotEmpty(), 1);
        FP::Preset nan = samplePreset();
        nan.params.push_back({ "bad", std::numeric_limits<float>::quiet_NaN() });
        error.clear();
        P.eq("write.refuse.nan", !FP::PresetFile::write(kConfig, nan, root.getChildFile("nan.fgtpreset"), &error)
                                     && error.contains("bad") && !root.getChildFile("nan.fgtpreset").exists(), 1);
        FP::Preset noUuid = samplePreset();
        noUuid.uuid = {};
        P.eq("write.no_uuid_ok", FP::PresetFile::write(kConfig, noUuid, root.getChildFile("nouuid.fgtpreset")), 1);

        error.clear();
        P.eq("read.refuse.missing", !FP::PresetFile::read(kConfig, root.getChildFile("missing.fgtpreset"), &error)
                                        && error.isNotEmpty(), 1);
        const auto big = root.getChildFile("big.fgtpreset");
        big.replaceWithText(juce::String::repeatedString("x", 300 * 1024));
        error.clear();
        P.eq("read.refuse.too_large", !FP::PresetFile::read(kConfig, big, &error) && error.contains("too large"), 1);
    }

    // ---- safeFileName -----------------------------------------------------------------------------------------------
    {
        auto ok = [](const juce::String& s)
        {
            return s.isNotEmpty() && !s.containsAnyOf("/\\:*?\"<>|") && !s.startsWithChar('.') && s.length() <= 100
                && s == s.trim();
        };
        P.eq("safe.plain", FP::PresetFile::safeFileName("Big Hall") == "Big Hall", 1);
        P.eq("safe.separators", ok(FP::PresetFile::safeFileName("a/b\\c:d*e?f\"g<h>i|j")), 1);
        P.eq("safe.leading_dots", FP::PresetFile::safeFileName("..hidden") == "hidden", 1);
        P.eq("safe.empty", FP::PresetFile::safeFileName("   ") == "Preset", 1);
        P.eq("safe.control_chars", FP::PresetFile::safeFileName(juce::String("a") + juce::String::charToString(7) + "b")
                                       == "ab", 1);
        P.eq("safe.long", FP::PresetFile::safeFileName(juce::String::repeatedString("x", 300)).length() == 100, 1);
        P.eq("safe.unicode_kept", FP::PresetFile::safeFileName(U("Äther")) == U("Äther"), 1);
    }

    root.deleteRecursively();
    return P.finish();
}
