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
 * and load nothing. That is also why they fire no `popstate`: the standard's own note records that "popstate
 * events fire for fragment navigations, but not for history.pushState() calls".
 *
 * BUT THEY DO RUN THE PAGE'S CODE, AND SO THEY ARE STEP MACHINES. §7.4.4 step 11 updates the NAVIGATION API
 * entries, which fires `currententrychange` at `navigation` and then `dispose` at every entry a push threw off
 * the forward history — so a `pushState` after a `back()` runs a page's own listeners in the middle of itself,
 * and the member suspends there like every other member that dispatches. It was a plain C body for exactly as
 * long as §7.2.6 was absent.
 *
 * `go`, `back` AND `forward` ARE ONE ALGORITHM TOO, and §7.2.5 says so as plainly as it does for the other pair:
 * all three "are to DELTA TRAVERSE this given" a number, and the numbers are the argument, −1 and +1. So they are
 * one body with a magic, and the body is §7.2.5's delta-traverse steps in the standard's own order — because the
 * order is observable: a detached iframe's `history.back()` is a SecurityError and not a silent no-op, since the
 * fully-active check is step 2 and the traversal is step 5.
 *
 * WHAT THEY END IN IS NOT HERE. §7.4.3's TRAVERSE THE HISTORY BY A DELTA appends session history traversal steps
 * to the traversable, and §7.4.6's APPLY THE HISTORY STEP then fires `popstate` and queues `hashchange` at the
 * page's own listeners — which is the page's code, so it is a JOB that suspends and a step machine that resumes,
 * both in core/frame/session_history.c. This interface schedules it and returns, which is what `undefined
 * go(optional long delta = 0)` means: a page that calls `back()` and reads `location.href` on the next line
 * still reads the address it was at.
 *
 * `go(0)` IS NOT A TRAVERSAL AT ALL — "if delta is 0, then RELOAD document's node navigable, and return" — and
 * reloading is a capability this build does not have (every Location member's setter, `assign`, `replace` and
 * `reload` end in Location-object navigate, which is likewise absent). It crashes at the step that needs it,
 * naming it, rather than doing nothing and letting a page believe it reloaded. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/frame/history.h"
#include "core/frame/navigate_event_fire.h"
#include "core/frame/session_history.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/url/url.h"

static JSClassID g_history_class;
static int g_obj_slot = -1;
static int g_id_push = -1, g_id_replace = -1, g_id_scroll_setter = -1;
static int g_id_go = -1, g_id_back = -1, g_id_forward = -1;

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
 * That is what makes pushState same-origin-only, and it is why the refusal is a SecurityError.
 *
 * ITS HOST COMPARISON IS url.c's — this file held a copy, origin.c held a second, and a third question was
 * about to ask the same thing; "identical hosts" is ONE rule of §4.2 and it now has one implementation. */
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

bool history_document_can_have_url_rewritten(JSContext *ctx, const char *target_url)
{
    UrlRecord doc_url, target;
    bool ok;

    DCHECK(target_url != NULL && *target_url,
           "§7.2.5's can-have-its-URL-rewritten was asked about no URL — both of its callers hold a serialized "
           "absolute URL by the time they ask");
    url_record_init(&doc_url);
    url_record_init(&target);
    CHECK(url_parse(&doc_url, document_base_url(ctx), strlen(document_base_url(ctx)), NULL),
          "this realm's document address is not a URL — the host captured something this engine cannot make a "
          "principal out of");
    CHECK(url_parse(&target, target_url, strlen(target_url), NULL),
          "§7.2.5's can-have-its-URL-rewritten was given a target that does not parse — the caller serialized "
          "it out of a URL record, so a failure here is a serialization this engine cannot read back");
    ok = document_can_have_url_rewritten(&doc_url, &target);
    url_record_free(&doc_url);
    url_record_free(&target);
    return ok;
}

/* ---- §7.2.5's SHARED HISTORY PUSH/REPLACE STATE STEPS --------------------------------------------------------
 *
 * THREE STAGES, AND THE MIDDLE ONE IS THE PAGE'S CHANCE TO REFUSE THE NAVIGATION. Steps 7-9 fire a navigate
 * event at §7.2.6.2's Navigation and return when it answers false, so a router's `navigate` listener can stop a
 * `pushState` — and the dispatch runs that listener, which is why it is a rest point of its own between the
 * checks and §7.4.4. It was a `realm_awaits` naming exactly that stage for as long as §7.2.6.10.4 was absent.
 *
 * AND THE FIRST STAGE IS NOT A FORMALITY EITHER. This declared ONE — step 10 — on the ground that
 * everything before it is engine state (a serialization, a URL parse and two refusals) and that §7.4.4's own
 * split is what makes the tail parkable while the head is not. That reasoning is the one quickjs-step.h's
 * JSTrampStepDef::steps forbids: what makes a rest point necessary is the ENGINE — RAM pressure paging the
 * low-value tail to the cold tier, a cross-session resume, a flow that outranks this one — and none of those
 * ask whether the span in front of them runs the page's code. Step 3's StructuredSerializeForStorage walks a
 * graph the PAGE chose the size of, and step 5's encoding-parse walks a string it chose the length of.
 *   IT ALSO COST A REAL BUG, which is what quickjs-step.h's STEP_DISPATCH now makes unwritable. With one stage
 * declared, IDL_STEP_STAGE_BASE made that constant IDL_STEP_FIRST — the stage a member's body is ENTERED at —
 * so `if (hdr->stage == HPR_UPDATE) goto update;` fired on the first entry and jumped past the serialization,
 * the URL parse and both SecurityError refusals into a §7.4.4 work record nobody had begun. The member holds
 * only that work record, which is why the head had nothing of its own to leave behind and the jump was silent. */
#define HPR_STAGES(X)                                                                                     \
    X(HPR_CHECKS,   "HTML §7.2.5 shared history push/replace state steps 1-6 (the fully-active check, "    \
                    "StructuredSerializeForStorage(data), encoding-parsing url, "                          \
                    "can-have-its-URL-rewritten, and allowed-to-perform-a-navigation-or-history-update)")  \
    X(HPR_NAVIGATE, "HTML §7.2.5 shared history push/replace state steps 7-9 (fire a push/replace/reload " \
                    "navigate event at navigation with isSameDocument true, and return if it answers "     \
                    "false)")                                                                              \
    X(HPR_UPDATE,   "HTML §7.2.5 shared history push/replace state steps step 10 (run the URL and history "\
                    "update steps given document and newURL)")
enum { IDL_STEP_STAGE_BASE(HPR_STAGES) HPR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const HPR_STEPS[] = { HPR_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* WHAT THE MEMBER HOLDS ACROSS THE NAVIGATE EVENT, and why it holds it rather than recomputing it. Steps 3 and
   5 compute serializedData and newURL; step 10 uses both; and the navigate event fires BETWEEN them, running
   every `navigate` listener the page has. A listener can push its own entry, replace the address, or navigate
   away — so newURL re-derived after the dispatch would be a different URL, and the serialization would be of a
   value the page has since mutated. The two ride the machine's state as a string and an ArrayBuffer, which is
   what parks; §7.2.6.10.4's own work record takes its copy of both for exactly the same reason. */
typedef struct {
    SessionHistoryUrlUpdate w;
    NavigateEventFireWork   fire;
    JSValue                 new_url;   /* step 5's newURL, serialized (owned string) */
    JSValue                 classic;   /* step 3's serializedData, as the bytes (owned ArrayBuffer) */
} HprState;

static void hpr_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    HprState *s = st;

    session_history_url_update_visit(ctx, &s->w, v);
    navigate_event_fire_work_visit(ctx, &s->fire, v);
    v->val(ctx, &s->new_url);
    v->val(ctx, &s->classic);
}

/* THERE IS NO `release`. §7.4.4's record holds two ENTRIES and the navigation API's request buffer, and every
   one of them is a JSValue the visit above already names — so the teardown frees them through that ONE list,
   which is the whole reason IdlStepDecl states its ownership once. A `release` here would be the second list,
   and idl_args.c asserts across the call that a release freed nothing the declaration owns. */

static int js_hist_push_replace(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                                JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    HprState *s = state;
    StructuredData serialized;
    UrlRecord doc_url, target;
    const char *url_arg = NULL;
    char *new_url = NULL;
    bool have_target = false;
    int magic = idl_step_magic(hdr), r;

    STEP_DISPATCH(HPR_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(HPR_CHECKS);
    DCHECK(magic == HIST_PUSH || magic == HIST_REPLACE,
           "§7.2.5's shared push/replace state steps ran with a mode neither of its two callers declares");
    /* THE SLOTS ARE UNDEFINED BEFORE THEY ARE ANYTHING ELSE — a zeroed JSValue is the INTEGER 0, not undefined
       (JS_TAG_INT is 0), so a slot the visit walks before its stage has written it would hand the fork a real
       value the page can see. Every record this machine holds is started here, on the one entry that precedes
       all of them. */
    session_history_url_update_start(&s->w);
    navigate_event_fire_work_start(&s->fire);
    s->new_url = s->classic = JS_UNDEFINED;
    /* STEPS 1-2. */
    if (!hist_entry(ctx, hdr->this_val)) { JS_FreeValue(ctx, cb_result); return JS_STEP_ABRUPT; }
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
    if (structured_serialize(ctx, argv[0], &serialized) < 0) {
        JS_FreeValue(ctx, cb_result);
        return JS_STEP_ABRUPT;
    }

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
            JS_FreeValue(ctx, cb_result);
            JS_ThrowDOMException(ctx, "SecurityError", "the URL passed to %s could not be parsed",
                                 magic == HIST_PUSH ? "pushState" : "replaceState");
            return JS_STEP_ABRUPT;
        }
        have_target = true;
        if (!document_can_have_url_rewritten(&doc_url, &target)) {
            JS_FreeCString(ctx, url_arg);
            structured_data_free(ctx, &serialized);
            url_record_free(&doc_url);
            url_record_free(&target);
            JS_FreeValue(ctx, cb_result);
            JS_ThrowDOMException(ctx, "SecurityError",
                                 "this document cannot have its URL rewritten to the URL passed to %s",
                                 magic == HIST_PUSH ? "pushState" : "replaceState");
            return JS_STEP_ABRUPT;
        }
    }
    if (url_arg) JS_FreeCString(ctx, url_arg);

    /* STEP 6: "if history's relevant global object's navigable's ALLOWED TO PERFORM A NAVIGATION OR HISTORY
       UPDATE returns blocked, then return." HTML §7.3.1 declares that algorithm IMPLEMENTATION-DEFINED, and
       gives the only example of what it is for: "this can return blocked if invoked too many times within a
       certain timespan". This user agent has no such throttle and must not acquire one — a rate cap on how
       often a bundle may route is a bound on the exploration, which CLAUDE.md's §NO BOUNDS forbids outright —
       so the algorithm returns ALLOWED for every navigable here. Evaluated at the step that asks it rather
       than dropped — and unlike a MISSING state there is nothing here to build later: §7.3.1 declares the
       algorithm implementation-defined and ALLOWED is this implementation's answer. */

    /* newURL AND serializedData ARE TAKEN OUT OF THIS STAGE'S C LOCALS AND ONTO THE MACHINE, because the
       navigate event fires between them and step 10 and its listeners are the page's own code. A string and an
       ArrayBuffer are what park; the URL records and the serialization's bytes are freed here, where they were
       made. */
    new_url = have_target ? url_serialize(&target, false) : url_serialize(&doc_url, false);
    CHECK(new_url != NULL, "history: the new address could not be serialized");
    s->new_url = JS_NewString(ctx, new_url);
    CHECK(!JS_IsException(s->new_url), "history: the new address could not be held across the navigate event");
    s->classic = JS_NewArrayBufferCopy(ctx, serialized.buf, serialized.len);
    CHECK(!JS_IsException(s->classic),
          "history: the serialized state could not be held across the navigate event");
    /* STEPS 7-9: "let navigation be history's relevant global object's navigation API; let continue be the
       result of FIRING A PUSH/REPLACE/RELOAD NAVIGATE EVENT at navigation with navigationType set to
       historyHandling, isSameDocument set to true, destinationURL set to newURL, and classicHistoryAPIState set
       to serializedData; if continue is false, then return."
       THE OPERATION IS CREATED HERE AND DRIVEN AT THE NEXT STAGE, and it takes newURL and serializedData WITH
       it — core/frame/navigate_event_fire.h states why: between this line and the dispatch's answer every
       `navigate` listener the page has runs, so an algorithm that read the destination back off the navigable
       would resolve this navigation against whatever a listener left behind. */
    navigate_event_fire_push_replace_reload_begin(ctx, &s->fire, magic == HIST_PUSH ? "push" : "replace",
                                                  new_url, /*is_same_document*/ true, &serialized);
    free(new_url);
    structured_data_free(ctx, &serialized);
    url_record_free(&doc_url);
    url_record_free(&target);
    /* AND IT RETURNS. Setting the stage and running on is what the declaration would then be lying about: the
       driver never saw the boundary, so nothing could park between §7.2.5's head and the dispatch and the
       second label named a rest point that did not exist. JS_STEP_YIELD hands the decision to the scheduler —
       it parks the flow if a sibling outranks it and re-enters here immediately if none does. Nothing is
       carried across: `cb_result` is this entry's (JS_UNDEFINED, since no request was in flight) and the next
       stage's own arrives on its re-entry. */
    JS_FreeValue(ctx, cb_result);
    STEP_GOTO(hdr->stage, HPR_NAVIGATE, &s->fire.phase, &s->fire.abort.phase,
              &s->fire.abort.sig.phase, &s->w.nav.phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(HPR_NAVIGATE);
    {
        /* §7.2.6.10.4 answers, having dispatched `navigate` at this realm's Navigation and run every listener
           the page registered for it. */
        bool proceed = false;

        r = navigate_event_fire_run(ctx, &s->fire, cb_result, out_cb, out_argc, &proceed);
        if (r != 0) return r;
        /* STEP 9: "if continue is FALSE, then return." A listener called preventDefault(), or the firing
           algorithm answered false before it ever built an event — either way §7.4.4 must not run, and the
           member answers `undefined` exactly as it does when it succeeds. */
        if (!proceed) {
            *presult = JS_UNDEFINED;
            return JS_STEP_DONE;
        }
    }
    /* STEP 10 — §7.4.4's two halves. _begin runs its steps 1-10 and touches none of the page's code. Its two
       arguments are the values this machine took with it before the dispatch; the StructuredData below is a
       BORROWED view of the ArrayBuffer's storage, so it is not freed through structured_data_free — the buffer
       owns those bytes and the machine's declaration frees the buffer. */
    {
        const char *url = JS_ToCString(ctx, s->new_url);
        StructuredData held;

        CHECK(url != NULL, "history: the new address could not be read back after the navigate event");
        held.buf = JS_GetArrayBuffer(ctx, &held.len, s->classic);
        DCHECK(held.buf != NULL, "history: the serialized state held across the navigate event is not the "
                                 "bytes — this machine's own stage above is the only writer of that slot");
        session_history_url_update_begin(ctx, &s->w, url, &held, magic == HIST_PUSH);
        JS_FreeCString(ctx, url);
    }
    STEP_GOTO(hdr->stage, HPR_UPDATE, &s->fire.phase, &s->fire.abort.phase,
              &s->fire.abort.sig.phase, &s->w.nav.phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(HPR_UPDATE);
    /* §7.4.4 steps 11-13, the half that runs the page's code: the navigation API's `currententrychange` and
       `dispose`, then the finalize. */
    r = session_history_url_update_run(ctx, &s->w, cb_result, out_cb, out_argc);
    if (r != 0) return r;
    *presult = JS_UNDEFINED;
    return JS_STEP_DONE;
}

static const IdlStepDecl HPR_DECL = {
    js_hist_push_replace, sizeof(HprState), hpr_visit, NULL,
    "HTML §7.2.5 the shared history push/replace state steps", HPR_STEPS
};

/* ---- §7.2.5's DELTA TRAVERSE, and the three members that are it ---------------------------------------------
 *
 * "The go(delta) method steps are to DELTA TRAVERSE this given delta. The back() method steps are to delta
 * traverse this given −1. The forward() method steps are to delta traverse this given +1." One algorithm, three
 * spellings, so one body with a magic — and the magic says which of the three, not what the delta is, because
 * `go`'s comes from its argument and the other two's are constants of the standard. */
enum { HIST_TRAVERSE_GO = 0, HIST_TRAVERSE_BACK, HIST_TRAVERSE_FORWARD };

static JSValue js_hist_delta_traverse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    int32_t delta = 0;

    /* STEPS 1-2: the Document, and "if document is NOT FULLY ACTIVE, then throw a SecurityError DOMException".
       It is the same opening every member of §7.2.5 has and it is reached through the same door — a THROW and
       not an assert, because a page reaches it deliberately: removing an iframe and then calling
       `frame.contentWindow.history.back()` is a thing the corpus does on purpose. It is FIRST, which is
       observable: `go(0)` on a detached frame is a SecurityError and not a reload. */
    if (!hist_entry(ctx, this_val)) return JS_EXCEPTION;
    switch (magic) {
    case HIST_TRAVERSE_BACK:    delta = -1; break;
    case HIST_TRAVERSE_FORWARD: delta = +1; break;
    case HIST_TRAVERSE_GO:
        /* `optional long delta = 0`. An argument the page did not pass, and one it passed as `undefined` —
           §3.6.2 makes those the same thing for an optional position — is the IDL's declared default, which is
           why `history.go()` and `history.go(undefined)` both reload. Anything it did pass has already been
           through the `long` conversion (ToNumber, then modulo 2^32 signed), so the page's `valueOf` ran on the
           machine and this reads a number. */
        if (argc >= 1 && !JS_IsUndefined(argv[0])) {
            DCHECK(JS_IsNumber(argv[0]),
                   "§7.2.5's `go` was handed something that is not a number — its argument is declared IDL_LONG, "
                   "so Web IDL's `long` conversion produced one before this body was entered");
            JS_ToInt32(ctx, &delta, argv[0]);
        }
        break;
    default:
        DFAIL("a History traversal member ran with a magic no member of this file declares — the magic IS which "
              "of §7.2.5's three delta-traverse spellings was called");
        return JS_UNDEFINED;
    }
    /* STEP 3: "if history's relevant global object's navigable's ALLOWED TO PERFORM A NAVIGATION OR HISTORY
       UPDATE returns blocked, then return." The same implementation-defined algorithm §7.2.5's push/replace
       steps evaluate above, with the same answer and for the same reason: HTML §7.3.1 gives its only example as
       "this can return blocked if invoked too many times within a certain timespan", and a rate cap on how often
       a bundle may route is a bound on the exploration. It returns ALLOWED for every navigable here. */
    /* STEP 4: "if delta is 0, then RELOAD document's node navigable, AND RETURN." The return is the step's and
       stands on its own — a zero delta never reaches §7.4.3 — so it is written here rather than left to the
       crash above, which is dev-only. */
    if (delta == 0) {
        DFAIL("HTML §7.2.5's delta traverse step 4 answers a delta of 0 with a RELOAD of the document's node "
              "navigable, and this build cannot reload one. §7.4.3's RELOAD A NAVIGABLE sets the active session "
              "history entry's document state's RELOAD PENDING and applies the reload history step, which is "
              "§7.4.6.1's apply-the-history-step with navigationType \"reload\" — the CROSS-DOCUMENT half: "
              "§7.4.5's populate-the-history-entry's-document fetches the entry's URL, §7.4.6.1's DEACTIVATE "
              "fires pageswap and unloads the displayed document (pagehide, the unload event, destroy), and "
              "activate-history-entry then makes the new Document active. Build it in "
              "core/frame/session_history.c beside the same-document traversal, driven from the same machine, "
              "and core/frame/location.c's `reload` — which is the same algorithm reached from the other "
              "member — installs with it");
        return JS_UNDEFINED;
    }
    /* STEP 5: "TRAVERSE THE HISTORY BY A DELTA given document's node navigable's traversable navigable, delta,
       and with sourceDocument set to document." It SCHEDULES the traversal — §7.4.3's steps are appended to the
       traversable — so this member returns `undefined` with the page's own `popstate` listener not yet run. */
    session_history_traverse_by_delta(ctx, delta);
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
    /* §7.2.5's IDL order: go, back, forward, pushState, replaceState. `go`'s LENGTH is 0 — Web IDL §3.7.4.1's
       length is the number of REQUIRED arguments, and `optional long delta = 0` is not one. */
    idl_install_method(ctx, proto, "go", 0, g_id_go);
    idl_install_method(ctx, proto, "back", 0, g_id_back);
    idl_install_method(ctx, proto, "forward", 0, g_id_forward);
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
    /* §7.2.2's `readonly attribute History history` — and it carries NO [Replaceable], which is the extended
       attribute the neighbouring `[Replaceable] readonly attribute Navigation navigation` does carry. The two
       differ in a way a page can see: `window.navigation = 1` replaces the accessor with a data property and
       `window.history = 1` does not (it is silently ignored in sloppy mode and a TypeError in strict), so this
       is an ordinary accessor. It was installed replaceable with a comment quoting an extended attribute the
       IDL does not have — checked against §7.2.2's interface block, not inferred. */
    idl_install_accessor(ctx, global, "history", js_win_history, 0, -1);
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

        g_id_push = idl_method_id_step(ctx, ARGS, 3, NULL, 0, &HPR_DECL, HIST_PUSH);
        idl_optional_from(2);
        g_id_replace = idl_method_id_step(ctx, ARGS, 3, NULL, 0, &HPR_DECL, HIST_REPLACE);
        idl_optional_from(2);
    }
    /* §7.2.5's `undefined go(optional long delta = 0)`, `undefined back()` and `undefined forward()` — one
       algorithm declared three times with the three magics its one body switches on. `back` and `forward` take
       no arguments at all, which is what a declaration of zero says; `go`'s single argument is DECLARED
       optional, so §3.6.2 hands the body `undefined` for an absent one and the body applies the IDL default. */
    {
        static const IdlArgType GO_ARGS[] = { IDL_LONG };

        g_id_go = idl_method_id(ctx, GO_ARGS, 1, js_hist_delta_traverse, HIST_TRAVERSE_GO);
        idl_optional_from(0);
        g_id_back = idl_method_id(ctx, NULL, 0, js_hist_delta_traverse, HIST_TRAVERSE_BACK);
        g_id_forward = idl_method_id(ctx, NULL, 0, js_hist_delta_traverse, HIST_TRAVERSE_FORWARD);
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
    g_id_push = g_id_replace = g_id_scroll_setter = -1;
    g_id_go = g_id_back = g_id_forward = -1;
}
