/* Symbol */

/* 20.4.1.1 step 1: `new Symbol()` is a TypeError BEFORE the description is coerced. For a constructor step the
   header's this_val IS new_target, which is the same operand the C body took. */
static int js_symbol_ctor_precheck(JSContext *ctx, const JSStepHdr *h)
{
    if (!JS_IsUndefined(h->this_val)) {
        JS_ThrowTypeErrorNotAConstructor(ctx, h->this_val);
        return -1;
    }
    return 0;
}

static JSValue js_symbol_constructor(JSContext *ctx, JSValueConst new_target,
                                     int argc, JSValueConst *argv)
{
    JSValue str;
    JSString *p;

    /* the NewTarget test is the declaration's precheck and the ToString its coercion, so `argv[0]` is already a
       primitive here and nothing below invokes the page's code. */
    DCHECK(JS_IsUndefined(new_target), "the Symbol constructor body ran with a NewTarget the precheck rejects");
    if (argc == 0 || JS_IsUndefined(argv[0])) {
        p = NULL;
    } else {
        /* §20.4.1.1 Symbol ( [ description ] ) step 3 OVER UNKNOWN EXTERNAL INPUT. The declaration's coercion
           is a ToPrimitive, which over a concolic is the identity, so unknown input arrives here uncoerced —
           and step 4 wants a new Symbol whose [[Description]] IS that unknown String, which
           JS_NewSymbolInternal cannot build (it takes a JSString) and the conversion boundary below cannot
           produce. The operator answers with the derived unknown instead: what a page does with a Symbol is
           use it as a property key, and an unknown key already denotes its own shape through the key_name
           hook, so the write and the later read still land in the same slot. */
        JSValue c = js_concolic_derive(ctx, argv[0], "Symbol", JS_UNDEFINED);
        if (!JS_IsUninitialized(c))
            return c;
        str = JS_ToString(ctx, argv[0]);
        if (JS_IsException(str))
            return JS_EXCEPTION;
        p = JS_VALUE_GET_STRING(str);
    }
    return JS_NewSymbolInternal(ctx, p, JS_ATOM_TYPE_SYMBOL);
}

/* §20.4.3.4.1 ThisSymbolValue ( arg ). Over an unknown carrier the Symbol is unknown too. */
static JSValue js_thisSymbolValue(JSContext *ctx, JSValueConst this_val)
{
    if (js_value_is_concolic(this_val))
        return js_slot_check_unknown(ctx, this_val, JS_SLOT_SYMBOL_DATA);
    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_SYMBOL)
        return js_dup(this_val);

    if (JS_VALUE_GET_TAG(this_val) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(this_val);
        if (p->class_id == JS_CLASS_SYMBOL) {
            if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_SYMBOL)
                return js_dup(p->u.object_data);
        }
    }
    return JS_ThrowTypeError(ctx, "not a symbol");
}

/* An unknown Symbol's string: derived from it, with the example's string as its example. */
static JSValue js_symbol_derive_string(JSContext *ctx, JSValue sym, const char *op, bool description)
{
    JSValue ex = js_concolic_operand(ctx, sym), real = JS_UNDEFINED, r;

    if (js_concolic_held(sym) && JS_IsSymbol(ex)) {
        JSAtomStruct *p = JS_VALUE_GET_PTR(ex);

        real = JS_AtomToString(ctx, js_get_atom_index(ctx->rt, p));
        if (description && p->len == 0 && p->is_wide_char != 0) {
            JS_FreeValue(ctx, real);
            real = JS_UNDEFINED;
        } else if (!description) {
            real = JS_ConcatString3(ctx, "Symbol(", real, ")");
        }
    }
    JS_FreeValue(ctx, ex);
    r = js_concolic_derive(ctx, sym, op, real);
    DCHECK(js_value_is_concolic(r), "a Symbol's string derived from an unknown Symbol is not unknown");
    g_concolic.type_fix(ctx, r, description ? JS_LANG_TYPE_STRING | JS_LANG_TYPE_UNDEFINED : JS_LANG_TYPE_STRING);
    JS_FreeValue(ctx, sym);
    return r;
}

/* §20.4.3.3.1 SymbolDescriptiveString ( symbol ), over the Symbol `val` (taken), which may be unknown. */
static JSValue js_symbol_descriptive_string(JSContext *ctx, JSValue val)
{
    JSValue ret;

    if (js_value_is_concolic(val))
        return js_symbol_derive_string(ctx, val, "20.4.3.3 Symbol.prototype.toString", false);
    /* String(sym)'s description branch, inlined: the argument is a SYMBOL, so there is no coercion and nothing
       to suspend — the String constructor is a step machine now and calling it from here would be an unrouted
       call shape for no benefit. */
    { JSAtomStruct *p = JS_VALUE_GET_PTR(val);
      ret = JS_ConcatString3(ctx, "Symbol(", JS_AtomToString(ctx, js_get_atom_index(ctx->rt, p)), ")"); }
    JS_FreeValue(ctx, val);
    return ret;
}

static JSValue js_symbol_get_description(JSContext *ctx, JSValueConst this_val)
{
    JSValue val, ret;
    JSAtomStruct *p;

    val = js_thisSymbolValue(ctx, this_val);
    if (JS_IsException(val))
        return val;
    if (js_value_is_concolic(val))
        return js_symbol_derive_string(ctx, val, "20.4.3.2 get Symbol.prototype.description", true);
    p = JS_VALUE_GET_PTR(val);
    if (p->len == 0 && p->is_wide_char != 0) {
        ret = JS_UNDEFINED;
    } else {
        ret = JS_AtomToString(ctx, js_get_atom_index(ctx->rt, p));
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static const JSCFunctionListEntry js_symbol_proto_funcs[] = {
    JS_CFUNC_STEP_DEF("toString", 0, STEPDEF_SYMBOL_TOSTRING ),
    JS_CFUNC_STEP_DEF("valueOf", 0, STEPDEF_SYMBOL_VALUEOF ),
    // XXX: should have writable: false
    JS_CFUNC_STEP_DEF("[Symbol.toPrimitive]", 1, STEPDEF_SYMBOL_TOPRIM ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Symbol", JS_PROP_CONFIGURABLE ),
    JS_CGETSET_DEF("description", js_symbol_get_description, NULL ),
};

/* §20.4.2.4 "Symbol.for ( key )" as a STEP MACHINE. Its step 1 is `? ToString(key)`, which for an OBJECT argument is the
   page's @@toPrimitive/valueOf/toString — JS_ToString ran it from C, so `Symbol.for({toString(){ while(x){} }})`
   had no flow base. The registry lookup that follows invokes nothing. */
typedef struct JSSymbolFor {
    JSStepHdr hdr;      /* MUST be first */
    JSValue result;     /* owned: the symbol */
} JSSymbolFor;
_Static_assert(offsetof(JSSymbolFor, hdr) == 0, "JSStepHdr must be first in JSSymbolFor");

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). */
#define SYMFOR_STAGES(X) \
    X(SYMFOR_ENTRY,    "§20.4.2.4, entered: the machine has its key and nothing has been coerced yet") \
    X(SYMFOR_TOSTRING, "§20.4.2.4 step 1 (stringKey is ToString(key)) - and steps 2-8, the GlobalSymbolRegistry " \
                       "lookup and the new Symbol it records")
enum { SYMFOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_symbol_for_steps[] = { SYMFOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_symbol_for_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSSymbolFor *m = st;
    JSValue str;
    int r;

    if (m->hdr.stage == SYMFOR_ENTRY) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        m->result = JS_UNDEFINED;
        m->hdr.stage = SYMFOR_TOSTRING;
    }
    DCHECK(m->hdr.stage == SYMFOR_TOSTRING, "Symbol.for resumed in an unknown stage");
    r = step_tostring_run(ctx, &m->hdr, step_arg(&m->hdr, 0), cb_result, &str, out_cb, out_argc);
    if (r) return r;
    /* __JS_NewAtom CONSUMES the string, which is why there is no free here — the C body relied on that too, and
       adding one double-freed every Symbol.for. And the coercion can answer with a ROPE, which has no JSString
       to hand it: JS_ToStringFree flattens, and on an already-flat string it is the identity. */
    if (JS_VALUE_GET_TAG(str) == JS_TAG_STRING_ROPE) {
        str = JS_ToStringFree(ctx, str);
        if (JS_IsException(str)) return -1;
    }
    DCHECK(JS_VALUE_GET_TAG(str) == JS_TAG_STRING, "ToString answered with something that is not a string");
    /* §20.4.2.4 reads the surrounding agent's [[GlobalSymbolRegistry]], and the runtime's atom table is the one
       registry its agents share: unobservable, because no Symbol crosses agents (HTML's StructuredSerializeInternal
       throws a DataCloneError for one). */
    m->result = JS_NewSymbolInternal(ctx, JS_VALUE_GET_STRING(str), JS_ATOM_TYPE_GLOBAL_SYMBOL);
    return JS_IsException(m->result) ? (m->result = JS_UNDEFINED, -1) : 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The symbol; the key's ToString is the header's coercion. */
static void js_symbol_for_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSSymbolFor *m = st;
    v->val(ctx, &m->result);
}

static JSValue js_symbol_for_fini(JSContext *ctx, void *st, bool take_result)
{
    JSSymbolFor *m = st;
    JSValue r = take_result ? m->result : JS_UNDEFINED;
    if (take_result) m->result = JS_UNDEFINED;
    return r;
}

static const JSTrampStepDef js_symbol_for_def = {
    sizeof(JSSymbolFor), js_symbol_for_step, js_symbol_for_fini, 0, .visit = js_symbol_for_visit,
    .algorithm = "§20.4.2.4 Symbol.for", .steps = js_symbol_for_steps };

static JSValue js_symbol_keyFor(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    JSAtomStruct *p;

    if (!JS_IsSymbol(argv[0]))
        return JS_ThrowTypeError(ctx, "not a symbol");
    p = JS_VALUE_GET_PTR(argv[0]);
    if (p->atom_type != JS_ATOM_TYPE_GLOBAL_SYMBOL)
        return JS_UNDEFINED;
    return js_dup(JS_MKPTR(JS_TAG_STRING, p));
}

static const JSCFunctionListEntry js_symbol_funcs[] = {
    JS_CFUNC_STEP_DEF("for", 1, STEPDEF_SYMBOL_FOR ),
    JS_CFUNC_DEF("keyFor", 1, js_symbol_keyFor ),
    JS_PROP_SYMBOL_DEF("toPrimitive", JS_ATOM_Symbol_toPrimitive, 0),
    JS_PROP_SYMBOL_DEF("iterator", JS_ATOM_Symbol_iterator, 0),
    JS_PROP_SYMBOL_DEF("match", JS_ATOM_Symbol_match, 0),
    JS_PROP_SYMBOL_DEF("matchAll", JS_ATOM_Symbol_matchAll, 0),
    JS_PROP_SYMBOL_DEF("replace", JS_ATOM_Symbol_replace, 0),
    JS_PROP_SYMBOL_DEF("search", JS_ATOM_Symbol_search, 0),
    JS_PROP_SYMBOL_DEF("split", JS_ATOM_Symbol_split, 0),
    JS_PROP_SYMBOL_DEF("toStringTag", JS_ATOM_Symbol_toStringTag, 0),
    JS_PROP_SYMBOL_DEF("isConcatSpreadable", JS_ATOM_Symbol_isConcatSpreadable, 0),
    JS_PROP_SYMBOL_DEF("hasInstance", JS_ATOM_Symbol_hasInstance, 0),
    JS_PROP_SYMBOL_DEF("species", JS_ATOM_Symbol_species, 0),
    JS_PROP_SYMBOL_DEF("unscopables", JS_ATOM_Symbol_unscopables, 0),
    JS_PROP_SYMBOL_DEF("asyncIterator", JS_ATOM_Symbol_asyncIterator, 0),
    JS_PROP_SYMBOL_DEF("dispose", JS_ATOM_Symbol_dispose, 0),
    JS_PROP_SYMBOL_DEF("asyncDispose", JS_ATOM_Symbol_asyncDispose, 0),
};

