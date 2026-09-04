/* THE REQUEST INTERFACE — WHATWG Fetch §5.4 "Request class".
 *
 * WHAT IT IS FOR HERE. `fetch(input, init)` takes a `RequestInfo`, which is a Request or a string, so the
 * interface is half of the fetch method's own argument. It is also where two of §5.1's guards become
 * observable at all: a page's own `new Headers()` refuses nothing, and only a Request's header list carries
 * the "request" guard that drops `Host`, `Cookie` and a method-override header smuggling CONNECT/TRACE/TRACK,
 * or the "request-no-cors" guard that keeps only the four no-CORS-safelisted names.
 *
 * THE CONSTRUCTOR IS A MACHINE because every one of its inputs is the page's: `input` may be a string whose
 * `toString` the page wrote, `init` is a dictionary of [[Get]]s, and `init.headers` is the whole fill.
 *
 * §5.4's `signal` IS DOM §3.2 Interface AbortSignal's DEPENDENT ABORT SIGNAL AND NOT A STORED REFERENCE, which is the
 * whole reason it is this interface's own state rather than a member read back off `init`. The constructor's steps
 * are "let signals be « signal » if signal is non-null; otherwise « »" and "set this's signal to the result of
 * creating a dependent abort signal from signals" — so EVERY Request has one, a Request built with no `init.signal`
 * has a fresh never-aborting signal rather than null, and one built with a signal has a NEW signal that aborts when
 * that one does. §5.4 states the invariant in the getter's own prose ("this's signal is always initialized in the
 * constructor and when cloning"), which is why the getter asserts it instead of admitting an absence. The dependency
 * is core/dom/abort.h's `abort_signal_dependent_new`, which is DOM §3.2 Interface AbortSignal's one algorithm — an
 * imitation that added an abort ALGORITHM to the source would abort one turn late and the page can see the
 * difference.
 *
 * WHAT IS HONESTLY ABSENT: `body` as a ReadableStream, and `formData()`/`blob()`. Each is absent rather than
 * answered wrongly. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "solver/concolic.h"
#include "core/dom/abort.h"
#include "core/fetch/fetch.h"
#include "core/file/blob.h"
#include "core/fetch/request.h"
#include "core/fetch/headers.h"
#include "core/fetch/body.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/streams/readable_stream.h"
#include "core/url/url.h"

/* §5.4's request, as the fields the interface reports. The enumerated members are stored as their spec strings
   because that is what the attributes return and what `init` supplies — there is no computation on them yet,
   and inventing an enum would be a second spelling to keep in step with the first. */
typedef struct {
    char     *url;             /* the serialized parsed URL */
    /* §2.2.5's request, as the record §5.4 steps 10-27 fill — request.h, and the ONE place those steps run. */
    RequestRecord rec;
    BodyState body;
    JSValue   headers;         /* [SameObject] */
    /* §5.4: a Request built from a `blob:` URL CAPTURES its blob URL entry, so revoking the URL afterwards
       does not stop that request — the entry is the Request's, not the store's. Without it, the ordinary
       `new Request(u); URL.revokeObjectURL(u); fetch(req)` sequence a page uses to clean up eagerly failed. */
    JSValue blob_entry;
    /* §5.4's signal — §3.2's dependent abort signal, built at construction and at clone. It is OWNED, so it is
       freed by the finalizer, MARKED by the gc_mark (the collector cannot see through a class opaque) and
       DUP'd by every clone: a field added to this struct is an obligation at all three sites, and the three
       are read together for exactly that reason. */
    JSValue signal;
} RequestData;

static JSClassID g_request_class;
static JSRuntime *g_request_rt;
static int       g_request_ctor_stepid = -1;
static int       g_request_body_handle = -1;

static void request_finalizer(JSRuntime *rt, JSValue val)
{
    RequestData *d = JS_GetOpaque(val, g_request_class);
    if (!d) return;
    JS_FreeValueRT(rt, d->headers);
    JS_FreeValueRT(rt, d->blob_entry);
    JS_FreeValueRT(rt, d->signal);
    js_free_rt(rt, d->url);
    request_record_free(rt, &d->rec);
    body_state_free(rt, &d->body);
    js_free_rt(rt, d);
}

/* The Headers a Request holds is a JSValue in the class opaque, which the collector cannot see through. */
static void request_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    RequestData *d = JS_GetOpaque(val, g_request_class);
    if (d) {
        JS_MarkValue(rt, d->headers, mark_func);
        JS_MarkValue(rt, d->blob_entry, mark_func);
        JS_MarkValue(rt, d->signal, mark_func);
    }
    if (d) body_state_mark(rt, &d->body, mark_func);
}

static RequestData *request_of(JSValueConst v) { return JS_GetOpaque(v, g_request_class); }

/* ---- Fetch §2.2.5 "Requests"' record — see request.h --------------------------------------------------- */

void request_record_init(RequestRecord *rec)
{
    DCHECK(rec != NULL, "a §2.2.5 request record was initialized through a null pointer");
    memset(rec, 0, sizeof *rec);
}

void request_record_free(JSRuntime *rt, RequestRecord *rec)
{
    if (!rec) return;
    js_free_rt(rt, rec->method); js_free_rt(rt, rec->mode); js_free_rt(rt, rec->credentials);
    js_free_rt(rt, rec->cache); js_free_rt(rt, rec->redirect); js_free_rt(rt, rec->referrer);
    js_free_rt(rt, rec->referrer_policy); js_free_rt(rt, rec->integrity); js_free_rt(rt, rec->destination);
    request_record_init(rec);
}

/* EVERY FIELD IS PLACED BEFORE THE NEXT ONE IS ATTEMPTED, so a failure leaves a record whose remaining fields
   are still NULL and whose free is exact — the same rule the constructor's own state follows and for the same
   reason: the failure path frees what the record holds and nothing else. */
int request_record_copy(JSContext *ctx, RequestRecord *dst, const RequestRecord *src)
{
    DCHECK(dst != NULL && src != NULL, "a §2.2.5 request record was copied through a null pointer");
    request_record_init(dst);
    dst->keepalive = src->keepalive;
#define REQ_REC_DUP(f) do { \
        if (src->f) { dst->f = js_strdup(ctx, src->f); if (!dst->f) return -1; } \
    } while (0)
    REQ_REC_DUP(method); REQ_REC_DUP(mode); REQ_REC_DUP(credentials); REQ_REC_DUP(cache);
    REQ_REC_DUP(redirect); REQ_REC_DUP(referrer); REQ_REC_DUP(referrer_policy); REQ_REC_DUP(integrity);
    REQ_REC_DUP(destination);
#undef REQ_REC_DUP
    return 0;
}

const RequestRecord *request_record_of(JSValueConst v)
{
    RequestData *d = g_request_class ? JS_GetOpaque(v, g_request_class) : NULL;
    return d ? &d->rec : NULL;
}

/* §5.4's `formData()` asks the including interface for its Content-Type, because only it knows where its
   headers live. NULL when there is none, which is a body with no form encoding and therefore a TypeError. */
static char *request_body_mime(JSContext *ctx, JSValueConst v)
{
    RequestData *d = JS_GetOpaque(v, g_request_class);
    (void)ctx;
    return d ? header_list_get(headers_list_of(d->headers), "content-type") : NULL;
}

static BodyState *request_body_of(JSValueConst v)
{
    RequestData *d = JS_GetOpaque(v, g_request_class);
    return d ? &d->body : NULL;
}

/* THE BRAND — see request.h. The same `JS_GetOpaque` against this file's class id that every accessor here
   already uses to decide whether it is holding one of ours; it is public because a UNION resolves on exactly
   this question and the answer may not be duck-typed at the call site. */
bool request_is(JSValueConst v)
{
    return g_request_class && JS_GetOpaque(v, g_request_class) != NULL;
}

/* §5.4's captured blob URL entry, or JS_UNDEFINED — what a fetch of this Request answers from. Borrowed. */
JSValueConst request_blob_entry(JSValueConst v)
{
    RequestData *d = g_request_class ? JS_GetOpaque(v, g_request_class) : NULL;
    return d ? d->blob_entry : JS_UNDEFINED;
}

/* §2.2.1's "normalize a method": UPPERCASE for the six HTTP names, and byte-for-byte for anything else — `patch`
   stays lowercase while `post` becomes `POST`, which is the difference a server sees. */
static char *method_normalize(JSContext *ctx, const char *m)
{
    static const char *const UP[] = { "DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT" };
    size_t i;
    for (i = 0; i < sizeof(UP) / sizeof(UP[0]); i++)
        if (!strcasecmp(m, UP[i])) return js_strdup(ctx, UP[i]);
    return js_strdup(ctx, m);
}

/* §2.2.1's "forbidden method": the three a page may never send, however it spells them. */
static int method_is_forbidden(const char *m)
{
    return !strcasecmp(m, "CONNECT") || !strcasecmp(m, "TRACE") || !strcasecmp(m, "TRACK");
}

/* §2.2.1's "method": an RFC 7230 token, which is what makes `new Request(u, {method: "G ET"})` a TypeError. */
static int method_is_token(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!*p) return 0;
    for (; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (c && strchr("!#$%&'*+-.^_`|~", (char)c)) continue;
        return 0;
    }
    return 1;
}

bool request_method_is_token(const char *m) { return method_is_token(m) != 0; }

/* §5.4 STEP 25 AS ONE OPERATION, because two call sites perform it: this constructor, and `fetch(input, init)`,
   which runs §5.4 inline over an init it never turns into a Request. fetch() had none of it — no token test, no
   forbidden-method refusal and no normalization — so `{method:"connect"}` went out and `{method:"post"}` was
   reported on the endpoint surface as a method distinct from POST. Returns the normalized method (the caller
   frees it with js_free) or NULL with a TypeError live. */
char *request_method_check(JSContext *ctx, const char *m)
{
    if (!method_is_token(m) || method_is_forbidden(m)) {
        JS_ThrowTypeError(ctx, "the Request method is not a usable method");
        return NULL;
    }
    return method_normalize(ctx, m);
}

enum { REQ_METHOD = 0, REQ_URL, REQ_HEADERS, REQ_DESTINATION, REQ_REFERRER, REQ_REFERRER_POLICY,
       REQ_MODE, REQ_CREDENTIALS, REQ_CACHE, REQ_REDIRECT, REQ_INTEGRITY, REQ_KEEPALIVE,
       REQ_IS_RELOAD_NAV, REQ_IS_HISTORY_NAV, REQ_DUPLEX, REQ_SIGNAL };

static JSValue js_request_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    RequestData *d = request_of(this_val);
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Request");
    switch (magic) {
    case REQ_METHOD:          return JS_NewString(ctx, d->rec.method);
    case REQ_URL:             return JS_NewString(ctx, d->url);
    case REQ_HEADERS:         return JS_DupValue(ctx, d->headers);   /* [SameObject] */
    case REQ_DESTINATION:     return JS_NewString(ctx, d->rec.destination);
    case REQ_REFERRER:        return JS_NewString(ctx, d->rec.referrer);
    case REQ_REFERRER_POLICY: return JS_NewString(ctx, d->rec.referrer_policy);
    case REQ_MODE:            return JS_NewString(ctx, d->rec.mode);
    case REQ_CREDENTIALS:     return JS_NewString(ctx, d->rec.credentials);
    case REQ_CACHE:           return JS_NewString(ctx, d->rec.cache);
    case REQ_REDIRECT:        return JS_NewString(ctx, d->rec.redirect);
    case REQ_INTEGRITY:       return JS_NewString(ctx, d->rec.integrity);
    case REQ_KEEPALIVE:       return JS_NewBool(ctx, d->rec.keepalive != 0);
    case REQ_SIGNAL:
        /* §5.4: "this's signal is always initialized in the constructor and when cloning" — the member is a
           non-nullable `AbortSignal`, so an absence here is an engine defect and never a value to report. */
        DCHECK(abort_signal_is(ctx, d->signal),
               "a Request answered `signal` with something that is not an AbortSignal — §5.4 builds one in the "
               "constructor and one in clone(), so a Request that reached a page without one was minted by a "
               "third path that owes itself the dependent signal both of those create");
        return JS_DupValue(ctx, d->signal);
    /* §5.4: both are false for a request a script constructed — only a navigation sets them, and there is no
       navigation here to set them from. This is a computed answer, not a placeholder. */
    case REQ_IS_RELOAD_NAV:
    case REQ_IS_HISTORY_NAV:  return JS_FALSE;
    default:
        DCHECK(magic == REQ_DUPLEX, "a Request accessor was declared with a magic this component does not answer");
        return JS_NewString(ctx, "half");
    }
}

/* §5.4 clone(). Like Response's, the "unusable" check is SYNCHRONOUS — a page that guards with try/catch sees
   the throw where it wrote the catch — and the clone gets its OWN body-used latch, because the point of
   cloning is two independent reads. The headers are a NEW Headers over the same list with the same guard:
   [SameObject] is per request, so `r.clone().headers === r.headers` is false. */
static JSValue js_request_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    RequestData *d = request_of(this_val), *c;
    JSValue obj;

    (void)argc; (void)argv;
    if (!d)
        return JS_ThrowTypeError(ctx, "not a Request");
    if (d->body.used)
        return JS_ThrowTypeError(ctx, "cannot clone a Request whose body has been read");
    {
        JSValue rproto = JS_GetClassProto(ctx, g_request_class);
        DCHECK(!JS_IsNull(rproto), "a Request was minted in a realm that never ran its install");
        obj = JS_NewObjectProtoClass(ctx, rproto, g_request_class);
        JS_FreeValue(ctx, rproto);
    }
    if (JS_IsException(obj))
        return obj;
    c = js_mallocz(ctx, sizeof(*c));
    if (!c) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    c->headers = JS_UNDEFINED;
    c->signal  = JS_UNDEFINED;   /* placed before the first step that can fail — see the constructor's */
    JS_SetOpaque(obj, c);
    /* §5.4 clone copies the request, and its blob URL ENTRY is part of what it is — a clone of a request built
       from a since-revoked URL fetches exactly as the original does. */
    c->blob_entry      = JS_DupValue(ctx, d->blob_entry);
    c->url             = js_strdup(ctx, d->url);
    /* §5.4 clone(): every field of the request comes with it, through the one copy of the record — a field
       added to §2.2.5's record is then carried here without this site being touched, which is the whole
       reason the record is a record. */
    if (!c->url || request_record_copy(ctx, &c->rec, &d->rec) < 0 ||
        body_state_set(ctx, &c->body, d->body.has ? d->body.bytes : NULL, d->body.len) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    c->headers = headers_new(ctx, headers_list_of(d->headers), headers_guard_of(d->headers));
    if (JS_IsException(c->headers)) { c->headers = JS_UNDEFINED; JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    /* §5.4 clone(): "assert: this's signal is non-null", then "let clonedSignal be the result of creating a
       dependent abort signal from « this's signal »". The clone does NOT share the signal object — aborting
       the original's signal aborts the clone's through the dependency, and the two are still `!==`. */
    DCHECK(abort_signal_is(ctx, d->signal),
           "a Request being cloned carried no AbortSignal — §5.4's clone steps assert this's signal is "
           "non-null, and every path that mints a Request builds one");
    {
        JSValueConst source[1];
        source[0] = d->signal;
        c->signal = abort_signal_dependent_new(ctx, source, 1);
    }
    if (JS_IsException(c->signal)) { c->signal = JS_UNDEFINED; JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    return obj;
}

/* ---- the constructor ------------------------------------------------------------------------------------- */

/* WHERE THIS MACHINE RESTS, AS §5.4 NUMBERS IT. The constructor's forty-two steps run the page's code in
   exactly two places — the header fill at step 33 and nothing else before it, and the body extraction at
   step 37 — so the stages are the three spans those two points cut it into, and each label says its span. The
   `stage` byte this state carried said none of that: a flow parked here could be described only as "stage 1 of
   something", which is neither a resume point a later build can resolve nor a thing a park can report. */
#define REQ_CTOR_STAGES(X) \
    X(REQ_CTOR_RECORD = IDL_STEP_FIRST, \
      "Fetch §5.4 new Request(input, init) steps 5-30 (the request record: step 12's carry-forward of a " \
      "Request input's URL, method, header list, mode, credentials mode, cache mode, redirect mode, " \
      "integrity and keepalive, then every init member that overrides one)") \
    X(REQ_CTOR_HEADERS, \
      "Fetch §5.4 new Request(input, init) steps 31-33 (step 32's CORS-safelisted-method test under a " \
      "\"no-cors\" mode, then this's headers under the guard that step chose)") \
    X(REQ_CTOR_BODY, \
      "Fetch §5.4 new Request(input, init) steps 34-41 (a body on GET or HEAD is a TypeError whether it is " \
      "the init's or the input Request's; extract init[\"body\"], or proxy the input Request's)")
enum { REQ_CTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const REQ_CTOR_STEPS[] = { REQ_CTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    HeadersFill fill;
    HeaderList  list;
    JSValue     result;
} JSRequestCtorState;

static void js_request_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRequestCtorState *s = st;
    headers_fill_visit(ctx, &s->fill, v);
    v->val(ctx, &s->result);
}

/* The header list alone — see js_headers_ctor_release. The rest is js_request_ctor_visit's declaration. */
static void js_request_ctor_release(JSContext *ctx, void *st)
{
    (void)ctx;
    header_list_free(&((JSRequestCtorState *)st)->list);
}

/* `dictionary RequestInit`, IN ONE PLACE, because it has TWO readers and they must not drift: the declaration
   below hands it to the argument machine, and req_init_is_empty walks the same names to answer §5.4 step 13's
   "If init is not empty". A second hand-written list of member names is how that question starts answering for
   a dictionary the declaration no longer has. Web IDL reads members in LEXICOGRAPHIC order, which is not the
   order §5.4 uses them in.
   AND THE LIST IS NOT FETCH'S ALONE. Two other standards extend this dictionary with a `partial dictionary
   RequestInit`, and Web IDL §2.7 Dictionaries says what that means for the read order in one sentence: "the
   dictionary members on the one dictionary definition (INCLUDING ANY PARTIAL DICTIONARY DEFINITIONS) are
   ordered lexicographically by the Unicode codepoints that comprise their identifiers". So a partial's member
   is not appended after Fetch's — it sorts among them, which is why `duplex` sits between `credentials` and
   `headers` and `targetAddressSpace` between `signal` and `window`. Each foreign member says which standard
   it came from at its own row, because that is where a reader asks.
   `AbortSignal? signal` CARRIES NO DEFAULT, which is Fetch's own IDL and is load-bearing here rather than
   pedantic: a defaulted member is PRESENT on every converted dictionary, so `signal` with an IDL default made
   `new Request(r, {})` a NON-EMPTY init and step 13 would have reset the referrer of every construction there
   has ever been. The body reads it with abort_signal_is, which answers false for an absent member exactly as
   it does for `null`, so nothing needed the default. */
/* §5.4's `enum RequestDuplex { "half" };` — a ONE-VALUE enumeration, which is not a degenerate case but the
   whole point of the member: §3.2.18 makes every other string a TypeError, and step 39's test is that the
   member EXISTS, so "half" is the only spelling by which a page can say "yes, I know this body is a stream". */
IDL_ENUM_VALUES(REQUEST_DUPLEX, "half");
/* §5.4's `enum RequestPriority { "high", "low", "auto" };` — the hint step 27 puts on the request. */
IDL_ENUM_VALUES(REQUEST_PRIORITY, "auto", "high", "low");
/* LOCAL NETWORK ACCESS §2.1 IP Address Space's `enum IPAddressSpace { "public", "local", "loopback" };`, which
   LOCAL NETWORK ACCESS §3.1.2 Fetch API attaches to RequestInit by a partial dictionary. The standard's name
   is repeated rather than written "that standard's" because the second number would then name NO standard,
   and Fetch — this file's own — HAS a §3.1.2 ("`Set-Cookie` header"), so the shorter spelling is a real
   number in the wrong document. */
IDL_ENUM_VALUES(REQUEST_TARGET_ADDRESS_SPACE, "public", "local", "loopback");
/* §5.4's OTHER FIVE ENUMERATIONS, WHICH THIS TABLE DECLARED AS `DOMString` AND WHICH ARE NOT ONE. Fetch's IDL
   writes `RequestMode mode; RequestCredentials credentials; RequestCache cache; RequestRedirect redirect;
   ReferrerPolicy referrerPolicy;`, and §3.2.18 Enumeration types makes every value outside the list a
   TypeError — so `new Request(u, {credentials: "always"})` threw in every browser and built a request here
   whose `credentials` attribute then ANSWERED "always". That is the defect a declaration exists to make
   impossible arriving THROUGH one: the member was declared, converted and stored, and the only thing wrong
   with it was the type, which no member-list audit can see (idlgen diffs which members EXIST). Four of the
   five are Fetch's own, quoted from its IDL block; ReferrerPolicy is REFERRER POLICY §3 Referrer Policies'
   and its FIRST value is the EMPTY STRING, which is a value the enumeration defines and not an absence. */
IDL_ENUM_VALUES(REQUEST_MODE, "navigate", "same-origin", "no-cors", "cors");
IDL_ENUM_VALUES(REQUEST_CREDENTIALS, "omit", "same-origin", "include");
IDL_ENUM_VALUES(REQUEST_CACHE, "default", "no-store", "reload", "no-cache", "force-cache", "only-if-cached");
IDL_ENUM_VALUES(REQUEST_REDIRECT, "follow", "error", "manual");
IDL_ENUM_VALUES(REFERRER_POLICY, "", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin",
                "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url");

static const IdlDictMember REQUEST_INIT[] = {
    { "body",           IDL_BODYINIT_NULLABLE,  false },
    { "cache",          IDL_ENUM,               false, REQUEST_CACHE },
    { "credentials",    IDL_ENUM,               false, REQUEST_CREDENTIALS },
    /* `RequestDuplex duplex` — read at step 39, and read for its PRESENCE rather than for its value. */
    { "duplex",         IDL_ENUM,               false, REQUEST_DUPLEX },
    { "headers",        IDL_ANY,                false },   /* HeadersInit: the union the fill converts */
    { "integrity",      IDL_DOMSTRING,          false },
    /* `boolean keepalive` declares NO default in Fetch's IDL, so IDL_BOOLEAN_NO_DEFAULT and not IDL_BOOLEAN:
       step 24 sets the request's keepalive only "if init["keepalive"] exists", and a member converted with
       ToBoolean(undefined) answers `false` for an absent one — which would overwrite the keepalive a Request
       input contributed at step 12 with a value the page never wrote. */
    { "keepalive",      IDL_BOOLEAN_NO_DEFAULT, false },
    { "method",         IDL_BYTESTRING,         false },
    { "mode",           IDL_ENUM,               false, REQUEST_MODE },
    /* `RequestPriority priority` — §5.4 step 27. THE CONVERSION IS THE WHOLE OF WHAT IS OBSERVABLE HERE, and
       that is a fact about the platform rather than a gap: §3.2.18 makes `{priority: "urgent"}` a TypeError,
       and step 27's own effect is to set the REQUEST's priority, which §5.4 exposes through no attribute —
       `Request` has fourteen getters and `priority` is not among them.
       NAMED RESIDUAL: step 27's "set request's priority to init["priority"]" stores nothing. §2.2.5 Requests
       gives a request a priority ("high", "low" or "auto", "auto" unless stated otherwise) and this engine's
       RequestData does not hold one, because no reader exists for it.
       WHICH RECORD, STATED EXACTLY, BECAUSE THIS CLAUSE USED TO NAME THE WRONG ONE. It said the endpoint
       surface records method, URL, headers and body "and the trusted zone's chokepoint is handed those", then
       sent the next diff to state priority "beside the method" on that record. There is no method there to
       state it beside: `safeFetch(url, opts)` is handed the URL positionally and an opts bag whose keys are a
       CLOSED SET — pageUrl, pageOrigin, destination, provenance, credentialed, headers, signal — enforced by
       `_refuseUnreadOptions`, which DCHECKs on any other key and names `method` and `body` as the two that
       matter, because that file hardcodes `method:"GET"` and reads neither. The method rides the OTHER record:
       the pending line, which is keyed on method AND url together (see `qjs_provide`, which takes the method
       first). The two are different records and this clause conflated them.
       THE NEXT DIFF therefore carries the request's priority as a new key on the chokepoint's opts, added to
       `_SAFEFETCH_OPTIONS` and read by that file's body in the same diff — the closed-set DCHECK is the forcing
       function for the second half and fires on the author's own first call if it is forgotten. WHAT MUST
       EXIST AFTERWARD: a reader in the trusted zone, so the member's value is read rather than only checked.
       ITS ABSENCE SHOWS the day a report distinguishes a `priority: "low"` prefetch from an ordinary call:
       today the two records are identical.
       (Step 27's OTHER arm — "if request's internal priority is not null, update it in an
       implementation-defined manner" — is unreachable from here: §2.2.5 makes internal priority null unless
       stated otherwise, and step 12 copies the input request's, which no path in this engine ever sets.) */
    { "priority",       IDL_ENUM,               false, REQUEST_PRIORITY },
    { "redirect",       IDL_ENUM,               false, REQUEST_REDIRECT },
    { "referrer",       IDL_USVSTRING,          false },
    { "referrerPolicy", IDL_ENUM,               false, REFERRER_POLICY },
    /* `AbortSignal? signal` — a DECLARED interface type, so `new Request(u, {signal: 5})` is Web IDL's
       own TypeError thrown before this constructor's first step rather than a check the body makes. */
    { "signal",         IDL_INTERFACE_NULLABLE, false },
    /* `IPAddressSpace targetAddressSpace`, from LOCAL NETWORK ACCESS §3.1.2 Fetch API's partial dictionary.
       THE CONVERSION IS ENGINE WORK AND THE DECISION IS NOT, and the split is this project's own rule rather
       than a convenience: the member declares which address space the page BELIEVES it is reaching, and what
       a browser does with that belief is refuse or allow a local-network request — a network policy, which
       lives at `extension/lib/safe-fetch.js`'s chokepoint by construction and which that file already makes
       (it refuses a private-network target from a public page, reinstating the browser's own answer that the
       extension's host permissions would otherwise bypass). A second copy of that decision inside the engine
       is the layering violation, so the engine converts §3.2.18 and stops.
       NAMED RESIDUAL: the step LOCAL NETWORK ACCESS §3.1.2 Fetch API appends to §5.4 — it opens "If
       init["targetAddressSpace"] exists, then switch on init["targetAddressSpace"]" and its two arms are
       `public`, "Do nothing", and `local`, "Set request's targetAddressSpace to local" (the enumeration's
       third value, `loopback`, has no arm at all in that draft) — stores nothing here.
       AND THE ATTRIBUTE THE SAME PARTIAL DECLARES IS A DIFFERENT ROW, ABSENT ON PURPOSE, AND THE ENGINE NOW
       SAYS SO WHERE AN INSTRUMENT CAN READ IT — `idl_members_excluded` at this file's prototype build. This
       paragraph said the gap audit REPORTS it, and it did, as one of the four hundred-odd members whose verdict
       line instructs a reader to "implement the member in its real component". For this member that instruction
       is SPEC-WRONG, and a wrong instruction in an instrument every lane reads is the stale-DFAIL failure with
       nothing to grep: a decision reasoned out in prose is, to the auditor, indistinguishable from a member
       nobody has looked at. `partial interface Request { readonly attribute IPAddressSpace
       targetAddressSpace; }` states no getter steps, and the slot it would reflect cannot be spelled in its
       own declared type: LOCAL NETWORK ACCESS §3.1.1 Fetching says, in full, "Request objects are given a new
       target IP address space property, initially null", while `IPAddressSpace` is NOT NULLABLE and its three
       values are "public", "local" and "loopback". So a freshly-built `new Request(u)` holds null in the slot
       and the attribute has no value it is permitted to answer with — the member is unimplementable AS
       WRITTEN, not merely unbuilt, and every candidate answer is invented rather than computed. "public" is
       the tempting one and it is the worst, because the constructor's `public` arm is "Do nothing" and would
       round-trip, making the invention look like a reading; that draft's own check then asserts "request's
       target IP address space is not public", so the one value a getter could plausibly return is the one
       value the algorithm guarantees is never stored. That is §NO STUBS' getter returning a value where the
       spec computes none, so absence is the correct answer here and stays correct until the draft states
       getter steps or makes the attribute nullable. THAT LAST CLAUSE IS THE ONE THE EXCLUSION CANNOT CHECK
       FOR YOU, and saying so is part of declaring it: the two sides the declaration IS checked from are that
       the corpus still carries the name and that this file does not install it, neither of which moves when a
       draft gains getter steps — so a reader who finds getter steps in that section deletes the exclusion, and
       no run will have told them to. The next diff carries the member on the request
       record the chokepoint receives, so `safe-fetch.js` decides the local-network question with the page's
       own declaration in hand instead of from the host alone.
       ITS ABSENCE SHOWS as `fetch("http://router.local/ping", {targetAddressSpace: "local"})` being graded by
       hostname alone — identical to the same call with the member omitted. */
    { "targetAddressSpace", IDL_ENUM,           false, REQUEST_TARGET_ADDRESS_SPACE },
    /* `any window; // can only be set to null` — the comment is Fetch's own, and steps 10 and 11 are what
       make it true. It is DECLARED `any`, so there is no conversion to perform and no page code to run; the
       refusal is the CONSTRUCTOR's, at the step that states it. Undeclared, `{window: 5}` was accepted. */
    { "window",         IDL_ANY,                false },
};
/* `PrivateToken privateToken`, which the PRIVATE STATE TOKEN API §6.1 Definitions attaches to RequestInit by a
   partial dictionary, is NOT DECLARED ABOVE, AND THAT IS A DECISION RATHER THAN A RESIDUAL.
   THE RESIDUAL THAT STOOD HERE NAMED A CAPABILITY THAT IS BUILT, and it named it by quoting an abort — "a
   dictionary member was declared as a dictionary — the conversion cursor is per-argument, so a nested one would
   read the outer's names". That abort is gone from `core/idl_args.c`: `idl_type_pushes_level` is asked by the
   §3.2.17 member loop itself, and a dictionary-valued member PUSHES its own level and converts against its own
   `IdlDictMember` list, exactly as an IDL_DICT argument does. A reader who obeyed the retired clause would have
   set out to build the walk this engine already runs, which is the failure mode CLAUDE.md names: a claim about
   THIS TREE, true when written, read afterwards as an instruction.
   WHAT IS ACTUALLY ABSENT AND WHY IT STAYS ABSENT. Declaring the member would convert it and nothing more: the
   issuance and redemption protocol, the `Sec-Private-State-Token` request header §12.1 defines, and the
   permissions-policy features §6.2 names are none of them here, and none of them is a conversion. That makes
   declaring it WRONG IN BOTH DIRECTIONS at once — `fetch(u, {privateToken: {}})` would throw a §3.2.17 TypeError
   where a user agent WITHOUT the feature does not throw at all (the member is not in its RequestInit), while
   still not issuing or redeeming a token where a user agent WITH the feature does. Undeclared, this engine is
   exactly a user agent that does not ship the API, which is a state real browsers are in and a page can handle.
   So the honest answer is the absence, and it is not silent: `node engine/idlgen.mjs` names `RequestInit:
   privateToken` on every run, which is the row that keeps this decision visible instead of forgotten.
   WHAT WOULD CHANGE IT is the FEATURE, not the type. If it is ever built, the one declared type still missing
   is `sequence<USVString>` for `issuers` — there is no such row in the IdlArgType enum, and the DOMString
   spelling is a different type: §3.2.12 USVString replaces every unpaired surrogate with U+FFFD, and an issuer
   is an origin that goes on the wire. The other three members are declarable today (two `required` §3.2.18
   enumerations and one carrying a `= "none"` default, all three shapes already in use above). */
#define REQUEST_INIT_N ((int)(sizeof(REQUEST_INIT) / sizeof(REQUEST_INIT[0])))

/* Web IDL §3.2.17 Dictionary types: a member the page did not supply is NOT ON the converted dictionary, and
   `undefined` is how that reads — which is the same test §5.4 spells "init[member] exists". Every member of
   REQUEST_INIT declares no default (see above), so absence and `undefined` coincide for all of them. */
static bool init_has(JSContext *ctx, JSValueConst init, const char *name)
{
    JSValue v = idl_dict_get(ctx, init, name);
    bool present = !JS_IsUndefined(v);

    JS_FreeValue(ctx, v);
    return present;
}

/* §5.4 step 13's "If init is not empty" — Infra's emptiness of the converted DICTIONARY, so it is a question
   about which members are PRESENT and never about which properties the page's object literal had. */
static bool req_init_is_empty(JSContext *ctx, JSValueConst init)
{
    int i;

    for (i = 0; i < REQUEST_INIT_N; i++)
        if (init_has(ctx, init, REQUEST_INIT[i].name))
            return false;
    return true;
}

/* A dictionary member as a plain string, or `dflt` when the page did not supply it. The declaration has
   already converted each to a real string, so this reads an engine-built object and runs nothing.
   RETURNS NULL FOR EXACTLY ONE THING — a member whose value is UNKNOWN EXTERNAL INPUT, with the TypeError it
   throws live and the DFAILF beside it naming what to build. OOM is a CHECK below and aborts, so a caller
   testing for NULL is testing for that refusal and for nothing else.
   `dflt` IS THE VALUE STEP 12 CARRIED FORWARD, never a constant the call site picked: §5.4 sets a request's
   mode / credentials mode / cache mode / redirect mode / integrity metadata from the INIT MEMBER ONLY WHERE IT
   EXISTS, and from the input request otherwise. A `?:` PAST A FAILED CONVERSION IS GONE with it — the read
   could only fail on OOM, and answering that with the default turned "this engine could not allocate" into
   "the page asked for same-origin credentials", which is a plausible datum where a crash belonged. */
static char *init_str(JSContext *ctx, JSValueConst init, const char *name, const char *dflt)
{
    JSValue v = idl_dict_get(ctx, init, name);
    char *r;
    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return js_strdup(ctx, dflt); }
    /* SEVEN MEMBERS ARRIVE HERE AND THE UNKNOWN ONE SPLITS THEM IN TWO, WHICH IS WHY THIS IS ONE ABORT AND
     * NOT ONE ANSWER. `fetch(u, {credentials: cfg.creds})` and `fetch(u, {integrity: cfg.sri})` reach this
     * same line with unknown external input and take the document down at the ToString boundary below, and
     * what each of them NEEDS is different:
     *
     * FIVE OF THE SEVEN ARE ENUMERATIONS — `mode`, `credentials`, `cache`, `redirect`, `referrerPolicy` — and
     * an unknown at one of them needs NOTHING of this record at all. Web IDL §3.2.18 Enumeration types makes
     * the member's domain FINITE: the declared value list, plus the TypeError §3.2.18 states for everything
     * else. Every one of those worlds places a REAL string from the list, which a `char *` carries perfectly,
     * so the arms are feasible worlds the algorithms behind this boundary observe apart — `credentials:
     * "include"` is a credentialed request and `"omit"` is not, and picking either for a value nothing is
     * known about deletes the other. That is the same shape core/idl_args.h's `idl_concolic_rule` already
     * files Web IDL §3.2.3 boolean under, and IDL_ENUM sits under that switch's `default:` at CROSSES today —
     * which is the state that row's own comment records the boolean having been in, a type whose conversion
     * decides a world filed with the types whose conversion merely coerces bytes a body still holds. (That
     * sentence is idl_args.h's, not a standard's; grep the row before acting on this, since it is a claim
     * about a tree that moves.) The resolution site is the DECLARATION's, which holds the machine this
     * operation does not, and the member loop in idl_args.c already asserts that every FORKS type has an arm
     * in it and tells its reader to give the type that arm beside the boolean's. So an enum member is not
     * this file's to answer, and answering it here would be the second copy of a rule that exists to have
     * one.
     *
     * TWO OF THE SEVEN ARE PLAIN STRINGS — `referrer` (USVString) and `integrity` (DOMString) — whose bytes
     * are the page's own and whose worlds are not enumerable. Those need what §5.4 step 25's method needs and
     * for the same reason: a record field that can BE the unknown. See that step's block below.
     *
     * A `?:` PAST THIS IS THE ONE ANSWER THAT IS NEVER RIGHT, for the reason the `dflt` paragraph above
     * already gives about OOM: a default here turns "the page asked for something this engine cannot state"
     * into "the page asked for same-origin credentials", which is a plausible datum where a crash belongs. */
    if (concolic_is(v)) {
        DFAILF("Fetch §5.4 new Request(input, init) applied the RequestInit member `%s`, and its value is "
               "UNKNOWN EXTERNAL INPUT. If `%s` is one of §5.4's five ENUMERATION members the fix is not "
               "here: Web IDL §3.2.18 Enumeration types gives it a finite domain whose every world is a real "
               "string this record already carries, so give IDL_ENUM its FORKS row in idl_concolic_rule and "
               "its arm in the dictionary member loop, beside the boolean's. If it is `referrer` or "
               "`integrity` it needs what §5.4 step 25's method needs — that field on the request record as "
               "a JSValue, and this operation holding the JSStepHdr a fork is asked through", name, name);
        JS_FreeValue(ctx, v);
        JS_ThrowTypeError(ctx, "this engine cannot yet apply a RequestInit member whose value is unknown "
                               "external input");
        return NULL;
    }
    {
        const char *c = JS_ToCString(ctx, v);
        CHECK(c != NULL, "OOM reading a RequestInit member the declaration has already converted to a string");
        r = js_strdup(ctx, c);
        JS_FreeCString(ctx, c);
    }
    JS_FreeValue(ctx, v);
    return r;
}

/* FETCH §5.4 new Request(input, init) STEPS 10-27, ONCE — see request.h for why this is a shared operation and
   not a span of the constructor's first stage.
   NONE OF IT RUNS THE PAGE'S CODE. `init` is the dictionary Web IDL §3.2.17 Dictionary types already built, so
   every [[Get]], every coercion and every §3.2.18 refusal is behind us; that is what lets eighteen steps sit in
   one stage of whichever machine calls this. The ORDER is still §5.4's own, because it decides WHICH TypeError
   a page sees when two members are bad at once — step 17 refuses a "navigate" mode before step 25 looks at the
   method — and that order is observable. */
int request_init_apply(JSContext *ctx, JSValueConst init, const RequestRecord *from, RequestRecord *rec)
{
    bool init_not_empty = !req_init_is_empty(ctx, init);

    DCHECK(rec != NULL, "§5.4's member application was handed no request record to fill");
    DCHECK(rec->method == NULL && rec->mode == NULL,
           "§5.4 steps 10-27 were applied twice to one request record — every field below is a fresh "
           "allocation, so a second pass leaks the first pass's nine strings and there is no reader that "
           "could report it: the record looks exactly as it should");

    /* §5.4 STEP 10: "If init["window"] exists and is NON-NULL, then throw a TypeError." Fetch's IDL declares
       the member `any` and writes the reason beside it as a comment — "can only be set to null" — so the type
       converts nothing and this step is the entire refusal. Undeclared and unchecked, every value a page could
       write was accepted: `new Request(u, {window: window})`, which is the shape the member exists to refuse,
       built a Request in this engine and a TypeError in every browser.
       IT SITS FIRST BECAUSE THE ORDER IS OBSERVABLE. Step 5's URL parse and its credentials check are the
       CALLER's and run before this; step 17's "navigate" mode and step 21's cache/mode pair are below, so
       `new Request(u, {window: 5, mode: "navigate"})` reports this one — which is what a browser does.
       §3.2.17 puts no member on the converted dictionary when the page wrote undefined, so `init_has` IS step
       10's "exists" and `{window: undefined}` is absent rather than present-and-not-null. `{window: null}` is
       the one accepted spelling.
       NAMED RESIDUAL: step 11 — "If init["window"] exists, then set traversableForUserPrompts to
       "no-traversable"" — sets nothing. A request's TRAVERSABLE FOR USER PROMPTS is the navigable a fetch may
       raise an authentication prompt in, and this engine models no such prompt: there is no reader, so a field
       here would be a value nothing ever asks for. The next diff builds the request record's traversable for
       user prompts together with the §4.4 HTTP fetch step that consults it, so the two arrive with a question
       and an answer. ITS ABSENCE SHOWS the day a 401 with a `WWW-Authenticate` reaches a flow: `{window: null}`
       must make that reply arrive unprompted, and here every reply does, so the member's whole effect is
       currently indistinguishable from its absence. */
    if (init_has(ctx, init, "window")) {                                          /* step 10 */
        JSValue w = idl_dict_get(ctx, init, "window");
        bool nonnull = !JS_IsNull(w);

        JS_FreeValue(ctx, w);
        if (nonnull) {
            JS_ThrowTypeError(ctx, "the RequestInit member `window` can only be set to null");
            return -1;
        }
    }

    /* §5.4 step 13: "If init is not empty" resets the referrer to "client" and the referrer policy to the
       empty string BEFORE steps 14-15 read the members — so a Request input's referrer survives only a
       construction that supplies no member at all. This is the one place step 13 is observable here: its other
       clauses are the reload/history-navigation flags and the origin, which this engine's request record does
       not hold, and the "navigate" mode it rewrites is asserted unreachable below.
       EVERY DEFAULT BELOW IS STEP 12'S CARRY-FORWARD, and the bare constant is what a STRING input starts
       from. §5.4 spells each of these "If init[member] exists, then set request's <field> to it", over a
       request that already holds the input's value — so a constant in the `dflt` position is a statement that
       the page asked for it, and for a Request input that statement is false. */
    /* EVERY init_str BELOW IS CHECKED AT ITS OWN STEP, AND THE ANSWER IT IS CHECKED FOR IS ITS REFUSAL AND
       NEVER AN OOM — that is a CHECK inside it, which aborts. NULL therefore means exactly one thing (see its
       block: a member whose value is unknown external input) with the TypeError already live, and it is
       checked HERE rather than at the CHECK below because §5.4's ORDER IS OBSERVABLE: a page that writes two
       bad members sees the earlier step's refusal, and two of these fields are `strcmp`'d by the steps that
       follow them. The record is left safe to free either way, which is this operation's stated contract. */
    rec->referrer        = init_str(ctx, init, "referrer",                          /* steps 13-14 */
                                    (from && !init_not_empty) ? from->referrer : "about:client");
    if (!rec->referrer) return -1;
    rec->referrer_policy = init_str(ctx, init, "referrerPolicy",                    /* steps 13, 15 */
                                    (from && !init_not_empty) ? from->referrer_policy : "");
    if (!rec->referrer_policy) return -1;
    /* §5.4 steps 16-18: "navigate" is not a mode a page may ask for. Step 16 is "Let mode be init["mode"] if
       it exists, and fallbackMode otherwise" — and fallbackMode is set to "cors" at step 5.5, the STRING arm,
       and left null by the Request arm; step 18's "If mode is non-null" is what then leaves a Request input's
       own mode standing. So "cors" here is the string input's fallback and never a Request's. */
    DCHECK(!from || strcmp(from->mode, "navigate") != 0,
           "a Request used as `input` carried mode \"navigate\" — step 17 throws on it and no other path in "
           "this engine mints a Request, so step 13's \"if request's mode is navigate, set it to same-origin\" "
           "is unreachable rather than unimplemented");
    rec->mode = init_str(ctx, init, "mode", from ? from->mode : "cors");
    if (!rec->mode) return -1;
    if (!strcmp(rec->mode, "navigate")) {                                         /* step 17 */
        JS_ThrowTypeError(ctx, "a Request cannot be constructed with mode \"navigate\"");
        return -1;
    }
    rec->credentials     = init_str(ctx, init, "credentials",                      /* step 19 */
                                    from ? from->credentials : "same-origin");
    if (!rec->credentials) return -1;
    rec->cache           = init_str(ctx, init, "cache", from ? from->cache : "default");   /* step 20 */
    if (!rec->cache) return -1;
    /* §5.4 step 21: "only-if-cached" asks the cache to answer without going to the network, which only means
       anything for a same-origin request — so any other mode is a TypeError. */
    if (!strcmp(rec->cache, "only-if-cached") && strcmp(rec->mode, "same-origin")) {
        JS_ThrowTypeError(ctx, "a Request with cache \"only-if-cached\" must have mode \"same-origin\"");
        return -1;
    }
    rec->redirect        = init_str(ctx, init, "redirect", from ? from->redirect : "follow");   /* step 22 */
    if (!rec->redirect) return -1;
    rec->integrity       = init_str(ctx, init, "integrity", from ? from->integrity : "");       /* step 23 */
    if (!rec->integrity) return -1;
    /* §5.4 step 24: "If init["keepalive"] exists, then set request's keepalive to it" — so an ABSENT member
       leaves step 12's value standing, which is the input request's or `false` for a string.
       THE READ IS `idl_dict_bool` AND NOT A BARE `JS_ToBool`, because ToBoolean over unknown external input
       pins it to `true` and deletes the false world; the declared member has already been converted, so the
       reader's assert is what says that is still so if the declaration ever changes underneath it. */
    rec->keepalive = from ? from->keepalive : 0;                                  /* step 12 */
    if (init_has(ctx, init, "keepalive"))                                         /* step 24 */
        rec->keepalive = idl_dict_bool(ctx, init, "keepalive");
    /* §5.4 step 25's method: a token, not a forbidden method, then normalized. An absent member leaves step
       12's — the input request's already-normalized method, or `GET` for a string input. */
    {
        JSValue mv = idl_dict_get(ctx, init, "method");
        if (JS_IsUndefined(mv)) {
            /* §2.2.5 Requests: "A request has an associated method (a method). Unless stated otherwise it is
               `GET`." So this constant is the SPEC's initial value for the request step 5 minted out of a
               STRING input, reached only where the member is absent — not a field this reader defaulted. */
            rec->method = js_strdup(ctx, from ? from->method : "GET");
        } else if (concolic_is(mv)) {
            /* §5.4 STEP 25 OVER A METHOD NOBODY KNOWS — THE THREE THINGS THAT ARE ALREADY SETTLED, AND THE ONE
             * THAT IS NOT. `fetch(u, {method: options.method || "get"})` is the shape, one arm of that `||`
             * carrying the unknown, and an orphan drive of any wrapper reaches it that way by construction:
             * Web IDL declares `method` a ByteString and core/idl_args.h's IDL_CONCOLIC_CROSSES passes unknown
             * external input through that conversion UNCONVERTED on purpose, so opacity survives it.
             *
             * (1) READING BYTES OFF IT IS WRONG, which is what the line below did and what took the document
             * down: ECMAScript §7.1.19 ToString ( arg ) steps 9-12 hand an Object to §7.1.1 ToPrimitive (
             * input [ , preferredType ] ), which over an unknown is the identity, so the byte consumer owed C
             * a JSString that does not exist.
             * (2) SUBSTITUTING ITS DISPLAY SHAPE IS WORSE and is the answer to refuse first, because it does
             * not abort. `{orphan….arg0}.method` is not a token, so §2.2.1 Methods' "A method is a byte
             * sequence that matches the method token production" fails and step 25 throws a TypeError that
             * DELETES the request — the endpoint this tool exists to emit — by running a predicate over a
             * string no run ever computed. It would also decide step 32.1's CORS-safelisted-method test and
             * step 35's GET/HEAD body test by `strcmp` against that same fabricated string.
             * (3) THE COMPLETIONS ARE TWO AND NOT THREE, counted from what the PAGE CAN OBSERVE rather than
             * from the branches of the prose: `new Request` returned, or it threw a TypeError. §2.2.1 makes a
             * forbidden method a method ("A forbidden method is a method that is a byte-case-insensitive
             * match for `CONNECT`, `TRACE`, or `TRACK`"), so the two refusals partition the domain — but §5.4
             * step 25 throws ONE TypeError for both where XHR §3.5.1 The open() method names a "SyntaxError"
             * and a "SecurityError" apart, and two arms a page cannot tell apart are one world twice. There is
             * no third: the operand is a value, not an algorithm that could settle nothing.
             *
             * WHAT IS MISSING IS SOMEWHERE TO PUT THE ANSWER. On the ordinary completion step 25.3's
             * "Normalize method" is a DERIVATION and never a decision — §2.2.1's "To normalize a method, if it
             * is a byte-case-insensitive match for `DELETE`, `GET`, `HEAD`, `OPTIONS`, `POST`, or `PUT`,
             * byte-uppercase it" answers an unknown with an unknown in both of its worlds — and step 25.4 then
             * has to STORE that, where request.h declares a `char *`. WHAT THE NEXT DIFF BUILDS: §2.2.5
             * Requests' method on the request record as a JSValue so it can BE the unknown, with `Request`'s
             * `method` getter answering it, steps 32.1 and 35 asking the fork rather than `strcmp`, the @H
             * surface taking the domain-annotated SHAPE (concolic_name_cstr) in the one column an endpoint is
             * identified by, and §4.1 Main fetch answering a method this agent cannot spell INSIDE the agent
             * — credentialed by construction, not established to be in RFC 9110 §9.2.1 Safe Methods' safe set,
             * and off a forced arm is the one combination that is never a setting, whose correct output is to
             * derive it, report it and not send it. AND THE FORK NEEDS A MACHINE: this operation is §5.4 steps
             * 10-27 for BOTH of its callers and holds no JSStepHdr, so step_fork_run cannot be asked here
             * until it takes one — which is the single change that unblocks this member and the seven
             * `init_str` members above it.
             * HOW ITS ABSENCE SHOWS: this abort, on any bundle whose method is not a literal. */
            DFAIL("Fetch §5.4 new Request(input, init) step 25 was handed a method that is UNKNOWN EXTERNAL "
                  "INPUT. Its two observable completions are `returned` and `threw a TypeError`, and the "
                  "ordinary one has nowhere to store its answer: core/fetch/request.h declares the record's "
                  "method a `char *`, which cannot carry an unknown any more than the ToString boundary "
                  "below it could. Build §2.2.5 Requests' method as a JSValue on the record, and give this "
                  "operation the JSStepHdr its fork has to be asked through");
            JS_FreeValue(ctx, mv);
            JS_ThrowTypeError(ctx, "this engine cannot yet build a Request whose method is unknown external "
                                   "input");
            return -1;
        } else {
            const char *mc = JS_ToCString(ctx, mv);
            if (!mc) { JS_FreeValue(ctx, mv); return -1; }
            rec->method = request_method_check(ctx, mc);
            JS_FreeCString(ctx, mc);
            if (!rec->method) { JS_FreeValue(ctx, mv); return -1; }
        }
        JS_FreeValue(ctx, mv);
    }
    /* §2.2.5: "unless stated otherwise it is the empty string", and neither §5.4 nor §5.6 states otherwise —
       so a script-constructed request and a `fetch()` are both the empty destination, which is the positive
       statement "these bytes are data" that the trusted zone's CORB class is read off. */
    rec->destination = js_strdup(ctx, from ? from->destination : "");
    CHECK(rec->method && rec->mode && rec->credentials && rec->cache && rec->redirect && rec->referrer &&
          rec->referrer_policy && rec->integrity && rec->destination,
          "request: OOM applying Fetch §5.4's RequestInit members to a request record");
    /* §5.4 step 27's "set request's priority to init["priority"]" and LOCAL NETWORK ACCESS §3.1.2 Fetch API's
       `targetAddressSpace` switch store nothing — each is a NAMED RESIDUAL at its row in REQUEST_INIT above,
       where the member and the reason a reader does not exist yet are stated together. */
    return 0;
}

const IdlDictMember *request_init_members(int *n)
{
    DCHECK(n != NULL, "the RequestInit declaration table was asked for without a place to report its length");
    *n = REQUEST_INIT_N;
    return REQUEST_INIT;
}

static int js_request_ctor_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    JSRequestCtorState *s = st;
    JSValueConst input = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;
    RequestData *d;
    int r;

    if (hdr->stage == REQ_CTOR_RECORD) {
        JSValue obj;
        /* §5.4 step 6: "Set request to input's request". `from` IS that request — every field of it, not the
           URL alone — and steps 12 and 13 below are what carry it forward. */
        RequestData *from = request_of(input);
        const char *from_request = from ? from->url : NULL;

        if (JS_IsUndefined(hdr->this_val)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "constructor Request requires 'new'");
            return -1;
        }
        {
            JSValue rproto = JS_GetClassProto(ctx, g_request_class);
            DCHECK(!JS_IsNull(rproto), "a Request was minted in a realm that never ran its install");
            obj = JS_NewObjectProtoClass(ctx, rproto, g_request_class);
            JS_FreeValue(ctx, rproto);
        }
        if (JS_IsException(obj)) { JS_FreeValue(ctx, cb_result); return -1; }
        d = js_mallocz(ctx, sizeof(*d));
        if (!d) { JS_FreeValue(ctx, obj); JS_FreeValue(ctx, cb_result); return -1; }
        d->headers = JS_UNDEFINED;
        d->blob_entry = JS_UNDEFINED;
        /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST STEP THAT CAN THROW — the failure path frees exactly
           what the record holds, so a field handed over late is one the teardown reads uninitialised. The
           signal is BUILT at the end of this stage, where §5.4 builds it. */
        d->signal = JS_UNDEFINED;
        JS_SetOpaque(obj, d);
        s->result = obj;

        /* §5.4 step 5: a STRING input is parsed against the base URL, and a failure is a TypeError. A REQUEST
           input contributes its URL already parsed. THE TWO ARMS DIFFER HERE AND AT EVERY MEMBER BELOW, which
           is the correction this comment used to be the whole of: it said "which is why the two arms differ
           only here", and step 12 lists thirteen properties a Request input contributes — method, header
           list, referrer, referrer policy, mode, CREDENTIALS MODE, cache mode, redirect mode, integrity
           metadata and keepalive among them. Taking only the URL meant `new Request(r)` — the request-wrapping
           idiom every fetch interceptor is written in — answered POST as GET, `include` as `same-origin` and
           `no-cors` as `cors`, out of the DEFAULTS below rather than out of anything the page wrote. Each was
           a REAL value belonging to some request, which is what made it invisible: a wrong answer the engine
           then modelled, and every @H example value and @S verdict downstream of one is derived from a request
           the page never made. */
        if (from_request) {
            d->url = js_strdup(ctx, from_request);
        } else {
            UrlRecord rec;
            size_t n = 0;
            const char *in = JS_ToCStringLen(ctx, &n, input);
            bool ok;
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            if (!in) return -1;
            /* AGAINST THE API BASE URL, through Fetch's one parse. `new Request("/api/users")` is how a
               bundle names its own endpoints, and with a NULL base every one of them was a TypeError. */
            ok = fetch_parse_url(ctx, &rec, in, n);
            JS_FreeCString(ctx, in);
            if (!ok) {
                url_record_free(&rec);
                JS_ThrowTypeError(ctx, "the Request input is not a valid URL");
                return -1;
            }
            /* §5.4 step 2.2: a URL with credentials is a TypeError — a page may not put a password on the
               wire by writing it into a fetch. */
            if ((rec.username && *rec.username) || (rec.password && *rec.password)) {
                url_record_free(&rec);
                JS_ThrowTypeError(ctx, "the Request input URL includes credentials");
                return -1;
            }
            {
                /* url_serialize returns a plain malloc'd string, and everything this struct holds is freed
                   with the ENGINE's allocator — mixing the two is a heap corruption that only shows up at
                   teardown, which is exactly where it showed up. Copy in, free out. */
                char *ser = url_serialize(&rec, false);
                d->url = js_strdup(ctx, ser);
                free(ser);
                /* §5.4: RESOLVE the blob URL now and hold what it named. A page that revokes eagerly —
                   `const r = new Request(u); URL.revokeObjectURL(u); fetch(r)` — still fetches, because the
                   entry belongs to the request from this moment. The FRAGMENT is not part of the entry's
                   identity, which is why the lookup key excludes it. */
                if (rec.scheme && !strcmp(rec.scheme, "blob")) {
                    char *key = url_serialize(&rec, true);
                    d->blob_entry = blob_url_lookup(ctx, key, strlen(key));
                    free(key);
                }
            }
            url_record_free(&rec);
        }

        /* §5.4 STEPS 10-27, THROUGH THE ONE IMPLEMENTATION OF THEM (request.h). They stood written out here,
           and `fetch(input, init)` — which Fetch §5.6 "Fetch methods" step 2 defines as an invocation of THIS
           constructor — had its own partial copy that read three members and dropped thirteen. Two copies of
           one algorithm is how a step lands in one of them: this is the copy that grew step 10, step 21 and
           step 32.1, and the other one never did. */
        if (request_init_apply(ctx, init, from ? &from->rec : NULL, &d->rec) < 0)
            return -1;
        CHECK(d->url != NULL, "request: OOM building a Request's URL");
        /* §5.4 steps 4 / 6.3 / 26 and then "let signals be « signal » if signal is non-null; otherwise « »"
           and "set this's signal to the result of creating a dependent abort signal from signals". The two
           sources are the Request `input` (a `new Request(other)` inherits other's signal) and init["signal"],
           and init WINS because step 26 runs after step 6.3. It is built HERE — after every step of this stage
           that can throw and before the header fill, which is where §5.4 puts it — so a construction that
           refuses has registered no dependency on a signal the page still holds. */
        {
            JSValueConst sources[1];
            int n = 0;
            JSValue given = idl_dict_get(ctx, init, "signal");

            if (abort_signal_is(ctx, given))
                sources[n++] = given;
            else if (from) {
                DCHECK(abort_signal_is(ctx, from->signal),
                       "a Request used as `input` carried no AbortSignal — §5.4 gives every Request one, so a "
                       "source without one was built by a path that skipped the dependent signal");
                sources[n++] = from->signal;
            }
            d->signal = abort_signal_dependent_new(ctx, sources, n);
            JS_FreeValue(ctx, given);
            if (JS_IsException(d->signal)) { d->signal = JS_UNDEFINED; return -1; }
        }
        /* §5.4 step 12's HEADER LIST: "A copy of request's header list" — the input Request's, which this
           constructor took none of. Step 33 then re-appends every entry UNDER THE NEW REQUEST'S GUARD when
           init is not empty, which is what drops a `Range` that a `no-cors` mode no longer admits; the guard
           is computed here because it is a function of the mode this stage just settled. Running the guarded
           append unconditionally is the spec's own answer in every case this engine can reach: init being
           EMPTY is exactly the case in which the mode — and therefore the guard — is the input's own, and a
           list §5.1 built under a guard re-appends through that same guard unchanged.
           When init["headers"] EXISTS the copy is discarded rather than seeded: step 33's own sub-steps set
           `headers` to the init member and then empty this's header list, leaving nothing of the copy. */
        if (from && !init_has(ctx, init, "headers")) {
            const HeaderList *src = headers_list_of(from->headers);
            HeadersGuard guard = !strcmp(d->rec.mode, "no-cors") ? HEADERS_GUARD_REQUEST_NO_CORS
                                                             : HEADERS_GUARD_REQUEST;
            int i;

            DCHECK(src != NULL, "a Request used as `input` carried no Headers object — §5.4 gives every "
                                "Request one at step 31, so a source without one was built by a path that "
                                "skipped it");
            for (i = 0; i < src->n; i++)
                if (header_list_append_guarded(ctx, &s->list, guard, src->e[i].name, src->e[i].value) < 0)
                    return -1;
        }
        headers_fill_init(&s->fill);
        hdr->stage = REQ_CTOR_HEADERS;
    }

    d = request_of(s->result);
    DCHECK(d != NULL, "the Request the constructor allocated stopped being one mid-construction");

    if (hdr->stage == REQ_CTOR_HEADERS) {
        /* §5.4's headers: guard "request", or "request-no-cors" when the mode says so — which is the ONLY way
           either guard becomes observable, since a page's own Headers has guard "none". */
        HeadersGuard guard = !strcmp(d->rec.mode, "no-cors") ? HEADERS_GUARD_REQUEST_NO_CORS
                                                         : HEADERS_GUARD_REQUEST;
        JSValue hv;

        /* §5.4 step 32.1: under a "no-cors" mode the method must be a CORS-SAFELISTED METHOD — §2.2.1 Methods'
           `GET`, `HEAD` or `POST` — and anything else is a TypeError. It was missing, so
           `new Request(u, {mode:"no-cors", method:"PUT"})` built a request no browser will make, and the
           mode carried forward at step 12 gives that shape a second way in. The method here is normalized, so
           the comparison is against the uppercase spellings and nothing else. */
        if (guard == HEADERS_GUARD_REQUEST_NO_CORS && strcmp(d->rec.method, "GET") &&
            strcmp(d->rec.method, "HEAD") && strcmp(d->rec.method, "POST")) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "a Request with mode \"no-cors\" must use a CORS-safelisted method");
            return -1;
        }
        hv = idl_dict_get(ctx, init, "headers");
        r = headers_fill_run(ctx, hdr, &s->fill, hv, &s->list, guard, cb_result, out_cb, out_argc);
        JS_FreeValue(ctx, hv);
        if (r > 0) return r;
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        JS_FreeValue(ctx, d->headers);
        d->headers = headers_new(ctx, &s->list, guard);
        if (JS_IsException(d->headers)) return -1;
        hdr->stage = REQ_CTOR_BODY;
    }

    DCHECK(hdr->stage == REQ_CTOR_BODY,
           "the Request constructor was re-entered at a stage §5.4 does not have");
    /* §5.4 steps 34-41. A body on a GET or a HEAD is a TypeError. The extraction itself is §5.2's, which
       body.c owns — both including interfaces run the same six-armed union, and the Content-Type it produces
       is set only where the header list has none.
       AND `inputBody` IS THE HALF THAT WAS ABSENT: step 34 is "Let inputBody be input's request's body if
       input is a Request object", and every step after it reads that name. Without it a `new Request(post)`
       came out with the method (now) and NO BODY, which is a request the server answers differently and the
       page never wrote — and step 35's TypeError, which is about the INHERITED body as much as the init's,
       could not fire at all. */
    JS_FreeValue(ctx, cb_result);
    {
        RequestData *from = request_of(input);
        bool input_body = from && from->body.has;
        JSValue bv = idl_dict_get(ctx, init, "body");
        bool init_body = !JS_IsUndefined(bv) && !JS_IsNull(bv);

        /* STEP 35: "If either init["body"] exists and is non-null OR INPUTBODY IS NON-NULL, and request's
           method is `GET` or `HEAD`, then throw a TypeError" — so `new Request(post, {method:"GET"})` is
           refused rather than silently issued bodiless. */
        if ((init_body || input_body) && (!strcmp(d->rec.method, "GET") || !strcmp(d->rec.method, "HEAD"))) {
            JS_FreeValue(ctx, bv);
            JS_ThrowTypeError(ctx, "a Request with a GET or HEAD method cannot have a body");
            return -1;
        }
        if (init_body) {                                                   /* step 37 */
            char *mime = NULL;
            /* §5.4 extracts "with keepalive set to request's keepalive", and that is the value step 24 read
               out of init a stage ago — not a constant. §5.2's ReadableStream arm begins "If keepalive is
               true, then throw a TypeError", so a hardcoded false made `new Request(u, {method: "POST",
               keepalive: true, body: stream})` build a request a browser refuses. */
            if (body_extract(ctx, &d->body, bv, d->rec.keepalive != 0, &mime) < 0) {
                free(mime);
                JS_FreeValue(ctx, bv);
                return -1;
            }
            if (mime) {
                const HeaderList *hl = headers_list_of(d->headers);
                char *existing = header_list_get(hl, "content-type");
                if (!existing)
                    header_list_append((HeaderList *)hl, "content-type", mime);
                free(existing);
                free(mime);
            }
        }
        JS_FreeValue(ctx, bv);
        /* STEP 39: "If inputOrInitBody is non-null and inputOrInitBody's SOURCE IS NULL, then: …". Step 38
           names inputOrInitBody as initBody where there is one and inputBody otherwise, so the body this step
           asks about is the one just extracted or the input Request's — and the question it asks of it is
           §5.2 BodyInit unions' `source`, which core/fetch/body.h stores because it cannot be derived. A null
           source means exactly one thing: the body came from the ReadableStream arm, the only arm of §5.2's
           switch that names no source. The whole step was absent, so a streaming request body was accepted
           with no `duplex` and under any mode — two TypeErrors a browser throws and this engine did not.
           NAMED RESIDUAL: sub-step 39.3, "Set this's request's use-CORS-preflight flag", sets nothing. §2.2.5
           Requests defines it — "A request has an associated use-CORS-preflight flag. Unless stated
           otherwise, it is unset." — and states what setting it buys: "The use-CORS-preflight flag being set
           is one of several conditions that results in a CORS-preflight request." The preflight itself is the
           trusted zone's, because `extension/lib/safe-fetch.js` is where a cross-origin decision is made by
           construction. The next diff carries the flag as a new key on that file's opts, added to
           `_SAFEFETCH_OPTIONS` and read by its body in the same diff, so a preflight is asked for by the
           request that requires one rather than inferred at the chokepoint.
           ITS ABSENCE SHOWS IN THE DERIVED RECORD AND NOT ON THE WIRE, which is the correction this clause
           needed: it used to say a streamed cross-origin POST "reaching the chokepoint" is indistinguishable
           from a byte-bodied one. No POST reaches it. `safeFetchMethodRefusal` returns a decline for every
           non-GET and is asked at the CALL SITE — `extension/bridge.js` on both the pending and the XHR
           paths, and `engine/trusted.mjs` — so a request that could require a preflight is refused before the
           chokepoint sees it, and the flow stays parked rather than being fired. What the flag's absence
           actually costs is the REPORT: CLAUDE.md's §A-REQUEST-CARRIES-THE-PROVENANCE says a derived-and-
           unfired request IS the report, and a Request built with a stream body and a cross-origin mode
           derives a record identical to the byte-bodied one, carrying nothing that says a browser would have
           preflighted it. */
        {
            const BodyState *ioi = init_body ? &d->body : (input_body ? &from->body : NULL);

            if (ioi && ioi->source_null) {
                /* STEP 39.1: "If initBody is non-null and init["duplex"] does not exist, then throw a
                   TypeError." It is the member's PRESENCE and never its value — `RequestDuplex` has the one
                   value "half", so §3.2.18 has already refused every other spelling at the conversion, and
                   what is left for this step to test is whether the page said anything at all. */
                if (init_body && !init_has(ctx, init, "duplex")) {
                    JS_ThrowTypeError(ctx, "a Request with a ReadableStream body must set duplex to \"half\"");
                    return -1;
                }
                /* STEP 39.2: "If this's request's mode is neither "same-origin" nor "cors", then throw a
                   TypeError." A streamed body cannot be sent no-cors — there is no way to preflight it. */
                if (strcmp(d->rec.mode, "same-origin") && strcmp(d->rec.mode, "cors")) {
                    JS_ThrowTypeError(ctx, "a Request with a ReadableStream body must have mode "
                                           "\"same-origin\" or \"cors\"");
                    return -1;
                }
            }
        }
        /* STEPS 38, 40-41: "Let inputOrInitBody be initBody if it is non-null; otherwise inputBody" — so the
           input's body is this request's ONLY where the init supplied none, which is the whole of the
           `new Request(r, {...})` wrapper idiom. */
        if (!init_body && input_body) {
            bool locked = false;
            /* STEP 41.1: "If inputBody is unusable, then throw a TypeError" — §2.2.4 Bodies' unusable is
               DISTURBED **or** LOCKED, which is the pair Response.clone's own entry reports, and not the read
               latch alone: a body a page has taken a reader on cannot be proxied, and reporting only `used`
               would reach the proxy and fail inside it — the wrong error at the wrong place. */
            readable_stream_query(from->body.stream, NULL, &locked);
            if (from->body.used || readable_stream_disturbed(from->body.stream) || locked) {
                JS_ThrowTypeError(ctx, "a Request whose body is disturbed or locked cannot be used as the "
                                       "input of a new Request");
                return -1;
            }
            /* STEP 41.2: "set finalBody to the result of CREATING A PROXY FOR inputBody" — Streams §9.5
               Piping's create-a-proxy, which is NOT §2.2.4's clone: the input becomes locked and disturbed,
               so `new Request(r)` leaves `r.text()` throwing. body.c owns that distinction. */
            if (body_create_proxy(ctx, input, &from->body, &d->body) < 0)
                return -1;
        }
    }
    *presult = s->result;
    s->result = JS_UNDEFINED;
    return 0;
}

static const IdlStepDecl js_request_ctor_decl = {
    js_request_ctor_step, sizeof(JSRequestCtorState), js_request_ctor_visit, js_request_ctor_release,
    "Fetch §5.4 new Request(input, init)", REQ_CTOR_STEPS
};

static const JSCFunctionListEntry js_request_proto_funcs[] = {
    JS_CFUNC_DEF("clone", 0, js_request_clone),
    JS_CGETSET_MAGIC_DEF("method", js_request_get, NULL, REQ_METHOD),
    JS_CGETSET_MAGIC_DEF("url", js_request_get, NULL, REQ_URL),
    JS_CGETSET_MAGIC_DEF("headers", js_request_get, NULL, REQ_HEADERS),
    JS_CGETSET_MAGIC_DEF("destination", js_request_get, NULL, REQ_DESTINATION),
    JS_CGETSET_MAGIC_DEF("referrer", js_request_get, NULL, REQ_REFERRER),
    JS_CGETSET_MAGIC_DEF("referrerPolicy", js_request_get, NULL, REQ_REFERRER_POLICY),
    JS_CGETSET_MAGIC_DEF("mode", js_request_get, NULL, REQ_MODE),
    JS_CGETSET_MAGIC_DEF("credentials", js_request_get, NULL, REQ_CREDENTIALS),
    JS_CGETSET_MAGIC_DEF("cache", js_request_get, NULL, REQ_CACHE),
    JS_CGETSET_MAGIC_DEF("redirect", js_request_get, NULL, REQ_REDIRECT),
    JS_CGETSET_MAGIC_DEF("integrity", js_request_get, NULL, REQ_INTEGRITY),
    JS_CGETSET_MAGIC_DEF("keepalive", js_request_get, NULL, REQ_KEEPALIVE),
    JS_CGETSET_MAGIC_DEF("isReloadNavigation", js_request_get, NULL, REQ_IS_RELOAD_NAV),
    JS_CGETSET_MAGIC_DEF("isHistoryNavigation", js_request_get, NULL, REQ_IS_HISTORY_NAV),
    JS_CGETSET_MAGIC_DEF("duplex", js_request_get, NULL, REQ_DUPLEX),
    JS_CGETSET_MAGIC_DEF("signal", js_request_get, NULL, REQ_SIGNAL),
};

/* THE ONE MEMBER OF `Request` THIS USER AGENT MUST NOT HAVE — LOCAL NETWORK ACCESS §3.1.2 Fetch API's
   `partial interface Request`. The argument is at the REQUEST_INIT row that declares the same partial's
   DICTIONARY half, because that is where a reader asks; what is here is the assertion. */
static const char *const REQUEST_ABSENT[] = { "targetAddressSpace" };

void request_init(JSContext *ctx)
{
    JSClassDef def = { "Request", .finalizer = request_finalizer, .gc_mark = request_gc_mark };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* `constructor(RequestInfo input, optional RequestInit init = {})`. REQUEST_INIT is at file scope because
       §5.4 step 13's emptiness test reads the same list; see its declaration. */
    static const IdlArgType CTOR_ARGS[2] = { IDL_ANY, IDL_DICT };   /* RequestInfo: the machine resolves it */

    DCHECK(g_request_rt == NULL || g_request_rt == rt,
           "Request was installed into a second runtime — its class id and step ids belong to the first, and "
           "one WASM instance is one document");
    if (g_request_rt == rt)
        return;
    g_request_rt = rt;
    JS_NewClassID(rt, &g_request_class);
    JS_NewClass(rt, g_request_class, &def);
    /* NO SOURCE: a Request's body is bytes the PAGE composed and handed to fetch(), never a byte sequence a
       server filled — see core/byte_reader.h for why NULL here is a statement and not a hole. */
    g_request_body_handle = body_declare(ctx, g_request_class, request_body_of, request_body_mime, NULL,
                                         "Request");
    g_request_ctor_stepid = idl_method_id_step(ctx, CTOR_ARGS, 2, REQUEST_INIT, REQUEST_INIT_N,
                                               &js_request_ctor_decl, 0);
    idl_optional_from(1);   /* §5.4: `optional RequestInit init = {}` */
    idl_iface_brand(abort_signal_class());   /* RequestInit's one interface-typed member */
    realm_declare_intrinsic(request_install_proto);
}

/* §5.4's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM — `url` resolves against the READING realm's API base URL,
   so a shared one answers a child document's `new Request("api/x").url` with the parent's address. */
void request_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_request_class != 0, "a realm asked for Request.prototype before the class was declared");
    prev = JS_GetClassProto(ctx, g_request_class);
    DCHECK(JS_IsNull(prev), "request_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Request.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Request");
    JS_SetPropertyFunctionList(ctx, proto, js_request_proto_funcs,
                               (int)(sizeof(js_request_proto_funcs) / sizeof(js_request_proto_funcs[0])));
    body_install(ctx, proto, g_request_body_handle);
    idl_members_excluded(ctx, proto, "Request", REQUEST_ABSENT,
                         (int)(sizeof(REQUEST_ABSENT) / sizeof(REQUEST_ABSENT[0])),
                         "LOCAL NETWORK ACCESS §3.1.1 Fetching states \"Request objects are given a new target "
                         "IP address space property, initially null\" and LOCAL NETWORK ACCESS §3.1.2 Fetch "
                         "API states no getter steps over it, while the `IPAddressSpace` the attribute is "
                         "declared to return is not nullable and has no value naming that initial state — so "
                         "every answer a getter could give is invented, and \"public\" worst of all, since "
                         "LOCAL NETWORK ACCESS §3.1.1 Fetching's own check asserts \"request's target IP "
                         "address space is not public\"");
    JS_SetClassProto(ctx, g_request_class, proto);
}

void request_install(JSContext *ctx, JSValueConst global)
{
    JSValue ctor;
    DCHECK(g_request_ctor_stepid >= 0, "Request was installed before request_init declared its constructor");
    ctor = idl_step_constructor(ctx, "Request", g_request_ctor_stepid);
    CHECK(!JS_IsException(ctor), "the Request interface object could not be allocated");
    {
        JSValue proto = JS_GetClassProto(ctx, g_request_class);
        DCHECK(!JS_IsNull(proto), "Request was installed into a realm that never ran its proto build");
        JS_SetConstructor(ctx, ctor, proto);
        JS_FreeValue(ctx, proto);
    }
    JS_SetPropertyStr(ctx, (JSValue)global, "Request", ctor);
}

void request_free(JSContext *ctx)
{
    if (!g_request_rt)
        return;
    /* the prototypes are the REALMS' — released with their contexts */
    g_request_rt = NULL;
    g_request_ctor_stepid = -1;
}
