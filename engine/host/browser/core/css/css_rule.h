/* CSSOM §6.4's CSS RULES — §6.4.2 CSSRule, §6.4.5 CSSGroupingRule, §6.4.3 CSSStyleRule, §6.4.4 CSSImportRule
 * and §6.4.9 CSSNamespaceRule, plus CSS Conditional §7.2's CSSConditionRule and §7.3's CSSMediaRule (the
 * `@media` half of the same object) and CSS Fonts §12.1's CSSFontFaceRule.
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

void css_rule_init(JSContext *ctx);
/* Every §6.4 and §7.x rule prototype for ONE realm — declared into core/realm.h's list. */
void css_rule_install_proto(JSContext *ctx);
/* `CSSRule`, `CSSGroupingRule`, `CSSStyleRule`, `CSSConditionRule`, `CSSMediaRule`, `CSSImportRule`,
   `CSSNamespaceRule` and `CSSFontFaceRule` as globals. */
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
   the conditional group rules whose condition holds, each serialized as `selector{block}` for the SELECTOR
   MATCHER to re-parse — a parsed selector being the one thing the rule objects cannot carry — preceded by the
   sheet's `@namespace` rules, which go in verbatim because the selectors are written against the prefixes they
   bind. `*pn` is how many rules were emitted, which is what the caller's round-trip assertion compares the
   re-parse against.
   OWNED. NULL when a stored text could not be read back, which leaves a pending exception on `ctx`. */
char *css_rule_cascade_text(JSContext *ctx, JSValueConst list, uint32_t *pn);

#endif
