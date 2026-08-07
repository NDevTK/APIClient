/* THE POLICY CONTAINER — HTML §7.2.6. See policy_container.c.
 *
 * ONE INSTANCE PER AGENT, NOT PER DOCUMENT. This file is where that correction is written down, because it is
 * the initial about:blank that forces it. A same-origin child created with no URL — `window.open()`, an
 * `<iframe>` with no src — gets its Document SYNCHRONOUSLY, in the creator's own agent, and is scripted
 * synchronously: `iframe.contentWindow.document.body` must answer on the next line. No message-passing scheme
 * can do that, which is why a real browser does not use one: the child is created in place, and its policy
 * container is a CLONE of the creator's rather than something fetched.
 *
 * So the boundary that deserves a separate WASM instance is the AGENT — the same-origin group — and not the
 * document. Two same-origin documents share a heap, a scheduler and a set of flows, and script each other
 * synchronously. A CROSS-ORIGIN document is the case that gets its own instance, and there HTML never asks for
 * synchronous access: the entire cross-origin surface is postMessage and a fixed whitelist, which is exactly
 * what survives being asynchronous and going over a message port. That is the shape of the cross-WASM work,
 * and it is much smaller than "any document might be remote" would have made it. */
#ifndef ENGINE_HOST_BROWSER_CORE_FRAME_POLICY_CONTAINER_H
#define ENGINE_HOST_BROWSER_CORE_FRAME_POLICY_CONTAINER_H
#include <stdbool.h>

typedef struct PolicyContainer PolicyContainer;

/* From a document's own headers/meta. `csp_text` is the serialized policy list or NULL; both are copied. */
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
