/* CSSOM §6.6 — the CSS DECLARATION BLOCK, its two interfaces, and getComputedStyle().
 *
 * WHAT WAS HERE BEFORE: nothing. `el.style.display = 'none'` wrote an ordinary JS property on an object that
 * did not exist, and getComputedStyle was absent, so a page reading a computed value threw. Both are named in
 * this project's own rules as the shape of a fidelity gap — a getter returning opaque where the SPEC COMPUTES A
 * REAL VALUE — and both matter to what this engine is for: a bundle that branches on
 * `getComputedStyle(el).display === 'none'` routes differently on each side, and each side has its own
 * endpoints.
 *
 * A BLOCK IS THE FIVE THINGS §6.6 SAYS IT IS, AND THAT IS WHAT DECIDES EVERY MEMBER'S ANSWER: the COMPUTED
 * FLAG, the READONLY FLAG, the DECLARATIONS, the PARENT CSS RULE and the OWNER NODE. Three creators differ only
 * in what they set those to — §7.1's `element.style` (owner node this, no parent rule, neither flag), §7.2's
 * getComputedStyle (owner node the element, both flags) and §6.4.3's `rule.style` (NO owner node, parent rule
 * the rule, neither flag). They used to be a two-valued `mode`, which could express the first two and had no
 * room for the third, and whose "no element" arm answered the empty string for every member — so a block with
 * no owner node, which is exactly what a rule's is, would have read as an empty declaration block rather than
 * as the rule's own declarations.
 *
 * LEXBOR OWNS THE CSS, and that is the point of binding to it rather than hand-rolling. It has the real
 * property registry (so the camel-cased IDL attributes are GENERATED FROM THE SPEC'S OWN PROPERTY LIST rather
 * than typed here), a real declaration parser, real value serializers, and a real selector matcher that answers
 * for a SINGLE node. Every layer below is Lexbor doing the parsing and this file doing the cascade.
 *
 * THE DECLARATIONS ARE THE BACKING'S OWN TEXT, which is the design decision the rest follows from. §6.6 models
 * them as a list the block object holds and pushes back to the `style` attribute through "update style
 * attribute"; this engine keeps the BACKING authoritative and derives the list per read, because the backing is
 * what time-travels. For an element that backing is the `style` CONTENT ATTRIBUTE: lexbor can also hold parsed
 * styles on the element (an AVL keyed by property), and using that would have been faster and WRONG — it lives
 * outside the per-flow DOM delta, so a `style.color` written by one forked arm would be visible to its sibling,
 * while the attribute IS captured. For a rule it is the rule record's block TEXT, captured by the same per-flow
 * COW delta through the record's accessor, so two flows can disagree about `rule.style.color` exactly as they
 * can about an inline style. Nothing is cached in between, for the reason the cascade caches nothing.
 *
 * THE CASCADE IS RESOLVED LIVE, per read, from THE RUNNING FLOW'S OWN OBJECTS: the author rules of the CSS
 * STYLE SHEETS in §6.2's list for this element's root — the sheet objects a page holds, whose rules it inserts,
 * deletes and retargets — the element's own style attribute, css-cascade-5 §6.5's AUTHOR PRESENTATIONAL HINT
 * ORIGIN (core/css/css_presentational_hints.h, which is where HTML's own markup enters the cascade and why it
 * is an origin of its own rather than a row of the UA table), and the UA default. THEY ARE COLLECTED, NOT ASKED
 * IN TURN: each contributes its declarations into ONE list and core/css/css_cascade.h sorts that list by §6.1's
 * six criteria, because the criteria are LEXICOGRAPHIC and asking the origins in sequence silently reorders
 * them (it puts §6.1's Element-Attached criterion above its Origin-and-Importance one, and it cannot express
 * §6.4's Layers criterion at all, which sits above Specificity). Nothing is cached across a read, because a
 * cache would be shared state that the flow machinery does not swap; and reading the OBJECTS rather than
 * re-parsing each `<style>` element's text is what makes the objects load-bearing instead of inert, which is
 * the whole of why §6.1, §6.2 and §6.4 exist.
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
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/selector_match.h"
#include "core/css/css_cascade.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_keyframes.h"
#include "core/css/css_page.h"
#include "core/css/css_presentational_hints.h"
#include "core/css/css_rule.h"
#include "core/css/css_shorthand.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_style_sheet.h"
#include "core/css/style_sheet_list.h"
#include "solver/dom_cow.h"

/* TWO PRIVATE KEYS, BOTH SYMBOLS, AND THEY ARE TWO BECAUSE A SLOT IS A BRAND. `g_decl_key` hangs a block's own
   §6.6 record off the block; `g_inline_key` hangs §7.1's [SameObject] block off the ELEMENT's wrapper. One key
   served both, which made an element pass the block's brand check — `CSSStyleDeclaration.prototype.item.call(el,
   0)` found the declaration object where the record belongs and read a field out of it — the same defect
   style_sheet_list.c records for its own two keys. */
static JSValue g_decl_key = JS_UNDEFINED, g_inline_key = JS_UNDEFINED;
/* PER REALM — §3.7, and here it decides ANSWERS: a C member runs in the realm that DEFINED it. Every block this
   engine builds is a §6.6.1 CSSStyleProperties (all three creators say so), so THAT is the class, and
   CSSStyleDeclaration.prototype — the base nothing is an instance of — is a per-realm value slot beside it,
   which is the same shape §6.1.1's StyleSheet and §6.4.2's CSSRule take. */
static JSClassID g_cssd_class;
static int       g_declaration_proto_slot = -1;
/* CSS Fonts §12.1's CSSFontFaceDescriptors.prototype, the same way and for the same reason. It is a THIRD
   prototype over the SAME class and the same record: an `@font-face` block's declarations are kept where
   §6.4.3's are (the rule's own text, through core/css/css_rule.h), so what differs is only which member names
   the interface answers to. */
static int       g_font_face_proto_slot = -1;
/* CSSOM §6.4.7's CSSPageDescriptors.prototype, the same way and for the same reason — a FOURTH prototype over
   the one class and the one record. A `@page` rule's descriptors are kept where §6.4.3's declarations are (the
   rule's own text, through core/css/css_rule.h), so what differs is only which member names the interface
   answers to and, through core/css/css_page.h, which declarations the block admits at all. */
static int       g_page_proto_slot = -1;
/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. The camel-cased
   attributes are GENERATED from Lexbor's property registry, so their setter ids are an array indexed the same
   way the registry is — one entry per property, declared once, installed into every realm. */
static int g_set_css_text_id = -1, g_get_prop_id = -1, g_remove_prop_id = -1, g_get_priority_id = -1,
           g_set_prop_id = -1, g_item_id = -1, g_put_forwards_id = -1;
static int g_camel_set_id[LXB_CSS_PROPERTY__LAST_ENTRY];
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
   one to anything at all and §3.7.5's answer is a TypeError — which a page tells apart from the empty string
   the "no element" arm used to hand back. Returns JS_EXCEPTION with the error already thrown. OWNED. */
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

/* §6.6's OWNER NODE, as the element it is. NULL is a REAL state and not a failure: §6.4.3's block has none. */
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
 * appropriate CSS specifications", which for a shorthand is CSS Cascade §Shorthand Properties' "sets all of
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

    if (at >= 0) {
        if (d->v[at].important && !important) { free(value); return; }
        cssd_decls_remove_at(d, (unsigned)at);
    }
    cssd_decls_append(d, cssd_strdup(name), value, important);
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
       reached — including the one `animation-timing-function` that §3 admits as a declaration of its own. */
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
           §7.3: "As specified in CSS Values and Units, ALL CSS PROPERTIES CAN ACCEPT THESE VALUES." Lexbor's
           value grammar carries `initial`, `inherit`, `unset` and `revert` and predates css-cascade-5's §7.3.5
           and §7.3.6, so `height: revert-layer` fails that grammar and arrives here as an invalid declaration
           while `translate: revert-layer` — a property the grammar does not type at all — arrives as a value.
           Dropping the first would make a CSS-wide keyword mean something different depending on which
           properties the vendored parser happens to know, which is a wrong answer per property rather than a
           missing capability. `undef->type` carries the real property id and `undef->value` the raw source
           span (the `!important` is a separate offset and is already on the declaration), so both halves of
           the declaration survive. */
        if (d->type == LXB_CSS_PROPERTY__UNDEF) {
            char *raw = cssd_decl_value(d);
            bool wide = raw != NULL && css_wide_keyword(raw);

            if (wide) {
                name = cssd_decl_name(d);   /* NULL when lexbor has no id for the property either */
                if (name) {
                    cssd_decls_collect_declaration(out, name, raw, d->important);
                    free(name);
                }
            }
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

/* §6.6.1's GET PROPERTY VALUE over a block's text, including the step that is only reachable now that the
   longhand->shorthand direction exists: "if property is a shorthand property ... for each longhand property
   longhand that property maps to, in canonical order ... if declaration is null, then return the empty string
   ... if important flags of all declarations in list are same, then return the serialization of list."
   IT IS THE SAME WALK FOR BOTH BACKINGS AND FOR A SHORTHAND SPELLED EITHER WAY, because the declarations it
   reads are LONGHANDS whichever spelling produced them. So `border` reads back out of a block holding
   `border: 1px solid red` (all seventeen longhands answer, and consolidate to what was written) and reads back
   as the EMPTY STRING out of one holding only `border-width`/`border-style`/`border-color` (the five
   `border-image` longhands answer nothing, and §3.4's `border` is the shorthand that resets them) — which is
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

/* §6.1's ELEMENT-ATTACHED declaration for `name`, or NULL — the contents of the style attribute, which is
   per-flow because the attribute it reads is. The CASCADE reaches it with an element and no declaration object,
   which is why this takes one rather than a block, and it reports the IMPORTANCE because §6.1 compares that
   first: attached-ness is the criterion BELOW origin and importance, not above it. */
static char *cssd_inline_value(lxb_dom_element_t *el, const char *name, bool *pimportant)
{
    size_t len = 0;
    const char *text = cssd_inline_text(el, &len);

    return cssd_value_in_block(text, len, name, pimportant);
}

/* THE AUTHOR ORIGIN's declarations, collected from THE SHEET OBJECTS of the element's root.
 *
 * IT USED TO RE-WALK THE `<style>` ELEMENTS AND RE-PARSE THEIR TEXT, AND THAT WAS TWO MECHANISMS DESCRIBING ONE
 * FACT — the one that answered questions was not the one the page mutated. §6.1's sheets, §6.4's rules and
 * their §6.6 declaration blocks are the document's style; the elements are where they came FROM. So
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
 * WHAT THE TEXT CANNOT CARRY IS THE CASCADE LAYER, so the emission reports one per rule beside it. §6.1 puts
 * §6.4's Layers criterion ABOVE Specificity, which is why flattening a `@layer` block's children into the
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

/* The sheet's serialization, rebuilt from its RULE OBJECTS, with the §6.4.3 cascade layer of every rule that
   went into it. THE FLATTENING IS core/css/css_rule.h's, because deciding which rules apply is §6.4's business
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

/* EVERY AUTHOR-ORIGIN DECLARATION OF `name` ON `el`, added to `cascade` — not the one that wins. §6.1's sort is
   over the whole list at once and §7.3's roll-backs re-run it with a part removed, so a collector that kept
   only a running best would have thrown away exactly what both need. `*pseq` is the document-order counter
   §6.1's Order of Appearance reads, carried across the sheets so it is one sequence and not one per sheet, and
   bumped per RULE so it also names the rule §7.3.6's `revert-rule` removes. */
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
           "author layer IS §6.2's list of CSS style sheets, which lives on that document's root wrapper, so a "
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
               "§6.2's list holds something that is not a CSS style sheet — its add is the one thing that ever "
               "puts one in");
        /* §6.1.1's DISABLED FLAG, which is what it is FOR: "the disabled attribute ... whether the style sheet
           is applied". `sheet.disabled = true` and `<style disabled>`'s forwarding both land here, and until
           the cascade read the objects neither could do anything at all. */
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
                   counter can be both §6.1's order of appearance and §7.3.6's identity of the rule. */
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
        /* §6.1's Order of Appearance across SHEETS: "declarations from style sheets independently linked by the
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
       CSS Logical §2 is why: the two members of such a pair "share a computed value ... determined by
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
   §6.4's serialize-a-CSS-rule reads as its "null if there are no such declarations". */
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
   one this engine has no interface for and crashes on anyway; it stopped being invisible when §6.4.7's
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
            kids = st->child;                     /* CSS Nesting's own rules, which §6.4.5 makes `cssRules` */
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
                   "the only thing that can say which §6.4 interface the rule wants");
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
               holds BOTH and §6.4.7's `@page` is exactly one: page descriptors beside §4.3's margin at-rules.
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
   bundle actually branches on. Modelling it is the difference between answering the spec's value and shrugging;
   what is NOT here — the rest of html.css — is honestly absent and reads as the property's initial value. */
static const struct { const char *tag; const char *prop; const char *value; } UA_DEFAULT[] = {
    { "html", "display", "block" },  { "body", "display", "block" },
    { "div", "display", "block" },   { "p", "display", "block" },
    { "h1", "display", "block" },    { "h2", "display", "block" },
    { "h3", "display", "block" },    { "h4", "display", "block" },
    { "h5", "display", "block" },    { "h6", "display", "block" },
    { "ul", "display", "block" },    { "ol", "display", "block" },
    { "li", "display", "list-item" },{ "form", "display", "block" },
    { "header", "display", "block" },{ "footer", "display", "block" },
    { "section", "display", "block" }, { "article", "display", "block" },
    { "nav", "display", "block" },   { "aside", "display", "block" },
    { "main", "display", "block" },  { "figure", "display", "block" },
    { "table", "display", "table" }, { "tr", "display", "table-row" },
    { "td", "display", "table-cell" }, { "th", "display", "table-cell" },
    /* §15.3.1's FIRST RULE, entire: `area, base, basefont, datalist, head, link, meta, noembed, noframes,
       param, rp, script, style, template, title { display: none }`. Seven of the fourteen used to be here and
       seven were not, which is not a smaller stylesheet — it is a `<datalist>` this engine says generates a
       box, and every §6 geometry member and HTML's `being rendered` reading that answer. */
    { "area", "display", "none" },   { "base", "display", "none" },
    { "basefont", "display", "none" }, { "datalist", "display", "none" },
    { "head", "display", "none" },   { "link", "display", "none" },
    { "meta", "display", "none" },   { "noembed", "display", "none" },
    { "noframes", "display", "none" }, { "param", "display", "none" },
    { "rp", "display", "none" },     { "script", "display", "none" },
    { "style", "display", "none" },  { "template", "display", "none" },
    { "title", "display", "none" },
};

/* §15.3.1's `hidden` RULES, which are ATTRIBUTE selectors and therefore outrank every type selector in the
   table above — so they are asked first, and they are asked HERE rather than by each component that wants to
   know whether an element generates a box. element_view.c walked the ancestor chain for the attribute itself,
   which is the same rule implemented a second time and implemented WRONG in two ways this one is not: it read
   `hidden="until-found"` (which §15.3.1 makes `content-visibility: hidden`, a rendered element) as a removed
   box, and it ignored `embed[hidden]`; and being outside the cascade it could not be overridden by the author
   rule that outranks it, so a page's own `[hidden] { display: block }` did nothing.
     [hidden]:not([hidden=until-found i]):not(embed) { display: none }
     embed[hidden] { display: inline; height: 0; width: 0 } */
static const char *cssd_ua_hidden(lxb_dom_element_t *el, const lxb_char_t *tag, size_t taglen)
{
    size_t vlen = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"hidden", 6, &vlen);
    static const char UNTIL_FOUND[] = "until-found";
    size_t i;

    if (!v) return NULL;
    if (taglen == 5 && memcmp(tag, "embed", 5) == 0) return "inline";
    if (vlen == sizeof(UNTIL_FOUND) - 1) {
        for (i = 0; i < vlen; i++)
            if ((char)tolower((unsigned char)v[i]) != UNTIL_FOUND[i]) break;
        if (i == vlen) return NULL;   /* `until-found` sets content-visibility, and the box stays */
    }
    return "none";
}

static const char *cssd_ua_value(lxb_dom_element_t *el, const char *name)
{
    size_t n = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &n);
    unsigned i;

    if (!tag) return NULL;
    if (strcmp(name, "display") == 0) {
        const char *h = cssd_ua_hidden(el, tag, n);
        if (h) return h;
    }
    for (i = 0; i < sizeof(UA_DEFAULT) / sizeof(UA_DEFAULT[0]); i++)
        if (strlen(UA_DEFAULT[i].tag) == n && memcmp(UA_DEFAULT[i].tag, tag, n) == 0 &&
            strcmp(UA_DEFAULT[i].prop, name) == 0)
            return UA_DEFAULT[i].value;
    /* Every element the table does not name is `display: inline`, which is the UA sheet's own default. */
    if (strcmp(name, "display") == 0) return "inline";
    return NULL;
}

/* THE INITIAL VALUES LEXBOR'S REGISTRY DOES NOT CARRY. An initial value is a fact about the PROPERTY, stated
   on its own `Initial:` line, and it exists whether or not the vendored parser has a generated entry for it —
   so a property lexbor does not know still has one, and answering NULL for it is not "undeclared", it is a
   cascade that stopped a layer early. Lexbor carries the `border` and `border-<side>` SHORTHANDS and the four
   `border-*-color` longhands and nothing else of the border, so the eight below have no entry: the four widths
   are `medium` (css-backgrounds-3 §3.3) and the four styles are `none` (§3.2), which together are why the
   spec's own note says "although the initial width is medium, the initial style is none; therefore the used
   initial width is 0". The registry is still asked FIRST for every property, and a name here that lexbor DOES
   carry would be one fact with two sources — asserted below rather than assumed. */
static const struct { const char *name; const char *initial; } CSSD_INITIAL_UNREGISTERED[] = {
    { "border-top-width", "medium" }, { "border-right-width", "medium" },
    { "border-bottom-width", "medium" }, { "border-left-width", "medium" },
    { "border-top-style", "none" }, { "border-right-style", "none" },
    { "border-bottom-style", "none" }, { "border-left-style", "none" },
};

/* THE INITIAL VALUES LEXBOR'S REGISTRY GETS WRONG, each with the answer it gives today so the row EXPIRES.
   This is a different table from the one above and deliberately so: there the registry is silent and the fact
   has one source, here it SPEAKS and disagrees with the property's own `Initial:` line, so the row has to say
   what it is overriding or it is one fact with two sources and no way to tell which is stale. The DCHECK below
   re-reads the registry every time and fires the day lexbor's answer changes — which is the same shape
   css_color.c uses for the one `<color>` production it reads itself, and for the same reason: a vendored
   parser is a moving target and a silent divergence from it is worse than a crash.
   CSS Color 4 §3.2 gives `color` an `Initial:` line of `CanvasText`; lexbor answers `currentcolor`, which
   cannot be an initial value at all — §6.4 makes currentcolor's used value the used value of `color` on the
   same element, so on the root element, where §7.2's inherited value IS the initial value, it would be a
   definition of itself with no base case. */
static const struct { const char *name; const char *initial; const char *registry; } CSSD_INITIAL_WRONG[] = {
    { "color", "canvastext", "currentcolor" },
};

/* CSS Cascade §7.1's INITIAL VALUE, straight out of Lexbor's registry, which is where the spec's own initial
   values live, and out of the table above for the properties it has no entry for.
   IT IS NOT A LAYER OF THE CASCADE, which is why it is no longer the last thing `cssom_cascaded_value` tries.
   §6 answers which DECLARATION won and §7 is a separate step over that answer — and the difference is
   observable the moment a property is INHERITED: folding the initial value into the cascade makes "nobody
   declared it" indistinguishable from "somebody declared the initial value", and §7.2 has to tell those apart
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
   won — which is not yet the computed value and is not yet §9's resolved value; css_computed_value.c owns both
   of those steps, and this is the one entry it reads the cascade through. */
char *cssom_cascaded_value(lxb_dom_element_t *el, const char *name)
{
    CssLayerOrder *order;
    CssCascade *cascade;
    const char *ua;
    uint32_t seq = 0;
    bool important = false;
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
           "css_resolved_value over §7.2's resolved longhands. A third caller must run it too");
    /* EVERY ORIGIN CONTRIBUTES INTO ONE LIST, AND THE SORT DECIDES — which is §6.1 and is not what asking each
       origin in turn does. The four used to be asked in precedence order and the first that answered won, and
       that is a DIFFERENT ordering: it hoists §6.1's Element-Attached criterion above its Origin-and-Importance
       one, so `<p style="color:blue">` beat `p { color: red !important }`, which §6.1 and §6.3 both say it
       loses to ("an important declaration takes precedence over a normal declaration", and Element-Attached is
       the criterion BELOW Origin and Importance, reached only when it ties). */
    order = css_layer_order_create();
    cascade = css_cascade_create(order);
    cssd_author_collect(el, name, cascade, order, &seq);
    /* §6.1's ELEMENT-ATTACHED STYLES: "declarations that are attached directly to an element (such as the
       contents of a style attribute) rather than indirectly mapped by means of a style rule selector take
       precedence over declarations the same importance that are mapped via style rule." It is in the AUTHOR
       origin ([CSSSTYLEATTR]) and in no explicit cascade layer, and §6.1's Order of Appearance places it after
       every style sheet ("declarations from style attributes ... are all placed after any style sheets"),
       which is what the counter reaching here already is. */
    v = cssd_inline_value(el, name, &important);
    if (v) {
        css_cascade_add(cascade, CSS_ORIGIN_AUTHOR, important, true, css_layer_order_root(order), 0, ++seq, v);
        free(v);
    }
    /* css-cascade-5 §6.5's AUTHOR PRESENTATIONAL HINT ORIGIN, "between the regular user origin and the author
       origin". It is an ORIGIN and not a row of the UA table below because that is where §6.5 puts it: an
       author rule of any specificity outranks a hint, and a hint outranks the UA sheet. */
    v = css_presentational_hint(el, name);
    if (v) {
        css_cascade_add(cascade, CSS_ORIGIN_PRESENTATIONAL_HINT, false, false, NULL, 0, ++seq, v);
        free(v);
    }
    ua = cssd_ua_value(el, name);
    if (ua) css_cascade_add(cascade, CSS_ORIGIN_UA, false, false, NULL, 0, ++seq, ua);
    /* §6.4.3's order is a fact about the WHOLE document's layers, so it is sealed once every sheet has been
       walked and before the first index is read. Nothing below declares a layer. */
    css_layer_order_seal(order);
    /* NULL HERE IS "NO DECLARATION WON", which is a real answer and not a missing one — CSS Cascade §7.1 and
       §7.2 are both written for exactly this state ("unless the cascade results in a value"), and which of them
       applies is the property's own `Inherited:` line. §7's step is core/css/css_defaulting.h's and it runs
       above this; §7.3's three cascade-dependent keywords are discharged BELOW it, inside the sort, because
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
           "§7.2 states them as the resolved value of every longhand supported CSS property, which this engine "
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
                      "declaration's own §3.6.2 step 1 count is what should have refused the call");
    name = JS_ToCString(ctx, argv[0]);
    value = JS_ToCString(ctx, argv[1]);
    /* §3.6.2's ABSENT OPTIONAL ARGUMENT, in both of its spellings: a call that stopped short of the position
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

/* The camel-cased IDL attributes. §6.6.1 states both halves as a forward — the getter "must return the result
   of invoking getPropertyValue()" and the setter "must invoke setProperty() ... and no third argument" — so
   they answer out of the same two paths above and never grow a rule of their own. `magic` is Lexbor's own
   property id, so the dashed name is read back out of the registry rather than stored twice. */
static JSValue js_cssd_camel_get(JSContext *ctx, JSValueConst this_val, int magic)
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

static JSValue js_cssd_camel_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
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

/* ---- THE DESCRIPTOR INTERFACES: CSS Fonts §12.1's CSSFontFaceDescriptors and CSSOM §6.4.7's
 * CSSPageDescriptors -----------------------------------------------------------------------------------------
 *
 * EACH LIST IS TYPED OUT BECAUSE THERE IS NO REGISTRY TO GENERATE IT FROM, and that is the opposite of the
 * camel-cased property attributes above, which are generated precisely because lexbor's property table IS the
 * "supported CSS property" set §6.6.1 states them over. A DESCRIPTOR is not a property: `src` and
 * `unicode-range` are accepted nowhere but inside an `@font-face` rule and `size` nowhere but inside an
 * `@page`, lexbor's registry has no entry for any of them, and each set is closed by the IDL rather than by
 * what this engine implements. So each spec's own names are its list, and each name carries the two spellings
 * the IDL declares (`fontFamily` and `font-family`, `marginTop` and `margin-top`) — a name that is already one
 * word (`src`, `size`, `marks`, `bleed`, `margin`) has one.
 *
 * TWO TABLES, ONE MAGIC SPACE. The attribute bodies below are shared: a descriptor is read through
 * getPropertyValue and written through setProperty whichever interface declared it, so what an install has to
 * carry is only WHICH NAME, and the magic indexes the two tables read end to end. A second pair of bodies over
 * a second table would be the same twenty lines twice, and the two copies would be free to disagree about
 * §6.6.1's own paths. */
static const char *const FONT_FACE_DESCRIPTORS[] = {
    "src", "font-family", "font-style", "font-weight", "font-stretch", "font-width", "unicode-range",
    "font-feature-settings", "font-variation-settings", "font-named-instance", "font-display",
    "font-language-override", "ascent-override", "descent-override", "line-gap-override",
};
#define FONT_FACE_DESCRIPTOR_N ((int)(sizeof(FONT_FACE_DESCRIPTORS) / sizeof(FONT_FACE_DESCRIPTORS[0])))

/* CSSOM §6.4.7's `interface CSSPageDescriptors : CSSStyleDeclaration`, whose fourteen attributes are these
   nine names in their two spellings. It derives from CSSStyleDeclaration and NOT from CSSStyleProperties, and
   that is the interface saying what CSS Paged Media §4.3 says: a page context holds page properties, so
   `pageRule.style.transform` is not a member and `pageRule.style.cssFloat` is undefined — which is what
   css/cssom/page-descriptors.html reads directly. */
static const char *const PAGE_DESCRIPTORS[] = {
    "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
    "size", "page-orientation", "marks", "bleed",
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
    if (magic < FONT_FACE_DESCRIPTOR_N) return FONT_FACE_DESCRIPTORS[magic];
    return PAGE_DESCRIPTORS[magic - FONT_FACE_DESCRIPTOR_N];
}

/* Both halves forward exactly as §6.6.1's do — the getter is getPropertyValue and the setter is setProperty
   with no third argument — so a descriptor is read and written through the very paths a property is, and the
   block's declarations have ONE builder. There is no computed arm: a descriptor block is never a computed
   style (§7.2's creator makes a CSSStyleProperties), which the flag asserts from the other side. */
static JSValue js_cssd_descriptor_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    const char *name = cssd_descriptor(magic);
    JSValue block = cssd_block(ctx, this_val), r;
    size_t len = 0;
    char *text, *v;

    if (JS_IsException(block)) return block;
    DCHECK(!cssd_flag(ctx, block, "computed"),
           "a descriptor attribute was read off a COMPUTED declaration block — §7.2's getComputedStyle is the "
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

/* Web IDL §3.4.4's [PutForwards=cssText], which BOTH `style` attributes carry — §7.1's on an element and
   §6.4.3's on a style rule. The attribute is readonly and yet `el.style = 'color:red'` works, because the
   setter is "let Q be ? Get(esValue, id); if Type(Q) is not Object, throw a TypeError; perform
   ? Set(Q, forwardId, V, true)" — a real property get by NAME through the getter, which is why one setter
   serves both attributes and neither component needs its own. Installing the attribute without it was not a
   missing feature but a WRONG one: the assignment was silently dropped in sloppy mode and threw a TypeError in
   strict mode, where a browser sets the declaration block's text. */
static JSValue js_cssd_put_forwards(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue q = JS_GetPropertyStr(ctx, this_val, "style");
    int ok;

    (void)magic;
    if (JS_IsException(q)) return q;
    if (!JS_IsObject(q)) {
        JS_FreeValue(ctx, q);
        return JS_ThrowTypeError(ctx, "the attribute this assignment forwards to is not an object");
    }
    ok = JS_SetPropertyStr(ctx, q, "cssText", JS_DupValue(ctx, val));
    JS_FreeValue(ctx, q);
    return ok < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* §7.2's DECLARATIONS OF A COMPUTED BLOCK, which are not stored anywhere and are not the owner element's
   inline ones: "a list of CSS declarations ... with the following properties: ... declarations: the resolved
   value of every LONGHAND property that is a supported CSS property, in LEXICOGRAPHICAL ORDER, plus every
   custom property whose computed value is not the guaranteed-invalid value."
 *
 * THE SUPPORTED SET IS THE ONE THIS ENGINE CAN ANSWER FOR, and that is a real answer rather than a smaller
 * one: `item(i)` names a property, `getPropertyValue` of that name must return its resolved value, and a name
 * whose resolved value this build does not derive would crash the very read the enumeration invites. So the
 * set is `css_computed_models`' — core/css/css_computed_value.h's own list, which is where the `Computed
 * value:` lines live — minus anything css_shorthand.c records as a shorthand, since §7.2 says LONGHAND.
 * THE NAMESPACE IS THE UNION OF TWO PLACES and both are asked, because neither alone is the engine's property
 * list: lexbor's registry, and the longhands css_shorthand.c owns the grammar of (the four `border-*-width`
 * and four `border-*-style`, which the registry does not carry at all).
 * NO CUSTOM PROPERTY IS ENUMERATED, and that is a positive statement rather than a gap: a custom property's
 * computed value comes from CSS Cascade §7's defaulting over a registration, and this engine registers none,
 * so every one of them holds the guaranteed-invalid value that §7.2's own clause excludes. */
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

    for (id = 1; id < LXB_CSS_PROPERTY__LAST_ENTRY; id++) {
        const lxb_css_entry_data_t *e = lxb_css_property_by_id(id);

        if (!e || !e->name || e->length == 0) continue;
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
    /* LEXICOGRAPHICAL ORDER, which §7.2 states outright and which is therefore the enumeration's contract
       rather than a tidy-up: `item(i)` and `length` are the same list read two ways, and a page walking the
       indices expects the order the spec named. */
    for (i = 1; i < d->n; i++) {
        CssDecl cur = d->v[i];
        unsigned j2;

        for (j2 = i; j2 > 0 && strcmp(d->v[j2 - 1].name, cur.name) > 0; j2--) d->v[j2] = d->v[j2 - 1];
        d->v[j2] = cur;
    }
}

/* The block's declarations — §7.2's list for a computed block, and what parsing the backing's text produces for
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

static JSValue js_cssd_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue block = cssd_block(ctx, this_val);
    CssDecls d = { 0 };
    unsigned n;

    (void)magic;
    if (JS_IsException(block)) return block;
    cssd_declared_decls(ctx, block, &d);
    n = d.n;
    cssd_decls_free(&d);
    JS_FreeValue(ctx, block);
    return JS_NewInt32(ctx, (int32_t)n);
}

static JSValue js_cssd_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue block = cssd_block(ctx, this_val), r;
    CssDecls d = { 0 };
    int64_t idx = 0;

    (void)magic;
    if (JS_IsException(block)) return block;
    DCHECK(argc >= 1, "§6.6.1's `item` reached its body with no index — its IDL argument is required");
    JS_ToInt64(ctx, &idx, argv[0]);   /* a real number by now: the declaration converted it */
    cssd_declared_decls(ctx, block, &d);
    /* "If there is no indexth object in the collection, then the method must return the empty string." */
    r = (idx >= 0 && idx < (int64_t)d.n) ? JS_NewString(ctx, d.v[idx].name) : JS_NewStringLen(ctx, "", 0);
    cssd_decls_free(&d);
    JS_FreeValue(ctx, block);
    return r;
}

/* §6.6.1: "The parentRule attribute must return the parent CSS rule." It was a `null` DATA property on the
   prototype — the right answer for the two element-backed blocks and a wrong one for §6.4.3's, which is the
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
           "interface the block is is the caller's to state, because §6.6's three creators and CSS Fonts "
           "§12.1's fourth do not all make the same one");
    DCHECK(JS_IsNull(owner_node) != JS_IsNull(parent_rule),
           "§6.6's owner node and parent CSS rule are not two independent fields for this engine: one of them "
           "is where the declarations LIVE, so a block with both or with neither is a block whose declarations "
           "are kept in two places or in none");
    DCHECK(!computed || readonly,
           "a COMPUTED declaration block was minted WRITABLE. §7.2 is the only creator that sets the computed "
           "flag and it sets the readonly flag in the same breath — a writable one would take §6.6.1's set-a-"
           "CSS-declaration path into a block whose declarations are computed per read and stored nowhere");
    obj = JS_NewObjectProto(ctx, proto);
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

/* §6.4.3: "The style attribute must return a CSSStyleProperties object for the style rule" — computed flag
   unset, readonly flag unset, declarations the rule's own, parent CSS rule THIS, owner node null. */
JSValue cssom_style_properties_for_rule(JSContext *ctx, JSValueConst rule)
{
    JSValue proto = cssd_proto(ctx), out;

    DCHECK(css_rule_is(rule),
           "§6.4.3's `style` was asked to back a declaration block with something that is not a CSS rule");
    out = cssd_new(ctx, proto, JS_NULL, rule, false, false);
    JS_FreeValue(ctx, proto);
    return out;
}

/* CSS Fonts §12.1's `style` — see the header. The block's PROPERTIES are §6.4.3's exactly (computed flag unset,
   readonly flag unset, declarations the rule's own, parent CSS rule the rule, owner node null); only the
   prototype differs, which is what makes the descriptors reachable and the properties not. */
JSValue cssom_font_face_descriptors_for_rule(JSContext *ctx, JSValueConst rule)
{
    JSValue proto = realm_value_get(ctx, g_font_face_proto_slot), out;

    DCHECK(css_rule_is(rule),
           "CSS Fonts §12.1's `style` was asked to back a descriptor block with something that is not a CSS "
           "rule");
    out = cssd_new(ctx, proto, JS_NULL, rule, false, false);
    JS_FreeValue(ctx, proto);
    return out;
}

/* CSSOM §6.4.7's `style` — see the header. The block's PROPERTIES are §6.4.3's exactly (computed flag unset,
   readonly flag unset, declarations "the declared descriptors in the rule, in specified order", parent CSS
   rule the rule, owner node null); only the prototype differs. */
JSValue cssom_page_descriptors_for_rule(JSContext *ctx, JSValueConst rule)
{
    JSValue proto = realm_value_get(ctx, g_page_proto_slot), out;

    DCHECK(css_rule_is(rule),
           "§6.4.7's `style` was asked to back a descriptor block with something that is not a CSS rule");
    out = cssd_new(ctx, proto, JS_NULL, rule, false, false);
    JS_FreeValue(ctx, proto);
    return out;
}

/* §7.1's ElementCSSInlineStyle: "The style attribute must return a CSSStyleProperties object whose readonly
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
    /* Web IDL §3.7.5's brand check, and a THROW rather than an assert: the member is on a prototype and a page
       reaches an accessor off one with `.call` on anything at all. Without it the block below would be minted
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

/* §7.2's getComputedStyle(elt, pseudoElt) — "return a live CSSStyleProperties object" with the computed flag
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
           over the same record costs a value slot and not a class. `[object …]` is §3.7.3's @@toStringTag on
           the prototype, so CSSStyleProperties and CSS Fonts §12.1's CSSFontFaceDescriptors are told apart by
           a page even though their records are identical. */
        JSClassDef d = { "CSSStyleProperties" };
        JS_NewClassID(JS_GetRuntime(ctx), &g_cssd_class);
        JS_NewClass(JS_GetRuntime(ctx), g_cssd_class, &d);
    }
    g_declaration_proto_slot = realm_value_declare(ctx, "CSSOM §6.6.1 CSSStyleDeclaration.prototype");
    g_font_face_proto_slot = realm_value_declare(ctx, "CSS Fonts §12.1 CSSFontFaceDescriptors.prototype");
    g_page_proto_slot = realm_value_declare(ctx, "CSSOM §6.4.7 CSSPageDescriptors.prototype");
    g_ready = 1;
    {
        static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
        static const IdlArgType ONE_LONG[1] = { IDL_LONG };
        static const IdlArgType THREE_STR[3] = { IDL_DOMSTRING, IDL_DOMSTRING, IDL_DOMSTRING };
        g_set_css_text_id = idl_setter_id(ctx, IDL_DOMSTRING, false, js_cssd_set_css_text, 0);
        g_put_forwards_id = idl_setter_id(ctx, IDL_ANY, false, js_cssd_put_forwards, 0);
        g_get_prop_id = idl_method_id(ctx, ONE_STR, 1, js_cssd_prop_op, 0);
        g_remove_prop_id = idl_method_id(ctx, ONE_STR, 1, js_cssd_prop_op, 1);
        g_get_priority_id = idl_method_id(ctx, ONE_STR, 1, js_cssd_prop_op, 2);
        g_set_prop_id = idl_method_id(ctx, THREE_STR, 3, js_cssd_set_property, 0);
        /* §6.6.1: `setProperty(CSSOMString property, CSSOMString value, optional CSSOMString priority = "")` */
        idl_optional_from(2);
        g_item_id = idl_method_id(ctx, ONE_LONG, 1, js_cssd_item, 0);
        {
            /* CSSOM §7.1: `getComputedStyle(elt, optional pseudoElt)`. DECLARED HERE with the rest — the
               member lives on the WINDOW, which is per realm, and the declaration is the agent's. */
            static const IdlArgType TWO[2] = { IDL_ANY, IDL_DOMSTRING };
            g_id_gcs = idl_method_id(ctx, TWO, 2, js_get_computed_style, 0);
            idl_optional_from(1);
        }
    }
    for (id = 1; id < LXB_CSS_PROPERTY__LAST_ENTRY; id++)
        g_camel_set_id[id] = idl_setter_id(ctx, IDL_DOMSTRING, false, js_cssd_camel_set, (int)id);
    {
        /* CSS Fonts §12.1 and CSSOM §6.4.7 both declare every descriptor attribute
           `[LegacyNullToEmptyString]`, so `null` reaches the setter as "" and REMOVES the descriptor rather
           than declaring the string "null". */
        int i;

        for (i = 0; i < DESCRIPTOR_N; i++)
            g_desc_set_id[i] = idl_setter_id(ctx, IDL_DOMSTRING, true, js_cssd_descriptor_set, i);
    }
    realm_declare_intrinsic(cssom_install_proto);
}

/* ONE DESCRIPTOR INTERFACE'S ATTRIBUTES, over the magic range `[first, first + n)` that is its table. BOTH
   SPELLINGS ARE INSTALLED because the IDL declares both — `fontFamily` and `font-family` are two attributes of
   the interface, not one attribute and a convenience, and so are `marginTop` and `margin-top` — and the dashed
   one is exactly what css/cssom/cssstyledeclaration-cssfontrule.tentative.html reads (`"unicode-range" in
   style`) and what css/cssom/page-descriptors.html asserts as an own property of the prototype. A name that is
   already one word has two spellings that COINCIDE, so one attribute is defined for it. */
static void cssd_install_descriptors(JSContext *ctx, JSValueConst proto, int first, int n)
{
    int d;

    for (d = first; d < first + n; d++) {
        const char *name = cssd_descriptor(d);
        char camel[48];
        size_t i, j = 0;
        bool up = false;

        idl_install_accessor(ctx, proto, name, js_cssd_descriptor_get, d, g_desc_set_id[d]);
        for (i = 0; name[i]; i++) {
            if (name[i] == '-') { up = true; continue; }
            DCHECK(j + 1 < sizeof(camel), "a descriptor's camel-cased spelling outgrew its buffer");
            camel[j++] = up ? (char)toupper((unsigned char)name[i]) : name[i];
            up = false;
        }
        camel[j] = 0;
        if (strcmp(camel, name) != 0)
            idl_install_accessor(ctx, proto, camel, js_cssd_descriptor_get, d, g_desc_set_id[d]);
    }
}

/* FOUR INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM, because four specs split them. §6.6.1's
   `interface CSSStyleProperties : CSSStyleDeclaration` carries `cssFloat` and the per-property camel-cased
   attributes; CSS Fonts §12.1's `interface CSSFontFaceDescriptors : CSSStyleDeclaration` carries the fifteen
   `@font-face` DESCRIPTORS and CSSOM §6.4.7's `interface CSSPageDescriptors : CSSStyleDeclaration` the
   fourteen `@page` ones, which are different sets with a different source (see the tables above); and
   CSSStyleDeclaration carries the block's own eight members, which all three inherit. Installing all of them on
   one object made `CSSStyleProperties` an absent global — an honest ReferenceError for an interface every one
   of this engine's blocks IS — and made `Object.getOwnPropertyNames(CSSStyleDeclaration.prototype)` report
   three hundred property attributes a browser does not have there. Nothing is an instance of the base, so it
   holds no class of its own, exactly as §6.1.1's StyleSheet and §6.4.2's CSSRule do. */
void cssom_install_proto(JSContext *ctx)
{
    JSValue base, proto, descriptors, page, prev;
    uintptr_t id;

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
    idl_install_method(ctx, base, "getPropertyValue", 1, g_get_prop_id);
    idl_install_method(ctx, base, "removeProperty", 1, g_remove_prop_id);
    idl_install_method(ctx, base, "getPropertyPriority", 1, g_get_priority_id);
    idl_install_method(ctx, base, "setProperty", 2, g_set_prop_id);
    idl_install_method(ctx, base, "item", 1, g_item_id);

    proto = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(proto), "CSSStyleProperties.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "CSSStyleProperties");
    /* THE CAMEL-CASED IDL ATTRIBUTES, generated from LEXBOR'S OWN CSS PROPERTY REGISTRY. §6.6.1 states them as
       a partial interface "for each CSS property property that is a supported CSS property", so the registry IS
       the list — typing a hundred names here would be a second copy of it that could disagree, and inventing
       them would be worse. */
    for (id = 1; id < LXB_CSS_PROPERTY__LAST_ENTRY; id++) {
        const lxb_css_entry_data_t *e = lxb_css_property_by_id(id);
        char camel[64];
        size_t i, j = 0;
        bool up = false;

        if (!e || !e->name || e->length == 0 || e->length + 1 >= sizeof(camel)) continue;
        if (e->name[0] == '-') continue;   /* a vendor-prefixed name has its own IDL spelling; not invented here */
        for (i = 0; i < e->length; i++) {
            if (e->name[i] == '-') { up = true; continue; }
            camel[j++] = up ? (char)toupper(e->name[i]) : (char)e->name[i];
            up = false;
        }
        camel[j] = 0;
        /* §CSSOM's one exception: `float` is a reserved word in some bindings, so its attribute is `cssFloat`. */
        if (strcmp(camel, "float") == 0) strcpy(camel, "cssFloat");
        idl_install_accessor(ctx, proto, camel, js_cssd_camel_get, (int)id, g_camel_set_id[id]);
    }

    /* CSS Fonts §12.1's CSSFontFaceDescriptors.prototype and CSSOM §6.4.7's CSSPageDescriptors.prototype, each
       over the magic range that is its own table. */
    descriptors = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(descriptors), "CSSFontFaceDescriptors.prototype could not be allocated");
    idl_interface_tag(ctx, descriptors, "CSSFontFaceDescriptors");
    cssd_install_descriptors(ctx, descriptors, 0, FONT_FACE_DESCRIPTOR_N);

    page = JS_NewObjectProto(ctx, base);
    CHECK(!JS_IsException(page), "CSSPageDescriptors.prototype could not be allocated");
    idl_interface_tag(ctx, page, "CSSPageDescriptors");
    cssd_install_descriptors(ctx, page, FONT_FACE_DESCRIPTOR_N, PAGE_DESCRIPTOR_N);

    JS_SetClassProto(ctx, g_cssd_class, proto);
    realm_value_set(ctx, g_declaration_proto_slot, base);
    realm_value_set(ctx, g_font_face_proto_slot, descriptors);
    realm_value_set(ctx, g_page_proto_slot, page);
}

int cssom_put_forwards_setter(void)
{
    DCHECK(g_put_forwards_id >= 0,
           "§3.4.4's [PutForwards] setter was asked for before cssom_init declared it — the two `style` "
           "attributes that carry it are installed onto prototypes this component's init runs ahead of");
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
    idl_install_method(ctx, global, "getComputedStyle", 1, g_id_gcs);
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
