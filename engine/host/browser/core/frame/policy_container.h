/* THE POLICY CONTAINER — HTML §7.2.6. See policy_container.c.
 *
 * IT CROSSES INSTANCES, and the initial about:blank is what forces that. A child created with no URL —
 * `window.open()`, an `<iframe>` with no src — gets its Document synchronously and has no response to take a
 * policy from; §7.4 says its container is a CLONE OF THE CREATOR'S. One WASM instance is one DOCUMENT
 * regardless of origin, so that clone is a CROSS-INSTANCE operation: the creator's container is serialized to
 * the child's instance, and the requesting flow SUSPENDS across the boundary the same way it suspends at an
 * await. Same-origin is not an exemption — co-locating two documents to make the clone a local memcpy is
 * dodging the transport, and it is the one design this file must not encourage.
 *
 * WHICH IS WHY IT IS A VALUE, NOT A POINTER GRAPH. Everything here is a flat parse over one owned string, so a
 * container serializes to its `csp_text` and reconstitutes by parsing it again — the clone that crosses an
 * instance and the clone that crosses a session are then the same operation, and neither needs a live heap. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_POLICY_CONTAINER_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_POLICY_CONTAINER_H
#include <stdbool.h>

typedef struct PolicyContainer PolicyContainer;

/* From a document's own headers/meta. `csp_text` is a SERIALIZED CSP LIST — CSP §2.2, comma-delimited, which
   is how several policies travel in one header and how several `<meta>` elements compose. It is a list rather
   than a single policy because the policies are enforced INDEPENDENTLY: a resource must be allowed by every
   one of them, so a second policy can only ever narrow. Both arguments are copied; either may be NULL. */
PolicyContainer *policy_container_new(const char *csp_text, const char *referrer_policy);

/* §7.2.6's "clone a policy container" — §7.4 performs this for a navigable created with a creator, which is
   how an initial about:blank inherits its CSP. A DEEP copy: the child's policy is its own from the moment it
   exists, so a later navigation of the parent cannot reach back and change what the child may do. */
PolicyContainer *policy_container_clone(const PolicyContainer *src);
void policy_container_free(PolicyContainer *p);

/* The serialized policy, for a report that has to name what blocked it. BORROWED; NULL for no policy. */
const char *policy_container_csp(const PolicyContainer *p);

/* WOULD THIS RUN? §S says a firing breakout in the model is not an exploit until it survives the page's actual
   policy — an inline `onerror` is dead under `script-src 'self'`, and the honest report is then "sink REAL,
   CSP blocks: needs X" rather than a bare XSS. These are the four questions a breakout turns on. */
typedef enum {
    POLICY_INLINE_SCRIPT = 0,   /* <script>…</script> injected into the document */
    POLICY_INLINE_HANDLER,      /* onerror=…, onload=… — cannot carry a nonce */
    POLICY_JAVASCRIPT_URL,      /* a javascript: URL navigated to — cannot carry a nonce either */
    POLICY_EVAL,                /* eval, new Function, setTimeout(string) */
} PolicyScriptKind;
bool policy_allows(const PolicyContainer *p, PolicyScriptKind kind);

#endif
