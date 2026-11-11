int JS_SetOpaque(JSValueConst obj, void *opaque)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        p = JS_VALUE_GET_OBJ(obj);
        // User code can't set the opaque of internal objects.
        if (p->class_id >= JS_CLASS_INIT_COUNT) {
            p->u.opaque = opaque;
            return 0;
        }
    }

    return -1;
}

/* |obj| must be a JSObject of an internal class. */
static void JS_SetOpaqueInternal(JSValueConst obj, void *opaque)
{
    JSObject *p;
    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT, "JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT");
    p = JS_VALUE_GET_OBJ(obj);
    DCHECK(p->class_id < JS_CLASS_INIT_COUNT, "p->class_id < JS_CLASS_INIT_COUNT");
    p->u.opaque = opaque;
}

/* return NULL if not an object of class class_id */
void *JS_GetOpaque(JSValueConst obj, JSClassID class_id)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return NULL;
    p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id != class_id)
        return NULL;
    return p->u.opaque;
}

void *JS_GetOpaque2(JSContext *ctx, JSValueConst obj, JSClassID class_id)
{
    void *p = JS_GetOpaque(obj, class_id);
    if (unlikely(!p)) {
        JS_ThrowTypeErrorInvalidClass(ctx, class_id);
    }
    return p;
}

void *JS_GetAnyOpaque(JSValueConst obj, JSClassID *class_id)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
        *class_id = 0;
        return NULL;
    }
    p = JS_VALUE_GET_OBJ(obj);
    *class_id = p->class_id;
    return p->u.opaque;
}

