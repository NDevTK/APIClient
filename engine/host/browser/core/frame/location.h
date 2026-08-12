/* The Location interface — Blink core/frame. Installed on the baseline from the document's own URL. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_LOCATION_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_LOCATION_H
#include "quickjs.h"

/* Install `location` (and `document.URL`'s eventual source) built from `url` — the document's address, which
   the host captured. A NULL or empty url installs nothing: a document with no address has no Location, and the
   page's own throw on reading it is the honest answer. */
/* THE AGENT'S HALF: the two attacker SOURCES this component owns and how a browser delivers each. A source's
   delivery is a fact about the component, so it is declared once per agent — not once per document. */
void location_init(JSContext *ctx);

void location_install(JSContext *ctx, JSValueConst global, const char *url);

/* HTML'S API BASE URL IS THE DOCUMENT'S ADDRESS, and it is read from the DOCUMENT — document_base_url(ctx) —
   rather than recorded here. This component kept a module-static copy that every install overwrote, which was
   one answer for a heap that now holds one realm per same-origin document: materializing a popup rewrote its
   OPENER'S base, so the opener's next relative fetch resolved against the popup's address. A per-realm fact
   stored per agent is the same defect as `name` having had two sources, and the Document already holds it. */

#endif
