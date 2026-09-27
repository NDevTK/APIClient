/* Selectors 4 §17.3 "Match a Selector Against an Element", and the compiled selector list it is matched from.
 *
 * ONE PLACE LEXBOR'S SELECTOR ENGINE IS ASKED, so DOM's four §1.3/§4.9 members and CSS 2.1 §6.4 "The cascade"
 * cannot disagree
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

/* THE HOST LANGUAGE'S ANSWER TABLE — the pseudo-classes lexbor cannot decide from the tree (`:defined` is HTML
   §4.16.3 "Pseudo-classes" deferring to DOM §4.9 "Interface Element"'s custom element state). It is exposed
   because a second `lxb_selectors_t` anywhere in this engine must answer them IDENTICALLY: this file's own
   contract above is that there is ONE meaning for a selector, and an arena built without this table does not
   give a different answer, it calls through a NULL. Borrowed, static, valid for the process. */
const lxb_selectors_host_cb_t *selector_match_host_cb(void);

/* Selectors 4 §17.1 "Parse A Selector". NULL is the spec's `failure`, which every caller turns into a
   SyntaxError. The returned record is owned by the caller at one reference. */
SelectorList *selector_list_compile(const char *sel);
/* JSStepVisit::shared's destroy — the signature is that hook's, which is why it takes a JSContext it has no
   use for and a void *. */
void selector_list_destroy(JSContext *ctx, void *p);

/* THE THIRD ANSWER, AND THE QUESTION IT IS ABOUT — Kleene's UNKNOWN carried out of a match together with the
 * §6 PREDICATE that caused it, because a caller that can KEEP the arm has to name what it is keeping.
 *
 * `undetermined` IS THE ANSWER AND EVERYTHING ELSE IS THE QUESTION. Selectors 4 §6 is two-valued, so a test
 * decided from an attribute whose value the host declines to state has no answer a `bool` can hold; the
 * matcher already carries that outward (`lxb_selectors_t::unknown`) and this is where a caller reads it. When
 * it is false every other field is zero and means nothing.
 *
 * `over` IS THE UNKNOWN VALUE THE TEST WAS DECIDED FROM and is `step_fork_run`'s `over`. It is BORROWED out of
 * the per-flow taint shadow and is valid until the NEXT match, so a caller that keeps it across a rest point
 * DUPS it — which is exactly what a caller handing this record back as `decided` must do, since the snapshot
 * has to carry the value the arm is about.
 *
 * THE PREDICATE IS THE OTHER HALF OF THE CONSTRAINT KEY AND NOT A DIAGNOSTIC. §Solver-half keys a fork on "the
 * PREDICATE's own identity — operator and both operands", so `[att=dark]` and `[att=light]` over ONE
 * undetermined value are TWO questions: a key composed from anything coarser answers one arm for both and
 * leaves the flow standing on two constraints saying the attribute is two different strings. `attr`, `op`,
 * `operand` and `insensitive` are that key's fields and `selector_undet_key` is its one speller.
 *
 * `forkable` IS THE RECORD'S OWN STATEMENT THAT THE KEY IT CARRIES IS EXACT, THE VALUE IS SPELLABLE AND THE ARM
 * IS REALISABLE, and it is one bit rather than a caller's judgement because all three of its reasons are facts
 * this file holds and the caller does not — and because a caller can act on none of them alone. (a) `attr` and
 * `operand` are FIXED-WIDTH copies, so a truncated one would spell two different operands the same way and
 * merge their questions — the very merge the key exists to prevent, arriving through the buffer instead of
 * through the key. (b) The VALUE must have an IDENTITY (`concolic_ident_c`), which is the other half of the
 * constraint key and is also what the supply below recognises the decided predicate's value BY. Without one
 * there is no key to file the arm under and no way to grant the arm when it comes back, so the match would
 * decline on the same predicate for ever — a fork per re-match, which is the one shape §NO BOUNDS's "never a
 * bound" must not be read as licensing. That is a population the solver's own outcome seam handles by keeping
 * both arms with no record; refusing the fork here is narrower and the abort names it. (c) The TRUE arm is
 * realised by handing the matcher THE OPERAND'S OWN BYTES, which satisfy every §6.1/§6.2 operator and
 * §6.6/§6.7's synthesised ones — `=`/`|=` by equality, `^=`/`$=`/`*=` because a string is its own prefix,
 * suffix and substring, `~=` because a one-token list contains its token — with EXACTLY ONE exception: §6.1's
 * `~=` says "If "val" contains whitespace, it will never represent anything", so a whitespace operand cannot
 * satisfy its own test and its TRUE arm has no realisation here. A record that is undetermined and NOT forkable
 * keeps the abort.
 *
 * `declines` IS HOW MANY ASKS DECLINED and the recorded predicate is the FIRST of them — see the residual at
 * `host_attr_value_read`. */
typedef struct {
    bool                     undetermined;
    bool                     forkable;
    JSValue                  over;
    unsigned                 declines;
    lxb_css_selector_match_t op;
    bool                     insensitive;
    char                     attr[64];
    char                     operand[96];
} SelectorUndet;

/* THE WIDTH A PREDICATE'S KEY IS SPELLED INTO. Sized from the record above rather than guessed: the two
   copies plus §6's longest operator, the `i`, the brackets and the algorithm name. */
#define SELECTOR_UNDET_KEY_MAX 256

/* THE PREDICATE'S CONSTRAINT KEY — `step_fork_run`'s `op`, spelled through ONE entry so a machine's ask and
   the same machine's re-ask after a park cannot differ by a character, and so that the operator's own spelling
   has one source. Returns false when it would truncate, which a truncated `attr` or `operand` has already made
   impossible by clearing `forkable`. */
bool selector_undet_key(char *out, size_t cap, const SelectorUndet *u);

/* ARE THESE TWO RECORDS THE SAME §6 TEST? — the four fields `selector_undet_key` spells, compared. It is
   EXPORTED rather than each side asking it because there were two sites asking it and two answers to one
   question is the shape that drifts: the matcher's own supply grants an arm on it and the machine that forked
   asserts on it, and if those ever disagreed the supply would be refused for a predicate the machine believes
   it proved — a walk forking over one question for ever. The VALUE is deliberately not in here: it is the other
   half of the key and its accessor is the solver's, so the one site that needs both asks each once. */
bool selector_pred_same(const SelectorUndet *a, const SelectorUndet *b);

/* Selectors 4 §17.3 — does `node` match `list`? `out_spec` receives the HIGHEST specificity that matched (a
   list matches through whichever of its selectors did, and CSS 2.1 §6.4's cascade weighs that one), or is
   NULL for a caller that only asks the question. A non-element never matches.

   `out_undet` IS THE CALLER'S DECLARATION THAT IT CAN CARRY KLEENE'S THIRD VALUE, and the abort that used to
   stand here unconditionally now stands for a caller that passes NULL — or for one whose answer is
   undetermined in a shape no fork can realise (`SelectorUndet::forkable`). It is the out-parameter the old
   note here said the next diff would build, and `document.c`'s selector walk is the consumer that makes it
   one rather than a write whose reader is the crash it replaces.
   THE TWO CALLERS ARE NOT ALIKE AND THIS IS ROUTING RATHER THAN A FALLBACK, by §C-stack's own test: delete
   the abort and the question "can this caller keep the arm" still has to be asked, because a caller that
   cannot must not be handed one. `document.c`'s walk is a step machine and holds the resume point
   `engine_prepare_fork` requires; the cascade's is a plain C loop inside `cssom_cascaded_value` and that same
   function DFAILs by name for one, naming JS_CFUNC_STEP_DEF as the declaration to build. So the cascade half
   is a step machine to DECLARE and this signature is what it will pass when it exists.

   `decided` IS WHAT THIS FLOW HAS ALREADY PROVED ABOUT ONE §6 PREDICATE, or NULL, and it is the half that
   makes an answered fork ACTIONABLE. An arm that says "this test HOLDS" is realised by supplying the matcher
   the predicate's own operand for that ONE ask — so the matcher, not this engine, composes the answer through
   §6.6's `~=`, the compound's AND, the list's OR and any `:not()`, and a compound whose LATER member is also
   undetermined declines again and is forked in its own right rather than being decided by an arm that was
   never about it. The record's `over` must be the value the arm was about and is compared by
   `concolic_ident_c`, which is what the constraint key is composed from, so a park and a cross-session resume
   compare the same fact the solver filed. The FALSE arm needs nothing here: an undetermined read already
   behaves as no-match in the matcher's control flow, so the answer a caller already holds IS the answer under
   "this test does not hold". */
bool selector_match_node(lxb_dom_node_t *node, const lxb_css_selector_list_t *list,
                         lxb_css_selector_specificity_t *out_spec,
                         const SelectorUndet *decided, SelectorUndet *out_undet);

#endif
