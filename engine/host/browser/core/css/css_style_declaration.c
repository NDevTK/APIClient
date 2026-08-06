/* CSSOM — CSSStyleDeclaration, `element.style`, and getComputedStyle().
 *
 * WHAT WAS HERE BEFORE: nothing. `el.style.display = 'none'` wrote an ordinary JS property on an object that
 * did not exist, and getComputedStyle was absent, so a page reading a computed value threw. Both are named in
 * this project's own rules as the shape of a fidelity gap — a getter returning opaque where the SPEC COMPUTES A
 * REAL VALUE — and both matter to what this engine is for: a bundle that branches on
 * `getComputedStyle(el).display === 'none'` routes differently on each side, and each side has its own
 * endpoints.
 *
 * LEXBOR OWNS THE CSS, and that is the point of binding to it rather than hand-rolling. It has the real
 * property registry (so the camel-cased IDL attributes are GENERATED FROM THE SPEC'S OWN PROPERTY LIST rather
 * than typed here), a real declaration parser, real value serializers, and a real selector matcher that answers
 * for a SINGLE node. Every layer below is Lexbor doing the parsing and this file doing the cascade.
 *
 * THE STORAGE IS THE `style` CONTENT ATTRIBUTE, which is the design decision the rest follows from. Lexbor can
 * also hold parsed styles on the element (an AVL keyed by property), and using that would have been faster and
 * WRONG: it lives outside the per-flow DOM delta, so a `style.color` written by one forked arm would be visible
 * to its sibling. The attribute IS captured, so storing there makes an inline style time-travel for free, and
 * the spec already defines the round-trip between the two.
 *
 * THE CASCADE IS RESOLVED LIVE, per read, from the RUNNING FLOW'S TREE: inline, then the author rules in that
 * flow's own `<style>` elements matched with lxb_selectors_match_node, then the UA default, then the property's
 * initial value. Nothing is cached across a read, because a cache would be shared state that the flow machinery
 * does not swap — the same reason the storage is the attribute. */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>

#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

#include "check.h"
#include "quickjs.h"
#include "core/idl_args.h"
#include "core/dom/node.h"
#include "core/dom/element.h"
#include "core/css/css_style_declaration.h"
#include "solver/dom_cow.h"

/* The private key the declaration's own slots hang off — a Symbol, so a page cannot see or forge it, which is
   also the brand check every member performs. */
static JSValue g_key;
static JSValue g_proto;
static int     g_ready;

/* WHICH VIEW of an element a declaration object is. Decided when the object is built and never asked again:
   `element.style` is the inline block and is writable; getComputedStyle's is the resolved cascade and is
   read-only, which is what the spec makes it. */
enum { CSSD_INLINE = 0, CSSD_COMPUTED = 1 };

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

/* ---- the element behind a declaration -------------------------------------------------------------------- */
static JSValue cssd_slots(JSContext *ctx, JSValueConst v)
{
    JSAtom k;
    JSValue slots;

    DCHECK(g_ready, "a CSSStyleDeclaration's slots were asked for before cssom_init ran");
    if (!JS_IsObject(v)) return JS_UNDEFINED;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &slots, v, k) <= 0)   /* an own SLOT, never a lookup — see event.c */
        slots = JS_UNDEFINED;
    JS_FreeAtom(ctx, k);
    return slots;
}

static lxb_dom_element_t *cssd_element(JSContext *ctx, JSValueConst v, int *pmode)
{
    JSValue slots = cssd_slots(ctx, v), owner, m;
    lxb_dom_node_t *n;

    if (!JS_IsObject(slots)) { JS_FreeValue(ctx, slots); return NULL; }
    owner = JS_GetPropertyStr(ctx, slots, "element");
    m = JS_GetPropertyStr(ctx, slots, "mode");
    if (pmode) *pmode = JS_ToBool(ctx, m) ? CSSD_COMPUTED : CSSD_INLINE;
    JS_FreeValue(ctx, m);
    n = node_of(owner);
    JS_FreeValue(ctx, owner);
    JS_FreeValue(ctx, slots);
    return (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT) ? lxb_dom_interface_element(n) : NULL;
}

/* ---- the parser, and the declaration list of a chunk of CSS text ---------------------------------------- */
static lxb_css_parser_t *g_parser;
static lxb_selectors_t  *g_selectors;

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
    *pmem = mem;
    return list;
}

/* A declaration's PROPERTY NAME, serialized. Comparing by name rather than by the registry id is what makes a
   CUSTOM property (`--brand`) work through exactly the same path as a known one — it has no id to compare. */
static bool cssd_decl_named(const lxb_css_rule_declaration_t *d, const char *name)
{
    CssBuf b = { 0 };
    bool same;

    lxb_css_property_serialize_name(d->u.user, d->type, css_buf_cb, &b);
    same = b.s && strcmp(b.s, name) == 0;
    css_buf_free(&b);
    return same;
}

static char *cssd_decl_value(const lxb_css_rule_declaration_t *d)
{
    CssBuf b = { 0 };
    lxb_css_property_serialize(d->u.user, d->type, css_buf_cb, &b);
    return b.s ? b.s : NULL;
}

/* The element's inline `style` attribute, as text. BORROWED from Lexbor's own storage. */
static const char *cssd_inline_text(lxb_dom_element_t *el, size_t *plen)
{
    const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"style", 5, plen);
    return (const char *)v;
}

/* LAYER 1 — the INLINE declaration for `name`, or NULL. The highest-weight layer short of !important, and the
   one that is per-flow because the attribute it reads is. */
static char *cssd_inline_value(lxb_dom_element_t *el, const char *name, bool *pimportant)
{
    size_t len = 0;
    const char *text = cssd_inline_text(el, &len);
    lxb_css_memory_t *mem = NULL;
    lxb_css_rule_declaration_list_t *list;
    lxb_css_rule_t *r;
    char *out = NULL;

    if (!text || !len) return NULL;
    list = cssd_parse_block(text, len, &mem);
    if (list) {
        for (r = list->first; r; r = r->next) {
            lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(r);
            if (r->type != LXB_CSS_RULE_DECLARATION || !cssd_decl_named(d, name)) continue;
            out = cssd_decl_value(d);
            if (pimportant) *pimportant = d->important;
        }
    }
    if (mem) lxb_css_memory_destroy(mem, true);
    return out;   /* the LAST wins, which is what a declaration block means */
}

/* LAYER 2 — the AUTHOR cascade, resolved from the RUNNING FLOW'S tree. Every `<style>` element in the document
   is parsed, every style rule's selector is matched against THIS element with Lexbor's own matcher, and the
   winner is the highest specificity, later document order breaking a tie — which is the cascade. It is redone
   per read on purpose: a cache would be shared state the flow machinery does not swap, so a `<style>` one arm
   injected would decide another arm's computed values. */
typedef struct { bool matched; lxb_css_selector_specificity_t spec; } CssMatch;

static lxb_status_t cssd_match_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx)
{
    CssMatch *m = ctx;
    (void)node;
    /* A selector LIST matches through whichever of its selectors matched, and the cascade uses the highest —
       `#id, div { … }` on a div with that id contributes the id's weight, not the tag's. */
    if (!m->matched || spec > m->spec) m->spec = spec;
    m->matched = true;
    return LXB_STATUS_OK;
}

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
            }
            if (sst && sst->root && sst->root->type == LXB_CSS_RULE_LIST) {
                lxb_css_rule_t *r;
                for (r = lxb_css_rule_list(sst->root)->first; r; r = r->next) {
                    lxb_css_rule_style_t *st = lxb_css_rule_style(r);
                    CssMatch m = { false, 0 };
                    lxb_css_rule_t *dr;
                    if (r->type != LXB_CSS_RULE_STYLE || !st->selector || !st->declarations) continue;
                    lxb_selectors_match_node(g_selectors, self, st->selector, cssd_match_cb, &m);
                    if (!m.matched) continue;
                    for (dr = st->declarations->first; dr; dr = dr->next) {
                        lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(dr);
                        bool wins;
                        if (dr->type != LXB_CSS_RULE_DECLARATION || !cssd_decl_named(d, name)) continue;
                        /* THE CASCADE, in the order §6.4 states it: IMPORTANCE first, then SPECIFICITY, then
                           document ORDER. Order alone is what this compared at first, which is wrong in the
                           most ordinary way there is — `#main { display:block }` written before
                           `div { display:none }` would have lost, and every page puts its general rules last.
                           Lexbor reports the specificity through the match callback; throwing it away and
                           calling document order "the cascade" was skipping the subproblem. */
                        wins = !have
                            || (d->important && !best_important)
                            || (d->important == best_important && m.spec >= best_spec);
                        if (!wins) continue;
                        free(out);
                        out = cssd_decl_value(d);
                        best_spec = m.spec;
                        best_important = d->important;
                        have = true;
                    }
                }
            }
            if (sst) lxb_css_stylesheet_destroy(sst, true);   /* takes its arena with it */
            else if (smem) lxb_css_memory_destroy(smem, true);
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
    { "head", "display", "none" },   { "style", "display", "none" },
    { "script", "display", "none" }, { "template", "display", "none" },
    { "link", "display", "none" },   { "meta", "display", "none" },
    { "title", "display", "none" },
};

static const char *cssd_ua_value(lxb_dom_element_t *el, const char *name)
{
    size_t n = 0;
    const lxb_char_t *tag = lxb_dom_element_local_name(el, &n);
    unsigned i;

    if (!tag) return NULL;
    for (i = 0; i < sizeof(UA_DEFAULT) / sizeof(UA_DEFAULT[0]); i++)
        if (strlen(UA_DEFAULT[i].tag) == n && memcmp(UA_DEFAULT[i].tag, tag, n) == 0 &&
            strcmp(UA_DEFAULT[i].prop, name) == 0)
            return UA_DEFAULT[i].value;
    /* Every element the table does not name is `display: inline`, which is the UA sheet's own default. */
    if (strcmp(name, "display") == 0) return "inline";
    return NULL;
}

/* LAYER 4 — the property's INITIAL value, straight out of Lexbor's registry, which is where the spec's own
   initial values live. */
static char *cssd_initial_value(const char *name)
{
    const lxb_css_entry_data_t *e = lxb_css_property_by_name((const lxb_char_t *)name, strlen(name));
    CssBuf b = { 0 };

    if (!e || !e->initial) return NULL;
    lxb_css_property_serialize(e->initial, e->unique, css_buf_cb, &b);
    return b.s;
}

/* THE CASCADE, in the order the spec resolves it. */
static char *cssd_resolved(lxb_dom_element_t *el, const char *name, int mode)
{
    char *v = cssd_inline_value(el, name, NULL);

    if (v || mode == CSSD_INLINE)
        return v;   /* `element.style` is the inline block ALONE — it is not a resolved value */
    v = cssd_author_value(el, name);
    if (v) return v;
    {
        const char *ua = cssd_ua_value(el, name);
        if (ua) return strdup(ua);
    }
    return cssd_initial_value(name);
}

/* ---- the interface ---------------------------------------------------------------------------------------- */

/* Rewrite the inline block with `name` set to `value`, or removed when `value` is NULL. Through setAttribute's
   own chokepoint, so the write is captured by the per-flow delta like every other DOM write. */
static void cssd_write_inline(JSContext *ctx, lxb_dom_element_t *el, const char *name, const char *value)
{
    size_t len = 0;
    const char *text = cssd_inline_text(el, &len);
    lxb_css_memory_t *mem = NULL;
    lxb_css_rule_declaration_list_t *list;
    CssBuf out = { 0 };
    bool wrote = false;

    if (text && len) {
        lxb_css_rule_t *r;
        list = cssd_parse_block(text, len, &mem);
        if (list) {
            for (r = list->first; r; r = r->next) {
                lxb_css_rule_declaration_t *d = lxb_css_rule_declaration(r);
                if (r->type != LXB_CSS_RULE_DECLARATION) continue;
                if (cssd_decl_named(d, name)) {
                    if (!value) continue;            /* removeProperty drops it */
                    if (wrote) continue;             /* one entry per property */
                    css_buf_add(&out, name); css_buf_add(&out, ": ");
                    css_buf_add(&out, value); css_buf_add(&out, "; ");
                    wrote = true;
                    continue;
                }
                lxb_css_property_serialize_name(d->u.user, d->type, css_buf_cb, &out);
                css_buf_add(&out, ": ");
                lxb_css_property_serialize(d->u.user, d->type, css_buf_cb, &out);
                if (d->important) css_buf_add(&out, " !important");
                css_buf_add(&out, "; ");
            }
        }
        if (mem) lxb_css_memory_destroy(mem, true);
    }
    if (value && !wrote) {
        css_buf_add(&out, name); css_buf_add(&out, ": ");
        css_buf_add(&out, value); css_buf_add(&out, "; ");
    }
    dom_cow_set_attribute(el, "style", out.s ? out.s : "", out.s ? out.n : 0);
    css_buf_free(&out);
}

/* magic 0 = getPropertyValue, 1 = removeProperty, 2 = getPropertyPriority */
static JSValue js_cssd_prop_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    int mode = CSSD_INLINE;
    lxb_dom_element_t *el = cssd_element(ctx, this_val, &mode);
    const char *name;
    JSValue r;

    if (!el) return JS_NewStringLen(ctx, "", 0);
    if (argc < 1) return JS_NewStringLen(ctx, "", 0);
    name = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!name) return JS_EXCEPTION;
    if (magic == 1) {
        char *old = cssd_inline_value(el, name, NULL);
        /* §CSSOM: a read-only declaration throws rather than silently ignoring the write. */
        if (mode == CSSD_COMPUTED) {
            free(old);
            JS_FreeCString(ctx, name);
            return JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                                        "a computed style declaration is read-only");
        }
        cssd_write_inline(ctx, el, name, NULL);
        r = old ? JS_NewString(ctx, old) : JS_NewStringLen(ctx, "", 0);
        free(old);
    } else if (magic == 2) {
        bool important = false;
        char *v = cssd_inline_value(el, name, &important);
        r = JS_NewString(ctx, (v && important) ? "important" : "");
        free(v);
    } else {
        char *v = cssd_resolved(el, name, mode);
        r = v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
        free(v);
    }
    JS_FreeCString(ctx, name);
    return r;
}

static JSValue js_cssd_set_property(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    int mode = CSSD_INLINE;
    lxb_dom_element_t *el = cssd_element(ctx, this_val, &mode);
    const char *name, *value;

    (void)magic;
    if (!el || argc < 2) return JS_UNDEFINED;
    if (mode == CSSD_COMPUTED)
        return JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                                    "a computed style declaration is read-only");
    name = JS_ToCString(ctx, argv[0]);
    value = JS_ToCString(ctx, argv[1]);
    if (!name || !value) {
        if (name) JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        return JS_EXCEPTION;
    }
    /* §CSSOM: setting the empty string REMOVES the declaration. */
    cssd_write_inline(ctx, el, name, *value ? value : NULL);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

/* The camel-cased IDL attributes. `magic` is Lexbor's own property id, so the dashed name is read back out of
   the registry rather than stored twice. */
static JSValue js_cssd_camel_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    const lxb_css_entry_data_t *e = lxb_css_property_by_id((uintptr_t)magic);
    int mode = CSSD_INLINE;
    lxb_dom_element_t *el = cssd_element(ctx, this_val, &mode);
    char *v;
    JSValue r;

    DCHECK(e != NULL, "a CSS attribute was declared with a property id the registry does not have");
    if (!el || !e) return JS_NewStringLen(ctx, "", 0);
    v = cssd_resolved(el, (const char *)e->name, mode);
    r = v ? JS_NewString(ctx, v) : JS_NewStringLen(ctx, "", 0);
    free(v);
    return r;
}

static JSValue js_cssd_camel_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    const lxb_css_entry_data_t *e = lxb_css_property_by_id((uintptr_t)magic);
    int mode = CSSD_INLINE;
    lxb_dom_element_t *el = cssd_element(ctx, this_val, &mode);
    const char *v;

    DCHECK(e != NULL, "a CSS attribute was declared with a property id the registry does not have");
    if (!el || !e) return JS_UNDEFINED;
    if (mode == CSSD_COMPUTED)
        return JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                                    "a computed style declaration is read-only");
    v = JS_ToCString(ctx, val);   /* a real string by now: the declaration converted it */
    if (!v) return JS_EXCEPTION;
    cssd_write_inline(ctx, el, (const char *)e->name, *v ? v : NULL);
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}

/* §CSSOM cssText / length / item(i) — the declaration block as text and as an indexed list of its property
   names. The INLINE block for both views: a computed declaration enumerates the properties it was asked for,
   which is a resolved-value set this engine builds per read rather than holding. */
static JSValue js_cssd_css_text(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = cssd_element(ctx, this_val, NULL);
    size_t len = 0;
    const char *text;

    (void)magic;
    if (!el) return JS_NewStringLen(ctx, "", 0);
    text = cssd_inline_text(el, &len);
    return text ? JS_NewStringLen(ctx, text, len) : JS_NewStringLen(ctx, "", 0);
}

static JSValue js_cssd_set_css_text(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    int mode = CSSD_INLINE;
    lxb_dom_element_t *el = cssd_element(ctx, this_val, &mode);
    const char *v;

    (void)magic;
    if (!el) return JS_UNDEFINED;
    if (mode == CSSD_COMPUTED)
        return JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                                    "a computed style declaration is read-only");
    v = JS_ToCString(ctx, val);
    if (!v) return JS_EXCEPTION;
    dom_cow_set_attribute(el, "style", v, strlen(v));   /* the attribute IS the block */
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}

/* The property NAMES the inline block declares, in order — what `length` counts and `item(i)` answers. */
static int cssd_names(lxb_dom_element_t *el, char **out, int max)
{
    size_t len = 0;
    const char *text = cssd_inline_text(el, &len);
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
            if (r->type != LXB_CSS_RULE_DECLARATION) continue;
            lxb_css_property_serialize_name(d->u.user, d->type, css_buf_cb, &b);
            if (b.s) out[n++] = b.s;
        }
    }
    if (mem) lxb_css_memory_destroy(mem, true);
    return n;
}

#define CSSD_MAX_NAMES 256

static JSValue js_cssd_length(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = cssd_element(ctx, this_val, NULL);
    char *names[CSSD_MAX_NAMES];
    int n, i;

    (void)magic;
    if (!el) return JS_NewInt32(ctx, 0);
    n = cssd_names(el, names, CSSD_MAX_NAMES);
    for (i = 0; i < n; i++) free(names[i]);
    return JS_NewInt32(ctx, n);
}

static JSValue js_cssd_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = cssd_element(ctx, this_val, NULL);
    char *names[CSSD_MAX_NAMES];
    int n, i;
    int64_t idx = 0;
    JSValue r;

    (void)magic;
    if (!el || argc < 1) return JS_NewStringLen(ctx, "", 0);
    JS_ToInt64(ctx, &idx, argv[0]);   /* a real number by now: the declaration converted it */
    n = cssd_names(el, names, CSSD_MAX_NAMES);
    r = (idx >= 0 && idx < n) ? JS_NewString(ctx, names[idx]) : JS_NewStringLen(ctx, "", 0);
    for (i = 0; i < n; i++) free(names[i]);
    return r;
}

/* Build a declaration object bound to `owner` (an element WRAPPER, so the element cannot go away underneath it
   and the identity table stays the one place a node is named). */
static JSValue cssd_new(JSContext *ctx, JSValueConst owner, int mode)
{
    JSValue obj, slots;
    JSAtom k;

    DCHECK(g_ready, "a CSSStyleDeclaration was minted before cssom_init ran");
    obj = JS_NewObjectProto(ctx, g_proto);
    if (JS_IsException(obj)) return obj;
    slots = JS_NewObject(ctx);
    k = JS_ValueToAtom(ctx, g_key);
    CHECK(!JS_IsException(slots) && k != JS_ATOM_NULL, "the CSSStyleDeclaration slot record allocation failed");
    JS_SetPropertyStr(ctx, slots, "element", JS_DupValue(ctx, owner));
    JS_SetPropertyStr(ctx, slots, "mode", JS_NewBool(ctx, mode == CSSD_COMPUTED));
    JS_SetProperty(ctx, obj, k, slots);
    JS_FreeAtom(ctx, k);
    return obj;
}

/* HTMLElement's `[SameObject] attribute CSSStyleDeclaration style`. SameObject is why the declaration is
   remembered on the element rather than rebuilt: a page holds `el.style` and compares it, and a fresh object
   per read makes every such comparison false — the same rule node identity follows. It is stored as an own
   SLOT, so it is per-flow like everything else on the wrapper. */
static JSValue js_el_get_style(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSAtom k;
    JSValue cur;

    (void)magic;
    k = JS_ValueToAtom(ctx, g_key);
    if (k == JS_ATOM_NULL) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &cur, this_val, k) <= 0)
        cur = JS_UNDEFINED;
    if (!JS_IsObject(cur)) {
        JS_FreeValue(ctx, cur);
        cur = cssd_new(ctx, this_val, CSSD_INLINE);
        JS_SetProperty(ctx, (JSValue)this_val, k, JS_DupValue(ctx, cur));
    }
    JS_FreeAtom(ctx, k);
    return cur;
}

/* §CSSOM-VIEW getComputedStyle(elt, pseudoElt) — the RESOLVED value, read-only. The pseudo-element argument is
   converted and rejected rather than ignored: this engine has no pseudo-element boxes, and answering the
   ORIGINATING element's values for `::before` would be a wrong answer rather than a missing one. */
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
    return cssd_new(ctx, argv[0], CSSD_COMPUTED);
}

void cssom_init(JSContext *ctx)
{
    uintptr_t id;

    DCHECK(!g_ready, "cssom_init ran twice — one instance is one document");
    g_parser = lxb_css_parser_create();
    CHECK(g_parser != NULL && lxb_css_parser_init(g_parser, NULL) == LXB_STATUS_OK,
          "the CSS parser could not be created");
    g_selectors = lxb_selectors_create();
    CHECK(g_selectors != NULL && lxb_selectors_init(g_selectors) == LXB_STATUS_OK,
          "the CSS selector matcher could not be created");
    g_key = JS_NewSymbol(ctx, "cssStyleDeclaration", false);
    g_proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_key) && !JS_IsException(g_proto),
          "the CSSStyleDeclaration key or prototype allocation failed");
    idl_interface_tag(ctx, g_proto, "CSSStyleDeclaration");
    g_ready = 1;

    JS_SetPropertyStr(ctx, g_proto, "parentRule", JS_NULL);   /* no CSSRule objects yet, and null is the answer */
    idl_install_accessor(ctx, g_proto, "length", js_cssd_length, 0, -1);
    idl_install_accessor(ctx, g_proto, "cssText", js_cssd_css_text, 0,
                         idl_setter_id(ctx, IDL_DOMSTRING, false, js_cssd_set_css_text, 0));
    {
        static const IdlArgType ONE_STR[1] = { IDL_DOMSTRING };
        static const IdlArgType ONE_LONG[1] = { IDL_LONG };
        static const IdlArgType THREE_STR[3] = { IDL_DOMSTRING, IDL_DOMSTRING, IDL_DOMSTRING };
        idl_install_method(ctx, g_proto, "getPropertyValue", 1,
                           idl_method_id(ctx, ONE_STR, 1, js_cssd_prop_op, 0));
        idl_install_method(ctx, g_proto, "removeProperty", 1,
                           idl_method_id(ctx, ONE_STR, 1, js_cssd_prop_op, 1));
        idl_install_method(ctx, g_proto, "getPropertyPriority", 1,
                           idl_method_id(ctx, ONE_STR, 1, js_cssd_prop_op, 2));
        idl_install_method(ctx, g_proto, "setProperty", 2,
                           idl_method_id(ctx, THREE_STR, 3, js_cssd_set_property, 0));
        idl_optional_from(2);   /* CSSOM §6.7: `setProperty(property, value, optional priority)` */
        idl_install_method(ctx, g_proto, "item", 1,
                           idl_method_id(ctx, ONE_LONG, 1, js_cssd_item, 0));
    }

    /* THE CAMEL-CASED IDL ATTRIBUTES, generated from LEXBOR'S OWN CSS PROPERTY REGISTRY. Web IDL states them as
       "for each CSS property, a camel-cased attribute", so the registry IS the list — typing a hundred names
       here would be a second copy of it that could disagree, and inventing them would be worse. */
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
        idl_install_accessor(ctx, g_proto, camel, js_cssd_camel_get, (int)id,
                             idl_setter_id(ctx, IDL_DOMSTRING, false, js_cssd_camel_set, (int)id));
    }
}

void cssom_install_style_attribute(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "the style attribute was installed before cssom_init ran");
    idl_install_accessor(ctx, proto, "style", js_el_get_style, 0, -1);
}

void cssom_install(JSContext *ctx, JSValueConst global)
{
    DCHECK(g_ready, "CSSStyleDeclaration was installed before cssom_init ran");
    node_install_interface(ctx, global, "CSSStyleDeclaration", g_proto);
    {
        static const IdlArgType TWO[2] = { IDL_ANY, IDL_DOMSTRING };
        idl_install_method(ctx, global, "getComputedStyle", 1,
                           idl_method_id(ctx, TWO, 2, js_get_computed_style, 0));
        idl_optional_from(1);   /* CSSOM §7.1: `getComputedStyle(elt, optional pseudoElt)` */
    }
}

void cssom_free(JSContext *ctx)
{
    if (!g_ready) return;
    JS_FreeValue(ctx, g_key);
    JS_FreeValue(ctx, g_proto);
    g_key = g_proto = JS_UNDEFINED;
    if (g_selectors) { lxb_selectors_destroy(g_selectors, true); g_selectors = NULL; }
    if (g_parser) { lxb_css_parser_destroy(g_parser, true); g_parser = NULL; }
    g_ready = 0;
}
