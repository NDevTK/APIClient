/* THE HTML FRAGMENT SERIALIZATION ALGORITHM — HTML §13.3, as ONE machine, for every member that is it.
 *
 * WHY IT IS A COMPONENT AND NOT A BLOCK OF element.c. §13.3 is the algorithm behind FOUR members across TWO
 * interfaces — §8.5.4's innerHTML getter on Element and on ShadowRoot, §8.5.5's outerHTML getter, and §8.5.3's
 * getHTML on both — and the ONLY thing that differs between them is where the walk starts and what the two
 * shadow-root options are. Written once per interface it becomes two walks that disagree about `<template>`,
 * about void elements and about which shadow roots are emitted; the one that lived in element.c was already
 * the copy every getter shared, and the first thing this diff had to do was make ShadowRoot reach it rather
 * than grow a second.
 *
 * WHAT WAS MISSING, AND WHY IT IS THE SAME GAP AS THE PARSER'S. `<template shadowrootmode>` became a real
 * shadow root when the declarative parser landed, and nothing could serialize one back: `getHTML` did not
 * exist, `ShadowRoot.serializable` was a flag with no reader, and `element.innerHTML` answered a string in
 * which the component's entire shadow tree was absent with nothing to say so. A page that round-trips its own
 * markup (`host.innerHTML = host.innerHTML`, the ordinary spelling) therefore DELETED its shadow trees, and
 * the engine reported the moat of a page whose components had been erased.
 *
 * LEXBOR OWNS THE SERIALIZATION OF ONE NODE and this file owns the WALK. The escaping, the attribute quoting
 * and the raw-text elements are HTML's own rules and lexbor implements them beside the parser that reads them
 * back; hand-rolling a second copy here would be a serializer that disagrees with the parser sitting next to
 * it. What lexbor cannot do is SUSPEND: `lxb_html_serialize_tree_str` runs a walk of the PAGE'S SIZE inside
 * one opcode, and `document.body.outerHTML` on a real page held the scheduler for the whole document with
 * every other flow parked behind it. So the walk is a machine that emits ONE node per step and yields, and
 * lexbor is asked for that one node.
 *
 * TWO DELIBERATE DIVERGENCES FROM LEXBOR, AND THE SPEC DECIDES BOTH.
 *   - §13.3 step 3: "If the node is a template element, then let the node instead be the template element's
 *     template contents" — the template is REPLACED by its content, so its ordinary children (which
 *     `t.appendChild(x)` really does create, since only the parser and `t.content` reach the fragment) are not
 *     serialized at all. Lexbor emits the content and THEN descends into first_child, which prints them.
 *   - §13.3 step 4, which lexbor has no notion of at all: a shadow host's shadow root is serialized, BEFORE
 *     the host's own children, as the `<template shadowrootmode>` the parser reads back.
 *
 * THE CLOSING TAG is the one piece lexbor does not export (`lxb_html_serialize_element_closed_cb` is static),
 * so it is emitted here from the same qualified name and gated on the same public `lxb_html_node_is_void` —
 * whose element list is exactly §13.3's "serializes as void".
 *
 * WHAT THE WALK IS. §13.3 recurses, and a machine's C stack is gone at every suspension, so the recursion is
 * an explicit stack of LEVELS. A level is "the children of some node", and there are three ways to be inside
 * one: an element's children, a `<template>`'s contents, and a shadow root's children — the first two resume
 * by closing the element they belong to, the third by closing the `<template>` §13.3 step 4 invented and then
 * serializing the host's OWN children. That difference is the whole of `SerLevel.kind`. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>
#include <lexbor/html/serialize.h>

#include "check.h"
#include "quickjs.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/html/fragment_serializer.h"
#include "core/idl_args.h"

static int g_ready;
static int g_id_get_html = -1;

/* ---- the accumulator and the one-node emitters ---------------------------------------------------------- */

/* A LEVEL of the walk. `node` is the element the level belongs to — the one whose end tag (or whose invented
   `</template>`) is written when the level is exhausted — and `limit` is the container the level was entered
   FROM, restored with it. */
enum { SER_LEVEL_ELEMENT = 0, SER_LEVEL_SHADOW };
typedef struct { lxb_dom_node_t *node; lxb_dom_node_t *limit; uint8_t kind; } SerLevel;

/* WHERE THIS MACHINE RESTS, AS THE STANDARD NUMBERS IT. Every stage is a step of §13.3 and nothing here can
   reach the page's code, so no stage has to split further — but each is its own rest point, because the walk
   is of the page's size and a flow parked in it must be able to say which of the algorithm's steps it is at
   in the next session as in this one. */
#define FRAGSER_STAGES(X) \
    X(FRAGSER_SETUP,  "HTML §8.5.4 innerHTML getter / §8.5.5 outerHTML getter / §8.5.3 getHTML step 1 (the " \
                      "node whose children are serialized, and §13.3's serializableShadowRoots and " \
                      "shadowRoots arguments)") \
    X(FRAGSER_SHADOW, "HTML §13.3 step 4 (a shadow host's `<template shadowrootmode>` start tag, and the " \
                      "descent into the shadow root this algorithm recurses over)") \
    X(FRAGSER_ENTER,  "HTML §13.3 step 3 and step 5's descent (a template element replaced by its template " \
                      "contents, and the first child of the node being serialized)") \
    X(FRAGSER_EMIT,   "HTML §13.3 step 5.2 (append this child's start tag, character data, comment, " \
                      "processing instruction or doctype)") \
    X(FRAGSER_CLOSE,  "HTML §13.3 step 5.2's \"</\", tagname, U+003E (the child's end tag, which a void " \
                      "element does not have, and the step to the next child in tree order)") \
    X(FRAGSER_POP,    "HTML §13.3 step 5.2's return from the recursive invocation (this level is exhausted, " \
                      "so the element it belongs to is closed; a shadow level appends \"</template>\" and " \
                      "the host's own children follow)")
enum { IDL_STEP_STAGE_BASE(FRAGSER_STAGES) FRAGSER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const FRAGSER_STEPS[] = { FRAGSER_STAGES(JS_STEP_STAGE_LABEL) NULL };

typedef struct {
    /* §13.3's `the node` when the OUTPUT is its children — NULL for §8.5.5's outerHTML, whose output is the
       node itself and whose enclosing level is therefore a fictional parent with exactly one child. It is what
       tells FRAGSER_ENTER not to push a level for the top: the top node's own tags are not part of the output,
       and an exhausted stack IS the end of the walk. */
    lxb_dom_node_t *top;
    lxb_dom_node_t *cur;      /* the child being processed */
    lxb_dom_node_t *limit;    /* the container `cur` is a child of; NULL only for outerHTML's fictional parent */
    SerLevel       *stack;
    int             sp, scap;
    char           *out;      /* the accumulator: js_malloc'd, because a fork gives each arm its own copy */
    size_t          out_len, out_cap;
    /* §13.3's TWO ARGUMENTS. `shadow_roots` is step 4.2's list — an engine-built Array of ShadowRoot wrappers
       the DECLARATION converted (IDL_SEQUENCE_INTERFACE), so nothing of the page's is left on it and reading it
       runs none of the page's code. JS_UNDEFINED is « », which is what §8.5's `= []` default means. */
    bool            serializable_shadow_roots;
    JSValue         shadow_roots;
} FragSerState;

/* THE RUNTIME'S ALLOCATOR, BECAUSE THE DECLARATION'S IS. js_frag_ser_visit hands a forked arm a js_malloc'd
   copy of this accumulator and the teardown discharges it with js_free, so growing it through the C library
   would hand the runtime a block it never issued to free. That is also why the context is a PARAMETER: the
   pair below is what lexbor's one-pointer callback carries, rather than a JSContext stored on a state whose
   whole purpose is to be copied to a sibling and parked to the cold tier. */
static void ser_append(JSContext *ctx, FragSerState *s, const lxb_char_t *data, size_t len)
{
    if (s->out_len + len + 1 > s->out_cap) {
        size_t want = s->out_cap ? s->out_cap * 2 : 256;
        char *n;
        while (want < s->out_len + len + 1) want *= 2;
        n = js_realloc(ctx, s->out, want);
        CHECK(n != NULL, "the HTML fragment serializer could not grow its accumulator");
        s->out = n;
        s->out_cap = want;
    }
    memcpy(s->out + s->out_len, data, len);
    s->out_len += len;
}

/* WHAT LEXBOR'S SERIALIZER IS HANDED — the accumulator and the runtime that owns it, as one context. */
typedef struct { JSContext *ctx; FragSerState *s; } SerSink;

static lxb_status_t ser_sink_cb(const lxb_char_t *data, size_t len, void *vctx)
{
    SerSink *k = vctx;

    ser_append(k->ctx, k->s, data, len);
    return LXB_STATUS_OK;
}

static void ser_str(JSContext *ctx, FragSerState *s, const char *lit)
{
    ser_append(ctx, s, (const lxb_char_t *)lit, strlen(lit));
}

/* `</name>`, which lexbor emits from a static function. A void element has none, and neither does anything
   that is not an element — the same two conditions lexbor's own ascent tests, and §13.3 step 5.2's own. */
static void ser_close(JSContext *ctx, FragSerState *s, lxb_dom_node_t *n)
{
    const lxb_char_t *name;
    size_t len = 0;

    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT || lxb_html_node_is_void(n)) return;
    name = lxb_dom_element_qualified_name(lxb_dom_interface_element(n), &len);
    DCHECK(name != NULL, "an element in the tree has no qualified name to close");
    ser_str(ctx, s, "</");
    ser_append(ctx, s, name, len);
    ser_str(ctx, s, ">");
}

/* HTML §13.3's TEXT CASE, WRITTEN HERE BECAUSE THE ALGORITHM DECIDES BY INTERFACE AND LEXBOR DECIDES BY
   nodeType. "If current node is a Text node" is true of a CDATASection — §4.12 is `CDATASection : Text` — but
   lexbor's `lxb_html_serialize_cb` switches on `node->type` and has no CDATA arm at all, so it returned
   LXB_STATUS_ERROR and the DCHECK below fired. That abort was the whole of what seventeen dom/ranges and
   dom/traversal files reported, because dom/common.js builds its sixth paragraph out of two CDATA sections and
   every one of those files serializes the fixture to name its subtests.
   Lexbor's per-kind serializers are internal, so this is §13.3 ported rather than delegated — the last rung of
   "bind before build", and the port is one escape table.
   §13.3: a Text node whose PARENT is a raw-text element is appended literally; otherwise its data is escaped
   with the attribute-mode flag unset, which is `&`, U+00A0, `<` and `>`. */
static bool ser_parent_is_raw_text(const lxb_dom_node_t *n)
{
    lxb_dom_node_t *p = n->parent;

    if (!p || p->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    return lxb_html_tree_node_is(p, LXB_TAG_STYLE)    || lxb_html_tree_node_is(p, LXB_TAG_SCRIPT) ||
           lxb_html_tree_node_is(p, LXB_TAG_XMP)      || lxb_html_tree_node_is(p, LXB_TAG_IFRAME) ||
           lxb_html_tree_node_is(p, LXB_TAG_NOEMBED)  || lxb_html_tree_node_is(p, LXB_TAG_NOFRAMES) ||
           lxb_html_tree_node_is(p, LXB_TAG_PLAINTEXT) || lxb_html_tree_node_is(p, LXB_TAG_NOSCRIPT);
}

static void ser_text_node(JSContext *ctx, FragSerState *s, lxb_dom_node_t *n)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;
    const lxb_char_t *d = cd->data.data;
    size_t len = cd->data.length, i, run = 0;

    if (ser_parent_is_raw_text(n)) { ser_append(ctx, s, d, len); return; }
    for (i = 0; i < len; i++) {
        const char *rep = NULL;
        size_t skip = 1;

        if (d[i] == '&')      rep = "&amp;";
        else if (d[i] == '<') rep = "&lt;";
        else if (d[i] == '>') rep = "&gt;";
        else if (d[i] == 0xC2 && i + 1 < len && d[i + 1] == 0xA0) { rep = "&nbsp;"; skip = 2; }  /* U+00A0 */
        if (!rep) { run++; continue; }
        if (run) ser_append(ctx, s, d + i - run, run);
        run = 0;
        ser_append(ctx, s, (const lxb_char_t *)rep, strlen(rep));
        i += skip - 1;
    }
    if (run) ser_append(ctx, s, d + len - run, run);
}

/* §13.3 step 5.2's KIND SWITCH for ONE node — its start tag with its attributes, or its character data, or its
   comment/PI/doctype form. Lexbor's callback serializer is exactly this switch and nothing more: it emits the
   node itself and never descends, which is what makes it usable one node at a time. */
static void ser_one_node(JSContext *ctx, FragSerState *s, lxb_dom_node_t *n)
{
    if (n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION) {
        ser_text_node(ctx, s, n);   /* §13.3's Text case — see ser_text_node */
        return;
    }
    {
        SerSink sink = { ctx, s };
        lxb_status_t status = lxb_html_serialize_cb(n, ser_sink_cb, &sink);
        DCHECK(status == LXB_STATUS_OK, "lexbor refused to serialize a node kind this tree contains");
        (void)status;
    }
}

static void ser_push(JSContext *ctx, FragSerState *s, lxb_dom_node_t *node, lxb_dom_node_t *limit, int kind)
{
    if (s->sp == s->scap) {
        int want = s->scap ? s->scap * 2 : 8;
        SerLevel *n = js_realloc(ctx, s->stack, sizeof(SerLevel) * (size_t)want);
        CHECK(n != NULL, "the HTML fragment serializer could not grow its level stack");
        s->stack = n;
        s->scap = want;
    }
    s->stack[s->sp].node = node;
    s->stack[s->sp].limit = limit;
    s->stack[s->sp].kind = (uint8_t)kind;
    s->sp++;
}

/* A `<template>`'s template contents. Every template element has one — §4.12.3 establishes it when the element
   is created, and lexbor's template interface constructor is where that happens here. */
static lxb_dom_node_t *ser_template_contents(lxb_dom_node_t *n)
{
    lxb_html_template_element_t *t = lxb_html_interface_template(n);

    DCHECK(t->content != NULL, "a <template> element has no template contents — §4.12.3 establishes them when "
                               "the element is created, so an element without them was made some other way");
    return &t->content->node;
}

/* ---- §13.3 step 4's shadow-root branch ------------------------------------------------------------------- */

/* Step 4.2's TWO conditions, either of which includes this shadow root in the output: it is SERIALIZABLE and
   the caller asked for serializable ones, or the caller NAMED it. The second is a list the page passed, so the
   membership test is over object IDENTITY — a shadow root is one node and one wrapper, so comparing the nodes
   the wrappers hold IS `===`. It runs none of the page's code: the declaration already converted the sequence,
   and what is left is an engine-built Array of platform objects. */
static bool ser_shadow_included(JSContext *ctx, const FragSerState *s, lxb_dom_node_t *shadow)
{
    uint32_t n = 0, i;
    JSValue len;

    if (s->serializable_shadow_roots && shadow_root_flag(ctx, shadow, SHADOW_ROOT_SERIALIZABLE)) return true;
    if (!JS_IsObject(s->shadow_roots)) return false;
    len = JS_GetPropertyStr(ctx, s->shadow_roots, "length");
    JS_ToUint32(ctx, &n, len);
    JS_FreeValue(ctx, len);
    for (i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, s->shadow_roots, i);
        lxb_dom_node_t *en = node_of(e);

        JS_FreeValue(ctx, e);
        DCHECK(en != NULL && shadow_root_is(en),
               "§8.5's shadowRoots list holds something that is not a shadow root — the sequence's element type "
               "is what brands it, so a non-ShadowRoot here means the declaration lost its narrowing");
        if (en == shadow) return true;
    }
    return false;
}

/* Steps 4.2.1-4.2.10: the `<template>` start tag §13.2.6.4.4 reads back. The attribute ORDER is the standard's
   own, because the output of this algorithm is markup a parser consumes and a round-trip is what both halves
   are for. Every attribute is the BOOLEAN spelling (`=""`) except slot assignment, which is enumerated. */
static void ser_shadow_start_tag(JSContext *ctx, FragSerState *s, lxb_dom_node_t *shadow)
{
    ser_str(ctx, s, "<template shadowrootmode=\"");
    ser_str(ctx, s, shadow_root_is_open(shadow) ? "open" : "closed");
    ser_str(ctx, s, "\"");
    if (shadow_root_flag(ctx, shadow, SHADOW_ROOT_DELEGATES_FOCUS)) ser_str(ctx, s, " shadowrootdelegatesfocus=\"\"");
    if (shadow_root_flag(ctx, shadow, SHADOW_ROOT_SERIALIZABLE))    ser_str(ctx, s, " shadowrootserializable=\"\"");
    if (shadow_root_slot_assignment_is_manual(ctx, shadow))         ser_str(ctx, s, " shadowrootslotassignment=\"manual\"");
    if (shadow_root_flag(ctx, shadow, SHADOW_ROOT_CLONABLE))        ser_str(ctx, s, " shadowrootclonable=\"\"");
    /* Step 4.2.9's `shouldAppendRegistryAttribute` is DECIDED here rather than absent: it asks whether the
       shadow root's custom element registry differs from its node document's, and this engine has exactly one
       registry — the document's — which shadow_root.c records by name as the reason `ShadowRootInit`'s
       `customElementRegistry` member does not exist. Both reads therefore answer the same global registry and
       step 4.2.9.2 returns false, so the attribute is never appended. It becomes a real read in the diff that
       makes a scoped registry attachable. */
    ser_str(ctx, s, ">");
}

/* ---- the machine ----------------------------------------------------------------------------------------- */

static int js_frag_ser_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                            JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FragSerState *s = st;
    int magic = idl_step_magic(hdr);

    (void)cb_result; (void)out_cb; (void)out_argc;

    switch (hdr->stage) {
    case FRAGSER_SETUP: {
        lxb_dom_node_t *n = node_of(hdr->this_val);

        /* THE STATE IS ZEROED, AND A ZEROED JSValue IS THE INTEGER 0 — JS_TAG_INT is 0 — so a field that
           carries a value has to be given one before anything can read it as "absent". */
        s->shadow_roots = JS_UNDEFINED;

        /* WEB IDL §3.7.5's BRAND CHECK, and it is a THROW: a page reaches an accessor or a method off the
           prototype with `.call` on anything it likes, so what the receiver is is the PAGE's input and not
           this engine's invariant. §8.5.5's outerHTML is declared on Element alone; the other two are declared
           on Element and on ShadowRoot, which is a DocumentFragment and not an element. */
        if (!n || (n->type != LXB_DOM_NODE_TYPE_ELEMENT &&
                   (magic == FRAGMENT_SERIALIZE_SELF || !shadow_root_is(n)))) {
            JS_ThrowTypeError(ctx, "the HTML fragment serialization algorithm was reached on something that is "
                                   "neither an Element nor a ShadowRoot");
            return JS_STEP_ABRUPT;
        }
        if (magic == FRAGMENT_SERIALIZE_GET_HTML) {
            JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;

            /* Both of §8.5's members, as the declaration converted them: the iterator protocol behind
               `shadowRoots` ran during the DICTIONARY's conversion, in the order §3.2.18 reads it, and what is
               here is its result. */
            s->shadow_roots = idl_dict_get(ctx, options, "shadowRoots");
            s->serializable_shadow_roots = idl_dict_bool(ctx, options, "serializableShadowRoots");
        }
        if (magic == FRAGMENT_SERIALIZE_SELF) {
            /* §8.5.5's getter serializes THIS element as the only child of a fictional parent. A NULL `limit`
               IS that parent: the level holds one node, so FRAGSER_CLOSE ends the walk there instead of
               stepping to the element's real next sibling — which it did, and `a.outerHTML` on an element with
               a following sibling returned that sibling's markup too. */
            s->top = NULL;
            s->cur = n;
            s->limit = NULL;
            hdr->stage = FRAGSER_EMIT;
            return JS_STEP_YIELD;
        }
        /* §13.3 step 1. A void element has no children, so the serialization of its children is the empty
           string — stated as its own step because the algorithm is reachable with such a node. */
        if (lxb_html_node_is_void(n)) {
            *presult = JS_NewStringLen(ctx, "", 0);
            return JS_STEP_DONE;
        }
        s->top = n;
        s->cur = n;
        s->limit = NULL;
        hdr->stage = FRAGSER_SHADOW;
        return JS_STEP_YIELD;
    }

    case FRAGSER_SHADOW: {
        lxb_dom_node_t *shadow = NULL;

        if (s->cur->type == LXB_DOM_NODE_TYPE_ELEMENT)
            shadow = shadow_root_of_element(ctx, lxb_dom_interface_element(s->cur));
        /* Step 5 follows either way — the shadow tree is emitted BEFORE the host's own children, never
           instead of them. */
        hdr->stage = FRAGSER_ENTER;
        if (!shadow || !ser_shadow_included(ctx, s, shadow))
            return JS_STEP_YIELD;
        ser_shadow_start_tag(ctx, s, shadow);
        if (!shadow->first_child) {
            /* Step 4.2.11's recursion over an empty shadow root is the empty string; the level would be
               pushed and popped with nothing between, so the close is written here instead. */
            ser_str(ctx, s, "</template>");
            return JS_STEP_YIELD;
        }
        ser_push(ctx, s, s->cur, s->limit, SER_LEVEL_SHADOW);
        s->limit = shadow;
        s->cur = shadow->first_child;
        hdr->stage = FRAGSER_EMIT;
        return JS_STEP_YIELD;
    }

    case FRAGSER_ENTER: {
        lxb_dom_node_t *container = s->cur;

        DCHECK(s->cur != NULL, "the fragment serializer entered the children of no node");
        DCHECK(s->cur->type == LXB_DOM_NODE_TYPE_ELEMENT || s->cur == s->top,
               "the fragment serializer descended into a node that is neither an element nor the node it was "
               "asked to serialize the children of — §13.3 step 5.2 recurses for elements and for nothing else");
        /* §13.3 step 3. The template's OWN children are not serialized at all: the standard replaces the node
           with its contents, and only the parser and `t.content` can put anything in either. */
        if (container->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_html_tree_node_is(container, LXB_TAG_TEMPLATE))
            container = ser_template_contents(container);
        /* THE TOP LEVEL IS NOT PUSHED. Its node's own start and end tags are not part of the output (that is
           what "serializes the CHILDREN of the node" means), so there is nothing to resume into and an
           exhausted stack IS the end of the walk. */
        if (!(s->cur == s->top && s->sp == 0))
            ser_push(ctx, s, s->cur, s->limit, SER_LEVEL_ELEMENT);
        s->limit = container;
        s->cur = container->first_child;
        hdr->stage = s->cur ? FRAGSER_EMIT : FRAGSER_POP;
        return JS_STEP_YIELD;
    }

    case FRAGSER_EMIT:
        DCHECK(s->cur != NULL, "the fragment serializer resumed at step 5.2 with no child to append");
        ser_one_node(ctx, s, s->cur);
        /* §13.3 step 5.2: an element that serializes as void "continues on to the next child node at this
           point" — no recursion and no end tag — and nothing that is not an element has children to recurse
           into. Everything else reaches step 4 and then step 5 for itself. */
        hdr->stage = (s->cur->type == LXB_DOM_NODE_TYPE_ELEMENT && !lxb_html_node_is_void(s->cur))
                     ? FRAGSER_SHADOW : FRAGSER_CLOSE;
        return JS_STEP_YIELD;

    case FRAGSER_CLOSE:
        ser_close(ctx, s, s->cur);
        if (!s->limit) {
            DCHECK(s->sp == 0 && magic == FRAGMENT_SERIALIZE_SELF,
                   "a level with no container appeared inside the walk — only §8.5.5's fictional parent has "
                   "none, and it holds exactly one node");
            break;
        }
        if (s->cur->next) {
            s->cur = s->cur->next;
            hdr->stage = FRAGSER_EMIT;
            return JS_STEP_YIELD;
        }
        hdr->stage = FRAGSER_POP;
        return JS_STEP_YIELD;

    case FRAGSER_POP: {
        SerLevel lv;

        if (s->sp == 0) break;             /* the walk itself is over */
        lv = s->stack[--s->sp];
        s->cur = lv.node;
        s->limit = lv.limit;
        if (lv.kind == SER_LEVEL_SHADOW) {
            /* Step 4.2.12, and then step 5 for the host: its own light children follow its shadow tree. The
               host's step 4 has already run, which is why this resumes at ENTER and not at SHADOW. */
            ser_str(ctx, s, "</template>");
            hdr->stage = FRAGSER_ENTER;
        } else {
            hdr->stage = FRAGSER_CLOSE;
        }
        return JS_STEP_YIELD;
    }

    default:
        DFAIL("the fragment serializer resumed into a stage §13.3 does not have");
    }
    *presult = JS_NewStringLen(ctx, s->out ? s->out : "", s->out_len);
    return JS_STEP_DONE;
}

static void js_frag_ser_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    FragSerState *s = st;

    /* Both are plain storage a forked arm must not share: the two arms append their own remaining nodes to the
       accumulator, and each unwinds its own level stack. The DOM pointers inside are per-flow COW nodes, which
       every arm reaches by the same address. */
    v->buf(ctx, (void **)&s->out, s->out_cap);
    v->buf(ctx, (void **)&s->stack, sizeof(SerLevel) * (size_t)s->scap);
    v->val(ctx, &s->shadow_roots);
}

static const IdlStepDecl FRAGMENT_SERIALIZER_STEP = {
    /* No release: the accumulator, the level stack and `shadow_roots` are all js_frag_ser_visit's, and the
       teardown discharges that one list. */
    js_frag_ser_step, sizeof(FragSerState), js_frag_ser_visit, NULL,
    "HTML §13.3 the HTML fragment serialization algorithm (§8.5.3 getHTML, §8.5.4 innerHTML getter, "
    "§8.5.5 outerHTML getter)",
    FRAGSER_STEPS
};

const IdlStepDecl *fragment_serializer_decl(void)
{
    return &FRAGMENT_SERIALIZER_STEP;
}

/* ---- declaration and installation ------------------------------------------------------------------------ */

/* §8.5's `dictionary GetHTMLOptions`. Web IDL §3.2.18 reads a dictionary's members LEXICOGRAPHICALLY, which
   for these two is also their declaration order. `shadowRoots` is `sequence<ShadowRoot>`, and it is the
   DECLARATION that runs §3.2.21's iterator protocol over it — the page's iterator, its `next`, and every
   `done`/`value` read park this machine on the element they are on, in the order §3.2.18 states, which a body
   walking the list afterwards could not reproduce. */
static const IdlDictMember GET_HTML_OPTIONS[] = {
    { "serializableShadowRoots", IDL_BOOLEAN,             false, NULL, 0 },
    { "shadowRoots",             IDL_SEQUENCE_INTERFACE,  false, NULL, 0 },
};
static const IdlArgType ONE_DICT[1] = { IDL_DICT };

void fragment_serializer_init(JSContext *ctx)
{
    DCHECK(!g_ready, "fragment_serializer_init ran twice — §8.5.3's member is declared once per AGENT");
    g_id_get_html = idl_method_id_step(ctx, ONE_DICT, 1, GET_HTML_OPTIONS,
                                       (int)(sizeof(GET_HTML_OPTIONS) / sizeof(GET_HTML_OPTIONS[0])),
                                       &FRAGMENT_SERIALIZER_STEP, FRAGMENT_SERIALIZE_GET_HTML);
    idl_optional_from(0);   /* §8.5.3: `getHTML(optional GetHTMLOptions options = {})` */
    /* `sequence<ShadowRoot>`'s ELEMENT TYPE. Every node wrapper is one class, so the class says "a Node" and
       the narrowing says which kind — `getHTML({shadowRoots: [document.body]})` is a TypeError, thrown by the
       type rather than by anything this file tests. */
    idl_iface_brand(node_class_id());
    idl_iface_narrow(shadow_root_is_value);
    g_ready = 1;
}

void fragment_serializer_install_get_html(JSContext *ctx, JSValueConst proto)
{
    DCHECK(g_ready, "§8.5.3's getHTML was installed before fragment_serializer_init ran");
    idl_install_method(ctx, proto, "getHTML", 0, g_id_get_html);
}

void fragment_serializer_free(void)
{
    g_id_get_html = -1;
    g_ready = 0;
}
