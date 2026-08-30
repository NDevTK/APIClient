/* HTML §7.4.1's session history, §7.4.3's traversal, and the two synchronous history updates — §7.4.4's URL
   and history update steps and §7.4.2.3.3's navigate to a fragment — see session_history.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/agent_state.h"
#include "core/dom/document.h"
#include "core/events/event_target.h"
#include "core/events/hash_change_event.h"
#include "core/events/pop_state_event.h"
#include "core/frame/navigation.h"
#include "core/frame/session_history.h"
#include "core/frame/window_proxy.h"
#include "core/realm.h"
#include "core/structured_clone.h"
#include "core/url/url.h"
#include "solver/route_seed.h"   /* §7.4.4 step 8 is where an application declares one of its own pages */

static int g_slot = -1;

/* ---- §7.4.1.1's SESSION HISTORY ENTRY, and §7.4.1.2's DOCUMENT STATE -----------------------------------------
 *
 * Both are STRUCTS in the standard and both are ORDINARY JS OBJECTS here, for the reason session_history.h
 * gives: a struct a flow mutates has to ride that flow's COW delta, and a property write already does. Their
 * fields are named by these atoms and by nothing else, so a field the code reads and no builder writes is one
 * assertion away rather than a silent `undefined`.
 *
 * THE FIELDS THIS ENGINE DOES NOT HOLD ARE NAMED WHERE THEY ARE NOT HELD, not omitted. §7.4.1.1 also gives an
 * entry a SCROLL POSITION DATA and a PERSISTED USER STATE, which §7.4.6.5's save/restore are the only writers
 * and readers of. A traversal reaches those two, and reaches them through sh_persisted_state, which asserts
 * against the `scrollTo` that would give this user agent a scroll position to save in the first place.
 *
 * THE NAVIGATION API'S THREE FIELDS ARE HELD, and they are §7.4.1.1's own rather than §7.2.6's: an entry has a
 * navigation API STATE (serialized, initially StructuredSerializeForStorage(UNDEFINED) — not null, which is
 * what the classic state's initial value is, and the difference is observable through `entry.getState()`), a
 * navigation API KEY and a navigation API ID. core/frame/navigation.c reads all three through the accessors at
 * the foot of this file and writes none of them directly. */
#define SH_E_STEP    "step"        /* a non-negative integer, or the string "pending" */
#define SH_E_URL     "url"
#define SH_E_DOCSTATE "documentState"
#define SH_E_CLASSIC "classicState" /* serialized state — an ArrayBuffer of §2.7 bytes, never a live value */
#define SH_E_SCROLL  "scrollRestoration"
#define SH_E_NAV_STATE "navigationState" /* §7.2.6's serialized state — an ArrayBuffer, never a live value */
#define SH_E_NAV_KEY   "navigationKey"
#define SH_E_NAV_ID    "navigationId"

/* §7.4.1.2's document state. Its `document` is a Document or null, and a DOCUMENT IS NAMED BY ITS ID here —
   the same id the world registry names documents by, which is what crosses an instance, a session and a park.
   A `JSContext *` is none of those things, which is the whole reason the name is the field. */
#define SH_D_DOC     "document"     /* the world-registry document id; -1 for §7.4.1.2's null document */
#define SH_D_ORIGIN  "origin"
#define SH_D_EVER    "everPopulated"

/* ---- the per-realm record ------------------------------------------------------------------------------------
 *
 * TWO OWNERS, ONE RECORD, AND THE SPLIT IS THE STANDARD'S. `entries` and `step` belong to the TRAVERSABLE —
 * §7.4.1's session history is the traversable's, and every navigable under it reads its length and its current
 * step from there. `active`, `latest`, `state`, `length` and `index` belong to THIS navigable's Document: the
 * active session history entry is the navigable's (§7.4.6's activate-history-entry writes it), the latest entry
 * is the Document's (§7.4.1.1's last paragraph), and the other three are the History object's own fields
 * (§7.2.5: "Each History object has state / a length / an index").
 *
 * They share one record because they share one lifetime and one realm, and because a traversable's realm needs
 * both halves; the accessors below say which half they are reaching for, and reaching for the traversable's
 * half goes through sh_traversable_record so that a nested navigable cannot read its own empty `entries`. */
#define SH_R_ENTRIES "entries"
#define SH_R_STEP    "step"
#define SH_R_ACTIVE  "active"
/* §7.4.6.1's "set navigable's CURRENT session history entry to targetEntry" — a SECOND entry beside the active
   one, and the two are different things whenever a traversal is in flight. The standard's own worked example
   says why they cannot be one field: `location.href = '#foo'` "synchronously changes the ACTIVE session history
   entry to a newly-created one … Note that this does not yet update the current session history entry, current
   session history step, or the session history entries list; those updates cannot be done synchronously, and
   instead must be done as part of the queued steps." So the ACTIVE entry is what the document is showing and
   what §7.2.5's `state` and `scrollRestoration` read; the CURRENT entry is where the traversable has decided the
   navigable is going, and §7.4.1.4's get-all-navigables-whose-current-session-history-entry-will-change is the
   algorithm that reads it. */
#define SH_R_CURRENT "current"
#define SH_R_LATEST  "latest"
#define SH_R_STATE   "state"
#define SH_R_LENGTH  "length"
#define SH_R_INDEX   "index"
/* THE COUNTER §7.4.1.1's "generate a random UUID" IS ANSWERED FROM — see session_history.h for why a counter
   and not a draw. It is on the RECORD rather than in a C static for the reason core/file/blob.c's mint counter
   is: a static is one number for the whole agent, so two arms of a fork minting an entry at the same source
   line would get DIFFERENT keys, which is precisely what the determinism exists to prevent. */
#define SH_R_UUID    "uuidCounter"

static JSValue sh_record(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_slot);

    DCHECK(JS_IsObject(rec), "a realm answered for its §7.4.1 session history with no record — the record is "
                             "built with the realm by session_history_install_realm");
    return rec;
}

/* ---- INFRA's list operations over a JS Array, as §6.6.7's autofocus candidates already spell them ----------- */

static uint32_t list_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    return n;
}

static void list_truncate(JSContext *ctx, JSValueConst list, uint32_t n)
{
    JS_SetPropertyStr(ctx, list, "length", JS_NewUint32(ctx, n));
}

/* ---- reading a record's fields ------------------------------------------------------------------------------ */

static uint32_t rec_uint(JSContext *ctx, JSValueConst rec, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, rec, name);
    uint32_t n = 0;

    DCHECK(JS_IsNumber(v), "a §7.4.1 record field that is a non-negative integer held something else — every "
                           "writer of these fields is in this file and every one of them writes a number");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

static void rec_set_uint(JSContext *ctx, JSValueConst rec, const char *name, uint32_t n)
{
    JS_SetPropertyStr(ctx, rec, name, JS_NewUint32(ctx, n));
}

/* ---- §7.4.1.4's "get session history entries of a navigable" -------------------------------------------------
 *
 * The standard's algorithm is a walk: if the navigable IS the traversable, the answer is the traversable's own
 * entries; otherwise it searches every entry's document state's NESTED HISTORIES for the one whose id is this
 * navigable's, descending through the document states it finds on the way.
 *
 * A NESTED HISTORY IS CREATED BY §7.4's CREATE A NEW NAVIGABLE, NOT BY THIS FILE, which is why the second half
 * is a DFAIL naming that step rather than a walk over a list that is empty by construction. §7.4's create a new
 * navigable appends a new nested history — with the child's id and a fresh entries list — to the PARENT ENTRY's
 * document state, and until core/frame/navigable.c does that, every child navigable's entries list is one
 * nobody ever built. Answering the traversable's list for a child instead would put an iframe's pushState into
 * its parent's history, which is a wrong answer and not a partial one. */
/* The TRAVERSABLE's record — where `entries` and the current session history step live. OWNED.
 *
 * A NAVIGABLE THAT IS ITS OWN TRAVERSABLE IS THE ONLY ONE THIS ANSWERS FOR, and the assertion names the two
 * different builds the two other cases need. It does NOT walk to the top navigable and read that realm's
 * record: for a SAME-INSTANCE child the answer would be the parent's entries list, which is the wrong list
 * rather than a missing one, and for a CROSS-ORIGIN child the top navigable's realm is a peer's and reading it
 * from here would be a cross-instance read answered out of the wrong heap. */
static void sh_assert_is_traversable(JSContext *ctx)
{
    DCHECK(window_proxy_is_top_level(document_window_proxy(ctx)),
           "HTML §7.4.1.4 \"Low-level operations on session history\"'s get-session-history-entries reached a "
           "navigable that is not its own traversable. There are two builds behind this, and which one depends "
           "on where the traversable is. SAME INSTANCE: this navigable's entries live in a NESTED HISTORY "
           "inside its parent entry's document state (§7.4.1.2 \"Document state\" gives every document state a "
           "NESTED HISTORIES list, initially empty), and nothing has built one. HTML §7.3.1.3 \"Child "
           "navigables\"' CREATE A NEW CHILD NAVIGABLE is the algorithm — there is no \"create a new "
           "navigable\" in the standard, and this crash named one — and its append is NOT of an empty list: "
           "\"let nestedHistory be a new nested history whose id is navigable's id and entries list is « "
           "historyEntry »\", where historyEntry is the child's own first entry, which the same algorithm has "
           "just built and given a step. A nested history appended EMPTY would leave a child navigable with "
           "zero session history entries, so `history.length` inside a fresh frame would read 0 where every "
           "browser reads 1. Do the append in core/frame/navigable.c's navigable_create, where the child's id "
           "is minted and its first entry is available, and §7.4.1.4's walk over document states then finds it "
           "by id. THAT WALK IS THE OTHER HALF AND IT LANDS WITH THE APPEND: this engine keeps a session "
           "history record PER REALM (session_history_install_document builds one at step 0 for every "
           "Document), so a child's entries are in that record and not in its parent's document state — "
           "appending a nested history nothing reads would be a computed writer with no reader, which is the "
           "one shape this file must not grow. ANOTHER INSTANCE (a cross-origin frame): the entries, the "
           "current session history step and every step assignment belong to the traversable's instance, so a "
           "push or a replace here is a cross-instance operation that SUSPENDS the asking flow (SECURITY.md's "
           "closed set) — build it in core/frame/window_proxy.c beside the other cross-instance requests, "
           "carrying the entry as TEXT the way every other cross-instance answer does");
}

static JSValue sh_traversable_record(JSContext *ctx)
{
    sh_assert_is_traversable(ctx);
    return sh_record(ctx);
}

/* §7.4.1.4's GET SESSION HISTORY ENTRIES, for THIS realm's navigable. OWNED. */
static JSValue sh_entries(JSContext *ctx)
{
    JSValue rec = sh_traversable_record(ctx), list = JS_GetPropertyStr(ctx, rec, SH_R_ENTRIES);

    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsArray(list), "§7.4.1's session history entries held something that is not a list");
    return list;
}

/* ---- §7.4.1.1's entry, and §7.4.1.2's document state --------------------------------------------------------- */

/* SERIALIZED STATE IS BYTES, NEVER A LIVE VALUE — the same rule a cross-document message is carried under, and
   here for the same three reasons: an entry parks to the IDB cold tier, resumes in a later session, and is read
   back by a DESERIALIZE that must produce a fresh object graph in the reading realm each time. An ArrayBuffer
   is what carries bytes through all three for free. `d` is BORROWED; this copies. */
static JSValue sh_state_buffer(JSContext *ctx, const StructuredData *d)
{
    JSValue buf;

    DCHECK(d != NULL && d->buf != NULL, "§7.4.1.1's serialized state was built from no bytes — the initial "
                                        "value is StructuredSerializeForStorage(null), which is bytes, so an "
                                        "entry never legitimately holds nothing");
    buf = JS_NewArrayBufferCopy(ctx, d->buf, d->len);
    CHECK(!JS_IsException(buf), "session history: an entry's serialized state could not be allocated");
    return buf;
}

/* §2.7's StructuredSerializeForStorage of a PRIMITIVE — §7.4.1.1's two initial values, and they are two
   DIFFERENT primitives: the classic history API state starts at serialized NULL and the navigation API state
   at serialized UNDEFINED. The difference is observable (`history.state` is null on a fresh document while
   `navigation.currentEntry.getState()` is undefined), which is why this takes the value rather than baking one
   in. `out` is the caller's to free with structured_data_free. */
static void sh_serialize_primitive(JSContext *ctx, JSValueConst v, StructuredData *out)
{
    int r = structured_serialize(ctx, v, out);

    DCHECK(JS_IsNull(v) || JS_IsUndefined(v),
           "§7.4.1.1's initial serialized states are null and undefined and this is neither — a value that can "
           "run the page's code has no business being serialized from a plain C call");
    CHECK(r == 0, "session history: StructuredSerializeForStorage of a primitive failed — null and undefined "
                  "are serializable by every clause of §2.7, so a refusal here is the serializer being unable "
                  "to allocate");
}

/* §7.4.1.2's DOCUMENT STATE for the Document of `ctx`. Its `ever populated` is TRUE: this engine's Documents
   are built from a completed parse, so a document state exists only once its document does. */
static JSValue sh_document_state_new(JSContext *ctx)
{
    JSValueConst proxy = document_window_proxy(ctx);
    const Origin *origin = window_proxy_origin(proxy);
    JSValue ds = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(ds), "session history: a §7.4.1.2 document state could not be allocated");
    JS_SetPropertyStr(ctx, ds, SH_D_DOC, JS_NewUint32(ctx, document_doc(ctx)));
    /* §7.4.1.2's origin is "the origin that we set about:-schemed Documents' origin to … also used to compare
       the origin before and after the session history entry is repopulated". The navigable's active document's
       origin IS that origin, and window_proxy is the one place this engine holds it. IT IS STORED AS A HANDLE
       — a §7.1.1 origin is a record whose first comparison step is IDENTITY, so an entry that kept only the
       serialization could not tell one opaque origin from another and would break exactly the run of
       about:-schemed entries this field exists for. */
    JS_SetPropertyStr(ctx, ds, SH_D_ORIGIN, JS_NewUint32(ctx, origin_id(origin)));
    JS_SetPropertyStr(ctx, ds, SH_D_EVER, JS_TRUE);
    return ds;
}

/* §7.4.1.1's "the result of GENERATING A RANDOM UUID", answered deterministically — see session_history.h for
   why, and core/file/blob.c for the same rule stated where it was first paid for. The shape is a version-4
   UUID's, because a page reads these as opaque UUID strings and some of them validate the shape; what fills it
   is this DOCUMENT's id and a counter on the per-realm record, which makes it unique by construction across
   every entry of every document in the agent rather than unique with high probability. OWNED. */
static JSValue sh_mint_uuid(JSContext *ctx)
{
    JSValue rec = sh_record(ctx);
    uint32_t n = rec_uint(ctx, rec, SH_R_UUID);
    char buf[40];

    rec_set_uint(ctx, rec, SH_R_UUID, n + 1);
    JS_FreeValue(ctx, rec);
    snprintf(buf, sizeof buf, "%08x-0000-4000-8000-%012x", (unsigned)document_doc(ctx), (unsigned)n);
    return JS_NewString(ctx, buf);
}

/* §7.4.1.1's "a session history entry is a struct with the following items", built with the seven this engine
   holds. `doc_state`, `classic` and `nav_state` are CONSUMED; `url` and `scroll` are copied. The KEY and the ID
   are minted here and are never arguments: §7.4.4's new entry takes neither from the entry it replaces, and
   §7.4.2.3.1's cross-document finalize — the one algorithm that DOES carry a key across — is not in this build
   (see sh_finalize_same_document_navigation). */
static JSValue sh_entry_new(JSContext *ctx, const char *url, JSValue doc_state, JSValue classic,
                            JSValue nav_state, const char *scroll)
{
    JSValue e = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(e), "session history: a §7.4.1.1 entry could not be allocated");
    /* "step, a non-negative integer or 'pending', initially 'pending'" — the string, because a number that
       means "not a number yet" is a sentinel the assertions below could not tell from a real step. */
    JS_SetPropertyStr(ctx, e, SH_E_STEP, JS_NewString(ctx, "pending"));
    JS_SetPropertyStr(ctx, e, SH_E_URL, JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, e, SH_E_DOCSTATE, doc_state);
    JS_SetPropertyStr(ctx, e, SH_E_CLASSIC, classic);
    JS_SetPropertyStr(ctx, e, SH_E_NAV_STATE, nav_state);
    JS_SetPropertyStr(ctx, e, SH_E_NAV_KEY, sh_mint_uuid(ctx));
    JS_SetPropertyStr(ctx, e, SH_E_NAV_ID, sh_mint_uuid(ctx));
    JS_SetPropertyStr(ctx, e, SH_E_SCROLL, JS_NewString(ctx, scroll));
    return e;
}

/* THE NAVIGABLE'S ACTIVE SESSION HISTORY ENTRY. OWNED. */
static JSValue sh_active_entry(JSContext *ctx)
{
    JSValue rec = sh_record(ctx), e = JS_GetPropertyStr(ctx, rec, SH_R_ACTIVE);

    JS_FreeValue(ctx, rec);
    DCHECK(JS_IsObject(e),
           "a navigable answered for its ACTIVE SESSION HISTORY ENTRY with nothing — every navigable has one "
           "from the moment its Document is installed (session_history_install_document), so a realm that has "
           "a Document and no active entry never ran that install");
    return e;
}

static uint32_t sh_entry_step(JSContext *ctx, JSValueConst e)
{
    JSValue v = JS_GetPropertyStr(ctx, e, SH_E_STEP);
    uint32_t n = 0;

    DCHECK(JS_IsNumber(v), "a session history entry's step was read while it was still \"pending\" — §7.4.2.3.3's "
                           "finalize-a-same-document-navigation is what assigns one, and it runs before any "
                           "algorithm that indexes by step");
    JS_ToUint32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return n;
}

/* IS THIS ENTRY'S §7.4.1.2 DOCUMENT STATE NAMING THIS REALM'S DOCUMENT — the question §7.4.6.1 asks twice, as
   "targetEntry's document is null" (a traversal that must fetch) and "targetEntry's document is equal to
   displayedDocument" (a traversal that unloads nothing). A document state names its document BY ID here — see
   SH_D_DOC — and -1 is §7.4.1.2's null document, which no id equals because a minted id is non-negative. */
static bool sh_entry_is_this_document(JSContext *ctx, JSValueConst e)
{
    JSValue ds = JS_GetPropertyStr(ctx, e, SH_E_DOCSTATE);
    JSValue d;
    uint32_t id = 0;
    bool same;

    DCHECK(JS_IsObject(ds), "a session history entry held no §7.4.1.2 document state");
    d = JS_GetPropertyStr(ctx, ds, SH_D_DOC);
    DCHECK(JS_IsNumber(d), "a §7.4.1.2 document state's document is not an id — the world registry names "
                           "documents by id, and that name is the field because it is what crosses an "
                           "instance, a session and a park");
    JS_ToUint32(ctx, &id, d);
    same = id == document_doc(ctx);
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, ds);
    return same;
}

/* §7.4.4 STEP 4's "document's IS INITIAL about:blank" — defined with the URL and history update steps below,
   and read by §7.4.6.1's activate-history-entry assertion above it. */
static bool sh_is_initial_about_blank(JSContext *ctx);

/* ---- §7.4.1.4's low-level operations -------------------------------------------------------------------------
 *
 * THE ENTRIES OF A TRAVERSABLE WITH NO CHILD NAVIGABLES ARE SORTED AND DISTINCT BY CONSTRUCTION, and that is
 * asserted rather than assumed: §7.4.2.3.3 appends at current-step + 1 after clearing the forward history, or
 * replaces in place keeping the replaced entry's step, so the list is the run 0..n-1 in order. §7.4.1.4's "get
 * all used history steps" collects the steps of every entry of every nested history and SORTS them precisely
 * because a nested history breaks that; when nested histories arrive (see sh_entries) this becomes that
 * collection, and the assertion is what will fire on the first entry that no longer fits. */
static void sh_assert_steps_are_the_run(JSContext *ctx, JSValueConst entries)
{
    uint32_t n = list_len(ctx, entries), i;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, entries, i);
        uint32_t s = sh_entry_step(ctx, e);

        JS_FreeValue(ctx, e);
        DCHECK(s == i, "HTML §7.4.1.4's used history steps are no longer the run 0..n-1 — the only writers of "
                       "an entry's step are §7.4.2.3.3's append (current step + 1, after the forward history "
                       "is cleared) and its replace (the replaced entry's step), so a gap means a nested "
                       "history's entries have joined the list and getting all used history steps must become "
                       "the SORTED COLLECTION over nested histories that §7.4.1.4 states");
        (void)s;
    }
}

/* §7.4.1.4's CLEAR THE FORWARD SESSION HISTORY: "remove every session history entry from entryList that has a
   step greater than step". Over the run above, that is a truncation to step + 1 entries. */
static void sh_clear_forward(JSContext *ctx, JSValueConst entries, uint32_t step)
{
    uint32_t n = list_len(ctx, entries);

    sh_assert_steps_are_the_run(ctx, entries);
    DCHECK(step < n, "§7.4.1.4's clear-the-forward-session-history ran with a current step past the end of the "
                     "entries — the current step always names an entry that exists");
    if (n > step + 1) list_truncate(ctx, entries, step + 1);
}

/* §7.4.6.1's GET THE HISTORY OBJECT LENGTH AND INDEX, over the same run: the length is the number of used
   steps and the index is where `step` sits among them. */
static void sh_length_and_index(JSContext *ctx, JSValueConst entries, uint32_t step,
                               uint32_t *plength, uint32_t *pindex)
{
    uint32_t n = list_len(ctx, entries);

    sh_assert_steps_are_the_run(ctx, entries);
    DCHECK(step < n, "§7.4.6.1's get-the-history-object-length-and-index asserts that the used steps contain "
                     "the step it was given, and this one names no entry");
    *plength = n;
    *pindex = step;
}

/* §7.4.1.4's GET THE TARGET HISTORY ENTRY given a navigable and a step: "return the item in entries that has the
   GREATEST STEP LESS THAN OR EQUAL TO step". The "less than or equal to" is not slack — the standard's own Jake
   diagram shows a child navigable whose entries stop at step 1 answering step 3 with its step-1 entry — and over
   this traversable's run 0..n-1 it collapses to entries[step], which is asserted rather than assumed. OWNED. */
static JSValue sh_target_history_entry(JSContext *ctx, JSValueConst entries, uint32_t step)
{
    JSValue e;

    sh_assert_steps_are_the_run(ctx, entries);
    DCHECK(step < list_len(ctx, entries),
           "§7.4.1.4's get-the-target-history-entry was given a step past the end of a run of entries — over a "
           "run there is no entry with a smaller step to fall back to, so the answer would be nothing at all");
    e = JS_GetPropertyUint32(ctx, entries, step);
    DCHECK(JS_IsObject(e), "§7.4.1.4's get-the-target-history-entry found something that is not an entry");
    return e;
}

/* ---- §7.4.1.4's GET SESSION HISTORY ENTRIES FOR THE NAVIGATION API ------------------------------------------
 *
 * IT IS NOT THE ENTRIES LIST. The navigation API sees only the SAME-ORIGIN CONTIGUOUS RUN around the entry the
 * document is at: the algorithm starts at the entry with the greatest step less than or equal to targetStep,
 * walks BACKWARDS while each entry's document state's origin is same origin with that one's and stops at the
 * first that is not, then walks FORWARDS the same way. That truncation is the whole reason §7.2.6.6's
 * `canGoBack` is documented as "there is a previous session history entry for this navigable, AND its document
 * state's origin is same origin with the current Document's" — a cross-origin entry two steps back is simply
 * not in the list, so an index of 0 already means there is nothing this API may go back to.
 *
 * EVERY ENTRY THIS BUILD CREATES SHARES ONE DOCUMENT STATE, so every origin in the run is this document's and
 * neither walk ever breaks — which makes the answer the whole list today and makes the origin comparison a
 * branch nothing yet takes. It is written anyway, because the first cross-document navigation puts an entry
 * with another origin in the middle of the list, and it compares against the STARTING entry's origin, which is
 * what the standard says and is not the same thing as this realm's.
 *
 * The answer is a new JS Array holding the entries themselves. OWNED. */

/* A §7.4.1.2 document state's ORIGIN. The slot holds the origin's HANDLE rather than its serialization, for
   the reason the standard's own wording gives — the field is "the origin that we set about:-schemed
   Documents' origin to … also used to COMPARE the origin before and after the entry is repopulated", and a
   comparison of origins is §7.1.1's algorithm, whose first step is identity. A serialization drops that.
   BORROWED: an origin lives for the agent (core/url/origin.h), so there is nothing here for the caller to
   free — which is why this no longer hands back a JSValue to hold. */
static const Origin *sh_entry_origin(JSContext *ctx, JSValueConst e)
{
    JSValue ds = JS_GetPropertyStr(ctx, e, SH_E_DOCSTATE);
    JSValue ov;
    uint32_t id = 0;
    int rc;

    DCHECK(JS_IsObject(ds), "a session history entry held no §7.4.1.2 document state");
    ov = JS_GetPropertyStr(ctx, ds, SH_D_ORIGIN);
    JS_FreeValue(ctx, ds);
    rc = JS_ToUint32(ctx, &id, ov);
    (void)rc;
    JS_FreeValue(ctx, ov);
    DCHECK(rc == 0 && id != 0, "a §7.4.1.2 document state held no origin handle — every entry is built by "
                               "sh_document_state_new, which writes one");
    return origin_by_id(id);
}

JSValue session_history_entries_for_navigation_api(JSContext *ctx)
{
    JSValue entries = sh_entries(ctx), trec = sh_traversable_record(ctx), out = JS_NewArray(ctx);
    JSValue start;
    uint32_t target_step, n, k = 0;
    const Origin *start_origin;
    int64_t i;

    CHECK(!JS_IsException(out), "session history: §7.4.1.4's navigation API entry list could not be allocated");
    target_step = rec_uint(ctx, trec, SH_R_STEP);
    JS_FreeValue(ctx, trec);
    sh_assert_steps_are_the_run(ctx, entries);
    n = list_len(ctx, entries);
    DCHECK(target_step < n, "§7.4.1.4's get-session-history-entries-for-the-navigation-API was given a step "
                            "past the end of a run of entries — over a run every step in range is used, so a "
                            "starting index outside it names no entry to start from");
    /* "Let startingIndex be the index of the session history entry in rawEntries who has the GREATEST STEP LESS
       THAN OR EQUAL TO targetStep" — over the run 0..n-1 that is targetStep itself, which the assertion above
       keeps true. */
    start = JS_GetPropertyUint32(ctx, entries, target_step);
    start_origin = sh_entry_origin(ctx, start);
    JS_SetPropertyUint32(ctx, out, k++, start);   /* CONSUMES start */
    /* The BACKWARD walk. The standard's condition is `while i > 0`, so the entry at index 0 is reached by the
       body when i is 1 and index 0 itself is never tested — written here as the same bound rather than as
       `i >= 0`, because the difference is one entry and it is the FIRST one. */
    for (i = (int64_t)target_step - 1; i > 0; i--) {
        JSValue e = JS_GetPropertyUint32(ctx, entries, (uint32_t)i);
        /* §7.1.1's SAME ORIGIN over two document states, decided on the RECORD — so two entries that share one
           opaque origin (an about:blank document and the document that created it) stay in one run, which a
           serialized comparison answered "different" for. */
        bool same = origin_same(sh_entry_origin(ctx, e), start_origin);
        uint32_t j;

        if (!same) { JS_FreeValue(ctx, e); break; }
        /* PREPEND — the list stays in session-history order, which is what makes §7.2.6.5's `index` mean what
           a page reads it as. A shift is what a prepend over an Array is; its length is the session history's
           and not anything one navigation grows. */
        for (j = k; j > 0; j--)
            JS_SetPropertyUint32(ctx, out, j, JS_GetPropertyUint32(ctx, out, j - 1));
        JS_SetPropertyUint32(ctx, out, 0, e);
        k++;
    }
    /* The FORWARD walk. */
    for (i = (int64_t)target_step + 1; i < (int64_t)n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, entries, (uint32_t)i);
        bool same = origin_same(sh_entry_origin(ctx, e), start_origin);

        if (!same) { JS_FreeValue(ctx, e); break; }
        JS_SetPropertyUint32(ctx, out, k++, e);
    }
    JS_FreeValue(ctx, entries);
    return out;
}

/* §7.4.3 steps 4.1-4.4: "let allSteps be the result of GETTING ALL USED HISTORY STEPS for traversable; let
   currentStepIndex be the INDEX OF traversable's current session history step WITHIN allSteps; let
   targetStepIndex be currentStepIndex plus delta; if allSteps[targetStepIndex] does not exist, then ABORT THESE
   STEPS."
   Over the run 0..n-1 the used steps ARE their own indices, so the current step is its own index and the target
   step is its own too — which is why this is index arithmetic and not two searches. The delta is SIGNED and the
   step is not, so the sum is computed in int64 and the existence test is the range check on it; a `uint32_t`
   sum would make `history.go(-5)` from step 0 an enormous forward step rather than the standard's abort.
   Returns false for the standard's abort. */
static bool sh_resolve_delta(JSContext *ctx, int32_t delta, uint32_t *ptarget_step)
{
    JSValue entries = sh_entries(ctx), trec = sh_traversable_record(ctx);
    int64_t target = (int64_t)rec_uint(ctx, trec, SH_R_STEP) + (int64_t)delta;
    uint32_t n;

    sh_assert_steps_are_the_run(ctx, entries);
    n = list_len(ctx, entries);
    JS_FreeValue(ctx, trec);
    JS_FreeValue(ctx, entries);
    if (target < 0 || target >= (int64_t)n) return false;
    *ptarget_step = (uint32_t)target;
    return true;
}

/* A session history entry's URL, as the string the entry holds. OWNED — released with JS_FreeCString. */
static const char *sh_entry_url(JSContext *ctx, JSValueConst e)
{
    JSValue v = JS_GetPropertyStr(ctx, e, SH_E_URL);
    const char *s = JS_ToCString(ctx, v);

    JS_FreeValue(ctx, v);
    CHECK(s != NULL, "session history: an entry's URL could not be read");
    return s;
}

/* §7.4.6.2 step 6.4.5's "if oldURL's FRAGMENT is not equal to entry's URL's fragment". §4.1's fragment is a
   string OR NULL, and the two are distinct: `/a` has no fragment and `/a#` has the empty one, so a traversal
   between them DOES fire `hashchange`. A comparison that folded null into "" would silently drop that. */
static bool sh_fragment_equal(const char *a_url, const char *b_url)
{
    UrlRecord a, b;
    bool same;

    url_record_init(&a);
    url_record_init(&b);
    CHECK(url_parse(&a, a_url, strlen(a_url), NULL) && url_parse(&b, b_url, strlen(b_url), NULL),
          "session history: an entry's URL is not a URL — every writer of the field serialized a URL record");
    if (!a.fragment || !b.fragment) same = a.fragment == b.fragment;
    else                            same = strcmp(a.fragment, b.fragment) == 0;
    url_record_free(&a);
    url_record_free(&b);
    return same;
}

/* §7.4.2.2's THIRD AND FOURTH CONJUNCTS — see session_history.h for the other two and for why this is a
 * component rather than an `if` at a call site.
 *
 * THE THIRD IS URL §4.6 "URL equivalence" WITH EXCLUDE FRAGMENTS, and that algorithm is a comparison of
 * SERIALIZATIONS and of nothing else: "let serializedA be the result of serializing A, with exclude fragment
 * set to exclude fragments … return true if serializedA is serializedB; otherwise false". A field-by-field
 * comparison is a different predicate — it would call two records that serialize alike unequal whenever the
 * parser had a choice, and §4.6 exists precisely so that nobody makes that choice twice.
 *
 * THE FOURTH IS `url's fragment is NON-NULL`, and null is not the empty string: `/a` has no fragment and `/a#`
 * has the empty one. `location.href = "/a"` from `/a#x` therefore fails this conjunct and is a CROSS-DOCUMENT
 * navigation that re-fetches — which is what browsers do and what a folded comparison would silently turn into
 * a no-op. */
bool session_history_is_fragment_navigation(JSContext *ctx, const char *url)
{
    UrlRecord target, active;
    JSValue entry;
    const char *active_url;
    char *bare_target, *bare_active;
    bool same;

    DCHECK(g_slot >= 0, "§7.4.2.2's same-document test ran before session_history_init declared the record");
    DCHECK(url != NULL, "§7.4.2.2's same-document test was asked about no URL — its caller has a destination "
                        "or it is not navigating");
    url_record_init(&target);
    url_record_init(&active);
    CHECK(url_parse(&target, url, strlen(url), NULL),
          "session history: §7.4.2.2's destination is not a URL — every caller hands over a SERIALIZATION of a "
          "record the parser produced, and a serialized URL re-parses");
    entry = sh_active_entry(ctx);
    active_url = sh_entry_url(ctx, entry);
    CHECK(url_parse(&active, active_url, strlen(active_url), NULL),
          "session history: the active entry's URL is not a URL — every writer of the field serialized a URL "
          "record");
    bare_target = url_serialize(&target, /*exclude_fragment*/ true);
    bare_active = url_serialize(&active, /*exclude_fragment*/ true);
    CHECK(bare_target && bare_active, "session history: a URL could not be serialized for §4.6's equivalence");
    same = strcmp(bare_target, bare_active) == 0 && target.fragment != NULL;
    free(bare_target);
    free(bare_active);
    JS_FreeCString(ctx, active_url);
    JS_FreeValue(ctx, entry);
    url_record_free(&target);
    url_record_free(&active);
    return same;
}

/* ---- §7.4.6's APPLY THE HISTORY STEP — ONE ALGORITHM, TWO ENTRY POINTS ---------------------------------------
 *
 * §7.4.6.1's apply-the-history-step is an enormous algorithm because it coordinates a traversable with every
 * descendant navigable that a traversal changes, populating them in parallel and synchronising back up. Its four
 * entry points — update-for-navigable-creation/destruction, apply-the-PUSH/REPLACE-history-step, apply-the-RELOAD
 * -history-step and apply-the-TRAVERSE-history-step — differ only in the five arguments they pass, and the
 * standard writes them as five one-line wrappers over the one algorithm for exactly that reason. TWO of them are
 * built here (push/replace and traverse) and they are ONE body, not two: a second implementation beside this one
 * is where the two would disagree about what a `pushState` inside a `popstate` handler does.
 *
 * WHAT SPLITS THE BODY IN HALF IS THE STANDARD'S OWN SPLIT, not this engine's. §7.4.6.1 queues the per-navigable
 * work as two tasks and says why: "this set of steps are split into two parts to allow synchronous navigations to
 * be processed before documents unload. State is stored in changingNavigableContinuations for the second part."
 * SessionHistoryApply IS that changing navigable continuation state — the standard's struct, with the
 * length/index and the previous entry the second half is also handed — and _begin and _finish are its two parts.
 *
 * ONLY ONE OF THE TWO HALVES CAN RUN THE PAGE'S CODE, and that is what lets a caller that cannot park still
 * reach the algorithm. A PUSH or a REPLACE takes the update-only exit — §7.4.4 synchronously set the navigable's
 * active session history entry to the new entry, so the algorithm's own test ("if displayedEntry is targetEntry
 * and targetEntry's document state's reload pending is false, set update-only to true and abort these steps")
 * holds, its note saying "this case occurs due to a synchronous navigation which already updated the active
 * session history entry" — and §7.4.6.2 then reaches neither the popstate branch nor the reactivate branch, which
 * is the standard's own explanation of why "popstate events fire for fragment navigations, but not for
 * history.pushState() calls". A TRAVERSAL takes the other exit and fires popstate at the page's listeners, which
 * is a DISPATCH and therefore a request the caller's machine parks on. So the entry points differ in whether
 * they can park, exactly as core/events/event_target.c's two reaches into §2.9 do, and the algorithm does not.
 *
 * WHERE THIS MACHINE RESTS. `stage` is on the continuation state rather than only on the step header because the
 * synchronous caller has no header and must still be told, by the same field, that the algorithm wants to park —
 * which it asserts against. The traversal machine copies it onto its header, so there is ONE declaration of the
 * rest points and the two cannot drift. There are THREE now: the traversal's own resolve, step 6.4.2's
 * navigation API update (which fires `currententrychange` and then `dispose` at each disposed entry, so the
 * walk inside it has its own sub-cursor) and step 6.4.3's popstate. */
#define SH_APPLY_STAGES(X)                                                                              \
    X(SH_APPLY_STEP,     "HTML §7.4.3 traverse the history by a delta steps 4.1-4.5 (get all used "      \
                         "history steps, resolve the delta, apply the traverse history step)")           \
    X(SH_APPLY_NAV,      "HTML §7.4.6.2 update document for history step application step 6.4.2 (update " \
                         "the navigation API entries for a same-document navigation: currententrychange, " \
                         "then dispose at each disposed entry)")                                          \
    X(SH_APPLY_POPSTATE, "HTML §7.4.6.2 update document for history step application step 6.4.3 (fire "  \
                         "an event named popstate at the document's relevant global object, using "      \
                         "PopStateEvent)")
enum { SH_APPLY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SH_APPLY_STEPS[] = { SH_APPLY_STAGES(JS_STEP_STAGE_LABEL) NULL };
/* ONE NAME FOR THE ALGORITHM THESE STAGES ARE STEPS OF. Three functions dispatch on this one stage list — the
   traversal machine's step, the apply's second half, and §7.4.6.2 — and the machine's definition names it a
   fourth time. Written out at each of them they are four statements of one fact, and the abort a
   STEP_DISPATCH raises would name a different algorithm depending on which of the three it came from. */
#define SH_APPLY_ALGORITHM "HTML §7.4.6.1 apply the history step, as the traversable's queued session history " \
                           "traversal steps"

/* §7.4.6.1's CHANGING NAVIGABLE CONTINUATION STATE is SessionHistoryApply, and it is DECLARED IN THE HEADER —
   see there for what its fields are and why. It moved out of this file when §7.4.2.3.3's fragment navigation
   became a machine a MEMBER hosts: §7.4.2.3.3 step 14 calls update-document-for-history-step-application, whose
   three rest points are this struct's `stage`, so the record has to be a field of a work record a caller
   outside this file declares. Nothing else about it changed, and every function below still takes it. */

static void sh_restore_history_object_state(JSContext *ctx, JSValueConst entry);

static void sh_apply_free(JSContext *ctx, SessionHistoryApply *a)
{
    int k;

    JS_FreeValue(ctx, a->target_entry);
    JS_FreeValue(ctx, a->displayed_entry);
    JS_FreeValue(ctx, a->old_url);
    JS_FreeValue(ctx, a->popstate);
    a->target_entry = a->displayed_entry = a->old_url = a->popstate = JS_UNDEFINED;
    STEP_CB_FOREACH(a->fire_cb, k) {
        JS_FreeValue(ctx, a->fire_cb[k]);
        a->fire_cb[k] = JS_UNDEFINED;
    }
    navigation_update_work_release(ctx, &a->nav);
}

/* §7.4.6.5's SAVE PERSISTED STATE TO a session history entry, and its twin RESTORE PERSISTED STATE FROM one.
 * Both are entirely about state a RENDERING holds: "set the scroll position data of entry to contain the scroll
 * positions for all of entry's document's restorable scrollable regions", and then an OPTIONAL clause for "any
 * state that the user agent wishes to persist, such as the values of form fields".
 * THIS USER AGENT HAS NO SCROLL POSITION TO SAVE — core/frame/viewport.c DERIVES scrollX/scrollY rather than
 * holding them, because with no layout the scrolling area is the viewport and the only position it can have is
 * (0,0) — so the scroll position data of every entry is that one point and restoring it moves nothing. The
 * optional clause is optional. Asserted against the producer rather than written down as a claim: `scrollTo` is
 * the member whose arrival means a scrolling box can be moved at all, which is the same probe viewport.c's own
 * two-sided assertion and core/html/autofocus.c's target-element checks are asked against. */
static void sh_persisted_state(JSContext *ctx)
{
    realm_awaits(ctx, "scrollTo", "HTML §7.4.6.5's save/restore persisted state carries a session history "
                                  "entry's SCROLL POSITION DATA across a traversal, and this build now has a "
                                  "way to move a scrolling box — so an entry gains a scroll position data "
                                  "field, activate-history-entry writes the outgoing entry's from the "
                                  "document's restorable scrollable regions, and §7.4.6.2 step 6.4.4 restores "
                                  "the incoming entry's when its scroll restoration mode is \"auto\"");
}

/* §7.4.6.1's ACTIVATE HISTORY ENTRY for this navigable — reached only when update-only is false, which is only
 * ever a TRAVERSAL here.
 *
 * THE DOCUMENT'S ADDRESS MOVES WITH THE ACTIVE ENTRY, and the standard does not say so in this algorithm. That
 * is not a licence to leave it behind: §7.4.6.2 step 6.4.5 fires `hashchange` with its `newURL` "initialized to
 * the serialization of entry's URL", and §7.2.7.3 defines that attribute as "the URL of the session history
 * entry that IS NOW CURRENT" — so a Document whose address stayed at the old entry would hand a listener a
 * `newURL` its own `location.href` disagreed with, and §7.2.5's `state` and `scrollRestoration` (which read
 * through the ACTIVE entry) would answer for one entry while `location` answered for another. §7.4.4 step 8
 * sets the document's URL when an entry becomes active by a push or a replace; which entry is active is a fact
 * about the navigable, not about the route by which it got there.
 *
 * "MAKE ACTIVE newDocument" and "set the initial visibility state" are the two remaining steps, and both are
 * already true of this document: it never stopped being the navigable's active document (this is a SAME-document
 * traversal — _begin asserts it) and it never stopped being visible. */
static void sh_activate_history_entry(JSContext *ctx, JSValueConst entry)
{
    JSValue rec;
    const char *url;

    sh_persisted_state(ctx);   /* "save persisted state to the navigable's active session history entry" */
    /* "Assert: newDocument's is initial about:blank is false, i.e., we never traverse back to the initial
       about:blank Document because it always gets replaced when we navigate away from it." */
    DCHECK(!sh_is_initial_about_blank(ctx),
           "§7.4.6.1's activate-history-entry asserts the entry's document is not the initial about:blank — an "
           "entry naming one is an entry §7.4.4 built before the navigable was ever navigated, and §7.4.4 step "
           "4 turns a push on such a document into a replace precisely so that no second entry can name it");
    rec = sh_record(ctx);
    JS_SetPropertyStr(ctx, rec, SH_R_ACTIVE, JS_DupValue(ctx, entry));
    JS_FreeValue(ctx, rec);
    url = sh_entry_url(ctx, entry);
    document_set_url(ctx, url);
    JS_FreeCString(ctx, url);
}

/* ---- §7.4.6.2's UPDATE DOCUMENT FOR HISTORY STEP APPLICATION -------------------------------------------------
 *
 * Returns JS_STEP_CALL when it has parked on step 6.4.3's popstate dispatch (the caller returns it), or 0 when
 * the algorithm is finished. `in` is the dispatch's answer on re-entry.
 *
 * THE THREE BRANCHES THIS BUILD DOES NOT TAKE ARE EVALUATED, NOT DROPPED. documentIsNew is false because a
 * Document reaching this always has a latest entry (session_history_install_document gives it one before any
 * page script runs), so step 8's WebDriver BiDi branch and step 6.5's initialize-the-navigation-API-entries
 * branch are unreachable and say so — the latter is run at the document's install instead, which is where this
 * engine's collapsed populate-and-activate happens. Step 7's NavigationActivation branch has a REAL condition —
 * "navigationType is 'reload' or previousEntryForActivation's document is not document" — which is evaluated
 * here and is false for both callers, because neither a push/replace nor a same-document traversal changes the
 * document. */
static int sh_update_document_for_history_step(JSContext *ctx, SessionHistoryApply *a, JSValue in,
                                               JSValue **out_cb, int *out_argc)
{
    JSValue rec, latest, global;
    bool documents_entry_changed;
    int r;

    STEP_DISPATCH(SH_APPLY_STAGES, a->stage, SH_APPLY_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(SH_APPLY_STEP);
    rec = sh_record(ctx);
    latest = JS_GetPropertyStr(ctx, rec, SH_R_LATEST);
    /* STEP 1. */
    DCHECK(JS_IsObject(latest),
           "§7.4.6.2 step 1's documentIsNew is TRUE — a Document reached update-document-for-history-step-"
           "application with no LATEST ENTRY, which cannot happen while every Document gets one at install. "
           "Its branch is step 8: assert the during-loading navigation ID for WebDriver BiDi is not null, "
           "invoke WebDriver BiDi navigation committed, and TRY TO SCROLL TO THE FRAGMENT (§7.4.6.4)");
    /* STEP 2. */
    documents_entry_changed = JS_VALUE_GET_PTR(latest) != JS_VALUE_GET_PTR(a->target_entry);
    /* STEPS 3 AND 4 — the real values, replacing §7.4.4 step 6's "temporary best-guess" ones. */
    rec_set_uint(ctx, rec, SH_R_INDEX, a->index);
    rec_set_uint(ctx, rec, SH_R_LENGTH, a->length);
    /* STEP 5 is "let navigation be history's relevant global object's NAVIGATION API" — this realm's, which
       core/frame/navigation.c answers for; the two later steps that read it reach it through that component
       rather than holding one here. */
    if (!documents_entry_changed) {
        /* STEP 9: "otherwise, if documentsEntryChanged is false and doNotReactivate is false, REACTIVATE
           document". doNotReactivate is update-only, and the two are false together only when a Document comes
           back out of the back/forward cache — which needs a document that was deactivated rather than one that
           never stopped being active. */
        DCHECK(a->update_only,
               "§7.4.6.2 reached step 9 — documentsEntryChanged is false and doNotReactivate is false, which is "
               "a Document being RESTORED FROM BFCACHE. §7.4.6.2's REACTIVATE has to be written: reset every "
               "autofill=off form control, resume the suspended timer handles by the suspension's duration, "
               "restore persisted state, and run §7.2.6.4's UPDATE THE NAVIGATION API ENTRIES FOR REACTIVATION "
               "(core/frame/navigation.c holds its two siblings and names this one at its initialize)");
        JS_FreeValue(ctx, latest);
        JS_FreeValue(ctx, rec);
        /* `in` is a REQUEST'S ANSWER and every exit owns it: the dispatch below consumes it, so an exit that
           never reaches the dispatch releases it here. */
        JS_FreeValue(ctx, in);
        goto activation_branch;
    }
    /* STEP 6.1 — oldURL, read BEFORE step 6.2 replaces the latest entry, and held across the dispatch below
       because a `pushState` from a popstate listener replaces the latest entry again before step 6.4.5 reads
       the fragments. */
    a->old_url = JS_GetPropertyStr(ctx, latest, SH_E_URL);
    JS_FreeValue(ctx, latest);
    /* STEP 6.2. */
    JS_SetPropertyStr(ctx, rec, SH_R_LATEST, JS_DupValue(ctx, a->target_entry));
    JS_FreeValue(ctx, rec);
    /* STEP 6.3. */
    sh_restore_history_object_state(ctx, a->target_entry);
    /* STEP 6.4.1: "assert: navigationType is not null." */
    DCHECK(a->navigation_type != NULL,
           "§7.4.6.2 step 6.4.1 asserts a non-null NavigationType, and the only entry point that passes null is "
           "§7.4.6.1's update-for-navigable-creation/destruction — which never changes a document's entry");
    /* AND IT RETURNS RATHER THAN RUNNING ON. Step 6.4.1 ends here and step 6.4.2 is the next stage; a machine
       that assigns the stage and falls into the next arm has crossed a boundary the driver never saw, so the
       label would name a rest point the engine could not park at — which is the cap quickjs-step.h's
       JSTrampStepDef::steps refuses. JS_STEP_YIELD is what asks: the scheduler parks this traversal if a
       sibling flow outranks it and re-enters immediately if none does. `in` is this entry's request answer and
       nothing below has asked for one yet, so it is released here rather than carried across the rest point. */
    JS_FreeValue(ctx, in);
    STEP_GOTO(a->stage, SH_APPLY_NAV, &a->nav.phase, &a->fire_phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(SH_APPLY_NAV);
    /* STEP 6.4.2: "UPDATE THE NAVIGATION API ENTRIES FOR A SAME-DOCUMENT NAVIGATION given navigation, entry
       and navigationType." IT RUNS THE PAGE'S CODE — `currententrychange` at the Navigation, then `dispose` at
       every entry the update threw away — so it is a request and this is where the traversal parks first. It
       runs BEFORE the popstate below, which is the standard's order and is observable: a `currententrychange`
       listener sees `navigation.currentEntry` already moved while `history.state` has just been restored. */
    r = navigation_update_entries_run(ctx, &a->nav, a->target_entry, a->navigation_type, in, out_cb, out_argc);
    if (r != 0) return r;
    in = JS_UNDEFINED;
    /* STEP 6.4.3 — the popstate event. `hasUAVisualTransition` is "true if a VISUAL TRANSITION, to display a
       cached rendered state of the latest entry, was done by the user agent"; this user agent performs none, so
       it is false — a computed answer about this agent and not a default. The `state` is the history object's,
       which step 6.3 has just deserialized into this realm, and it is handed over as the SAME object so that a
       listener's `e.state === history.state` holds. */
    {
        JSValue state = session_history_state(ctx);

        a->popstate = pop_state_event_new_to_fire(ctx, state, /*has_ua_visual_transition*/ false);
        JS_FreeValue(ctx, state);
        if (JS_IsException(a->popstate)) {
            a->popstate = JS_UNDEFINED;
            JS_FreeValue(ctx, in);
            return JS_STEP_ABRUPT;
        }
    }
    /* The event is minted in the stage that ENDS and the fire is the stage that begins, for the reason
       core/file/file_picker.c mints step 7.4's `dismissed` a stage early: the next arm is re-entered by its own
       request cursor on every resume of the dispatch, so anything it built itself it would build again. */
    STEP_GOTO(a->stage, SH_APPLY_POPSTATE, &a->nav.phase, &a->fire_phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(SH_APPLY_POPSTATE);
    global = JS_GetGlobalObject(ctx);
    /* `popstate` is not cancelable, so there is no cancellation to read back — §2.9's answer is discarded the
       way DOM's fire-an-event discards it for every event no algorithm branches on. */
    r = event_target_fire_run(ctx, &a->fire_phase, STEP_CB(a->fire_cb), global, a->popstate, JS_UNDEFINED, in,
                              NULL, out_cb, out_argc);
    JS_FreeValue(ctx, global);
    if (r > 0) return r;
    JS_FreeValue(ctx, a->popstate);
    a->popstate = JS_UNDEFINED;
    /* STEP 6.4.4. */
    sh_persisted_state(ctx);   /* "restore persisted state given entry" */
    /* STEP 6.4.5: "if oldURL's fragment is not equal to entry's URL's fragment, then QUEUE A GLOBAL TASK on the
       DOM MANIPULATION task source … to fire an event named hashchange … using HashChangeEvent, with the oldURL
       attribute initialized to the serialization of oldURL and the newURL attribute initialized to the
       serialization of entry's URL". QUEUED, unlike the popstate above — which is why this is the other reach
       into §2.9 (event_target_fire) and not a second request. */
    {
        const char *old_url = JS_ToCString(ctx, a->old_url), *new_url;

        CHECK(old_url != NULL, "session history: §7.4.6.2's oldURL could not be read");
        new_url = sh_entry_url(ctx, a->target_entry);
        if (!sh_fragment_equal(old_url, new_url)) {
            JSValue win = JS_GetGlobalObject(ctx);

            event_target_fire(ctx, win, hash_change_event_new_to_fire(ctx, old_url, new_url), JS_UNDEFINED);
            JS_FreeValue(ctx, win);
        }
        JS_FreeCString(ctx, new_url);
        JS_FreeCString(ctx, old_url);
    }
activation_branch:
    /* STEP 7's condition, EVALUATED. Its third disjunct is "navigationType is 'reload' or
       previousEntryForActivation's DOCUMENT is not document" — and both callers here are same-document, so the
       branch is not entered. The moment a cross-document traversal or a reload exists it is. */
    DCHECK(strcmp(a->navigation_type, "reload") != 0 && sh_entry_is_this_document(ctx, a->displayed_entry),
           "§7.4.6.2 step 7 was reached with a previous entry belonging to ANOTHER Document, or with a reload — "
           "its body is §7.2.6.9's NAVIGATION ACTIVATION, an interface core/frame/navigation.c does not build "
           "(its `activation` member is absent for exactly this reason). Build NavigationActivation there — "
           "old entry, new entry, navigation type — and then here: create one in the navigation API's relevant "
           "realm if it has none, set its old entry from previousEntryForActivation's index in the entry list "
           "(or, for a \"replace\" from a same-origin non-initial-about:blank document, a fresh "
           "NavigationHistoryEntry over that entry), its new entry to navigation's current entry, and its "
           "navigation type to navigationType");
    /* AND THE ALGORITHM IS BACK AT ITS FIRST STEP. §7.4.6.2 is finished, so the record its caller carries names
       the step it would be re-entered at rather than the last one it rested at — which is what the traversal
       machine copies onto its header. The update-only exit reaches here already there, which STEP_GOTO admits:
       a write that changes nothing is not a transition. */
    STEP_GOTO(a->stage, SH_APPLY_STEP, &a->nav.phase, &a->fire_phase, NULL);
    return 0;
}

/* ---- §7.4.6.1's APPLY THE HISTORY STEP, first half --------------------------------------------------------- */

static void sh_apply_history_step_begin(JSContext *ctx, SessionHistoryApply *a, uint32_t step, const char *nav_type)
{
    JSValue entries = sh_entries(ctx), trec = sh_traversable_record(ctx);
    int k;

    DCHECK(nav_type != NULL && (!strcmp(nav_type, "push") || !strcmp(nav_type, "replace") ||
                                !strcmp(nav_type, "traverse")),
           "§7.4.6.1 was entered with a NavigationType none of its entry points passes — the four are \"push\", "
           "\"replace\", \"reload\" and \"traverse\", and the two this component builds are the first, the "
           "second and the last");
    a->navigation_type = nav_type;
    a->stage = SH_APPLY_STEP;
    a->target_entry = a->displayed_entry = a->old_url = a->popstate = JS_UNDEFINED;
    a->fire_phase = 0;
    STEP_CB_FOREACH(a->fire_cb, k) a->fire_cb[k] = JS_UNDEFINED;
    navigation_update_work_start(&a->nav);
    a->begin_step = rec_uint(ctx, trec, SH_R_STEP);

    /* STEP 2: "let targetStep be the result of GETTING THE USED STEP given traversable and step" — the greatest
       used step less than or equal to `step`, which caters for a step whose entry a destroyed navigable removed.
       Over a run of entries every step in range is used, so the used step is the step; the assertion that keeps
       that true is sh_assert_steps_are_the_run, which sh_target_history_entry makes. */
    a->target_step = step;
    a->target_entry = sh_target_history_entry(ctx, entries, a->target_step);
    /* STEP 3 is the INITIATOR SANDBOXING check, skipped because initiatorToCheck is null for both entry points:
       apply-the-push/replace-history-step "never passes source snapshot params or an initiator navigable … this
       is because those checks are done earlier in the navigation algorithm", and §7.4.3's delta traverse passes
       one only for a traversal initiated by a document that is not this navigable's own — which is the
       cross-navigable case sh_entries already asserts against.
       STEP 5's CHECKING IF UNLOADING IS CANCELED IS NOT SKIPPED FOR THE SAME REASON, AND THIS SAID IT WAS. The
       claim was that it is "reached only for a navigable that goes CROSS-DOCUMENT, and there are none", which
       is true of the LIST it is handed (navigablesCrossingDocuments, empty for a same-document traversal) and
       false of the ALGORITHM: check-if-unloading-is-canceled takes that list AND the TRAVERSABLE, and its
       traverse-navigate-event branch is guarded by "if traversable was given" — which apply-the-history-step
       always does, whatever the list holds. So a `history.back()` on a top-level traversable takes that branch
       with an EMPTY beforeunload list and fires a TRAVERSE NAVIGATE EVENT the page can cancel. That is the
       third time in this file's neighbourhood a guard has been right about the case it was written for and
       wrong about the case that arrived, and it is the same shape as the two STEP_DISPATCH now makes
       unwritable — the difference being that this one was a claim in prose, which no macro can refuse.
       WHICH ENTRY POINT REACHES IT IS `checkForCancelation`, and the two differ: apply-the-push/replace-history
       -step passes FALSE and apply-the-traverse-history-step passes TRUE. So the assertion for it belongs to
       the TRAVERSAL machine rather than here, where both entry points meet — see js_sh_traverse_step. */
    /* §7.4.6.1's "for each navigable of changingNavigables: SET NAVIGABLE'S CURRENT SESSION HISTORY ENTRY to
       targetEntry". It is set for BOTH exits — the update-only test below is about the ACTIVE entry, not this
       one — which is what makes §7.4.1.4's get-all-navigables-whose-current-entry-will-change answer correctly
       for the next application. */
    {
        JSValue rec = sh_record(ctx);
        JS_SetPropertyStr(ctx, rec, SH_R_CURRENT, JS_DupValue(ctx, a->target_entry));
        JS_FreeValue(ctx, rec);
    }
    /* The per-navigable job's FIRST half: "let displayedEntry be navigable's ACTIVE session history entry; let
       targetEntry be navigable's CURRENT session history entry" — and then the update-only test. There is no
       RELOAD PENDING on a document state here (nothing sets one: §7.4.3's reload is the only writer and this
       component installs no reload), so the test is the entry comparison alone. */
    a->displayed_entry = sh_active_entry(ctx);
    a->update_only = JS_VALUE_GET_PTR(a->displayed_entry) == JS_VALUE_GET_PTR(a->target_entry);
    if (!a->update_only) {
        JSValue ds = JS_GetPropertyStr(ctx, a->target_entry, SH_E_DOCSTATE);
        JSValue ever = JS_GetPropertyStr(ctx, ds, SH_D_EVER);

        /* The NavigationType switch. Only the traverse arm is reachable with update-only false: a push or a
           replace has already made its new entry the active one, which is the test just above. */
        DCHECK(!strcmp(a->navigation_type, "traverse"),
               "§7.4.6.1's per-navigable job took the CHANGING exit for a push or a replace — its switch then "
               "asserts that targetEntry's step is displayedEntry's step (replace) or displayedEntry's step + 1 "
               "(push) AND that the entry's document state's EVER POPULATED is false, which is a brand-new "
               "entry that no Document has been built for: §7.4.5's populate-a-session-history-entry is what "
               "would have to run");
        DCHECK(JS_ToBool(ctx, ever),
               "§7.4.6.1's \"traverse\" arm asserts targetEntry's document state's EVER POPULATED is true, and "
               "this entry's is false — an entry whose Document has never been built. §7.4.5's POPULATE THE "
               "HISTORY ENTRY'S DOCUMENT has to run for it: fetch the entry's URL, build the Document, and only "
               "then continue the traversal");
        JS_FreeValue(ctx, ever);
        JS_FreeValue(ctx, ds);
        /* "If targetEntry's DOCUMENT IS NULL, or targetEntry's document state's reload pending is true" — a
           CROSS-DOCUMENT traversal, which fetches. Every entry this component creates shares the ACTIVE entry's
           document state (§7.4.4 steps 1-3), so the document is this one and the traversal is same-document. */
        DCHECK(sh_entry_is_this_document(ctx, a->target_entry),
               "§7.4.6.1 reached a traversal whose target entry names ANOTHER Document — a CROSS-DOCUMENT "
               "traversal. It is the long half of the algorithm and none of it is here: §7.4.5's populate the "
               "history entry's document IN PARALLEL, then §7.4.6.1's DEACTIVATE (pageswap, unload-a-document-"
               "and-its-descendants, pagehide), then activate-history-entry over the new Document. Build it in "
               "this file beside the same-document path, driven from the same machine");
        /* "If navigable is NOT TRAVERSABLE, and targetEntry is not navigable's current session history entry,
           and oldOrigin is the same as … then FIRE A TRAVERSE NAVIGATE EVENT" — §7.2.6.10.4's, and the FIRST
           CONJUNCT IS FALSE here for the reason sh_entries states: this navigable IS its own traversable. So
           the branch is not taken, and that is a fact about this build rather than a gap in it — the branch
           exists to let a NESTED navigable's traversal be canceled from its own document.
           THIS SITE CARRIED THE ASSERTION FOR THE TRAVERSE NAVIGATE EVENT AND IT WAS THE WRONG SITE. Its own
           text said the top-level fire "would fire it here", and it would not: §7.4.6.1 step 8 is where the
           NESTED fire lives, and the one a traversable performs for ITSELF is step 5's CHECK IF UNLOADING IS
           CANCELED — see sh_apply_history_step_begin, whose step-5 comment made the matching mistake. An
           assertion at a branch that is never taken is an assertion that never fires, which is why the
           misplacement was invisible. */
        sh_assert_is_traversable(ctx);
    }
    /* "Let (scriptHistoryLength, scriptHistoryIndex) be the result of GETTING THE HISTORY OBJECT LENGTH AND
       INDEX given traversable and targetStep." */
    sh_length_and_index(ctx, entries, a->target_step, &a->length, &a->index);
    JS_FreeValue(ctx, trec);
    JS_FreeValue(ctx, entries);
}

/* ---- §7.4.6.1's APPLY THE HISTORY STEP, second half -------------------------------------------------------- */

static int sh_apply_history_step_finish(JSContext *ctx, SessionHistoryApply *a, JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValue trec;
    int r;

    /* THE DISPATCH IS THE DECLARATION'S, AND THAT IS WHY THIS HALF IS SAFE TO ADD A STAGE TO. It was
       `if (a->stage != SH_APPLY_POPSTATE)` — "anything but the last stage" — while there were two rest points,
       and the moment §7.4.6.2 gained a third (step 6.4.2's navigation API update) that spelling started
       re-running activate-history-entry on every resume through it. The afterPotentialUnloads assertion below
       would then have reported a SECOND history step applied across this one and sent its reader to build
       §7.4.1.3's traversal queue for a race that never happened. A resume guard written as a negation is wrong
       for every stage added after it, and a positive one that names only some of them is wrong for the arm it
       does not name — so neither is written here: quickjs-step.h's STEP_DISPATCH generates the arms from
       SH_APPLY_STAGES, and a fourth stage added to that list does not compile until it has a body. */
    STEP_DISPATCH(SH_APPLY_STAGES, a->stage, SH_APPLY_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(SH_APPLY_STEP);
    {
        /* "If changingNavigableContinuation's update-only is true, OR targetEntry's document is
           displayedDocument: this is a SAME-DOCUMENT NAVIGATION, we proceed without unloading" — both hold, the
           second because _begin asserted the target entry names this Document. The other arm DEACTIVATES the
           displayed document, and _begin's assertion names what that needs.
           "afterPotentialUnloads: let previousEntry be navigable's ACTIVE SESSION HISTORY ENTRY" — read here
           rather than reused from _begin because that is where the standard reads it, and because reading it
           twice is what catches a second history step having been applied in between. */
        JSValue previous = sh_active_entry(ctx);
        bool same = JS_VALUE_GET_PTR(previous) == JS_VALUE_GET_PTR(a->displayed_entry);

        JS_FreeValue(ctx, previous);
        DCHECK(same,
               "§7.4.6.1's afterPotentialUnloads found the navigable's active session history entry is no "
               "longer the displayed entry its own first half recorded — a SECOND history step was applied "
               "across this one. §7.4.1.3's SESSION HISTORY TRAVERSAL QUEUE is what orders them (its algorithm "
               "set, its running-nested-apply-history-step flag, and §7.4.6.1's "
               "navigablesThatMustWaitBeforeHandlingSyncNavigation), and it has to be built for two "
               "applications to overlap");
        /* "If changingNavigableContinuation's update-only is false, then ACTIVATE HISTORY ENTRY targetEntry for
           navigable." */
        if (!a->update_only)
            sh_activate_history_entry(ctx, a->target_entry);
        /* "If targetEntry's document is equal to displayedDocument, then PERFORM updateDocument" — it is, so it
           runs here rather than as a queued task in the target document's realm. */
    }
    /* AND THE THREE ARMS SHARE THIS TAIL, which is `case A: case B: case C:` and reads as one: §7.4.6.2 IS the
       perform-updateDocument this step reaches, and the two later stages are rest points INSIDE it, so a resume
       at either of them re-enters the same call. The stage has not moved across the arms above, so no rest
       point is crossed by falling into it. */
    STEP_ARM(SH_APPLY_NAV);
    STEP_ARM(SH_APPLY_POPSTATE);
    r = sh_update_document_for_history_step(ctx, a, in, out_cb, out_argc);
    /* JS_STEP_CALL to park on the dispatch, JS_STEP_ABRUPT if the event could not be minted — either way the
       algorithm has not reached its last step, and only 0 means it has. A `> 0` test here would swallow the
       throw AND run the trailing step on a half-applied history step. */
    if (r != 0) return r;
    /* "Let totalNonchangingJobs be the size of nonchangingNavigablesThatStillNeedUpdates" — the navigables whose
       target entry IS already their current entry, which for a traversable with no child navigables is the empty
       list whenever the traversable itself changed, and is the traversable itself when it did not. Either way
       the job's body (set the document's history object index and length) is what §7.4.6.2 above has already
       done for this one navigable, so there is nothing left to run.
       "SET TRAVERSABLE'S CURRENT SESSION HISTORY STEP TO targetStep" — the last step of the algorithm. */
    trec = sh_traversable_record(ctx);
    DCHECK(rec_uint(ctx, trec, SH_R_STEP) == a->begin_step,
           "§7.4.6.1's final step was about to overwrite a CURRENT SESSION HISTORY STEP that moved while this "
           "application was running — a nested synchronous navigation (a pushState from a popstate listener) "
           "reached the traversable. §7.4.6.1 orders that with its running-nested-apply-history-step flag and "
           "its navigablesThatMustWaitBeforeHandlingSyncNavigation set, which §7.4.1.3's traversal queue owns "
           "and which has to be built for the two to interleave");
    rec_set_uint(ctx, trec, SH_R_STEP, a->target_step);
    JS_FreeValue(ctx, trec);
    return 0;
}

/* §7.4.6.1's APPLY THE PUSH/REPLACE HISTORY STEP: "return the result of applying the history step step to
   traversable given false, null, null, userInvolvement, and historyHandling". It is the entry point that CANNOT
   PARK, and the assertion below is what makes that safe rather than assumed: the update-only exit runs none of
   the page's code, which is the standard's own reason `popstate` does not fire for `pushState`. */
static void sh_apply_push_replace_history_step(JSContext *ctx, uint32_t target_step, const char *history_handling)
{
    SessionHistoryApply a;
    JSValue *cb = NULL;
    int cb_argc = 0, r;

    sh_apply_history_step_begin(ctx, &a, target_step, history_handling);
    DCHECK(a.update_only,
           "§7.4.6.1's apply-the-push/replace-history-step took the CHANGING exit — §7.4.4 step 10 sets the "
           "navigable's active session history entry to the new entry before §7.4.2.3.3's finalize reaches "
           "here, so the update-only test holds unless a second writer moved it");
    r = sh_apply_history_step_finish(ctx, &a, JS_UNDEFINED, &cb, &cb_argc);
    DCHECK(r == 0,
           "§7.4.6.1's apply-the-push/replace-history-step PARKED — the update-only exit reached §7.4.6.2's "
           "popstate dispatch, which runs the page's listeners, and this entry point is a plain C algorithm "
           "called from §7.2.5's pushState with no flow to suspend. Either documentsEntryChanged became true "
           "for a push (which §7.4.4 step 10 forbids), or this call has to become a queued job on §7.4.1.3's "
           "SESSION HISTORY TRAVERSAL QUEUE like the traversal below");
    sh_apply_free(ctx, &a);
}

/* ---- §7.4.2.3.3's FINALIZE A SAME-DOCUMENT NAVIGATION ---------------------------------------------------------
 *
 * IT IS THE STEP THAT GIVES THE NEW ENTRY ITS PLACE IN THE LIST, and the standard runs it on the traversable's
 * SESSION HISTORY TRAVERSAL PARALLEL QUEUE, one turn later than the synchronous updates §7.4.4 makes. That
 * queue exists to serialise modifications that arrive from MULTIPLE EVENT LOOPS — §7.4.1.3: "this is especially
 * important due to how session history is influenced by all of the descendant navigables, and thus by multiple
 * event loops" — and its step 2 ("if targetNavigable's active session history entry is not targetEntry, then
 * return") is how a queued finalize discovers that a later synchronous navigation overtook it.
 *
 * HERE THE NAVIGABLE AND ITS TRAVERSABLE ARE ONE EVENT LOOP, so there is no second writer for the queue to
 * order against and the finalize runs in the turn that made it — which is the degenerate case of the standard's
 * own algorithm and not a shortcut past it. That is only true while every navigable using this file IS its own
 * traversable, which is exactly what sh_entries asserts: the case the queue is for is the case that crashes. */
static void sh_finalize_same_document_navigation(JSContext *ctx, JSValueConst target_entry,
                                                 JSValueConst entry_to_replace, const char *history_handling)
{
    JSValue entries = sh_entries(ctx), trec = sh_traversable_record(ctx), active;
    uint32_t current, target_step;

    /* STEP 2 — the race resolver. It is an assertion rather than a return for the reason above: with one event
       loop nothing can have overtaken this, and a silent return would leave an entry that is the navigable's
       active one and is in no list. */
    active = sh_active_entry(ctx);
    DCHECK(JS_VALUE_GET_PTR(active) == JS_VALUE_GET_PTR(target_entry),
           "§7.4.2.3.3's finalize found the navigable's active session history entry is not the entry it was "
           "given — §7.4.4 set it in the same turn, so a second writer exists and the SESSION HISTORY TRAVERSAL "
           "PARALLEL QUEUE (§7.4.1.3) has to be built for these steps to be ordered against it");
    JS_FreeValue(ctx, active);

    current = rec_uint(ctx, trec, SH_R_STEP);
    if (JS_IsNull(entry_to_replace)) {
        /* STEP 5's FIRST BRANCH — a PUSH. Clearing the forward history first is what makes a push after a
           traversal discard the entries the page went back past. */
        sh_clear_forward(ctx, entries, current);
        target_step = current + 1;
        JS_SetPropertyStr(ctx, (JSValue)target_entry, SH_E_STEP, JS_NewUint32(ctx, target_step));
        JS_SetPropertyUint32(ctx, entries, list_len(ctx, entries), JS_DupValue(ctx, target_entry));
    } else {
        /* STEP 5's SECOND BRANCH — a REPLACE. The new entry takes the replaced one's place AND its step, so
           the run stays contiguous and the current step still names it. */
        uint32_t n = list_len(ctx, entries), i, at = n;

        for (i = 0; i < n; i++) {
            JSValue e = JS_GetPropertyUint32(ctx, entries, i);
            bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(entry_to_replace);

            JS_FreeValue(ctx, e);
            if (same) { at = i; break; }
        }
        DCHECK(at < n, "§7.4.2.3.3's finalize was told to replace an entry that is not in the navigable's "
                       "session history entries — the entry to replace is the ACTIVE one, and an active entry "
                       "that is in no list is an entry §7.4.4 appended nowhere");
        target_step = sh_entry_step(ctx, entry_to_replace);
        JS_SetPropertyStr(ctx, (JSValue)target_entry, SH_E_STEP, JS_NewUint32(ctx, target_step));
        JS_SetPropertyUint32(ctx, entries, at, JS_DupValue(ctx, target_entry));
        /* THE NAVIGATION API KEY IS NOT CARRIED ACROSS, and that is the standard's own asymmetry rather than an
           omission here: §7.4.2.3.1's finalize-a-CROSS-document-navigation has the line ("if historyEntry's
           document state's origin is same origin with entryToReplace's … set historyEntry's navigation API key
           to entryToReplace's navigation API key") and §7.4.2.3.3's finalize — this one, the one a
           `replaceState` reaches — does not. So `replaceState` mints a fresh key, which is what the spec text
           says and what a reader coming from §7.2.6.5's prose about keys surviving a replace will not expect.
           Checked against the standard, not inferred; do not "fix" it without the line to point at. */
        /* "Set targetStep to traversable's current session history step" — a replace does not move it. */
        target_step = current;
    }
    JS_FreeValue(ctx, trec);
    JS_FreeValue(ctx, entries);
    /* STEP 6, "done even for replace navigations, as it resolves race conditions across multiple synchronous
       navigations". The HISTORY HANDLING travels with it because §7.4.6.1's apply-the-push/replace-history-step
       passes it on as the NavigationType, which §7.4.6.1's own switch and §7.4.6.2 step 6.4.1 both read. */
    DCHECK(history_handling != NULL && (!strcmp(history_handling, "push") || !strcmp(history_handling, "replace")),
           "§7.4.2.3.3's finalize was given a history handling behaviour that is neither \"push\" nor "
           "\"replace\" — those are the two §7.4.4 selects between, and the entry-to-replace argument above is "
           "the same choice spelled a second way");
    DCHECK(JS_IsNull(entry_to_replace) == (strcmp(history_handling, "push") == 0),
           "§7.4.2.3.3's finalize was told to push and handed an entry to replace, or told to replace and "
           "handed none — §7.4.4 step 5 derives the one from the other, so they cannot disagree");
    sh_apply_push_replace_history_step(ctx, target_step, history_handling);
}

/* ---- §7.4.6.2's RESTORE THE HISTORY OBJECT STATE -------------------------------------------------------------
 *
 * "Let state be StructuredDeserialize(entry's classic history API state, targetRealm). If this throws an
 * exception, catch it and let state be null." The catch is the standard's, and it is not reachable from a page:
 * the bytes were written by this engine's own serializer from a value it accepted, so a refusal on the way back
 * in is the engine disagreeing with itself — which structured_deserialize already asserts. */
static void sh_restore_history_object_state(JSContext *ctx, JSValueConst entry)
{
    JSValue buf = JS_GetPropertyStr(ctx, entry, SH_E_CLASSIC), rec;
    StructuredData d;
    JSValue state;

    d.buf = JS_GetArrayBuffer(ctx, &d.len, buf);
    DCHECK(d.buf != NULL, "§7.4.1.1's classic history API state held something that is not the serialized "
                          "bytes — every writer of the field is in this file and every one writes an "
                          "ArrayBuffer");
    state = structured_deserialize(ctx, &d);
    JS_FreeValue(ctx, buf);
    rec = sh_record(ctx);
    JS_SetPropertyStr(ctx, rec, SH_R_STATE, state);
    JS_FreeValue(ctx, rec);
}

/* ---- §7.4.4's URL AND HISTORY UPDATE STEPS -------------------------------------------------------------------- */

/* §7.4.4 STEP 4's "document's IS INITIAL about:blank" — the CONJUNCTION, and both halves are needed.
 *
 * §7.4's create-a-new-navigable makes every navigable with an initial about:blank Document at that address, and
 * core/frame/window_proxy.c's `ever_navigated` records the one operation that ever replaces a navigable's
 * active document. So a document addressed `about:blank` in a navigable nothing has navigated IS that Document,
 * and one addressed `about:blank` in a navigable that HAS been navigated is a different document that happens
 * to share the address — which is a state this tree really reaches, because §7.4 step 14's load navigates to
 * `about:blank` for real (navigable.c: "the corpus does it while an initial load is still pending"). Testing
 * the address alone would call that one initial and silently turn its pushState into a replaceState. */
static bool sh_is_initial_about_blank(JSContext *ctx)
{
    const char *url = document_url(ctx);

    if (!url || strcmp(url, "about:blank") != 0) return false;
    return !window_proxy_ever_navigated(document_window_proxy(ctx));
}

bool session_history_is_initial_about_blank(JSContext *ctx)
{
    return sh_is_initial_about_blank(ctx);
}

/* ---- WHAT §7.4.3's RELOAD READS OFF THE ACTIVE ENTRY — see session_history.h for why it is the ENTRY -------- */

JSValue session_history_active_entry_url(JSContext *ctx)
{
    JSValue e = sh_active_entry(ctx), url = JS_GetPropertyStr(ctx, e, SH_E_URL);

    JS_FreeValue(ctx, e);
    DCHECK(JS_IsString(url),
           "§7.4.1.1's session history entry held no URL — sh_entry_new writes one for every entry this "
           "component makes and the field has no other writer, so an entry without one was not built here");
    return url;
}

JSValue session_history_active_entry_navigation_state(JSContext *ctx)
{
    JSValue e = sh_active_entry(ctx), buf = JS_GetPropertyStr(ctx, e, SH_E_NAV_STATE);

    JS_FreeValue(ctx, e);
    DCHECK(JS_IsArrayBuffer(buf),
           "§7.4.1.1's navigation API state held something that is not the SERIALIZED bytes — every writer of "
           "the field is in this file and every one writes an ArrayBuffer");
    return buf;
}

uint32_t session_history_active_entry_step(JSContext *ctx)
{
    JSValue e = sh_active_entry(ctx);
    uint32_t step = sh_entry_step(ctx, e);

    JS_FreeValue(ctx, e);
    return step;
}

/* §7.4.4's TWO HALVES — see session_history.h for where the split is and why it is there.
 *
 * ONE REST POINT, which is step 11's request. Steps 12 and 13 after it are the finalize and the
 * apply-the-push/replace-history-step, both of which take §7.4.6.1's update-only exit and run none of the
 * page's code (sh_apply_push_replace_history_step asserts exactly that, and its assertion is why the whole tail
 * can be a plain call). */
void session_history_url_update_start(SessionHistoryUrlUpdate *w)
{
    /* A zeroed JSValue is the INTEGER 0, not undefined — the same rule core/streams/stream_work.h states, and
       the same one this record would be read through if a caller forgot. */
    w->push = false;
    w->new_entry = w->to_replace = JS_UNDEFINED;
    navigation_update_work_start(&w->nav);
}

void session_history_url_update_visit(JSContext *ctx, SessionHistoryUrlUpdate *w, JSStepVisit *v)
{
    v->val(ctx, &w->new_entry);
    v->val(ctx, &w->to_replace);
    navigation_update_work_visit(ctx, &w->nav, v);
}

void session_history_url_update_release(JSContext *ctx, SessionHistoryUrlUpdate *w)
{
    JS_FreeValue(ctx, w->new_entry);
    JS_FreeValue(ctx, w->to_replace);
    w->new_entry = w->to_replace = JS_UNDEFINED;
    navigation_update_work_release(ctx, &w->nav);
}

void session_history_url_update_begin(JSContext *ctx, SessionHistoryUrlUpdate *w, const char *new_url,
                                      const StructuredData *serialized, bool push)
{
    JSValue active, doc_state, classic, nav_state, rec;
    const char *scroll;
    JSValue scroll_v;
    StructuredData undef;

    DCHECK(g_slot >= 0, "§7.4.4's URL and history update steps ran before session_history_init declared the "
                        "record");
    DCHECK(new_url != NULL, "§7.4.4 runs with a URL — its step 2 defaults it to the document's own address, so "
                            "the caller always has one");
    DCHECK(JS_IsUndefined(w->new_entry),
           "§7.4.4's _begin ran twice over one work record — the algorithm builds ONE entry and the second "
           "would leave the first as the navigable's active entry and in no list");
    /* STEPS 1-3: the new entry, with the ACTIVE entry's DOCUMENT STATE — shared by reference, which is what
       §7.4.1.2's "several contiguous entries in a session history can share the same document state" is, and
       what makes ten pushState calls ten entries of ONE Document rather than ten documents. */
    active = sh_active_entry(ctx);
    doc_state = JS_GetPropertyStr(ctx, active, SH_E_DOCSTATE);
    DCHECK(JS_IsObject(doc_state), "a session history entry held no §7.4.1.2 document state");
    scroll_v = JS_GetPropertyStr(ctx, active, SH_E_SCROLL);
    scroll = JS_ToCString(ctx, scroll_v);
    CHECK(scroll != NULL, "session history: an entry's scroll restoration mode could not be read");
    if (serialized)
        classic = sh_state_buffer(ctx, serialized);
    else
        classic = JS_GetPropertyStr(ctx, active, SH_E_CLASSIC);   /* "otherwise activeEntry's classic … state" */
    /* §7.4.4's new entry names FIVE fields and the navigation API state is not one of them, so it takes
       §7.4.1.1's initial value — serialized UNDEFINED — rather than the active entry's. That is why
       `history.pushState(x, "")` leaves `navigation.currentEntry.getState()` undefined while
       `navigation.navigate(url, {state:x})` does not: the two stores are unrelated, which §7.2.6.5 says. */
    sh_serialize_primitive(ctx, JS_UNDEFINED, &undef);
    nav_state = sh_state_buffer(ctx, &undef);
    structured_data_free(ctx, &undef);
    w->new_entry = sh_entry_new(ctx, new_url, doc_state, classic, nav_state, scroll);
    JS_FreeCString(ctx, scroll);
    JS_FreeValue(ctx, scroll_v);

    /* STEP 4: "if document's is initial about:blank is true, then set historyHandling to replace". The
       standard's own note — "this means that pushState() on an initial about:blank Document behaves as a
       replaceState() call". */
    if (sh_is_initial_about_blank(ctx)) push = false;
    w->push = push;
    /* STEP 5. */
    w->to_replace = push ? JS_NULL : JS_DupValue(ctx, active);

    rec = sh_record(ctx);
    /* STEP 6's TEMPORARY BEST-GUESS VALUES, "for immediate synchronous access" — a page reading
       `history.length` on the line after a pushState reads these, and §7.4.6.1's apply-the-push/replace step
       overwrites them with the real ones. They are no longer merely notional: step 11 below RUNS THE PAGE'S
       CODE, so a `currententrychange` listener really does observe the algorithm between these two writes,
       which is exactly the state the standard describes them as holding. */
    if (push) {
        uint32_t index = rec_uint(ctx, rec, SH_R_INDEX) + 1;

        rec_set_uint(ctx, rec, SH_R_INDEX, index);
        rec_set_uint(ctx, rec, SH_R_LENGTH, index + 1);
    }
    /* STEP 7. */
    if (serialized) sh_restore_history_object_state(ctx, w->new_entry);
    /* STEP 8: "set the URL given document to newURL". The standard's note is why nothing is fired here —
       "since this is neither a navigation nor a history traversal, it does not cause a hashchange event to be
       fired".
       AND IT IS THE ONE PLACE AN APPLICATION SAYS "THIS ADDRESS IS A PAGE OF MINE". §7.4.4's own opening
       sentence is why the observation belongs to this algorithm rather than to §7.2.5's members: "session
       history entries can be pushed or replaced via one more mechanism, the URL and history update steps. The
       most well-known callers of these steps are the history.replaceState() and history.pushState() APIs, but
       various other parts of the standard also need to perform updates to the active history entry, and they
       use these steps to do so." So a caller added later declares its route through this same line, and there
       is no second site for the question to go unasked at. solver/route_seed.h holds what a declaration IS,
       why running the page's own call to a routing member is not the string-matching §RUN-DON'T-MATCH forbids,
       and why the address leaves as a one-way notice rather than as a park.
       BEFORE THE SET, because the declaration is that the address CHANGED and the comparison needs both sides
       — the same "an operation takes its inputs with it" the loader's own job already turns on. */
    route_seed_declare(ctx, document_url(ctx), new_url);
    document_set_url(ctx, new_url);
    /* STEPS 9 AND 10 — the Document's latest entry and the NAVIGABLE's active session history entry. Both are
       set before the finalize below, which is what makes its step 2 hold. */
    JS_SetPropertyStr(ctx, rec, SH_R_LATEST, JS_DupValue(ctx, w->new_entry));
    JS_SetPropertyStr(ctx, rec, SH_R_ACTIVE, JS_DupValue(ctx, w->new_entry));
    JS_FreeValue(ctx, rec);
    JS_FreeValue(ctx, active);
}

int session_history_url_update_run(JSContext *ctx, SessionHistoryUrlUpdate *w, JSValue in,
                                   JSValue **out_cb, int *out_argc)
{
    int r;

    DCHECK(JS_IsObject(w->new_entry),
           "§7.4.4's _run reached a work record with no entry — _begin builds it and must run first");
    /* STEP 11: "UPDATE THE NAVIGATION API ENTRIES FOR A SAME-DOCUMENT NAVIGATION given document's relevant
       global object's navigation API, newEntry, and historyHandling." This is the step that makes a pushState
       run the page's code, and its position is load-bearing: it is BEFORE the finalize, so a
       `currententrychange` listener sees the Document's URL and `history.state` already changed while the
       entry is not yet in §7.4.1's list and `history.length` still holds step 6's best guess. */
    r = navigation_update_entries_run(ctx, &w->nav, w->new_entry, w->push ? "push" : "replace", in,
                                      out_cb, out_argc);
    if (r != 0) return r;
    /* STEPS 12-13. */
    sh_finalize_same_document_navigation(ctx, w->new_entry, w->to_replace, w->push ? "push" : "replace");
    return 0;
}

/* ---- §7.4.2.3.3's NAVIGATE TO A FRAGMENT ----------------------------------------------------------------------
 *
 * THE OTHER SYNCHRONOUS HISTORY UPDATE, AND IT SITS HERE SO THE TWO CAN BE READ AGAINST EACH OTHER. §7.4.4's
 * own closing note is the comparison: "although both fragment navigation and the URL and history update steps
 * perform synchronous history updates, only fragment navigation contains a synchronous call to UPDATE DOCUMENT
 * FOR HISTORY STEP APPLICATION. The URL and history update steps instead perform a few select updates inside
 * the above algorithm, omitting others. This is somewhat of an unfortunate historical accident … For example,
 * this means that popstate events fire for fragment navigations, but not for history.pushState() calls."
 *
 * SO THE STEPS ABOVE ARE NOT REUSED AND MUST NOT BE. Four of their differences are observable and none of them
 * is a refinement: this algorithm fires a navigate event of its own (step 4) where §7.2.5's caller fires
 * §7.4.4's; its entry carries the ACTIVE entry's NAVIGATION API STATE where §7.4.4's takes §7.4.1.1's initial
 * serialized `undefined`; it sets neither the Document's LATEST ENTRY nor `history.state` from a serialization,
 * because §7.4.6.2 steps 6.2 and 6.3 do both when step 14 calls it; and it has no is-initial-about:blank step,
 * because §7.4.2.2 resolved historyHandling before it branched here.
 *
 * ITS ENTRY IS PER-FLOW STATE AND THAT IS THE WHOLE POINT. Two forked flows exploring two routes each assign
 * `location.hash` and each appends its own entry; the entries are ordinary JS objects in a JS array on the
 * per-realm record, so every append, every step assignment and the active-entry write are PROPERTY WRITES the
 * heap COW delta already captures. One flow's route change is therefore invisible to its sibling, and a flow
 * parked mid-`popstate` resumes with the history it built and not with the one that overtook it. A malloc'd
 * list here would revert its head/tail POINTERS on a context switch and leave the entries reachable from
 * nothing, which is the leak the runtime's own GC walk cannot see. */
#define SHFRAG_STAGES(X)                                                                                       \
    X(SHFRAG_EVENT,  "HTML §7.4.2.3.3 navigate to a fragment steps 1-4 (the destination navigation API state, " \
                     "then firing a push/replace/reload navigate event at the navigation API with "             \
                     "isSameDocument set to true)")                                                             \
    X(SHFRAG_UPDATE, "HTML §7.4.2.3.3 navigate to a fragment steps 5-14 (the event's answer, the new session "  \
                     "history entry, the history object's best-guess index and length, the document's URL, "    \
                     "the navigable's active session history entry, and update document for history step "      \
                     "application)")                                                                            \
    X(SHFRAG_TAIL,   "HTML §7.4.2.3.3 navigate to a fragment steps 15-17 (scroll to the fragment, and the "     \
                     "queued finalize-a-same-document-navigation)")
enum { SHFRAG_STAGES(JS_STEP_STAGE_ENUM) };
/* THERE IS NO `steps` ARRAY HERE, and that is what SH_APPLY_STAGES does too: the array is materialized where a
   JSTrampStepDef declares it, and no def declares THIS algorithm — the MEMBER that drives it declares its own
   stages, and its label for the one that drives this names the algorithm rather than restating its steps. The
   X-list is where the labels live either way, and STEP_DISPATCH is generated from it. */
/* ONE NAME FOR THE ALGORITHM THESE STAGES ARE STEPS OF — the dispatch's abort reads it, so a record entered at
   a stage it does not declare says which algorithm's stage list it was measured against. */
#define SHFRAG_ALGORITHM "HTML §7.4.2.3.3 navigate to a fragment"

void session_history_fragment_nav_start(SessionHistoryFragmentNav *w)
{
    int k;

    /* A zeroed JSValue is the INTEGER 0, not undefined, so every slot the visit walks is started here — on the
       one call that precedes every stage — rather than by whichever stage happens to write it first. */
    w->stage = SHFRAG_EVENT;
    w->history_handling = NULL;
    w->url = w->history_entry = w->to_replace = JS_UNDEFINED;
    navigate_event_fire_work_start(&w->fire);
    /* §7.4.6.1's continuation state has no _start of its own — sh_apply_history_step_begin is the only other
       producer and it is §7.4.6.1's entry, which this algorithm is not — so its slots are started here, on the
       same one call. Step 14 below writes the rest of them out of values steps 6-13 compute. */
    w->apply.target_entry = w->apply.displayed_entry = JS_UNDEFINED;
    w->apply.old_url = w->apply.popstate = JS_UNDEFINED;
    STEP_CB_FOREACH(w->apply.fire_cb, k) w->apply.fire_cb[k] = JS_UNDEFINED;
    w->apply.fire_phase = 0;
    w->apply.stage = 0;
    w->apply.update_only = false;
    w->apply.navigation_type = NULL;
    w->apply.target_step = w->apply.begin_step = w->apply.length = w->apply.index = 0;
    navigation_update_work_start(&w->apply.nav);
}

void session_history_fragment_nav_visit(JSContext *ctx, SessionHistoryFragmentNav *w, JSStepVisit *v)
{
    int k;

    v->val(ctx, &w->url);
    v->val(ctx, &w->history_entry);
    v->val(ctx, &w->to_replace);
    navigate_event_fire_work_visit(ctx, &w->fire, v);
    v->val(ctx, &w->apply.target_entry);
    v->val(ctx, &w->apply.displayed_entry);
    v->val(ctx, &w->apply.old_url);
    v->val(ctx, &w->apply.popstate);
    STEP_CB_FOREACH(w->apply.fire_cb, k) v->val(ctx, &w->apply.fire_cb[k]);
    navigation_update_work_visit(ctx, &w->apply.nav, v);
}

void session_history_fragment_nav_begin(JSContext *ctx, SessionHistoryFragmentNav *w, const char *url,
                                        const char *history_handling)
{
    DCHECK(g_slot >= 0, "§7.4.2.3.3's navigate to a fragment ran before session_history_init declared the "
                        "record");
    DCHECK(url != NULL && *url, "§7.4.2.3.3 was begun with no destination — §7.4.2.2's fourth conjunct is that "
                                "the URL has a non-null FRAGMENT, so a caller that reached this branch has one");
    DCHECK(history_handling != NULL &&
           (!strcmp(history_handling, "push") || !strcmp(history_handling, "replace")),
           "§7.4.2.3.3 was begun with a history handling behaviour that is neither \"push\" nor \"replace\" — "
           "§7.4.2.2's step 9 resolves \"auto\" to one of the two before it branches here, and its \"reload\" "
           "reaches §7.4.3 and never this algorithm");
    DCHECK(JS_IsUndefined(w->url),
           "§7.4.2.3.3's _begin ran twice over one work record — the algorithm builds ONE entry, and the second "
           "would leave the first as the navigable's active entry and in no list");
    DCHECK(session_history_is_fragment_navigation(ctx, url),
           "§7.4.2.3.3 was begun for a destination §7.4.2.2's own test says is NOT a fragment navigation — the "
           "four conjuncts are what selects this algorithm, so a caller that asked a different question has "
           "routed a CROSS-DOCUMENT navigation into the one algorithm that never fetches");
    /* THE DESTINATION IS TAKEN WITH THE OPERATION, as a string, and it is never read back off the navigable.
       Between the navigate event below and step 12's write every `navigate` listener the page has runs, and any
       of them may push an entry or change the address — so a re-read on resume would resolve this navigation
       against whatever a listener left behind. */
    w->url = JS_NewString(ctx, url);
    CHECK(!JS_IsException(w->url), "session history: §7.4.2.3.3's destination could not be held across its "
                                   "navigate event");
    w->history_handling = history_handling;
}

int session_history_fragment_nav_run(JSContext *ctx, SessionHistoryFragmentNav *w, JSValue in,
                                     JSValue **out_cb, int *out_argc)
{
    JSValue rec, active;
    const char *url;
    int r;

    STEP_DISPATCH(SHFRAG_STAGES, w->stage, SHFRAG_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(SHFRAG_EVENT);
    DCHECK(JS_IsString(w->url), "§7.4.2.3.3's _run reached a work record with no destination — _begin takes it "
                                "and must run first");
    /* STEP 1 is "let navigation be navigable's active window's NAVIGATION API" — this realm's, which
       core/frame/navigation.c answers for; the steps below reach it through that component rather than holding
       one across the dispatch.
       STEPS 2-3: "let destinationNavigationAPIState be navigable's ACTIVE session history entry's navigation
       API state. If navigationAPIState is not null, then set destinationNavigationAPIState to
       navigationAPIState." Every caller here is a navigation, not `navigation.navigate(url, {state})`, so the
       argument is null and step 3 does not fire — which is the standard's own note: "for other fragment
       navigations, including user-initiated ones, the navigation API state is CARRIED OVER from the previous
       entry". It is carried at step 6 below, straight off the active entry — AND IT IS ALSO STEP 4's, which
       this site used to leave out: the sentence above accounted for the ENTRY the algorithm builds and said
       nothing about the NAVIGATE EVENT it fires first, so the page's own `navigate` listener was handed
       `event.destination.getState()` of the serialization of `null` for a state the page itself had put on
       the entry. Reading it HERE and not at step 6 is the same rule the destination above is taken by: every
       `navigate` listener runs between the two, and any of them may make a different entry active.
       STEP 4: "let continue be the result of FIRING A PUSH/REPLACE/RELOAD NAVIGATE EVENT at navigation with
       navigationType set to historyHandling, isSameDocument set to TRUE, … destinationURL set to url, and
       navigationAPIState set to destinationNavigationAPIState."
       ITS classicHistoryAPIState IS NULL AND THAT IS THE SPEC'S OWN ASYMMETRY, not an omission: §7.2.5's
       pushState passes the bytes it serialized and this wrapper has none to pass, which is the same sentence
       §7.4.1.1 states about the entry ("the classic history API state is never carried over"). */
    url = JS_ToCString(ctx, w->url);
    CHECK(url != NULL, "session history: §7.4.2.3.3's destination could not be read");
    {
        JSValue dest_state = session_history_active_entry_navigation_state(ctx);

        navigate_event_fire_push_replace_reload_begin(ctx, &w->fire, w->history_handling, url,
                                                      /*is_same_document*/ true, /*classic_state*/ NULL,
                                                      dest_state);
        JS_FreeValue(ctx, dest_state);
    }
    JS_FreeCString(ctx, url);
    /* AND IT RETURNS RATHER THAN RUNNING ON: the dispatch is the next stage, and a body that sets its stage and
       falls into the next arm has crossed a boundary the driver never saw. `in` is this entry's request answer
       and nothing below has asked for one, so it is released here rather than carried across the rest point. */
    JS_FreeValue(ctx, in);
    STEP_GOTO(w->stage, SHFRAG_UPDATE, &w->fire.phase, &w->fire.abort.phase, &w->fire.abort.sig.phase,
              &w->apply.nav.phase, &w->apply.fire_phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(SHFRAG_UPDATE);
    /* WHAT THIS TEST IS, AND WHAT IT IS NOT. It is not "have I started" — a stage can never answer that, and a
       machine that needs one keeps a byte. It asks whether STEP 14's continuation state has been BUILT, which
       is a fact about a field with exactly one writer: `target_entry` is JS_UNDEFINED until step 14's arguments
       are assembled and an object for ever after. On the first entry to this stage the navigate event has just
       answered and steps 5-13 have not run; on every resume through §7.4.6.2's own three rest points it has,
       and none of those steps runs twice. */
    if (JS_IsUndefined(w->apply.target_entry)) {
        bool proceed = false;

        /* §7.2.6.10.4 answers, having dispatched `navigate` at this realm's Navigation and run every listener
           the page registered for it. */
        r = navigate_event_fire_run(ctx, &w->fire, in, out_cb, out_argc, &proceed);
        if (r != 0) return r;
        in = JS_UNDEFINED;
        /* STEP 5: "if continue is FALSE … return." A router's `navigate` listener called preventDefault(), so
           the address does not move, no entry is appended, and the member that asked answers exactly as it does
           when the navigation succeeded — §7.2.4's setters have no return value and this return is not an
           error.
           STEP 5.1 IS NOT WRITTEN, AND THAT IS THE ABSENCE OF A READER RATHER THAN A DEFERRED STEP. It sets the
           navigable's ONGOING NAVIGATION to this navigation's id, and the standard's own note says what that is
           for: "this makes intercepted hash navigations cancelable by browser UI or window.stop()". This build
           has no browser UI, and its `window.stop()` is §7.2.2.1's documented no-effect body (window.c's
           js_win_noeffect, whose comment gives the same reason — there is no in-flight navigation to abort
           because §7.4.2.5's ABORT A NAVIGATION is not built). A field written here would be a field with one
           writer and no reader, which is the shape that makes an unwritten producer indistinguishable from a
           measured one. The two land together: §7.4.2.5's abort, and the navigable's ongoing-navigation field
           it reads — at which point `stop()` stops sharing the no-effect body and this line writes it. */
        if (!proceed) return 0;
        /* STEP 6: "let historyEntry be a NEW SESSION HISTORY ENTRY, with URL url, document state navigable's
           active session history entry's DOCUMENT STATE, navigation API state destinationNavigationAPIState,
           and scroll restoration mode navigable's active session history entry's scroll restoration mode."
           THE DOCUMENT STATE IS SHARED BY REFERENCE, which is §7.4.1.2's "several contiguous entries in a
           session history can share the same document state" and is what makes ten hash routes ten entries of
           ONE Document rather than ten documents. THE CLASSIC HISTORY API STATE IS NOT IN THE LIST, so the
           entry takes §7.4.1.1's initial value — StructuredSerializeForStorage(NULL) — which is the standard's
           "the classic history API state is never carried over", and is why a hash route nulls `history.state`
           even when the entry it left carried one. */
        active = sh_active_entry(ctx);
        {
            JSValue doc_state = JS_GetPropertyStr(ctx, active, SH_E_DOCSTATE);
            JSValue nav_state = JS_GetPropertyStr(ctx, active, SH_E_NAV_STATE);
            JSValue scroll_v = JS_GetPropertyStr(ctx, active, SH_E_SCROLL);
            const char *scroll = JS_ToCString(ctx, scroll_v);
            StructuredData null_state;
            const char *dest;

            DCHECK(JS_IsObject(doc_state), "a session history entry held no §7.4.1.2 document state");
            CHECK(scroll != NULL, "session history: an entry's scroll restoration mode could not be read");
            sh_serialize_primitive(ctx, JS_NULL, &null_state);
            dest = JS_ToCString(ctx, w->url);
            CHECK(dest != NULL, "session history: §7.4.2.3.3's destination could not be read back after the "
                                "navigate event");
            w->history_entry = sh_entry_new(ctx, dest, doc_state, sh_state_buffer(ctx, &null_state), nav_state,
                                            scroll);
            JS_FreeCString(ctx, dest);
            structured_data_free(ctx, &null_state);
            JS_FreeCString(ctx, scroll);
            JS_FreeValue(ctx, scroll_v);
        }
        /* STEP 7: "let entryToReplace be navigable's active session history entry if historyHandling is
           'replace', otherwise null." */
        w->to_replace = strcmp(w->history_handling, "replace") == 0 ? JS_DupValue(ctx, active) : JS_NULL;
        /* STEPS 8-11. The history object's index and length are read BEFORE they are adjusted, which is what
           makes them §7.4.4 step 6's "temporary best-guess values for immediate synchronous access" — a
           `popstate` listener really does observe them, because step 14 below runs it. Step 11's THREE
           sub-steps fire only for a push: `history.state` is nulled, the index is incremented, and the length
           becomes the index + 1. The null is not redundant with the entry's initial classic state: §7.4.6.2
           step 6.3 restores the state from the entry a moment later, and between here and there a `navigate`
           listener has already returned and the popstate has not yet been minted. */
        rec = sh_record(ctx);
        w->apply.index = rec_uint(ctx, rec, SH_R_INDEX);
        w->apply.length = rec_uint(ctx, rec, SH_R_LENGTH);
        if (strcmp(w->history_handling, "push") == 0) {
            JS_SetPropertyStr(ctx, rec, SH_R_STATE, JS_NULL);
            w->apply.index += 1;
            w->apply.length = w->apply.index + 1;
        }
        /* STEP 12: "set navigable's active document's URL to url". It moves BEFORE step 14 fires anything, so
           a `popstate` listener reading `location.href` sees the new address — which is the whole reason a
           router listens for it. */
        {
            const char *dest = JS_ToCString(ctx, w->url);

            CHECK(dest != NULL, "session history: §7.4.2.3.3's destination could not be read for step 12");
            document_set_url(ctx, dest);
            JS_FreeCString(ctx, dest);
        }
        /* STEP 13: "set navigable's ACTIVE session history entry to historyEntry" — and NOT its current entry
           and not the entries list, which is the standard's own worked example: "note that this does not yet
           update the current session history entry, current session history step, or the session history
           entries list; those updates cannot be done synchronously, and instead must be done as part of the
           queued steps". Step 17's finalize is those queued steps. */
        JS_SetPropertyStr(ctx, rec, SH_R_ACTIVE, JS_DupValue(ctx, w->history_entry));
        JS_FreeValue(ctx, rec);
        /* STEP 14's ARGUMENTS, ONTO §7.4.6.1's CONTINUATION STATE: "update document for history step
           application given navigable's active document, historyEntry, TRUE, scriptHistoryIndex,
           scriptHistoryLength, and historyHandling". The third argument is doNotReactivate, which is the field
           §7.4.6.1 calls update-only; the Document is this realm's, so it is not a field (sh_entries asserts
           the one place a second navigable would matter).
           `displayed_entry` IS THE ENTRY THIS ALGORITHM JUST REPLACED, because §7.4.6.2 step 7 is the only
           reader and what it wants is previousEntryForActivation. Reading the ACTIVE entry here would answer
           with historyEntry, which step 13 has just installed, and the step-7 assertion would then be checking
           the new entry against itself. */
        w->apply.displayed_entry = active;         /* the reference sh_active_entry returned, kept */
        w->apply.target_entry = JS_DupValue(ctx, w->history_entry);
        w->apply.update_only = true;
        w->apply.navigation_type = w->history_handling;
        /* `target_step` AND `begin_step` ARE §7.4.6.1's OWN AND STAY AT THEIR START VALUES, because step 14 is
           not §7.4.6.1: the only reader of either is sh_apply_history_step_finish, which the finalize at step
           17 reaches through its OWN continuation state and not through this one. A step 17 that reused this
           record would be applying a history step whose target step nobody computed. */
    }
    /* STEP 14, DRIVEN. §7.4.6.2 owns three rest points of its own — the navigation API's `currententrychange`
       and `dispose`, then the `popstate` dispatch — and it re-enters itself at whichever of them the record
       holds, so a resume at this stage runs none of the steps above again; the `target_entry` test above is
       what keeps steps 5-13 on the first entry only.
       THE hashchange IS QUEUED AND THE popstate IS NOT, and that asymmetry is §7.4.6.2's own: step 6.4.3 fires
       `popstate` synchronously at the Window, and step 6.4.5 QUEUES A GLOBAL TASK on the DOM manipulation task
       source to fire `hashchange`. A router that listens for both therefore sees popstate inside its own
       assignment and hashchange in a later task — which is observable, is what every browser does, and is why
       neither may be turned into the other here. */
    r = sh_update_document_for_history_step(ctx, &w->apply, in, out_cb, out_argc);
    if (r != 0) return r;
    STEP_GOTO(w->stage, SHFRAG_TAIL, &w->fire.phase, &w->fire.abort.phase, &w->fire.abort.sig.phase,
              &w->apply.nav.phase, &w->apply.fire_phase, NULL);
    return JS_STEP_YIELD;

    STEP_ARM(SHFRAG_TAIL);
    JS_FreeValue(ctx, in);
    /* STEP 15: "SCROLL TO THE FRAGMENT given navigable's active document" — §7.4.6.4, which is more than a
       scroll: it sets the Document's TARGET ELEMENT (the `:target` pseudo-class), runs the ancestor revealing
       algorithm, runs the FOCUSING STEPS for that element and moves the sequential focus navigation starting
       point. None of it has a producer in this build, and the assertion is asked against the member whose
       arrival makes the algorithm expressible at all rather than being written down as a claim. */
    realm_awaits(ctx, "scrollTo",
                 "HTML §7.4.2.3.3 step 15 SCROLLS TO THE FRAGMENT after a hash route, and §7.4.6.4 is four "
                 "observable things and not one: it sets the Document's TARGET ELEMENT from its INDICATED PART "
                 "(which is what `:target` selects, and what §6.6.7's flush autofocus candidates step 4 reads "
                 "— core/html/autofocus.c asserts against this same member for it), runs the ancestor "
                 "revealing algorithm, runs §6.6.3's focusing steps for that element with the viewport as the "
                 "fallback target, and moves the sequential focus navigation starting point. This build now "
                 "has a way to scroll a scrolling box: write §7.4.6.4 in core/rendering, give the Document its "
                 "target element and its indicated part (§7.4.6.4's own definition: the node the URL's "
                 "fragment identifies, or null), and call it here and at §7.4.6.2 step 8's try-to-scroll");
    /* STEPS 16-17: "let traversable be navigable's TRAVERSABLE NAVIGABLE. Append the following session history
       SYNCHRONOUS NAVIGATION STEPS involving navigable to traversable: finalize a same-document navigation …;
       invoke WebDriver BiDi fragment navigated …". The finalize is what gives the entry its STEP and its place
       in the list, and it is the same one §7.4.4 step 13 reaches — one implementation, so a fragment route and
       a `pushState` cannot disagree about what a push after a traversal discards.
       IT RUNS IN THIS TURN, which is the degenerate case of the standard's queue and not a shortcut past it:
       the navigable and its traversable are one event loop here (sh_entries asserts it), so there is no second
       writer for the queue to order against, and its step 2 — "if targetNavigable's active session history
       entry is not targetEntry, then return" — is the assertion inside the finalize rather than a return.
       There is no WebDriver BiDi in this build at all; it is a remote-control protocol for a browser under
       test, and every one of its invocations in §7.4 is a report to an agent that does not exist. */
    sh_finalize_same_document_navigation(ctx, w->history_entry, w->to_replace, w->history_handling);
    /* AND THE ALGORITHM IS BACK AT ITS FIRST STEP: the record a caller holds names the step it would be
       re-entered at rather than the last one it rested at. */
    STEP_GOTO(w->stage, SHFRAG_EVENT, &w->fire.phase, &w->fire.abort.phase, &w->fire.abort.sig.phase,
              &w->apply.nav.phase, &w->apply.fire_phase, NULL);
    return 0;
}

/* ---- §7.4.3's TRAVERSE THE HISTORY BY A DELTA, AS A JOB -------------------------------------------------------
 *
 * IT IS A WORK ITEM AND THE STANDARD SAYS SO: every step of §7.4.3 after the first three is inside "APPEND THE
 * FOLLOWING SESSION HISTORY TRAVERSAL STEPS to traversable", so `history.back()` returns to its caller having
 * scheduled the traversal and not having performed it — which is why a page that calls `back()` and then reads
 * `location.href` on the next line still reads the address it was already at.
 *
 * AND THE DELTA IS RESOLVED IN THE JOB, NOT AT THE CALL. §7.4.3's steps 4.1-4.3 read the traversable's CURRENT
 * session history step when the queued steps run, so `history.back(); history.back();` in one turn is two
 * traversals resolved against two different current steps and goes back TWO — where a delta resolved at the call
 * would resolve both against the same step and go back one. That is the whole reason this component's resolution
 * lives inside the machine below rather than in §7.2.5's member.
 *
 * IT SUSPENDS BECAUSE §7.4.6.2 FIRES popstate, and a dispatch runs the page's listeners: the machine parks on
 * the fire (event_target_fire_run, the request reach into §2.9), sibling flows run while a listener's own loop
 * or `await` is in flight, and the traversal resumes at the exact step it rested at — SH_APPLY_STAGES names
 * which one. It is a flow on the ONE frontier, not a driver: nothing here loops over anything.
 *
 * THE MACHINE OWNS NOTHING BUT THE CONTINUATION STATE. Its `visit` walks SessionHistoryApply's five value
 * fields plus the fire request's buffer, which is the same list `fini` releases — one list, so a field added to the struct is
 * added to both by being added to sh_apply_free and to this. */
typedef struct {
    JSStepHdr           hdr;   /* FIRST — the driver writes the def and the operand bounds through it */
    SessionHistoryApply apply;
} SHTraverseState;

static void js_sh_traverse_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    SHTraverseState *s = st;
    int k;

    v->val(ctx, &s->apply.target_entry);
    v->val(ctx, &s->apply.displayed_entry);
    v->val(ctx, &s->apply.old_url);
    v->val(ctx, &s->apply.popstate);
    STEP_CB_FOREACH(s->apply.fire_cb, k)
        v->val(ctx, &s->apply.fire_cb[k]);
    navigation_update_work_visit(ctx, &s->apply.nav, v);
}

static int js_sh_traverse_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    SHTraverseState *s = st;
    uint32_t target = 0;
    int32_t delta = 0;
    int r;

    STEP_DISPATCH(SH_APPLY_STAGES, s->hdr.stage, SH_APPLY_ALGORITHM, JS_STEP_ABRUPT);

    STEP_ARM(SH_APPLY_STEP);
    {
        /* STEPS 4.1-4.4. `delta` arrived as the job's one argument, already converted by §7.2.5's `optional long
           delta = 0` declaration — a `long` is 32 bits and signed, which is why the arithmetic below is. */
        DCHECK(JS_IsNumber(step_arg(&s->hdr, 0)),
               "§7.4.3's traversal job was given a delta that is not a number — §7.2.5 declares `optional long "
               "delta = 0`, so Web IDL converted it before the member enqueued this");
        JS_ToInt32(ctx, &delta, step_arg(&s->hdr, 0));
        DCHECK(delta != 0, "§7.4.3's traversal job was enqueued with a delta of 0 — §7.2.5's delta traverse "
                           "step 4 answers that with a RELOAD and returns before it ever reaches §7.4.3");
        if (!sh_resolve_delta(ctx, delta, &target)) {
            /* STEP 4.4: "if allSteps[targetStepIndex] does not exist, then ABORT THESE STEPS." `back()` at the
               first entry and `forward()` at the last both land here, and both are no-ops in every browser. */
            JS_FreeValue(ctx, cb_result);
            return JS_STEP_DONE;
        }
        /* STEP 4.5: "APPLY THE TRAVERSE HISTORY STEP allSteps[targetStepIndex] to traversable" — which is
           §7.4.6.1's apply-the-history-step "given TRUE, sourceSnapshotParams, initiatorToCheck,
           userInvolvement, and \"traverse\"". THAT FIRST ARGUMENT IS `checkForCancelation`, and this is the
           only entry point in this file that passes it true — apply-the-push/replace-history-step passes false
           and says why ("those checks are done earlier in the navigation algorithm").
           SO §7.4.6.1 STEP 5 BELONGS HERE, and it is the step that fires the TRAVERSE NAVIGATE EVENT for a
           top-level traversable: check-if-unloading-is-canceled's own branch is "if traversable was given",
           which is satisfied whatever navigablesCrossingDocuments holds, so a same-document `history.back()`
           reaches it. A FALSE answer is "canceled-by-navigate" and the traversal must not apply at all.
           THE DISPATCH IS A REST POINT, so building it adds a stage to SH_APPLY_STAGES ahead of SH_APPLY_STEP
           and this call moves behind it. The INNER navigate event firing algorithm now exists — it is
           core/frame/navigate_event_fire.c, and §7.2.5's pushState drives it — so what is missing here is the
           TRAVERSE WRAPPER and this site's stage, which is what the assertion says. It probes `onnavigate`
           because that is the observable of a navigate event being fired at all, and this file's traversal is
           now the one navigation in the build that fires none. */
        realm_awaits(ctx, "Navigation.prototype.onnavigate",
                     "HTML §7.4.6.1 step 5 CHECKS IF UNLOADING IS CANCELED before it applies a traverse history "
                     "step, and this build fires the navigate event everywhere ELSE: a `history.pushState()` "
                     "asks the page's `navigate` listeners and a `history.back()` does not, which is the "
                     "inconsistency this fires on. Write §7.2.6.10.4's FIRE A TRAVERSE NAVIGATE EVENT beside "
                     "the push/replace/reload wrapper in core/frame/navigate_event_fire.c: it takes the entry "
                     "sh_target_history_entry resolved for `target` and builds a NavigationDestination over "
                     "that entry's URL, over its NavigationHistoryEntry (navigation_entry_index_of finds it in "
                     "the Navigation's entry list, and a destination whose entry is non-null is what makes the "
                     "inner algorithm's step 3 reachable — the upcoming traverse API method trackers, which "
                     "arrive with §7.2.6.7's traverseTo), over its navigation API state, and with is-same-"
                     "document set from whether the entry's document is this one. The inner algorithm then "
                     "needs the two things only a \"traverse\" reaches: step 8's traverseCanBeCanceled, whose "
                     "third conjunct is HTML §6.6's HISTORY-ACTION ACTIVATION, and step 28's consume-history-"
                     "action-user-activation. A FALSE answer is \"canceled-by-navigate\": return JS_STEP_DONE "
                     "without calling sh_apply_history_step_begin at all, so a traversal a listener canceled "
                     "leaves the traversable's current step where it was. THE STAGE IS THE OTHER HALF — the "
                     "dispatch is a rest point, and SH_APPLY_STAGES is dispatched over by three functions, so "
                     "the new stage needs an arm in each of them");
        sh_apply_history_step_begin(ctx, &s->apply, target, "traverse");
    }
    /* THE SAME SHARED TAIL AS THE HALF IT DRIVES, for the same reason: §7.4.6.1's second half owns all three
       stages, so a resume at either of the later two re-enters it and nothing above runs again. */
    STEP_ARM(SH_APPLY_NAV);
    STEP_ARM(SH_APPLY_POPSTATE);
    r = sh_apply_history_step_finish(ctx, &s->apply, cb_result, out_cb, out_argc);
    /* THE MACHINE'S REST POINT IS THE ALGORITHM'S — one declaration (SH_APPLY_STAGES), copied rather than
       restated, so a parked traversal names the spec step it is parked at and cannot name a different one from
       the algorithm that parked it. */
    s->hdr.stage = s->apply.stage;
    if (r != 0) return r;   /* JS_STEP_CALL: parked on the popstate dispatch. JS_STEP_ABRUPT: it threw. */
    return JS_STEP_DONE;
}

static const JSTrampStepDef js_sh_traverse_def = {
    sizeof(SHTraverseState), js_sh_traverse_step, NULL, 0, .visit = js_sh_traverse_visit,
    .algorithm = SH_APPLY_ALGORITHM,
    .steps = SH_APPLY_STEPS
};
static int g_traverse_stepid = -1;

void session_history_traverse_by_delta(JSContext *ctx, int32_t delta)
{
    JSValueConst argv[1];
    JSValue fn, d;

    DCHECK(g_slot >= 0, "§7.4.3's traverse-the-history-by-a-delta ran before session_history_init declared the "
                        "record");
    DCHECK(delta != 0, "§7.4.3 was reached with a delta of 0 — §7.2.5's delta traverse step 4 turns that into a "
                       "RELOAD of the document's node navigable and returns, so it never gets this far");
    /* STEPS 1-3 are the SOURCE SNAPSHOT PARAMS and the INITIATOR TO CHECK, both of which exist to let §7.4.6.1
       step 3 refuse a traversal a SANDBOXED initiator is not allowed to perform on another navigable. The
       initiator here is always this navigable's own document — a cross-navigable `otherFrame.history.back()`
       reaches another instance's History and is the case sh_entries asserts against — so there is no second
       party for the check to be about, and §7.4.6.1 step 3 is skipped for a null initiatorToCheck. Step 2's
       user navigation involvement is "none" for the same reason: a script initiated this, not the browser UI. */
    if (g_traverse_stepid < 0)
        g_traverse_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &js_sh_traverse_def);
    /* THE CALLEE IS MINTED IN THIS REALM. A C function runs in the realm that DEFINED it, and every step of the
       algorithm below reads THIS document's session history off `ctx`; one held in a static would traverse
       whichever document's history happened to enqueue the first traversal. */
    fn = JS_NewCFunction2(ctx, NULL, "traverse", 1, JS_CFUNC_step, g_traverse_stepid);
    CHECK(!JS_IsException(fn), "the history traversal job's callee could not be allocated");
    d = JS_NewInt32(ctx, delta);
    argv[0] = d;
    /* IT IS NOT A MICROTASK, AND THE SPEC DOES NOT LEAVE THAT OPEN. §7.4.3 step 4 is "APPEND THE FOLLOWING
       SESSION HISTORY TRAVERSAL STEPS TO TRAVERSABLE", and §7.4.1.3 defines that queue as a session history
       traversal PARALLEL QUEUE — "very similar to a parallel queue", run in parallel — whose every observable
       effect is then queued back onto the event loop as a global task on the navigation and traversal task
       source (§7.4.6.1's apply the history step). This engine collapses the parallel queue and the tasks it
       posts into the one work item below, so the position that item must hold is the TASK one: as a microtask
       it ran inside the calling script's own checkpoint, so `history.back()` fired `popstate` before a timer
       that had already expired and before a message already delivered — a traversal completing inside the
       turn that requested it, which no browser does and which §8.1.7's two queues exist to forbid. */
    JS_EnqueueCallTask(ctx, fn, 1, argv);   /* §7.4.1.3's session history traversal queue */
    JS_FreeValue(ctx, d);
    JS_FreeValue(ctx, fn);
}

/* ---- what §7.2.5's members read ------------------------------------------------------------------------------ */

uint32_t session_history_length(JSContext *ctx)
{
    JSValue rec;

    /* §7.2.5's length is "the number of overall session history entries for the current TRAVERSABLE navigable",
       and the field this reads is written by §7.4.6.1's apply-the-history-step — which runs for the traversable.
       A child navigable's field was therefore never written, so the question is asked here rather than the
       record's initial 0 being answered as though it were a count. */
    sh_assert_is_traversable(ctx);
    rec = sh_record(ctx);
    uint32_t n = rec_uint(ctx, rec, SH_R_LENGTH);

    JS_FreeValue(ctx, rec);
    return n;
}

JSValue session_history_state(JSContext *ctx)
{
    JSValue rec = sh_record(ctx), v = JS_GetPropertyStr(ctx, rec, SH_R_STATE);

    JS_FreeValue(ctx, rec);
    return v;
}

const char *session_history_scroll_restoration(JSContext *ctx)
{
    JSValue e = sh_active_entry(ctx), v = JS_GetPropertyStr(ctx, e, SH_E_SCROLL);
    const char *s;

    JS_FreeValue(ctx, e);
    /* The two values are the IDL enumeration's, so the pointer can be a static: the setter below writes one of
       these two strings and nothing else, which is asserted there. */
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL, "session history: a scroll restoration mode could not be read");
    JS_FreeValue(ctx, v);
    if (!strcmp(s, "manual")) { JS_FreeCString(ctx, s); return "manual"; }
    DCHECK(!strcmp(s, "auto"), "§7.4.1.1's scroll restoration mode held a value the ScrollRestoration "
                               "enumeration does not have — the setter is declared with the enumeration as its "
                               "IDL type, so a third value never reaches the field");
    JS_FreeCString(ctx, s);
    return "auto";
}

void session_history_set_scroll_restoration(JSContext *ctx, const char *mode)
{
    JSValue e;

    DCHECK(mode != NULL && (!strcmp(mode, "auto") || !strcmp(mode, "manual")),
           "§7.2.5's scrollRestoration setter was handed a value the ScrollRestoration enumeration does not "
           "have — the member declares IDL_ENUM, so Web IDL has already refused anything else with a TypeError");
    e = sh_active_entry(ctx);
    JS_SetPropertyStr(ctx, e, SH_E_SCROLL, JS_NewString(ctx, mode));
    JS_FreeValue(ctx, e);
}

/* ---- what §7.2.6's navigation API reads off a §7.4.1.1 entry ------------------------------------------------ */

/* The two UUIDs, as the strings §7.2.6.5's `key` and `id` return. They are read rather than computed: an
   entry's key and id are minted once, at sh_entry_new, and nothing rewrites them — §7.4.2.3.1's cross-document
   finalize is the one algorithm in the standard that ever assigns a key to an existing entry, and it is not in
   this build (sh_finalize_same_document_navigation names the asymmetry). */
static JSValue sh_entry_string(JSContext *ctx, JSValueConst e, const char *field)
{
    JSValue v = JS_GetPropertyStr(ctx, e, field);

    DCHECK(JS_IsString(v), "a §7.4.1.1 entry's navigation API key or ID is not a string — both are minted by "
                           "sh_entry_new and neither has another writer");
    return v;
}

JSValue session_history_entry_nav_key(JSContext *ctx, JSValueConst e)
{
    return sh_entry_string(ctx, e, SH_E_NAV_KEY);
}

JSValue session_history_entry_nav_id(JSContext *ctx, JSValueConst e)
{
    return sh_entry_string(ctx, e, SH_E_NAV_ID);
}

JSValue session_history_entry_url(JSContext *ctx, JSValueConst e)
{
    return sh_entry_string(ctx, e, SH_E_URL);
}

bool session_history_entry_is_this_document(JSContext *ctx, JSValueConst e)
{
    return sh_entry_is_this_document(ctx, e);
}

JSValue session_history_entry_nav_state(JSContext *ctx, JSValueConst e)
{
    JSValue buf = JS_GetPropertyStr(ctx, e, SH_E_NAV_STATE), v;
    StructuredData d;

    d.buf = JS_GetArrayBuffer(ctx, &d.len, buf);
    DCHECK(d.buf != NULL, "§7.4.1.1's navigation API state held something that is not the serialized bytes — "
                          "every writer of the field is in this file and every one writes an ArrayBuffer");
    /* A FRESH DESERIALIZATION, which is what §7.2.6.5's `getState()` promises and why it is a method: "unless
       the state value is a primitive, entry.getState() !== entry.getState()". */
    v = structured_deserialize(ctx, &d);
    JS_FreeValue(ctx, buf);
    return v;
}

void session_history_entry_set_nav_state(JSContext *ctx, JSValueConst e, const StructuredData *d)
{
    JS_SetPropertyStr(ctx, (JSValue)e, SH_E_NAV_STATE, sh_state_buffer(ctx, d));
}

/* ---- declaration and install --------------------------------------------------------------------------------- */

/* THE RECORD IS BUILT WITH THE REALM and the ENTRY with the DOCUMENT — see session_history.h for why the two
   cannot be one call. Both are baseline: a record or an entry made on first touch would belong to whichever
   flow happened to read first, and every flow that forked before it would have a history it never built. */
static void session_history_install_realm(JSContext *ctx)
{
    JSValue rec = JS_NewObjectProto(ctx, JS_NULL), entries;

    CHECK(!JS_IsException(rec), "session history: this realm's §7.4.1 record could not be allocated");
    entries = JS_NewArray(ctx);
    CHECK(!JS_IsException(entries), "session history: this realm's §7.4.1 entries list could not be allocated");
    JS_SetPropertyStr(ctx, rec, SH_R_ENTRIES, entries);
    JS_SetPropertyStr(ctx, rec, SH_R_STEP, JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, rec, SH_R_ACTIVE, JS_NULL);
    JS_SetPropertyStr(ctx, rec, SH_R_CURRENT, JS_NULL);
    JS_SetPropertyStr(ctx, rec, SH_R_LATEST, JS_NULL);
    /* §7.2.5: "Each History object has state, initially null … a length, initially 0 … an index, initially
       0." The two integers are corrected by the first apply-the-history-step, which the document install runs
       below; they are written here because a field with no value is a field a getter answers `undefined` for. */
    JS_SetPropertyStr(ctx, rec, SH_R_STATE, JS_NULL);
    JS_SetPropertyStr(ctx, rec, SH_R_LENGTH, JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, rec, SH_R_INDEX, JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, rec, SH_R_UUID, JS_NewUint32(ctx, 0));
    realm_value_set(ctx, g_slot, rec);
}

void session_history_install_document(JSContext *ctx)
{
    JSValue rec, entry, entries;
    StructuredData nul, undef;

    DCHECK(g_slot >= 0, "a document reached §7.4.1 before session_history_init declared the record");
    rec = sh_record(ctx);
    {
        JSValue prev = JS_GetPropertyStr(ctx, rec, SH_R_ACTIVE);
        bool none = JS_IsNull(prev);

        JS_FreeValue(ctx, prev);
        DCHECK(none, "a second Document was installed into one realm's session history — a realm is a Document "
                     "here, so that is a NAVIGATION, and a navigation appends an entry through §7.4.6's "
                     "activate-history-entry rather than replacing the list this navigable already has");
    }
    sh_serialize_primitive(ctx, JS_NULL, &nul);
    sh_serialize_primitive(ctx, JS_UNDEFINED, &undef);
    entry = sh_entry_new(ctx, document_url(ctx), sh_document_state_new(ctx), sh_state_buffer(ctx, &nul),
                         sh_state_buffer(ctx, &undef), "auto");
    structured_data_free(ctx, &undef);
    structured_data_free(ctx, &nul);
    /* THE FIRST ENTRY IS STEP 0 and the traversable's current step is 0 — the state §7.4.6's apply-the-history
       -step leaves a freshly loaded document in, reached here directly because the load is what built the
       document rather than something this engine can be inside of. */
    JS_SetPropertyStr(ctx, entry, SH_E_STEP, JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, rec, SH_R_ACTIVE, JS_DupValue(ctx, entry));
    /* THE CURRENT SESSION HISTORY ENTRY IS THE SAME ONE while nothing is traversing — they diverge only between
       §7.4.6.1 setting the current entry and activate-history-entry making it active, which is the window a
       traversal is in flight in. It is written here rather than left null because §7.4.1.4's
       get-all-navigables-whose-current-session-history-entry-will-change reads it for the FIRST traversal, and a
       field with no value would make that read `undefined`. */
    JS_SetPropertyStr(ctx, rec, SH_R_CURRENT, JS_DupValue(ctx, entry));
    JS_SetPropertyStr(ctx, rec, SH_R_LATEST, JS_DupValue(ctx, entry));
    /* THE ENTRY GOES IN THE TRAVERSABLE'S OWN LIST, AND ONLY THE TRAVERSABLE HAS ONE HERE. A navigable that is
       not its own traversable keeps its ACTIVE ENTRY — which is all §7.2.5's `state` and `scrollRestoration`
       read, so an iframe answers both correctly — and gets no entries list, because its list is a NESTED
       HISTORY inside its parent entry's document state and nothing builds one (sh_assert_is_traversable names
       the build). The crash is therefore where the missing capability is USED — an iframe's `pushState`,
       `replaceState` or `length` — and not at the install of every framed document, which would take down
       pages that never touch session history at all. */
    if (window_proxy_is_top_level(document_window_proxy(ctx))) {
        /* §7.2.5's length is 1 and its index 0 for a traversable with one entry — the values §7.4.6.1's
           get-the-history-object-length-and-index computes over a one-entry run. */
        rec_set_uint(ctx, rec, SH_R_LENGTH, 1);
        rec_set_uint(ctx, rec, SH_R_INDEX, 0);
        entries = sh_entries(ctx);
        DCHECK(list_len(ctx, entries) == 0,
               "a traversable's session history entries were not empty when its first Document was installed — "
               "a realm is installed once, so a non-empty list means a second document reached this install");
        JS_SetPropertyUint32(ctx, entries, 0, entry);   /* CONSUMES entry */
        JS_FreeValue(ctx, entries);
        /* §7.2.6.4's INITIALIZE THE NAVIGATION API ENTRIES FOR A NEW DOCUMENT, run HERE because this call is
           where this engine's collapsed populate-and-activate happens (see the header). §7.4.6.2 step 6.5 is
           where the standard runs it, and that step is unreachable in this build for the reason
           sh_update_document_for_history_step states: a Document that reaches §7.4.6.2 already has a latest
           entry, so documentIsNew is false there and this is the only place it can be true.
           It goes AFTER the entry is in the list, because §7.4.1.4's get-session-history-entries-for-the-
           navigation-API reads that list and §7.2.6.4 step 5 then looks the initial entry up inside it. */
        {
            JSValue for_nav = session_history_entries_for_navigation_api(ctx);
            JSValue active = sh_active_entry(ctx);

            navigation_initialize_entries(ctx, for_nav, active);
            JS_FreeValue(ctx, active);
            JS_FreeValue(ctx, for_nav);
        }
    } else {
        /* A CHILD NAVIGABLE GETS NO ENTRY LIST AND NO NAVIGATION API ENTRIES, for the one reason above: its
           §7.4.1 entries live in a nested history nothing builds, and §7.4.1.4's walk over them is what
           sh_assert_is_traversable names. Its Navigation therefore keeps §7.2.6.3's initial current entry index
           of −1, and the crash is at the first member that needs an entry — which is where the missing
           capability is USED, exactly as `pushState` is. */
        JS_FreeValue(ctx, entry);
    }
    JS_FreeValue(ctx, rec);
}

void session_history_init(JSContext *ctx)
{
    DCHECK(g_slot < 0, "session_history_init ran twice — §7.4.1's record is declared once per AGENT");
    g_slot = realm_value_declare(ctx, "HTML §7.4.1 the session history entries, the current session history "
                                      "step, and the History object's state, length and index");
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The traversal machine's id is
       declared HERE although it is registered LAZILY, at the first §7.4.3 traverse-the-history-by-a-delta: the
       registry's question is what this component's release owes, and a slot that is sometimes set is one the
       release owes on exactly the runs where it was. A declaration that only existed on those runs would be a
       release column nothing checks on the others. */
    agent_state_id("session_history", &g_slot,
                   "HTML §7.4.1 Session history's realm-value slot for the session history entries, the "
                   "current session history step and the History object's state, length and index — and this "
                   "component's declaration latch");
    agent_state_id("session_history", &g_traverse_stepid,
                   "HTML §7.4.3 Reloading and traversing's traverse-the-history-by-a-delta step definition, "
                   "registered with the runtime");
    realm_declare_intrinsic(session_history_install_realm);
}

void session_history_free(void)
{
    /* The records are the REALMS' — each is released with its context. What the agent holds is the slot and the
       traversal machine's registered id, and both name things in a runtime that is going away with them. */
    g_slot = -1;
    g_traverse_stepid = -1;
}
