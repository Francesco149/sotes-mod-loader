{
  description = "SotES Mod Loader — a universal, stability-first native mod loader + Lua/native mod API for Fortune Summoners (and, via game profiles, other 32-bit Win32 games).";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # 32-bit mingw cross-compiler — the game is a 32-bit Win32 PE, so the
        # proxy `version.dll`, every native mod, and the launcher all target i686
        # to match the host process bitness.  (Same toolchain as the OpenSummoners
        # sibling; this repo is intentionally self-contained so it clones alone.)
        mingw32 = pkgs.pkgsCross.mingw32.buildPackages;

        # Dear ImGui sources.  We compile the .cpp directly into the mingw32 PE
        # (DX11 backend) — ImGui is source-vendored via IMGUI_DIR, not a lib.
        imguiSrc = pkgs.imgui.src;

        # LuaJIT source, cross-built to a 32-bit Windows static lib by core/Makefile.
        # Building a 32-bit *target* requires a 32-bit *host* toolchain (LuaJIT's
        # buildvm/minilua run on the host during the build and must match target
        # pointer size) — hence pkgsi686Linux gcc as HOST_CC.  JIT is disabled at
        # compile time (interpreter + FFI only): most stable inside an injected DLL
        # — no JIT-compiled code pages in the game's address space.  FFI stays ON.
        luajitSrc  = pkgs.luajit.src;
        hostCC32   = "${pkgs.pkgsi686Linux.buildPackages.gcc}/bin/gcc";

        pythonEnv = pkgs.python3.withPackages (ps: with ps; [
          # launcher / registry tooling: fetch git-repo registries, verify hashes,
          # drive install/update.  requests for HTTP, pyyaml/tomli for manifests.
          requests
          pyyaml
          tomli
          rich
          pytest
        ]);

      in {
        devShells.default = pkgs.mkShell {
          name = "sotes-mod-loader-dev";

          packages = with pkgs; [
            # ── build toolchain (32-bit Win32 target) ─────────────────
            mingw32.gcc            # i686-w64-mingw32-gcc / -g++ — Win32 PE
            mingw32.binutils
            gnumake
            pkg-config

            # ── launcher toolchain (P8): Rust ─────────────────────────
            # The package-manager launcher (launcher/) is Rust + egui — see
            # launcher/DESIGN.md "Tech decision".  Host builds + tests of the
            # plumbing crate (sml-core) run here; mkShell provides a host `cc`
            # for linking.  NOTE the launcher is a normal 64-bit desktop app,
            # NOT i686 — it's an external process manager (spawns the game,
            # never injects), so it needn't match the game's 32-bit-ness.  The
            # Windows cross-target (x86_64-pc-windows-gnu) for the eframe GUI
            # is wired in a follow-up (rust-overlay + x86_64 mingw); the
            # plumbing crate needs neither, so `cargo test` is green with these.
            rustc
            cargo
            clippy
            rustfmt

            # ── scripting / tooling ───────────────────────────────────
            pythonEnv
            git

            # ── convenience ───────────────────────────────────────────
            jq
            ripgrep
            fd
            file
            binutils               # nm / objdump / strings for the built DLLs
          ];

          shellHook = ''
            export SOTES_MOD_LOADER_ROOT=$PWD

            # ImGui sources (compiled directly into the DX11 UI; osr_view pattern).
            export IMGUI_DIR="${imguiSrc}"

            # LuaJIT: source tree + the 32-bit host compiler for its buildvm.
            # core/Makefile copies $LUAJIT_SRC into build/luajit (writable),
            # cross-builds libluajit.a for the i686 Windows target, and links it.
            export LUAJIT_SRC="${luajitSrc}"
            export LUAJIT_HOST_CC="${hostCC32}"

            # mingw aliases (match OpenSummoners' habit).
            export MINGW_CC=i686-w64-mingw32-gcc
            export MINGW_CXX=i686-w64-mingw32-g++
            export MINGW_AR=i686-w64-mingw32-ar

            echo "sotes-mod-loader dev shell ready"
            echo "  mingw cc:     $(command -v i686-w64-mingw32-gcc)"
            echo "  imgui:        $IMGUI_DIR"
            echo "  luajit src:   $LUAJIT_SRC"
            echo "  luajit host:  $LUAJIT_HOST_CC (32-bit, for buildvm)"
            echo "  rust:         $(command -v cargo)"
            echo ""
            echo "Build:  nix develop --command make -C core        # -> build/version.dll"
            echo "Launcher core:  cargo test --manifest-path launcher/Cargo.toml --workspace"
          '';
        };
      });
}
