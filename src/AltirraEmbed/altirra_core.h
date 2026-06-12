// AltirraCore — host-facing handle for the headless wasm core. One instance
// per workbench session; methods map 1:1 to the JS-side `EmuBackend`
// interface (madside).
//
// Construction triggers EnsureInitialized() (a no-op after the first call).
// Method bodies are split across TUs by concern:
//   - bindings.cpp  — class lifetime + load/hardware/getters + audio +
//                     pixels + sendKey + EMSCRIPTEN_BINDINGS facade
//   - bp_step.cpp   — setBreakpoints / step / advanceFrame / frameRefresh

#pragma once

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>
#include <vd2/Kasumi/pixmaputils.h>
#include <cstdint>
#include <vector>

class AltirraCore {
public:
    AltirraCore();

    void reset();

    void setHardwareMode(unsigned mode);
    void setMemoryMode(unsigned mode);
    void setBasic(bool enabled);
    void setKernel(int firmwareId);

    bool loadXEX(emscripten::val data);
    bool loadATR(emscripten::val data);
    bool loadCAR(emscripten::val data);
    bool loadCAS(emscripten::val data);
    bool loadMedia(const char* filename, emscripten::val data);

    void setBreakpoints(emscripten::val addrs);
    int  advanceFrame(emscripten::val trapFnLegacy);
    void frameRefresh();
    int  step();

    int  getPC()              const;
    int  getA()               const;
    int  getX()               const;
    int  getY()               const;
    int  getS()               const;
    int  getP()               const;
    bool isAtInstrBoundary()  const;

    emscripten::val readMem(int addr, int len);

    void sendKey(int keyCode, int charCode, bool isDown, int modifiers);

    // Capture GTIA's last posted frame into mPixels (XRGB8888, tightly
    // packed at kDisplayWidth × kDisplayHeight). Returns true when a frame
    // was available.
    bool capturePixels();

    // Public Embind entry point. Usually re-captures from GTIA; after
    // frameRefresh() it returns the cached post-advance pixels once and
    // then resumes normal capture behaviour.
    emscripten::val pixels();

    int width()      const;
    int height()     const;
    int sampleRate() const;

    // Pull buffered audio samples (mono float32) and clear the ring. JS
    // feeds these to a Web Audio buffer source / worklet.
    emscripten::val getAudioSamples();

    emscripten::val saveState();
    void            loadState(emscripten::val s);

private:
    std::vector<uint32_t>      mPixels;
    std::vector<uint8_t>       mLoaded;
    std::vector<uint8_t>       mMemBuf;
    std::vector<float>         mAudioOut;
    std::vector<uint32_t>      mDebugBpIds;
    VDPixmapBuffer             mXrgbBuf;
    bool mPixelsCached = false;

    // 6502 PC walks through operand bytes mid-instruction (a 3-byte JMP at
    // $2017 shows PC=$2018, $2019, $201A before resetting back to $2017).
    // Reading a snapshot during the running sim would catch that
    // oscillation. Cache the last PC seen at an instruction boundary so the
    // host always reads the post-execute / pre-fetch address — the one
    // users actually care about.
    mutable uint16_t mLastStablePC = 0;
};

#endif  // __EMSCRIPTEN__
