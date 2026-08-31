/* HTML §2.5.4 "CORS settings attributes". See cors_settings_attribute.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CORS_SETTINGS_ATTRIBUTE_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CORS_SETTINGS_ATTRIBUTE_H

#include "core/html/enumerated_attribute.h"

/* §2.5.4's states. The names are the section's own ("Anonymous", "Use Credentials", and the No CORS state its
   defaults sentence names), and No CORS is the one with NO KEYWORD — which is not an omission but the whole
   reason §2.6.1's getter has a branch for a state with no associated keyword value: `<img>` with no
   `crossorigin` attribute reflects the empty string, not a word. */
enum { CORS_NO_CORS = 0, CORS_ANONYMOUS, CORS_USE_CREDENTIALS };

/* §2.5.4's attribute as §2.3.3 defines it — two keywords, and three special states that are NOT all the same:
   "the attribute's missing value default is the No CORS state, and its invalid value default and empty value
   default are both the Anonymous state". `crossorigin=""` is therefore Anonymous and reflects "anonymous",
   while an absent attribute reflects "". */
extern const EnumeratedAttribute CORS_SETTINGS_ATTRIBUTE;

#endif
