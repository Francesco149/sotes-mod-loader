// core/test/host_stub.c — a minimal "game" for P0 acceptance testing WITHOUT the
// real engine.  It imports version.dll (by calling a forwarded export), which forces
// Windows to load our proxy from the app dir -> DllMain -> loader_thread scans mods\
// -> runs each mod's init.lua -> mod.log writes oss_modloader.log.  Staged beside a
// copy of the real system version.dll (as realver.dll) + a mods\ folder, this exercises
// the entire P0 path end to end (everything except the engine the loader doesn't touch
// in P0).  Run it, then read oss_modloader.log.
#include <windows.h>
#include <stdio.h>

int main(void) {
    // Referencing a version.dll export forces the import -> our proxy loads.
    DWORD handle = 0;
    DWORD sz = GetFileVersionInfoSizeA("nonexistent.dummy", &handle);
    printf("host_stub: version.dll import resolved (GetFileVersionInfoSizeA=%lu); "
           "waiting for the loader thread...\n", sz);
    Sleep(1500);   // let loader_thread scan mods\ + run init.lua
    printf("host_stub: done\n");
    return 0;
}
