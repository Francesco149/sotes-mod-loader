# core/Makefile — the SotES Mod Loader host (proxy version.dll) + its LuaJIT runtime.
#
# Two stages:
#   1. LuaJIT: copy $(LUAJIT_SRC) (read-only nix store) into ../build/luajit, then
#      cross-build libluajit.a for the i686 Windows target.  A 32-bit *target* needs
#      a 32-bit *host* toolchain for LuaJIT's buildvm/minilua ($(LUAJIT_HOST_CC), the
#      flake's pkgsi686Linux gcc).  JIT compiled OUT (LUAJIT_DISABLE_JIT); FFI stays on.
#   2. The host C (loader.c + lua_host.c) -> version.dll, linking libluajit.a and the
#      forwards in version.def.  Single-language C for P0 (the ImGui/DX11 UI arrives in
#      P5 and will pull in g++/libstdc++ then).
#
#   nix develop --command make -C core        # -> ../build/version.dll
#
# -static -static-libgcc: the game's DLL search path has no libgcc_s, so a default
# mingw build (which imports it) would fail to LoadLibrary — link it in.

CC   := i686-w64-mingw32-gcc
OUT  ?= ../build/version.dll
OBJ  := obj

# ── LuaJIT (cross-built static lib) ──────────────────────────────────────────
LJ_SRC ?= $(LUAJIT_SRC)
LJ_DIR := ../build/luajit
LJ_LIB := $(LJ_DIR)/src/libluajit.a
LJ_INC := $(LJ_DIR)/src
LJ_HOSTCC ?= $(LUAJIT_HOST_CC)

# ── host sources ─────────────────────────────────────────────────────────────
CSRC   := loader.c lua_host.c mem.c game_bindings.c sotes_bindings.c
COBJ   := $(addprefix $(OBJ)/, $(CSRC:.c=.o))

CFLAGS  := -O2 -Wall -Wextra -I$(LJ_INC)
LDFLAGS := -shared -static -static-libgcc -s
LIBS    := $(LJ_LIB) -lkernel32 -luser32

.PHONY: all clean clean-luajit
all: $(OUT)

$(OUT): $(COBJ) version.def
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(COBJ) version.def $(LIBS)
	@echo "  built $@"

# our objects need the LuaJIT headers -> gate compilation on the lib being built.
$(OBJ)/%.o: %.c loader_internal.h lua_host.h $(LJ_LIB)
	@mkdir -p $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# Copy the read-only source once, make it writable, then cross-build just the static
# lib (not the luajit.exe, which would need a Windows-target link we don't want here).
$(LJ_LIB):
	@mkdir -p $(dir $(LJ_DIR))
	@if [ ! -e $(LJ_DIR)/Makefile ]; then \
	    echo "  staging LuaJIT source -> $(LJ_DIR)"; \
	    rm -rf $(LJ_DIR); cp -r $(LJ_SRC) $(LJ_DIR); chmod -R u+w $(LJ_DIR); \
	fi
	@echo "  cross-building LuaJIT (i686 Windows, JIT off, FFI on)"
	$(MAKE) -C $(LJ_DIR)/src libluajit.a \
	    HOST_CC="$(LJ_HOSTCC)" \
	    CROSS=i686-w64-mingw32- \
	    TARGET_SYS=Windows \
	    BUILDMODE=static \
	    XCFLAGS="-DLUAJIT_DISABLE_JIT"

clean:
	rm -rf $(OBJ) $(OUT)

clean-luajit:
	rm -rf $(LJ_DIR)
