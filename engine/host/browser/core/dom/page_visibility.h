/* PAGE VISIBILITY — HTML §6.6, `document.visibilityState` and `document.hidden`.
 *
 * ONE FACT BEHIND TWO MEMBERS, and that is the whole design. §6.6 defines `hidden` as "true if
 * visibilityState is 'hidden'", so the two are not two pieces of state — one is a comparison over the other.
 * Minting a concolic source for EACH would be the defect CLAUDE.md names most often: one fact answered from
 * two places, so a flow that decided `document.hidden` would learn nothing about `visibilityState === "hidden"`
 * and the solver would fork the same question twice and believe contradictory things in one world.
 * So `visibilityState` IS the source and `hidden` is `concolic_new_cmp(source, ==, "hidden")` — the spec's own
 * sentence, expressed in the value type. Both members then key the SAME path constraint.
 *
 * IT IS A SOURCE RATHER THAN A CONSTANT because a real user CAN background the tab, and a bundle's
 * `if (document.hidden) return;` is a gate with real code behind it — polling loops, analytics beacons,
 * refresh timers, the endpoints they call. Collapsing it to a concrete `false` deletes that arm and everything
 * it reaches, which is what CLAUDE.md means by never collapsing a modelable value to bare-concrete. It carries
 * the modelled UA answer as its EXAMPLE ("visible" / false), so a page that merely READS it gets what a
 * headless UA truthfully presents, while a page that BRANCHES on it explores both worlds.
 *
 * A CONFORMANCE HOST GETS THE PLAIN VALUE. concolic_source_wrap returns its computed argument unchanged where
 * no source overlay is installed, so the WPT runner sees `"visible"` and `false` — the spec's answers, which
 * is exactly what a conformance run must measure. Nothing here needs to know which host it is in. */
#ifndef ENGINE_HOST_BROWSER_CORE_DOM_PAGE_VISIBILITY_H
#define ENGINE_HOST_BROWSER_CORE_DOM_PAGE_VISIBILITY_H

#include <stdbool.h>

#include "quickjs.h"

/* §6.6's two members, onto Document.prototype — called from document_install_proto, like every other
   component that puts something there. */
void page_visibility_install(JSContext *ctx, JSValueConst proto);

/* IS THIS DOCUMENT HIDDEN, as the ENGINE must decide it — HTML §8.1.7.3 step 3 removes a doc whose visibility
   state is "hidden", and that step is C and cannot fork. It CONCRETIZES ON THE PIN: it asks the flow's own
   path constraint what it has already decided about this exact predicate (decide_value_arm, by value, so the
   key has one speller) and falls back to the modelled example when the flow has committed to neither. A flow
   exploring the backgrounded world therefore gets no rendering opportunity, which is what a browser does to a
   hidden tab, and the primary flow keeps its frames. */
bool page_visibility_hidden(JSContext *ctx);

#endif
