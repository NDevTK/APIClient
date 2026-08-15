/* ORIGIN — HTML §7.1.1's record and its algorithms, over URL §4.7's construction. See origin.h for why this is
 * a record with an identity rather than the string this engine used to compare. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/url/origin.h"

/* §7.1.1's TWO SHAPES IN ONE RECORD, and `nonce` is which: NON-ZERO is the OPAQUE origin — "an internal value,
   with no serialization it can be recreated from … for which the only meaningful operation is testing for
   equality", and the nonce IS that value — while ZERO is the TUPLE origin, whose four components are below.
   A tuple origin's components are never read off an opaque one, which origin_is_opaque exists to ask first. */
struct Origin {
    uint64_t nonce;
    char    *scheme;      /* §7.1.1's scheme (an ASCII string) — tuple only */
    char    *host;        /* §7.1.1's host, SERIALIZED (see origin_same) — tuple only */
    int      port;        /* §7.1.1's port; -1 is the spec's null — tuple only */
    /* §7.1.1's DOMAIN: "null unless stated otherwise", and §7.1.1.2's `document.domain` setter is the only
       thing in the platform that states otherwise. This engine does not have that member — a web API it has
       not built is honestly ABSENT — so every origin here has a null domain, and the day the setter lands it
       writes HERE. It is a field rather than an omission because §7.1.1's SAME ORIGIN-DOMAIN is defined over
       it and §7.3.1's `content document` filters on that algorithm, so the code that reads it is already
       written below. Note also that an origin is SHARED between Documents (§7.3.1's inheritance cases), and
       the standard says so precisely to state that document.domain "affects both" — which is why origins are
       never interned by value here: two Documents the spec gives separate tuple origins to must not become
       one record that a future setter would change for both. */
    char    *domain;
    char    *serialized;  /* §7.1.1's serialization, computed once — see origin_serialized */
    uint32_t id;
    Origin  *next;
};

/* EVERY ORIGIN THIS AGENT HAS MINTED. They are immutable and live for the agent: a parked flow's COW delta
   holds a POD pointer to one (window_proxy.c captures its record by bytes), so freeing one before the agent
   ends resumes that flow onto freed memory. Released together at origin_release. */
static Origin  *g_origins;
static uint32_t g_next_id = 1;      /* 0 is "no origin", so a handle is never a valid zero */
static uint64_t g_next_nonce = 1;   /* 0 is "this is a tuple origin", so a nonce is never zero */
static const Origin *g_agent;

static Origin *origin_alloc(void)
{
    Origin *o = calloc(1, sizeof *o);

    CHECK(o != NULL, "origin: OOM minting an origin — every same-origin check in the engine compares one, so a "
                     "document without one has no principal to be judged against");
    o->port = -1;
    o->id = g_next_id++;
    o->next = g_origins;
    g_origins = o;
    return o;
}

/* THE MINT — the standard's "return a NEW opaque origin", and the one place a nonce is created. Every caller
   of it is a spec step that says exactly those words: URL §4.7's `data:`/`file:`/unknown-scheme case, and
   §7.3.1's steps 1 and 2. */
static const Origin *origin_opaque_new(void)
{
    Origin *o = origin_alloc();

    o->nonce = g_next_nonce++;
    return o;
}

static char *origin_strdup(const char *s)
{
    char *c = strdup(s);

    CHECK(c != NULL, "origin: OOM recording a tuple origin's component");
    return c;
}

/* §7.1.1's TUPLE ORIGIN. `host` is taken (owned by the record from here on). NOT INTERNED — see the `domain`
   field for why two equal tuples must stay two records. */
static const Origin *origin_tuple_new(const char *scheme, char *host, int port)
{
    Origin *o = origin_alloc();

    o->scheme = origin_strdup(scheme);
    o->host = host;
    o->port = port;
    return o;
}

bool origin_is_opaque(const Origin *o)
{
    DCHECK(o != NULL, "the kind of a NULL origin was asked for — every Document has an origin, and a caller "
                      "holding none has not asked whoever owns the document for it");
    return o->nonce != 0;
}

/* §7.1.1: "Two origins, A and B, are said to be same origin if the following algorithm returns true:
     1. If A and B are the same opaque origin, then return true.
     2. If A and B are both tuple origins and their schemes, hosts, and port are identical, then return true.
     3. Return false."
   STEP 1 IS THE NONCE, and it is why this file exists. STEP 2's hosts are compared through their
   SERIALIZATIONS, which is exact here rather than an approximation: a tuple origin exists only for the five
   schemes URL §4.7 names, all of them special, so both hosts came out of the same host parser — a domain is
   already lowercased and ASCII, an IPv4 is already canonical dotted-decimal, an IPv6 is already the compressed
   bracketed form — and two hosts are equal exactly when those agree. */
bool origin_same(const Origin *a, const Origin *b)
{
    DCHECK(a != NULL && b != NULL, "§7.1.1's same origin was asked about a NULL origin");
    if (a->nonce || b->nonce)
        return a->nonce == b->nonce;      /* step 1, and step 2's "both tuple origins" failing */
    return !strcmp(a->scheme, b->scheme) && !strcmp(a->host, b->host) && a->port == b->port;
}

/* §7.1.1: "Two origins, A and B, are said to be same origin-domain if the following algorithm returns true:
     1. If A and B are the same opaque origin, then return true.
     2. If A and B are both tuple origins:
        1. If A and B's schemes are identical, and their domains are identical and non-null, then return true.
        2. Otherwise, if A and B are same origin and their domains are both null, return true.
     3. Return false."
   IT IS NOT A LAXER SAME-ORIGIN: the standard's own table has a row where two origins are same origin-domain
   and NOT same origin (equal domains, different ports) and a row the other way (same origin, one domain set). */
bool origin_same_domain(const Origin *a, const Origin *b)
{
    DCHECK(a != NULL && b != NULL, "§7.1.1's same origin-domain was asked about a NULL origin");
    if (a->nonce || b->nonce)
        return a->nonce == b->nonce;                                        /* step 1 */
    if (!strcmp(a->scheme, b->scheme) && a->domain && b->domain && !strcmp(a->domain, b->domain))
        return true;                                                        /* step 2.1 */
    return origin_same(a, b) && !a->domain && !b->domain;                   /* step 2.2 */
}

/* §7.1.1's SERIALIZATION: "1. If origin is an opaque origin, then return `null`. 2. Otherwise, let result be
   origin's scheme. 3. Append `://` to result. 4. Append origin's host, serialized, to result. 5. If origin's
   port is non-null, append U+003A COLON and origin's port, serialized, to result. 6. Return result."
   Computed once and kept, because the record is immutable and every caller wants BYTES to hand outwards. */
const char *origin_serialized(const Origin *o)
{
    DCHECK(o != NULL, "the serialization of a NULL origin was asked for");
    if (!o->serialized) {
        Origin *m = (Origin *)o;   /* a pure function of an immutable record, memoized */
        if (o->nonce) {
            m->serialized = origin_strdup("null");
        } else {
            size_t n = strlen(o->scheme) + strlen(o->host) + 16;
            char *s = malloc(n);
            CHECK(s != NULL, "origin: OOM serializing an origin");
            if (o->port >= 0) snprintf(s, n, "%s://%s:%d", o->scheme, o->host, o->port);
            else              snprintf(s, n, "%s://%s", o->scheme, o->host);
            m->serialized = s;
        }
    }
    return o->serialized;
}

bool origin_is_serialized_tuple(const Origin *o, const char *serialized)
{
    DCHECK(o != NULL && serialized != NULL, "§7.1.1 step 2 was asked with nothing on one side");
    /* AN OPAQUE ORIGIN ANSWERS FALSE FOR EVERY INPUT, "null" INCLUDED. Step 1 is an identity comparison and a
       serialization carries no identity, so a string can never be the answer to it — treating "null" as a
       match would let any document claim to be any sandboxed one. */
    if (o->nonce) return false;
    return !strcmp(origin_serialized(o), serialized);
}

/* ---- URL §4.7's ORIGIN OF A URL ---------------------------------------------------------------------------
 *
 * "The origin of a URL url is the origin returned by running these steps, switching on url's scheme:
 *    `blob`   — 1. If url's blob URL entry is non-null, then return url's blob URL entry's environment's
 *                  origin. 2. Let pathURL be the result of parsing the result of URL path serializing url.
 *               3. If pathURL is failure, then return a new opaque origin. 4. If pathURL's scheme is `http`,
 *                  `https`, or `file`, then return pathURL's origin. 5. Return a new opaque origin.
 *    `ftp` `http` `https` `ws` `wss` — Return the tuple origin (url's scheme, url's host, url's port, null).
 *    `file`   — Unfortunate as it is, this is left as an exercise to the reader. When in doubt, return a new
 *               opaque origin.
 *    Otherwise — Return a new opaque origin."
 *
 * THE TUPLE HALF IS COMPUTED ONCE, HERE, and both public entry points read it: one builds a RECORD out of it
 * and the other builds BYTES. url.c used to hold a second copy of this rule for the bytes; a second copy of a
 * spec sentence is the second answer that is always subtly wrong, and it is now one. */
static bool url_origin_tuple(const UrlRecord *u, const char **scheme, char **host, int *port);

/* §4.7's `blob` case, split out for the reason url.c's version was: expressing it as a self-call makes §4.7 a C
   recursion whose depth the call graph cannot see, and the recursion is a fiction — the inner URL is
   constrained to http/https/file, none of which is blob, so there is no depth. */
static bool url_origin_tuple_blob(const UrlRecord *u, const char **scheme, char **host, int *port)
{
    UrlRecord inner;
    char *path = url_serialize_path(u);
    bool ok = false;

    url_record_init(&inner);
    /* §4.7 names THREE schemes here and only those: a `blob:` whose path spells ftp, ws, wss or another blob
       has an opaque origin. `file` is named and still comes out opaque below, because §4.7's `file` case says
       so — the two rules compose, and short-circuiting `file` here would state the same answer for the wrong
       reason. */
    if (path && url_parse(&inner, path, strlen(path), NULL) && inner.scheme &&
        (!strcmp(inner.scheme, "http") || !strcmp(inner.scheme, "https") || !strcmp(inner.scheme, "file"))) {
        const char *inner_scheme;

        if (url_origin_tuple(&inner, &inner_scheme, host, port)) {
            /* THE SCHEME IS RE-STATED AS A LITERAL rather than passed through: url_origin_tuple borrows it
               from the record it read, and `inner` is the one record that does not outlive the answer. §4.7
               constrains it to these two — `file` runs the file case and comes back opaque. */
            *scheme = !strcmp(inner_scheme, "https") ? "https" : "http";
            ok = true;
        }
    }
    url_record_free(&inner);
    free(path);
    return ok;
}

/* Answers false for an origin that is OPAQUE. On true, `*scheme` is BORROWED from `u` and `*host` is OWNED. */
static bool url_origin_tuple(const UrlRecord *u, const char **scheme, char **host, int *port)
{
    if (!u->scheme) return false;
    if (!strcmp(u->scheme, "blob")) return url_origin_tuple_blob(u, scheme, host, port);
    if (strcmp(u->scheme, "http") && strcmp(u->scheme, "https") && strcmp(u->scheme, "ftp") &&
        strcmp(u->scheme, "ws") && strcmp(u->scheme, "wss"))
        return false;   /* §4.7: `file` and every other scheme has an opaque origin */
    *scheme = u->scheme;
    *host = url_serialize_host(&u->host);
    CHECK(*host != NULL, "origin: OOM serializing a tuple origin's host");
    *port = u->port;
    return true;
}

const Origin *origin_of_url(const UrlRecord *u)
{
    const char *scheme = NULL;
    char *host = NULL;
    int port = -1;

    DCHECK(u != NULL, "URL §4.7 was asked for the origin of no URL — the spec's null URL is §7.3.1 step 2's "
                      "case and is spelled by passing NULL to origin_determine, not to this");
    if (!url_origin_tuple(u, &scheme, &host, &port))
        return origin_opaque_new();
    return origin_tuple_new(scheme, host, port);
}

char *origin_serialize_of_url(const UrlRecord *u)
{
    const char *scheme = NULL;
    char *host = NULL, *s;
    int port = -1;
    size_t n;

    DCHECK(u != NULL, "URL §4.7 was asked for the origin of no URL");
    if (!url_origin_tuple(u, &scheme, &host, &port))
        return origin_strdup("null");
    n = strlen(scheme) + strlen(host) + 16;
    s = malloc(n);
    CHECK(s != NULL, "origin: OOM serializing a URL's origin");
    if (port >= 0) snprintf(s, n, "%s://%s:%d", scheme, host, port);
    else           snprintf(s, n, "%s://%s", scheme, host);
    free(host);
    return s;
}

/* ---- HTML §7.3.1's DETERMINE THE ORIGIN ------------------------------------------------------------------- */

/* HTML §2.4.1: "A URL matches about:blank if its scheme is `about`, its path contains a single string `blank`,
   its username and password are the empty string, and its host is null" — its query and fragment MAY be
   non-null, which is why they are not tested. "A URL matches about:srcdoc if its scheme is `about`, its path
   contains a single string `srcdoc`, its QUERY IS NULL, its username and password are the empty string, and
   its host is null." A non-special scheme's path is the record's opaque path, which is where `blank` lands. */
static bool url_matches_about(const UrlRecord *u, const char *what, bool query_must_be_null)
{
    if (!u->scheme || strcmp(u->scheme, "about")) return false;
    if (!u->opaque_path || strcmp(u->opaque_path, what)) return false;
    if (u->username && *u->username) return false;
    if (u->password && *u->password) return false;
    if (query_must_be_null && u->query) return false;
    return u->host.kind == URL_HOST_NULL;
}

const Origin *origin_determine(const UrlRecord *url, bool sandboxed_origin, const Origin *source)
{
    /* STEP 1 — "if sandboxFlags has its sandboxed origin browsing context flag set, then return a NEW OPAQUE
       ORIGIN". This is the mint that makes two sandboxed frames two principals, and it is why the answer is a
       record: two documents both serializing to "null" are the same string and different origins. */
    if (sandboxed_origin) return origin_opaque_new();
    /* STEP 2 — a null URL is a document that was never loaded from anywhere, and it gets its own opaque
       origin rather than inheriting one. */
    if (!url) return origin_opaque_new();
    /* STEPS 3 AND 4 — THE COPY. The SAME record travels, so the two Documents hold the same nonce and §7.1.1
       step 1 answers true for them: "the cases that return sourceOrigin result in two Documents that end up
       with the same underlying origin". */
    if (url_matches_about(url, "srcdoc", /*query_must_be_null*/ true)) {
        DCHECK(source != NULL, "§7.3.1 step 3's assert: an about:srcdoc document was created with no source "
                               "origin, and an srcdoc document has no origin of its own to fall back to");
        return source;
    }
    if (url_matches_about(url, "blank", /*query_must_be_null*/ false) && source) return source;
    return origin_of_url(url);                                              /* step 5 */
}

/* ---- the agent's own origin, and the transports ------------------------------------------------------------ */

void origin_agent_adopt(const char *serialized)
{
    DCHECK(serialized != NULL && *serialized,
           "an agent was brought up with no serialized PRINCIPAL — the trusted zone states it (SECURITY.md), "
           "and an engine that invented one would be judging every same-origin check against a guess");
    DCHECK(g_agent == NULL,
           "an agent adopted a SECOND origin — an instance is an ORIGIN-KEYED agent cluster, so a document at "
           "another origin is another INSTANCE and never a second principal in this one");
    g_agent = origin_parse(serialized);
}

const Origin *origin_agent(void)
{
    DCHECK(g_agent != NULL,
           "this agent's own origin was read before platform_agent_init adopted it — every same-origin check "
           "compares against it, so there is nothing here to compare and the answer would be a guess");
    return g_agent;
}

const Origin *origin_parse(const char *serialized)
{
    UrlRecord rec;
    const Origin *o;

    DCHECK(serialized != NULL, "an origin was asked for from no serialization");
    /* A SERIALIZED TUPLE ORIGIN IS ITSELF A URL, and running the real parser over it is what makes this the
       same §4.7 the rest of the file is rather than a second string format. "null" fails that parse — it has
       no scheme — and lands on the mint, which is exactly §7.1.1's "no serialization it can be recreated
       from": what comes back is A NEW opaque origin, same origin with nothing, which is the honest answer for
       a principal that crossed a boundary as bytes. */
    url_record_init(&rec);
    if (url_parse(&rec, serialized, strlen(serialized), NULL))
        o = origin_of_url(&rec);
    else
        o = origin_opaque_new();
    url_record_free(&rec);
    return o;
}

uint32_t origin_id(const Origin *o)
{
    DCHECK(o != NULL, "the handle of a NULL origin was asked for");
    return o->id;
}

const Origin *origin_by_id(uint32_t id)
{
    Origin *o;

    for (o = g_origins; o; o = o->next)
        if (o->id == id) return o;
    /* ALWAYS FATAL, dev and release: what did not resolve is a PRINCIPAL, and continuing without one is
       continuing with no answer to every same-origin check downstream — SECURITY.md's boundary, not a
       fidelity detail. */
    CHECK_FAIL("an origin HANDLE named no origin of this agent. A handle is meaningful only inside the agent "
               "that minted it — it is not a serialization — so a handle that resolves to nothing arrived from "
               "another instance or from another SESSION. Crossing an instance is origin_parse's job (the "
               "trusted zone stamps the serialization); crossing a session needs this table to park and resume "
               "with the frontier, which is not built");
    return NULL;
}

void origin_release(void)
{
    Origin *o = g_origins;

    while (o) {
        Origin *n = o->next;
        free(o->scheme);
        free(o->host);
        free(o->domain);
        free(o->serialized);
        free(o);
        o = n;
    }
    g_origins = NULL;
    g_agent = NULL;
    g_next_id = 1;
    g_next_nonce = 1;
}
