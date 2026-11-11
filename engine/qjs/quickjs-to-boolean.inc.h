void JS_SetIsHTMLDDA(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return;
    p = JS_VALUE_GET_OBJ(obj);
    p->is_HTMLDDA = true;
}

static inline bool JS_IsHTMLDDA(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return false;
    p = JS_VALUE_GET_OBJ(obj);
    return p->is_HTMLDDA;
}

static int JS_ToBoolFree(JSContext *ctx, JSValue val)
{
    uint32_t tag = JS_VALUE_GET_TAG(val);
    switch(tag) {
    case JS_TAG_INT:
        return JS_VALUE_GET_INT(val) != 0;
    case JS_TAG_BOOL:
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
        return JS_VALUE_GET_INT(val);
    case JS_TAG_EXCEPTION:
        return -1;
    case JS_TAG_STRING:
        {
            bool ret = JS_VALUE_GET_STRING(val)->len != 0;
            JS_FreeValue(ctx, val);
            return ret;
        }
    case JS_TAG_STRING_ROPE:
        {
            bool ret = JS_VALUE_GET_STRING_ROPE(val)->len != 0;
            JS_FreeValue(ctx, val);
            return ret;
        }
    case JS_TAG_SHORT_BIG_INT:
        return JS_VALUE_GET_SHORT_BIG_INT(val) != 0;
    case JS_TAG_BIG_INT:
        {
            JSBigInt *p = JS_VALUE_GET_PTR(val);
            bool ret;
            int i;

            /* fail safe: we assume it is not necessarily
                normalized. Beginning from the MSB ensures that the
                test is fast. */
            ret = false;
            for(i = p->len - 1; i >= 0; i--) {
                if (p->tab[i] != 0) {
                    ret = true;
                    break;
                }
            }
            JS_FreeValue(ctx, val);
            return ret;
        }
    case JS_TAG_OBJECT:
        {
            JSObject *p = JS_VALUE_GET_OBJ(val);
            bool ret = !p->is_HTMLDDA;
            JS_FreeValue(ctx, val);
            return ret;
        }
        break;
    default:
        if (JS_TAG_IS_FLOAT64(tag)) {
            double d = JS_VALUE_GET_FLOAT64(val);
            return !isnan(d) && d != 0;
        } else {
            JS_FreeValue(ctx, val);
            return true;
        }
    }
}

int JS_ToBool(JSContext *ctx, JSValueConst val)
{
    return JS_ToBoolFree(ctx, js_dup(val));
}

