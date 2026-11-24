#pragma once

// A FileRequest as SDL3's file dialogs take it (v0.28.0): the pure part of DesktopHost::chooseFiles, plain C++ with no
// SDL header, so its rules are tested without a dialog (fg.desktop.files). The rules are HostServices::chooseFiles'
// (panel/HostServices.h), as EditorHost and HeadlessHost apply them:
// - The pattern. FunkGui's is wildcards separated by ';' ("*.fcmppreset", "*.wav;*.aiff"; empty: every file). SDL's
//   filter is extensions separated by ';' ("wav;aiff"), of letters, digits, hyphens, underscores and periods only. A
//   wildcard "*.<ext>" with such an extension is taken; anything else in the pattern ("*", "name.*", "*.a?") is left
//   out, and a pattern that leaves nothing gives no filter, which shows every file.
// - A save's extension: when the pattern is a single "*.<ext>", the chosen path ends in ".<ext>" (compared without
//   ASCII case): a path that does not loses what follows the last '.' of its file name and gains it.
// - A save's suggested name loses the characters a file name cannot hold on some system (the separators, the
//   characters Windows refuses, control characters) and the spaces and periods at its ends.

#include <cstddef>
#include <string>
#include <string_view>

namespace funkgui::desktop
{
    // FunkGui's pattern as SDL's filter pattern: "" when nothing in it is an extension SDL can filter by.
    inline std::string sdlFilterPattern(std::string_view pattern)
    {
        std::string out;
        std::size_t at = 0;
        while (at <= pattern.size())
        {
            std::size_t end = pattern.find(';', at);
            if (end == std::string_view::npos)
                end = pattern.size();
            std::string_view part = pattern.substr(at, end - at);
            while (!part.empty() && part.front() == ' ')
                part.remove_prefix(1);
            while (!part.empty() && part.back() == ' ')
                part.remove_suffix(1);
            if (part.size() >= 3 && part[0] == '*' && part[1] == '.')
            {
                const std::string_view ext = part.substr(2);
                bool legal = true;
                for (const char c : ext)
                    legal = legal && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                                      || c == '-' || c == '_' || c == '.');
                if (legal)
                {
                    if (!out.empty())
                        out.push_back(';');
                    out.append(ext);
                }
            }
            at = end + 1;
        }
        return out;
    }

    // The extension a save must end in: ".ext" for a pattern that is a single "*.ext", else "" (no rule).
    inline std::string_view saveExtension(std::string_view pattern)
    {
        if (pattern.size() < 3 || pattern[0] != '*' || pattern[1] != '.')
            return {};
        const std::string_view ext = pattern.substr(1);
        return ext.find_first_of("*?;, ") == std::string_view::npos ? ext : std::string_view{};
    }

    // `path` ending in `ext` (".ext"): unchanged when it does already (ASCII case aside) or `ext` is empty, else what
    // follows the last '.' of its file name is replaced by it, or it is appended.
    inline std::string withExtension(std::string path, std::string_view ext)
    {
        const auto low = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (ext.empty())
            return path;
        if (path.size() >= ext.size())
        {
            bool same = true;
            const std::size_t from = path.size() - ext.size();
            for (std::size_t i = 0; same && i < ext.size(); ++i)
                same = low(path[from + i]) == low(ext[i]);
            if (same)
                return path;
        }
        const std::size_t slash = path.find_last_of("/\\");
        const std::size_t dot = path.rfind('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            path.erase(dot);
        path.append(ext);
        return path;
    }

    // A suggested file name as one a save dialog can start with on every system (the header comment has the rule).
    inline std::string legalFileName(std::string_view name)
    {
        std::string out;
        for (const char c : name)
        {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20u || u == 0x7fu || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"'
                || c == '<' || c == '>' || c == '|')
                continue;
            out.push_back(c);
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
            out.pop_back();
        std::size_t lead = 0;
        while (lead < out.size() && out[lead] == ' ')
            ++lead;
        out.erase(0, lead);
        return out;
    }
}
