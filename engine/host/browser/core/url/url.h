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
/* HTML §2.4.1's "a URL MATCHES about:blank" / "matches about:srcdoc" — `what` is "blank" or "srcdoc", and
   `query_must_be_null` is the one place the two relations differ (srcdoc's query must be null, blank's need
   not be). Two askers: HTML §7.3.2.1 "Creating browsing contexts"' determine the origin and §2.4.3's
   fallback base URL. */
bool url_matches_about(const UrlRecord *u, const char *what, bool query_must_be_null);
/* A HOST on its own — released, parsed and compared without a URL record around it, because a host is parsed
   and compared in places that have none. HTML §7.1.1's origin holds two (its host and its domain), and
   §7.1.1.2's `document.domain` setter parses one out of a bare string with no scheme and no base in sight. */
void url_host_free(UrlHost *h);
/* §3.5 Host parsing's HOST PARSER, which is what every standard means by "parsing" a bare host string;
   `is_opaque` is its own `isNotSpecial` parameter and decides between the domain parser and the opaque-host
   parser. Returns false for the spec's FAILURE. `*out` is zeroed either way and is the caller's to url_host_free. */
bool url_parse_host(UrlHost *out, const char *s, size_t len, bool is_opaque);
/* §4.2's HOST EQUALITY, by value over the parsed host — what §7.1.1 step 2, §7.1.1's same origin-domain,
   §7.2.5's can-have-its-URL-rewritten and §7.1.1.2's step 4 each mean by two hosts being "identical". */
bool url_host_equal(const UrlHost *a, const UrlHost *b);
/* A deep copy — the parser needs one because `new URL(input, base)` must not write through to `base`. */
bool url_record_copy(UrlRecord *dst, const UrlRecord *src);

/* §4.4 "basic URL parser". `base` may be NULL. Returns false for the spec's FAILURE, which is what makes the
   URL constructor throw. `out` is left initialised-and-empty on failure, so the caller frees it either way. */
bool url_parse(UrlRecord *out, const char *input, size_t len, const UrlRecord *base);

/* §4.4 with a STATE OVERRIDE — the entry every setter algorithm in every standard is one call to, named by the
   STATE its own section names rather than by a number.
   THE SEVEN ARE THE ONES OTHER STANDARDS INVOKE, and there is no eighth: §6.1's URL setters and HTML §7.2.4's
   Location setters between them reach scheme start, host, hostname, port, path start, query and fragment, each
   written out in the calling step ("Basic URL parse the given value, with copyURL as url and host state as
   state override"). Every other state §4.4 defines is an internal transition of the machine and no section
   sends a caller to one, so the enum is a CLOSED list rather than a subset someone stopped extending — the day
   a standard names an eighth, it arrives here and in the switch that maps it, together.
   THIS USED TO PUBLISH `url_parse_into` WITH A RAW `int state_override`, described in this very comment as
   the parser's own state numbering, private to url.c, which is where the setters live. Both halves were
   wrong at once: a header cannot hand out a numbering and call it private, and the setters are NOT all in url.c —
   §4.4's scheme state says so itself, "This indication of failure is used exclusively by the Location object's
   protocol setter", and that setter is core/frame/location.c's. The function had no caller outside url.c at
   all, so what the header published was an invitation to depend on a numbering nobody had agreed to.
   `base` is gone with it rather than passed as NULL: every one of the seven states is reached with the record
   ALREADY built, so a base is what a state BEFORE them would have consumed and there is no override for which
   one is meaningful.
   ON FAILURE THE RECORD IS LEFT UNTOUCHED, which is what a setter's "return" means — a setter that refuses its
   input must not also destroy the URL. FAILURE IS ONLY EVER OBSERVABLE IN THE SCHEME STATES: §4.4 returns
   failure under an override in exactly two arms, both in scheme start/scheme state, and the standard names the
   one caller that reads it. */
typedef enum {
    URL_OVERRIDE_SCHEME_START,   /* §4.4 scheme start state — the only override whose failure is observable */
    URL_OVERRIDE_HOST,           /* §4.4 host state */
    URL_OVERRIDE_HOSTNAME,       /* §4.4 hostname state — host state refusing the `:` that would start a port */
    URL_OVERRIDE_PORT,           /* §4.4 port state */
    URL_OVERRIDE_PATH_START,     /* §4.4 path start state */
    URL_OVERRIDE_QUERY,          /* §4.4 query state */
    URL_OVERRIDE_FRAGMENT,       /* §4.4 fragment state */
} UrlStateOverride;
bool url_parse_override(UrlRecord *url, const char *input, size_t len, UrlStateOverride state);

/* §4.2 "URL miscellaneous": "A URL cannot have a username/password/port if its host is null or the empty
   string, or its scheme is "file"." The preamble of §6.1's and HTML §7.2.4's port setters both, verbatim.
   THIS DECLARATION SAID "the empty host" WHERE THE STANDARD SAYS "the empty string", and its definition in
   url.c had already been repaired — one sentence quoted at two sites, one of them corrected, which is the
   state a per-site check cannot report and a diff of the siblings finds for nothing. The word matters for the
   reason recorded there: the EMPTY HOST is §4.1's own named host value, so the mis-transcription read as a
   claim about a host variant rather than about a host that serializes to nothing. */
bool url_cannot_have_username_password_port(const UrlRecord *u);

/* §4.5 "URL serializer" — the whole record, or every part of it. Each returns a malloc'd string. */
/* §4.4's ELEVEN MEMBERS, by index. Public because §4.6.3's HTMLHyperlinkElementUtils is the same eleven
   algorithms over a URL that lives in an element's href attribute rather than in a URL object. */
enum { URL_HREF = 0, URL_ORIGIN, URL_PROTOCOL, URL_USERNAME, URL_PASSWORD, URL_HOST, URL_HOSTNAME,
       URL_PORT, URL_PATHNAME, URL_SEARCH, URL_HASH };
/* Read one member. Returns an OWNED string. */
char *url_member_get(const UrlRecord *u, int member);
/* Write one member, per §4.4's setter algorithms. 0, or -1 when `href` was not a URL — every other member is a
   NO-OP on input it cannot use, which is the spec's answer rather than a throw. */
int url_member_set(UrlRecord *u, int member, const char *v, size_t vlen);

char *url_serialize(const UrlRecord *u, bool exclude_fragment);
char *url_serialize_host(const UrlHost *h);
/* `host` (the host with its port when there is one) and `port` (empty when null) as both URL and Location read
   them — one serialization, because the two interfaces state the same one. */
char *url_serialize_host_port(const UrlRecord *u);
char *url_serialize_port(const UrlRecord *u);
/* §4.7's `origin`, SERIALIZED: the tuple origin for a scheme that has one, and the string "null" for every
   other. THE BYTES, never a principal — an origin is a record with an identity (core/url/origin.h) and every
   same-origin decision is made on that record; this is what `URL.origin` and `location.origin` return. */
char *url_serialize_origin(const UrlRecord *u);
/* The path, as `pathname` reports it: the opaque path, or "/" joined segments. */
char *url_serialize_path(const UrlRecord *u);

/* §4.2's special schemes and their default ports — the whole of what "special" means. -1 when the scheme has
   no default port (which is `file`, the one special scheme with none). */
bool url_scheme_is_special(const char *scheme);
int  url_default_port(const char *scheme);

/* FETCH §2.1 "URL", verbatim: "A local scheme is `about`, `blob`, or `data`. A URL is local if its scheme is a
   local scheme." It sits beside §4.2's `special` because it is the same KIND of question — a membership test
   the parser's lowercased scheme is the only legitimate input to — and it is exported because its callers are
   not fetches at all: HTML §7.1.7's determine-navigation-params-policy-container is written over "responseURL
   is local", and that predicate is what decides whether a Document is judged under its own response's policy
   or under the CLONE of its creator's. Asking it of the raw address instead (a `strncmp(url, "about:", 6)`)
   answers `about` and misses `data:` and `blob:` — which are exactly the two schemes whose Document has an
   OPAQUE origin, and therefore exactly the case CSP §2.2's self-origin note is written about. */
bool url_scheme_is_local(const char *scheme);

/* FETCH §2.1 "URL", the next sentence: "An HTTP(S) scheme is `http` or `https`." Beside `local` because it is
   the same section and the same kind of membership test. HTML §7.2.4's protocol setter is what asks: the step
   that TERMINATES a protocol set whose result is not http(s) is why `location.protocol = "data"` changes
   nothing rather than throwing. */
bool url_scheme_is_http_s(const char *scheme);

/* §5.2 application/x-www-form-urlencoded serializing's URLENCODED SERIALIZER encode set, exported because
   URLSearchParams is the other user of it. */
char *url_percent_encode(const char *s, size_t len, int set);
/* §1.3 "percent-decode". IT ANSWERS A BYTE SEQUENCE, NOT A STRING — "let output be an empty byte sequence …
   append byte to output" — and `%XX` reaches all 256, so the result is arbitrary bytes whatever the input was.
   `*out_n` is that sequence's LENGTH, which is not strlen when the input decodes a %00.
   WHETHER A DECODE FOLLOWS IS THE CALLING STANDARD'S STEP, and both answers are live in this tree: URL §3.5's
   host parser and §5.1's urlencoded parser want a STRING and run Encoding §6's UTF-8 decode without BOM on
   this (the standard's own note here: "using anything but UTF-8 decode without BOM when input contains bytes
   that are not ASCII bytes might be insecure"), HTML §7.4.2.3.2's javascript: URL wants one and runs §6's
   UTF-8 decode, while CSP §6.7.2.12's path comparison and a data URL's body are byte sequences to their own
   standards and are DONE here. Skipping an owed decode does not fail loudly: it hands the next component bytes
   it will read as text — an overlong `%C1%A1` spelled the host `a.com` until §3.5 ran its decode. */
char *url_percent_decode(const char *s, size_t len, size_t *out_n);

/* The RECORD behind a `URL` wrapper, or NULL when `v` is not one — how URLSearchParams writes §6.2
   URLSearchParams class's update steps back onto the URL it belongs to. */
UrlRecord *url_record_of(JSValueConst v);
enum { URL_SET_C0 = 0, URL_SET_FRAGMENT, URL_SET_QUERY, URL_SET_SPECIAL_QUERY, URL_SET_PATH,
       URL_SET_USERINFO, URL_SET_COMPONENT, URL_SET_URLENCODED };

/* The `application/x-www-form-urlencoded` LIST — the URL Standard's (§5 application/x-www-form-urlencoded),
   not URLSearchParams'. That interface is one view over it and `.formData()` is the other, so it lives with
   the spec that defines it.
   A pair carries its LENGTHS because a name or a value may contain U+0000: `?a=b%00c` is one pair whose value
   is three characters, and a strlen would make it one.
   AND EACH HALF CARRIES WHETHER IT IS A HOLE RATHER THAN DATA. §6.2's members take USVStrings and this list
   holds their BYTES, so an unknown reaches it as its display SHAPE — a NAME for a value the code did not
   compute, never bytes the page produced. §5.2 application/x-www-form-urlencoded serializing runs every half
   through §1.3 Percent-encoded bytes' urlencoded set, and the shape's own punctuation is IN that set: `{` and
   `}` enter at the path set and are inherited by the component and urlencoded sets, while `(`, `)`, `!`, `'`
   and `~` are the urlencoded set's own additions. So a shape serialized as data is spelled out of existence in
   BOTH of the two forms a shape comes in, and they lose different things:
     - a DECLARED attacker source is BRACED (`{location.hash}` — concolic_source_wrap requires that spelling
       exactly where concolic_source_encodes answers), and encoding gives `%7Blocation.hash%7D`. The brace is
       the only thing an emission has to read a hole by (solver/concolic.c's concolic_hole_key returns NULL
       without one), so the parameter loses its DOMAIN as well as its provenance.
     - an UNDECLARED source is minted with a bare dotted path (`navigator.language`) and a derivation composes
       on that, so `navigator.language.toLowerCase()` encodes to `navigator.language.toLowerCase%28%29` —
       provenance still legible to a human, and mangled.
   Either way the surface reports a parameter value no run computed, which is a plausible datum and not a
   measurement. A hole half is therefore emitted verbatim; see the serializer. */
typedef struct { char *name, *value; size_t nlen, vlen; unsigned nhole : 1, vhole : 1; } UrlEncodedPair;
typedef struct { UrlEncodedPair *e; int n, cap; } UrlEncodedList;

void  url_encoded_list_free(UrlEncodedList *l);
/* `nhole`/`vhole` say that half's bytes are a display SHAPE and not data — see UrlEncodedPair. Bytes that came
   off the wire are always data, so §5.1's parser passes 0 for both and only §6.2's members ever pass 1. */
void  url_encoded_list_append(UrlEncodedList *l, const char *name, size_t nn, int nhole,
                              const char *value, size_t vn, int vhole);
/* §5.1 application/x-www-form-urlencoded parsing: split on `&`, split each sequence at its FIRST `=`, turn `+`
   into a space in BOTH halves and only then percent-decode — the other order would decode a `%2B` into a `+`
   and then into a space. */
void  url_encoded_parse(UrlEncodedList *out, const char *s, size_t len);
/* §5.2 application/x-www-form-urlencoded serializing: `name=value` joined by `&`, each DATA half through the
   urlencoded encode set and each HOLE half verbatim. */
char *url_encoded_serialize(const UrlEncodedList *l, size_t *out_n);
/* §6.2's ordering, which is by UTF-16 CODE UNITS and not by UTF-8 bytes. */
int   url_encoded_name_cmp(const UrlEncodedPair *a, const UrlEncodedPair *b);
char *url_encoded_strdup(const char *s, size_t n);

/* URL §6.1 "URL class" — the `URL` interface. This line cited §5, which is "application/x-www-form-urlencoded"
   and defines a serializer over a byte sequence rather than any interface; §6 "API" is where this standard puts
   its two classes. */
void url_init(JSContext *ctx);
/* §6.1's prototype, its Web IDL §3.7.1 interface object, and the §3.8 property references for `URL` and
   `webkitURL` — for ONE realm, declared into core/realm.h's list. ONE entry because §3.8 `define the global
   property references` is given "target" and "a realm realm" and its step 1 population is "every interface
   that is exposed in realm": no Document appears in it. §6.1 declares `[Exposed=*]`, so EVERY realm owes the
   name — and while the interface object was installed from core/platform.c's per-document column, a worker
   realm, which reaches no platform_document_install, received none of it. (This declaration cited §4.4, which
   under this project's standard is URL §4.4 URL parsing — an algorithm over a string with nothing to say
   about an interface's prototype; the concept is Web IDL's and is named above.) */
void url_install_realm(JSContext *ctx);
void url_free(JSContext *ctx);

#endif
