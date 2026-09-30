#pragma once

// The preset database: every user preset, a synced copy of the factory bank (so search and tags cover it), the colour
// tags and their labels, and each preset's attributes. (HardwareReverb Source/presets/PresetStore.h; G8: the product
// comes from ProductConfig, and schema v2 adds the attributes.)
//
// SQLite, from the macOS system library. One connection per process, shared by every plugin instance in it: hold a
// juce::SharedResourcePointer of a product subclass that passes the product's configuration —
//
//   struct FcmpPresetStore : funkgui::presets::PresetStore { FcmpPresetStore() : PresetStore(kPresetConfig) {} };
//   juce::SharedResourcePointer<FcmpPresetStore> store;
//
// — so the database opens with the first editor and closes with the last, and a plugin instance with no editor never
// touches the disk. MESSAGE THREAD ONLY: no processor, no audio thread, no host program call ever reaches it.
//
// Several processes (two hosts, a host and the Standalone) share the file: WAL journalling and a busy timeout make
// that safe, and pollExternalChanges() lets an open browser notice another process's commit.
//
// Every mutation returns false and sets lastError() rather than throwing, and nothing here may crash the host: a file
// that cannot be opened, or fails its integrity check, is set aside and replaced; if no file can be written at all (a
// sandboxed host, a read-only home), the store runs on an in-memory database with the factory bank and says so through
// isPersistent() (K2 #19).

#include <funkgui/presets/PresetTypes.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace funkgui::presets
{
    class PresetStore
    {
    public:
        // Opens defaultLocation(config). An invalid configuration (ProductConfig::isValid) runs in memory.
        explicit PresetStore(const ProductConfig& config);
        ~PresetStore();

        // ~/Library/Application Support/<productName>/Presets.db (Linux, v0.11.0: $XDG_CONFIG_HOME, else ~/.config,
        // /<productName>/Presets.db), or the path in the environment variable
        // config.dbEnvVar when that is set and not empty — harnesses and frame captures point it at a scratch file so
        // they never read or write the real one. The variable is read on every call (each store construction), so a
        // harness can point successive stores at successive files. An invalid configuration gives juce::File().
        static juce::File defaultLocation(const ProductConfig& config);

        const ProductConfig& config() const;
        bool isOpen() const;
        bool isPersistent() const;
        juce::String lastError() const;
        juce::File file() const;

        // Bring the factory rows in line with the bank compiled into this build: upsert by uuid, delete factory rows
        // the bank no longer has. Tags on factory presets survive (they are keyed by uuid, in their own table). Cheap
        // when bankRevision matches what was last synced.
        void syncFactory(const std::vector<Preset>& bank, int bankRevision);

        std::vector<Preset> query(const Query&) const;
        std::optional<Preset> get(const juce::String& uuid) const;
        juce::StringArray categories() const;        // distinct, sorted, case-insensitive
        int count(Source) const;

        // User presets. isFactory is forced false on the way in.
        bool saveNew(Preset& p);                 // assigns uuid and timestamps; name made unique
        bool overwrite(const Preset& p);         // replace params, attributes + metadata of an existing user preset
        bool rename(const juce::String& uuid, const juce::String& newName);   // fails if the name is taken
        bool setCategory(const juce::String& uuid, const juce::String& category);
        bool setNotes(const juce::String& uuid, const juce::String& notes);
        bool remove(const juce::String& uuid);   // its tags go with it

        // Any preset, factory included.
        bool setTags(const juce::String& uuid, TagMask tags);
        void markUsed(const juce::String& uuid);

        juce::String tagLabel(Tag) const;        // the user's label, or defaultTagName()
        bool setTagLabel(Tag, const juce::String& label);   // empty restores the default

        // "Name", else "Name 2", "Name 3", ... against user AND factory names, case-insensitively; ignoreUuid lets a
        // preset keep its own name.
        juce::String uniqueName(const juce::String& wanted, const juce::String& ignoreUuid = {}) const;

        struct ImportResult
        {
            bool ok = false;
            juce::String uuid, name;
            bool renamed   = false;   // name was taken; a suffix was added
            bool duplicate = false;   // identical preset already present; nothing written
            juce::String error;
        };
        // A preset read from a file. Same uuid with different content (name, values or attributes), or a factory uuid,
        // is imported under a fresh uuid; the name is made unique.
        ImportResult importPreset(Preset p);

        // Bumped by every change this process makes, and by pollExternalChanges() when it sees another process's
        // commit. Editors compare it per frame.
        uint64_t revision() const;
        bool pollExternalChanges();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        JUCE_DECLARE_NON_COPYABLE(PresetStore)
    };
}
