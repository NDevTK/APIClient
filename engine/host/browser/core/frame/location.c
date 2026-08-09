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
#include "core/url/url.h"
#include "core/dom/document.h"   /* this realm's address — the one place a document's URL lives */

/* HTML's API BASE URL IS NOT KEPT HERE. It is the DOCUMENT's address and is read from the Document record
   (document_base_url), because it is a per-REALM fact: a module-static copy was overwritten by every install,
   so materializing a same-origin popup rewrote its OPENER's base and the opener's next relative fetch resolved
   against the popup's address. That string was the whole of this component's agent-lifetime state, so its
   teardown went with it rather than staying as an empty function three hosts call. */

/* Install a member from a malloc'd serialization and take ownership of it — every one of Location's concrete
   members is one of these, so the free belongs here rather than at six call sites. */
static void loc_put(JSContext *ctx, JSValueConst loc, const char *name, char *owned)
{
    JS_SetPropertyStr(ctx, (JSValue)loc, name, JS_NewString(ctx, owned));
    free(owned);
}

/* §7.10.5's STRINGIFIER: `[LegacyUnforgeable] stringifier attribute USVString href`. A stringifier attribute
   gives the interface a `toString()` that answers that attribute, which is what makes a Location usable
   ANYWHERE a USVString is expected — and that is not a corner: `new URL(path, location)` is how a page builds
   an absolute URL from a relative one, and WPT's own /common/dispatcher/dispatcher.js opens with exactly it.
   Without the stringifier, ToString found Object.prototype.toString and handed the URL parser the eight
   characters `[object Object]`, so the constructor threw "the base URL is not a valid URL" — four webmessaging
   files ended at their first import.
   Read at CALL time rather than captured at install, because it must answer whatever the address is then; the
   href member beside it is the one place the address lives. */
static JSValue js_loc_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_GetPropertyStr(ctx, this_val, "href");
}

static char *loc_concat(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    char *r = malloc(na + nb + 1);
    CHECK(r, "location: OOM building an address");
    memcpy(r, a, na);
    memcpy(r + na, b, nb);
    r[na + nb] = 0;
    return r;
}

/* THE COMPONENT PART OF THIS REALM'S ADDRESS, prefixed as §7.10.5 serializes it ("?x=1", "#frag") and empty
   when the address has none. Read from the DOCUMENT rather than captured at install for the reason the API
   base URL is: the address is a per-realm fact and a module-static copy is overwritten by the next install. */
static JSValue loc_component_example(JSContext *ctx, bool want_query)
{
    UrlRecord rec;
    const char *addr = document_base_url(ctx);
    JSValue v;

    DCHECK(addr != NULL && *addr, "a Location member was read in a realm whose Document has no address");
    url_record_init(&rec);
    CHECK(url_parse(&rec, addr, strlen(addr), NULL),
          "this realm's own address does not parse — it was parsed once already to install this Location");
    {
        const char *part = want_query ? rec.query : rec.fragment;
        if (!part) {
            v = JS_NewStringLen(ctx, "", 0);
        } else {
            char *pref = loc_concat(want_query ? "?" : "#", part);
            v = JS_NewString(ctx, pref);
            free(pref);
        }
    }
    url_record_free(&rec);
    return v;
}

/* THE TWO ATTACKER SOURCES, AND THEY CARRY THE EXAMPLE THE ADDRESS ACTUALLY HAS. Both were minted example-free,
   which says "nothing is known about this value" — but the document was LOADED from an address, and whatever
   query and fragment it carries are observed facts, not invented ones. §solver's rule is that a value is a
   domain plus an example when the code pins, computes or LEARNS one, and this one is learned from the address
   the host supplied; every other source in this engine that has one already supplies it (window.name, screen,
   navigator). Example-free is not neutral either: it made the value unusable wherever a real string was needed,
   and the conformance host answered that by building a SECOND Location out of the address — two components
   answering `location`, so a fix to one silently missed the other, which is exactly how the stringifier landed
   in a file the conformance runner does not use.
   The value stays CONCOLIC: the domain is unconstrained and a branch on it still forks, which is the whole
   point of the source. It simply also knows what it concretely is right now. */
static JSValue js_loc_get_search(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return concolic_source_wrap(ctx, "{location.search}", "location.search", loc_component_example(ctx, true));
}

static JSValue js_loc_get_hash(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return concolic_source_wrap(ctx, "{location.hash}", "location.hash", loc_component_example(ctx, false));
}

/* THE TWO ATTACKER SOURCES THIS COMPONENT OWNS, declared ONCE PER AGENT — a source's browser delivery is a
   fact about the COMPONENT, not about a document, so it belongs beside the class registrations and not in the
   per-realm install. A second same-origin document re-declaring it is what caught this.
   THE TWO SETS, verified on Chrome and stated in CLAUDE.md: both components encode space, `"`, `<` and `>`;
   the FRAGMENT additionally encodes the backtick and NOT the apostrophe, and the special-scheme QUERY encodes
   the apostrophe and `#` and NOT the backtick. That difference is the whole reason these are two sources rather
   than one — the same candidate is a live JS-context breakout through the fragment and a dead one through the
   query, and vice versa for a template-literal context. Declared here, where the component that owns the source
   is, rather than known to the solver. */
void location_init(JSContext *ctx)
{
    (void)ctx;
    concolic_declare_source("location.hash", " \"<>`", '#');
    concolic_declare_source("location.search", " \"#<>'", '?');
}

void location_install(JSContext *ctx, JSValueConst global, const char *url)
{
    UrlRecord rec;
    JSValue loc;
    char *s;

    /* EVERY DOCUMENT HAS AN ADDRESS — `about:blank` is one, and a document created from no response is created
       AT one. This used to return quietly for an empty string, which is the defensive branch that hid the
       caller's bug rather than the caller's bug: the shipped host was passing this the document's ORIGIN, and
       an origin is never empty, so the guard never fired and never could have said so. */
    DCHECK(url != NULL && *url,
           "a Location was installed for a document with no address — every document is loaded FROM somewhere, "
           "so a host with nothing to pass here has not decided what document this realm is");

    /* THE ADDRESS GOES THROUGH THE REAL PARSER. A hand-rolled splitter stood here — strstr("://"),
       strrchr(':'), fixed 256-byte buffers — and it was wrong wherever a URL is interesting: it CHECK-failed
       on an address with no "://", it read `http://0x7f.1/` as the domain `0x7f.1` rather than the address
       127.0.0.1, and its port split guessed at IPv6 brackets. It is deleted rather than kept for the common
       case, because the common case is exactly where two parsers agree and the rare one is where the page's
       origin comparison goes wrong. */
    CHECK(url_parse(&rec, url, strlen(url), NULL),
          "the document address is not a URL — the host captured something this engine cannot make a "
          "principal out of");

    loc = JS_NewObject(ctx);
    CHECK(!JS_IsException(loc), "the Location allocation failed");

    loc_put(ctx, loc, "protocol", loc_concat(rec.scheme, ":"));
    loc_put(ctx, loc, "host",     url_serialize_host_port(&rec));
    loc_put(ctx, loc, "hostname", url_serialize_host(&rec.host));
    loc_put(ctx, loc, "port",     url_serialize_port(&rec));
    loc_put(ctx, loc, "origin",   url_serialize_origin(&rec));
    loc_put(ctx, loc, "pathname", url_serialize_path(&rec));

    /* Attacker input, separate identities because the browser encodes them by different sets. Example-free:
       nothing about the address tells this engine what an attacker WILL put there, and inventing one would be
       a fabricated observation. */
    /* GETTERS, not values. A candidate run substitutes a source with a breakout at MINT time, so a source that
       is minted once at install can never receive one — its sink would be detected and never fire-verified.
       Reading it per access also matches the interface: location.hash is whatever the address says NOW. */
    JS_DefinePropertyGetSet(ctx, loc, JS_NewAtom(ctx, "search"),
                            JS_NewCFunction(ctx, js_loc_get_search, "get search", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_DefinePropertyGetSet(ctx, loc, JS_NewAtom(ctx, "hash"),
                            JS_NewCFunction(ctx, js_loc_get_hash, "get hash", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);

    /* href is the address as loaded, which is the serialization with the query and the fragment left off —
       those two are the attacker's and are read through their own getters. */
    {
        char *origin = url_serialize_origin(&rec), *path = url_serialize_path(&rec);
        s = loc_concat(origin, path);
        free(origin); free(path);
        loc_put(ctx, loc, "href", s);
    }

    /* [LegacyUnforgeable], which for Web IDL means an OWN property that cannot be reconfigured or overwritten —
       a page must not be able to make its own Location lie about its address, which is precisely what a
       replaceable `toString` would let a script do to every same-realm check written against it. */
    JS_DefinePropertyValueStr(ctx, loc, "toString",
                              JS_NewCFunction(ctx, js_loc_to_string, "toString", 0), 0);

    url_record_free(&rec);
    JS_SetPropertyStr(ctx, (JSValue)global, "location", loc);
}
