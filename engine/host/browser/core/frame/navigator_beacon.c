/* BEACON §2.1 "sendBeacon() Method" and §3 "Processing Model" — Blink modules/beacon.
 *
 * WHY THIS MEMBER IS NOT A FIDELITY FOOTNOTE. `sendBeacon(url, data)` IS a request: a URL the page composed
 * and a body the page composed, POSTed. Analytics, telemetry, error reporters and session-end reporting reach
 * their collector through this member and through nothing else, so an engine without it loses the endpoint
 * TWICE — the address is never recorded, and the call throws `TypeError: navigator.sendBeacon is not a
 * function`, which aborts the flow at that line and takes everything after it with it.
 *
 * THE REQUEST IS DERIVED AND NEVER SENT, AND THAT IS A STRUCTURAL FACT RATHER THAN A CHECK. HTML §4.10.22.3
 * "Form submission algorithm" is the same shape and core/html/html_form.c says so at the same place: the
 * operation composes the request and ends at `endpoint_record`, and there is simply no network seam on the
 * path — its step 26's plan-to-navigate is the one step that is not performed. Read the two rules
 * this sits between, because they look opposed and are about different questions:
 *
 *   WHETHER A REQUEST EXISTS AT ALL is the browser half's model of the operation, and it is decided ONCE, at
 *   the definition, by the SPEC. §3's request initialization writes `method: POST` as a constant — §1's own
 *   list of non-goals says the method is not customizable — so "is this state-mutating?" is answered by the
 *   standard and not by a value a page computed. There is no predicate to write and no per-call decision that
 *   could be wrong: this file calls no FetchProvider, so no `if` can be bypassed, mis-ordered or forgotten.
 *
 *   WHETHER A REQUEST THE ENGINE ASKS FOR MAY GO OUT is the TRUSTED ZONE's, and that is what "a safety `if`
 *   inside the engine is a layering violation" forbids. `fetch(url, {method: m})` takes its method from PAGE
 *   DATA, so the question is per call — and the engine must not answer it. It hands the whole request, method
 *   and body included, to `FetchProvider.owe`, and `extension/lib/safe-fetch.js` decides. A method predicate
 *   written inside the sandbox would be both a layering violation and worthless: the sandbox is attacker-
 *   controlled, so the trusted zone has to apply the rule anyway.
 *
 * AND THE NON-DELIVERY IS NOT OBSERVABLE THROUGH THIS MEMBER'S OWN CONTRACT, which is why this is a faithful
 * implementation rather than a hole. §2.1.4 "Return Value" defines the boolean as whether the user agent
 * "is able to successfully queue the data for transfer", and states outright that "since the actual data
 * transfer happens asynchronously, this method does not provide any information whether the data transfer has
 * succeeded or not". A page cannot tell a user agent that queued and never dispatched from one that did.
 *
 * THE BOOLEAN IS COMPUTED, NEVER A CONSTANT, and it splits exactly where the knowledge does. §3 step 6.2
 * refuses a payload that exceeds the keepalive quota, which Fetch §4.6 "HTTP-network-or-cache fetch" states as
 * `contentLength + inflightKeepaliveBytes > 64 kibibytes`. `inflightKeepaliveBytes` is a sum over the fetch
 * group's in-flight keepalive requests — network timing this engine does not model — so:
 *
 *   - a body LARGER than the quota answers a CONCRETE `false`. The sum is never smaller than contentLength, so
 *     every world agrees and there is no other arm for a branch to reach;
 *   - anything else answers a CONCOLIC boolean whose example is `true` (what an idle fetch group returns), and
 *     BOTH arms are then reachable. That is not pedantry: `if (!navigator.sendBeacon(u, d)) fetchFallback(u, d)`
 *     is the standard shape in the wild, and the fallback usually names a SECOND endpoint. Collapsing the
 *     return to a bare `true` deletes that arm and the endpoint behind it.
 *
 * WHAT §3 STEP 6.2 ALSO MEANS IS THAT AN OVER-QUOTA BEACON RECORDS NOTHING. The step terminates the algorithm
 * before step 7 builds a request at all, so there is no request to record — recording one would put a request
 * on the @H surface that no user agent ever creates. The `false` it returns is what carries the information
 * forward: the page's own fallback runs, and the surface learns whatever THAT sends.
 *
 * corsMode (steps 5 and 6.3.1/6.3.2) IS DELIBERATELY NOT COMPUTED. Its only consumer is the `mode` of the
 * request step 7 fetches, and step 7 fetches nothing here; the @H record carries a method, a URL, headers and
 * a body and has no place to put it. A value nothing reads is dead code, and dead code that looks like a
 * modelled fact is worse than an absence. The Content-Type step 6.3.3 appends is a different matter — it is a
 * HEADER, the record holds headers, and a reviewer replaying the call needs it. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/fetch/body.h"
#include "core/fetch/fetch.h"
#include "core/frame/navigator.h"
#include "core/frame/navigator_beacon.h"
#include "core/idl_args.h"
#include "core/agent_state.h"
#include "core/url/url.h"
#include "solver/concolic.h"
#include "solver/endpoint.h"
#include "solver/engine.h"     /* the ONE composition of what a request this running code builds is evidence of */

/* Fetch §4.6 "HTTP-network-or-cache fetch": "If the sum of contentLength and inflightKeepaliveBytes is greater
   than 64 kibibytes, then return a network error." Beacon §3 step 6.2 is that same limit reported as a `false`
   rather than as a network error, which is why the number lives here once and is compared with `>`. */
#define BEACON_KEEPALIVE_QUOTA_BYTES (64 * 1024)

/* §5.2 "BodyInit unions"' scalar-value-string arm sets the type to `text/plain;charset=UTF-8`, and that is the
   type an UNKNOWN body carries too: a concolic is none of the union's interface arms, so it IS the string arm.
   solver/endpoint.c reads that type as an undeclared body and runs the real JSON parser over the bytes, which
   is what makes `sendBeacon(u, JSON.stringify(x))` contribute its FIELDS and not just its address. */
#define BEACON_STRING_MIME "text/plain;charset=UTF-8"

static int g_id_send_beacon = -1;

/* WEB IDL §3.7.7 "Operations"' BRAND CHECK — "If jsValue does not implement the interface target, throw a
   TypeError" — asked of the component that owns the class. A page tells a TypeError apart
   from `undefined` — a feature detector reads the throw as "this is a real interface" — so it is a real throw
   and never an assert. */
static bool beacon_brand(JSContext *ctx, JSValueConst this_val)
{
    if (navigator_is(this_val)) return true;
    JS_ThrowTypeError(ctx, "sendBeacon was reached on something that is not a Navigator");
    return false;
}

/* THE HALF OF "THIS's relevant settings object" THIS ENGINE CAN ANSWER — the same assert core/frame/navigator.c
   makes of its own members, and it is here for the same reason: `js_call_c_function` takes `ctx` from the
   FUNCTION object, so a member reached through ONE realm's Navigator.prototype on ANOTHER realm's Navigator
   would resolve steps 1 and 2 (the API base URL and the origin) out of the wrong document and record the
   endpoint against the wrong address. */
static void beacon_assert_this_realm(JSContext *ctx, JSValueConst this_val)
{
    JSValue own = navigator_object(ctx);
    bool same = JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(this_val);

    JS_FreeValue(ctx, own);
    DCHECK(same, "§3's sendBeacon was reached through ONE realm's Navigator.prototype on ANOTHER realm's "
                 "Navigator — steps 1 and 2 read the API base URL and the origin of the member's own realm, "
                 "so the request would be composed against a document that did not make the call. BUILD the "
                 "Navigator that carries its own realm (core/frame/navigator.c names the same gap)");
}

/* §2.1.4's RETURN VALUE for a request the algorithm did reach step 7 with. See the file comment for why this
   is a concolic rather than a constant, and for the one case that is concrete instead.
   `url_text` is the address this beacon names — its serialized URL, or an unknown URL's display shape. It is
   part of the value's IDENTITY because two beacons to two collectors ask two independent questions, and an
   identity shared between them would let a flow that pinned one DECIDE the other and prune an arm nothing
   contradicts (solver/concolic.h). It is composed out of the ADDRESS rather than out of a call counter for the
   reason §the-scheduler gives for a drive's name: an ordinal is session-local, so a resumed flow would ask a
   different question at the first branch and discard every arm it had recorded. */
static JSValue beacon_queue_result(JSContext *ctx, const char *url_text)
{
    char *name, *hole;
    JSValue v;
    int n;

    DCHECK(url_text != NULL, "the beacon queue result was named against no address — every path that reaches "
                             "step 7 has either a serialized URL or an unknown URL's display shape");
    n = snprintf(NULL, 0, "navigator.sendBeacon(%s)", url_text);
    CHECK(n > 0, "beacon: measuring the queue result's source name failed");
    name = malloc((size_t)n + 1);
    CHECK(name, "beacon: OOM naming the queue result's source");
    snprintf(name, (size_t)n + 1, "navigator.sendBeacon(%s)", url_text);
    /* THE SHAPE IS THAT NAME IN BRACES — concolic_new's rule, and it is spelled here rather than folded into
       the format above because the two halves are different facts. `url_text` may itself be an unknown URL's
       display shape, so the shape can nest (`{navigator.sendBeacon({location.hash})}`); concolic_hole_key
       strips every brace, so the nesting composes to one stable name rather than to two. */
    n = snprintf(NULL, 0, "{%s}", name);
    CHECK(n > 0, "beacon: measuring the queue result's display shape failed");
    hole = malloc((size_t)n + 1);
    CHECK(hole, "beacon: OOM naming the queue result's hole");
    snprintf(hole, (size_t)n + 1, "{%s}", name);
    /* THE SEAM, not concolic_new: a host that is not exploring (the conformance runner installs the value
       semantics and NOT the source overlay) gets the plain boolean back, which is what a spec test reads. */
    v = concolic_source_wrap(ctx, hole, name, JS_TRUE);
    free(name);
    free(hole);
    return v;
}

/* BEACON §3 "Processing Model". */
static JSValue js_nav_send_beacon(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                  int magic)
{
    JSValueConst url_arg, data;
    /* §3 step 4: "Let headerList be an empty list." At most one entry ever reaches it — step 6.3.3's
       Content-Type — because §1 states the method takes no custom request headers. */
    EndpointHeader hdrs[1];
    int nhdrs = 0;
    EndpointBody eb = { NULL, NULL, 0 };
    const EndpointBody *ebp = NULL;
    BodyState b = { 0 };
    char *mime = NULL;
    /* The address as the @H surface must see it: the JSValue endpoint_record reads (a concolic passes through
       as itself so its shape is preserved), and the TEXT the queue result is named against. */
    JSValueConst url_value;
    JSValue url_str = JS_UNDEFINED;
    const char *url_c = NULL;      /* JS_ToCString'd, freed with JS_FreeCString */
    char *url_owned = NULL;        /* url_serialize'd, freed with free */
    const char *url_text;
    JSValue ret = JS_UNDEFINED;

    (void)magic;
    if (!beacon_brand(ctx, this_val)) return JS_EXCEPTION;
    beacon_assert_this_realm(ctx, this_val);

    DCHECK(argc >= 2, "§2.1's sendBeacon was called with fewer positions than its declaration lists — the args "
                      "machine converts a declared optional position with a default (`data = null`) even when "
                      "the page stopped short of it, so both are always present");
    url_arg = argv[0];
    data    = argv[1];

    /* ---- §3 step 3: parse the URL, and refuse a scheme the beacon cannot use ------------------------------ */
    if (concolic_is(url_arg)) {
        /* AN UNKNOWN ADDRESS IS NOT REFUSED. §3 step 3's throw is decided by the parse and by the scheme, and
           neither is known for a URL built out of unknown external input — the solver's rule for an undecided
           predicate is that uncertainty KEEPS the arm, which is the same answer core/fetch/fetch.c gives §2.9's
           port block and §4.1.2's CSP check over a shape. Refusing on the accident that a display shape's text
           carried no `http` would delete a real endpoint from the surface. */
        const char *shape = concolic_shape_c(url_arg);

        DCHECK(shape != NULL, "a concolic URL reached §3 step 3 with no display shape — the shape is what this "
                              "member records in place of an address it cannot know, so a value that has lost "
                              "it would be recorded as the two characters an unnameable hole prints as");
        url_value = url_arg;
        url_text = shape;
    } else {
        UrlRecord rec;
        bool ok;

        DCHECK(JS_IsString(url_arg), "§2.1 declares `USVString url`, so the declaration has already converted "
                                     "every value that is not unknown external input — a non-string here means "
                                     "the position was declared with the wrong IDL type");
        url_c = JS_ToCString(ctx, url_arg);
        if (!url_c) return JS_EXCEPTION;
        url_record_init(&rec);
        /* THE BASE IS STEP 1's, and fetch_parse_url is the ONE operation in this engine that resolves a URL a
           page wrote against HTML's API base URL — restating it here would be a second answer to one question. */
        ok = fetch_parse_url(ctx, &rec, url_c, strlen(url_c));
        if (ok && (!rec.scheme || (strcmp(rec.scheme, "http") && strcmp(rec.scheme, "https")))) {
            url_record_free(&rec);
            JS_FreeCString(ctx, url_c);
            /* §3 step 3's second half, and it is a THROW rather than a `false`: the two are different answers
               and a page that feature-detects with `try { navigator.sendBeacon("x:") }` reads them apart. */
            return JS_ThrowTypeError(ctx, "sendBeacon requires an http or https URL");
        }
        if (!ok) {
            JS_FreeCString(ctx, url_c);
            return JS_ThrowTypeError(ctx, "sendBeacon could not parse its URL");
        }
        /* The address the surface records is the PARSED one, which is what step 3 produced and what steps 7's
           request carries — a relative reference is an endpoint nothing can replay. */
        url_owned = url_serialize(&rec, /*exclude_fragment*/ false);
        url_record_free(&rec);
        JS_FreeCString(ctx, url_c);
        url_c = NULL;
        CHECK(url_owned, "beacon: OOM serializing §3 step 3's parsed URL");
        url_str = JS_NewString(ctx, url_owned);
        if (JS_IsException(url_str)) { free(url_owned); return JS_EXCEPTION; }
        url_value = url_str;
        url_text = url_owned;
    }

    /* ---- §3 step 6: extract the body ---------------------------------------------------------------------- */
    if (!JS_IsNull(data) && !JS_IsUndefined(data)) {
        if (concolic_is(data)) {
            /* UNKNOWN EXTERNAL INPUT TAKES §5.2's STRING ARM, because it is none of the union's interface
               arms — and its bytes are unknown, so what the surface records is the SHAPE, the same answer
               solver/endpoint.c already gives a concolic URL and core/html/form_data.c gives an unknown entry.
               Its LENGTH is unknown too, so step 6.2's comparison is undecided and the return stays the
               concolic below rather than collapsing to either answer. */
            const char *shape = concolic_shape_c(data);

            DCHECK(shape != NULL, "a concolic body reached §3 step 6.1 with no display shape — the shape is "
                                  "the whole of what an unknown payload contributes to the @H surface");
            eb.mime = BEACON_STRING_MIME;
            eb.bytes = shape;
            eb.len = strlen(shape);
            ebp = &eb;
            hdrs[nhdrs].name = "Content-Type";           /* step 6.3.3 */
            hdrs[nhdrs].value = BEACON_STRING_MIME;
            nhdrs++;
        } else {
            /* §3 step 6.1: "extract data's byte stream WITH THE KEEPALIVE FLAG SET". The flag is §5.2's own
               argument and it is not decorative: the ReadableStream arm throws a TypeError when it is set, so
               `navigator.sendBeacon(u, someStream)` must throw where the page wrote it. */
            if (body_extract(ctx, &b, data, /*keepalive*/ true, &mime) < 0) {
                free(mime);
                body_state_free(JS_GetRuntime(ctx), &b);
                JS_FreeValue(ctx, url_str);
                free(url_owned);
                return JS_EXCEPTION;
            }
            DCHECK(b.has, "§5.2's extraction answered NO BODY for a `data` that is neither null nor undefined "
                          "— every arm of the union produces one, so an absent body here is an arm that was "
                          "taken and produced nothing");
            /* §3 step 6.2, which Fetch §4.6 states as the sum against 64 KiB. A body over the quota on its own
               exceeds it in EVERY world (the sum is never smaller than contentLength), so this arm is concrete
               and the algorithm terminates here: step 7 never builds a request, so there is nothing to record. */
            if (b.len > BEACON_KEEPALIVE_QUOTA_BYTES) {
                free(mime);
                body_state_free(JS_GetRuntime(ctx), &b);
                JS_FreeValue(ctx, url_str);
                free(url_owned);
                return JS_FALSE;
            }
            eb.mime = mime;                    /* NULL for §5.2's arms that carry no type — a BufferSource */
            eb.bytes = b.bytes;
            eb.len = b.len;
            ebp = &eb;
            if (mime) {                        /* step 6.3.3, run only "if contentType is not null" */
                hdrs[nhdrs].name = "Content-Type";
                hdrs[nhdrs].value = mime;
                nhdrs++;
            }
        }
    }

    DCHECK(nhdrs <= (int)(sizeof hdrs / sizeof hdrs[0]),
           "§3 built a beacon header list longer than the one entry step 6.3.3 can append — §1 states the "
           "member provides no way to add custom request headers, so a second entry means a step was invented");
    DCHECK(ebp == NULL || ebp->bytes != NULL,
           "§3 step 7 was reached with a body record naming no bytes — a body is bytes WITH a type, and a "
           "record holding one of the two is what solver/endpoint.h refuses to represent");

    /* ---- §3 step 7's REQUEST, derived and recorded. See the file comment for why it is not fetched. ------- */
    endpoint_record(ctx, "POST", url_value, nhdrs ? hdrs : NULL, nhdrs, ebp,
                    engine_prov_of_running_path());

    ret = beacon_queue_result(ctx, url_text);

    free(mime);
    body_state_free(JS_GetRuntime(ctx), &b);
    JS_FreeValue(ctx, url_str);
    free(url_owned);
    return ret;
}

void navigator_beacon_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_send_beacon >= 0, "§2.1's member was installed on a realm before navigator_beacon_init "
                                  "declared it — the declaration is per AGENT and the install per REALM");
    /* `boolean sendBeacon(USVString url, optional BodyInit? data = null)`: ONE required argument, so the
       function's `length` is 1. The IDL has no [SecureContext] and Chrome exposes it over http, so it is
       installed unconditionally rather than through the exposure-stating form. */
    idl_install_method(ctx, proto, "sendBeacon", g_id_send_beacon);
}

void navigator_beacon_init(JSContext *ctx)
{
    /* §2.1's IDL, as the declaration: the USVString's §3.2.12 scalar-value conversion and the BodyInit union's
       brand test are the ARGUMENT MACHINE's, so the body below receives values that are already the types the
       spec names — or unknown external input, which crosses every IDL boundary as itself. */
    static const IdlArgType BEACON_ARGS[] = { IDL_USVSTRING, IDL_BODYINIT_NULLABLE };

    DCHECK(g_id_send_beacon < 0, "navigator_beacon_init ran twice — §2.1's member is declared once per AGENT");
    g_id_send_beacon = idl_method_id(ctx, BEACON_ARGS, 2, js_nav_send_beacon, 0);
    /* `optional … data = null` — both halves of it. The position is optional from index 1, and its DEFAULT is
       the IDL's `null` rather than the absence: declared, the machine PLACES a real null and the body reads
       the IDL's value instead of inventing one from a hole. */
    idl_optional_from(1);
    idl_arg_default(1, IDL_DEFAULT_NULL, NULL);
    /* DECLARED UNDER `navigator`, BECAUSE THE NAME IS THE ROW'S AND NOT THE FILE'S — core/agent_state.h.
       core/platform.c's list has no `navigator_beacon` row and must not grow one: §2.1's member is a partial
       interface of §8.10.1's Navigator, so navigator_init is what declares this component and navigator_free
       is what releases it, exactly as it is for Permissions §6 beside it. `navigator` is therefore the row
       whose release column this declaration is the inverse of. Declared under this file's own name it was not
       a smaller check but an ABSENT one — the row pairing can only ask "does anybody release this?" about a
       name a row carries — and it left `navigator` able to report "declared no agent state" in exactly the
       words a component that had declared nothing would use. */
    agent_state_id("navigator", &g_id_send_beacon, "Beacon §2.1's sendBeacon declaration");
}

void navigator_beacon_free(void)
{
    g_id_send_beacon = -1;
}
