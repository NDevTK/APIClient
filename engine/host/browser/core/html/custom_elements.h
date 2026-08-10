/* CUSTOM ELEMENTS — HTML §4.13: the registry, the upgrade, and the lifecycle reactions. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CUSTOM_ELEMENTS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CUSTOM_ELEMENTS_H
#include <lexbor/dom/dom.h>
#include "quickjs.h"
#include "quickjs-step.h"

/* attributeChangedCallback's (localName, oldValue, newValue, NAMESPACE) — the widest lifecycle callback there
   is. FOUR, because DOM §4.9's "handle attribute changes" step 2 enqueues « attribute's local name, oldValue,
   newValue, attribute's namespace »: the namespace is the argument a page reads to tell an `xlink:href` from
   the null-namespace `href` it also observes, and a three-slot buffer silently drops it. */
#define CE_MAX_REACTION_ARGS 4

/* §4.13.6 STEP 4'S DRAIN, AS A STRUCT THE CALLING MACHINE EMBEDS. Invoking a reaction runs the page's code, so
   the drain parks — and it parks inside whichever machine is performing the `[CEReactions]` wrapper, which is
   the IDL member machine. So the state belongs to that machine (it must ride its `visit` for a fork and its
   release for the throw path) while the ALGORITHM belongs here. One struct, three lifecycle calls, and no
   second scheduler. */
typedef struct {
    JSValue  queue;    /* the element queue popped off the stack (owned), UNDEFINED before step 3 */
    uint32_t i;        /* the element cursor into it */
    uint8_t  phase;    /* the call request's own phase */
    JSValue  cb[2 + CE_MAX_REACTION_ARGS];   /* the call request buffer: [this, callback, args…] */
} CustomElementQueue;

void custom_elements_queue_init(CustomElementQueue *q);
void custom_elements_queue_visit(JSContext *ctx, CustomElementQueue *q, JSStepVisit *v);
void custom_elements_queue_release(JSContext *ctx, CustomElementQueue *q);

/* §4.13.6 step 1: `q` becomes the CURRENT element queue, for as long as the calling member's own steps run.
   There is no stack array — see custom_elements.c: a declared member's steps run inside one C activation of
   the IDL machine and a member parks by RETURNING, so the nesting §4.13.6's stack models is one frame deep by
   construction, and a shared stack would be baseline state written twice per DOM API call. */
void custom_elements_reactions_push(CustomElementQueue *q);
/* §4.13.6 step 3: no queue is current. Called on the way out of the same C activation. */
void custom_elements_reactions_pop(void);
/* §4.13.6 step 4: invoke the reactions in the queue this member filled, one per entry.
   Returns JS_STEP_CALL parked on one reaction (the caller returns it), or 0 when the queue is exhausted.
   `cb_result` is the answer to the previous park and is CONSUMED. */
int  custom_elements_reactions_invoke(JSContext *ctx, CustomElementQueue *q, JSValue cb_result,
                                      JSValue **out_cb, int *out_argc);

void custom_elements_init(JSContext *ctx);
void custom_elements_free(JSContext *ctx);

/* HTML §4.13.2's `[HTMLConstructor]` AS THIS REALM'S HTMLElement INTERFACE OBJECT. It is minted here and not
   where the other interface objects are because the algorithm is this component's — it walks the definition
   set and the construction stack — while WHICH interface carries it is html_element.c's. OWNED (consumed by
   the install). */
JSValue custom_elements_html_constructor(JSContext *ctx);

/* §4.13.4's "look up a custom element definition" by local name, for DOM §4.9 step 3 — the definition or
   JS_UNDEFINED. OWNED. Its ONE reader is `create an element`, which needs the definition to decide between
   step 5's synchronous Construct and step 6's plain creation. */
JSValue custom_elements_definition_for_name(JSContext *ctx, const char *name, size_t len);
/* The definition's constructor — DOM §4.9 step 5.1.1's `C`, the value `create an element` Constructs. OWNED. */
JSValue custom_elements_definition_constructor(JSContext *ctx, JSValueConst def);
/* DOM §4.9 steps 5.1.4.2-11: the checks the spec runs on what the page's constructor RETURNED, and the state
   it then writes onto it. They are here rather than in document.c because "result's custom element state" and
   "result's custom element definition" are this component's own record, and a second writer of them is a
   second answer to what a custom element is. Returns 0, or -1 having thrown the NotSupportedError/TypeError
   the step names. `local` is the local name `create an element` was given. */
int custom_elements_created_check(JSContext *ctx, JSValueConst result, JSValueConst def,
                                  lxb_dom_document_t *doc, const char *local, size_t len);
/* `window.customElements` — §4.13.4's CustomElementRegistry. */
void custom_elements_install(JSContext *ctx, JSValueConst global);

/* §4.13.3 "try to upgrade": called when an element enters the tree. A no-op unless its local name is defined
   and it has not been upgraded already. */
void custom_elements_try_upgrade(JSContext *ctx, lxb_dom_element_t *el);
/* §4.13.3's disconnected reaction — the twin of the upgrade, for an element LEAVING a document. A no-op unless
   the element was upgraded, because only an upgraded element has a lifecycle to react with. */
void custom_elements_disconnected(JSContext *ctx, lxb_dom_element_t *el);
/* §4.13.3's attribute-changed reaction. Called BEFORE the write, so the element still holds the old value;
   `val` is NULL for a removal. A no-op unless the element is upgraded and its definition OBSERVES this name.
   THE ATTRIBUTE IS NAMED THE WAY §4.9 NAMES IT — (namespace, LOCAL name), `ns` NULL for the null namespace —
   because §4.9's "handle attribute changes" step 2 enqueues the reaction with « local name, oldValue, newValue,
   NAMESPACE » and the observed-attributes filter is over the LOCAL name: a qualified name would neither match
   `observedAttributes` for a prefixed attribute nor be able to supply the fourth argument at all. */
void custom_elements_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                       const char *val, size_t val_len);

#endif
