/* CSSOM §6.4's CSS RULES — §6.4.2 CSSRule, §6.4.5 CSSGroupingRule, §6.4.3 CSSStyleRule, §6.4.4 CSSImportRule,
 * §6.4.7 CSSPageRule, §6.4.8 CSSMarginRule and §6.4.9 CSSNamespaceRule, plus CSS Conditional §7.2's
 * CSSConditionRule, §7.3's CSSMediaRule and §7.4's CSSSupportsRule (the `@media` and `@supports` halves of the
 * same object), CSS Fonts §12.1's CSSFontFaceRule, CSS Animations §6.2/§6.3's CSSKeyframeRule and
 * CSSKeyframesRule, and CSS Cascade §8.1/§8.2's CSSLayerBlockRule and CSSLayerStatementRule.
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
 * §3.7.5's brand check on a derived interface's member tests. It used to be answered from the CLASS, on the
 * stated ground that this build had exactly one rule interface; that stopped being true with §7.3. The
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
 * layer's rules ("such @layer block rules have the same restrictions and processing as a conditional group rule
 * with a true condition"), a statement contains nothing at all; a block declares AT MOST ONE name and a
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
 * §3.7.5's brand for the three members CSSGroupingRule declares, and it asserts it is the narrower of the two.
 * §6.3.3's INDEXED PROPERTY GETTER is not a member at all — Web IDL §3.9 makes it the object's own-property
 * behaviour — so it is an EXOTIC on this component's class, running core/idl_indexed.h's one algorithm over a
 * decl this file hands out for that one rule type. A collection object could not have carried it: a
 * CSSKeyframesRule is a rule, its record lives behind this class's opaque, and one object has one class.
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
#include "core/css/css_layer_order.h"

void css_rule_init(JSContext *ctx);
/* Every §6.4 and §7.x rule prototype for ONE realm — declared into core/realm.h's list. */
void css_rule_install_proto(JSContext *ctx);
/* `CSSRule`, `CSSGroupingRule`, `CSSStyleRule`, `CSSConditionRule`, `CSSMediaRule`, `CSSSupportsRule`,
   `CSSImportRule`, `CSSNamespaceRule`, `CSSFontFaceRule`, `CSSPageRule`, `CSSMarginRule`, `CSSKeyframeRule`,
   `CSSKeyframesRule`, `CSSLayerBlockRule` and `CSSLayerStatementRule` as globals. */
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
 * what decides step 5: an `@import` or an `@namespace` cannot go inside a conditional group rule AT ALL, while
 * at a SHEET's top level step 5 is the RANK ORDER CSS Cascade §2 and CSS Namespaces §2 state (imports, then
 * namespaces, then everything else), asked in BOTH directions — a style rule inserted before an existing
 * `@import` is refused by the same test that refuses an `@import` inserted after one.
 * `css_rule_list_insert` answers §6.4 step 8's index, or an exception with the right DOMException pending. */
JSValue css_rule_list_insert(JSContext *ctx, JSValueConst list, JSValueConst parent_sheet,
                             JSValueConst parent_rule, uint32_t index, const char *text, bool nested);
JSValue css_rule_list_delete(JSContext *ctx, JSValueConst list, uint32_t index);

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
