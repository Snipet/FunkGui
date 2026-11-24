#include <funkgui/desktop/PrefsFile.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>
#include <utility>

// The preferences' text file (v0.28.0; desktop/PrefsFile.h has the format and the rules). std::filesystem for the
// folder and the rename (a path is UTF-8 here and wide on Windows: std::filesystem::u8path's job), C stdio for the
// bytes.

namespace funkgui::desktop
{
    namespace
    {
        constexpr char kFirstLine[] = "# FunkGui preferences 1";

        std::filesystem::path fromUtf8(const std::string& utf8)
        {
            // C++20: a char8_t string is the UTF-8 one whatever the system's narrow encoding.
            return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        }

        std::FILE* openFile(const std::filesystem::path& p, bool write)
        {
#if defined(_WIN32)
            return ::_wfopen(p.c_str(), write ? L"wb" : L"rb");
#else
            return std::fopen(p.c_str(), write ? "wb" : "rb");
#endif
        }

        bool storable(const std::string& key)
        {
            return !key.empty() && key.front() != '#' && key.find_first_of("=\r\n") == std::string::npos;
        }

        std::string escaped(const std::string& value)
        {
            std::string out;
            for (const char c : value)
            {
                if (c == '\\')
                    out += "\\\\";
                else if (c == '\n')
                    out += "\\n";
                else if (c == '\r')
                    out += "\\r";
                else
                    out.push_back(c);
            }
            return out;
        }

        std::string unescaped(const std::string& text)
        {
            std::string out;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '\\' && i + 1 < text.size()
                    && (text[i + 1] == '\\' || text[i + 1] == 'n' || text[i + 1] == 'r'))
                {
                    ++i;
                    out.push_back(text[i] == 'n' ? '\n' : text[i] == 'r' ? '\r' : '\\');
                }
                else
                    out.push_back(text[i]);
            }
            return out;
        }

        using Values = std::map<std::string, std::string>;

        // The file's keys and values; empty for a file that is missing or cannot be read.
        Values readFile(const std::filesystem::path& path)
        {
            Values values;
            std::FILE* f = openFile(path, false);
            if (f == nullptr)
                return values;
            std::string text;
            char buf[4096];
            for (std::size_t n = std::fread(buf, 1, sizeof buf, f); n > 0; n = std::fread(buf, 1, sizeof buf, f))
                text.append(buf, n);
            std::fclose(f);
            std::size_t at = 0;
            while (at < text.size())
            {
                std::size_t end = text.find('\n', at);
                if (end == std::string::npos)
                    end = text.size();
                std::string line = text.substr(at, end - at);
                at = end + 1;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();                 // a file a Windows editor saved
                if (line.empty() || line.front() == '#')
                    continue;
                const std::size_t eq = line.find('=');
                if (eq == std::string::npos || eq == 0)
                    continue;
                values[line.substr(0, eq)] = unescaped(line.substr(eq + 1));
            }
            return values;
        }

        bool writeFile(const std::filesystem::path& path, const Values& values)
        {
            std::error_code ec;
            if (path.has_parent_path())
                std::filesystem::create_directories(path.parent_path(), ec);     // there already is no error
            std::filesystem::path tmp = path;
            tmp += ".partial";
            std::FILE* f = openFile(tmp, true);
            if (f == nullptr)
                return false;
            std::string text = kFirstLine;
            text.push_back('\n');
            for (const auto& [key, value] : values)
            {
                text += key;
                text.push_back('=');
                text += escaped(value);
                text.push_back('\n');
            }
            const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
            const bool closed = std::fclose(f) == 0;
            if (written && closed)
            {
                std::filesystem::rename(tmp, path, ec);          // replaces the file there, on Windows too
                if (!ec)
                    return true;
            }
            std::filesystem::remove(tmp, ec);
            return false;
        }

        class FileBackend final : public UiPreferences::Backend
        {
        public:
            explicit FileBackend(std::string path) : path_(fromUtf8(path)), values_(readFile(path_)) {}

            bool read(const char* key, std::string& value) const override
            {
                if (key == nullptr)
                    return false;
                const auto it = values_.find(key);
                if (it == values_.end())
                    return false;
                value = it->second;
                return true;
            }

            void write(const char* key, const std::string& value) override
            {
                if (key == nullptr || !storable(key))
                    return;
                values_[key] = value;
                static_cast<void>(writeFile(path_, values_));   // kept in memory when the file cannot be written
            }

            bool reload() override
            {
                Values now = readFile(path_);
                if (now == values_)
                    return false;
                values_ = std::move(now);
                return true;
            }

        private:
            std::filesystem::path path_;
            Values values_;
        };
    }

    std::unique_ptr<UiPreferences::Backend> prefsFileBackend(std::string path)
    {
        return std::make_unique<FileBackend>(std::move(path));
    }
}
