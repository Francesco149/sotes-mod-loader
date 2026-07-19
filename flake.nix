{
  description = "SotES Mod Loader — a universal, stability-first native mod loader + Lua/native mod API for Fortune Summoners (and, via game profiles, other 32-bit Win32 games).";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    # Rust toolchain WITH extra targets — stock nixpkgs rustc can't add the
    # x86_64-pc-windows-gnu target the launcher's Windows .exe cross-build needs.
    rust-overlay.url = "github:oxalica/rust-overlay";
  };

  outputs = { self, nixpkgs, flake-utils, rust-overlay }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          overlays = [ rust-overlay.overlays.default ];
        };

        # 32-bit mingw cross-compiler — the game is a 32-bit Win32 PE, so the
        # proxy `version.dll` and every native mod target i686 to match the host
        # process bitness.  (Same toolchain as the OpenSummoners sibling; this repo
        # is intentionally self-contained so it clones alone.)  NOTE the launcher is
        # NOT here — it's a 64-bit external app (see rustToolchain below).
        mingw32 = pkgs.pkgsCross.mingw32.buildPackages;

        # Launcher (P8): Rust + egui, cross-built to a single 64-bit Windows .exe
        # from this Linux/WSL env.  rust-overlay supplies a toolchain carrying the
        # x86_64-pc-windows-gnu target std (stock nixpkgs rustc can't add targets);
        # mingwW64 gcc is that target's PE linker driver.  (Distinct from the i686
        # mingw above, which builds the injected 32-bit DLL.)
        rustToolchain = pkgs.rust-bin.stable.latest.default.override {
          targets = [ "x86_64-pc-windows-gnu" ];
        };
        mingwW64 = pkgs.pkgsCross.mingwW64.buildPackages;

        # rust's x86_64-pc-windows-gnu std links `-l:libpthread.a` (winpthreads), which in
        # nixpkgs lives in a SEPARATE mingw-w64 output not on the gcc default search path.
        # We add its lib dir to the target's rustflags in shellHook so the .exe links.
        mingwPthreads = pkgs.pkgsCross.mingwW64.windows.pthreads;

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

            # ── launcher toolchain (P8): Rust + egui ──────────────────
            # The package-manager launcher (launcher/) is Rust + egui — see
            # launcher/DESIGN.md "Tech decision".  `rustToolchain` does host builds
            # + tests of the plumbing crate (sml-core; mkShell provides a host `cc`)
            # AND carries the x86_64-pc-windows-gnu target for the GUI .exe, which
            # links through the mingw-w64 gcc driver below.  (It bundles
            # rustc/cargo/clippy/rustfmt, so no separate entries are needed.)
            rustToolchain
            mingwW64.gcc           # x86_64-w64-mingw32-gcc — PE linker for the launcher .exe

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

            # Launcher cross-link: cargo drives the x86_64-pc-windows-gnu link through
            # the mingw-w64 gcc (which knows the PE CRT + import libs), plus the winpthreads
            # lib dir the rust std needs (`-l:libpthread.a`).
            export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc
            export CARGO_TARGET_X86_64_PC_WINDOWS_GNU_RUSTFLAGS="-L native=${mingwPthreads}/lib"

            echo "sotes-mod-loader dev shell ready"
            echo "  mingw cc:     $(command -v i686-w64-mingw32-gcc)"
            echo "  imgui:        $IMGUI_DIR"
            echo "  luajit src:   $LUAJIT_SRC"
            echo "  luajit host:  $LUAJIT_HOST_CC (32-bit, for buildvm)"
            echo "  rust:         $(command -v cargo)"
            echo ""
            echo "Build:  nix develop --command make -C core        # -> build/version.dll"
            echo "Launcher core:  cargo test  --manifest-path launcher/Cargo.toml"
            echo "Launcher .exe:  cargo build --manifest-path launcher/Cargo.toml -p sml-gui --release --target x86_64-pc-windows-gnu"
          '';
        };
      });
}
