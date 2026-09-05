/* CUSTOM ELEMENTS — HTML §4.13: the registry, the upgrade, and the lifecycle reactions. */
#ifndef ENGINE_HOST_BROWSER_CORE_HTML_CUSTOM_ELEMENTS_H
#define ENGINE_HOST_BROWSER_CORE_HTML_CUSTOM_ELEMENTS_H
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include <stdint.h>   /* custom_elements_reactions_enqueued's monotone count */
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/events/report_exception.h"

/* attributeChangedCallback's (localName, oldValue, newValue, NAMESPACE) — the widest lifecycle callback there
   is. FOUR, because DOM §4.9's "handle attribute changes" step 2 enqueues « attribute's local name, oldValue,
   newValue, attribute's namespace »: the namespace is the argument a page reads to tell an `xlink:href` from
   the null-namespace `href` it also observes, and a three-slot buffer silently drops it. */
#define CE_MAX_REACTION_ARGS 4

/* DOM §4.9's CUSTOM ELEMENT STATE — the five values, and they are five because §4.13.5 "Upgrades" step 1
   branches on exactly this set. It was a BOOLEAN ("does this wrapper carry a definition?"), which cannot tell
   an element whose upgrade THREW from one that was never tried: §4.13.5 returns early for "undefined" and
   "uncustomized" and for nothing else, so a failed element was upgraded again on every re-insertion and its
   constructor ran a second time. "precustomized" is the window between step 10.2 and the constructor
   returning, which is what DOM §4.9 step 5.1.4's assert distinguishes an element that reached `super()` by. */
typedef enum {
    /* a local name §4.13.3 "Core concepts" could never accept — the default for most elements */
    CE_STATE_UNCUSTOMIZED = 0,
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
/* THE THIRD TYPE IS §4.13.6 ENQUEUE STEP 3'S SYNTHESIZED CALLBACK, and it is a type rather than two queued
   reactions because step 3.4 makes ONE callback out of two calls. Web IDL § 3.12 Invoking callback functions
   supplies "rethrow" wherever a specification states none — verbatim, "specifications which fail to provide a
   value here when it would be mandatory should be understood as supplying \"rethrow\"" — so step 3.4.1's bare
   "call disconnectedCallback with no arguments" ABANDONS step 3.4.2 when it throws, and the outer invoke's
   "report" reports it ONCE. Two queued callback reactions would run the second anyway and report twice. */
enum { CE_REACTION_CALLBACK = 0, CE_REACTION_UPGRADE = 1, CE_REACTION_MOVE_PAIR = 2 };

/* HTML §4.13.5 "Upgrades"' UPGRADE AN ELEMENT, AS A CURSOR ITS CALLERS EMBED — the same shape as the drain
   below and for the same reason: its step 10.3 CONSTRUCTS the page's class, so the algorithm PARKS, and a
   parked algorithm's state has to ride the calling machine's `visit` for a fork and its teardown for the
   throw path.
   TWO CALLERS AND ONE IMPLEMENTATION, WHICH IS WHAT MAKES THIS A STRUCT RATHER THAN A SECOND DRIVER. §4.13.6
   step 1.3.1's upgrade arm runs it from the reaction drain, and DOM §4.9 "Interface Element"'s create an
   element step 4.3 runs it with no drain in sight — verbatim "If synchronousCustomElements is true, then run
   this step while catching any exceptions", whose catching list is "Upgrade result using definition". That is
   `document.createElement("button", {is:"my-btn"})`, where there is no reaction, no element queue and nothing
   to dequeue from; welding the cursor to the drain's record is what would have forced a second copy of
   §4.13.5 for the caller that has no drain, which is the dual-system rot this file's own architecture rules
   name. The cursor moved OUT of CustomElementQueue rather than being duplicated into DocCreateElState.
   THE ELEMENT AND THE DEFINITION ARE HELD HERE and not re-read from the caller at each re-entry, because step
   10's regardless-list is owed by a flow that is TORN DOWN mid-Construct and therefore never re-enters at
   all: custom_elements_upgrade_unlock is that exit, and it can name only what this record itself holds. */
typedef struct {
    /* 0 exactly when no upgrade is in flight, which is what every caller's teardown reads to decide whether
       step 10's regardless-list is still owed. */
    uint8_t stage;
    uint8_t phase;      /* step 10.3's Construct request phase */
    JSValue cb[1];      /* [C] — step 10.3 constructs "with no arguments", so one slot is the whole buffer */
    JSValue el;         /* the element being upgraded (owned) */
    JSValue def;        /* the definition it is being upgraded with (owned) */
} CeUpgrade;

void custom_elements_upgrade_init(CeUpgrade *u);
void custom_elements_upgrade_visit(JSContext *ctx, CeUpgrade *u, JSStepVisit *v);
/* HTML §4.13.5 step 10's REGARDLESS-LIST, for the flow that never comes back to run it — "regardless of
   whether the above steps threw an exception or not" is a claim about every exit from step 10, and a flow torn
   down while parked on step 10.3's Construct is one of them. What step 10 took by then outlives the flow: the
   active custom element constructor map entry steps 8-9 pushed, and step 6's construction stack entry. A
   no-op when no upgrade is in flight, so every caller calls it unconditionally from its own release. */
void custom_elements_upgrade_unlock(JSContext *ctx, CeUpgrade *u);
/* HTML §4.13.5 "Upgrades", ONE STEP AT A TIME. `el` and `def` are the algorithm's two inputs and are ADOPTED
   at the first entry; a re-entry passes the same pair and is asserted against what was adopted, because a
   resume that names a different upgrade is the one way this cursor can be driven wrong.
   Returns JS_STEP_CONSTRUCT parked on the page's constructor (the caller returns it), or 0 when the upgrade
   has finished. On 0, `*pexc` is step 10's finally-list step 3's RETHROW — the exception, OWNED, which the
   caller reports (§4.13.6 step 1.3.1 for the drain, DOM §4.9 step 4.3's threw-list for createElement) — or
   JS_UNDEFINED when the upgrade set the element's state to "custom". `cb_result` is CONSUMED. */
int  custom_elements_upgrade_run(JSContext *ctx, CeUpgrade *u, JSValueConst el, JSValueConst def,
                                 JSValue cb_result, JSValue **out_cb, int *out_argc, JSValue *pexc);

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
       (step 10.3's Construct), so it needs a resume point of its own inside the one reaction the drain is on —
       and it is the SAME record DOM §4.9's create an element embeds, because there is one §4.13.5. */
    CeUpgrade up;
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
    /* WHICH OF §4.13.6 ENQUEUE STEP 3.4'S TWO CALLS A CE_REACTION_MOVE_PAIR REACTION IS ON — 0 before 3.4.1,
       1 before 3.4.2, 2 when the synthesized callback has returned. `phase` cannot answer this: step_call_run
       RESETS it to 0 when it hands back a result, so being about to issue the first call and being about to
       issue the second are the same phase byte, and the resume would re-run disconnectedCallback for ever.
       It is a CURSOR and not a second phase for that reason — the two calls share one request buffer and one
       phase, in sequence, which is what makes them one reaction with two rest points. */
    uint8_t  syn;
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

/* HTML §3.2.3 "HTML element constructors"'s `[HTMLConstructor]` AS THIS REALM'S HTMLElement INTERFACE
   OBJECT. It is minted here and not
   where the other interface objects are because the algorithm is this component's — it walks the definition
   set and the construction stack — while WHICH interface carries it is html_element.c's. OWNED (consumed by
   the install). */
JSValue custom_elements_html_constructor(JSContext *ctx);

/* THE SAME ALGORITHM AS THIS REALM'S INTERFACE OBJECT FOR EVERY OTHER HTML ELEMENT INTERFACE. §3.2.3 says
   interfaces annotated `[HTMLConstructor]` "have the following overridden constructor steps" — one list of
   steps, shared, and the only step whose ANSWER depends on which interface is running is 8.1's "the list of
   local names for elements … that use the active function object as their element interface", which the
   machine asks HTML §3.2.2 rather than carrying a table of. So this takes an interface NAME only to name the
   function object it mints; nothing downstream reads it.
   IT IS WHAT MAKES A CUSTOMIZED BUILT-IN CONSTRUCTIBLE, and nothing else needs it: `new HTMLButtonElement()` is
   step 1's TypeError with or without this, and an autonomous class extending the wrong interface is step 7.1's.
   OWNED (consumed by the install). */
JSValue custom_elements_element_constructor(JSContext *ctx, const char *iface);

/* §4.13.3's lookup performed FOR AN ELEMENT — its own custom element registry, its namespace, its local name
   and its is value, which are the four arguments the algorithm takes. This is the form every caller that HAS a
   node should use: it is the only one that can answer out of a SCOPED registry. OWNED; JS_UNDEFINED when there
   is no definition (including when the element's registry is null, which is the algorithm's step 1). */
JSValue custom_elements_definition_lookup_for_element(JSContext *ctx, JSValueConst el_wrap);

/* THE REGISTRY QUESTIONS DOM §4.9's attachShadow AND create-an-element ASK BEFORE THEY CAN ACT — this
   document's registry, whether a page-supplied value is a CustomElementRegistry at all, whether it is scoped,
   and the association itself. Neither algorithm lives here, and neither may re-derive them: the record, the
   `is scoped` flag, the once-only association rule and the scoped-registry latch are all this component's.
   `custom_elements_document_registry` is OWNED; the association takes the node's WRAPPER, because the slot
   is per-flow state on it. */
bool    custom_elements_is_registry(JSValueConst v);
bool    custom_elements_registry_is_scoped(JSContext *ctx, JSValueConst reg);
/* DOM §4.5 "Interface Document": "Null or a CustomElementRegistry object registry is a GLOBAL CUSTOM ELEMENT
   REGISTRY if registry is non-null and registry's is scoped is false." It takes a value that may be JS_NULL,
   which is the whole difference between it and the predicate above — every caller is a spec step stated over
   "null or a CustomElementRegistry object", so a null-refusing form would push the null arm out to each of
   them and they would not agree for long. HTML §13.3 "Serializing HTML fragments"' shouldAppendRegistryAttribute
   asks it twice in one step and is why it is exported. */
bool    custom_elements_registry_is_global(JSContext *ctx, JSValueConst reg);
void    custom_elements_node_associate_registry(JSContext *ctx, JSValueConst wrap, JSValueConst reg);
JSValue custom_elements_document_registry(JSContext *ctx);
/* A NODE'S own registry, derived where it holds none — for an algorithm that PASSES one on rather than looking
   a definition up with it (DOM §4.4 clone step 6.2 hands the original shadow root's to the copy's). OWNED. */
JSValue custom_elements_node_registry(JSContext *ctx, JSValueConst wrap);
/* The definition's constructor — DOM §4.9 step 5.1.1's `C`, the value `create an element` Constructs. OWNED. */
JSValue custom_elements_definition_constructor(JSContext *ctx, JSValueConst def);
/* DOM §4.9 "Interface Element"'s create an element STEP 4'S OWN CONDITION, whose second half is verbatim
   "definition's name is not equal to its local name (i.e., definition represents a customized built-in
   element)" — the question that decides whether a definition the lookup already found is UPGRADED onto an
   existing built-in (step 4) or CONSTRUCTED into a new autonomous element (step 5).
   IT ASKS THE DEFINITION AND NOT THE CALLER, and that is the difference between this and re-deriving the
   answer from the local name the operation was given. §4.13.3's lookup has already decided WHICH definition
   this element gets, by either of its two arms; step 4 asks only what KIND of definition that is, and a caller
   comparing its own `localName` against the definition's name would be asking a third question that agrees
   with this one only for the arm it happens to be thinking of. */
bool custom_elements_definition_is_customized_builtin(JSContext *ctx, JSValueConst def);
/* DOM §4.9 "Interface Element"'s IS VALUE, AS THE QUESTION THE STEPS THAT REFUSE A CUSTOMIZED BUILT-IN ASK.
   HTML §4.13.7 "Element internals"' attachInternals step 1 is verbatim "If this's is value is not null, then
   throw a "NotSupportedError" DOMException", and it is a test on the SLOT rather than on the `is` CONTENT
   ATTRIBUTE — DOM §4.9 fixes the is value at creation and never lets it move, so a `setAttribute("is", …)` on
   an ordinary element must not start making this throw and a `removeAttribute("is")` on a customized built-in
   must not stop it. The slot is this component's record, which is why the predicate is here and why no caller
   may re-derive the answer from the attribute list.
   IT IS A TEST AND NOT A LOST FORK, WHICH IS THE QUESTION TO ASK OF EVERY READER OF THIS SLOT. An is value
   whose bytes are unknown external input is NON-NULL on every arm of whatever it turns out to be, so this
   step's answer does not depend on the bytes and forking here would mint two siblings that throw the same
   DOMException. The readers that DO depend on the bytes are the two that compare the is value against a
   definition's NAME — §4.13.3 "Core concepts"' look up a custom element definition step 4 and §4.13.4 "The
   CustomElementRegistry interface"' upgrade particular elements within a document — and those are forks. */
bool custom_elements_element_has_is_value(JSContext *ctx, JSValueConst wrap);

/* §4.13.4'S ACTIVE CUSTOM ELEMENT CONSTRUCTOR MAP, AS THE ONE PAIR THAT BRACKETS A CONSTRUCT. "Each
   similar-origin window agent has an associated active custom element constructor map, which is a map of
   constructors to CustomElementRegistry objects" — and every algorithm that Constructs a definition's `C` has
   to put `registry` in it first and take it back out after, because HTML §3.2.3 "HTML element constructors"
   step 3 reads it and is the ONLY way a class defined in a SCOPED registry can resolve its own definition from
   inside its own `super()`. Without the entry §3.2.3 step 4 derives the current global's document's registry
   instead, so the definition is real and simply not in the set that was asked, and the constructor throws a
   TypeError at the page.
   TWO CALLS AND NOT A SLOT, because what the standard writes is a SAVE and a RESTORE — §4.13.5 "Upgrades"
   steps 8-9 and its step 10 regardless-list steps 1-2, and DOM §4.9 "Interface Element" create an element
   steps 5.1.2-5.1.3 and 5.1.5-5.1.6 — and `previousRegistry` is what makes them a pair rather than two writes.
   The leave takes `ctor` back so the assert it carries is an IDENTITY question: a pair that ran out of order
   would otherwise restore some other algorithm's previousRegistry with nothing to say so.
   THE LEAVE OWES EVERY EXIT, TEARDOWN INCLUDED. "Regardless of whether the above steps threw" covers the flow
   that is DISCARDED while parked on the page's constructor, which no resume ever comes back from. The map is
   agent-wide, so an entry left on it answers every later `new C()` in the agent, in flows that never named
   this registry — and nothing reports it, because the entry is a live value on a live object.
   IT IS NOT AN IdlStepDecl `release`, AND THE REASON IS THAT AN ENTRY IS NOT A FLAG. That field's stated
   purpose is what no declaration can name — a lexbor handle, a foreign allocation, a flag to lower — while this
   map holds CONSTRUCTORS as keys, so the leave mutates the agent's own object graph and drops a reference to
   `C`. A step machine that enters therefore declares the leave with core/idl_args.h's idl_active_ctor_owed, and
   the machine performs it at its teardown, below idl_args.c's `release` bracket. §4.13.5 "Upgrades"' half is
   given back the same way and at the same point, through custom_elements_queue_unlock, and THAT is what makes
   the placement load-bearing rather than tidy: the two are a nested pair and must unwind in nesting order, and
   a member's `release` runs before that unlock. (It is no longer the fingerprint that forces it. idl_args.c
   once folded the heap's reference count of every declared value, which refused this give-back — `C` is a
   declared value — and aborted every completed `document.createElement` of a defined name; that fold reads slot
   IDENTITY now and would not object.) */
void custom_elements_active_ctor_enter(JSContext *ctx, JSValueConst ctor, JSValueConst registry);
void custom_elements_active_ctor_leave(JSContext *ctx, JSValueConst ctor);
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
/* DOM §4.9 "Interface Element"'s IS VALUE, WRITTEN AT THE ONE MOMENT THE STANDARD WRITES IT — "create an
   element internal"'s step 2, which sets the is value, TOGETHER WITH "create an element" step 6.3, which is
   the state that follows from it. They are ONE entry and not two because a caller that could perform one
   without the other would be a second answer to what an is value means: an element carrying one and still
   deriving "uncustomized" reports as `:defined` while being a custom element that has not been upgraded, and
   an element marked "undefined" with no is value is a `<button>` no lookup can ever resolve. Both producers
   of an is value go through here — HTML §13.2.6.1 "Creating and inserting nodes"' create an element for the
   token step 5, and DOM §4.5 "Interface Document"'s ElementCreationOptions `is` — so there is one write site
   and the invariant that it happens exactly once per element is assertable at it.
   `is` NULL IS DOM'S NULL IS VALUE and writes nothing; a NON-NULL `is` of length 0 is `is=""`, which DOM §4.9
   step 6.3 counts as non-null. IT TAKES THE ELEMENT AND NO REALM, for custom_elements_is_defined's reason —
   the caller is a parse edge standing on a Lexbor node, and the realm this state belongs to is the ELEMENT'S
   OWN DOCUMENT'S, never whichever one happens to be running.
   AN IS VALUE THE CALLER CANNOT SPELL ARRIVES AS `unknown`, AND THAT IS A THIRD STATE RATHER THAN A SECOND
   SPELLING OF THE NULL ONE. DOM §4.9's is value is "null or a string"; this engine's third answer is a string
   whose BYTES are unknown external input, which is non-null on every arm of whatever it turns out to be —
   `createElement("button", {is: location.hash.slice(1)})` made a customized built-in in a browser for every
   value the fragment can hold. Collapsing it to the null is value is the defaulted-field defect at its
   sharpest: the slot then reads ABSENT to every reader, the element derives "uncustomized", and unknown
   external input has been turned into the positive statement that the page made no customized built-in at all.
   EXACTLY ONE OF THE TWO SPELLINGS IS SUPPLIED and the entry asserts it: `is` non-NULL is the known value,
   `unknown` non-UNDEFINED is the unknown one, and NEITHER is DOM's null. They are two parameters and not two
   entries because there is still ONE write and one moment — the argument above for a single writer is about
   what an is value MEANS, and it does not weaken because the bytes arrive by two roads. The byte road stays
   bytes so the parse edge keeps needing no realm: it has no context to mint a string in, and handing it one
   would duplicate at the caller the realm resolution this entry exists to own. */
void custom_elements_created_with_is_value(lxb_dom_element_t *el, const char *is, size_t len,
                                           JSValueConst unknown);
/* `window.customElements` — this realm's Document's CustomElementRegistry, and the `CustomElementRegistry`
   interface object that makes `new CustomElementRegistry()` (a SCOPED one) constructible. */
void custom_elements_install(JSContext *ctx, JSValueConst global);
/* §4.13.4's interface PROTOTYPE for ONE realm — declared into core/realm.h's list, because the members on it
   answer out of the realm that defined them. */
void custom_elements_install_proto(JSContext *ctx);

/* DOM'S `readonly attribute CustomElementRegistry? customElementRegistry;`, ON THE TWO SURFACES THAT DECLARE
   IT — §4.9 "Interface Element" (its own member) and §4.2.5 "Mixin DocumentOrShadowRoot" (which `Document
   includes` and `ShadowRoot includes`, so the second entry is called once per including interface). The member
   is INSTALLED FROM HERE and merely HOSTED there, because the node's registry, its derivation and the
   once-only association rule are this component's record — the same reason attachShadow and create-an-element
   ask the questions above rather than deriving them. The two are separate entry points and not one because
   they are separate IDL declarations with separate brand checks: an Element's member must throw for a
   Document receiver, and a member that admitted every node kind would be one interface answering for another. */
void custom_elements_install_element_member(JSContext *ctx, JSValueConst element_proto);
void custom_elements_install_document_or_shadow_root_member(JSContext *ctx, JSValueConst target);

/* §4.13.4 step 15'S THREE BOOLEAN FIELDS OF A DEFINITION, named so a reader outside this component can ask for
   one without knowing how a definition is stored. `disable shadow` has TWO readers and they refuse at
   different moments, which is the whole reason the field outlives `attachShadow`'s own check: DOM's "attach a
   shadow root" refuses only when the element's `is` value is non-null, and §4.13.5 "Upgrades" step 10.1
   refuses an element that ALREADY carries a root when the definition arrives — the case a
   `<template shadowrootmode>` parsed before `define()` creates. `disable internals` is collected beside it
   because step 14.10 reads the same `disabledFeatures` sequence step 14.9 does, and collecting one of the two
   would make the definition disagree with the sequence it was built from. */
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
/* DOM §4.9's "defined" — the state is "uncustomized" or "custom" — which is what HTML §4.16.3
   "Pseudo-classes" defines `:defined` as matching. It takes the NODE and no realm because the selector
   matcher has neither: `lxb_selectors_host_cb_t` (lexbor/selectors/selectors.h) is asked while walking a
   tree, so the element's own document's realm is derived here rather than carried in from whoever ran the
   query. Never mints a wrapper. */
bool custom_elements_is_defined(const lxb_dom_node_t *n);
/* HTML §4.13.3 "Core concepts"'s "valid custom element name" — five requirements over UTF-8 bytes, the first
   of which is the DOM's own "valid element local name" (core/dom/names.h). It is NO LONGER a grammar: the
   `PotentialCustomElementName` production this comment used to name has been REMOVED from the standard, which
   is why the name is now stated as a conjunction of five bullets and why a much wider set of names is legal
   than a PCENChar list admits ("a large variety of names is allowed, to give maximum flexibility for use cases
   like <math-α> or <emotion-😍>", §4.13.3). A citation to a production the spec no longer contains reads as
   authoritative and sends the next reader to look for text that is not there.
   Public because DOM §4.9's "valid shadow host name" is stated as a valid custom element name or one of
   eighteen named built-ins: a second copy of the requirements beside it would be a second answer to which
   names may host a shadow tree. */
/* WHICH OF THE FIVE FAILED, because §4.13.4's step 2 answers all five with ONE "SyntaxError" and the page's
   `catch` therefore cannot tell a missing hyphen from a reserved MathML name from an uppercase letter. That is
   not a cosmetic difference: a check that throws the right exception for the WRONG REASON passes any test that
   only checks the throw, so four of these five clauses could be deleted and every assertion over the throw
   would stay green. The verdict is the ONE implementation and `custom_elements_name_is_valid` is `verdict ==
   CE_NAME_OK` — never a second walk beside it, which is how the registry's own four-line copy drifted from the
   DOM's predicate in the first place. The ORDER is §4.13.3's own bullet order, so the verdict names the FIRST
   requirement the standard lists that this name fails. */
typedef enum {
    CE_NAME_OK = 0,
    CE_NAME_NOT_A_LOCAL_NAME,      /* bullet 1 — DOM §1.4 "valid element local name" */
    CE_NAME_NOT_LOWER_ALPHA_FIRST, /* bullet 2 — the 0th code point is an ASCII lower alpha */
    CE_NAME_HAS_UPPER,             /* bullet 3 — it contains no ASCII upper alphas */
    CE_NAME_NO_HYPHEN,             /* bullet 4 — it contains a U+002D (-) */
    CE_NAME_RESERVED,              /* bullet 5 — it is one of the eight SVG/MathML hyphenated names */
    CE_NAME_VERDICT_COUNT
} CeNameVerdict;
CeNameVerdict custom_elements_name_verdict(const char *name, size_t len);
/* The sentence §4.13.4 step 2's SyntaxError carries for that verdict. It starts with "not a valid custom
   element name" for every one of them — that half is the SPEC's answer and is what a reader greps for — and
   names the failed bullet after a colon. DFAILs on CE_NAME_OK and on a verdict with no sentence, because a
   clause added to the enum without a message is a fifth reason rendered as a fourth. */
const char *custom_elements_name_why(CeNameVerdict v);
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

/* DOM §4.2.3 `insert` STEP 7.7.3, which is NOT an insertion step — it is the sibling of one. Step 7.7.1 is
   "Run the insertion steps with inclusiveDescendant"; this is 7.7.3, guarded by 7.7.2's "If inclusiveDescendant
   is not connected, then continue": an element that is already "custom" gets a connectedCallback reaction, and
   any other element is TRIED FOR UPGRADE (§4.13.5's "try to upgrade", which ENQUEUES an upgrade reaction — it
   never constructs here). The two are one branch and not two calls because the spec writes them as one, and
   because doing both for an element that is already custom would run its constructor twice.
   THE DISTINCTION IS LOAD-BEARING AND NOT PEDANTRY: this engine DEFERS the insertion and removing steps,
   because they park and fork, and it must NOT defer this — §4.5 adopt step 3.3.3's `adoptedCallback` is
   enqueued synchronously (adopt on a parentless node performs no tree change at all, so it has no deferred half
   to live in), and deferring only one of the pair reordered every cross-document move. Called from the DOM
   mutation chokepoint; see core/dom/node.c's node_custom_element_reactions_tree_steps. */
void custom_elements_element_connected(JSContext *ctx, lxb_dom_element_t *el);
/* DOM §4.2.3 `remove` STEPS 13 and 14.2 — §4.13.3's disconnected reaction, the twin of the above, for an
   element LEAVING a document. `remove` separates it from the removing steps by NUMBER rather than by loop
   position: step 11 runs the removing steps, step 12 reads isParentConnected, step 13 enqueues. A no-op unless
   the element was upgraded, because only an upgraded element has a lifecycle to react with. Step 12's
   condition belongs to the CALLER — it is one fact for the whole removed subtree, read off the parent because
   the node itself is detached by then. */
void custom_elements_disconnected(JSContext *ctx, lxb_dom_element_t *el);
/* HOW MANY REACTIONS §4.13.6's "enqueue an element on the appropriate element queue" has run for, ever.
   Monotone and agent-wide. It exists so a span of steps can assert it enqueued NOTHING — the ordering
   invariant that lets the insertion/removing steps be deferred while the enqueues above are not. A caller may
   only bracket a span NO OTHER FLOW CAN RUN INSIDE: one counter cannot tell two forked arms apart. */
uint64_t custom_elements_reactions_enqueued(void);
/* DOM §4.2.3 MOVE STEP 24.3 — "if inclusiveDescendant is custom and newParent is connected, then enqueue a
   custom element callback reaction with inclusiveDescendant, callback name "connectedMoveCallback", and « »".
   THE THIRD SIDE OF A TREE CHANGE, and it is neither of the other two: a move runs no insertion steps and no
   removing steps, so an element that is moved must not be told it disconnected and reconnected — HTML
   §4.13.2.1 "Preserving custom element state when moved" is the section that says so, and its whole example is
   an element whose observer and tab index survive because the pair did not fire.
   THE CALLER OWNS "newParent is connected" — one fact for the whole moved subtree, decided once by the move.
   A no-op for an element that is not custom, and for a definition with none of the three callbacks. */
void custom_elements_moved(JSContext *ctx, lxb_dom_element_t *el);
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
