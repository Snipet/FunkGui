// FUNKGUI_TEST name=fg.shader.hash timeout=300 gpu=1 links=harness
//
// fg.shader.hash (FCompressor docs/design/02-funkgui-and-ui.md §1.5, §3.11; 03 §2.5): fingerprints the Metal shader
// binaries FunkGuiShaders compiled with the selected shaderc and embedded as ${FUNKGUI_GENERATED_DIR}/funkgui/shaders/
// {vs_ui,fs_ui}.mtl.h. A shaderc built from another bgfx (or a changed shaders/*.sc) moves the golden hash, so the
// mismatch fails a test instead of the renderer. The bgfx chunk magic is a spec row.

#include <cstddef>
#include <cstdint>

#include <funkgui/shaders/fs_ui.mtl.h>
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
    P.eq("shader.vs_ui.magic", magic(vs_ui_mtl, sizeof vs_ui_mtl, 'V'), 1);
    P.eq("shader.fs_ui.magic", magic(fs_ui_mtl, sizeof fs_ui_mtl, 'F'), 1);
    P.hash("shader.vs_ui.hash", T::fnv1a(vs_ui_mtl, sizeof vs_ui_mtl));
    P.num("shader.vs_ui.bytes", static_cast<double>(sizeof vs_ui_mtl), T::Tol::exact());
    P.hash("shader.fs_ui.hash", T::fnv1a(fs_ui_mtl, sizeof fs_ui_mtl));
    P.num("shader.fs_ui.bytes", static_cast<double>(sizeof fs_ui_mtl), T::Tol::exact());
    return P.finish();
}
