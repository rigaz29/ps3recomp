/*
 * ps3recomp - player settings (see include/ps3emu/ps3_settings.h)
 *
 * The file is kept as its lines, so writing it back keeps the player's
 * comments and the keys this build does not know. Lookups scan the lines
 * (a settings file is a few dozen of them, and each setting is read once);
 * what a lookup resolved is kept in a small table, which is also what makes
 * the log line appear only once per setting.
 */
#include "ps3emu/ps3_settings.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* section;
    char* key;
    char* value;        /* NULL = no value anywhere: the caller's default */
    const char* from;   /* "command line", the environment variable, "file" */
    int   warned;
} resolved_t;

typedef struct {
    char* section;
    char* key;
    char* value;
} override_t;

static char**      s_lines;
static int         s_nlines, s_cap;
static char        s_path[1024];
static resolved_t* s_res;
static int         s_nres, s_res_cap;
static override_t* s_ovr;
static int         s_novr;
static int         s_lock;   /* the reads: several threads may resolve their first settings at once */

static void lk(void)  { while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE)) { } }
static void ulk(void) { __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE); }

static char* dup_n(const char* s, size_t n)
{
    char* d = (char*)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
static char* dup_s(const char* s) { return dup_n(s, strlen(s)); }

int ps3_settings_ieq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int ieq_n(const char* a, size_t n, const char* b)
{
    size_t i = 0;
    for (; i < n && b[i]; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return i == n && !b[i];
}

static void trim(const char** b, const char** e)
{
    while (*b < *e && isspace((unsigned char)**b)) (*b)++;
    while (*e > *b && isspace((unsigned char)(*e)[-1])) (*e)--;
}

static void forget_resolved(void)
{
    for (int i = 0; i < s_nres; i++) {
        free(s_res[i].section); free(s_res[i].key); free(s_res[i].value);
    }
    s_nres = 0;
}

/* One line: a section header (sets *sec), a key line (sets the key and value
 * ranges and returns 1), or anything else (0). */
static int parse_line(const char* line, const char** sec_b, const char** sec_e,
                      const char** kb, const char** ke, const char** vb, const char** ve)
{
    const char* b = line;
    const char* e = line + strlen(line);
    trim(&b, &e);
    if (b == e || *b == ';' || *b == '#') return 0;
    if (*b == '[') {
        const char* c = memchr(b, ']', (size_t)(e - b));
        if (!c) return 0;
        *sec_b = b + 1; *sec_e = c;
        trim(sec_b, sec_e);
        return 0;
    }
    const char* eq = memchr(b, '=', (size_t)(e - b));
    if (!eq) return 0;
    *kb = b; *ke = eq;
    trim(kb, ke);
    const char* v = eq + 1;
    const char* w = e;
    trim(&v, &w);
    if (v < w && *v == '"') {
        const char* q = memchr(v + 1, '"', (size_t)(w - v - 1));
        if (q) { *vb = v + 1; *ve = q; return *kb < *ke; }
    }
    /* A comment starts at ';' or '#' after a space, or as the whole value. */
    for (const char* p = v; p < w; p++)
        if ((*p == ';' || *p == '#') && (p == v || isspace((unsigned char)p[-1]))) { w = p; break; }
    trim(&v, &w);
    *vb = v; *ve = w;
    return *kb < *ke;
}

/* The line index of section.key in the file, or -1. *last_in_section is the
 * last key line (or the header) of the section, -1 when there is no section. */
static int find_line(const char* section, const char* key, int* last_in_section,
                     const char** vb, const char** ve)
{
    const char* sb = NULL;
    const char* se = NULL;
    int in = 0, found = -1;
    if (last_in_section) *last_in_section = -1;
    for (int i = 0; i < s_nlines; i++) {
        const char *kb = NULL, *ke = NULL, *b = NULL, *e = NULL;
        const char* osb = sb;
        int is_key = parse_line(s_lines[i], &sb, &se, &kb, &ke, &b, &e);
        if (sb != osb) {   /* a section header */
            in = ieq_n(sb, (size_t)(se - sb), section);
            if (in && last_in_section) *last_in_section = i;
            continue;
        }
        if (!in || !is_key) continue;
        if (last_in_section) *last_in_section = i;
        if (found < 0 && ieq_n(kb, (size_t)(ke - kb), key)) {
            found = i;
            if (vb) { *vb = b; *ve = e; }
        }
    }
    return found;
}

static int push_line(int at, char* text)
{
    if (!text) return -1;
    if (s_nlines == s_cap) {
        int cap = s_cap ? s_cap * 2 : 64;
        char** n = (char**)realloc(s_lines, (size_t)cap * sizeof *n);
        if (!n) { free(text); return -1; }
        s_lines = n; s_cap = cap;
    }
    memmove(s_lines + at + 1, s_lines + at, (size_t)(s_nlines - at) * sizeof *s_lines);
    s_lines[at] = text;
    s_nlines++;
    return 0;
}

int ps3_settings_load(const char* path)
{
    for (int i = 0; i < s_nlines; i++) free(s_lines[i]);
    s_nlines = 0;
    forget_resolved();
    s_path[0] = 0;
    if (!path || !*path) return 1;
    snprintf(s_path, sizeof s_path, "%s", path);
    FILE* f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? 1 : -1;
    char buf[4096];
    int rc = 0;
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
        if (push_line(s_nlines, dup_n(buf, n))) { rc = -1; break; }
    }
    fclose(f);
    return rc;
}

const char* ps3_settings_path(void) { return s_path; }

void ps3_settings_override(const char* section, const char* key, const char* value)
{
    override_t* n = (override_t*)realloc(s_ovr, (size_t)(s_novr + 1) * sizeof *n);
    if (!n) return;
    s_ovr = n;
    s_ovr[s_novr].section = dup_s(section);
    s_ovr[s_novr].key = dup_s(key);
    s_ovr[s_novr].value = dup_s(value);
    s_novr++;
    forget_resolved();
}

static int needs_quotes(const char* v)
{
    if (!*v) return 0;
    if (isspace((unsigned char)v[0]) || isspace((unsigned char)v[strlen(v) - 1])) return 1;
    return strpbrk(v, ";#\"") != NULL;
}

int ps3_settings_set(const char* section, const char* key, const char* value)
{
    char line[2048];
    const char* q = needs_quotes(value) ? "\"" : "";
    int last;
    const int at = find_line(section, key, &last, NULL, NULL);
    forget_resolved();
    if (at >= 0) {
        /* Keep the key as the file spells it, and a comment after the value. */
        const char *sb = NULL, *se = NULL, *kb = NULL, *ke = NULL, *vb = NULL, *ve = NULL;
        parse_line(s_lines[at], &sb, &se, &kb, &ke, &vb, &ve);
        const char* tail = ve;
        if (*tail == '"') tail++;
        while (*tail && isspace((unsigned char)*tail)) tail++;
        snprintf(line, sizeof line, "%.*s = %s%s%s%s%s", (int)(ke - kb), kb, q, value, q,
                 *tail ? " " : "", tail);
        char* t = dup_s(line);
        if (!t) return -1;
        free(s_lines[at]);
        s_lines[at] = t;
        return 0;
    }
    snprintf(line, sizeof line, "%s = %s%s%s", key, q, value, q);
    if (last >= 0) return push_line(last + 1, dup_s(line));
    if (s_nlines && s_lines[s_nlines - 1][0] && push_line(s_nlines, dup_s(""))) return -1;
    char head[512];
    snprintf(head, sizeof head, "[%s]", section);
    if (push_line(s_nlines, dup_s(head))) return -1;
    return push_line(s_nlines, dup_s(line));
}

int ps3_settings_save(const char* path)
{
    if (!path || !*path) path = s_path;
    if (!*path) return -1;
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < s_nlines; i++) fprintf(f, "%s\n", s_lines[i]);
    const int bad = ferror(f);
    if (fclose(f) || bad) return -1;
    if (path != s_path) snprintf(s_path, sizeof s_path, "%s", path);
    return 0;
}

static resolved_t* resolve(const char* section, const char* key, const char* env, const char* def)
{
    for (int i = 0; i < s_nres; i++)
        if (ps3_settings_ieq(s_res[i].section, section) && ps3_settings_ieq(s_res[i].key, key))
            return &s_res[i];
    if (s_nres == s_res_cap) {
        int cap = s_res_cap ? s_res_cap * 2 : 32;
        resolved_t* n = (resolved_t*)realloc(s_res, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        s_res = n; s_res_cap = cap;
    }
    resolved_t* r = &s_res[s_nres];
    memset(r, 0, sizeof *r);
    for (int i = s_novr - 1; i >= 0 && !r->from; i--)
        if (ps3_settings_ieq(s_ovr[i].section, section) && ps3_settings_ieq(s_ovr[i].key, key)) {
            r->value = dup_s(s_ovr[i].value);
            r->from = "command line";
        }
    const char* e = (!r->from && env) ? getenv(env) : NULL;
    if (e) { r->value = dup_s(e); r->from = env; }
    if (!r->from) {
        const char *vb = NULL, *ve = NULL;
        if (find_line(section, key, NULL, &vb, &ve) >= 0) {
            r->value = dup_n(vb, (size_t)(ve - vb));
            r->from = "file";
        }
    }
    r->section = dup_s(section);
    r->key = dup_s(key);
    if (!r->section || !r->key) { free(r->section); free(r->key); free(r->value); return NULL; }
    s_nres++;
    if (r->from) printf("[settings] %s.%s = %s (%s)\n", section, key, r->value ? r->value : "", r->from);
    else         printf("[settings] %s.%s = %s (default)\n", section, key, def ? def : "");
    return r;
}

const char* ps3_settings_str(const char* section, const char* key, const char* env, const char* def)
{
    lk();
    const resolved_t* r = resolve(section, key, env, def);
    const char* v = r && r->value ? r->value : def;
    ulk();
    return v;
}

static void bad_value(resolved_t* r, const char* what)
{
    if (r->warned) return;
    r->warned = 1;
    printf("[settings] %s.%s = '%s' (%s) is not %s; using the default\n",
           r->section, r->key, r->value, r->from, what);
}

int ps3_settings_bool(const char* section, const char* key, const char* env, int def)
{
    char d[8];
    snprintf(d, sizeof d, "%s", def ? "on" : "off");
    static const char* const on[]  = { "on", "true", "yes", "1" };
    static const char* const off[] = { "off", "false", "no", "0" };
    lk();
    resolved_t* r = resolve(section, key, env, d);
    int v = def;
    if (r && r->value) {
        int i = 0;
        for (; i < 4; i++) {
            if (ps3_settings_ieq(r->value, on[i])) { v = 1; break; }
            if (ps3_settings_ieq(r->value, off[i])) { v = 0; break; }
        }
        if (i == 4) bad_value(r, "on or off");
    }
    ulk();
    return v;
}

long ps3_settings_int(const char* section, const char* key, const char* env, long def)
{
    char d[32];
    snprintf(d, sizeof d, "%ld", def);
    lk();
    resolved_t* r = resolve(section, key, env, d);
    long v = def;
    if (r && r->value) {
        char* end = NULL;
        errno = 0;
        const long n = strtol(r->value, &end, 0);
        if (end == r->value || *end || errno) bad_value(r, "a whole number");
        else v = n;
    }
    ulk();
    return v;
}

double ps3_settings_num(const char* section, const char* key, const char* env, double def)
{
    char d[32];
    snprintf(d, sizeof d, "%g", def);
    lk();
    resolved_t* r = resolve(section, key, env, d);
    double v = def;
    if (r && r->value) {
        char* end = NULL;
        errno = 0;
        const double n = strtod(r->value, &end);
        if (end == r->value || *end || errno) bad_value(r, "a number");
        else v = n;
    }
    ulk();
    return v;
}
