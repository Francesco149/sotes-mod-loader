# examples/early_probe/Makefile — build early_probe.dll (a native OssModEarlyInit mod).
#
#   nix develop --command make -C examples/early_probe
#
# -static-libgcc so the DLL has no libgcc_s dependency in the game's search path (like the loader).
# -I../../core finds oss_mod_api.h; an out-of-tree mod copies that header beside its source instead.
# OssModEarlyInit is __cdecl, so mingw exports it undecorated (GetProcAddress finds it by name).

CC := i686-w64-mingw32-gcc

all: early_probe.dll

early_probe.dll: early_probe.c ../../core/oss_mod_api.h
	$(CC) -O2 -Wall -Wextra -shared -static -static-libgcc -I../../core -o $@ early_probe.c -lkernel32
	@echo "  built $@  (drop it in the game's mods\\ folder)"

clean:
	rm -f early_probe.dll
