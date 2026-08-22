/* CSS Cascade §6.1's sorting order and §7.3's roll-back keywords. See css_cascade.h for why the cascade is a
   sorted list rather than a sequence of layers asked in turn, what the level is made of, and why the three
   roll-backs resolve here instead of in §7's defaulting step. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "core/css/css_cascade.h"
#include "core/css/css_defaulting.h"
#include "core/css/css_layer_order.h"

typedef struct {
    char               *value;            /* the declared value, OWNED */
    const CssLayerNode *layer;            /* §6.4.3's node, or NULL for §6.1's implicit final layer */
    uint32_t            specificity;
    uint32_t            seq;              /* §6.1's Order of Appearance, and the rule's identity */
    unsigned            origin;           /* a CssOrigin */
    bool                important;
    bool                element_attached;
    /* Whether §7.3's roll-back has removed this declaration from the cascade being re-run. It starts true and
       only ever becomes false, which is what makes the loop below terminate without a bound: every round
       removes at least the declaration that asked for the roll-back. */
    bool                live;
} CssCascadeDecl;

struct CssCascade {
    CssCascadeDecl *v;
    unsigned        n, cap;
    CssLayerOrder  *layers;
};

CssCascade *css_cascade_create(CssLayerOrder *layers)
{
    CssCascade *c = calloc(1, sizeof(*c));

    DCHECK(layers != NULL,
           "a cascade was built over no §6.4.3 layer order. Every declaration has a cascade layer — §6.1 puts "
           "the ones with no explicit layer in an implicit final one — so the order is not optional even for a "
           "document with no `@layer` rule in it");
    CHECK(c != NULL, "cssom: OOM building CSS Cascade §6's cascade");
    c->layers = layers;
    return c;
}

void css_cascade_free(CssCascade *c)
{
    unsigned i;

    if (!c) return;
    for (i = 0; i < c->n; i++) free(c->v[i].value);
    free(c->v);
    free(c);
}

void css_cascade_add(CssCascade *c, CssOrigin origin, bool important, bool element_attached,
                     const CssLayerNode *layer, uint32_t specificity, uint32_t seq, const char *value)
{
    CssCascadeDecl *d;

    DCHECK(c != NULL, "a declared value was added to no cascade");
    DCHECK(value != NULL,
           "a declaration with NO VALUE reached §6.1's sort. A declaration whose value is the empty string is a "
           "value (CSS Syntax admits one, which is why §6.6's serialization step is conditional); the absence "
           "of one is a collector that has not decided whether the property is declared at all");
    DCHECK(!(element_attached && origin != CSS_ORIGIN_AUTHOR),
           "§6.1's ELEMENT-ATTACHED criterion was claimed for a declaration outside the author origin. The "
           "criterion is about a style ATTRIBUTE, which [CSSSTYLEATTR] puts in the author origin; a UA default "
           "and a §6.5 presentational hint are computed per element and are still MAPPED declarations, not "
           "attached ones");
    if (c->n == c->cap) {
        unsigned cap = c->cap ? c->cap * 2 : 8;
        CssCascadeDecl *grown = realloc(c->v, (size_t)cap * sizeof(*grown));

        CHECK(grown != NULL, "cssom: OOM collecting a declaration into CSS Cascade §6's cascade");
        c->v = grown;
        c->cap = cap;
    }
    d = &c->v[c->n++];
    d->value = strdup(value);
    CHECK(d->value != NULL, "cssom: OOM copying a declared value into the cascade — a dropped declaration is a "
                            "cascade that answers as if the rule had never been written");
    d->layer = layer;
    d->specificity = specificity;
    d->seq = seq;
    d->origin = (unsigned)origin;
    d->important = important;
    d->element_attached = element_attached;
    d->live = true;
}

/* §6.1's ORIGIN AND IMPORTANCE, as a WEIGHT — the section's list read bottom-up, so a larger number wins. The
   list is, in descending order of precedence: "Transition declarations. Important user agent declarations.
   Important user declarations. Important author declarations. Animation declarations. Normal author
   declarations. Normal user declarations. Normal user agent declarations." §6.5 inserts one more, the author
   presentational hint origin, "between the regular user origin and the author origin".
   THE UNOCCUPIED BANDS ARE NAMED, because their numbers are what put the occupied ones where §6.1 puts them:
   the gap between important author (5) and normal author (3) is the ANIMATION origin, and it is the gap
   §7.3.5's note is about ("the animation origin is not collapsed with the author origin for this purpose as it
   is for revert, and thus effectively forms its own cascade layer"). */
enum {
    CASCADE_BAND_NORMAL_UA = 0,
    CASCADE_BAND_NORMAL_USER,             /* no user style sheet in this engine */
    CASCADE_BAND_NORMAL_HINT,             /* §6.5, "between the regular user origin and the author origin" */
    CASCADE_BAND_NORMAL_AUTHOR,
    CASCADE_BAND_ANIMATION,               /* [css-animations-1]; nothing here generates one */
    CASCADE_BAND_IMPORTANT_AUTHOR,
    CASCADE_BAND_IMPORTANT_USER,
    CASCADE_BAND_IMPORTANT_UA,
    CASCADE_BAND_TRANSITION,              /* [css-transitions-1]; nothing here generates one */
    CASCADE_BAND_N
};

static unsigned cascade_band(unsigned origin, bool important)
{
    if (important) {
        /* §6.3: "Author and user style sheets may contain important declarations ... User-agent style sheets
           may also contain important declarations." A PRESENTATIONAL HINT cannot: §6.5 places that origin in
           the normal ordering only, and HTML §15.2's hints are markup mappings with no `!important` to carry. */
        DCHECK(origin != CSS_ORIGIN_PRESENTATIONAL_HINT,
               "an IMPORTANT declaration claimed §6.5's author presentational hint origin. That origin is a "
               "translation of document-language markup and has no importance to declare — a hint that needs "
               "one is a hint HTML §15.2 puts in the UA origin instead");
        if (origin == CSS_ORIGIN_UA) return CASCADE_BAND_IMPORTANT_UA;
        DCHECK(origin == CSS_ORIGIN_AUTHOR, "an important declaration claimed an origin §6.2 does not define");
        return CASCADE_BAND_IMPORTANT_AUTHOR;
    }
    if (origin == CSS_ORIGIN_AUTHOR) return CASCADE_BAND_NORMAL_AUTHOR;
    if (origin == CSS_ORIGIN_PRESENTATIONAL_HINT) return CASCADE_BAND_NORMAL_HINT;
    DCHECK(origin == CSS_ORIGIN_UA, "a normal declaration claimed an origin §6.2 does not define");
    return CASCADE_BAND_NORMAL_UA;
}

/* The three criteria §6.1 states ABOVE Specificity, as one monotone number. See css_cascade.h for the shape and
   for why it has to be an ordered scalar rather than three compared fields: §7.3.5's roll-back is defined over
   a RANGE of it. */
static uint64_t cascade_level(const CssCascade *c, unsigned origin, bool important, bool element_attached,
                              const CssLayerNode *layer)
{
    unsigned nl = css_layer_order_count(c->layers);
    /* §6.1's Layers step: "any declaration not assigned to an explicit layer is added to an implicit final
       layer", which §6.4.3's order numbers LAST. */
    unsigned li = layer ? css_layer_order_index(c->layers, layer) : nl - 1;
    unsigned key;

    DCHECK(li < nl, "a declaration's cascade layer is not in the §6.4.3 order this cascade sorts by");
    /* §6.1: "for normal rules the declaration whose cascade layer is LATEST in the layer order wins, and for
       important rules the declaration whose cascade layer is EARLIEST wins." One field, read from both ends. */
    key = important ? (nl - 1 - li) : li;
    return ((uint64_t)cascade_band(origin, important) * 2u + (element_attached ? 1u : 0u)) * (uint64_t)nl + key;
}

static uint64_t cascade_decl_level(const CssCascade *c, const CssCascadeDecl *d)
{
    return cascade_level(c, d->origin, d->important, d->element_attached, d->layer);
}

/* §6.1 entire, over two declarations: the level above, then "the declaration with the highest specificity
   wins", then "the last declaration in document order wins". */
static bool cascade_beats(const CssCascade *c, const CssCascadeDecl *a, const CssCascadeDecl *b)
{
    uint64_t la = cascade_decl_level(c, a), lb = cascade_decl_level(c, b);

    if (la != lb) return la > lb;
    if (a->specificity != b->specificity) return a->specificity > b->specificity;
    DCHECK(a->seq != b->seq,
           "two declarations reached §6.1's ORDER OF APPEARANCE holding the same position. The position is the "
           "collector's document-order counter and it is bumped per rule, and §6.6's collapse leaves one "
           "declaration per property per rule — so a tie means two declarations were added under one counter "
           "value and the cascade has no way to say which is last");
    return a->seq > b->seq;
}

/* The live declaration §6.1 sorts highest, or NULL when the cascade holds none. */
static CssCascadeDecl *cascade_winner(CssCascade *c)
{
    CssCascadeDecl *best = NULL;
    unsigned i;

    for (i = 0; i < c->n; i++) {
        if (!c->v[i].live) continue;
        if (!best || cascade_beats(c, &c->v[i], best)) best = &c->v[i];
    }
    return best;
}

/* §7.3.4's `revert` for a declaration in the author origin: "Rolls back the cascaded value to the user level,
   so that the specified value is calculated as if no author-origin rules were specified for this property on
   this element. For the purpose of revert, this origin includes the Animation origin." §6.5 adds the other half
   — the author presentational hint origin "for the purpose of the revert keyword (but not for the revert-layer
   keyword) ... is considered part of the author origin" — so both go, in both importances. */
static unsigned cascade_revert_origin(CssCascade *c)
{
    unsigned killed = 0, i;

    for (i = 0; i < c->n; i++) {
        if (!c->v[i].live) continue;
        if (c->v[i].origin != CSS_ORIGIN_AUTHOR && c->v[i].origin != CSS_ORIGIN_PRESENTATIONAL_HINT) continue;
        c->v[i].live = false;
        killed++;
    }
    return killed;
}

/* §7.3.5's `revert-layer`: "the cascaded value is rolled back to the earlier layer, so that the specified value
   is calculated as if no rules were specified in the current cascade layer—or between its normal and important
   levels in the cascade—for this property on this element." The two levels are the winner's own, computed with
   the importance flipped, and everything at or between them goes.
   THE ELEMENT-ATTACHED CARVE-OUT IS THE SECTION'S NEXT SENTENCE: "For revert-layer in important element-
   attached styles, however, it only reverts the element-attached styles and the intervening animation origin,
   and not any of the intervening author-origin important rules." */
static unsigned cascade_revert_layer(CssCascade *c, const CssCascadeDecl *w)
{
    unsigned killed = 0, i;
    uint64_t lo, hi;
    bool attached_only = w->important && w->element_attached;

    lo = cascade_level(c, w->origin, false, w->element_attached, w->layer);
    hi = cascade_level(c, w->origin, true, w->element_attached, w->layer);
    DCHECK(lo <= hi,
           "§7.3.5's normal level sorts ABOVE its important level. §6.3 inverts the order of precedence for an "
           "important declaration — that is what the annotation is for — so a band function that puts the "
           "normal one higher has read §6.1's origin list upside down");
    for (i = 0; i < c->n; i++) {
        if (!c->v[i].live) continue;
        if (attached_only) {
            if (!c->v[i].element_attached) continue;
        } else {
            uint64_t l = cascade_decl_level(c, &c->v[i]);

            if (l < lo || l > hi) continue;
        }
        c->v[i].live = false;
        killed++;
    }
    return killed;
}

/* §7.3.6's `revert-rule`: "the cascaded value is rolled back such that the specified value is calculated as if
   the current style rule had not been present at all." The rule is named by the collector's per-rule counter,
   which is also §6.1's order of appearance — one number because §6.6's collapse makes a rule contribute at
   most one declaration per property, so "this rule's declarations of this property" and "this declaration" are
   the same set. A declaration attached to the ELEMENT is in no style rule; the block it is in is what the
   section's sentence removes, and for a style rule that block IS the rule. */
static unsigned cascade_revert_rule(CssCascade *c, const CssCascadeDecl *w)
{
    unsigned killed = 0, i;
    uint32_t rule = w->seq;

    for (i = 0; i < c->n; i++) {
        if (!c->v[i].live || c->v[i].seq != rule) continue;
        c->v[i].live = false;
        killed++;
    }
    return killed;
}

char *css_cascade_value(CssCascade *c)
{
    DCHECK(c != NULL, "a cascaded value was asked of no cascade");
    for (;;) {
        CssCascadeDecl *w = cascade_winner(c);
        unsigned killed = 0;
        char *out;

        /* §7.1 and §7.2's "unless the cascade results in a value" — which is also where a roll-back that ran
           off the bottom of the cascade lands, and is a real answer at both. */
        if (!w) return NULL;
        switch (css_rollback_keyword(w->value)) {
        case CSS_ROLLBACK_NONE:
            out = strdup(w->value);
            CHECK(out != NULL, "cssom: OOM answering CSS Cascade §6's cascaded value");
            return out;
        case CSS_ROLLBACK_ORIGIN:
            /* §7.3.4's first arm, stated for the origin that has nothing under it: "user-agent origin:
               Equivalent to unset." §7.3.3's keyword is exactly what §7's defaulting step resolves, so it is
               the answer rather than a fourth thing this component computes. */
            if (w->origin == CSS_ORIGIN_UA) {
                out = strdup("unset");
                CHECK(out != NULL, "cssom: OOM answering §7.3.4's user-agent-origin `revert`");
                return out;
            }
            killed = cascade_revert_origin(c);
            break;
        case CSS_ROLLBACK_LAYER: killed = cascade_revert_layer(c, w); break;
        case CSS_ROLLBACK_RULE:  killed = cascade_revert_rule(c, w);  break;
        default:
            DFAIL("§7.3's roll-back classification answered a kind this switch has no arm for — "
                  "core/css/css_defaulting.h's enumeration and this switch are one list and have come apart");
            return NULL;
        }
        /* THE LOOP NEEDS NO BOUND BECAUSE IT NEEDS NO BOUND: every arm above removes at least the declaration
           that asked for the roll-back — `revert` removes its own origin, `revert-layer` its own level, and
           `revert-rule` its own rule — so the live set strictly shrinks and the next round has strictly fewer
           declarations to sort. A round that removed nothing would spin, which is why it is asserted here and
           not counted. */
        DCHECK(killed >= 1,
               "a §7.3 roll-back removed NOTHING from the cascade, so the same declaration wins again. Each of "
               "the three is defined to remove the part of the cascade the reverting declaration is IN — its "
               "origin, its layer's level range, its rule — so a round that removes nothing means the exclusion "
               "does not cover the winner it was computed from");
        if (!killed) return NULL;
    }
}
