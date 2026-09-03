/* The `script` element's parse state, HTML §4.12.1's preparation and its `async` member — see html_script.h for
   why two booleans nobody else can store are a component. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "solver/dom_cow.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "core/dom/node.h"
#include "core/dom/text_content.h"   /* §4.12.1.1's "el's child text content" is DOM §4.11's, over DOM's own Text test */
#include "core/dom/element.h"    /* §4.12.1.1's children changed steps are RECORDED for the ONE drain that can run them */
#include "core/dom/document.h"   /* which DOCUMENT this program belongs to: the realm it is compiled in */
#include "core/dom/document_current_script.h"   /* "execute the script element" step 6's classic arm, steps 1-2 and 4 */
#include "core/events/report_exception.h"       /* §8.1.4.4 "Calling scripts" step 8's third bullet */
#include "core/events/event.h"           /* §4.12.1.1's error arm dispatches an Event, so it MINTS one */
#include "core/events/event_target.h"    /* …through DOM §2.9's one dispatch, which runs the PAGE's listeners */
#include "core/idl_args.h"       /* the `async` attribute's setter, declared like every other IDL member's */
#include "core/url/url.h"        /* §4.12.1's "encoding-parsing a URL given src, relative to el's node document" */
#include "core/loader/document_scripts.h"   /* §4.12.1's type-string steps, asked ONCE for both halves */
#include "core/html/html_script.h"

/* §4.12.1's `already started`, on the element's wrapper under a Symbol this file minted and never published —
   the store DOM §4.9's custom element state uses, for the two reasons html_script.h gives. */
static JSValue g_started_key = JS_UNDEFINED;
static JSAtom  g_atom_started = JS_ATOM_NULL;
/* …and §4.12.1's `force async`, in the same store under its own key. Two keys and not one record: each is a
   bare boolean the standard writes independently, and a record would be a third thing to keep consistent. */
static JSValue g_force_async_key = JS_UNDEFINED;
static JSAtom  g_atom_force_async = JS_ATOM_NULL;
static int     g_id_set_async = -1;   /* the `async` setter's pool id — declared per AGENT, installed per REALM */
/* §4.12.1.1's error arm, as a machine — registered per RUNTIME, like every other host step def. */
static int     g_err_stepid = -1;

/* CONFIGURABLE AND WRITABLE for the reason custom_elements.c's slots are: the flag is written more than once
   over one element's life — the parse marks it, and §4.12.1.1 "Processing model"'s cloning steps write the
   copy's from the original's — and a slot defined with no flags makes the second write a silent no-op. */
#define SCRIPT_SLOT_FLAGS (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)

/* §4.12.1's `async` SETTER STEPS, declared here because the declaration is the agent's and the init below is
   where an agent's one-per-runtime state is minted; the steps themselves are beside the getter. */
static JSValue js_script_set_async(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic);

static void script_children_changed(JSContext *ctx, lxb_dom_node_t *parent);

/* ---- HTML §4.12.1.1 "Processing model"'s ERROR ARM ---------------------------------------------------------
 *
 * THE STANDARD SPELLS IT THE SAME WAY THREE TIMES INSIDE ONE BRANCH — "queue an element task on the DOM
 * manipulation task source given el to fire an event named error at el, and return" — for a `src` the element
 * may not fetch from (`importmap`/`speculationrules`), a `src` that is the empty string, and a `url` that is
 * failure. It is ONE component and not an `if` at each of them for the reason every other repeated step in
 * this file is: three copies of a sentence the standard states once is three places for it to drift.
 *
 * IT WAS A COMMENT, WHICH IS THE ONE THING IT MAY NOT BE. Both of the arms this file already stood at returned
 * SILENTLY, with a paragraph above them recording that the event was "still owed" — so `<script src="">` and
 * `<script src="http://[">` left the algorithm the way a browser leaves it and fired NOTHING, and a page's
 * `s.onerror = () => loadFrom(FALLBACK_CDN)` never ran. That handler is not an edge case in this project's
 * terms: §What-the-tool-produces is about reaching what the bundle CAN do but didn't, and a script element's
 * error handler is exactly where a bundle keeps its fallback host and its degraded-mode configuration.
 *
 * IT IS A TASK, WHICH IS WHAT §4.12.1.1 SAYS IT IS. A synchronous fire inside `prepare` is two things wrong at
 * once: it is the wrong position in HTML §8.1.7's event loop (a page's `s.src = ""` would see its own
 * `onerror` run before the next statement), and it is unparkable — the listener list is the PAGE's, so the
 * dispatch runs the page's code and must have a flow base under it. core/html/html_image.c's §4.8.4.3.5 task
 * is the same sentence for `img` and this is built to match it.
 *
 * IT FIRES `error` AND NOTHING ELSE, AND THE NAME IS THEREFORE NOT A PARAMETER. §4.12.1.1's other event — "if
 * el's from an external file is true, then fire an event named load at el" — is a BARE SYNCHRONOUS STEP of
 * "execute the script element", so core/events/event_target.h assigns it to event_target_fire_run from the
 * machine that is executing, never to a queued task. A task here that could fire `load` would be a task able
 * to report that a script ran. */
#define SCRIPT_ERR_STAGES(X) \
    X(SERR_FIRE, "HTML §4.12.1.1 Processing model — the queued element task's fire of `error` at the element")
enum { SCRIPT_ERR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const script_err_steps[] = { SCRIPT_ERR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    JSStepHdr   hdr;       /* FIRST — the driver writes the def and the operand bounds through it */
    uint8_t     fphase;    /* the dispatch's own phase, held across a suspension */
    uint8_t     started;
    JSValue     ev;        /* the Event being dispatched (owned) */
    EventFireCb cb;        /* §2.9's dispatch request buffer, whose width travels with its type */
} ScriptErrTask;

static void script_err_visit(JSContext *ctx, void *stp, JSStepVisit *v)
{
    ScriptErrTask *s = stp;
    int k;

    /* NOTHING IS OWNED UNTIL THE FIRST STEP RUNS. The state's block is ZEROED (quickjs-step.h) and a zeroed
       JSValue is not JS_UNDEFINED, so a visit reaching a machine the driver has allocated and not yet entered
       would hand the collector a value nobody wrote. */
    if (!s->started) return;
    v->val(ctx, &s->ev);
    STEP_CB_FOREACH(s->cb, k) v->val(ctx, &s->cb[k]);
}

static int script_err_step(JSContext *ctx, void *stp, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    ScriptErrTask *s = stp;
    JSValueConst element = step_arg(&s->hdr, 0);
    int r;

    STEP_DISPATCH(SCRIPT_ERR_STAGES, s->hdr.stage, s->hdr.def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(SERR_FIRE);
        if (!s->started) {
            int k;

            /* EVERY OWNED FIELD IS PLACED BEFORE THE FIRST THING THAT CAN FAIL — `event_new` allocates, which
               is a moment the collector can walk this machine through script_err_visit. */
            s->started = 1;
            s->fphase = 0;
            s->ev = JS_UNDEFINED;
            STEP_CB_FOREACH(s->cb, k) s->cb[k] = JS_UNDEFINED;
            /* NEITHER FLAG IS SET: the standard names the fire as "fire an event named error at el" and gives
               no initialiser, so DOM §2.6 "Interface Event"'s defaults stand — it does not bubble and it is
               not cancelable. An `error` that bubbled would reach the Window's `onerror`, which is a
               DIFFERENT report (§8.1.4.4 "Calling scripts"' report an exception) about a different failure. */
            s->ev = event_new(ctx, "error", /*bubbles*/ false, /*cancelable*/ false);
            if (JS_IsException(s->ev)) { s->ev = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        }
        r = event_target_fire_run(ctx, &s->fphase, STEP_CB(s->cb), element, s->ev, JS_UNDEFINED, cb_result,
                                  NULL, out_cb, out_argc);
        if (r > 0) return r;   /* parked INSIDE the dispatch: a listener is the page's code */
        JS_FreeValue(ctx, s->ev);
        s->ev = JS_UNDEFINED;
        if (r < 0) return JS_STEP_ABRUPT;
        return JS_STEP_DONE;
}

static const JSTrampStepDef script_err_def = {
    sizeof(ScriptErrTask), script_err_step, NULL, 0,
    .visit = script_err_visit,
    .algorithm = "HTML §4.12.1.1 Processing model — a queued element task on the DOM manipulation task source",
    .steps = script_err_steps
};

void html_script_queue_error(JSContext *ctx, lxb_dom_element_t *el)
{
    JSValueConst argv[1];
    JSValue fn, elv;

    DCHECK(el != NULL, "§4.12.1.1's error arm was queued for no element — every one of its three sites reaches "
                       "it from inside `prepare the script element`, whose whole subject is EL, and the fire "
                       "is `at el`");
    DCHECK(g_err_stepid >= 0, "a §4.12.1.1 error task was queued before html_script_init registered its "
                              "machine — the id is per RUNTIME and the register is that function's last act");
    /* THE CALLEE IS MINTED IN THE ENQUEUING REALM. A C function runs in the realm that DEFINED it
       (js_call_c_function reads `p->u.cfunc.realm`), and this one fires an event at an element of THIS
       document — so a runtime-lifetime function object held in a static would answer every document's
       §4.12.1.1 out of whichever realm happened to reach the arm first. */
    fn = JS_NewCFunction2(ctx, NULL, "scriptElementTask", 1, JS_CFUNC_step, g_err_stepid);
    CHECK(!JS_IsException(fn), "§4.12.1.1: the queued error task's callee could not be allocated");
    elv = node_wrap(ctx, lxb_dom_interface_node(el));
    CHECK(!JS_IsException(elv), "§4.12.1.1: OOM wrapping the element its error event is fired at");
    argv[0] = elv;
    JS_EnqueueCallTask(ctx, fn, 1, argv);   /* §4.12.1.1: the DOM manipulation task source */
    JS_FreeValue(ctx, elv);
    JS_FreeValue(ctx, fn);
}

void html_script_init(JSContext *ctx)
{
    DCHECK(g_atom_started == JS_ATOM_NULL, "html_script_init ran twice in one runtime — the slot key is a "
                                           "Symbol, and a second one would leave every element already marked "
                                           "under the first key answering false under the second");
    g_started_key = JS_NewSymbol(ctx, "scriptAlreadyStarted", false);
    CHECK(!JS_IsException(g_started_key), "the script already-started slot key allocation failed");
    g_atom_started = JS_ValueToAtom(ctx, g_started_key);
    CHECK(g_atom_started != JS_ATOM_NULL, "the script already-started slot key could not be interned");
    g_force_async_key = JS_NewSymbol(ctx, "scriptForceAsync", false);
    CHECK(!JS_IsException(g_force_async_key), "the script force-async slot key allocation failed");
    g_atom_force_async = JS_ValueToAtom(ctx, g_force_async_key);
    CHECK(g_atom_force_async != JS_ATOM_NULL, "the script force-async slot key could not be interned");
    g_id_set_async = idl_setter_id(ctx, IDL_BOOLEAN, false, js_script_set_async, 0);
    /* HTML §4.12.1.1 "Processing model"'s children changed steps FOR `script` ELEMENTS — the family is DOM
       §4.2.3 "Mutation algorithms"'s and this standard states this element's — the third of the three doors
       into `prepare`. See
       script_children_changed for why only having the second one silently lost every text-injected chunk. */
    node_add_children_changed_hook(script_children_changed);
    /* §4.12.1.1's ERROR ARM, whose machine is per RUNTIME because a step def is. `ctx` is this agent's first
       realm and the runtime is the agent's, which is the same scope the atoms above are freed at. */
    DCHECK(g_err_stepid < 0, "§4.12.1.1's error machine is already registered for this agent — a second id "
                             "would leave the first one's def reachable by an element task nobody re-queued");
    g_err_stepid = JS_RegisterStepDef(JS_GetRuntime(ctx), &script_err_def);
}

void html_script_free(JSRuntime *rt)
{
    JS_FreeAtomRT(rt, g_atom_started);
    g_atom_started = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_started_key);
    g_started_key = JS_UNDEFINED;
    JS_FreeAtomRT(rt, g_atom_force_async);
    g_atom_force_async = JS_ATOM_NULL;
    JS_FreeValueRT(rt, g_force_async_key);
    g_force_async_key = JS_UNDEFINED;
    g_id_set_async = -1;
    /* GIVEN BACK WITH THE RUNTIME THAT ISSUED IT. The id is only meaningful against that runtime's table, and
       a corpus host takes one down and brings another up per file — a stale id would have the next agent's
       first `<script src="">` queue a task against a def the previous runtime owned. */
    g_err_stepid = -1;
}

/* IS THIS NODE A `script` ELEMENT? The INTERNED TAG ID and the pair of namespaces a `script` can be in, which
   is the same composite test §8.6.4 set and filter HTML step 3 makes a few hundred lines away in element.c — HTML's `script` and
   SVG's are both script elements, and lexbor's own `lxb_html_tree_node_is` answers only for the first because
   it hardcodes the HTML namespace. It replaces a memcmp over the QUALIFIED name, which is the same set by
   accident (a prefixed `foo:script` does not match six bytes) and says nothing about why. */
bool html_script_is(const lxb_dom_node_t *n)
{
    return n != NULL && n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
           n->local_name == LXB_TAG_SCRIPT && (n->ns == LXB_NS_HTML || n->ns == LXB_NS_SVG);
}

/* §4.12.1's `already started` for an element. ABSENT IS FALSE — the standard's own initial value — so this
   reads through node_wrap_peek and never mints a wrapper: an element nothing has marked is an element nothing
   has written, and allocating one to learn a default would put a wrapper on every `<script>` a page inserts. */
static bool script_already_started(JSContext *ctx, const lxb_dom_node_t *n)
{
    JSValueConst wrap;
    JSValue v;
    int r;

    DCHECK(g_atom_started != JS_ATOM_NULL,
           "a script's `already started` was asked for before html_script_init minted its slot key");
    wrap = node_wrap_peek(n);
    if (!JS_IsObject(wrap)) return false;
    r = JS_GetOwnSlot(ctx, &v, wrap, g_atom_started);
    if (r <= 0) return false;
    DCHECK(JS_IsBool(v), "a script's `already started` slot holds something that is not a boolean — the slot is "
                         "written by html_script.c and by nothing else");
    r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return r != 0;
}

/* Write it. This one DOES mint the wrapper, because there is nowhere else for the fact to live — and it is
   reached only for an element some parse or clone actually marked, so the allocation is one per inert script
   rather than one per script. */
static void script_set_already_started(JSContext *ctx, lxb_dom_node_t *n)
{
    JSValue wrap;

    DCHECK(g_atom_started != JS_ATOM_NULL,
           "a script's `already started` was written before html_script_init minted its slot key");
    DCHECK(html_script_is(n), "`already started` was written onto a node that is not an HTML `script` element");
    wrap = node_wrap(ctx, n);
    CHECK(JS_IsObject(wrap), "a script element could not be wrapped to carry its `already started` — an "
                             "unmarked script is one §4.12.1 step 1 lets run, so failing quietly here would "
                             "execute markup the fragment parse is required to keep inert");
    JS_DefinePropertyValue(ctx, wrap, g_atom_started, JS_TRUE, SCRIPT_SLOT_FLAGS);
    JS_FreeValue(ctx, wrap);
}

/* §4.12.1's `force async` for an element. ABSENT IS TRUE — "a script element has a force async boolean,
   INITIALLY TRUE" — which is the opposite of `already started` above and is why the two cannot share a reader:
   an element nothing has written is one whose flag still holds its initial value, and here that value is the
   one that decides the ASAP SET. So a `createElement('script')` needs no wrapper to answer true, exactly as an
   unmarked one needs none to answer `already started` false. */
static bool script_force_async(JSContext *ctx, const lxb_dom_node_t *n)
{
    JSValueConst wrap;
    JSValue v;
    int r;

    DCHECK(g_atom_force_async != JS_ATOM_NULL,
           "a script's `force async` was asked for before html_script_init minted its slot key");
    wrap = node_wrap_peek(n);
    if (!JS_IsObject(wrap)) return true;
    r = JS_GetOwnSlot(ctx, &v, wrap, g_atom_force_async);
    if (r <= 0) return true;
    DCHECK(JS_IsBool(v), "a script's `force async` slot holds something that is not a boolean — the slot is "
                         "written by html_script.c and by nothing else");
    r = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return r != 0;
}

/* Write it. Unlike `already started` this writes BOTH values: false is the interesting one (§4.12.1's three
   writers all clear it) and true has to be expressible because §4.12.1 sets it back on an element whose
   preparation returned early, so a writer that could only clear would make that step unstatable. */
static void script_set_force_async(JSContext *ctx, lxb_dom_node_t *n, bool on)
{
    JSValue wrap;

    DCHECK(g_atom_force_async != JS_ATOM_NULL,
           "a script's `force async` was written before html_script_init minted its slot key");
    DCHECK(html_script_is(n), "`force async` was written onto a node that is not an HTML `script` element");
    wrap = node_wrap(ctx, n);
    CHECK(JS_IsObject(wrap), "a script element could not be wrapped to carry its `force async` — the flag "
                             "decides whether §4.12.1 puts the element in the ASAP SET or in the ordered list, "
                             "so losing a write would silently unorder the page's own lazy chunks");
    JS_DefinePropertyValue(ctx, wrap, g_atom_force_async, JS_NewBool(ctx, on), SCRIPT_SLOT_FLAGS);
    JS_FreeValue(ctx, wrap);
}

/* THE RECEIVER, for the two members below. Web IDL §3.7.6's brand check: `async` reached on something that is
   not a `script` element is a TypeError, which a page distinguishes from `undefined`. */
static lxb_dom_node_t *script_receiver(JSContext *ctx, JSValueConst this_val, const char *member)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (html_script_is(n)) return n;
    JS_ThrowTypeError(ctx, "HTMLScriptElement.%s was reached on something that is not a <script> element",
                      member);
    return NULL;
}

/* §4.12.1: "The async getter steps are: 1. If this's force async is true, then return true. 2. If this's async
   content attribute is present, then return true. 3. Return false."
   STEP 2 IS THE ATTRIBUTE'S PRESENCE and is asked of the attribute LIST, not through get_attribute, which
   answers NULL for the valueless spelling `<script async>` — the same read document_scripts.c had to correct
   for `defer`, and the reason the two halves of §4.12.1 must ask this one question the same way. */
static JSValue js_script_async(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *n = script_receiver(ctx, this_val, "async");

    (void)magic;
    if (!n) return JS_EXCEPTION;
    if (script_force_async(ctx, n)) return JS_TRUE;                                        /* step 1 */
    return JS_NewBool(ctx, lxb_dom_element_has_attribute(lxb_dom_interface_element(n),
                                                         (const lxb_char_t *)"async", 5)); /* steps 2-3 */
}

/* §4.12.1: "The async setter steps are: 1. Set this's force async to false. 2. If the given value is true, then
   set this's async content attribute to the empty string. 3. Otherwise, remove this's async content attribute."
   STEP 1 IS UNCONDITIONAL and is the whole reason this member is not a boolean reflection: `s.async = false` is
   how a page asks for the `list of scripts that will execute in order as soon as possible`, and it does that by
   CLEARING a flag rather than by writing an attribute — the attribute it touches is already absent. */
static JSValue js_script_set_async(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_node_t *n = script_receiver(ctx, this_val, "async");

    (void)magic;
    if (!n) return JS_EXCEPTION;
    script_set_force_async(ctx, n, false);                                       /* step 1 */
    /* THE ATTRIBUTE GOES THROUGH THE COW CHOKEPOINT, like every other attribute write, so the change is
       captured into the running flow's DOM delta and one flow's ordered chunk is not another's. Adding it runs
       §4.9's attribute change steps, which reach html_script_attr_changed and clear the flag a second time —
       the same answer, which is what makes the two writers consistent rather than a pair to keep in step. */
    if (JS_ToBool(ctx, val)) dom_cow_set_attribute(lxb_dom_interface_element(n), "async", "", 0, JS_UNDEFINED);
    else                     dom_cow_remove_attribute(lxb_dom_interface_element(n), "async");
    return JS_UNDEFINED;
}

void html_script_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_id_set_async >= 0, "§4.12.1's `async` was installed before html_script_init declared its setter");
    idl_install_accessor(ctx, proto, "async", js_script_async, 0, g_id_set_async);
}

/* HTML §4.12.1.1 "Processing model": "The script HTML element POST-CONNECTION STEPS, given insertedNode, are:
 * 1. If insertedNode is parser-inserted, then return. 2. Prepare the script element given insertedNode."
 *
 * IT IS THE ENTRY POINT, AND THIS FILE HAD ONE CALLER FOR THREE OF THEM. The comment below already said "a page
 * loads code conditionally in three ways and this is the second", and only the second was wired: `prepare` was
 * reached from DOM §4.2.3's insertion steps and from nowhere else. The spec reaches these same steps from the
 * CHILDREN CHANGED STEPS and from the ATTRIBUTE CHANGE STEPS as well, and both of those are ordinary
 * lazy-loader idioms that this engine silently dropped:
 *
 *     s = document.createElement("script"); document.body.appendChild(s); s.src = "/chunk.js";
 *     s = document.createElement("script"); document.body.appendChild(s); s.textContent = code;
 *
 * In both, the append prepares an element with no source and queues nothing, and the line that actually names
 * the code arrives afterwards — so the chunk was never fetched, never run and never reported, which is the
 * exact defect the paragraph below records for the insertion half and the exact surface this tool exists to
 * reach. Parser-inserted is step 1's own question and the answer here is structurally NO: a parser-inserted
 * script is prepared by html_script_parsed on the document scan, and an element reaching either of the two
 * callers below was mutated by script after the parse. */
/* `imm` IS THE CALLER'S, for the reason html_script.h gives: step 36's nested run belongs to whoever is
   standing on a step machine, and this file's remaining caller of it — §4.12.1.1's attribute change steps —
   stands nowhere near one. The children changed steps below no longer come through here at all: they record. */
static void script_post_connection(JSContext *ctx, lxb_dom_element_t *el, ScriptImmediate *imm)
{
    imm->text = NULL; imm->text_n = 0; imm->el = NULL;
    /* "The HTML element post-connection steps only run when the inserted element is still CONNECTED" — a
       script mutated while detached prepares when it is inserted, through the insertion half, and preparing it
       here as well would run one element's code twice. */
    if (!node_is_connected(lxb_dom_interface_node(el))) return;
    /* THE ELEMENT'S OWN DOCUMENT'S REALM, never the realm that performed the write. §4.12.1 step 32 is "let
       settings object be el's NODE DOCUMENT's relevant settings object" and step 34's base URL is that
       document's, and two same-origin documents are ONE agent — so `frame.contentDocument.body.appendChild(s)`
       reaches these steps from the parent's realm about a child's element. core/dom/element.c's own walk made
       exactly this correction for insert step 12; the attribute change steps had not. */
    ctx = document_realm_of(lxb_dom_interface_node(el));
    DCHECK(ctx != NULL,
           "§4.12.1.1's post-connection steps reached a connected `script` element in a document no realm was "
           "installed for — a document that can hold a connected node is a document a flow can run steps in, "
           "so build its realm rather than borrowing whichever one performed the write");
    /* NOT PARSER-INSERTED, and step 1 of these very steps is why: "If insertedNode is parser-inserted, then
       return" — so an element that reaches the post-connection steps at all has a null parser document. */
    html_script_prepare(ctx, el, /*parser_inserted*/false, imm);
}

/* §4.12.1.1: "The script CHILDREN CHANGED STEPS given changedNode are: 1. If the script element is not
 * connected, then return. 2. Run the script HTML element post-connection steps, given changedNode."
 * The hook is handed the PARENT whose child list changed, which for this family IS changedNode — the script
 * element whose text was written.
 *
 * IT RECORDS AND DOES NOT RUN, WHICH IS WHAT MAKES THIS DOOR CONVERGE. Step 2's post-connection steps end at
 * §4.12.1.1 step 36's "immediately execute the script element", and this hook is called from inside the DOM
 * mutation chokepoint (core/dom/node.c's node_children_changed, on DOM §4.2.3's `insert`, its `remove` and
 * §4.10's `replace data`) — a C body with no flow base under it, where a nested program would be the
 * drive-to-completion this engine aborts on. So `s.textContent = code` hands the element to the SAME record
 * insert step 12 is drained from, and the one machine that can hold the request runs both doors' elements.
 * THE ORDER THAT MOVES IS BETWEEN §4.2.3's STEP 9 AND ITS STEP 12, AND NOTHING ELSE. `insert` runs the children
 * changed steps at step 9 and the post-connection steps at step 12, with only step 10's declaration and step
 * 11's collection in between — none of which runs the page's code — so an element recorded here and run there
 * runs at the same point in the page's own timeline. What CANNOT happen is the two families interleaving,
 * because the walk drains one list in one order.
 * THE REALM IS NOT TAKEN HERE: node.h states that `ctx` is the MUTATING realm, and the drain resolves the
 * node's own document's realm per entry. This hook took the mutating one and handed it to `prepare` as if it
 * were §4.12.1 step 32's settings object. */
static void script_children_changed(JSContext *ctx, lxb_dom_node_t *parent)
{
    (void)ctx;
    if (!html_script_is(parent)) return;
    /* STEP 1 — "if the script element is not connected, then return". Asked HERE because it is the children
       changed steps' own step and it is asked at their own moment; insert step 12's connectedness is a
       SEPARATE and later read, which the drain performs per entry (an earlier entry's program may have removed
       this one). */
    if (!node_is_connected(parent)) return;
    element_post_connection_record(parent);
}

void html_script_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                              const char *val)
{
    if (!html_script_is(lxb_dom_interface_node(el))) return;
    if (ns != NULL || !local) return;   /* "If namespace is not null, then return." */
    /* HTML §4.12.1.1 "Processing model"'s attribute change steps FOR `script` ELEMENTS — the family is DOM
       §4.9 "Interface Element"'s and this standard states this element's: "If localName is `src`, value is not
       null, and element is connected, then run the script HTML element post-connection steps, given
       element." REMOVING `src` is not one of
       them — the step asks for a non-null value — so a page that clears the attribute loads nothing, which is
       what it does in a browser. */
    if (!strcmp(local, "src")) {
        if (val) {
            ScriptImmediate imm;

            script_post_connection(ctx, el, &imm);
            /* AND IT CANNOT OWE A NESTED RUN, WHICH IS WHY THIS DOOR NEEDS NO MACHINE UNDER IT. §4.12.1.1
               reaches step 36 only for what falls PAST step 35's "if el's type is `classic` and el has a src
               attribute, or el's type is `module`", and this door is entered only when the `src` attribute
               change steps saw a non-null value — which is the attribute already written, since §4.9's
               attribute change steps run after the write. So an element standing at step 36 here would be one
               that arrived without the attribute it was told about. */
            DCHECK(imm.text == NULL,
                   "§4.12.1.1's `src` attribute change steps prepared a script that owes step 36's immediate "
                   "execution — step 36 is reached only past step 35's \"el has a src attribute\", and this "
                   "door is entered only when that attribute was just set, so the element and the attribute "
                   "change that named it disagree");
            html_script_immediate_free(&imm);
        }
        return;
    }
    if (strcmp(local, "async")) return;   /* the `async` CONTENT attribute, null namespace */
    if (!val) return;   /* "when an async attribute is ADDED" — removing one does not set the flag back */
    script_set_force_async(ctx, lxb_dom_interface_node(el), false);
}

/* A `<template>`'s CONTENT FRAGMENT, or NULL — a tree reached other than through child links, and the reason
   this walk is not the three-line one beside it. */
static lxb_dom_node_t *template_content(lxb_dom_node_t *n)
{
    lxb_html_template_element_t *t;

    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || !lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE)) return NULL;
    t = lxb_html_interface_template(n);
    return t->content ? &t->content->node : NULL;
}

void html_script_parsed(JSContext *ctx, lxb_dom_node_t *root, bool inert)
{
    lxb_dom_node_t *n = root, *content;

    if (!root) return;
    /* AN ITERATIVE DESCENT, like dom_attr_normalize_parsed's over the same tree at the same moment: this is a
       parse product, so its depth is the markup's, and a recursive walk would put the page's nesting on the C
       stack — which §C-stack is the whole reason nothing in this engine does.
       IT ENTERS `<template>` CONTENTS, which the walk beside it does not have to. The parser puts a template's
       markup in its CONTENT FRAGMENT, and a `<script>` in there was created by THIS parse under the same Inert
       mode, so it is already started too — and it is reachable: `t.content.cloneNode(true)` copies it out, and
       §4.12.1.1 "Processing model"'s cloning steps carry the flag with it, so an unmarked one would run from
       the clone. lexbor
       leaves the fragment's `parent` NULL and points its `host` back at the element, which is what the ascent
       climbs; a template can hold BOTH lists (only the parser and `t.content` reach the fragment, while
       `t.appendChild(x)` reaches the element), so coming back visits the ordinary children next. */
    for (;;) {
        if (html_script_is(n)) {
            /* §4.12.1.1: `force async` "is set to false by the HTML parser and the XML parser on script
               elements they insert" — EVERY parse, not only the inert one, which is why this walk is no longer
               the Inert marking alone. Without it a parsed `<script>` kept the boolean's initial TRUE and its
               `async` getter answered true for markup that has no `async` attribute; the ordered-list branch of
               §4.12.1 would be unreachable for it too. */
            script_set_force_async(ctx, n, false);
            /* …and §13.2.4.5's INERT mode's own stamp, which is the FRAGMENT parse's alone. */
            if (inert) script_set_already_started(ctx, n);
        }
        content = template_content(n);
        if (content && content->first_child) { n = content->first_child; continue; }
    children:
        if (n->first_child) { n = n->first_child; continue; }
        for (;;) {
            if (n == root) return;
            if (n->next) { n = n->next; break; }
            if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT &&
                lxb_dom_interface_document_fragment(n)->host != NULL) {
                n = lxb_dom_interface_node(lxb_dom_interface_document_fragment(n)->host);
                goto children;
            }
            n = n->parent;
            DCHECK(n != NULL,
                   "the parser's script marking walked off the top of the tree it was given — every node it "
                   "reaches is either under `root` or in a `<template>` content fragment whose host is, so a "
                   "null parent means the parse handed back a node that is in neither");
        }
    }
}

void html_script_cloned(JSContext *ctx, lxb_dom_node_t *src, lxb_dom_node_t *copy)
{
    if (!html_script_is(src)) return;
    DCHECK(html_script_is(copy),
           "DOM §4.4 clone a node produced a copy of a `script` element that is not one — the cloning steps "
           "HTML defines for `script` are stated over a copy of the same element, and a pair that disagrees "
           "means step 2's `clone a single node` built the wrong interface");
    /* "Set copy's already started to node's already started." FALSE is the copy's initial value and there is
       no slot to clear — a fresh element has never been written — so only the true case has anything to do. */
    if (script_already_started(ctx, src)) script_set_already_started(ctx, copy);
}

/* HTML §4.12.1 "The script element"'s "prepare the script element" — the one body, reached by both of the two
 * ways a `script` element becomes a program in this engine.
 *
 * THE INSERTION HALF is DOM §4.2.3's insertion steps and the §4.12.1.1 post-connection/children-changed steps
 * beside them: `s = createElement("script"); s.src = u; body.appendChild(s)`. Before this existed the injection
 * was a SILENT no-op — the element went into the tree and the code it named was never fetched, never run, never
 * even reported, so every endpoint and sink behind an A/B flag or a feature gate was missing with nothing to say
 * so. The loaded code is more PROGRAM OF THE INJECTING FLOW: it joins that flow's script sequence, so it runs
 * under the delta, the pins and the position in the BFS of the world that injected it, and a sibling that never
 * took the branch never sees it.
 *
 * THE PARSER HALF is HTML §13.2.6.4.8 'The "text" insertion mode' — "An end tag whose tag name is 'script' …
 * prepare the script element script" — which is `html_script_parser_inserted` below, and which is why
 * `parser_inserted` is a PARAMETER. §4.12.1 step 2 reads it off the element's `parser document`, a field this
 * engine does not keep (html_script.h says why it was a stub); the CALLER is the party that holds it, because
 * the caller is either §13.2.6 tree construction, which is the thing that sets it, or page code, which cannot.
 * It was a hardcoded `false` with a paragraph arguing that everything reaching here was page-inserted, and that
 * paragraph is gone with the second caller it did not anticipate. */
void html_script_prepare(JSContext *ctx, lxb_dom_element_t *el, bool parser_inserted, ScriptImmediate *imm)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    size_t n_len = 0;
    const lxb_char_t *src;
    ScriptSchedule sched;
    bool has_src;
    /* THE ELEMENT'S TYPE OUTLIVES THE STEPS THAT COMPUTE IT, because §4.12.1.1 asks it TWICE: once to decide
       whether anything runs at all, and again at "execute the script element", whose whole body is a switch on
       it. It was scoped to the first question while only the first question existed. */
    ScriptType st;
    JSValue t;

    /* THE REPORT IS EMPTIED FIRST AND ON EVERY PATH, which is what lets a caller read it as a POSITIVE
       statement rather than as a field it must remember whether anything wrote. `prepare` has fifteen returns
       and every one of them means "no nested run is owed"; only the arm at the very bottom fills it. */
    DCHECK(imm != NULL, "§4.12.1 was prepared with nowhere to report step 36 — the record is mandatory because "
                        "the one destination it names is a nested run, and a caller with nowhere to put it is "
                        "a caller that would silently drop the page's own code");
    imm->text = NULL; imm->text_n = 0; imm->el = NULL;
    if (!html_script_is(n)) return;
    /* STEP 1: "If el's already started is true, then return." This is the whole of what makes §13.4's fragment
       parse inert — the parsed script is in the tree, is queryable, serialises back out, and does not run. */
    if (script_already_started(ctx, n)) return;
    /* STEPS 5 AND 6 — "Let source text be el's child text content" and "If el has no src attribute, and source
       text is the empty string, then return". THEY ARE HERE, AHEAD OF STEP 15, BECAUSE STEP 15 IS NOW
       PERFORMED, and the pair is what keeps it from being performed on an element the standard leaves alone.
       `s = createElement("script"); body.appendChild(s); s.textContent = code` is the idiom: the append
       prepares an element with no source, which must return at step 6 with `already started` STILL FALSE so
       that the assignment can prepare it again — the second of the two lazy-loader shapes the post-connection
       steps exist for. Setting the flag before this test would make that element permanently inert and the
       chunk would never run, which is the exact defect those steps were built to end.
       THE LENGTH IS THE DOM's AND NOT A `strlen` — see the report at the bottom for why this element's text is the
       one inline source that can hold a U+0000.
       AND IT IS THE **CHILD** TEXT CONTENT, WHICH IS THE STANDARD'S OWN WORD AND NOT THIS ENGINE'S SHORTHAND.
       DOM §4.11 "Interface Text" defines two concatenations and §4.12.1.1 names the CHILD one; the difference
       is invisible in an HTML document, where §13.2.5.4 "Script data state" makes a `script`'s content one
       Text node and nothing else, and REAL in an XML one, where a `script` element's content is XML §3.1's
       [43] `content` and may hold elements. It is also where DOM's "Text node" stops meaning "nodeType is
       TEXT": §4.12 "Interface CDATASection" is `interface CDATASection : Text { };`, so the CDATA section an
       XHTML document writes its program in IS part of this concatenation. core/dom/text_content.h answers both
       halves once; a nodeType-keyed walk answered "" here and took step 6's return for a page that had a
       program. */
    has_src = lxb_dom_element_has_attribute(el, (const lxb_char_t *)"src", 3);
    {
        char *probe = has_src ? NULL : dom_child_text_content(n, &n_len);

        if (!has_src) {
            bool empty = n_len == 0;

            free(probe);
            if (empty) return;
        }
    }
    /* THE TYPE-STRING STEPS, WHICH THIS HALF NEVER ASKED — so an injected `<script type="application/json">`
       was handed to the compiler and RAN, as did an import map, while the document-scan half had recognised
       both since it was written. One element, one question: `script_block_type` is that question, and the two
       halves of §4.12.1 must not disagree about what a `type` attribute means. */
    {
        st = script_block_type(el);
        /* HTML's null and the two data types: "No script is executed." An import map and a set of speculation
           rules are REGISTERED on the relevant global rather than evaluated, which is a capability this engine
           does not have — and their absence is honest, because neither runs code. */
        if (!script_type_executes(st)) {
            /* …BUT ONLY *NULL* LEAVES THE ALGORITHM AT THIS STEP, AND THE OTHER TWO LEAVE IT AT STEP 33.
               §4.12.1.1's type switch returns in exactly one arm, its last: step 13 is "Otherwise, return.
               (No script is executed, and el's type is left as null.)" — and `importmap` and
               `speculationrules` are not that arm, they are steps 11 and 12, which SET el's type and fall
               through. So they walk on through step 15's "Set el's already started to true." and step 18's
               "If scripting is disabled for el, then return." and only then reach step 33, "If el has a src
               content attribute:", whose FIRST sub-step is "If el's type is \"importmap\" or
               \"speculationrules\", then queue an element task on the DOM manipulation task source given el
               to fire an event named error at el, and return". Collapsing those returns into this one is
               wrong in two observable ways at once — the element is left UNMARKED, so a later prepare runs
               the whole algorithm again, and the `error` the standard owes is never fired.
               IT CRASHES RATHER THAN FIRING THE EVENT FROM HERE, because firing it at step 13 would be the
               observable half of a step whose other half (step 15's mark, and step 18's scripting-disabled
               test, which can legitimately suppress the event entirely) would still be missing — a half-fix
               wearing a fixed one. What the next diff builds is the fall-through: the two data types reach
               steps 15 and 18 alongside the executing types and branch at step 33, where
               html_script_queue_error is already waiting for them. Its absence shows as this abort, on markup
               no browser supports either (§4.12.1.1's own note: "External import maps and speculation rules
               are not currently supported"), which is why it is reachable at all rather than common. */
            DCHECK(!has_src || (st != SCRIPT_TYPE_IMPORTMAP && st != SCRIPT_TYPE_SPECULATIONRULES),
                   "a `<script type=importmap|speculationrules>` carries a `src` — HTML §4.12.1.1 "
                   "\"Processing model\" does NOT return for these at its type step (only a null type does), "
                   "so this element owes step 15's `already started` mark, step 18's scripting-disabled test "
                   "and then the src branch's `queue an element task on the DOM manipulation task source "
                   "given el to fire an event named error at el`. Let the two data types fall through to "
                   "those steps and branch at the src step, where html_script_queue_error is");
            return;
        }
        /* A MODULE TRAVELS THIS ROUTE NOW, and the row is what carries it: the flow's dynamic sequence has a
           ScriptType per entry (solver/flow.h's `dyn_type`), so flow_step evaluates an injected
           `<script type=module>` with §8.1.4.4 "Calling scripts"'s run-a-module-script rather than handing the
           page's own `import` to the classic entry and taking a SyntaxError back from a parser that is fine.
           The DCHECK that stood here aborted the whole engine on that markup — one of three, with
           core/frame/navigable.c's and solver/engine.c's engine_join_document, all three naming that column. */
        /* STEP 14 — "If parser document is non-null, then set el's parser document back to parser document and
           SET EL'S FORCE ASYNC TO FALSE." It is the SECOND half of a round trip and the net of the pair is what
           is written here: step 4 sets force async TRUE for a parser-inserted element with no `async`
           attribute, and this step sets it false again whether or not step 4 fired, so a parser-inserted
           element leaves these steps with force async FALSE unconditionally. That is the same value §4.12.1.1's
           parser stamp gives a PARSED element (`html_script_parsed`), and it has to be written here as well
           because a script the parser prepares at its own end tag reaches this line BEFORE that stamp runs —
           the stamp is applied to the finished tree and the end tag is inside the parse. */
        if (parser_inserted) script_set_force_async(ctx, n, false);
        /* §4.12.1's LAST STEPS, asked of the same element by the same function the document scan asks — one
           element, one classification. */
        sched = script_block_schedule(el, st, parser_inserted, script_force_async(ctx, n));
    }
    /* STEP 15 — "Set el's already started to true." IT WAS MISSING, and the two shapes it costs are both
       ordinary: `s.textContent = code; s.src = "/chunk.js"` prepared the element TWICE (the children-changed
       steps, then the attribute change steps) and ran the same program twice, and a `<script>` the §13.2.6.4.8
       route below prepares would be prepared again by any later reach at all. The flag is what makes
       "prepare" idempotent, which is the whole of step 1's job, and step 1 had nothing to read.
       IT IS AFTER THE TYPE STEPS AND AHEAD OF EVERY REMAINING RETURN, which is where §4.12.1 puts it: an
       element whose type runs nothing is left unmarked (step 13 returns before this), and an element whose
       `src` does not parse is marked and then abandoned (step 33's own arms return after it), so a page that
       fixes the URL afterwards does NOT get a second run. */
    script_set_already_started(ctx, n);
    /* STEP 18 — "If scripting is disabled for el, then return", AT STEP 18 AND NOT AT A DOOR.
     *
     * IT IS THE ORDER THAT IS THE SPEC HERE. §4.12.1.1 "Processing model" puts this test FOURTEEN STEPS after
     * step 1 and THREE after step 15, so every script the parser prepares in a document with no browsing
     * context leaves these steps with `already started` TRUE and runs nothing — which is the whole of why a
     * `<script>` cannot be laundered through a scripting-disabled document and re-inserted into a live one.
     * The step's own note names that population: "scripts in XMLHttpRequest's responseXML documents, scripts
     * in DOMParser-created documents, scripts in documents created by XSLTProcessor's transformToDocument
     * feature, and scripts that are first inserted by a script into a Document that was created using the
     * createDocument() API".
     *
     * IT WAS ASKED AT ONE CALLER AND NOT AT THE OTHER, WHICH IS ONE MISSING STEP WEARING TWO SHAPES.
     * `html_script_parser_inserted` asked it at its DOOR — before step 1 — so a parsed script skipped steps 1
     * through 17 and kept `already started` FALSE; `script_post_connection` did not ask it at all, so a
     * `<script>` a program appended into a DOMParser document was COMPILED AND RUN in that document's realm.
     * Both are the same step being somewhere other than step 18, and both are closed by it being here: one
     * question, one site, N callers, which is what stops the next door from having to remember it.
     *
     * §8.1.3.4 "Enabling and disabling scripting" defines it over the NODE DOCUMENT of the object, so that is
     * what is asked — never `ctx`, which is only the realm the flag above was written through and which any
     * realm that can reach the node can be. Its clause is "The object implements Node, and object's node
     * document's browsing context is null", and `document_active_realm_of` answering NULL IS that clause: a
     * Document that is not the active document of the realm that owns it has no browsing context, which is
     * exactly the four-way population step 18's own note lists. */
    {
        JSContext *bc = document_active_realm_of(lxb_dom_interface_node(n->owner_document));

        if (!bc) return;
        /* …AND IT IS THE SAME REALM EVERY STEP BELOW USES, WHICH IS STEP 32 ASSERTED RATHER THAN ASSUMED.
           §4.12.1.1 step 32 is "let settings object be el's node document's relevant settings object" and step
           34's base URL is that document's, so every destination below — `document_doc(ctx)` for both queues,
           `script_src_absolute`'s base, `endpoint_record` — must stand in EL'S NODE DOCUMENT's realm and not
           in whichever realm performed the write. All three callers derive `ctx` that way (`document_realm_of`
           of the node), and this line is what turns a fourth one that does not into an abort instead of a
           child's program compiled in its parent's Window: two same-origin documents are ONE agent, so
           `frame.contentDocument.body.appendChild(s)` reaches these steps from the PARENT's realm about a
           CHILD's element, which makes the mutating realm a plausible wrong answer rather than an obvious
           one. This file's own history has it — the children-changed hook took the mutating realm and handed
           it to `prepare` as if it were step 32's settings object. */
        DCHECK(bc == ctx,
               "§4.12.1.1 was prepared with a realm that is not el's node document's — step 32's settings "
               "object and step 34's base URL are the NODE DOCUMENT's, so a caller passing the realm that "
               "PERFORMED THE WRITE compiles this element's program in another document's Window and resolves "
               "its `src` against another document's base URL. Derive it at the call site with "
               "document_realm_of(el), never from the member's own ctx");
    }
    /* An UNKNOWN src is a URL this engine cannot fetch, but it is still a request the page makes — recorded so
       it reaches the @H surface as the shape it is, rather than disappearing. */
    t = dom_cow_attr_taint(el, "src");
    if (!JS_IsUndefined(t)) {
        /* AND IT IS A REQUEST RUNNING CODE BUILT, WHICH IS ASSERTED RATHER THAN ASSUMED. `observed` — the one
           grade `engine_prov_of_running_path` cannot answer — is "a real load of this document makes exactly
           this request", and its first conjunct is §4.12.1's parser-inserted flag, which IS in hand here as a
           parameter. It is structurally never the answer on this arm: the taint shadow map holds an entry for
           (el, "src") only where a script ASSIGNED the attribute a concolic value (solver/attr_shadow.h), and
           a parser-inserted element's attributes come out of the tokenizer, never through that write. So the
           two facts cannot both hold, and a day they do is a day this element's provenance is being composed
           from the wrong one of them rather than a day the grade is merely coarse. */
        DCHECK(!parser_inserted,
               "a PARSER-INSERTED `<script>` reached §4.12.1 with a TAINTED `src` — the taint shadow map is "
               "written only by a script assigning the attribute, so either the parser has started routing "
               "attribute values through it (and this request's provenance is `observed`, which the running "
               "path cannot say) or an element's shadow entry is being read for the wrong element");
        endpoint_record(ctx, "GET", t, NULL, 0, NULL, engine_prov_of_running_path());
        return;
    }
    /* §4.12.1's `src` BRANCH IS ENTERED ON THE ATTRIBUTE, which is the same correction the document scan needed
       and for the same reason: `get_attribute` answers NULL for an attribute whose value is absent, so a
       presence test written over the VALUE let `<script src="">` fall through to the child-text branch and RUN
       it — markup a browser runs nothing for. */
    if (has_src) {
        char *u;

        src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &n_len);
        /* "IF SRC IS THE EMPTY STRING, THEN QUEUE AN ELEMENT TASK ON THE DOM MANIPULATION TASK SOURCE GIVEN EL
           TO FIRE AN EVENT NAMED ERROR AT EL, AND RETURN." Both halves of that sentence run now; the return
           alone stood here for as long as the event was a comment. A NULL from `get_attribute` is the same
           arm: `has_src` was answered by `has_attribute`, so the attribute IS present and an absent VALUE is
           the empty string — which is exactly what the step names. */
        if (!src || !n_len) { html_script_queue_error(ctx, el); return; }
        /* "ENCODING-PARSING A URL GIVEN src, RELATIVE TO EL'S NODE DOCUMENT" — §4.12.1's own step, and the
           realm this chokepoint was entered with IS that document (core/dom/element.c hands the inserted node's
           document, not the mutating one). It was missing: the raw ATTRIBUTE went to the host, so an injected
           `<script src="./chunk.js">` named an address only the host's own base could resolve — and the host
           that has one is a different origin from the page. NULL is the standard's branch for a `src` that does
           not parse: "if url is failure, then queue an element task on the DOM manipulation task source given
           el to fire an event named error at el, and return" — so the element runs no script AND the page is
           told, which is the half that used to be a paragraph instead of a task. */
        u = script_src_absolute(ctx, (const char *)src, n_len);
        if (!u) { html_script_queue_error(ctx, el); return; }
        /* FETCH §4.3 SCHEME FETCH IS ASKED BY WHICHEVER PARK THIS URL REACHES, AND NEITHER DESTINATION BELOW
           NEEDS A LINE HERE. §8.1.4.2 "Fetching scripts"' fetch is the same algorithm `fetch()` runs, so §4.3's
           switch decides who answers `<script src="data:text/javascript,…">` — a 200 built out of bytes already
           in this address space — and both destinations below hand the URL to the flow's pending register,
           where solver/engine.c's pending_park_request runs §4.3 and places its response on the record. The
           delivery then reads that response exactly as it reads the trusted zone's, so an element loaded from
           a local scheme takes the same position, the same decode and the same `currentScript` bracket as one
           the host fetched. */
        /* WHICH OF THE TWO ASAP DESTINATIONS, and the difference is a POSITION. The `set of scripts that will
           execute as soon as possible` has none — §13.2.7 waits for that set only before the load event — so it
           parks and its reply becomes a program whenever it drains. The `list of scripts that will execute in
           order as soon as possible` is what `s.async = false` puts an element in, and §4.12.1's own steps for
           it are "if scripts[0] is not el, then abort" — the element holds its place against the others, so it
           takes a slot in the flow's sequence and the flow stops there until the reply fills it. */
        if (sched == SCRIPT_SCHED_ASAP) engine_pending_script_url(ctx, u, st, el);
        else {
            /* …AND THE THREE ORDERED DESTINATIONS ARE ONE DESTINATION HERE, WHICH IS A STATEMENT ABOUT THIS
               ENGINE'S ONE SEQUENCE AND NOT A COLLAPSE OF THREE SPEC STEPS INTO ONE. §4.12.1's `list of scripts
               that will execute in order as soon as possible`, its `list of scripts that will execute when the
               document has finished parsing` and its `pending parsing-blocking script` differ in WHEN §13.2.7
               "The end" drains them relative to the document's OWN scripts — and a script reaching this line
               was prepared while a flow is standing past every one of those, so all three name the same place:
               the position this element holds against the other programs the flow has yet to run. That is what
               engine_queue_docscript_url is (solver/engine.h: the element takes the slot and the flow stops
               there until the reply fills it).
               WHAT IS NOT EXPRESSED, AND IS NAMED RATHER THAN APPROXIMATED AWAY: §13.2.6.4.8's pending
               parsing-blocking script BLOCKS THE TOKENIZER, so in a browser the markup written after a
               `<script src>` in one `document.write` is tokenized only once that script has run. This engine
               tokenizes the whole chunk first (core/html/html_parse.h states the parser-suspension capability
               that is owed), so the DOM those later bytes build exists before the script does. The script
               ORDER is right either way, which is what this destination is chosen for. */
            DCHECK(sched == SCRIPT_SCHED_IN_ORDER_ASAP || sched == SCRIPT_SCHED_PARSER_BLOCKING ||
                   sched == SCRIPT_SCHED_WHEN_PARSED,
                   "an external script was scheduled somewhere other than the four destinations §4.12.1 has "
                   "for one — the fifth is `immediately execute the script element`, which the standard "
                   "reaches only for what falls past \"if el's type is `classic` and el has a src attribute\", "
                   "so an element with one cannot be standing there");
            engine_queue_docscript_url(document_doc(ctx), u, st, el);
        }
        free(u);
        return;
    }
    /* No src: the element's own text IS the program. WHERE it runs is the schedule's answer and the two
       answers are different spec steps — §4.12.1.1 reaches `immediately execute the script element` only for
       what falls past "If el's type is `classic` and el has a src attribute, or el's type is `module`", so an
       inline CLASSIC script runs INSIDE the operation that reached these steps and an inline MODULE joins one
       of the two `as soon as possible` destinations and takes a POSITION in the sequence. A module has a graph
       to LOAD before its result exists, which is exactly why the standard does not run it in place. */
    DCHECK(sched == SCRIPT_SCHED_IMMEDIATE || st == SCRIPT_TYPE_MODULE,
           "an inline injected CLASSIC script is scheduled somewhere other than its own insertion point — "
           "§4.12.1.1 owes no fetch for a classic script whose source it already has, so its tail ends at "
           "`immediately execute the script element` and only a module leaves by another door");
    {
        char *txt = dom_child_text_content(n, &n_len);
        /* STEP 5's SOURCE TEXT AGAIN, AND IT IS NOT EMPTY — step 6 above returned for an element with no src
           whose child text content is the empty string, and no page code has run since (the steps between are
           this engine's own reads). A second read rather than a saved buffer because the first is discarded on
           the path that keeps going, and holding it would mean owning it across the type steps' returns. */
        DCHECK(txt != NULL && n_len != 0,
               "§4.12.1's step 15 marked a `script` element already started and then found it has no program — "
               "step 6 returns for an element with no `src` whose source text is empty, and nothing between "
               "that step and this one can change the element's children, so an empty one here means the two "
               "reads of the child text content disagree");
        if (txt) {
            /* IN THE DOCUMENT WHOSE TREE IT WAS INSERTED INTO — "prepare the script" runs it with the
               element's node document's settings object, which is the realm this chokepoint was entered
               with. A program is a program OF a document (solver/flow.h), so it names one. */
            /* …AND AT THE SLOT THE ASSERT ABOVE ALREADY NAMED, WHICH IS AHEAD OF EVERYTHING THE SEQUENCE HOLDS
               AND STILL BEHIND THE PROGRAM THAT CAUSED IT. §4.12.1.1's last step is "immediately execute the
               script element el, even if other scripts are already executing"; the slot after the cursor is
               ahead of every program the flow has left, which is what the APPEND entries — the TAIL, the
               position of the `as soon as possible` destinations this element is explicitly not in — got
               wrong. It is not what "immediately" means, and the DFAIL below is where that is stated. */
            /* AN INLINE MODULE TAKES A POSITION INSTEAD — see the schedule note above. Both `as soon as
               possible` destinations an injected module reaches hold their elements in order, or in the SET's
               case have no position at all (§13.2.7 waits for the set only before the load event), so the tail
               of this flow's sequence is a correct place for both. */
            if (n_len) {
                /* AND THE ELEMENT GOES WITH THE PROGRAM, at both destinations: §4.12.1.1's "execute the
                   script element" is a switch on EL and its classic arm sets this document's §3.1.7
                   `currentScript` to it for the run — which is what a page reads back to find its own
                   `<script>` and, through it, the prefix its lazy chunks are served from. */
                /* AND `n_len` GOES WITH IT, WHICH IS THE LENGTH THIS CALL ALREADY HELD AND WAS DROPPING. The
                   queue took a C string, so an injected program was read to its first NUL — and THIS element's
                   text is the one inline source that can hold one: it was ASSIGNED by page code
                   (`s.textContent = …`, `s.text = …`), so it never went through HTML §13.2.5.4 "Script data
                   state", whose U+0000 NULL row ("Emit a U+FFFD REPLACEMENT CHARACTER character token") is
                   what makes a PARSED inline script NUL-free. ECMAScript §11.1 "Source Text" permits every
                   code point from U+0000 up, so an assignment of `x="<U+0000>";X9()` is a program a browser
                   runs whole and this engine ran the three bytes in front of that code point of. */
                if (st == SCRIPT_TYPE_MODULE) {
                    engine_queue_element_script(document_doc(ctx), (const char *)txt, n_len, st, el);
                } else {
                    /* STEP 36'S LAST SUB-STEP — "Otherwise, immediately execute the script element el, even if
                       other scripts are already executing" — REPORTED. Every OTHER destination §4.12.1 has is a
                       POSITION IN A SEQUENCE (a list, a set, a pending slot) and a row in the flow's program
                       sequence expresses each of them exactly; this one is a NESTED RUN inside the operation
                       that reached these steps, which no position can say. The queue's nearest expression of it
                       — the slot after the running program — put the REST OF THE CAUSING PROGRAM in front of
                       it, so `body.appendChild(s); f()` ran `f()` before `s`'s code where a browser runs it
                       after. See html_script.h for why the report leaves this algorithm rather than being
                       performed in it: this body is reached from inside a DOM mutation, from inside an
                       attribute change and from inside a parse, and running the page's code needs a flow base
                       under it that none of those three has.
                       THE COPY IS THE LIFETIME AND NO LONGER THE NUL. It used to be the NUL: the tree layer
                       allocated `length + 1` for an element's text content and its concatenating walk never
                       wrote the last byte, so a program compiled off that buffer ended in whatever the
                       document arena last held there. core/dom/text_content.h terminates its own answer, so
                       that argument has been retired — what remains is that `txt` is released at the bottom of
                       this block while `imm->text` is read by the flow that performs step 36 afterwards, which
                       is a lifetime and not a byte. */
                    imm->text = malloc(n_len + 1);
                    CHECK(imm->text != NULL,
                          "§4.12.1.1 step 36's source text could not be copied — dropping it means an injected "
                          "inline script silently never runs, which is the whole surface this engine exists to "
                          "reach");
                    memcpy(imm->text, txt, n_len);
                    imm->text[n_len] = '\0';
                    imm->text_n = n_len;
                    imm->el = el;
                }
            }
            free(txt);
        }
    }
}

/* THE REALM THE TWO PARSER ENTRIES BELOW RUN IN — §13.2.6's for both, and §14.2's for the second of them; see
   html_script.h for why it is derived and not passed.
   TWO QUESTIONS, AND ONLY ONE OF THEM IS A DOOR'S TO ASK.
   `document_realm_of` is the realm the DOM's own steps run in, and is what the `already started` slot is
   WRITTEN through: the slot is an own property of the element's wrapper, so it needs A realm and any realm
   that can reach the node will do. That is the question a door asks, because a document no realm has ever
   reached has nowhere to put the flag. `document_active_realm_of` is §8.1.3.4 "Enabling and disabling
   scripting"'s browsing context — a strictly narrower answer, which a DOMParser document does not have — and
   it is §4.12.1.1's STEP 18, so it is asked at step 18, inside `html_script_prepare`, once for every caller.
   IT USED TO BE ASKED HERE, AT THE DOOR, AND THAT IS AN ORDER BUG RATHER THAN A PLACEMENT PREFERENCE: step 18
   sits fourteen steps after step 1 and three after step 15, so hoisting it skipped the marking that makes a
   scripting-disabled document's `<script>` inert once it is moved into a live one. A door that answers a
   later step's question decides the earlier ones by omission. */

void html_script_end_of_file(lxb_dom_node_t *script)
{
    JSContext *ctx = document_realm_of(script);

    DCHECK(script != NULL, "§13.2.6.4.8's end-of-file step was reached with no current node");
    DCHECK(html_script_is(script),
           "§13.2.6.4.8's end-of-file step was handed a node that is not a `script` element — the step is "
           "\"if the current node is a script element\", so the test belongs to the caller and a node that "
           "failed it should never have arrived");
    /* A DOCUMENT NO REALM HAS EVER REACHED HAS NO WRAPPER TO WRITE THE FLAG ON, and it needs none: nothing can
       read the flag either, because reading it is `already started` and the only readers are §4.12.1's step 1
       and its cloning steps, both of which run in a realm. A solver scratch parse (solve_html.c's witness
       documents) is the population, and its `<script>` elements are never prepared by anything. */
    if (!ctx) return;
    script_set_already_started(ctx, script);
}

void html_script_parser_inserted(lxb_dom_node_t *script)
{
    JSContext *ctx;

    DCHECK(script != NULL, "a parser reached a `script` element's end tag with no element — HTML §13.2.6.4.8 "
                           "'The \"text\" insertion mode' takes the CURRENT NODE and HTML §14.2 \"Parsing XML "
                           "documents\" takes the element whose end tag was just parsed, and neither of those "
                           "can be absent at the moment the step runs");
    DCHECK(html_script_is(script),
           "a parser's `script` end-tag step was handed a node that is not a `script` element — §13.2.6.4.8 "
           "says \"let script be the current node (which will be a script element)\", so an HTML caller that "
           "misses means it took the current node at a moment other than before the pop; an XML caller that "
           "misses asked §14.2's question about the wrong end tag, since core/xml/xml_tree.h reports EVERY "
           "element's close and the `script` test is the caller's filter");
    /* THE REALM THE FLAG IS WRITTEN THROUGH, WHICH IS NOT THE ONE STEP 18 ASKS ABOUT — see the two-questions
       note above. §13.2.6.4.8 'The "text" insertion mode' says "prepare the script element script" with no
       condition on the document's scripting mode at all, so this door performs §4.12.1.1's steps IN ORDER and
       lets step 18 be reached at step 18. It used to ask step 18 HERE, ahead of step 1, and the cost was step
       15: a script the parser prepared in a document with no browsing context kept `already started` FALSE,
       so moving it into a live document ran it — the launder a browser's step ordering forbids.
       A DOCUMENT NO REALM HAS EVER REACHED still returns, for html_script_end_of_file's reason and not for
       step 18's: there is no wrapper to write the flag on, and nothing can read it either. */
    ctx = document_realm_of(script);
    if (!ctx) return;
    {
    ScriptImmediate imm;
    /* "PREPARE THE SCRIPT ELEMENT SCRIPT", and it is PARSER-INSERTED — §13.2.6.4.4 'The "in head" insertion
       mode' set this element's parser document when it created it, which is the fact steps 4 and 14 turn on and
       which decides whether a `<script src>` with no `async` attribute is a parser-blocking script or a member
       of the in-order ASAP list. HTML §14.2 "Parsing XML documents" states the same of ITS parser in the same
       breath as the end tag — "it must have its parser document set and its force async set to false" — so the
       XML caller passes true here for the standard's own reason and not by analogy with this one. */
    html_script_prepare(ctx, lxb_dom_interface_element(script), /*parser_inserted*/true, &imm);
    /* …AND STEP 36, WHICH THIS DOOR CANNOT YET PERFORM AND WHICH IS THEREFORE THE ONE PLACE LEFT WHERE A ROW
       STANDS IN FOR A NESTED RUN.
       WHAT IS NOT COVERED, EXACTLY: a `document.write`n inline classic script takes the slot AFTER the program
       that wrote it instead of running INSIDE the `document.write` call, so a write of a `script` element whose
       body calls `a()`, followed by a call to `b()`, runs `b()` before `a()` where a browser runs it after. The two DOM doors no longer do this — insert
       step 12 and §4.12.1.1's children changed steps both reach core/dom/element.c's tree-steps machine, which
       makes the request — and this one cannot join them because §13.2.6.4.8 'The "text" insertion mode' runs
       inside lexbor's tokenizer: a C loop with no step machine and no flow base under it, so there is nothing
       here to park.
       WHAT THE NEXT DIFF BUILDS: the parser-suspension capability core/html/html_parse.h already owes by name —
       §13.2.3.5's insertion point held across the run, the tokenizer stopped at the end tag and resumed at the
       program's completion. With it this call site becomes the same html_script_exec_run request the drain
       makes, and this row and engine_queue_script_immediate go together.
       HOW ITS ABSENCE SHOWS: a document that writes a `<script>` and then reads back what that script defined,
       in the SAME written chunk or in the writing program's next statement, sees it undefined — and §8.4.3's
       own permission ("user agents are explicitly allowed to avoid executing script elements inserted via this
       method") is why nothing else in this tree reports it. */
    if (imm.text) {
        engine_queue_script_immediate(document_doc(ctx), imm.text, imm.text_n, imm.el);
        html_script_immediate_free(&imm);
    }
    }
}

/* ---- HTML §4.12.1.1's STEP 36, AND THE "execute the script element" IT REACHES ------------------------------
 *
 * See html_script.h for why `prepare` reports this rather than performing it, and for what each field of the
 * report means. */
void html_script_immediate_free(ScriptImmediate *imm)
{
    DCHECK(imm != NULL, "a §4.12.1.1 step 36 report was released through a null record");
    free(imm->text);
    imm->text = NULL;
    imm->text_n = 0;
    imm->el = NULL;
}

/* WHICH STEP OF "execute the script element" THE STATE IS AT. Declared rather than numbered ad hoc, because a
   cross-session resume reads this byte and a private numbering says nothing about the algorithm. */
enum {
    SX_IDLE = 0,   /* nothing owed — the state a driving machine's buffer is built in and returns to */
    SX_RUN,        /* step 6's classic arm: the program is compiled, the bracket is open, the run is in flight */
    SX_REPORT      /* §8.1.4.4 "Calling scripts" step 8's third bullet, INSIDE the bracket step 4 will close */
};

void html_script_exec_init(ScriptExec *x)
{
    x->text = NULL;
    x->text_n = 0;
    x->el = NULL;
    x->stage = SX_IDLE;
    x->phase = 0;
    x->cb[0] = JS_UNDEFINED;
    x->cb[1] = JS_UNDEFINED;
    x->old = JS_UNDEFINED;
    x->exc = JS_UNDEFINED;
    report_exception_work_start(&x->rx);
}

bool html_script_exec_owed(const ScriptExec *x) { return x->stage != SX_IDLE; }

void html_script_exec_begin(ScriptExec *x, ScriptImmediate *imm)
{
    DCHECK(x->stage == SX_IDLE,
           "a second §4.12.1.1 step 36 run was begun on a state that is still executing one — a machine holds "
           "one nested program at a time, and adopting another would drop whichever of the two the state stops "
           "naming, with §3.1.7's currentScript left holding its element");
    DCHECK(imm->text != NULL && imm->el != NULL,
           "§4.12.1.1 step 36's run was begun on an EMPTY report — an empty record is the positive statement "
           "that this preparation owed no nested run, so beginning one means the caller did not look");
    x->text = imm->text;
    x->text_n = imm->text_n;
    x->el = imm->el;
    imm->text = NULL; imm->text_n = 0; imm->el = NULL;
    x->stage = SX_RUN;
}

void html_script_exec_visit(JSContext *ctx, ScriptExec *x, JSStepVisit *v)
{
    /* THE SOURCE TEXT IS NOT ON THIS LIST AND MUST NOT BE. It is a plain C allocation, and every visit
       operation this engine has copies through the JS allocator — so a fork would leave one buffer named by two
       arms and the second free would be a double free. It never has to be on the list, because the FIRST leg of
       the run below compiles it and releases it without returning: there is no rest point at which this state
       holds one. */
    DCHECK(x->text == NULL,
           "a §4.12.1.1 step 36 state was visited still holding its source text — the compile releases it in "
           "the same leg it reads it, so a buffer alive here has crossed a rest point that would hand one "
           "allocation to two forked arms");
    v->val(ctx, &x->cb[0]);
    v->val(ctx, &x->cb[1]);
    v->val(ctx, &x->old);
    v->val(ctx, &x->exc);
    report_exception_work_visit(ctx, &x->rx, v);
}

void html_script_exec_release(JSContext *ctx, ScriptExec *x)
{
    /* §8.1.4.6 step 6.1's ERROR REPORTING MODE, given back — a FLAG and not a reference, which is exactly what
       a `release` may touch and what no declaration can carry. Left set it would put the global in error
       reporting mode for the rest of the session and silently swallow every later report. */
    report_exception_work_unlock(ctx, &x->rx);
    DCHECK(x->text == NULL,
           "a §4.12.1.1 step 36 state was released still holding its source text — see the visit");
}

/* §4.12.1.1's "execute the script element" step 6's CLASSIC ARM STEP 4 — "set document's currentScript
   attribute to oldCurrentScript" — and the end of the algorithm. Reached from the normal completion and from
   the report of an abrupt one alike, because §8.1.4.4 "Calling scripts" step 8's third bullet is INSIDE "run
   the classic script", which is step 6's third sub-step: the report happens while the bracket is still open. */
static void script_exec_finish(JSContext *ctx, ScriptExec *x)
{
    document_current_script_restore(ctx, x->el, x->old);
    x->old = JS_UNDEFINED;
    x->el = NULL;
    x->stage = SX_IDLE;
}

int html_script_exec_run(JSContext *ctx, ScriptExec *x, JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    DCHECK(x->stage != SX_IDLE,
           "\"execute the script element\" was stepped with nothing to execute — html_script_exec_owed is the "
           "question, and a machine that steps this without asking it delivers a completion to an algorithm "
           "that made no request");
    DCHECK(x->el != NULL,
           "\"execute the script element\" is a switch on EL and this state has none — step 6's classic arm "
           "sets this document's §3.1.7 currentScript to it, so a run without one could not open its bracket");

    if (x->stage == SX_RUN) {
        JSValue out = JS_UNDEFINED;

        if (x->phase == 0) {
            JSValue prog, global;

            DCHECK(JS_IsUndefined(in),
                   "\"execute the script element\" was entered carrying a completion before it had asked for "
                   "anything — the compile below is its first act, so a value here is another algorithm's "
                   "answer routed into this one");
            JS_FreeValue(ctx, in);
            DCHECK(x->text != NULL, "step 36's run reached its compile with no source text");
            /* §4.12.1.1 STEP 34's CLASSIC ARM — "let script be the result of creating a classic script using
               source text, settings object, base URL, and options" — performed HERE rather than where the step
               is numbered, which is unobservable and is what lets the report cross from `prepare` as BYTES:
               nothing between step 34 and step 36 runs the page's code, and step 35's four arms are the ones
               this element did not take.
               JS_EVAL_FLAG_TRAMP_CLOSURE hands the program back AS A CLOSURE so its body runs on the calling
               flow's own trampoline chain — preemptible per opcode, parkable at any loop back-edge or `await`,
               forkable at a concolic branch inside it. Without it the body would run in its own activation off
               the chain, which is the drive-to-completion this engine aborts on, and a loop inside an injected
               script could not park for the scheduler at all. It is NOT a claim that this is an eval —
               quickjs.h says so at the flag, and the @S string-to-code seam keys on the eval TYPE (DIRECT /
               INDIRECT), which a `<script>`'s JS_EVAL_TYPE_GLOBAL program is neither of.
               THE NAME IS THE DOCUMENT'S ADDRESS, which is step 34's "let base URL be el's node document's
               document base URL" for a script with no `src` — the base a relative `import('./chunk.js')` inside
               it resolves against.
               JS_EVAL_FLAG_INLINE_SCRIPT for the reason solver/engine.c stamps it on an address-less
               page-script row: this program's source text arrived in the document rather than in a subresource
               bundle served identically to everybody. */
            {
                const char *base = document_base_url(ctx);

                DCHECK(base != NULL,
                       "HTML §4.12.1.1 step 34's base URL is \"el's node document's document base URL\" and this "
                       "document has none — HTML §2.4.3 \"Document base URLs\" ends its fallback base URL at "
                       "\"return document's URL\", so every Document has one, and an absent one is a document "
                       "built somewhere that never set its URL");
                prog = JS_Eval(ctx, x->text, x->text_n, base,
                               JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_TRAMP_CLOSURE | JS_EVAL_FLAG_INLINE_SCRIPT);
            }
            free(x->text);
            x->text = NULL;
            x->text_n = 0;
            /* STEP 6's CLASSIC ARM, STEPS 1-2 — "let oldCurrentScript be the value to which document's
               currentScript object was most recently set" and "if el's root is not a shadow root, then set
               document's currentScript attribute to el; otherwise, set it to null". OPENED BEFORE THE COMPILE'S
               ANSWER IS READ, because a parse error does not skip the arm: §4.12.1.1 step 4 returns only when
               el's RESULT IS NULL, and "creating a classic script" over source text that does not parse returns
               a script whose `error to rethrow` is set rather than null. So the throw is reported from INSIDE
               this bracket, exactly as an evaluation's would be.
               oldCurrentScript IS CARRIED AND NOT ASSERTED NULL, and step 36 is the case that made that
               necessary: the program that inserted this element is still live, with its own element in the
               slot. */
            x->old = document_current_script_set(ctx, x->el);
            if (!JS_IsException(prog)) {
                /* "RUN THE CLASSIC SCRIPT GIVEN BY EL'S RESULT" — the receiver is the GLOBAL OBJECT: a classic
                   script's program is global scope whatever its strictness, so a `"use strict"` prologue must
                   still see the global and not undefined. */
                global = JS_GetGlobalObject(ctx);
                r = step_call_run(ctx, &x->phase, x->cb, SCRIPT_EXEC_CB_SLOTS, prog, global, 0, NULL,
                                  JS_UNDEFINED, &out, out_cb, out_argc);
                JS_FreeValue(ctx, global);
                JS_FreeValue(ctx, prog);
                DCHECK(r == JS_STEP_CALL,
                       "§4.12.1.1 step 36's nested program answered without parking — the request is a CALL of "
                       "the page's own code and there is no arm of it that completes in place");
                return r;
            }
            /* §8.1.4.4 step 6 — "if script's error to rethrow is not null, then set evaluationStatus to
               ThrowCompletion(script's error to rethrow)" — and then step 8's third bullet below. */
            x->exc = JS_GetException(ctx);
            x->stage = SX_REPORT;
            in = JS_UNDEFINED;
        } else {
            r = step_call_run(ctx, &x->phase, x->cb, SCRIPT_EXEC_CB_SLOTS, JS_UNDEFINED, JS_UNDEFINED, 0, NULL,
                              in, &out, out_cb, out_argc);
            DCHECK(r == 0,
                   "§4.12.1.1 step 36's nested program re-parked on the same request — a call request has two "
                   "legs and the second one collects");
            in = JS_UNDEFINED;   /* step_call_run moved it into `out` */
            if (!JS_IsException(out)) {
                /* A `<script>`'s completion value is not observable to the page — only an eval API surfaces
                   one — so it is taken and released here rather than discarded by the engine. */
                JS_FreeValue(ctx, out);
                script_exec_finish(ctx, x);
                return 0;
            }
            x->exc = JS_GetException(ctx);
            x->stage = SX_REPORT;
        }
    }

    /* HTML §8.1.4.4 "Calling scripts" STEP 8's THIRD BULLET — "otherwise, rethrow errors is false. Perform the
       following steps: report an exception given by evaluationStatus.[[Value]] for script's settings object's
       global object; clean up after running script with settings; return evaluationStatus."
       IT IS `rethrow errors` FALSE HERE, AND THAT IS §4.12.1.1'S CHOICE RATHER THAN A DEFAULT: "execute the
       script element" invokes "run the classic script given by el's result" with no rethrow argument. A page's
       injected script that throws must therefore NOT complete the mutation that inserted it — `appendChild`
       does not throw what the injected program threw — and the `error` listeners this report fires are the
       page's own code, which is why it is a request and not a call. */
    DCHECK(x->stage == SX_REPORT, "\"execute the script element\" is standing in a stage it has no steps for");
    r = report_exception_run(ctx, &x->rx, x->exc, in, out_cb, out_argc);
    if (r) return r;
    JS_FreeValue(ctx, x->exc);
    x->exc = JS_UNDEFINED;
    script_exec_finish(ctx, x);
    return 0;
}

/* HTML §4.12.1.1 Processing model — see html_script.h, which is where this step is now stated once for every
   caller. It used to be three identical private copies, one per way a `<script src>` reaches a loader. */
char *script_src_absolute(JSContext *ctx, const char *src, size_t src_len)
{
    UrlRecord base, rec;
    const char *base_url = document_base_url(ctx);
    bool have_base;
    char *abs_url = NULL;

    url_record_init(&base);
    have_base = base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    url_record_init(&rec);
    if (url_parse(&rec, src, src_len, have_base ? &base : NULL))
        abs_url = url_serialize(&rec, false);
    url_record_free(&rec);
    url_record_free(&base);
    return abs_url;
}
