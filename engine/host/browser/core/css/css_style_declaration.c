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
#include "core/agent_state.h"
#include "core/idl_slots.h"
#include "core/idl_args.h"
#include "core/idl_index_arg.h"   /* §6.6.1's `item` index, known and unknown — the ARGUMENT half */
#include "core/idl_indexed.h"   /* §6.6.1's `getter CSSOMString item(unsigned long index)` — the getter half */
#include "quickjs-step.h"
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/document.h"
#include "core/dom/element.h"
#include "core/dom/selector_match.h"
#include "core/html/integer_microsyntax.h"   /* §4.3.11.1's `headingoffset` runs §2.3.4.2's rules for parsing non-negative integers */
#include "core/css/css_cascade.h"
#include "core/css/css_cascade_pass.h"   /* css-cascade-5 §4.2's answer, held for one render */
#include "core/css/css_computed_value.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_font_family.h"
#include "core/css/css_font_src.h"
#include "core/css/css_keyframes.h"
#include "core/css/css_logical.h"
#include "core/css/css_pending_substitution.h"
#include "core/css/css_math.h"
#include "core/css/css_page.h"
#include "core/css/css_property_numeric.h"
#include "core/css/css_presentational_hints.h"
#include "core/css/css_rule.h"
#include "core/css/css_background_shorthand.h"
#include "core/css/css_shorthand.h"
#include "core/css/css_var.h"   /* css-variables-1 §3's var(), for the parse-time rule below */
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
static JSClassID g_declaration_proto_slot = JS_INVALID_CLASS_ID;
/* CSS Fonts 5 §9.1 The CSSFontFaceRule interface's CSSFontFaceDescriptors.prototype, the same way and for the
   same reason. THE LEVEL IS PART OF THIS CITATION: CSS Fonts 4 numbers the same-titled section §12.1 and declares
   the interface with SIX names fewer, so a bare "CSS Fonts §12.1" sends a reader to an edition that does not
   carry what this file installs — the title is identical across both, which is exactly why the number alone
   cannot be checked. It is a THIRD
   prototype over the SAME class and the same record: an `@font-face` block's declarations are kept where
   CSSOM §6.4.3's are (the rule's own text, through core/css/css_rule.h), so what differs is only which member names
   the interface answers to. */
static JSClassID g_font_face_proto_slot = JS_INVALID_CLASS_ID;
/* CSSOM §6.4.7 The CSSPageRule Interface's CSSPageDescriptors.prototype, the same way and for the same
   reason — a FOURTH prototype over
   the one class and the one record. A `@page` rule's descriptors are kept where CSSOM §6.4.3's declarations are (the
   rule's own text, through core/css/css_rule.h), so what differs is only which member names the interface
   answers to and, through core/css/css_page.h, which declarations the block admits at all. */
static JSClassID g_page_proto_slot = JS_INVALID_CLASS_ID;
/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. §6.6.1's per-property
   attributes are GENERATED from Lexbor's property registry, so their setter ids are an array indexed the same
   way the registry is — one entry per property, declared once, installed into every realm, and SHARED by that
   property's camel-cased, webkit-cased and dashed spellings because §6.6.1 gives the three one set of setter
   steps. A row CSSOM §2 Terminology excludes holds -1, which is `no setter`: an installer reaching one would make an
   attribute silently read-only, so it is DCHECKed at the install rather than left to be discovered. */
static int g_set_css_text_id = -1, g_get_prop_id = -1, g_remove_prop_id = -1, g_get_priority_id = -1,
           g_set_prop_id = -1, g_item_id = -1, g_put_forwards_id = -1;
/* The bound on this engine's OWN supported properties — the half of the id space below that the vendored
   registry does not provide. Declared here because the setter-id array spans the whole space; the list itself
   and why it exists are at `cssd_own_init`. */
#define CSSD_OWN_MAX 128
static int g_property_set_id[LXB_CSS_PROPERTY__LAST_ENTRY + CSSD_OWN_MAX];
/* CSSOM §6.6.1's id space resolved to ONE name — defined beside the list it reads, forward-declared here
   because the two per-property accessors above it are what `idl_setter_id` is handed. */
static const char *cssd_property_name_of(uintptr_t id);
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

/* A declaration's value AS THE PAGE SPELLED IT — the slice of the parsed text between the offsets lexbor
   recorded for it, rather than the value its registry TYPED. It is what a property whose grammar this engine
   owns has to be asked about, because lexbor's typed value is downstream of a parse that may have refused the
   declaration outright (`__UNDEF`, where the value is a raw span) or accepted it under a NARROWER grammar and
   re-spelled it (where the serialization above is its own answer and not the page's). Both are wrong inputs to
   a second parse: the first is right only by accident of the arena copy, and the second re-parses text one
   grammar already normalized, which loses exactly what the two grammars disagree about.
   THE OFFSETS INDEX THE TOKENIZER'S INPUT BUFFER — the very string handed to `lxb_css_declaration_list_parse`
   or `lxb_css_stylesheet_parse` — which is the same invariant `cssd_at_prelude` asserts one function along and
   for the same reason. THAT IS A DCHECK AND NOT A REFUSAL BECAUSE IT IS ABOUT THIS FILE'S OWN PLUMBING AND NOT
   ABOUT THE PAGE'S BYTES: no declaration a page can write moves an offset outside the text it was parsed from,
   so a violation is a caller that passed a buffer other than the one it parsed.
   `!important` IS NOT IN THE SLICE. lexbor closes `value_end` at the whitespace before the `!` and records the
   important run in offsets of its own, so the span is the value and nothing else.
   OWNED, NULL for a span the offsets do not describe. */
static char *cssd_decl_source_value(const lxb_css_rule_declaration_t *d, const char *text, size_t len)
{
    size_t begin, end;
    char *out;

    DCHECK(d != NULL && text != NULL, "a declaration's source value was asked for with no declaration or no "
                                      "text — the text IS where the offsets point, so there is nothing to "
                                      "slice without it");
    begin = d->offset.value_begin;
    end = d->offset.value_end;
    DCHECK(begin <= end && end <= len,
           "a declaration's value offsets fall outside the text that was parsed — they index the tokenizer's "
           "input buffer, which is the very string handed to the parse that produced this rule, so a caller "
           "reading a rule against a DIFFERENT buffer is the only way this can be false");
    if (begin > end || end > len) return NULL;
    out = malloc(end - begin + 1);
    CHECK(out != NULL, "cssom: OOM copying a declaration's source value");
    memcpy(out, text + begin, end - begin);
    out[end - begin] = '\0';
    return out;
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
       Properties and Values API 1 owns its grammar — `--x: calc(1s)` is a valid declaration whatever it says.
       AND IT IS SCOPED TO A *VALID* MATH FUNCTION, which is a narrower gate than "is a math function at all"
       and is the one this assertion may stand on. Its subject is a PRODUCER that asked the wrong production,
       so the value has to be one css-values-4 §10.9 "Type Checking" RESOLVES: `calc(1px + 1s)` is §4.3.2's
       failure and `calc(50% * 50%)` types to «["percent" -> 2]», and neither names a production any
       property could have asked for — they are the PAGE's mistake, and CLAUDE.md forbids an assert standing
       on bytes a page wrote. The two are told apart by core/css/css_math.h's middle rung, not by this file. */
#if APICLIENT_DEV
    /* …AND IT IS SCOPED AWAY FROM A VALUE NO PRODUCER HAS JUDGED YET, which is what an unsubstituted
       arbitrary substitution function is. css-values-5 makes such a value "assumed to be valid at parse time"
       and "only syntax-checked at computed-value time, after … functions have been substituted", so there is
       no producer here to have asked the wrong production of and the invariant this assert states is not yet
       about anything. `calc(var(--gap) + 1px)` is the shape: §10.9 cannot type an unsubstituted reference, so
       `css_math_is_valid_function` already answers false for it and this conjunct changes no verdict today —
       it is written because that is a fact about ANOTHER component's typing rather than about this rule, and
       a value admitted by the arm above must not depend on it to stay out of an abort. */
    if (value != NULL && !(name[0] == '-' && name[1] == '-') && !css_var_references(value) &&
        css_property_numeric_audited(name) && css_math_is_valid_function(value, strlen(value))) {
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
    /* css-values-5: "If a property value contains one or more arbitrary substitution functions, and all of
       those functions are themselves syntactically valid according to their argument grammars, THE ENTIRE
       VALUE'S GRAMMAR MUST BE ASSUMED TO BE VALID AT PARSE TIME." That is the same question the two arms
       around it ask — is this a value a LATER LEVEL OF CSS defines than the grammar that refused it — and
       `var()` is its widest member yet: the vendored grammar predates css-variables-1 entirely, so EVERY
       declaration whose value references a custom property arrived here and was dropped, on every property
       and at every site. MEASURED before this arm, reading a rule's own style block back through CSSOM:
       `background-color: var(--x)` and `margin-top: var(--pad)` were both absent from it while their literal
       twins were present, and an inline `style="color:var(--x)"` was absent too — so the substitution step
       core/css/css_computed_value.c performs was correct, was reached, and was handed nothing, because the
       cascade had no declaration to substitute into.
       NO NUMERIC SHAPE IS ASKED, UNLIKE THE MATH ARM BELOW, and that asymmetry is the standard's: a math
       function has a TYPE, so it is a value of a property only where the grammar names a numeric production,
       while an arbitrary substitution function may resolve to anything at all and the sentence above is
       therefore unconditional on the property. Asking a shape here would re-impose at parse time exactly the
       judgement the standard defers to computed-value time.
       THE SCAN IS core/css/css_var.h's OWN, which is what keeps this arm and the substitution that follows it
       from disagreeing about what a function token is: a `var(` inside a string is not one, and neither is
       the `(` of an ident that merely ends in `var`. Two spellings of that question would let a declaration
       be admitted here and then substituted into by a step that cannot see the function.
       A SHORTHAND IS ADMITTED TOO, AND WHAT SPLITS IT IS NOT THIS TEST. A residual stood here saying a
       shorthand must be refused because this engine had no pending-substitution value to fill its longhands
       with; core/css/css_pending_substitution.h is that value, so the refusal is retired and the standard's
       own split moves one function along to `cssd_decls_collect_property`, which fills each longhand with a
       pending-substitution value instead of asking core/css/css_shorthand.h for a component it cannot yet
       compute. The question THIS function answers is only whether a refused declaration is a declaration at
       all, and css-values-5's sentence above makes a shorthand's value as valid at parse time as a
       longhand's: both are "assumed to be valid" until substitution has run. Refusing one here would answer
       a different question — which longhand gets which component — in the one place that has no way to.
       (The retired residual's next-diff clause named exactly what this diff built, which is the mechanism
       working. Its QUOTATION carried straight apostrophes where css-values-5 writes curly ones; the clause
       was right and the transcription was a typo, and core/css/css_pending_substitution.h now holds the
       sentence checked against the fetched draft.) */
    if (css_var_references(value)) return true;
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
    case CSSOM_BLOCK_FONT_FACE:
        /* NAMED RESIDUAL — THE MEMBERSHIP HALF OF `@font-face` IS NOT BUILT AND THIS ARM ADMITS WHAT
           UNRESTRICTED ADMITS.
           WHAT IS NOT COVERED: css-fonts-4 §4.1 "The @font-face rule" says "Like properties in a declaration
           block, declarations of any descriptors that are not supported by the user agent must be ignored",
           and `color` is not a descriptor of that rule at all — so `@font-face { color: red }` declares
           nothing in a browser and is kept here.
           WHAT THE NEXT DIFF BUILDS: this arm asked of the DESCRIPTOR SET this file already states once, the
           `FONT_FACE_DESCRIPTORS[]` table CSS Fonts 5 §9.1's CSSFontFaceDescriptors is built from, so the
           membership question has one answer rather than a second list beside it.
           HOW ITS ABSENCE WOULD SHOW: an `@font-face` rule's `style.length` counts a declaration whose name
           is a PROPERTY and not a descriptor, and that declaration serializes back out of the rule's
           `cssText`.
           IT IS SEPARATE FROM THE GRAMMAR HALF ON PURPOSE: the grammar question below is about which value
           definition a name that IS a descriptor takes, and answering both from one arm is the predicate
           CLAUDE.md warns is decided by the stricter of its two questions. */
        return true;
    default:
        DCHECK(context == CSSOM_BLOCK_UNRESTRICTED,
               "a declaration block was collected in a context css_style_declaration.h does not declare — the "
               "enum IS the list of specifications that restrict a block, so a fourth value is a restriction "
               "with no rule behind it");
        return true;
    }
}

/* IS THIS BLOCK A DESCRIPTOR BODY — the OTHER question the context answers, kept apart from the membership
   one above for the reason css_style_declaration.h gives. A name that reaches the walk inside an at-rule body
   that declares DESCRIPTORS takes the descriptor's value definition, which for `font-family` is css-fonts-4
   §4.2's `<font-family-name>` rather than §2.1's `[ <font-family-name> | <generic-font-family> ]#`. */
static bool cssd_block_is_descriptor_body(CssomBlockContext context)
{
    return context == CSSOM_BLOCK_FONT_FACE;
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
    /* css-values-5 "Appendix A: Arbitrary Substitution Functions" / "Substitution in Shorthand Properties" —
       AN APPENDIX, so the titles are the citation and no § is written beside them. "If a shorthand property
       contains an arbitrary substitution function in its value, the longhand properties it’s associated with
       must instead be filled in with a special, unobservable-to-authors pending-substitution value that
       indicates the shorthand contains an arbitrary substitution function, and thus the longhand’s value
       can’t be determined until after substituted." (The spec's own curly marks are kept.)
       SO THE EXPANSION BELOW IS NOT MERELY SKIPPED, IT IS UNANSWERABLE HERE, which is the Note's own reason:
       "When the shorthand contains a var(), however, this can’t be done, as the var() could be substituted
       with anything." `margin: var(--g)` may resolve to one component or to four, and CSS 2.1 §8.3's
       rotation cannot say which longhand each lands in until it has bytes to rotate. The pending value
       carries the name and the ORIGINAL value to core/css/css_computed_value.c, which substitutes and then
       runs exactly the expansion below.
       IT IS THE SAME SCAN core/css/css_var.h PERFORMS — the one `cssd_undef_is_declaration` admitted this
       declaration on — so the two cannot come to disagree about what a function token is and leave a
       shorthand admitted at the gate and expanded here with a `var(` still in it. */
    if (value != NULL && css_var_references(value)) {
        for (i = 0; i < n; i++) cssd_decls_collect(d, lh[i], css_pending_make(name, value), important);
        return;
    }
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

/* IS THIS LEXBOR RULE A DECLARATION THIS ENGINE HOLDS, AND WHAT ARE ITS NAME AND VALUE — asked ONCE, by every
 * path that turns lexbor's output into a declaration of a block.
 *
 * IT IS A COMPONENT AND NOT AN `if` AT ONE CALLER, because the question has TWO askers and they answered it
 * DIFFERENTLY. The READ path (cssd_decls_from_list, and therefore every style="" attribute, every style rule
 * and every `cssText` assignment) re-judged a `__UNDEF` through cssd_undef_is_declaration and kept it; the
 * WRITE path (cssom_parse_a_css_value, and therefore `setProperty`, every §6.6.1 per-property IDL attribute and
 * `cssFloat`) tested `d->type != LXB_CSS_PROPERTY__UNDEF` and dropped it. Lexbor's value grammar has no math
 * functions AT ALL — no `calc` token exists in its registry — so every math function is a `__UNDEF`, and the
 * consequence was that `el.style.width = "calc(100% - 20px)"` did NOTHING while `el.setAttribute("style",
 * "width: calc(100% - 20px)")` worked. Not a dropped declaration a page could see thrown: §6.6.1's setProperty
 * step after the parse says "If component value list is null, then return", so the assignment left the block
 * ALONE and the next read answered the property's initial value. Two answers to one question is the defect;
 * one function both callers go through is the fix, and a third asker cannot now disagree because there is
 * nothing left for it to re-derive.
 *
 * WHAT IT ANSWERS IS CSSOM §6.7.1 "Parsing CSS Values"' step 3 — "If the above step failed, return null" —
 * over lexbor's output rather than over the text: false is that null, and true carries the `list` its step 4
 * returns ("Return list"), serialized.
 *
 * CSS Syntax's INVALID DECLARATION is not in the block, so it is not in the block's declarations either.
 * Lexbor keeps one in the list as a `__UNDEF` holding the property id and the RAW UNPARSED TOKENS so that a
 * serializer can round-trip the block it came from — and a cascade that read it back without this test handed
 * those tokens on as if they were a value: `display: bogus` won the cascade and
 * `getComputedStyle(el).display` answered "bogus", a string no property's grammar admits.
 * EXCEPT WHEN THOSE TOKENS ARE A CSS-WIDE KEYWORD, WHICH IS A VALID DECLARATION OF EVERY PROPERTY.
 * css-cascade-5 §7.3 "Explicit Defaulting": "As specified in CSS Values and Units, ALL CSS PROPERTIES CAN
 * ACCEPT THESE VALUES." Lexbor's value grammar carries `initial`, `inherit`, `unset` and `revert` and predates
 * css-cascade-5 §7.3.5 "Rolling Back Cascade Layers: the revert-layer keyword" and §7.3.6 "Rolling Back Rules:
 * the revert-rule keyword", so `height: revert-layer` fails that grammar and arrives here as an invalid
 * declaration while `translate: revert-layer` — a property the grammar does not type at all — arrives as a
 * value. Dropping the first would make a CSS-wide keyword mean something different depending on which
 * properties the vendored parser happens to know, which is a wrong answer per property rather than a missing
 * capability. `undef->type` carries the real property id and `undef->value` the raw source span (the
 * `!important` is a separate offset and is already on the declaration), so both halves survive.
 * AND THE SAME IS TRUE OF A MATH FUNCTION, FOR THE SAME REASON ONE LEVEL WIDER. The CSS-wide keyword arm was
 * written because a value's validity was being decided by which properties the vendored parser happens to
 * type; a `calc()` is that defect at the scale of the whole modern web, because the parser types `width`,
 * `height` and `font-size` and has no math functions at all. Both arms are one question — is this a value a
 * LATER LEVEL OF CSS defines than the grammar that refused it — and cssd_undef_is_declaration is where it is
 * asked.
 *
 * `text` AND `len` ARE THE BUFFER THIS RULE WAS PARSED FROM, and they are REQUIRED rather than optional
 * because a property whose grammar this engine owns is asked about the page's own spelling — which lives at
 * offsets into that buffer and nowhere else. A caller that lost it would not answer such a declaration
 * WRONGLY, it would drop it, and a dropped declaration reads back as one the page never wrote; so the absence
 * is asserted here rather than defaulted past.
 *
 * ON TRUE, `*pname` and `*pvalue` are OWNED by the caller. `*pvalue` may be NULL: CSS Syntax admits a
 * declaration whose value is empty, which is why §6.6's step 4 is conditional — and a `__UNDEF` can never be
 * that, because the re-judge is asked ABOUT the raw span and there is none to ask about.
 * ON FALSE NEITHER OUT-PARAMETER IS WRITTEN, so a caller cannot free what it never received. */
static bool cssd_decl_take(const lxb_css_rule_declaration_t *d, const char *text, size_t len,
                           CssomBlockContext context, char **pname, char **pvalue)
{
    char *name, *value;

    DCHECK(d != NULL && pname != NULL && pvalue != NULL,
           "CSSOM §6.7.1's parse a CSS value was asked about no declaration, or with nowhere to report the "
           "name and value it produces — this entry answers a rule lexbor already parsed, so an absent one is "
           "a caller that lost it rather than a declaration that never had it");
    name = cssd_decl_name(d);
    if (!name) return false;                       /* lexbor has no id for the property either */
    /* css-fonts-4 §2.1 "Font family: the font-family property", AHEAD OF LEXBOR ON THE READ PATH TOO.
       `cssom_parse_a_css_value` takes this grammar back from the registry for every WRITE, and this is the
       same branch for every READ — a `style=""` attribute, a style rule's block, an `@font-face` descriptor
       body and every `cssText` — because they are not two questions. THE BLOCK'S DECLARATIONS ARE TEXT, which
       is what makes them one: a write serializes its answer back into that text and the very next read
       re-parses it, so one grammar for the write and another for the read is not a disagreement a page has to
       construct — it is one a page reaches by writing a value and reading it back.
       `"New Century Schoolbook", serif` IS THE WHOLE CASE IN ONE VALUE. The write answers the unquoted
       `<custom-ident>+` join of css-fonts-4 §2.1.1 "Syntax of <font-family-name>", and lexbor's `font-family`
       state accepts exactly ONE token per list item — after the first it asks for a comma or the end — so it
       REFUSES the three idents it is handed back and the declaration is dropped as invalid. The value a page
       stored through `style.fontFamily` therefore read back as the EMPTY STRING.
       THE SOURCE SLICE AND NOT `cssd_decl_value` IS WHAT IS ASKED, for the reason that function's own note
       gives: for a refused declaration lexbor hands back the raw span (which would be right here by accident),
       and for an accepted one it hands back ITS OWN SERIALIZATION under a narrower grammar, re-spelled by a
       BYTE test over a Latin-1 map where the one css-fonts-4 §2.1.1 names is a code-point test over
       CSS Syntax §4.2 "Definitions"' ident set. Re-parsing that is a second parse of a value one grammar has
       already normalized, and it loses precisely what the two grammars disagree about.
       THE NAME IS COMPARED CASE-SENSITIVELY AND IT IS CANONICAL HERE, which is a different fact from the one
       `cssom_parse_a_css_value` states: this name was SERIALIZED OUT OF LEXBOR'S REGISTRY rather than supplied
       by a page, so `FONT-FAMILY` in the source has already been folded by lexbor's own case-insensitive
       property lookup and arrives spelled one way.
       A VALUE OUTSIDE css-fonts-4 §2.1's GRAMMAR IS CSS Syntax's INVALID DECLARATION and is dropped whole,
       which is the same answer this function gives a `__UNDEF` it cannot re-judge.

       THE DESCRIPTOR GRAMMAR IS A SECOND VALUE DEFINITION OVER THE SAME NAME, AND THIS SEAM SERVES BOTH.
       css-fonts-4 §4.2 "Font family: the font-family descriptor" gives an `@font-face` descriptor the value
       `<font-family-name>` — ONE name, with no `#` and no `<generic-font-family>` alternative — where the
       PROPERTY of css-fonts-4 §2.1 is `[ <font-family-name> | <generic-font-family> ]#`. A descriptor body
       reaches this arm under the same property name as a style rule's declaration, so WHICH BODY THIS IS has
       to be an argument: it is the one thing the text cannot say.
       THE RESIDUAL THIS REPLACES NAMED THE TWO PRODUCTIONS `<family-name>` AND `<generic-family>`, WHICH ARE
       NOT css-fonts-4's SPELLINGS, and the correction is recorded rather than quietly applied because the
       clause's SUBSTANCE was exactly right and only its transcription was stale — the routine half of
       CLAUDE.md's mis-transcription rule. Its `WHAT THE NEXT DIFF BUILDS` clause named a DESCRIPTOR member of
       `CssomBlockContext` threaded to this arm, which is what landed; its `HOW ITS ABSENCE WOULD SHOW` clause
       — an `@font-face` whose `font-family` names several families or a generic serializing back out of that
       rule's `cssText` — is the observation that retires it.
       WHAT DOES NOT MOVE IS THE PROPERTY'S OWN ANSWER. Lexbor's registry carries no descriptor grammar at all,
       so an `@font-face` body's `font-family` reached the SAME property state of css-fonts-4 §2.1 before this
       and a style rule's still does. */
    if (strcmp(name, "font-family") == 0) {
        char *raw = cssd_decl_source_value(d, text, len);

        value = raw ? (cssd_block_is_descriptor_body(context) ? css_font_family_descriptor_value(raw)
                                                              : css_font_family_value(raw))
                    : NULL;
        free(raw);
        if (!value) { free(name); return false; }
        *pname = name;
        *pvalue = value;
        return true;
    }
    /* css-fonts-4 §4.3 "Font reference: the src descriptor", WHICH IS A DESCRIPTOR AND NOT A PROPERTY — so
       unlike the `font-family` arm above, the CONTEXT decides whether this grammar is asked AT ALL rather
       than which of two grammars it is. There is no `src` property in any specification, so outside an
       `@font-face` body the name keeps the answer it has always had.
       IT IS ASKED AHEAD OF LEXBOR BECAUSE LEXBOR HAS NO OPINION TO TAKE BACK. Its registry does not carry
       `src`, so `lxb_css_declaration_create` falls to `LXB_CSS_PROPERTY__CUSTOM` — the same arm a `--x`
       takes — and the declaration reached the block with its RAW TEXT stored verbatim and nothing anywhere
       having judged it. Verbatim is a defensible answer for a descriptor nothing reads; it stops being one
       the moment a consumer has to resolve urls out of it, which is what CSS Font Loading §2.2 "The load()
       method" does with the `[[Urls]]` slot core/fonts/font_face.c fills, and it is why an `@font-face`
       carrying `src: url(should be quoted.ttf)` read back as a declaration where a browser drops it.
       THE SOURCE SLICE AND NOT `cssd_decl_value` IS WHAT IS ASKED, for the reason the `font-family` arm above
       gives at length: the page's own spelling lives at offsets into the text and nowhere else, and a url is
       exactly the value whose spelling the grammar is about.
       `css_shorthand_validates_longhand` GOES ON ANSWERING FALSE FOR `src` AND THAT IS NOT A GAP — the value
       has been through its grammar HERE, so `cssd_decls_collect_declaration` stores the answer verbatim and
       a second entry in that component would be a second grammar over one descriptor. */
    if (strcmp(name, "src") == 0 && cssd_block_is_descriptor_body(context)) {
        char *raw = cssd_decl_source_value(d, text, len);

        value = raw ? css_font_src_descriptor_value(raw) : NULL;
        free(raw);
        if (!value) { free(name); return false; }
        *pname = name;
        *pvalue = value;
        return true;
    }
    if (d->type == LXB_CSS_PROPERTY__UNDEF) {
        value = cssd_decl_value(d);
        if (value && cssd_undef_is_declaration(name, value)) {
            *pname = name;
            *pvalue = value;
            return true;
        }
        free(name);
        free(value);
        return false;
    }
    *pname = name;
    *pvalue = cssd_decl_value(d);   /* the trimmed value — see its note */
    return true;
}

/* §6.6's PARSE A CSS DECLARATION BLOCK, from what lexbor's parser produced: every declaration expanded to its
   longhands and collapsed to one per property. This is the ONE builder — the serialization, `length`, `item`,
   every property read and every write go through it, so no two of them can disagree about what the block
   declares. */
static void cssd_decls_from_list(const lxb_css_rule_declaration_list_t *list, const char *text, size_t len,
                                 CssDecls *out)
{
    const lxb_css_rule_t *r;

    DCHECK(text != NULL,
           "a declaration list was collected without the TEXT it was parsed from. A property whose grammar "
           "this engine owns is asked about the page's own spelling, which lives at offsets into that text "
           "and nowhere else — so a caller that has lost it would silently drop every such declaration rather "
           "than answer it wrongly, and the block would read back as if the page had never written one");
    for (r = list ? list->first : NULL; r; r = r->next) {
        const lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(r);
        char *name, *value;

        if (r->type != LXB_CSS_RULE_DECLARATION) continue;
        if (!cssd_decl_take(d, text, len, out->context, &name, &value)) continue;
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
    lxb_css_rule_declaration_list_t *list;

    if (!text || !len) return;
    /* The list is taken into a local before the collect rather than passed inline, because the collect is also
       handed the TEXT the offsets on that list index and C fixes no order between two arguments. */
    list = cssd_parse_block(text, len, &mem);
    cssd_decls_from_list(list, text, len, out);
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
static char *cssd_value_in_decls(const CssDecls *d, const char *name, bool *pimportant)
{
    char *out = NULL;
    int at;

    at = cssd_decls_index(d, name);
    /* THE BLOCK DECLARES A PROPERTY AT MOST ONCE, so there is no "last wins" left to do here: which of several
       declarations of one property survives is decided where they are COLLECTED, by the cascade's own two
       criteria (importance, then order), and this is the one answer that came out. */
    if (at >= 0 && d->v[at].value) {
        /* css-values-5 "Appendix A: Arbitrary Substitution Functions" / "Substitution in Shorthand
           Properties" — AN APPENDIX, so the titles are the citation: "Pending-substitution values must be
           serialized as the empty string, if an API allows them to be observed." THIS ENTRY IS EVERY SUCH
           API for a LONGHAND — §6.6.1's getPropertyValue reaches it, and so does `cssom_declared_value` —
           so the empty string is answered here rather than at each of them.
           IT IS THE EMPTY STRING AND NOT NULL, because NULL already means the block declares the property
           NOWHERE and a shorthand containing an arbitrary substitution function does declare its longhands —
           css-values-5 "Substitution in Shorthand Properties" again (the audit reports this quotation
           against cssom §6.6.1, the nearest indexed anchor above it — see
           core/css/css_pending_substitution.h, and do not repair it here): "the longhand properties it’s
           associated with must instead be filled in". Collapsing the two would
           make `margin: var(--g)` read back as a block that never mentioned `margin-top`, which is the state
           this engine was in before the pending-substitution value existed. */
        out = cssd_strdup(css_pending_is(d->v[at].value) ? "" : d->v[at].value);
        if (pimportant) *pimportant = d->v[at].important;
    }
    return out;
}

/* The same question asked of a block that is kept as TEXT and nothing else — the cascade's element-attached
   layer and `cssom_declared_value`'s callers, neither of which has a §6.6 block object to hold a store. */
static char *cssd_value_in_block(const char *text, size_t len, const char *name, bool *pimportant)
{
    CssDecls d = { 0 };
    char *out;

    cssd_decls_from_text(text, len, &d);
    out = cssd_value_in_decls(&d, name, pimportant);
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
static char *cssd_property_value(const CssDecls *d, const char *name)
{
    const char *const *lh;
    const char *values[CSS_SHORTHAND_MAX_LONGHANDS];
    unsigned n, i;
    bool important = false, ok = true;
    char *out = NULL;
    int at;

    lh = css_shorthand_longhands(name, &n);
    if (!lh) return cssd_value_in_decls(d, name, NULL);
    CHECK(n <= CSS_SHORTHAND_MAX_LONGHANDS,
          "cssom: a shorthand's longhand list outgrew the array §6.6.1's getPropertyValue sized from it");
    for (i = 0; i < n && ok; i++) {
        at = cssd_decls_index(d, lh[i]);
        /* "If declaration is null, then return the empty string", and "if important flags of all declarations
           in list are same, then return the serialization of list" — otherwise the empty string. */
        if (at < 0 || !d->v[at].value) { ok = false; break; }
        if (i == 0) important = d->v[at].important;
        else if (d->v[at].important != important) { ok = false; break; }
        values[i] = d->v[at].value;
    }
    if (ok) out = css_shorthand_serialize_value(name, (const char *const *)values);
    return out;
}

/* §6.6.1's GET PROPERTY PRIORITY, whose shorthand step is the same shape with the values thrown away: "for
   each longhand property longhand that property maps to, append the result of invoking getPropertyPriority()
   with longhand as argument to list. If all items in list are the string 'important', then return the string
   'important'." A longhand the block does not declare has no priority, so the answer is not "important". */
static bool cssd_property_important(const CssDecls *d, const char *name)
{
    const char *const *lh;
    unsigned n, i;
    bool all = true;

    lh = css_shorthand_longhands(name, &n);
    if (!lh) {
        bool imp = false;
        char *v = cssd_value_in_decls(d, name, &imp);
        bool got = v != NULL && imp;

        free(v);
        return got;
    }
    for (i = 0; i < n && all; i++) {
        int at = cssd_decls_index(d, lh[i]);

        all = at >= 0 && d->v[at].value != NULL && d->v[at].important;
    }
    return all;
}

/* css-cascade-5 §6.1's ELEMENT-ATTACHED layer — the contents of the style attribute, which is per-flow
   because the attribute it reads is. It reports the IMPORTANCE into the cascade because css-cascade-5 §6.1
   compares that first: attached-ness is the criterion BELOW origin and importance, not above it.
   @LOGICAL — IT COLLECTS THE css-logical-1 §4 PAIR AND NOT ONE PROPERTY, which is why it is a collector and
   no longer an entry returning one value. The two declarations are ordered against EACH OTHER inside one
   block, so each is added with its INDEX in that block; two separate single-property reads have no shared
   position to order by, and the single-property entry that used to stand here is gone rather than kept
   beside this one — it answered a question no caller asks any more.
   THE BLOCK IS PARSED ONCE, which is also why the shared `cssd_value_in_block` is not reused here: that entry
   answers ONE property and throws the parse away, so asking it twice would parse the attribute twice and
   still not say which declaration came first. */
static void cssd_inline_collect(lxb_dom_element_t *el, const char *name, const char *partner,
                                CssCascade *cascade, CssLayerOrder *order, uint32_t *pseq)
{
    size_t len = 0;
    const char *text = cssd_inline_text(el, &len);
    const char *want[2];
    CssDecls d = { 0 };
    unsigned k, n = 0;

    want[n++] = name;
    if (partner != NULL) want[n++] = partner;
    cssd_decls_from_text(text, len, &d);
    ++*pseq;
    for (k = 0; k < n; k++) {
        int at = cssd_decls_index(&d, want[k]);

        if (at >= 0 && d.v[at].value)
            css_cascade_add(cascade, CSS_ORIGIN_AUTHOR, d.v[at].important, true,
                            css_layer_order_root(order), 0, *pseq, (uint32_t)at, d.v[at].value);
    }
    cssd_decls_free(&d);
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

/* ---- THE SHEET, PARSED ONCE PER CONTENT -----------------------------------------------------------------
 *
 * THE DEFECT IS NOT A SLOW PARSER, IT IS ONE TEXT PARSED ONCE PER ASK. The walk below rebuilds a sheet's
 * serialization out of its rule objects and then hands that text to lexbor to get back the selector lists
 * and declaration blocks those rules were parsed FROM — a round trip CSSOM §6.4's objects force, because a
 * rule holds the SERIALIZATION lexbor produced for it and nothing else. The round trip runs per
 * (element, property): css-cascade-5 §7.2 "Inheritance" climbs to the root and css-logical-1 §4
 * "Flow-Relative Box Model Properties" makes two further climbs a PREREQUISITE of every box-model
 * resolution, so one box-model read on a deep document re-parses every author sheet once per ancestor per
 * property. core/css/css_cascade_pass.h removes the repeated (element, property) ASK and is silent about
 * this, because its record is keyed on the question and this cost is paid on every MISS.
 *
 * IT IS KEYED ON THE BYTES, WHICH IS WHAT MAKES IT IMPOSSIBLE FOR ONE FLOW TO BE SERVED ANOTHER FLOW'S
 * PARSE RATHER THAN MERELY UNLIKELY. The standing objection to holding a lexbor arena at all is stated at
 * `cssd_parse_block` — one arena per parse, `so nothing outlives the read that asked for it, which is what
 * keeps this free of state the flow machinery would have to swap` — and core/css/css_style_declaration.h
 * states the same rule for rule OBJECTS, which are handed out as text because they `park to the IDB cold
 * tier and fork per flow, and a rule named by a pointer into a freed arena can do neither`. Both are about
 * (Those two runs are BACKTICKED and not quoted: they are THIS TREE'S OWN PROSE being shown, and the
 * citation auditor anchors a quotation on the nearest preceding citation — which here is a CSS standard —
 * so quoting them would report two verbatim sentences of this file as a fabricated spec quotation.)
 * state named by an OBJECT, whose content differs per flow while its identity does not. This table is named
 * by CONTENT: an entry is served only when the emission the caller just built is byte-identical to the
 * emission the entry was parsed from, and `lxb_css_stylesheet_parse` is a function of that text and of
 * nothing else — the parser's own arena is swapped in and out around each parse and its selector record is
 * asserted unchanged after it. So a hit and a miss produce the same stylesheet, and there is no difference
 * for a flow to observe. Two flows whose sheets differ emit different text, miss, and each parse their own;
 * two flows alternating on one slot thrash it, which is correct and is exactly today's cost.
 *
 * ONE ENTRY PER SHEET POSITION IS WHAT KEEPS IT FROM BEING THE ACCUMULATION `cssd_parse_block` REFUSES. A
 * parser-lifetime arena would hold every sheet this engine ever parsed; a slot holds ONE parse and destroys
 * it the instant that slot's content changes, so the table's whole size is one parsed copy of the sheet set
 * the document actually has — which is what a browser holds anyway (Blink's StyleSheetContents is the same
 * fact, built once per content and shared). It is NOT flow state under CLAUDE.md's
 * §PLATFORM-DATA-A-FLOW-QUEUES-IS-A-JS-VALUE: nothing here is captured into the COW delta, no rule and no
 * sheet names it, every entry is DERIVED from an emission any flow can rebuild, and dropping the whole table
 * at any instant changes no answer. It is the re-derivable tier of §Time-travel-resume rather than a cap or
 * a floor.
 *
 * WHAT IT DOES NOT REMOVE, STATED BECAUSE A READER WILL EXPECT IT TO: the EMISSION. `css_rule_cascade_sheet`
 * still walks every rule object of every sheet on every ask, because the emission is what produces the key
 * — there is no cheaper fingerprint of a flow's rule objects than the text they serialize to. What goes is
 * the arena create, the full CSS parse of that text, and the arena destroy.
 *
 * THE ROUND-TRIP ASSERTIONS ARE UNWEAKENED AND STILL RUN PER ASK, which is worth saying because a cached
 * parse sounds like it would move them: they live in the rule-matching loop below, which walks the cached
 * stylesheet against the FRESHLY EMITTED `view.n` and `view.layer` on every ask, so a parse that had somehow
 * come from different bytes than the emission beside it fires them at the first ask rather than at the first
 * parse.
 *
 * A SHEET WHOSE TEXT DOES NOT PARSE LEAVES ITS SLOT EMPTY and is re-parsed on every ask, exactly as it is
 * today. Caching the failure would be a second thing to invalidate for no answer served.
 *
 * RETIREMENT: this table goes when a CSSOM §6.4 rule owns a parsed form that survives its own serialization
 * — there is then no text to re-parse and no key to hold. */
typedef struct {
    char                 *text;  /* OWNED — the emission this parse is OF, and the whole of the key */
    size_t                len;
    lxb_css_memory_t     *mem;   /* OWNED — the arena the stylesheet, its rules and its selectors live in */
    lxb_css_stylesheet_t *sst;   /* BORROWED from `mem`, which is the one owner */
} CssdSheetParse;

/* Indexed by the sheet's position in CSSOM §6.2's list — a POSITION and not an identity, so two documents in
   one agent share slots and a mismatch there costs a re-parse and never a wrong answer. */
static CssdSheetParse *g_sheet_parse;
static uint32_t        g_sheet_parse_n;

static void cssd_sheet_parse_drop(CssdSheetParse *e)
{
    if (e->mem != NULL) {
        /* THE ARENA BEING FREED MUST NOT BE THE ONE THE PARSER IS CURRENTLY PARSING INTO. Every parse below
           sets the parser's arena and takes it back on the same two lines, so this can only fire if a parse
           acquired a slot's arena and left it installed — which is the dangling-parser-state defect
           `cssd_selectors_intact` already exists for, arriving through the memory pointer instead. */
        DCHECK(g_parser == NULL || lxb_css_parser_memory(g_parser) != e->mem,
               "a cached sheet parse was dropped while the CSS parser was still pointing at its arena — the "
               "next parse would allocate out of freed memory. Every parse in this file sets the parser's "
               "arena and takes it back before it returns, so find the one that returned without doing so");
        lxb_css_memory_destroy(e->mem, true);
    }
    free(e->text);
    e->text = NULL;
    e->len = 0;
    e->mem = NULL;
    e->sst = NULL;
}

static void cssd_sheet_parse_free(void)
{
    uint32_t i;

    for (i = 0; i < g_sheet_parse_n; i++) cssd_sheet_parse_drop(&g_sheet_parse[i]);
    free(g_sheet_parse);
    g_sheet_parse = NULL;
    g_sheet_parse_n = 0;
}

/* THE PARSE OF `text`, WHICH IS EITHER THE ONE SLOT `si` ALREADY HOLDS OF THESE EXACT BYTES OR A FRESH ONE
   THAT REPLACES IT. NULL when the text did not parse, which is the same answer and the same cost the
   uncached path gave. The returned stylesheet is BORROWED and is valid until this slot is next replaced —
   which, because the author walk is asserted non-re-entrant at its call, cannot happen while a caller holds
   one. */
static lxb_css_stylesheet_t *cssd_sheet_parsed(uint32_t si, const char *text, size_t len)
{
    CssdSheetParse *e;
    lxb_css_memory_t *mem;
    lxb_css_stylesheet_t *sst;
    char *copy;

    DCHECK(text != NULL && len > 0,
           "a sheet was handed to the parse table with no emission to be a parse OF — the walk tests for an "
           "empty emission before it gets here, because a sheet that declares only `@layer` names emits no "
           "text and has nothing to match against");
    if (si >= g_sheet_parse_n) {
        uint32_t want = si + 1;
        CssdSheetParse *grown = realloc(g_sheet_parse, (size_t)want * sizeof *grown);

        CHECK(grown != NULL, "cssom: the per-sheet parse table allocation failed");
        memset(grown + g_sheet_parse_n, 0, (size_t)(want - g_sheet_parse_n) * sizeof *grown);
        g_sheet_parse = grown;
        g_sheet_parse_n = want;
    }
    e = &g_sheet_parse[si];
    if (e->text != NULL && e->len == len && memcmp(e->text, text, len) == 0) {
        /* THE KEY IS THIS TABLE'S OWN COPY AND NEVER THE CALLER'S POINTER, which is the one invariant a
           later reader can quietly break to save a `memcpy`: the caller frees its emission at the end of
           every sheet iteration, so a key borrowed from it would be read out of freed memory on the very
           next ask and would then compare equal to whatever happened to be reallocated there. */
        DCHECK(e->text != text,
               "the per-sheet parse table is keyed on a pointer its caller owns rather than on a copy of "
               "its own — the emission is freed at the end of each sheet iteration, so every later "
               "comparison would read freed memory. Copy the bytes at the store");
        DCHECK(e->sst != NULL,
               "the per-sheet parse table holds an emission with no stylesheet parsed from it — the store "
               "below is the only writer and it stores the two together, so a half-filled slot is a failure "
               "arm that took the key with it");
        return e->sst;
    }
    cssd_sheet_parse_drop(e);
    mem = lxb_css_memory_create();
    if (mem == NULL) return NULL;
    if (lxb_css_memory_init(mem, 128) != LXB_STATUS_OK) {
        lxb_css_memory_destroy(mem, true);
        return NULL;
    }
    sst = lxb_css_stylesheet_create(mem);
    /* The ARENA IS THE PARSER'S FOR THE DURATION OF THE PARSE AND THIS TABLE'S AFTERWARDS — set it, parse,
       take it back, exactly as `cssd_parse_block` does. What differs is only who owns it when the parse
       returns. */
    lxb_css_parser_memory_set(g_parser, mem);
    if (sst && lxb_css_stylesheet_parse(sst, g_parser, (const lxb_char_t *)text, len) != LXB_STATUS_OK)
        sst = NULL;
    lxb_css_parser_memory_set(g_parser, NULL);
    cssd_selectors_intact();
    if (sst == NULL) {
        lxb_css_memory_destroy(mem, true);
        return NULL;
    }
    copy = malloc(len + 1);
    CHECK(copy != NULL, "cssom: the per-sheet parse key allocation failed");
    memcpy(copy, text, len);
    copy[len] = '\0';
    e->text = copy;
    e->len = len;
    e->mem = mem;
    e->sst = sst;
    return sst;
}

/* @LOGICAL — THE ONE PARSER IS NOT RE-ENTRANT AND A NESTED RESOLUTION IS NOW REACHABLE. The AUTHOR-ORIGIN sheet walk below drives
   `g_parser` — it sets the parser's arena around each parse it has to make — and it owns the per-sheet parse
   table above, whose slots it REPLACES when a sheet's emission has changed, destroying the arena the
   previous parse lived in. css-logical-1 §4 "Flow-Relative Box Model Properties"'s pairing made the cascade
   ask for a computed `writing-mode` in the middle of resolving some other property, which is a second
   resolution and therefore a second walk. The bracket that asserts they do not nest is at the CALL, in
   `cssom_cascaded_value`, because that is where the ordering which keeps them apart is written and this body
   has two exits. */
static bool g_collecting_sheets = false;

/* EVERY AUTHOR-ORIGIN DECLARATION OF `name` ON `el`, added to `cascade` — not the one that wins. css-cascade-5
   §6.1's sort is over the whole list at once and css-cascade-5 §7.3's roll-backs re-run it with a part removed,
   so a collector that kept only a running best would have thrown away exactly what both need. `*pseq` is the
   document-order counter css-cascade-5 §6.1's Order of Appearance reads, carried across the sheets so it is one
   sequence and not one per sheet, and bumped per RULE so it also names the rule css-cascade-5 §7.3.6's
   `revert-rule` removes. */
static void cssd_author_collect(lxb_dom_element_t *el, const char *name, const char *partner,
                                CssCascade *cascade, CssLayerOrder *order, uint32_t *pseq)
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
        lxb_css_stylesheet_t *sst;

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
        /* HTML §4.2.4.1 "Processing the media attribute"'s MUST, and it governs BOTH kinds of sheet in
           this list rather than only the `<link>` that made it reachable: "if the link is an external
           resource link, then the media attribute is prescriptive. The user agent must APPLY the external
           resource when the media attribute's value matches the environment and the other relevant
           conditions apply, and must not apply it otherwise."
           A `<style media=print>` was already reaching this loop and cascading, which is the same wrong
           answer through the other creator — CSSOM §6.1 gives a `<style>` sheet its media from the identical
           content attribute, and CSS Cascade 5 §6.2 admits a declaration to the author origin only from a
           sheet that applies. One question asked the same wrong way at two sites is one defect, so the
           repair is here, at the one place both creators' sheets are read, rather than at whichever of them
           was noticed. What made it REACHABLE at scale is the `<link>` arm: a real page's print rules ship in
           a separate `media=print` sheet, and cascading one over a screen render is the difference between a
           screenshot of the page and a screenshot of its printout.
           THE `@media` AT-RULES INSIDE THE SHEET ARE A DIFFERENT QUESTION AND ARE ALREADY ANSWERED BELOW —
           §4.2.4.1's own note says so ("the external resource might have further restrictions defined within
           that limit its applicability. For example, a CSS style sheet might have some @media blocks. This
           specification does not override such further restrictions"), so this gate is the OUTER one and
           neither subsumes nor duplicates it. */
        if (!css_style_sheet_media_matches(ctx, sheet)) { JS_FreeValue(ctx, sheet); continue; }
        if (!cssd_sheet_view(ctx, sheet, order, &view)) { JS_FreeValue(ctx, sheet); continue; }
        JS_FreeValue(ctx, sheet);
        /* A SHEET CAN DECLARE LAYERS AND EMIT NO RULES — `@layer a, b;` alone is a whole sheet establishing an
           order for the sheets after it — so the emptiness is tested on the emission and the walk that just
           happened has already done the part that matters. */
        if (!view.text) { css_rule_cascade_sheet_free(&view); continue; }
        /* THE SELECTOR LISTS AND DECLARATION BLOCKS THIS EMISSION PARSES BACK TO, WHICH IS A PARSE ONLY
           WHEN THIS SHEET'S EMISSION HAS CHANGED SINCE THE LAST ASK. See the parse table above for why a
           served parse cannot be another flow's: the key is the emission's own BYTES, so a hit is a parse of
           exactly what a miss would have parsed. */
        sst = cssd_sheet_parsed(si, view.text, strlen(view.text));
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
                cssd_decls_from_list(st->declarations, view.text, strlen(view.text), &rd);
                /* @LOGICAL — BOTH MEMBERS OF css-logical-1 §4's PAIR ARE TAKEN FROM THIS ONE BLOCK, in ONE
                   pass and with each declaration's own index in it. The index is css-cascade-5 §6.1's Order
                   of Appearance at the granularity a block has: two names in one rule share `*pseq +
                   at_rule` — which is that rule's identity for css-cascade-5 §7.3.6's `revert-rule` — and
                   css-logical-1 §4's worked example turns on which of them the author wrote second.
                   A SECOND WALK FOR THE PARTNER WOULD BE WRONG AND NOT MERELY WASTEFUL: every rule of the
                   second walk would carry a LATER `*pseq` than every rule of the first, so the partner would
                   win every tie in the document regardless of where it was written. */
                at = cssd_decls_index(&rd, name);
                if (at >= 0 && rd.v[at].value)
                    css_cascade_add(cascade, CSS_ORIGIN_AUTHOR, rd.v[at].important, false, layer,
                                    (uint32_t)spec, *pseq + at_rule, (uint32_t)at, rd.v[at].value);
                if (partner != NULL) {
                    int pat = cssd_decls_index(&rd, partner);

                    if (pat >= 0 && rd.v[pat].value)
                        css_cascade_add(cascade, CSS_ORIGIN_AUTHOR, rd.v[pat].important, false, layer,
                                        (uint32_t)spec, *pseq + at_rule, (uint32_t)pat, rd.v[pat].value);
                }
                cssd_decls_free(&rd);
            }
            DCHECK(back == view.n,
                   "a CSS style sheet's rules did not ROUND-TRIP: the rule objects serialized to a text that "
                   "parses back to FEWER rules than went in. A rule holds the SERIALIZATION lexbor produced "
                   "for it, so re-parsing it must yield exactly what it came from — find which rule's text "
                   "does not, and fix the serializer that wrote it rather than tolerating the drift");
        }
        /* THE ARENA IS THE ONE OWNER AND IT IS THE PARSE TABLE'S, NOT THIS LOOP'S. The reason it is freed
           OUTRIGHT rather than through lexbor's own destroy is unchanged and is why that call appears
           nowhere here: `lxb_css_stylesheet_create` REF-INCREMENTS the memory it is handed (to 2, since
           `lxb_css_memory_init` starts it at 1) and `lxb_css_stylesheet_destroy` only ref-DECREMENTS (back
           to 1), so that destroy frees nothing at all. The stylesheet, its rules and its selectors are all
           allocated FROM the arena, so destroying the arena is what releases them — which the table does
           when it replaces a slot and at `cssom_free`. Nothing this loop keeps points into it: every value
           it takes out of a matched rule is copied. */
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
        CssLogicalGroup group;
        bool physical = true, mine = true, chosen = false;

        for (i = 0; i < n; i++)
            if (at[i] == j) { chosen = true; break; }
        if (chosen) continue;
        group = css_logical_group_of(d->v[j].name, &physical);
        /* A property in no group pairs with nothing, so it cannot reorder one. */
        if (group == CSS_LOGICAL_GROUP_NONE) continue;
        for (i = 0; i < n; i++)
            if (css_logical_group_of(lh[i], &mine) == group && mine != physical) return false;
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
        /* css-values-5 "Substitution in Shorthand Properties": "Pending-substitution values must be
           serialized as the empty string, if an API allows them to be observed." REACHED ONLY WHERE THE
           SHORTHAND ITSELF COULD NOT BE WRITTEN — `css_shorthand_serialize_value` answers the shorthand's
           own original value when every one of its longhands is pending from it, so this line is the
           standard's `Otherwise` arm: a block holding `margin: var(--g) 0; margin-top: 5px` has three
           pending longhands and one real one, no shorthand can absorb them, and each pending one goes out
           under its own name with an empty value. */
        cssd_append_declaration(&out, &first, d->v[i].name,
                                css_pending_is(d->v[i].value) ? "" : d->v[i].value, d->v[i].important);
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

/* §6.6'S SERIALIZATION IS NO LONGER THIS BLOCK'S BACKING, AND `cssd_backing_assert_round_trip` IS RETIRED
   WITH THE STATE IT ASSERTED OVER. That check asked whether the text a write was about to STORE still NAMED
   every declaration whose value no page can spell, and answered no for a block holding `margin: var(--g) 0`
   written through `el.style.marginTop = '5px'` — measured, 3 of 3, at the abort it named. Its own stated
   retirement condition was that the record goes when the block's backing keeps its declarations as values
   rather than as their serialization, after which a serialization rule cannot reach the storage and there is
   no round trip to assert over. `cssd_declarations_put` is that backing, so the condition is met and the
   record goes with the diff that met it rather than being kept beside it.
   WHAT REPLACES IT IS IN `cssd_decls_load` AND ASKS A DIFFERENT QUESTION, which is why it is not this check
   moved: the store's declarations must SERIALIZE BACK to the projection they were filed beside, so what is
   asserted is this file's own encode against its own decode, over two `CssDecls` built by two different runs.
   The old question — does the projection still name them — is now a question about a string the block does
   not store, and its answer is NO by design for exactly the values the store exists to carry. */

static char *cssd_serialize_block(const lxb_css_rule_declaration_list_t *list, const char *text, size_t len)
{
    CssDecls d = { 0 };
    char *out;

    cssd_decls_from_list(list, text, len, &d);
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
static char *cssd_serialize_at_block(const lxb_css_rule_list_t *block, const char *text, size_t len)
{
    CssDecls d = { 0 };
    const lxb_css_rule_t *r;
    char *out;

    for (r = block ? block->first : NULL; r; r = r->next)
        if (r->type == LXB_CSS_RULE_DECLARATION_LIST)
            cssd_decls_from_list(lxb_css_rule_declaration_list(r), text, len, &d);
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
            block = cssd_serialize_block(st->declarations, text, len);
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
            block = cssd_serialize_block(bad->declarations, text, len);
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
                block = cssd_serialize_at_block(kids, text, len);
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
 * bundle actually branches on. Modelling it is the difference between answering the spec's value and shrugging.
 *
 * WHETHER A DECLARATION THIS TABLE DOES NOT CARRY IS AN HONEST ABSENCE IS A QUESTION ABOUT THE PROPERTY AND IS
 * ASKED PER PROPERTY. This lead-in used to answer it for the whole table at once — what is not here "is
 * honestly absent and reads as the property's initial value" — and that sentence is TRUE and is not the same
 * sentence as "harmless". An initial value is a REAL value a consumer acts on, so the absence is honest only
 * where nothing can tell it apart from the declaration; the paragraph directly below is that test FAILING for
 * `display`, and it fails the same way one property along. HTML §15.3.8 Tables gives every table part
 * `vertical-align: middle` where the initial value is `baseline`, and core/layout/table_height.c reads a
 * cell's alignment to decide which arm of CSS 2.1 §17.5.3 Table height algorithms measures the row it is in —
 * so an ordinary multi-cell row was measured through a procedure no browser applies to it. The question to ask
 * of a rendering-section declaration is therefore not whether it is missing but whether anything READS the
 * property, and where something does, the row is owed rather than excused.
 *
 * A TAG THIS TABLE DOES NOT NAME IS `inline`, WHICH IS WHY A MISSING ROW IS NOT A MISSING ANSWER BUT A WRONG
 * ONE, AND WHY IT IS SILENT. `cssd_ua_value`'s last line is the UA sheet's own default, so an element the
 * table forgets does not reach a crash and does not read as unset — it reads as a real value that three
 * components then act on. core/dom/element_view.c's `clientWidth`/`clientHeight` step 1, which is
 * CSSOM View §6 Extensions to the Element Interface's, is "if the element has
 * no associated box OR IF THE BOX IS INLINE, return zero", so a forgotten row makes those members answer 0 for
 * an element whose padding edge this engine can compute; `element_view_fragment_kind` reads `inline` as ONE
 * FRAGMENT PER LINE BOX, so `offsetWidth`, `offsetHeight` and `getClientRects` abort naming CSS 2 §9.4.2's
 * inline formatting context for an element that has no inline formatting context to wait on; and
 * core/layout/block_flow.c's child classification refuses to lay out any block container holding one. A zero
 * that a page compares against is CLAUDE.md §Architecture's plausible datum, and this is where it is born.
 *
 * EVERY ROW IS TRANSCRIBED FROM HTML'S RENDERING SECTION, BY NUMBER AND TITLE, AND A ROW IS ALWAYS A LONGHAND.
 * `cssom_cascaded_value` asserts that the cascade is over longhands only, and `cssd_ua_value` finds a row by
 * `strcmp` on the property NAME — so a row spelling a shorthand is a declaration no read can ever reach, which
 * is the write-with-no-reader shape and not a smaller transcription. A shorthand the rendering section writes
 * is transcribed as the longhands css-cascade-5 §3 "Shorthand Properties" makes it set, that section's own
 * sentence deciding the ones the shorthand omits: "each “missing” sub-property is assigned its initial value".
 * The STANDARD IS WRITTEN ON EVERY ROW rather than in this lead-in, because a citation is checkable only where
 * its standard is inside the forty characters before the `§` — see the convention at the head of this file —
 * and a list is exactly the shape where one name at
 * the top covers none of the numbers under it. The sections are:
 *   HTML §15.3.1  Hidden elements                  — the fourteen-element `display: none` rule
 *   HTML §15.3.2  The page                         — `html, body { display: block }`
 *   HTML §15.3.3  Flow content                     — the block-level flow rule, `slot { display: contents }`, and
 *                                                    its two margin rules `margin-block: 1em` / `margin-inline: 40px`
 *   HTML §15.3.6  Sections and headings            — `article, aside, :heading, hgroup, nav, section`, and its six
 *                                                    `:heading(n)` rules' font sizes and block margins
 *   HTML §15.3.7  Lists                            — `dir, dd, dl, dt, menu, ol, ul`, `li { display: list-item }`,
 *                                                    and its two unconditional inline-edge rules
 *   HTML §15.3.8  Tables                           — the nine table box types, its two `vertical-align` rules, and
 *                                                    `td, th { padding: 1px }`
 *   HTML §15.3.10 Form controls                    — `input, button { display: inline-block }`
 *   HTML §15.3.11 The hr element                   — its block margins and its `auto` inline margins
 *   HTML §15.3.12 The fieldset and legend elements — `fieldset { display: block }`, and the fieldset's and the
 *                                                    legend's margins and paddings
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
 * type, and closing it needs a structural selector this layer does not evaluate. HTML §15.3.7's list-style rules
 * are the same absence stated once above.
 *   `:heading(n)`'S FONT SIZES AND BLOCK MARGINS USED TO BE ON THAT LIST AND ARE NOT AN ABSENCE ANY MORE —
 *   `cssd_ua_heading_font_size` and `cssd_ua_heading_margin_block`
 *   below answer them, because the level that pseudo-class selects on is HTML §4.3.11.1 "Heading levels & offsets"'s
 *   algorithm over the element alone rather than a selector anything has to match. The clause is rewritten and
 *   not deleted so that a reader who re-derives a-pseudo-class-needs-a-matcher from the `summary` case above
 *   does not conclude the same of a level: the two are different in kind, and which one a rule is decides
 *   whether it is buildable here.
 *   THE MARGINS OF HTML §15.3.3 Flow content AND HTML §15.3.6 Sections and headings ARE HERE NOW — the first
 *   as longhand rows of the table, the second as `cssd_ua_heading_margin_block` beside the font sizes — AND
 *   THE RESIDUAL THAT NAMED THEM IS RETIRED WITH ITS REASONING REWRITTEN RATHER THAN DROPPED, TWICE OVER,
 *   because both retired reasons are ones a reader re-derives from the code in front of them.
 *   THE FIRST RETIRED REASON WAS THE LAYER, and it was retired by the diff that landed core/css/css_logical.h.
 *   It read: a row spelling `margin-block-start` is a declaration the cascade would carry and NO layout read
 *   would ever ask for — the write-with-no-reader shape — since core/layout/used_value.c carries the ten
 *   PHYSICAL box-model lengths and DFAILs by name on a logical spelling; and a row spelling `margin-top`
 *   instead would be this file deciding a writing mode on core/layout's behalf. THE SECOND HALF STILL HOLDS
 *   AND THE FIRST DOES NOT. css-logical-1 §4 "Flow-Relative Box Model Properties" pairs the two properties and
 *   makes them SHARE A COMPUTED VALUE, so a `margin-block-start` declaration is what `margin-top`'s computed
 *   value is cascaded from — `cssom_cascaded_value` reads the partner and collects BOTH members' UA rows into
 *   one cascade — and block_flow.c's `used_value_px(el, "margin-top")` therefore reads a row written
 *   logically. The layer was the whole of that error: at `used_value_px`, where the retired clause put it, the
 *   mapping would have left the COMPUTED value of `margin-top` at its initial `0`, which
 *   css-logical-1 §4's own last sentence forbids.
 *   THE SECOND RETIRED REASON WAS THE REPLACEMENT CLAUSE ITSELF, WHICH WAS WRONG WHEN WRITTEN, and that is
 *   recorded here rather than quietly corrected because a next-diff clause is read once, by somebody who has
 *   already decided to do the work. It said these rows "need the UA table to carry a row whose property is a
 *   css-logical-1 §4.2 SHORTHAND, since `margin-block` expands to two longhands and every row here states one
 *   property". THE PREMISE IS THIS BLOCK'S OWN RULE AND THE CONCLUSION INVERTS IT: the lead-in twelve lines
 *   above says a row is ALWAYS A LONGHAND and says why — `cssd_ua_value` finds a row by `strcmp` on the
 *   property name and `cssom_cascaded_value` asserts the cascade is over longhands only — so a shorthand row
 *   is unreachable by construction and the table needed no new capability at all. What the rows needed was the
 *   expansion the lead-in already prescribes, and css-logical-1 §4.2 states it: "If only one value is given,
 *   it applies to both the start and end edges." THE TELL WAS IN THE CLAUSE'S OWN SENTENCE — it named a
 *   mechanism ("carry a shorthand row") as the thing to build, and a clause that names a mechanism is a claim
 *   about THIS TREE written by somebody who knew what was missing and was guessing at what fills it.
 *   THE FOUR OTHER SECTIONS' MARGINS AND PADDINGS ARE HERE NOW except for TWO RULES, and the clause that
 *   named them is rewritten rather than deleted because it was wrong in a way a reader re-derives.
 *   IT WAS A PER-SECTION ANSWER TO A PER-RULE QUESTION. It read `§15.3.7 IS THE ONE THAT MUST NOT LAND
 *   ALONE`, on the ground that `:is(dir, dl, menu, ol, ul) :is(dir, dl, menu, ol, ul) { margin-block: 0 }` is
 *   a DESCENDANT COMBINATOR this layer does not evaluate, so a type rule landing without it gives a NESTED
 *   list the 1em a browser takes away. That reasoning is exactly right and it is about ONE of HTML §15.3.7
 *   "Lists"' three margin-and-padding rules. The section has three descendant rules and every one of them
 *   governs `margin-block`, `list-style-type` or (in quirks mode) `list-style-position`; NOTHING in it alters
 *   a `padding-inline-start`, and `dd` is a member of none of the `:is()` lists — so `dd { margin-inline-start:
 *   40px }` and `dir, menu, ol, ul { padding-inline-start: 40px }` answer the same at every nesting depth in a
 *   browser and land below, while `dir, dl, menu, ol, ul { margin-block: 1em }` waits.
 *   AND ITS ENUMERATION WAS SHORT BY A RULE INSIDE THE SECTION IT HAD JUST LANDED, which is the failure a
 *   list of absences has: it named four OTHER sections and HTML §15.3.3 "Flow content"'s own
 *   `dialog { margin: auto; padding: 1em }` is a bare type selector in that section's main block — neither a
 *   presentational hint nor a quirks rule — and is owed here as much as any of them.
 *   WHAT IS NOT COVERED, AS TWO RULES RATHER THAN AS SECTIONS:
 *     HTML §15.3.7 "Lists"' `dir, dl, menu, ol, ul { margin-block: 1em }`, for the companion-rule reason
 *     above. WHAT THE NEXT DIFF BUILDS: a UA rule whose key is a SELECTOR compiled through
 *     core/dom/selector_match.h's `selector_list_compile` and matched with `selector_match_node` — which
 *     `cssd_author_collect` above already runs once per element per rule — so this table's `{tag, prop}` key
 *     becomes the degenerate shape of one; the §15.3.8 `table > tr` comment below names the same diff.
 *     HTML §15.3.3 "Flow content"'s `dialog { margin: auto; padding: 1em }`, which is NINE declarations of one
 *     rule whose parts decide each other: `position: absolute`, `width: fit-content` and `height: fit-content`
 *     are what make its `margin: auto` CENTRE the box. Landing the margin alone would be RIGHT ONLY WHILE
 *     `position` IS MISSING — a static block takes CSS 2.1 §10.3.3's rule 5 and both margins are 0 — and
 *     core/layout/used_value.c DFAILs by name on that state, in its own words — "a HORIZONTAL margin
 *     computes to `auto` on an ABSOLUTELY POSITIONED box, whose used value CSS 2.1 §10.3.7 solves from its
 *     own constraint equation" — so the row would silently become an abort on the day the row beside it
 *     lands. WHAT THE NEXT DIFF BUILDS: CSS 2.1 §10.3.7 "Absolutely positioned, non-replaced elements"' used
 *     `left`/`right` and static position in core/layout/used_value.c, and css-sizing-3 §3.2 "Sizing Values:
 *     the <length-percentage [0,∞]>, auto | none, stretch, min-content, max-content, and fit-content
 *     values"' `fit-content`, with the whole rule landing together afterwards.
 *   HOW THEIR ABSENCE WOULD SHOW: a nested `<ul>` carries a block margin between its items where a browser
 *   carries none, and an OPEN `<dialog>` is a full-width static band rather than a shrink-wrapped box centred
 *   in its containing block.
 *   RETIREMENT: this record goes when every margin and padding HTML §15 states over a bare type selector has
 *   a row here, at which point there is no partial transcription left for a reader to re-derive a reason for. */
/*
 * AND HTML §15.3.4 Phrasing content's `ruby { display: ruby }` / `rt { display: ruby-text }` ARE DELIBERATELY
 * ABSENT, which is the one place adding a row would make this engine WORSE rather than more complete, and the
 * test is the one this
 * whole comment is about. Every consumer of a computed `display` reads `inline` as a box it declines to
 * measure — core/dom/element_view.c's step 1 answers zero, its fragment count aborts naming CSS 2.1 §9.4.2 — while
 * `ruby` and `ruby-text` are box types NO consumer has an arm for, and the ARGUMENT FOR OMITTING THE ROWS HAS
 * CHANGED WHILE THE CONCLUSION HAS NOT — which is worth writing down, because the old reason is the one a
 * reader would re-derive. It used to be that core/layout/used_value.c's `uv_box_kind` would classify a ruby
 * box as BLOCK FLOW and hand CSS 2.1 §10.3.3's constraint equation a box CSS 2.1 §10 does not describe, so
 * `clientWidth` would stop being zero and start being a real number computed for the wrong box. That fall no
 * longer exists: `uv_box_kind`'s tail is a closed list and a ruby value now ABORTS there, naming CSS Ruby
 * Annotation Layout Module Level 1 and the three sections that build it. So adding these rows today would not
 * produce a wrong number — it would abort on every page containing a `<ruby>`, for a module that is not built.
 * THAT IS STILL NOT A REASON TO ADD THEM, and not because a crash is unwelcome: the crash is the module's
 * forcing function and it already stands, at the site where the missing algorithm is. A UA row would only move
 * WHICH ELEMENTS reach it, from the ones an author styled to every `<ruby>` in every document, and reaching a
 * known absence from more places is not progress. These two rows land in the same diff that gives CSS Ruby a
 * box type in `uv_box_kind` and in `element_view_fragment_kind`.
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
    /* HTML §15.3.3 Flow content's TWO MARGIN RULES, transcribed verbatim from that section:
         blockquote, figure, listing, p, plaintext, pre, xmp {
           margin-block: 1em;
         }

         blockquote, figure { margin-inline: 40px; }
       EACH IS FOUR LONGHAND ROWS PER TAG AND NOT ONE SHORTHAND ROW, which is the lead-in's rule and is where
       this file's own retired residual had it wrong: it named "a row whose property is a css-logical-1 §4.2
       SHORTHAND" as the thing the next diff needed, and `cssd_ua_value` finds a row by `strcmp` on the
       property NAME while `cssom_cascaded_value` asserts the cascade is over LONGHANDS ONLY — so a
       `margin-block` row is a declaration no read can ever reach. The expansion is the shorthand's own
       sentence, in css-logical-1 §4.2 "Flow-Relative Margins: the margin-block-start, margin-block-end,
       margin-inline-start, margin-inline-end properties and margin-block and margin-inline shorthands":
       "These two shorthand properties set the margin-block-start & margin-block-end and margin-inline-start &
       margin-inline-end, respectively. The first value represents the start edge style, and the second value
       represents the end edge style. If only one value is given, it applies to both the start and end edges."
       BOTH RULES GIVE ONE VALUE, so both edges of each pair carry it — and the emphasis is outside the
       quotation marks deliberately, because this file writes emphasis in capitals and a capitalised run
       inside a quotation is bytes the standard does not have.
       THE SPELLING IS THE SECTION'S OWN — FLOW-RELATIVE — AND THAT IS WHAT MAKES THE ROWS READABLE rather
       than a second decision about a writing mode. css-logical-1 §4 "Flow-Relative Box Model Properties"
       pairs each of these with one physical margin using the element's own computed writing mode and makes
       the pair SHARE A COMPUTED VALUE, and `cssom_cascaded_value` collects BOTH members of the pair into one
       cascade — so `used_value_px(el, "margin-top")`, which core/layout/block_flow.c asks of every block
       container, is answered by the `margin-block-start` row here in a horizontal-tb document and by the
       `margin-inline-start` row in a vertical one, with no line in this file naming an axis.
       NO PAIR HAS BOTH MEMBERS DECLARED HERE, which `cssom_cascaded_value` asserts rather than assumes: these
       are the flow-relative members and no physical margin row exists for any of these seven tags. The
       PHYSICAL margin spellings HTML §15.3.3 and HTML §15.4.2 Images do carry (`hr[align=left i]`,
       `img[align=left i]`) are presentational hints and quirks rules, which css-cascade-5 §6.5 puts in a
       different origin and which this table does not hold.
       WITHOUT THESE ROWS EVERY BLOCK-LEVEL BOX IN EVERY RENDERED DOCUMENT TOUCHED THE NEXT — a paragraph, a
       blockquote and a `<pre>` were separated from their neighbours by the line box alone, which is the
       picture the retired residual named as the way its absence would show. */
    { "blockquote", "margin-block-start", "1em" }, { "blockquote", "margin-block-end", "1em" },
    { "figure", "margin-block-start", "1em" },     { "figure", "margin-block-end", "1em" },
    { "listing", "margin-block-start", "1em" },    { "listing", "margin-block-end", "1em" },
    { "p", "margin-block-start", "1em" },          { "p", "margin-block-end", "1em" },
    { "plaintext", "margin-block-start", "1em" },  { "plaintext", "margin-block-end", "1em" },
    { "pre", "margin-block-start", "1em" },        { "pre", "margin-block-end", "1em" },
    { "xmp", "margin-block-start", "1em" },        { "xmp", "margin-block-end", "1em" },
    { "blockquote", "margin-inline-start", "40px" }, { "blockquote", "margin-inline-end", "40px" },
    { "figure", "margin-inline-start", "40px" },     { "figure", "margin-inline-end", "40px" },
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
    /* HTML §15.3.7 "Lists"' TWO UNCONDITIONAL INLINE-EDGE RULES, transcribed verbatim from that section:
         dd { margin-inline-start: 40px; }
         dir, menu, ol, ul { padding-inline-start: 40px; }
       EACH IS ONE ROW PER TAG AND NOT TWO, which is the difference between these and every `margin-block`
       above: css-logical-1 §4.2 "Flow-Relative Margins: the margin-block-start, margin-block-end,
       margin-inline-start, margin-inline-end properties and margin-block and margin-inline shorthands"' two
       edges belong to the SHORTHAND, and both rules here name a LONGHAND — the start edge alone — so the end
       edge keeps its initial `0` in a browser too.
       `dl` IS IN THE FIRST LIST AND NOT THE SECOND AND `dd` IS IN NEITHER, which is the section's own spelling
       and is the kind of thing a re-spelling loses: `dir, menu, ol, ul` is four tags where the section's
       display rule is seven, so a `<dl>` has NO padding and a `<dd>` has a margin instead.
       WHY THESE TWO LAND AND THE SECTION'S `margin-block` RULE DOES NOT is the per-rule answer the retired
       residual above got wrong as a per-section one, and the test is a companion rule: §15.3.7's three
       descendant-combinator rules govern `margin-block`, `list-style-type` and (in quirks mode)
       `list-style-position`, and not one of them names a `padding-inline-start` or reaches a `dd`. So a
       browser answers these two identically at every nesting depth and this table can too.
       WITHOUT THEM EVERY `<ul>` AND `<ol>` IN EVERY RENDERED DOCUMENT WAS FLUSH WITH ITS CONTAINING BLOCK —
       the list indent is 40px of `padding-inline-start` and nothing else, so a nav, a table of contents and a
       bulleted list all sat in the margin of the prose around them rather than inside it. */
    { "dd", "margin-inline-start", "40px" },
    { "dir", "padding-inline-start", "40px" }, { "menu", "padding-inline-start", "40px" },
    { "ol", "padding-inline-start", "40px" },  { "ul", "padding-inline-start", "40px" },
    /* HTML §15.3.8 Tables — all nine box types, because a `<tbody>` reading `inline` is not a table this engine
       cannot lay out, it is a box CSS 2 §9.2 says exists nowhere in a table. */
    { "table", "display", "table" }, { "caption", "display", "table-caption" },
    { "colgroup", "display", "table-column-group" }, { "col", "display", "table-column" },
    { "thead", "display", "table-header-group" }, { "tbody", "display", "table-row-group" },
    { "tfoot", "display", "table-footer-group" }, { "tr", "display", "table-row" },
    { "td", "display", "table-cell" }, { "th", "display", "table-cell" },
    /* HTML §15.3.8 "Tables"' CELL PADDING, which that section states verbatim as `td, th { padding: 1px; }`.
       IT IS THE ONE MARGIN-OR-PADDING RULE IN HTML §15 THIS TABLE HOLDS PHYSICALLY, and that is transcription
       rather than a decision: `padding` is the PHYSICAL shorthand, so css-cascade-5 §3 "Shorthand Properties"
       makes it set `padding-top`, `padding-right`, `padding-bottom` and `padding-left` — the flow-relative
       spelling would be this file re-writing the sheet. IT ALSO COSTS NOTHING TO BE PHYSICAL HERE, which is
       why the choice is free rather than merely faithful: all four sides carry ONE value, so every
       css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings" permutation of the four gives the same answer
       and no writing mode is being decided on core/layout's behalf.
       AND IT REFUTES A GENERALISATION `cssom_cascaded_value` BELOW USED TO MAKE — that "§15's rendering rules
       state every margin and padding LOGICALLY" and the physical spellings appear "only in the
       presentational-hint and quirks rules". This rule is in §15.3.8's MAIN block under neither qualifier.
       The DCHECK that sentence sits above is untouched by it and still holds: it asks whether BOTH members of
       one css-logical-1 §4 "Flow-Relative Box Model Properties" pair carry a UA row, and a cell has a physical
       padding row and no flow-relative one.
       CSS 2.1 §17.5 "Visual layout of table contents" IS WHAT MAKES THE ROW READABLE — "Cells have padding as
       well" — so core/layout/used_value.c's `uv_side` asks a cell for it under every border model but the
       collapsed table's, where §17.6.2 "The collapsing border model" removes the TABLE's padding and not the
       cell's. Without these eight rows every cell's text touched its own cell edge and its neighbour's, which
       is the one gap in a rendered table that no amount of border work closes. */
    { "td", "padding-top", "1px" },    { "td", "padding-right", "1px" },
    { "td", "padding-bottom", "1px" }, { "td", "padding-left", "1px" },
    { "th", "padding-top", "1px" },    { "th", "padding-right", "1px" },
    { "th", "padding-bottom", "1px" }, { "th", "padding-left", "1px" },
    /* HTML §15.3.8 Tables' TWO `vertical-align` RULES, which that section states verbatim as:
         thead, tbody, tfoot, table > tr { vertical-align: middle; }
         tr, td, th { vertical-align: inherit; }
       Every `<td>` in a parsed document is therefore `middle`, and it read `baseline` here — the initial value,
       which is a real value and not an absent one. core/layout/table_height.c's `th_cell_align` reads the
       alignment of every cell in a row, and CSS 2.1 §17.5.3 Table height algorithms measures a row holding two
       or more BASELINE cells through its own four-step alignment procedure, which exceeds the maximum cell box
       height whenever those cells' baselines sit at different distances from their tops. So an ordinary
       multi-cell row was measured through a procedure a browser applies to no cell in it.
       EACH RULE IS THREE ROWS BECAUSE `vertical-align` IS A SHORTHAND IN THIS CASCADE. css-inline-3 §4.2
       "Transverse Box Alignment: the vertical-align property" defines it as
       `[ first | last] || <'alignment-baseline'> || <'baseline-shift'>`, so `middle` is §4.2.2's
       `alignment-baseline` with §4.2.1's `auto` source and §4.2.3's `0` shift beside it — the two terms the
       rule omits, which css-cascade-5 §3 "Shorthand Properties" assigns their initial values. The shift's zero
       carries its unit for the reason core/css/css_shorthand.c's `VERTICAL_ALIGN_INITIAL` states at length:
       css-values-4 §6 "Distance Units: the <length> type" makes `0` and `0px` two spellings of one value ("for
       zero lengths the unit identifier is optional"), and every reader of this table produces the second.
       THE `inherit` RULE'S THREE ROWS ARE LOAD-BEARING IN A WAY THE OTHER RULE'S TWO RESET ROWS ARE NOT, because
       css-cascade-5 §3 gives a CSS-wide keyword on a shorthand the same reach — "it sets all of its
       sub-properties to that keyword, including any that are reset-only sub-properties" — and the SHIFT is the
       longhand that carries a row's own alignment down to its cells. HTML §15.3.8's `tr[valign=top i]`
       presentational hint is `vertical-align: top`, which §4.2.3 puts in `baseline-shift`, and `th_cell_align`
       reads the shift BEFORE the alignment baseline; a cell whose shift did not inherit would answer
       `baseline` for a row the markup aligned to its top.
       WHAT THESE ROWS DO NOT COVER IS `table > tr`, WHICH IS A COMBINATOR AND NOT A TAG. A `<tr>` whose parent
       is the `<table>` element takes the `tr` row here and inherits the table's own initial `baseline` where
       the first rule states `middle`; every `<tr>` inside a row group is `middle` already, so the divergence is
       confined to a tree the HTML parser does not build — HTML §13.2.6.4.9 The "in table" insertion mode
       answers a `"tr"` start tag by "Insert an HTML element for a "tbody" start tag token with no attributes,
       then switch the insertion mode to "in table body". Reprocess the current token." The next diff gives this
       layer core/dom/selector_match.h's matcher, which `cssd_author_collect` above already runs once per
       element per rule, so a UA rule becomes a SELECTOR and this table's `{tag, prop}` key becomes the
       degenerate shape of one — which is also what `cssd_ua_display_conditional` below stops being. Its absence
       shows as a script-built `table.appendChild(tr)` row of two or more cells whose §17.5.3 height is measured
       through the baseline procedure while a browser measures it as the tallest cell box. */
    { "thead", "baseline-source", "auto" }, { "thead", "alignment-baseline", "middle" },
    { "thead", "baseline-shift", "0px" },
    { "tbody", "baseline-source", "auto" }, { "tbody", "alignment-baseline", "middle" },
    { "tbody", "baseline-shift", "0px" },
    { "tfoot", "baseline-source", "auto" }, { "tfoot", "alignment-baseline", "middle" },
    { "tfoot", "baseline-shift", "0px" },
    { "tr", "baseline-source", "inherit" }, { "tr", "alignment-baseline", "inherit" },
    { "tr", "baseline-shift", "inherit" },
    { "td", "baseline-source", "inherit" }, { "td", "alignment-baseline", "inherit" },
    { "td", "baseline-shift", "inherit" },
    { "th", "baseline-source", "inherit" }, { "th", "alignment-baseline", "inherit" },
    { "th", "baseline-shift", "inherit" },
    /* HTML §15.3.11 "The hr element"' MARGINS, two of the six declarations that section states over the bare
       type selector `hr`:
         hr {
           color: gray;
           border-style: inset;
           border-width: 1px;
           margin-block: 0.5em;
           margin-inline: auto;
           overflow: hidden;
         }
       `auto` SURVIVES core/layout/used_value.c BECAUSE OF THE ROW THAT IS NOT HERE, which is worth stating
       because it looks like luck: an `<hr>` is `display: block` with no `width` row, so CSS 2.1 §10.3.3
       "Block-level, non-replaced elements in normal flow"' rule 5 applies — "if 'width' is set to 'auto', any
       other 'auto' values become '0'" — and both inline margins are 0, which is what a browser gives a
       full-width rule too. The arm that would abort is §10.3.7's, and only an ABSOLUTELY POSITIONED box
       reaches it.
       WHAT IS NOT COVERED — the section's `border-style: inset` and `border-width: 1px`, which are what a
       browser PAINTS an `<hr>` as, and they are held out by the RASTERIZER rather than by this table:
       core/paint/display_list_raster.c DFAILs by name on it, in that file's own words — "a `groove`,
       `ridge`, `inset` or `outset` border side reached the rasterizer, which draws only CSS 2.1 §8.5.3's
       `solid`" — so these two rows would abort every document containing an `<hr>` rather than draw one.
       WHAT THE NEXT DIFF BUILDS: a lightening and a darkening entry in core/css/css_color.h, which that
       crash names as the
       missing thing and which this file cannot supply. HOW ITS ABSENCE WOULD SHOW: an `<hr>` occupies the
       right amount of vertical space in a rendered document and paints no line in it, so a page divided by
       rules reads as one column of prose with gaps at the divisions.
       THE SPACE IS NOT THE LINE AND LANDING IT TAKES NOTHING AWAY — an `<hr>` had `margin: 0` and a 0 content
       height, so it occupied no space at all and separated nothing; it now separates its neighbours by the
       1em a browser gives them, and is still invisible. */
    { "hr", "margin-block-start", "0.5em" },  { "hr", "margin-block-end", "0.5em" },
    { "hr", "margin-inline-start", "auto" },  { "hr", "margin-inline-end", "auto" },
    /* HTML §15.3.10 Form controls, HTML §15.3.12 The fieldset and legend elements, and HTML §15.5's widget sections. An
       `<input>` is the single most measured element on the web and `input.clientWidth` was zero for every one
       of them. */
    { "input", "display", "inline-block" }, { "button", "display", "inline-block" },
    { "fieldset", "display", "block" },
    { "details", "display", "block" }, { "summary", "display", "block" },
    { "marquee", "display", "inline-block" },
    { "select", "display", "inline-block" }, { "textarea", "display", "inline-block" },
    { "option", "display", "block" }, { "optgroup", "display", "block" },
    /* HTML §15.3.12 "The fieldset and legend elements"' MARGINS AND PADDINGS, transcribed verbatim from the
       two rules that section states over bare type selectors:
         fieldset {
           display: block;
           margin-inline: 2px;
           border: groove 2px ThreeDFace;
           padding-block: 0.35em 0.625em;
           padding-inline: 0.75em;
           min-inline-size: min-content;
         }

         legend {
           padding-inline: 2px;
         }
       `padding-block: 0.35em 0.625em` IS THE TWO-VALUE FORM AND IS THE ONLY ONE IN THIS TABLE, so it is the
       one expansion the rules above did not exercise: css-logical-1 §4.4 "Flow-Relative Padding: the
       padding-block-start, padding-block-end, padding-inline-start, padding-inline-end properties and
       padding-block and padding-inline shorthands" says "The first value represents the start edge style, and
       the second value represents the end edge style", so the two edges take DIFFERENT values and the
       one-value sentence the margins above quote does not apply to it.
       WHAT IS NOT COVERED — TWO THINGS, and neither is a transcription this table declined to make.
         THE BORDER, `groove 2px ThreeDFace`, is held out at BOTH ends: core/paint/display_list_raster.c
         DFAILs on a `groove` side exactly as it does on the `<hr>`'s `inset` above, and
         core/css/css_system_color.c carries no `ThreeDFace` row, so the colour has nowhere to resolve either.
         WHAT THE NEXT DIFF BUILDS: that rasterizer's named colour derivation, and the `ThreeDFace` keyword
         beside the system colours already there — one of the deprecated ones CSS Color Module Level 4 §6.2
         "System Colors" sends to its own Appendix A.
         §15.3.12'S OWN USED-VALUE RULE, which is a LAYOUT rule and not a sheet one: the section says of a
         fieldset's box that "The used value of the 'padding-top', 'padding-right', 'padding-bottom', and
         'padding-left' properties are expected to be zero", the padding being applied instead by the
         ANONYMOUS FIELDSET CONTENT BOX, which the same section makes inherit `padding-bottom`, `padding-left`,
         `padding-right` and `padding-top` from the fieldset element. SO THE COMPUTED VALUE IS THIS TABLE'S AND
         THE USED VALUE IS core/layout's, and the row below is the thing that box would read rather than a
         stand-in for it — which is why it lands here rather than waiting: with no anonymous box the padding
         insets the content from the fieldset's own border edge, which is the picture a browser draws through
         a box this engine does not build. WHAT THE NEXT DIFF BUILDS: that anonymous content box in
         core/layout, taking the fieldset's computed padding and leaving the fieldset's own used padding at
         zero, together with §15.3.12's rendered legend. HOW ITS ABSENCE WOULD SHOW:
         `getComputedStyle(fieldset).paddingTop` answers the declared length where a browser answers `0px`,
         because CSSOM §9 "Resolved Values" makes a padding's resolved value the USED one; and `clientWidth`
         measures the fieldset's own padding edge rather than the anonymous box's.
       `min-inline-size: min-content` IS NOT A MARGIN OR A PADDING and is outside this row block's subject; it
       is css-sizing-3 §3.2 "Sizing Values: the <length-percentage [0,∞]>, auto | none, stretch,
       min-content, max-content, and fit-content values"' `min-content`, and it lands with the intrinsic
       sizing the `dialog` rule above waits on. */
    { "fieldset", "margin-inline-start", "2px" },     { "fieldset", "margin-inline-end", "2px" },
    { "fieldset", "padding-block-start", "0.35em" },  { "fieldset", "padding-block-end", "0.625em" },
    { "fieldset", "padding-inline-start", "0.75em" }, { "fieldset", "padding-inline-end", "0.75em" },
    { "legend", "padding-inline-start", "2px" },      { "legend", "padding-inline-end", "2px" },
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
    /* HTML §15.3.4 Phrasing content's THREE `font-size` RULES, which that section states as
         big { font-size: larger; }
         small { font-size: smaller; }
         sub, sup { line-height: normal; font-size: smaller; }
       and which are TYPE selectors, so unlike §15.3.6's they are rows. Each value is css-fonts-4 §2.5 (Font size:
       the font-size property)'s `<relative-size>`, whose arm core/css/css_computed_value.c already carries —
       `css_relative_size_px` off the PARENT's computed size, which is what makes `smaller` inside `smaller`
       compound the way a browser's does. The `line-height` half of the third rule is not here; the reason is
       with the six heading rules below, beside the other declarations of §15.3.4 this table does not carry.
       WITHOUT THESE THREE `<big>`, `<small>`, `<sub>` AND `<sup>` WERE LAID AND PAINTED AT BODY SIZE, which is
       the same defect as the headings' and is visible in the same picture. */
    { "big", "font-size", "larger" },   { "small", "font-size", "smaller" },
    { "sub", "font-size", "smaller" },  { "sup", "font-size", "smaller" },
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
/* An attribute selector's `i` FLAG, over a raw attribute value: Selectors 4 §6.3's ASCII case-insensitive match.
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

/* AN ATTRIBUTE SELECTOR'S `[attr]` IS A PRESENCE TEST, AND A PRESENCE TEST IS `lxb_dom_element_has_attribute`
   — NEVER a non-NULL answer from `lxb_dom_element_get_attribute`. The two are not two spellings of one
   question: lexbor's HTML tree construction sets an attribute's value only when the token carried one
   (`html/tree.c`'s `if (token_attr->value_begin != NULL)`), so a VALUELESS attribute — which is how every
   boolean content attribute is actually written — leaves `attr->value` NULL, and `lxb_dom_attr_value` answers
   NULL for that exactly as it answers NULL for an attribute that is not there. `<div hidden>` therefore did
   not match `[hidden]` while `<div hidden="">` did, one rule, two spellings, opposite boxes.
   THE REASON WAS ALREADY WRITTEN DOWN IN THIS TREE and the defect recurred anyway, which is the part worth
   keeping: core/layout/replaced_element.c draws this same split for §4.8.3's `alt` — "Attribute PRESENCE and
   attribute EMPTINESS are two questions … Lexbor's `get_attribute` answers NULL for both, which is why
   presence goes through `has_attribute`" — and it keeps a SECOND helper for the emptiness question rather
   than letting one predicate answer both. The wrong spelling is shorter and reads as if it means what you
   want, which is the whole of why it comes back; where a rule needs the VALUE as well (the `i`-flagged
   `[hidden=until-found i]` below), the presence is asked first and the value is read second. */
static const char *cssd_ua_hidden(lxb_dom_element_t *el, const lxb_char_t *tag, size_t taglen)
{
    size_t vlen = 0;
    const lxb_char_t *v;

    if (!lxb_dom_element_has_attribute(el, (const lxb_char_t *)"hidden", 6)) return NULL;
    v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"hidden", 6, &vlen);
    if (taglen == 5 && memcmp(tag, "embed", 5) == 0) return "inline";
    /* `until-found` sets content-visibility, and the box stays. `v` is NULL for a valueless `hidden`, which
       `cssd_attr_is_ascii_ci` answers FALSE for — a bare `hidden` is not `hidden="until-found"`. */
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
    /* `dialog:not([open])` is a PRESENCE test and takes `has_attribute` for the reason `cssd_ua_hidden` states:
       `<dialog open>` is the spelling every page writes and it carries no value, so asking `get_attribute` for
       it answered NULL and gave an OPEN dialog `display: none`. */
    if (taglen == 6 && memcmp(tag, "dialog", 6) == 0 &&
        !lxb_dom_element_has_attribute(el, (const lxb_char_t *)"open", 4))
        return "none";
    return NULL;
}

/* HTML §4.3.11.1 Heading levels & offsets' COMPUTED HEADING LEVEL, or 0 for an element that has none.
 * IT IS ONE FUNCTION BECAUSE THE SECTION IS ONE ALGORITHM AND §15.3.6 STATES TWO PROPERTIES OVER IT — the
 * font sizes and the block margins are two row sets keyed by the SAME level, and a second copy of this walk
 * beside the second table is the shape where one of them ends up reading `headingoffset` and the other not.
 * THE `9` HERE IS §4.3.11.1'S OWN LAST STEP and belongs to the algorithm rather than to either table: "If
 * level is greater than 9, then return 9". Each reader below asserts its own rows against it separately,
 * because a table added or shortened without the other moving is exactly the state this split makes
 * possible and the cap cannot see.
 *
 * HTML §4.3.11.1 Heading levels & offsets IS THE LEVEL, and it is not the digit in the tag name: "Increment
 * level by the result of getting an element's computed heading offset given element", where the offset is an
 * inclusive-ancestor walk accumulating `headingoffset` and stopping at `headingreset`. Selectors 5 §8 Heading
 * Structures says the same thing from the other side — "the heading level might be different from an element's
 * type selector. Thus, a selector h1:heading(3) matches any h1 tag which has an exposed heading level of 3" —
 * so a `{"h1", "font-size", "2.00em"}` ROW WOULD BE WRONG rather than narrow, answering `2.00em` for an `<h1>`
 * inside `<article headingoffset="1">` where the spec's own example says the level is 2. Selectors 5 has no
 * committed corpus here, so that citation is counted and never checked — see the census the audit prints.
 * THE WALK IS `css_parent_element` AND NOT A RAW PARENT because §4.3.11.1's own step is "If inclusiveAncestor's
 * parent is a shadow root, then set inclusiveAncestor to that shadow root's host and continue", which is
 * exactly what that entry does, and its next step — "Set inclusiveAncestor to inclusiveAncestor's parent
 * element" — is the NULL that entry answers for a Document parent.
 */
static unsigned cssd_ua_heading_level(lxb_dom_element_t *el, const lxb_char_t *tag, size_t taglen)
{
    unsigned level, offset = 0;
    lxb_dom_element_t *anc;

    /* §4.3.11.1's first six steps, verbatim: "If element's local name is h1, then set level to 1" and so on to
       h6, then "Assert: level is not 0". An element the six do not name has NO heading level, which is also
       what makes Selectors 5 §8's non-functional `:heading` exactly these six elements.
       `0` IS THAT ANSWER AND IT CANNOT COLLIDE WITH A LEVEL, which is why the readers below test it rather
       than carrying a separate found flag: §4.3.11.1 seeds level from one of the six tag names and then only
       ADDS an offset to it, and its own next step is "Assert: level is not 0" — so every level this function
       can return is at least 1 and 0 is free to mean `this element has no heading level`. */
    if (taglen != 2 || tag[0] != 'h' || tag[1] < '1' || tag[1] > '6') return 0;
    level = (unsigned)(tag[1] - '1') + 1u;
    /* §4.3.11.1's GET AN ELEMENT'S COMPUTED HEADING OFFSET. The accumulator SATURATES at 9 rather than being
       carried wide, which is not a cap on the walk: §4.3.11.1's next step is "If level is greater than 9, then
       return 9", and level is at least 1, so every offset at or above 9 names the same row. Saturating is what
       makes the addition total over an attribute whose value the rules put no upper bound on — the authoring
       requirement of "between 0 and 8, inclusive" is a conformance rule for authors and not a parse limit. */
    for (anc = el; anc != NULL; anc = css_parent_element(anc)) {
        size_t vlen = 0;
        const lxb_char_t *v;
        HtmlInteger num;

        /* "If inclusiveAncestor is an HTML element and has a headingoffset attribute" — the namespace test is
           the algorithm's own and is honoured, which is a different question from the `@namespace` on the UA
           RULES that this layer does not honour (see the table's lead-in). */
        if (lxb_dom_interface_node(anc)->ns != LXB_NS_HTML) continue;
        /* The VALUE is what is wanted here and not the presence, so this is `get_attribute` deliberately. A
           bare `<article headingoffset>` has the attribute with no value, whose DOM value is the empty string,
           and HTML §2.3.4.2 Non-negative integers' rules return an ERROR for that — so HTML §4.3.11.1's next step,
           "If the result of parsing the value is not an error, then set nextOffset to that value", leaves
           nextOffset at 0, which is the arm a NULL takes here. The two routes reach one answer; a NULL handed
           to the parser would not. */
        v = lxb_dom_element_get_attribute(anc, (const lxb_char_t *)"headingoffset", 13, &vlen);
        if (v != NULL && html_parse_non_negative_integer((const char *)v, vlen, &num)) {
            if (num.overflow || num.value >= 9 || offset + (unsigned)num.value >= 9) offset = 9;
            else offset += (unsigned)num.value;
        }
        /* "If inclusiveAncestor is an HTML element and has a headingreset attribute, then return offset" — a
           boolean attribute, so PRESENCE and not a non-NULL value, for the reason `cssd_ua_hidden` states. */
        if (lxb_dom_element_has_attribute(anc, (const lxb_char_t *)"headingreset", 12)) break;
    }
    level += offset;
    /* HTML §4.3.11.1 Heading levels & offsets' own last step, "If level is greater than 9, then return 9". */
    if (level > 9) level = 9;
    return level;
}

/* THE CAP AND A ROW SET ARE TWO SEPARATE NINES AND THIS IS WHERE EACH PAIR IS HELD TOGETHER. The cap is
   HTML §4.3.11.1 Heading levels & offsets' and lives in `cssd_ua_heading_level` above; a row set is
   HTML §15.3.6 Sections and headings' and carries its own length. A row added or dropped without the other
   moving is a read past the end of that table, which is a state an edit can reach and the only one it can —
   an assert over the LEVEL alone could not fail, because the seed is one of six and the cap is right above
   the return. THE SPLIT MADE THIS ASSERT LOAD-BEARING TWICE RATHER THAN ONCE: two tables now key on one
   level and each can come apart from the cap on its own, so the check is at each reader and is not hoisted
   into the level function, which has no row set to check against. The emptiness half is the same invariant
   `cssd_ua_table_check` asserts of every row of the type table: an empty string is a value the cascade
   would carry, not an absent declaration. */
#define CSSD_UA_HEADING_ROW(level, table, section)                                                          \
    DCHECK((level) - 1u < sizeof(table) / sizeof((table)[0]) &&                                             \
               (table)[(level) - 1u] != NULL && (table)[(level) - 1u][0] != '\0',                           \
           "HTML §15.3.6 (Sections and headings)'s " section " row set and HTML §4.3.11.1 (Heading levels & "  \
           "offsets)'s level range have come apart — a computed heading level selected past the last row "   \
           "transcribed here, or selected one with no value in it")

/* HTML §15.3.6 Sections and headings' SIX FONT-SIZE RULES, whose selector is `:heading(n)` — a pseudo-class and
 * therefore not a row of the type-name table, and NOT a selector this layer has to evaluate either, because
 * the LEVEL it selects on is an algorithm over the element alone. That is the same reason `[hidden]` and
 * `dialog:not([open])` are functions above rather than rows: what a `{tag, prop}` key cannot express and a
 * matcher is not needed for is a question asked of one element, which is exactly what these are.
 *
 * WHY THESE SIX AND NOT THE REST OF §15.3.6 AND §15.3.4, which is the whole of the judgement here and is the
 * test this file's own lead-in states — "The question to ask of a rendering-section declaration is therefore
 * not whether it is missing but whether anything READS the property, and where something does, the row is owed
 * rather than excused". `font-size` is read all the way to the rasterizer: core/css/css_computed_value.c's
 * `css_font_size_px` is what css-values-4 §6.1.1's advance measure scales a glyph's width by
 * (core/layout/text_run.c), what core/layout/line_box.c's used line height is derived from, and what
 * core/paint/box_paint.c writes into a glyph mark's `em` for core/paint/display_list_raster.c to size the
 * outline with. So a missing `font-size` row is not an absent declaration, it is every heading on every page
 * laid out and PAINTED at body size — which is what this engine did.
 *   §15.3.6's `:heading { font-weight: bold }` and §15.3.4's `b, strong { font-weight: bolder }`,
 *   `cite, dfn, em, i, var { font-style: italic }` and `code, kbd, samp, tt { font-family: monospace }` are
 *   DELIBERATELY ABSENT by that same test and not by oversight: `font-weight`, `font-style` and `font-family`
 *   are carried by the cascade and read by NOTHING — no layout entry, no paint entry, no computed-value entry
 *   names any of the three — so a row for one is a declaration with no reader, which is the write-with-no-reader
 *   shape and not a smaller transcription. They land in the diff that gives core/css/font_metrics.h a second
 *   face to select, because that is the consumer whose absence makes them inert.
 *   §15.3.4's `sub, sup { line-height: normal }` IS read (core/layout/line_box.c takes
 *   `css_used_line_height_px`) and is left for the next diff rather than excused: `normal` is that property's
 *   own initial value, so the row's whole effect is to STOP an inherited non-normal line height reaching a
 *   `<sub>`, which is a second property with a second value arm to verify and not a rider on this one.
 *   §15.3.3's MARGINS ARE NO LONGER ON THIS LIST — they are rows of the table above, and §15.3.6's own
 *   `margin-block` rules are the function directly below this one, which is what `margin-block` and
 *   `font-size` sharing a level and a selector makes possible. §15.3.7's are still absent and the reason is
 *   a different one, stated at the table.
 *
 */
static const char *cssd_ua_heading_font_size(lxb_dom_element_t *el, const lxb_char_t *tag, size_t taglen)
{
    /* §15.3.6, transcribed in the spec's own spelling. The index is the LEVEL minus one, so the table is a
       total function over §4.3.11.1's whole range: levels 1-5 are that section's five single-level rules and
       6-9 are its `:heading(6, 7, 8, 9)` rule, which exists precisely because an offset can push a level past
       the six tag names. LEVEL 4's `1.00em` IS THE IDENTITY of the inherited value and is transcribed anyway:
       it is a rule of the section, and a hole where a rule is would have to be re-derived by the next reader
       from the fact that `1em` and inheritance agree — a fact about this property, not about this table. */
    static const char *const HEADING_FONT_SIZE[9] = {
        "2.00em", "1.50em", "1.17em", "1.00em", "0.83em", "0.67em", "0.67em", "0.67em", "0.67em"
    };
    unsigned level = cssd_ua_heading_level(el, tag, taglen);

    if (level == 0) return NULL;
    CSSD_UA_HEADING_ROW(level, HEADING_FONT_SIZE, "font-size");
    return HEADING_FONT_SIZE[level - 1];
}

/* HTML §15.3.6 Sections and headings' SIX MARGIN RULES, which that section states beside the font sizes in
 * the same six declarations and in the same `:heading(n)` selector:
 *     :heading(1) { margin-block: 0.67em; font-size: 2.00em; }
 *     :heading(2) { margin-block: 0.83em; font-size: 1.50em; }
 *     :heading(3) { margin-block: 1.00em; font-size: 1.17em; }
 *     :heading(4) { margin-block: 1.33em; font-size: 1.00em; }
 *     :heading(5) { margin-block: 1.67em; font-size: 0.83em; }
 *     :heading(6, 7, 8, 9) { font-size: 0.67em; margin-block: 2.33em; }
 * SO THEY ARE A FUNCTION AND NOT ROWS FOR THE REASON THE FONT SIZES ARE, and the reason is worth not
 * re-deriving: the selector is a pseudo-class, which a `{tag, prop}` key cannot express — and the LEVEL it
 * selects on is HTML §4.3.11.1's algorithm over the element alone, so no matcher is needed either. A
 * `{"h1", "margin-block-start", "0.67em"}` ROW WOULD BE WRONG rather than narrow for exactly the reason
 * stated at the font sizes: an `<h1>` inside `<article headingoffset="1">` has level 2 and takes `0.83em`.
 * EACH RULE IS TWO LONGHANDS, by css-logical-1 §4.2's own sentence — "If only one value is given, it applies
 * to both the start and end edges" — and the two edges of one level always carry the same value, which is
 * why ONE row per level answers both and the caller asks which edge it wants.
 *   `margin-block` AND NOT `margin-top` IS WHAT MAKES THIS READABLE AT ALL: css-logical-1 §4's pairing is
 *   applied by `cssom_cascaded_value`, so the value returned here for `margin-block-start` is what
 *   `margin-top`'s computed value is cascaded from and core/layout/block_flow.c's `used_value_px(el,
 *   "margin-top")` therefore reads it.
 * WITHOUT THESE SIX EVERY HEADING ON EVERY PAGE SAT FLUSH AGAINST THE PROSE ABOVE AND BELOW IT — the same
 * defect as the font sizes', in the same picture, and visible in it even after the sizes landed. */
static const char *cssd_ua_heading_margin_block(lxb_dom_element_t *el, const lxb_char_t *tag, size_t taglen)
{
    static const char *const HEADING_MARGIN_BLOCK[9] = {
        "0.67em", "0.83em", "1.00em", "1.33em", "1.67em", "2.33em", "2.33em", "2.33em", "2.33em"
    };
    unsigned level = cssd_ua_heading_level(el, tag, taglen);

    if (level == 0) return NULL;
    CSSD_UA_HEADING_ROW(level, HEADING_MARGIN_BLOCK, "margin-block");
    return HEADING_MARGIN_BLOCK[level - 1];
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
    /* HTML §15.3.6's `:heading(n)` font sizes, asked ahead of the table for the same reason the `display`
       conditionals are: a pseudo-class rule and a type rule are both in the UA origin, so where both could
       match one element the order they are asked in IS the cascade between them. They cannot both match today
       — no `hN` carries a `font-size` row — and the order is the one css-cascade-5 §6.1's Specificity criterion
       gives (`:heading(n)` is a class, (0,1,0); `big` is a type, (0,0,1)), written down so it stays right when
       a type rule for one of the six arrives. NORMAL importance: §15.3.6 writes no `!important`, and the flag
       is left as the `false` set above rather than re-written, which is the same contract the `display` arm
       asserts one branch up. */
    if (strcmp(name, "font-size") == 0) {
        const char *fs = cssd_ua_heading_font_size(el, tag, n);

        if (fs) return fs;
    }
    /* HTML §15.3.6's `:heading(n)` BLOCK MARGINS, asked here for the same reason and answering the same level.
       BOTH EDGES TAKE ONE ROW because `margin-block: <one value>` gives them one value — css-logical-1 §4.2's
       "If only one value is given, it applies to both the start and end edges" — so the two names below are
       the two longhands of one declaration rather than two rules.
       THE INLINE EDGES ARE NOT HERE AND THAT IS §15.3.6 AND NOT AN OMISSION: the section writes `margin-block`
       and never `margin-inline`, so a heading's inline margins are its initial `0` in a browser too.
       THE SIX HEADING TAGS CARRY NO MARGIN ROW IN THE TABLE BELOW, so the order these two questions are asked
       in cannot decide anything today — and it is written the way css-cascade-5 §6.1's Specificity criterion
       would decide it if one arrived, exactly as the `font-size` arm above is: `:heading(n)` is a class,
       (0,1,0), and a type selector is (0,0,1). NORMAL importance, for the reason stated one branch up: §15.3.6
       writes no `!important`, and the flag is left as the `false` set above. */
    if (strcmp(name, "margin-block-start") == 0 || strcmp(name, "margin-block-end") == 0) {
        const char *mb = cssd_ua_heading_margin_block(el, tag, n);

        if (mb) return mb;
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
    /* CSS 2.1 §17 Tables' four, each straight off its own `Initial:` line: §17.4.1 Caption position and
       alignment gives `caption-side` `top`, §17.5.2 Table width algorithms: the 'table-layout' property gives
       `table-layout` `auto`, §17.6 Borders gives `border-collapse` `separate`, and §17.6.1 The separated
       borders model gives `border-spacing` `0`.
       THE ROWS ARE WHAT MAKE THESE PROPERTIES ANSWERABLE AT ALL, exactly as `transform`'s does one line up and
       for the identical reason: lexbor's registry carries none of the four, so with no row here css-cascade-5
       §7.1 has no initial value to fall to and the cascade answers NULL for every element that does not
       declare one — which is every element on almost every page. That is not `separate`, and a §17.6 consumer
       asking which of the two border models a table is in would get an answer it could not read. THREE OF THE
       FOUR ARE ALSO INHERITED (core/css/css_defaulting.c carries `caption-side`, `border-collapse` and
       `border-spacing` on their `Inherited: yes` lines; `table-layout`'s line is `Inherited: no`), and §7.2
       answers the ROOT element with the initial value — so without these rows the inherited three had no base
       case either and the whole chain answered NULL from the root down.
       `border-spacing`'s `0` IS A LENGTH WITH NO UNIT AND THAT IS THE SPEC'S OWN TEXT — css-values-4 §6
       "Distance Units: the <length> type" states "For zero lengths the unit identifier is optional" — and
       core/css/css_computed_value.h's entry is what turns it into the two absolute lengths §17.6.1's
       `Computed value:` line asks for. */
    { "caption-side", "top" }, { "table-layout", "auto" },
    { "border-collapse", "separate" }, { "border-spacing", "0" },
    /* css-align-3 §7.1 "Inline-Axis (or Main-Axis) Default Alignment: the justify-items property" gives
       `justify-items` an `Initial:` of `legacy`, and css-align-3 §6.1 "Inline-Axis (or Main-Axis)
       Self-Alignment: the justify-self property" gives `justify-self` an `Initial:` of `auto`. THE ROWS ARE WHAT MAKE THE TWO
       ANSWERABLE AT ALL, for the same reason CSS 2.1 §17's four above need theirs: lexbor's registry carries
       neither, so with no row here css-cascade-5 §7.1 has no initial value to fall to and the cascade answers
       NULL for every element that does not declare one — which is every element on almost every page. Both
       `Inherited:` lines are `no`, so unlike three of §17's four neither belongs in core/css/css_defaulting.c
       and css-cascade-5 §7.2 needs no base case for them.
       THEY ARRIVE WITH core/css/css_shorthand.c's `place-items` AND `place-self` ROWS, which are what sets
       either property: an initial value with no way to declare one is a row for a property no page can reach,
       and the shorthand's expansion is the reach. `legacy` IS an initial value a page never writes and the
       grammar still admits — css-align-3 §7.1's `Value:` line carries it as its own term — so it is a declarable keyword
       here and not a sentinel. */
    { "justify-items", "legacy" }, { "justify-self", "auto" },
    /* css-ui-4 §6.2 "Exclusion from Hit-testing: the pointer-events property", whose `Initial:` line is
       `auto`. THE ROW IS WHAT MAKES "HIT-TESTABLE" A COMPUTED VALUE RATHER THAN A SILENCE, which is the
       `transform` argument above word for word: lexbor's registry carries no `pointer-events` entry — 107
       properties and this is not one of them — so with no row here css-cascade-5 §7.1 has no initial value to
       fall to and the cascade answers NULL for every element that does not declare one, which is every
       element on almost every page. A DECLARED `pointer-events` reaches the cascade already, by the same
       `__CUSTOM` route the `transform` row names, so this row completes the pair rather than standing in for
       it.
       ITS `Inherited:` LINE IS `yes` AND core/css/css_defaulting.c ALREADY CARRIES IT, so unlike
       `justify-items` above this one needs no companion row there — css-cascade-5 §7.2's base case is this value, and the
       row that supplies it is the one being added here. The two tables were already half agreed: the
       inheritance half has listed this property since before anything could ask for it, which is why the
       absence read as a modelled property rather than as an unmodelled one. */
    { "pointer-events", "auto" },
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
    /* THE INITIAL VALUE NO PARSER CAN CARRY, because the property's own line does not state one:
       css-fonts-4 §2.1 "Font family: the font-family property" gives `Initial:` as "depends on user agent".
       THAT IS WHY THE REGISTRY'S POINTER IS NULL HERE, and it is the ONLY one that is — of lexbor's property
       table exactly one entry has a null initial and it is this property. The derivation rather than the
       count, because the table moves with the vendored parser. Over
       engine/lexbor/source/lexbor/css/property/res.h, the null-initial rows are
           `grep -cE '^\s*NULL\},$'`            -> 1
       and the entries they are drawn from are
           `grep -cE '^\s*\{\(lxb_char_t'`     -> 103
       Both count LINES, over a file that puts each of those on one of its own, so a line count and an
       occurrence count are the same number here. So the silence is CORRECT and is not an omission to be filed
       in either table above. CSSD_INITIAL_UNREGISTERED is for a property
       lexbor does not carry AT ALL and asserts `e == NULL`; CSSD_INITIAL_WRONG is for one whose answer lexbor
       GIVES and this file disagrees with, and asserts `e->initial != NULL`. This property is registered AND
       unanswered, which is a third state neither assertion admits — and that is not an accident of the
       parser, it is the spec declining to state a value that belongs to the user agent.

       ANSWERING NULL IS NOT "NOT SET", AND THE DEFECT IS THE ONE THE BACKGROUND LONGHANDS BELOW ALREADY
       RECORD. core/css/css_computed_value.c's `css_resolved_computed` reads a NULL from this entry as
       CSSOM §6.6.1 "The CSSStyleDeclaration Interface"'s answer for a property that is not set — true of a
       custom property nobody registered, false of a longhand with an `Initial:` line — so
       `getComputedStyle(el).fontFamily` answered the EMPTY STRING. css-fonts-4 §2.1 gives `Inherited:` as
       `yes` and core/css/css_defaulting.c carries it, so CSS Cascade 5 §7.2 "Inheritance" had no base case
       either and the empty answer came from the root down: every element of every page that declares no
       family, which is almost every element of almost every page. HTML's rendering section does not close the
       gap — it sets `font-family` on `listing, plaintext, pre, xmp` and on `code, kbd, samp, tt` and on no
       ancestor of theirs — so the root's family is this line and nothing else.

       THE VALUE IS A GENERIC KEYWORD AND NOT A NAME, BECAUSE THE SHIPPED FACE HAS NO NAME. css-fonts-4 §2.1
       gives `Computed value:` as "list, each item a string and/or <generic-font-family> keywords", so a
       generic is a first-class item of that list rather than a stand-in for a real one; and the face this
       user agent defaults to is a metrics-only sfnt whose 'name' table engine/fontsubset.mjs drops, so there
       is no family name in core/fonts/default_font_data.c to report and a concrete string here would name a
       face nothing in this engine can produce or match. WHICH GENERIC IS A FACT ABOUT THAT FACE and is
       checkable from the generated file rather than chosen: its header names the bytes it copied, which are
       DejaVu Sans, so the keyword that describes it is `sans-serif`. That is one of css-fonts-4 §2.1.2
       "Syntax of <generic-font-family>"'s `<generic-font-complete>` arm, which that section's own example
       calls "a universal generic font, which is guaranteed to match on all systems" — and that guarantee is
       what makes it the one answer css-fonts-4 §5 "Font Matching Algorithm" cannot fail to resolve once it
       exists, since that section's terminal arm is this user agent's default font and this keyword names
       exactly the face that is.

       NAMED RESIDUAL — THE VALUE IS A PICKED ENVIRONMENT FACT AND CROSSES TO THE PAGE AS A BARE STRING.
       core/frame/viewport.h's test is whether the model PICKED one point out of a range the environment
       leaves free, and this is such a point by the argument core/css/font_size_functions.h already makes for
       `medium`: nothing in this engine determines it, the property's own line says it depends on the user
       agent, and a page reads it back.
         WHAT IS NOT COVERED: the string carries no domain, so the read is decided rather than forked. Every
         fact-carrying computed value in this engine is a LENGTH — `viewport_env_derived` takes a `CssPx`, and
         core/css/css_length.h says that struct's `env`/`realm` pair is written by `css_px_env` and by nothing
         else — so there is no seam a STRING may cross carrying a fact, and this arm cannot mint what it has
         no way to hand over.
         WHAT THE NEXT DIFF BUILDS: a member of core/css/css_length.h's fact vocabulary for this choice, and a
         seam in core/frame/viewport.h that wraps a STRING against a fact set — the same widening
         core/css/css_computed_value.c's `border-spacing` arm asks for on the other axis, where one string is
         a function of two lengths' facts and the seam takes one set.
         HOW ITS ABSENCE WOULD SHOW: a page that branches on the family it is given takes ONE arm, so that
         read never appears in the fork census, while a page branching on the reported default font size does
         appear there — two reads of one family of picked user-agent facts, one forked and one decided. */
    if (strcmp(name, "font-family") == 0) {
        char *out;

        DCHECK(e != NULL && e->initial == NULL,
               "lexbor's property registry has changed its mind about `font-family`'s initial value, which is "
               "this arm's whole expiry condition and the only thing that can make it wrong. NO ENTRY AT ALL "
               "means the property has left the registry, so the row belongs in CSSD_INITIAL_UNREGISTERED "
               "beside the other properties lexbor does not carry, whose assertion is the one it would then "
               "pass. AN ENTRY CARRYING AN INITIAL VALUE means a vendored parser is now stating a value "
               "css-fonts-4 §2.1's own `Initial:` line makes the USER AGENT's to state: read what it says, and "
               "if it is to be overridden rather than adopted the row belongs in CSSD_INITIAL_WRONG, which is "
               "the table for disagreeing with an answer the registry gives and which records the answer it is "
               "disagreeing with so the disagreement expires too");
        out = strdup("sans-serif");
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
    /* css-backgrounds-3 §2.10 "Backgrounds Shorthand: the background property"'s LONGHANDS, ASKED OF THE
       COMPONENT THAT OWNS THEM rather than listed in the table above — the same seam `cssd_property_name_of`
       below already uses for their NAMES, and for the same reason: core/css/css_background_shorthand.c states
       each `Initial:` line once, beside the serializer that omits against it, so a row here would be one fact
       with two sources and the copy that drifts is the one no serializer reads.
       THE TABLE ABOVE USED TO CARRY THE ARGUMENT FOR LEAVING THEM OUT — `an initial value is a fact a property
       has whether or not anything asks for it, and nothing asks these for one` — and that was a claim about
       THIS TREE which `cssd_own_init` had already made false: it adds all eight to the engine's own supported
       set, so §6.6.1 installs `backgroundImage` and the rest as IDL attributes and a PAGE asks. What it got was
       not a refusal but the EMPTY STRING, because `css_resolved_computed`'s last line reads a NULL here as
       §6.6.1's answer for a property that is not set — true of a custom property nobody registered, false of a
       longhand whose own `Initial:` line says `none`. So `getComputedStyle(el).backgroundImage` answered `""`
       where every user agent answers `none`, and a page branching on that took the arm for an element that HAS
       a background image. NO IMAGE MACHINERY IS INVOLVED IN FIXING IT: the initial value is a KEYWORD, and the
       DECLARED `url()` path is untouched — css_background_shorthand.c has validated and serialized those all
       along. This closes the UNDECLARED half only.
       `background-color` NEVER REACHES HERE: lexbor types it and the registry is asked above, which is why the
       assert is the same one the loop above makes. It fires the day lexbor gains a row for one of the other
       seven, which is exactly when this call would become the second answer to a settled question. */
    {
        const char *bg = css_background_shorthand_initial(name);

        if (bg != NULL) {
            char *out;

            DCHECK(e == NULL,
                   "a css-backgrounds-3 §2.10 longhand this engine states the initial value of is ALSO in "
                   "lexbor's property registry — one fact with two answers, and the registry's is the one "
                   "every other property in CSS reads. The registry is asked FIRST above, so reaching this "
                   "line means lexbor has the row and no initial value on it: DELETE the value from "
                   "core/css/css_background_shorthand.c's BG_INITIAL and let the registry answer, after "
                   "checking that what it now says is the property's own `Initial:` line");
            out = strdup(bg);
            CHECK(out != NULL, "cssom: OOM copying an initial value — a dropped one reads as no value at all, "
                               "which is a cascade that stopped before its last layer");
            return out;
        }
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
    const char *partner, *ua, *pua;
    uint32_t seq = 0;
    bool ua_important = false, pua_important = false;
    char *v, *pv, *out;

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
    /* THE RENDER'S RECORD OF THIS EXACT QUESTION, ASKED BEFORE ANY OF THE WORK BELOW AND BEFORE
       css-logical-1 §4's PAIRING — which is the whole point of asking here rather than in any caller. The
       pairing is a PREREQUISITE of this cascade and is itself two computed values that INHERIT, so an ask
       answered from the record skips two climbs to the root before it skips one sheet walk. See
       core/css/css_cascade_pass.h for the two shapes that multiply, for the measurement, and for the three
       places the span this record is sound over is asserted. A NULL answer is one of the values it carries
       (css-cascade-5 §4.2 "Cascaded Values"' empty list), so a hit is the BOOLEAN and never the
       pointer. */
    if (css_cascade_pass_ask(el, name, &out)) return out;
    /* EVERY ORIGIN CONTRIBUTES INTO ONE LIST, AND THE SORT DECIDES — which is css-cascade-5 §6.1 and is not
       what asking each
       origin in turn does. The four used to be asked in precedence order and the first that answered won, and
       that is a DIFFERENT ordering: it hoists css-cascade-5 §6.1's Element-Attached criterion above its
       Origin-and-Importance
       one, so `<p style="color:blue">` beat `p { color: red !important }`, which css-cascade-5 §6.1 and
       css-cascade-5 §6.3 both say it
       loses to ("an important declaration takes precedence over a normal declaration", and Element-Attached is
       the criterion BELOW Origin and Importance, reached only when it ties). */
    /* @LOGICAL — css-logical-1 §4 "Flow-Relative Box Model Properties": "Within each logical property group,
       corresponding flow-relative and physical properties are paired using the element's own computed writing
       mode. Although the specified value of each property remains distinct, PAIRED PROPERTIES SHARE A COMPUTED
       VALUE. This shared value is determined by CASCADING THE DECLARATIONS OF BOTH PROPERTIES TOGETHER AS ONE."
       So the pair is ONE cascade and not two resolutions compared afterwards, and every layer below collects
       both members — which is why the partner is read here, once, rather than at each layer.
       IT IS RESOLVED BEFORE ANY DECLARATION IS COLLECTED because §4 says the mapping is a prerequisite: "It
       also requires that writing-mode, direction, and text-orientation be computed as a prerequisite for
       cascading together the flow-relative and physical declarations of a logical property group to find their
       computed values." Those three are in NO group — core/css/css_logical.c asserts exactly that — so the
       nested resolution this line starts terminates at depth one rather than re-entering itself.
       NULL IS THE ANSWER FOR MOST PROPERTIES and it is a real one: a property in no group has no second
       declaration list, and every collector below then behaves exactly as it did. */
    partner = css_logical_partner_of(el, name);
    DCHECK(partner == NULL || strcmp(partner, name) != 0,
           "css-logical-1 §4's pairing answered the property it was asked about. The pair's two members have "
           "DIFFERENT mapping logic — one physical, one flow-relative — so a property is never its own "
           "partner, and collecting it twice would put two identical declarations at one position in "
           "css-cascade-5 §6.1's order");
    order = css_layer_order_create();
    cascade = css_cascade_create(order);
    /* @LOGICAL — THE BRACKET THE NESTED RESOLUTION ABOVE MADE NECESSARY. The partner is resolved BEFORE this
       line and that ordering is what keeps the two sheet walks apart: the inner one has destroyed its lexbor
       arena before the outer one sets its own, so `g_parser` has one owner at a time. Moving the partner read
       below this call would nest them and the inner walk would destroy the arena the outer walk is parsing
       into — a use-after-free whose symptom is nowhere near the edit, which is why the resource asserts it
       rather than a comment describing the ordering. */
    DCHECK(!g_collecting_sheets,
           "the AUTHOR-ORIGIN sheet walk was re-entered. It owns `g_parser` and the per-sheet parse table "
           "the arenas live in, and an inner walk whose sheet emits DIFFERENT text for a slot the outer "
           "walk is reading REPLACES that slot and destroys its arena — so the outer walk would go on "
           "reading a stylesheet, its rules and its selectors out of freed memory. The only "
           "nested resolution on this path is css-logical-1 §4's PREREQUISITE — the computed `writing-mode` "
           "and `direction` the pairing above is derived from — and it is ordered ahead of this call for "
           "exactly this reason; a second nested read added below it is what this crash names");
    g_collecting_sheets = true;
    cssd_author_collect(el, name, partner, cascade, order, &seq);
    g_collecting_sheets = false;
    /* css-cascade-5 §6.1's ELEMENT-ATTACHED STYLES: "declarations that are attached directly to an element (such as the
       contents of a style attribute) rather than indirectly mapped by means of a style rule selector take
       precedence over declarations the same importance that are mapped via style rule." It is in the AUTHOR
       origin ([CSSSTYLEATTR]) and in no explicit cascade layer, and css-cascade-5 §6.1's Order of Appearance places it after
       every style sheet ("declarations from style attributes ... are all placed after any style sheets"),
       which is what the counter reaching here already is. */
    cssd_inline_collect(el, name, partner, cascade, order, &seq);
    /* css-cascade-5 §6.5's AUTHOR PRESENTATIONAL HINT ORIGIN, "between the regular user origin and the author
       origin". It is an ORIGIN and not a row of the UA table below because that is where css-cascade-5 §6.5 puts it: an
       author rule of any specificity outranks a hint, and a hint outranks the UA sheet. */
    v = css_presentational_hint(el, name);
    pv = partner ? css_presentational_hint(el, partner) : NULL;
    /* @LOGICAL — AT MOST ONE MEMBER OF A PAIR IS HINTED, and that is a fact about HTML §15.4 "Replaced
       elements" and §15.5's attribute mappings rather than a convenience: every one of them is stated
       PHYSICALLY ("The hspace attribute of embed, img, or object elements ... maps to the dimension
       properties 'margin-left' and 'margin-right'"), so the flow-relative member of any pair is hinted by
       nothing. The hint origin is ONE css-cascade-5 §6.1 position, so two hints would need an order between
       them that a per-property table has no column for — hence the crash rather than a pick. */
    DCHECK(v == NULL || pv == NULL,
           "BOTH members of a css-logical-1 §4 logical property group carry a css-cascade-5 §6.5 "
           "presentational hint on one element. HTML §15's attribute mappings are all stated over PHYSICAL "
           "properties, so a flow-relative hint is a row somebody added — and the hint origin is a single "
           "position in css-cascade-5 §6.1's Order of Appearance, with no column saying which of two hints "
           "the UA sheet writes first. Give core/css/css_presentational_hints.h an ORDER over its rows and "
           "pass it as the declaration position, exactly as the author walk passes a block index");
    ++seq;
    if (v) {
        css_cascade_add(cascade, CSS_ORIGIN_PRESENTATIONAL_HINT, false, false, NULL, 0, seq, 0, v);
        free(v);
    }
    if (pv) {
        css_cascade_add(cascade, CSS_ORIGIN_PRESENTATIONAL_HINT, false, false, NULL, 0, seq, 1, pv);
        free(pv);
    }
    /* css-cascade-5 §6.2's USER-AGENT ORIGIN, with css-cascade-5 §6.3's IMPORTANCE carried rather than assumed
       normal: two of HTML §15.3.1's
       `display` rules are `!important`, and "important user agent declarations" is the TOP band of
       css-cascade-5 §6.3's list
       — above important author declarations — so a flag dropped here is a page's own rule silently giving a
       box to an element the UA sheet says has none. */
    ua = cssd_ua_value(el, name, &ua_important);
    pua = partner ? cssd_ua_value(el, partner, &pua_important) : NULL;
    /* @LOGICAL — AT MOST ONE MEMBER OF A PAIR HAS A UA ROW, which is checked rather than assumed, and the
       REASON THAT USED TO BE GIVEN FOR IT IS WITHDRAWN. It read: §15's rendering rules state every margin and
       padding LOGICALLY — HTML §15.3.3 "Flow content" is `blockquote, figure, listing, p, plaintext, pre, xmp
       { margin-block: 1em }` and `blockquote, figure { margin-inline: 40px }` — while the physical spellings
       in that section appear only in the presentational-hint and quirks rules. THE EXAMPLE IS EXACT AND THE
       GENERALISATION IS FALSE: HTML §15.3.8 "Tables"' main block states `td, th { padding: 1px }`, the
       PHYSICAL shorthand, under neither qualifier, and HTML §15.3.3's own `dialog { margin: auto; padding:
       1em }` does the same. It is rewritten rather than deleted because a reader who re-derives it from the
       §15.3.3 rows will re-derive it the same way and will then read a physical row below as a mistake.
       WHAT HOLDS IS THE INVARIANT AND NOT THE RULE OF THUMB: a section states each of its margins and
       paddings in exactly ONE spelling, so no (element, group) has both members declared in the UA origin —
       which is what this DCHECK asks, and which a physical row satisfies as readily as a flow-relative one.
       The table below has no column for the order between two rows of one pair if one ever did. */
    DCHECK(ua == NULL || pua == NULL,
           "BOTH members of a css-logical-1 §4 logical property group have a USER-AGENT declaration on one "
           "element. HTML §15's rendering rules state each of its margins and paddings in exactly one "
           "spelling, so this is two rows added for one pair — and the UA table is a flat {tag, property, "
           "value} scan whose row ORDER is the sheet's, which nothing here reports, so css-cascade-5 §6.1's "
           "Order of Appearance between them is unavailable. Report the row index from cssd_ua_value and pass "
           "it as the declaration position, exactly as the author walk passes a block index");
    ++seq;
    if (ua) css_cascade_add(cascade, CSS_ORIGIN_UA, ua_important, false, NULL, 0, seq, 0, ua);
    if (pua) css_cascade_add(cascade, CSS_ORIGIN_UA, pua_important, false, NULL, 0, seq, 1, pua);
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
    /* …AND THE ANSWER, REPORTED WHETHER OR NOT A PASS IS OPEN. The record's census counts the QUESTION and
       the RESOLUTION at the same event, so a run that renders nothing still closes its own identity; what an
       open pass decides is the STORAGE. `out` is BORROWED here — this caller still owns it and its one
       caller still frees it. */
    css_cascade_pass_record(el, name, out);
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

/* ---- §6.6's DECLARATIONS AS VALUES: the block's own store, over the text the two backings keep -------------
 *
 * THE BACKING ABOVE IS A SERIALIZATION, AND A SERIALIZATION IS AN OBSERVATION ALGORITHM. §6.6's serialize a
 * CSS declaration block answers what `cssText` and `getAttribute('style')` SHOW, so every rule that makes it
 * hide something makes a write that stores its output DESTROY that something — the two coincide for a
 * declaration whose value is the page's own bytes and come apart for a value no page can spell.
 *
 * THAT IS NOT A DEFECT IN THE SERIALIZER AND FIXING IT THERE IS SPEC-WRONG, which is worth stating because it
 * is the repair a reader reaches for first — emit the shorthand for the longhands that still share one
 * pending value and the overriding longhand after it, and the text round-trips. MEASURED by fetching the
 * current editor's draft of CSSOM §6.6 "CSS Declaration Blocks": the block algorithm's DECLARATION arm has no
 * skip-if-empty step at all — it serializes the declaration and appends the result to the output list
 * unconditionally, so a longhand whose value serializes to nothing is written as `name: ;`. (THE TITLE HERE
 * READ "Serializing CSS Values" AND THAT IS §6.7.2, caught by `citegen.mjs` on the first run against this
 * banner; the number was right and the title named a different section, which is the pair a reader cannot
 * check by eye and the instrument can.) And the SHORTHAND arm may not rescue it either: css-values-5's
 * "Substitution in Shorthand
 * Properties" requires a shorthand with a MIXED set of longhands to serialize as the empty string, which
 * §6.6's shorthand loop reads as "continue". Both rules are quoted in full, once, at
 * core/css/css_pending_substitution.h, which also records why an audit reports them against the wrong
 * standard; they are not re-quoted here, because N copies of one spec sentence are N chances to be stale.
 * So `margin-right: ;` is exactly what a browser writes, and the very next parse of it drops the declaration
 * whole. The standard is obeyed and the STORAGE is what must stop being that string.
 *
 * SO THE DECLARATIONS ARE KEPT AS VALUES, AND THE TEXT IS THE PROJECTION. A write serializes as before and
 * writes the backing as before — §6.6's "update style attribute" is unchanged, so `getAttribute('style')`,
 * `cssText` and the cascade's element-attached layer read byte-identically what they read before — and it
 * additionally files the declarations THEMSELVES beside the bytes it wrote.
 *
 * IT IS A JS ARRAY ON THE BLOCK'S OWN RECORD, for the reason `cssd_taint_set`'s record states in full: an
 * ordinary property write is what the per-flow COW delta captures, so the store forks, parks and resumes for
 * free, and a malloc'd C list beside it would be state the delta does not swap.
 *
 * THE STORE IS VALIDATED BY CONTENT AND NEEDS NO INVALIDATION CALL ANYWHERE, which is the same property and
 * the same argument the unknown record below is built on. It files the PROJECTION it wrote beside the values,
 * and a read takes the values only while the backing still holds exactly those bytes. A `setAttribute('style',
 * …)`, a `cssText =` through another path, a rule's own text written elsewhere, a COW rewind that restores the
 * attribute — every one of them leaves a text the projection does not match, and the read then parses the text
 * exactly as it did before this store existed. There is no write that can leave a WRONG answer behind.
 *
 * THAT QUESTION IS ROUTING AND NOT A FALLBACK, by §C-stack's own test: delete the values store and the text
 * parse is STILL the only way to answer a block whose text the page wrote directly, because the page's own
 * `setAttribute` is a declaration list this component never saw. What a fallback would look like is a second
 * ANSWER to a question this store can answer, and there is none: where the projection matches, the text is not
 * consulted at all.
 *
 * WHAT IT DOES NOT REACH, NAMED. The cascade's element-attached layer (`cssd_inline_collect`) reads the
 * element's `style` attribute directly and has no block in hand, so a declaration this store holds and the
 * projection hides is invisible to the cascade. That population is EMPTY for every value a page can spell and
 * is exactly the population §6.6.1's no-example write will create. WHAT THE NEXT DIFF BUILDS: the fork and its
 * record entry (`cssd_value`'s ordered (2) and (3)), after which the cascade's inline collector must ask the
 * BLOCK rather than the attribute for an element that has one. HOW ITS ABSENCE WOULD SHOW, as an observation:
 * a block that declares a property through `el.style` and a `getComputedStyle` of the same element that does
 * not, on one element, with no write to the attribute in between. */
#define CSSD_DECLS_FIELD      "declarations"
/* The bytes the write that filed those declarations left in the backing — the whole of the staleness test. */
#define CSSD_DECLS_TEXT_FIELD "declarationsProjection"

/* FILE the declarations `proj` was the serialization of, or CLEAR the store when `proj` is NULL. Clearing is a
   positive statement and not an omission: a write whose bytes the backing did not keep verbatim has left a
   block this store cannot describe, and a stale array left behind would be read by the next matching
   projection. */
static void cssd_decls_store(JSContext *ctx, JSValueConst block, const CssDecls *d, const char *proj)
{
    JSValue arr;
    unsigned i;

    if (!proj) {
        JS_SetPropertyStr(ctx, block, CSSD_DECLS_FIELD, JS_UNDEFINED);
        JS_SetPropertyStr(ctx, block, CSSD_DECLS_TEXT_FIELD, JS_UNDEFINED);
        return;
    }
    arr = JS_NewArray(ctx);
    CHECK(JS_IsObject(arr),
          "cssom: the declaration block's value store could not be allocated — a dropped one would silently "
          "return the block to a backing that cannot hold a value no page can spell");
    /* THREE ENTRIES PER DECLARATION AND NOT AN OBJECT PER DECLARATION, because the three are ONE fact written
       in ONE step — the same reason the unknown record below writes its two in one call — and because a
       declaration's VALUE is the field that must be able to be absent: `JS_NULL` is a declaration the block
       holds and has no bytes for, which is the capability the text backing did not have. */
    for (i = 0; i < d->n; i++) {
        JS_SetPropertyUint32(ctx, arr, i * 3u,      JS_NewString(ctx, d->v[i].name));
        JS_SetPropertyUint32(ctx, arr, i * 3u + 1u,
                             d->v[i].value ? JS_NewString(ctx, d->v[i].value) : JS_NULL);
        JS_SetPropertyUint32(ctx, arr, i * 3u + 2u, JS_NewBool(ctx, d->v[i].important));
    }
    JS_SetPropertyStr(ctx, block, CSSD_DECLS_FIELD, arr);
    JS_SetPropertyStr(ctx, block, CSSD_DECLS_TEXT_FIELD, JS_NewString(ctx, proj));
}

/* THE STORE, WHEN THE BACKING STILL HOLDS THE BYTES IT WAS FILED BESIDE. FALSE means the caller parses the
   text, which is every block this component has not written and every block somebody else has. */
static bool cssd_decls_load(JSContext *ctx, JSValueConst block, const char *cur, size_t curlen, CssDecls *out)
{
    JSValue arr, proj;
    const char *held;
    size_t heldlen = 0;
    uint32_t n = 0, i;
    bool same, shaped = true;

    DCHECK(out->n == 0,
           "the declaration block's value store was decoded into a list that already holds declarations — the "
           "store IS the block's declarations, so appending them to somebody else's would file one block's "
           "under another's and the round-trip check below would be asked about neither");
    proj = JS_GetPropertyStr(ctx, block, CSSD_DECLS_TEXT_FIELD);
    if (!JS_IsString(proj)) { JS_FreeValue(ctx, proj); return false; }
    held = JS_ToCStringLen(ctx, &heldlen, proj);
    JS_FreeValue(ctx, proj);
    if (!held) return false;
    /* BYTE-FOR-BYTE AND NEVER A LENGTH OR A PREFIX. The two strings are a serialization this component wrote
       and whatever the backing answers now, and the only thing that makes the store safe is that nothing but
       that exact write can produce the first. */
    same = heldlen == curlen && (curlen == 0 || memcmp(held, cur ? cur : "", curlen) == 0);
    JS_FreeCString(ctx, held);
    if (!same) return false;
    arr = JS_GetPropertyStr(ctx, block, CSSD_DECLS_FIELD);
    if (!JS_IsArray(arr)) { JS_FreeValue(ctx, arr); return false; }
    {
        JSValue len = JS_GetPropertyStr(ctx, arr, "length");

        JS_ToUint32(ctx, &n, len);
        JS_FreeValue(ctx, len);
    }
    /* THE SHAPE IS VALIDATED BEFORE ONE DECLARATION IS TAKEN, and a store that fails it is REFUSED WHOLE
       rather than read past. Refusing is a real answer and not a hole: the caller parses the backing's text,
       which is exactly what every block without a store does, so the release arm of this assert is a DEFINED
       narrower answer rather than a block with some of its declarations missing. */
    if (n % 3u != 0) shaped = false;
    for (i = 0; shaped && i + 2u < n; i += 3u) {
        JSValue jn = JS_GetPropertyUint32(ctx, arr, i);
        JSValue jv = JS_GetPropertyUint32(ctx, arr, i + 1u);
        JSValue ji = JS_GetPropertyUint32(ctx, arr, i + 2u);

        shaped = JS_IsString(jn) && (JS_IsString(jv) || JS_IsNull(jv)) && JS_IsBool(ji);
        JS_FreeValue(ctx, jn);
        JS_FreeValue(ctx, jv);
        JS_FreeValue(ctx, ji);
    }
    DCHECK(shaped,
           "the declaration block's value store is not a run of (name, value-or-null, important) triples — "
           "the array is written in one loop by this file alone, on a record whose own slot no page can "
           "reach, so a malformed one is this component disagreeing with itself. A declaration with no NAME "
           "is one no member could ever ask for again, and a value that is neither bytes nor the stated "
           "absence of them is a third state this store has no reading for");
    if (!shaped) { JS_FreeValue(ctx, arr); return false; }
    for (i = 0; i + 2u < n; i += 3u) {
        JSValue jn = JS_GetPropertyUint32(ctx, arr, i);
        JSValue jv = JS_GetPropertyUint32(ctx, arr, i + 1u);
        JSValue ji = JS_GetPropertyUint32(ctx, arr, i + 2u);
        const char *name = JS_ToCString(ctx, jn);
        const char *value = JS_IsString(jv) ? JS_ToCString(ctx, jv) : NULL;

        CHECK(name != NULL && (value != NULL || JS_IsNull(jv)),
              "cssom: OOM reading a declaration back out of the block's value store — a dropped one would "
              "take the declaration with it, and this loop has already refused to be a partial read");
        /* APPENDED AND NOT COLLECTED, because the stored list has ALREADY been through the collapse and the
           ORDER is part of what it stores: §6.6's shorthand loop asks what sits BETWEEN two longhands, so a
           decode that re-ran the collector would answer a different serialization for the same block. */
        cssd_decls_append(out, cssd_strdup(name), value ? cssd_strdup(value) : NULL, JS_ToBool(ctx, ji));
        JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        JS_FreeValue(ctx, jn);
        JS_FreeValue(ctx, jv);
        JS_FreeValue(ctx, ji);
    }
    JS_FreeValue(ctx, arr);
#if APICLIENT_DEV
    /* THE ONE CHECK THIS STORE OWES AND THE ONE THAT CAN FAIL: the declarations it just handed back must
       serialize to the very projection it was filed beside. It is NOT the identity by construction — the two
       strings are produced by two different runs of `cssd_serialize_decls` over two different `CssDecls`, one
       built by a write and one decoded here — so it is exactly the encode/decode round trip, and it is asked
       where the answer is about to become a member's answer.
       IT IS TOTAL AND NOT SCOPED, AND THE COST ARGUMENT IS WHY IT CAN AFFORD TO BE: what this store REPLACED
       on every read was a full lexbor parse of the block's text, and a serialization is cheaper than that —
       so a dev read is cheaper than it was before this diff and a release read is cheaper still. A check
       scoped to blocks holding a value no page can spell would have been the population where the ANSWER is
       load-bearing and not the population where the DEFECT lives: an encode/decode disagreement is a property
       of this file's two loops and shows on any block at all.
       WHAT IT REPLACES IS `cssd_backing_assert_round_trip`, which is RETIRED and not restated: that check
       asked whether the TEXT still named every declaration, which after this diff is a question about a
       string the block does not store and whose answer is NO by design for the values the store carries. */
    {
        char *again = cssd_serialize_decls(out);
        JSValue jp = JS_GetPropertyStr(ctx, block, CSSD_DECLS_TEXT_FIELD);
        const char *want = JS_IsString(jp) ? JS_ToCString(ctx, jp) : NULL;

        DCHECKF(want != NULL && strcmp(again ? again : "", want) == 0,
                "a CSS declaration block's value store decoded to declarations that do NOT serialize back to "
                "the projection they were filed beside — `%s` against `%s`. The store and the backing are one "
                "write, so the two can only disagree if this file's encode and its decode disagree, and the "
                "store is what every §6.6.1 member now reads INSTEAD of the block's text",
                again ? again : "", want ? want : "(absent)");
        if (want) JS_FreeCString(ctx, want);
        JS_FreeValue(ctx, jp);
        free(again);
    }
#endif
    return true;
}

/* THE BLOCK'S DECLARATIONS — the store when it still describes the backing, and a parse of the backing's text
   when it does not. Every §6.6.1 member that reads declarations goes through here, so no two of them can
   disagree about which of the two answered. */
static void cssd_declarations_read(JSContext *ctx, JSValueConst block, CssDecls *out)
{
    size_t len = 0;
    char *text = cssd_declarations_text(ctx, block, &len);

    if (!cssd_decls_load(ctx, block, text, len, out))
        cssd_decls_from_text(text, len, out);
    free(text);
}

/* THE WHOLE OF A WRITE'S STORAGE STEP: serialize, update the backing, and file the declarations beside the
   bytes that landed. THE BYTES ARE RE-READ RATHER THAN ASSUMED, because a backing may not keep what it was
   handed — `css_rule_set_block_text` re-serializes a PAGE, MARGIN or KEYFRAME rule's block through that
   context's own restriction, so what it stores is a SUBSET of what this call serialized. Filing `d` beside a
   projection the backing did not keep would make the store claim declarations the restriction removed, which
   is a wrong answer where the missing store is merely a smaller one — so a write the backing altered files
   NOTHING and that block keeps exactly the behaviour it had before this store existed. */
static void cssd_declarations_put(JSContext *ctx, JSValueConst block, const CssDecls *d)
{
    char *next = cssd_serialize_decls(d);
    size_t nlen = next ? strlen(next) : 0;
    char *back;
    size_t blen = 0;
    bool kept;

    cssd_declarations_write(ctx, block, next ? next : "", nlen);
    back = cssd_declarations_text(ctx, block, &blen);
    kept = blen == nlen && (nlen == 0 || memcmp(back ? back : "", next ? next : "", nlen) == 0);
    free(back);
    cssd_decls_store(ctx, block, d, kept ? (next ? next : "") : NULL);
    free(next);
}

/* §6.6.1's "let component value list be the result of parsing value for property property", asked of THE
   PARSER: the declaration `name: value` goes through exactly what a declaration in a block goes through —
   cssd_decl_take, the ONE answer to CSSOM §6.7.1 "Parsing CSS Values" — and what comes back is the canonical
   serialization of its value, or NULL.
   IT IS THE SAME FUNCTION THE READ PATH CALLS, and that is the whole of why it is not written out here. The
   version that tested `d->type != LXB_CSS_PROPERTY__UNDEF` for itself asked the same question and gave the
   other answer, so `el.style.width = "calc(100% - 20px)"` set nothing while the identical declaration in a
   `style=""` attribute set it — see cssd_decl_take for why lexbor makes every math function a `__UNDEF`.
   NULL IS "IF COMPONENT VALUE LIST IS NULL, THEN RETURN" — the whole call is abandoned and the block is left
   alone, which is what makes `style.color = 'unknown color'` leave a standing `color: red` standing. Storing
   the unparseable text instead was not a smaller version of that: the next read dropped the declaration as
   `__UNDEF` and the property came back as UNDECLARED, so the assignment silently removed a value it had no
   business touching.
   IT HAS A SECOND CALLER OUTSIDE THIS COMPONENT AND THAT IS WHY IT IS EXPORTED. CSS Conditional Rules 3
   §7.5's two-argument `CSS.supports(property, value)` ends in "and value successfully parses according to that
   property's grammar", which is this question and not a second reading of it — and its own Note pins the
   `!important` rule to the same sentence §6.7.1's Note states, so the two members must reach ONE entry or the
   second Note is obeyed in one place and not the other.
   IT ANSWERS ABOUT THE NAME IT IS GIVEN, CASE-SENSITIVELY, which is what the strcmp below is: a caller holding
   a page's spelling must resolve it through cssom_supported_css_property_named first. That is not this entry's
   business to do — §7.5 matches the property name ASCII case-insensitively against a SET before any parse, and
   folding the two into one call would make a member that failed the set test indistinguishable from one whose
   value did not parse.
   IT IS ALSO WHAT KEEPS A PROPERTY NAME FROM CARRYING A DECLARATION OF ITS OWN. This engine's block is TEXT,
   so `setProperty('a;color', 'red')` written into it would parse back as two declarations; the parse here is
   required to produce exactly ONE, named exactly `name`. §6.6.1's note that "value can not include
   !important" is the same check from the other side: a value that parsed as important is not this value.
   OWNED. */
char *cssom_parse_a_css_value(const char *name, const char *value)
{
    lxb_css_memory_t *mem = NULL;
    lxb_css_rule_declaration_list_t *list;
    CssBuf text = { 0 };
    char *out = NULL;

    DCHECK(name != NULL && value != NULL, "a declaration's value was parsed with no property name or no value");
    /* css-fonts-4 §2.1 "Font family: the font-family property", AHEAD OF LEXBOR AND NOT AFTER IT — the one
       property in this file whose grammar is taken back from the registry, and the reason is that lexbor
       cannot express it rather than that this engine prefers its own answer.
       §2.1.1's `<font-family-name> = <string> | <custom-ident>+` admits a family name spelled as SEVERAL
       identifiers, and lexbor's `font-family` state accepts exactly ONE token per list item: after the first
       it asks for a comma or the end, so `font-family: New Century Schoolbook` is a parse failure there and a
       valid declaration in a browser. THAT IS NOT A SERIALIZATION BUG AND CANNOT BE REPAIRED DOWNSTREAM —
       `css_shorthand_validates_longhand`, the hook this file already has for a longhand grammar, runs on the
       value this call RETURNS, so it never sees a value lexbor refused. The parse is the part that has to
       move, which is why the branch is here and not there; that hook's own contract stays true, since it
       still answers FALSE for every property lexbor types.
       AND IT MUST BE THIS ENTRY RATHER THAN setProperty's, because CSSOM §6.6.1 "The CSSStyleDeclaration
       Interface"'s setProperty is not the only caller: CSS Conditional Rules 3 §7.5 "The CSS namespace, and the supports() function"'s
       two-argument `CSS.supports(property, value)` ends in "and value successfully parses according to that
       property's grammar" and asks the same question here. A branch at either caller alone would make the two
       members disagree about what a `font-family` is — and `css/css-fonts/parsing/font-family-computed.html`
       asserts both, `CSS.supports` before every one of its ten values and `getComputedStyle` after.
       THE NAME IS COMPARED CASE-SENSITIVELY BECAUSE THIS ENTRY IS, which its own paragraph below states: a
       caller holding a page's spelling has already resolved it through cssom_supported_css_property_named. */
    if (strcmp(name, "font-family") == 0) return css_font_family_value(value);
    css_buf_add(&text, name);
    css_buf_add(&text, ":");
    css_buf_add(&text, value);
    list = cssd_parse_block(text.s, text.n, &mem);
    if (list && list->first && list->first->next == NULL &&
        list->first->type == LXB_CSS_RULE_DECLARATION) {
        lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(list->first);
        char *dname, *dvalue;

        /* §6.7.1's Note — "\"!important\" declarations are not part of the property value space and will
           therefore cause parse a CSS value to return null" — asked of the DECLARATION rather than of the
           text, because that is where lexbor records the flag and it records it for a `__UNDEF` too. */
        /* CSSOM §6.7.1 "Parsing CSS Values"' operand is a PROPERTY — this entry's own header says its one
           other caller is CSS Conditional Rules 3 §7.5's `CSS.supports(property, value)` — so the context is
           the unrestricted one and the descriptor grammar is not reachable from here. A descriptor body's
           write does not lose it: `css_rule_set_block_text` re-serializes the whole block through the rule's
           own context afterwards, which is the second of the two moments a restricted block's text is
           DECIDED. */
        if (!d->important && cssd_decl_take(d, text.s, text.n, CSSOM_BLOCK_UNRESTRICTED, &dname, &dvalue)) {
            DCHECK(dname != NULL,
                   "cssd_decl_take answered TRUE with no property name — a declaration this engine holds is "
                   "one whose name §6.6.1's set a CSS declaration compares case-sensitively, so a nameless "
                   "one could not be stored, read back or removed");
            if (strcmp(dname, name) == 0) {
                out = dvalue;
                dvalue = NULL;
            }
            free(dname);
            free(dvalue);
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
    CssLogicalGroup group = CSS_LOGICAL_GROUP_NONE;
    bool physical = true, move = false;
    unsigned j;

    if (at >= 0) {
        group = css_logical_group_of(name, &physical);
        for (j = (unsigned)at + 1; group != CSS_LOGICAL_GROUP_NONE && j < d->n; j++) {
            bool other = true;

            if (css_logical_group_of(d->v[j].name, &other) == group && other != physical) {
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

/* THE DECLARATIONS ONE WRITE DECIDED, reported back by name. §6.6.1's setProperty step 8 expands a SHORTHAND
   into its longhands, so ONE call sets N declarations and none of them is named `property` — and the caller
   that has to say something ABOUT each of them (below: which unknown the page's value left in it) cannot
   recover that list from the arguments it passed. The names are the CALLER'S OWN and the longhand table's, both
   of which outlive the call; the VALUES are read back out of the list afterwards, because `cssd_decls_set` may
   grow it and a pointer taken during the walk would dangle. */
typedef struct { const char *name[CSS_SHORTHAND_MAX_LONGHANDS]; unsigned n; } CssdSetNames;

static void cssd_set_names_add(CssdSetNames *set, const char *name)
{
    if (!set) return;
    CHECK(set->n < CSS_SHORTHAND_MAX_LONGHANDS,
          "cssom: a write set more declarations than a shorthand has longhands — the array §6.6.1's setProperty "
          "step 8 expands through is sized from that list");
    set->name[set->n++] = name;
}

/* §6.6.1's setProperty over the declarations, from its step 5 on: "Let component value list be the result of
   parsing value for property property", step 6's "If component value list is null, then return", and step 8's
   "If property is a shorthand property, then for each longhand property longhand that property maps to, in
   canonical order, follow these substeps" — whose 8.1 is "set the CSS declaration longhand with the appropriate
   value(s) from component value list". A shorthand therefore
   sets its longhands and never itself — which is the same expansion the read path does, run through the same
   two functions, so a block cannot hold a property one of them would have expanded. */
static void cssd_decls_set_property(CssDecls *d, const char *name, const char *value, bool important,
                                    CssdSetNames *set)
{
    const char *const *lh;
    char *values[CSS_SHORTHAND_MAX_LONGHANDS];
    char *parsed;
    unsigned n, i;

    parsed = cssom_parse_a_css_value(name, value);
    if (!parsed) return;
    lh = css_shorthand_longhands(name, &n);
    if (!lh) {
        char *v = css_shorthand_validates_longhand(name) ? css_shorthand_longhand_value(name, parsed)
                                                         : parsed;

        if (v != parsed) free(parsed);
        if (v) { cssd_decls_set(d, name, v, important); cssd_set_names_add(set, name); }
        return;
    }
    CHECK(n <= CSS_SHORTHAND_MAX_LONGHANDS,
          "cssom: a shorthand's longhand list outgrew the array §6.6.1's setProperty expands through");
    /* css-values-5 "Substitution in Shorthand Properties"' pending-substitution value, on the WRITE path for
       the same reason the read path has it: §6.6.1's step 8.1 wants "the appropriate value(s) from component
       value list", and a component value list still holding an arbitrary substitution function has no
       appropriate values yet. `el.style.margin = "var(--g) 0"` and `<div style="margin: var(--g) 0">` are one
       declaration written two ways, so a branch at one of them alone would make the same bytes set four
       longhands through one door and none through the other. */
    if (css_var_references(parsed)) {
        for (i = 0; i < n; i++) {
            cssd_decls_set(d, lh[i], css_pending_make(name, parsed), important);
            cssd_set_names_add(set, lh[i]);
        }
        free(parsed);
        return;
    }
    for (i = 0; i < n; i++) {
        values[i] = css_shorthand_component(name, parsed, lh[i]);
        if (!values[i]) break;
    }
    free(parsed);
    if (i < n) {                       /* the shorthand's own grammar refused it: the call is abandoned */
        while (i > 0) free(values[--i]);
        return;
    }
    for (i = 0; i < n; i++) { cssd_decls_set(d, lh[i], values[i], important); cssd_set_names_add(set, lh[i]); }
}

/* §6.6.1's removeProperty, steps 5 and 6: "If property is a shorthand property, for each longhand property
   longhand that property maps to" [… step 5.2 …] "Remove that CSS declaration and let removed be true", and
   step 6's "Otherwise, if property is a case-sensitive match for a property name of a CSS declaration in the
   declarations, remove that CSS declaration". The shorthand's OWN name needs no removal step, because a block
   never holds one. */
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

/* ---- THE UNKNOWN A DECLARATION'S VALUE CARRIES, which the declarations themselves cannot -----------------
 *
 * §6.6's DECLARATIONS ARE TEXT — the element's `style` attribute, or the rule's own block text — so a value the
 * page supplied that is UNKNOWN EXTERNAL INPUT has nowhere in them to be. Web IDL crosses one of those into a
 * `CSSOMString` argument AS ITSELF (core/idl_args.h's `idl_concolic_rule` answers IDL_CONCOLIC_CROSSES for
 * every coercing type, so opacity survives the conversion and the BODY is what owes the handling), and the body
 * that then asked C for bytes aborted at the ToString boundary. This is the side map that answers instead, and
 * it is the same decision core/dom/element.c makes one component over for an ATTRIBUTE value: Lexbor stores the
 * ToString'd bytes and the provenance is gone, so the WRITE records the unknown and the READ recovers it.
 *
 * THE BYTES ARE THE VALUE'S OWN EXAMPLE AND ARE NOT INVENTED. A concolic carries the concrete value this engine
 * COMPUTED by running the real operators on real operands (`window.innerWidth - document.documentElement.
 * clientWidth` is a real subtraction of two numbers this engine derived from CSS 2.1 §10.1 "Definition of
 * 'containing block'"'s initial containing block), so §6.6.1's step 5 parse runs on bytes the run produced
 * rather than on a placeholder — and the declaration that lands is byte-identical to the one a browser lands.
 * What would be invented is a value for an unknown with NO example, and that case does not reach here.
 *
 * THE RECORD IS KEYED BY PROPERTY NAME AND VALIDATED BY THE DECLARATION'S OWN BYTES, which is what keeps it
 * from going stale in a way no writer could be made to notice. Every entry STATES those bytes as a fact of its
 * own, written in the same call that files the unknown — so a read that finds the block no
 * longer declaring those bytes has found a record about a declaration that is gone, and answers the text. A
 * VALIDATION READ OUT OF THE UNKNOWN'S EXAMPLE IS WHAT USED TO STAND HERE AND IT WAS NOT A SECOND SPELLING OF
 * THIS ONE: `concolic_example` answers PER FLOW and withdraws itself on a path whose own branch contradicted
 * it, so the bytes went away under an entry nothing had touched — see `cssd_taint_set`. The key itself never
 * had a value component and is the same key core/dom/element.c's attribute shadow uses (the SLOT's identity,
 * nothing of what is in it); what parts the two is that an attribute has ONE writer — solver/dom_cow.h's
 * chokepoint, which fuses the value and the taint — while a declaration's text has several, so this record
 * needs a staleness test where that shadow needs none. A
 * wholesale replacement through `cssText`, through `setAttribute('style', …)` or through the rule's own text
 * therefore needs no invalidation call anywhere: there is no write that can leave a WRONG answer behind, only
 * one that leaves a silent entry. The residual is the other direction and it is SOUND: a concrete write of the
 * very same bytes through a path that does not clear (a `style` attribute rewritten to the identical string)
 * keeps an unknown alive over a value the page has since spelled out, which over-explores and never drops an
 * arm — which is the side §Solver-half's feasible-refinement rule says to err on.
 *
 * WHAT IT COVERS IS THE BLOCK THE PAGE WROTE THROUGH, AND THE CASCADE IS A NAMED RESIDUAL. §7.2's computed
 * block is a DIFFERENT block over a DIFFERENT question: its declarations are not stored anywhere, they are the
 * resolved value of every longhand, and the path to one runs `css_resolved_value` -> the computed value -> the
 * CASCADE, every step of which answers `char *`. So `el.style.setProperty('--w', w + 'px')` followed by
 * `getComputedStyle(el).getPropertyValue('--w')` answers the bytes with the domain dropped, while the same
 * read through `el.style` answers the unknown. THE NEXT DIFF makes the cascade's winning declaration able to
 * SAY it came from a block that recorded an unknown for it — `cssom_cascaded_value` reports which origin won
 * (core/css/css_cascade.h already sorts them), and the element-attached arm asks this record — after which
 * `css_resolved_value`'s "any other property" arm can derive from it exactly as its box-model arms already
 * derive from a `CssPx`'s environment facts. ITS ABSENCE SHOWS as a page whose `getComputedStyle` read of a
 * custom property it set from a viewport measurement takes one arm of a comparison where the `el.style` read
 * of the same property forks both.
 *
 * IT IS A JS OBJECT ON THE BLOCK'S OWN RECORD, so it is per-flow, snapshot-able and parkable for free: an
 * ordinary property write is what the COW delta captures, and the block itself already belongs to the flow that
 * first asked for it (both creators say so). A malloc'd C map beside it would be state the delta does not swap.
 * The record has a NULL prototype (core/idl_slots.h), which is what makes a CSS property name safe to use as a
 * key: `setProperty('__proto__', v)` would otherwise write through Object.prototype's accessor instead of
 * creating an own property. */
#define CSSD_TAINT_FIELD "declarationTaint"
/* The spec algorithm the stored bytes came out of, which solver/concolic.h requires a derivation to
   name; the LONGHAND it produced them for is appended, because one write can produce several. */
#define CSSD_TAINT_OP    "parse-a-css-value:"

static JSValue cssd_taint_record(JSContext *ctx, JSValueConst block, bool create)
{
    JSValue rec = JS_GetPropertyStr(ctx, block, CSSD_TAINT_FIELD);

    if (JS_IsObject(rec) || !create) return rec;
    JS_FreeValue(ctx, rec);
    rec = idl_slots_new(ctx);
    CHECK(JS_IsObject(rec), "cssom: the declaration block's unknown-value record could not be allocated — a "
                            "dropped one would read as a declaration whose provenance the page never supplied");
    JS_SetPropertyStr(ctx, block, CSSD_TAINT_FIELD, JS_DupValue(ctx, rec));
    return rec;
}

/* ONE ENTRY STATES TWO FACTS AND THEY ARE WRITTEN IN ONE CALL, which is solver/dom_cow.h's own rule for an
   attribute's value and its taint — "They were two calls every caller made in agreement over a key each
   computed for itself: a caller that made one and not the other left a stale taint on a fresh value". The two
   are the UNKNOWN the page's value was, and DECLARED, the bytes this write put in the block for it. */
#define CSSD_TAINT_UNKNOWN  "unknown"
#define CSSD_TAINT_DECLARED "declared"

/* Record `v` — the unknown behind the declaration named `name` — beside `declared`, THE BYTES THIS WRITE
   STORED IN THE BLOCK FOR IT. Those bytes are the entry's own fact and are NOT read back out of the unknown.
   THE BYTES USED TO BE TAKEN FROM THE UNKNOWN'S EXAMPLE AT THE READ, AND THAT WAS A LIVE DEFECT RATHER THAN
   only the thing that blocked a declaration holding an unknown with none. `concolic_example` is a PER-FLOW
   accessor: solver/concolic.h's `concolic_contradict_example` makes a value answer with NO example on a path
   whose own branch contradicted it, which is §Solver-half's forced sibling and is the central mechanism this
   engine exists to run — `decide_note_forced_arm` files it under the branched value's identity AND under its
   comparison subject's, and a value read back OUT of this record and branched on is exactly such a subject.
   So `el.style.color = x; if (el.style.color === 'blue') …` left the read on the forced arm asking a value
   that had stopped answering, after which the entry matched nothing and the member returned the BLOCK'S PLAIN
   TEXT — the example the flow had just proved wrong, handed to the page as a concrete string with the domain
   gone. The record's own header states the rule correctly ("VALIDATED BY THE DECLARATION'S OWN BYTES") and the
   mechanism read them through the one accessor entitled to withdraw them. */
static void cssd_taint_set(JSContext *ctx, JSValueConst block, const char *name, JSValueConst v,
                           const char *declared)
{
    JSValue rec, entry, bytes;
    JSAtom k;

    DCHECK(concolic_is(v),
           "a declaration's unknown was recorded with a value that is not one — this record answers "
           "§6.6.1's reads INSTEAD of the block's text, so a real string in it would be a second, "
           "unvalidated copy of a declaration the text already holds");
    DCHECK(declared != NULL,
           "a declaration's unknown was recorded without the bytes the write stored for it — those bytes are "
           "what ties the entry to THIS declaration rather than to a property name somebody has since written, "
           "so an entry without them could never be matched to the block again");
    rec = cssd_taint_record(ctx, block, true);
    entry = idl_slots_new(ctx);
    CHECK(JS_IsObject(entry),
          "cssom: a declaration's unknown-value entry could not be allocated — a dropped one would read as a "
          "declaration whose provenance the page never supplied");
    bytes = JS_NewString(ctx, declared);
    CHECK(JS_IsString(bytes),
          "cssom: OOM copying the bytes a declaration's unknown was stored as — an entry that lost them would "
          "match no declaration ever again, so the unknown would be dropped on the next read");
    JS_SetPropertyStr(ctx, entry, CSSD_TAINT_UNKNOWN, JS_DupValue(ctx, v));
    JS_SetPropertyStr(ctx, entry, CSSD_TAINT_DECLARED, bytes);
    k = JS_NewAtom(ctx, name);
    CHECK(k != JS_ATOM_NULL, "cssom: a property name could not be interned for the block's unknown record");
    JS_SetProperty(ctx, rec, k, entry);   /* consumes `entry` */
    JS_FreeAtom(ctx, k);
    JS_FreeValue(ctx, rec);
}

/* CLEAR the entry for `name`, which is what a CONCRETE write says: this declaration is no longer an unknown's.
   IT IS A SEPARATE ENTRY AND NOT A SENTINEL ARGUMENT TO THE ONE ABOVE, because the pair (no unknown, some
   declared bytes) and the pair (an unknown, no declared bytes) are both states that call would have to refuse
   at runtime, and §Fix-the-ROOT wants them unspellable instead. */
static void cssd_taint_clear(JSContext *ctx, JSValueConst block, const char *name)
{
    JSValue rec = cssd_taint_record(ctx, block, false);
    JSAtom k;

    if (!JS_IsObject(rec)) { JS_FreeValue(ctx, rec); return; }
    k = JS_NewAtom(ctx, name);
    CHECK(k != JS_ATOM_NULL, "cssom: a property name could not be interned for the block's unknown record");
    JS_DeleteProperty(ctx, rec, k, 0);
    JS_FreeAtom(ctx, k);
    JS_FreeValue(ctx, rec);
}

/* The unknown behind the declaration `name` currently declares, or JS_UNDEFINED when there is none. `declared`
   is what the BLOCK'S TEXT says that property's value is, read by the caller through its own §6.6.1 step — the
   record answers only for a declaration whose bytes are still the ones it was written about, and the bytes it
   compares are the ENTRY'S OWN, stated by the write. NOTHING HERE ASKS THE UNKNOWN ANYTHING, which is the
   property that makes the record usable by a value with no example at all; the paragraph at `cssd_taint_set`
   is why it is also a correctness rule for every value that has one. OWNED. */
static JSValue cssd_taint_read(JSContext *ctx, JSValueConst block, const char *name, const char *declared)
{
    JSValue rec = cssd_taint_record(ctx, block, false), entry, held, bytes;
    const char *stored;
    bool same;

    if (!JS_IsObject(rec) || !declared) { JS_FreeValue(ctx, rec); return JS_UNDEFINED; }
    entry = JS_GetPropertyStr(ctx, rec, name);
    JS_FreeValue(ctx, rec);
    if (!JS_IsObject(entry)) { JS_FreeValue(ctx, entry); return JS_UNDEFINED; }
    held  = JS_GetPropertyStr(ctx, entry, CSSD_TAINT_UNKNOWN);
    bytes = JS_GetPropertyStr(ctx, entry, CSSD_TAINT_DECLARED);
    JS_FreeValue(ctx, entry);
    DCHECK(concolic_is(held) && JS_IsString(bytes),
           "a declaration's unknown-value entry holds something other than an unknown and the bytes it was "
           "declared as — the entry is written in ONE call by this file alone, on a record whose own slot the "
           "page cannot reach, so a half-formed one is this component disagreeing with itself");
    stored = JS_IsString(bytes) ? JS_ToCString(ctx, bytes) : NULL;
    JS_FreeValue(ctx, bytes);
    same = stored != NULL && concolic_is(held) && strcmp(stored, declared) == 0;
    if (stored) JS_FreeCString(ctx, stored);
    if (same) return held;
    JS_FreeValue(ctx, held);
    return JS_UNDEFINED;
}

/* §6.6.1's cssText setter step 2, "Empty the declarations", applied to this record — every entry it holds is
   about a declaration that no longer exists. The reads above would already answer nothing for them; this is
   the step the spec states, run where it is stated, rather than left to the validation to absorb. */
static void cssd_taint_empty(JSContext *ctx, JSValueConst block)
{
    JSValue rec = cssd_taint_record(ctx, block, false);

    if (!JS_IsObject(rec)) { JS_FreeValue(ctx, rec); return; }
    JS_FreeValue(ctx, rec);
    JS_SetPropertyStr(ctx, block, CSSD_TAINT_FIELD, JS_UNDEFINED);
}

/* THE BYTES AND THE UNKNOWN A DECLARATION'S VALUE WRITES, which is ONE decision resolved in ONE place because
   §6.6.1's `setProperty`, its per-property IDL attributes and CSS Fonts 5 §9.1's descriptor attributes are
   three spellings of it — a second copy is a second answer to "what does a declaration store for a value the
   page could not spell". `owned` is the JS_ToCString to free. */
typedef struct { const char *bytes; JSValueConst taint; const char *owned; } CssdValue;

/* `member` NAMES THE §6.6.1 SPELLING THE PAGE WROTE AND `property` THE CSS PROPERTY IT WROTE IT TO, and they
   are parameters rather than anything this body could derive: THREE members answer through here, so the abort
   below used to stamp this helper's own line for all of them and name an action with no object. What a reader
   met was a correct spec claim and a remedy — "the three value writes here" — with no way to tell which of the
   three they were standing in or which property's grammar step 5 was about, and the property is not decoration:
   it is what decides whether step 5's parse is the permissive css-variables-1 §2.1 "Custom Property Value
   Syntax" one or a longhand's own. Establishing it cost a lane a fetch of a third party's bundle. */
static bool cssd_value(JSContext *ctx, JSValueConst value, const char *member, const char *property,
                       CssdValue *out)
{
    DCHECK(member != NULL && property != NULL,
           "a declaration's value was resolved without naming the §6.6.1 member that wrote it or the property "
           "it was written to — the abort below is reached from three members and stamps this one line for "
           "all of them, so a caller that does not name itself makes that crash unactionable");
    /* EVERY FIELD BEFORE THE FIRST THING THAT CAN FAIL, because the failure path is the caller's `_free`, which
       frees exactly what this struct holds and nothing else — the same rule quickjs-step.h's step states owe. */
    out->bytes = NULL;
    out->owned = NULL;
    out->taint = JS_UNDEFINED;
    if (concolic_is(value)) {
        JSValue example = concolic_example(ctx, value);

        /* AN UNKNOWN WITH NO EXAMPLE HAS NO BYTES AT ALL, and §6.6.1 step 5's "parsing value for property
           property" is over bytes. Its outcome then decides step 6's "If component value list is null, then
           return" — so the block either gains this declaration or is left exactly as it was, and BOTH worlds
           are feasible. Neither may be picked: picking the first writes a declaration whose value this engine
           invented, and picking the second silently discards a write the page made. */
        if (JS_IsUndefined(example)) {
            const char *shape = concolic_shape_c(value);

            JS_FreeValue(ctx, example);
            DFAILF("CSSOM §6.6.1 The CSSStyleDeclaration Interface's %s reached step 5, \"Let component value "
                   "list be the result of parsing value for property property\", for the property `%s` with a "
                   "value that is UNKNOWN EXTERNAL INPUT WITH NO EXAMPLE (`%s`) — so there are no bytes to "
                   "parse and no way to decide step 6's \"If component value list is null, then return\" — nor "
                   "step 3's \"If value is the empty string, invoke removeProperty() with property as "
                   "argument and return\", two steps above it. "
                   "THERE ARE THREE FEASIBLE COMPLETIONS AND NOT TWO, and none of them may be picked. THIS "
                   "SENTENCE SAID `BOTH` AND NAMED THE FIRST AND THIRD, which is the enumeration error that "
                   "comes of counting the arms of the STEP a crash stands at rather than the OBSERVABLE "
                   "outcomes of the member: the block GAINS this declaration (step 5 parsed something), the "
                   "block LOSES it (step 3 — every caller spells the write `*v.bytes ? v.bytes : NULL` and "
                   "`cssd_write_declaration` reads NULL as REMOVE, so an unknown standing for the empty "
                   "string is a removeProperty), or the block is left exactly as it was (step 6). The third "
                   "is not the second: they differ on every block that already declares this property, and "
                   "js_cssd_set_property's step-3 paragraph already names that world for an unknown that HAS "
                   "an example. "
                   "WHAT IS MISSING IS THE FORK AND THE RECORD ENTRY ITS SUCCESS ARM FILES, AND EVERYTHING "
                   "UNDER THEM IS BUILT. THE STORE IS: this block's unknown record above used to validate an "
                   "entry by strcmp of its concolic's EXAMPLE against the declared bytes, which keyed the "
                   "record by an example and refused the one kind of value that has none; it now states those "
                   "bytes as the ENTRY'S own fact and asks the unknown nothing — and `cssd_taint_set` records "
                   "why that keying was ALSO a live defect for the entries that do have an example. AND THE "
                   "DECLARATION ITSELF IS STORABLE NOW: `cssd_declarations_put` keeps a block's declarations "
                   "as VALUES on its own record and the backing text is the PROJECTION, so a declaration the "
                   "serialization must hide survives the write that made it. "
                   "THE SENTENCE `§6.6'S DECLARATIONS ARE TEXT, SO THE BLOCK CANNOT HOLD A PROPERTY WITHOUT "
                   "BYTES FOR IT` STOOD HERE AND IS REWRITTEN RATHER THAN DELETED, because it is what a "
                   "reader re-derives from `getAttribute('style')` and because the TRAP it was guarding is "
                   "untouched: `cssd_write_declaration` still reads a NULL value as REMOVE and all three "
                   "callers still spell the argument `*v.bytes ? v.bytes : NULL`, so an arm with no bytes IS "
                   "the arm that removes the declaration, and a fork whose success arm goes through that "
                   "spelling still leaves two identical worlds. What has changed is that the success arm now "
                   "HAS somewhere to put a declaration with no bytes, so the fix is at the three call sites "
                   "and the write's own NULL contract rather than in the storage. And the older refusal "
                   "stands unchanged: writing `concolic_shape_c`'s shape the way core/dom/element.c's "
                   "`el_attr_value` writes it into an ATTRIBUTE is NOT the symmetric answer this crash once "
                   "called it — an attribute value has no grammar and a declaration's has one, the parse here "
                   "is REAL (`cssom_parse_a_css_value` runs lexbor and answers NULL for a value the grammar "
                   "refuses, which IS step 6's arm), so the shape would parse as nothing and the write would "
                   "silently become the arm that leaves the block alone. "
                   "WHAT THE NEXT DIFF BUILDS — AND ITS FIRST HALF IS ONE STEP LOWER THAN THIS CLAUSE USED TO "
                   "SAY. THE FORK IS UNCHANGED AND IS RIGHT: these three bodies as step machines asking "
                   "quickjs-step.h's `step_fork_run` which completion the member reached, numbered so that "
                   "outcome 0 is the ordinary one in which the declaration is written. WHAT WAS WRONG WAS THE "
                   "CARRIER. The clause named `css_pending_make`/`css_pending_is` as the pattern "
                   "core/css/css_pending_substitution.h already carries, and that is a real encoding of the "
                   "WRONG KIND: a pending-substitution value is never STORED. It is DERIVED AT EVERY PARSE "
                   "from the shorthand's own text — `cssd_decls_collect_declaration` mints one per longhand "
                   "whenever a shorthand's value references an arbitrary substitution function, and "
                   "`cssd_try_shorthand` writes the group back out as that ORIGINAL SHORTHAND — so the "
                   "block's backing never holds one and never has to. A value with NO BYTES has no text to be "
                   "derived from, so the pattern cannot carry it. Read those two together with "
                   "`cssd_serialize_decls`: that is the whole derivation and it needs no run. "
                   "SUBPROBLEM (1) — THE BACKING — IS BUILT, AND THE SENTENCES THAT DESCRIBED IT AS MISSING "
                   "ARE REWRITTEN RATHER THAN DELETED, because a reader who re-derives the argument from the "
                   "serialization alone will re-state it. The argument was: §6.6's declarations are kept as "
                   "the SERIALIZATION of the block, re-parsed on every read, and a serialization is an "
                   "OBSERVATION algorithm — so every candidate carrier written into it is one of two wrong "
                   "answers, a value the re-parse DROPS (the write arm and the leave-alone arm become one "
                   "world and the fork bought nothing) or a value the re-parse KEEPS, which is by "
                   "construction a value a page can read back out of `getAttribute('style')`. Every clause of "
                   "that is still true OF THE SERIALIZATION and is no longer true of the STORAGE: "
                   "`cssd_declarations_put` files the declarations THEMSELVES on the block's record beside the "
                   "bytes it wrote, and `cssd_declarations_read` takes them while the backing still holds "
                   "those bytes. A declaration with NO BYTES is therefore storable, which is the capability "
                   "this crash said was missing. "
                   "SO WHAT REMAINS IS (2) AND (3), WHICH ARE ONE LANDING: these three bodies as step "
                   "machines over `step_fork_run`, and the write arm filing this record's entry against the "
                   "declaration the store now lets the block hold. Either alone is a pair of identical "
                   "worlds, which is the trap the paragraph above names; neither is blocked by anything else. "
                   "AND (1) DID NOT LAND AS PREDICTED IN ONE RESPECT, RECORDED HERE BECAUSE THE NEXT READER "
                   "WILL OTHERWISE INHERIT THE PREDICTION. This clause said (1) \"alters no member's "
                   "behaviour and no page-visible answer\". It alters exactly the answers the loss was "
                   "corrupting: a block holding `margin: var(--g) 0` written through "
                   "`el.style.marginTop = '5px'` used to DESTROY `margin-right`, `margin-bottom` and "
                   "`margin-left` — measured 3 of 3 at the instrument that has now retired — so `length` "
                   "answered 1 where css-values-5 \"Substitution in Shorthand Properties\" says the block "
                   "declares four, and `item(i)` enumerated one. Those answers are now right. What is "
                   "unchanged is what a page can SEE of the values: a pending-substitution value still reads "
                   "back as the empty string, and `cssText` and `getAttribute('style')` are byte-identical to "
                   "what they were, because §6.6's serialization is untouched. The lesson is the general one: "
                   "a storage change that FIXES a destroyed declaration cannot also leave every member's "
                   "answer alone, and a clause claiming both was describing the storage it wanted rather than "
                   "the defect it was fixing. "
                   "AND THAT ORDER IS THIS CRASH'S ALONE — IT DOES NOT DEFER THE OTHER TWO FORKS THE SAME "
                   "MEMBERS OWE. js_cssd_set_property's step-3 and step-4 residuals are about an unknown that "
                   "HAS an example, and a success arm there has real bytes to write, so (2) builds them with "
                   "no new backing under it. A reader arriving here from one of those must not read (1) as "
                   "standing in front of it: what (1) unblocks is the NO-EXAMPLE write and nothing else. "
                   "HOW ITS ABSENCE WOULD SHOW, as an observation: a run carries this abort at a value derived "
                   "on an arm the run FORCED rather than observed — a forced sibling drops the example its "
                   "branch contradicted, so every value computed downstream of one has no example, and a page "
                   "that reads prior-session state and styles itself from it meets that on its first write. "
                   "This is NOT the crash for an unknown that HAS an example: that one parses its own computed "
                   "bytes and keeps its domain in the block's unknown record above.",
                   member, property, shape ? shape : "{}");
            return false;   /* release: no capability to add, so step 6's own answer — the call is abandoned */
        }
        DCHECK(!JS_IsObject(example),
               "a declaration value's unknown carries an OBJECT as its concrete example — an example is the "
               "value this engine COMPUTED, so it is a primitive, and §7.1.19 ToString over an object one runs "
               "the PAGE's toString from a C activation with no flow base under it");
        out->owned = JS_ToCString(ctx, example);   /* a PRIMITIVE: §7.1.19 ToString here runs no page code */
        JS_FreeValue(ctx, example);
        if (!out->owned) return false;
        out->bytes = out->owned;
        out->taint = value;
        return true;
    }
    out->owned = JS_ToCString(ctx, value);
    DCHECK(out->owned != NULL,
           "a declaration value reached the write unconverted — its IDL declaration is what converts it, and "
           "running the page's toString from here is the drive-to-completion the flow machinery exists to "
           "avoid");
    if (!out->owned) return false;
    out->bytes = out->owned;
    return true;
}

static void cssd_value_free(JSContext *ctx, CssdValue *v) { if (v->owned) JS_FreeCString(ctx, v->owned); }

/* The whole of a member's write: read the declarations, edit them, put them back — where "put them back" is
   §6.6's UPDATE STYLE ATTRIBUTE, whose step is "set an attribute value for owner node using 'style' and the
   result of SERIALIZING declaration block", generalized to the backing the block actually has.
   THE SERIALIZATION IS LOSSLESS FOR THE DECLARATIONS A PAGE CAN SPELL, AND WAS NOT BEFORE, which is why the
   block used to store an unconsolidated list instead. §6.6's serialization CONSOLIDATES — four `margin-*`
   declarations become one `margin` — and while a block held the author's shorthand unexpanded, storing that
   threw away which longhands it declared, so the very next `removeProperty('margin-top')` found nothing by that
   name. Now the declarations ARE longhands: `margin: 2px 1px 1px` parses back to exactly the four the serializer
   consolidated, so the round trip is the identity on them, and what a page reads out of `getAttribute('style')`
   is the string a browser writes there.
   THAT SENTENCE READ `LOSSLESS FOR THE DECLARATIONS` WITHOUT THE QUALIFIER AND IS REWRITTEN RATHER THAN
   DELETED, because the argument it makes is correct and a reader who re-derives it from the consolidation
   alone will re-state it unqualified. It is FALSE of a declaration whose value is one no page can spell, and
   the reason is not the consolidation at all: §6.6's serialization is an OBSERVATION algorithm and is required
   to HIDE such a value, so storing its output deletes the declaration.
   AND THE WHOLE PARAGRAPH IS NOW ABOUT THE PROJECTION RATHER THAN ABOUT THE STORAGE, which is the one thing
   that changed under it: `cssd_declarations_put` files the declarations THEMSELVES beside the bytes it wrote,
   so the serialization's faithfulness decides what a READER sees and no longer decides what the block HOLDS.
   Every sentence above stays true of `getAttribute('style')` and of `cssText`, which is what they were always
   really about; what they may no longer be read as is a claim about what survives a write. */
/* `taint` IS THE UNKNOWN THE PAGE'S VALUE WAS, or JS_UNDEFINED when the value was a real string — and a
   CONCRETE write is a positive statement that this declaration is nobody's unknown any more, which is why it
   clears rather than leaving the last one standing.
   EACH DECLARATION THIS WRITE DECIDED GETS ITS OWN DERIVATION AND NOT A SHARED POINTER TO THE PAGE'S VALUE.
   Step 8 expands `margin` into four longhands, and `concolic_ident_c` is what a per-flow path constraint is
   keyed by — so four declarations sharing one identity would make a branch on `marginTop` decide `marginLeft`,
   which is solver/concolic.h's own worked defect. The derivation names the algorithm that produced the bytes
   (§6.6.1 step 5's parse, and step 8's component extraction for a shorthand) TOGETHER WITH the longhand it
   produced them for, and its EXAMPLE is exactly the bytes stored — which is the record's whole validation rule
   above, so it is asserted here rather than assumed. */
static void cssd_write_declaration(JSContext *ctx, JSValueConst block, const char *name, const char *value,
                                   bool important, JSValueConst taint)
{
    CssDecls d = { 0 };
    CssdSetNames set = { { NULL }, 0 };
    unsigned i;

    cssd_declarations_read(ctx, block, &d);
    if (value) cssd_decls_set_property(&d, name, value, important, &set);
    else       cssd_decls_remove_property(&d, name);
    for (i = 0; i < set.n; i++) {
        int at = cssd_decls_index(&d, set.name[i]);

        DCHECK(at >= 0 && d.v[at].value != NULL,
               "a write reported setting a declaration the list does not hold — the two are one step, and a "
               "name reported without a value is a report composed somewhere other than at the set");
        if (at < 0 || !d.v[at].value) continue;
        if (!concolic_is(taint)) { cssd_taint_clear(ctx, block, set.name[i]); continue; }
        {
            /* MEASURED AND NOT CAPPED, because the operation name is HALF THE DERIVATION'S IDENTITY: a custom
               property's name is whatever the page wrote, so a fixed buffer would truncate two long names to
               one string and file two declarations under one constraint key. */
            size_t n = sizeof CSSD_TAINT_OP - 1 + strlen(set.name[i]) + 1;
            char *op = malloc(n);
            JSValue derived;

            CHECK(op != NULL, "cssom: OOM naming the derivation behind an unknown declaration value");
            memcpy(op, CSSD_TAINT_OP, sizeof CSSD_TAINT_OP - 1);
            memcpy(op + sizeof CSSD_TAINT_OP - 1, set.name[i], strlen(set.name[i]) + 1);
            derived = concolic_builtin_hook(ctx, taint, op, JS_NewString(ctx, d.v[at].value));
            free(op);
            DCHECK(concolic_is(derived),
                   "a derivation over an unknown declaration value came back as something other than an "
                   "unknown — solver/concolic.h answers JS_UNINITIALIZED only for an operand that is not one, "
                   "and this one was tested directly above");
            /* THE BYTES THIS WRITE STORED, handed to the record as the entry's own fact. They are the same
               string the derivation above was given as its example, and that is a COINCIDENCE OF THIS CALL
               rather than the record's rule: the example answers per flow and these bytes do not. */
            cssd_taint_set(ctx, block, set.name[i], derived, d.v[at].value);
            JS_FreeValue(ctx, derived);
        }
    }
    /* §6.6.1's removeProperty steps 5 and 6 take the declaration away, so every record about it is about a
       declaration that is gone. The shorthand walk is the same one the removal made, asked of the same entry. */
    if (!value) {
        const char *const *lh;
        unsigned n;

        lh = css_shorthand_longhands(name, &n);
        if (lh) for (i = 0; i < n; i++) cssd_taint_clear(ctx, block, lh[i]);
        else    cssd_taint_clear(ctx, block, name);
    }
    cssd_declarations_put(ctx, block, &d);
    cssd_decls_free(&d);
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
    /* THE NAME, WHICH CAN BE UNKNOWN EXTERNAL INPUT. Web IDL §3.2 JavaScript type mapping crosses one into a
       `CSSOMString` argument AS ITSELF — CSSOM §3 CSSOMString makes that type "either USVString or DOMString",
       and core/idl_args.h's idl_concolic_rule answers IDL_CONCOLIC_CROSSES for every type whose conversion
       merely COERCES — so the body is what has to answer, and an unknown NAME denotes its
       own display SHAPE — a real string, stable per source. THAT IS THE HONEST ANSWER AND NOT A LUCKY ONE: all
       three members here are stated over the block's own declarations, and a shape matches no property name in
       them, so getPropertyValue reaches its last step ("Return the empty string"), getPropertyPriority reaches
       its own, and removeProperty removes nothing — which is what a browser answers for any other string that
       names no declaration. An unknown name is a question about a property this block does not have; it is not
       a question this engine failed to answer. */
    name = concolic_name_cstr(ctx, argv[0]);
    if (!name) { JS_FreeValue(ctx, block); return JS_EXCEPTION; }
    if (magic == 1) {
        /* §6.6.1's removeProperty step 3: "Let value be the return value of invoking getPropertyValue() with
           property as argument" — read BEFORE the removal, because step 8's "Return value" returns it. */
        CssDecls d = { 0 };
        char *old;
        JSValue unknown;

        cssd_declarations_read(ctx, block, &d);
        old = cssd_property_value(&d, name);
        /* The declaration's own unknown, read BEFORE the removal clears it — the value this member returns is
           the value it had, and if that value was an unknown then so is what step 8 returns. */
        unknown = cssd_taint_read(ctx, block, name, old);
        cssd_decls_free(&d);
        /* §6.6.1's step 5 — "If property is a shorthand property, for each longhand property longhand that
           property maps to" — is one edit of the declarations, made where every other edit is. The extra
           removal of the SHORTHAND'S OWN NAME that used to be here is gone with the thing that made it
           necessary: the block held the author's spelling, and now it holds the longhands §6.6 says it holds.
           THE NUMBER 4 STOOD HERE AND SO DID A TAIL READING `invoke removeProperty with longhand as argument`,
           which is a RETIRED EDITION'S WORDING — step 4 is "Let removed be false", and this step's own substeps
           now read "If longhand is not a property name of a CSS declaration in the declarations, continue" and
           "Remove that CSS declaration and let removed be true". A quotation is the half of a citation a reader
           trusts most and opens the spec for least, so one carrying words the standard no longer has is worse
           than no quotation at all. */
        cssd_write_declaration(ctx, block, name, NULL, false, JS_UNDEFINED);
        r = !JS_IsUndefined(unknown) ? unknown
          : old ? JS_NewString(ctx, old) : JS_NewStringLen(ctx, "", 0);
        free(old);
    } else if (magic == 2) {
        /* §6.6.1's getPropertyPriority. A COMPUTED block's declarations are resolved values and carry no
           important flag at all, so the answer over its whole domain is the empty string — a positive
           statement about that block, not a hole where its text would be. */
        bool important = false;

        if (!computed) {
            CssDecls d = { 0 };

            cssd_declarations_read(ctx, block, &d);
            important = cssd_property_important(&d, name);
            cssd_decls_free(&d);
        }
        r = JS_NewString(ctx, important ? "important" : "");
    } else if (computed) {
        /* CSSOM §9's RESOLVED value, which for most properties is the computed value and for the box-model
           ones is the used value (css_computed_value.h). It answers a JSValue rather than text because a used
           value derived from CSS 2.1 §10.1's initial containing block carries the VIEWPORT's domain, and a
           `char *` here would carry the number and drop the fork. */
        r = css_resolved_value(ctx, cssd_owner_element(ctx, block), name);
    } else {
        CssDecls d = { 0 };
        char *v;
        JSValue unknown;

        cssd_declarations_read(ctx, block, &d);
        v = cssd_property_value(&d, name);
        /* The declaration's own unknown, when the value the page wrote was one — see the record above. It is
           asked with the bytes the block CURRENTLY declares, which is what ties the record to this declaration
           rather than to a property name that has since been written by somebody else.
           A SHORTHAND ASKED FOR BY NAME IS NOT COVERED AND THAT IS A RESIDUAL, NOT A HOLE: the block holds
           LONGHANDS (§6.6.1 step 8 expands), so each of them carries its own unknown and the shorthand `v`
           above is the four of them SERIALIZED — a value whose domain is the union of four. THE NEXT DIFF
           builds that join with solver/concolic.h's `concolic_new_derived` over the tainted longhands' values
           in canonical order, named for §6.7.2's serialize-a-CSS-value and given the assembled string as its
           example; it is the same joint the `border-spacing` arm of core/css/css_computed_value.c names from
           the read side. ITS ABSENCE SHOWS as `el.style.margin` answering a plain string while
           `el.style.marginTop` answers an unknown, on one block, after one `style.margin = w + 'px'`. */
        unknown = cssd_taint_read(ctx, block, name, v);

        cssd_decls_free(&d);
        r = !JS_IsUndefined(unknown) ? unknown
          : v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
        free(v);
    }
    JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, block);
    return r;
}

static JSValue js_cssd_set_property(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue block = cssd_block(ctx, this_val);
    const char *name, *priority;
    CssdValue v;
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
    /* THE NAME denotes its SHAPE when it is unknown, exactly as it does at the three members above and for the
       same reason; the VALUE is the one argument whose unknown has somewhere to GO, and cssd_value decides
       what it stores and what it leaves behind. */
    name = concolic_name_cstr(ctx, argv[0]);
    /* THE NAME IS TAKEN BEFORE THE VALUE IS RESOLVED, because step 5 parses "for property property" and a value
       cannot be parsed for a property this call could not name — and because the abort inside `cssd_value`
       names that property, which requires it to be in hand there. It used to be tested below in one condition
       with the priority conversion, which made it reachable only after the value had been resolved. */
    if (!name) {
        JS_FreeValue(ctx, block);
        return JS_EXCEPTION;
    }
    if (!cssd_value(ctx, argv[1], "setProperty", name, &v)) {
        JS_FreeCString(ctx, name);
        JS_FreeValue(ctx, block);
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
    }
    /* Web IDL §3.6 Overload resolution algorithm's ABSENT OPTIONAL ARGUMENT, in both of its spellings: a call
       that stopped short of the position arrives with a shorter argc (step 16, whose 16.2 appends "the special
       value 'missing'"), and one that reached it with `undefined` arrives with undefined in the slot (step
       15.4, "If optionality is 'optional' and V is undefined", whose 15.4.2 appends the same).
       THE SENTENCE QUOTED HERE BEFORE — "if the argument is optional and its value is undefined, it is absent"
       — APPEARS NOWHERE IN WEB IDL. It is the same fabrication idl_args.c records having found at its own
       §3.6 sites, arriving a second time in a second file, which is why the numbered steps are named here
       instead: a fabricated quotation is the one citation error that tells the reader not to open the spec.
       This member's IDL writes
       `optional CSSOMString priority = ""`, so absent IS the empty string, which is a POSITIVE statement that
       the page named no priority rather than a hole. Converting either would produce the four characters
       "undefined" and abandon the call at step 4 below. */
    /* AND THE PRIORITY IS NEITHER OF THOSE TWO ANSWERS, WHICH IS WHY IT CRASHES WHERE THE VALUE DOES NOT. It is
       not a NAME — nothing is looked up by it — and it is not a value with a home: what step 4 produces is the
       IMPORTANT FLAG, one bit on a declaration, and a bit has nowhere to carry a domain. The value's unknown is
       taken concretely BECAUSE the record above keeps its domain; taking this one concretely would decide a
       gate off an example with nothing anywhere recording that the other world was never explored, which is
       the silent loss §Offensive-programming says must crash instead. It crashes for an unknown that HAS an
       example as much as for one that has not, and that is the same rule and not a stricter one. */
    if (argc >= 3 && concolic_is(argv[2]))
        DFAILF("CSSOM §6.6.1 The CSSStyleDeclaration Interface's setProperty step 4 tests the PRIORITY argument "
               "of a write to the property `%s` against the string \"important\", and this one is UNKNOWN "
               "EXTERNAL INPUT (`%s`). The two "
               "outcomes are two worlds — the declaration is written IMPORTANT, or step 4 returns and the block "
               "is left exactly as it was — and the answer decides a BIT, which has nowhere to keep the domain "
               "the way a declaration's value does. WHAT IS MISSING is the same OUTCOME FORK the no-example "
               "value arm above names: this member as a step machine, asking quickjs-step.h's step_fork_run "
               "which completion the ASCII case-insensitive match reached. ITS ABSENCE WOULD SHOW as a page "
               "that reads its priority out of injected state (`el.style.setProperty('color', c, "
               "cfg.emphasis)`) exploring neither arm.",
               name, concolic_shape_c(argv[2]) ? concolic_shape_c(argv[2]) : "{}");
    priority = (argc >= 3 && !JS_IsUndefined(argv[2])) ? JS_ToCString(ctx, argv[2]) : NULL;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !priority) {
        JS_FreeCString(ctx, name);
        cssd_value_free(ctx, &v);
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
            cssd_value_free(ctx, &v);
            JS_FreeCString(ctx, priority);
            JS_FreeValue(ctx, block);
            return JS_UNDEFINED;
        }
    }
    /* §6.6.1's step 3: setting the empty string invokes removeProperty and returns.
       IT IS ASKED OF THE EXAMPLE WHEN THE VALUE IS AN UNKNOWN, AND THAT IS A NAMED RESIDUAL. The example is the
       value THIS FLOW computed by running the real operators on real operands, so the arm it selects is the arm
       a real session takes — right for this flow, and NARROWER than the spec, which has two feasible worlds
       here exactly as step 4 does. THE NEXT DIFF is the one the two DFAILs above name and is the same one for
       all three tests: this member as a step machine, so step 3's emptiness test, step 4's priority match and
       step 5's parse each ask quickjs-step.h's step_fork_run and both completions run. ITS ABSENCE SHOWS as a
       block that never loses a declaration to an unknown whose example happens to be "" — the sibling world in
       which the same write is a removeProperty is not explored, and nothing in the emitted surface says so. */
    cssd_write_declaration(ctx, block, name, *v.bytes ? v.bytes : NULL, important, v.taint);
    JS_FreeCString(ctx, name);
    cssd_value_free(ctx, &v);
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
    const char *pname = cssd_property_name_of((uintptr_t)magic);
    JSValue block = cssd_block(ctx, this_val), r;

    DCHECK(pname != NULL,
           "a CSS attribute was declared with a property id the SPACE does not have. `magic` indexes the "
           "registry's rows followed by this engine's own (cssd_property_name_of), so a NULL here is an "
           "installer and that seam disagreeing about which properties exist");
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "computed")) {
        r = css_resolved_value(ctx, cssd_owner_element(ctx, block), pname);
    } else {
        CssDecls d = { 0 };
        char *v;
        JSValue unknown;

        cssd_declarations_read(ctx, block, &d);
        v = cssd_property_value(&d, pname);
        unknown = cssd_taint_read(ctx, block, pname, v);

        cssd_decls_free(&d);
        r = !JS_IsUndefined(unknown) ? unknown
          : v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
        free(v);
    }
    JS_FreeValue(ctx, block);
    return r;
}

static JSValue js_cssd_property_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *pname = cssd_property_name_of((uintptr_t)magic);
    JSValue block = cssd_block(ctx, this_val);
    CssdValue v;

    DCHECK(pname != NULL,
           "a CSS attribute was declared with a property id the SPACE does not have. `magic` indexes the "
           "registry's rows followed by this engine's own (cssd_property_name_of), so a NULL here is an "
           "installer and that seam disagreeing about which properties exist");
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "readOnly")) {
        JS_FreeValue(ctx, block);
        return cssd_readonly_throw(ctx);
    }
    if (!cssd_value(ctx, val, "per-property IDL attribute setter", pname, &v)) {
        JS_FreeValue(ctx, block);
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
    }
    cssd_write_declaration(ctx, block, pname, *v.bytes ? v.bytes : NULL, false, v.taint);
    cssd_value_free(ctx, &v);
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

/* CSSOM §2 Terminology's SUPPORTED CSS PROPERTY SET, ASKED BY NAME — the same question the row predicate above
 * answers, reached from the other end, and reached through it rather than beside it so this engine has ONE
 * answer to what it supports. §6.6.1's per-property IDL attributes walk the registry and ask the predicate per
 * row; CSS Conditional Rules 3 §7.5's `CSS.supports(property, value)` holds a name and asks about that one.
 * Two entries deciding the same set could disagree, and the disagreement would read as a page bug: an
 * `el.style.width` that exists beside a `CSS.supports("width","5px")` that is false.
 *
 * THE MATCH IS ASCII CASE-INSENSITIVE, which is what §7.5 asks for ("an ASCII case-insensitive match for any
 * defined CSS property that the UA supports") and what lexbor's own lookup does — its static hash is entered
 * through the lowercasing probe, so `WIDTH` and `width` reach one row. Nothing else is normalised, which is
 * the whole of §7.5's first Note: a leading space is not whitespace to be trimmed here, it is part of the name
 * being matched, so `" width"` is in no registry row and this answers NULL.
 *
 * IT RETURNS THE REGISTRY'S OWN SPELLING rather than a boolean, because the caller's next act is a parse and
 * cssom_parse_a_css_value compares the parsed declaration's name CASE-SENSITIVELY. Handing that entry the
 * page's `WIDTH` would answer false for a property this one just said is supported — the two-entry
 * disagreement above, one call apart. NOT OWNED: it points into lexbor's static registry, which outlives every
 * caller. NULL is "not a supported CSS property", which for a CUSTOM property is the right answer and not a
 * miss: §2 excludes custom properties from the set by name, and §7.5 asks about them in its own separate
 * clause. */
/* ---- CSSOM §6.6.1's PROPERTY ID SPACE, WHICH IS THIS ENGINE'S AND NOT THE REGISTRY'S -----------------------
 *
 * §6.6.1's three per-property installers walk a SPACE rather than asking about a name they hold, and every one
 * of them walked lexbor's ids — so the thirty-nine properties this engine implements and the registry does not
 * carry had no id to be installed under, and `el.style.transform` was not an accessor at all. The space is
 * therefore the registry's ids FOLLOWED BY this engine's own, and the accessors' `magic` indexes the whole of
 * it.
 *
 * WHY IDS ABOVE THE REGISTRY'S MAXIMUM ARE SAFE HERE, WHICH IS A QUESTION ABOUT BOUNDARIES AND NOT ABOUT
 * ARITHMETIC. Lexbor's `LXB_CSS_PROPERTY__LAST_ENTRY` MOVES on a vendor bump, so every engine-side id shifts
 * with it. That is harmless for an id that lives only inside one process and FATAL for one that crosses a
 * boundary whose two ends were built at different revisions — a parked flow's snapshot, a store record, a
 * mojom field, a census row a driver parses — where it would be the stale-coordinate defect with a number on
 * it. IT IS ESTABLISHED AND NOT ASSUMED, by three greps rather than by reasoning about intent:
 *   every use of a lexbor property id in `engine/host` is in THIS FILE (`git grep -n
 *     "lxb_css_property_id\|LXB_CSS_PROPERTY__LAST_ENTRY\|property_by_id" -- engine/host`);
 *   no persisted or serialized record names one (`git grep -ni "property_id\|propId\|cssPropId" --
 *     engine/host/solver extension/lib` is EMPTY);
 *   and the only place an id reaches a JS object is as an accessor's `magic`, installed by
 *     `cssom_install_proto`, which is a `realm_declare_intrinsic` — so it is rebuilt from these tables at
 *     every realm creation and no two realms can disagree about it.
 * A later reader will re-ask this, which is why the commands are here rather than the conclusion alone.
 *
 * THE OWN LIST IS BUILT FROM THE OWNERS AND NEVER TYPED. Each of the three sources answers for the properties
 * IT owns the grammar of, exactly as `cssom_supported_css_property_named` asks them, so there is no fourth
 * list to drift: this array holds POINTERS to the owners' own static names and copies nothing. It is deduped
 * because a name in two sources would otherwise take two ids and install one attribute twice. */
static const char *g_own[CSSD_OWN_MAX];
static unsigned    g_own_n;

static void cssd_own_add(const char *name)
{
    unsigned i;

    /* A name the REGISTRY carries is not this engine's own, and admitting one would be one property with two
       ids — the registry is asked first everywhere, so the second would be unreachable and its attribute a
       silent duplicate. This file already asserts the same thing for its initial-value table. */
    if (lxb_css_property_by_name((const lxb_char_t *)name, strlen(name)) != NULL) return;
    for (i = 0; i < g_own_n; i++)
        if (strcmp(g_own[i], name) == 0) return;
    CHECK(g_own_n < CSSD_OWN_MAX,
          "cssom: this engine's own supported-property list outgrew its array. It is built from the components "
          "that own the grammars, so growing past the bound means a component gained properties rather than "
          "that the bound was wrong — raise it rather than dropping a member, because a dropped one is a "
          "property a page can declare and CSSOM says does not exist");
    g_own[g_own_n++] = name;
}

/* The engine's own supported properties, collected once per agent from the three components that own them. */
static void cssd_own_init(void)
{
    unsigned i;
    const char *n;

    g_own_n = 0;
    for (i = 0; i < sizeof(CSSD_INITIAL_UNREGISTERED) / sizeof(CSSD_INITIAL_UNREGISTERED[0]); i++)
        cssd_own_add(CSSD_INITIAL_UNREGISTERED[i].name);
    for (i = 0; i < CSS_BACKGROUND_SHORTHAND_N; i++)
        cssd_own_add(CSS_BACKGROUND_SHORTHAND_LONGHANDS[i]);
    for (i = 0; (n = css_shorthand_name_at(i)) != NULL; i++)
        cssd_own_add(n);
}

/* ONE ID, ONE NAME — the seam every consumer of the space goes through, so the registry's rows and this
   engine's own are told apart in exactly one place. NULL for an id outside the space and for a registry row
   CSSOM §2 excludes, which is what the installers' `continue` reads. */
static const char *cssd_property_name_of(uintptr_t id)
{
    if (id < LXB_CSS_PROPERTY__LAST_ENTRY) {
        const lxb_css_entry_data_t *e = lxb_css_property_by_id(id);

        return cssom_supported_css_property(id, e) ? (const char *)e->name : NULL;
    }
    DCHECK(id - LXB_CSS_PROPERTY__LAST_ENTRY < g_own_n,
           "a CSS property id was resolved past the end of this engine's own list — the space is the "
           "registry's ids followed by `g_own`, and an id past both is an installer and this seam disagreeing "
           "about how many properties there are");
    return g_own[id - LXB_CSS_PROPERTY__LAST_ENTRY];
}

/* The whole space's extent, which every §6.6.1 installer walks. */
static uintptr_t cssd_property_id_end(void)
{
    return (uintptr_t)LXB_CSS_PROPERTY__LAST_ENTRY + g_own_n;
}

const char *cssom_supported_css_property_named(const char *name)
{
    const lxb_css_entry_data_t *e;
    const char *own;
    unsigned i;

    DCHECK(name != NULL, "CSSOM §2's supported CSS property set was asked about no name at all — the empty "
                         "string is a real question with the answer NO, and the absence of a name is a caller "
                         "that never took one");
    e = lxb_css_property_by_name((const lxb_char_t *)name, strlen(name));
    if (e != NULL && cssom_supported_css_property(e->unique, e)) {
        DCHECK(e->name != NULL, "lexbor's property registry answered a supported row with no name");
        return (const char *)e->name;
    }
    /* AND THE PROPERTIES THIS ENGINE IMPLEMENTS THAT THE VENDORED REGISTRY DOES NOT CARRY, which the registry
       alone cannot answer for and which this set is NOT allowed to exclude.
       CSSOM §2 defines the term in one sentence — "The term supported CSS property refers to a CSS property
       that the user agent implements, including any vendor-prefixed properties, but excluding custom
       properties" — and the USER AGENT IS THIS ENGINE, not the parser it embeds. Reading the set off
       `lxb_css_property_by_name` alone answered for the LIBRARY instead, which is the defect class this file
       is now the worked example of: A CAPABILITY PREDICATE DERIVED FROM A VENDORED REGISTRY ANSWERS FOR THE
       LIBRARY AND NOT FOR THE ENGINE THAT EXTENDS IT. It is worth stating in those words because the
       population it got wrong is not a corner: it is every property this engine implements itself — the four
       `border-*-width`, the four `border-*-style`, `border-spacing`, `caption-side`, `table-layout`,
       `transform`, the seven `font-variant-*`, the eight background longhands — which is the same set a
       separate hunt found holding page-held abort switches, and that coincidence is a fact about the
       architecture rather than about either defect.
       WHAT IT COST WAS A SILENT WRONG ANSWER AND NOT A REFUSAL, which is why it outranks the read it also
       breaks. §7.5's setProperty asks this set BEFORE it parses anything, so a NULL here made
       `el.style.setProperty("transform", …)` return having set nothing, and CSSOM §6.6.1's per-property IDL
       attributes are installed by walking the registry BY ID, so `el.style.transform = x` was not an accessor
       at all: it created an ordinary own property that reached no declaration block, changed nothing, and READ
       BACK AS IF IT HAD WORKED. A page that writes a style and reads it back then diverges from the browser
       with NO CRASH, so the forcing function never fires and every branch behind it explores a world the page
       is not in.
       EACH SOURCE IS ASKED OF THE COMPONENT THAT OWNS IT, and there is no fourth list here: a set assembled by
       re-typing its members is the copy that drifts, and this file already asserts that a name in its own
       table which lexbor ALSO carries is one fact with two sources. The registry is still asked FIRST, so a
       property lexbor types keeps its canonical spelling from the row that types it. */
    for (i = 0; i < sizeof(CSSD_INITIAL_UNREGISTERED) / sizeof(CSSD_INITIAL_UNREGISTERED[0]); i++)
        if (strcmp(CSSD_INITIAL_UNREGISTERED[i].name, name) == 0)
            return CSSD_INITIAL_UNREGISTERED[i].name;
    /* css-backgrounds-3 §2.10's eight, whose grammars core/css/css_background_shorthand.h owns. They are NOT
       in the table above and are asked of that component instead — so a set built from that table alone would
       have been short by exactly the eight this engine most obviously implements.
       THE REASON THIS LINE USED TO GIVE FOR THEIR ABSENCE — `an initial value is a fact a property has
       whether or not anything asks for it, and nothing asks these for one` — had a second half that was
       already false when it was written: THIS LOOP is what makes a page ask. It puts all eight in the engine's own
       supported set, so §6.6.1 installs `backgroundImage` and the rest as IDL attributes, and every one of
       those reads went to `cssom_initial_value`, got NULL, and came back as the EMPTY STRING. The initial
       values are stated now — by the owner, read through `css_background_shorthand_initial` — so the set here
       and the values there come from one place and the sentence has nothing left to be right about. */
    for (i = 0; i < CSS_BACKGROUND_SHORTHAND_N; i++)
        if (strcmp(CSS_BACKGROUND_SHORTHAND_LONGHANDS[i], name) == 0)
            return CSS_BACKGROUND_SHORTHAND_LONGHANDS[i];
    /* And the SHORTHANDS core/css/css_shorthand.h expands, asked of that component rather than listed here. */
    own = css_shorthand_property_named(name);
    if (own != NULL) return own;
    return NULL;
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
/* IT TAKES THE NAME AND NOT A REGISTRY ROW, because a property this ENGINE implements has no row — and §6.6.1
   runs "For each CSS property property that is a supported CSS property" — the doubled word is the spec's own,
   the second being its variable — and CSSOM §2 defines that set as one the USER AGENT implements rather than
   one the vendored parser types. */
static void cssom_css_property_to_idl_attribute(const char *property, bool lowercase_first,
                                                char *out, size_t cap)
{
    size_t i = 0, j = 0, len = strlen(property);
    bool uppercase_next = false;

    DCHECK(len > 0, "§6.6.1's algorithm was run for a property with no name");
    DCHECK(len < cap, "a CSS property name outgrew the IDL attribute buffer — raise "
                      "CSSOM_IDL_ATTRIBUTE_MAX rather than dropping the member");
    if (lowercase_first)
        i = 1;
    for (; i < len; i++) {
        char c = property[i];

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

    for (id = 0; id < cssd_property_id_end(); id++) {
        const char *p = cssd_property_name_of(id);
        char name[CSSOM_IDL_ATTRIBUTE_MAX];

        if (p == NULL) continue;
        cssom_css_property_to_idl_attribute(p, false, name, sizeof name);
        cssom_install_property_attribute(ctx, proto, id, name);
    }
}

static void cssom_install_webkit_cased_attributes(JSContext *ctx, JSValueConst proto)
{
    static const char PREFIX[] = "-webkit-";
    uintptr_t id;

    for (id = 0; id < cssd_property_id_end(); id++) {
        const char *p = cssd_property_name_of(id);
        char name[CSSOM_IDL_ATTRIBUTE_MAX];

        if (p == NULL) continue;
        /* "and that begins with the string -webkit-" */
        if (strlen(p) < sizeof(PREFIX) - 1 || memcmp(p, PREFIX, sizeof(PREFIX) - 1) != 0) continue;
        cssom_css_property_to_idl_attribute(p, true, name, sizeof name);
        cssom_install_property_attribute(ctx, proto, id, name);
    }
}

static void cssom_install_dashed_attributes(JSContext *ctx, JSValueConst proto)
{
    uintptr_t id;

    for (id = 0; id < cssd_property_id_end(); id++) {
        const char *p = cssd_property_name_of(id);

        if (p == NULL) continue;
        /* "except for properties that have no "-" (U+002D) in the property name" — and "dashed attribute is
           property", so no algorithm runs here at all: the member's name IS the property's. */
        if (strchr(p, '-') == NULL) continue;
        cssom_install_property_attribute(ctx, proto, id, p);
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
    JSValue block = cssd_block(ctx, this_val), r, unknown;
    CssDecls d = { 0 };
    char *v;

    if (JS_IsException(block)) return block;
    DCHECK(!cssd_flag(ctx, block, "computed"),
           "a descriptor attribute was read off a COMPUTED declaration block — CSSOM §7.2's getComputedStyle "
           "is the "
           "only creator that sets that flag and it mints a CSSStyleProperties, which has no descriptor "
           "attribute for this member to have been reached through");
    cssd_declarations_read(ctx, block, &d);
    v = cssd_property_value(&d, name);
    unknown = cssd_taint_read(ctx, block, name, v);
    cssd_decls_free(&d);
    r = !JS_IsUndefined(unknown) ? unknown
      : v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
    free(v);
    JS_FreeValue(ctx, block);
    return r;
}

static JSValue js_cssd_descriptor_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const char *name = cssd_descriptor(magic);
    JSValue block = cssd_block(ctx, this_val);
    CssdValue v;

    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "readOnly")) {
        JS_FreeValue(ctx, block);
        return cssd_readonly_throw(ctx);
    }
    /* null is "" — the IDL says [LegacyNullToEmptyString] — and an unknown is what cssd_value decides. */
    if (!cssd_value(ctx, val, "descriptor IDL attribute setter", name, &v)) {
        JS_FreeValue(ctx, block);
        return JS_HasException(ctx) ? JS_EXCEPTION : JS_UNDEFINED;
    }
    cssd_write_declaration(ctx, block, name, *v.bytes ? v.bytes : NULL, false, v.taint);
    cssd_value_free(ctx, &v);
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
    CssDecls d = { 0 };
    char *out;

    (void)magic;
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "computed")) {
        JS_FreeValue(ctx, block);
        return JS_NewStringLen(ctx, "", 0);
    }
    /* §6.6's SERIALIZE A CSS DECLARATION BLOCK, over the block's declarations — which are now the store's
       where it has one. The answer is byte-identical to the projection the backing holds for every block
       whose store matched, and that is not a coincidence to rely on: it is the equality `cssd_decls_load`
       asserts, so this member and `getAttribute('style')` cannot come apart. */
    cssd_declarations_read(ctx, block, &d);
    out = cssd_serialize_decls(&d);
    cssd_decls_free(&d);
    r = out ? JS_NewString(ctx, out) : JS_NewStringLen(ctx, "", 0);
    free(out);
    JS_FreeValue(ctx, block);
    return r;
}

/* Setting it: throw when readonly, then "empty the declarations" and parse the given value into them. It goes
   through the same storage step every other write does, which is what drops an invalid declaration here
   rather than at every later read.
   THE SENTENCE THAT STOOD HERE SAID THE UNPARSED VALUE IS "what the backing then re-parses", and that is
   rewritten rather than deleted because it was true of a text backing and a reader who re-derives it from the
   attribute alone will re-state it. The parse happens HERE now, once, and what the block keeps is its result;
   the backing still receives the serialization, because §6.6's update-style-attribute step is unchanged. */
static JSValue js_cssd_set_css_text(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    JSValue block = cssd_block(ctx, this_val);
    const char *v;

    (void)magic;
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "readOnly")) {
        JS_FreeValue(ctx, block);
        return cssd_readonly_throw(ctx);
    }
    /* AN UNKNOWN cssText IS A WHOLE DECLARATION LIST AND NOT A VALUE, so it does not reach cssd_value: what
       step 3 parses out of it is N declarations, and each of them would need its own derivation over this one
       string, named for the declaration it produced. The three VALUE writes above already do exactly that per
       longhand; what is missing here is the enumeration that turns one unknown into that per-declaration list. */
    if (concolic_is(val))
        DFAILF("CSSOM §6.6.1 The CSSStyleDeclaration Interface's cssText setter was given UNKNOWN EXTERNAL "
               "INPUT (`%s`), and its step 3 is \"Parse the given value and, if the return value is not the "
               "empty list, insert the items in the list into the declarations, in specified order\" — a whole "
               "DECLARATION LIST, so there is no one property for the block's unknown record to file it under. "
               "WHAT THE NEXT DIFF BUILDS: serialize this value's own EXAMPLE the way the concrete arm below "
               "does, enumerate the declarations it produced with cssd_decls_from_text, and record for each a "
               "derivation over this string named for THAT declaration — the same composition "
               "cssd_write_declaration makes per longhand, one level up, so `margin` and `color` set by one "
               "cssText do not share a constraint key. ITS ABSENCE WOULD SHOW as this abort, and after it is "
               "built, as `el.style.cssText = '--w:' + w + 'px'` leaving `getPropertyValue('--w')` a plain "
               "string where the same write through setProperty leaves an unknown.",
               concolic_shape_c(val) ? concolic_shape_c(val) : "{}");
    v = JS_ToCString(ctx, val);
    if (!v) { JS_FreeValue(ctx, block); return JS_EXCEPTION; }
    /* §6.6.1's cssText setter step 2, "Empty the declarations" — every unknown the block recorded was about a
       declaration this write has just taken away. */
    cssd_taint_empty(ctx, block);
    /* Step 3's parse, then the ONE storage step every other write makes. It used to serialize the parse
       straight into the backing and leave the store untouched, which is the state a wholesale replacement
       must never leave: the content test would have refused the stale store anyway, so this is not a
       correctness fix but it is the difference between a block that CARRIES its declarations after a
       `cssText =` and one that has to re-parse them at the very next read. */
    {
        CssDecls d = { 0 };

        cssd_decls_from_text(v, strlen(v), &d);
        cssd_declarations_put(ctx, block, &d);
        cssd_decls_free(&d);
    }
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
    unsigned i;

    /* THE SAME QUESTION §6.6.1's installers ask, asked THROUGH THE SAME PREDICATE. This walk had its own
       spelling of it — a start index and a null/empty guard — which let lexbor's custom-property marker
       through to be filtered by whether css_computed_models happens to model a property called `#сustom`.
       One "supported CSS property" question with two answers is how the two sites drift apart. */
    for (id = 0; id < cssd_property_id_end(); id++) {
        const char *p = cssd_property_name_of(id);

        if (p == NULL) continue;
        cssd_computed_name(d, p);
    }
    /* THE HAND-ADDED BORDER LONGHANDS ARE GONE, AND THEIR ARGUMENT IS RETIRED RATHER THAN DELETED. A loop here
       walked `border-width` and `border-style`'s longhands by hand, because "lexbor's registry does not carry"
       the eight and the walk above could therefore not reach them — which was true of a walk over the
       REGISTRY'S ids and is false of one over this engine's own space. Keeping it would not be a harmless
       belt-and-braces: `cssd_computed_name` appends unconditionally, so every one of the eight would be
       enumerated TWICE and CSSOM §7.2's `length` and `item(i)` would both report the duplicates.
       IT IS THE SAME FACT THE SPACE ABOVE EXISTS FOR, which is why the compensation and the gap it compensated
       for had to go in one diff: a site that works around an absence is falsified by the diff that ends the
       absence, and a reader who finds it still standing will re-derive the reason it was there. */
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
    if (cssd_flag(ctx, block, "computed")) { cssd_computed_names(out); return; }
    cssd_declarations_read(ctx, block, out);
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

   THE INDEX IS `unsigned long`, AND THE ARRAY'S OWN LENGTH IS THEREFORE THE ONLY BOUND. §6.6.1 writes `getter
   CSSOMString item(unsigned long index)`; this was declared `long`, and under that declaration the `idx >= 0`
   half of the guard was LOAD-BEARING rather than defensive — Web IDL §3.2.4.5 long converts with §3.2.4.9
   Abstract operations' ConvertToInt(V, 32, "signed"), whose final step is "If signedness is 'signed' and x ≥
   2^(bitLength−1), then return x − 2^bitLength", so the converted value reaches −2147483648, and `d.v` is a
   REAL ARRAY of exactly `d.n` CssDecl entries. Deleting that half without fixing the type is an out-of-bounds
   READ, which is why the type and the guard move in ONE diff and neither is a separate step. §3.2.4.6 unsigned
   long's ConvertToInt(V, 32, "unsigned") produces [0, 2**32−1] and `d.n` is an `unsigned`, so `i < d.n` is a
   single unsigned comparison covering the whole converted range: the negative arm is unreachable BY TYPE, not
   by a check, and there is nothing left for a second test to catch.
   THE COMPENSATION IS WHY THE WRONG TYPE SURVIVED: a declaration block cannot hold 2**31 declarations, so
   every value at or past 2**31 is past the end under either sign and the empty string is the answer both ways
   — the declaration's error had no discriminating input through this member. The declaration is the spec of
   the conversion; a body re-deriving the sign is the second copy of §3.2.4.9 Abstract operations' arithmetic
   that idl_args.c exists to prevent. */

/* THE PROPERTY NAME OF THE CSS DECLARATION AT A POSITION — §6.6.1's "the property name of the CSS declaration
   at position index", and JS_UNDEFINED when the declarations are not that long. One implementation for the
   same reason cssd_count is one: §6.6.1 gives this walk TWO readings that must never disagree — the indexed
   property getter, where absence IS "not a supported property index", and the OPERATION, whose own sentence
   turns that absence into the empty string. It answers in the GETTER's spelling and the operation folds it,
   because idl_indexed.c's contract is the one that cannot be restated: an empty string from a backing is a
   property whose value is "", so `3 in el.style` would answer true on a three-declaration block.
   THE BLOCK IS OPENED HERE AND NOT PASSED IN, which is what keeps a CssDecls off the member's own frame. That
   array is a C allocation the step state does not name and cannot park with, so a member that held one across
   a fork would be holding it across a snapshot, an eviction and a cross-session resume. */
static JSValue cssd_name_at(JSContext *ctx, JSValueConst self, uint32_t i)
{
    JSValue block = cssd_block(ctx, self), r;
    CssDecls d = { 0 };

    DCHECK(!JS_IsException(block),
           "a CSS declaration at a position was asked for on an object with no declaration block — every "
           "caller of this establishes the brand first, the getter by the decl that only cssd_new attaches and "
           "the member by its own cssd_block, so a throw here is a caller that did neither");
    cssd_declared_decls(ctx, block, &d);
    r = i < d.n ? JS_NewString(ctx, d.v[i].name) : JS_UNDEFINED;
    cssd_decls_free(&d);
    JS_FreeValue(ctx, block);
    return r;
}

/* IT IS A STEP MACHINE BECAUSE ITS ONE ARGUMENT CAN BE UNKNOWN, and the DCHECK that used to refuse an unknown
 * index into a NON-EMPTY block is gone with the arm it guarded. That check was honest and was still this
 * member answering nothing: `el.style.item(location.hash.length)` on any element carrying a `style` attribute
 * took the document down. core/idl_index_arg.h's elimination chain is the answer, and asking it is what a
 * plain C activation has nowhere to park for.
 *
 * ITS PAST-THE-END ANSWER IS THE EMPTY STRING AND NOT NULL, WHICH CHANGES THE VALUE AND NOT THE QUESTION.
 * §6.6.1: "If there is no indexth object in the collection, then the method must return the empty string" —
 * where every `item(index)` beside it in that family answers null. The chain answers with a FLAG for exactly
 * this reason: what the past-the-end world IS belongs to the algorithm and is stated at the caller, so one
 * component does not decide every member's answer from one place.
 *
 * NOTHING IS HELD ACROSS THE FORK. `npositions` is read through cssd_count, whose CssDecls dies inside it, and
 * the name is fetched through cssd_name_at only once the chain has answered — so the C array this member's
 * body used to build around the index never spans a park. Re-reading the block on the far side is not a second
 * source of truth: idl_index_chain_run runs none of the page's code, so the count a resumed entry takes is the
 * count the link was drawn against, and a page that HAS mutated the block between two entries is answered by
 * the chain's own soundness — a flow that eliminated 0..k-1 has established `index >= k` in any world. */
#define CSSD_ITEM_ALGORITHM "CSSOM §6.6.1 The CSSStyleDeclaration Interface item(index)"
#define CSSD_ITEM_STAGES(X)                                                                                   \
    X(CSSD_ITEM_READ, CSSD_ITEM_ALGORITHM " (the property name of the CSS declaration at position index, or "  \
                                          "the empty string when there is no indexth object)")
enum { IDL_STEP_STAGE_BASE(CSSD_ITEM_STAGES) CSSD_ITEM_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CSSD_ITEM_STEPS[] = { CSSD_ITEM_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_cssd_item(JSContext *ctx, JSStepHdr *hdr, void *state, int argc, JSValueConst *argv,
                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    IdlIndexChain *s = state;
    JSValue block, r;
    uint32_t i = 0, n;
    bool past_end = false;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);   /* this machine makes no request that delivers a value */
    *presult = JS_UNDEFINED;
    DCHECK(hdr->stage == CSSD_ITEM_READ,
           "§6.6.1's item(index) resumed into a stage the algorithm does not have — it is ONE sentence, and "
           "the chain of questions it may ask is a cursor on this machine's own state rather than a stage "
           "apiece");
    DCHECK(argc == 1,
           "§6.6.1's item(index) reached its body with an argument count its declaration does not produce — "
           "`index` is REQUIRED, so §3.6's argument-count check refuses a bare item() before this body runs");
    /* Web IDL §3.7.7 Operations' BRAND CHECK, which is this member's and runs on every entry: `el.style.item`
       called on a plain object throws, and a resumed entry re-establishes it rather than trusting the one
       before. */
    block = cssd_block(ctx, hdr->this_val);
    if (JS_IsException(block))
        return JS_STEP_ABRUPT;
    n = cssd_count(ctx, block);
    JS_FreeValue(ctx, block);
    if (concolic_is(argv[0])) {
        int rc = idl_index_chain_run(ctx, hdr, s, argv[0], n, CSSD_ITEM_ALGORITHM, &i, &past_end);
        if (rc)
            return rc;   /* parked at the fork */
        if (past_end) {
            *presult = JS_NewStringLen(ctx, "", 0);   /* §6.6.1's own answer for no indexth object */
            return JS_STEP_DONE;
        }
    } else {
        /* The declaration ran §3.2.4.6 unsigned long's ConvertToInt(V, 32, "unsigned"), which is §3.2.4.9
           Abstract operations' modulo and not a clamp — `el.style.item(2**32)` is declaration 0. */
        i = idl_index_arg_known(ctx, argv[0], CSSD_ITEM_ALGORITHM);
    }
    r = cssd_name_at(ctx, hdr->this_val, i);
    *presult = JS_IsUndefined(r) ? JS_NewStringLen(ctx, "", 0) : r;
    return JS_STEP_DONE;
}

static const IdlStepDecl CSSD_ITEM_DECL = {
    js_cssd_item, sizeof(IdlIndexChain), idl_index_chain_visit, NULL,
    CSSD_ITEM_ALGORITHM, CSSD_ITEM_STEPS, 0, NULL
};

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
 * different ones, and the elimination chain js_cssd_item runs is owed by the member alone. That clause used to
 * name a fork that a DCHECK named instead, which was true while the member REFUSED an unknown index rather
 * than forking it, and the refusal is gone. The same split dom_token_list.c records between its tl_item and
 * js_tl_item. */
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
   empty string — see the banner. It is cssd_name_at's own answer, which is why this is one line: the bound is
   one unsigned comparison there for the reason js_cssd_item records (Web IDL §3.2.4.6 unsigned long converts
   to [0, 2**32−1] and `d.n` is an `unsigned`, so a negative index is unreachable BY TYPE), and idl_indexed.c's
   own array-index-property-name parse has already refused `"-1"`, `"01"` and `"1.0"` before this is reached. */
static JSValue cssd_indexed_item(JSContext *ctx, JSValueConst self, uint32_t i)
{
    return cssd_name_at(ctx, self, i);
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

/* CSSOM §7.2 Extensions to the Window Interface's getComputedStyle(elt, pseudoElt), whose step 6 returns "a
   live CSSStyleProperties object" with the computed flag set, the readonly flag set, the parent CSS rule null
   and the owner node `obj`.
   §7.2 HAS NO THROW ANYWHERE IN IT, AND THIS MEMBER HAD ONE. What stood here answered `NotSupportedError` for
   EVERY non-empty pseudoElt, and the six steps contain no such step: step 3 is entered only "If pseudoElt is
   provided, is not the empty string, AND STARTS WITH A COLON", and its 3.2 handles a pseudo this UA cannot
   resolve by making `obj` null rather than by throwing — "If type is failure, or is a ::slotted() or ::part()
   pseudo-element, let obj be null". Three spellings a browser answers were therefore refused: an explicit
   `null` (the declared type was `IDL_DOMSTRING`, not the IDL's `CSSOMString?`, so it arrived as the four
   characters "null" and was non-empty), any COLONLESS string (`getComputedStyle(el, "foo")` is step 3's
   condition unmet, so `obj` stays `elt`), and every call whose second argument a page computed. A fabricated
   refusal is worse than a missing capability: the page cannot tell it from a real UA answer, and no gate here
   can see a member inventing a behaviour its section does not contain.
   WHAT IS GENUINELY MISSING IS STEP 3'S TWO HALVES and they crash by name below. */
static JSValue js_get_computed_style(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_node_t *n;
    const char *pseudo = NULL;   /* §7.2 step 3's bytes; NULL when the argument is absent or the IDL null */

    (void)magic; (void)this_val;
    /* `elt` is required and its type is `Element`, and BOTH are the declaration's: §3.6 step 5 throws for a
       call with no argument before any body is entered, and §3.2.15's brand test — the node class narrowed to
       an element — throws for anything that is not one. The body's copies of both were the brand written out
       by hand that a declared type exists to replace. */
    DCHECK(argc >= 1, "getComputedStyle's body ran with no `elt` — §3.6 step 5's required-argument check is the "
                      "declaration's and it did not run");
    n = node_of(argv[0]);
    DCHECK(n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT,
           "getComputedStyle's `elt` reached its body as something that is not an element — the declaration "
           "brands it against the node class and narrows it with element_is, so §3.2.15 owes the TypeError");
    /* §7.2 STEP 3'S CONDITION IS OVER BYTES, SO IT IS ASKED OF BYTES — never of the argument's TAG. The test
       that stood here was `JS_IsString(argv[1])`, and a concolic fails it BY CONSTRUCTION rather than by
       anything about the value: unknown external input crosses a `CSSOMString?` position AS ITSELF
       (core/idl_args.h's idl_concolic_rule), so `getComputedStyle(el, cfg.pseudo)` was not a string, took the
       no-pseudo arm, and answered the element's own style with nothing anywhere saying a condition had been
       decided by a type test instead of by the value. That is the silent half of §RUN-DON'T-MATCH: an arm
       chosen for a reason that is not about the page's value at all.
       ABSENCE AND NULL ARE THE DECLARATION'S ANSWER AND ARE ASKED AS SUCH. The position is `optional
       CSSOMString? pseudoElt`, so IDL_DOMSTRING_NULLABLE makes Web IDL §3.2.20's null rule convert an explicit
       `null` AND an explicit `undefined` to the IDL null before any ToString — and a concolic is neither, which
       is exactly why THESE two tests are sound where a tag test is not: they ask what the conversion produced,
       not what kind of thing the page wrote. */
    if (argc > 1 && !JS_IsNull(argv[1])) {
        JSValue example = concolic_is(argv[1]) ? concolic_example(ctx, argv[1]) : JS_UNDEFINED;

        /* AN UNKNOWN WITH NO EXAMPLE CANNOT BE ASKED STEP 3'S CONDITION AT ALL, and both of its answers are
           feasible worlds this member's steps tell apart. */
        if (concolic_is(argv[1]) && JS_IsUndefined(example)) {
            const char *shape = concolic_shape_c(argv[1]);

            JS_FreeValue(ctx, example);
            DFAILF("CSSOM §7.2 Extensions to the Window Interface's getComputedStyle was given a `pseudoElt` "
                   "that is UNKNOWN EXTERNAL INPUT WITH NO EXAMPLE (`%s`), and step 3's condition — "
                   "\"provided, is not the empty string, and starts with a colon\" — is a question about its "
                   "BYTES. Both answers are feasible and they are two different objects: step 3 unentered "
                   "leaves `obj` as `elt` and step 6 returns the ELEMENT's computed style, while step 3 entered "
                   "makes `obj` a pseudo-element or null. WHAT IS MISSING is the OUTCOME FORK at this member's "
                   "own seam — it is a plain C body, so building it means making it a step machine and asking "
                   "quickjs-step.h's step_fork_run which completion the condition reached, exactly as "
                   "§6.6.1's three value tests in this file need. ITS ABSENCE WOULD SHOW as a page reading its "
                   "pseudo out of injected state (`getComputedStyle(el, cfg.pseudo)`) exploring neither world.",
                   shape ? shape : "{}");
        }
        DCHECK(!JS_IsObject(example),
               "a `pseudoElt`'s unknown carries an OBJECT as its concrete example — an example is the value "
               "this engine COMPUTED, so it is a primitive, and §7.1.19 ToString over an object one runs the "
               "PAGE's toString from a C activation with no flow base under it");
        /* THE EXAMPLE IS THIS FLOW'S OWN ANSWER TO STEP 3, AND THAT IS A NAMED RESIDUAL. It is the value the
           run computed by running the real operators on real operands, so the arm it selects is the arm a real
           session takes — right for this flow, NARROWER than the spec, and the sibling world is what the fork
           above builds. ITS ABSENCE SHOWS as a page whose computed pseudo happens to be empty never exploring
           the world in which it names a pseudo-element. */
        pseudo = JS_IsUndefined(example) ? JS_ToCString(ctx, argv[1]) : JS_ToCString(ctx, example);
        JS_FreeValue(ctx, example);
        if (!pseudo) return JS_EXCEPTION;
    }
    /* CSSOM §7.2's getComputedStyle, STEP 3, ENTERED. Its two halves are both absent from this engine and
       neither may be faked: step 3.1's parse and step 3.3's pseudo-element. Claiming step 3.2's `failure`
       for a pseudo that PARSES would report an empty block as this UA's answer for `::before`, which is the
       invented-value defect one layer up from the throw this replaces. RELEASE FALLS THROUGH to the
       element's own style, which is what every other unbuilt arm in this component does
       (core/css/css_computed_value.c's inset arm is the same shape) — a dev build cannot reach it, and
       release adds no capability. */
    if (pseudo && *pseudo == ':')
        DFAIL("CSSOM §7.2 Extensions to the Window Interface's getComputedStyle entered step 3 — its "
              "`pseudoElt` is provided, is not the empty string, and starts with a colon — and BOTH of that "
              "step's substeps are unbuilt here. 3.1 is \"Parse pseudoElt as a <pseudo-element-selector>, and "
              "let type be the result\", which this engine has no parser for: core/dom/selector_match.c owns "
              "the agent's one selector matcher and answers about ELEMENTS, and nothing in core/css parses a "
              "pseudo-element selector into a type. 3.3 is \"Otherwise let obj be the given pseudo-element of "
              "elt\", and there is no pseudo-element box in this engine to be given — core/layout names "
              "pseudo-elements only inside the spec sentences it quotes and builds one nowhere. BUILD 3.1 "
              "FIRST, as its own component beside the "
              "selector matcher, because it is what makes 3.2's \"If type is failure, or is a ::slotted() or "
              "::part() pseudo-element, let obj be null\" answerable — and that arm needs no boxes at all: it "
              "is a computed block whose declarations are EMPTY, which step 5's \"If obj is not null\" is what "
              "produces. Only then does 3.3 need the box tree. WHAT MUST NOT HAPPEN IS THE THROW THAT STOOD "
              "HERE: §7.2 contains no throw, so `NotSupportedError` was a behaviour this member invented and a "
              "page cannot tell from a real one");
    /* ONE FREE, AFTER THE TEST AND NOT INSIDE IT. A `JS_FreeCString` in the arm above would run TWICE in a
       release build — the DFAIL is compiled out there, so the arm falls through to this line — which is the
       shape §Offensive-programming warns of from the other side: a dev-only abort is what was holding an
       ownership invariant together, and the release build is where that shows. */
    if (pseudo) JS_FreeCString(ctx, pseudo);
    {
        JSValue proto = cssd_proto(ctx), out = cssd_new(ctx, proto, argv[0], JS_NULL, true, true);

        JS_FreeValue(ctx, proto);
        return out;
    }
}

void cssom_init(JSContext *ctx)
{
    uintptr_t id;
    static const char DECLARATION_PROTO[] = "CSSOM §6.6.1 CSSStyleDeclaration.prototype";
    static const char FONT_FACE_PROTO[]   = "CSS Fonts 5 §9.1 CSSFontFaceDescriptors.prototype";
    static const char PAGE_PROTO[]        = "CSSOM §6.4.7 CSSPageDescriptors.prototype";

    DCHECK(!g_ready, "cssom_init ran twice — one instance is one document");
    /* The shorthand table's own invariants, asserted before anything reads it — §6.6's serialization walks it
       in both directions and the cascade walks it in one, so a row that disagrees with itself is a wrong
       string and a wrong computed value at once. */
    css_shorthand_init();
    /* @LOGICAL — css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings"' table and css-logical-1 §4's
       group table, checked here for the same reason the two beside it are: both are read by a scan that
       stops at the first match, and the §6.4 one must additionally be a PERMUTATION in every column or its
       inverse — which is the direction a `margin-top` query takes — silently answers the wrong side. */
    css_logical_init();
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
        agent_state_class("element", &g_cssd_class,
                          "CSSOM §6.6.1 \"The CSSStyleDeclaration Interface\"'s per-realm prototype-holder class");
    }
    /* THE TWO HALVES ASK DIFFERENT QUESTIONS AND THEREFORE NO LONGER SHARE A SPELLING, WHICH IS A RETIRED
       ARGUMENT AND NOT A REGRESSION. Each slot used to hand ONE sentence to both, for the stated reason that "the
       same sentence typed twice on two adjacent lines is a fact kept in step by whoever remembers" — true of
       a DESCRIPTION, which is what both halves then wanted. It stopped being true when the realm half became
       a class BRAND: core/realm.h's realm_proto_declare names the object for §20.1.3.6 and for the constraint
       key `%CSSStyleDeclaration.prototype%`, and core/agent_state.h names the SLOT for the assert a forgotten
       release fires. One is an interface identifier and the other is a citation; a single spelling can only
       be wrong for one of them, and it was — the citation went into `class_name` and aborted a real page.
       The clerical risk the old note names is real and is answered by the door instead: a description reaching
       realm_proto_declare is refused there by name, so the two cannot be swapped silently.
       THE NAMES ARE VERIFIED AGAINST THE MAINTAINED EDITIONS AND NOT AGAINST RECALL: CSSOM §6.6.1 "The
       CSSStyleDeclaration Interface" declares `interface CSSStyleDeclaration`; CSSOM §6.4.7 "The CSSPageRule
       Interface" declares `interface CSSPageDescriptors : CSSStyleDeclaration` — the section is titled for
       the RULE and defines the descriptors interface inside it, which is why the citation beside it names a
       section whose title is another interface's and is right anyway; CSS Fonts 5 §9.1 "The CSSFontFaceRule
       interface" declares `interface CSSFontFaceDescriptors : CSSStyleDeclaration`, the same arrangement. */
    g_declaration_proto_slot = realm_proto_declare(ctx, "CSSStyleDeclaration");
    agent_state_realm_slot("element", &g_declaration_proto_slot, DECLARATION_PROTO);
    g_font_face_proto_slot = realm_proto_declare(ctx, "CSSFontFaceDescriptors");
    agent_state_realm_slot("element", &g_font_face_proto_slot, FONT_FACE_PROTO);
    g_page_proto_slot = realm_proto_declare(ctx, "CSSPageDescriptors");
    agent_state_realm_slot("element", &g_page_proto_slot, PAGE_PROTO);
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
        /* §6.6.1's `item` IS A MACHINE — a declaration and not a dispatch, since there is no second body for
           anything to select against. `index` is REQUIRED, and it can be unknown external input. */
        g_item_id = idl_method_id_step(ctx, ONE_ULONG, 1, NULL, 0, &CSSD_ITEM_DECL, 0);
        {
            /* CSSOM §7.2 Extensions to the Window Interface: `getComputedStyle(elt, optional pseudoElt)`.
               DECLARED HERE with the rest — the member lives on the WINDOW, which is per realm, and the
               declaration is the agent's. The number here was CSSOM §7.1, which is The ElementCSSInlineStyle
               Mixin: the sentence beside it already said WINDOW, and every other getComputedStyle citation in
               this file already said CSSOM §7.2. */
            /* `Element elt` IS A DECLARED TYPE and it was IDL_ANY, so §3.2.15's brand test was the body's
               own `node_of` plus a node-type test. Every DOM node wrapper is one class, so the class says
               only "a Node" and idl_iface_narrow(element_is) is what says which kind — the pairing
               core/dom/shadow_root.h states and slot.c, element_internals.c and intersection_observer.c
               already use. */
            /* `optional CSSOMString? pseudoElt` — NULLABLE, and it was declared IDL_DOMSTRING. Web IDL
               §3.2.20's null rule converts an explicit `null` and an explicit `undefined` to the IDL null
               before ToString is ever reached, so the non-nullable spelling handed the body the four
               characters "null" and `getComputedStyle(el, null)` — which every browser answers — was refused
               as a pseudo-element. It is the same load-bearing distinction core/idl_args.h records for
               `textContent`, arriving in a second member. */
            static const IdlArgType TWO[2] = { IDL_INTERFACE, IDL_DOMSTRING_NULLABLE };
            g_id_gcs = idl_method_id(ctx, TWO, 2, js_get_computed_style, 0);
            idl_optional_from(1);
            idl_iface_brand(node_class_id());
            idl_iface_narrow(element_is);
        }
    }
    /* §6.6.1 declares all three per-property spellings `[CEReactions] attribute [LegacyNullToEmptyString]
       CSSOMString`, so `el.style.color = null` REMOVES the declaration exactly as `""` does. This declared them
       with null_to_empty FALSE, which made `null` reach the setter as the four-character string "null" and
       declare `color: null` — the descriptors beside them had it right, which is what made the disagreement
       readable. One id per property, shared by the property's camel-cased, webkit-cased and dashed attributes,
       because §6.6.1 gives all three the same setter steps. */
    /* THE OWN LIST IS BUILT BEFORE THE SETTERS ARE DECLARED, because the space's extent is what this loop
       walks and `cssd_property_id_end` is that extent. Both are per AGENT — a setter id is a runtime's, and
       the list is collected from tables that do not change while one runs. */
    cssd_own_init();
    for (id = 0; id < cssd_property_id_end(); id++)
        g_property_set_id[id] = cssd_property_name_of(id) != NULL
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
    /* THE PARSE TABLE GOES BEFORE THE PARSER, because dropping a slot asserts that the parser is not
       pointing at the arena it is about to free — a check that reads `g_parser` and would read it out of
       freed memory if the parser had gone first. */
    cssd_sheet_parse_free();
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
    /* THE CLASS ID AND THE THREE PROTOTYPE SLOTS ARE NOT RESET HERE. All four are declared under
       `element`, whose release ends in agent_state_undo — one reset, computed from the registry that
       already holds their addresses and their kinds, rather than a second list kept in step with the
       declarations three hundred lines above by whoever remembered. What stays is every REFERENCE and the
       null that guards a free: the undo resets HANDLES and never references, so the two key values, the
       parse table, the selector state and the parser keep their own reset beside them.
       AND THE CASCADE REACHED THIS FILE, which is the claim that entitles element_free's last line to put
       the four back: element_free calls cssom_free, and this says so. See core/agent_state.h. */
    agent_state_reached("element");
}
