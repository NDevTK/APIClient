/* DOMException */
typedef struct JSDOMExceptionData {
    JSValue name;
    JSValue message;
    int code;
} JSDOMExceptionData;

typedef struct JSDOMExceptionNameDef {
    const char * const name;
    const char * const code_name;
} JSDOMExceptionNameDef;

static const JSDOMExceptionNameDef js_dom_exception_names_table[] = {
    { "IndexSizeError", "INDEX_SIZE_ERR" },
    { NULL, "DOMSTRING_SIZE_ERR" },
    { "HierarchyRequestError", "HIERARCHY_REQUEST_ERR" },
    { "WrongDocumentError", "WRONG_DOCUMENT_ERR" },
    { "InvalidCharacterError", "INVALID_CHARACTER_ERR" },
    { NULL, "NO_DATA_ALLOWED_ERR" },
    { "NoModificationAllowedError", "NO_MODIFICATION_ALLOWED_ERR" },
    { "NotFoundError", "NOT_FOUND_ERR" },
    { "NotSupportedError", "NOT_SUPPORTED_ERR" },
    { "InUseAttributeError", "INUSE_ATTRIBUTE_ERR" },
    { "InvalidStateError", "INVALID_STATE_ERR" },
    { "SyntaxError", "SYNTAX_ERR" },
    { "InvalidModificationError", "INVALID_MODIFICATION_ERR" },
    { "NamespaceError", "NAMESPACE_ERR" },
    { "InvalidAccessError", "INVALID_ACCESS_ERR" },
    { NULL, "VALIDATION_ERR" },
    { "TypeMismatchError", "TYPE_MISMATCH_ERR" },
    { "SecurityError", "SECURITY_ERR" },
    { "NetworkError", "NETWORK_ERR" },
    { "AbortError", "ABORT_ERR" },
    { "URLMismatchError", "URL_MISMATCH_ERR" },
    { "QuotaExceededError", "QUOTA_EXCEEDED_ERR" },
    { "TimeoutError", "TIMEOUT_ERR" },
    { "InvalidNodeTypeError", "INVALID_NODE_TYPE_ERR" },
    { "DataCloneError", "DATA_CLONE_ERR" }
};

static void js_domexception_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSDOMExceptionData *s = JS_GetOpaque(val, JS_CLASS_DOM_EXCEPTION);
    if (s) {
        JS_FreeValueRT(rt, s->name);
        JS_FreeValueRT(rt, s->message);
        js_free_rt(rt, s);
    }
}

static void js_domexception_mark(JSRuntime *rt, JSValueConst val,
                                 JS_MarkFunc *mark_func)
{
    JSDOMExceptionData *s = JS_GetOpaque(val, JS_CLASS_DOM_EXCEPTION);
    if (s) {
        JS_MarkValue(rt, s->name, mark_func);
        JS_MarkValue(rt, s->message, mark_func);
    }
}

/* Everything the DOMException constructor does ON the object OrdinaryCreateFromConstructor made. It OWNS obj —
   the shape the constructor was already written in. */
static JSValue js_domexception_ctor_body(JSContext *ctx, JSValueConst obj_,
                                         int argc, JSValueConst *argv,
                                         int backtrace_flags)
{
    JSDOMExceptionData *s;
    JSValue obj = (JSValue)obj_, message, name;

    /* WEB IDL §4.4 DOMException's constructor takes `optional DOMString message` and `optional DOMString
       name`, and §3.2.10 DOMString IS the ToString of each. OVER UNKNOWN EXTERNAL INPUT that conversion has
       no real JSString to produce, and the boundary underneath owes C one — so each argument answers as its
       own derived unknown and the exception is BUILT with it, which is the shape 20.5.1.1's `message` already
       takes one screen up (js_error_ctor_step). It is not decoration: a bundle that wraps a failed parse of a
       query parameter in a DOMException and then renders `e.message` has put unknown input in a sink, and
       keeping the source attached is what lets that be solved. The example is the OPERAND's own — ToString of
       a String is the identity, so the operation really did produce it. */
    if (!JS_IsUndefined(argv[0])) {
        message = js_concolic_derive(ctx, argv[0], "DOMException.message", js_concolic_operand_str(ctx, argv[0]));
        if (JS_IsUninitialized(message))
            message = JS_ToString(ctx, argv[0]);
    } else
        message = js_empty_string(ctx->rt);
    if (JS_IsException(message))
        goto fail1;
    if (!JS_IsUndefined(argv[1])) {
        name = js_concolic_derive(ctx, argv[1], "DOMException.name", js_concolic_operand_str(ctx, argv[1]));
        if (JS_IsUninitialized(name))
            name = JS_ToString(ctx, argv[1]);
    } else
        name = JS_AtomToString(ctx, JS_ATOM_Error);
    if (JS_IsException(name))
        goto fail2;
    s = js_malloc(ctx, sizeof(*s));
    if (!s)
        goto fail3;
    s->name = name;
    s->message = message;
    s->code = -1;
    JS_SetOpaqueInternal(obj, s);
    build_backtrace(ctx, obj, JS_UNDEFINED, NULL, 0, 0, backtrace_flags, NULL);
    return obj;
fail3:
    JS_FreeValue(ctx, name);
fail2:
    JS_FreeValue(ctx, message);
fail1:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* WebIDL's `[Exposed] constructor` requires new; that check precedes the `prototype` read. */
static int js_domexception_ctor_precheck(JSContext *ctx, const JSStepHdr *h)
{
    if (JS_IsUndefined(h->this_val)) {
        JS_ThrowTypeError(ctx, "constructor requires 'new'");
        return -1;
    }
    return 0;
}

/* The INTERNAL DOMException creations (js_new_dom_exception) pass no new.target, so the create runs nothing;
   a REAL new.target reaches the machine. */
static JSValue js_domexception_constructor0(JSContext *ctx, JSValueConst new_target,
                                            int argc, JSValueConst *argv,
                                            int backtrace_flags)
{
    JSValue obj;
    DCHECK(JS_IsUndefined(new_target),
           "the DOMException constructor reached its C entry with a real new.target — route that call site onto "
           "the STEPDEF_DOMEXCEPTION_CTOR machine");
    obj = js_create_from_intrinsic(ctx, JS_CLASS_DOM_EXCEPTION);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    return js_domexception_ctor_body(ctx, obj, argc, argv, backtrace_flags);
}

static JSValue js_domexception_getfield(JSContext *ctx, JSValueConst this_val,
                                        int magic)
{
    JSDOMExceptionData *s;
    JSValue *valp;

    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_DOM_EXCEPTION);
    if (!s)
        return JS_EXCEPTION;
    valp = (void *)((char *)s + magic);
    return js_dup(*valp);
}

static JSValue js_domexception_get_code(JSContext *ctx, JSValueConst this_val)
{
    JSDOMExceptionData *s;
    const char *name, *it;
    int i;
    size_t len;

    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_DOM_EXCEPTION);
    if (!s)
        return JS_EXCEPTION;
    /* WEB IDL §4.4 DOMException's `code` is the legacy number the NAME maps to, so an unknown name is an
       unknown code — derived from the name rather than looked up, because the table below would otherwise read
       an unknown through JS_ToCStringLen and abort at a boundary that owes C bytes. Nothing is cached: the
       answer belongs to the value, not to this object's `-1` sentinel, and a cached 0 would say "no legacy
       code" about a name that may well have one. */
    {
        JSValue c = js_concolic_derive(ctx, s->name, "DOMException.code", JS_UNDEFINED);
        if (!JS_IsUninitialized(c))
            return c;
    }
    if (s->code == -1) {
        name = JS_ToCStringLen(ctx, &len, s->name);
        if (!name)
            return JS_EXCEPTION;
        for (i = 0; i < countof(js_dom_exception_names_table); i++) {
            it = js_dom_exception_names_table[i].name;
            if (it && !strcmp(it, name) && len == strlen(it)) {
                s->code = i;
                break;
            }
        }
        s->code++;
        JS_FreeCString(ctx, name);
    }
    return js_int32(s->code);
}

/* DOMException IS A WEB IDL INTERFACE, so its prototype members carry WEB IDL's property attributes and not an
   ECMAScript builtin's. §3.7.6: an attribute is { enumerable: true, configurable: true }. It was
   configurable-only, which is what an ECMAScript accessor is — and the difference is not cosmetic: Web IDL's
   own §3.2.21 record conversion walks ENUMERABLE own properties, so `new URLSearchParams(DOMException.prototype)`
   read nothing and quietly built an empty object where every browser throws from the brand check on `name`. */
#define JS_IDL_ATTRIBUTE (JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE)
static const JSCFunctionListEntry js_domexception_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF2("name", js_domexception_getfield, NULL,
        offsetof(JSDOMExceptionData, name), JS_IDL_ATTRIBUTE ),
    JS_CGETSET_MAGIC_DEF2("message", js_domexception_getfield, NULL,
        offsetof(JSDOMExceptionData, message), JS_IDL_ATTRIBUTE ),
    JS_CGETSET_DEF2("code", js_domexception_get_code, NULL, JS_IDL_ATTRIBUTE ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "DOMException", JS_PROP_CONFIGURABLE ),
};

static const JSClassShortDef js_domexception_class_def[] = {
    { JS_ATOM_DOMException, js_domexception_finalizer, js_domexception_mark }, /* JS_CLASS_DOM_EXCEPTION */
};

/* The internal slots, for a HOST diagnostic. A stored value, never an operation — the accessor on the prototype
   would be a call, and a reporter runs from outside any flow. */
JSValue JS_GetDOMExceptionName(JSContext *ctx, JSValueConst val)
{
    JSDOMExceptionData *s = JS_GetOpaque(val, JS_CLASS_DOM_EXCEPTION);
    (void)ctx;
    return s ? js_dup(s->name) : JS_UNDEFINED;
}

JSValue JS_GetDOMExceptionMessage(JSContext *ctx, JSValueConst val)
{
    JSDOMExceptionData *s = JS_GetOpaque(val, JS_CLASS_DOM_EXCEPTION);
    (void)ctx;
    return s ? js_dup(s->message) : JS_UNDEFINED;
}

JSValue JS_PRINTF_FORMAT_ATTR(3, 4) JS_ThrowDOMException(JSContext *ctx, const char *name, JS_PRINTF_FORMAT const char *fmt, ...)
{
    JSValue obj, js_name, js_message;
    JSValueConst argv[2];
    va_list ap;
    char buf[256];

    DCHECK(JS_IsRegisteredClass(ctx->rt, JS_CLASS_DOM_EXCEPTION), "JS_IsRegisteredClass(ctx->rt, JS_CLASS_DOM_EXCEPTION)");
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    js_name = JS_NewString(ctx, name);
    if (JS_IsException(js_name))
        return JS_EXCEPTION;
    js_message = JS_NewString(ctx, buf);
    if (JS_IsException(js_message)) {
        JS_FreeValue(ctx, js_name);
        return JS_EXCEPTION;
    }
    argv[0] = js_message;
    argv[1] = js_name;
    obj = js_domexception_constructor0(ctx, JS_UNDEFINED, 2, argv, 0);
    JS_FreeValue(ctx, js_message);
    JS_FreeValue(ctx, js_name);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    return JS_Throw(ctx, obj);
}

/* PER CONTEXT, AND IDEMPOTENT — which it was not, and the difference was the whole realm.
   The CLASS registration is runtime-wide and was already guarded. The PROTOTYPE and the CONSTRUCTOR are this
   context's, and were not: a second call rebuilt them and assigned over
   ctx->class_proto[JS_CLASS_DOM_EXCEPTION], dropping the previous prototype's only reference. That prototype is
   reachable from the constructor, which is reachable from the global, so the leak took 333 GC objects with it —
   including the JSContext itself, which is why JS_FreeContext's teardown body then never ran at all and the
   final GC could not collect the cycle.
   Reaching a second call needed no unusual host: JS_NewContext calls JS_AddIntrinsicAToB, which installed this
   whenever the CLASS was unregistered, so `JS_NewContext` followed by an explicit JS_AddIntrinsicDOMException —
   exactly what a host that wants the interface writes, and what engine/host/main.c writes — leaked every time.
   Guarding on the prototype rather than the class is what makes the two questions the right ones: is the class
   registered in this RUNTIME, and does this CONTEXT already have its prototype. */
int JS_AddIntrinsicDOMException(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;
    int i;
    JSAtom name;
    JSValue ctor, proto;

    if (JS_IsObject(ctx->class_proto[JS_CLASS_DOM_EXCEPTION]))
        return 0;   /* this context already has it */
    if (!JS_IsRegisteredClass(rt, JS_CLASS_DOM_EXCEPTION)) {
        if (init_class_range(rt, js_domexception_class_def, JS_CLASS_DOM_EXCEPTION,
                             countof(js_domexception_class_def)))
            return -1;
    }
    proto = JS_NewObjectClass(ctx, JS_CLASS_ERROR);
    JS_SetPropertyFunctionList(ctx, proto,
                               js_domexception_proto_funcs,
                               countof(js_domexception_proto_funcs));
    ctor = JS_NewCFunction2(ctx, NULL, "DOMException", 2,
                            JS_CFUNC_step_ctor, STEPDEF_DOMEXCEPTION_CTOR);
    JS_SetConstructor(ctx, ctor, proto);
    for (i = 0; i < countof(js_dom_exception_names_table); i++) {
        name = JS_NewAtom(ctx, js_dom_exception_names_table[i].code_name);
        JS_DefinePropertyValue(ctx, proto, name, js_int32(i + 1),
                               JS_PROP_ENUMERABLE);
        JS_DefinePropertyValue(ctx, ctor, name, js_int32(i + 1),
                               JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, name);
    }
    JS_DefinePropertyValue(ctx, ctx->global_obj, JS_ATOM_DOMException, ctor,
                           JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    DCHECK(!JS_IsObject(ctx->class_proto[JS_CLASS_DOM_EXCEPTION]),
           "assigning over a class_proto slot that already holds a prototype — the old one's only reference is "
           "this slot, so the assignment leaks it and everything reachable from it");
    ctx->class_proto[JS_CLASS_DOM_EXCEPTION] = proto;
    return 0;
}
