// core/test/host_stub.c — a minimal "game" for P0 acceptance testing WITHOUT the
// real engine.  It imports version.dll (by calling a forwarded export), which forces
// Windows to load our proxy from the app dir -> DllMain -> loader_thread scans mods\
// -> runs each mod's init.lua -> mod.log writes oss_modloader.log.  Staged beside a
// copy of the real system version.dll (as realver.dll) + a mods\ folder, this exercises
// the entire P0 path end to end (everything except the engine the loader doesn't touch
// in P0).  Run it, then read oss_modloader.log.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    // Referencing a version.dll export forces the import -> our proxy loads.
    DWORD handle = 0;
    DWORD sz = GetFileVersionInfoSizeA("nonexistent.dummy", &handle);
    // Optional argv[1] = ms to live (default 1500).  The executor's window-search
    // fallback takes ~5s, so pass e.g. 7000 to let the fallback path complete.
    int ms = (argc > 1) ? atoi(argv[1]) : 1500;
    printf("host_stub: version.dll import resolved (GetFileVersionInfoSizeA=%lu); "
           "living %d ms for the loader thread...\n", sz, ms);
    Sleep((DWORD)ms);
    printf("host_stub: done\n");
    return 0;
}
