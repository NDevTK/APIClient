/* HTML §7.4.1's session history and §7.4.4's URL and history update steps — see session_history.h. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/document.h"
#include "core/frame/session_history.h"
#include "core/frame/window_proxy.h"
#include "core/realm.h"
#include "core/structured_clone.h"

static int g_slot = -1;

/* ---- §7.4.1.1's SESSION HISTORY ENTRY, and §7.4.1.2's DOCUMENT STATE -----------------------------------------
 *
 * Both are STRUCTS in the standard and both are ORDINARY JS OBJECTS here, for the reason session_history.h
 * gives: a struct a flow mutates has to ride that flow's COW delta, and a property write already does. Their
 * fields are named by these atoms and by nothing else, so a field the code reads and no builder writes is one
 * assertion away rather than a silent `undefined`.
 *
 * THE FIELDS THIS ENGINE DOES NOT HOLD ARE NAMED WHERE THEY ARE NOT HELD, not omitted. §7.4.1.1 also gives an
 * entry a NAVIGATION API STATE, KEY and ID — §7.2.7's, whose whole interface is out of this component — and a
 * SCROLL POSITION DATA and PERSISTED USER STATE, which §7.4.6.5's save/restore are the only writers and readers
 * of and which only a TRAVERSAL reaches. Neither is a field a push or a replace can observe. */
#define SH_E_STEP    "step"        /* a non-negative integer, or the string "pending" */
#define SH_E_URL     "url"
#define SH_E_DOCSTATE "documentState"
#define SH_E_CLASSIC "classicState" /* serialized state — an ArrayBuffer of §2.7 bytes, never a live value */
#define SH_E_SCROLL  "scrollRestoration"

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
#define SH_R_LATEST  "latest"
#define SH_R_STATE   "state"
#define SH_R_LENGTH  "length"
#define SH_R_INDEX   "index"

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
           "HTML §7.4.1.4's get-session-history-entries reached a navigable that is not its own traversable. "
           "There are two builds behind this, and which one depends on where the traversable is. SAME "
           "INSTANCE: this navigable's entries live in a NESTED HISTORY inside its parent entry's document "
           "state, and nothing has built one — §7.4's create a new navigable appends a new nested history (the "
           "child's id, an empty entries list) to the creating entry's document state, so do that append in "
           "core/frame/navigable.c's navigable_create where the child's id is minted, and §7.4.1.4's walk over "
           "document states then finds it by id. ANOTHER INSTANCE (a cross-origin frame): the entries, the "
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

/* §2.7's StructuredSerializeForStorage(null) — §7.4.1.1's initial classic history API state. `out` is the
   caller's to free with structured_data_free. */
static void sh_serialize_null(JSContext *ctx, StructuredData *out)
{
    int r = structured_serialize(ctx, JS_NULL, out);

    CHECK(r == 0, "session history: StructuredSerializeForStorage(null) failed — null is serializable by "
                  "every clause of §2.7, so a refusal here is the serializer being unable to allocate");
}

/* §7.4.1.2's DOCUMENT STATE for the Document of `ctx`. Its `ever populated` is TRUE: this engine's Documents
   are built from a completed parse, so a document state exists only once its document does. */
static JSValue sh_document_state_new(JSContext *ctx)
{
    JSValueConst proxy = document_window_proxy(ctx);
    const char *origin = window_proxy_origin(proxy);
    JSValue ds = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(ds), "session history: a §7.4.1.2 document state could not be allocated");
    JS_SetPropertyStr(ctx, ds, SH_D_DOC, JS_NewUint32(ctx, document_doc(ctx)));
    /* §7.4.1.2's origin is "the origin that we set about:-schemed Documents' origin to … also used to compare
       the origin before and after the session history entry is repopulated". The navigable's active document's
       origin IS that origin, and window_proxy is the one place this engine holds it. */
    JS_SetPropertyStr(ctx, ds, SH_D_ORIGIN, JS_NewString(ctx, origin ? origin : "null"));
    JS_SetPropertyStr(ctx, ds, SH_D_EVER, JS_TRUE);
    return ds;
}

/* §7.4.1.1's "a session history entry is a struct with the following items", built with the four this engine
   holds. `doc_state` and `classic` are CONSUMED; `url` and `scroll` are copied. */
static JSValue sh_entry_new(JSContext *ctx, const char *url, JSValue doc_state, JSValue classic,
                            const char *scroll)
{
    JSValue e = JS_NewObjectProto(ctx, JS_NULL);

    CHECK(!JS_IsException(e), "session history: a §7.4.1.1 entry could not be allocated");
    /* "step, a non-negative integer or 'pending', initially 'pending'" — the string, because a number that
       means "not a number yet" is a sentinel the assertions below could not tell from a real step. */
    JS_SetPropertyStr(ctx, e, SH_E_STEP, JS_NewString(ctx, "pending"));
    JS_SetPropertyStr(ctx, e, SH_E_URL, JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, e, SH_E_DOCSTATE, doc_state);
    JS_SetPropertyStr(ctx, e, SH_E_CLASSIC, classic);
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

/* ---- §7.4.6.2's UPDATE DOCUMENT FOR HISTORY STEP APPLICATION -------------------------------------------------
 *
 * REACHED ONLY FROM A PUSH OR A REPLACE, which is what makes this the short algorithm rather than the long one.
 * §7.4.4 has already set the Document's latest entry to the new entry (its step 10), so documentsEntryChanged is
 * FALSE; the Document is not new, so documentIsNew is FALSE; and §7.4.4's own call passes doNotReactivate TRUE.
 * Every branch of §7.4.6.2 is therefore skipped except its first two steps — and those two are the whole reason
 * the algorithm is called at all, because they are what replaces §7.4.4 step 6's "temporary best-guess values"
 * with the real ones.
 *
 * THE SKIPPED BRANCHES ARE THE TRAVERSAL'S, and each is asserted rather than dropped: the popstate branch is
 * inside `if documentsEntryChanged`, the reactivate branch inside `if documentsEntryChanged is false and
 * doNotReactivate is false`, and the NavigationActivation branch needs §7.2.7's navigation API. The standard's
 * own note names this asymmetry as the reason `popstate` does not fire for `pushState`. */
static void sh_update_document_for_history_step(JSContext *ctx, JSValueConst entry, uint32_t length,
                                                uint32_t index)
{
    JSValue rec = sh_record(ctx), latest;

    latest = JS_GetPropertyStr(ctx, rec, SH_R_LATEST);
    DCHECK(JS_VALUE_GET_PTR(latest) == JS_VALUE_GET_PTR(entry),
           "§7.4.6.2 was reached with documentsEntryChanged TRUE from a push or a replace — §7.4.4 step 10 sets "
           "the Document's latest entry to the new entry before this runs, so a difference here means a "
           "TRAVERSAL reached this algorithm and its popstate branch (fire an event named popstate at the "
           "relevant global object, using PopStateEvent, with the state attribute initialized to the history "
           "object's state) and its hashchange branch have to be written");
    JS_FreeValue(ctx, latest);
    /* STEPS 3 AND 4. */
    rec_set_uint(ctx, rec, SH_R_INDEX, index);
    rec_set_uint(ctx, rec, SH_R_LENGTH, length);
    JS_FreeValue(ctx, rec);
}

/* ---- §7.4.6.1's APPLY THE PUSH/REPLACE HISTORY STEP ----------------------------------------------------------
 *
 * §7.4.6.1's apply-the-history-step is an enormous algorithm because it coordinates a traversable with every
 * descendant navigable that a traversal changes, populating them in parallel and synchronising back up. For a
 * PUSH or a REPLACE performed by §7.4.4 there is exactly one changing navigable and its work is already done:
 * §7.4.4 synchronously set the navigable's active session history entry to the new entry, so the algorithm's
 * own test — "if displayedEntry is targetEntry and targetEntry's document state's reload pending is false, set
 * update-only to true and abort these steps" — holds, and with update-only true there is no deactivate, no
 * unload and no activate. What remains is the two lines this performs: the document's history object length and
 * index, and the traversable's current session history step.
 *
 * The standard's note on that test says exactly this: "this case occurs due to a synchronous navigation which
 * already updated the active session history entry." */
static void sh_apply_push_replace_history_step(JSContext *ctx, uint32_t target_step)
{
    JSValue entries = sh_entries(ctx), trec = sh_traversable_record(ctx), active;
    uint32_t length = 0, index = 0;

    sh_length_and_index(ctx, entries, target_step, &length, &index);
    active = sh_active_entry(ctx);
    sh_update_document_for_history_step(ctx, active, length, index);
    JS_FreeValue(ctx, active);
    /* THE LAST STEP OF APPLY THE HISTORY STEP: "set traversable's current session history step to
       targetStep". Everything between it and the entry point is the coordination of navigables that are not
       changing, and there are none — a child navigable's entries live in a nested history this engine does not
       build yet, and sh_entries asserts that at the one place it would matter. */
    rec_set_uint(ctx, trec, SH_R_STEP, target_step);
    JS_FreeValue(ctx, trec);
    JS_FreeValue(ctx, entries);
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
                                                 JSValueConst entry_to_replace)
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
        /* "Set targetStep to traversable's current session history step" — a replace does not move it. */
        target_step = current;
    }
    JS_FreeValue(ctx, trec);
    JS_FreeValue(ctx, entries);
    /* STEP 6, "done even for replace navigations, as it resolves race conditions across multiple synchronous
       navigations". */
    sh_apply_push_replace_history_step(ctx, target_step);
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
    const char *url = document_base_url(ctx);

    if (!url || strcmp(url, "about:blank") != 0) return false;
    return !window_proxy_ever_navigated(document_window_proxy(ctx));
}

void session_history_url_and_history_update(JSContext *ctx, const char *new_url,
                                            const StructuredData *serialized, bool push)
{
    JSValue active, doc_state, classic, new_entry, rec, to_replace;
    const char *scroll;
    JSValue scroll_v;

    DCHECK(g_slot >= 0, "§7.4.4's URL and history update steps ran before session_history_init declared the "
                        "record");
    DCHECK(new_url != NULL, "§7.4.4 runs with a URL — its step 2 defaults it to the document's own address, so "
                            "the caller always has one");
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
    new_entry = sh_entry_new(ctx, new_url, doc_state, classic, scroll);
    JS_FreeCString(ctx, scroll);
    JS_FreeValue(ctx, scroll_v);

    /* STEP 4: "if document's is initial about:blank is true, then set historyHandling to replace". The
       standard's own note — "this means that pushState() on an initial about:blank Document behaves as a
       replaceState() call". */
    if (sh_is_initial_about_blank(ctx)) push = false;
    /* STEP 5. */
    to_replace = push ? JS_NULL : JS_DupValue(ctx, active);

    rec = sh_record(ctx);
    /* STEP 6's TEMPORARY BEST-GUESS VALUES, "for immediate synchronous access" — a page reading
       `history.length` on the line after a pushState reads these, and §7.4.6.1's apply-the-push/replace step
       below overwrites them with the real ones in the same turn. Written even so: they are what the standard
       says the field holds between these two points, and a reader that could observe the difference is a
       reader that could observe a step machine parking between them. */
    if (push) {
        uint32_t index = rec_uint(ctx, rec, SH_R_INDEX) + 1;

        rec_set_uint(ctx, rec, SH_R_INDEX, index);
        rec_set_uint(ctx, rec, SH_R_LENGTH, index + 1);
    }
    /* STEP 7. */
    if (serialized) sh_restore_history_object_state(ctx, new_entry);
    /* STEP 8: "set the URL given document to newURL". The standard's note is why nothing is fired here —
       "since this is neither a navigation nor a history traversal, it does not cause a hashchange event to be
       fired". */
    document_set_url(ctx, new_url);
    /* STEPS 9 AND 10 — the Document's latest entry and the NAVIGABLE's active session history entry. Both are
       set before the finalize below, which is what makes its step 2 hold. */
    JS_SetPropertyStr(ctx, rec, SH_R_LATEST, JS_DupValue(ctx, new_entry));
    JS_SetPropertyStr(ctx, rec, SH_R_ACTIVE, JS_DupValue(ctx, new_entry));
    JS_FreeValue(ctx, rec);
    /* STEP 11 is "update the navigation API entries for a same-document navigation", which belongs to §7.2.7's
       navigation API — a separate interface this component deliberately does not build (session_history.h).
       Its absence is asserted where the interface would be: core/rendering/rendering.c's realm_awaits probe. */
    /* STEPS 12-13. */
    sh_finalize_same_document_navigation(ctx, new_entry, to_replace);
    JS_FreeValue(ctx, to_replace);
    JS_FreeValue(ctx, new_entry);
    JS_FreeValue(ctx, active);
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
    JS_SetPropertyStr(ctx, rec, SH_R_LATEST, JS_NULL);
    /* §7.2.5: "Each History object has state, initially null … a length, initially 0 … an index, initially
       0." The two integers are corrected by the first apply-the-history-step, which the document install runs
       below; they are written here because a field with no value is a field a getter answers `undefined` for. */
    JS_SetPropertyStr(ctx, rec, SH_R_STATE, JS_NULL);
    JS_SetPropertyStr(ctx, rec, SH_R_LENGTH, JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, rec, SH_R_INDEX, JS_NewUint32(ctx, 0));
    realm_value_set(ctx, g_slot, rec);
}

void session_history_install_document(JSContext *ctx)
{
    JSValue rec, entry, entries;
    StructuredData nul;

    DCHECK(g_slot >= 0, "a document reached §7.4.1 before session_history_init declared the record");
    /* THE TWO-SIDED ASSERTION FOR THE HALF OF §7.4 THIS COMPONENT DOES NOT PERFORM — a TRAVERSAL. §7.2.5's
       `go`, `back` and `forward` are all "delta traverse this given <n>", which ends in §7.4.3's TRAVERSE THE
       HISTORY BY A DELTA and §7.4.6's APPLY THE HISTORY STEP; core/frame/history.c installs none of the three,
       and what stops them being written is not the bookkeeping this file already holds but the EVENTS
       §7.4.6.2 fires while applying the step. The probe is asked HERE, at document install, rather than at the
       realm intrinsic that builds History: an intrinsic runs while the realm's platform surface is still being
       assembled, so a probe there would answer "absent" for an interface a later intrinsic installs, and would
       go on answering it forever. Every intrinsic has run by this line. */
    realm_awaits(ctx, "PopStateEvent",
                 "HTML §7.4.6.2's update-document-for-history-step-application FIRES popstate at the "
                 "document's relevant global object, using PopStateEvent, with the state attribute initialized "
                 "to the history object's state — which is why core/frame/history.c installs no `go`, `back` "
                 "or `forward`: a traversal that moved the current session history step without firing the "
                 "event would leave a page believing it navigated and a router that never re-renders. This "
                 "build now HAS that interface, so write the traversal: §7.4.3 resolves the delta against the "
                 "used history steps (sh_length_and_index already computes over them), §7.4.6.1 activates the "
                 "target entry — whose document is non-null for every same-document entry a pushState made, so "
                 "no population is needed — and §7.4.6.2 restores the history object state, fires popstate, "
                 "and queues a hashchange (HashChangeEvent, oldURL and newURL serialized) when the old and new "
                 "entries' fragments differ");
    rec = sh_record(ctx);
    {
        JSValue prev = JS_GetPropertyStr(ctx, rec, SH_R_ACTIVE);
        bool none = JS_IsNull(prev);

        JS_FreeValue(ctx, prev);
        DCHECK(none, "a second Document was installed into one realm's session history — a realm is a Document "
                     "here, so that is a NAVIGATION, and a navigation appends an entry through §7.4.6's "
                     "activate-history-entry rather than replacing the list this navigable already has");
    }
    sh_serialize_null(ctx, &nul);
    entry = sh_entry_new(ctx, document_base_url(ctx), sh_document_state_new(ctx), sh_state_buffer(ctx, &nul),
                         "auto");
    structured_data_free(ctx, &nul);
    /* THE FIRST ENTRY IS STEP 0 and the traversable's current step is 0 — the state §7.4.6's apply-the-history
       -step leaves a freshly loaded document in, reached here directly because the load is what built the
       document rather than something this engine can be inside of. */
    JS_SetPropertyStr(ctx, entry, SH_E_STEP, JS_NewUint32(ctx, 0));
    JS_SetPropertyStr(ctx, rec, SH_R_ACTIVE, JS_DupValue(ctx, entry));
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
    } else {
        JS_FreeValue(ctx, entry);
    }
    JS_FreeValue(ctx, rec);
}

void session_history_init(JSContext *ctx)
{
    DCHECK(g_slot < 0, "session_history_init ran twice — §7.4.1's record is declared once per AGENT");
    g_slot = realm_value_declare(ctx, "HTML §7.4.1 the session history entries, the current session history "
                                      "step, and the History object's state, length and index");
    realm_declare_intrinsic(session_history_install_realm);
}

void session_history_free(void)
{
    /* The records are the REALMS' — each is released with its context. What the agent holds is the slot, and a
       slot id is a class id in a runtime that is going away with it. */
    g_slot = -1;
}
