/* CSSOM §6.4.2 CSSRule and §6.4.3 CSSStyleRule — a rule in a style sheet.
 *
 * A RULE IS MADE OF TEXT, AND THAT IS THE CONSTRAINT THE WHOLE COMPONENT IS BUILT ON. Lexbor parses a
 * stylesheet into an ARENA and names every rule, selector and value by a pointer into it, and a pointer has no
 * cross-tier identity: a sheet holding one could neither park to the IDB cold tier nor fork per flow, and
 * `insertRule`/`deleteRule` are exactly the mutations two flows must be able to disagree about. So the parse's
 * output crosses out of the arena as SERIALIZED TEXT (core/css/css_style_declaration.h's `cssom_parse_rules`),
 * and a rule holds JS strings that the COW delta captures, the snapshot carries and the cold tier writes.
 *
 * WHAT A RULE HOLDS AND WHAT READS IT. The selector text is read by §6.4.3's `selectorText`, which is also
 * SETTABLE — one of the two reasons the record time-travels. The declaration block text is the rule's BODY, and
 * §6.4.3's `style` is the CSS DECLARATION BLOCK over it: core/css/css_style_declaration.h owns §6.6 and reads
 * and writes the text through the two entries below, so the rule's declarations have ONE storage rather than a
 * copy in each component that could disagree. That storage is the other reason: `rule.style.color = 'red'` is
 * precisely a mutation two flows must be able to disagree about, and it lands in a C record behind a class
 * opaque where no property hook can see it.
 * §6.4.2's `cssText` — the whole rule, selector and body — is the member still absent, because it needs §6.6's
 * serialize-a-CSS-declaration-block INCLUDING its shorthand consolidation loop, and the IDL audit reports it,
 * which is the ledger.
 *
 * `CSSStyleRule : CSSGroupingRule` IN THE IDL, and this prototype chains to CSSRule.prototype instead, because
 * CSSGroupingRule is not built — it is nested rules, with its own `cssRules`/`insertRule`/`deleteRule`. The
 * install asserts against its arrival rather than leaving the chain one link short in silence. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_RULE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_RULE_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"

void css_rule_init(JSContext *ctx);
/* §6.4.2's and §6.4.3's prototypes for ONE realm — declared into core/realm.h's list. */
void css_rule_install_proto(JSContext *ctx);
/* `CSSRule` and `CSSStyleRule` as globals. */
void css_rule_install(JSContext *ctx, JSValueConst global);
void css_rule_free(JSContext *ctx);

/* A §6.4.3 CSSStyleRule over the two texts a parse produced for it, and the sheet it belongs to. `parent_rule`
   is §6.4.2's parent CSS rule — JS_NULL for a top-level rule, and non-null only once CSSGroupingRule nests one.
   OWNED: the caller frees. */
JSValue css_style_rule_new(JSContext *ctx, JSValueConst parent_style_sheet, JSValueConst parent_rule,
                           const char *selector_text, const char *block_text);

/* Is `v` a CSSRule? The class brand, for a caller holding something it took out of a rule list. */
bool css_rule_is(JSValueConst v);

/* THE RULE'S DECLARATIONS, as the text they are stored as — §6.6's declaration block reads them through here.
   OWNED: the caller frees. NULL, with `*plen` zero, for a rule whose body declares nothing. */
char *css_rule_block_text(JSContext *ctx, JSValueConst rule, size_t *plen);

/* Replace them. The write goes through the record's capturing accessor, so it rides the running flow's COW
   delta exactly as `selectorText`'s does. */
void css_rule_set_block_text(JSContext *ctx, JSValueConst rule, const char *text, size_t len);

/* §6.4's "remove a CSS rule" last step — "set old rule's parent CSS rule and parent CSS style sheet to null".
   The rule object survives a page's reference to it and must stop naming the sheet it has left. */
void css_rule_orphan(JSContext *ctx, JSValueConst rule);

#endif
