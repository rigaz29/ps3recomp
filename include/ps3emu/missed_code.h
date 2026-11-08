/*
 * ps3recomp - the code a run executed without a lift of it (runtime/diag/missed_code.c)
 */
#ifndef PS3EMU_MISSED_CODE_H
#define PS3EMU_MISSED_CODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Record to `path`, adding to what earlier runs left there; tools/missed_code.py reads it.
 * Call once, before the title's threads start. A later call is ignored. */
void ps3_missed_code_to(const char* path);

/* Write the file now if anything changed since the last write. The run writes it by itself every
 * so often, and at exit. */
void ps3_missed_code_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_MISSED_CODE_H */
