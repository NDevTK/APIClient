/* HTML §4.10.7 "The select element" — see html_select.h for why `remove` is a component and what it answered
   before this file existed. */
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "core/agent_state.h"
#include "core/dom/node.h"
#include "core/html/html_form.h"     /* §4.10.7's own "get the list of options" walk */
#include "core/html/html_select.h"
#include "core/idl_args.h"

/* §4.10.7's `remove`, which is ONE declaration carrying BOTH of the section's entries — see html_select.h. */
static int g_id_remove = -1;

/* THE NAMESPACE IS PART OF THE QUESTION — a `select` in another namespace is a different element with a
   different interface, the same test core/html/html_progress.c makes for `<progress>`. */
static bool select_element_is(const lxb_dom_node_t *n)
{
    size_t len = 0;
    const lxb_char_t *name;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || n->ns != LXB_NS_HTML) return false;
    name = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return name && len == 6 && memcmp(name, "select", 6) == 0;
}

/* WEB IDL §3.7.7 "Operations"' BRAND CHECK — create-an-operation-function's own step over the receiver, which
   reads "If jsValue does not implement the interface target, throw a TypeError". A member is on a prototype
   and a page can call it on anything, so the receiver is a real question with a real spec answer rather than
   an invariant to assert: a `DCHECK` here would hand any page an abort switch, which is what CLAUDE.md
   §WHOSE-BYTES-STATE-THE-VALUE forbids for a value the page states.
   THE WORDS ARE THE SECTION'S NOW AND WERE THIS FILE'S BEFORE. What stood here was a paraphrase punctuated as
   a quotation: it spelled the receiver `thisValue` and the interface `the interface on which the operation
   was declared`, where §3.7.7 says `jsValue` and `target`. engine/citegen.mjs reported it on the first run,
   which is the quotation channel doing the one thing a reader trusts most and verifies least. */
static lxb_dom_node_t *select_receiver(JSContext *ctx, JSValueConst this_val, const char *member)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (select_element_is(n)) return n;
    JS_ThrowTypeError(ctx, "HTMLSelectElement.%s was reached on something that is not a <select> element",
                      member);
    return NULL;
}

/* The length of the JS Array §4.10.7's walk returns — the same read core/html/html_form.c makes of its own
   control lists, and not a second answer to anything: the ARRAY is the list of options and this is its size. */
static uint32_t option_list_len(JSContext *ctx, JSValueConst list)
{
    JSValue lv = JS_GetPropertyStr(ctx, list, "length");
    uint32_t n = 0;

    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

/* HTML §2.6.4.3 "The HTMLOptionsCollection interface"'s REMOVE AN OPTION, verbatim and in its own order:
 *   1. "If the number of nodes represented by collection is 0, then return."
 *   2. "If index is not a number greater than or equal to 0 and less than the number of nodes represented by
 *       collection, then return."
 *   3. "Let element be the indexth element in collection."
 *   4. "Remove element from its parent node."
 * — which is what §2.6.4.3's own `remove(index)` method steps are, and therefore what §4.10.7's one-argument
 * entry "must act like". Step 1 is not subsumed by step 2 in the STANDARD's text and is not written as if it
 * were here, because reading them as one is how a reader loses the fact that this algorithm never throws.
 *
 * THE COLLECTION IS THE LIST. §4.10.7 defines the `options` collection's filter as "the elements in the list
 * of options", so "the number of nodes represented by collection" is that list's size and "the indexth
 * element in collection" is its indexth entry. The HTMLOptionsCollection OBJECT carries no state of its own
 * that either read consults.
 *
 * NO SELECTEDNESS RESET IS RUN HERE, AND THAT IS A DECISION THIS FILE INHERITS RATHER THAN MAKES.
 * core/html/html_option.h states it: §4.10.7's selectedness setting algorithm is IDEMPOTENT and writes only
 * the selectedness slot, so this engine runs it at every READ of that state instead of at each of the moments
 * §4.10.7 names — of which "options are added or removed" is one. Running it here as well would be a second
 * site for one write.
 */
static void remove_an_option(JSContext *ctx, lxb_dom_node_t *select, double index)
{
    JSValue list = html_form_select_option_list(ctx, select);
    uint32_t len = option_list_len(ctx, list);
    JSValue element;
    lxb_dom_node_t *n;

    /* STEPS 1 AND 2. The comparison is over the DOUBLE the conversion produced and not over a cast of it: Web
       IDL §3.2.4.5 long is ConvertToInt(V, 32, "signed"), so the value is any integer in [-2**31, 2**31-1] and
       a negative one is an ordinary call rather than an error — `select.remove(-1)` is step 2's return arm. */
    if (len == 0 || !(index >= 0) || !(index < (double)len)) { JS_FreeValue(ctx, list); return; }

    element = JS_GetPropertyUint32(ctx, list, (uint32_t)index);      /* STEP 3 */
    n = node_of(element);
    /* STEP 4. `dom_cow_remove_child` is the removal chokepoint every tree mutation in this engine goes
       through, so the per-flow COW delta captures this one as it captures theirs.
       THE PARENT IS THIS CODEBASE'S OWN INVARIANT AND NOT PAGE INPUT, which is what makes an assert the right
       thing here and the wrong thing at the argument above: §4.10.7's walk descends from the select, so every
       node it returns is a DESCENDANT of it and has a parent by construction. */
    DCHECK(n && n->parent,
           "§4.10.7's list of options held an entry that is not a node with a parent — the walk descends from "
           "the select itself, so every option it returns is a descendant of it and §2.6.4.3's remove an "
           "option step 4 has a parent node to remove from");
    dom_cow_remove_child(n);
    JS_FreeValue(ctx, element);
    JS_FreeValue(ctx, list);
}

/* §4.10.7's `remove`, BOTH ENTRIES, with Web IDL §3.6 steps 3-4 read off `argc`.
 *
 * `argc` IS §3.6's `argcount` AND NOT A COUNT OF WHAT THE PAGE WROTE. The machine hands a non-variadic body
 * min(the page's argument count, the member's declared positions), which is Web IDL §3.6 "Overload resolution
 * algorithm" step 3's "Initialize argcount to be min(maxarg, n)" for a member whose longest type list is its
 * declaration — so `select.remove(0, 1)` is argc 1 and runs the one-argument entry, as a browser does.
 *
 * ZERO ARGUMENTS IS THE ChildNode ENTRY AND IT WAS ALREADY CORRECT. §4.10.7 sends it to "its namesake method
 * on the ChildNode interface implemented by the HTMLSelectElement ancestor interface Element", which is
 * DOM §4.2.8 "Mixin ChildNode"'s `remove()`, and is reached through core/dom/node.h's one implementation of those
 * two steps rather than through a copy here. Installing this member takes that call OFF Element.prototype and
 * answers it here, so the routing has to be exact: it is the same function the mixin body itself calls.
 */
static JSValue js_select_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n = select_receiver(ctx, this_val, "remove");
    double index = 0;
    int have;

    (void)magic;
    if (!n) return JS_EXCEPTION;

    if (argc == 0) { node_child_node_remove(n); return JS_UNDEFINED; }

    /* AN UNKNOWN INDEX IS A FORK THIS BODY CANNOT YET PERFORM, and the shape is core/css/css_rule.h's
       CSS_RULE_INSERT_INDEX exactly — down to the reason the unknown's own EXAMPLE may not decide it. */
    if (concolic_is(argv[0])) {
        DFAIL("HTML §4.10.7 The select element's `remove(index)` was given an UNKNOWN `index`. HTML §2.6.4.3 "
              "The HTMLOptionsCollection interface's remove an option step 2 is: If index is not a number "
              "greater than or equal to 0 and less than the number of nodes represented by collection, then "
              "return — and step 3 then names WHICH option by that number. Web IDL §3.2.4.5 long's "
              "ConvertToInt(V, 32, \"signed\") is total over [-2**31, 2**31-1], so every position of the list "
              "and the past-the-end arm are all feasible and none may be chosen. Deciding it from the "
              "unknown's own example would collapse a modelable value to bare-concrete and delete every other "
              "arm. BUILD THE FORK: make this body an IdlStepBody (core/idl_args.h, IDL_STEP_FIRST) so it can "
              "park, then ask steps 2 and 3 through the elimination chain core/idl_index_arg.h holds, passing "
              "the list's length as `npositions` — which is the count of positions this algorithm admits, "
              "exactly as remove-a-CSS-rule passes its length. WHAT THAT CHAIN DOES NOT YET STATE is that its "
              "predicate names Web IDL §3.2.4.6 `unsigned long` while this argument is §3.2.4.5 `long`: the "
              "FACT a link establishes is `the number this unknown denotes is exactly k`, which is the same "
              "fact for either declared type, so the two members must share ONE constraint key and the key's "
              "text is what has to be settled before this member joins them");
        /* THE RELEASE ARM MUTATES NOTHING, AND THAT IS THE ALGORITHM'S OWN ARM RATHER THAN A QUIET RETURN
           INVENTED HERE. CLAUDE.md §AND-THE-ARM-BENEATH-A-`DFAIL` asks what state the arm leaves behind and
           names a successful return as the tell: here §2.6.4.3 has NO throwing arm at all, its steps 1 and 2
           both end in "then return", and the state that leaves is a tree the algorithm did not touch — which
           no sibling component takes a release arm of this algorithm to be handed. Throwing would be this
           engine inventing a failure the standard does not have, on a call a page makes with a value it
           computed. */
        return JS_UNDEFINED;
    }

    /* The number the converted `long` denotes. `idl_number_of` is what a body reads a converted numeric slot
       through — core/idl_args.h bans JS_ToFloat64 on a member's own argument — and its "no example" answer is
       the unknown's, which returned above.
       THE CALL IS NOT INSIDE THE `DCHECK`, AND THAT IS THE MACRO'S CONTRACT RATHER THAN STYLE: a DCHECK's
       condition is compiled out in release, so a call that WRITES `index` from inside one would leave every
       release build removing option 0 whatever the page asked for — a plausible datum produced by the check
       that was supposed to guard it. */
    have = idl_number_of(ctx, IDL_LONG, argv[0], &index);
    DCHECK(have == 1,
           "idl_number_of found no number for `remove`'s `index`, which is not unknown external input — it "
           "answers 0 only for an unknown carrying no example, and that arm returned above");
    remove_an_option(ctx, n, index);
    return JS_UNDEFINED;
}

void html_select_declare(JSContext *ctx)
{
    /* §4.10.7's `[CEReactions] undefined remove(long index)` — the LONGER of the section's two entries, and
       the only one with a type list, so the declaration's own list is that entry's. */
    static const IdlArgType REMOVE_ARGS[1] = { IDL_LONG };

    DCHECK(g_id_remove < 0, "html_select_declare ran twice in one agent");
    g_id_remove = idl_method_id(ctx, REMOVE_ARGS, 1, js_select_remove, 0);
    /* THE SHORTER ENTRY'S OWN OPTIONALITY, which is what §3.6 step 5 measures a zero-argument call against:
       `remove()` declares no position at all, so its "there are none" value is 0 and `select.remove()` is a
       legal call rather than a TypeError. This is also the number Web IDL §3.7.7 Operations' `length` is read
       off at argument count 0, and 0 is what `Element.prototype.remove.length` already answered. */
    idl_optional_from(0);
    /* §3.6's LENGTH-DIFFERING SPLIT, BY ARITY: the shorter entry's type list ends BEFORE position 0, which is
       -1. See core/idl_args.h for why that is a position and not a sentinel, and for the bound this member is
       the first to need widened. */
    idl_overload_length_split_at(-1);
    /* THE LONGER ENTRY'S OWN OPTIONALITY: `remove(long index)` declares ONE REQUIRED position, so its "there
       are none" value is 1 — which is what makes position 0 a CONVERTED argument at argument count 1 instead
       of §3.6 step 15.4.2's "missing". `select.remove(undefined)` is the call that reads the difference: the
       one-argument entry is the only one left after steps 3-4, its optionality at position 0 is "required",
       so step 15.5 converts and §3.2.4.5's ConvertToInt makes it 0 — a browser removes the FIRST option. Read
       off the shorter entry's list instead, the same call would have been an absent argument and would have
       run the ChildNode entry, which is the defect this file exists to end arriving one input later. */
    idl_overload_split_optional_from(1);
    agent_state_id("html_select", &g_id_remove, "§4.10.7's `remove` overload");
}

void html_select_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_remove >= 0, "§4.10.7's `remove` was installed before html_select_declare declared it");
    idl_install_method(ctx, proto, "remove", g_id_remove);
}
