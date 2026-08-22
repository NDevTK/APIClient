/* HTML §3.2.6 directionality — see directionality.h. */
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/html/bidi_class.h"
#include "core/html/directionality.h"
#include "core/html/html_form.h"
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"

/* §3.2.6.4's ENUMERATED `dir` STATES. Undefined is the missing-value AND the invalid-value default, so an
   element with `dir=sideways` is in it — which is why an unrecognised keyword falls through to Undefined
   rather than being a case of its own. */
enum { DIR_UNDEFINED = 0, DIR_LTR, DIR_RTL, DIR_AUTO };

static bool tag_is(const lxb_dom_node_t *n, const char *name)
{
    size_t len = 0;
    const lxb_char_t *t;

    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    /* §3.2.6's element names are HTML's, so the NAMESPACE is part of the test — the standard says so in as
       many words ("the dir attribute is only defined for HTML elements, it cannot be present on elements from
       other namespaces"), and an SVG `<style>` skipped as if it were HTML's would hide the text under it. */
    if (n->ns != LXB_NS_HTML) return false;
    t = lxb_dom_element_local_name(lxb_dom_interface_element((lxb_dom_node_t *)n), &len);
    return t && len == strlen(name) && memcmp(t, name, len) == 0;
}

static bool ascii_ci_is(const char *a, size_t alen, const char *lower)
{
    size_t i, n = strlen(lower);

    if (!a || alen != n) return false;
    for (i = 0; i < n; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lower[i]) return false;
    }
    return true;
}

/* THE NODE'S ROOT — DOM §4.2's "root", which for a node inside a shadow tree is the ShadowRoot. §3.2.6 asks
   twice whether a `slot`'s ROOT is a shadow root, and asking whether the SLOT is one answers a different
   question that is false for every slot there has ever been. Iterative for the reason every walk here is. */
static lxb_dom_node_t *root_of(const lxb_dom_node_t *n)
{
    lxb_dom_node_t *r = (lxb_dom_node_t *)n;

    while (r->parent) r = r->parent;
    return r;
}

/* IS THIS A `slot` IN A SHADOW TREE, and if so whose — §3.2.6's two slot cases share the one test. Answers the
   shadow root's HOST, or NULL. */
static lxb_dom_element_t *slot_shadow_host(const lxb_dom_node_t *n)
{
    lxb_dom_node_t *r;

    if (!tag_is(n, "slot")) return NULL;
    r = root_of(n);
    /* ASKED BEFORE THE HOST, because shadow_root_host asserts its argument IS a shadow root — a slot in the
       ordinary document tree is the common case and is not a violation of anything. */
    return shadow_root_is(r) ? shadow_root_host(r) : NULL;
}

static int dir_state(const lxb_dom_element_t *el)
{
    size_t len = 0;
    const char *v;

    if (!el || lxb_dom_interface_node((lxb_dom_element_t *)el)->ns != LXB_NS_HTML) return DIR_UNDEFINED;
    v = (const char *)lxb_dom_element_get_attribute((lxb_dom_element_t *)el, (const lxb_char_t *)"dir", 3,
                                                    &len);
    if (!v) return DIR_UNDEFINED;
    if (ascii_ci_is(v, len, "ltr"))  return DIR_LTR;
    if (ascii_ci_is(v, len, "rtl"))  return DIR_RTL;
    if (ascii_ci_is(v, len, "auto")) return DIR_AUTO;
    return DIR_UNDEFINED;
}

/* WHICH OF THE THREE STRONG CLASSES A CODE POINT IS, by binary search over the generated runs. A code point
   in no run is BIDI_STRONG_NONE, which the scan walks past. */
static int bidi_strong_of(uint32_t cp)
{
    int lo = 0, hi = BIDI_STRONG_RUN_COUNT - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;

        if (cp < bidi_strong_runs[mid].lo)      hi = mid - 1;
        else if (cp > bidi_strong_runs[mid].hi) lo = mid + 1;
        else                                    return bidi_strong_runs[mid].strong;
    }
    return BIDI_STRONG_NONE;
}

/* ONE UTF-8 CODE POINT, advancing `*i`. Lexbor's strings are UTF-8 and its parser has already replaced
   malformed input, so a byte that cannot begin a sequence is a lexbor invariant failure rather than an input
   case: it is consumed as itself, which keeps the scan total, and the DCHECK is what says the tree held
   something the parser should never have produced. */
static uint32_t utf8_next(const unsigned char *s, size_t len, size_t *i)
{
    unsigned char c = s[*i];
    uint32_t cp;
    int n;

    if (c < 0x80)             { (*i)++; return c; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; n = 3; }
    else {
        DCHECK(false, "a DOM string held a byte that cannot begin a UTF-8 sequence — lexbor's parser replaces "
                      "malformed input, so a tree containing one was built by something that did not");
        (*i)++;
        return c;
    }
    if (*i + (size_t)n >= len) {
        /* A sequence running off the end of the buffer, for the same reason: not an input case. */
        DCHECK(false, "a DOM string ended inside a UTF-8 sequence");
        *i = len;
        return 0xFFFD;
    }
    for (int k = 1; k <= n; k++) {
        unsigned char cc = s[*i + (size_t)k];

        DCHECK((cc & 0xC0) == 0x80, "a UTF-8 sequence in a DOM string had a non-continuation byte");
        cp = (cp << 6) | (uint32_t)(cc & 0x3Fu);
    }
    *i += (size_t)n + 1;
    return cp;
}

/* THE FIRST STRONG CHARACTER of a run of text — the shape all four of §3.2.6's scans share. Answers
   BIDI_STRONG_L / _R / _AL, or _NONE when the text contains none of the three. */
static int first_strong(const char *s, size_t len)
{
    size_t i = 0;

    while (i < len) {
        int strong = bidi_strong_of(utf8_next((const unsigned char *)s, len, &i));

        if (strong != BIDI_STRONG_NONE) return strong;
    }
    return BIDI_STRONG_NONE;
}

/* §3.2.6's THREE-VALUED answer: a direction, or "this decided nothing". The spec's `null` is not 'ltr' — the
   difference is load-bearing at every call site, because a null from a child means KEEP LOOKING while an
   'ltr' means stop. */
#define DIR_NONE (-1)

static int dir_from_strong(int strong)
{
    if (strong == BIDI_STRONG_AL || strong == BIDI_STRONG_R) return DIRECTION_RTL;
    if (strong == BIDI_STRONG_L)                             return DIRECTION_LTR;
    return DIR_NONE;
}

/* §3.2.6's TEXT NODE DIRECTIONALITY. */
static int text_node_directionality(const lxb_dom_node_t *n)
{
    const lxb_dom_character_data_t *cd = (const lxb_dom_character_data_t *)n;

    if (!cd->data.data) return DIR_NONE;
    return dir_from_strong(first_strong((const char *)cd->data.data, cd->data.length));
}

/* §3.2.6's SKIP TEST — the elements whose text does not count towards an ancestor's auto directionality,
   because each of them either isolates its own direction (bdi, and any element that states its own `dir`) or
   is not rendered text at all (script, style, textarea). */
static bool excluded_subtree(const lxb_dom_node_t *n)
{
    return tag_is(n, "bdi") || tag_is(n, "script") || tag_is(n, "style") || tag_is(n, "textarea")
           || (n->type == LXB_DOM_NODE_TYPE_ELEMENT
               && dir_state(lxb_dom_interface_element((lxb_dom_node_t *)n)) != DIR_UNDEFINED);
}

/* §3.2.6's CONTAINED TEXT AUTO DIRECTIONALITY — the descendants of `el` in TREE ORDER, skipping the subtree
 * under anything excluded_subtree names.
 *
 * `defer_host` is the one answer this cannot give by itself: a descendant that is a `slot` in a shadow tree
 * contributes THE DIRECTIONALITY OF THAT SHADOW ROOT'S HOST, which is a fresh §3.2.6 computation over another
 * element. Answering it here would be a self-call whose depth is the page's shadow nesting, so the host is
 * handed back and the one loop in directionality_of continues with it — the same reason the Undefined case is
 * a loop rather than a recursion.
 *
 * THE SKIP IS A SUBTREE SKIP, not a node skip: the spec's condition names the descendant OR any ancestor of
 * it below `el`, so an excluded element takes everything under it with it. Walking node-by-node and testing
 * only the node would let a `<bdi>`'s own text decide its grandparent's direction. */
static int contained_text_auto_directionality(lxb_dom_element_t *el, bool can_exclude_root,
                                              lxb_dom_element_t **defer_host)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(el), *n;

    *defer_host = NULL;
    if (can_exclude_root && excluded_subtree(root)) return DIR_NONE;

    n = root->first_child;
    while (n) {
        bool skip = excluded_subtree(n);

        if (!skip) {
            /* §3.2.6: a slot in a shadow tree stands for its host's direction, and it does so BEFORE the
               "not a Text node, continue" step — so it is answered even though a slot holds no text. */
            {
                lxb_dom_element_t *host = slot_shadow_host(n);

                if (host) { *defer_host = host; return DIR_NONE; }
            }
            if (n->type == LXB_DOM_NODE_TYPE_TEXT) {
                int d = text_node_directionality(n);

                if (d != DIR_NONE) return d;
            }
        }
        /* TREE ORDER, with an excluded element's subtree stepped OVER rather than descended into. */
        if (!skip && n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = (n == root) ? NULL : n->parent;
        if (n == root) break;
        n = n ? n->next : NULL;
    }
    return DIR_NONE;
}

/* §3.2.6's AUTO DIRECTIONALITY. The form-associated case reads the control's VALUE rather than its text,
   which is why it is first: a `<textarea dir=auto>` takes its direction from what the user typed, and its
   child text node is the default value rather than the current one. */
static int auto_directionality(JSContext *ctx, lxb_dom_element_t *el, lxb_dom_element_t **defer_host)
{
    lxb_dom_node_t *n = lxb_dom_interface_node(el);
    JSValue wrap;

    *defer_host = NULL;
    if (html_form_is_auto_directionality_face(n)) {
        size_t vlen = 0;
        const char *v = NULL;
        JSValue val;
        int strong;

        /* THE CONTROL'S VALUE, WHICH MAY BE A CONCOLIC — `input.value = location.hash` keeps its provenance
           through the value slot, so this reads a JSValue and stringifies it rather than reading bytes off the
           attribute. The direction of an attacker-controlled value is computed from the EXAMPLE it carries,
           which is what a real browser would render. */
        wrap = node_wrap(ctx, n);
        val = html_form_control_value(ctx, wrap);
        if (!JS_IsException(val)) v = JS_ToCStringLen(ctx, &vlen, val);
        strong = v ? first_strong(v, vlen) : BIDI_STRONG_NONE;
        if (v) JS_FreeCString(ctx, v);
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, wrap);
        if (strong == BIDI_STRONG_AL || strong == BIDI_STRONG_R) return DIRECTION_RTL;
        if (vlen != 0) return DIRECTION_LTR;   /* a non-empty value with no strong RTL is 'ltr' */
        return DIR_NONE;
    }
    /* THE `slot` CASE IS ABOUT ASSIGNED NODES, not children, and it is not the same as the slot case inside
       contained text auto directionality: there a slot stands for its host, here a slot in a shadow tree takes
       the direction of the first of its ASSIGNED nodes that decides one. */
    if (slot_shadow_host(n)) {
        DFAIL("HTML §3.2.6's auto directionality reached a `slot` whose assigned nodes decide its direction — "
              "build the assigned-nodes walk here. It needs slot.c's assigned nodes (the flattened assignment, "
              "not the slot's children), each Text node scanned and each Element asked for its contained text "
              "auto directionality with canExcludeRoot true");
    }
    return contained_text_auto_directionality(el, false, defer_host);
}

int directionality_of(JSContext *ctx, lxb_dom_element_t *el)
{
    /* §3.2.6 AS ONE LOOP. Every branch of the algorithm that does not answer immediately answers "the
       directionality of some OTHER element" — the parent, a shadow host, a slot's host — so the whole of it
       is a walk with a moving cursor. Written as the spec's recursion it would be C recursion whose depth is
       the page's nesting, on a path a form submission reaches. */
    while (el) {
        lxb_dom_node_t *n = lxb_dom_interface_node(el);
        lxb_dom_element_t *host = NULL;
        int state = dir_state(el);
        int d;

        if (state == DIR_LTR) return DIRECTION_LTR;
        if (state == DIR_RTL) return DIRECTION_RTL;
        if (state == DIR_AUTO || (state == DIR_UNDEFINED && tag_is(n, "bdi"))) {
            d = auto_directionality(ctx, el, &host);
            if (host) { el = host; continue; }
            return d == DIR_NONE ? DIRECTION_LTR : d;
        }
        /* §3.2.6's one type-specific Undefined case: a telephone input is 'ltr' whatever is around it. */
        if (html_form_is_telephone_input(n)) return DIRECTION_LTR;

        /* PARENT DIRECTIONALITY: the parent element, or a shadow root's HOST, or 'ltr' at the top. */
        if (!n->parent) return DIRECTION_LTR;
        if (shadow_root_is(n->parent) && (host = shadow_root_host(n->parent)) != NULL) { el = host; continue; }
        if (n->parent->type != LXB_DOM_NODE_TYPE_ELEMENT) return DIRECTION_LTR;
        el = lxb_dom_interface_element(n->parent);
    }
    return DIRECTION_LTR;
}

int directionality_of_attribute(JSContext *ctx, lxb_dom_element_t *el, const char *value, size_t len)
{
    /* §3.2.6: an Auto element's directionality-capable attribute takes its direction from the ATTRIBUTE'S own
       first strong character, which is a different scan from the element's — the element's text may run the
       other way. Any other state is the element's own answer. */
    if (dir_state(el) == DIR_AUTO) {
        int d = dir_from_strong(first_strong(value, len));

        return d == DIR_NONE ? DIRECTION_LTR : d;
    }
    return directionality_of(ctx, el);
}

const char *directionality_name(int dir)
{
    DCHECK(dir == DIRECTION_LTR || dir == DIRECTION_RTL,
           "§3.2.6's directionality is either 'ltr' or 'rtl' and something produced a third answer — the "
           "algorithm's null means \"decided nothing\" and every caller turns it into 'ltr' at its own step");
    return dir == DIRECTION_RTL ? "rtl" : "ltr";
}
