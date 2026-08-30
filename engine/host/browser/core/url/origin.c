/* ORIGIN — HTML §7.1.1's record and its algorithms, over URL §4.7's construction. See origin.h for why this is
 * a record with an identity rather than the string this engine used to compare. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/url/origin.h"
/* §7.1.1's one mutable component is per-flow state, so the one place it is written captures it — the browser
   owns the API, the solver owns the time travel. */
#include "solver/cow.h"

/* §7.1.1's TWO SHAPES IN ONE RECORD, and `nonce` is which: NON-ZERO is the OPAQUE origin — "an internal value,
   with no serialization it can be recreated from … for which the only meaningful operation is testing for
   equality", and the nonce IS that value — while ZERO is the TUPLE origin, whose four components are below.
   A tuple origin's components are never read off an opaque one, which origin_is_opaque exists to ask first. */
struct Origin {
    uint64_t nonce;
    char    *scheme;      /* §7.1.1's scheme (an ASCII string) — tuple only */
    /* §7.1.1's HOST, WHICH IS "a host" AND NOT A STRING — URL §4.2's parsed value: a domain, an IPv4 NUMBER,
       an IPv6 address, an opaque host or the empty host. Kept as the parsed value because that is what the
       record IS, because step 2's "their hosts are identical" is then a comparison of HOSTS rather than of
       text, and because the algorithms defined over an origin read it that way — Secure Contexts §3.1 step 4
       matches against 127.0.0.0/8 and ::1/128 (so `http://0x7f.1/`, which url.c resolves to the NUMBER
       127.0.0.1, is loopback) and step 5 matches `localhost` BY NAME. Tuple only. */
    UrlHost  host;
    int      port;        /* §7.1.1's port; -1 is the spec's null — tuple only */
    /* §7.1.1's DOMAIN: "null unless stated otherwise", and §7.1.1.2's `document.domain` setter is the only
       thing in the platform that states otherwise — origin_set_domain below is that setter's step 6 and the
       only writer there is or can be. NULL is the spec's null.
       IT IS A POINTER INTO AGENT-LIFETIME STORAGE and never an inline value, because this slot is the one
       mutable field of an otherwise immutable record: the running flow captures it into its COW delta by BYTES
       before writing, and a byte capture may only ever restore a pointer that is still live. g_domains below is
       what keeps every parsed domain alive for the agent, exactly as g_origins does for the records.
       AN ORIGIN IS SHARED BETWEEN DOCUMENTS (§7.3.2.1's inheritance cases), and the standard says so precisely to
       state that document.domain "affects both" — which is why origins are never interned BY VALUE here: two
       Documents the spec gives separate tuple origins to must not become one record this setter changes for
       both, and two Documents it gives ONE record to must. */
    const UrlHost *domain;
    char    *serialized;  /* §7.1.1's serialization, computed once — see origin_serialized */
    uint32_t id;
    Origin  *next;
};

/* EVERY DOMAIN §7.1.1.2's SETTER HAS EVER PARSED, in a list of stable nodes for the agent. A relaxed domain is
   never freed and never overwritten in place: the slot that names it is captured by BYTES into a flow's delta,
   so a second `document.domain =` that reused the storage would rewrite what a parked sibling still points at,
   and a free would leave it pointing at nothing. Appending instead makes the slot's every historical value live
   — which is the same rule g_origins states for the records themselves. */
typedef struct OriginDomain {
    UrlHost              host;
    struct OriginDomain *next;
} OriginDomain;

static OriginDomain *g_domains;

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
   of it is a spec step that says exactly those words: URL §4.7's `data:`/`file:`/unknown-scheme case,
   §7.3.2.1's steps 1 and 2, and Permissions Policy §7.2's declared origin steps 1 and 2. */
const Origin *origin_opaque_new(void)
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

/* §4.2's HOST, COPIED — the record owns its own, because the URL record it came from is a local of whoever
   parsed it. Only the domain/opaque spelling carries memory; the rest is POD. */
static void origin_host_copy(UrlHost *dst, const UrlHost *src)
{
    *dst = *src;
    dst->domain = src->domain ? origin_strdup(src->domain) : NULL;
}

/* §7.1.1's TUPLE ORIGIN. NOT INTERNED — see the `domain` field for why two equal tuples must stay two
   records. */
static const Origin *origin_tuple_new(const char *scheme, const UrlHost *host, int port)
{
    Origin *o = origin_alloc();

    o->scheme = origin_strdup(scheme);
    origin_host_copy(&o->host, host);
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
   STEP 1 IS THE NONCE, and it is why this file exists. STEP 2's hosts are compared as HOSTS (by value, over
   the parsed host), never as text. */
bool origin_same(const Origin *a, const Origin *b)
{
    DCHECK(a != NULL && b != NULL, "§7.1.1's same origin was asked about a NULL origin");
    if (a->nonce || b->nonce)
        return a->nonce == b->nonce;      /* step 1, and step 2's "both tuple origins" failing */
    return !strcmp(a->scheme, b->scheme) && url_host_equal(&a->host, &b->host) && a->port == b->port;
}

/* §7.1.1: "Two origins, A and B, are said to be same origin-domain if the following algorithm returns true:
     1. If A and B are the same opaque origin, then return true.
     2. If A and B are both tuple origins:
        1. If A and B's schemes are identical, and their domains are identical and non-null, then return true.
        2. Otherwise, if A and B are same origin and their domains are both null, return true.
     3. Return false."
   IT IS NOT A LAXER SAME-ORIGIN, and the standard's own five-row table is what says so. Two rows disagree, one
   in each direction, and both are answered here rather than approximated:

     A = ("https", "example.org", 314, "example.org")   B = ("https", "example.org", 420, "example.org")
       same origin ❌ (the PORTS differ, and step 2 of same origin compares them) — same origin-domain ✅
       (step 2.1 compares SCHEME and DOMAIN and never looks at the port).
     A = ("https", "example.org", null, null)           B = ("https", "example.org", null, "example.org")
       same origin ✅ (identical tuples) — same origin-domain ❌ (step 2.1 needs BOTH domains non-null and
       step 2.2 needs BOTH null, so a pair with one relaxed side satisfies neither).

   The second row is the one a page reaches by itself: one Document sets `document.domain` and its same-origin
   sibling has not, and §7.3.1's `content document` — which filters on THIS algorithm — answers null. */
bool origin_same_origin_domain(const Origin *a, const Origin *b)
{
    DCHECK(a != NULL && b != NULL, "§7.1.1's same origin-domain was asked about a NULL origin");
    if (a->nonce || b->nonce)
        return a->nonce == b->nonce;                                        /* step 1 */
    if (!strcmp(a->scheme, b->scheme) && a->domain && b->domain && url_host_equal(a->domain, b->domain))
        return true;                                                        /* step 2.1 */
    return origin_same(a, b) && !a->domain && !b->domain;                   /* step 2.2 */
}

/* §7.1.1's EFFECTIVE DOMAIN, all three steps — the value §7.1.1.2's getter serializes and the host its setter
   measures the assigned value against. */
const UrlHost *origin_effective_domain(const Origin *o)
{
    DCHECK(o != NULL, "§7.1.1's effective domain was asked of no origin");
    if (o->nonce) return NULL;      /* step 1 — an opaque origin has no components at all */
    if (o->domain) return o->domain;                                        /* step 2 */
    return &o->host;                                                        /* step 3 */
}

/* §7.1.1's TUPLE COMPONENTS — see origin.h for why they are not the effective domain and why an opaque origin
   asserts instead of answering. The three are spelled separately rather than as one "give me the tuple"
   accessor because the caller that reads them (CSP §6.7.2.8) reads them in three different comparisons, one of
   which is a scheme relation and not an equality. */
const char *origin_scheme(const Origin *o)
{
    DCHECK(o != NULL, "§7.1.1's scheme was asked of no origin");
    DCHECK(!o->nonce, "the SCHEME of an OPAQUE origin was read — §7.1.1 gives an opaque origin no components "
                      "at all, so a caller that needs one asks origin_is_opaque first and answers that case "
                      "itself; an origin reaching here with a nonce means that question was never asked");
    return o->scheme;
}

const UrlHost *origin_host(const Origin *o)
{
    DCHECK(o != NULL, "§7.1.1's host was asked of no origin");
    DCHECK(!o->nonce, "the HOST of an OPAQUE origin was read — §7.1.1 gives an opaque origin no components at "
                      "all, so a caller that needs one asks origin_is_opaque first");
    return &o->host;
}

int origin_port(const Origin *o)
{
    DCHECK(o != NULL, "§7.1.1's port was asked of no origin");
    DCHECK(!o->nonce, "the PORT of an OPAQUE origin was read — §7.1.1 gives an opaque origin no components at "
                      "all, so a caller that needs one asks origin_is_opaque first");
    return o->port;
}

bool origin_same_as_url(const Origin *o, const UrlRecord *u)
{
    UrlRecord scratch;
    const UrlRecord *t;
    Origin probe;
    bool same;

    DCHECK(o != NULL && u != NULL, "§7.1.1's same origin was asked with no origin, or about no URL");
    t = origin_tuple_url(u, &scratch);
    /* §4.7's every-other-scheme case is "return a NEW OPAQUE ORIGIN", and §7.1.1 step 1 compares IDENTITY — a
       nonce minted here is same origin with nothing that already exists, so the answer is false and the record
       never needs to be made. This is where the "cannot be same origin with themselves" note is cashed in. */
    if (!t) {
        url_record_free(&scratch);
        return false;
    }
    /* THE PROBE IS NOT A MINT. It is never handed out, never appended to g_origins and never serialized, so it
       needs neither an identity nor a lifetime; what it is, is URL §4.7 Origin's tuple origin for `u` —
       nonce zero, and the three components url_record_free below still owns — so that §7.1.1 is decided by the ONE
       implementation of it rather than by a second comparison written out here. */
    memset(&probe, 0, sizeof probe);
    probe.scheme = (char *)t->scheme;
    probe.host = t->host;          /* BORROWED: url_host_equal reads it and origin_same keeps nothing */
    probe.port = t->port;
    same = origin_same(o, &probe);
    url_record_free(&scratch);
    return same;
}

void origin_set_domain(JSContext *ctx, JSValueConst owner, const Origin *o, const UrlHost *domain)
{
    Origin *m = (Origin *)o;   /* §7.1.1's one mutable component — see origin.h */
    OriginDomain *d;

    DCHECK(o != NULL && domain != NULL, "§7.1.1.2 step 6 was asked to set a domain on no origin, or to no host");
    DCHECK(!o->nonce, "§7.1.1.2 step 6 reached an OPAQUE origin — only the domain of a TUPLE origin can be "
                      "changed, and the setter's step 3 throws a SecurityError before this because an opaque "
                      "origin's effective domain is null");
    /* THE INSTANCE BOUNDARY, ASSERTED RATHER THAN ARGUED. An instance is an ORIGIN-KEYED agent cluster
       (SECURITY.md), so every Document in this heap is same origin with this agent and a Document at another
       origin is another INSTANCE holding its own records. A domain written on an origin that is NOT this
       agent's could only be a PEER's — a record origin_parse minted out of a serialization — and writing one
       would be this engine claiming same origin-DOMAIN across an instance boundary. It also could not be
       observed if it were: §7.1.1's serialization has no domain component, so an origin that crossed a
       boundary as bytes always arrives with a null domain, and step 2.1 needs BOTH sides non-null. The
       relaxation is therefore confined to this heap by construction, and this is where that is checked. */
    DCHECK(origin_same(o, origin_agent()),
           "§7.1.1.2's setter reached an origin that is not this agent's — an instance is an ORIGIN-KEYED agent "
           "cluster, so the only origins in this heap are this agent's own; one that is not came from "
           "origin_parse, which is how a PEER INSTANCE's principal arrives, and relaxing a peer's domain would "
           "be claiming same origin-domain across the boundary that exists to prevent exactly that");
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "origin: OOM recording a relaxed domain — §7.1.1.2's setter has already passed every one "
                     "of its security checks at this point, so there is no answer left that is not this one");
    origin_host_copy(&d->host, domain);
    d->next = g_domains;
    g_domains = d;
    /* THE SOLVER OWNS THE TIME TRAVEL — the same split every DOM write host-edge follows. The slot is shared
       baseline state (an origin lives for the agent and several Documents may hold this one record), so the
       running flow captures its BYTES before the write; without it, one forked arm's relaxation would decide
       every sibling arm's `contentDocument`. Only the slot, never the whole record: `serialized` is a lazily
       computed memo of an immutable value and reverting it would leak the string and recompute it. */
    cow_capture_host_state(ctx, owner, &m->domain, sizeof m->domain);
    m->domain = &d->host;
}

/* §7.1.1's SERIALIZATION: "1. If origin is an opaque origin, then return `null`. 2. Otherwise, let result be
   origin's scheme. 3. Append `://` to result. 4. Append origin's host, serialized, to result. 5. If origin's
   port is non-null, append U+003A COLON and origin's port, serialized, to result. 6. Return result."
   Computed once and kept, because the record is immutable and every caller wants BYTES to hand outwards. */
static char *tuple_serialize(const char *scheme, const UrlHost *host, int port)
{
    char *h = url_serialize_host(host), *s;
    size_t n;

    CHECK(h != NULL, "origin: OOM serializing a tuple origin's host");
    n = strlen(scheme) + strlen(h) + 16;
    s = malloc(n);
    CHECK(s != NULL, "origin: OOM serializing an origin");
    if (port >= 0) snprintf(s, n, "%s://%s:%d", scheme, h, port);   /* step 5 */
    else           snprintf(s, n, "%s://%s", scheme, h);
    free(h);
    return s;
}

const char *origin_serialized(const Origin *o)
{
    DCHECK(o != NULL, "the serialization of a NULL origin was asked for");
    if (!o->serialized) {
        Origin *m = (Origin *)o;   /* a pure function of an immutable record, memoized */
        m->serialized = o->nonce ? origin_strdup("null")                       /* step 1 */
                                 : tuple_serialize(o->scheme, &o->host, o->port);
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
 * THE TUPLE HALF IS COMPUTED ONCE, HERE, and every entry point reads it: one builds a RECORD, one builds
 * BYTES, and one hands the components to an algorithm that reads them and decides nothing about identity.
 * url.c used to hold a second copy of this rule for the bytes; a second copy of a spec sentence is the second
 * answer that is always subtly wrong, and it is now one. */

/* §4.7's SCHEMES WITH A TUPLE ORIGIN, and only these five. `file` is deliberately absent — §4.7's file case
   says "when in doubt, return a new opaque origin" — which is also what makes the blob case below compose
   rather than needing a second list. */
static bool scheme_has_tuple_origin(const char *scheme)
{
    return !strcmp(scheme, "http") || !strcmp(scheme, "https") || !strcmp(scheme, "ftp") ||
           !strcmp(scheme, "ws") || !strcmp(scheme, "wss");
}

const UrlRecord *origin_tuple_url(const UrlRecord *u, UrlRecord *scratch)
{
    DCHECK(u != NULL && scratch != NULL, "URL §4.7 was asked for the origin of no URL");
    url_record_init(scratch);   /* initialised on EVERY path: the caller frees it whatever the answer is */
    if (!u->scheme) return NULL;
    /* §4.7's `blob` case, and there is no recursion in it: the inner URL is constrained to http/https/file,
       none of which is blob, so a self-call would have expressed a depth that cannot exist. `file` parses
       through and comes back opaque below, because §4.7's file case says so — the two rules compose. */
    if (!strcmp(u->scheme, "blob")) {
        char *path = url_serialize_path(u);
        const UrlRecord *r = NULL;

        /* §4.7 NAMES THREE INNER SCHEMES AND ONLY THOSE, and the list is not the same as the five above:
           returning the inner origin for any scheme that has a tuple one would make `blob:ws://example.org/`
           same origin with a WebSocket endpoint. `file` is named here and still comes back opaque, because the
           second test is §4.7's file case. */
        if (path && url_parse(scratch, path, strlen(path), NULL) && scratch->scheme &&
            (!strcmp(scratch->scheme, "http") || !strcmp(scratch->scheme, "https") ||
             !strcmp(scratch->scheme, "file")) &&
            scheme_has_tuple_origin(scratch->scheme))
            r = scratch;
        free(path);
        return r;
    }
    return scheme_has_tuple_origin(u->scheme) ? u : NULL;
}

const Origin *origin_of_url(const UrlRecord *u)
{
    UrlRecord scratch;
    const UrlRecord *t;
    const Origin *o;

    DCHECK(u != NULL, "URL §4.7 was asked for the origin of no URL — the spec's null URL is §7.3.2.1 step 2's "
                      "case and is spelled by passing NULL to origin_determine, not to this");
    t = origin_tuple_url(u, &scratch);
    o = t ? origin_tuple_new(t->scheme, &t->host, t->port) : origin_opaque_new();
    url_record_free(&scratch);
    return o;
}

char *origin_serialize_of_url(const UrlRecord *u)
{
    UrlRecord scratch;
    const UrlRecord *t = origin_tuple_url(u, &scratch);
    char *s = t ? tuple_serialize(t->scheme, &t->host, t->port) : origin_strdup("null");

    url_record_free(&scratch);
    return s;
}

/* ---- HTML §7.3.2.1's DETERMINE THE ORIGIN ------------------------------------------------------------------- */

/* HTML §2.4.1's TWO MATCH RELATIONS live in core/url/url.c now — §7.3.2.1 is no longer their only asker.
   §2.4.3's fallback base URL asks the same two questions of the same records, and two copies of a relation
   this precise (the query is tested for one and deliberately not for the other) is how they drift apart. */

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
        DCHECK(source != NULL, "§7.3.2.1 step 3's assert: an about:srcdoc document was created with no source "
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
    OriginDomain *d = g_domains;

    while (o) {
        Origin *n = o->next;
        free(o->scheme);
        url_host_free(&o->host);
        free(o->serialized);
        free(o);
        o = n;
    }
    /* The relaxed domains outlive the records that point at them by exactly nothing: both lists go together,
       and neither is walked from the other. */
    while (d) {
        OriginDomain *n = d->next;
        url_host_free(&d->host);
        free(d);
        d = n;
    }
    g_domains = NULL;
    g_origins = NULL;
    g_agent = NULL;
    g_next_id = 1;
    g_next_nonce = 1;
}
