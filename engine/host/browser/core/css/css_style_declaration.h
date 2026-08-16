/* CSSOM §6.6 — the CSS DECLARATION BLOCK, its two interfaces (§6.6.1's CSSStyleDeclaration and the
   CSSStyleProperties that carries the per-property attributes), and the cascade every block's computed view
   reads through. A block is made by one of three creators and they differ only in §6.6's associated properties:
   §7.1's `element.style`, §7.2's `getComputedStyle` and §6.4.3's `rule.style`. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_DECLARATION_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_STYLE_DECLARATION_H
#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>
#include "quickjs.h"

void cssom_init(JSContext *ctx);
/* CSS Cascade §6's CASCADED VALUE for `name` on `el` — inline, then this flow's author rules, then
   css-cascade-5 §6.5's author presentational hint origin (core/css/css_presentational_hints.h), then the UA
   sheet — as text. OWNED: the caller frees.
   NULL WHEN NO DECLARATION WON, which is the common case and a real answer rather than a missing one: §7.1 and
   §7.2 are both written for it ("unless the cascade results in a value"), and telling it apart from a declared
   initial value is what lets an INHERITED property ask the parent instead. §7's DEFAULTING is the step that
   acts on it (core/css/css_defaulting.h), and the property's `Computed value:` line is applied after that
   (core/css/css_computed_value.h) — three steps, in that order, and this is the first. */
char *cssom_cascaded_value(lxb_dom_element_t *el, const char *name);

/* CSS Cascade §7.1's INITIAL VALUE of `name` — the property's own `Initial:` line, out of lexbor's registry and
   out of this component's table of the eight border longhands the registry does not carry. OWNED; NULL for a
   property with no initial value anywhere (a custom property nobody set), which §6.6.1 answers as the empty
   string. Exported for §7's defaulting step, which is the only thing that reaches for it. */
char *cssom_initial_value(const char *name);
/* CSSOM §6.6.1's two prototypes for ONE realm — declared into core/realm.h's list, run once per realm. */
void cssom_install_proto(JSContext *ctx);
void cssom_free(JSRuntime *rt);
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
 * Every rule is handed to `cb` as TEXT, never as a lexbor pointer: the arena is destroyed before this returns,
 * and CSSOM §6.4's objects have to outlive it — they park to the IDB cold tier and fork per flow, and a rule
 * named by a pointer into a freed arena can do neither. Every string below is BORROWED for the duration of the
 * call.
 *
 * Returns how many TOP-LEVEL rules the text produced — which is what CSS Syntax's "parse a RULE" needs in order
 * to be that instead of this: exactly one, or a syntax error. */
typedef struct {
    /* WHICH KIND OF RULE, by NAME. NULL for a QUALIFIED (style) rule; otherwise the at-rule's identifier with
       no `@`, lowercased — "media", "import", "keyframes" — so the CALLER decides which §6.4 interface it has
       and which it has none for, rather than this file deciding for it from a lexbor enumeration that knows
       only three at-rules by name. */
    const char *at_name;
    /* A style rule's SERIALIZED SELECTOR LIST, or an at-rule's own PRELUDE text (for `@media`, its media query
       list), with leading and trailing whitespace removed. Never NULL — `@media {}` has an EMPTY prelude, which
       is a media query list of no queries and not the absence of one. */
    const char *prelude;
    /* A style rule's SERIALIZED DECLARATION BLOCK. NULL for an at-rule, whose body is RULES and not
       declarations; the empty string for a style rule that declares nothing. */
    const char *block;
    /* Does this rule have a `{}` BLOCK at all? `@import url(x);` is a STATEMENT at-rule and has none, which is
       a different fact from having an empty one and is what tells the two kinds apart. */
    bool        has_block;
} CssomRule;

/* Called once per rule the parse KEPT, in document order. `parent` is what this callback returned for the rule
 * that ENCLOSES this one, or NULL at the top level; a nested rule always arrives after its parent, so a builder
 * needs no depth arithmetic and never sees a lexbor list. What the callback returns is opaque here — the parse
 * only ever hands it back — with the ONE meaning NULL carries: a callback that answers NULL for a rule makes
 * that rule's children arrive as though they were TOP-LEVEL, so a builder keeping objects must return a handle
 * for every rule it is told about, the ones it decided to drop included.
 *
 * A rule CSS SYNTAX SAYS IS INVALID is dropped by the parse and never reported: a qualified rule whose prelude
 * is not a selector list, an at-rule whose own grammar failed, and a declaration where a rule belongs. That is
 * what every user agent does with one — an invalid rule is not in `cssRules` — and it is also what makes the
 * count above mean what CSS Syntax's parse-a-RULE means, so `insertRule('???')` is a SyntaxError rather than a
 * rule with no object. */
typedef void *(*CssomRuleFn)(void *ud, void *parent, const CssomRule *rule);
unsigned cssom_parse_rules(const char *text, size_t len, CssomRuleFn cb, void *ud);

#endif
