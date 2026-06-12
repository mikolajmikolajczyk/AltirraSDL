// Simulator boot path. Init steps mirror AltirraBridgeServer/main_bridge.cpp's
// InitSimulator(), with two adaptations for the embed target:
//
// (1) Each step is wrapped in EMBED_STEP so a failing one surfaces a readable
//     string in the JS console instead of the opaque `{ excPtr: ... }` that
//     Embind otherwise reports.
// (2) After settings load, the simulator is force-configured to XL + LLEXL +
//     64K — without this, the simulator boots with kernel=0 and the CPU is
//     stuck reading $FFFF at the reset vector.

#include <stdafx.h>

#ifdef __EMSCRIPTEN__

#include "embed_init.h"
#include "audio_tap.h"

#include <emscripten/console.h>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <string>

#include <vd2/system/VDString.h>
#include <vd2/system/registry.h>
#include <vd2/system/error.h>

#include "simulator.h"
#include "gtia.h"
#include "settings.h"
#include "constants.h"
#include "devicemanager.h"
#include "firmwaremanager.h"
#include <at/ataudio/audiooutput.h>

extern ATSimulator g_sim;

// Externs avoid the deep header chase main_bridge.cpp also avoids.
extern VDStringA ATGetConfigDir();
extern void      ATRegistryLoadFromDisk();
extern void      ATInitSaveStateDeserializer();
extern void      ATVFSInstallAtfsHandler();
extern void      ATRegisterDevices(ATDeviceManager& dm);
extern void      ATRegisterDeviceXCmds(ATDeviceManager& dm);
extern void      ATInitDebugger();
extern bool      ATSocketInit();
extern void      ATLoadConfigVars();
extern void      ATOptionsLoad();
extern bool      ATLoadDefaultProfiles();
extern IVDVideoDisplay* ATBridgeCreateNullVideoDisplay();

std::atomic<bool> g_initialized{false};
IVDVideoDisplay*  g_pNullDisplay = nullptr;

// Wrap each init step so a failing one surfaces a readable string in the
// JS console — an uncaught C++ exception otherwise comes through Embind
// as `{ excPtr: 16597640 }` with no symbol.
#define EMBED_STEP(label, code)                                            \
    do {                                                                   \
        try { code; }                                                      \
        catch (const MyError& e) {                                         \
            emscripten_console_errorf("[altirra-embed] " label             \
                " MyError: %s", e.c_str());                                \
            throw;                                                         \
        } catch (const std::exception& e) {                                \
            emscripten_console_errorf("[altirra-embed] " label             \
                " std::exception: %s", e.what());                          \
            throw;                                                         \
        } catch (...) {                                                    \
            emscripten_console_error("[altirra-embed] " label              \
                " threw unknown exception");                               \
            throw;                                                         \
        }                                                                  \
    } while (0)

void EnsureInitialized() {
    if (g_initialized.exchange(true)) return;

    EMBED_STEP("setDefaultKey",
        VDRegistryAppKey::setDefaultKey("AltirraSDL"));
    EMBED_STEP("ATRegistryLoadFromDisk",
        ATRegistryLoadFromDisk());
    EMBED_STEP("ATInitSaveStateDeserializer",
        ATInitSaveStateDeserializer());
    EMBED_STEP("ATVFSInstallAtfsHandler",
        ATVFSInstallAtfsHandler());

    EMBED_STEP("g_sim.Init",
        g_sim.Init());
    EMBED_STEP("g_sim.SetRandomSeed",
        g_sim.SetRandomSeed((uint32)std::rand() ^ ((uint32)std::rand() << 15)));
    EMBED_STEP("g_sim.LoadROMs",
        g_sim.LoadROMs());

    EMBED_STEP("ATBridgeCreateNullVideoDisplay", {
        g_pNullDisplay = ATBridgeCreateNullVideoDisplay();
        g_sim.GetGTIA().SetVideoOutput(g_pNullDisplay);
        g_sim.GetGTIA().SetFrameSkip(true);
    });

    EMBED_STEP("ATRegisterDevices", {
        ATRegisterDevices(*g_sim.GetDeviceManager());
        ATRegisterDeviceXCmds(*g_sim.GetDeviceManager());
    });

    EMBED_STEP("ATSocketInit",     ATSocketInit());
    EMBED_STEP("ATLoadConfigVars", ATLoadConfigVars());
    EMBED_STEP("ATOptionsLoad",    ATOptionsLoad());
    EMBED_STEP("ATLoadDefaultProfiles", ATLoadDefaultProfiles());

    EMBED_STEP("ATSettingsLoadLastProfile",
        ATSettingsLoadLastProfile((ATSettingsCategory)(
            kATSettingsCategory_All
            & ~kATSettingsCategory_FullScreen
            & ~kATSettingsCategory_Input
            & ~kATSettingsCategory_InputMaps
        )));

    EMBED_STEP("ATInitDebugger",    ATInitDebugger());

    // Hook audio tap so we can pull samples to JS Web Audio. Must come
    // after g_sim.Init() since the audio output is constructed there.
    EMBED_STEP("audio tap", {
        if (auto* out = g_sim.GetAudioOutput())
            out->SetAudioTap(&g_audioTap);
    });

    // Override whatever defaults settings.ini didn't set: force XL hardware
    // + AltirraOS LLE XL kernel + 64K. Without this the simulator boots
    // with kernel=0 → no reset vector → CPU stuck at $FFFC reading $FFFF.
    EMBED_STEP("force XL+LLEXL+64K", {
        g_sim.SetHardwareMode(kATHardwareMode_800XL);
        g_sim.SetKernel(kATFirmwareId_Kernel_LLEXL);
        g_sim.SetBasic(0);
        g_sim.SetMemoryMode(kATMemoryMode_64K);
        // Default ATMemoryClearMode_DRAM3 paints RAM with the
        // `FF 00 FF 00` pattern — confusing in a "fresh boot" UI. Zero
        // matches what AltirraBridgeServer does in practice when no
        // settings file exists.
        g_sim.SetMemoryClearMode(kATMemoryClearMode_Zero);
    });

    EMBED_STEP("g_sim.ColdReset",   g_sim.ColdReset());
    EMBED_STEP("g_sim.Resume",      g_sim.Resume());
}

std::string getExceptionMessage(intptr_t excPtr) {
    auto* e = reinterpret_cast<std::exception*>(excPtr);
    return e ? std::string(e->what()) : std::string("(null exception)");
}

#endif  // __EMSCRIPTEN__
