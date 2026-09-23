#include <funkgui/core/Env.h>

#include <funkgui/core/Config.h>

#include <cstdlib>
#include <forward_list>
#include <map>
#include <mutex>
#include <string>

namespace funkgui
{
    namespace
    {
        struct EnvCache
        {
            std::mutex mutex;
            std::map<std::string, const char*, std::less<>> values;   // unprefixed name -> cached value (or nullptr)
            std::forward_list<std::string> storage;                   // only grows: returned pointers stay valid
        };

        EnvCache& cache()
        {
            // Never destroyed, like UiPreferences and the frame pump: a plugin binary is unloaded while statics are
            // torn down in an order nothing here controls, and a caller may still hold a returned pointer.
            static EnvCache* const c = new EnvCache();
            return *c;
        }
    }

    const char* env(std::string_view name)
    {
        EnvCache& c = cache();
        const std::lock_guard<std::mutex> lock(c.mutex);
        if (const auto it = c.values.find(name); it != c.values.end())
            return it->second;

        const std::string full = std::string(config::kEnvPrefix) + std::string(name);
        const char* kept = nullptr;
        if (const char* raw = std::getenv(full.c_str()))
        {
            c.storage.emplace_front(raw);
            kept = c.storage.front().c_str();
        }
        c.values.emplace(std::string(name), kept);
        return kept;
    }

    void envReload()
    {
        EnvCache& c = cache();
        const std::lock_guard<std::mutex> lock(c.mutex);
        c.values.clear();
    }
}
