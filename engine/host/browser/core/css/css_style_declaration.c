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
 * THE CASCADE IS RESOLVED LIVE, per read, from the RUNNING FLOW'S TREE: inline, then the author rules in that
 * flow's own `<style>` elements matched with lxb_selectors_match_node, then the UA default, then the property's
 * initial value. Nothing is cached across a read, because a cache would be shared state that the flow machinery
 * does not swap — the same reason the storage is the attribute.
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

#include <lexbor/css/css.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_slots.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/dom/selector_match.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_rule.h"
#include "core/css/css_shorthand.h"
#include "core/css/css_style_declaration.h"
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
 * `cssd_author_value` was reached from ONE call chain, so the stale address was re-materialised as the same
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

/* THE VALUE THIS DECLARATION GIVES TO `name`, or NULL when it gives it none. A declaration sets a longhand
   either by BEING it or by being a SHORTHAND of it — CSS Cascade §Shorthand Properties makes a shorthand
   declaration set every longhand it names, and the cascade is over longhands only. Every layer below asks
   through here, so `overflow: hidden` is read by `el.style.overflowX`, by the author cascade and by
   getComputedStyle identically rather than by whichever of them remembered to expand it. OWNED. */
static char *cssd_decl_value_for(const lxb_css_rule_declaration_t *d, const char *name)
{
    char *dname, *value, *out;

    /* CSS Syntax's INVALID DECLARATION, dropped. Lexbor keeps one in the list as a `__UNDEF` holding the
       property id and the RAW UNPARSED TOKENS, so that a serializer can round-trip the block it came from —
       and the cascade that read it back without this line handed those tokens on as if they were a value:
       `display: bogus` won the cascade and `getComputedStyle(el).display` answered "bogus", a string no
       property's grammar admits. The one from `declarations_bad` does not even carry a name. */
    if (d->type == LXB_CSS_PROPERTY__UNDEF) return NULL;
    dname = cssd_decl_name(d);
    if (!dname) return NULL;
    if (strcmp(dname, name) == 0) {
        free(dname);
        value = cssd_decl_value(d);
        /* THE DECLARATION IS THE LONGHAND, and its value has been through a grammar only if lexbor's registry
           TYPES it. For the properties it does not carry, css_shorthand.h owns that grammar — the same one its
           shorthand expansion applies to each component — and an invalid value is a DROPPED declaration. */
        if (!value || !css_shorthand_validates_longhand(name)) return value;
        out = css_shorthand_longhand_value(name, value);
        free(value);
        return out;
    }
    value = cssd_decl_value(d);
    out = value ? css_shorthand_component(dname, value, name) : NULL;
    free(value);
    free(dname);
    return out;
}

/* The element's inline `style` attribute, as text. BORROWED from Lexbor's own storage. */
static const char *cssd_inline_text(lxb_dom_element_t *el, size_t *plen)
{
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"style", 5, plen);
    return (const char *)v;
}

/* THE VALUE A DECLARATION BLOCK'S TEXT GIVES `name`, and whether that declaration carries `!important`. NULL
   when the block declares it nowhere. This is what every reader of a declaration block goes through — the
   cascade's inline layer below, and §6.6.1's `getPropertyValue`/`getPropertyPriority` over either backing —
   because the two differ in WHERE the text is kept and in nothing else. OWNED. */
static char *cssd_value_in_block(const char *text, size_t len, const char *name, bool *pimportant)
{
    lxb_css_memory_t *mem = NULL;
    lxb_css_rule_declaration_list_t *list;
    lxb_css_rule_t *r;
    char *out = NULL;

    if (!text || !len) return NULL;
    list = cssd_parse_block(text, len, &mem);
    if (list) {
        for (r = list->first; r; r = r->next) {
            lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(r);
            char *v;
            if (r->type != LXB_CSS_RULE_DECLARATION) continue;
            v = cssd_decl_value_for(d, name);
            if (!v) continue;
            free(out);
            out = v;
            if (pimportant) *pimportant = d->important;
        }
    }
    if (mem) lxb_css_memory_destroy(mem, true);
    return out;   /* the LAST wins, which is what a declaration block means */
}

/* LAYER 1 — the INLINE declaration for `name`, or NULL. The highest-weight layer short of !important, and the
   one that is per-flow because the attribute it reads is. The CASCADE reaches it with an element and no
   declaration object, which is why this takes one rather than a block. */
static char *cssd_inline_value(lxb_dom_element_t *el, const char *name, bool *pimportant)
{
    size_t len = 0;
    const char *text = cssd_inline_text(el, &len);

    return cssd_value_in_block(text, len, name, pimportant);
}

/* LAYER 2 — the AUTHOR cascade, resolved from the RUNNING FLOW'S tree. Every `<style>` element in the document
   is parsed, every style rule's selector is matched against THIS element with Lexbor's own matcher, and the
   winner is the highest specificity, later document order breaking a tie — which is the cascade. It is redone
   per read on purpose: a cache would be shared state the flow machinery does not swap, so a `<style>` one arm
   injected would decide another arm's computed values.
   THE MATCH ITSELF IS core/dom/selector_match.c's, which is where the agent's one lxb_selectors_t lives. This
   file held a second one, so "does this element match this selector" had two implementations that could
   disagree — and the arena is scratch either way: the match cleans its own pools before it returns. */

static char *cssd_author_value(lxb_dom_element_t *el, const char *name)
{
    lxb_dom_node_t *self = lxb_dom_interface_node(el), *root, *n;
    lxb_css_selector_specificity_t best_spec = 0;
    bool best_important = false, have = false;
    char *out = NULL;

    for (root = self; root->parent; root = root->parent) { }
    for (n = root; n; ) {
        lxb_char_t *text;
        size_t tlen = 0;
        size_t nlen = 0;
        const lxb_char_t *tag;

        tag = (n->type == LXB_DOM_NODE_TYPE_ELEMENT)
                  ? lxb_dom_element_local_name(lxb_dom_interface_element(n), &nlen) : NULL;
        if (tag && nlen == 5 && memcmp(tag, "style", 5) == 0 &&
            (text = lxb_dom_node_text_content(n, &tlen)) != NULL) {
            lxb_css_memory_t *smem = lxb_css_memory_create();
            lxb_css_stylesheet_t *sst = NULL;
            if (smem && lxb_css_memory_init(smem, 128) == LXB_STATUS_OK) {
                sst = lxb_css_stylesheet_create(smem);
                lxb_css_parser_memory_set(g_parser, smem);
                if (sst && lxb_css_stylesheet_parse(sst, g_parser, text, tlen) != LXB_STATUS_OK) sst = NULL;
                lxb_css_parser_memory_set(g_parser, NULL);
                cssd_selectors_intact();
            }
            if (sst && sst->root && sst->root->type == LXB_CSS_RULE_LIST) {
                lxb_css_rule_t *r;
                for (r = lxb_css_rule_list(sst->root)->first; r; r = r->next) {
                    lxb_css_rule_style_t *st = lxb_css_rule_style(r);
                    lxb_css_selector_specificity_t spec = 0;
                    lxb_css_rule_t *dr;
                    if (r->type != LXB_CSS_RULE_STYLE || !st->selector || !st->declarations) continue;
                    if (!selector_match_node(self, st->selector, &spec)) continue;
                    for (dr = st->declarations->first; dr; dr = dr->next) {
                        lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(dr);
                        char *v;
                        bool wins;
                        if (dr->type != LXB_CSS_RULE_DECLARATION) continue;
                        v = cssd_decl_value_for(d, name);
                        if (!v) continue;
                        /* THE CASCADE, in the order §6.4 states it: IMPORTANCE first, then SPECIFICITY, then
                           document ORDER. Order alone is what this compared at first, which is wrong in the
                           most ordinary way there is — `#main { display:block }` written before
                           `div { display:none }` would have lost, and every page puts its general rules last.
                           selector_match_node reports the specificity that matched; throwing it away and
                           calling document order "the cascade" was skipping the subproblem. */
                        wins = !have
                            || (d->important && !best_important)
                            || (d->important == best_important && spec >= best_spec);
                        if (!wins) { free(v); continue; }
                        free(out);
                        out = v;
                        best_spec = spec;
                        best_important = d->important;
                        have = true;
                    }
                }
            }
            /* THE ARENA IS THE ONE OWNER, and it is freed outright. This used to read
               `lxb_css_stylesheet_destroy(sst, true)` under a comment saying that "takes its arena with it",
               and it does not: `lxb_css_stylesheet_create` REF-INCREMENTS the memory it is handed (to 2, since
               `lxb_css_memory_init` starts it at 1) and that destroy only ref-DECREMENTS (back to 1), so the
               arena — the stylesheet, every rule, every selector and every string in it — was never freed at
               all. One leaked arena per `<style>` element per read, which the ancestor walks then multiplied by
               the depth of the chain. The stylesheet, its rules and its selectors are all allocated FROM this
               arena, so destroying the arena is what releases them; nothing here outlives it (every value this
               function keeps is copied out with strdup). */
            if (smem) lxb_css_memory_destroy(smem, true);
            lxb_dom_document_destroy_text(n->owner_document, text);
        }
        /* A pre-order walk over the flow's own tree, with an explicit cursor — the same reason every walk in
           node.c has one: the depth is the page's, so a recursive one is an unbounded C stack. */
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
    return out;
}

/* ---- CSS Syntax's "parse a stylesheet's contents", for CSSOM §6.4's rule objects --------------------------
 *
 * §6.6's SERIALIZE A CSS DECLARATION, and the BLOCK serialization its entries are joined into with a single
 * SPACE. The spec's note is the exact shape — "no whitespace appears before the first property name and no
 * whitespace appears after the final semicolon delimiter" — and it is NOT lexbor's serialization, which joins
 * with "; " and emits no trailing semicolon at all. A CSSOM member must answer CSSOM's string.
 *
 * WHAT IS NOT HERE IS THE SHORTHAND CONSOLIDATION LOOP. §6.6's algorithm re-consolidates a full set of
 * longhands back into the shorthand that covers them (`margin-top`, `-right`, `-bottom`, `-left` all present
 * serialize as one `margin`), which needs the LONGHAND -> SHORTHAND direction and the shorthands' preferred
 * order; core/css/css_shorthand.h carries only the forward direction, so consolidating is a component's worth
 * of work. §6.4.2's `cssText` — a whole RULE, selector and body — is honestly absent until it exists, and the
 * IDL audit reports that, which is the ledger. */
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

static char *cssd_serialize_block(const lxb_css_rule_declaration_list_t *list)
{
    CssBuf out = { 0 };
    const lxb_css_rule_t *r;
    bool first = true;

    for (r = list ? list->first : NULL; r; r = r->next) {
        const lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(r);
        char *name, *value;

        /* CSS Syntax's INVALID DECLARATION is not in the block, so it is not in the block's serialization
           either — the same drop the cascade and the rewrite make, for the same `__UNDEF` reason. */
        if (r->type != LXB_CSS_RULE_DECLARATION || d->type == LXB_CSS_PROPERTY__UNDEF) continue;
        name = cssd_decl_name(d);
        value = cssd_decl_value(d);   /* the whitespace-trimmed one — see its own note */
        if (name) cssd_append_declaration(&out, &first, name, value, d->important);
        free(name);
        free(value);
    }
    return out.s;   /* NULL for a block with no valid declaration, which IS the empty serialization */
}

/* The same, from the TEXT a backing keeps — which is what §6.6.1's `cssText` getter answers for both backings:
   "return the result of serializing the declarations", where the declarations are what parsing that text
   produced, NOT the bytes the page happened to write. `<div style="color:red">` therefore reads back as
   "color: red;" exactly as it does in a browser. OWNED, NULL for a block that declares nothing. */
static char *cssd_serialize_text(const char *text, size_t len)
{
    lxb_css_memory_t *mem = NULL;
    lxb_css_rule_declaration_list_t *list;
    char *out;

    if (!text || !len) return NULL;
    list = cssd_parse_block(text, len, &mem);
    out = cssd_serialize_block(list);
    if (mem) lxb_css_memory_destroy(mem, true);
    return out;
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
    if (sst && sst->root && sst->root->type == LXB_CSS_RULE_LIST) {
        lxb_css_rule_t *r;

        for (r = lxb_css_rule_list(sst->root)->first; r; r = r->next) {
            n++;
            if (r->type != LXB_CSS_RULE_STYLE) { cb(ud, (unsigned)r->type, NULL, NULL); continue; }
            {
                lxb_css_rule_style_t *st = lxb_css_rule_style(r);
                CssBuf sel = { 0 };
                char *block;

                lxb_css_selector_serialize_list_chain(st->selector, css_buf_cb, &sel);
                block = cssd_serialize_block(st->declarations);
                cb(ud, (unsigned)r->type, sel.s ? sel.s : "", block ? block : "");
                free(block);
                css_buf_free(&sel);
            }
        }
    }
    /* THE ARENA IS THE ONE OWNER and it is freed outright, for the reason the author cascade's is: creating a
       stylesheet REF-INCREMENTS the memory it is handed, so `lxb_css_stylesheet_destroy(sst, true)` only
       decrements it back and frees nothing. Every rule, selector and string above lives in here, which is
       exactly why the callback got TEXT and why it got it before this line. */
    lxb_css_memory_destroy(mem, true);
    return n;
}

/* LAYER 3 — the UA DEFAULT. A headless run still has a user-agent stylesheet, and `display` is the property a
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

/* LAYER 4 — the property's INITIAL value, straight out of Lexbor's registry, which is where the spec's own
   initial values live, and out of the table above for the properties it has no entry for. */
static char *cssd_initial_value(const char *name)
{
    const lxb_css_entry_data_t *e = lxb_css_property_by_name((const lxb_char_t *)name, strlen(name));
    CssBuf b = { 0 };
    unsigned i;

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
    char *v;

    DCHECK(g_ready, "the cascade was resolved before cssom_init built the CSS parser it parses every layer "
                    "with — the component is initialised with the DOM, so a caller reaching it first is a "
                    "component ordered ahead of the browser's own setup");
    DCHECK(el != NULL && name != NULL, "the cascade was asked to resolve with no element or no property name");
    v = cssd_inline_value(el, name, NULL);
    if (v) return v;
    v = cssd_author_value(el, name);
    if (v) return v;
    {
        const char *ua = cssd_ua_value(el, name);
        if (ua) {
            char *out = strdup(ua);
            CHECK(out != NULL, "cssom: OOM copying a UA-stylesheet value — a dropped one reads as undeclared");
            return out;
        }
    }
    return cssd_initial_value(name);
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

/* §6.6.1's SET A CSS DECLARATION and REMOVE A CSS DECLARATION, over the block's serialization: the declarations
   of `text` with `name` set to `value` and the given important flag, or REMOVED when `value` is NULL. The
   declaration keeps the POSITION it had — the spec's own recommended algorithm updates the target declaration
   in place, and its constraint is that exactly one declaration for the property exists afterwards — and a name
   the block does not declare is appended. What comes out is §6.6's serialize-a-CSS-declaration-block. OWNED,
   NULL for a block left with no declarations, which IS the empty serialization. */
static char *cssd_block_with(const char *text, size_t len, const char *name, const char *value, bool important)
{
    lxb_css_memory_t *mem = NULL;
    lxb_css_rule_declaration_list_t *list;
    CssBuf out = { 0 };
    bool first = true, wrote = false;

    if (text && len) {
        lxb_css_rule_t *r;

        list = cssd_parse_block(text, len, &mem);
        for (r = list ? list->first : NULL; r; r = r->next) {
            lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(r);
            char *dname, *dvalue;

            /* The invalid declaration is dropped on the way OUT as well as on the way in — CSSOM serializes a
               declaration block from the declarations it holds, and lexbor's `__UNDEF` placeholder is not one
               of them (the one a bad block produces carries no property record at all, so serializing it reads
               through a null). */
            if (r->type != LXB_CSS_RULE_DECLARATION || d->type == LXB_CSS_PROPERTY__UNDEF) continue;
            dname = cssd_decl_name(d);
            if (dname && strcmp(dname, name) == 0) {
                free(dname);
                if (!value) continue;            /* removeProperty drops it */
                if (wrote) continue;             /* exactly one declaration per property */
                cssd_append_declaration(&out, &first, name, value, important);
                wrote = true;
                continue;
            }
            dvalue = cssd_decl_value(d);
            if (dname) cssd_append_declaration(&out, &first, dname, dvalue, d->important);
            free(dname);
            free(dvalue);
        }
        if (mem) lxb_css_memory_destroy(mem, true);
    }
    if (value && !wrote) cssd_append_declaration(&out, &first, name, value, important);
    return out.s;
}

/* The whole of a member's write: read the declarations, edit them, put them back. */
static void cssd_write_declaration(JSContext *ctx, JSValueConst block, const char *name, const char *value,
                                   bool important)
{
    size_t len = 0;
    char *text = cssd_declarations_text(ctx, block, &len);
    char *next = cssd_block_with(text, len, name, value, important);

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
        /* §6.6.1's removeProperty: the value is read BEFORE the removal, because that value is what it
           returns. */
        size_t len = 0;
        char *text = cssd_declarations_text(ctx, block, &len);
        char *old = cssd_value_in_block(text, len, name, NULL);

        free(text);
        cssd_write_declaration(ctx, block, name, NULL, false);
        r = old ? JS_NewString(ctx, old) : JS_NewStringLen(ctx, "", 0);
        free(old);
    } else if (magic == 2) {
        /* §6.6.1's getPropertyPriority. A COMPUTED block's declarations are resolved values and carry no
           important flag at all, so the answer over its whole domain is the empty string — a positive
           statement about that block, not a hole where its text would be. */
        bool important = false;
        char *v = NULL;

        if (!computed) {
            size_t len = 0;
            char *text = cssd_declarations_text(ctx, block, &len);

            v = cssd_value_in_block(text, len, name, &important);
            free(text);
        }
        r = JS_NewString(ctx, (v && important) ? "important" : "");
        free(v);
    } else {
        char *v;

        if (computed) {
            /* CSSOM §9's RESOLVED value, which for most properties is the computed value and for the box-model
               ones is the used value (css_computed_value.h). */
            v = css_resolved_value(cssd_owner_element(ctx, block), name);
        } else {
            size_t len = 0;
            char *text = cssd_declarations_text(ctx, block, &len);

            v = cssd_value_in_block(text, len, name, NULL);
            free(text);
        }
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
    char *v;

    DCHECK(e != NULL, "a CSS attribute was declared with a property id the registry does not have");
    if (JS_IsException(block)) return block;
    if (cssd_flag(ctx, block, "computed")) {
        v = css_resolved_value(cssd_owner_element(ctx, block), (const char *)e->name);
    } else {
        size_t len = 0;
        char *text = cssd_declarations_text(ctx, block, &len);

        v = cssd_value_in_block(text, len, (const char *)e->name, NULL);
        free(text);
    }
    r = v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
    free(v);
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

/* The property NAMES the block declares, in order — what `length` counts and `item(i)` answers. */
static int cssd_names(const char *text, size_t len, char **out, int max)
{
    lxb_css_memory_t *mem = NULL;
    lxb_css_rule_declaration_list_t *list;
    lxb_css_rule_t *r;
    int n = 0;

    if (!text || !len) return 0;
    list = cssd_parse_block(text, len, &mem);
    if (list) {
        for (r = list->first; r && n < max; r = r->next) {
            lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(r);
            CssBuf b = { 0 };
            /* An invalid declaration is not in the block, so `length` does not count it and `item(i)` does not
               name it — the same drop the cascade and the rewrite make. */
            if (r->type != LXB_CSS_RULE_DECLARATION || d->type == LXB_CSS_PROPERTY__UNDEF) continue;
            lxb_css_property_serialize_name(d->u.user, d->type, css_buf_cb, &b);
            if (b.s) out[n++] = b.s;
        }
    }
    if (mem) lxb_css_memory_destroy(mem, true);
    return n;
}

#define CSSD_MAX_NAMES 256

/* The names of the block's declarations, collected. Both §6.6.1 members that need them go through here, and
   both assert the same premise: a COMPUTED block's declarations are NOT the ones its owner element declares
   inline, so counting those is a real number belonging to another block. Returns the count. */
static int cssd_declared_names(JSContext *ctx, JSValueConst block, char **names, int max)
{
    size_t len = 0;
    char *text;
    int n;

    DCHECK(!cssd_flag(ctx, block, "computed"),
           "§6.6.1's `length` and `item` count the CSS declarations of a COMPUTED block, and §7.2 states what "
           "those are: every LONGHAND property that is a supported CSS property, in lexicographical order, plus "
           "every custom property whose computed value is not the guaranteed-invalid value. This engine has no "
           "such list — lexbor's registry carries the shorthands beside the longhands and says which is which "
           "nowhere — so build the longhand set and enumerate it here. Until then these two answered out of the "
           "element's INLINE attribute, which is a real number belonging to a different declaration block");
    text = cssd_declarations_text(ctx, block, &len);
    n = cssd_names(text, len, names, max);
    free(text);
    return n;
}

static JSValue js_cssd_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue block = cssd_block(ctx, this_val);
    char *names[CSSD_MAX_NAMES];
    int n, i;

    (void)magic;
    if (JS_IsException(block)) return block;
    n = cssd_declared_names(ctx, block, names, CSSD_MAX_NAMES);
    for (i = 0; i < n; i++) free(names[i]);
    JS_FreeValue(ctx, block);
    return JS_NewInt32(ctx, n);
}

static JSValue js_cssd_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue block = cssd_block(ctx, this_val), r;
    char *names[CSSD_MAX_NAMES];
    int n, i;
    int64_t idx = 0;

    (void)magic;
    if (JS_IsException(block)) return block;
    DCHECK(argc >= 1, "§6.6.1's `item` reached its body with no index — its IDL argument is required");
    JS_ToInt64(ctx, &idx, argv[0]);   /* a real number by now: the declaration converted it */
    n = cssd_declared_names(ctx, block, names, CSSD_MAX_NAMES);
    /* "If there is no indexth object in the collection, then the method must return the empty string." */
    r = (idx >= 0 && idx < n) ? JS_NewString(ctx, names[idx]) : JS_NewStringLen(ctx, "", 0);
    for (i = 0; i < n; i++) free(names[i]);
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
static JSValue cssd_new(JSContext *ctx, JSValueConst owner_node, JSValueConst parent_rule,
                        bool computed, bool readonly)
{
    JSValue obj, slots, proto;
    JSAtom k;

    DCHECK(g_ready, "a CSS declaration block was minted before cssom_init ran");
    DCHECK(JS_IsNull(owner_node) != JS_IsNull(parent_rule),
           "§6.6's owner node and parent CSS rule are not two independent fields for this engine: one of them "
           "is where the declarations LIVE, so a block with both or with neither is a block whose declarations "
           "are kept in two places or in none");
    DCHECK(!computed || readonly,
           "a COMPUTED declaration block was minted WRITABLE. §7.2 is the only creator that sets the computed "
           "flag and it sets the readonly flag in the same breath — a writable one would take §6.6.1's set-a-"
           "CSS-declaration path into a block whose declarations are computed per read and stored nowhere");
    proto = cssd_proto(ctx);
    obj = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
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
    DCHECK(css_rule_is(rule),
           "§6.4.3's `style` was asked to back a declaration block with something that is not a CSS rule");
    return cssd_new(ctx, JS_NULL, rule, false, false);
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
        JS_FreeValue(ctx, cur);
        cur = cssd_new(ctx, this_val, JS_NULL, false, false);
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
    return cssd_new(ctx, argv[0], JS_NULL, true, true);
}

void cssom_init(JSContext *ctx)
{
    uintptr_t id;

    DCHECK(!g_ready, "cssom_init ran twice — one instance is one document");
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
        /* EVERY block this engine builds is a §6.6.1 CSSStyleProperties — all three creators say so — so the
           CLASS is that one, and §6.6.1's CSSStyleDeclaration base gets a per-realm value slot instead. */
        JSClassDef d = { "CSSStyleProperties" };
        JS_NewClassID(JS_GetRuntime(ctx), &g_cssd_class);
        JS_NewClass(JS_GetRuntime(ctx), g_cssd_class, &d);
    }
    g_declaration_proto_slot = realm_value_declare(ctx, "CSSOM §6.6.1 CSSStyleDeclaration.prototype");
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
    realm_declare_intrinsic(cssom_install_proto);
}

/* CSSOM §6.6.1's TWO INTERFACE PROTOTYPE OBJECTS, FOR ONE REALM. They are two because the spec splits them:
   `interface CSSStyleProperties : CSSStyleDeclaration` carries `cssFloat` and the per-property camel-cased
   attributes, and CSSStyleDeclaration carries the block's own eight members. Installing all of them on one
   object made `CSSStyleProperties` an absent global — an honest ReferenceError for an interface every one of
   this engine's blocks IS — and made `Object.getOwnPropertyNames(CSSStyleDeclaration.prototype)` report three
   hundred property attributes a browser does not have there. Nothing is an instance of the base, so it holds no
   class of its own, exactly as §6.1.1's StyleSheet and §6.4.2's CSSRule do. */
void cssom_install_proto(JSContext *ctx)
{
    JSValue base, proto, prev;
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
    JS_SetClassProto(ctx, g_cssd_class, proto);
    realm_value_set(ctx, g_declaration_proto_slot, base);
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
    JS_FreeValue(ctx, base);
    JS_FreeValue(ctx, proto);
    idl_install_method(ctx, global, "getComputedStyle", 1, g_id_gcs);
}

void cssom_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_decl_key);   /* the prototypes are the REALMS' — released with their contexts */
    JS_FreeValue(ctx, g_inline_key);
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
