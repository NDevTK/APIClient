/* SELECTORS §3/§5 — the agent's selector matcher. See selector_match.h for why the two lexbor objects below
 * live here and not in the machines that use them. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/dom/selector_match.h"

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
