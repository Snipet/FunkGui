#include <funkgui/web/WebPrefs.h>

#include <funkgui/core/Config.h>

#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// The localStorage side of web/WebPrefs.h (v0.13.0; FCompressor ADR-93, web Sprint C): the two calls WebPrefsBackend
// makes of its storage, as JavaScript. Everything is inside try/catch: reading window.localStorage throws where the
// browser forbids storage (a sandboxed frame, cookies blocked), and setItem throws when a private window or a full
// quota refuses the write. The rule itself (the mirror, the prefix, what reload reports) is the header's.

// (EM_JS defines a C symbol: file scope. Its body passes through the C preprocessor.)
EM_JS_DEPS(funkgui_web_prefs_deps, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8");

// Every key that starts with `prefix`, with its value, as UTF-8 into `out`: key, NUL, value, NUL, and so on. Returns
// the bytes that takes, written only when `capacity` holds them and one more (the caller asks again with room when it
// does not), or -1 when the storage cannot be read. A key or value that itself holds a NUL is not a preference written
// here: it is left out.
EM_JS(int, funkgui_web_prefs_read, (const char* prefix, char* out, int capacity), {
    try {
        const storage = globalThis.localStorage;
        if (!storage) return -1;
        const wanted = UTF8ToString(prefix);
        const nul = String.fromCharCode(0);
        const found = [];
        for (let i = 0; i < storage.length; ++i) {
            const key = storage.key(i);
            if (key === null || !key.startsWith(wanted) || key.includes(nul)) continue;
            const value = storage.getItem(key);
            if (value !== null && !value.includes(nul)) found.push(key + nul + value + nul);
        }
        const text = found.join(String());
        const bytes = lengthBytesUTF8(text);
        if (bytes < capacity) stringToUTF8(text, out, capacity);
        return bytes;
    } catch (e) {
        return -1;
    }
});

EM_JS(int, funkgui_web_prefs_write, (const char* key, const char* value), {
    try {
        globalThis.localStorage.setItem(UTF8ToString(key), UTF8ToString(value));
        return 1;
    } catch (e) {
        return 0;
    }
});

namespace funkgui
{
    namespace
    {
        class LocalStorage final : public WebStorage
        {
        public:
            bool readAll(std::string_view prefix, Entries& out) const override
            {
                const std::string wanted(prefix);
                std::vector<char> text(1024);
                for (;;)
                {
                    const int capacity = static_cast<int>(text.size());
                    const int bytes = funkgui_web_prefs_read(wanted.c_str(), text.data(), capacity);
                    if (bytes < 0)
                        return false;
                    const bool fitted = static_cast<std::size_t>(bytes) < text.size();
                    text.resize(static_cast<std::size_t>(bytes) + (fitted ? 0u : 1u));
                    if (fitted)
                        break;
                }
                Entries found;
                const char* const end = text.data() + text.size();   // every key and value ends in its NUL before it
                for (const char* p = text.data(); p < end;)
                {
                    std::string key(p);
                    p += key.size() + 1;
                    if (p >= end)
                        break;
                    std::string value(p);
                    p += value.size() + 1;
                    found.insert_or_assign(std::move(key), std::move(value));
                }
                out = std::move(found);
                return true;
            }

            bool write(const std::string& key, const std::string& value) override
            {
                return funkgui_web_prefs_write(key.c_str(), value.c_str()) != 0;
            }
        };
    }

    void installLocalStoragePrefs(const char* keyPrefix)
    {
        std::string prefix = keyPrefix != nullptr && keyPrefix[0] != '\0' ? std::string(keyPrefix)
                                                                          : std::string(config::kPrefsFolder) + ".";
        UiPreferences::get().setBackend(
            std::make_unique<WebPrefsBackend>(std::make_unique<LocalStorage>(), std::move(prefix)));
    }
}
