/* SELECTORS §3/§5 — the agent's selector matcher. See selector_match.h for why the two lexbor objects below
 * live here and not in the machines that use them. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/dom/selector_match.h"
#include "core/html/custom_elements.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"

/* THE HOST LANGUAGE'S ANSWERS, for the pseudo-classes whose truth is not a shape of the tree. Selectors Level
   5 §7 "Exposing custom state: the :state() pseudo-class" states the rule in general — "The exact matching
   behavior of :state() pseudo-class is defined by the host language" — and HTML §4.16.3 "Pseudo-classes"
   defines `:defined` the same way, by deferring to DOM §4.9 "Interface Element"'s custom element state. The
   table is STATIC because lxb_selectors_host_cb_set borrows it for the arena's whole life, and it is installed
   at init rather than per match so there is no window in which a compiled `:defined` could be matched against
   an arena that has nobody to ask. */
static bool host_defined(const lxb_dom_node_t *node, void *ctx)
{
    (void)ctx;   /* the realm is the ELEMENT's own document's, derived per node — never the running flow's */
    return custom_elements_is_defined(node);
}

/*
 * THE HOST'S ANSWER FOR AN ATTRIBUTE WHOSE VALUE IS NOT BYTES — SELECTORS §6 "Attribute selectors" read
 * against §Solver-half's unknown.
 *
 * WHAT THE MATCHER IS ABOUT TO DO. DOM §4.9's write stores a concolic value's SHAPE in the tree, because a
 * concolic has no bytes and `core/dom/element.c`'s write says so at its own site ("A concolic value has no
 * bytes to store ... The SHAPE is the honest byte form for the tree") and files the value itself in the
 * per-flow taint shadow. A shape is a DISPLAY FORM and not the value: nothing the page could write makes it
 * equal to `dark`, so every §6.1 `=` test against it answers false, and so does every test against every
 * other operand. Both arms of a two-armed question are therefore decided, and §Solver-half permits pruning
 * only a CONTRADICTED branch — "a contradicted branch is pruned (sound-only — uncertainty keeps the arm)".
 * Neither arm is contradicted here. The page renders with neither rule and NOTHING SAYS SO.
 *
 * WHY THIS IS A CRASH AND NOT A REFUSAL. §Offensive-programming's category (2): a capability that should
 * exist and does not. It is NOT the page-held abort switch §WHOSE-BYTES-STATE-THE-VALUE forbids — the value
 * asserted on is one THIS ENGINE MINTED to stand for something it does not know, not bytes a stranger
 * stated, and no string a page can write reaches this. What it asserts is this engine's own capability,
 * which is what a `DFAIL` is for.
 *
 * WHAT THE NEXT DIFFS BUILD, IN ORDER, and why none of them is this one:
 *   (0) CONCRETIZE-ON-PIN AT THE MATCH, WHICH NEEDS NO FORK AND WAS LANDED BELOW (1) BY MISTAKE — THE
 *       CORRECTION IS RECORDED HERE RATHER THAN BY REORDERING THE LIST IN SILENCE, because the reasoning
 *       that put the pin last is the reasoning the next reader will re-derive. It went: the unknown reaches
 *       the attribute with its example already dropped (the forced sibling of `if (stored)`), so nothing can
 *       decide the match and a FORK is the only way out. That is true of the VALUE and false of the FLOW.
 *       §Solver-half's CONCRETIZE-ON-PIN says "once `x==='admin'` pins the value, a later READ of that source
 *       returns the pinned bytes, so a later branch on it is decided by RUNNING the real predicate on a real
 *       string and does not fork at all" — so a flow whose own `===` already pinned the value HAS the answer
 *       and the match need only ASK. That is what core/css/media_query.c's `media_query_matches_now` does
 *       for a CSS read one component over, in its own words "C cannot fork, so it takes the arm this flow
 *       already committed to", and it is the shape to copy.
 *       WHAT MADE THE MISTAKE INVISIBLE IS THAT THE PIN IS WRITTEN BY THE PAGE AND NOT BY THIS ENGINE, so it
 *       is in neither this file nor the sheet: the bundle that writes an unknown into an attribute is very
 *       often the same bundle that BRANCHES on it a few statements later, and the branch seam has a resume
 *       point where the match does not. Measured on the document this seam was built from: its theme script
 *       sets the attribute and then tests the SAME unknown with `=== 'dark'` and `=== 'light'` three lines
 *       down, so both worlds are already in the frontier, minted by the interpreter, each with the value
 *       pinned — and the cascade answers false for both.
 *       WHAT THE DIFF IS: concolic.c holds the read-back already, as the STATIC `pin_of`, keyed by the
 *       value's own `src`; the attribute shadow hands this seam that same value, so what is missing is an
 *       EXPORT of that read and a `=`/`|=` arm here that runs the real byte comparison against the pinned
 *       bytes. The rest of §6's operators follow for free, because a pinned value is a real string and the
 *       matcher's own comparison is then the right one.
 *       HOW ITS ABSENCE WOULD SHOW, which is what this abort is: a flow that has PROVED what an attribute
 *       holds still refusing to answer a selector over it.
 *   (1) THREE-VALUED MATCHING, for the flows (0) cannot answer — the ones that reached the cascade having
 *       committed to NEITHER arm, where there is no pin to read and a real question has to be asked.
 *       `lxb_selectors_match_*` must be able to answer UNKNOWN, and `:not()`, `:is()`
 *       and the combinators must propagate it. The RULE is Kleene's and this engine already implements it,
 *       in core/css/media_query.c's `mq_not`/`mq_and`/`mq_or` over an `MQ_UNKNOWN` — so what is missing is
 *       the propagation and not the arithmetic. ITS CAUSE THERE IS NOT THE CAUSE HERE and the analogy stops
 *       at the table: Media Queries Level 4 §3.1 "Evaluating Media Queries" makes a query UNKNOWN when the
 *       UA does not UNDERSTAND it (`<general-enclosed>`), where a selector here is perfectly understood and
 *       it is the ATTRIBUTE'S VALUE that is unknown. Nothing downstream can know a question was asked until
 *       this lands, which is why it is first.
 *   (2) A CASCADE THAT REPORTS ITS UNANSWERED PREDICATE, rather than an answer it does not have.
 *   (3) A CONSUMER THAT CAN ASK. A fork needs a RESUME POINT, and there is none inside a match: the arena
 *       note below says lxb_selectors_match_node "is a single C call that returns before the machine driving
 *       it can yield", and solver/engine.c's `engine_prepare_fork` aborts by name for a C body that forks
 *       from inside its own activation, naming the remedy — JS_CFUNC_STEP_DEF, with the ask in the machine's
 *       own `step_fork_run`. So the ask belongs at whichever step machine reached the cascade, which re-runs
 *       it once per answered predicate exactly as quickjs.c's `step_ownkeys_chain` re-runs its enumeration.
 *       WHICH MACHINES THOSE ARE IS THE PART TO DERIVE RATHER THAN ASSUME: `cssom_cascaded_value`'s callers
 *       are what reach it, and the ones a page drives (`getComputedStyle`, the §7 view members) are plain C
 *       bodies today, so each is a declaration to build and not a call to add.
 *   (4) ONE KEY PER PREDICATE, NOT PER RULE. `html[data-theme=dark]` and `html:not([data-theme=dark])` are
 *       ONE question asked twice, and a key composed from the rule rather than from (the value's identity,
 *       the operator, the operand) forks a world for each — §Solver-half's "keyed by the PREDICATE's own
 *       identity — operator and both operands".
 *   (5) THE PIN. §concretize-on-pin: `[att=dark]` answered TRUE pins the value, after which `[att=light]` is
 *       DECIDED by the flow's own constraint rather than forked. Without it the worlds multiply with the
 *       number of operands the sheet tests, where the page can only be in one.
 * HOW ITS ABSENCE WOULD SHOW once (1)-(5) exist: a document whose only style difference between two flows is
 * an attribute this engine never observed would report one computed value where a browser has two.
 *
 * WHAT IT DOES NOT COVER, AND THE NEXT DIFF FOR IT: §6.1's `~=` with WHITESPACE in its operand ("If "val"
 * contains whitespace, it will never represent anything") is decided false by the operand alone, exactly as
 * the empty-operand arms the matcher already declines to ask about are — so this refuses a match that is
 * false under every value the attribute could hold. The next diff tests the operand for whitespace beside
 * the length test in `lxb_selectors_match_attribute`. It would show as this abort naming an attribute whose
 * only test in the sheet is a `~=` whose operand has a space in it. */
static void host_attr_value_read(const lxb_dom_node_t *node, const lxb_dom_attr_t *attr, void *ctx)
{
#if !APICLIENT_DEV
    /* THE RELEASE ARM, STATED RATHER THAN LEFT TO THE MACRO. §Offensive-programming's release exemption makes
       the abort below dev-only, and what remains for release is the answer the matcher already gives: the
       shape bytes are compared and the test answers false. That is a DEFINED wrong answer with no sibling to
       compose badly with — the cascade receives a value like any other — and it is byte-for-byte the
       behaviour every build had before this seam existed. The WORK is compiled out with the crash because
       its only consumer is the crash: resolving §4.9's key out of the attribute allocates, and doing it per
       attribute test per rule per element for a check that cannot fire is a cost paid for nothing. */
    (void)node; (void)attr; (void)ctx;
#else
    lxb_dom_element_t *el;
    JSValue taint;
    const lxb_char_t *tag, *name;
    size_t tag_n = 0, name_n = 0;

    (void)ctx;
    /* THE O(1) PRECONDITION FIRST. This runs per attribute test per rule per element, and resolving §4.9's
       key out of the attribute allocates; a document that never put an unknown in an attribute — which is
       most of them — pays one load for its whole cascade. */
    if (attr_shadow_count() == 0) return;
    DCHECK(node != NULL && attr != NULL,
           "SELECTORS §6's value seam was asked about no element or no attribute — the matcher holds both at "
           "the comparison, so half a key here is a caller that composed the ask somewhere else");
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    el = lxb_dom_interface_element((lxb_dom_node_t *)node);
    taint = dom_cow_attr_taint_node(el, attr);   /* BORROWED */
    if (!concolic_is(taint)) return;
    tag = lxb_dom_element_local_name(el, &tag_n);
    name = lxb_dom_attr_qualified_name((lxb_dom_attr_t *)attr, &name_n);
    DFAILF("<%.*s %.*s> — SELECTORS §6 \"Attribute selectors\" is being decided from an attribute whose "
           "value this engine does not know (`%s`), so BOTH arms of the test are about to be answered "
           "false against a DISPLAY SHAPE that no operand can equal. §6's own rule is two-valued — \"an "
           "attribute selector must be considered to match an element if that element has an attribute that "
           "matches the attribute represented by the attribute selector\" — so the matcher has no third "
           "answer. ASK FIRST WHETHER THIS FLOW ALREADY KNOWS: a page that writes an unknown into an "
           "attribute very often branches on the SAME unknown a few statements later, and §Solver-half's "
           "concretize-on-pin makes that flow's answer a real string. See host_attr_value_read in this "
           "file for the ordered diffs, of which the first is that read and needs no fork at all.",
           (int)tag_n, tag ? (const char *)tag : "?",
           (int)name_n, name ? (const char *)name : "?",
           concolic_shape_c(taint) ? concolic_shape_c(taint) : "{}");
#endif
}

static const lxb_selectors_host_cb_t HOST_CB = { host_defined, host_attr_value_read };

const lxb_selectors_host_cb_t *selector_match_host_cb(void) { return &HOST_CB; }

/* THE ONE ARENA, AND WHY ONE IS ENOUGH: there is NO REST POINT INSIDE A MATCH. lxb_selectors_match_node is a
   single C call that returns before the machine driving it can yield, and it ends in lxb_selectors_clean — so
   the arena is empty at every point a flow can be parked at, and no two flows can ever be inside one. That is
   a statement about where this engine can suspend and not about what the page is running, which is the
   difference between a shared scratch allocator and a shared piece of state. `g_in_match` asserts it at the
   one call, because the day it stops holding is the day two interleaved matches share one entry pool. */
static lxb_selectors_t *g_arena;
static bool g_in_match;

void selector_match_init(void)
{
    DCHECK(g_arena == NULL, "selector_match_init ran twice — one agent has one selector-matching arena");
    g_arena = lxb_selectors_create();
    CHECK(g_arena != NULL && lxb_selectors_init(g_arena) == LXB_STATUS_OK,
          "the CSS selector matcher could not be created");
    lxb_selectors_host_cb_set(g_arena, &HOST_CB, NULL);
}

void selector_match_free(void)
{
    DCHECK(!g_in_match, "the agent is being torn down from inside a selector match — the arena being destroyed "
                        "is the one the match is standing in");
    if (g_arena) { lxb_selectors_destroy(g_arena, true); g_arena = NULL; }
}

SelectorList *selector_list_compile(const char *sel)
{
    lxb_css_parser_t *parser;
    lxb_css_selector_list_t *list;
    SelectorList *sl;

    DCHECK(sel != NULL, "a selector was compiled from no string — the caller reads its argument first, and an "
                        "unknown one denotes its SHAPE rather than nothing");
    DCHECK(g_arena != NULL, "a selector was compiled before selector_match_init ran");
    /* THE PARSER LIVES AND DIES INSIDE THIS CALL, and that is the whole reason this function exists. A machine
       that kept one across its suspensions would be holding a tokenizer standing at a position — the one
       lexbor object a forked arm cannot be given half of — for the entire walk that follows, when the compile
       it was created for finished in the first step. */
    parser = lxb_css_parser_create();
    CHECK(parser != NULL && lxb_css_parser_init(parser, NULL) == LXB_STATUS_OK,
          "the CSS selector parser could not be initialised");
    list = lxb_css_selectors_parse(parser, (const lxb_char_t *)sel, strlen(sel));
    /* THE COMPILED LIST OUTLIVES ITS PARSER. lxb_css_selectors_parse_list allocates the lxb_css_memory_t the
       list is built in, leaves it on the parser and hands the list a pointer to it;
       lxb_css_parser_destroy frees the tokenizer and the parser's own scratch and does NOT touch that memory,
       which is what selector_list_destroy then owns. On failure the same function has already destroyed it. */
    lxb_css_parser_destroy(parser, true);
    if (!list) return NULL;   /* SELECTORS §5's `failure` */
    sl = malloc(sizeof(*sl));
    CHECK(sl != NULL, "a compiled selector list could not be recorded");
    sl->refs = 1;
    sl->list = list;
    return sl;
}

void selector_list_destroy(JSContext *ctx, void *p)
{
    SelectorList *sl = p;
    (void)ctx;
    DCHECK(sl != NULL, "a compiled selector list's destroy ran on nothing");
    DCHECK(sl->refs == 0, "a compiled selector list was destroyed with arms still naming it — the shared visit "
                          "drops one reference and destroys at zero, so a non-zero count here is a machine "
                          "that freed the record by hand beside that declaration");
    lxb_css_selector_list_destroy_memory(sl->list);
    free(sl);
}

typedef struct { bool matched; lxb_css_selector_specificity_t spec; } SelHit;

static lxb_status_t sel_hit_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *vctx)
{
    SelHit *h = vctx;
    (void)node;
    /* A selector LIST matches through whichever of its selectors matched, and §6.4's cascade uses the highest —
       `#id, div { … }` on a div with that id contributes the id's weight, not the tag's. */
    if (!h->matched || spec > h->spec) h->spec = spec;
    h->matched = true;
    return LXB_STATUS_OK;
}

bool selector_match_node(lxb_dom_node_t *node, const lxb_css_selector_list_t *list,
                         lxb_css_selector_specificity_t *out_spec)
{
    SelHit h = { false, 0 };

    DCHECK(g_arena != NULL, "a selector was matched before selector_match_init ran");
    DCHECK(g_arena->host == &HOST_CB,
           "the agent's selector-matching arena has no host-language answer table — a `:defined` in the "
           "compiled list reaches lxb_selectors_pseudo_class's host arm and calls through a NULL. "
           "selector_match_init installs it; an arena that reached a match without one was built somewhere "
           "else or had the table cleared under it");
    DCHECK(node != NULL && list != NULL, "a selector match was asked about no node or no compiled selector");
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    DCHECK(!g_in_match, "the agent's one selector-matching arena was re-entered — one arena is correct only "
                        "because a match has no rest point inside it and cleans its pools before it returns, "
                        "and a match inside a match would share those pools");
    g_in_match = true;
    /* lxb_selectors_match_node, not a subtree find: a combinator is resolved by walking UP from the candidate
       through the whole document, so §1.3's scoped matching still holds when the caller filters the results to
       a subtree — `el.querySelectorAll('div p')` finds a <p> under `el` whose <div> ancestor is OUTSIDE it. */
    lxb_selectors_match_node(g_arena, node, list, sel_hit_cb, &h);
    g_in_match = false;
    if (out_spec) *out_spec = h.spec;
    return h.matched;
}
