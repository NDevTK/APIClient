/* See console_label.h. The Console Standard, https://console.spec.whatwg.org/.
 *
 * WHAT THIS REPLACED, STATED PRECISELY, BECAUSE THREE OF THE FIVE MEMBERS DID NOT CRASH AND THAT IS THE PART
 * THAT MATTERS. `JS_ValueToAtom` on unknown external input does not abort: it reaches ECMAScript §7.1.21
 * ToPropertyKey ( arg ), whose concolic arm answers with the value's own display SHAPE as a real string
 * (quickjs's JS_ToPropertyKeyInternal, and JSConcolicHooks.key_name behind it). So `console.time(x)` over an
 * unknown `x` ran to completion: it filed a timer under the slot `{x}`, and §1.4.1's "If the associated timer
 * table contains an entry with key label" was answered NO for every label the page had ever started — not
 * approximately, but by CONSTRUCTION, because a shape is never one of the map's own keys. §1.2.1 and §1.2.2
 * reached the same collapse behind a type assert that fired first, and §1.4.2/§1.4.3 reached it and then died
 * one step LATER, at the JS_ToCString that reads the label back for the printed concatenation.
 * That is three different symptoms — a silent wrong answer, an abort naming a type, and an abort naming a byte
 * consumer — for ONE missing question, which is why repairing the assert alone would have moved the crash
 * rather than the defect.
 *
 * THE SHAPE-AS-KEY MODEL IS KEPT AND IS THE REMAINDER'S ANSWER. Once every existing key has been eliminated,
 * the label names none of them, and it must still name a STABLE slot — §1.2.1's "Otherwise, set map[label] to
 * 1" writes one and a later `console.count(x)` on the same path must find it. That is exactly what §7.1.21's
 * concolic arm provides: a real string, stable per source, so two lookups through one source agree and two
 * sources never collide. The defect was never the minting; it was minting WITHOUT ASKING.
 *
 * WHY AN ELIMINATION CHAIN AND NOT AN N-WAY COMPLETION. solver/decide.c can walk N-1 binary decisions for a
 * machine declaring `n`, but solver_outcome composes each completion's key out of `"%d"` of its POSITION — and
 * a console map is a set the PAGE mutates, so a replayed position names a different label at the second ask.
 * A sequence of two-armed asks, each keyed by the label's own text, is the SAME elimination with every
 * completion carrying its name; `n == 2` at every link also keeps a page holding many labels away from the
 * return protocol's ceiling (solver/decide.h's SOLVER_FORKED_BIT). core/idl_name_chain.c holds the link.
 *
 * THE ORDER IS THE SNAPSHOT'S AND THE PARENT SITS ON THE EXAMPLE'S ARM AT EVERY LINK. The sequence is a
 * function of the map at the chain's first link, so it is the same sequence in a sibling's snapshot and after
 * a park; the parent answers each question with what its own example says, which marks the real arm primary at
 * every link. With NO example the parent walks the whole chain to the remainder and one sibling per key
 * carries that key's world — neither arm marked forced, because nothing was contradicted. */
#include <stdbool.h>
#include <stdint.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"

#include "core/console/console_label.h"
#include "core/idl_name_chain.h"
#include "solver/concolic.h"

void console_label_chain_visit(JSContext *ctx, ConsoleLabelChain *c, JSStepVisit *v)
{
    DCHECK(c != NULL && v != NULL,
           "a console member named its §1.2/§1.4 label chain with no chain or no visitor — the visit is how a "
           "fork copies the key snapshot and the composed key, so a machine that skips it hands its sibling "
           "pointers into storage the parent's discharge frees");
    /* THE POINTER AND THE COUNT ARE ONE FACT AND THIS IS WHERE THEY ARE READ TOGETHER — the copy takes a
       reference to every atom in the array and the free drops one from each, so a count that has drifted from
       the array either under-counts a reference or walks off the end of it. */
    DCHECK(c->keys != NULL || c->nkeys == 0,
           "a console §1.2/§1.4 label chain holds a key count with no key array — the snapshot writes the two "
           "together and an empty map writes NULL and 0, so a positive count with no array is a snapshot half "
           "taken");
    v->props(ctx, &c->keys, c->nkeys);
    idl_name_chain_supplied_visit(ctx, &c->key, v);
}

/* THE ATOM THIS FLOW'S EXAMPLE WOULD DENOTE, or JS_ATOM_NULL when the run cannot say — a POSITIVE fact the
 * chain reads as JS_OUTCOME_REAL_UNSTATED and never as a label to fall back on.
 *
 * IT IS §7.1.21 ToPropertyKey ( arg ) ITSELF, RUN, which is what makes the comparison exact rather than a
 * rule predicting it: a lookup in §1.2's or §1.4's map converts its key that way, so an example of `7` denotes
 * the member named "7" and an example of `null` the member named "null". Running it here costs nothing of the
 * page's, because an example is a CONCRETE value and ToPropertyKey reaches the page's code only through
 * §7.1.1 ToPrimitive over an OBJECT — which is the one shape excluded below, from a C activation with no flow
 * base under it, exactly as every other converter in this engine refuses it.
 *
 * AN ABSENT EXAMPLE AND AN EXAMPLE THAT IS `undefined` ARE ONE ANSWER HERE and that is the accessor's own
 * conflation rather than this file's choice: concolic_example returns JS_UNDEFINED for both and offers no
 * third answer. Reading it as "no example" is the sound direction — it states less than the run knows and
 * keeps both arms — where reading it as the label "undefined" would decide a link on a fact nobody has. */
static JSAtom console_label_example_atom(JSContext *ctx, JSValueConst label, const char *algorithm)
{
    JSValue ex = concolic_example(ctx, label);
    JSAtom a;

    if (JS_IsUndefined(ex) || JS_IsObject(ex)) {
        JS_FreeValue(ctx, ex);
        return JS_ATOM_NULL;
    }
    a = JS_ValueToAtom(ctx, ex);
    JS_FreeValue(ctx, ex);
    CHECKF(a != JS_ATOM_NULL,
           "%s could not intern the property key its label's own example denotes — every shape reaching this "
           "line is a primitive, whose §7.1.21 ToPropertyKey ( arg ) runs none of the page's code and has "
           "no failing arm but allocation", algorithm);
    return a;
}

/* THE LOOP, SPLIT OUT SO THE EXAMPLE'S ATOM HAS EXACTLY ONE RELEASE POINT. Every arm below either returns a
   step code the caller returns unchanged or answers with a key, and both of those leave through
   console_label_run, which is the one place that frees what it interned. */
static int console_label_chain(JSContext *ctx, JSStepHdr *hdr, ConsoleLabelChain *c, JSValueConst label,
                               JSAtom ex, const char *algorithm, JSAtom *pkey)
{
    for (;;) {
        const char *name;
        JSAtom cand;
        int real, rc;
        bool yes = false;

        if (c->next >= c->nkeys) {
            /* EVERY KEY ELIMINATED, WHICH IS THE ALGORITHMS' OWN "otherwise" WORLD: §1.2.1's "Otherwise, set
               map[label] to 1", §1.2.2's "Otherwise:" message, §1.4.1's "Otherwise, set the value of the entry
               with key label … to the current time", and the undefined `startTime` §1.4.2/§1.4.3 read. The
               label still needs a slot, and §7.1.21's concolic arm is what names one — stable per source, so a
               second ask through the same source finds what the first wrote. It is also the whole answer for
               an EMPTY map, which is why an empty map asks no question: one feasible completion is not a fork,
               it is the answer. */
            *pkey = JS_ValueToAtom(ctx, label);
            CHECKF(*pkey != JS_ATOM_NULL,
                   "%s could not intern the slot its unknown label denotes — ECMAScript §7.1.21 ToPropertyKey "
                   "( arg ) answers an unknown with its display shape as a real String, so the only failing "
                   "arm here is allocation", algorithm);
#if APICLIENT_DEV
            {
                /* THE ONE WAY THIS WORLD CAN BE WRONG, AND IT IS WRONG RATHER THAN NARROW, SO IT CRASHES. The
                   remainder's slot is the label's display shape, and a shape is a string a page may also spell
                   by hand — `console.time("{location.hash}")`. If it does, the chain has just established that
                   the label is NOT that key and the write would go to that very entry, so the flow would act
                   against a fact its own decision vector records. Nothing downstream could notice: every arm
                   is in range and the map is a real map. The remedy is NOT in this file — §7.1.21's concolic
                   arm owes an unforgeable spelling for the slot an unknown key denotes, and it is the one
                   place that can give one, because it is the one place all of `o[x] = 1`, `x in o`,
                   `delete o[x]` and every key-taking builtin converge on. */
                uint32_t i;

                for (i = 0; i < c->nkeys; i++)
                    DCHECKF(c->keys[i].atom != *pkey,
                            "%s eliminated the key an unknown label's own display shape spells, and then wrote "
                            "under it — the page named a real entry with the same text this engine gives the "
                            "unknown's slot, so the flow's vector says the label is not that entry while its "
                            "write says it is. The shape a page cannot forge is JS_ToPropertyKeyInternal's to "
                            "mint, not this component's to work around", algorithm);
            }
#endif
            return 0;
        }
        cand = c->keys[c->next].atom;
        /* THE MEMBER'S NAME, READ OUT OF THE SNAPSHOT'S ATOM. An atom is already a string, so this runs none
           of the page's code; it is BORROWED for the length of the ask, which copies its bytes into the
           composed key, and released before this body returns through any arm. */
        name = JS_AtomToCString(ctx, cand);
        CHECKF(name != NULL, "%s could not read the name of a key its own snapshot holds", algorithm);
        /* THE MACHINE'S SECOND DECLARATION — which completion this operation reaches on the operand's own
           EXAMPLE, computed by RUNNING §7.1.21 above rather than by a rule predicting it. Atoms are interned,
           so equality of the two is equality of the strings they name. */
        real = (ex != JS_ATOM_NULL) ? (ex == cand) : JS_OUTCOME_REAL_UNSTATED;
        rc = idl_name_chain_ask_supplied(ctx, hdr, &c->key, label, CONSOLE_LABEL_PREDICATE, name, real,
                                         algorithm, &yes);
        JS_FreeCString(ctx, name);
        if (rc)
            return rc;
        /* NAMED RESIDUAL — the DECISION is recorded and the value's DOMAIN is not, which is narrower than
           §Solver's concretize-on-pin and is not wrong: an unnarrowed value keeps arms, and keeping an arm is
           the sound direction. WHAT IS NOT COVERED: each link is an equality over the label — the YES arm
           proves it IS this key and the NO arm proves it is NOT — and neither fact reaches the VALUE; only the
           decision vector holds them, and a vector answers the question it recorded and no other.
           WHAT THE NEXT DIFF BUILDS: the pin and the exclusion solver/decide.c already takes at a bytecode
           equality, taken over the OPERAND's own identity rather than off a comparison RESULT this seam does
           not have — the same piece core/timing/timer.c and core/idl_index_arg.c each name at their own
           chains, and ONE piece for all three.
           HOW ITS ABSENCE SHOWS: a BYTECODE test on the same value after the chain has answered —
           `if (lbl === "MyTimer")` following the `console.timeEnd(lbl)` that established it — forks BOTH arms,
           because decide.c keys a comparison off the comparison RESULT's identity and nothing joins that to
           what this chain recorded; and an @H parameter carrying this label renders with no domain beside it
           where the run observed one. */
        if (yes) {
            /* THIS WORLD'S ANSWER: the label IS this key. The atom came out of this machine's own snapshot, so
               it is a name this component holds a reference to for as long as the chain lives; the caller gets
               its own. */
            *pkey = JS_DupAtom(ctx, cand);
            return 0;
        }
        /* ELIMINATED. The cursor is advanced AFTER the answer and never before the ask, which is what makes the
           two arms agree: the sibling's snapshot was taken with the cursor still at `next`, so it re-asks about
           the same key and answers the other way. */
        c->next++;
    }
}

int console_label_run(JSContext *ctx, JSStepHdr *hdr, ConsoleLabelChain *c, JSValueConst label,
                      JSValueConst map, const char *algorithm, JSAtom *pkey)
{
    JSAtom ex;
    int rc;

    DCHECK(algorithm != NULL && algorithm[0] != '\0',
           "a console §1.2/§1.4 label question was asked for a member that did not name itself — the name is "
           "the ADDRESS every assert made on its behalf reports, so an unnamed one aborts naming no site and "
           "sends its reader to a shared component instead of to the member that reached it");
    DCHECKF(c != NULL && pkey != NULL,
            "%s asked the §1.2/§1.4 label question with no chain to park on or nowhere to put its answer — the "
            "chain's storage must be on the machine's state because the fork copies it and the park carries "
            "it", algorithm);
    DCHECKF(JS_IsObject(map),
            "%s asked the §1.2/§1.4 label question about something that is not a map — §1.2's count map and "
            "§1.4's timer table are the two, and both are built as null-prototype objects by this namespace's "
            "realm intrinsic", algorithm);
    /* THE ARGUMENT'S TWO LEGITIMATE SHAPES, AND THE ASSERT THAT USED TO NAME ONLY THE FIRST. Web IDL §3.2.10
       DOMString converts the page's value and §3.6 Overload resolution algorithm places the declaration's
       `= "default"` when the page reached no value at all, so a KNOWN label is a String; and unknown external
       input CROSSES that conversion as itself (core/idl_args.h's rule), so an unknown label reaches the body
       still unknown. An assert naming only the String was therefore false of every call this component exists
       for — it is the assert engine/argaudit.mjs reported as ASSERT-NO-UNKNOWN — and it stood in front of a
       body that had no correct crash behind it. */
    DCHECKF(JS_IsString(label) || concolic_is(label),
            "%s reached its body with a `label` that is neither a String nor unknown external input — the IDL "
            "declares `optional DOMString label = \"default\"`, so §3.2.10 DOMString converted the page's "
            "value or §3.6 placed the default, and unknown input crosses that conversion as itself; a third "
            "shape means this position was never declared a DOMString", algorithm);

    /* A KNOWN LABEL DENOTES EXACTLY ONE KEY AND THERE IS NOTHING TO ASK. This is routing and not a fallback:
       there is no second implementation of the membership question for it to select against — the map lookup
       the member performs with this atom IS the answer — and asking the chain here would fork a sibling per
       entry over a string the flow already holds. */
    if (!concolic_is(label)) {
        *pkey = JS_ValueToAtom(ctx, label);
        CHECKF(*pkey != JS_ATOM_NULL,
               "%s could not intern a label this arm has already established is a real String — §7.1.21 "
               "ToPropertyKey ( arg ) over a String is the identity and has no failing arm but "
               "allocation", algorithm);
        return 0;
    }

    /* THE SNAPSHOT, TAKEN ONCE, AT THE FIRST LINK. See the header for why the set must be fixed before a
       cursor may name a position in it. It is taken from a map this component's own realm intrinsic built as a
       null-prototype object that nothing reachable from the page holds — so it can be no Proxy, this walk runs
       none of the page's code, and the drive-to-completion that a C-side key walk over a page object would be
       (quickjs-step.h's step_ownkeys_run states that case) does not arise here.
       STRING KEYS ONLY, because §1.2's map is "a map of strings to numbers" and §1.4's "a map of strings to
       times" — a Symbol could not have been put there by any of the five members, and asking whether a label
       is one would be a question with no world behind it. */
    if (!c->taken) {
        CHECKF(JS_GetOwnPropertyNames(ctx, &c->keys, &c->nkeys, map, JS_GPN_STRING_MASK) == 0,
               "%s could not take the own-key snapshot of the map its label question is about", algorithm);
        c->taken = 1;
        DCHECKF(c->keys != NULL || c->nkeys == 0,
                "%s was handed a key count with no key array by its own snapshot", algorithm);
    }

    ex = console_label_example_atom(ctx, label, algorithm);
    rc = console_label_chain(ctx, hdr, c, label, ex, algorithm, pkey);
    JS_FreeAtom(ctx, ex);
    return rc;
}
