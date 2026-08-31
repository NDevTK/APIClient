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
 * THIS FILE STATES THE ENUMERATION AND NOTHING ELSE. §2.5.4's other half — "the CORS settings attribute
 * credentials mode", and the create-a-potential-CORS-request algorithm the section points at — is a FETCH
 * decision, and the engine holds no network policy by construction. */
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
