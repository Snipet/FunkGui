#pragma once

// Preferences in the browser (v0.13.0; FunkGui::web; FCompressor ADR-93, web Sprint C): a UiPreferences::Backend over
// window.localStorage, the durable store a JUCE-free build lacks (prefs/UiPreferences.h, "Storage backends"). A
// product calls installLocalStoragePrefs() once, before it constructs its Panel: a Panel may read a preference while
// it is constructed, and setBackend() copies nothing from the store it replaces.
//
// The backend is two parts. WebPrefsBackend is the rule, in plain C++, so it compiles and is tested on every host
// (fg.web.prefs, with a storage of the test's own): an in-memory mirror of the keys that carry the product's prefix.
// - read() answers from the mirror, never from the storage: a getter costs nothing and cannot fail.
// - write() puts the value in the mirror and in the storage, under <prefix><key>. A storage that refuses (a private
//   window, a full quota) loses nothing for this page: the mirror keeps the value, and keeps it across reload(), until
//   a later write of that key is taken.
// - reload() reads the prefixed keys again (another tab of the same origin may have written them) and reports whether
//   any key or value changed. A storage that cannot be read at all leaves the mirror as it is and reports no change.
// WebStorage is what it needs of a store; the localStorage one lives in src/web/WebPrefs.cpp (Emscripten only), where
// every call into the browser is inside try/catch: the property itself throws where storage is forbidden.
//
// This header is plain C++ (it compiles on every host); installLocalStoragePrefs() is defined only in FunkGui::web.
// Main thread only, as UiPreferences is.

#include <funkgui/prefs/UiPreferences.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace funkgui
{
    // A store of text values under text keys, shaped like the browser's Storage.
    class WebStorage
    {
    public:
        using Entries = std::map<std::string, std::string, std::less<>>;

        virtual ~WebStorage() = default;

        // Every entry whose key starts with `prefix`, under its whole key (prefix included), replacing what `out`
        // held. False, and `out` untouched, when the store cannot be read.
        virtual bool readAll(std::string_view prefix, Entries& out) const = 0;

        // Stores `value` under `key`; false when the store refused it.
        virtual bool write(const std::string& key, const std::string& value) = 0;
    };

    class WebPrefsBackend final : public UiPreferences::Backend
    {
    public:
        // Reads the prefixed keys once. `storage` may be null: the mirror alone, for the life of the object. An empty
        // prefix takes every key of the store as a preference.
        WebPrefsBackend(std::unique_ptr<WebStorage> storage, std::string keyPrefix)
            : storage_(std::move(storage)), prefix_(std::move(keyPrefix))
        {
            reload();
        }

        bool read(const char* key, std::string& value) const override
        {
            if (key == nullptr)
                return false;
            const auto it = mirror_.find(std::string_view(key));
            if (it == mirror_.end())
                return false;
            value = it->second;
            return true;
        }

        void write(const char* key, const std::string& value) override
        {
            if (key == nullptr || key[0] == '\0')
                return;
            mirror_.insert_or_assign(key, value);
            if (storage_ != nullptr && storage_->write(prefix_ + key, value))
                unsaved_.erase(key);
            else
                unsaved_.insert(key);                // held here only: reload() must not take it back
        }

        bool reload() override
        {
            WebStorage::Entries stored;
            if (storage_ == nullptr || !storage_->readAll(prefix_, stored))
                return false;                        // nothing to read from: the mirror is all there is
            WebStorage::Entries next;
            for (const auto& [key, value] : stored)
                if (key.size() > prefix_.size() && key.compare(0, prefix_.size(), prefix_) == 0)
                    next.insert_or_assign(key.substr(prefix_.size()), value);
            for (const std::string& key : unsaved_)
                if (const auto held = mirror_.find(key); held != mirror_.end())
                    next.insert_or_assign(key, held->second);
            const bool changed = next != mirror_;
            mirror_ = std::move(next);
            return changed;
        }

        const std::string& keyPrefix() const noexcept { return prefix_; }
        std::size_t size() const noexcept { return mirror_.size(); }             // the keys the mirror holds
        std::size_t unsavedCount() const noexcept { return unsaved_.size(); }    // of them, those the storage refused

    private:
        std::unique_ptr<WebStorage>      storage_;
        std::string                      prefix_;
        WebStorage::Entries              mirror_;    // by the key UiPreferences gives: no prefix
        std::set<std::string, std::less<>> unsaved_;
    };

    // Makes window.localStorage the store of UiPreferences::get(), for a page (FunkGui::web only): a WebPrefsBackend
    // over it, installed with setBackend(), so the theme is read from it and revision() is bumped. Every key is stored
    // as <keyPrefix><key> ("FCompressor.theme"), since an origin's localStorage is shared by every page it serves;
    // null or empty means the product's own, FUNKGUI_PREFS_FOLDER followed by a dot. Where the browser gives no
    // storage, or refuses it, the preferences live for the life of the page, as with UiPreferences::memoryBackend().
    void installLocalStoragePrefs(const char* keyPrefix);
}
