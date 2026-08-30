/* HTML §8.5.8 — the `XMLSerializer` interface.
 *
 * WHAT IT IS. Two members over one algorithm that lives somewhere else entirely: `new XMLSerializer()`, whose
 * "constructor steps are to do nothing", and `serializeToString(root)`, whose method steps are one sentence —
 * "Return the XML serialization of root given false". That algorithm is DOM Parsing and Serialization §3.2.1
 * XML Serialization and it is core/xml/xml_serialize.h's, for the reason that header gives: HTML §8.5.4 The
 * innerHTML property's fragment serializing algorithm steps reach the same algorithm with require well-formed
 * TRUE, so a copy owned by this member would be the second transcription of it.
 *
 * SO THIS FILE IS THE INTERFACE AND NOTHING ELSE, and that is the whole of its job: the class a receiver is
 * branded against, the constructor, the per-realm prototype, and the one member declared against §3.2.1's
 * machine. HTML §8.5.8's own note says why the interface is shaped this way at all — "The design of
 * XMLSerializer, as a class that needs to be constructed and then have its serializeToString() method called,
 * is an unfortunate historical artifact. If we were designing this functionality today it would be a
 * standalone function." Which is exactly what §3.2.1 is here, with this wrapper around it.
 *
 * THE OBJECT CARRIES NO STATE, AND THAT IS THE STANDARD RATHER THAN AN OMISSION. §8.5.8's constructor steps do
 * nothing and its one member reads only its argument, so unlike core/html/domparser.c — whose §8.5.1 step 2
 * has to answer out of the DOMParser's OWN relevant global — there is no fact for the object to hold. It
 * therefore has no record, no finalizer and no gc_mark, which is also why core/agent_state.h's finalizer
 * hazard (a collector reading a class id its own release has already reset) cannot arise here.
 *
 * THE RECEIVER IS STILL BRANDED. Web IDL §3.7.7 Operations makes `serializeToString` a method of the interface
 * prototype object, so a page reaches it with `.call` on anything it likes and
 * `XMLSerializer.prototype.serializeToString.call({}, node)` must be a TypeError rather than a serialization.
 * That is a THROW and not an assert, because the receiver is the PAGE's input.
 *
 * THE MEMBER IS A STEP MACHINE BECAUSE THE WALK IS OF THE PAGE'S SIZE. §3.2.1 recurses over the tree and loops
 * over each element's attributes, and `new XMLSerializer().serializeToString(document)` on a real page is a
 * walk of the whole document; run inside one opcode it would hold the scheduler for all of it with every other
 * flow parked behind it. The machine rests once per node and once per attribute — core/xml/xml_serialize.h's
 * stage list — so a flow can be preempted, parked to the cold tier and resumed mid-serialization. */
#include <stdbool.h>

#include <lexbor/dom/dom.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/dom/node.h"
#include "core/html/xml_serializer.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/xml/xml_serialize.h"

/* PER REALM — §3.7. The class is the AGENT's; the prototype lives in quickjs's per-context class-proto slot. */
static JSClassID g_class;
static int       g_ready;
static int       g_id_ctor = -1, g_id_serialize = -1;

/* WHERE §8.5.8's ONE MEMBER RESTS. Its own two stages bracket §3.2.1's, which are expanded here with this
   member's own leading text — stage identity is the LABEL, so a second consumer of the same algorithm names
   its own standard's step in front of §3.2.1's and the two never resolve to each other on a resume. */
#define XMLSER_STAGES(X) \
    X(XMLSER_ENTER,  "HTML §8.5.8 The XMLSerializer interface serializeToString(root): the Web IDL §3.7.7 " \
                     "Operations brand check on the receiver, and the entry into DOM Parsing and " \
                     "Serialization §3.2.1 XML Serialization's steps 1-4") \
    XML_SERIALIZE_ALGO_STAGES(X, XMLSER, "HTML §8.5.8 The XMLSerializer interface serializeToString(root)") \
    X(XMLSER_RETURN, "HTML §8.5.8 The XMLSerializer interface serializeToString(root): return the XML " \
                     "serialization of root given false")
enum { IDL_STEP_STAGE_BASE(XMLSER_STAGES) XMLSER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const XMLSER_STEPS[] = { XMLSER_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* `new XMLSerializer()` — "the constructor steps are to do nothing". The object exists to be a BRAND and holds
   nothing, so this is the mint and no more; a record would be state the standard does not define. */
static JSValue js_xml_serializer_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    JSValue proto, obj;

    (void)this_val; (void)argc; (void)argv; (void)magic;
    DCHECK(g_ready, "an XMLSerializer was constructed before its interface was declared");
    proto = JS_GetClassProto(ctx, g_class);
    DCHECK(!JS_IsNull(proto), "XMLSerializer.prototype was asked for in a realm that never ran its install");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    JS_FreeValue(ctx, proto);
    CHECK(!JS_IsException(obj), "an XMLSerializer object could not be allocated");
    return obj;
}

static int js_xml_serializer_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                                  JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    XmlSerializeState *s = st;

    (void)cb_result; (void)out_cb; (void)out_argc;

    STEP_DISPATCH(XMLSER_STAGES, hdr->stage, hdr->def->algorithm, JS_STEP_ABRUPT);

    STEP_ARM(XMLSER_ENTER);
    {
        lxb_dom_node_t *n;

        /* Web IDL §3.7.7's brand check — a THROW, because the receiver is the page's input. */
        if (JS_GetClassID(hdr->this_val) != g_class) {
            JS_ThrowTypeError(ctx, "this is not an XMLSerializer");
            return JS_STEP_ABRUPT;
        }
        DCHECK(argc >= 1, "§8.5.8's serializeToString was reached with no argument — `Node root` is REQUIRED, "
                          "and Web IDL §3.6's arity check in the declaration is what throws for a call that "
                          "omits it, before this body runs");
        /* `Node root` — Web IDL §3.2.15's brand is the DECLARATION's (idl_iface_brand), so what arrives here
           is a node wrapper and never the page's own object. */
        n = node_of(argv[0]);
        DCHECK(n != NULL, "§8.5.8's serializeToString was reached with an argument that is not a Node — the "
                          "declared interface type is what refuses everything else, before step 1");
        /* "Return the XML serialization of root given FALSE." The flag is §3.2.1's `require well-formed`, and
           this member is the consumer that passes false; HTML §8.5.4's fragment serializing algorithm steps
           are the one that passes true. */
        xml_serialize_start(ctx, hdr, s, n, /*require_well_formed*/false, XMLSER_DISPATCH, XMLSER_RETURN);
        return JS_STEP_YIELD;
    }

    /* §3.2.1's OWN stages, every one of them named so that a stage added to XML_SERIALIZE_ALGO_STAGES does not
       compile until it has an arm here — the same reason core/dom/node.c names `clone a node`'s six. */
    STEP_ARM(XMLSER_DISPATCH);
    STEP_ARM(XMLSER_RECORD);
    STEP_ARM(XMLSER_NAME);
    STEP_ARM(XMLSER_ATTRS);
    STEP_ARM(XMLSER_OPEN);
    STEP_ARM(XMLSER_LEAF);
    STEP_ARM(XMLSER_NEXT);
    STEP_ARM(XMLSER_CLOSE);
    return xml_serialize_run(ctx, hdr, s, XMLSER_DISPATCH);

    STEP_ARM(XMLSER_RETURN);
    *presult = xml_serialize_result(ctx, s);
    return JS_STEP_DONE;
}

static void js_xml_serializer_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    xml_serialize_visit_state(ctx, st, v);
}

static const IdlStepDecl XML_SERIALIZER_STEP = {
    /* No release: every allocation is xml_serialize_visit_state's, and the teardown discharges that one list. */
    js_xml_serializer_step, sizeof(XmlSerializeState), js_xml_serializer_visit, NULL,
    "HTML §8.5.8 XMLSerializer.serializeToString (over DOM Parsing and Serialization §3.2.1's XML "
    "serialization algorithm)",
    XMLSER_STEPS
};

void xml_serializer_init(JSContext *ctx)
{
    JSClassDef d = { "XMLSerializer", NULL, NULL };
    /* §8.5.8: `DOMString serializeToString(Node root)`. One required argument of an INTERFACE type, so
       `serializeToString(null)` and `serializeToString("<a/>")` are TypeErrors thrown by the conversion — the
       algorithm's own "Anything else: throw a TypeError" arm is what the type is standing in for. */
    static const IdlArgType ONE_NODE[1] = { IDL_INTERFACE };

    DCHECK(!g_ready, "xml_serializer_init ran twice — the interface is declared once per AGENT");
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    JS_NewClass(JS_GetRuntime(ctx), g_class, &d);
    g_id_ctor = idl_method_id(ctx, NULL, 0, js_xml_serializer_ctor, 0);
    g_id_serialize = idl_method_id_step(ctx, ONE_NODE, 1, NULL, 0, &XML_SERIALIZER_STEP, 0);
    /* "a platform object implementing the interface crosses as itself" — every node wrapper is one class and
       the algorithm serves every node KIND, so the brand is `a Node` with no narrowing beside it. */
    idl_iface_brand(node_class_id());
    g_ready = 1;
    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. */
    agent_state_class("xml_serializer", &g_class, "HTML §8.5.8's XMLSerializer class");
    agent_state_flag("xml_serializer", &g_ready, "HTML §8.5.8's declaration latch");
    agent_state_id("xml_serializer", &g_id_ctor, "HTML §8.5.8's XMLSerializer constructor declaration");
    agent_state_id("xml_serializer", &g_id_serialize, "HTML §8.5.8's serializeToString declaration");
    realm_declare_intrinsic(xml_serializer_install_proto);
}

/* The class goes back to 0 because a class is registered in a RUNTIME — core/agent_state.h's one policy — and
   the latch goes with it, since a carried latch makes the next agent's `xml_serializer_init` return before
   re-registering anything. */
void xml_serializer_free(void)
{
    DCHECK(g_ready, "§8.5.8's XMLSerializer was released in an agent that never declared it");
    g_id_ctor = g_id_serialize = -1;
    g_ready = 0;
    g_class = 0;
}

void xml_serializer_install_proto(JSContext *ctx)
{
    JSValue proto, prev;

    DCHECK(g_ready, "a realm asked for XMLSerializer.prototype before the interface was declared");
    prev = JS_GetClassProto(ctx, g_class);
    DCHECK(JS_IsNull(prev), "xml_serializer_install_proto ran twice in one realm");
    JS_FreeValue(ctx, prev);
    proto = JS_NewObject(ctx);
    CHECK(!JS_IsException(proto), "XMLSerializer.prototype could not be allocated");
    idl_interface_tag(ctx, proto, "XMLSerializer");
    idl_install_method(ctx, proto, "serializeToString", 1, g_id_serialize);
    JS_SetClassProto(ctx, g_class, proto);
}

void xml_serializer_install(JSContext *ctx, JSValueConst global)
{
    JSValue proto = JS_GetClassProto(ctx, g_class), ctor;

    DCHECK(g_ready, "XMLSerializer was installed before xml_serializer_init declared it");
    DCHECK(!JS_IsNull(proto), "XMLSerializer was installed in a realm that never ran its prototype install");
    ctor = idl_step_constructor(ctx, "XMLSerializer", 0, g_id_ctor);
    JS_SetConstructor(ctx, ctor, proto);
    JS_FreeValue(ctx, proto);
    JS_SetPropertyStr(ctx, (JSValue)global, "XMLSerializer", ctor);
}
