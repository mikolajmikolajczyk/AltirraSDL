# Altirra Embed Target — Implementation Notes

Notes for `madside-embed` branch — headless wasm Altirra core with Embind facade.

## F3 — Core Surface Map

### Sources
- **`ALTIRRA_ALL_SOURCES`** glob in `src/AltirraSDL/cmake/altirra_core_sources.cmake` already filters out Win32/UI sources. Reuse as-is.
- **AltirraBridgeServer** target is the existing headless reference (`-DALTIRRA_BRIDGE_SERVER=ON`). Forced OFF for wasm because of TCP transport.

### New target: `AltirraEmbed`
Sibling to `AltirraBridgeServer`. Same emulation core, different frontend:

- **Replace** `main_bridge.cpp` + `bridge_*.cpp` with `main_embed.cpp` + `bindings.cpp` (Embind).
- **Reuse** `ui_stubs.cpp` (null video display, no-op UI renderer) — exact same.
- **Reuse stubs:** `console_stubs.cpp`, `oshelper_stubs.cpp`, `uiaccessors_stubs.cpp`, `win32_stubs.cpp`, `device_stubs.cpp`.
- **Reuse SDL3-portable glue:** `directorywatcher_sdl3.cpp`, `midimate_sdl3.cpp`, `keyboard_keymap_sdl3.cpp`.
- **Reuse Altirra glue:** `Altirra/source/uiregistry.cpp`, `Altirra/source/savestateio.cpp`, `ATUI/source/uicommandmanager.cpp`.

### New CMake option
- `ALTIRRA_EMBED=ON` — opts into the new target.
- Works with `ALTIRRA_WASM=ON` (no force-OFF guard).
- Should also work for native builds (smoke testing on Linux without browser).

## F4 — CMake Target Skeleton

```cmake
# src/CMakeLists.txt
option(ALTIRRA_EMBED "Build the headless AltirraEmbed wasm/native target" OFF)
if(ALTIRRA_EMBED AND ALTIRRA_SDL3 AND NOT ANDROID)
    add_subdirectory(AltirraEmbed)
endif()
```

```cmake
# src/AltirraEmbed/CMakeLists.txt — clone AltirraBridgeServer/CMakeLists.txt
add_executable(AltirraEmbed
    main_embed.cpp
    bindings.cpp
    ui_stubs.cpp                  # copy from AltirraBridgeServer
    ${ALTIRRA_ALL_SOURCES}
    # ... same stubs as BridgeServer
)

target_link_libraries(AltirraEmbed PRIVATE
    ATCPU ATCore ATDebugger ATDevices ATEmulation
    ATIO ATAudio ATBasic ATCompiler ATVM
    system vdjson Kasumi
)

if(ALTIRRA_WASM)
    target_link_options(AltirraEmbed PRIVATE
        -lembind
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sENVIRONMENT=web,worker
        -sALLOW_MEMORY_GROWTH=1
        -sWASM_BIGINT=1
        -sEXPORT_NAME=createAltirraCore
    )
    set_target_properties(AltirraEmbed PROPERTIES
        SUFFIX ".js"
        OUTPUT_NAME "altirra-core"
    )
endif()
```

## F5 — Embind API Surface

Mirror `EmuBackend` interface:

```cpp
// bindings.cpp
EMSCRIPTEN_BINDINGS(altirra_core) {
    class_<AltirraCore>("AltirraCore")
        .constructor<>()
        .function("reset", &AltirraCore::Reset)
        .function("loadXEX", &AltirraCore::LoadXEX)
        .function("advanceFrame", &AltirraCore::AdvanceFrame)
        .function("step", &AltirraCore::Step)
        .function("getPC", &AltirraCore::GetPC)
        .function("isAtInstrBoundary", &AltirraCore::IsAtInstrBoundary)
        .function("readMem", &AltirraCore::ReadMem)
        .function("sendKey", &AltirraCore::SendKey)
        .property("pixels", &AltirraCore::GetPixels)
        .property("width", &AltirraCore::width)
        .property("height", &AltirraCore::height)
        .property("sampleRate", &AltirraCore::sampleRate)
        .function("saveState", &AltirraCore::SaveState)
        .function("loadState", &AltirraCore::LoadState);
}
```

## F6 — Build Command

```sh
emcmake cmake -B build/wasm-embed \
  -DCMAKE_BUILD_TYPE=Release \
  -DALTIRRA_SDL3=ON -DALTIRRA_WASM=ON -DALTIRRA_EMBED=ON \
  -DALTIRRA_BRIDGE=OFF -DALTIRRA_BRIDGE_SERVER=OFF
cmake --build build/wasm-embed -j
# Output: build/wasm-embed/src/AltirraEmbed/altirra-core.{wasm,js}
```

Target size: < 3 MB (compare to full AltirraSDL wasm = 12 MB with ImGui).
