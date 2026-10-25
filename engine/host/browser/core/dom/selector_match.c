/* Selectors 4 §17.3 "Match a Selector Against an Element" and Selectors 4 §17.1 "Parse A Selector" — the
 * agent's selector matcher. See selector_match.h for why the two lexbor objects below
 * live here and not in the machines that use them. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/dom/selector_match.h"
#include "core/html/custom_elements.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/decide.h"
#include "solver/dom_cow.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */
#include "solver/flow.h"

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
 * THE PREDICATE THIS MATCH DECLINED ON -- the ask's own identity, kept so that the crash at
 * `selector_match_node` names the TEST rather than only the selector it was inside, and so that a caller that
 * can KEEP the arm can compose the fork's key from it.
 *
 * WHY THE SELECTOR TEXT IS NOT ENOUGH. A real sheet's selector is a LIST, and the list is what the crash can
 * already serialize; which of its simple selectors read a value the host declined to state is not recoverable
 * from that text, because most of them read one. `#app, .theme-dark, [data-theme="dark"]` poses three
 * questions and the reader has to know WHICH to go and fork on -- and the operator is half of that, since
 * `[att=dark]` and `[att^=dark]` are two questions over one value.
 *
 * IT IS THE OPERAND HALF OF THE FORK'S KEY AND THAT IS ITS SECOND REASON. Solver-half keys a fork on "the
 * PREDICATE's own identity -- operator and both operands", so the fork that this crash stands in for cannot be
 * composed at all from a scope bit: `[att=dark]` and `[att=light]` over ONE undetermined value are TWO
 * questions, and a key that merged them would answer one arm for both and leave the flow standing on two
 * constraints saying the attribute is two different strings. This record is where that key's operator and
 * operand come from.
 *
 * COPIED AND NOT BORROWED, which is the one thing that makes it sound to read after the match returns. The
 * attribute lives on the element and the operand points into the compiled list, and both outlive the match --
 * but the LIFETIME ARGUMENT for borrowing anything out of a match is "there is no rest point inside one", and
 * that argument stops at the return. A copy has no lifetime question, and a diagnostic's cost is paid on the
 * decline path only. Truncation is correct for a diagnostic, exactly as it is for the selector text below.
 *
 * IT RUNS IN BOTH BUILDS, AND ITS OWN RETIREMENT CONDITION IS WHY. It read "DEV ONLY, BECAUSE ITS ONLY
 * CONSUMER IS THE CRASH", with "RETIREMENT: this record's gate goes when the fork reads it, because the key is
 * then a correctness datum in both builds" -- and the fork at `document.c`'s selector walk is that reader. The
 * retired argument is kept rather than deleted because it was the same argument this file's value seam once
 * gave for compiling its own work out, and a reader who re-derives it from "a diagnostic costs nothing in
 * release" will gate this again and take the fork's key with it.
 *
 * NAMED RESIDUAL -- IT IS THE FIRST DECLINE OF THE MATCH AND NOT THE ONE THE CONCLUSION RESTS ON. WHAT IS NOT
 * COVERED: an undetermined read behaves as no-match in control flow and the scope's bit is STICKY, so a
 * compound whose later member would have failed definitely still concludes UNKNOWN, and the predicate named
 * here is then a test whose answer never mattered. The FIRST is recorded rather than the last because the last
 * is a test the matcher may have already abandoned by backtracking, which is strictly worse. WHAT THE NEXT
 * DIFF BUILDS: the undetermined predicate recorded on lxb_selectors_nested_t beside that scope's own bit, so
 * the conclusion carries its own cause and a scope that fails definitely discards it with the attempt. HOW ITS
 * ABSENCE WOULD SHOW, as an observation: a crash naming a predicate whose attribute the selector it prints
 * tests in a compound that also tests something the element plainly does not have.
 */
/* THE RECORD IS THE HEADER'S, so the crash's fields and the fork's key fields are ONE list. `seen` is that
   type's `undetermined`: the record is written at the DECLINE and the conclusion is read from the arena, and
   the assert at `selector_match_node` is what holds the two together. */
static SelectorUndet g_undet;

/* Whether the agent's arena is inside a match (see the arena note below). */
static bool g_in_match;

/* BOUNDED, ALWAYS NUL-TERMINATED, and the length is the DESTINATION's because the source is a lexbor string
   with no terminator of its own. IT REPORTS TRUNCATION, and that is a correctness answer rather than a
   diagnostic's tidiness: these two copies are fields of the fork's KEY, so an operand cut at the buffer's
   width spells two different operands the same way and merges their questions -- the exact merge the key
   exists to prevent, arriving through the buffer. `SelectorUndet::forkable` is where the answer lands. */
static bool sel_undet_copy(char *dst, size_t cap, const lxb_char_t *data, size_t len)
{
    bool whole = len <= cap - 1;

    if (!whole) len = cap - 1;
    if (data != NULL && len != 0) memcpy(dst, data, len);
    dst[len] = '\0';
    return whole;
}

/* §6.1/§6.2's OPERATOR, spelled. A `default` arm here answers "?" rather than asserting, and that reasoning is
   UNCHANGED for the reason it was written: this is the text of a crash message, and a diagnostic that can itself
   abort trades a report for a second abort at a worse site.
   WHAT CHANGED IS THAT THE SAME STRING IS NOW HALF A CONSTRAINT KEY, so the `?` has a second meaning it must
   never carry: two operators outside the enumeration would spell ONE key and merge two questions. The operand
   is one LEXBOR enumerates and every one of its six members is spelled above — so the guard is sound where it
   is and the INVARIANT belongs where the key is composed, which is sel_undet_compose. Asserting it here would
   put it inside the crash. */
static const char *sel_undet_op(lxb_css_selector_match_t m)
{
    switch (m) {
    case LXB_CSS_SELECTOR_MATCH_EQUAL:     return "=";
    case LXB_CSS_SELECTOR_MATCH_INCLUDE:   return "~=";
    case LXB_CSS_SELECTOR_MATCH_DASH:      return "|=";
    case LXB_CSS_SELECTOR_MATCH_PREFIX:    return "^=";
    case LXB_CSS_SELECTOR_MATCH_SUFFIX:    return "$=";
    case LXB_CSS_SELECTOR_MATCH_SUBSTRING: return "*=";
    default:                               return "?";
    }
}

/* CAN THIS PREDICATE'S TRUE ARM BE REALISED BY HANDING THE MATCHER THE OPERAND'S OWN BYTES? -- the one
   question `SelectorUndet::forkable` asks that is about §6 rather than about a buffer, and the whole of the
   exception is one operator. Every §6.1/§6.2 operator is satisfied by its own operand (`=`/`|=` by equality,
   `^=`/`$=`/`*=` because a string is its own prefix, suffix and substring) and so are §6.6's and §6.7's
   synthesised ones -- except §6.1's `~=`, which reads the value as "a whitespace-separated list of words, one
   of which is exactly val" and whose own text says "If "val" contains whitespace, it will never represent
   anything". So a whitespace operand's TRUE arm is a world no value satisfies and there is nothing to hand the
   matcher; declining to declare the fork keeps the abort, which is where that case is named.
   ITS COUNTERPART IN THE MATCHER IS A NAMED NEXT DIFF AND NOT A SECOND COPY OF THIS TEST: host_attr_value_read
   records that `lxb_selectors_match_attribute` should test the operand for whitespace beside its length test,
   after which such a predicate never ASKS and this arm answers about an empty population. */
static bool sel_undet_satisfiable(const SelectorUndet *u)
{
    size_t i;

    if (u->op != LXB_CSS_SELECTOR_MATCH_INCLUDE) return true;
    for (i = 0; u->operand[i] != '\0'; i++) {
        /* §6.1's "whitespace" is CSS Syntax 3 §3.2 "Definitions"' -- newline, tab and space, with §3.3
           "Preprocessing the input stream" having already turned CR and FF into newlines. */
        if (u->operand[i] == ' ' || u->operand[i] == '\t' || u->operand[i] == '\n') return false;
    }
    return u->operand[0] != '\0';
}

/* The bytes that realise a failing arm for one asking predicate: a value its test rejects. Only `=` and `|=`
   ask with an empty operand (the other four are decided false by it without asking), and both reject a
   one-character value that is not `-`; with a non-empty operand every operator rejects the empty value, since
   each needs at least the operand's length (§6.1's `~=` needs a word, and the empty value has none). */
static const char *sel_undet_refuting_bytes(const SelectorUndet *u)
{
    DCHECK(u->operand[0] != '\0' || u->op == LXB_CSS_SELECTOR_MATCH_EQUAL || u->op == LXB_CSS_SELECTOR_MATCH_DASH,
           "a §6 predicate with an empty operand asked about a value under an operator that is decided by the "
           "operand alone, so lxb_selectors_match_attribute asked where it states it does not");
    return u->operand[0] != '\0' ? "" : "x";
}

/* WHICH TEST THIS ASK IS -- the record's four key fields, composed from the seam's own arguments, through ONE
   entry. Every decline and every decided-arm comparison goes through it, so the fields the crash prints, the
   fields the key is spelled from and the fields a supply is granted on cannot be three spellings of one test.
   IT IS PER ASK AND NOT PER MATCH, which is the defect it exists to make impossible: a selector list is an OR
   and a combinator walk asks per candidate, so ONE match declines several times and a comparison made against
   the FIRST decline's fields would grant a supply to a later ask about a different attribute -- deciding a test
   nothing forked, and then refusing the one that was forked, which is a walk that forks over one question for
   ever. Returns the record's own `forkable`, which is the only thing a caller asks it. */
static bool sel_undet_compose(SelectorUndet *u, const lxb_dom_attr_t *attr,
                              const lxb_selectors_predicate_t *pred, JSValue taint)
{
    const lxb_char_t *an;
    size_t an_n = 0;
    bool whole;

    /* THE OPERATOR IS ONE OF THE SIX LEXBOR ENUMERATES, asserted where the KEY is built rather than where it is
       spelled: `sel_undet_op` answers "?" for anything else so that a crash cannot abort inside its own message,
       and one "?" standing for two operators would file two questions under one key. It is a CLOSED enumeration
       that lexbor owns, so this is a guard on a state the three ask sites make impossible (each sets one of the
       six by hand) and not a capability this engine has yet to build. */
    DCHECK(pred->match >= LXB_CSS_SELECTOR_MATCH_EQUAL && pred->match < LXB_CSS_SELECTOR_MATCH__LAST_ENTRY,
           "Selectors 4 §6's value seam was asked about an operator outside the closed set lexbor enumerates — "
           "the three arms that ask each set one of the six by hand (§6.1/§6.2's, and §6.6's and §6.7's "
           "synthesised ones), so a seventh is a matcher arm that composed its predicate somewhere else");
    memset(u, 0, sizeof *u);
    u->undetermined = true;
    u->declines = 1;
    u->op = pred->match;
    u->insensitive = pred->insensitive;
    u->over = taint;   /* BORROWED -- see SelectorUndet; a caller that keeps it dups it */
    an = lxb_dom_attr_local_name(attr, &an_n);
    whole = sel_undet_copy(u->attr, sizeof u->attr, an, an_n);
    whole &= sel_undet_copy(u->operand, sizeof u->operand, pred->operand->data, pred->operand->length);
    /* THE KEY IS EXACT, THE VALUE IS SPELLABLE AND THE TRUE ARM IS REALISABLE, asked as ONE bit because a
       caller can act on none of the three alone -- see SelectorUndet::forkable for each. The identity is not
       decoration on the key: it is what the supply below RECOGNISES this predicate's value by, so a value
       without one could be forked and its arm could never be granted. */
    u->forkable = whole && concolic_ident_c(taint) != NULL && sel_undet_satisfiable(u);
    return u->forkable;
}

bool selector_pred_same(const SelectorUndet *a, const SelectorUndet *b)
{
    DCHECK(a != NULL && b != NULL, "two §6 predicates were compared and one of them is not there");
    /* THE FIELDS, COMPARED RATHER THAN RE-SPELLED. A comparison over `selector_undet_key`'s composed string
       would be a SECOND consumer of that format, and the two could then disagree about what the key names. */
    return a->op == b->op && a->insensitive == b->insensitive
           && strcmp(a->attr, b->attr) == 0 && strcmp(a->operand, b->operand) == 0;
}

void selector_pred_settle(const SelectorUndet *u, bool holds)
{
    char *hole;

    DCHECK(u != NULL && u->undetermined && u->forkable,
           "an answered §6 predicate was settled from a record no consumer could have forked");
    /* §6.1's `[att=val]` "Represents an element with the att attribute whose value is exactly "val"", so only a
       case-sensitive `=` states the value; the other operators and §6.3's `i` leave a set. */
    /* Named residual. Not covered: a holding test under another operator or under `i` determines no value, so
       such answers are not checked against each other or against a later `=` (`[a^=da]` and `[a=light]` can
       both hold in one flow). Next diff: those answers filed as the value's string facets (prefix, suffix,
       substring, case-folded equality), against which a later fork's holding arm is pruned. Absence shows as:
       a flow whose pinned attribute value fails a test the same flow took as holding. */
    if (u->op != LXB_CSS_SELECTOR_MATCH_EQUAL || u->insensitive) return;
    if (holds) {
        /* Chosen by a fork with no real arm stated, so the pin is a witness no observation confirmed. */
        if (concolic_pin_tostring(u->over, u->operand)) {
            flow_mark_pinned_value();
            return;
        }
        DFAILF("Selectors 4 §6.1's `[%s=\"%s\"]` holds in this flow and the pin store could not state the "
               "attribute's value: the value is a derivation, which owns no `src` to pin, or its type facet admits "
               "another value whose ToString is the operand. A later test with another operand then forks into a "
               "world where the attribute equals two strings. Build a determination keyed by the value's identity "
               "that concolic_pin_bytes reads", u->attr, u->operand);
        return;
    }
    hole = concolic_hole_key(concolic_shape_c(u->over));
    if (hole != NULL) {
        concolic_exclude(hole, u->operand);
        free(hole);
    }
}

bool selector_undet_key(char *out, size_t cap, const SelectorUndet *u)
{
    int wrote;

    DCHECK(out != NULL && u != NULL, "a §6 predicate's constraint key was spelled from nothing or into nowhere");
    /* THE ALGORITHM AND THE FOUR FIELDS, AND NOTHING ELSE. It is deliberately NOT the selector's text: the
       question this key names is "does this test hold of this value", which `[att=dark]` and
       `:not([att=dark])` ask ONCE between them -- §Solver-half's "keyed by the PREDICATE's own identity" --
       and a key carrying the rule would fork a world for each. The ATTRIBUTE is in it because two attributes
       of one element can hold two different unknowns and §6.6's and §6.7's synthesised predicates are tests on
       `class` and `id`; `insensitive` is in it because §6.3's `i` changes which values satisfy the test and
       therefore which question is asked. The VALUE is the other half and is `step_fork_run`'s `over`, which
       the solver composes in -- see solver/decide.c's outcome_key. */
    wrote = snprintf(out, cap, "Selectors 4 §17.3 over [%s%s\"%s\"%s]",
                     u->attr, sel_undet_op(u->op), u->operand, u->insensitive ? " i" : "");
    return wrote > 0 && (size_t)wrote < cap;
}

/*
 * THE HOST'S ANSWER FOR AN ATTRIBUTE WHOSE VALUE IS NOT BYTES — Selectors 4 §6 "Attribute selectors" read
 * against §Solver-half's unknown.
 *
 * WHAT THE MATCHER WOULD OTHERWISE DO. DOM §4.9's write stores a concolic value's SHAPE in the tree, because
 * a concolic has no bytes and `core/dom/element.c`'s write says so at its own site ("A concolic value has no
 * bytes to store ... The SHAPE is the honest byte form for the tree") and files the value itself in the
 * per-flow taint shadow. A shape is a DISPLAY FORM and not the value: nothing the page could write makes it
 * equal to `dark`, so every §6.1 `=` test against it answers false, and so does every test against every
 * other operand. Both arms of a two-armed question would therefore be decided, and §Solver-half permits
 * pruning only a CONTRADICTED branch — "a contradicted branch is pruned (sound-only — uncertainty keeps the
 * arm)". Neither arm is contradicted here.
 *
 * SO THIS FUNCTION DECLINES TO STATE A VALUE, and that is now an answer the matcher can carry rather than a
 * crash. THE CRASH MOVED AND DID NOT GO: it is at `selector_match_node` below, where the WHOLE selector's
 * answer is known, and its old reasoning is retired rather than deleted because a reader will re-derive it.
 * It read: "WHY THIS IS A CRASH AND NOT A REFUSAL — §Offensive-programming's category (2): a capability that
 * should exist and does not." That was exactly right about the CAPABILITY and wrong about the SITE, and the
 * difference is what (1) bought. A crash HERE fires once per attribute test per rule per element and knows
 * nothing about the selector it is inside, so it fired for `#known, [att=x]` on an element `#known` matches
 * — a question whose answer was never in doubt — and it fired three times for a selector that poses one
 * question. A crash at the CONCLUSION fires only where the selector's own answer is undetermined, which is
 * the population a fork is for. Nothing about the category changed; the engine simply now knows which
 * asks are real.
 *
 * IT WAS NEVER THE PAGE-HELD ABORT SWITCH §WHOSE-BYTES-STATE-THE-VALUE FORBIDS, and it is further from one
 * now: the value declined on is one THIS ENGINE MINTED to stand for something it does not know, not bytes a
 * stranger stated, and no string a page can write reaches this. The refusal itself asserts nothing at all.
 *
 * How an undetermined value is answered, in the order this function and its consumers apply it:
 *   - A flow that determined the value has a pin, read first through concolic_pin_bytes, and the matcher's own
 *     comparison then decides every operator on real bytes. The page's own `===` makes one; so does a holding
 *     `=` a consumer's fork settles (selector_pred_settle). A value the page derived before storing it
 *     (`'x-' + cfg.theme`) owns no `src` and has no pin, so it is undetermined like any other.
 *   - Otherwise the seam answers UNDETERMINED. lexbor's matcher carries Kleene's third value through `:not()`,
 *     `:is()`, `:has()`, `:nth-child(… of …)` and the combinators, with its one inverted arm at
 *     lxb_selectors_state_after_not; an undetermined read is no-match in control flow, so a compound abandons
 *     at it (UNKNOWN where FALSE was available, which keeps an arm rather than dropping one).
 *     core/css/media_query.c holds its own Kleene table because lexbor is compiled with LEXBOR_INC alone and
 *     reaches no host header.
 *   - A consumer that can keep the arm forks on the predicate, keyed by selector_undet_key and the value's
 *     identity: core/dom/document.c's walk forks itself, and the cascade's resting build hands the decline to
 *     its layout job, whose step machine forks (layout_job_decline). The fork is over the predicate rather than
 *     the selector, so a later undetermined member declines and forks in its own right, and the answer comes
 *     back here from the flow's constraint.
 *
 * WHAT IT DOES NOT COVER, AND THE NEXT DIFF FOR IT: §6.1's `~=` with WHITESPACE in its operand ("If "val"
 * contains whitespace, it will never represent anything") is decided false by the operand alone, exactly as
 * the empty-operand arms the matcher already declines to ask about are — so this is asked about a match that
 * is false under every value the attribute could hold, and now answers UNDETERMINED for it rather than
 * false. The next diff tests the operand for whitespace beside the length test in
 * `lxb_selectors_match_attribute`. It would show as the abort below naming a selector whose only test
 * against the undetermined attribute is a `~=` whose operand has a space in it. */
static lxb_selectors_value_t host_attr_value_read(const lxb_dom_node_t *node,
                                                  const lxb_dom_attr_t *attr,
                                                  const lxb_selectors_predicate_t *pred,
                                                  lexbor_str_t *out, void *ctx)
{
    lxb_dom_element_t *el;
    JSValue taint;
    const char *pinned;
    SelectorUndet ask;

    (void)ctx;
    /* THE O(1) PRECONDITION FIRST. This runs per attribute test per rule per element, and resolving §4.9's
       key out of the attribute allocates; a document that never put an unknown in an attribute — which is
       most of them — pays one load for its whole cascade. */
    if (attr_shadow_count() == 0) return LXB_SELECTORS_VALUE_TREE;
    DCHECK(node != NULL && attr != NULL && out != NULL,
           "Selectors 4 §6's value seam was asked about no element, no attribute or with nowhere to put its "
           "answer — the matcher holds all three at the comparison, so a missing one is a caller that "
           "composed the ask somewhere else");
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return LXB_SELECTORS_VALUE_TREE;
    el = lxb_dom_interface_element((lxb_dom_node_t *)node);
    taint = dom_cow_attr_taint_node(el, attr);   /* BORROWED */
    if (!concolic_is(taint)) return LXB_SELECTORS_VALUE_TREE;

    /* WHAT THIS FLOW HAS ALREADY PROVED, ASKED BEFORE ANYTHING ELSE — §Solver-half's CONCRETIZE-ON-PIN, and
       the reason it belongs at a READ rather than at a fork: the determination is made by the PAGE'S OWN
       predicate, in a flow the interpreter's branch seam already minted, and a value pinned there is a real
       string by the time the cascade asks. The page that writes an unknown into an attribute is very often
       the same page that branches on that same unknown a few statements later, so this arm is not a corner —
       it is the shape the document this seam was built from actually has.
       AND IT IS ASKED OF THE *VALUE*, NEVER OF A KEY THIS FILE COMPOSES. concolic.h states why: a pin is
       stored under `src`, `src` is the INJECTION identity, and a DERIVATION inherits its first unknown
       operand's — so a key spelled here would hand `'x-' + cfg.theme` back `cfg.theme`'s bytes as if they
       were the concatenation's. The record knows which of the two it is holding and this file cannot, so the
       question goes to the record. NULL is the positive statement that this flow has determined nothing about
       this value, which is exactly the population that answers UNDETERMINED below. */
    pinned = concolic_pin_bytes(taint);
    if (pinned != NULL) {
        /* THE BYTES ARE THE ATTRIBUTE'S VALUE AND NOT A RENDERING OF IT. DOM §4.9 "Interface Element"'s
           attribute value is a string, and `setAttribute` reaches it through a Web IDL DOMString conversion —
           §7.1.19 ToString — which is exactly the spelling the pin store holds. So this is the byte form the
           tree WOULD have held had the page run with the value this flow proved it has, and every operator
           §6 and §6.2 define is then decided by the matcher's own comparison on a real string.
           BORROWED FOR THE LENGTH OF THE MATCH, which is what makes handing out an interior pointer sound
           here and nowhere else: `lxb_selectors_match_node` runs none of the page's code and has no rest
           point inside it (see the arena note below), so nothing can pin again, reset the flow or switch away
           between this line and the comparison that reads it. */
        out->data = (lxb_char_t *)pinned;
        out->length = strlen(pinned);
        return LXB_SELECTORS_VALUE_HOST;
    }

    /* AND WHERE THE FLOW HAS PROVED NOTHING, THE ANSWER IS THAT THERE IS NO VALUE — Selectors 4 §6's test
       has no two-valued answer here, and saying so is a POSITIVE STATEMENT rather than a refusal. A flow that
       reached the cascade having committed to NEITHER arm has no determination to read, and answering it
       either way would decide a branch nothing contradicted; the matcher now carries that outward through
       `lxb_selectors_t::unknown` and `selector_match_node` is where it is read.
       IT ASSERTS NOTHING ON THE VALUE, which §WHOSE-BYTES-STATE-THE-VALUE requires of every seam whose
       operand a page can reach — and the old crash here did not either, the value being one THIS ENGINE
       MINTED rather than bytes a stranger stated. What changed is not the entitlement but the SITE: the
       question this function is asked ("what are this attribute's bytes") has an honest answer and the
       question worth aborting on ("does this selector match") is one function away.
       THIS RUNS IN BOTH BUILDS, and the old reason for compiling the work out is retired: it read "the WORK
       is compiled out with the crash because its only consumer is the crash", which was true while the seam's
       only product was a `DFAIL` and became false the moment the pin read landed above it. Both answers this
       function gives are now CORRECTNESS answers — a release build that skipped either would render a
       different page from the dev build that measured it — and the `attr_shadow_count` load above is what
       keeps the whole seam free for every document that stores no unknown. */
    /* §6.1's `[att]` reads no value and never asks, so every ask that reaches here has an operand --
       lxb_selectors_predicate_t says so at its own site, and this is the one place that claim is relied
       on rather than merely stated. */
    DCHECK(pred->operand != NULL,
           "Selectors 4 §6's value seam was asked with no operand. Every shape that asks reads a value "
           "and therefore has one to compare against; the shapes decided by the operand alone do not "
           "ask at all. A NULL here is a matcher arm that started asking about `[att]`");
    /* WHICH ASK THIS IS, composed for THIS ask and not for the match -- see sel_undet_compose. */
    sel_undet_compose(&ask, attr, pred, taint);

    /* What this flow has already answered about this test: a fork over this predicate and value that any
       consumer took, settled in the flow's constraint under the key the fork was asked with. It is realised
       for this one ask by bytes that decide this test and no other: a holding answer by the operand itself,
       which satisfies every operator a forkable predicate has (sel_undet_satisfiable), a failing one by bytes
       the test rejects (sel_undet_refuting_bytes). The matcher composes that answer through §6.6's `~=`, the
       compound's AND, the list's OR and any `:not()`; a second test of the same attribute is a different
       predicate and declines in its own right. What an answer establishes about the value itself is
       selector_pred_settle's, read above as a pin. */
    if (ask.forkable) {
        char key[SELECTOR_UNDET_KEY_MAX];
        int c = selector_undet_key(key, sizeof key, &ask) ? solver_outcome_settled(taint, key, 2) : -1;

        if (c >= 0) {
            /* These bytes only make the byte matcher return the answer the constraint holds; they are no
               value of the attribute, so they reach nothing but lxb_selectors_match_attribute. */
            DCHECK(g_in_match, "a settled §6 predicate was realised outside the agent's selector match, so its "
                               "bytes could reach a reader that takes them for the attribute's value");
            /* Borrowed for the match: the operand is the compiled list's, which nothing writes after it is
               built, and the refuting bytes are literals. */
            if (c == 1) {
                out->data = pred->operand->data;
                out->length = pred->operand->length;
            } else {
                const char *no = sel_undet_refuting_bytes(&ask);

                out->data = (lxb_char_t *)no;
                out->length = strlen(no);
            }
            return LXB_SELECTORS_VALUE_HOST;
        }
    }

    /* WHICH ASK DECLINED, for the crash at `selector_match_node` and for the fork's key after it. RECORDED
       AFTER THE SUPPLY AND NOT BEFORE IT, because a supplied ask is not a decline: counting one would report a
       match that declined on nothing as having declined once, and the count is what a reader of the crash uses
       to tell one question from ten. The FIRST decline is kept and later ones only counted -- see SelectorUndet
       for why the first rather than the last, and for what a first that the conclusion does not rest on costs. */
    if (!g_undet.undetermined) g_undet = ask;
    else                       g_undet.declines++;
    return LXB_SELECTORS_VALUE_UNDETERMINED;
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
    if (!list) return NULL;   /* Selectors 4 §17.1's `failure` */
    sl = lexbor_malloc(sizeof(*sl));
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
    /* A selector LIST matches through whichever of its selectors matched, and CSS 2.1 §6.4's cascade uses the
       highest —
       `#id, div { … }` on a div with that id contributes the id's weight, not the tag's. */
    if (!h->matched || spec > h->spec) h->spec = spec;
    h->matched = true;
    return LXB_STATUS_OK;
}

#if APICLIENT_DEV
/* THE SELECTOR THE ABORT IS ABOUT, in the author's own spelling. A compiled list is a graph and a reader
   standing at a crash needs the text they wrote; lexbor already serializes one, so this is the callback its
   signature asks for and nothing more. Truncation is the correct behaviour for a diagnostic — a selector
   longer than this names itself in its first hundred bytes — and the buffer is a caller-owned automatic. */
typedef struct { char buf[256]; size_t n; } SelText;

static lxb_status_t sel_text_cb(const lxb_char_t *data, size_t len, void *vctx)
{
    SelText *t = vctx;
    size_t room = sizeof(t->buf) - 1 - t->n;

    if (len > room) len = room;
    memcpy(t->buf + t->n, data, len);
    t->n += len;
    t->buf[t->n] = '\0';
    return LXB_STATUS_OK;
}
#endif

bool selector_match_node(lxb_dom_node_t *node, const lxb_css_selector_list_t *list,
                         lxb_css_selector_specificity_t *out_spec, SelectorUndet *out_undet)
{
    SelHit h = { false, 0 };

    /* THE CALLER'S RECORD IS WRITTEN HERE, BEFORE ANY RETURN CAN BE TAKEN, AND NOT ONLY AT THE TAIL.
       The non-element arm below returns a definite `false` -- a text or comment node matches no selector and
       asks nothing -- and it returned it with `out_undet` UNWRITTEN, so the one caller that keeps a record read
       an UNINITIALISED stack struct on its very next line. DOM §1.3 step 3's walk visits every node of the
       subtree, so that is MOST of them rather than an edge case.
       ONE READ, TWO ABORTS, NEITHER AT THE SITE THAT PRODUCED IT, which is why this is recorded here and not
       where it fired: `undetermined` and `forkable` read true from whatever the slot held, so the walk filed a
       fork; `selector_undet_key` composed the key from `attr`/`operand`, which printed the previous frame's
       bytes; and `over` was a JSValue the outcome seam then refused as not unknown. A constraint key is what a
       parked flow's recorded answers are filed under, so the quiet arm of this was a key naming stale memory.
       IT IS DISCHARGED BEFORE THE FIRST BRANCH RATHER THAN AT EACH CALLER, because the contract is this
       function's: a caller-side initialiser is one guard per call site and the next call site forgets it,
       while a new early return added anywhere below this line inherits a written record. */
    if (out_undet != NULL) {
        memset(out_undet, 0, sizeof *out_undet);
        out_undet->over = JS_UNDEFINED;   /* a tag of 0 is a tag and not `undefined` -- g_undet's own reason */
    }

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
    /* CLEARED HERE AND NOT AT THE DECLINE, on the same span the arena clears its own bit on. lexbor sets
       `unknown` false at the entry of match_node and says why ("a field the caller is about to read may not be
       cleared under it"), so a record cleared anywhere else would answer about a PREVIOUS match for a match
       that declined on nothing. `over` is set to a REAL JSValue rather than left as the memset's zero bytes,
       because a tag of 0 is a tag and not `undefined` -- it is borrowed, so nothing is released. */
    memset(&g_undet, 0, sizeof g_undet);
    g_undet.over = JS_UNDEFINED;
    /* lxb_selectors_match_node, not a subtree find: a combinator is resolved by walking UP from the candidate
       through the whole document, so §1.3's scoped matching still holds when the caller filters the results to
       a subtree — `el.querySelectorAll('div p')` finds a <p> under `el` whose <div> ancestor is OUTSIDE it. */
    lxb_selectors_match_node(g_arena, node, list, sel_hit_cb, &h);
    g_in_match = false;

    /* THE THIRD ANSWER, HANDED TO A CALLER THAT SAID IT COULD KEEP IT. `g_arena->unknown` is Kleene's value
       carried out of the match and the record is which ask produced it; the two are ONE fact and the assert
       below is what holds them together, so this fills the caller's record from both rather than from either.
       IT IS GATED ON `!h.matched`, WHICH IS KLEENE'S OR: a selector list matches through whichever of its
       selectors did, so a definite match makes the list's answer TRUE whatever some other branch could not
       decide -- `#known, [att=x]` on an element `#known` matches is ANSWERED, and the seam being asked along
       the way is not a question this engine failed. */
    if (out_undet != NULL) {
        *out_undet = g_undet;
        out_undet->undetermined = !h.matched && g_arena->unknown;
        out_undet->forkable = out_undet->undetermined && g_undet.forkable;
    }

#if APICLIENT_DEV
    /* The question this selector asked and this caller cannot keep: `g_arena->unknown` is Kleene's third
       value carried out of the match, so "did not match" is UNKNOWN rather than false, and either the caller
       passed no record (it has no resume point) or the record cannot be realised by a fork. It is gated on
       `!h.matched`, Kleene's OR: a list that matched through one selector is answered whatever another branch
       could not decide. The bit is one this engine set about its own capability; no page string reaches it.
       Named residual. Not covered: `out_spec` is a lower bound when a branch was undetermined, since a list
       reports the highest specificity that matched, so `div, #a[att=x]` on a div with that id weighs the rule
       at 0,0,1 where the undecided branch would weigh it 1,1,0, and CSS 2.1 §6.4 orders it wrongly against a
       competitor in between. Next diff: the record carries the undetermined branches' specificities, since a
       cascade that forks on the predicate forks on its weight too. Absence shows as: two rules whose winner
       changes with an attribute this engine never observed, reported with the loser's declaration and no
       question asked. */
    if (!h.matched && g_arena->unknown && (out_undet == NULL || !out_undet->forkable)) {
        SelText t;
        const lxb_char_t *tag;
        size_t tag_n = 0;

        t.n = 0; t.buf[0] = '\0';
        lxb_css_selector_serialize_list_chain((lxb_css_selector_list_t *)list,
                                              sel_text_cb, &t);
        tag = lxb_dom_element_local_name(lxb_dom_interface_element(node), &tag_n);
        /* THE BIT AND THE RECORD ARE ONE FACT AND BOTH SIDES CAN DISAGREE, which is what makes this an
           assertion rather than a restatement. lxb_selectors_host_attr_value is the ONLY site that ORIGINATES
           `unknown` — every other site that sets it (the nested-scope leave, the `of`-scope third exit, the
           per-node roll-up) propagates an inner bit outward — so a conclusion of UNKNOWN with no recorded ask
           is a matcher arm that invented the answer, and a recorded ask with no conclusion is this record
           outliving its span. Neither operand is a byte a page wrote: one is lexbor's own Kleene bit and the
           other is what this file wrote down one call earlier. */
        DCHECK(g_undet.undetermined,
               "Selectors 4 §17.3's answer is UNKNOWN and no ask in this match declined. The host value seam "
               "is the only place that answer is born, so either an arm set the conclusion without asking — in "
               "which case the bit is wrong — or this record was cleared inside the match it is about");
        DFAILF("<%.*s> vs `%s` — Selectors 4 §17.3 \"Match a Selector Against an Element\" answered UNKNOWN "
               "on `[%s%s\"%s\"]%s`, %u ask(s) undetermined: "
               "a test in this selector was decided from an attribute whose value this engine does not know "
               "and this flow has not pinned, and Kleene's rules carried that through the compound, the "
               "combinators and any `:not()`/`:is()` to the selector's own answer. THE MATCHER DECLINED "
               "RATHER THAN PICKING, which is correct and is as far as it can go: §Solver-half's \"a "
               "contradicted branch is pruned (sound-only — uncertainty keeps the arm)\" means the arm must "
               "be KEPT, and keeping it needs a consumer that can ask. NOTHING HERE CAN: `bool` has nowhere "
               "to put a third answer, so the cascade would receive `false` for a predicate nobody decided "
               "and both worlds would paint the same page. %s",
               (int)tag_n, tag ? (const char *)tag : "?",
               t.buf[0] ? t.buf : "?",
               g_undet.attr, sel_undet_op(g_undet.op), g_undet.operand,
               g_undet.insensitive ? " i" : "", g_undet.declines,
               /* WHY THIS CALLER CANNOT KEEP IT, and it is now TWO different answers rather than one — which
                  is the whole of what changed here: the abort no longer means "nothing in this engine can
                  carry a third answer", it means "not THIS caller, for THIS reason". */
               out_undet == NULL
                 ? "THIS CALLER PASSED NO `out_undet`: the cascade's build with no resting drive, reached by a "
                   "style read from a member that does not own a layout job, so nothing can rest and keep the "
                   "arm. A resting build keeps it (layout_job_decline, forked by the job's step machine). "
                   "Convert the member in the backtrace to a step machine that owns a driven job and asks "
                   "cssom_style_ask first, as `clientWidth` does."
                 : "THIS CALLER CAN KEEP AN ARM AND THIS PREDICATE'S CANNOT BE REALISED — either a field of "
                   "the key was TRUNCATED into its buffer (two operands would spell one question) or the TRUE "
                   "arm has no realisation, which for §6.1's `~=` means the operand contains whitespace and "
                   "its own text says such a selector \"will never represent anything\". The next diff for the "
                   "second is in `lxb_selectors_match_attribute`: test the operand for whitespace beside its "
                   "length test, after which such a predicate never asks at all.");
    }
#endif

    if (out_spec) *out_spec = h.spec;
    /* THE RELEASE ANSWER IS `h.matched`, which for an undetermined selector is FALSE — the same defined
       wrong answer every build gave before this seam existed, with no sibling to compose badly with, which
       is the one thing §THE-ARM-BENEATH-A-`DFAIL` asks of a release arm. */
    return h.matched;
}
