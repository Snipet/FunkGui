#include <funkgui/prefs/UiPreferences.h>

#include "PrefsBackend.h"

#include <funkgui/core/Theme.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

// UiPreferences over a storage backend (v0.12.0). This file is JUCE-free: the class's logic (the theme's cached copy
// and clamp, the generic int keys, the revision) and the in-memory backend. The properties-file backend, file() and
// defaultFile() are src/juce/PrefsFileBackend.cpp; which backend a new store starts with is
// detail::makeDefaultPrefsBackend() (src/prefs/PrefsBackend.h). With JUCE every call reaches the same
// juce::PropertiesFile calls, in the same order, as before the split, so the file's bytes are unchanged
// (fg.prefs.check's golden rows).

namespace funkgui
{
    namespace
    {
        constexpr const char* kThemeKey = "theme";

        char lowerAscii(char c) noexcept
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        // "Theme" and "THEME" are the theme (fg.prefs.check: prefs.int.theme_key_ignores_case).
        bool isThemeKey(const char* key) noexcept
        {
            const char* t = kThemeKey;
            for (; *key != '\0' && *t != '\0'; ++key, ++t)
                if (lowerAscii(*key) != *t)
                    return false;
            return *key == '\0' && *t == '\0';
        }

        int clampTo(int v, int lo, int hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // A decimal integer that fits an int ("-12", "48"), and nothing else: a lenient reader takes "48abc" as 48 and
        // "abc" as 0, which would turn a damaged file into a real setting.
        bool parseInt(const std::string& t, int& out)
        {
            if (t.empty())
                return false;
            const char* first = t.data();
            const char* last = t.data() + t.size();
            const auto [end, ec] = std::from_chars(first, last, out);
            return ec == std::errc{} && end == last;
        }

        // The theme's own reading, kept as it always was: the properties file's getIntValue, which for JUCE's UTF-8
        // strings is atoi. Leading white space, an optional '+' or '-', then digits up to the first character that is
        // not one; nothing readable is 0. "3abc" is 3, "+1" is 1, "abc" is 0; the clamp follows. (atoi is strtol
        // narrowed to int on the platforms this ran on, which is what this does.)
        int lenientInt(const std::string& t) noexcept
        {
            return static_cast<int>(std::strtol(t.c_str(), nullptr, 10));
        }

        // The store without JUCE, and anyone's sandbox: nothing outside the process can change it.
        class MemoryBackend final : public UiPreferences::Backend
        {
        public:
            bool read(const char* key, std::string& value) const override
            {
                const auto it = values_.find(key);
                if (it == values_.end())
                    return false;
                value = it->second;
                return true;
            }

            void write(const char* key, const std::string& value) override { values_[key] = value; }

            bool reload() override { return false; }

        private:
            std::map<std::string, std::string, std::less<>> values_;
        };
    }

    UiPreferences& UiPreferences::get()
    {
        // Deliberately never destroyed, for the same reason the frame pump
        // isn't: a plugin binary is unloaded while other statics are being
        // torn down in an order nothing here controls, and a PropertiesFile
        // destructor writes to disk. Every change is saved eagerly instead, so
        // there is nothing left to flush at shutdown.
        static UiPreferences* prefs = new UiPreferences();
        return *prefs;
    }

    std::unique_ptr<UiPreferences::Backend> UiPreferences::memoryBackend()
    {
        return std::make_unique<MemoryBackend>();
    }

    UiPreferences::UiPreferences()
        : backend_(detail::makeDefaultPrefsBackend())
    {
        theme_ = readTheme();
    }

    int UiPreferences::readTheme() const
    {
        std::string text;
        const int stored = backend_ != nullptr && backend_->read(kThemeKey, text) ? lenientInt(text) : 0;
        return clampTo(stored, 0, Theme::kCount - 1);
    }

    void UiPreferences::setBackend(std::unique_ptr<Backend> backend)
    {
        backend_ = backend != nullptr ? std::move(backend) : detail::makeDefaultPrefsBackend();
        theme_ = readTheme();
        ++revision_;                                     // another store: every preference may have moved
    }

    void UiPreferences::reload()
    {
        if (backend_ == nullptr) return;
        const bool changed = backend_->reload();
        theme_ = readTheme();
        // Any key, not just the theme: an editor follows every preference.
        if (changed) ++revision_;
    }

    void UiPreferences::setTheme(int idx)
    {
        const int clamped = clampTo(idx, 0, Theme::kCount - 1);
        if (clamped == theme_) return;

        theme_ = clamped;
        ++revision_;

        // Written through immediately rather than at destruction: a host
        // that is force-quit, or a plugin unloaded mid-teardown, must not
        // lose the choice the user just made. Two hosts open at once are
        // last-writer-wins, which is the right semantics for a preference.
        if (backend_ != nullptr)
            backend_->write(kThemeKey, std::to_string(theme_));
    }

    int UiPreferences::getInt(const char* key, int fallback, int lo, int hi) const
    {
        if (lo > hi)
            std::swap(lo, hi);
        if (key == nullptr || key[0] == '\0')
            return clampTo(fallback, lo, hi);
        if (isThemeKey(key))
            return clampTo(theme_, lo, hi);
        int v = fallback;
        std::string text;
        if (backend_ != nullptr && backend_->read(key, text) && !parseInt(text, v))
            v = fallback;
        return clampTo(v, lo, hi);
    }

    void UiPreferences::setInt(const char* key, int value)
    {
        if (key == nullptr || key[0] == '\0')
            return;
        if (isThemeKey(key))
        {
            setTheme(value);                             // the theme keeps its clamp and its cached copy
            return;
        }
        if (backend_ == nullptr)
            return;
        const std::string text = std::to_string(value);
        std::string held;
        if (backend_->read(key, held) && held == text)
            return;                                      // already held: no write, no revision
        backend_->write(key, text);                      // written through, as setTheme()
        ++revision_;
    }
}
