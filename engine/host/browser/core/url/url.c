/* THE WHATWG URL PARSER — the URL Standard's §4, as the state machine it is written as.
 *
 * WHY IT EXISTS. Three things need a URL and none of them can have one without this: `Response.redirect`
 * parses its argument and throws on failure, `new Request(input)` parses a relative string against the
 * document's base, and `location` had a HAND-ROLLED SPLITTER — strstr("://"), strrchr(':'), fixed 256-byte
 * buffers — which is an approximation of a specified algorithm and is wrong in every way the spec is
 * interesting: `http://0x7f.1/` is 127.0.0.1, `http://x/a/../b` is `/b`, `http:/example.com/` has a host,
 * `/a` against a base is not a scheme, and a URL with no `://` at all is not a crash.
 *
 * IT IS THE SPEC'S STATE MACHINE, not a tidier equivalent. The states are named as §4.4 names them and the
 * pointer moves as §4.4 moves it, because the algorithm's edges ARE the behaviour: the difference between
 * "special authority slashes" and "path or authority" is what makes `http:/x` and `nonspecial:/x` disagree,
 * and a parser written from the shape of a URL rather than from the algorithm gets that wrong quietly.
 *
 * VALIDATION ERRORS ARE NOT FAILURES. The spec reports many inputs as "validation error" and keeps parsing;
 * only an explicit "return failure" is a failure. Treating the first as the second is the single most common
 * way a URL parser is wrong, so this file never conflates them — a validation error is not recorded at all,
 * because nothing in the platform surfaces it.
 *
 * §4.2's DOMAIN-TO-ASCII IS ITS OWN COMPONENT, core/url/idna.c, which this file calls. It is Unicode's UTS-46
 * table plus RFC 3492's Punycode — one assertable contract with nothing of the state machine in it — and this
 * header used to say instead that it did not exist and that a non-ASCII domain reached a DFAIL naming it. That
 * sentence outlived the DFAIL: idna.c's own header records that this file's crash is why it was written. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/url/url.h"
#include "core/url/origin.h"   /* §4.7's origin is a RECORD; this file only ever serializes one */
#include "core/encoding/encoding.h"   /* §6's "UTF-8 decode without BOM", which §5.1 runs on a percent-decoding */
#include "solver/concolic.h"   /* §6.1 over unknown input answers at the operator, not at the coercion */
#include "core/file/blob.h"
#include "core/url/idna.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/url/url_search_params.h"

/* ---- a growable byte string --------------------------------------------------------------------------- */

typedef struct { char *p; size_t n, cap; } UStr;

static void ustr_init(UStr *s) { s->p = NULL; s->n = 0; s->cap = 0; }

static void ustr_reserve(UStr *s, size_t extra)
{
    if (s->n + extra + 1 <= s->cap) return;
    s->cap = s->cap ? s->cap * 2 : 32;
    while (s->n + extra + 1 > s->cap) s->cap *= 2;
    s->p = realloc(s->p, s->cap);
    CHECK(s->p, "url: OOM building a URL string");
}

static void ustr_putc(UStr *s, char c) { ustr_reserve(s, 1); s->p[s->n++] = c; s->p[s->n] = 0; }

static void ustr_put(UStr *s, const char *b, size_t n)
{
    if (!n) { ustr_reserve(s, 1); s->p[s->n] = 0; return; }
    ustr_reserve(s, n);
    memcpy(s->p + s->n, b, n);
    s->n += n;
    s->p[s->n] = 0;
}

static void ustr_puts(UStr *s, const char *b) { ustr_put(s, b, strlen(b)); }

/* Hand the buffer over; the UStr owns nothing afterwards. Never NULL — an empty string is a string. */
static char *ustr_take(UStr *s)
{
    char *r = s->p;
    if (!r) { r = malloc(1); CHECK(r, "url: OOM"); r[0] = 0; }
    ustr_init(s);
    return r;
}

static void ustr_free(UStr *s) { free(s->p); ustr_init(s); }

static char *xstrdup(const char *s)
{
    char *r = strdup(s ? s : "");
    CHECK(r, "url: OOM copying a URL part");
    return r;
}

/* ---- character classes the spec names ------------------------------------------------------------------ */

static int is_ascii_digit(int c)      { return c >= '0' && c <= '9'; }
static int is_ascii_hex(int c)        { return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int is_ascii_alpha(int c)      { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_ascii_alnum(int c)      { return is_ascii_alpha(c) || is_ascii_digit(c); }
static int lower(int c)               { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
static int hexval(int c)              { return is_ascii_digit(c) ? c - '0' : lower(c) - 'a' + 10; }

/* §4.2 "forbidden host code point" and "forbidden domain code point". The second is the first plus the C0
   controls, DEL and `%` — a domain may not carry a percent-encoding, an opaque host may. */
static int is_forbidden_host(int c)
{
    return c == 0x00 || c == 0x09 || c == 0x0a || c == 0x0d || c == ' ' || c == '#' || c == '/' ||
           c == ':' || c == '<' || c == '>' || c == '?' || c == '@' || c == '[' || c == '\\' ||
           c == ']' || c == '^' || c == '|';
}

static int is_forbidden_domain(int c)
{
    return is_forbidden_host(c) || (unsigned char)c <= 0x1f || c == '%' || c == 0x7f;
}

/* §1.3's "single-dot path segment" / "double-dot path segment", which are case-insensitive and accept the
   percent-encoded spellings — `/%2e%2E/` pops a segment exactly as `/../` does. */
static int seg_is_dot(const char *s)
{
    return !strcmp(s, ".") || !strcasecmp(s, "%2e");
}

static int seg_is_dotdot(const char *s)
{
    return !strcmp(s, "..") || !strcasecmp(s, ".%2e") || !strcasecmp(s, "%2e.") || !strcasecmp(s, "%2e%2e");
}

/* §4.2's special schemes. The list IS the definition — there is no property of a scheme that makes it
   special, so a test against a table is the algorithm and not a shortcut. */
static const struct { const char *scheme; int port; } SPECIAL[] = {
    { "ftp", 21 }, { "file", -1 }, { "http", 80 }, { "https", 443 }, { "ws", 80 }, { "wss", 443 },
};

bool url_scheme_is_special(const char *scheme)
{
    size_t i;
    if (!scheme) return false;
    for (i = 0; i < sizeof(SPECIAL) / sizeof(SPECIAL[0]); i++)
        if (!strcmp(scheme, SPECIAL[i].scheme)) return true;
    return false;
}

/* FETCH §2.1 "URL": "A local scheme is `about`, `blob`, or `data`." The list IS the definition here for the
   same reason SPECIAL's is — there is no property of a scheme that makes it local — and it is a SEPARATE table
   rather than a column of SPECIAL because the two sets are disjoint and a shared row would invite a reader to
   look for a relation between them that the two standards do not state. */
static const char *const LOCAL[] = { "about", "blob", "data" };

bool url_scheme_is_local(const char *scheme)
{
    size_t i;
    if (!scheme) return false;
    for (i = 0; i < sizeof(LOCAL) / sizeof(LOCAL[0]); i++)
        if (!strcmp(scheme, LOCAL[i])) return true;
    return false;
}

/* FETCH §2.1 "URL", the sentence after the one above: "An HTTP(S) scheme is `http` or `https`." It sits here
   for the reason `local` does — the same section, the same KIND of question, and no property of a scheme makes
   it one. Its caller is HTML §7.2.4's protocol setter, whose step 6 is "if copyURL's scheme is not an HTTP(S)
   scheme, then terminate these steps": that step is what makes `location.protocol = "data"` a silent no-op on
   an http document while `location.protocol = "†"` is a SyntaxError, which is two whole WPT files. */
bool url_scheme_is_http_s(const char *scheme)
{
    return scheme && (!strcmp(scheme, "http") || !strcmp(scheme, "https"));
}

int url_default_port(const char *scheme)
{
    size_t i;
    if (!scheme) return -1;
    for (i = 0; i < sizeof(SPECIAL) / sizeof(SPECIAL[0]); i++)
        if (!strcmp(scheme, SPECIAL[i].scheme)) return SPECIAL[i].port;
    return -1;
}

/* ---- §1.3's percent encode sets ------------------------------------------------------------------------- */

/* Each set is the previous one plus a few code points, exactly as the spec builds them. Written as the
   inclusion chain rather than as eight tables, because the chain is what the spec states and a table copied
   per set is eight places for one code point to go missing. */
static int in_encode_set(unsigned char c, int set)
{
    if (c < 0x20 || c > 0x7e) return 1;                                        /* C0 control */
    if (set == URL_SET_C0) return 0;
    if (c == ' ' || c == '"' || c == '<' || c == '>') return 1;                /* fragment */
    if (set == URL_SET_FRAGMENT) return c == '`';
    if (c == '#') return 1;                                                    /* query */
    if (set == URL_SET_QUERY) return 0;
    if (set == URL_SET_SPECIAL_QUERY) return c == '\'';
    if (c == '?' || c == '^' || c == '`' || c == '{' || c == '}') return 1;    /* path */
    if (set == URL_SET_PATH) return 0;
    if (c == '/' || c == ':' || c == ';' || c == '=' || c == '@' ||            /* userinfo */
        c == '[' || c == '\\' || c == ']' || c == '|') return 1;              /* ^ is already in path */
    if (set == URL_SET_USERINFO) return 0;
    if (c == '$' || c == '%' || c == '&' || c == '+' || c == ',') return 1;    /* component */
    if (set == URL_SET_COMPONENT) return 0;
    DCHECK(set == URL_SET_URLENCODED, "a percent-encode set this file does not define was asked for");
    return c == '!' || c == '\'' || c == '(' || c == ')' || c == '~';
}

static void ustr_put_encoded(UStr *out, const char *s, size_t n, int set)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        /* §5.1's urlencoded serializer maps 0x20 to `+` rather than percent-encoding it. It is part of THAT
           SET's rule, so it is here — doing it in the caller means replacing spaces in an already-encoded
           string, which finds none because the space is already `%20`. */
        if (set == URL_SET_URLENCODED && c == ' ') { ustr_putc(out, '+'); continue; }
        if (in_encode_set(c, set)) {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", c);
            ustr_put(out, buf, 3);
        } else {
            ustr_putc(out, (char)c);
        }
    }
}

char *url_percent_encode(const char *s, size_t len, int set)
{
    UStr o;
    ustr_init(&o);
    ustr_put_encoded(&o, s, len, set);
    return ustr_take(&o);
}

/* §1.3 "percent-decode": `%` followed by two hex digits, and anything else is itself. Public because §5.1's
   urlencoded parser is the other user, and it is one algorithm. */
char *url_percent_decode(const char *s, size_t n, size_t *out_n)
{
    UStr o;
    size_t i;
    ustr_init(&o);
    for (i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && is_ascii_hex((unsigned char)s[i + 1]) &&
            is_ascii_hex((unsigned char)s[i + 2])) {
            ustr_putc(&o, (char)(hexval((unsigned char)s[i + 1]) * 16 + hexval((unsigned char)s[i + 2])));
            i += 2;
        } else {
            ustr_putc(&o, s[i]);
        }
    }
    if (out_n) *out_n = o.n;
    return ustr_take(&o);
}

/* ---- the record ------------------------------------------------------------------------------------------ */

void url_host_free(UrlHost *h) { free(h->domain); h->domain = NULL; h->kind = URL_HOST_NULL; }

/* §4.2's HOST EQUALITY, BY VALUE OVER THE PARSED HOST — "identical" wherever a standard compares two hosts:
   §7.1.1 step 2's same origin, §7.1.1's same origin-domain over two DOMAINS, §7.2.5's can-have-its-URL-rewritten
   and §7.1.1.2's "hostSuffix does not equal originalHost". It lived TWICE — once in origin.c and once in
   history.c — and the two had already drifted at the edge that matters least until it matters: origin.c closed
   its switch with `default: return true`, so a host of a kind url.h does not declare compared EQUAL to
   anything, where history.c's crashed. One rule, one implementation, and the crashing reading is the one that
   survived. Two hosts of different KINDS are
   different however they spell, an IPv4 is a number, an IPv6 is eight of them, and a domain is already
   lowercased ASCII by domain-to-ASCII when the parser built it. */
bool url_host_equal(const UrlHost *a, const UrlHost *b)
{
    DCHECK(a != NULL && b != NULL, "§4.2's host equality was asked with nothing on one side");
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case URL_HOST_DOMAIN:
    case URL_HOST_OPAQUE: return a->domain && b->domain && !strcmp(a->domain, b->domain);
    case URL_HOST_IPV4:   return a->ipv4 == b->ipv4;
    case URL_HOST_IPV6:   return memcmp(a->ipv6, b->ipv6, sizeof a->ipv6) == 0;
    case URL_HOST_NULL:
    case URL_HOST_EMPTY:  return true;   /* the null and empty hosts carry no payload to compare */
    }
    DFAIL("a URL host of a kind url.h does not declare was compared — the parser produces exactly the five, so "
          "a sixth came from a record built by hand");
    return false;
}

void url_record_init(UrlRecord *u)
{
    memset(u, 0, sizeof *u);
    u->port = -1;
}

/* HTML §2.4.1: "A URL matches about:blank if its scheme is `about`, its path contains a single string `blank`,
   its username and password are the empty string, and its host is null" — its query and fragment MAY be
   non-null, which is why they are not tested. "A URL matches about:srcdoc if its scheme is `about`, its path
   contains a single string `srcdoc`, its QUERY IS NULL, its username and password are the empty string, and
   its host is null." A non-special scheme's path is the record's opaque path, which is where `blank` lands.
   IT IS HERE AND NOT IN core/url/origin.c, where it began, because it has two askers now: §7.3.1's determine
   the origin and §2.4.3's fallback base URL. The asymmetry between the two relations (one tests the query, the
   other must not) is exactly the detail a second copy loses. */
bool url_matches_about(const UrlRecord *u, const char *what, bool query_must_be_null)
{
    if (!u->scheme || strcmp(u->scheme, "about")) return false;
    if (!u->opaque_path || strcmp(u->opaque_path, what)) return false;
    if (u->username && *u->username) return false;
    if (u->password && *u->password) return false;
    if (query_must_be_null && u->query) return false;
    return u->host.kind == URL_HOST_NULL;
}

void url_record_free(UrlRecord *u)
{
    int i;
    if (!u) return;
    free(u->scheme); free(u->username); free(u->password);
    url_host_free(&u->host);
    for (i = 0; i < u->npath; i++) free(u->path[i]);
    free(u->path);
    free(u->opaque_path); free(u->query); free(u->fragment);
    url_record_init(u);
}

static void path_push(UrlRecord *u, const char *seg, size_t n)
{
    char *s;
    if (u->npath >= u->cpath) {
        u->cpath = u->cpath ? u->cpath * 2 : 4;
        u->path = realloc(u->path, (size_t)u->cpath * sizeof(char *));
        CHECK(u->path, "url: OOM growing a path");
    }
    s = malloc(n + 1);
    CHECK(s, "url: OOM copying a path segment");
    if (n) memcpy(s, seg, n);
    s[n] = 0;
    u->path[u->npath++] = s;
}

static void path_pop(UrlRecord *u)
{
    if (u->npath > 0) free(u->path[--u->npath]);
}

/* §4.4 "shorten a url's path": a `file:` URL whose single segment is a Windows drive letter keeps it. */
static int is_windows_drive(const char *s, size_t n)
{
    return n == 2 && is_ascii_alpha((unsigned char)s[0]) && (s[1] == ':' || s[1] == '|');
}

static int is_normalized_drive(const char *s)
{
    return strlen(s) == 2 && is_ascii_alpha((unsigned char)s[0]) && s[1] == ':';
}

static void path_shorten(UrlRecord *u)
{
    if (u->scheme && !strcmp(u->scheme, "file") && u->npath == 1 && is_normalized_drive(u->path[0]))
        return;
    path_pop(u);
}

bool url_record_copy(UrlRecord *dst, const UrlRecord *src)
{
    int i;
    url_record_init(dst);
    dst->scheme = xstrdup(src->scheme);
    dst->username = xstrdup(src->username);
    dst->password = xstrdup(src->password);
    dst->host.kind = src->host.kind;
    dst->host.ipv4 = src->host.ipv4;
    memcpy(dst->host.ipv6, src->host.ipv6, sizeof dst->host.ipv6);
    if (src->host.domain) dst->host.domain = xstrdup(src->host.domain);
    dst->port = src->port;
    for (i = 0; i < src->npath; i++) path_push(dst, src->path[i], strlen(src->path[i]));
    if (src->opaque_path) dst->opaque_path = xstrdup(src->opaque_path);
    if (src->query) dst->query = xstrdup(src->query);
    if (src->fragment) dst->fragment = xstrdup(src->fragment);
    return true;
}

/* ---- §4.2's host parser ---------------------------------------------------------------------------------- */

/* "ends in a number": what decides whether a domain is really an IPv4 address written in one of the four
   legacy radices. `http://1.2.3.4.5/` is a DOMAIN and `http://0x7f.1/` is 127.0.0.1, and this is the whole of
   the difference. */
static int last_part_is_number(const char *s, size_t n)
{
    const char *b, *e = s + n;
    size_t i;
    /* the last part, after dropping one trailing empty part */
    if (n && s[n - 1] == '.') e--;
    b = e;
    while (b > s && b[-1] != '.') b--;
    if (b == e) return 0;
    if ((size_t)(e - b) >= 2 && b[0] == '0' && lower((unsigned char)b[1]) == 'x') {
        for (i = 2; b + i < e; i++) if (!is_ascii_hex((unsigned char)b[i])) return 0;
        return 1;
    }
    for (i = 0; b + i < e; i++) if (!is_ascii_digit((unsigned char)b[i])) return 0;
    return 1;
}

/* §4.2's "IPv4 number parser": the radix comes from the prefix, and an empty body after `0x` is zero. */
static int ipv4_number(const char *b, const char *e, uint64_t *out, int *validation)
{
    int radix = 10;
    uint64_t v = 0;
    if (b == e) return 0;
    if ((size_t)(e - b) >= 2 && b[0] == '0' && lower((unsigned char)b[1]) == 'x') {
        *validation = 1; radix = 16; b += 2;
    } else if ((size_t)(e - b) >= 2 && b[0] == '0') {
        *validation = 1; radix = 8; b += 1;
    }
    if (b == e) { *out = 0; return 1; }
    for (; b < e; b++) {
        int d;
        if (radix == 16) { if (!is_ascii_hex((unsigned char)*b)) return 0; d = hexval((unsigned char)*b); }
        else if (radix == 8) { if (*b < '0' || *b > '7') return 0; d = *b - '0'; }
        else { if (!is_ascii_digit((unsigned char)*b)) return 0; d = *b - '0'; }
        v = v * (uint64_t)radix + (uint64_t)d;
        if (v > 0xffffffffull) return 0;
    }
    *out = v;
    return 1;
}

static bool parse_ipv4(const char *s, size_t n, uint32_t *out)
{
    const char *parts[8];
    size_t lens[8];
    int np = 0, i, validation = 0;
    const char *b = s, *e = s + n;
    uint64_t nums[8], v;

    if (n && s[n - 1] == '.') e--;                    /* one trailing empty part is dropped */
    for (;;) {
        const char *dot = memchr(b, '.', (size_t)(e - b));
        const char *pe = dot ? dot : e;
        if (np >= 8) return false;
        parts[np] = b; lens[np] = (size_t)(pe - b); np++;
        if (!dot) break;
        b = dot + 1;
    }
    if (np > 4) return false;
    for (i = 0; i < np; i++)
        if (!ipv4_number(parts[i], parts[i] + lens[i], &nums[i], &validation)) return false;
    for (i = 0; i < np - 1; i++)
        if (nums[i] > 255) return false;
    if (nums[np - 1] >= (uint64_t)1 << (8 * (5 - np))) return false;
    v = nums[np - 1];
    for (i = 0; i < np - 1; i++)
        v += nums[i] << (8 * (3 - i));
    *out = (uint32_t)v;
    return true;
}

/* §4.2's IPv6 parser, written as the spec writes it — the compressed `::` piece index and all. */
static bool parse_ipv6(const char *s, size_t n, uint16_t out[8])
{
    int piece = 0, compress = -1, i;
    const char *p = s, *e = s + n;

    memset(out, 0, 8 * sizeof(uint16_t));
    if (p < e && *p == ':') {
        if (e - p < 2 || p[1] != ':') return false;
        p += 2;
        piece++;
        compress = piece;
    }
    while (p < e) {
        unsigned value = 0, length = 0;
        if (piece == 8) return false;
        if (*p == ':') {
            if (compress >= 0) return false;
            p++;
            piece++;
            compress = piece;
            continue;
        }
        while (length < 4 && p < e && is_ascii_hex((unsigned char)*p)) {
            value = value * 16 + (unsigned)hexval((unsigned char)*p);
            p++; length++;
        }
        if (p < e && *p == '.') {
            int numbers = 0;
            if (length == 0) return false;
            p -= length;
            if (piece > 6) return false;
            while (p < e) {
                unsigned ipv4piece = 0;
                int have = 0;
                if (numbers > 0) {
                    if (*p == '.' && numbers < 4) p++;
                    else return false;
                }
                if (p >= e || !is_ascii_digit((unsigned char)*p)) return false;
                while (p < e && is_ascii_digit((unsigned char)*p)) {
                    if (!have) { ipv4piece = (unsigned)(*p - '0'); have = 1; }
                    else if (ipv4piece == 0) return false;
                    else ipv4piece = ipv4piece * 10 + (unsigned)(*p - '0');
                    if (ipv4piece > 255) return false;
                    p++;
                }
                out[piece] = (uint16_t)(out[piece] * 0x100 + ipv4piece);
                numbers++;
                if (numbers == 2 || numbers == 4) piece++;
                if (numbers == 4) break;
            }
            if (numbers != 4) return false;
            /* §4.2's IPv6 parser RETURNS after the embedded IPv4, so anything still in the input is a
               FAILURE — `[::127.0.0.1.]`, `[::1.2.3.4x]` and `[::127.0.0.0.1]` all parsed as valid because
               this loop merely stopped reading rather than refusing what it had not read. */
            if (p != e) return false;
            break;
        }
        if (p < e && *p == ':') {
            p++;
            if (p >= e) return false;
        } else if (p < e) {
            return false;
        }
        out[piece++] = (uint16_t)value;
    }
    if (compress >= 0) {
        int swaps = piece - compress;
        piece = 7;
        while (piece != 0 && swaps > 0) {
            uint16_t t = out[piece];
            out[piece] = out[compress + swaps - 1];
            out[compress + swaps - 1] = t;
            piece--; swaps--;
        }
    } else if (piece != 8) {
        return false;
    }
    return true;
}

/* §4.2's "opaque-host parser": every forbidden HOST code point is a failure, and the rest is percent-encoded
   with the C0 control set. */
static bool parse_opaque_host(const char *s, size_t n, UrlHost *out)
{
    UStr o;
    size_t i;
    for (i = 0; i < n; i++)
        if (is_forbidden_host((unsigned char)s[i])) return false;
    ustr_init(&o);
    ustr_put_encoded(&o, s, n, URL_SET_C0);
    out->kind = URL_HOST_OPAQUE;
    out->domain = ustr_take(&o);
    return true;
}

/* §4.2's "domain to ASCII" for the ASCII case, which is a lowercase and the validity tests. A non-ASCII domain
   is UTS-46 plus Punycode and goes to idna.c: answering with the bytes lowercased would put a domain in the
   record that no resolver would agree with, and this comment used to say that case reached a DFAIL — which it
   has not since idna.c was built, and the call to it is in the body this sentence sits on. */
static bool parse_host(const char *s, size_t n, bool is_opaque, UrlHost *out)
{
    char *decoded;
    size_t dn = 0, i;
    bool ok = true;

    memset(out, 0, sizeof *out);
    if (n >= 2 && s[0] == '[') {
        if (s[n - 1] != ']') return false;
        if (!parse_ipv6(s + 1, n - 2, out->ipv6)) return false;
        out->kind = URL_HOST_IPV6;
        return true;
    }
    /* THE EMPTY HOST IS THE EMPTY HOST, opaque scheme or not. Sending an empty opaque input through the
       opaque-host parser produced URL_HOST_OPAQUE with an empty string, which serializes identically and is
       therefore invisible — except to "cannot have a username/password/port", which tests for the empty host
       and so let `sc:///`.username = "x" write `sc://x@/`. */
    if (n == 0) { out->kind = URL_HOST_EMPTY; return true; }
    if (is_opaque)
        return parse_opaque_host(s, n, out);

    /* §3.5 step 5: "Let domain be the result of running UTF-8 decode without BOM on the percent-decoding of
       input." The percent-decoding answers BYTES and the decode is what makes them a domain — and the standard
       says what happens next in its own note: "UTF-8 decode without BOM or fail can be used, coupled with an
       early return for failure, as the domain parser fails on U+FFFD". So a malformed sequence becomes a URL
       that does not parse, which is what the replacement character reaching an IDNA_ERROR range does here.
       Handing the raw bytes to domain-to-ASCII instead let an OVERLONG sequence be read as the character it is
       not allowed to spell: `http://%C1%A1.com/` walked 0xC1 0xA1 as U+0061 and produced the host `a.com`,
       where the standard has failure — a hostname smuggled past every check that compared one. */
    {
        size_t rawn = 0;
        char *raw = url_percent_decode(s, n, &rawn);
        decoded = encoding_utf8_decode_without_bom(raw, rawn, &dn);
        free(raw);
    }
    /* §4.2 step 4: DOMAIN TO ASCII. A non-ASCII domain is not lowercased and hoped for — `münchen.de` IS
       `xn--mnchen-3ya.de` in the record, and every comparison a page makes against `location.host` is against
       that A-label. The ASCII path stays here because UTS-46's answer for it is exactly this lowercase, and
       running the whole of IDNA on `example.com` would allocate four times to reach the same bytes. */
    for (i = 0; i < dn; i++) if ((unsigned char)decoded[i] >= 0x80) break;
    if (i < dn) {
        char *ascii = NULL;
        size_t an = 0;
        int r = idna_domain_to_ascii(decoded, dn, &ascii, &an);
        free(decoded);
        if (r < 0) return false;
        decoded = ascii;
        dn = an;
    }
    for (i = 0; i < dn; i++) {
        if (is_forbidden_domain((unsigned char)decoded[i])) { ok = false; break; }
        decoded[i] = (char)lower((unsigned char)decoded[i]);
    }
    if (!ok || dn == 0) { free(decoded); return false; }
    if (last_part_is_number(decoded, dn)) {
        uint32_t v;
        bool r = parse_ipv4(decoded, dn, &v);
        free(decoded);
        if (!r) return false;
        out->kind = URL_HOST_IPV4;
        out->ipv4 = v;
        return true;
    }
    out->kind = URL_HOST_DOMAIN;
    out->domain = decoded;
    return true;
}

/* §4.2's HOST PARSER, AS ITS OWN ENTRY, because a host is parsed in places that have no URL to parse it into.
   HTML §7.1.1.2's `document.domain` setter is one: its step 2 is "let hostSuffix be the result of PARSING
   hostSuffixString" and there is no scheme, no base and no record anywhere in that algorithm — the answer is a
   HOST and the caller compares it against another host. Routing that through a scratch URL would make the
   answer depend on a scheme the algorithm never mentions (`isNotSpecial` decides between the domain parser and
   the opaque-host parser, so a scratch `sc:` URL would percent-encode where the standard wants domain-to-ASCII).
   `is_opaque` is §4.2's own parameter. `*out` is left zeroed on failure and is the CALLER's to url_host_free
   whatever the answer was. */
bool url_parse_host(UrlHost *out, const char *s, size_t len, bool is_opaque)
{
    DCHECK(out != NULL && s != NULL, "§4.2's host parser was called with nothing to parse or nowhere to put it");
    if (parse_host(s, len, is_opaque, out)) return true;
    /* ZEROED ON FAILURE, and asserted rather than re-zeroed here: §4.2's parser clears `out` before it starts
       and every failure path of it returns before allocating, so a partially built host arriving here is that
       contract broken — and quietly memsetting it would turn a LEAK into a clean-looking return. */
    DCHECK(out->kind == URL_HOST_NULL && out->domain == NULL,
           "§4.2's host parser left a partially built host behind on FAILURE — whatever it allocated is now "
           "unreachable, and the caller is about to free a record that no longer names it");
    return false;
}

/* §4.3's host serializer. The IPv6 form is the compressed one, which is why the record keeps the pieces. */
char *url_serialize_host(const UrlHost *h)
{
    UStr o;
    ustr_init(&o);
    switch (h->kind) {
    case URL_HOST_DOMAIN:
    case URL_HOST_OPAQUE:
        ustr_puts(&o, h->domain ? h->domain : "");
        break;
    case URL_HOST_IPV4: {
        char buf[16];
        uint32_t v = h->ipv4;
        snprintf(buf, sizeof buf, "%u.%u.%u.%u", (v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
        ustr_puts(&o, buf);
        break;
    }
    case URL_HOST_IPV6: {
        /* the longest run of two or more zero pieces is the one that compresses, and only that one */
        int best = -1, best_len = 0, i, run = -1, run_len = 0;
        for (i = 0; i < 8; i++) {
            if (h->ipv6[i] == 0) {
                if (run < 0) { run = i; run_len = 1; } else run_len++;
                if (run_len > best_len) { best = run; best_len = run_len; }
            } else {
                run = -1; run_len = 0;
            }
        }
        if (best_len < 2) best = -1;
        ustr_putc(&o, '[');
        for (i = 0; i < 8; i++) {
            if (best >= 0 && i == best) {
                ustr_puts(&o, i == 0 ? "::" : ":");
                i += best_len - 1;
                continue;
            }
            {
                char buf[8];
                snprintf(buf, sizeof buf, "%x", h->ipv6[i]);
                ustr_puts(&o, buf);
            }
            if (i != 7) ustr_putc(&o, ':');
        }
        ustr_putc(&o, ']');
        break;
    }
    default:
        break;   /* null and empty both serialize to nothing */
    }
    return ustr_take(&o);
}

/* `host` and `port` AS THEY ARE READ — the same serialization for URL and for Location, which is why they are
   here and not written twice. §5.1's `host` is the host with the port appended when there is one; `port` is
   the empty string when the port is null, which is what a default port becomes at parse time. */
char *url_serialize_host_port(const UrlRecord *u)
{
    UStr o;
    char *h;
    ustr_init(&o);
    if (u->host.kind == URL_HOST_NULL) return ustr_take(&o);
    h = url_serialize_host(&u->host);
    ustr_puts(&o, h);
    free(h);
    if (u->port >= 0) { char b[8]; snprintf(b, sizeof b, ":%d", u->port); ustr_puts(&o, b); }
    return ustr_take(&o);
}

char *url_serialize_port(const UrlRecord *u)
{
    char b[8];
    if (u->port < 0) return xstrdup("");
    snprintf(b, sizeof b, "%d", u->port);
    return xstrdup(b);
}

/* §4.5's "URL PATH SERIALIZING", which is what §5.1's `pathname` returns: the opaque path, or the segments
   each preceded by a slash. The `/.` prefix is NOT here — that belongs to the whole-URL serializer, and
   putting it here made `pathname` report `/.//path` where the spec says `//path`. */
char *url_serialize_path(const UrlRecord *u)
{
    UStr o;
    int i;
    ustr_init(&o);
    if (u->opaque_path) { ustr_puts(&o, u->opaque_path); return ustr_take(&o); }
    for (i = 0; i < u->npath; i++) {
        ustr_putc(&o, '/');
        ustr_puts(&o, u->path[i]);
    }
    return ustr_take(&o);
}

char *url_serialize(const UrlRecord *u, bool exclude_fragment)
{
    UStr o;
    char *host, *path;

    ustr_init(&o);
    ustr_puts(&o, u->scheme ? u->scheme : "");
    ustr_putc(&o, ':');
    if (u->host.kind != URL_HOST_NULL) {
        ustr_puts(&o, "//");
        if ((u->username && *u->username) || (u->password && *u->password)) {
            ustr_puts(&o, u->username ? u->username : "");
            if (u->password && *u->password) { ustr_putc(&o, ':'); ustr_puts(&o, u->password); }
            ustr_putc(&o, '@');
        }
        host = url_serialize_host(&u->host);
        ustr_puts(&o, host);
        free(host);
        if (u->port >= 0) {
            char buf[8];
            snprintf(buf, sizeof buf, ":%d", u->port);
            ustr_puts(&o, buf);
        }
    }
    /* §4.5: a host-less URL whose first segment is empty gets `/.` in FRONT of its path, so the result cannot
       be re-read as an authority — `/​/path` would parse back with the host `path`. It is the SERIALIZER's
       step and not the path's, which is why `pathname` does not show it. */
    if (u->host.kind == URL_HOST_NULL && !u->opaque_path && u->npath > 1 && u->path[0][0] == 0)
        ustr_puts(&o, "/.");
    path = url_serialize_path(u);
    ustr_puts(&o, path);
    free(path);
    if (u->query) { ustr_putc(&o, '?'); ustr_puts(&o, u->query); }
    if (!exclude_fragment && u->fragment) { ustr_putc(&o, '#'); ustr_puts(&o, u->fragment); }
    return ustr_take(&o);
}

/* §4.7's ORIGIN OF A URL, SERIALIZED. THE RULE ITSELF LIVES IN core/url/origin.c, WHICH OWNS THE TYPE — an
   origin is a RECORD (a nonce for the opaque kind, a scheme/host/port/domain tuple otherwise) and this is one
   operation over it, so this file no longer holds a second copy of §4.7's scheme switch. The copy mattered:
   the two answers were the same only while nothing compared origins by IDENTITY, and every same-origin check
   in the engine now does. */
char *url_serialize_origin(const UrlRecord *u)
{
    return origin_serialize_of_url(u);
}

/* ---- §4.4's basic URL parser ------------------------------------------------------------------------------ */

enum {
    ST_SCHEME_START, ST_SCHEME, ST_NO_SCHEME, ST_SPECIAL_RELATIVE_OR_AUTHORITY, ST_PATH_OR_AUTHORITY,
    ST_RELATIVE, ST_RELATIVE_SLASH, ST_SPECIAL_AUTHORITY_SLASHES, ST_SPECIAL_AUTHORITY_IGNORE_SLASHES,
    ST_AUTHORITY, ST_HOST, ST_PORT, ST_FILE, ST_FILE_SLASH, ST_FILE_HOST, ST_PATH_START, ST_PATH,
    ST_OPAQUE_PATH, ST_QUERY, ST_FRAGMENT,
    /* §4.4 lists `hostname state` as its own state; it differs from host state only in refusing the `:` that
       would start a port, so it is spelled as a distinct OVERRIDE value over the one implementation rather
       than as a copy of the state. */
    ST_HOSTNAME_OVERRIDE,
};

/* §4.2 "URL miscellaneous": "A URL cannot have a username/password/port if its host is null or the empty
   host, or its scheme is `file`." The three setters that write those parts all return early on it.
   IT CARRIES THE SPEC'S WHOLE NAME AND IS PUBLIC. It was `url_cannot_have_credentials`, private — a name that
   drops the PORT, which is the one of the three another standard asks about: HTML §7.2.4's port setter opens
   with this predicate word for word. A shortened name invites the reader who needs the port arm to re-read
   three fields in their own file, which is the second answer that drifts. (The section is §4.2, not §4.1 as
   the line above this used to cite: §4.1 is URL representation and defines the record's FIELDS; the
   predicates over them — special, includes credentials, opaque path, this one — are §4.2's.) */
bool url_cannot_have_username_password_port(const UrlRecord *u)
{
    return u->host.kind == URL_HOST_NULL || u->host.kind == URL_HOST_EMPTY ||
           (u->scheme && !strcmp(u->scheme, "file"));
}

static bool url_includes_credentials(const UrlRecord *u)
{
    return (u->username && *u->username) || (u->password && *u->password);
}

/* §4.4 WITH A STATE OVERRIDE is the same algorithm entered part-way through, and every setter is one call to
   it. The override also changes what several states DO — a scheme that would change specialness is refused
   rather than applied, and the host, port and hostname states return the moment they have their answer
   instead of running on into the path. Writing the setters as field assignments instead would be eleven
   places to get `u.protocol = "https:"` wrong; this is one. */
static bool url_parse_into(UrlRecord *url, const char *input_raw, size_t raw_len, const UrlRecord *base,
                           int state_override)
{
    char *input;
    size_t len = 0, i;
    /* the hostname override runs the HOST state; only its `:` arm differs, which the override value selects */
    int state = state_override < 0 ? ST_SCHEME_START
              : (state_override == ST_HOSTNAME_OVERRIDE ? ST_HOST : state_override);
    size_t pointer;
    UStr buffer;
    int at_sign_seen = 0, inside_brackets = 0, password_token_seen = 0;
    bool ok = false;

    if (state_override < 0)
        url_record_init(url);
    ustr_init(&buffer);

    /* §4.4 steps 1-3. The leading/trailing C0-control-or-space STRIP is step 1, which runs only "if url is not
       given" — so a SETTER does not strip, and `u.protocol = "\0http"` therefore fails to parse and leaves the
       URL alone rather than quietly setting `http`. The tab/newline removal is step 3 and is unconditional,
       which is why those three characters ARE stripped from a setter's input. */
    input = malloc(raw_len + 1);
    CHECK(input, "url: OOM copying a URL input");
    {
        size_t b = 0, e = raw_len;
        if (state_override < 0) {
            while (b < e && (unsigned char)input_raw[b] <= 0x20) b++;
            while (e > b && (unsigned char)input_raw[e - 1] <= 0x20) e--;
        }
        for (i = b; i < e; i++) {
            char c = input_raw[i];
            if (c == 0x09 || c == 0x0a || c == 0x0d) continue;
            input[len++] = c;
        }
    }
    input[len] = 0;

    if (state_override < 0) {
        url->scheme = xstrdup("");
        url->username = xstrdup("");
        url->password = xstrdup("");
    }

    /* The pointer walks one PAST the end: the spec's EOF is a real position every state may act on. */
    for (pointer = 0; pointer <= len; pointer++) {
        int c = (pointer < len) ? (unsigned char)input[pointer] : -1;
        int reprocess = 1;
        while (reprocess) {
            reprocess = 0;
            switch (state) {
            case ST_SCHEME_START:
                if (c >= 0 && is_ascii_alpha(c)) { ustr_putc(&buffer, (char)lower(c)); state = ST_SCHEME; }
                else if (state_override >= 0) goto fail;   /* §4.4: a setter's non-alpha start IS a failure */
                /* §4.4 "decrease pointer by 1" and the loop's increment cancel: the SAME character is
                   reprocessed in no-scheme state. Writing the decrement literally and leaving `c` behind made
                   the next state read a character that was not in the input — `new URL("./foo", base)` came
                   out as `/%FE./foo`, which is that stale byte percent-encoded into the path. */
                else { state = ST_NO_SCHEME; reprocess = 1; }
                break;

            case ST_SCHEME:
                if (c >= 0 && (is_ascii_alnum(c) || c == '+' || c == '-' || c == '.')) {
                    ustr_putc(&buffer, (char)lower(c));
                } else if (c == ':') {
                    const char *nb = buffer.p ? buffer.p : "";
                    if (state_override >= 0) {
                        /* §4.4 refuses a scheme change that would change SPECIALNESS, that would give a
                           `file:` URL credentials or a port, or that would make a credential-bearing URL
                           `file:` — each of those would leave a record no parse could have produced. */
                        if (url_scheme_is_special(url->scheme) != url_scheme_is_special(nb)) goto done_ok;
                        if (!strcmp(nb, "file") && (url_includes_credentials(url) || url->port >= 0))
                            goto done_ok;
                        if (!strcmp(url->scheme, "file") && url->host.kind == URL_HOST_EMPTY) goto done_ok;
                    }
                    free(url->scheme);
                    url->scheme = xstrdup(nb);
                    ustr_free(&buffer);
                    if (state_override >= 0) {
                        if (url->port == url_default_port(url->scheme)) url->port = -1;
                        goto done_ok;
                    }
                    if (!strcmp(url->scheme, "file")) {
                        state = ST_FILE;
                    } else if (url_scheme_is_special(url->scheme) && base &&
                               base->scheme && !strcmp(base->scheme, url->scheme)) {
                        state = ST_SPECIAL_RELATIVE_OR_AUTHORITY;
                    } else if (url_scheme_is_special(url->scheme)) {
                        state = ST_SPECIAL_AUTHORITY_SLASHES;
                    } else if (pointer + 1 < len && input[pointer + 1] == '/') {
                        state = ST_PATH_OR_AUTHORITY;
                        pointer++;
                    } else {
                        free(url->opaque_path);
                        url->opaque_path = xstrdup("");
                        state = ST_OPAQUE_PATH;
                    }
                } else if (state_override >= 0) {
                    goto fail;
                } else {
                    /* not a scheme after all: start over from the beginning in no-scheme state */
                    ustr_free(&buffer);
                    state = ST_NO_SCHEME;
                    pointer = (size_t)-1;   /* the for's ++ brings it back to 0 */
                    goto next_char;
                }
                break;

            case ST_NO_SCHEME:
                if (!base || (base->opaque_path && c != '#')) goto fail;
                if (base->opaque_path && c == '#') {
                    free(url->scheme); url->scheme = xstrdup(base->scheme);
                    free(url->opaque_path); url->opaque_path = xstrdup(base->opaque_path);
                    if (base->query) url->query = xstrdup(base->query);
                    free(url->fragment); url->fragment = xstrdup("");
                    state = ST_FRAGMENT;
                } else if (strcmp(base->scheme, "file")) {
                    state = ST_RELATIVE;
                    reprocess = 1;
                } else {
                    state = ST_FILE;
                    reprocess = 1;
                }
                break;

            case ST_SPECIAL_RELATIVE_OR_AUTHORITY:
                if (c == '/' && pointer + 1 < len && input[pointer + 1] == '/') {
                    state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                    pointer++;
                } else {
                    state = ST_RELATIVE;
                    reprocess = 1;
                }
                break;

            case ST_PATH_OR_AUTHORITY:
                if (c == '/') state = ST_AUTHORITY;
                else { state = ST_PATH; reprocess = 1; }
                break;

            case ST_RELATIVE:
                DCHECK(base != NULL && strcmp(base->scheme, "file") != 0,
                       "the relative state was entered with no base or with a file base — §4.4 asserts both");
                free(url->scheme); url->scheme = xstrdup(base->scheme);
                if (c == '/' || (c == '\\' && url_scheme_is_special(url->scheme))) {
                    state = ST_RELATIVE_SLASH;
                } else {
                    int k;
                    free(url->username); url->username = xstrdup(base->username);
                    free(url->password); url->password = xstrdup(base->password);
                    url_host_free(&url->host);
                    url->host.kind = base->host.kind;
                    url->host.ipv4 = base->host.ipv4;
                    memcpy(url->host.ipv6, base->host.ipv6, sizeof url->host.ipv6);
                    if (base->host.domain) url->host.domain = xstrdup(base->host.domain);
                    url->port = base->port;
                    for (k = 0; k < base->npath; k++) path_push(url, base->path[k], strlen(base->path[k]));
                    if (c == '?') {
                        free(url->query); url->query = xstrdup("");
                        state = ST_QUERY;
                    } else if (c == '#') {
                        if (base->query) url->query = xstrdup(base->query);
                        free(url->fragment); url->fragment = xstrdup("");
                        state = ST_FRAGMENT;
                    } else if (c != -1) {
                        if (base->query) { /* the base's query is dropped, per §4.4 */ }
                        path_shorten(url);
                        state = ST_PATH;
                        reprocess = 1;
                    } else {
                        if (base->query) url->query = xstrdup(base->query);
                    }
                }
                break;

            case ST_RELATIVE_SLASH:
                if (url_scheme_is_special(url->scheme) && (c == '/' || c == '\\')) {
                    state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                } else if (c == '/') {
                    state = ST_AUTHORITY;
                } else {
                    free(url->username); url->username = xstrdup(base->username);
                    free(url->password); url->password = xstrdup(base->password);
                    url_host_free(&url->host);
                    url->host.kind = base->host.kind;
                    url->host.ipv4 = base->host.ipv4;
                    memcpy(url->host.ipv6, base->host.ipv6, sizeof url->host.ipv6);
                    if (base->host.domain) url->host.domain = xstrdup(base->host.domain);
                    url->port = base->port;
                    state = ST_PATH;
                    reprocess = 1;
                }
                break;

            case ST_SPECIAL_AUTHORITY_SLASHES:
                if (c == '/' && pointer + 1 < len && input[pointer + 1] == '/') {
                    state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                    pointer++;
                } else {
                    state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                    reprocess = 1;
                }
                break;

            case ST_SPECIAL_AUTHORITY_IGNORE_SLASHES:
                if (c != '/' && c != '\\') { state = ST_AUTHORITY; reprocess = 1; }
                break;

            case ST_AUTHORITY:
                if (c == '@') {
                    size_t k;
                    if (at_sign_seen) {
                        UStr t;
                        ustr_init(&t);
                        ustr_puts(&t, "%40");
                        ustr_put(&t, buffer.p ? buffer.p : "", buffer.n);
                        ustr_free(&buffer);
                        buffer = t;
                    }
                    at_sign_seen = 1;
                    for (k = 0; k < buffer.n; k++) {
                        char ch = buffer.p[k];
                        if (ch == ':' && !password_token_seen) { password_token_seen = 1; continue; }
                        {
                            UStr t;
                            char *cur = password_token_seen ? url->password : url->username;
                            ustr_init(&t);
                            ustr_puts(&t, cur);
                            ustr_put_encoded(&t, &ch, 1, URL_SET_USERINFO);
                            free(cur);
                            if (password_token_seen) url->password = ustr_take(&t);
                            else url->username = ustr_take(&t);
                        }
                    }
                    ustr_free(&buffer);
                } else if (c == -1 || c == '/' || c == '?' || c == '#' ||
                           (c == '\\' && url_scheme_is_special(url->scheme))) {
                    if (at_sign_seen && buffer.n == 0) goto fail;
                    pointer -= buffer.n + 1;
                    ustr_free(&buffer);
                    state = ST_HOST;
                    goto next_char;
                } else {
                    ustr_putc(&buffer, (char)c);
                }
                break;

            case ST_HOST:
                if (state_override >= 0 && url->scheme && !strcmp(url->scheme, "file")) {
                    pointer--;
                    state = ST_FILE_HOST;
                    goto next_char;
                }
                if (c == ':' && !inside_brackets) {
                    UrlHost h;
                    /* §4.4: `u.hostname = "x:8080"` stops AT the colon — the hostname setter is not a host
                       setter, and writing the port through it is exactly what the override refuses. */
                    if (state_override == ST_HOSTNAME_OVERRIDE) goto done_ok;
                    if (buffer.n == 0) goto fail;
                    if (!parse_host(buffer.p, buffer.n, !url_scheme_is_special(url->scheme), &h)) goto fail;
                    url_host_free(&url->host);
                    url->host = h;
                    ustr_free(&buffer);
                    state = ST_PORT;
                } else if (c == -1 || c == '/' || c == '?' || c == '#' ||
                           (c == '\\' && url_scheme_is_special(url->scheme))) {
                    UrlHost h;
                    pointer--;
                    if (url_scheme_is_special(url->scheme) && buffer.n == 0) goto fail;
                    if (state_override >= 0 && buffer.n == 0 &&
                        (url_includes_credentials(url) || url->port >= 0)) goto done_ok;
                    if (!parse_host(buffer.p ? buffer.p : "", buffer.n,
                                    !url_scheme_is_special(url->scheme), &h)) goto fail;
                    url_host_free(&url->host);
                    url->host = h;
                    ustr_free(&buffer);
                    if (state_override >= 0) goto done_ok;
                    state = ST_PATH_START;
                    goto next_char;
                } else {
                    if (c == '[') inside_brackets = 1;
                    if (c == ']') inside_brackets = 0;
                    ustr_putc(&buffer, (char)c);
                }
                break;

            case ST_PORT:
                if (c >= 0 && is_ascii_digit(c)) {
                    ustr_putc(&buffer, (char)c);
                } else if (c == -1 || c == '/' || c == '?' || c == '#' ||
                           (c == '\\' && url_scheme_is_special(url->scheme)) || state_override >= 0) {
                    /* §4.4's port state ends on "or state override is given" — which is why `u.port = "90\0"`
                       is port 90 and not a failure: a setter takes the digits it has and stops, where a fresh
                       parse would call the same character invalid. */
                    if (buffer.n) {
                        long v = 0;
                        size_t k;
                        for (k = 0; k < buffer.n; k++) {
                            v = v * 10 + (buffer.p[k] - '0');
                            if (v > 65535) goto fail;
                        }
                        url->port = (v == url_default_port(url->scheme)) ? -1 : (int)v;
                        ustr_free(&buffer);
                    }
                    if (state_override >= 0) goto done_ok;
                    state = ST_PATH_START;
                    reprocess = 1;
                } else {
                    goto fail;
                }
                break;

            case ST_FILE:
                free(url->scheme); url->scheme = xstrdup("file");
                url_host_free(&url->host);
                url->host.kind = URL_HOST_EMPTY;
                if (c == '/' || c == '\\') {
                    state = ST_FILE_SLASH;
                } else if (base && !strcmp(base->scheme, "file")) {
                    int k;
                    url_host_free(&url->host);
                    url->host.kind = base->host.kind;
                    url->host.ipv4 = base->host.ipv4;
                    memcpy(url->host.ipv6, base->host.ipv6, sizeof url->host.ipv6);
                    if (base->host.domain) url->host.domain = xstrdup(base->host.domain);
                    for (k = 0; k < base->npath; k++) path_push(url, base->path[k], strlen(base->path[k]));
                    if (c == '?') {
                        free(url->query); url->query = xstrdup("");
                        state = ST_QUERY;
                    } else if (c == '#') {
                        if (base->query) url->query = xstrdup(base->query);
                        free(url->fragment); url->fragment = xstrdup("");
                        state = ST_FRAGMENT;
                    } else if (c != -1) {
                        /* a Windows drive letter starts a fresh path rather than continuing the base's */
                        if (!(len - pointer >= 2 && is_windows_drive(input + pointer, 2) &&
                              (len - pointer == 2 || strchr("/\\?#", input[pointer + 2]))))
                            path_shorten(url);
                        else
                            while (url->npath) path_pop(url);
                        state = ST_PATH;
                        reprocess = 1;
                    } else {
                        /* EOF with a file base takes the base's QUERY too — §4.4 sets host, path AND query
                           together, and leaving the query out made `new URL("", "file:///t?q#f")` come back
                           as `file:///t`, silently dropping the query the base carried. */
                        if (base->query) url->query = xstrdup(base->query);
                    }
                } else {
                    state = ST_PATH;
                    reprocess = 1;
                }
                break;

            case ST_FILE_SLASH:
                if (c == '/' || c == '\\') {
                    state = ST_FILE_HOST;
                } else {
                    if (base && !strcmp(base->scheme, "file")) {
                        url_host_free(&url->host);
                        url->host.kind = base->host.kind;
                        url->host.ipv4 = base->host.ipv4;
                        memcpy(url->host.ipv6, base->host.ipv6, sizeof url->host.ipv6);
                        if (base->host.domain) url->host.domain = xstrdup(base->host.domain);
                        if (!(len - pointer >= 2 && is_windows_drive(input + pointer, 2)) &&
                            base->npath > 0 && is_normalized_drive(base->path[0]))
                            path_push(url, base->path[0], strlen(base->path[0]));
                    }
                    state = ST_PATH;
                    reprocess = 1;
                }
                break;

            case ST_FILE_HOST:
                if (c == -1 || c == '/' || c == '\\' || c == '?' || c == '#') {
                    pointer--;
                    if (is_windows_drive(buffer.p ? buffer.p : "", buffer.n)) {
                        /* NOT A HOST AT ALL: `file://C:/` is a PATH, and §4.4 keeps the buffer so the path
                           state consumes it as the first segment. Freeing it here dropped the drive letter,
                           so `file://C:/` came out as `file:////` — a different URL with an empty host. */
                        state = ST_PATH;
                        goto next_char;
                    } else if (buffer.n == 0) {
                        url_host_free(&url->host);
                        url->host.kind = URL_HOST_EMPTY;
                        if (state_override >= 0) { ustr_free(&buffer); goto done_ok; }
                        state = ST_PATH_START;
                    } else {
                        UrlHost h;
                        if (!parse_host(buffer.p, buffer.n, false, &h)) goto fail;
                        if (h.kind == URL_HOST_DOMAIN && h.domain && !strcmp(h.domain, "localhost")) {
                            url_host_free(&h);
                            h.kind = URL_HOST_EMPTY;
                        }
                        url_host_free(&url->host);
                        url->host = h;
                        ustr_free(&buffer);
                        if (state_override >= 0) goto done_ok;
                        state = ST_PATH_START;
                    }
                    ustr_free(&buffer);
                    goto next_char;
                }
                ustr_putc(&buffer, (char)c);
                break;

            case ST_PATH_START:
                if (url_scheme_is_special(url->scheme)) {
                    state = ST_PATH;
                    if (c != '/' && c != '\\') reprocess = 1;
                } else if (state_override < 0 && c == '?') {
                    free(url->query); url->query = xstrdup("");
                    state = ST_QUERY;
                } else if (state_override < 0 && c == '#') {
                    free(url->fragment); url->fragment = xstrdup("");
                    state = ST_FRAGMENT;
                } else if (c != -1) {
                    state = ST_PATH;
                    if (c != '/') reprocess = 1;
                } else if (state_override >= 0 && url->host.kind == URL_HOST_NULL) {
                    /* §4.4's last path-start clause, which only a SETTER reaches: an empty pathname on a
                       host-less URL is one EMPTY SEGMENT, not no path at all — `foo:/some/path` with
                       `pathname = ""` is `foo:/`, and without this it collapsed to `foo:` and could never get
                       a path back. */
                    path_push(url, "", 0);
                }
                break;

            case ST_PATH:
                /* §4.4: `?` and `#` end the path only when NO state override is given. A setter is writing one
                   component, so they are ordinary path characters there and get percent-encoded —
                   `u.pathname = "?"` is `/%3F`, and terminating on them made it `/?`, which is a query. */
                if (c == -1 || c == '/' || (c == '\\' && url_scheme_is_special(url->scheme)) ||
                    (state_override < 0 && (c == '?' || c == '#'))) {
                    const char *b = buffer.p ? buffer.p : "";
                    if (seg_is_dotdot(b)) {
                        path_shorten(url);
                        if (c != '/' && !(c == '\\' && url_scheme_is_special(url->scheme)))
                            path_push(url, "", 0);
                    } else if (seg_is_dot(b) && c != '/' &&
                               !(c == '\\' && url_scheme_is_special(url->scheme))) {
                        path_push(url, "", 0);
                    } else if (!seg_is_dot(b)) {
                        if (!strcmp(url->scheme, "file") && url->npath == 0 && is_windows_drive(b, buffer.n)) {
                            char fixed[3] = { b[0], ':', 0 };
                            path_push(url, fixed, 2);
                        } else {
                            path_push(url, b, buffer.n);
                        }
                    }
                    ustr_free(&buffer);
                    if (c == '?') { free(url->query); url->query = xstrdup(""); state = ST_QUERY; }
                    else if (c == '#') { free(url->fragment); url->fragment = xstrdup(""); state = ST_FRAGMENT; }
                } else {
                    ustr_put_encoded(&buffer, (const char *)&(char){ (char)c }, 1, URL_SET_PATH);
                }
                break;

            case ST_OPAQUE_PATH:
                if (c == '?') {
                    free(url->query); url->query = xstrdup("");
                    state = ST_QUERY;
                } else if (c == '#') {
                    free(url->fragment); url->fragment = xstrdup("");
                    state = ST_FRAGMENT;
                } else if (c != -1) {
                    UStr t;
                    ustr_init(&t);
                    ustr_puts(&t, url->opaque_path ? url->opaque_path : "");
                    /* §4.4's ONE special case in this state: a SPACE becomes `%20` when what REMAINS starts
                       with `?` or `#`, and stays a literal space otherwise. The C0 control set does not encode
                       space, so without this `non-special:opaque  ?hi` kept both spaces literal — and a
                       trailing space before a terminator is exactly what re-parsing would strip. */
                    if (c == ' ' && pointer + 1 < len && (input[pointer + 1] == '?' || input[pointer + 1] == '#'))
                        ustr_puts(&t, "%20");
                    else
                        ustr_put_encoded(&t, (const char *)&(char){ (char)c }, 1, URL_SET_C0);
                    free(url->opaque_path);
                    url->opaque_path = ustr_take(&t);
                }
                break;

            case ST_QUERY:
                /* Same rule: `#` ends the query only without a state override, so `u.search = "#"` encodes it. */
                if ((state_override < 0 && c == '#') || c == -1) {
                    UStr t;
                    int set = url_scheme_is_special(url->scheme) ? URL_SET_SPECIAL_QUERY : URL_SET_QUERY;
                    ustr_init(&t);
                    ustr_puts(&t, url->query ? url->query : "");
                    ustr_put_encoded(&t, buffer.p ? buffer.p : "", buffer.n, set);
                    free(url->query);
                    url->query = ustr_take(&t);
                    ustr_free(&buffer);
                    if (c == '#') { free(url->fragment); url->fragment = xstrdup(""); state = ST_FRAGMENT; }
                } else {
                    ustr_putc(&buffer, (char)c);
                }
                break;

            case ST_FRAGMENT:
                if (c != -1) {
                    UStr t;
                    ustr_init(&t);
                    ustr_puts(&t, url->fragment ? url->fragment : "");
                    ustr_put_encoded(&t, (const char *)&(char){ (char)c }, 1, URL_SET_FRAGMENT);
                    free(url->fragment);
                    url->fragment = ustr_take(&t);
                }
                break;

            default:
                DFAIL("the URL parser reached a state it does not define");
            }
        }
    next_char: ;
    }
done_ok:
    ok = true;

fail:
    ustr_free(&buffer);
    free(input);
    /* A SETTER THAT FAILS LEAVES THE URL ALONE. §6.1's setters "return" on a parse failure, so wiping the
       record here would turn a no-op into a destroyed URL — only a fresh parse owns what it built. And HTML
       §7.2.4's setters depend on it twice over: each of them parses into a COPY of the Location's url, so a
       refusal that wiped the copy would navigate to an empty URL rather than doing nothing. */
    if (!ok && state_override < 0) {
        url_record_free(url);
        url_record_init(url);
    }
    return ok;
}

bool url_parse(UrlRecord *out, const char *input, size_t len, const UrlRecord *base)
{
    return url_parse_into(out, input, len, base, -1);
}

/* §4.4's STATE OVERRIDE, NAMED BY THE STATE RATHER THAN NUMBERED — the entry another standard reaches this
   parser through. The numbering above is this file's own and stays that way; what crosses the header is the
   spec's vocabulary, so a caller writes the state its own section names ("with copyURL as url and host state
   as state override") and cannot express a state no standard sends it to.
   IT EXISTS BECAUSE THE SETTERS ARE NOT ALL IN THIS FILE. url.h used to publish `url_parse_into` with a raw
   `int state_override` and a comment calling the values "private to url.c, which is where the setters live" —
   a header that hands out a numbering while saying it is not public, and a claim the URL Standard itself
   contradicts in the scheme state: "This indication of failure is used exclusively by the Location object's
   protocol setter". HTML §7.2.4's setters are the other caller, and they are in core/frame/location.c.
   The mapping is a SWITCH and not a table, so a value added to one enum and not the other is a compile
   diagnostic on the missing case rather than a row that reads as the wrong state. */
bool url_parse_override(UrlRecord *url, const char *input, size_t len, UrlStateOverride state)
{
    int st;

    switch (state) {
    case URL_OVERRIDE_SCHEME_START: st = ST_SCHEME_START; break;
    case URL_OVERRIDE_HOST:         st = ST_HOST;         break;
    case URL_OVERRIDE_HOSTNAME:     st = ST_HOSTNAME_OVERRIDE; break;
    case URL_OVERRIDE_PORT:         st = ST_PORT;         break;
    case URL_OVERRIDE_PATH_START:   st = ST_PATH_START;   break;
    case URL_OVERRIDE_QUERY:        st = ST_QUERY;        break;
    case URL_OVERRIDE_FRAGMENT:     st = ST_FRAGMENT;     break;
    default:
        DFAIL("§4.4's basic URL parser was entered at a state override no standard names — the public enum is "
              "the seven states other specifications invoke this parser at, so a value outside it is a caller "
              "inventing an entry point rather than citing one");
        return false;
    }
    /* §4.4's `base` is not passed with a state override and never can be: every one of the seven states above
       is reached with the record ALREADY built, and a base is what a state before them would have consumed. */
    return url_parse_into(url, input, len, NULL, st);
}



/* ---- §5.1's `application/x-www-form-urlencoded` list ------------------------------------------------------
 *
 * It is the URL STANDARD's §5.1, not URLSearchParams' — that interface is one view over it, and `.formData()`
 * is the other: a Request or a Response whose Content-Type says urlencoded parses its body with exactly this
 * parser. It lived inside url_search_params.c, which is where it was first needed; a second copy for FormData
 * would have been a second place for `+` decoding, the `%` rules and the code-unit ordering to drift. */
char *url_encoded_strdup(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    CHECK(r, "url: OOM copying a pair");
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* §6.2's sort(): "sorting in ascending order this's list, with a being less than b if a's name is CODE UNIT
   less than b's name." A name is stored here as UTF-8, so the comparison walks UTF-16 CODE UNITS over UTF-8
   bytes — and a code unit is not a code point. The two orders disagree for exactly one range: a supplementary
   code point (U+10000 and up) is a SURROGATE PAIR in UTF-16, so it begins at 0xD800..0xDBFF — BELOW
   U+E000..U+FFFF — while its UTF-8 encoding starts with 0xF0 and sorts above theirs. `ﬃ` (U+FB03) and `🌈`
   (U+1F308) are that case.
   THE LOW SURROGATE IS A UNIT TOO, and yielding only the high one is what this cursor used to do: every pair
   of code points inside ONE high-surrogate block then compared EQUAL, so `?🌉=1&🌈=2` — U+1F309 and U+1F308,
   both 0xD83C first — sorted as a tie and a stable sort left them in input order where the standard swaps
   them. So the cursor holds the second unit and hands it over on the next turn. */
typedef struct { size_t i; uint32_t low; } Utf16Cursor;

static bool url_utf16_done(const Utf16Cursor *c, size_t n) { return c->i >= n && c->low == 0; }

static uint32_t url_encoded_next_unit(const char *s, size_t n, Utf16Cursor *c)
{
    unsigned char ch;
    uint32_t cp;
    int extra;

    if (c->low) { uint32_t u = c->low; c->low = 0; return u; }
    DCHECK(c->i < n, "a UTF-16 code unit was asked for past the end of a name — every walk over one is bounded "
                     "by url_utf16_done, which counts the pending low surrogate as well as the bytes");
    ch = (unsigned char)s[c->i];
    if (ch < 0x80)                { cp = ch;         extra = 0; }
    else if ((ch & 0xe0) == 0xc0) { cp = ch & 0x1f;  extra = 1; }
    else if ((ch & 0xf0) == 0xe0) { cp = ch & 0x0f;  extra = 2; }
    else                          { cp = ch & 0x07;  extra = 3; }
    c->i++;
    while (extra-- > 0 && c->i < n && ((unsigned char)s[c->i] & 0xc0) == 0x80)
        cp = (cp << 6) | ((unsigned char)s[c->i++] & 0x3f);
    if (cp < 0x10000)
        return cp;
    c->low = 0xdc00 + ((cp - 0x10000) & 0x3ff);
    return 0xd800 + ((cp - 0x10000) >> 10);
}

int url_encoded_name_cmp(const UrlEncodedPair *a, const UrlEncodedPair *b)
{
    Utf16Cursor ca = { 0, 0 }, cb = { 0, 0 };
    while (!url_utf16_done(&ca, a->nlen) && !url_utf16_done(&cb, b->nlen)) {
        uint32_t ua = url_encoded_next_unit(a->name, a->nlen, &ca);
        uint32_t ub = url_encoded_next_unit(b->name, b->nlen, &cb);
        if (ua != ub) return ua < ub ? -1 : 1;
    }
    /* a shorter name that is a prefix of a longer one sorts first */
    return !url_utf16_done(&ca, a->nlen) ? 1 : (!url_utf16_done(&cb, b->nlen) ? -1 : 0);
}

void url_encoded_list_free(UrlEncodedList *l)
{
    int i;
    for (i = 0; i < l->n; i++) { free(l->e[i].name); free(l->e[i].value); }
    free(l->e);
    l->e = NULL; l->n = l->cap = 0;
}

void url_encoded_list_append(UrlEncodedList *l, const char *name, size_t nn, int nhole,
                             const char *value, size_t vn, int vhole)
{
    /* WHAT A HOLE HALF MUST SATISFY IS THAT IT IS MEASURABLE, AND NOT THAT IT IS BRACED. This asserted a brace
       first, on the reasoning that a display shape carries its provenance in braces — and that is true of a
       DECLARED source only. `concolic_source_wrap` requires the braced spelling exactly where
       `concolic_source_encodes(src)` answers, i.e. for a source with a delivery declaration; a modelled
       environment value is minted with a bare dotted path (`navigator.language`), and a derivation over one
       composes on that (`navigator.language.toLowerCase()`). So the brace test would have aborted on the very
       call this whole mechanism was built for, which is what a condition written from a rule rather than from
       the values that reach it does.
       WHAT §5.2 ACTUALLY NEEDS HERE is that the half copies verbatim and measures with strlen, so U+0000 is
       the invariant and it is asserted where the half is BORN rather than where it is written out. */
    DCHECK(!nhole || (name && memchr(name, 0, nn) == NULL),
           "an urlencoded pair's NAME was appended as a hole containing U+0000 — a hole is a concolic's "
           "display shape, and §5.2 copies one verbatim and measures it with strlen, so an embedded NUL would "
           "truncate the half. A real USVString may contain U+0000 and must not be labelled a hole");
    DCHECK(!vhole || (value && memchr(value, 0, vn) == NULL),
           "an urlencoded pair's VALUE was appended as a hole containing U+0000 — a hole is a concolic's "
           "display shape, and §5.2 copies one verbatim and measures it with strlen, so an embedded NUL would "
           "truncate the half. A real USVString may contain U+0000 and must not be labelled a hole");
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->e = realloc(l->e, (size_t)l->cap * sizeof(UrlEncodedPair));
        CHECK(l->e, "url: OOM growing the list");
    }
    l->e[l->n].name = url_encoded_strdup(name, nn);
    l->e[l->n].value = url_encoded_strdup(value, vn);
    l->e[l->n].nlen = nn;
    l->e[l->n].vlen = vn;
    l->e[l->n].nhole = nhole ? 1u : 0u;
    l->e[l->n].vhole = vhole ? 1u : 0u;
    l->n++;
}

/* ---- §5.1's application/x-www-form-urlencoded ------------------------------------------------------------ */

/* THE PARSER. Split on `&`; each sequence splits at its FIRST `=` (and is all-name when it has none); `+`
   becomes a space in BOTH halves and only then is the percent-decoding run. Doing the two in the other order
   would decode a `%2B` into a `+` and then turn that into a space, which is a different string.
 *
 * AND THE PERCENT-DECODING IS NOT YET THE STRING. Step 3.5: "Let nameString and valueString be the result of
 * running UTF-8 decode without BOM on the percent-decoding of name and value, respectively." The percent-decode
 * answers BYTES, and `a=%FF` answers one byte that no UTF-8 sequence contains — so the decode is the step that
 * makes them text, one U+FFFD per the standard's "replacement" error mode. Storing the raw bytes was invisible
 * through `get`, whose JS string conversion replaces them the same way, and visible through the SERIALIZER,
 * which re-percent-encoded the byte it had been handed: `new URLSearchParams("a=%FF").toString()` answered
 * `a=%FF` where every browser answers `a=%EF%BF%BD`. */
void url_encoded_parse(UrlEncodedList *out, const char *s, size_t len)
{
    size_t i = 0;
    while (i <= len) {
        size_t start = i, eq = (size_t)-1, end;
        while (i < len && s[i] != '&') {
            if (s[i] == '=' && eq == (size_t)-1) eq = i;
            i++;
        }
        end = i;
        i++;   /* past the '&' */
        if (end == start) { if (end >= len) break; else continue; }   /* an empty sequence is skipped */
        {
            const char *nb = s + start, *vb = s + end;
            size_t nn = (eq == (size_t)-1) ? end - start : eq - start;
            size_t vn = (eq == (size_t)-1) ? 0 : end - eq - 1;
            char *nplus = url_encoded_strdup(nb, nn), *vplus;
            char *ndec, *vdec, *nstr, *vstr;
            size_t ndn = 0, vdn = 0, nsn = 0, vsn = 0, k;
            if (eq != (size_t)-1) vb = s + eq + 1;
            vplus = url_encoded_strdup(vb, vn);
            for (k = 0; k < nn; k++) if (nplus[k] == '+') nplus[k] = ' ';
            for (k = 0; k < vn; k++) if (vplus[k] == '+') vplus[k] = ' ';
            ndec = url_percent_decode(nplus, nn, &ndn);
            vdec = url_percent_decode(vplus, vn, &vdn);
            nstr = encoding_utf8_decode_without_bom(ndec, ndn, &nsn);   /* step 3.5 */
            vstr = encoding_utf8_decode_without_bom(vdec, vdn, &vsn);
            url_encoded_list_append(out, nstr, nsn, 0, vstr, vsn, 0);   /* bytes off the wire are always data */
            free(nplus); free(vplus); free(ndec); free(vdec); free(nstr); free(vstr);
        }
        if (end >= len) break;
    }
}

/* §5.2 application/x-www-form-urlencoded serializing. `name=value` joined by `&`, each DATA half through the
   urlencoded encode set — whose own rule writes SPACE as `+`, which is why that is in the set and not here.
 *
 * A HOLE HALF IS NOT ENCODED, and that is a statement about what a hole IS rather than an exception to §5.2.
 * The steps run over a tuple of USVStrings the page produced; a hole is this engine's display SHAPE for a
 * value the code did not compute, put into the list by §6.2's members because a `const char *` cannot carry a
 * concolic. Percent-encoding it spells the shape's own punctuation away — the braces of a declared source, the
 * call parentheses of a derivation over an undeclared one — and the result reaches the @H surface as a
 * parameter value nothing constrained, which §Solver-half calls a wrong report and not a partial one. See
 * UrlEncodedPair for which shape loses what. */
char *url_encoded_serialize(const UrlEncodedList *l, size_t *out_n)
{
    char *out = NULL;
    size_t cap = 0, n = 0;
    int i;

    for (i = 0; i < l->n; i++) {
        char *en, *ev;
        size_t need;
        /* §5.2 step 3.1, WORD FOR WORD: "Assert: tuple's name and tuple's value are scalar value strings."
           It is the serializer's assert and it is about its PRODUCERS — every one of them has to make it hold,
           which §5.1's parser does by running UTF-8 decode without BOM and §6.2's members do by taking
           USVString. It could not be written while the parser stored the raw percent-decoded bytes, because
           the bytes of `a=%FF` are not a scalar value string and this is the line that would have said so.
           A HOLE IS ASSERTED TOO: a display shape is ASCII, so it is a scalar value string like any other and
           there is nothing here for the hole bit to exempt. */
        DCHECK(encoding_is_scalar_value_string(l->e[i].name, l->e[i].nlen) &&
               encoding_is_scalar_value_string(l->e[i].value, l->e[i].vlen),
               "§5.2's assert: an urlencoded tuple's name or value is not a scalar value string — the producer "
               "of this list skipped UTF-8 decode without BOM");
        en = l->e[i].nhole ? url_encoded_strdup(l->e[i].name,  l->e[i].nlen)
                           : url_percent_encode(l->e[i].name,  l->e[i].nlen, URL_SET_URLENCODED);
        ev = l->e[i].vhole ? url_encoded_strdup(l->e[i].value, l->e[i].vlen)
                           : url_percent_encode(l->e[i].value, l->e[i].vlen, URL_SET_URLENCODED);
        need = strlen(en) + strlen(ev) + 3;
        if (n + need > cap) {
            cap = (n + need) * 2;
            out = realloc(out, cap);
            CHECK(out, "url: OOM serializing");
        }
        if (i) out[n++] = '&';
        memcpy(out + n, en, strlen(en)); n += strlen(en);
        out[n++] = '=';
        memcpy(out + n, ev, strlen(ev)); n += strlen(ev);
        out[n] = 0;
        free(en); free(ev);
    }
    if (!out) { out = malloc(1); CHECK(out, "url: OOM"); out[0] = 0; }
    if (out_n) *out_n = n;
    return out;
}


/* ---- §5's URL interface ------------------------------------------------------------------------------------
 *
 * The interface is THIN over the record on purpose: every member is a serializer or a re-parse of one
 * component, and the algorithms live above where the spec states them once. A setter is not a field write —
 * `u.protocol = "https"` re-runs the basic URL parser in that component's state override and does nothing at
 * all when the result would be invalid, which is why they all route through the same parse. */

static JSClassID g_url_class;
static JSRuntime *g_url_rt;
static int       g_url_ctor_stepid = -1;

/* The wrapper holds the record AND the [SameObject] `searchParams` — §5.1 declares that attribute SameObject,
   so the object is built at most once per URL and the two hold each other (the params write §6.1's update
   steps back onto this record). A real cycle, which is what gc_mark is for. */
typedef struct { UrlRecord rec; JSValue params; } UrlObj;

static void url_finalizer(JSRuntime *rt, JSValue val)
{
    UrlObj *u = JS_GetOpaque(val, g_url_class);
    if (u) { url_record_free(&u->rec); JS_FreeValueRT(rt, u->params); free(u); }
}

static void url_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    UrlObj *u = JS_GetOpaque(val, g_url_class);
    if (u) JS_MarkValue(rt, u->params, mark_func);
}

UrlRecord *url_record_of(JSValueConst v)
{
    UrlObj *u = JS_GetOpaque(v, g_url_class);
    return u ? &u->rec : NULL;
}

static UrlRecord *url_of(JSContext *ctx, JSValueConst v)
{
    UrlObj *u = JS_GetOpaque(v, g_url_class);
    if (!u) JS_ThrowTypeError(ctx, "not a URL");
    return u ? &u->rec : NULL;
}

static JSValue url_wrap(JSContext *ctx, UrlRecord *rec)
{
    UrlObj *held;
    JSValue proto = JS_GetClassProto(ctx, g_url_class);
    JSValue obj;

    DCHECK(!JS_IsNull(proto), "a URL was minted in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_url_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;
    held = malloc(sizeof *held);
    CHECK(held, "url: OOM holding a URL record");
    held->rec = *rec;
    held->params = JS_UNDEFINED;
    url_record_init(rec);   /* the wrapper owns it now */
    JS_SetOpaque(obj, held);
    return obj;
}


/* §5.1's `searchParams`, [SameObject]: built on the first read from this URL's query and held afterwards, so
   `u.searchParams === u.searchParams` and a mutation of it is a mutation of the URL. */
static JSValue js_url_get_params(JSContext *ctx, JSValueConst this_val, int magic)
{
    UrlObj *u = JS_GetOpaque(this_val, g_url_class);
    (void)magic;
    if (!u) return JS_ThrowTypeError(ctx, "not a URL");
    if (JS_IsUndefined(u->params)) {
        u->params = usp_new(ctx, this_val, u->rec.query, u->rec.query ? strlen(u->rec.query) : 0);
        if (JS_IsException(u->params)) { u->params = JS_UNDEFINED; return JS_EXCEPTION; }
    }
    return JS_DupValue(ctx, u->params);
}

/* §5.1: `href` and `search` both re-initialise the query, and §6.1 says the associated params object is
   re-initialised with it — otherwise `u.search = "?b=2"` would leave `u.searchParams` reporting the old pairs. */
static void url_params_resync(JSContext *ctx, JSValueConst this_val)
{
    UrlObj *u = JS_GetOpaque(this_val, g_url_class);
    if (!u || JS_IsUndefined(u->params)) return;
    usp_reset(ctx, u->params, u->rec.query, u->rec.query ? strlen(u->rec.query) : 0);
}

/* §4.4's PER-MEMBER READ, over a bare record. It is public because §4.6.3's HTMLHyperlinkElementUtils —
   `a.protocol`, `area.hostname`, and the rest — is the SAME eleven algorithms over a URL that happens to live
   in an element's href attribute instead of in a URL object. Two copies of these would be two answers to
   "what is this URL's host", and the second one is always the one that is subtly wrong.
   Returns an OWNED string. */
char *url_member_get(const UrlRecord *u, int magic)
{
    char *s;

    switch (magic) {
    case URL_HREF:     s = url_serialize(u, false); break;
    case URL_ORIGIN:   s = url_serialize_origin(u); break;
    case URL_PROTOCOL: {
        UStr o; ustr_init(&o); ustr_puts(&o, u->scheme); ustr_putc(&o, ':'); s = ustr_take(&o); break;
    }
    case URL_USERNAME: s = xstrdup(u->username); break;
    case URL_PASSWORD: s = xstrdup(u->password); break;
    case URL_HOST:     s = url_serialize_host_port(u); break;
    case URL_HOSTNAME: s = url_serialize_host(&u->host); break;
    case URL_PORT:     s = url_serialize_port(u); break;
    case URL_PATHNAME: s = url_serialize_path(u); break;
    case URL_SEARCH:
        /* §5.1: the empty query serializes to "" and not to "?", which is what makes `?` round-trip. */
        if (!u->query || !*u->query) return xstrdup("");
        { UStr o; ustr_init(&o); ustr_putc(&o, '?'); ustr_puts(&o, u->query); s = ustr_take(&o); }
        break;
    default:
        DCHECK(magic == URL_HASH, "a URL accessor was declared with a magic this component does not answer");
        if (!u->fragment || !*u->fragment) return xstrdup("");
        { UStr o; ustr_init(&o); ustr_putc(&o, '#'); ustr_puts(&o, u->fragment); s = ustr_take(&o); }
        break;
    }
    return s;
}

static JSValue js_url_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    UrlRecord *u = url_of(ctx, this_val);
    char *s;
    JSValue r;

    if (!u) return JS_EXCEPTION;
    s = url_member_get(u, magic);
    r = JS_NewString(ctx, s);
    free(s);
    return r;
}

/* URL Standard §6.1 URL class — `toJSON()` and the stringifier are both `href`. */
static JSValue js_url_tojson(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return js_url_get(ctx, this_val, URL_HREF);
}

/* THE REAL PARSE, RUN ON THE OPERAND'S OWN EXAMPLE — the concrete half of the concolic triple, shared by every
 * §6.1 URL class entry point that takes an address the page did not determine. §Solver allows exactly one way
 * for an example to propagate: the engine RUNS the real algorithm (§6.1's API URL parser) on the concrete it
 * already holds. Nothing here predicts what running it would have produced, and with no example there is
 * nothing to parse — which is an absent answer, not a guessed one.
 *
 * THREE OUTCOMES, NOT TWO, because canParse's example depends on telling them apart: an example that PARSED
 * (1, `*out` is the record and the caller frees it), an example that DID NOT parse (0 — `canParse` really is
 * false for that concrete), and NO EXAMPLE AT ALL (-1 — this engine knows nothing and must not say false).
 * A `||`-style collapse of the last two into "false" is the defaulted field §Offensive-programming names: a
 * plausible datum indistinguishable from a measurement.
 *
 * `*out` IS OWNED BY THIS FUNCTION UNLESS IT RETURNS 1. url_parse fills the record whether or not it succeeds
 * — which is why every other caller in this file frees it on the failure path too — so the failure is released
 * here and the caller is left with exactly one rule: free on 1, touch nothing otherwise. Splitting that rule
 * across the callers is how the version this replaced leaked a record on every address that did not parse.
 *
 * THE BASE IS AN OPERAND LIKE THE ADDRESS, AND THIS USED TO DROP IT WHENEVER IT WAS UNKNOWN. The rule it
 * stated for doing so — "an unknown base has no bytes to parse either" — is false of this engine's own value
 * class: §Solver-half's triple is a domain AND a concrete EXAMPLE, and a source mints one (`location.search`
 * carries the query the document was actually loaded with). What the old line described is a source with no
 * example, which is the -1 arm below and is a different state. Dropping a base that HAS one does not decline
 * to answer, it answers WRONG: §4.4's basic parser fails outright on a relative input with no base, so
 * `URL.canParse(rel, unknownBase)` came back a computed-looking `false` and `URL.parse` a computed-looking
 * `null` — from a parse the run never performed. That is the defaulted-field defect exactly, and it is worse
 * here than a missing answer because `if (URL.canParse(x))` is how a bundle spells a validity gate: the false
 * arm is the one the flow then takes.
 * So each operand's BYTES are its example when the page did not determine it and itself when it did — one
 * rule, asked of both, which is also what removes the `!concolic_is(argv[1])` this used to carry. */
static const char *url_operand_bytes(JSContext *ctx, JSValueConst v)
{
    if (JS_IsUndefined(v)) return NULL;
    if (concolic_is(v)) {
        JSValue ex = concolic_example(ctx, v);
        const char *s;

        /* An example is a PRIMITIVE and an address is text, so anything else is a producer that attached a
           value it never computed — and the JS_ToCString below would be a coercion over it. */
        DCHECK(JS_IsString(ex) || JS_IsUndefined(ex),
               "a concolic address or base carries an example that is not a string — an example is what the "
               "engine COMPUTED for this source, and no source in this engine mints a non-string address");
        if (!JS_IsString(ex)) { JS_FreeValue(ctx, ex); return NULL; }
        s = JS_ToCString(ctx, ex);
        JS_FreeValue(ctx, ex);
        return s;
    }
    return JS_ToCString(ctx, v);
}

static int url_example_parse(JSContext *ctx, JSValueConst v, int argc, JSValueConst *argv, UrlRecord *out)
{
    UrlRecord b2;
    bool have_base = false;
    const char *a, *bs2;
    int r = -1;

    DCHECK(concolic_is(v) || (argc > 1 && concolic_is(argv[1])),
           "the example parse was handed a call the page DETERMINED BOTH OPERANDS of — this runs only on the "
           "arm that answers over unknown input, and on a known call it would parse the value twice and hand "
           "the result back as an example OF ITSELF");
    a = url_operand_bytes(ctx, v);
    if (!a) return -1;
    url_record_init(&b2);
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        /* A BASE THE CALL HAS AND THIS ENGINE HAS NO BYTES FOR IS THE -1 ARM, NOT A PARSE WITHOUT ONE, and
           getting that wrong is the same fabricated verdict this helper's own three outcomes exist to prevent
           — reached one operand further in. §4.4 FAILS on a relative input with a null base, so parsing
           `rel/path` with the base dropped answers a confident `did not parse` for a call that had a base all
           along: `URL.canParse(rel, unknownBase)` renders that as `false` and the flow takes the arm nothing
           computed. An operand present with no example is this engine knowing NOTHING about the call, which is
           exactly what -1 means and what @H reports as absent. Only an operand the call does not HAVE
           (`argc < 2`, or an explicit undefined) is a genuine null base and parses as one. */
        if (!(bs2 = url_operand_bytes(ctx, argv[1]))) {
            url_record_free(&b2);
            JS_FreeCString(ctx, a);
            return -1;
        }
        have_base = url_parse(&b2, bs2, strlen(bs2), NULL);
        JS_FreeCString(ctx, bs2);
    }
    r = url_parse(out, a, strlen(a), have_base ? &b2 : NULL) ? 1 : 0;
    if (r == 0)
        url_record_free(out);   /* url_parse fills the record either way; only a SUCCESS is handed on */
    url_record_free(&b2);
    JS_FreeCString(ctx, a);
    return r;
}

/* WHICH OPERAND A §6.1 ANSWER IS UNKNOWN BECAUSE OF, asked ONCE for the constructor and both statics because
 * they run one algorithm over one pair of operands and a second copy of this question is a second answer.
 * Returns the operand the result must be derived from, or JS_UNINITIALIZED when the page determined both —
 * which is the concrete path and needs no derivation at all.
 *
 * THE BASE IS AN OPERAND AND WAS NOT BEING TREATED AS ONE. Only `url` was tested for unknown input; an unknown
 * BASE fell through to the coercion below it, which owes C real bytes and can only abort — so `new URL(path,
 * cfg.apiBase)` over server-injected state took the whole document down at the ToString rather than answering.
 * The base decides the result exactly as the address does (§4.4 resolves the input against it), so an answer
 * derived from it keeps the source's identity and a later branch still forks and a later sink still solves.
 *
 * BOTH UNKNOWN IS THE ONE THIS ENGINE CANNOT NAME YET, AND IT CRASHES RATHER THAN PICKING ONE. The result is a
 * function of two sources, and a value carries ONE provenance and ONE delivery root — so answering with either
 * operand's would state one source's percent-encode set and one source's delivery mechanism as the whole
 * constraint on a value that carries bytes from two. §@S is explicit that a shape stating one of its two facts
 * is a WRONG report rather than a partial one, and the wrongness here is the kind that reproduces: the envelope
 * would tell a researcher to deliver through the component the payload did not ride.
 * WHAT TO BUILD is a provenance that names the SET — the joint identity the solver already mints for a value
 * several facts determine — extended so that each MEMBER keeps its own delivery declaration, so the search
 * seeds one candidate per member and the one that fires is emitted with its own envelope. §@S's own rule for
 * this is that a wrong solve simply never fires and is discarded, so trying both is completeness rather than
 * guesswork. Until that exists there is no honest single answer here. */
static JSValueConst url_unknown_operand(int argc, JSValueConst *argv)
{
    int ua = argc > 0 && concolic_is(argv[0]);
    int ub = argc > 1 && concolic_is(argv[1]);

    if (ua && ub)
        DFAIL("§6.1's URL parse was given an unknown ADDRESS and an unknown BASE, so its result is a function "
              "of two attacker sources — and a concolic carries one provenance and one delivery root, so "
              "whichever this answered with would state that source's percent-encode set and delivery "
              "mechanism as the whole constraint on bytes that came from both. Build the SET-valued provenance "
              "(the joint identity, with each member keeping its own delivery declaration) and seed one "
              "candidate per member");
    if (ua) return argv[0];
    if (ub) return argv[1];
    return JS_UNINITIALIZED;
}

/* §6.1 URL class's static `parse` and `canParse` — the constructor's parse without the throw. */
enum { URL_STATIC_PARSE = 0, URL_STATIC_CANPARSE };

static JSValue js_url_static(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    const char *in = NULL, *bs = NULL;
    size_t in_len = 0, bs_len = 0;
    UrlRecord base, rec;
    bool have_base = false, okp;
    JSValue r;
    JSValueConst unk = url_unknown_operand(argc, argv);

    (void)this_val;
    /* §6.1's `static URL? parse(USVString url, ...)` and `static boolean canParse(USVString url, ...)` OVER
       UNKNOWN INPUT. They are the constructor's own algorithm without the throw, so they answer the way the
       constructor answers: a derived unknown that keeps the source's identity, so a later branch still forks
       and a later sink still solves for the original source. The parser underneath owes C real bytes and can
       only abort on a concolic — and `if (URL.canParse(x))` is how a real bundle spells a validity gate, so
       aborting there ends the document at the exact branch the solver exists to fork.
       WHICH OPERAND the answer is derived from is url_unknown_operand's question and not this function's: the
       base decides the result the same way the address does, and asking it here would be a second answer. */
    if (!JS_IsUninitialized(unk)) {
        UrlRecord ex_rec;
        int got = url_example_parse(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, argc, argv, &ex_rec);
        JSValue real = JS_UNDEFINED;

        DCHECK(got >= -1 && got <= 1,
               "the example parse answered with an outcome §6.1 does not have — parsed, did not parse and no "
               "example are the whole space, and a fourth would be read here as one of them");
        DCHECK(magic == URL_STATIC_PARSE || magic == URL_STATIC_CANPARSE,
               "a URL static was installed with a magic this function does not implement — the derived "
               "value's name is composed from it, so an unknown one would label the result as canParse");
        if (magic == URL_STATIC_CANPARSE) {
            /* A BOOLEAN IS THE ANSWER'S TYPE, so the example is the real parse's own verdict — computed, never
               invented. `got < 0` means no example: absent, which is what @H reports rather than a fabricated
               `false`. */
            if (got >= 0) real = JS_NewBool(ctx, got == 1);
        } else if (got == 1) {
            real = url_wrap(ctx, &ex_rec);
            if (JS_IsException(real)) { JS_FreeValue(ctx, JS_GetException(ctx)); real = JS_UNDEFINED; }
        } else if (got == 0) {
            real = JS_NULL;   /* §6.1: parse returns null for an address that does not parse */
        }
        if (got == 1) url_record_free(&ex_rec);
        return concolic_builtin_hook(ctx, unk,
                                     magic == URL_STATIC_CANPARSE ? "URL.canParse" : "URL.parse", real);
    }
    in = JS_ToCStringLen(ctx, &in_len, argc > 0 ? argv[0] : JS_UNDEFINED);
    if (!in) return JS_EXCEPTION;
    url_record_init(&base);
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        bs = JS_ToCStringLen(ctx, &bs_len, argv[1]);
        if (!bs) { JS_FreeCString(ctx, in); return JS_EXCEPTION; }
        have_base = url_parse(&base, bs, bs_len, NULL);
        JS_FreeCString(ctx, bs);
        if (!have_base) {
            JS_FreeCString(ctx, in);
            url_record_free(&base);
            return magic == URL_STATIC_CANPARSE ? JS_NewBool(ctx, false) : JS_NULL;
        }
    }
    okp = url_parse(&rec, in, in_len, have_base ? &base : NULL);
    JS_FreeCString(ctx, in);
    url_record_free(&base);
    if (magic == URL_STATIC_CANPARSE) {
        url_record_free(&rec);
        return JS_NewBool(ctx, okp);
    }
    if (!okp) { url_record_free(&rec); return JS_NULL; }
    r = url_wrap(ctx, &rec);
    url_record_free(&rec);
    return r;
}

/* THE CONSTRUCTOR IS A MACHINE because both of its arguments are USVStrings the page may compute: `new
   URL({toString(){…}})` runs that toString, and running it from C is the drive-to-completion this engine
   aborts on. The declaration converts them; §5.1's own steps are the two parses and the throw. */
/* WHERE THIS MACHINE RESTS. §5.1's constructor is three steps and NONE of them can run the page's code — the
   declaration has already converted both arguments to USVStrings, and what is left is the API URL parser and
   a throw. So the machine has exactly one stage and never returns to it. It carried a `stage` byte nothing
   read: a resume point that did not exist, which is the other half of the defect a private counter has. */
#define URL_CTOR_STAGES(X) \
    X(URL_CTOR_PARSE = IDL_STEP_FIRST, \
      "URL §5.1 new URL(url, base) steps 1-3 (the API URL parser over url and base, the TypeError a failure " \
      "is, and initializing this with the result)")
enum { URL_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const URL_CTOR_STEPS[] = { URL_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { int unused; } JSUrlCtorState;

static void js_url_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int js_url_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    const char *in = NULL, *bs = NULL;
    size_t in_len = 0, bs_len = 0;
    UrlRecord base, rec;
    bool have_base = false;

    (void)st; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(hdr->stage == URL_CTOR_PARSE, "the URL constructor resumed at a stage §5.1 does not have");
    if (JS_IsUndefined(hdr->this_val)) {
        JS_ThrowTypeError(ctx, "constructor URL requires 'new'");
        return -1;
    }
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "URL requires at least 1 argument");
        return -1;
    }
    /* §6.1 URL class's `constructor(USVString url, optional USVString base)` OVER UNKNOWN INPUT. The parser
       needs real bytes and the coercion below owes C them, so a concolic operand can only crash there; the
       constructor answers instead with an unknown derived from the source. Reading `.href`/`.pathname` off the
       result then yields further unknowns tied to the same source, which is what a later branch forks on and a
       later sink solves for.
       WHICH OPERAND, AND THE FACT THAT THE BASE IS ONE, are url_unknown_operand's — the same question the two
       statics ask, asked once. `new URL(rel, base)` resolves the input AGAINST the base (§4.4), so an unknown
       base decides the result exactly as an unknown address does, and this arm used to test only the address:
       an unknown base fell through to the coercion below and took the document down at the ToString.
       The example is the real parse over the operands' own concretes, through the SAME helper §6.1's statics
       use — one algorithm, one answer, and no second copy of it to drift. */
    {
        JSValueConst unk = url_unknown_operand(argc, argv);

        if (!JS_IsUninitialized(unk)) {
            UrlRecord ex_rec;
            JSValue real = JS_UNDEFINED;
            if (url_example_parse(ctx, argv[0], argc, argv, &ex_rec) == 1) {
                real = url_wrap(ctx, &ex_rec);
                url_record_free(&ex_rec);
                if (JS_IsException(real)) { JS_FreeValue(ctx, JS_GetException(ctx)); real = JS_UNDEFINED; }
            }
            *presult = concolic_builtin_hook(ctx, unk, "URL", real);
            return 0;
        }
    }
    in = JS_ToCStringLen(ctx, &in_len, argv[0]);
    if (!in) return -1;
    url_record_init(&base);
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        bs = JS_ToCStringLen(ctx, &bs_len, argv[1]);
        if (!bs) { JS_FreeCString(ctx, in); return -1; }
        have_base = url_parse(&base, bs, bs_len, NULL);
        JS_FreeCString(ctx, bs);
        if (!have_base) {
            JS_FreeCString(ctx, in);
            url_record_free(&base);
            JS_ThrowTypeError(ctx, "the base URL is not a valid URL");
            return -1;
        }
    }
    if (!url_parse(&rec, in, in_len, have_base ? &base : NULL)) {
        JS_FreeCString(ctx, in);
        url_record_free(&base);
        url_record_free(&rec);
        JS_ThrowTypeError(ctx, "the string is not a valid URL");
        return -1;
    }
    JS_FreeCString(ctx, in);
    url_record_free(&base);
    *presult = url_wrap(ctx, &rec);
    url_record_free(&rec);
    return JS_IsException(*presult) ? -1 : 0;
}

static const IdlStepDecl js_url_ctor_decl = {
    js_url_ctor_step, sizeof(JSUrlCtorState), js_url_ctor_visit, NULL,
    "URL §5.1 new URL(url, base)", URL_CTOR_STEPS
};

/* §6.1 "URL class"'s SETTERS — the interface's, and NOT the ones HTML §7.2.4 gives a Location, which
   core/frame/location.c implements separately over the same parser because three of them genuinely differ
   (protocol throws, search parses in the document's encoding, hash has no empty-string case and bails out on
   an unchanged fragment). This comment used to cite §5.1, which is `application/x-www-form-urlencoded`
   parsing — a real section of this standard that this block has nothing to do with, which is the shape of
   wrong citation that reads as authoritative.
   Each is the basic URL parser with a state override plus the component's own preamble, and
   every one of those preambles is a spec step rather than a guard someone thought of: `pathname` on an
   opaque-path URL does nothing, `port` on a URL that cannot have one does nothing, and an empty `search` or
   `hash` sets the component to NULL rather than to the empty string — which is the difference between
   `http://x/` and `http://x/?`. */
/* §4.4's PER-MEMBER WRITE, over a bare record — the setter half of url_member_get and public for the same
   reason. Returns 0, or -1 when `href` was handed something that is not a URL (the caller decides what that
   means: the URL interface throws, §4.6.3's `a.href` does not).
   EVERY OTHER MEMBER IS A NO-OP ON BAD INPUT, which is the spec's own answer — a setter's failure leaves the
   URL alone rather than throwing, so `a.port = "x"` is silently ignored exactly as in a browser. */
int url_member_set(UrlRecord *u, int magic, const char *v, size_t vlen)
{
    switch (magic) {
    case URL_HREF: {
        UrlRecord fresh;
        if (!url_parse(&fresh, v, vlen, NULL)) {
            url_record_free(&fresh);
            return -1;
        }
        url_record_free(u);
        *u = fresh;
        break;
    }
    case URL_PROTOCOL: {
        UStr t;
        ustr_init(&t);
        ustr_put(&t, v, vlen);
        ustr_putc(&t, ':');
        url_parse_override(u, t.p, t.n, URL_OVERRIDE_SCHEME_START);
        ustr_free(&t);
        break;
    }
    case URL_USERNAME:
        if (!url_cannot_have_username_password_port(u)) {
            free(u->username);
            u->username = url_percent_encode(v, vlen, URL_SET_USERINFO);
        }
        break;
    case URL_PASSWORD:
        if (!url_cannot_have_username_password_port(u)) {
            free(u->password);
            u->password = url_percent_encode(v, vlen, URL_SET_USERINFO);
        }
        break;
    case URL_HOST:
        if (!u->opaque_path) url_parse_override(u, v, vlen, URL_OVERRIDE_HOST);
        break;
    case URL_HOSTNAME:
        if (!u->opaque_path) url_parse_override(u, v, vlen, URL_OVERRIDE_HOSTNAME);
        break;
    case URL_PORT:
        if (!url_cannot_have_username_password_port(u)) {
            if (vlen == 0) u->port = -1;
            else url_parse_override(u, v, vlen, URL_OVERRIDE_PORT);
        }
        break;
    case URL_PATHNAME:
        if (!u->opaque_path) {
            while (u->npath) path_pop(u);
            url_parse_override(u, v, vlen, URL_OVERRIDE_PATH_START);
        }
        break;
    case URL_SEARCH:
        if (vlen == 0) {
            free(u->query);
            u->query = NULL;
        } else {
            const char *in = v[0] == '?' ? v + 1 : v;
            size_t n = v[0] == '?' ? vlen - 1 : vlen;
            free(u->query);
            u->query = xstrdup("");
            url_parse_override(u, in, n, URL_OVERRIDE_QUERY);
        }
        break;
    default:
        DCHECK(magic == URL_HASH, "a URL setter was declared with a magic this component does not answer");
        if (vlen == 0) {
            free(u->fragment);
            u->fragment = NULL;
        } else {
            const char *in = v[0] == '#' ? v + 1 : v;
            size_t n = v[0] == '#' ? vlen - 1 : vlen;
            free(u->fragment);
            u->fragment = xstrdup("");
            url_parse_override(u, in, n, URL_OVERRIDE_FRAGMENT);
        }
        break;
    }
    return 0;
}

static JSValue js_url_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    UrlRecord *u = url_of(ctx, this_val);
    const char *v;
    size_t vlen = 0;
    int rc;

    if (!u) return JS_EXCEPTION;
    v = JS_ToCStringLen(ctx, &vlen, val);
    if (!v) return JS_EXCEPTION;
    rc = url_member_set(u, magic, v, vlen);
    JS_FreeCString(ctx, v);
    if (rc < 0) return JS_ThrowTypeError(ctx, "the string is not a valid URL");
    if (magic == URL_HREF || magic == URL_SEARCH)
        url_params_resync(ctx, this_val);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_url_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("href", js_url_get, js_url_set, URL_HREF),
    JS_CGETSET_MAGIC_DEF("origin", js_url_get, NULL, URL_ORIGIN),
    JS_CGETSET_MAGIC_DEF("protocol", js_url_get, js_url_set, URL_PROTOCOL),
    JS_CGETSET_MAGIC_DEF("username", js_url_get, js_url_set, URL_USERNAME),
    JS_CGETSET_MAGIC_DEF("password", js_url_get, js_url_set, URL_PASSWORD),
    JS_CGETSET_MAGIC_DEF("host", js_url_get, js_url_set, URL_HOST),
    JS_CGETSET_MAGIC_DEF("hostname", js_url_get, js_url_set, URL_HOSTNAME),
    JS_CGETSET_MAGIC_DEF("port", js_url_get, js_url_set, URL_PORT),
    JS_CGETSET_MAGIC_DEF("pathname", js_url_get, js_url_set, URL_PATHNAME),
    JS_CGETSET_MAGIC_DEF("search", js_url_get, js_url_set, URL_SEARCH),
    JS_CGETSET_MAGIC_DEF("hash", js_url_get, js_url_set, URL_HASH),
    JS_CGETSET_MAGIC_DEF("searchParams", js_url_get_params, NULL, 0),
    JS_CFUNC_DEF("toJSON", 0, js_url_tojson),
    JS_CFUNC_DEF("toString", 0, js_url_tojson),
};

/* File API §8's two members, DECLARED on the URL interface and DEFINED by File API — so they install here and
   the blob URL store lives with the component that knows what a Blob is. Neither runs the page's code: the
   argument is brand-tested rather than converted, and the store is this engine's own. */
static JSValue js_url_object_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)this_val;
    if (magic == 0) {
        char *url = blob_url_create(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        JSValue r;
        if (!url) return JS_EXCEPTION;
        r = JS_NewString(ctx, url);
        free(url);
        return r;
    }
    {
        size_t len = 0;
        const char *url = JS_ToCStringLen(ctx, &len, argc > 0 ? argv[0] : JS_UNDEFINED);
        if (!url) return JS_EXCEPTION;
        blob_url_revoke(ctx, url, len);
        JS_FreeCString(ctx, url);
        return JS_UNDEFINED;
    }
}

static const JSCFunctionListEntry js_url_static_funcs[] = {
    JS_CFUNC_MAGIC_DEF("parse", 1, js_url_static, URL_STATIC_PARSE),
    JS_CFUNC_MAGIC_DEF("canParse", 1, js_url_static, URL_STATIC_CANPARSE),
    JS_CFUNC_MAGIC_DEF("createObjectURL", 1, js_url_object_url, 0),
    JS_CFUNC_MAGIC_DEF("revokeObjectURL", 1, js_url_object_url, 1),
};

void url_init(JSContext *ctx)
{
    JSClassDef def = { "URL", .finalizer = url_finalizer, .gc_mark = url_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType CTOR_ARGS[2] = { IDL_USVSTRING, IDL_USVSTRING };

    DCHECK(g_url_rt == NULL || g_url_rt == rt,
           "URL was installed into a second runtime — its class id and step id belong to the first, and one "
           "WASM instance is one document");
    if (g_url_rt == rt)
        return;
    g_url_rt = rt;
    JS_NewClassID(rt, &g_url_class);
    JS_NewClass(rt, g_url_class, &def);
    g_url_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, NULL, 0, &js_url_ctor_decl, 0);
    idl_optional_from(1);   /* §5.1: `constructor(USVString url, optional USVString base)` */
    realm_declare_intrinsic(url_install_proto);
}

/* §4.4's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. `searchParams` hands back a URLSearchParams, which is
   minted from THIS realm's class slot, so the two interfaces have to be per-realm together or a URL built in
   one document answers `.searchParams` with an object belonging to another. */
void url_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_url_class != 0, "a realm asked for URL.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_url_class);
    DCHECK(JS_IsNull(prev), "url_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "URL.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "URL");
    JS_SetPropertyFunctionList(ctx, proto, js_url_proto_funcs,
                               (int)(sizeof(js_url_proto_funcs) / sizeof(js_url_proto_funcs[0])));
    JS_SetClassProto(ctx, g_url_class, proto);
}

void url_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_url_ctor_stepid >= 0, "URL was installed before url_init declared its constructor");
    ctor = idl_step_constructor(ctx, "URL", 1, g_url_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the URL interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_url_class);
        DCHECK(!JS_IsNull(proto), "URL was installed into a realm that never ran its proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyFunctionList(ctx, ctor, js_url_static_funcs,
                               (int)(sizeof(js_url_static_funcs) / sizeof(js_url_static_funcs[0])));
    JS_SetPropertyStr(ctx, (JSValue)global, "URL", ctor);
}

void url_free(JSContext *ctx)
{
    /* the prototypes are the REALMS' — released with their contexts */
    g_url_rt = NULL;
    g_url_ctor_stepid = -1;
}
