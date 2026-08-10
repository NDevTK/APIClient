/* THE ELEMENT INTERFACE — Blink core/dom, over the real Lexbor tree.
 *
 * IDENTITY IS THE INVARIANT. One JS object per Lexbor element, kept in a map on this component. A page compares
 * nodes by identity constantly (`el === document.body`, a Set of visited nodes, a WeakMap keyed by node), and a
 * fresh wrapper per lookup makes every one of those silently false — the page then re-walks, re-binds and
 * re-inserts, and this engine reports a surface built out of that confusion instead of the page's.
 *
 * READS are pure Lexbor and run no page code, so they are ordinary C. WRITES go through the solver's
 * chokepoints (dom_cow_set_attribute) because a DOM write is per-flow TIME-TRAVEL state: two forked arms write
 * the same attribute differently and each reads back its own. Never raw Lexbor, which would make the write
 * global and the flows visible to each other.
 *
 * setAttribute also carries TAINT. Lexbor stores bytes, so an attacker value written into an attribute and read
 * back would come out a plain string with its provenance gone; attr_shadow keeps the (element,name) -> concolic
 * association, so `el.setAttribute("data-x", location.hash)` followed later by a sink read is still solved. */
#include <stdlib.h>
#include <string.h>

#include <lexbor/html/html.h>

#include "check.h"
#include "quickjs.h"
#include "solver/attr_shadow.h"
#include "solver/concolic.h"
#include "solver/dom_cow.h"
#include "solver/endpoint.h"
#include "solver/engine.h"
#include "solver/solve.h"
#include "core/dom/element.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event_target.h"
#include "core/html/html_element.h"
#include "core/html/custom_elements.h"
#include "core/html/html_iframe.h"
#include "core/html/trusted_types.h"
#include "core/dom/dom_token_list.h"
#include "core/dom/collections.h"
#include "core/dom/attr.h"
#include "core/dom/document_fragment.h"
#include "core/idl_indexed.h"
#include "core/css/css_style_declaration.h"
#include <lexbor/ns/ns.h>

/* The two shapes every DOM member in this file has. Spelled once so a member declares its IDL, not a bitmask. */
static JSAtom g_attrs_key = JS_ATOM_NULL;   /* the [SameObject] NamedNodeMap cache slot on an element's wrapper */

static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
#include <lexbor/html/serialize.h>
#include "core/dom/node.h"
#include "core/dom/document.h"

/* IDENTITY AND THE TREE BASE LIVE IN node.c — one wrapper table for every node kind, because a tree whose only
   node kind is Element cannot represent the document it just parsed. This file is what makes a node an ELEMENT:
   attributes, tagName, innerHTML, the reflected properties. */
static lxb_dom_element_t *elem_of(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return NULL;
    return lxb_dom_interface_element(n);
}

static JSValue js_el_get_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    lxb_dom_element_t *el = elem_of(this_val);
    const char *name;
    JSValue r = JS_NULL;
    int si;

    if (!el || argc < 1) return JS_NULL;
    name = concolic_name_cstr(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    /* The TAINT SHADOW answers first: an attacker value written here came back out of Lexbor as plain bytes
       with its provenance gone, and a sink reading it would look clean. */
    si = attr_shadow_find(el, ATTR_SLOT_ATTRIBUTE, name);
    if (si >= 0) {
        r = JS_DupValue(ctx, attr_shadow_opaque(si));   /* borrowed — dup to hand it out */
    } else {
        size_t vl = 0;
        const lxb_char_t *v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl);
        if (v) r = JS_NewStringLen(ctx, (const char *)v, vl);
    }
    JS_FreeCString(ctx, name);
    return r;
}

/* DOM §4.9 "set an attribute value" — setAttribute's steps 4-7 and setAttributeNS's step 3, over a name that is
   already the one the algorithm computed (validated, and ASCII-lowercased where step 2 says so) and a value
   whose Trusted Type step 3 has already verified. Every write in this file lands here, which is what makes the
   taint shadow and the per-flow DOM delta impossible for a caller to skip.
   NO USER CODE: `handle attribute changes` enqueues a mutation record and an `attributeChangedCallback` and
   runs the attribute change steps, and SPEC_STEPS §2.5 is the statement that none of those three runs script.
   The reactions surface at the `[CEReactions]` boundary, not here. */
static void el_write_attribute(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst value)
{
    const char *val;

    /* A concolic value has no bytes to store. Record it in the shadow so the read gives the SAME concolic back,
       and write its shape into the tree so a serialization of the document still shows something. */
    if (concolic_is(value)) {
        attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, name, value);
        /* NEVER ToString a concolic to get bytes: its coercion belongs to the concolic hooks and answers with
           another concolic, so the call THROWS — and the throw was being dropped here, leaving a pending
           exception behind a normal return. The SHAPE is the honest byte form for the tree. */
        val = concolic_shape_c(value);
        dom_cow_set_attribute(el, name, val ? val : "", val ? strlen(val) : 0);
        return;
    }
    attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, name, JS_UNDEFINED);   /* a concrete write clears any earlier taint */
    val = JS_ToCString(ctx, value);
    DCHECK(val != NULL, "an attribute value reached the write unconverted — the IDL declaration is what converts "
                        "it, and running the page's toString from here is the drive-to-completion the flow "
                        "machinery exists to avoid");
    if (!val) return;
    dom_cow_set_attribute(el, name, val, strlen(val));   /* chokepoint: capture-then-mutate, per flow */
    JS_FreeCString(ctx, val);
}

/* DOM §1.4: a string is a VALID ATTRIBUTE LOCAL NAME if its length is at least 1 and it contains no ASCII
   whitespace, U+0000, U+002F (/), U+003D (=) or U+003E (>). setAttribute step 1 throws InvalidCharacterError
   for anything else, and a page's own feature detection reads that throw. */
static bool el_valid_attribute_local_name(const char *n)
{
    size_t i;

    if (!n || !*n) return false;
    for (i = 0; n[i]; i++) {
        char c = n[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') return false;
        if (c == '/' || c == '=' || c == '>') return false;
    }
    return true;   /* a U+0000 cannot appear in a NUL-terminated C string, and JS_ToCString rejects one */
}

/* THE ELEMENT AS §3.8 NAMES IT — its namespace URL and its local name, which together decide which interface
   it implements and therefore which row of the Trusted Types table it can match. Borrowed from Lexbor's own
   interned strings; `*ns` is NULL for an element in no namespace. */
static void el_ns_and_local(lxb_dom_element_t *el, const char **ns, const char **local,
                            char *nsbuf, size_t nscap, char *lobuf, size_t locap)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    size_t len = 0;
    const lxb_char_t *s;

    *ns = NULL;
    *local = "";
    s = lxb_ns_by_id(n->owner_document->ns, n->ns, &len);
    if (s && len && len < nscap) { memcpy(nsbuf, s, len); nsbuf[len] = 0; *ns = nsbuf; }
    len = 0;
    s = lxb_dom_element_local_name(el, &len);
    if (s && len < locap) { memcpy(lobuf, s, len); lobuf[len] = 0; *local = lobuf; }
}

/* §4.9 setAttribute, AS A MACHINE, because its step 3 is Trusted Types §3.7 → §3.4 → §3.5, and §3.5 calls the
   page's own default-policy `createHTML`/`createScript`/`createScriptURL`. That is author code running in the
   MIDDLE of setAttribute, between the lowercase and the write, so the algorithm cannot be one C body — a stage
   boundary has to exist exactly there or the flow has nowhere to park.
   ITS OWN STAGE TABLE, NOT ONE SHARED WITH setAttributeNS. The two put the Trusted Types call at different
   positions — after the lowercase (step 3 of 7) here, after validate-and-extract (step 2 of 3) there — so one
   table across both would name the wrong step for one of them, which is a parked flow that reports a position
   in its algorithm that it is not at. */
#define EL_SET_ATTR_STAGES(X) \
    X(SETATTR_NAME,    "DOM §4.9 setAttribute steps 1-2 (throw InvalidCharacterError unless qualifiedName is a " \
                       "valid attribute local name; ASCII-lowercase it for an element in the HTML namespace in " \
                       "an HTML document)") \
    X(SETATTR_TRUSTED, "DOM §4.9 setAttribute step 3 (get trusted type compliant attribute value with " \
                       "qualifiedName, a null attribute namespace, this and value), which is where Trusted " \
                       "Types §3.5's default-policy callback runs") \
    X(SETATTR_WRITE,   "DOM §4.9 setAttribute steps 4-7 (change the attribute of that qualified name, or " \
                       "create one and append it)")
enum { IDL_STEP_STAGE_BASE(EL_SET_ATTR_STAGES) EL_SET_ATTR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EL_SET_ATTR_STEPS[] = { EL_SET_ATTR_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    char   *name;        /* step 2's answer: the lowercased qualified name, held across step 3's suspension */
    size_t  name_len;
    JSValue verified;    /* step 3's answer, owned — what steps 5-7 write, never the argument */
} SetAttrState;

static void set_attr_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    SetAttrState *s = st;
    v->val(ctx, &s->verified);
    v->buf(ctx, (void **)&s->name, s->name_len ? s->name_len + 1 : 0);
}

static void set_attr_release(JSContext *ctx, void *st)
{
    SetAttrState *s = st;
    free(s->name);
    s->name = NULL;
    JS_FreeValue(ctx, s->verified);
    s->verified = JS_UNDEFINED;
}

static int js_el_set_attribute(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                               JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SetAttrState *s = st;
    lxb_dom_element_t *el = elem_of(hdr->this_val);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;

    if (hdr->stage == SETATTR_NAME) {
        /* THE NAME, WHICH CAN ALSO BE UNKNOWN. The VALUE has had a concolic path since the taint shadow was
           built; the name never did, so `setAttribute(someParameter, v)` crashed at the coercion while
           `setAttribute("href", someParameter)` worked. An unknown name denotes its shape, stable per source. */
        const char *name = concolic_name_cstr(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        size_t i;

        if (!name) return JS_STEP_ABRUPT;
        if (!el || argc < 2) { JS_FreeCString(ctx, name); return JS_STEP_DONE; }
        if (!el_valid_attribute_local_name(name)) {          /* step 1 */
            JS_FreeCString(ctx, name);
            JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                 "setAttribute was given a name that is not a valid attribute local name");
            return JS_STEP_ABRUPT;
        }
        s->name_len = strlen(name);
        s->name = malloc(s->name_len + 1);
        CHECK(s->name != NULL, "setAttribute could not copy its own attribute name");
        memcpy(s->name, name, s->name_len + 1);
        JS_FreeCString(ctx, name);
        /* STEP 2. Lexbor lowercases the name it STORES for an HTML element in an HTML document, and this
           member did not — so the taint shadow was keyed on the case the page wrote while the tree held the
           lowercase, and `el.setAttribute("SRC", location.hash)` then read back clean through
           `el.getAttribute("src")`. Doing step 2 here is what makes the two agree. */
        if (lxb_dom_interface_node(el)->ns == LXB_NS_HTML &&
            lxb_dom_interface_node(el)->owner_document->type == LXB_DOM_DOCUMENT_DTYPE_HTML)
            for (i = 0; i < s->name_len; i++)
                if (s->name[i] >= 'A' && s->name[i] <= 'Z') s->name[i] = (char)(s->name[i] - 'A' + 'a');
        hdr->stage = SETATTR_TRUSTED;
    }
    if (hdr->stage == SETATTR_TRUSTED) {
        char nsbuf[128], lobuf[64];
        const char *ns, *local;

        if (!el) return JS_STEP_DONE;
        el_ns_and_local(el, &ns, &local, nsbuf, sizeof(nsbuf), lobuf, sizeof(lobuf));
        /* STEP 3, with a NULL attribute namespace — which is what makes `el.setAttribute("onclick", s)` a
           TrustedScript sink and `el.setAttributeNS(XLINK, "xlink:href", s)` not the same one. */
        s->verified = trusted_types_compliant_attribute_value(ctx, ns, local, NULL, s->name,
                                                              argc > 1 ? argv[1] : JS_UNDEFINED);
        if (JS_IsException(s->verified)) { s->verified = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        hdr->stage = SETATTR_WRITE;
    }
    DCHECK(hdr->stage == SETATTR_WRITE, "setAttribute resumed into a stage it does not have");
    if (el) el_write_attribute(ctx, el, s->name, s->verified);   /* steps 4-7 */
    return JS_STEP_DONE;
}

static const IdlStepDecl EL_SET_ATTR_STEP = {
    js_el_set_attribute, sizeof(SetAttrState), set_attr_visit, set_attr_release,
    "DOM §4.9 Element.setAttribute", EL_SET_ATTR_STEPS
};

/* THE ENGINE'S OWN ATTRIBUTE WRITE — a reflection's setter, the hyperlink mixin re-serialising `href`. It is
   DOM §4.9's "set an attribute value" and nothing above it: the name is the engine's own (so step 1 cannot
   fail and step 2 has nothing to lowercase), and the Trusted Types step belongs to the MEMBER the page called,
   which for a reflected IDL attribute is that attribute's own setter. */
static void el_set_attribute_internal(JSContext *ctx, JSValueConst this_val, const char *name, JSValueConst value)
{
    lxb_dom_element_t *el = elem_of(this_val);

    if (!el) return;
    DCHECK(el_valid_attribute_local_name(name), "the engine wrote an attribute whose name the DOM would reject");
    el_write_attribute(ctx, el, name, value);
}

/* §4.9 tagName is the HTML-UPPERCASED qualified name: for an element in the HTML namespace whose document is an
   HTML document, the qualified name in ASCII uppercase. This returned the qualified name itself, so `p` where
   every browser says `P` — and `el.tagName === 'DIV'` is one of the most common things a page writes, silently
   false in every one of them.
   The engine already had the right answer in the next member along: nodeName goes through lxb_dom_node_name,
   which calls lxb_dom_element_tag_name, which IS this rule. So `el.nodeName` said `P` while `el.tagName` said
   `p` — two members of one interface that must agree, disagreeing, because one of them reached past the
   function that knows the rule. */
static JSValue js_el_get_tag(JSContext *ctx, JSValueConst this_val)
{
    lxb_dom_element_t *el = elem_of(this_val);
    size_t n = 0;
    const lxb_char_t *t;
    if (!el) return JS_UNDEFINED;
    t = lxb_dom_element_tag_name(el, &n);
    return t ? JS_NewStringLen(ctx, (const char *)t, n) : JS_UNDEFINED;
}

/* §8.4 THE FRAGMENT SERIALISER — innerHTML and outerHTML as READS, which they were not: the accessor was
   write-only and every `el.innerHTML` answered undefined. That is the worst shape a gap can take here, because
   undefined does not throw — it PROPAGATES. `wrap.innerHTML = head.innerHTML + row` builds the string
   "undefined…" and the page carries on, so the engine reports a surface assembled out of a value the page
   never had, and nothing anywhere names the missing capability.
   Lexbor owns the serialisation OF ONE NODE, which is the point: the escaping, the attribute quoting and the
   raw-text elements are HTML's own rules, and hand-rolling them here would be a second HTML serialiser that
   disagrees with the parser sitting beside it. What this file owns is the WALK — because the walk is of the
   PAGE'S SIZE, and `lxb_html_serialize_tree_str` runs it to completion inside one opcode. That is the
   drive-to-completion this engine has no room for: `document.body.outerHTML` on a real page held the scheduler
   for the whole document with every other flow parked behind it. So the walk is a machine that emits ONE node
   per step and yields, and lexbor is asked for that one node.
   The closing tag is the one piece lexbor does not export (`lxb_html_serialize_element_closed_cb` is static),
   so it is emitted here from the same qualified name and gated on the same public `lxb_html_node_is_void`.
   ONE DELIBERATE DIVERGENCE FROM LEXBOR, and it is the SPEC that decides it. §13.3 step 2: "If current node is a
   template element, then let current node instead be the template element's template contents" — the template is
   REPLACED by its content, so its ordinary children (which `t.appendChild(x)` really does create, since only the
   parser and `t.content` reach the fragment) are not serialised at all. Lexbor emits the content and THEN
   descends into first_child, which prints them; this walk comes back from the content straight to the close tag.
   Asserted by /api/tplboth, whose template has one of each.
   magic 0 = innerHTML (the CHILDREN), 1 = outerHTML (the element itself). */

/* A LEVEL of the walk: `<template>`'s children live on a SEPARATE tree (its content fragment, whose node has no
   parent), so serialising one means walking a second tree and coming back. Lexbor recurses for that; this
   pushes, because a machine's C stack is gone at every suspension. */
typedef struct { lxb_dom_node_t *node; lxb_dom_node_t *limit; } SerFrame;

/* WHERE THIS MACHINE RESTS, AS THE STANDARD NUMBERS IT. Both getters are one sentence over the fragment
   serializing algorithm, which for an HTML document is §13.3's HTML fragment serialization algorithm; §13.3's
   step 4 walks the children in tree order and its step 4.2 appends each one's string. The walk RESTS twice per
   node — once having emitted it and once having advanced past it — and those were one stage and a private
   `phase` byte beside it, which is two suspension points wearing one number. They are two stages now. Nothing
   here can reach the page's code, so no stage has to split further. */
#define EL_GET_HTML_STAGES(X) \
    X(ELHTML_SETUP,   "HTML §8.5.4 innerHTML getter / §8.5.5 outerHTML getter steps 1-2 (the node to serialize)") \
    X(ELHTML_EMIT,    "HTML §13.3 step 4.2 (append the current node's start tag or its character data; a " \
                      "template's contents are the node instead, per step 3)") \
    X(ELHTML_ADVANCE, "HTML §13.3 step 4 (descend to the node's children, or append its end tag and advance in " \
                      "tree order)")
enum { IDL_STEP_STAGE_BASE(EL_GET_HTML_STAGES) EL_GET_HTML_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EL_GET_HTML_STEPS[] = { EL_GET_HTML_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    lxb_dom_node_t *node;    /* the cursor; NULL once the walk is finished */
    lxb_dom_node_t *limit;   /* the ascent stops HERE and does not close it — it is not part of the output */
    SerFrame       *stack;   /* the template levels above this one */
    int             sp, scap;
    char           *out;     /* the accumulator: malloc'd, because a fork gives each arm its own */
    size_t          out_len, out_cap;
} ElHtmlState;

static lxb_status_t el_ser_append(const lxb_char_t *data, size_t len, void *vctx)
{
    ElHtmlState *s = vctx;
    if (s->out_len + len + 1 > s->out_cap) {
        size_t want = s->out_cap ? s->out_cap * 2 : 256;
        char *n;
        while (want < s->out_len + len + 1) want *= 2;
        n = realloc(s->out, want);
        CHECK(n != NULL, "the HTML serialiser could not grow its accumulator");
        s->out = n;
        s->out_cap = want;
    }
    memcpy(s->out + s->out_len, data, len);
    s->out_len += len;
    return LXB_STATUS_OK;
}

/* `</name>`, which lexbor emits from a static function. Void elements have none, and neither does anything that
   is not an element — the same two conditions lexbor's own ascent tests. */
static void el_ser_close(ElHtmlState *s, lxb_dom_node_t *n)
{
    const lxb_char_t *name;
    size_t len = 0;
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || lxb_html_node_is_void(n)) return;
    name = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &len);
    DCHECK(name != NULL, "an element in the tree has no qualified name to close");
    el_ser_append((const lxb_char_t *)"</", 2, s);
    el_ser_append(name, len, s);
    el_ser_append((const lxb_char_t *)">", 1, s);
}

static void el_ser_push(ElHtmlState *s, lxb_dom_node_t *node, lxb_dom_node_t *limit)
{
    if (s->sp == s->scap) {
        int want = s->scap ? s->scap * 2 : 8;
        SerFrame *n = realloc(s->stack, sizeof(SerFrame) * (size_t)want);
        CHECK(n != NULL, "the HTML serialiser could not grow its template stack");
        s->stack = n;
        s->scap = want;
    }
    s->stack[s->sp].node = node;
    s->stack[s->sp].limit = limit;
    s->sp++;
}

static int js_el_get_html_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                               JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    ElHtmlState *s = st;
    lxb_dom_element_t *el;
    lxb_dom_node_t *n;

    (void)argc; (void)argv; (void)cb_result; (void)out_cb; (void)out_argc;

    if (hdr->stage == ELHTML_SETUP) {
        el = elem_of(hdr->this_val);
        if (!el) { *presult = JS_UNDEFINED; return JS_STEP_DONE; }
        n = lxb_dom_interface_node(el);
        DCHECK(n->local_name != LXB_TAG__DOCUMENT,
               "the fragment serialiser reached a DOCUMENT — this accessor lives on Element, and a document "
               "serialises its children with no wrapper of its own");
        /* magic 0 walks the CHILDREN with the element as the limit, so the element's own tags are not emitted;
           magic 1 walks the element itself, limited by its parent. One walk, two starting points. */
        int inner = idl_step_magic(hdr) == 0;
        s->limit = inner ? n : n->parent;
        s->node  = inner ? n->first_child : n;
        hdr->stage = ELHTML_EMIT;
    }

    if (!s->node) goto finished;

    if (hdr->stage == ELHTML_EMIT) {
        lxb_status_t status = lxb_html_serialize_cb(s->node, el_ser_append, s);
        DCHECK(status == LXB_STATUS_OK, "lexbor refused to serialise a node kind this tree contains");
        (void)status;
        /* `<template>`'s children are on its content fragment, not under it. Descend there before the template
           is closed, and come back to the template's ADVANCE when that level runs out. */
        if (lxb_html_tree_node_is(s->node, LXB_TAG_TEMPLATE)) {
            lxb_html_template_element_t *t = lxb_html_interface_template(s->node);
            if (t->content && t->content->node.first_child) {
                el_ser_push(s, s->node, s->limit);
                s->limit = &t->content->node;
                s->node  = t->content->node.first_child;
                return JS_STEP_YIELD;
            }
        }
        hdr->stage = ELHTML_ADVANCE;
        return JS_STEP_YIELD;
    }

    /* ADVANCE. A void element has no children to descend into even when the tree gave it some. */
    DCHECK(hdr->stage == ELHTML_ADVANCE, "the fragment serialiser resumed into a stage §13.3 does not have");
    if (!lxb_html_node_is_void(s->node) && s->node->first_child) {
        s->node = s->node->first_child;
        hdr->stage = ELHTML_EMIT;
        return JS_STEP_YIELD;
    }
    for (;;) {
        el_ser_close(s, s->node);
        if (s->node->next) { s->node = s->node->next; hdr->stage = ELHTML_EMIT; return JS_STEP_YIELD; }
        s->node = s->node->parent;
        if (s->node == s->limit) {
            if (s->sp == 0) break;                       /* the walk itself is over */
            s->sp--;                                     /* back to the template that owns this level */
            s->node  = s->stack[s->sp].node;
            s->limit = s->stack[s->sp].limit;
            continue;                                    /* close the <template> and carry on from it */
        }
        DCHECK(s->node != NULL, "the serialiser walked off the top of the tree without reaching its limit — the "
                                "cursor left the subtree the walk started in");
    }
finished:
    *presult = JS_NewStringLen(ctx, s->out ? s->out : "", s->out_len);
    return JS_STEP_DONE;
}

static void js_el_get_html_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    ElHtmlState *s = st;
    /* Both are plain storage a forked arm must not share: the two arms append their own remaining nodes to the
       accumulator, and each unwinds its own template stack. The DOM pointers inside are per-flow COW nodes,
       which every arm reaches by the same address. */
    v->buf(ctx, (void **)&s->out, s->out_cap);
    v->buf(ctx, (void **)&s->stack, sizeof(SerFrame) * (size_t)s->scap);
}

static void js_el_get_html_release(JSContext *ctx, void *st)
{
    ElHtmlState *s = st;
    (void)ctx;
    free(s->out);
    free(s->stack);
    s->out = NULL;
    s->stack = NULL;
}

static const IdlStepDecl EL_GET_HTML_STEP = {
    js_el_get_html_step, sizeof(ElHtmlState), js_el_get_html_visit, js_el_get_html_release,
    "HTML §8.5.4/§8.5.5 innerHTML/outerHTML getter (over §13.3's HTML fragment serialization)",
    EL_GET_HTML_STEPS
};

/* §13.4 THE FRAGMENT PARSE, as ONE operation, because there are four members that do it and they differ only
   in where the result goes. `context` is the element whose parsing state the fragment is parsed IN — a `<tr>`
   is dropped anywhere but inside a table, and that is the tree builder's rule, not something a caller chooses.
   The parsed nodes are handed to `place`, which inserts each through the per-flow chokepoints.
   LEXBOR MUST NOT RUN PAGE CODE. That is what lets a parser — a state machine with a great deal of internal
   position — live inside an engine whose flows suspend and resume at any depth: the parse holds no continuation
   across anything the page can preempt, so it never has to be suspended and never has to be part of a snapshot.
   It completes inside one opcode over bytes, and any <script> it produces is QUEUED as a flow by
   element_prepare_script rather than executed by the tree builder.
   Re-entry is what a violation would look like: page code running mid-parse and reaching one of these again.
   Asserted rather than assumed, because the day it stops holding is the day a half-built tree ends up inside
   another flow's delta. */
enum { PLACE_CHILDREN = 0, PLACE_BEFORE, PLACE_AFTER, PLACE_FIRST_CHILD, PLACE_REPLACE };

/* THE FRAGMENT PARSE, AS A MACHINE — the last drive-to-completion left beside the insertion it feeds.
 * `lxb_html_document_parse_fragment` tokenises and tree-builds the whole markup inside one opcode, so
 * `container.innerHTML = bigMarkup` held the scheduler for the length of the markup. The insertion steps next
 * to it were converted first and this was still the larger half.
 *
 * LEXBOR HAS THE SEAM ALREADY: chunk_begin / chunk_process / chunk_end is exactly a resumable parse, and the
 * `lexbor_in` machinery behind it exists so a token can span two chunks. So the parser is fed ONE BYTE per
 * step. A byte is the finest unit lexbor offers — it will not expose a token boundary — and it needs no chosen
 * quantum, which is the thing a "parse 4096 bytes then yield" would have to invent and defend.
 *
 * A PRIVATE PARSER PER PARSE, and that is not an optimisation — it is what makes yielding legal at all.
 * `lxb_html_document_parse_fragment` uses the DOCUMENT's parser, and the moment a parse can suspend, a second
 * flow can start its own; two interleaved parses sharing one tokenizer and one open-element stack would
 * corrupt both. chunk_begin takes the parser explicitly and builds its own temporary document, so a parser per
 * parse is independent by construction. The old `in_parse` re-entry DCHECK is gone with it: it asserted that a
 * parse never overlaps, which is now exactly what this machine is built to allow.
 *
 * IT STILL RUNS NO PAGE CODE. That is what keeps a suspended parse safe to leave lying around: the tree builder
 * cannot reach a script (element_prepare_script QUEUES one), so nothing can observe a half-built fragment, and
 * nothing can fork this flow while the machine is on its chain. */
/* WHERE THIS MACHINE RESTS. The three members that parse markup are the same shape — a few leading steps of
   their own, then "let fragment be the result of invoking the fragment parsing algorithm steps", then a
   placement — so the stages after the entry belong to those two operations and each member's declaration names
   them in ITS OWN numbering. The clear is LAST in the enum on purpose: only the innerHTML setter replaces its
   target's children, so insertAdjacentHTML simply does not declare that stage, and the driver's check is what
   says so if the shared machine ever reaches it from there.
   THE ORDER IS THE SPEC'S, and it was not: the children were removed BEFORE the parse, which is step 5 running
   before step 4. Nothing observed the difference — the tree builder reads the context element's name, not its
   children, and no page code runs inside either — but a stage cannot name a step it runs out of order, and
   that is exactly what naming them exposed. The parse now completes first and the replacement follows it. */
/* ONE LIST FOR ONE MACHINE, and the two members that drive it expand it — the shared four, then the fifth that
   only innerHTML= reaches. A stage of a shared machine is ONE rest point, so it carries ONE label naming every
   section that reaches it (the way QS_STEPS names all four of its members'); which member a parked flow is in
   is what the declaration's `algorithm` says. Splitting the wording per member would be two statements of one
   stage, which is the drift the X-list exists to prevent. */
#define FRAG_STAGES(X) \
    X(FRAG_TRUSTED, "HTML §8.5.4 / §8.5.5 / §8.5.6 step 1 (get trusted type compliant string with TrustedHTML " \
                    "and this sink), which is where Trusted Types §4.2's default-policy callback runs") \
    X(FRAG_START, "HTML §8.5.4 innerHTML setter steps 2-3 / §8.5.5 outerHTML setter steps 2-5 / §8.5.6 " \
                  "insertAdjacentHTML steps 2-4 (the target the fragment is parsed against)") \
    X(FRAG_FEED,  "HTML §8.5.4 step 4 / §8.5.5 step 6 / §8.5.6 step 5 (the fragment parsing algorithm), one " \
                  "byte per step") \
    X(FRAG_PLACE, "HTML §8.5.4 step 5 / §8.5.5 step 7 / §8.5.6 step 6 (insert one node of the fragment at the " \
                  "position the member names)") \
    X(FRAG_DONE,  "HTML §8.5.4 step 5 / §8.5.5 step 7 / §8.5.6 step 6 (the fragment is placed)")
/* FOUR STAGES, NOT FIVE, for insertAdjacentHTML: it never replaces its target's children, so FRAG_CLEAR is past
   the end of what it declares and the driver says so if the shared machine ever reaches it from there. It is
   its own list for that reason — the setter's declaration is the shared four followed by this one. */
#define EL_SET_HTML_EXTRA(X) \
    X(FRAG_CLEAR, "HTML §8.5.4 step 5 (replace all within target: remove one existing child per step)")
enum { IDL_STEP_STAGE_BASE(FRAG_STAGES)
       FRAG_STAGES(JS_STEP_STAGE_ENUM) EL_SET_HTML_EXTRA(JS_STEP_STAGE_ENUM) };

typedef struct {
    uint8_t where;
    uint8_t clear_first;          /* innerHTML= empties the element before parsing, one child per step */
    /* STEP 1's ANSWER, held across the stage boundary (owned). It is the compliant string and not the
       argument: once Trusted Types §3 exists, step 1 runs the default policy's callback and what the fragment
       is parsed from is what that callback RETURNED, which is a different value from the one passed in. */
    JSValue compliant;
    lxb_html_parser_t *parser;    /* THIS parse's own — see above */
    char   *html;                 /* the markup, owned: the parser is handed slices of it across suspensions */
    size_t  len, off;
    lxb_dom_element_t *context;
    lxb_dom_node_t *anchor, *ref, *frag, *node;
} FragState;

static void frag_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FragState *s = st;
    v->val(ctx, &s->compliant);
    /* A FORK CANNOT REACH A PARSE IN FLIGHT. A fork is a concolic branch, which is bytecode, and this machine
       runs none — the tree builder cannot reach the page's code. Two flows handed one lexbor parser would
       corrupt both, so it is asserted rather than trusted; if it ever fires, the parser needs a real ownership
       declaration and there is no such thing as half a tokenizer to clone. */
    DCHECK(s->parser == NULL, "a fragment parse was forked mid-parse");
    v->buf(ctx, (void **)&s->html, s->len ? s->len + 1 : 0);
}

static void frag_release(JSContext *ctx, void *st)
{
    FragState *s = st;
    /* THE THROW PATH OWNS THE PARSER TOO. A flow dropped mid-parse would otherwise leak a tokenizer, an
       open-element stack and the temporary document behind them. */
    if (s->parser) { lxb_html_parse_fragment_chunk_end(s->parser); lxb_html_parser_destroy(s->parser); }
    s->parser = NULL;
    free(s->html);
    s->html = NULL;
    JS_FreeValue(ctx, s->compliant);
    s->compliant = JS_UNDEFINED;
}

/* Set the machine up for a parse of `html` into `where` around `anchor`, parsed in `context`'s tree-building
   context. `html` is COPIED because the JSString it came from is released before the first suspension. */
static void frag_begin(JSContext *ctx, FragState *s, lxb_dom_element_t *context, lxb_dom_node_t *anchor,
                       int where, const char *html, bool clear_first)
{
    (void)ctx;
    /* The clear is `replace all within TARGET`, and the target is the anchor — which for a <template> is its
       content fragment rather than the element. Only the children-replacing member asks for it, which is what
       lets the placement's reference child be computed before the clear rather than after it. */
    DCHECK(!clear_first || where == PLACE_CHILDREN,
           "a fragment parse asked to replace its target's children at a position that is not PLACE_CHILDREN — "
           "the placement's reference child is fixed before the clear, which only holds for an append");
    s->context = context;
    s->anchor = anchor;
    s->where = (uint8_t)where;
    s->clear_first = clear_first;
    s->len = strlen(html);
    s->html = malloc(s->len + 1);
    CHECK(s->html != NULL, "the fragment parse could not copy its markup");
    memcpy(s->html, html, s->len + 1);
    s->off = 0;
    s->node = NULL;
}

/* ONE STEP of the parse-and-place. Returns JS_STEP_YIELD while there is more, or 0 when the fragment is in the
   tree. Every caller is a member body that returns whatever this returns. */
static int frag_step(JSContext *ctx, JSStepHdr *hdr, FragState *s)
{
    switch (hdr->stage) {
    case FRAG_CLEAR: {
        /* `Replace all with fragment within target` REMOVES the target's children first, and a page's existing
           subtree is as big as the page. The parse is already finished by the time this runs, which is the
           order the setter states. */
        lxb_dom_node_t *next;
        if (!s->node) {
            if (!s->frag) { hdr->stage = FRAG_DONE; return 0; }
            s->node = s->frag->first_child;
            hdr->stage = FRAG_PLACE;
            return JS_STEP_YIELD;
        }
        next = s->node->next;
        dom_cow_remove_child(s->node);
        s->node = next;
        return JS_STEP_YIELD;
    }
    case FRAG_FEED:
        if (!s->parser) {
            lxb_dom_node_t *cn = lxb_dom_interface_node(s->context);
            s->parser = lxb_html_parser_create();
            CHECK(s->parser != NULL && lxb_html_parser_init(s->parser) == LXB_STATUS_OK,
                  "the fragment parser could not be created");
            lxb_html_parse_fragment_chunk_begin(s->parser,
                lxb_html_interface_document(cn->owner_document), cn->local_name, cn->ns);
            return JS_STEP_YIELD;
        }
        if (s->off < s->len) {
            /* ONE BYTE. lexbor's incoming-buffer machinery is what makes a token able to span two of these. */
            lxb_html_parse_fragment_chunk_process(s->parser, (const lxb_char_t *)s->html + s->off, 1);
            s->off++;
            return JS_STEP_YIELD;
        }
        s->frag = lxb_html_parse_fragment_chunk_end(s->parser);
        lxb_html_parser_destroy(s->parser);
        s->parser = NULL;
        /* The reference child is fixed BEFORE anything moves: inserting changes `anchor->next`. The clear that
           may follow cannot move it either — it only ever empties an append target, which frag_begin asserts. */
        s->ref = (s->where == PLACE_AFTER) ? s->anchor->next
               : (s->where == PLACE_FIRST_CHILD) ? s->anchor->first_child
               : s->anchor;
        if (s->clear_first) {
            s->node = s->anchor->first_child;
            hdr->stage = FRAG_CLEAR;
            return JS_STEP_YIELD;
        }
        if (!s->frag) { hdr->stage = FRAG_DONE; return 0; }
        s->node = s->frag->first_child;
        hdr->stage = FRAG_PLACE;
        return JS_STEP_YIELD;

    case FRAG_PLACE: {
        /* Everything here moves nodes OUT of what the parse just built, which nothing else has ever seen — see
           dom_cow.h. `frag` is the declaration, passed to each operation. */
        lxb_dom_node_t *node = s->node, *next;
        if (!node) {
            dom_cow_destroy_private(s->frag, /*with_children*/ false);
            if (s->where == PLACE_REPLACE) dom_cow_remove_child(s->anchor);
            hdr->stage = FRAG_DONE;
            return 0;
        }
        next = node->next;
        dom_cow_take_private(s->frag, node);   /* out of the private tree; the INSERT below is the shared write */
        switch (s->where) {
        case PLACE_CHILDREN:    dom_cow_append_child(s->anchor, node); break;
        case PLACE_BEFORE:
        case PLACE_REPLACE:     dom_cow_insert_before(s->anchor, node); break;
        case PLACE_AFTER:       if (s->ref) dom_cow_insert_before(s->ref, node);
                                else dom_cow_append_child(s->anchor->parent, node);
                                break;
        case PLACE_FIRST_CHILD: if (s->ref) dom_cow_insert_before(s->ref, node);
                                else dom_cow_append_child(s->anchor, node);
                                break;
        default: DFAIL("a fragment was placed with an unknown position"); break;
        }
        s->node = next;
        return JS_STEP_YIELD;
    }
    default:
        DCHECK(hdr->stage == FRAG_DONE, "the fragment machine resumed into a stage it does not have");
        return 0;
    }
}

/* An HTML-context sink is TWO things and it must do both.
   It is a SINK, so the assigned value goes to the solver, which decides the breakout against the real parse
   context. And it MUTATES THE TREE — a page that builds its DOM this way and then queries it must find what it
   built, or every getElementById after it answers null and the engine reports a surface the page never had.
   Reporting the sink and dropping the markup was the second half missing.
   Both halves go through the per-flow chokepoints, so two forked arms each see their own subtree. A concolic
   value has no bytes to parse — the sink report IS the answer for it.
   magic 0 = innerHTML=, 1 = outerHTML=. */
static int js_el_set_html(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FragState *s = st;
    int magic = idl_step_magic(hdr);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;

    if (hdr->stage == FRAG_TRUSTED) {
        /* §8.5.4 / §8.5.5 step 1. It runs BEFORE the element and its parent are looked at, which is the order
           the standard writes and the order that decides what a page under a trusted-types policy sees: the
           throw comes from step 1, not from step 4's parse, so `document.createElement("b").outerHTML = s`
           throws the TypeError rather than returning at step 3's null parent. */
        s->compliant = trusted_types_compliant_string(ctx, TRUSTED_TYPE_HTML, argc > 0 ? argv[0] : JS_UNDEFINED,
                                                      magic == 0 ? "Element innerHTML" : "Element outerHTML");
        if (JS_IsException(s->compliant)) { s->compliant = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        hdr->stage = FRAG_START;
    }
    if (hdr->stage == FRAG_START) {
        lxb_dom_element_t *el = elem_of(hdr->this_val);
        JSValueConst val = s->compliant;
        lxb_dom_node_t *n;
        const char *html;

        if (!el) return JS_STEP_DONE;
        n = lxb_dom_interface_node(el);
        if (magic == 1) {
            /* §8.5.5 steps 3-4, WHICH ARE TWO DIFFERENT ANSWERS and were one. A null parent RETURNS — the
               spec's own reason is that there would be no way to obtain a reference to the nodes created —
               and only a DOCUMENT parent throws. Collapsing them into "not an element parent → throw" made
               `document.createElement("b").outerHTML = "<i>"` a NoModificationAllowedError, which is a throw
               the standard does not have and which a page's own try/catch reads as a real failure. */
            if (!n->parent) return JS_STEP_DONE;                            /* step 3 */
            if (n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT) {            /* step 4 */
                JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                                     "outerHTML on an element whose parent is a Document");
                return JS_STEP_ABRUPT;
            }
            DCHECK(n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
                   "§8.5.5 step 5: outerHTML on an element whose parent is a DocumentFragment parses against a "
                   "freshly created HTML `body` element, which this engine has no create-an-element path to "
                   "here — build it rather than parsing against the fragment");
        }
        solve_html_sink(ctx, val);
        if (concolic_is(val))
            return JS_STEP_DONE;   /* nothing concrete to parse; the sink is what this write means */

        DCHECK(JS_IsString(val), "an HTML sink reached the body unconverted — the IDL declaration is what "
                                 "converts it, and running the page's toString from here is the "
                                 "drive-to-completion the flow machinery exists to avoid");
        html = JS_ToCString(ctx, val);
        if (!html) return JS_STEP_ABRUPT;
        if (magic == 0) {
            /* §8.5.4 step 3: a <template>'s children are NOT what innerHTML= replaces — its TEMPLATE CONTENTS
               are, which is a separate tree reached through the element. The parse context stays the element,
               because the tree builder's "in template" insertion mode is what decides whether a <tr> survives;
               only the target of the replacement moves. Without this a page that filled a template this way
               got an element with children nothing renders and a `content` fragment that stayed empty. */
            lxb_dom_node_t *target = n;
            if (lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE)) {
                lxb_html_template_element_t *t = lxb_html_interface_template(n);
                DCHECK(t->content != NULL, "a <template> element in the tree has no content fragment — lexbor's "
                                           "template interface is what owns it, and §4.12.3 gives every "
                                           "template one");
                target = &t->content->node;
            }
            frag_begin(ctx, s, el, target, PLACE_CHILDREN, html, /*clear_first*/ true);
        } else {
            /* §8.5.5 step 6: the fragment is parsed in the PARENT's context, because that is where it lives. */
            frag_begin(ctx, s, lxb_dom_interface_element(n->parent), n, PLACE_REPLACE, html, false);
        }
        JS_FreeCString(ctx, html);
        hdr->stage = FRAG_FEED;
        return JS_STEP_YIELD;
    }
    return frag_step(ctx, hdr, s);
}

static const char *const EL_SET_HTML_STEPS[] = {
    FRAG_STAGES(JS_STEP_STAGE_LABEL) EL_SET_HTML_EXTRA(JS_STEP_STAGE_LABEL) NULL
};

static const IdlStepDecl EL_SET_HTML_STEP = { js_el_set_html, sizeof(FragState), frag_visit, frag_release,
                                              "HTML §8.5.4/§8.5.5 innerHTML/outerHTML setter", EL_SET_HTML_STEPS };

/* §4.9 insertAdjacentHTML / insertAdjacentElement / insertAdjacentText — the SAME four positions, which is why
   one body reads the position and three members differ only in what they place. insertAdjacentHTML is an
   HTML-context sink exactly like innerHTML, and it was absent: a bundle using it had its DOM unbuilt AND its
   XSS invisible, which is the pair this engine exists to report.
   magic 0 = HTML, 1 = Element, 2 = Text. */
/* §4.9's ADJACENT POSITION, shared by the three members that take one. The four names, ASCII
   case-insensitively; anything else is a SyntaxError, not a quiet no-op. Returns false having thrown. */
static bool adjacent_where(JSContext *ctx, JSValueConst posv, lxb_dom_node_t *n, int *pwhere, bool *poutside)
{
    const char *pos = JS_ToCString(ctx, posv);   /* a real string by now: the declaration converted it */

    if (!pos) return false;
    if (!strcasecmp(pos, "beforebegin"))     { *pwhere = PLACE_BEFORE;      *poutside = true;  }
    else if (!strcasecmp(pos, "afterbegin")) { *pwhere = PLACE_FIRST_CHILD; *poutside = false; }
    else if (!strcasecmp(pos, "beforeend"))  { *pwhere = PLACE_CHILDREN;    *poutside = false; }
    else if (!strcasecmp(pos, "afterend"))   { *pwhere = PLACE_AFTER;       *poutside = true;  }
    else {
        JS_FreeCString(ctx, pos);
        JS_ThrowDOMException(ctx, "SyntaxError", "not one of the four adjacent positions");
        return false;
    }
    JS_FreeCString(ctx, pos);
    if (*poutside && (!n->parent || n->parent->type != LXB_DOM_NODE_TYPE_ELEMENT)) {
        JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                             "an adjacent position outside an element with no element parent");
        return false;
    }
    return true;
}

/* §4.9 insertAdjacentHTML — its own declaration, because it is its own algorithm: it PARSES, and the other two
   adjacent members do not. One member whose body forks on a magic between "parse markup" and "insert a node
   the caller already has" would be two algorithms wearing one declaration. */
static int js_el_insert_adjacent_html(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FragState *s = st;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;

    if (hdr->stage == FRAG_TRUSTED) {
        /* §8.5.6 step 1 — before step 2's position parse, which is the order the standard writes: a document
           under a trusted-types policy throws the TypeError for `insertAdjacentHTML("nonsense", s)` rather
           than the SyntaxError the position would have produced. */
        s->compliant = trusted_types_compliant_string(ctx, TRUSTED_TYPE_HTML, argc > 1 ? argv[1] : JS_UNDEFINED,
                                                      "Element insertAdjacentHTML");
        if (JS_IsException(s->compliant)) { s->compliant = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        hdr->stage = FRAG_START;
    }
    if (hdr->stage == FRAG_START) {
        lxb_dom_element_t *el = elem_of(hdr->this_val);
        lxb_dom_node_t *n;
        const char *html;
        int where;
        bool outside;

        if (!el || argc < 2) return JS_STEP_DONE;
        n = lxb_dom_interface_node(el);
        if (!adjacent_where(ctx, argv[0], n, &where, &outside)) return JS_STEP_ABRUPT;
        solve_html_sink(ctx, s->compliant);
        if (concolic_is(s->compliant)) return JS_STEP_DONE;
        DCHECK(JS_IsString(s->compliant), "insertAdjacentHTML reached the body unconverted");
        html = JS_ToCString(ctx, s->compliant);
        if (!html) return JS_STEP_ABRUPT;
        /* Parsed in the context it will LIVE in: the parent for the outside positions, this element for the
           inside ones. A `<td>` inserted beforeend of a `<tr>` survives; parsed against the wrong context it
           would be dropped by the tree builder and the page would find nothing it inserted. */
        frag_begin(ctx, s, outside ? lxb_dom_interface_element(n->parent) : el, n, where, html, false);
        JS_FreeCString(ctx, html);
        hdr->stage = FRAG_FEED;
        return JS_STEP_YIELD;
    }
    return frag_step(ctx, hdr, s);
}

static const char *const EL_ADJACENT_HTML_STEPS[] = { FRAG_STAGES(JS_STEP_STAGE_LABEL) NULL };

static const IdlStepDecl EL_ADJACENT_HTML_STEP = {
    js_el_insert_adjacent_html, sizeof(FragState), frag_visit, frag_release,
    "HTML §8.5.6 Element.insertAdjacentHTML", EL_ADJACENT_HTML_STEPS
};

/* §4.9 insertAdjacentElement / insertAdjacentText — the two that take a node the caller already has, or a
   string this turns into one. magic 1 = element, 2 = text. */
static JSValue js_el_insert_adjacent(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                     int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_node_t *n;
    int where;
    bool outside;

    if (!el || argc < 2) return JS_UNDEFINED;
    n = lxb_dom_interface_node(el);
    if (!adjacent_where(ctx, argv[0], n, &where, &outside)) return JS_EXCEPTION;
    {
        lxb_dom_node_t *added, *ref;
        if (magic == 1) {
            added = node_of(argv[1]);
            if (!added) return JS_ThrowTypeError(ctx, "insertAdjacentElement requires an Element");
        } else {
            const char *s;
            size_t slen = 0;
            lxb_dom_text_t *t;
            s = JS_ToCStringLen(ctx, &slen, argv[1]);
            if (!s) return JS_EXCEPTION;
            t = lxb_dom_document_create_text_node(n->owner_document, (const lxb_char_t *)s, slen);
            JS_FreeCString(ctx, s);
            if (!t) return JS_UNDEFINED;
            added = lxb_dom_interface_node(t);
        }
        ref = (where == PLACE_AFTER) ? n->next : (where == PLACE_FIRST_CHILD ? n->first_child : n);
        switch (where) {
        case PLACE_BEFORE:      dom_cow_insert_before(n, added); break;
        case PLACE_CHILDREN:    dom_cow_append_child(n, added); break;
        case PLACE_AFTER:       if (ref) dom_cow_insert_before(ref, added);
                                else dom_cow_append_child(n->parent, added);
                                break;
        case PLACE_FIRST_CHILD: if (ref) dom_cow_insert_before(ref, added);
                                else dom_cow_append_child(n, added);
                                break;
        default: DFAIL("insertAdjacent ran with an unknown position"); break;
        }
        /* §4.9: insertAdjacentElement returns the inserted node; insertAdjacentText returns undefined. */
        return magic == 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    }
}

/* §4.9 removeAttribute / hasAttribute / toggleAttribute / hasAttributes / getAttributeNames — the rest of the
   attribute family. removeAttribute in particular had no implementation at all, so a boolean reflection had no
   way to UNSET itself and `el.hidden = false` could only ever set. magic: 0 remove, 1 has, 2 toggle. */
static JSValue js_el_attr_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    const char *name;
    size_t vl = 0;
    bool present;
    JSValue r;

    if (!el || argc < 1) return magic == 0 ? JS_UNDEFINED : JS_FALSE;
    name = concolic_name_cstr(ctx, argv[0]);   /* the declaration passes UNKNOWN input through as itself, so an unknown name denotes its SHAPE */
    if (!name) return JS_EXCEPTION;
    present = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vl) != NULL;
    switch (magic) {
    case 0:
        dom_cow_remove_attribute(el, name);
        attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, name, JS_UNDEFINED);   /* the taint goes with the value */
        r = JS_UNDEFINED;
        break;
    case 1:
        r = JS_NewBool(ctx, present);
        break;
    default:
        /* §4.9 toggleAttribute(name, optional force): with no force it flips; with one it sets or removes. */
        DCHECK(magic == 2, "an attribute operation was declared with a magic this file does not name");
        {
            bool want = (argc > 1 && !JS_IsUndefined(argv[1])) ? JS_ToBool(ctx, argv[1]) : !present;
            if (want) dom_cow_set_attribute(el, name, "", 0);
            else {
                dom_cow_remove_attribute(el, name);
                attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, name, JS_UNDEFINED);
            }
            r = JS_NewBool(ctx, want);
        }
        break;
    }
    JS_FreeCString(ctx, name);
    return r;
}

/* §4.9 hasAttributes / getAttributeNames / localName / prefix / namespaceURI — pure reads over the attribute
   list Lexbor already holds. magic: 0 hasAttributes, 1 getAttributeNames. */
static JSValue js_el_attr_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_attr_t *at;
    JSValue arr;
    uint32_t i = 0;

    (void)argc; (void)argv;
    if (!el) return magic == 0 ? JS_FALSE : JS_NewArray(ctx);
    if (magic == 0)
        return JS_NewBool(ctx, el->first_attr != NULL);
    DCHECK(magic == 1, "an attribute-list read was declared with a magic this file does not name");
    arr = JS_NewArray(ctx);
    for (at = el->first_attr; at; at = at->next) {
        size_t n = 0;
        const lxb_char_t *k = lxb_dom_attr_qualified_name(at, &n);
        if (k) JS_SetPropertyUint32(ctx, arr, i++, JS_NewStringLen(ctx, (const char *)k, n));
    }
    return arr;
}

/* §4.9 localName / prefix / namespaceURI — the three parts of an element's NAME the spec keeps apart, and each
   of which Lexbor already interned. magic: 0 localName, 1 prefix, 2 namespaceURI. */
static JSValue js_el_name_part(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    size_t n = 0;
    const lxb_char_t *v;

    if (!el) return JS_NULL;
    switch (magic) {
    case 0: v = lxb_dom_element_local_name(el, &n); break;
    case 1: v = lxb_dom_element_prefix(el, &n);     break;
    default:
        DCHECK(magic == 2, "an element name part was declared with a magic this file does not name");
        v = lxb_ns_by_id(lxb_dom_interface_node(el)->owner_document->ns,
                         lxb_dom_interface_node(el)->ns, &n);
        break;
    }
    if (!v || !n) return magic == 0 ? JS_NewStringLen(ctx, "", 0) : JS_NULL;
    return JS_NewStringLen(ctx, (const char *)v, n);
}

/* [Reflect]ed content attributes: the IDL property IS the attribute, so both directions go through the same
   attribute read (taint shadow first) and the same per-flow write. Spelling the list out rather than generating
   it from the IDL is the gap engine/idlgen.mjs exists to report; what must not happen is a property that answers
   something its attribute does not say, which is exactly what `script.src` did: with no reflection it became an
   ordinary JS property, the element carried no src attribute, and the injected script named a URL nothing would
   ever fetch.
   THE IDL NAME AND THE CONTENT-ATTRIBUTE NAME ARE TWO DIFFERENT STRINGS and the pair is what a reflection is —
   `className` reflects `class`, `htmlFor` reflects `for`, `httpEquiv` reflects `http-equiv`.
   THE TABLE IS NO LONGER ONE FLAT LIST ON Element, because the IDL does not put them there: `src` belongs to
   HTMLScriptElement, HTMLImageElement and HTMLIFrameElement, `content` to HTMLMetaElement, `name` to a dozen
   interfaces. An interface DECLARES its own reflections and hands them to element_install_reflections, which
   assigns each a magic out of one shared registry — so the magic is still an index into one table and the
   bodies below still take exactly one. */
static ElReflect g_reflect[320];
static int       g_reflect_set[320];   /* each entry's setter pool id — declared with it, per AGENT */
static int       g_reflect_n;

static JSValue js_el_reflect_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    JSValue nv, r;
    size_t vl = 0;

    DCHECK(magic >= 0 && magic < g_reflect_n,
           "a reflected property was declared with a magic the registry does not name");
    if (!el) return g_reflect[magic].kind == REFLECT_BOOL ? JS_FALSE : JS_NewStringLen(ctx, "", 0);
    /* §2.2.1 a BOOLEAN reflection is the attribute's PRESENCE, not its value — `<input disabled>` and
       `<input disabled="false">` are both disabled, and a string reflection here would report "false". */
    if (g_reflect[magic].kind == REFLECT_BOOL)
        return JS_NewBool(ctx, lxb_dom_element_get_attribute(el, (const lxb_char_t *)g_reflect[magic].attr,
                                                             strlen(g_reflect[magic].attr), &vl) != NULL);
    nv = JS_NewString(ctx, g_reflect[magic].attr);
    r = js_el_get_attribute(ctx, this_val, 1, (JSValueConst *)&nv, 0);   /* a real string already: the reflected NAME is the engine's */
    JS_FreeValue(ctx, nv);
    return JS_IsNull(r) ? JS_NewStringLen(ctx, "", 0) : r;   /* a reflected string attribute defaults to "" */
}

static JSValue js_el_reflect_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    JSValue verified;

    DCHECK(magic >= 0 && magic < g_reflect_n,
           "a reflected property was declared with a magic the registry does not name");
    if (g_reflect[magic].kind == REFLECT_BOOL) {
        if (!el) return JS_UNDEFINED;
        /* §2.2.1: setting true ADDS the attribute with the empty string, setting false REMOVES it. */
        if (JS_ToBool(ctx, val)) dom_cow_set_attribute(el, g_reflect[magic].attr, "", 0);
        else {
            dom_cow_remove_attribute(el, g_reflect[magic].attr);
            attr_shadow_set(ctx, el, ATTR_SLOT_ATTRIBUTE, g_reflect[magic].attr, JS_UNDEFINED);
        }
        return JS_UNDEFINED;
    }
    /* A REFLECTED ATTRIBUTE THAT IS A TRUSTED TYPES SINK IS ONE THROUGH BOTH SPELLINGS. §3.8's table is a
       table of (element, attribute) pairs, not of member names, so `script.src = s` and
       `script.setAttribute("src", s)` are the same sink and must throw together under
       `require-trusted-types-for 'script'`; HTML says the same thing by declaring srcdoc's IDL type as
       `(TrustedHTML or DOMString)`. Nearly every reflection maps to nothing and this returns the value it was
       given. */
    if (!el) return JS_UNDEFINED;
    {
        char nsbuf[128], lobuf[64];
        const char *ns, *local;

        el_ns_and_local(el, &ns, &local, nsbuf, sizeof(nsbuf), lobuf, sizeof(lobuf));
        verified = trusted_types_compliant_attribute_value(ctx, ns, local, NULL, g_reflect[magic].attr, val);
        if (JS_IsException(verified)) return JS_EXCEPTION;
    }
    el_set_attribute_internal(ctx, this_val, g_reflect[magic].attr, verified);
    JS_FreeValue(ctx, verified);
    return JS_UNDEFINED;
}

/* AN ELEMENT'S CONTENT ATTRIBUTE, for a component that is not a plain reflection. §4.6.3's
   HTMLHyperlinkElementUtils is the case: `a.protocol = "https"` re-serialises a URL and writes it back to the
   `href` ATTRIBUTE, which is a read and a write of the same attribute rather than a mirror of it.
   BOTH GO THROUGH THE SAME CHOKEPOINT the reflection uses, so the write is captured into the running flow's
   DOM delta like every other attribute write — a mixin reaching for lxb_dom_element_set_attribute directly
   would be invisible to time travel, which is exactly what check_dom_chokepoint.mjs exists to prevent.
   Returns an OWNED string, or NULL when the attribute is absent. */
char *element_attr_get(JSContext *ctx, JSValueConst el, const char *name)
{
    JSValue nv = JS_NewString(ctx, name);
    JSValue r = js_el_get_attribute(ctx, el, 1, (JSValueConst *)&nv, 0);
    char *out = NULL;

    JS_FreeValue(ctx, nv);
    if (!JS_IsNull(r) && !JS_IsException(r)) {
        const char *c = JS_ToCString(ctx, r);
        if (c) { out = strdup(c); JS_FreeCString(ctx, c); }
    }
    JS_FreeValue(ctx, r);
    return out;
}

void element_attr_set(JSContext *ctx, JSValueConst el, const char *name, const char *value)
{
    JSValue v = JS_NewString(ctx, value ? value : "");

    el_set_attribute_internal(ctx, el, name, v);
    JS_FreeValue(ctx, v);
}

/* A REFLECTION IS DECLARED ONCE AND INSTALLED PER REALM, like every other member — the registry entry and its
   setter's pool id are the AGENT's, the accessor on a prototype is the REALM's. The caller keeps the BASE index
   the declaration returned, which is what lets the install name the same registry entries again without
   appending a second copy of the table. */
int element_declare_reflections(JSContext *ctx, const ElReflect *r, int n)
{
    int base = g_reflect_n, i;

    for (i = 0; i < n; i++) {
        CHECK(g_reflect_n < (int)(sizeof(g_reflect) / sizeof(g_reflect[0])),
              "the reflection registry is full — raise it rather than dropping an interface's attributes");
        g_reflect[g_reflect_n] = r[i];
        /* A boolean reflection's value is ToBoolean, which is total and runs none of the page's code; a string
           one is a DOMString, which is ToString on whatever the page passed. Two types, one declaration each. */
        g_reflect_set[g_reflect_n] = idl_setter_id(ctx, r[i].kind == REFLECT_BOOL ? IDL_ANY : IDL_DOMSTRING,
                                                   false, js_el_reflect_set, g_reflect_n);
        g_reflect_n++;
    }
    return base;
}

void element_install_reflections(JSContext *ctx, JSValueConst proto, int base, int n)
{
    int i;

    DCHECK(base >= 0 && base + n <= g_reflect_n,
           "an interface installed reflections it never declared — the base index comes from "
           "element_declare_reflections and names the entries that declaration made");
    for (i = 0; i < n; i++)
        idl_install_accessor(ctx, proto, g_reflect[base + i].idl, js_el_reflect_get, base + i,
                             g_reflect_set[base + i]);
}

/* 4.12.1 "prepare the script", the insertion half. A page loads code conditionally in three ways and this is the
   second: `s = createElement("script"); s.src = u; body.appendChild(s)`. Before this the injection was a SILENT
   no-op — the element went into the tree and the code it named was never fetched, never run, never even
   reported, so every endpoint and sink behind an A/B flag or a feature gate was missing with nothing to say so.
   The loaded code is more PROGRAM OF THE INJECTING FLOW: it joins that flow's script sequence, so it runs under
   the delta, the pins and the position in the BFS of the world that injected it, and a sibling that never took
   the branch never sees it.
   INSERTION is the trigger, which is why this is here and not in the innerHTML path: markup parsed into
   innerHTML does not execute its scripts, and that difference is load-bearing for the @S breakout contexts. */
static void element_prepare_script(JSContext *ctx, lxb_dom_element_t *el)
{
    size_t n = 0, vl = 0;
    const lxb_char_t *tag = lxb_dom_element_qualified_name(el, &n);
    const lxb_char_t *src;
    int si;

    if (!tag || n != 6 || memcmp(tag, "script", 6) != 0)
        return;
    /* An UNKNOWN src is a URL this engine cannot fetch, but it is still a request the page makes — recorded so
       it reaches the @H surface as the shape it is, rather than disappearing. */
    si = attr_shadow_find(el, ATTR_SLOT_ATTRIBUTE, "src");
    if (si >= 0) {
        endpoint_record(ctx, "GET", attr_shadow_opaque(si), NULL, 0);
        return;
    }
    src = lxb_dom_element_get_attribute(el, (const lxb_char_t *)"src", 3, &vl);
    if (src && vl) {
        char *u = malloc(vl + 1);
        CHECK(u, "element: OOM copying an injected script's URL");
        memcpy(u, src, vl); u[vl] = 0;
        engine_pending_script_url(ctx, u);
        free(u);
        return;
    }
    /* No src: the element's own text IS the program, and it runs on insertion. */
    {
        lxb_char_t *txt = lxb_dom_node_text_content(lxb_dom_interface_node(el), &n);
        if (txt) {
            if (n) engine_queue_script((const char *)txt);
            lxb_dom_document_destroy_text(lxb_dom_interface_node(el)->owner_document, txt);
        }
    }
}

/* The READ-ONLY members: pure walks over the flow's own tree, so they are ordinary C getters. */
/* §4.9 the three parts of an element's name, each a pure Lexbor read. */
static const JSCFunctionListEntry js_element_name_parts[] = {
    JS_CGETSET_MAGIC_DEF("localName", js_el_name_part, NULL, 0),
    JS_CGETSET_MAGIC_DEF("prefix", js_el_name_part, NULL, 1),
    JS_CGETSET_MAGIC_DEF("namespaceURI", js_el_name_part, NULL, 2),
};

static const JSCFunctionListEntry js_element_readonly[] = {
    JS_CGETSET_DEF("tagName", js_el_get_tag, NULL),
};

/* §4.2.3's INSERTION and REMOVING STEPS, over the whole SUBTREE — inserting a subtree connects every element
   in it, and a page building its UI off-tree and appending the root once is the ordinary case, not a corner.
   Walking only the inserted node meant a custom element inside a built fragment was never upgraded and its
   lifecycle code never ran, which is precisely the code this engine exists to reach.
   Only a CONNECTED node has these steps run for it: §4.13.3 upgrades on entering a document, and a subtree
   moved between two detached parents has entered nothing. */
/* §4.2.3 THE INSERTION AND REMOVING STEPS — RECORDED HERE, WALKED SOMEWHERE THAT CAN YIELD.
 *
 * The steps are a walk of the whole inserted (or removed) subtree, and `container.innerHTML = markup` inserts
 * every node the parse produced in one go. It ran inside the mutation chokepoint, which is inside a C member
 * body, which is the deepest place in this engine with no way to suspend — so the hottest walk in the DOM was
 * also the least interruptible one, and it held the scheduler for as long as the page's markup was large.
 * IT CANNOT SIMPLY BECOME A DEFERRED JOB. §4.2.3 runs these steps synchronously as part of the insertion, and a
 * page that appends an element and then calls a method its upgrade installed depends on that. So the walk moves
 * out of the chokepoint but stays inside the same member call: the chokepoint RECORDS what changed, and the
 * machine every declared member converges on drains the record before the member returns. No page code runs in
 * between, so the ordering the spec states is the ordering that happens — the only thing that changed is that
 * the walk can now yield to another flow, which cannot observe it because a flow's DOM is its own.
 * THE CONNECTEDNESS TEST STAYS HERE, at record time, and that is load-bearing: a REMOVAL fires the hook BEFORE
 * the detach, because "was it connected" has no answer afterwards. The record carries the decision the spec
 * made at mutation time, and the drain never re-derives it.
 * EVERY PER-NODE EFFECT IS AN ENQUEUE — a script queued as a flow, an endpoint recorded, a custom-element
 * reaction enqueued — so nothing the drain does reaches back into the chokepoint and no entry can appear while
 * the walk that would consume it is running. */
typedef struct {
    lxb_dom_node_t *root;     /* the subtree the steps run over */
    lxb_dom_node_t *cursor;   /* how far the walk has got — the resume point */
    uint8_t         inserted;
} TreeStepEntry;

/* The buffer a machine takes ownership of. Per-machine and not global, because the drain YIELDS: a global list
   would be appended to by whichever flow ran during the suspension, and the resuming one would then run another
   flow's insertion steps over another flow's nodes. */
typedef struct { TreeStepEntry *e; int n, i; } TreeStepBuf;

static TreeStepEntry *g_ts;
static int g_ts_n, g_ts_cap;

static void element_tree_changed(JSContext *ctx, lxb_dom_node_t *root, int inserted)
{
    (void)ctx;
    if (!root || !node_is_connected(root)) return;
    if (g_ts_n == g_ts_cap) {
        int want = g_ts_cap ? g_ts_cap * 2 : 8;
        TreeStepEntry *a = realloc(g_ts, sizeof(*a) * (size_t)want);
        CHECK(a != NULL, "the pending tree-steps list could not grow — dropping one means an inserted <script> "
                         "never runs and a custom element never upgrades, silently");
        g_ts = a; g_ts_cap = want;
    }
    g_ts[g_ts_n].root = g_ts[g_ts_n].cursor = root;
    g_ts[g_ts_n].inserted = (uint8_t)(inserted != 0);
    g_ts_n++;
}

/* Hand the running member everything recorded so far, and leave the global empty. Called at every boundary a
   body can return through, so nothing recorded outlives the member that caused it. */
static void *element_tree_steps_take(JSContext *ctx)
{
    TreeStepBuf *b;
    (void)ctx;
    if (!g_ts_n) return NULL;
    b = malloc(sizeof *b);
    CHECK(b != NULL, "the tree-steps buffer could not be allocated");
    b->e = g_ts; b->n = g_ts_n; b->i = 0;
    g_ts = NULL; g_ts_n = g_ts_cap = 0;
    return b;
}

/* ONE NODE. Returns true while there is more to do, which is what makes the caller's loop a yield per node. */
static bool element_tree_steps_step(JSContext *ctx, void *vb)
{
    TreeStepBuf *b = vb;
    TreeStepEntry *e;
    lxb_dom_node_t *n;

    DCHECK(b && b->i < b->n, "the tree-steps drain was stepped past its end");
    e = &b->e[b->i];
    n = e->cursor;
    /* §4.2.3's STEPS RUN IN THE NODE'S DOCUMENT'S REALM, not the mutating member's. Two same-origin documents
       are ONE agent, so `frame.contentDocument.body.appendChild(subframe)` is a mutation this flow may make in
       another document's tree — and its navigable, its <script> preparation and its custom-element upgrade all
       belong to THAT document. The mutating ctx answered the parent's realm for every one of them. */
    ctx = document_realm_of(n);
    DCHECK(ctx != NULL,
           "a tree write reached §4.2.3's steps in a document no realm was installed for — a document that can "
           "hold a connected node is a document a flow can run steps in, so build its realm rather than "
           "borrowing whichever one performed the write");
    if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        lxb_dom_element_t *el = lxb_dom_interface_element(n);
        if (e->inserted) {
            /* §4.8.5: an inserted <iframe> CREATES A CHILD NAVIGABLE, right here, which is where the spec puts
               it — `frame.contentWindow` answers on the line after the append. It does not suspend (the child's
               name is minted locally), so it does not need the enqueue this walk's buffer would otherwise
               demand; it joins <script> preparation and custom-element upgrades as one more per-node effect. */
            {
                size_t qn = 0;
                const lxb_char_t *q = lxb_dom_element_qualified_name(el, &qn);
                /* ASCII-case-insensitive: a parsed `<iframe>` and a `createElement('iframe')` are the same
                   element and must not be told apart by how their name happened to be stored. */
                if (q && qn == 6 && !strncasecmp((const char *)q, "iframe", 6)) {
                    JSValue w = node_wrap(ctx, n);
                    iframe_create_navigable(ctx, w);
                    JS_FreeValue(ctx, w);
                }
            }
            element_prepare_script(ctx, el);   /* HTML 4.12.1: an inserted <script> is PREPARED */
            /* §4.13.3: an element that ENTERS a document is upgraded if its name is defined — the other half of
               "learned by execution", beside the <script> preparation right above it. */
            custom_elements_try_upgrade(ctx, el);
        } else {
            /* §4.8.5's removing steps, the pair of the insertion steps above: an <iframe> that LEAVES a
               document destroys its child navigable. Without it a removed frame kept answering as a live one —
               `contentWindow` stayed non-null and `closed` stayed false, which is precisely what the spec files
               distinguish a destroyed navigable by. */
            size_t qn = 0;
            const lxb_char_t *q = lxb_dom_element_qualified_name(el, &qn);
            if (q && qn == 6 && !strncasecmp((const char *)q, "iframe", 6)) {
                JSValue w = node_wrap(ctx, n);
                iframe_destroy_navigable(ctx, w);
                JS_FreeValue(ctx, w);
            }
            custom_elements_disconnected(ctx, el);
        }
    }
    if (n->first_child) { e->cursor = n->first_child; return true; }
    while (n && !n->next) n = (n == e->root) ? NULL : n->parent;
    n = n ? n->next : NULL;
    e->cursor = n;
    if (n) return true;
    return ++b->i < b->n;
}

static void element_tree_steps_free(JSContext *ctx, void *vb)
{
    TreeStepBuf *b = vb;
    (void)ctx;
    if (!b) return;
    free(b->e);
    free(b);
}

static bool element_tree_steps_recorded(void) { return g_ts_n != 0; }

static const IdlTreeSteps ELEMENT_TREE_STEPS = {
    element_tree_steps_take, element_tree_steps_step, element_tree_steps_free, element_tree_steps_recorded
};

/* §4.9 `[SameObject] readonly attribute NamedNodeMap attributes`. [SameObject] is an identity the IDL states,
   so the map is cached on the element's own wrapper — a page that stashes `el.attributes` and re-reads it must
   be holding the same object. The cache lives on the wrapper because that is per-flow state the COW already
   isolates. */
static JSValue js_el_attributes(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue cached;

    (void)magic;
    if (!elem_of(this_val)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &cached, this_val, g_attrs_key) > 0 && JS_IsObject(cached))
        return cached;
    JS_FreeValue(ctx, cached);
    cached = attr_named_node_map_new(ctx, this_val);
    JS_DefinePropertyValue(ctx, (JSValue)this_val, g_attrs_key, JS_DupValue(ctx, cached), 0);
    return cached;
}

/* §4.9 getAttributeNames() — the qualified names, in order. The cheap half of the same capability: a page that
   only wants the names should not have to build an Attr for each of them. */
static JSValue js_el_attribute_names(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                     int magic)
{
    lxb_dom_element_t *el = elem_of(this_val);
    lxb_dom_attr_t *a;
    JSValue arr;
    uint32_t n = 0;

    (void)argc; (void)argv; (void)magic;
    arr = JS_NewArray(ctx);
    if (!el) return arr;
    for (a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a)) {
        size_t len = 0;
        const lxb_char_t *q = lxb_dom_attr_qualified_name(a, &len);
        if (q) JS_SetPropertyUint32(ctx, arr, n++, JS_NewStringLen(ctx, (const char *)q, len));
    }
    return arr;
}

/* §4.9's attribute change steps, fired by the mutation chokepoint for the same reason the tree steps are:
   `setAttribute`, a reflected IDL attribute, a boolean reflection unsetting itself and innerHTML's parse all
   reach the tree through one function, and a per-caller notification would miss whichever one was added last. */
static void element_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *name,
                                 const char *val, size_t val_len)
{
    /* §4.13.3's attributeChangedCallback BELONGS TO THE ELEMENT'S DOCUMENT, for the reason §4.2.3's steps do:
       the definition set is that document's, so looking it up in the mutating realm finds nothing for an
       element in another same-origin document and the callback silently never fires. */
    JSContext *rctx = document_realm_of(lxb_dom_interface_node(el));

    (void)ctx;
    DCHECK(rctx != NULL,
           "an attribute was set on an element in a document no realm was installed for — §4.13.3's reaction "
           "resolves its definition in that document's registry, so build its realm");
    custom_elements_attribute_changed(rctx, el, name, val, val_len);
}

JSValue element_wrap(JSContext *ctx, lxb_dom_element_t *el)
{
    return node_wrap(ctx, el ? lxb_dom_interface_node(el) : NULL);
}

/* ELEMENT.PROTOTYPE — §4.9, `interface Element : Node`, built ONCE on top of the base node.c owns and handed to
   node.c as the interface every element node wears. It used to be a per-wrapper INSTALLER callback, which minted
   a fresh closure for every member of every element and made `a.getAttribute === b.getAttribute` false. */
/* PER REALM — §3.7. The node-type table names the CLASS; the prototype lives in its per-context slot. */
static JSClassID g_element_class;
/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. */
static int g_refl_base = -1, g_refl_n;
static int g_id_get_attr = -1, g_id_set_attr = -1, g_id_matches = -1, g_id_closest = -1,
           g_id_inner_get = -1, g_id_inner_set = -1, g_id_outer_get = -1, g_id_outer_set = -1,
           g_id_adj_html = -1, g_id_adj_el = -1, g_id_adj_text = -1, g_id_attr_names = -1,
           g_id_remove_attr = -1, g_id_has_attr = -1, g_id_toggle_attr = -1;

void element_init(JSContext *ctx)
{
    static const IdlArgType ADJ_ANY[2] = { IDL_DOMSTRING, IDL_ANY };
    static const IdlArgType TOGGLE[2] = { IDL_DOMSTRING, IDL_ANY };   /* `optional boolean force` is ToBoolean */
    JSClassDef d = { "Element" };

    node_init(ctx);
    JS_NewClassID(JS_GetRuntime(ctx), &g_element_class);
    JS_NewClass(JS_GetRuntime(ctx), g_element_class, &d);
    node_claim_type(LXB_DOM_NODE_TYPE_ELEMENT, g_element_class);

    /* `name`, `value` and `selectors` are DOMStrings, so each is ToString on whatever the page passed:
       `el.getAttribute({toString(){ … }})` is the page's code, and the declaration parks the machine on that
       argument rather than running it out of a C activation. */
    g_id_get_attr = idl_method_id(ctx, IDL_1STR, 1, js_el_get_attribute, 0);
    /* `[CEReactions] undefined setAttribute(DOMString qualifiedName, (TrustedType or DOMString) value)`. The
       union's platform-object arm is §2's three types, which do not exist here, so every value takes the
       DOMString arm — and it becomes an arm the moment §2 lands, in the DECLARATION rather than in the body. */
    g_id_set_attr = idl_method_id_step(ctx, IDL_2STR, 2, NULL, 0, &EL_SET_ATTR_STEP, 0);
    /* §4.9: webkitMatchesSelector is `matches` under its historical name — the IDL declares it as the same
       operation, so it IS the same declaration and not a forwarding wrapper. Both are magics on the one
       selector machine document.c owns, which is what stops four members disagreeing about a selector. */
    g_id_matches = idl_method_id_step(ctx, IDL_1STR, 1, NULL, 0, document_qs_decl(), 2);
    g_id_closest = idl_method_id_step(ctx, IDL_1STR, 1, NULL, 0, document_qs_decl(), 3);
    /* `[CEReactions] attribute [LegacyNullToEmptyString] DOMString innerHTML` — the extended attribute is part
       of the TYPE, so `el.innerHTML = null` empties the element instead of parsing the markup `null`. */
    g_id_inner_get = idl_getter_id_step(ctx, &EL_GET_HTML_STEP, 0);
    g_id_inner_set = idl_setter_id_step(ctx, IDL_DOMSTRING, true, &EL_SET_HTML_STEP, 0);
    g_id_outer_get = idl_getter_id_step(ctx, &EL_GET_HTML_STEP, 1);
    g_id_outer_set = idl_setter_id_step(ctx, IDL_DOMSTRING, true, &EL_SET_HTML_STEP, 1);
    /* §4.9's three adjacent members. The HTML one takes two DOMStrings; the other two take a position and a
       value the IDL leaves alone (a Node, or a DOMString this stringifies into a Text node). */
    g_id_adj_html = idl_method_id_step(ctx, IDL_2STR, 2, NULL, 0, &EL_ADJACENT_HTML_STEP, 0);
    g_id_adj_el = idl_method_id(ctx, ADJ_ANY, 2, js_el_insert_adjacent, 1);
    g_id_adj_text = idl_method_id(ctx, IDL_2STR, 2, js_el_insert_adjacent, 2);
    g_id_attr_names = idl_method_id(ctx, NULL, 0, js_el_attribute_names, 0);
    g_id_remove_attr = idl_method_id(ctx, IDL_1STR, 1, js_el_attr_op, 0);
    g_id_has_attr = idl_method_id(ctx, IDL_1STR, 1, js_el_attr_op, 1);
    g_id_toggle_attr = idl_method_id(ctx, TOGGLE, 2, js_el_attr_op, 2);
    idl_optional_from(1);   /* §4.9: `toggleAttribute(qualifiedName, optional force)` */
    {
        static const ElReflect R[] = {
            { "id", "id", REFLECT_STRING }, { "className", "class", REFLECT_STRING },
            { "slot", "slot", REFLECT_STRING },
        };
        g_refl_n = (int)(sizeof(R) / sizeof(R[0]));
        g_refl_base = element_declare_reflections(ctx, R, g_refl_n);
    }

    idl_indexed_init(ctx);      /* the exotic class every indexed interface is built on */
    attr_init(ctx);             /* §4.9.2 Attr — registered for node type 2, which node_wrap had no interface for */
    g_attrs_key = JS_NewAtom(ctx, "__attributesSlot");
    CHECK(g_attrs_key != JS_ATOM_NULL, "the attributes slot key could not be interned");
    collections_init(ctx);      /* NodeList and HTMLCollection, which childNodes and children are */
    dom_token_list_init(ctx);   /* its class must exist before classList names it */
    node_set_tree_hook(element_tree_changed);
    idl_set_tree_steps(&ELEMENT_TREE_STEPS);
    dom_cow_set_attr_hook(element_attr_changed);
    realm_declare_intrinsic(element_install_proto);
    custom_elements_init(ctx);
    cssom_init(ctx);          /* CSSStyleDeclaration, which HTMLElement's `style` attribute names */
    html_element_init(ctx);   /* the HTML half, which builds HTMLElement and the per-tag interfaces on this */
}

/* §4.9's INTERFACE PROTOTYPE OBJECT, FOR ONE REALM. It is DECLARED before custom_elements/cssom/html_element
   above so that the list runs it FIRST — every one of those chains its own prototypes to this one. */
void element_install_proto(JSContext *ctx)
{
    JSValue proto, base, prev;

    prev = JS_GetClassProto(ctx, g_element_class);
    DCHECK(JS_IsNull(prev), "element_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    base = node_proto(ctx);
    proto = JS_NewObjectProto(ctx, base);
    JS_FreeValue(ctx, base);
    CHECK(!JS_IsException(proto), "Element.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "Element");
    JS_SetPropertyFunctionList(ctx, proto, js_element_readonly,
                               (int)(sizeof(js_element_readonly) / sizeof(js_element_readonly[0])));
    idl_install_method(ctx, proto, "getAttribute", 1, g_id_get_attr);
    idl_install_method(ctx, proto, "setAttribute", 2, g_id_set_attr);
    idl_install_method(ctx, proto, "matches", 1, g_id_matches);
    idl_install_method(ctx, proto, "webkitMatchesSelector", 1, g_id_matches);
    idl_install_method(ctx, proto, "closest", 1, g_id_closest);
    dom_token_list_install_element(ctx, proto);   /* §4.9's [SameObject] classList */
    node_install_child_mixin(ctx, proto);    /* remove / before / after / replaceWith */
    node_install_parent_mixin(ctx, proto);   /* append / prepend / replaceChildren */
    idl_install_accessor_step(ctx, proto, "innerHTML", g_id_inner_get, g_id_inner_set);
    idl_install_accessor_step(ctx, proto, "outerHTML", g_id_outer_get, g_id_outer_set);
    idl_install_method(ctx, proto, "insertAdjacentHTML", 2, g_id_adj_html);
    idl_install_method(ctx, proto, "insertAdjacentElement", 2, g_id_adj_el);
    idl_install_method(ctx, proto, "insertAdjacentText", 2, g_id_adj_text);
    /* The rest of the attribute family. removeAttribute had no implementation at all, which is also why a
       boolean reflection could not unset itself. */
    idl_install_accessor(ctx, proto, "attributes", js_el_attributes, 0, -1);
    idl_install_method(ctx, proto, "getAttributeNames", 0, g_id_attr_names);
    idl_install_method(ctx, proto, "removeAttribute", 1, g_id_remove_attr);
    idl_install_method(ctx, proto, "hasAttribute", 1, g_id_has_attr);
    idl_install_method(ctx, proto, "toggleAttribute", 1, g_id_toggle_attr);
    JS_SetPropertyStr(ctx, proto, "hasAttributes",
                      JS_NewCFunctionMagic(ctx, js_el_attr_list, "hasAttributes", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, proto, "getAttributeNames",
                      JS_NewCFunctionMagic(ctx, js_el_attr_list, "getAttributeNames", 0, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyFunctionList(ctx, proto, js_element_name_parts,
                               (int)(sizeof(js_element_name_parts) / sizeof(js_element_name_parts[0])));
    /* §4.9's OWN reflections, and only those: `id`, `class` and `slot`. `src`, `name` and `content` used to be
       here too, which is three properties Element's IDL does not declare — they belong to HTMLScriptElement,
       to a dozen form interfaces and to HTMLMetaElement, and they are installed there now. */
    element_install_reflections(ctx, proto, g_refl_base, g_refl_n);
    /* GlobalEventHandlers is NOT on Element — the IDL mixes it into HTMLElement, which is where it is
       installed now that that interface exists. */
    JS_SetClassProto(ctx, g_element_class, proto);
}

JSValue element_proto(JSContext *ctx)
{
    JSValue proto = JS_GetClassProto(ctx, g_element_class);
    DCHECK(!JS_IsNull(proto), "Element.prototype was asked for in a realm that never ran element_install_proto");
    return proto;   /* OWNED */
}

void element_free(JSContext *ctx)
{
    /* NODE.C IS PART OF THIS GROUP AND WAS THE ONE MEMBER MISSING FROM THE CASCADE. It owns the WRAPPER
       IDENTITY TABLE, which holds a reference to every node wrapper ever minted — and a wrapper holds its
       prototype, which holds the realm, so four surviving wrappers kept the whole context alive: MEASURED as
       2200 retained objects and a JSContext at refcount 2212 on the shipped entry, reported by the runtime's
       leak walk as anonymous Functions with nothing naming the owner. */
    node_free(ctx);
    html_element_free(ctx);
    cssom_free(ctx);
    custom_elements_free(ctx);
    dom_token_list_free(ctx);
    collections_free(ctx);
    attr_free(ctx);
    if (g_attrs_key != JS_ATOM_NULL) { JS_FreeAtom(ctx, g_attrs_key); g_attrs_key = JS_ATOM_NULL; }
    document_fragment_free(ctx);
    idl_indexed_free(ctx);
    /* the prototypes are the REALMS' — each is released with its context */
    g_reflect_n = 0;
    node_free(ctx);
}
