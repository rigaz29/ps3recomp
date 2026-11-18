/*
 * ps3recomp - a title's trophy set, read from its TROPHY.TRP
 *
 * A TROPHY.TRP is a flat archive: a 0x40-byte header (magic 0xDCA24D00, version, size, the number of
 * files at +0x10 and the size of an entry at +0x14), then the entries from 0x40, each a 32-byte name, a
 * 64-bit offset and a 64-bit size, all big-endian. In it: TROPCONF.SFM (each trophy's grade, whether it
 * is hidden and the platinum it counts toward), TROP.SFM (the same with the names and details, in the
 * set's own language), TROP_<nn>.SFM (the same in PS3 language nn), ICON0.PNG and TROP<nnn>.PNG, 240 x
 * 240. The .SFM files are XML. A set with encrypted .ESFM files in their place is not read.
 *
 * It is read from the title's own files at run time; the runtime ships none of it.
 */
#ifndef PS3RECOMP_TROPHY_SET_H
#define PS3RECOMP_TROPHY_SET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_TROPHY_SET_MAX    128
#define PS3_TROPHY_NAME_MAX   128
#define PS3_TROPHY_DETAIL_MAX 1024

typedef struct PS3TrophyEntry {
    int  grade;                             /* 1 platinum, 2 gold, 3 silver, 4 bronze; 0 no such trophy */
    int  hidden;
    int  pid;                               /* the platinum it counts toward, -1 none */
    char name[PS3_TROPHY_NAME_MAX];         /* UTF-8 */
    char detail[PS3_TROPHY_DETAIL_MAX];     /* UTF-8 */
} PS3TrophyEntry;

typedef struct PS3TrophySet {
    char npcommid[16];                      /* "NPWR00802_00" */
    char title[PS3_TROPHY_NAME_MAX];        /* UTF-8 */
    char detail[PS3_TROPHY_DETAIL_MAX];
    char path[1024];                        /* the TROPHY.TRP it came from, a host path */
    int  lang;                              /* the PS3 language of the names, -1 the set's own (TROP.SFM) */
    int  count;                             /* trophy ids 0 .. count - 1 */
    PS3TrophyEntry t[PS3_TROPHY_SET_MAX];   /* by id */
} PS3TrophySet;

/* Read the set at trp (a host path) with its names in PS3 language lang (TROP_<lang>.SFM), else in the set's
 * own (TROP.SFM), else none (TROPCONF.SFM alone). 0 on success, -1 if it is missing or cannot be read. */
int ps3_trophy_set_load(const char* trp, int lang, PS3TrophySet* set);

/* One file of the TROPHY.TRP at trp ("TROP005.PNG"), malloc'd, its size in *size; NULL if there is none. */
unsigned char* ps3_trophy_trp_file(const char* trp, const char* name, size_t* size);

/* Trophy id of the set, or NULL. */
const PS3TrophyEntry* ps3_trophy_set_entry(const PS3TrophySet* set, int id);

/* "Platinum", "Gold", "Silver", "Bronze" for a grade, "" for another. */
const char* ps3_trophy_grade_name(int grade);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_TROPHY_SET_H */
