/* CSSOM §6.4's CSS RULES — §6.4.2 CSSRule, §6.4.5 CSSGroupingRule, §6.4.3 CSSStyleRule, §6.4.4 CSSImportRule,
 * §6.4.7 CSSPageRule, §6.4.8 CSSMarginRule and §6.4.9 CSSNamespaceRule, plus CSS Conditional 3 §7.2's
 * CSSConditionRule, §7.3's CSSMediaRule and §7.4's CSSSupportsRule (the `@media` and `@supports` halves of the
 * same object), CSS Fonts 5 §9.1's CSSFontFaceRule, CSS Animations §6.2/§6.3's CSSKeyframeRule and
 * CSSKeyframesRule, CSS Cascade §8.1/§8.2's CSSLayerBlockRule and CSSLayerStatementRule, and CSS Properties and
 * Values API 1 §6.1's CSSPropertyRule.
 *
 * A CSSPropertyRule HAS NO `style`, AND THAT IS THE ONE STRUCTURAL THING TO KNOW ABOUT IT. §6.1's IDL is
 * `interface CSSPropertyRule : CSSRule` with four readonly attributes — `name`, `syntax`, `inherits`,
 * `initialValue` — and nothing else, even though §3 gives the at-rule a `<declaration-list>` body. So its
 * descriptors are read ONCE at the parse into three fields the three attributes answer from, rather than kept
 * as this rule's declaration block: no member could read a block back, §6.1's serialization emits the three in
 * the SECTION'S order rather than the author's, and each descriptor has a grammar and an INITIAL of its own
 * that a declaration block knows nothing about. Its `type` is 0, for the reason the two CSSLayer* rules' is.
 * ITS `name` HAS AN UNRESOLVED CASE AND THE ASSERT IS WHERE THAT IS STATED. §3's prelude is
 * `<custom-property-name>#` and §3 makes a multi-name rule VALID ("a valid @property rule represents a custom
 * property registration for each <custom-property-name> in the rule's prelude"), while §6.1 declares one `name`
 * and carries the CSSWG's own note that the CSSOM for that shape "has not been resolved on". So the rule parses,
 * the list is stored, and the ONE reader that both `name` and the serialization go through is where the shape
 * with no defined answer is refused rather than answered with an invented one.
 *
 * `CSSImportRule.styleSheet` IS ABSENT, AND IT IS THE ONE MEMBER OF THESE INTERFACES THAT IS. §6.4.4 defines it
 * as "the associated CSS style sheet, if any, or null otherwise" and its own note gives the case that produces
 * null (an import whose `supports()` condition does not match). This engine loads no CSS subresource at all —
 * there is no `@import` fetch and no `<link rel=stylesheet>` sheet either, so `css_style_sheet_create` has
 * exactly one caller and it is `<style>` — which means a getter here could only ever answer null, and that null
 * is indistinguishable from the spec's real one. That is the shape §NO STUBS forbids: not a missing answer but
 * a WRONG one wearing a right one's clothes, invisible to the page and to the next reader. So the member is
 * honestly missing, the page's own TypeError names it, and the IDL gap audit reports it. THE CAPABILITY TO
 * BUILD IS THE SHEET FETCH: §6.3's "obtain a CSS style sheet" over the import's URL, resolved against the
 * importing sheet's base URL, creating a child sheet whose parent CSS style sheet is the importer and whose
 * owner CSS rule is the import rule — which is also what fills the `ownerRule` css_style_sheet.c already reads
 * off its record for exactly this day.
 *
 * A RULE IS MADE OF TEXT, AND THAT IS THE CONSTRAINT THE WHOLE COMPONENT IS BUILT ON. Lexbor parses a
 * stylesheet into an ARENA and names every rule, selector and value by a pointer into it, and a pointer has no
 * cross-tier identity: a sheet holding one could neither park to the IDB cold tier nor fork per flow, and
 * `insertRule`/`deleteRule` are exactly the mutations two flows must be able to disagree about. So the parse's
 * output crosses out of the arena as SERIALIZED TEXT (core/css/css_style_declaration.h's `cssom_parse_rules`),
 * and a rule holds JS strings that the COW delta captures, the snapshot carries and the cold tier writes.
 *
 * A GROUPING RULE'S CHILD LIST IS A JS ARRAY, for that same reason and for one more. §6.4 calls it the rule's
 * "child CSS rules ... the list can be mutated"; §6.4.5's `cssRules` is a `[SameObject]` CSSRuleList over it,
 * and §6.4.5 gives the rule an `insertRule` and a `deleteRule` of its own — so it is mutable cross-flow state
 * that has to park and fork, which an Array is and a `lxb_css_rule_list_t *` is not. The CSSRuleList SHARES that
 * very Array rather than copying it, because §6.1.2's liveness note requires the collection to track the list,
 * and sharing is also the only way `groupingRule.cssRules[0]` after an `insertRule` is the rule that was
 * inserted. It is the identical decision css_style_sheet.h records for a sheet's rules, and it is identical on
 * purpose: §6.4's insert and remove algorithms are stated ONCE over "a CSS rule list", so there must be one kind
 * of thing for them to be stated over.
 *
 * ONE CLASS, ONE RECORD, MANY PROTOTYPES. §6.4's `type` is a STATE ITEM — "initialized when a rule is created
 * and cannot change" — and it is also WHICH INTERFACE the rule is, so those are one field rather than two facts
 * that could disagree. It is what §6.4 steps 5 and 6 ask about a rule (an `@import` may not follow a style rule
 * and an `@namespace` may not join a sheet that holds one), what serialize-a-CSS-rule branches on, and what
 * §3.7.6 Attributes' and §3.7.7 Operations' brand check on a derived interface's member tests. It used to be
 * answered from the CLASS, on the stated ground that this build had exactly one rule interface; that stopped
 * being true with §7.3. The
 * alternative — a class per interface — would have
 * put §6.4.2's four members (which run with `this` being ANY rule, whatever its interface) behind a lookup that
 * tries each class in turn, or copied them onto every concrete prototype, which is the hand-maintained list
 * core/realm.h exists to abolish. So the BRAND is the one class, the INTERFACE is the stored type, and each
 * interface prototype is the realm's own.
 *
 * A PAGE RULE IS TWO GRAMMARS THIS FILE DOES NOT OWN. CSS Paged Media §4.3's `<page-selector-list>` is parsed
 * and canonicalised by core/css/css_at_rule_prelude.h — CSSOM §6.4.7 names "parse a list of CSS page
 * selectors" and "serialize a list of CSS page selectors" and then declines to define either, so the grammar
 * is the one place they can come from — and §4.3's restriction on which declarations a page context and a
 * margin context hold is core/css/css_page.h's, applied by core/css/css_style_declaration.h before the block
 * text is ever stored. Both are asked at the two moments a page rule's text is WRITTEN (the parse, and every
 * §6.6.1 write through `style`), which is what makes `selectorText`, `cssText`, `length` and
 * `getPropertyValue` agree without any of them asking for itself.
 *
 * ONE `@layer` KEYWORD IS TWO INTERFACES, AND THE BLOCK IS WHAT DECIDES WHICH. CSS Cascade §6.4.4 gives the
 * at-rule two grammars: §6.4.4.1's `@layer <layer-name>? { <rule-list> }` is §8.1's CSSLayerBlockRule and
 * §6.4.4.2's `@layer <layer-name>#;` is §8.2's CSSLayerStatementRule, so the builder's `has_block` fork — the
 * one `@import` and `@font-face` already take to tell a rule from a drop — here picks between two interfaces
 * instead. They differ in EVERYTHING the interfaces differ in: a block is a §6.4.5 grouping rule holding the
 * layer's rules ("such @layer block rules have the same restrictions and processing as a conditional group
 * rule [CSS-CONDITIONAL-3] with a true condition"), a statement contains nothing at all; a block declares AT
 * MOST ONE name and a
 * statement ONE OR MORE; and they sit in different places in a sheet — §6.4.4.2 admits the statement before
 * `@import` and `@namespace` as well as wherever any rule may go, which is the ONE rule type with two
 * admissible positions and the reason css_rule.c states a sheet's prologue as a set of ZONES rather than as a
 * rank per type. What they SHARE is the `<layer-name>` grammar, so they share one storage: §8.2 requires
 * `nameList` "normalized following the same rule as the CSSLayerBlockRule's name attribute", and §8.1's `name`
 * is that list read at index 0 — the empty string when there is nothing there, which is §6.4.2.1's anonymous
 * layer.
 * NEITHER HAS A `type` NUMBER, and that is §6.4.2 speaking rather than a gap: its table ends "otherwise, return
 * 0" with the note that "this enumeration is thus frozen in its current state, and no new values will be added
 * to reflect additional at-rules". So the stored discriminator continues past the table and `rule_legacy_type`
 * maps it back — the same shape as the split below, and for the same reason.
 *
 * A `@keyframes` HOLDS RULES AND IS NOT A §6.4.5 GROUPING RULE, so those are two questions here and not one.
 * CSS Animations §6.3.1 declares `interface CSSKeyframesRule : CSSRule` and then gives it a `cssRules` of its
 * own, an `appendRule(CSSOMString)`, a `deleteRule(CSSOMString)` and a `findRule(CSSOMString)` — a
 * `deleteRule` whose argument is a keyframe SELECTOR where §6.4.5's is an INDEX, which is exactly why the IDL
 * keeps the two interfaces apart and why one predicate could not serve both. `rule_type_has_child_rules`
 * decides storage (an Array, for the reason above) and what the parse may nest; `rule_type_is_grouping` is
 * the brand for the three members CSSGroupingRule declares — §3.7.6 Attributes' for `cssRules`, §3.7.7
 * Operations' for the other two — and it asserts it is the narrower of the two.
 * §6.3.3's INDEXED PROPERTY GETTER is not a member at all — Web IDL §3.9 makes it the object's own-property
 * behaviour — so it is an EXOTIC on this component's class, running core/idl_indexed.h's one algorithm over a
 * decl this file hands out for that one rule type. A collection object could not have carried it: a
 * CSSKeyframesRule is a rule, its record lives behind this class's opaque, and one object has one class.
 *
 * A PREFIXED AT-KEYWORD IS A SECOND SPELLING OF ONE RULE, RESOLVED IN FRONT OF THE BUILDER'S DISPATCH — never
 * an arm of its own. CSS Compatibility Standard §3.1 "CSS At-rules" requires "-webkit- vendor prefixed
 * at-rules ... as aliases of the corresponding unprefixed at-rules" over a table with one row today
 * (`@-webkit-keyframes` onto `@keyframes`), so the alias names the AT-KEYWORD and nothing else: one interface,
 * one prototype, one §6.4.2 `type`, one prelude grammar, one body. A second arm would be a second creator able
 * to disagree with the first about any of those and no reader would see it. What the two spellings DO NOT
 * share is the at-keyword §6.4's serialize-a-CSS-rule emits — which is why the rule stores the one it was
 * written with, and why that half is settled by measuring a real browser rather than by reading CSSOM's arm,
 * which names the literal `"@keyframes "` and predates §3.1 entirely.
 * AND THE REST OF THE PREFIXED CLASS IS NOT AN ABSENT CAPABILITY, WHICH IS WHY IT DROPS RATHER THAN CRASHING.
 * CSS 2.1 §4.1.2.1 "Vendor-specific extensions" reserves an initial `-` or `_` for one vendor's own
 * extensions and guarantees CSS itself will never use one, and §4.2 "Rules for handling parsing errors" then
 * ignores the at-rule; so `@-moz-keyframes` and `@-ms-viewport` name a user agent this is not, and there is no
 * interface for them to be missing. An UNPREFIXED at-keyword is the opposite case by the same two sections —
 * §4.2 reserves it to CSS — so one this build has no arm for is a real interface that is missing here, and it
 * keeps crashing by name.
 *
 * WHAT A RULE HOLDS AND WHAT READS IT. The selector text is read by §6.4.3's `selectorText`, which is also
 * SETTABLE — one of the reasons the record time-travels. The declaration block text is a style rule's BODY, and
 * §6.4.3's `style` is the CSS DECLARATION BLOCK over it: core/css/css_style_declaration.h owns §6.6 and reads
 * and writes the text through the two entries below, so the rule's declarations have ONE storage rather than a
 * copy in each component that could disagree. That storage is the other reason: `rule.style.color = 'red'` is
 * precisely a mutation two flows must be able to disagree about, and it lands in a C record behind a class
 * opaque where no property hook can see it. A conditional rule's `media` is a §4.4 MediaList, whose own
 * collection is an Array for the reasons above again.
 * §6.4.2's `cssText` is §6.4's SERIALIZE A CSS RULE over all of it: for a style rule the stored selector list,
 * then §6.6's serialize-a-CSS-declaration-block over the stored body (which is where the shorthand consolidation
 * loop runs), then its nested rules; for a media rule `@media`, the media query list and its nested rules. Its
 * setter is the spec's own no-effect ("on setting the cssText attribute must do nothing"), not an unbuilt one. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_RULE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_RULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_index_arg.h"
#include "core/css/css_layer_order.h"

void css_rule_init(JSContext *ctx);
/* Every §6.4 and §7.x rule prototype for ONE realm — declared into core/realm.h's list. */
void css_rule_install_proto(JSContext *ctx);
/* `CSSRule`, `CSSGroupingRule`, `CSSStyleRule`, `CSSConditionRule`, `CSSMediaRule`, `CSSSupportsRule`,
   `CSSImportRule`, `CSSNamespaceRule`, `CSSFontFaceRule`, `CSSPageRule`, `CSSMarginRule`, `CSSKeyframeRule`,
   `CSSKeyframesRule`, `CSSLayerBlockRule`, `CSSLayerStatementRule` and `CSSPropertyRule` as globals. */
void css_rule_install(JSContext *ctx, JSValueConst global);
void css_rule_free(JSRuntime *rt);

/* Is `v` a CSSRule? The class brand, for a caller holding something it took out of a rule list. */
bool css_rule_is(JSValueConst v);

/* THE RULE'S DECLARATIONS, as the text they are stored as — §6.6's declaration block reads them through here,
   and so does the CASCADE, which resolves the author layer from these objects rather than from the `<style>`
   element's bytes. OWNED: the caller frees. NULL, with `*plen` zero, for a rule whose body declares nothing and
   for every rule type that has no declaration block at all. */
char *css_rule_block_text(JSContext *ctx, JSValueConst rule, size_t *plen);

/* Replace them. The write goes through the record's capturing accessor, so it rides the running flow's COW
   delta exactly as `selectorText`'s does. */
void css_rule_set_block_text(JSContext *ctx, JSValueConst rule, const char *text, size_t len);

/* §6.4's "INSERT A CSS RULE rule in a CSS rule list list at index index, with a flag nested", and "REMOVE A CSS
 * RULE from a CSS rule list list at index index", ENTIRE — the two algorithms §6.1.2's `insertRule`/`deleteRule`
 * and §6.4.5's are EACH stated over, so they are one implementation reached from two declarations rather than
 * two that can drift.
 *
 * `list` is the Array the rules live in, `parent_sheet` the CSS style sheet every rule in it names, and
 * `parent_rule` the enclosing rule (JS_NULL at a sheet's top level). `nested` is the spec's own flag, and it is
 * what decides step 6: an `@import` or an `@namespace` cannot go inside a conditional group rule AT ALL, while
 * at a SHEET's top level step 6 is the RANK ORDER CSS Cascade §2 and CSS Namespaces §2 state (imports, then
 * namespaces, then everything else), asked in BOTH directions — a style rule inserted before an existing
 * `@import` is refused by the same test that refuses an `@import` inserted after one.
 * `css_rule_list_insert` answers §6.4 step 9's index, or an exception with the right DOMException pending. */
JSValue css_rule_list_insert(JSContext *ctx, JSValueConst list, JSValueConst parent_sheet,
                             JSValueConst parent_rule, uint32_t index, const char *text, bool nested);
JSValue css_rule_list_delete(JSContext *ctx, JSValueConst list, uint32_t index);

/* §6.4's "REMOVE A CSS RULE" STEPS 2 AND 3 OVER AN UNKNOWN INDEX — the one thing either `deleteRule` needs
 * that neither of their files can state, because the algorithm the steps belong to lives here and both members
 * are declared over it.
 *
 * BOTH `deleteRule`s TAKE A `unsigned long index` THAT UNKNOWN EXTERNAL INPUT REACHES AS ITSELF. Web IDL
 * §3.2's conversion is a boundary an unknown CROSSES (core/idl_args.h's `idl_number_of` states the rule and
 * names the shape that breaks it: A BODY MAY NOT CALL JS_ToFloat64 ON ITS OWN ARGUMENT), so
 * `sheet.deleteRule(location.hash.length - 1)` arrives holding the unknown, and a body owing C a `uint32_t`
 * for it has no number to give.
 *
 * IT IS ONE QUESTION. §6.4's remove-a-CSS-rule step 2 is "If index is greater than or equal to length, then
 * throw an "IndexSizeError" exception", and step 3 is "Set old rule to the indexth item in list" — so the
 * algorithm needs the comparison AND the position, and answering only the first would leave step 3 holding an
 * unknown again. §3.2.4.6 unsigned long's ConvertToInt(V, 32, "unsigned") is TOTAL over [0, 2**32-1], which
 * makes the two questions one: `index >= length` is exactly "index is none of 0 … length-1", because the type
 * admits no value below 0.
 *
 * THE DECOMPOSITION IS core/idl_index_arg.h's ELIMINATION CHAIN AND IS NO LONGER WRITTEN HERE. This file held
 * the loop first; the `item(index)` family then turned out to ask the identical question of the identical
 * type, so the chain — its cursor, its operation string, its one reading of the example, its arm numbering and
 * its named residual — became the component both use, and the state below is that component's type. What
 * stays §6.4's is the two facts the component takes as parameters and the one it refuses to decide: how many
 * positions the algorithm admits, what to call the question, and what the past-the-end world IS. Here that
 * world is step 2's IndexSizeError; for every `item(index)` it is an ordinary null.
 *
 * `c` is the machine's own state and MUST live on it rather than in a C local: `next` is the cursor a park
 * resumes on, and `op` is read by the DRIVER after the chain has returned, so a stack buffer would dangle
 * exactly where the constraint key is built.
 *
 * Returns >0 (the caller returns it — the flow is parked at the fork), 0 with `*pindex` this world's position,
 * or JS_STEP_ABRUPT with step 2's IndexSizeError live. `index_v` must be unknown external input; the known
 * value is `idl_index_arg_known`'s and never comes here. */
#define CSS_RULE_REMOVE_INDEX_ALGORITHM "CSSOM §6.4 CSS Rules remove a CSS rule steps 2-3"

int css_rule_delete_index_run(JSContext *ctx, JSStepHdr *hdr, IdlIndexChain *c,
                              JSValueConst index_v, JSValueConst list, uint32_t *pindex);

/* THE SAME `unsigned long index`, READ BY THE OTHER TWO MEMBERS — §6.1.2's and §6.4.5's `insertRule`, which are
 * declared over §6.4's INSERT a CSS rule. The KNOWN value is answered here, by the one copy of the arithmetic;
 * the UNKNOWN is a fork these two bodies cannot yet perform, and it ABORTS naming what to build rather than
 * coercing.
 *
 * THEY ARE NOT MACHINES YET AND THE REASON IS THE STEP THE INDEX SHARES ITS BODY WITH. §6.4's insert-a-CSS-rule
 * step 2 is "If index is greater than length, then throw an "IndexSizeError" exception" — `>` and not `>=`,
 * because appending at the very end is legal — so its chain runs over 0 … length, ONE position longer than
 * remove's; that part is a parameter and not an obstacle. What is an obstacle is that these bodies hold a
 * SECOND unknown: `CSSOMString rule` reaches the same body and `JS_ToCString` on unknown external input is the
 * string half of exactly this defect, which is a different question with a different reader
 * (core/idl_args.h's `concolic_name_cstr`). Converting the index here and not the text would leave a machine
 * that parks on one argument and aborts on the other one line later — so the two are converted together, in
 * the diff that answers the string, and until then this states the position honestly.
 *
 * WHAT THE NEXT DIFF BUILDS: these two bodies become IdlStepBody machines exactly as the two `deleteRule`s
 * now are, and §6.1.2's own steps 3-5 (parse a rule, then its two SyntaxError arms) become the stages that
 * precede it — which is the ordering this file does not have today either, since `css_rule_list_insert` parses
 * AFTER its step 2 and §6.1.2 parses BEFORE reaching it. The `>` / `>=` difference this clause used to name as
 * work is NOT work any more: core/idl_index_arg.h's chain takes the count of positions the algorithm admits as
 * a parameter, so insert's step 2 passes `length + 1` where remove's passes `length` and nothing else differs.
 * HOW ITS ABSENCE SHOWS: `sheet.insertRule(text, location.hash.length)` aborts in a dev build naming this
 * macro, where the two `deleteRule`s beside it fork; and a page that reaches an `insertRule` behind an unknown
 * index contributes no rule-list world to the frontier at all.
 *
 * A MACRO AND NOT A HELPER, because a should-never-happen stamps the line it is WRITTEN at: one function shared
 * by two members in two files would report its own line for both. `member` is what lets the abort say which. */
#define CSS_RULE_INSERT_INDEX(ctx_, dst_, arg_, member_)                                                      \
    do {                                                                                                      \
        JSValueConst cri_v_ = (arg_);                                                                         \
        double cri_n_ = 0;                                                                                    \
                                                                                                              \
        if (concolic_is(cri_v_)) {                                                                            \
            DFAIL("CSSOM " member_ " was given an UNKNOWN `index`. §6.4 CSS Rules' insert a CSS rule step 2 "  \
                  "is: If index is greater than length, then throw an \"IndexSizeError\" exception. That is a "\
                  "comparison over this value, and Web IDL §3.2.4.6 unsigned long's ConvertToInt(V, 32, "      \
                  "\"unsigned\") is total over [0, 2**32-1], so BOTH completions are feasible and neither arm "\
                  "may be chosen. Deciding it from the unknown's own example would collapse a modelable value "\
                  "to bare-concrete and delete the other arm. BUILD THE FORK: make this body an IdlStepBody "  \
                  "(core/idl_args.h, IDL_STEP_FIRST) so it can park, then ask step 2 through the elimination "  \
                  "chain core/idl_index_arg.h holds, passing length + 1 positions where remove-a-CSS-rule "    \
                  "passes length — see this macro's own comment in "                                           \
                  "core/css/css_rule.h for why the string argument beside this one is converted in the same "  \
                  "diff and not after it");                                                                   \
            /* THE MACRO RETURNS, and both bodies that expand it return JSValue. It has to: DFAIL is          \
               `((void)0)` in a release build, so without this the branch would fall through with `dst_` at   \
               its initializer and insert at position 0 — a plausible datum for a call whose index nobody     \
               knows, and one that MUTATES the sheet. Throwing is what the coercion this replaced already did \
               in release at the same boundary. */                                                            \
            return JS_ThrowTypeError((ctx_),                                                                  \
                                     "`insertRule` was given an unknown `index`, and CSSOM §6.4's insert a "   \
                                     "CSS rule step 2 over it is not modelled yet");                          \
        } else {                                                                                              \
            int cri_have_ = idl_number_of((ctx_), IDL_UNSIGNED_LONG, cri_v_, &cri_n_);                        \
                                                                                                              \
            DCHECK(cri_have_ == 1,                                                                            \
                   "idl_number_of found no number for the `index` of " member_ ", which is not unknown "       \
                   "external input — it answers 0 only for an unknown carrying no example, and that arm "     \
                   "returned above");                                                                         \
            (dst_) = (uint32_t)cri_n_;                                                                        \
        }                                                                                                     \
    } while (0)

/* CSS Syntax's PARSE A STYLESHEET'S CONTENTS, as §6.4 rule OBJECTS appended to `list` — what HTML §4.2.6's
   sheet creation runs. `parent_sheet` is the sheet every rule in it names. */
void css_rule_build_sheet(JSContext *ctx, JSValueConst list, JSValueConst parent_sheet,
                          const char *text, size_t len);

/* THE AUTHOR CASCADE'S VIEW of a rule list: the STYLE rules that apply in `ctx`'s environment, flattened out of
 * the conditional group rules whose condition holds, each serialized as `selector{block}` for the SELECTOR
 * MATCHER to re-parse — a parsed selector being the one thing the rule objects cannot carry — preceded by the
 * sheet's `@namespace` rules, which go in verbatim because the selectors are written against the prefixes they
 * bind.
 *
 * A NESTED STYLE RULE IS RESOLVED ON THE WAY OUT, NEVER FLATTENED. CSS Nesting §3 "Nesting Style Rules" makes
 * a nested rule's prelude a `<relative-selector-list>` relative to its parent's, so lifting its stored text to
 * the sheet's top level would style a subtree the page never selected. §4 "Nesting Selector: the & selector"
 * gives the resolution — the nesting selector "replaced ... with the parent style rule's selector, wrapped in
 * an :is() selector" — and the `:is()` is load-bearing rather than cosmetic: Selectors 4 §15 "Calculating a
 * selector's specificity" gives it the specificity of its most specific argument, which IS §4's specificity
 * rule for `&`, so a concatenation would match identically and CASCADE differently. The walk carries the
 * PARENT'S EMITTED selector down, so depth needs no special case: `.a { .b { .c { } } }` comes out as three
 * rules ending in `:is(:is(.a) .b) .c`. §3.4 "Mixing Nesting Rules and Declarations" fixes their order —
 * "nested style rules and nested group rules are considered to come after their parent rule" — which is the
 * emission's own order, and CSS Cascade §6.1's Order of Appearance is that position.
 * A RESOLVED SELECTOR IS PARSED BEFORE IT IS EMITTED and what goes in is the parse's own serialization, which
 * is what discharges §3.1 "Syntax"'s "An invalid nested style rule is ignored, along with its contents" and
 * what keeps the per-index correspondence below true by construction rather than by hope.
 *
 * IT IS TEXT AND A LAYER PER RULE, NOT TEXT ALONE, because CSS Cascade §6.1 puts Layers ABOVE Specificity and
 * a flat text cannot say which layer a rule was in. Flattening `@layer a { #x { color: red } }` beside
 * `p { color: blue }` produces a sheet that resolves to RED on a `<p id=x>` where the standard resolves BLUE,
 * and the wrongness is invisible because both are real values. So the walk carries §6.4.3's layer as it
 * descends — declaring every layer it meets into `order`, in document order, which is what makes
 * first-declaration order the walk's own order — and reports the node each emitted rule belongs to.
 *
 * `layer[i]` PAIRS WITH THE i-TH RULE THE TEXT PARSES BACK TO, and NULL is a positive statement: the emitted
 * rule at that index is not a style rule (an `@namespace`, which is emitted for its prefix bindings and matches
 * no element). A caller reads it only for a rule it is about to match, and asserting the two agree at every
 * index is a stronger round-trip check than comparing the totals — a rule that re-parsed as two while its
 * neighbour re-parsed as none keeps the total right and shifts every layer after it.
 *
 * False when the sheet cannot be resolved at all: a stored text that could not be read back, which leaves a
 * pending exception on `ctx`. Nothing is allocated in that case. */
typedef struct {
    char                *text;    /* the flattened sheet. OWNED. NULL when the sheet emitted nothing. */
    const CssLayerNode **layer;   /* one entry per emitted rule, in emission order. OWNED. */
    uint32_t             n;       /* how many rules were emitted, and how long `layer` is */
} CssRuleCascadeSheet;

bool css_rule_cascade_sheet(JSContext *ctx, JSValueConst list, CssLayerOrder *order,
                            CssRuleCascadeSheet *out);
void css_rule_cascade_sheet_free(CssRuleCascadeSheet *s);

#endif
