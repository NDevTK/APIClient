/* css-values-5 "Appendix A: Arbitrary Substitution Functions", subsection "Substitution in Shorthand
 * Properties" — THE VALUE THAT STANDS IN A LONGHAND WHILE ITS SHORTHAND IS STILL UNSUBSTITUTED.
 *
 * THE MACHINERY IS IN AN APPENDIX, SO THERE IS NO SECTION NUMBER AND THE TITLES ARE THE CITATION. Every
 * reference below names the appendix and its subsection by title for that reason, not by oversight; a `§`
 * written beside any of them would name a section css-values-5 does not have. The two subsections this file
 * implements are "Substitution in Shorthand Properties" and, for the failure arm, "Invalid Substitution".
 *
 * WHAT IT IS, IN THE STANDARD'S OWN WORDS: "If a shorthand property contains an arbitrary substitution
 * function in its value, the longhand properties it’s associated with must instead be filled in with a
 * special, unobservable-to-authors pending-substitution value that indicates the shorthand contains an
 * arbitrary substitution function, and thus the longhand’s value can’t be determined until after
 * substituted." And what happens to it next: "This value must then be cascaded as normal, and at
 * computed-value time, after substitution, the shorthand must be parsed and the longhands must be given
 * their appropriate values at that point." (The spec's own curly marks are kept — the straight ones would
 * close each quotation early.)
 *
 * WHY IT CANNOT BE SOLVED AT PARSE TIME, which is the whole reason a distinguished value has to exist. The
 * standard's Note says it: "When a shorthand is written without an arbitrary substitution function, it is
 * parsed and separated out into its component longhand properties at parse time; the longhands then
 * participate in the cascade, with the shorthand property more or less discarded. When the shorthand contains
 * a var(), however, this can’t be done, as the var() could be substituted with anything." `margin: var(--g)`
 * may resolve to one component or to four, and which longhand each component lands in is not known until the
 * substitution has run — so the split core/css/css_shorthand.h performs has no input yet.
 *
 * WHY IT IS A STRING AND NOT A TYPED VALUE, WHICH IS THIS ENGINE'S CONSTRAINT AND NOT THE STANDARD'S. A
 * cascaded value is a `char *` from the declaration collectors through `cssom_cascaded_value` to
 * core/css/css_computed_value.c; there is no value object to put a variant on. So the pending-substitution
 * value is an ENCODING carrying the two facts the resolution step needs — the SHORTHAND'S NAME and its
 * ORIGINAL, unsubstituted VALUE — and this file is the only place that encoding is written or read.
 *
 * THE ENCODING: U+0001, the shorthand's name, U+0001, the shorthand's original value. The shorthand name
 * comes from core/css/css_shorthand.c's table and never contains U+0001, so the SECOND mark is unambiguously
 * the separator however the value is spelled — a value carrying U+0001 of its own is carried through
 * verbatim and splits correctly, which is why the split needs no escaping and no length prefix.
 *
 * TWO CITATION FINDINGS AGAINST THIS WORK ARE MANUFACTURED BY AN ABSENT CORPUS ROW, AND NO EDIT AT EITHER
 * SITE DRAINS THEM. MEASURED with `node engine/citegen.mjs --all` over the five files this component touches,
 * findings intersected with the lines the diff added: core/css/css_computed_value.c's substituted-CSS-wide-
 * keyword paragraph is reported as diverging from `csscascade5 §7.3.3`, and core/css/css_style_declaration.c's
 * longhand-serialization comment from `cssom §6.6.1`. BOTH QUOTATIONS ARE css-values-5's AND BOTH ARE EXACT —
 * verified by hand against the fetched editor's draft, each present once, each with a one-word corruption of
 * itself absent from the same document so the check is armed rather than merely quiet. citegen indexes no
 * css-values-5, and this appendix's subsections carry no NUMBER for an anchor to be built from, so each
 * quotation resolves to the nearest PRECEDING indexed citation in its own prose — which is the accusing half
 * of an unindexed standard rather than the silent half. NAMING THE STANDARD IN FRONT OF THE QUOTATION WAS
 * TRIED AND CHANGED NOTHING, which is the expected result and is why it is written down: the name resolves to
 * nothing, so the anchor wins anyway, and a reader who follows the standing advice and re-runs the audit sees
 * the count unmoved and concludes the citation is wrong. IT IS NOT. Do not "repair" either site.
 * NAMED RESIDUAL — CSS-VALUES-5 HAS NO COMMITTED CORPUS ROW. WHAT IS NOT COVERED: every quotation this
 * component and its two callers make of css-values-5 is compared against some OTHER standard or against
 * nothing, so the axis this project audits most carefully is the one axis under which this diff is unchecked.
 * WHAT THE NEXT DIFF BUILDS: a `SPECS` row for css-values-5 in engine/citegen.mjs with its section index and
 * text corpus — measured over the WHOLE corpus and not over this standard, because indexing one makes it a
 * neighbour of every other and can move findings in files it has nothing to do with. HOW ITS ABSENCE WOULD
 * SHOW, as an observation: a run of the citation audit reports a quotation of css-values-5 as diverging from
 * a document the site does not name, and the divergence word is the second or third rather than a late one.
 * RETIREMENT: this record goes when `ls engine/specindex/text/` lists a css-values-5 row.
 *   THAT CONDITION READ `when the audit's census line stops counting css-values-5 among the standards it
 *   indexes no text for`, AND IT WAS SATISFIED FROM THE DAY IT WAS WRITTEN — which is the one way a retirement
 *   condition fails that nothing reports, because it retires a residual whose gap is still open. The census
 *   line counts a standard only where a citation NAMES it beside a number, and this appendix has no numbers:
 *   `git grep -ci` over `css-values-5 §` answers ZERO across the tree, so `css-values-5=` appears nowhere in
 *   a whole-run census and the line can never stop counting what it never counted. It is recorded rather than
 *   silently replaced because a condition phrased over an INSTRUMENT'S OUTPUT is the natural thing to reach
 *   for and is wrong whenever the population that output is drawn from cannot contain the subject; a
 *   condition phrased over the ARTIFACT cannot fail that way.
 *
 * NAMED RESIDUAL — THE ENCODING IS UNFORGEABLE BY ARGUMENT AND NOT BY CONSTRUCTION. WHAT IS NOT COVERED: an
 * author's declaration value is bytes css-syntax-3 §3.3 "Preprocessing the input stream" replaces only U+0000
 * and the surrogates in, so U+0001 survives tokenization and a `<string-token>` may carry one; nothing in
 * this file's encoding is therefore impossible for a page to write down. What BOUNDS it today is a property
 * of the READS rather than of the value, and there are two of them. The RESOLUTION read
 * (core/css/css_computed_value.c's cascaded-value step) hands the decoded name straight to
 * `css_shorthand_component`, which returns NULL for a name its table does not record and for a shorthand that
 * does not set the longhand being asked — so a forged value resolves to CSS Cascade 5 §7.3.3's `unset` rather
 * than to somebody else's component. The SERIALIZATION reads (core/css/css_style_declaration.c's
 * `cssd_value_in_block` and its block serializer) are not bounded that way and ask only `css_pending_is`, so
 * a forged value there reads back through CSSOM as the empty string. Both are WRONG ANSWERS ON ONE PROPERTY
 * OF ONE ELEMENT and neither is an abort: nothing here asserts on bytes a page wrote, which is the one
 * property this residual may not trade away. WHAT THE NEXT DIFF BUILDS: a cascaded value that carries its KIND
 * beside its text, so `pending-substitution` is a state of the value object and the cascade cannot be handed
 * a string that means one; that is a widening of `cssom_cascaded_value`'s return type and of every collector
 * that feeds it. HOW ITS ABSENCE WOULD SHOW, as an observation: a declaration whose value begins with U+0001
 * and carries a second one computes as `unset`, or reads back through CSSOM as the empty string, on a page
 * that wrote no shorthand at all — so a declaration a browser honours is silently defaulted or silently
 * blanked on exactly one property.
 * RETIREMENT: this record goes when a cascaded value states its kind and this file's split is over that kind
 * rather than over a leading byte.
 */
#ifndef ENGINE_HOST_BROWSER_CORE_CSS_CSS_PENDING_SUBSTITUTION_H
#define ENGINE_HOST_BROWSER_CORE_CSS_CSS_PENDING_SUBSTITUTION_H
#include <stdbool.h>

/* The pending-substitution value for one longhand of `shorthand`, whose declaration's value is `original` —
   which is the value AS WRITTEN, arbitrary substitution functions and all, because that is what the
   computed-value step re-parses. Every longhand of one declaration gets a BYTE-IDENTICAL value, which is what
   makes the serialization rule below a `strcmp`. OWNED: the caller frees. */
char *css_pending_make(const char *shorthand, const char *original);

/* Is `value` one? NULL is not. Asked before any use of a cascaded value's text, and asked in exactly one
   place per reader, so that a value which IS one is never also read as a property's own value. */
bool css_pending_is(const char *value);

/* Decode. On TRUE `*shorthand` is an OWNED copy of the shorthand's name and `*original` BORROWS the original
   value from inside `value`, which therefore outlives neither. On FALSE both are untouched and `value` is an
   ordinary value. One function and not two accessors, because two scans of one encoding are two ideas of
   where the separator is. */
bool css_pending_split(const char *value, char **shorthand, const char **original);

/* "If all of the component longhand properties for a given shorthand are pending-substitution values from the
   same original shorthand value, the shorthand property must serialize to that original (arbitrary
   substitution function-containing) value. Otherwise, if any of the component longhand properties for a given
   shorthand are pending-substitution values, or contain arbitrary substitution functions of their own that
   have not yet been substituted, the shorthand property must serialize to the empty string."
   THE TWO ARMS ARE ONE ANSWER, so they are one call: `*out` receives the original (OWNED) for the first arm
   and NULL for the second, and the return value says whether the standard's rule decided at all. FALSE means
   the rule says nothing about this list and the caller's own grammar answers.
   THE SECOND DISJUNCT IS WHY THIS TAKES A LIST OF VALUES RATHER THAN A LIST OF PENDING-SUBSTITUTION VALUES:
   a longhand DECLARED with an arbitrary substitution function of its own is not one of this file's values at
   all, and the standard gives it the same answer. */
bool css_pending_serialize(const char *const *values, unsigned n, char **out);

#endif
