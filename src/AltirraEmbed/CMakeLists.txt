# AltirraEmbed — headless wasm Altirra core with Embind facade.
#
# Sibling to AltirraBridgeServer: same emulation core, same stubs, same
# strip-the-UI strategy. Replaces the TCP bridge transport with Embind
# bindings (see bindings.cpp) so a JS host (madside IDE) can drive the
# core directly via class_<AltirraCore>.
#
# Opt-in via -DALTIRRA_EMBED=ON. Works alongside ALTIRRA_WASM (no force-OFF
# guard, unlike ALTIRRA_BRIDGE_SERVER, which assumes a socket transport
# that wasm browsers can't provide).
#
# Output (wasm):
#   build/<preset>/src/AltirraEmbed/altirra-core.{wasm,js}
# Target size: < 3 MB (compare to full AltirraSDL wasm ≈ 12 MB).

set(ALTIRRA_SDL_DIR ${CMAKE_SOURCE_DIR}/src/AltirraSDL)
set(ALTIRRA_BRIDGE_SERVER_DIR ${CMAKE_SOURCE_DIR}/src/AltirraBridgeServer)

add_executable(AltirraEmbed
    main_embed.cpp
    # Embind API surface (TU split — see cd90f9d):
    bindings.cpp        # AltirraCore runtime methods + EMSCRIPTEN_BINDINGS
    embed_init.cpp      # EnsureInitialized + EMBED_STEP + globals
    audio_tap.cpp       # EmbedAudioTap
    bp_step.cpp         # setBreakpoints / step / advanceFrame / frameRefresh

    # Reuse the bridge server's UI-no-op stubs verbatim — they provide
    # ATBridgeNullVideoDisplay (used as GTIA's video sink) plus no-op
    # IATUIRenderer + a handful of frontend symbols the linker would
    # otherwise complain about.
    ${ALTIRRA_BRIDGE_SERVER_DIR}/ui_stubs.cpp

    # --- All emulation core files (filtered, shared with AltirraSDL) ---
    ${ALTIRRA_ALL_SOURCES}

    # --- Stub implementations replacing Win32-only files ---
    ${ALTIRRA_SDL_DIR}/stubs/console_stubs.cpp
    ${ALTIRRA_SDL_DIR}/stubs/oshelper_stubs.cpp
    ${ALTIRRA_SDL_DIR}/stubs/uiaccessors_stubs.cpp
    ${ALTIRRA_SDL_DIR}/stubs/win32_stubs.cpp
    ${ALTIRRA_SDL_DIR}/stubs/device_stubs.cpp

    # Portable polling-based directory watcher and MidiMate backend
    # (no-op on wasm, but the symbols are still referenced by core code).
    ${ALTIRRA_SDL_DIR}/source/os/directorywatcher_sdl3.cpp
    ${ALTIRRA_SDL_DIR}/source/os/midimate_sdl3.cpp

    # Keymap port — provides ATUIInitVirtualKeyMap etc., referenced by
    # settings.cpp.
    ${ALTIRRA_SDL_DIR}/source/input/keyboard_keymap_sdl3.cpp

    # Altirra platform glue.
    ${CMAKE_SOURCE_DIR}/src/Altirra/source/uiregistry.cpp
    ${CMAKE_SOURCE_DIR}/src/Altirra/source/savestateio.cpp
    ${CMAKE_SOURCE_DIR}/src/ATUI/source/uicommandmanager.cpp
    ${CMAKE_SOURCE_DIR}/src/Dita/source/accel.cpp

    # VDDisplay utility sources still referenced by some core paths.
    ${CMAKE_SOURCE_DIR}/src/VDDisplay/source/screenfx.cpp
    ${CMAKE_SOURCE_DIR}/src/VDDisplay/source/bloom.cpp
    ${CMAKE_SOURCE_DIR}/src/VDDisplay/source/bicubic.cpp
    ${CMAKE_SOURCE_DIR}/src/VDDisplay/source/displaytypes.cpp

    # Audio mixer compiled with ALTIRRA_AUDIO_NULL — no SDL_AudioStream,
    # samples are produced and discarded. (Real audio output is handled
    # via Embind callbacks; F5 will wire this up.)
    ${CMAKE_SOURCE_DIR}/src/ATAudio/source/audiooutput_sdl3.cpp
)

# POSIX-only device source replacements. wasm32-emscripten reports as
# UNIX so these get pulled in — they're network/modem device backends
# that won't actually be wired up in the embed target (no JS hooks yet),
# but the symbols are needed for linking the core.
if(NOT WIN32)
    target_sources(AltirraEmbed PRIVATE
        ${ALTIRRA_SDL_DIR}/source/os/modemtcp_sdl3.cpp
        ${ALTIRRA_SDL_DIR}/source/os/idephysdisk_sdl3.cpp
        ${ALTIRRA_SDL_DIR}/source/os/pipeserial_sdl3.cpp
    )
endif()

target_compile_definitions(AltirraEmbed PRIVATE
    ALTIRRA_EMBED_ENABLED=1
    ALTIRRA_AUDIO_NULL=1
    AT_SDL3_PORTABLE=1
)

target_include_directories(AltirraEmbed PRIVATE
    ${ALTIRRA_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/Altirra/h
    ${CMAKE_SOURCE_DIR}/src/Altirra/source
    ${ALTIRRA_SDL_DIR}/source
    ${ALTIRRA_SDL_DIR}/source/app
    ${ALTIRRA_SDL_DIR}/source/ui/core
    ${CMAKE_SOURCE_DIR}/src/ATAudio/h
    ${CMAKE_SOURCE_DIR}/src/VDDisplay/h
    ${CMAKE_SOURCE_DIR}/src/Altirra/res
    ${CMAKE_SOURCE_DIR}/src/Altirra/autogen
    ${CMAKE_SOURCE_DIR}/src/Altirra/autobuild_default
)

if(ALTIRRA_COMPAT_DIR)
    target_include_directories(AltirraEmbed PRIVATE ${ALTIRRA_COMPAT_DIR})
endif()

# Stub headers (imgui.h shim etc.) come BEFORE the real headers, same as
# AltirraBridgeServer.
target_include_directories(AltirraEmbed BEFORE PRIVATE
    ${ALTIRRA_BRIDGE_SERVER_DIR}/stubs
    ${ALTIRRA_SDL_DIR}/stubs
    ${CMAKE_SOURCE_DIR}/src/Altirra/h
)

target_link_libraries(AltirraEmbed PRIVATE
    ATCPU
    ATEmulation
    ATDevices
    ATIO
    ATAudio
    ATNetwork
    ATNetworkSockets
    ATDebugger
    ATVM
    ATCore
    Kasumi
    vdjson
    system
)

# Same per-platform OS classification as AltirraBridgeServer.
if(WIN32)
    target_compile_definitions(AltirraEmbed PRIVATE VD_OS_WINDOWS=1)
elseif(APPLE)
    target_compile_definitions(AltirraEmbed PRIVATE VD_OS_MACOS=1)
elseif(EMSCRIPTEN)
    # wasm32-emscripten masquerades as UNIX. Tag it explicitly so
    # platform-specific code paths in the core can branch off.
    target_compile_definitions(AltirraEmbed PRIVATE
        VD_OS_LINUX=1
        ATMIDI_BACKEND_NULL=1
    )
elseif(UNIX)
    target_compile_definitions(AltirraEmbed PRIVATE VD_OS_LINUX=1)
    if(NOT ANDROID)
        find_package(ALSA)
        if(ALSA_FOUND)
            target_link_libraries(AltirraEmbed PRIVATE ALSA::ALSA)
        else()
            target_compile_definitions(AltirraEmbed PRIVATE ATMIDI_BACKEND_NULL=1)
        endif()
    endif()
endif()

# Per-file SIMD flags — mirror AltirraBridgeServer.
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/src/Altirra/source/antic.cpp"
        PROPERTIES COMPILE_FLAGS "-mbmi"
    )
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/src/Altirra/source/tracefileencoding.cpp"
        PROPERTIES COMPILE_FLAGS "-mssse3 -mpopcnt"
    )
endif()

# Emscripten output: ES6 module named altirra-core.js + altirra-core.wasm.
# Embind needs `-lembind`. We allow memory growth and 64-bit BigInt
# because the simulator state allocations exceed wasm default linear
# memory pretty quickly.
if(EMSCRIPTEN)
    target_link_options(AltirraEmbed PRIVATE
        -lembind
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createAltirraCore
        -sENVIRONMENT=web,worker
        -sALLOW_MEMORY_GROWTH=1
        -sWASM_BIGINT=1
        -sINITIAL_MEMORY=64MB
        -sMAXIMUM_MEMORY=512MB
        -sDISABLE_EXCEPTION_CATCHING=0
    )
    set_target_properties(AltirraEmbed PROPERTIES
        OUTPUT_NAME "altirra-core"
        SUFFIX ".js"
    )
endif()
