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
#include "core/frame/opener_policy.h"
/* §7.1.7's POLICY CONTAINER, in the form it crosses this seam — a Document is created with one, so the struct
   below holds one and this unit is a direct user of the type. */
#include "core/frame/policy_container.h"
#include "core/frame/sandboxing.h"

/* THE AGENT'S FACTS — what a declaration needs that is true of the whole similar-origin window agent. */
typedef struct {
    /* The agent's PRINCIPAL. §9.5's named bus is keyed by it and §7.2.1's proxy decides remoteness by it. */
    const char *origin;
    /* HTML §8.1.3.1's TOP-LEVEL CREATION URL, which §8.1.3.5 reads to decide whether the first realm is a
       SECURE CONTEXT — and Web IDL §3.3.13's members exist or do not by that answer. It is the environment's,
       not the document's: an `https` iframe of an `http` page holds the `http` address here. */
    const char *top_level_url;
    /* §7.5.1's `requestsOAC`, which §8.1.2.2's obtain-a-similar-origin-window-agent allocates this agent's
       CLUSTER with — the `Origin-Agent-Cluster` response header, parsed as a structured-field boolean and
       cleared for a non-secure context (core/frame/navigation_params.h).
       IT IS AN AGENT FACT AND NOT A DOCUMENT ONE, which is why it is on this struct rather than beside the
       policy on the document below: §8.1.2.2 allocates a cluster ONCE per agent, and §7.1.2's own note says a
       later same-origin Document in the same group inherits that allocation through the historical agent
       cluster key map even when it sends a different header. The document that roots the agent is the one
       whose header decides it. */
    bool requests_oac;
    /* §7.1.3's OPENER POLICY VALUE of the response that created the document this agent is rooted at, which
       §7.3.2.3's create-a-new-browsing-context-group takes because §7.1.3.2's swap is the only step that ever
       gives a group a cross-origin isolation mode other than `none` (core/frame/browsing_context_group.h).
       IT IS AN AGENT FACT FOR THE SAME REASON `requests_oac` above is, and a STRONGER one: SECURITY.md keys an
       instance on `(browsing context group, origin)`, so the group is half the key that decided there is one
       instance here — a navigable whose navigation swaps groups leaves this instance rather than changing this
       value. It is the VALUE and not the whole policy because a group reads only that: the reporting endpoints
       and the report-only half belong to the DOCUMENT, which carries them on its navigable
       (core/frame/window_proxy.h). */
    OpenerPolicyValue opener_policy;
} PlatformAgent;

/* ONE DOCUMENT'S FACTS. `url` and `origin` ARE TWO DIFFERENT FACTS and this struct is why they can no longer
   be confused: the ADDRESS is what §4.4's API base URL resolves `fetch("api/users")` against, the PRINCIPAL is
   what a same-origin check compares. A host that had one field passed whichever it had. */
typedef struct {
    lxb_html_document_t *dom;    /* the parsed tree this realm's `document` is a wrapper over */
    const char          *url;    /* the document's ADDRESS */
    const char          *origin; /* the document's PRINCIPAL */
    /* HTML §7.1.7's POLICY CONTAINER this Document is created with — §7.5.1's own creation table row
       ("policy container … navigationParams's policy container"), decided by §7.1.7's determine step and never
       re-derived here.
       IT IS ONE VALUE AND NOT ONE FIELD PER ITEM, which is what makes the container's next item impossible to
       drop on the way in: it is built through the one constructor that names every item, so a host that stops
       stating one stops compiling. Its CSP list's SELF-ORIGIN is a THIRD fact beside `url` and `origin` and
       not a spelling of either — §2.2.2 sets it from the RESPONSE's URL, §7.3.2.1's clone carries the
       CREATOR's into a document that came from no response, and §2.2's own note says why the distinction is
       the point: a document with an OPAQUE origin which inherited its policy still resolves `'self'` against
       the origin that policy came from. A host that passed `origin` for it would be right for every
       unsandboxed top-level document and silently wrong for exactly those two cases.
       IT ALWAYS EXISTS: every Document has a container, including the initial about:blank. */
    SerializedPolicyContainer policy;
    /* HTML §7.1.5's ACTIVE SANDBOXING FLAG SET this Document is created with. A SEPARATE fact from `csp` for
       the same reason `url` and `origin` are separate from each other: §7.1.7's policy container holds no
       flag set, so a host that derived one from the policy would answer a question the container was never
       asked. §7.5.1 lists them as two rows of one creation, and this struct is where the pair travels. */
    SandboxFlags         sandbox_flags;
    uint32_t             doc_id; /* the world registry's name for this document */
    JSValueConst         nav_proxy;   /* §7.2.3's ONE WindowProxy for the navigable this is active in */
} PlatformDocument;

/* THE AGENT HALF — every component's declaration, once per JSRuntime, in dependency order, ending in the one
   call every realm goes through (realm_install_intrinsics), so the agent's FIRST realm is built by exactly the
   mechanism a child navigable's realm is. */
void platform_agent_init(JSContext *ctx, const PlatformAgent *agent);

/* THE DOCUMENT HALF — every component's per-realm install, run once per document INCLUDING the first, so a
   same-origin child navigable's window is the same window the top-level document got. A child whose `window`
   is smaller is a different browser. */
void platform_document_install(JSContext *ctx, JSValueConst global, const PlatformDocument *doc);

/* THE AGENT HALF, UNDONE — every component's agent-lifetime release, in the reverse of the declaration order,
 * before the runtime is freed.
 *
 * IT IS THE THIRD COLUMN OF THE SAME ROW, and it is here for the reason the other two are. A component that
 * holds state for the whole AGENT rather than for a realm — RFC 6265 §5.3's cookie store is the first — has
 * nothing that goes with a JSContext, so it needs a call at teardown, and a call at teardown written into each
 * host is the hand-copied list this file was built to abolish: the same list, in the same three places, drifting
 * the same way (and it is not hypothetical — the shipped ABI entry's teardown had already drifted four frees
 * from the fixture's, and the gate could not see it because the gate runs the other entry's main). A host calls
 * this ONCE, and what it releases is what this browser declared.
 *
 * A RELEASE IS NOT A STEP MACHINE'S. It frees what a C static holds for the agent, which no `visit` names and
 * no JSContext frees — a per-realm value belongs in core/realm.h's slot store, which the runtime already frees
 * with the context, and a row that reaches for this column for one of those is answering the wrong question.
 *
 * A JSContext IN THE SIGNATURE IS NOT THE TEST. Read the wrong way round, the paragraph above says a component
 * whose `_free` takes a JSContext belongs somewhere else — and that reading is what kept forty-odd components
 * off this column while each of them held nothing but agent state. `event_target_free` took a ctx to drop three
 * Symbols; `timer_free` took one to give back two atoms; every one of them says in its own comment that "the
 * prototypes are the REALMS' — released with their contexts". The test is WHAT A RELEASE GIVES BACK, and the
 * signature follows from it: JS_FreeValue is JS_FreeValueRT(ctx->rt, v), so a release that only drops values
 * takes the runtime and becomes a row. `document_free` is the one release that fails the test — it reads
 * `doc_of(ctx)` and clears the realm's own opaque — and it is the reason the paragraph exists, not a pattern.
 *
 * WHICH IS ABOUT A RELEASE AND NOT ABOUT A COMPONENT, and reading it the second way is what kept `document` off
 * this column entirely while it held a class, fifteen pool entries, two realm-value slot ids, TEN
 * sub-components and a claim on the ONE frontier. A component may have BOTH halves, and `document` is the one
 * that does: `document_free` releases a REALM's records, once per realm; `document_agent_free` is its row here.
 * THE TWO RUN AT DIFFERENT PHASES AND NEITHER IS ORDERED AGAINST THE OTHER, because a child navigable's
 * document is released from quickjs's realm-teardown hook — inside JS_RunGC or JS_FreeRuntime, both after this
 * whole list. What makes that safe is that the per-realm half reads no static of its file, which that file
 * states and asserts at the release rather than leaving to an ordering nothing could arrange.
 *
 * A COMPONENT CAN ALSO HOLD A SLOT IN ANOTHER COMPONENT, and that is the fourth thing this column is for. A
 * hook, a tree walk, an activation behaviour, a census callback: the STORAGE is the other component's static
 * and the CLAIM is this one's agent state, so the claimant gives it back at its own release and the holder
 * asserts, at its own, that nothing is left pointing into it. That assert is what turns the order between the
 * two into a checked fact — a claimant released after its holder is the scheduler-holding-a-freed-component
 * defect core/agent_state.h records for Indexed Database §2.7.1, and reverse declaration order is what places
 * them correctly, since a component that claims a slot in another one is declared after it. */
void platform_agent_free(void);

#endif
