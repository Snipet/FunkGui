// FUNKGUI_TEST name=fg.shader.hash timeout=300 gpu=1 links=harness
//
// fg.shader.hash (FCompressor docs/design/02-funkgui-and-ui.md §1.5, §3.11; 03 §2.5): fingerprints the Metal shader
// binaries FunkGuiShaders compiled with the selected shaderc and embedded as ${FUNKGUI_GENERATED_DIR}/funkgui/shaders/
// {vs_ui,fs_ui}.mtl.h. A changed shaders/*.sc moves the golden hashes (a DRIFT candidate). A shaderc built from another
// bgfx fails a spec row (03 §2.5; S0 review R-G1 #9): the stamp of the shaderc in use, which cmake/FunkGuiDeps.cmake
// reads and <funkgui/shaders/shaderc_stamp.h> carries, must name the pinned bgfx.cmake SHA. The bgfx chunk magic is a
// spec row too.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include <funkgui/shaders/fs_ui.mtl.h>
#include <funkgui/shaders/shaderc_stamp.h>
#include <funkgui/shaders/vs_ui.mtl.h>
#include <funkgui/test/Harness.h>

namespace T = funkgui::test;

namespace
{
    // bgfx shader binaries start with a four-character chunk code: 'V','S','H',<version> or 'F','S','H',<version>.
    bool magic(const uint8_t* bin, std::size_t n, char stage)
    {
        return n > 4 && bin[0] == static_cast<uint8_t>(stage) && bin[1] == 'S' && bin[2] == 'H';
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.shader.hash", "", argc, argv);
    std::printf("INFO     shaderc stamp '%s', pin '%s'\n", FUNKGUI_SHADERC_STAMP, FUNKGUI_SHADERC_PIN_STAMP);
    P.eq("shaderc.stamp_is_pin", std::string_view(FUNKGUI_SHADERC_STAMP) == FUNKGUI_SHADERC_PIN_STAMP, 1);
    P.eq("shader.vs_ui.magic", magic(vs_ui_mtl, sizeof vs_ui_mtl, 'V'), 1);
    P.eq("shader.fs_ui.magic", magic(fs_ui_mtl, sizeof fs_ui_mtl, 'F'), 1);
    P.hash("shader.vs_ui.hash", T::fnv1a(vs_ui_mtl, sizeof vs_ui_mtl));
    P.num("shader.vs_ui.bytes", static_cast<double>(sizeof vs_ui_mtl), T::Tol::exact());
    P.hash("shader.fs_ui.hash", T::fnv1a(fs_ui_mtl, sizeof fs_ui_mtl));
    P.num("shader.fs_ui.bytes", static_cast<double>(sizeof fs_ui_mtl), T::Tol::exact());
    return P.finish();
}
