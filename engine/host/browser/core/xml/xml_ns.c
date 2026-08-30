/* See xml_ns.h. */
#include <string.h>

#include <lexbor/core/array_obj.h>
#include <lexbor/core/mraw.h>

#include "check.h"
#include "core/xml/xml_ns.h"
#include "solver/flow.h"

/* ONE BINDING. `prefix` NULL is §3's DefaultAttName arm — the default namespace, which is a binding of no
   prefix rather than a binding of the empty prefix, because §6.2 gives it its own scoping sentence and its own
   answer for attributes. `ns` NULL is "bound to nothing", which §6.2 produces legally (`xmlns=""` "has the same
   effect, within the scope of the declaration, of there being no default namespace") and which §5's No Prefix
   Undeclaring produces illegally for a prefix. Storing the illegal one is what keeps resolution TOTAL — see
   xml_ns.h on why a reported declaration is still recorded. */
typedef struct {
    const char *prefix; size_t prefix_len;
    const char *ns;     size_t ns_len;
} XmlNsBinding;

/* THE BINDINGS ARE ONE FLAT STACK AND THE FRAMES ARE INDICES INTO IT, which is not a packing trick: §6.1
   resolves a prefix to its INNERMOST declaration, and a single top-down walk of a flat stack visits declarations
   in exactly innermost-first order across frame boundaries with no per-frame loop to get wrong. A frame is then
   only the index to truncate back to when its end-tag arrives. */
struct XmlNsScope {
    lexbor_array_obj_t *bindings;   /* XmlNsBinding */
    lexbor_array_obj_t *frames;     /* size_t — each the bindings length when its start-tag opened */
    lexbor_mraw_t      *arena;      /* the copied prefix and namespace-name bytes; see xml_ns.h on its lifetime */
    Flow               *owner;      /* the flow that created it; see xml_ns.h's last paragraph */
};

static bool str_is(const char *s, size_t len, const char *lit)
{
    return s && len == strlen(lit) && memcmp(s, lit, len) == 0;
}

static bool str_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

/* §Offensive-programming's "assert every invariant eagerly at its origin", for the one invariant this component
   makes about itself rather than about its input. */
static void assert_owner(const XmlNsScope *s)
{
    DCHECK(s != NULL, "a namespace scope operation ran on no scope");
    DCHECK(s->owner == flow_running(),
           "an XML namespace scope stack was reached by a flow other than the one that created it — the stack "
           "is flow-private C memory that no COW delta captures, so two flows standing on one of them are two "
           "timelines writing one set of bindings. What has to be built is this state's JSStepVisit "
           "declaration in the step machine that holds the parse, and it is not a byte copy: a binding's "
           "prefix and namespace name are interior pointers into THIS stack's arena, so a copied binding names "
           "the original arm's bytes. core/html/tree_construction.c is the same operation for the HTML tree "
           "builder and core/dom/core/html/fragment_parser.c's fragment_parse_unforkable is what stands in until a state declares one");
}

const char *xml_ns_error_message(XmlNsError err)
{
    switch (err) {
    case XML_NS_OK:
        return "no namespace constraint was violated";
    case XML_NS_ERR_XML_PREFIX_REBOUND:
        return "namespace constraint \"Reserved Prefixes and Namespace Names\": the prefix \"xml\" is by "
               "definition bound to http://www.w3.org/XML/1998/namespace and MUST NOT be bound to any other "
               "namespace name";
    case XML_NS_ERR_XML_NAMESPACE_ON_OTHER_PREFIX:
        return "namespace constraint \"Reserved Prefixes and Namespace Names\": prefixes other than \"xml\" "
               "MUST NOT be bound to http://www.w3.org/XML/1998/namespace";
    case XML_NS_ERR_XML_NAMESPACE_AS_DEFAULT:
        return "namespace constraint \"Reserved Prefixes and Namespace Names\": "
               "http://www.w3.org/XML/1998/namespace MUST NOT be declared as the default namespace";
    case XML_NS_ERR_XMLNS_PREFIX_DECLARED:
        return "namespace constraint \"Reserved Prefixes and Namespace Names\": the prefix \"xmlns\" is used "
               "only to declare namespace bindings and MUST NOT itself be declared";
    case XML_NS_ERR_XMLNS_NAMESPACE_ON_OTHER_PREFIX:
        return "namespace constraint \"Reserved Prefixes and Namespace Names\": prefixes other than \"xmlns\" "
               "MUST NOT be bound to http://www.w3.org/2000/xmlns/";
    case XML_NS_ERR_XMLNS_NAMESPACE_AS_DEFAULT:
        return "namespace constraint \"Reserved Prefixes and Namespace Names\": http://www.w3.org/2000/xmlns/ "
               "MUST NOT be declared as the default namespace";
    case XML_NS_ERR_XMLNS_ELEMENT_PREFIX:
        return "namespace constraint \"Reserved Prefixes and Namespace Names\": element names MUST NOT have "
               "the prefix \"xmlns\"";
    case XML_NS_ERR_PREFIX_UNDECLARING:
        return "namespace constraint \"No Prefix Undeclaring\": in a namespace declaration for a prefix, the "
               "attribute value MUST NOT be empty";
    case XML_NS_ERR_PREFIX_UNDECLARED:
        return "namespace constraint \"Prefix Declared\": the namespace prefix, unless it is \"xml\" or "
               "\"xmlns\", MUST have been declared in a namespace declaration attribute in the start-tag of "
               "the element where it is used or in an ancestor element";
    }
    DFAIL("xml_ns_error_message was handed a value that is not an XmlNsError — the enum is the whole list of "
          "sentences this component can report and a value outside it names no constraint");
    return "";
}

XmlNsAttKind xml_ns_att_kind(const XmlQName *attr_name)
{
    DCHECK(attr_name != NULL, "the NSAttName production was asked about no name");
    /* [2] PrefixedAttName ::= 'xmlns:' NCName. The QName parse has already established that the local part is
       an NCName, which is the whole of [2]'s tail, so the prefix is the entire question here. */
    if (attr_name->prefix && str_is(attr_name->prefix, attr_name->prefix_len, "xmlns"))
        return XML_NS_ATT_PREFIXED;
    /* [3] DefaultAttName ::= 'xmlns'. UNPREFIXED — `p:xmlns` is an ordinary attribute whose local part happens
       to be spelled `xmlns`, and treating it as a declaration would bind a namespace a document never declared. */
    if (!attr_name->prefix && str_is(attr_name->local, attr_name->local_len, "xmlns"))
        return XML_NS_ATT_DEFAULT;
    return XML_NS_ATT_NONE;
}

XmlNsScope *xml_ns_scope_create(void)
{
    XmlNsScope *s = lexbor_calloc(1, sizeof *s);

    CHECK(s != NULL, "the XML namespace scope stack could not be allocated");
    s->bindings = lexbor_array_obj_create();
    s->frames = lexbor_array_obj_create();
    s->arena = lexbor_mraw_create();
    CHECK(s->bindings && s->frames && s->arena,
          "the XML namespace scope stack's storage could not be allocated");
    CHECK(lexbor_array_obj_init(s->bindings, 16, sizeof(XmlNsBinding)) == LXB_STATUS_OK,
          "the XML namespace binding stack could not be initialised");
    CHECK(lexbor_array_obj_init(s->frames, 16, sizeof(size_t)) == LXB_STATUS_OK,
          "the XML namespace frame stack could not be initialised");
    CHECK(lexbor_mraw_init(s->arena, 1024) == LXB_STATUS_OK,
          "the XML namespace name arena could not be initialised");
    s->owner = flow_running();
    return s;
}

/* THE TEARDOWN, SHARED BY THE TWO ANSWERS BELOW — they differ in what they assert and in nothing else. */
static void ns_scope_free(XmlNsScope *s)
{
    lexbor_array_obj_destroy(s->bindings, true);
    lexbor_array_obj_destroy(s->frames, true);
    lexbor_mraw_destroy(s->arena, true);
    lexbor_free(s);
}

void xml_ns_scope_destroy(XmlNsScope *s)
{
    if (!s) return;
    assert_owner(s);
    DCHECK(lexbor_array_obj_length(s->frames) == 0,
           "an XML namespace scope stack was DESTROYED with element scopes still open — every start-tag's push "
           "is matched by its end-tag's pop, so a residue here is a tree builder that lost an end-tag and a "
           "tree whose later elements resolved against namespaces that had gone out of scope. A parse that "
           "STOPPED with elements open has a residue that is not that defect, and it says so by calling "
           "xml_ns_scope_abandon");
    ns_scope_free(s);
}

void xml_ns_scope_abandon(XmlNsScope *s)
{
    if (!s) return;
    assert_owner(s);
    /* NO RESIDUE ASSERTION: the open scopes ARE what abandonment means. The frames array is POD (one `size_t`
       per open element) and the bindings it indexes live in this stack's own arena, so an abandoned stack
       frees exactly what a finished one does — there is no per-frame release to skip. */
    ns_scope_free(s);
}

void xml_ns_push(XmlNsScope *s)
{
    size_t *frame;

    assert_owner(s);
    frame = lexbor_array_obj_push(s->frames);
    CHECK(frame != NULL, "an XML namespace scope could not be pushed");
    *frame = lexbor_array_obj_length(s->bindings);
}

void xml_ns_pop(XmlNsScope *s)
{
    size_t *frame;

    assert_owner(s);
    frame = lexbor_array_obj_pop(s->frames);
    DCHECK(frame != NULL,
           "an XML end-tag popped a namespace scope that no start-tag pushed — §6.1's scope runs from the "
           "beginning of a start-tag to the end of the CORRESPONDING end-tag, so an unmatched pop is a tree "
           "builder that has stopped tracking which element it is closing");
    if (!frame) return;
    DCHECK(*frame <= lexbor_array_obj_length(s->bindings),
           "an XML namespace frame names more bindings than the stack holds — a frame index is only ever the "
           "stack's own length at push time and the stack only grows between a push and its pop");
    /* The bytes are NOT freed here — see xml_ns.h on why the arena's lifetime is the whole parse. */
    lexbor_array_obj_delete(s->bindings, *frame, lexbor_array_obj_length(s->bindings) - *frame);
}

size_t xml_ns_depth(const XmlNsScope *s)
{
    assert_owner(s);
    return lexbor_array_obj_length(s->frames);
}

/* Copy into the arena. An NCName is never empty and a namespace name that IS empty is stored as the absence
   (see XmlNsBinding), so a zero length never reaches here. */
static const char *arena_dup(XmlNsScope *s, const char *p, size_t len)
{
    void *copy;

    DCHECK(p != NULL && len > 0, "the XML namespace arena was asked to copy nothing");
    copy = lexbor_mraw_dup(s->arena, p, len);
    CHECK(copy != NULL, "an XML namespace binding's bytes could not be copied");
    return (const char *)copy;
}

XmlNsError xml_ns_declare(XmlNsScope *s, const XmlQName *attr_name, const char *value, size_t value_len)
{
    XmlNsAttKind kind;
    XmlNsError err = XML_NS_OK;
    const char *prefix = NULL;
    size_t prefix_len = 0;
    XmlNsBinding *b;

    assert_owner(s);
    DCHECK(attr_name != NULL, "a namespace declaration was recorded from no attribute name");
    kind = xml_ns_att_kind(attr_name);
    DCHECK(kind != XML_NS_ATT_NONE,
           "xml_ns_declare was handed an attribute name that is not an NSAttName — whether a name declares a "
           "namespace is xml_ns_att_kind's question and must be asked before this, because binding an ordinary "
           "attribute's name as a prefix would put the whole subtree in a namespace the document never wrote");
    DCHECK(lexbor_array_obj_length(s->frames) > 0,
           "a namespace declaration was recorded with no element scope open — §3's declaration is attached to "
           "an element and §6.1 scopes it to that element's tag, so a binding with no frame belongs to nothing "
           "and would never be popped");
    DCHECK(value != NULL || value_len == 0,
           "a namespace declaration was recorded with no attribute value — §3 requires the normalized value to "
           "be a URI reference or the empty string, and the tokenizer produces one or the other, never neither");

    if (kind == XML_NS_ATT_PREFIXED) {
        prefix = attr_name->local;                    /* [2]'s NCName IS the declared prefix */
        prefix_len = attr_name->local_len;
        DCHECK(xml_name_is_ncname(prefix, prefix_len),
               "a namespace declaration declared a prefix that is not an NCName — [2] PrefixedAttName is "
               "'xmlns:' NCName, so the tokenizer has already rejected the alternative and a non-NCName here "
               "is a name that was never run through the production");
    }

    /* §3's Reserved Prefixes and Namespace Names, then §5's No Prefix Undeclaring. The standard gives no order
       for a declaration that violates two at once, so this reports the sentence about the PREFIX before the one
       about the VALUE: `xmlns:xmlns=""` is illegal whatever its value, and reporting "the value must not be
       empty" for it would send an author to fix the half that is not the problem. */
    if (kind == XML_NS_ATT_PREFIXED) {
        if (str_is(prefix, prefix_len, "xmlns"))
            err = XML_NS_ERR_XMLNS_PREFIX_DECLARED;
        else if (str_is(prefix, prefix_len, "xml")) {
            /* "It MAY, but need not, be declared" — so `xmlns:xml` bound to its own namespace name is legal
               and must not be reported. */
            if (!str_is(value, value_len, XML_NS_XML_NAMESPACE)) err = XML_NS_ERR_XML_PREFIX_REBOUND;
        }
        else if (str_is(value, value_len, XML_NS_XML_NAMESPACE))
            err = XML_NS_ERR_XML_NAMESPACE_ON_OTHER_PREFIX;
        else if (str_is(value, value_len, XML_NS_XMLNS_NAMESPACE))
            err = XML_NS_ERR_XMLNS_NAMESPACE_ON_OTHER_PREFIX;
        else if (value_len == 0)
            err = XML_NS_ERR_PREFIX_UNDECLARING;
    }
    else {
        /* §6.2: "The attribute value in a default namespace declaration MAY be empty" — so the empty value is
           checked for NOTHING here, which is the whole difference from the prefixed arm above. */
        if (str_is(value, value_len, XML_NS_XML_NAMESPACE))
            err = XML_NS_ERR_XML_NAMESPACE_AS_DEFAULT;
        else if (str_is(value, value_len, XML_NS_XMLNS_NAMESPACE))
            err = XML_NS_ERR_XMLNS_NAMESPACE_AS_DEFAULT;
    }

#if APICLIENT_DEV
    /* XML 1.0 §3.1's WFC "Unique Att Spec" — "An attribute name MUST NOT appear more than once in the same
       start-tag or empty-element tag" — is the tokenizer's, and it is a FATAL error there rather than a
       reported one here (xml_ns.h's second paragraph). Two declarations of one prefix on one tag are two
       attributes with the identical name `xmlns:p`, so the tokenizer has already stopped; this asserts that
       contract rather than silently letting the later one win. */
    {
        size_t i, start = *(size_t *)lexbor_array_obj_last(s->frames);
        for (i = start; i < lexbor_array_obj_length(s->bindings); i++) {
            const XmlNsBinding *e = lexbor_array_obj_get(s->bindings, i);
            bool same = (e->prefix == NULL) == (prefix == NULL)
                        && (prefix == NULL || str_eq(e->prefix, e->prefix_len, prefix, prefix_len));

            DCHECK(!same,
                   "one start-tag declared the same namespace prefix twice — those are two attributes with the "
                   "identical name, which XML 1.0's Unique Att Spec well-formedness constraint makes a FATAL "
                   "error the tokenizer must stop on before the tree builder records either of them");
        }
    }
#endif

    b = lexbor_array_obj_push(s->bindings);
    CHECK(b != NULL, "an XML namespace binding could not be recorded");
    b->prefix = prefix ? arena_dup(s, prefix, prefix_len) : NULL;
    b->prefix_len = prefix ? prefix_len : 0;
    b->ns = value_len ? arena_dup(s, value, value_len) : NULL;
    b->ns_len = value_len;
    return err;
}

/* §6.1's innermost-first resolution. `prefix` NULL asks for §6.2's default namespace. Returns the binding, or
   NULL when the prefix has no declaration in scope; a binding whose `ns` is NULL is a prefix or default that
   an inner declaration took OUT of scope, which is a found answer of "no namespace name" for the default and
   §5's Prefix Declared violation for a prefix. */
static const XmlNsBinding *lookup(const XmlNsScope *s, const char *prefix, size_t prefix_len)
{
    size_t i = lexbor_array_obj_length(s->bindings);

    while (i-- > 0) {
        const XmlNsBinding *b = lexbor_array_obj_get(s->bindings, i);

        if ((b->prefix == NULL) != (prefix == NULL)) continue;
        if (prefix == NULL || str_eq(b->prefix, b->prefix_len, prefix, prefix_len)) return b;
    }
    return NULL;
}

XmlNsError xml_ns_expand(const XmlNsScope *s, const XmlQName *qname, XmlNsNameKind kind, XmlExpandedName *out)
{
    const XmlNsBinding *b;

    assert_owner(s);
    DCHECK(qname != NULL && out != NULL, "a qualified name was expanded from or into nothing");
    DCHECK(qname->local != NULL && qname->local_len > 0,
           "a qualified name reached expansion with no local part — [11] LocalPart is an NCName and an NCName "
           "is never empty, so this name was never run through the QName production");
    DCHECK(qname->prefix == NULL || qname->prefix_len > 0,
           "a qualified name reached expansion carrying an EMPTY prefix — [9] UnprefixedName is the absence of "
           "a prefix and `:local` is not a QName at all, so an empty-but-present prefix is a third state the "
           "production does not have and would resolve against a binding no declaration can make");

    out->prefix = qname->prefix; out->prefix_len = qname->prefix_len;
    out->local = qname->local;   out->local_len = qname->local_len;

    if (qname->prefix) {
        /* The two prefixes §3 binds BY DEFINITION. §5's Prefix Declared names both as its exceptions — "unless
           it is xml or xmlns" — so neither consults the stack: a document that never declares `xml` still
           resolves it, and a document that declares it to anything else was reported at the declaration and
           still resolves it to the namespace name the standard fixes. */
        if (str_is(qname->prefix, qname->prefix_len, "xmlns")) {
            /* §3, last sentence of the constraint: "Element names MUST NOT have the prefix xmlns." An
               ATTRIBUTE with that prefix is [2] PrefixedAttName — a declaration — and the DOM keeps it as an
               Attr in the XMLNS namespace, which is the expansion asked for here. */
            if (kind == XML_NS_NAME_ELEMENT) return XML_NS_ERR_XMLNS_ELEMENT_PREFIX;
            out->ns = XML_NS_XMLNS_NAMESPACE; out->ns_len = strlen(XML_NS_XMLNS_NAMESPACE);
            return XML_NS_OK;
        }
        if (str_is(qname->prefix, qname->prefix_len, "xml")) {
            out->ns = XML_NS_XML_NAMESPACE; out->ns_len = strlen(XML_NS_XML_NAMESPACE);
            return XML_NS_OK;
        }
        b = lookup(s, qname->prefix, qname->prefix_len);
        if (!b || !b->ns) return XML_NS_ERR_PREFIX_UNDECLARED;
        out->ns = b->ns; out->ns_len = b->ns_len;
        return XML_NS_OK;
    }

    /* §6.2: "Default namespace declarations do not apply directly to attribute names… The namespace name for an
       unprefixed attribute name ALWAYS has no value." Not "has no value unless a default is in scope" — the
       default namespace is not consulted at all, which is what makes `<good a="1" n1:a="2"/>` legal under a
       default declaration that binds `n1`'s namespace (§6.3's second example). */
    if (kind == XML_NS_NAME_ATTRIBUTE) {
        out->ns = NULL; out->ns_len = 0;
        return XML_NS_OK;
    }
    b = lookup(s, NULL, 0);
    /* "If there is no default namespace declaration in scope, the namespace name has no value" — and a default
       declared as `xmlns=""` is §6.2's explicit "same effect… of there being no default namespace", which is
       the b->ns NULL case rather than a separate rule. */
    out->ns = (b && b->ns) ? b->ns : NULL;
    out->ns_len = (b && b->ns) ? b->ns_len : 0;
    return XML_NS_OK;
}
