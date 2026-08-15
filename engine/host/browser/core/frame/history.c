/* THE History INTERFACE — HTML §7.2.5, the scriptable face of core/frame/session_history.c.
 *
 * THIS IS THE MEMBER EVERY CLIENT-SIDE ROUTER IS BUILT ON. React Router, Vue Router, Angular's Router and every
 * hand-rolled one change route by calling `history.pushState()`; with `history` absent a routing bundle threw on
 * its first navigation, so every route it could reach, every lazily-loaded chunk behind a route and every
 * endpoint those chunks call went unexplored. The interface is thin — the state machine is next door — but it is
 * the door the whole of that surface comes through.
 *
 * pushState AND replaceState ARE ONE ALGORITHM WITH A MODE. §7.2.5 says so in as many words: both "run the
 * shared history push/replace state steps given this, data, url, and" the one word that differs. So they are one
 * body with a magic, and the four things that body does — serialize, parse, refuse, update — happen in exactly
 * the order the standard lists them, because the order is observable: a value §2.7 refuses is a DataCloneError
 * even when the URL is also cross-origin, since the serialization is step 3 and the URL check is step 5.
 *
 * NEITHER OF THEM FETCHES. The steps they end in are §7.4.4's URL AND HISTORY UPDATE STEPS — the standard's
 * NON-FRAGMENT SYNCHRONOUS "navigation" section — which change the Document's address and its session history
 * and load nothing. That is also why they fire no event: the standard's own note records that "popstate events
 * fire for fragment navigations, but not for history.pushState() calls".
 *
 * `go`, `back` AND `forward` ARE HONESTLY ABSENT, and the IDL audit names them. All three are "delta traverse
 * this given <n>", which ends in §7.4.3's TRAVERSE THE HISTORY BY A DELTA and then §7.4.6's APPLY THE HISTORY
 * STEP — an algorithm that deactivates the displayed document, may populate a target entry from the network,
 * and fires `pagehide`/`pageswap`/`popstate`/`hashchange`/`pageshow` at the page's own listeners. This build has
 * neither PopStateEvent nor HashChangeEvent (core/events/create_event.c's table names both as interfaces
 * `createEvent` would build and nothing constructs), so the events those steps fire do not exist to be fired.
 * A `go()` that moved the current step without firing them would be the worst of the three states: a page that
 * believes it navigated, a router that never re-renders, and nothing anywhere to say so. So the members are
 * not installed, and history_install_realm asserts against the day PopStateEvent arrives — which is the two-
 * sided form, so the absence cannot outlive the thing it describes. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/frame/history.h"
#include "core/frame/session_history.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/url/url.h"

static JSClassID g_history_class;
static int g_obj_slot = -1;
static int g_id_push = -1, g_id_replace = -1, g_id_scroll_setter = -1;

/* §7.2.5's `enum ScrollRestoration { "auto", "manual" }` — the TYPE of the `scrollRestoration` attribute, so
   the list is what the declaration carries and not something the setter's body re-states. */
static const char *const SCROLL_RESTORATION[] = { "auto", "manual", NULL };

/* THE READ-ONLY MEMBERS, in the order §7.2.5's IDL declares them — the getter's magic is an index into this. */
typedef enum { HIST_LENGTH, HIST_SCROLL_RESTORATION, HIST_STATE, HIST_N } HistMember;
static const char *const HIST_NAME[] = { "length", "scrollRestoration", "state" };

/* §7.2.5's two mode words, as the magic pushState and replaceState are declared with. */
enum { HIST_PUSH, HIST_REPLACE };

/* WEB IDL §3.7.5's BRAND. The one History per realm WEARS the class, so the check is a class-id comparison a
   page cannot forge, and pulling a getter off the prototype and applying it to something else is the TypeError
   a browser answers with. */
static bool hist_brand(JSContext *ctx, JSValueConst this_val)
{
    DCHECK(g_history_class != 0, "a History member ran before history_init declared the class");
    if (JS_GetClassID(this_val) == g_history_class) return true;
    JS_ThrowTypeError(ctx, "a History member was reached on something that is not a History");
    return false;
}

/* THE HALF OF "this's relevant global object's associated Document" THIS ENGINE CAN ANSWER, asserted rather
   than assumed — the same shape and the same reason as core/frame/location.c's. A C member runs in the realm
   that DEFINED it, so a member pulled off THIS realm's History.prototype and applied to ANOTHER realm's History
   would read the wrong document's session history. Same-origin documents are one heap, so both objects are
   reachable from one flow and the mix-up is a reachable state rather than a hypothetical. */
static void hist_assert_this_realm(JSContext *ctx, JSValueConst this_val)
{
    JSValue own = realm_value_get(ctx, g_obj_slot);
    bool same = JS_VALUE_GET_PTR(own) == JS_VALUE_GET_PTR(this_val);

    JS_FreeValue(ctx, own);
    DCHECK(same, "a History member was reached on ONE realm's History through ANOTHER realm's function — every "
                 "member reads the session history of ITS OWN realm, so the answer would be a different "
                 "navigable's history wearing this document's History. BUILD the History that carries its own "
                 "realm: give the instance its realm as its class opaque (with the finalizer, gc_mark and "
                 "cow_capture_host_record contract that entails) so the member reads it off THIS");
}

/* EVERY MEMBER OF §7.2.5 OPENS WITH THE SAME TWO STEPS — the brand, then "if this's relevant global object's
   associated Document is not fully active, then throw a SecurityError DOMException". The second is what makes
   a detached iframe's `history` inert rather than a window onto the document that removed it, and it is a
   THROW rather than a DCHECK because a page reaches it deliberately: removing an iframe and then reading
   `frame.contentWindow.history.length` is a thing the corpus does on purpose. Returns false with the
   exception live. */
static bool hist_entry(JSContext *ctx, JSValueConst this_val)
{
    if (!hist_brand(ctx, this_val)) return false;
    hist_assert_this_realm(ctx, this_val);
    if (!document_fully_active(ctx)) {
        JS_ThrowDOMException(ctx, "SecurityError", "the History object's Document is not fully active");
        return false;
    }
    return true;
}

static JSValue js_hist_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    if (!hist_entry(ctx, this_val)) return JS_EXCEPTION;
    switch (magic) {
    /* "Return this's length" — the History object's own field, which §7.4.6.1's apply-the-history-step keeps
       equal to the number of overall session history entries of the traversable. */
    case HIST_LENGTH: return JS_NewUint32(ctx, session_history_length(ctx));
    /* "Return this's relevant global object's navigable's active session history entry's scroll restoration
       mode" — the ENTRY's, not the History's, which is why it is read through the entry every time: a page
       that pushes a new entry gets that entry's mode and not the one it set two entries ago. */
    case HIST_SCROLL_RESTORATION: return JS_NewString(ctx, session_history_scroll_restoration(ctx));
    /* "Return this's state" — the History's own field, written by §7.4.6.2's restore-the-history-object-state.
       It is a field rather than a deserialization per read because a page compares `history.state` against
       itself and against what it pushed. */
    case HIST_STATE: return session_history_state(ctx);
    default:
        DFAIL("a History member was read with a magic no member of this file declares — the magic IS the "
              "member index, so an unknown one means a name was installed without a case to answer it");
        return JS_UNDEFINED;
    }
}

/* §7.2.5's scrollRestoration SETTER: "set this's relevant global object's navigable's active session history
   entry's scroll restoration mode to the given value". The value has already been checked against the
   ScrollRestoration enumeration by the declared IDL_ENUM type, so an invalid one threw a TypeError before this
   body was entered — which is what makes `history.scrollRestoration = "bogus"` a TypeError and not a silent
   no-op. A router setting it to "manual" is the ordinary use. */
static JSValue js_hist_set_scroll_restoration(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *s;

    (void)magic;
    if (!hist_entry(ctx, this_val)) return JS_EXCEPTION;
    s = JS_ToCString(ctx, val);
    CHECK(s != NULL, "history: the ScrollRestoration value the IDL conversion produced could not be read — it "
                     "is one of two static strings by then, so a failure here is an allocation");
    session_history_set_scroll_restoration(ctx, s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* ---- §7.2.5's "a Document can have its URL rewritten to a URL targetURL" ------------------------------------
 *
 * Verbatim, and the shape of it matters: the first test is a conjunction over the ORIGIN-BEARING components
 * (scheme, username, password, host, port) and every later test loosens by scheme. `https://example.com/home`
 * may be rewritten to `https://example.com/shop`, may not be rewritten to `http://example.com/home`, and may
 * not be rewritten to `https://user:pass@example.com/home` — those three rows are the standard's own table.
 * That is what makes pushState same-origin-only, and it is why the refusal is a SecurityError. */
static bool url_host_equal(const UrlHost *a, const UrlHost *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case URL_HOST_DOMAIN:
    case URL_HOST_OPAQUE: return a->domain && b->domain && !strcmp(a->domain, b->domain);
    case URL_HOST_IPV4:   return a->ipv4 == b->ipv4;
    case URL_HOST_IPV6:   return memcmp(a->ipv6, b->ipv6, sizeof a->ipv6) == 0;
    case URL_HOST_NULL:
    case URL_HOST_EMPTY:  return true;
    }
    DFAIL("a URL host of a kind url.h does not declare reached §7.2.5's can-have-its-URL-rewritten");
    return false;
}

static bool str_equal_or_both_absent(const char *a, const char *b)
{
    if (!a || !b) return a == b || (!(a && *a) && !(b && *b));
    return !strcmp(a, b);
}

/* The PATH, compared as the serialized component — §7.2.5 compares "their path component", and a segment list
   and an opaque path are the two spellings one record has. Both malloc'd; both freed here. */
static bool url_path_equal(const UrlRecord *a, const UrlRecord *b)
{
    char *pa = url_serialize_path(a), *pb = url_serialize_path(b);
    bool same;

    CHECK(pa != NULL && pb != NULL, "history: a URL path could not be serialized");
    same = !strcmp(pa, pb);
    free(pa);
    free(pb);
    return same;
}

static bool document_can_have_url_rewritten(const UrlRecord *doc_url, const UrlRecord *target)
{
    /* STEP 2 — the origin-bearing components. */
    if (strcmp(doc_url->scheme, target->scheme) != 0) return false;
    if (!str_equal_or_both_absent(doc_url->username, target->username)) return false;
    if (!str_equal_or_both_absent(doc_url->password, target->password)) return false;
    if (!url_host_equal(&doc_url->host, &target->host)) return false;
    if (doc_url->port != target->port) return false;
    /* STEP 3: "if targetURL's scheme is an HTTP(S) scheme, then return true" — differences in path, query and
       fragment are allowed for http: and https:, which is the whole of what a router needs. */
    if (!strcmp(target->scheme, "http") || !strcmp(target->scheme, "https")) return true;
    /* STEP 4 — `file:` allows differences in query and fragment but not in path. */
    if (!strcmp(target->scheme, "file")) return url_path_equal(doc_url, target);
    /* STEP 5: "only differences in fragment are allowed for other types of URLs." */
    if (!url_path_equal(doc_url, target)) return false;
    return str_equal_or_both_absent(doc_url->query, target->query);
}

/* ---- §7.2.5's SHARED HISTORY PUSH/REPLACE STATE STEPS -------------------------------------------------------- */

static JSValue js_hist_push_replace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic)
{
    StructuredData serialized;
    UrlRecord doc_url, target;
    const char *url_arg = NULL;
    char *new_url = NULL;
    bool have_target = false;

    DCHECK(magic == HIST_PUSH || magic == HIST_REPLACE,
           "§7.2.5's shared push/replace state steps ran with a mode neither of its two callers declares");
    /* STEPS 1-2. */
    if (!hist_entry(ctx, this_val)) return JS_EXCEPTION;
    DCHECK(argc >= 2, "pushState/replaceState ran with fewer than its two required arguments — `any data` and "
                      "`DOMString unused` are both required, so §3.6.2's arity check answered that before this "
                      "body was entered");
    /* STEP 3: "let serializedData be StructuredSerializeForStorage(data). Rethrow any exceptions." IT IS FIRST,
       and that order is observable: `history.pushState(function(){}, "", "https://other/")` is a
       DataCloneError and not a SecurityError.
       §2.7's ForStorage variant differs from the plain one in exactly one refusal — a SharedArrayBuffer, whose
       [[ArrayBufferData]] is shared and which "cannot be serialized for storage". This engine has no
       SharedArrayBuffer at all (core/structured_clone.c states it: "a SharedArrayBuffer is a different class,
       so it is not transferable here"), so the two variants coincide and this IS the ForStorage one. */
    if (structured_serialize(ctx, argv[0], &serialized) < 0) return JS_EXCEPTION;

    /* STEP 4: "let newURL be document's URL." */
    url_record_init(&doc_url);
    url_record_init(&target);
    CHECK(url_parse(&doc_url, document_base_url(ctx), strlen(document_base_url(ctx)), NULL),
          "this realm's document address is not a URL — the host captured something this engine cannot make a "
          "principal out of");

    /* STEP 5 — "if url is not null OR THE EMPTY STRING". The empty-string half is historical and the standard
       says so: `history.pushState(null, "", "")` bypasses URL parsing entirely and keeps the document's
       address, where `location.href = ""` parses the empty string. An argument the page did not pass arrives
       as undefined and one it passed as null arrives as JS_NULL (the declared type is `USVString?`); both are
       the IDL null. */
    if (argc >= 3 && !JS_IsNull(argv[2]) && !JS_IsUndefined(argv[2])) {
        url_arg = JS_ToCString(ctx, argv[2]);
        CHECK(url_arg != NULL, "history: the USVString the IDL conversion produced could not be read");
    }
    if (url_arg && *url_arg) {
        /* "Set newURL to the result of ENCODING-PARSING A URL given url, relative to the relevant settings
           object of history" — whose API base URL is this document's address, already parsed above. */
        if (!url_parse(&target, url_arg, strlen(url_arg), &doc_url)) {
            JS_FreeCString(ctx, url_arg);
            structured_data_free(ctx, &serialized);
            url_record_free(&doc_url);
            url_record_free(&target);
            return JS_ThrowDOMException(ctx, "SecurityError", "the URL passed to %s could not be parsed",
                                        magic == HIST_PUSH ? "pushState" : "replaceState");
        }
        have_target = true;
        if (!document_can_have_url_rewritten(&doc_url, &target)) {
            JS_FreeCString(ctx, url_arg);
            structured_data_free(ctx, &serialized);
            url_record_free(&doc_url);
            url_record_free(&target);
            return JS_ThrowDOMException(ctx, "SecurityError",
                                        "this document cannot have its URL rewritten to the URL passed to %s",
                                        magic == HIST_PUSH ? "pushState" : "replaceState");
        }
    }
    if (url_arg) JS_FreeCString(ctx, url_arg);

    /* STEP 6: "if history's relevant global object's navigable's ALLOWED TO PERFORM A NAVIGATION OR HISTORY
       UPDATE returns blocked, then return." HTML §7.3.1 declares that algorithm IMPLEMENTATION-DEFINED, and
       gives the only example of what it is for: "this can return blocked if invoked too many times within a
       certain timespan". This user agent has no such throttle and must not acquire one — a rate cap on how
       often a bundle may route is a bound on the exploration, which CLAUDE.md's §NO BOUNDS forbids outright —
       so the algorithm returns ALLOWED for every navigable here. Evaluated at the step that asks it rather
       than dropped, exactly as core/html/autofocus.c evaluates the sandboxing flag it has no state for. */

    /* STEPS 7-9 are §7.2.7's NAVIGATION API: fire a push/replace/reload navigate event at `navigation` with
       classicHistoryAPIState set to serializedData, and return if it is canceled. That whole interface —
       `navigation`, NavigationHistoryEntry, NavigateEvent and its interception — is deliberately not built
       here (core/frame/session_history.h), and its absence is asserted where the interface would be: the
       realm_awaits probe in core/rendering/rendering.c. A page cannot observe the difference without it,
       because there is no `navigation` object to add a listener to. */

    /* STEP 10. */
    new_url = have_target ? url_serialize(&target, false) : url_serialize(&doc_url, false);
    CHECK(new_url != NULL, "history: the new address could not be serialized");
    session_history_url_and_history_update(ctx, new_url, &serialized, magic == HIST_PUSH);
    free(new_url);
    structured_data_free(ctx, &serialized);
    url_record_free(&doc_url);
    url_record_free(&target);
    return JS_UNDEFINED;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------- */

/* §7.2.5: "A Document has a history object, a History object", and §7.2.2's `[Replaceable] readonly attribute
   History history` on the Window. ONE per realm, built WITH the realm — the same §3.7 rule Location and Screen
   are built under, and the same reason: a member runs in the realm that defined it, so one shared History would
   answer every document's `length` out of whichever realm built it first. */
static JSValue js_win_history(JSContext *ctx, JSValueConst this_val, int magic)
{
    (void)this_val; (void)magic;
    return realm_value_get(ctx, g_obj_slot);
}

static void history_install_realm(JSContext *ctx)
{
    JSValue proto, prev, global, hist;
    int i;

    prev = JS_GetClassProto(ctx, g_history_class);
    DCHECK(JS_IsNull(prev), "history_install_realm ran twice in one realm — everything already holding the "
                            "first History would answer out of a discarded object");
    JS_FreeValue(ctx, prev);

    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "History.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "History");
    for (i = 0; i < HIST_N; i++)
        idl_install_accessor(ctx, proto, HIST_NAME[i], js_hist_get, i,
                             i == HIST_SCROLL_RESTORATION ? g_id_scroll_setter : -1);
    idl_install_method(ctx, proto, "pushState", 2, g_id_push);
    idl_install_method(ctx, proto, "replaceState", 2, g_id_replace);
    JS_SetClassProto(ctx, g_history_class, JS_DupValue(ctx, proto));

    global = JS_GetGlobalObject(ctx);
    /* §3.7.1's INTERFACE OBJECT. History declares no constructor, so `new History()` is a TypeError — and its
       PRESENCE is what `history instanceof History` and every prototype-patching router shim needs. */
    JS_SetPropertyStr(ctx, global, "History", idl_interface_object(ctx, "History", proto));

    hist = JS_NewObjectProtoClass(ctx, proto, g_history_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(hist), "the Document's associated History could not be allocated");
    /* §7.2.2's `[Replaceable] readonly attribute History history` — an accessor until a page assigns to it and
       a plain data property afterwards, which is what Replaceable means and what a plain data property could
       never be. */
    idl_install_replaceable(ctx, global, "history", js_win_history, 0);
    realm_value_set(ctx, g_obj_slot, hist);
    JS_FreeValue(ctx, global);
}

void history_init(JSContext *ctx)
{
    JSClassDef d = { "History" };

    DCHECK(g_obj_slot < 0, "history_init ran twice — the class, the slot and the member declarations are made "
                           "once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_history_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_history_class, &d) == 0,
          "History: the per-realm prototype slot could not be declared");
    g_obj_slot = realm_value_declare(ctx, "HTML §7.2.5 the Document's associated History");

    /* §7.2.5's `undefined pushState(any data, DOMString unused, optional USVString? url = null)`, declared
       twice with the two modes its one algorithm takes. `unused` is a real DOMString conversion — the page's
       `toString` runs, and the machine parks on it — which is why the argument that "exists for historical
       reasons" is still declared rather than ignored. */
    {
        static const IdlArgType ARGS[] = { IDL_ANY, IDL_DOMSTRING, IDL_USVSTRING_NULLABLE };

        g_id_push = idl_method_id(ctx, ARGS, 3, js_hist_push_replace, HIST_PUSH);
        idl_optional_from(2);
        g_id_replace = idl_method_id(ctx, ARGS, 3, js_hist_push_replace, HIST_REPLACE);
        idl_optional_from(2);
    }
    /* §7.2.5's `attribute ScrollRestoration scrollRestoration`. The enumeration's value list IS the type, so it
       is declared here beside the member and the setter's body never sees an invalid value. */
    g_id_scroll_setter = idl_setter_id(ctx, IDL_ENUM, false, js_hist_set_scroll_restoration, 0);
    idl_enum_values(SCROLL_RESTORATION);

    realm_declare_intrinsic(history_install_realm);
}

void history_free(void)
{
    /* The prototypes, the interface objects and the History objects are the REALMS' — each is released with
       its context. What the agent holds is the slot, and a slot id is a class id in a runtime that is going
       away with it. */
    g_obj_slot = -1;
}
