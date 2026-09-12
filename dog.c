/*
 * dog.c -- parser for the .dog text data format (DOG/1, magic `.dog/1.0`).
 *
 * Single-file, zero dependencies, standard C library only
 * (stdio / stdlib / string / ctype). Mirrors the reference parser dog.py
 * and the dog.js / dog.go / dog.rs / dog.kt ports.
 *
 * Library API:
 *     DogValue *dog_parse(const char *text, char **err);
 *   Parses a .dog document and returns the equivalent JSON-able value tree,
 *   or NULL on error with *err set to a malloc'd message (free it; may be
 *   NULL to ignore). Free the tree with dog_free().
 *
 * CLI (dog.py-style):
 *     gcc -Wall -Wextra -O2 -std=c11 dog.c -o dog && ./dog file.dog
 *   Prints the equivalent JSON to stdout (pretty-printed like dog.py).
 *   Parse errors go to stderr, exit 1.
 *
 * Hard rule (fleet-wide, pending the founder's D1 ruling on abbreviated
 * directives): truncated known directive names (`l:`, `di:`) are REJECTED
 * as hard errors -- dog.py's silent leniency is NOT copied, because a
 * silently mis-resolved directive (e.g. `l: json` parsing a JSON body as
 * native dog) violates the no-guessing rule.
 *
 * This product was made by DOGS -- https://wearedogs.net
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <locale.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "dog: out of memory\n"); exit(3); }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "dog: out of memory\n"); exit(3); }
    return q;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ------------------------------------------------------------------ */
/* growable string buffer (always NUL-terminated)                      */
/* ------------------------------------------------------------------ */

typedef struct { char *data; size_t len, cap; } Str;

static void str_init(Str *s) { s->data = NULL; s->len = 0; s->cap = 0; }

static void str_reserve(Str *s, size_t extra) {
    if (s->len + extra + 1 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 64;
        while (nc < s->len + extra + 1) nc *= 2;
        s->data = xrealloc(s->data, nc);
        s->cap = nc;
    }
}

static void str_pushn(Str *s, const char *p, size_t n) {
    str_reserve(s, n);
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
}

static void str_pushc(Str *s, char c) { str_pushn(s, &c, 1); }
static void str_pushz(Str *s, const char *z) { str_pushn(s, z, strlen(z)); }

/* transfer ownership of the buffer (never NULL) */
static char *str_take(Str *s) {
    char *p = s->data ? s->data : xstrdup("");
    s->data = NULL; s->len = 0; s->cap = 0;
    return p;
}

static void str_free(Str *s) { free(s->data); s->data = NULL; s->len = s->cap = 0; }

/* ------------------------------------------------------------------ */
/* string vector / string->string map                                  */
/* ------------------------------------------------------------------ */

typedef struct { char **items; size_t len, cap; } StrVec;

static void sv_init(StrVec *v) { v->items = NULL; v->len = 0; v->cap = 0; }

static void sv_push(StrVec *v, char *s) { /* takes ownership */
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, v->cap * sizeof *v->items);
    }
    v->items[v->len++] = s;
}

static void sv_free(StrVec *v) {
    for (size_t i = 0; i < v->len; i++) free(v->items[i]);
    free(v->items);
    v->items = NULL; v->len = v->cap = 0;
}

typedef struct { char **names; char **vals; size_t len, cap; } StrMap;

static void sm_init(StrMap *m) { m->names = NULL; m->vals = NULL; m->len = 0; m->cap = 0; }

/* last one wins */
static void sm_set(StrMap *m, const char *name, const char *val) {
    for (size_t i = 0; i < m->len; i++) {
        if (strcmp(m->names[i], name) == 0) {
            free(m->vals[i]);
            m->vals[i] = xstrdup(val);
            return;
        }
    }
    if (m->len == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->names = xrealloc(m->names, m->cap * sizeof *m->names);
        m->vals = xrealloc(m->vals, m->cap * sizeof *m->vals);
    }
    m->names[m->len] = xstrdup(name);
    m->vals[m->len] = xstrdup(val);
    m->len++;
}

static const char *sm_get(const StrMap *m, const char *name) {
    for (size_t i = 0; i < m->len; i++)
        if (strcmp(m->names[i], name) == 0) return m->vals[i];
    return NULL;
}

static void sm_free(StrMap *m) {
    for (size_t i = 0; i < m->len; i++) { free(m->names[i]); free(m->vals[i]); }
    free(m->names); free(m->vals);
    m->names = m->vals = NULL; m->len = m->cap = 0;
}

/* ------------------------------------------------------------------ */
/* values                                                              */
/* ------------------------------------------------------------------ */

typedef enum { V_NULL, V_BOOL, V_INT, V_FLOAT, V_STR, V_ARR, V_MAP } VType;

typedef struct DogValue DogValue;
typedef struct { char *key; DogValue *val; } MapEnt;

struct DogValue {
    VType type;
    union {
        int boolean;
        char *integer;  /* canonical decimal literal, e.g. "-7", "1000" */
        double number;
        char *str;
        struct { DogValue **items; size_t len, cap; } arr;
        struct { MapEnt *ents; size_t len, cap; } map;
    } u;
};

void dog_free(DogValue *v); /* defined below; used by map_set */

static DogValue *v_new(VType t) {
    DogValue *v = xmalloc(sizeof *v);
    v->type = t;
    memset(&v->u, 0, sizeof v->u);
    return v;
}

static DogValue *v_null(void) { return v_new(V_NULL); }
static DogValue *v_bool(int b) { DogValue *v = v_new(V_BOOL); v->u.boolean = b ? 1 : 0; return v; }
static DogValue *v_int_text(char *t) { DogValue *v = v_new(V_INT); v->u.integer = t; return v; } /* takes t */
static DogValue *v_float(double d) { DogValue *v = v_new(V_FLOAT); v->u.number = d; return v; }
static DogValue *v_str(const char *s) { DogValue *v = v_new(V_STR); v->u.str = xstrdup(s); return v; }
static DogValue *v_str_take(char *s) { DogValue *v = v_new(V_STR); v->u.str = s; return v; } /* takes s */
static DogValue *v_arr(void) { return v_new(V_ARR); }
static DogValue *v_map(void) { return v_new(V_MAP); }

static void arr_push(DogValue *a, DogValue *item) {
    if (a->u.arr.len == a->u.arr.cap) {
        a->u.arr.cap = a->u.arr.cap ? a->u.arr.cap * 2 : 8;
        a->u.arr.items = xrealloc(a->u.arr.items, a->u.arr.cap * sizeof *a->u.arr.items);
    }
    a->u.arr.items[a->u.arr.len++] = item;
}

/* last duplicate wins (keeps first-insertion position, like Python dicts) */
static void map_set(DogValue *m, const char *key, DogValue *val) {
    for (size_t i = 0; i < m->u.map.len; i++) {
        if (strcmp(m->u.map.ents[i].key, key) == 0) {
            dog_free(m->u.map.ents[i].val);
            m->u.map.ents[i].val = val;
            return;
        }
    }
    if (m->u.map.len == m->u.map.cap) {
        m->u.map.cap = m->u.map.cap ? m->u.map.cap * 2 : 8;
        m->u.map.ents = xrealloc(m->u.map.ents, m->u.map.cap * sizeof *m->u.map.ents);
    }
    m->u.map.ents[m->u.map.len].key = xstrdup(key);
    m->u.map.ents[m->u.map.len].val = val;
    m->u.map.len++;
}

static DogValue *map_get(DogValue *m, const char *key) {
    for (size_t i = 0; i < m->u.map.len; i++)
        if (strcmp(m->u.map.ents[i].key, key) == 0) return m->u.map.ents[i].val;
    return NULL;
}

/* remove an entry and hand back its value without freeing it */
static DogValue *map_take(DogValue *m, const char *key) {
    for (size_t i = 0; i < m->u.map.len; i++) {
        if (strcmp(m->u.map.ents[i].key, key) == 0) {
            DogValue *v = m->u.map.ents[i].val;
            free(m->u.map.ents[i].key);
            memmove(&m->u.map.ents[i], &m->u.map.ents[i + 1],
                    (m->u.map.len - i - 1) * sizeof *m->u.map.ents);
            m->u.map.len--;
            return v;
        }
    }
    return NULL;
}

void dog_free(DogValue *v) {
    if (!v) return;
    size_t i;
    switch (v->type) {
    case V_INT: free(v->u.integer); break;
    case V_STR: free(v->u.str); break;
    case V_ARR:
        for (i = 0; i < v->u.arr.len; i++) dog_free(v->u.arr.items[i]);
        free(v->u.arr.items);
        break;
    case V_MAP:
        for (i = 0; i < v->u.map.len; i++) {
            free(v->u.map.ents[i].key);
            dog_free(v->u.map.ents[i].val);
        }
        free(v->u.map.ents);
        break;
    default: break;
    }
    free(v);
}

/* ------------------------------------------------------------------ */
/* parse errors (first failure wins)                                   */
/* ------------------------------------------------------------------ */

typedef struct { char *msg; } P;

static void fail(P *p, const char *fmt, ...) {
    if (p->msg) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    p->msg = xstrdup(buf);
}

/* ------------------------------------------------------------------ */
/* small string utilities                                              */
/* ------------------------------------------------------------------ */

static int trim_lead_len(const unsigned char *p) {
    if (isspace(*p)) return 1;
    if (p[0] == 0xC2 && p[1] == 0xA0) return 2;                       /* nbsp */
    if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) return 3; /* U+2028/29 */
    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) return 3;       /* BOM */
    return 0;
}

static int trim_trail_len(const unsigned char *start, const unsigned char *end) {
    if (end > start && isspace(end[-1])) return 1;
    if (end - 2 >= start && end[-2] == 0xC2 && end[-1] == 0xA0) return 2;
    if (end - 3 >= start && end[-3] == 0xE2 && end[-2] == 0x80 &&
        (end[-1] == 0xA8 || end[-1] == 0xA9)) return 3;
    if (end - 3 >= start && end[-3] == 0xEF && end[-2] == 0xBB && end[-1] == 0xBF) return 3;
    return 0;
}

static char *trim(const char *s) {
    const unsigned char *a = (const unsigned char *)s;
    const unsigned char *b = a + strlen((const char *)a);
    int n;
    while (a < b && (n = trim_lead_len(a)) > 0) a += n;
    while (b > a && (n = trim_trail_len(a, b)) > 0) b -= n;
    return xstrndup((const char *)a, (size_t)(b - a));
}

static char *to_lower_ascii(const char *s) {
    char *o = xstrdup(s);
    for (char *q = o; *q; q++)
        if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + ('a' - 'A'));
    return o;
}

/* split on runs of ASCII whitespace */
static void split_ws(const char *s, StrVec *out) {
    sv_init(out);
    const char *p = s;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *q = p;
        while (*q && !isspace((unsigned char)*q)) q++;
        sv_push(out, xstrndup(p, (size_t)(q - p)));
        p = q;
    }
}

/* split on a literal (non-empty) separator string */
static void split_str(const char *s, const char *sep, StrVec *out) {
    sv_init(out);
    size_t sl = strlen(sep);
    const char *p = s;
    for (;;) {
        const char *f = strstr(p, sep);
        if (!f) { sv_push(out, xstrdup(p)); break; }
        sv_push(out, xstrndup(p, (size_t)(f - p)));
        p = f + sl;
    }
}

/* non-overlapping left-to-right replace; old_ must be non-empty */
static char *replace_all(const char *s, const char *old_, const char *new_) {
    size_t olen = strlen(old_), nlen = strlen(new_);
    Str out;
    str_init(&out);
    const char *p = s;
    for (;;) {
        const char *f = strstr(p, old_);
        if (!f) { str_pushz(&out, p); break; }
        str_pushn(&out, p, (size_t)(f - p));
        str_pushn(&out, new_, nlen);
        p = f + olen;
    }
    return str_take(&out);
}

/* byte length of the UTF-8 character starting at p */
static size_t utf8_len(const unsigned char *p) {
    unsigned char c = p[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return 2;
    if (c >= 0xE0 && c <= 0xEF) return 3;
    if (c >= 0xF0 && c <= 0xF4) return 4;
    return 1;
}

static void utf8_encode(Str *out, unsigned int cp) {
    if (cp < 0x80) {
        str_pushc(out, (char)cp);
    } else if (cp < 0x800) {
        str_pushc(out, (char)(0xC0 | (cp >> 6)));
        str_pushc(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        str_pushc(out, (char)(0xE0 | (cp >> 12)));
        str_pushc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        str_pushc(out, (char)(0x80 | (cp & 0x3F)));
    } else {
        str_pushc(out, (char)(0xF0 | (cp >> 18)));
        str_pushc(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
        str_pushc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        str_pushc(out, (char)(0x80 | (cp & 0x3F)));
    }
}

/* ------------------------------------------------------------------ */
/* header                                                              */
/* ------------------------------------------------------------------ */

static const char *KNOWN_DIRECTIVES[] = {
    "lang", "dict", "rep", "keys", "sep", "indent", "quote"
};
#define N_KNOWN_DIRECTIVES 7

/* split on \r\n | \n | \r | NEL(U+0085) | U+2028 | U+2029 (byte sequences) */
static void split_lines(const char *text, StrVec *out) {
    sv_init(out);
    size_t n = strlen(text), i = 0;
    while (i <= n) {
        size_t j = i, blen = 0;
        while (j < n) {
            if (text[j] == '\r' && j + 1 < n && text[j + 1] == '\n') { blen = 2; break; }
            if (text[j] == '\n' || text[j] == '\r') { blen = 1; break; }
            if ((unsigned char)text[j] == 0xC2 && j + 1 < n && (unsigned char)text[j + 1] == 0x85) { blen = 2; break; }
            if ((unsigned char)text[j] == 0xE2 && j + 2 < n && (unsigned char)text[j + 1] == 0x80 &&
                ((unsigned char)text[j + 2] == 0xA8 || (unsigned char)text[j + 2] == 0xA9)) { blen = 3; break; }
            j++;
        }
        sv_push(out, xstrndup(text + i, j - i));
        if (j >= n) break;
        i = j + blen;
    }
}

/* Split a header line into whitespace-separated tokens. Double-quoted
 * spans may contain spaces (`keys:"a b"`); inside quotes a backslash
 * escapes the next character (`\"`, `\\`). Returns 0 on success,
 * nonzero after fail() on unterminated quote. */
static int tokenize_header_line(P *p, const char *line, StrVec *out) {
    Str cur;
    str_init(&cur);
    int in_quotes = 0, escaped = 0;
    for (const unsigned char *s = (const unsigned char *)line; *s; s++) {
        char ch = (char)*s;
        if (in_quotes) {
            if (escaped) { str_pushc(&cur, ch); escaped = 0; }
            else if (ch == '\\') escaped = 1;
            else if (ch == '"') in_quotes = 0;
            else str_pushc(&cur, ch);
        } else if (ch == '"') {
            in_quotes = 1;
        } else if (ch == ' ' || ch == '\t' || ch == '\r' ||
                   ch == '\v' || ch == '\f') {
            if (cur.len) sv_push(out, str_take(&cur));
        } else {
            str_pushc(&cur, ch);
        }
    }
    if (in_quotes || escaped) {
        fail(p, "unterminated quote in header line: '%s'", line);
        str_free(&cur);
        return 1;
    }
    if (cur.len) sv_push(out, str_take(&cur));
    else str_free(&cur);
    return 0;
}

/* Split raw text into header directives and body lines.
 * The header is exactly ONE line: line 1, starting with the magic
 * `.dog/1.0` followed by space-separated `name:value` params. A reader
 * never has to guess where the header ends -- it is always line 1.
 * Unknown directives are ignored (forward compat) -- EXCEPT truncated
 * known-directive names, which are hard errors. The body is every line
 * after line 1; one optional blank line right after the header is
 * skipped. */
static void parse_header(P *p, const char *text, StrMap *dirs,
                         char ***body_out, size_t *nbody_out) {
    const char *t = text;
    if ((unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB &&
        (unsigned char)t[2] == 0xBF) t += 3; /* single BOM, like dog.js */
    StrVec lines;
    sv_init(&lines);
    split_lines(t, &lines);
    *body_out = NULL; *nbody_out = 0;
    if (lines.len == 0) {
        fail(p, "first line must be the magic '.dog/1.0'");
        sv_free(&lines);
        return;
    }
    StrVec toks;
    sv_init(&toks);
    if (tokenize_header_line(p, lines.items[0], &toks)) {
        sv_free(&toks);
        sv_free(&lines);
        return;
    }
    if (toks.len == 0 || strcmp(toks.items[0], ".dog/1.0") != 0) {
        fail(p, "first line must be the magic '.dog/1.0'");
        sv_free(&toks);
        sv_free(&lines);
        return;
    }
    for (size_t i = 1; i < toks.len; i++) {
        char *tok = toks.items[i];
        char *ci = strchr(tok, ':');
        if (!ci) {
            fail(p, "bad header directive '%s' (want name:value)", tok);
            sv_free(&toks);
            sv_free(&lines);
            return;
        }
        size_t nlen = (size_t)(ci - tok);
        char *name = xmalloc(nlen + 1);
        for (size_t q = 0; q < nlen; q++) {
            char c = tok[q];
            name[q] = (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        }
        name[nlen] = '\0';
        int bad = nlen == 0;
        for (size_t q = 0; q < nlen && !bad; q++)
            if (isspace((unsigned char)name[q])) bad = 1;
        if (bad) {
            fail(p, "bad directive name in '%s'", tok);
            free(name);
            sv_free(&toks);
            sv_free(&lines);
            return;
        }
        /* Abbreviated directives are NOT in the spec (needs founder
         * sign-off, bag log D1). A truncated known-directive name is a
         * hard error here, never silently resolved -- and never confused
         * with the unknown-directive ignore rule, which only covers
         * names that are not truncations of known directives. */
        for (size_t k = 0; k < N_KNOWN_DIRECTIVES; k++) {
            const char *full = KNOWN_DIRECTIVES[k];
            if (strcmp(name, full) != 0 && strncmp(full, name, strlen(name)) == 0) {
                fail(p, "truncated directive '%s:' is not valid -- abbreviations are not "
                        "in the spec (want '%s:')", name, full);
                free(name);
                sv_free(&toks);
                sv_free(&lines);
                return;
            }
        }
        sm_set(dirs, name, ci + 1);
        free(name);
    }
    sv_free(&toks);
    size_t body_start = 1;
    if (body_start < lines.len) {
        char *s = trim(lines.items[body_start]);
        int blank = s[0] == '\0';
        free(s);
        if (blank) body_start++;
    }
    if (body_start < lines.len) {
        *body_out = xmalloc((lines.len - body_start) * sizeof(char *));
        for (size_t i = body_start; i < lines.len; i++)
            (*body_out)[i - body_start] = xstrdup(lines.items[i]);
        *nbody_out = lines.len - body_start;
    }
    sv_free(&lines);
}

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

typedef struct { char *tok; char *exp; } RepPair;

typedef struct {
    char *lang;                 /* lowercased */
    StrMap dict;                /* alias -> real key */
    RepPair *rep; size_t rep_len, rep_cap;
    char **keys; size_t keys_len, keys_cap;  /* positional row keys */
    char *sep;
    long indent;
    char *quote;
} Config;

static void rep_push(Config *cfg, const char *tok, const char *exp) {
    if (cfg->rep_len == cfg->rep_cap) {
        cfg->rep_cap = cfg->rep_cap ? cfg->rep_cap * 2 : 8;
        cfg->rep = xrealloc(cfg->rep, cfg->rep_cap * sizeof *cfg->rep);
    }
    cfg->rep[cfg->rep_len].tok = xstrdup(tok);
    cfg->rep[cfg->rep_len].exp = xstrdup(exp);
    cfg->rep_len++;
}

static void build_config(P *p, const StrMap *dirs, Config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    sm_init(&cfg->dict);
    const char *lv = sm_get(dirs, "lang");
    if (lv) { char *t = trim(lv); cfg->lang = to_lower_ascii(t); free(t); }
    else cfg->lang = xstrdup("dog");
    if (strcmp(cfg->lang, "dog") != 0 && strcmp(cfg->lang, "json") != 0 &&
        strcmp(cfg->lang, "xml") != 0 && strcmp(cfg->lang, "yaml") != 0) {
        fail(p, "unsupported lang: '%s' (want dog|json|xml|yaml)", lv ? lv : "dog");
        return;
    }
    const char *dv = sm_get(dirs, "dict");
    if (dv) {
        StrVec parts;
        split_ws(dv, &parts);
        for (size_t i = 0; i < parts.len && !p->msg; i++) {
            char *eq = strchr(parts.items[i], '=');
            if (!eq) {
                fail(p, "bad dict pair \"%s\" (want alias=key)", parts.items[i]);
            } else if (eq == parts.items[i] || eq[1] == '\0') {
                fail(p, "bad dict pair \"%s\"", parts.items[i]);
            } else {
                char *alias = xstrndup(parts.items[i], (size_t)(eq - parts.items[i]));
                sm_set(&cfg->dict, alias, eq + 1);
                free(alias);
            }
        }
        sv_free(&parts);
        if (p->msg) return;
    }
    const char *rv = sm_get(dirs, "rep");
    if (rv) {
        StrVec parts;
        split_ws(rv, &parts);
        for (size_t i = 0; i < parts.len && !p->msg; i++) {
            char *eq = strchr(parts.items[i], '=');
            if (!eq) {
                fail(p, "bad rep pair \"%s\" (want token=expansion)", parts.items[i]);
            } else if (eq == parts.items[i]) {
                fail(p, "empty rep token in \"%s\"", parts.items[i]);
            } else {
                char *tok = xstrndup(parts.items[i], (size_t)(eq - parts.items[i]));
                rep_push(cfg, tok, eq + 1);
                free(tok);
            }
        }
        sv_free(&parts);
        if (p->msg) return;
        /* longest token wins on overlap (stable for equal lengths) */
        for (size_t i = 1; i < cfg->rep_len; i++) {
            RepPair tmp = cfg->rep[i];
            size_t j = i;
            while (j > 0 && strlen(cfg->rep[j - 1].tok) < strlen(tmp.tok)) {
                cfg->rep[j] = cfg->rep[j - 1];
                j--;
            }
            cfg->rep[j] = tmp;
        }
    }
    const char *kv = sm_get(dirs, "keys");
    if (kv) {
        StrVec parts;
        split_ws(kv, &parts);
        if (parts.len == 0) {
            fail(p, "empty keys: directive");
            sv_free(&parts);
            return;
        }
        cfg->keys = parts.items; /* take ownership of the array */
        cfg->keys_len = parts.len;
        cfg->keys_cap = parts.cap;
    }
    cfg->indent = 2;
    const char *iv = sm_get(dirs, "indent");
    if (iv) {
        const char *q = iv;
        if (*q == '+' || *q == '-') q++;
        int ok = *q != '\0';
        for (; *q; q++) if (!isdigit((unsigned char)*q)) ok = 0;
        if (!ok) { fail(p, "indent: must be an integer"); return; }
        long n = strtol(iv, NULL, 10);
        if (n < 1) { fail(p, "indent: must be >= 1"); return; }
        cfg->indent = n;
    }
    const char *sv = sm_get(dirs, "sep");
    cfg->sep = xstrdup(sv ? sv : "|");
    if (cfg->sep[0] == '\0') { fail(p, "sep: must not be empty"); return; }
    const char *qv = sm_get(dirs, "quote");
    cfg->quote = xstrdup(qv ? qv : "\"");
    if (cfg->quote[0] == '\0') { fail(p, "quote: must not be empty"); return; }
}

static void free_config(Config *cfg) {
    free(cfg->lang);
    sm_free(&cfg->dict);
    for (size_t i = 0; i < cfg->rep_len; i++) { free(cfg->rep[i].tok); free(cfg->rep[i].exp); }
    free(cfg->rep);
    for (size_t i = 0; i < cfg->keys_len; i++) free(cfg->keys[i]);
    free(cfg->keys);
    free(cfg->sep);
    free(cfg->quote);
}

/* ------------------------------------------------------------------ */
/* expansion                                                           */
/* ------------------------------------------------------------------ */

/* Apply rep: substitutions to a scalar string.
 *
 * A backslash escapes the next character (so `\~` is a literal tilde when
 * `~` is a rep token). Longest tokens substitute first. Escaped characters
 * are hidden behind a U+E000 placeholder during substitution so a token
 * can never match inside them. */
static char *expand_rep(const char *s, const RepPair *rep, size_t rep_len) {
    if (rep_len == 0) return xstrdup(s);
    static const char PH[4] = "\xEE\x80\x80"; /* U+E000, like dog.py */
    Str out;
    str_init(&out);
    StrVec saved;
    sv_init(&saved);
    const unsigned char *q = (const unsigned char *)s;
    while (*q) {
        if (*q == '\\' && q[1]) {
            size_t cl = utf8_len(q + 1);
            str_pushn(&out, PH, 3);
            sv_push(&saved, xstrndup((const char *)(q + 1), cl));
            q += 1 + cl;
        } else {
            str_pushc(&out, (char)*q);
            q++;
        }
    }
    char *stage = str_take(&out);
    for (size_t r = 0; r < rep_len; r++) {
        char *next = replace_all(stage, rep[r].tok, rep[r].exp);
        free(stage);
        stage = next;
    }
    Str fin;
    str_init(&fin);
    size_t si = 0;
    const char *z = stage;
    while (*z) {
        if (si < saved.len &&
            (unsigned char)z[0] == 0xEE && (unsigned char)z[1] == 0x80 &&
            (unsigned char)z[2] == 0x80) {
            str_pushz(&fin, saved.items[si++]);
            z += 3;
        } else {
            str_pushc(&fin, *z);
            z++;
        }
    }
    free(stage);
    sv_free(&saved);
    return str_take(&fin);
}

/* ------------------------------------------------------------------ */
/* scalars                                                             */
/* ------------------------------------------------------------------ */

static int is_int_scalar(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (*p == '-') p++;
    if (!isdigit(*p)) return 0;
    while (isdigit(*p)) p++;
    return *p == '\0';
}

/* mirrors ^-?(\d+\.\d*|\.\d+|\d+)([eE][-+]?\d+)?$ (int check runs first) */
static int is_float_scalar(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (*p == '-') p++;
    int has = 0;
    while (isdigit(*p)) { p++; has = 1; }
    if (*p == '.') {
        p++;
        while (isdigit(*p)) { p++; has = 1; }
    }
    if (!has) return 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit(*p)) return 0;
        while (isdigit(*p)) p++;
    }
    return *p == '\0';
}

/* canonicalize an integer literal: "-007" -> "-7", "-0" -> "0".
 * Arbitrary precision is preserved (never goes through strtoll). */
static char *normalize_int(const char *s) {
    int neg = s[0] == '-';
    const char *d = s + neg;
    while (*d == '0') d++;
    if (*d == '\0') return xstrdup("0");
    size_t n = strlen(d);
    char *out = xmalloc(n + (size_t)neg + 1);
    if (neg) out[0] = '-';
    memcpy(out + neg, d, n + 1);
    return out;
}

/* shortest round-trip decimal, Python-repr style ("1000.0", "-0.0015") */
static void fmt_double(char *out, double d) {
    if (isnan(d)) { strcpy(out, "NaN"); return; }
    if (isinf(d)) { strcpy(out, d > 0 ? "Infinity" : "-Infinity"); return; }
    if (d == 0.0) { strcpy(out, signbit(d) ? "-0.0" : "0.0"); return; }
    char tmp[64];
    int p;
    for (p = 1; p <= 17; p++) {
        snprintf(tmp, sizeof tmp, "%.*g", p, d);
        if (strtod(tmp, NULL) == d) break;
    }
    /* decompose [-]I[.F][eX] */
    const char *q = tmp;
    int neg = 0;
    if (*q == '-') { neg = 1; q++; }
    else if (*q == '+') q++;
    char digits[64];
    size_t nd = 0, point_pos = 0;
    while (*q && *q != '.' && *q != 'e' && *q != 'E') { digits[nd++] = *q++; point_pos++; }
    if (*q == '.') {
        q++;
        while (*q && *q != 'e' && *q != 'E') digits[nd++] = *q++;
    }
    long E = 0;
    if (*q == 'e' || *q == 'E') E = strtol(q + 1, NULL, 10);
    digits[nd] = '\0';
    size_t lz = 0;
    while (lz < nd && digits[lz] == '0') lz++;
    size_t L = nd - lz; /* significant digits, >= 1 for nonzero d */
    long exp10 = E + (long)point_pos - (long)nd + (long)L - 1;
    Str o;
    str_init(&o);
    if (neg) str_pushc(&o, '-');
    if (exp10 < -4 || exp10 >= 16) {
        str_pushc(&o, digits[lz]);
        if (L > 1) {
            str_pushc(&o, '.');
            str_pushn(&o, digits + lz + 1, L - 1);
        }
        str_pushc(&o, 'e');
        char eb[32]; /* exp10 of a double is within +-324; plenty of room */
        if (exp10 < 0) snprintf(eb, sizeof eb, "-%02ld", -exp10);
        else snprintf(eb, sizeof eb, "+%02ld", exp10);
        str_pushz(&o, eb);
    } else {
        long ipos = exp10 + 1; /* digits before the point */
        if (ipos <= 0) {
            str_pushz(&o, "0.");
            for (long i = 0; i < -ipos; i++) str_pushc(&o, '0');
            str_pushn(&o, digits + lz, L);
        } else if ((size_t)ipos >= L) {
            str_pushn(&o, digits + lz, L);
            for (long i = 0; i < ipos - (long)L; i++) str_pushc(&o, '0');
            str_pushz(&o, ".0");
        } else {
            str_pushn(&o, digits + lz, (size_t)ipos);
            str_pushc(&o, '.');
            str_pushn(&o, digits + lz + (size_t)ipos, L - (size_t)ipos);
        }
    }
    strcpy(out, o.data);
    str_free(&o);
}

/* Parse a scalar string: quoting, then JSON-ish literal coercion. */
static DogValue *parse_scalar(const char *s, const RepPair *rep, size_t rep_len,
                              const char *quote) {
    char *e = expand_rep(s, rep, rep_len);
    DogValue *v;
    size_t elen = strlen(e), qlen = strlen(quote);
    if (qlen == 1 && elen >= 2 && e[0] == quote[0] && e[elen - 1] == quote[0]) {
        char *inner = xstrndup(e + 1, elen - 2);
        char pat[3] = { '\\', quote[0], '\0' };
        char *t1 = replace_all(inner, pat, quote);
        free(inner);
        char *t2 = replace_all(t1, "\\\\", "\\");
        free(t1);
        v = v_str_take(t2);
    } else {
        char *low = to_lower_ascii(e);
        if (strcmp(low, "null") == 0) v = v_null();
        else if (strcmp(low, "true") == 0) v = v_bool(1);
        else if (strcmp(low, "false") == 0) v = v_bool(0);
        else if (is_int_scalar(e)) v = v_int_text(normalize_int(e));
        else if (is_float_scalar(e)) v = v_float(strtod(e, NULL));
        else v = v_str(e);
        free(low);
    }
    free(e);
    return v;
}

/* scalar coercion without rep (for XML text / YAML subset values) */
static DogValue *coerce_loose(const char *s) {
    return parse_scalar(s, NULL, 0, "\"");
}

/* ------------------------------------------------------------------ */
/* native `dog` body: indentation-based maps / lists / scalars / rows  */
/* ------------------------------------------------------------------ */

typedef struct { long indent; char *text; char *raw; } Item;
typedef struct { Item *items; size_t len, cap; } ItemVec;

static void iv_push(ItemVec *v, long indent, char *text, char *raw) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 16;
        v->items = xrealloc(v->items, v->cap * sizeof *v->items);
    }
    v->items[v->len].indent = indent;
    v->items[v->len].text = text;
    v->items[v->len].raw = raw;
    v->len++;
}

static void iv_free(ItemVec *v) {
    for (size_t i = 0; i < v->len; i++) { free(v->items[i].text); free(v->items[i].raw); }
    free(v->items);
    v->items = NULL; v->len = v->cap = 0;
}

static long indent_of(P *p, const char *raw) {
    size_t i = 0;
    while (raw[i] == ' ') i++;
    if (raw[i] == '\t') {
        fail(p, "tabs are not allowed for indentation: '%s'", raw);
        return 0;
    }
    return (long)i;
}

static void body_items(P *p, char **body, size_t nbody, ItemVec *out) {
    out->items = NULL; out->len = out->cap = 0;
    for (size_t k = 0; k < nbody; k++) {
        const char *raw = body[k];
        long ind = indent_of(p, raw); /* may fail on tabs -- same order as dog.py */
        if (p->msg) { iv_free(out); return; }
        char *tr = trim(raw);
        if (tr[0] == '\0') { free(tr); continue; }
        const char *ls = raw;
        while (*ls == ' ') ls++;
        if (*ls == '#') { free(tr); continue; }
        iv_push(out, ind, tr, xstrdup(raw));
    }
}

/* `^([^\s:]+)\s*:(?:[ \t]+(.*))?$` -- key_out is malloc'd;
 * when it matches, *has_val says whether a value group participated
 * (val_out is malloc'd, possibly ""), else val_out is NULL. */
static int match_pair(const char *text, char **key_out, char **val_out, int *has_val) {
    const char *p = text;
    const char *ks = p;
    while (*p && *p != ':' && !isspace((unsigned char)*p)) p++;
    if (p == ks) return 0;
    const char *ke = p;
    while (isspace((unsigned char)*p)) p++;
    if (*p != ':') return 0;
    p++;
    if (*p == '\0') {
        *has_val = 0;
        *val_out = NULL;
    } else {
        if (*p != ' ' && *p != '\t') return 0;
        while (*p == ' ' || *p == '\t') p++;
        *has_val = 1;
        *val_out = xstrdup(p);
    }
    *key_out = xstrndup(ks, (size_t)(ke - ks));
    return 1;
}

static int is_list_item(const char *text) {
    return text[0] == '-' && (text[1] == '\0' || text[1] == ' ');
}

static DogValue *parse_value(P *p, ItemVec *items, size_t *ip, long level, Config *cfg);
static DogValue *parse_map(P *p, ItemVec *items, size_t *ip, long level, Config *cfg,
                           const char *seed_key, DogValue *seed_val);
static DogValue *parse_list(P *p, ItemVec *items, size_t *ip, long level, Config *cfg);

/* `key: |` -- following deeper-indented lines become one literal string */
static char *parse_block_scalar(P *p, ItemVec *items, size_t *ip, long level) {
    size_t i = *ip;
    if (i >= items->len || items->items[i].indent <= level) {
        fail(p, "expected an indented block after `|`");
        return NULL;
    }
    long base = items->items[i].indent;
    Str buf;
    str_init(&buf);
    int first = 1;
    while (i < items->len && items->items[i].indent >= base) {
        const char *raw = items->items[i].raw;
        if (!first) str_pushc(&buf, '\n');
        first = 0;
        if ((long)strlen(raw) >= base) str_pushz(&buf, raw + base);
        i++;
    }
    *ip = i;
    return str_take(&buf);
}

static const char *dict_lookup(const Config *cfg, const char *key) {
    const char *m = sm_get(&cfg->dict, key);
    return m ? m : key;
}

static DogValue *parse_map(P *p, ItemVec *items, size_t *ip, long level, Config *cfg,
                           const char *seed_key, DogValue *seed_val) {
    DogValue *d = v_map();
    if (seed_key) map_set(d, seed_key, seed_val);
    size_t i = *ip;
    while (i < items->len) {
        Item *it = &items->items[i];
        if (it->indent < level) break;
        if (it->indent != level) {
            fail(p, "bad indentation in map at '%s'", it->text);
            dog_free(d);
            return NULL;
        }
        char *rk = NULL, *val = NULL;
        int has_val = 0;
        if (!match_pair(it->text, &rk, &val, &has_val)) {
            fail(p, "expected `key: value` in map, got '%s'", it->text);
            dog_free(d);
            return NULL;
        }
        char *key = expand_rep(dict_lookup(cfg, rk), cfg->rep, cfg->rep_len);
        free(rk);
        i++;
        DogValue *v = NULL;
        if (!has_val || val[0] == '\0') {
            if (i < items->len && items->items[i].indent > level)
                v = parse_value(p, items, &i, items->items[i].indent, cfg);
            else
                v = v_null();
        } else if (strcmp(val, "|") == 0) {
            char *bs = parse_block_scalar(p, items, &i, level);
            v = bs ? v_str_take(bs) : NULL;
        } else {
            v = parse_scalar(val, cfg->rep, cfg->rep_len, cfg->quote);
        }
        free(val);
        if (p->msg) {
            free(key);
            dog_free(v);
            dog_free(d);
            return NULL;
        }
        map_set(d, key, v);
        free(key);
    }
    *ip = i;
    return d;
}

static DogValue *parse_list(P *p, ItemVec *items, size_t *ip, long level, Config *cfg) {
    DogValue *out = v_arr();
    size_t i = *ip;
    while (i < items->len) {
        Item *it = &items->items[i];
        if (it->indent < level) break;
        if (it->indent != level) {
            fail(p, "bad indentation in list at '%s'", it->text);
            dog_free(out);
            return NULL;
        }
        if (!is_list_item(it->text)) break;
        char *rest = trim(it->text + 1);
        i++;
        if (rest[0] == '\0') {
            DogValue *v;
            if (i < items->len && items->items[i].indent > level)
                v = parse_value(p, items, &i, items->items[i].indent, cfg);
            else
                v = v_null();
            free(rest);
            if (p->msg) { dog_free(v); dog_free(out); return NULL; }
            arr_push(out, v);
            continue;
        }
        char *rk = NULL, *val = NULL;
        int has_val = 0;
        if (match_pair(rest, &rk, &val, &has_val)) {
            char *key = expand_rep(dict_lookup(cfg, rk), cfg->rep, cfg->rep_len);
            free(rk);
            if (!has_val || val[0] == '\0') {
                /* `- key:` -- deeper block is the VALUE of key (YAML rule) */
                DogValue *v;
                if (i < items->len && items->items[i].indent > level)
                    v = parse_value(p, items, &i, items->items[i].indent, cfg);
                else
                    v = v_null();
                free(val); free(rest);
                if (p->msg) { free(key); dog_free(v); dog_free(out); return NULL; }
                DogValue *d = v_map();
                map_set(d, key, v);
                free(key);
                arr_push(out, d);
            } else if (strcmp(val, "|") == 0) {
                char *bs = parse_block_scalar(p, items, &i, level);
                free(val); free(rest);
                if (p->msg) { free(key); free(bs); dog_free(out); return NULL; }
                DogValue *d = v_map();
                map_set(d, key, v_str_take(bs));
                free(key);
                arr_push(out, d);
            } else {
                DogValue *seed_v = parse_scalar(val, cfg->rep, cfg->rep_len, cfg->quote);
                free(val); free(rest);
                DogValue *d;
                if (i < items->len && items->items[i].indent > level) {
                    d = parse_map(p, items, &i, items->items[i].indent, cfg, key, seed_v);
                    free(key);
                    if (p->msg) { dog_free(out); return NULL; }
                } else {
                    d = v_map();
                    map_set(d, key, seed_v);
                    free(key);
                }
                arr_push(out, d);
            }
        } else {
            DogValue *v = parse_scalar(rest, cfg->rep, cfg->rep_len, cfg->quote);
            free(rest);
            arr_push(out, v);
        }
    }
    *ip = i;
    return out;
}

static DogValue *parse_value(P *p, ItemVec *items, size_t *ip, long level, Config *cfg) {
    Item *it = &items->items[*ip];
    if (it->indent != level) {
        fail(p, "bad indentation at '%s'", it->text);
        return NULL;
    }
    if (is_list_item(it->text)) return parse_list(p, items, ip, level, cfg);
    char *rk = NULL, *val = NULL;
    int has_val = 0;
    if (match_pair(it->text, &rk, &val, &has_val)) {
        free(rk); free(val);
        return parse_map(p, items, ip, level, cfg, NULL, NULL);
    }
    DogValue *v = parse_scalar(it->text, cfg->rep, cfg->rep_len, cfg->quote);
    (*ip)++;
    return v;
}

/* keys: mode -- every body line is a sep-separated positional row */
static DogValue *parse_rows(P *p, ItemVec *items, Config *cfg) {
    size_t nk = cfg->keys_len;
    char **keys = xmalloc((nk ? nk : 1) * sizeof *keys);
    for (size_t i = 0; i < nk; i++)
        keys[i] = xstrdup(dict_lookup(cfg, cfg->keys[i])); /* aliases allowed, rep not applied */
    DogValue *out = v_arr();
    for (size_t n = 0; n < items->len; n++) {
        Item *it = &items->items[n];
        if (it->indent != 0) {
            fail(p, "positional rows must not be indented: '%s'", it->text);
            goto fail;
        }
        StrVec fields;
        split_str(it->text, cfg->sep, &fields);
        if (fields.len != nk) {
            fail(p, "row has %lu fields but keys: declares %lu: '%s'",
                 (unsigned long)fields.len, (unsigned long)nk, it->text);
            sv_free(&fields);
            goto fail;
        }
        DogValue *d = v_map();
        for (size_t f = 0; f < nk; f++) {
            char *fv = trim(fields.items[f]);
            DogValue *v = (fv[0] == '\0') ? v_null()
                : parse_scalar(fv, cfg->rep, cfg->rep_len, cfg->quote);
            map_set(d, keys[f], v);
            free(fv);
            if (p->msg) { dog_free(d); sv_free(&fields); goto fail; }
        }
        sv_free(&fields);
        arr_push(out, d);
    }
    for (size_t i = 0; i < nk; i++) free(keys[i]);
    free(keys);
    return out;
fail:
    for (size_t i = 0; i < nk; i++) free(keys[i]);
    free(keys);
    dog_free(out);
    return NULL;
}

static DogValue *parse_dog_body(P *p, char **body, size_t nbody, Config *cfg) {
    ItemVec items;
    body_items(p, body, nbody, &items);
    if (p->msg) return NULL;
    if (items.len == 0) { iv_free(&items); return v_null(); }
    if (cfg->keys_len > 0) {
        DogValue *v = parse_rows(p, &items, cfg);
        iv_free(&items);
        return v;
    }
    size_t i = 0;
    DogValue *v = parse_value(p, &items, &i, 0, cfg);
    if (p->msg) { dog_free(v); iv_free(&items); return NULL; }
    if (i != items.len) {
        fail(p, "unexpected trailing content: '%s'", items.items[i].text);
        dog_free(v); iv_free(&items);
        return NULL;
    }
    if (items.items[0].indent != 0) {
        fail(p, "top-level content must not be indented");
        dog_free(v); iv_free(&items);
        return NULL;
    }
    iv_free(&items);
    return v;
}

/* ------------------------------------------------------------------ */
/* json body -- byte-identical, the header alone makes it .dog         */
/* ------------------------------------------------------------------ */

typedef struct { P *p; const char *s; size_t n, pos; } JP;

static void j_skip(JP *j) {
    while (j->pos < j->n) {
        char c = j->s[j->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->pos++;
        else break;
    }
}

static DogValue *j_value(JP *j);

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static DogValue *j_string(JP *j) {
    j->pos++; /* consume opening quote */
    Str out;
    str_init(&out);
    for (;;) {
        if (j->pos >= j->n) {
            fail(j->p, "bad JSON body: unterminated string");
            str_free(&out);
            return NULL;
        }
        char c = j->s[j->pos];
        if (c == '"') { j->pos++; break; }
        if (c == '\\') {
            j->pos++;
            if (j->pos >= j->n) {
                fail(j->p, "bad JSON body: unterminated escape");
                str_free(&out);
                return NULL;
            }
            char e = j->s[j->pos++];
            switch (e) {
            case '"': str_pushc(&out, '"'); break;
            case '\\': str_pushc(&out, '\\'); break;
            case '/': str_pushc(&out, '/'); break;
            case 'b': str_pushc(&out, '\b'); break;
            case 'f': str_pushc(&out, '\f'); break;
            case 'n': str_pushc(&out, '\n'); break;
            case 'r': str_pushc(&out, '\r'); break;
            case 't': str_pushc(&out, '\t'); break;
            case 'u': {
                if (j->pos + 4 > j->n) {
                    fail(j->p, "bad JSON body: bad \\u escape");
                    str_free(&out);
                    return NULL;
                }
                int d0 = hex_val(j->s[j->pos]), d1 = hex_val(j->s[j->pos + 1]);
                int d2 = hex_val(j->s[j->pos + 2]), d3 = hex_val(j->s[j->pos + 3]);
                if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) {
                    fail(j->p, "bad JSON body: bad \\u escape");
                    str_free(&out);
                    return NULL;
                }
                unsigned int cp = (unsigned int)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
                j->pos += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (j->pos + 6 <= j->n && j->s[j->pos] == '\\' && j->s[j->pos + 1] == 'u') {
                        int e0 = hex_val(j->s[j->pos + 2]), e1 = hex_val(j->s[j->pos + 3]);
                        int e2 = hex_val(j->s[j->pos + 4]), e3 = hex_val(j->s[j->pos + 5]);
                        unsigned int lo = (unsigned int)((e0 << 12) | (e1 << 8) | (e2 << 4) | e3);
                        if (e0 < 0 || e1 < 0 || e2 < 0 || e3 < 0 || lo < 0xDC00 || lo > 0xDFFF) {
                            fail(j->p, "bad JSON body: bad surrogate pair");
                            str_free(&out);
                            return NULL;
                        }
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        j->pos += 6;
                    } else {
                        fail(j->p, "bad JSON body: lone surrogate");
                        str_free(&out);
                        return NULL;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    fail(j->p, "bad JSON body: lone surrogate");
                    str_free(&out);
                    return NULL;
                }
                utf8_encode(&out, cp);
                break;
            }
            default:
                fail(j->p, "bad JSON body: bad escape '\\%c'", e);
                str_free(&out);
                return NULL;
            }
        } else if ((unsigned char)c < 0x20) {
            fail(j->p, "bad JSON body: unescaped control character in string");
            str_free(&out);
            return NULL;
        } else {
            str_pushc(&out, c);
            j->pos++;
        }
    }
    return v_str_take(str_take(&out));
}

static DogValue *j_number(JP *j) {
    size_t start = j->pos, q = j->pos, n = j->n;
    const char *s = j->s;
    if (q < n && s[q] == '-') q++;
    if (q < n && s[q] == '0') {
        q++;
    } else if (q < n && s[q] >= '1' && s[q] <= '9') {
        while (q < n && isdigit((unsigned char)s[q])) q++;
    } else {
        fail(j->p, "bad JSON body: invalid number");
        return NULL;
    }
    int is_float = 0;
    if (q < n && s[q] == '.') {
        is_float = 1;
        q++;
        if (q >= n || !isdigit((unsigned char)s[q])) {
            fail(j->p, "bad JSON body: invalid number");
            return NULL;
        }
        while (q < n && isdigit((unsigned char)s[q])) q++;
    }
    if (q < n && (s[q] == 'e' || s[q] == 'E')) {
        is_float = 1;
        q++;
        if (q < n && (s[q] == '+' || s[q] == '-')) q++;
        if (q >= n || !isdigit((unsigned char)s[q])) {
            fail(j->p, "bad JSON body: invalid number");
            return NULL;
        }
        while (q < n && isdigit((unsigned char)s[q])) q++;
    }
    char *lit = xstrndup(s + start, q - start);
    j->pos = q;
    DogValue *v = is_float ? v_float(strtod(lit, NULL)) : v_int_text(normalize_int(lit));
    free(lit);
    return v;
}

static int j_expect_lit(JP *j, const char *lit) {
    size_t L = strlen(lit);
    if (j->n - j->pos >= L && memcmp(j->s + j->pos, lit, L) == 0) {
        j->pos += L;
        return 1;
    }
    return 0;
}

static DogValue *j_array(JP *j) {
    j->pos++; /* consume '[' */
    DogValue *a = v_arr();
    j_skip(j);
    if (j->pos < j->n && j->s[j->pos] == ']') { j->pos++; return a; }
    for (;;) {
        j_skip(j);
        DogValue *v = j_value(j);
        if (j->p->msg) { dog_free(v); dog_free(a); return NULL; }
        arr_push(a, v);
        j_skip(j);
        if (j->pos >= j->n) {
            fail(j->p, "bad JSON body: unterminated array");
            dog_free(a);
            return NULL;
        }
        if (j->s[j->pos] == ',') { j->pos++; continue; }
        if (j->s[j->pos] == ']') { j->pos++; break; }
        fail(j->p, "bad JSON body: expected ',' or ']' in array");
        dog_free(a);
        return NULL;
    }
    return a;
}

static DogValue *j_object(JP *j) {
    j->pos++; /* consume '{' */
    DogValue *o = v_map();
    j_skip(j);
    if (j->pos < j->n && j->s[j->pos] == '}') { j->pos++; return o; }
    for (;;) {
        j_skip(j);
        if (j->pos >= j->n || j->s[j->pos] != '"') {
            fail(j->p, "bad JSON body: expected string key in object");
            dog_free(o);
            return NULL;
        }
        DogValue *k = j_string(j);
        if (j->p->msg) { dog_free(k); dog_free(o); return NULL; }
        j_skip(j);
        if (j->pos >= j->n || j->s[j->pos] != ':') {
            fail(j->p, "bad JSON body: expected ':' in object");
            dog_free(k); dog_free(o);
            return NULL;
        }
        j->pos++;
        DogValue *v = j_value(j);
        if (j->p->msg) { dog_free(k); dog_free(v); dog_free(o); return NULL; }
        map_set(o, k->u.str, v);
        dog_free(k);
        j_skip(j);
        if (j->pos >= j->n) {
            fail(j->p, "bad JSON body: unterminated object");
            dog_free(o);
            return NULL;
        }
        if (j->s[j->pos] == ',') { j->pos++; continue; }
        if (j->s[j->pos] == '}') { j->pos++; break; }
        fail(j->p, "bad JSON body: expected ',' or '}' in object");
        dog_free(o);
        return NULL;
    }
    return o;
}

static DogValue *j_value(JP *j) {
    j_skip(j);
    if (j->pos >= j->n) {
        fail(j->p, "bad JSON body: unexpected end of input");
        return NULL;
    }
    char c = j->s[j->pos];
    switch (c) {
    case '{': return j_object(j);
    case '[': return j_array(j);
    case '"': return j_string(j);
    case 't':
        if (j_expect_lit(j, "true")) return v_bool(1);
        break;
    case 'f':
        if (j_expect_lit(j, "false")) return v_bool(0);
        break;
    case 'n':
        if (j_expect_lit(j, "null")) return v_null();
        break;
    case '-':
        return j_number(j);
    default:
        if (isdigit((unsigned char)c)) return j_number(j);
        break;
    }
    fail(j->p, "bad JSON body: unexpected character '%c'", c);
    return NULL;
}

static DogValue *parse_json_body(P *p, char **body, size_t nbody) {
    Str t;
    str_init(&t);
    for (size_t i = 0; i < nbody; i++) {
        if (i) str_pushc(&t, '\n');
        str_pushz(&t, body[i]);
    }
    char *text = trim(t.data ? t.data : "");
    str_free(&t);
    DogValue *v;
    if (text[0] == '\0') {
        v = v_null();
    } else {
        JP j;
        j.p = p; j.s = text; j.n = strlen(text); j.pos = 0;
        v = j_value(&j);
        if (!p->msg) {
            j_skip(&j);
            if (j.pos != j.n) {
                fail(p, "bad JSON body: trailing data after top-level value");
                dog_free(v);
                v = NULL;
            }
        } else {
            dog_free(v);
            v = NULL;
        }
    }
    free(text);
    return v;
}

/* ------------------------------------------------------------------ */
/* xml body -- minimal parser: elements, attributes, text, comments,   */
/* PIs, DOCTYPE skipping, and the five predefined + numeric entities.  */
/* Enough for the v1 mapping (attributes -> "@name", repeated siblings */
/* -> arrays, element text -> "#text" when attributes/children exist). */
/* ------------------------------------------------------------------ */

typedef struct XNode XNode;
struct XNode {
    char *tag;
    StrMap attrs; /* name -> value (entities already unescaped) */
    XNode **children;
    size_t clen, ccap;
    char *text; /* leading text before the first child (raw, entities intact) */
};

typedef struct { P *p; const char *s; size_t n, pos; } XP;

static XNode *xnode_new(char *tag) {
    XNode *nd = xmalloc(sizeof *nd);
    nd->tag = tag;
    sm_init(&nd->attrs);
    nd->children = NULL; nd->clen = nd->ccap = 0;
    nd->text = NULL;
    return nd;
}

static void xnode_add_child(XNode *nd, XNode *ch) {
    if (nd->clen == nd->ccap) {
        nd->ccap = nd->ccap ? nd->ccap * 2 : 4;
        nd->children = xrealloc(nd->children, nd->ccap * sizeof *nd->children);
    }
    nd->children[nd->clen++] = ch;
}

static void xnode_free(XNode *nd) {
    if (!nd) return;
    free(nd->tag);
    sm_free(&nd->attrs);
    for (size_t i = 0; i < nd->clen; i++) xnode_free(nd->children[i]);
    free(nd->children);
    free(nd->text);
    free(nd);
}

static int x_starts(XP *x, const char *lit) {
    size_t L = strlen(lit);
    return x->n - x->pos >= L && memcmp(x->s + x->pos, lit, L) == 0;
}

static void x_skip_ws(XP *x) {
    while (x->pos < x->n && isspace((unsigned char)x->s[x->pos])) x->pos++;
}

/* comments, PIs, DOCTYPE/declarations before or after the root */
static void x_skip_misc(XP *x) {
    for (;;) {
        if (x_starts(x, "<!--")) {
            const char *e = strstr(x->s + x->pos + 4, "-->");
            if (!e) { fail(x->p, "bad XML body: unterminated comment"); return; }
            x->pos = (size_t)(e - x->s) + 3;
        } else if (x_starts(x, "<?")) {
            const char *e = strstr(x->s + x->pos + 2, "?>");
            if (!e) { fail(x->p, "bad XML body: unterminated processing instruction"); return; }
            x->pos = (size_t)(e - x->s) + 2;
        } else if (x_starts(x, "<!")) {
            size_t d = x->pos + 2;
            int depth = 0;
            while (d < x->n) {
                if (x->s[d] == '>' && depth == 0) break;
                if (x->s[d] == '[') depth++;
                if (x->s[d] == ']') depth--;
                d++;
            }
            if (d >= x->n) { fail(x->p, "bad XML body: unterminated declaration"); return; }
            x->pos = d + 1;
        } else {
            return;
        }
    }
}

static char *x_read_name(XP *x) {
    size_t start = x->pos;
    while (x->pos < x->n) {
        char c = x->s[x->pos];
        if (isspace((unsigned char)c) || c == '/' || c == '>' || c == '=') break;
        x->pos++;
    }
    if (x->pos == start) {
        fail(x->p, "bad XML body: expected a name at offset %lu", (unsigned long)x->pos);
        return NULL;
    }
    return xstrndup(x->s + start, x->pos - start);
}

static char *x_unescape(XP *x, const char *s) {
    Str out;
    str_init(&out);
    const char *q = s;
    while (*q) {
        if (*q == '&') {
            const char *semi = strchr(q, ';');
            if (!semi) {
                fail(x->p, "bad XML body: unterminated entity");
                str_free(&out);
                return NULL;
            }
            char *ent = xstrndup(q + 1, (size_t)(semi - q - 1));
            char *rep = NULL;
            if (strcmp(ent, "amp") == 0) rep = xstrdup("&");
            else if (strcmp(ent, "lt") == 0) rep = xstrdup("<");
            else if (strcmp(ent, "gt") == 0) rep = xstrdup(">");
            else if (strcmp(ent, "quot") == 0) rep = xstrdup("\"");
            else if (strcmp(ent, "apos") == 0) rep = xstrdup("'");
            else if (ent[0] == '#') {
                char *end = NULL;
                unsigned long cp;
                if ((ent[1] == 'x' || ent[1] == 'X') && ent[2]) {
                    cp = strtoul(ent + 2, &end, 16);
                    if (!end || *end) cp = 0x110000; /* force the failure below */
                } else if (ent[1] && ent[1] != 'x' && ent[1] != 'X') {
                    cp = strtoul(ent + 1, &end, 10);
                    if (!end || *end) cp = 0x110000;
                } else {
                    cp = 0x110000;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    fail(x->p, "bad XML body: bad character reference &%s;", ent);
                } else {
                    Str tmp;
                    str_init(&tmp);
                    utf8_encode(&tmp, (unsigned int)cp);
                    rep = str_take(&tmp);
                }
            } else {
                fail(x->p, "bad XML body: unknown entity &%s;", ent);
            }
            free(ent);
            if (x->p->msg) {
                free(rep);
                str_free(&out);
                return NULL;
            }
            str_pushz(&out, rep);
            free(rep);
            q = semi + 1;
        } else {
            str_pushc(&out, *q);
            q++;
        }
    }
    return str_take(&out);
}

/* skip one comment or PI inside element content; 1 = skipped, 0 = not one */
static int x_skip_comment_pi(XP *x) {
    if (x_starts(x, "<!--")) {
        const char *e = strstr(x->s + x->pos + 4, "-->");
        if (!e) { fail(x->p, "bad XML body: unterminated comment"); return -1; }
        x->pos = (size_t)(e - x->s) + 3;
        return 1;
    }
    if (x_starts(x, "<?")) {
        const char *e = strstr(x->s + x->pos + 2, "?>");
        if (!e) { fail(x->p, "bad XML body: unterminated processing instruction"); return -1; }
        x->pos = (size_t)(e - x->s) + 2;
        return 1;
    }
    return 0;
}

static XNode *x_parse_element(XP *x) {
    x->pos++; /* consume '<' */
    char *name = x_read_name(x);
    if (x->p->msg) return NULL;
    XNode *nd = xnode_new(name);
    for (;;) {
        x_skip_ws(x);
        if (x_starts(x, "/>")) {
            x->pos += 2;
            nd->text = xstrdup("");
            return nd;
        }
        if (x->pos < x->n && x->s[x->pos] == '>') { x->pos++; break; }
        if (x->pos >= x->n) {
            fail(x->p, "bad XML body: unterminated start tag <%s>", name);
            xnode_free(nd);
            return NULL;
        }
        char *an = x_read_name(x);
        if (x->p->msg) { xnode_free(nd); return NULL; }
        x_skip_ws(x);
        if (x->pos >= x->n || x->s[x->pos] != '=') {
            fail(x->p, "bad XML body: expected '=' after attribute %s", an);
            free(an); xnode_free(nd);
            return NULL;
        }
        x->pos++;
        x_skip_ws(x);
        char qc = x->pos < x->n ? x->s[x->pos] : '\0';
        if (qc != '"' && qc != '\'') {
            fail(x->p, "bad XML body: attribute value must be quoted");
            free(an); xnode_free(nd);
            return NULL;
        }
        x->pos++;
        size_t vs = x->pos;
        while (x->pos < x->n && x->s[x->pos] != qc) x->pos++;
        if (x->pos >= x->n) {
            fail(x->p, "bad XML body: unterminated attribute value");
            free(an); xnode_free(nd);
            return NULL;
        }
        char *rawv = xstrndup(x->s + vs, x->pos - vs);
        x->pos++;
        char *uv = x_unescape(x, rawv);
        free(rawv);
        if (x->p->msg) { free(an); xnode_free(nd); return NULL; }
        sm_set(&nd->attrs, an, uv);
        free(an);
        free(uv);
    }
    Str tacc;
    str_init(&tacc);
    int seen_child = 0;
    for (;;) {
        if (x->pos >= x->n) {
            fail(x->p, "bad XML body: unterminated element <%s>", name);
            str_free(&tacc); xnode_free(nd);
            return NULL;
        }
        if (x->s[x->pos] == '<') {
            if (x_starts(x, "</")) {
                x->pos += 2;
                x_skip_ws(x);
                char *cn = x_read_name(x);
                if (x->p->msg) { str_free(&tacc); xnode_free(nd); return NULL; }
                x_skip_ws(x);
                if (x->pos >= x->n || x->s[x->pos] != '>') {
                    fail(x->p, "bad XML body: expected '>' in closing tag");
                    free(cn); str_free(&tacc); xnode_free(nd);
                    return NULL;
                }
                x->pos++;
                if (strcmp(cn, name) != 0) {
                    fail(x->p, "bad XML body: mismatched close tag </%s> for <%s>", cn, name);
                    free(cn); str_free(&tacc); xnode_free(nd);
                    return NULL;
                }
                free(cn);
                break;
            }
            int skipped = x_skip_comment_pi(x);
            if (skipped != 0) {
                if (skipped < 0) { str_free(&tacc); xnode_free(nd); return NULL; }
                continue;
            }
            if (x_starts(x, "<![CDATA[")) {
                const char *e = strstr(x->s + x->pos + 9, "]]>");
                if (!e) {
                    fail(x->p, "bad XML body: unterminated CDATA section");
                    str_free(&tacc); xnode_free(nd);
                    return NULL;
                }
                if (!seen_child)
                    str_pushn(&tacc, x->s + x->pos + 9, (size_t)(e - (x->s + x->pos + 9)));
                x->pos = (size_t)(e - x->s) + 3;
            } else if (x_starts(x, "<!")) {
                fail(x->p, "bad XML body: unexpected declaration inside element");
                str_free(&tacc); xnode_free(nd);
                return NULL;
            } else {
                XNode *ch = x_parse_element(x);
                if (x->p->msg) { str_free(&tacc); xnode_free(nd); return NULL; }
                xnode_add_child(nd, ch);
                seen_child = 1;
            }
        } else {
            const char *lt = memchr(x->s + x->pos, '<', x->n - x->pos);
            size_t te = lt ? (size_t)(lt - x->s) : x->n;
            if (!seen_child) str_pushn(&tacc, x->s + x->pos, te - x->pos);
            x->pos = te;
        }
    }
    nd->text = str_take(&tacc);
    return nd;
}

static DogValue *x_value(XP *x, XNode *nd, P *p);

static DogValue *x_value(XP *x, XNode *nd, P *p) {
    DogValue *d = v_map();
    for (size_t a = 0; a < nd->attrs.len; a++) {
        char *k = xmalloc(strlen(nd->attrs.names[a]) + 2);
        k[0] = '@';
        strcpy(k + 1, nd->attrs.names[a]);
        map_set(d, k, v_str(nd->attrs.vals[a]));
        free(k);
    }
    if (nd->clen == 0) {
        char *ue = x_unescape(x, nd->text ? nd->text : "");
        if (p->msg) { dog_free(d); return NULL; }
        char *t = trim(ue);
        free(ue);
        DogValue *r;
        if (nd->attrs.len > 0) {
            if (t[0]) map_set(d, "#text", v_str(t));
            r = d;
        } else {
            r = coerce_loose(t);
            dog_free(d);
        }
        free(t);
        return r;
    }
    for (size_t c = 0; c < nd->clen; c++) {
        XNode *ch = nd->children[c];
        DogValue *v = x_value(x, ch, p);
        if (p->msg) { dog_free(v); dog_free(d); return NULL; }
        DogValue *cur = map_get(d, ch->tag);
        if (!cur) {
            map_set(d, ch->tag, v);
        } else if (cur->type == V_ARR) {
            arr_push(cur, v);
        } else {
            DogValue *old = map_take(d, ch->tag);
            DogValue *arr = v_arr();
            arr_push(arr, old);
            arr_push(arr, v);
            map_set(d, ch->tag, arr);
        }
    }
    return d;
}

static DogValue *parse_xml_body(P *p, char **body, size_t nbody) {
    Str t;
    str_init(&t);
    for (size_t i = 0; i < nbody; i++) {
        if (i) str_pushc(&t, '\n');
        str_pushz(&t, body[i]);
    }
    char *text = trim(t.data ? t.data : "");
    str_free(&t);
    if (text[0] == '\0') { free(text); return v_null(); }
    XP x;
    x.p = p; x.s = text; x.n = strlen(text); x.pos = 0;
    x_skip_ws(&x);
    x_skip_misc(&x);
    if (p->msg) { free(text); return NULL; }
    x_skip_ws(&x);
    if (x.pos >= x.n || x.s[x.pos] != '<') {
        fail(p, "bad XML body: no root element");
        free(text);
        return NULL;
    }
    XNode *root = x_parse_element(&x);
    if (p->msg) { free(text); return NULL; }
    x_skip_ws(&x);
    x_skip_misc(&x);
    if (p->msg) { xnode_free(root); free(text); return NULL; }
    x_skip_ws(&x);
    if (x.pos != x.n) {
        fail(p, "bad XML body: junk after root element");
        xnode_free(root); free(text);
        return NULL;
    }
    DogValue *w = v_map();
    DogValue *rv = x_value(&x, root, p);
    if (p->msg) { dog_free(rv); dog_free(w); xnode_free(root); free(text); return NULL; }
    map_set(w, root->tag, rv);
    xnode_free(root);
    free(text);
    return w;
}

/* ------------------------------------------------------------------ */
/* top-level                                                           */
/* ------------------------------------------------------------------ */

DogValue *dog_parse(const char *text, char **err) {
    P p;
    p.msg = NULL;
    StrMap dirs;
    sm_init(&dirs);
    char **body = NULL;
    size_t nbody = 0;
    parse_header(&p, text, &dirs, &body, &nbody);
    Config cfg;
    memset(&cfg, 0, sizeof cfg);
    DogValue *v = NULL;
    if (!p.msg) build_config(&p, &dirs, &cfg);
    if (!p.msg) {
        if (strcmp(cfg.lang, "json") == 0) {
            v = parse_json_body(&p, body, nbody);
        } else if (strcmp(cfg.lang, "xml") == 0) {
            v = parse_xml_body(&p, body, nbody);
        } else if (strcmp(cfg.lang, "yaml") == 0) {
            /* header tools are native-dog features; YAML bodies stay byte-identical */
            Config ycfg = cfg;
            ycfg.dict.len = 0;
            ycfg.rep_len = 0;
            ycfg.keys = NULL;
            ycfg.keys_len = 0;
            v = parse_dog_body(&p, body, nbody, &ycfg);
        } else {
            v = parse_dog_body(&p, body, nbody, &cfg);
        }
    }
    for (size_t i = 0; i < nbody; i++) free(body[i]);
    free(body);
    free_config(&cfg);
    sm_free(&dirs);
    if (p.msg) {
        dog_free(v);
        if (err) *err = p.msg;
        else free(p.msg);
        return NULL;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* JSON output (pretty-printed like dog.py: indent=2, ensure_ascii=F)  */
/* ------------------------------------------------------------------ */

static void json_pad(Str *out, int level) {
    for (int i = 0; i < level * 2; i++) str_pushc(out, ' ');
}

static void json_escape(Str *out, const char *s) {
    str_pushc(out, '"');
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) {
        switch (*q) {
        case '"': str_pushz(out, "\\\""); break;
        case '\\': str_pushz(out, "\\\\"); break;
        case '\b': str_pushz(out, "\\b"); break;
        case '\f': str_pushz(out, "\\f"); break;
        case '\n': str_pushz(out, "\\n"); break;
        case '\r': str_pushz(out, "\\r"); break;
        case '\t': str_pushz(out, "\\t"); break;
        default:
            if (*q < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", *q);
                str_pushz(out, buf);
            } else {
                str_pushc(out, (char)*q);
            }
            break;
        }
    }
    str_pushc(out, '"');
}

static void json_write(Str *out, DogValue *v, int level) {
    switch (v->type) {
    case V_NULL: str_pushz(out, "null"); break;
    case V_BOOL: str_pushz(out, v->u.boolean ? "true" : "false"); break;
    case V_INT: str_pushz(out, v->u.integer); break;
    case V_FLOAT: {
        char buf[96];
        fmt_double(buf, v->u.number);
        str_pushz(out, buf);
        break;
    }
    case V_STR: json_escape(out, v->u.str); break;
    case V_ARR: {
        if (v->u.arr.len == 0) { str_pushz(out, "[]"); break; }
        str_pushz(out, "[\n");
        for (size_t i = 0; i < v->u.arr.len; i++) {
            json_pad(out, level + 1);
            json_write(out, v->u.arr.items[i], level + 1);
            if (i + 1 < v->u.arr.len) str_pushc(out, ',');
            str_pushc(out, '\n');
        }
        json_pad(out, level);
        str_pushc(out, ']');
        break;
    }
    case V_MAP: {
        if (v->u.map.len == 0) { str_pushz(out, "{}"); break; }
        str_pushz(out, "{\n");
        for (size_t i = 0; i < v->u.map.len; i++) {
            json_pad(out, level + 1);
            json_escape(out, v->u.map.ents[i].key);
            str_pushz(out, ": ");
            json_write(out, v->u.map.ents[i].val, level + 1);
            if (i + 1 < v->u.map.len) str_pushc(out, ',');
            str_pushc(out, '\n');
        }
        json_pad(out, level);
        str_pushc(out, '}');
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Str s;
    str_init(&s);
    char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) str_pushn(&s, buf, r);
    fclose(f);
    return str_take(&s);
}

int main(int argc, char **argv) {
    setlocale(LC_NUMERIC, "C"); /* strtod/snprintf always use '.' */
    if (argc != 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr, "usage: dog file.dog\n");
        return 2;
    }
    char *text = read_file(argv[1]);
    if (!text) {
        fprintf(stderr, "dog: cannot read %s: %s\n", argv[1], strerror(errno));
        return 2;
    }
    char *err = NULL;
    DogValue *v = dog_parse(text, &err);
    free(text);
    if (!v) {
        fprintf(stderr, "dog error: %s\n", err ? err : "parse failed");
        free(err);
        return 1;
    }
    Str out;
    str_init(&out);
    json_write(&out, v, 0);
    str_pushc(&out, '\n');
    fwrite(out.data, 1, out.len, stdout);
    str_free(&out);
    dog_free(v);
    return 0;
}
