# examples/native_hello/Makefile — build native_hello.dll (a native OssModInit mod).
#
#   nix develop --command make -C examples/native_hello
#
# -static-libgcc so the DLL has no libgcc_s dependency in the game's search path (like the
# loader).  -I../../core finds oss_mod_api.h; an out-of-tree mod copies that header beside its
# source instead.  OssModInit is __cdecl, so mingw exports it undecorated (GetProcAddress finds it).

CC := i686-w64-mingw32-gcc

all: native_hello.dll

native_hello.dll: native_hello.c ../../core/oss_mod_api.h
	$(CC) -O2 -Wall -Wextra -shared -static -static-libgcc -I../../core -o $@ native_hello.c -lkernel32
	@echo "  built $@  (drop it in the game's mods\\ folder)"

clean:
	rm -f native_hello.dll
