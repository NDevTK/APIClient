/* CSSOM §6.6 — the CSS DECLARATION BLOCK, its two interfaces (§6.6.1's CSSStyleDeclaration and the
   CSSStyleProperties that carries the per-property attributes), and the cascade every block's computed view
   reads through. A block is made by one of three creators and they differ only in §6.6's associated properties:
   §7.1's `element.style`, §7.2's `getComputedStyle` and §6.4.3's `rule.style`. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_DECLARATION_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_DECLARATION_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"

void cssom_init(JSContext *ctx);
/* THE CASCADE'S WINNER for `name` on `el` — inline, then this flow's author rules, then css-cascade-5 §6.5's
   author presentational hint origin (core/css/css_presentational_hints.h), then the UA sheet, then the
   property's initial value — as text. It is the SPECIFIED value: the declaration that won, before the
   property's own `Computed value:` line has been applied to it, which is core/css/css_computed_value.h's job
   and is who this exists for. OWNED: the caller frees. NULL only for a property no layer declares and that has
   no initial value in lexbor's registry (a custom property nobody set). */
char *cssom_cascaded_value(lxb_dom_element_t *el, const char *name);
/* CSSOM §6.6.1's two prototypes for ONE realm — declared into core/realm.h's list, run once per realm. */
void cssom_install_proto(JSContext *ctx);
void cssom_free(JSContext *ctx);
/* `CSSStyleDeclaration` and `CSSStyleProperties` as globals, and `getComputedStyle` on the one the Window IDL
   puts it on. */
void cssom_install(JSContext *ctx, JSValueConst global);
/* §7.1's ElementCSSInlineStyle `[SameObject, PutForwards=cssText] readonly attribute CSSStyleProperties style`
   — installed by the html layer, because the attribute is HTMLElement's and the object is this component's. */
void cssom_install_style_attribute(JSContext *ctx, JSValueConst proto);

/* CSSOM §6.6's SERIALIZE A CSS DECLARATION BLOCK over the text a backing keeps — the declarations that parsing
   `text` produces, with §6.6's shorthand consolidation loop applied, joined by a single SPACE and each closed
   by a semicolon. OWNED. NULL for a block that declares nothing, which is both the spec's "the serialization
   of an empty CSS declaration block is the empty string" and §6.4's "null if there are no such declarations" —
   the two answers a caller has to tell apart, which is why this reports it as NULL rather than as "".
   EXPORTED FOR §6.4's SERIALIZE A CSS RULE, because a rule's `cssText` is its selector list and this. */
char *cssom_serialize_declarations(const char *text, size_t len);

/* §6.4.3's `style`: a CSSStyleProperties whose DECLARATIONS are `rule`'s — computed flag unset, readonly flag
   unset, parent CSS rule the rule, owner node null. Every read and every write goes back through the rule's own
   record, so two flows disagree about `rule.style.color` exactly as they disagree about an inline style.
   OWNED: the caller frees. */
JSValue cssom_style_properties_for_rule(JSContext *ctx, JSValueConst rule);

/* Web IDL §3.4.4's [PutForwards=cssText] SETTER, declared once and shared by the two attributes that carry it
   — §7.1's `element.style` and §6.4.3's `rule.style`. It reads the attribute back by NAME through its own
   getter, which is what §3.4.4 states, so one declaration serves both and neither component writes its own. */
int cssom_put_forwards_setter(void);

/* CSS SYNTAX'S "PARSE A STYLESHEET'S CONTENTS", through THE AGENT'S ONE CSS PARSER.
 *
 * IT IS AN ENTRY AND NOT A HANDLE ON THE PARSER, and that is the whole point. This component owns the parser
 * because it owns the SELECTOR-STATE RECORD lexbor would otherwise leave on a dead stack frame — see the long
 * note in css_style_declaration.c, and the segfault it describes — and that ownership is only worth anything
 * while the arena swap, the parse and the "the record came back unchanged" assertion stay together in ONE
 * place. Handing a second component the `lxb_css_parser_t *` would be handing it three steps to remember.
 *
 * Each TOP-LEVEL rule is handed to `cb` as TEXT, never as a lexbor pointer: the arena is destroyed before this
 * returns, and CSSOM §6.4's objects have to outlive it — they park to the IDB cold tier and fork per flow, and
 * a rule named by a pointer into a freed arena can do neither. `type` is lexbor's own rule type, so the caller
 * decides what it has no interface for rather than this silently dropping it. For LXB_CSS_RULE_STYLE,
 * `selector_text` is the serialized selector list and `block_text` the serialized declaration block; for any
 * other type both are NULL, because how a rule splits into prelude and body is that rule interface's business.
 * Both strings are BORROWED for the duration of the call.
 *
 * Returns how many top-level rules the text produced — which is what CSS Syntax's "parse a RULE" needs in
 * order to be that instead of this: exactly one, or a syntax error. */
typedef void (*CssomRuleFn)(void *ud, unsigned type, const char *selector_text, const char *block_text);
unsigned cssom_parse_rules(const char *text, size_t len, CssomRuleFn cb, void *ud);

#endif
