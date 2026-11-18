/*
 * ps3recomp - a title's trophy set, read from its TROPHY.TRP (see trophy_set.h)
 */
#include "trophy_set.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRP_MAGIC 0xDCA24D00u

static uint32_t trp_be32(const unsigned char* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t trp_be64(const unsigned char* p)
{
    return (uint64_t)trp_be32(p) << 32 | trp_be32(p + 4);
}

/* An entry's 32-byte name field against name, without regard to case. */
static int trp_name_is(const unsigned char* field, const char* name)
{
    size_t i = 0;
    for (; i < 32 && name[i]; i++)
        if (tolower(field[i]) != tolower((unsigned char)name[i]))
            return 0;
    return !name[i] && (i == 32 || field[i] == 0);
}

unsigned char* ps3_trophy_trp_file(const char* trp, const char* name, size_t* size)
{
    if (size) *size = 0;
    FILE* f = fopen(trp, "rb");
    if (!f) return NULL;
    unsigned char h[0x40], e[256];
    unsigned char* out = NULL;
    if (fread(h, 1, sizeof h, f) == sizeof h && trp_be32(h) == TRP_MAGIC) {
        const uint32_t n = trp_be32(h + 0x10), esz = trp_be32(h + 0x14);
        if (esz >= 48 && esz <= sizeof e && n <= 4096) {
            for (uint32_t i = 0; i < n; i++) {
                if (fseek(f, (long)(0x40 + (uint64_t)i * esz), SEEK_SET) || fread(e, 1, esz, f) != esz)
                    break;
                if (!trp_name_is(e, name))
                    continue;
                const uint64_t off = trp_be64(e + 32), len = trp_be64(e + 40);
                if (len == 0 || len > (64u << 20) || fseek(f, (long)off, SEEK_SET))
                    break;
                out = (unsigned char*)malloc((size_t)len + 1);
                if (out && fread(out, 1, (size_t)len, f) == (size_t)len) {
                    out[len] = 0;                       /* a text file is then a C string */
                    if (size) *size = (size_t)len;
                } else {
                    free(out);
                    out = NULL;
                }
                break;
            }
        }
    }
    fclose(f);
    return out;
}

/* Code point c into out at o as UTF-8, if it fits; the new end. */
static size_t utf8_put(char* out, size_t o, size_t cap, unsigned c)
{
    char b[4];
    size_t n;
    if (c < 0x80)         { b[0] = (char)c; n = 1; }
    else if (c < 0x800)   { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 0x3F));
                            b[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
    else                  { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 0x3F));
                            b[2] = (char)(0x80 | (c >> 6 & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
    if (o + n >= cap) return o;
    memcpy(out + o, b, n);
    return o + n;
}

/* The XML text [s, e) into out with its entities decoded: the five named ones and &#N; / &#xH;. */
static void xml_text(const char* s, const char* e, char* out, size_t cap)
{
    size_t o = 0;
    while (s < e && o + 1 < cap) {
        if (*s == '&') {
            const char* semi = (const char*)memchr(s, ';', (size_t)(e - s));
            if (semi && semi - s <= 10) {
                unsigned c = 0;
                if (s[1] == '#')
                    c = (unsigned)(s[2] == 'x' || s[2] == 'X' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10));
                else if (!strncmp(s, "&amp;", 5))  c = '&';
                else if (!strncmp(s, "&lt;", 4))   c = '<';
                else if (!strncmp(s, "&gt;", 4))   c = '>';
                else if (!strncmp(s, "&quot;", 6)) c = '"';
                else if (!strncmp(s, "&apos;", 6)) c = '\'';
                if (c) {
                    o = utf8_put(out, o, cap, c);
                    s = semi + 1;
                    continue;
                }
            }
        }
        out[o++] = *s++;
    }
    out[o] = 0;
}

/* needle in the NUL-terminated text from s, only if it ends by e. */
static const char* find_by(const char* s, const char* e, const char* needle)
{
    const char* r = strstr(s, needle);
    return r && r + strlen(needle) <= e ? r : NULL;
}

/* The text of the first <el>...</el> in [s, e), trimmed and decoded into out; 0 if there is one. */
static int xml_elem(const char* s, const char* e, const char* el, char* out, size_t cap)
{
    char open[40], close[40];
    snprintf(open, sizeof open, "<%s>", el);
    snprintf(close, sizeof close, "</%s>", el);
    const char* a = find_by(s, e, open);
    if (!a) return -1;
    a += strlen(open);
    const char* b = find_by(a, e, close);
    if (!b) return -1;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    xml_text(a, b, out, cap);
    return 0;
}

/* Attribute a of the tag [tag, end), its raw value, into out; 0 if it is there. */
static int xml_attr(const char* tag, const char* end, const char* a, char* out, size_t cap)
{
    const size_t al = strlen(a);
    for (const char* p = tag + 1; p + al + 2 < end; p++) {
        if (!isspace((unsigned char)p[-1]) || strncmp(p, a, al) || p[al] != '=' || (p[al + 1] != '"' && p[al + 1] != '\''))
            continue;
        const char* v = p + al + 2;
        const char* ve = (const char*)memchr(v, p[al + 1], (size_t)(end - v));
        if (!ve) return -1;
        size_t n = (size_t)(ve - v);
        if (n >= cap) n = cap - 1;
        memcpy(out, v, n);
        out[n] = 0;
        return 0;
    }
    return -1;
}

/* One of the set's .SFM files, x (a C string): the set's title, and each <trophy> with or without its names. */
static int trp_parse_sfm(const char* x, PS3TrophySet* set, int with_names)
{
    const char* end = x + strlen(x);
    const char* first = strstr(x, "<trophy ");
    const char* head = first ? first : end;
    char v[64];
    if (!xml_elem(x, head, "npcommid", v, sizeof v))
        snprintf(set->npcommid, sizeof set->npcommid, "%s", v);
    if (with_names) {
        xml_elem(x, head, "title-name", set->title, sizeof set->title);
        xml_elem(x, head, "title-detail", set->detail, sizeof set->detail);
    }
    int n = 0;
    for (const char* p = first; p; p = strstr(p + 8, "<trophy ")) {
        const char* gt = strchr(p, '>');
        if (!gt) break;
        if (xml_attr(p, gt, "id", v, sizeof v)) continue;
        const int id = atoi(v);
        if (id < 0 || id >= PS3_TROPHY_SET_MAX) continue;
        PS3TrophyEntry* t = &set->t[id];
        memset(t, 0, sizeof *t);
        if (!xml_attr(p, gt, "ttype", v, sizeof v))
            t->grade = v[0] == 'P' ? 1 : v[0] == 'G' ? 2 : v[0] == 'S' ? 3 : v[0] == 'B' ? 4 : 0;
        t->hidden = !xml_attr(p, gt, "hidden", v, sizeof v) && !strcmp(v, "yes");
        t->pid = !xml_attr(p, gt, "pid", v, sizeof v) ? atoi(v) : -1;
        if (with_names && gt[-1] != '/') {
            const char* close = strstr(gt, "</trophy>");
            if (close) {
                xml_elem(gt, close, "name", t->name, sizeof t->name);
                xml_elem(gt, close, "detail", t->detail, sizeof t->detail);
            }
        }
        if (!t->name[0])
            snprintf(t->name, sizeof t->name, "Trophy %d", id);
        if (t->grade && id + 1 > set->count)
            set->count = id + 1;
        n++;
    }
    return n > 0 ? 0 : -1;
}

int ps3_trophy_set_load(const char* trp, int lang, PS3TrophySet* set)
{
    memset(set, 0, sizeof *set);
    snprintf(set->path, sizeof set->path, "%s", trp);
    set->lang = -1;
    unsigned char* x = NULL;
    if (lang >= 0) {
        char name[32];
        snprintf(name, sizeof name, "TROP_%02d.SFM", lang);
        if ((x = ps3_trophy_trp_file(trp, name, NULL)) != NULL)
            set->lang = lang;
    }
    if (!x)
        x = ps3_trophy_trp_file(trp, "TROP.SFM", NULL);
    const int names = x != NULL;
    if (!x)
        x = ps3_trophy_trp_file(trp, "TROPCONF.SFM", NULL);
    if (!x)
        return -1;
    const int r = trp_parse_sfm((const char*)x, set, names);
    free(x);
    return r == 0 && set->count > 0 ? 0 : -1;
}

const PS3TrophyEntry* ps3_trophy_set_entry(const PS3TrophySet* set, int id)
{
    if (!set || id < 0 || id >= set->count || !set->t[id].grade)
        return NULL;
    return &set->t[id];
}

const char* ps3_trophy_grade_name(int grade)
{
    switch (grade) {
    case 1: return "Platinum";
    case 2: return "Gold";
    case 3: return "Silver";
    case 4: return "Bronze";
    default: return "";
    }
}
