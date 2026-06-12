// One-shot simulator boot path shared by every Embind entry point. The
// first AltirraCore construction triggers `EnsureInitialized()` — a
// stripped-down port of `AltirraBridgeServer/main_bridge.cpp`'s
// `InitSimulator()`. Subsequent constructs are no-ops; the simulator is a
// process-global singleton (`g_sim` in main_embed.cpp).

#pragma once

#ifdef __EMSCRIPTEN__

#include <vd2/VDDisplay/display.h>
#include <atomic>
#include <cstdint>
#include <string>

// Display geometry the JS host reads via AltirraCore::width()/height().
// Matches what AltirraBridgeServer reports for an XL+LLEXL+64K profile.
constexpr int kDisplayWidth  = 336;
constexpr int kDisplayHeight = 224;
constexpr int kSampleRate    = 63920;

// One-shot guard set by EnsureInitialized().
extern std::atomic<bool> g_initialized;

// Null video display sink the simulator writes into. Set by
// EnsureInitialized() and consumed by advanceFrame() via
// ATBridgeNullVideoDisplayConsumeFramePosted.
extern IVDVideoDisplay* g_pNullDisplay;

void EnsureInitialized();

// Embind helper — given an Embind exception pointer, return the C++
// exception message. Called from JS as `Module.getExceptionMessage(p)`.
std::string getExceptionMessage(intptr_t excPtr);

#endif  // __EMSCRIPTEN__
