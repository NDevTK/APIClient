/* COOKIE STORE API §3 The CookieStore interface — §3.1's get and §3.2's getAll over §7.1 "Query cookies".
 * See cookie_store.h for which standard this is, where it moved to, and what this component deliberately is not.
 *
 * WHY THE QUERY HALF LANDS ALONE, AND IT IS A SUBPROBLEM ORDER RATHER THAN A CONVENIENCE. §7.2 "Set a cookie"
 * step 12.3 refuses a Domain that "is not a registrable domain suffix of and is not equal to host", and that
 * predicate is HTML's `is a registrable domain suffix of or is equal to`, which is decided against the PUBLIC
 * SUFFIX LIST. cookie_jar.c's own header states this engine is not given that data — it is why RFC 6265 §5.3
 * step 5's public-suffix rejection is a step whose condition is false here. So `set` and `delete` (which §7.3
 * defines AS a `set`) stand on an unbuilt component, and §7.1 does not: it reads RFC 6265 §5.4, which the jar
 * implements whole. Building the write half first would have meant either a silent wrong answer for every
 * `Domain` a page passes, or a hand-rolled suffix guess — and a guess about which registrable domain a host
 * belongs to is a security answer, not a heuristic.
 *
 * NOTHING HERE SUSPENDS, AND THAT IS WHY NONE OF IT IS A STEP MACHINE. §3.1 step 6 runs its work "in parallel"
 * and settles a promise; in this engine the work is a walk of an in-memory jar, so it completes within the call
 * and the promise is settled before the member returns. The page cannot observe the difference — a promise
 * settled during the call still delivers its reaction as a JOB, which is a first-class flow on the one WFQ —
 * and a step machine would be a continuation-holding builtin with no continuation to hold. What DOES run the
 * page's code is the argument conversion, and that is the declaration's work rather than this body's: a
 * dictionary member's getter runs as a rest point of §3.2.17's walk, before any line below executes. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/cookie_store/cookie_store.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "core/loader/cookie_jar.h"
#include "core/realm.h"
#include "core/url/origin.h"
#include "core/url/url.h"
#include "solver/concolic.h"

#define CS_COMPONENT "cookie_store"
/* THE SOURCE IDENTITY, and it is the member's own name rather than `document.cookie`'s because a candidate run
   substitutes BY identity: a PoC that fires through `cookieStore.get(...).value` is delivered and reproduced
   through this member, and one that fires through `document.cookie` is not. See the declaration in
   cookie_store_init for the character set, which is the SAME RFC 6265 fact stated in a second place. */
#define CS_SOURCE "cookieStore"

static JSClassID g_cs_class;
static int       g_obj_slot = -1;
static int       g_id_get = -1;
static int       g_id_get_all = -1;

/* Which member this invocation is — §3.1 and §3.2 differ in three steps and share every other, so they are one
   body and one magic rather than two copies of §7.1's caller. */
#define CS_GET     0
#define CS_GET_ALL 1

/* §3's `dictionary CookieStoreGetOptions { USVString name; USVString url; };` — two OPTIONAL members with no
   default, which is what §3.1 step 5's "If options is empty" then reads: an absent member is absent, so a
   dictionary with neither present is the empty one and a `get()` with no argument is a rejection rather than a
   query for everything. Declared in Web IDL §3.2.17's read order. */
static const IdlDictMember COOKIE_STORE_GET_OPTIONS[] = {
    { "name", IDL_USVSTRING },
    { "url",  IDL_USVSTRING },
};

/* WEB IDL §3.7.7's brand check, and it THROWS rather than asserting. A receiver is PAGE-SUPPLIED INPUT — the
   page writes `CookieStore.prototype.get.call(null)` — so an assert here would hand any document an abort
   switch over this engine, which is the one thing §Offensive-programming's "assert only what you computed"
   forbids. §3.7.7 wraps the brand check in its Try, and this member declares idl_returns_promise, so the
   TypeError becomes a REJECTED promise exactly as that section requires. */
static bool cs_brand(JSContext *ctx, JSValueConst this_val)
{
    if (JS_GetClassID(this_val) == g_cs_class)
        return true;
    JS_ThrowTypeError(ctx, "CookieStore.prototype method called on a receiver that is not a CookieStore");
    return false;
}

/* §3.1 step 4's `settings's creation URL`, as the REQUEST-URI RFC 6265 §5.4 is computed against. It is this
   realm's document address: `cookieStore` is a per-realm object, so `this`'s relevant settings object is the
   settings object of the realm this member was installed on, and a child navigable's store answers for the
   child. Returns false for a document with no usable address; `*rec` is initialised either way and the caller
   ALWAYS frees it. */
static bool cs_request_uri(JSContext *ctx, UrlRecord *rec)
{
    const char *url = document_url(ctx);

    url_record_init(rec);
    if (!url || !*url)
        return false;
    CHECK(url_parse(rec, url, strlen(url), NULL),
          "a document's own address is not a URL — the host captured something this engine cannot make a "
          "principal out of");
    /* RFC 6265's store is reached for http(s) alone, which is the same condition HTML §3.1.4's cookie-averse
       test applies to `document.cookie` and the same one cookie_jar.c's request-host assert relies on. */
    return rec->scheme && (strcmp(rec->scheme, "http") == 0 || strcmp(rec->scheme, "https") == 0);
}

/* §7.1's `create a CookieListItem` — steps 1 to 3, over one « name, value » pair of the jar's cookie-list.
 *
 * IT RETURNS EXACTLY TWO MEMBERS BECAUSE THE DICTIONARY HAS EXACTLY TWO. §7.1's step 3 is «[ "name" → name,
 * "value" → value ]» and §3's IDL is `dictionary CookieListItem { USVString name; USVString value; };`. The
 * `domain`, `path`, `expires`, `secure` and `sameSite` of earlier drafts are GONE, and the standard's own Note
 * beside that step says so: "One implementation is known to expose information beyond _name_ and _value_." A
 * fifth field added here would be this engine becoming that implementation.
 *
 * THE VALUE IS THE SOURCE AND THE NAME IS NOT, which is a claim about which of the two an attacker writes.
 * A cookie's VALUE is attacker-controlled — RFC 6265 gives a sibling subdomain a write to this jar, and a
 * planted cookie is read by a later load — so it is minted through the one seam solver/concolic.h names and
 * the auth gate a bundle writes over it FORKS. The NAME is not left concrete for want of rigour: for `get(n)`
 * and `getAll(n)` §7.1 step 3.2.3 has already CONTINUED past every cookie whose name is not `n`, so the name
 * on every surviving item is a value this run compared and pinned — wrapping it would make `item.name === n`
 * fork a world in which a cookie the filter just matched does not match it. For the unfiltered `getAll()` the
 * name is a jar key this component enumerated out of the store's own property order. Both are values this
 * codebase computed, which is the line §Offensive-programming draws. */
static JSValue cs_item(JSContext *ctx, JSValueConst pair)
{
    JSValue item = JS_NewObject(ctx);
    JSValue name, value;

    CHECK(!JS_IsException(item), "OOM building a CookieListItem");
    name = JS_GetPropertyUint32(ctx, pair, 0);
    value = JS_GetPropertyUint32(ctx, pair, 1);
    CHECK(!JS_IsException(name) && !JS_IsException(value),
          "a cookie-list entry did not carry the two strings cookie_jar_cookie_list puts on it");
    value = concolic_source_wrap(ctx, "{cookieStore}", CS_SOURCE, value);
    CHECK(JS_SetPropertyStr(ctx, item, "name", name) >= 0 &&
          JS_SetPropertyStr(ctx, item, "value", value) >= 0,
          "a CookieListItem refused its own name and value");
    return item;
}

/* §3.1 get and §3.2 getAll, whose only differences are step 5's empty-options rejection and what they settle
 * with. Both reach §7.1 "Query cookies", whose step 1 is RFC 6265 §5.4 with the cookie-string discarded — which
 * is why the jar hands back the intermediate cookie-list and this body never sees a cookie-string at all. */
static JSValue js_cs_query(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    bool by_string;
    JSValue opt_name = JS_UNDEFINED, opt_url = JS_UNDEFINED;
    JSValue list = JS_UNDEFINED, out = JS_UNDEFINED;
    UrlRecord uri;
    const char *want = NULL;
    size_t want_len = 0;
    uint32_t n = 0, i, kept = 0;

    if (!cs_brand(ctx, this_val))
        return JS_EXCEPTION;
    /* IDL_USVSTRING_OR_DICT places a string for Web IDL §3.6 step 12.15's entry and an engine-built dictionary
       for step 12.11's, and places UNKNOWN EXTERNAL INPUT as itself on the forked USVString world — so a
       concolic here is the string arm, which is the same test core/frame/window_message.c makes and states. */
    by_string = JS_IsString(arg) || concolic_is(arg);
    DCHECK(by_string || JS_IsObject(arg) || JS_IsUndefined(arg),
           "CookieStore's get/getAll reached its body with an argument that is neither a string, an object nor "
           "absent — IDL_USVSTRING_OR_DICT places one of those three, and an OMITTED argument is the `= {}` "
           "dictionary rather than an absent one");
    if (!by_string && !JS_IsUndefined(arg)) {
        opt_name = idl_dict_get(ctx, arg, "name");
        opt_url = idl_dict_get(ctx, arg, "url");
    }
    /* §3.1's get(options) STEP 5, which §3.2's getAll(options) does NOT have: "If options is empty, then return
       a promise rejected with a TypeError." So `cookieStore.get()` rejects and `cookieStore.getAll()` returns
       every cookie — two members that look alike and differ here, stated once. */
    if (magic == CS_GET && !by_string && JS_IsUndefined(opt_name) && JS_IsUndefined(opt_url)) {
        JS_FreeValue(ctx, opt_name);
        JS_FreeValue(ctx, opt_url);
        return JS_ThrowTypeError(ctx, "cookieStore.get() needs a name or a url — Cookie Store API §3.1 rejects "
                                      "an empty CookieStoreGetOptions rather than querying for everything");
    }
    /* §3.1 STEPS 1-4 / §3.2 STEPS 1-4: the relevant settings object's origin and creation URL. The opaque test
       is asked of the AGENT's principal, which is what an instance is keyed by — the same question and the same
       answer `document.cookie`'s own SecurityError is decided by. */
    if (origin_is_opaque(window_proxy_origin(document_window_proxy(ctx)))) {
        JS_FreeValue(ctx, opt_name);
        JS_FreeValue(ctx, opt_url);
        return JS_ThrowDOMException(ctx, "SecurityError",
                                    "a document with an opaque origin has no cookies");
    }
    if (!cs_request_uri(ctx, &uri)) {
        /* A cookie-averse document has no cookie store to query. §7.1 step 1 computes a cookie-string for a
           request-uri, and there is none — the list is empty, which is what a document with no cookies has. */
        url_record_free(&uri);
        JS_FreeValue(ctx, opt_name);
        JS_FreeValue(ctx, opt_url);
        list = JS_NewArray(ctx);
        CHECK(!JS_IsException(list), "OOM building an empty cookie-list");
        return magic == CS_GET ? (JS_FreeValue(ctx, list), JS_NULL) : list;
    }
    /* §3.1 STEP 6 / §3.2 STEP 5: `options["url"]`, which in a Window may only ever name THIS document. The
       standard allows a ServiceWorker to query another URL and this engine has none, so both of that step's
       rejections reduce to one comparison against the creation URL with fragments excluded. */
    if (!JS_IsUndefined(opt_url)) {
        const char *given = JS_ToCString(ctx, opt_url);
        char *mine = url_serialize(&uri, /*exclude_fragment*/ true);
        UrlRecord parsed;
        char *theirs = NULL;
        bool ok;

        const char *base_str = document_base_url(ctx);
        UrlRecord base;
        bool have_base;

        url_record_init(&parsed);
        url_record_init(&base);
        CHECK(mine != NULL, "OOM serializing this document's creation URL for §3.1 step 6.2");
        CHECK(given != NULL, "OOM reading the url member of a CookieStoreGetOptions");
        /* STEP 6.1: "parsing options["url"] with settings's API BASE URL" — which is the document's base URL
           and NOT its creation URL, so a `<base href>` moves what a relative `url` option resolves against
           exactly as it moves every other API-base-relative parse this platform makes. */
        have_base = base_str && *base_str && url_parse(&base, base_str, strlen(base_str), NULL);
        ok = url_parse(&parsed, given, strlen(given), have_base ? &base : NULL);
        if (ok)
            theirs = url_serialize(&parsed, /*exclude_fragment*/ true);
        url_record_free(&base);
        /* STEPS 6.2 AND 6.3 collapse here: equality with fragments excluded implies same origin, so the
           stronger test is the one asked and the weaker one cannot then fail. */
        ok = ok && theirs && strcmp(mine, theirs) == 0;
        free(theirs);
        free(mine);
        JS_FreeCString(ctx, given);
        url_record_free(&parsed);
        if (!ok) {
            url_record_free(&uri);
            JS_FreeValue(ctx, opt_name);
            JS_FreeValue(ctx, opt_url);
            return JS_ThrowTypeError(ctx, "cookieStore's url option must name this document's own URL — Cookie "
                                          "Store API §3.1 step 6.2 rejects any other, and a Window has no "
                                          "second URL it may query");
        }
    }
    JS_FreeValue(ctx, opt_url);

    /* §7.1 STEP 1: RFC 6265 §5.4 with the cookie-string discarded and its intermediate cookie-list kept. */
    list = cookie_jar_cookie_list(ctx, &uri);
    url_record_free(&uri);
    CHECK(!JS_IsException(list), "§7.1 step 1's cookie-list could not be computed");
    /* §7.1 step 3.2's name filter — `name` is the string argument, or the dictionary's member, or absent. */
    if (by_string) {
        want = JS_ToCStringLen(ctx, &want_len, arg);
        CHECK(want != NULL, "OOM reading the name argument of a CookieStore query");
    } else if (!JS_IsUndefined(opt_name)) {
        want = JS_ToCStringLen(ctx, &want_len, opt_name);
        CHECK(want != NULL, "OOM reading the name member of a CookieStoreGetOptions");
    }
    CHECK(JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, list, "length")) == 0,
          "the cookie-list did not carry its own length");

    out = JS_NewArray(ctx);
    CHECK(!JS_IsException(out), "OOM building §7.1 step 2's list");
    /* §7.1 STEP 3, over the cookie-list in §5.4 step 2's order. */
    for (i = 0; i < n; i++) {
        JSValue pair = JS_GetPropertyUint32(ctx, list, i);
        JSValue item;

        CHECK(!JS_IsException(pair), "a cookie-list entry could not be read back");
        if (want) {
            /* STEPS 3.2.2 AND 3.2.3: the stored name, UTF-8 decoded, compared with the requested one. */
            JSValue nm = JS_GetPropertyUint32(ctx, pair, 0);
            size_t nlen = 0;
            const char *have = JS_ToCStringLen(ctx, &nlen, nm);
            bool same;

            CHECK(have != NULL, "a cookie-list entry's name could not be read as a string");
            same = nlen == want_len && memcmp(have, want, want_len) == 0;
            JS_FreeCString(ctx, have);
            JS_FreeValue(ctx, nm);
            if (!same) { JS_FreeValue(ctx, pair); continue; }
        }
        item = cs_item(ctx, pair);           /* STEP 3.3 */
        JS_FreeValue(ctx, pair);
        CHECK(JS_DefinePropertyValueUint32(ctx, out, kept, item, JS_PROP_C_W_E) >= 0,
              "§7.1 step 3.4 could not append a CookieListItem");
        kept++;
        /* §3.1 settles with the FIRST item, so `get` stops at one — which is not an optimisation but the
           difference between minting one attacker source and minting one per cookie in the jar. */
        if (magic == CS_GET)
            break;
    }
    if (want)
        JS_FreeCString(ctx, want);
    JS_FreeValue(ctx, opt_name);
    JS_FreeValue(ctx, list);

    if (magic == CS_GET_ALL)
        return out;                          /* §3.2 step 6.3: resolve with the list */
    /* §3.1 steps 6.3 and 6.4: null for an empty list, the first item otherwise. */
    if (!kept) {
        JS_FreeValue(ctx, out);
        return JS_NULL;
    }
    {
        JSValue first = JS_GetPropertyUint32(ctx, out, 0);
        JS_FreeValue(ctx, out);
        return first;
    }
}

/* §6.1 The Window interface's `[SameObject] readonly attribute CookieStore cookieStore` — THIS realm's. */
static JSValue cs_get_cookie_store(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val;
    (void)magic;
    return realm_value_get(ctx, g_obj_slot);
}

static void cs_install_realm(JSContext *ctx)
{
    /* §3's `interface CookieStore : EventTarget`, so the prototype is CREATED over this realm's
       EventTarget.prototype rather than re-parented after the fact — see core/events/event_target.h. */
    JSValue proto = event_target_derived_proto(ctx);
    JSValue global, obj;

    CHECK(!JS_IsException(proto), "this realm's CookieStore.prototype could not be allocated");
    /* §3.7.3's @@toStringTag, which also ASSERTS §3.7.3's proto step against browser/idl_inheritance.h — that
       table already carries { "CookieStore", "EventTarget", IDL_PROTO_INHERITS }, so this is what checks that
       the prototype above really was built over this realm's EventTarget.prototype rather than merely named as
       though it had been. It is also what engine/idl_installed.mjs reads to decide which INTERFACE this file's
       installs belong to: without it the audit sees two installed members it cannot attribute and reports them
       as undecided rather than crediting them to CookieStore. */
    idl_interface_tag(ctx, proto, "CookieStore");
    /* §3's interface and its Window member are both `[SecureContext]`, so Web IDL §3.3.13 REMOVES them in a
       non-secure realm rather than making them throw: `window.cookieStore ? … : document.cookie` takes the
       fallback there, which is the branch a bundle writes it to take, and `"cookieStore" in window` is false.
       §3.3 set and §3.4 delete are ABSENT here and that is this component's stated narrowing, not an exposure
       decision — see the file header and the residual below. */
    idl_install_method_exposed(ctx, proto, "get", g_id_get, IDL_SECURE_CONTEXT);
    idl_install_method_exposed(ctx, proto, "getAll", g_id_get_all, IDL_SECURE_CONTEXT);
    JS_SetClassProto(ctx, g_cs_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    idl_install_interface_object_exposed(ctx, global, "CookieStore", proto, IDL_SECURE_CONTEXT);

    /* BUILT WITH THE REALM AND NOT ON FIRST READ, which is core/realm.h's rule and matters here for the reason
       it matters everywhere: an object minted inside whichever FLOW happened to read first would be that flow's
       private creation, so a sibling arm would get a different `cookieStore` and `===` would answer false where
       `[SameObject]` requires true. */
    obj = JS_NewObjectProtoClass(ctx, proto, g_cs_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "this realm's CookieStore could not be allocated");
    realm_value_set(ctx, g_obj_slot, obj);

    idl_install_accessor_exposed(ctx, global, "cookieStore", cs_get_cookie_store, 0, -1, IDL_SECURE_CONTEXT);
    JS_FreeValue(ctx, global);
}

void cookie_store_init(JSContext *ctx)
{
    /* §3.1's `get(USVString name)` and `get(optional CookieStoreGetOptions options = {})` — Web IDL §3.6's
       effective overload set has an entry of length 0 and two of length 1, so at the one arity both stand and
       the distinguishing argument decides: IDL_USVSTRING_OR_DICT is that decision stated as a type. */
    static const IdlArgType QUERY_ARGS[1] = { IDL_USVSTRING_OR_DICT };
    const int NOPT = (int)(sizeof(COOKIE_STORE_GET_OPTIONS) / sizeof(COOKIE_STORE_GET_OPTIONS[0]));
    JSClassDef d = { "CookieStore" };

    DCHECK(g_obj_slot < 0, "cookie_store_init ran twice — the class and the slot are declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_cs_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_cs_class, &d) == 0,
          "CookieStore: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "Cookie Store API §6.1 this realm's CookieStore object");

    g_id_get = idl_method_id_dict(ctx, QUERY_ARGS, 1, COOKIE_STORE_GET_OPTIONS, NOPT, js_cs_query, CS_GET);
    idl_optional_from(0);
    idl_returns_promise();
    g_id_get_all = idl_method_id_dict(ctx, QUERY_ARGS, 1, COOKIE_STORE_GET_OPTIONS, NOPT, js_cs_query,
                                      CS_GET_ALL);
    idl_optional_from(0);
    idl_returns_promise();

    /* THE ATTACKER SOURCE, with the constraint that makes a PoC through it reproduce. The excluded set is RFC
       6265 §4.1.1's cookie-value production — a value cannot carry whitespace, a double quote, a comma, a
       semicolon or a backslash — and the delivery is a PLANT because a cookie is not carried by the victim's
       load: it has to be in the jar BEFORE it.
       IT IS THE SAME RFC FACT core/dom/document_metadata.c STATES FOR `document.cookie`, and the two are two
       statements of one thing; see the residual below. */
    concolic_declare_source(CS_COMPONENT, CS_SOURCE, " \",;\\", 0, SRC_DELIVER_PLANT);

    agent_state_id(CS_COMPONENT, &g_obj_slot, "§6.1's per-realm CookieStore slot, and the declaration latch");
    agent_state_id(CS_COMPONENT, &g_id_get, "Cookie Store API §3.1's get");
    agent_state_id(CS_COMPONENT, &g_id_get_all, "Cookie Store API §3.2's getAll");
    agent_state_class(CS_COMPONENT, &g_cs_class, "Cookie Store API §3 CookieStore's per-realm slot and brand");
    realm_declare_intrinsic(cs_install_realm);
}

void cookie_store_free(void)
{
    concolic_undeclare_sources(CS_COMPONENT);
    g_obj_slot = -1;
    g_id_get = -1;
    g_id_get_all = -1;
}
