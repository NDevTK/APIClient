/* FETCH §4.3 Scheme fetch — "Switch on request's current URL's scheme and run the associated steps".
 *
 * WHY IT IS A COMPONENT AND NOT AN `if` AT A CALLER. §4.3 is reached by EVERY request: §4.1 "Main fetch"'s
 * step "Return the result of running override fetch given "scheme-fetch" and fetchParams" sits on the arm
 * taken by a same-origin basic request, by a navigate/websocket/webtransport request, AND by a request whose
 * "current URL's scheme is "data"" — which is a row of that switch and not a property of any one API. So the
 * question "who answers these bytes" belongs to the request, and asking it at some entries and not at others
 * is one missing capability wearing two names.
 *
 * WHAT THAT COST, MEASURED RATHER THAN ARGUED. The switch existed TWICE — once inside `fetch()` and once
 * inside XMLHttpRequest's own main-fetch step — and nowhere else. Both had grown a `data` arm; only one had
 * grown a `blob` arm; neither had an `about` arm. Every OTHER entry that builds a request — an `<img>`'s
 * "update the image data", a `<link rel=preload>`, an injected `<script src>`, a document script's slot, a
 * dynamic `import()` — sent the URL to the trusted zone, which can fetch nothing but an HTTP(S) scheme
 * (Fetch §2.1 "URL") and answered `blocked-scheme:data:`. A `data:image/svg+xml` from an injected `src` was
 * therefore a PAGE-VISIBLE LOAD FAILURE, indistinguishable from a network one, for bytes that were already in
 * this address space — and the flow spent a real host round trip to be told so. §4.3 answers it with a 200.
 *
 * SO THE INPUT IS COMPUTED HERE. The switch reads a PARSED URL's scheme — what the URL parser says it is, not
 * what the string starts with — and an entry that parsed it for itself and passed the answer in would be a
 * second answer to the one question this file exists to answer once. The two copies this replaces are exactly
 * that: each parsed, each switched, and they disagreed about `blob:`.
 *
 * AND THE CLOSURE IS AT THE CONSUMER: `scheme_fetch_require_network` (scheme_fetch.h) is what an entry with no
 * local delivery runs, so a route added later CRASHES instead of silently widening the old wrong answer.
 *
 * WHAT IS NOT HERE. §4.1 "Main fetch"'s step 7 — bad port, mixed content, Content Security Policy — is a
 * different step of a different algorithm and stays with its callers; §4.3 runs after it. §6's `data:` URL
 * processor is core/fetch/data_url.c and §4.3 merely runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/fetch/data_url.h"
#include "core/fetch/fetch.h"
#include "core/fetch/headers.h"
#include "core/fetch/scheme_fetch.h"
#include "core/file/blob.h"
#include "core/mime/mime_type.h"
#include "core/url/url.h"

/* §4.3's SWITCH, AS ONE ANSWER. The arms are the standard's own rows in the standard's own order, and the
   fallthrough is its step 4 — "Return a network error" — which is what an unlisted scheme gets. */
typedef enum { ARM_ABOUT, ARM_BLOB, ARM_DATA, ARM_FILE, ARM_HTTP, ARM_UNLISTED } SchemeArm;

static SchemeArm scheme_fetch_arm(const UrlRecord *u)
{
    if (!u->scheme) return ARM_UNLISTED;
    if (!strcmp(u->scheme, "about")) return ARM_ABOUT;
    if (!strcmp(u->scheme, "blob"))  return ARM_BLOB;
    if (!strcmp(u->scheme, "data"))  return ARM_DATA;
    if (!strcmp(u->scheme, "file"))  return ARM_FILE;
    /* Fetch §2.1 "URL": "An HTTP(S) scheme is `http` or `https`." The predicate is core/url/url.c's, so this
       row and `location.protocol`'s termination step read one membership test. */
    if (url_scheme_is_http_s(u->scheme)) return ARM_HTTP;
    return ARM_UNLISTED;
}

/* §4.3's "about" arm. */
static SchemeFetchOutcome scheme_fetch_about(JSContext *ctx, const UrlRecord *rec, JSValue *out_reply)
{
    HeaderList hl = { 0 };
    char *abs;

    /* "If request's current URL's path is the string `blank`" — the PATH, and nothing else. An `about:` URL is
       non-special, so its path is the record's OPAQUE path; a URL written with an authority (`about://x/blank`)
       has a path LIST instead, which is not "the string `blank`", and falls to the network error below.
       IT IS NOT `url_matches_about`. That predicate is HTML's "a URL matches about:blank" relation, which also
       tests the username, password and host — it answers identically on every input this parser can produce
       (an opaque-path URL has no authority to carry one), and it is a DIFFERENT standard's question. §4.3's
       arm is the path test, so the path test is what stands here. */
    if (!rec->opaque_path || strcmp(rec->opaque_path, "blank"))
        return SCHEME_FETCH_NETWORK_ERROR;   /* §4.3's note: "about:config" is a network error when fetched */

    /* "…then return a new response whose status message is `OK`, header list is «(`Content-Type`,
       `text/html;charset=utf-8`)», and body is the empty byte sequence as a body." */
    abs = url_serialize(rec, /*exclude_fragment*/ false);
    CHECK(abs, "scheme fetch: OOM serializing an about: URL for the response's URL list");
    header_list_append(&hl, "content-type", "text/html;charset=utf-8");
    /* §4.2's ESSENCE of the type this arm's own header states. There is no network and no sniff to run: the
       standard writes the header, so the host's computed type IS what the standard wrote. */
    *out_reply = fetch_reply_new(ctx, 200, "OK", &hl, /*body*/ "", /*body_len*/ 0,
                                 (const char *const *)&abs, 1, "text/html");
    header_list_free(&hl);
    free(abs);
    return SCHEME_FETCH_RESPONSE;
}

/* §4.3's "blob" arm. */
static SchemeFetchOutcome scheme_fetch_blob(JSContext *ctx, const FetchRequest *req, const UrlRecord *rec,
                                            JSValueConst blob_entry, JSValue *out_reply)
{
    /* The FRAGMENT is stripped for the lookup and kept on the response's URL: a fragment names a place within a
       resource, not a different entry. */
    char *key = url_serialize(rec, /*exclude_fragment*/ true);
    char *abs = url_serialize(rec, /*exclude_fragment*/ false);
    /* §5.4's CAPTURED ENTRY FIRST — see scheme_fetch.h. `looked` is this scope's own reference and is released
       on every way out; `blob` is the borrowed alias the steps below read through. */
    JSValue looked = JS_IsUndefined(blob_entry) && key ? blob_url_lookup(ctx, key, strlen(key)) : JS_UNDEFINED;
    JSValueConst blob = !JS_IsUndefined(blob_entry) ? blob_entry : looked;
    SchemeFetchOutcome out = SCHEME_FETCH_NETWORK_ERROR;

    CHECK(abs, "scheme fetch: OOM serializing a blob: URL for the response's URL list");
    /* "If request's method is not `GET` or blobURLEntry is null, then return a network error." The standard's
       own note calls the GET restriction interoperability and nothing else, which is why it is stated and not
       reasoned about. */
    if (!JS_IsUndefined(blob) && strcmp(req->method, "GET"))
        blob = JS_UNDEFINED;
    /* "If blob is not a Blob object, then return a network error." It is a STEP of the algorithm and therefore
       an `if` rather than an assert: the store holds what `URL.createObjectURL` put there, and §5.4's captured
       entry is whatever the Request captured, so this is a case the standard HANDLES and not one this engine's
       own logic guarantees away. */
    if (!JS_IsUndefined(blob) && !blob_is(blob))
        blob = JS_UNDEFINED;

    if (!JS_IsUndefined(blob)) {
        size_t blen = 0;
        const char *btype = NULL;
        const char *bytes = blob_bytes_of(blob, &blen, &btype);
        HeaderList bh = { 0 };
        char *len_s;

        /* "If request's header list does not contain `Range`" is the arm below; the OTHERWISE arm is a 206 with
           a sliced body and a `Content-Range`, and this engine builds neither. That is WRONG rather than
           narrower — a page asking for bytes 10-19 of a Blob is handed all of them under a 200 and has no way
           to notice — so it crashes here naming what to build rather than answering plausibly.
           WHAT THE NEXT DIFF BUILDS: §4.3's range steps, over §2.2's "parsing a single range header value"
           (which this engine does not have either) and File API's slice blob, ending in the 206. */
        if (req->headers) {
            char *r = header_list_get(req->headers, "range");
            if (r) {
                free(r);
                DFAIL("Fetch §4.3 Scheme fetch's `blob` arm was given a request carrying a `Range` header — "
                      "its 206/`Content-Range` steps are not built, and the 200 below would answer a partial "
                      "request with the whole blob. Build the range arm: parse a single range header value, "
                      "slice the blob, and set the range-requested flag");
            }
        }

        /* "Set response's status message to `OK`", "Set response's body to bodyWithType's body", and "Set
           response's header list to «(`Content-Length`, serializedFullLength), (`Content-Type`, type)»" — both
           headers, because the standard writes both and a page reads `Content-Length` off a blob response. */
        len_s = malloc(32);
        CHECK(len_s, "scheme fetch: OOM serializing a blob's length");
        snprintf(len_s, 32, "%zu", blen);
        /* BOTH headers, UNCONDITIONALLY, because the standard writes both: a Blob with no type has the EMPTY
           type and gets an empty `Content-Type`, which is a positive statement about the resource rather than a
           header this arm decided to omit. */
        header_list_append(&bh, "content-length", len_s);
        header_list_append(&bh, "content-type", btype ? btype : "");
        /* §4.2's ESSENCE the host computed: a Blob's type IS what this resource is — there is no server to have
           named one and no bytes to sniff that the store did not already label. */
        *out_reply = fetch_reply_new(ctx, 200, "OK", &bh, bytes, blen,
                                     (const char *const *)&abs, 1, btype ? btype : "");
        header_list_free(&bh);
        free(len_s);
        out = SCHEME_FETCH_RESPONSE;
    }

    JS_FreeValue(ctx, looked);
    free(key);
    free(abs);
    return out;
}

/* §4.3's "data" arm. */
static SchemeFetchOutcome scheme_fetch_data(JSContext *ctx, const UrlRecord *rec, JSValue *out_reply)
{
    DataUrlStruct ds;
    SchemeFetchOutcome out = SCHEME_FETCH_NETWORK_ERROR;

    /* "Let dataURLStruct be the result of running the data: URL processor on request's current URL." */
    if (data_url_process(rec, &ds)) {
        /* "Let mimeType be dataURLStruct's MIME type, serialized." … "Return a new response whose status
           message is `OK`, header list is «(`Content-Type`, mimeType)», and body is dataURLStruct's body as a
           body." */
        char *ct = mime_type_serialize(&ds.mime);
        /* §4.2's ESSENCE of that same type. §4.3 says the MIME type IS the one parsed out of the URL, so the
           URL is the authority and this is stated here rather than re-derived by a reader downstream. */
        char *cty = mime_type_essence(&ds.mime);
        char *abs = url_serialize(rec, /*exclude_fragment*/ false);
        HeaderList hl = { 0 };

        CHECK(ct && cty, "scheme fetch: OOM serializing a data: URL's MIME type");
        CHECK(abs, "scheme fetch: OOM serializing a data: URL for the response's URL list");
        header_list_append(&hl, "content-type", ct);
        *out_reply = fetch_reply_new(ctx, 200, "OK", &hl, ds.body, ds.body_len,
                                     (const char *const *)&abs, 1, cty);
        header_list_free(&hl);
        free(abs);
        free(cty);
        free(ct);
        out = SCHEME_FETCH_RESPONSE;
    }
    /* "If dataURLStruct is failure, then return a network error." §6 leaves `ds` initialised-and-empty on
       failure, so this frees it either way (core/fetch/data_url.h). */
    data_url_struct_free(&ds);
    return out;
}

SchemeFetchOutcome scheme_fetch(JSContext *ctx, const FetchRequest *req, JSValueConst blob_entry,
                                JSValue *out_reply)
{
    UrlRecord rec;
    SchemeFetchOutcome out;

    DCHECK(req != NULL && req->url != NULL,
           "scheme fetch was run over no request URL — §4.3 switches on `request's current URL's scheme`, and "
           "a request with no URL has no scheme to switch on");
    DCHECK(req->method != NULL && *req->method,
           "scheme fetch was run over a request that states no METHOD — the `blob` arm's first step is `If "
           "request's method is not `GET` … return a network error`, so an unstated method makes that step "
           "read a field the component that built this request dropped");
    DCHECK(out_reply != NULL, "scheme fetch was given nowhere to put the response it may build");

    url_record_init(&rec);
    /* THE PARSE IS THIS COMPONENT'S — see the file comment. It is core/fetch's own, so the base a relative
       endpoint resolves against is the one HTML's API base URL names, exactly as it is at every other Fetch
       entry point. A parse that REFUSES the string names no scheme, so it falls to the unlisted arm below and
       becomes §4.3's network error — which is what a browser does with a string that is not a URL. */
    if (!fetch_parse_url(ctx, &rec, req->url, strlen(req->url))) {
        url_record_free(&rec);
        return SCHEME_FETCH_NETWORK_ERROR;
    }

    switch (scheme_fetch_arm(&rec)) {
    case ARM_ABOUT: out = scheme_fetch_about(ctx, &rec, out_reply); break;
    case ARM_BLOB:  out = scheme_fetch_blob(ctx, req, &rec, blob_entry, out_reply); break;
    case ARM_DATA:  out = scheme_fetch_data(ctx, &rec, out_reply); break;
    /* §4.3's "file" arm carries no normative steps at all — "For now, unfortunate as it is, file: URLs are left
       as an exercise for the reader. When in doubt, return a network error." — so control reaches the
       algorithm's own step 4 with it, which is the network error below. */
    case ARM_FILE:  out = SCHEME_FETCH_NETWORK_ERROR; break;
    case ARM_HTTP:  out = SCHEME_FETCH_NETWORK; break;
    /* §4.3 step 4: "Return a network error." Every scheme the switch does not list — `ws:`, `chrome:`,
       `javascript:`, an application's own — is answered HERE and never sent to the host, which could only
       refuse it. */
    default:        out = SCHEME_FETCH_NETWORK_ERROR; break;
    }
    url_record_free(&rec);
    DCHECK(out != SCHEME_FETCH_RESPONSE || JS_IsObject(*out_reply),
           "scheme fetch reported that it built a response and did not build one — the reply record is what "
           "the caller's processResponse steps read, and an arm that returns RESPONSE without one settles a "
           "flow with a value that is not a reply");
    return out;
}

bool scheme_fetch_answer(JSContext *ctx, JSValueConst deliver, const FetchRequest *req, JSValueConst blob_entry)
{
    JSValue reply = JS_UNDEFINED;
    SchemeFetchOutcome out = scheme_fetch(ctx, req, blob_entry, &reply);
    JSValue delivered;

    if (out == SCHEME_FETCH_NETWORK)
        return false;

    /* §4.3's network error crosses as the JSON `null` a host delivers for one (core/fetch/fetch.h), so the
       caller's processResponse steps have ONE shape and cannot tell a locally-refused scheme apart from a
       refused network — which is right, because the standard does not either. */
    delivered = out == SCHEME_FETCH_RESPONSE ? reply : JS_NULL;
    /* AS A FLOW, never a plain call. The delivery runs the CALLER'S processResponse steps, and for `fetch()`
       those settle a promise — 27.5.1.3 "CreateResolvingFunctions"' resolveSteps read `Get(resolution, "then")`
       off a Response whose prototype the page owns, so this is the page's code and needs a flow base under it.
       A C activation with no flow base is the drive-to-completion this engine aborts on. */
    if (JS_CallAsFlow(ctx, deliver, delivered) < 0) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);   /* the caller's own steps threw; that is their completion, not this call's */
    }
    JS_FreeValue(ctx, reply);
    return true;
}

void scheme_fetch_require_network(JSContext *ctx, const char *url)
{
#if APICLIENT_DEV
    UrlRecord rec;
    SchemeArm arm;

    DCHECK(url != NULL && *url, "an entry required a network scheme of no URL at all");
    url_record_init(&rec);
    /* A string the parser REFUSES names no scheme and is not §4.3's business — a concolic's display shape parks
       here and it is not a URL (core/fetch/fetch.c's projection). It passes; what does not is a real URL whose
       real scheme §4.3 answers inside this agent. */
    if (fetch_parse_url(ctx, &rec, url, strlen(url))) {
        arm = scheme_fetch_arm(&rec);
        DCHECK(arm == ARM_HTTP,
               "a request whose URL's scheme Fetch §4.3 Scheme fetch answers INSIDE this agent was parked on "
               "the HOST'S network instead. The trusted zone can fetch nothing but an HTTP(S) scheme "
               "(Fetch §2.1 URL) and answers a refusal the page cannot tell from a network failure, so this "
               "flow is parked on an answer that can only ever be `blocked`. Run scheme_fetch at the entry "
               "that built this request and DELIVER its response: the parks that cannot yet are the ones with "
               "no `deliver` closure — an external <script src>, a document script's slot and a dynamic "
               "import() — whose local answer is a reply record placed on the pending entry itself "
               "(solver/pending.h's PEND_VALUE with haveValue, which the scheduler delivers without ever "
               "showing the host a request)");
    }
    url_record_free(&rec);
#else
    (void)ctx; (void)url;
#endif
}
