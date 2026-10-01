// FUNKGUI_TEST name=fg.presets.store timeout=300 gpu=0 links=presets
//
// fg.presets.store: PresetStore (presets/PresetStore.h; FCompressor docs/design/01-core-contracts.md §9.2, K2 #19) —
//   ENV       ProductConfig::dbEnvVar replaces the database path, read afresh by every store; unset or empty gives
//             ~/Library/Application Support/<productName>/Presets.db (Linux: ~/.config/<productName>/Presets.db),
//             which this test never opens;
//   ROUND     every field of a user and a factory preset, attributes included, survives save, overwrite, the
//             mutations and a reopen; floats bit for bit; revision() moves once per change and never on a refusal;
//   WAL       the file is schema v2 in WAL mode, a second connection sees commits, pollExternalChanges() sees another
//             connection's commit once, syncFactory is idempotent and keeps tags across a bank change;
//   FALLBACK  an unwritable folder or an invalid ProductConfig runs in memory; a corrupt file is set aside and
//             replaced; a read-only file (v2, or HardwareReverb's v1) is copied into memory and never modified; a v1
//             file is migrated to v2 in place; a newer file is used or left untouched per schema_min_reader.
// HardwareReverb's PresetProbe held the v1 store to the same rules. Spec rows only; every file lives in the sandbox.

#include <funkgui/presets/PresetFile.h>
#include <funkgui/presets/PresetStore.h>
#include <funkgui/test/Harness.h>

#include <juce_core/juce_core.h>

#include <sqlite3.h>
#include <sys/stat.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
namespace FP = funkgui::presets;

namespace
{
    constexpr const char* kEnv = "FUNKGUI_TEST_PRESETS_DB";
    const FP::ProductConfig kConfig { "FunkGuiTest", ".fgtpreset", "FunkGuiTestPreset", kEnv };

    juce::String U(const char* utf8) { return juce::String::fromUTF8(utf8); }

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    // As sets: the store keeps the first of a repeated id or key and does not promise an order.
    bool sameParams(const std::vector<FP::ParamValue>& a, const std::vector<FP::ParamValue>& b)
    {
        if (a.size() != b.size()) return false;
        for (const auto& x : a)
        {
            bool found = false;
            for (const auto& y : b)
                if (x.id == y.id) { found = sameBits(x.value, y.value); break; }
            if (!found) return false;
        }
        return true;
    }

    bool sameAttrs(const std::vector<FP::Attribute>& a, const std::vector<FP::Attribute>& b)
    {
        if (a.size() != b.size()) return false;
        for (const auto& x : a)
        {
            bool found = false;
            for (const auto& y : b)
                if (x.key == y.key) { found = x.value == y.value; break; }
            if (!found) return false;
        }
        return true;
    }

    std::vector<FP::Preset> bank(int revision)
    {
        FP::Preset init;
        init.uuid = "00000000-0000-4000-8000-000000000001";
        init.name = "Init";
        init.author = "FunkGui";
        init.isFactory = true;
        init.attributes = { { "modeId", "clean" } };

        FP::Preset hall;
        hall.uuid = "00000000-0000-4000-8000-000000000002";
        hall.name = "Big Hall";
        hall.category = "Hall";
        hall.author = "FunkGui";
        hall.notes = "wide";
        hall.isFactory = true;
        hall.params = { { "thr", revision == 1 ? -24.0f : -20.0f }, { "ratio", 4.0f } };
        hall.attributes = { { "modeId", "fet-76" }, { "modeRev", "1" } };

        FP::Preset plate;
        plate.uuid = "00000000-0000-4000-8000-000000000003";
        plate.name = "Plate";
        plate.category = "Plate";
        plate.isFactory = true;
        plate.params = { { "thr", -10.0f } };

        FP::Preset added;
        added.uuid = "00000000-0000-4000-8000-000000000004";
        added.name = "Added";
        added.category = "hall";                    // folds with the bank's "Hall"
        added.isFactory = true;

        if (revision == 1) return { init, hall, plate };
        return { init, hall, added };                // revision 2: Plate removed, Added added, Big Hall changed
    }

    // ---- raw SQLite: what another process sees ----------------------------------------------------------------------
    struct Raw
    {
        sqlite3* db = nullptr;
        Raw(const juce::File& f, bool writable)
        {
            if (sqlite3_open_v2(f.getFullPathName().toRawUTF8(), &db,
                                writable ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY,
                                nullptr) != SQLITE_OK)
            {
                sqlite3_close_v2(db);
                db = nullptr;
            }
            else
                sqlite3_busy_timeout(db, 2000);
        }
        ~Raw() { if (db != nullptr) sqlite3_close_v2(db); }
        Raw(const Raw&) = delete;
        Raw& operator=(const Raw&) = delete;

        bool exec(const std::string& sql) const
        {
            return db != nullptr && sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
        }
        int64_t integer(const char* sql) const
        {
            int64_t v = -1;
            sqlite3_stmt* s = nullptr;
            if (db != nullptr && sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
                v = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
            return v;
        }
        std::string text(const char* sql) const
        {
            std::string v;
            sqlite3_stmt* s = nullptr;
            if (db != nullptr && sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW
                && sqlite3_column_text(s, 0) != nullptr)
                v = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
            sqlite3_finalize(s);
            return v;
        }
    };

    constexpr const char* kHrUuid = "6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a4b";

    // A database exactly as HardwareReverb's v1 store (Source/presets/PresetStore.cpp, toV1) leaves it, with one user
    // preset carrying a tag: the file a user of HardwareReverb has today.
    bool makeV1(const juce::File& f)
    {
        f.getParentDirectory().createDirectory();
        f.deleteFile();
        Raw r(f, true);
        const char* steps[] = {
            "CREATE TABLE preset ("
            "  uuid         TEXT PRIMARY KEY NOT NULL,"
            "  name         TEXT NOT NULL,"
            "  name_key     TEXT NOT NULL,"
            "  sort_key     TEXT NOT NULL DEFAULT '',"
            "  category     TEXT NOT NULL DEFAULT '',"
            "  category_key TEXT NOT NULL DEFAULT '',"
            "  author       TEXT NOT NULL DEFAULT '',"
            "  notes        TEXT NOT NULL DEFAULT '',"
            "  search_key   TEXT NOT NULL DEFAULT '',"
            "  is_factory   INTEGER NOT NULL DEFAULT 0 CHECK (is_factory IN (0, 1)),"
            "  format       INTEGER NOT NULL DEFAULT 1,"
            "  params       TEXT NOT NULL DEFAULT '{}',"
            "  created_ms   INTEGER NOT NULL DEFAULT 0,"
            "  modified_ms  INTEGER NOT NULL DEFAULT 0,"
            "  last_used_ms INTEGER NOT NULL DEFAULT 0)",
            "CREATE UNIQUE INDEX preset_user_name ON preset(name_key) WHERE is_factory = 0",
            "CREATE INDEX preset_name ON preset(name_key)",
            "CREATE INDEX preset_sort ON preset(sort_key, name_key, uuid)",
            "CREATE INDEX preset_category ON preset(category_key)",
            "CREATE TABLE tag ("
            "  preset_uuid TEXT NOT NULL REFERENCES preset(uuid) ON DELETE CASCADE ON UPDATE CASCADE,"
            "  color       INTEGER NOT NULL CHECK (color BETWEEN 0 AND 6),"
            "  PRIMARY KEY (preset_uuid, color)) WITHOUT ROWID",
            "CREATE INDEX tag_color ON tag(color)",
            "CREATE TABLE tag_label ("
            "  color INTEGER PRIMARY KEY CHECK (color BETWEEN 0 AND 6),"
            "  label TEXT NOT NULL)",
            "CREATE TABLE meta (key TEXT PRIMARY KEY NOT NULL, value TEXT) WITHOUT ROWID",
            "INSERT OR REPLACE INTO meta(key, value) VALUES ('schema_min_reader', '1')",
            "PRAGMA user_version = 1",
            "PRAGMA journal_mode=WAL",
            "INSERT INTO preset (uuid, name, name_key, sort_key, category, category_key, author, notes, search_key,"
            " is_factory, format, params, created_ms, modified_ms, last_used_ms) VALUES ("
            " '6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a4b', 'Old Hall', 'old hall', 'old hall', 'Hall', 'hall', 'HR', '',"
            " 'old hall' || char(31) || 'hall' || char(31) || 'hr' || char(31), 0, 1, '{\"size\":0.5,\"decay\":0.25}',"
            " 1000, 1000, 0)",
            "INSERT INTO tag (preset_uuid, color) VALUES ('6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a4b', 2)",
        };
        bool ok = r.db != nullptr;
        for (const char* s : steps) ok = ok && r.exec(s);
        return ok;
    }

    juce::MemoryBlock bytesOf(const juce::File& f)
    {
        juce::MemoryBlock m;
        f.loadFileAsData(m);
        return m;
    }

    bool setMode(const juce::File& f, int mode) { return ::chmod(f.getFullPathName().toRawUTF8(), static_cast<mode_t>(mode)) == 0; }

    void useDb(const juce::File& f) { T::setEnv(kEnv, f.getFullPathName().toRawUTF8()); }

    bool canonicalUuid(const juce::String& s)
    {
        return s.length() == 36 && s.containsOnly("0123456789abcdef-") && s == juce::Uuid(s).toDashedString();
    }

    std::vector<juce::String> names(const std::vector<FP::Preset>& v)
    {
        std::vector<juce::String> out;
        for (const auto& p : v) out.push_back(p.name);
        return out;
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.presets.store", "", argc, argv);

    const auto root = juce::File::getCurrentWorkingDirectory().getChildFile("presets_store");
    // A previous run that stopped half-way may have left a folder read-only.
    for (const auto* sub : { "locked", "ro", "v1ro" })
        if (root.getChildFile(sub).exists()) setMode(root.getChildFile(sub), 0700);
    for (const auto* f : { "ro/Presets.db", "v1ro/Presets.db" })
        if (root.getChildFile(f).exists()) setMode(root.getChildFile(f), 0600);
    root.deleteRecursively();
    P.eq("sandbox.created", root.createDirectory().wasOk() ? 1 : 0, 1);

    // =================================================================================================================
    // ENV
    // =================================================================================================================
    T::unsetEnv(kEnv);
    // ~/Library/Application Support/FunkGuiTest/ on macOS; on Linux (v0.11.0) the XDG configuration directory.
   #if JUCE_LINUX || JUCE_BSD
    const auto realDefault = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                 .getChildFile("FunkGuiTest/Presets.db");
   #else
    const auto realDefault = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                 .getChildFile("Application Support/FunkGuiTest/Presets.db");
   #endif
    const bool realFolderBefore = realDefault.getParentDirectory().exists();
    P.eq("env.unset_is_default", FP::PresetStore::defaultLocation(kConfig) == realDefault, 1);
    T::setEnv(kEnv, "");
    P.eq("env.empty_is_default", FP::PresetStore::defaultLocation(kConfig) == realDefault, 1);

    const auto envA = root.getChildFile("env/a/deep/Presets.db");
    useDb(envA);
    P.eq("env.override_path", FP::PresetStore::defaultLocation(kConfig) == envA, 1);
    {
        FP::PresetStore s(kConfig);
        P.eq("env.override_opens_there", s.isPersistent() && s.file() == envA && envA.existsAsFile(), 1);
        P.eq("env.config_kept", s.config().productName == "FunkGuiTest" && s.config().dbEnvVar == kEnv, 1);
    }
    const auto envB = root.getChildFile("env/b/Presets.db");
    useDb(envB);
    {
        FP::PresetStore s(kConfig);
        P.eq("env.read_per_store", s.file() == envB && envB.existsAsFile(), 1);
    }
    T::setEnv(kEnv, "rel/Presets.db");
    P.eq("env.relative_to_cwd", FP::PresetStore::defaultLocation(kConfig)
                                    == juce::File::getCurrentWorkingDirectory().getChildFile("rel/Presets.db"), 1);
    {
        FP::ProductConfig none = kConfig;
        none.dbEnvVar = {};
        P.eq("env.none_configured_is_default", FP::PresetStore::defaultLocation(none) == realDefault, 1);
        FP::ProductConfig other = kConfig;
        other.dbEnvVar = "FUNKGUI_TEST_OTHER_PRESETS_DB";
        T::unsetEnv("FUNKGUI_TEST_OTHER_PRESETS_DB");
        P.eq("env.only_its_own_variable", FP::PresetStore::defaultLocation(other) == realDefault, 1);
        FP::ProductConfig product2 = kConfig;
        product2.productName = "FunkGuiTest2";
        product2.dbEnvVar = {};
        P.eq("env.default_per_product", FP::PresetStore::defaultLocation(product2).getParentDirectory().getFileName()
                                            == "FunkGuiTest2", 1);
    }

    // =================================================================================================================
    // ROUND TRIP
    // =================================================================================================================
    const auto mainDb = root.getChildFile("main/Presets.db");
    useDb(mainDb);
    juce::String userUuid, secondUuid;
    FP::Preset saved;
    {
        FP::PresetStore s(kConfig);
        P.eq("store.persistent", s.isOpen() && s.isPersistent(), 1);
        P.eq("store.no_error", s.lastError().isEmpty(), 1);
        P.eq("store.file", s.file() == mainDb, 1);
        P.eq("store.empty", s.count(FP::Source::all), 0);

        auto rev = s.revision();
        s.syncFactory(bank(1), 1);
        P.eq("factory.sync_count", s.count(FP::Source::factory), 3);
        P.eq("factory.sync_revision", static_cast<int64_t>(s.revision() - rev), 1);
        const auto hall = s.get(bank(1)[1].uuid);
        P.eq("factory.fields", hall.has_value() && hall->isFactory && hall->name == "Big Hall" && hall->notes == "wide"
                                   && hall->author == "FunkGui" && hall->createdMs == 0, 1);
        P.eq("factory.params", hall.has_value() && sameParams(hall->params, bank(1)[1].params), 1);
        P.eq("factory.attributes", hall.has_value() && sameAttrs(hall->attributes, bank(1)[1].attributes), 1);

        FP::Preset u;
        u.name     = "  My Hall  ";
        u.category = " Hall ";
        u.author   = " me ";
        u.notes    = U("  notes kept as typed ü  ");
        u.params   = { { "thr", -24.0f }, { "tiny", 7.038531e-26f }, { "negzero", -0.0f },
                       { "denormal", std::numeric_limits<float>::denorm_min() }, { "max", 3.4028235e38f },
                       { "thr", 5.0f } };
        u.attributes = { { "modeId", "opto-2a" }, { "modeRev", "3" }, { "quote", U("\"ü\\\n") }, { "empty", "" },
                         { "modeId", "clean" } };
        u.tags = FP::tagBit(FP::Tag::red) | FP::tagBit(FP::Tag::blue);
        u.isFactory = true;                                  // forced false on the way in
        rev = s.revision();
        const bool savedOk = s.saveNew(u);
        P.eq("save.ok", savedOk, 1);
        P.eq("save.revision_once", static_cast<int64_t>(s.revision() - rev), 1);
        P.eq("save.uuid_assigned", canonicalUuid(u.uuid), 1);
        userUuid = u.uuid;
        const auto g = s.get(userUuid);
        P.eq("round.found", g.has_value(), 1);
        if (g)
        {
            P.eq("round.name_trimmed", g->name == "My Hall", 1);
            P.eq("round.category_trimmed", g->category == "Hall", 1);
            P.eq("round.author_trimmed", g->author == "me", 1);
            P.eq("round.notes_verbatim", g->notes == u.notes, 1);
            P.eq("round.not_factory", g->isFactory, 0);
            P.eq("round.format", g->format, FP::kFormat);
            P.eq("round.params_bit_exact_first_wins", sameParams(g->params, { u.params.begin(), u.params.end() - 1 }), 1);
            P.eq("round.attributes_first_wins",
                 sameAttrs(g->attributes, { u.attributes.begin(), u.attributes.end() - 1 }), 1);
            P.eq("round.tags", g->tags, u.tags);
            P.eq("round.timestamps", g->createdMs > 0 && g->modifiedMs == g->createdMs && g->lastUsedMs == 0, 1);
            saved = *g;
        }

        FP::Preset u2;
        u2.name = U("MY HALL");
        P.eq("save.unique_name", s.saveNew(u2) && u2.name == "MY HALL 2", 1);
        secondUuid = u2.uuid;
        FP::Preset nan;
        nan.name = "nan";
        nan.params = { { "x", std::numeric_limits<float>::quiet_NaN() } };
        rev = s.revision();
        P.eq("save.refuse_nan", !s.saveNew(nan) && s.lastError().isNotEmpty() && s.revision() == rev, 1);

        // overwrite: params, attributes and metadata replaced, history kept
        FP::Preset o = saved;
        o.params = { { "thr", 1.5f } };
        o.attributes = { { "modeId", "bus-g" } };
        o.notes = "overwritten";
        juce::Thread::sleep(2);
        rev = s.revision();
        P.eq("overwrite.ok", s.overwrite(o), 1);
        P.eq("overwrite.revision_once", static_cast<int64_t>(s.revision() - rev), 1);
        const auto og = s.get(userUuid);
        P.eq("overwrite.values", og && sameParams(og->params, o.params) && sameAttrs(og->attributes, o.attributes)
                                     && og->notes == "overwritten", 1);
        P.eq("overwrite.history_kept", og && og->createdMs == saved.createdMs && og->modifiedMs > saved.createdMs, 1);
        P.eq("overwrite.tags_kept", og && og->tags == saved.tags, 1);

        // factory rows are read-only; refusals leave revision() alone
        rev = s.revision();
        P.eq("refuse.overwrite_factory", !s.overwrite(*hall), 1);
        P.eq("refuse.rename_factory", !s.rename(hall->uuid, "X"), 1);
        P.eq("refuse.category_factory", !s.setCategory(hall->uuid, "X"), 1);
        P.eq("refuse.remove_factory", !s.remove(hall->uuid) && s.lastError().contains("factory"), 1);
        P.eq("refuse.not_found", !s.rename("00000000-0000-4000-8000-0000000000ff", "X")
                                     && s.lastError().contains("not found"), 1);
        P.eq("refuse.name_taken", !s.rename(userUuid, U("my hall 2")), 1);
        P.eq("refuse.revision_unmoved", s.revision() == rev, 1);

        // user mutations keep the attributes (they rewrite the whole row)
        P.eq("mutate.rename", s.rename(userUuid, "Renamed"), 1);
        P.eq("mutate.category", s.setCategory(userUuid, " Room "), 1);
        P.eq("mutate.notes", s.setNotes(userUuid, "n2"), 1);
        const auto mg = s.get(userUuid);
        P.eq("mutate.fields", mg && mg->name == "Renamed" && mg->category == "Room" && mg->notes == "n2", 1);
        P.eq("mutate.attributes_kept", mg && sameAttrs(mg->attributes, o.attributes), 1);
        P.eq("mutate.tags_factory", s.setTags(hall->uuid, FP::tagBit(FP::Tag::green)), 1);
        rev = s.revision();
        P.eq("mutate.tags_unchanged_silent", s.setTags(hall->uuid, FP::tagBit(FP::Tag::green)) && s.revision() == rev, 1);
        s.markUsed(hall->uuid);
        P.eq("mutate.mark_used", s.get(hall->uuid)->lastUsedMs > 0, 1);
        P.eq("tags.default_label", s.tagLabel(FP::Tag::red) == "Red", 1);
        P.eq("tags.set_label", s.setTagLabel(FP::Tag::red, " Keep ") && s.tagLabel(FP::Tag::red) == "Keep", 1);

        // search
        FP::Query q;
        q.text = "RENAMED";
        P.eq("query.text", static_cast<int64_t>(s.query(q).size()), 1);
        q = {};
        q.source = FP::Source::user;
        P.eq("query.user", static_cast<int64_t>(s.query(q).size()), 2);
        q.source = FP::Source::factory;
        P.eq("query.factory", static_cast<int64_t>(s.query(q).size()), 3);
        q = {};
        q.anyOfTags = FP::tagBit(FP::Tag::green);
        const auto tagged = s.query(q);
        P.eq("query.tags", tagged.size() == 1 && tagged[0].uuid == hall->uuid, 1);
        q = {};
        q.category = "hall";
        P.eq("query.category_folded", static_cast<int64_t>(s.query(q).size()), 1);
        q = {};
        const auto all = names(s.query(q));
        const std::vector<juce::String> order { "Big Hall", "Init", "MY HALL 2", "Plate", "Renamed" };
        P.eq("query.sort_name", all == order, 1);
        P.eq("query.carries_attributes", !s.query(q).empty() && sameAttrs(s.query(q)[0].attributes,
                                                                          bank(1)[1].attributes), 1);
        const auto cats = s.categories();
        P.eq("categories", cats.size() == 3 && cats[0] == "Hall" && cats[1] == "Plate" && cats[2] == "Room", 1);

        // import (through the file format, as a browser's import does)
        const auto exported = FP::PresetFile::fromXmlString(kConfig, FP::PresetFile::toXmlString(kConfig, *s.get(userUuid)));
        auto imp = s.importPreset(*exported);
        P.eq("import.duplicate", imp.ok && imp.duplicate && imp.uuid == userUuid, 1);
        FP::Preset changed = *exported;
        changed.setAttr("modeId", "fet-76");                 // same uuid, name and values: a different preset
        imp = s.importPreset(changed);
        P.eq("import.attribute_difference_kept", imp.ok && !imp.duplicate && imp.uuid != userUuid && imp.renamed
                                                     && imp.name == "Renamed 2", 1);
        const auto importedRow = s.get(imp.uuid);
        P.eq("import.attributes_stored", importedRow && importedRow->attr("modeId") != nullptr
                                             && importedRow->attr("modeId")->value == "fet-76", 1);
        imp = s.importPreset(*hall);                          // a factory preset exported and imported unchanged
        P.eq("import.factory_identical_duplicate", imp.ok && imp.duplicate && imp.uuid == hall->uuid, 1);
        FP::Preset fromFactory = *hall;
        fromFactory.params[0].value = -1.0f;                   // an edited copy of a factory preset
        imp = s.importPreset(fromFactory);
        P.eq("import.factory_uuid_fresh", imp.ok && imp.uuid != hall->uuid && imp.name == "Big Hall 2"
                                              && !s.get(imp.uuid)->isFactory, 1);
        FP::Preset newer = *exported;
        newer.format = FP::kFormat + 1;
        imp = s.importPreset(newer);
        P.eq("import.refuse_newer", !imp.ok && imp.error.contains("newer version of FunkGuiTest"), 1);
        newer.format = 0;
        imp = s.importPreset(newer);
        P.eq("import.refuse_format0", !imp.ok && imp.error.contains("not a FunkGuiTest preset"), 1);

        P.eq("remove.ok", s.remove(secondUuid) && !s.get(secondUuid).has_value(), 1);
        P.eq("unique_name.free_again", s.uniqueName("my hall 2") == "my hall 2", 1);
    }
    {
        // Everything above survived the connection closing.
        FP::PresetStore s(kConfig);
        const auto g = s.get(userUuid);
        P.eq("reopen.persistent", s.isPersistent(), 1);
        P.eq("reopen.row", g && g->name == "Renamed" && g->category == "Room" && g->notes == "n2", 1);
        P.eq("reopen.attributes", g && g->attributes.size() == 1 && g->attr("modeId")->value == "bus-g", 1);
        P.eq("reopen.tags", g && g->tags == saved.tags, 1);
        P.eq("reopen.label", s.tagLabel(FP::Tag::red) == "Keep", 1);
        P.eq("reopen.counts", s.count(FP::Source::factory) == 3 && s.count(FP::Source::user) == 3, 1);
    }

    // =================================================================================================================
    // WAL, two connections
    // =================================================================================================================
    {
        FP::PresetStore a(kConfig);
        Raw raw(mainDb, false);
        P.eq("wal.raw_open", raw.db != nullptr, 1);
        P.eq("wal.journal_mode", raw.text("PRAGMA journal_mode") == "wal", 1);
        P.eq("wal.schema_version", raw.integer("PRAGMA user_version"), 2);
        P.eq("wal.min_reader", raw.text("SELECT value FROM meta WHERE key = 'schema_min_reader'") == "1", 1);
        P.eq("wal.attributes_column",
             raw.integer("SELECT COUNT(*) FROM pragma_table_info('preset') WHERE name = 'attributes'"), 1);
        P.eq("wal.sidecar_exists", mainDb.getSiblingFile("Presets.db-wal").existsAsFile(), 1);

        const auto before = raw.integer("SELECT COUNT(*) FROM preset WHERE is_factory = 0");
        FP::Preset x;
        x.name = "Seen";
        a.saveNew(x);
        P.eq("wal.reader_sees_commit", raw.integer("SELECT COUNT(*) FROM preset WHERE is_factory = 0") - before, 1);

        P.eq("wal.poll_quiet", a.pollExternalChanges(), 0);
        FP::Preset y;
        y.name = "From Elsewhere";
        {
            FP::PresetStore b(kConfig);
            b.saveNew(y);
        }
        const auto rev = a.revision();
        P.eq("wal.poll_sees_other", a.pollExternalChanges() && a.revision() == rev + 1, 1);
        P.eq("wal.poll_once", a.pollExternalChanges(), 0);
        P.eq("wal.other_row_visible", a.get(y.uuid).has_value(), 1);

        // Two connections saving under one name, interleaved: every save lands, every name distinct.
        {
            FP::PresetStore b(kConfig);
            int ok = 0;
            for (int i = 0; i < 20; ++i)
            {
                FP::Preset r;
                r.name = "Race";
                ok += ((i % 2) == 0 ? a : b).saveNew(r) ? 1 : 0;
            }
            FP::Query q;
            q.text = "race";
            const auto rows = a.query(q);
            std::set<juce::String> distinct;
            for (const auto& r : rows) distinct.insert(r.name.toLowerCase());
            P.eq("wal.race_all_saved", ok, 20);
            P.eq("wal.race_names_distinct", static_cast<int64_t>(distinct.size()), 20);
        }

        // syncFactory: the same revision commits nothing (another connection's data_version does not move) ...
        Raw watch(mainDb, false);
        const auto dv0 = watch.integer("PRAGMA data_version");
        a.syncFactory(bank(1), 1);
        P.eq("factory.sync_idempotent", watch.integer("PRAGMA data_version") == dv0, 1);
        // ... and a new bank upserts by uuid: tags and last-used survive, removed rows go, new rows come.
        const auto hallBefore = a.get(bank(1)[1].uuid);
        a.syncFactory(bank(2), 2);
        const auto hallAfter = a.get(bank(2)[1].uuid);
        P.eq("factory.update_committed", watch.integer("PRAGMA data_version") != dv0, 1);
        P.eq("factory.update_values", hallAfter && sameParams(hallAfter->params, bank(2)[1].params), 1);
        P.eq("factory.update_keeps_tags", hallAfter && hallAfter->tags == FP::tagBit(FP::Tag::green), 1);
        P.eq("factory.update_keeps_last_used", hallBefore && hallAfter && hallAfter->lastUsedMs == hallBefore->lastUsedMs
                                                   && hallAfter->lastUsedMs > 0, 1);
        P.eq("factory.update_removes", !a.get(bank(1)[2].uuid).has_value(), 1);
        P.eq("factory.update_adds", a.get(bank(2)[2].uuid).has_value() && a.count(FP::Source::factory) == 3, 1);
        P.eq("factory.categories_fold", a.categories().contains("Hall") && !a.categories().contains("hall"), 1);
    }
    {
        // A damaged attributes value (edited with the sqlite3 shell) degrades to the entries that still read.
        {
            Raw w(mainDb, true);
            const std::string sql = "UPDATE preset SET attributes = '{\"n\":1,\"ok\":\"yes\",\"bad\":' WHERE uuid = '"
                                  + userUuid.toStdString() + "'";
            P.eq("damaged.write", w.exec(sql), 1);
        }
        FP::PresetStore s(kConfig);
        const auto g = s.get(userUuid);
        P.eq("damaged.attributes_tolerated", g && g->attributes.size() == 1 && g->attr("ok") != nullptr
                                                 && g->attr("ok")->value == "yes", 1);
    }

    // =================================================================================================================
    // FALLBACK
    // =================================================================================================================
    const auto bank1 = bank(1);
    {
        // A sandboxed host / read-only home: nothing can be created (K2 #19).
        const auto locked = root.getChildFile("locked");
        locked.createDirectory();
        P.eq("fallback.chmod", setMode(locked, 0500), 1);
        for (const auto& target : { locked.getChildFile("sub/Presets.db"), locked.getChildFile("Presets.db") })
        {
            useDb(target);
            FP::PresetStore s(kConfig);
            const std::string k = target.getParentDirectory() == locked ? "fallback.unwritable_folder"
                                                                        : "fallback.uncreatable_folder";
            P.eq(k + ".open", s.isOpen(), 1);
            P.eq(k + ".memory", !s.isPersistent() && s.file() == juce::File(), 1);
            P.eq(k + ".says_why", s.lastError().contains("will not be saved"), 1);
            s.syncFactory(bank1, 1);
            FP::Preset p;
            p.name = "Memory";
            p.attributes = { { "modeId", "clean" } };
            P.eq(k + ".works", s.count(FP::Source::factory) == 3 && s.saveNew(p)
                                   && s.get(p.uuid)->attr("modeId") != nullptr, 1);
            P.eq(k + ".nothing_written", !target.exists(), 1);
        }
        setMode(locked, 0700);
    }
    {
        FP::ProductConfig invalid = kConfig;
        invalid.productName = "a/b";
        P.eq("fallback.invalid_config.no_location", FP::PresetStore::defaultLocation(invalid) == juce::File(), 1);
        FP::PresetStore s(invalid);
        s.syncFactory(bank1, 1);
        P.eq("fallback.invalid_config.memory", s.isOpen() && !s.isPersistent() && s.lastError().isNotEmpty()
                                                   && s.count(FP::Source::factory) == 3, 1);
    }
    {
        // Not a database: set aside (never deleted) and replaced.
        const auto bad = root.getChildFile("corrupt/Presets.db");
        bad.getParentDirectory().createDirectory();
        const juce::String junk = juce::String::repeatedString("this is not a database. ", 200);
        bad.replaceWithText(junk);
        useDb(bad);
        FP::PresetStore s(kConfig);
        P.eq("fallback.corrupt.replaced", s.isPersistent() && s.lastError().contains("set aside"), 1);
        const auto aside = bad.getParentDirectory().findChildFiles(juce::File::findFiles, false, "Presets.db.corrupt-*");
        P.eq("fallback.corrupt.kept_aside", aside.size() == 1 && aside[0].loadFileAsString() == junk, 1);
    }
    {
        // A read-only file of this schema: copied into memory, visible, never modified.
        const auto ro = root.getChildFile("ro/Presets.db");
        useDb(ro);
        juce::String uuid;
        {
            FP::PresetStore s(kConfig);
            s.syncFactory(bank1, 1);
            FP::Preset p;
            p.name = "Mine";
            p.attributes = { { "modeId", "mu-67" } };
            s.saveNew(p);
            uuid = p.uuid;
        }
        const auto before = bytesOf(ro);
        P.eq("fallback.readonly.chmod", setMode(ro, 0444), 1);
        {
            FP::PresetStore s(kConfig);
            const auto g = s.get(uuid);
            P.eq("fallback.readonly.memory", s.isOpen() && !s.isPersistent() && s.lastError().contains("read-only"), 1);
            P.eq("fallback.readonly.sees_presets", g && g->attr("modeId") != nullptr && g->attr("modeId")->value == "mu-67"
                                                       && s.count(FP::Source::factory) == 3, 1);
            FP::Preset p;
            p.name = "Unsaved";
            P.eq("fallback.readonly.edits_in_memory", s.saveNew(p), 1);
        }
        P.eq("fallback.readonly.untouched", bytesOf(ro) == before, 1);
        setMode(ro, 0644);
    }
    {
        // HardwareReverb's v1 file, read-only: copied, migrated in memory, the row visible, the file untouched.
        const auto v1ro = root.getChildFile("v1ro/Presets.db");
        P.eq("migrate.v1ro.fixture", makeV1(v1ro), 1);
        const auto before = bytesOf(v1ro);
        setMode(v1ro, 0444);
        useDb(v1ro);
        {
            FP::PresetStore s(kConfig);
            const auto g = s.get(kHrUuid);
            P.eq("migrate.v1ro.memory", s.isOpen() && !s.isPersistent(), 1);
            P.eq("migrate.v1ro.row", g && g->name == "Old Hall" && g->find("size") != nullptr
                                         && sameBits(g->find("size")->value, 0.5f) && g->attributes.empty()
                                         && g->tags == FP::tagBit(FP::Tag::yellow), 1);
        }
        P.eq("migrate.v1ro.untouched", bytesOf(v1ro) == before, 1);
        setMode(v1ro, 0644);
    }
    {
        // HardwareReverb's v1 file, writable: migrated to v2 in place, every row and tag kept.
        const auto v1 = root.getChildFile("v1/Presets.db");
        P.eq("migrate.v1.fixture", makeV1(v1), 1);
        useDb(v1);
        {
            FP::PresetStore s(kConfig);
            const auto g = s.get(kHrUuid);
            P.eq("migrate.v1.persistent", s.isPersistent() && s.lastError().isEmpty(), 1);
            P.eq("migrate.v1.row_kept", g && g->name == "Old Hall" && g->createdMs == 1000 && g->attributes.empty()
                                            && g->tags == FP::tagBit(FP::Tag::yellow)
                                            && sameParams(g->params, { { "size", 0.5f }, { "decay", 0.25f } }), 1);
            FP::Preset o = *g;
            o.setAttr("modeId", "clean");
            P.eq("migrate.v1.attributes_writable", s.overwrite(o) && s.get(kHrUuid)->attr("modeId") != nullptr, 1);
            FP::Preset same;
            same.name = "old HALL";
            P.eq("migrate.v1.unique_index_kept", s.saveNew(same) && same.name == "old HALL 2", 1);
        }
        Raw raw(v1, false);
        P.eq("migrate.v1.version", raw.integer("PRAGMA user_version"), 2);
        P.eq("migrate.v1.min_reader_unchanged", raw.text("SELECT value FROM meta WHERE key = 'schema_min_reader'") == "1", 1);
    }
    {
        // A newer build's file it marked unreadable: this build runs in memory and leaves the file byte for byte.
        const auto nu = root.getChildFile("newer-unreadable/Presets.db");
        useDb(nu);
        { FP::PresetStore s(kConfig); }
        {
            Raw w(nu, true);
            P.eq("newer.unreadable.fixture", w.exec("UPDATE meta SET value = '9' WHERE key = 'schema_min_reader'")
                                                 && w.exec("PRAGMA user_version = 9"), 1);
        }
        const auto before = bytesOf(nu);
        {
            FP::PresetStore s(kConfig);
            P.eq("newer.unreadable.memory", s.isOpen() && !s.isPersistent()
                                                && s.lastError().contains("newer version of FunkGuiTest"), 1);
        }
        P.eq("newer.unreadable.untouched", bytesOf(nu) == before, 1);

        // One it marked readable (it only added): used as it is, unmigrated.
        const auto nr = root.getChildFile("newer-readable/Presets.db");
        useDb(nr);
        { FP::PresetStore s(kConfig); }
        {
            Raw w(nr, true);
            P.eq("newer.readable.fixture", w.exec("CREATE TABLE future (x INTEGER)") && w.exec("PRAGMA user_version = 3"), 1);
        }
        {
            FP::PresetStore s(kConfig);
            FP::Preset p;
            p.name = "On Newer";
            P.eq("newer.readable.used", s.isPersistent() && s.saveNew(p), 1);
        }
        Raw raw(nr, false);
        P.eq("newer.readable.version_kept", raw.integer("PRAGMA user_version"), 3);
    }
    {
        // v0: an empty file is ours to set up; someone else's "preset" table is not, and is left alone.
        const auto empty = root.getChildFile("v0-empty/Presets.db");
        empty.getParentDirectory().createDirectory();
        empty.create();
        useDb(empty);
        {
            FP::PresetStore s(kConfig);
            P.eq("v0.empty_migrated", s.isPersistent(), 1);
        }
        const auto foreignOk = root.getChildFile("v0-foreign/Presets.db");
        {
            foreignOk.getParentDirectory().createDirectory();
            Raw w(foreignOk, true);
            w.exec("CREATE TABLE notes (x TEXT)");
        }
        useDb(foreignOk);
        {
            FP::PresetStore s(kConfig);
            P.eq("v0.foreign_tables_migrated", s.isPersistent(), 1);
        }
        const auto clash = root.getChildFile("v0-clash/Presets.db");
        {
            clash.getParentDirectory().createDirectory();
            Raw w(clash, true);
            w.exec("CREATE TABLE preset (something_else TEXT)");
        }
        const auto before = bytesOf(clash);
        useDb(clash);
        {
            FP::PresetStore s(kConfig);
            P.eq("v0.clash_memory", s.isOpen() && !s.isPersistent(), 1);
        }
        P.eq("v0.clash_untouched", bytesOf(clash) == before, 1);
    }

    T::unsetEnv(kEnv);
    P.eq("env.real_database_untouched", realDefault.getParentDirectory().exists() == realFolderBefore, 1);
    root.deleteRecursively();
    return P.finish();
}
