/* @LOGICAL — css-writing-modes-4 §6 "Abstract Box Terminology"'s mapping, and css-logical-1 §4's groups.
   The contract, and why this is at the cascade rather than at the used value, is in css_logical.h. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "core/css/css_computed_value.h"
#include "core/css/css_logical.h"

/* css-writing-modes-4 §3.2 "Block Flow Direction: the writing-mode property"'s five values, in the order its
   `Value:` line writes them. `sideways-rl` has its own row even though css-writing-modes-4 §6.4
   "Abstract-to-Physical Mappings" gives it and `vertical-rl` one column: a shared column is a fact about that
   table's cells and not about the property, and writing it out is what lets `logical_init`'s walk read every
   value the cascade can produce rather than a set this file decided was interesting. */
typedef enum { LG_WM_HORIZONTAL_TB = 0, LG_WM_VERTICAL_RL, LG_WM_VERTICAL_LR, LG_WM_SIDEWAYS_RL,
               LG_WM_SIDEWAYS_LR, LG_WM_N } LgWritingMode;

/* css-writing-modes-4 §2.1 "Specifying Directionality: the direction property"'s two values. The one read
   here is §6.4's own — "based on the USED direction and writing-mode" — which `logical_used_direction`
   derives. */
typedef enum { LG_DIR_LTR = 0, LG_DIR_RTL, LG_DIR_N } LgDirection;

/* The four PHYSICAL sides and the four FLOW-RELATIVE ones (css-writing-modes-4 §6.2 "Flow-relative
   Directions"), then the two PHYSICAL dimensions and the two ABSTRACT ones (css-writing-modes-4 §6.1
   "Abstract Dimensions"). A row's role decides its MAPPING LOGIC outright — a physical role is a physical
   property and an abstract one is flow-relative — so the table has no separate flag that could disagree with
   the role beside it. */
typedef enum {
    LG_TOP = 0, LG_RIGHT, LG_BOTTOM, LG_LEFT,
    LG_BLOCK_START, LG_BLOCK_END, LG_INLINE_START, LG_INLINE_END,
    LG_HORIZONTAL, LG_VERTICAL,
    LG_BLOCK_DIM, LG_INLINE_DIM,
} LgRole;

#define LG_IS_PHYSICAL(r) ((r) <= LG_LEFT || (r) == LG_HORIZONTAL || (r) == LG_VERTICAL)
#define LG_IS_SIDE(r)     ((r) <= LG_INLINE_END)
#define LG_ABSTRACT_SIDE_AT(r) ((unsigned)((r) - LG_BLOCK_START))   /* 0..3, the side table's column index */

/* css-writing-modes-4 §6.4 "Abstract-to-Physical Mappings" — THE TABLE, transcribed cell by cell from its
   eight columns. §6.4's own lead-in is "The following table summarizes the abstract-to-physical mappings
   (based on the used direction and writing-mode)".
   THE COLUMNS ARE NOT ALL DISTINCT IN THE DOCUMENT and they are all written out here. §6.4 heads one column
   `vertical-rl, sideways-rl` and spans `block-start`'s cell across `vertical-lr` and `sideways-lr`; a
   transcription that re-created those spans would be a second encoding of the same fact, and the thing a
   reader needs to check is a CELL against the document rather than a span against a span. */
static const LgRole LG_SIDE[LG_WM_N][LG_DIR_N][4] = {
    /*                     block-start  block-end   inline-start  inline-end */
    [LG_WM_HORIZONTAL_TB][LG_DIR_LTR] = { LG_TOP,    LG_BOTTOM, LG_LEFT,   LG_RIGHT  },
    [LG_WM_HORIZONTAL_TB][LG_DIR_RTL] = { LG_TOP,    LG_BOTTOM, LG_RIGHT,  LG_LEFT   },
    [LG_WM_VERTICAL_RL]  [LG_DIR_LTR] = { LG_RIGHT,  LG_LEFT,   LG_TOP,    LG_BOTTOM },
    [LG_WM_VERTICAL_RL]  [LG_DIR_RTL] = { LG_RIGHT,  LG_LEFT,   LG_BOTTOM, LG_TOP    },
    [LG_WM_VERTICAL_LR]  [LG_DIR_LTR] = { LG_LEFT,   LG_RIGHT,  LG_TOP,    LG_BOTTOM },
    [LG_WM_VERTICAL_LR]  [LG_DIR_RTL] = { LG_LEFT,   LG_RIGHT,  LG_BOTTOM, LG_TOP    },
    [LG_WM_SIDEWAYS_RL]  [LG_DIR_LTR] = { LG_RIGHT,  LG_LEFT,   LG_TOP,    LG_BOTTOM },
    [LG_WM_SIDEWAYS_RL]  [LG_DIR_RTL] = { LG_RIGHT,  LG_LEFT,   LG_BOTTOM, LG_TOP    },
    [LG_WM_SIDEWAYS_LR]  [LG_DIR_LTR] = { LG_LEFT,   LG_RIGHT,  LG_BOTTOM, LG_TOP    },
    [LG_WM_SIDEWAYS_LR]  [LG_DIR_RTL] = { LG_LEFT,   LG_RIGHT,  LG_TOP,    LG_BOTTOM },
};

/* §6.4's first two rows. They carry ONE cell per writing mode in the document — `block-size` reads `height`
   across both `horizontal-tb` columns and `width` across the six others — which is css-writing-modes-4 §6.1
   "Abstract Dimensions" stated again: "inline size ... refers to the physical width (horizontal dimension) in
   horizontal writing modes, and to the physical height (vertical dimension) in vertical writing modes",
   with no mention of a direction. The direction index is carried anyway so that `logical_init` can ASSERT the
   independence rather than this file assuming it. */
static const LgRole LG_DIM[LG_WM_N][LG_DIR_N][2] = {
    /*                     block-size     inline-size */
    [LG_WM_HORIZONTAL_TB][LG_DIR_LTR] = { LG_VERTICAL,   LG_HORIZONTAL },
    [LG_WM_HORIZONTAL_TB][LG_DIR_RTL] = { LG_VERTICAL,   LG_HORIZONTAL },
    [LG_WM_VERTICAL_RL]  [LG_DIR_LTR] = { LG_HORIZONTAL, LG_VERTICAL   },
    [LG_WM_VERTICAL_RL]  [LG_DIR_RTL] = { LG_HORIZONTAL, LG_VERTICAL   },
    [LG_WM_VERTICAL_LR]  [LG_DIR_LTR] = { LG_HORIZONTAL, LG_VERTICAL   },
    [LG_WM_VERTICAL_LR]  [LG_DIR_RTL] = { LG_HORIZONTAL, LG_VERTICAL   },
    [LG_WM_SIDEWAYS_RL]  [LG_DIR_LTR] = { LG_HORIZONTAL, LG_VERTICAL   },
    [LG_WM_SIDEWAYS_RL]  [LG_DIR_RTL] = { LG_HORIZONTAL, LG_VERTICAL   },
    [LG_WM_SIDEWAYS_LR]  [LG_DIR_LTR] = { LG_HORIZONTAL, LG_VERTICAL   },
    [LG_WM_SIDEWAYS_LR]  [LG_DIR_RTL] = { LG_HORIZONTAL, LG_VERTICAL   },
};

/* THE LONGHANDS, one row per property, grouped by the css-logical-1 §4 "Flow-Relative Box Model Properties"
   group its own definition names. Every group holds exactly one row per role of its kind — four physical
   sides and four abstract ones, or two physical dimensions and two abstract ones — which is what makes the
   partner lookup a total function and is what `logical_init` asserts.
   THE SHORTHANDS ARE ABSENT BY THE SPEC'S OWN PARENTHESIS: css-logical-1 §4 defines the term as "Each set of
   parallel flow-relative properties and physical properties (IGNORING SHORTHAND PROPERTIES) related by
   setting equivalent styles on the various sides or dimensions of a box", so `margin-block` and `margin` are
   not members and the cascade never asks about one (core/css/css_style_declaration.c's cascaded-value entry
   asserts it is over longhands only). */
static const struct { const char *name; CssLogicalGroup group; LgRole role; } LG_PROP[] = {
    /* css-logical-1 §4.2 "Flow-Relative Margins" — `Logical property group: margin`. */
    { "margin-top",            CSS_LOGICAL_GROUP_MARGIN, LG_TOP          },
    { "margin-right",          CSS_LOGICAL_GROUP_MARGIN, LG_RIGHT        },
    { "margin-bottom",         CSS_LOGICAL_GROUP_MARGIN, LG_BOTTOM       },
    { "margin-left",           CSS_LOGICAL_GROUP_MARGIN, LG_LEFT         },
    { "margin-block-start",    CSS_LOGICAL_GROUP_MARGIN, LG_BLOCK_START  },
    { "margin-block-end",      CSS_LOGICAL_GROUP_MARGIN, LG_BLOCK_END    },
    { "margin-inline-start",   CSS_LOGICAL_GROUP_MARGIN, LG_INLINE_START },
    { "margin-inline-end",     CSS_LOGICAL_GROUP_MARGIN, LG_INLINE_END   },

    /* css-logical-1 §4.4 "Flow-Relative Padding" — `Logical property group: padding`. */
    { "padding-top",           CSS_LOGICAL_GROUP_PADDING, LG_TOP          },
    { "padding-right",         CSS_LOGICAL_GROUP_PADDING, LG_RIGHT        },
    { "padding-bottom",        CSS_LOGICAL_GROUP_PADDING, LG_BOTTOM       },
    { "padding-left",          CSS_LOGICAL_GROUP_PADDING, LG_LEFT         },
    { "padding-block-start",   CSS_LOGICAL_GROUP_PADDING, LG_BLOCK_START  },
    { "padding-block-end",     CSS_LOGICAL_GROUP_PADDING, LG_BLOCK_END    },
    { "padding-inline-start",  CSS_LOGICAL_GROUP_PADDING, LG_INLINE_START },
    { "padding-inline-end",    CSS_LOGICAL_GROUP_PADDING, LG_INLINE_END   },

    /* css-logical-1 §4.5.1 "Flow-Relative Border Widths" — `Logical property group: border-width`. */
    { "border-top-width",           CSS_LOGICAL_GROUP_BORDER_WIDTH, LG_TOP          },
    { "border-right-width",         CSS_LOGICAL_GROUP_BORDER_WIDTH, LG_RIGHT        },
    { "border-bottom-width",        CSS_LOGICAL_GROUP_BORDER_WIDTH, LG_BOTTOM       },
    { "border-left-width",          CSS_LOGICAL_GROUP_BORDER_WIDTH, LG_LEFT         },
    { "border-block-start-width",   CSS_LOGICAL_GROUP_BORDER_WIDTH, LG_BLOCK_START  },
    { "border-block-end-width",     CSS_LOGICAL_GROUP_BORDER_WIDTH, LG_BLOCK_END    },
    { "border-inline-start-width",  CSS_LOGICAL_GROUP_BORDER_WIDTH, LG_INLINE_START },
    { "border-inline-end-width",    CSS_LOGICAL_GROUP_BORDER_WIDTH, LG_INLINE_END   },

    /* css-logical-1 §4.5.2 "Flow-Relative Border Styles" — `Logical property group: border-style`. */
    { "border-top-style",           CSS_LOGICAL_GROUP_BORDER_STYLE, LG_TOP          },
    { "border-right-style",         CSS_LOGICAL_GROUP_BORDER_STYLE, LG_RIGHT        },
    { "border-bottom-style",        CSS_LOGICAL_GROUP_BORDER_STYLE, LG_BOTTOM       },
    { "border-left-style",          CSS_LOGICAL_GROUP_BORDER_STYLE, LG_LEFT         },
    { "border-block-start-style",   CSS_LOGICAL_GROUP_BORDER_STYLE, LG_BLOCK_START  },
    { "border-block-end-style",     CSS_LOGICAL_GROUP_BORDER_STYLE, LG_BLOCK_END    },
    { "border-inline-start-style",  CSS_LOGICAL_GROUP_BORDER_STYLE, LG_INLINE_START },
    { "border-inline-end-style",    CSS_LOGICAL_GROUP_BORDER_STYLE, LG_INLINE_END   },

    /* css-logical-1 §4.5.3 "Flow-Relative Border Colors" — `Logical property group: border-color`. */
    { "border-top-color",           CSS_LOGICAL_GROUP_BORDER_COLOR, LG_TOP          },
    { "border-right-color",         CSS_LOGICAL_GROUP_BORDER_COLOR, LG_RIGHT        },
    { "border-bottom-color",        CSS_LOGICAL_GROUP_BORDER_COLOR, LG_BOTTOM       },
    { "border-left-color",          CSS_LOGICAL_GROUP_BORDER_COLOR, LG_LEFT         },
    { "border-block-start-color",   CSS_LOGICAL_GROUP_BORDER_COLOR, LG_BLOCK_START  },
    { "border-block-end-color",     CSS_LOGICAL_GROUP_BORDER_COLOR, LG_BLOCK_END    },
    { "border-inline-start-color",  CSS_LOGICAL_GROUP_BORDER_COLOR, LG_INLINE_START },
    { "border-inline-end-color",    CSS_LOGICAL_GROUP_BORDER_COLOR, LG_INLINE_END   },

    /* css-logical-1 §4.3 "Flow-Relative Offsets" — `Logical property group: inset`. The physical members are
       the four BARE names: §4.3's own sentence is "The top, left, bottom, right physical properties, their
       inset-block-start ... flow-relative correspondents, and the inset-block, inset-inline, and inset
       shorthands, are collectively known as the inset properties." */
    { "top",                   CSS_LOGICAL_GROUP_INSET, LG_TOP          },
    { "right",                 CSS_LOGICAL_GROUP_INSET, LG_RIGHT        },
    { "bottom",                CSS_LOGICAL_GROUP_INSET, LG_BOTTOM       },
    { "left",                  CSS_LOGICAL_GROUP_INSET, LG_LEFT         },
    { "inset-block-start",     CSS_LOGICAL_GROUP_INSET, LG_BLOCK_START  },
    { "inset-block-end",       CSS_LOGICAL_GROUP_INSET, LG_BLOCK_END    },
    { "inset-inline-start",    CSS_LOGICAL_GROUP_INSET, LG_INLINE_START },
    { "inset-inline-end",      CSS_LOGICAL_GROUP_INSET, LG_INLINE_END   },

    /* css-logical-1 §4.1 "Logical Height and Logical Width" — three groups on one line of that section,
       `size`, `min-size` and `max-size`, each a DIMENSION mapping rather than a side one: "These properties
       correspond to the height and width properties. The mapping depends on the element's writing-mode",
       with no direction named. */
    { "width",                 CSS_LOGICAL_GROUP_SIZE, LG_HORIZONTAL },
    { "height",                CSS_LOGICAL_GROUP_SIZE, LG_VERTICAL   },
    { "inline-size",           CSS_LOGICAL_GROUP_SIZE, LG_INLINE_DIM },
    { "block-size",            CSS_LOGICAL_GROUP_SIZE, LG_BLOCK_DIM  },

    { "min-width",             CSS_LOGICAL_GROUP_MIN_SIZE, LG_HORIZONTAL },
    { "min-height",            CSS_LOGICAL_GROUP_MIN_SIZE, LG_VERTICAL   },
    { "min-inline-size",       CSS_LOGICAL_GROUP_MIN_SIZE, LG_INLINE_DIM },
    { "min-block-size",        CSS_LOGICAL_GROUP_MIN_SIZE, LG_BLOCK_DIM  },

    { "max-width",             CSS_LOGICAL_GROUP_MAX_SIZE, LG_HORIZONTAL },
    { "max-height",            CSS_LOGICAL_GROUP_MAX_SIZE, LG_VERTICAL   },
    { "max-inline-size",       CSS_LOGICAL_GROUP_MAX_SIZE, LG_INLINE_DIM },
    { "max-block-size",        CSS_LOGICAL_GROUP_MAX_SIZE, LG_BLOCK_DIM  },

    /* css-overflow-3 §3.1 "Managing Overflow: the overflow-x, overflow-y, and overflow properties" carries
       all four on one `Name:` line under `Logical property group: overflow`. `overflow-x` is the HORIZONTAL
       axis and `overflow-inline` the inline one, so the mapping is the dimension table's. */
    { "overflow-x",            CSS_LOGICAL_GROUP_OVERFLOW, LG_HORIZONTAL },
    { "overflow-y",            CSS_LOGICAL_GROUP_OVERFLOW, LG_VERTICAL   },
    { "overflow-inline",       CSS_LOGICAL_GROUP_OVERFLOW, LG_INLINE_DIM },
    { "overflow-block",        CSS_LOGICAL_GROUP_OVERFLOW, LG_BLOCK_DIM  },
};
#define LG_N(a) ((unsigned)(sizeof(a) / sizeof((a)[0])))

static const char *const LG_WM_NAME[LG_WM_N] = {
    "horizontal-tb", "vertical-rl", "vertical-lr", "sideways-rl", "sideways-lr",
};

static LgWritingMode logical_writing_mode(lxb_dom_element_t *el)
{
    char *wm = css_computed_value(el, "writing-mode");
    unsigned i;

    DCHECK(wm != NULL, "the cascade produced no computed `writing-mode` — css-writing-modes-4 §3.2 \"Block "
                       "Flow Direction: the writing-mode property\" gives it an `Initial:` line of "
                       "`horizontal-tb` and lexbor's registry carries the row, so the last layer of the "
                       "cascade always answers and a NULL here is a cascade that stopped early");
    for (i = 0; i < LG_WM_N; i++)
        if (strcmp(LG_WM_NAME[i], wm) == 0) { free(wm); return (LgWritingMode)i; }
    free(wm);
    /* A GUARD AND NOT A GAP. The operand is enumerated by THIS codebase — lexbor's property registry carries
       `writing-mode`'s `Value:` line and its five keywords, and css-writing-modes-4 §3.2 declares exactly
       those five — so a sixth value means the cascade produced something the registry cannot have parsed. */
    DFAIL("a computed `writing-mode` is none of css-writing-modes-4 §3.2 \"Block Flow Direction: the "
          "writing-mode property\"'s five values. Its `Value:` line is `horizontal-tb | vertical-rl | "
          "vertical-lr | sideways-rl | sideways-lr` and lexbor's registry enumerates the same five, so this "
          "is a cascaded value that no declaration of this property could have produced");
    return LG_WM_HORIZONTAL_TB;
}

/* css-writing-modes-4 §6.4's USED direction, which is not always the computed one. §6.4's own closing Note:
   "The used direction depends on the computed writing-mode and text-orientation: in vertical writing modes, a
   text-orientation value of upright forces the used direction to ltr."
   THE VERTICAL ARM CRASHES AND THE HORIZONTAL ONE IS COMPLETE, which is where css-writing-modes-4 §6.2's own
   split pays: only an INLINE-axis question ever reaches here, so a `margin-top` or a `block-size` resolves in
   every one of the five writing modes without it. */
static LgDirection logical_used_direction(lxb_dom_element_t *el, LgWritingMode wm)
{
    char *dir;
    LgDirection out;

    if (wm != LG_WM_HORIZONTAL_TB)
        DFAIL("css-writing-modes-4 §6.4 \"Abstract-to-Physical Mappings\"'s USED direction was asked for on "
              "an element in a VERTICAL writing mode, and its Note makes that a question about "
              "`text-orientation` — \"in vertical writing modes, a text-orientation value of upright forces "
              "the used direction to ltr\" — which this engine has no computed value for: "
              "core/css/css_computed_value.c's `css_computed_models` carries no row for it, so asking would "
              "abort one component over. BUILD that row: css-writing-modes-4 §5.1 \"Orienting Text: the "
              "text-orientation property\" gives `Value: mixed | upright | sideways`, `Initial: mixed`, "
              "`Inherited: yes` and `Computed value: specified value`, and lexbor's registry carries the "
              "property — so it is ONE row of css_computed_models' as-specified arm plus one of "
              "css_shorthand_complete_for, which is the same row core/css/css_computed_value.c's `ch`-unit "
              "advance-direction crash already names. The BLOCK axis needs none of this and is answered for "
              "every writing mode above");
    dir = css_computed_value(el, "direction");
    DCHECK(dir != NULL, "the cascade produced no computed `direction` — css-writing-modes-4 §2.1 "
                        "\"Specifying Directionality: the direction property\" gives it an `Initial:` line "
                        "of `ltr` and lexbor's registry carries the row, so the last layer always answers");
    if (strcmp(dir, "ltr") == 0) out = LG_DIR_LTR;
    else if (strcmp(dir, "rtl") == 0) out = LG_DIR_RTL;
    else {
        /* A GUARD: `direction`'s `Value:` line is the two keywords and lexbor's registry enumerates them. */
        DFAIL("a computed `direction` is neither `ltr` nor `rtl`. css-writing-modes-4 §2.1 \"Specifying "
              "Directionality: the direction property\" gives the property those two values and nothing "
              "else, and lexbor's registry enumerates the same pair");
        out = LG_DIR_LTR;
    }
    free(dir);
    return out;
}

static int logical_row_of(const char *name)
{
    unsigned i;

    for (i = 0; i < LG_N(LG_PROP); i++)
        if (strcmp(LG_PROP[i].name, name) == 0) return (int)i;
    return -1;
}

static const char *logical_name_of(CssLogicalGroup group, LgRole role)
{
    unsigned i;

    for (i = 0; i < LG_N(LG_PROP); i++)
        if (LG_PROP[i].group == group && LG_PROP[i].role == role) return LG_PROP[i].name;
    /* `logical_init` asserts every group holds one row per role of its kind, so a miss here is that
       invariant broken after the table was checked. */
    DFAIL("css-logical-1 §4's pairing reached a group with no member for the role it mapped to. Every group "
          "in the table holds one row per role of its kind — four sides or two dimensions on each side of the "
          "pairing — and `css_logical_init` asserts exactly that, so this is a lookup running against a table "
          "that changed after the check");
    return NULL;
}

/* css-writing-modes-4 §6.4, forward: the PHYSICAL role an abstract one maps to on this element. */
static LgRole logical_physical_role(lxb_dom_element_t *el, LgRole abstract)
{
    LgWritingMode wm = logical_writing_mode(el);

    if (LG_IS_SIDE(abstract)) {
        /* §6.2's Note: the block sides "depend only on the writing-mode property", so the direction column is
           not read for them — the two columns of a writing mode carry the same cell there, which
           `css_logical_init` asserts. */
        LgDirection dir = (abstract == LG_BLOCK_START || abstract == LG_BLOCK_END)
                              ? LG_DIR_LTR : logical_used_direction(el, wm);

        return LG_SIDE[wm][dir][LG_ABSTRACT_SIDE_AT(abstract)];
    }
    /* §6.1's abstract dimensions name no direction at all. */
    return LG_DIM[wm][LG_DIR_LTR][abstract == LG_BLOCK_DIM ? 0u : 1u];
}

/* css-writing-modes-4 §6.4, inverted: the ABSTRACT role that maps to this physical one on this element. It is
   a search over the same table rather than a second table, which is what makes the two directions unable to
   disagree — the cost is four comparisons and the alternative is a transposed copy that drifts. */
static LgRole logical_abstract_role(lxb_dom_element_t *el, LgRole physical)
{
    LgWritingMode wm = logical_writing_mode(el);
    LgDirection dir;
    unsigned i;

    if (!LG_IS_SIDE(physical))
        return LG_DIM[wm][LG_DIR_LTR][0] == physical ? LG_BLOCK_DIM : LG_INLINE_DIM;
    /* THE BLOCK SIDES ARE TRIED FIRST AND THAT IS NOT AN OPTIMISATION — it is what keeps a `margin-top`
       query out of `logical_used_direction` in a vertical writing mode, where §6.2's Note says the answer
       does not depend on a direction and this engine cannot yet compute the used one. */
    for (i = 0; i < 2; i++)
        if (LG_SIDE[wm][LG_DIR_LTR][i] == physical) return (LgRole)(LG_BLOCK_START + i);
    dir = logical_used_direction(el, wm);
    for (i = 2; i < 4; i++)
        if (LG_SIDE[wm][dir][i] == physical) return (LgRole)(LG_BLOCK_START + i);
    DFAIL("css-writing-modes-4 §6.4 \"Abstract-to-Physical Mappings\" was inverted over a physical side that "
          "none of the four flow-relative sides maps to in this element's writing mode and used direction. "
          "The four map ONTO the four — `css_logical_init` asserts that every column of the table is a "
          "permutation — so a physical side with no abstract preimage is that invariant broken");
    return LG_BLOCK_START;
}

bool css_logical_axis_is_vertical(lxb_dom_element_t *el, CssLogicalAxis axis)
{
    LgRole physical;

    DCHECK(el != NULL,
           "css-writing-modes-4 §6.4 \"Abstract-to-Physical Mappings\"' DIMENSION rows were asked with no "
           "element. The element is not optional and is not a convenience: §6.1 \"Abstract Dimensions\" "
           "defines the block axis as \"the vertical axis in horizontal writing modes and the horizontal axis "
           "in vertical writing modes\", so an abstract axis names no physical one until a writing mode is in "
           "hand, and css-writing-modes-4 §7.4 \"Flow-Relative Mappings\" is what decides WHOSE");
    DCHECK(axis == CSS_LOGICAL_AXIS_BLOCK || axis == CSS_LOGICAL_AXIS_INLINE,
           "css-writing-modes-4 §6.1 \"Abstract Dimensions\" defines TWO abstract axes and this is neither — "
           "the block axis and the inline axis are the whole of that section's list, so a third value is a "
           "caller passing something that is not a `CssLogicalAxis`");
    physical = logical_physical_role(el, axis == CSS_LOGICAL_AXIS_BLOCK ? LG_BLOCK_DIM : LG_INLINE_DIM);
    /* `logical_physical_role`'s dimension arm reads `LG_DIM`, whose every cell `css_logical_init` has already
       checked is one of the two physical dimensions and whose two entries per column it has checked DIFFER —
       §6.1's two measurements are perpendicular, so this is that pair of invariants re-read at the boundary
       that exposes them rather than a third opinion about the table. */
    DCHECK(physical == LG_VERTICAL || physical == LG_HORIZONTAL,
           "a cell of css-writing-modes-4 §6.4 \"Abstract-to-Physical Mappings\"' dimension rows answered a "
           "role that is neither physical dimension, which `css_logical_init` asserts cannot be in the table");
    return physical == LG_VERTICAL;
}

CssLogicalGroup css_logical_group_of(const char *longhand, bool *pphysical)
{
    int at;

    DCHECK(longhand != NULL && pphysical != NULL,
           "css-logical-1 §4's group question was asked with no property name or nowhere to report the "
           "MAPPING LOGIC — the flag is written on every path, so a caller that cannot receive it would "
           "compare two properties using whatever the last question left there");
    *pphysical = true;
    at = logical_row_of(longhand);
    if (at < 0) return CSS_LOGICAL_GROUP_NONE;
    *pphysical = LG_IS_PHYSICAL(LG_PROP[at].role);
    return LG_PROP[at].group;
}

const char *css_logical_partner_of(lxb_dom_element_t *el, const char *longhand)
{
    int at;
    LgRole role;

    DCHECK(el != NULL && longhand != NULL,
           "css-logical-1 §4's pairing was asked with no element or no property name. The element is not "
           "optional: §4 pairs the two members \"using the element's own computed writing mode\", so there "
           "is no answer that is a fact about the property alone");
    at = logical_row_of(longhand);
    if (at < 0) return NULL;
    role = LG_PROP[at].role;
    role = LG_IS_PHYSICAL(role) ? logical_abstract_role(el, role) : logical_physical_role(el, role);
    return logical_name_of(LG_PROP[at].group, role);
}

void css_logical_init(void)
{
    unsigned wm, dir, i, j, g;

    /* css-writing-modes-4 §6.4's four flow-relative sides map ONTO the four physical ones in every column of
       the table: they are the four sides of one box named twice over, so a column in which two of them land
       on one physical side has lost a side and duplicated another. This is the check a transcription error in
       a single cell fails, and it fails per column, which is what names the column to look at. */
    for (wm = 0; wm < LG_WM_N; wm++)
        for (dir = 0; dir < LG_DIR_N; dir++) {
            unsigned seen = 0;

            for (i = 0; i < 4; i++) {
                LgRole p = LG_SIDE[wm][dir][i];

                DCHECK(p <= LG_LEFT, "a cell of css-writing-modes-4 §6.4 \"Abstract-to-Physical Mappings\"'s "
                                     "side table holds a role that is not one of the four PHYSICAL sides");
                seen |= 1u << (unsigned)p;
            }
            DCHECK(seen == 0xFu,
                   "a column of css-writing-modes-4 §6.4 \"Abstract-to-Physical Mappings\"'s side table is "
                   "not a PERMUTATION of the four physical sides — two flow-relative sides map to one "
                   "physical side and another has no preimage, which is a transcription error in one cell of "
                   "that column and makes the inverse mapping a partial function");
            DCHECK(LG_DIM[wm][dir][0] != LG_DIM[wm][dir][1],
                   "a column of css-writing-modes-4 §6.4's dimension rows maps the BLOCK and INLINE "
                   "dimensions to the same physical dimension, so css-writing-modes-4 §6.1 \"Abstract "
                   "Dimensions\"' two measurements have become one");
        }
    /* css-writing-modes-4 §6.2 "Flow-relative Directions"' closing Note, asserted rather than assumed:
       "determining the block-start and block-end sides of a box depends only on the writing-mode property".
       The whole reason a `margin-top` resolves in a vertical writing mode this engine cannot yet give a USED
       direction is that these two cells do not move with the direction column. */
    for (wm = 0; wm < LG_WM_N; wm++) {
        DCHECK(LG_SIDE[wm][LG_DIR_LTR][0] == LG_SIDE[wm][LG_DIR_RTL][0] &&
                   LG_SIDE[wm][LG_DIR_LTR][1] == LG_SIDE[wm][LG_DIR_RTL][1],
               "css-writing-modes-4 §6.4's `block-start` or `block-end` cell differs between a writing "
               "mode's two DIRECTION columns, and css-writing-modes-4 §6.2 \"Flow-relative Directions\" says "
               "it cannot: the block sides depend only on the writing-mode property. The block-axis lookup "
               "reads the `ltr` column outright on the strength of that sentence");
        DCHECK(LG_DIM[wm][LG_DIR_LTR][0] == LG_DIM[wm][LG_DIR_RTL][0] &&
                   LG_DIM[wm][LG_DIR_LTR][1] == LG_DIM[wm][LG_DIR_RTL][1],
               "css-writing-modes-4 §6.4's dimension rows differ between a writing mode's two DIRECTION "
               "columns, and css-writing-modes-4 §6.1 \"Abstract Dimensions\" names no direction at all in "
               "either definition");
    }
    /* EVERY GROUP IS COMPLETE ON BOTH SIDES. A group missing one role is a partner lookup that answers NULL
       for a property the table claims is in a group, which the cascade would read as "this property has no
       pair" and resolve as a single property's — the silent half of the defect this component exists to end. */
    for (g = CSS_LOGICAL_GROUP_MARGIN; g <= CSS_LOGICAL_GROUP_OVERFLOW; g++) {
        unsigned sides = 0, dims = 0;

        for (i = 0; i < LG_N(LG_PROP); i++) {
            if (LG_PROP[i].group != (CssLogicalGroup)g) continue;
            if (LG_IS_SIDE(LG_PROP[i].role)) sides |= 1u << (unsigned)LG_PROP[i].role;
            else dims |= 1u << (unsigned)(LG_PROP[i].role - LG_HORIZONTAL);
        }
        DCHECK((sides == 0xFFu && dims == 0u) || (sides == 0u && dims == 0xFu),
               "a css-logical-1 §4 logical property group is not COMPLETE: it must hold either all four "
               "physical sides and all four flow-relative ones, or both physical dimensions and both "
               "abstract ones, and it holds neither set. A group with a missing member answers no partner "
               "for the properties that are present, so their declarations cascade alone");
    }
    /* NO NAME APPEARS TWICE. The lookup is a linear scan that stops at the first match, so a second row for
       one property is a row that can never be read and a disagreement nothing would report. */
    for (i = 0; i < LG_N(LG_PROP); i++)
        for (j = i + 1; j < LG_N(LG_PROP); j++)
            DCHECK(strcmp(LG_PROP[i].name, LG_PROP[j].name) != 0,
                   "one property name has TWO rows in css-logical-1 §4's group table — css-logical-1 §4 says "
                   "\"(Each longhand property can belong to at most one logical property group.)\", and the "
                   "second row is unreachable behind the first");
    /* THE THREE PROPERTIES THE MAPPING ITSELF READS ARE IN NO GROUP, and this is the assert that keeps the
       recursion finite rather than a comment claiming it is. css-logical-1 §4: "It also requires that
       writing-mode, direction, and text-orientation be computed as a prerequisite for cascading together the
       flow-relative and physical declarations of a logical property group to find their computed values." The
       cascaded value of a grouped property asks this component for a partner, which asks the cascade for
       these three — so a row for any of them is an unbounded recursion between two components. */
    {
        static const char *const PREREQ[] = { "writing-mode", "direction", "text-orientation" };
        bool physical;

        for (i = 0; i < LG_N(PREREQ); i++)
            DCHECK(css_logical_group_of(PREREQ[i], &physical) == CSS_LOGICAL_GROUP_NONE,
                   "one of css-logical-1 §4's three PREREQUISITES for cascading a logical property group — "
                   "`writing-mode`, `direction`, `text-orientation` — has been given a group of its own. "
                   "Resolving a grouped property's cascade asks this component for its partner, and the "
                   "partner is derived from those three, so the cascade would re-enter itself without bound");
    }
}
