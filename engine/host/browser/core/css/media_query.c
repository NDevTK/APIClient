/* MEDIA QUERIES LEVEL 4 — the grammar of §3, the features of §4. See media_query.h for why the evaluation is a
   plain bool, why the logic is three-valued, and why nothing here throws. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/css/media_query.h"
#include "core/frame/screen.h"
#include "core/frame/viewport.h"

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
    const char *p;      /* the cursor, at the START of `kind` */
    const char *next;   /* just past `kind` */
    int         kind;
    double      num;
    char        ident[MQ_IDENT_MAX];   /* TK_IDENT: lowercased; TK_DIM: the unit, lowercased */
} MqLex;

static bool mq_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static bool mq_is_digit(char c) { return c >= '0' && c <= '9'; }
static bool mq_is_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || (unsigned char)c >= 0x80;
}
static bool mq_is_name(char c) { return mq_is_name_start(c) || mq_is_digit(c); }
static char mq_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static void mq_scan(MqLex *L)
{
    const char *p = L->next;
    size_t i;

    while (mq_is_space(*p)) p++;
    L->p = p;
    L->ident[0] = 0;
    L->num = 0;
    if (!*p) { L->kind = TK_EOF; L->next = p; return; }
    if (mq_is_name_start(*p) && !(*p == '-' && mq_is_digit(p[1]))) {
        for (i = 0; mq_is_name(p[i]); i++)
            if (i + 1 < sizeof(L->ident)) L->ident[i] = mq_lower(p[i]);
        L->ident[i < sizeof(L->ident) ? i : sizeof(L->ident) - 1] = 0;
        L->kind = TK_IDENT;
        L->next = p + i;
        return;
    }
    if (mq_is_digit(*p) || ((*p == '-' || *p == '+' || *p == '.') && (mq_is_digit(p[1]) ||
                            (p[1] == '.' && mq_is_digit(p[2]))))) {
        char *end = NULL;
        L->num = strtod(p, &end);
        p = end;
        if (mq_is_name_start(*p)) {                       /* a `<dimension>`: the unit rides the number */
            for (i = 0; mq_is_name(p[i]); i++)
                if (i + 1 < sizeof(L->ident)) L->ident[i] = mq_lower(p[i]);
            L->ident[i < sizeof(L->ident) ? i : sizeof(L->ident) - 1] = 0;
            L->kind = TK_DIM;
            L->next = p + i;
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

char *media_query_serialize(const MediaQuerySet *set)
{
    MqOut o = { NULL, 0, 0 };
    int i;

    out_put(&o, "");
    for (i = 0; i < set->n; i++) {
        const MqQuery *q = &set->q[i];

        if (i) out_put(&o, ", ");
        if (q->invalid) { out_put(&o, "not all"); continue; }
        if (q->qualifier) out_put(&o, q->qualifier == 1 ? "not " : "only ");
        if (q->type[0]) {
            out_put(&o, q->type);
            if (q->cond) { out_put(&o, " and "); out_cond(&o, q->cond); }
        } else {
            DCHECK(q->cond != NULL, "a media query with neither a type nor a condition survived the parser");
            out_cond(&o, q->cond);
        }
    }
    return o.s;
}

/* ---- evaluation — MQ4 §3 and §4 ------------------------------------------------------------------------------ */

/* A `<length>` in CSS pixels. §4: font-relative units resolve against the INITIAL value of `font-size`, because
   a media query is evaluated before any element exists to have one; viewport units resolve against the
   viewport, which is what this whole file is about. An unknown unit cannot reach here — value_ok admitted the
   dimension and this is the same list, which is what the DFAIL states. */
#define MQ_INITIAL_FONT_SIZE 16.0

static double length_px(const MqValue *v, const MqEnv *e, bool *ok)
{
    const char *u = v->unit;

    *ok = true;
    if (v->kind == MV_NUMBER) return v->a;          /* a unitless zero */
    if (!strcmp(u, "px")) return v->a;
    if (!strcmp(u, "em") || !strcmp(u, "rem")) return v->a * MQ_INITIAL_FONT_SIZE;
    if (!strcmp(u, "ex")) return v->a * MQ_INITIAL_FONT_SIZE * 0.5;
    if (!strcmp(u, "ch")) return v->a * MQ_INITIAL_FONT_SIZE * 0.5;
    if (!strcmp(u, "cm")) return v->a * 96.0 / 2.54;
    if (!strcmp(u, "mm")) return v->a * 96.0 / 25.4;
    if (!strcmp(u, "q"))  return v->a * 96.0 / 101.6;
    if (!strcmp(u, "in")) return v->a * 96.0;
    if (!strcmp(u, "pt")) return v->a * 96.0 / 72.0;
    if (!strcmp(u, "pc")) return v->a * 16.0;
    if (!strcmp(u, "vw")) return v->a * e->width / 100.0;
    if (!strcmp(u, "vh")) return v->a * e->height / 100.0;
    if (!strcmp(u, "vmin")) return v->a * (e->width < e->height ? e->width : e->height) / 100.0;
    if (!strcmp(u, "vmax")) return v->a * (e->width > e->height ? e->width : e->height) / 100.0;
    *ok = false;
    return 0;
}

static double resolution_dppx(const MqValue *v, bool *ok)
{
    const char *u = v->unit;

    *ok = true;
    if (!strcmp(u, "dppx") || !strcmp(u, "x")) return v->a;
    if (!strcmp(u, "dpi")) return v->a / 96.0;
    if (!strcmp(u, "dpcm")) return v->a * 2.54 / 96.0;
    *ok = false;
    return 0;
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
