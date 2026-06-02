// AltirraEmbed — headless wasm Altirra core for embedding in a host page.
//
// Mirrors AltirraBridgeServer structurally but replaces the TCP bridge
// transport with an Embind facade (bindings.cpp). The host (e.g. madside)
// instantiates the AltirraCore class from JS, drives advanceFrame() at
// 60 Hz, and reads pixels/audio/state directly through Embind.
//
// This file provides the minimal entry point. With -sMODULARIZE=1 the
// emitted module is a library, so main() just returns 0; the actual
// API surface lives in bindings.cpp.

#include <stdafx.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "simulator.h"

// The Altirra core references a single global ATSimulator instance via
// the `g_sim` symbol (see main_bridge.cpp for the canonical reference).
// Settings persistence, device managers, debugger glue — they all reach
// into this global. The embed target keeps the same convention so the
// shared core sources link unchanged.
ATSimulator g_sim;

int main() {
    // Module is consumed via Embind class instantiation; nothing to do here.
    return 0;
}
