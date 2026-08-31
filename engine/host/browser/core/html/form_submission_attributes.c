/* HTML §4.10.19.6 "Form submission attributes" — see form_submission_attributes.h for why these two members
   are a component and the section's other four are registry rows. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <lexbor/dom/dom.h>
#include <lexbor/ns/ns.h>

#include "check.h"
#include "quickjs.h"
#include "solver/concolic.h"      /* the empty-string test runs on the value's own example */
#include "core/dom/node.h"
#include "core/dom/element.h"     /* §2.6.1's URL model, steps 2 and 3 of both algorithms below */
#include "core/dom/document.h"    /* §4.5's `URL` — the node document's ADDRESS, which step 1 answers with */
#include "core/idl_args.h"
#include "core/html/form_submission_attributes.h"

/* §4.10.19.6'S TWO MEMBERS — ONE LIST, EXPANDED WHERE EACH FACT IS NEEDED: the IDL name, the content attribute
   it is stated over, and WHICH ELEMENTS DECLARE IT. The three columns cannot drift because installing a member
   resolves all of them through this one table, which is core/dom/dom_token_list.c's shape for the same reason:
   a member and its attribute are one fact, and a second copy of it is the one that goes wrong.
   `tags` is the brand: Web IDL §3.7.6 Attributes' receiver check is "does this implement the interface", and
   HTML §3.2.2 Elements in the DOM decides which interface an element implements from its namespace and local
   name — so the tag list IS the interface list, spelled in the terms this file can actually test. */
typedef struct { const char *idl, *attr; const char *const *tags; } FsaMember;
static const char *const FSA_FORM_TAGS[]   = { "form", NULL };
static const char *const FSA_SUBMIT_TAGS[] = { "input", "button", NULL };
static const FsaMember FSA[] = {
    { "action",     "action",     FSA_FORM_TAGS },
    { "formAction", "formaction", FSA_SUBMIT_TAGS },
};
#define FSA_N ((int)(sizeof(FSA) / sizeof(FSA[0])))

/* Declared once per AGENT (the IDL pool is sealed after agent init); installed per realm. */
static int g_set_id[FSA_N] = { -1, -1 };

/* WEB IDL §3.7.6 Attributes' RECEIVER CHECK, a THROW and not an assert: the member sits on a prototype and a
   page reaches an accessor off one with `.call` on anything at all. The namespace is part of the test because
   §3.2.2 is — an element named `form` in the SVG namespace implements no HTML interface. */
static lxb_dom_element_t *fsa_receiver(JSContext *ctx, JSValueConst this_val, const FsaMember *m)
{
    lxb_dom_node_t *n = node_of(this_val);

    if (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT && n->ns == LXB_NS_HTML) {
        size_t len = 0;
        const lxb_char_t *local = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
        int i;

        /* An HTML-namespace element's local name is ASCII-lowercase already, which is what lets this compare
           bytes rather than fold — the same reading core/html/global_attributes.c states at its own. */
        for (i = 0; local && m->tags[i]; i++)
            if (strlen(m->tags[i]) == len && memcmp(local, m->tags[i], len) == 0)
                return lxb_dom_interface_element(n);
    }
    JS_ThrowTypeError(ctx, "%s was reached on an element that does not declare it", m->idl);
    return NULL;
}

/* §4.10.19.6 step 1's SECOND CONDITION — "attribute's value is the empty string".
 *
 * IT IS DECIDED ON THE VALUE'S OWN EXAMPLE, which is the engine's ordinary work and not a concretization this
 * file invents: a source stashed in `action` carries the bytes the run actually built, and the real predicate
 * runs on them exactly as `+` runs on them. A concolic with NO example yet has no bytes to test and §4.10.19.6
 * has no step that invents them, so it is NOT empty here — which hands it to steps 2-3, where
 * element_reflect_url_get answers with the value itself for the same reason. That is the only answer that
 * keeps the provenance: returning the document's address for an unknown attribute would replace an attacker's
 * URL with the page's own and report a same-origin destination for a request that is not one. */
static bool fsa_value_is_empty(JSContext *ctx, JSValueConst v)
{
    JSValue concrete = concolic_is(v) ? concolic_example(ctx, v) : JS_DupValue(ctx, v);
    bool empty = false;

    if (JS_IsString(concrete)) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, concrete);

        if (s) { empty = (len == 0); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, concrete);
    return empty;
}

static JSValue js_fsa_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    const FsaMember *m;
    lxb_dom_element_t *el;
    JSValue attribute;

    DCHECK(magic >= 0 && magic < FSA_N,
           "§4.10.19.6's getter ran with a magic the member table does not name — the magic IS the index into "
           "that table, so a value outside it is a member this component did not install");
    m = &FSA[magic];
    el = fsa_receiver(ctx, this_val, m);
    if (!el) return JS_EXCEPTION;

    /* Step 1: "Let attribute be this's action attribute." Taken as a VALUE and not as bytes, because an
       attacker string a flow stashed here is the whole reason this member is interesting to §@S. */
    attribute = element_attr_get_value(ctx, this_val, m->attr);
    if (JS_IsException(attribute)) return attribute;

    /* Step 2: "If attribute is null or attribute's value is the empty string, then return this's node
       document's URL." §4.5's URL is the document's ADDRESS and not its base URL — a document carrying a
       `<base href>` still submits a bare `<form>` to its own address, so document_base_url_of would be the
       wrong one of the two. */
    if (JS_IsNull(attribute) || fsa_value_is_empty(ctx, attribute)) {
        const char *addr;

        JS_FreeValue(ctx, attribute);
        DCHECK(lxb_dom_interface_node(el)->owner_document != NULL,
               "§4.10.19.6 step 1 read a form submission attribute on an element with no node document");
        addr = document_url_of(lxb_dom_interface_node(el)->owner_document);
        return JS_NewString(ctx, addr ? addr : "");
    }

    /* Steps 2 and 3 ARE §2.6.1's URL model, called rather than copied: "Let urlString be the result of
       encoding-parsing-and-serializing a URL given attribute's value, relative to this's node document. If
       urlString is not failure, then return urlString. Return attribute's value, converted to a scalar value
       string." */
    return element_reflect_url_get(ctx, el, attribute, m->idl);
}

/* §2.6.2's `[ReflectSetter]`, which is the whole of what the extended attribute means: the setter reflects, so
   it writes the content attribute and nothing else. The value crossed Web IDL §3.2.12 USVString at the
   declaration, which is where the scalar-value conversion the return type carries happens. */
static JSValue js_fsa_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
    DCHECK(magic >= 0 && magic < FSA_N, "§4.10.19.6's setter ran with a magic the member table does not name");
    if (!fsa_receiver(ctx, this_val, &FSA[magic])) return JS_EXCEPTION;
    element_attr_set_value(ctx, this_val, FSA[magic].attr, val);
    return JS_UNDEFINED;
}

void form_submission_attributes_init(JSContext *ctx)
{
    int i;

    DCHECK(g_set_id[0] < 0, "form_submission_attributes_init ran twice in one runtime — the setters are "
                            "declared once per AGENT and installed once per realm");
    for (i = 0; i < FSA_N; i++)
        g_set_id[i] = idl_setter_id(ctx, IDL_USVSTRING, false, js_fsa_set, i);
}

/* The agent-lifetime release. The only agent state is the two POOL IDS — ints, so there is nothing to hand
   back — and they are RESET for the reason core/html/html_base_element.c resets its one: a second runtime in
   one process would install a member out of a pool that no longer holds it. */
void form_submission_attributes_free(void)
{
    int i;

    for (i = 0; i < FSA_N; i++) g_set_id[i] = -1;
}

void form_submission_attributes_install(JSContext *ctx, JSValueConst proto, const char *member)
{
    int i;

    /* THE MEMBER INSTALLED IS THE ONE THE INTERFACE ASKED FOR — the same rule core/dom/dom_token_list.c states
       at its own install: which member and which prototype are both the CALLER's, and naming the table cell
       instead would say, to anything reading this line, that one call installs both of §4.10.19.6's members on
       whichever prototype it was handed. */
    for (i = 0; i < FSA_N; i++)
        if (!strcmp(FSA[i].idl, member)) {
            DCHECK(g_set_id[i] >= 0,
                   "a §4.10.19.6 member was installed before form_submission_attributes_init declared its "
                   "[ReflectSetter] setter");
            idl_install_accessor(ctx, proto, member, js_fsa_get, i, g_set_id[i]);
            return;
        }
    DFAIL("an interface asked for a form submission attribute §4.10.19.6 does not state as its own algorithm — "
          "the section's other four answer from the attribute alone and are reflection registry rows in "
          "core/html/html_element.c, so a name outside this table belongs there instead");
}
