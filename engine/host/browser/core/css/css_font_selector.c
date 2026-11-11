/* css-fonts-4 §5.2 "Matching font styles" over a document's `@font-face` rules. See css_font_selector.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/realm.h"
#include "core/css/css_author_sheets.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_font_face_descriptors.h"
#include "core/css/css_font_family.h"
#include "core/css/css_font_selector.h"
#include "core/css/css_font_src.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_unicode_range.h"
#include "core/dom/document.h"
#include "core/fetch/fetch.h"
#include "core/fonts/font_matching.h"
#include "core/frame/policy_container.h"
#include "core/loader/script_fetch.h"   /* Fetch §2.2.3 "Statuses"' ok status */
#include "core/url/url.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/reclaim.h"

/* A font resource's state in its document's record, stored as the word beside it. SEL_NONE is an absent
   key: a resource never asked for. */
typedef enum { SEL_NONE = 0, SEL_LOADING, SEL_LOADED, SEL_ERROR, SEL_STATES } SelState;
static const char *const SEL_WORD[SEL_STATES] = { NULL, "loading", "loaded", "error" };

static JSClassID  g_slot = JS_INVALID_CLASS_ID;   /* the per-realm record: absolute URL -> state */
static JSRuntime *g_rt;                           /* the runtime this was declared in */

/* One `@font-face` rule a walk's family matched. The strings are copies, so the walk borrows nothing from
   the layout job's sheet set. */
typedef struct {
    char *src;                  /* css-fonts-4 §4.3's descriptor, serialized. Owned. */
    char *location;             /* the rule's sheet's location, or NULL for an embedded sheet. Owned. */
    CssUnicodeRangeSet range;   /* css-fonts-4 §4.5's set; unread where `all` */
    bool all;                   /* the rule declares no unicode-range: the initial U+0-10FFFF */
    CssFontRange weight, width; /* css-fonts-4 §4.4's ranges the face supports */
    CssFontFaceStyle style;
    bool matched;               /* in §5.2's matching set for the walk's element, after its narrowing */
    bool rests;                 /* this walk has found the face loading, loaded or local */
} SelFace;

/* One family of the element's list that names something: an `@font-face` family's faces in rule order, or
   the installed face a generic family or installed name resolves to. */
typedef struct {
    bool web;
    InstalledFont installed;
    size_t first, n;            /* `faces[first .. first + n)` where `web` */
} SelFamily;

struct CssFontSelectorWalk {
    JSContext *ctx;             /* the document's realm */
    SelFamily *families;
    size_t nfamilies;
    SelFace *faces;
    size_t nfaces;
};

/* ---- the document's font resources ------------------------------------------------------------------------- */

static JSValue sel_record(JSContext *ctx)
{
    JSValue rec;

    DCHECK(g_rt != NULL, "a document's font resources were reached before css_font_selector_init declared them");
    rec = realm_value_get(ctx, g_slot);
    DCHECK(JS_IsObject(rec), "a realm has no font-resource record; css_font_selector_install is on realm.h's one "
                             "list, so this realm skipped it");
    return rec;
}

/* The state of `url` in `ctx`'s document. */
static SelState sel_state(JSContext *ctx, const char *url)
{
    JSValue rec = sel_record(ctx), v = JS_GetPropertyStr(ctx, rec, url);
    SelState state = SEL_NONE;
    const char *s;
    int i;

    JS_FreeValue(ctx, rec);
    if (JS_IsUndefined(v)) return SEL_NONE;
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL, "css-fonts: OOM reading a font resource's state");
    for (i = SEL_LOADING; i < SEL_STATES; i++)
        if (strcmp(s, SEL_WORD[i]) == 0) state = (SelState)i;
    DCHECKF(state != SEL_NONE, "a font resource's state is `%s`, which is none of the three this file writes", s);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return state;
}

static void sel_set_state(JSContext *ctx, const char *url, SelState state)
{
    JSValue rec = sel_record(ctx);

    DCHECK(state > SEL_NONE && state < SEL_STATES, "a font resource was given a state that is not one of the three");
    JS_SetPropertyStr(ctx, rec, url, JS_NewString(ctx, SEL_WORD[state]));
    JS_FreeValue(ctx, rec);
}

/* css-fonts-4 §4.8.2's processResponse for the fetch of `func_data[0]`: a network error (the host's null) or
   a status that is not an ok status is a failed load, and css-fonts-4 §5.2 then treats the face "as not
   present in the family". Chrome 153 marks a face whose font answered 404 `error` and moves to the next src
   item. An ok reply is recorded as loaded and its bytes are left unread (css_font_selector.h's first
   residual). */
static JSValue sel_deliver(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                           JSValueConst *func_data)
{
    const char *url;
    bool ok;

    (void)this_val; (void)magic;
    DCHECK(argc >= 1 && (JS_IsObject(argv[0]) || JS_IsNull(argv[0])),
           "a font reply was delivered as something other than the host's reply record or null for a network "
           "error");
    ok = argc >= 1 && JS_IsObject(argv[0]) && script_fetch_status_ok(fetch_reply_status(ctx, argv[0]));
    url = JS_ToCString(ctx, func_data[0]);
    CHECK(url != NULL, "css-fonts: OOM reading the address a font reply answered");
    DCHECKF(sel_state(ctx, url) == SEL_LOADING,
            "a font reply for %s arrived for a resource that is not loading; the fetch is issued once per "
            "document and writes `loading` before it parks", url);
    sel_set_state(ctx, url, ok ? SEL_LOADED : SEL_ERROR);
    JS_FreeCString(ctx, url);
    return JS_UNDEFINED;
}

/* css-fonts-4 §4.8.2 "Font fetching requirements" for the resolved `url`: css-values-4 §4.5.4's request
   (destination "font", mode "cors", credentials mode "same-origin", referrer `referrer`), through Fetch
   §4.1 "Main fetch"'s steps 4, 6 and 7 in the engine and then parked on the running flow. False when step 7
   blocks it, which is a network error. */
static bool sel_fetch(JSContext *ctx, const char *url, const char *referrer)
{
    CspRequestMetadata meta = csp_request_metadata_unstated();
    FetchRequest req = { 0 };
    JSValueConst data[1];
    JSValue urlv, deliver;
    char *abs = reclaim_strdup(url), *up;

    CHECK(abs != NULL, "css-fonts: OOM copying a font request's address");
    DCHECK(flow_running() != NULL,
           "a font load was started outside a running flow. Layout that lays out text runs inside a flow's "
           "member call or its update-the-rendering task; a layout job the host drives needs its font loads "
           "issued by a task of the document's own event loop");
    fetch_main_report_csp(ctx, abs, "font", meta);
    up = fetch_main_upgrade(ctx, abs, "font", NULL);
    if (up != NULL) { free(abs); abs = up; }
    if (fetch_main_blocked(ctx, abs, "font", meta, FETCH_MODE_CORS)) {
        sel_set_state(ctx, url, SEL_ERROR);
        free(abs);
        return false;
    }
    urlv = JS_NewString(ctx, url);
    CHECK(!JS_IsException(urlv), "css-fonts: OOM keeping a font request's address for its reply");
    data[0] = urlv;
    deliver = JS_NewCFunctionData(ctx, sel_deliver, 1, 0, 1, data);
    CHECK(!JS_IsException(deliver), "css-fonts: OOM allocating a font reply's processResponse steps");
    JS_FreeValue(ctx, urlv);
    sel_set_state(ctx, url, SEL_LOADING);
    req.method = "GET";
    req.url = abs;
    req.destination = "font";
    req.metadata = meta;
    req.credentials = FETCH_CREDENTIALS_SAME_ORIGIN;
    req.mode = FETCH_MODE_CORS;
    req.referrer = referrer;
    req.referrer_policy = "";
    req.address = FETCH_ADDRESS_CONCRETE;
    engine_pending_resource_url(ctx, deliver, &req);
    JS_FreeValue(ctx, deliver);
    free(abs);
    return true;
}

/* ---- css-fonts-4 §4.3.3 "Selecting items in the src" --------------------------------------------------------- */

static bool sel_word_in(const char *word, const char *const *set, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        const char *a = word, *b = set[i];

        while (*a && *b && ((*a >= 'A' && *a <= 'Z') ? *a - 'A' + 'a' : *a) == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return true;
    }
    return false;
}

/* §4.3.3: "Conformant user agents must skip downloading a font resource if the format hint indicates an
   unsupported or unknown font format, or if any of the font technologies are unsupported by the user agent.
   If no format hint is supplied, the user agent should download the font resource." The two sets are the
   ones Chrome 153 answers true for through `CSS.supports("font-format(...)")` and `font-tech(...)` on Linux,
   since which resource is requested is what a server observes. */
static bool sel_item_supported(const CssFontSrcItem *it)
{
    static const char *const FORMATS[] = { "collection", "opentype", "truetype", "woff", "woff2" };
    static const char *const TECHS[] = { "features-opentype", "features-aat", "color-colrv0", "color-colrv1",
                                         "color-sbix", "color-cbdt", "variations", "palettes" };
    const char *format = it->format_kw != NULL ? it->format_kw : it->format_str;
    size_t i;

    if (format != NULL && !sel_word_in(format, FORMATS, sizeof FORMATS / sizeof FORMATS[0])) return false;
    for (i = 0; i < it->ntech; i++)
        if (!sel_word_in(it->tech[i], TECHS, sizeof TECHS / sizeof TECHS[0])) return false;
    return true;
}

/* Whether a `local()` item names an installed face (css-fonts-4 §4.3.3.1 "Local font fallback"). */
static bool sel_item_local(const CssFontSrcItem *it)
{
    CssFontFamilyList names = { 0 };
    InstalledFont face;
    bool found;

    found = css_font_family_list(it->local, &names) && names.n == 1 && names.v[0].name != NULL &&
            font_matching_installed_face_named(names.v[0].name, &face);
    css_font_family_list_free(&names);
    return found;
}

/* css-values-4 §4.5.4's resolve a style resource URL: `url` against `base`. NULL when the parse fails. */
static char *sel_resolve(const char *url, const char *base)
{
    UrlRecord b, rec;
    char *abs = NULL;
    bool have_base;

    url_record_init(&b);
    have_base = url_parse(&b, base, strlen(base), NULL);
    url_record_init(&rec);
    if (have_base && url_parse(&rec, url, strlen(url), &b)) abs = url_serialize(&rec, /*exclude_fragment*/ false);
    url_record_free(&rec);
    url_record_free(&b);
    return abs;
}

/* §5.2's "load the font" for one face: its src items in order, each skipped when unsupported, a failed
   resource moving to the next. True when the face rests on an item: a local face, or a resource that is
   loading or loaded, which css-fonts-4 §3.2 lays out with a fallback without loading a fallback's font.
   False when every item failed, where §5.2 treats the face as not present and moves to the next family. */
static bool sel_face_load(CssFontSelectorWalk *w, const SelFace *f)
{
    CssFontSrcList items = { NULL, 0, 0 };
    /* css-values-4 §4.5.4's style resource base URL: the sheet's location, else the document's base URL. */
    const char *base = f->location != NULL ? f->location : document_base_url(w->ctx);
    /* §4.5.4: "If req's mode is "cors", and sheet is not null, then set req's referrer to the style resource
       base URL". Chrome 153 follows it for a linked sheet (Referer is the sheet's URL) and not for an
       embedded one under a `base` element, where its Referer is the document's URL, which is Fetch's
       "client"; the engine sends what Chrome sends. */
    const char *referrer = f->location != NULL ? f->location : "client";
    bool rests = false;
    size_t i;

    DCHECK(base != NULL, "a document has no base URL for an `@font-face` rule's src to resolve against");
    if (!css_font_src_items(f->src, &items)) return false;
    for (i = 0; i < items.n && !rests; i++) {
        const CssFontSrcItem *it = &items.v[i];
        SelState state;
        char *abs;

        if (it->local != NULL) { rests = sel_item_local(it); continue; }
        if (!sel_item_supported(it)) continue;
        abs = sel_resolve(it->url, base);
        if (abs == NULL) continue;   /* §4.5.4's fetch a style resource: "If that failed, return." */
        state = sel_state(w->ctx, abs);
        if (state == SEL_NONE) rests = sel_fetch(w->ctx, abs, referrer);
        else rests = state != SEL_ERROR;
        free(abs);
    }
    css_font_src_items_free(&items);
    return rests;
}

/* ---- the walk ------------------------------------------------------------------------------------------------ */

/* css-fonts-4 §5.1 "Localized name matching" between an element's family name and a rule's. */
/* NAMED RESIDUAL. Not covered: default caseless matching of letters outside ASCII; those compare by code
   point. Next diff builds: Unicode full case folding shared with core/fonts/font_matching.c's table. Absence
   shows as a family whose name differs from its rule's only by the case of a non-ASCII letter not loading. */
static bool sel_names_match(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;

        if (x != y) return false;
    }
    return *a == '\0' && *b == '\0';
}

static void sel_push_family(CssFontSelectorWalk *w, SelFamily fam)
{
    SelFamily *grown = lexbor_realloc(w->families, (w->nfamilies + 1) * sizeof *grown);

    CHECK(grown != NULL, "css-fonts: OOM recording a family of css-fonts-4 §5.2's walk");
    w->families = grown;
    w->families[w->nfamilies++] = fam;
}

static void sel_push_face(CssFontSelectorWalk *w, const CssRuleFontFace *rule)
{
    SelFace *grown = lexbor_realloc(w->faces, (w->nfaces + 1) * sizeof *grown), *f;

    CHECK(grown != NULL, "css-fonts: OOM recording a face of css-fonts-4 §5.2's walk");
    w->faces = grown;
    f = &w->faces[w->nfaces++];
    memset(f, 0, sizeof *f);
    f->src = reclaim_strdup(rule->src);
    f->location = rule->location != NULL ? reclaim_strdup(rule->location) : NULL;
    CHECK(f->src != NULL && (rule->location == NULL || f->location != NULL),
          "css-fonts: OOM copying an `@font-face` rule's descriptors");
    /* An absent descriptor is the initial `auto`; a stored one passed the descriptor's grammar at the parse. */
    {
        bool weight = css_font_face_weight(rule->weight != NULL ? rule->weight : "auto", &f->weight);
        bool width = css_font_face_width(rule->width != NULL ? rule->width : "auto", &f->width);
        bool style = css_font_face_style(rule->style != NULL ? rule->style : "auto", &f->style);

        DCHECK(weight && width && style,
               "an `@font-face` rule's stored font-weight, font-width or font-style descriptor is outside "
               "css-fonts-4 §4.4's grammar, which the descriptor's own parse admitted when the block was stored");
        (void)weight; (void)width; (void)style;
    }
    f->all = rule->unicode_range == NULL;
    if (!f->all) {
        bool parsed = css_unicode_range_parse(rule->unicode_range, &f->range);

        DCHECKF(parsed, "an `@font-face` rule's stored unicode-range `%s` is outside css-fonts-4 §4.5's "
                "grammar, which the descriptor's own parse admitted when the block was stored",
                rule->unicode_range);
        (void)parsed;
    }
}

/* ---- css-fonts-4 §5.2's narrowing of a family's faces ------------------------------------------------------ */

static bool sel_in(CssFontRange r, double v) { return v >= r.lo && v <= r.hi; }

/* The value §5.2 determines for one axis when no face's range includes `want`: the nearest supported value
   on the preferred side, else the nearest on the other. `r(f)` is face `f`'s range on the axis. */
typedef CssFontRange (*SelAxis)(const SelFace *f);
static CssFontRange sel_axis_width(const SelFace *f) { return f->width; }
static CssFontRange sel_axis_weight(const SelFace *f) { return f->weight; }

/* The nearest supported value below `want` (the greatest `hi` under it) or above it (the least `lo` over it),
   within `[floor, ceil]`, over the matched faces. False when no face has one. */
static bool sel_nearest(const SelFace *f, size_t n, SelAxis r, double want, bool below, double floor,
                        double ceil, double *out)
{
    bool found = false;
    size_t i;

    for (i = 0; i < n; i++) {
        CssFontRange x = r(&f[i]);
        double v = below ? x.hi : x.lo;

        if (!f[i].matched || (below ? v >= want : v <= want) || v < floor || v > ceil) continue;
        if (!found || (below ? v > *out : v < *out)) { *out = v; found = true; }
    }
    return found;
}

/* Removes from the matching set the faces whose range on axis `r` does not include `v`. */
static void sel_keep(SelFace *f, size_t n, SelAxis r, double v)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (f[i].matched && !sel_in(r(&f[i]), v)) f[i].matched = false;
}

/* §5.2: "font-width is tried first." A desired width at or under 100% checks narrower values first, a wider
   one checks wider values first. */
static void sel_narrow_width(SelFace *f, size_t n, double want)
{
    double v = want;
    size_t i;

    for (i = 0; i < n; i++)
        if (f[i].matched && sel_in(f[i].width, want)) { sel_keep(f, n, sel_axis_width, want); return; }
    if (sel_nearest(f, n, sel_axis_width, want, want <= 100.0, -1e9, 1e9, &v) ||
        sel_nearest(f, n, sel_axis_width, want, want > 100.0, -1e9, 1e9, &v))
        sel_keep(f, n, sel_axis_width, v);
}

/* §5.2: "font-weight is matched next." A desired weight in [400, 500] checks up to 500, then below, then
   above 500; one under 400 checks below then above; one over 500 checks above then below. */
static void sel_narrow_weight(SelFace *f, size_t n, double want)
{
    double v = want;
    bool found;
    size_t i;

    for (i = 0; i < n; i++)
        if (f[i].matched && sel_in(f[i].weight, want)) { sel_keep(f, n, sel_axis_weight, want); return; }
    if (want >= 400.0 && want <= 500.0)
        found = sel_nearest(f, n, sel_axis_weight, want, false, -1e9, 500.0, &v) ||
                sel_nearest(f, n, sel_axis_weight, want, true, -1e9, 1e9, &v) ||
                sel_nearest(f, n, sel_axis_weight, want, false, -1e9, 1e9, &v);
    else if (want < 400.0)
        found = sel_nearest(f, n, sel_axis_weight, want, true, -1e9, 1e9, &v) ||
                sel_nearest(f, n, sel_axis_weight, want, false, -1e9, 1e9, &v);
    else
        found = sel_nearest(f, n, sel_axis_weight, want, false, -1e9, 1e9, &v) ||
                sel_nearest(f, n, sel_axis_weight, want, true, -1e9, 1e9, &v);
    if (found) sel_keep(f, n, sel_axis_weight, v);
}

/* How far face style `have` is from the desired `want` in §5.2's search order for font-style; the least is
   the match and 0 is a face that has the desired style. The bands are the order the spec checks kinds in
   (for italic: italic, oblique from 11deg up, oblique under 11deg down, normal, then negative angles), and
   the offset inside a band is the order within it. */
static double sel_style_distance(CssFontFaceStyle want, CssFontFaceStyle have)
{
    /* A face's oblique range as the positive angle nearest the desired one, mirrored for a negative ask. */
    double sign = (want.slant == CSS_FONT_FACE_OBLIQUE && want.angle.lo < 0.0) ? -1.0 : 1.0;
    double a = sign * want.angle.lo, lo = sign > 0 ? have.angle.lo : -have.angle.hi,
           hi = sign > 0 ? have.angle.hi : -have.angle.lo;
    bool spans_zero = have.slant == CSS_FONT_FACE_OBLIQUE && lo <= 0.0 && hi >= 0.0;
    bool have_normal = have.slant == CSS_FONT_FACE_NORMAL || spans_zero;
    bool have_oblique = have.slant == CSS_FONT_FACE_OBLIQUE && hi > 0.0;

    if (want.slant == CSS_FONT_FACE_NORMAL) {
        if (have_normal) return 0.0;
        if (have.slant == CSS_FONT_FACE_OBLIQUE && lo > 0.0) return 1000.0 + lo;
        if (have.slant == CSS_FONT_FACE_ITALIC) return 2000.0;
        return 3000.0 - hi;
    }
    if (want.slant == CSS_FONT_FACE_ITALIC) {
        if (have.slant == CSS_FONT_FACE_ITALIC) return 0.0;
        if (have_oblique && hi >= 11.0) return 1000.0 + (lo > 11.0 ? lo - 11.0 : 0.0);
        if (have_oblique) return 2000.0 + (11.0 - hi);
        if (have_normal) return 3000.0;
        return 4000.0 - hi;
    }
    if (have.slant == CSS_FONT_FACE_OBLIQUE && a >= lo && a <= hi) return 0.0;
    if (have.slant == CSS_FONT_FACE_OBLIQUE && hi > 0.0) {
        /* An angle of 11deg or more checks larger angles first; a smaller one checks smaller angles first. */
        if (lo > a) return (a >= 11.0 ? 1000.0 : 2000.0) + (lo - a);
        return (a >= 11.0 ? 2000.0 : 1000.0) + (a - hi);
    }
    if (have.slant == CSS_FONT_FACE_ITALIC) return 3000.0;
    if (have_normal) return 4000.0;
    return 5000.0 - hi;
}

/* §5.2: "font-style is tried next." The faces at the least distance stay in the matching set. */
static void sel_narrow_style(SelFace *f, size_t n, CssFontFaceStyle want)
{
    double best = 0.0;
    bool any = false;
    size_t i;

    for (i = 0; i < n; i++) {
        double d;

        if (!f[i].matched) continue;
        d = sel_style_distance(want, f[i].style);
        if (!any || d < best) { best = d; any = true; }
    }
    for (i = 0; i < n; i++)
        if (f[i].matched && sel_style_distance(want, f[i].style) != best) f[i].matched = false;
}

/* §5.2's narrowing of one family's faces "to a single face using other font properties", against `style`'s
   computed font-width, font-style and font-weight. What stays matched is one composite face: rules "with
   identical font descriptor values but differing unicode-range values". */
static void sel_narrow(lxb_dom_element_t *style, SelFace *f, size_t n)
{
    char *weight = css_computed_value(style, "font-weight"), *slant = css_computed_value(style, "font-style"),
         *width = css_computed_value(style, "font-stretch");
    CssFontRange w, s;
    CssFontFaceStyle st;
    bool read;
    size_t i;

    /* A computed value of each property is a value its descriptor's grammar reads too: the property's values
       are the descriptor's single-value forms. */
    read = weight != NULL && slant != NULL && width != NULL && css_font_face_weight(weight, &w) &&
           css_font_face_style(slant, &st) && css_font_face_width(width, &s);
    if (!read)
        DFAILF("an element's computed font-weight `%s`, font-style `%s` or font-stretch `%s` is not a "
               "number, a keyword or an angle; build the missing form (a math function, `left`/`right`) in "
               "core/css/css_font_face_descriptors.c's property-side reads", weight ? weight : "(none)",
               slant ? slant : "(none)", width ? width : "(none)");
    free(weight);
    free(slant);
    free(width);
    for (i = 0; i < n; i++) f[i].matched = true;
    if (!read) return;
    sel_narrow_width(f, n, s.lo);
    sel_narrow_style(f, n, st);
    sel_narrow_weight(f, n, w.lo);
}

CssFontSelectorWalk *css_font_selector_walk_open(lxb_dom_element_t *style)
{
    lxb_dom_node_t *doc;
    const CssRuleFontFace *rules;
    CssFontFamilyList list = { 0 };
    CssFontSelectorWalk *w;
    uint32_t nrules = 0, r;
    char *family;
    bool parsed, any_web = false;
    size_t i;

    DCHECK(style != NULL && style->node.owner_document != NULL,
           "css-fonts-4 §5.2's walk was asked for no element or one with no node document");
    doc = lxb_dom_interface_node(style->node.owner_document);
    rules = css_author_sheets_font_faces(cssom_author_sheets(doc), &nrules);
    if (nrules == 0) return NULL;

    w = lexbor_calloc(1, sizeof *w);
    CHECK(w != NULL, "css-fonts: OOM building css-fonts-4 §5.2's walk");
    w->ctx = document_realm_of(doc);
    DCHECK(w->ctx != NULL, "an element being laid out is in a document with no realm record");
    family = css_computed_value(style, "font-family");
    parsed = css_font_family_list(family, &list);
    DCHECKF(parsed, "an element's computed `font-family` `%s` is outside css-fonts-4 §2.1's grammar", family);
    free(family);
    for (i = 0; i < list.n; i++) {
        SelFamily fam = { false, INSTALLED_FONT_COUNT, w->nfaces, 0 };

        /* §5.2: a name is looked up "among fonts defined via @font-face rules and then among available
           installed fonts"; a generic family is never an `@font-face` family. */
        if (list.v[i].name != NULL)
            for (r = 0; r < nrules; r++)
                if (sel_names_match(list.v[i].name, rules[r].family)) sel_push_face(w, &rules[r]);
        fam.n = w->nfaces - fam.first;
        if (fam.n > 0) {
            sel_narrow(style, &w->faces[fam.first], fam.n);
            fam.web = true;
            any_web = true;
            sel_push_family(w, fam);
        } else if (font_matching_installed_family(list.v[i].generic, list.v[i].name, &fam.installed)) {
            sel_push_family(w, fam);
        }
    }
    css_font_family_list_free(&list);
    if (!any_web) {
        css_font_selector_walk_free(w);
        return NULL;
    }
    return w;
}

void css_font_selector_walk_free(CssFontSelectorWalk *w)
{
    size_t i;

    DCHECK(w != NULL, "css-fonts-4 §5.2's walk was freed through nothing");
    for (i = 0; i < w->nfaces; i++) {
        free(w->faces[i].src);
        free(w->faces[i].location);
        css_unicode_range_free(&w->faces[i].range);
    }
    free(w->faces);
    free(w->families);
    free(w);
}

void css_font_selector_walk_character(CssFontSelectorWalk *w, uint32_t cp)
{
    size_t i, k;

    DCHECK(w != NULL, "css-fonts-4 §5.2 was asked for a character through no walk");
    DCHECK(cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF),
           "css-fonts-4 §5.2 was asked for a face for something that is not a Unicode scalar value");
    /* A load is the page's own act: it parks on the flow whose layout needs the face. A layout the host
       drives between steps is the tool's read of the tree and starts none; the page's next layout does. */
    if (flow_running() == NULL) return;
    for (i = 0; i < w->nfamilies; i++) {
        const SelFamily *fam = &w->families[i];

        if (!fam->web) {
            /* An installed family earlier in the list that has the character ends the walk: Chrome 153
               requests nothing for `font-family: 'Liberation Sans', W`. */
            if (open_type_metrics_covers(installed_font_metrics(fam->installed), cp)) return;
            continue;
        }
        /* §5.2: the composite face's faces are taken "in reverse order of @font-face rule definition", and
           css-fonts-4 §4.5 forbids a download "for codepoints outside this set". Chrome 153 requests only the
           later of two same-style rules whose ranges both hold the character. A face whose every src item
           failed is "treated as not present" and the walk moves on. */
        for (k = fam->first + fam->n; k-- > fam->first;) {
            SelFace *face = &w->faces[k];

            if (!face->matched || !(face->all || css_unicode_range_includes(&face->range, cp))) continue;
            if (face->rests) return;
            if (sel_face_load(w, face)) {
                face->rests = true;
                return;
            }
        }
    }
}

/* ---- declaration ------------------------------------------------------------------------------------------- */

void css_font_selector_init(JSContext *ctx)
{
    DCHECK(g_rt == NULL, "css_font_selector_init ran twice; one declaration per agent");
    g_rt = JS_GetRuntime(ctx);
    g_slot = realm_value_declare(ctx, "css-fonts-4 §4.8.2 a document's font resources");
    agent_state_ptr("css_font_selector", &g_rt, "the runtime the font-resource realm-value slot was declared in");
    agent_state_realm_slot("css_font_selector", &g_slot, "the per-realm slot a document's font resources live in");
    realm_declare_intrinsic(css_font_selector_install);
}

void css_font_selector_install(JSContext *ctx)
{
    JSValue rec;

    DCHECK(g_rt != NULL, "a realm asked for its font-resource record before the component was declared");
    /* Built with the realm, so a flow's write to it is captured by that flow's delta and a fork inherits its
       parent's requests. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "a document's font-resource record could not be allocated");
    realm_value_set(ctx, g_slot, rec);
}

void css_font_selector_free(JSRuntime *rt)
{
    DCHECK(g_rt != NULL, "the font selector was released in an agent that never declared it");
    DCHECK(g_rt == rt, "the font selector was released against a runtime other than the one it was declared in");
    g_slot = JS_INVALID_CLASS_ID;
    g_rt = NULL;
}
