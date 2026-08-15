/* THE PLATFORM THIS ENGINE IS, IN ONE PLACE EVERY HOST GOES THROUGH.
 *
 * core/realm.h abolished the hand-copied list ONE LEVEL DOWN: a component declares its per-realm intrinsic
 * into one list, and every realm runs that list, so a second realm cannot get a smaller set than the first.
 * The list ONE LEVEL UP was still hand-copied three times. Each host wrote out, by hand, which components an
 * AGENT declares and which a DOCUMENT installs — engine_agent_init/engine_realm_install in main.c,
 * wpt_agent_init/wpt_realm_install in wpt_runner.c, tf_agent_init/tf_realm_install in test_forced.c — and the
 * three had drifted exactly the way a hand-maintained list drifts:
 *
 *   - wpt_runner.c was missing FIVE components — navigator, screen, storage_manager, unhandled_rejection,
 *     module_loader — plus §8.1.7.2's Window event handlers. The first of those is the expensive one: the
 *     WPT GATE HAD NO `navigator`, so `html/webappapis/…/the-navigator-object` was collected and failing on a
 *     missing global rather than on the component, and everything hanging off a partial interface of
 *     Navigator (Permissions §6.2, Storage §3 and the File System Access surface `navigator.storage`
 *     reaches, HTML §6.4.4's UserActivation) had no door into that runner at all.
 *   - wpt_runner.c also handed window_install the ORIGIN where the other two hand it the ADDRESS — one
 *     argument, silently wrong, in the one host whose whole job is measuring fidelity.
 *   - test_forced.c was missing TWENTY-ONE components its own comment claimed it had ("the components the ABI
 *     entry installs, so this fixture runs the engine that ships"): url, url_search_params, blob, all four
 *     stream standards, encoding and text_stream, message_port, xml_http_request, broadcast_channel,
 *     window_message, structured_clone, message_event, error_event, the three File System Access components,
 *     storage_manager and module_loader.
 *
 * None of those is a decision anybody made. They are what a list becomes when there are three of it. So there
 * is ONE, here, and a host cannot express an omission at all: it calls platform_agent_init and
 * platform_document_install, and what it gets is what this browser is.
 *
 * A COMPONENT IS ONE ROW, WITH BOTH HALVES ON IT. The agent half (declare the class, the slots and the pool
 * entries, once per JSRuntime) and the document half (install the members on one realm's global, once per
 * JSContext) are two lists everywhere else, and a component present in one and absent from the other is the
 * next shape of this bug. On a row they cannot separate.
 *
 * WHAT IS *NOT* HERE IS THE HOST'S EDGES, and that boundary is the whole reason this file can be one list.
 * A component is WHAT THE PLATFORM IS; an edge is WHO ANSWERS — the network provider (fetch_set_provider),
 * the string-handler sink (timer_set_script_sink), the child-realm builder (navigable_set_realm_builder), the
 * runner's own `print`/`gc`, the fixture's sinks. Those genuinely differ per host, they are already
 * PARAMETERS rather than presence, and each component asserts its own edge at the first use that needs it —
 * fetch.c aborts on a fetch with no provider, which is the two-sided half an omission list could never have.
 * A host adds its edges and its own globals AFTER this call; it can add, and it cannot subtract. */
#ifndef ENGINE_HOST_BROWSER_CORE_PLATFORM_H
#define ENGINE_HOST_BROWSER_CORE_PLATFORM_H

#include <stdint.h>

#include <lexbor/html/html.h>

#include "quickjs.h"

/* THE AGENT'S FACTS — what a declaration needs that is true of the whole similar-origin window agent. */
typedef struct {
    /* The agent's PRINCIPAL. §9.5's named bus is keyed by it and §7.2.5.1's proxy decides remoteness by it. */
    const char *origin;
    /* HTML §8.1.3.1's TOP-LEVEL CREATION URL, which §8.1.3.5 reads to decide whether the first realm is a
       SECURE CONTEXT — and Web IDL §3.3.13's members exist or do not by that answer. It is the environment's,
       not the document's: an `https` iframe of an `http` page holds the `http` address here. */
    const char *top_level_url;
} PlatformAgent;

/* ONE DOCUMENT'S FACTS. `url` and `origin` ARE TWO DIFFERENT FACTS and this struct is why they can no longer
   be confused: the ADDRESS is what §4.4's API base URL resolves `fetch("api/users")` against, the PRINCIPAL is
   what a same-origin check compares. A host that had one field passed whichever it had. */
typedef struct {
    lxb_html_document_t *dom;    /* the parsed tree this realm's `document` is a wrapper over */
    const char          *url;    /* the document's ADDRESS */
    const char          *origin; /* the document's PRINCIPAL */
    const char          *csp;    /* §7.2.6's header half, or NULL for a document that came from no response */
    uint32_t             doc_id; /* the world registry's name for this document */
    JSValueConst         nav_proxy;   /* §7.2.5.1's ONE WindowProxy for the navigable this is active in */
} PlatformDocument;

/* THE AGENT HALF — every component's declaration, once per JSRuntime, in dependency order, ending in the one
   call every realm goes through (realm_install_intrinsics), so the agent's FIRST realm is built by exactly the
   mechanism a child navigable's realm is. */
void platform_agent_init(JSContext *ctx, const PlatformAgent *agent);

/* THE DOCUMENT HALF — every component's per-realm install, run once per document INCLUDING the first, so a
   same-origin child navigable's window is the same window the top-level document got. A child whose `window`
   is smaller is a different browser. */
void platform_document_install(JSContext *ctx, JSValueConst global, const PlatformDocument *doc);

#endif
