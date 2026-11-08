/* A document's last painted frame — see core/paint/painted_frame.h. */
#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/realm.h"
#include "core/paint/document_paint.h"
#include "core/paint/painted_frame.h"

static JSClassID  g_class;                          /* a published frame: its opaque is the DocumentPaintFrame */
static JSClassID  g_slot = JS_INVALID_CLASS_ID;      /* the per-realm record (a baseline object) */
static JSAtom     g_atom_frame = JS_ATOM_NULL;
static JSRuntime *g_rt;                              /* the runtime this component was declared in */

/* The collector dispatched here through the class, so the opaque is read without the static the release
   resets (core/agent_state.h). */
static void pf_finalizer(JSRuntime *rt, JSValue v)
{
    JSClassID id;
    DocumentPaintFrame *f = JS_GetAnyOpaque(v, &id);

    (void)rt;
    if (f != NULL) document_paint_frame_free(f);
}

/* This realm's record, OWNED. */
static JSValue pf_record(JSContext *docctx)
{
    JSValue rec;

    DCHECK(g_rt != NULL, "a document's painted frame was reached before the component was declared, or after "
                         "painted_frame_free gave its slot back");
    rec = realm_value_get(docctx, g_slot);
    DCHECK(JS_IsObject(rec), "a realm has no painted-frame record — painted_frame_install is on realm.h's one "
                             "list, so this realm skipped it");
    return rec;
}

void painted_frame_publish(JSContext *docctx, DocumentPaintFrame *f)
{
    JSValue rec, obj;

    DCHECK(f != NULL && document_paint_finished(f), "step 22 published no frame, or one whose walk is still open "
                                                    "— a published frame is immutable");
    obj = JS_NewObjectClass(docctx, (int)g_class);
    CHECK(!JS_IsException(obj), "update the rendering step 22 could not allocate the object its frame is held by");
    JS_SetOpaque(obj, f);
    rec = pf_record(docctx);
    JS_SetProperty(docctx, rec, g_atom_frame, obj);
    JS_FreeValue(docctx, rec);
}

JSValue painted_frame_object(JSContext *docctx)
{
    JSValue rec = pf_record(docctx), v = JS_GetProperty(docctx, rec, g_atom_frame);

    JS_FreeValue(docctx, rec);
    return v;
}

const DocumentPaintFrame *painted_frame_of(JSValueConst v)
{
    DCHECK(JS_GetClassID(v) == g_class, "a painted frame was asked of an object that is not one");
    return JS_GetOpaque(v, g_class);
}

const DocumentPaintFrame *painted_frame_last(JSContext *docctx)
{
    JSValue v = painted_frame_object(docctx);
    const DocumentPaintFrame *f = NULL;

    if (JS_IsNull(v)) return NULL;
    DCHECK(JS_GetClassID(v) == g_class,
           "a painted-frame record holds something that is not a frame — only painted_frame_publish writes it");
    f = JS_GetOpaque(v, g_class);
    DCHECK(f != NULL, "a frame object on a document's painted-frame record carries no frame, so a world that "
                      "painted would read as one that never did");
    /* The record still holds the object, so the frame outlives this reference until the next publish. */
    JS_FreeValue(docctx, v);
    return f;
}

void painted_frame_init(JSContext *ctx)
{
    JSClassDef def = { "DocumentPaintFrame", .finalizer = pf_finalizer };

    DCHECK(g_rt == NULL, "painted_frame_init ran twice — one declaration per agent");
    g_rt = JS_GetRuntime(ctx);
    JS_NewClassID(g_rt, &g_class);
    CHECK(JS_NewClass(g_rt, g_class, &def) == 0, "the painted frame's class could not be declared");
    g_atom_frame = JS_NewAtom(ctx, "paintedFrame");
    CHECK(g_atom_frame != JS_ATOM_NULL, "the painted-frame key could not be interned");
    g_slot = realm_value_declare(ctx, "HTML §8.1.7.3 step 22: a document's last painted frame");
    agent_state_ptr("painted_frame", &g_rt, "the runtime the painted-frame realm-value slot was declared in");
    agent_state_class("painted_frame", &g_class, "the class a published frame is held by");
    agent_state_atom("painted_frame", &g_atom_frame, "the per-realm record's key");
    agent_state_realm_slot("painted_frame", &g_slot, "the per-realm slot a document's last painted frame lives in");
    realm_declare_intrinsic(painted_frame_install);
}

void painted_frame_install(JSContext *ctx)
{
    JSValue rec;

    DCHECK(g_rt != NULL, "a realm asked for its painted-frame record before the component was declared");
    /* Built with the realm, so the record is the pre-boot baseline's and a publish is captured by the running
       flow's COW delta. Null is "no rendering opportunity has painted this document". */
    rec = JS_NewObjectProto(ctx, JS_NULL);
    CHECK(!JS_IsException(rec), "a document's painted-frame record could not be allocated");
    JS_SetProperty(ctx, rec, g_atom_frame, JS_NULL);
    realm_value_set(ctx, g_slot, rec);
}

void painted_frame_free(JSRuntime *rt)
{
    DCHECK(g_rt != NULL, "the painted frame was released in an agent that never declared it");
    DCHECK(g_rt == rt, "the painted frame was released against a runtime other than the one it was declared in");
    /* The records and the frames they hold are the realms' — each goes with its context. */
    JS_FreeAtomRT(rt, g_atom_frame);
    g_atom_frame = JS_ATOM_NULL;
    g_slot = JS_INVALID_CLASS_ID;
    g_class = 0;
    g_rt = NULL;
}
