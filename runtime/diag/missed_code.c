/*
 * ps3recomp - the code a run executed without a lift of it, in a file the next lift reads
 *
 * SPU code nothing was lifted for runs in the interpreter at a fraction of the compiled speed, and
 * from the outside that is only a scene that is slow for no visible reason: God of War: Ascension's
 * brothel illusion fell to 3.5 frames a second on 20 such pieces, found by hand with
 * SPU_INTERP_BYSIG and SPU_DMA_RANGE. Here the run writes down what it missed, and
 * tools/missed_code.py turns that into the lists the lifters read.
 *
 * Text, one row a line, so a person can read it and a later run can add to it:
 *
 *   ps3recomp-missed-code 1
 *   spu <16 bytes at the entry> ea=<GET's EA> size=<GET's size> off=<entry in the GET> pc=<LS> owner=<image> calls=<n> steps=<n>
 *   ppu <address> lr=<a caller> word=<first instruction> calls=<n>
 *
 * Streamed code runs from wherever it lands in local store, so what names a piece is the transfer
 * that brought it: each thread keeps its last GETs out of the executable, and an entry is credited
 * to the newest one that covers it. ea=00000000 means no GET was known. The bytes at the entry go
 * in the row so the tool can check them against the executable: a profile from another build of the
 * title then shows up as rows that do not match, with no need of a build id.
 *
 * Nothing here runs unless ps3_missed_code_to() was called: the three hooks stay NULL. They are
 * variables defined where the events happen (spu_interp.c, spu_channels.c, ppu_loader.cpp) rather
 * than calls into this file because the fixtures link those files on their own.
 */
#include "ps3emu/missed_code.h"
#include "ps3emu/log_first.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern volatile long long* (*g_spu_missed_entry)(const void* ctx, uint32_t pc, int owner, const uint32_t sig[4]);
extern void (*g_spu_missed_get)(const void* ctx, uint32_t ea, uint32_t lsa, uint32_t size);
extern void (*g_ppu_missed_hook)(uint32_t addr, uint32_t lr, uint32_t word);

#define MISSED_MAGIC "ps3recomp-missed-code 1"

enum {
    SPU_ROWS = 8192, PPU_ROWS = 1024, PROBES = 128,   /* slots are powers of two; a full table drops rows */
    GETS = 64                                         /* a thread's last GETs out of the executable */
};
/* The executable is loaded below this (ppu_loader.cpp: above it is the heap and mapped data). */
#define CODE_EA_END 0x10000000u

typedef struct {
    volatile long state;     /* 0 free, 1 being filled, 2 ready */
    uint32_t sig[4];
    uint32_t ea, size, off, pc;
    int owner;
    volatile long long calls, steps;
} spu_row;

typedef struct {
    volatile long state;
    uint32_t addr, lr, word;
    volatile long long calls;
} ppu_row;

typedef struct { const void* ctx; uint32_t ea, lsa, size; } get_note;

static spu_row s_spu[SPU_ROWS];
static ppu_row s_ppu[PPU_ROWS];
static _Thread_local get_note t_gets[GETS];
static _Thread_local unsigned t_get_next;

static char s_path[1024];
static volatile long s_writing, s_new_rows;
static volatile long long s_events, s_written_events, s_last_write;

static spu_row* spu_row_at(const uint32_t sig[4], uint32_t ea, uint32_t size, uint32_t off, uint32_t pc, int owner,
                           int* created)
{
    const uint32_t h = (sig[0] * 0x9E3779B1u) ^ (sig[1] * 0x85EBCA77u) ^ sig[2] ^ (sig[3] >> 7) ^
                       (ea * 0xC2B2AE3Du) ^ (off * 0x27D4EB2Fu) ^ (ea ? 0u : pc);
    for (unsigned k = 0; k < PROBES; k++) {
        spu_row* r = &s_spu[(h + k) & (SPU_ROWS - 1)];
        if (!__atomic_load_n(&r->state, __ATOMIC_ACQUIRE)) {
            long z = 0;
            if (__atomic_compare_exchange_n(&r->state, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                memcpy(r->sig, sig, sizeof r->sig);
                r->ea = ea; r->size = size; r->off = off; r->pc = pc; r->owner = owner;
                __atomic_store_n(&r->state, 2, __ATOMIC_RELEASE);
                if (created) *created = 1;
                return r;
            }
        }
        while (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) == 1) {}
        if (!memcmp(r->sig, sig, sizeof r->sig) && r->ea == ea && r->size == size && r->off == off &&
            (ea || (r->pc == pc && r->owner == owner)))
            return r;
    }
    return NULL;
}

static ppu_row* ppu_row_at(uint32_t addr, uint32_t lr, uint32_t word, int* created)
{
    const uint32_t h = addr * 0x9E3779B1u;
    for (unsigned k = 0; k < PROBES; k++) {
        ppu_row* r = &s_ppu[(h + k) & (PPU_ROWS - 1)];
        if (!__atomic_load_n(&r->state, __ATOMIC_ACQUIRE)) {
            long z = 0;
            if (__atomic_compare_exchange_n(&r->state, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                r->addr = addr; r->lr = lr; r->word = word;
                __atomic_store_n(&r->state, 2, __ATOMIC_RELEASE);
                if (created) *created = 1;
                return r;
            }
        }
        while (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) == 1) {}
        if (r->addr == addr) return r;
    }
    return NULL;
}

/* A write to <path>.tmp moved over the file, so a run that dies mid-write leaves the last good one. */
static void write_file(void)
{
    long z = 0;
    if (!__atomic_compare_exchange_n(&s_writing, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&s_new_rows, 0, __ATOMIC_RELEASE);
    const long long events = __atomic_load_n(&s_events, __ATOMIC_ACQUIRE);
    char tmp[sizeof s_path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    FILE* f = fopen(tmp, "w");
    int bad = !f;
    if (f) {
        fprintf(f, "%s\n", MISSED_MAGIC);
        for (unsigned i = 0; i < SPU_ROWS; i++) {
            const spu_row* r = &s_spu[i];
            if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) != 2) continue;
            /* The interpreter gave it straight back to lifted code: nothing was missed. */
            if (!__atomic_load_n(&r->steps, __ATOMIC_RELAXED)) continue;
            fprintf(f, "spu %08X%08X%08X%08X ea=%08X size=%u off=%X pc=%05X owner=%d calls=%lld steps=%lld\n",
                    r->sig[0], r->sig[1], r->sig[2], r->sig[3], r->ea, r->size, r->off, r->pc, r->owner,
                    (long long)__atomic_load_n(&r->calls, __ATOMIC_RELAXED),
                    (long long)__atomic_load_n(&r->steps, __ATOMIC_RELAXED));
        }
        for (unsigned i = 0; i < PPU_ROWS; i++) {
            const ppu_row* r = &s_ppu[i];
            if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) != 2) continue;
            fprintf(f, "ppu %08X lr=%08X word=%08X calls=%lld\n", r->addr, r->lr, r->word,
                    (long long)__atomic_load_n(&r->calls, __ATOMIC_RELAXED));
        }
        bad = ferror(f);
        if (fclose(f)) bad = 1;
    }
    if (!bad) {
#ifdef _WIN32
        bad = !MoveFileExA(tmp, s_path, MOVEFILE_REPLACE_EXISTING);
#else
        bad = rename(tmp, s_path) != 0;
#endif
    }
    if (bad) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 4)) fprintf(stderr, "[missed] cannot write %s\n", s_path);
    }
    __atomic_store_n(&s_written_events, events, __ATOMIC_RELEASE);
    __atomic_store_n(&s_last_write, (long long)time(NULL), __ATOMIC_RELEASE);
    __atomic_store_n(&s_writing, 0, __ATOMIC_RELEASE);
}

/* From the threads that hit a row, so nothing runs while nothing is missed. A new row is on disk
 * within 10 s; the counters of known rows, which change all the time, within a minute. */
static void maybe_write(void)
{
    const long long since = (long long)time(NULL) - __atomic_load_n(&s_last_write, __ATOMIC_RELAXED);
    if (since < 10 || (since < 60 && !__atomic_load_n(&s_new_rows, __ATOMIC_RELAXED))) return;
    write_file();
}

static void note_get(const void* ctx, uint32_t ea, uint32_t lsa, uint32_t size)
{
    if (ea >= CODE_EA_END || size < 16) return;
    get_note* g = &t_gets[t_get_next++ & (GETS - 1)];
    g->ctx = ctx; g->ea = ea; g->lsa = lsa; g->size = size;
}

static volatile long long* note_entry(const void* ctx, uint32_t pc, int owner, const uint32_t sig[4])
{
    uint32_t ea = 0, size = 0, off = 0;
    for (unsigned k = 1; k <= GETS; k++) {
        const get_note* g = &t_gets[(t_get_next - k) & (GETS - 1)];
        if (g->ctx == ctx && pc - g->lsa < g->size) {
            ea = g->ea; size = g->size; off = pc - g->lsa;
            break;
        }
    }
    int created = 0;
    spu_row* r = spu_row_at(sig, ea, size, off, pc, owner, &created);
    __atomic_fetch_add(&s_events, 1, __ATOMIC_RELAXED);
    if (created) __atomic_fetch_add(&s_new_rows, 1, __ATOMIC_RELEASE);
    maybe_write();
    if (!r) return NULL;
    __atomic_fetch_add(&r->calls, 1, __ATOMIC_RELAXED);
    return &r->steps;
}

static void note_ppu(uint32_t addr, uint32_t lr, uint32_t word)
{
    int created = 0;
    ppu_row* r = ppu_row_at(addr, lr, word, &created);
    __atomic_fetch_add(&s_events, 1, __ATOMIC_RELAXED);
    if (created) __atomic_fetch_add(&s_new_rows, 1, __ATOMIC_RELEASE);
    maybe_write();
    if (r) __atomic_fetch_add(&r->calls, 1, __ATOMIC_RELAXED);
}

/* What earlier runs left. A file that is not a profile is left out, and the first write replaces it. */
static long load_file(void)
{
    FILE* f = fopen(s_path, "r");
    if (!f) return 0;
    char line[256];
    long rows = 0;
    if (!fgets(line, sizeof line, f) || strncmp(line, MISSED_MAGIC, strlen(MISSED_MAGIC))) {
        fprintf(stderr, "[missed] %s is not a profile of this version: starting a new one\n", s_path);
        fclose(f);
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned s0, s1, s2, s3, ea, size, off, pc, addr, lr, word;
        int owner;
        long long calls, steps;
        if (sscanf(line, "spu %8x%8x%8x%8x ea=%x size=%u off=%x pc=%x owner=%d calls=%lld steps=%lld",
                   &s0, &s1, &s2, &s3, &ea, &size, &off, &pc, &owner, &calls, &steps) == 11) {
            const uint32_t sig[4] = {s0, s1, s2, s3};
            spu_row* r = spu_row_at(sig, ea, size, off, pc, owner, NULL);
            if (r) { r->calls += calls; r->steps += steps; rows++; }
        } else if (sscanf(line, "ppu %8x lr=%x word=%x calls=%lld", &addr, &lr, &word, &calls) == 4) {
            ppu_row* r = ppu_row_at(addr, lr, word, NULL);
            if (r) { r->calls += calls; rows++; }
        }
    }
    fclose(f);
    return rows;
}

void ps3_missed_code_flush(void)
{
    if (s_path[0] && __atomic_load_n(&s_events, __ATOMIC_ACQUIRE) != __atomic_load_n(&s_written_events, __ATOMIC_ACQUIRE))
        write_file();
}

void ps3_missed_code_to(const char* path)
{
    if (s_path[0] || !path || !*path) return;
    snprintf(s_path, sizeof s_path, "%s", path);
    const long rows = load_file();
    __atomic_store_n(&s_last_write, (long long)time(NULL), __ATOMIC_RELEASE);
    g_spu_missed_get = note_get;
    g_spu_missed_entry = note_entry;
    g_ppu_missed_hook = note_ppu;
    atexit(ps3_missed_code_flush);
    fprintf(stderr, "[missed] recording to %s, %ld rows so far\n", s_path, rows);
}
