/* HTML §2.3.3 "Keywords and enumerated attributes" — the DETERMINE THE STATE algorithm, written once.
 *
 * WHY THIS IS A COMPONENT. It is one question — "which state is this attribute in" — asked by every enumerated
 * attribute in the platform, and the way it goes wrong is not that a copy is wrong on its own terms but that
 * each copy answers a DIFFERENT SUBSET of the section and the difference is invisible at the call site. The
 * shapes measured in this tree before this file existed: one implementation of all four steps with all three
 * special states, whose own header said the algorithm was "written ONCE here"; one that collapsed the missing
 * and invalid value defaults into a single field, so an attribute that distinguishes them could not use it; and
 * two that were a bare `strcasecmp`-shaped loop over a keyword list with no defaults at all. None of them was
 * reachable from another file, so the fifth caller — HTML §2.5.5's referrer policy attribute — would have been
 * a fifth copy, and the site that needed it had instead been comparing the RAW ATTRIBUTE with `strcmp`.
 *
 * WHAT A RAW `strcmp` GETS WRONG, since that is the defect this file closes and it is worth being exact about:
 * §2.3.3 matches ASCII CASE-INSENSITIVELY, so `referrerpolicy="No-Referrer"` is the no-referrer state and a
 * byte comparison says it is not; and an unrecognised value is the INVALID VALUE DEFAULT, so `="bogus"` is a
 * real state rather than a value nothing matches. A byte comparison is wrong in both directions and silently.
 *
 * THE STATE IS READ OFF LEXBOR AND NOT THROUGH solver/attr_shadow.c, for the reason global_attributes.c states
 * in full: the shadow keeps an attacker value a flow stashed in an attribute so its taint survives the DOM, and
 * what §2.3.3 asks for is not the attribute's VALUE but its STATE, which a fixed keyword set decides. A concolic
 * value matches no keyword, so both reads give the same state and this one gives it without wrapping anything.
 * WRITES are unaffected and still go through core/dom/element.c's chokepoint, which is what the DOM COW delta
 * captures.
 *
 * THE COMPARISON IS ASCII'S AND NOT THE LOCALE'S. `strcasecmp` folds by the current locale, and a Turkish locale
 * folds `I` to `ı` — which would fail to recognise `shadowrootmode="OPEN"` and `referrerpolicy="ORIGIN"`. */
#include <string.h>

#include "check.h"
#include "core/html/enumerated_attribute.h"

bool enumerated_attribute_keyword_match(const char *keyword, const char *value, size_t value_len)
{
    size_t i;

    DCHECK(keyword != NULL, "§2.3.3's keyword match was given no keyword — a table row's keyword is NULL only "
                            "as the terminator, so reaching the comparison with one is a walk that ran past "
                            "the end of the table");
    DCHECK(value != NULL || value_len == 0,
           "§2.3.3's keyword match was given a NULL attribute value with a non-zero length — lexbor stores an "
           "attribute that is present with no value as a NULL pointer and a length of zero, so a NULL with a "
           "length is a read of a value that was freed or never written");
    for (i = 0; i < value_len; i++) {
        char x = keyword[i], y = value[i];

        if (!x) return false;                                   /* keyword is shorter than the value */
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return keyword[value_len] == 0;                             /* and no longer than it either */
}

/* Every keyword in a table is its state's CANONICAL form, and canonical is ASCII lowercase for every enumerated
   attribute HTML defines. Checked here rather than trusted, because the failure is silent in one direction only:
   the MATCH would still succeed for an uppercase row, and §2.6.1's reflected getter would then hand a page bytes
   that no keyword list contains. */
static void dcheck_table_is_canonical(const EnumeratedKeyword *kw)
{
#if APICLIENT_DEV
    int i;
    size_t j;

    DCHECK(kw != NULL, "an enumerated attribute's state was asked for with no keyword table");
    for (i = 0; kw[i].keyword; i++) {
        DCHECK(kw[i].state != ENUMERATED_NO_STATE,
               "an enumerated attribute's keyword table names ENUMERATED_NO_STATE as one of its states — that "
               "value IS §2.3.3's \"return no state\" answer, so a keyword mapping to it makes a real state "
               "indistinguishable from the absence of one, and §2.6.1's getter would hand a page a keyword for "
               "an attribute value that corresponds to no state at all");
        for (j = 0; kw[i].keyword[j]; j++)
            DCHECK(!(kw[i].keyword[j] >= 'A' && kw[i].keyword[j] <= 'Z'),
                   "an enumerated attribute's keyword table holds a row that is not ASCII lowercase — §2.3.3's "
                   "match would still accept it, and §2.6.1's reflected getter returns these exact bytes, so a "
                   "page would read back a keyword the attribute's own specification does not define");
    }
#else
    (void)kw;
#endif
}

int enumerated_attribute_state(const lxb_dom_element_t *el, const char *attr,
                               const EnumeratedKeyword *kw, int missing, int empty, int invalid)
{
    lxb_dom_element_t *e = (lxb_dom_element_t *)el;
    size_t alen, len = 0;
    const lxb_char_t *v;
    int i;

    DCHECK(e != NULL, "an enumerated attribute's state was asked for with no element");
    DCHECK(attr != NULL, "an enumerated attribute's state was asked for with no attribute name");
    dcheck_table_is_canonical(kw);
    alen = strlen(attr);
    /* STEP 1: "if the attribute is not specified", the missing value default — or no state. */
    if (!lxb_dom_element_has_attribute(e, (const lxb_char_t *)attr, alen))
        return missing;
    v = lxb_dom_element_get_attribute(e, (const lxb_char_t *)attr, alen, &len);
    /* An attribute PRESENT with no value at all (`<div translate>`) is the empty string, which lexbor stores as
       a NULL value pointer. STEP 2 STILL RUNS FOR IT, and that is not a detail: §2.5.5's referrer policy
       attribute has THE EMPTY STRING AS A KEYWORD, so `referrerpolicy=""` is matched here rather than falling
       to step 3 — an attribute may legitimately map the empty string to a state that is not its empty value
       default. Skipping the loop on a NULL pointer would decide that by how lexbor happens to store a value. */
    if (!v) len = 0;
    for (i = 0; kw[i].keyword; i++)                             /* STEP 2 */
        if (enumerated_attribute_keyword_match(kw[i].keyword, (const char *)v, len)) return kw[i].state;
    if (len == 0) return empty;                                 /* STEP 3 */
    return invalid;                                             /* STEPS 4-5 */
}

bool enumerated_attribute_state_has_keyword(const EnumeratedKeyword *kw, int state)
{
    int i;

    dcheck_table_is_canonical(kw);
    for (i = 0; kw[i].keyword; i++)
        if (kw[i].state == state) return true;
    return false;
}

const char *enumerated_attribute_canonical_keyword(const EnumeratedKeyword *kw, int state)
{
    const char *found = NULL;
    int i;

    dcheck_table_is_canonical(kw);
    for (i = 0; kw[i].keyword; i++) {
        if (kw[i].state != state) continue;
        DCHECK(found == NULL,
               "§2.3.3's canonical keyword was asked for a state TWO keywords map to — the section's first arm "
               "(\"only one keyword mapping to the given state\") does not apply, and which of the two is "
               "canonical is then stated by the attribute's own specification. Answering with whichever row "
               "came first would be inventing it");
        found = kw[i].keyword;
    }
    DCHECK(found != NULL,
           "§2.3.3's canonical keyword was asked for a state NO keyword maps to — \"states which have any "
           "keywords mapping to them are said to have a canonical keyword\", so such a state has none at all "
           "and §2.6.1's getter answers the empty string for it by its own rule rather than by this one");
    return found;
}
