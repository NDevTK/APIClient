/* CSS TRANSFORMS 1 §7 "The Transform Functions" — THE `<transform-list>` GRAMMAR, AND §3 "The transform
 * Property"'s COMPUTED VALUE OVER IT.
 *
 * WHICH EDITION'S NUMBERS THESE ARE. The same answer core/css/css_transform.h gives and for the same reason:
 * css-transforms-1's CR numbers "Terminology" and its Editor's Draft renders that heading with no number at
 * all, so every section below it differs by one. EVERY NUMBER HERE IS THE ED'S, which is what the committed
 * section index resolves against and what the rest of this tree cites. Each carries its TITLE, which survives
 * a renumbering the number does not.
 *
 * WHY THIS IS A COMPONENT AND NOT A FUNCTION IN ITS ONE CALLER. css-transforms-1 §3 "The transform Property"'s
 * `Computed value:` line is "as specified,
 * but with lengths made absolute", and the FIRST half of that sentence is a parse: a component that cannot
 * find which parts of `translate(10em, 50%) rotate(45deg)` are lengths cannot absolutize them, and every other
 * consumer of a transform needs the SAME decomposition rather than its serialization. Three algorithms in
 * three standards are waiting on it and each names the list rather than the text — css-transforms-1 §3.2
 * "Resolved value of transform" post-multiplies the functions into one matrix, CSSOM VIEW §6 "Extensions to
 * the Element Interface"'s getClientRects() maps a border area through that matrix, and INTERSECTION OBSERVER
 * §3.2.9 "Calculate a target's Effective Transformation Matrix" accumulates it up a containing block chain.
 * So the PARSED LIST is what this file answers and the string is one reader of it.
 *
 * WHAT IT DOES NOT DO, AND WHY THAT IS THE NEXT DIFF RATHER THAN A GAP HERE. §12 "Mathematical Description of
 * Transform Functions" gives each function its matrix; the reduction of a list to one 4x4 is §3.2's own three
 * steps. Neither is here. core/dom/element_view.c crashes for exactly that and names it, which is the order
 * §Do-subproblems-IN-ORDER asks for: the grammar first, because the matrix is a function OF the parsed list
 * and cannot be written before there is one.
 *
 * THE TWO WAYS A VALUE CAN FAIL ARE DIFFERENT FACTS AND THIS FILE ANSWERS THEM DIFFERENTLY. That distinction
 * is the whole reason a `transform` needs a component rather than a predicate, because lexbor's property
 * registry carries NO `transform` entry: a declared transform reaches the cascade as a `__CUSTOM` declaration
 * carrying the author's RAW value with nothing having validated it against §7's grammar. So this file is the
 * first thing that looks, and it meets both populations.
 *   AN AUTHORING MISTAKE IS A DROPPED DECLARATION, NEVER A CRASH. CSS Syntax §2.2 "Error Handling": "the user
 *   agent checks it against its expected grammar. If it does not match the grammar" it is ignored. `false`
 *   from the parse is that answer, and the caller falls to §3's `Initial:` value of `none`. It is also the
 *   only answer CLAUDE.md's rule about foreign bytes permits: a stylesheet is a STRANGER'S text, so a
 *   `DCHECK` standing on its shape would hand any page an abort switch for this engine.
 *   A CAPABILITY THIS ENGINE HAS NOT BUILT CRASHES BY NAME. `translate3d(…)` is not a mistake — css-transforms-2
 *   §12.2 "3D Transform Functions" defines it and this engine implements no level 2 — so answering `none` for
 *   it would drop an author's own declaration and report a WRONG transform rather than an absent one.
 *   THE DISCRIMINATOR IS WHO ENUMERATES THE NAME: a function a STANDARD names is a gap, and one nobody names
 *   is invalid input. That is the only thing separating the two, and it is why the level-2 names are written
 *   out below rather than left to a catch-all.
 *
 * A NUMBER IS NOT A LENGTH AND THE THREE ARGUMENT TYPES ARE CARRIED APART. §7.1 "2D Transform Functions" gives
 * each function ONE argument production — `<number>` for matrix() and the scales, `<length-percentage>` for
 * the translations, `[ <angle> | <zero> ]` for the rotation and the skews — so the kind decides which of the
 * three arrays below is the value and no argument is ever both. A LENGTH is the only one css-transforms-1 §3
 * "The transform Property" absolutizes; an angle and a number are carried AS SPECIFIED, because that section's
 * own words are "as specified, but with lengths made absolute", which says
 * precisely which one moves. That is why `angle_unit` exists: `rotate(1turn)` computes to `rotate(1turn)` and
 * not to `rotate(360deg)`, and a component that stored only the canonical degrees would have thrown away the
 * half of §3's sentence that does NOT change.
 *
 * A PERCENTAGE IS NOT A GAP AT COMPUTED-VALUE TIME. §7 says "A percentage for horizontal translations is
 * relative to the width of the reference box", and §5 "Transform reference box: the transform-box property"
 * makes that box a LAYOUT — so a percentage is resolved at USED-value time and §3's line leaves it exactly
 * where css-values-4 §10.11 "Computed Value" leaves every other unresolved percentage: as specified. A
 * `translate(50%)` therefore computes here with no box and no crash, and the box becomes a question only for
 * the consumer that maps a rectangle.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_FUNCTION_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_TRANSFORM_FUNCTION_H

#include <stdbool.h>
#include <stddef.h>

#include "quickjs.h"
#include "core/css/css_length.h"

/* §7.1's ELEVEN FUNCTIONS, in the order that section writes them, which is also §3's `Canonical order:` line
   ("per grammar") and therefore the order a serialization states its arguments in. */
typedef enum {
    CSS_TF_MATRIX = 0,
    CSS_TF_TRANSLATE,
    CSS_TF_TRANSLATE_X,
    CSS_TF_TRANSLATE_Y,
    CSS_TF_SCALE,
    CSS_TF_SCALE_X,
    CSS_TF_SCALE_Y,
    CSS_TF_ROTATE,
    CSS_TF_SKEW,
    CSS_TF_SKEW_X,
    CSS_TF_SKEW_Y
} CssTransformFnKind;

/* `matrix( <number>#{6} )` is the widest argument list §7.1 defines, so six is the bound for every function
   here rather than a number chosen for headroom. */
#define CSS_TF_ARG_MAX 6

/* The longest §7.1 angle unit is `turn` at four bytes (css-values-4 §7.1 "Angle Units: the <angle> type and
   deg, grad, rad, turn units" defines exactly four), so a unit that does not fit is not one of them and the
   comparison can answer without a copy. One larger than it has to be so the NUL is never what decides. */
#define CSS_TF_ANGLE_UNIT_MAX 8

/* ONE `<transform-function>` AS §3 COMPUTES IT. `argc` is how many arguments were WRITTEN and not how many the
   function has: §7.1 gives translate(), scale() and skew() an optional second argument whose DEFAULT DIFFERS
   PER FUNCTION — "If <ty> is not provided, ty has zero as a value" for translate(), "If the second parameter
   is not provided, it takes a value equal to the first" for scale(), "If the second parameter is not provided,
   it has a zero value" for skew() — so a component that filled the omitted slot in would have to know which
   default, and a reader that saw the filled value could no longer tell `scale(2)` from `scale(2, 2)`. The
   defaults belong to whoever builds the matrix; this file records what the author wrote. */
typedef struct {
    CssTransformFnKind kind;
    unsigned           argc;
    double             number[CSS_TF_ARG_MAX];              /* matrix(), scale(), scaleX(), scaleY() */
    CssLength          length[2];                           /* translate(), translateX(), translateY() */
    double             angle[2];                            /* rotate(), skew(), skewX(), skewY() */
    char               angle_unit[2][CSS_TF_ANGLE_UNIT_MAX];
} CssTransformFunction;

/* `<transform-list> = <transform-function>+`, which is §3's own production and is why `n` is never zero for a
   list that parsed: a `+` admits no empty match, and `none` is the OTHER arm of §3's `Value:` line rather than
   an empty list. A caller holding `none` never reaches this component. */
typedef struct {
    CssTransformFunction *fn;
    size_t                n;
} CssTransformList;

/* §7's GRAMMAR OVER ONE SPECIFIED VALUE, with §3's absolutization performed on every `<length>` it finds.
   TRUE with `*out` owned by the caller (release it with `css_transform_list_free`); FALSE writing nothing,
   which is CSS Syntax §2.2 "Error Handling"'s dropped declaration and not an error state — see the header for
   why an authoring mistake may not crash here and a missing capability must.
   `realm` and `font` ARE THE ABSOLUTIZATION'S, and they are core/css/css_length.h's own two operands for the
   reason that file states: which computed `font-size` an `em` means is CSS Cascade §7.2's inheritance walk
   over a tree this component does not hold, and a `vw` is the element's own document's initial containing
   block rather than the running realm's. Both are required of every caller and consulted only by the arm that
   meets a relative unit. */
bool css_transform_list_parse(JSContext *realm, const CssFontMetrics *font, const char *value,
                              CssTransformList *out);

/* Releases a list `css_transform_list_parse` wrote. Idempotent over a zeroed list, so a caller may release on
   a path the parse refused without asking which it took. */
void css_transform_list_free(CssTransformList *list);

/* §3.1 "Serialization of <transform-function>s", which states the whole rule in one sentence: "To serialize
   the <transform-function>s, serialize as per their individual grammars, in the order the grammars are written
   in, avoiding <calc()> expressions where possible, avoiding <calc()> transformations, omitting components
   when possible without changing the meaning, joining space-separated tokens with a single space, and
   following each serialized comma with a single space."
   "OMITTING COMPONENTS WHEN POSSIBLE" IS READ AS THE GRAMMAR'S OWN OPTIONAL ARGUMENTS AND NOTHING WIDER, which
   is a deliberate reading rather than an unbuilt half. §7.1's three defaults differ per function, so
   canonicalising a WRITTEN `translate(1px, 0)` down to `translate(1px)` requires deciding that a length is
   zero — which for a `<length-percentage>` means deciding it for a percentage and for a math function's
   two-term residue as well, and css-values-4 §10.10.1 "Simplification" says in as many words that "Zero-valued
   terms cannot be simply removed from a Sum". So an argument the author wrote is serialized and one the author
   omitted is omitted, which can never change a meaning. IT IS UNOBSERVABLE FROM A PAGE TODAY and the
   observation that would make it matter is named rather than left to be discovered: §3.2 makes `transform` a
   resolved value special case, so `getComputedStyle(el).transform` reports a `matrix()` and never this string,
   and this reading becomes visible the day a member reports a computed `<transform-list>` verbatim.
   OWNED: the caller frees. */
char *css_transform_list_serialize(const CssTransformList *list);

#endif
