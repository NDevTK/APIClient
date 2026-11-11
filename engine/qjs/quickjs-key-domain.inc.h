/* How js_key_domain_walk ended (its definition states each). */
enum { JS_CHAIN_KEYS_ALL = 1, JS_CHAIN_KEYS_STOPPED = 0, JS_CHAIN_KEYS_COMPOSED = -1, JS_CHAIN_KEYS_STEP = 2 };

/* Whether a String key `p` holds in its own storage (shape and element storage) lies in the domain of the unknown
   `key` (JSConcolicHooks.key_domain_holds). Symbols are passed over: no String key equals one. Each key visited
   adds one to `*visited`. */
static bool js_storage_key_in_domain(JSContext *ctx, JSObject *p, JSValueConst key, uint64_t *visited)
{
    JSRuntime *rt = ctx->rt;
    JSShapeProperty *prs;
    uint32_t i;
    char ibuf[16];

    for (i = 0, prs = get_shape_prop(p->shape); i < (uint32_t)p->shape->prop_count; i++, prs++) {
        JSAtom a = prs->atom;
        size_t n;
        const char *s;
        bool in;

        if (a == JS_ATOM_NULL) continue;
        if (!__JS_AtomIsTaggedInt(a) && rt->atom_array[a]->atom_type != JS_ATOM_TYPE_STRING) continue;
        (*visited)++;
        s = JS_AtomToCStringLen(ctx, &n, a);
        CHECK(s != NULL, "quickjs: OOM spelling a chain key against an unknown key's domain");
        in = g_concolic.key_domain_holds(ctx, key, s, n);
        JS_FreeCString(ctx, s);
        if (in) return true;
    }
    if (p->fast_array)
        for (i = 0; i < p->u.array.count; i++) {
            int n = snprintf(ibuf, sizeof ibuf, "%u", i);

            (*visited)++;
            if (g_concolic.key_domain_holds(ctx, key, ibuf, (size_t)n)) return true;
        }
    return false;
}

/* ECMAScript §10.1.8.1 OrdinaryGet ( O, P, Receiver ) for an unknown String key P, asked of P's domain rather
   than of one key: each link's [[GetOwnProperty]] and then its [[GetPrototypeOf]], from `*plink` on. A link
   whose both methods are ordinary answers from its storage; a class composing its keys answers through
   JSClassExoticMethods.key_domain or key_domain_step. Answers JS_CHAIN_KEYS_ALL when no link answers a key in
   the domain (the read misses in every world), JS_CHAIN_KEYS_STOPPED when one does, JS_CHAIN_KEYS_COMPOSED at a
   link whose keys no domain answer states (a Proxy, any other composing class), and JS_CHAIN_KEYS_STEP with
   `*plink` the link whose step answers next, its storage already read. Runs no page code; every storage key
   visited is retired as one unit of the running flow's work.
   Named residual. Not covered: a class composing its keys that declares neither hook (a form's named controls,
   Storage, a String object, a DOMStringMap). Next diff builds: key_domain_step over the form's named walk and
   key_domain over Storage's keys. Absence shows as an expando-shaped read on a form or on localStorage keeping
   its hit arm while the same read on a div does not. */
static int js_key_domain_walk(JSContext *ctx, JSObject **plink, JSValueConst key)
{
    JSRuntime *rt = ctx->rt;
    JSObject *p;
    uint64_t visited = 0;
    int r = JS_CHAIN_KEYS_ALL;

    DCHECK(g_concolic.key_domain_stated(ctx, key),
           "an unknown key's domain walk ran over a key whose facets state no domain");
    for (p = *plink; p != NULL; p = js_walk_proto(ctx, p)) {
        const JSClassExoticMethods *em = rt->class_array[p->class_id].exotic;
        JSValueConst stand = JS_UNINITIALIZED;
        int h;

        if (p->class_id == JS_CLASS_PROXY) { r = JS_CHAIN_KEYS_COMPOSED; break; }
        if (js_storage_key_in_domain(ctx, p, key, &visited)) { r = JS_CHAIN_KEYS_STOPPED; break; }
        if (!em || !(em->get_own_property || em->get_own_property_step || em->get_own_property_names ||
                     em->get_property || em->has_property || em->get_prototype))
            continue;
        if (em->get_property || em->has_property || (em->get_prototype && !em->get_prototype_walkable)) {
            r = JS_CHAIN_KEYS_COMPOSED;
            break;
        }
        DCHECK(!(em->key_domain && em->key_domain_step),
               "a class answers an unknown key's domain both in place and by a step");
        if (em->key_domain_step) { *plink = p; r = JS_CHAIN_KEYS_STEP; break; }
        if (!em->key_domain) { r = JS_CHAIN_KEYS_COMPOSED; break; }
        h = em->key_domain(ctx, JS_MKPTR(JS_TAG_OBJECT, p), key, &stand);
        if (h < 0) { r = JS_CHAIN_KEYS_COMPOSED; break; }
        if (h > 0) { r = JS_CHAIN_KEYS_STOPPED; break; }
        if (!JS_IsUninitialized(stand)) {
            JSObject *sp = JS_VALUE_GET_OBJ(stand);

            DCHECK(JS_VALUE_GET_TAG(stand) == JS_TAG_OBJECT && sp->class_id != JS_CLASS_PROXY,
                   "a class's domain answer stands for something that is not an object read from storage");
            if (js_storage_key_in_domain(ctx, sp, key, &visited)) { r = JS_CHAIN_KEYS_STOPPED; break; }
        }
    }
    JS_FlowRetireWork(visited);
    return r;
}

