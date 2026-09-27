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

/* WHAT THIS FLOW HAS ALREADY PROVED ABOUT ONE §6 PREDICATE, for the length of ONE match -- the caller's
   `decided`, parked here because `lxb_selectors_host_cb_t`'s ctx is the ARENA's and is installed once at init,
   while this fact is the MATCH's. A file static is correct for exactly the reason one arena is: `g_in_match`
   asserts there is no rest point inside a match and no two matches can interleave, so the span this is live
   over is the span that assert already owns. Cleared on the way out beside the bit it is read with. */
static const SelectorUndet *g_decided;

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
 * the population (2)-(5) are for. Nothing about the category changed; the engine simply now knows which
 * asks are real.
 *
 * IT WAS NEVER THE PAGE-HELD ABORT SWITCH §WHOSE-BYTES-STATE-THE-VALUE FORBIDS, and it is further from one
 * now: the value declined on is one THIS ENGINE MINTED to stand for something it does not know, not bytes a
 * stranger stated, and no string a page can write reaches this. The refusal itself asserts nothing at all.
 *
 * WHAT THE NEXT DIFFS BUILD, IN ORDER, and why none of them is this one:
 *   (0) CONCRETIZE-ON-PIN AT THE MATCH — LANDED, AND IT IS WHAT THIS FUNCTION STILL DOES FIRST. A flow whose
 *       own `===` already determined the value HAS the answer, so the match need only ASK: §Solver-half's
 *       "once `x==='admin'` pins the value, a later READ of that source returns the pinned bytes, so a later
 *       branch on it is decided by RUNNING the real predicate on a real string and does not fork at all".
 *       `concolic_pin_bytes` is that read, and the matcher's own byte comparison is then the right one for
 *       EVERY operator §6 has — including §6.6's, which is `~=` over `class` and is the arm a real bundle
 *       reaches most (`el.className = <unknown>`). THE ONE THING THAT MADE THIS EASY TO MISS IS WORTH KEEPING
 *       AND IS ONE SENTENCE: the pin is written by the PAGE and is therefore in neither this file nor the
 *       sheet, so a reader of either sees a value nothing can decide and reaches for a fork.
 *       ITS RESIDUAL IS RETIRED BY (1) AND ITS ARGUMENT IS NOT, which is why the argument stays: a value the
 *       page DERIVED before storing it (`el.setAttribute('t', 'x-' + cfg.theme)`) files a DERIVATION in the
 *       shadow, and a derivation carries its first unknown operand's `src` (concolic_add_hook takes
 *       `ca ? concolic_src_c(a) : …` and says why), so a flow's pin on `cfg.theme` is a determination of the
 *       OPERAND and not of the stored value and `concolic_pin_bytes` answers NULL for it by construction.
 *       THE OBVIOUS REPAIR IS STILL THE ONE NOT TO MAKE: re-deriving the stored value at the READ would need
 *       the record to have kept its operands, and a value plus the expression that made it is the recorded
 *       transform-expression §Re-execution forbids BY NAME — the record deliberately keeps a shape, an
 *       identity and an example and no operands. The sound way to get a derivation's real bytes is the one
 *       this engine already has: RE-EXECUTE the write under the constraint, which is what a resumed flow and
 *       a candidate re-fire both do, and what the pin cannot do for a store that already happened. So the
 *       derived case is not a narrower version of the pin read; it is an UNDETERMINED value like any other,
 *       and three-valued matching is now its answer.
 *   (1) THREE-VALUED MATCHING — LANDED, AND IT IS WHY THIS FUNCTION RETURNS RATHER THAN ABORTS.
 *       `lxb_selectors_host_cb_t.attr_value_read` has a third answer, `:not()`, `:is()`, `:has()`,
 *       `:nth-child(… of …)` and the combinators propagate it, and `lxb_selectors_t::unknown` carries the
 *       conclusion out. The RULE is Kleene's and the arithmetic was never the work: the state machine already
 *       computes AND as a compound's short-circuit and OR as a list's and a combinator walk's, so what (1)
 *       added is a sticky per-scope bit and one inverted arm. ITS CAUSE IS NOT THE CAUSE IN
 *       core/css/media_query.c, whose `mq_not`/`mq_and`/`mq_or` hold the same table for a different reason —
 *       Media Queries Level 4 §3.1 "Evaluating Media Queries" makes a query UNKNOWN when the UA does not
 *       UNDERSTAND it (`<general-enclosed>`), where a selector here is perfectly understood and it is the
 *       ATTRIBUTE'S VALUE that is unknown. AND THE TWO TABLES ARE NOT SHARED, WHICH IS A FACT ABOUT THE BUILD
 *       AND NOT A CHOICE: `engine/build.mjs` compiles lexbor into its own archive with `-I` LEXBOR_INC ALONE,
 *       so no host header is reachable from `selectors.c` and a shared spelling would invert the embedding.
 *       What keeps them from drifting is that lexbor grew no table at all — it has one inverted arm, at
 *       `lxb_selectors_state_after_not`, and its own comment there is what a reader checks.
 *       WHAT (1) DOES NOT COVER is recorded where it is caused, at `lxb_selectors_host_attr_value`: an
 *       undetermined read behaves as no-match in CONTROL FLOW, so a compound abandons at the undetermined
 *       member rather than reaching a later member that would decide it false. That answers UNKNOWN where
 *       FALSE was available, which keeps an arm that could have been dropped and is the safe direction.
 *   (2) A CASCADE THAT REPORTS ITS UNANSWERED PREDICATE, rather than an answer it does not have. STILL OPEN,
 *       AND ITS "THIS IS NEXT" IS SPENT: the sentence read "THIS IS NEXT, AND `selector_match_node`'s abort
 *       below is where it lands: the answer exists now and the signature has nowhere to put it, so the abort is
 *       standing in for the out-parameter." The out-parameter LANDED — `selector_match_node`'s `out_undet` —
 *       and it landed for the DOM half, which is why the paragraph below this one was right about the order and
 *       this one's own numbering was not. What (2) still needs is unchanged and is a CONSUMER rather than a
 *       type: the cascade's walk has no resume point, so a third state at `cssom_cascaded_value` has nothing
 *       that can keep an arm and is a write whose reader is the crash it replaces.
 *       THE MEASUREMENT THAT SETTLED IT IS KEPT BECAUSE IT IS WHAT MAKES THE ORDER CHECKABLE, and not because
 *       the count is durable: GREPPED, the cascade's three callers are core/css/css_computed_value.c's
 *       `css_cv_cascaded` (which asks WHAT THE VALUE IS and reads NULL as css-cascade-5 §4.2's empty list),
 *       core/html/html_element_view.c's `hev_declares` and core/layout/used_value.c's
 *       `uv_require_readable_positioning` (which ask DOES THIS ELEMENT DECLARE IT and whose own comments say
 *       the answer's only outcome is a crash) — so all three spell `decl != NULL`, the only thing any of them
 *       can do with a third state is abort, and two of them abort already. In RELEASE every one of those aborts
 *       is compiled out, so the widening alone changes nothing observable in EITHER build.
 *       AND (4) IS A PREREQUISITE OF (3) RATHER THAN A REFINEMENT AFTER IT, which is the half of this list that
 *       cannot simply be reordered by a reader who notices. `step_fork_run` takes the unknown operand as `over`
 *       and the PREDICATE as `op`, and together they are the constraint key — so a fork keyed on anything
 *       coarser than (the value's identity, the operator, the operand) merges `[att=dark]` and `[att=light]`
 *       into ONE question, answers one arm for both, and leaves the flow standing on two constraints saying the
 *       attribute is two different strings. That is not a loose key, it is an inconsistent world, and it is
 *       what building (3) before (4) produces. THE KEY COULD NOT BE COMPOSED AT ALL until the value seam was
 *       told which test it was being asked about: `lxb_selectors_t::unknown` is a BARE BOOL and the callback
 *       took `(node, attr, out, ctx)`, so the operator and the operand were never in the host's hands.
 *       `lxb_selectors_predicate_t` and the record above are that, and they are the first member rather than
 *       the fourth.
 *       AND THE CONSUMER FOR (3) EXISTS ON ONE OF THE TWO CALLERS AND NOT THE OTHER, which is why the DOM half
 *       lands before the cascade half however the numbering reads: `core/dom/document.c`'s selector walk is
 *       ALREADY a step machine (an `IdlStepDecl`, stages and a `visit`), so it holds the resume point
 *       `engine_prepare_fork` requires and its `selector_match_node` call sits one line from a `JS_STEP_YIELD`;
 *       the cascade's walk is a plain C loop and `engine_prepare_fork` DFAILs by name for one, naming
 *       JS_CFUNC_STEP_DEF as the declaration to build. So (3) for `querySelectorAll` is a call to add and (3)
 *       for `getComputedStyle` is a step machine to declare first.
 *       RETIREMENT: this record goes when the ordered list itself is numbered in landing order, because the
 *       hazard is then a property of the numbering rather than of a paragraph a reader has to reach.
 *   (3) AND (4) TOGETHER — A CONSUMER THAT CAN ASK, KEYED ONE PER PREDICATE. LANDED FOR THE DOM HALF, at
 *       core/dom/document.c's selector walk, and the two are ONE landing because the paragraph above says so:
 *       a fork keyed coarser than the predicate merges two operands' questions, so there was never a (3) to
 *       land without (4). A fork needs a RESUME POINT and there is none inside a match — the arena note below
 *       says lxb_selectors_match_node "is a single C call that returns before the machine driving it can
 *       yield", and solver/engine.c's `engine_prepare_fork` aborts by name for a C body that forks from inside
 *       its own activation — so the ask is at the MACHINE, which re-runs the match once per answered predicate
 *       exactly as quickjs.c's `step_ownkeys_chain` re-runs its enumeration.
 *       THE COMPLETIONS ARE THE PREDICATE'S AND NOT THE SELECTOR'S, and that is the load-bearing choice rather
 *       than a spelling. A fork over "does this SELECTOR match" cannot be keyed soundly at all: the matcher
 *       abandons a compound at its first undetermined member, so a `matched` arm would be a claim the matcher
 *       never verified AND the flow would hold no constraint on the members it never reached — after which a
 *       second element sharing the recorded value replays that arm and its own undetermined member is decided
 *       by an answer that was never about it, which is §a-wrong-narrowing's deleted arm. Forking the PREDICATE
 *       has neither problem: the matcher composes it, and a later undetermined member declines in its own
 *       right and is forked in its own right.
 *       THE TRUE ARM IS REALISED WITHOUT A LEXBOR CHANGE and the FALSE ARM NEEDS NOTHING AT ALL, which is why
 *       this is a call to add rather than a seam to widen. TRUE hands the matcher the predicate's own operand
 *       (see sel_undet_satisfiable for the one operator that has no such realisation); FALSE is already what
 *       an undetermined read does in control flow, so the answer the machine already holds IS the answer under
 *       it. OUTCOME 0 IS THEREFORE "THE TEST DOES NOT HOLD", which is step_fork_run's one numbering rule read
 *       against this predicate: a run with no forking policy takes it and gets the byte-identical answer every
 *       build gave before this landed.
 *       WHAT IS NOT COVERED: ONE DECIDED PREDICATE PER MATCH. A compound both of whose members are undetermined
 *       and whose flow answers TRUE to BOTH needs two supplied at once, and `document.c`'s machine holds one
 *       slot and aborts by name for the second. The next diff is the slot becoming a SET the machine's `visit`
 *       carries. How its absence would show: that abort, on a selector with two undetermined tests in one
 *       compound, in a flow that took the holding arm of both.
 *       WHICH MACHINES THE CASCADE HALF NEEDS IS STILL THE PART TO DERIVE RATHER THAN ASSUME:
 *       `cssom_cascaded_value`'s callers are what reach it, and the ones a page drives (`getComputedStyle`, the
 *       §7 view members) are plain C bodies today, so each is a declaration to build and not a call to add.
 *   (5) THE PIN TAKEN *AT* THE MATCH, which is the direction (0) does NOT cover and is a different fact:
 *       (0) READS a determination the page's own predicate made, while this one MAKES one — `[att=dark]`
 *       answered TRUE by a fork of (3) pins the value, after which `[att=light]` is DECIDED by the flow's own
 *       constraint rather than forked again. Without it the worlds multiply with the number of operands the
 *       sheet tests, where the page can only be in one.
 * HOW ITS ABSENCE WOULD SHOW once (2)-(5) exist: a document whose only style difference between two flows is
 * an attribute this engine never observed would report one computed value where a browser has two.
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

    /* AND WHAT THIS FLOW HAS ALREADY PROVED ABOUT *THIS TEST* -- the answered fork, realised. §Solver-half's
       "uncertainty keeps the arm" is what the decline below is; this is the OTHER arm, where a consumer that
       kept it has come back with the answer. The arm says the test HOLDS, and the honest realisation of that
       is to hand the matcher the predicate's OWN OPERAND: it satisfies every operator §6 defines (see
       sel_undet_satisfiable, which is why an unsatisfiable one is never declared forkable), so the matcher
       decides this ONE test true and composes it through §6.6's `~=`, the compound's AND, the list's OR and
       any `:not()` exactly as it would have on real bytes.
       IT IS NOT A VALUE AND MUST NEVER BECOME ONE. The bytes decide THIS test and nothing else: a second test
       over the same attribute is a DIFFERENT predicate, does not match this record, and declines again -- so
       `[a^=dark][a$=mode]` forks twice and ends holding both facts, rather than having the first arm's bytes
       answer the second test. Nothing is PINNED, which is the difference between this and §Solver-half's
       concretize-on-pin: a pin is a claim about the VALUE, and `^=dark` does not determine one. The pin taken
       AT a match is a later diff and is named in the ordered list above.
       THE COMPARISON IS THE FULL KEY. The predicate's four fields AND the value's identity -- because two
       attributes of one element can hold two different unknowns under one predicate, and `concolic_ident_c` is
       what the solver's own key is composed from, so this compares the fact the solver filed and survives a
       park and a cross-session resume. An absent identity on either side is a value this engine cannot spell:
       it matches nothing here and was never declared forkable at the ask that produced it either, so there is
       no arm to realise and no route to a supply that names the wrong value. */
    if (g_decided != NULL && g_decided->undetermined && selector_pred_same(g_decided, &ask)) {
        const char *want = concolic_ident_c(g_decided->over), *have = concolic_ident_c(taint);

        /* BOTH ARE NON-NULL FOR ANY RECORD A CALLER COULD HAVE FORKED, because `forkable` required one at the
           ask -- so this is the assertion that a `decided` record came from THIS seam rather than a test. */
        DCHECK(want != NULL, "a decided §6 predicate was handed back naming a value this engine cannot spell — "
                             "the ask that produced it would not have been declared forkable, so this record "
                             "was composed somewhere other than sel_undet_compose");
        if (want != NULL && have != NULL && strcmp(want, have) == 0) {
            /* BORROWED FOR THE LENGTH OF THE MATCH, exactly as the pin's bytes are and for the same reason:
               the operand points into the compiled selector list, which nothing writes after it is built, and
               a match runs none of the page's code. */
            out->data = (lxb_char_t *)g_decided->operand;
            out->length = strlen(g_decided->operand);
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
    if (!list) return NULL;   /* Selectors 4 §17.1's `failure` */
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
                         lxb_css_selector_specificity_t *out_spec,
                         const SelectorUndet *decided, SelectorUndet *out_undet)
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
    /* CLEARED HERE AND NOT AT THE DECLINE, on the same span the arena clears its own bit on. lexbor sets
       `unknown` false at the entry of match_node and says why ("a field the caller is about to read may not be
       cleared under it"), so a record cleared anywhere else would answer about a PREVIOUS match for a match
       that declined on nothing. `over` is set to a REAL JSValue rather than left as the memset's zero bytes,
       because a tag of 0 is a tag and not `undefined` -- it is borrowed, so nothing is released. */
    memset(&g_undet, 0, sizeof g_undet);
    g_undet.over = JS_UNDEFINED;
    /* AND WHAT THE CALLER HAS ALREADY PROVED, live for exactly this match -- see g_decided. */
    g_decided = decided;
    /* lxb_selectors_match_node, not a subtree find: a combinator is resolved by walking UP from the candidate
       through the whole document, so §1.3's scoped matching still holds when the caller filters the results to
       a subtree — `el.querySelectorAll('div p')` finds a <p> under `el` whose <div> ancestor is OUTSIDE it. */
    lxb_selectors_match_node(g_arena, node, list, sel_hit_cb, &h);
    g_in_match = false;
    g_decided = NULL;

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
    /* THE QUESTION THIS SELECTOR ASKED AND NOBODY CAN ANSWER — §Offensive-programming's category (2), a
       capability that should exist and does not, and the crash that (1) MOVED here from the value seam.
       `g_arena->unknown` is Kleene's third value carried out of the match: some test in this selector was
       decided from an attribute whose value the host declined to state, and the conclusion "did not match"
       is therefore UNKNOWN rather than false.
       IT IS GATED ON `!h.matched`, WHICH IS KLEENE'S OR AND IS WHY THIS IS NOT THE OLD CRASH WITH A NEW
       ADDRESS. A selector list matches through whichever of its selectors did, so a definite match makes the
       list's answer TRUE whatever some other branch could not decide — `#known, [att=x]` on an element
       `#known` matches is answered, and the seam being asked along the way is not a question the engine
       failed. The old crash could not tell those apart because it fired before the list had one.
       AND WHAT IT IS STANDING IN FOR IS NOW A PROPERTY OF THE CALLER RATHER THAN OF THIS FILE, which is the
       argument this paragraph retires rather than deletes: it read "WHAT THE NEXT DIFF BUILDS is (2) in this
       file's ordered list, and this abort is standing in for its signature: the answer exists now and `bool`
       has nowhere to put it, so the cascade cannot yet REPORT an unanswered predicate and the process stops
       instead." The signature HAS somewhere to put it — `out_undet` — and `document.c`'s selector walk forks
       on it. A reader who re-derives the old reason from the `bool` in front of them will re-propose the
       out-parameter and find it built, which is the same wasted reading the note in the header was written to
       prevent one layer up. What remains true is the SECOND half: the CASCADE still cannot keep an arm,
       because keeping one needs a resume point and its walk is a plain C loop, so a third state at
       `cssom_cascaded_value` is still a write whose only reader would be the crash it replaces until that walk
       is a step machine. The condition below is what says which of the two a reader is standing at.
       THE VALUE IS NOT ASSERTED ON and no page string reaches this condition: `g_arena->unknown` is a bit
       THIS ENGINE set about its own capability, which is what a `DFAIL` is for.
       NAMED RESIDUAL — `out_spec` IS A LOWER BOUND WHEN A BRANCH WAS UNDETERMINED. What is not covered: a
       list reports the HIGHEST specificity that MATCHED, so a branch this match could not decide contributes
       none, and `div, #a[att=x]` on a div with that id weighs the rule at 0,0,1 where the undecided branch
       would have weighed it 1,1,0 — the rule APPLIES either way and CSS 2.1 §6.4 orders it wrongly against a
       competitor in between. What the next diff builds: (2)'s out-parameter carries the undetermined
       branches' specificities beside the answer, since a cascade that must fork on the predicate must fork on
       its weight too. How its absence would show: two rules whose winner changes with an attribute this
       engine never observed, reported with the loser's declaration and no question asked. */
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
                 ? "THIS CALLER PASSED NO `out_undet`, so it has not declared that it can keep the arm — and "
                   "keeping one needs a RESUME POINT. `document.c`'s selector walk is a step machine and forks "
                   "here; the cascade's is a plain C loop inside `cssom_cascaded_value`, which "
                   "solver/engine.c's `engine_prepare_fork` refuses by name. Declare that walk "
                   "JS_CFUNC_STEP_DEF and pass this record — see host_attr_value_read's ordered diffs."
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
