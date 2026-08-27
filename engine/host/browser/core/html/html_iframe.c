/* HTMLIFrameElement's NAVIGABLE — HTML §4.8.5, and the element half of the cross-document machinery.
 *
 * §4.8.5's insertion steps CREATE A CHILD NAVIGABLE when an `<iframe>` is inserted into a document, and they do
 * it THERE — synchronously, in the same turn as the append. `frame.contentWindow` answers on the very next line
 * in every browser, and the spec files say so directly: `const otherW = document.body.appendChild(frame)
 * .contentWindow` then reads `otherW.self`. It is not lazily on the first `contentWindow` either, which is
 * observably different — a page may insert a frame and read `window.length` without ever touching it.
 *
 * IT WAS A QUEUED TASK, AND THAT WAS A WORKAROUND FOR AN IDENTITY PROBLEM, NOT A SCHEDULING ONE. Creating the
 * child meant asking the host to mint its document id, asking suspends, and the insertion-steps drain may not
 * suspend — so the work was pushed into a task that could. The fix is not a better place to suspend: it is not
 * to suspend. A document is NAMED and a child's name is minted locally (world.h), so creation is a mint, a
 * notice and an object allocation, none of which can block. The task is gone and so is the ordering lie its
 * comment recorded.
 *
 * THE NAVIGABLE IS PER-FLOW, and it is kept on the ELEMENT'S WRAPPER rather than in a table beside it. A flow
 * that inserted the frame has one; a sibling that never did must not see it, and a C-side registry would show
 * it to both — silently, because a proxy that exists in the wrong world answers reads perfectly well. A hidden
 * own slot on the wrapper is an ordinary property write on a baseline object, so the heap COW delta isolates it
 * with nothing added here; a forked arm inherits its parent's through the delta and issues no second create,
 * which is also what keeps one child document per WORLD rather than one per flow. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/html/html_iframe.h"
#include "core/dom/element.h"
#include "core/dom/node.h"
#include "core/dom/document.h"
#include "core/events/event.h"
#include "core/events/event_target.h"
#include "core/frame/document_lifecycle.h"
#include "core/frame/navigable.h"
#include "core/frame/sandboxing.h"
#include "core/frame/window_proxy.h"
#include "core/url/url.h"   /* §4.8.5's shared attribute processing steps ask whether a url MATCHES about:blank */
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/world.h"
#include "core/idl_args.h"

/* The wrapper slot the navigable lives in. Not a name a page would write, and read through JS_GetOwnSlot so no
   prototype lookup and no page code can intercept it — the same arrangement the custom-element upgrade mark
   uses, and for the same reason. */
static JSAtom g_atom_navigable = JS_ATOM_NULL;

/* See html_iframe.h. The name is read off the node rather than off a wrapper because two of the askers are
   inside tree walks that have a node and would have to mint a wrapper to ask. */
bool iframe_element_is(const lxb_dom_node_t *n)
{
    size_t qn = 0;
    const lxb_char_t *q;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    q = lxb_dom_element_qualified_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &qn);
    return q && qn == 6 && !strncasecmp((const char *)q, "iframe", 6);
}

/* This element's navigable IN THIS FLOW, or JS_UNDEFINED. */
JSValue iframe_navigable(JSContext *ctx, JSValueConst wrap)
{
    JSValue v;
    if (JS_GetOwnSlot(ctx, &v, wrap, g_atom_navigable) <= 0) return JS_UNDEFINED;
    return v;
}

bool iframe_has_navigable(JSContext *ctx, JSValueConst wrap)
{
    JSValue v = iframe_navigable(ctx, wrap);
    bool had = !JS_IsUndefined(v);
    JS_FreeValue(ctx, v);
    return had;
}

void iframe_create_navigable(JSContext *ctx, JSValueConst wrap)
{
    char *src, *name, *sandbox;
    SandboxFlags iframe_flags;
    JSValue proxy;

    DCHECK(g_atom_navigable != JS_ATOM_NULL, "an iframe's navigable was created before iframe_init ran");
    DCHECK(JS_IsObject(wrap), "something that is not an element wrapper was given a child navigable");
    if (iframe_has_navigable(ctx, wrap)) return;   /* this flow already has one */

    /* §4.8.5's "process the iframe attributes": `src` is the child's initial address, and an absent or empty
       one is the initial about:blank Document. `name` becomes the BROWSING CONTEXT's name — which is why
       renaming the window later leaves the attribute alone, and why removing the frame empties the name while
       the attribute keeps its value. Both are read through the DOM chokepoint so the read stays in the running
       flow's delta — a flow that set `src` and a sibling that did not create different children, which is the
       whole reason the navigable is per-flow. */
    src  = element_attr_get(ctx, wrap, "src");
    name = element_attr_get(ctx, wrap, "name");
    /* §7.1.5's IFRAME SANDBOXING FLAG SET: "every iframe element has an iframe sandboxing flag set … which
       flags in it are set at any particular time is determined by the iframe element's sandbox attribute."
       AN ABSENT ATTRIBUTE IS AN EMPTY SET AND `sandbox=""` IS NEARLY THE WHOLE SET, which is why the presence
       of the attribute and its value are two different questions and only the read can tell them apart —
       element_attr_get answers NULL for the first and "" for the second, and collapsing them would make
       `<iframe sandbox>` (the most restrictive form there is) mean nothing at all.
       READ THROUGH THE DOM CHOKEPOINT like `src` and `name` beside it, so the value is the RUNNING FLOW's:
       an arm that wrote `sandbox` and an arm that did not create two children with two flag sets, which is
       the whole of how §7.1.5's set becomes per-flow without anything having to capture the set itself. */
    sandbox = element_attr_get(ctx, wrap, "sandbox");
    iframe_flags = sandbox ? sandbox_parse_directive(sandbox, strlen(sandbox)) : 0;
    /* §7.3.1.3 "Child navigables": "To create a new child navigable, given an element element" — THIS element,
       handed over rather than looked up, because the container link the create makes is a link back to it and
       §7.2.2.4's `frameElement` is the read that follows it. The slot written below is the same link's other
       half; they are one step of one algorithm and both are written here. */
    proxy = navigable_create(ctx, src, name, true, NULL, iframe_flags, wrap);
    /* §4.8.5 has no "did not parse" branch the way §7.4 does: an `<iframe src="::">` still has a navigable,
       holding the initial about:blank it was created with. */
    if (JS_IsUndefined(proxy)) proxy = navigable_create(ctx, NULL, name, true, NULL, iframe_flags, wrap);
    free(src);
    free(name);
    free(sandbox);
    DCHECK(!JS_IsUndefined(proxy), "an about:blank child navigable could not be created, which has no failing "
                                   "branch — its address needs no parse and its origin is this document's");
    /* WRITABLE, NOT CONFIGURABLE. The removing steps below CLEAR this slot, and a slot defined with no flags
       at all can be neither rewritten nor deleted — the destroy silently did nothing and a removed frame kept
       answering `contentWindow`. Non-configurable is deliberate: a page that guessed the name still cannot
       delete the navigable out from under the element. */
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_navigable, proxy, JS_PROP_WRITABLE);
}

/* HTML §4.8.5 "The iframe element" — the SHARED ATTRIBUTE PROCESSING STEPS FOR IFRAME AND FRAME ELEMENTS,
 * asked for the ONE thing the branch below needs of their result: does the url they produce MATCH about:blank.
 *
 *   1. Let url be the URL record about:blank.
 *   2. If element has a src attribute specified, and its value is not the empty string: let maybeURL be the
 *      result of encoding-parsing a URL given that attribute's value, relative to element's node document. If
 *      maybeURL is not failure, then set url to maybeURL.
 *
 * BOTH SPELLINGS OF "NO ADDRESS" ANSWER TRUE AND THEY ARE DIFFERENT FACTS THE READ TELLS APART: an ABSENT
 * attribute and `src=""` are step 2's two disqualifiers, and element_attr_get answers NULL for the first and
 * "" for the second. A PARSE FAILURE also answers true, and that is step 2's own wording rather than a
 * fallback — "if maybeURL is not failure, then set url" leaves url at step 1's about:blank, which is why
 * `<iframe src="::">` has a navigable holding the initial about:blank and gets this load event.
 * THE QUERY IS NOT PART OF THE RELATION. URL's "matches about:blank" tests the scheme, the opaque path, the
 * username, the password and a null host and says nothing about the query — which is precisely why the shared
 * steps carry a further step for `about:blank?foo` (perform the URL and history update steps), and that step
 * belongs to the SRC path this engine does not have yet; see the assert in iframe_process_attributes.
 *
 * THE BASE IS "element's NODE DOCUMENT"'s, which is the realm the caller is standing in: core/dom/element.c's
 * tree-steps drain re-points `ctx` at document_realm_of(n) before running §4.2.3's steps, exactly so that a
 * `frame.contentDocument.body.appendChild(subframe)` resolves the subframe's `src` against the document it was
 * inserted into and not the one that performed the write.
 *
 * STEP 3, THE SELF-NESTING GUARD, IS NOT HERE AND ITS ABSENCE IS NOT A HOLE IN THIS BRANCH: it returns null so
 * that the OTHERWISE branch does not navigate, and a url that matches about:blank is never the url of an
 * ancestor navigable's active document in the first place. It belongs with the navigate it guards, which
 * §7.3.1.3's create already performed by the time this runs (see iframe_process_attributes). */
static bool iframe_src_matches_about_blank(JSContext *ctx, JSValueConst wrap)
{
    char *src = element_attr_get(ctx, wrap, "src");
    UrlRecord base, rec;
    const char *base_url;
    bool have_base, matches;

    if (!src || !*src) { free(src); return true; }                                            /* STEP 1 */
    base_url = document_base_url(ctx);
    url_record_init(&base);
    have_base = base_url && *base_url && url_parse(&base, base_url, strlen(base_url), NULL);
    url_record_init(&rec);
    matches = url_parse(&rec, src, strlen(src), have_base ? &base : NULL)                     /* STEP 2 */
              ? url_matches_about(&rec, "blank", /*query_must_be_null*/ false)
              : true;
    url_record_free(&rec);
    url_record_free(&base);
    free(src);
    return matches;
}

/* HTML §4.8.5's PROCESS THE IFRAME ATTRIBUTES — see html_iframe.h for why this exists and what depends on it.
 *
 * THE OTHERWISE BRANCH'S NAVIGATE ALREADY RAN, AND THAT IS A FACT ABOUT THIS ENGINE'S STRUCTURE RATHER THAN A
 * STEP SKIPPED HERE. The standard splits §4.8.5's post-connection steps in two — step 2 CREATES the navigable
 * holding the initial about:blank and step 3 NAVIGATES it — while navigable_create does both at once (its own
 * `if (strncmp(addr, "about:", 6) != 0)` is where §4.8.5's navigate is enqueued from). So the only branch of
 * this algorithm left to run at this seam is the about:blank one, and calling the navigate here as well would
 * be the DOUBLE LOAD navigable_create carries a paragraph about. Undoing the fold is what gives the shared
 * steps' step 3 and an `iframe.src` write somewhere to act; until then it is one algorithm reached through
 * two functions, and this is the half the other one cannot reach. */
void iframe_process_attributes(JSContext *ctx, JSValueConst wrap, bool initial_insertion)
{
    DCHECK(initial_insertion,
           "§4.8.5's process the iframe attributes was asked with initialInsertion FALSE, and the only caller "
           "that passes false is the `src` ATTRIBUTE CHANGE STEPS — core/dom/element.c's element_attr_changed "
           "has no `<iframe>` entry, so writing `frame.src` on a connected frame re-navigates nothing in this "
           "engine. Build that entry (it runs these steps with initialInsertion false, which reaches §4.8.5's "
           "navigate an iframe or frame), and this assert is what tells you this branch is then reachable");
    /* §4.8.5's own step 1 for the load event steps below is "Assert: element's content navigable is not null",
       and this algorithm is reached only from the post-connection steps that created it one line earlier — so
       an element without one here means those two steps have come apart. */
    DCHECK(iframe_has_navigable(ctx, wrap),
           "§4.8.5's process the iframe attributes ran for an `<iframe>` with no content navigable in this "
           "flow — the post-connection steps create it in the step directly before this one, so the two halves "
           "of that pair are being called separately");
    /* "If url matches about:blank and initialInsertion is true: Run the iframe load event steps given
       element. Return." The RETURN is the rest of this function. */
    if (!iframe_src_matches_about_blank(ctx, wrap)) return;
    iframe_run_load_event_steps(ctx, wrap);
}

/* §4.8.5's REMOVING STEPS: an <iframe> that leaves a document runs §7.3.1's DESTROY A CHILD NAVIGABLE over the
   navigable it contained. The container's half is SYNCHRONOUS and the document's half is a JOB, which is the
   spec's own split and is why this used to be wrong: the element loses its navigable on this line (step 3, so
   `contentWindow` is null from here on), while step 5's destruction of the active document and everything under
   it is queued — it disentangles that document's ports, drops its queued tasks and only then nulls its browsing
   context, none of which can happen inside a tree mutation.
   WHAT WAS HERE INSTEAD WAS ONE BYTE. Setting `closed` on the proxy announced a destruction that had not
   happened and never would: the child's Document, its Window, its realm, its queued tasks and its whole
   subtree were left exactly as they were, and the announcement is what made that invisible. The proxy a page
   is still holding does stay the same object and does end up reporting `closed` — a WindowProxy outlives the
   navigable it named — but it reports it because the destruction ran, not instead of it. */
void iframe_destroy_navigable(JSContext *ctx, JSValueConst wrap)
{
    JSValue proxy = iframe_navigable(ctx, wrap);

    if (JS_IsUndefined(proxy)) return;   /* this flow never had one — §7.3.1 step 2 */
    document_lifecycle_destroy_child(ctx, proxy);   /* §7.3.1 steps 4-5 */
    JS_FreeValue(ctx, proxy);
    /* CLEARED, not deleted: the slot is non-configurable so it cannot be deleted, and it does not need to be —
       an empty slot is what "this element has no navigable" means everywhere it is read. It is an ordinary
       property write on the wrapper, so the heap COW delta isolates it: a sibling arm that never removed the
       element still sees the frame it knew. */
    JS_DefinePropertyValue(ctx, (JSValue)wrap, g_atom_navigable, JS_UNDEFINED, JS_PROP_WRITABLE);
}

/* HTML §4.8.5's IFRAME LOAD EVENT STEPS. Seven steps in the standard; five of them are one pair of flags and a
 * resource-timing entry, and what is here is the two that this engine can state:
 *
 *   1. Assert: element's content navigable is not null.
 *   2. Let childDocument be element's content navigable's active document.
 *   3. If childDocument has its MUTE IFRAME LOAD flag set, then return.
 *   4. [resource timing]
 *   5. Set childDocument's IFRAME LOAD IN PROGRESS flag.
 *   6. Fire an event named `load` at element.
 *   7. Unset childDocument's iframe load in progress flag.
 *
 * THE TWO FLAGS ARE ONE PAIR WITH §8.4.1's DOCUMENT OPEN STEPS AND LAND WITH THEM, not before. They form a
 * closed loop with exactly one participant outside this algorithm: §8.4.1's "opening the input stream" is the
 * only reader of `iframe load in progress` and the only writer of `mute iframe load`, and this algorithm is the
 * only writer of the first and the only reader of the second. So building either one here alone produces
 * precisely the two defects CLAUDE.md names as a matched pair — a field written by this engine and read by
 * nothing, and a field read by this engine and written by nothing, the second of which a page cannot tell from
 * a flag that is simply always clear. `document.open()` is what closes the loop, and it closes both halves in
 * the same diff or neither. Until then step 3 cannot be reached with a set flag by any program, so its absence
 * changes no answer; what would change an answer is a `false` invented for it here.
 *
 * IT IS A QUEUED FIRE, WHICH IS §7.5.8's OWN WORD AND IS NOT §4.8.5's. §7.5.8 queues an ELEMENT TASK on the
 * DOM manipulation task source, so for a Document a navigation produced the dispatch belongs to the event loop
 * and not to the line that finished the load — event_target_fire is that enqueue, and the listener body is
 * then an ordinary call-root flow, preemptible and parkable like every other program, which a C activation
 * could not host. §4.8.5's initial-insertion caller reaches the same enqueue, and WHETHER THAT IS RIGHT IS A
 * QUESTION THE STANDARD AND THE CORPUS ANSWER DIFFERENTLY, so it is recorded rather than decided here: §7.5.8's
 * note calls this fire "a SYNCHRONOUS load event", while html/browsers/the-window-object/
 * window-reuse-in-nested-browsing-contexts.tentative.html asserts in its own comment that "the task to perform
 * the iframe load event steps should still be QUEUED" and FAILS a browser that has already fired by the next
 * line — and that file says of itself that it is outdated. The two differ in exactly one program, a listener
 * registered on the line AFTER the append, and the way to settle it is to run that file rather than to pick a
 * sentence. If it turns out to be the note, the cost is the flow base: a synchronous dispatch from inside the
 * tree-steps drain would run a page's listener under a C activation, which is the one thing this seam may not
 * do, so the drain itself would have to deliver it at the node before it yields.
 *
 * `load` AT AN ELEMENT DOES NOT BUBBLE. DOM's "fire an event named e at target" with no initialisation is
 * bubbles false and cancelable false, and this is the one `load` in the engine that is not the Window's — the
 * Window's is §13.2.7 step 9.5 and carries a legacy target override this one has no business with. */
void iframe_run_load_event_steps(JSContext *ctx, JSValueConst wrap)
{
    /* STEP 1, and it is an assert in the standard's own word. Either caller reached this element through a
       relation that names the navigable: §7.5.8 asked a NAVIGABLE for its container, and §4.8.5's process the
       iframe attributes ran one step after the create. So an element that answers as a container while holding
       no content navigable means the two halves of §7.3.1.3's relation disagree, which is the one state
       window_proxy_container's read-the-forward-slot-back construction exists to make impossible. */
    DCHECK(iframe_has_navigable(ctx, wrap),
           "§4.8.5's iframe load event steps step 1 asserts the element's content navigable is not null, and "
           "this one has none in the running flow — §7.5.8 named this element as its document's navigable's "
           "container, so the container link and the content-navigable slot are naming each other in only one "
           "direction");
    event_target_fire(ctx, wrap, event_new(ctx, "load", /*bubbles*/ false, /*cancelable*/ false),
                      JS_UNDEFINED);                                                              /* STEP 6 */
}

/* §4.8.5 FOR THE ELEMENTS THE PARSER INSERTED. A browser runs the insertion steps during tree construction, so
 * an `<iframe>` in the page's own markup has a navigable before the first script runs — `window.length` is 1 on
 * a document that never scripted anything. This engine's tree comes from a Lexbor parse that does not pass
 * through the DOM chokepoint, so the parsed tree's iframes get their step here, once, when the document is
 * installed. Everything a script appends afterwards goes through the chokepoint and needs nothing from this. */
void iframe_document_parsed(JSContext *ctx)
{
    lxb_dom_node_t *root = document_root_node(ctx), *n = root;

    while (n) {
        if (iframe_element_is(n)) {
            JSValue w = node_wrap(ctx, n);
            iframe_create_navigable(ctx, w);
            /* §4.8.5's POST-CONNECTION STEP 3, which belongs to every entry that runs step 2 and not only to
               the chokepoint's. A `<iframe>` the PARSER inserted is one a browser ran the whole of these steps
               for during tree construction, so a page whose markup carries a srcless frame has already had its
               `load` by the time its first script runs — asking this question at one of the two entries and
               not the other is one capability wearing two names, and the entry that skipped it would report a
               frame that never loads while the other reports one that does. */
            iframe_process_attributes(ctx, w, /*initialInsertion*/ true);
            JS_FreeValue(ctx, w);
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
}

/* §7.2.2.2's DOCUMENT-TREE CHILD NAVIGABLES, in TREE ORDER — the set `window.length` counts and `window[i]`
 * indexes. It is a WALK, never a counter kept beside the tree: the set changes on every insertion, every
 * removal and every reparent, and a page reads `length` after doing all three. A count that a mutation forgot
 * to adjust is wrong in exactly the case the spec files test.
 *
 * IT IS PER-FLOW TWICE OVER, and both halves come for free. The TREE is per-flow (the DOM COW delta), so a
 * flow that appended a frame walks a document containing it and its sibling does not; and the NAVIGABLE is
 * per-flow (the wrapper slot), so an element that is in both flows' trees still only counts for the flow that
 * gave it one. That is why this asks iframe_has_navigable rather than counting <iframe> ELEMENTS: an element
 * whose navigable was destroyed is still in the tree until it is removed, and it is not a child navigable.
 *
 * `want` < 0 counts them all and returns JS_UNDEFINED; otherwise the nth is returned, or JS_UNDEFINED. */
static JSValue child_navigables(JSContext *ctx, int want, int *out_n)
{
    lxb_dom_node_t *root = document_root_node(ctx);
    lxb_dom_node_t *n = root;
    int seen = 0;

    if (out_n) *out_n = 0;
    while (n) {
        if (iframe_element_is(n)) {
            JSValue w = node_wrap(ctx, n);
            JSValue nav = iframe_navigable(ctx, w);
            JS_FreeValue(ctx, w);
            if (!JS_IsUndefined(nav)) {
                if (want == seen) { if (out_n) *out_n = seen + 1; return nav; }
                seen++;
            }
            JS_FreeValue(ctx, nav);
        }
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        n = n ? n->next : NULL;
    }
    if (out_n) *out_n = seen;
    return JS_UNDEFINED;
}

int iframe_child_navigable_count(JSContext *ctx)
{
    int n = 0;
    JSValue v = child_navigables(ctx, -1, &n);
    JS_FreeValue(ctx, v);
    return n;
}

JSValue iframe_child_navigable(JSContext *ctx, int index)
{
    return index < 0 ? JS_UNDEFINED : child_navigables(ctx, index, NULL);
}

/* §4.8.5 `contentDocument`, and the read that made the cross-agent reference necessary. It is the child
 * navigable's ACTIVE DOCUMENT, filtered by §7.2.1's same-origin check — which is the whole of the spec's
 * text, because a browser has the two documents in one agent cluster and the answer is a pointer.
 *
 * HERE IT IS IN ANOTHER INSTANCE, so the flow SUSPENDS: the peer exports its `document` and answers with the
 * NAME of it, and this side resolves that name to the one reference for it. `contentDocument` is therefore a
 * step machine where `contentWindow` is a plain accessor — a WindowProxy names a NAVIGABLE, which this agent
 * created and knows, and a Document is the peer's object. */
/* WHERE THIS MACHINE RESTS, AS THE STANDARD NUMBERS IT. §4.8.5's getter is one sentence over §7.3.1's
   `content document`, whose four steps are: no content navigable → null; its active document; §7.2.1's same
   origin-domain filter; return it. Steps 1-3 are one stage — they are decided here, and no page code runs
   between them. Step 4 is its OWN stage whenever the document lives in another instance, because that answer
   arrives from a peer's scheduled turn and this flow is parked until it does; the resume point was a request
   handle being non-zero, which is a stage nothing could name.
   Same-origin in THIS agent the two documents are one heap, so step 4 is answered in the same turn and the
   machine never rests at the second stage — a suspend there would be observable, which makes it a fidelity bug
   rather than extra rigor. */
#define CONTENT_DOC_STAGES(X) \
    X(CONTENTDOC_RESOLVE, "HTML §4.8.5 contentDocument → §7.3.1 content document steps 1-3 (the content " \
                          "navigable's active document, filtered by §7.2.1's same origin-domain check)") \
    X(CONTENTDOC_ANSWER,  "HTML §7.3.1 content document step 4 (the answer, from the instance that holds the " \
                          "document)")
enum { IDL_STEP_STAGE_BASE(CONTENT_DOC_STAGES) CONTENT_DOC_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const CONTENT_DOC_STEPS[] = { CONTENT_DOC_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct { uint32_t req; } ContentDocState;

static void iframe_cd_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

static int iframe_content_document_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                        JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ContentDocState *s = st;
    JSValueConst answer;

    (void)argc; (void)argv; (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (hdr->stage == CONTENTDOC_RESOLVE) {
        JSValue nav = iframe_navigable(ctx, hdr->this_val);
        Flow *f = flow_running();
        char op[1024];
        int n;

        /* §4.8.5: no navigable, no document. */
        if (JS_IsUndefined(nav)) { *presult = JS_NULL; return JS_STEP_DONE; }
        /* §7.3.1.3 "Child navigables" STEP 2 CAN BIND NULL, and that is a DIFFERENT null from step 1's. Step 1
           is "container's content navigable is null" — §7.3.1.6's destroy-a-child-navigable clearing the slot,
           which is the line above. Step 2 is "let document be container's content navigable's ACTIVE DOCUMENT",
           and §7.5.10 step 9 nulls that for every document in a destroyed SUBTREE while leaving each inner
           container's slot exactly as it was: destroy-a-document-and-its-descendants destroys documents, and
           only the container the page removed ever has its content navigable cleared. So a page holding an
           `<iframe>` from inside a removed subtree reaches here with a navigable that has no active document,
           and step 4 returns that null. Without this the read would build one — a Document for a browsing
           context that is null — which is what window_proxy.c's proxy_realm now refuses outright. */
        if (window_proxy_destroyed(nav)) {
            JS_FreeValue(ctx, nav);
            *presult = JS_NULL;
            return JS_STEP_DONE;
        }
        /* §7.3.1's content document step 3 filters BEFORE asking: a cross-origin child's document is null, and
           asking the peer for it would both leak and suspend a flow on a question whose answer is already
           known. THE FILTER IS SAME ORIGIN-DOMAIN, which is §7.1.1's other algorithm and the one this step
           names. It is NOT the same answer as same origin: a child that has run §7.1.1.2's `document.domain`
           setter while this container has not is same origin with it and NOT same origin-domain, which is the
           standard's own fourth table row, and this line is what turns that into a null. */
        if (!window_proxy_same_origin_domain_of(ctx, nav)) {
            JS_FreeValue(ctx, nav);
            *presult = JS_NULL;
            return JS_STEP_DONE;
        }
        /* SAME-ORIGIN AND IN THIS AGENT: one heap, so the answer is the child realm's own `document` object and
           it is handed back in THIS turn. §4.8.5 is synchronous here — `frame.contentDocument.body` reads on
           the line after the append — so a suspend would be observable, which makes it a fidelity bug rather
           than extra rigor. It is also what the corpus does with the result: window_length.html appends a node
           THIS document created into that body, and afterwards `subframe.parentNode` is a node of the other
           document while the wrapper stays the same object. There is no message that carries that. */
        if (!window_proxy_is_remote(nav)) {
            JSContext *cctx = window_proxy_realm(ctx, nav);
            *presult = JS_DupValue(ctx, document_object(cctx));
            JS_FreeValue(ctx, nav);
            return JS_STEP_DONE;
        }
        DCHECK(f != NULL, "a cross-document read was issued outside a flow — there would be nothing to suspend");
        /* THE WORLD VECTOR IS world_serialize'S AND NOBODY ELSE'S (solver/world.h), and this site was the
           second spelling of it — the head written by hand, then a hand-rolled loop over world_ancestry. Two
           spellings are two peers materializing different segments for one flow, which is the reason that
           function exists; and this one ALSO had the exact failure its CHECK is there to prevent, twice over.
           `snprintf` returns the length it WOULD have written, so once the record filled `op` the accumulated
           `n` ran PAST `sizeof op` and `sizeof op - (size_t)n` UNDERFLOWED to a huge size_t — a write past the
           end of a stack buffer, reached by nothing louder than a deep enough frame tree or a long enough fork
           chain. The quiet half is the loop guard `n < (int)sizeof op`, which permitted a truncated ancestry
           to be SENT: a prefix makes the peer fork a more distant ancestor than the sender named and silently
           lose every write in between, which is world.h's stated reason for crashing rather than sending one. */
        n = snprintf(op, sizeof op, "windowproxy.get\t%s\t", world_doc_name(window_proxy_doc(nav)));
        CHECK(n > 0 && (size_t)n < sizeof op,
              "a cross-document read's target document name did not fit its record — a truncated name reaches "
              "no instance, and the asking flow parks on a question nothing will ever be asked");
        n += world_serialize(f->world, op + n, sizeof op - (size_t)n);
        n += snprintf(op + n, sizeof op - (size_t)n, "\tdocument");
        CHECK((size_t)n < sizeof op,
              "a cross-document read's member name did not fit its record — the peer would run a program for a "
              "TRUNCATED member, answering a different question as if it were this one");
        JS_FreeValue(ctx, nav);
        s->req = engine_host_request(ctx, op);
        hdr->stage = CONTENTDOC_ANSWER;
        return JS_STEP_YIELD;
    }
    DCHECK(hdr->stage == CONTENTDOC_ANSWER, "contentDocument resumed into a stage §7.3.1 does not have");
    DCHECK(s->req != 0, "contentDocument is parked on step 4 with no request outstanding — the stage says a "
                        "peer was asked and nothing was");
    if (!engine_host_answered(s->req, &answer))
        return JS_STEP_YIELD;
    /* The peer answered with a COMPLETION: §7.3.1's read runs the peer's own program, and a throw from it is
       raised here, at the `iframe.contentDocument` that parked. */
    {
        int r = engine_host_take_completion(ctx, s->req, presult);
        s->req = 0;
        return r;
    }
}

static const IdlStepDecl CONTENT_DOC_DECL = { iframe_content_document_step, sizeof(ContentDocState),
                                              iframe_cd_visit, NULL,
                                              "HTML §4.8.5 HTMLIFrameElement.contentDocument "
                                              "(over §7.3.1's content document)", CONTENT_DOC_STEPS };

/* §4.8.5 `contentWindow`: this flow's child navigable, or null when there is none. Reading THROUGH it is what
   suspends; this read does not, because the proxy is a local object naming a remote document. */
static JSValue js_iframe_content_window(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue v = iframe_navigable(ctx, this_val);
    (void)magic;
    if (JS_IsUndefined(v)) return JS_NULL;
    return v;
}

void iframe_init(JSContext *ctx)
{
    DCHECK(g_atom_navigable == JS_ATOM_NULL, "iframe_init ran twice — one instance is one document");
    g_atom_navigable = JS_NewAtom(ctx, "apiclientNavigable");
    CHECK(g_atom_navigable != JS_ATOM_NULL, "the iframe navigable slot could not be interned");
    agent_state_atom("html_iframe", &g_atom_navigable,
                     "§4.8.5's navigable slot name on an iframe's wrapper, and the declaration latch");
}

/* Declared once per AGENT, installed per realm — see hyperlink.c for why the split exists at all. */
static int g_content_doc_id = -1;

void iframe_declare(JSContext *ctx)
{
    g_content_doc_id = idl_getter_id_step(ctx, &CONTENT_DOC_DECL, 0);
}

void iframe_install(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_content_doc_id >= 0, "§4.8.5's members were installed before they were declared");
    idl_install_accessor(ctx, proto, "contentWindow", js_iframe_content_window, 0, -1);
    idl_install_accessor_step(ctx, proto, "contentDocument", g_content_doc_id, -1);
}

void iframe_free(JSRuntime *rt)
{
    JS_FreeAtomRT(rt, g_atom_navigable);
    g_atom_navigable = JS_ATOM_NULL;
}
