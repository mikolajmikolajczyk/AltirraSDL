// AltirraEmbed — Embind API surface.
//
// Mirrors the madside `EmuBackend` interface. The host owns a single
// `AltirraCore` instance and drives it from JS. Class declaration lives in
// altirra_core.h; debug/step plumbing lives in bp_step.cpp; one-shot boot
// machinery lives in embed_init.{h,cpp}; the audio tap lives in
// audio_tap.{h,cpp}. This TU keeps the runtime methods (media load,
// hardware setters, register getters, memory read, key dispatch, pixels +
// audio pull) plus the EMSCRIPTEN_BINDINGS facade.

#include <stdafx.h>

#ifndef __EMSCRIPTEN__
int altirra_embed_bindings_marker() { return 0; }
#else

#include "altirra_core.h"
#include "audio_tap.h"
#include "embed_init.h"

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <cstdint>
#include <vector>

#include <vd2/system/VDString.h>
#include <vd2/system/text.h>
#include <vd2/system/file.h>
#include <vd2/VDDisplay/display.h>
#include <vd2/Kasumi/pixmap.h>
#include <vd2/Kasumi/pixmaputils.h>
#include <vd2/Kasumi/pixmapops.h>

#include "simulator.h"
#include "gtia.h"
#include "cpu.h"
#include "mediamanager.h"
#include <at/ataudio/pokey.h>

extern ATSimulator g_sim;

using emscripten::val;
using emscripten::typed_memory_view;

// --- Lifecycle ---

AltirraCore::AltirraCore() {
    EnsureInitialized();
    mPixels.assign(kDisplayWidth * kDisplayHeight, 0u);
}

void AltirraCore::reset()                              { g_sim.ColdReset(); }
void AltirraCore::setHardwareMode(unsigned mode)       { g_sim.SetHardwareMode((ATHardwareMode)mode); }
void AltirraCore::setMemoryMode(unsigned mode)         { g_sim.SetMemoryMode((ATMemoryMode)mode); }
void AltirraCore::setBasic(bool enabled)               { g_sim.SetBasic(enabled); }
void AltirraCore::setKernel(int firmwareId)            { g_sim.SetKernel((uint64)firmwareId); }

int  AltirraCore::width()      const                   { return kDisplayWidth; }
int  AltirraCore::height()     const                   { return kDisplayHeight; }
int  AltirraCore::sampleRate() const                   { return kSampleRate; }

// --- Media load ---

bool AltirraCore::loadXEX(val data) { return loadMedia("loaded.xex", data); }
bool AltirraCore::loadATR(val data) { return loadMedia("loaded.atr", data); }
bool AltirraCore::loadCAR(val data) { return loadMedia("loaded.car", data); }
bool AltirraCore::loadCAS(val data) { return loadMedia("loaded.cas", data); }

bool AltirraCore::loadMedia(const char* filename, val data) {
    const auto len = data["length"].as<unsigned>();
    mLoaded.assign(len, 0);
    val memView{ typed_memory_view(len, mLoaded.data()) };
    memView.call<void>("set", data);

    // Wrap bytes in a memory stream so Altirra never touches the
    // filesystem (no sidecar `.lst`/`.lab`/`.lbl`/`.elf` probing, no
    // MEMFS pollution between loads). Altirra's generic media loader
    // autodetects format from the filename hint + content magic: .xex
    // executable, .atr disk image, .car cartridge, .cas cassette.
    // mOriginalPath stays empty so the loader treats this as ephemeral.
    // Exceptions from sim.Load (MyError etc.) propagate to JS —
    // `AltirraBackend` decodes via `getExceptionMessage`.
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

// --- Register getters ---

int AltirraCore::getPC() const {
    auto& cpu = g_sim.GetCPU();
    if (!cpu.IsInstructionInProgress())
        mLastStablePC = cpu.GetPC() & 0xffff;
    return mLastStablePC;
}
int  AltirraCore::getA()              const { return g_sim.GetCPU().GetA() & 0xff; }
int  AltirraCore::getX()              const { return g_sim.GetCPU().GetX() & 0xff; }
int  AltirraCore::getY()              const { return g_sim.GetCPU().GetY() & 0xff; }
int  AltirraCore::getS()              const { return g_sim.GetCPU().GetS() & 0xff; }
int  AltirraCore::getP()              const { return g_sim.GetCPU().GetP() & 0xff; }
bool AltirraCore::isAtInstrBoundary() const { return g_sim.GetCPU().IsAtInsnStep(); }

val AltirraCore::readMem(int addr, int len) {
    // mMemBuf must be a member, not a stack local — typed_memory_view
    // aliases the underlying storage and is read by JS after this
    // function returns.
    mMemBuf.resize(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) {
        mMemBuf[i] = g_sim.DebugReadByte((uint16)((addr + i) & 0xffff));
    }
    return val(typed_memory_view(mMemBuf.size(), mMemBuf.data()));
}

// --- Keyboard ---

void AltirraCore::sendKey(int keyCode, int charCode, bool isDown, int modifiers) {
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
        // Key up: release whatever was held. We don't track per-key raw
        // state right now, so just clear all raw keys — fine for simple
        // typing.
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

// --- Pixels + audio ---

bool AltirraCore::capturePixels() {
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

val AltirraCore::pixels() {
    if (mPixelsCached) {
        mPixelsCached = false;
    } else {
        capturePixels();
    }
    return val(typed_memory_view(mPixels.size(), mPixels.data()));
}

val AltirraCore::getAudioSamples() {
    mAudioOut.assign(g_audioTap.mBuf.begin(), g_audioTap.mBuf.end());
    g_audioTap.mBuf.clear();
    return val(typed_memory_view(mAudioOut.size(), mAudioOut.data()));
}

// --- Save / load state (stubs until snapshot serializer works headless) ---

val  AltirraCore::saveState()         { return val::null(); }
void AltirraCore::loadState(val /*s*/) {}

// --- Embind facade ---

using namespace emscripten;

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
