#include <funkgui/presets/PresetFile.h>

#include "Platform.h"

#include <charconv>
#include <cmath>
#include <cstdlib>

// The shape, written by this build (<Root> = ProductConfig::xmlRoot, <Product> = its productName):
//
//   <?xml version="1.0" encoding="UTF-8"?>
//   <Root format="1" plugin="Product" uuid="..." name="..." category="..." author="..." notes="...">
//     <ATTR key="modeId" value="fet-76"/>
//     ...
//     <PARAM id="size" value="0.7"/>
//     ...
//   </Root>
//
// Tested by fg.presets.file (round trip, every refusal below); HardwareReverb's PresetProbe held the same rows.
// (HardwareReverb Source/presets/PresetFile.cpp; G8: ProductConfig, ATTR.)

namespace funkgui::presets::PresetFile
{
    namespace
    {
        using detail::strtofC;

        constexpr const char* kParamTag  = "PARAM";
        constexpr const char* kAttrTag   = "ATTR";

        // A preset with every parameter is under 1 KB; notes are the only
        // free-length field. 256 KB leaves room for a novel in the notes and
        // still refuses the wrong file (a WAV, a session) before reading it
        // all into memory and handing it to the XML parser.
        constexpr juce::int64 kMaxBytes = 256 * 1024;

        // Numbers are written and read in the "C" locale whatever the host
        // set: snprintf("%g", 0.15) under de_DE prints "0,15" (measured), and
        // a file exported in Berlin must import in Boston. std::to_chars is
        // shortest-exact and locale-free; strtof_l reads that text back to the
        // identical float for all 2^31 positive finite floats (measured
        // exhaustively; the strtod-then-narrow path juce's getDoubleAttribute
        // takes misses one, 7.038531e-26, by double rounding). strtofC
        // (Platform.h) is that read on both operating systems.

        juce::String formatFloat(float v)
        {
            char buf[32];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v);
            return juce::String(buf, (size_t) (r.ptr - buf));
        }

        // Strict: the whole attribute must be one number and the number must
        // be finite as a float. getDoubleAttribute would read "abc" as 0 and
        // "1e39" as inf without a word, and a preset that silently loads a
        // knob at zero is worse than one that refuses to load.
        bool parseFloat(const juce::String& text, float& out)
        {
            const auto t = text.trim();
            if (t.isEmpty()) return false;
            const char* s = t.toRawUTF8();
            char* end = nullptr;
            const float v = strtofC(s, &end);
            if (end == s || *end != 0 || !std::isfinite(v)) return false;
            out = v;
            return true;
        }

        void setError(juce::String* error, const juce::String& what)
        {
            if (error != nullptr) *error = what;
        }

        // "not a FCompressor preset (...)": every refusal names the product the file was expected to be for.
        juce::String notA(const ProductConfig& c, const juce::String& why = {})
        {
            return "not a " + c.productName + " preset" + (why.isNotEmpty() ? " (" + why + ")" : juce::String());
        }

        constexpr const char* kBadConfig = "invalid preset product configuration";
    }

    juce::String toXmlString(const ProductConfig& config, const Preset& p)
    {
        if (!config.isValid()) return {};

        juce::XmlElement root(config.xmlRoot);
        root.setAttribute("format", p.format);
        root.setAttribute("plugin", config.productName);
        root.setAttribute("uuid", p.uuid);
        root.setAttribute("name", p.name);
        root.setAttribute("category", p.category);
        root.setAttribute("author", p.author);
        root.setAttribute("notes", p.notes);

        // The attributes first (01 §9.2's shape), first occurrence of a key only, empty keys never.
        juce::StringArray seenKeys;
        for (const auto& a : p.attributes)
        {
            if (a.key.isEmpty() || seenKeys.contains(a.key)) continue;
            seenKeys.add(a.key);
            auto* e = root.createNewChildElement(kAttrTag);
            e->setAttribute("key", a.key);
            e->setAttribute("value", a.value);
        }

        // First occurrence of an id wins (what Preset::find returns); a
        // non-finite value has no text form a reader would accept, so it is
        // left out — write() refuses such a preset before it gets here.
        juce::StringArray seen;
        for (const auto& v : p.params)
        {
            if (v.id.isEmpty() || !std::isfinite(v.value) || seen.contains(v.id)) continue;
            seen.add(v.id);
            auto* e = root.createNewChildElement(kParamTag);
            e->setAttribute("id", v.id);
            e->setAttribute("value", formatFloat(v.value));
        }
        return root.toString();
    }

    std::optional<Preset> fromXmlString(const ProductConfig& config, const juce::String& text, juce::String* error)
    {
        if (!config.isValid())
        {
            setError(error, kBadConfig);
            return std::nullopt;
        }
        if ((juce::int64) text.getNumBytesAsUTF8() > kMaxBytes)
        {
            setError(error, "too large to be a " + config.productName + " preset");
            return std::nullopt;
        }

        // No DTD, ever: juce::XmlDocument expands internal <!ENTITY>
        // definitions recursively (juce_XmlDocument.cpp, expandExternalEntity),
        // so a 1 KB "billion laughs" file would expand without bound inside
        // the host's process. This format never declares one.
        if (text.containsIgnoreCase("<!DOCTYPE") || text.containsIgnoreCase("<!ENTITY"))
        {
            setError(error, notA(config, "it declares a DTD"));
            return std::nullopt;
        }

        juce::XmlDocument doc(text);
        const auto x = doc.getDocumentElement();
        if (x == nullptr)
        {
            setError(error, notA(config, doc.getLastParseError().isNotEmpty() ? doc.getLastParseError()
                                                                              : juce::String("not XML")));
            return std::nullopt;
        }
        if (!x->hasTagName(config.xmlRoot))
        {
            setError(error, notA(config, "<" + x->getTagName() + ">"));
            return std::nullopt;
        }
        // Absent is accepted (a hand-written file); present and different is
        // another product's file that happens to share the root tag.
        if (x->hasAttribute("plugin") && x->getStringAttribute("plugin") != config.productName)
        {
            setError(error, "a preset for " + x->getStringAttribute("plugin") + ", not " + config.productName);
            return std::nullopt;
        }

        // The format is read, not judged: PresetStore::importPreset refuses a
        // newer one with the message that names the reason. Here it only has
        // to be a positive integer.
        const auto fmt = x->getStringAttribute("format").trim();
        if (fmt.isEmpty() || !fmt.containsOnly("0123456789") || fmt.length() > 6 || fmt.getIntValue() < 1)
        {
            setError(error, notA(config, "no format"));
            return std::nullopt;
        }

        Preset p;
        p.format    = fmt.getIntValue();
        p.uuid      = x->getStringAttribute("uuid").trim();
        p.name      = x->getStringAttribute("name").trim();
        p.category  = x->getStringAttribute("category").trim();
        p.author    = x->getStringAttribute("author").trim();
        p.notes     = x->getStringAttribute("notes");
        p.isFactory = false;           // a file is always the user's
        p.tags      = 0;               // never carried in a file

        if (p.name.isEmpty())
        {
            setError(error, "the preset has no name");
            return std::nullopt;
        }

        // Attributes: a key is required and so is the value attribute (the empty string is a value); the first of a
        // repeated key wins, as for parameters. Unknown keys are kept: they are the product's, not ours.
        for (auto* e : x->getChildWithTagNameIterator(kAttrTag))
        {
            const auto key = e->getStringAttribute("key").trim();
            if (key.isEmpty())
            {
                setError(error, "an attribute has no key");
                return std::nullopt;
            }
            if (!e->hasAttribute("value"))
            {
                setError(error, "attribute \"" + key + "\" has no value");
                return std::nullopt;
            }
            if (p.attr(key) == nullptr)
                p.attributes.push_back({ key, e->getStringAttribute("value") });
        }

        // Unknown ids are KEPT: they are parameters of a newer build, and
        // PresetManager ignores what it does not know. Other child elements
        // are skipped for the same reason (a newer format's additions).
        for (auto* e : x->getChildWithTagNameIterator(kParamTag))
        {
            const auto id = e->getStringAttribute("id").trim();
            float v = 0.0f;
            if (id.isEmpty() || !e->hasAttribute("value") || !parseFloat(e->getStringAttribute("value"), v))
            {
                setError(error, "parameter \"" + id + "\" has no usable value (\""
                                    + e->getStringAttribute("value") + "\")");
                return std::nullopt;
            }
            if (p.find(id) == nullptr)
                p.params.push_back({ id, v });
        }
        return p;
    }

    bool write(const ProductConfig& config, const Preset& p, const juce::File& f, juce::String* error)
    {
        if (!config.isValid())
        {
            setError(error, kBadConfig);
            return false;
        }
        // The name only, not isValid(): a uuid is optional in a file (the
        // importing store assigns one), so a preset that has none yet can
        // still be exported.
        if (p.name.trim().isEmpty())
        {
            setError(error, "the preset has no name");
            return false;
        }
        for (const auto& v : p.params)
            if (!std::isfinite(v.value))
            {
                setError(error, "parameter \"" + v.id + "\" has a value that is not a number");
                return false;
            }
        if (!f.getParentDirectory().createDirectory())
        {
            setError(error, "cannot create " + f.getParentDirectory().getFullPathName());
            return false;
        }
        // replaceWithText writes a temporary sibling and renames it over the
        // target, so an interrupted export never leaves half a file under
        // the name the user chose.
        if (!f.replaceWithText(toXmlString(config, p), false, false, "\n"))
        {
            setError(error, "cannot write " + f.getFullPathName());
            return false;
        }
        return true;
    }

    std::optional<Preset> read(const ProductConfig& config, const juce::File& f, juce::String* error)
    {
        if (!config.isValid())
        {
            setError(error, kBadConfig);
            return std::nullopt;
        }
        if (!f.existsAsFile())
        {
            setError(error, "no such file: " + f.getFullPathName());
            return std::nullopt;
        }
        // Checked before reading: the cap exists so a wrong file is not
        // loaded whole.
        if (f.getSize() > kMaxBytes)
        {
            setError(error, f.getFileName() + " is too large to be a " + config.productName + " preset");
            return std::nullopt;
        }
        return fromXmlString(config, f.loadFileAsString(), error);
    }

    juce::String safeFileName(const juce::String& presetName)
    {
        juce::String s;
        for (auto c : presetName)
            if (c >= 0x20 && c != 0x7f) s += juce::String::charToString(c);

        // createLegalFileName strips the characters macOS and Windows refuse
        // ("#@,;:<>*^|?\/). A leading dot would make the export invisible in
        // Finder; 100 characters leaves room for " 2" and the extension
        // under the 255-byte name limit even when every character is 2 bytes.
        s = juce::File::createLegalFileName(s.trim()).trim();
        while (s.startsWithChar('.')) s = s.substring(1).trimStart();
        if (s.length() > 100) s = s.substring(0, 100).trimEnd();
        return s.isNotEmpty() ? s : juce::String("Preset");
    }
}
