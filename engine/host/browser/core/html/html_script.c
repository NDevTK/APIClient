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
   over one element's life — the parse marks it, and §4.12.1's cloning steps write the copy's from the
   original's — and a slot defined with no flags makes the second write a silent no-op. */
#define SCRIPT_SLOT_FLAGS (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE)

/* §4.12.1's `async` SETTER STEPS, declared here because the declaration is the agent's and the init below is
   where an agent's one-per-runtime state is minted; the steps themselves are beside the getter. */
static JSValue js_script_set_async(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic);

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
   is the same composite test §8.6.4 step 3 makes a few hundred lines away in element.c — HTML's `script` and
   SVG's are both script elements, and lexbor's own `lxb_html_tree_node_is` answers only for the first because
   it hardcodes the HTML namespace. It replaces a memcmp over the QUALIFIED name, which is the same set by
   accident (a prefixed `foo:script` does not match six bytes) and says nothing about why. */
static bool script_is(const lxb_dom_node_t *n)
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
    DCHECK(script_is(n), "`already started` was written onto a node that is not an HTML `script` element");
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
    DCHECK(script_is(n), "`force async` was written onto a node that is not an HTML `script` element");
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

    if (script_is(n)) return n;
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

void html_script_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                              const char *val)
{
    if (!script_is(lxb_dom_interface_node(el))) return;
    if (ns != NULL || !local || strcmp(local, "async")) return;   /* the `async` CONTENT attribute, null namespace */
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
       §4.12.1's cloning steps carry the flag with it, so an unmarked one would run from the clone. lexbor
       leaves the fragment's `parent` NULL and points its `host` back at the element, which is what the ascent
       climbs; a template can hold BOTH lists (only the parser and `t.content` reach the fragment, while
       `t.appendChild(x)` reaches the element), so coming back visits the ordinary children next. */
    for (;;) {
        if (script_is(n)) {
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
    if (!script_is(src)) return;
    DCHECK(script_is(copy),
           "DOM §4.4 clone a node produced a copy of a `script` element that is not one — the cloning steps "
           "HTML defines for `script` are stated over a copy of the same element, and a pair that disagrees "
           "means step 2's `clone a single node` built the wrong interface");
    /* "Set copy's already started to node's already started." FALSE is the copy's initial value and there is
       no slot to clear — a fresh element has never been written — so only the true case has anything to do. */
    if (script_already_started(ctx, src)) script_set_already_started(ctx, copy);
}

/* HTML §4.12.1 "prepare the script element", the INSERTION half — DOM §4.2.3's insertion steps are what reach
 * it. A page loads code conditionally in three ways and this is the second:
 * `s = createElement("script"); s.src = u; body.appendChild(s)`. Before this existed the injection was a SILENT
 * no-op — the element went into the tree and the code it named was never fetched, never run, never even
 * reported, so every endpoint and sink behind an A/B flag or a feature gate was missing with nothing to say so.
 * The loaded code is more PROGRAM OF THE INJECTING FLOW: it joins that flow's script sequence, so it runs under
 * the delta, the pins and the position in the BFS of the world that injected it, and a sibling that never took
 * the branch never sees it. */
static char *script_src_absolute(JSContext *ctx, const char *src, size_t src_len);

void html_script_prepare(JSContext *ctx, lxb_dom_element_t *el)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    size_t n_len = 0;
    const lxb_char_t *src;
    ScriptSchedule sched;
    JSValue t;

    if (!script_is(n)) return;
    /* STEP 1: "If el's already started is true, then return." This is the whole of what makes §13.4's fragment
       parse inert — the parsed script is in the tree, is queryable, serialises back out, and does not run. */
    if (script_already_started(ctx, n)) return;
    /* THE TYPE-STRING STEPS, WHICH THIS HALF NEVER ASKED — so an injected `<script type="application/json">`
       was handed to the compiler and RAN, as did an import map, while the document-scan half had recognised
       both since it was written. One element, one question: `script_block_type` is that question, and the two
       halves of §4.12.1 must not disagree about what a `type` attribute means. */
    {
        ScriptType st = script_block_type(el);
        /* HTML's null and the two data types: "No script is executed." An import map and a set of speculation
           rules are REGISTERED on the relevant global rather than evaluated, which is a capability this engine
           does not have — and their absence is honest, because neither runs code. */
        if (!script_type_executes(st)) return;
        /* A MODULE cannot travel this route yet, and running it as a classic script is the exact defect the
           document-scan half was just fixed for — it would come back a SyntaxError on the page's own `import`.
           The route is engine_queue_script, whose entries the scheduler reads as DYN_PAGE_SCRIPT and compiles
           with §8.1.3.3's CLASSIC entry; carrying MODULE means the flow's dynamic sequence carries a ScriptType
           beside each body the way the document's sequence now does (solver/engine.h's `types`), and the
           compile in flow_step then routes it to JS_FlowEvalModule exactly as it routes a document module. */
        DCHECK(st != SCRIPT_TYPE_MODULE,
               "a `<script type=module>` was inserted into the tree and this half can only queue a CLASSIC "
               "program — give the flow's dynamic script sequence a ScriptType per entry (engine_queue_script "
               "and solver/flow.h's dyn arrays), the way the document's sequence carries one, so flow_step "
               "routes it to §8.1.3.3's module entry instead of compiling the page's `import` as a script");
        /* §4.12.1's LAST STEPS, asked of the same element by the same function the document scan asks — one
           element, one classification. PARSER-INSERTED IS FALSE HERE and that is a fact about this entry rather
           than a default: a fragment parse's scripts are already started and returned at step 1 above, and a
           DOCUMENT parse's are collected by core/loader/document_scripts.c instead of ever reaching §4.2.3's
           insertion steps — so everything that arrives here was inserted by page code and has a null parser
           document. Which leaves the two destinations a non-parser-inserted element can reach, and `force
           async` is what decides between them. */
        sched = script_block_schedule(el, st, /*parser_inserted*/false, script_force_async(ctx, n));
    }
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
    if (lxb_dom_element_has_attribute(el, (const lxb_char_t *)"src", 3)) {
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
        /* WHICH OF THE TWO ASAP DESTINATIONS, and the difference is a POSITION. The `set of scripts that will
           execute as soon as possible` has none — §13.2.7 waits for that set only before the load event — so it
           parks and its reply becomes a program whenever it drains. The `list of scripts that will execute in
           order as soon as possible` is what `s.async = false` puts an element in, and §4.12.1's own steps for
           it are "if scripts[0] is not el, then abort" — the element holds its place against the others, so it
           takes a slot in the flow's sequence and the flow stops there until the reply fills it. */
        if (sched == SCRIPT_SCHED_ASAP) engine_pending_script_url(ctx, u);
        else {
            DCHECK(sched == SCRIPT_SCHED_IN_ORDER_ASAP,
                   "an injected external script was scheduled somewhere other than the two `as soon as "
                   "possible` destinations — §4.12.1 reaches the when-parsed list and the pending "
                   "parsing-blocking script only for an element with a non-null parser document, and every "
                   "element that reaches this half was inserted by page code");
            engine_queue_docscript_url(document_doc(ctx), u);
        }
        free(u);
        return;
    }
    /* No src: the element's own text IS the program, and it runs on insertion. */
    DCHECK(sched == SCRIPT_SCHED_IMMEDIATE,
           "an inline injected script is scheduled somewhere other than its own insertion point — §4.12.1 owes "
           "no fetch for a classic script whose source it already has, so its tail ends at `immediately execute "
           "the script element`, and the one inline element that goes elsewhere is a MODULE, rejected above");
    {
        lxb_char_t *txt = lxb_dom_node_text_content(n, &n_len);
        if (txt) {
            /* IN THE DOCUMENT WHOSE TREE IT WAS INSERTED INTO — "prepare the script" runs it with the
               element's node document's settings object, which is the realm this chokepoint was entered
               with. A program is a program OF a document (solver/flow.h), so it names one. */
            /* …AND AT THE SLOT THE ASSERT ABOVE ALREADY NAMED. §4.12.1.1's last step is "immediately execute
               the script element el, even if other scripts are already executing", so this program runs before
               anything the flow's sequence already holds. It went to engine_queue_script, whose entries take
               the TAIL — the position of the `as soon as possible` destinations this element is explicitly not
               in — so an injected <script> ran after every remaining program of the document instead of
               before the next statement of the code that injected it. */
            if (n_len) engine_queue_script_immediate(document_doc(ctx), (const char *)txt);
            lxb_dom_document_destroy_text(n->owner_document, txt);
        }
    }
}

/* §4.12.1's "encoding-parsing a URL given src, RELATIVE TO EL'S NODE DOCUMENT" — §4.4's API base URL, which for
   an element inserted into a child navigable's document is THAT document's and not the creator's. Answers
   malloc'd, or NULL for the standard's own "return" branch: a `src` that does not parse runs no script. The
   same three lines core/frame/navigable.c resolves its document's own `<script src>` with, here because a
   SCRIPT-inserted element reaches the loader by the other of §4.12.1's two halves. */
static char *script_src_absolute(JSContext *ctx, const char *src, size_t src_len)
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
