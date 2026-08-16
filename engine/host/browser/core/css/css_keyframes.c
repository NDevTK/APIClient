/* CSS Animations Level 1 §3's membership table. See css_keyframes.h for why the refused set is the closed one,
 * why there is no shorthand table beside it, and why a custom property reaches this entry. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "check.h"
#include "core/css/css_keyframes.h"

/* §4's nine properties, in the spec's own order — §4.1 through §4.9's `Name:` lines, which are the whole of
   "those defined in this specification". The `animation` shorthand is the last of them and is refused by name
   like the other eight: a shorthand is a property this specification defines, and the refusal is asked of the
   declaration AS WRITTEN, so `animation: 3s foo` sets none of the eight longhands §4.9 expands it to — the
   seven that are refused here on their own, and `animation-timing-function`, which §3 admits only as a
   declaration of its own and interprets specially when it is one. */
static const char *const ANIMATION_PROPERTIES[] = {
    "animation-name", "animation-duration", "animation-timing-function", "animation-iteration-count",
    "animation-direction", "animation-play-state", "animation-delay", "animation-fill-mode", "animation",
};

/* §3's ONE EXCEPTION, named by the sentence itself: "but does accept the animation-timing-function property
   and interprets it specially" — §3's own worked example declares it on the 0%, 25%, 50% and 75% keyframes of
   `@keyframes bounce` and says what each one means. It is spelled once, here, so the assertion below can read
   it back against the table rather than a reader having to notice it is missing from one. */
#define KEYFRAME_TIMING_FUNCTION "animation-timing-function"

static bool css_keyframes_defined_here(const char *name)
{
    size_t i, n = sizeof(ANIMATION_PROPERTIES) / sizeof(ANIMATION_PROPERTIES[0]);

    for (i = 0; i < n; i++)
        if (strcmp(ANIMATION_PROPERTIES[i], name) == 0) return true;
    return false;
}

bool css_keyframes_declaration_applies(const char *name, bool important)
{
    DCHECK(name != NULL, "a keyframe block was asked about a declaration with no property name");
    DCHECK(css_keyframes_defined_here(KEYFRAME_TIMING_FUNCTION),
           "§3's excepted property is not among the properties §4 defines, so the exception excepts nothing "
           "and the table above has drifted from the specification it is transcribed from");
    /* §3: "properties qualified with !important are invalid and ignored" — a statement about the DECLARATION
       rather than about the property, so it is asked of every name (a custom property's included) and it is
       asked FIRST, because it decides on its own. */
    if (important) return false;
    /* "accepts any CSS property except those defined in this specification" — everything outside the table is
       in, which is what makes the table the closed half. */
    if (!css_keyframes_defined_here(name)) return true;
    return strcmp(name, KEYFRAME_TIMING_FUNCTION) == 0;
}
