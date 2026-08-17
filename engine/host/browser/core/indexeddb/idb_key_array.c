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

/* THE PHASES, FROM THE SAME X-LIST EVERY CALLER'S LABELS COME FROM. A caller's block holds the six in this
   order and the body below is written against the OFFSET into it, so this file names no caller's constants and
   the order cannot drift from the labels — they are one declaration. */
enum { IDB_KEY_ARRAY_ALGO_STAGES(JS_STEP_STAGE_ENUM, IDB_KW, "") IDB_KW_N };

/* EVERY REQUEST THIS ALGORITHM CAN HAVE IN FLIGHT, LISTED ONCE — the header's keyed-read cursor (step 1's and
   step 5.3's Get) and its own-descriptor cursor (step 5.1's HasOwnProperty). Written out at each of the six
   transitions they would be six statements of one fact. */
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
    walk_release(ctx, w);
    w->key = key;
    w->res = res;
    IDB_KW_GOTO(hdr, after);
    return JS_STEP_YIELD;
}

/* ---- the entry, the run and the answer -------------------------------------------------------------------- */

void idb_key_walk_start(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, JSValueConst input, int base, int after)
{
    JSValue arr = JS_UNDEFINED;

    walk_release(ctx, w);
    w->after = after;
    w->entry = JS_UNDEFINED;
    w->key = JS_UNDEFINED;
    /* STEP 1: "If seen was not given, then let seen be a new empty set." Every §4 member's call is the
       one-argument form, so the set is fresh per conversion and §4.7's `bound` converts its two values
       independently. */
    w->seen = JS_NewArray(ctx);
    CHECK(!JS_IsException(w->seen), "IndexedDB: §7.4's `seen` set could not be allocated");
    /* STEP 2 is vacuous over a fresh set, and STEP 3's arms that run none of the page's code answer here. */
    w->res = idb_key_convert_here(ctx, input, &w->key, &arr);
    if (w->res != IDB_KEY_ARRAY) {
        hdr->stage = (uint16_t)after;
        return;
    }
    w->res = IDB_KEY_OK;   /* nothing is decided yet: the walk below is what answers */
    walk_push(ctx, w, arr);
    hdr->stage = (uint16_t)(base + IDB_KW_LENGTH);
}

int idb_key_walk_run(JSContext *ctx, JSStepHdr *hdr, IdbKeyWalk *w, JSValue in, int base,
                     JSValue **out_cb, int *out_argc)
{
    int phase = hdr->stage - base;
    IdbKeyLevel *f;
    int r;

    DCHECK(phase >= 0 && phase < IDB_KW_N, "§7.4's array arm was resumed at a stage outside the block its "
                                           "caller declared for it");
    DCHECK(w->sp > 0, "§7.4's array arm was driven with no level under it — idb_key_walk_start points the stage "
                      "into this block only after pushing the input's own level, and every completion points it "
                      "back at the caller's");
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
        if (r < 0) return JS_STEP_ABRUPT;      /* the `?`: the page's throw leaves §7.4 entirely */
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
        /* STEP 5.1: "Let hop be ? HasOwnProperty(input, index)" — 7.3.13, which is [[GetOwnProperty]] and a
           test of whether the descriptor is undefined. The property key is ToPropertyKey of the NUMBER, which
           is its canonical numeric string. The atom is held ON THE WALK across the request (see hop_atom) and
           released only once it has answered. */
        if (w->hop_atom == JS_ATOM_NULL) {
            w->hop_atom = JS_NewAtomUInt32(ctx, (uint32_t)f->index);
            CHECK(w->hop_atom != JS_ATOM_NULL, "IndexedDB: §7.4 step 5.1 could not intern an array index");
        }
        r = step_getownprop_run(ctx, hdr, f->src, w->hop_atom, in, &desc, out_cb, out_argc);
        if (r > 0) return r;   /* the atom stays held: the request carries it BORROWED */
        JS_FreeAtom(ctx, w->hop_atom);
        w->hop_atom = JS_ATOM_NULL;
        if (r < 0) return JS_STEP_ABRUPT;
        /* STEP 5.2: "If hop is false, return 'invalid value'." A HOLE is what this refuses, which is why
           `[1, , 3]` is not a key and `[1, undefined, 3]` is refused one step later for its type instead. */
        if (JS_IsUndefined(desc)) {
            JS_FreeValue(ctx, desc);
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
        if (r < 0) return JS_STEP_ABRUPT;
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
        if (walk_seen_contains(ctx, w, w->entry))
            return walk_finish(ctx, hdr, w, IDB_KEY_INVALID_VALUE, JS_UNDEFINED);
        sr = idb_key_convert_here(ctx, w->entry, &sub, &arr);
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
        /* STEP 5.6: "If key is 'invalid value' or 'invalid type' abort these steps and return 'invalid
           value'." The two refusals are ONE answer here, which is the standard's own collapse and the only
           place in it that makes one. */
        if (sr != IDB_KEY_OK) {
            DCHECK(JS_IsUndefined(sub), "§7.4's arms left a key behind on a refusal");
            return walk_finish(ctx, hdr, w, IDB_KEY_INVALID_VALUE, JS_UNDEFINED);
        }
        walk_append(ctx, f->keys, sub);   /* STEP 5.7 */
        f->index++;                       /* STEP 5.8 */
        IDB_KW_GOTO(hdr, base + IDB_KW_HOP);
        return JS_STEP_YIELD;
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
        walk_append(ctx, f->keys, key);   /* the PARENT's step 5.7 */
        f->index++;                       /* and its step 5.8 */
        IDB_KW_GOTO(hdr, base + IDB_KW_HOP);
        return JS_STEP_YIELD;
    }
}

int idb_key_walk_take(JSContext *ctx, IdbKeyWalk *w, JSValue *pkey)
{
    DCHECK(w->sp == 0, "§7.4's answer was taken while its walk still stands on a level — the algorithm points "
                       "the stage at the caller's own only once every level has left");
    if (w->res == IDB_KEY_OK) {
        DCHECK(JS_IsObject(w->key), "§7.4 answered with a key that is not a key record");
        *pkey = w->key;
        w->key = JS_UNDEFINED;
        return 0;
    }
    *pkey = JS_UNDEFINED;
    JS_ThrowDOMException(ctx, "DataError", "the value is not a valid IndexedDB key");
    return -1;
}
