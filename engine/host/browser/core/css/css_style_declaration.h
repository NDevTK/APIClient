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
/* CSS Cascade §6's CASCADED VALUE for `name` on `el`, as text. Every origin this engine has contributes its
   declarations into ONE list — this flow's author style rules, the element's own style attribute, css-cascade-5
   §6.5's author presentational hint origin (core/css/css_presentational_hints.h) and the UA sheet — and
   core/css/css_cascade.h sorts that list by §6.1's criteria, §6.4.3's layer order included. OWNED: the caller
   frees.
   NULL WHEN NO DECLARATION WON, which is the common case and a real answer rather than a missing one: §7.1 and
   §7.2 are both written for it ("unless the cascade results in a value"), and telling it apart from a declared
   initial value is what lets an INHERITED property ask the parent instead. §7's DEFAULTING is the step that
   acts on it (core/css/css_defaulting.h), and the property's `Computed value:` line is applied after that
   (core/css/css_computed_value.h) — three steps, in that order, and this is the first.
   §7.3's THREE CASCADE-DEPENDENT KEYWORDS NEVER COME OUT OF HERE, because they are discharged INSIDE the sort:
   `revert`, `revert-layer` and `revert-rule` are defined by the origin, layer and rule the declaration sat in,
   and none of those survives into a cascaded value. The one exception is the one §7.3.4 states — a `revert` in
   the user-agent origin "is equivalent to unset" — which is answered as that keyword for §7 to resolve. */
char *cssom_cascaded_value(lxb_dom_element_t *el, const char *name);

/* CSS Cascade §7.1's INITIAL VALUE of `name` — the property's own `Initial:` line, out of lexbor's registry and
   out of this component's table of the eight border longhands the registry does not carry. OWNED; NULL for a
   property with no initial value anywhere (a custom property nobody set), which §6.6.1 answers as the empty
   string. Exported for §7's defaulting step, which is the only thing that reaches for it. */
char *cssom_initial_value(const char *name);
/* §6.6.1's two prototypes, CSS Fonts §12.1's third and CSSOM §6.4.7's fourth, for ONE realm — declared into
   core/realm.h's list, run once per realm. */
void cssom_install_proto(JSContext *ctx);
void cssom_free(JSRuntime *rt);
/* `CSSStyleDeclaration`, `CSSStyleProperties`, `CSSFontFaceDescriptors` and `CSSPageDescriptors` as globals,
   and `getComputedStyle` on the one the Window IDL puts it on. */
void cssom_install(JSContext *ctx, JSValueConst global);
/* §7.1's ElementCSSInlineStyle `[SameObject, PutForwards=cssText] readonly attribute CSSStyleProperties style`
   — installed by the html layer, because the attribute is HTMLElement's and the object is this component's. */
void cssom_install_style_attribute(JSContext *ctx, JSValueConst proto);

/* WHICH RULE'S DECLARATION BLOCK THIS IS — the one thing that decides which declarations it may HOLD, and
 * therefore an argument of the serialization below rather than a question any reader asks for itself.
 *
 * THREE SPECIFICATIONS RESTRICT A BLOCK BY THE RULE IT BELONGS TO, and each states it as a closed sentence.
 * CSS Paged Media §4.3: "The @page rule can only contain page properties and margin at-rules. The margin
 * at-rules can only contain page-margin properties" (core/css/css_page.h owns the two lists). CSS Animations
 * §3: "The <declaration-list> inside of <keyframe-block> accepts any CSS property except those defined in this
 * specification, but does accept the animation-timing-function property", plus "properties qualified with
 * !important are invalid and ignored" (core/css/css_keyframes.h owns both halves). UNRESTRICTED is every other
 * block this engine has — an inline style, a computed style, a style rule's and an `@font-face`'s — and it is
 * ZERO so that a caller with no rule to name states it by naming nothing.
 *
 * IT IS A RULE TYPE AND NOT A PROPERTY FILTER, which is Blink's `StyleRule::RuleType` reaching
 * `CSSParserImpl::ConsumeDeclaration` and is the shape that keeps the three restrictions from becoming three
 * entry points that a fourth rule type could be added to only two of. */
typedef enum {
    CSSOM_BLOCK_UNRESTRICTED = 0,
    CSSOM_BLOCK_PAGE,        /* CSS Paged Media §4.3's page context — inside `@page` itself */
    CSSOM_BLOCK_MARGIN,      /* one of §5's margin at-rules */
    CSSOM_BLOCK_KEYFRAME,    /* CSS Animations §3's `<keyframe-block>` */
} CssomBlockContext;

/* CSSOM §6.6's SERIALIZE A CSS DECLARATION BLOCK over the text a backing keeps — the declarations that parsing
 * `text` produces, with §6.6's shorthand consolidation loop applied, joined by a single SPACE and each closed
 * by a semicolon, RESTRICTED to what `context`'s rule may hold. OWNED. NULL for a block that declares nothing,
 * which is both the spec's "the serialization of an empty CSS declaration block is the empty string" and
 * §6.4's "null if there are no such declarations" — the two answers a caller has to tell apart, which is why
 * this reports it as NULL rather than as "". EXPORTED FOR §6.4's SERIALIZE A CSS RULE, because a rule's
 * `cssText` is its prelude and this.
 *
 * THE RESTRICTION IS A SERIALIZATION AND NOT A PREDICATE because that is where it has to bite. A rule's
 * declarations are kept as TEXT (core/css/css_rule.h says why), so the two moments a restricted block's text
 * is DECIDED are the two moments it is written — the parse that builds the rule, and §6.6.1's writes through
 * the block — and filtering at both is what makes `length`, `cssText`, `getPropertyValue` and `setProperty`
 * agree without any of them asking the question for itself. */
char *cssom_serialize_declarations(const char *text, size_t len, CssomBlockContext context);

/* §6.4.3's `style`: a CSSStyleProperties whose DECLARATIONS are `rule`'s — computed flag unset, readonly flag
   unset, parent CSS rule the rule, owner node null. Every read and every write goes back through the rule's own
   record, so two flows disagree about `rule.style.color` exactly as they disagree about an inline style.
   OWNED: the caller frees. */
JSValue cssom_style_properties_for_rule(JSContext *ctx, JSValueConst rule);

/* CSS Fonts §12.1's `[SameObject, PutForwards=cssText] readonly attribute CSSFontFaceDescriptors style` on a
 * CSSFontFaceRule — the SAME §6.6 declaration block over the same rule-backed text, behind a DIFFERENT
 * interface, which is the whole of what makes it a different object.
 *
 * IT IS A SEPARATE INTERFACE AND NOT A CSSStyleProperties, because an `@font-face` block declares DESCRIPTORS
 * and not properties: `src` and `unicode-range` are not CSS properties at all (nothing outside a font-face
 * rule accepts them), `color` is not a descriptor, and the IDL says so by listing fifteen names rather than
 * inheriting §6.6.1's per-property partial interface. `cssom-fontfacerule-constructors.html` reads the
 * difference directly — `style.toString()` must be `[object CSSFontFaceDescriptors]` — and
 * `cssstyledeclaration-cssfontrule.tentative.html` reads it the other way, asserting `"unicode-range" in style`
 * on a block where CSSStyleProperties has no such attribute. OWNED: the caller frees. */
JSValue cssom_font_face_descriptors_for_rule(JSContext *ctx, JSValueConst rule);

/* CSSOM §6.4.7's `[SameObject, PutForwards=cssText] readonly attribute CSSPageDescriptors style` on a
 * CSSPageRule — the SAME §6.6 declaration block over the same rule-backed text, behind a THIRD interface.
 *
 * IT IS A SEPARATE INTERFACE FOR THE REASON CSS Fonts' IS, and the IDL says so twice over: §6.4.7 declares
 * `interface CSSPageDescriptors : CSSStyleDeclaration` — NOT `: CSSStyleProperties` — and then lists fourteen
 * attributes rather than inheriting §6.6.1's per-property partial interface. So `pageRule.style.cssFloat` is
 * `undefined` and `pageRule.style.size` is a member, which is exactly what css/cssom/page-descriptors.html
 * reads from both sides. The declarations the block ADMITS are restricted to match, by the entry above.
 * OWNED: the caller frees. */
JSValue cssom_page_descriptors_for_rule(JSContext *ctx, JSValueConst rule);

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
    /* IS `prelude` A SERIALIZED SELECTOR LIST? True for a qualified rule whose prelude parsed as one; FALSE
       for a qualified rule whose prelude did NOT, where it is the RAW SOURCE SPAN instead, and false for every
       at-rule, whose prelude is its own grammar's raw source and which `at_name` already marks.
       IT IS THE CONTEXT'S QUESTION AND THEREFORE THE CALLER'S. CSS Syntax consumes a qualified rule without
       deciding what its prelude MEANS — "if the rule is valid in the current context, return it" — and at a
       stylesheet's top level the context is a style rule, so `0%, 100% { }` there is an invalid selector list
       and the rule is dropped. Inside a `@keyframes` the very same text is CSS Animations §3's
       `<keyframe-selector>#` and the rule is a `<keyframe-block>`. Lexbor reports the second shape as a
       BAD_STYLE rule carrying the raw prelude, which is exactly the material the caller needs to tell the two
       apart, so it is reported rather than dropped in the parser layer — the same reason a body's declarations
       AND its child rules are both reported for every rule that has a body. */
    bool        prelude_is_selectors;
    /* A SERIALIZED DECLARATION BLOCK — the declarations this rule's body declares. NULL for a statement
       at-rule with no body at all (`@import url(x);`); the empty string for a body that declares nothing,
       which is every `@media` and every `@page` that holds only rules.
       A RULE REPORTED WITH A BLOCK IS ALSO WALKED FOR CHILDREN, because CSS Syntax's `<declaration-rule-list>`
       is a body holding BOTH and two rules in this build are one: a style rule since CSS Nesting, and
       CSSOM §6.4.7's `@page`, whose body is page descriptors beside CSS Paged Media §4.3's margin at-rules.
       Which of the two a given at-rule may HAVE is the caller's to decide — an `@font-face` contains no rules
       and a `@media` declares nothing, and CSS Syntax drops what the rule may not hold. */
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
 * A rule CSS SYNTAX SAYS IS INVALID IN EVERY CONTEXT is dropped by the parse and never reported: an at-rule
 * whose own grammar failed, and a declaration where a rule belongs. That is what every user agent does with
 * one — an invalid rule is not in `cssRules` — and it is also what makes the count above mean what CSS
 * Syntax's parse-a-RULE means, so `insertRule('@namespace { }')` is a SyntaxError rather than a rule with no
 * object.
 * A QUALIFIED RULE WHOSE PRELUDE IS NOT A SELECTOR LIST IS NOT THAT, AND USED TO BE TREATED AS IT. Whether one
 * is invalid depends on the CONTEXT and nothing here knows the context, so it is reported with
 * `prelude_is_selectors` unset and the builder decides — which is what makes `0%, 100% { }` a
 * `<keyframe-block>` inside a `@keyframes` and a dropped rule anywhere else. A builder that drops it must
 * still return a handle for it, by the rule above, and must still count it out of its own total. */
typedef void *(*CssomRuleFn)(void *ud, void *parent, const CssomRule *rule);
unsigned cssom_parse_rules(const char *text, size_t len, CssomRuleFn cb, void *ud);

#endif
