/* SECURE CONTEXTS §3.1 and §3.2, and HTML §8.1.3.5 — see secure_context.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/secure_context.h"
#include "core/realm.h"
#include "core/url/origin.h"
#include "core/url/url.h"

/* §3.1 STEPS 3 THROUGH 8, OVER A TUPLE ORIGIN'S COMPONENTS — the scheme and the HOST, which is URL §4.2's
   parsed value and not a string (see secure_context.h). They are the components rather than a URL record
   because that is what the algorithm reads, and because §4.7 answers a `blob:` URL with ANOTHER URL's tuple:
   both routes reach these steps with the same three things, and neither is a self-call whose depth would be a
   fact about the input. Depth is not a thing a page gets to choose in this engine. */
static bool tuple_origin_trustworthy(const char *scheme, const UrlHost *h)
{
    const char *host;
    size_t n;

    DCHECK(scheme != NULL, "a tuple origin with no scheme — §3.1 step 2 asserts this is a tuple origin, and an "
                           "origin with no scheme is not one");

    /* STEP 3: "If origin's scheme is either `https` or `wss`, return Potentially Trustworthy." NEITHER the
       port NOR the host has any say — the spec's own closing note says so, which is why a `https://` on a
       non-default port is still trustworthy. */
    if (!strcmp(scheme, "https") || !strcmp(scheme, "wss")) return true;

    /* STEP 4: "If origin's host matches one of the CIDR notations 127.0.0.0/8 or ::1/128". A CIDR match over
       the parsed host, not a string compare against "127.0.0.1": url.c's parser resolves `http://0x7f.1/` to
       the IPv4 NUMBER 127.0.0.1 and `http://[0:0::1]/` to the IPv6 address ::1, and a text test would call
       both of those untrusted while a browser calls them loopback. */
    if (h->kind == URL_HOST_IPV4 && (h->ipv4 >> 24) == 127) return true;
    if (h->kind == URL_HOST_IPV6) {
        int i;
        bool is_loopback = h->ipv6[7] == 1;
        for (i = 0; i < 7; i++) if (h->ipv6[i] != 0) is_loopback = false;
        if (is_loopback) return true;
    }

    /* STEP 5: `localhost`, `localhost.`, and anything ending in `.localhost` or `.localhost.` — BY NAME.
       RFC6761 §6.3 makes those names special and §5.2 says a user agent may trust them only if it also
       adheres to the localhost name resolution rules (localhost never resolves off the loopback). THIS ENGINE
       RESOLVES NO NAME AT ALL — it has no resolver — so it cannot resolve one to a non-loopback address and
       the condition holds by construction rather than by configuration.
       The host must be a DOMAIN: `http://localhost/` parses to one, and an opaque host carrying the same
       bytes belongs to a non-special scheme whose origin step 1 already refused. url.c's parser has already
       lowercased a domain (domain-to-ASCII), so this compares against the one case that can arrive. */
    if (h->kind == URL_HOST_DOMAIN) {
        host = h->domain;
        DCHECK(host != NULL, "a URL_HOST_DOMAIN host with no domain string — url.c's parser produces the kind "
                             "and the bytes together");
        n = strlen(host);
        if (n && host[n - 1] == '.') n--;                       /* the trailing-dot ("localhost.") spelling */
        if (n == 9 && !memcmp(host, "localhost", 9)) return true;
        if (n > 10 && !memcmp(host + n - 10, ".localhost", 10)) return true;
    }

    /* STEP 6 — "If origin's scheme is `file`, return Potentially Trustworthy" — IS UNREACHABLE HERE, AND THAT
       IS A DECISION THIS ENGINE HAS ALREADY MADE ELSEWHERE rather than a step skipped. The step exists for a
       user agent that gives `file:` URLs a TUPLE origin; URL §4.7 leaves that to the implementation ("when in
       doubt, return a new opaque origin") and core/url/origin.c takes the opaque answer, which is also what
       stops two unrelated local files being same origin. So §3.1 step 1 answers first for every file URL, and
       §3.1's own note — "the user agent SHOULD treat file URLs as potentially trustworthy" — is stated at the
       one layer that still knows the URL: secure_context_url_potentially_trustworthy below. Two components
       disagreeing about what a file URL's origin IS was the previous arrangement, and it is the defect this
       one removes. */

    /* STEPS 7 AND 8 have no answer to give here, and that is a statement rather than an omission. Step 7 is
       "a scheme the user agent considers authenticated" — §7.1's packaged applications, `app:` and
       `chrome-extension:`, and this user agent packages nothing. Step 8 is "an origin CONFIGURED as
       trustworthy" — §7.2's development-environment override, which is a user preference and this engine has
       no user. Inventing either would make the answer depend on something no page can observe. */
    return false;   /* STEP 9 */
}

bool secure_context_origin_potentially_trustworthy(const UrlRecord *u)
{
    UrlRecord scratch;
    const UrlRecord *t;
    bool r;

    DCHECK(u != NULL, "§3.1 was asked about no origin at all");

    /* STEP 1: "If origin is an opaque origin, return Not Trustworthy." WHICH URLs HAVE ONE IS URL §4.7'S RULE
       AND core/url/origin.c OWNS IT — this asks for the TUPLE and takes NULL as step 1's answer, rather than
       keeping a scheme list of its own that could disagree with the one §4.7 is implemented from.
       IT USED TO ASK BY SERIALIZING AND RE-PARSING: an origin was a string here, so the only way to reach the
       parsed host steps 4 and 5 need was to serialize §4.7's answer and run the URL parser over the bytes
       again. That was the last site where the lossy serialization was load-bearing, and it was correct for the
       same reason the old same-origin check was correct until it was not — it never compared two origins.
       `blob:` IS THE ONE URL WHOSE ORIGIN IS SOMEBODY ELSE'S — §4.7 gives it the origin of the URL its path
       spells, which is precisely why §3.2's note says a blob created in a trustworthy origin is itself
       trustworthy — and that is the case `scratch` carries. */
    t = origin_tuple_url(u, &scratch);
    /* STEP 2's assert holds by construction: what came back is a tuple origin or nothing. */
    r = t != NULL && tuple_origin_trustworthy(t->scheme, &t->host);
    url_record_free(&scratch);
    return r;
}

bool secure_context_origin_record_potentially_trustworthy(const Origin *o)
{
    DCHECK(o != NULL, "§3.1 was asked about no origin at all — every environment settings object has one, so "
                      "a NULL here is a caller that has not decided which environment it is asking about");
    /* STEP 1: "If origin is an opaque origin, return Not Trustworthy." Asked of the RECORD, which is the one
       thing this entry does differently from its sibling above — §7.1.1 gives an opaque origin no scheme and
       no host, and core/url/origin.h's component accessors ASSERT that a caller answered this first. */
    if (origin_is_opaque(o)) return false;
    /* STEP 2's assert holds by construction: what is left is a tuple origin.
       STEPS 3 THROUGH 9, over the same components the URL form reads — and NOT origin_effective_domain, which
       §3.1's own closing note excludes by name: "Neither origin's domain nor port has any effect on whether or
       not it is considered to be a secure context." A `document.domain` write must not make an origin
       trustworthy, and reading the effective domain here is exactly how it would. */
    return tuple_origin_trustworthy(origin_scheme(o), origin_host(o));
}

/* §3.2 STEP 1's two literals. "If url is `about:blank` or `about:srcdoc`" means the URL RECORD is that one and
   not one that merely starts with it: `about:blank?x` has a query, has no inherited context, and is not what
   a navigable's initial Document is created at. */
static bool url_is_about(const UrlRecord *u, const char *what)
{
    return u->scheme && !strcmp(u->scheme, "about") && u->opaque_path && !strcmp(u->opaque_path, what) &&
           !u->query && !u->fragment;
}

bool secure_context_url_potentially_trustworthy(const char *url)
{
    UrlRecord u;
    bool r;

    DCHECK(url != NULL && *url,
           "§3.2 was asked about an empty address — every environment is created AT a URL, so a caller with "
           "nothing to pass here has not decided which document its realm is");
    url_record_init(&u);
    if (!url_parse(&u, url, strlen(url), NULL)) {
        url_record_free(&u);
        return false;   /* input that is not a URL has no origin, so it cannot have a trustworthy one */
    }
    /* STEPS 1 AND 2: the three URLs that INHERIT their context from their creator rather than having an
       origin of their own. They are trustworthy HERE and the inheritance is modelled ELSEWHERE — a child
       navigable takes its creator's top-level creation URL (core/realm.h), so an `about:blank` iframe of an
       http page never reaches this line with `about:blank`. What does reach it is a TOP-LEVEL about:blank,
       whose creator is the user agent itself. */
    /* STEP 2 is `data:`, and the LINE AFTER IT IS §3.1's NOTE, made at this layer because this is the layer
       that still has a URL. "The user agent SHOULD treat file URLs as potentially trustworthy … treating such
       resources as potentially trustworthy is convenient for developers building an application before
       deploying it to the public" — which is exactly the case a bundle is run from before it ships, and
       refusing it would hide that bundle's gated code. §3.1's step 6 says the same thing over an ORIGIN whose
       scheme is `file`, and this engine has no such origin: URL §4.7 leaves file's origin to the
       implementation and core/url/origin.c gives it an OPAQUE one, so that step is unreachable (see it) and
       this is where the SHOULD is honoured. */
    r = url_is_about(&u, "blank") || url_is_about(&u, "srcdoc") ||   /* step 1 */
        (u.scheme && !strcmp(u.scheme, "data")) ||                   /* step 2 */
        (u.scheme && !strcmp(u.scheme, "file")) ||
        secure_context_origin_potentially_trustworthy(&u);           /* step 3 */
    url_record_free(&u);
    return r;
}

bool secure_context_is(JSContext *ctx)
{
    JSValue v;
    const char *url;
    bool r;

    /* HTML §8.1.3.5 "Secure contexts", ALL THREE OF ITS TOP-LEVEL STEPS, in the order it states them — and the
       order is the whole content rather than a formality. Step 1 opens "If environment is an environment
       settings object", which every environment this engine has is; its 1.1 names the global, and 1.2 and 1.3
       ask what KIND of global that is. Only if neither answers does step 2 look at the top-level creation URL.
       WHY THAT ORDER IS LOAD-BEARING HERE: HTML §10.2.6.2 "Script settings for workers" sets a worker
       environment's "creation URL to worker global scope's url, top-level creation URL to null", so step 2 has
       no operand at all in a worker realm — a worker of an `https` page would answer FALSE out of step 2 and
       TRUE out of step 1.2, and Web IDL §3.3.13 [SecureContext] then removes a member rather than making it
       throw, so the two arms are two different platform surfaces and not two spellings of one.
       THIS FILE USED TO IMPLEMENT STEP 2 ALONE, under a header comment arguing that the Worker and Worklet
       branches were unreachable here because this engine had no WorkerGlobalScope and no WorkletGlobalScope,
       so every environment it had was a Window one. That was true of the tree it was written against and
       stopped being true when a realm gained the ability to state which §3.3.8 [Global] interface its global
       object implements — at which point the missing arms were not a narrowing but a WRONG ANSWER, reached by
       falling through to a step the standard never lets a worker reach. (The retired sentence is paraphrased
       rather than quoted deliberately: double quotes in this tree are a STANDARD's words, and engine/citegen
       reads them as such — it reported this very paragraph as a fabricated Web IDL §3.3.13 quotation when the
       sentence was reproduced verbatim, which is the auditor being right about the notation.) */
    if (realm_global_is_worker(ctx))               /* step 1.2 */
        return realm_owner_is_secure_context(ctx); /* step 1.2.1 / 1.2.2 — see core/realm.h for why a boolean */
    /* STEP 1.3, whose whole body is the answer: "If global is a WorkletGlobalScope, then return true."
       ("Worklets can only be created in secure contexts.") NOTHING IN THIS BUILD REACHES IT — no host states a
       worklet [Global] interface and core/realm.c DFAILs on one — and it is written anyway rather than left
       for the day a worklet realm arrives, because a step omitted here is a step whoever builds that realm has
       to REMEMBER, which is the hand-copied list core/realm.h exists to abolish. It has no operand, so there is
       nothing about it that could be got wrong by writing it early. */
    if (realm_global_is_worklet(ctx))              /* step 1.3 */
        return true;
    /* STEP 2, and STEP 3's `false` as the answer of the predicate it calls. */
    v = realm_top_level_creation_url(ctx);
    url = JS_ToCString(ctx, v);
    /* THE ENVIRONMENT IS THE REALM'S AND IT IS SET WHEN THE REALM IS BUILT, so there is no realm this can be
       asked of that has none — realm_top_level_creation_url asserts that from the other side, including the
       worker case the branch above has already returned for. A ToString over a string runs no page code and
       has nothing to fail with. */
    CHECK(url != NULL, "the realm's top-level creation URL would not convert to a C string");
    r = secure_context_url_potentially_trustworthy(url);
    JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, v);
    return r;
}
