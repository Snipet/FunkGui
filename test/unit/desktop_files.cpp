// FUNKGUI_TEST name=fg.desktop.files timeout=120 gpu=0 links=harness
//
// fg.desktop.files (v0.28.0): desktop/DesktopFiles.h, the pure part of DesktopHost::chooseFiles (a FileRequest as
// SDL3's file dialogs take it). Header-only and free of SDL, so the rows run in every configuration; the dialogs
// themselves are a person's to try (README, "Desktop").
//   filter.*     FunkGui's wildcards as SDL's filter: "*.ext" taken, several joined by ';', spaces round them
//                dropped, anything SDL cannot filter by left out, nothing left: no filter (every file)
//   extension.*  the extension a save must end in: a single "*.ext" only
//   save.*       a path given that extension: kept when it ends in it (case aside), another extension replaced, none
//                appended, a period in a folder's name left alone, on either separator
//   name.*       a suggested name made legal: separators, the characters Windows refuses and control characters
//                dropped, spaces and periods at the ends dropped
// Spec rows only.

#include <funkgui/desktop/DesktopFiles.h>
#include <funkgui/test/Harness.h>

#include <string>

namespace T = funkgui::test;
namespace D = funkgui::desktop;

int main(int argc, char** argv)
{
    T::Probe P("fg.desktop.files", "", argc, argv);

    P.eq("filter.one", D::sdlFilterPattern("*.fcmppreset") == "fcmppreset", 1);
    P.eq("filter.several", D::sdlFilterPattern("*.wav;*.aiff;*.flac") == "wav;aiff;flac", 1);
    P.eq("filter.spaces", D::sdlFilterPattern(" *.wav ; *.aif ") == "wav;aif", 1);
    P.eq("filter.double_extension", D::sdlFilterPattern("*.tar.gz") == "tar.gz", 1);
    P.eq("filter.empty_is_every_file", D::sdlFilterPattern("").empty(), 1);
    P.eq("filter.star_is_every_file", D::sdlFilterPattern("*").empty(), 1);
    P.eq("filter.unfilterable_left_out", D::sdlFilterPattern("*.wav;name.*;*.a?;*.txt") == "wav;txt", 1);
    P.eq("filter.empty_parts", D::sdlFilterPattern(";;*.wav;") == "wav", 1);
    P.eq("filter.punctuation", D::sdlFilterPattern("*.my-file_1") == "my-file_1", 1);
    P.eq("filter.other_characters_refused", D::sdlFilterPattern("*.w v;*.a+b").empty(), 1);

    P.eq("extension.single", D::saveExtension("*.fcmppreset") == ".fcmppreset", 1);
    P.eq("extension.several_is_no_rule", D::saveExtension("*.wav;*.aiff").empty(), 1);
    P.eq("extension.empty_is_no_rule", D::saveExtension("").empty(), 1);
    P.eq("extension.star_is_no_rule", D::saveExtension("*.*").empty(), 1);
    P.eq("extension.name_is_no_rule", D::saveExtension("preset.txt").empty(), 1);

    P.eq("save.kept", D::withExtension("/a/b/song.wav", ".wav") == "/a/b/song.wav", 1);
    P.eq("save.kept_case_aside", D::withExtension("/a/b/SONG.WAV", ".wav") == "/a/b/SONG.WAV", 1);
    P.eq("save.replaced", D::withExtension("/a/b/song.txt", ".wav") == "/a/b/song.wav", 1);
    P.eq("save.appended", D::withExtension("/a/b/song", ".wav") == "/a/b/song.wav", 1);
    P.eq("save.folder_period_left", D::withExtension("/a/b.c/song", ".wav") == "/a/b.c/song.wav", 1);
    P.eq("save.backslashes", D::withExtension("C:\\a\\b.c\\song", ".wav") == "C:\\a\\b.c\\song.wav", 1);
    P.eq("save.no_rule", D::withExtension("/a/b/song.txt", "") == "/a/b/song.txt", 1);
    P.eq("save.shorter_than_extension", D::withExtension("a", ".wav") == "a.wav", 1);

    P.eq("name.plain", D::legalFileName("My preset.fcmppreset") == "My preset.fcmppreset", 1);
    P.eq("name.separators", D::legalFileName("a/b\\c:d") == "abcd", 1);
    P.eq("name.windows_characters", D::legalFileName("what?*\"<>|.txt") == "what.txt", 1);
    P.eq("name.control_characters", D::legalFileName(std::string("a\tb\nc\x7f") + "d") == "abcd", 1);
    P.eq("name.ends", D::legalFileName("  name . ") == "name", 1);
    P.eq("name.nothing_left", D::legalFileName("/:?").empty(), 1);
    P.eq("name.utf8_kept", D::legalFileName("caf\xC3\xA9 \xE2\x88\x92 1") == "caf\xC3\xA9 \xE2\x88\x92 1", 1);
    return P.finish();
}
