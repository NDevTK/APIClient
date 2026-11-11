/* CSS Color 5 §3 "Mixing Colors: the color-mix() Function". See css_color_mix.h. */
#include <string.h>

#include <lexbor/css/css.h>

#include "check.h"
#include "core/css/css_color.h"
#include "core/css/css_color_mix.h"
#include "core/css/css_math.h"
#include <lexbor/core/base.h>   /* lexbor_realloc: the allocator the engine gives its refusal edge */

/* One component value at the top level of an argument: its byte span in the input, its first token's type,
   the number a `<percentage>` token carries, and whether it is a math function. */
typedef struct {
    size_t start, end;
    lxb_css_syntax_token_type_t type;
    double pct;
    bool math;
} CcmComponent;

/* One comma-separated argument. The grammar admits at most four components in one (the method's
   `in <polar-color-space> <hue> hue`), so a fifth is recorded only as `over`, which no production matches. */
typedef struct {
    CcmComponent comp[4];
    unsigned n;
    bool over;
} CcmSegment;

/* A mix item: a colour and its percentage, `pct` negative where the author omitted it. */
typedef struct {
    CssColor color;
    double pct;
} CcmItem;

static bool ccm_name_is(const lxb_char_t *data, size_t len, const char *name)
{
    size_t i;

    if (strlen(name) != len) return false;
    for (i = 0; i < len; i++) {
        unsigned char c = data[i];

        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (c != (unsigned char)name[i]) return false;
    }
    return true;
}

static bool ccm_opens(lxb_css_syntax_token_type_t t)
{
    return t == LXB_CSS_SYNTAX_TOKEN_FUNCTION || t == LXB_CSS_SYNTAX_TOKEN_LS_BRACKET ||
           t == LXB_CSS_SYNTAX_TOKEN_L_PARENTHESIS || t == LXB_CSS_SYNTAX_TOKEN_LC_BRACKET;
}

static bool ccm_closes(lxb_css_syntax_token_type_t t)
{
    return t == LXB_CSS_SYNTAX_TOKEN_RS_BRACKET || t == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS ||
           t == LXB_CSS_SYNTAX_TOKEN_RC_BRACKET;
}

static void ccm_push_segment(CcmSegment **segs, size_t *n, size_t *cap)
{
    if (*n == *cap) {
        CcmSegment *g;

        *cap = *cap ? *cap * 2 : 4;
        g = lexbor_realloc(*segs, *cap * sizeof *g);
        CHECK(g != NULL, "css color-mix: OOM growing the argument list");
        *segs = g;
    }
    memset(&(*segs)[*n], 0, sizeof **segs);
    (*n)++;
}

/* The top-level arguments of the color-mix() function at the start of `text`, split on CSS Syntax 3's comma
   tokens at depth one. False where the input does not begin with that function or carries anything after its
   close; a function the input leaves unclosed ends at the input's end, as CSS Syntax 3 §5.5.10 "Consume a
   function" ends one at EOF. Each component's span ends where its last token does. */
static CssColorMixResult ccm_split(const char *text, size_t len, CcmSegment **segs, size_t *nsegs)
{
    lxb_css_syntax_tokenizer_t *tkz;
    const lxb_css_syntax_token_t *t;
    CssColorMixResult res = CSS_COLOR_MIX_FAILURE;
    CcmComponent *open = NULL;
    size_t cap = 0;
    unsigned depth = 1;

    *segs = NULL;
    *nsegs = 0;
    tkz = lxb_css_syntax_tokenizer_create();
    CHECK(tkz != NULL, "css color-mix: the CSS tokenizer allocation failed");
    CHECK(lxb_css_syntax_tokenizer_init(tkz) == LXB_STATUS_OK, "css color-mix: the CSS tokenizer could not initialise");
    lxb_css_syntax_tokenizer_buffer_set(tkz, (const lxb_char_t *)text, len);

    t = lxb_css_syntax_token_wo_ws(tkz);
    CHECK(t != NULL, "css color-mix: OOM tokenizing");
    if (t->type != LXB_CSS_SYNTAX_TOKEN_FUNCTION ||
        !ccm_name_is(lxb_css_syntax_token_function(t)->data, lxb_css_syntax_token_function(t)->length,
                     "color-mix")) {
        res = CSS_COLOR_MIX_NOT_A_MIX;
        goto done;
    }
    lxb_css_syntax_token_consume(tkz);
    ccm_push_segment(segs, nsegs, &cap);
    for (;;) {
        lxb_css_syntax_token_type_t type;
        size_t off, tlen;
        CcmSegment *seg = &(*segs)[*nsegs - 1];

        t = lxb_css_syntax_token(tkz);
        CHECK(t != NULL, "css color-mix: OOM tokenizing");
        type = t->type;
        off = (size_t)t->offset;
        tlen = lxb_css_syntax_token_base(t)->length;
        if (type == LXB_CSS_SYNTAX_TOKEN__EOF) break;
        if (depth > 1) {
            DCHECK(open != NULL, "a nested block inside a color-mix() argument has no component that opened it");
            if (ccm_opens(type)) depth++;
            else if (ccm_closes(type) && --depth == 1) open->end = off + tlen;
            lxb_css_syntax_token_consume(tkz);
            continue;
        }
        if (type == LXB_CSS_SYNTAX_TOKEN_R_PARENTHESIS) {
            lxb_css_syntax_token_consume(tkz);
            break;
        }
        if (type == LXB_CSS_SYNTAX_TOKEN_COMMA) ccm_push_segment(segs, nsegs, &cap);
        else if (type != LXB_CSS_SYNTAX_TOKEN_WHITESPACE) {
            if (seg->n == 4) seg->over = true;
            else {
                open = &seg->comp[seg->n++];
                open->start = off;
                open->end = off + tlen;
                open->type = type;
                if (type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE) open->pct = lxb_css_syntax_token_percentage(t)->num;
                if (type == LXB_CSS_SYNTAX_TOKEN_FUNCTION)
                    open->math = css_math_is_function((const char *)lxb_css_syntax_token_function(t)->data,
                                                      lxb_css_syntax_token_function(t)->length);
            }
            if (ccm_opens(type)) depth++;
        }
        lxb_css_syntax_token_consume(tkz);
    }
    /* An argument left open at the input's end runs to it. */
    if (depth > 1) open->end = len;
    t = lxb_css_syntax_token_wo_ws(tkz);
    CHECK(t != NULL, "css color-mix: OOM tokenizing");
    if (t->type == LXB_CSS_SYNTAX_TOKEN__EOF) res = CSS_COLOR_MIX_PARSED;

done:
    lxb_css_syntax_tokenizer_destroy(tkz);
    return res;
}

/* `c` converted to the interpolation space `space` with CSS Color 4 §13.4 "Interpolating with Missing
   Components"' carried-forward components re-inserted. Every CssColorSpace member is an RGB or XYZ space, and
   §13.4's table makes r, g and b analogous to x, y and z, so a missing component carries forward by index. */
static void ccm_to_space(CssColor *c, CssColorSpace space)
{
    unsigned carried = c->missing & (CSS_COLOR_MISSING_COMPONENT(0) | CSS_COLOR_MISSING_COMPONENT(1) |
                                     CSS_COLOR_MISSING_COMPONENT(2));
    int i;

    css_color_convert(c, space);
    for (i = 0; i < 3; i++)
        if (carried & CSS_COLOR_MISSING_COMPONENT(i)) c->c[i] = 0.0;
    c->missing |= carried;
}

/* CSS Color 4 §13 "Color Interpolation" of `a` and `b` at progress `p` (the amount of `b`, §13.2
   "Component-wise Linear Interpolation"), in the rectangular space `space`: §13.4's fill of a component
   missing in one colour from the other, then §13.5 "Interpolating with Alpha"'s premultiplication. */
static CssColor ccm_interpolate(CssColor a, CssColor b, double p, CssColorSpace space)
{
    CssColor r = { space, { 0.0, 0.0, 0.0 }, 0.0, 0u };
    bool alpha_none;
    int i;

    DCHECK(p >= 0.0 && p <= 1.0, "a colour interpolation progress outside [0, 1]: normalized mix percentages "
                                 "are non-negative and sum to their combined percentage");
    ccm_to_space(&a, space);
    ccm_to_space(&b, space);
    for (i = 0; i < 4; i++) {
        unsigned bit = i < 3 ? CSS_COLOR_MISSING_COMPONENT(i) : CSS_COLOR_MISSING_ALPHA;
        double *va = i < 3 ? &a.c[i] : &a.a, *vb = i < 3 ? &b.c[i] : &b.a;

        if ((a.missing & bit) && !(b.missing & bit)) { *va = *vb; a.missing &= ~bit; }
        else if ((b.missing & bit) && !(a.missing & bit)) { *vb = *va; b.missing &= ~bit; }
    }
    /* After the fill a value is missing only where both are, so one colour's flag answers for the pair. */
    alpha_none = (a.missing & CSS_COLOR_MISSING_ALPHA) != 0;
    if (alpha_none) r.missing |= CSS_COLOR_MISSING_ALPHA;
    else r.a = a.a * (1.0 - p) + b.a * p;
    for (i = 0; i < 3; i++) {
        double pa, pb;

        if (a.missing & CSS_COLOR_MISSING_COMPONENT(i)) {
            r.missing |= CSS_COLOR_MISSING_COMPONENT(i);
            continue;
        }
        pa = alpha_none ? a.c[i] : a.c[i] * a.a;
        pb = alpha_none ? b.c[i] : b.c[i] * b.a;
        r.c[i] = pa * (1.0 - p) + pb * p;
        if (!alpha_none && r.a != 0.0) r.c[i] /= r.a;
    }
    return r;
}

/* An argument's colour and optional percentage: `<color> && <percentage [0,100]>?`. */
static bool ccm_item(const char *text, const CcmSegment *seg, const CssColor *current, CcmItem *out)
{
    const CcmComponent *col, *pct = NULL;

    if (seg->over || seg->n == 0 || seg->n > 2) return false;
    col = &seg->comp[0];
    if (seg->n == 2) {
        unsigned k = seg->comp[0].type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE ? 0u :
                     seg->comp[1].type == LXB_CSS_SYNTAX_TOKEN_PERCENTAGE ? 1u : 2u;

        if (k == 2) {
            for (k = 0; k < 2; k++) {
                const CcmComponent *m = &seg->comp[k];

                if (m->math && css_math_matches(text + m->start, m->end - m->start, CSS_MATH_PROD_PERCENTAGE)) {
                    DFAIL("a color-mix() argument's <percentage> is a css-values-4 §10 math function, and CSS "
                          "Color 4 §4.5's colour parse holds no resolver for its leaves. WHAT TO BUILD: "
                          "evaluate it through css_math_eval with the caller's CssMathResolver");
                    return false;
                }
            }
            return false;
        }
        pct = &seg->comp[k];
        col = &seg->comp[1 - k];
    }
    out->pct = -1.0;
    if (pct != NULL) {
        if (!(pct->pct >= 0.0 && pct->pct <= 100.0)) return false;   /* §3.2: "Negative percentages are ... disallowed" */
        out->pct = pct->pct;
    }
    return css_color_parse_in(text + col->start, col->end - col->start, current, &out->color);
}

/* css-values-5 §6.1 "Normalizing Mix Percentages" with the force normalization flag set, in place over
   `items`, answering the leftover percentage. */
static double ccm_normalize(CcmItem *items, size_t n)
{
    double specified = 0.0, total = 0.0;
    size_t i, omitted = 0;

    for (i = 0; i < n; i++) {
        if (items[i].pct < 0.0) omitted++;
        else specified += items[i].pct;
    }
    if (specified > 100.0) specified = 100.0;
    for (i = 0; i < n; i++) {
        if (items[i].pct < 0.0) items[i].pct = (100.0 - specified) / (double)omitted;
        total += items[i].pct;
    }
    if (total > 0.0)
        for (i = 0; i < n; i++) items[i].pct *= 100.0 / total;
    return total < 100.0 ? 100.0 - total : 0.0;
}

CssColorMixResult css_color_mix_parse(const char *text, size_t len, const CssColor *current, CssColor *out)
{
    CcmSegment *segs;
    CcmItem *items = NULL;
    size_t nsegs, first = 0, n = 0, i;
    CssColorSpace space = CSS_COLOR_SPACE_SRGB;
    bool space_known = false;
    CssColorMixResult res;
    double leftover;
    CssColor color;

    DCHECK(text != NULL && out != NULL, "css color-mix: a parse was asked of no text or into nowhere");
    res = ccm_split(text, len, &segs, &nsegs);
    if (res != CSS_COLOR_MIX_PARSED) goto done;
    res = CSS_COLOR_MIX_FAILURE;
    /* `<color-interpolation-method>? ,`: css-values-4 §2 "Value Definition Syntax" omits the comma with the
       omitted method, so a first argument opening with the ident `in` is the method. */
    if (segs[0].n > 0 && segs[0].comp[0].type == LXB_CSS_SYNTAX_TOKEN_IDENT &&
        ccm_name_is((const lxb_char_t *)text + segs[0].comp[0].start, segs[0].comp[0].end - segs[0].comp[0].start,
                    "in")) {
        const char *w[4];
        size_t wl[4];
        unsigned k;

        if (segs[0].over) goto done;
        for (k = 0; k < segs[0].n; k++) {
            if (segs[0].comp[k].type != LXB_CSS_SYNTAX_TOKEN_IDENT) goto done;
            w[k] = text + segs[0].comp[k].start;
            wl[k] = segs[0].comp[k].end - segs[0].comp[k].start;
        }
        if (css_color_interpolation_method_match(w, wl, segs[0].n) != segs[0].n) goto done;
        space_known = css_color_space_by_name(w[1], wl[1], &space);
        first = 1;
    }
    if (nsegs <= first) goto done;
    items = lexbor_malloc((nsegs - first) * sizeof *items);
    CHECK(items != NULL, "css color-mix: OOM holding the mix items");
    for (i = first; i < nsegs; i++)
        if (!ccm_item(text, &segs[i], current, &items[n++])) goto done;
    /* §3.1 "Colorspace for mixing": "If no color interpolation method is specified, assume Oklab." */
    if (!space_known) {
        DFAIL("a color-mix() mixes in lab, oklab (the default) or a polar space, and CssColorSpace has none of "
              "them: css_color.c converts that family to sRGB at parse. WHAT TO BUILD: the lab family and the "
              "polar spaces as CssColorSpace members with §11's conversions in both directions, and §13.6 "
              "\"Hue Interpolation\" for the polar ones");
        goto done;
    }
    /* §3.3 "Calculating the Result of color-mix". */
    leftover = ccm_normalize(items, n);
    color = items[0].color;
    if (n == 1) ccm_to_space(&color, space);
    else {
        double pct = items[0].pct;

        for (i = 1; i < n; i++) {
            double combined = pct + items[i].pct;

            color = ccm_interpolate(color, items[i].color, combined > 0.0 ? items[i].pct / combined : 0.5, space);
            pct = combined;
        }
    }
    color.a *= 1.0 - leftover / 100.0;
    *out = color;
    res = CSS_COLOR_MIX_PARSED;

done:
    free(items);
    free(segs);
    return res;
}
