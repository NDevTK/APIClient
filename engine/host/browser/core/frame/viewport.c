/* THE VIEWPORT — CSS 2.1 §9.1.1 / §10.1, and CSSOM VIEW §4's Window extensions over it. See viewport.h for why
   this is a modelled UA choice, why it is a component of its own, why it is answered per realm, and why §4's
   members are installed here rather than in window.c. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/dom/document.h"
#include "core/dom/element_scrolling.h"
#include "core/dom/perform_scroll.h"
#include "core/frame/screen.h"
#include "core/css/font_metrics.h"
#include "core/css/font_size_functions.h"
#include "core/frame/viewport.h"
#include "core/frame/window_proxy.h"
#include "core/layout/layout_job.h"
#include "core/layout/used_value.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "solver/concolic.h"
#include <lexbor/core/base.h>   /* lexbor_malloc: the allocator the engine gives its refusal edge */

/* THE TOP-LEVEL TRAVERSABLE's viewport. A UA answering the question CSS hands it, exactly as rendering.c
   answers the refresh rate: 1280 x 720 is an ordinary desktop content area, and it is deliberately NOT
   screen.c's 1920 x 1080 — a window is not a screen, and a bundle that compares `innerWidth` against
   `screen.width` is asking a real question whose two sides must be able to differ. */
#define VIEWPORT_TOP_WIDTH   1280.0
#define VIEWPORT_TOP_HEIGHT   720.0

/* A CHILD NAVIGABLE's viewport is its container's CONTENT BOX, and for an `iframe` with no author size that
   box is CSS 2.1 §10.3.2's default for a replaced element with no natural dimensions: 300 x 150. That is the
   spec's own number rather than a second UA choice, which is why it is stated as a derivation and not as a
   preference — and it is no longer stated HERE. core/layout/used_value.h's `used_value_default_replaced_size`
   is the one place §10.3.2's and §10.6.2's sentences are read, including the cap against the device that
   neither this file nor a constant could express, and the two numbers that used to sit here as `#define`s were
   a second copy of exactly the fact that component owns. HTML §15.4.1 "Embedded content" is what makes the
   frame replaced at all ("the embed, iframe, and video elements are expected to be treated as replaced
   elements") and css-images-3 §4.1 "Object-Sizing Terminology" is what gives it no natural dimensions
   ("embedded documents, such as the iframe element in HTML" are "an example of an object with no natural
   dimensions at all"); core/layout/replaced_element.c holds both citations.
   AN AUTHOR STYLE THAT RESIZES THE FRAME IS NOT MODELLED, and NAMING WHAT IS MISSING IS THE WHOLE POINT OF
   THIS PARAGRAPH: it used to say `iframe { width: 500px }` "is a cascade this engine has no CSS parser for, so
   the used width it produces does not exist to be read", and that stopped being true. There IS a cascade —
   core/css/css_style_declaration.c resolves an author declaration out of the style sheet OBJECTS §6.2's list
   holds — and there IS a used value over it: core/layout/used_value.h's `used_value_px(el, "width")`, which
   for a `width: auto` iframe now runs §10.3.2 and lands on this very default.
   THE REACH IS NO LONGER MISSING, and this paragraph used to say it was: `window_proxy_container` answers
   §7.3.1.3 "Child navigables"' container of a navigable, recorded by create-a-new-child-navigable and
   confirmed against the element's own content navigable, so it is per-flow with nothing to capture. HTML
   §15.3.2 "The page" states the consequence this file wants in one sentence — "if a Document's node navigable
   is a child navigable, then it is expected to be positioned and sized to fit inside the CONTENT BOX of the
   container of that navigable" — so the number is `used_value_content_px(container, vertical)` and the walk
   to it is `window_proxy_container(ctx, document_window_proxy(ctx))` -> `node_of`.
   TWO THINGS STAND BETWEEN THAT AND THIS ENTRY, and neither is the reach. (1) THE ENVIRONMENT FACT CHANGES
   KIND. A top-level traversable's viewport is PICKED, which is what makes CSS_ENV_ICB_WIDTH a row in the seam
   below; a CHILD's would be DERIVED from its container's used size, which bottoms out in the parent's own ICB
   for a percentage-sized frame and in nothing at all for `iframe { width: 500px }`. viewport.h's test then
   says a child ICB is not a picked fact and must carry its container's facts WHOLE rather than mint a key of
   its own — while `innerWidth` in that child still crosses through the MEMBER seam, which mints per document.
   Those two answers have to be made one deliberately, and `viewport_width` returning a bare `double` is where
   the choice is currently hidden rather than made. (2) THE CROSS-ORIGIN ARM. A container in a peer instance
   has no wrapper in this heap, so the read is CLAUDE.md §Security's suspend-at-the-boundary — the same
   primitive core/css/css_presentational_hints.c crashes for when §15.3.2's third margin source needs the same
   element's attribute, and the same one §15.3.2's own "if the container is not being rendered, the navigable
   is expected to have a viewport with zero width and zero height" would be asked through. */

/* A modelled non-HiDPI display. See viewport.h. */
#define VIEWPORT_DPPX           1.0

/* THE CLIENT WINDOW — the browser window the top-level traversable is presented in, which is what CSSOM VIEW
   §4's `outerWidth`, `outerHeight`, `screenX` and `screenY` are all about. It is NOT the viewport: the viewport
   is the content area INSIDE it, and the difference is the UA's own chrome (a tab strip and a toolbar). A
   bundle that computes `outerHeight - innerHeight` is measuring exactly that, so the two must be able to
   differ; a client window equal to its viewport is the shrug this component exists not to make.
   THE SIZE IS DERIVED FROM THE VIEWPORT, in that direction, because the viewport is the load-bearing fact this
   file already states. There is no horizontal chrome on a desktop browser window, so only the height grows.
   IT IS THE SAME WINDOW FOR EVERY REALM OF THE PAGE. A child navigable has its own viewport and does NOT have
   its own client window — `iframe.contentWindow.screenX` is the position of the browser window, which is why
   these four take a realm only to be asked whether the document is being presented at all. */
#define VIEWPORT_CHROME_HEIGHT 74.0

/* IS THIS REALM's document IN the top-level traversable? Asked of the NAVIGABLE tree rather than remembered at
   install, because a realm is built before it is attached to anything and the answer is a fact about where the
   document sits. `window_proxy_top_navigable` answers the top-level traversable's proxy in every case
   INCLUDING the caller's own (window_proxy.h's note: `top` would answer the Window there, and a walk handed
   that reaches something that is not a proxy). */
static bool viewport_is_top(JSContext *ctx)
{
    JSValueConst self = document_window_proxy(ctx);
    JSValue top;
    bool is_top;

    DCHECK(window_proxy_is(self),
           "the viewport was asked for a realm whose document has no WindowProxy — every Document this agent "
           "holds is the active document of a navigable, and the navigable is what a viewport belongs to");
    top = window_proxy_top_navigable(ctx, self);
    DCHECK(window_proxy_is(top),
           "a navigable answered its top-level traversable with something that is not a WindowProxy");
    is_top = window_proxy_doc(top) == window_proxy_doc(self);
    JS_FreeValue(ctx, top);
    return is_top;
}

bool viewport_exists(JSContext *ctx)
{
    return document_fully_active(ctx);
}

double viewport_width(JSContext *ctx)
{
    return viewport_is_top(ctx) ? VIEWPORT_TOP_WIDTH : used_value_default_replaced_size(false).px;
}

double viewport_height(JSContext *ctx)
{
    return viewport_is_top(ctx) ? VIEWPORT_TOP_HEIGHT : used_value_default_replaced_size(true).px;
}

double viewport_device_pixel_ratio(JSContext *ctx)
{
    (void)ctx;
    return VIEWPORT_DPPX;
}

double viewport_env_fact_value(JSContext *realm, CssEnvFact fact)
{
    DCHECK(realm != NULL, "an environment fact's value was asked of no realm");
    switch (fact) {
    case CSS_ENV_ICB_WIDTH:
    case CSS_ENV_SMALL_VIEWPORT_WIDTH:
    case CSS_ENV_DYNAMIC_VIEWPORT_WIDTH:  return viewport_width(realm);
    case CSS_ENV_ICB_HEIGHT:
    case CSS_ENV_SMALL_VIEWPORT_HEIGHT:
    case CSS_ENV_DYNAMIC_VIEWPORT_HEIGHT: return viewport_height(realm);
    case CSS_ENV_DEVICE_PIXEL_RATIO:      return viewport_device_pixel_ratio(realm);
    case CSS_ENV_DEFAULT_FONT_SIZE:       return css_default_font_size_px();
    case CSS_ENV_DEFAULT_FIXED_FONT_SIZE: return css_default_fixed_font_size_px();
    default: {
        InstalledFont face;
        FontMetricsFaceFact which;

        if (font_metrics_fact_face(fact, &face, &which))
            return which == FONT_METRICS_FACE_DESCENT  ? font_metrics_descent_em(face)
                 : which == FONT_METRICS_FACE_LINE_GAP ? font_metrics_line_gap_em(face)
                                                       : font_metrics_ascent_em(face);
        DFAILF("environment fact %d has no value source", (int)fact);
        return 0.0;
    }
    }
}

/* CSS 2.1 §10.1: the initial containing block "has the dimensions of the viewport". */
CssPx viewport_icb_width(JSContext *ctx)
{
    return css_px_fact(CSS_ENV_ICB_WIDTH, ctx);
}

CssPx viewport_icb_height(JSContext *ctx)
{
    return css_px_fact(CSS_ENV_ICB_HEIGHT, ctx);
}

/* css-values-4 §6.1.2.1: the SMALL viewport size is "the viewport sized assuming any UA interfaces that are
   dynamically expanded and retracted to be expanded", and the DYNAMIC one is "sized with dynamic consideration
   of" them. THIS MODEL HAS NO SUCH INTERFACE, so both antecedents are satisfied by the one viewport and each
   answers its number — which is §6.1.2.1's own "possibly identical" arm and not a rounding of it. What is NOT
   shared is the FACT each carries, so a page comparing two families forks rather than being answered. */
CssPx viewport_small_width(JSContext *ctx)
{
    return css_px_fact(CSS_ENV_SMALL_VIEWPORT_WIDTH, ctx);
}

CssPx viewport_small_height(JSContext *ctx)
{
    return css_px_fact(CSS_ENV_SMALL_VIEWPORT_HEIGHT, ctx);
}

CssPx viewport_dynamic_width(JSContext *ctx)
{
    return css_px_fact(CSS_ENV_DYNAMIC_VIEWPORT_WIDTH, ctx);
}

CssPx viewport_dynamic_height(JSContext *ctx)
{
    return css_px_fact(CSS_ENV_DYNAMIC_VIEWPORT_HEIGHT, ctx);
}

bool viewport_canvas_region(JSContext *ctx, CssPx out[4])
{
    DCHECK(out != NULL, "CSS 2.1 §2.3.1 \"The canvas\"'s rendered region was asked for through no rectangle");
    if (!viewport_exists(ctx)) return false;
    out[0] = css_px(0.0);
    out[1] = css_px(0.0);
    out[2] = viewport_icb_width(ctx);
    out[3] = viewport_icb_height(ctx);
    return true;
}

/* ONE ROW PER FACT, INDEXED BY THE FACT ITSELF — the one table over `CssEnvFact`, so a length mints its domain
   in one place whether it is a function of one fact or of three.
   THE SECOND COLUMN IS §4's "OR ZERO IF THERE IS NO VIEWPORT", asked per FACT because the facts differ on it
   and js_vp_get below already draws the same line at the same place: the INITIAL CONTAINING BLOCK has the
   dimensions of the viewport, so a document no navigable presents has none — while `devicePixelRatio`'s
   algorithm asks about the OUTPUT DEVICE, which exists whether or not this document is on it. One condition
   for both would either crash for a border width on a DOMParser element or wave through a length derived from
   a rectangle that does not exist. */
static const struct { const char *member; bool presented; } VIEWPORT_FACT[CSS_ENV_FACT_COUNT] = {
    [CSS_ENV_ICB_WIDTH]          = { "initialContainingBlock.width",  true  },
    [CSS_ENV_ICB_HEIGHT]         = { "initialContainingBlock.height", true  },
    /* css-values-4 §6.1.2.1 "The Large, Small, and Dynamic Viewport Sizes"' other two sizes. `presented` is
       TRUE for the same reason it is true two rows up and false three rows down: these are RECTANGLES A
       NAVIGABLE PRESENTS, so a document that is not fully active has none of them either — a viewport sized
       assuming a UA interface expanded is still a viewport, and there is not one here. The LARGE size has no
       row because §6.1.2.1 gives it to the default family as well, so it IS the pair above. */
    [CSS_ENV_SMALL_VIEWPORT_WIDTH]    = { "smallViewportSize.width",   true  },
    [CSS_ENV_SMALL_VIEWPORT_HEIGHT]   = { "smallViewportSize.height",  true  },
    [CSS_ENV_DYNAMIC_VIEWPORT_WIDTH]  = { "dynamicViewportSize.width",  true },
    [CSS_ENV_DYNAMIC_VIEWPORT_HEIGHT] = { "dynamicViewportSize.height", true },
    [CSS_ENV_DEVICE_PIXEL_RATIO] = { "devicePixelRatio",              false },
    /* THE NUMBER IS NOT THIS FILE'S — core/css/font_size_functions.h picks css-fonts-4 §2.5's `medium` and says
       why it passes this component's PICKED-rather-than-DERIVED test — and that is what this seam is FOR: its
       contract is WHICH facts a length may be a function of and what each is called, never where the value
       came from. `presented` is FALSE for the same reason it is false one row up: the reader's own default
       text size is a preference the user agent has whether or not this document is on a screen, while the
       INITIAL CONTAINING BLOCK is a rectangle a navigable presents and stops existing when it does not. */
    [CSS_ENV_DEFAULT_FONT_SIZE]  = { "defaultFontSize",               false },
    /* The fixed-width default beside it, `presented` false for the same reason. */
    [CSS_ENV_DEFAULT_FIXED_FONT_SIZE] = { "defaultFixedFontSize",     false },
    /* CSS 2.1 §10.8.1's `A`, read off the picked face by core/css/font_metrics.h, which is where the
       PICKED-rather-than-DERIVED test is argued for it — what the model picked is the FACE and not the
       ratio. It is its own row beside the one above rather than sharing it because the two are
       separately observable: a script reads `getComputedStyle(el).fontSize` for the size and measures a `1cap`
       box for the ascent, and one key for both would decide their RATIO on the example — the same mistake
       core/css/css_length.c resolves its `sv*`/`lv*`/`dv*` units by, giving §6.1.2.1's three viewport
       sizes the rows above though all three answer one rectangle today. `presented` is false for the reason it is false one row up: the user agent's installed face
       exists whether or not this document is on a screen. */
    /* CSS 2.1 §10.8.1's `D`, the other half of the pair. Two rows for one face is the same answer this table
       gives the viewport, whose width and height are also one rectangle and also two facts: what decides it is
       whether a page can read them APART, and `1cap` against `1lh` is exactly that subtraction. */
#define VIEWPORT_FONT_FACT_NAMES(id, family, fact, bytes)            \
    [CSS_ENV_FONT_ASCENT_##id]  = { "fontAscent." fact,  false },    \
    [CSS_ENV_FONT_DESCENT_##id] = { "fontDescent." fact, false },    \
    [CSS_ENV_FONT_LINE_GAP_##id] = { "fontLineGap." fact, false },
    INSTALLED_FONT_LIST(VIEWPORT_FONT_FACT_NAMES)
#undef VIEWPORT_FONT_FACT_NAMES
};

/* THE SOURCE KEY, SPELLED ONCE — the document is part of it for media_query_list.c's reason (viewport.h), and
   both the member seam and the derived-length seam below compose it from here so a joint identity's members
   and a member read directly by a page cannot come out under two different names for one fact. */
#define VIEWPORT_SRC_MAX 128

static void viewport_src_key(JSContext *ctx, const char *member, char *out, size_t n)
{
    JSValueConst self = document_window_proxy(ctx);

    DCHECK(window_proxy_is(self),
           "a viewport-derived member was read in a realm whose document has no WindowProxy — a viewport "
           "belongs to a navigable, and every Document this agent holds is one's active document");
    DCHECK(strlen(member) + 24 < n, "a CSSOM VIEW member name longer than any in the IDL");
    snprintf(out, n, "{viewport#%u}%s", (unsigned)window_proxy_doc(self), member);
}

/* See viewport.h: the one place a length's fact SET becomes a domain a page can fork on. Every fact in the set
   contributes one member of solver/concolic.h's JOINT identity, in the table's own order — which the solver
   canonicalizes, so the identity is the set's and not this loop's. */
/* One member per fact in `len`'s set, in the table's order: `key[i]` the per-realm source key, `hole[i]` its
   braced shape, `fact[i]` the fact. Answers the count. */
typedef struct {
    const char *shapes[CSS_ENV_FACT_COUNT];
    const char *srcs[CSS_ENV_FACT_COUNT];
    CssEnvFact  fact[CSS_ENV_FACT_COUNT];
    char        key[CSS_ENV_FACT_COUNT][VIEWPORT_SRC_MAX];
    char        hole[CSS_ENV_FACT_COUNT][VIEWPORT_SRC_MAX];
} ViewportEnvMembers;

static int viewport_env_members(CssPx len, ViewportEnvMembers *m)
{
    char (*key)[VIEWPORT_SRC_MAX] = m->key, (*hole)[VIEWPORT_SRC_MAX] = m->hole;
    const char **shapes = m->shapes, **srcs = m->srcs;
    int f, n = 0;

    DCHECK(len.realm != NULL,
           "a length that is a function of an environment fact reached the boundary with no realm to answer it "
           "per — a child navigable's ICB is 300 CSS pixels wide and the top-level traversable's is 1280, so "
           "the fact alone does not say which question this is");
    DCHECK((len.env & ~CSS_ENV_ALL) == CSS_ENV_NONE,
           "a length carries a CssEnvFact this seam has no member name for. Every fact is one core/frame/"
           "viewport.h has decided is PICKED — css_length.h says so, and the test for one is this component's "
           "— so a new fact is a new row HERE, and a fact without one would cross to the page as a bare number "
           "with its domain dropped");
    for (f = 0; f < CSS_ENV_FACT_COUNT; f++) {
        if (!(len.env & CSS_ENV_BIT(f))) continue;
        DCHECK(VIEWPORT_FACT[f].member != NULL,
               "a fact this seam declares has no member name in its row — the table is indexed BY the fact, so "
               "a hole in it is a fact whose row was never written and whose domain would be spelled empty");
        /* THE FACT NAMES ITSELF, because this line is ONE site for every `presented` row and a message that
           spelled one of them would send the reader to the wrong rectangle for the others — the shared-helper
           address defect, which this table grew the moment §6.1.2.1's viewport sizes joined the ICB here. */
        DCHECKF(!VIEWPORT_FACT[f].presented || viewport_exists(len.realm),
                "a length derived from `%s` reached the page out of a realm whose document is not being "
                "presented. Every fact this table marks as presented is a RECTANGLE A NAVIGABLE PRESENTS — "
                "CSS 2.1 §10.1's initial containing block has the dimensions of the viewport, and "
                "css-values-4 §6.1.2.1's small and dynamic viewport sizes are that viewport sized under two "
                "assumptions about UA interfaces — and viewport.h makes a document that is not fully active "
                "have none of them. So this length was derived from a rectangle that does not exist rather "
                "than from one whose size is a UA choice", VIEWPORT_FACT[f].member);
        viewport_src_key(len.realm, VIEWPORT_FACT[f].member, key[n], sizeof key[n]);
        /* EACH MEMBER'S SHAPE IS ITS OWN HOLE, exactly as it is at the scalar seam above — the joint's display
           form is these joined, so a member that named no hole would leave the composed shape naming one
           fewer than the value depends on, and concolic_hole_key would answer for the set under a name
           missing a member. */
        DCHECK(strlen(VIEWPORT_FACT[f].member) + 3 <= sizeof hole[n],
               "a CSSOM VIEW fact's member name is too long to brace");
        snprintf(hole[n], sizeof hole[n], "{%s}", VIEWPORT_FACT[f].member);
        shapes[n] = hole[n];
        srcs[n] = key[n];
        m->fact[n] = (CssEnvFact)f;
        n++;
    }
    DCHECK(n >= 1, "a non-empty fact set named no facts — the set and the table have come apart");
    return n;
}

JSValue viewport_env_derived(CssPx len, JSValue computed)
{
    ViewportEnvMembers m;
    int n;

    if (len.env == CSS_ENV_NONE) {
        DCHECK(len.realm == NULL,
               "a length that derives from NO environment fact carried a realm anyway — the two are written "
               "together by css_px_env and by nothing else, so one without the other is a length assembled "
               "field-by-field past that entry");
        return computed;
    }
    n = viewport_env_members(len, &m);
    return concolic_source_wrap_joint(len.realm, m.shapes, m.srcs, n, computed);
}

static char *ve_spell_atom(JSContext *realm, unsigned atom, const char *const *keys);

/* A monomial's spelling over `keys`, the per-realm source key of each fact (NULL for a fact not in the length's
   set): its facts as `key^e` joined by `*`, and, around them, the atom it holds as that atom's definition.
   Owned. */
static char *ve_spell_mono(JSContext *realm, const CssPxMonomial *mono, const char *const *keys)
{
    char *facts = NULL, *atom, *out;
    const char *fields[2];
    size_t len = 0;
    FILE *f = open_memstream(&facts, &len);
    bool any = false;
    int e;

    CHECK(f, "viewport: OOM spelling a monomial");
    for (e = 0; e < CSS_ENV_FACT_COUNT; e++) {
        if (!mono->e[e]) continue;
        DCHECKF(keys[e] != NULL, "a monomial names fact %d, which is not in the length's set", e);
        fprintf(f, "%s%s^%u", any ? "*" : "", keys[e], mono->e[e]);
        any = true;
    }
    CHECK(fclose(f) == 0 && facts, "viewport: OOM spelling a monomial");
    if (!mono->atom) {
        DCHECK(any, "a monomial of a length's form names neither a fact nor an atom");
        return facts;
    }
    atom = ve_spell_atom(realm, mono->atom, keys);
    fields[0] = atom;
    fields[1] = facts;
    out = concolic_ident_compose("atom", fields, 2);
    free(atom);
    free(facts);
    return out;
}

/* `c + Σ k·m` spelled over `keys`: the constant, then each monomial and its coefficient. Owned. */
static char *ve_spell_poly(JSContext *realm, const char *tag, double c, const CssPxMonomial *m, int n,
                           const char *const *keys)
{
    char **monos = lexbor_calloc((size_t)n + 1, sizeof *monos);
    char (*coeff)[32] = lexbor_calloc((size_t)n + 1, sizeof *coeff);
    const char **fields = lexbor_calloc(2 * (size_t)n + 1, sizeof *fields);
    char *out;
    int j, nf = 0;

    CHECK(monos && coeff && fields, "viewport: OOM spelling a length's form");
    snprintf(coeff[n], sizeof coeff[n], "%.17g", c);
    fields[nf++] = coeff[n];
    for (j = 0; j < n; j++) {
        monos[j] = ve_spell_mono(realm, &m[j], keys);
        snprintf(coeff[j], sizeof coeff[j], "%.17g", m[j].k);
        fields[nf++] = monos[j];
        fields[nf++] = coeff[j];
    }
    out = concolic_ident_compose(tag, fields, nf);
    for (j = 0; j < n; j++) free(monos[j]);
    free(monos);
    free(coeff);
    free(fields);
    return out;
}

/* An atom spelled by its definition (core/css/css_length.h's css_px_atom_definition): its operation over its
   operands, each a determined number or a form spelled over `keys`. A definition is the function's, so the
   spelling is one every session shares, where the atom's number is its realm's. Owned. */
static char *ve_spell_atom(JSContext *realm, unsigned atom, const char *const *keys)
{
    const CssPxAtomArg *args;
    int nargs, a;
    const char *op = css_px_atom_definition(realm, atom, &nargs, &args);
    char **sp = lexbor_calloc((size_t)nargs + 1, sizeof *sp);
    char *out;

    CHECK(sp, "viewport: OOM spelling an atom");
    for (a = 0; a < nargs; a++) {
        if (args[a].determined) {
            sp[a] = lexbor_malloc(32);
            CHECK(sp[a], "viewport: OOM spelling an atom");
            snprintf(sp[a], 32, "%.17g", args[a].px);
        } else {
            sp[a] = ve_spell_poly(realm, "form", args[a].c, args[a].m, args[a].n, keys);
        }
    }
    out = concolic_ident_compose(op, (const char *const *)sp, nargs);
    for (a = 0; a < nargs; a++) free(sp[a]);
    free(sp);
    return out;
}

/* See viewport.h. The term is named by the form's monomials over the facts' per-realm keys and by the
   fractional constant: `round(P + r)` is one function of the facts for one (P, r). An atom in P is named by its
   definition, so the term is one another session spells the same way. */
JSValue viewport_env_derived_long(CssPx len, JSValue computed)
{
    ViewportEnvMembers m;
    const char *keys[CSS_ENV_FACT_COUNT] = { NULL };
    double at, n_int, r, ex = 0.0;
    int n, i;
    char *term;
    JSValue out;

    if (len.env == CSS_ENV_NONE || !len.form->known) return viewport_env_derived(len, computed);
    n = viewport_env_members(len, &m);
    /* The form evaluated at this realm's facts is the example the layout computed: a writer that replaced the
       number and kept the form would fail here (css_px_atom is the route that replaces it). */
    at = css_px_form_eval(len);
    DCHECKF(fabs(at - len.px) <= 1e-9 * fmax(1.0, fabs(len.px)),
            "a length of %.17g CSS px carries a form that evaluates to %.17g at its own facts — its number "
            "was replaced past css_px_atom, or its form was composed from a different operation than its "
            "example", len.px, at);
    JS_ToFloat64(len.realm, &ex, computed);
    /* No fact moves the length: it is `round(c)` in every world, which is the example. */
    if (len.form->n == 0) {
        DCHECKF(ex == floor(len.form->c + 0.5),
                "a length whose form is the constant %.17g crossed as the `long` %.17g", len.form->c, ex);
        return computed;
    }
    n_int = floor(len.form->c);
    r = len.form->c - n_int;
    for (i = 0; i < n; i++) keys[m.fact[i]] = m.srcs[i];
    term = ve_spell_poly(len.realm, "cssom-long-round", r, len.form->m, len.form->n, keys);
    out = concolic_source_wrap_joint_affine(len.realm, m.shapes, m.srcs, n, term, n_int, computed);
    free(term);
    return out;
}

/* See viewport.h: the one seam, and the one speller of the key.
   THE SHAPE IS THE MEMBER IN BRACES AND THE SOURCE IDENTITY IS THE PER-REALM KEY — two different questions,
   and the shape used to be the bare member, which names no hole. concolic_new asserts the brace now, and the
   reason it matters here is `innerWidth` and `devicePixelRatio`: the breakpoint and the retina gate, branched
   on by nearly every responsive bundle, whose every recorded bound went to concolic_hole_key and came back
   NULL. visualViewport reaches this same seam with its member already qualified, so the two never collide. */
JSValue viewport_env_value(JSContext *ctx, const char *member, JSValue computed)
{
    char src[VIEWPORT_SRC_MAX], hole[VIEWPORT_SRC_MAX];

    viewport_src_key(ctx, member, src, sizeof src);
    DCHECK(strlen(member) + 3 <= sizeof hole, "a CSSOM VIEW member name too long to brace");
    snprintf(hole, sizeof hole, "{%s}", member);
    return concolic_source_wrap(ctx, hole, src, computed);
}

/* ---- CSSOM VIEW §4's `scrollX`/`scrollY`, WHICH ARE NOW A READ OF REAL PER-FLOW STATE ---------------------- */

/* §4: "the x-coordinate, RELATIVE TO THE INITIAL CONTAINING BLOCK ORIGIN, of the left of the viewport". So the
 * question is where the viewport sits over its own scrolling area, and the answer is STORED rather than
 * derived: CSSOM VIEW §3.1 "Scrolling"'s perform a scroll (core/dom/perform_scroll.h) writes the record below
 * and nothing else may, so this is the read of what §3.1 wrote.
 *
 * IT IS PER-FLOW STATE AND THAT IS WHY IT IS A RECORD AND NOT A `double`. Two flows that scrolled one document
 * differently must read back different numbers — a flow exploring the world in which a sticky header collapsed
 * is standing somewhere the flow beside it is not — so the position rides the per-flow COW delta. It rides it
 * for free because the record is an ordinary heap object and each write below is an ordinary property write,
 * which is the same shape as §13.1's resize latch at the bottom of this file, and the same reason: a `double`
 * in a static would be ONE answer for every timeline, and a malloc'd record would need a capture at every
 * accessor.
 *
 * IT COSTS NOTHING PER FLOW UNTIL A FLOW SCROLLS, and that is worth stating because the arithmetic reads the
 * other way at a glance. The record is built WITH the realm, which is before any page script runs, so it is
 * part of the COW BASELINE that every flow shares — a frontier of hundreds of members holds ONE of these, not
 * one each. A delta entry appears only where a flow WRITES, and the only writer is §3.1's perform a scroll, so
 * a flow that never scrolled carries nothing about the scroll position at all.
 *
 * WHAT THIS RETIRES, RESTATED IN CAPITALS SO NOBODY RE-DERIVES IT. Three arguments stood here and each was the
 * reason `scrollX` answered zero. THE FIRST: THIS ENGINE GENERATES NO BOXES. THE SECOND: THE ONLY BOX WITH
 * GEOMETRY IS THE ICB, SO NO DESCENDANT'S MARGIN EDGE EXTENDS §2's SCROLLING AREA PAST IT, AND A SCROLLING BOX
 * WHOSE SCROLLING AREA IS ITS OWN SIZE HAS ONE VALID POSITION. The diff that built §2's viewport row retired
 * those two. THE THIRD SURVIVED THEM AND IS WHAT THIS DIFF RETIRES: A SCROLL POSITION MOVES ONLY WHEN §3.1's
 * PERFORM A SCROLL RUNS, AND NOTHING IN THIS ENGINE REACHES ONE. §3.1 is written; the position moves; the zero
 * below is now just the value a document starts at. All three corrections are one defect — a sentence about
 * what this engine cannot do, outliving the day it could not do it, and read by everyone downstream as the
 * reason not to look.
 *
 * WHAT IS STILL MISSING IS NOT IN THIS FILE. §3.1 changes the position and then CRASHES, because CSSOM VIEW
 * §13.2 "Scrolling" has no list for the `scroll` and `scrollend` a moved viewport owes and HTML §8.1.7.3
 * "Processing model" update-the-rendering step 9 has no drain — perform_scroll.c names both at the site. So a
 * dev build reaches a non-zero position and aborts there; nothing here is waiting on anything. */

/* The record's two fields are the position, and they are the ONLY writable state this component holds about
   the viewport. `viewport_install` builds it with the realm, so every realm has one before any member of §4
   can be read. */
#define VP_POS_X "x"
#define VP_POS_Y "y"

static JSClassID g_scroll_slot = JS_INVALID_CLASS_ID;

static double vp_scroll_axis(JSContext *ctx, const char *field)
{
    JSValue rec = realm_value_get(ctx, g_scroll_slot);
    JSValue v = JS_GetPropertyStr(ctx, rec, field);
    double d = 0.0;

    DCHECK(JS_IsNumber(v),
           "the viewport's scroll-position record holds an axis that is not a number — CSSOM VIEW §3.1's "
           "perform a scroll is the only writer and it writes a clamped `double` on both axes, so this is a "
           "write from outside that algorithm");
    JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    return d;
}

double viewport_scroll_x(JSContext *ctx)
{
    return vp_scroll_axis(ctx, VP_POS_X);
}

double viewport_scroll_y(JSContext *ctx)
{
    return vp_scroll_axis(ctx, VP_POS_Y);
}

/* §3.1's INSTANT SCROLL OF THIS REALM'S VIEWPORT — the one writer, and see viewport.h for why the position is
   this component's to hold rather than §3.1's.
   The clamp is the caller's and is not re-made here: §4's steps 7-8 and §6.1's own two rows are the same four
   rows over one fact (core/dom/element_scrolling.h). What is asserted is the clamp's postcondition against the
   very box `b` the caller measured and clamped into, so a route that skipped the clamp crashes here instead of
   placing the viewport outside its scrolling area, and no second walk of that area is made. */
void viewport_set_scroll_position(JSContext *ctx, double x, double y, const ElementScrollingBox *b)
{
    JSValue rec;

    DCHECK(viewport_exists(ctx),
           "the viewport's scroll position was written in a realm that is presenting no document — §3.1's "
           "perform a scroll asserts the same thing one frame up, and a position without a viewport is a "
           "number about a rectangle that is not there");
    DCHECK(b->el == NULL && b->dctx == ctx,
           "the viewport's scroll position was written against a box that is not this realm's viewport");
    DCHECK(isfinite(x) && isfinite(y),
           "the viewport's scroll position was written non-finite — CSSOM VIEW §3.2 \"WebIDL values\"' "
           "normalize non-finite values runs at the member and §6.1's clamp is total, so a non-finite here is "
           "one of those two having been skipped");
    /* A magnitude, so it holds for either overflow direction: max(0, min(v, slack)) lands in [0, slack] and
       min(0, max(v, -slack)) in [-slack, 0]. */
    DCHECKF(fabs(x) <= b->area[0].px - b->extent[0].px && fabs(y) <= b->area[1].px - b->extent[1].px,
            "the viewport was moved to (%g, %g), outside §2's scrolling area of a viewport (%g x %g over a %g x "
            "%g viewport) that the caller measured: every route to this writer runs §4's steps 7-8 or §6.1's "
            "identical rows over that box first, so this route skipped them",
            x, y, b->area[0].px, b->area[1].px, b->extent[0].px, b->extent[1].px);
    rec = realm_value_get(ctx, g_scroll_slot);
    JS_SetPropertyStr(ctx, rec, VP_POS_X, JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, rec, VP_POS_Y, JS_NewFloat64(ctx, y));
    JS_FreeValue(ctx, rec);
}

/* THE ATTRIBUTE, as opposed to the derivation above — see viewport.h for why the two are separate and why the
   member's "or zero if there is no viewport" is written once here rather than at each of its three callers. */
double viewport_window_scroll(JSContext *ctx, bool vertical)
{
    if (!viewport_exists(ctx)) return 0.0;
    return vertical ? viewport_scroll_y(ctx) : viewport_scroll_x(ctx);
}

/* CSSOM VIEW §4 "Extensions to the Window Interface"' scroll() steps 7 to 13, as the internal algorithm §2
   requires a caller to invoke — see viewport.h. The numbers are the thirteen top-level steps of the edition
   engine/specindex/cssomview.json is keyed to; steps 7 and 8 are one step each holding a two-armed
   `<dl class="switch">`, so check a cluster of them from its last member backwards. */
void viewport_scroll_measured(const ElementScrollingMeasure *m, ScrollRequest x, ScrollRequest y,
                              const char *behavior)
{
    JSContext *ctx = m->box.dctx;
    double xp, yp;

    DCHECK(m->box.el == NULL && viewport_exists(ctx),
           "§4's scroll() steps 7 to 13 were handed a box that is not a presented viewport — steps 4 to 6 are "
           "the caller's, and they measure the viewport the realm presents");
    /* Step 7 is the first step that reads the requested position, so it is where an unknown one is decided
       (core/dom/perform_scroll.h's `scroll_request_resolve`, for every scroll member).
       Steps 7 and 8 are one switch per axis on §2's overflow directions, both arms written once for an element's
       box and a viewport's (core/dom/element_scrolling.h), over the measured viewport: steps 5 and 6's
       viewport width and height (no scroll bar is rendered) are its extent, and the viewport scrolling area is
       its walked area. */
    xp = element_scrolling_measured_clamp(m, false, scroll_request_resolve(x));
    yp = element_scrolling_measured_clamp(m, true, scroll_request_resolve(y));
    /* Step 9 — "let position be the scroll position the viewport would have by aligning the x-coordinate x of
       the viewport scrolling area with the left of the viewport and aligning the y-coordinate y of the viewport
       scrolling area with the top of the viewport" — is the IDENTITY here, derived rather than skipped: §2's
       viewport row anchors the scrolling area's beginning edges on the initial containing block's own edges and
       core/layout/flow_position.h places every box in that same space, so a scrolling-area coordinate IS a
       scroll position and `position` is (x, y).
       §4's STEP 10 — "If position is the same as the viewport's current scroll position, and the viewport does
       not have an ongoing smooth scroll, return a resolved `Promise` and abort the remaining steps." The second
       conjunct is FALSE for every box in this engine, derived rather than skipped: §3.1 step 5's smooth arm is
       the only thing that starts one and this user agent never takes it (core/dom/perform_scroll.h states the
       derivation and asserts it). */
    if (xp == viewport_scroll_x(ctx) && yp == viewport_scroll_y(ctx)) return;
    /* Step 11 — "Let document be the viewport's associated `Document`." — is the measured box's document, and
       step 12's "document's root element as the associated element" is the one that measurement reached.
       STEP 12 — "Perform a scroll of the viewport to position, document's root element as the associated
       element, if there is one, or null otherwise, and the scroll behavior being the value of the `behavior`
       dictionary member of options."
       IT IS §3.1's SCROLLING-BOX ALGORITHM AND NOT ITS COORDINATED VIEWPORT ONE, and that is the standard's own
       permission rather than a shortcut: the note §4 prints directly under this step reads "User agents do not
       agree whether this uses the (coordinated) viewport perform a scroll or the scrolling box perform a scroll
       on the layout viewport's scrolling box." The two also COINCIDE in this model, which is why the choice costs
       nothing to make: the coordinated algorithm's visual deltas are min(maxX, max(0, visual x + dx)) - visual
       x, and core/frame/visual_viewport.h derives a scale factor of 1 — at which the visual viewport covers the
       layout viewport, so maxX and maxY are zero, both visual deltas are zero and the whole of the request is
       the layout delta this line performs. A scale factor that is not 1 is what would separate them, and it is
       visual_viewport.c that would then own the second half. */
    perform_scroll(ctx, &m->box, xp, yp, lxb_dom_interface_element(document_root_node(ctx)), behavior);
    /* Step 13 — "Return scrollPromise." This entry returns `void` and its callers mint a RESOLVED promise: §3.1
       resolves the promise it minted before it returns, because every scroll this user agent performs is an
       INSTANT one (core/dom/perform_scroll.h). */
}

/* The client window's size, in CSS pixels, and its position relative to §2.3's Web-exposed screen area origin.
   The position is DERIVED and not a second UA choice: an ordinary desktop window sits in the middle of the
   space the operating system leaves for it, and screen.c already states what that space is. It is DERIVED and
   still a SOURCE, which is not a contradiction and is the distinction viewport.h now draws — a joint over the
   two members it is a function of, minted at `vp_screen_pos` below.
   EVERY ASSERT BELOW IS OVER THE MODELLED EXAMPLE, which is the only value that reaches them: the size is a
   `double` this file computes and screen.c's is a `double` it computes, so neither can be a concolic and a C
   `if` here cannot pick an arm. What they assert is that the MODEL is coherent — a window a UA could actually
   open, at a coordinate on the screen the coordinate is measured from.
   THE TWO ASSERTS PER AXIS ARE TWO DIFFERENT AREAS AND NEITHER IMPLIES THE OTHER. §4's sentence measures the
   coordinate "relative to the origin of the Web-exposed SCREEN area" while §2.3's AVAILABLE area is the
   sub-area the window is positioned inside, so the model has to be coherent in both: a window wider than the
   available area yields a NEGATIVE coordinate, and one whose far edge passes the screen area's own extent
   yields a coordinate for a window hanging off the display. The second is what would fire the day the reserved
   strip is modelled at the TOP of the display rather than at its bottom — the available area's origin would
   then no longer be the screen area's, and this file's arithmetic, which measures from the available area and
   reports from the screen area, would be off by the strip with nothing else to say so. */
static double viewport_client_width(void)  { return VIEWPORT_TOP_WIDTH; }
static double viewport_client_height(void) { return VIEWPORT_TOP_HEIGHT + VIEWPORT_CHROME_HEIGHT; }

static double viewport_client_screen_x(void)
{
    double x;

    DCHECK(viewport_client_width() <= screen_avail_width(),
           "the modelled client window is wider than the Web-exposed available screen area it is positioned "
           "inside — a window a UA could not open, and `screenX` would come out negative");
    x = (double)(int)((screen_avail_width() - viewport_client_width()) / 2.0);
    DCHECK(x >= 0.0 && x + viewport_client_width() <= screen_width(),
           "CSSOM VIEW §4 measures `screenX` from the origin of the Web-exposed SCREEN area, and the modelled "
           "client window at the position this derives does not lie within that area — so the number a page "
           "would read names a window hanging off the display. The available area's origin is the screen "
           "area's ONLY while the space the OS reserves is not taken off the left edge; take it off the left "
           "and this arithmetic, which measures from the available area, must add that offset back");
    return x;
}

static double viewport_client_screen_y(void)
{
    double y;

    DCHECK(viewport_client_height() <= screen_avail_height(),
           "the modelled client window is taller than the Web-exposed available screen area it is positioned "
           "inside — a window a UA could not open, and `screenY` would come out negative");
    y = (double)(int)((screen_avail_height() - viewport_client_height()) / 2.0);
    DCHECK(y >= 0.0 && y + viewport_client_height() <= screen_height(),
           "CSSOM VIEW §4 measures `screenY` from the origin of the Web-exposed SCREEN area, and the modelled "
           "client window at the position this derives does not lie within that area — so the number a page "
           "would read names a window hanging off the display. The available area's origin is the screen "
           "area's ONLY while the space the OS reserves is a taskbar at the BOTTOM; model it as a menu bar at "
           "the top and this arithmetic, which measures from the available area, must add that offset back");
    return y;
}

/* ---- CSSOM VIEW §4's Window extensions ------------------------------------------------------------------- */

/* THE MEMBERS, AS ONE LIST, and the aliases are IN it rather than being a second install, so each pair shares
   one magic and there is no second body that could ever answer differently.
   §4 SPELLS THE TWO PAIRS DIFFERENTLY, AND THIS SAID THEY WERE SPELLED ALIKE. `pageXOffset` IS stated as
   another attribute's value — "The pageXOffset attribute must return the value returned by the scrollX
   attribute" — and `screenLeft` is NOT: §4 gives it and `screenX` ONE sentence naming both attributes ("The
   screenX and screenLeft attributes must return the x-coordinate, relative to the origin of the Web-exposed
   screen area, of the left of the client window as number of CSS pixels, or zero if there is no such thing").
   The word alias for that pair is in §17 "Changes", under its UNNUMBERED subsection headed "Changes since the
   17 March 2016 Working Draft" — named in the standard's own words because there is no number to cite it by,
   and citegen.mjs indexes numbered sections, so that one quotation is beyond what any instrument here can
   confirm and the reader is owed the heading instead.
   THE CLAIM THAT §4 STATES `screenLeft` AS `screenX`'s VALUE WAS WRITTEN HERE and was repeated once, from
   this comment, when the canonical-name helper below was added. It is corrected at the site it was made
   rather than only at the newer one, because a reader who re-derives the retired reason re-introduces it.
   Nothing about the shared magic changes — it was the right structure resting on a sentence the standard does
   not contain, which is exactly the fabricated quotation CLAUDE.md §Browser-half calls worse than a wrong
   number: the number resolved, the title matched, and only reading the section's words found it.
   Every one of them is [Replaceable] readonly — assigning replaces the accessor with an ordinary data
   property, which is what idl_install_replaceable performs. */
typedef enum {
    VP_INNER_W, VP_INNER_H, VP_OUTER_W, VP_OUTER_H,
    VP_SCROLL_X, VP_SCROLL_Y, VP_SCREEN_X, VP_SCREEN_Y, VP_DPPX
} ViewportMember;

#define VIEWPORT_WINDOW_MEMBERS(X)   \
    X("innerWidth",       VP_INNER_W)  \
    X("innerHeight",      VP_INNER_H)  \
    X("outerWidth",       VP_OUTER_W)  \
    X("outerHeight",      VP_OUTER_H)  \
    X("scrollX",          VP_SCROLL_X) \
    X("pageXOffset",      VP_SCROLL_X) \
    X("scrollY",          VP_SCROLL_Y) \
    X("pageYOffset",      VP_SCROLL_Y) \
    X("screenX",          VP_SCREEN_X) \
    X("screenLeft",       VP_SCREEN_X) \
    X("screenY",          VP_SCREEN_Y) \
    X("screenTop",        VP_SCREEN_Y) \
    X("devicePixelRatio", VP_DPPX)

/* The names are string LITERALS so engine/idlgen.mjs's install audit can see them — a name built by
   concatenation reads as absent to the auditor, which is the audit lying by omission. */
static const char *const VP_NAME[] = {
#define X(n, m) n,
    VIEWPORT_WINDOW_MEMBERS(X)
#undef X
};
static const int VP_MAGIC[] = {
#define X(n, m) (int)(m),
    VIEWPORT_WINDOW_MEMBERS(X)
#undef X
};
#define VP_NAMES ((int)(sizeof(VP_NAME) / sizeof(VP_NAME[0])))

/* A member whose IDL type is `long`. Every length this component models is a whole number of CSS pixels, so a
   fraction here is a derivation that produced something the type cannot carry rather than a value to round.
   THE ASSERT HOLDS FOR THE WHOLE DOMAIN and not merely for the example: the members it guards are declared
   `long`, so no viewport this UA could have picked makes one of them fractional. */
static JSValue vp_long(JSContext *ctx, double v)
{
    DCHECK(v == (double)(int32_t)v,
           "a CSSOM VIEW §4 Window member declared `long` computed a value that is not an integer");
    return JS_NewInt32(ctx, (int32_t)v);
}

/* THE CANONICAL NAME OF A §4 MEMBER — the FIRST name the one X-list gives that magic. Which of a pair is
   first is this file's choice among equals rather than the standard's ranking of them; the X-list's own
   comment above reads §4's two spellings and says why neither name is defined in terms of the other, and that
   is the one place the question is answered.
   Read OUT of the list rather than written a second time: a joint below names a member by this, and a name
   spelled twice is a joint naming a hole whose mint spells it differently — which composes a key no emission
   can look up.
   IT IS A `CHECK` AND NOT A `DFAIL`, WHICH IS THE ONE THING ABOUT IT THAT IS NOT ORDINARY. Every other
   should-never-happen in this file returns a VALUE past its abort, so a release build that compiles the abort
   out still answers something. This one returns a POINTER its caller hands to `snprintf`, so a dev-only abort
   would leave the dereference standing in exactly the build the guard is gone from — CLAUDE.md §Offensive
   programming's promotion rule, and the promotion belongs to the line that created the dereference rather than
   to a later reading of the assert. */
static const char *vp_member_name(ViewportMember m)
{
    int i;

    for (i = 0; i < VP_NAMES; i++)
        if (VP_MAGIC[i] == (int)m) return VP_NAME[i];
    CHECK_FAIL("a CSSOM VIEW §4 member has no name in the one list the install walks — the magic IS the "
               "member, so a member with no row is one this file answers for and never installs");
    return NULL;
}

/* A `long` member that reports the environment: the modelled answer as the EXAMPLE of a concolic keyed on this
   document's answer for this member. */
static JSValue vp_env_long(JSContext *ctx, const char *member, double v)
{
    return viewport_env_value(ctx, member, vp_long(ctx, v));
}

/* THE CLIENT WINDOW'S POSITION, AS A VALUE A PAGE CAN FORK ON — §4's `screenX`/`screenLeft` and
 * `screenY`/`screenTop`, and the one place in this file where a member is DERIVED and is still a source.
 *
 * WHY IT IS NOT `viewport_env_value`. A scalar source is a fact the model PICKED; this is a fact the model
 * COMPUTED out of two it picked — §2.3's available screen area and §4's client window size — and neither of
 * those two identities can answer for it. THE NEXT QUOTATION IS THIS TREE'S OWN PROSE AND NOT THE STANDARD'S:
 * solver/concolic.h, in the paragraph over `concolic_source_wrap_joint`, says that keeping one operand's
 * identity "reports a narrowing over the ICB the page never made". So the identity is the SET's, composed from
 * the two members' own identities, and the example is the real arithmetic this file already ran.
 *
 * WHY IT IS NOT CONCRETE, WHICH IS WHAT IT WAS. The argument it replaces is quoted from viewport.h — again
 * THIS TREE'S PROSE, not §4's — and said the position is derived from "two facts already forkable at their own
 * members … so a third independent source reaches no arm those two do not".
 * The premise is right and the conclusion does not follow, in two separate places. (1) A JOINT IS NOT A
 * THIRD INDEPENDENT SOURCE — it introduces no free parameter, carries one example, and is exactly the
 * mechanism the premise asks for; the old argument refuted a design nobody was proposing. (2) THE ARMS ARE NOT
 * THE SAME ARMS. `if (window.screenX < 100)` is its own predicate with its own constraint key, so against a
 * bare `double` it does not fork AT ALL — one arm runs and the other's code is never reached. A flow that
 * narrowed `screen.availWidth` has said nothing about this predicate and cannot decide it, which is the very
 * non-composition the joint's own paragraph calls the sound direction. §Headless's rule is the whole of it: a
 * modelable value collapsed to bare-concrete deletes the fork and its coverage, and `screenX` is what a
 * multi-monitor gate and a popup-placement routine branch on.
 *
 * IT PERMITS NO WORLD WHERE THE WINDOW SITS OFF THE SCREEN, which was the old argument's other half. A
 * concolic is opaque for CONTROL FLOW and carries the one concrete example this model computes; the asserts in
 * `viewport_client_screen_x`/`_y` run over that example and are untouched. */
static JSValue vp_screen_pos(JSContext *ctx, bool vertical, double v)
{
    const char *shapes[2], *srcs[2];
    const char *outer = vp_member_name(vertical ? VP_OUTER_H : VP_OUTER_W);
    char key[VIEWPORT_SRC_MAX], hole[VIEWPORT_SRC_MAX];

    /* MEMBER 0 — §2.3's available screen area, named by the component that mints it, so the joint names the
       very hole `screen.availWidth` reaches the page as. */
    screen_avail_source(vertical, &shapes[0], &srcs[0]);
    /* MEMBER 1 — this document's client-window size, which is `outerWidth`/`outerHeight`: the SAME fact, so
       the same source identity `vp_env_long` mints it under and not a second one for the same window. */
    viewport_src_key(ctx, outer, key, sizeof key);
    DCHECK(strlen(outer) + 3 <= sizeof hole, "a CSSOM VIEW §4 member name is too long to brace");
    snprintf(hole, sizeof hole, "{%s}", outer);
    shapes[1] = hole;
    srcs[1] = key;
    DCHECK(strcmp(srcs[0], srcs[1]) != 0,
           "the client window's size and the Web-exposed available screen area composed the SAME source "
           "identity, so this position would be keyed as a function of one fact twice over — the two are "
           "different facts (`availWidth < width` is the taskbar question) and one key for both would let a "
           "branch over either decide the other");
    return concolic_source_wrap_joint(ctx, shapes, srcs, 2, vp_long(ctx, v));
}

static JSValue js_vp_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    /* §4 conditions six of these on there being a viewport or a client window, and the two absences are the
       same one in this engine: both are what a NAVIGABLE presents, and a document that is not fully active is
       presented by nothing. `devicePixelRatio` is NOT one of them — its algorithm asks about the OUTPUT DEVICE,
       which exists whether or not this document is on it. */
    bool presented = viewport_exists(ctx);

    (void)this_val;
    /* WHICH OF THESE ARE SOURCES AND WHICH ARE DERIVED is viewport.h's paragraph, decided per member and shown
       here at the member. "OR ZERO IF THERE IS NO VIEWPORT" IS NEVER A SOURCE: a document that is not being
       presented has no viewport, the SPEC states the answer, and there is nothing for a UA to have chosen — so
       the absent case is concrete even where the present one forks. */
    switch ((ViewportMember)magic) {
    /* "The innerWidth attribute must return the viewport width INCLUDING the size of a rendered scroll bar (if
       any), or zero if there is no viewport." Including the scrollbar is why no scrollbar question arises: the
       answer is the viewport, whether or not one is rendered. */
    case VP_INNER_W:  return presented ? vp_env_long(ctx, "innerWidth", viewport_width(ctx)) : vp_long(ctx, 0.0);
    case VP_INNER_H:  return presented ? vp_env_long(ctx, "innerHeight", viewport_height(ctx))
                                       : vp_long(ctx, 0.0);
    /* "The outerWidth attribute must return the width of the client window. If there is no client window this
       attribute must return zero." A SEPARATE source from the viewport's own size, which is what makes
       `outerHeight - innerHeight` — how much chrome this UA wears — a question with two answers. */
    case VP_OUTER_W:  return presented ? vp_env_long(ctx, "outerWidth", viewport_client_width())
                                       : vp_long(ctx, 0.0);
    case VP_OUTER_H:  return presented ? vp_env_long(ctx, "outerHeight", viewport_client_height())
                                       : vp_long(ctx, 0.0);
    /* "The scrollX attribute must return the x-coordinate, relative to the initial containing block origin, of
       the left of the viewport, or zero if there is no viewport." An `unrestricted double`, not a `long`.
       CONCRETE, AND FOR A REASON THAT HAS CHANGED. IT USED TO BE THAT THE DERIVATION LEFT ONE VALID SCROLL
       POSITION AND THAT THE LAYOUT WHICH GAVE IT A RANGE WOULD ALSO GIVE IT A WRITER. The writer arrived
       (CSSOM VIEW §3.1's perform a scroll), and this stays concrete because what it reads is a number THIS
       ENGINE STORED — element_view.h's rule is that a fact the model derives or records stays concrete, and a
       scroll position is not an environment the model PICKED. What would make it a concolic is a page writing
       an UNKNOWN position, and core/dom/element_view.c's setter crashes naming that build rather than reaching
       this member with a `double` it invented. The member is `viewport_window_scroll`, which is this sentence
       WHOLE — the read and the no-viewport zero — because §10's `pageX` and §6's scroll members invoke this
       attribute by name and must get the same two halves. */
    case VP_SCROLL_X: return JS_NewFloat64(ctx, viewport_window_scroll(ctx, /*vertical*/ false));
    case VP_SCROLL_Y: return JS_NewFloat64(ctx, viewport_window_scroll(ctx, /*vertical*/ true));
    /* "The screenX and screenLeft attributes must return the x-coordinate, relative to the origin of the
       Web-exposed screen area, of the left of the client window as number of CSS pixels, or zero if there is
       no such thing." A JOINT source over the two facts it is a function of — see `vp_screen_pos` for why a
       derivation is still a source and why the old concrete answer deleted an arm. The no-client-window zero
       stays concrete on this file's own rule: §4 states that answer, so nothing was chosen. */
    case VP_SCREEN_X: return presented ? vp_screen_pos(ctx, /*vertical*/ false, viewport_client_screen_x())
                                       : vp_long(ctx, 0.0);
    case VP_SCREEN_Y: return presented ? vp_screen_pos(ctx, /*vertical*/ true, viewport_client_screen_y())
                                       : vp_long(ctx, 0.0);
    /* §4's DETERMINE THE DEVICE PIXEL RATIO: with no output device return 1; otherwise the CSS pixel size
       divided by the device pixel size. This engine models an output device (screen.c), so it is the ratio —
       and it is a source, because `devicePixelRatio > 1` is the retina gate. */
    case VP_DPPX:     return viewport_env_value(ctx, "devicePixelRatio",
                                                JS_NewFloat64(ctx, viewport_device_pixel_ratio(ctx)));
    }
    DFAIL("a CSSOM VIEW §4 Window member was read with a magic no member of this file declares — the magic IS "
          "the member, so an unknown one means a name was installed without a case to answer it");
    return JS_UNDEFINED;
}

/* ---- CSSOM VIEW §13.1's resize steps, this component's half ----------------------------------------------- */

/* The record's three fields ARE the spec sentence: what the viewport measured the last time the steps ran, and
   whether there HAS been a last time. See viewport.h for why the second one cannot be replaced by seeding the
   first at install.
   WHAT IT LATCHES IS THE MODELLED GEOMETRY, never what a flow decided about it. §13.1 asks whether the VIEWPORT
   changed, and a flow that took the true arm of `innerWidth < 768` did not resize anything — it recorded what
   it believes, and there is no solve-back that hands the model a different width. So the comparison is over the
   `double` this component computes, and the `JS_IsNumber` asserts below are two-sided: they fire if a concolic
   ever reaches the latch, which is what would say the seam had moved to the wrong side of the boundary. */
#define VP_RESIZE_RAN "hasBeenRun"
#define VP_RESIZE_W   "width"
#define VP_RESIZE_H   "height"

static JSClassID g_resize_slot = JS_INVALID_CLASS_ID;

bool viewport_resize_changed(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_resize_slot);
    JSValue ran = JS_GetPropertyStr(ctx, rec, VP_RESIZE_RAN);
    double w = viewport_width(ctx), h = viewport_height(ctx);
    bool had_run, changed;

    DCHECK(JS_IsBool(ran),
           "the §13.1 resize-steps record's `has been run` field is not a boolean — nothing but these steps "
           "ever writes this record, and they write exactly the three fields the algorithm names");
    had_run = JS_ToBool(ctx, ran);
    JS_FreeValue(ctx, ran);

    changed = false;
    if (had_run) {
        JSValue lw = JS_GetPropertyStr(ctx, rec, VP_RESIZE_W);
        JSValue lh = JS_GetPropertyStr(ctx, rec, VP_RESIZE_H);
        double dw = 0.0, dh = 0.0;

        DCHECK(JS_IsNumber(lw) && JS_IsNumber(lh),
               "the §13.1 resize-steps record holds a dimension that is not a number — a run that latched one "
               "latched both, so this is a write from outside the algorithm");
        JS_ToFloat64(ctx, &dw, lw);
        JS_ToFloat64(ctx, &dh, lh);
        JS_FreeValue(ctx, lw);
        JS_FreeValue(ctx, lh);
        changed = dw != w || dh != h;
    }
    /* WRITTEN ONLY WHEN IT MOVES, because each write is captured into the running flow's COW delta and a frame
       that re-latches an unchanged pair would grow every flow's delta once per rendering opportunity for
       nothing. Re-writing the same numbers is not observable, so not writing them is the same algorithm. */
    if (changed || !had_run) {
        JS_SetPropertyStr(ctx, rec, VP_RESIZE_W, JS_NewFloat64(ctx, w));
        JS_SetPropertyStr(ctx, rec, VP_RESIZE_H, JS_NewFloat64(ctx, h));
        if (!had_run)
            JS_SetPropertyStr(ctx, rec, VP_RESIZE_RAN, JS_TRUE);
    }
    JS_FreeValue(ctx, rec);
    return changed;
}

/* ---- CSSOM VIEW §4's `scroll()`, `scrollTo()` and `scrollBy()` --------------------------------------------- */

/* §4's OWN ENUMERATION AND DICTIONARY — see viewport.h for why they are exported and for §3.2.17's read order.
   The names are string LITERALS so engine/idlgen.mjs's install audit can see them. */
const char *const VIEWPORT_SCROLL_BEHAVIOR[] = { "auto", "instant", "smooth", NULL };
const IdlDictMember VIEWPORT_SCROLL_TO_OPTIONS[] = {
    { "behavior", IDL_ENUM, false, VIEWPORT_SCROLL_BEHAVIOR, 0, NULL, IDL_DEFAULT_STRING, "auto" },
    { "left",     IDL_UNRESTRICTED_DOUBLE, false, NULL, 1 },
    { "top",      IDL_UNRESTRICTED_DOUBLE, false, NULL, 1 },
};
const int VIEWPORT_SCROLL_TO_OPTIONS_N =
    (int)(sizeof VIEWPORT_SCROLL_TO_OPTIONS / sizeof VIEWPORT_SCROLL_TO_OPTIONS[0]);

/* §4's TWO OVERLOADS AS ONE DECLARATION — the longest type list the effective overload set has, with the
   position the entries split at carrying that split as its type:

       Promise<undefined> scroll(optional ScrollToOptions options = {});
       Promise<undefined> scroll(unrestricted double x, unrestricted double y);

   §3.6 steps 3-4 remove the dictionary entry the moment a second argument is passed, which is why position 0
   is one row rather than a shape test in the body — see IDL_UNRESTRICTED_DOUBLE_OR_DICT. It is the same
   declaration §6 makes for the element members, and it is written twice because it is a property of THESE
   THREE members: an argument-type list is not a shared fact the way the dictionary is, and an `extern` array
   of two enum values would cost a header entry to say nothing the IDL block above does not. */
static const IdlArgType VP_SCROLL_ARGS[2] = { IDL_UNRESTRICTED_DOUBLE_OR_DICT, IDL_UNRESTRICTED_DOUBLE };

typedef enum { VP_SCROLL_ABSOLUTE, VP_SCROLL_RELATIVE } ViewportScrollKind;

static int g_id_scroll = -1;
static int g_id_scroll_by = -1;

/* §4's resolved Promise: step 13's "return scrollPromise" and steps 4 and 10's "return a resolved Promise" are
   one object because every scroll this user agent performs is instant, so §3.1 resolves the promise it minted
   before it returns (core/dom/perform_scroll.h). The resolve cannot throw: the capability is fresh and the value
   is `undefined`, so no `then` getter of the page's is reachable. It runs through JS_CallAsFlow because a
   resolving function runs on a flow base and this is a C activation. */
static JSValue vp_resolved_promise(JSContext *ctx)
{
    JSValue funcs[2], promise = JS_NewPromiseCapability(ctx, funcs);
    int r;

    CHECK(!JS_IsException(promise),
          "CSSOM VIEW §4's scroll members answer with a Promise on every path they have, and this one's "
          "capability could not be allocated — a member that answers with neither a promise nor a throw is a "
          "call a page can only hang on");
    r = JS_CallAsFlow(ctx, funcs[0], JS_UNDEFINED);
    DCHECK(r >= 0, "resolving a fresh promise capability with `undefined` completed abruptly — nothing of the "
                   "page's is reachable from there, so a throw means the capability is not the one this just "
                   "created");
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    return promise;
}

/* §4's `scroll()`, which `scrollTo()` is ("act as if the scroll() method was invoked with the same arguments"),
 * and `scrollBy()`, the same steps with its own steps 3 and 4's addition ahead of them, as one step machine
 * owning a layout job. VP_SCROLL_READ is steps 1 to 4: the overload arms (read back off the converted argument
 * count, since §3.6 steps 3-4 decide `scroll` from the count alone), step 3's normalize and `scrollBy`'s
 * addition (core/dom/perform_scroll.h's reader), and step 4's "if there is no viewport, return a resolved
 * Promise"; VP_SCROLL_OPEN opens the viewport's box for measurement. VP_SCROLL_AREA walks the viewport
 * scrolling area one rest unit per activation under a resting drive (core/dom/element_scrolling.h's cursor),
 * then runs steps 7 to 13 (`viewport_scroll_measured`), the internal algorithm §6's element members reach too. */
#define VP_SCROLL_STAGES(X) \
    X(VP_SCROLL_READ, "CSSOM VIEW §4 \"Extensions to the Window Interface\" scroll() / scrollTo() / scrollBy() " \
                      "steps 1-4") \
    X(VP_SCROLL_OPEN, "CSSOM VIEW §4 scroll() steps 5-6 (the viewport's box opened on the layout job)") \
    X(VP_SCROLL_AREA, "CSSOM VIEW §4 scroll() steps 7-13 over the viewport scrolling area measured under a " \
                      "resting drive")
enum { IDL_STEP_STAGE_BASE(VP_SCROLL_STAGES) VP_SCROLL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const VP_SCROLL_STEPS[] = { VP_SCROLL_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    LayoutJob *job;               /* owned, declared through layout_job_step_ops; NULL before step 5 */
    ScrollRequest x, y;           /* the requested position, resolved by step 7 */
    char behavior[16];            /* §4's ScrollBehavior keyword, one of the three the enumeration admits */
    ElementScrollingMeasure m;    /* the viewport's box; its nodes are the running flow's, shared by address */
} VpScrollState;

static void vp_scroll_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    VpScrollState *s = st;

    v->tree(ctx, (void **)&s->job, NULL, 0, &layout_job_step_ops);
}

static int js_vp_scroll_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                             JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    VpScrollState *s = st;
    int magic = idl_step_magic(hdr);
    bool more;
    int rc;

    (void)cb_result; (void)out_cb; (void)out_argc;
    STEP_DISPATCH(VP_SCROLL_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(VP_SCROLL_READ);
    {
        const char *behavior = "auto";
        JSValue left, top, behavior_v = JS_UNDEFINED;

        DCHECK(magic == VP_SCROLL_ABSOLUTE || magic == VP_SCROLL_RELATIVE,
               "a CSSOM VIEW §4 scroll member was declared with a magic that is neither of the two algorithms — "
               "`scroll`/`scrollTo` is one and `scrollBy` is the other, and the magic is which");
        /* Steps 1 and 2, whose only difference is where `left` and `top` come from: "let the left dictionary
           member of options have the value x" makes the two-argument form's arguments those members. */
        if (argc >= 2) {
            DCHECK(argc == 2, "§4's scroll members declare two positions and the conversion converts no more than "
                              "a member lists, so a body seeing a third is a declaration that grew without this");
            left = JS_DupValue(ctx, argv[0]);
            top  = JS_DupValue(ctx, argv[1]);
        } else {
            DCHECK(argc == 1 && JS_IsObject(argv[0]),
                   "§4's scroll members reached their body at the dictionary arity with something that is not "
                   "the engine-built options object — `optional ScrollToOptions options = {}` means an omitted "
                   "argument is a dictionary carrying every member's default, which the argument machine "
                   "materializes");
            left = idl_dict_get(ctx, argv[0], "left");
            top  = idl_dict_get(ctx, argv[0], "top");
            /* §4's step 12 scrolls with "the scroll behavior being the value of the behavior dictionary
               member of options"; its IDL default `"auto"` is placed by the conversion for a page that wrote
               none. */
            behavior_v = idl_dict_get(ctx, argv[0], "behavior");
            if (concolic_is(behavior_v))
                DFAIL("CSSOM VIEW §4's scroll members were handed unknown external input as `behavior`, a Web "
                      "IDL enumeration whose worlds are its three values, so none may be picked. Build the fork "
                      "at §3.2.18's conversion: core/idl_args.h's idl_concolic_rule answers IDL_CONCOLIC_FORKS "
                      "for IDL_ENUM, and `idl_enum_fork` is the ask this step machine can make");
            DCHECK(JS_IsString(behavior_v),
                   "§4's ScrollToOptions declares `behavior` with the default \"auto\" and the conversion checks "
                   "it against the ScrollBehavior enumeration, so the converted dictionary carries a string");
            behavior = JS_ToCString(ctx, behavior_v);
            CHECK(behavior != NULL, "§4's ScrollBehavior keyword could not be read as a string");
        }
        DCHECKF(strlen(behavior) < sizeof s->behavior,
                "§4's ScrollBehavior keyword `%s` does not fit the state's copy — the enumeration's three values "
                "are what §3.2.18 admits, and each is shorter than the copy", behavior);
        strcpy(s->behavior, behavior);
        if (JS_IsString(behavior_v)) JS_FreeCString(ctx, behavior);
        JS_FreeValue(ctx, behavior_v);
        /* "Or the viewport's current scroll position on the x axis otherwise" is §4's `scrollX`/`scrollY` as §2
           requires it invoked, the internal one, so a page overriding `window.scrollX` changes nothing here. */
        s->x = scroll_request_member(ctx, left, viewport_window_scroll(ctx, /*vertical*/ false),
                                     magic == VP_SCROLL_RELATIVE);
        s->y = scroll_request_member(ctx, top,  viewport_window_scroll(ctx, /*vertical*/ true),
                                     magic == VP_SCROLL_RELATIVE);
        JS_FreeValue(ctx, left);
        JS_FreeValue(ctx, top);
        /* step 4 */
        if (!viewport_exists(ctx)) {
            *presult = vp_resolved_promise(ctx);
            return JS_STEP_DONE;
        }
        s->job = layout_job_new(LAYOUT_JOB_STEP);
        hdr->stage = VP_SCROLL_OPEN;
    }

    STEP_ARM(VP_SCROLL_OPEN);
    /* An entry the job's fork delivers to: its arm is recorded before the job is asked again. */
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    /* Steps 5 and 6, the viewport's width and height, are the measured box's extent; the box's overflow
       directions read the root's computed style on the job. */
    layout_job_enter(s->job);
    element_scrolling_measure_viewport(&s->m, ctx);
    layout_job_leave(s->job);
    if (layout_job_declined(s->job)) {
        rc = layout_job_fork(ctx, hdr, s->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    hdr->stage = VP_SCROLL_AREA;

    STEP_ARM(VP_SCROLL_AREA);
    /* An entry the job's fork delivers to: its arm is recorded before the drive runs again. */
    rc = layout_job_fork(ctx, hdr, s->job);
    if (rc) return rc;
    layout_job_enter(s->job);
    more = element_scrolling_measure_next(&s->m);
    if (!more) viewport_scroll_measured(&s->m, s->x, s->y, s->behavior);
    layout_job_leave(s->job);
    if (layout_job_declined(s->job)) {
        rc = layout_job_fork(ctx, hdr, s->job);
        return rc ? rc : JS_STEP_YIELD;
    }
    if (more) {
        layout_job_rested(s->job);
        return JS_STEP_YIELD;
    }
    *presult = vp_resolved_promise(ctx);
    return JS_STEP_DONE;
}

static const IdlStepDecl VP_SCROLL_STEP = {
    js_vp_scroll_step, sizeof(VpScrollState), vp_scroll_visit, NULL,
    "CSSOM VIEW §4 \"Extensions to the Window Interface\" scroll() / scrollTo() / scrollBy()",
    VP_SCROLL_STEPS
};

/* ONE OF §4's THREE SCROLL METHODS, DECLARED — the same argument shape and the same dictionary for all of
   them, differing only in which of the two algorithms the magic selects. */
static int vp_declare_scroll(JSContext *ctx, ViewportScrollKind kind)
{
    int id = idl_method_id_step(ctx, VP_SCROLL_ARGS, 2, VIEWPORT_SCROLL_TO_OPTIONS,
                                VIEWPORT_SCROLL_TO_OPTIONS_N, &VP_SCROLL_STEP, (int)kind);

    /* §3.7.7's PROMISE RETURN TYPE. It is what makes `window.scrollTo(0)` — §3.2.17 step 1 refusing a value
       that is not undefined, null or an Object — a REJECTED promise rather than a throw, which is what a page
       wrapping the call in `.catch` is relying on. */
    idl_returns_promise();
    /* The dictionary entry declares position 0 optional (`optional ScrollToOptions options = {}`), so
       `window.scrollTo()` is a legal call… */
    idl_optional_from(0);
    /* …AND THE NUMERIC ENTRY DECLARES NEITHER OF ITS TWO POSITIONS OPTIONAL, which is a different list of
       optionality values for the same declaration and is why §3.6 step 15.3 needs both. Without it
       `window.scrollTo(1, undefined)` would read `undefined` at position 1 as an ABSENT optional and default y
       to the current position, where the surviving entry owes it ToNumber(undefined) and then §3.2's
       normalized 0. */
    idl_overload_split_optional_from(2);
    return id;
}

/* ---- the declaration and the per-realm install ------------------------------------------------------------ */

static void viewport_install(JSContext *ctx)
{
    JSValue global, rec;
    int i;

    /* §13.1's record, built WITH the realm — running twice in one realm is asserted by realm_value_set, which
       is where the first record is standing. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "viewport: OOM building a realm's CSSOM VIEW §13.1 resize-steps record");
    JS_SetPropertyStr(ctx, rec, VP_RESIZE_RAN, JS_FALSE);
    JS_SetPropertyStr(ctx, rec, VP_RESIZE_W, JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, rec, VP_RESIZE_H, JS_NewFloat64(ctx, 0.0));
    realm_value_set(ctx, g_resize_slot, rec);

    /* THE SCROLL POSITION, built WITH the realm for the same reason and seeded at the origin — which is where
       a document that has not been scrolled is, and is a real starting VALUE rather than the derivation this
       component used to answer with. It is built eagerly rather than on the first read because a record minted
       inside whichever flow happened to touch it first would make that flow's object everyone's baseline —
       core/realm.h states the rule at the mechanism. */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "viewport: OOM building a realm's CSSOM VIEW §3.1 scroll-position record");
    JS_SetPropertyStr(ctx, rec, VP_POS_X, JS_NewFloat64(ctx, 0.0));
    JS_SetPropertyStr(ctx, rec, VP_POS_Y, JS_NewFloat64(ctx, 0.0));
    realm_value_set(ctx, g_scroll_slot, rec);

    /* Web IDL §3.7.3: Window is [Global], so its members are own properties of the GLOBAL OBJECT rather than of
       Window.prototype — see window.c, which states the same rule for the browsing-context half. */
    global = JS_GetGlobalObject(ctx);
    for (i = 0; i < VP_NAMES; i++)
        idl_install_replaceable(ctx, global, VP_NAME[i], js_vp_get, VP_MAGIC[i]);
    /* §4's three scroll METHODS, on the same target and by the same rule — Web IDL §3.7.3: Window is [Global],
       so its members are own properties of the global object rather than of a prototype. `scroll` and
       `scrollTo` share ONE declaration because §4 says the second acts as if the first were invoked, so the
       aliasing is in the install exactly as `pageXOffset`'s is above. */
    idl_install_method(ctx, global, "scroll", g_id_scroll);
    idl_install_method(ctx, global, "scrollTo", g_id_scroll);
    idl_install_method(ctx, global, "scrollBy", g_id_scroll_by);
    JS_FreeValue(ctx, global);
}

void viewport_init(JSContext *ctx)
{
    DCHECK(g_resize_slot == JS_INVALID_CLASS_ID, "viewport_init ran twice — the §13.1 record's slot is declared once per AGENT");
    g_resize_slot = realm_value_declare(ctx, "CSSOM VIEW §13.1 the viewport as the resize steps last saw it");
    g_scroll_slot = realm_value_declare(ctx, "CSSOM VIEW §3.1 the viewport's current scroll position");
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. It is the slot this init's own
       latch consults, so a release that kept it would hand a second agent a component reporting itself
       declared and holding a realm-value id from a runtime that no longer exists. */
    agent_state_realm_slot("viewport", &g_resize_slot,
                           "CSSOM VIEW §13.1 Resizing viewports' realm-value slot for the viewport as the resize steps "
                           "last saw it, and this component's declaration latch");
    agent_state_realm_slot("viewport", &g_scroll_slot,
                           "CSSOM VIEW §3.1 Scrolling's realm-value slot for the viewport's current scroll position — "
                           "the state §3.1's perform a scroll writes and §4's `scrollX`/`scrollY` read");
    /* §4's three scroll members, declared ONCE PER AGENT like every other member declaration — the install
       above is per realm and the declaration is not. */
    g_id_scroll    = vp_declare_scroll(ctx, VP_SCROLL_ABSOLUTE);
    g_id_scroll_by = vp_declare_scroll(ctx, VP_SCROLL_RELATIVE);
    realm_declare_intrinsic(viewport_install);
}

void viewport_free(void)
{
    /* The records are the REALMS' — each is released with its context. What the agent holds is the slots, and a
       slot id is a class id in a runtime that is going away with it. */
    g_resize_slot = JS_INVALID_CLASS_ID;
    g_scroll_slot = JS_INVALID_CLASS_ID;
    /* §4's three scroll members' ids are the AGENT's too, and the pool they live in goes with the runtime. A
       component that kept one would hand a second agent a member id from a runtime that no longer exists,
       which is the same failure the slots above are reset for. */
    g_id_scroll = -1;
    g_id_scroll_by = -1;
}
