/* HTML §7.4's `features` ARGUMENT — the third argument to `window.open`, and what it decides.
 *
 * `open(url, name, "menubar=no,width=400")` is not a hint and it is not decoration: the spec TOKENIZES that
 * string with a stated algorithm, normalizes legacy aliases, parses each value as a boolean feature, and from
 * the result decides three things a page can observe directly — whether the new navigable is a POPUP (which
 * every BarProp answers from), whether it has an OPENER, and whether it carries a REFERRER.
 *
 * IT WAS PARSED BY NOTHING. `js_win_open` read the argument and dropped it, so every popup was a tab, every
 * BarProp answered `true`, and `noopener` handed back a fully-linked window. That is a spec step skipped, not
 * a member missing, and the corpus measures it precisely: the failing files are literally named
 * open-features-tokenization-*.
 *
 * THE TOKENIZER IS THE SPEC'S, CHARACTER FOR CHARACTER, because that is the entire content of those tests —
 * `left=141`, ` left = 141`, `left==141`, `\nleft= 141`, `,left=141,,` and `LEFT=141` must all reach the same
 * feature, and the ones that must NOT are just as load-bearing. A lenient split on ',' and '=' gets the common
 * spellings right and the tested ones wrong, which is the shape of every approximation this project refuses. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_FEATURES_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_WINDOW_FEATURES_H

#include <stdbool.h>

/* WHAT §7.4 DECIDES FROM THE STRING — the three facts that outlive the parse. Everything else a features
   string can carry (width, height, top, left) is a REQUEST TO THE WINDOWING SYSTEM, and a headless engine has
   no such system to ask; those are honestly not represented rather than stored and ignored. The three here are
   different in kind: they are answerable without a screen, and a page reads all three. */
typedef struct {
    bool is_popup;     /* §7.4's "popup window is requested" — what every BarProp's `visible` is the negation of */
    bool noopener;     /* §7.4: the new navigable has no opener, and `open()` returns null */
    bool noreferrer;   /* §7.4: no opener AND no Referer — noreferrer implies noopener */
} WindowFeatures;

/* §7.4's TOKENIZE THE FEATURES ARGUMENT, then CHECK IF A POPUP WINDOW IS REQUESTED, over one pass of the
   string. `features` may be NULL or empty, which §7.4 answers with all three false — an empty features string
   is a tab, and that is step 1 of the popup check rather than a special case here. */
WindowFeatures window_features_parse(const char *features);

#endif
