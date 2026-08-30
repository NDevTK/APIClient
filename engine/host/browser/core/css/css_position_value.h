/* CSS Values and Units 4 §8.3 "2D Positioning: the <position> type" — the one production that is a RUN of
 * component values whose length is not fixed and whose ORDER is not the order it serializes in.
 *
 * WHY IT IS A COMPONENT OF ITS OWN. Two unrelated grammars are stated over it and neither can be written
 * without it: css-backgrounds-3 §2.6 "Positioning Images: the background-position property" makes
 * `<bg-position>` this type plus a three-value form, and css-images-3 §3.2.1 "radial-gradient() Syntax" puts
 * `at <position>` inside a function's arguments. A second copy is what disagrees about `top 50px` — which
 * §8.3.1 "Parsing <position>" answers with a ONE-component position followed by a length, in its own worked
 * example — the day one of them is edited.
 *
 * THE ANSWER IS A LENGTH, NOT A BOOLEAN, because the caller is a `||` partition that must know where the next
 * term begins. §8.3.1 makes the parse GREEDY — "it consumes as many components as possible" — so the match
 * reports how many components it took and the caller resumes after them.
 *
 * AND THE PARSE ORDER IS NOT THE SERIALIZATION ORDER, which is the whole reason the match writes a NORMALIZED
 * pair rather than handing the caller its own words back. §8.3.2 "Serializing <position>" is stated over the
 * two AXES ("components are serialized horizontal first, then vertical") while the grammar's `&&` arm admits
 * `top left`; a caller that copied the author's component order would answer `top left` where every user agent
 * answers `left top`. The keyword is the CANONICAL lower-case spelling because §8.3.2 serializes keywords as
 * keywords and CSS Syntax §4 makes an ident ASCII case-insensitive; the offset is the author's own bytes,
 * because core/css/css_length.h owns every question about a length's spelling. */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_POSITION_VALUE_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_POSITION_VALUE_H
#include <stdbool.h>
#include <stddef.h>

/* ONE POSITION, SPLIT BY AXIS. Each axis carries a keyword, an offset, or a keyword AND an offset — §8.3's
   fourth arm is `[ [ left | right ] <length-percentage> ] && [ [ top | bottom ] <length-percentage> ]`, so
   both halves of one axis can be present at once and neither can be derived from the other.
   NEITHER AXIS IS EVER EMPTY, which css_position_serialize asserts: the one-component arm supplies §8.3.2's
   "implied center keyword" for the axis the author left out, and every longer arm names both. */
typedef struct {
    const char *h_kw;      /* "left" | "right" | "center", or NULL when the axis is an offset alone. STATIC. */
    const char *h_off;     /* the author's own `<length-percentage>` bytes, or NULL. BORROWED. */
    size_t h_off_len;
    const char *v_kw;      /* "top" | "bottom" | "center", or NULL. STATIC. */
    const char *v_off;     /* BORROWED. */
    size_t v_off_len;
} CssPositionValue;

/* §8.3.1's GREEDY MATCH over the `n` component values `w[i]`/`wl[i]`, writing the normalized pair into `*out`.
   Returns how many components it CONSUMED, and 0 for no match — which is the caller's invalid value, since
   every grammar that names `<position>` names it as a required term.
   `three_value` is css-backgrounds-3 §2.6's own widening and nothing else's: `<bg-position>`'s third arm is
   `[ center | [ left | right ] <length-percentage>? ] && [ center | [ top | bottom ] <length-percentage>? ]`,
   whose optional offsets generate a THREE-component value that §8.3's fourth arm does not. §8.3's own Note
   says why the type itself refuses it — "this has been disallowed generically because it creates parsing
   ambiguities when combined with other length or percentage components in a property value" — so a caller
   that is not `background-position` passes false and a `left 10px top` there is one component short of a
   4-value position rather than a 3-value one.
   `*out` IS WRITTEN ONLY ON A MATCH; a caller that reads it after a 0 is reading its own uninitialised
   memory, which is asserted at the one place that can see both. */
unsigned css_position_match(const char *const *w, const size_t *wl, unsigned n, bool three_value,
                            CssPositionValue *out);

/* §8.3.2's SERIALIZE, whole: keywords as keywords, offsets as offsets, horizontal axis first, and the omitted
   offset of css-backgrounds-3 §2.6.1 "Serialization of background-position values" left omitted ("the
   specified value serialization is identical to the equivalent 4-value syntax except that the omitted offset
   remains omitted"). Never NULL — a matched position always has both axes. OWNED: the caller frees. */
char *css_position_serialize(const CssPositionValue *p);

#endif
