/* SELECTORS §3 — "match a selector against an element", and the compiled selector list it is matched from.
 *
 * ONE PLACE LEXBOR'S SELECTOR ENGINE IS ASKED, so the four §1.3/§4.9 members and §6.4's cascade cannot disagree
 * about what a selector means. It exists as its own component for a second reason, which is the one that made
 * it: a selector walk is a STEP MACHINE, and a machine's state is what it holds across a rest point. Neither
 * lexbor object here is that.
 *
 *   - The CSS PARSER is a tokenizer standing at a position, and that is the one lexbor object with no halves to
 *     give a forked arm. It never reaches a rest point because selector_list_compile creates it, uses it and
 *     destroys it inside one uninterruptible call.
 *   - The MATCHING ARENA (lxb_selectors_t) is not state at all: lxb_selectors_match_node ends in
 *     lxb_selectors_clean, which releases the entry and nested pools the match allocated, so nothing about one
 *     match survives into the next. It is the AGENT's, held here, and no machine holds one.
 *
 * What survives a compile is the COMPILED LIST, which nothing writes after it is built — so two forked arms
 * SHARE it by reference (JSStepVisit::shared) rather than each holding a parser to rebuild it from. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_SELECTOR_MATCH_H
#define ENGINE_HOST_BROWSER_CORE_DOM_SELECTOR_MATCH_H
#include <stdbool.h>

#include <lexbor/css/css.h>
#include <lexbor/dom/dom.h>
#include <lexbor/selectors/selectors.h>

#include "quickjs.h"

/* A COMPILED SELECTOR LIST, REFCOUNTED — read-only once built, which is what makes sharing it correct rather
   than a shortcut: the interior pointers a match takes into it stay valid in every arm holding one. */
typedef struct SelectorList {
    int refs;
    lxb_css_selector_list_t *list;
} SelectorList;

/* THE AGENT'S MATCHING ARENA. Eager, beside the other agent-scope DOM inits, because a lazily built one is a
   scratch allocator created inside whichever flow happened to run the first query. */
void selector_match_init(void);
void selector_match_free(void);

/* SELECTORS §5 "parse a selector". NULL is the spec's `failure`, which every caller turns into a SyntaxError.
   The returned record is owned by the caller at one reference. */
SelectorList *selector_list_compile(const char *sel);
/* JSStepVisit::shared's destroy — the signature is that hook's, which is why it takes a JSContext it has no
   use for and a void *. */
void selector_list_destroy(JSContext *ctx, void *p);

/* SELECTORS §3 — does `node` match `list`? `out_spec` receives the HIGHEST specificity that matched (a list
   matches through whichever of its selectors did, and §6.4's cascade weighs that one), or is NULL for a caller
   that only asks the question. A non-element never matches. */
bool selector_match_node(lxb_dom_node_t *node, const lxb_css_selector_list_t *list,
                         lxb_css_selector_specificity_t *out_spec);

#endif
