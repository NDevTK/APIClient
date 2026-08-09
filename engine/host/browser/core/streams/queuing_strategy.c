/* CountQueuingStrategy and ByteLengthQueuingStrategy — the Streams Standard §7.
 *
 * They are two interfaces over one shape: a stored high-water mark, and a `size` function that says what a
 * chunk weighs. §4.5's queue is defined over those two numbers, so a stream built with `{ highWaterMark: 4 }`
 * and no strategy object and a stream built with `new CountQueuingStrategy({ highWaterMark: 4 })` must agree —
 * they do, because both end up in the same two fields of the controller.
 *
 * THE `size` GETTER ANSWERS ONE FUNCTION OBJECT, NOT A NEW ONE. §7.1 and §7.2 both say "this's relevant global
 * object's ... size function", created once and handed out unchanged, and the corpus checks the identity. It is
 * also NOT A METHOD: it ignores its receiver entirely, which is what lets `const { size } = strategy` work and
 * is why it is a plain function rather than something installed on a prototype.
 *
 * BYTE LENGTH'S SIZE IS A MACHINE. It answers `chunk.byteLength`, and that read is one accessor or Proxy trap
 * away from being the page's code — reading it from C is the drive-to-completion this engine aborts on. Count's
 * answers 1 and reads nothing, so it is an ordinary function; the difference is the spec's, not a shortcut. */
#include "check.h"
#include "quickjs.h"
#include "quickjs-step.h"
#include "core/idl_args.h"
#include "core/realm.h"
#include "core/streams/queuing_strategy.h"

enum { QS_COUNT = 0, QS_BYTE_LENGTH, QS_N };

/* §7's `dictionary QueuingStrategyInit { required unrestricted double highWaterMark; }`. Required means an
   absent member is a TypeError, which is why `new CountQueuingStrategy({})` throws rather than defaulting. */
static const IdlDictMember QS_INIT[] = {
    { "highWaterMark", IDL_UNRESTRICTED_DOUBLE, true },
};

static JSClassID g_qs_class;
/* §7's TWO PROTOTYPES AND TWO SIZE FUNCTIONS ARE EACH REALM'S. Both interfaces' instances wear ONE class, so
   the prototypes cannot live in the class slot and take a per-realm VALUE slot each instead — the same store,
   named for what it holds. The size functions are function objects, which carry the realm they were minted
   in: `new CountQueuingStrategy({highWaterMark:1}).size` handed every document the first realm's. */
static int       g_qs_proto_slot[QS_N] = { -1, -1 };
static int       g_size_fn_slot[QS_N] = { -1, -1 };
static int       g_qs_ctor_stepid[QS_N] = { -1, -1 };
static int       g_byte_size_stepid = -1;
static JSRuntime *g_qs_rt;

/* The mark is the whole of an instance's state. It is a `double` rather than a JSValue because §7 stores the
   converted IDL value, and the getter hands back exactly what was stored — NaN and the infinities included,
   since `unrestricted double` admits them and §7 does not check. */
typedef struct { double hwm; int kind; } QueuingStrategyData;

static void qs_finalizer(JSRuntime *rt, JSValue val)
{
    QueuingStrategyData *q = JS_GetOpaque(val, g_qs_class);
    if (q) js_free_rt(rt, q);
}

static JSValue js_qs_hwm(JSContext *ctx, JSValueConst this_val, int magic)
{
    QueuingStrategyData *q = JS_GetOpaque(this_val, g_qs_class);
    (void)magic;
    if (!q) return JS_ThrowTypeError(ctx, "not a queuing strategy");
    return JS_NewFloat64(ctx, q->hwm);
}

static JSValue js_qs_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    QueuingStrategyData *q = JS_GetOpaque(this_val, g_qs_class);
    (void)magic;
    if (!q) return JS_ThrowTypeError(ctx, "not a queuing strategy");
    DCHECK(q->kind == QS_COUNT || q->kind == QS_BYTE_LENGTH, "a queuing strategy carries no kind");
    return realm_value_get(ctx, g_size_fn_slot[q->kind]);   /* OWNED — this realm's */
}

/* §7.2's size function: one, whatever the chunk is. */
static JSValue js_count_size(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NewInt32(ctx, 1);
}

/* §7.1's size function: `chunk.byteLength`, as a REQUEST — the read is the page's code the moment the chunk
   is anything but a plain typed array. */
typedef struct {
    JSStepHdr hdr;
    JSValue   value;
} JSByteSizeState;

static void js_byte_size_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSByteSizeState *s = st;
    v->val(ctx, &s->value);
}

static JSValue js_byte_size_fini(JSContext *ctx, void *st, bool take_result)
{
    JSByteSizeState *s = st;
    JSValue r = take_result ? s->value : JS_UNDEFINED;
    if (take_result) s->value = JS_UNDEFINED;
    JS_FreeValue(ctx, s->value);
    s->value = JS_UNDEFINED;
    return r;
}

static int js_byte_size_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSByteSizeState *s = st;
    JSAtom atom = JS_NewAtom(ctx, "byteLength");
    int r = step_getprop_run(ctx, &s->hdr, s->hdr.argc > 0 ? s->hdr.argv[0] : JS_UNDEFINED, atom,
                             cb_result, &s->value, out_cb, out_argc);
    JS_FreeAtom(ctx, atom);
    if (r > 0) return r;
    return r < 0 ? JS_STEP_ABRUPT : JS_STEP_DONE;
}

static const JSTrampStepDef js_byte_size_def = {
    sizeof(JSByteSizeState), js_byte_size_step, js_byte_size_fini, 0, .visit = js_byte_size_visit
};

/* §7's constructor, one body for both interfaces: the `magic` says which, exactly as the two prototypes and
   the two size functions are one mechanism with two rows. */
static JSValue js_qs_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    QueuingStrategyData *q;
    JSValue obj, hv;
    double h = 0;

    if (JS_IsUndefined(this_val))
        return JS_ThrowTypeError(ctx, "a queuing strategy constructor requires 'new'");
    hv = idl_dict_get(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, "highWaterMark");
    if (JS_ToFloat64(ctx, &h, hv) < 0) { JS_FreeValue(ctx, hv); return JS_EXCEPTION; }
    JS_FreeValue(ctx, hv);
    {
        JSValue proto = realm_value_get(ctx, g_qs_proto_slot[magic]);
        obj = JS_NewObjectProtoClass(ctx, proto, g_qs_class);
        JS_FreeValue(ctx, proto);
    }
    if (JS_IsException(obj)) return obj;
    q = js_mallocz(ctx, sizeof *q);
    if (!q) { JS_FreeValue(ctx, obj); return JS_EXCEPTION; }
    q->hwm = h;
    q->kind = magic;
    JS_SetOpaque(obj, q);
    return obj;
}

void queuing_strategy_init(JSContext *ctx)
{
    JSClassDef cd = { "QueuingStrategy", .finalizer = qs_finalizer };
    JSRuntime *rt = JS_GetRuntime(ctx);
    static const IdlArgType ONE_DICT[1] = { IDL_DICT };
    static const char *const NAMES[QS_N] = { "CountQueuingStrategy", "ByteLengthQueuingStrategy" };
    int i;

    DCHECK(g_qs_rt == NULL || g_qs_rt == rt, "the queuing strategies were installed into a second runtime");
    if (g_qs_rt == rt) return;
    g_qs_rt = rt;
    JS_NewClassID(rt, &g_qs_class);
    JS_NewClass(rt, g_qs_class, &cd);

    g_byte_size_stepid = JS_RegisterStepDef(rt, &js_byte_size_def);
    CHECK(g_byte_size_stepid >= 0, "queuing strategies: no step id for the byte-length size function");
    for (i = 0; i < QS_N; i++) {
        char what[64];
        g_qs_ctor_stepid[i] = idl_method_id_dict(ctx, ONE_DICT, 1, QS_INIT,
                                                 (int)(sizeof QS_INIT / sizeof *QS_INIT), js_qs_ctor, i);
        snprintf(what, sizeof what, "%s.prototype", NAMES[i]);
        g_qs_proto_slot[i] = realm_value_declare(ctx, what);
        snprintf(what, sizeof what, "%s size", NAMES[i]);
        g_size_fn_slot[i] = realm_value_declare(ctx, what);
    }
    realm_declare_intrinsic(queuing_strategy_install_protos);
}

/* §7's TWO INTERFACE PROTOTYPE OBJECTS AND THEIR SIZE FUNCTIONS, FOR ONE REALM. */
void queuing_strategy_install_protos(JSContext *ctx)
{
    static const char *const NAMES[QS_N] = { "CountQueuingStrategy", "ByteLengthQueuingStrategy" };
    JSValue size[QS_N];
    int i;

    DCHECK(g_qs_rt != NULL, "a realm asked for a queuing strategy prototype before the class was declared");
    size[QS_COUNT] = JS_NewCFunction2(ctx, js_count_size, "size", 0, JS_CFUNC_generic, 0);
    size[QS_BYTE_LENGTH] = JS_NewCFunction2(ctx, NULL, "size", 1, JS_CFUNC_step, g_byte_size_stepid);
    CHECK(!JS_IsException(size[QS_COUNT]) && !JS_IsException(size[QS_BYTE_LENGTH]),
          "a queuing strategy's size function could not be allocated");
    for (i = 0; i < QS_N; i++) {
        JSValue proto = JS_NewObject(ctx);
        CHECK(!JS_IsException(proto), "a queuing strategy prototype could not be allocated");
        idl_interface_tag(ctx, proto, NAMES[i]);
        idl_install_accessor(ctx, proto, "highWaterMark", js_qs_hwm, 0, -1);
        idl_install_accessor(ctx, proto, "size", js_qs_size, 0, -1);
        realm_value_set(ctx, g_qs_proto_slot[i], proto);
        realm_value_set(ctx, g_size_fn_slot[i], size[i]);
    }
}

void queuing_strategy_install(JSContext *ctx, JSValueConst global)
{
    static const char *const NAMES[QS_N] = { "CountQueuingStrategy", "ByteLengthQueuingStrategy" };
    int i;

    for (i = 0; i < QS_N; i++) {
        JSValue ctor;
        DCHECK(g_qs_ctor_stepid[i] >= 0, "a queuing strategy was installed before it was declared");
        ctor = idl_step_constructor(ctx, NAMES[i], 1, g_qs_ctor_stepid[i]);
        CHECK(!JS_IsException(ctor), "a queuing strategy interface object could not be allocated");
        {
            JSValue proto = realm_value_get(ctx, g_qs_proto_slot[i]);
            JS_SetConstructor(ctx, ctor, proto);
            JS_FreeValue(ctx, proto);
        }
        JS_SetPropertyStr(ctx, (JSValue)global, NAMES[i], ctor);
    }
}

void queuing_strategy_free(JSContext *ctx)
{
    int i;
    if (!g_qs_rt) return;
    for (i = 0; i < QS_N; i++) {
        /* the prototypes and size functions are the REALMS' — released with their contexts */
        g_qs_ctor_stepid[i] = -1;
    }
    g_byte_size_stepid = -1;
    g_qs_rt = NULL;
}
