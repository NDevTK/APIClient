/* SECURE CONTEXTS §3.1 and §3.2, and HTML §8.1.3.5 — see secure_context.h. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/secure_context.h"
#include "core/realm.h"
#include "core/url/url.h"

/* §3.1 STEPS 3 THROUGH 8, OVER A TUPLE ORIGIN. Split out from the algorithm below because step 1's opaque
   case reaches them by TWO routes — a URL that has a tuple origin of its own, and a `blob:` URL whose origin
   is the one its path spells — and the alternative was a self-call whose depth would be a fact about the
   input. Depth is not a thing a page gets to choose in this engine. */
static bool tuple_origin_trustworthy(const UrlRecord *u)
{
    const char *host;
    size_t n;

    DCHECK(u->scheme != NULL, "a tuple origin with no scheme — §3.1 step 2 asserts this is a tuple origin, and "
                              "a record with no scheme has no origin at all");

    /* STEP 3: "If origin's scheme is either `https` or `wss`, return Potentially Trustworthy." NEITHER the
       port NOR the host has any say — the spec's own closing note says so, which is why a `https://` on a
       non-default port is still trustworthy. */
    if (!strcmp(u->scheme, "https") || !strcmp(u->scheme, "wss")) return true;

    /* STEP 4: "If origin's host matches one of the CIDR notations 127.0.0.0/8 or ::1/128". A CIDR match over
       the parsed host, not a string compare against "127.0.0.1": url.c's parser resolves `http://0x7f.1/` to
       the IPv4 NUMBER 127.0.0.1 and `http://[0:0::1]/` to the IPv6 address ::1, and a text test would call
       both of those untrusted while a browser calls them loopback. */
    if (u->host.kind == URL_HOST_IPV4 && (u->host.ipv4 >> 24) == 127) return true;
    if (u->host.kind == URL_HOST_IPV6) {
        int i;
        bool is_loopback = u->host.ipv6[7] == 1;
        for (i = 0; i < 7; i++) if (u->host.ipv6[i] != 0) is_loopback = false;
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
    if (u->host.kind == URL_HOST_DOMAIN) {
        host = u->host.domain;
        DCHECK(host != NULL, "a URL_HOST_DOMAIN host with no domain string — url.c's parser produces the kind "
                             "and the bytes together");
        n = strlen(host);
        if (n && host[n - 1] == '.') n--;                       /* the trailing-dot ("localhost.") spelling */
        if (n == 9 && !memcmp(host, "localhost", 9)) return true;
        if (n > 10 && !memcmp(host + n - 10, ".localhost", 10)) return true;
    }

    /* STEP 6: "If origin's scheme is `file`, return Potentially Trustworthy." The spec's own note is that a
       user agent MAY be stricter; this one is not, because a file document is exactly the case a developer
       runs a bundle from before deploying it, and refusing it would hide that bundle's gated code. */
    if (!strcmp(u->scheme, "file")) return true;

    /* STEPS 7 AND 8 have no answer to give here, and that is a statement rather than an omission. Step 7 is
       "a scheme the user agent considers authenticated" — §7.1's packaged applications, `app:` and
       `chrome-extension:`, and this user agent packages nothing. Step 8 is "an origin CONFIGURED as
       trustworthy" — §7.2's development-environment override, which is a user preference and this engine has
       no user. Inventing either would make the answer depend on something no page can observe. */
    return false;   /* STEP 9 */
}

bool secure_context_origin_potentially_trustworthy(const UrlRecord *u)
{
    DCHECK(u != NULL, "§3.1 was asked about no origin at all");

    /* STEP 1: "If origin is an opaque origin, return Not Trustworthy." An origin is a TUPLE exactly for the
       URL Standard's special schemes (§4.2: ftp, file, http, https, ws, wss) — every other scheme has an
       opaque origin, which is what makes `about:`, `javascript:` and a bare `data:` untrusted here. `file` is
       in that set because §3.1 STEP 6 names it, and a step that could never be reached would not have been
       written.
       `blob:` IS THE ONE URL WHOSE ORIGIN IS SOMEBODY ELSE'S — URL §4.7 gives it the origin of the URL its
       path spells, which is precisely why §3.2's note says a blob created in a trustworthy origin is itself
       trustworthy. WHICH inner schemes count is §4.7's rule and url.c is the component that states it, so
       this asks url.c for the origin and reads the answer back rather than keeping a second scheme list that
       could disagree with the first. */
    if (!u->scheme) return false;
    if (!url_scheme_is_special(u->scheme)) {
        char *serialized = url_serialize_origin(u);
        UrlRecord tuple;
        bool r = false;

        if (serialized && strcmp(serialized, "null") != 0) {
            url_record_init(&tuple);
            CHECK(url_parse(&tuple, serialized, strlen(serialized), NULL),
                  "a serialized TUPLE origin did not parse back — §4.7 serializes one as scheme://host[:port], "
                  "which the same parser produced it from");
            DCHECK(tuple.scheme && url_scheme_is_special(tuple.scheme),
                   "a tuple origin serialized with a scheme that is not special — §4.7 gives a tuple origin "
                   "only to a special scheme, and anything else here would send this back through the opaque "
                   "branch it just came out of");
            r = tuple_origin_trustworthy(&tuple);
            url_record_free(&tuple);
        }
        free(serialized);
        return r;
    }
    return tuple_origin_trustworthy(u);   /* STEP 2's assert holds: a special scheme has a tuple origin */
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
    r = url_is_about(&u, "blank") || url_is_about(&u, "srcdoc") ||
        (u.scheme && !strcmp(u.scheme, "data")) ||
        secure_context_origin_potentially_trustworthy(&u);   /* STEP 3 */
    url_record_free(&u);
    return r;
}

bool secure_context_is(JSContext *ctx)
{
    JSValue v = realm_top_level_creation_url(ctx);
    const char *url = JS_ToCString(ctx, v);
    bool r;

    /* THE ENVIRONMENT IS THE REALM'S AND IT IS SET WHEN THE REALM IS BUILT, so there is no realm this can be
       asked of that has none — realm_top_level_creation_url asserts that from the other side. A ToString over
       a string runs no page code and has nothing to fail with. */
    CHECK(url != NULL, "the realm's top-level creation URL would not convert to a C string");
    r = secure_context_url_potentially_trustworthy(url);
    JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, v);
    return r;
}
