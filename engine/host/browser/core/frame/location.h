/* HTML §7.2.4 "The Location interface" — Blink core/frame. See location.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_LOCATION_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_LOCATION_H
#include "quickjs.h"

/* THE TWO SOURCES THIS COMPONENT OWNS, SPELLED ONCE. A source is TWO strings that must agree — the PROVENANCE
   `location.hash` that concolic_declare_source registers and every @S record names, and the DISPLAY SHAPE
   `{location.hash}` the @H surface reports a param's value as — and they were three separate string literals
   inside location.c with nothing tying them together. So a consumer had no token to bind to and had to retype
   the spelling: the offscreen invented a `{hash}|{search}|{pm}|{reply}` taxonomy (deleted, see
   solver/concolic.h), and test_forced.c's `loc-hash-param` row asserted `{hash}` — a name READ in one place and
   WRITTEN in none, which reads 0 forever while every mechanism under it is correct.
   THE SHAPE IS COMPOSED FROM THE PROVENANCE HERE, so the pair cannot drift, and solver/concolic.c asserts that
   composition at the mint for EVERY declared source (all five spell `{` src `}`). A consumer of either half
   uses these and never a literal of its own. */
#define LOCATION_HASH_SRC     "location.hash"
#define LOCATION_HASH_SHAPE   "{" LOCATION_HASH_SRC "}"
#define LOCATION_SEARCH_SRC   "location.search"
#define LOCATION_SEARCH_SHAPE "{" LOCATION_SEARCH_SRC "}"

/* Declared ONCE PER AGENT: the two attacker SOURCES this component owns and how a browser delivers each (a
   source's delivery is a fact about the COMPONENT, not about a document), the brand class, the per-realm slot,
   and the per-realm install this REGISTERS. §7.2.4 gives every Window "a unique instance of a Location object,
   allocated when the Window object is created", so the object is built WITH the realm and no host has an
   install line to remember. */
void location_init(JSContext *ctx);
/* The AGENT's half undone — core/platform.h's third column. It gives back the brand, the per-realm slot id and
   the two SOURCE claims this component holds in solver/concolic.c's registry. */
void location_free(void);

/* THIS REALM'S Location object — §7.2.4's "the Window object's location getter steps are to return this's
   Location object". OWNED: the caller frees.
   IT IS A FUNCTION AND NOT A PROPERTY READ, because `window.location` is an IDL ACCESSOR now and reading an
   accessor from C is the one thing this interpreter refuses (there is no flow base under a C activation). Two
   components ask — Document's §3.1.1 `location` and the WindowProxy's §7.2.2 `location` — and both used to
   reach through the global with JS_GetPropertyStr, which would abort at the getter. */
JSValue location_object(JSContext *ctx);

/* §7.2.4's LOCATION URL IS THE DOCUMENT'S ADDRESS — "a Location object has an associated url, which is this
   Location object's relevant Document's URL" — and it is read from the DOCUMENT (document_url(ctx)) rather
   than recorded here. This component kept a module-static copy that every install overwrote, which was one
   answer for a heap that now holds one realm per same-origin document: materializing a popup rewrote its
   OPENER'S copy, so the opener's next relative fetch resolved against the popup's address. A per-realm fact
   stored per agent is the same defect as `name` having had two sources, and the Document already holds it.
   IT IS THE ADDRESS AND NOT §2.4.3's DOCUMENT BASE URL, which this header used to call the same thing. They
   differ exactly when a page ships `<base href>`: the API base URL moves and `location.href` does not, so a
   Location reading the base URL would report an address the navigable was never at. THE SETTERS NEED THE
   OTHER ONE — §7.2.4's href setter, `assign` and `replace` encoding-parse relative to the entry settings
   object, whose API base URL is §7.2.2.6 "Script settings for Window objects"'s "the current base URL of
   window's associated Document" — so this component reads BOTH and the two are never one call.
   THE SECTION USED TO BE CITED AS §8.1.5.1, AND THAT IS "The resolution algorithm" — a real section that
   READS an API base URL for module specifiers and defines nothing about this one. The field is declared in
   §8.1.3.2 "Environment settings objects" and a Window's value is §7.2.2.6's. The wrong number reads as
   authoritative and sends the next reader to a section that does not say what the code claims, which is the
   one thing a citation must not do; core/dom/document.h carries the same wrong number and is not this
   component's to edit. */

#endif
