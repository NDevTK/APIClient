/* MEDIA QUERIES LEVEL 4 — the grammar of §3, the features of §4. See media_query.h for why the evaluation is a
   plain bool, why the logic is three-valued, and why nothing here throws. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/css_code_point.h"
#include "core/css/css_dimension.h"
#include "core/css/font_metrics.h"
#include "core/css/font_size_functions.h"
#include "core/css/media_query.h"
#include "core/dom/document.h"
#include "core/frame/screen.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "solver/concolic.h"
#include "solver/decide.h"

/* ---- three-valued logic — MQ4 §3.1 -------------------------------------------------------------------------
   Kleene's, which is what the spec's own table is. `unknown` is a real third answer and not a stand-in for
   false: `(bogus: 1) or (min-width: 1px)` is TRUE and `(bogus: 1) and (min-width: 1px)` is FALSE, and a leaf
   that collapsed to false would get the first of those wrong. */
enum { MQ_FALSE = 0, MQ_TRUE = 1, MQ_UNKNOWN = 2 };

static int mq_not(int a) { return a == MQ_UNKNOWN ? MQ_UNKNOWN : (a == MQ_TRUE ? MQ_FALSE : MQ_TRUE); }
static int mq_and(int a, int b)
{
    if (a == MQ_FALSE || b == MQ_FALSE) return MQ_FALSE;
    return (a == MQ_UNKNOWN || b == MQ_UNKNOWN) ? MQ_UNKNOWN : MQ_TRUE;
}
static int mq_or(int a, int b)
{
    if (a == MQ_TRUE || b == MQ_TRUE) return MQ_TRUE;
    return (a == MQ_UNKNOWN || b == MQ_UNKNOWN) ? MQ_UNKNOWN : MQ_FALSE;
}

/* ---- the AST ------------------------------------------------------------------------------------------------ */

#define MQ_IDENT_MAX 64
#define MQ_RAW_MAX  160

/* `<mf-comparison>` — MQ4 §3. MQ_CMP_NONE is "no bound on this side". */
enum { MQ_CMP_NONE = 0, MQ_CMP_LT, MQ_CMP_LE, MQ_CMP_GT, MQ_CMP_GE, MQ_CMP_EQ };

/* `<mf-value> = <number> | <dimension> | <ident> | <ratio>` (§4). A ratio's second term defaults to 1, which is
   what makes a bare `<number>` acceptable wherever a ratio is wanted. */
typedef enum { MV_ABSENT = 0, MV_NUMBER, MV_DIMENSION, MV_RATIO, MV_IDENT } MqValueKind;
typedef struct {
    MqValueKind kind;
    double      a, b;                 /* the number, or a ratio's two terms */
    char        unit[MQ_IDENT_MAX];
    char        ident[MQ_IDENT_MAX];
} MqValue;

/* A `<media-feature>`, NORMALISED to the general range form `lo <lo_cmp> name <hi_cmp> hi`, with `eq` for the
   `<mf-plain>` colon form and `boolean` for `<mf-boolean>`. `min-`/`max-` are the same thing spelled two ways —
   §4 says `(min-width: 600px)` IS `(width >= 600px)` — so they normalise into a bound here and `prefix` is kept
   only so the serialization answers what the page wrote. */
typedef struct {
    char    name[MQ_IDENT_MAX];       /* lowercased, prefix stripped */
    uint8_t prefix;                   /* 0 none, 1 min-, 2 max- */
    bool    boolean;                  /* `<mf-boolean>` */
    bool    unknown;                  /* `<general-enclosed>`: parsed, not understood, evaluates to unknown */
    char    raw[MQ_RAW_MAX];          /* a general-enclosed's own text, which is what it serializes as */
    MqValue eq;                       /* `<mf-plain>` / `name = value` */
    uint8_t lo_cmp, hi_cmp;
    MqValue lo, hi;
} MqFeature;

typedef enum { MQC_FEATURE = 0, MQC_NOT, MQC_AND, MQC_OR } MqCondOp;
typedef struct MqCond MqCond;
struct MqCond {
    MqCondOp   op;
    MqFeature  feat;                  /* MQC_FEATURE */
    MqCond   **kids;                  /* MQC_NOT (one), MQC_AND / MQC_OR (two or more) */
    int        nkids;
    bool       parenthesized;         /* `( <media-condition> )` — kept so the serialization round-trips */
};

typedef struct {
    uint8_t  qualifier;               /* 0 none, 1 `not`, 2 `only` */
    char     type[MQ_IDENT_MAX];      /* "" when the query is a bare `<media-condition>` */
    MqCond  *cond;                    /* NULL when the query is a bare `<media-type>` */
    bool     invalid;                 /* §3.1: replaced by `not all` */
} MqQuery;

struct MediaQuerySet {
    MqQuery *q;
    int      n;
};

static void cond_free(MqCond *c)
{
    int i;

    if (!c) return;
    for (i = 0; i < c->nkids; i++) cond_free(c->kids[i]);
    free(c->kids);
    free(c);
}

void media_query_free(MediaQuerySet *set)
{
    int i;

    if (!set) return;
    for (i = 0; i < set->n; i++) cond_free(set->q[i].cond);
    free(set->q);
    free(set);
}

/* ---- the media features — MQ4 §4 ----------------------------------------------------------------------------
 *
 * THE UA's ANSWERS, IN ONE TABLE. Every one of them is a question the spec asks the user agent and that a
 * headless run can still answer: §Headless's missing piece is a physical device, and "does this output device
 * support hover" has a defined answer for a UA that is not driving one. What is NOT modelled here is anything
 * this engine genuinely does not have — there is no such feature in Level 4, which is why the table has no
 * holes and an unknown NAME is the page's, not ours.
 *
 * A DISCRETE FEATURE DECLARES ITS LEGAL VALUES, and that is load-bearing rather than tidy: §3 makes a KNOWN
 * feature with an unacceptable value a SYNTAX ERROR — `(orientation: sideways)` is `not all` — while an
 * UNKNOWN feature is `<general-enclosed>` and merely unknown. Those two produce different answers under `or`,
 * so the table is what tells them apart. */
typedef enum { MFK_LENGTH, MFK_RATIO, MFK_RESOLUTION, MFK_INTEGER, MFK_DISCRETE } MfKind;
typedef struct {
    const char *name;
    MfKind      kind;
    const char *ua;        /* MFK_DISCRETE: the value this user agent reports */
    const char *legal;     /* MFK_DISCRETE: every value the IDL/grammar allows, space separated */
    uint8_t     bool_ctx;  /* MFK_DISCRETE: §2.4.3's boolean-context answer (false only for `none`/`0` kinds) */
} MfDef;

static const MfDef MQ_FEATURES[] = {
    /* §4.1-4.4 — the viewport's own dimensions, and §12's deprecated device-* twins, which report the OUTPUT
       DEVICE rather than the viewport and are kept because pages still ship them. */
    { "width",                MFK_LENGTH },
    { "height",               MFK_LENGTH },
    { "aspect-ratio",         MFK_RATIO },
    { "device-width",         MFK_LENGTH },
    { "device-height",        MFK_LENGTH },
    { "device-aspect-ratio",  MFK_RATIO },
    { "orientation",          MFK_DISCRETE, NULL, "portrait landscape", 1 },   /* computed — see mf_discrete_ua */
    /* §4.5-4.7 — the display. */
    { "resolution",           MFK_RESOLUTION },
    { "scan",                 MFK_DISCRETE, "progressive", "interlace progressive", 1 },
    { "grid",                 MFK_INTEGER },
    { "update",               MFK_DISCRETE, "fast", "none slow fast", 1 },
    { "overflow-block",       MFK_DISCRETE, "scroll", "none scroll optional-paged paged", 1 },
    { "overflow-inline",      MFK_DISCRETE, "scroll", "none scroll", 1 },
    { "color",                MFK_INTEGER },
    { "color-index",          MFK_INTEGER },
    { "monochrome",           MFK_INTEGER },
    { "color-gamut",          MFK_DISCRETE, "srgb", "srgb p3 rec2020", 1 },
    { "dynamic-range",        MFK_DISCRETE, "standard", "standard high", 1 },
    { "video-dynamic-range",  MFK_DISCRETE, "standard", "standard high", 1 },
    /* §4.8-4.9 — the input mechanisms. A headless agent is not a touch device and has no pointer that hovers
       less precisely than a mouse, which is what `fine` and `hover` say. */
    { "pointer",              MFK_DISCRETE, "fine", "none coarse fine", 1 },
    { "any-pointer",          MFK_DISCRETE, "fine", "none coarse fine", 1 },
    { "hover",                MFK_DISCRETE, "hover", "none hover", 1 },
    { "any-hover",            MFK_DISCRETE, "hover", "none hover", 1 },
    /* LEVEL 5's USER PREFERENCES. Each has a `no-preference`/`none` default that is exactly what a UA with no
       user to have expressed one reports, so these are the spec's own answers rather than choices. */
    { "prefers-color-scheme",          MFK_DISCRETE, "light", "light dark", 1 },
    { "prefers-reduced-motion",        MFK_DISCRETE, "no-preference", "no-preference reduce", 0 },
    { "prefers-reduced-transparency",  MFK_DISCRETE, "no-preference", "no-preference reduce", 0 },
    { "prefers-reduced-data",          MFK_DISCRETE, "no-preference", "no-preference reduce", 0 },
    { "prefers-contrast",              MFK_DISCRETE, "no-preference", "no-preference more less custom", 0 },
    { "forced-colors",                 MFK_DISCRETE, "none", "none active", 0 },
    { "inverted-colors",               MFK_DISCRETE, "none", "none inverted", 0 },
    /* §11 — scripting. This engine RUNS the page's scripts, in the document's own realm, which is the whole of
       what `enabled` asserts. */
    { "scripting",                     MFK_DISCRETE, "enabled", "none initial-only enabled", 1 },
    /* Viewport segments (Level 5) — a device with one continuous screen has one of each. */
    { "horizontal-viewport-segments",  MFK_INTEGER },
    { "vertical-viewport-segments",    MFK_INTEGER },
};
#define MQ_NFEATURES ((int)(sizeof(MQ_FEATURES) / sizeof(MQ_FEATURES[0])))

static const MfDef *mf_lookup(const char *name)
{
    int i;

    for (i = 0; i < MQ_NFEATURES; i++)
        if (!strcmp(MQ_FEATURES[i].name, name)) return &MQ_FEATURES[i];
    return NULL;
}

static bool mf_legal_value(const MfDef *d, const char *ident)
{
    const char *p = d->legal;
    size_t n = strlen(ident);

    DCHECK(d->kind == MFK_DISCRETE, "a range media feature was asked whether an IDENT is one of its values");
    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t len = sp ? (size_t)(sp - p) : strlen(p);

        if (len == n && !memcmp(p, ident, n)) return true;
        if (!sp) break;
        p = sp + 1;
    }
    return false;
}

/* ---- the environment ---------------------------------------------------------------------------------------- */

typedef struct {
    double width, height;      /* the viewport, in CSS pixels */
    double dev_width, dev_height;
    double dppx;
    double font_size;          /* css-values-4 §6.1.1's "initial values of the font ... properties" */
    double ascent;             /* CSS 2.1 §10.8.1's `A` at that size — core/css/font_metrics.h's picked face */
    double line_height;        /* §6.1.1's `lh` base — the same clause's "line-height" half, `normal` resolved */
    int    color_bits;
} MqEnv;

/* THE ENVIRONMENT AS `double`s — THE EXAMPLE — AND THAT IS WHERE THE TWO PATHS AGREE. viewport.c answers the
   modelled viewport as a number and mints the concolic at the JS boundary (`viewport_env_value`); screen.c does
   the same for the output device. This file evaluates the real predicate against those numbers, and
   media_query_list.c wraps the RESULT. So one fact is opaque for control flow through
   `matchMedia('(max-width: 768px)').matches` and through `innerWidth < 768` alike, and neither path ever puts a
   concolic in front of a C `if`. A value read here that was already concolic would be the seam on the wrong
   side of the boundary — see media_query.h for why the mint belongs to the reader and not to the language. */
static void mq_env(JSContext *ctx, MqEnv *e)
{
    e->width = viewport_width(ctx);
    e->height = viewport_height(ctx);
    e->dppx = viewport_device_pixel_ratio(ctx);
    /* §12's device-* features report the OUTPUT DEVICE. The modelled display is screen.c's, in device pixels,
       expressed in CSS pixels here because every length in a media query is one. ASKED of screen.c rather than
       written out: this held its own literal 1920 and 1080, which is the same fact answered from two places —
       the defect CLAUDE.md names — and the copy that is not maintained is the one that goes on being wrong. */
    e->dev_width = screen_width() / e->dppx;
    e->dev_height = screen_height() / e->dppx;
    /* css-values-4 §6.1.1 (Font-relative Lengths: the em, rem, ex, rex, cap, rcap, ch, rch, ic, ric, lh, rlh
       units): "when used OUTSIDE THE CONTEXT OF AN ELEMENT (such as in media queries), the font-relative
       lengths units refer to the metrics corresponding to the INITIAL VALUES of the font and line-height
       properties." So a media query's `em` is the initial `font-size` — css-fonts-4 §2.5's `medium` — and this
       file held its own literal 16 for it, which is the same fact answered from two places exactly as the
       1920/1080 two lines up was. core/css/font_size_functions.h picks the number; what is read here is its
       EXAMPLE, for the reason stated above this function: the comparison below is a C `if`. */
    e->font_size = css_default_font_size(ctx).px;
    /* §6.1.1's `cap` falls back to "the font's ascent", which is the SAME modelled face outside an element as
       inside one — §6.1.1's clause redirects the SIZE to the initial value and says nothing about the face.
       Its EXAMPLE is read here for the reason stated above this function, exactly as the font size's is: the
       comparison below is a C `if`, and core/css/font_metrics.c is where the fact behind the number is minted
       for the readers that cross to a page. */
    e->ascent = font_metrics_ascent_px(ctx, css_px(e->font_size)).px;
    /* §6.1.1's clause quoted above names TWO initial values, not one — "the initial values of the font AND
       LINE-HEIGHT properties" — and the second is what `lh` is a multiple of. css-inline-3 §5.1 "Line Spacing:
       the line-height property" gives `Initial: normal`, and §6.1.1 states the conversion for exactly that
       keyword: `lh` is "equal to the computed value of the line-height property of the element on which it is
       used, CONVERTING NORMAL TO AN ABSOLUTE LENGTH BY USING ONLY THE METRICS OF THE FIRST AVAILABLE FONT".
       That is one number core/css/font_metrics.h already owns under CSS 2.1 §10.8.1's name for it, `AD`, and
       it is READ here rather than re-derived for the reason the two lines above are: the ratio would then be
       one fact in two places, and a page could read two different answers for `line-height: normal` through
       `getComputedStyle` and through `1lh`. */
    e->line_height = font_metrics_normal_line_height_px(ctx, css_px(e->font_size)).px;
    /* THE ENVIRONMENT'S OWN INVARIANT, ASSERTED WHERE IT IS BORN. Every viewport-percentage and container
       length below is a FRACTION of one of these two extents, so a NaN or a non-positive one does not fail
       loudly there — it produces a plausible number that rides on as a concolic's EXAMPLE, which is strictly
       worse than a crash because a fabricated example is what the forced-execution search then composes its
       next candidate out of. core/frame/viewport.h answers a MODELLED rectangle — a picked size for the
       top-level traversable, and a child navigable's own for an iframe — and states which spec rule fixes
       each; both are positive by construction, and that header is where they are cited rather than here,
       because a citation repeated is a citation that can go stale in two places. So a violation is this
       engine's own geometry having lost an operand, not a fact about a page. */
    DCHECK(e->width > 0.0 && e->width == e->width && e->width * 0.0 == 0.0,
           "the modelled viewport reported a WIDTH that is not a finite positive number of CSS pixels. Every "
           "css-values-4 §6.1.2 viewport-percentage length is a fraction of it, so a zero, a negative, a NaN "
           "or an infinity here becomes a `100vw` this engine reports as a real measurement");
    DCHECK(e->height > 0.0 && e->height == e->height && e->height * 0.0 == 0.0,
           "the modelled viewport reported a HEIGHT that is not a finite positive number of CSS pixels. Every "
           "css-values-4 §6.1.2 viewport-percentage length is a fraction of it, so a zero, a negative, a NaN "
           "or an infinity here becomes a `100vh` this engine reports as a real measurement");
    /* §4.5: `color` is the bits per COLOUR COMPONENT of the output device. screen.c owns the depth. */
    e->color_bits = screen_color_depth() / 3;
}

/* The UA's value for a discrete feature, which for `orientation` is COMPUTED from the viewport rather than
   chosen — §4.4 defines it as portrait when height >= width. A table entry with a NULL `ua` is one of those. */
static const char *mf_discrete_ua(const MfDef *d, const MqEnv *e)
{
    if (d->ua) return d->ua;
    DCHECK(!strcmp(d->name, "orientation"),
           "a discrete media feature declared no user-agent value and is not one this file computes");
    return e->height >= e->width ? "portrait" : "landscape";
}

/* ---- the tokenizer ------------------------------------------------------------------------------------------
   CSS Syntax §4's tokens, narrowed to what §3's grammar can contain. It is a scanner over the source rather
   than a token array because the grammar is LL(2) at worst and a `<general-enclosed>` needs the raw span. */
enum { TK_EOF = 0, TK_IDENT, TK_NUM, TK_DIM, TK_COLON, TK_COMMA, TK_LPAREN, TK_RPAREN,
       TK_LT, TK_LE, TK_GT, TK_GE, TK_EQ, TK_SLASH, TK_JUNK };

typedef struct {
    const char *s;      /* the whole source, so a span can be taken */
    const char *end;    /* one past its last byte — CSS Syntax §4.2 is asked of a CODE POINT, and reading one
                           out of UTF-8 needs an extent; this text is NUL-terminated, so it is computed once
                           at init rather than re-measured per token */
    const char *p;      /* the cursor, at the START of `kind` */
    const char *next;   /* just past `kind` */
    int         kind;
    double      num;
    char        ident[MQ_IDENT_MAX];   /* TK_IDENT: lowercased; TK_DIM: the unit, lowercased */
} MqLex;

static bool mq_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static bool mq_is_digit(char c) { return c >= '0' && c <= '9'; }

/* WHAT MAY OPEN A `<media-feature>` NAME OR A `<dimension>` UNIT — CSS Syntax §4.2's ident-start code point,
 * plus §4.3.9 "Check if three code points would start an ident sequence"'s leading hyphen, which the caller's
 * own `-5` guard is the rest of.
 *
 * IT ASKS ABOUT A CODE POINT AND NOT A BYTE, AND THAT IS THE WHOLE OF THE CHANGE HERE. What stood in its place
 * ended in `(unsigned char)c >= 0x80`, which is not §4.2's set: §4.2 enumerates the non-ASCII ident code
 * points and leaves U+00D7, U+00F7, U+037E, U+FFFE and U+FFFF outside it, while every byte of each of their
 * UTF-8 encodings is >= 0x80. One question asked in one wrong way at three sites — core/css/css_code_point.h
 * is the one answer all three now route to. */
static bool mq_is_name_start(uint32_t cp) { return cp == '-' || css_cp_is_ident_start(cp); }
/* No hyphen arm here, and that asymmetry is CSS Syntax §4.2 "Definitions"'s: its ident code point is "An
   ident-start code point, a digit, or U+002D HYPHEN-MINUS (-)", which already contains the hyphen, so only
   the START of a name needs one added. */
static bool mq_is_name(uint32_t cp) { return css_cp_is_ident(cp); }
static char mq_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* THE BYTES of the ident sequence at `p`, which is what the two scans below copy. A LENGTH rather than a walk
   that copies as it goes, because the copy is over BYTES — `mq_lower` touches only ASCII, so a multi-byte code
   point is carried through it unchanged — and only the END of the sequence is a §4.2 question. */
static size_t mq_ident_len(const MqLex *L, const char *p)
{
    size_t k = 0, n;

    while (mq_is_name(css_cp_at(p + k, L->end, &n))) k += n;
    return k;
}

static void mq_scan(MqLex *L)
{
    const char *p = L->next;
    size_t i;

    while (mq_is_space(*p)) p++;
    L->p = p;
    L->ident[0] = 0;
    L->num = 0;
    if (!*p) { L->kind = TK_EOF; L->next = p; return; }
    if (mq_is_name_start(css_cp_at(p, L->end, NULL)) && !(*p == '-' && mq_is_digit(p[1]))) {
        size_t got = mq_ident_len(L, p);

        for (i = 0; i < got; i++)
            if (i + 1 < sizeof(L->ident)) L->ident[i] = mq_lower(p[i]);
        L->ident[i < sizeof(L->ident) ? i : sizeof(L->ident) - 1] = 0;
        L->kind = TK_IDENT;
        L->next = p + got;
        return;
    }
    if (mq_is_digit(*p) || ((*p == '-' || *p == '+' || *p == '.') && (mq_is_digit(p[1]) ||
                            (p[1] == '.' && mq_is_digit(p[2]))))) {
        char *end = NULL;
        L->num = strtod(p, &end);
        p = end;
        if (mq_is_name_start(css_cp_at(p, L->end, NULL))) {   /* a `<dimension>`: the unit rides the number */
            size_t got = mq_ident_len(L, p);

            for (i = 0; i < got; i++)
                if (i + 1 < sizeof(L->ident)) L->ident[i] = mq_lower(p[i]);
            L->ident[i < sizeof(L->ident) ? i : sizeof(L->ident) - 1] = 0;
            L->kind = TK_DIM;
            L->next = p + got;
        } else {
            L->kind = TK_NUM;
            L->next = p;
        }
        return;
    }
    L->next = p + 1;
    switch (*p) {
    case ':': L->kind = TK_COLON; return;
    case ',': L->kind = TK_COMMA; return;
    case '(': L->kind = TK_LPAREN; return;
    case ')': L->kind = TK_RPAREN; return;
    case '/': L->kind = TK_SLASH; return;
    case '=': L->kind = TK_EQ; return;
    case '<': if (p[1] == '=') { L->next = p + 2; L->kind = TK_LE; } else L->kind = TK_LT; return;
    case '>': if (p[1] == '=') { L->next = p + 2; L->kind = TK_GE; } else L->kind = TK_GT; return;
    default:  L->kind = TK_JUNK; return;
    }
}

static void mq_lex_init(MqLex *L, const char *s)
{
    L->s = s;
    L->end = s + strlen(s);
    L->next = s;
    mq_scan(L);
}

static bool tk_is_ident(const MqLex *L, const char *w) { return L->kind == TK_IDENT && !strcmp(L->ident, w); }

/* ---- the parser ---------------------------------------------------------------------------------------------
   §3's grammar, recursive descent. A production that fails returns false and the QUERY becomes `not all`; there
   is no error to report and no partial tree to keep, which is why every failure path frees what it built. */

static MqCond *cond_new(MqCondOp op)
{
    MqCond *c = calloc(1, sizeof *c);

    CHECK(c != NULL, "media queries: OOM building a media condition");
    c->op = op;
    return c;
}

static void cond_add(MqCond *c, MqCond *kid)
{
    MqCond **k = realloc(c->kids, (size_t)(c->nkids + 1) * sizeof *k);

    CHECK(k != NULL, "media queries: OOM extending a media condition");
    c->kids = k;
    c->kids[c->nkids++] = kid;
}

static bool parse_condition(MqLex *L, bool allow_or, MqCond **out);

/* `<mf-value> = <number> | <dimension> | <ident> | <ratio>`. A `<ratio>` is `<number> [ / <number> ]?`, so a
   bare number that is followed by a slash becomes one here rather than in the caller. */
static bool parse_value(MqLex *L, MqValue *v)
{
    memset(v, 0, sizeof *v);
    if (L->kind == TK_IDENT) {
        snprintf(v->ident, sizeof v->ident, "%s", L->ident);
        v->kind = MV_IDENT;
        mq_scan(L);
        return true;
    }
    if (L->kind == TK_DIM) {
        v->kind = MV_DIMENSION;
        v->a = L->num;
        snprintf(v->unit, sizeof v->unit, "%s", L->ident);
        mq_scan(L);
        return true;
    }
    if (L->kind == TK_NUM) {
        v->kind = MV_NUMBER;
        v->a = L->num;
        v->b = 1;
        mq_scan(L);
        if (L->kind == TK_SLASH) {
            mq_scan(L);
            if (L->kind != TK_NUM) return false;
            v->kind = MV_RATIO;
            v->b = L->num;
            mq_scan(L);
        }
        return true;
    }
    return false;
}

static int parse_cmp(MqLex *L)
{
    switch (L->kind) {
    case TK_LT: mq_scan(L); return MQ_CMP_LT;
    case TK_LE: mq_scan(L); return MQ_CMP_LE;
    case TK_GT: mq_scan(L); return MQ_CMP_GT;
    case TK_GE: mq_scan(L); return MQ_CMP_GE;
    case TK_EQ: mq_scan(L); return MQ_CMP_EQ;
    default: return MQ_CMP_NONE;
    }
}

/* Is this value acceptable for this feature? §3 makes a KNOWN feature with an unacceptable value a syntax
   error, which is the difference between `not all` and merely-unknown. */
static bool value_ok(const MfDef *d, const MqValue *v)
{
    if (d->kind == MFK_DISCRETE)
        return v->kind == MV_IDENT && mf_legal_value(d, v->ident);
    if (v->kind == MV_IDENT) return false;
    switch (d->kind) {
    case MFK_LENGTH:
        /* §4: a length, and ZERO may be written unitless — which is CSS's rule for lengths everywhere. */
        return v->kind == MV_DIMENSION || (v->kind == MV_NUMBER && v->a == 0);
    case MFK_RATIO:
        return v->kind == MV_RATIO || v->kind == MV_NUMBER;
    case MFK_RESOLUTION:
        return v->kind == MV_DIMENSION;
    case MFK_INTEGER:
        return v->kind == MV_NUMBER && v->a == floor(v->a);
    default:
        DFAIL("a media feature declared a kind media_query.c does not evaluate");
        return false;
    }
}

/* `<media-feature>` — everything between the parentheses, which the caller has already consumed. The whole
   §3 shape is here because the three spellings (`<mf-boolean>`, `<mf-plain>`, `<mf-range>`) are distinguished
   only by what follows the first token. */
static bool parse_feature_body(MqLex *L, MqFeature *f)
{
    const MfDef *d;
    MqValue first;

    memset(f, 0, sizeof *f);
    if (L->kind == TK_IDENT) {
        const char *n = L->ident;

        /* `min-`/`max-` are §4's other spelling of a bound, so they normalise into one. */
        if (!strncmp(n, "min-", 4)) { f->prefix = 1; n += 4; }
        else if (!strncmp(n, "max-", 4)) { f->prefix = 2; n += 4; }
        snprintf(f->name, sizeof f->name, "%s", n);
        d = mf_lookup(f->name);
        if (!d) return false;                              /* not a feature: the caller retries as enclosed */
        mq_scan(L);
        if (L->kind == TK_RPAREN) {                        /* `<mf-boolean>` */
            /* §4: the min-/max- prefixed forms take a value, so they have no boolean context. */
            if (f->prefix) return false;
            f->boolean = true;
            return true;
        }
        if (L->kind == TK_COLON) {                         /* `<mf-plain>` */
            mq_scan(L);
            if (!parse_value(L, &f->eq)) return false;
            if (!value_ok(d, &f->eq)) return false;
            if (f->prefix == 1) { f->lo_cmp = MQ_CMP_GE; f->lo = f->eq; f->eq.kind = MV_ABSENT; }
            else if (f->prefix == 2) { f->hi_cmp = MQ_CMP_LE; f->hi = f->eq; f->eq.kind = MV_ABSENT; }
            return L->kind == TK_RPAREN;
        }
        /* `<mf-range>` in its `name <cmp> value` form. A prefixed name may not carry one, and a DISCRETE
           feature has no ordering to compare with — both are syntax errors, not unknowns. */
        {
            int cmp = parse_cmp(L);

            if (cmp == MQ_CMP_NONE || f->prefix || d->kind == MFK_DISCRETE) return false;
            if (!parse_value(L, &f->hi)) return false;
            if (!value_ok(d, &f->hi)) return false;
            if (cmp == MQ_CMP_EQ) { f->eq = f->hi; f->hi.kind = MV_ABSENT; }
            else if (cmp == MQ_CMP_LT || cmp == MQ_CMP_LE) f->hi_cmp = (uint8_t)cmp;
            else { f->lo = f->hi; f->hi.kind = MV_ABSENT; f->lo_cmp = (uint8_t)(cmp == MQ_CMP_GT ? MQ_CMP_GT : MQ_CMP_GE); }
            return L->kind == TK_RPAREN;
        }
    }
    /* `<mf-range>` in its `value <cmp> name [ <cmp> value ]?` forms. */
    if (!parse_value(L, &first)) return false;
    {
        int cmp1 = parse_cmp(L), cmp2;

        if (cmp1 == MQ_CMP_NONE || cmp1 == MQ_CMP_EQ || L->kind != TK_IDENT) return false;
        snprintf(f->name, sizeof f->name, "%s", L->ident);
        d = mf_lookup(f->name);
        if (!d || d->kind == MFK_DISCRETE) return false;
        if (!value_ok(d, &first)) return false;
        mq_scan(L);
        /* `600px < width` bounds the feature from BELOW: the operator is read left to right, so it inverts. */
        if (cmp1 == MQ_CMP_LT || cmp1 == MQ_CMP_LE) {
            f->lo_cmp = (uint8_t)(cmp1 == MQ_CMP_LT ? MQ_CMP_GT : MQ_CMP_GE);
            f->lo = first;
        } else {
            f->hi_cmp = (uint8_t)(cmp1 == MQ_CMP_GT ? MQ_CMP_LT : MQ_CMP_LE);
            f->hi = first;
        }
        if (L->kind == TK_RPAREN) return true;
        cmp2 = parse_cmp(L);
        if (cmp2 == MQ_CMP_NONE || cmp2 == MQ_CMP_EQ) return false;
        /* §3: the two comparisons of a two-sided range must point the SAME way. */
        if ((cmp1 == MQ_CMP_LT || cmp1 == MQ_CMP_LE) != (cmp2 == MQ_CMP_LT || cmp2 == MQ_CMP_LE)) return false;
        if (cmp2 == MQ_CMP_LT || cmp2 == MQ_CMP_LE) {
            if (f->hi_cmp) return false;
            f->hi_cmp = (uint8_t)cmp2;
            if (!parse_value(L, &f->hi) || !value_ok(d, &f->hi)) return false;
        } else {
            if (f->lo_cmp) return false;
            f->lo_cmp = (uint8_t)(cmp2 == MQ_CMP_GT ? MQ_CMP_GT : MQ_CMP_GE);
            if (!parse_value(L, &f->lo) || !value_ok(d, &f->lo)) return false;
        }
        return L->kind == TK_RPAREN;
    }
}

/* Consume a balanced `( … )` starting at the current `(`, recording its text — §3's `<general-enclosed>`, which
   is how forward compatibility works: a construct this UA does not understand is still SYNTAX, so it parses and
   evaluates to unknown rather than poisoning the query. */
static bool parse_enclosed(MqLex *L, MqFeature *f)
{
    const char *start = L->p;
    int depth = 0;
    size_t len;

    memset(f, 0, sizeof *f);
    f->unknown = true;
    for (;;) {
        if (L->kind == TK_EOF) return false;
        if (L->kind == TK_LPAREN) depth++;
        else if (L->kind == TK_RPAREN && --depth == 0) { mq_scan(L); break; }
        mq_scan(L);
    }
    len = (size_t)(L->p - start);
    while (len && mq_is_space(start[len - 1])) len--;
    if (len >= sizeof f->raw) len = sizeof f->raw - 1;
    memcpy(f->raw, start, len);
    f->raw[len] = 0;
    return true;
}

/* `<media-in-parens> = ( <media-condition> ) | <media-feature> | <general-enclosed>` */
static bool parse_in_parens(MqLex *L, MqCond **out)
{
    MqLex save = *L;
    MqCond *inner = NULL;
    MqFeature f;

    if (L->kind != TK_LPAREN) return false;
    mq_scan(L);
    if (parse_condition(L, true, &inner) && L->kind == TK_RPAREN) {
        mq_scan(L);
        inner->parenthesized = true;
        *out = inner;
        return true;
    }
    cond_free(inner);
    *L = save;
    mq_scan(L);
    if (parse_feature_body(L, &f) && L->kind == TK_RPAREN) {
        mq_scan(L);
        *out = cond_new(MQC_FEATURE);
        (*out)->feat = f;
        return true;
    }
    *L = save;
    if (!parse_enclosed(L, &f)) return false;
    *out = cond_new(MQC_FEATURE);
    (*out)->feat = f;
    return true;
}

/* `<media-condition>` (§3), with `allow_or` false expressing `<media-condition-without-or>` — the production a
   media query's `<media-type> and …` tail uses, because `screen and (a) or (b)` is ambiguous and the grammar
   forbids it rather than picking a precedence. */
static bool parse_condition(MqLex *L, bool allow_or, MqCond **out)
{
    MqCond *first = NULL, *combined;
    bool is_and;

    if (tk_is_ident(L, "not")) {
        mq_scan(L);
        if (!parse_in_parens(L, &first)) return false;
        combined = cond_new(MQC_NOT);
        cond_add(combined, first);
        *out = combined;
        return true;
    }
    if (!parse_in_parens(L, &first)) return false;
    if (!tk_is_ident(L, "and") && !tk_is_ident(L, "or")) { *out = first; return true; }
    is_and = tk_is_ident(L, "and");
    if (!is_and && !allow_or) { cond_free(first); return false; }
    combined = cond_new(is_and ? MQC_AND : MQC_OR);
    cond_add(combined, first);
    while (tk_is_ident(L, is_and ? "and" : "or")) {
        MqCond *next = NULL;

        mq_scan(L);
        if (!parse_in_parens(L, &next)) { cond_free(combined); return false; }
        cond_add(combined, next);
    }
    /* §3: `and` and `or` may not be MIXED at one level without parentheses. */
    if (tk_is_ident(L, "and") || tk_is_ident(L, "or")) { cond_free(combined); return false; }
    *out = combined;
    return true;
}

/* Is this ident usable as a `<media-type>`? §3 excludes the keywords the grammar around it needs. */
static bool is_media_type_ident(const char *s)
{
    return strcmp(s, "not") && strcmp(s, "only") && strcmp(s, "and") && strcmp(s, "or") && strcmp(s, "layer");
}

/* `<media-query> = <media-condition> | [ not | only ]? <media-type> [ and <media-condition-without-or> ]?` */
static void parse_query(MqLex *L, MqQuery *q)
{
    MqLex save = *L;

    memset(q, 0, sizeof *q);
    if (parse_condition(L, true, &q->cond) && (L->kind == TK_COMMA || L->kind == TK_EOF))
        return;
    cond_free(q->cond);
    q->cond = NULL;
    *L = save;
    if (tk_is_ident(L, "not")) { q->qualifier = 1; mq_scan(L); }
    else if (tk_is_ident(L, "only")) { q->qualifier = 2; mq_scan(L); }
    if (L->kind != TK_IDENT || !is_media_type_ident(L->ident)) { q->invalid = true; return; }
    snprintf(q->type, sizeof q->type, "%s", L->ident);
    mq_scan(L);
    if (L->kind == TK_COMMA || L->kind == TK_EOF) return;
    if (!tk_is_ident(L, "and")) { q->invalid = true; return; }
    mq_scan(L);
    if (!parse_condition(L, false, &q->cond) || (L->kind != TK_COMMA && L->kind != TK_EOF)) {
        cond_free(q->cond);
        q->cond = NULL;
        q->invalid = true;
    }
}

MediaQuerySet *media_query_parse(const char *text)
{
    MediaQuerySet *set = calloc(1, sizeof *set);
    MqLex L;

    CHECK(set != NULL, "media queries: OOM parsing a media query list");
    if (!text) text = "";
    mq_lex_init(&L, text);
    /* §3.1: an EMPTY list is a list of no queries and it matches everything — `matchMedia("").matches` is true.
       That is not a special case here, it is what evaluating an empty disjunction means, so it needs only that
       nothing is appended. */
    if (L.kind == TK_EOF) return set;
    for (;;) {
        MqQuery *grown = realloc(set->q, (size_t)(set->n + 1) * sizeof *grown);

        CHECK(grown != NULL, "media queries: OOM extending a media query list");
        set->q = grown;
        parse_query(&L, &set->q[set->n]);
        if (set->q[set->n].invalid) {
            /* Skip to the next top-level comma: the OFFENDING QUERY is replaced by `not all`, and the rest of
               the list survives. */
            int depth = 0;
            while (L.kind != TK_EOF && !(L.kind == TK_COMMA && depth == 0)) {
                if (L.kind == TK_LPAREN) depth++;
                else if (L.kind == TK_RPAREN && depth > 0) depth--;
                mq_scan(&L);
            }
        }
        set->n++;
        if (L.kind != TK_COMMA) break;
        mq_scan(&L);
    }
    return set;
}

/* ---- serialization — CSSOM "serialize a media query list" ---------------------------------------------------- */

typedef struct { char *s; size_t len, cap; } MqOut;

static void out_put(MqOut *o, const char *s)
{
    size_t n = strlen(s);

    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 64;
        char *grown;

        while (cap < o->len + n + 1) cap *= 2;
        grown = realloc(o->s, cap);
        CHECK(grown != NULL, "media queries: OOM serializing a media query list");
        o->s = grown;
        o->cap = cap;
    }
    memcpy(o->s + o->len, s, n + 1);
    o->len += n;
}

/* CSS numbers serialize with no trailing zeros and no exponent for the range a media query can hold. */
static void out_num(MqOut *o, double d)
{
    char b[40];

    if (d == floor(d) && fabs(d) < 1e15) snprintf(b, sizeof b, "%.0f", d);
    else {
        int i;
        snprintf(b, sizeof b, "%.6f", d);
        for (i = (int)strlen(b) - 1; i > 0 && b[i] == '0'; i--) b[i] = 0;
        if (b[strlen(b) - 1] == '.') b[strlen(b) - 1] = 0;
    }
    out_put(o, b);
}

static void out_value(MqOut *o, const MqValue *v)
{
    switch (v->kind) {
    case MV_IDENT: out_put(o, v->ident); return;
    case MV_NUMBER: out_num(o, v->a); return;
    case MV_DIMENSION: out_num(o, v->a); out_put(o, v->unit); return;
    case MV_RATIO: out_num(o, v->a); out_put(o, " / "); out_num(o, v->b); return;
    default: DFAIL("a media feature value with no kind reached the serializer"); return;
    }
}

static const char *cmp_text(int cmp)
{
    switch (cmp) {
    case MQ_CMP_LT: return " < ";
    case MQ_CMP_LE: return " <= ";
    case MQ_CMP_GT: return " > ";
    case MQ_CMP_GE: return " >= ";
    default: DFAIL("a media feature bound with no comparison reached the serializer"); return " ";
    }
}

static void out_feature(MqOut *o, const MqFeature *f)
{
    if (f->unknown) { out_put(o, f->raw); return; }
    out_put(o, "(");
    /* The min-/max- spellings serialize as themselves: they are what the page wrote, and CSSOM's serialization
       is of the query it parsed rather than of a canonical rewrite of it. */
    if (f->prefix) {
        out_put(o, f->prefix == 1 ? "min-" : "max-");
        out_put(o, f->name);
        out_put(o, ": ");
        out_value(o, f->prefix == 1 ? &f->lo : &f->hi);
        out_put(o, ")");
        return;
    }
    if (f->lo_cmp && f->hi_cmp) {           /* the two-sided range keeps its `value op name op value` shape */
        out_value(o, &f->lo);
        out_put(o, cmp_text(f->lo_cmp == MQ_CMP_GT ? MQ_CMP_LT : MQ_CMP_LE));
        out_put(o, f->name);
        out_put(o, cmp_text(f->hi_cmp));
        out_value(o, &f->hi);
    } else if (f->lo_cmp) {
        out_put(o, f->name);
        out_put(o, cmp_text(f->lo_cmp));
        out_value(o, &f->lo);
    } else if (f->hi_cmp) {
        out_put(o, f->name);
        out_put(o, cmp_text(f->hi_cmp));
        out_value(o, &f->hi);
    } else if (f->boolean) {
        out_put(o, f->name);
    } else {
        out_put(o, f->name);
        out_put(o, ": ");
        out_value(o, &f->eq);
    }
    out_put(o, ")");
}

static void out_cond(MqOut *o, const MqCond *c)
{
    int i;

    if (c->parenthesized) out_put(o, "(");
    switch (c->op) {
    case MQC_FEATURE: out_feature(o, &c->feat); break;
    case MQC_NOT:     out_put(o, "not "); out_cond(o, c->kids[0]); break;
    case MQC_AND:
    case MQC_OR:
        for (i = 0; i < c->nkids; i++) {
            if (i) out_put(o, c->op == MQC_AND ? " and " : " or ");
            out_cond(o, c->kids[i]);
        }
        break;
    }
    if (c->parenthesized) out_put(o, ")");
}

/* CSSOM §4.2's SERIALIZE A MEDIA QUERY, for ONE query.
   ITS FOURTH STEP IS THE ONE THAT IS EASY TO MISS AND THE ONE A PAGE SEES: "if type is not `all` OR the media
   query is negated, append type, followed by ` and `" — so `all and (color)` serializes as `(color)` and
   `not all and (color)` keeps its type, which is the spec's own example table and what
   css/cssom/serialize-media-rule.html asserts byte for byte. `only` is not a negation and is not `all`'s
   omission either: §2.2 makes it a no-op for evaluation, and every engine keeps it in the serialization, so it
   holds the type the way a non-`all` type does. */
static void out_query(MqOut *o, const MqQuery *q)
{
    bool keep_type;

    if (q->invalid) { out_put(o, "not all"); return; }     /* §3.1: the replacement IS the serialization */
    if (q->qualifier) out_put(o, q->qualifier == 1 ? "not " : "only ");
    if (!q->type[0]) {
        DCHECK(q->cond != NULL, "a media query with neither a type nor a condition survived the parser");
        out_cond(o, q->cond);
        return;
    }
    /* Step 3 first: "if the media query does not contain media features append type, then return." */
    if (!q->cond) { out_put(o, q->type); return; }
    keep_type = strcmp(q->type, "all") != 0 || q->qualifier != 0;
    if (keep_type) {
        out_put(o, q->type);
        out_put(o, " and ");
    }
    out_cond(o, q->cond);
}

char *media_query_serialize(const MediaQuerySet *set)
{
    MqOut o = { NULL, 0, 0 };
    int i;

    out_put(&o, "");
    for (i = 0; i < set->n; i++) {
        if (i) out_put(&o, ", ");
        out_query(&o, &set->q[i]);
    }
    return o.s;
}

int media_query_count(const MediaQuerySet *set)
{
    DCHECK(set != NULL, "a media query list was counted after it was freed");
    return set->n;
}

char *media_query_serialize_at(const MediaQuerySet *set, int i)
{
    MqOut o = { NULL, 0, 0 };

    DCHECK(set != NULL, "a media query was serialized out of a list that was freed");
    if (i < 0 || i >= set->n) return NULL;      /* §4.4's `item`: null at or past the count */
    out_put(&o, "");
    out_query(&o, &set->q[i]);
    return o.s;
}

MediaQuerySet *media_query_parse_one(const char *text)
{
    MediaQuerySet *set = media_query_parse(text);

    /* §3.1's forward-compatible rule is about a query IN A LIST; a single query that does not match the grammar
       has no `not all` to become, it simply does not parse — which is the null both §4.4 methods return on.
       A comma makes it a LIST rather than a query, so more than one is a failure here too. */
    if (set->n == 1 && !set->q[0].invalid) return set;
    media_query_free(set);
    return NULL;
}

/* ---- evaluation — MQ4 §3 and §4 ------------------------------------------------------------------------------ */

/* css-writing-modes-4 §6.1 "Abstract Dimensions"'s INLINE and BLOCK AXES, resolved to the PHYSICAL ones. §6.1
   defines each relative to a writing mode — "inline axis: The axis in the inline dimension, i.e. the horizontal
   axis in horizontal writing modes and the vertical axis in vertical writing modes", and the block axis is
   "the axis in the block dimension, i.e. the vertical axis in horizontal writing modes and the horizontal axis
   in vertical writing modes". WHICH writing mode is not a choice made here: a media query is evaluated OUTSIDE
   THE CONTEXT OF AN ELEMENT, so there is no box whose `writing-mode` could be read and the property's initial
   value applies — css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property" gives
   `Initial: horizontal-tb`, whose typographic mode is horizontal.
   IT IS ONE FUNCTION rather than a `?:` at each reader because the abstract axes are asked for by BOTH families
   below — css-values-4 §6.1.2.2's `vi` and `vb` in each of their four spellings, and CSS Conditional 5 §7's
   `cqi` and `cqb` plus the pair §7 defines OVER them — so the day this engine models a vertical writing mode
   there is one place that answers, and no two of those readers can disagree about it in the meantime. */
typedef enum { MQ_AXIS_INLINE = 0, MQ_AXIS_BLOCK } MqAxis;

static double mq_axis_size(MqAxis axis, const MqEnv *e)
{
    DCHECK(axis == MQ_AXIS_INLINE || axis == MQ_AXIS_BLOCK,
           "css-writing-modes-4 §6.1's abstract dimensions were asked for an axis that is neither the inline "
           "one nor the block one — §6.1 defines exactly that pair, so a third value is a caller that composed "
           "an axis out of arithmetic instead of naming one");
    return axis == MQ_AXIS_BLOCK ? e->height : e->width;
}

/* THE EXTENT ONE PERCENT IS TAKEN OF, for each of css-values-4 §6.1.2.2's six viewport-percentage unit names —
   `u` is the name with §6.1.2.1's variant letter already stripped. FALSE for anything else, which is what makes
   this a lookup the caller can ask about a candidate rather than a table it must first know the answer for.
   §6.1.2.2 states each of the six in its own words and they are NOT one rule with six spellings: `*vw` and
   `*vh` are "1% of the width" and "1% of the height"; `*vi` and `*vb` are "1% of the size ... in the box's
   inline axis" and "... in the box's block axis", which is the abstract pair above and not the physical one;
   and `*vmin` and `*vmax` are "the smaller of *vw or *vh" and "the larger of *vw or *vh" — THE WIDTH AND HEIGHT
   PAIR, never the inline and block one. That last sentence is the whole reason §7's `cqmin` below cannot share
   this function: §7 pairs its own minimum over `cqi` and `cqb` instead, so reading the two out of one table
   would be four coincident spellings standing in for two different definitions. */
static bool mq_viewport_extent(const char *u, const MqEnv *e, double *extent)
{
    if (!strcmp(u, "vw"))   { *extent = e->width;  return true; }
    if (!strcmp(u, "vh"))   { *extent = e->height; return true; }
    if (!strcmp(u, "vi"))   { *extent = mq_axis_size(MQ_AXIS_INLINE, e); return true; }
    if (!strcmp(u, "vb"))   { *extent = mq_axis_size(MQ_AXIS_BLOCK, e);  return true; }
    if (!strcmp(u, "vmin")) { *extent = e->width < e->height ? e->width : e->height; return true; }
    if (!strcmp(u, "vmax")) { *extent = e->width > e->height ? e->width : e->height; return true; }
    return false;
}

/* THE EXTENT ONE PERCENT IS TAKEN OF, for each of CSS Conditional 5 §7 "Container Relative Lengths"'s six
   units. §7 states what one is worth in two sentences and this engine reaches only the second of them: "The
   query container for each axis is the nearest ancestor container that accepts container size queries on that
   axis. If no eligible query container is available, then use the small viewport size for that axis."
   THERE IS NO ELIGIBLE QUERY CONTAINER IN THIS ENGINE AND THE FALLBACK IS THEREFORE THE WHOLE ANSWER, not an
   approximation of one. §5.1 "Creating Query Containers: the container-type property" is what makes an element
   a query container, and `container-type` is not a property this engine cascades at all — `@container` reaches
   core/css/css_at_rule_prelude.c as a PRELUDE and core/css/css_rule.c as a CSSOM `CSSContainerRule`, and
   neither of those makes any element answer a size query. So §7's antecedent holds for every element there is.
   NAMED RESIDUAL — the code is correct and NARROWER than §7, so there is nothing here to crash on:
     NOT COVERED: a `1cqw` inside an element that IS a query container, which must be 1% of THAT element's
       width and not of the viewport.
     THE NEXT DIFF BUILDS: `container-type` as a cascaded property and the nearest-ancestor-container walk
       §7's first sentence names, which is §5.1's machinery and a component of its own. Grep
       `"container-type"` across core/css before building it — the day that returns a property table row
       rather than the two prose hits above, this function has become a silent wrong answer.
     ITS ABSENCE WOULD SHOW as `@container (inline-size >= 400px) { .card { width: 50cqw } }` computing 50% of
       the VIEWPORT for a card inside a 400-pixel container — a number that is plausible, never crashes, and
       is wrong by whatever ratio the container bears to the viewport.
   AND THE SMALL VIEWPORT SIZE IS THIS RECTANGLE, for the reason the `sv*` arm below rests on and stated once
   there rather than twice. */
static bool mq_container_extent(const char *u, const MqEnv *e, double *extent)
{
    double inl = mq_axis_size(MQ_AXIS_INLINE, e), blk = mq_axis_size(MQ_AXIS_BLOCK, e);

    if (!strcmp(u, "cqw")) { *extent = e->width;  return true; }   /* §7: "1% of a query container's width"  */
    if (!strcmp(u, "cqh")) { *extent = e->height; return true; }   /* §7: "1% of a query container's height" */
    if (!strcmp(u, "cqi")) { *extent = inl; return true; }         /* §7: "... inline size" */
    if (!strcmp(u, "cqb")) { *extent = blk; return true; }         /* §7: "... block size"  */
    /* §7's own table: `cqmin` is "The smaller value of cqi or cqb" and `cqmax` "The larger value of cqi or
       cqb" — the INLINE and BLOCK pair, which is the pairing §6.1.2.2 does NOT use for `vmin`/`vmax`. */
    if (!strcmp(u, "cqmin")) { *extent = inl < blk ? inl : blk; return true; }
    if (!strcmp(u, "cqmax")) { *extent = inl > blk ? inl : blk; return true; }
    return false;
}

/* A `<length>` in CSS pixels — css-values-4 §6's three families and CSS Conditional 5 §7's, over the modelled
   environment above.
   THIS IS AN ABSOLUTIZATION TABLE AND `css_length_is_length_unit` IS §6's `<length>` PRODUCTION. They answer
   different questions and the production is necessarily the wider one: it admits every unit any specification
   defines as a length precisely so that one this engine cannot absolutize is a MISSING COMPONENT rather than a
   syntax error. So the two sets part again the day a specification defines a new length unit, and a census of
   today's difference written here would be wrong the moment somebody drained it. WHAT THE DIFFERENCE IS IS
   DERIVED, and one command lists each side. `grep -o 'strcmp(u, "[a-z]*")' core/css/media_query.c` is THIS
   side — the rows of this function and of the two family helpers above, of which `mq_viewport_extent`'s six
   each stand for four spellings under §6.1.2.1's variant letter. `css_len_unit_known`'s four tables in
   core/css/css_length.c are the other — `CSS_ABSOLUTE`, `CSS_FONT_RELATIVE`, `CSS_VIEWPORT_RELATIVE` under
   that same variant-letter rule, and `CSS_CONTAINER_RELATIVE`. Count with `grep -o`, never with `grep -c`,
   which counts LINES and answers a smaller number than there are rows.
   WHAT REACHES `*ok = false` IS THEREFORE NOT A LENGTH AT ALL: `value_ok` admits every DIMENSION for a
   `<length>` feature, so `(min-width: 3deg)` arrives here and leaves through that arm, which `eval_feature`
   turns into MQ_FALSE. A unit that IS a `<length>` and has no row is a missing component and is the crash
   core/html/image_source_set.c states, in the one place a `sizes` attribute can distinguish the two.
   §6.1.1'S OWN LAST CLAUSE IS WHY THE FONT-RELATIVE UNITS RESOLVE AGAINST INITIAL VALUES and not computed
   ones: a media query is evaluated outside the context of an element, so there is no element whose `font-size`
   an `em` could be — "when used outside the context of an element (such as in media queries), the font-relative
   lengths units refer to the metrics corresponding to the initial values of the font and line-height
   properties". The `r`-prefixed twins are the same numbers by the SENTENCE THAT FOLLOWS that one, rather than
   by an entailment this file draws for itself: "Similarly, when specified in a document with no root element,
   the root font-relative lengths are resolved assuming the initial values of the font and line-height
   properties."
   THE METRIC RATIOS ARE READ FROM core/css/font_metrics.h AND ARE NOT WRITTEN HERE. They used to be two
   literal halves with §6.1.1's two sentences quoted beside them, which is the same fact answered from two
   places that css_length.h opens by naming and that core/css/font_size_functions.h was created to end for the
   default font size — and it is the shape this file's own next paragraph complains about one level up ("the
   copy is what would go on being wrong"). The initial `writing-mode` is `horizontal-tb`, which css-writing-
   modes-4 §5.1 "Orienting Text: the text-orientation property" makes a horizontal typographic mode — the
   property "has no effect in horizontal typographic modes" — so both advance measures below are taken in the
   HORIZONTAL direction and no orientation has to be resolved to know it.
   THIS TABLE ANSWERS THE VIEWPORT VARIANTS AND THE CONTAINER UNITS WHERE core/css/css_length.c's
   `css_len_unit_px` CRASHES FOR THEM, AND THAT IS NOT ONE OF THE TWO BEING BEHIND — the two seams differ in
   what the number they produce CARRIES. Here the answer is an EXAMPLE consumed by a C `if`, and the fork is
   minted below by `mq_source`, which keys a query list's answer on its OWN SERIALIZATION — the unit text
   included, because `out_value` writes a dimension's unit into it. So `(min-height: 100dvh)` and
   `(min-height: 100lvh)` are two source keys, two predicates and two independent forks, and a flow that pinned
   one has said nothing that could decide the other: the arm where the dynamic and large viewport sizes differ
   survives answering both from one rectangle. In the COMPUTED-VALUE chain it does not — a length crosses to the
   page there through `viewport_env_derived`'s JOINT over `CssEnvFact`s, where all four spellings would carry
   the one ICB fact — and that is what that file's own crash is about and why it must be answered THERE, by
   giving §6.1.2.1's three viewport sizes their own picked facts, rather than by copying these rows across.
   THE TABLE IS SPLIT FROM THE `MqValue` WALK because it has a SECOND caller with no MqValue to hand it, and
   that caller's rule is HTML §4.8.4.3 "Processing model" in one sentence: a source size's units other than the
   viewport-relative ones "must be interpreted THE SAME AS IN MEDIA QUERIES". So `media_query_length_px` below
   is this same table asked from outside, and not a copy of it — the copy is what would go on being wrong. */
static double mq_unit_px(const char *u, double n, const MqEnv *e, bool *ok)
{
    double extent = 0.0;

    *ok = true;
    if (!strcmp(u, "px")) return n;
    if (!strcmp(u, "em") || !strcmp(u, "rem")) return n * e->font_size;
    if (!strcmp(u, "ex") || !strcmp(u, "rex"))
        return n * e->font_size * font_metrics_x_height_em();
    /* §6.1.1's `ch`/`ic` ENTRY rather than the general advance measure: outside the context of an element the
       units still mean what the section says they mean, including its assumed values for a face that cannot
       supply the glyph. The direction is horizontal because there is no element to read `writing-mode` off, so
       its initial value applies: css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property"
       gives `Initial: horizontal-tb`, whose typographic mode is horizontal, and §6.1.1's advance measure is
       then the glyph's advance WIDTH. */
    if (!strcmp(u, "ch") || !strcmp(u, "rch"))
        return n * e->font_size *
               font_metrics_typical_advance_measure_em(0x0030, FONT_METRICS_ADVANCE_HORIZONTAL);
    if (!strcmp(u, "ic") || !strcmp(u, "ric"))
        return n * e->font_size *
               font_metrics_typical_advance_measure_em(0x6C34, FONT_METRICS_ADVANCE_HORIZONTAL);
    if (!strcmp(u, "cap") || !strcmp(u, "rcap")) return n * e->ascent;
    if (!strcmp(u, "cm")) return n * 96.0 / 2.54;
    if (!strcmp(u, "mm")) return n * 96.0 / 25.4;
    if (!strcmp(u, "q"))  return n * 96.0 / 101.6;
    if (!strcmp(u, "in")) return n * 96.0;
    if (!strcmp(u, "pt")) return n * 96.0 / 72.0;
    if (!strcmp(u, "pc")) return n * 16.0;
    /* §6.1.1's twelfth pair, on the base the clause above resolves for it — `line-height: normal` at the
       initial `font-size`, which `mq_env` reads from the one component that owns CSS 2.1 §10.8.1's `AD`. */
    if (!strcmp(u, "lh") || !strcmp(u, "rlh")) return n * e->line_height;
    /* §6.1.2.1's FOUR VARIANTS, resolved by stripping the one letter that names WHICH of its three viewport
       sizes the unit is a percentage of. The strip is the spec's own naming rule and not a coincidence of
       spelling: §6.1.2.1 introduces the `lv*`, `sv*` and `dv*` prefixes as the large, small and dynamic
       families of the same six names, and §6.1.2.2 then writes every definition once over a `*` standing for
       all four. Tabulating them a second time is what would let one spelling fall out of step.
       ALL FOUR ANSWER THE ONE MODELLED RECTANGLE, AND THAT IS §6.1.2.1's OWN ARM RATHER THAN A ROUNDING OF IT.
       It opens by saying there are "four variants of the viewport-percentage length units, corresponding to
       three (possibly identical) notions of the viewport size", and it then defines the three by what they
       assume about ONE thing: the large size assumes "any UA interfaces that are dynamically expanded and
       retracted to be retracted", the small size assumes them "to be expanded", and the dynamic size is "sized
       with dynamic consideration of" them. core/frame/viewport.h models one viewport and NO dynamically
       expanded and retracted interface, so the three antecedents are satisfied by the same rectangle and
       "possibly identical" is the case this engine is in. It is also what CSS Conditional 5 §7's fallback
       above asks for by name — §7 wants "the small viewport size for that axis", which is this rectangle by
       this same paragraph, so the coincidence is stated here once and read there rather than argued twice.
       WHAT MAKES THIS SAFE FOR THE SEARCH rather than a collapsed fork is the source key, which is the
       serialized query and not the viewport: see the note above this function. */
    if ((u[0] == 's' || u[0] == 'l' || u[0] == 'd') && mq_viewport_extent(u + 1, e, &extent))
        return n * extent / 100.0;
    if (mq_viewport_extent(u, e, &extent)) return n * extent / 100.0;
    if (mq_container_extent(u, e, &extent)) return n * extent / 100.0;
    *ok = false;
    return 0;
}

static double length_px(const MqValue *v, const MqEnv *e, bool *ok)
{
    *ok = true;
    if (v->kind == MV_NUMBER) return v->a;          /* a unitless zero */
    return mq_unit_px(v->unit, v->a, e, ok);
}

bool media_query_length_px(JSContext *ctx, double n, const char *unit, size_t unit_len, double *px)
{
    char u[MQ_IDENT_MAX];
    MqEnv e;
    size_t i;
    bool ok = false;

    DCHECK(px != NULL, "a `<length>` was resolved against the media query unit table with nowhere to put it");
    DCHECK(unit != NULL || unit_len == 0,
           "a `<length>`'s unit was given as no bytes with a non-zero length — a dimension token's unit is a "
           "span of its own source and an absent one is the UNITLESS case, which is a length of length zero");
    /* css-values-4 §6: the unit may be omitted for ZERO and for nothing else, which is the whole of what a bare
       `<number>` can be where a `<length>` is wanted. Answered here rather than by the table, which is a table
       of UNITS and has no row for the absence of one. */
    if (unit_len == 0) { *px = n; return n == 0.0; }
    if (unit_len >= sizeof u) return false;         /* longer than any unit §6 defines; not one of them */
    for (i = 0; i < unit_len; i++) u[i] = mq_lower(unit[i]);
    u[unit_len] = '\0';
    mq_env(ctx, &e);
    *px = mq_unit_px(u, n, &e, &ok);
    return ok;
}

MediaQuerySet *media_query_parse_condition(const char *text)
{
    MediaQuerySet *set;
    MqCond *cond = NULL;
    MqLex L;

    DCHECK(text != NULL, "a `<media-condition>` was parsed from no text — an absent condition is a caller that "
                         "never had one, and the EMPTY string is a condition that does not match the grammar");
    mq_lex_init(&L, text);
    /* §3's `<media-condition>`, with `or` allowed — the production HTML §4.8.4.3.11 step 3.5 names, which is
       NOT `<media-query>`: a bare `<media-type>` (`screen`) is a valid media query and is not a condition, so
       parsing this as a query would accept a sizes entry no browser accepts. It must consume the WHOLE text;
       a trailing token is a condition with something after it, which is not one. */
    if (!parse_condition(&L, true, &cond) || L.kind != TK_EOF) {
        cond_free(cond);
        return NULL;
    }
    set = calloc(1, sizeof *set);
    CHECK(set != NULL, "media queries: OOM holding a parsed media condition");
    set->q = calloc(1, sizeof *set->q);
    CHECK(set->q != NULL, "media queries: OOM holding a parsed media condition's query");
    set->n = 1;
    /* A bare `<media-condition>` IS §3's first arm of `<media-query>`, so it is held as a query with no
       `<media-type>` — which is exactly the shape parse_query produces for one, and is what makes
       `media_query_matches` and its concolic-carrying twin answer this without a second evaluator. */
    set->q[0].cond = cond;
    return set;
}

/* §4's `resolution` operand as the number `e->dppx` is measured in. The four unit identifiers and the two
   ratios are CSS Values §7.4's, answered by the component that owns them — this file used to carry its own
   copy of the same list, which is one fact in two places and is what disagrees about `x`. */
static double resolution_dppx(const MqValue *v, bool *ok)
{
    double dppx = 0.0;

    *ok = css_resolution_dppx(v->unit, strlen(v->unit), v->a, &dppx);
    return dppx;
}

/* The feature's own value, and the comparison operand, both as one comparable number — which is what makes
   `aspect-ratio` work: a ratio compares by CROSS MULTIPLICATION, so both sides are divided once here rather
   than compared as fractions at four call sites. */
static bool feature_numbers(const MfDef *d, const MqEnv *e, const MqValue *v, double *pactual, double *pwanted)
{
    bool ok = true;

    switch (d->kind) {
    case MFK_LENGTH:
        *pactual = !strcmp(d->name, "width") ? e->width
                 : !strcmp(d->name, "height") ? e->height
                 : !strcmp(d->name, "device-width") ? e->dev_width : e->dev_height;
        *pwanted = length_px(v, e, &ok);
        return ok;
    case MFK_RATIO:
        *pactual = !strcmp(d->name, "aspect-ratio") ? e->width / e->height : e->dev_width / e->dev_height;
        *pwanted = v->kind == MV_RATIO ? (v->b == 0 ? INFINITY : v->a / v->b) : v->a;
        return true;
    case MFK_RESOLUTION:
        *pactual = e->dppx;
        *pwanted = resolution_dppx(v, &ok);
        return ok;
    case MFK_INTEGER:
        *pactual = !strcmp(d->name, "color") ? e->color_bits
                 : !strcmp(d->name, "color-index") ? 0
                 : !strcmp(d->name, "monochrome") ? 0
                 : !strcmp(d->name, "grid") ? 0 : 1;   /* the two viewport-segment counts */
        *pwanted = v->a;
        return true;
    default:
        DFAIL("a discrete media feature reached the numeric comparison");
        return false;
    }
}

/* §4.6's `color-gamut` is discrete but ORDERED — a value matches when the device supports that gamut OR MORE —
   so it is the one discrete feature whose comparison is not string equality. Stating it here rather than
   giving every entry a comparator keeps the table what it is: the UA's answers. */
static int gamut_rank(const char *s)
{
    if (!strcmp(s, "srgb")) return 0;
    if (!strcmp(s, "p3")) return 1;
    DCHECK(!strcmp(s, "rec2020"), "an unlisted color-gamut value survived the feature table's own legal set");
    return 2;
}

static int eval_feature(const MqFeature *f, const MqEnv *e)
{
    const MfDef *d;
    double actual, wanted;

    if (f->unknown) return MQ_UNKNOWN;
    d = mf_lookup(f->name);
    DCHECK(d != NULL, "a parsed media feature names something the feature table does not have — the parser "
                      "admits a name only after looking it up in that same table");
    if (d->kind == MFK_DISCRETE) {
        const char *ua = mf_discrete_ua(d, e);

        if (f->boolean) return d->bool_ctx ? MQ_TRUE : MQ_FALSE;
        DCHECK(f->eq.kind == MV_IDENT,
               "a discrete media feature reached evaluation with something other than an identifier — the "
               "parser rejects a range comparison and a non-ident value on one, so nothing else can arrive");
        if (!strcmp(d->name, "color-gamut"))
            return gamut_rank(ua) >= gamut_rank(f->eq.ident) ? MQ_TRUE : MQ_FALSE;
        return !strcmp(ua, f->eq.ident) ? MQ_TRUE : MQ_FALSE;
    }
    if (f->boolean) {
        MqValue zero = { MV_NUMBER, 0, 1, "", "" };

        if (!feature_numbers(d, e, &zero, &actual, &wanted)) return MQ_FALSE;
        return actual != 0 ? MQ_TRUE : MQ_FALSE;   /* §2.4.3: true unless the value is zero */
    }
    if (f->eq.kind != MV_ABSENT) {
        if (!feature_numbers(d, e, &f->eq, &actual, &wanted)) return MQ_FALSE;
        return actual == wanted ? MQ_TRUE : MQ_FALSE;
    }
    if (f->lo_cmp) {
        if (!feature_numbers(d, e, &f->lo, &actual, &wanted)) return MQ_FALSE;
        if (f->lo_cmp == MQ_CMP_GT ? !(actual > wanted) : !(actual >= wanted)) return MQ_FALSE;
    }
    if (f->hi_cmp) {
        if (!feature_numbers(d, e, &f->hi, &actual, &wanted)) return MQ_FALSE;
        if (f->hi_cmp == MQ_CMP_LT ? !(actual < wanted) : !(actual <= wanted)) return MQ_FALSE;
    }
    DCHECK(f->lo_cmp || f->hi_cmp,
           "a media feature reached evaluation with no boolean form, no value and no bound — the parser "
           "produces one of those three or it produces a syntax error");
    return MQ_TRUE;
}

static int eval_cond(const MqCond *c, const MqEnv *e)
{
    int r, i;

    switch (c->op) {
    case MQC_FEATURE: return eval_feature(&c->feat, e);
    case MQC_NOT: return mq_not(eval_cond(c->kids[0], e));
    case MQC_AND:
        r = MQ_TRUE;
        for (i = 0; i < c->nkids; i++) r = mq_and(r, eval_cond(c->kids[i], e));
        return r;
    case MQC_OR:
        r = MQ_FALSE;
        for (i = 0; i < c->nkids; i++) r = mq_or(r, eval_cond(c->kids[i], e));
        return r;
    }
    DFAIL("a media condition with no operator reached evaluation");
    return MQ_FALSE;
}

/* §2.2: this user agent's media type. It renders a document for a screen — every geometric answer it gives is
   a viewport's — so `screen` is what it IS, and `print` is a different rendering it does not perform. `all`
   matches every type by definition. The types §2.2 deprecates (`tty`, `tv`, `projection`, `handheld`,
   `braille`, `embossed`, `aural`, `speech`) "must never match", which is the same answer an unknown type gets
   and is why there is no list of them here. */
static bool type_matches(const char *type)
{
    return !strcmp(type, "all") || !strcmp(type, "screen");
}

static bool query_matches(const MqQuery *q, const MqEnv *e)
{
    int r;

    if (q->invalid) return false;                         /* `not all` */
    r = q->type[0] ? (type_matches(q->type) ? MQ_TRUE : MQ_FALSE) : MQ_TRUE;
    if (q->cond) r = mq_and(r, eval_cond(q->cond, e));
    if (q->qualifier == 1) r = mq_not(r);                 /* `only` is not an operator: §2.2 makes it a no-op */
    return r == MQ_TRUE;                                  /* §3.1: unknown does not match */
}

bool media_query_matches(JSContext *ctx, const MediaQuerySet *set)
{
    MqEnv e;
    int i;

    DCHECK(set != NULL, "a media query list was evaluated after it was freed");
    mq_env(ctx, &e);
    if (set->n == 0) return true;                         /* the empty list matches everything */
    for (i = 0; i < set->n; i++)
        if (query_matches(&set->q[i], &e)) return true;
    return false;
}

/* ---- the CSSOM-facing answer — see media_query.h for why it is spelled HERE ---------------------------------- */

/* THE SOURCE IDENTITY of one document's answer to one query list. The DOCUMENT is part of it and that is not
   decoration: a child navigable's viewport is 300 CSS pixels wide and the top-level traversable's is 1280, so
   `(min-width: 600px)` is genuinely a different question in each — one key would let a branch taken in the
   parent decide the iframe's. The SHAPE is the human-readable half a finding carries, so it names the query and
   not the document id. */
static void mq_source(JSContext *ctx, const MediaQuerySet *set, char *shape, size_t nshape,
                      char *src, size_t nsrc)
{
    JSValueConst self = document_window_proxy(ctx);
    char *text = media_query_serialize(set);

    DCHECK(window_proxy_is(self), "a media query was reported in a realm whose document has no WindowProxy");
    CHECK(text != NULL, "media queries: OOM keying a media query's answer on its own serialization");
    snprintf(shape, nshape, "{media:%s}", text);
    snprintf(src, nsrc, "{media#%u}%s", (unsigned)window_proxy_doc(self), text);
    free(text);
}

JSValue media_query_matches_value(JSContext *ctx, const MediaQuerySet *set)
{
    char shape[256], src[256];

    DCHECK(set != NULL, "a media query list's CSSOM answer was asked for after it was freed");
    mq_source(ctx, set, shape, sizeof shape, src, sizeof src);
    /* concolic_source_wrap hands back the plain boolean where no source overlay is installed (a conformance
       host), which is what keeps this component testable against the standard. */
    return concolic_source_wrap(ctx, shape, src, JS_NewBool(ctx, media_query_matches(ctx, set)));
}

bool media_query_matches_now(JSContext *ctx, const MediaQuerySet *set)
{
    JSValue v = media_query_matches_value(ctx, set);
    int arm;
    bool r;

    if (!concolic_is(v)) {
        r = JS_ToBool(ctx, v) != 0;
        JS_FreeValue(ctx, v);
        return r;
    }
    /* THE ENGINE's OWN READ CONCRETIZES ON THE FLOW's PIN — asked BY VALUE so decide.c stays the only speller
       of the constraint key. -1 is "this flow has committed to neither arm", and the modelled example is then
       the answer, exactly as core/html/page_visibility.c does for `hidden`. */
    arm = decide_value_arm(v);
    if (arm >= 0) {
        JS_FreeValue(ctx, v);
        return arm == 1;
    }
    {
        JSValue ex = concolic_example(ctx, v);

        r = JS_ToBool(ctx, ex) != 0;
        JS_FreeValue(ctx, ex);
    }
    JS_FreeValue(ctx, v);
    return r;
}
