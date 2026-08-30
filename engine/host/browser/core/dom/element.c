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
#include "core/agent_state.h"
#include "core/dom/element.h"
#include "core/dom/aria_mixin.h"
#include "core/dom/name_intern.h"   /* §4.5's storage step stores the three names AS GIVEN — see that header */
#include "core/dom/node_interface.h" /* …and the (local name, namespace) pair decides the element's C STRUCT */
#include "core/dom/element_view.h"
#include "core/dom/node_iterator.h"
#include "core/dom/tree_walker.h"
#include "core/dom/node_filter.h"
#include "core/dom/range.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/events/event_target.h"
#include "core/html/autofocus.h"
#include "core/html/html_element.h"
#include "core/html/custom_elements.h"
#include "core/html/html_iframe.h"
#include "core/html/html_parse.h"    /* the ONE place an HTML parser is made — that header owns the token bytes */
#include "core/html/trusted_types.h"
#include "core/html/declarative_shadow.h"
#include "core/html/html_script.h"
#include "core/html/html_base_element.h"
#include "core/html/html_form.h"    /* §2.1.4's moving steps step 2 — the form owner a move may have to reset */
#include "core/html/html_style_element.h"
#include "core/html/media_element.h"
#include "core/html/html_image.h"
#include "core/html/html_link.h"
#include "core/html/fragment_parser.h"
#include "core/html/fragment_serializer.h"
#include "core/html/sanitizer.h"
#include "core/dom/dom_token_list.h"
#include "core/dom/collections.h"
#include "core/dom/selector_match.h"
#include "core/dom/attr.h"
#include "core/dom/document_fragment.h"
#include "core/idl_indexed.h"
#include "core/css/css_style_declaration.h"
#include "core/css/css_rule.h"
#include "core/css/media_list.h"
#include "core/css/css_rule_list.h"
#include "core/css/css_style_sheet.h"
#include "core/css/style_sheet_list.h"
#include "core/html/integer_microsyntax.h"
#include "core/url/url.h"
#include <lexbor/ns/ns.h>

/* The two shapes every DOM member in this file has. Spelled once so a member declares its IDL, not a bitmask. */
static JSAtom g_attrs_key = JS_ATOM_NULL;   /* the [SameObject] NamedNodeMap cache slot on an element's wrapper */

static const IdlArgType IDL_1STR[1] = { IDL_DOMSTRING };
static const IdlArgType IDL_2STR[2] = { IDL_DOMSTRING, IDL_DOMSTRING };
#include "core/dom/attr_list.h"
#include "core/dom/mutation_observer.h"
#include "core/dom/mutation_record.h"
#include "core/dom/names.h"   /* §1.4's name predicates, shared with createElement and the custom-element registry */
#include "core/dom/node.h"
#include "core/dom/shadow_root.h"
#include "core/dom/slot.h"
#include "core/dom/document.h"

/* IDENTITY AND THE TREE BASE LIVE IN node.c — one wrapper table for every node kind, because a tree whose only
   node kind is Element cannot represent the document it just parsed. This file is what makes a node an ELEMENT:
   attributes, tagName, innerHTML, the reflected properties. */
lxb_dom_element_t *element_of_value(JSValueConst v)
{
    lxb_dom_node_t *n = node_of(v);
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) return NULL;
    return lxb_dom_interface_element(n);
}

bool element_is(JSValueConst v)
{
    return element_of_value(v) != NULL;
}

static JSValue js_el_get_attribute(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)magic;
    lxb_dom_element_t *el = element_of_value(this_val);
    const char *name;
    JSValue r = JS_NULL, t;

    if (!el || argc < 1) return JS_NULL;
    name = concolic_name_cstr(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    /* The TAINT SHADOW answers first: an attacker value written here came back out of Lexbor as plain bytes
       with its provenance gone, and a sink reading it would look clean. */
    t = dom_cow_attr_taint(el, name);
    if (!JS_IsUndefined(t)) {
        r = JS_DupValue(ctx, t);   /* borrowed — dup to hand it out */
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
/* THE BYTES AND THE TAINT A VALUE WRITES, which is ONE decision — resolved here because both of §4.9's key
   spaces write, and a second copy of it is a second answer to "what does a concolic attribute value store".
   `owned` is the JS_ToCString to free, NULL when the bytes are the concolic's own shape. */
typedef struct { const char *bytes; size_t len; JSValueConst taint; const char *owned; } ElAttrValue;

static bool el_attr_value(JSContext *ctx, JSValueConst value, ElAttrValue *out)
{
    /* A concolic value has no bytes to store. Record it in the shadow so the read gives the SAME concolic back,
       and write its shape into the tree so a serialization of the document still shows something. */
    if (concolic_is(value)) {
        /* NEVER ToString a concolic to get bytes: its coercion belongs to the concolic hooks and answers with
           another concolic, so the call THROWS — and the throw was being dropped here, leaving a pending
           exception behind a normal return. The SHAPE is the honest byte form for the tree. */
        const char *shape = concolic_shape_c(value);
        out->owned = NULL;
        out->bytes = shape ? shape : "";
        out->len = shape ? strlen(shape) : 0;
        out->taint = value;
        return true;
    }
    out->owned = JS_ToCString(ctx, value);
    DCHECK(out->owned != NULL, "an attribute value reached the write unconverted — the IDL declaration is what "
                               "converts it, and running the page's toString from here is the "
                               "drive-to-completion the flow machinery exists to avoid");
    if (!out->owned) return false;
    out->bytes = out->owned;
    out->len = strlen(out->owned);
    /* JS_UNDEFINED is what clears any earlier taint: a concrete write says this attribute is no longer a source. */
    out->taint = JS_UNDEFINED;
    return true;
}

static void el_attr_value_free(JSContext *ctx, ElAttrValue *v) { if (v->owned) JS_FreeCString(ctx, v->owned); }

static void el_write_attribute(JSContext *ctx, lxb_dom_element_t *el, const char *name, JSValueConst value)
{
    ElAttrValue v;

    if (!el_attr_value(ctx, value, &v)) return;
    dom_cow_set_attribute(el, name, v.bytes, v.len, v.taint);   /* chokepoint: capture-then-mutate, per flow */
    el_attr_value_free(ctx, &v);
}

/* DOM §4.9 "set an attribute value" AT §4.9'S OWN KEY — setAttributeNS step 3 and every reflection whose
   attribute is namespaced. The twin above is NOT this algorithm: setAttribute step 4 finds "the FIRST attribute
   whose QUALIFIED name is", and the two can each find an attribute the other cannot, so they are two functions
   and not one with a flag. */
static void el_write_attribute_ns(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *prefix,
                                  const char *local, JSValueConst value)
{
    ElAttrValue v;

    if (!el_attr_value(ctx, value, &v)) return;
    dom_cow_set_attribute_ns(el, ns, prefix, local, v.bytes, v.len, v.taint);
    el_attr_value_free(ctx, &v);
}

void element_ns_and_local(lxb_dom_element_t *el, const char **ns, const char **local,
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

/* DOM §4.9 `prefix` — the THIRD of the three names §4.5's storage step wrote, and the one the reader above does
 * not carry because it is nullable and the other two are not.
 *
 * IT IS THIS ENGINE'S READ AND NOT lxb_dom_element_prefix, MEASURED. That function never writes `*len` on its
 * SUCCESS path — it writes it only at its two `goto empty` arms — so every caller that initialises the length
 * to zero (which is the only thing a caller can do with an out-parameter it does not own) reads a prefix of
 * length zero and concludes there is none. `element.prefix` answered null for `createElementNS(SVG_NS,
 * "svg:rect")`, and §4.4's `lookupNamespaceURI(null)` — whose match IS "this element has no prefix" — answered
 * the namespace of every prefixed element as though it were the default one. It also asks the wrong hash
 * (`owner_document->tags`), which happens not to matter only because lxb_ns_prefix_data_by_id ignores its hash
 * argument entirely and resolves a non-static id as its own address; the right hash is asked here so that stays
 * true by statement rather than by luck.
 *
 * BORROWED, like every interned name: the bytes live in the document's prefix hash for as long as the document
 * does, so there is nothing to copy and nothing to free. `*len` is written on EVERY path. */
const char *element_prefix(lxb_dom_element_t *el, size_t *len)
{
    const lxb_ns_prefix_data_t *d;

    DCHECK(el != NULL && len != NULL, "an element's namespace prefix was read off no element");
    *len = 0;
    if (el->node.prefix == LXB_NS__UNDEF) return NULL;   /* §1.4's null prefix, which is not the empty string */
    d = lxb_ns_prefix_data_by_id(lxb_dom_interface_node(el)->owner_document->prefix, el->node.prefix);
    CHECK(d != NULL, "an element carries a namespace prefix id its document cannot resolve — element_intern_"
                     "prefix is the only writer of that field and it interns the bytes before it stores the id, "
                     "so an unresolvable one is a node built by something that is not this engine");
    *len = d->entry.length;
    return (const char *)lexbor_hash_entry_str(&d->entry);
}

/* ---- DOM §4.5 "create an element internal", THE STORAGE STEP ---------------------------------------------
 *
 * "Set element's namespace to namespace, namespace prefix to prefix, local name to localName" — three strings
 * the algorithm was GIVEN, stored byte for byte. This engine stores each as an id into one of the document's
 * hashes, and those ids are what the two readers above turn back into bytes, so "stored as given" is a
 * property of THE INTERNING and of nothing else. core/dom/name_intern.h owns that interning and states which
 * standard says each of the three is a different name; the three sentences that stood here were moved there
 * whole when core/dom/attr_list.c turned out to need every one of them about the same hashes.
 *
 * WHAT REMAINS HERE IS §4.5's ORDER, and it is the reason this is a function rather than three calls at the
 * member: the ids are computed BEFORE the interface exists. lxb_dom_element_create interns and creates in one
 * breath, and the folded id had already CHOSEN THE INTERFACE — a lower-cased static probe answers LXB_NS_HTML
 * for "HTTP://WWW.W3.ORG/1999/XHTML", so lxb_html_interface_create built an HTML element struct for a namespace
 * that is not the HTML namespace. An element whose struct disagrees with its namespace is a state this engine
 * must not be able to reach, so the namespace is decided first and the interface is created from it. Repairing
 * node->ns afterwards would leave the wrong struct in place, and a side table of unfolded names beside the tree
 * would answer `namespaceURI` and leave `isEqualNode`, which compares the IDS, calling two namespaces one.
 *
 * lexbor's, exported and declared in no lexbor header — its own dom/interfaces/element.c declares its
 * neighbours at the call site in exactly this form, which is why the declaration is copied rather than
 * invented. */
lxb_status_t
lxb_dom_element_qualified_name_set(lxb_dom_element_t *element, const lxb_char_t *prefix, size_t prefix_len,
                                   const lxb_char_t *lname, size_t lname_len);

/* `ns` and `prefix` are NULL when there is none, which is §1.4's null and not the empty string. The three
   slices are BORROWED and none of them is NUL-terminated — they are the halves of the caller's qualifiedName
   that validate-and-extract handed back. */
lxb_dom_element_t *element_create_ns(lxb_dom_document_t *doc, const char *ns, size_t ns_len,
                                     const char *local, size_t local_len,
                                     const char *prefix, size_t prefix_len)
{
    lxb_tag_id_t tag_id;
    lxb_ns_id_t ns_id;
    lxb_dom_element_t *el;

    DCHECK(!(prefix != NULL && prefix_len != 0) || (ns != NULL && ns_len != 0),
           "an element was created with a namespace prefix and no namespace — DOM §1.4 step 8 throws a "
           "NamespaceError for that pair, so validate-and-extract cannot have produced it");
    tag_id = dom_intern_element_local_name(doc, local, local_len);
    ns_id = dom_intern_namespace(doc, ns, ns_len);
    el = lxb_dom_interface_element(dom_element_interface_create(doc, tag_id, ns_id));
    CHECK(el != NULL, "an element interface could not be created");
    if (prefix != NULL && prefix_len != 0) {
        /* BEFORE local_name is set, which is lexbor's own order and is load-bearing: the qualified name is
           interned with the element's CURRENT local-name id as its tag id, and that id is still LXB_TAG__UNDEF
           here, so the qualified name gets an id of its own rather than aliasing the local name's. */
        lxb_status_t st = lxb_dom_element_qualified_name_set(el, (const lxb_char_t *)prefix, prefix_len,
                                                             (const lxb_char_t *)local, local_len);
        el->node.prefix = dom_intern_prefix(doc, prefix, prefix_len);
        CHECK(st == LXB_STATUS_OK, "an element's qualified name could not be stored");
    }
    el->node.local_name = tag_id;
    el->node.ns = ns_id;
    el->custom_state = LXB_DOM_ELEMENT_CUSTOM_STATE_UNCUSTOMIZED;
    return el;
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

static int js_el_set_attribute(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                               JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SetAttrState *s = st;
    lxb_dom_element_t *el = element_of_value(hdr->this_val);

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
        if (!dom_valid_attribute_local_name(name, strlen(name))) {          /* step 1 */
            JS_FreeCString(ctx, name);
            JS_ThrowDOMException(ctx, "InvalidCharacterError",
                                 "setAttribute was given a name that is not a valid attribute local name");
            return JS_STEP_ABRUPT;
        }
        s->name_len = strlen(name);
        s->name = js_malloc(ctx, s->name_len + 1);
        CHECK(s->name != NULL, "setAttribute could not copy its own attribute name");
        memcpy(s->name, name, s->name_len + 1);
        JS_FreeCString(ctx, name);
        /* STEP 2 — "if this is in the HTML namespace and its node document is an HTML document, ASCII
           lowercase qualifiedName" — AND THIS LOOP IS THE ONLY THING THAT PERFORMS IT. The write below goes
           through attr_list.c's dom_attr_write, which hands lexbor the name AS GIVEN; nothing calls
           lxb_dom_element_set_attribute any more, so there is no longer a second, hidden lowercasing under
           this one. Delete this loop and `el.setAttribute("SRC", location.hash)` stores an attribute named
           `SRC` that `el.getAttribute("src")` cannot find, and the taint shadow keyed on it goes with it. */
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
        element_ns_and_local(el, &ns, &local, nsbuf, sizeof(nsbuf), lobuf, sizeof(lobuf));
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
    /* No release: `verified` and the name buffer are BOTH set_attr_visit's, and the teardown discharges that
       one declaration. */
    js_el_set_attribute, sizeof(SetAttrState), set_attr_visit, NULL,
    "DOM §4.9 Element.setAttribute", EL_SET_ATTR_STEPS
};

/* §4.9 setAttributeNS, AS ITS OWN MACHINE — three steps, with Trusted Types §3.7 at step 2 of them.
   NOT A MAGIC ON setAttribute'S TABLE. The two algorithms differ in every step: this one VALIDATES AND EXTRACTS
   (§1.4's twelve steps, throwing NamespaceError for a pairing setAttribute cannot even express) where that one
   validates a local name and lowercases; this one runs Trusted Types with the LOCAL name and the REAL attribute
   namespace where that one passes the whole qualified name and null; and this one writes at §4.9's own
   (namespace, local name) key where that one writes at the first attribute wearing a qualified name. A shared
   stage table would name the wrong step for whichever of them a parked flow was in. */
#define EL_SET_ATTR_NS_STAGES(X) \
    X(SETATTRNS_VALIDATE, "DOM §4.9 setAttributeNS step 1 (validate and extract namespace and qualifiedName " \
                          "with context \"attribute\" — DOM §1.4's twelve steps, throwing InvalidCharacterError " \
                          "or NamespaceError)") \
    X(SETATTRNS_TRUSTED,  "DOM §4.9 setAttributeNS step 2 (get trusted type compliant attribute value with the " \
                          "LOCAL name, the extracted namespace, this and value), which is where Trusted Types " \
                          "§3.5's default-policy callback runs") \
    X(SETATTRNS_WRITE,    "DOM §4.9 setAttributeNS step 3 (set an attribute value for this using localName, " \
                          "the verified value, prefix and namespace)")
enum { IDL_STEP_STAGE_BASE(EL_SET_ATTR_NS_STAGES) EL_SET_ATTR_NS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const EL_SET_ATTR_NS_STEPS[] = { EL_SET_ATTR_NS_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* THE EXTRACTED NAME IS ONE BUFFER WITH THE COLON REPLACED BY A NUL, which is what "prefix and local name are
   the two halves of qualifiedName" means once they have to be C strings: `qname` is the prefix and
   `qname + prefix_len + 1` is the local name, both NUL-terminated, and `prefix_len == 0` is §1.4's null prefix
   (step 4.3 rejects a zero-length one, so there is no third case). Two separate copies would be two answers to
   where the colon was. */
typedef struct {
    char   *qname;       /* step 1's qualified name, owned, colon overwritten with NUL */
    size_t  qname_len;
    size_t  prefix_len;  /* 0 = no prefix; otherwise the local name starts at prefix_len + 1 */
    char   *ns;          /* step 1's namespace, owned; NULL is the null namespace */
    size_t  ns_len;
    JSValue verified;    /* step 2's answer, owned — what step 3 writes, never the argument */
} SetAttrNsState;

static const char *set_attr_ns_local(const SetAttrNsState *s)
{
    return s->qname + (s->prefix_len ? s->prefix_len + 1 : 0);
}

static void set_attr_ns_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    SetAttrNsState *s = st;
    v->val(ctx, &s->verified);
    v->buf(ctx, (void **)&s->qname, s->qname_len ? s->qname_len + 1 : 0);
    v->buf(ctx, (void **)&s->ns, s->ns_len ? s->ns_len + 1 : 0);
}

static int js_el_set_attribute_ns(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                  JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    SetAttrNsState *s = st;
    lxb_dom_element_t *el = element_of_value(hdr->this_val);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;

    if (hdr->stage == SETATTRNS_VALIDATE) {
        const char *ns_arg, *qn;
        size_t ns_arg_len = 0, qn_len = 0;
        DomQName ex;
        bool ok;

        if (argc < 3) return JS_STEP_DONE;
        /* `DOMString? namespace`: the declaration made null of null AND undefined, and §1.4 step 1 makes null
           of the empty string too — so both reach validate-and-extract as the null namespace. */
        ns_arg = JS_IsNull(argv[0]) ? NULL : JS_ToCStringLen(ctx, &ns_arg_len, argv[0]);
        qn = concolic_name_cstr(ctx, argv[1]);
        if (!qn) { if (ns_arg) JS_FreeCString(ctx, ns_arg); return JS_STEP_ABRUPT; }
        qn_len = strlen(qn);
        ok = dom_validate_and_extract(ctx, ns_arg, ns_arg_len, qn, qn_len, DOM_NAME_ATTRIBUTE, &ex);   /* step 1 */
        if (ok) {
            s->qname_len = qn_len;
            s->qname = js_malloc(ctx, qn_len + 1);
            CHECK(s->qname != NULL, "setAttributeNS could not copy its own qualified name");
            memcpy(s->qname, qn, qn_len + 1);
            s->prefix_len = ex.prefix ? ex.prefix_len : 0;
            if (s->prefix_len) s->qname[s->prefix_len] = 0;   /* the colon IS the separator, now a terminator */
            if (ex.ns) {
                s->ns_len = ex.ns_len;
                s->ns = js_malloc(ctx, ex.ns_len + 1);
                CHECK(s->ns != NULL, "setAttributeNS could not copy its own namespace");
                memcpy(s->ns, ex.ns, ex.ns_len); s->ns[ex.ns_len] = 0;
            }
        }
        JS_FreeCString(ctx, qn);
        if (ns_arg) JS_FreeCString(ctx, ns_arg);
        if (!ok) return JS_STEP_ABRUPT;   /* §1.4 already threw the exception its step names */
        hdr->stage = SETATTRNS_TRUSTED;
    }
    if (hdr->stage == SETATTRNS_TRUSTED) {
        char nsbuf[128], lobuf[64];
        const char *el_ns, *el_local;

        if (!el) return JS_STEP_DONE;
        element_ns_and_local(el, &el_ns, &el_local, nsbuf, sizeof(nsbuf), lobuf, sizeof(lobuf));
        /* STEP 2, with the attribute's REAL namespace and its LOCAL name — which is what makes the XLink
           `href` of an SVGScriptElement a TrustedScriptURL sink and `setAttributeNS(null, "onclick", s)` an
           event-handler one, neither of which the by-name call can express. */
        s->verified = trusted_types_compliant_attribute_value(ctx, el_ns, el_local, s->ns,
                                                              set_attr_ns_local(s), argv[2]);
        if (JS_IsException(s->verified)) { s->verified = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        hdr->stage = SETATTRNS_WRITE;
    }
    DCHECK(hdr->stage == SETATTRNS_WRITE, "setAttributeNS resumed into a stage it does not have");
    if (el)   /* step 3 */
        el_write_attribute_ns(ctx, el, s->ns, s->prefix_len ? s->qname : NULL, set_attr_ns_local(s), s->verified);
    return JS_STEP_DONE;
}

static const IdlStepDecl EL_SET_ATTR_NS_STEP = {
    /* No release: `verified` and both name buffers are set_attr_ns_visit's, discharged with the rest. */
    js_el_set_attribute_ns, sizeof(SetAttrNsState), set_attr_ns_visit, NULL,
    "DOM §4.9 Element.setAttributeNS", EL_SET_ATTR_NS_STEPS
};

/* THE ENGINE'S OWN ATTRIBUTE WRITE — a reflection's setter, the hyperlink mixin re-serialising `href`. It is
   DOM §4.9's "set an attribute value" and nothing above it: the name is the engine's own (so step 1 cannot
   fail and step 2 has nothing to lowercase), and the Trusted Types step belongs to the MEMBER the page called,
   which for a reflected IDL attribute is that attribute's own setter. */
static void el_set_attribute_internal(JSContext *ctx, JSValueConst this_val, const char *name, JSValueConst value)
{
    lxb_dom_element_t *el = element_of_value(this_val);

    if (!el) return;
    DCHECK(dom_valid_attribute_local_name(name, strlen(name)),
           "the engine wrote an attribute whose name the DOM would reject");
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
    lxb_dom_element_t *el = element_of_value(this_val);
    size_t n = 0;
    const lxb_char_t *t;
    if (!el) return JS_UNDEFINED;
    t = lxb_dom_element_tag_name(el, &n);
    return t ? JS_NewStringLen(ctx, (const char *)t, n) : JS_UNDEFINED;
}

/* §13.3's FRAGMENT SERIALISER — innerHTML's and outerHTML's getters — IS core/html/fragment_serializer.c, and
   is not here. It was: this file held the walk, and the moment ShadowRoot needed the SAME algorithm (its own
   `innerHTML` getter, and §8.5.3's `getHTML` on both interfaces) a second copy of it was what a second
   installation would have grown. One algorithm, one component, four members — the magics are the component's
   FRAGMENT_SERIALIZE_* and the declarations below are what name them. */

/* An HTML-context sink is TWO things and it must do both.
   It is a SINK, so the assigned value goes to the solver, which decides the breakout against the real parse
   context. And it MUTATES THE TREE — a page that builds its DOM this way and then queries it must find what it
   built, or every getElementById after it answers null and the engine reports a surface the page never had.
   Reporting the sink and dropping the markup was the second half missing.
   Both halves go through the per-flow chokepoints, so two forked arms each see their own subtree. A concolic
   value has no bytes to parse — the sink report IS the answer for it.
   The magic is element.h's ELEMENT_SET_* / SHADOW_ROOT_SET_*: five members over one parse, differing in the
   TARGET the fragment replaces, the CONTEXT it is parsed in, and whether §13.4's allowDeclarativeShadowRoots
   is true. */
/* THE SINK NAME each member passes to §3.4, which is what a Trusted Types violation report names — so two
   sinks are distinguishable in one report. Indexed by the magic, beside the magic's own declaration. */
/* The two SAFE members are here for the same reason every other magic is — the array is indexed by it — and
   they name no sink because §8.5.2's `setHTML` invokes no trusted-type algorithm at all: its argument is a
   plain DOMString, and what makes it safe is the sanitizer rather than a policy. */
static const char *const FRAG_SINK[] = {
    "Element innerHTML", "Element outerHTML", "Element setHTMLUnsafe",
    "ShadowRoot innerHTML", "ShadowRoot setHTMLUnsafe",
    NULL, NULL,
};

static int js_el_set_html(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                          JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FragmentParse *s = st;
    int magic = idl_step_magic(hdr);
    /* §8.6.4 set and filter HTML's `safe`, and which members are `set and filter HTML` AT ALL — §8.5.4's innerHTML setter and
       §8.5.5's outerHTML setter are not, so they parse and place without ever consulting a configuration. */
    bool safe = magic == ELEMENT_SET_HTML || magic == SHADOW_ROOT_SET_HTML;
    bool unsafe = magic == ELEMENT_SET_HTML_UNSAFE || magic == SHADOW_ROOT_SET_HTML_UNSAFE;
    bool filtered = safe || unsafe;
    bool on_shadow = magic == SHADOW_ROOT_SET_INNER_HTML || magic == SHADOW_ROOT_SET_HTML_UNSAFE ||
                     magic == SHADOW_ROOT_SET_HTML;

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    *presult = JS_UNDEFINED;
    DCHECK(magic >= 0 && magic < (int)(sizeof(FRAG_SINK) / sizeof(FRAG_SINK[0])),
           "the fragment-parse machine was declared with a magic no member of element.h's list names");

    /* §8.6.4 set and filter HTML STEP 8's WALK owns every stage past this machine's own, and it resumes into itself for as many
       nodes as the fragment has. When it is done the member continues at step 9, which is the same placement
       the unfiltered members reach straight from the parse. */
    if (hdr->stage >= SAN_CHILD && hdr->stage <= SAN_POP) {
        int r = sanitizer_walk_step(ctx, hdr, &s->san);

        if (r) return r;
        fragment_parse_placement(s, hdr);
        return JS_STEP_YIELD;
    }

    if (hdr->stage == FRAG_TRUSTED && safe) {
        /* §8.5.2's `setHTML(DOMString html, …)` — there is no union and therefore no step 1: the argument is
           already the string the declaration converted, and the SANITIZER is what makes the member safe. The
           stage is still entered and left, because a stage is where the machine is and not what it did. */
        s->compliant = JS_DupValue(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
        hdr->stage = FRAG_START;
    }
    if (hdr->stage == FRAG_TRUSTED) {
        /* §8.5.4 / §8.5.5 / §8.5.2 step 1. It runs BEFORE the element and its parent are looked at, which is
           the order the standard writes and the order that decides what a page under a trusted-types policy
           sees: the throw comes from step 1, not from step 4's parse, so
           `document.createElement("b").outerHTML = s` throws the TypeError rather than returning at step 3's
           null parent. */
        s->compliant = trusted_types_compliant_string(ctx, TRUSTED_TYPE_HTML, argc > 0 ? argv[0] : JS_UNDEFINED,
                                                      FRAG_SINK[magic]);
        if (JS_IsException(s->compliant)) { s->compliant = JS_UNDEFINED; return JS_STEP_ABRUPT; }
        hdr->stage = FRAG_START;
    }
    if (hdr->stage == FRAG_START) {
        lxb_dom_node_t *n = node_of(hdr->this_val);
        lxb_dom_element_t *el;
        JSValueConst val = s->compliant;
        const char *html;
        /* §8.6.4 set and filter HTML STEP 5, "Let scriptingMode be Inert" — which is also the answer for the
           two members that are not `set and filter HTML` at all: §8.5.4's innerHTML setter and §8.5.5's
           outerHTML setter invoke the fragment parsing algorithm steps with no scriptingMode, and HTML §8.5.4
           gives that argument the default Inert. Only step 6 below moves it. */
        FragScriptingMode scripting = FRAG_SCRIPTING_INERT;

        /* WEB IDL §3.7.5's BRAND CHECK. `Element.prototype`'s members reach any receiver a page hands them
           with `.call`, and ShadowRoot's two are declared on an interface that is NOT an element — a
           DocumentFragment — so which of the two this is decides what the receiver must be. */
        if (on_shadow) {
            if (!shadow_root_is(n)) {
                JS_ThrowTypeError(ctx, "a ShadowRoot markup member was written on something that is not a "
                                       "shadow root");
                return JS_STEP_ABRUPT;
            }
        } else if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            JS_ThrowTypeError(ctx, "an Element markup member was written on something that is not an element");
            return JS_STEP_ABRUPT;
        }
        /* §8.6.4 set and filter HTML's STEPS 3, 4, 5 AND 6, read before anything is parsed because the standard reads them before its
           step 7's parse — and the options are a dictionary the declaration already converted, so nothing of
           the page's runs here.
           THE NUMBERS IN THIS BLOCK MOVED and this comment used to carry the old ones (3, 4 and 5, with the
           parse at 6). §8.6.4's list today is: 3 the safe-`script` return, 4 the sanitizer, 5 "Let
           scriptingMode be Inert", 6 the `runScripts` conditional, 7 the parse, 8 the sanitize, 9 the replace
           — verified against the section's own `<ol>` rather than recalled, because a cited number that has
           renumbered reads as authoritative and sends the next reader to a step that says something else. */
        if (filtered) {
            JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;

            /* STEP 3: a SAFE member whose CONTEXT is a `script` returns having done nothing at all — not even
               the parse. The context is step 1's, which for a ShadowRoot receiver is its host and not the
               shadow root itself, and the namespace half of the condition is what makes SVG's `script` one
               too. */
            lxb_dom_node_t *context = on_shadow ? lxb_dom_interface_node(shadow_root_host(n)) : n;

            if (safe && (context->ns == LXB_NS_HTML || context->ns == LXB_NS_SVG) &&
                lxb_html_tree_node_is(context, LXB_TAG_SCRIPT))
                return JS_STEP_DONE;
            /* STEP 4. The configuration is resolved HERE and not at step 8, because §8.6.4 set and filter HTML resolves it before
               the parse and a page can tell: a `sanitizer` that is not one is a TypeError thrown before the
               markup is parsed at all. */
            s->san_config = sanitizer_config_from_options(ctx, options, safe);
            if (JS_IsException(s->san_config)) { s->san_config = JS_UNDEFINED; return JS_STEP_ABRUPT; }
            s->sanitize = 1;
            s->safe = safe ? 1 : 0;
            /* STEPS 5 AND 6 — "Let scriptingMode be Inert" and "If options["runScripts"] is true: 1. Assert:
               safe is false. 2. Set scriptingMode to Fragment." `scripting` above is step 5; this is step 6.
               STEP 6.1's ASSERT IS STRUCTURAL HERE and not a check: `runScripts` is a member of
               SetHTMLUnsafeOptions and of no other dictionary, so a SAFE member's declaration does not list it
               and its body can never see one — which is why element_declare_set_html is its own declaration
               rather than a magic on setHTMLUnsafe's. Asserted anyway, because the thing that guarantees it is
               two declarations away from the thing that relies on it. */
            if (idl_dict_bool(ctx, options, "runScripts")) {
                DCHECK(!safe, "§8.6.4 set and filter HTML step 6.1 asserts `safe` is false when "
                              "options[\"runScripts\"] is true, and a SAFE member reached it — SetHTMLOptions "
                              "has no `runScripts` member, so a true one here means a declaration listed a "
                              "dictionary member its own IDL line does not have");
                scripting = FRAG_SCRIPTING_FRAGMENT;
            }
        }
        el = n->type == LXB_DOM_NODE_TYPE_ELEMENT ? lxb_dom_interface_element(n) : NULL;
        if (magic == ELEMENT_SET_OUTER_HTML) {
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
            /* §8.5.5 STEP 5: a DocumentFragment parent is not a parse context — the fragment parsing
               algorithm reads the context element's local name to pick the tokenizer state and to reset the
               insertion mode, and a fragment has neither. The standard says to parse against a freshly
               created HTML `body` element instead, which is what makes
               `frag.appendChild(td); td.outerHTML = "<tr>"` behave as `body` would rather than as whatever
               the fragment's first child happened to be. The element is created here and destroyed with the
               machine: it is in no tree, so nothing else ever would. */
            if (node_is_document_fragment(n->parent)) {
                s->own_context = lxb_dom_document_create_element(n->owner_document,
                                                                 (const lxb_char_t *)"body", 4, NULL);
                CHECK(s->own_context != NULL,
                      "§8.5.5 step 5's `body` element could not be created — outerHTML would then parse "
                      "against a context the algorithm does not have");
            } else {
                DCHECK(n->parent->type == LXB_DOM_NODE_TYPE_ELEMENT,
                       "§8.5.5 step 2 gave outerHTML a parent that is neither a Document (step 4's throw), a "
                       "DocumentFragment (step 5's body) nor an Element — DOM §4.2.3 admits no fourth kind of "
                       "parent, so this is a tree this engine built and the standard cannot describe");
            }
        }
        solve_html_sink(ctx, val);
        if (concolic_is(val))
            return JS_STEP_DONE;   /* nothing concrete to parse; the sink is what this write means */

        DCHECK(JS_IsString(val), "an HTML sink reached the body unconverted — the IDL declaration is what "
                                 "converts it, and running the page's toString from here is the "
                                 "drive-to-completion the flow machinery exists to avoid");
        html = JS_ToCString(ctx, val);
        if (!html) return JS_STEP_ABRUPT;
        if (magic == ELEMENT_SET_OUTER_HTML) {
            /* §8.5.5 step 6: parsed in the PARENT's context, because that is where it lives — or in step 5's
               `body` when the parent is a fragment. Step 7 still replaces `this` within THIS'S parent, which
               is the real one either way: step 5 reassigns the local, not the tree. */
            fragment_parse_begin(ctx, s, s->own_context ? s->own_context : lxb_dom_interface_element(n->parent),
                                 n, FRAG_INTO_REPLACE, html, false, /*allow_declarative*/ false, scripting);
        } else if (magic == SHADOW_ROOT_SET_INNER_HTML || magic == SHADOW_ROOT_SET_HTML_UNSAFE) {
            /* §13.4 step 2: "Let context be target if target is an Element; otherwise target's HOST." A shadow
               root has no local name and no tokenizer state of its own, so the markup is parsed exactly as the
               host's children would be, and it REPLACES the shadow root's children. */
            fragment_parse_begin(ctx, s, shadow_root_host(n), n, FRAG_INTO_CHILDREN, html,
                                 /*clear_first*/ true, filtered, scripting);
        } else {
            /* §8.5.4 step 2.2 / §8.5.2 setHTMLUnsafe step 2: a <template>'s children are NOT what these
               replace — its TEMPLATE CONTENTS are, which is a separate tree reached through the element. The
               parse context stays the element, because the tree builder's "in template" insertion mode is what
               decides whether a <tr> survives; only the target of the replacement moves. Without this a page
               that filled a template this way got an element with children nothing renders and a `content`
               fragment that stayed empty. */
            lxb_dom_node_t *target = n;
            if (lxb_html_tree_node_is(n, LXB_TAG_TEMPLATE)) {
                lxb_html_template_element_t *t = lxb_html_interface_template(n);
                DCHECK(t->content != NULL, "a <template> element in the tree has no content fragment — lexbor's "
                                           "template interface is what owns it, and §4.12.3 gives every "
                                           "template one");
                target = &t->content->node;
            }
            /* §8.6.4 set and filter HTML step 7 invokes the fragment parsing algorithm with allowDeclarativeShadowRoots TRUE for
               ALL FOUR of its members — a `<template shadowrootmode>` a SAFE member parses becomes a real
               shadow root and is then sanitized like any other tree, which is what step 1.5.6 walks into.
               §8.5.4's innerHTML setter is not one of them and passes false. */
            fragment_parse_begin(ctx, s, el, target, FRAG_INTO_CHILDREN, html, /*clear_first*/ true, filtered,
                                 scripting);
        }
        JS_FreeCString(ctx, html);
        hdr->stage = FRAG_FEED;
        return JS_STEP_YIELD;
    }
    return fragment_parse_step(ctx, hdr, s);
}

static const char *const EL_SET_HTML_STEPS[] = {
    FRAG_STAGES(JS_STEP_STAGE_LABEL) FRAG_STAGE_CLEAR(JS_STEP_STAGE_LABEL)
    SANITIZE_STAGES(JS_STEP_STAGE_LABEL) NULL
};

static const IdlStepDecl EL_SET_HTML_STEP = { js_el_set_html, sizeof(FragmentParse),
                                              fragment_parse_visit, fragment_parse_release,
                                              "HTML §8.5.4/§8.5.5 innerHTML/outerHTML setter, §8.5.2 "
                                              "setHTMLUnsafe (over §8.6.4 set and filter HTML's set and filter HTML)",
                                              EL_SET_HTML_STEPS, .unforkable = fragment_parse_unforkable };

const IdlStepDecl *element_set_html_decl(void)
{
    return &EL_SET_HTML_STEP;
}

/* §8.5.2's `[CEReactions] undefined setHTMLUnsafe((TrustedHTML or DOMString) html, optional
   SetHTMLUnsafeOptions options = {})` — DECLARED HERE FOR BOTH INTERFACES that have it. Element's and
   ShadowRoot's IDL for this member is the same line, so the argument list and the dictionary are stated once:
   two copies are two chances for one of them to lose a member, and a member the declaration does not list is
   one the body silently never sees.
   The union's TrustedHTML arm is §2's type, which does not exist here, so every value takes the DOMString arm
   — and there is NO [LegacyNullToEmptyString] on it, unlike innerHTML's, so `setHTMLUnsafe(null)` parses the
   four characters `null`. */
int element_declare_set_html_unsafe(JSContext *ctx, int magic)
{
    static const IdlArgType SET_HTML_UNSAFE_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
    /* Web IDL §3.2.17 reads a dictionary's members LEXICOGRAPHICALLY, which for these two is also declaration
       order. `sanitizer` is `(Sanitizer or SanitizerConfig or SanitizerPresets)` and is IDL_ANY because none of
       the three types exists yet — the body refuses a stated one by name rather than converting it to
       something it cannot honour. */
    static const IdlDictMember SET_HTML_UNSAFE_OPTIONS[] = {
        { "runScripts", IDL_BOOLEAN, false, NULL, 0 },
        { "sanitizer",  IDL_ANY,     false, NULL, 0 },
    };
    int id;

    DCHECK(magic == ELEMENT_SET_HTML_UNSAFE || magic == SHADOW_ROOT_SET_HTML_UNSAFE,
           "setHTMLUnsafe was declared under a magic that is not one of the two interfaces that have it");
    id = idl_method_id_step(ctx, SET_HTML_UNSAFE_ARGS, 2, SET_HTML_UNSAFE_OPTIONS,
                            (int)(sizeof(SET_HTML_UNSAFE_OPTIONS) / sizeof(SET_HTML_UNSAFE_OPTIONS[0])),
                            &EL_SET_HTML_STEP, magic);
    idl_optional_from(1);
    return id;
}

/* §8.5.2's `[CEReactions] undefined setHTML(DOMString html, optional SetHTMLOptions options = {})` — the SAFE
   member, on both interfaces that have it, and the same machine as the three above with §8.6.4 set and filter HTML's `safe` true.
   ITS OWN DECLARATION and not a magic on setHTMLUnsafe's, because the IDL is a different line: there is no
   `(TrustedHTML or DOMString)` union (nothing is trusted — the SANITIZER is what makes it safe, which is also
   why the member exists) and SetHTMLOptions declares ONE member where SetHTMLUnsafeOptions declares two. A
   `runScripts` this declaration does not list is one the body can never see, which is what makes §8.6.4 set and filter HTML step
   6.1's assert structural here rather than a check. */
int element_declare_set_html(JSContext *ctx, int magic)
{
    static const IdlArgType SET_HTML_ARGS[2] = { IDL_DOMSTRING, IDL_DICT };
    static const IdlDictMember SET_HTML_OPTIONS[] = {
        { "sanitizer", IDL_ANY, false, NULL, 0 },
    };
    int id;

    DCHECK(magic == ELEMENT_SET_HTML || magic == SHADOW_ROOT_SET_HTML,
           "setHTML was declared under a magic that is not one of the two interfaces that have it");
    id = idl_method_id_step(ctx, SET_HTML_ARGS, 2, SET_HTML_OPTIONS,
                            (int)(sizeof(SET_HTML_OPTIONS) / sizeof(SET_HTML_OPTIONS[0])),
                            &EL_SET_HTML_STEP, magic);
    idl_optional_from(1);
    return id;
}

/* §4.9 insertAdjacentHTML / insertAdjacentElement / insertAdjacentText — the SAME four positions, which is why
   one body reads the position and three members differ only in what they place. insertAdjacentHTML is an
   HTML-context sink exactly like innerHTML, and it was absent: a bundle using it had its DOM unbuilt AND its
   XSS invisible, which is the pair this engine exists to report.
   magic 0 = HTML, 1 = Element, 2 = Text. */
/* §4.9's ADJACENT POSITION, shared by the three members that take one. The four names, ASCII
   case-insensitively; anything else is a SyntaxError, not a quiet no-op. Returns false having thrown.
   THE POSITION IS ALL THIS ANSWERS. It used to also reject an outside position whose parent is not an ELEMENT,
   with a NoModificationAllowedError — which is HTML §8.5.6 "The insertAdjacentHTML() method"'s rule applied to
   all three members, and DOM §4.9's "insert adjacent" has no such step: for "beforebegin" and "afterend" it
   RETURNS NULL when the parent is null, and otherwise PRE-INSERTS, which is what makes
   `document.documentElement.insertAdjacentText("beforebegin", "x")` a §4.2.3 HierarchyRequestError rather than
   a NoModificationAllowedError. Two of the three members were answering the third's question. */
static bool adjacent_where(JSContext *ctx, JSValueConst posv, int *pwhere, bool *poutside)
{
    const char *pos = JS_ToCString(ctx, posv);   /* a real string by now: the declaration converted it */

    if (!pos) return false;
    if (!strcasecmp(pos, "beforebegin"))     { *pwhere = FRAG_INTO_BEFORE;      *poutside = true;  }
    else if (!strcasecmp(pos, "afterbegin")) { *pwhere = FRAG_INTO_FIRST_CHILD; *poutside = false; }
    else if (!strcasecmp(pos, "beforeend"))  { *pwhere = FRAG_INTO_CHILDREN;    *poutside = false; }
    else if (!strcasecmp(pos, "afterend"))   { *pwhere = FRAG_INTO_AFTER;       *poutside = true;  }
    else {
        JS_FreeCString(ctx, pos);
        JS_ThrowDOMException(ctx, "SyntaxError", "not one of the four adjacent positions");
        return false;
    }
    JS_FreeCString(ctx, pos);
    return true;
}

/* §4.9 insertAdjacentHTML — its own declaration, because it is its own algorithm: it PARSES, and the other two
   adjacent members do not. One member whose body forks on a magic between "parse markup" and "insert a node
   the caller already has" would be two algorithms wearing one declaration. */
static int js_el_insert_adjacent_html(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                      JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    FragmentParse *s = st;

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
        lxb_dom_element_t *el = element_of_value(hdr->this_val);
        lxb_dom_node_t *n;
        const char *html;
        int where;
        bool outside;

        if (!el || argc < 2) return JS_STEP_DONE;
        n = lxb_dom_interface_node(el);
        if (!adjacent_where(ctx, argv[0], &where, &outside)) return JS_STEP_ABRUPT;
        /* HTML §8.5.6 "The insertAdjacentHTML() method" step 3: for the two outside positions the context is
           this's parent, and "if context is null or a Document, throw a NoModificationAllowedError". NOT "is
           not an Element", which is what this used to ask through adjacent_where — an element inside a
           DocumentFragment has a parent that is neither, and its insertAdjacentHTML must work. */
        if (outside && (!n->parent || n->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT)) {
            JS_ThrowDOMException(ctx, "NoModificationAllowedError",
                                 "insertAdjacentHTML at an outside position whose context is null or a Document");
            return JS_STEP_ABRUPT;
        }
        solve_html_sink(ctx, s->compliant);
        if (concolic_is(s->compliant)) return JS_STEP_DONE;
        DCHECK(JS_IsString(s->compliant), "insertAdjacentHTML reached the body unconverted");
        html = JS_ToCString(ctx, s->compliant);
        if (!html) return JS_STEP_ABRUPT;
        /* Parsed in the context it will LIVE in: the parent for the outside positions, this element for the
           inside ones. A `<td>` inserted beforeend of a `<tr>` survives; parsed against the wrong context it
           would be dropped by the tree builder and the page would find nothing it inserted. */
        /* §8.5.6 step 5 invokes the fragment parsing algorithm with the two-argument spelling, whose
           `allowDeclarativeShadowRoots` default is FALSE — a `<template shadowrootmode>` inserted this way
           stays a template, exactly as it does through innerHTML. */
        /* §8.5.6 invokes the fragment parsing algorithm steps with no scriptingMode, so HTML §8.5.4's default
           INERT applies — an `insertAdjacentHTML`'d `<script>` is marked already started and does not run. */
        fragment_parse_begin(ctx, s, outside ? lxb_dom_interface_element(n->parent) : el, n, where, html,
                             false, false, FRAG_SCRIPTING_INERT);
        JS_FreeCString(ctx, html);
        hdr->stage = FRAG_FEED;
        return JS_STEP_YIELD;
    }
    return fragment_parse_step(ctx, hdr, s);
}

static const char *const EL_ADJACENT_HTML_STEPS[] = { FRAG_STAGES(JS_STEP_STAGE_LABEL) NULL };

static const IdlStepDecl EL_ADJACENT_HTML_STEP = {
    js_el_insert_adjacent_html, sizeof(FragmentParse), fragment_parse_visit, fragment_parse_release,
    "HTML §8.5.6 Element.insertAdjacentHTML", EL_ADJACENT_HTML_STEPS, .unforkable = fragment_parse_unforkable
};

/* §4.9 insertAdjacentElement / insertAdjacentText — the two that take a node the caller already has, or a
   string this turns into one. magic 1 = element, 2 = text. */
static JSValue js_el_insert_adjacent(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                     int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
    lxb_dom_node_t *n;
    int where;
    bool outside;

    if (!el || argc < 2) return JS_UNDEFINED;
    n = lxb_dom_interface_node(el);
    if (!adjacent_where(ctx, argv[0], &where, &outside)) return JS_EXCEPTION;
    {
        lxb_dom_node_t *added, *ref, *into;
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
            dom_cow_note_created(added);   /* this flow made it: the delta owns it and destroys it on discard */
        }
        /* §4.9 "insert adjacent" — FOUR PRE-INSERTS, not four raw tree writes. Each arm of the standard's list
           says "return the result of pre-inserting node into X before Y", so the node is adopted into the
           right document and §4.2.3's eleven validity steps decide the answer. Writing the links directly is
           what let `document.documentElement.insertAdjacentText("beforebegin", "x")` put a Text node beside
           <html> instead of throwing, and it skipped the adopt as well, so an element from another document
           landed in this tree still naming its old one. */
        switch (where) {
        case FRAG_INTO_BEFORE:      into = n->parent; ref = n;               break;
        case FRAG_INTO_AFTER:       into = n->parent; ref = n->next;         break;
        case FRAG_INTO_FIRST_CHILD: into = n;         ref = n->first_child;  break;
        case FRAG_INTO_CHILDREN:    into = n;         ref = NULL;            break;
        default: DFAIL("insertAdjacent ran with a position adjacent_where does not produce");
                 return JS_UNDEFINED;
        }
        /* The two OUTSIDE arms open with "if element's parent is null, return null" — a return, not a throw,
           which is the whole of what distinguishes these two members from insertAdjacentHTML's step 3. §4.9's
           IDL returns `Element?` from one and `undefined` from the other, so the null is the ELEMENT member's
           answer and the text member simply returns. */
        if (!into) return magic == 1 ? JS_NULL : JS_UNDEFINED;
        if (!node_pre_insert(ctx, added, into, ref)) return JS_EXCEPTION;
        /* §4.9: insertAdjacentElement returns the inserted node; insertAdjacentText returns undefined. */
        return magic == 1 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
    }
}

/* §4.9 removeAttribute / hasAttribute / toggleAttribute / hasAttributes / getAttributeNames — the rest of the
   attribute family. removeAttribute in particular had no implementation at all, so a boolean reflection had no
   way to UNSET itself and `el.hidden = false` could only ever set. magic: 0 remove, 1 has, 2 toggle. */
static JSValue js_el_attr_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
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
        dom_cow_remove_attribute(el, name);   /* the taint goes with the value */
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
            if (want) dom_cow_set_attribute(el, name, "", 0, JS_UNDEFINED);
            else dom_cow_remove_attribute(el, name);
            r = JS_NewBool(ctx, want);
        }
        break;
    }
    JS_FreeCString(ctx, name);
    return r;
}

/* §4.9's (NAMESPACE, LOCAL NAME) KEY SPACE — `getAttributeNS`, `hasAttributeNS`, `getAttributeNodeNS` and
   `removeAttributeNS`, which are the SAME lookup with four different answers. One body because the thing that
   is easy to get wrong is the KEY, not the answer: each of the four begins "if namespace is the empty string,
   set it to null" and then asks for THE attribute (not "the first") at that pair, and four copies of that are
   four chances to write `getAttribute`'s lowercasing into one of them by accident — NONE of this family
   lowercases anything.
   magic: 0 getAttributeNS, 1 hasAttributeNS, 2 getAttributeNodeNS, 3 removeAttributeNS. */
static JSValue js_el_attr_ns_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
    const char *ns, *local;
    lxb_dom_attr_t *a;
    JSValue r;

    if (!el || argc < 2) return magic == 1 ? JS_FALSE : (magic == 3 ? JS_UNDEFINED : JS_NULL);
    /* step 1 of both lookups: the empty string IS the null namespace, and so is the IDL null the `DOMString?`
       declaration made of null and undefined. */
    ns = JS_IsNull(argv[0]) ? NULL : JS_ToCString(ctx, argv[0]);
    if (ns && !ns[0]) { JS_FreeCString(ctx, ns); ns = NULL; }
    local = concolic_name_cstr(ctx, argv[1]);   /* an unknown local name denotes its SHAPE, as by-name does */
    if (!local) { if (ns) JS_FreeCString(ctx, ns); return JS_EXCEPTION; }
    a = dom_attr_get_ns(el, ns, local);
    switch (magic) {
    case 0: {
        /* The TAINT SHADOW answers first, exactly as `getAttribute` has it answer: an attacker value written
           through `setAttributeNS` came back out of Lexbor as plain bytes with its provenance gone. */
        JSValue t = dom_cow_attr_taint_ns(el, ns, local);
        if (!JS_IsUndefined(t)) { r = JS_DupValue(ctx, t); break; }   /* borrowed — dup to hand it out */
        {
            size_t vl = 0;
            const lxb_char_t *v = a ? lxb_dom_attr_value(a, &vl) : NULL;
            r = a ? JS_NewStringLen(ctx, (const char *)(v ? v : (const lxb_char_t *)""), v ? vl : 0) : JS_NULL;
        }
        break;
    }
    case 1:
        r = JS_NewBool(ctx, a != NULL);
        break;
    case 2:
        r = a ? node_wrap(ctx, lxb_dom_interface_node(a)) : JS_NULL;
        break;
    default:
        DCHECK(magic == 3, "a namespace-keyed attribute operation was declared with a magic this file does not name");
        dom_cow_remove_attribute_ns(el, ns, local);   /* the chokepoint's own step-2 guard decides "non-null" */
        r = JS_UNDEFINED;
        break;
    }
    JS_FreeCString(ctx, local);
    if (ns) JS_FreeCString(ctx, ns);
    return r;
}

/* §4.9 hasAttributes / getAttributeNames / localName / prefix / namespaceURI — pure reads over the attribute
   list Lexbor already holds. magic: 0 hasAttributes, 1 getAttributeNames. */
static JSValue js_el_attr_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
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
    lxb_dom_element_t *el = element_of_value(this_val);
    size_t n = 0;
    const lxb_char_t *v;

    if (!el) return JS_NULL;
    switch (magic) {
    case 0: v = lxb_dom_element_local_name(el, &n); break;
    case 1: v = (const lxb_char_t *)element_prefix(el, &n); break;
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

/* §2.6.1's getter for a reflected `USVString` TREATED AS A URL, over an attribute value that is present.
 *
 * THE BASE IS THE ELEMENT'S NODE DOCUMENT'S, which is what the algorithm says ("relative to element's node
 * document") and is NOT document_base_url — that one is the RUNNING REALM's active document, so an element
 * parsed by DOMParser or living in a template's contents owner would resolve against the wrong document. §4.4's
 * baseURI asks the same question of the same node and answers it with the same function; this asks it once more
 * rather than keeping a second idea of where a node lives.
 * AND IT IS THAT DOCUMENT'S BASE URL, NOT ITS ADDRESS. This read the ADDRESS, so `<base href="/app/v2/">`
 * changed nothing about `img.src`, `a.href` or `script.src` — the members through which the whole endpoint
 * surface of a page is read back — and every one of them reported a URL the page's own markup had replaced.
 *
 * A FAILED PARSE IS THE RAW VALUE. Step 2's serialization is skipped "if urlString is not failure", and step 3
 * then returns the content attribute converted to a SCALAR VALUE STRING — the USVString conversion the return
 * type carries, which is why `el.setAttribute('src', '\uD800')` reads back U+FFFD and not the lone surrogate.
 *
 * A CONCOLIC ATTRIBUTE VALUE STAYS CONCOLIC. An attacker string a flow stashed in `src` survives the round trip
 * through solver/attr_shadow.c, and resolving it is an ordinary op on the value: the REAL parse runs on the
 * concrete EXAMPLE — so a relative payload gains its absolute form the way `+` gains a concatenation — and the
 * result is DERIVED from it. Answering with the bare resolved string would de-taint the one member a page reads
 * an injected URL back out of.
 * IT IS THE DERIVATION SEAM AND NOT concolic_source_wrap, which mints AT A SOURCE. source_wrap asserts that a
 * declared source's shape is its provenance in braces, and provenance is INHERITED through a derivation — so
 * `img.src = location.hash.slice(1)` arrives carrying the declared source `location.hash` under a DERIVED
 * shape, and wrapping it aborted on that assert. It also gave every URL reflection of one element the same
 * identity, so a branch on one would have decided another. `member` is the reflection's IDL name, which is what
 * tells those derivations apart. */
static JSValue el_reflect_url(JSContext *ctx, lxb_dom_element_t *el, JSValue raw, const char *member)
{
    JSValue concrete = concolic_is(raw) ? concolic_example(ctx, raw) : JS_DupValue(ctx, raw);
    const char *base = document_base_url_of(lxb_dom_interface_node(el)->owner_document);
    UrlRecord u, b;
    const char *s;
    size_t len = 0;
    char *ser = NULL;
    bool have_base, parsed;
    JSValue out;

    /* A concolic value with no example yet has no bytes to parse; §2.6.1 has no step that invents them. */
    if (!JS_IsString(concrete)) { JS_FreeValue(ctx, concrete); return raw; }
    s = JS_ToCStringLen(ctx, &len, concrete);
    JS_FreeValue(ctx, concrete);
    if (!s) { JS_FreeValue(ctx, raw); return JS_EXCEPTION; }

    url_record_init(&u);
    url_record_init(&b);
    have_base = base && *base && url_parse(&b, base, strlen(base), NULL);
    parsed = url_parse(&u, s, len, have_base ? &b : NULL);
    if (parsed) {
        ser = url_serialize(&u, false);
        CHECK(ser != NULL, "§2.6.1: a parsed URL could not be serialized — an allocation failure, and a "
                           "reflection that answered the raw value instead would report a relative URL as "
                           "the absolute one the page is about to fetch");
    }
    url_record_free(&u);   /* owned either way — a failed parse leaves the partial record to release */
    url_record_free(&b);
    JS_FreeCString(ctx, s);

    /* Step 3, the failure path: the content attribute as a SCALAR VALUE STRING. A concolic value is returned
       AS IS — the conversion is over bytes it does not carry, and running it would ToString the shadow away. */
    if (!parsed) return concolic_is(raw) ? raw : JS_ToScalarValueString(ctx, raw);

    out = JS_NewString(ctx, ser);
    free(ser);
    if (concolic_is(raw))
        out = concolic_builtin_hook(ctx, raw, member, out);   /* consumes `out` as the example */
    JS_FreeValue(ctx, raw);
    return out;
}

/* §2.6.1's getter for a reflected `unsigned long`. `raw` is the content attribute's value or JS_NULL when it is
 * absent, and is CONSUMED — this kind has no early return for the absent case, so both paths land on the same
 * steps and an unset `<col>` answers 1 rather than the empty string every other kind answers.
 *
 * THE RANGE AND THE DEFAULT ARE TWO DIFFERENT STEPS AND THEY DO NOT AGREE. A value that PARSES but sits outside
 * `[minimum, maximum]` is pulled to the nearest end — but only when the member is CLAMPED TO THE RANGE, which
 * is the whole of what §2.6.2's `[ReflectRange]` adds; a value that does not parse at all falls to
 * `[ReflectDefault]`, or to `minimum` when there is none. `<td rowspan="99999">` is 65534 and
 * `<td rowspan="x">` is 1, from a range whose maximum is 65534 and a default of 1 — one member, two answers,
 * neither reachable from the other.
 *
 * AN OVERFLOWING RUN IS ABOVE THE MAXIMUM, NOT A PARSE ERROR. §2.3.4.2 has no upper bound, so the digits of
 * `span="99999999999"` denote a number larger than any maximum this platform declares;
 * core/html/integer_microsyntax.h reports that as `overflow` rather than wrapping, and it lands on the clamp
 * exactly as 4000 does. Calling it a parse failure would answer the DEFAULT — 1 where a browser says 1000.
 *
 * THE ANSWER IS A NUMBER, so a concolic attribute keeps its provenance THROUGH the parse: the real rules run on
 * the concrete example and the numeric result is re-wrapped, the shape el_reflect_url uses. */
static JSValue el_reflect_ulong(JSContext *ctx, const ElReflect *r, JSValue raw)
{
    JSValue concrete = concolic_is(raw) ? concolic_example(ctx, raw) : JS_DupValue(ctx, raw);
    long long minimum = r->has_range ? r->rmin : 0;
    long long maximum = r->has_range ? r->rmax : 2147483647LL;
    long long answer;
    HtmlInteger n;
    const char *s = NULL;
    size_t len = 0;
    bool ok = false;

    DCHECK(r->kind == REFLECT_ULONG, "el_reflect_ulong was handed a row of another kind");
    DCHECK(!r->has_range || r->rmin <= r->rmax,
           "a [ReflectRange] was declared with its ends swapped — §2.6.2's list is (clampedMin, clampedMax) in "
           "that order, and a reversed pair clamps every value to the smaller end");
    if (JS_IsString(concrete)) {
        s = JS_ToCStringLen(ctx, &len, concrete);
        if (!s) { JS_FreeValue(ctx, concrete); JS_FreeValue(ctx, raw); return JS_EXCEPTION; }
        ok = html_parse_non_negative_integer(s, len, &n);
    }
    JS_FreeValue(ctx, concrete);

    if (ok && !n.overflow && n.value >= minimum && n.value <= maximum) answer = n.value;
    else if (ok && r->has_range) answer = (!n.overflow && n.value < minimum) ? minimum : maximum;
    else if (r->has_dflt) answer = r->dflt;
    else answer = minimum;
    if (s) JS_FreeCString(ctx, s);

    {
        JSValue out = JS_NewInt64(ctx, answer);

        if (concolic_is(raw)) out = concolic_builtin_hook(ctx, raw, r->idl, out);
        JS_FreeValue(ctx, raw);
        return out;
    }
}

static JSValue js_el_reflect_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
    JSValue nv, r;

    DCHECK(magic >= 0 && magic < g_reflect_n,
           "a reflected property was declared with a magic the registry does not name");
    if (!el) return g_reflect[magic].kind == REFLECT_BOOL ? JS_FALSE
                  : g_reflect[magic].kind == REFLECT_STRING_NULLABLE ? JS_NULL
                  : g_reflect[magic].kind == REFLECT_ULONG
                    ? el_reflect_ulong(ctx, &g_reflect[magic], JS_NULL)
                  : JS_NewStringLen(ctx, "", 0);
    /* §2.2.1 a BOOLEAN reflection is the attribute's PRESENCE, not its value — `<input disabled>` and
       `<input disabled="false">` are both disabled, and a string reflection here would report "false".
       ASKED OF THE ATTRIBUTE LIST, because `get_attribute` answers NULL for an attribute whose VALUE is absent
       and that is the usual spelling of every one of these: lexbor's tree construction only calls
       `attr_set_value_wo_copy` when the token carried a `value_begin`, so `<input disabled>` has
       `attr->value == NULL` and a presence test written over the returned pointer reported FALSE for exactly
       the markup the attribute exists to express. It read the one case it must not miss as the absent one, for
       every boolean reflection in the table above; core/loader/document_scripts.c had to make the same
       correction for `<script defer>` and says so at its own reader. */
    if (g_reflect[magic].kind == REFLECT_BOOL)
        return JS_NewBool(ctx, lxb_dom_element_has_attribute(el, (const lxb_char_t *)g_reflect[magic].attr,
                                                             strlen(g_reflect[magic].attr)));
    nv = JS_NewString(ctx, g_reflect[magic].attr);
    r = js_el_get_attribute(ctx, this_val, 1, (JSValueConst *)&nv, 0);   /* a real string already: the reflected NAME is the engine's */
    JS_FreeValue(ctx, nv);
    /* §2.6.1's URL getter step 2a is the SAME answer the plain string reflection gives for an absent attribute
       ("if contentAttributeValue is null, then return the empty string"), so only the present case diverges. */
    if (g_reflect[magic].kind == REFLECT_ULONG)
        return el_reflect_ulong(ctx, &g_reflect[magic], r);   /* both the present and the absent value */
    if (!JS_IsNull(r))
        return g_reflect[magic].kind == REFLECT_URL ? el_reflect_url(ctx, el, r, g_reflect[magic].idl) : r;
    /* §2.6.1's two string models differ EXACTLY here: `DOMString` reads an absent attribute as the empty
       string, `DOMString?` reads it as null. A page tests one against the other (`el.ariaLabel === null`), so
       answering "" for a nullable member is a wrong value and not a lenient one. */
    return g_reflect[magic].kind == REFLECT_STRING_NULLABLE ? JS_NULL : JS_NewStringLen(ctx, "", 0);
}

static JSValue js_el_reflect_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
    JSValue verified;

    DCHECK(magic >= 0 && magic < g_reflect_n,
           "a reflected property was declared with a magic the registry does not name");
    if (g_reflect[magic].kind == REFLECT_BOOL) {
        if (!el) return JS_UNDEFINED;
        /* §2.2.1: setting true ADDS the attribute with the empty string, setting false REMOVES it. */
        if (JS_ToBool(ctx, val)) dom_cow_set_attribute(el, g_reflect[magic].attr, "", 0, JS_UNDEFINED);
        else dom_cow_remove_attribute(el, g_reflect[magic].attr);
        return JS_UNDEFINED;
    }
    /* A REFLECTED ATTRIBUTE THAT IS A TRUSTED TYPES SINK IS ONE THROUGH BOTH SPELLINGS. §3.8's table is a
       table of (element, attribute) pairs, not of member names, so `script.src = s` and
       `script.setAttribute("src", s)` are the same sink and must throw together under
       `require-trusted-types-for 'script'`; HTML says the same thing by declaring srcdoc's IDL type as
       `(TrustedHTML or DOMString)`. Nearly every reflection maps to nothing and this returns the value it was
       given. */
    if (!el) return JS_UNDEFINED;
    /* §2.6.1's `DOMString?` setter: "if the given value is null, then run this's delete the content
       attribute". The declared type is IDL_DOMSTRING_NULLABLE, so `undefined` is already this null. */
    if (g_reflect[magic].kind == REFLECT_STRING_NULLABLE && JS_IsNull(val)) {
        dom_cow_remove_attribute(el, g_reflect[magic].attr);
        return JS_UNDEFINED;
    }
    /* §2.6.1's `unsigned long` SETTER, and the sentence that makes it not the getter's mirror: "Clamped to the
       range has no effect on the setter steps." So `[ReflectRange]` is absent from these steps entirely and
       `td.colSpan = 5000` WRITES 5000 — the attribute holds it and only the getter clamps, which is what
       `td.colSpan = 5000; td.getAttribute('colspan')` reads back in a browser. What the setter does use is the
       DEFAULT: newValue starts at minimum, becomes defaultValue if the member has one, and becomes the given
       value only when that is in [minimum, 2147483647]. The declared IDL_UNSIGNED_LONG has already applied
       §3.2.4's modulo, so this runs none of the page's code and the value can only be 0..4294967295 — a page
       writing 3000000000 lands above 2147483647 and gets the default written, not a wrap. */
    if (g_reflect[magic].kind == REFLECT_ULONG) {
        /* minimum is 0 until a [ReflectPositive] kind exists; it is spelled so the day it does, this reads. */
        long long minimum = 0, given = 0, newValue;

        JS_ToInt64(ctx, &given, val);
        newValue = g_reflect[magic].has_dflt ? g_reflect[magic].dflt : minimum;
        if (given >= minimum && given <= 2147483647LL) newValue = given;
        /* "converted to the shortest possible string representing the number as a valid non-negative integer" —
           §2.3.4.2's production: base ten, no sign, no leading zeros. That is ECMAScript's own ToString of the
           number for every value this can hold, so it is the ENGINE's conversion and never a printf format, for
           the reason core/html/number_microsyntax.h gives at §2.3.4.3's counterpart. */
        verified = JS_ToString(ctx, JS_NewInt64(ctx, newValue));
        if (JS_IsException(verified)) return JS_EXCEPTION;
        el_set_attribute_internal(ctx, this_val, g_reflect[magic].attr, verified);
        JS_FreeValue(ctx, verified);
        return JS_UNDEFINED;
    }
    {
        char nsbuf[128], lobuf[64];
        const char *ns, *local;

        element_ns_and_local(el, &ns, &local, nsbuf, sizeof(nsbuf), lobuf, sizeof(lobuf));
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
   would be invisible to time travel. There was a gate that banned exactly that call outside attr_list.c
   (check_dom_chokepoint.mjs); it has been deleted, so the chokepoint is a convention now — held by attr_list.c
   being the only file that includes Lexbor's attribute mutators, and by nothing else.
   Returns an OWNED string, or NULL when the attribute is absent. */
/* THE VALUE PAIR IS THE PRIMITIVE AND THE BYTES PAIR IS BUILT ON IT, because an attribute is a slot a SOURCE
   can be stashed in and a `char *` cannot hold one. `js_el_get_attribute` answers out of §@S's (element, name)
   shadow (solver/attr_shadow.h), so what comes back for `a.href = location.hash.slice(1)` is the concolic
   itself — the triple of provenance, domain and example. A component that wants that value must take THE
   VALUE; the moment it asks for bytes the first two thirds are gone. */
JSValue element_attr_get_value(JSContext *ctx, JSValueConst el, const char *name)
{
    JSValue nv = JS_NewString(ctx, name);
    JSValue r = js_el_get_attribute(ctx, el, 1, (JSValueConst *)&nv, 0);

    JS_FreeValue(ctx, nv);
    return r;
}

void element_attr_set_value(JSContext *ctx, JSValueConst el, const char *name, JSValueConst value)
{
    el_set_attribute_internal(ctx, el, name, value);
}

char *element_attr_get(JSContext *ctx, JSValueConst el, const char *name)
{
    JSValue r = element_attr_get_value(ctx, el, name);
    char *out = NULL;

    /* THE TRIPLE DOES NOT SURVIVE THIS CALL, so a value carrying one must not arrive at it. JS_ToCString over a
       concolic reaches JS_ToStringInternal's own boundary, which owes C a real string and can only abort — and
       in a release build throws a TypeError that the `if (c)` below then DROPS, returning "the attribute is
       absent" with the throw still pending. Both endings are the §@S false negative: the sink downstream is fed
       an untainted string, or nothing at all, and a finding is silently missing rather than parked.
       The value is already there to be had — read it with element_attr_get_value and carry it. Where the
       consumer genuinely needs bytes (navigable_open's destination, url_member_set's component), carrying the
       concolic into THAT consumer is the mechanism to build; stringifying here only moves the loss upstream. */
    DCHECK(!concolic_is(r),
           "an attribute holding unknown external input was read through the BYTES accessor — the source "
           "identity and the constraint domain are taken off the triple here and only the example survives, "
           "so the sink this feeds reads as clean. Read it with element_attr_get_value and carry the value");
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

    element_attr_set_value(ctx, el, name, v);
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
        IdlArgType t;

        CHECK(g_reflect_n < (int)(sizeof(g_reflect) / sizeof(g_reflect[0])),
              "the reflection registry is full — raise it rather than dropping an interface's attributes");
        g_reflect[g_reflect_n] = r[i];
        /* THE KIND IS THE IDL TYPE, so the mapping is TOTAL and a kind with no row here CRASHES AT ITS
           DECLARATION rather than being handed a DOMString and answering wrongly at the read. That is the whole
           reason this is a switch and not the `?:` chain it was: a chain's tail is a silent default, and the
           kind added without one is exactly the kind whose setter then runs the wrong conversion.
           §2.6.2 states each pairing as a requirement of the extended attribute — `[ReflectURL]` "must only
           appear on attributes with a type of USVString", so a URL reflection declares IDL_USVSTRING and its
           §3.2.12 scalar-value conversion happens before the setter's one step is reached. A boolean's value is
           ToBoolean, total and running none of the page's code; a `DOMString?` turns null AND undefined into
           the IDL null so the body never sees the word. */
        switch (r[i].kind) {
        case REFLECT_BOOL:            t = IDL_ANY; break;
        case REFLECT_STRING:          t = IDL_DOMSTRING; break;
        case REFLECT_STRING_NULLABLE: t = IDL_DOMSTRING_NULLABLE; break;
        case REFLECT_URL:             t = IDL_USVSTRING; break;
        case REFLECT_ULONG:           t = IDL_UNSIGNED_LONG; break;
        default:
            t = IDL_ANY;
            DFAIL("a reflection was declared with a kind §2.6.1 has no processing model for — give the kind "
                  "its IDL type here and its getter steps in js_el_reflect_get");
        }
        /* §2.6.2 restricts BOTH augmenting attributes by TYPE — `[ReflectRange]` "must only be used on
           attributes with a type of unsigned long", `[ReflectDefault]` on double, long or unsigned long — so a
           row carrying either on a kind that has no step reading it is a declaration that silently does
           nothing. It crashes here instead: the row is written once and read at every get, and a default the
           getter never consults is indistinguishable from a member with no default at all. */
        DCHECK(r[i].kind == REFLECT_ULONG || (!r[i].has_range && !r[i].has_dflt),
               "a reflection declared [ReflectRange] or [ReflectDefault] on a kind whose steps do not read "
               "them — §2.6.2 allows neither outside the numeric types");
        g_reflect_set[g_reflect_n] = idl_setter_id(ctx, t, false, js_el_reflect_set, g_reflect_n);
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
 * NO PER-NODE EFFECT REACHES BACK INTO THE CHOKEPOINT — a script is queued as a flow, an endpoint recorded, a
 * custom-element reaction enqueued, a child navigable minted locally — so no entry can appear while the walk
 * that would consume it is running. But one of them ASKS: HTML §6.6.7's insertion steps run §6.6.6's allow
 * focus steps, whose second clause is §6.4.1's TRANSIENT ACTIVATION — unknown external state, so the answer is
 * a FORK, and the walk is re-entered at the same node with the arm this flow took. That is what the per-node
 * phase below is for. */
typedef struct {
    lxb_dom_node_t *root;     /* the subtree the steps run over */
    lxb_dom_node_t *cursor;   /* how far the walk has got — the resume point */
    uint8_t         inserted;
} TreeStepEntry;

/* WHERE THE WALK IS INSIDE THE NODE IT IS STANDING ON — the resume point a fork needs, one level finer than the
   cursor. Everything a node has already had done to it must be BEHIND the phase, because a re-entry without one
   re-runs it: the <iframe> gets a SECOND child navigable, the inserted <script> is prepared and its program
   queued twice, and the custom element is enqueued for upgrade twice. Each of those is a real effect on a real
   document with nothing to say it happened twice, which is exactly the shape §7.4's double load had. */
enum {
    /* §4.8.5's create-a-child-navigable, HTML §4.12.1's prepare-a-script and §4.13.3's upgrade — and the
       removing steps' pair of them. None can ask anything, so they all run in one step and the phase is past
       them before the step that can is reached. */
    TS_NODE_EFFECTS = 0,
    TS_NODE_AUTOFOCUS         /* HTML §6.6.7's insertion steps, whose step 5 can FORK */
};

/* The buffer a machine takes ownership of. Per-machine and not global, because the drain YIELDS: a global list
   would be appended to by whichever flow ran during the suspension, and the resuming one would then run another
   flow's insertion steps over another flow's nodes.
   ONE ALLOCATION, ENTRIES INCLUDED, because a fork mid-walk CLONES it: a header pointing at a separate array
   would need two visits in OPPOSITE orders for the clone and the teardown, which is the mistake JSStepVisit's
   `array` operation exists to make impossible. One block is one v->buf and cannot be got wrong. */
typedef struct {
    int      n, i;
    uint8_t  nphase;     /* TS_NODE_* — where the walk is inside e[i].cursor */
    /* §6.4.1's two CHAINED questions (sticky, then "and recently"), each of which forks: the byte says which
       one an answer is owed to, so it belongs to the walk and not to a C local that a park would forget. */
    uint8_t  ua_phase;
    TreeStepEntry e[];
} TreeStepBuf;

static TreeStepEntry *g_ts;
static int g_ts_n, g_ts_cap;

/* §4.2.3's LIVE-RANGE steps, both directions: the pre-remove steps before a node leaves the tree, and insert's
   step 4 after one enters it. */
static void element_range_steps(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase)
{
    (void)parent;
    if (phase == NODE_TREE_INSERTED)      range_did_insert(ctx, n);
    else if (phase == NODE_TREE_REMOVING) range_pre_remove(ctx, n);
}

/* §4.2.3's remove, step "for each NodeIterator object iterator whose root's node document is node's node
   document, run the NodeIterator pre-remove steps". It is a tree hook of its own rather than a line inside the
   one below, because they are two independent steps of the standard's algorithm and the one below returns early
   for a node that is not connected — which a NodeIterator rooted at a detached subtree very much is. */
static void element_iterator_pre_remove(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase)
{
    (void)parent;
    if (phase == NODE_TREE_REMOVING) node_iterator_pre_remove(ctx, n);
}

/* §4.2.3's SLOT steps, both sides of the detach — insert's two and remove's three. Registered between the
   pre-remove steps and the removing steps because that is where the standard's own `remove` runs them: after
   step 3's detach and before step 8's removing steps. */
static void element_slot_steps(JSContext *ctx, lxb_dom_node_t *n, lxb_dom_node_t *parent, int phase)
{
    if (phase == NODE_TREE_INSERTED)     slot_insert_steps(ctx, n, parent);
    else if (phase == NODE_TREE_REMOVED) slot_removed_steps(ctx, n, parent);
}

/* HTML §2.1.4 DOM trees — "THE MOVING STEPS FOR THE HTML STANDARD, given movedNode, isSubtreeRoot, and
 * oldAncestor", registered on DOM §4.2.3's moving-steps list (core/dom/node.h). TWO STEPS.
 *
 * STEP 1 dispatches to the per-local-name "HTML element moving steps", and this standard defines them for four
 * names — `a` and `area` (consider speculative loads), `img` and `source` (count this as a relevant mutation
 * for a `picture` ancestor), and `option` (update an option's nearest ancestor select). Each of those names a
 * component this engine does not have on ANY side of a tree change: there is no speculative parser, no
 * relevant-mutations counter and no nearest-ancestor-select update, so the identical clause in HTML's INSERTION
 * and REMOVING steps reaches nothing either. The absence is those components', not this step's, and a move
 * therefore loses nothing an insert or a remove would have done.
 *
 * STEP 2 is the one this engine can state: "if movedNode is a form-associated element with a non-null form
 * owner and movedNode and its form owner are no longer in the same tree, then reset the form owner of
 * movedNode." It is the reason the moving steps exist as a family at all — a control moved out from under its
 * `<form>` must lose it, and a move runs no removing steps to do that for it.
 *
 * A MOVE CANNOT CHANGE WHICH TREE A NODE IS IN — DOM §4.2.3 move step 1 makes the two shadow-including roots
 * the same — so the condition is only ever true for an element whose owner was set by a `form=` attribute
 * naming a form in a DIFFERENT tree, or by a reset that has not run since the tree changed under it. That it is
 * rarely true is not a reason to skip it: HTML §4.10.18.3 states the reset as the thing that keeps
 * `form.elements` and every entry list honest, and the failure mode of not running it is a form that submits a
 * control it no longer contains. */
static void element_moving_steps(JSContext *ctx, lxb_dom_node_t *n, bool is_subtree_root,
                                 lxb_dom_node_t *old_ancestor)
{
    JSValue wrap, owner;
    lxb_dom_node_t *ow;

    /* Step 1's arguments and no reader — see above: the four local names this standard defines moving steps
       for name three components this engine does not have, so there is nothing for them to be handed to. */
    (void)is_subtree_root; (void)old_ancestor;
    if (!n || n->type != LXB_DOM_NODE_TYPE_ELEMENT || !html_form_maybe_associated(n)) return;
    /* THE NODE'S OWN DOCUMENT'S REALM, for the reason element_tree_steps_step resolves one: two same-origin
       documents are one agent, so the flow performing the move is routinely not standing in the realm whose
       Document this element belongs to — and the form owner is a per-flow slot on a wrapper whose prototypes
       are that document's. */
    ctx = document_realm_of(n);
    DCHECK(ctx != NULL,
           "§4.2.3's moving steps reached an element in a document no realm was installed for — a document "
           "that can hold a moved node is a document a flow can run steps in, so build its realm rather than "
           "borrowing whichever one performed the move");
    wrap = node_wrap(ctx, n);
    if (JS_IsNull(wrap)) { JS_FreeValue(ctx, wrap); return; }
    owner = html_form_owner_of(ctx, wrap);
    ow = node_of(owner);
    /* "no longer in the same TREE" — §4.2's tree, which is the node's root, and not its node document: a
       control and its form can both be in one detached fragment, and moving between two detached fragments of
       one document is a move DOM §4.2.3 step 1 already refuses. */
    if (ow && node_root(n) != node_root(ow))
        JS_FreeValue(ctx, html_form_reset_owner(ctx, wrap, NULL));
    JS_FreeValue(ctx, owner);
    JS_FreeValue(ctx, wrap);
}

static void element_tree_changed(JSContext *ctx, lxb_dom_node_t *root, lxb_dom_node_t *parent, int phase)
{
    int inserted = phase == NODE_TREE_INSERTED;
    (void)ctx; (void)parent;
    /* §4.13's insertion and removing steps are the two SIDES of the tree change, so the post-detach phase is
       not one of them: it exists for §4.2.2's slot steps, which are a different component's. */
    if (phase == NODE_TREE_REMOVED) return;
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
   body can return through, so nothing recorded outlives the member that caused it.
   The entries are COPIED into the member's own block rather than the global's storage being handed over: the
   block is the thing a fork clones, so it is js-allocated like every other piece of a step machine's state,
   while the global stays the plain scratch list the chokepoint refills (element_free releases it). */
static void *element_tree_steps_take(JSContext *ctx)
{
    TreeStepBuf *b;

    if (!g_ts_n) return NULL;
    b = js_malloc(ctx, sizeof(TreeStepBuf) + sizeof(TreeStepEntry) * (size_t)g_ts_n);
    CHECK(b != NULL, "the tree-steps buffer could not be allocated — dropping it means an inserted <script> "
                     "never runs and a custom element never upgrades, silently");
    b->n = g_ts_n;
    b->i = 0;
    b->nphase = TS_NODE_EFFECTS;
    b->ua_phase = 0;
    memcpy(b->e, g_ts, sizeof *b->e * (size_t)g_ts_n);
    g_ts_n = 0;
    return b;
}

/* ONE NODE. JS_STEP_YIELD while there is more to do — which is what makes the caller's drain a yield per node —
   JS_STEP_FORK when a per-node effect's answer depends on unknown external state, and 0 when the walk is over.
   `h` is the DRIVING machine's header: a fork is snapshotted at that machine, and the arm is delivered back to
   the same call site inside the same node's phase. */
static int element_tree_steps_step(JSContext *ctx, void *vb, JSStepHdr *h)
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
            if (b->nphase == TS_NODE_EFFECTS) {
                /* §4.8.5: an inserted <iframe> CREATES A CHILD NAVIGABLE, right here, which is where the spec
                   puts it — `frame.contentWindow` answers on the line after the append. It does not suspend
                   (the child's name is minted locally), so it does not need the enqueue this walk's buffer
                   would otherwise demand; it joins <script> preparation and custom-element upgrades as one
                   more per-node effect. */
                if (iframe_element_is(n)) {
                    JSValue w = node_wrap(ctx, n);
                    iframe_create_navigable(ctx, w);
                    /* …AND §4.8.5's NEXT STEP, which is where a srcless frame's `load` comes from. The post-
                       connection steps are three — parse the sandboxing directive, create a new child
                       navigable, process the iframe attributes with initialInsertion true — and the third has
                       a branch no other algorithm reaches: a url that matches about:blank fires the iframe
                       load event steps HERE and does not navigate. §7.5.8's own note says why it has to be
                       here rather than at the load's end ("we do not fire an asynchronous load event on the
                       container element for such cases"), and without it
                       `document.body.appendChild(document.createElement("iframe"))` never fires `onload` at
                       all — which is a deadlock and not a delay, because the handler is where a test first
                       touches `contentWindow` and touching it is what would have materialized the document
                       the lifecycle walk was waiting to see. */
                    iframe_process_attributes(ctx, w, /*initialInsertion*/ true);
                    JS_FreeValue(ctx, w);
                }
                /* HTML §4.12.1: an inserted `<script>` is PREPARED — and its step 1 is what makes a script the
                   fragment parse produced inert, because §13.4's default scripting mode marked it already
                   started. core/html/html_script.c owns both halves of that pair. */
                /* NOT PARSER-INSERTED: §4.12.1.1's insertion steps are "if insertedNode is parser-inserted,
                   then return", so a node that reaches this walk has a null parser document. A parser's own
                   `<script>` is prepared at its end tag instead (html_script_parser_inserted). */
                html_script_prepare(ctx, el, /*parser_inserted*/false);
                /* HTML §4.8.12: an inserted `<source>` STARTS its parent media element's resource selection
                   algorithm, which is how a `<video>` with no `src` and only `<source>` children ever loads
                   anything. It is here rather than on node.c's tree-hook list because that list is the DOM's
                   own step families and this is an HTML ELEMENT INSERTION STEPS entry — the same family as the
                   `<script>` above and the `<iframe>` above that, which need this seam's realm (the inserted
                   node's document, not the mutating one) and its position (insert step 7, before step 8's
                   mutation record). */
                media_element_source_inserted(ctx, el);
                /* HTML §4.8.4.3.2: "The img or source HTML element insertion steps … count the mutation as a
                   relevant mutation" — so an `img` created with `createElement`, given a `src` and then
                   INSERTED updates its image data at the insertion, exactly as one whose `src` was written
                   after insertion does. It is here rather than on node.c's tree-hook list for the reason the
                   four above it are: an HTML ELEMENT INSERTION STEPS entry needs this seam's realm (the
                   inserted node's document, not the mutating one) and its position. */
                html_image_inserted(ctx, el);
                /* HTML §4.6.8.20: "The appropriate times to fetch and process the linked resource … When the
                   external resource link's link element becomes browsing-context connected." This is the seam
                   the whole lazy-chunk idiom runs through — a `<link rel=preload as=script>` is configured with
                   `createElement` and only becomes a request when it is INSERTED, and the `load` its response
                   fires is what creates the `<script src>` two lines above this one prepares. It is here rather
                   than on node.c's tree-hook list for the reason the five above it are: an HTML ELEMENT
                   INSERTION STEPS entry needs this seam's realm (the inserted node's document, not the mutating
                   one) and its position. */
                html_link_inserted(ctx, el);
                /* HTML §4.2.6's SECOND TRIGGER — "the element is not on the stack of open elements ... and it
                   becomes connected". `<style>` is the element whose insertion CREATES a CSS style sheet, and
                   the algorithm decides for itself whether this element is one. It is here rather than on
                   node.c's tree-hook list for the reason the three above it are: an HTML ELEMENT INSERTION
                   STEPS entry needs this seam's realm (the inserted node's document, not the mutating one). */
                html_style_element_update(el);
                /* DOM §4.2.3's insertion steps: an element that ENTERS a document gets its connectedCallback if
                   it is already custom, and is otherwise tried for upgrade — the other half of "learned by
                   execution", beside the <script> preparation right above it. The upgrade is ENQUEUED, never
                   run: it constructs the page's class, and this walk is C that cannot park. */
                custom_elements_element_connected(ctx, el);
                /* THE PHASE MOVES BEFORE THE STEP THAT CAN ASK, and that placement is the whole mechanism:
                   everything above is DONE for this node, and a re-entry that ran it again would mint a second
                   child navigable for one <iframe>, queue one <script>'s program twice and enqueue one custom
                   element's upgrade twice — three real effects with nothing to say they happened twice. */
                b->nphase = TS_NODE_AUTOFOCUS;
            }
            DCHECK(b->nphase == TS_NODE_AUTOFOCUS,
                   "§4.2.3's insertion steps resumed in a phase this walk never parks in — the only rest point "
                   "inside a node is §6.6.7's step 5, and a resume anywhere else means a phase was set by "
                   "something that does not park there");
            /* HTML §6.6.7's insertion steps: an element carrying the `autofocus` content attribute becomes a
               CANDIDATE on the top-level traversable's active document, to be flushed at the next rendering
               opportunity (§8.1.7.3 step 7). It does NOT focus anything here — the standard's whole point is
               that the decision is deferred and then taken once, over the whole queue. Last of the per-node
               effects because an `<iframe autofocus>` is a candidate whose focusable area is the content
               navigable created above.
               IT IS THE ONE THAT ASKS: its step 5 runs §6.6.6's allow focus steps, whose second clause is
               §6.4.1's transient activation — unknown external state, so this flow takes one arm and a sibling
               is snapshotted holding the other. */
            {
                int r = autofocus_element_inserted(el, h, &b->ua_phase);

                if (r) {
                    DCHECK(r == JS_STEP_FORK,
                           "§6.6.7's insertion steps answered with a step code that is not a fork — the only "
                           "question they ask is §6.4.1's activation state, and nothing in them calls the "
                           "page's code, which §4.2.3 forbids between two nodes' insertion steps");
                    return r;
                }
                DCHECK(b->ua_phase == 0,
                       "§6.4.1's chained questions answered §6.6.7's step 5 with the phase byte left "
                       "mid-chain — the next node would then ask the second conjunct of a question nobody "
                       "asked the first half of");
            }
        } else {
            DCHECK(b->nphase == TS_NODE_EFFECTS && b->ua_phase == 0,
                   "§4.2.3's REMOVING steps resumed mid-node — nothing in them asks anything, so there is no "
                   "point inside one for a walk to park at");
            /* §4.8.5's removing steps, the pair of the insertion steps above: an <iframe> that LEAVES a
               document destroys its child navigable. Without it a removed frame kept answering as a live one —
               `contentWindow` stayed non-null and `closed` stayed false, which is precisely what the spec files
               distinguish a destroyed navigable by. */
            if (iframe_element_is(n)) {
                JSValue w = node_wrap(ctx, n);
                iframe_destroy_navigable(ctx, w);
                JS_FreeValue(ctx, w);
            }
            /* §4.2.6's SAME TRIGGER, other side — "or disconnected". One algorithm for both, which is what
               the standard writes: its step 2 removes the sheet and its step 3 returns because the element is
               no longer connected, so a `<style>` taken out of the document stops having a style sheet and the
               one it had is orphaned rather than left claiming an owner node it no longer has. */
            html_style_element_update(el);
            custom_elements_disconnected(ctx, el);
        }
    } else {
        DCHECK(b->nphase == TS_NODE_EFFECTS && b->ua_phase == 0,
               "§4.2.3's steps resumed mid-node on a node that is not an element — every step that can rest "
               "inside a node is an element's, so this walk has no way to have parked here");
    }
    /* THE NODE IS FINISHED, so the phase belongs to the next one — reset BEFORE the cursor moves, since the
       two together are the resume point and a phase left behind would be read against a different node. */
    b->nphase = TS_NODE_EFFECTS;
    b->ua_phase = 0;
    if (n->first_child) { e->cursor = n->first_child; return JS_STEP_YIELD; }
    while (n && !n->next) n = (n == e->root) ? NULL : n->parent;
    n = n ? n->next : NULL;
    e->cursor = n;
    if (n) return JS_STEP_YIELD;
    return ++b->i < b->n ? JS_STEP_YIELD : 0;
}

static void element_tree_steps_free(JSContext *ctx, void *vb)
{
    js_free(ctx, vb);   /* ONE allocation, entries included */
}

/* THE BUFFER ACROSS A FORK. Nothing in it holds a reference — an entry names two Lexbor nodes, and the tree
   those name is the flow's own through the DOM COW delta — so the plain buffer copy IS the whole contract, and
   each arm walks on with its own cursor, its own `i` and its own per-node phase. */
static void element_tree_steps_visit(JSContext *ctx, void **slot, JSStepVisit *v)
{
    TreeStepBuf *b = *slot;

    if (!b) return;
    v->buf(ctx, slot, sizeof *b + sizeof *b->e * (size_t)b->n);
}

static bool element_tree_steps_recorded(void) { return g_ts_n != 0; }

static const IdlTreeSteps ELEMENT_TREE_STEPS = {
    element_tree_steps_take, element_tree_steps_step, element_tree_steps_free, element_tree_steps_recorded,
    element_tree_steps_visit
};

/* §4.9 `[SameObject] readonly attribute NamedNodeMap attributes`. [SameObject] is an identity the IDL states,
   so the map is cached on the element's own wrapper — a page that stashes `el.attributes` and re-reads it must
   be holding the same object. The cache lives on the wrapper because that is per-flow state the COW already
   isolates. */
static JSValue js_el_attributes(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue cached;

    (void)magic;
    if (!element_of_value(this_val)) return JS_UNDEFINED;
    if (JS_GetOwnSlot(ctx, &cached, this_val, g_attrs_key) > 0 && JS_IsObject(cached))
        return cached;
    JS_FreeValue(ctx, cached);
    cached = attr_named_node_map_new(ctx, this_val);
    JS_DefinePropertyValue(ctx, (JSValue)this_val, g_attrs_key, JS_DupValue(ctx, cached), 0);
    return cached;
}

/* §4.9 `Attr? getAttributeNode(DOMString qualifiedName)` — "the result of getting an attribute by name",
   which is §4.9.1's getNamedItem over the same element and therefore the same implementation. It was absent,
   and its absence is what made every Attr-node behaviour unreachable from script: `element.attributes` hands
   out Attr nodes but a page asks for one BY NAME, and DOM §5's own corpus builds its Attr-rooted ranges with
   exactly this call. */
static JSValue js_el_get_attribute_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                        int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
    const char *name;
    JSValue r;

    (void)magic;
    if (!el || argc < 1) return JS_NULL;
    name = JS_ToCString(ctx, argv[0]);   /* a real string by now: the declaration converted it */
    if (!name) return JS_EXCEPTION;
    r = attr_by_name(ctx, el, name);
    JS_FreeCString(ctx, name);
    return JS_IsUndefined(r) ? JS_NULL : r;
}

/* §4.9 `[CEReactions] Attr removeAttributeNode(Attr attr)` — NON-nullable, because it throws instead of
   returning null. Step 1 is an IDENTITY containment test on the list and not a name match: an Attr with the
   same name on another element, or a detached one, is a NotFoundError even though a by-name removal would have
   found something. Not a machine — it runs no author code before the `[CEReactions]` epilogue. */
static JSValue js_el_remove_attribute_node(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                           int magic)
{
    lxb_dom_element_t *el = element_of_value(this_val);
    lxb_dom_attr_t *a = argc > 0 ? attr_node_of(argv[0]) : NULL;

    (void)magic;
    if (!el) return JS_NULL;
    if (!a || a->owner != el)                                       /* step 1 */
        return JS_ThrowDOMException(ctx, "NotFoundError", "the attribute is not on this element");
    dom_cow_remove_attribute_node(a);                               /* step 2 */
    return JS_DupValue(ctx, argv[0]);                               /* step 3 — the SAME object, still alive */
}

/* §4.9's attribute change steps, fired by the mutation chokepoint for the same reason the tree steps are:
   `setAttribute`, a reflected IDL attribute, a boolean reflection unsetting itself and innerHTML's parse all
   reach the tree through one function, and a per-caller notification would miss whichever one was added last. */
static void element_attr_changed(JSContext *ctx, lxb_dom_element_t *el, const char *ns, const char *local,
                                 const char *old_val, size_t old_len, const char *val, size_t val_len)
{
    /* §4.13.3's attributeChangedCallback BELONGS TO THE ELEMENT'S DOCUMENT, for the reason §4.2.3's steps do:
       the definition set is that document's, so looking it up in the mutating realm finds nothing for an
       element in another same-origin document and the callback silently never fires. */
    JSContext *rctx = document_realm_of(lxb_dom_interface_node(el));

    (void)ctx;
    DCHECK(rctx != NULL,
           "an attribute was set on an element in a document no realm was installed for — §4.13.3's reaction "
           "resolves its definition in that document's registry, so build its realm");
    /* §4.9 "handle attribute changes" STEP 1 — the mutation record, and it comes BEFORE step 2's
       attributeChangedCallback because that is the order the steps are numbered in and both are enqueues:
       the record is delivered by §4.3's microtask and the reaction by §4.13.6's drain, so a page that
       observes an attribute AND defines the element sees the record queued first.
       THE OLD VALUE IS THE ONE THE CHOKEPOINT CARRIED ACROSS THE WRITE. This hook fires AFTER the write —
       §9.4.6 stores the value and then handles the change — so the element no longer has it, and reading it
       back off the element would report the NEW value as the old one for every mutation record. */
    mutation_observer_queue_record(rctx, MR_TYPE_ATTRIBUTES, lxb_dom_interface_node(el), local, ns,
                                   old_val, old_len, JS_UNDEFINED, JS_UNDEFINED, NULL, NULL);
    custom_elements_attribute_changed(rctx, el, ns, local, old_val, old_len, val, val_len);
    /* §4.2.2's two attribute change steps — a slottable's `slot` and a slot's `name`. They re-derive the name
       from the attribute that is NOW there, which is why the whole hook had to move after the write. */
    slot_attribute_changed(rctx, el, ns, local);
    /* HTML §4.8.11.2's `src` change step: "if a src attribute of a media element is set or changed, the user
       agent must invoke the media element's media element load algorithm". It is HERE, and not in the media
       element's own reflection setter, because a content attribute has more than one spelling and the setter
       answers for one of them — see core/html/media_element.c. The NEW value goes with it: "(Removing the src
       attribute does not do this)" is a question about the change and not about the element. */
    media_element_attr_changed(rctx, el, ns, local, val);
    /* HTML §4.8.4.3.2's relevant mutations for an `img`: "The element's src, srcset, width, or sizes
       attributes are set, changed, or removed", and the `crossorigin`/`referrerpolicy` state changes beside
       them. Here for the reason the media element's `src` is — a content attribute has more than one spelling
       (`img.src = u`, `setAttribute`, `attributes.src.value = u`) and the IDL reflection's setter answers for
       exactly one of them, which is how `<img>` came to reflect its address without ever requesting it. */
    html_image_attr_changed(rctx, el, ns, local);
    /* HTML §4.6.8.20's other appropriate times — the `rel` that CREATES the external resource link on an
       already-connected element, and the `href`/`as`/`type`/`media` changes. Here for the reason `img`'s `src`
       above is: a content attribute has more than one spelling (`l.href = u`, `setAttribute`,
       `attributes.href.value = u`) and the IDL reflection's setter answers for exactly one of them. */
    html_link_attr_changed(rctx, el, ns, local);
    /* HTML §4.12.1's `async` change step: "when an async attribute is added to a script element el, the user
       agent must set el's force async to false". Here for the reason `src` above is: a content attribute has
       more than one spelling, and the IDL setter answers for one of them. */
    html_script_attr_changed(rctx, el, ns, local, val);
    /* HTML §4.8.5: an `<iframe>` with a content navigable and no `srcdoc` NAVIGATES when its `src` is set,
       changed or removed. It is here beside the other four for that family's own reason — `frame.src = u`,
       `setAttribute`, `removeAttribute` and an `innerHTML` reparse are one write through one chokepoint — and
       it needs this seam's realm (the element's node document, not the mutating one) because §4.8.5's navigate
       resolves the address against that document and takes its policy container. */
    iframe_attr_changed(rctx, el, ns, local);
    /* HTML §2.6.1's attribute change steps for an ELEMENT-REFLECTING member: writing `aria-labelledby` by any
       spelling drops what `el.ariaLabelledByElements = [x]` recorded, so the next read resolves the ids the
       attribute now names. Here for the reason `src` and `async` above are: a content attribute has more than
       one spelling and the IDL setter answers for exactly one of them. */
    aria_mixin_attribute_changed(rctx, el, ns, local);
    /* HTML §4.2.3's SECOND SITUATION: "the base element is the first base element in tree order with an href
       content attribute in its Document, and its href content attribute is changed". Here for the reason
       `src` and `async` above are — a content attribute has more than one spelling and the IDL setter answers
       for exactly one of them — and this member has three (`b.href =`, `setAttribute`, `attributes.href.value
       =`), all of which move where every relative URL in the document resolves. */
    html_base_element_attr_changed(rctx, el, ns, local);
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
/* THE RUNTIME THIS GROUP WAS DECLARED IN. element_free is core/platform.h's release column, so it is handed a
   JSRuntime rather than deriving one from a realm — and JS_FreeValueRT will drop a reference against WHATEVER
   runtime it is given, silently, so the one thing the retarget to a runtime can get wrong is the runtime. This
   is the two-sided half of that: the declaration records it, the release asserts it, and the whole forty-two-
   component cascade below is covered by the one assert at the boundary the column reaches. */
static JSRuntime *g_element_rt;
/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. */
static int g_refl_base = -1, g_refl_n;
static int g_id_get_attr = -1, g_id_set_attr = -1, g_id_matches = -1, g_id_closest = -1,
           g_id_inner_get = -1, g_id_inner_set = -1, g_id_outer_get = -1, g_id_outer_set = -1,
           g_id_set_html_unsafe = -1, g_id_set_html = -1,
           g_id_adj_html = -1, g_id_adj_el = -1, g_id_adj_text = -1, g_id_attr_names = -1,
           g_id_remove_attr = -1, g_id_has_attr = -1, g_id_toggle_attr = -1, g_id_get_attr_node = -1,
           g_id_has_attrs = -1,
           /* §4.9's (namespace, local name) family — the four members that key on §4.9's own identity. */
           g_id_get_attr_ns = -1, g_id_has_attr_ns = -1, g_id_get_attr_node_ns = -1, g_id_remove_attr_ns = -1,
           g_id_set_attr_ns = -1,
           /* §4.9's NODE-valued members — the ones that name an attribute by object rather than by any name. */
           g_id_set_attr_node = -1, g_id_remove_attr_node = -1;

void element_init(JSContext *ctx)
{
    static const IdlArgType ADJ_ANY[2] = { IDL_DOMSTRING, IDL_ANY };
    static const IdlArgType TOGGLE[2] = { IDL_DOMSTRING, IDL_ANY };   /* `optional boolean force` is ToBoolean */
    /* `(DOMString? namespace, DOMString localName)` — §4.9's namespace-keyed argument list, and the one that
       carries `getAttributeNodeNS`'s and `removeAttributeNS`'s too. */
    static const IdlArgType NS_LOCAL[2] = { IDL_DOMSTRING_NULLABLE, IDL_DOMSTRING };
    static const IdlArgType NS_QNAME_VALUE[3] = { IDL_DOMSTRING_NULLABLE, IDL_DOMSTRING, IDL_DOMSTRING };
    static const IdlArgType ONE_ATTR[1] = { IDL_INTERFACE };
    JSClassDef d = { "Element" };

    DCHECK(g_element_rt == NULL,
           "element_init ran twice — this group is declared once per AGENT, and a second declaration re-mints "
           "the class ids every element wrapper already alive is branded with");
    g_element_rt = JS_GetRuntime(ctx);
    node_init(ctx);
    JS_NewClassID(JS_GetRuntime(ctx), &g_element_class);
    JS_NewClass(JS_GetRuntime(ctx), g_element_class, &d);
    node_claim_type(LXB_DOM_NODE_TYPE_ELEMENT, g_element_class);
    /* §13.3's serializer, declared before the four members that name it — `getHTML` is its own declaration and
       innerHTML's and outerHTML's getters are two magics on the same machine. */
    fragment_serializer_init(ctx);
    /* §8.7 Timers's Sanitizer, declared before the members that resolve one — §8.5.2's two filtering members read a
       `sanitizer` option, and its own interface object is installed per realm from its declaration. */
    sanitizer_init(ctx);

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
    g_id_inner_get = idl_getter_id_step(ctx, fragment_serializer_decl(), FRAGMENT_SERIALIZE_CHILDREN);
    g_id_inner_set = idl_setter_id_step(ctx, IDL_DOMSTRING, true, &EL_SET_HTML_STEP, ELEMENT_SET_INNER_HTML);
    g_id_outer_get = idl_getter_id_step(ctx, fragment_serializer_decl(), FRAGMENT_SERIALIZE_SELF);
    g_id_outer_set = idl_setter_id_step(ctx, IDL_DOMSTRING, true, &EL_SET_HTML_STEP, ELEMENT_SET_OUTER_HTML);
    g_id_set_html_unsafe = element_declare_set_html_unsafe(ctx, ELEMENT_SET_HTML_UNSAFE);
    g_id_set_html = element_declare_set_html(ctx, ELEMENT_SET_HTML);
    /* §4.9's three adjacent members. The HTML one takes two DOMStrings; the other two take a position and a
       value the IDL leaves alone (a Node, or a DOMString this stringifies into a Text node). */
    g_id_adj_html = idl_method_id_step(ctx, IDL_2STR, 2, NULL, 0, &EL_ADJACENT_HTML_STEP, 0);
    g_id_adj_el = idl_method_id(ctx, ADJ_ANY, 2, js_el_insert_adjacent, 1);
    g_id_adj_text = idl_method_id(ctx, IDL_2STR, 2, js_el_insert_adjacent, 2);
    g_id_has_attrs = idl_method_id(ctx, NULL, 0, js_el_attr_list, 0);
    g_id_attr_names = idl_method_id(ctx, NULL, 0, js_el_attr_list, 1);
    /* §4.9's namespace-keyed family. `DOMString? namespace` is the FIRST argument of every one of them, and it
       is declared nullable rather than tested in the body: Web IDL turns null AND undefined into the IDL null
       before ToString is ever reached, so `getAttributeNS(undefined, "x")` must not look for the namespace
       whose URL is the four characters `null`. */
    g_id_get_attr_ns = idl_method_id(ctx, NS_LOCAL, 2, js_el_attr_ns_op, 0);
    g_id_has_attr_ns = idl_method_id(ctx, NS_LOCAL, 2, js_el_attr_ns_op, 1);
    g_id_get_attr_node_ns = idl_method_id(ctx, NS_LOCAL, 2, js_el_attr_ns_op, 2);
    g_id_remove_attr_ns = idl_method_id(ctx, NS_LOCAL, 2, js_el_attr_ns_op, 3);
    /* `[CEReactions] undefined setAttributeNS(DOMString? namespace, DOMString qualifiedName,
       (TrustedType or DOMString) value)` — a machine, because its step 2 is the default policy's callback. */
    g_id_set_attr_ns = idl_method_id_step(ctx, NS_QNAME_VALUE, 3, NULL, 0, &EL_SET_ATTR_NS_STEP, 0);
    g_id_get_attr_node = idl_method_id(ctx, IDL_1STR, 1, js_el_get_attribute_node, 0);
    g_id_remove_attr = idl_method_id(ctx, IDL_1STR, 1, js_el_attr_op, 0);
    g_id_has_attr = idl_method_id(ctx, IDL_1STR, 1, js_el_attr_op, 1);
    g_id_toggle_attr = idl_method_id(ctx, TOGGLE, 2, js_el_attr_op, 2);
    idl_optional_from(1);   /* §4.9: `toggleAttribute(qualifiedName, optional force)` */
    {
        /* ELEMENT TIMING's `partial interface Element` is one member and that member is a whole reflection:
           `[CEReactions, Reflect] attribute DOMString elementTiming`, whose content attribute name §2.6.2 takes
           from the IDL name ASCII-lowercased. The attribute is a MARKER — it names an element the Element
           Timing observer should report — so mirroring it is not a stub standing in for behaviour, it is the
           entirety of what the IDL declares here; PerformanceElementTiming is a separate interface and is
           honestly absent. Element's IDL declares no other reflection of its own. */
        static const ElReflect R[] = {
            { "id", "id", REFLECT_STRING }, { "className", "class", REFLECT_STRING },
            { "slot", "slot", REFLECT_STRING },
            { "elementTiming", "elementtiming", REFLECT_STRING },
        };
        g_refl_n = (int)(sizeof(R) / sizeof(R[0]));
        g_refl_base = element_declare_reflections(ctx, R, g_refl_n);
    }
    /* WAI-ARIA's `Element includes ARIAMixin` — 52 members, declared beside §4.9's own three because they are
       the same kind of thing on the same interface: 44 reflections into the one registry above, and 8 members
       of §2.6.1's element-reflection model, which is its own component. */
    aria_mixin_init(ctx);

    idl_indexed_init(ctx);      /* the exotic class every indexed interface is built on */
    attr_init(ctx);             /* §4.9.2 Attr — registered for node type 2, which node_wrap had no interface for */
    /* AFTER attr_init, because both of these name the Attr CLASS and the machine attr.c declares: §4.9's "set
       an attribute" is one algorithm serving four members across two interfaces, and the interface that owns
       the Attr is the one that owns it. */
    /* `[CEReactions] Attr? setAttributeNode(Attr attr)` — ONE machine shared with NamedNodeMap's setNamedItem,
       because §4.9's "set an attribute" is what all four of those members are. Magic 0: `this` IS the element.
       The `Attr attr` position is an INTERFACE type, so a non-Attr is a TypeError before step 1. */
    g_id_set_attr_node = idl_method_id_step(ctx, ONE_ATTR, 1, NULL, 0, attr_set_attribute_decl(), 0);
    idl_iface_brand(attr_class_id());
    g_id_remove_attr_node = idl_method_id(ctx, ONE_ATTR, 1, js_el_remove_attribute_node, 0);
    idl_iface_brand(attr_class_id());

    g_attrs_key = JS_NewAtom(ctx, "__attributesSlot");
    CHECK(g_attrs_key != JS_ATOM_NULL, "the attributes slot key could not be interned");
    /* SELECTORS §3's matching arena, before anything that can run a query. It is the AGENT's and not a
       machine's, which is what lets a selector walk park and fork with nothing lexbor-shaped in its state. */
    selector_match_init();
    collections_init(ctx);      /* NodeList and HTMLCollection, which childNodes and children are */
    dom_token_list_init(ctx);   /* its class must exist before classList names it */
    /* §6's TRAVERSERS, before the tree hooks below: §4.2.3's remove runs §6.1's pre-remove steps BEFORE the
       removing steps, and the hook list runs in registration order, so the order here IS the standard's. */
    node_iterator_init(ctx);
    tree_walker_init(ctx);
    range_init(ctx);
    /* §4.2.3's remove runs the LIVE RANGE pre-remove steps FIRST, then §6.1's, then the removing steps — and
       the hook list runs in registration order, so this order IS the standard's. */
    node_add_tree_hook(element_range_steps);
    node_add_tree_hook(element_iterator_pre_remove);
    node_add_tree_hook(element_slot_steps);
    /* …AND THE MOVING STEPS, which are a DIFFERENT list and deliberately not one of the three above: DOM
       §4.2.3's move runs none of the insertion or removing steps, so the two lists share no registrant and a
       component that belongs on both says so twice. */
    node_add_moving_hook(element_moving_steps);
    /* HTML §4.2.3's FIRST SITUATION, BEFORE element_tree_changed. That hook PREPARES an inserted `<script>`,
       which resolves its `src` against the document base URL — so inserting `<div><base href=x><script
       src=y></div>` must freeze the base before the script is prepared, or the one script the markup put
       after the base is the one script that does not see it. */
    node_add_tree_hook(html_base_element_tree_steps);
    node_add_tree_hook(element_tree_changed);
    /* §4.3's RECORD IS QUEUED LAST, which is where §4.2.3 numbers it: insert step 8 follows step 7's insertion
       steps and custom-element reactions, and remove steps 15-16 follow the removing steps and the
       disconnectedCallback. The hook list runs in registration order, so this line IS that ordering. */
    mutation_observer_init(ctx);
    node_add_tree_hook(mutation_observer_tree_steps);
    dom_cow_set_cdata_hook(mutation_observer_character_data);
    idl_set_tree_steps(&ELEMENT_TREE_STEPS);
    dom_cow_set_attr_hook(element_attr_changed);
    realm_declare_intrinsic(element_install_proto);
    element_view_init(ctx);   /* CSSOM VIEW §6's `partial interface Element`, installed on the prototype below */
    custom_elements_init(ctx);
    html_script_init(ctx);    /* §4.12.1's `already started` slot, which the fragment parse below writes */
    html_base_element_init(ctx);   /* §4.2.3's `href` setter, whose getter is not a reflection */
    cssom_init(ctx);          /* CSSStyleDeclaration, which HTMLElement's `style` attribute names */
    css_style_sheet_init(ctx);   /* CSSOM §6.1's StyleSheet and CSSStyleSheet, which a `<style>` element creates */
    /* CSSOM §4.4's MediaList, BEFORE the rules: §7.3's `media` is [PutForwards=mediaText] and css_rule.c reads
       that setter's id out of this component while installing CSSMediaRule.prototype. */
    media_list_init(ctx);
    css_rule_init(ctx);          /* CSSOM §6.4's rules, §7.2/§7.3's conditional group rule, which a sheet holds */
    css_rule_list_init(ctx);     /* CSSOM §6.4.1 CSSRuleList, the collection §6.1.2's cssRules hands back */
    style_sheet_list_init(ctx);  /* CSSOM §6.2's collections, which §6.1's create adds every sheet to */
    /* HTML §4.2.6's association between the two. AFTER the sheet interface it creates, and after
       node_add_tree_hook's list above, because its own registration is on §4.2.3's children-changed family and
       the standard numbers that family after the mutation record the list's last entry queues. */
    html_style_element_init(ctx);
    html_element_init(ctx);   /* the HTML half, which builds HTMLElement and the per-tag interfaces on this */
    agent_state_ptr("element", &g_element_rt,
                    "the runtime this whole DOM group declared into, and the declaration latch");
    agent_state_atom("element", &g_attrs_key, "the [SameObject] NamedNodeMap cache slot on an element's wrapper");
    agent_state_ptr("element", &g_ts, "the tree-steps recorder's scratch list");
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
    /* DOM §4.2.7: `Element includes NonDocumentTypeChildNode` — previousElementSibling / nextElementSibling. */
    node_install_non_doctype_child_mixin(ctx, proto);
    idl_install_accessor_step(ctx, proto, "innerHTML", g_id_inner_get, g_id_inner_set);
    idl_install_accessor_step(ctx, proto, "outerHTML", g_id_outer_get, g_id_outer_set);
    /* §8.5's three partial-interface markup members. `setHTML` is the SAFE one — "set and filter HTML given
       target, html, options, and TRUE" — which is §8.7 Timers's sanitizer, and it is here now that §8.7 Timers is. */
    idl_install_method(ctx, proto, "setHTML", 1, g_id_set_html);
    idl_install_method(ctx, proto, "setHTMLUnsafe", 1, g_id_set_html_unsafe);
    fragment_serializer_install_get_html(ctx, proto);
    idl_install_method(ctx, proto, "insertAdjacentHTML", 2, g_id_adj_html);
    idl_install_method(ctx, proto, "insertAdjacentElement", 2, g_id_adj_el);
    idl_install_method(ctx, proto, "insertAdjacentText", 2, g_id_adj_text);
    /* The rest of the attribute family. removeAttribute had no implementation at all, which is also why a
       boolean reflection could not unset itself. */
    idl_install_accessor(ctx, proto, "attributes", js_el_attributes, 0, -1);
    idl_install_method(ctx, proto, "getAttributeNames", 0, g_id_attr_names);
    idl_install_method(ctx, proto, "getAttributeNode", 1, g_id_get_attr_node);
    idl_install_method(ctx, proto, "removeAttribute", 1, g_id_remove_attr);
    idl_install_method(ctx, proto, "hasAttribute", 1, g_id_has_attr);
    idl_install_method(ctx, proto, "toggleAttribute", 1, g_id_toggle_attr);
    idl_install_method(ctx, proto, "hasAttributes", 0, g_id_has_attrs);
    /* §4.9's (namespace, local name) family. Their absence was not a missing convenience: an SVG or MathML
       subtree in an HTML page carries its `xlink:href` in a namespace the by-name members cannot reach, so a
       page reading one got null and a page writing one created a second, null-namespace attribute beside it. */
    idl_install_method(ctx, proto, "getAttributeNS", 2, g_id_get_attr_ns);
    idl_install_method(ctx, proto, "setAttributeNS", 3, g_id_set_attr_ns);
    idl_install_method(ctx, proto, "removeAttributeNS", 2, g_id_remove_attr_ns);
    idl_install_method(ctx, proto, "hasAttributeNS", 2, g_id_has_attr_ns);
    idl_install_method(ctx, proto, "getAttributeNodeNS", 2, g_id_get_attr_node_ns);
    /* §4.9.9: `setAttributeNode` and `setAttributeNodeNS` are the SAME algorithm, verbatim, in one sentence —
       the NS suffix carries no behavioural difference at all, so it is one machine installed twice. */
    idl_install_method(ctx, proto, "setAttributeNode", 1, g_id_set_attr_node);
    idl_install_method(ctx, proto, "setAttributeNodeNS", 1, g_id_set_attr_node);
    idl_install_method(ctx, proto, "removeAttributeNode", 1, g_id_remove_attr_node);
    JS_SetPropertyFunctionList(ctx, proto, js_element_name_parts,
                               (int)(sizeof(js_element_name_parts) / sizeof(js_element_name_parts[0])));
    /* §4.9's OWN reflections, and only those: `id`, `class` and `slot`. `src`, `name` and `content` used to be
       here too, which is three properties Element's IDL does not declare — they belong to HTMLScriptElement,
       to a dozen form interfaces and to HTMLMetaElement, and they are installed there now. */
    element_install_reflections(ctx, proto, g_refl_base, g_refl_n);
    /* WAI-ARIA's ARIAMixin, which the IDL mixes into Element and therefore into every element interface. The
       string half is installed as ELEMENT reflections (this registry); the element-reflection half names the
       target kind, because ElementInternals includes the same eight members over a different target. */
    aria_mixin_install_strings(ctx, proto);
    aria_mixin_install_elements(ctx, proto, ARIA_TARGET_ELEMENT);
    /* §4.9's two Shadow DOM members — `attachShadow` and the `shadowRoot` getter, which the interface
       declares on Element and not on HTMLElement. */
    shadow_root_install_element_members(ctx, proto);
    /* §4.2.9: `Element includes Slottable`. */
    slot_install_slottable_mixin(ctx, proto);
    /* CSSOM VIEW §6's `partial interface Element` — the scroll and client geometry. Per realm because its
       answers are per realm: a child navigable's viewport is a different size from the traversable's. */
    element_view_install(ctx, proto);
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

void element_free(JSRuntime *rt)
{
    DCHECK(rt == g_element_rt,
           "the DOM group was released against a runtime that is not the one it declared in — every JSValue and "
           "every atom the cascade below gives back would have its reference subtracted from a runtime that "
           "never took it, and JS_FreeValueRT reports nothing at all when it is the wrong one");
    g_element_rt = NULL;
    /* THE TWO DOM-WRITE HOOKS THIS FILE CLAIMED, GIVEN BACK BEFORE THE CASCADE RUNS. Both live in
       solver/dom_cow.c and name code in this group — §4.3's character-data record and §4.9's attribute-changed
       steps — so a release that kept them would leave the DOM chokepoint calling into components the lines
       below are freeing. It is the defect core/agent_state.h found in idb_transaction, and it is here rather
       than at the end because the very next call can run a finalizer that mutates the tree. */
    dom_cow_set_cdata_hook(NULL);
    dom_cow_set_attr_hook(NULL);
    /* THE TREE-STEPS RECORDER'S SCRATCH LIST. It is the chokepoint's own storage, refilled between members and
       emptied by every take, so anything still counted here is a mutation whose insertion steps no member ever
       drained — which the args machine already asserts at its own end and this states again at the teardown,
       where the answer is the whole agent's and not one member's. */
    DCHECK(g_ts_n == 0, "the agent is being torn down with tree steps still recorded — an inserted <script> "
                        "never ran and a custom element never upgraded, and the mutation that recorded them "
                        "was not inside a declared member");
    free(g_ts);
    g_ts = NULL;
    g_ts_n = g_ts_cap = 0;
    element_view_free();
    html_element_free(rt);
    html_style_element_free(rt);   /* before the sheet interface whose objects it holds */
    style_sheet_list_free(rt);
    css_rule_list_free(rt);
    css_rule_free(rt);
    media_list_free(rt);         /* after the rules, whose `media` attribute holds one */
    css_style_sheet_free(rt);
    cssom_free(rt);
    selector_match_free();   /* after every component that can still match a selector */
    custom_elements_free(rt);
    html_base_element_free();
    html_script_free(rt);
    mutation_observer_free(rt);
    range_free(rt);
    tree_walker_free(rt);
    node_iterator_free(rt);
    node_filter_free(rt);
    dom_token_list_free(rt);
    collections_free(rt);
    attr_free(rt);
    if (g_attrs_key != JS_ATOM_NULL) { JS_FreeAtomRT(rt, g_attrs_key); g_attrs_key = JS_ATOM_NULL; }
    /* §4.7's DocumentFragment, §4.8's ShadowRoot and §4.2.2's slots WERE FREED HERE and are not any more. Not
       one of the three is declared by element_init — all three are declared by document_init — so this cascade
       was undoing another row's work, which is the shape core/platform.c's own table check forbids one level
       up ("a platform component releases agent state it never declared"). They are released from
       document_agent_free now, and `document` is declared after `element`, so reverse declaration order gives
       them back BEFORE this cascade runs: nothing here reads a slot key or a class of theirs.
       fragment_serializer and sanitizer stay, because element_init is what declares them. */
    fragment_serializer_free();
    sanitizer_free();
    idl_indexed_free(rt);
    aria_mixin_free(rt);   /* its state key is the AGENT's; its 44 registry rows go with the registry below */
    /* the prototypes are the REALMS' — each is released with its context */
    g_reflect_n = 0;
    /* NODE.C IS LAST, and it is the member of this group that holds real references rather than slot keys: the
       WRAPPER IDENTITY TABLE names every node wrapper ever minted, and a wrapper holds its prototype, which
       holds the realm — four surviving wrappers were measured keeping a whole JSContext alive at refcount 2212
       on the shipped entry, reported by the runtime's leak walk as anonymous Functions with nothing naming the
       owner. It goes after every member above because those are the ones that can still resolve a wrapper
       (node_wrap_peek answers out of this table), so emptying it first would answer them all with UNDEFINED.
       IT WAS CALLED TWICE, once here and once at the top of this function under a comment claiming node.c had
       been "the one member missing from the cascade" — it never was; the call at the bottom predates that
       claim by four months. The second call was harmless only because node_free happens to be idempotent, and
       a teardown whose safety rests on that is one edit away from a double free. */
    node_free(rt);
}
