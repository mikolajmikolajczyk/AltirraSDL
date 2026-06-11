// AltirraEmbed — Embind API surface.
//
// Mirrors the madside `EmuBackend` interface. The host owns a single
// `AltirraCore` instance and drives it from JS.
//
// The first AltirraCore construction triggers `EnsureInitialized()` —
// a stripped-down port of `AltirraBridgeServer/main_bridge.cpp`'s
// `InitSimulator()`. Subsequent constructs are no-ops; the simulator
// is a process-global singleton (`g_sim` in main_embed.cpp).

#include <stdafx.h>

#ifndef __EMSCRIPTEN__
int altirra_embed_bindings_marker() { return 0; }
#else

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <emscripten/console.h>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <unordered_set>
#include <atomic>
#include <fstream>
#include <exception>

#include <vd2/system/VDString.h>
#include <vd2/system/registry.h>
#include <vd2/system/text.h>
#include <vd2/system/error.h>
#include <vd2/system/file.h>
#include <vd2/VDDisplay/display.h>
#include <vd2/Kasumi/pixmap.h>
#include <vd2/Kasumi/pixmaputils.h>
#include <vd2/Kasumi/pixmapops.h>

#include "simulator.h"
#include "gtia.h"
#include "cpu.h"
#include "devicemanager.h"
#include "mediamanager.h"
#include "settings.h"
#include "firmwaremanager.h"
#include "constants.h"
#include "debugger.h"
#include <at/atcore/serializable.h>
#include <at/ataudio/audiooutput.h>
#include <at/ataudio/pokey.h>

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
extern IVDVideoDisplay *ATBridgeCreateNullVideoDisplay();
extern bool ATBridgeNullVideoDisplayConsumeFramePosted(IVDVideoDisplay *display);
extern IATDebugger *ATGetDebugger();

namespace {

// Native AltirraBridgeServer rawscreen for an XL+LLEXL+64K profile reports
// width=336, height=224, stride=1344 (= w*4, XRGB8888). Hardcoding here
// matches that — once F5 wiring exposes GTIA's GetCanvasSize this becomes
// dynamic, but for M1 the canvas is fixed.
constexpr int kDisplayWidth  = 336;
constexpr int kDisplayHeight = 224;
constexpr int kSampleRate    = 63920;

// Audio tap: receives raw PCM samples from ATAudioOutput's mixer pipeline
// before native delivery. We buffer into a ring; the host pulls via
// getAudioSamples() each frame and feeds Web Audio.
class EmbedAudioTap : public IATAudioTap {
public:
    void WriteRawAudio(const float* left, const float* right,
                       uint32_t count, uint32_t /*timestamp*/) override {
        // Stereo when `right` is non-null, otherwise mono in `left`.
        // Web Audio mixes mono → 2-channel automatically, so we just
        // store the left channel (matches AltirraSDL's pushStereoAsMono
        // fast path the simulator uses by default).
        const size_t before = mBuf.size();
        mBuf.resize(before + count);
        std::memcpy(mBuf.data() + before, left, count * sizeof(float));
        (void)right;   // stereo support deferred until host plays in stereo
        // Cap ring buffer to ~0.5s @ 48kHz so a paused host doesn't
        // grow it unboundedly.
        constexpr size_t kMax = 24000;
        if (mBuf.size() > kMax) {
            mBuf.erase(mBuf.begin(), mBuf.begin() + (mBuf.size() - kMax));
        }
    }
    std::vector<float> mBuf;
};

std::atomic<bool> g_initialized{false};
IVDVideoDisplay*  g_pNullDisplay = nullptr;
EmbedAudioTap     g_audioTap;

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

    // Hook audio tap so we can pull samples to JS Web Audio. Must
    // come after g_sim.Init() since the audio output is constructed
    // there.
    EMBED_STEP("audio tap", {
        if (auto* out = g_sim.GetAudioOutput())
            out->SetAudioTap(&g_audioTap);
    });

    // Override whatever defaults settings.ini didn't set: force XL
    // hardware + AltirraOS LLE XL kernel + 64K. Without this the
    // simulator boots with kernel=0 → no reset vector → CPU stuck
    // at $FFFC reading $FFFF.
    EMBED_STEP("force XL+LLEXL+64K", {
        g_sim.SetHardwareMode(kATHardwareMode_800XL);
        g_sim.SetKernel(kATFirmwareId_Kernel_LLEXL);
        g_sim.SetBasic(0);
        g_sim.SetMemoryMode(kATMemoryMode_64K);
        // Default ATMemoryClearMode_DRAM3 paints RAM with the
        // `FF 00 FF 00` pattern — confusing in a "fresh boot" UI.
        // Zero matches what AltirraBridgeServer does in practice when
        // no settings file exists.
        g_sim.SetMemoryClearMode(kATMemoryClearMode_Zero);
    });

    EMBED_STEP("g_sim.ColdReset",   g_sim.ColdReset());
    EMBED_STEP("g_sim.Resume",      g_sim.Resume());
}

// Embind helper — given an Embind exception pointer, return the C++
// exception message. Called from JS as `Module.getExceptionMessage(p)`.
std::string getExceptionMessage(intptr_t excPtr) {
    auto* e = reinterpret_cast<std::exception*>(excPtr);
    return e ? std::string(e->what()) : std::string("(null exception)");
}

using namespace emscripten;

class AltirraCore {
public:
    AltirraCore() {
        EnsureInitialized();
        mPixels.assign(kDisplayWidth * kDisplayHeight, 0u);
    }

    void reset() {
        g_sim.ColdReset();
    }

    void setHardwareMode(unsigned mode) {
        g_sim.SetHardwareMode((ATHardwareMode)mode);
    }
    void setMemoryMode(unsigned mode) {
        g_sim.SetMemoryMode((ATMemoryMode)mode);
    }
    void setBasic(bool enabled) {
        g_sim.SetBasic(enabled);
    }
    void setKernel(int firmwareId) {
        g_sim.SetKernel((uint64)firmwareId);
    }

    bool loadXEX(val data) { return loadMedia("loaded.xex", data); }
    bool loadATR(val data) { return loadMedia("loaded.atr", data); }
    bool loadCAR(val data) { return loadMedia("loaded.car", data); }
    bool loadCAS(val data) { return loadMedia("loaded.cas", data); }

    bool loadMedia(const char* filename, val data) {
        const auto len = data["length"].as<unsigned>();
        mLoaded.assign(len, 0);
        val memView{ typed_memory_view(len, mLoaded.data()) };
        memView.call<void>("set", data);

        // Wrap bytes in a memory stream so Altirra never touches the
        // filesystem (no sidecar `.lst`/`.lab`/`.lbl`/`.elf` probing,
        // no MEMFS pollution between loads). Altirra's generic media
        // loader autodetects format from the filename hint + content
        // magic: .xex executable, .atr disk image, .car cartridge,
        // .cas cassette. mOriginalPath stays empty so the loader
        // treats this as ephemeral. Exceptions from sim.Load (MyError
        // etc.) propagate to JS — `AltirraBackend` decodes via
        // `getExceptionMessage`.
        VDMemoryStream stream(mLoaded.data(), (uint32)mLoaded.size());
        ATMediaLoadContext ctx;
        const VDStringW wname = VDTextU8ToW(VDStringSpanA(filename));
        ctx.mImageName = wname.c_str();
        ctx.mpStream   = &stream;
        ctx.mWriteMode = kATMediaWriteMode_RO;
        if (!g_sim.Load(ctx)) return false;
        g_sim.ColdReset();
        g_sim.Resume();
        return true;
    }

    void setBreakpoints(val addrs) {
        // Register BPs with Altirra's debugger so they're checked inside
        // the simulator's own Advance() loop (vs. our JS-side polling
        // which only sees post-frame state — Advance() runs full frames
        // at a time, so JS-side checks never observe transient PCs).
        IATDebugger* dbg = ATGetDebugger();
        if (!dbg) {
            emscripten_console_error("[altirra-embed] setBreakpoints: no debugger");
            return;
        }
        // Wipe prior BPs we registered.
        for (uint32_t id : mDebugBpIds) (void)dbg->ClearUserBreakpoint(id, false);
        mDebugBpIds.clear();
        const auto len = addrs["length"].as<unsigned>();
        for (unsigned i = 0; i < len; ++i) {
            ATDebuggerBreakpointInfo info;
            info.mTargetIndex = 0;
            info.mAddress     = (uint32_t)(addrs[i].as<int>() & 0xffff);
            info.mLength      = 1;
            info.mbBreakOnPC  = true;
            info.mpCondition  = nullptr;
            try {
                const uint32_t id = dbg->SetBreakpoint(-1, info);
                mDebugBpIds.push_back(id);
            } catch (const MyError& e) {
                emscripten_console_errorf("[altirra-embed] BP set failed @ %04x: %s",
                    info.mAddress, e.c_str());
            }
        }
    }

    int advanceFrame(val /*trapFnLegacy*/) {
        // Sim may be halted from a prior BP hit / step — resume it so
        // Advance() actually advances. (We do NOT Resume after a trap,
        // see comment at end of this function.)
        if (!g_sim.IsRunning()) g_sim.Resume();
        constexpr int kMaxIters = 4 * 35568;
        int iters = 0;
        while (iters < kMaxIters) {
            const auto r = g_sim.Advance(false);
            ++iters;
            // Debugger BP hits halt the simulator → Stopped. Do NOT
            // Resume here — leave the sim halted so the host's next
            // step() / advanceFrame() can decide what to do. (StepInto
            // bails out if the sim is already running, which would
            // turn every Step into a full-frame run.)
            if (r == ATSimulator::kAdvanceResult_Stopped) break;
            if (ATBridgeNullVideoDisplayConsumeFramePosted(g_pNullDisplay)) break;
        }
        return iters;
    }

    void frameRefresh() {
        // Snapshot → advance one frame → capture pixels → Apply.
        // Known-broken: Apply leaves the sim in `mbRunning=true` plus
        // inconsistent debugger linkage, so the next StepInto bails
        // out early and turns into a full-frame run. Disabled until we
        // find a way to round-trip debugger state through Apply (or
        // skip Apply entirely with a direct ANTIC scanline render). The
        // host currently calls advanceFrame() instead.
        IATDebugger* dbg = ATGetDebugger();
        std::vector<ATDebuggerBreakpointInfo> savedBps;
        if (dbg) {
            for (uint32_t id : mDebugBpIds) {
                ATDebuggerBreakpointInfo info;
                if (dbg->GetBreakpointInfo(id, info)) savedBps.push_back(info);
                dbg->ClearUserBreakpoint(id, false);
            }
            mDebugBpIds.clear();
            dbg->Break();
        }

        vdrefptr<IATSerializable> snap;
        vdrefptr<IATSerializable> snapInfo;
        try { g_sim.CreateSnapshot(~snap, ~snapInfo); }
        catch (...) {
            if (dbg) {
                for (auto& info : savedBps) {
                    try { mDebugBpIds.push_back(dbg->SetBreakpoint(-1, info)); }
                    catch (...) {}
                }
            }
            return;
        }

        if (!g_sim.IsRunning()) g_sim.Resume();
        constexpr int kMaxIters = 2 * 35568;
        for (int i = 0; i < kMaxIters; ++i) {
            g_sim.Advance(false);
            if (ATBridgeNullVideoDisplayConsumeFramePosted(g_pNullDisplay)) break;
        }
        capturePixels();
        mPixelsCached = true;

        if (snap) {
            try { g_sim.ApplySnapshot(*snap, nullptr); }
            catch (...) {}
        }
        if (g_sim.IsRunning()) g_sim.Pause();

        if (dbg) {
            for (auto& info : savedBps) {
                try { mDebugBpIds.push_back(dbg->SetBreakpoint(-1, info)); }
                catch (...) {}
            }
        }
    }

    int step() {
        // Altirra's single-instruction step via the debugger: arms
        // ATCPUStepCondition::CreateSingleStep() on the CPU and
        // Resume()s — the CPU halts itself at the next instruction
        // boundary, sim returns kAdvanceResult_Stopped from its next
        // Advance().
        IATDebugger* dbg = ATGetDebugger();
        if (!dbg) { g_sim.Resume(); return 0; }
        dbg->StepInto(kATDebugSrcMode_Disasm);

        int iters = 0;
        constexpr int kMaxIters = 2 * 35568;
        for (; iters < kMaxIters; ++iters) {
            const auto r = g_sim.Advance(false);
            if (r == ATSimulator::kAdvanceResult_Stopped) break;
        }
        return iters;
    }


    // 6502 PC walks through operand bytes mid-instruction (a 3-byte
    // JMP at $2017 shows PC=$2018, $2019, $201A before resetting back
    // to $2017). Reading a snapshot during the running sim would catch
    // that oscillation. Cache the last PC seen at an instruction
    // boundary so the host always reads the post-execute / pre-fetch
    // address — the one users actually care about.
    int  getPC()              const {
        auto& cpu = g_sim.GetCPU();
        if (!cpu.IsInstructionInProgress())
            mLastStablePC = cpu.GetPC() & 0xffff;
        return mLastStablePC;
    }
    int  getA()               const { return g_sim.GetCPU().GetA() & 0xff; }
    int  getX()               const { return g_sim.GetCPU().GetX() & 0xff; }
    int  getY()               const { return g_sim.GetCPU().GetY() & 0xff; }
    int  getS()               const { return g_sim.GetCPU().GetS() & 0xff; }
    int  getP()               const { return g_sim.GetCPU().GetP() & 0xff; }
    bool isAtInstrBoundary()  const { return g_sim.GetCPU().IsAtInsnStep(); }

    val readMem(int addr, int len) {
        // mMemBuf must be a member, not a stack local — typed_memory_view
        // aliases the underlying storage and is read by JS after this
        // function returns.
        mMemBuf.resize(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i) {
            mMemBuf[i] = g_sim.DebugReadByte((uint16)((addr + i) & 0xffff));
        }
        return val(typed_memory_view(mMemBuf.size(), mMemBuf.data()));
    }

    void sendKey(int keyCode, int charCode, bool isDown, int modifiers) {
        // JS → Atari KBCODE mapping. Atari KBCODE bits: 6=Ctrl, 7=Shift,
        // 0-5 = key. We supply the base key here; modifiers go through
        // POKEY's shift/control state APIs.
        auto& pokey = g_sim.GetPokey();

        // Handle modifier keys directly.
        const bool shift = (modifiers & 2) != 0;
        const bool ctrl  = (modifiers & 4) != 0;
        pokey.SetShiftKeyState(shift, true);
        pokey.SetControlKeyState(ctrl);

        if (!isDown) {
            // Key up: release whatever was held. We don't track per-key
            // raw state right now, so just clear all raw keys — fine for
            // simple typing.
            pokey.ReleaseAllRawKeys(true);
            return;
        }

        uint8_t kbcode = 0xff;

        // Letters via charCode (lower or upper case both fine — Atari KB
        // mirrors letter positions).
        const int cc = charCode;
        if (cc >= 'a' && cc <= 'z') {
            static const uint8_t letters[26] = {
                0x3F, 0x15, 0x12, 0x3A, 0x2A, 0x38, 0x3D, 0x39,   // A..H
                0x0D, 0x01, 0x05, 0x00, 0x25, 0x23, 0x08, 0x0A,   // I..P
                0x2F, 0x28, 0x3E, 0x2D, 0x0B, 0x10, 0x2E, 0x16,   // Q..X
                0x2B, 0x17,                                        // Y..Z
            };
            kbcode = letters[cc - 'a'];
        } else if (cc >= 'A' && cc <= 'Z') {
            static const uint8_t letters[26] = {
                0x3F, 0x15, 0x12, 0x3A, 0x2A, 0x38, 0x3D, 0x39,
                0x0D, 0x01, 0x05, 0x00, 0x25, 0x23, 0x08, 0x0A,
                0x2F, 0x28, 0x3E, 0x2D, 0x0B, 0x10, 0x2E, 0x16,
                0x2B, 0x17,
            };
            kbcode = letters[cc - 'A'];
        } else if (cc >= '0' && cc <= '9') {
            static const uint8_t digits[10] = {
                0x32, 0x1F, 0x1E, 0x1A, 0x18, 0x1D, 0x1B, 0x33, 0x35, 0x30
            };
            kbcode = digits[cc - '0'];
        } else {
            // Map common non-printable JS key codes (`event.keyCode`).
            switch (keyCode) {
                case 13:  kbcode = 0x0C; break;   // Enter/Return
                case 32:  kbcode = 0x21; break;   // Space
                case 27:  kbcode = 0x1C; break;   // Esc
                case 9:   kbcode = 0x2C; break;   // Tab
                case 8:   kbcode = 0x34; break;   // Backspace
                case 189: kbcode = 0x0E; break;   // - / _
                case 187: kbcode = 0x0F; break;   // = / +
                case 188: kbcode = 0x20; break;   // ,
                case 190: kbcode = 0x22; break;   // .
                case 191: kbcode = 0x26; break;   // /
                case 186: kbcode = 0x02; break;   // ;
                case 20:  kbcode = 0x3C; break;   // Caps Lock
                default:  return;                 // unmapped key
            }
        }
        // PushKey args (per bridge usage): code, repeat=false, allowQueue=true,
        // flushQueue=false, useCooldown=true. Lets multiple keys queue
        // naturally and respects the 9-frame keyboard scan delay.
        pokey.PushKey(kbcode, false, true, false, true);
    }

    // Capture GTIA's last posted frame into mPixels (XRGB8888,
    // tightly packed at kDisplayWidth × kDisplayHeight). Returns
    // true when a frame was available.
    bool capturePixels() {
        VDPixmapBuffer srcBuf;
        VDPixmap src;
        if (!g_sim.GetGTIA().GetLastFrameBuffer(srcBuf, src)) return false;
        if (mXrgbBuf.format == 0
            || mXrgbBuf.w != src.w
            || mXrgbBuf.h != src.h) {
            mXrgbBuf.init(src.w, src.h, nsVDPixmap::kPixFormat_XRGB8888);
        }
        VDPixmapBlt(mXrgbBuf, src);
        const int w = std::min<int>(mXrgbBuf.w, kDisplayWidth);
        const int h = std::min<int>(mXrgbBuf.h, kDisplayHeight);
        for (int y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(
                (const uint8_t*)mXrgbBuf.data + (ptrdiff_t)mXrgbBuf.pitch * y);
            uint32_t* dst = mPixels.data() + (size_t)y * kDisplayWidth;
            for (int x = 0; x < w; ++x) dst[x] = row[x];
        }
        return true;
    }

    // Public Embind entry point. Usually re-captures from GTIA; after
    // frameRefresh() it returns the cached post-advance pixels once and
    // then resumes normal capture behaviour.
    val pixels() {
        if (mPixelsCached) {
            mPixelsCached = false;
        } else {
            capturePixels();
        }
        return val(typed_memory_view(mPixels.size(), mPixels.data()));
    }

    int width()      const { return kDisplayWidth; }
    int height()     const { return kDisplayHeight; }
    int sampleRate() const { return kSampleRate; }

    // Pull buffered audio samples (mono float32) and clear the ring.
    // JS feeds these to a Web Audio buffer source / worklet.
    val getAudioSamples() {
        mAudioOut.assign(g_audioTap.mBuf.begin(), g_audioTap.mBuf.end());
        g_audioTap.mBuf.clear();
        return val(typed_memory_view(mAudioOut.size(), mAudioOut.data()));
    }

    val saveState()           { return val::null(); }
    void loadState(val /*s*/) {}

private:
    std::vector<uint32_t>      mPixels;
    std::vector<uint8_t>       mLoaded;
    std::vector<uint8_t>       mMemBuf;
    std::vector<float>         mAudioOut;
    std::vector<uint32_t>      mDebugBpIds;
    VDPixmapBuffer             mXrgbBuf;
    bool mPixelsCached = false;
    mutable uint16_t mLastStablePC = 0;
};

}  // namespace

EMSCRIPTEN_BINDINGS(altirra_core) {
    function("getExceptionMessage", &getExceptionMessage);
    class_<AltirraCore>("AltirraCore")
        .constructor<>()
        .function("reset",             &AltirraCore::reset)
        .function("loadXEX",           &AltirraCore::loadXEX)
        .function("loadATR",           &AltirraCore::loadATR)
        .function("loadCAR",           &AltirraCore::loadCAR)
        .function("loadCAS",           &AltirraCore::loadCAS)
        .function("setHardwareMode",   &AltirraCore::setHardwareMode)
        .function("setMemoryMode",     &AltirraCore::setMemoryMode)
        .function("setBasic",          &AltirraCore::setBasic)
        .function("setKernel",         &AltirraCore::setKernel)
        .function("advanceFrame",      &AltirraCore::advanceFrame)
        .function("setBreakpoints",    &AltirraCore::setBreakpoints)
        .function("step",              &AltirraCore::step)
        .function("frameRefresh",      &AltirraCore::frameRefresh)
        .function("getPC",             &AltirraCore::getPC)
        .function("getA",              &AltirraCore::getA)
        .function("getX",              &AltirraCore::getX)
        .function("getY",              &AltirraCore::getY)
        .function("getS",              &AltirraCore::getS)
        .function("getP",              &AltirraCore::getP)
        .function("isAtInstrBoundary", &AltirraCore::isAtInstrBoundary)
        .function("readMem",           &AltirraCore::readMem)
        .function("sendKey",           &AltirraCore::sendKey)
        .function("pixels",            &AltirraCore::pixels)
        .function("getAudioSamples",   &AltirraCore::getAudioSamples)
        .function("saveState",         &AltirraCore::saveState)
        .function("loadState",         &AltirraCore::loadState)
        .property("width",             &AltirraCore::width)
        .property("height",            &AltirraCore::height)
        .property("sampleRate",        &AltirraCore::sampleRate);
}

#endif  // __EMSCRIPTEN__
