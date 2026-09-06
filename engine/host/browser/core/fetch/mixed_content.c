/* MIXED CONTENT §4.1, §4.3 and §4.4 — see mixed_content.h, including why every number here is counted and
   never checked. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/fetch/mixed_content.h"
#include "core/frame/secure_context.h"
#include "core/realm.h"
#include "core/url/origin.h"
#include "core/url/url.h"

bool mixed_content_settings_prohibit(JSContext *ctx)
{
    /* STEP 1: "If settings' origin is a potentially trustworthy origin, then return Prohibits Mixed Security
       Contexts." THE SETTINGS OBJECT'S ORIGIN IS THIS AGENT'S — an instance is an origin-keyed agent cluster
       (SECURITY.md), so "this document's origin" and "this agent's origin" are one fact and core/url/origin.h
       says so at the accessor. It is the ORIGIN and not the document's ADDRESS: §3.2 over a URL gives
       `about:blank` and `data:` a free pass that only makes sense for an environment's creation URL, and an
       `about:blank` iframe of an http page would answer trustworthy through it. */
    if (secure_context_origin_record_potentially_trustworthy(origin_agent())) return true;

    /* STEP 2 is "If settings' global object is a window", and its 2.2 walks the ancestor navigables. Every
       ancestor in THIS instance shares the origin step 1 just asked about, so the walk can only add an answer
       for an ancestor in a peer instance — which is the named residual in mixed_content.h. Nothing is written
       here for the window test either: a worker's ancestors are not navigables at all, so step 2 is entered
       only for a Window global, and with 2.2 unbuildable the arm has no body that would differ. */
    (void)realm_global_is_worker;   /* step 2's test has nothing to guard while 2.2 is the residual */

    return false;   /* STEP 3: "Return Does Not Restrict Mixed Security Contexts." */
}

bool mixed_content_should_block_fetching(JSContext *ctx, const UrlRecord *url, const char *destination,
                                         bool has_parent)
{
    char *serialized;
    bool trustworthy;

    DCHECK(url != NULL,
           "§4.4 was asked about a request with no parsed URL — its step 1.2 reads whether that URL is a "
           "potentially trustworthy URL, so a caller with none has not resolved the address it is fetching");
    DCHECK(destination != NULL,
           "§4.4 was asked about a request stating no DESTINATION — Fetch §2.2.5 \"Requests\" gives every "
           "request one and step 1.4 compares it against `document`, so a request without one would be "
           "compared against a pointer nobody wrote");

    /* STEP 1.1: "§ 4.3 Does settings prohibit mixed security contexts? returns Does Not Restrict Mixed
       Security Contexts when applied to request's client." An insecure page's own mixed content is not mixed
       content at all — this is the arm that makes an http document's http subresources ordinary. */
    if (!mixed_content_settings_prohibit(ctx)) return false;

    /* STEP 1.2: "request's URL is a potentially trustworthy URL." Secure Contexts §3.2, over the address this
       request is actually going to — which is the address AFTER §4.1's upgrade, because Fetch runs that at
       step 6 and this at step 7. A caller that asks this of the pre-upgrade address refuses what a browser
       loads (mixed_content.h). §3.2 takes a serialization here because that is the entry this tree has and
       the record is the caller's; the round trip is exact for a record the parser produced. */
    serialized = url_serialize(url, /*exclude_fragment*/ false);
    CHECK(serialized != NULL, "Mixed Content §4.4: OOM serializing a request's URL for Secure Contexts §3.2");
    trustworthy = secure_context_url_potentially_trustworthy(serialized);
    free(serialized);
    if (trustworthy) return false;

    /* STEP 1.3 is "The user agent has been instructed to allow mixed content, as described in § 7.2 User
       Controls", and this user agent HAS NO USER to instruct it. That is a statement rather than a step
       skipped: §7.2 describes a per-origin control a person grants in a browser's own UI, and inventing one
       would make this engine's answer depend on something no page can observe — the same reason Secure
       Contexts §3.1's steps 7 and 8 are unreachable here and say so at their own site. */

    /* STEP 1.4: "request's destination is `document`, and request's target browsing context has no parent
       browsing context." The top-level-navigation exemption, and BOTH halves are required — a `document`
       destination for a CHILD navigable is still mixed content, which is the ordinary `<iframe src="http://…">`
       inside an https page. The note under it says user agents MAY additionally enforce on insecure form
       submissions; this one does not, which is the standard's permission and not a gap. */
    if (!strcmp(destination, "document") && !has_parent) return false;

    return true;   /* STEP 2: "Return blocked." */
}

char *mixed_content_upgrade_url(JSContext *ctx, const char *url, const char *destination,
                                const char *initiator)
{
    UrlRecord u;
    char *out = NULL;

    DCHECK(url != NULL, "§4.1 was asked to upgrade no address at all — every caller resolved one before it "
                        "built the request this step is about");
    DCHECK(destination != NULL,
           "§4.1 was asked about a request stating no DESTINATION — Fetch §2.2.5 \"Requests\" gives every "
           "request one and step 1.4 is a membership test over it, so a request without one decides this "
           "step's whole answer on a pointer nobody wrote");

    /* STEP 1.4 FIRST, BECAUSE IT IS THE ONLY CONDITION THAT NEEDS NO PARSE and it excludes almost every
       request this engine makes: "request's destination is not `image`, `audio`, or `video`" ends the
       algorithm. Reading it before the parse is not an optimisation dressed as an order — the four remaining
       conditions all read a URL RECORD, so this is the boundary between "no work" and "parse the address". */
    if (strcmp(destination, "image") && strcmp(destination, "audio") && strcmp(destination, "video"))
        return NULL;

    /* STEP 1.5: "request's destination is `image` and request's initiator is `imageset`." The note says why —
       "For historical reasons, `imageset` is not upgraded" — so this is a carve-out that must be honoured
       rather than reasoned about: an `<img srcset>` on an https page is BLOCKED by §4.4 where a plain
       `<img src>` is upgraded and loaded, and treating the two alike loses one of them either way. */
    if (!strcmp(destination, "image") && initiator != NULL && !strcmp(initiator, "imageset")) return NULL;

    /* STEP 1.3: "§ 4.3 … returns Does Not Restrict Mixed Security Contents when applied to request's client."
       An insecure page's http subresources are not upgraded, which is the same arm §4.4 step 1.1 reads and
       the reason a plain http page's images keep loading over http. */
    if (!mixed_content_settings_prohibit(ctx)) return NULL;

    url_record_init(&u);
    /* AN ADDRESS THAT DOES NOT PARSE IS NOT UPGRADED, which is the behaviour Fetch §4.1 step 7's own component
       already has and is right for the same reason: this step is a question about a request's URL, and a
       string that names none is a failure the caller's own algorithm answers. */
    if (!url_parse(&u, url, strlen(url), NULL)) { url_record_free(&u); return NULL; }

    /* STEP 1.1: "request's URL is a potentially trustworthy URL." An https image is already where the upgrade
       would put it. STEP 1.2: "request's URL's host is an IP address." §4.1 excludes both IPv4 and IPv6
       literals — an address with no name cannot have been meant to be reached by one, and upgrading it would
       demand a certificate for a number. url.c's parser has already resolved `0x7f.1` to the IPv4 NUMBER, so
       this is a question about the parsed host and not about the bytes the author typed. */
    if (u.host.kind != URL_HOST_IPV4 && u.host.kind != URL_HOST_IPV6 &&
        !secure_context_url_potentially_trustworthy(url) &&
        u.scheme && !strcmp(u.scheme, "http")) {
        /* STEP 2: "If request's URL's scheme is http, set request's URL's scheme to https, and return."
           THE PORT IS DELIBERATELY UNTOUCHED, which is the standard's own note: "we do not modify the port
           because it will be set to null when the scheme is http, and interpreted as 443 once the scheme is
           changed to https". So `http://x/` becomes `https://x/` and `http://x:8080/` keeps its 8080 — the
           parser has already normalised a default `:80` to the null port, so nothing here has to. */
        free(u.scheme);
        u.scheme = strdup("https");
        CHECK(u.scheme != NULL, "Mixed Content §4.1: OOM upgrading a mixed content request's scheme");
        out = url_serialize(&u, /*exclude_fragment*/ false);
        CHECK(out != NULL, "Mixed Content §4.1: OOM serializing an upgraded mixed content request's URL");
    }
    url_record_free(&u);
    return out;
}
