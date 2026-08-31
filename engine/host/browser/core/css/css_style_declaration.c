/* CSSOM §6.6 — the CSS DECLARATION BLOCK, its two interfaces, and getComputedStyle().
 *
 * WHAT WAS HERE BEFORE: nothing. `el.style.display = 'none'` wrote an ordinary JS property on an object that
 * did not exist, and getComputedStyle was absent, so a page reading a computed value threw. Both are named in
 * this project's own rules as the shape of a fidelity gap — a getter returning opaque where the SPEC COMPUTES A
 * REAL VALUE — and both matter to what this engine is for: a bundle that branches on
 * `getComputedStyle(el).display === 'none'` routes differently on each side, and each side has its own
 * endpoints.
 *
 * THE CITATION CONVENTION, AND IT IS A RULE THIS FILE BROKE RATHER THAN ONE IT LACKED. A bare `§N` here means
 * CSSOM: the line above says so, and engine/citegen.mjs reads it the same way, resolving a citation that names
 * no standard by the file's DOMINANT ANCHOR. That convention is safe only where CSSOM is the only standard this
 * file cites that HAS an §N — and it is not. The cascade this file resolves lives in css-cascade-5, and these
 * eight numbers are two documents each:
 *   css-cascade-5 §6.1   Cascade Sorting Order        CSSOM §6.1   CSS Style Sheets
 *   css-cascade-5 §6.2   Cascading Origins            CSSOM §6.2   CSS Style Sheet Collections
 *   css-cascade-5 §6.3   Important Declarations       CSSOM §6.3   Style Sheet Association
 *   css-cascade-5 §6.4   Cascade Layers               CSSOM §6.4   CSS Rules
 *   css-cascade-5 §6.4.2 Layer Naming and Nesting     CSSOM §6.4.2 The CSSRule Interface
 *   css-cascade-5 §6.4.3 Layer Ordering               CSSOM §6.4.3 The CSSStyleRule Interface
 *   css-cascade-5 §7.1   Initial Values               CSSOM §7.1   The ElementCSSInlineStyle Mixin
 *   css-cascade-5 §7.2   Inheritance                  CSSOM §7.2   Extensions to the Window Interface
 * Both meanings of §6.1 and of §6.4 appear in the ONE paragraph below. So a `§` that is not CSSOM's NAMES ITS
 * STANDARD, and names it WITHIN THE FORTY CHARACTERS BEFORE THE `§` — the window citegen reads — which means
 * the name goes AT THE CITATION and not at the head of the sentence, and a LIST carries the name on every row
 * rather than once above it.
 * A SECOND CITATION IN ONE SENTENCE NEEDS ITS OWN NAME. The prose between two `§`s is exactly what pushes the
 * first name out of the second's window, so naming the standard once per sentence leaves the second citation
 * unanchored and the file vote answers it — which is how `css-values-4 §10.9 "Type Checking" ... §10's opening
 * sentence`, one correct claim about one document, filed its second half under CSSOM §10 IANA Considerations
 * inside a DCHECK message that a crash prints.
 *
 * A BLOCK IS THE FIVE THINGS CSSOM §6.6 SAYS IT IS, AND THAT IS WHAT DECIDES EVERY MEMBER'S ANSWER: the
 * COMPUTED FLAG, the READONLY FLAG, the DECLARATIONS, the PARENT CSS RULE and the OWNER NODE. Three creators
 * differ only in what they set those to — CSSOM §7.1's `element.style` (owner node this, no parent rule,
 * neither flag), CSSOM §7.2's getComputedStyle (owner node the element, both flags) and CSSOM §6.4.3's
 * `rule.style` (NO owner node, parent rule the rule, neither flag).
 * They used to be a two-valued `mode`, which could express the first two and had no
 * room for the third, and whose "no element" arm answered the empty string for every member — so a block with
 * no owner node, which is exactly what a rule's is, would have read as an empty declaration block rather than
 * as the rule's own declarations.
 *
 * LEXBOR OWNS THE CSS, and that is the point of binding to it rather than hand-rolling. It has the real
 * property registry — which is CSSOM §2 Terminology's "supported CSS property" set for this engine, so CSSOM
 * §6.6.1's three per-property partial interfaces are GENERATED FROM IT rather than typed here — a real declaration
 * parser, real value serializers, and a real selector matcher that answers
 * for a SINGLE node. Every layer below is Lexbor doing the parsing and this file doing the cascade.
 *
 * THE DECLARATIONS ARE THE BACKING'S OWN TEXT, which is the design decision the rest follows from. CSSOM §6.6
 * models them as a list the block object holds and pushes back to the `style` attribute through "update style
 * attribute"; this engine keeps the BACKING authoritative and derives the list per read, because the backing is
 * what time-travels. For an element that backing is the `style` CONTENT ATTRIBUTE: lexbor can also hold parsed
 * styles on the element (an AVL keyed by property), and using that would have been faster and WRONG — it lives
 * outside the per-flow DOM delta, so a `style.color` written by one forked arm would be visible to its sibling,
 * while the attribute IS captured. For a rule it is the rule record's block TEXT, captured by the same per-flow
 * COW delta through the record's accessor, so two flows can disagree about `rule.style.color` exactly as they
 * can about an inline style. Nothing is cached in between, for the reason the cascade caches nothing.
 *
 * THE CASCADE IS RESOLVED LIVE, per read, from THE RUNNING FLOW'S OWN OBJECTS: the author rules of the CSS
 * STYLE SHEETS in CSSOM §6.2's list for this element's root — the sheet objects a page holds, whose rules it
 * inserts, deletes and retargets — the element's own style attribute, css-cascade-5 §6.5's AUTHOR
 * PRESENTATIONAL HINT ORIGIN (core/css/css_presentational_hints.h, which is where HTML's own markup enters the
 * cascade and why it is an origin of its own rather than a row of the UA table), and the UA default. THEY ARE
 * COLLECTED, NOT ASKED IN TURN: each contributes its declarations into ONE list and core/css/css_cascade.h
 * sorts that list by css-cascade-5 §6.1's six criteria, because the criteria are LEXICOGRAPHIC and asking the
 * origins in sequence silently reorders them (it puts css-cascade-5 §6.1's Element-Attached criterion above its
 * Origin-and-Importance one, and it cannot express css-cascade-5 §6.4's Layers criterion at all, which sits
 * above Specificity). Nothing is cached across a read, because a cache would be shared state that the flow
 * machinery does not swap; and reading the OBJECTS rather than re-parsing each `<style>` element's text is what
 * makes the objects load-bearing instead of inert, which is the whole of why CSSOM §6.1 CSS Style Sheets,
 * CSSOM §6.2 CSS Style Sheet Collections and CSSOM §6.4 CSS Rules exist. THE SAME TWO NUMBERS, THE OTHER
 * DOCUMENT, THREE SENTENCES APART — which is the convention paragraph above stated as a measurement.
 *
 * AND THE CASCADE IS WHERE THIS FILE STOPS. What it produces is the SPECIFIED value — the declaration that won
 * — which is neither the computed value a spec algorithm reads nor the resolved value getComputedStyle
 * returns; core/css/css_computed_value.h owns both of those steps and reads the cascade through the one entry
 * below. On the way IN, a declaration reaches the cascade through core/css/css_shorthand.h, because a shorthand
 * declaration sets its longhands and the cascade is over longhands only. */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include <lexbor/css/css.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/idl_args.h"
#include "core/idl_indexed.h"   /* §6.6.1's `getter CSSOMString item(unsigned long index)` — the getter half */
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/selector_match.h"
#include "core/css/css_cascade.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_keyframes.h"
#include "core/css/css_math.h"
#include "core/css/css_page.h"
#include "core/css/css_property_numeric.h"
#include "core/css/css_presentational_hints.h"
#include "core/css/css_rule.h"
#include "core/css/css_shorthand.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_style_sheet.h"
#include "core/css/style_sheet_list.h"
#include "solver/concolic.h"   /* §6.6.1's `item` takes an index unknown external input crosses AS ITSELF */
#include "solver/dom_cow.h"

/* TWO PRIVATE KEYS, BOTH SYMBOLS, AND THEY ARE TWO BECAUSE A SLOT IS A BRAND. `g_decl_key` hangs a block's own
   §6.6 record off the block; `g_inline_key` hangs CSSOM §7.1's [SameObject] block off the ELEMENT's wrapper. One key
   served both, which made an element pass the block's brand check — `CSSStyleDeclaration.prototype.item.call(el,
   0)` found the declaration object where the record belongs and read a field out of it — the same defect
   style_sheet_list.c records for its own two keys. */
static JSValue g_decl_key = JS_UNDEFINED, g_inline_key = JS_UNDEFINED;
/* PER REALM — Web IDL §3.7 Interfaces, and here it decides ANSWERS: a C member runs in the realm that DEFINED
   it. Every block this engine builds is a CSSOM §6.6.1 CSSStyleProperties (all three creators say so), so THAT
   is the class, and CSSStyleDeclaration.prototype — the base nothing is an instance of — is a per-realm value
   slot beside it, which is the same shape CSSOM §6.1.1's StyleSheet and CSSOM §6.4.2's CSSRule take. */
static JSClassID g_cssd_class;
static int       g_declaration_proto_slot = -1;
/* CSS Fonts 5 §9.1 The CSSFontFaceRule interface's CSSFontFaceDescriptors.prototype, the same way and for the
   same reason. THE LEVEL IS PART OF THIS CITATION: CSS Fonts 4 numbers the same-titled section §12.1 and declares
   the interface with SIX names fewer, so a bare "CSS Fonts §12.1" sends a reader to an edition that does not
   carry what this file installs — the title is identical across both, which is exactly why the number alone
   cannot be checked. It is a THIRD
   prototype over the SAME class and the same record: an `@font-face` block's declarations are kept where
   CSSOM §6.4.3's are (the rule's own text, through core/css/css_rule.h), so what differs is only which member names
   the interface answers to. */
static int       g_font_face_proto_slot = -1;
/* CSSOM §6.4.7 The CSSPageRule Interface's CSSPageDescriptors.prototype, the same way and for the same
   reason — a FOURTH prototype over
   the one class and the one record. A `@page` rule's descriptors are kept where CSSOM §6.4.3's declarations are (the
   rule's own text, through core/css/css_rule.h), so what differs is only which member names the interface
   answers to and, through core/css/css_page.h, which declarations the block admits at all. */
static int       g_page_proto_slot = -1;
/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. §6.6.1's per-property
   attributes are GENERATED from Lexbor's property registry, so their setter ids are an array indexed the same
   way the registry is — one entry per property, declared once, installed into every realm, and SHARED by that
   property's camel-cased, webkit-cased and dashed spellings because §6.6.1 gives the three one set of setter
   steps. A row CSSOM §2 Terminology excludes holds -1, which is `no setter`: an installer reaching one would make an
   attribute silently read-only, so it is DCHECKed at the install rather than left to be discovered. */
static int g_set_css_text_id = -1, g_get_prop_id = -1, g_remove_prop_id = -1, g_get_priority_id = -1,
           g_set_prop_id = -1, g_item_id = -1, g_put_forwards_id = -1;
static int g_property_set_id[LXB_CSS_PROPERTY__LAST_ENTRY];
static int g_id_gcs;   /* getComputedStyle — declared once per agent, installed on each realm's window */
static int     g_ready;

/* ---- the text buffer every serializer writes into ------------------------------------------------------- */
typedef struct { char *s; size_t n, cap; } CssBuf;

static lxb_status_t css_buf_cb(const lxb_char_t *data, size_t len, void *ctx)
{
    CssBuf *b = ctx;
    if (b->n + len + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 128;
        while (c < b->n + len + 1) c *= 2;
        b->s = realloc(b->s, c);
        CHECK(b->s != NULL, "cssom: OOM serializing a declaration — a dropped value would read as unset");
        b->cap = c;
    }
    memcpy(b->s + b->n, data, len);
    b->n += len;
    b->s[b->n] = 0;
    return LXB_STATUS_OK;
}

static void css_buf_free(CssBuf *b) { free(b->s); b->s = NULL; b->n = b->cap = 0; }

static void css_buf_add(CssBuf *b, const char *s) { css_buf_cb((const lxb_char_t *)s, strlen(s), b); }

/* ---- §6.6's five associated properties, as the record every member reads ---------------------------------- */

/* The block's own record, BRAND-CHECKED. Every member of both interfaces is on a PROTOTYPE, so a page can apply
   one to anything at all, and the answer is a TypeError — Web IDL §3.7.6 Attributes' "if jsValue does not
   implement target, then ... throw a TypeError" for the accessors, Web IDL §3.7.7 Operations' for `item`,
   `getPropertyValue` and the rest — which a page tells apart from the empty string the "no element" arm used to
   hand back. THE NUMBER HERE WAS Web IDL §3.7.5, WHICH IS "Constants": a real section of the right standard, about
   something else entirely, and nothing could catch it because a brand check is not a term any index files.
   Returns JS_EXCEPTION with the error already thrown. OWNED. */
static JSValue cssd_block(JSContext *ctx, JSValueConst v)
{
    JSAtom k;
    JSValue slots = JS_UNDEFINED;

    DCHECK(g_ready, "a CSS declaration block's record was asked for before cssom_init ran");
    if (JS_IsObject(v)) {
        k = JS_ValueToAtom(ctx, g_decl_key);
        CHECK(k != JS_ATOM_NULL, "the CSS declaration block key could not be interned");
        if (JS_GetOwnSlot(ctx, &slots, v, k) <= 0)   /* an own SLOT, never a lookup — see event.c */
            slots = JS_UNDEFINED;
        JS_FreeAtom(ctx, k);
        if (JS_IsObject(slots)) return slots;
        JS_FreeValue(ctx, slots);
    }
    return JS_ThrowTypeError(ctx, "not a CSSStyleDeclaration");
}

/* §6.6's COMPUTED FLAG and READONLY FLAG. Both are written once by the creator and never change, so they are
   read as the booleans they are rather than inferred from which other field is set. */
static bool cssd_flag(JSContext *ctx, JSValueConst block, const char *name)
{
    JSValue f = JS_GetPropertyStr(ctx, block, name);
    bool set = JS_ToBool(ctx, f) != 0;

    JS_FreeValue(ctx, f);
    return set;
}

/* §6.6's OWNER NODE, as the element it is. NULL is a REAL state and not a failure: CSSOM §6.4.3's block has
   none. */
static lxb_dom_element_t *cssd_owner_element(JSContext *ctx, JSValueConst block)
{
    JSValue owner = JS_GetPropertyStr(ctx, block, "ownerNode");
    lxb_dom_node_t *n = node_of(owner);

    JS_FreeValue(ctx, owner);
    if (!n) return NULL;
    DCHECK(n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "§6.6 types a CSS declaration block's owner node as an Element, and a creator handed over a node "
           "that is not one");
    return lxb_dom_interface_element(n);
}

/* §6.6's PARENT CSS RULE — JS_NULL for the two element-backed blocks. OWNED. */
static JSValue cssd_parent_rule(JSContext *ctx, JSValueConst block)
{
    return JS_GetPropertyStr(ctx, block, "parentRule");
}

/* ---- the parser, and the declaration list of a chunk of CSS text ---------------------------------------- */
static lxb_css_parser_t *g_parser;

/* THE PARSER'S SELECTOR-PARSE STATE, AND WHY THIS ENGINE OWNS IT RATHER THAN LETTING LEXBOR PICK ONE.
 *
 * `lxb_css_stylesheet_parse` declares `lxb_css_selectors_t selectors;` ON ITS OWN STACK, installs it when the
 * parser has none (`parser->selectors = &selectors`), and on the way out runs
 * `parser->selectors = lxb_css_selectors_destroy(&selectors, false)` — which with `self_destroy == false`
 * RETURNS ITS ARGUMENT. So a parser that arrives with no selector state leaves every stylesheet parse holding a
 * pointer INTO A DEAD STACK FRAME, and the NEXT parse takes the other branch: it CLEANS that address (seven
 * field writes) and then uses it as the live selector state for the whole parse.
 *
 * That is a write through a dangling pointer, and for a while it was invisible for the worst possible reason:
 * the author collection was reached from ONE call chain, so the stale address was re-materialised as the same
 * slot of the same frame at the same depth every time — self-consistent by accident. The moment the computed
 * value gained a second and a third caller at different depths (element_view.c's ancestor walk for `display`,
 * and css_computed_value.c's own walk to the box parent), the stale address pointed at a LIVE frame instead,
 * and the second element of a walk segfaulted inside `lxb_css_stylesheet_destroy` reading a `sst` that the
 * clean had overwritten.
 *
 * `lxb_css_parser_init` does not initialise `selectors` (the field is NULL only because the parser is
 * calloc'd), and `lxb_css_parser_destroy` does not free it — so lexbor's own pair for owning one,
 * `lxb_css_parser_selectors_init` / `_destroy`, is what this uses. With a record of the AGENT's lifetime
 * installed, the stack-local branch above is unreachable and there is nothing left to dangle. */
static lxb_css_selectors_t *g_selectors;

/* Asserted after every parse rather than trusted: the record is what keeps the branch unreachable, so a parse
   that hands it back changed is the one thing that could put the stack local back. */
static void cssd_selectors_intact(void)
{
    DCHECK(g_selectors != NULL && lxb_css_parser_selectors(g_parser) == g_selectors,
           "the CSS parser came out of a parse holding selector state that is not this component's. Lexbor's "
           "`lxb_css_stylesheet_parse` installs a STACK-LOCAL selectors record when the parser has none and "
           "leaves the parser pointing at it after the frame is dead, so a parser with no record of its own "
           "writes through a dangling pointer on its very next parse — cssom_init installs one for exactly "
           "that reason, and it must survive every parse");
}

/* Parse `text` as a declaration BLOCK (the contents of a style="" attribute). The returned list owns a memory
   arena that the caller destroys — one arena per parse, so nothing outlives the read that asked for it, which
   is what keeps this free of state the flow machinery would have to swap. */
static lxb_css_rule_declaration_list_t *cssd_parse_block(const char *text, size_t len, lxb_css_memory_t **pmem)
{
    lxb_css_memory_t *mem = lxb_css_memory_create();
    lxb_css_rule_declaration_list_t *list;

    CHECK(mem != NULL, "cssom: the CSS arena allocation failed");
    if (lxb_css_memory_init(mem, 64) != LXB_STATUS_OK) {
        lxb_css_memory_destroy(mem, true);
        *pmem = NULL;
        return NULL;
    }
    /* The ARENA IS THE PARSER'S, and it is swapped per parse rather than shared: lexbor moved the memory from
       an argument onto the parser, and a parser-lifetime arena would accumulate every declaration this engine
       ever parsed — a leak the whole point of one arena per read was to avoid. Set it, parse, take it back. */
    lxb_css_parser_memory_set(g_parser, mem);
    list = lxb_css_declaration_list_parse(g_parser, (const lxb_char_t *)text, len);
    lxb_css_parser_memory_set(g_parser, NULL);
    cssd_selectors_intact();
    *pmem = mem;
    return list;
}

/* A declaration's PROPERTY NAME, serialized. Comparing by name rather than by the registry id is what makes a
   CUSTOM property (`--brand`) work through exactly the same path as a known one — it has no id to compare, and
   neither does a SHORTHAND lexbor's registry does not carry. OWNED. */
static char *cssd_decl_name(const lxb_css_rule_declaration_t *d)
{
    CssBuf b = { 0 };

    lxb_css_property_serialize_name(d->u.user, d->type, css_buf_cb, &b);
    return b.s ? b.s : NULL;
}

/* A declaration's VALUE, serialized. CSS Syntax's CONSUME A DECLARATION ends with "while the last token in
   value is a <whitespace-token>, remove that token", and this is where that step lands. A declaration lexbor's
   registry TYPES is serialized back out of that typed value and carries no surrounding whitespace at all; one
   it does NOT — every `__CUSTOM`, which is every property outside the registry, including the `border-width`
   and `border-*-style` spellings — is the RAW TOKEN STREAM with the whitespace tokens in it. `border-top-
   style: solid ;` would otherwise reach its grammar as "solid " and match no keyword in it, and the failure
   would be a silent initial value rather than a crash. Leading whitespace is already gone: the parser consumes
   it between the colon and the first value token. OWNED. */
static char *cssd_decl_value(const lxb_css_rule_declaration_t *d)
{
    CssBuf b = { 0 };
    size_t n;

    lxb_css_property_serialize(d->u.user, d->type, css_buf_cb, &b);
    if (!b.s) return NULL;
    for (n = b.n; n > 0 && isspace((unsigned char)b.s[n - 1]); n--) { }
    b.s[n] = '\0';
    return b.s;
}

/* ---- §6.6's DECLARATIONS: the LONGHAND list, and the ONE place a block's is built -------------------------
 *
 * §6.6's declarations are LONGHANDS, AT MOST ONE PER PROPERTY, and both halves are the spec's own words.
 * "A CSS declaration block is an ordered collection of CSS PROPERTIES with their associated values" is one
 * entry per property; §6.6's parse-a-CSS-declaration-block parses each declaration "according to the
 * appropriate CSS specifications", which for a shorthand is css-cascade-5 §3 "Shorthand Properties"' "sets all of
 * its longhand sub-properties, exactly as if expanded in place". So `style="margin:1px"` holds FOUR
 * declarations, `length` is 4, and `item(0)` is `margin-top`. This engine's block used to hold the author's
 * shorthand unexpanded, which is why `length` crashed rather than answering 1.
 *
 * THE DUPLICATE COLLAPSE IS THE EXPANSION'S PREREQUISITE, because `margin:1px; margin-top:2px` expands to five
 * declarations for four properties. IT DOES NOT BELONG IN THE SERIALIZER, and the reading that put it there
 * misreads §6.6's own loop: the serializer's `already serialized` set is the SHORTHAND loop's bookkeeping —
 * what it skips is a declaration a shorthand already absorbed — and a duplicate reaching it is skipped because
 * the FIRST of the pair was written, which is the cascade's LOSER. No engine does that, and nothing could: the
 * block that reaches the serializer already holds one declaration per property, because that is what a block
 * IS. §6.6.1's set a CSS declaration states the invariant outright ("Exactly one CSS declaration whose property
 * name is a case-sensitive match of property must exist in declarations") and it is the only place CSSOM states
 * it, so the collapse belongs where the declarations are COLLECTED — here.
 *
 * THE SURVIVOR TAKES THE LATER DECLARATION'S PLACE, NOT THE EARLIER ONE'S, AND THAT IS A CASCADE FACT rather
 * than a serialization preference. CSS Logical §4 pairs a flow-relative property with a physical one and makes
 * them "share a computed value ... determined by cascading the declarations of both properties together as
 * one", and then says in its own note that this "requires implementations to maintain relative order of
 * declarations within a CSS declaration block". Collapsing `margin: 10px; …; margin-bottom: 10px` into the
 * EARLIER slot moves `margin-bottom` back in front of a `margin-block-end` that was declared after it, and the
 * pair's shared value flips to the wrong declaration. Removing the earlier entry and APPENDING is also §6.6.1's
 * own "simplest way to conform with the constraints", and it satisfies that section's logical-group constraint
 * ("target declaration must be at an index after all of those") unconditionally.
 * MEASURED, because the two readings disagree on a real test: on `css/cssom/cssstyledeclaration-csstext.html`'s
 * "Shorthands aren't serialized ..." subtest, Firefox (which removes and appends) PASSES, while Chrome 153 and
 * Safari 250 FAIL with `margin: 10px; margin-inline: 10px; margin-block: 10px;` — exactly the string an
 * in-place collapse produces, because Blink's parse path replaces a declaration where it stands and only its
 * CSSOM path relocates. The spec text and the passing browser agree; the two failing ones are the outlier.
 *
 * AND AN EARLIER `!important` DECLARATION IS NOT REPLACED AT ALL. CSS Cascade's sorting order puts importance
 * ahead of order of appearance, so in `padding: 10px !important; padding-left: 20px` the important declaration
 * wins and the later one never enters the block — which is what
 * `css/cssom/cssstyledeclaration-csstext-important.html` asserts and every engine does. */
typedef struct { char *name; char *value; bool important; } CssDecl;

/* WHICH RULE'S BLOCK is being collected — css_style_declaration.h's `CssomBlockContext`, which states the three
   restrictions and why the question belongs to the rule rather than to any reader. This is the one place a
   block's declarations are built, which is why it is asked here and nowhere else, and CSSOM_BLOCK_UNRESTRICTED
   is ZERO so that the `{ 0 }` every collector starts from states it. */
typedef struct { CssDecl *v; unsigned n, cap; CssomBlockContext context; } CssDecls;

static void cssd_decls_free(CssDecls *d)
{
    unsigned i;

    for (i = 0; i < d->n; i++) { free(d->v[i].name); free(d->v[i].value); }
    free(d->v);
    d->v = NULL;
    d->n = d->cap = 0;
}

/* Takes ownership of `name` and `value`. `value` may be NULL: CSS Syntax admits a declaration whose value is
   empty, which is why §6.6's step 4 is conditional. */
static void cssd_decls_append(CssDecls *d, char *name, char *value, bool important)
{
    if (d->n == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 8;
        d->v = realloc(d->v, d->cap * sizeof(*d->v));
        CHECK(d->v != NULL, "cssom: OOM collecting a declaration block's declarations — a dropped one reads as "
                            "a property the block never declared");
    }
    d->v[d->n].name = name;
    d->v[d->n].value = value;
    d->v[d->n].important = important;
    d->n++;
}

/* WHERE THE BLOCK DECLARES `name`, or -1. There is at most one, which is the invariant the collapse below
   maintains and every reader depends on. */
static int cssd_decls_index(const CssDecls *d, const char *name)
{
    unsigned i;

    for (i = 0; i < d->n; i++)
        if (strcmp(d->v[i].name, name) == 0) return (int)i;
    return -1;
}

static void cssd_decls_remove_at(CssDecls *d, unsigned at)
{
    DCHECK(at < d->n, "a declaration was removed from past the end of its own block");
    free(d->v[at].name);
    free(d->v[at].value);
    memmove(d->v + at, d->v + at + 1, (d->n - at - 1) * sizeof(*d->v));
    d->n--;
}

static char *cssd_strdup(const char *s)
{
    char *out = strdup(s);

    CHECK(out != NULL, "cssom: OOM copying a declaration's property name — a dropped one reads as a property "
                       "the block never declared");
    return out;
}

/* ONE PARSED LONGHAND DECLARATION joining the block's, under the collapse rule the note above states: an
   earlier important declaration is not replaced, and any other earlier one is REMOVED so the survivor takes
   this declaration's place. Takes ownership of `value`. */
static void cssd_decls_collect(CssDecls *d, const char *name, char *value, bool important)
{
    int at = cssd_decls_index(d, name);

    /* THE ONE POINT EVERY DECLARATION ENTERING A BLOCK CONVERGES ON, which is why the math-function invariant
       is asserted here and not at any of the three call sites. A longhand arrives from the declaration as
       written, from a shorthand's expansion, or from a `__UNDEF` this file re-judged, and each of those has its
       own reason to believe the value is valid — so the ASSERTION that they agree belongs where they meet.
       WHAT IT CATCHES is a math function reaching the cascade under a property whose grammar does not name its
       type: `border-top-width: calc(50% + 2px)` produced by an expansion that asked `<length-percentage>` where
       css-backgrounds-3 §3.3's `<line-width>` is `<length [0,∞]>`. That is not a dropped declaration and not a
       crash downstream — it is a value that flows all the way to a used value and answers a layout question
       with a number the page never wrote, which is the failure this whole path exists to end.
       IT IS A LONE MATH FUNCTION ONLY, because a longer value's components are the owning grammar's to split
       and this file holds no splitter; the crash for the case that needs one stands in
       cssd_undef_is_declaration, where the value is still whole.
       AND IT IS SCOPED TO THE PROPERTIES SOMETHING IN THIS ENGINE ACTUALLY VALIDATES, which is what
       `css_property_numeric_audited` answers. The invariant is that a producer which VALIDATED a value asked
       the right production, so it says nothing about a value no producer ever judged — and most of CSS is in
       that state here, because lexbor's registry stops well short of it: `border-radius: calc(4px)` and
       `gap: calc(1rem)` are typed by nothing, collected as raw tokens, and reach the cascade unexamined (which
       is why they have always worked). Asserting over those would abort on two of the commonest declarations
       on the web while proving nothing. A custom property is outside it for the stronger reason that CSS
       Properties and Values API 1 owns its grammar — `--x: calc(1s)` is a valid declaration whatever it says. */
#if APICLIENT_DEV
    if (value != NULL && !(name[0] == '-' && name[1] == '-') &&
        css_property_numeric_audited(name) && css_math_is_lone_function(value, strlen(value))) {
        unsigned prods, p;
        bool ok = false;

        (void)css_property_numeric(name, &prods);
        for (p = 0; !ok && (prods >> p) != 0; p++)
            ok = (prods & CSS_NUMERIC_BIT(p)) != 0 && css_math_matches(value, strlen(value), (CssMathProduction)p);
        DCHECK(ok,
               "a MATH FUNCTION is entering a declaration block under a property whose grammar names no "
               "numeric production its css-values-4 §10.9 'Type Checking' type matches. css-values-4 §10 "
               "'Mathematical Expressions' opens with the invariant — a math function \"can be used wherever "
               "such a value would be valid\", and "
               "nowhere else — so this is a producer that asked the WRONG production, and the answer it "
               "produced is a plausible number rather than a dropped declaration. The productions are per "
               "property and the neighbours disagree (core/css/css_property_numeric.h): whoever validated this "
               "value asked `<length-percentage>` for a `<line-width>`, or `<number>` for a `<percentage>`. "
               "Find the producer — the shorthand expansion in core/css/css_shorthand.c, or the `__UNDEF` "
               "re-judge in this file — and make it ask this property's own production");
    }
#endif
    if (at >= 0) {
        if (d->v[at].important && !important) { free(value); return; }
        cssd_decls_remove_at(d, (unsigned)at);
    }
    cssd_decls_append(d, cssd_strdup(name), value, important);
}

/* ---- css-values-4 §10's MATH FUNCTIONS AS A DECLARATION'S VALUE -------------------------------------------
 *
 * WHY THIS IS HERE AT ALL. css-values-4 §10 "Mathematical Expressions" opens: "A math function represents a numeric value,
 * one of: <length>, <frequency>, <angle>, <time>, <flex>, <resolution>, <percentage>, <number>, <integer>
 * ...or the <length-percentage>/etc mixed types, AND CAN BE USED WHEREVER SUCH A VALUE WOULD BE VALID." The
 * vendored parser has no math functions in it at all — `calc` appears nowhere under its css module — so every
 * property whose grammar it DOES carry rejects one, and CSS Syntax 3 §5.5.6 "Consume a declaration"'s last
 * step ("If decl is valid in the current context, return it; otherwise return nothing") then drops the
 * declaration. `width: calc(100vw - 20px)` therefore never reached a length parser, and the property fell back
 * to its initial value with nothing anywhere reporting it — the failure mode CSS is worst at, because an
 * initial value is a plausible value and every layout answer derived from it looks like a measurement.
 *
 * CSS 2.1 §4.2 "Rules for handling parsing errors" IS THE WARRANT, not an exception to it. Its whole framing
 * is forward compatibility — "To ensure that new properties and NEW VALUES FOR EXISTING PROPERTIES can be
 * added in the future, user agents are required to obey the following rules" — and it closes the illegal-value
 * rule with "A user agent conforming to a future CSS specification may accept one or more of the other rules
 * as well." A math function is exactly a new value for an existing property, and this engine is exactly the
 * later user agent. So re-judging here is not a loosening of CSS 2.1 §4.2's "user agents must ignore a declaration
 * with an illegal value"; it is the recognition that the value was never illegal, and that the parser deciding
 * so is one edition behind the value.
 *
 * WHAT IS NOT WIDENED. A declaration whose value carries no math function is lexbor's verdict and stands —
 * `display: bogus` is dropped here exactly as it was, and so is a math function whose css-values-4 §10.9 "Type
 * Checking" type does not match the production this property's grammar names. Both of those are css-syntax-3
 * §5.5.6's "return nothing", and neither becomes a crash: a page's own invalid declaration is not an engine gap.
 * The property's production is core/css/css_property_numeric.h's, and it is PER PROPERTY because the spec is:
 * css-fonts-4 §2.5's `font-size` is a `<length-percentage [0,∞]>` and css-backgrounds-3 §3.3's `<line-width>`
 * is `<length [0,∞]> | thin | medium | thick`, so `font-size: calc(50% + 2px)` is a declaration and
 * `border-top-width: calc(50% + 2px)` is not. Asking one question for both silently does one of them wrong. */

/* Does `value` contain a math function at all? CSS Syntax 3 §4.3.4 "Consume an ident-like token" is the whole
   rule — a FUNCTION token is an ident sequence "immediately followed by a U+0028 LEFT PARENTHESIS" — so the
   name is the ident run ending at a `(`, and WHICH names are math functions is core/css/css_math.h's closed
   list of css-values-4 §10.8 "Syntax"'s twenty-one notations rather than a second one written here.
   IT IS DELIBERATELY NOT A PARSE. The question this answers is only "is a math function why lexbor refused
   this", which decides whether to ASK the grammar at all; the grammar itself then decides validity, and a
   false yes here costs one refused `css_math_matches` rather than an accepted declaration. */
static bool cssd_has_math_function(const char *value)
{
    const char *p;

    if (!value) return false;
    for (p = value; *p; p++) {
        const char *start;

        if (*p != '(') continue;
        start = p;
        /* css-syntax-3 §4.3.4's ident sequence, scanned backwards from the parenthesis. `-` and `_` are ident code points
           and a digit is one after the first, which is why the run is taken by CHARACTER CLASS and not by an
           `isalpha` that would cut `atan2` in half and ask about `atan`. */
        while (start > value &&
               (isalnum((unsigned char)start[-1]) || start[-1] == '-' || start[-1] == '_')) start--;
        if (start < p && css_math_is_function(start, (size_t)(p - start))) return true;
    }
    return false;
}

/* IS THIS A DECLARATION AFTER ALL — asked only of a declaration lexbor's own grammar refused, and answering
   TRUE only for a value a LATER LEVEL OF CSS defines than the grammar that refused it. Two forms qualify and
   each is named by a specification rather than recognised by a pattern; anything else keeps lexbor's verdict.
   THE SECOND ARM IS SPLIT BY WHERE THE NUMERIC PRODUCTION STANDS IN THE GRAMMAR, because that is what decides
   whether this file can judge the value. A CSS_NUMERIC_WHOLE property's grammar makes the numeric production
   the ENTIRE value, so `css_math_matches` — which parses one math function and requires the stream to end
   after it — decides it in full. A CSS_NUMERIC_COMPONENT property's value is a SEQUENCE of component values
   whose grammar says which position is which, and that grammar lives in the component that owns the shorthand;
   a value that is nonetheless one lone math function is still decided here, because a `{1,4}` repetition or an
   omitted optional makes the one-component spelling legal (`margin: calc(1rem)`, `text-indent: calc(2em)`). */
static bool cssd_undef_is_declaration(const char *name, const char *value)
{
    CssNumericShape shape;
    unsigned prods, p;

    DCHECK(name != NULL && value != NULL,
           "a refused declaration was re-judged with no property name or no value — lexbor keeps both on the "
           "`__UNDEF` it converts one into (the real property id, and the raw source span of the value), so an "
           "absent one is a caller that lost it rather than a declaration that never had it");
    /* css-cascade-5 §7.3 "Explicit Defaulting": "As specified in CSS Values and Units, all CSS properties can
       accept these values." The vendored grammar carries `initial`/`inherit`/`unset` and css-cascade-5 §7.3.4
       "Rolling Back Cascade Origins: the revert keyword"'s `revert`, and predates css-cascade-5 §7.3.5
       "Rolling Back Cascade Layers: the revert-layer keyword" and css-cascade-5 §7.3.6 "Rolling Back Rules:
       the revert-rule keyword", so which CSS-wide keywords survive its parse depends on which properties it
       happens to type — a wrong answer per property rather than a missing capability. */
    if (css_wide_keyword(value)) return true;
    if (!cssd_has_math_function(value)) return false;
    shape = css_property_numeric(name, &prods);
    /* The grammar names no numeric production anywhere, so a math function is not a value of this property and
       css-syntax-3 §5.5.6 drops the declaration. This is a real answer and not an absence — see the table. */
    if (shape == CSS_NUMERIC_NONE) return false;
    /* A shorthand core/css/css_shorthand.h expands: its own grammar splits the value into component values and
       validates each against the longhand it sets, which is the judgement this file cannot make. It is routed
       there whole, and a component outside its grammar drops the declaration exactly as one that carried no
       math function does. */
    if (shape == CSS_NUMERIC_COMPONENT && css_shorthand_is_shorthand(name)) return true;
    /* css-values-4 §10.9's last rule asked once per production the grammar names, because a grammar spelled
       with `|` names as many as it has numeric branches and a math function is valid there when its type
       matches ANY of them (css-inline-3 §5.1's `line-height` is a `<number>` OR a `<length-percentage>`, and
       CSS Typed OM 1 §4.3.2 "Numeric Value Typing"'s algebra — which css-values-4 §10.9 links to by name —
       makes those disjoint). The loop is bounded by the MASK rather than by the last enum member, so a production
       added to CssMathProduction after `CSS_MATH_PROD_LENGTH_PERCENTAGE` cannot silently fall outside it. */
    for (p = 0; (prods >> p) != 0; p++)
        if ((prods & CSS_NUMERIC_BIT(p)) && css_math_matches(value, strlen(value), (CssMathProduction)p))
            return true;
    /* THE VALUE CARRIES A MATH FUNCTION AND IS NOT ONE THAT MATCHES, and the two reasons for that are a page's
       mistake and an engine gap — so they are told apart rather than sharing an answer.
       For a CSS_NUMERIC_WHOLE property there is only the first: its grammar makes the numeric production the
       ENTIRE value, so a second component value, or a type css-values-4 §10.9 resolves to a production the
       property does not name, is out of the grammar and css-syntax-3 §5.5.6 drops it.
       For a CSS_NUMERIC_COMPONENT property, a value that IS one lone math function is likewise just invalid —
       `text-indent: calc(1s)` is a math function whose type is a `<time>`, and no position in that grammar
       takes one. It is only a value that is NOT a lone math function, and yet contains one, that this file
       cannot judge; that is the crash below, and `css_math_is_lone_function` is what keeps a page's typo out of
       it (it answers TRUE for a math function whose type is FAILURE, which `css_math_matches` cannot). */
    if (shape == CSS_NUMERIC_WHOLE || css_math_is_lone_function(value, strlen(value))) return false;
    DFAIL("a declaration lexbor refused carries a MATH FUNCTION as ONE COMPONENT of a multi-component value, "
          "for a property whose grammar puts a numeric production in a component position and which "
          "core/css/css_shorthand.h does not expand — `text-indent: calc(2em) hanging` (css-text-3 §8.1's "
          "\"[ <length-percentage> ] && hanging? && each-line?\"), `font-style: oblique calc(10deg)` "
          "(css-fonts-4 §2.4), `flex: 1 1 calc(100% - 10px)` (css-flexbox-1 §7.1), `border-image-width: "
          "calc(1px) 2px` (css-backgrounds-3 §5.3 'Drawing Areas: the border-image-width property'). Every one "
          "of those is VALID CSS and dropping it would be the "
          "same silent initial value this whole path exists to stop, so it crashes instead. WHAT IS MISSING is "
          "a split of a declaration's value into CSS Syntax 3 §5.5.8 \"Consume a component value\"'s component "
          "values, and a per-property grammar that says which production each POSITION takes — the second half "
          "is the real work, and core/css/css_shorthand.h already holds it for the shorthands it expands, "
          "which is exactly why those are routed there one branch above rather than reaching here. BUILD that "
          "positional grammar for the properties this table marks CSS_NUMERIC_COMPONENT, or expand the "
          "shorthand in css_shorthand.c so this name takes the branch above; a value whose math components "
          "match is NOT enough on its own, because the components that are NOT math functions would then "
          "enter the block with nothing having validated them");
    return false;
}

/* IS THIS DECLARATION IN THIS RULE'S BLOCK AT ALL — the three restrictions css_style_declaration.h names,
   reached through the ONE question every declaration is asked, so a rule type is either restricted here or
   restricted nowhere.
   A CUSTOM PROPERTY REACHES THE TWO SPECIFICATIONS DIFFERENTLY, and each arm says which. CSS Paged Media's
   Appendix A is a list of CSS 2.1 PROPERTIES, so it cannot name `--x` and must not be asked about one (its own
   entry asserts that); CSS Animations' refused set cannot name one either, so the name half admits it — but
   that spec's other half is about the DECLARATION's importance, which applies to a custom property like any
   other. */
static bool cssd_block_admits(CssomBlockContext context, const char *name, bool important)
{
    bool custom = name[0] == '-' && name[1] == '-';

    switch (context) {
    case CSSOM_BLOCK_PAGE:
        return custom || css_page_property_applies(CSS_PAGE_CONTEXT_PAGE, name);
    case CSSOM_BLOCK_MARGIN:
        return custom || css_page_property_applies(CSS_PAGE_CONTEXT_MARGIN, name);
    case CSSOM_BLOCK_KEYFRAME:
        return css_keyframes_declaration_applies(name, important);
    default:
        DCHECK(context == CSSOM_BLOCK_UNRESTRICTED,
               "a declaration block was collected in a context css_style_declaration.h does not declare — the "
               "enum IS the list of specifications that restrict a block, so a fourth value is a restriction "
               "with no rule behind it");
        return true;
    }
}

/* THE LONGHAND DECLARATIONS ONE DECLARATION PRODUCES, collected into the block. A declaration sets a longhand
   either by BEING it or by being a SHORTHAND of it, and CSS Syntax drops an INVALID declaration whole — so a
   shorthand whose value fails one component's grammar sets none of them, rather than the ones before it. */
static void cssd_decls_collect_declaration(CssDecls *d, const char *name, const char *value, bool important)
{
    const char *const *lh;
    char *values[CSS_SHORTHAND_MAX_LONGHANDS];
    unsigned n, i;

    /* The rule's own restriction, asked of the DECLARATION AS WRITTEN and before the expansion below. Before
       it, because a shorthand a restriction admits expands to longhands it may not name — `text-decoration` is
       a page property and `text-decoration-line` is a CSS 3 longhand no CSS 2.1 list can carry — so filtering
       the expansion would delete the very declaration the spec admits. It runs in the other direction too:
       `animation` is refused inside a keyframe by NAME, so none of the eight longhands it expands to is ever
       reached — including the one `animation-timing-function` that css-animations-1 §3 "Declaring Keyframes"
       admits as a declaration of its own. */
    if (!cssd_block_admits(d->context, name, important)) return;
    lh = css_shorthand_longhands(name, &n);
    if (!lh) {
        /* THE DECLARATION IS THE LONGHAND, and its value has been through a grammar only if lexbor's registry
           TYPES it. For the properties it does not carry, css_shorthand.h owns that grammar — the same one its
           shorthand expansion applies to each component — and an invalid value is a DROPPED declaration. */
        char *v;

        if (!value || !css_shorthand_validates_longhand(name)) {
            cssd_decls_collect(d, name, value ? cssd_strdup(value) : NULL, important);
            return;
        }
        v = css_shorthand_longhand_value(name, value);
        if (v) cssd_decls_collect(d, name, v, important);
        return;
    }
    CHECK(n <= CSS_SHORTHAND_MAX_LONGHANDS,
          "cssom: a shorthand's longhand list outgrew the array its expansion is collected through");
    /* A shorthand with NO VALUE matches no shorthand's grammar — every one of them names at least one
       component — so it sets nothing, exactly as an out-of-grammar value does. */
    for (i = 0; i < n; i++) {
        values[i] = value ? css_shorthand_component(name, value, lh[i]) : NULL;
        if (!values[i]) break;
    }
    if (i < n) {
        while (i > 0) free(values[--i]);
        return;
    }
    for (i = 0; i < n; i++) cssd_decls_collect(d, lh[i], values[i], important);
}

/* §6.6's PARSE A CSS DECLARATION BLOCK, from what lexbor's parser produced: every declaration expanded to its
   longhands and collapsed to one per property. This is the ONE builder — the serialization, `length`, `item`,
   every property read and every write go through it, so no two of them can disagree about what the block
   declares. */
static void cssd_decls_from_list(const lxb_css_rule_declaration_list_t *list, CssDecls *out)
{
    const lxb_css_rule_t *r;

    for (r = list ? list->first : NULL; r; r = r->next) {
        const lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(r);
        char *name, *value;

        if (r->type != LXB_CSS_RULE_DECLARATION) continue;
        /* CSS Syntax's INVALID DECLARATION is not in the block, so it is not in the block's declarations
           either. Lexbor keeps one in the list as a `__UNDEF` holding the property id and the RAW UNPARSED
           TOKENS so that a serializer can round-trip the block it came from — and a cascade that read it back
           without this test handed those tokens on as if they were a value: `display: bogus` won the cascade
           and `getComputedStyle(el).display` answered "bogus", a string no property's grammar admits.
           EXCEPT WHEN THOSE TOKENS ARE A CSS-WIDE KEYWORD, WHICH IS A VALID DECLARATION OF EVERY PROPERTY.
           css-cascade-5 §7.3: "As specified in CSS Values and Units, ALL CSS PROPERTIES CAN ACCEPT THESE
           VALUES." Lexbor's value grammar carries `initial`, `inherit`, `unset` and `revert` and predates
           css-cascade-5 §7.3.5's `revert-layer` and css-cascade-5 §7.3.6's `revert-rule`,
           so `height: revert-layer` fails that grammar and arrives here as an invalid declaration
           while `translate: revert-layer` — a property the grammar does not type at all — arrives as a value.
           Dropping the first would make a CSS-wide keyword mean something different depending on which
           properties the vendored parser happens to know, which is a wrong answer per property rather than a
           missing capability. `undef->type` carries the real property id and `undef->value` the raw source
           span (the `!important` is a separate offset and is already on the declaration), so both halves of
           the declaration survive.
           AND THE SAME IS TRUE OF A MATH FUNCTION, FOR THE SAME REASON ONE LEVEL WIDER. The CSS-wide keyword
           arm was written because a value's validity was being decided by which properties the vendored parser
           happens to type; a `calc()` is that defect at the scale of the whole modern web, because the parser
           types `width`, `height` and `font-size` and has no math functions at all. Both arms are one
           question — is this a value a LATER LEVEL OF CSS defines than the grammar that refused it — and
           cssd_undef_is_declaration is where it is asked. */
        if (d->type == LXB_CSS_PROPERTY__UNDEF) {
            char *raw = cssd_decl_value(d);

            name = raw ? cssd_decl_name(d) : NULL;   /* NULL when lexbor has no id for the property either */
            if (name && cssd_undef_is_declaration(name, raw))
                cssd_decls_collect_declaration(out, name, raw, d->important);
            free(name);
            free(raw);
            continue;
        }
        name = cssd_decl_name(d);
        if (!name) continue;
        value = cssd_decl_value(d);   /* the trimmed value — see its note */
        cssd_decls_collect_declaration(out, name, value, d->important);
        free(name);
        free(value);
    }
}

/* The same, from the TEXT a backing keeps — which is where every block this engine has keeps its declarations,
   for the reason the file header gives: the backing is what time-travels. */
static void cssd_decls_from_text(const char *text, size_t len, CssDecls *out)
{
    lxb_css_memory_t *mem = NULL;

    if (!text || !len) return;
    cssd_decls_from_list(cssd_parse_block(text, len, &mem), out);
    if (mem) lxb_css_memory_destroy(mem, true);
}

/* The element's inline `style` attribute, as text. BORROWED from Lexbor's own storage. */
static const char *cssd_inline_text(lxb_dom_element_t *el, size_t *plen)
{
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"style", 5, plen);
    return (const char *)v;
}

/* THE VALUE A DECLARATION BLOCK'S TEXT GIVES ONE PROPERTY, and whether that declaration carries `!important`.
   NULL when the block declares it nowhere. `name` is a property in its own right — a longhand or a shorthand
   the table does not record — because a recorded shorthand is not IN the declarations and is answered from its
   longhands by §6.6.1's own steps one function along. The two backings differ in WHERE the text is kept and in
   nothing else, so the cascade's inline layer and §6.6.1's members share this. OWNED. */
static char *cssd_value_in_block(const char *text, size_t len, const char *name, bool *pimportant)
{
    CssDecls d = { 0 };
    char *out = NULL;
    int at;

    cssd_decls_from_text(text, len, &d);
    at = cssd_decls_index(&d, name);
    /* THE BLOCK DECLARES A PROPERTY AT MOST ONCE, so there is no "last wins" left to do here: which of several
       declarations of one property survives is decided where they are COLLECTED, by the cascade's own two
       criteria (importance, then order), and this is the one answer that came out. */
    if (at >= 0 && d.v[at].value) {
        out = cssd_strdup(d.v[at].value);
        if (pimportant) *pimportant = d.v[at].important;
    }
    cssd_decls_free(&d);
    return out;
}

char *cssom_declared_value(const char *text, size_t len, const char *name)
{
    DCHECK(g_ready, "a declaration block was read before cssom_init built the parser it goes through");
    DCHECK(name != NULL, "a declaration block was asked what it declares for nothing");
    return cssd_value_in_block(text, len, name, NULL);
}

/* §6.6.1's GET PROPERTY VALUE over a block's text, including the step that is only reachable now that the
   longhand->shorthand direction exists: "if property is a shorthand property ... for each longhand property
   longhand that property maps to, in canonical order ... if declaration is null, then return the empty string
   ... if important flags of all declarations in list are same, then return the serialization of list."
   IT IS THE SAME WALK FOR BOTH BACKINGS AND FOR A SHORTHAND SPELLED EITHER WAY, because the declarations it
   reads are LONGHANDS whichever spelling produced them. So `border` reads back out of a block holding
   `border: 1px solid red` (all seventeen longhands answer, and consolidate to what was written) and reads back
   as the EMPTY STRING out of one holding only `border-width`/`border-style`/`border-color` (the five
   `border-image` longhands answer nothing, and css-backgrounds-3 §3.4's `border` is the shorthand that resets
   them) — which is
   exactly the split WPT's border-shorthand-serialization.html pins.
   OWNED, NULL when the block gives the property no value. */
static char *cssd_property_value(const char *text, size_t len, const char *name)
{
    const char *const *lh;
    const char *values[CSS_SHORTHAND_MAX_LONGHANDS];
    CssDecls d = { 0 };
    unsigned n, i;
    bool important = false, ok = true;
    char *out = NULL;
    int at;

    lh = css_shorthand_longhands(name, &n);
    if (!lh) return cssd_value_in_block(text, len, name, NULL);
    CHECK(n <= CSS_SHORTHAND_MAX_LONGHANDS,
          "cssom: a shorthand's longhand list outgrew the array §6.6.1's getPropertyValue sized from it");
    cssd_decls_from_text(text, len, &d);
    for (i = 0; i < n && ok; i++) {
        at = cssd_decls_index(&d, lh[i]);
        /* "If declaration is null, then return the empty string", and "if important flags of all declarations
           in list are same, then return the serialization of list" — otherwise the empty string. */
        if (at < 0 || !d.v[at].value) { ok = false; break; }
        if (i == 0) important = d.v[at].important;
        else if (d.v[at].important != important) { ok = false; break; }
        values[i] = d.v[at].value;
    }
    if (ok) out = css_shorthand_serialize_value(name, (const char *const *)values);
    cssd_decls_free(&d);
    return out;
}

/* §6.6.1's GET PROPERTY PRIORITY, whose shorthand step is the same shape with the values thrown away: "for
   each longhand property longhand that property maps to, append the result of invoking getPropertyPriority()
   with longhand as argument to list. If all items in list are the string 'important', then return the string
   'important'." A longhand the block does not declare has no priority, so the answer is not "important". */
static bool cssd_property_important(const char *text, size_t len, const char *name)
{
    const char *const *lh;
    CssDecls d = { 0 };
    unsigned n, i;
    bool all = true;

    lh = css_shorthand_longhands(name, &n);
    if (!lh) {
        bool imp = false;
        char *v = cssd_value_in_block(text, len, name, &imp);
        bool got = v != NULL && imp;

        free(v);
        return got;
    }
    cssd_decls_from_text(text, len, &d);
    for (i = 0; i < n && all; i++) {
        int at = cssd_decls_index(&d, lh[i]);

        all = at >= 0 && d.v[at].value != NULL && d.v[at].important;
    }
    cssd_decls_free(&d);
    return all;
}

/* css-cascade-5 §6.1's ELEMENT-ATTACHED declaration for `name`, or NULL — the contents of the style attribute,
   which is per-flow because the attribute it reads is. The CASCADE reaches it with an element and no
   declaration object, which is why this takes one rather than a block, and it reports the IMPORTANCE because
   css-cascade-5 §6.1 compares that first: attached-ness is the criterion BELOW origin and importance, not
   above it. */
static char *cssd_inline_value(lxb_dom_element_t *el, const char *name, bool *pimportant)
{
    size_t len = 0;
    const char *text = cssd_inline_text(el, &len);

    return cssd_value_in_block(text, len, name, pimportant);
}

/* THE AUTHOR ORIGIN's declarations, collected from THE SHEET OBJECTS of the element's root.
 *
 * IT USED TO RE-WALK THE `<style>` ELEMENTS AND RE-PARSE THEIR TEXT, AND THAT WAS TWO MECHANISMS DESCRIBING ONE
 * FACT — the one that answered questions was not the one the page mutated. CSSOM §6.1's sheets, CSSOM §6.4's
 * rules and their §6.6 declaration blocks are the document's style; the elements are where they came FROM. So
 * `insertRule('p{color:red}')` changed `cssRules` and left `getComputedStyle` alone, `deleteRule` deleted
 * nothing anybody could see, `rule.selectorText = '#other'` retargeted a rule that still matched the old
 * selector, `rule.style.color = 'red'` was inert, and `sheet.disabled = true` disabled a sheet that went on
 * styling the page. Every one of those is now a real change to what the cascade resolves, because the cascade
 * reads the objects the page holds.
 *
 * A SHEET'S TEXT IS REBUILT FROM ITS RULES, not kept beside them. The rules are the authority (css_rule.h: a
 * rule is TEXT because a lexbor arena has no cross-tier identity), so the sheet's serialization is derived
 * from them per read and handed to lexbor for the SELECTOR MATCHER — which needs a parsed selector, and which
 * is the one thing the objects cannot carry. The parse is asserted against the emission AT EVERY INDEX, so a
 * rule whose stored text does not round-trip crashes here instead of silently shifting every rule after it.
 *
 * WHAT THE TEXT CANNOT CARRY IS THE CASCADE LAYER, so the emission reports one per rule beside it.
 * css-cascade-5 §6.1 puts
 * css-cascade-5 §6.4's Layers criterion ABOVE Specificity, which is why flattening a `@layer` block's children into the
 * sheet is a WRONG answer and not an approximate one — and why the index correspondence above is load-bearing
 * rather than a tidiness check: the layer is looked up BY the rule's position in the re-parse.
 *
 * IT IS REDONE PER READ ON PURPOSE, exactly as the element walk was: a cache would be shared state the flow
 * machinery does not swap, so a rule one arm inserted would decide another arm's computed values. Reading the
 * OBJECTS is what makes that per-flow rather than merely uncached — the sheet list is an Array on the root's
 * wrapper and the rules are an Array on the sheet's record, so both are the running flow's own.
 *
 * THE MATCH ITSELF IS core/dom/selector_match.c's, which is where the agent's one lxb_selectors_t lives. This
 * file held a second one, so "does this element match this selector" had two implementations that could
 * disagree — and the arena is scratch either way: the match cleans its own pools before it returns. */

/* The sheet's serialization, rebuilt from its RULE OBJECTS, with the css-cascade-5 §6.4.3 "Layer Ordering"
   cascade layer of every rule that went into it — NOT CSSOM §6.4.3, which is The CSSStyleRule Interface and is
   what a §6.4.3 means everywhere else in this file.
   THE FLATTENING IS core/css/css_rule.h's, because deciding which rules apply is CSSOM §6.4's business
   and not this file's: a conditional group rule contributes its children only when its condition holds, so what
   the matcher re-parses is not the sheet's top level at all — and neither is the layer a rule is in something a
   flat text could carry. */
static bool cssd_sheet_view(JSContext *ctx, JSValueConst sheet, CssLayerOrder *order, CssRuleCascadeSheet *out)
{
    JSValue rules = css_style_sheet_rules(ctx, sheet);
    bool ok = css_rule_cascade_sheet(ctx, rules, order, out);

    JS_FreeValue(ctx, rules);
    return ok;
}

/* EVERY AUTHOR-ORIGIN DECLARATION OF `name` ON `el`, added to `cascade` — not the one that wins. css-cascade-5
   §6.1's sort is over the whole list at once and css-cascade-5 §7.3's roll-backs re-run it with a part removed,
   so a collector that kept only a running best would have thrown away exactly what both need. `*pseq` is the
   document-order counter css-cascade-5 §6.1's Order of Appearance reads, carried across the sheets so it is one
   sequence and not one per sheet, and bumped per RULE so it also names the rule css-cascade-5 §7.3.6's
   `revert-rule` removes. */
static void cssd_author_collect(lxb_dom_element_t *el, const char *name, CssCascade *cascade,
                                CssLayerOrder *order, uint32_t *pseq)
{
    lxb_dom_node_t *self = lxb_dom_interface_node(el);
    /* THE REALM IS THE ELEMENT'S OWN DOCUMENT'S, never the running one: the sheets hang off the root's WRAPPER,
       and a wrapper belongs to the realm whose record owns that document. css_length.c's viewport DFAIL asks
       for the same plumbing from the other end and names this entry. */
    JSContext *ctx = document_realm_of(self);
    JSValue sheets;
    uint32_t ns = 0, si;

    DCHECK(ctx != NULL,
           "the author cascade was asked to resolve for an element whose document has NO REALM RECORD — the "
           "author layer IS CSSOM §6.2's list of CSS style sheets, which lives on that document's root wrapper, so a "
           "document nobody built a record for has no author style at all rather than an empty one. Give the "
           "caller's document a record, or establish that this element cannot reach a computed value");
    sheets = style_sheet_list_of(ctx, node_root(self));
    if (!JS_IsArray(sheets)) { JS_FreeValue(ctx, sheets); return; }
    {
        JSValue len = JS_GetPropertyStr(ctx, sheets, "length");

        JS_ToUint32(ctx, &ns, len);
        JS_FreeValue(ctx, len);
    }
    for (si = 0; si < ns; si++) {
        JSValue sheet = JS_GetPropertyUint32(ctx, sheets, si);
        CssRuleCascadeSheet view = { NULL, NULL, 0 };
        lxb_css_memory_t *smem;
        lxb_css_stylesheet_t *sst = NULL;

        DCHECK(css_style_sheet_is(sheet),
               "CSSOM §6.2's list holds something that is not a CSS style sheet — its add is the one thing "
               "that ever puts one in");
        /* CSSOM §6.1's DISABLED FLAG — the FLAG is CSSOM §6.1's ("Either set or unset. Unset by default"), and CSSOM
           §6.1.1 The StyleSheet Interface is only where the `disabled` ATTRIBUTE that sets and unsets it is
           declared. This read is of the flag, so it cites the flag's own section; the number here was CSSOM
           §6.1.1 beside a quotation mark, and the words inside those quotation marks appear NOWHERE IN CSSOM.
           `sheet.disabled = true` and `<style disabled>`'s forwarding both land here, and until the cascade
           read the objects neither could do anything at all. */
        if (css_style_sheet_disabled(sheet)) { JS_FreeValue(ctx, sheet); continue; }
        if (!cssd_sheet_view(ctx, sheet, order, &view)) { JS_FreeValue(ctx, sheet); continue; }
        JS_FreeValue(ctx, sheet);
        /* A SHEET CAN DECLARE LAYERS AND EMIT NO RULES — `@layer a, b;` alone is a whole sheet establishing an
           order for the sheets after it — so the emptiness is tested on the emission and the walk that just
           happened has already done the part that matters. */
        if (!view.text) { css_rule_cascade_sheet_free(&view); continue; }
        smem = lxb_css_memory_create();
        if (smem && lxb_css_memory_init(smem, 128) == LXB_STATUS_OK) {
            sst = lxb_css_stylesheet_create(smem);
            lxb_css_parser_memory_set(g_parser, smem);
            if (sst && lxb_css_stylesheet_parse(sst, g_parser, (const lxb_char_t *)view.text,
                                                strlen(view.text)) != LXB_STATUS_OK)
                sst = NULL;
            lxb_css_parser_memory_set(g_parser, NULL);
            cssd_selectors_intact();
        }
        if (sst && sst->root && sst->root->type == LXB_CSS_RULE_LIST) {
            lxb_css_rule_t *r;
            uint32_t back = 0;

            for (r = lxb_css_rule_list(sst->root)->first; r; r = r->next) {
                lxb_css_rule_style_t *st = lxb_css_rule_style(r);
                lxb_css_selector_specificity_t spec = 0;
                const CssLayerNode *layer;
                CssDecls rd = { 0 };
                uint32_t at_rule = back++;
                int at;

                /* THE ROUND TRIP IS ASSERTED PER RULE, NOT PER SHEET. Every rule the emission wrote must come
                   back as exactly one rule AT THE SAME INDEX, because the index is what names its cascade
                   layer: a rule that re-parsed as two while its neighbour re-parsed as none keeps a total
                   right and shifts every layer after it, which is the silent version of the whole sheet
                   cascading in the wrong order. The layer entry is NULL for exactly the non-style rules the
                   emission writes (an `@namespace`), so the two questions answer each other. */
                DCHECK(at_rule < view.n,
                       "a CSS style sheet's rules did not ROUND-TRIP: the rule objects serialized to a text "
                       "that parses back to MORE rules than went in. A rule holds the SERIALIZATION lexbor "
                       "produced for it, so re-parsing it must yield exactly what it came from — find which "
                       "rule's text does not, and fix the serializer that wrote it");
                if (at_rule >= view.n) break;
                layer = view.layer[at_rule];
                DCHECK((r->type == LXB_CSS_RULE_STYLE) == (layer != NULL),
                       "a CSS style sheet's rules did not ROUND-TRIP IN ORDER: the rule at this index parses "
                       "back as a different KIND of rule than the one the author cascade emitted there. The "
                       "emission records a cascade layer for a style rule and nothing for an `@namespace`, so "
                       "a disagreement means the rules have shifted and every rule after this one would be "
                       "cascaded in a neighbour's layer");
                if (r->type != LXB_CSS_RULE_STYLE || !st->selector || !st->declarations) continue;
                if (!selector_match_node(self, st->selector, &spec)) continue;
                /* THE RULE'S OWN §6.6 DECLARATIONS, which is the same list its `rule.style` reports and the
                   same one an inline block is read through — expanded to longhands and collapsed to one per
                   property. A rule declaring a property twice has already been resolved by the collapse's own
                   two criteria, so what arrives here is one declaration per property — which is also why one
                   counter can be both css-cascade-5 §6.1's order of appearance and css-cascade-5 §7.3.6's
                   identity of the rule. */
                cssd_decls_from_list(st->declarations, &rd);
                at = cssd_decls_index(&rd, name);
                if (at >= 0 && rd.v[at].value)
                    css_cascade_add(cascade, CSS_ORIGIN_AUTHOR, rd.v[at].important, false, layer,
                                    (uint32_t)spec, *pseq + at_rule, rd.v[at].value);
                cssd_decls_free(&rd);
            }
            DCHECK(back == view.n,
                   "a CSS style sheet's rules did not ROUND-TRIP: the rule objects serialized to a text that "
                   "parses back to FEWER rules than went in. A rule holds the SERIALIZATION lexbor produced "
                   "for it, so re-parsing it must yield exactly what it came from — find which rule's text "
                   "does not, and fix the serializer that wrote it rather than tolerating the drift");
        }
        /* THE ARENA IS THE ONE OWNER, and it is freed outright. `lxb_css_stylesheet_create` REF-INCREMENTS the
           memory it is handed (to 2, since `lxb_css_memory_init` starts it at 1) and `lxb_css_stylesheet_destroy`
           only ref-DECREMENTS (back to 1), so that destroy frees nothing at all — one leaked arena per sheet per
           read, which the ancestor walks then multiply by the depth of the chain. The stylesheet, its rules and
           its selectors are all allocated FROM this arena, so destroying the arena is what releases them;
           nothing here outlives it (every value this function keeps is copied out). */
        if (smem) lxb_css_memory_destroy(smem, true);
        /* css-cascade-5 §6.1's Order of Appearance across SHEETS: "declarations from style sheets independently linked by the
           originating document are treated as if they were concatenated in linking order", so the next sheet's
           first rule follows this sheet's last one rather than restarting. */
        *pseq += view.n;
        css_rule_cascade_sheet_free(&view);
    }
    JS_FreeValue(ctx, sheets);
}

/* ---- CSS Syntax's "parse a stylesheet's contents", for CSSOM §6.4's rule objects --------------------------
 *
 * §6.6's SERIALIZE A CSS DECLARATION, and the BLOCK serialization its entries are joined into with a single
 * SPACE. The spec's note is the exact shape — "no whitespace appears before the first property name and no
 * whitespace appears after the final semicolon delimiter" — and it is NOT lexbor's serialization, which joins
 * with "; " and emits no trailing semicolon at all. A CSSOM member must answer CSSOM's string. */
static void cssd_append_declaration(CssBuf *out, bool *first, const char *name, const char *value,
                                    bool important)
{
    if (!*first) css_buf_add(out, " ");
    *first = false;
    css_buf_add(out, name);
    css_buf_add(out, ": ");
    /* §6.6's step 4 is conditional — "if value contains any non-whitespace characters, append value to s" — so
       a valueless declaration serializes as `name: ;`, which is what the algorithm produces. */
    if (value) css_buf_add(out, value);
    if (important) css_buf_add(out, " !important");
    css_buf_add(out, ";");
}

/* ---- §6.6's SHORTHAND CONSOLIDATION LOOP, over the declarations built above --------------------------------
 *
 * §6.6 models a block's declarations as a LIST, and its serialization walks that list twice over: once per
 * declaration, and once per shorthand that could cover a group of them. The loop needs to look BACKWARDS and
 * FORWARDS across the whole block — the shorthand's longhands may sit anywhere, and the logical-property-group
 * step asks what sits BETWEEN them — which is one of the two reasons the declarations are lifted out of
 * lexbor's arena into a CssDecls first. The other is that they are LONGHANDS: what the arena holds is what the
 * author wrote. */

/* ONE PASS OF §6.6's SHORTHAND LOOP: can `shorthand` stand for a group of `d`'s not-yet-serialized
   declarations, and if so write it and mark them. FALSE is the spec's "continue with the steps labeled
   shorthand loop" — every one of its five refusals is below, in the order the algorithm states them. */
static bool cssd_try_shorthand(const CssDecls *d, bool *done, const char *shorthand, CssBuf *out, bool *first)
{
    const char *const *lh;
    const char *values[CSS_SHORTHAND_MAX_LONGHANDS];
    unsigned at[CSS_SHORTHAND_MAX_LONGHANDS];
    unsigned n, i, j, lo, hi;
    bool important;
    char *value;

    lh = css_shorthand_longhands(shorthand, &n);
    /* ALWAYS FATAL, because the two failures it covers both read or write past the arrays sized from it: a
       name css_shorthand.c does not record as a shorthand answers with no longhands at all, and a longhand
       list that outgrew CSS_SHORTHAND_MAX_LONGHANDS overruns `values` and `at`. The name came out of
       css_shorthand_shorthands_of, so either one means the two readings of that table have come apart. */
    CHECK(lh != NULL && n >= 2 && n <= CSS_SHORTHAND_MAX_LONGHANDS,
          "cssom: §6.6's shorthand loop was handed a name with no usable longhand list");
    /* "Let longhands be an array consisting of all CSS declarations ... that are not in already serialized and
       have a property name that maps to one of the shorthand properties in shorthands", then "if not all
       properties that map to shorthand are present in longhands, continue". A declaration named by
       `shorthand`'s own list necessarily maps to it, so the two steps are one scan per longhand. */
    for (i = 0; i < n; i++) {
        unsigned found = 0;

        at[i] = d->n;
        for (j = 0; j < d->n; j++) {
            if (done[j] || strcmp(d->v[j].name, lh[i]) != 0) continue;
            at[i] = j;
            found++;
        }
        /* A BLOCK DECLARES A PROPERTY AT MOST ONCE — §6.6's own definition of one, maintained where the
           declarations are COLLECTED — so a second match is a list something built without the collapse, and
           the shorthand this loop would then write drops one of the two values with nothing to say which. */
        DCHECK(found <= 1, "a CSS declaration block holds TWO declarations of one property, so §6.6's shorthand "
                           "loop has two values for one slot. The collapse belongs to cssd_decls_collect, which "
                           "every builder of a CssDecls goes through — this list came from somewhere else");
        if (found != 1) return false;
        /* A longhand whose value is empty is a real declaration and not a hole (CSS Syntax admits it, which is
           why §6.6's step 4 is conditional) — and no shorthand's grammar has a component that can be empty, so
           there is no value the shorthand could carry that would mean this. */
        if (!d->v[at[i]].value) return false;
        values[i] = d->v[at[i]].value;
    }
    lo = hi = at[0];
    for (i = 1; i < n; i++) {
        if (at[i] < lo) lo = at[i];
        if (at[i] > hi) hi = at[i];
    }
    /* "If there are one or more CSS declarations in current longhands that have their important flag set and
       one or more with it unset, continue" — one declaration cannot carry two priorities. */
    important = d->v[at[0]].important;
    for (i = 1; i < n; i++)
        if (d->v[at[i]].important != important) return false;
    /* "If there is any declaration in declaration block in between the first and the last longhand in current
       longhands which belongs to the same logical property group, but has a different mapping logic as any of
       the longhands in current longhands, and is not in current longhands, continue."
       css-logical-1 §4 "Flow-Relative Box Model Properties" is why — the same section cssd_decls_collect's
       own note cites for the same sentence, which is what made the css-logical-1 §2 that stood here visible:
       the two members of such a pair "share a
       computed value ... determined by
       cascading the declarations of both properties together as one", so which of them came LAST decides the
       answer. Writing the shorthand would move its longhands to one position and reorder them across the
       flow-relative declaration sitting between — a different cascade for the same bytes. */
    for (j = lo + 1; j < hi; j++) {
        unsigned group;
        bool physical = true, mine = true, chosen = false;

        for (i = 0; i < n; i++)
            if (at[i] == j) { chosen = true; break; }
        if (chosen) continue;
        group = css_shorthand_logical_group(d->v[j].name, &physical);
        if (group == 0) continue;   /* a property in no group pairs with nothing, so it cannot reorder one */
        for (i = 0; i < n; i++)
            if (css_shorthand_logical_group(lh[i], &mine) == group && mine != physical) return false;
    }
    /* "Let value be the result of invoking serialize a CSS value with current longhands. If value is the empty
       string, continue with the steps labeled shorthand loop." */
    value = css_shorthand_serialize_value(shorthand, (const char *const *)values);
    if (!value) return false;
    cssd_append_declaration(out, first, shorthand, value, important);
    free(value);
    for (i = 0; i < n; i++) done[at[i]] = true;
    return true;
}

/* §6.6's SERIALIZE A CSS DECLARATION BLOCK, entire. OWNED, NULL for a block with no declarations — which is
   the spec's own note ("the serialization of an empty CSS declaration block is the empty string") and which
   CSSOM §6.4's serialize-a-CSS-rule reads as its "null if there are no such declarations". */
static char *cssd_serialize_decls(const CssDecls *d)
{
    CssBuf out = { 0 };
    bool first = true;
    bool *done;
    unsigned i;

    if (d->n == 0) return NULL;
    done = calloc(d->n, sizeof(*done));
    CHECK(done != NULL, "cssom: OOM serializing a declaration block — a dropped already-serialized mark would "
                        "write one declaration twice");
    for (i = 0; i < d->n; i++) {
        const char *sh[CSS_SHORTHAND_MAX_OF];
        unsigned nsh, s;
        bool emitted = false;

        /* "If property is in already serialized, continue with the steps labeled declaration loop." */
        if (done[i]) continue;
        /* "If property maps to one or more shorthand properties, let shorthands be an array of those shorthand
           properties, in preferred order." A property that maps to none — every shorthand, every custom
           property, and every longhand no recorded shorthand sets — answers 0 and goes straight out. */
        nsh = css_shorthand_shorthands_of(d->v[i].name, sh, CSS_SHORTHAND_MAX_OF);
        for (s = 0; s < nsh && !emitted; s++)
            emitted = cssd_try_shorthand(d, done, sh[s], &out, &first);
        if (emitted) continue;
        cssd_append_declaration(&out, &first, d->v[i].name, d->v[i].value, d->v[i].important);
        done[i] = true;
    }
    /* EVERY DECLARATION IS IN THE STRING EXACTLY ONCE — either under its own name or inside the one shorthand
       that absorbed it. The loop marks each index it visits, and a shorthand only ever absorbs indices it
       found unmarked, so a survivor here would be a declaration silently dropped from a block's serialization
       — the one failure of this algorithm that produces a plausible string rather than a crash. */
    for (i = 0; i < d->n; i++)
        DCHECK(done[i], "a declaration was left out of its own block's serialization");
    free(done);
    return out.s;
}

static char *cssd_serialize_block(const lxb_css_rule_declaration_list_t *list)
{
    CssDecls d = { 0 };
    char *out;

    cssd_decls_from_list(list, &d);
    out = cssd_serialize_decls(&d);
    cssd_decls_free(&d);
    return out;
}

/* AN AT-RULE WHOSE BODY IS DECLARATIONS, serialized — `@font-face`'s descriptors, and nothing else this build
   parses. Lexbor gives every at-rule block the same shape (a RULE LIST), and what distinguishes a descriptor
   body from a nested-rule body is what is IN it: the parser lifts a run of declarations into one
   DECLARATION_LIST child, so a body may hold several if an at-rule interrupts one run. They are ONE block —
   §6.6 says a declaration block holds one declaration per property, whichever run declared it — so all of them
   are collected before the collapse runs, rather than serialized separately and concatenated. OWNED, NULL for
   a body that declares nothing. */
static char *cssd_serialize_at_block(const lxb_css_rule_list_t *block)
{
    CssDecls d = { 0 };
    const lxb_css_rule_t *r;
    char *out;

    for (r = block ? block->first : NULL; r; r = r->next)
        if (r->type == LXB_CSS_RULE_DECLARATION_LIST)
            cssd_decls_from_list(lxb_css_rule_declaration_list(r), &d);
    out = cssd_serialize_decls(&d);
    cssd_decls_free(&d);
    return out;
}

/* The same, from the TEXT a backing keeps — which is what §6.6.1's `cssText` getter answers for both backings:
   "return the result of serializing the declarations", where the declarations are what parsing that text
   produced, NOT the bytes the page happened to write. `<div style="color:red">` therefore reads back as
   "color: red;" exactly as it does in a browser. OWNED, NULL for a block that declares nothing. */
static char *cssd_serialize_text(const char *text, size_t len)
{
    CssDecls d = { 0 };
    char *out;

    cssd_decls_from_text(text, len, &d);
    out = cssd_serialize_decls(&d);
    cssd_decls_free(&d);
    return out;
}

char *cssom_serialize_declarations(const char *text, size_t len, CssomBlockContext context)
{
    CssDecls d = { 0 };
    char *out;

    DCHECK(g_ready, "a declaration block was serialized before cssom_init built the parser it goes through");
    d.context = context;
    cssd_decls_from_text(text, len, &d);
    out = cssd_serialize_decls(&d);
    cssd_decls_free(&d);
    return out;
}

/* AN AT-RULE's OWN NAME, which is the only thing that can say WHICH at-rule this is. Lexbor recognises exactly
   three by name (`@media`, `@font-face`, `@namespace`) and every other one it parses becomes a `_CUSTOM` rule
   carrying the identifier it was written with, so the name is read from whichever of those two places holds it.
   A rule whose own grammar FAILED has been converted to `_UNDEF` and is dropped by the walk below, which never
   reaches here. Returns NULL for a type lexbor's own table has no entry for, which cannot happen.
   ASCII-LOWERCASED, AND THAT IS NOT COSMETIC. CSS Syntax makes an at-keyword ASCII case-insensitive and
   lexbor's own table lookup agrees (`lexbor_shs_entry_get_lower_static`), so `@MEDIA` arrives here as the
   table's lowercase "media" — but the `_CUSTOM` arm copies the identifier VERBATIM out of the token, so
   `@PAGE` would arrive as "PAGE" and match no builder. That was invisible while every `_CUSTOM` at-rule was
   one this engine has no interface for and crashes on anyway; it stopped being invisible when CSSOM §6.4.7's
   `@page`, which lexbor's table does not carry, became a rule. OWNED: the caller frees. */
static char *cssd_at_rule_name(const lxb_css_rule_at_t *at)
{
    const lxb_css_entry_at_rule_data_t *e;
    const char *raw;
    char *out;
    size_t i;

    if (at->type == LXB_CSS_AT_RULE__CUSTOM) raw = (const char *)at->u.custom->name.data;
    else {
        e = lxb_css_at_rule_by_id(at->type);
        raw = e ? (const char *)e->name : NULL;
    }
    if (!raw) return NULL;
    out = strdup(raw);
    CHECK(out != NULL, "cssom: OOM copying an at-rule's name");
    for (i = 0; out[i]; i++)
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    return out;
}

/* A RULE'S PRELUDE, sliced out of the source by the offsets lexbor recorded while consuming it. It is taken
   from the text rather than from the parsed value because only `_UNDEF` and `_CUSTOM` keep a copy of it at all
   — `@media`'s parsed form is its block and nothing else — and because the offsets are exactly what
   `lxb_css_make_data` itself reads: they index the buffer handed to lxb_css_stylesheet_parse, which is `text`.
   Trimmed of ASCII whitespace, because the prelude's span runs from its first token to the `{` and the parser
   that consumes it next has no use for either edge. OWNED.
   IT IS SHARED WITH THE BAD-STYLE ARM, which keeps its own copy of exactly the same span under a different
   field name: a qualified rule whose prelude is not a selector list is reported with the RAW prelude, and a
   second slicer for it would be a second chance to disagree about the trim. */
static char *cssd_prelude_span(const char *text, size_t len, size_t begin, size_t end)
{
    char *out;

    DCHECK(begin <= end && end <= len,
           "a rule's prelude offsets fall outside the text that was parsed — they index the tokenizer's "
           "input buffer, which is the very string handed to lxb_css_stylesheet_parse");
    if (begin > end || end > len) begin = end = 0;
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
                           text[begin] == '\r' || text[begin] == '\f')) begin++;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' ||
                           text[end - 1] == '\r' || text[end - 1] == '\f')) end--;
    out = malloc(end - begin + 1);
    CHECK(out != NULL, "cssom: OOM copying an at-rule's prelude");
    memcpy(out, text + begin, end - begin);
    out[end - begin] = '\0';
    return out;
}

/* One rule list, walked in document order, each rule reported and then its OWN nested list walked with the
   handle the callback just returned. The recursion is over the PARSE TREE and its depth is the nesting depth of
   the stylesheet's own braces, which the tokenizer has already bounded by consuming them. */
static void cssd_emit_rules(const char *text, size_t len, const lxb_css_rule_list_t *list,
                            CssomRuleFn cb, void *ud, void *parent, unsigned *pn)
{
    lxb_css_rule_t *r;

    for (r = list->first; r; r = r->next) {
        CssomRule out = { NULL, "", false, NULL, false };
        const lxb_css_rule_list_t *kids = NULL;
        CssBuf sel = { 0 };
        char *block = NULL, *prelude = NULL, *at_name = NULL;
        void *handle;

        switch (r->type) {
        case LXB_CSS_RULE_STYLE: {
            lxb_css_rule_style_t *st = lxb_css_rule_style(r);

            lxb_css_selector_serialize_list_chain(st->selector, css_buf_cb, &sel);
            block = cssd_serialize_block(st->declarations);
            out.prelude = sel.s ? sel.s : "";
            out.prelude_is_selectors = true;
            out.block = block ? block : "";
            out.has_block = true;
            kids = st->child;                     /* CSS Nesting's own rules, which CSSOM §6.4.5 makes `cssRules` */
            break;
        }
        /* THE SAME RULE WITH A PRELUDE LEXBOR'S SELECTOR PARSER REFUSED. It is a qualified rule and it has a
           block, so everything about it is reported exactly as a style rule's is — the ONE difference is that
           `prelude` is the raw source span rather than a serialized selector list, which is what
           `prelude_is_selectors` says and what lets the BUILDER decide whether the rule is valid in the
           context it is in. Dropping it here (which is what this arm used to do beside the declaration-list
           one) decided that question in the parser layer, where the context is not known, and made
           `@keyframes foo { 0% { } }` a keyframes rule with no keyframes in it. */
        case LXB_CSS_RULE_BAD_STYLE: {
            lxb_css_rule_bad_style_t *bad = lxb_css_rule_bad_style(r);

            prelude = cssd_prelude_span(text, len, bad->prelude_begin, bad->prelude_end);
            block = cssd_serialize_block(bad->declarations);
            out.prelude = prelude;
            out.block = block ? block : "";
            out.has_block = true;
            kids = bad->child;
            break;
        }
        case LXB_CSS_RULE_AT_RULE: {
            lxb_css_rule_at_t *at = lxb_css_rule_at(r);

            /* `_UNDEF` is what lexbor converts an at-rule to when its OWN grammar failed, and CSS Syntax says
               an invalid rule is dropped — which is what a user agent does with `@namespace { }` and why an
               invalid at-rule is not in `cssRules`. */
            if (at->type == LXB_CSS_AT_RULE__UNDEF) continue;
            at_name = cssd_at_rule_name(at);
            out.at_name = at_name;
            DCHECK(out.at_name != NULL,
                   "lexbor reported an at-rule whose own name table has no entry for its type — the name is "
                   "the only thing that can say which CSSOM §6.4 interface the rule wants");
            if (!out.at_name) continue;
            prelude = cssd_prelude_span(text, len, at->prelude_begin, at->prelude_end);
            out.prelude = prelude;
            /* Every at-rule lexbor knows keeps its block in the same place in the union, and a STATEMENT
               at-rule (`@import url(x);`) keeps a null one — which is the fact `has_block` reports. */
            kids = (at->type == LXB_CSS_AT_RULE_MEDIA) ? at->u.media->block
                 : (at->type == LXB_CSS_AT_RULE_FONT_FACE) ? at->u.font_face->block
                 : (at->type == LXB_CSS_AT_RULE__CUSTOM) ? at->u.custom->block : NULL;
            out.has_block = kids != NULL;
            /* A BODY'S DECLARATIONS ARE REPORTED FOR EVERY AT-RULE THAT HAS A BODY, and its child rules are
               walked for every at-rule too, because CSS Syntax's `<declaration-rule-list>` is a body that
               holds BOTH and CSSOM §6.4.7's `@page` is exactly one: page descriptors beside css-page-3 §4.3
               "@page rule grammar"'s margin at-rules.
               This used to name `@font-face` and take the other fork for it, which was the same statement with
               a name in it — an `@font-face`'s block simply contains no rules to walk, and a `@media`'s
               contains no declarations to report, so asking both questions of both is what makes the ANSWER
               the body's own rather than a table of at-rule names kept in the parser layer. The BUILDER
               decides which of the two a given at-rule is allowed to have; CSS Syntax drops the other. */
            if (kids) {
                block = cssd_serialize_at_block(kids);
                out.block = block ? block : "";
            }
            break;
        }
        /* A DECLARATION WHERE A RULE BELONGS, which CSS Syntax calls invalid and drops. A declaration list
           inside a STYLE rule is the one lexbor has already lifted out into that rule's own declarations, so
           what remains here is a second one AFTER a nested rule — which is CSSOM's CSSNestedDeclarations, a
           rule interface this build does not have and that the builder would have to be told about. */
        case LXB_CSS_RULE_DECLARATION_LIST:
            continue;
        default:
            DFAIL("CSS Syntax produced a top-level rule kind this walk has no arm for — lexbor's own list is "
                  "STYLE, BAD_STYLE, AT_RULE and DECLARATION_LIST, and a fifth means the parser grew a kind "
                  "whose prelude and body split differently");
            continue;
        }
        if (pn) (*pn)++;
        handle = cb(ud, parent, &out);
        if (kids) cssd_emit_rules(text, len, kids, cb, ud, handle, NULL);
        free(prelude);
        free(block);
        free(at_name);
        css_buf_free(&sel);
    }
}

unsigned cssom_parse_rules(const char *text, size_t len, CssomRuleFn cb, void *ud)
{
    lxb_css_memory_t *mem = lxb_css_memory_create();
    lxb_css_stylesheet_t *sst = NULL;
    unsigned n = 0;

    DCHECK(g_ready, "CSS text was parsed before cssom_init built the parser it goes through");
    DCHECK(text != NULL && cb != NULL, "a stylesheet parse was asked for with no text or nowhere to put it");
    CHECK(mem != NULL, "cssom: the CSS arena allocation failed");
    if (lxb_css_memory_init(mem, 128) != LXB_STATUS_OK) {
        lxb_css_memory_destroy(mem, true);
        return 0;
    }
    sst = lxb_css_stylesheet_create(mem);
    lxb_css_parser_memory_set(g_parser, mem);
    if (sst && lxb_css_stylesheet_parse(sst, g_parser, (const lxb_char_t *)text, len) != LXB_STATUS_OK)
        sst = NULL;
    lxb_css_parser_memory_set(g_parser, NULL);
    cssd_selectors_intact();
    if (sst && sst->root && sst->root->type == LXB_CSS_RULE_LIST)
        cssd_emit_rules(text, len, lxb_css_rule_list(sst->root), cb, ud, NULL, &n);
    /* THE ARENA IS THE ONE OWNER and it is freed outright, for the reason the author cascade's is: creating a
       stylesheet REF-INCREMENTS the memory it is handed, so `lxb_css_stylesheet_destroy(sst, true)` only
       decrements it back and frees nothing. Every rule, selector and string above lives in here, which is
       exactly why the callback got TEXT and why it got it before this line. */
    lxb_css_memory_destroy(mem, true);
    return n;
}

/* LAYER 4 — the UA DEFAULT. A headless run still has a user-agent stylesheet, and `display` is the property a
 * bundle actually branches on. Modelling it is the difference between answering the spec's value and shrugging;
 * what is NOT here — the rest of html.css — is honestly absent and reads as the property's initial value.
 *
 * A TAG THIS TABLE DOES NOT NAME IS `inline`, WHICH IS WHY A MISSING ROW IS NOT A MISSING ANSWER BUT A WRONG
 * ONE, AND WHY IT IS SILENT. `cssd_ua_value`'s last line is the UA sheet's own default, so an element the
 * table forgets does not reach a crash and does not read as unset — it reads as a real value that three
 * components then act on. core/dom/element_view.c's `clientWidth`/`clientHeight` step 1 is "if the element has
 * no associated box OR IF THE BOX IS INLINE, return zero", so a forgotten row makes those members answer 0 for
 * an element whose padding edge this engine can compute; `element_view_fragment_kind` reads `inline` as ONE
 * FRAGMENT PER LINE BOX, so `offsetWidth`, `offsetHeight` and `getClientRects` abort naming CSS 2 §9.4.2's
 * inline formatting context for an element that has no inline formatting context to wait on; and
 * core/layout/block_flow.c's child classification refuses to lay out any block container holding one. A zero
 * that a page compares against is CLAUDE.md §Architecture's plausible datum, and this is where it is born.
 *
 * EVERY ROW IS TRANSCRIBED FROM HTML'S RENDERING SECTION, BY NUMBER AND TITLE, AND `display` ONLY — the rest of
 * each rule's declarations are the honest absence above. The STANDARD IS WRITTEN ON EVERY ROW rather than in
 * this lead-in, because a citation is checkable only where its standard is inside the forty characters before
 * the `§` — see the convention at the head of this file — and a list is exactly the shape where one name at
 * the top covers none of the numbers under it. The sections are:
 *   HTML §15.3.1  Hidden elements                  — the fourteen-element `display: none` rule
 *   HTML §15.3.2  The page                         — `html, body { display: block }`
 *   HTML §15.3.3  Flow content                     — the block-level flow rule, and `slot { display: contents }`
 *   HTML §15.3.6  Sections and headings            — `article, aside, :heading, hgroup, nav, section`
 *   HTML §15.3.7  Lists                            — `dir, dd, dl, dt, menu, ol, ul`, and `li { display: list-item }`
 *   HTML §15.3.8  Tables                           — the nine table box types
 *   HTML §15.3.10 Form controls                    — `input, button { display: inline-block }`
 *   HTML §15.3.12 The fieldset and legend elements — `fieldset { display: block }`
 *   HTML §15.5.5  The details and summary elements — `details, summary { display: block }`
 *   HTML §15.5.13 The marquee element              — `marquee { display: inline-block }`
 *   HTML §15.5.16 The select element               — `select { display: inline-block }`, `option`, `optgroup`
 *   HTML §15.5.17 The textarea element             — its prose, "expected to render as an 'inline-block' box"
 * HTML §15.3.6 selects the headings with `:heading` rather than by name; the six elements that pseudo-class matches
 * in an HTML document are the six named here, and there is no seventh element for a row to be missing.
 *
 * WHAT A TYPE-SELECTOR TABLE CANNOT EXPRESS IS NAMED RATHER THAN APPROXIMATED, and each one is a rule whose
 * SELECTOR is not a tag: HTML §15.5.5's `details > summary:first-of-type { display: list-item }` gives the FIRST
 * summary of a details a marker, which the row below reads as plain `block` — the two are both block-level
 * block containers and differ in the marker box alone, so the deviation is in the marker and not in the box
 * type, and closing it needs a structural selector this layer does not evaluate. `:heading(n)`'s font sizes and
 * margins, HTML §15.3.3's margins and HTML §15.3.7's list-style rules are the same absence stated once above.
 *
 * AND HTML §15.3.4 Phrasing content's `ruby { display: ruby }` / `rt { display: ruby-text }` ARE DELIBERATELY
 * ABSENT, which is the one place adding a row would make this engine WORSE rather than more complete, and the
 * test is the one this
 * whole comment is about. Every consumer of a computed `display` reads `inline` as a box it declines to
 * measure — core/dom/element_view.c's step 1 answers zero, its fragment count aborts naming CSS 2.1 §9.4.2 — while
 * `ruby` and `ruby-text` are box types NO consumer has an arm for: core/layout/used_value.c's `uv_box_kind`
 * would classify one as BLOCK FLOW and hand CSS 2.1 §10.3.3's constraint equation a box CSS 2.1 §10 does not
 * describe,
 * so `clientWidth` would stop being zero and start being a real number computed for the wrong box. A wrong
 * number is worse than the wrong-but-declined answer it replaces. These two rows land in the same diff that
 * gives CSS Ruby a box type in `uv_box_kind` and in `element_view_fragment_kind`.
 *
 * THE `@namespace "http://www.w3.org/1999/xhtml"` AT THE HEAD OF EVERY ONE OF THOSE RULES IS NOT HONOURED HERE
 * and the lookup is by LOCAL NAME alone. It is a real divergence and it is stated rather than assumed away: an
 * SVG `<title>` and an SVG `<style>` take the HTML rows above, which happens to be the answer SVG's own UA
 * sheet gives them, and no row added here collides with an SVG or MathML local name. Honouring it means adding
 * the SVG UA sheet in the same diff — a namespace test alone would take `display: none` AWAY from those two
 * and generate a box for them, which is strictly worse than the divergence it removes. */
static const struct { const char *tag; const char *prop; const char *value; } UA_DEFAULT[] = {
    /* HTML §15.3.2 The page */
    { "html", "display", "block" },  { "body", "display", "block" },
    /* HTML §15.3.3 Flow content: `address, blockquote, center, dialog, div, figure, figcaption, footer, form,
       header, hr, legend, listing, main, p, plaintext, pre, search, xmp { display: block }`. `dialog` is here
       for the box it has when it is OPEN; `dialog:not([open])` is an attribute selector and is applied with
       the other attribute-conditional rules below. */
    { "address", "display", "block" }, { "blockquote", "display", "block" },
    { "center", "display", "block" }, { "dialog", "display", "block" },
    { "div", "display", "block" },   { "figure", "display", "block" },
    { "figcaption", "display", "block" }, { "footer", "display", "block" },
    { "form", "display", "block" },  { "header", "display", "block" },
    { "hr", "display", "block" },    { "legend", "display", "block" },
    { "listing", "display", "block" }, { "main", "display", "block" },
    { "p", "display", "block" },     { "plaintext", "display", "block" },
    { "pre", "display", "block" },   { "search", "display", "block" },
    { "xmp", "display", "block" },
    /* HTML §15.3.3's `slot { display: contents }` — a box type, and the one in this table that generates NO
       box: css-display-3 §2.5 "Box Generation: the none and contents keywords" keeps the element's children and
       drops its own box ("the element itself does not generate any boxes, but its children ... do"), which
       core/dom/element_view.c's one box predicate and core/css/css_computed_value.c's box-parent walk both
       already read. The number here was css-display-3 §3.1, which is that module's "Reordering and
       Accessibility". */
    { "slot", "display", "contents" },
    /* HTML §15.3.6 Sections and headings */
    { "article", "display", "block" }, { "aside", "display", "block" },
    { "h1", "display", "block" },    { "h2", "display", "block" },
    { "h3", "display", "block" },    { "h4", "display", "block" },
    { "h5", "display", "block" },    { "h6", "display", "block" },
    { "hgroup", "display", "block" }, { "nav", "display", "block" },
    { "section", "display", "block" },
    /* HTML §15.3.7 Lists */
    { "dir", "display", "block" },   { "dd", "display", "block" },
    { "dl", "display", "block" },    { "dt", "display", "block" },
    { "menu", "display", "block" },  { "ol", "display", "block" },
    { "ul", "display", "block" },    { "li", "display", "list-item" },
    /* HTML §15.3.8 Tables — all nine box types, because a `<tbody>` reading `inline` is not a table this engine
       cannot lay out, it is a box CSS 2 §9.2 says exists nowhere in a table. */
    { "table", "display", "table" }, { "caption", "display", "table-caption" },
    { "colgroup", "display", "table-column-group" }, { "col", "display", "table-column" },
    { "thead", "display", "table-header-group" }, { "tbody", "display", "table-row-group" },
    { "tfoot", "display", "table-footer-group" }, { "tr", "display", "table-row" },
    { "td", "display", "table-cell" }, { "th", "display", "table-cell" },
    /* HTML §15.3.10 Form controls, HTML §15.3.12 The fieldset and legend elements, and HTML §15.5's widget sections. An
       `<input>` is the single most measured element on the web and `input.clientWidth` was zero for every one
       of them. */
    { "input", "display", "inline-block" }, { "button", "display", "inline-block" },
    { "fieldset", "display", "block" },
    { "details", "display", "block" }, { "summary", "display", "block" },
    { "marquee", "display", "inline-block" },
    { "select", "display", "inline-block" }, { "textarea", "display", "inline-block" },
    { "option", "display", "block" }, { "optgroup", "display", "block" },
    /* HTML §15.3.1's FIRST RULE, entire: `area, base, basefont, datalist, head, link, meta, noembed, noframes,
       param, rp, script, style, template, title { display: none }`. Seven of the fourteen used to be here and
       seven were not, which is not a smaller stylesheet — it is a `<datalist>` this engine says generates a
       box, and every CSSOM View §6 Extensions to the Element Interface geometry member and HTML's `being
       rendered` reading that answer. */
    { "area", "display", "none" },   { "base", "display", "none" },
    { "basefont", "display", "none" }, { "datalist", "display", "none" },
    { "head", "display", "none" },   { "link", "display", "none" },
    { "meta", "display", "none" },   { "noembed", "display", "none" },
    { "noframes", "display", "none" }, { "param", "display", "none" },
    { "rp", "display", "none" },     { "script", "display", "none" },
    { "style", "display", "none" },  { "template", "display", "none" },
    { "title", "display", "none" },
    /* HTML §15.3.1's LAST RULE, `@media (scripting) { noscript { display: none !important } }`. It is here as an
       ordinary row and not among the conditional rules below because its condition is not about the ELEMENT:
       core/css/media_query.c answers `scripting` with `enabled`, on the ground that this engine runs the
       page's scripts in the document's own realm, so the media query is true for every document this table can
       be asked about. Its `!important` is carried by cssd_ua_important_display — a `<noscript>` whose contents
       the parser took as TEXT because scripting is enabled is exactly the element an author rule must not be
       able to make visible. */
    { "noscript", "display", "none" },
};

/* HTML §15.3.1's `hidden` RULES, which are ATTRIBUTE selectors and therefore outrank every type selector in the
   table above — so they are asked first, and they are asked HERE rather than by each component that wants to
   know whether an element generates a box. element_view.c walked the ancestor chain for the attribute itself,
   which is the same rule implemented a second time and implemented WRONG in two ways this one is not: it read
   `hidden="until-found"` (which HTML §15.3.1 makes `content-visibility: hidden`, a rendered element) as a removed
   box, and it ignored `embed[hidden]`; and being outside the cascade it could not be overridden by the author
   rule that outranks it, so a page's own `[hidden] { display: block }` did nothing.
     [hidden]:not([hidden=until-found i]):not(embed) { display: none }
     embed[hidden] { display: inline; height: 0; width: 0 } */
/* An attribute selector's `i` FLAG, over a raw attribute value: Selectors §6.3's ASCII case-insensitive match.
   `want` is lowercase ASCII and NUL-terminated; `v`/`vlen` are the attribute's own bytes and are neither. It is
   a function because four of the rules above and below carry the flag and each hand-rolled copy is one more
   place `type=HIDDEN` can be read as a different value than `type=hidden`. */
static bool cssd_attr_is_ascii_ci(const lxb_char_t *v, size_t vlen, const char *want)
{
    size_t i, n = strlen(want);

    if (v == NULL || vlen != n) return false;
    for (i = 0; i < n; i++)
        if ((char)tolower((unsigned char)v[i]) != want[i]) return false;
    return true;
}

static const char *cssd_ua_hidden(lxb_dom_element_t *el, const lxb_char_t *tag, size_t taglen)
{
    size_t vlen = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"hidden", 6, &vlen);

    if (!v) return NULL;
    if (taglen == 5 && memcmp(tag, "embed", 5) == 0) return "inline";
    /* `until-found` sets content-visibility, and the box stays */
    if (cssd_attr_is_ascii_ci(v, vlen, "until-found")) return NULL;
    return "none";
}

/* THE OTHER TWO UA RULES WHOSE SELECTOR IS AN ATTRIBUTE AND WHOSE PROPERTY IS `display`, each one a box an
 * element HAS OR HAS NOT depending on a content attribute, which is why neither can be a row of the type-name
 * table above and why leaving them out is a box that should not exist rather than a value that is missing:
 *
 *   HTML §15.3.1 Hidden elements: `input[type=hidden i] { display: none !important }`. Its IMPORTANCE is part of the
 *   rule and is reported, not dropped — CSS Cascade §6.3 puts an important user-agent declaration above every
 *   author declaration, which is the whole point of writing it that way: a page's own `input { display: block }`
 *   must not give a hidden input a box. Reported through `*important` for that reason and because the value is
 *   the same `none` the normal rules produce, so the flag is the only thing that carries the difference.
 *
 *   HTML §15.3.3 Flow content: `dialog:not([open]) { display: none }`, at NORMAL importance, which is why a page
 *   CAN show a closed dialog with its own rule and is a real difference from the line above.
 *
 * WHAT IS DELIBERATELY NOT HERE: HTML §15.3.3's `[popover]:not(:popover-open):not(dialog[open]) { display: none }`
 * and `dialog:popover-open { display: block }`. Both turn on the POPOVER VISIBILITY STATE, which is an
 * element's own state and not an attribute — there is no attribute to read it off, so a rule written from the
 * `popover` attribute alone would hide every popover that a page had shown. Build the popover state machine
 * (HTML §6.12 "The popover attribute") and these two rules land beside the two above. */
static const char *cssd_ua_display_conditional(lxb_dom_element_t *el, const lxb_char_t *tag, size_t taglen,
                                               bool *important)
{
    size_t vlen = 0;
    const lxb_char_t *v;
    const char *hidden;

    /* css-cascade-5 §6.3's TOP BAND FIRST — "important user agent declarations" — because these three rules can all match one
       element and the order they are asked in IS the cascade between them. `<input type=hidden hidden>` matches
       this rule AND the `[hidden]` rule below, both with the value `none`, and only the IMPORTANCE tells them
       apart: asked the other way round the answer would be a normal declaration a page's own
       `input { display: block !important }` outranks. */
    if (taglen == 5 && memcmp(tag, "input", 5) == 0) {
        v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"type", 4, &vlen);
        if (cssd_attr_is_ascii_ci(v, vlen, "hidden")) { *important = true; return "none"; }
    }
    /* Then the two NORMAL rules, in css-cascade-5 §6.1's Specificity order, which is the criterion left once the band ties:
       `[hidden]:not([hidden=until-found i]):not(embed)` is (0,2,1) and `dialog:not([open])` is (0,1,1). They
       agree on `none` wherever both match, so the order is not observable today — it is the order the spec
       gives, written down so that it stays right when a third rule joins them. */
    hidden = cssd_ua_hidden(el, tag, taglen);
    if (hidden) return hidden;
    if (taglen == 6 && memcmp(tag, "dialog", 6) == 0) {
        v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"open", 4, &vlen);
        if (v == NULL) return "none";
    }
    return NULL;
}

/* THE UA DECLARATION for `name` on `el`, and its css-cascade-5 §6.3 IMPORTANCE. `*important` is written on EVERY path,
   including the ones that answer nothing, because the caller passes it straight into the cascade and a flag it
   did not write would carry whatever the last resolution left there — the failure mode being an ordinary
   `display: block` that outranks the page's own rule. */
static const char *cssd_ua_value(lxb_dom_element_t *el, const char *name, bool *important)
{
    size_t n = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &n);
    unsigned i;

    DCHECK(important != NULL,
           "a UA declaration was resolved with nowhere to report its IMPORTANCE. Two of HTML §15.3.1's display "
           "rules are `!important` and CSS Cascade §6.3 puts an important user-agent declaration above every "
           "author one, so a caller that cannot receive the flag would cascade `input[type=hidden]` as a "
           "normal declaration a page's own rule outranks");
    *important = false;
    if (!tag) return NULL;
    if (strcmp(name, "display") == 0) {
        const char *h = cssd_ua_display_conditional(el, tag, n, important);

        if (h) return h;
        DCHECK(!*important,
               "an attribute-conditional UA rule reported IMPORTANCE and no value — the flag is written only "
               "beside the declaration it belongs to, so one without the other is a rule that set it and then "
               "fell through");
    }
    for (i = 0; i < sizeof(UA_DEFAULT) / sizeof(UA_DEFAULT[0]); i++)
        if (strlen(UA_DEFAULT[i].tag) == n && memcmp(UA_DEFAULT[i].tag, tag, n) == 0 &&
            strcmp(UA_DEFAULT[i].prop, name) == 0) {
            /* HTML §15.3.1's `noscript` rule is `!important` inside `@media (scripting)`; the row carries the value
               and this carries the half of the rule a `{tag, prop, value}` triple has no column for. */
            *important = strcmp(UA_DEFAULT[i].tag, "noscript") == 0 && strcmp(name, "display") == 0;
            return UA_DEFAULT[i].value;
        }
    /* Every element the table does not name is `display: inline`, which is the UA sheet's own default. */
    if (strcmp(name, "display") == 0) return "inline";
    return NULL;
}

/* THE TABLE'S OWN INVARIANT, asserted once per instance beside the shorthand table's. ONE `{tag, prop}` PAIR
   HAS ONE ROW: the lookup above is a linear scan that stops at the first match, so a second row for a pair is a
   declaration that can never be read and a disagreement nothing would report — which is how a `display` for one
   element ends up depending on where in the file somebody added it. Every value is non-empty for the same
   reason `cssd_ua_value`'s last line exists: an empty string is a value the cascade would carry, not an absent
   declaration. */
static void cssd_ua_table_check(void)
{
#if APICLIENT_DEV
    unsigned i, j;

    for (i = 0; i < sizeof(UA_DEFAULT) / sizeof(UA_DEFAULT[0]); i++) {
        DCHECK(UA_DEFAULT[i].tag != NULL && UA_DEFAULT[i].tag[0] != '\0' &&
                   UA_DEFAULT[i].prop != NULL && UA_DEFAULT[i].prop[0] != '\0' &&
                   UA_DEFAULT[i].value != NULL && UA_DEFAULT[i].value[0] != '\0',
               "a row of the UA default stylesheet has an empty tag, property or value — a row is a whole "
               "declaration from HTML's rendering section and an empty half of one is a rule nobody wrote");
        for (j = 0; j < i; j++)
            DCHECK(strcmp(UA_DEFAULT[i].tag, UA_DEFAULT[j].tag) != 0 ||
                       strcmp(UA_DEFAULT[i].prop, UA_DEFAULT[j].prop) != 0,
                   "the UA default stylesheet declares one property TWICE for one element name. The lookup "
                   "stops at the first row, so the second is a declaration that can never win and never be "
                   "reported — which is the shape a transcription error from HTML's rendering section takes "
                   "when two of its sections name the same element");
    }
#endif
}

/* THE INITIAL VALUES LEXBOR'S REGISTRY DOES NOT CARRY. An initial value is a fact about the PROPERTY, stated
   on its own `Initial:` line, and it exists whether or not the vendored parser has a generated entry for it —
   so a property lexbor does not know still has one, and answering NULL for it is not "undeclared", it is a
   cascade that stopped a layer early. Lexbor carries the `border` and `border-<side>` SHORTHANDS and the four
   `border-*-color` longhands and nothing else of the border, so the eight below have no entry: the four widths
   are `medium` (css-backgrounds-3 §3.3) and the four styles are `none` (css-backgrounds-3 §3.2), which together are why the
   spec's own note says "although the initial width is medium, the initial style is none; therefore the used
   initial width is 0". The registry is still asked FIRST for every property, and a name here that lexbor DOES
   carry would be one fact with two sources — asserted below rather than assumed. */
static const struct { const char *name; const char *initial; } CSSD_INITIAL_UNREGISTERED[] = {
    { "border-top-width", "medium" }, { "border-right-width", "medium" },
    { "border-bottom-width", "medium" }, { "border-left-width", "medium" },
    { "border-top-style", "none" }, { "border-right-style", "none" },
    { "border-bottom-style", "none" }, { "border-left-style", "none" },
    /* css-fonts-4 §2.7's RESET IMPLICITLY group, plus the `font-variant-caps` its `<font-variant-css2>` term
       sets. Lexbor's registry carries six font properties (`font-family`, `font-size`, `font-stretch`,
       `font-style`, `font-weight`) and `line-height`, and none of these — yet css-fonts-4 §2.7 states that "all
       subproperties of the font property in the Set Explicitly and Reset Implicitly groups are FIRST RESET to
       their initial values", so a `font` declaration cannot be expanded without them and answering NULL would
       make the whole declaration invalid. Each value is that property's own `Initial:` line, and each is cited
       by STANDARD, NUMBER AND TITLE because the numbers are not adjacent and are not guessable — and the
       standard is on every row rather than in this lead-in, for the reason the file header states: a name
       further back than forty characters anchors nothing, and a list is where that bites hardest.
         css-fonts-4 §2.6  Relative sizing: the font-size-adjust property                    -> none
         css-fonts-4 §6.3  Kerning: the font-kerning property                                -> auto
         css-fonts-4 §6.4  Ligatures: the font-variant-ligatures property                    -> normal
         css-fonts-4 §6.5  Subscript and superscript forms: the font-variant-position property -> normal
         css-fonts-4 §6.6  Capitalization: the font-variant-caps property                    -> normal
         css-fonts-4 §6.7  Numerical formatting: the font-variant-numeric property           -> normal
         css-fonts-4 §6.8  Alternates and swashes: the font-variant-alternates property      -> normal
         css-fonts-4 §6.10 East Asian text rendering: the font-variant-east-asian property   -> normal
         css-fonts-4 §6.12 Low-level font feature settings control: the font-feature-settings property -> normal
         css-fonts-4 §6.13 Font language override: the font-language-override property       -> normal
         css-fonts-4 §8.1  Optical sizing control: the font-optical-sizing property          -> auto
         css-fonts-4 §8.2  Low-level font variation settings control: the font-variation-settings property -> normal
         css-fonts-4 §9.3  Selecting the text presentation style: The font-variant-emoji property -> normal
       TWO OF THE THIRTEEN ARE `auto` AND ELEVEN ARE NOT, which is the only thing about this table a reader has
       to get right: css-fonts-4 §6.3's `font-kerning` and css-fonts-4 §8.1's `font-optical-sizing` are the two,
       and every other row is its property's own stated word (css-fonts-4 §2.6's `none`, and `normal` for the
       rest). */
    { "font-feature-settings", "normal" }, { "font-kerning", "auto" },
    { "font-language-override", "normal" }, { "font-optical-sizing", "auto" },
    { "font-size-adjust", "none" }, { "font-variant-alternates", "normal" },
    { "font-variant-caps", "normal" }, { "font-variant-east-asian", "normal" },
    { "font-variant-emoji", "normal" }, { "font-variant-ligatures", "normal" },
    { "font-variant-numeric", "normal" }, { "font-variant-position", "normal" },
    { "font-variation-settings", "normal" },
    /* css-transforms-1 §3 "The transform Property" (css-transforms-1 §4 in the CR — the module renumbered when its ED dropped
       the number off "Terminology"), whose `Initial:` line is `none`. THE ROW IS WHAT MAKES "NO TRANSFORM" A
       COMPUTED VALUE RATHER THAN A SILENCE, and the difference is not pedantic: lexbor's registry carries no
       `transform` entry, so with no row here css-cascade-5 §7.1 had no initial value to fall to and the cascade answered
       NULL for every element on every page — which is not `none`, and which is why every consumer that asked
       "is this element transformed" got an answer it could not read. A DECLARED transform does reach the
       cascade already (lexbor turns a property it has no id for into a `__CUSTOM` declaration carrying the
       real name and the raw value), so this row completes the pair rather than standing in for it: declared
       values come through the cascade and undeclared ones come through here. */
    { "transform", "none" },
};

/* THE INITIAL VALUES LEXBOR'S REGISTRY GETS WRONG, each with the answer it gives today so the row EXPIRES.
   This is a different table from the one above and deliberately so: there the registry is silent and the fact
   has one source, here it SPEAKS and disagrees with the property's own `Initial:` line, so the row has to say
   what it is overriding or it is one fact with two sources and no way to tell which is stale. The DCHECK below
   re-reads the registry every time and fires the day lexbor's answer changes — which is the same shape
   css_color.c uses for the one `<color>` production it reads itself, and for the same reason: a vendored
   parser is a moving target and a silent divergence from it is worse than a crash.
   CSS Color 4 §3.2 gives `color` an `Initial:` line of `CanvasText`; lexbor answers `currentcolor`, which
   cannot be an initial value at all — CSS Color 4 §6.4 makes currentcolor's used value the used value of
   `color` on the same element, so on the root element, where css-cascade-5 §7.2's inherited value IS the
   initial value, it would be a
   definition of itself with no base case. */
static const struct { const char *name; const char *initial; const char *registry; } CSSD_INITIAL_WRONG[] = {
    { "color", "canvastext", "currentcolor" },
};

/* CSS Cascade §7.1's INITIAL VALUE, straight out of Lexbor's registry, which is where the spec's own initial
   values live, and out of the table above for the properties it has no entry for.
   IT IS NOT A LAYER OF THE CASCADE, which is why it is no longer the last thing `cssom_cascaded_value` tries.
   css-cascade-5 §6 Cascading answers which DECLARATION won and css-cascade-5 §7 Defaulting is a separate step
   over that answer — and the difference is
   observable the moment a property is INHERITED: folding the initial value into the cascade makes "nobody
   declared it" indistinguishable from "somebody declared the initial value", and css-cascade-5 §7.2 has to
   tell those apart
   to know whether to ask the parent. So the cascade reports the absence and this is exported for the step that
   acts on it (core/css/css_defaulting.h). */
char *cssom_initial_value(const char *name)
{
    const lxb_css_entry_data_t *e = lxb_css_property_by_name((const lxb_char_t *)name, strlen(name));
    CssBuf b = { 0 };
    unsigned i;

    for (i = 0; i < sizeof(CSSD_INITIAL_WRONG) / sizeof(CSSD_INITIAL_WRONG[0]); i++) {
        char *out;

        if (strcmp(CSSD_INITIAL_WRONG[i].name, name) != 0) continue;
        DCHECK(e != NULL && e->initial != NULL,
               "a property this file OVERRIDES the registry's initial value for has no registry entry at all — "
               "the row exists to disagree with lexbor, so with nothing to disagree with it belongs in "
               "CSSD_INITIAL_UNREGISTERED instead");
        lxb_css_property_serialize(e->initial, e->unique, css_buf_cb, &b);
        DCHECK(b.s != NULL && strcmp(b.s, CSSD_INITIAL_WRONG[i].registry) == 0,
               "lexbor's registry no longer answers the initial value this row was written to override. That "
               "is the row's own expiry condition: DELETE it and let the registry answer, after checking that "
               "what it now says is the property's `Initial:` line");
        free(b.s);
        out = strdup(CSSD_INITIAL_WRONG[i].initial);
        CHECK(out != NULL, "cssom: OOM copying an initial value — a dropped one reads as no value at all, "
                           "which is a cascade that stopped before its last layer");
        return out;
    }
    if (e && e->initial) {
        lxb_css_property_serialize(e->initial, e->unique, css_buf_cb, &b);
        return b.s;
    }
    for (i = 0; i < sizeof(CSSD_INITIAL_UNREGISTERED) / sizeof(CSSD_INITIAL_UNREGISTERED[0]); i++) {
        char *out;

        if (strcmp(CSSD_INITIAL_UNREGISTERED[i].name, name) != 0) continue;
        DCHECK(e == NULL,
               "a property this file states an initial value for is ALSO in lexbor's property registry — one "
               "fact with two answers, and the registry's is the one every other property in CSS reads. DELETE "
               "the row: the registry entry is what a pinned parser upgrade would keep in step");
        out = strdup(CSSD_INITIAL_UNREGISTERED[i].initial);
        CHECK(out != NULL, "cssom: OOM copying an initial value — a dropped one reads as no value at all, "
                           "which is a cascade that stopped before its last layer");
        return out;
    }
    return NULL;
}

/* THE CASCADE, in the order the spec resolves it. What comes out is the SPECIFIED value — the declaration that
   won — which is not yet the computed value and is not yet CSSOM §9's resolved value; css_computed_value.c owns both
   of those steps, and this is the one entry it reads the cascade through. */
char *cssom_cascaded_value(lxb_dom_element_t *el, const char *name)
{
    CssLayerOrder *order;
    CssCascade *cascade;
    const char *ua;
    uint32_t seq = 0;
    bool important = false, ua_important = false;
    char *v, *out;

    DCHECK(g_ready, "the cascade was resolved before cssom_init built the CSS parser it parses every layer "
                    "with — the component is initialised with the DOM, so a caller reaching it first is a "
                    "component ordered ahead of the browser's own setup");
    DCHECK(el != NULL && name != NULL, "the cascade was asked to resolve with no element or no property name");
    DCHECK(!css_shorthand_is_shorthand(name),
           "a SHORTHAND was asked of the cascade, and the cascade is over LONGHANDS ONLY — CSS Cascade says so, "
           "and §6.6's declarations are longhands for the same reason, so NO layer below declares one: the "
           "answer would be the property's initial value with nothing to say the longhands that DID set it were "
           "never looked at. §6.6.1's getPropertyValue owns the shorthand step, and both paths that can be "
           "asked for one run it BEFORE reaching here — cssd_property_value over a block's declarations, and "
           "css_resolved_value over CSSOM §7.2's resolved longhands. A third caller must run it too");
    /* EVERY ORIGIN CONTRIBUTES INTO ONE LIST, AND THE SORT DECIDES — which is css-cascade-5 §6.1 and is not
       what asking each
       origin in turn does. The four used to be asked in precedence order and the first that answered won, and
       that is a DIFFERENT ordering: it hoists css-cascade-5 §6.1's Element-Attached criterion above its
       Origin-and-Importance
       one, so `<p style="color:blue">` beat `p { color: red !important }`, which css-cascade-5 §6.1 and
       css-cascade-5 §6.3 both say it
       loses to ("an important declaration takes precedence over a normal declaration", and Element-Attached is
       the criterion BELOW Origin and Importance, reached only when it ties). */
    order = css_layer_order_create();
    cascade = css_cascade_create(order);
    cssd_author_collect(el, name, cascade, order, &seq);
    /* css-cascade-5 §6.1's ELEMENT-ATTACHED STYLES: "declarations that are attached directly to an element (such as the
       contents of a style attribute) rather than indirectly mapped by means of a style rule selector take
       precedence over declarations the same importance that are mapped via style rule." It is in the AUTHOR
       origin ([CSSSTYLEATTR]) and in no explicit cascade layer, and css-cascade-5 §6.1's Order of Appearance places it after
       every style sheet ("declarations from style attributes ... are all placed after any style sheets"),
       which is what the counter reaching here already is. */
    v = cssd_inline_value(el, name, &important);
    if (v) {
        css_cascade_add(cascade, CSS_ORIGIN_AUTHOR, important, true, css_layer_order_root(order), 0, ++seq, v);
        free(v);
    }
    /* css-cascade-5 §6.5's AUTHOR PRESENTATIONAL HINT ORIGIN, "between the regular user origin and the author
       origin". It is an ORIGIN and not a row of the UA table below because that is where css-cascade-5 §6.5 puts it: an
       author rule of any specificity outranks a hint, and a hint outranks the UA sheet. */
    v = css_presentational_hint(el, name);
    if (v) {
        css_cascade_add(cascade, CSS_ORIGIN_PRESENTATIONAL_HINT, false, false, NULL, 0, ++seq, v);
        free(v);
    }
    /* css-cascade-5 §6.2's USER-AGENT ORIGIN, with css-cascade-5 §6.3's IMPORTANCE carried rather than assumed
       normal: two of HTML §15.3.1's
       `display` rules are `!important`, and "important user agent declarations" is the TOP band of
       css-cascade-5 §6.3's list
       — above important author declarations — so a flag dropped here is a page's own rule silently giving a
       box to an element the UA sheet says has none. */
    ua = cssd_ua_value(el, name, &ua_important);
    if (ua) css_cascade_add(cascade, CSS_ORIGIN_UA, ua_important, false, NULL, 0, ++seq, ua);
    /* css-cascade-5 §6.4.3 "Layer Ordering"'s order is a fact about the WHOLE document's layers, so it is sealed once every sheet has been
       walked and before the first index is read. Nothing below declares a layer. */
    css_layer_order_seal(order);
    /* NULL HERE IS "NO DECLARATION WON", which is a real answer and not a missing one — CSS Cascade §7.1 and
       css-cascade-5 §7.2 are both written for exactly this state ("unless the cascade results in a value"),
       and which of them
       applies is the property's own `Inherited:` line. css-cascade-5 §7's step is core/css/css_defaulting.h's
       and it runs
       above this; css-cascade-5 §7.3's three cascade-dependent keywords are discharged BELOW it, inside the
       sort, because
       each is defined by a fact only the sort holds. */
    out = css_cascade_value(cascade);
    css_cascade_free(cascade);
    css_layer_order_free(order);
    return out;
}

/* ---- §6.6's DECLARATIONS: where the two backings keep them, and how a member edits them ------------------- */

/* THE TEXT the block's declarations are kept as — the element's `style` content attribute, or the rule's block
   text. Both are per-flow, which is the whole reason each is where it is. OWNED, NULL for a block that declares
   nothing. */
static char *cssd_declarations_text(JSContext *ctx, JSValueConst block, size_t *plen)
{
    JSValue rule = cssd_parent_rule(ctx, block);
    lxb_dom_element_t *el;
    const char *text;
    size_t len = 0;
    char *out;

    DCHECK(!cssd_flag(ctx, block, "computed"),
           "a COMPUTED declaration block's declarations were asked for as stored text, and there are none: "
           "CSSOM §7.2 states them as the resolved value of every longhand supported CSS property, which this "
           "engine "
           "computes per read (css_computed_value.h) rather than holding. Every member that can meet a computed "
           "block answers it from the resolved value before reaching here");
    *plen = 0;
    if (!JS_IsNull(rule)) {
        out = css_rule_block_text(ctx, rule, plen);
        JS_FreeValue(ctx, rule);
        return out;
    }
    JS_FreeValue(ctx, rule);
    el = cssd_owner_element(ctx, block);
    DCHECK(el != NULL,
           "a CSS declaration block has neither an owner node nor a parent CSS rule, so its declarations are "
           "kept nowhere — every creator sets exactly one of the two");
    text = cssd_inline_text(el, &len);
    if (!text || !len) return NULL;
    out = malloc(len + 1);
    CHECK(out != NULL, "cssom: OOM copying a declaration block's text");
    memcpy(out, text, len);
    out[len] = '\0';
    *plen = len;
    return out;
}

/* §6.6's UPDATE STYLE ATTRIBUTE, generalized to the backing the block actually has. For an element it goes
   through setAttribute's own chokepoint, so the write is captured by the per-flow delta like every other DOM
   write; for a rule it goes through the rule record's capturing accessor, which is the same guarantee one
   component along. */
static void cssd_declarations_write(JSContext *ctx, JSValueConst block, const char *text, size_t len)
{
    JSValue rule = cssd_parent_rule(ctx, block);
    lxb_dom_element_t *el;

    DCHECK(!cssd_flag(ctx, block, "readOnly"),
           "a READ-ONLY declaration block was written — §6.6.1 makes every mutating member throw a "
           "NoModificationAllowedError before it reaches its steps, so a write that got here skipped that");
    if (!JS_IsNull(rule)) {
        css_rule_set_block_text(ctx, rule, text, len);
        JS_FreeValue(ctx, rule);
        return;
    }
    JS_FreeValue(ctx, rule);
    el = cssd_owner_element(ctx, block);
    DCHECK(el != NULL, "a CSS declaration block with no backing at all was written");
    dom_cow_set_attribute(el, "style", text, len, JS_UNDEFINED);
}

/* §6.6.1's "let component value list be the result of parsing value for property property", asked of THE
   PARSER: the declaration `name: value` goes through exactly the parse a declaration in a block goes through,
   and what comes back is the canonical serialization of its value, or NULL.
   NULL IS "IF COMPONENT VALUE LIST IS NULL, THEN RETURN" — the whole call is abandoned and the block is left
   alone, which is what makes `style.color = 'unknown color'` leave a standing `color: red` standing. Storing
   the unparseable text instead was not a smaller version of that: the next read dropped the declaration as
   `__UNDEF` and the property came back as UNDECLARED, so the assignment silently removed a value it had no
   business touching.
   IT IS ALSO WHAT KEEPS A PROPERTY NAME FROM CARRYING A DECLARATION OF ITS OWN. This engine's block is TEXT,
   so `setProperty('a;color', 'red')` written into it would parse back as two declarations; the parse here is
   required to produce exactly ONE, named exactly `name`. §6.6.1's note that "value can not include
   !important" is the same check from the other side: a value that parsed as important is not this value.
   OWNED. */
static char *cssd_parse_value(const char *name, const char *value)
{
    lxb_css_memory_t *mem = NULL;
    lxb_css_rule_declaration_list_t *list;
    CssBuf text = { 0 };
    char *out = NULL;

    DCHECK(name != NULL && value != NULL, "a declaration's value was parsed with no property name or no value");
    css_buf_add(&text, name);
    css_buf_add(&text, ":");
    css_buf_add(&text, value);
    list = cssd_parse_block(text.s, text.n, &mem);
    if (list && list->first && list->first->next == NULL &&
        list->first->type == LXB_CSS_RULE_DECLARATION) {
        lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(list->first);

        if (d->type != LXB_CSS_PROPERTY__UNDEF && !d->important) {
            char *dname = cssd_decl_name(d);

            if (dname && strcmp(dname, name) == 0) out = cssd_decl_value(d);
            free(dname);
        }
    }
    if (mem) lxb_css_memory_destroy(mem, true);
    css_buf_free(&text);
    return out;
}

/* §6.6.1's SET A CSS DECLARATION, over the block's declarations. Its constraints are the whole algorithm — the
   spec states no steps at all, only what must hold afterwards — and this is the second of the two algorithms
   its own note offers, which is also what both shipping engines do: the target declaration keeps the POSITION
   it had, UNLESS a declaration of the same logical property group with the other mapping logic sits after it,
   in which case it is removed and appended so that it ends up "at an index after all of those".
   That last clause is not decoration: CSS Logical §4 makes such a pair share a computed value "determined by
   cascading the declarations of both properties together as one", so leaving the target where it stood would
   set a value the very next read resolves to the other member's. Takes ownership of `value`. */
static void cssd_decls_set(CssDecls *d, const char *name, char *value, bool important)
{
    int at = cssd_decls_index(d, name);
    bool physical = true, move = false;
    unsigned group, j;

    if (at >= 0) {
        group = css_shorthand_logical_group(name, &physical);
        for (j = (unsigned)at + 1; group != 0 && j < d->n; j++) {
            bool other = true;

            if (css_shorthand_logical_group(d->v[j].name, &other) == group && other != physical) {
                move = true;
                break;
            }
        }
        if (!move) {
            free(d->v[at].value);
            d->v[at].value = value;
            d->v[at].important = important;
            return;
        }
        cssd_decls_remove_at(d, (unsigned)at);
    }
    cssd_decls_append(d, cssd_strdup(name), value, important);
}

/* §6.6.1's setProperty over the declarations, from its step 6 on: parse the value, and "if property is a
   shorthand property, then for each longhand property longhand that property maps to, IN CANONICAL ORDER, set
   the CSS declaration longhand with the appropriate value(s) from component value list". A shorthand therefore
   sets its longhands and never itself — which is the same expansion the read path does, run through the same
   two functions, so a block cannot hold a property one of them would have expanded. */
static void cssd_decls_set_property(CssDecls *d, const char *name, const char *value, bool important)
{
    const char *const *lh;
    char *values[CSS_SHORTHAND_MAX_LONGHANDS];
    char *parsed;
    unsigned n, i;

    parsed = cssd_parse_value(name, value);
    if (!parsed) return;
    lh = css_shorthand_longhands(name, &n);
    if (!lh) {
        char *v = css_shorthand_validates_longhand(name) ? css_shorthand_longhand_value(name, parsed)
                                                         : parsed;

        if (v != parsed) free(parsed);
        if (v) cssd_decls_set(d, name, v, important);
        return;
    }
    CHECK(n <= CSS_SHORTHAND_MAX_LONGHANDS,
          "cssom: a shorthand's longhand list outgrew the array §6.6.1's setProperty expands through");
    for (i = 0; i < n; i++) {
        values[i] = css_shorthand_component(name, parsed, lh[i]);
        if (!values[i]) break;
    }
    free(parsed);
    if (i < n) {                       /* the shorthand's own grammar refused it: the call is abandoned */
        while (i > 0) free(values[--i]);
        return;
    }
    for (i = 0; i < n; i++) cssd_decls_set(d, lh[i], values[i], important);
}

/* §6.6.1's removeProperty, steps 4 and 5: "if property is a shorthand property, for each longhand property
   longhand that property maps to ... remove that CSS declaration", otherwise remove the one named. The
   shorthand's OWN name needs no removal step, because a block never holds one. */
static void cssd_decls_remove_property(CssDecls *d, const char *name)
{
    const char *const *lh;
    unsigned n, i;
    int at;

    lh = css_shorthand_longhands(name, &n);
    for (i = 0; lh && i < n; i++) {
        at = cssd_decls_index(d, lh[i]);
        if (at >= 0) cssd_decls_remove_at(d, (unsigned)at);
    }
    if (lh) return;
    at = cssd_decls_index(d, name);
    if (at >= 0) cssd_decls_remove_at(d, (unsigned)at);
}

/* The whole of a member's write: read the declarations, edit them, put them back — where "put them back" is
   §6.6's UPDATE STYLE ATTRIBUTE, whose step is "set an attribute value for owner node using 'style' and the
   result of SERIALIZING declaration block", generalized to the backing the block actually has.
   THE SERIALIZATION IS LOSSLESS FOR THE DECLARATIONS NOW AND WAS NOT BEFORE, which is why the block used to
   store an unconsolidated list instead. §6.6's serialization CONSOLIDATES — four `margin-*` declarations become
   one `margin` — and while a block held the author's shorthand unexpanded, storing that threw away which
   longhands it declared, so the very next `removeProperty('margin-top')` found nothing by that name. Now the
   declarations ARE longhands: `margin: 2px 1px 1px` parses back to exactly the four the serializer consolidated,
   so the round trip is the identity on the block's declarations, and what a page reads out of
   `getAttribute('style')` is the string a browser writes there. */
static void cssd_write_declaration(JSContext *ctx, JSValueConst block, const char *name, const char *value,
                                   bool important)
{
    size_t len = 0;
    char *text = cssd_declarations_text(ctx, block, &len);
    CssDecls d = { 0 };
    char *next;

    cssd_decls_from_text(text, len, &d);
    if (value) cssd_decls_set_property(&d, name, value, important);
    else       cssd_decls_remove_property(&d, name);
    next = cssd_serialize_decls(&d);
    cssd_decls_free(&d);

    free(text);
    cssd_declarations_write(ctx, block, next ? next : "", next ? strlen(next) : 0);
    free(next);
}

/* ---- the interfaces --------------------------------------------------------------------------------------- */

/* §6.6.1's NoModificationAllowedError, which every mutating member throws FIRST — before it converts anything
   and before it looks at the declarations. It is keyed on the READONLY FLAG and not on "is this computed",
   which are two different properties of a block that this engine's `mode` could not tell apart. */
static JSValue cssd_readonly_throw(JSContext *ctx)
{
    return JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                                "the CSS declaration block's readonly flag is set");
}

/* magic 0 = getPropertyValue, 1 = removeProperty, 2 = getPropertyPriority */
static JSValue js_cssd_prop_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue block = cssd_block(ctx, this_val);
    const char *name;
    bool computed;
    JSValue r;

    if (JS_IsException(block)) return block;
    computed = cssd_flag(ctx, block, "computed");
    if (magic == 1 && cssd_flag(ctx, block, "readOnly")) {
        JS_FreeValue(ctx, block);
        return cssd_readonly_throw(ctx);
    }
    DCHECK(argc >= 1, "a §6.6.1 property member reached its body with no property name — its IDL argument is "
                      "required, so the declaration's own argument-count check should have refused the call");
    name = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!name) { JS_FreeValue(ctx, block); return JS_EXCEPTION; }
    if (magic == 1) {
        /* §6.6.1's removeProperty: "let value be the return value of invoking getPropertyValue()" — read
           BEFORE the removal, because that value is what it returns. */
        size_t len = 0;
        char *text = cssd_declarations_text(ctx, block, &len);
        char *old = cssd_property_value(text, len, name);

        free(text);
        /* §6.6.1's step 4 — "if property is a shorthand property, for each longhand property longhand that
           property maps to, invoke removeProperty() with longhand as argument" — is one edit of the
           declarations, made where every other edit is. The extra removal of the SHORTHAND'S OWN NAME that
           used to be here is gone with the thing that made it necessary: the block held the author's spelling,
           and now it holds the longhands §6.6 says it holds. */
        cssd_write_declaration(ctx, block, name, NULL, false);
        r = old ? JS_NewString(ctx, old) : JS_NewStringLen(ctx, "", 0);
        free(old);
    } else if (magic == 2) {
        /* §6.6.1's getPropertyPriority. A COMPUTED block's declarations are resolved values and carry no
           important flag at all, so the answer over its whole domain is the empty string — a positive
           statement about that block, not a hole where its text would be. */
        bool important = false;

        if (!computed) {
            size_t len = 0;
            char *text = cssd_declarations_text(ctx, block, &len);

            important = cssd_property_important(text, len, name);
            free(text);
        }
        r = JS_NewString(ctx, important ? "important" : "");
    } else if (computed) {
        /* CSSOM §9's RESOLVED value, which for most properties is the computed value and for the box-model
           ones is the used value (css_computed_value.h). It answers a JSValue rather than text because a used
           value derived from CSS 2.1 §10.1's initial containing block carries the VIEWPORT's domain, and a
           `char *` here would carry the number and drop the fork. */
        r = css_resolved_value(ctx, cssd_owner_element(ctx, block), name);
    } else {
        size_t len = 0;
        char *text = cssd_declarations_text(ctx, block, &len);
        char *v = cssd_property_value(text, len, name);

        free(text);
        r = v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
        free(v);
    }
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, block);
    return r;
}

static JSValue js_cssd_set_property(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue block = cssd_block(ctx, this_val);
    const char *name, *value, *priority;
    bool important;

    (void)magic;
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "readOnly")) {
        JS_FreeValue(ctx, block);
        return cssd_readonly_throw(ctx);
    }
    DCHECK(argc >= 2, "§6.6.1's setProperty reached its body without the two arguments its IDL requires — the "
                      "declaration's own Web IDL §3.6 Overload resolution algorithm step 5 count is what "
                      "should have refused the call");
    name = JS_ToCString(ctx, argv[0]);
    value = JS_ToCString(ctx, argv[1]);
    /* Web IDL §3.6's ABSENT OPTIONAL ARGUMENT, in both of its spellings: a call that stopped short of the position
       arrives with a shorter argc, and one that reached it with `undefined` arrives with undefined in the slot —
       "if the argument is optional and its value is undefined, it is absent". This member's IDL writes
       `optional CSSOMString priority = ""`, so absent IS the empty string, which is a POSITIVE statement that
       the page named no priority rather than a hole. Converting either would produce the four characters
       "undefined" and abandon the call at step 4 below. */
    priority = (argc >= 3 && !JS_IsUndefined(argv[2])) ? JS_ToCString(ctx, argv[2]) : NULL;
    if (!name || !value || (argc >= 3 && !JS_IsUndefined(argv[2]) && !priority)) {
        if (name) JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        if (priority) JS_FreeCString(ctx, priority);
        JS_FreeValue(ctx, block);
        return JS_EXCEPTION;
    }
    /* §6.6.1's step 4: "if priority is not the empty string and is not an ASCII case-insensitive match for the
       string 'important', then return". An unrecognised priority is not a declaration written without one — it
       abandons the call, so `setProperty('color','red','urgent')` leaves the block alone. The match is ASCII by
       the spec's own word, so it is spelled out rather than left to a locale-dependent library compare. */
    important = priority && *priority != '\0';
    if (important) {
        static const char IMPORTANT[] = "important";
        size_t i;

        for (i = 0; i < sizeof(IMPORTANT); i++)
            if ((char)tolower((unsigned char)priority[i]) != IMPORTANT[i]) break;
        if (i != sizeof(IMPORTANT)) {
            JS_FreeCString(ctx, name);
            JS_FreeCString(ctx, value);
            JS_FreeCString(ctx, priority);
            JS_FreeValue(ctx, block);
            return JS_UNDEFINED;
        }
    }
    /* §6.6.1's step 3: setting the empty string invokes removeProperty and returns. */
    cssd_write_declaration(ctx, block, name, *value ? value : NULL, important);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    JS_FreeCString(ctx, priority);
    JS_FreeValue(ctx, block);
    return JS_UNDEFINED;
}

/* THE ONE PAIR OF BODIES ALL THREE §6.6.1 SPELLINGS ANSWER THROUGH. §6.6.1 states both halves of each of them
   as a forward — the getter "must return the result of invoking getPropertyValue()" and the setter "must invoke
   setProperty() ... and no third argument" — so they answer out of the same two paths above and never grow a
   rule of their own. `magic` is Lexbor's own property id, so the PROPERTY name is read back out of the registry
   rather than stored twice, and the attribute's own spelling is never inverted to recover it. */
static JSValue js_cssd_property_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    const lxb_css_entry_data_t *e = lxb_css_property_by_id((uintptr_t)magic);
    JSValue block = cssd_block(ctx, this_val), r;

    DCHECK(e != NULL, "a CSS attribute was declared with a property id the registry does not have");
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "computed")) {
        r = css_resolved_value(ctx, cssd_owner_element(ctx, block), (const char *)e->name);
    } else {
        size_t len = 0;
        char *text = cssd_declarations_text(ctx, block, &len);
        char *v = cssd_property_value(text, len, (const char *)e->name);

        free(text);
        r = v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
        free(v);
    }
    JS_FreeValue(ctx, block);
    return r;
}

static JSValue js_cssd_property_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const lxb_css_entry_data_t *e = lxb_css_property_by_id((uintptr_t)magic);
    JSValue block = cssd_block(ctx, this_val);
    const char *v;

    DCHECK(e != NULL, "a CSS attribute was declared with a property id the registry does not have");
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "readOnly")) {
        JS_FreeValue(ctx, block);
        return cssd_readonly_throw(ctx);
    }
    v = JS_ToCString(ctx, val);   /* a real string by now: the declaration converted it */
    if (!v) { JS_FreeValue(ctx, block); return JS_EXCEPTION; }
    cssd_write_declaration(ctx, block, (const char *)e->name, *v ? v : NULL, false);
    JS_FreeCString(ctx, v);
    JS_FreeValue(ctx, block);
    return JS_UNDEFINED;
}

/* ---- CSSOM §6.6.1's THREE PER-PROPERTY PARTIAL INTERFACES ---------------------------------------------------
 *
 * §6.6.1 The CSSStyleDeclaration Interface declares THREE partial interfaces on CSSStyleProperties — one per
 * SPELLING of the same property — and this engine installed one of them, so `element.style["font-size"]` was
 * undefined in a browser that has it:
 *   - the CAMEL-CASED attribute, "for each CSS property property that is a supported CSS property", named by
 *     running the CSS property to IDL attribute algorithm for property;
 *   - the WEBKIT-CASED attribute, "for each CSS property property that is a supported CSS property and that
 *     begins with the string -webkit-", named by that same algorithm "with the lowercase first flag set";
 *   - the DASHED attribute, "for each CSS property property that is a supported CSS property, except for
 *     properties that have no "-" (U+002D) in the property name", "where dashed attribute is property".
 *
 * THREE FUNCTIONS AND NOT ONE LOOP WITH THREE `if`s, because the spec states three partial interfaces and each
 * one is a contract that can be absent on its own: an installer that is never called is a partial interface
 * this engine does not have, and one function per partial is what makes that visible from outside — to a reader
 * and to engine/idlgen.mjs's gap audit, which names these three by their own names and reports the day one of
 * them stops being called. A single loop can lose a spelling silently inside itself.
 *
 * ALL THREE ANSWER THROUGH js_cssd_property_get / js_cssd_property_set, whose `magic` is lexbor's property
 * id, which is exactly what §6.6.1 asks for and is why no inverse algorithm is written here. The getter of a
 * webkit-cased attribute must invoke getPropertyValue "with the argument being the result of running the IDL
 * attribute to CSS property algorithm ... with the dash prefix flag set" — that inverse exists to recover the
 * property name from the ATTRIBUTE name, and this engine never lost it: the id names the registry row, so the
 * property name is read back rather than reconstructed. Writing the inverse would be a second derivation free
 * to disagree with the first. */

/* CSSOM §2 Terminology: "The term supported CSS property refers to a CSS property that the user agent
   implements, INCLUDING ANY VENDOR-PREFIXED PROPERTIES, but excluding custom properties." Lexbor's registry IS
   that set for this engine, minus its two non-property rows: `LXB_CSS_PROPERTY__UNDEF` is its "no property"
   sentinel and `LXB_CSS_PROPERTY__CUSTOM` is the custom-property marker CSSOM §2 excludes by name.
   WHAT STOOD HERE WAS THE OPPOSITE OF CSSOM §2 ON BOTH COUNTS. It skipped every name beginning with "-" — a
   vendor-prefixed property, which CSSOM §2 says IS supported and CSSOM §6.6.1 gives two spellings of — and it started the
   walk at id 1, so lexbor's custom-property marker was installed as an IDL attribute literally spelled
   `#сustom` (with a Cyrillic С, and a `length` field that disagrees with its own bytes: the one row in the
   registry whose declared length is not its strlen). A member no browser has, on the prototype of every
   declaration block in the engine. */
static bool cssom_supported_css_property(uintptr_t id, const lxb_css_entry_data_t *e)
{
    DCHECK(e != NULL && e->name != NULL, "lexbor's property registry answered a row with no name");
    if (id == LXB_CSS_PROPERTY__UNDEF || id == LXB_CSS_PROPERTY__CUSTOM)
        return false;
    DCHECK(e->name[0] != '#',
           "a lexbor property row spells a MARKER rather than a property name — CSSOM §2 Terminology's two "
           "exclusions are the ids above, and this registry has grown a third that would install as a member");
    DCHECK(e->length == strlen((const char *)e->name),
           "a lexbor property row's declared length is not its own strlen, so an IDL attribute name derived "
           "from it would be truncated or would read past the row");
    return true;
}

/* An IDL attribute name is bounded by the property name it comes from, since §6.6.1's algorithm only ever
   DROPS characters and the dashed spelling IS the property name. Sized well past lexbor's longest row so the
   DCHECK below states the invariant rather than guarding a real edge; the version of this loop that
   `continue`d past an over-long name would have dropped a member of a browser's surface without a word. */
#define CSSOM_IDL_ATTRIBUTE_MAX 64

/* CSSOM §6.6.1 The CSSStyleDeclaration Interface's CSS PROPERTY TO IDL ATTRIBUTE algorithm, "optionally with a
   lowercase first flag set", step for step:
     1. Let output be the empty string.        2. Let uppercase next be unset.
     3. If the lowercase first flag is set, remove the first character from property.
     4. For each character c in property: if c is "-" (U+002D), let uppercase next be set; otherwise, if
        uppercase next is set, let uppercase next be unset and append c converted to ASCII uppercase to output;
        otherwise, append c to output.
     5. Return output.
   Step 3 is what the flag's NAME describes rather than what the step does — it removes a character, and the
   lowercase first is the CONSEQUENCE, because the removed character is the "-" that would otherwise have set
   uppercase next. `-webkit-transform` is the spec's own worked example: with the flag it is `webkitTransform`,
   without it `WebkitTransform`, and §6.6.1 says a user agent supporting that property has BOTH. */
static void cssom_css_property_to_idl_attribute(const lxb_css_entry_data_t *e, bool lowercase_first,
                                                char *out, size_t cap)
{
    size_t i = 0, j = 0;
    bool uppercase_next = false;

    DCHECK(e->length > 0, "§6.6.1's algorithm was run for a property with no name");
    DCHECK(e->length < cap, "a CSS property name outgrew the IDL attribute buffer — raise "
                            "CSSOM_IDL_ATTRIBUTE_MAX rather than dropping the member");
    if (lowercase_first)
        i = 1;
    for (; i < e->length; i++) {
        lxb_char_t c = e->name[i];

        if (c == '-') { uppercase_next = true; continue; }
        out[j++] = uppercase_next ? (char)toupper(c) : (char)c;
        uppercase_next = false;
    }
    out[j] = 0;
    DCHECK(j > 0, "§6.6.1's algorithm produced the empty string for a supported CSS property");
}

/* THE ONE INSTALL ALL THREE SPELLINGS GO THROUGH. §6.6.1 gives the camel-cased, webkit-cased and dashed
   attributes of one property the same getter steps and the same setter steps, so what differs between them is
   the NAME and nothing else — the getter, the setter id and the magic are the PROPERTY's. */
static void cssom_install_property_attribute(JSContext *ctx, JSValueConst proto, uintptr_t id, const char *name)
{
    DCHECK(g_property_set_id[id] >= 0,
           "a §6.6.1 per-property attribute is being installed for a property cssom_init declared no setter "
           "for — the attribute would be silently read-only");
    idl_install_accessor(ctx, proto, name, js_cssd_property_get, (int)id, g_property_set_id[id]);
}

static void cssom_install_camel_cased_attributes(JSContext *ctx, JSValueConst proto)
{
    uintptr_t id;

    for (id = 0; id < LXB_CSS_PROPERTY__LAST_ENTRY; id++) {
        const lxb_css_entry_data_t *e = lxb_css_property_by_id(id);
        char name[CSSOM_IDL_ATTRIBUTE_MAX];

        if (!cssom_supported_css_property(id, e)) continue;
        cssom_css_property_to_idl_attribute(e, false, name, sizeof name);
        cssom_install_property_attribute(ctx, proto, id, name);
    }
}

static void cssom_install_webkit_cased_attributes(JSContext *ctx, JSValueConst proto)
{
    static const char PREFIX[] = "-webkit-";
    uintptr_t id;

    for (id = 0; id < LXB_CSS_PROPERTY__LAST_ENTRY; id++) {
        const lxb_css_entry_data_t *e = lxb_css_property_by_id(id);
        char name[CSSOM_IDL_ATTRIBUTE_MAX];

        if (!cssom_supported_css_property(id, e)) continue;
        /* "and that begins with the string -webkit-" */
        if (e->length < sizeof(PREFIX) - 1 || memcmp(e->name, PREFIX, sizeof(PREFIX) - 1) != 0) continue;
        cssom_css_property_to_idl_attribute(e, true, name, sizeof name);
        cssom_install_property_attribute(ctx, proto, id, name);
    }
}

static void cssom_install_dashed_attributes(JSContext *ctx, JSValueConst proto)
{
    uintptr_t id;

    for (id = 0; id < LXB_CSS_PROPERTY__LAST_ENTRY; id++) {
        const lxb_css_entry_data_t *e = lxb_css_property_by_id(id);

        if (!cssom_supported_css_property(id, e)) continue;
        /* "except for properties that have no "-" (U+002D) in the property name" — and "dashed attribute is
           property", so no algorithm runs here at all: the member's name IS the registry row's. */
        if (memchr(e->name, '-', e->length) == NULL) continue;
        cssom_install_property_attribute(ctx, proto, id, (const char *)e->name);
    }
}

/* ---- THE DESCRIPTOR INTERFACES: CSS Fonts 5 §9.1's CSSFontFaceDescriptors and CSSOM §6.4.7's
 * CSSPageDescriptors -----------------------------------------------------------------------------------------
 *
 * EACH LIST IS TYPED OUT BECAUSE THERE IS NO REGISTRY TO GENERATE IT FROM, and that is the opposite of
 * §6.6.1's three per-property partial interfaces above, which are generated precisely because lexbor's
 * property table IS the "supported CSS property" set §6.6.1 states them over. A DESCRIPTOR is not a property:
 * `src` and `unicode-range` are accepted nowhere but inside an `@font-face` rule and `size` nowhere but inside an
 * `@page`, lexbor's registry has no entry for any of them, and each set is closed by the IDL rather than by
 * what this engine implements.
 *
 * BOTH SPELLINGS ARE WRITTEN OUT, because the IDL DECLARES BOTH — `fontFamily` and `font-family` are two
 * attributes of the interface, not one attribute and a convenience — and a table that carried only the dashed
 * one and ran §6.6.1's CSS-property-to-IDL-attribute algorithm over it at install time would be DERIVING a list
 * the spec STATES. The derivation is not even total for the properties it was borrowed from: §6.6.1 declares
 * `cssFloat` as its own attribute, which no dash-to-camel walk produces. A name that is already one word
 * (`src`, `size`, `marks`, `bleed`, `margin`) has ONE attribute, and its two columns say so by coinciding.
 *
 * TWO TABLES, ONE MAGIC SPACE. The attribute bodies below are shared: a descriptor is read through
 * getPropertyValue and written through setProperty whichever interface declared it, so what an install has to
 * carry is only WHICH NAME, and the magic indexes the two tables read end to end. A second pair of bodies over
 * a second table would be the same twenty lines twice, and the two copies would be free to disagree about
 * §6.6.1's own paths. Each table is nonetheless installed from ITS OWN name, never through a shared pointer
 * or a magic range: which interface declares a descriptor is exactly the fact `pageRule.style.src` being
 * undefined rests on, so the two lists must not be reachable as one.
 *
 * CSS Fonts Module Level 5 §9.1's `interface CSSFontFaceDescriptors : CSSStyleDeclaration`, whose forty-one
 * attributes are these twenty-one names. CSS Fonts 4 §12.1 declares the same interface with SIX names fewer —
 * `font-size`, `size-adjust` and the four descriptors of CSS Fonts 5 §4.6 "Superscript and subscript metrics
 * overrides" — and Level 5 is the level webref extracts, so this list is Level 5's. THOSE FOUR WERE CITED AS
 * CSS Fonts 4 §6.10, "East Asian text rendering: the font-variant-east-asian property", which is not a
 * section CSS Fonts 5 has at all; they are descriptors of `@font-face`, not properties, which is why they are
 * numbered in the descriptor chapter and not in the feature one. */
static const struct { const char *dashed; const char *camel; } FONT_FACE_DESCRIPTORS[] = {
    { "src",                            "src" },
    { "font-family",                    "fontFamily" },
    { "font-style",                     "fontStyle" },
    { "font-weight",                    "fontWeight" },
    { "font-stretch",                   "fontStretch" },
    { "font-width",                     "fontWidth" },
    { "font-size",                      "fontSize" },
    { "size-adjust",                    "sizeAdjust" },
    { "unicode-range",                  "unicodeRange" },
    { "font-feature-settings",          "fontFeatureSettings" },
    { "font-variation-settings",        "fontVariationSettings" },
    { "font-named-instance",            "fontNamedInstance" },
    { "font-display",                   "fontDisplay" },
    { "font-language-override",         "fontLanguageOverride" },
    { "ascent-override",                "ascentOverride" },
    { "descent-override",               "descentOverride" },
    { "line-gap-override",              "lineGapOverride" },
    { "superscript-position-override",  "superscriptPositionOverride" },
    { "subscript-position-override",    "subscriptPositionOverride" },
    { "superscript-size-override",      "superscriptSizeOverride" },
    { "subscript-size-override",        "subscriptSizeOverride" },
};
#define FONT_FACE_DESCRIPTOR_N ((int)(sizeof(FONT_FACE_DESCRIPTORS) / sizeof(FONT_FACE_DESCRIPTORS[0])))

/* CSSOM §6.4.7's `interface CSSPageDescriptors : CSSStyleDeclaration`, whose fourteen attributes are these
   nine names in their two spellings. It derives from CSSStyleDeclaration and NOT from CSSStyleProperties, and
   that is the interface saying what CSS Paged Media §4.3 says: a page context holds page properties, so
   `pageRule.style.transform` is not a member and `pageRule.style.cssFloat` is undefined — which is what
   css/cssom/page-descriptors.html reads directly. */
static const struct { const char *dashed; const char *camel; } PAGE_DESCRIPTORS[] = {
    { "margin",           "margin" },
    { "margin-top",       "marginTop" },
    { "margin-right",     "marginRight" },
    { "margin-bottom",    "marginBottom" },
    { "margin-left",      "marginLeft" },
    { "size",             "size" },
    { "page-orientation", "pageOrientation" },
    { "marks",            "marks" },
    { "bleed",            "bleed" },
};
#define PAGE_DESCRIPTOR_N ((int)(sizeof(PAGE_DESCRIPTORS) / sizeof(PAGE_DESCRIPTORS[0])))
#define DESCRIPTOR_N (FONT_FACE_DESCRIPTOR_N + PAGE_DESCRIPTOR_N)
static int g_desc_set_id[DESCRIPTOR_N];

/* The descriptor `magic` names, asserted rather than clamped: the magic is written by the install loop below
   and by nothing else, so one out of range is this file disagreeing with itself. */
static const char *cssd_descriptor(int magic)
{
    DCHECK(magic >= 0 && magic < DESCRIPTOR_N,
           "a descriptor attribute ran with a magic neither descriptor table has");
    if (magic < FONT_FACE_DESCRIPTOR_N) return FONT_FACE_DESCRIPTORS[magic].dashed;
    return PAGE_DESCRIPTORS[magic - FONT_FACE_DESCRIPTOR_N].dashed;
}

/* Both halves forward exactly as §6.6.1's do — the getter is getPropertyValue and the setter is setProperty
   with no third argument — so a descriptor is read and written through the very paths a property is, and the
   block's declarations have ONE builder. There is no computed arm: a descriptor block is never a computed
   style (CSSOM §7.2's creator makes a CSSStyleProperties), which the flag asserts from the other side. */
static JSValue js_cssd_descriptor_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    const char *name = cssd_descriptor(magic);
    JSValue block = cssd_block(ctx, this_val), r;
    size_t len = 0;
    char *text, *v;

    if (JS_IsException(block)) return block;
    DCHECK(!cssd_flag(ctx, block, "computed"),
           "a descriptor attribute was read off a COMPUTED declaration block — CSSOM §7.2's getComputedStyle "
           "is the "
           "only creator that sets that flag and it mints a CSSStyleProperties, which has no descriptor "
           "attribute for this member to have been reached through");
    text = cssd_declarations_text(ctx, block, &len);
    v = cssd_property_value(text, len, name);
    free(text);
    r = v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
    free(v);
    JS_FreeValue(ctx, block);
    return r;
}

static JSValue js_cssd_descriptor_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *name = cssd_descriptor(magic);
    JSValue block = cssd_block(ctx, this_val);
    const char *v;

    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "readOnly")) {
        JS_FreeValue(ctx, block);
        return cssd_readonly_throw(ctx);
    }
    v = JS_ToCString(ctx, val);   /* a real string by now, and null is "" — the IDL says [LegacyNullToEmptyString] */
    if (!v) { JS_FreeValue(ctx, block); return JS_EXCEPTION; }
    cssd_write_declaration(ctx, block, name, *v ? v : NULL, false);
    JS_FreeCString(ctx, v);
    JS_FreeValue(ctx, block);
    return JS_UNDEFINED;
}

/* §6.6.1's cssText. Getting it is two steps and the FIRST is the computed one: "if the computed flag is set,
   then return the empty string", and only then "return the result of serializing the declarations". This used
   to hand back the element's `style` attribute BYTES for both — which is a real string belonging to a different
   question: it is not a serialization (`style="color:red"` reads back as `color: red;` in a browser), and for a
   computed block it is the inline declarations of a block that has none. */
static JSValue js_cssd_css_text(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue block = cssd_block(ctx, this_val), r;
    size_t len = 0;
    char *text, *out;

    (void)magic;
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "computed")) {
        JS_FreeValue(ctx, block);
        return JS_NewStringLen(ctx, "", 0);
    }
    text = cssd_declarations_text(ctx, block, &len);
    out = cssd_serialize_text(text, len);
    free(text);
    r = out ? JS_NewString(ctx, out) : JS_NewStringLen(ctx, "", 0);
    free(out);
    JS_FreeValue(ctx, block);
    return r;
}

/* Setting it: throw when readonly, then "empty the declarations" and parse the given value into them. Writing
   the value through unparsed is what the backing then re-parses, and for a rule it is what `cssRules` reports —
   so it goes through the same serialization every other write does, which is what drops an invalid declaration
   here rather than at every later read. */
static JSValue js_cssd_set_css_text(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue block = cssd_block(ctx, this_val);
    const char *v;
    char *out;

    (void)magic;
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "readOnly")) {
        JS_FreeValue(ctx, block);
        return cssd_readonly_throw(ctx);
    }
    v = JS_ToCString(ctx, val);
    if (!v) { JS_FreeValue(ctx, block); return JS_EXCEPTION; }
    out = cssd_serialize_text(v, strlen(v));
    cssd_declarations_write(ctx, block, out ? out : "", out ? strlen(out) : 0);
    free(out);
    JS_FreeCString(ctx, v);
    JS_FreeValue(ctx, block);
    return JS_UNDEFINED;
}

/* CSSOM §7.2's DECLARATIONS OF A COMPUTED BLOCK, which are not stored anywhere and are not the owner element's
   inline ones: "a list of CSS declarations ... with the following properties: ... declarations: the resolved
   value of every LONGHAND property that is a supported CSS property, in LEXICOGRAPHICAL ORDER, plus every
   custom property whose computed value is not the guaranteed-invalid value."
 *
 * THE SUPPORTED SET IS THE ONE THIS ENGINE CAN ANSWER FOR, and that is a real answer rather than a smaller
 * one: `item(i)` names a property, `getPropertyValue` of that name must return its resolved value, and a name
 * whose resolved value this build does not derive would crash the very read the enumeration invites. So the
 * set is `css_computed_models`' — core/css/css_computed_value.h's own list, which is where the `Computed
 * value:` lines live — minus anything css_shorthand.c records as a shorthand, since CSSOM §7.2 says LONGHAND.
 * THE NAMESPACE IS THE UNION OF TWO PLACES and both are asked, because neither alone is the engine's property
 * list: lexbor's registry, and the longhands css_shorthand.c owns the grammar of (the four `border-*-width`
 * and four `border-*-style`, which the registry does not carry at all).
 * NO CUSTOM PROPERTY IS ENUMERATED, and that is a positive statement rather than a gap: a custom property's
 * computed value comes from CSS Cascade §7's defaulting over a registration, and this engine registers none,
 * so every one of them holds the guaranteed-invalid value that CSSOM §7.2's own clause excludes. */
static void cssd_computed_name(CssDecls *d, const char *name)
{
    if (!css_computed_models(name) || css_shorthand_is_shorthand(name)) return;
    /* A COMPUTED BLOCK'S DECLARATIONS ARE NOT STORED and its resolved values are derived per read, so the entry
       is the NAME alone — every member that can meet a computed block answers from css_resolved_value before it
       reaches the declarations. */
    cssd_decls_append(d, cssd_strdup(name), NULL, false);
}

static void cssd_computed_names(CssDecls *d)
{
    uintptr_t id;
    unsigned k, nlh, j, i;

    /* THE SAME QUESTION §6.6.1's installers ask, asked THROUGH THE SAME PREDICATE. This walk had its own
       spelling of it — a start index and a null/empty guard — which let lexbor's custom-property marker
       through to be filtered by whether css_computed_models happens to model a property called `#сustom`.
       One "supported CSS property" question with two answers is how the two sites drift apart. */
    for (id = 0; id < LXB_CSS_PROPERTY__LAST_ENTRY; id++) {
        const lxb_css_entry_data_t *e = lxb_css_property_by_id(id);

        if (!cssom_supported_css_property(id, e)) continue;
        cssd_computed_name(d, (const char *)e->name);
    }
    for (k = 0; k < 2; k++) {
        const char *const *border = css_shorthand_longhands(k == 0 ? "border-width" : "border-style", &nlh);

        DCHECK(border != NULL,
               "css_shorthand.c stopped recording `border-width`/`border-style`, which are the only place the "
               "eight longhands lexbor's registry does not carry are named — the enumeration would silently "
               "lose eight properties whose computed value this build does resolve");
        for (j = 0; border && j < nlh; j++)
            cssd_computed_name(d, border[j]);
    }
    /* LEXICOGRAPHICAL ORDER, which CSSOM §7.2 states outright and which is therefore the enumeration's contract
       rather than a tidy-up: `item(i)` and `length` are the same list read two ways, and a page walking the
       indices expects the order the spec named. */
    for (i = 1; i < d->n; i++) {
        CssDecl cur = d->v[i];
        unsigned j2;

        for (j2 = i; j2 > 0 && strcmp(d->v[j2 - 1].name, cur.name) > 0; j2--) d->v[j2] = d->v[j2 - 1];
        d->v[j2] = cur;
    }
}

/* The block's declarations — CSSOM §7.2's list for a computed block, and what parsing the backing's text produces for
   the other two. Both §6.6.1 members that need them go through here. */
static void cssd_declared_decls(JSContext *ctx, JSValueConst block, CssDecls *out)
{
    size_t len = 0;
    char *text;

    if (cssd_flag(ctx, block, "computed")) { cssd_computed_names(out); return; }
    text = cssd_declarations_text(ctx, block, &len);
    cssd_decls_from_text(text, len, out);
    free(text);
}

/* §6.6.1 `readonly attribute unsigned long length` — "the number of CSS declarations in the declarations". It
   is minted UNSIGNED because the IDL says the type is, not because a block could hold 2**31 declarations: a
   `long` spelling of an `unsigned long` is the same disagreement between a declaration and its member that
   `item` carried below, and it is worth nothing to leave one of the pair right and the other wrong. */
/* THE NUMBER OF CSS DECLARATIONS, over a block already resolved — ONE implementation of it, because §6.6.1's
   `length`, its indexed getter's supported property indices and its `item`'s unknown-index assert all ask the
   SAME question. Three walks would be three chances for the size a member reports, the size a lookup is
   bounded by and the size a should-never-happen tests against to disagree, which is the one way that assert
   could be right about a block nobody has. The same reason dom_token_list.c has one set_size. */
static unsigned cssd_count(JSContext *ctx, JSValueConst block)
{
    CssDecls d = { 0 };
    unsigned n;

    cssd_declared_decls(ctx, block, &d);
    n = d.n;
    cssd_decls_free(&d);
    return n;
}

static JSValue js_cssd_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue block = cssd_block(ctx, this_val);
    unsigned n;

    (void)magic;
    if (JS_IsException(block)) return block;
    n = cssd_count(ctx, block);
    JS_FreeValue(ctx, block);
    return JS_NewUint32(ctx, n);
}

/* CSSOM §6.6.1 The CSSStyleDeclaration Interface: "The item(index) method must return the property name of the
   CSS declaration at position index. If there is no indexth object in the collection, then the method must
   return the empty string."

   THE INDEX IS `unsigned long`, AND THE ARRAY'S OWN LENGTH IS THEREFORE THE ONLY BOUND. §6.6.1 writes
   `getter CSSOMString item(unsigned long index)`; this was declared `long`, and under that declaration the
   `idx >= 0` half of the guard was LOAD-BEARING rather than defensive — Web IDL §3.2.4.5 long converts with
   ConvertToInt(V, 32, "signed"), whose final step is "If signedness is 'signed' and x ≥ 2^(bitLength−1), then
   return x − 2^bitLength", so the converted value reaches −2147483648, and `d.v` is a REAL ARRAY of exactly
   `d.n` CssDecl entries. Deleting that half without fixing the type is an out-of-bounds READ, which is why the
   type and the guard move in ONE diff and neither is a separate step. §3.2.4.6 unsigned long's
   ConvertToInt(V, 32, "unsigned") produces [0, 2**32−1] and `d.n` is an `unsigned`, so `i < d.n` is a single
   unsigned comparison covering the whole converted range: the negative arm is unreachable BY TYPE, not by a
   check, and there is nothing left for a second test to catch.
   THE COMPENSATION IS WHY THE WRONG TYPE SURVIVED: a declaration block cannot hold 2**31 declarations, so
   every value at or past 2**31 is past the end under either sign and the empty string is the answer both ways
   — the declaration's error had no discriminating input through this member. The declaration is the spec of
   the conversion; a body re-deriving the sign is the second copy of §3.2.4.9 Abstract operations' arithmetic
   that idl_args.c exists to prevent. */
static JSValue js_cssd_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue block = cssd_block(ctx, this_val), r;
    CssDecls d = { 0 };
    uint32_t i = 0;

    (void)magic;
    if (JS_IsException(block)) return block;
    DCHECK(argc >= 1, "§6.6.1's `item` reached its body with no index — its IDL argument is required");
    cssd_declared_decls(ctx, block, &d);
    if (concolic_is(argv[0])) {
        /* AN UNKNOWN INDEX, and it reaches the body unconverted because §3.2's conversion is a boundary
           unknown external input crosses AS ITSELF (idl_concolic_rule answers IDL_CONCOLIC_CROSSES for every
           integer type). The EMPTY block is the one length at which that has an answer rather than a fork:
           §6.6.1 returns the empty string for every index at or past the number of declarations, and at zero
           declarations that is every index there is.
           READING IT WITH `JS_ToInt64` INSTEAD — which is what stood here — IS THE SHAPE idl_args.h BANS BY
           NAME: a concolic is a real JSObject, so ToNumber reaches ToPrimitive and runs a getter from a plain
           C frame, which this engine aborts on somewhere inside the coercion rather than here at the member. */
        DCHECK(d.n == 0,
               "§6.6.1's `item` was given an UNKNOWN index into a NON-EMPTY declaration block — every property "
               "name in it is a distinct answer, so the read must FORK one flow per supported index (plus the "
               "empty-string arm for an index past the end) instead of deciding it here");
        r = JS_NewStringLen(ctx, "", 0);
    } else {
        JS_ToUint32(ctx, &i, argv[0]);   /* the declaration ran §3.2.4.6 unsigned long: already [0, 2**32-1] */
        r = i < d.n ? JS_NewString(ctx, d.v[i].name) : JS_NewStringLen(ctx, "", 0);
    }
    cssd_decls_free(&d);
    JS_FreeValue(ctx, block);
    return r;
}

/* §6.6.1: "The parentRule attribute must return the parent CSS rule." It was a `null` DATA property on the
   prototype — the right answer for the two element-backed blocks and a wrong one for CSSOM §6.4.3's, which is the
   rule itself, and a data property is shared by every block in the realm besides. */
static JSValue js_cssd_parent_rule(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue block = cssd_block(ctx, this_val), r;

    (void)magic;
    if (JS_IsException(block)) return block;
    r = cssd_parent_rule(ctx, block);
    JS_FreeValue(ctx, block);
    return r;
}

/* ---- CSSOM §6.6.1 The CSSStyleDeclaration Interface's INDEXED PROPERTY GETTER ------------------------------
 *
 * §6.6.1 writes `getter CSSOMString item(unsigned long index)`. THE `getter` KEYWORD IS THE HALF THAT WAS
 * MISSING: an operation declared `getter` is BOTH a named method and Web IDL §3.9 Legacy platform objects'
 * indexed property getter, so `el.style.item(0)` and `el.style[0]` are two spellings of one algorithm — and
 * only the first of them existed here. `el.style[0]` fell through to an ordinary property lookup, walked the
 * three hundred per-property accessors on CSSStyleProperties.prototype without matching one, and answered
 * `undefined` for a block whose first declaration a browser names. It is the shape §NO STUBS is about from the
 * other side: not a member returning opaque, a member that is not installed at all while the object LOOKS
 * complete, because the operation half of the same line is there.
 *
 * THE SUPPORTED PROPERTY INDICES ARE §6.6.1'S OWN, quoted: "The object's supported property indices are the
 * numbers in the range zero to one less than the number of CSS declarations in the declarations. If there are
 * no such CSS declarations, then there are no supported property indices." That is idl_indexed.c's contract
 * exactly — JS_UNDEFINED past the end means the property is NOT THERE, so `el.style[0]` on an empty block is
 * undefined and `0 in el.style` is false, which is the whole difference from the OPERATION, whose §6.6.1 text
 * ("If there is no indexth object in the collection, then the method must return the empty string") makes
 * `el.style.item(0)` the empty STRING on that same block.
 *
 * AND THE UNKNOWN-INDEX QUESTION THE OPERATION ASKS DOES NOT ARISE HERE, which is a statement about the two
 * paths and not an omission. js_cssd_item can be handed a concolic because a Web IDL §3.2 conversion crosses
 * unknown external input AS ITSELF (idl_args.h's IDL_CONCOLIC_CROSSES). A property LOOKUP cannot: the key
 * reaching idl_indexed_own_property is a JSAtom, so whatever produced it has already been through ToPropertyKey
 * and what arrives is a real string. The two therefore do not disagree about one question — they are asked
 * different ones, and the fork js_cssd_item's DCHECK names is owed by the member alone. The same split
 * dom_token_list.c records between its tl_item and js_tl_item. */
static uint32_t cssd_indexed_length(JSContext *ctx, JSValueConst self)
{
    JSValue block = cssd_block(ctx, self);
    uint32_t n;

    /* THE BRAND CANNOT FAIL HERE and that is why it is asserted rather than returned past: this decl is
       attached by cssd_new and by nothing else, in the same function that hangs the §6.6 record off the
       object, so an object carrying CSSD_INDEXED carries the record. cssd_block THROWS for a stranger, and a
       throw from inside a [[GetOwnProperty]] the class declares free of the page's code is a pending exception
       nobody is standing there to take. */
    DCHECK(!JS_IsException(block),
           "§6.6.1's indexed getter was resolved against an object with no CSS declaration block — the decl is "
           "installed only by cssd_new, which writes the record in the same call");
    n = cssd_count(ctx, block);
    JS_FreeValue(ctx, block);
    return n;
}

/* §6.6.1's `item` steps, reached as a LOOKUP: the property NAME of the CSS declaration at position index.
   JS_UNDEFINED past the end is idl_indexed.c's "not a supported property index" and is NOT the operation's
   empty string — see the banner. The bound is one unsigned comparison against `d.n` for the reason js_cssd_item
   records: Web IDL §3.2.4.6 unsigned long converts to [0, 2**32−1] and `d.n` is an `unsigned`, so a negative
   index is unreachable BY TYPE — and idl_indexed.c's own array-index-property-name parse has already refused
   `"-1"`, `"01"` and `"1.0"` before this is reached, so there is nothing left for a second test to catch. */
static JSValue cssd_indexed_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue block = cssd_block(ctx, self), r;
    CssDecls d = { 0 };

    DCHECK(!JS_IsException(block),
           "§6.6.1's indexed getter was asked for an item of an object with no CSS declaration block — the "
           "decl is installed only by cssd_new, which writes the record in the same call");
    cssd_declared_decls(ctx, block, &d);
    r = i < d.n ? JS_NewString(ctx, d.v[i].name) : JS_UNDEFINED;
    cssd_decls_free(&d);
    JS_FreeValue(ctx, block);
    return r;
}

/* NO NAMED PROPERTY GETTER and NO INDEX CACHE, both stated rather than left blank. §6.6.1 declares one getter
   and it is the indexed one — the per-property attributes (`style.color`) are ATTRIBUTES on the prototype, not
   named properties, which is why `style.nosuch` is undefined and not a lookup this decl answers. The cache is
   0 because the two callbacks above are not an O(i) walk of a live child list, which is what idl_indexed.h's
   scratch exists for: each is one parse of the block's declarations, the same parse js_cssd_length and
   js_cssd_item already pay per call. */
static const IdlIndexedDecl CSSD_INDEXED = { "CSSStyleDeclaration", cssd_indexed_length, cssd_indexed_item,
                                             NULL, 0 };

/* §6.6.1's CSSStyleProperties.prototype FOR THIS REALM — which is the prototype every block gets, because
   every one of them is a CSSStyleProperties. OWNED. */
static JSValue cssd_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_cssd_class);

    DCHECK(!JS_IsNull(proto),
           "CSSStyleProperties.prototype was asked for in a realm that never ran its install");
    return proto;
}

/* §6.6's CSS DECLARATION BLOCK, built with the four associated properties its three creators differ in. Exactly
   one of the owner node and the parent CSS rule is non-null in every one of them, which is also what makes
   "where are the declarations kept" answerable — so it is asserted here, at the only place a block is made,
   rather than discovered by a read that finds neither. `owner_node` is an element WRAPPER, so the element
   cannot go away underneath the block and the identity table stays the one place a node is named. */
static JSValue cssd_new(JSContext *ctx, JSValueConst proto, JSValueConst owner_node, JSValueConst parent_rule,
                        bool computed, bool readonly)
{
    JSValue obj, slots;
    JSAtom k;

    DCHECK(g_ready, "a CSS declaration block was minted before cssom_init ran");
    DCHECK(JS_IsObject(proto),
           "a CSS declaration block was minted in a realm that never ran its prototype install — WHICH "
           "interface the block is is the caller's to state, because §6.6's three creators and CSS Fonts 5 "
           "§9.1's fourth do not all make the same one");
    DCHECK(JS_IsNull(owner_node) != JS_IsNull(parent_rule),
           "§6.6's owner node and parent CSS rule are not two independent fields for this engine: one of them "
           "is where the declarations LIVE, so a block with both or with neither is a block whose declarations "
           "are kept in two places or in none");
    DCHECK(!computed || readonly,
           "a COMPUTED declaration block was minted WRITABLE. CSSOM §7.2 is the only creator that sets the "
           "computed "
           "flag and it sets the readonly flag in the same breath — a writable one would take §6.6.1's set-a-"
           "CSS-declaration path into a block whose declarations are computed per read and stored nowhere");
    /* AN INDEXED-PROPERTY OBJECT, because §6.6.1's `getter CSSOMString item(unsigned long index)` is what a
       CSSStyleDeclaration IS — see CSSD_INDEXED above. It was `JS_NewObjectProto`, a plain object, so every
       block this engine has ever minted answered `el.style[0]` with undefined. THIS IS THE ONE PLACE A BLOCK
       IS MADE (the assert directly above says so and is why it is the one place), which is what makes the
       getter reach all four prototypes — CSSStyleProperties, CSSFontFaceDescriptors, CSSPageDescriptors and
       the base — with no per-creator line to forget. */
    obj = idl_indexed_new(ctx, proto, &CSSD_INDEXED);
    if (JS_IsException(obj)) return obj;
    slots = idl_slots_new(ctx);
    k = JS_ValueToAtom(ctx, g_decl_key);
    CHECK(!JS_IsException(slots) && k != JS_ATOM_NULL, "the CSS declaration block record allocation failed");
    JS_SetPropertyStr(ctx, slots, "ownerNode", JS_DupValue(ctx, owner_node));
    JS_SetPropertyStr(ctx, slots, "parentRule", JS_DupValue(ctx, parent_rule));
    JS_SetPropertyStr(ctx, slots, "computed", JS_NewBool(ctx, computed));
    JS_SetPropertyStr(ctx, slots, "readOnly", JS_NewBool(ctx, readonly));
    JS_SetProperty(ctx, obj, k, slots);
    JS_FreeAtom(ctx, k);
    return obj;
}

/* CSSOM §6.4.3: "The style attribute must return a CSSStyleProperties object for the style rule" — computed flag
   unset, readonly flag unset, declarations the rule's own, parent CSS rule THIS, owner node null. */
JSValue cssom_style_properties_for_rule(JSContext *ctx, JSValueConst rule)
{
    JSValue proto = cssd_proto(ctx), out;

    DCHECK(css_rule_is(rule),
           "CSSOM §6.4.3's `style` was asked to back a declaration block with something that is not a CSS "
           "rule");
    out = cssd_new(ctx, proto, JS_NULL, rule, false, false);
    JS_FreeValue(ctx, proto);
    return out;
}

/* CSS Fonts 5 §9.1's `style` — see the header. The block's PROPERTIES are CSSOM §6.4.3's exactly (computed flag unset,
   readonly flag unset, declarations the rule's own, parent CSS rule the rule, owner node null); only the
   prototype differs, which is what makes the descriptors reachable and the properties not. */
JSValue cssom_font_face_descriptors_for_rule(JSContext *ctx, JSValueConst rule)
{
    JSValue proto = realm_value_get(ctx, g_font_face_proto_slot), out;

    DCHECK(css_rule_is(rule),
           "CSS Fonts 5 §9.1's `style` was asked to back a descriptor block with something that is not a CSS "
           "rule");
    out = cssd_new(ctx, proto, JS_NULL, rule, false, false);
    JS_FreeValue(ctx, proto);
    return out;
}

/* CSSOM §6.4.7's `style` — see the header. The block's PROPERTIES are CSSOM §6.4.3's exactly (computed flag unset,
   readonly flag unset, declarations "the declared descriptors in the rule, in specified order", parent CSS
   rule the rule, owner node null); only the prototype differs. */
JSValue cssom_page_descriptors_for_rule(JSContext *ctx, JSValueConst rule)
{
    JSValue proto = realm_value_get(ctx, g_page_proto_slot), out;

    DCHECK(css_rule_is(rule),
           "CSSOM §6.4.7's `style` was asked to back a descriptor block with something that is not a CSS "
           "rule");
    out = cssd_new(ctx, proto, JS_NULL, rule, false, false);
    JS_FreeValue(ctx, proto);
    return out;
}

/* CSSOM §7.1's ElementCSSInlineStyle: "The style attribute must return a CSSStyleProperties object whose readonly
   flag is unset, whose parent CSS rule is null, and whose owner node is this." [SameObject] is why the block is
   remembered on the element rather than rebuilt: a page holds `el.style` and compares it, and a fresh object
   per read makes every such comparison false — the same rule node identity follows. It is stored as an own
   SLOT, so it is per-flow like everything else on the wrapper. */
static JSValue js_el_get_style(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    JSAtom k;
    JSValue cur;

    (void)magic;
    /* Web IDL §3.7.6 Attributes' brand check — "if jsValue does not implement target ... throw a TypeError",
       in the steps that create an attribute getter — and a THROW rather than an assert: the member is on a
       prototype and a page
       reaches an accessor off one with `.call` on anything at all. Web IDL §3.7.5 is "Constants", which is
       what this cited, and a page cannot tell a citation apart from a recollection.
       Without it the block below would be minted
       over an owner node that is not a node, and the first read of its declarations would crash on an engine
       invariant that the page, not the engine, had broken. */
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return JS_ThrowTypeError(ctx, "the style attribute was reached on something that is not an element");
    k = JS_ValueToAtom(ctx, g_inline_key);
    CHECK(k != JS_ATOM_NULL, "the inline-style slot key could not be interned");
    if (JS_GetOwnSlot(ctx, &cur, this_val, k) <= 0)
        cur = JS_UNDEFINED;
    if (!JS_IsObject(cur)) {
        JSValue proto = cssd_proto(ctx);

        JS_FreeValue(ctx, cur);
        cur = cssd_new(ctx, proto, this_val, JS_NULL, false, false);
        JS_FreeValue(ctx, proto);
        JS_SetProperty(ctx, (JSValue)this_val, k, JS_DupValue(ctx, cur));
    }
    JS_FreeAtom(ctx, k);
    return cur;
}

/* CSSOM §7.2's getComputedStyle(elt, pseudoElt) — "return a live CSSStyleProperties object" with the computed flag
   SET, the readonly flag SET, the parent CSS rule null and the owner node the element. The pseudo-element
   argument is converted and rejected rather than ignored: this engine has no pseudo-element boxes, and
   answering the ORIGINATING element's values for `::before` would be a wrong answer rather than a missing
   one. */
static JSValue js_get_computed_style(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n;

    (void)magic; (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getComputedStyle requires an element");
    n = node_of(argv[0]);
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return JS_ThrowTypeError(ctx, "getComputedStyle requires an element");
    if (argc > 1 && JS_IsString(argv[1])) {
        const char *p = JS_ToCString(ctx, argv[1]);
        bool named = p && *p;
        JS_FreeCString(ctx, p);
        if (named)
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "pseudo-element computed styles are not modelled: this engine builds "
                                        "no pseudo-element boxes, and answering the originating element's "
                                        "values would be a wrong answer rather than a missing one");
    }
    {
        JSValue proto = cssd_proto(ctx), out = cssd_new(ctx, proto, argv[0], JS_NULL, true, true);

        JS_FreeValue(ctx, proto);
        return out;
    }
}

void cssom_init(JSContext *ctx)
{
    uintptr_t id;

    DCHECK(!g_ready, "cssom_init ran twice — one instance is one document");
    /* The shorthand table's own invariants, asserted before anything reads it — §6.6's serialization walks it
       in both directions and the cascade walks it in one, so a row that disagrees with itself is a wrong
       string and a wrong computed value at once. */
    css_shorthand_init();
    /* The numeric-production table's, likewise before anything reads it: it is read by BINARY SEARCH, so an
       out-of-order row is not a slow answer but a row the search never reaches — reported as a property the
       table has never heard of, two lines below the row that holds it. */
    css_property_numeric_init();
    /* The UA stylesheet's own invariants, for the same reason: it is scanned first-match-wins, so a duplicated
       row is a declaration that can never be reported and a transcription error nothing else would surface. */
    cssd_ua_table_check();
    g_parser = lxb_css_parser_create();
    CHECK(g_parser != NULL && lxb_css_parser_init(g_parser, NULL) == LXB_STATUS_OK,
          "the CSS parser could not be created");
    /* THE PARSER'S OWN SELECTOR STATE, installed here and never NULL again — see g_selectors above for the
       dangling stack frame this exists to make unreachable. `lxb_css_parser_init` leaves the field alone (it is
       NULL only because the parser is calloc'd), so this is the step that gives the parser one. */
    CHECK(lxb_css_parser_selectors_init(g_parser) == LXB_STATUS_OK,
          "the CSS parser's selector state could not be allocated");
    g_selectors = lxb_css_parser_selectors(g_parser);
    CHECK(g_selectors != NULL, "the CSS parser accepted its selector state and then reported none");
    g_decl_key = JS_NewSymbol(ctx, "cssDeclarationBlock", false);
    g_inline_key = JS_NewSymbol(ctx, "elementInlineStyle", false);
    CHECK(!JS_IsException(g_decl_key) && !JS_IsException(g_inline_key),
          "the CSS declaration block key allocations failed");
    {
        /* THE CLASS IS A PER-REALM PROTOTYPE HOLDER AND NOTHING ELSE — no block is ever an instance of it
           (`cssd_new` builds a plain object over the prototype it is handed), which is why a THIRD interface
           over the same record costs a value slot and not a class. `[object …]` is Web IDL §3.7.3's @@toStringTag on
           the prototype, so CSSStyleProperties and CSS Fonts 5 §9.1's CSSFontFaceDescriptors are told apart by
           a page even though their records are identical. */
        JSClassDef d = { "CSSStyleProperties" };
        JS_NewClassID(JS_GetRuntime(ctx), &g_cssd_class);
        JS_NewClass(JS_GetRuntime(ctx), g_cssd_class, &d);
    }
    g_declaration_proto_slot = realm_value_declare(ctx, "CSSOM §6.6.1 CSSStyleDeclaration.prototype");
    g_font_face_proto_slot = realm_value_declare(ctx, "CSS Fonts 5 §9.1 CSSFontFaceDescriptors.prototype");
    g_page_proto_slot = realm_value_declare(ctx, "CSSOM §6.4.7 CSSPageDescriptors.prototype");
    g_ready = 1;
    {
        static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
        /* §6.6.1 writes `getter CSSOMString item(unsigned long index)` and carries NO [EnforceRange], so
           §3.2.4.9 Abstract operations' ConvertToInt modulo IS the specified behaviour and there is nothing
           here to throw. The type states the SIGN, which is the whole of what it decides — see js_cssd_item
           for the array bound that used to be spelled twice because of it. */
        static const IdlArgType ONE_ULONG[1] = { IDL_UNSIGNED_LONG };
        static const IdlArgType THREE_STR[3] = { IDL_DOMSTRING, IDL_DOMSTRING, IDL_DOMSTRING };
        g_set_css_text_id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_cssd_set_css_text, 0);
        /* Web IDL §3.3.10's [PutForwards=cssText], which BOTH `style` attributes carry — CSSOM §7.1
           ElementCSSInlineStyle's on an element and CSSOM §6.4.3 CSSStyleRule's on a style rule — plus the CSS Fonts
           and CSSOM page/margin descriptor blocks below. The five steps are Web IDL §3.7.6 Attributes' and
           are declared ONCE for the whole platform (idl_args.c); this component states only the pair. It had
           its own copy, which is how it came to write Web IDL §3.7.6 step 4.5.8.4's Throw flag as `true` where the
           standard writes `false`, and to reach the forwarded-to setter with a JS_SetPropertyStr from C. */
        g_put_forwards_id = idl_setter_id_put_forwards(ctx, "style", "cssText");
        g_get_prop_id = idl_method_id(ctx, ONE_STR, 1, js_cssd_prop_op, 0);
        g_remove_prop_id = idl_method_id(ctx, ONE_STR, 1, js_cssd_prop_op, 1);
        g_get_priority_id = idl_method_id(ctx, ONE_STR, 1, js_cssd_prop_op, 2);
        g_set_prop_id = idl_method_id(ctx, THREE_STR, 3, js_cssd_set_property, 0);
        /* §6.6.1: `setProperty(CSSOMString property, CSSOMString value, optional CSSOMString priority = "")` */
        idl_optional_from(2);
        g_item_id = idl_method_id(ctx, ONE_ULONG, 1, js_cssd_item, 0);
        {
            /* CSSOM §7.2 Extensions to the Window Interface: `getComputedStyle(elt, optional pseudoElt)`.
               DECLARED HERE with the rest — the member lives on the WINDOW, which is per realm, and the
               declaration is the agent's. The number here was CSSOM §7.1, which is The ElementCSSInlineStyle
               Mixin: the sentence beside it already said WINDOW, and every other getComputedStyle citation in
               this file already said CSSOM §7.2. */
            static const IdlArgType TWO[2] = { IDL_ANY, IDL_DOMSTRING };
            g_id_gcs = idl_method_id(ctx, TWO, 2, js_get_computed_style, 0);
            idl_optional_from(1);
        }
    }
    /* §6.6.1 declares all three per-property spellings `[CEReactions] attribute [LegacyNullToEmptyString]
       CSSOMString`, so `el.style.color = null` REMOVES the declaration exactly as `""` does. This declared them
       with null_to_empty FALSE, which made `null` reach the setter as the four-character string "null" and
       declare `color: null` — the descriptors beside them had it right, which is what made the disagreement
       readable. One id per property, shared by the property's camel-cased, webkit-cased and dashed attributes,
       because §6.6.1 gives all three the same setter steps. */
    for (id = 0; id < LXB_CSS_PROPERTY__LAST_ENTRY; id++)
        g_property_set_id[id] = cssom_supported_css_property(id, lxb_css_property_by_id(id))
                              ? idl_setter_id(ctx, IDL_DOMSTRING, true, js_cssd_property_set, (int)id)
                              : -1;
    {
        /* CSS Fonts 5 §9.1 and CSSOM §6.4.7 both declare every descriptor attribute
           `[LegacyNullToEmptyString]`, so `null` reaches the setter as "" and REMOVES the descriptor rather
           than declaring the string "null". */
        int i;

        for (i = 0; i < DESCRIPTOR_N; i++)
            g_desc_set_id[i] = idl_setter_id(ctx, IDL_DOMSTRING, true, js_cssd_descriptor_set, i);
    }
    realm_declare_intrinsic(cssom_install_proto);
}

/* FOUR INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM, because four specs split them. CSSOM §6.6.1 The
   CSSStyleDeclaration Interface's `interface CSSStyleProperties` carries `cssFloat`, and its three
   `partial interface CSSStyleProperties` blocks the per-property camel-cased, webkit-cased and dashed
   attributes; CSS Fonts 5 §9.1 The CSSFontFaceRule interface's
   `interface CSSFontFaceDescriptors : CSSStyleDeclaration` carries the forty-one `@font-face` DESCRIPTORS its
   twenty-one names spell, and CSSOM §6.4.7 The CSSPageRule Interface's
   `interface CSSPageDescriptors : CSSStyleDeclaration` the fourteen `@page` ones its nine names spell, which
   are different sets with a different source (see the tables above); and
   CSSStyleDeclaration carries the block's own eight members, which all three inherit. Installing all of them on
   one object made `CSSStyleProperties` an absent global — an honest ReferenceError for an interface every one
   of this engine's blocks IS — and made `Object.getOwnPropertyNames(CSSStyleDeclaration.prototype)` report
   three hundred property attributes a browser does not have there. Nothing is an instance of the base, so it
   holds no class of its own, exactly as CSSOM §6.1.1's StyleSheet and CSSOM §6.4.2's CSSRule do. */
void cssom_install_proto(JSContext *ctx)
{
    JSValue base, proto, descriptors, page, prev;
    int d;

    DCHECK(g_ready, "a realm asked for the declaration-block prototypes before the interfaces were declared");
    prev = JS_GetClassProto(ctx, g_cssd_class);
    DCHECK(JS_IsNull(prev), "cssom_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = JS_NewObject(ctx);
    CHECK(!JS_IsException(base), "CSSStyleDeclaration.prototype could not be allocated");
    idl_interface_tag(ctx, base, "CSSStyleDeclaration");
    idl_install_accessor(ctx, base, "parentRule", js_cssd_parent_rule, 0, -1);
    idl_install_accessor(ctx, base, "length", js_cssd_length, 0, -1);
    idl_install_accessor(ctx, base, "cssText", js_cssd_css_text, 0, g_set_css_text_id);
    idl_install_method(ctx, base, "getPropertyValue", g_get_prop_id);
    idl_install_method(ctx, base, "removeProperty", g_remove_prop_id);
    idl_install_method(ctx, base, "getPropertyPriority", g_get_priority_id);
    idl_install_method(ctx, base, "setProperty", g_set_prop_id);
    idl_install_method(ctx, base, "item", g_item_id);
    /* Web IDL §3.7.9 Iterable declarations' define the iteration methods, step 1.1: "If definition has an indexed
       property getter, then: Perform DefineMethodProperty(target, %Symbol.iterator%, %Array.prototype.values%,
       false)." §6.6.1 has one, and an integer `length` beside it, so `[...el.style]` and `for (const p of
       el.style)` are ordinary code — which is how a bundle enumerates the properties it set.
       THIS SITE WAS ONCE THE ONLY CALLER OF THIS FUNCTION WITH THE RIGHT NUMBER, and its note said so; the
       other callers have since been corrected to §3.7.9 and the note would now be describing a tree that no
       longer exists, so what survives is the REASON rather than the census. §3.7.10 is "Asynchronous iterable
       declarations", the clause that owns `async_iterable<>` and that idl_async_iter.c cites CORRECTLY, and it
       forecloses this clause in its own words — its step 2 asserts a definition reaching it "does not have an
       indexed property getter or an iterable declaration". The wrong number survived here because nothing
       mechanical could see it: citegen resolves a number that EXISTS, and a bare number with no title or
       algorithm beside it gives it nothing to compare. That is why every citation of this clause in the engine
       now names the algorithm — and it has to, because §2.5.9 carries the SAME TITLE as §3.7.9 (the
       declaration and its terminology, against this section's binding steps), so a title alone would not have
       disambiguated it either.
       IT IS ON THE BASE and not on CSSStyleProperties.prototype because §3.7.9 defines the iteration methods
       on the interface prototype object of the interface that DECLARES the getter, which is
       CSSStyleDeclaration; the other three prototypes inherit from this one.
       §6.6.1 declares NO `iterable<>`, so `entries`, `keys`, `values` and `forEach` are honestly absent — the
       same split HTMLCollection and FileList are on, and the reason idl_indexed.h keeps the two installs
       apart. */
    idl_indexed_install_iterable(ctx, base);

    proto = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(proto), "CSSStyleProperties.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CSSStyleProperties");
    /* §6.6.1's THREE PER-PROPERTY PARTIAL INTERFACES, each generated from LEXBOR'S OWN CSS PROPERTY REGISTRY:
       §6.6.1 states each of them "for each CSS property property that is a supported CSS property", so the
       registry IS the list — typing a hundred names here would be a second copy of it that could disagree, and
       inventing them would be worse. The three are called here, side by side, because that is what the spec
       declares: three partial interfaces on this one prototype. */
    cssom_install_camel_cased_attributes(ctx, proto);
    cssom_install_webkit_cased_attributes(ctx, proto);
    cssom_install_dashed_attributes(ctx, proto);
    {
        /* §6.6.1's `cssFloat` IS A MEMBER OF `interface CSSStyleProperties` ITSELF — the IDL declares it there
           and not in any of the three partials — AND IT IS A SECOND ATTRIBUTE OVER THE SAME PROPERTY, NOT A
           RENAME OF THE FIRST. The CSS property to IDL attribute algorithm has no float case, so it produces
           `float`, and §6.6.1 then declares `cssFloat` separately, defined to invoke setProperty "with float as
           first argument". Renaming inside the generated loop DELETED `float`, so `element.style.float` was
           undefined in this engine and it is a property of every browser. */
        const lxb_css_entry_data_t *f = lxb_css_property_by_name((const lxb_char_t *)"float", 5);

        DCHECK(f != NULL && f->unique != LXB_CSS_PROPERTY__UNDEF,
               "CSSOM §6.6.1's `cssFloat` forwards to the `float` property and lexbor's registry has no such "
               "row, so the member would answer for a property that does not exist");
        cssom_install_property_attribute(ctx, proto, f->unique, "cssFloat");
    }

    /* CSS Fonts 5 §9.1's CSSFontFaceDescriptors.prototype and CSSOM §6.4.7's CSSPageDescriptors.prototype,
       each over ITS OWN table. Both spellings are installed because the IDL declares both, and the dashed one
       is exactly what css/cssom/cssstyledeclaration-cssfontrule.tentative.html reads (`"unicode-range" in
       style`) and what css/cssom/page-descriptors.html asserts as an own property of the prototype. The
       `strcmp` is not a guard against a bad name: it is the one-word case, where the IDL declares a single
       attribute and the table's two columns coincide to say so. */
    descriptors = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(descriptors), "CSSFontFaceDescriptors.prototype could not be allocated");
    idl_interface_tag(ctx, descriptors, "CSSFontFaceDescriptors");
    for (d = 0; d < FONT_FACE_DESCRIPTOR_N; d++) {
        idl_install_accessor(ctx, descriptors, FONT_FACE_DESCRIPTORS[d].dashed, js_cssd_descriptor_get, d,
                             g_desc_set_id[d]);
        if (strcmp(FONT_FACE_DESCRIPTORS[d].camel, FONT_FACE_DESCRIPTORS[d].dashed) != 0)
            idl_install_accessor(ctx, descriptors, FONT_FACE_DESCRIPTORS[d].camel, js_cssd_descriptor_get, d,
                                 g_desc_set_id[d]);
    }

    page = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(page), "CSSPageDescriptors.prototype could not be allocated");
    idl_interface_tag(ctx, page, "CSSPageDescriptors");
    for (d = 0; d < PAGE_DESCRIPTOR_N; d++) {
        int m = FONT_FACE_DESCRIPTOR_N + d;

        idl_install_accessor(ctx, page, PAGE_DESCRIPTORS[d].dashed, js_cssd_descriptor_get, m,
                             g_desc_set_id[m]);
        if (strcmp(PAGE_DESCRIPTORS[d].camel, PAGE_DESCRIPTORS[d].dashed) != 0)
            idl_install_accessor(ctx, page, PAGE_DESCRIPTORS[d].camel, js_cssd_descriptor_get, m,
                                 g_desc_set_id[m]);
    }

    JS_SetClassProto(ctx, g_cssd_class, proto);
    realm_value_set(ctx, g_declaration_proto_slot, base);
    realm_value_set(ctx, g_font_face_proto_slot, descriptors);
    realm_value_set(ctx, g_page_proto_slot, page);
}

int cssom_put_forwards_setter(void)
{
    DCHECK(g_put_forwards_id >= 0,
           "Web IDL §3.3.10's [PutForwards=cssText] setter was asked for before cssom_init declared it — the "
           "`style` attributes that carry it are installed onto prototypes this component's init runs ahead of");
    return g_put_forwards_id;
}

void cssom_install_style_attribute(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "the style attribute was installed before cssom_init ran");
    idl_install_accessor(ctx, proto, "style", js_el_get_style, 0, g_put_forwards_id);
}

void cssom_install(JSContext *ctx, JSValueConst global)
{
    JSValue base = realm_value_get(ctx, g_declaration_proto_slot);
    JSValue proto = cssd_proto(ctx);

    DCHECK(g_ready, "the declaration-block interfaces were installed before cssom_init ran");
    DCHECK(JS_IsObject(base),
           "the declaration-block interfaces were installed in a realm that never ran their prototype install");
    node_install_interface(ctx, global, "CSSStyleDeclaration", base);
    node_install_interface(ctx, global, "CSSStyleProperties", proto);
    {
        JSValue descriptors = realm_value_get(ctx, g_font_face_proto_slot);

        DCHECK(JS_IsObject(descriptors),
               "CSSFontFaceDescriptors was installed in a realm that never ran its prototype install");
        node_install_interface(ctx, global, "CSSFontFaceDescriptors", descriptors);
        JS_FreeValue(ctx, descriptors);
    }
    {
        JSValue page = realm_value_get(ctx, g_page_proto_slot);

        DCHECK(JS_IsObject(page),
               "CSSPageDescriptors was installed in a realm that never ran its prototype install");
        node_install_interface(ctx, global, "CSSPageDescriptors", page);
        JS_FreeValue(ctx, page);
    }
    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, proto);
    idl_install_method(ctx, global, "getComputedStyle", g_id_gcs);
}

void cssom_free(JSRuntime *rt)
{
    if (!g_ready) return;
    JS_FreeValueRT(rt, g_decl_key);   /* the prototypes are the REALMS' — released with their contexts */
    JS_FreeValueRT(rt, g_inline_key);
    g_decl_key = g_inline_key = JS_UNDEFINED;
    /* The selector state is released BY NAME: `lxb_css_parser_destroy` frees the parser's stack, rules, string
       buffer, log and tokenizer and does NOT touch `selectors`, so the record installed in cssom_init is this
       component's to free — and freeing it after the parser would read a pointer out of freed memory. */
    if (g_parser) {
        lxb_css_parser_selectors_destroy(g_parser);
        g_selectors = NULL;
        lxb_css_parser_destroy(g_parser, true);
        g_parser = NULL;
    }
    g_ready = 0;
}
