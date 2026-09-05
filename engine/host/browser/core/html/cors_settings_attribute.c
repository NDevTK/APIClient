/* HTML §2.5.4 "CORS settings attributes" — the attribute's keyword/state table, in ONE place.
 *
 * WHY IT IS A COMPONENT AND NOT A TABLE ON WHICHEVER ELEMENT ASKED FIRST. It is the argument
 * core/html/referrer_policy_attribute.c makes at length and it applies here identically: `crossorigin` is
 * spelled the same on `<audio>`, `<img>`, `<link>`, `<script>` and `<video>`, §2.5.4 defines it once for all of
 * them, and a table on one of those elements would be the first of five — the fifth being the one that had not
 * heard about a change. What is element-specific is not the table but WHICH elements carry the attribute at
 * all, which is stated by each element's own reflection row.
 *
 * WHAT THE DEFAULTS MEAN, since two of the three differ and the difference is observable. §2.5.4: "The
 * attribute's missing value default is the No CORS state, and its invalid value default and empty value default
 * are both the Anonymous state." So `<img crossorigin>` and `<img crossorigin="bogus">` are BOTH Anonymous —
 * `img.crossOrigin` reads back "anonymous" for each — while `<img>` is No CORS, a state with no keyword, which
 * §2.6.1's getter answers with the empty string. A table that collapsed missing and invalid into one default
 * could not express that, which is the shape core/html/enumerated_attribute.c's header records as one of the
 * four it replaced.
 *
 * THIS FILE STATED THE ENUMERATION AND NOTHING ELSE, on the argument that §2.5.4's other half — "the CORS
 * settings attribute credentials mode", and the create-a-potential-CORS-request algorithm the section points
 * at — "is a FETCH decision, and the engine holds no network policy by construction". THE PREMISE IS RIGHT
 * AND THE CONCLUSION WAS BACKWARDS, and it is worth keeping the retired sentence rather than deleting it,
 * because a reader who re-derives it will re-introduce it. Holding no network policy means the engine does
 * not DECIDE whether to send cookies; it does not mean the engine may not SAY what the standard makes a
 * request's credentials mode. Those are the two halves CLAUDE.md separates by name — the engine states and
 * the zone decides — and this half is a fact about an ELEMENT's own attribute that no other party can know.
 * While the sentence stood, every request this engine built reached the trusted chokepoint with nothing to
 * decide from, and the chokepoint had to either default the question or refuse the request.
 * SO THE TWO ANSWERS ARE HERE NOW, AND THEY DISAGREE — see the header for why they are two entries. */
#include "check.h"
#include "core/html/cors_settings_attribute.h"

/* §2.5.4's table, in the section's own order. One keyword per state and no keyword at all for No CORS, so
   §2.3.3's first canonical-keyword arm applies to the two that have one. */
static const EnumeratedKeyword CORS_KEYWORDS[] = {
    { "anonymous",       CORS_ANONYMOUS },
    { "use-credentials", CORS_USE_CREDENTIALS },
    { NULL,              0 }
};

const EnumeratedAttribute CORS_SETTINGS_ATTRIBUTE = {
    CORS_KEYWORDS, CORS_NO_CORS, CORS_ANONYMOUS, CORS_ANONYMOUS
};

int cors_settings_attribute_state(const lxb_dom_element_t *el)
{
    return enumerated_attribute_state(el, "crossorigin", CORS_SETTINGS_ATTRIBUTE.keywords,
                                      CORS_SETTINGS_ATTRIBUTE.missing, CORS_SETTINGS_ATTRIBUTE.empty,
                                      CORS_SETTINGS_ATTRIBUTE.invalid);
}

/* THE ONE THING BOTH ANSWERS ASSERT. §2.3.3's determine the state cannot return ENUMERATED_NO_STATE for this
   attribute — §2.5.4 declares all three special states, so every element has one of the three — and a fourth
   value would be a table above this line that gained a row without a caller. Asserted rather than answered by
   a trailing arm, because a trailing arm is what makes the two functions below differ in silence. */
static void cors_state_check(int state)
{
    DCHECK(state == CORS_NO_CORS || state == CORS_ANONYMOUS || state == CORS_USE_CREDENTIALS,
           "a CORS settings attribute state that is none of §2.5.4's three reached a credentials-mode "
           "answer — the section declares a missing, an empty and an invalid value default, so §2.3.3's "
           "\"return no state\" is unreachable for this attribute and the table above is the whole domain");
    (void)state;
}

FetchCredentialsMode cors_potential_request_credentials(int cors_attribute_state)
{
    cors_state_check(cors_attribute_state);
    /* "Let credentialsMode be `include`." — then "If corsAttributeState is Anonymous, set credentialsMode to
       `same-origin`." The order is the algorithm's and is why No CORS keeps `include`: the only state the
       second sentence names is Anonymous. */
    return cors_attribute_state == CORS_ANONYMOUS ? FETCH_CREDENTIALS_SAME_ORIGIN : FETCH_CREDENTIALS_INCLUDE;
}

FetchCredentialsMode cors_settings_attribute_credentials(int cors_attribute_state)
{
    cors_state_check(cors_attribute_state);
    /* §2.5.4's table: No CORS and Anonymous share the `same-origin` row and Use Credentials is `include`. It
       is written as the table's own grouping rather than as the negation of the other answer, so a reader can
       check it against the section without inverting anything. */
    return cors_attribute_state == CORS_USE_CREDENTIALS ? FETCH_CREDENTIALS_INCLUDE
                                                        : FETCH_CREDENTIALS_SAME_ORIGIN;
}
