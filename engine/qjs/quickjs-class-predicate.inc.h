bool JS_IsFunction(JSContext *ctx, JSValueConst val)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(val) != JS_TAG_OBJECT)
        return false;
    p = JS_VALUE_GET_OBJ(val);
    switch(p->class_id) {
    case JS_CLASS_BYTECODE_FUNCTION:
        return true;
    case JS_CLASS_PROXY:
        return p->u.proxy_data->is_func;
    default:
        return (ctx->rt->class_array[p->class_id].call != NULL);
    }
}

bool JS_IsAsyncFunction(JSValueConst val)
{
    return JS_CLASS_ASYNC_FUNCTION == JS_GetClassID(val);
}

static bool JS_IsCFunction(JSContext *ctx, JSValueConst val, JSCFunction *func,
                           int magic)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(val) != JS_TAG_OBJECT)
        return false;
    p = JS_VALUE_GET_OBJ(val);
    if (p->class_id == JS_CLASS_C_FUNCTION)
        return (p->u.cfunc.c_function.generic == func && p->u.cfunc.magic == magic);
    else
        return false;
}

bool JS_IsConstructor(JSContext *ctx, JSValueConst val)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(val) != JS_TAG_OBJECT)
        return false;
    p = JS_VALUE_GET_OBJ(val);
    return p->is_constructor;
}

bool JS_SetConstructorBit(JSContext *ctx, JSValueConst func_obj, bool val)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(func_obj) != JS_TAG_OBJECT)
        return false;
    p = JS_VALUE_GET_OBJ(func_obj);
    p->is_constructor = val;
    return true;
}

bool JS_IsRegExp(JSValueConst val)
{
    return JS_CLASS_REGEXP == JS_GetClassID(val);
}

bool JS_IsMap(JSValueConst val)
{
    return JS_CLASS_MAP == JS_GetClassID(val);
}

bool JS_IsSet(JSValueConst val)
{
    return JS_CLASS_SET == JS_GetClassID(val);
}

bool JS_IsWeakRef(JSValueConst val)
{
    return JS_CLASS_WEAK_REF == JS_GetClassID(val);
}

bool JS_IsWeakSet(JSValueConst val)
{
    return JS_CLASS_WEAKSET == JS_GetClassID(val);
}

bool JS_IsWeakMap(JSValueConst val)
{
    return JS_CLASS_WEAKMAP == JS_GetClassID(val);
}

bool JS_IsDataView(JSValueConst val)
{
    return JS_CLASS_DATAVIEW == JS_GetClassID(val);
}

bool JS_IsError(JSValueConst val)
{
    return JS_CLASS_ERROR == JS_GetClassID(val);
}

