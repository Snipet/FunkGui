#include "PresetStore.h"
#include "Platform.h"
#include "Sqlite.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// Measured with Tools/PresetProbe.cpp, which is the gate for everything here
// (schema, every mutation, search, import, two processes, recovery, cost).

namespace hrvb::presets
{
    namespace
    {
        // ---- schema -------------------------------------------------------
        //
        // PRAGMA user_version is the schema version; kLadder below is every
        // step from an empty file to it. A v2 is one more function and one
        // more row: each step runs inside the same BEGIN IMMEDIATE as the
        // user_version write, so two processes opening an old file at the
        // same time cannot both migrate it — the second waits on the write
        // lock (busy timeout), re-reads the version inside its transaction
        // and finds nothing left to do.
        constexpr int kSchemaVersion = 1;

        // Forward compatibility. A database a NEWER build has migrated past
        // kSchemaVersion is used as it is, unmigrated, when that build wrote
        // meta.schema_min_reader <= kSchemaVersion — i.e. when its steps only
        // added (a column, a table, an index) and an older reader still reads
        // and writes it correctly. A step that changes a meaning raises
        // min_reader, and an older build then runs in memory (factory bank
        // only) rather than write rows the newer one would misread. Without
        // this key every schema bump would lock a user who went back one
        // version out of their own presets.
        constexpr int kMinReaderWritten = 1;

        // Long enough that other processes' transactions always finish inside
        // it (the probe's race: 250 saves from five processes, contending for
        // one write lock, finish in 70-280 ms in total); short enough
        // that a wedged process cannot freeze an editor for more than a
        // moment. Waiting happens on the message thread, never on audio.
        constexpr int kBusyTimeoutMs = 2000;

        // The folded *_key columns, and why they exist rather than COLLATE
        // NOCASE: Platform.cpp.
        juce::String fold(const juce::String& s) { return foldText(s, foldCase); }

        // The ORDER BY key for names — display order only, never identity.
        // Case- AND diacritic-folded so "Äther" files among the A's rather
        // than after "Z" (UTF-8 byte order puts U+00E4 past every ASCII
        // letter), and every run of digits left-padded to ten places so
        // "Hall 2" comes before "Hall 10". Still locale-independent, so every
        // process orders the same rows the same way; name_key then uuid break
        // the ties it creates ("Hall 2" / "Hall 02", "Äther" / "Ather").
        juce::String sortKey(const juce::String& name)
        {
            const auto f = foldText(name, foldCase | foldDiacritics | foldWidth);
            juce::String out;
            for (int i = 0; i < f.length();)
            {
                if (f[i] < '0' || f[i] > '9')
                {
                    out += juce::String::charToString(f[i++]);
                    continue;
                }
                int j = i;
                while (j < f.length() && f[j] >= '0' && f[j] <= '9') ++j;
                auto run = f.substring(i, j).trimCharactersAtStart("0");
                if (run.isEmpty()) run = "0";
                out += run.length() < 10 ? juce::String::repeatedString("0", 10 - run.length()) + run : run;
                i = j;
            }
            return out;
        }

        // One string per row that text search runs instr() over. The unit
        // separator cannot be typed, and is stripped from the needle, so a
        // match can never straddle two fields.
        juce::String searchKey(const Preset& p)
        {
            const juce::String sep = juce::String::charToString((juce::juce_wchar) 0x1f);
            return fold(p.name) + sep + fold(p.category) + sep + fold(p.author) + sep + fold(p.notes);
        }

        // ---- parameter payload ------------------------------------------
        //
        // A JSON object {id: plainValue}. Written by hand rather than through
        // juce::JSON for two measured reasons:
        //   * the number format must not depend on the host's locale:
        //     snprintf("%g", 0.15) prints "0,15" under de_DE (measured), which
        //     is not JSON. std::to_chars is locale-free and shortest-exact;
        //   * parsing must give back the SAME float. Shortest-float text
        //     parsed to double and then narrowed (strtod_l then a cast — the
        //     path juce::JSON takes too, readDoubleValue ending in strtod_l)
        //     fails for exactly one magnitude in
        //     all 2^31 positive finite floats (7.038531e-26: double rounding),
        //     measured exhaustively; strtof_l on the same text fails for
        //     none. A preset that loads one ulp off would read as "modified".
        //     strtofC (Platform.h) is that read on both operating systems.

        void appendFloat(std::string& out, float v)
        {
            char buf[32];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v);
            out.append(buf, r.ptr);
        }

        void appendJsonString(std::string& out, const juce::String& s)
        {
            out += '"';
            for (const char* c = s.toRawUTF8(); *c != 0; ++c)
            {
                const auto u = (unsigned char) *c;
                if (u == '"' || u == '\\') { out += '\\'; out += (char) u; }
                else if (u < 0x20) { char e[8]; std::snprintf(e, sizeof(e), "\\u%04x", u); out += e; }
                else out += (char) u;
            }
            out += '"';
        }

        // Duplicate ids keep the FIRST value — the one Preset::find() would
        // have returned, so what is stored is what the caller was looking at.
        // Non-finite values are refused before this is reached (validate).
        std::string paramsToJson(const std::vector<ParamValue>& params)
        {
            std::string out = "{";
            juce::StringArray seen;
            for (const auto& p : params)
            {
                if (p.id.isEmpty() || !std::isfinite(p.value) || seen.contains(p.id)) continue;
                seen.add(p.id);
                if (out.size() > 1) out += ',';
                appendJsonString(out, p.id);
                out += ':';
                appendFloat(out, p.value);
            }
            out += '}';
            return out;
        }

        // The reader for the writer above, tolerant of whitespace and string
        // escapes (someone may have edited a row with the sqlite3 shell). An
        // entry it cannot read is skipped rather than failing the preset:
        // an absent parameter loads at its default, by the rule in
        // PresetTypes.h, so a damaged value degrades one knob, not the row.
        struct JsonReader
        {
            const char* p;
            const char* end;

            void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
            bool eat(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }

            bool string(std::string& out)
            {
                if (!eat('"')) return false;
                while (p < end && *p != '"')
                {
                    if (*p == '\\')
                    {
                        if (++p >= end) return false;
                        switch (*p)
                        {
                            case 'u':
                            {
                                if (end - p < 5) return false;
                                const std::string hex(p + 1, p + 5);
                                const auto cp = (juce::juce_wchar) std::strtol(hex.c_str(), nullptr, 16);
                                out += juce::String::charToString(cp).toStdString();
                                p += 4;
                                break;
                            }
                            case 'n': out += '\n'; break;
                            case 't': out += '\t'; break;
                            case 'r': out += '\r'; break;
                            case 'b': out += '\b'; break;
                            case 'f': out += '\f'; break;
                            default:  out += *p; break;
                        }
                        ++p;
                    }
                    else out += *p++;
                }
                return eat('"');
            }

            bool number(float& v)
            {
                ws();
                const char* s = p;
                while (p < end && (std::isdigit((unsigned char) *p) || *p == '-' || *p == '+'
                                   || *p == '.' || *p == 'e' || *p == 'E'))
                    ++p;
                if (p == s) return false;
                const std::string text(s, p);
                char* stop = nullptr;
                v = strtofC(text.c_str(), &stop);
                return stop == text.c_str() + text.size() && std::isfinite(v);
            }

            // Skips one value of any JSON type (an entry this build cannot use).
            void skipValue()
            {
                ws();
                int depth = 0;
                bool inString = false;
                while (p < end)
                {
                    const char c = *p;
                    if (inString) { if (c == '\\') ++p; else if (c == '"') inString = false; }
                    else if (c == '"') inString = true;
                    else if (c == '{' || c == '[') ++depth;
                    else if (c == '}' || c == ']') { if (depth == 0) return; --depth; }
                    else if (c == ',' && depth == 0) return;
                    ++p;
                }
            }
        };

        std::vector<ParamValue> paramsFromJson(const char* text, int bytes)
        {
            std::vector<ParamValue> out;
            if (text == nullptr) return out;
            JsonReader r { text, text + bytes };
            if (!r.eat('{')) return out;
            if (r.eat('}')) return out;
            do
            {
                std::string key;
                if (!r.string(key) || !r.eat(':')) break;
                const char* before = r.p;
                float v = 0.0f;
                if (r.number(v))
                {
                    const auto id = juce::String::fromUTF8(key.c_str(), (int) key.size());
                    bool dup = false;
                    for (const auto& e : out) if (e.id == id) { dup = true; break; }
                    if (!dup && id.isNotEmpty()) out.push_back({ id, v });
                }
                else
                {
                    r.p = before;
                    r.skipValue();
                }
            } while (r.eat(','));
            return out;
        }

        bool paramsEqual(const std::vector<ParamValue>& a, const std::vector<ParamValue>& b)
        {
            // As sets: the file and the row may list ids in different orders.
            // Compared after the same first-wins dedupe the writer applies.
            auto norm = [](const std::vector<ParamValue>& v)
            {
                std::map<juce::String, float> m;
                for (const auto& p : v) if (p.id.isNotEmpty() && m.count(p.id) == 0) m[p.id] = p.value;
                return m;
            };
            return norm(a) == norm(b);
        }

        juce::String validateParams(const std::vector<ParamValue>& params)
        {
            for (const auto& p : params)
                if (!std::isfinite(p.value))
                    return "parameter \"" + p.id + "\" has a value that is not a number";
            return {};
        }

        // A well-formed UUID in the canonical lowercase dashed form, or empty.
        // A hand-edited file can carry anything in its uuid attribute.
        juce::String canonicalUuid(const juce::String& s)
        {
            const auto t = s.trim();
            if (t.length() != 36 && t.length() != 32) return {};
            if (!t.containsOnly("0123456789abcdefABCDEF-")) return {};
            const juce::Uuid u(t);
            if (u.isNull()) return {};
            const auto d = u.toDashedString();
            return d.removeCharacters("-").equalsIgnoreCase(t.removeCharacters("-")) ? d : juce::String();
        }

        juce::int64 nowMs() { return juce::Time::currentTimeMillis(); }

        constexpr const char* kRowColumns =
            "p.uuid, p.name, p.category, p.author, p.notes, p.is_factory, p.format, p.params, "
            "p.created_ms, p.modified_ms, p.last_used_ms, "
            "(SELECT COALESCE(SUM(1 << t.color), 0) FROM tag t WHERE t.preset_uuid = p.uuid)";

        enum class OpenResult { ok, readOnly, corrupt, unavailable, tooNew };

        OpenResult classify(int rc)
        {
            switch (rc & 0xff)
            {
                case SQLITE_NOTADB:
                case SQLITE_CORRUPT: return OpenResult::corrupt;
                default:             return OpenResult::unavailable;
            }
        }
    }

    // =======================================================================

    struct PresetStore::Impl
    {
        sqlite3* db = nullptr;
        juce::File path;
        bool persistent = false;
        juce::String error;
        uint64_t revision = 1;
        sqlite3_int64 dataVersion = -1;
        std::map<std::string, sqlite3_stmt*> cache;

        ~Impl() { close(); }

        void close()
        {
            for (auto& [sql, s] : cache) sqlite3_finalize(s);
            cache.clear();
            if (db != nullptr) sqlite3_close_v2(db);
            db = nullptr;
        }

        juce::String sqliteError() const
        {
            return db != nullptr ? juce::String::fromUTF8(sqlite3_errmsg(db)) : juce::String("no database");
        }

        bool fail(const juce::String& what)
        {
            error = what;
            return false;
        }

        bool failSql(const juce::String& what)
        {
            return fail(what + ": " + sqliteError());
        }

        // ---- statements ---------------------------------------------------
        //
        // Every statement is prepared once per connection and kept: query()
        // runs on each keystroke in the browser's search field, and preparing
        // costs about as much as running the statement over a small table.
        // Only bound parameters ever carry user text.
        sqlite3_stmt* prepare(const std::string& sql)
        {
            if (db == nullptr) return nullptr;
            if (auto it = cache.find(sql); it != cache.end()) return it->second;
            sqlite3_stmt* s = nullptr;
            if (sqlite3_prepare_v3(db, sql.c_str(), (int) sql.size() + 1, SQLITE_PREPARE_PERSISTENT, &s, nullptr) != SQLITE_OK)
            {
                sqlite3_finalize(s);
                return nullptr;
            }
            cache.emplace(sql, s);
            return s;
        }

        bool exec(const char* sql)
        {
            return db != nullptr && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
        }

        sqlite3_int64 pragmaInt(const char* sql)
        {
            sqlite3_stmt* s = nullptr;
            sqlite3_int64 v = -1;
            if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
                v = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
            return v;
        }

        // ---- opening ------------------------------------------------------

        OpenResult tryOpenFile(const juce::File& f)
        {
            if (!f.getParentDirectory().createDirectory())
            {
                error = "cannot create " + f.getParentDirectory().getFullPathName();
                return OpenResult::unavailable;
            }

            int rc = sqlite3_open_v2(f.getFullPathName().toRawUTF8(), &db,
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
            if (rc != SQLITE_OK)
            {
                error = "cannot open " + f.getFullPathName() + ": " + sqliteError();
                return classify(rc);
            }
            sqlite3_extended_result_codes(db, 1);
            sqlite3_busy_timeout(db, kBusyTimeoutMs);

            // quick_check, not integrity_check: it skips the index-vs-table
            // cross check (the expensive half) and still catches a damaged
            // page, which is what a crash mid-write or a bad copy leaves. It
            // is also the first statement to read the file, so a file that is
            // not a database at all fails here with SQLITE_NOTADB. Measured
            // on a 1000-preset file: the whole open, check included, 0.55 ms
            // idle and ~2 ms with every core busy — once per process, when the
            // first editor opens.
            {
                sqlite3_stmt* s = nullptr;
                rc = sqlite3_prepare_v2(db, "PRAGMA quick_check(1)", -1, &s, nullptr);
                if (rc == SQLITE_OK) rc = sqlite3_step(s);
                juce::String verdict;
                if (rc == SQLITE_ROW)
                    verdict = juce::String::fromUTF8((const char*) sqlite3_column_text(s, 0));
                const int stepRc = rc;
                sqlite3_finalize(s);
                if (stepRc != SQLITE_ROW)
                {
                    error = "cannot read " + f.getFullPathName() + ": " + sqliteError();
                    return classify(sqlite3_extended_errcode(db));
                }
                if (verdict != "ok")
                {
                    error = f.getFullPathName() + " failed its integrity check (" + verdict + ")";
                    return OpenResult::corrupt;
                }
            }

            const auto version = pragmaInt("PRAGMA user_version");
            if (version > kSchemaVersion)
            {
                const auto minReader = pragmaInt("SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'schema_min_reader'");
                if (minReader < 0 || minReader > kSchemaVersion)
                {
                    error = "the preset database was made by a newer version of HardwareReverb";
                    return OpenResult::tooNew;
                }
            }

            if (sqlite3_db_readonly(db, "main") == 1)
            {
                error = f.getFullPathName() + " is read-only";
                return version >= kSchemaVersion ? OpenResult::readOnly : OpenResult::unavailable;
            }

            if (!exec("PRAGMA synchronous=NORMAL") || !exec("PRAGMA foreign_keys=ON"))
            {
                error = "cannot configure " + f.getFullPathName() + ": " + sqliteError();
                return classify(sqlite3_extended_errcode(db));
            }

            // Migrate BEFORE switching to WAL: journal_mode=WAL rewrites the
            // file header, so doing it first modified a file whose migration
            // then failed (measured: the v0-with-a-foreign-"preset"-table
            // case changed hash although no row was touched). A file this
            // build cannot use is left byte-for-byte as it was found.
            if (!migrate())
                return classify(sqlite3_extended_errcode(db));

            // WAL: readers never block the writer and the writer never blocks
            // readers, which is what lets a second host's browser poll while
            // this one saves. synchronous=NORMAL is WAL's intended pairing: a
            // commit survives an application crash, and a power cut can lose
            // at most the last commits, never corrupt the file. Persistent
            // (in the file) — set on every open anyway, it is a no-op then.
            {
                sqlite3_stmt* s = nullptr;
                juce::String mode;
                if (sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL", -1, &s, nullptr) == SQLITE_OK
                    && sqlite3_step(s) == SQLITE_ROW)
                    mode = juce::String::fromUTF8((const char*) sqlite3_column_text(s, 0));
                sqlite3_finalize(s);
                if (!mode.equalsIgnoreCase("wal"))
                {
                    error = "cannot enable WAL on " + f.getFullPathName() + ": " + sqliteError();
                    return classify(sqlite3_extended_errcode(db));
                }
            }

            return OpenResult::ok;
        }

        bool openMemory()
        {
            close();
            if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
            {
                close();
                return false;
            }
            sqlite3_extended_result_codes(db, 1);
            exec("PRAGMA foreign_keys=ON");
            return migrate();
        }

        // Moves Presets.db, and its -wal and -shm, to Presets.db.corrupt-<ms>
        // (-wal, -shm): the user's data is never deleted, only set aside
        // where it can be sent in or recovered with the sqlite3 shell.
        bool setAside(const juce::File& f)
        {
            const auto stamp = juce::String(nowMs());
            const auto dest = f.getSiblingFile(f.getFileName() + ".corrupt-" + stamp);
            bool ok = !f.exists() || f.moveFileTo(dest);
            for (const char* suffix : { "-wal", "-shm" })
            {
                const auto side = f.getSiblingFile(f.getFileName() + suffix);
                if (side.exists())
                    ok = side.moveFileTo(dest.getSiblingFile(dest.getFileName() + suffix)) && ok;
            }
            return ok;
        }

        // A file that is readable but not writable (permissions, a locked
        // volume) is copied into memory: the user still sees and loads their
        // presets, and isPersistent() says their edits will not be kept.
        bool copyIntoMemory()
        {
            sqlite3* src = db;
            db = nullptr;
            for (auto& [sql, s] : cache) sqlite3_finalize(s);
            cache.clear();
            sqlite3* mem = nullptr;
            bool ok = sqlite3_open_v2(":memory:", &mem, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK;
            if (ok)
            {
                sqlite3_backup* b = sqlite3_backup_init(mem, "main", src, "main");
                ok = b != nullptr && sqlite3_backup_step(b, -1) == SQLITE_DONE;
                if (b != nullptr) sqlite3_backup_finish(b);
            }
            sqlite3_close_v2(src);
            if (!ok) { if (mem != nullptr) sqlite3_close_v2(mem); return false; }
            db = mem;
            sqlite3_extended_result_codes(db, 1);
            exec("PRAGMA foreign_keys=ON");
            return true;
        }

        void open(const juce::File& f)
        {
            path = f;
            // The SQL here needs 3.24 (upsert). macOS 14's system SQLite is far
            // newer; the copy Windows ships (winsqlite3) follows the Windows
            // build. Say so plainly rather than fail later on one statement.
            // With no connection, isOpen() is false and every call refuses.
            if (sqlite3_libversion_number() < 3024000)
            {
                error = juce::String("SQLite 3.24 or newer is needed; this system has ")
                      + juce::String::fromUTF8(sqlite3_libversion());
                persistent = false;
                return;
            }
            auto r = tryOpenFile(f);
            if (r == OpenResult::corrupt)
            {
                const auto why = error;
                close();
                if (setAside(f))
                {
                    r = tryOpenFile(f);
                    if (r == OpenResult::ok)
                        error = why + "; it was set aside and a new database created";
                }
                else
                {
                    error = why + "; it could not be set aside";
                    r = OpenResult::unavailable;
                }
            }

            if (r == OpenResult::ok)
            {
                persistent = true;
                dataVersion = pragmaInt("PRAGMA data_version");
                return;
            }

            const auto why = error;
            if (r == OpenResult::readOnly && copyIntoMemory())
            {
                error = why + "; changes will not be saved";
                return;
            }
            close();
            persistent = false;
            if (!openMemory())
                error = why + "; and no in-memory database either";
            else
                error = why + "; presets will not be saved this session";
        }

        // ---- migrations ---------------------------------------------------

        // v0 is a file with nothing of ours in it: new (zero bytes), or one
        // some tool created. CREATE TABLE without IF NOT EXISTS on purpose: a
        // v0 file that already has a "preset" table of some other shape fails
        // the migration, and the store then runs in memory rather than
        // writing rows into a table it does not understand.
        static bool toV1(Impl& I)
        {
            static const char* steps[] = {
                "CREATE TABLE preset ("
                "  uuid         TEXT PRIMARY KEY NOT NULL,"
                "  name         TEXT NOT NULL,"
                "  name_key     TEXT NOT NULL,"           // fold(name): uniqueness
                "  sort_key     TEXT NOT NULL DEFAULT '',"// sortKey(name): display order
                "  category     TEXT NOT NULL DEFAULT '',"
                "  category_key TEXT NOT NULL DEFAULT '',"// fold(category): filter, grouping
                "  author       TEXT NOT NULL DEFAULT '',"
                "  notes        TEXT NOT NULL DEFAULT '',"
                "  search_key   TEXT NOT NULL DEFAULT '',"// fold(name|category|author|notes)
                "  is_factory   INTEGER NOT NULL DEFAULT 0 CHECK (is_factory IN (0, 1)),"
                "  format       INTEGER NOT NULL DEFAULT 1,"
                "  params       TEXT NOT NULL DEFAULT '{}',"
                "  created_ms   INTEGER NOT NULL DEFAULT 0,"
                "  modified_ms  INTEGER NOT NULL DEFAULT 0,"
                "  last_used_ms INTEGER NOT NULL DEFAULT 0)",
                // User names are unique; factory names are exempt because the
                // bank is not the user's to rename and may legitimately gain a
                // name a user already chose. uniqueName() still avoids them.
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
            };
            for (const char* s : steps)
                if (!I.exec(s)) return I.failSql("schema v1");
            const auto minReader = "INSERT OR REPLACE INTO meta(key, value) VALUES ('schema_min_reader', '"
                                   + std::to_string(kMinReaderWritten) + "')";
            return I.exec(minReader.c_str()) || I.failSql("schema v1");
        }

        struct Step { int to; bool (*apply)(Impl&); };

        bool migrate()
        {
            static const Step kLadder[] = {
                { 1, &toV1 },
                // { 2, &toV2 },   next: one function, one row, kSchemaVersion = 2
            };
            static_assert(sizeof(kLadder) / sizeof(kLadder[0]) == kSchemaVersion,
                          "one ladder step per schema version");

            // Fast path without the write lock: every open after the first.
            if (pragmaInt("PRAGMA user_version") >= kSchemaVersion) return true;

            if (!exec("BEGIN IMMEDIATE")) return failSql("cannot lock the preset database to upgrade it");
            auto version = pragmaInt("PRAGMA user_version");
            for (const auto& step : kLadder)
            {
                if (step.to <= version) continue;
                const std::string setVersion = "PRAGMA user_version = " + std::to_string(step.to);
                if (!step.apply(*this) || !exec(setVersion.c_str()))
                {
                    const auto why = error.isNotEmpty() ? error : "schema upgrade: " + sqliteError();
                    exec("ROLLBACK");
                    return fail(why);
                }
                version = step.to;
            }
            if (!exec("COMMIT"))
            {
                const auto why = "schema upgrade: " + sqliteError();
                exec("ROLLBACK");
                return fail(why);
            }
            return true;
        }

        // ---- rows ---------------------------------------------------------

        static juce::String text(sqlite3_stmt* s, int col)
        {
            return juce::String::fromUTF8((const char*) sqlite3_column_text(s, col), sqlite3_column_bytes(s, col));
        }

        static Preset readRow(sqlite3_stmt* s)
        {
            Preset p;
            p.uuid       = text(s, 0);
            p.name       = text(s, 1);
            p.category   = text(s, 2);
            p.author     = text(s, 3);
            p.notes      = text(s, 4);
            p.isFactory  = sqlite3_column_int(s, 5) != 0;
            p.format     = sqlite3_column_int(s, 6);
            p.params     = paramsFromJson((const char*) sqlite3_column_text(s, 7), sqlite3_column_bytes(s, 7));
            p.createdMs  = sqlite3_column_int64(s, 8);
            p.modifiedMs = sqlite3_column_int64(s, 9);
            p.lastUsedMs = sqlite3_column_int64(s, 10);
            p.tags       = static_cast<TagMask>(sqlite3_column_int(s, 11) & kAllTags);
            return p;
        }

        std::optional<Preset> load(const juce::String& uuid)
        {
            auto* s = prepare(std::string("SELECT ") + kRowColumns + " FROM preset p WHERE p.uuid = ?1");
            if (s == nullptr) { failSql("read"); return std::nullopt; }
            sqlite3_bind_text(s, 1, uuid.toRawUTF8(), -1, SQLITE_TRANSIENT);
            std::optional<Preset> out;
            const int rc = sqlite3_step(s);
            if (rc == SQLITE_ROW) out = readRow(s);
            else if (rc != SQLITE_DONE) failSql("read");
            sqlite3_reset(s);
            sqlite3_clear_bindings(s);
            return out;
        }

        static void bindText(sqlite3_stmt* s, int i, const juce::String& v)
        {
            sqlite3_bind_text(s, i, v.toRawUTF8(), -1, SQLITE_TRANSIENT);
        }

        // Full-row insert or update of a USER preset (factory rows are only
        // ever written by syncFactory). The keys are derived here and
        // nowhere else, so they cannot drift from the text they fold.
        bool writeUser(const Preset& p, bool insert)
        {
            auto* s = prepare(insert
                ? "INSERT INTO preset (name, name_key, category, category_key, author, notes, search_key,"
                  " format, params, created_ms, modified_ms, last_used_ms, uuid, sort_key, is_factory)"
                  " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, 0)"
                : "UPDATE preset SET name = ?1, name_key = ?2, category = ?3, category_key = ?4, author = ?5,"
                  " notes = ?6, search_key = ?7, format = ?8, params = ?9, created_ms = ?10, modified_ms = ?11,"
                  " last_used_ms = ?12, sort_key = ?14 WHERE uuid = ?13 AND is_factory = 0");
            if (s == nullptr) return failSql("save");
            bindText(s, 1, p.name);
            bindText(s, 2, fold(p.name));
            bindText(s, 3, p.category);
            bindText(s, 4, fold(p.category));
            bindText(s, 5, p.author);
            bindText(s, 6, p.notes);
            bindText(s, 7, searchKey(p));
            sqlite3_bind_int(s, 8, p.format);
            const auto json = paramsToJson(p.params);
            sqlite3_bind_text(s, 9, json.c_str(), (int) json.size(), SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 10, p.createdMs);
            sqlite3_bind_int64(s, 11, p.modifiedMs);
            sqlite3_bind_int64(s, 12, p.lastUsedMs);
            bindText(s, 13, p.uuid);
            bindText(s, 14, sortKey(p.name));
            const int rc = sqlite3_step(s);
            sqlite3_reset(s);
            sqlite3_clear_bindings(s);
            if (rc != SQLITE_DONE) return failSql("save");
            return insert || sqlite3_changes(db) == 1 || fail("preset not found");
        }

        bool writeTags(const juce::String& uuid, TagMask tags)
        {
            auto* del = prepare("DELETE FROM tag WHERE preset_uuid = ?1");
            auto* ins = prepare("INSERT INTO tag (preset_uuid, color) VALUES (?1, ?2)");
            if (del == nullptr || ins == nullptr) return failSql("tags");
            bindText(del, 1, uuid);
            int rc = sqlite3_step(del);
            sqlite3_reset(del);
            sqlite3_clear_bindings(del);
            if (rc != SQLITE_DONE) return failSql("tags");
            for (int c = 0; c < kNumTags; ++c)
            {
                if ((tags & tagBit(static_cast<Tag>(c))) == 0) continue;
                bindText(ins, 1, uuid);
                sqlite3_bind_int(ins, 2, c);
                rc = sqlite3_step(ins);
                sqlite3_reset(ins);
                sqlite3_clear_bindings(ins);
                if (rc != SQLITE_DONE) return failSql("tags");
            }
            return true;
        }

        bool nameTaken(const juce::String& name, const juce::String& ignoreUuid)
        {
            auto* s = prepare("SELECT 1 FROM preset WHERE name_key = ?1 AND uuid <> ?2 LIMIT 1");
            if (s == nullptr) return false;
            bindText(s, 1, fold(name));
            bindText(s, 2, ignoreUuid);
            const bool taken = sqlite3_step(s) == SQLITE_ROW;
            sqlite3_reset(s);
            sqlite3_clear_bindings(s);
            return taken;
        }

        juce::String uniqueName(const juce::String& wanted, const juce::String& ignoreUuid)
        {
            const auto base = wanted.trim().isEmpty() ? juce::String("Untitled") : wanted.trim();
            if (!nameTaken(base, ignoreUuid)) return base;

            // "Hall 2" taken continues at "Hall 3", not "Hall 2 2".
            auto stem = base;
            int n = 2;
            const auto lastSpace = base.lastIndexOfChar(' ');
            if (lastSpace > 0)
            {
                const auto tail = base.substring(lastSpace + 1);
                if (tail.length() <= 6 && tail.containsOnly("0123456789") && tail.isNotEmpty())
                {
                    stem = base.substring(0, lastSpace).trimEnd();
                    n = juce::jmax(2, tail.getIntValue() + 1);
                }
            }
            for (int i = 0; i < 100000; ++i, ++n)
            {
                const auto candidate = stem + " " + juce::String(n);
                if (!nameTaken(candidate, ignoreUuid)) return candidate;
            }
            return base + " " + juce::Uuid().toString().substring(0, 8);
        }

        // Why a user mutation matched no row, for lastError().
        bool failNotUser(const juce::String& uuid)
        {
            const auto p = load(uuid);
            if (!p) return fail("preset not found");
            return fail("\"" + p->name + "\" is a factory preset and cannot be changed");
        }

        // RAII transaction. IMMEDIATE, never DEFERRED: a deferred transaction
        // that reads and then writes must upgrade its lock, and in WAL mode an
        // upgrade that finds another process's newer commit fails with
        // SQLITE_BUSY at once — the busy handler cannot help, because waiting
        // would not make this transaction's snapshot current. Taking the write
        // lock up front is what makes busy_timeout cover every mutation.
        // Measured by the probe's race (five processes, 50 saves each, all
        // under one name, writing concurrently for ~200 ms): with plain BEGIN
        // 226-231 of the 250 saves failed with SQLITE_BUSY over three runs;
        // with BEGIN IMMEDIATE, none, and the 250 names came out "Race" ..
        // "Race 250" — uniqueName() runs inside the same transaction as the
        // INSERT, so two processes cannot both claim "Race 7".
        struct Txn
        {
            Impl& I;
            bool active = false;
            explicit Txn(Impl& impl) : I(impl)
            {
                active = I.exec("BEGIN IMMEDIATE");
                if (!active) I.failSql("cannot lock the preset database");
            }
            ~Txn() { if (active) I.exec("ROLLBACK"); }
            bool commit()
            {
                if (!active) return false;
                active = false;
                if (I.exec("COMMIT")) return true;
                const auto why = I.sqliteError();
                I.exec("ROLLBACK");
                return I.fail("cannot save: " + why);
            }
            JUCE_DECLARE_NON_COPYABLE(Txn)
        };

        // Load, check it is a user preset, change, write — the shape of
        // every single-field user mutation, in one IMMEDIATE transaction so
        // the check and the write see the same row.
        template <typename Fn>
        bool mutateUser(const juce::String& uuid, Fn&& change)
        {
            if (db == nullptr) return fail("no database");
            Txn t(*this);
            if (!t.active) return false;
            auto p = load(uuid);
            if (!p)            return fail("preset not found");
            if (p->isFactory)  return failNotUser(uuid);
            if (!change(*p))   return false;
            p->modifiedMs = nowMs();
            if (!writeUser(*p, false) || !t.commit()) return false;
            ++revision;
            return true;
        }
    };

    // =======================================================================

    PresetStore::PresetStore() : impl_(std::make_unique<Impl>())
    {
        impl_->open(defaultLocation());
    }

    PresetStore::~PresetStore() = default;

    juce::File PresetStore::defaultLocation()
    {
        if (const char* p = std::getenv("HRVB_PRESETS_DB"); p != nullptr && *p != 0)
            return juce::File(juce::String::fromUTF8(p));
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("Application Support/HardwareReverb/Presets.db");
    }

    bool PresetStore::isOpen() const              { return impl_->db != nullptr; }
    bool PresetStore::isPersistent() const        { return impl_->db != nullptr && impl_->persistent; }
    juce::String PresetStore::lastError() const   { return impl_->error; }
    juce::File PresetStore::file() const          { return impl_->persistent ? impl_->path : juce::File(); }

    // ---- factory ----------------------------------------------------------

    void PresetStore::syncFactory(const std::vector<Preset>& bank, int bankRevision)
    {
        auto& I = *impl_;
        if (I.db == nullptr) return;

        int valid = 0;
        for (const auto& p : bank) valid += p.isValid() ? 1 : 0;

        // The cheap path, taken on every open after the first: two indexed
        // reads, no write lock, nothing committed (the probe asserts that
        // PRAGMA data_version on a second connection does not move). The row
        // count guards against a factory row removed behind our back.
        {
            auto* s = I.prepare("SELECT (SELECT value FROM meta WHERE key = 'factory_revision'),"
                                " (SELECT COUNT(*) FROM preset WHERE is_factory = 1)");
            if (s != nullptr && sqlite3_step(s) == SQLITE_ROW)
            {
                const bool revMatches = sqlite3_column_type(s, 0) != SQLITE_NULL
                                        && sqlite3_column_int(s, 0) == bankRevision;
                const bool countMatches = sqlite3_column_int(s, 1) == valid;
                sqlite3_reset(s);
                if (revMatches && countMatches) return;
            }
            else if (s != nullptr) sqlite3_reset(s);
        }

        Impl::Txn t(I);
        if (!t.active) return;

        // An upsert, NOT "INSERT OR REPLACE": REPLACE deletes the old row
        // and inserts a new one, and the delete cascades to the tag table —
        // every colour a user put on a factory preset would vanish on each
        // bank update. DO UPDATE keeps the row, so the tags, created_ms and
        // last_used_ms stay. The WHERE on the update skips unchanged rows
        // (no write, no WAL frame) and refuses to convert a user row that
        // somehow holds a factory uuid.
        auto* up = I.prepare(
            "INSERT INTO preset (uuid, name, name_key, category, category_key, author, notes, search_key,"
            " format, params, sort_key, is_factory, created_ms, modified_ms, last_used_ms)"
            " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, 1, 0, 0, 0)"
            " ON CONFLICT(uuid) DO UPDATE SET name = excluded.name, name_key = excluded.name_key,"
            " sort_key = excluded.sort_key,"
            " category = excluded.category, category_key = excluded.category_key, author = excluded.author,"
            " notes = excluded.notes, search_key = excluded.search_key, format = excluded.format,"
            " params = excluded.params"
            " WHERE preset.is_factory = 1 AND (preset.name IS NOT excluded.name"
            " OR preset.category IS NOT excluded.category OR preset.author IS NOT excluded.author"
            " OR preset.notes IS NOT excluded.notes OR preset.format IS NOT excluded.format"
            " OR preset.params IS NOT excluded.params OR preset.search_key IS NOT excluded.search_key"
            " OR preset.sort_key IS NOT excluded.sort_key)");
        if (up == nullptr) { I.failSql("factory sync"); return; }

        std::vector<juce::String> keep;
        for (const auto& b : bank)
        {
            if (!b.isValid()) continue;
            Preset p = b;
            p.name = p.name.trim();
            p.category = p.category.trim();
            Impl::bindText(up, 1, p.uuid);
            Impl::bindText(up, 2, p.name);
            Impl::bindText(up, 3, fold(p.name));
            Impl::bindText(up, 4, p.category);
            Impl::bindText(up, 5, fold(p.category));
            Impl::bindText(up, 6, p.author);
            Impl::bindText(up, 7, p.notes);
            Impl::bindText(up, 8, searchKey(p));
            sqlite3_bind_int(up, 9, p.format);
            const auto json = paramsToJson(p.params);
            sqlite3_bind_text(up, 10, json.c_str(), (int) json.size(), SQLITE_TRANSIENT);
            Impl::bindText(up, 11, sortKey(p.name));
            const int rc = sqlite3_step(up);
            sqlite3_reset(up);
            sqlite3_clear_bindings(up);
            if (rc != SQLITE_DONE) { I.failSql("factory sync"); return; }
            keep.push_back(p.uuid);
        }

        // Factory rows the bank no longer has; their tags cascade with them.
        // The uuids to keep are bound one per parameter, not as one JSON array
        // through json_each: SQLite's JSON functions are only built in from
        // 3.38, and the copy Windows ships (winsqlite3) may be older. The
        // statement text depends only on the bank's size, so it is cached.
        std::string delSql = "DELETE FROM preset WHERE is_factory = 1";
        if (!keep.empty())
        {
            delSql += " AND uuid NOT IN (";
            for (size_t i = 0; i < keep.size(); ++i)
                delSql += (i == 0 ? "?" : ", ?") + std::to_string(i + 1);
            delSql += ")";
        }
        auto* del = I.prepare(delSql);
        auto* meta = I.prepare("INSERT INTO meta (key, value) VALUES ('factory_revision', ?1)"
                               " ON CONFLICT(key) DO UPDATE SET value = excluded.value");
        if (del == nullptr || meta == nullptr) { I.failSql("factory sync"); return; }
        for (size_t i = 0; i < keep.size(); ++i)
            Impl::bindText(del, (int) i + 1, keep[i]);
        int rc = sqlite3_step(del);
        sqlite3_reset(del);
        sqlite3_clear_bindings(del);
        if (rc != SQLITE_DONE) { I.failSql("factory sync"); return; }
        sqlite3_bind_int(meta, 1, bankRevision);
        rc = sqlite3_step(meta);
        sqlite3_reset(meta);
        sqlite3_clear_bindings(meta);
        if (rc != SQLITE_DONE) { I.failSql("factory sync"); return; }

        if (t.commit()) ++I.revision;
    }

    // ---- reads ------------------------------------------------------------

    std::vector<Preset> PresetStore::query(const Query& q) const
    {
        auto& I = *impl_;
        std::vector<Preset> out;

        // Only fixed fragments are concatenated; every value is bound. At most
        // 4 sorts x 2^4 filter combinations = 64 distinct statements, each
        // prepared once and cached.
        std::string sql = std::string("SELECT ") + kRowColumns + " FROM preset p WHERE 1";

        // instr() on the folded search key, not LIKE: instr has no
        // metacharacters, so "100%" and "a_b" match literally with nothing to
        // escape, and the fold makes it Unicode-case-insensitive where LIKE
        // is ASCII-only. Control characters (the key's field separator among
        // them) are removed from the needle so it cannot span two fields.
        juce::String needle;
        for (auto c : q.text.trim())
            if (c >= 0x20) needle += juce::String::charToString(c);
        needle = fold(needle);
        if (needle.isNotEmpty())       sql += " AND instr(p.search_key, :text) > 0";
        const auto cat = fold(q.category.trim());
        if (cat.isNotEmpty())          sql += " AND p.category_key = :cat";
        if (q.source == Source::factory) sql += " AND p.is_factory = 1";
        if (q.source == Source::user)    sql += " AND p.is_factory = 0";
        const auto tags = static_cast<int>(q.anyOfTags & kAllTags);
        if (tags != 0)
            sql += " AND EXISTS (SELECT 1 FROM tag t WHERE t.preset_uuid = p.uuid AND ((1 << t.color) & :tags) <> 0)";

        // sort_key, name_key, uuid ends every order, so it is total: the
        // uuid is unique, and decides the same way on every call and in every
        // process when two names fold alike (a factory "Alpha" and a user
        // "alpha" can coexist).
        switch (q.sort)
        {
            case Sort::name:     sql += " ORDER BY p.sort_key, p.name_key, p.uuid"; break;
            // Uncategorised last: an empty group at the top of a category
            // sort pushes every real group below the fold.
            case Sort::category: sql += " ORDER BY (p.category_key = ''), p.category_key, p.sort_key, p.name_key, p.uuid"; break;
            // Never used (0) sorts last, newest first.
            case Sort::recent:   sql += " ORDER BY p.last_used_ms DESC, p.sort_key, p.name_key, p.uuid"; break;
            // Factory rows have created_ms 0, so they follow every user preset.
            case Sort::created:  sql += " ORDER BY p.created_ms DESC, p.sort_key, p.name_key, p.uuid"; break;
        }

        auto* s = I.prepare(sql);
        if (s == nullptr) { I.failSql("search"); return out; }
        if (needle.isNotEmpty()) Impl::bindText(s, sqlite3_bind_parameter_index(s, ":text"), needle);
        if (cat.isNotEmpty())    Impl::bindText(s, sqlite3_bind_parameter_index(s, ":cat"), cat);
        if (tags != 0)           sqlite3_bind_int(s, sqlite3_bind_parameter_index(s, ":tags"), tags);

        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW)
            out.push_back(Impl::readRow(s));
        if (rc != SQLITE_DONE) I.failSql("search");
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        return out;
    }

    std::optional<Preset> PresetStore::get(const juce::String& uuid) const
    {
        return impl_->load(uuid);
    }

    juce::StringArray PresetStore::categories() const
    {
        // One spelling per folded key: the factory's if it has one (the bank
        // is the house style), else the alphabetically first user spelling.
        juce::StringArray out;
        auto* s = impl_->prepare("SELECT category, category_key FROM preset WHERE category_key <> ''"
                                 " ORDER BY category_key, is_factory DESC, category");
        if (s == nullptr) return out;
        juce::String lastKey;
        while (sqlite3_step(s) == SQLITE_ROW)
        {
            const auto key = Impl::text(s, 1);
            if (key == lastKey) continue;
            lastKey = key;
            out.add(Impl::text(s, 0));
        }
        sqlite3_reset(s);
        return out;
    }

    int PresetStore::count(Source src) const
    {
        const char* sql = src == Source::all     ? "SELECT COUNT(*) FROM preset"
                        : src == Source::factory ? "SELECT COUNT(*) FROM preset WHERE is_factory = 1"
                                                 : "SELECT COUNT(*) FROM preset WHERE is_factory = 0";
        auto* s = impl_->prepare(sql);
        if (s == nullptr) return 0;
        const int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
        sqlite3_reset(s);
        return n;
    }

    // ---- user mutations ---------------------------------------------------

    bool PresetStore::saveNew(Preset& p)
    {
        auto& I = *impl_;
        if (I.db == nullptr) return I.fail("no database");
        if (const auto bad = validateParams(p.params); bad.isNotEmpty()) return I.fail(bad);

        Impl::Txn t(I);
        if (!t.active) return false;

        // Built on a copy so a failed save leaves the caller's preset as it
        // was, not half-assigned.
        Preset n = p;
        n.uuid       = juce::Uuid().toDashedString();
        n.isFactory  = false;
        n.format     = kFormat;          // captured by this build: this build's meanings
        n.name       = I.uniqueName(p.name, {});
        n.category   = n.category.trim();
        n.author     = n.author.trim();
        n.createdMs  = n.modifiedMs = nowMs();
        n.lastUsedMs = 0;
        n.tags       = static_cast<TagMask>(n.tags & kAllTags);

        if (!I.writeUser(n, true) || !I.writeTags(n.uuid, n.tags) || !t.commit()) return false;
        p = n;
        ++I.revision;
        return true;
    }

    bool PresetStore::overwrite(const Preset& p)
    {
        auto& I = *impl_;
        if (I.db == nullptr) return I.fail("no database");
        if (const auto bad = validateParams(p.params); bad.isNotEmpty()) return I.fail(bad);

        Impl::Txn t(I);
        if (!t.active) return false;
        const auto existing = I.load(p.uuid);
        if (!existing)             return I.fail("preset not found");
        if (existing->isFactory)   return I.failNotUser(p.uuid);

        Preset n = p;
        n.name = p.name.trim();
        if (n.name.isEmpty())                 return I.fail("a preset needs a name");
        if (I.nameTaken(n.name, n.uuid))      return I.fail("a preset named \"" + n.name + "\" already exists");
        n.isFactory  = false;
        n.format     = kFormat;
        n.category   = n.category.trim();
        n.author     = n.author.trim();
        n.createdMs  = existing->createdMs;   // identity and history are the row's,
        n.lastUsedMs = existing->lastUsedMs;  // tags have their own call
        n.modifiedMs = nowMs();
        if (!I.writeUser(n, false) || !t.commit()) return false;
        ++I.revision;
        return true;
    }

    bool PresetStore::rename(const juce::String& uuid, const juce::String& newName)
    {
        auto& I = *impl_;
        return I.mutateUser(uuid, [&](Preset& p)
        {
            const auto n = newName.trim();
            if (n.isEmpty())             return I.fail("a preset needs a name");
            if (I.nameTaken(n, uuid))    return I.fail("a preset named \"" + n + "\" already exists");
            p.name = n;
            return true;
        });
    }

    bool PresetStore::setCategory(const juce::String& uuid, const juce::String& category)
    {
        auto& I = *impl_;
        return I.mutateUser(uuid, [&](Preset& p) { p.category = category.trim(); return true; });
    }

    bool PresetStore::setNotes(const juce::String& uuid, const juce::String& notes)
    {
        auto& I = *impl_;
        return I.mutateUser(uuid, [&](Preset& p) { p.notes = notes; return true; });
    }

    bool PresetStore::remove(const juce::String& uuid)
    {
        auto& I = *impl_;
        if (I.db == nullptr) return I.fail("no database");
        auto* s = I.prepare("DELETE FROM preset WHERE uuid = ?1 AND is_factory = 0");
        if (s == nullptr) return I.failSql("delete");
        Impl::bindText(s, 1, uuid);
        const int rc = sqlite3_step(s);
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        if (rc != SQLITE_DONE) return I.failSql("delete");
        // Tags go by ON DELETE CASCADE (foreign_keys=ON, set per connection).
        if (sqlite3_changes(I.db) != 1) return I.failNotUser(uuid);
        ++I.revision;
        return true;
    }

    // ---- any preset -------------------------------------------------------

    bool PresetStore::setTags(const juce::String& uuid, TagMask tags)
    {
        auto& I = *impl_;
        if (I.db == nullptr) return I.fail("no database");
        tags = static_cast<TagMask>(tags & kAllTags);
        Impl::Txn t(I);
        if (!t.active) return false;
        const auto p = I.load(uuid);
        if (!p) return I.fail("preset not found");
        if (p->tags == tags) return true;          // nothing to write, nothing to announce
        if (!I.writeTags(uuid, tags) || !t.commit()) return false;
        ++I.revision;
        return true;
    }

    void PresetStore::markUsed(const juce::String& uuid)
    {
        auto& I = *impl_;
        auto* s = I.prepare("UPDATE preset SET last_used_ms = ?1 WHERE uuid = ?2");
        if (s == nullptr) { I.failSql("mark used"); return; }
        sqlite3_bind_int64(s, 1, nowMs());
        Impl::bindText(s, 2, uuid);
        const int rc = sqlite3_step(s);
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        if (rc != SQLITE_DONE) { I.failSql("mark used"); return; }
        if (sqlite3_changes(I.db) == 1) ++I.revision;
    }

    juce::String PresetStore::tagLabel(Tag tag) const
    {
        const int c = static_cast<int>(tag);
        if (c < 0 || c >= kNumTags) return {};
        auto* s = impl_->prepare("SELECT label FROM tag_label WHERE color = ?1");
        juce::String label;
        if (s != nullptr)
        {
            sqlite3_bind_int(s, 1, c);
            if (sqlite3_step(s) == SQLITE_ROW) label = Impl::text(s, 0);
            sqlite3_reset(s);
            sqlite3_clear_bindings(s);
        }
        return label.isNotEmpty() ? label : juce::String(defaultTagName(tag));
    }

    bool PresetStore::setTagLabel(Tag tag, const juce::String& label)
    {
        auto& I = *impl_;
        const int c = static_cast<int>(tag);
        if (c < 0 || c >= kNumTags) return I.fail("no such tag");
        if (I.db == nullptr) return I.fail("no database");
        const auto l = label.trim();
        auto* s = I.prepare(l.isEmpty()
                                ? "DELETE FROM tag_label WHERE color = ?1"
                                : "INSERT INTO tag_label (color, label) VALUES (?1, ?2)"
                                  " ON CONFLICT(color) DO UPDATE SET label = excluded.label");
        if (s == nullptr) return I.failSql("tag label");
        sqlite3_bind_int(s, 1, c);
        if (l.isNotEmpty()) Impl::bindText(s, 2, l);
        const int rc = sqlite3_step(s);
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        if (rc != SQLITE_DONE) return I.failSql("tag label");
        ++I.revision;
        return true;
    }

    juce::String PresetStore::uniqueName(const juce::String& wanted, const juce::String& ignoreUuid) const
    {
        return impl_->uniqueName(wanted, ignoreUuid);
    }

    // ---- import -----------------------------------------------------------

    PresetStore::ImportResult PresetStore::importPreset(Preset p)
    {
        auto& I = *impl_;
        ImportResult r;
        auto refuse = [&](const juce::String& why) { r.error = why; I.error = why; return r; };

        if (I.db == nullptr)       return refuse("no database");
        if (p.format > kFormat)    return refuse("\"" + p.name.trim() + "\" was made by a newer version of HardwareReverb");
        if (p.format < 1)          return refuse("not a HardwareReverb preset (format " + juce::String(p.format) + ")");
        p.name = p.name.trim();
        if (p.name.isEmpty())      return refuse("the preset has no name");
        if (const auto bad = validateParams(p.params); bad.isNotEmpty()) return refuse(bad);

        Impl::Txn t(I);
        if (!t.active) return refuse(I.error);

        // Identical = same uuid, same name, same values: importing a file
        // twice, or re-importing an export, is a no-op that says so. Any
        // difference keeps both, the newcomer under a fresh uuid, because
        // silently replacing the user's version with a file's is the one
        // outcome that cannot be undone. A factory uuid that differs gets a
        // fresh uuid too, since factory rows are read-only.
        auto uuid = canonicalUuid(p.uuid);
        if (uuid.isNotEmpty())
        {
            if (const auto existing = I.load(uuid))
            {
                if (existing->name == p.name && paramsEqual(existing->params, p.params))
                {
                    r.ok = true;
                    r.duplicate = true;
                    r.uuid = existing->uuid;
                    r.name = existing->name;
                    return r;                         // Txn rolls back: nothing written
                }
                uuid = {};
            }
        }
        if (uuid.isEmpty()) uuid = juce::Uuid().toDashedString();

        Preset n = p;
        n.uuid       = uuid;
        n.isFactory  = false;
        n.name       = I.uniqueName(p.name, {});
        n.category   = n.category.trim();
        n.author     = n.author.trim();
        n.createdMs  = n.modifiedMs = nowMs();
        n.lastUsedMs = 0;
        n.tags       = 0;                             // files never carry tags
        if (!I.writeUser(n, true) || !t.commit()) return refuse(I.error);

        ++I.revision;
        r.ok = true;
        r.uuid = n.uuid;
        r.name = n.name;
        r.renamed = n.name != p.name;
        return r;
    }

    // ---- change tracking --------------------------------------------------

    uint64_t PresetStore::revision() const { return impl_->revision; }

    bool PresetStore::pollExternalChanges()
    {
        // data_version changes when ANOTHER connection commits to the file,
        // never for this connection's own commits (those bump revision
        // directly). One PRAGMA on an open connection: 1.3-1.5 us measured
        // idle (~5 us under load), so polling once per frame is free.
        auto& I = *impl_;
        if (I.db == nullptr || !I.persistent) return false;
        const auto v = I.pragmaInt("PRAGMA data_version");
        if (v < 0 || v == I.dataVersion) return false;
        I.dataVersion = v;
        ++I.revision;
        return true;
    }
}
