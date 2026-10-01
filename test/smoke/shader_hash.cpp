// FUNKGUI_TEST name=fg.shader.hash timeout=300 gpu=1 links=harness
//
// fg.shader.hash (FCompressor docs/design/02-funkgui-and-ui.md §1.5, §3.11; 03 §2.5): fingerprints the shader binaries
// FunkGuiShaders compiled with the selected shaderc and embedded as ${FUNKGUI_GENERATED_DIR}/funkgui/shaders/
// {vs_ui,fs_ui}.<profile>.h: Metal (mtl, the macOS renderer) and, from v0.11.0, SPIR-V (spv, the Linux Vulkan
// renderer). Every host compiles both, and shaderc's output depends on the pinned bgfx alone, so the golden rows are
// the same on macOS and Linux. A changed shaders/*.sc moves the golden hashes (a DRIFT candidate). A shaderc built from
// another bgfx fails a spec row (03 §2.5; S0 review R-G1 #9): the stamp of the shaderc in use, which
// cmake/FunkGuiDeps.cmake reads and <funkgui/shaders/shaderc_stamp.h> carries, must name the pinned bgfx.cmake SHA. The
// bgfx chunk magic is a spec row too.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <funkgui/shaders/fs_ui.mtl.h>
#include <funkgui/shaders/fs_ui.spv.h>
#include <funkgui/shaders/shaderc_stamp.h>
#include <funkgui/shaders/vs_ui.mtl.h>
#include <funkgui/shaders/vs_ui.spv.h>
#include <funkgui/test/Harness.h>

namespace T = funkgui::test;

namespace
{
    // bgfx shader binaries start with a four-character chunk code: 'V','S','H',<version> or 'F','S','H',<version>.
    bool magic(const uint8_t* bin, std::size_t n, char stage)
    {
        return n > 4 && bin[0] == static_cast<uint8_t>(stage) && bin[1] == 'S' && bin[2] == 'H';
    }

    // One profile's rows. The Metal rows keep their v0.1.0 keys (shader.<name>.*); later profiles add their extension
    // (shader.<name>.<ext>.*).
    void profile(T::Probe& P, const char* ext, const uint8_t* vs, std::size_t vsBytes, const uint8_t* fs,
                 std::size_t fsBytes)
    {
        const std::string sfx = *ext != '\0' ? std::string(".") + ext : std::string();
        P.eq("shader.vs_ui" + sfx + ".magic", magic(vs, vsBytes, 'V'), 1);
        P.eq("shader.fs_ui" + sfx + ".magic", magic(fs, fsBytes, 'F'), 1);
        P.hash("shader.vs_ui" + sfx + ".hash", T::fnv1a(vs, vsBytes));
        P.num("shader.vs_ui" + sfx + ".bytes", static_cast<double>(vsBytes), T::Tol::exact());
        P.hash("shader.fs_ui" + sfx + ".hash", T::fnv1a(fs, fsBytes));
        P.num("shader.fs_ui" + sfx + ".bytes", static_cast<double>(fsBytes), T::Tol::exact());
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.shader.hash", "", argc, argv);
    std::printf("INFO     shaderc stamp '%s', pin '%s'\n", FUNKGUI_SHADERC_STAMP, FUNKGUI_SHADERC_PIN_STAMP);
    P.eq("shaderc.stamp_is_pin", std::string_view(FUNKGUI_SHADERC_STAMP) == FUNKGUI_SHADERC_PIN_STAMP, 1);
    profile(P, "", vs_ui_mtl, sizeof vs_ui_mtl, fs_ui_mtl, sizeof fs_ui_mtl);
    profile(P, "spv", vs_ui_spv, sizeof vs_ui_spv, fs_ui_spv, sizeof fs_ui_spv);
    return P.finish();
}
