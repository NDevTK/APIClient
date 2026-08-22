/* CUSTOM ELEMENTS — HTML §4.13: the registry, the upgrade, and the lifecycle reactions. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CUSTOM_ELEMENTS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CUSTOM_ELEMENTS_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/report_exception.h"

/* attributeChangedCallback's (localName, oldValue, newValue, NAMESPACE) — the widest lifecycle callback there
   is. FOUR, because DOM §4.9's "handle attribute changes" step 2 enqueues « attribute's local name, oldValue,
   newValue, attribute's namespace »: the namespace is the argument a page reads to tell an `xlink:href` from
   the null-namespace `href` it also observes, and a three-slot buffer silently drops it. */
#define CE_MAX_REACTION_ARGS 4

/* DOM §4.9's CUSTOM ELEMENT STATE — the five values, and they are five because §4.13.5 step 1 branches on
   exactly this set. It was a BOOLEAN ("does this wrapper carry a definition?"), which cannot tell an element
   whose upgrade THREW from one that was never tried: §4.13.5 returns early for "undefined" and "uncustomized"
   and for nothing else, so a failed element was upgraded again on every re-insertion and its constructor ran
   a second time. "precustomized" is the window between step 8.2 and the constructor returning, which is what
   DOM §4.9 step 5.1.4's assert distinguishes an element that reached `super()` by. */
typedef enum {
    CE_STATE_UNCUSTOMIZED = 0,   /* a local name §4.13.1 could never accept — the default for most elements */
    CE_STATE_UNDEFINED,          /* a valid custom element name with no definition committed for it yet */
    CE_STATE_FAILED,
    CE_STATE_PRECUSTOMIZED,
    CE_STATE_CUSTOM
} CustomElementState;

/* §4.13.6's REACTION TYPE. The spec's invoke SWITCHES on it, and the two arms run different algorithms with
   different park shapes — a callback reaction CALLS a function, an upgrade reaction runs §4.13.5 and
   CONSTRUCTS a class. Stored as the reaction's own first element rather than inferred from whether entry 0
   happens to be callable, because a definition and a callback are both objects and a shape sniff would make
   the two arms one bug apart. */
enum { CE_REACTION_CALLBACK = 0, CE_REACTION_UPGRADE = 1 };

/* §4.13.6 STEP 4'S DRAIN, AS A STRUCT THE CALLING MACHINE EMBEDS. Invoking a reaction runs the page's code, so
   the drain parks — and it parks inside whichever machine is performing the `[CEReactions]` wrapper, which is
   the IDL member machine. So the state belongs to that machine (it must ride its `visit` for a fork and its
   release for the throw path) while the ALGORITHM belongs here. One struct, three lifecycle calls, and no
   second scheduler. */
typedef struct {
    JSValue  queue;    /* the element queue popped off the stack (owned), UNDEFINED before step 3 */
    uint32_t i;        /* the element cursor into it */
    uint8_t  phase;    /* the call or construct request's own phase — one reaction is in flight at a time */
    /* §4.13.5's OWN CURSOR while the reaction being run is an UPGRADE. The upgrade parks on the page's class
       (step 8.3's Construct), so it needs a resume point of its own inside the one reaction the drain is on. */
    uint8_t  up_stage;
    /* §4.13.6 step 1.3.1's upgrade arm: "If this throws an exception, catch it, and report it". The throw is
       the algorithm's VALUE here, so the exception is held and HTML §8.1.4.6's report runs as a request — it
       fires an `error` event, which is the page's code and therefore another park. */
    uint8_t  reporting;
    JSValue  exc;
    ReportExceptionWork rep;
    /* §4.13.6 step 1.3's "REMOVE the first element of reactions, and let reaction be that element" — the
       reaction and the element it belongs to, held HERE because the removal happens BEFORE the reaction runs
       and the run parks. Re-reading them off the element's queue at the resume instead is what made a
       re-entrant drain (a `[CEReactions]` member inside a constructor, dequeuing the SAME element) see the
       in-flight reaction still at the head and run it a second time. Both owned. */
    JSValue  cur;
    JSValue  cur_el;
    JSValue  cb[2 + CE_MAX_REACTION_ARGS];   /* the call request buffer: [this, callback, args…] */
} CustomElementQueue;

/* WHICH ARM OF §4.13.6 step 1.3.1 THE DRAIN IS PARKED IN, so the calling machine can name its resume point as
   the spec step it actually is rather than as "somewhere in step 4". Read after the invoke returns a park. */
enum { CE_ARM_CALLBACK = 0, CE_ARM_UPGRADE = 1, CE_ARM_REPORT = 2 };
int custom_elements_queue_arm(const CustomElementQueue *q);

void custom_elements_queue_init(CustomElementQueue *q);
void custom_elements_queue_visit(JSContext *ctx, CustomElementQueue *q, JSStepVisit *v);
/* The half of this record's teardown that is NOT a reference — the error-reporting-mode flag the drain may be
   holding on the global. A step machine whose `visit` names this queue discharges its references through that
   one declaration and calls THIS; see report_exception_work_unlock for why the split exists. */
void custom_elements_queue_unlock(JSContext *ctx, CustomElementQueue *q);

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
void custom_elements_free(JSRuntime *rt);

/* HTML §4.13.2's `[HTMLConstructor]` AS THIS REALM'S HTMLElement INTERFACE OBJECT. It is minted here and not
   where the other interface objects are because the algorithm is this component's — it walks the definition
   set and the construction stack — while WHICH interface carries it is html_element.c's. OWNED (consumed by
   the install). */
JSValue custom_elements_html_constructor(JSContext *ctx);

/* §4.13.3's lookup performed FOR AN ELEMENT — its own custom element registry, its namespace, its local name
   and its is value, which are the four arguments the algorithm takes. This is the form every caller that HAS a
   node should use: it is the only one that can answer out of a SCOPED registry. OWNED; JS_UNDEFINED when there
   is no definition (including when the element's registry is null, which is the algorithm's step 1). */
JSValue custom_elements_definition_lookup_for_element(JSContext *ctx, JSValueConst el_wrap);

/* THE REGISTRY QUESTIONS §4.8's attachShadow AND §4.9's create-an-element ASK BEFORE THEY CAN ACT — this
   document's registry, whether a page-supplied value is a CustomElementRegistry at all, whether it is scoped,
   and the association itself. Neither algorithm lives here, and neither may re-derive them: the record, the
   `is scoped` flag, the once-only association rule and the scoped-registry latch are all this component's.
   `custom_elements_document_registry` is OWNED; the association takes the node's WRAPPER, because the slot
   is per-flow state on it. */
bool    custom_elements_is_registry(JSValueConst v);
bool    custom_elements_registry_is_scoped(JSContext *ctx, JSValueConst reg);
void    custom_elements_node_associate_registry(JSContext *ctx, JSValueConst wrap, JSValueConst reg);
JSValue custom_elements_document_registry(JSContext *ctx);
/* A NODE'S own registry, derived where it holds none — for an algorithm that PASSES one on rather than looking
   a definition up with it (DOM §4.4 clone step 6.2 hands the original shadow root's to the copy's). OWNED. */
JSValue custom_elements_node_registry(JSContext *ctx, JSValueConst wrap);
/* The definition's constructor — DOM §4.9 step 5.1.1's `C`, the value `create an element` Constructs. OWNED. */
JSValue custom_elements_definition_constructor(JSContext *ctx, JSValueConst def);
/* DOM §4.9 steps 5.1.4.2-11: the checks the spec runs on what the page's constructor RETURNED, and the state
   it then writes onto it. They are here rather than in document.c because "result's custom element state" and
   "result's custom element definition" are this component's own record, and a second writer of them is a
   second answer to what a custom element is. Returns 0, or -1 having thrown the NotSupportedError/TypeError
   the step names. `local` is the local name `create an element` was given. */
int custom_elements_created_check(JSContext *ctx, JSValueConst result,
                                  lxb_dom_document_t *doc, const char *local, size_t len);
/* DOM §4.9 step 5.1.4's failure arm: the element it answers with has custom element state "failed". Written
   from here and not by the caller because the state is this component's own record — and it is what stops the
   element being tried for upgrade again the moment it enters a document. */
void custom_elements_mark_failed(JSContext *ctx, JSValueConst wrap);
/* `window.customElements` — this realm's Document's CustomElementRegistry, and the `CustomElementRegistry`
   interface object that makes `new CustomElementRegistry()` (a SCOPED one) constructible. */
void custom_elements_install(JSContext *ctx, JSValueConst global);
/* §4.13.4's interface PROTOTYPE for ONE realm — declared into core/realm.h's list, because the members on it
   answer out of the realm that defined them. */
void custom_elements_install_proto(JSContext *ctx);

/* §4.13.4 step 15'S THREE BOOLEAN FIELDS OF A DEFINITION, named so a reader outside this component can ask for
   one without knowing how a definition is stored. `disable shadow` has no reader yet — §4.13.5 step 8.1 and
   `attachShadow` are the two, and neither exists — but it is COLLECTED, because step 14.10 reads the same
   `disabledFeatures` sequence step 14.9 does and collecting one of the two would make the definition disagree
   with the sequence it was built from. */
typedef enum {
    CE_DEF_FORM_ASSOCIATED = 0,
    CE_DEF_DISABLE_INTERNALS,
    CE_DEF_DISABLE_SHADOW
} CustomElementDefinitionFlag;
bool custom_elements_definition_flag(JSContext *ctx, JSValueConst def, CustomElementDefinitionFlag which);

/* THE ELEMENT'S OWN DEFINITION — §4.13.5 step 2's record, or JS_UNDEFINED. OWNED. `attachInternals` needs it
   for step 2, and every form-associated member needs it to answer "is this a form-associated custom element".
   It is the element's own state and not a registry lookup, which is what makes an element upgraded by a
   definition keep answering for THAT definition after the registry moves on. */
JSValue custom_elements_definition_of_element(JSContext *ctx, JSValueConst wrap);
/* DOM §4.9's custom element state for an element — one of the five CE_STATE_* values. §4.13.7's
   `attachInternals` step 6 branches on exactly it. */
int custom_elements_state_of_element(JSContext *ctx, JSValueConst wrap);
/* HTML §4.13.1's "valid custom element name" — the PotentialCustomElementName production plus the reserved
   list, over UTF-8 bytes. Public because DOM §4.8's "valid shadow host name" is stated as "a valid custom
   element name, or one of eighteen built-ins": a second copy of the production beside it would be a second
   answer to which names may host a shadow tree.*/
bool custom_elements_name_is_valid(const char *name, size_t len);

/* §4.13's "element is a form-associated custom element": it carries a definition whose form-associated field
   is true. The predicate every ElementInternals form member throws a NotSupportedError on. */
bool custom_elements_is_form_associated(JSContext *ctx, JSValueConst wrap);

/* §4.13.6 "enqueue a custom element callback reaction" for one of §4.13.4 step 14.13's FORM callbacks, from
   the component that owns the state which changed. `which` is one of the CE_FORM_CB_* ids below; `args` is
   the argument list the spec names for it. A no-op when the element carries no definition or the definition
   collected no such callback, which is step 3 of the enqueue. */
enum { CE_FORM_CB_ASSOCIATED = 0, CE_FORM_CB_RESET, CE_FORM_CB_DISABLED, CE_FORM_CB_STATE_RESTORE };
void custom_elements_enqueue_form_callback(JSContext *ctx, JSValueConst wrap, int which,
                                           int argc, JSValueConst *args);

/* DOM §4.2.3's INSERTION STEPS for an element, as HTML's custom-element half states them: an element that is
   already "custom" gets a connectedCallback reaction, and any other element is TRIED FOR UPGRADE (§4.13.5's
   "try to upgrade", which ENQUEUES an upgrade reaction — it never constructs here, because the insertion is
   inside a C walk that cannot park). The two are one branch and not two calls because the spec writes them as
   one, and because doing both for an element that is already custom would run its constructor twice. */
void custom_elements_element_connected(JSContext *ctx, lxb_dom_element_t *el);
/* §4.13.3's disconnected reaction — the twin of the upgrade, for an element LEAVING a document. A no-op unless
   the element was upgraded, because only an upgraded element has a lifecycle to react with. */
void custom_elements_disconnected(JSContext *ctx, lxb_dom_element_t *el);
/* §4.13.3's attribute-changed reaction. Called BEFORE the write, so the element still holds the old value;
   `val` is NULL for a removal. A no-op unless the element is upgraded and its definition OBSERVES this name.
   BOTH VALUES ARE PASSED, because §9.4.6 step 3 runs the change steps AFTER step 2 stored the new one — the
   element no longer holds the old value by the time this is called, and reading it back off the element was how
   the callback reported the value it was replacing as both arguments.
   THE ATTRIBUTE IS NAMED THE WAY §4.9 NAMES IT — (namespace, LOCAL name), `ns` NULL for the null namespace —
   because §4.9's "handle attribute changes" step 2 enqueues the reaction with « local name, oldValue, newValue,
   NAMESPACE » and the observed-attributes filter is over the LOCAL name: a qualified name would neither match
   `observedAttributes` for a prefixed attribute nor be able to supply the fourth argument at all. */
void custom_elements_attribute_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                       const char *old, size_t old_len, const char *val, size_t val_len);

/* DOM §4.5 "ADOPT A NODE" STEP 3'S REGISTRY ARMS, for ONE shadow-including inclusive descendant the walk has
   just moved from `old_document` into `document` — step 3.2 (a shadow root takes the new document's global
   registry unless its own is scoped), step 3.3.2 (an element re-derives its registry from its PARENT, or from
   the new document when it has none or is a child of an exclusive DocumentFragment) and step 3.3.3 (a CUSTOM
   element gets an `adoptedCallback` reaction with « oldDocument, document »).
   THE WHOLE ARM IS ONE ENTRY because every part of it is this component's record — the registry object, its
   `is scoped` boolean, DOM §4.5's "effective global custom element registry", the node's registry slot, the
   element's custom element state and its definition. node.c owns the WALK and step 3.1's node documents; it
   must not be able to name any of the above, or there are two answers to what a node's registry is.
   Called ONLY from inside that walk, which is why it asserts `document != old_document` rather than testing
   it: step 3's condition is the walk's, and an arm reached without it rewrites a registry adoption never
   touches. */
void custom_elements_node_adopted(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_document_t *document,
                                  lxb_dom_document_t *old_document);

#endif
