/*
 * ps3recomp - player settings
 *
 * What a player chooses (window mode, vsync, language, input device...) is
 * kept in an INI file: [section] headers, `key = value` lines, comments after
 * ';' or '#'. A port loads it once, before any guest code runs; the runtime
 * reads a setting where it used to read an environment variable.
 *
 * A setting's value comes from, first match wins:
 *   1. a command-line override (ps3_settings_override),
 *   2. the environment variable the caller names, if it is set,
 *   3. the file,
 *   4. the port's default (ps3_settings_default),
 *   5. the caller's default.
 * So a test can still force anything with an environment variable, and a run
 * with no file behaves as before. Each setting is logged once, with where its
 * value came from, the first time it is read.
 *
 * Diagnostics stay environment variables; this is for player-facing choices.
 *
 * Loading, overriding, setting and saving are for the start-up thread before
 * guest threads exist. Reading is fine from any thread after that.
 */
#ifndef PS3EMU_PS3_SETTINGS_H
#define PS3EMU_PS3_SETTINGS_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the file. 0 = loaded, 1 = no such file (defaults apply), -1 = could
 * not be read. A second call replaces what the first one read. */
int ps3_settings_load(const char* path);

/* The path last given to load or save (even when the file did not exist yet),
 * or "" when there is none. */
const char* ps3_settings_path(void);

/* A command-line value; it wins over the environment and the file. */
void ps3_settings_override(const char* section, const char* key, const char* value);

/* A port's own default, for a title whose players want something other than
 * the runtime's default; the file still wins over it. */
void ps3_settings_default(const char* section, const char* key, const char* value);

/* Change the file's value (the launcher's edits). The line is changed in
 * place, or added at the end of its section; the section is added when the
 * file has none. 0 = done, -1 = out of memory. */
int ps3_settings_set(const char* section, const char* key, const char* value);

/* Write the file back: every line as it was read, comments and keys this
 * build does not know included, with ps3_settings_set's changes. 0 = written. */
int ps3_settings_save(const char* path);

/* The value of section.key, or `def`. `env` may be NULL. The string stays
 * valid until the next load, override or set. */
const char* ps3_settings_str(const char* section, const char* key, const char* env, const char* def);

/* on/off, true/false, yes/no, 1/0 (any case). Anything else is reported and
 * gives `def`. */
int ps3_settings_bool(const char* section, const char* key, const char* env, int def);

/* Decimal or 0x hex. Anything else is reported and gives `def`. */
long ps3_settings_int(const char* section, const char* key, const char* env, long def);

/* A decimal number such as 0.24. Anything else is reported and gives `def`. */
double ps3_settings_num(const char* section, const char* key, const char* env, double def);

/* Case-insensitive ASCII compare, for values such as "auto" or "off". */
int ps3_settings_ieq(const char* a, const char* b);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_PS3_SETTINGS_H */
