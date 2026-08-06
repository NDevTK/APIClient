/* THE URL RECORD AND THE BASIC URL PARSER — WHATWG URL §4. See url.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_URL_URL_H
#define ENGINE_HOST_BROWSER_CORE_URL_URL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"

/* §4.2's HOST: not a string. A host is a domain, an IPv4 address, an IPv6 address, an opaque host or the empty
   host, and the difference is observable — `http://0x7f.1/` serializes as `http://127.0.0.1/` because the
   parser produced a NUMBER, and `http://[::1]/` re-serializes with the brackets and the compression the IPv6
   serializer applies. A char* host would have to re-derive that at every read. */
typedef enum {
    URL_HOST_NULL = 0,   /* the host is not present at all (an opaque-path URL, or a bare `file:`) */
    URL_HOST_DOMAIN,     /* an ASCII domain, already lowercased by domain-to-ASCII */
    URL_HOST_IPV4,
    URL_HOST_IPV6,
    URL_HOST_OPAQUE,     /* a non-special scheme's host: kept as written, percent-encoded, never resolved */
    URL_HOST_EMPTY,      /* the empty host, which `file:///x` has and which is NOT the same as null */
} UrlHostKind;

typedef struct {
    UrlHostKind kind;
    char       *domain;      /* URL_HOST_DOMAIN / URL_HOST_OPAQUE (owned) */
    uint32_t    ipv4;
    uint16_t    ipv6[8];
} UrlHost;

/* §4.1's URL record. `port` is -1 for the spec's null port; `query` and `fragment` are NULL for the spec's
   null, which is not the empty string — `http://x/` and `http://x/?` differ in exactly that. */
typedef struct {
    char    *scheme;         /* lowercased, without the ":" */
    char    *username;
    char    *password;
    UrlHost  host;
    int      port;
    char   **path;           /* the path's segments; `npath` of them */
    int      npath, cpath;
    char    *opaque_path;    /* §4.1: a URL has EITHER a list of segments OR one opaque string, never both */
    char    *query;
    char    *fragment;
} UrlRecord;

void url_record_init(UrlRecord *u);
void url_record_free(UrlRecord *u);
/* A deep copy — the parser needs one because `new URL(input, base)` must not write through to `base`. */
bool url_record_copy(UrlRecord *dst, const UrlRecord *src);

/* §4.4 "basic URL parser". `base` may be NULL. Returns false for the spec's FAILURE, which is what makes the
   URL constructor throw. `out` is left initialised-and-empty on failure, so the caller frees it either way. */
bool url_parse(UrlRecord *out, const char *input, size_t len, const UrlRecord *base);

/* §4.4 with a STATE OVERRIDE — what every §5.1 setter is one call to. The override values are the parser's own
   state numbering and are private to url.c, which is where the setters live; -1 is a fresh parse and is what
   url_parse passes. On failure with an override the record is left UNTOUCHED, which is what §5.1's "return"
   means — a setter that refuses its input must not also destroy the URL. */
bool url_parse_into(UrlRecord *url, const char *input, size_t len, const UrlRecord *base, int state_override);

/* §4.5 "URL serializer" — the whole record, or every part of it. Each returns a malloc'd string. */
char *url_serialize(const UrlRecord *u, bool exclude_fragment);
char *url_serialize_host(const UrlHost *h);
/* `host` (the host with its port when there is one) and `port` (empty when null) as both URL and Location read
   them — one serialization, because the two interfaces state the same one. */
char *url_serialize_host_port(const UrlRecord *u);
char *url_serialize_port(const UrlRecord *u);
/* §4.7's `origin`: the tuple origin for a scheme that has one, and the string "null" for every other. */
char *url_serialize_origin(const UrlRecord *u);
/* The path, as `pathname` reports it: the opaque path, or "/" joined segments. */
char *url_serialize_path(const UrlRecord *u);

/* §4.2's special schemes and their default ports — the whole of what "special" means. -1 when the scheme has
   no default port (which is `file`, the one special scheme with none). */
bool url_scheme_is_special(const char *scheme);
int  url_default_port(const char *scheme);

/* §5.1's URLENCODED SERIALIZER's encode set, exported because URLSearchParams is the other user of it. */
char *url_percent_encode(const char *s, size_t len, int set);
/* §1.3 "percent-decode". `*out_n` is the decoded LENGTH, which is not strlen when the input decodes a %00. */
char *url_percent_decode(const char *s, size_t len, size_t *out_n);

/* The RECORD behind a `URL` wrapper, or NULL when `v` is not one — how URLSearchParams writes §6.1's update
   steps back onto the URL it belongs to. */
UrlRecord *url_record_of(JSValueConst v);
enum { URL_SET_C0 = 0, URL_SET_FRAGMENT, URL_SET_QUERY, URL_SET_SPECIAL_QUERY, URL_SET_PATH,
       URL_SET_USERINFO, URL_SET_COMPONENT, URL_SET_URLENCODED };

/* §5.1's `application/x-www-form-urlencoded` LIST — the URL Standard's, not URLSearchParams'. That
   interface is one view over it and `.formData()` is the other, so it lives with the spec that defines it.
   A pair carries its LENGTHS because a name or a value may contain U+0000: `?a=b%00c` is one pair whose value
   is three characters, and a strlen would make it one. */
typedef struct { char *name, *value; size_t nlen, vlen; } UrlEncodedPair;
typedef struct { UrlEncodedPair *e; int n, cap; } UrlEncodedList;

void  url_encoded_list_free(UrlEncodedList *l);
void  url_encoded_list_append(UrlEncodedList *l, const char *name, size_t nn, const char *value, size_t vn);
/* §5.1's PARSER: split on `&`, split each sequence at its FIRST `=`, turn `+` into a space in BOTH halves and
   only then percent-decode — the other order would decode a `%2B` into a `+` and then into a space. */
void  url_encoded_parse(UrlEncodedList *out, const char *s, size_t len);
/* §5.1's SERIALIZER: `name=value` joined by `&`, each half through the urlencoded encode set. */
char *url_encoded_serialize(const UrlEncodedList *l, size_t *out_n);
/* §6.2's ordering, which is by UTF-16 CODE UNITS and not by UTF-8 bytes. */
int   url_encoded_name_cmp(const UrlEncodedPair *a, const UrlEncodedPair *b);
char *url_encoded_strdup(const char *s, size_t n);

/* The `URL` interface — §5. */
void url_init(JSContext *ctx);
void url_install(JSContext *ctx, JSValueConst global);
void url_free(JSContext *ctx);

#endif
