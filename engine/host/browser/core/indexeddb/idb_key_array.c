/* INDEXED DATABASE §7.4's "convert a value to a key" OVER AN ARRAY EXOTIC OBJECT — see idb_key_array.h for why
 * this arm is a component of its own and idb_key.h's other arms are not.
 *
 * WHAT THE THREE PIECES OF STATE ARE, AND WHY NONE OF THEM IS THE OTHER:
 *
 *   THE LEVEL STACK is the algorithm's own recursion, made explicit. Step 5.4 converts an element "with
 *   arguments entry and seen", and an element that is itself an Array re-enters the same steps — so a level
 *   holds exactly what one entry of that recursion holds (the array, its len, its index, and the keys collected
 *   off it) and a PUSH is the recursive call. C recursion could not be one: the depth is `[[[[…]]]]` as deep as
 *   the page wrote it, and engine/check_recursion.mjs is the gate that says so over the whole program.
 *
 *   `seen` IS NOT THAT STACK, and reading it as one is the mistake this file is built to make impossible. §7.4
 *   never REMOVES from seen — step 2 appends and nothing pops — so the set is every array the whole conversion
 *   has touched and not the chain of ancestors above the cursor. The observable difference is one line of page
 *   code: `const a = [1]; IDBKeyRange.only([a, a])` is a "DataError", because by the time step 5.4 reaches the
 *   second `a` the set already contains it, while `IDBKeyRange.only([[1], [1]])` is a perfectly good two-subkey
 *   key. A stack of ancestors answers the first of those wrong and nothing else in the algorithm would notice.
 *
 *   THE KEYS ARE A PLAIN ARRAY, which is §2.4's "a list of other keys" and is the same decision §2.5's list key
 *   path made: the list is written by a FLOW, so it has to be a JS value the COW delta captures and the cold
 *   tier parks, never a malloc'd list whose pointers a context switch would revert while its nodes stayed
 *   reachable from nothing.
 *
 * EVERY READ OFF THE PAGE'S OBJECT IS A REQUEST, WHICH IS THE WHOLE REASON THIS IS A MACHINE. Step 5.1's
 * HasOwnProperty is 7.3.13 over [[GetOwnProperty]] — step_getownprop_run, answered undefined when the property
 * is absent, which IS "hop is false" — and step 5.3's Get can be the page's own index accessor. Both suspend,
 * so the walk parks at every element of a structure the page sized.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/indexeddb/idb_key.h"
#include "core/indexeddb/idb_key_array.h"
#include "solver/concolic.h"

/* THE PHASES, FROM THE SAME X-LIST EVERY CALLER'S LABELS COME FROM. A caller's block holds them in this
   order and the body below is written against the OFFSET into it, so this file names no caller's constants and
   the order cannot drift from the labels — they are one declaration. The COUNT is not written here either:
   the list is the one place that holds it, and a number in prose beside it is a second copy that goes stale
   the day a stage is appended, which is what happened the day the fork's was. */
enum { IDB_KEY_ARRAY_ALGO_STAGES(JS_STEP_STAGE_ENUM, IDB_KW, "") IDB_KW_N };

/* EVERY REQUEST THIS ALGORITHM CAN HAVE IN FLIGHT, LISTED ONCE — the header's keyed-read cursor (step 1's and
   step 5.3's Get) and its own-descriptor cursor (step 5.1's HasOwnProperty). Written out at every transition
   they would be one statement of one fact per transition. */
#define IDB_KW_GOTO(hdr, to) STEP_GOTO((hdr)->stage, (to), &(hdr)->get_phase, &(hdr)->desc_phase, NULL)

/* ---- the record ------------------------------------------------------------------------------------------- */

static void idb_key_level_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    IdbKeyLevel *f = elem;

    v->val(ctx, &f->src);
    v->val(ctx, &f->keys);
}

void idb_key_walk_visit(JSContext *ctx, IdbKeyWalk *w, JSStepVisit *v)
{
    v->array(ctx, (void **)&w->lv, sizeof(IdbKeyLevel), w->sp, w->cap, idb_key_level_visit);
    v->val(ctx, &w->seen);
    v->val(ctx, &w->entry);
    v->atom(ctx, &w->hop_atom);
    /* THE FORK'S OPERAND IS NAMED HERE OR THE SIBLING READS A FREED ONE. `step_fork_run` BORROWS it onto the
       header and the sibling is cloned AT the ask, so this is the declaration that copies it into the arm
       that will re-ask about it — a `val` the visit does not name is not copied by a fork. */
    v->val(ctx, &w->unknown);
    v->val(ctx, &w->key);
}

/* THE DECLARATION ABOVE DISCHARGED, AND NOTHING RESTATED — the shape core/idl_iter.c's `iter_cursor_release`
   has, and it exists for the same reason: §4.7's `bound` converts a SECOND value through the same record, so
   the release happens mid-member rather than at a teardown. `sp`/`cap` are reset here because the visitor can
   only NULL the allocation it is handed; a stack pointer left naming a freed array is what the next visit
   would walk. */
static void walk_release(JSContext *ctx, IdbKeyWalk *w)
{
    idb_key_walk_visit(ctx, w, JS_StepFreeVisitor());
    w->sp = 0;
    w->cap = 0;
    w->res = IDB_KEY_OK;
    w->multi = 0;
}

/* THE RUNTIME'S ALLOCATOR, BECAUSE THE DECLARATION'S IS: `v->array` copies this stack with js_malloc for a
   forked sibling and frees it with js_free, so a stack grown with the C library's realloc is one an arm would
   hand to the wrong allocator. `src` is CONSUMED. */
static void walk_push(JSContext *ctx, IdbKeyWalk *w, JSValue src)
{
    IdbKeyLevel *f;

    DCHECK(JS_IsArray(src), "§7.4's array arm pushed a level over something that is not an Array exotic "
                            "object — idb_key_convert_here is what decides that, and it answers IDB_KEY_ARRAY "
                            "for nothing else");
    if (w->sp == w->cap) {
        int want = w->cap ? w->cap * 2 : 4;
        IdbKeyLevel *a = js_realloc(ctx, w->lv, sizeof(IdbKeyLevel) * (size_t)want);

        CHECK(a != NULL, "IndexedDB: §7.4's array arm could not grow its level stack — the nesting is the "
                         "page's own, and a dropped level would file a record under a key that is not the one "
                         "the page gave");
        w->lv = a;
        w->cap = want;
    }
    f = &w->lv[w->sp++];
    memset(f, 0, sizeof(*f));
    /* A ZEROED JSValue IS THE INTEGER 0 AND NOT JS_UNDEFINED (JS_TAG_INT is 0), so both slots are placed. */
    f->src = src;
    f->keys = JS_UNDEFINED;
    f->len = 0;
    f->index = 0;
}

/* THE LENGTH OF ONE OF THIS FILE'S OWN ARRAYS. It runs none of the page's code: `seen` and every `keys` list is
   engine-built, has no prototype chain worth consulting for `length` and no getter to run. */
static uint32_t walk_len(JSContext *ctx, JSValueConst list)
{
    JSValue len = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;
    int r;

    DCHECK(!JS_IsException(len), "reading the length of one of §7.4's own Arrays threw");
    r = JS_ToUint32(ctx, &n, len);
    DCHECK(r >= 0, "one of §7.4's own Arrays had a length that is not a number");
    (void)r;
    JS_FreeValue(ctx, len);
    return n;
}

/* Append to one of them. `v` is CONSUMED. */
static void walk_append(JSContext *ctx, JSValueConst list, JSValue v)
{
    int r = JS_DefinePropertyValueUint32(ctx, list, walk_len(ctx, list), v, JS_PROP_C_W_E);

    CHECK(r >= 0, "IndexedDB: §7.4 could not append to one of its own lists");
}

/* §7.4 STEP 2's "if seen CONTAINS input". Infra's set membership over ECMAScript values, which for the only
   thing step 2 ever appends — an object — is identity; JS_IsSameValue is that and runs none of the page's
   code. The scan is linear over a set that holds one entry per ARRAY the conversion has reached, which is what
   makes it cheap for the ordinary flat key and exactly the standard's own cost otherwise. */
static bool walk_seen_contains(JSContext *ctx, IdbKeyWalk *w, JSValueConst v)
{
    uint32_t i, n = walk_len(ctx, w->seen);

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, w->seen, i);
        bool same = JS_IsSameValue(ctx, e, v);

        JS_FreeValue(ctx, e);
        if (same)
            return true;
    }
    return false;
}

/* THE ALGORITHM IS OVER — with a key, or with one of §7.4's two refusals. Every level is unwound (a refusal
   ABORTS the steps at whatever depth it was reached, which is what step 5.6's "abort these steps" says) and
   the caller's own stage is where control goes. `key` is CONSUMED. */
static int walk_finish(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, IdbKeyResult res, JSValue key)
{
    int after = w->after;

    DCHECK(res != IDB_KEY_ARRAY, "§7.4's array arm finished with the answer that means \"this is an array, walk "
                                 "it\" — that answer is consumed by the push and never reaches a completion");
    DCHECK(res != IDB_KEY_UNKNOWN, "§7.4 finished with the answer that means \"this value's arm is undecided, "
                                   "fork it\" — that answer is consumed by the UNKNOWN stage, which replaces it "
                                   "with the completion this flow took, and a caller handed it would read an "
                                   "unasked question as the algorithm's result");
    walk_release(ctx, w);
    w->key = key;
    w->res = res;
    IDB_KW_GOTO(hdr, after);
    return JS_STEP_YIELD;
}

/* THE MULTIENTRY MODE'S ONE DIFFERENCE THAT IS NOT A SKIP: "if there is no item in keys equal to key, then
   append key to keys". Equal by §2.4's compare, which is the only equality this standard has for keys. It runs
   at the TOP level only, because a nested level is the ordinary algorithm. */
static bool multi_keys_hold(JSContext *ctx, JSValueConst keys, JSValueConst key)
{
    uint32_t i, n = walk_len(ctx, keys);

    for (i = 0; i < n; i++) {
        JSValue k = JS_GetPropertyUint32(ctx, keys, i);
        int c = idb_key_compare(ctx, k, key);

        JS_FreeValue(ctx, k);
        if (c == 0)
            return true;
    }
    return false;
}

/* STEP 5.7 FOR EITHER MODE — the parent's "append key to keys", which multiEntry's top level filters. `key` is
   CONSUMED either way, so a dropped duplicate is freed here and not left to the caller. */
static void level_append_subkey(JSContext *ctx, IdbKeyWalk *w, IdbKeyLevel *f, JSValue key)
{
    if (w->multi && f == &w->lv[0] && multi_keys_hold(ctx, f->keys, key)) {
        JS_FreeValue(ctx, key);
        return;
    }
    walk_append(ctx, f->keys, key);
}

/* MULTIENTRY'S "IF entry IS NOT AN ABRUPT COMPLETION" AND "IF key IS NOT 'invalid value' OR 'invalid type' OR
 * AN ABRUPT COMPLETION" — one mechanism, because both say the same thing about the same element: this top-level
 * member contributes nothing and the loop moves on.
 *
 * IT UNWINDS TO THE TOP LEVEL, which is what makes it right at any depth. The refusal may be reached inside a
 * NESTED conversion (a hole in `[[1, , 2]]`, a throwing accessor two arrays down), and that whole nested
 * conversion is the one call whose result is being discarded — so every level below the top leaves, and the top
 * level's index advances exactly once. */
static int multi_skip(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, int base)
{
    DCHECK(w->multi && w->sp >= 1, "§7.4's multiEntry skip ran with no top level under it, or in the mode that "
                                   "has no skip — convert-a-value-to-a-key REFUSES where this advances");
    if (JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, w->entry);
    w->entry = JS_UNDEFINED;
    DCHECK(w->hop_atom == JS_ATOM_NULL, "§7.4's multiEntry skip ran with step 5.1's property key still held — "
                                        "the atom is released the moment that request answers, and every site "
                                        "that skips is at or after it");
    while (w->sp > 1) {
        IdbKeyLevel *f = &w->lv[--w->sp];

        JS_FreeValue(ctx, f->src);
        JS_FreeValue(ctx, f->keys);
        f->src = JS_UNDEFINED;
        f->keys = JS_UNDEFINED;
    }
    w->lv[0].index++;
    IDB_KW_GOTO(hdr, base + IDB_KW_HOP);
    return JS_STEP_YIELD;
}

/* §7.4 STEPS 5.6-5.8 OVER A SUBKEY THAT HAS BEEN CONVERTED — one copy, because the conversion answers at TWO
 * sites now: in C at the SUBKEY stage for a value whose type §7.4 can read off it, and at the UNKNOWN stage
 * for one whose type is the fork's answer. The steps after the conversion are identical for both and the one
 * thing they must not do is differ, since a subkey that is appended on one route and refused on the other
 * would make the same element mean two things depending on how its type was decided.
 * `sub` is CONSUMED (it is JS_UNDEFINED on either refusal). Returns a step code the caller returns. */
static int subkey_answered(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, int base, IdbKeyResult sr,
                           JSValue sub)
{
    /* READ AFTER the conversion and never carried across it: a nested array PUSHES, and `js_realloc` may have
       moved the stack under a pointer taken before. */
    IdbKeyLevel *f = &w->lv[w->sp - 1];

    /* STEP 5.6: "If key is 'invalid value' or 'invalid type' abort these steps and return 'invalid value'."
       The two refusals are ONE answer here, which is the standard's own collapse and the only place in it
       that makes one. */
    if (sr != IDB_KEY_OK) {
        DCHECK(JS_IsUndefined(sub), "§7.4's arms left a key behind on a refusal");
        if (w->multi) return multi_skip(ctx, hdr, w, base);
        return walk_finish(ctx, hdr, w, IDB_KEY_INVALID_VALUE, JS_UNDEFINED);
    }
    level_append_subkey(ctx, w, f, sub);   /* STEP 5.7 */
    f->index++;                            /* STEP 5.8 */
    IDB_KW_GOTO(hdr, base + IDB_KW_HOP);
    return JS_STEP_YIELD;
}

/* ---- the entry, the run and the answer -------------------------------------------------------------------- */

static void walk_start(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, JSValueConst input, int base, int after,
                       bool multi)
{
    JSValue arr = JS_UNDEFINED;

    walk_release(ctx, w);
    w->multi = multi ? 1 : 0;
    w->after = after;
    w->entry = JS_UNDEFINED;
    w->unknown = JS_UNDEFINED;
    w->key = JS_UNDEFINED;
    /* STEP 1: "If seen was not given, then let seen be a new empty set." Every §4 member's call is the
       one-argument form, so the set is fresh per conversion and §4.7's `bound` converts its two values
       independently. */
    w->seen = JS_NewArray(ctx);
    CHECK(!JS_IsException(w->seen), "IndexedDB: §7.4's `seen` set could not be allocated");
    /* STEP 2 is vacuous over a fresh set, and STEP 3's arms that run none of the page's code answer here. */
    w->res = idb_key_convert_here(ctx, input, &w->key, &arr, &w->unknown);
    /* THE ARM WHOSE ANSWER IS UNDECIDED RESTS, EXACTLY AS THE ARRAY ARM WALKS. Both are handed back by the
       same function for the same reason — they need a FLOW, one to run the page's own index accessors and
       one to snapshot a sibling at — and both are pointed into this block rather than at the caller's own
       stage. The top level stands on NO LEVEL here, which is what the UNKNOWN stage reads to tell the
       caller's answer from a subkey's. */
    if (w->res == IDB_KEY_UNKNOWN) {
        DCHECK(JS_IsUndefined(w->key) && JS_IsUndefined(arr),
               "§7.4's arms handed back an undecided value and a key or an array with it — the three answers "
               "are exclusive and a caller reading one slot would act on another arm's operand");
        w->res = IDB_KEY_OK;   /* nothing is decided yet: the fork below is what answers */
        hdr->stage = (uint16_t)(base + IDB_KW_UNKNOWN);
        return;
    }
    if (w->res != IDB_KEY_ARRAY) {
        hdr->stage = (uint16_t)after;
        return;
    }
    w->res = IDB_KEY_OK;   /* nothing is decided yet: the walk below is what answers */
    walk_push(ctx, w, arr);
    hdr->stage = (uint16_t)(base + IDB_KW_LENGTH);
}

void idb_key_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, JSValueConst input, int base, int after)
{
    walk_start(ctx, hdr, w, input, base, after, false);
}

/* "If input is an Array exotic object, then: ... Otherwise, return the result of converting a value to a key
   with argument input." Both arms are the entry above with the mode set: a non-Array never reaches a level, so
   the flag decides nothing for it, and an Array's top level is the one this mode changes. */
void idb_key_walk_start_multi_entry(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, JSValueConst input,
                                    int base, int after)
{
    walk_start(ctx, hdr, w, input, base, after, true);
}

/* §7.4 OVER A VALUE THIS FLOW HAS NO KEY BYTES FOR — the FORK, and the one stage of this algorithm that is
 * not a step of the standard.
 *
 * WHY THERE IS A WORLD TO FORK OVER AT ALL. §7.4 decides a key's TYPE by asking what the value IS, and a page
 * keys its records by what it read from the address, from a message, from a reply — so the value arriving at
 * step 3 is routinely one whose bytes this flow does not have. Asking "is it a Number" of such a value answers
 * no, because a concolic rides an Object, and so does every other test: the whole surface would be a
 * "DataError" and §4's get, delete, count and openCursor over attacker-derived keys would be out of reach.
 * That answer is not coarse, it is DECIDED — one arm taken and the rest deleted with nothing to say so — and
 * §Solver-half's rule is that where the domain permits several outcomes they all run.
 *
 * HOW MANY WORLDS, AND WHY IT IS NOT THE NUMBER §7.4's PROSE ENUMERATES. The arm list is Number / Date /
 * String / buffer source / Array / Otherwise, which is SIX, and the completions are THREE. Two corrections in
 * opposite directions produce that, and both are read off what a PAGE OBSERVES rather than off the prose:
 *   - §7.4's TWO refusals COLLAPSE, and so do the sub-worlds inside three of its arms. "invalid value" and
 *     "invalid type" are told apart by no algorithm in this standard (core/indexeddb/idb_key.h argues it at
 *     length), so the world in which the unknown is a NaN Number, the one in which it is a Date whose
 *     [[DateValue]] is NaN, the one in which it is a detached buffer source and the one in which it is an
 *     ordinary object are ONE arm: every §4 member answers all four with the same "DataError".
 *   - "a key" DOES NOT COLLAPSE, which is what the crash this replaces got wrong when it asked for "a key arm
 *     and an invalid arm". §2.4's compare reads a key's TYPE before it reads any value — "If ta does not
 *     equal tb" over number < date < string < binary < array — so `indexedDB.cmp(x, 5)` answers from the
 *     type ALONE, with no second unknown in it, and §7.3 hands a number key back as a Number and a string
 *     key back as a String. A page observes which type it got.
 *
 * WHAT BOUNDS IT AT THREE IS WHAT CAN BE MINTED WITHOUT INVENTING, AND THE REST IS A NAMED RESIDUAL.
 * NOT COVERED: the DATE, BINARY and ARRAY arms, and the throw that only the ARRAY arm can reach. §2.4's value
 * for a date key is a double, for a binary key a byte sequence and for an array key a list of other keys, and
 * core/indexeddb/idb_key.c states at its concolic arm why none of the three is a place a concolic can ride —
 * so entering them means minting bytes nothing observed, which is §RUN-DON'T-MATCH's invention, and the array
 * arm additionally needs a `length` that only a bound could supply. The standard's own result sentence names
 * a fourth outcome kind beside its three answers — "or the steps may throw an exception" — and every route
 * to it is a `?` inside the array arm's reads of the page's own object, so it is entailed by that arm rather
 * than separate from it.
 * WHAT THE NEXT DIFF BUILDS: a key record whose value may be a concolic for the DATE type too, which is
 * §2.4's value being made concolic-carrying rather than a new arm here, and with it a fourth completion
 * appended — appended, because solver/decide.h's numbering rule makes an insert a migration.
 * HOW ITS ABSENCE WOULD SHOW: a document that keys its records by a Date built from unknown input, or by a
 * binary key, reaches §4's member and the engine explores no world in which the lookup succeeds — observable
 * as a store whose records are never found on any arm, where a browser finds them on the one that matters.
 *
 * AND THE WORLD THAT IS NOT §7.4's TO FORK OVER AT ALL IS NAMED HERE SO THE NEXT READER DOES NOT WELD IT ON.
 * §2.9's convert-a-value-to-a-key-RANGE is what most of §4 reaches §7.4 through, and its steps 1 and 2 answer
 * BEFORE step 3 runs: "If value is a key range, return value" and "If value is undefined or is null ... return
 * an unbounded key range". An unknown could be either, and those worlds are observably enormous — an
 * unbounded range is EVERY record in the store — while core/indexeddb/idb_key_range.c's `range_convert_pre`
 * answers both of them concretely today, a brand test and a null test over a value that rides an Object. They
 * are not completions of this algorithm: solver/decide.h is explicit that an arm asked by a DIFFERENT
 * algorithm at a different moment is one fork answering two questions, so §2.9 owes a fork of its OWN at its
 * own ask site, and appending its arms to this machine would be migration-safe and wrong.
 *
 * `real` IS JS_OUTCOME_REAL_UNSTATED ON BOTH STATES THAT REACH HERE, AND THAT IS A POSITIVE STATEMENT. A
 * value with NO example has no bytes to run §7.4 against. A CONTRADICTED one has bytes this path's own arm
 * PROVED WRONG, so naming the arm its example reaches would mark as forced the single world the run has
 * already disproved. Both arms still run and neither is marked forced, which is what the sentinel means. */
static int walk_unknown(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, JSValue in, int base)
{
    JSValue unk, key;
    IdbKeyResult res;
    int arm = 0, rc;

    JS_FreeValue(ctx, in);
    DCHECK(concolic_is(w->unknown),
           "§7.4's fork was resumed over an operand that is NOT unknown external input — a value this flow "
           "has bytes for takes idb_key_concrete_arm, which decides its type by asking what it IS, so this "
           "stage holding a known value means the conversion routed an arm it had already answered");
    rc = step_fork_run(ctx, hdr, w->unknown, IDB_KEY_UNKNOWN_ASK, IDB_KEY_UNKNOWN_ARMS,
                       JS_OUTCOME_REAL_UNSTATED, &arm);
    if (rc)
        return rc;   /* JS_STEP_FORK: parked at the ask, and the sibling's snapshot was taken there */
    DCHECK(arm >= 0 && arm < IDB_KEY_UNKNOWN_ARMS,
           "§7.4's fork came back standing at a completion it never declared — the worlds are the refusal and "
           "the two key types whose §2.4 value a concolic can ride, and there is nothing outside them");
    /* THE OPERAND IS TAKEN OFF THE RECORD BEFORE ANYTHING ELSE RUNS, because `step_fork_run` is answered now
       and the next ask on this machine asserts no operand is still held. */
    unk = w->unknown;
    w->unknown = JS_UNDEFINED;
    /* THE ANSWER IS COMPOSED ONCE, because the two routes below carry it to different places and a second
       reading of the arm is a second chance for them to disagree about which world this flow is in.
       §7.4's "Otherwise: return 'invalid type'" is what the refusal arm IS — the engine's own type tests all
       answer no for a value riding an Object — and idb_key.h argues why the standard's OTHER refusal does
       not need an arm of its own. */
    res = arm == IDB_KEY_UNKNOWN_INVALID ? IDB_KEY_INVALID_TYPE : IDB_KEY_OK;
    if (res != IDB_KEY_OK) {
        JS_FreeValue(ctx, unk);
        key = JS_UNDEFINED;
    } else {
        key = idb_key_new_unknown(ctx, (IdbKeyUnknownArm)arm, unk);
        JS_FreeValue(ctx, unk);
    }
    /* WHOSE ANSWER THIS IS, READ OFF THE LEVEL STACK AND NOT OFF A FLAG. The top-level ask happens only where
       the input is NOT an Array exotic object, so idb_key_walk_start pushed nothing and `sp` is 0; a SUBKEY
       ask happens inside step 5.4, which is reached only from a level. The two are mutually exclusive by the
       algorithm's own shape, and `sp` is that shape rather than a second record of it. */
    if (w->sp == 0)
        return walk_finish(ctx, hdr, w, res, key);
    return subkey_answered(ctx, hdr, w, base, res, key);
}

int idb_key_walk_run(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, JSValue in, int base,
                     JSValue **out_cb, int *out_argc)
{
    int phase = hdr->stage - base;
    IdbKeyLevel *f;
    int r;

    DCHECK(phase >= 0 && phase < IDB_KW_N, "§7.4's array arm was resumed at a stage outside the block its "
                                           "caller declared for it");
    /* THE ONE STAGE THAT IS NOT THE ARRAY ARM'S, ANSWERED BEFORE THE LEVEL IS READ. §7.4's fork rests on no
       level when it is the TOP-LEVEL input that is undecided, which is the ordinary case — `store.get(x)`
       and not `store.get([x])` — so the assertion below would fire on it. */
    if (phase == IDB_KW_UNKNOWN)
        return walk_unknown(ctx, hdr, w, in, base);
    DCHECK(w->sp > 0, "§7.4's array arm was driven with no level under it — idb_key_walk_start points the stage "
                      "into this block only after pushing the input's own level, and every completion points it "
                      "back at the caller's. The fork above is the one stage this is not true of, and it is "
                      "answered before this line");
    f = &w->lv[w->sp - 1];

    if (phase == IDB_KW_LENGTH) {
        /* STEP 1: "Let len be ? ToLength(? Get(input, "length"))." */
        JSAtom a = JS_NewAtom(ctx, "length");
        JSValue lenv = JS_UNDEFINED;
        double d = 0;

        CHECK(a != JS_ATOM_NULL, "IndexedDB: §7.4 step 1 could not intern `length`");
        r = step_getprop_run(ctx, hdr, f->src, a, in, &lenv, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        /* The `?`: the page's throw leaves §7.4 entirely — and multiEntry's own step 1.1 spells the same `?`,
           so the TOP level rethrows too. Below it the throw belongs to a nested conversion whose result that
           mode discards. */
        if (r < 0) {
            if (w->multi && w->sp > 1) return multi_skip(ctx, hdr, w, base);
            return JS_STEP_ABRUPT;
        }
        /* AN ARRAY EXOTIC OBJECT'S `length` IS ITS OWN WRITABLE, NON-CONFIGURABLE DATA PROPERTY — the one
           property of an Array a page can neither shadow nor turn into an accessor — so this read cannot have
           run the page's code and ToLength cannot coerce anything. Asserted rather than assumed, because it is
           what makes the arithmetic below the whole of the step. */
        DCHECK(JS_IsNumber(lenv), "§7.4 step 1 read a `length` that is not a number: the array arm's input is "
                                  "an Array exotic object, whose `length` is an own data property, so nothing "
                                  "the page wrote can answer that read");
        JS_ToFloat64(ctx, &d, lenv);
        JS_FreeValue(ctx, lenv);
        f->len = (int64_t)d;
        DCHECK(f->len >= 0 && (double)f->len == d && f->len <= (int64_t)UINT32_MAX,
               "§7.4 step 1's len is not an Array exotic object's length — a length is an integer in "
               "[0, 2^32-1], and every index below is addressed as one");
        IDB_KW_GOTO(hdr, base + IDB_KW_BEGIN);
        return JS_STEP_YIELD;
    }

    if (phase == IDB_KW_BEGIN) {
        JS_FreeValue(ctx, in);
        walk_append(ctx, w->seen, JS_DupValue(ctx, f->src));   /* STEP 2 */
        DCHECK(JS_IsUndefined(f->keys), "§7.4 step 3 was re-entered on a level that already holds its list — "
                                        "the stage is a rest point and not a loop body");
        f->keys = JS_NewArray(ctx);                            /* STEP 3 */
        CHECK(!JS_IsException(f->keys), "IndexedDB: §7.4 step 3's list of subkeys could not be allocated");
        f->index = 0;                                          /* STEP 4 */
        IDB_KW_GOTO(hdr, base + IDB_KW_HOP);
        return JS_STEP_YIELD;
    }

    if (phase == IDB_KW_HOP) {
        JSValue desc = JS_UNDEFINED;

        /* STEP 5: "While index is less than len". The test is re-made on the resume leg of this stage's own
           request and that is sound, unlike the filter idb_open.c's step 10.2 used to re-make there: nothing a
           [[GetOwnProperty]] can run changes `index` or `len`, both of which are this level's own. */
        if (f->index >= f->len) {
            JS_FreeValue(ctx, in);
            IDB_KW_GOTO(hdr, base + IDB_KW_LEAVE);
            return JS_STEP_YIELD;
        }
        /* MULTIENTRY'S TOP LEVEL HAS NO STEP 5.1 AT ALL — its loop is Indexed Database §7.4's "Let entry be
           Get(input, index)." and nothing else — which is why `[10, , 20]` is a two-subkey multiEntry key
           where it is no key at all for the ordinary conversion. Every level below is that ordinary
           conversion and asks. */
        if (w->multi && w->sp == 1) {
            JS_FreeValue(ctx, in);
            IDB_KW_GOTO(hdr, base + IDB_KW_ENTRY);
            return JS_STEP_YIELD;
        }
        /* §7.4 STEP 5.1: "Let hop be ? HasOwnProperty(input, index)." — ECMAScript §7.3.13, which is
           [[GetOwnProperty]] and a test of whether the descriptor is undefined. The property key is
           ToPropertyKey of the NUMBER, which is its canonical numeric string. The atom is held ON THE WALK
           across the request (see hop_atom) and released only once it has answered. */
        if (w->hop_atom == JS_ATOM_NULL) {
            w->hop_atom = JS_NewAtomUInt32(ctx, (uint32_t)f->index);
            CHECK(w->hop_atom != JS_ATOM_NULL, "IndexedDB: §7.4 step 5.1 could not intern an array index");
        }
        r = step_getownprop_run(ctx, hdr, f->src, w->hop_atom, in, &desc, out_cb, out_argc);
        if (r > 0) return r;   /* the atom stays held: the request carries it BORROWED */
        JS_FreeAtom(ctx, w->hop_atom);
        w->hop_atom = JS_ATOM_NULL;
        if (r < 0) {
            if (w->multi) return multi_skip(ctx, hdr, w, base);
            return JS_STEP_ABRUPT;
        }
        /* STEP 5.2: "If hop is false, return 'invalid value'." A HOLE is what this refuses, which is why
           `[1, , 3]` is not a key and `[1, undefined, 3]` is refused one step later for its type instead. */
        if (JS_IsUndefined(desc)) {
            JS_FreeValue(ctx, desc);
            if (w->multi) return multi_skip(ctx, hdr, w, base);
            return walk_finish(ctx, hdr, w, IDB_KEY_INVALID_VALUE, JS_UNDEFINED);
        }
        JS_FreeValue(ctx, desc);
        IDB_KW_GOTO(hdr, base + IDB_KW_ENTRY);
        return JS_STEP_YIELD;
    }

    if (phase == IDB_KW_ENTRY) {
        /* STEP 5.3: "Let entry be ? Get(input, index)." THE PAGE'S CODE: step 5.1 proved the property is the
           array's OWN, and an own array index may perfectly well be an accessor. */
        JSAtom a = JS_NewAtomUInt32(ctx, (uint32_t)f->index);

        CHECK(a != JS_ATOM_NULL, "IndexedDB: §7.4 step 5.3 could not intern an array index");
        JS_FreeValue(ctx, w->entry);
        w->entry = JS_UNDEFINED;
        r = step_getprop_run(ctx, hdr, f->src, a, in, &w->entry, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        if (r > 0) return r;
        /* MULTIENTRY'S TOP-LEVEL READ IS NOT `?` — "let entry be Get(input, index); if entry is not an abrupt
           completion, then:" — so a throwing accessor drops that one member and the conversion goes on. */
        if (r < 0) {
            if (w->multi) return multi_skip(ctx, hdr, w, base);
            return JS_STEP_ABRUPT;
        }
        IDB_KW_GOTO(hdr, base + IDB_KW_SUBKEY);
        return JS_STEP_YIELD;
    }

    if (phase == IDB_KW_SUBKEY) {
        JSValue sub = JS_UNDEFINED, arr = JS_UNDEFINED;
        IdbKeyResult sr;

        JS_FreeValue(ctx, in);
        /* STEP 5.4, whose first two steps are §7.4's own 1 and 2 over `entry`: the set is the one this walk is
           carrying, so an array that is already in it is a CYCLE (or a repeat) and the answer is "invalid
           value" — reached here rather than at the push, because it is a step of the recursive call and not of
           this level's loop. */
        if (walk_seen_contains(ctx, w, w->entry)) {
            if (w->multi) return multi_skip(ctx, hdr, w, base);
            return walk_finish(ctx, hdr, w, IDB_KEY_INVALID_VALUE, JS_UNDEFINED);
        }
        /* NOTHING IS OUTSTANDING WHEN THE CONVERSION IS ASKED, which is what makes it safe to let it write
           the operand slot directly: `idb_key_convert_here` PLACES that slot on entry, so a fork still
           standing here would have its operand dropped with no reference left and the sibling would resume
           over a freed value. The UNKNOWN stage takes the operand off the moment the ask is answered, and the
           top-level ask cannot coexist with a level, so the slot is empty at every route to this line. */
        DCHECK(JS_IsUndefined(w->unknown),
               "§7.4's array arm reached step 5.4 with a fork's operand still on its record — the UNKNOWN "
               "stage takes it off as soon as the ask is answered, so one standing here is a fork whose arm "
               "nothing consumed, and the conversion below is about to overwrite the slot it lives in");
        sr = idb_key_convert_here(ctx, w->entry, &sub, &arr, &w->unknown);
        if (sr == IDB_KEY_ARRAY) {
            /* THE RECURSION, AS A PUSH. `f` is not read again: the stack may have moved. */
            JS_FreeValue(ctx, w->entry);
            w->entry = JS_UNDEFINED;
            walk_push(ctx, w, arr);
            IDB_KW_GOTO(hdr, base + IDB_KW_LENGTH);
            return JS_STEP_YIELD;
        }
        JS_FreeValue(ctx, w->entry);
        w->entry = JS_UNDEFINED;
        /* AN ELEMENT WHOSE OWN §7.4 ANSWER IS UNDECIDED RESTS AT THE FORK, exactly as the top-level input
           does — `IDBKeyRange.only([x])` owes the same three worlds `only(x)` does, one level in. The level
           stays standing, so the stage reached there reads `sp > 0` and finishes step 5.6-5.8 through the
           same tail this arm does. */
        if (sr == IDB_KEY_UNKNOWN) {
            DCHECK(JS_IsUndefined(sub), "§7.4's arms handed back an undecided subkey and a key with it");
            IDB_KW_GOTO(hdr, base + IDB_KW_UNKNOWN);
            return JS_STEP_YIELD;
        }
        return subkey_answered(ctx, hdr, w, base, sr, sub);   /* STEPS 5.6-5.8 */
    }

    DCHECK(phase == IDB_KW_LEAVE, "§7.4's array arm was re-entered at a phase it never rests at");
    {
        /* STEP 6: "Return a new array key with value keys." The level LEAVES: its key is the parent's step
           5.7 subkey, or — at the bottom of the stack — the algorithm's answer. */
        JSValue key;

        JS_FreeValue(ctx, in);
        DCHECK(f->index == f->len, "§7.4 step 6 was reached with elements left to convert");
        key = idb_key_new_array(ctx, f->keys);
        f->keys = JS_UNDEFINED;
        JS_FreeValue(ctx, f->src);
        f->src = JS_UNDEFINED;
        w->sp--;
        if (w->sp == 0)
            return walk_finish(ctx, hdr, w, IDB_KEY_OK, key);
        f = &w->lv[w->sp - 1];
        level_append_subkey(ctx, w, f, key);   /* the PARENT's step 5.7 */
        f->index++;                            /* and its step 5.8 */
        IDB_KW_GOTO(hdr, base + IDB_KW_HOP);
        return JS_STEP_YIELD;
    }
}

IdbKeyResult idb_key_walk_result(JSContext *ctx, IdbKeyWalk *w, JSValue *pkey)
{
    (void)ctx;
    DCHECK(w->res != IDB_KEY_UNKNOWN && w->res != IDB_KEY_ARRAY,
           "§7.4's answer was taken while one of this engine's two HAND-BACKS was still standing in it — "
           "neither is one of the algorithm's answers, and every route out of them replaces it with one");
    DCHECK(JS_IsUndefined(w->unknown),
           "§7.4's answer was taken with the fork's operand still on the record — the UNKNOWN stage takes it "
           "off the moment the fork is answered, so one left here is a fork whose arm nothing consumed");
    DCHECK(w->sp == 0, "§7.4's answer was taken while its walk still stands on a level — the algorithm points "
                       "the stage at the caller's own only once every level has left");
    *pkey = JS_UNDEFINED;
    if (w->res != IDB_KEY_OK)
        return w->res;
    DCHECK(JS_IsObject(w->key), "§7.4 answered with a key that is not a key record");
    *pkey = w->key;
    w->key = JS_UNDEFINED;
    return IDB_KEY_OK;
}

int idb_key_walk_take(JSContext *ctx, IdbKeyWalk *w, JSValue *pkey)
{
    if (idb_key_walk_result(ctx, w, pkey) == IDB_KEY_OK) return 0;
    JS_ThrowDOMException(ctx, "DataError", "the value is not a valid IndexedDB key");
    return -1;
}
