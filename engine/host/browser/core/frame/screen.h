/* CSSOM VIEW §4.3's Screen interface — Blink core/frame. See screen.c. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_SCREEN_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_SCREEN_H
#include <stdbool.h>

#include "quickjs.h"

/* Declared ONCE PER AGENT: the brand class, the two per-realm slots, and the per-realm install this REGISTERS,
   so no host has an install line to remember. §3.7 gives every realm its own Screen.prototype, its own `Screen`
   interface object and its own Screen — a host that does not want the interface declines it by not calling
   this, which is how every other per-realm component is declined. */
void screen_init(JSContext *ctx);
void screen_free(void);

/* THE MODELLED DISPLAY'S BIT DEPTH — the EXAMPLE `screen.colorDepth` carries, as a plain number. MEDIA QUERIES
   §4.5's `color` feature is the second reader of that one fact (bits per RGB component is this divided by
   three), and a media-feature table with its own constant would be a second answer to it. */
int screen_color_depth(void);

/* THE MODELLED DISPLAY'S GEOMETRY, in CSS pixels — the EXAMPLES §4.3's four size members carry, as plain
   numbers, and the numbers themselves rather than the concolics wrapping them.
   THREE STANDARDS READ THIS ONE FACT and two of them had already written their own copy of it. §4.3 exposes it;
   MEDIA QUERIES §12's `device-width` and `device-height` report it, and media_query.c held a second literal
   1920 and 1080 for exactly that until this existed; and CSSOM VIEW §2.3's WEB-EXPOSED AVAILABLE SCREEN AREA is
   the area a CLIENT WINDOW is positioned inside, which is what viewport.c derives `screenX`/`screenY` from.
   `avail` is that available area — §2.3's separate term, and a separate fact: `availHeight < height` is the
   "does the OS reserve a taskbar" question, and one shared source would tie it to the size question. */
double screen_width(void);
double screen_height(void);
double screen_avail_width(void);
double screen_avail_height(void);

/* …AND THE MEMBER THOSE NUMBERS ARE THE EXAMPLE OF, BY NAME — for a component whose OWN value is a JOINT
 * function of the available area and must name this member as one of the joint's members.
 *
 * WHY A COMPONENT NEEDS THIS AND NOT JUST THE NUMBER. §4.3's `availWidth` reaches the page as a concolic: the
 * number above is its EXAMPLE and `{screen.availWidth}` is the hole a report looks a domain up by. A value
 * DERIVED from it — viewport.c's `screenX` is `(availWidth − outerWidth) / 2` — is a joint function of this
 * fact and one of its own, and solver/concolic.h's `concolic_source_wrap_joint` composes an identity out of
 * the MEMBERS' own identities. So the deriving component needs this member's identity, not merely its value,
 * and the two halves must be the same two strings this file's own mint used: a joint composed from a second
 * spelling names a hole nothing mints, and `concolic_hole_key` then answers for a set under a key no emission
 * can find — a constraint observed, stored, and unreadable.
 *
 * BOTH ARE BORROWED and live for the agent: they are string literals out of the one member X-list.
 * `vertical` picks the axis — false is `availWidth`, true is `availHeight` — and they are two members and not
 * one for the reason above: `availHeight < height` is a different question from `availWidth < width`. */
void screen_avail_source(bool vertical, const char **shape, const char **src);

#endif
