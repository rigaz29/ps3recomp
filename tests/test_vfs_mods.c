/* PS3_MODS_ROOT (runtime/syscalls/vfs_mods.h): which guest paths the mod folder answers. */
#include "vfs_mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mk(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mk(p) mkdir(p, 0755)
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

int main(void)
{
    const char* dirs[] = { "vfs_mods_root", "vfs_mods_root/dev_hdd0", "vfs_mods_root/dev_hdd0/game",
                           "vfs_mods_root/dev_hdd0/game/T", "vfs_mods_root/dev_hdd0/game/T/USRDIR",
                           "vfs_mods_root/dev_hdd0/game/T/USRDIR/SUB" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) mk(dirs[i]);
    put("vfs_mods_root/dev_hdd0/game/T/USRDIR/PATCHES", "6");
#ifdef _WIN32
    _putenv_s("PS3_MODS_ROOT", "vfs_mods_root");
#else
    setenv("PS3_MODS_ROOT", "vfs_mods_root", 1);
#endif

    char out[256];
    /* A file in the folder answers its guest path. */
    CHECK(ps3_vfs_mod_path("/dev_hdd0/game/T/USRDIR/PATCHES", out, sizeof out) == 1);
    CHECK(strcmp(out, "vfs_mods_root/dev_hdd0/game/T/USRDIR/PATCHES") == 0);
    /* Nothing there: translated as usual. */
    CHECK(ps3_vfs_mod_path("/dev_hdd0/game/T/USRDIR/ARCHIVES", out, sizeof out) == 0);
    /* A folder is not a file: listings stay the guest's. */
    CHECK(ps3_vfs_mod_path("/dev_hdd0/game/T/USRDIR/SUB", out, sizeof out) == 0);
    /* No way out of the folder, and no relative guest paths. */
    CHECK(ps3_vfs_mod_path("/dev_hdd0/game/T/USRDIR/../USRDIR/PATCHES", out, sizeof out) == 0);
    CHECK(ps3_vfs_mod_path("dev_hdd0/game/T/USRDIR/PATCHES", out, sizeof out) == 0);
    /* A host path that does not fit: translated as usual, out untouched. */
    char small[16] = "unchanged";
    CHECK(ps3_vfs_mod_path("/dev_hdd0/game/T/USRDIR/PATCHES", small, sizeof small) == 0);
    CHECK(strcmp(small, "unchanged") == 0);

    remove("vfs_mods_root/dev_hdd0/game/T/USRDIR/PATCHES");
    for (size_t i = sizeof dirs / sizeof dirs[0]; i-- > 0;) {
#ifdef _WIN32
        _rmdir(dirs[i]);
#else
        rmdir(dirs[i]);
#endif
    }
    printf(fails ? "vfs_mods: %d failed\n" : "vfs_mods: ok\n", fails);
    return fails != 0;
}
