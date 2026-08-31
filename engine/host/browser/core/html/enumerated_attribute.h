/* HTML §2.3.3 "Keywords and enumerated attributes" — DETERMINE THE STATE, as one component. See
   enumerated_attribute.c for why it is a component and not a static in whichever file asked first. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_ENUMERATED_ATTRIBUTE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_ENUMERATED_ATTRIBUTE_H

#include <stdbool.h>
#include <stddef.h>

#include <lexbor/dom/dom.h>

/* One row of an attribute's KEYWORD/STATE MAPPINGS, terminated by a row whose keyword is NULL. §2.3.3 allows
   "multiple keywords can map to the same state", so `state` is not an index and two rows may repeat one.
   THE KEYWORD IS STORED IN ITS CANONICAL FORM, which for every HTML enumerated attribute is ASCII lowercase:
   the MATCH is case-insensitive, but §2.6.1's reflected getter hands these exact bytes back to the page, so a
   row spelled `"No-Referrer"` would answer a page with text no keyword list contains. Asserted below. */
typedef struct { const char *keyword; int state; } EnumeratedKeyword;

/* §2.3.3's "return no state", AS A VALUE, so that a caller which has nowhere to put that answer can still ask
   for it. §2.3.3 names two ways to reach it — an absent attribute where the specification gives no missing
   value default, and an unmatched value where it gives no invalid value default — and the attributes that do
   that are ordinary: `formmethod` and `formenctype` have no missing value default by §4.10.19.6, and `as` and
   `inputmode` declare neither default at all. A caller whose states are its own enum passes this in the
   `missing`/`invalid` position and reads it back; no keyword table may name it, which is asserted, so it can
   never be confused with a state. */
#define ENUMERATED_NO_STATE (-1)

/* §2.3.3's ATTRIBUTE DEFINITION as §2.6.1's getter looks it up — "let attributeDefinition be the attribute
   definition of element's content attribute" — which is the keyword table AND the three special states
   TOGETHER, because neither half decides a state without the other.
   IT IS ONE STRUCT RATHER THAN FOUR FIELDS ON EACH CONSUMER for the reason the file's own header gives about
   copies: `method` and `formmethod` share §4.10.19.6's keyword table and differ ONLY in that `formmethod` has
   no missing value default, and `enctype`/`formenctype` are the same pair again — so the difference the
   standard states is a difference between two DEFINITIONS over one table, and a consumer that flattened them
   would have to restate the keywords to state the difference. */
typedef struct {
    const EnumeratedKeyword *keywords;
    int missing;   /* §2.3.3 step 1's missing value default, or ENUMERATED_NO_STATE */
    int empty;     /* step 3's empty value default; where the attribute declares none this is `invalid` */
    int invalid;   /* steps 4-5's invalid value default, or ENUMERATED_NO_STATE */
} EnumeratedAttribute;

/* §2.3.3's comparison: an ASCII case-insensitive match between one keyword and an attribute value that is NOT
   NUL-terminated (lexbor stores a length). `value` may be NULL only when `value_len` is 0 — an attribute that
   is present with no value at all is the empty string, which lexbor stores as a NULL pointer. */
bool enumerated_attribute_keyword_match(const char *keyword, const char *value, size_t value_len);

/* §2.3.3's DETERMINE THE STATE, in the section's own four steps. The three special states are PASSED because
   the section says they are "given in the specification of the attribute" — an attribute that defines one of
   them passes it, and an attribute that does not passes what §2.3.3's own fall-through reduces to:
     - no MISSING value default  ⇒ §2.3.3's step 1 "return no state", so pass the caller's no-state sentinel;
     - no EMPTY value default    ⇒ step 3 is skipped and step 4 runs, so pass `invalid` here;
     - no INVALID value default  ⇒ step 5 "return no state", so pass the sentinel here too.
   There is no separate "no state" return: a state the caller cannot confuse with a real one IS that answer,
   and inventing a sentinel here would be a second vocabulary for a fact the attribute already names. */
int enumerated_attribute_state(const lxb_dom_element_t *el, const char *attr,
                               const EnumeratedKeyword *keywords, int missing, int empty, int invalid);

/* §2.3.3's CANONICAL KEYWORD of a state — "if there is only one keyword mapping to the given state, then it is
   that keyword", which is the arm every attribute in this engine's tables uses. A state reached by TWO keywords
   has its canonical keyword decided by the section's later arms (which conform, and which name it explicitly in
   the attribute's own specification), and a state reached by NONE has no canonical keyword at all; both crash
   here rather than being answered, because the answer would have to be invented. */
const char *enumerated_attribute_canonical_keyword(const EnumeratedKeyword *keywords, int state);

/* §2.3.3: "states which have any keywords mapping to them are said to have a canonical keyword" — the QUESTION
   the sentence above CRASHES on, asked instead of assumed. It exists because §2.6.1's limited-to-only-known-
   values getter has that state as one of its OWN branches rather than as an error: "if it is in a state ... with
   no associated keyword value, then return the empty string". `dir`'s Undefined state and `popover`'s No Popover
   state are real states no keyword names, and so is the no-state answer §2.3.3 returns for an attribute with no
   missing value default, so a getter that could not ask this would have to crash on ordinary markup. */
bool enumerated_attribute_state_has_keyword(const EnumeratedKeyword *keywords, int state);

#endif
