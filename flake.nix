{
  description = "AltirraSDL (madside-embed) build environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.flake-utils.url = "github:numtide/flake-utils";

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = import nixpkgs { inherit system; };
      in {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            # Native build
            cmake
            ninja
            pkg-config
            gcc
            sdl3
            libGL
            mesa
            xorg.libX11
            xorg.libXext
            xorg.libXrandr
            xorg.libXcursor
            xorg.libXi
            wayland
            wayland-protocols
            libxkbcommon
            libxcb
            xorg.xcbutil
            xorg.xcbutilwm
            xorg.xcbutilkeysyms
            xorg.xcbutilimage
            xorg.libpthreadstubs
            xorg.libXdmcp
            vulkan-loader
            vulkan-headers
            libdrm
            libpulseaudio
            alsa-lib
            dbus

            # Wasm build
            emscripten

            # Misc
            git
            zip
            unzip
            python3
          ];

          shellHook = ''
            export EM_CACHE="$PWD/.emcache"
            echo "altirra build shell"
            echo "  cmake: $(cmake --version | head -1)"
            echo "  g++:   $(g++ --version | head -1)"
            echo "  emcc:  $(emcc --version | head -1)"
            echo ""
            echo "Native:  ./build.sh"
            echo "Wasm:    follow HOSTING.md (emcmake cmake -B build-wasm ...)"
          '';
        };
      });
}
