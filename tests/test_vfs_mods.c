/* PS3_MODS_DIRS and PS3_MODS_ROOT (runtime/syscalls/vfs_mods.h): which guest paths the mod folders answer. */
#include "vfs_mods.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define mk(p) _mkdir(p)
#define rm_dir(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define mk(p) mkdir(p, 0755)
#define rm_dir(p) rmdir(p)
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static void set(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* The guest path's answer: its host path, or "-" for none. */
static const char* look(const char* guest)
{
    static char out[256];
    if (!ps3_vfs_mod_path(guest, out, sizeof out)) return "-";
    return out;
}

/* "untitled": without PS3_TITLE, a folder named as titles are is the game's. */
int main(int argc, char** argv)
{
    const int untitled = argc > 1 && !strcmp(argv[1], "untitled");
    const char* dirs[] = { "vfs_mods_root", "vfs_mods_root/dev_hdd0", "vfs_mods_root/dev_hdd0/game",
                           "vfs_mods_root/dev_hdd0/game/T", "vfs_mods_root/dev_hdd0/game/T/USRDIR",
                           "vfs_mods_root/dev_hdd0/game/T/USRDIR/SUB",
                           "vfs_mods_a", "vfs_mods_a/WAD", "vfs_mods_a/WAD/PS3_BE", "vfs_mods_b" };
    const char* files[] = { "vfs_mods_root/dev_hdd0/game/T/USRDIR/PATCHES", "vfs_mods_root/dev_hdd0/game/T/USRDIR/ROOTONLY",
                            "vfs_mods_a/PATCHES", "vfs_mods_a/WAD/PS3_BE/SHELL.WAD", "vfs_mods_b/PATCHES",
                            "vfs_mods_b/onlyb.bin", "vfs_mods_b/TPPS6.PSARC" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) mk(dirs[i]);
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) put(files[i], "6");
    set("PS3_MODS_ROOT", "vfs_mods_root");
    /* Two folders for USRDIR, the first one first: a trailing slash and a backslash are taken too. */
    set("PS3_MODS_DIRS", "vfs_mods_a/|vfs_mods_b||");
    if (untitled) {
        set("PS3_TITLE", "");
        /* Any title's folder, as titles are named, but not a game data folder nor another name. */
        CHECK(strcmp(look("/dev_hdd0/game/BCES01741/USRDIR/TPPS6.PSARC"), "vfs_mods_b/TPPS6.PSARC") == 0);
        CHECK(strcmp(look("/dev_hdd0/game/NPUA80918/USRDIR/PATCHES"), "vfs_mods_a/PATCHES") == 0);
        CHECK(strcmp(look("/dev_hdd0/game/BCES01741-DATA/USRDIR/PATCHES"), "-") == 0);
        CHECK(strcmp(look("/dev_hdd0/game/bces01741/USRDIR/PATCHES"), "-") == 0);
        CHECK(strcmp(look("/dev_hdd0/game/T/USRDIR/TPPS6.PSARC"), "-") == 0);
        CHECK(strcmp(look("/dev_bdvd/PS3_GAME/USRDIR/TPPS6.PSARC"), "vfs_mods_b/TPPS6.PSARC") == 0);
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) remove(files[i]);
        for (size_t i = sizeof dirs / sizeof dirs[0]; i-- > 0;) rm_dir(dirs[i]);
        printf(fails ? "vfs_mods: %d failed\n" : "vfs_mods: ok\n", fails);
        return fails != 0;
    }
    set("PS3_TITLE", "T");

    /* PS3_MODS_DIRS: the first folder that has the file wins, over PS3_MODS_ROOT as well. */
    CHECK(strcmp(look("/dev_hdd0/game/T/USRDIR/PATCHES"), "vfs_mods_a/PATCHES") == 0);
    /* A file only the second folder has, through the title's folder and the disc's USRDIR: one folder serves both. */
    CHECK(strcmp(look("/dev_bdvd/PS3_GAME/USRDIR/TPPS6.PSARC"), "vfs_mods_b/TPPS6.PSARC") == 0);
    CHECK(strcmp(look("/dev_hdd0/game/T/USRDIR/TPPS6.PSARC"), "vfs_mods_b/TPPS6.PSARC") == 0);
    /* Not another title's folder, nor the title's game data folder: those have a USRDIR of their own. */
    CHECK(strcmp(look("/dev_hdd0/game/BCES01741/USRDIR/TPPS6.PSARC"), "-") == 0);
    CHECK(strcmp(look("/dev_hdd0/game/T-DATA/USRDIR/PATCHES"), "-") == 0);
    /* Names without regard to case, and down the folders. */
    CHECK(strcmp(look("/dev_hdd0/game/T/USRDIR/ONLYB.BIN"), "vfs_mods_b/onlyb.bin") == 0);
    CHECK(strcmp(look("/dev_bdvd/PS3_GAME/USRDIR/WAD/PS3_BE/shell.wad"), "vfs_mods_a/WAD/PS3_BE/SHELL.WAD") == 0);
    /* Not the game's USRDIR: not theirs. A folder is not a file. */
    CHECK(strcmp(look("/dev_hdd0/game/T/PATCHES"), "-") == 0);
    CHECK(strcmp(look("/dev_hdd0/home/00000001/PATCHES"), "-") == 0);
    CHECK(strcmp(look("/dev_bdvd/PS3_GAME/PATCHES"), "-") == 0);
    CHECK(strcmp(look("/dev_hdd0/game//USRDIR/PATCHES"), "-") == 0);
    CHECK(strcmp(look("/dev_bdvd/PS3_GAME/USRDIR/WAD"), "-") == 0);
    CHECK(strcmp(look("/dev_bdvd/PS3_GAME/USRDIR/"), "-") == 0);

    /* PS3_MODS_ROOT: a file in its folder answers its guest path, where PS3_MODS_DIRS has none. */
    CHECK(strcmp(look("/dev_hdd0/game/T/USRDIR/ROOTONLY"), "vfs_mods_root/dev_hdd0/game/T/USRDIR/ROOTONLY") == 0);
    /* Nothing there: translated as usual. */
    CHECK(strcmp(look("/dev_hdd0/game/T/USRDIR/ARCHIVES"), "-") == 0);
    /* A folder is not a file: listings stay the guest's. */
    CHECK(strcmp(look("/dev_hdd0/game/T/USRDIR/SUB"), "-") == 0);
    /* No way out of the folders, and no relative guest paths. */
    CHECK(strcmp(look("/dev_hdd0/game/T/USRDIR/../USRDIR/PATCHES"), "-") == 0);
    CHECK(strcmp(look("dev_hdd0/game/T/USRDIR/PATCHES"), "-") == 0);
    /* A host path that does not fit: translated as usual, out untouched. */
    char small[16] = "unchanged";
    CHECK(ps3_vfs_mod_path("/dev_hdd0/game/T/USRDIR/PATCHES", small, sizeof small) == 0);
    CHECK(strcmp(small, "unchanged") == 0);
    CHECK(ps3_vfs_mod_path("/dev_hdd0/game/T/USRDIR/ROOTONLY", small, sizeof small) == 0);
    CHECK(strcmp(small, "unchanged") == 0);

    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) remove(files[i]);
    for (size_t i = sizeof dirs / sizeof dirs[0]; i-- > 0;) rm_dir(dirs[i]);
    printf(fails ? "vfs_mods: %d failed\n" : "vfs_mods: ok\n", fails);
    return fails != 0;
}
