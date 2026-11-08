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
#include "core/idl_indexed.h"

/* §4.10.7's `remove`, which is ONE declaration carrying BOTH of the section's entries — see html_select.h. */
static int g_id_remove = -1;
/* The class every HTML `select` element's wrapper is built with (node_wrap): a Web IDL §3.9 legacy platform
   object, because HTML §4.10.7 The select element declares an indexed property getter. */
static JSClassID g_select_class;

JSClassID html_select_class(void)
{
    return g_select_class;
}

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

/* ---- the indexed property getter and `length` ------------------------------------------------------------ */

/* HTML §4.10.7 The select element: "The supported property indices at any instant are the indices supported by the
   object returned by the options attribute at that instant." That collection's filter is the list of options, so they
   are the list's indices, and "The length IDL attribute must return the number of nodes represented by the options
   collection" is the list's size. */
static uint32_t si_length(JSContext *ctx, JSValueConst self)
{
    lxb_dom_node_t *n = node_of(self);
    JSValue list;
    uint32_t len;

    DCHECK(select_element_is(n), "the select indexed getter ran on a wrapper that is not a <select>'s — the hooks "
                                 "are reached only through the select class, which node_wrap gives selects");
    list = html_form_select_option_list(ctx, n);
    len = option_list_len(ctx, list);
    JS_FreeValue(ctx, list);
    return len;
}

/* The IDL's `getter HTMLOptionElement? item(unsigned long index)`: the indexth option in the list. */
static JSValue si_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue list = html_form_select_option_list(ctx, node_of(self)), v = JS_GetPropertyUint32(ctx, list, i);

    JS_FreeValue(ctx, list);
    return v;
}

static const IdlIndexedDecl SELECT_INDEXED = { "HTMLSelectElement", si_length, si_item, NULL, 0 };

static int si_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    return idl_indexed_own_property(ctx, desc, obj, prop, &SELECT_INDEXED);
}

static int si_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    return idl_indexed_own_property_names(ctx, ptab, plen, obj, &SELECT_INDEXED);
}

static int si_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val, JSValueConst getter,
                         JSValueConst setter, int flags)
{
    return idl_indexed_define_own_property(ctx, obj, prop, val, getter, setter, flags, &SELECT_INDEXED);
}

static int si_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    return idl_indexed_delete_property(ctx, obj, prop, &SELECT_INDEXED);
}

/* Web IDL §3.9.5 [[PreventExtensions]]: "Return false." */
static int si_prevent_extensions(JSContext *ctx, JSValueConst obj)
{
    (void)ctx; (void)obj;
    return 0;
}

/* The indexed setter `setter undefined (unsigned long index, HTMLOptionElement? option)` is core/idl_indexed.c's
   has_indexed_setter residual: an indexed write is refused. */
static const JSClassExoticMethods SELECT_EXOTIC = {
    .get_own_property = si_get_own,
    .get_own_property_names = si_own_names,
    .define_own_property = si_define_own,
    .delete_property = si_delete,
    .prevent_extensions = si_prevent_extensions,
    .get_own_property_no_user_code = true,   /* a walk of the select's descendants: no page code */
};

/* Named residual. Not covered: the `length` setter, which HTML §4.10.7 The select element makes act like HTML
   §2.6.4.3 The HTMLOptionsCollection interface's (append new option elements given n, or remove the last n
   options). Next diff builds those steps as a setter over the list of options. Absence shows as
   `select.length = 0` leaving every option in place. */
static JSValue js_select_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = select_receiver(ctx, this_val, "length");

    (void)magic;
    if (!n) return JS_EXCEPTION;
    return JS_NewUint32(ctx, si_length(ctx, this_val));
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
/* Steps 1-3: the option step 4 removes, or NULL where step 1 or 2 returns. */
static lxb_dom_node_t *remove_an_option(JSContext *ctx, lxb_dom_node_t *select, double index)
{
    JSValue list = html_form_select_option_list(ctx, select);
    uint32_t len = option_list_len(ctx, list);
    JSValue element;
    lxb_dom_node_t *n;

    /* STEPS 1 AND 2. The comparison is over the DOUBLE the conversion produced and not over a cast of it: Web
       IDL §3.2.4.5 long is ConvertToInt(V, 32, "signed"), so the value is any integer in [-2**31, 2**31-1] and
       a negative one is an ordinary call rather than an error — `select.remove(-1)` is step 2's return arm. */
    if (len == 0 || !(index >= 0) || !(index < (double)len)) { JS_FreeValue(ctx, list); return NULL; }

    element = JS_GetPropertyUint32(ctx, list, (uint32_t)index);      /* STEP 3 */
    n = node_of(element);
    /* STEP 4 is the caller's, through the remove machine.
       THE PARENT IS THIS CODEBASE'S OWN INVARIANT AND NOT PAGE INPUT, which is what makes an assert the right
       thing here and the wrong thing at the argument above: §4.10.7's walk descends from the select, so every
       node it returns is a DESCENDANT of it and has a parent by construction. */
    DCHECK(n && n->parent,
           "§4.10.7's list of options held an entry that is not a node with a parent — the walk descends from "
           "the select itself, so every option it returns is a descendant of it and §2.6.4.3's remove an "
           "option step 4 has a parent node to remove from");
    JS_FreeValue(ctx, element);
    JS_FreeValue(ctx, list);
    return n;
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
/* The option remove(index) removes, or NULL. */
static lxb_dom_node_t *select_remove_index(JSContext *ctx, lxb_dom_node_t *n, JSValueConst *argv)
{
    double index = 0;
    int have;

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
        return NULL;
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
    return remove_an_option(ctx, n, index);
}

/* The member: argument count 0 is the ChildNode entry, as a step over core/dom/node.h's one start of it. */
#define SELECT_REMOVE_STAGES(X) \
    X(SR_ENTRY, "HTML §4.10.7 select remove() (Web IDL §3.6's entry, by argument count)") \
    NODE_REMOVE_ALGO_STAGES(X, SR, "HTML §4.10.7 select remove() → DOM §4.2.8 remove() step 2, or HTML §2.6.4.3 " \
                                   "remove an option step 4") \
    X(SR_DONE,  "HTML §4.10.7 select remove() (done)")
enum { IDL_STEP_STAGE_BASE(SELECT_REMOVE_STAGES) SELECT_REMOVE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const SELECT_REMOVE_STEPS[] = { SELECT_REMOVE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static void select_remove_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    (void)ctx; (void)st; (void)v;   /* lexbor nodes, the document's; the header holds this */
}

static int js_select_remove(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    NodeRemoveState *s = st;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    STEP_DISPATCH(SELECT_REMOVE_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(SR_ENTRY);
    {
        lxb_dom_node_t *n = select_receiver(ctx, hdr->this_val, "remove");

        if (!n) return JS_STEP_ABRUPT;
        if (argc == 0) {
            node_child_node_remove_start(hdr, s, n, SR_RANGES, SR_DONE);
            return JS_STEP_YIELD;
        }
        n = select_remove_index(ctx, n, argv);
        if (!n) return JS_STEP_DONE;
        node_remove_start(hdr, s, n, /*suppress_observers*/ false, SR_RANGES, SR_DONE);   /* STEP 4 */
        return JS_STEP_YIELD;
    }

    NODE_REMOVE_ALGO_STAGES(JS_STEP_STAGE_ARM, SR, "")
    return node_remove_run(ctx, hdr, s, SR_RANGES);

    STEP_ARM(SR_DONE);
    return JS_STEP_DONE;
}

static const IdlStepDecl SELECT_REMOVE_STEP = {
    js_select_remove, sizeof(NodeRemoveState), select_remove_visit, NULL,
    "HTML §4.10.7 HTMLSelectElement.remove", SELECT_REMOVE_STEPS
};

void html_select_declare(JSContext *ctx)
{
    /* §4.10.7's `[CEReactions] undefined remove(long index)` — the LONGER of the section's two entries, and
       the only one with a type list, so the declaration's own list is that entry's. */
    static const IdlArgType REMOVE_ARGS[1] = { IDL_LONG };

    DCHECK(g_id_remove < 0, "html_select_declare ran twice in one agent");
    g_id_remove = idl_method_id_step(ctx, REMOVE_ARGS, 1, NULL, 0, &SELECT_REMOVE_STEP, 0);
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
    /* `element`, THE ROW THAT RELEASES THIS, and not this file — html_select_free is reached from
       html_element_free and thence from element_free, which is the `element` row's release column, and this
       component has no row of its own. core/html/html_canvas_element.c declares under the same name and is
       given back by html_element_free like this one; core/html/close_watcher.c does too, one level
       deeper, through html_form_free.
       WHAT STOOD HERE WAS THIS FILE'S OWN NAME, and core/platform.c's list holds no row of it — so the
       pairing that asks "does anybody RELEASE this?" was never run over this slot at all, and `element`,
       which really owns it, reported "declared no agent state" in character-for-character the words a
       component that declared nothing produces. core/platform.c's walk over declarations that name no row
       caught it on the first dev build after this file landed. */
    agent_state_id("element", &g_id_remove, "§4.10.7's `remove` overload");
    {
        JSClassDef d = { "HTMLSelectElement", .exotic = &SELECT_EXOTIC };

        JS_NewClassID(JS_GetRuntime(ctx), &g_select_class);
        CHECK(JS_NewClass(JS_GetRuntime(ctx), g_select_class, &d) == 0, "the select wrapper class could not be "
                                                                         "registered");
        agent_state_class("element", &g_select_class,
                          "HTML §4.10.7 The select element's wrapper class, a legacy platform object's");
        idl_iface_implements(g_select_class, node_class_id());   /* a select wrapper is a Node wrapper */
    }
}

void html_select_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_remove >= 0, "§4.10.7's `remove` was installed before html_select_declare declared it");
    idl_install_method(ctx, proto, "remove", g_id_remove);
    idl_install_accessor(ctx, proto, "length", js_select_length, 0, -1);
}

void html_select_free(void)
{
    /* THE ID IS NOT PUT BACK HERE, and that is the contract rather than an omission. It is declared under
       `element`, whose release ends in agent_state_undo — ONE reset, computed from the registry that already
       holds this slot's address and its kind. A line here as well would be a SECOND resetter beside that
       one, which is the pair core/agent_state.h's undo exists to stop being kept by hand.
       SO THIS FUNCTION GIVES BACK NO REFERENCE AND TAKES NO RUNTIME: what §4.10.7 holds for the agent is one
       member id, and an id is not a reference. The claim below is the whole of what this release is.
       AND THE CASCADE REACHED THIS FILE — the claim that entitles element_free's last line to put this
       file's slot back, and which that line REFUSES to proceed without: a row is declared from several
       files, the undo resets every slot carrying the row's name, and a member the cascade DROPPED would
       otherwise have its handle put back by a release that never ran. See core/agent_state.h's
       agent_state_reached. */
    agent_state_reached("element");
}
