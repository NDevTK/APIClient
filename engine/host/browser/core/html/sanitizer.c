/* HTML §8.6 — THE SANITIZER API, as its own component.
 *
 * WHERE THE TEXT IS. §8.6 is in the HTML Standard itself (multipage/dynamic-markup-insertion.html, "8.6 HTML
 * sanitization"): the interface at §8.6.2, the configuration at §8.6.3, the algorithms at §8.6.4 and the
 * built-in configurations at §8.6.5. The WICG draft it grew from is not the normative text and is not what this
 * file follows.
 *
 * WHY IT IS A COMPONENT. §8.6.4's `set and filter HTML` is the shared body of FOUR members across three
 * interfaces (`Element`/`ShadowRoot` `setHTML` and `setHTMLUnsafe`, and `Document.parseHTML*`), and every one
 * of them differs only in `safe` and in what the parse target is. The FILTER, though, is one algorithm over a
 * configuration, and the configuration is a data structure with fifteen operations of its own — canonicalize,
 * validity, two kinds of remove, the sorted `get` — so it is a component beside the parser rather than a block
 * of element.c, exactly as §13.3's serializer is.
 *
 * THE CONFIGURATION IS A JS OBJECT, and that is not a convenience. It is per-flow state that must PARK: a
 * `Sanitizer` a page builds and then hands to `setHTML` is state one flow may mutate and a sibling must not
 * see, and it must survive a cold-tier resume. A JS object gets both for free — its members are property
 * writes the heap COW delta already captures, and the snapshot machinery already carries it — where a malloc'd
 * C list would be a structure the delta cannot see and the park cannot serialize. The record BEHIND the
 * wrapper (one JSValue) time-travels through cow_capture_host_record in the accessor, which is where a
 * component's own record is captured.
 *
 * THE MEMBERS' SHAPE IS THE SPEC'S, item for item: an element or attribute is `«[ "name" → …, "namespace" → …
 * ]»` and a processing instruction is `«[ "target" → … ]»`, because §8.6.2's operations are stated over exactly
 * those keys and a different internal spelling would be a second vocabulary to translate at every step.
 *
 * WHAT WALKS PAGE-SIZED DATA AND WHAT DOES NOT. The TREE is the page's, so §8.6.4's inner sanitize steps are a
 * machine that rests at every node and every attribute (see sanitizer.h). The CONFIGURATION is not: the only
 * thing that builds one here is §8.6.5's built-in tables below, because the one arm that would let the page
 * supply its own — a `SanitizerConfig` dictionary — crashes naming the conversion machine it needs. So the
 * members that walk a configuration (`get`, `removeUnsafe`) are ordinary bodies over data this file wrote, and
 * they become machines in the same diff that lets a page write one. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/dom/attr_list.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/events/event_target.h"
#include "core/html/sanitizer.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/url/url.h"
#include "solver/cow.h"
#include "solver/dom_cow.h"

/* The four namespaces §8.6.5's tables name. Written out because a namespace is a STRING in a canonical item —
   the spec's own key — and lexbor's ids are an index into a per-document table. */
#define SAN_NS_HTML   "http://www.w3.org/1999/xhtml"
#define SAN_NS_SVG    "http://www.w3.org/2000/svg"
#define SAN_NS_MATHML "http://www.w3.org/1998/Math/MathML"
#define SAN_NS_XLINK  "http://www.w3.org/1999/xlink"

/* ---- §8.6.5's built-in configurations, as the standard's tables ------------------------------------------ */

/* One row of the built-in safe default configuration's element list: the element's name and the attributes its
   own definition marks allowed, space-separated. Both halves are read off the standard's per-element
   "Sanitization" line ("Default with attributes cite, datetime"), which is where the list normatively lives. */
typedef struct { const char *name; const char *attrs; } SanElementRow;

static const SanElementRow SAN_DEFAULT_HTML[] = {
    { "a", "href hreflang type" }, { "abbr", NULL }, { "address", NULL }, { "article", NULL },
    { "aside", NULL }, { "b", NULL }, { "bdi", NULL }, { "bdo", NULL }, { "blockquote", "cite" },
    { "body", NULL }, { "br", NULL }, { "caption", NULL }, { "cite", NULL }, { "code", NULL },
    { "col", "span" }, { "colgroup", "span" }, { "data", "value" }, { "dd", NULL },
    { "del", "cite datetime" }, { "dfn", NULL }, { "div", NULL }, { "dl", NULL }, { "dt", NULL },
    { "em", NULL }, { "figcaption", NULL }, { "figure", NULL }, { "footer", NULL },
    { "h1", NULL }, { "h2", NULL }, { "h3", NULL }, { "h4", NULL }, { "h5", NULL }, { "h6", NULL },
    { "head", NULL }, { "header", NULL }, { "hgroup", NULL }, { "hr", NULL }, { "html", NULL },
    { "i", NULL }, { "ins", "cite datetime" }, { "kbd", NULL }, { "li", "value" }, { "main", NULL },
    { "mark", NULL }, { "menu", NULL }, { "nav", NULL }, { "ol", "reversed start type" }, { "p", NULL },
    { "pre", NULL }, { "q", NULL }, { "rp", NULL }, { "rt", NULL }, { "ruby", NULL }, { "s", NULL },
    { "samp", NULL }, { "search", NULL }, { "section", NULL }, { "small", NULL }, { "span", NULL },
    { "strong", NULL }, { "sub", NULL }, { "sup", NULL }, { "table", NULL }, { "tbody", NULL },
    { "td", "colspan headers rowspan" }, { "tfoot", NULL },
    { "th", "abbr colspan headers rowspan scope" }, { "thead", NULL }, { "time", "datetime" },
    { "title", NULL }, { "tr", NULL }, { "u", NULL }, { "ul", NULL }, { "var", NULL }, { "wbr", NULL },
};

static const SanElementRow SAN_DEFAULT_MATHML[] = {
    { "math", NULL }, { "merror", NULL }, { "mfrac", NULL }, { "mi", NULL }, { "mmultiscripts", NULL },
    { "mn", NULL },
    { "mo", "fence form largeop lspace maxsize minsize movablelimits rspace separator stretchy symmetric" },
    { "mover", "accent" }, { "mpadded", "depth height lspace voffset width" }, { "mphantom", NULL },
    { "mprescripts", NULL }, { "mroot", NULL }, { "mrow", NULL }, { "ms", NULL },
    { "mspace", "depth height width" }, { "msqrt", NULL }, { "mstyle", NULL }, { "msub", NULL },
    { "msubsup", NULL }, { "msup", NULL }, { "mtable", NULL }, { "mtd", "columnspan rowspan" },
    { "mtext", NULL }, { "mtr", NULL }, { "munder", "accentunder" },
    { "munderover", "accent accentunder" }, { "semantics", NULL },
};

static const SanElementRow SAN_DEFAULT_SVG[] = {
    { "a", "href hreflang type" }, { "circle", "cx cy pathLength r" }, { "defs", NULL }, { "desc", NULL },
    { "ellipse", "cx cy pathLength rx ry" }, { "foreignObject", "height width x y" }, { "g", NULL },
    { "line", "pathLength x1 x2 y1 y2" },
    { "marker", "markerHeight markerUnits markerWidth orient preserveAspectRatio refX refY viewBox" },
    { "metadata", NULL }, { "path", "d pathLength" }, { "polygon", "pathLength points" },
    { "polyline", "pathLength points" }, { "rect", "height pathLength rx ry width x y" },
    { "svg", "height preserveAspectRatio viewBox width x y" },
    { "text", "dx dy lengthAdjust rotate textLength x y" },
    { "textPath", "lengthAdjust method path side spacing startOffset textLength" }, { "title", NULL },
    { "tspan", "dx dy lengthAdjust rotate textLength x y" },
};

/* §8.6.5's default `attributes` list — the global allow-list, every entry in the null namespace. */
static const char *const SAN_DEFAULT_ATTRIBUTES[] = {
    "dir", "lang", "title",
    "alignment-baseline", "baseline-shift", "clip-path", "clip-rule", "color", "color-interpolation",
    "cursor", "direction", "display", "displaystyle", "dominant-baseline", "fill", "fill-opacity",
    "fill-rule", "font-family", "font-size", "font-size-adjust", "font-stretch", "font-style",
    "font-variant", "font-weight", "letter-spacing", "marker-end", "marker-mid", "marker-start",
    "mathbackground", "mathcolor", "mathsize", "opacity", "paint-order", "pointer-events", "scriptlevel",
    "shape-rendering", "stop-color", "stop-opacity", "stroke", "stroke-dasharray", "stroke-dashoffset",
    "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-opacity", "stroke-width",
    "text-anchor", "text-decoration", "text-overflow", "text-rendering", "transform", "transform-origin",
    "unicode-bidi", "vector-effect", "visibility", "white-space", "word-spacing", "writing-mode",
    NULL
};

/* One (element, attribute) pair of §8.6.5's two built-in URL-attribute lists, and of the non-replaceable list
   (whose rows name no attribute). */
typedef struct { const char *el; const char *el_ns; const char *attr; const char *attr_ns; } SanPairRow;

/* §8.6.5's BUILT-IN SAFE BASELINE CONFIGURATION's `removeElements`: every HTML element its own definition marks
   Unsafe, the obsolete `frame`, and SVG's `script` and `use`. Its `removeAttributes` is empty — the event
   handler content attributes come out through remove unsafe's own third clause, not through this list. */
static const SanPairRow SAN_BASELINE_REMOVE_ELEMENTS[] = {
    { "base", SAN_NS_HTML, NULL, NULL }, { "embed", SAN_NS_HTML, NULL, NULL },
    { "frame", SAN_NS_HTML, NULL, NULL }, { "iframe", SAN_NS_HTML, NULL, NULL },
    { "object", SAN_NS_HTML, NULL, NULL }, { "script", SAN_NS_HTML, NULL, NULL },
    { "script", SAN_NS_SVG, NULL, NULL }, { "use", SAN_NS_SVG, NULL, NULL },
};

/* §8.6.5's BUILT-IN NAVIGATING URL ATTRIBUTES LIST: the HTML elements whose definitions mark one, plus the
   standard's own two-row table for SVG's `a`. */
static const SanPairRow SAN_NAVIGATING_URL_ATTRIBUTES[] = {
    { "a",      SAN_NS_HTML, "href",       NULL },
    { "area",   SAN_NS_HTML, "href",       NULL },
    { "button", SAN_NS_HTML, "formaction", NULL },
    { "form",   SAN_NS_HTML, "action",     NULL },
    { "input",  SAN_NS_HTML, "formaction", NULL },
    { "a",      SAN_NS_SVG,  "href",       NULL },
    { "a",      SAN_NS_SVG,  "href",       SAN_NS_XLINK },
};

/* §8.6.5's BUILT-IN ANIMATING URL ATTRIBUTES LIST. */
static const SanPairRow SAN_ANIMATING_URL_ATTRIBUTES[] = {
    { "animate",          SAN_NS_SVG, "attributeName", NULL },
    { "animateTransform", SAN_NS_SVG, "attributeName", NULL },
    { "set",              SAN_NS_SVG, "attributeName", NULL },
};

/* §8.6.5's BUILT-IN NON-REPLACEABLE ELEMENTS LIST — replacing one of these with its children re-parses into a
   different tree, which is why the standard refuses it rather than leaving it to the author. */
static const SanPairRow SAN_NON_REPLACEABLE[] = {
    { "html", SAN_NS_HTML,   NULL, NULL },
    { "svg",  SAN_NS_SVG,    NULL, NULL },
    { "math", SAN_NS_MATHML, NULL, NULL },
};

#define SAN_NROWS(t) ((int)(sizeof(t) / sizeof((t)[0])))

/* ---- the canonical configuration, and the operations §8.6.2 states over it -------------------------------- */

/* A canonical item's two members, as C strings. `ns` NULL is the spec's null namespace, which is a DIFFERENT
   thing from the empty string — §8.6.2's canonicalize a sanitizer name turns the empty string into null
   exactly so that nothing below has to know the difference. */
typedef struct { const char *name; const char *ns; } SanName;

static void san_name_release(JSContext *ctx, SanName *n)
{
    if (n->name) JS_FreeCString(ctx, n->name);
    if (n->ns) JS_FreeCString(ctx, n->ns);
    n->name = n->ns = NULL;
}

/* Read one canonical item. Returns false only for an item that is not an object, which is a config this file
   did not build. */
static bool san_name_of(JSContext *ctx, JSValueConst item, SanName *out)
{
    JSValue nv, sv;

    out->name = out->ns = NULL;
    if (!JS_IsObject(item)) return false;
    nv = JS_GetPropertyStr(ctx, item, "name");
    out->name = JS_ToCString(ctx, nv);
    JS_FreeValue(ctx, nv);
    sv = JS_GetPropertyStr(ctx, item, "namespace");
    if (!JS_IsNull(sv) && !JS_IsUndefined(sv)) out->ns = JS_ToCString(ctx, sv);
    JS_FreeValue(ctx, sv);
    return out->name != NULL;
}

static bool san_ns_eq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

/* §8.6.2's item equality: "all of their members are equal", which for a canonical element or attribute is the
   name and the namespace. */
static bool san_item_is(JSContext *ctx, JSValueConst item, const char *name, const char *ns)
{
    SanName n;
    bool eq;

    if (!san_name_of(ctx, item, &n)) { san_name_release(ctx, &n); return false; }
    eq = strcmp(n.name, name) == 0 && san_ns_eq(n.ns, ns);
    san_name_release(ctx, &n);
    return eq;
}

static uint32_t san_len(JSContext *ctx, JSValueConst list)
{
    JSValue lv;
    uint32_t n = 0;

    if (!JS_IsObject(list)) return 0;
    lv = JS_GetPropertyStr(ctx, list, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    return n;
}

/* The index of `name` in a canonical list, or -1 — the one primitive "contains", "remove" and "the item whose
   name member is" are all stated in terms of. */
static int san_index_of(JSContext *ctx, JSValueConst list, const char *name, const char *ns)
{
    uint32_t n = san_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        bool hit = san_item_is(ctx, e, name, ns);

        JS_FreeValue(ctx, e);
        if (hit) return (int)i;
    }
    return -1;
}

static bool san_contains(JSContext *ctx, JSValueConst list, const char *name, const char *ns)
{
    return san_index_of(ctx, list, name, ns) >= 0;
}

/* A processing instruction list is keyed on `target` alone — §8.6.2 canonicalizes a SanitizerPI to exactly
   that one member, so it is its own lookup rather than a namespace-less name. */
static int san_pi_index_of(JSContext *ctx, JSValueConst list, const char *target)
{
    uint32_t n = san_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);
        JSValue t = JS_IsObject(e) ? JS_GetPropertyStr(ctx, e, "target") : JS_UNDEFINED;
        const char *s = JS_ToCString(ctx, t);
        bool hit = s && strcmp(s, target) == 0;

        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, t);
        JS_FreeValue(ctx, e);
        if (hit) return (int)i;
    }
    return -1;
}

/* Remove one entry, keeping the rest in order — Infra's "remove" over a list, which is what every one of
   §8.6.2's removes is. */
static void san_remove_at(JSContext *ctx, JSValueConst list, int idx)
{
    uint32_t n = san_len(ctx, list), i;

    DCHECK(idx >= 0 && (uint32_t)idx < n, "a sanitizer list entry was removed at an index it does not have");
    for (i = (uint32_t)idx + 1; i < n; i++)
        JS_SetPropertyUint32(ctx, list, i - 1, JS_GetPropertyUint32(ctx, list, i));
    JS_SetPropertyStr(ctx, list, "length", JS_NewUint32(ctx, n - 1));
}

static void san_append(JSContext *ctx, JSValueConst list, JSValue item)
{
    JS_SetPropertyUint32(ctx, list, san_len(ctx, list), item);
}

/* "Remove `name` from this list if it is there" — returns whether anything changed, which is what §8.6.2's
   modifier methods answer with. */
static bool san_list_remove(JSContext *ctx, JSValueConst list, const char *name, const char *ns)
{
    int idx;

    if (!JS_IsObject(list)) return false;
    idx = san_index_of(ctx, list, name, ns);
    if (idx < 0) return false;
    san_remove_at(ctx, list, idx);
    return true;
}

/* Does the configuration HAVE this member — §8.6's "exists", which is not the same question as its value.
   A member set to `undefined` does not exist, which is how this file spells "not present". */
static bool san_has(JSContext *ctx, JSValueConst cfg, const char *member)
{
    JSValue v = JS_GetPropertyStr(ctx, cfg, member);
    bool has = !JS_IsUndefined(v);

    JS_FreeValue(ctx, v);
    return has;
}

static JSValue san_get(JSContext *ctx, JSValueConst cfg, const char *member)
{
    return JS_GetPropertyStr(ctx, cfg, member);
}

static JSValue san_new_item(JSContext *ctx, const char *name, const char *ns)
{
    JSValue o = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, o, "namespace", ns ? JS_NewString(ctx, ns) : JS_NULL);
    return o;
}

/* A space-separated attribute list from §8.6.5's tables, as the canonical `attributes` sequence the element's
   own row states. Every one of them is in the null namespace. */
static JSValue san_attrs_of_row(JSContext *ctx, const char *attrs)
{
    JSValue list = JS_NewArray(ctx);
    const char *p = attrs;

    while (p && *p) {
        const char *e = strchr(p, ' ');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        JSValue item = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, item, "name", JS_NewStringLen(ctx, p, len));
        JS_SetPropertyStr(ctx, item, "namespace", JS_NULL);
        san_append(ctx, list, item);
        p = e ? e + 1 : NULL;
    }
    return list;
}

static void san_add_rows(JSContext *ctx, JSValueConst elements, const SanElementRow *rows, int n,
                         const char *ns)
{
    int i;

    for (i = 0; i < n; i++) {
        JSValue el = san_new_item(ctx, rows[i].name, ns);

        /* §8.6.5: "Attributes included in those definitions are included in each element's respective
           attributes list" — and §8.6.2's canonicalize a SanitizerElementWithAttributes gives an element with
           neither list an EMPTY removeAttributes, which is what makes the config valid. */
        if (rows[i].attrs) JS_SetPropertyStr(ctx, el, "attributes", san_attrs_of_row(ctx, rows[i].attrs));
        else               JS_SetPropertyStr(ctx, el, "removeAttributes", JS_NewArray(ctx));
        san_append(ctx, elements, el);
    }
}

/* §8.6.5's BUILT-IN SAFE DEFAULT CONFIGURATION, canonical and valid by construction. */
static JSValue san_default_config(JSContext *ctx)
{
    JSValue cfg = JS_NewObject(ctx);
    JSValue elements = JS_NewArray(ctx);
    JSValue attributes = JS_NewArray(ctx);
    int i;

    san_add_rows(ctx, elements, SAN_DEFAULT_HTML, SAN_NROWS(SAN_DEFAULT_HTML), SAN_NS_HTML);
    san_add_rows(ctx, elements, SAN_DEFAULT_MATHML, SAN_NROWS(SAN_DEFAULT_MATHML), SAN_NS_MATHML);
    san_add_rows(ctx, elements, SAN_DEFAULT_SVG, SAN_NROWS(SAN_DEFAULT_SVG), SAN_NS_SVG);
    for (i = 0; SAN_DEFAULT_ATTRIBUTES[i]; i++)
        san_append(ctx, attributes, san_new_item(ctx, SAN_DEFAULT_ATTRIBUTES[i], NULL));
    JS_SetPropertyStr(ctx, cfg, "elements", elements);
    JS_SetPropertyStr(ctx, cfg, "attributes", attributes);
    JS_SetPropertyStr(ctx, cfg, "processingInstructions", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, cfg, "comments", JS_FALSE);
    JS_SetPropertyStr(ctx, cfg, "dataAttributes", JS_FALSE);
    return cfg;
}

/* §8.6.2's CANONICALIZE THE CONFIGURATION over an EMPTY dictionary, which is the only configuration dictionary
   this engine can be handed without the page's code running: steps 1, 2 and 3 give the three remove-lists and
   step 10 the `comments` member. It is `SetHTMLUnsafeOptions`'s `sanitizer = {}` default — a configuration
   that allows everything — and with `allowCommentsPIsAndDataAttributes` false it is the empty SAFE one. */
static JSValue san_empty_config(JSContext *ctx, bool allow_comments_pis_data)
{
    JSValue cfg = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, cfg, "removeElements", JS_NewArray(ctx));       /* step 1 */
    JS_SetPropertyStr(ctx, cfg, "removeAttributes", JS_NewArray(ctx));     /* step 2 */
    /* Step 3: which of the two PI lists is created is the flag's own question — an empty ALLOW list allows no
       processing instruction at all, which is what a safe configuration means by not naming one. */
    JS_SetPropertyStr(ctx, cfg, allow_comments_pis_data ? "removeProcessingInstructions"
                                                        : "processingInstructions", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, cfg, "comments", JS_NewBool(ctx, allow_comments_pis_data));   /* step 10 */
    /* Step 11 sets `dataAttributes` only when an `attributes` allow-list exists, and this configuration has
       none — which is also why §8.6.3's validity says a `dataAttributes` beside a remove-list is invalid. */
    return cfg;
}

/* §8.6.2's "REMOVE AN ELEMENT FROM A CONFIGURATION". Returns whether the configuration changed. */
static bool san_config_remove_element(JSContext *ctx, JSValueConst cfg, const char *name, const char *ns)
{
    JSValue rwc = san_get(ctx, cfg, "replaceWithChildrenElements");
    JSValue list;
    bool modified = san_list_remove(ctx, rwc, name, ns);   /* step 3 */

    JS_FreeValue(ctx, rwc);
    if (san_has(ctx, cfg, "elements")) {                   /* step 4 */
        list = san_get(ctx, cfg, "elements");
        if (san_list_remove(ctx, list, name, ns)) modified = true;
        JS_FreeValue(ctx, list);
        return modified;
    }
    list = san_get(ctx, cfg, "removeElements");            /* step 5 */
    DCHECK(JS_IsObject(list), "§8.6.3: a canonical configuration has an elements list or a removeElements one, "
                              "and this one has neither — it was built by something that did not canonicalize");
    if (san_contains(ctx, list, name, ns)) { JS_FreeValue(ctx, list); return modified; }
    san_append(ctx, list, san_new_item(ctx, name, ns));
    JS_FreeValue(ctx, list);
    return true;
}

/* §8.6.2's "REMOVE AN ATTRIBUTE FROM A CONFIGURATION". */
static bool san_config_remove_attribute(JSContext *ctx, JSValueConst cfg, const char *name, const char *ns)
{
    JSValue elements = san_get(ctx, cfg, "elements");
    JSValue list;
    bool modified;
    uint32_t n, i;

    if (san_has(ctx, cfg, "attributes")) {                 /* step 3 */
        list = san_get(ctx, cfg, "attributes");
        modified = san_list_remove(ctx, list, name, ns);
        JS_FreeValue(ctx, list);
        n = san_len(ctx, elements);
        for (i = 0; i < n; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, elements, i);
            JSValue local = JS_GetPropertyStr(ctx, el, "attributes");

            if (san_list_remove(ctx, local, name, ns)) modified = true;
            JS_FreeValue(ctx, local);
            local = JS_GetPropertyStr(ctx, el, "removeAttributes");
            if (san_list_remove(ctx, local, name, ns))
                DCHECK(modified, "§8.6.2 step 3.2.2.2: an attribute in an element's local removeAttributes was "
                                 "not in the global attributes allow-list — §8.6.3's validity requires the "
                                 "local remove list to be a subset of it");
            JS_FreeValue(ctx, local);
            JS_FreeValue(ctx, el);
        }
        JS_FreeValue(ctx, elements);
        return modified;
    }
    list = san_get(ctx, cfg, "removeAttributes");           /* step 4 */
    DCHECK(JS_IsObject(list), "§8.6.3: a canonical configuration has an attributes list or a removeAttributes "
                              "one, and this one has neither");
    if (san_contains(ctx, list, name, ns)) {
        JS_FreeValue(ctx, list);
        JS_FreeValue(ctx, elements);
        return false;
    }
    n = san_len(ctx, elements);
    for (i = 0; i < n; i++) {
        JSValue el = JS_GetPropertyUint32(ctx, elements, i);
        JSValue local = JS_GetPropertyStr(ctx, el, "attributes");

        san_list_remove(ctx, local, name, ns);
        JS_FreeValue(ctx, local);
        local = JS_GetPropertyStr(ctx, el, "removeAttributes");
        san_list_remove(ctx, local, name, ns);
        JS_FreeValue(ctx, local);
        JS_FreeValue(ctx, el);
    }
    san_append(ctx, list, san_new_item(ctx, name, ns));
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, elements);
    return true;
}

/* §8.6.2's "REMOVE UNSAFE FROM A CONFIGURATION": the baseline's remove-lists, and then EVERY event handler
   content attribute. */
static bool san_config_remove_unsafe(JSContext *ctx, JSValueConst cfg)
{
    bool result = false;
    int i;

    for (i = 0; i < SAN_NROWS(SAN_BASELINE_REMOVE_ELEMENTS); i++)
        if (san_config_remove_element(ctx, cfg, SAN_BASELINE_REMOVE_ELEMENTS[i].el,
                                      SAN_BASELINE_REMOVE_ELEMENTS[i].el_ns))
            result = true;
    /* STEP 4: EVERY EVENT HANDLER CONTENT ATTRIBUTE, REMOVED FROM THE CONFIGURATION. It is one call per name
       through the operation that already exists, because "remove an attribute from a configuration" is the
       same algorithm whichever list the configuration has — an allow-list drops the entry, a remove-list gains
       one — and san_config_remove_attribute is where both arms live.
       WHAT WAS HERE INSTEAD was a bespoke walk of the allow-list plus a DFAIL for the other shape, and the
       DFAIL was right about why: filtering an allow-list for the entries that ARE handlers is only equivalent
       to this step while the configuration happens to be an allow-list. Over a remove-list the step is an
       APPEND per name, which needs the names — event_target.c now enumerates them off the one X-list §8.1.7.2
       defines the set by, so a handler added there is removed here without anything else being edited.
       §8.6.5's built-in safe baseline has an EMPTY removeAttributes list precisely because the attributes it
       would otherwise carry are these, named here rather than duplicated there. */
    for (i = 0; i < event_target_handler_attribute_count(); i++)
        if (san_config_remove_attribute(ctx, cfg, event_target_handler_attribute_at(i), NULL))
            result = true;
    return result;
}

/* §8.6.2's "COMPARE SANITIZER ITEMS" — the null namespace sorts first, then namespaces by code unit, then
   names. `get()` is the one place the order is observable, which is why it is stated once here. */
static bool san_item_less(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    SanName na, nb;
    bool less;

    san_name_of(ctx, a, &na);
    san_name_of(ctx, b, &nb);
    if (!na.ns && nb.ns)      less = true;
    else if (na.ns && !nb.ns) less = false;
    else if (na.ns && nb.ns && strcmp(na.ns, nb.ns) != 0) less = strcmp(na.ns, nb.ns) < 0;
    else                      less = na.name && nb.name && strcmp(na.name, nb.name) < 0;
    san_name_release(ctx, &na);
    san_name_release(ctx, &nb);
    return less;
}

static bool san_pi_less(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    JSValue ta = JS_GetPropertyStr(ctx, a, "target"), tb = JS_GetPropertyStr(ctx, b, "target");
    const char *sa = JS_ToCString(ctx, ta), *sb = JS_ToCString(ctx, tb);
    bool less = sa && sb && strcmp(sa, sb) < 0;

    if (sa) JS_FreeCString(ctx, sa);
    if (sb) JS_FreeCString(ctx, sb);
    JS_FreeValue(ctx, ta);
    JS_FreeValue(ctx, tb);
    return less;
}

/* Sort one of the configuration's lists in place. An insertion sort because the list it is asked of is one of
   §8.6.5's, whose size is this file's own table — see the header comment on what walks page-sized data. */
static void san_sort(JSContext *ctx, JSValueConst list, bool pi)
{
    uint32_t n = san_len(ctx, list), i;

    for (i = 1; i < n; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, list, i);
        uint32_t j = i;

        while (j > 0) {
            JSValue prev = JS_GetPropertyUint32(ctx, list, j - 1);
            bool swap = pi ? san_pi_less(ctx, v, prev) : san_item_less(ctx, v, prev);

            if (!swap) { JS_FreeValue(ctx, prev); break; }
            JS_SetPropertyUint32(ctx, list, j, prev);
            j--;
        }
        JS_SetPropertyUint32(ctx, list, j, v);
    }
}

/* A deep copy of a canonical list, which is what `get()` hands the page: the spec's dictionaries are values,
   so a page that mutates the result must not be mutating the Sanitizer. */
static JSValue san_copy_list(JSContext *ctx, JSValueConst list, bool with_local_attributes);

static JSValue san_copy_item(JSContext *ctx, JSValueConst item, bool with_local_attributes)
{
    JSValue o = JS_NewObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, item, "target");

    if (!JS_IsUndefined(v)) {                              /* a processing instruction: one member */
        JS_SetPropertyStr(ctx, o, "target", v);
        return o;
    }
    JS_FreeValue(ctx, v);
    JS_SetPropertyStr(ctx, o, "name", JS_GetPropertyStr(ctx, item, "name"));
    JS_SetPropertyStr(ctx, o, "namespace", JS_GetPropertyStr(ctx, item, "namespace"));
    if (with_local_attributes) {
        v = JS_GetPropertyStr(ctx, item, "attributes");
        if (!JS_IsUndefined(v)) JS_SetPropertyStr(ctx, o, "attributes", san_copy_list(ctx, v, false));
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, item, "removeAttributes");
        if (!JS_IsUndefined(v)) JS_SetPropertyStr(ctx, o, "removeAttributes", san_copy_list(ctx, v, false));
        JS_FreeValue(ctx, v);
    }
    return o;
}

static JSValue san_copy_list(JSContext *ctx, JSValueConst list, bool with_local_attributes)
{
    JSValue out = JS_NewArray(ctx);
    uint32_t n = san_len(ctx, list), i;

    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, list, i);

        JS_SetPropertyUint32(ctx, out, i, san_copy_item(ctx, e, with_local_attributes));
        JS_FreeValue(ctx, e);
    }
    return out;
}

/* ---- the Sanitizer object -------------------------------------------------------------------------------- */

typedef struct { JSValue config; } SanitizerRec;

static JSClassID g_class;
static int g_ready;
static int g_id_ctor = -1, g_id_get = -1, g_id_remove_unsafe = -1, g_id_set_comments = -1,
           g_id_set_data_attributes = -1;

/* THE RECORD TIME-TRAVELS. Its one owned value is named here and freed by the finalizer below — one list, read
   together, so a field added to the record is a field both of them get. */
static const uint16_t SANITIZER_VALS[] = { (uint16_t)offsetof(SanitizerRec, config) };
static const CowRecord SANITIZER_REC = { sizeof(SanitizerRec), SANITIZER_VALS, 1 };

/* The capture belongs HERE, in the accessor: a flow that has reached the record is one that may write it, and
   there is then no write site left to miss. */
static SanitizerRec *san_rec_of(JSValueConst v)
{
    SanitizerRec *r = JS_GetOpaque(v, g_class);

    if (r) cow_capture_host_record(v, r, &SANITIZER_REC);
    return r;
}

static SanitizerRec *san_rec_here(JSContext *ctx, JSValueConst v)
{
    SanitizerRec *r = san_rec_of(v);

    if (!r) JS_ThrowTypeError(ctx, "a Sanitizer member was reached on something that is not a Sanitizer");
    return r;
}

bool sanitizer_is(JSValueConst v)
{
    return JS_GetOpaque(v, g_class) != NULL;
}

static void san_finalizer(JSRuntime *rt, JSValue val)
{
    SanitizerRec *r = JS_GetOpaque(val, g_class);   /* NOT the accessor: a capture during collection would dup */

    if (!r) return;
    JS_FreeValueRT(rt, r->config);
    free(r);
}

static void san_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    SanitizerRec *r = JS_GetOpaque(val, g_class);

    if (!r) return;
    JS_MarkValue(rt, r->config, mark_func);
}

/* §8.6.2's `configuration` slot, filled — `config` is CONSUMED. */
static JSValue san_new_object(JSContext *ctx, JSValue config)
{
    JSValue proto = JS_GetClassProto(ctx, g_class);
    JSValue obj;
    SanitizerRec *r;

    DCHECK(!JS_IsNull(proto), "a Sanitizer was constructed in a realm that never ran its prototype install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, config); return obj; }
    r = calloc(1, sizeof *r);
    CHECK(r != NULL, "a Sanitizer's configuration record could not be allocated");
    r->config = config;
    JS_SetOpaque(obj, r);
    return obj;
}

/* §8.6.4's "if sanitizerSpec is a string / a dictionary" arms, over the ONE value a member was handed — the
   `sanitizer` option and the constructor's argument are the same union, so they resolve it in one place.
   Returns an OWNED canonical configuration, or JS_EXCEPTION with a TypeError pending. */
static JSValue san_config_from_spec(JSContext *ctx, JSValueConst spec)
{
    DCHECK(!JS_IsUndefined(spec), "§8.6.4 step 4.1 resolved an ABSENT sanitizer here — which default it takes "
                                  "is the DICTIONARY's (\"default\" for SetHTMLOptions, {} for "
                                  "SetHTMLUnsafeOptions), so the caller answers it and this never sees one");
    if (sanitizer_is(spec)) {
        /* §8.6.4 step 5 returns the INSTANCE, and `sanitize` step 3 then mutates ITS configuration — so this
           is the same object the page holds, not a copy of it. */
        SanitizerRec *r = san_rec_of(spec);

        return JS_DupValue(ctx, r->config);
    }
    if (JS_IsString(spec)) {
        const char *s = JS_ToCString(ctx, spec);
        bool is_default = s && strcmp(s, "default") == 0;

        if (s) JS_FreeCString(ctx, s);
        /* `enum SanitizerPresets { "default" }` — §3.2.19: a string that is not one of the enumeration's
           values is a TypeError, never a silent fall back to the default. */
        if (!is_default)
            return JS_ThrowTypeError(ctx, "the sanitizer is neither a Sanitizer, a SanitizerConfig nor the "
                                          "SanitizerPresets value \"default\"");
        return san_default_config(ctx);
    }
    DFAIL("§8.6.3's SanitizerConfig was supplied as a DICTIONARY and this engine cannot convert one — its "
          "members are `sequence<(DOMString or SanitizerElementNamespace)>` and every step of §3.2.21's "
          "iterator protocol is the page's code, as is each entry's `name`/`namespace`/`attributes` read. "
          "Build that conversion as a step machine (an IterCursor per list plus an inner one for an element's "
          "own attributes lists, resting on the entry it is on) and run §8.6.2's canonicalize the "
          "configuration over its result — a C walk here would drive the page's iterator to completion");
    /* A RELEASE BUILD CANNOT BUILD THE MISSING MACHINE, so the same unsupported arm is the page's TypeError
       rather than a configuration that is not one — the abort above is what a dev build answers. */
    return JS_ThrowTypeError(ctx, "a SanitizerConfig dictionary is not yet convertible by this engine");
}

JSValue sanitizer_config_from_options(JSContext *ctx, JSValueConst options, bool safe)
{
    JSValue spec = idl_dict_get(ctx, options, "sanitizer");
    JSValue cfg;

    /* THE ABSENT MEMBER'S DEFAULT IS THE DICTIONARY'S OWN, and the two are different: SetHTMLOptions declares
       `= "default"` and SetHTMLUnsafeOptions `= {}`. That one line is the whole difference between a safe
       member's built-in allow-list and an unsafe member's allow-everything. */
    if (JS_IsUndefined(spec))
        cfg = safe ? san_default_config(ctx)
                   : san_empty_config(ctx, /*allowCommentsPIsAndDataAttributes*/ true);
    else
        cfg = san_config_from_spec(ctx, spec);
    JS_FreeValue(ctx, spec);
    return cfg;
}

/* §8.6.2's constructor: `new Sanitizer(optional (SanitizerConfig or SanitizerPresets) configuration =
   "default")`. Its steps are the preset resolution and then `configure`, whose canonicalize-and-validate is
   what san_config_from_spec's arms already produce — the built-in configurations are canonical and valid by
   construction, and the arm that is not yet convertible never reaches here. */
static JSValue js_san_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    JSValue cfg;

    (void)magic;
    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "constructor Sanitizer requires 'new'");
    /* The argument's IDL default is the PRESET `"default"`, which step 1.2 turns into the built-in safe
       default configuration — a Sanitizer built with no argument is the safe one, and `sanitize` is what
       removes unsafe on top of it when a SAFE member asked for it. */
    if (argc == 0 || JS_IsUndefined(argv[0])) cfg = san_default_config(ctx);
    else                                      cfg = san_config_from_spec(ctx, argv[0]);
    if (JS_IsException(cfg)) return cfg;
    return san_new_object(ctx, cfg);
}

/* §8.6.2's `get()`: a COPY of the configuration, with every list sorted. */
static JSValue js_san_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    static const char *const LISTS[] = { "elements", "removeElements", "replaceWithChildrenElements",
                                         "attributes", "removeAttributes", NULL };
    static const char *const PI_LISTS[] = { "processingInstructions", "removeProcessingInstructions", NULL };
    SanitizerRec *r = san_rec_here(ctx, this_val);
    JSValue out;
    int i;

    (void)argc; (void)argv; (void)magic;
    if (!r) return JS_EXCEPTION;
    out = JS_NewObject(ctx);
    for (i = 0; LISTS[i]; i++) {
        JSValue l = san_get(ctx, r->config, LISTS[i]);

        if (!JS_IsUndefined(l)) {
            /* The per-element `attributes` and `removeAttributes` lists are sorted with the same comparison,
               which is what steps 3.1.1 and 3.1.2 say before step 3.2 sorts the elements themselves. */
            JSValue copy = san_copy_list(ctx, l, strcmp(LISTS[i], "elements") == 0);
            uint32_t n = san_len(ctx, copy), k;

            for (k = 0; k < n; k++) {
                JSValue el = JS_GetPropertyUint32(ctx, copy, k);
                JSValue sub = JS_GetPropertyStr(ctx, el, "attributes");

                if (!JS_IsUndefined(sub)) san_sort(ctx, sub, false);
                JS_FreeValue(ctx, sub);
                sub = JS_GetPropertyStr(ctx, el, "removeAttributes");
                if (!JS_IsUndefined(sub)) san_sort(ctx, sub, false);
                JS_FreeValue(ctx, sub);
                JS_FreeValue(ctx, el);
            }
            san_sort(ctx, copy, false);
            JS_SetPropertyStr(ctx, out, LISTS[i], copy);
        }
        JS_FreeValue(ctx, l);
    }
    for (i = 0; PI_LISTS[i]; i++) {
        JSValue l = san_get(ctx, r->config, PI_LISTS[i]);

        if (!JS_IsUndefined(l)) {
            JSValue copy = san_copy_list(ctx, l, false);

            san_sort(ctx, copy, true);
            JS_SetPropertyStr(ctx, out, PI_LISTS[i], copy);
        }
        JS_FreeValue(ctx, l);
    }
    for (i = 0; i < 2; i++) {
        const char *m = i ? "dataAttributes" : "comments";
        JSValue v = san_get(ctx, r->config, m);

        if (!JS_IsUndefined(v)) JS_SetPropertyStr(ctx, out, m, v);
        else JS_FreeValue(ctx, v);
    }
    return out;
}

/* §8.6.2's `removeUnsafe()`. */
static JSValue js_san_remove_unsafe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                    int magic)
{
    SanitizerRec *r = san_rec_here(ctx, this_val);

    (void)argc; (void)argv; (void)magic;
    if (!r) return JS_EXCEPTION;
    return JS_NewBool(ctx, san_config_remove_unsafe(ctx, r->config));
}

/* §8.6.2's `setComments(allow)` and `setDataAttributes(allow)` — magic 0 and 1. Both answer whether the
   configuration changed, and `setDataAttributes` is refused outright on a configuration with no `attributes`
   allow-list, because §8.6.3 makes the member an extension of that list. */
static JSValue js_san_set_flag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    SanitizerRec *r = san_rec_here(ctx, this_val);
    const char *member = magic ? "dataAttributes" : "comments";
    bool allow, had;
    JSValue cur;

    if (!r) return JS_EXCEPTION;
    /* §3.6.2 threw for a call with no argument before this body ran, and the declaration converted what there
       was — so this is ToBoolean over a value that is already one, or over unknown external input, which
       forks rather than being asserted about. */
    allow = argc > 0 && JS_ToBool(ctx, argv[0]);
    if (magic && !san_has(ctx, r->config, "attributes")) return JS_FALSE;    /* step 3 */
    cur = san_get(ctx, r->config, member);
    had = !JS_IsUndefined(cur) && JS_ToBool(ctx, cur) == allow;
    JS_FreeValue(ctx, cur);
    if (had) return JS_FALSE;
    if (magic && allow) {
        /* Step 5: a custom data attribute may not be listed in an allow-list once they are allowed globally —
           §8.6.3's validity says so, so the lists are cleaned rather than left contradicting it. */
        JSValue elements = san_get(ctx, r->config, "elements");
        JSValue attrs = san_get(ctx, r->config, "attributes");
        uint32_t n = san_len(ctx, elements), i;

        for (i = 0; i < n; i++) {
            JSValue el = JS_GetPropertyUint32(ctx, elements, i);
            JSValue local = JS_GetPropertyStr(ctx, el, "attributes");

            if (!JS_IsUndefined(local)) {
                uint32_t m = san_len(ctx, local), k = 0;

                while (k < m) {
                    JSValue e = JS_GetPropertyUint32(ctx, local, k);
                    SanName nm;
                    bool data;

                    san_name_of(ctx, e, &nm);
                    data = nm.name && nm.ns == NULL && strncmp(nm.name, "data-", 5) == 0;
                    san_name_release(ctx, &nm);
                    JS_FreeValue(ctx, e);
                    if (!data) { k++; continue; }
                    san_remove_at(ctx, local, (int)k);
                    m--;
                }
            }
            JS_FreeValue(ctx, local);
            JS_FreeValue(ctx, el);
        }
        {
            uint32_t m = san_len(ctx, attrs), k = 0;

            while (k < m) {
                JSValue e = JS_GetPropertyUint32(ctx, attrs, k);
                SanName nm;
                bool data;

                san_name_of(ctx, e, &nm);
                data = nm.name && nm.ns == NULL && strncmp(nm.name, "data-", 5) == 0;
                san_name_release(ctx, &nm);
                JS_FreeValue(ctx, e);
                if (!data) { k++; continue; }
                san_remove_at(ctx, attrs, (int)k);
                m--;
            }
        }
        JS_FreeValue(ctx, attrs);
        JS_FreeValue(ctx, elements);
    }
    JS_SetPropertyStr(ctx, r->config, member, JS_NewBool(ctx, allow));
    return JS_TRUE;
}

/* ---- §8.6.4's inner sanitize steps, as the walk --------------------------------------------------------- */

/* This machine's own numbering of the shared X-list. The host expands the SAME list into its stage enum at its
   own base and hands that base to `begin`, so neither file can renumber without the other. */
enum { SANITIZE_STAGES(JS_STEP_STAGE_ENUM) };

static void san_push(SanitizerWalk *w, lxb_dom_node_t *node, lxb_dom_node_t *root, int after)
{
    if (w->sp == w->scap) {
        int want = w->scap ? w->scap * 2 : 8;
        SanLevel *n = realloc(w->stack, sizeof(SanLevel) * (size_t)want);

        CHECK(n != NULL, "the sanitizer walk could not grow its level stack");
        w->stack = n;
        w->scap = want;
    }
    w->stack[w->sp].node = node;
    w->stack[w->sp].root = root;
    w->stack[w->sp].after = (uint8_t)after;
    w->sp++;
}

void sanitizer_walk_begin(JSContext *ctx, SanitizerWalk *w, lxb_dom_node_t *root, JSValue config,
                          bool safe, int stage_base)
{
    memset(w, 0, sizeof *w);
    w->config = config;
    w->handle_js_urls = safe ? 1 : 0;
    w->stage_base = stage_base;
    w->root = root;
    w->tree_root = root;
    w->cur = root ? root->first_child : NULL;
    /* §8.6.4 sanitize step 3: a SAFE sanitization removes what is unsafe from the configuration FIRST, so
       nothing the baseline names can be allowed by anything the caller wrote. */
    if (safe && JS_IsObject(config)) san_config_remove_unsafe(ctx, config);
}

void sanitizer_walk_visit(JSContext *ctx, SanitizerWalk *w, JSStepVisit *v)
{
    v->val(ctx, &w->config);
    /* The level stack is plain storage a forked arm must not share — each arm unwinds its own. The DOM
       pointers in it are per-flow COW nodes, which every arm reaches by the same address. */
    v->buf(ctx, (void **)&w->stack, sizeof(SanLevel) * (size_t)w->scap);
}

void sanitizer_walk_release(JSContext *ctx, SanitizerWalk *w)
{
    JS_FreeValue(ctx, w->config);
    w->config = JS_UNDEFINED;
    free(w->stack);
    w->stack = NULL;
    w->scap = w->sp = 0;
}

/* An element's name and namespace as §8.6.4's `elementName` — the two members of a SanitizerElementNamespace,
   read off the node. */
typedef struct { char ns[128]; char local[192]; const char *nsp; const char *lp; } SanNodeName;

static void san_node_name(lxb_dom_node_t *n, SanNodeName *out)
{
    element_ns_and_local(lxb_dom_interface_element(n), &out->nsp, &out->lp,
                         out->ns, sizeof out->ns, out->local, sizeof out->local);
    DCHECK(out->lp != NULL && out->lp[0] != 0,
           "an element in the tree has no local name to match against the sanitizer's lists — a name longer "
           "than this buffer would answer the empty string, and an allow-list would then remove the element");
}

/* §8.6.4's "does this attribute contain a javascript: URL": the BASIC URL PARSER over its value, and the
   scheme of what it produces. Not a prefix test — the parser is what decides that `\njavascript\t:x` is one. */
static bool san_attr_is_javascript_url(const lxb_dom_attr_t *a)
{
    size_t len = 0;
    const lxb_char_t *v = lxb_dom_attr_value((lxb_dom_attr_t *)a, &len);
    UrlRecord u;
    bool js;

    if (!v) return false;
    url_record_init(&u);
    if (!url_parse(&u, (const char *)v, len, NULL)) { url_record_free(&u); return false; }
    js = u.scheme && strcmp(u.scheme, "javascript") == 0;
    url_record_free(&u);
    return js;
}

static bool san_pair_lists(const SanPairRow *rows, int n, const char *el, const char *el_ns,
                           const char *attr, const char *attr_ns)
{
    int i;

    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].el, el) != 0 || !san_ns_eq(rows[i].el_ns, el_ns)) continue;
        if (!rows[i].attr) return true;                       /* a row that names no attribute: the element */
        if (strcmp(rows[i].attr, attr) != 0) continue;
        if (!san_ns_eq(rows[i].attr_ns, attr_ns)) continue;
        return true;
    }
    return false;
}

/* Step 1's REMOVAL, entered with the doomed child. The subtree is freed deepest node first, ONE per step: the
   fragment is a private tree, so its nodes are destroyed through dom_cow's private operations rather than the
   capturing chokepoint, and the private destroy takes a LEAF — which is also what makes a page-sized subtree a
   page-sized number of rest points instead of one long stall. */
static void san_begin_remove(JSContext *ctx, JSStepHdr *hdr, SanitizerWalk *w, lxb_dom_node_t *child)
{
    DCHECK(child != NULL, "the sanitizer walk was asked to remove nothing");
    /* A SHADOW HOST'S SHADOW ROOT IS NOT ONE OF ITS CHILDREN, so freeing the host's subtree leaves the shadow
       tree reachable from nothing — a leak the runtime's own GC walk cannot attribute. */
    DCHECK(!(child->type == LXB_DOM_NODE_TYPE_ELEMENT &&
             shadow_root_of_element(ctx, lxb_dom_interface_element(child)) != NULL),
           "the sanitizer removed a SHADOW HOST — its shadow root is not a child and would be freed by "
           "nothing; build the shadow-root teardown into the removal before a configuration can remove one");
    w->doomed = child;
    w->dead = child;
    w->dead_root = w->tree_root;
    w->dead_depth = 0;
    w->next_sib = child->next;
    hdr->stage = w->stage_base + SAN_REMOVE;
}

int sanitizer_walk_step(JSContext *ctx, JSStepHdr *hdr, SanitizerWalk *w)
{
    int stage = hdr->stage - w->stage_base;

    DCHECK(stage >= 0 && stage <= SAN_POP, "the sanitizer walk was resumed at a stage §8.6.4 does not have");

    switch (stage) {
    case SAN_CHILD: {
        lxb_dom_node_t *child = w->cur;
        JSValue list;

        if (!child) { hdr->stage = w->stage_base + SAN_POP; return JS_STEP_YIELD; }
        /* Step 1.1, and it is an assert in the standard because a parsed tree holds nothing else. */
        DCHECK(child->type == LXB_DOM_NODE_TYPE_TEXT || child->type == LXB_DOM_NODE_TYPE_CDATA_SECTION ||
               child->type == LXB_DOM_NODE_TYPE_COMMENT || child->type == LXB_DOM_NODE_TYPE_ELEMENT ||
               child->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION ||
               child->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE,
               "§8.6.4 step 1.1: a child of the sanitized tree is none of the five node kinds the algorithm "
               "admits");
        if (child->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE || child->type == LXB_DOM_NODE_TYPE_TEXT ||
            child->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {                       /* step 1.2 */
            hdr->stage = w->stage_base + SAN_NEXT;
            return JS_STEP_YIELD;
        }
        if (child->type == LXB_DOM_NODE_TYPE_COMMENT) {                             /* step 1.3 */
            JSValue c = san_get(ctx, w->config, "comments");
            bool keep = JS_IsBool(c) && JS_ToBool(ctx, c);

            JS_FreeValue(ctx, c);
            if (keep) hdr->stage = w->stage_base + SAN_NEXT;
            else      san_begin_remove(ctx, hdr, w, child);
            return JS_STEP_YIELD;
        }
        if (child->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION) {              /* step 1.4 */
            size_t tlen = 0;
            const lxb_char_t *t = lxb_dom_processing_instruction_target(
                                      lxb_dom_interface_processing_instruction(child), &tlen);
            char target[192];
            bool drop;

            DCHECK(t != NULL && tlen < sizeof target,
                   "a processing instruction's target does not fit the sanitizer's buffer — a truncated one "
                   "would be matched against the configuration's lists as a different target");
            memcpy(target, t, tlen);
            target[tlen] = 0;
            if (san_has(ctx, w->config, "processingInstructions")) {                /* step 1.4.2 */
                list = san_get(ctx, w->config, "processingInstructions");
                drop = san_pi_index_of(ctx, list, target) < 0;
            } else {                                                                /* step 1.4.3 */
                list = san_get(ctx, w->config, "removeProcessingInstructions");
                DCHECK(JS_IsObject(list), "§8.6.3: a canonical configuration has one of the two processing "
                                          "instruction lists, and this one has neither");
                drop = san_pi_index_of(ctx, list, target) >= 0;
            }
            JS_FreeValue(ctx, list);
            if (drop) san_begin_remove(ctx, hdr, w, child);
            else      hdr->stage = w->stage_base + SAN_NEXT;
            return JS_STEP_YIELD;
        }
        hdr->stage = w->stage_base + SAN_ELEMENT;                                   /* step 1.5 */
        return JS_STEP_YIELD;
    }

    case SAN_ELEMENT: {
        SanNodeName nm;
        JSValue list;
        bool drop;

        san_node_name(w->cur, &nm);                                                 /* step 1.5.1 */
        if (san_has(ctx, w->config, "replaceWithChildrenElements")) {               /* step 1.5.2 */
            list = san_get(ctx, w->config, "replaceWithChildrenElements");
            drop = san_contains(ctx, list, nm.lp, nm.nsp);
            JS_FreeValue(ctx, list);
            DCHECK(!drop || !san_pair_lists(SAN_NON_REPLACEABLE, SAN_NROWS(SAN_NON_REPLACEABLE),
                                            nm.lp, nm.nsp, "", NULL),
                   "§8.6.3: a valid configuration's replaceWithChildrenElements cannot name one of the "
                   "built-in non-replaceable elements, and this one does — replacing it with its children "
                   "re-parses into a different tree");
            if (drop)
                DFAIL("§8.6.4 step 1.5.2.5 replaces an element with its children IN ITS PLACE, and dom_cow's "
                      "private-tree operations can only APPEND (dom_cow_insert_private / "
                      "dom_cow_move_private), which would move the children to the end of their new parent "
                      "and reorder the page's markup. Build dom_cow_insert_before_private — a positional "
                      "insert into a tree nothing else can see — and this step is then the spec's own five "
                      "lines");
        }
        if (san_has(ctx, w->config, "elements")) {                                  /* step 1.5.3 */
            list = san_get(ctx, w->config, "elements");
            drop = !san_contains(ctx, list, nm.lp, nm.nsp);
        } else {                                                                    /* step 1.5.4 */
            list = san_get(ctx, w->config, "removeElements");
            DCHECK(JS_IsObject(list), "§8.6.3: a canonical configuration has an elements list or a "
                                      "removeElements one, and this one has neither");
            drop = san_contains(ctx, list, nm.lp, nm.nsp);
        }
        JS_FreeValue(ctx, list);
        if (drop) { san_begin_remove(ctx, hdr, w, w->cur); return JS_STEP_YIELD; }
        /* Step 1.5.5: a <template>'s TEMPLATE CONTENTS are sanitized, and they are not its children — the
           parser puts everything it holds in the separate fragment §4.12.3 gives it. */
        if (nm.nsp && strcmp(nm.nsp, SAN_NS_HTML) == 0 && strcmp(nm.lp, "template") == 0) {
            lxb_html_template_element_t *t = lxb_html_interface_template(w->cur);

            DCHECK(t->content != NULL, "a <template> element has no template contents — §4.12.3 establishes "
                                       "them when the element is created");
            san_push(w, w->cur, w->tree_root, SAN_AFTER_TEMPLATE);
            w->tree_root = &t->content->node;
            w->cur = t->content->node.first_child;
            hdr->stage = w->stage_base + SAN_CHILD;
            return JS_STEP_YIELD;
        }
        hdr->stage = w->stage_base + SAN_SHADOW;
        return JS_STEP_YIELD;
    }

    case SAN_SHADOW: {
        lxb_dom_node_t *shadow = shadow_root_of_element(ctx, lxb_dom_interface_element(w->cur));  /* 1.5.6 */

        if (shadow && shadow->first_child) {
            san_push(w, w->cur, w->tree_root, SAN_AFTER_SHADOW);
            w->tree_root = shadow;
            w->cur = shadow->first_child;
            hdr->stage = w->stage_base + SAN_CHILD;
            return JS_STEP_YIELD;
        }
        hdr->stage = w->stage_base + SAN_ATTRS;
        w->attr = lxb_dom_element_first_attribute(lxb_dom_interface_element(w->cur));
        return JS_STEP_YIELD;
    }

    case SAN_ATTRS: {
        lxb_dom_attr_t *a = w->attr, *next;
        SanNodeName nm;
        size_t alen = 0, nslen = 0;
        const lxb_char_t *al, *ans;
        char aname[192], ansbuf[128];
        const char *ansp;
        JSValue elements, local_el = JS_UNDEFINED, list;
        bool drop = false;

        if (!a) { hdr->stage = w->stage_base + SAN_DESCEND; return JS_STEP_YIELD; }
        next = lxb_dom_element_next_attribute(a);
        san_node_name(w->cur, &nm);
        al = lxb_dom_attr_local_name(a, &alen);
        ans = dom_attr_ns(a, &nslen);
        DCHECK(al != NULL && alen < sizeof aname && nslen < sizeof ansbuf,
               "an attribute's name does not fit the sanitizer's buffer — a truncated one would be matched "
               "against the configuration's lists as a different attribute");
        memcpy(aname, al, alen);
        aname[alen] = 0;
        ansp = NULL;
        if (ans && nslen) { memcpy(ansbuf, ans, nslen); ansbuf[nslen] = 0; ansp = ansbuf; }
        /* Step 1.5.7-1.5.8: this element's own entry in the allow-list, when there is one. */
        elements = san_get(ctx, w->config, "elements");
        if (JS_IsObject(elements)) {
            int idx = san_index_of(ctx, elements, nm.lp, nm.nsp);

            if (idx >= 0) local_el = JS_GetPropertyUint32(ctx, elements, (uint32_t)idx);
        }
        JS_FreeValue(ctx, elements);
        list = JS_IsObject(local_el) ? JS_GetPropertyStr(ctx, local_el, "removeAttributes") : JS_UNDEFINED;
        if (JS_IsObject(list) && san_contains(ctx, list, aname, ansp)) {            /* step 1.5.9.2 */
            drop = true;
        } else if (san_has(ctx, w->config, "attributes")) {                          /* step 1.5.9.3 */
            JSValue global = san_get(ctx, w->config, "attributes");
            JSValue local = JS_IsObject(local_el) ? JS_GetPropertyStr(ctx, local_el, "attributes")
                                                  : JS_UNDEFINED;
            bool allowed = san_contains(ctx, global, aname, ansp) ||
                           (JS_IsObject(local) && san_contains(ctx, local, aname, ansp));

            if (!allowed) {
                /* The custom-data escape: a `data-` attribute in the null namespace survives an allow-list it
                   is not in, when the configuration allows data attributes at all. */
                JSValue da = san_get(ctx, w->config, "dataAttributes");

                allowed = strncmp(aname, "data-", 5) == 0 && ansp == NULL && JS_ToBool(ctx, da);
                JS_FreeValue(ctx, da);
            }
            drop = !allowed;
            JS_FreeValue(ctx, local);
            JS_FreeValue(ctx, global);
        } else {
            JSValue local = JS_IsObject(local_el) ? JS_GetPropertyStr(ctx, local_el, "attributes")
                                                  : JS_UNDEFINED;

            if (JS_IsObject(local)) {                                                /* step 1.5.9.4.1 */
                drop = !san_contains(ctx, local, aname, ansp);
            } else {                                                                 /* step 1.5.9.4.2 */
                JSValue rem = san_get(ctx, w->config, "removeAttributes");

                DCHECK(JS_IsObject(rem), "§8.6.3: a canonical configuration has an attributes list or a "
                                         "removeAttributes one, and this one has neither");
                drop = san_contains(ctx, rem, aname, ansp);
                JS_FreeValue(ctx, rem);
            }
            JS_FreeValue(ctx, local);
        }
        JS_FreeValue(ctx, list);
        JS_FreeValue(ctx, local_el);
        if (!drop && w->handle_js_urls) {                                            /* step 1.5.9.5 */
            if (san_pair_lists(SAN_NAVIGATING_URL_ATTRIBUTES, SAN_NROWS(SAN_NAVIGATING_URL_ATTRIBUTES),
                               nm.lp, nm.nsp, aname, ansp) && san_attr_is_javascript_url(a))
                drop = true;
            /* Step 1.5.9.5.2: MathML's `href`, in the null or the XLink namespace, is a navigating URL
               attribute on every MathML element rather than on a listed pair. */
            if (!drop && nm.nsp && strcmp(nm.nsp, SAN_NS_MATHML) == 0 && strcmp(aname, "href") == 0 &&
                (ansp == NULL || strcmp(ansp, SAN_NS_XLINK) == 0) && san_attr_is_javascript_url(a))
                drop = true;
            /* Step 1.5.9.5.3: an SVG animation that TARGETS a URL attribute animates a navigation into being,
               so the pair is removed on the attribute's VALUE rather than on a URL. */
            if (!drop && san_pair_lists(SAN_ANIMATING_URL_ATTRIBUTES, SAN_NROWS(SAN_ANIMATING_URL_ATTRIBUTES),
                                        nm.lp, nm.nsp, aname, ansp)) {
                size_t vlen = 0;
                const lxb_char_t *v = lxb_dom_attr_value(a, &vlen);

                if (v && ((vlen == 4 && memcmp(v, "href", 4) == 0) ||
                          (vlen == 10 && memcmp(v, "xlink:href", 10) == 0)))
                    drop = true;
            }
        }
        if (drop) dom_cow_remove_attribute_node(a);
        w->attr = next;
        return JS_STEP_YIELD;
    }

    case SAN_DESCEND:                                                                /* step 1.5.10 */
        san_push(w, w->cur, w->tree_root, SAN_AFTER_CHILDREN);
        w->cur = w->cur->first_child;
        hdr->stage = w->stage_base + SAN_CHILD;
        return JS_STEP_YIELD;

    case SAN_NEXT:
        w->cur = w->cur->next;
        hdr->stage = w->stage_base + SAN_CHILD;
        return JS_STEP_YIELD;

    case SAN_REMOVE: {
        lxb_dom_node_t *n = w->dead, *parent;

        /* Deepest first: the private destroy takes a node with no children, so the subtree is unwound from the
           bottom and every node of it is one step.
           A `<template>` IS A LEAF WITH A TREE BEHIND IT — its contents are a detached fragment, not children —
           and lexbor's destructor would free that fragment with the element, taking nodes this walk never
           released and leaving the identity map naming freed memory. So the contents are unwound first, as
           their own private tree, exactly as the sanitize walk itself descends into them. */
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && n->ns == LXB_NS_HTML &&
            lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE)) {
            lxb_html_template_element_t *t = lxb_html_interface_template(n);

            DCHECK(t->content != NULL, "a <template> element being removed has no template contents");
            if (t->content->node.first_child) {
                san_push(w, n, w->dead_root, SAN_AFTER_TEMPLATE);
                w->dead_depth++;
                w->dead_root = &t->content->node;
                w->dead = t->content->node.first_child;
                return JS_STEP_YIELD;
            }
        }
        if (n->first_child) { w->dead = n->first_child; return JS_STEP_YIELD; }
        if (n == w->doomed) {
            DCHECK(w->dead_depth == 0, "the sanitizer's removal reached its doomed node with a template "
                                       "contents still unwound — the climb skipped a level it pushed");
            dom_cow_discard_private(w->dead_root, n);
            w->doomed = w->dead = NULL;
            w->cur = w->next_sib;
            w->next_sib = NULL;
            hdr->stage = w->stage_base + SAN_CHILD;
            return JS_STEP_YIELD;
        }
        parent = n->parent;
        dom_cow_discard_private(w->dead_root, n);
        /* An emptied template contents is not a node to free — it belongs to the element that owns it, which
           is what the level says. The climb goes back to that element and frees IT next. */
        if (w->dead_depth > 0 && parent == w->dead_root && parent->first_child == NULL) {
            SanLevel lv;

            DCHECK(w->sp > 0, "the sanitizer's removal emptied a private tree it never descended into");
            lv = w->stack[--w->sp];
            w->dead_depth--;
            w->dead = lv.node;
            w->dead_root = lv.root;
            return JS_STEP_YIELD;
        }
        w->dead = parent;
        return JS_STEP_YIELD;
    }

    default: {
        SanLevel lv;

        DCHECK(stage == SAN_POP, "the sanitizer walk resumed into a stage §8.6.4 does not have");
        if (w->sp == 0) return 0;                    /* the outermost invocation has returned: step 4 is done */
        lv = w->stack[--w->sp];
        w->cur = lv.node;
        w->tree_root = lv.root;
        if (lv.after == SAN_AFTER_TEMPLATE) {
            hdr->stage = w->stage_base + SAN_SHADOW;
        } else if (lv.after == SAN_AFTER_SHADOW) {
            w->attr = lxb_dom_element_first_attribute(lxb_dom_interface_element(w->cur));
            hdr->stage = w->stage_base + SAN_ATTRS;
        } else {
            hdr->stage = w->stage_base + SAN_NEXT;
        }
        return JS_STEP_YIELD;
    }
    }
}

/* ---- declaration and installation ------------------------------------------------------------------------ */

static void sanitizer_install_realm(JSContext *ctx);

void sanitizer_init(JSContext *ctx)
{
    static const IdlArgType ONE_ANY[1] = { IDL_ANY };
    static const IdlArgType ONE_BOOL[1] = { IDL_BOOLEAN };
    JSClassDef d = { "Sanitizer", san_finalizer, san_gc_mark };

    DCHECK(!g_ready, "sanitizer_init ran twice — §8.6's interface is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);
    /* `(SanitizerConfig or SanitizerPresets)` is IDL_ANY because its arms are a DICTIONARY and an ENUMERATION,
       and the union's own rule — which arm a value takes — is stated at the one place that resolves it. */
    g_id_ctor = idl_method_id(ctx, ONE_ANY, 1, js_san_ctor, 0);
    idl_optional_from(0);
    g_id_get = idl_method_id(ctx, NULL, 0, js_san_get, 0);
    g_id_remove_unsafe = idl_method_id(ctx, NULL, 0, js_san_remove_unsafe, 0);
    g_id_set_comments = idl_method_id(ctx, ONE_BOOL, 1, js_san_set_flag, 0);
    g_id_set_data_attributes = idl_method_id(ctx, ONE_BOOL, 1, js_san_set_flag, 1);
    g_ready = 1;
    realm_declare_intrinsic(sanitizer_install_realm);
}

static void sanitizer_install_realm(JSContext *ctx)
{
    JSValue proto, prev, ctor, global;

    DCHECK(g_ready, "a realm asked for Sanitizer before sanitizer_init declared it");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "sanitizer_install_realm ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "Sanitizer.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Sanitizer");
    idl_install_method(ctx, proto, "get", 0, g_id_get);
    idl_install_method(ctx, proto, "removeUnsafe", 0, g_id_remove_unsafe);
    idl_install_method(ctx, proto, "setComments", 1, g_id_set_comments);
    idl_install_method(ctx, proto, "setDataAttributes", 1, g_id_set_data_attributes);
    /* §8.6.2's SIX NAME-TAKING MODIFIERS — allowElement, removeElement, replaceElementWithChildren,
       allowAttribute, removeAttribute, allowProcessingInstruction, removeProcessingInstruction — are ABSENT
       rather than stubbed: each takes a `(DOMString or dictionary)` whose conversion is the same machine
       san_config_from_spec's dictionary arm names, so building that one is what installs these. A page calling
       one gets the TypeError of a missing method, which is the honest answer. */
    JS_SetClassProto(ctx, g_class, JS_DupValue(ctx, proto));
    ctor = idl_step_constructor(ctx, "Sanitizer", 0, g_id_ctor);
    CHECK(!JS_IsException(ctor), "the Sanitizer interface object could not be allocated");
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "Sanitizer", ctor);
    JS_FreeValue(ctx, global);
}

void sanitizer_free(void)
{
    /* The prototypes and the interface objects are the REALMS' — each is released with its context. */
    g_ready = 0;
    g_id_ctor = g_id_get = g_id_remove_unsafe = g_id_set_comments = g_id_set_data_attributes = -1;
}
