/* THE LOCATION INTERFACE — Blink core/frame, and the place the two halves meet.
 *
 * Half of it is the PRINCIPAL and is CONCRETE: origin, protocol, host, hostname, port, pathname. A bundle
 * builds its request URLs out of these (`location.origin + "/api/x"`), so a concolic origin would turn every
 * endpoint into a shape and lose the very values this tool reports. CLAUDE.md says so directly: the principal
 * is concrete for URL building.
 *
 * The other half is ATTACKER INPUT and is CONCOLIC: `search` and `hash` are whatever the attacker puts in the
 * URL they get someone to click, so they are domain-carrying and example-free, they must never force a branch,
 * and they are the @S sources a breakout is solved for. They are NOT the same source: the browser
 * percent-encodes them by DIFFERENT sets (verified on Chrome — both encode `< > "` and space; the FRAGMENT
 * additionally encodes backtick and NOT `'`, the special-scheme QUERY encodes `'` and NOT backtick), so a
 * candidate that breaks out through one may be dead through the other and they carry separate identities.
 *
 * `href` is the address as loaded: origin + pathname, concrete. It does NOT splice `search`/`hash` in, because
 * this is a data property read once at install and a page that reads href does not thereby read the attacker's
 * query — a bundle routing on `location.href` is reading the address it was served at. When href becomes an
 * ACCESSOR that recomputes (it must, once assignment to location is modelled), the two concolic halves belong
 * in it and they will propagate through the interpreter's own `+` with no special case here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "core/frame/location.h"

/* Split `url` into its concrete parts. Everything from `?` or `#` on is the attacker's and is not parsed here. */
typedef struct {
    char protocol[16];   /* "https:" */
    char host[256];      /* "e.test:8443" */
    char hostname[256];
    char port[8];
    char origin[300];    /* "https://e.test:8443" */
    char pathname[512];  /* "/a/b" — "/" when the address names none */
} LocParts;

static void loc_split(const char *url, LocParts *p)
{
    const char *scheme_end, *host_start, *host_end, *colon, *path_end;
    size_t n;

    memset(p, 0, sizeof(*p));
    scheme_end = strstr(url, "://");
    CHECK(scheme_end != NULL, "a document address with no scheme reached Location — the host captured a URL "
                              "this engine cannot make a principal out of");
    n = (size_t)(scheme_end - url) + 1;                    /* include the ':' */
    CHECK(n < sizeof(p->protocol), "the document's scheme is longer than any real one");
    memcpy(p->protocol, url, n);

    host_start = scheme_end + 3;
    host_end = host_start + strcspn(host_start, "/?#");
    n = (size_t)(host_end - host_start);
    CHECK(n < sizeof(p->host), "the document's host is longer than any real one");
    memcpy(p->host, host_start, n);

    /* hostname/port split on the LAST colon, which is what an IPv6 literal's brackets keep unambiguous. */
    colon = strrchr(p->host, ':');
    if (colon && !strchr(colon, ']')) {
        memcpy(p->hostname, p->host, (size_t)(colon - p->host));
        snprintf(p->port, sizeof(p->port), "%s", colon + 1);
    } else {
        snprintf(p->hostname, sizeof(p->hostname), "%s", p->host);
    }
    snprintf(p->origin, sizeof(p->origin), "%s//%s", p->protocol, p->host);

    if (*host_end == '/') {
        path_end = host_end + strcspn(host_end, "?#");
        n = (size_t)(path_end - host_end);
        CHECK(n < sizeof(p->pathname), "the document's path is longer than this engine models");
        memcpy(p->pathname, host_end, n);
    } else {
        p->pathname[0] = '/';   /* 4.7.1: an address naming no path has "/" */
    }
}

void location_install(JSContext *ctx, JSValueConst global, const char *url)
{
    LocParts p;
    JSValue loc, search, hash;

    if (!url || !*url)
        return;   /* no address, no Location — the page's own throw is the honest answer */
    loc_split(url, &p);

    loc = JS_NewObject(ctx);
    CHECK(!JS_IsException(loc), "the Location allocation failed");

    JS_SetPropertyStr(ctx, loc, "protocol", JS_NewString(ctx, p.protocol));
    JS_SetPropertyStr(ctx, loc, "host",     JS_NewString(ctx, p.host));
    JS_SetPropertyStr(ctx, loc, "hostname", JS_NewString(ctx, p.hostname));
    JS_SetPropertyStr(ctx, loc, "port",     JS_NewString(ctx, p.port));
    JS_SetPropertyStr(ctx, loc, "origin",   JS_NewString(ctx, p.origin));
    JS_SetPropertyStr(ctx, loc, "pathname", JS_NewString(ctx, p.pathname));

    /* Attacker input, separate identities because the browser encodes them by different sets. Example-free:
       nothing about the address tells this engine what an attacker WILL put there, and inventing one would be
       a fabricated observation. */
    search = concolic_new(ctx, "{location.search}", "location.search", JS_UNDEFINED);
    hash   = concolic_new(ctx, "{location.hash}",   "location.hash",   JS_UNDEFINED);
    JS_SetPropertyStr(ctx, loc, "search", search);
    JS_SetPropertyStr(ctx, loc, "hash",   hash);

    /* href is origin + pathname + the two attacker parts. Built by the interpreter's own concatenation, so the
       concolic halves propagate into it the way every other `+` propagates them — no special case. */
    {
        char href[sizeof(p.origin) + sizeof(p.pathname)];
        snprintf(href, sizeof(href), "%s%s", p.origin, p.pathname);
        JS_SetPropertyStr(ctx, loc, "href", JS_NewString(ctx, href));
    }

    JS_SetPropertyStr(ctx, (JSValue)global, "location", loc);
}
