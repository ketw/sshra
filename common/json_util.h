/*
 * json_util.h - Minimal zero-dependency JSON builder/parser
 * We keep it simple: build JSON by hand (safe, fast, no malloc for small msgs)
 * Parsing uses a tiny state-machine finder (no full AST needed)
 */

#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* ---- Builder ---- */

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
    int     first_field;   /* tracks comma insertion */
} json_builder_t;

static inline void jb_init(json_builder_t *b, char *buf, size_t cap) {
    b->buf = buf; b->cap = cap; b->len = 0; b->first_field = 1;
    b->buf[0] = '\0';
}

static inline void jb_append(json_builder_t *b, const char *s) {
    size_t n = strlen(s);
    if (b->len + n + 1 >= b->cap) return;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static inline void jb_begin(json_builder_t *b) {
    jb_append(b, "{"); b->first_field = 1;
}
static inline void jb_end(json_builder_t *b) { jb_append(b, "}"); }

static inline void jb_comma(json_builder_t *b) {
    if (!b->first_field) jb_append(b, ",");
    b->first_field = 0;
}

static inline void jb_str(json_builder_t *b, const char *key, const char *val) {
    jb_comma(b);
    char tmp[1024];
    /* Simple escape: replace " and \ */
    char esc[512]; int ei = 0;
    for (const char *p = val; *p && ei < 510; p++) {
        if (*p == '"' || *p == '\\') esc[ei++] = '\\';
        esc[ei++] = *p;
    }
    esc[ei] = '\0';
    snprintf(tmp, sizeof(tmp), "\"%s\":\"%s\"", key, esc);
    jb_append(b, tmp);
}

static inline void jb_int(json_builder_t *b, const char *key, long long val) {
    jb_comma(b);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "\"%s\":%lld", key, val);
    jb_append(b, tmp);
}

static inline void jb_float(json_builder_t *b, const char *key, double val) {
    jb_comma(b);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "\"%s\":%.2f", key, val);
    jb_append(b, tmp);
}

static inline void jb_uint64(json_builder_t *b, const char *key, uint64_t val) {
    jb_comma(b);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "\"%s\":%llu", key, (unsigned long long)val);
    jb_append(b, tmp);
}

/* ---- Parser: find value of a string key ---- */
/* Returns pointer into src at start of value string (without quotes), writes len */
/* Returns NULL if not found. Only handles flat string/number values. */

static inline const char *json_find_str(const char *src, const char *key,
                                         char *out, size_t out_cap) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(src, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') {
        p++; /* skip opening quote */
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < out_cap) {
            if (*p == '\\' && *(p+1)) { p++; } /* unescape */
            out[i++] = *p++;
        }
        out[i] = '\0';
        return out;
    }
    /* number or bare value */
    size_t i = 0;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && i+1 < out_cap)
        out[i++] = *p++;
    out[i] = '\0';
    return out;
}

static inline long long json_find_int(const char *src, const char *key, long long def) {
    char tmp[64];
    if (!json_find_str(src, key, tmp, sizeof(tmp))) return def;
    return (long long)atoll(tmp);
}

#endif /* JSON_UTIL_H */
