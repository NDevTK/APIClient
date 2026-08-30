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
#include "core/dom/document.h"   /* which DOCUMENT this program belongs to: the realm it is compiled in */
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

/* CONFIGURABLE AND WRITABLE for the reason custom_elements.c's slots are: the flag is written more than once
   over one element's life — the parse marks it, and §4.12.1.1 "Processing model"'s cloning steps write the
   copy's from the original's — and a slot defined with no flags makes the second write a silent no-op. */
#define SCRIPT_SLOT_FLAGS (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)

/* §4.12.1's `async` SETTER STEPS, declared here because the declaration is the agent's and the init below is
   where an agent's one-per-runtime state is minted; the steps themselves are beside the getter. */
static JSValue js_script_set_async(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic);

static void script_children_changed(JSContext *ctx, lxb_dom_node_t *parent);

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
static void script_post_connection(JSContext *ctx, lxb_dom_element_t *el)
{
    /* "The HTML element post-connection steps only run when the inserted element is still CONNECTED" — a
       script mutated while detached prepares when it is inserted, through the insertion half, and preparing it
       here as well would run one element's code twice. */
    if (!node_is_connected(lxb_dom_interface_node(el))) return;
    /* NOT PARSER-INSERTED, and step 1 of these very steps is why: "If insertedNode is parser-inserted, then
       return" — so an element that reaches the post-connection steps at all has a null parser document. */
    html_script_prepare(ctx, el, /*parser_inserted*/false);
}

/* §4.12.1.1: "The script CHILDREN CHANGED STEPS given changedNode are: 1. If the script element is not
   connected, then return. 2. Run the script HTML element post-connection steps, given changedNode."
   The hook is handed the PARENT whose child list changed, which for this family IS changedNode — the script
   element whose text was written. */
static void script_children_changed(JSContext *ctx, lxb_dom_node_t *parent)
{
    if (!html_script_is(parent)) return;
    script_post_connection(ctx, lxb_dom_interface_element(parent));
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
        if (val) script_post_connection(ctx, el);
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
void html_script_prepare(JSContext *ctx, lxb_dom_element_t *el, bool parser_inserted)
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
       THE LENGTH IS THE DOM's AND NOT A `strlen` — see the queue call below for why this element's text is the
       one inline source that can hold a U+0000. */
    has_src = lxb_dom_element_has_attribute(el, (const lxb_char_t *)"src", 3);
    {
        lxb_char_t *probe = has_src ? NULL : lxb_dom_node_text_content(n, &n_len);

        if (!has_src) {
            bool empty = probe == NULL || n_len == 0;

            if (probe) lxb_dom_document_destroy_text(n->owner_document, probe);
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
        if (!script_type_executes(st)) return;
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
    /* An UNKNOWN src is a URL this engine cannot fetch, but it is still a request the page makes — recorded so
       it reaches the @H surface as the shape it is, rather than disappearing. */
    t = dom_cow_attr_taint(el, "src");
    if (!JS_IsUndefined(t)) {
        endpoint_record(ctx, "GET", t, NULL, 0, NULL);
        return;
    }
    /* §4.12.1's `src` BRANCH IS ENTERED ON THE ATTRIBUTE, which is the same correction the document scan needed
       and for the same reason: `get_attribute` answers NULL for an attribute whose value is absent, so a
       presence test written over the VALUE let `<script src="">` fall through to the child-text branch and RUN
       it — markup a browser runs nothing for. The standard's second step is `src` being the empty string:
       "queue an element task … to fire an event named error at el, and return". What is still owed is that
       error event, which needs a task on this element's document rather than anything here. */
    if (has_src) {
        char *u;

        src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &n_len);
        if (!src || !n_len) return;
        /* "ENCODING-PARSING A URL GIVEN src, RELATIVE TO EL'S NODE DOCUMENT" — §4.12.1's own step, and the
           realm this chokepoint was entered with IS that document (core/dom/element.c hands the inserted node's
           document, not the mutating one). It was missing: the raw ATTRIBUTE went to the host, so an injected
           `<script src="./chunk.js">` named an address only the host's own base could resolve — and the host
           that has one is a different origin from the page. NULL is the standard's branch for a `src` that does
           not parse — "return", so the element runs no script. */
        u = script_src_absolute(ctx, (const char *)src, n_len);
        if (!u) return;
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
        lxb_char_t *txt = lxb_dom_node_text_content(n, &n_len);
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
                    /* AND THIS IS THE ONE ROW WHOSE POSITION IS A DEVIATION RATHER THAN A MODEL OF ONE, so it
                       crashes here instead of being described. Every OTHER destination §4.12.1 has is a
                       POSITION IN A SEQUENCE — a list, a set, a pending slot — and a row expresses each of them
                       exactly. `immediately execute` is not a position in any sequence: it is a nested run
                       INSIDE the operation that reached these steps, and the flow's one program sequence has no
                       way to say that, so the queue's nearest expression of it (the slot after the running
                       program) puts the rest of the causing program in front of it. That is a timeline no
                       browser produces, and it was invisible for exactly as long as the prose above it claimed
                       the slot WAS the step. */
                    DFAIL("HTML §4.12.1.1 \"Processing model\" step 36 is \"Otherwise, immediately execute the "
                          "script element el, even if other scripts are already executing\", and this engine "
                          "cannot perform it: the program is queued at the slot AFTER the program that "
                          "inserted the element, so `body.appendChild(s); f()` runs f() before s's code where "
                          "a browser runs it after — and so does `s.textContent = code; f()` through the "
                          "children changed steps, and a `document.write` of an inline classic script through "
                          "the parser. DOM §4.2.3 \"Mutation algorithms\"'s insert step 12 is \"for "
                          "each node of staticNodeList: if node is connected, then run the post-connection "
                          "steps with node\", so the causing program is mid-statement while the script runs. "
                          "WHAT THE NEXT DIFF BUILDS: this call becomes a program sub-sequence on the "
                          "tree-steps drain, which is already a step machine holding a JSStepHdr "
                          "(core/dom/element.c's element_tree_steps_step) — the same shape ECMAScript "
                          "§19.2.1.1 PerformEval already has in the fork (step_program_run), where the program "
                          "is compiled, its closure handed to the trampoline and the machine parked until the "
                          "value comes back. "
                          "ITS SUBPROBLEM — THE ORDER — IS BUILT, AND THIS CALL IS REACHED FROM IT: "
                          "core/dom/element.c drains §4.2.3's insertion steps (insert step 7.7) and its "
                          "post-connection steps (step 12) as TWO phases over one batch, and this element's "
                          "preparation is HTML §4.12.1.1's own \"script HTML element post-connection steps\", "
                          "so it runs in the second phase over the staticNodeList step 10 collects up front. "
                          "Step 12's connectedness is re-read per entry there, which is what makes "
                          "§4.12.1.1's worked example — `body.append(script1, script2)` where script1's body "
                          "removes script2 prints nothing — come out right the moment the program above "
                          "actually runs. "
                          "AND TWO NAMED THINGS STILL STAND BETWEEN THIS DFAIL AND THAT RUN, each checked "
                          "rather than recalled; a reader who writes the request without them finds there is "
                          "nowhere to put it and no way to compile what it would carry. "
                          "(1) THE COMPILE. There is no entry that turns a classic script's source text into a "
                          "trampolinable closure. JS_EVAL_FLAG_TRAMP_CLOSURE is the flag that produces one, and "
                          "JS_EvalInternal DCHECKs that a compile carrying it passed the @S EVAL-sink seam "
                          "(js_eval_program_source) — which is right for the two algorithms that set it today, "
                          "ECMAScript §19.2.1.1 PerformEval and §20.2.1.1.1 CreateDynamicFunction, and WRONG "
                          "for this element: a `<script>`'s program is HTML §8.1.4.4 \"Calling scripts\"'s run "
                          "a classic script and is not a code-execution sink, so announcing it would report "
                          "every page script as one. The assert must key on the eval TYPE that makes an "
                          "algorithm a sink (DIRECT / INDIRECT), never on the flag that only says who "
                          "trampolines the body. "
                          "(2) THE OTHER TWO DOORS. This arm is reached from THREE positions and only the "
                          "post-connection steps stand on a step machine. §4.12.1.1's children changed steps "
                          "run at DOM §4.2.3 insert step 9 through core/dom/node.c's node_children_changed — a "
                          "plain hook list called inside the mutation, which is where `s.textContent = code` "
                          "arrives — and the parser's own end-tag step runs inside a lexbor parse, which is the "
                          "parser-suspension capability core/html/html_parse.h already owes. So a seam built at "
                          "the drain alone would leave engine_queue_script_immediate standing as the fallback "
                          "for the other two, which is the one thing it may not be: the children changed steps "
                          "become a recorded-and-drained family exactly as the tree steps are, so every door "
                          "into `execute the script element` converges on the ONE machine that can carry the "
                          "request, and the queue entry is DELETED in that diff rather than kept behind an if. "
                          "HOW ITS ABSENCE SHOWS: web-platform-tests "
                          "domparsing/createContextualFragment.html's \"<script>s should be run when appended "
                          "to the document (but not before)\" fails at its LAST assertion and passes the two "
                          "before it, which only require that nothing ran too EARLY");
                    engine_queue_script_immediate(document_doc(ctx), (const char *)txt, n_len, el);
                }
            }
            lxb_dom_document_destroy_text(n->owner_document, txt);
        }
    }
}

/* THE REALM THE TWO PARSER ENTRIES BELOW RUN IN — §13.2.6's for both, and §14.2's for the second of them; see
   html_script.h for why it is derived and not passed.
   TWO QUESTIONS, TWO CALLS, AND THEY ARE DIFFERENT QUESTIONS rather than one answered twice.
   `document_realm_of` is the realm the DOM's own steps run in, and is what the `already started` slot is
   WRITTEN through: the slot is an own property of the element's wrapper, so it needs A realm and any realm
   that can reach the node will do. `document_active_realm_of` is §8.1.3.4 "Enabling and disabling scripting"'s
   browsing context, which is what §4.12.1 step 18 RETURNS on, and it is a strictly narrower answer — a
   DOMParser document has the first and not the second, which is exactly the population that section's own note
   lists. Using either for the other's job is the whole difference between marking a flag and running a page's
   code in a document that has no browsing context to run it in. */

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
    /* §4.12.1 step 18 — "If scripting is disabled for el, then return", which §8.1.3.4 "Enabling and disabling
       scripting" defines over the node document's browsing context. Asked as the ACTIVE-document realm because
       that IS the browsing context here, and answered NULL for every complete parse this engine performs before
       a Document is given its navigable — whose scripts core/loader/document_scripts.c inventories instead.
       See html_script.h for why that makes this route the one that reaches a written script and nothing else. */
    ctx = document_active_realm_of(lxb_dom_interface_node(script->owner_document));
    if (!ctx) return;
    /* "PREPARE THE SCRIPT ELEMENT SCRIPT", and it is PARSER-INSERTED — §13.2.6.4.4 'The "in head" insertion
       mode' set this element's parser document when it created it, which is the fact steps 4 and 14 turn on and
       which decides whether a `<script src>` with no `async` attribute is a parser-blocking script or a member
       of the in-order ASAP list. HTML §14.2 "Parsing XML documents" states the same of ITS parser in the same
       breath as the end tag — "it must have its parser document set and its force async set to false" — so the
       XML caller passes true here for the standard's own reason and not by analogy with this one. */
    html_script_prepare(ctx, lxb_dom_interface_element(script), /*parser_inserted*/true);
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
