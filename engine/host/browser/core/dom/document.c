/* THE DOCUMENT INTERFACE — Blink core/dom, the members that can be answered truthfully today.
 *
 * WHAT IS HERE splits the same way Location does. `URL`, `documentURI`, `domain` and `title` are facts about
 * the document this engine actually parsed, so they are CONCRETE and a bundle that routes on them gets the real
 * value. `cookie` and `referrer` are INPUT — a cookie jar this engine was not given and a referrer the visitor
 * arrived with — so they are concolic, example-free, and a branch on either FORKS. `document.cookie` in
 * particular is the source that carries a session into a request URL, and collapsing it to "" makes every
 * cookie-gated path unreachable.
 *
 * WHAT IS NOT HERE is the tree: querySelector, getElementById, createElement, body, head. They need the Element
 * interface, which does not exist yet, and the honest answer is ABSENCE. A querySelector that returns null for
 * an element the document HAS is a lie the page cannot detect and this engine would report a surface it never
 * reached; a ReferenceError names the component to write. The DOM is parsed and sitting in `dom` — this
 * component holds it precisely so Element can be grown against it. */
#include <string.h>

#include "check.h"
#include "solver/dom_cow.h"   /* dom_cow_note_created — a created node belongs to the flow's delta */
#include "quickjs.h"
#include "solver/concolic.h"
#include "solver/engine.h"
#include "core/events/event.h"
#include "core/events/create_event.h"
#include "core/events/event_target.h"
#include "core/events/report_exception.h"
#include "core/html/html_element.h"
#include "core/html/html_form.h"
#include "core/html/custom_elements.h"
#include "core/html/element_internals.h"
#include "core/html/html_iframe.h"
#include "core/dom/dom_token_list.h"
#include "core/dom/collections.h"
#include "core/dom/attr.h"
#include "core/dom/attr_list.h"
#include "core/css/css_style_declaration.h"
#include "core/dom/document.h"
#include "solver/world.h"
#include "core/frame/window_proxy.h"
#include "core/frame/navigable.h"
#include "core/dom/page_visibility.h"
#include "core/dom/document_fragment.h"
#include "core/dom/document_type.h"
#include "core/dom/dom_implementation.h"
#include "core/idl_args.h"
#include "core/realm.h"

/* Every member here takes DOMStrings; createElementNS takes two. Declared, not masked. */
static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
#include "core/dom/node.h"
#include <lexbor/css/css.h>
#include <lexbor/selectors/selectors.h>

#include "core/dom/element.h"
#include "core/dom/node_filter.h"
#include "core/dom/node_iterator.h"
#include "core/dom/tree_walker.h"
#include "core/dom/range.h"

/* THE DOCUMENT'S OWN STATE, HELD ON THE REALM THAT IS THIS DOCUMENT — not on the file.
 *
 * AN AGENT IS A JSRuntime AND A DOCUMENT IS A JSContext IN IT, because that is what the two words mean. HTML
 * puts every same-origin document of one browsing-context group in ONE similar-origin window agent — one heap —
 * and gives each its own global; a JSRuntime is the heap and a JSContext is the global. So the state a document
 * HAS (its tree, its address, its policy container, its `document` object, its Window) hangs off the context.
 *
 * It was file-scope, and file-scope IS the sentence "one instance is one document" — which is the sentence this
 * design stopped making. Same-origin documents share a heap and the corpus RELIES on it:
 * `iframe.contentDocument.body.appendChild(subframe)` inserts a node THIS document created, and afterwards
 * `subframe.parentNode` is a node of the OTHER document while the wrapper stays the same object. There is no
 * name to pass there — it is one object graph.
 *
 * WHAT DOES NOT LIVE HERE is anything the whole agent shares: a class id, a prototype, an interface object, and
 * the ORIGIN — an agent is origin-keyed, so every document in it has the same one, which is exactly why
 * SECURITY.md's one-principal-per-instance still holds word for word. */
/* A REALM IS THE ACTIVE DOCUMENT OF ONE NAVIGABLE — AND IT IS NOT THE ONLY DOCUMENT IN IT.
 *
 * §4.5's createHTMLDocument, createDocument and `new Document()` build a Document that has NO browsing context:
 * no navigable, no Window, no WindowProxy, and no scripts of its own. HTML's similar-origin window agent is one
 * heap, so such a document is neither a second realm nor a second instance — it is a second tree in this one,
 * and its nodes are ordinary objects of this realm (`foreignDoc.createElement("p") instanceof Element` holds,
 * and its wrapper's prototype is THIS realm's).
 *
 * SO THE RECORD IS PER DOCUMENT, NOT PER REALM, and it hangs off the Lexbor document's own embedder slot — the
 * same slot that used to hold the realm pointer, which was already the (document -> realm) answer §4.2.3 needs.
 * One indirection more buys every per-document fact: the address `baseURI` reads, the content type, and §4.5's
 * `[SameObject] implementation`. The realm's context opaque still names its ACTIVE document, because that is a
 * fact about the realm and the answer to "the API base URL" and "who fires load".
 *
 * WHAT IS PER NAVIGABLE stays on the active document's record and is UNDEFINED on every other: a document with
 * no browsing context has no Window and no WindowProxy, which is exactly what §3.1.1's `location` returning
 * null for one means. */
typedef struct Document {
    JSContext           *realm;    /* the realm this document belongs to — every document has exactly one */
    uint32_t             doc;      /* this document's handle in the world registry — its NAME is what crosses */
    lxb_html_document_t *dom;
    int                  owned;    /* this record destroys `dom` — true for a document the page CREATED */
    PolicyContainer     *policy;    /* owned; NULL for a document with no browsing context */
    JSValue              doc_obj;   /* the `document` object — HELD, released by document_free */
    JSValue              win_obj;   /* this document's Window — HELD, and UNDEFINED with no browsing context */
    /* §7.2.5.1's ONE WindowProxy FOR THIS NAVIGABLE — `window`, `self`, and the `source` of every message this
       document posts. It lives on the REALM because that is what it is one of: a page comparing `e.source`
       across two messages must find the same object, and a table keyed by document would be an immortal root
       holding one proxy per navigable a forced-execution frontier ever created — thousands, none collectable.
       HELD, released with the realm. UNDEFINED for a document with no browsing context. */
    JSValue              proxy;
    /* §4.5's `[SameObject] readonly attribute DOMImplementation implementation`. SameObject is what makes this
       a field rather than a fresh object per read: a page holds `document.implementation` and compares it. */
    JSValue              impl;
    char                 url[2048]; /* the document's address, which §4.4 baseURI reads */
    char                 content_type[32];   /* §4.5 contentType — what this document was created as */
    /* THE REALM'S CHAIN OF DOCUMENTS IT CREATED AT BASELINE. A document a FLOW creates is owned by that flow's
       COW delta (dom_cow_note_created_document) and dies with it; one created while capture is off is baseline,
       exactly like a baseline node, and the realm that made it is what outlives it. Head on the active
       document's record, because that is the record the realm's teardown already reaches. */
    struct Document     *next_created;
} Document;

/* THE RUNNING REALM'S ACTIVE DOCUMENT. The context opaque, because a JSContext IS one navigable's document — so
   there is no table to look it up in and no way for the answer to be the wrong document's. NULL before install,
   which is a state only the accessors that tolerate it may see. */
static Document *doc_of(JSContext *ctx)
{
    return (Document *)JS_GetContextOpaque(ctx);
}

static Document *doc_here(JSContext *ctx)
{
    Document *d = doc_of(ctx);
    DCHECK(d != NULL, "a document member ran in a realm with no Document — document_install names which realm "
                      "a document is, and a realm that never had one cannot answer for a tree it has not got");
    return d;
}

/* THE RECORD FOR A DOM DOCUMENT — the answer to every per-document question, whatever realm is asking. NULL for
   a Lexbor document no record was ever built for, which is a solver scratch parse. */
static Document *doc_rec(const lxb_dom_document_t *dom)
{
    return dom ? (Document *)dom->user : NULL;
}

/* THE RECORD FOR THE DOCUMENT A MEMBER WAS CALLED ON. Every §4.5 member is `Document.prototype`'s, so its
   receiver names WHICH document it is about — `foreignDoc.createElement` must build its element in foreignDoc,
   and reading the realm's active document instead is the defect a second Document makes visible. */
static Document *doc_receiver(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_node_t *n = node_of(this_val);
    Document *d;

    /* WEB IDL §3.7.5: a member reached with a receiver that does not implement the interface is a TypeError,
       thrown at the read — `Object.getOwnPropertyDescriptor(Document.prototype, "URL").get.call(null)` is a
       thing the corpus does deliberately. It is NOT an engine invariant and so NOT a DCHECK: asserting it would
       turn a test that asks for the throw into an abort that takes the whole file with it. */
    if (!n || n->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
        JS_ThrowTypeError(ctx, "this is not a Document");
        return NULL;
    }
    d = doc_rec(lxb_dom_interface_document(n));
    DCHECK(d != NULL, "a Document member ran on a Lexbor document with no record — document_install and "
                      "document_new are the two places one is built, and a tree that came from neither cannot "
                      "answer for its own address");
    return d;
}

/* §4.2.6 / §4.9 THE SELECTOR MEMBERS, AS ONE MACHINE — querySelector, querySelectorAll, matches, closest.
 *
 * They were two implementations with two different defects, and the defects were the same shape twice: work
 * done per node that belongs to the query.
 *
 *   - qs_run reached lxb_selectors_find, which walks the whole subtree to completion inside one opcode. It is
 *     the most-called query in a modern page and it was the last drive-to-completion in this component.
 *   - document_sel_match CREATED AND DESTROYED a CSS parser, a selectors context and a compiled selector list
 *     on EVERY CALL — and `closest` calls it once per ancestor, so walking up ten levels compiled the same
 *     selector ten times.
 *
 * Compiling once and then walking is what both of them wanted, and it is also exactly what a machine needs: the
 * compiled list is the thing that survives the suspension, and the cursor is the resume point. So there is one
 * of these, and what the four members differ in is WHERE the cursor goes and WHAT is done with a match —
 * declared as a magic, not as four bodies.
 *
 * lxb_selectors_match_node is what makes the walk equivalent to lxb_selectors_find rather than an approximation
 * of it: a combinator is resolved by walking UP from the candidate, through the whole document, so
 * §4.2.6's scoped matching still holds — `el.querySelectorAll('div p')` finds a <p> under `el` whose <div>
 * ancestor is OUTSIDE `el`, because the selector is evaluated against the document and only the RESULTS are
 * filtered to the subtree. That is asserted rather than assumed; it is the case an implementation that walks a
 * subtree in isolation gets wrong. */
enum { QS_FIRST = 0, QS_ALL, QS_MATCHES, QS_CLOSEST };

/* WHERE THIS MACHINE RESTS, AS THE STANDARD NUMBERS IT. All four members are the same two steps: parse the
 * selector, then match it — §1.3 states them for querySelector and querySelectorAll (through scope-match a
 * selectors string) and §4.9 restates them for matches and closest, with the same wording and the same order.
 * The parse is ONE stage because no page code can run between "parse a selector" and "if it is failure, throw"
 * — nothing observes the intermediate — and the label says the range. The match is its own stage because it is
 * a walk of the page's tree, and it rests once per node so a sibling flow can overtake it there. */
#define QS_STAGES(X) \
    X(QS_PARSE, "DOM §1.3 steps 1-2 / §4.9 steps 1-2 (parse a selector; SyntaxError if it is failure)") \
    X(QS_MATCH, "DOM §1.3 step 3 (match a selector against a tree) / §4.9 matches step 3 / closest steps 3-5, " \
                "one node per step")
enum { IDL_STEP_STAGE_BASE(QS_STAGES) QS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const QS_STEPS[] = { QS_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    lxb_css_parser_t       *parser;
    lxb_selectors_t        *selectors;
    lxb_css_selector_list_t *list;
    lxb_dom_node_t *root, *cursor;
    JSValue arr;      /* QS_ALL's collected matches (owned) */
    uint32_t n;
} QsState;

static lxb_status_t qs_hit_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *vctx)
{
    (void)node; (void)spec;
    *(bool *)vctx = true;
    return LXB_STATUS_OK;
}

static void qs_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    QsState *s = st;
    /* A FORK CANNOT REACH A SELECTOR WALK. It runs none of the page's code, so no concolic branch can happen
       under it — which is what lets the compiled list be held as a bare pointer: there is no such thing as half
       a selector context to hand a second flow. */
    DCHECK(s->parser == NULL, "a selector walk was forked mid-walk");
    v->val(ctx, &s->arr);
}

static void qs_release(JSContext *ctx, void *st)
{
    QsState *s = st;
    (void)ctx;
    /* The throw path owns these too — a flow dropped mid-walk would otherwise leak a compiled selector list and
       the two contexts behind it. */
    if (s->list) lxb_css_selector_list_destroy_memory(s->list);
    if (s->selectors) lxb_selectors_destroy(s->selectors, true);
    if (s->parser) lxb_css_parser_destroy(s->parser, true);
    s->list = NULL; s->selectors = NULL; s->parser = NULL;
}

/* Does the compiled selector match this node? The one place lexbor is asked, so the four members cannot
   disagree about what a selector means. */
static bool qs_matches(QsState *s, lxb_dom_node_t *node)
{
    bool hit = false;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_selectors_match_node(s->selectors, node, s->list, qs_hit_cb, &hit);
    return hit;
}

static int js_document_qs(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    QsState *s = st;
    int magic = idl_step_magic(hdr);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);

    if (hdr->stage == QS_PARSE) {
        lxb_dom_node_t *n = node_of(hdr->this_val);
        const char *sel;

        if (!n || argc < 1) {
            *presult = magic == QS_ALL ? collections_static(ctx, JS_NewArray(ctx))
                     : magic == QS_MATCHES ? JS_FALSE : JS_NULL;
            return JS_STEP_DONE;
        }
        sel = concolic_name_cstr(ctx, argv[0]);   /* the declaration passes UNKNOWN input through as itself, so an unknown name denotes its SHAPE */
        if (!sel) return JS_STEP_ABRUPT;
        s->parser = lxb_css_parser_create();
        s->selectors = lxb_selectors_create();
        if (!s->parser || lxb_css_parser_init(s->parser, NULL) != LXB_STATUS_OK ||
            !s->selectors || lxb_selectors_init(s->selectors) != LXB_STATUS_OK) {
            JS_FreeCString(ctx, sel);
            CHECK_FAIL("the CSS selector engine could not be initialised");
        }
        s->list = lxb_css_selectors_parse(s->parser, (const lxb_char_t *)sel, strlen(sel));
        JS_FreeCString(ctx, sel);
        if (!s->list) {
            /* §4.2.6 AND §4.9: an unparseable selector is a SyntaxError from ALL FOUR members. matches and
               closest already threw; querySelector and querySelectorAll answered null and an empty list, so a
               page with a typo in a selector got "no such element" instead of being told, and the branch behind
               that answer ran. */
            JS_ThrowDOMException(ctx, "SyntaxError", "not a valid selector");
            return JS_STEP_ABRUPT;
        }
        s->root = n;
        /* WHERE THE CURSOR GOES is the whole of what the four members differ in: a subtree for the two queries,
           the node itself for matches, and the node plus its ancestors for closest. */
        s->cursor = (magic == QS_FIRST || magic == QS_ALL) ? node_next_in(n, n) : n;
        if (magic == QS_ALL) {
            s->arr = JS_NewArray(ctx);
            CHECK(!JS_IsException(s->arr), "querySelectorAll could not allocate its result");
        }
        hdr->stage = QS_MATCH;
        return JS_STEP_YIELD;
    }

    DCHECK(hdr->stage == QS_MATCH, "a selector walk resumed into a stage its algorithm does not have");
    if (!s->cursor) {
        /* Ran out without a match. */
        switch (magic) {
        case QS_ALL:
            /* §4.2.6: a STATIC NodeList, because the spec says the result does not track the tree — and a real
               one, so `instanceof NodeList` holds and `.map` is honestly absent as it is in a browser. */
            *presult = collections_static(ctx, s->arr);
            s->arr = JS_UNDEFINED;
            break;
        case QS_MATCHES: *presult = JS_FALSE; break;
        default:         *presult = JS_NULL;  break;
        }
        return JS_STEP_DONE;
    }

    if (qs_matches(s, s->cursor)) {
        switch (magic) {
        case QS_ALL:
            JS_SetPropertyUint32(ctx, s->arr, s->n++, node_wrap(ctx, s->cursor));
            break;
        case QS_MATCHES:
            *presult = JS_TRUE;
            return JS_STEP_DONE;
        default:
            *presult = node_wrap(ctx, s->cursor);   /* the FIRST in tree order, or the nearest ancestor */
            return JS_STEP_DONE;
        }
    }

    switch (magic) {
    case QS_FIRST:
    case QS_ALL:     s->cursor = node_next_in(s->cursor, s->root); break;
    case QS_MATCHES: s->cursor = NULL;                             break;   /* this node alone */
    default:         s->cursor = s->cursor->parent;                break;   /* INCLUSIVE ancestors */
    }
    return JS_STEP_YIELD;
}

static const IdlStepDecl QS_STEP = { js_document_qs, sizeof(QsState), qs_visit, qs_release,
                                     "DOM §1.3 scope-match a selectors string / §4.9 Element.matches, closest",
                                     QS_STEPS };

const IdlStepDecl *document_qs_decl(void) { return &QS_STEP; }

/* §3.1.5 THE DOCUMENT'S ELEMENT SHORTCUTS — forms, images, scripts, embeds and links. Every one is a LIVE
   HTMLCollection the spec defines as "the elements of type X in the document", so each is the by-name
   collection over the document with a tag baked in, and `links` is the one that is a predicate instead
   (`a`/`area` WITH an href). A page uses these to find its own markup, and a bundle scanner uses
   `document.scripts` and `document.forms` in particular — with them absent the loop over them never ran and
   nothing said why.
   magic 0 = forms, 1 = images, 2 = scripts, 3 = embeds, 4 = links. */
static JSValue js_doc_shortcut(JSContext *ctx, JSValueConst this_val, int magic)
{
    static const char *const TAGS[] = { "form", "img", "script", "embed" };
    if (magic == 4) return collections_links(ctx, this_val);
    DCHECK(magic >= 0 && magic < (int)(sizeof(TAGS) / sizeof(TAGS[0])),
           "a document element-shortcut ran with a magic it does not have");
    return collections_by_name(ctx, this_val, TAGS[magic], false);
}

/* §4.5 createDocumentFragment(). A page batches inserts into one and attaches it once, which is the ordinary
   way to add many nodes — and it is the same object `new DocumentFragment()` builds, so this is the member
   name for a constructor that already exists rather than a second way to make one. */
static JSValue js_doc_create_fragment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    lxb_dom_node_t *n = node_of(this_val);
    lxb_dom_document_fragment_t *frag;

    (void)argc; (void)argv; (void)magic;
    DCHECK(n != NULL, "createDocumentFragment ran on something that is not the document");
    frag = lxb_dom_document_fragment_interface_create(n->owner_document);
    CHECK(frag != NULL, "createDocumentFragment: the Lexbor fragment allocation failed");
    return node_wrap(ctx, lxb_dom_interface_node(frag));
}

/* 4.5.1 createElement. The element is created IN this document and returned DETACHED — a page builds a subtree
   and attaches it later, and creating it already-attached would put nodes in the tree the page never inserted.
   It is not a per-flow write for that reason: nothing observable changed until appendChild, which IS one. */
static JSValue js_doc_create_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *tag;
    lxb_dom_element_t *el;
    Document *d;
    JSValue r;

    if (argc < 1) return JS_NULL;
    /* 4.5.1 OVER AN UNKNOWN TAG NAME. Lexbor needs real bytes to create an element and the coercion below owes
       them, so an unknown tag can only crash there — and `createElement(someParameter)` is a real pattern, and
       an XSS-relevant one (the tag decides whether the node executes). The answer is an unknown derived from
       the source rather than a node of a guessed name: a concrete tag would be an element the page never
       created, and every query and every sink after it would be about the wrong node. */
    if (concolic_is(argv[0])) {
        /* THE CREATION RUNS, on the tag this source concretely is. §solver allows no other way for an example
           to propagate — the engine performs the real operation on the concrete, never a rule that predicts
           what performing it would have produced — so this re-enters with the example in place of the operand
           and the element it really builds becomes the derived value's example. With no example there is
           nothing to run and the answer is honestly unknown, which is a non-answer rather than a guess. */
        JSValue ex = concolic_example(ctx, argv[0]), real = JS_UNDEFINED;
        if (JS_IsString(ex)) {
            JSValueConst a2[1];
            a2[0] = ex;
            real = js_doc_create_element(ctx, this_val, 1, a2, magic);
            if (JS_IsException(real)) { JS_FreeValue(ctx, JS_GetException(ctx)); real = JS_UNDEFINED; }
        }
        JS_FreeValue(ctx, ex);
        return concolic_builtin_hook(ctx, argv[0], "createElement", real);
    }
    tag = JS_ToCString(ctx, argv[0]);
    if (!tag) return JS_EXCEPTION;
    /* §4.5 step 5's "create an element GIVEN THIS": the element's node document is the RECEIVER's, never the
       realm's active one — `foreignDoc.createElement("p")` builds a node of foreignDoc. */
    d = doc_receiver(ctx, this_val);
    if (!d) { JS_FreeCString(ctx, tag); return JS_EXCEPTION; }
    el = lxb_dom_document_create_element(lxb_dom_interface_document(d->dom),
                                         (const lxb_char_t *)tag, strlen(tag), NULL);
    JS_FreeCString(ctx, tag);
    dom_cow_note_created(el ? lxb_dom_interface_node(el) : NULL);   /* this flow made it: the delta owns it */
    DCHECK(el != NULL, "createElement produced no element — a page building its DOM would silently build "
                       "nothing and every query after it would answer null");
    r = element_wrap(ctx, el);
    return r;
}

/* DOM §4.5 createElement, AS THE ALGORITHM IT DELEGATES TO — §4.9 "create an element" with
 * `synchronousCustomElements` TRUE, which is the `true` that makes a custom element's constructor run INSIDE
 * document.createElement rather than at some later checkpoint.
 *
 * WHY IT IS A MACHINE. Step 5.1.4.1 is "constructing C with no arguments", and C is the page's class: its body
 * has loops, awaits and DOM mutations in it, so a JS_CallConstructor from here is the drive-to-completion the
 * engine aborts on and the class's own `super()` would reach a C entry with no flow base under it. A construct
 * is a request like every other, and the machine rests on it.
 *
 * AND STEP 5.1.4 IS "RUN THESE STEPS WHILE CATCHING ANY EXCEPTIONS", which is the whole of why the answer to a
 * throwing constructor is an element and not a propagating throw. The construct AND the checks after it are
 * inside that catch, so `class X extends HTMLElement { constructor(){ super(); this.attachInternals(); } }`
 * over an absent API — or one that returns the wrong node — REPORTS (fires `error` at the global, the page's
 * code again) and answers with an HTMLUnknownElement of the requested local name whose custom element state is
 * "failed". Propagating instead destroyed the whole document: eleven corpus files went from a real number to
 * no result at all on the diff that first made constructors run. The capability is DECLARED
 * (IdlStepDecl::catches_abrupt) rather than assumed, because the abrupt then arrives at this body's own
 * request site and every request this machine makes has to answer for it. */
#define DCE_STAGES(X) \
    X(DCE_LOOKUP,    "DOM §4.5 steps 1-5 into §4.9 steps 1-3 (the local name, and looking up a custom element " \
                     "definition for it) and §4.9 step 6 when there is none") \
    X(DCE_CONSTRUCT, "DOM §4.9 step 5.1.4.1 (constructing C with no arguments — the page's constructor)") \
    X(DCE_CHECKS,    "DOM §4.9 steps 5.1.4.2-11 (what the constructor returned, checked against what this " \
                     "operation asked for)") \
    X(DCE_REPORT,    "DOM §4.9 step 5.1.4's exception arm (report the exception the construct or the checks " \
                     "threw), which is HTML §8.1.4.6 report an exception")
enum { IDL_STEP_STAGE_BASE(DCE_STAGES) DCE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const DCE_STEPS[] = { DCE_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    uint8_t phase;      /* the construct request's own cursor */
    JSValue cb[1];      /* [ctor] — §4.9 step 5.1.4.1 passes no arguments, so the buffer is one slot */
    JSValue def;        /* step 3's definition (owned) */
    JSValue local;      /* the local name the operation was given (owned) — step 5.1.4.8 compares against it */
    JSValue result;     /* what the page's constructor answered (owned) */
    JSValue exc;        /* step 5.1.4's caught exception (owned), held across the report's own park */
    ReportExceptionWork rw;
} DocCreateElState;

static void doc_create_el_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    DocCreateElState *s = st;
    v->val(ctx, &s->cb[0]);
    v->val(ctx, &s->def);
    v->val(ctx, &s->local);
    v->val(ctx, &s->result);
    v->val(ctx, &s->exc);
    report_exception_work_visit(ctx, &s->rw, v);
}

static void doc_create_el_release(JSContext *ctx, void *st)
{
    DocCreateElState *s = st;
    JS_FreeValue(ctx, s->cb[0]);
    JS_FreeValue(ctx, s->def);
    JS_FreeValue(ctx, s->local);
    JS_FreeValue(ctx, s->result);
    JS_FreeValue(ctx, s->exc);
    s->cb[0] = s->def = s->local = s->result = s->exc = JS_UNDEFINED;
    report_exception_work_release(ctx, &s->rw);
}

static int js_doc_create_element_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    DocCreateElState *s = st;
    int r;

    if (hdr->stage == DCE_LOOKUP) {
        const char *tag;
        size_t len = 0;

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        /* EVERY owned field before the first thing that can throw — the failure path tears this state down
           through doc_create_el_release, which frees exactly what the state holds and nothing else. */
        s->cb[0] = s->def = s->local = s->result = s->exc = JS_UNDEFINED;
        report_exception_work_start(&s->rw);
        /* §4.9 step 3 needs a NAME to look a definition up by, and a concolic tag has none — the creation is
           the one below, over whatever example the source carries. */
        if (argc < 1 || concolic_is(argv[0])) {
            *presult = js_doc_create_element(ctx, hdr->this_val, argc, argv, 0);
            return JS_IsException(*presult) ? -1 : 0;
        }
        tag = JS_ToCStringLen(ctx, &len, argv[0]);   /* a real string by now: the declaration converted it */
        if (!tag) return -1;
        s->def = custom_elements_definition_for_name(ctx, tag, len);
        if (!JS_IsObject(s->def)) {                  /* §4.9 step 6: not a custom element */
            JS_FreeCString(ctx, tag);
            *presult = js_doc_create_element(ctx, hdr->this_val, argc, argv, 0);
            return JS_IsException(*presult) ? -1 : 0;
        }
        s->local = JS_NewStringLen(ctx, tag, len);
        JS_FreeCString(ctx, tag);
        if (JS_IsException(s->local)) { s->local = JS_UNDEFINED; return -1; }
        hdr->stage = DCE_CONSTRUCT;
    }
    if (hdr->stage == DCE_CONSTRUCT) {
        /* §4.9 steps 5.1.1-5.1.4.1. The agent's active custom element constructor map (steps 5.1.2-5.1.3, and
           5.1.5-5.1.6's restore) is what a SCOPED registry is read through, and there are none, so the map has
           one entry for every constructor and setting it changes nothing that can be read back. It becomes
           real state in the same diff that makes `customElementRegistry` a creation option. */
        JSValue ctor = custom_elements_definition_constructor(ctx, s->def);
        JSValue made = JS_UNDEFINED;

        r = step_construct_run(ctx, &s->phase, STEP_CB(s->cb), ctor, 0, NULL, cb_result, &made,
                               out_cb, out_argc);
        JS_FreeValue(ctx, ctor);
        cb_result = JS_UNDEFINED;
        if (r > 0) return r;                          /* parked ON the page's constructor */
        /* step 5.1.4's catch. The construct threw — synchronously (r < 0) or delivered as JS_EXCEPTION,
           which is what this member's declared catches_abrupt asks the driver for. */
        if (r < 0 || JS_IsException(made)) {
            JS_FreeValue(ctx, made);
            s->exc = JS_GetException(ctx);
            hdr->stage = DCE_REPORT;
            goto report;
        }
        s->result = made;
        hdr->stage = DCE_CHECKS;
    }
    if (hdr->stage == DCE_CHECKS) {
        size_t len = 0;
        const char *local = JS_ToCStringLen(ctx, &len, s->local);
        Document *d = doc_receiver(ctx, hdr->this_val);

        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        if (!local || !d) { if (local) JS_FreeCString(ctx, local); return -1; }
        r = custom_elements_created_check(ctx, s->result,
                                          lxb_dom_interface_document(d->dom), local, len);
        JS_FreeCString(ctx, local);
        /* The checks are INSIDE step 5.1.4's catch too — a constructor that gave its element an attribute, a
           child, a parent or the wrong local name throws a NotSupportedError the spec REPORTS. */
        if (r < 0) { s->exc = JS_GetException(ctx); hdr->stage = DCE_REPORT; goto report; }
        *presult = s->result;
        s->result = JS_UNDEFINED;
        return 0;
    }
report:
    DCHECK(hdr->stage == DCE_REPORT, "document.createElement resumed into a stage DOM §4.9 does not have");
    r = report_exception_run(ctx, &s->rw, s->exc, cb_result, out_cb, out_argc);
    if (r > 0) return r;                              /* parked inside the `error` event's own dispatch */
    JS_FreeValue(ctx, s->exc);
    s->exc = JS_UNDEFINED;
    /* "Set result to the result of creating an element internal given document, HTMLUnknownElement, localName,
       the HTML namespace, prefix, "failed", null, and registry." The INTERFACE is named by the step, so the
       element the page gets back is an HTMLUnknownElement carrying the local name it asked for — which is how
       a page distinguishes a component whose constructor threw from one that worked. */
    {
        /* Created through the member's own plain body, so the element lands in the RECEIVER's document — §4.5
           step 5's "given this", which a create in the current global's document would get wrong for
           `otherDoc.createElement(...)`. */
        JSValue el = js_doc_create_element(ctx, hdr->this_val, argc, argv, 0);
        JSValue proto;

        if (JS_IsException(el)) return -1;
        proto = html_unknown_element_proto(ctx);
        JS_SetPrototype(ctx, el, proto);
        JS_FreeValue(ctx, proto);
        custom_elements_mark_failed(ctx, el);
        *presult = el;
    }
    return 0;
}

static const IdlStepDecl DOC_CREATE_EL_STEP = {
    js_doc_create_element_step, sizeof(DocCreateElState), doc_create_el_visit, doc_create_el_release,
    "DOM §4.5 Document.createElement, over §4.9 create an element", DCE_STEPS,
    /* DOM §4.9 step 5.1.4 is "run these steps while catching any exceptions", so the construct's abrupt
       completion is this algorithm's VALUE — it reports it and answers with a failed HTMLUnknownElement. */
    .catches_abrupt = 1
};

/* DOM §4.9 "create an element internal" for THIS REALM'S document — see document.h. Separate from the member
   above because the two name different documents on purpose: `createElement` creates in its RECEIVER's
   document (§4.5 step 5's "given this"), and HTML §4.13.2 step 7.2 creates in the CURRENT GLOBAL's. */
JSValue document_create_element_internal(JSContext *ctx, const char *local, size_t len)
{
    lxb_dom_element_t *el = lxb_dom_document_create_element(lxb_dom_interface_document(doc_here(ctx)->dom),
                                                            (const lxb_char_t *)local, len, NULL);

    dom_cow_note_created(el ? lxb_dom_interface_node(el) : NULL);   /* this flow made it: the delta owns it */
    DCHECK(el != NULL, "HTML §4.13.2 step 7 produced no element for a definition's local name — the definition "
                       "was made from a name §4.13.1 already accepted, so Lexbor refusing it is a disagreement "
                       "about what a name is");
    return element_wrap(ctx, el);
}

/* 4.5.3 createElementNS(namespace, qualifiedName). Same element creation as createElement, plus the spec's
   "validate and extract": a qualified name may carry a prefix (`svg:rect`), and the element is created in the
   named NAMESPACE rather than the document's default. testharness.js reaches it on every completed document —
   `output_document.createElementNS(xhtml_ns, "style")` in Output.show_results — and a missing one threw
   "not a function" inside the completion callback, which aborted the callback list and so silenced the
   REPORTING of documents whose tests had all already run.
   Lexbor carries the namespace on the element, so this is its create with the namespace resolved, not a
   createElement in disguise: `el.namespaceURI` is what the page asked for. */
/* 4.5.1 createTextNode / createComment. The two non-element nodes a page builds by hand, and without them a
   page could not put TEXT into the tree at all: testharness.js's make_dom_single does
   `output_document.createTextNode(template[i])` for every string in a template. Detached, like createElement —
   nothing is observable until appendChild, which IS the per-flow write. */
static JSValue js_doc_create_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *s;
    size_t len = 0;
    lxb_dom_text_t *t;
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    s = argc >= 1 ? JS_ToCStringLen(ctx, &len, argv[0]) : JS_ToCStringLen(ctx, &len, JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    t = lxb_dom_document_create_text_node(lxb_dom_interface_document(d->dom), (const lxb_char_t *)s, len);
    dom_cow_note_created(t ? lxb_dom_interface_node(t) : NULL);   /* this flow made it */
    JS_FreeCString(ctx, s);
    DCHECK(t != NULL, "createTextNode produced no node — a page building its DOM would silently build nothing");
    return node_wrap(ctx, lxb_dom_interface_node(t));
}

/* §4.5 `new Document()` — "set this's origin to the origin of current global object's associated Document",
   and nothing else: the document it makes has NO tree, no doctype and no document element, its URL is
   `about:blank` and its content type is `application/xml`. It is the ONE way a page gets an XML document
   without going through DOMImplementation, and dom/common.js opens with it.
   The Document is built here rather than by the interface object's shared throw because §4.5's IDL declares a
   constructor; `node_install_interface` is for the interfaces that declare none. */
static JSValue js_doc_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv, int magic)
{
    lxb_html_document_t *dom;

    (void)new_target; (void)argc; (void)argv; (void)magic;
    dom = lxb_html_document_create();
    CHECK(dom != NULL, "new Document(): OOM building a second Document");
    /* The ORIGIN is the constructing realm's document's, which document_new takes from the realm it runs in —
       the same rule createDocument's step 6 states, and the reason this is not `document_install`. */
    return document_new(ctx, dom, "about:blank", "application/xml");
}

/* §4.5.1 `createCDATASection(data)` and `createProcessingInstruction(target, data)` — the two node factories
   an XML document has and an HTML one does not, and the reason they are here rather than absent: dom/common.js
   builds `paras[5]` out of two CDATA sections and `xmlDoc` out of two processing instructions, so EVERY
   §5 and §6 test file that includes it threw inside its own setup and reported zero subtests. That is the
   excluded-test defect wearing a page's TypeError: fifteen files ERRORed at load, and the count looked like
   fifteen rather than like the hundreds of subtests they contain.
   magic 0 = createCDATASection, 1 = createProcessingInstruction. */
static JSValue js_doc_create_xml_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    Document *d = doc_receiver(ctx, this_val);
    lxb_dom_document_t *dom;
    const char *data, *target = NULL;
    size_t dlen = 0, tlen = 0;
    lxb_dom_node_t *made = NULL;
    JSValue r;

    if (!d) return JS_EXCEPTION;
    dom = lxb_dom_interface_document(d->dom);
    if (magic == 0) {
        /* STEP 1. `createCDATASection` on an HTML document is a NotSupportedError — this engine's documents
           are HTML unless createDocument or `new Document()` made them, and the content type is what says so.
           §4.5's own words are "if this is an HTML document"; the content type is how a Document records it. */
        if (!strcmp(d->content_type, "text/html"))
            return JS_ThrowDOMException(ctx, "NotSupportedError",
                                        "an HTML document has no CDATA sections");
    } else {
        target = JS_ToCStringLen(ctx, &tlen, argv[0]);
        if (!target) return JS_EXCEPTION;
    }
    data = JS_ToCStringLen(ctx, &dlen, argv[magic == 0 ? 0 : 1]);
    if (!data) { if (target) JS_FreeCString(ctx, target); return JS_EXCEPTION; }
    /* STEP 2 in both algorithms: the one sequence the node's own serialization cannot survive. */
    if (magic == 0 ? (strstr(data, "]]>") != NULL) : (strstr(data, "?>") != NULL)) {
        JS_FreeCString(ctx, data);
        if (target) JS_FreeCString(ctx, target);
        return JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                    magic == 0 ? "a CDATA section cannot contain \"]]>\""
                                               : "a processing instruction cannot contain \"?>\"");
    }
    if (magic == 0) {
        lxb_dom_cdata_section_t *c =
            lxb_dom_document_create_cdata_section(dom, (const lxb_char_t *)data, dlen);
        CHECK(c != NULL, "createCDATASection: the Lexbor node allocation failed");
        made = lxb_dom_interface_node(c);
    } else {
        lxb_dom_processing_instruction_t *pi =
            lxb_dom_document_create_processing_instruction(dom, (const lxb_char_t *)target, tlen,
                                                           (const lxb_char_t *)data, dlen);
        CHECK(pi != NULL, "createProcessingInstruction: the Lexbor node allocation failed");
        made = lxb_dom_interface_node(pi);
    }
    dom_cow_note_created(made);   /* this flow made it; detached until the page inserts it */
    JS_FreeCString(ctx, data);
    if (target) JS_FreeCString(ctx, target);
    r = node_wrap(ctx, made);
    return r;
}

static JSValue js_doc_create_comment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *s;
    size_t len = 0;
    lxb_dom_comment_t *c;
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    s = argc >= 1 ? JS_ToCStringLen(ctx, &len, argv[0]) : JS_ToCStringLen(ctx, &len, JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    c = lxb_dom_document_create_comment(lxb_dom_interface_document(d->dom), (const lxb_char_t *)s, len);
    dom_cow_note_created(c ? lxb_dom_interface_node(c) : NULL);   /* this flow made it */
    JS_FreeCString(ctx, s);
    DCHECK(c != NULL, "createComment produced no node — a page building its DOM would silently build nothing");
    return node_wrap(ctx, lxb_dom_interface_node(c));
}

/* DOM 4.5.3 "validate and extract" — the whole of it, because every one of its failures is a DOMException the
   spec names and a page catches. `""` MEANS NULL: a namespace of the empty string is set to null before
   anything else, and skipping that step handed Lexbor a zero-length namespace it refuses, which is what made
   `document.createElementNS("", "div")` — a shape five WPT documents open with — produce no element at all.
   Returns 0 and leaves an exception pending on failure; on success *local points into qname. */
static int validate_and_extract(JSContext *ctx, const char **ns, const char *qname,
                                const char **local, size_t *prefix_len)
{
    static const char XML_NS[]   = "http://www.w3.org/XML/1998/namespace";
    static const char XMLNS_NS[] = "http://www.w3.org/2000/xmlns/";
    const char *colon;

    if (*ns && **ns == 0)
        *ns = NULL;                       /* 1. "If namespace is the empty string, set it to null." */
    if (!*qname) {
        JS_ThrowDOMException(ctx, "InvalidCharacterError", "the qualified name is empty");
        return 0;
    }
    colon = strchr(qname, ':');
    *local = colon ? colon + 1 : qname;
    *prefix_len = colon ? (size_t)(colon - qname) : 0;
    if (colon && (*prefix_len == 0 || **local == 0 || strchr(*local, ':'))) {
        JS_ThrowDOMException(ctx, "InvalidCharacterError", "'%s' is not a valid qualified name", qname);
        return 0;
    }
    if (colon && !*ns) {
        JS_ThrowDOMException(ctx, "NamespaceError", "a prefixed name needs a namespace");
        return 0;
    }
    if (colon && *prefix_len == 3 && memcmp(qname, "xml", 3) == 0 && strcmp(*ns, XML_NS) != 0) {
        JS_ThrowDOMException(ctx, "NamespaceError", "the xml prefix is bound to the XML namespace");
        return 0;
    }
    {
        int q_is_xmlns = strcmp(qname, "xmlns") == 0;
        int p_is_xmlns = colon && *prefix_len == 5 && memcmp(qname, "xmlns", 5) == 0;
        if ((q_is_xmlns || p_is_xmlns) && (!*ns || strcmp(*ns, XMLNS_NS) != 0)) {
            JS_ThrowDOMException(ctx, "NamespaceError", "xmlns is bound to the XMLNS namespace");
            return 0;
        }
        if (*ns && strcmp(*ns, XMLNS_NS) == 0 && !q_is_xmlns && !p_is_xmlns) {
            JS_ThrowDOMException(ctx, "NamespaceError", "the XMLNS namespace only binds xmlns");
            return 0;
        }
    }
    return 1;
}

/* 4.5.3 createElementNS(namespace, qualifiedName). createElement's element creation over the validated triple —
   the element carries the NAMESPACE the page asked for, so `el.namespaceURI` and a namespaced selector answer
   what they should. testharness.js reaches it on every completed document (`createElementNS(xhtml_ns, "style")`
   in Output.show_results), and a missing one threw inside the completion-callback list, silencing documents
   whose tests had all already run. */
static JSValue js_doc_create_element_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    const char *ns = NULL, *qname, *local;
    size_t prefix_len = 0;
    lxb_dom_element_t *el;
    Document *d = doc_receiver(ctx, this_val);
    JSValue r;

    if (!d) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "createElementNS requires a namespace and a qualified name");
    if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0])) {
        ns = JS_ToCString(ctx, argv[0]);
        if (!ns) return JS_EXCEPTION;
    }
    qname = JS_ToCString(ctx, argv[1]);
    if (!qname) { if (ns) JS_FreeCString(ctx, ns); return JS_EXCEPTION; }

    {
        const char *ns_in = ns;   /* validate_and_extract may null it; the ORIGINAL is what must be freed */
        if (!validate_and_extract(ctx, &ns, qname, &local, &prefix_len)) {
            if (ns_in) JS_FreeCString(ctx, ns_in);
            JS_FreeCString(ctx, qname);
            return JS_EXCEPTION;
        }
        el = lxb_dom_element_create(lxb_dom_interface_document(d->dom),
                                    (const lxb_char_t *)local, strlen(local),
                                    (const lxb_char_t *)ns, ns ? strlen(ns) : 0,
                                    prefix_len ? (const lxb_char_t *)qname : NULL, prefix_len,
                                    NULL, 0, false);
        DCHECK(el != NULL, "createElementNS produced no element for a name the spec accepts — a page building "
                           "its DOM would silently build nothing and every query after it would answer null");
        if (ns_in) JS_FreeCString(ctx, ns_in);
    }
    JS_FreeCString(ctx, qname);
    r = element_wrap(ctx, el);
    return r;
}

/* §4.5's "INTERNAL createElementNS STEPS", named as the spec names them because a second caller reaches them:
   §4.5.1's createDocument step 3 is "the internal createElementNS steps, GIVEN DOCUMENT" — the new document,
   not the one whose implementation was asked. One implementation of validate-and-extract and of the element
   creation, rather than a second one over there that could disagree about a prefix. */
JSValue document_create_element_ns(JSContext *ctx, JSValueConst doc, int argc, JSValueConst *argv)
{
    return js_doc_create_element_ns(ctx, doc, argc, argv, 0);
}

/* §4.5's `[SameObject] readonly attribute DOMImplementation implementation`. SameObject is the whole reason the
   object lives on the document's record: a page holds it and calls it later, and a fresh one per read would
   compare unequal to the one it kept. */
static JSValue js_doc_implementation(JSContext *ctx, JSValueConst this_val, int magic)
{
    Document *d = doc_receiver(ctx, this_val);

    (void)magic;
    if (!d) return JS_EXCEPTION;
    DCHECK(JS_IsObject(d->impl),
           "a document answered for its `implementation` with no object — it is built WITH the document, so a "
           "document that has one and cannot say so came from neither document_install nor document_new");
    return JS_DupValue(ctx, d->impl);
}

/* THE DOCUMENT'S LOAD LIFECYCLE. Stage 0 is DOMContentLoaded — fired at the DOCUMENT and bubbling to window,
   which is where a page registers it — and stage 1 is load, fired at window. `readyState` moves with them,
   because a page that missed the event reads it instead. Both are per-FLOW: the scheduler asks once per stage
   for each flow that has run everything the document gave it, so an arm that reached the end of the document
   fires its own listeners in its own world. */
/* The parsed document's ROOT node — what a whole-tree walk starts from. One component owns which document a
   realm parsed; a second copy of that pointer is how the two drift apart. NULL before the install, which is a
   real state a Window member reads (`window.document` before there is one). */
lxb_dom_node_t *document_root_node(JSContext *ctx)
{
    Document *d = doc_of(ctx);
    return d ? lxb_dom_interface_node(d->dom->dom_document.element) : NULL;
}

const char *document_base_url(JSContext *ctx)
{
    Document *d = doc_here(ctx);
    DCHECK(d->url[0] != '\0', "a node's baseURI was read before the document was installed");
    return d->url;
}

/* HTML's CURRENT DOCUMENT READINESS, and the reason it is not simply `readyState`. §3.1.1 declares readyState
   a READONLY attribute; this engine has it as a data property, so a page can assign it — and the load
   lifecycle and §8.1.7.3 step 3's render-blocked test both READ it, which would let
   `document.readyState = "complete"` skip a document's DOMContentLoaded and unblock its rendering.
 *
 * IT LIVES IN THIS REALM'S OWN BASELINE RECORD, the same shape §8.9's map of animation frame callbacks and
 * §7.4.6.3's has-been-revealed use, and for the same two reasons: the record is unreachable from the page, so
 * nothing can write the readiness but this component; and its `stage` is an ordinary property write, so the
 * heap COW captures it and one arm of a fork advances its lifecycle without touching its sibling's.
 * A private Symbol on the Document would have done the first job and not the second cleanly — and it would
 * have been an AGENT-wide value freed by a PER-REALM teardown (document_free runs once per realm, from
 * navigable.c's realm sweep), which is one document dropping the key the others still read through. */
static int g_ready_slot = -1;

static void document_set_ready(JSContext *ctx, int stage)
{
    Document *d = doc_here(ctx);
    static const char *const NAMES[3] = { "loading", "interactive", "complete" };
    JSValue rec;

    DCHECK(stage >= 0 && stage <= 2, "a document readiness HTML does not define");
    rec = realm_value_get(ctx, g_ready_slot);
    DCHECK(JS_IsObject(rec), "a realm answered for its document readiness with no record");
    JS_SetPropertyStr(ctx, rec, "stage", JS_NewInt32(ctx, stage));
    JS_FreeValue(ctx, rec);
    if (JS_IsObject(d->doc_obj))   /* `readyState` REFLECTS it, for the page that missed the event */
        JS_SetPropertyStr(ctx, d->doc_obj, "readyState", JS_NewString(ctx, NAMES[stage]));
}

/* This document's readiness: 0 loading, 1 interactive, 2 complete. */
static int document_readiness(JSContext *ctx)
{
    JSValue rec = realm_value_get(ctx, g_ready_slot), v;
    int32_t r = 0;

    DCHECK(JS_IsObject(rec), "a realm answered for its document readiness with no record");
    v = JS_GetPropertyStr(ctx, rec, "stage");
    JS_ToInt32(ctx, &r, v);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, rec);
    DCHECK(r >= 0 && r <= 2, "a document's readiness record holds a stage HTML does not define");
    return r;
}

/* HTML §8.1.7.3 "update the rendering" step 3's RENDER-BLOCKED clause, answered by the component that owns the
   document's load lifecycle rather than guessed by the one that runs the steps.
 *
 * A Document is render-blocked while it has render-blocking elements, and the parser is what removes the last
 * of them: a browser does not present a document, and does not reveal it, until its parse has finished. In
 * this engine the tree is one Lexbor parse and the parser's completion IS stage 0 — the moment the readiness
 * leaves "loading" and DOMContentLoaded fires. It is what puts the first rendering opportunity (and therefore
 * `pagereveal` and the first animation frame) AFTER DOMContentLoaded, which is where a browser puts it. */
bool document_render_blocked(JSContext *ctx)
{
    return document_readiness(ctx) == 0;
}

static int document_done_stage(JSContext *ctx, int stage)
{
    if (stage == 0) {
        document_set_ready(ctx, 1);
        /* §3.1.1: DOMContentLoaded is fired AT THE DOCUMENT and BUBBLES, which is how a `window.onload`-style
           listener registered on window hears it — the propagation path derives that from the document's
           ancestors now rather than the caller naming the window. It is not cancelable. */
        event_target_fire(ctx, doc_here(ctx)->doc_obj,
                          event_new(ctx, "DOMContentLoaded", /*bubbles*/ true, /*cancelable*/ false));
        return 1;
    }
    DCHECK(stage == 1, "the document lifecycle was asked for a stage it does not have");
    document_set_ready(ctx, 2);
    /* HTML: `load` is fired at the WINDOW and does not bubble — there is nothing above it to bubble to. */
    event_target_fire(ctx, doc_here(ctx)->win_obj,
                      event_new(ctx, "load", /*bubbles*/ false, /*cancelable*/ false));
    return 1;
}

/* THE LOAD LIFECYCLE IS PER DOCUMENT, and it was per FLOW — one counter, for one document, driven with the
 * ROOT realm's ctx. That is not a small mismatch: HTML gives every Document its own readiness and its own
 * DOMContentLoaded, so a CHILD navigable's document could never leave "loading". It never fired
 * DOMContentLoaded, never fired `load`, and — the moment §8.1.7.3 step 3's render-blocked clause existed — was
 * removed from every rendering opportunity for ever. A child document simply never ran the half of its code
 * that is behind those events, and nothing anywhere said so; wpt's
 * animation-frames/callback-cross-realm-report-exception is one test that says it out loud.
 *
 * SO THE STAGE IS READ FROM THE DOCUMENT, and the flow's counter is DELETED. The readiness is already
 * per-document and per-flow (an own slot on each realm's Document, isolated by the COW delta), which is
 * exactly what the counter was trying to be and could not be, because one integer cannot hold N documents.
 *
 * THE ORDER IS THE SPEC'S, and it is two passes rather than one. Every document's DOMContentLoaded comes
 * before any document's `load` — a parent's parse finishes while its frames are still loading — and a CHILD's
 * `load` fires before its PARENT's, because a parent's load waits for its subframes. Tree order gives the
 * first; the REVERSE of tree order gives the second, since a container precedes what it contains.
 *
 * ONE DOCUMENT PER CALL, then return, because this is a work item on the one frontier like everything else the
 * scheduler asks for — each fire queues listener tasks the loop picks up before the next document's stage.
 * Returns 1 when it advanced one, 0 when every document of this agent is complete. */
int document_lifecycle_step(JSContext *ctx)
{
    JSValue docs = navigable_tree_order(ctx), len;
    uint32_t n = 0, i;
    int did = 0;

    len = JS_GetPropertyStr(ctx, docs, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n && !did; i++) {          /* pass one: DOMContentLoaded, in tree order */
        JSValue proxy = JS_GetPropertyUint32(ctx, docs, i);
        JSContext *realm = window_proxy_realm(ctx, proxy);
        if (document_readiness(realm) == 0) {
            document_done_stage(realm, 0);
            DCHECK(document_readiness(realm) == 1,
                   "a document's DOMContentLoaded stage ran and left its readiness where it was — this walk "
                   "would then pick the same document for ever, queueing its listeners again every turn, which "
                   "is a live-lock the scheduler cannot tell from progress");
            did = 1;
        }
        JS_FreeValue(ctx, proxy);
    }
    for (i = n; i > 0 && !did; i--) {          /* pass two: `load`, innermost first */
        JSValue proxy = JS_GetPropertyUint32(ctx, docs, i - 1);
        JSContext *realm = window_proxy_realm(ctx, proxy);
        if (document_readiness(realm) == 1) {
            document_done_stage(realm, 1);
            DCHECK(document_readiness(realm) == 2,
                   "a document's `load` stage ran and left its readiness where it was — as above, the walk "
                   "would re-fire it every turn");
            did = 1;
        }
        JS_FreeValue(ctx, proxy);
    }
    JS_FreeValue(ctx, docs);
    return did;
}

/* §3.1.1's `location` — the LOCATION OBJECT OF THIS DOCUMENT'S RELEVANT GLOBAL. It was absent, and absent is
   not a small gap here: `document.location.pathname` is how WPT's own /common/PrefixedPostMessage.js names a
   message channel, so 63 subtests across html/browsers failed on a property of undefined without ever reaching
   what they were testing. The IDL audit had it listed among Document's absent members the whole time.
   IT IS THE GLOBAL'S, not a second Location: a document and its window are one browsing context and §3.1.1 says
   "the Location object of this's relevant global object", so this reads the one location.c installed rather
   than building another that would compare unequal to it.
   NULL WHEN THE DOCUMENT IS NOT FULLY ACTIVE, which is what §3.1.1's `Location?` is for — and it is also the
   honest answer for a host that installed no Location at all (one whose document has no address), where
   inventing an object would claim an address the engine was never given. */
static JSValue js_doc_location(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue g, loc;

    (void)magic;
    /* §3.1.1: null when this document is not FULLY ACTIVE — and a document §4.5 created has no browsing context
       at all, so it can never be. It was the realm's `location` unconditionally, which handed a page an address
       belonging to a different document the moment a second one existed. */
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    if (JS_IsUndefined(d->proxy))
        return JS_NULL;
    g = JS_GetGlobalObject(ctx);
    loc = JS_GetPropertyStr(ctx, g, "location");
    JS_FreeValue(ctx, g);
    if (JS_IsUndefined(loc)) { JS_FreeValue(ctx, loc); return JS_NULL; }
    return loc;
}


/* §3.1.1's TREE ENTRY POINTS, COMPUTED FROM THE RECEIVER'S TREE — documentElement, body, head and doctype.
 *
 * They were four DATA PROPERTIES latched onto the `document` object at install, and that is wrong twice. It is
 * wrong in TIME: each is defined as a lookup in the tree AS IT IS, so a page that replaces `<body>` (which
 * `document.body = el` and a `replaceChild` both do) went on being handed the node the parse produced. And it
 * is wrong in SUBJECT: a value stored on one object cannot answer for a second document, so
 * `implementation.createHTMLDocument("").body` was undefined — the whole reason this had to be looked at.
 * magic 0 = documentElement, 1 = body, 2 = head, 3 = doctype. */
static lxb_dom_node_t *doc_child_named(lxb_dom_node_t *parent, const char *a, const char *b)
{
    lxb_dom_node_t *n;

    for (n = parent ? parent->first_child : NULL; n; n = n->next) {
        size_t qn = 0;
        const lxb_char_t *q;
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        q = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &qn);
        if (!q) continue;
        if ((qn == strlen(a) && !memcmp(q, a, qn)) || (b && qn == strlen(b) && !memcmp(q, b, qn)))
            return n;
    }
    return NULL;
}

/* §4.5 "document element": the ELEMENT child of the document. There is at most one, and NULL is a real answer —
   a document §4.5's createDocument built with no qualified name has none. */
static lxb_dom_node_t *doc_element_of(const lxb_dom_node_t *doc)
{
    lxb_dom_node_t *n;

    if (!doc || doc->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return NULL;
    for (n = doc->first_child; n; n = n->next)
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT)
            return n;
    return NULL;
}

/* DOM §2.7's DEFAULT PASSIVE VALUE names four targets, and three of them are this file's definitions: the node
   DOCUMENT itself, its document element, and its body. The fourth is the Window, which is not a node and is
   answered where the registration is. It lives here so the two §3.1.1 lookups have one implementation. */
bool document_is_passive_default_node(const lxb_dom_node_t *n)
{
    lxb_dom_node_t *doc;

    if (!n)
        return false;
    if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT)
        return true;
    doc = n->owner_document ? lxb_dom_interface_node(n->owner_document) : NULL;
    return n == doc_element_of(doc) || n == doc_child_named(doc_element_of(doc), "body", "frameset");
}

/* HTML's "the document's relevant global object", which §2.9's get the parent puts above a Document in the
   event path. BORROWED, and JS_NULL for a document with no browsing context — one `createHTMLDocument` built,
   whose events therefore stop at the document exactly as the spec says. */
JSValueConst document_window_of(const lxb_dom_node_t *n)
{
    Document *d = n ? doc_rec(n->owner_document) : NULL;

    if (!d || !JS_IsObject(d->win_obj))
        return JS_NULL;
    return d->win_obj;
}

static JSValue js_doc_tree(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_node_t *doc = node_of(this_val), *root, *n;

    /* WEB IDL §3.7.5's brand check — a TypeError, not an assert; see doc_receiver. */
    if (!doc || doc->type != LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_ThrowTypeError(ctx, "this is not a Document");
    root = doc_element_of(doc);
    switch (magic) {
    case 0:
        return node_wrap(ctx, root);
    case 1:
        /* §3.1.1: "the first of the html element's children that is either a BODY or a FRAMESET element, or
           null" — a frameset document has no body at all, which is the parser following the spec. */
        return node_wrap(ctx, doc_child_named(root, "body", "frameset"));
    case 2:
        /* §3.1.1: "the first head element that is a child of the html element". */
        return node_wrap(ctx, doc_child_named(root, "head", NULL));
    default:
        DCHECK(magic == 3, "a Document tree accessor was declared with a magic this table does not name");
        /* §4.5 doctype: "the first DocumentType node child, in tree order, or null". */
        for (n = doc->first_child; n; n = n->next)
            if (n->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE) return node_wrap(ctx, n);
        return JS_NULL;
    }
}

/* §4.5 / §3.1.1's PER-DOCUMENT STRINGS. Every one is a fact about the receiver rather than about the realm, and
   every one was a data property latched at install and therefore absent on any other document.
   magic 0 = URL and documentURI (§4.5 defines the second as an alias of the first), 1 = contentType,
   2 = compatMode, 3 = characterSet / charset / inputEncoding. */
static JSValue js_doc_strings(JSContext *ctx, JSValueConst this_val, int magic)
{
    Document *d = doc_receiver(ctx, this_val);

    if (!d) return JS_EXCEPTION;
    switch (magic) {
    case 0:
        return JS_NewString(ctx, d->url);
    case 1:
        return JS_NewString(ctx, d->content_type);
    case 2:
        /* §4.5: "BackCompat" if the document is in quirks mode, "CSS1Compat" otherwise — the PARSER's answer,
           read off the tree it built rather than assumed. */
        return JS_NewString(ctx, d->dom->dom_document.compat_mode == LXB_DOM_DOCUMENT_CMODE_QUIRKS
                                     ? "BackCompat" : "CSS1Compat");
    default:
        DCHECK(magic == 3, "a Document string accessor was declared with a magic this table does not name");
        /* §4.5's encoding trio. This engine decodes every document as UTF-8, so that is the real answer and not
           a placeholder; `charset` and `inputEncoding` are the spec's own historical aliases of `characterSet`
           and are the SAME getter rather than three that could disagree. */
        return JS_NewString(ctx, "UTF-8");
    }
}

/* §4.5's TWO TRAVERSER FACTORIES. `createNodeIterator(root, whatToShow, filter)` and `createTreeWalker(...)`
   are the same five-line construction with a different object at the end, so they are one body with a magic —
   and neither runs a line of the page's code once its arguments are converted, which is why they are plain C
   and their MEMBERS are machines. The IDL is what does the work: `Node root` is an interface type (a non-Node
   is a TypeError before step 1), `optional unsigned long whatToShow = 0xFFFFFFFF` is ToNumber and a modulo, and
   `optional NodeFilter? filter = null` accepts a function OR any object and rejects a primitive.
   magic 0 = createNodeIterator, 1 = createTreeWalker. */
static JSValue js_doc_create_traverser(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int magic)
{
    JSValueConst root = argc > 0 ? argv[0] : JS_UNDEFINED, filter = JS_NULL;
    uint32_t what = 0xFFFFFFFFu;

    DCHECK(node_of(this_val) != NULL, "a traverser factory ran on something that is not a document");
    if (!node_of(root))
        return JS_ThrowTypeError(ctx, "createNodeIterator/createTreeWalker requires a Node root");
    /* The IDL's defaults. An absent optional argument arrives as undefined, which is what §3.6.2 means by
       absent — so the default is applied here and nowhere else. */
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToUint32(ctx, &what, argv[1]) < 0)
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2])) filter = argv[2];
    return magic == 0 ? node_iterator_new(ctx, root, what, filter)
                      : tree_walker_new(ctx, root, what, filter);
}

/* §4.5 `[NewObject] Range createRange()` — "a new live range with (this, 0) as its start and end". It is the
   same construction `new Range()` performs, and it is the Document's rather than the current global's: a page
   that calls `otherDoc.createRange()` gets a range rooted in THAT document. */
static JSValue js_doc_create_range(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    (void)argc; (void)argv; (void)magic;
    DCHECK(node_of(this_val) != NULL, "createRange ran on something that is not a document");
    return range_new_at(ctx, this_val);
}

/* §4.5 `[NewObject] Event createEvent(DOMString interface)` — the legacy factory. The TABLE and the
   construction belong to the events component; what is Document's is the member and the realm whose interfaces
   the exposure check asks about, which is this document's global. */
static JSValue js_doc_create_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic)
{
    JSValue global, r;
    const char *iface;

    (void)this_val; (void)argc; (void)magic;
    iface = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!iface) return JS_EXCEPTION;
    global = JS_GetGlobalObject(ctx);
    r = create_event(ctx, global, iface);
    JS_FreeValue(ctx, global);
    JS_FreeCString(ctx, iface);
    return r;
}

/* The Document METHODS — on Document.prototype, so there is one of each rather than one per install, and so
   `Document.prototype.querySelector` is a thing that exists. */
/* THE DECLARATIONS ARE THE AGENT'S, THE INSTALLS ARE THE REALM'S — the IDL pool is sealed after agent init, so
   a declaration minted from a per-realm install trips idl_declared_before_seal on the SECOND realm. */
static JSClassID g_document_class;   /* §3.1.1's prototype slot, per realm */
static int g_id_create_element = -1, g_id_create_text = -1, g_id_create_comment = -1,
           g_id_create_fragment = -1, g_id_create_element_ns = -1, g_id_create_iterator = -1,
           g_id_create_walker = -1, g_id_create_range = -1, g_id_create_event = -1,
           g_id_create_cdata = -1, g_id_create_pi = -1, g_id_doc_ctor = -1;

static void document_declare_members(JSContext *ctx)
{
    /* §4.5's `[CEReactions, NewObject] Element createElement(DOMString localName, …)` — a STEP because §4.9
       step 5.1.4.1 constructs the page's custom element class synchronously inside it. */
    g_id_create_element = idl_method_id_step(ctx, IDL_1STR, 1, NULL, 0, &DOC_CREATE_EL_STEP, 0);
    g_id_create_text = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_text, 0);
    g_id_create_comment = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_comment, 0);
    g_id_create_cdata = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_xml_node, 0);
    g_id_create_pi = idl_method_id(ctx, IDL_2STR, 2, js_doc_create_xml_node, 1);
    /* §4.5's `[Exposed=Window] interface Document { constructor(); }` — the interface object is CONSTRUCTIBLE,
       which it was not, and `new Document()` is how a page gets an XML document without DOMImplementation. */
    g_id_doc_ctor = idl_method_id(ctx, NULL, 0, js_doc_ctor, 0);
    g_id_create_fragment = idl_method_id(ctx, NULL, 0, js_doc_create_fragment, 0);
    g_id_create_element_ns = idl_method_id(ctx, IDL_2STR, 2, js_doc_create_element_ns, 0);
    {
        /* §4.5: `(Node root, optional unsigned long whatToShow = 0xFFFFFFFF, optional NodeFilter? filter =
           null)`, twice. */
        static const IdlArgType TRAVERSER[3] = { IDL_INTERFACE, IDL_UNSIGNED_LONG,
                                                 IDL_CALLBACK_INTERFACE_NULLABLE };
        g_id_create_iterator = idl_method_id(ctx, TRAVERSER, 3, js_doc_create_traverser, 0);
        idl_iface_brand(node_class_id());
        idl_optional_from(1);
        g_id_create_walker = idl_method_id(ctx, TRAVERSER, 3, js_doc_create_traverser, 1);
        idl_iface_brand(node_class_id());
        idl_optional_from(1);
    }
    g_id_create_range = idl_method_id(ctx, NULL, 0, js_doc_create_range, 0);
    g_id_create_event = idl_method_id(ctx, IDL_1STR, 1, js_doc_create_event, 0);
}

static void document_install_members(JSContext *ctx, JSValueConst proto)
{
    idl_install_method(ctx, proto, "createElement", 1, g_id_create_element);
    idl_install_method(ctx, proto, "createTextNode", 1, g_id_create_text);
    idl_install_method(ctx, proto, "createComment", 1, g_id_create_comment);
    idl_install_method(ctx, proto, "createCDATASection", 1, g_id_create_cdata);
    idl_install_method(ctx, proto, "createProcessingInstruction", 2, g_id_create_pi);
    idl_install_method(ctx, proto, "createDocumentFragment", 0, g_id_create_fragment);
    {
        /* §3.1.5's five element shortcuts, each a LIVE HTMLCollection over the document. */
        static const char *const NAMES[] = { "forms", "images", "scripts", "embeds", "links" };
        unsigned k;
        for (k = 0; k < sizeof(NAMES) / sizeof(NAMES[0]); k++)
            idl_install_accessor(ctx, proto, NAMES[k], js_doc_shortcut, (int)k, -1);
    }
    idl_install_method(ctx, proto, "createElementNS", 2, g_id_create_element_ns);
    /* §4.5's two ATTRIBUTE factories, declared beside the interface they build (attr.c) — "create an attribute"
       is §4.9.2's algorithm and belongs to the attribute component, not to a second copy of it here. */
    attr_install_document_members(ctx, proto);
    idl_install_method(ctx, proto, "createNodeIterator", 1, g_id_create_iterator);
    idl_install_method(ctx, proto, "createTreeWalker", 1, g_id_create_walker);
    idl_install_method(ctx, proto, "createRange", 0, g_id_create_range);
    idl_install_method(ctx, proto, "createEvent", 1, g_id_create_event);
    /* §3.1.1: `[PutForwards=href] readonly attribute Location? location`. The forwarding half of the extended
       attribute — `document.location = url` navigating — is NOT built, and it is absent rather than silently
       dropped: a setter that stored a string would make a page believe it had navigated. */
    idl_install_accessor(ctx, proto, "location", js_doc_location, 0, -1);
    /* §3.1.1's TREE ENTRY POINTS and §4.5's per-document strings. They were data properties latched onto ONE
       document object at install — wrong in time (each is a lookup in the tree as it IS) and wrong in subject
       (a value stored on one object cannot answer for a second document). */
    idl_install_accessor(ctx, proto, "documentElement", js_doc_tree, 0, -1);
    idl_install_accessor(ctx, proto, "body",           js_doc_tree, 1, -1);
    idl_install_accessor(ctx, proto, "head",           js_doc_tree, 2, -1);
    idl_install_accessor(ctx, proto, "doctype",        js_doc_tree, 3, -1);
    idl_install_accessor(ctx, proto, "URL",            js_doc_strings, 0, -1);
    idl_install_accessor(ctx, proto, "documentURI",    js_doc_strings, 0, -1);
    idl_install_accessor(ctx, proto, "contentType",    js_doc_strings, 1, -1);
    idl_install_accessor(ctx, proto, "compatMode",     js_doc_strings, 2, -1);
    idl_install_accessor(ctx, proto, "characterSet",   js_doc_strings, 3, -1);
    idl_install_accessor(ctx, proto, "charset",        js_doc_strings, 3, -1);
    idl_install_accessor(ctx, proto, "inputEncoding",  js_doc_strings, 3, -1);
    idl_install_accessor(ctx, proto, "implementation", js_doc_implementation, 0, -1);
}

/* HTML §7.2.6's container for THIS document, from BOTH halves of the policy list.
   `csp` IS WHAT THE DOCUMENT WAS CREATED WITH — the response's `Content-Security-Policy` header, or §7.4's
   clone of the creator's for a document that came from no response — and it used to arrive nowhere: the
   trusted zone captured the header, handed it to the engine, and the engine's entry point cast it to `(void)`.
   So every document was judged against its `<meta>` policies alone, and a sink that the page's real policy
   kills was reported as a working exploit — the exact false PoC §@S exists to never emit.
   The meta half is a REAL LEXBOR WALK, not a regex over the source: a `content` attribute is parsed markup by
   the time it is here, so entity decoding and quoting are the parser's answer rather than a second one — the
   same reason the bundle id is a `<script>` scan. */
PolicyContainer *document_policy_new(lxb_html_document_t *dom, const char *csp)
{
    lxb_dom_node_t *cur;
    char *acc = NULL;
    size_t acc_len = 0;

    /* THE CREATED-WITH POLICIES COME FIRST, because they were delivered first; every policy in a list is
       enforced so the order changes no verdict, but a container that reports its own text should report it in
       the order the document received it. */
    if (csp && *csp) {
        acc_len = strlen(csp);
        acc = malloc(acc_len + 1);
        CHECK(acc != NULL, "document: OOM holding the policy this document was created with");
        memcpy(acc, csp, acc_len + 1);
    }

    /* No guard for a missing tree: document_install has already asserted there is one, and a second, softer
       answer here would be the defensive branch that hides the case the assert exists to catch. */
    for (cur = lxb_dom_interface_node(dom)->first_child; cur; ) {
        if (cur->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t qn = 0;
            const lxb_char_t *q = lxb_dom_element_qualified_name((lxb_dom_element_t *)cur, &qn);
            if (q && qn == 4 && !memcmp(q, "meta", 4)) {
                size_t hl = 0, cl = 0;
                const lxb_char_t *he = lxb_dom_element_get_attribute((lxb_dom_element_t *)cur,
                                                                     (const lxb_char_t *)"http-equiv", 10, &hl);
                /* The equivalence is ASCII case-insensitive, which is how every real page spells it. */
                if (he && hl == 23 && !strncasecmp((const char *)he, "content-security-policy", 23)) {
                    const lxb_char_t *cv = lxb_dom_element_get_attribute((lxb_dom_element_t *)cur,
                                                                         (const lxb_char_t *)"content", 7, &cl);
                    if (cv && cl) {
                        /* SEVERAL META POLICIES ALL APPLY, and they are joined with a COMMA because that is
                           CSP §2.2's serialization of a policy LIST. A ';' join would have made them one
                           policy, where a repeated directive is ignored and `script-src` overrides
                           `default-src` — so the narrowing second policy would have silently vanished. */
                        size_t add = cl + (acc_len ? 1 : 0);
                        char *g = realloc(acc, acc_len + add + 1);
                        CHECK(g != NULL, "document: OOM collecting a policy");
                        acc = g;
                        if (acc_len) acc[acc_len++] = ',';
                        memcpy(acc + acc_len, cv, cl);
                        acc_len += cl;
                        acc[acc_len] = 0;
                    }
                }
            }
        }
        if (cur->first_child) { cur = cur->first_child; continue; }
        while (cur && !cur->next) cur = cur->parent;
        if (cur) cur = cur->next;
    }
    {
        PolicyContainer *p = policy_container_new(acc, NULL);
        free(acc);
        return p;
    }
}

const PolicyContainer *document_policy(JSContext *ctx) { return doc_here(ctx)->policy; }

JSValueConst document_window_proxy(JSContext *ctx)
{
    Document *d = doc_here(ctx);
    DCHECK(!JS_IsUndefined(d->proxy), "this realm's WindowProxy was read before its Document was installed — "
                                      "§7.2.5.1 gives a navigable ONE, and it is minted with the realm");
    return d->proxy;
}

/* HTML §7.3.1 "FULLY ACTIVE": a Document is fully active when it is the active document of a navigable, and
 * that navigable is either a top-level traversable or its container document is itself fully active.
 *
 * WHY IT IS ITS OWN QUESTION AND NOT `!closed`. The two differ exactly where it matters: removing an
 * `<iframe>` destroys THAT navigable, and every document nested inside it stops being fully active without
 * anything having been done to its own navigable. So the answer is the WALK the definition states — this
 * navigable and every one containing it — and it is asked, not remembered, because the tree it walks is
 * per-flow: one arm of a fork removed the frame and its sibling did not.
 *
 * WHO ASKS. Every algorithm the standards guard with it, and there is a family of them: the Observable
 * standard opens §2.1's next/error/complete/addTeardown, §2.2.1's subscribe and §3's when() with this exact
 * sentence, and §2.1's close-a-subscription RE-asks it before every teardown because "each teardown could
 * result in the above Document becoming inactive". A detached document must silently do nothing rather than
 * push values into a realm the user agent has discarded. */
bool document_fully_active(JSContext *ctx)
{
    Document *d = doc_here(ctx);
    JSValue cur;
    bool ok = true;

    /* A Document with no navigable at all — `new Document()`, a DOMParser result — has no browsing context, so
       the guard's own premise ("the relevant global object is a Window") is false and the algorithm proceeds.
       The realm this runs in is a Window's, and this realm's proxy is minted with it. */
    if (!window_proxy_is(d->proxy))
        return true;
    cur = JS_DupValue(ctx, d->proxy);
    for (;;) {
        JSValue parent;
        if (window_proxy_closed(ctx, cur)) { ok = false; break; }
        parent = window_proxy_parent(ctx, cur);
        /* §7.3.1's base case is a TOP-LEVEL traversable, and §7.2.5's `parent` of one is the navigable ITSELF —
           so the walk ends when the answer stops moving, or when it is not a navigable's proxy at all (a
           cross-instance parent this agent cannot walk into, which is answered by its own instance). */
        if (!window_proxy_is(parent) ||
            JS_VALUE_GET_PTR(parent) == JS_VALUE_GET_PTR(cur)) {
            JS_FreeValue(ctx, parent);
            break;
        }
        JS_FreeValue(ctx, cur);
        cur = parent;
    }
    JS_FreeValue(ctx, cur);
    return ok;
}

uint32_t document_doc(JSContext *ctx) { return doc_here(ctx)->doc; }

JSValueConst document_object(JSContext *ctx) { return doc_here(ctx)->doc_obj; }

/* DOCUMENT.PROTOTYPE, and the Document as a real NODE. §4.4 `interface Document : Node`, and it was neither —
   a plain JS_NewObject with the members copied onto it. So `document.nodeType` was undefined,
   `document.appendChild` was not a function, `document.contains(el)` (which is how a page asks whether a node
   is still in the tree) was absent, and `document.body.parentNode.parentNode === document` compared a node
   wrapper against something that was not one. It is a node_wrap of the document node now, so it is in the ONE
   identity table with everything else and its members come from a prototype chained to Node.prototype rather
   than being installed per object.
   IT IS THE AGENT'S HALF, with every other prototype: a member is DECLARED once and a declaration builds one
   pool entry, so building this inside the per-document install declared the whole of Document a second time
   for a second realm — which is the shape the pool's seal exists to catch. Web IDL wants the PROTOTYPE per
   realm too; that is the gap this split makes visible, and it is the same one every DOM component has. */
void document_init(JSContext *ctx)
{
    JSClassDef d = { "Document" };

    JS_NewClassID(JS_GetRuntime(ctx), &g_document_class);
    JS_NewClass(JS_GetRuntime(ctx), g_document_class, &d);
    node_claim_type(LXB_DOM_NODE_TYPE_DOCUMENT, g_document_class);
    g_ready_slot = realm_value_declare(ctx, "HTML current document readiness");
    document_declare_members(ctx);
    document_fragment_init(ctx);   /* §4.7, before any fragment is wrapped as a bare Node */
    document_type_init(ctx);       /* §4.6, before the parser's doctype is wrapped as a bare Node */
    dom_implementation_init(ctx);  /* §4.5.1, which every document's record builds one of */
    realm_declare_intrinsic(document_install_proto);
}

/* §3.1.1's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. */
void document_install_proto(JSContext *ctx)
{
    JSValue proto, base, prev;

    prev = JS_GetClassProto(ctx, g_document_class);
    DCHECK(JS_IsNull(prev), "document_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = node_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "Document.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Document");
    document_install_members(ctx, proto);
    /* §3.1.1's IDL includes GlobalEventHandlers and adds onreadystatechange / onvisibilitychange. */
    event_target_install_handlers(ctx, proto, EH_GLOBAL | EH_DOCUMENT);
    /* §4.5: `Document includes ParentNode` — not ChildNode, because a document has no parent to be removed
       from — and `NonElementParentNode`, the same getElementById DocumentFragment includes. */
    node_install_parent_mixin(ctx, proto);
    node_install_nonelement_parent_mixin(ctx, proto);
    /* HTML §6.6's `visibilityState` and `hidden` — one source and the comparison the spec defines over it. */
    page_visibility_install(ctx, proto);
    JS_SetClassProto(ctx, g_document_class, proto);
    /* THIS REALM'S DOCUMENT READINESS, built with the realm so it belongs to the pre-boot BASELINE — the same
       reason §8.9's map and §7.4.6.3's flag are built here. It exists before this realm has a Document at all,
       which is right: "loading" is what a Document that has not been installed yet would answer anyway, and
       the lifecycle walk only ever reaches a realm through its materialized navigable. */
    {
        JSValue rec = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(rec), "this realm's document-readiness record could not be allocated");
        JS_SetPropertyStr(ctx, rec, "stage", JS_NewInt32(ctx, 0));
        realm_value_set(ctx, g_ready_slot, rec);
    }
}

/* A DOCUMENT'S RECORD, AND THE ONE PLACE THE (document -> record) ANSWER IS ESTABLISHED.
 *
 * It lives on the Lexbor document's own `user` slot, which lexbor keeps for its embedder and never reads. That
 * slot used to hold the REALM pointer, which was already the (document -> realm) answer §4.2.3 needs; holding
 * the record instead answers that question and every other per-document one through one indirection, with no
 * registry to keep in step — a registry is a second list of documents whose failure mode is a stale row. */
static Document *doc_rec_new(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *type)
{
    lxb_dom_document_t *dd = lxb_dom_interface_document(dom);
    Document *d;

    DCHECK(dom != NULL, "a document record was built for no document");
    DCHECK(dd->user == NULL, "a second record was built for one Lexbor document — the first would be leaked and "
                             "every node of the tree would answer through whichever won");
    d = calloc(1, sizeof *d);
    CHECK(d != NULL, "document: OOM naming a document");
    d->realm = ctx;
    d->dom = dom;
    d->doc_obj = JS_UNDEFINED;
    d->win_obj = JS_UNDEFINED;
    d->proxy = JS_UNDEFINED;
    d->impl = JS_UNDEFINED;
    snprintf(d->url, sizeof d->url, "%s", url ? url : "");
    snprintf(d->content_type, sizeof d->content_type, "%s", type);
    dd->user = d;
    return d;
}

/* THE RECORD'S HELD REFERENCES GO BACK, and the tree goes with them when this record OWNS it. Every node of a
   document the page created has a wrapper the identity map holds a reference to, so a document freed without
   handing those back leaves the map naming freed memory and the runtime's own leak walk counting its whole
   tree. */
static void doc_rec_release(Document *d)
{
    JSContext *ctx = d->realm;

    dom_implementation_detach(ctx, d->impl);
    JS_FreeValue(ctx, d->impl);
    JS_FreeValue(ctx, d->proxy);
    JS_FreeValue(ctx, d->win_obj);
    JS_FreeValue(ctx, d->doc_obj);
    policy_container_free(d->policy);   /* malloc'd, so the GC walk would never have named it */
    if (d->dom)
        lxb_dom_interface_document(d->dom)->user = NULL;
    free(d);
}

/* THE TREE IS ABOUT TO GO, SO THE RECORD THAT NAMES IT GOES FIRST — called from the ONE place a document's
   lifetime ends. A delta-owned document is destroyed by the flow that made it, and its record is reachable from
   nothing else, so without this the struct and the two references it holds (the wrapper and the
   DOMImplementation) survive every exploration arm that ever created a document. */
void document_record_release(lxb_html_document_t *dom)
{
    Document *d = doc_rec(lxb_dom_interface_document(dom));

    if (!d) return;   /* already released — the realm's own path clears the record before it destroys the tree */
    DCHECK(!d->owned, "a document the REALM owns was destroyed through the delta's path — two owners for one "
                      "tree is one free too many");
    doc_rec_release(d);
}

/* The realm's own path: clear the record FIRST, so the destroy below finds nothing to release and the two
   owners cannot both run. */
static void doc_rec_free(JSContext *ctx, Document *d)
{
    lxb_html_document_t *dom = d->owned ? d->dom : NULL;

    (void)ctx;
    doc_rec_release(d);
    if (dom)
        dom_cow_destroy_document(dom);
}

/* A SECOND DOCUMENT IN THIS REALM — see document.h. */
JSValue document_new(JSContext *ctx, lxb_html_document_t *dom, const char *url, const char *content_type)
{
    Document *d = doc_rec_new(ctx, dom, url, content_type);
    JSValue doc;

    /* WHO DESTROYS IT. A document a FLOW created is that flow's, exactly like a node it created: the COW delta
       owns it and destroys it when the delta is discarded, so the frontier does not accumulate one document per
       exploration arm. A creation made while capture is OFF is BASELINE — the boot flow's creations are the
       baseline by definition — and the realm that made it is what outlives it, so it goes on the realm's chain
       and dies with document_free. Exactly one of the two, which is what the flag records. */
    if (!dom_cow_note_created_document(dom)) {
        Document *active = doc_of(ctx);
        DCHECK(active != NULL, "a document was created in a realm that has none — the chain that owns a baseline "
                               "creation hangs off the realm's ACTIVE document, and there is none to hang it on");
        d->owned = 1;
        d->next_created = active->next_created;
        active->next_created = d;
    }
    doc = node_wrap(ctx, lxb_dom_interface_node(dom));
    CHECK(JS_IsObject(doc), "a created Document's wrapper allocation failed");
    d->doc_obj = JS_DupValue(ctx, doc);
    d->impl = dom_implementation_new(ctx, doc);
    return doc;
}

const char *document_url_of(const lxb_dom_document_t *dom)
{
    Document *d = doc_rec(dom);

    DCHECK(d != NULL, "a node's baseURI was read in a document with no record — §4.4 reads the NODE DOCUMENT's "
                      "address, and a tree that came from neither document_install nor document_new has none");
    return d->url;
}

void document_install(JSContext *ctx, JSValueConst global, lxb_html_document_t *dom, const char *url,
                      const char *csp, uint32_t doc_id, JSValueConst nav_proxy)
{
    Document *d;
    JSValue doc;

    DCHECK(dom != NULL, "the Document install was handed no parsed document");
    /* BEFORE ANYTHING ELSE, and before the no-address return below: the policy is a property of the parsed
       TREE, not of the address, and §7.4 clones it for an about:blank child at the moment that child is
       created — which can be the first thing a boot script does.
       ONCE PER REALM, ON THE BASELINE. A realm IS a document, so a second install into the same one means a
       NAVIGATION, and a navigation's container is per-flow: the flow that navigated sees the new policy and its
       siblings still see the old one. Replacing it here would answer for whichever world ran last, so the
       second install crashes naming the COW record to build instead. A SECOND DOCUMENT is a second realm and
       does not come through here twice. */
    DCHECK(doc_of(ctx) == NULL,
           "a document was installed twice into one realm — that is a NAVIGATION, and its container is "
           "per-flow state: build it as a COW record (like ProxyData's PROXY_REC) captured in its accessor, so "
           "the flow that navigated and the sibling that did not each read their own");
    d = doc_rec_new(ctx, dom, url, "text/html");
    d->doc = doc_id;
    d->policy = document_policy_new(dom, csp);
    /* THE REALM'S ACTIVE DOCUMENT FROM HERE ON — set before the early return below, because the policy was
       already built and §7.4 clones it for an about:blank child whether or not this document got an address. */
    JS_SetContextOpaque(ctx, d);
    /* §7.2.5.1's ONE WindowProxy FOR THIS NAVIGABLE, minted WITH the realm because that is what it is one of.
       Before the early return below: a document with no address still has a navigable, and `window.closed`
       reads the navigable's state through this object. */
    /* §7.2.5.1: A NAVIGABLE HAS ONE WindowProxy, AND THE NAVIGABLE COMES FIRST. A realm is built for a
       navigable that already exists — §7.4 created it, named it and handed its proxy to the page — so minting
       one here made a SECOND proxy for a navigable that had one. The consequence is not academic: the second
       carries no parent and no opener, so a child's `parent` answered ITSELF instead of its creator and a
       popup's `opener` was null, which is the whole of what a popup is for.
       The caller supplies it, because the caller is whoever owns the navigable: the host for the ROOT one it
       named, and §7.4 for every child it created. */
    DCHECK(window_proxy_is(nav_proxy),
           "a Document was installed for a realm with no navigable — §7.2.5.1's proxy belongs to the navigable "
           "and the navigable exists before its realm, so the caller that owns it passes it in");
    d->proxy = JS_DupValue(ctx, nav_proxy);
    if (!url || !*url)
        return;   /* no address, no Document — the page's own throw is the honest answer */

    /* DOCUMENT.PROTOTYPE, and the Document as a real NODE. §4.4 `interface Document : Node`, and it was neither
       — a plain JS_NewObject with the members copied onto it. So `document.nodeType` was undefined,
       `document.appendChild` was not a function, `document.contains(el)` (which is how a page asks whether a
       node is still in the tree) was absent, and `document.body.parentNode.parentNode === document` compared a
       node wrapper against something that was not one. It is a node_wrap of the document node now, so it is in
       the ONE identity table with everything else and its members come from a prototype chained to
       Node.prototype rather than being installed per object. */
    doc = node_wrap(ctx, lxb_dom_interface_node(dom));
    CHECK(JS_IsObject(doc), "the Document wrapper allocation failed");
    d->impl = dom_implementation_new(ctx, doc);   /* §4.5's [SameObject], built WITH the document */

    /* `URL`, `documentURI`, `documentElement`, `body` and `head` were SET HERE, as data properties on this one
       object. Every one of them is now an accessor on Document.prototype computed from the receiver's tree —
       see js_doc_tree and js_doc_strings — because a value latched onto one object is wrong in TIME (each is
       defined as a lookup in the tree AS IT IS) and wrong in SUBJECT (it cannot answer for a second document). */

    /* 3.1.1 title: the document's title, which Lexbor already computed from the tree — a pure read, no page
       code, and the real answer rather than a placeholder. */
    {
        size_t n = 0;
        const lxb_char_t *t = lxb_html_document_title(dom, &n);
        JS_SetPropertyStr(ctx, doc, "title",
                          t ? JS_NewStringLen(ctx, (const char *)t, n) : JS_NewString(ctx, ""));
    }

    /* INPUT. A cookie jar this engine was not handed and a referrer the visitor arrived with: unknown, not
       empty. `document.cookie` is how a session reaches a request URL, so "" would make every cookie-gated
       path unreachable — the same mistake as a concrete `undefined` for absent app state. */
    JS_SetPropertyStr(ctx, doc, "cookie",   concolic_new(ctx, "{document.cookie}",   "document.cookie",   JS_UNDEFINED));
    JS_SetPropertyStr(ctx, doc, "referrer", concolic_new(ctx, "{document.referrer}", "document.referrer", JS_UNDEFINED));

    /* §4.4 a Document is an EventTarget through Node, so addEventListener comes down the prototype chain now
       rather than being installed here. "loading" until its scripts have run. */
    /* The readiness is set through its ONE writer below, once `doc_obj` is in place, so the internal slot and
       the reflecting `readyState` cannot disagree — which is the whole reason the slot exists. */

    JS_SetPropertyStr(ctx, (JSValue)global, "document", JS_DupValue(ctx, doc));
    /* HELD, not borrowed: `doc` is this function's own reference and the global got a DUP of it, so the
       component owns one of the two and document_free is what releases it. The comment here used to say
       "borrowed", and a reference nobody released kept the Document — and through it the wrapped tree and the
       window — alive: JS_FreeRuntime's gc_obj_list walk counted 751 surviving objects, one per object in the
       page, from these two lines. */
    d->doc_obj = doc;
    /* HTML: a Document's readiness starts at "loading" — its parser has not finished. Written through the one
       writer so the record and the reflecting `readyState` cannot disagree. */
    document_set_ready(ctx, 0);
    d->win_obj = JS_DupValue(ctx, global);
    /* The interface OBJECTS, now that every prototype exists. Node's goes first because the derived ones
       inherit from it; each component names the one it owns rather than node.c enumerating them. */
    node_install_interfaces(ctx, global);
    {
        JSValue ep = element_proto(ctx);
        node_install_interface(ctx, global, "Element", ep);
        JS_FreeValue(ctx, ep);
    }
    html_element_install(ctx, global);   /* HTMLElement and every per-tag interface object */
    cssom_install(ctx, global);          /* CSSStyleDeclaration, and getComputedStyle on the Window */
    custom_elements_install(ctx, global);   /* §4.13.4 window.customElements */
    element_internals_install(ctx, global);  /* §4.13.7 ElementInternals, CustomStateSet, ValidityState */
    dom_token_list_install(ctx, global);    /* §7.1 DOMTokenList */
    node_filter_install(ctx, global);       /* §6.3 NodeFilter — the constants every traverser is read with */
    node_iterator_install(ctx, global);     /* §6.1 NodeIterator */
    tree_walker_install(ctx, global);       /* §6.2 TreeWalker */
    range_install(ctx, global);             /* §5.3 AbstractRange, §5.4 StaticRange, §5.5 Range */
    collections_install(ctx, global);       /* §4.2.10 NodeList, §4.2.11 HTMLCollection */
    attr_install(ctx, global);              /* §4.9.1/§4.9.2 NamedNodeMap and Attr */
    document_fragment_install(ctx, global); /* §4.7 DocumentFragment, which IS constructible */
    document_type_install(ctx, global);     /* §4.6 DocumentType */
    dom_implementation_install(ctx, global);/* §4.5.1 DOMImplementation */
    {
        /* §4.5 declares a CONSTRUCTOR, so `Document` is not one of the interface objects whose call is the
           shared "Illegal constructor" throw. */
        JSValue dp = node_type_proto(ctx, LXB_DOM_NODE_TYPE_DOCUMENT);
        node_install_interface_ctor(ctx, global, "Document", dp,
                                    idl_step_constructor(ctx, "Document", 0, g_id_doc_ctor));
        JS_FreeValue(ctx, dp);
    }
    /* §4.8.5 FOR THE TREE THE PARSER BUILT. Insertion steps run during tree construction in a browser, so an
       <iframe> the page's own markup contains has a child navigable before the first script runs — this
       engine's tree comes from a Lexbor parse that does not pass through the DOM chokepoint, so the parsed
       tree's iframes get their step here. It is LAST, after every wrapper and prototype exists, because
       creating a navigable wraps the element and stores a WindowProxy on it. */
    /* HTML tree construction produces attributes in the NULL namespace; lexbor stamps them with the element's
       namespace instead, and only here — on the tree the parse just built — are the two distinguishable. */
    dom_attr_normalize_parsed(lxb_dom_interface_node(dom));
    iframe_document_parsed(ctx);
    engine_set_document_done_hook(document_lifecycle_step);
}

/* THE DOCUMENT'S LIFECYCLE REFERENCES. Both are HELD across the lifecycle — `DOMContentLoaded` fires at the
   Document and `load` at the window long after install returns — and a held reference to either keeps the whole
   object graph alive. With no release, JS_FreeRuntime's gc_obj_list walk reported 751 surviving objects, which
   is the entire page counted one object at a time. A component that holds a reference owns releasing it. */
/* THE (DOCUMENT -> REALM) ANSWER, WHICH §4.2.3 NEEDS AND §3.7 EXPLAINS.
 *
 * The insertion and removing steps belong to the node's NODE DOCUMENT, not to whoever performed the mutation:
 * two same-origin documents are one agent (SECURITY.md's origin-keyed cluster), so `parentDoc.body` and
 * `frame.contentDocument.body` are both writable from one flow, and an <iframe> appended into the CHILD's tree
 * must create its navigable, prepare its scripts and upgrade its custom elements in the CHILD's realm. The
 * mutating member's ctx is the wrong answer for exactly the case the cluster exists to allow.
 *
 * IT LIVES ON THE DOM DOCUMENT'S OWN `user` SLOT, which lexbor keeps for its embedder and never reads. That is
 * O(1) with no registry to keep in step with the realms — a registry is a second list of documents, and the
 * failure mode of one is a stale row answering for a realm that is gone.
 *
 * IT IS ONE INDIRECTION FURTHER NOW, through the document's own record — the slot holds the RECORD and the
 * record names the realm. That is what makes a second Document expressible at all: the slot used to be the
 * whole answer, and "which document" and "which realm" were then the same question, which is exactly the
 * question §4.5.1's three factories have to ask twice. */
JSContext *document_realm_of(const lxb_dom_node_t *n)
{
    Document *d = n ? doc_rec(n->owner_document) : NULL;
    return d ? d->realm : NULL;
}

void document_free(JSContext *ctx)
{
    Document *d = doc_of(ctx), *c, *next;

    if (!d) return;   /* a realm that never had a document — the runner builds one per component test */
    /* THE DOCUMENTS THIS REALM CREATED AT BASELINE go first: each holds a wrapper of the ACTIVE document's realm
       and each owns a whole Lexbor tree, and the active record is what the chain hangs off. */
    for (c = d->next_created; c; c = next) { next = c->next_created; doc_rec_free(ctx, c); }
    doc_rec_free(ctx, d);
    JS_SetContextOpaque(ctx, NULL);
}
