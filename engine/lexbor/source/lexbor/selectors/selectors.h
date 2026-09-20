/*
 * Copyright (C) 2021-2025 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */


#ifndef LEXBOR_SELECTORS_H
#define LEXBOR_SELECTORS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/selectors/base.h"
#include "lexbor/dom/dom.h"
#include "lexbor/css/selectors/selectors.h"
#include "lexbor/core/array_obj.h"


typedef enum {
    LXB_SELECTORS_OPT_DEFAULT = 0x00,

    /*
     * Includes the passed (root) node in the search.
     *
     * By default, the root node does not participate in selector searches,
     * only its children.
     *
     * This behavior is logical, if you have found a node and then you want to
     * search for other nodes in it, you don't need to check it again.
     *
     * But there are cases when it is necessary for root node to participate
     * in the search.  That's what this option is for.
     */
    LXB_SELECTORS_OPT_MATCH_ROOT = 1 << 1,

    /*
     * Stop searching after the first match with any of the selectors
     * in the list.
     *
     * By default, the callback will be triggered for each selector list.
     * That is, if your node matches different selector lists, it will be
     * returned multiple times in the callback.
     *
     * For example:
     *    HTML: <div id="ok"><span>test</span></div>
     *    Selectors: div, div[id="ok"], div:has(:not(a))
     *
     * The default behavior will cause three callbacks with the same node (div).
     * Because it will be found by every selector in the list.
     *
     * This option allows you to end the element check after the first match on
     * any of the selectors.  That is, the callback will be called only once
     * for example above.  This way we get rid of duplicates in the search.
     */
    LXB_SELECTORS_OPT_MATCH_FIRST = 1 << 2
}
lxb_selectors_opt_t;

typedef struct lxb_selectors lxb_selectors_t;
typedef struct lxb_selectors_entry lxb_selectors_entry_t;
typedef struct lxb_selectors_nested lxb_selectors_nested_t;

typedef lxb_status_t
(*lxb_selectors_cb_f)(lxb_dom_node_t *node,
                      lxb_css_selector_specificity_t spec, void *ctx);

typedef lxb_selectors_entry_t *
(*lxb_selectors_state_cb_f)(lxb_selectors_t *selectors,
                            lxb_selectors_entry_t *entry);

/*
 * WHAT THE HOST HAS TO SAY ABOUT AN ATTRIBUTE'S VALUE, and there are THREE answers because there are three
 * states an embedder can be in. This used to be a bool, and the two answers it could carry -- "the value is
 * THESE bytes" and "I have nothing to add" -- are unchanged and keep their old meanings; what a bool could
 * not say is that the host KNOWS the tree's bytes do not state the value and cannot supply the ones that do.
 * A host with that to say had to pick one of the other two, and both are wrong in the same direction: every
 * operator below is then decided against a placeholder that no operand can equal, so BOTH arms of the test
 * answer false and nothing anywhere says a question was asked.
 */
typedef enum {
    /* "I have nothing to add" -- compare the bytes the tree holds. The old `false`, unchanged, and what
       every embedder that installs no table gets. */
    LXB_SELECTORS_VALUE_TREE         = 0,

    /* "the value is THESE bytes" -- `*out` is filled and is what the comparison reads. The old `true`. */
    LXB_SELECTORS_VALUE_HOST         = 1,

    /* "I cannot state this value" -- there is no byte string this comparison may be decided against, so the
       test has no two-valued answer and the match's answer is UNKNOWN. See lxb_selectors_nested_t::unknown
       for what the matcher then does with it. */
    LXB_SELECTORS_VALUE_UNDETERMINED = 2
}
lxb_selectors_value_t;

/*
 * THE HOST LANGUAGE'S SEAM. A pseudo-class whose answer is NOT a fact about the tree cannot be decided here:
 * Selectors Level 5 §7 "Exposing custom state: the :state() pseudo-class" says outright that "The exact
 * matching behavior of :state() pseudo-class is defined by the host language", and HTML §4.16.3
 * "Pseudo-classes" defines `:defined` by deferring to DOM §4.9 "Interface Element"'s custom element state --
 * which is state the embedder keeps, not a shape this tree has.
 *
 * So those arms ASK, exactly as lxb_html_tree_dom_cb_t is asked for the writes tree construction makes. An
 * embedder that compiles such a selector and installs no table is a NULL call and not a wrong answer, which is
 * the point: a default of `false` would report every element as un-defined and nothing would say so.
 */
typedef struct {
    /* DOM §4.9: "An element whose custom element state is "uncustomized" or "custom" is said to be defined." */
    bool (*defined)(const lxb_dom_node_t *node, void *ctx);

    /*
     * THE HOST IS ABOUT TO HAVE A MATCH DECIDED FROM THIS ATTRIBUTE'S VALUE BYTES, and this is its one chance
     * to say that those bytes do not STATE the value. Selectors §6 "Attribute selectors" is two-valued -- "an
     * attribute selector must be considered to match an element if that element has an attribute that matches
     * the attribute represented by the attribute selector" -- so a matcher has no third answer to give, and a
     * host whose attribute value is a stand-in for something it does not know would otherwise have BOTH arms
     * decided against it in silence. It is the same seam `defined` is, asked for a VALUE instead of for a
     * state: the byte comparison below cannot tell a real value from a placeholder, and the embedder can.
     *
     * It is CALLED ONLY WHERE THE ANSWER DEPENDS ON THE VALUE. §6.1 "Attribute presence and value selectors"
     * gives `[att]` as "Represents an element with the att attribute, whatever the value of the attribute" --
     * no value is read -- and its `~=` is decided by the operand alone where that operand is empty ("Also if
     * "val" is the empty string, it will never represent anything"), as are §6.2 "Substring matching
     * attribute selectors"' three ("If "val" is the empty string then the selector does not represent
     * anything"). None of those asks. §6.6 "Class selectors" and §6.7 "ID selectors" DO read a value and do
     * ask, because `.x` and `#x` are attribute value tests that happen to have their own syntax -- §6.6 says
     * so outright for the first ("it is equivalent to the ~= notation applied to the local class attribute").
     *
     * IT ANSWERS, AND THE ANSWER IS BYTES OR THE STATEMENT THAT THERE ARE NONE. This used to be VOID, on the
     * reasoning that "there is nothing for the matcher to do with a refusal" -- true of a host that can only
     * REFUSE, and that is not the only thing a host has to say. A host whose attribute value is a stand-in may
     * have SINCE ESTABLISHED what it stands for, and then the honest value of the attribute for this match is
     * those bytes and not what the tree holds; every operator below is then decided by the matcher's own
     * comparison, on a real string, exactly as it would have been had the bytes been there all along. And a
     * host that has established NOTHING has a third thing to say, which is lxb_selectors_value_t's whole
     * subject: see it for what each of the three answers means.
     *
     * `*out` IS BORROWED FOR THE DURATION OF THE MATCH and the matcher neither frees nor writes it, and it is
     * read ONLY for LXB_SELECTORS_VALUE_HOST. `attr` is the attribute the comparison is about and `node` its
     * element.
     */
    lxb_selectors_value_t (*attr_value_read)(const lxb_dom_node_t *node, const lxb_dom_attr_t *attr,
                                             lexbor_str_t *out, void *ctx);
}
lxb_selectors_host_cb_t;

struct lxb_selectors_entry {
    uintptr_t                     id;
    lxb_css_selector_combinator_t combinator;
    const lxb_css_selector_t      *selector;
    lxb_dom_node_t                *node;
    lxb_selectors_entry_t         *next;
    lxb_selectors_entry_t         *prev;
    lxb_selectors_entry_t         *following;
    lxb_selectors_nested_t        *nested;
};

struct lxb_selectors_nested {
    lxb_selectors_entry_t    *entry;
    lxb_selectors_state_cb_f return_state;

    lxb_selectors_cb_f       cb;
    void                     *ctx;

    lxb_dom_node_t           *root;
    lxb_selectors_nested_t   *parent;
    lxb_selectors_entry_t    *first;
    lxb_selectors_entry_t    *top;

    size_t                   index;

    bool                     forward;

    /*
     * AN UNDETERMINED VALUE WAS READ WHILE THIS SCOPE WAS BEING EVALUATED -- Kleene's third value, carried as
     * a sticky flag rather than as a third return type, because THE STATE MACHINE ALREADY COMPUTES AND AND OR
     * AS CONTROL FLOW and a third value every caller collapsed the same way would be a type nobody branches
     * on. A compound is an AND and stops at a definite false; a selector list, a `:is()` and a combinator's
     * candidate walk are ORs and stop at a definite true. Both already keep going exactly as three-valued
     * evaluation requires. What an undetermined read adds is this bit.
     *
     * AN UNDETERMINED READ BEHAVES AS NO-MATCH IN CONTROL FLOW AND SETS THIS. The flag is then read on ONE
     * path -- where the machine concludes NOT MATCHED -- and it is what makes that conclusion UNKNOWN instead
     * of FALSE. A definite TRUE never consults it, which is Kleene's OR: a match found along a path of
     * definite answers is a match whatever some other candidate could not answer.
     *
     * SO THE ANSWER IS NEVER WRONG AND IS SOMETIMES COARSE. It can never report TRUE where the truth is not
     * true: a true comes only from a path of definite answers reaching the callback. It can never report
     * FALSE where an undetermined value was touched: that is exactly what this bit prevents. It CAN report
     * UNKNOWN where a definite answer was available -- see the residual at lxb_selectors_host_attr_value --
     * and reporting unknown where the truth is decided keeps an arm that could have been dropped, which is
     * the safe direction and the only one a solver may take.
     *
     * `:not()` IS THE ONE PLACE A CARELESS RULE INVERTS IT, and it is one line: Kleene's NOT maps unknown to
     * unknown, so a `:not()` whose inner scope concluded NOT MATCHED **with this bit set** is UNKNOWN and must
     * not become true. lxb_selectors_state_after_not is where that is spelled. A `:not()` that took the bit
     * for a false would answer TRUE for a test it never decided -- the matcher PICKING an arm rather than
     * declining to answer, which is the whole thing this bit exists to make impossible.
     *
     * ONE BIT PER SCOPE, and nested scopes do not share it: an undetermined read inside a `:not()` belongs to
     * the `:not()`'s own answer and reaches the outer scope only through that answer. lxb_selectors_nested_make
     * clears it for every entry into a scope, because the nested record is REUSED across matches.
     */
    bool                     unknown;
};

struct lxb_selectors {
    lxb_selectors_state_cb_f state;
    lexbor_dobject_t         *objs;
    lexbor_dobject_t         *nested;

    lxb_selectors_nested_t   *current;

    /* NULL until lxb_selectors_host_cb_set -- lxb_selectors_create calloc()s, so an embedder that never sets
       one gets NULL rather than a stale table. */
    const lxb_selectors_host_cb_t *host;
    void                     *host_ctx;

    lxb_selectors_opt_t      options;
    lxb_status_t             status;

    /*
     * THE ANSWER THE CALLER CAME FOR THAT THE CALLBACK CANNOT CARRY. lxb_selectors_find and
     * lxb_selectors_match_node report a match by CALLING BACK, so "matched" has a channel and "did not match"
     * is its absence -- and a third answer has neither. This is that channel: true after the call means at
     * least one node's answer in it was UNDETERMINED rather than false, which for lxb_selectors_match_node's
     * one node is exactly that node's answer.
     *
     * IT IS SET AT ENTRY, NOT BY lxb_selectors_clean, because clean runs BEFORE the call returns and a field
     * the caller is about to read may not be cleared under it. Each of find/match_node clears it itself.
     *
     * AN EMBEDDER THAT DOES NOT READ IT GETS THE FALSE ARM OF EVERY UNDETERMINED TEST, silently. That is the
     * old behaviour and is why the seam's own host answers `false` rather than aborting: a matcher cannot make
     * its embedder ask. NAMED RESIDUAL -- WHAT IS NOT COVERED: an arena whose owner never reads this field
     * decides an undetermined test against the tree's placeholder bytes and says nothing. WHAT THE NEXT DIFF
     * BUILDS: the embedder-side read at every arena this engine creates, which is a fact about the embedder
     * and not about this file. HOW ITS ABSENCE WOULD SHOW: a selector answered false for an element whose
     * attribute the host had declined to state, in a walk whose owner never looked here.
     */
    bool                     unknown;
};


/*
 * Create lxb_selectors_t object.
 *
 * @return lxb_selectors_t * if successful, otherwise NULL.
 */
LXB_API lxb_selectors_t *
lxb_selectors_create(void);

/*
 * Initialization of lxb_selectors_t object.
 *
 * Caches are initialized in this function.
 *
 * @param[in] lxb_selectors_t *
 *
 * @return LXB_STATUS_OK if successful, otherwise an error status value.
 */
LXB_API lxb_status_t
lxb_selectors_init(lxb_selectors_t *selectors);

/*
 * Clears the object. Returns object to states as after initialization.
 *
 * After each call to lxb_selectors_find() and lxb_selectors_find_for_node(),
 * the lxb_selectors_t object is cleared. That is, you don't need to call this
 * function every time after searching by a selector.
 *
 * @param[in] lxb_url_parser_t *
 */
LXB_API void
lxb_selectors_clean(lxb_selectors_t *selectors);

/*
 * Destroy lxb_selectors_t object.
 *
 * Destroying all caches.
 *
 * @param[in] lxb_selectors_t *. Can be NULL.
 * @param[in] if false: only destroys internal caches.
 * if true: destroys the lxb_selectors_t object and all internal caches.
 *
 * @return lxb_selectors_t * if self_destroy = false, otherwise NULL.
 */
LXB_API lxb_selectors_t *
lxb_selectors_destroy(lxb_selectors_t *selectors, bool self_destroy);

/*
 * Search for nodes by selector list.
 *
 * Default Behavior:
 *    1. The root node does not participate in the search, only its child nodes.
 *    2. If a node matches multiple selector lists, a callback with that node
 *       will be called on each list.
 *       For example:
 *           HTML: <div id="ok"><span></span></div>
 *           Selectors: div, div[id="ok"], div:has(:not(a))
 *       For each selector list, a callback with a "div" node will be called.
 *
 * To change the search behavior, see lxb_selectors_opt_set().
 *
 * @param[in] lxb_selectors_t *.
 * @param[in] lxb_dom_node_t *.  The node from which the search will begin.
 * @param[in] const lxb_css_selector_list_t *.  Selectors List.
 * @param[in] lxb_selectors_cb_f.  Callback for a found node.
 * @param[in] void *.  Context for the callback.
 * if true: destroys the lxb_selectors_t object and all internal caches.
 *
 * @return LXB_STATUS_OK if successful, otherwise an error status value.
 */
LXB_API lxb_status_t
lxb_selectors_find(lxb_selectors_t *selectors, lxb_dom_node_t *root,
                   const lxb_css_selector_list_t *list,
                   lxb_selectors_cb_f cb, void *ctx);

/*
 * Match a node to a Selectors List.
 *
 * In other words, the function checks which selector lists will find the
 * specified node.
 *
 * Default Behavior:
 *    1. If a node matches multiple selector lists, a callback with that node
 *       will be called on each list.
 *       For example:
 *           HTML: <div id="ok"><span></span></div>
 *           Node: div
 *           Selectors: div, div[id="ok"], div:has(:not(a))
 *       For each selector list, a callback with a "div" node will be called.
 *
 * To change the search behavior, see lxb_selectors_opt_set().
 *
 * @param[in] lxb_selectors_t *.
 * @param[in] lxb_dom_node_t *.  The node from which the search will begin.
 * @param[in] const lxb_css_selector_list_t *.  Selectors List.
 * @param[in] lxb_selectors_cb_f.  Callback for a found node.
 * @param[in] void *.  Context for the callback.
 * if true: destroys the lxb_selectors_t object and all internal caches.
 *
 * @return LXB_STATUS_OK if successful, otherwise an error status value.
 */
LXB_API lxb_status_t
lxb_selectors_match_node(lxb_selectors_t *selectors, lxb_dom_node_t *node,
                         const lxb_css_selector_list_t *list,
                         lxb_selectors_cb_f cb, void *ctx);

/*
 * Deprecated!
 * This function does exactly the same thing as lxb_selectors_match_node().
 */
LXB_API LXB_DEPRECATED(lxb_status_t
lxb_selectors_find_reverse(lxb_selectors_t *selectors, lxb_dom_node_t *root,
                           const lxb_css_selector_list_t *list,
                           lxb_selectors_cb_f cb, void *ctx));

/*
 * Inline functions.
 */

/*
 * The function sets the node search options.
 *
 * For more information, see lxb_selectors_opt_t.
 *
 * @param[in] lxb_selectors_t *.
 * @param[in] lxb_selectors_opt_t.
 */
lxb_inline void
lxb_selectors_opt_set(lxb_selectors_t *selectors, lxb_selectors_opt_t opt)
{
    selectors->options = opt;
}

/*
 * Install the host language's answer table -- see lxb_selectors_host_cb_t.
 *
 * `cb` is BORROWED and must outlive `selectors`; a static table is the shape intended. `ctx` is handed back to
 * every callback unchanged.
 */
lxb_inline void
lxb_selectors_host_cb_set(lxb_selectors_t *selectors,
                          const lxb_selectors_host_cb_t *cb, void *ctx)
{
    selectors->host = cb;
    selectors->host_ctx = ctx;
}

/*
 * Get the current selector.
 *
 * Function to get the selector by which the node was found.
 * Use context (void *ctx) to pass the lxb_selectors_t object to the callback.
 *
 * @param[in] const lxb_selectors_t *.
 *
 * @return const lxb_css_selector_list_t *.
 */
lxb_inline const lxb_css_selector_list_t *
lxb_selectors_selector(const lxb_selectors_t *selectors)
{
    return selectors->current->entry->selector->list;
}

/*
 * Not inline for inline.
 */

/*
 * Same as lxb_selectors_opt_set() function, but not inline.
 */
LXB_API void
lxb_selectors_opt_set_noi(lxb_selectors_t *selectors, lxb_selectors_opt_t opt);

/*
 * Same as lxb_selectors_selector() function, but not inline.
 */
LXB_API const lxb_css_selector_list_t *
lxb_selectors_selector_noi(const lxb_selectors_t *selectors);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_SELECTORS_H */
