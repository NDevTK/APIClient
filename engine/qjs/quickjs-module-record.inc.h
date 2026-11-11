/* 'name' is freed */
/* THE RECORD, WITHOUT ITS MODULE-MAP ENTRY. 16.2.1.7.1 ParseModule produces a Source Text Module Record and
   16.2.1.10 HostLoadImportedModule is what puts one in a [[LoadedModules]] list; they are two steps, and this
   engine performed them as one for as long as a module parse could not be interrupted.
   WHY THEY ARE TWO ENTRIES NOW. `ctx->loaded_modules` IS the module map and js_find_loaded_module is the memo
   every load consults — two sites say so in their own words ("the module map is the load's memo"). A record
   published there is therefore reachable by ANY flow of this realm, and a record whose parse has not finished
   has `func_obj` UNDEFINED and PARTIALLY FILLED request tables. While a module parse ran inside one call that
   window was zero scheduler steps wide and nothing could look; a parse that hands the thread back makes it
   unbounded, so a sibling flow's `import()` of the same specifier would find a half-built record, walk its
   truncated request table, and reach js_create_module_bytecode_function — whose first act is
   `b = JS_VALUE_GET_PTR(m->func_obj)` followed by a refcount increment through it. That is a NULL dereference
   in RELEASE, on a name the page chose, and the DCHECK that would have named it sits one phase LATER in
   js_module_link_advance's prologue arm, so the crash would land in the wrong file naming memory.
   IT IS §Fix-the-ROOT AND NOT AN ASSERT. The state is made impossible rather than reported: a record enters
   the map at the instant it GAINS A BODY (js_eval_compile_finish) and not at the instant its parse begins, so
   there is no window in which the map can hand out a record that has none. Nothing during a parse reads the
   map for its own record — the parse runs no user code, and its own two writers (add_req_module_entry,
   add_export_entry2) take `m` directly — which is what makes the move safe rather than merely later.
   `link` IS SELF-LINKED RATHER THAN LEFT ZEROED, because js_free_module_def's js_module_free_tables ends in an
   unconditional `list_del(&m->link)`: on a self-linked node that is a no-op, and on a zeroed one it is a NULL
   store. So a parse that FAILS or is DROPPED frees an unpublished record through exactly the path a published
   one takes, with no second spelling of the teardown to drift. */
static JSModuleDef *js_new_module_def_unpublished(JSContext *ctx, JSAtom name)
{
    JSModuleDef *m;
    m = js_mallocz(ctx, sizeof(*m));
    if (!m) {
        JS_FreeAtom(ctx, name);
        return NULL;
    }
    JS_REF_COUNT(m) = 1;
    m->module_name = name;
    m->module_ns = JS_UNDEFINED;
    m->func_obj = JS_UNDEFINED;
    m->eval_exception = JS_UNDEFINED;
    m->meta_obj = JS_UNDEFINED;
    m->promise = JS_UNDEFINED;
    m->resolving_funcs[0] = JS_UNDEFINED;
    m->resolving_funcs[1] = JS_UNDEFINED;
    m->private_value = JS_UNDEFINED;
    init_list_head(&m->link);
    return m;
}

/* …AND THE PUBLICATION, which is the one act that makes a record findable by name. Asserted rather than made
   idempotent: a second publication would put one record in the list twice, after which list_del removes one
   of the two appearances and every later walk reads a freed node. `list_empty` on the node's OWN link is the
   unpublished test — a self-linked node is a list of itself — which is this file's own spelling and not a
   second encoding of the fact. */
static void js_module_publish(JSContext *ctx, JSModuleDef *m)
{
    DCHECK(list_empty(&m->link),
           "a module record was published to the module map twice — `loaded_modules` would then hold one "
           "record at two positions and the first list_del would leave a freed node on the walk");
    list_add_tail(&m->link, &ctx->loaded_modules);
}

/* The two composed, for every record whose body does not arrive from a parse that can rest: a C module is
   complete the moment it is minted, and the bytecode reader fills its record inside one read. */
static JSModuleDef *js_new_module_def(JSContext *ctx, JSAtom name)
{
    JSModuleDef *m = js_new_module_def_unpublished(ctx, name);
    if (m)
        js_module_publish(ctx, m);
    return m;
}

static void js_mark_module_def(JSRuntime *rt, JSModuleDef *m,
                               JS_MarkFunc *mark_func)
{
    int i;

    for(i = 0; i < m->req_module_entries_count; i++) {
        JSReqModuleEntry *rme = &m->req_module_entries[i];
        JS_MarkValue(rt, rme->attributes, mark_func);
    }

    for(i = 0; i < m->export_entries_count; i++) {
        JSExportEntry *me = &m->export_entries[i];
        if (me->export_type == JS_EXPORT_TYPE_LOCAL &&
            me->u.local.var_ref) {
            mark_func(rt, &me->u.local.var_ref->header);
        }
    }

    JS_MarkValue(rt, m->module_ns, mark_func);
    JS_MarkValue(rt, m->func_obj, mark_func);
    JS_MarkValue(rt, m->eval_exception, mark_func);
    JS_MarkValue(rt, m->meta_obj, mark_func);
    JS_MarkValue(rt, m->promise, mark_func);
    JS_MarkValue(rt, m->resolving_funcs[0], mark_func);
    JS_MarkValue(rt, m->resolving_funcs[1], mark_func);
    JS_MarkValue(rt, m->private_value, mark_func);
}

/* A MODULE'S TEARDOWN, SPLIT THE WAY ITS REALM'S IS — see JS_FreeContext. The split is not a convenience for
   that caller: it is what makes a module's teardown expressible at all when the realm holding it reaches zero
   references INSIDE a collection. Deferring the WHOLE of it to after the collection would release the module's
   JSValues onto objects the collection has already reclaimed, and running the whole of it inside would free the
   module's memory while the collector may still be walking. So the halves are: the references, released where
   the objects are still valid, and the tables, freed when the phase is over.
   EACH HALF RUNS ONCE, and the first leaves nothing for the second to find: every released value is replaced by
   JS_UNDEFINED, so a second release is a no-op the DCHECKs below would not need to catch. */
static void js_module_release_refs(JSContext *ctx, JSModuleDef *m)
{
    int i;

    for(i = 0; i < m->req_module_entries_count; i++) {
        JSReqModuleEntry *rme = &m->req_module_entries[i];
        JS_FreeValue(ctx, rme->attributes);
        rme->attributes = JS_UNDEFINED;
    }
    for(i = 0; i < m->export_entries_count; i++) {
        JSExportEntry *me = &m->export_entries[i];
        if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
            free_var_ref(ctx->rt, me->u.local.var_ref);
            me->u.local.var_ref = NULL;
        }
    }
    JS_FreeValue(ctx, m->module_ns);
    JS_FreeValue(ctx, m->func_obj);
    JS_FreeValue(ctx, m->eval_exception);
    JS_FreeValue(ctx, m->meta_obj);
    JS_FreeValue(ctx, m->promise);
    JS_FreeValue(ctx, m->resolving_funcs[0]);
    JS_FreeValue(ctx, m->resolving_funcs[1]);
    JS_FreeValue(ctx, m->private_value);
    m->module_ns = JS_UNDEFINED;
    m->func_obj = JS_UNDEFINED;
    m->eval_exception = JS_UNDEFINED;
    m->meta_obj = JS_UNDEFINED;
    m->promise = JS_UNDEFINED;
    m->resolving_funcs[0] = JS_UNDEFINED;
    m->resolving_funcs[1] = JS_UNDEFINED;
    m->private_value = JS_UNDEFINED;
}

/* The atoms and the arrays — no JSValue is reachable from here, which is the property that lets this run after
   the collection that freed the objects the half above released. */
static void js_module_free_tables(JSContext *ctx, JSModuleDef *m)
{
    int i;

    JS_FreeAtom(ctx, m->module_name);
    for(i = 0; i < m->req_module_entries_count; i++)
        JS_FreeAtom(ctx, m->req_module_entries[i].module_name);
    js_free(ctx, m->req_module_entries);

    for(i = 0; i < m->export_entries_count; i++) {
        JSExportEntry *me = &m->export_entries[i];
        DCHECK(me->export_type != JS_EXPORT_TYPE_LOCAL || me->u.local.var_ref == NULL,
               "a module's export tables were freed while a local export still held its var_ref — "
               "js_module_release_refs is the half that releases it and it did not run");
        JS_FreeAtom(ctx, me->export_name);
        JS_FreeAtom(ctx, me->local_name);
    }
    js_free(ctx, m->export_entries);

    js_free(ctx, m->star_export_entries);

    for(i = 0; i < m->import_entries_count; i++) {
        JSImportEntry *mi = &m->import_entries[i];
        JS_FreeAtom(ctx, mi->import_name);
    }
    js_free(ctx, m->import_entries);
    js_free(ctx, m->async_parent_modules);
    list_del(&m->link);
    js_free(ctx, m);
}

static void js_free_module_def(JSContext *ctx, JSModuleDef *m)
{
    js_module_release_refs(ctx, m);
    js_module_free_tables(ctx, m);
}

#ifndef QJS_DISABLE_PARSER

static int add_req_module_entry(JSContext *ctx, JSModuleDef *m,
                                JSAtom module_name)
{
    JSReqModuleEntry *rme;
    int i;

    /* no need to add the module request if it is already present */
    for(i = 0; i < m->req_module_entries_count; i++) {
        rme = &m->req_module_entries[i];
        if (rme->module_name == module_name)
            return i;
    }

    if (js_resize_array(ctx, (void **)&m->req_module_entries,
                        sizeof(JSReqModuleEntry),
                        &m->req_module_entries_size,
                        m->req_module_entries_count + 1))
        return -1;
    rme = &m->req_module_entries[m->req_module_entries_count++];
    rme->module_name = JS_DupAtom(ctx, module_name);
    rme->module = NULL;
    rme->attributes = JS_UNDEFINED;
    return i;
}

#endif // QJS_DISABLE_PARSER

static JSExportEntry *find_export_entry(JSContext *ctx, const JSModuleDef *m,
                                        JSAtom export_name)
{
    JSExportEntry *me;
    int i;
    for(i = 0; i < m->export_entries_count; i++) {
        me = &m->export_entries[i];
        if (me->export_name == export_name)
            return me;
    }
    return NULL;
}

static JSExportEntry *add_export_entry2(JSContext *ctx,
                                        JSParseState *s, JSModuleDef *m,
                                       JSAtom local_name, JSAtom export_name,
                                       JSExportTypeEnum export_type)
{
    JSExportEntry *me;

    if (find_export_entry(ctx, m, export_name)) {
        char buf1[ATOM_GET_STR_BUF_SIZE];
        if (s) {
            js_parse_error(s, "duplicate exported name '%s'",
                           JS_AtomGetStr(ctx, buf1, sizeof(buf1), export_name));
        } else {
            JS_ThrowSyntaxErrorAtom(ctx, "duplicate exported name '%s'", export_name);
        }
        return NULL;
    }

    if (js_resize_array(ctx, (void **)&m->export_entries,
                        sizeof(JSExportEntry),
                        &m->export_entries_size,
                        m->export_entries_count + 1))
        return NULL;
    me = &m->export_entries[m->export_entries_count++];
    memset(me, 0, sizeof(*me));
    me->local_name = JS_DupAtom(ctx, local_name);
    me->export_name = JS_DupAtom(ctx, export_name);
    me->export_type = export_type;
    return me;
}

#ifndef QJS_DISABLE_PARSER

static JSExportEntry *add_export_entry(JSParseState *s, JSModuleDef *m,
                                       JSAtom local_name, JSAtom export_name,
                                       JSExportTypeEnum export_type)
{
    return add_export_entry2(s->ctx, s, m, local_name, export_name,
                             export_type);
}

static int add_star_export_entry(JSContext *ctx, JSModuleDef *m,
                                 int req_module_idx)
{
    JSStarExportEntry *se;

    if (js_resize_array(ctx, (void **)&m->star_export_entries,
                        sizeof(JSStarExportEntry),
                        &m->star_export_entries_size,
                        m->star_export_entries_count + 1))
        return -1;
    se = &m->star_export_entries[m->star_export_entries_count++];
    se->req_module_idx = req_module_idx;
    return 0;
}

#endif // QJS_DISABLE_PARSER

/* create a C module */
/* `name_str` may be pure ASCII or UTF-8 encoded */
JSModuleDef *JS_NewCModule(JSContext *ctx, const char *name_str,
                           JSModuleInitFunc *func)
{
    JSModuleDef *m;
    JSAtom name;
    name = JS_NewAtom(ctx, name_str);
    if (name == JS_ATOM_NULL)
        return NULL;
    m = js_new_module_def(ctx, name);
    if (!m)
        return NULL;
    m->init_func = func;
    return m;
}

/* `export_name` may be pure ASCII or UTF-8 encoded */
int JS_AddModuleExport(JSContext *ctx, JSModuleDef *m, const char *export_name)
{
    JSExportEntry *me;
    JSAtom name;
    name = JS_NewAtom(ctx, export_name);
    if (name == JS_ATOM_NULL)
        return -1;
    me = add_export_entry2(ctx, NULL, m, JS_ATOM_NULL, name,
                           JS_EXPORT_TYPE_LOCAL);
    JS_FreeAtom(ctx, name);
    if (!me)
        return -1;
    else
        return 0;
}

/* `export_name` may be pure ASCII or UTF-8 encoded */
int JS_SetModuleExport(JSContext *ctx, JSModuleDef *m, const char *export_name,
                       JSValue val)
{
    JSExportEntry *me;
    JSAtom name;
    name = JS_NewAtom(ctx, export_name);
    if (name == JS_ATOM_NULL)
        goto fail;
    me = find_export_entry(ctx, m, name);
    JS_FreeAtom(ctx, name);
    if (!me)
        goto fail;
    set_value(ctx, me->u.local.var_ref->pvalue, val);
    return 0;
 fail:
    JS_FreeValue(ctx, val);
    return -1;
}

void JS_SetModuleLoaderFunc(JSRuntime *rt,
                            JSModuleNormalizeFunc *module_normalize,
                            JSModuleLoaderFunc *module_loader, void *opaque)
{
    rt->module_normalize_has_attr = false;
    rt->normalize_u.module_normalize_func = module_normalize;
    rt->module_loader_has_attr = false;
    rt->u.module_loader_func = module_loader;
    rt->module_check_attrs = NULL;
    rt->module_loader_opaque = opaque;
}

void JS_SetModuleLoaderFunc2(JSRuntime *rt,
                             JSModuleNormalizeFunc *module_normalize,
                             JSModuleLoaderFunc2 *module_loader,
                             JSModuleCheckSupportedImportAttributes *module_check_attrs,
                             void *opaque)
{
    rt->module_normalize_has_attr = false;
    rt->normalize_u.module_normalize_func = module_normalize;
    rt->module_loader_has_attr = true;
    rt->u.module_loader_func2 = module_loader;
    rt->module_check_attrs = module_check_attrs;
    rt->module_loader_opaque = opaque;
}

void JS_SetModuleImportMetaFunc(JSRuntime *rt, JSModuleImportMetaFunc *func)
{
    rt->module_import_meta_func = func;
}

void JS_SetModuleNormalizeFunc2(JSRuntime *rt,
                                JSModuleNormalizeFunc2 *module_normalize)
{
    rt->module_normalize_has_attr = true;
    rt->normalize_u.module_normalize_func2 = module_normalize;
}

int JS_SetModulePrivateValue(JSContext *ctx, JSModuleDef *m, JSValue val)
{
    set_value(ctx, &m->private_value, val);
    return 0;
}

JSValue JS_GetModulePrivateValue(JSContext *ctx, JSModuleDef *m)
{
    return js_dup(m->private_value);
}

/* default module filename normalizer */
static char *js_default_module_normalize_name(JSContext *ctx,
                                              const char *base_name,
                                              const char *name)
{
    char *filename, *p;
    const char *r;
    int cap;
    int len;

    if (name[0] != '.') {
        /* if no initial dot, the module name is not modified */
        return js_strdup(ctx, name);
    }

    r = strrchr(base_name, '/');
    if (r)
        len = r - base_name;
    else
        len = 0;

    cap = len + strlen(name) + 1 + 1;
    filename = js_malloc(ctx, cap);
    if (!filename)
        return NULL;
    memcpy(filename, base_name, len);
    filename[len] = '\0';

    /* we only normalize the leading '..' or '.' */
    r = name;
    for(;;) {
        if (r[0] == '.' && r[1] == '/') {
            r += 2;
        } else if (r[0] == '.' && r[1] == '.' && r[2] == '/') {
            /* remove the last path element of filename, except if "."
               or ".." */
            if (filename[0] == '\0')
                break;
            p = strrchr(filename, '/');
            if (!p)
                p = filename;
            else
                p++;
            if (!strcmp(p, ".") || !strcmp(p, ".."))
                break;
            if (p > filename)
                p--;
            *p = '\0';
            r += 3;
        } else {
            break;
        }
    }
    if (filename[0] != '\0')
        js__pstrcat(filename, cap, "/");
    js__pstrcat(filename, cap, r);
    //    printf("normalize: %s %s -> %s\n", base_name, name, filename);
    return filename;
}

static JSModuleDef *js_find_loaded_module(JSContext *ctx, JSAtom name)
{
    struct list_head *el;
    JSModuleDef *m;

    /* first look at the loaded modules */
    list_for_each(el, &ctx->loaded_modules) {
        m = list_entry(el, JSModuleDef, link);
        if (m->module_name == name)
            return m;
    }
    return NULL;
}

/* return NULL in case of exception (e.g. module could not be loaded) */
/* `base_cname` and `cname1` may be pure ASCII or UTF-8 encoded */
/* HostLoadImportedModule keys the module map on the specifier AND the import attributes, so the same file
   imported as JavaScript and with `type: 'text'` are DIFFERENT Module Records. Keying on the specifier alone
   handed a self-referential text import the JS record, whose `default` export does not exist. The separator is
   a control character, which cannot appear in a path or in a type name, so the key is unambiguous. */
static JSAtom js_module_map_key(JSContext *ctx, const char *cname, JSValueConst attributes)
{
    JSValue tv;
    const char *tstr;
    char *keyed;
    JSAtom a;
    size_t n;

    if (!JS_IsObject(attributes))
        return JS_NewAtom(ctx, cname);
    tv = JS_GetPropertyStr(ctx, attributes, "type");
    if (JS_IsException(tv))
        return JS_ATOM_NULL;
    if (!JS_IsString(tv)) {
        JS_FreeValue(ctx, tv);
        return JS_NewAtom(ctx, cname);
    }
    tstr = JS_ToCString(ctx, tv);
    JS_FreeValue(ctx, tv);
    if (!tstr)
        return JS_ATOM_NULL;
    n = strlen(cname) + strlen(tstr) + sizeof("\001type=");
    keyed = js_malloc(ctx, n);
    if (!keyed) {
        JS_FreeCString(ctx, tstr);
        return JS_ATOM_NULL;
    }
    snprintf(keyed, n, "%s\001type=%s", cname, tstr);
    JS_FreeCString(ctx, tstr);
    a = JS_NewAtom(ctx, keyed);
    js_free(ctx, keyed);
    return a;
}

/* true when `key` is just the specifier — i.e. the attributes contributed nothing to the module map key. */
static bool keyed_by_attributes(JSContext *ctx, JSAtom key, const char *cname)
{
    JSAtom plain = JS_NewAtom(ctx, cname);
    bool same = (plain == key);
    JS_FreeAtom(ctx, plain);
    return same;
}

static JSModuleDef *js_host_resolve_imported_module(JSContext *ctx,
                                                    const char *base_cname,
                                                    const char *cname1,
                                                    JSValueConst attributes)
{
    JSRuntime *rt = ctx->rt;
    JSModuleDef *m;
    char *cname;
    JSAtom module_name;

    if (!rt->normalize_u.module_normalize_func && !rt->normalize_u.module_normalize_func2) {
        cname = js_default_module_normalize_name(ctx, base_cname, cname1);
    } else if (rt->module_normalize_has_attr) {
        cname = rt->normalize_u.module_normalize_func2(ctx, base_cname, cname1,
                                                       attributes,
                                                       rt->module_loader_opaque);
    } else {
        cname = rt->normalize_u.module_normalize_func(ctx, base_cname, cname1,
                                                      rt->module_loader_opaque);
    }
    if (!cname)
        return NULL;

    module_name = js_module_map_key(ctx, cname, attributes);
    if (module_name == JS_ATOM_NULL) {
        js_free(ctx, cname);
        return NULL;
    }

    /* first look at the loaded modules */
    m = js_find_loaded_module(ctx, module_name);
    if (m) {
        js_free(ctx, cname);
        JS_FreeAtom(ctx, module_name);
        return m;
    }

    /* load the module */
    if (!rt->u.module_loader_func) {
        /* XXX: use a syntax error ? */
        // XXX: update JS_DetectModule when you change this
        JS_ThrowReferenceError(ctx, "could not load module '%s'",
                               cname);
        js_free(ctx, cname);
        JS_FreeAtom(ctx, module_name);
        return NULL;
    }

    if (rt->module_loader_has_attr) {
        m = rt->u.module_loader_func2(ctx, cname, rt->module_loader_opaque, attributes);
    } else {
        m = rt->u.module_loader_func(ctx, cname, rt->module_loader_opaque);
    }
    /* THE LOADER PARKED, so the RESOLVED specifier goes with the promise. Only this function ever computes it,
       and whoever finishes the load needs it for both of the things a module's name is: the module map key it
       will be registered under, and the base every specifier inside it resolves against. Written here, taken by
       js_module_take_pending — one write, one read, and a DCHECK on each side that the pair is never split. */
    if (!m && !JS_IsUndefined(ctx->module_load_pending)) {
        DCHECK(JS_IsUndefined(ctx->module_load_pending_name),
               "a second module load parked its resolved specifier on a slot the first was never taken from");
        ctx->module_load_pending_name = JS_NewString(ctx, cname);
        CHECK(!JS_IsException(ctx->module_load_pending_name),
              "a parked module load could not keep its resolved specifier — nothing could then finish the load, "
              "and the flow that imported would stay parked on a source that never arrives");
    }
    /* The loader registers the record under the raw specifier (it is loading a FILE). When the attributes made
       the key DIFFER from that specifier, re-key the record so the next import with the same type finds it and
       one with a different type does not. A plain import's key IS the specifier, and then nothing is touched —
       the module name is also its resolution base for nested imports, so only a synthetic module (json/text/
       bytes, which has neither nested imports nor import.meta) is ever renamed. */
    if (m && !keyed_by_attributes(ctx, module_name, cname)) {
        JS_FreeAtom(ctx, m->module_name);
        m->module_name = module_name;   /* ownership transfers */
    } else {
        JS_FreeAtom(ctx, module_name);
    }
    js_free(ctx, cname);
    return m;
}

static JSModuleDef *js_host_resolve_imported_module_atom(JSContext *ctx,
                                                         JSAtom base_module_name,
                                                         JSAtom module_name1,
                                                         JSValueConst attributes)
{
    const char *base_cname, *cname;
    JSModuleDef *m;

    base_cname = JS_AtomToCString(ctx, base_module_name);
    if (!base_cname)
        return NULL;
    cname = JS_AtomToCString(ctx, module_name1);
    if (!cname) {
        JS_FreeCString(ctx, base_cname);
        return NULL;
    }
    m = js_host_resolve_imported_module(ctx, base_cname, cname, attributes);
    JS_FreeCString(ctx, base_cname);
    JS_FreeCString(ctx, cname);
    return m;
}

typedef struct JSResolveEntry {
    JSModuleDef *module;
    JSAtom name;
} JSResolveEntry;

typedef struct JSResolveState {
    JSResolveEntry *array;
    int size;
    int count;
} JSResolveState;

static int find_resolve_entry(JSResolveState *s,
                              JSModuleDef *m, JSAtom name)
{
    int i;
    for(i = 0; i < s->count; i++) {
        JSResolveEntry *re = &s->array[i];
        if (re->module == m && re->name == name)
            return i;
    }
    return -1;
}

static int add_resolve_entry(JSContext *ctx, JSResolveState *s,
                             JSModuleDef *m, JSAtom name)
{
    JSResolveEntry *re;

    if (js_resize_array(ctx, (void **)&s->array,
                        sizeof(JSResolveEntry),
                        &s->size, s->count + 1))
        return -1;
    re = &s->array[s->count++];
    re->module = m;
    re->name = JS_DupAtom(ctx, name);
    return 0;
}

typedef enum JSResolveResultEnum {
    JS_RESOLVE_RES_EXCEPTION = -1, /* memory alloc error */
    JS_RESOLVE_RES_FOUND = 0,
    JS_RESOLVE_RES_NOT_FOUND,
    JS_RESOLVE_RES_CIRCULAR,
    JS_RESOLVE_RES_AMBIGUOUS,
} JSResolveResultEnum;

/* One ResolveExport activation. `star_i` IS the resume point: < 0 means the frame has not run its head yet,
   >= 0 means it is partway through its star-export walk and the registers below carry what the child returned. */
typedef struct JSResolveFrame {
    JSModuleDef *m;
    JSAtom export_name;     /* borrowed: `s` owns the dup that guards the cycle */
    int star_i;
    JSModuleDef *acc_module; /* the resolution accumulated so far by this frame's star walk */
    JSExportEntry *acc_me;
} JSResolveFrame;

static int js_resolve_frame_push(JSContext *ctx, JSResolveFrame **pstack,
                                 int *psize, int *psp,
                                 JSModuleDef *m, JSAtom export_name)
{
    JSResolveFrame *f;
    if (js_resize_array(ctx, (void **)pstack, sizeof(JSResolveFrame), psize, *psp + 1))
        return -1;
    f = &(*pstack)[(*psp)++];
    f->m = m;
    f->export_name = export_name;
    f->star_i = -1;
    f->acc_module = NULL;
    f->acc_me = NULL;
    return 0;
}

/* THE STAR-EXPORT WALK IS THE PAGE'S GRAPH. ResolveExport recurses twice over data the page writes: an
   INDIRECT re-export forwards the whole resolution (`export { v } from "m"`), and a STAR export fans out over
   every `export * from`. Both were C frames, so a chain of N re-exports was N frames deep and 40000 of them
   segfaulted. The indirect case is a tail forward and becomes an assignment; the star case is a real branching
   search with an accumulator, so it becomes an explicit frame stack driven by one loop. `s` still carries the
   visited set, so the cycle guard is untouched, and `star_i` is the frame's resume point. */
static JSResolveResultEnum js_resolve_export1(JSContext *ctx,
                                              JSModuleDef **pmodule,
                                              JSExportEntry **pme,
                                              JSModuleDef *m,
                                              JSAtom export_name,
                                              JSResolveState *s)
{
    JSResolveFrame *stack = NULL;
    int stack_size = 0, sp = 0;
    /* the return registers of the frame that just finished — read only at the resume point */
    JSResolveResultEnum ret = JS_RESOLVE_RES_NOT_FOUND;
    JSModuleDef *res_m = NULL;
    JSExportEntry *res_me = NULL;

    if (js_resolve_frame_push(ctx, &stack, &stack_size, &sp, m, export_name) < 0) {
        *pmodule = NULL;
        *pme = NULL;
        return JS_RESOLVE_RES_EXCEPTION;
    }

    for(;;) {
        JSResolveFrame *f = &stack[sp - 1];

        if (f->star_i < 0) {
            /* head: resolve within this module, following indirect re-exports without a frame */
            for(;;) {
                JSExportEntry *me;
                if (find_resolve_entry(s, f->m, f->export_name) >= 0) {
                    ret = JS_RESOLVE_RES_CIRCULAR;
                    goto frame_done;
                }
                if (add_resolve_entry(ctx, s, f->m, f->export_name) < 0) {
                    ret = JS_RESOLVE_RES_EXCEPTION;
                    goto frame_done;
                }
                me = find_export_entry(ctx, f->m, f->export_name);
                if (!me)
                    break;
                if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
                    /* local export */
                    res_m = f->m;
                    res_me = me;
                    ret = JS_RESOLVE_RES_FOUND;
                    goto frame_done;
                }
                /* indirect export */
                if (me->is_namespace) {
                    /* export ns from */
                    res_m = f->m;
                    res_me = me;
                    ret = JS_RESOLVE_RES_FOUND;
                    goto frame_done;
                }
                f->m = f->m->req_module_entries[me->u.req_module_idx].module;
                f->export_name = me->local_name;
            }
            /* not found in direct or indirect exports: try star exports */
            if (f->export_name == JS_ATOM_default) {
                ret = JS_RESOLVE_RES_NOT_FOUND;
                goto frame_done;
            }
            f->star_i = 0;
        } else {
            /* resume: the star-export child at f->star_i returned into (ret, res_m, res_me) */
            if (ret == JS_RESOLVE_RES_AMBIGUOUS || ret == JS_RESOLVE_RES_EXCEPTION)
                goto frame_done;
            if (ret == JS_RESOLVE_RES_FOUND) {
                if (f->acc_me != NULL) {
                    /* 16.2.1.7.2.2 ResolveExport step 10.d.ii compares the ResolvedBinding's [[Module]] and
                       [[BindingName]]. For an `all` indirect entry (`export * as ns from "m"`, and an
                       `export { ns }` of a namespace import) the ResolvedBinding is
                       { Module: the IMPORTED module, BindingName: namespace } — NOT the re-exporting module
                       and its local name, which is what this compared, making two star-exports of the same
                       namespace look ambiguous. */
                    JSModuleDef *a_mod = res_m, *b_mod = f->acc_module;
                    bool a_ns = (res_me->export_type == JS_EXPORT_TYPE_INDIRECT &&
                                 res_me->is_namespace);
                    bool b_ns = (f->acc_me->export_type == JS_EXPORT_TYPE_INDIRECT &&
                                 f->acc_me->is_namespace);
                    if (a_ns)
                        a_mod = res_m->req_module_entries[res_me->u.req_module_idx].module;
                    if (b_ns)
                        b_mod = f->acc_module->req_module_entries[f->acc_me->u.req_module_idx].module;
                    if (a_ns != b_ns || a_mod != b_mod ||
                        (!a_ns && res_me->local_name != f->acc_me->local_name)) {
                        ret = JS_RESOLVE_RES_AMBIGUOUS;
                        goto frame_done;
                    }
                } else {
                    f->acc_module = res_m;
                    f->acc_me = res_me;
                }
            }
            f->star_i++;
        }

        if (f->star_i < f->m->star_export_entries_count) {
            JSStarExportEntry *se = &f->m->star_export_entries[f->star_i];
            JSModuleDef *m1 = f->m->req_module_entries[se->req_module_idx].module;
            if (js_resolve_frame_push(ctx, &stack, &stack_size, &sp, m1, f->export_name) < 0) {
                ret = JS_RESOLVE_RES_EXCEPTION;
                goto frame_done;
            }
            continue;
        }
        if (f->acc_me != NULL) {
            res_m = f->acc_module;
            res_me = f->acc_me;
            ret = JS_RESOLVE_RES_FOUND;
        } else {
            ret = JS_RESOLVE_RES_NOT_FOUND;
        }
    frame_done:
        /* the caller frame resumes with (ret, res_m, res_me); an AMBIGUOUS or EXCEPTION result is re-read
           at its resume point and pops it in turn, which is how the original's early `return` unwound. */
        if (--sp == 0)
            break;
    }

    js_free(ctx, stack);
    if (ret == JS_RESOLVE_RES_FOUND) {
        *pmodule = res_m;
        *pme = res_me;
    } else {
        *pmodule = NULL;
        *pme = NULL;
    }
    return ret;
}

/* If the return value is JS_RESOLVE_RES_FOUND, return the module
  (*pmodule) and the corresponding local export entry
  (*pme). Otherwise return (NULL, NULL) */
static JSResolveResultEnum js_resolve_export(JSContext *ctx,
                                             JSModuleDef **pmodule,
                                             JSExportEntry **pme,
                                             JSModuleDef *m,
                                             JSAtom export_name)
{
    JSResolveState ss, *s = &ss;
    int i;
    JSResolveResultEnum ret;

    s->array = NULL;
    s->size = 0;
    s->count = 0;

    ret = js_resolve_export1(ctx, pmodule, pme, m, export_name, s);

    for(i = 0; i < s->count; i++)
        JS_FreeAtom(ctx, s->array[i].name);
    js_free(ctx, s->array);

    return ret;
}

static JSVarRef *js_get_local_export_var_ref1(JSContext *ctx, JSModuleDef *m,
                                              JSExportEntry *me,
                                              JSResolveState *s)
{
    /* FOLLOWING THE ALIAS IS A WALK ALONG THE PAGE'S RE-EXPORT CHAIN, and it was a tail call, so the chain's
       length was C-stack depth. It carries nothing across the hop — the next module and export entry are the
       whole state — so it is an assignment. `s` still guards the cycle. */
    for(;;) {
        JSVarRef *var_ref = me->u.local.var_ref;
        bool followed = false;
        int i;

        if (!var_ref) {
            JSObject *p = JS_VALUE_GET_OBJ(m->func_obj);
            var_ref = p->u.func.var_refs[me->u.local.var_idx];
        }
        if (var_ref)
            return var_ref;
        /* the slot is a re-exported import whose source is not linked yet: follow
           the import alias, guarding against re-export cycles like js_resolve_export1 */
        for(i = 0; i < m->import_entries_count; i++) {
            JSImportEntry *mi = &m->import_entries[i];
            JSModuleDef *m1, *res_m;
            JSExportEntry *res_me;

            if (mi->var_idx != me->u.local.var_idx)
                continue;
            /* a namespace import's slot holds the namespace OBJECT — there is no exported binding behind it to
               follow, and its import_name is the `*` the user wrote, which would otherwise be looked up as a
               real export name. */
            if (mi->is_namespace)
                return NULL;
            if (find_resolve_entry(s, m, mi->import_name) >= 0)
                return NULL;
            if (add_resolve_entry(ctx, s, m, mi->import_name) < 0)
                return NULL;
            m1 = m->req_module_entries[mi->req_module_idx].module;
            if (js_resolve_export1(ctx, &res_m, &res_me, m1, mi->import_name, s) ==
                    JS_RESOLVE_RES_FOUND &&
                !res_me->is_namespace) {
                m = res_m;
                me = res_me;
                followed = true;
            }
            break;
        }
        if (!followed)
            return NULL;
    }
}

/* var_ref of a local export. For a re-exported import the slot is NULL until
   the source is linked, so follow the import alias; returns NULL if
   unresolvable (e.g. a re-export cycle with no concrete binding). */
static JSVarRef *js_get_local_export_var_ref(JSContext *ctx, JSModuleDef *m,
                                             JSExportEntry *me)
{
    JSResolveState ss, *s = &ss;
    JSVarRef *var_ref;
    int i;

    s->array = NULL;
    s->size = 0;
    s->count = 0;
    var_ref = js_get_local_export_var_ref1(ctx, m, me, s);
    for(i = 0; i < s->count; i++)
        JS_FreeAtom(ctx, s->array[i].name);
    js_free(ctx, s->array);
    return var_ref;
}

static void js_resolve_export_throw_error(JSContext *ctx,
                                          JSResolveResultEnum res,
                                          JSModuleDef *m, JSAtom export_name)
{
    char buf1[ATOM_GET_STR_BUF_SIZE];
    char buf2[ATOM_GET_STR_BUF_SIZE];
    switch(res) {
    case JS_RESOLVE_RES_EXCEPTION:
        break;
    default:
    case JS_RESOLVE_RES_NOT_FOUND:
        JS_ThrowSyntaxError(ctx, "Could not find export '%s' in module '%s'",
                            JS_AtomGetStr(ctx, buf1, sizeof(buf1), export_name),
                            JS_AtomGetStr(ctx, buf2, sizeof(buf2), m->module_name));
        break;
    case JS_RESOLVE_RES_CIRCULAR:
        JS_ThrowSyntaxError(ctx, "circular reference when looking for export '%s' in module '%s'",
                            JS_AtomGetStr(ctx, buf1, sizeof(buf1), export_name),
                            JS_AtomGetStr(ctx, buf2, sizeof(buf2), m->module_name));
        break;
    case JS_RESOLVE_RES_AMBIGUOUS:
        JS_ThrowSyntaxError(ctx, "export '%s' in module '%s' is ambiguous",
                            JS_AtomGetStr(ctx, buf1, sizeof(buf1), export_name),
                            JS_AtomGetStr(ctx, buf2, sizeof(buf2), m->module_name));
        break;
    }
}


typedef enum {
    EXPORTED_NAME_AMBIGUOUS,
    EXPORTED_NAME_NORMAL,
    EXPORTED_NAME_NS,
} ExportedNameEntryEnum;

typedef struct ExportedNameEntry {
    JSAtom export_name;
    ExportedNameEntryEnum export_type;
    union {
        JSExportEntry *me; /* using when the list is built */
        JSVarRef *var_ref; /* EXPORTED_NAME_NORMAL */
        JSModuleDef *module; /* for EXPORTED_NAME_NS */
    } u;
} ExportedNameEntry;

typedef struct GetExportNamesState {
    JSModuleDef **modules;
    int modules_size;
    int modules_count;

    ExportedNameEntry *exported_names;
    int exported_names_size;
    int exported_names_count;
} GetExportNamesState;

static int find_exported_name(GetExportNamesState *s, JSAtom name)
{
    int i;
    for(i = 0; i < s->exported_names_count; i++) {
        if (s->exported_names[i].export_name == name)
            return i;
    }
    return -1;
}

static __exception int get_exported_names(JSContext *ctx,
                                          GetExportNamesState *s,
                                          JSModuleDef *m, bool from_star)
{
    ExportedNameEntry *en;
    int i, j, cursor, start;

    /* THE `export * from` CHAIN IS THE PAGE'S GRAPH, and this walked it by recursing into each star target —
       one C frame per link of a re-export chain the page writes. It needs no separate worklist: `s->modules`
       IS one. It is already the circular-reference set, appended in exactly the order modules are first
       reached, so walking it with a cursor visits the same modules in the same order with no frames.
       `from_star` follows from the position: the module this call was given keeps the caller's value, and every
       module appended after it got there through a star export, which is what from_star means. */
    for(i = 0; i < s->modules_count; i++) {
        if (s->modules[i] == m)
            return 0;
    }
    if (js_resize_array(ctx, (void **)&s->modules, sizeof(s->modules[0]),
                        &s->modules_size, s->modules_count + 1))
        return -1;
    start = s->modules_count;
    s->modules[s->modules_count++] = m;

    for(cursor = start; cursor < s->modules_count; cursor++) {
        JSModuleDef *cm = s->modules[cursor];
        bool cstar = (cursor == start) ? from_star : true;

        for(i = 0; i < cm->export_entries_count; i++) {
            JSExportEntry *me = &cm->export_entries[i];
            if (cstar && me->export_name == JS_ATOM_default)
                continue;
            j = find_exported_name(s, me->export_name);
            if (j < 0) {
                if (js_resize_array(ctx, (void **)&s->exported_names,
                                    sizeof(s->exported_names[0]),
                                    &s->exported_names_size,
                                    s->exported_names_count + 1))
                    return -1;
                en = &s->exported_names[s->exported_names_count++];
                en->export_name = me->export_name;
                /* avoid a second lookup for simple module exports */
                if (cstar || me->export_type != JS_EXPORT_TYPE_LOCAL)
                    en->u.me = NULL;
                else
                    en->u.me = me;
            } else {
                en = &s->exported_names[j];
                en->u.me = NULL;
            }
        }
        for(i = 0; i < cm->star_export_entries_count; i++) {
            JSStarExportEntry *se = &cm->star_export_entries[i];
            JSModuleDef *m1 = cm->req_module_entries[se->req_module_idx].module;
            int k;
            for(k = 0; k < s->modules_count; k++) {
                if (s->modules[k] == m1)
                    break;
            }
            if (k < s->modules_count)
                continue;   /* already reached: the circular-reference check, unchanged */
            if (js_resize_array(ctx, (void **)&s->modules, sizeof(s->modules[0]),
                                &s->modules_size, s->modules_count + 1))
                return -1;
            s->modules[s->modules_count++] = m1;   /* the queue this loop walks */
        }
    }
    return 0;
}

/* Unfortunately, the spec gives a different behavior from GetOwnProperty ! */
static int js_module_ns_has(JSContext *ctx, JSValueConst obj, JSAtom atom)
{
    return (find_own_property1(JS_VALUE_GET_OBJ(obj), atom) != NULL);
}

static const JSClassExoticMethods js_module_ns_exotic_methods = {
    .has_property = js_module_ns_has,
};

static int exported_names_cmp(const void *p1, const void *p2, void *opaque)
{
    JSContext *ctx = opaque;
    const ExportedNameEntry *me1 = p1;
    const ExportedNameEntry *me2 = p2;
    JSValue str1, str2;
    int ret;

    /* XXX: should avoid allocation memory in atom comparison */
    str1 = JS_AtomToString(ctx, me1->export_name);
    str2 = JS_AtomToString(ctx, me2->export_name);
    if (JS_IsException(str1) || JS_IsException(str2)) {
        /* XXX: raise an error ? */
        ret = 0;
    } else {
        ret = js_string_compare(JS_VALUE_GET_STRING(str1),
                                JS_VALUE_GET_STRING(str2));
    }
    JS_FreeValue(ctx, str1);
    JS_FreeValue(ctx, str2);
    return ret;
}

static JSValue js_module_ns_autoinit(JSContext *ctx, JSObject *p, JSAtom atom,
                                     void *opaque)
{
    JSModuleDef *m = opaque;
    return JS_GetModuleNamespace(ctx, m);
}

static JSValue js_build_module_ns(JSContext *ctx, JSModuleDef *m)
{
    JSValue obj;
    JSObject *p;
    GetExportNamesState s_s, *s = &s_s;
    int i, ret;
    JSProperty *pr;

    obj = JS_NewObjectClass(ctx, JS_CLASS_MODULE_NS);
    if (JS_IsException(obj))
        return obj;
    p = JS_VALUE_GET_OBJ(obj);

    memset(s, 0, sizeof(*s));
    ret = get_exported_names(ctx, s, m, false);
    js_free(ctx, s->modules);
    if (ret)
        goto fail;

    /* Resolve the exported names. The ambiguous exports are removed */
    for(i = 0; i < s->exported_names_count; i++) {
        ExportedNameEntry *en = &s->exported_names[i];
        JSResolveResultEnum res;
        JSExportEntry *res_me;
        JSModuleDef *res_m;

        if (en->u.me) {
            res_me = en->u.me; /* fast case: no resolution needed */
            res_m = m;
            res = JS_RESOLVE_RES_FOUND;
        } else {
            res = js_resolve_export(ctx, &res_m, &res_me, m,
                                    en->export_name);
        }
        if (res != JS_RESOLVE_RES_FOUND) {
            if (res != JS_RESOLVE_RES_AMBIGUOUS) {
                /* for a re-export the binding is missing from the target
                   module, not from m; report the target and the looked-up name */
                JSExportEntry *me = find_export_entry(ctx, m, en->export_name);
                if (me && me->export_type == JS_EXPORT_TYPE_INDIRECT &&
                    !me->is_namespace) {
                    JSModuleDef *m1 =
                        m->req_module_entries[me->u.req_module_idx].module;
                    js_resolve_export_throw_error(ctx, res, m1, me->local_name);
                } else {
                    js_resolve_export_throw_error(ctx, res, m, en->export_name);
                }
                goto fail;
            }
            en->export_type = EXPORTED_NAME_AMBIGUOUS;
        } else {
            if (res_me->is_namespace) {
                en->export_type = EXPORTED_NAME_NS;
                en->u.module = res_m->req_module_entries[res_me->u.req_module_idx].module;
            } else {
                en->export_type = EXPORTED_NAME_NORMAL;
                /* follow the import alias when the slot is NULL (cyclic
                   re-export of an imported binding) */
                en->u.var_ref = js_get_local_export_var_ref(ctx, res_m, res_me);
            }
        }
    }

    /* sort the exported names */
    rqsort(s->exported_names, s->exported_names_count,
           sizeof(s->exported_names[0]), exported_names_cmp, ctx);

    for(i = 0; i < s->exported_names_count; i++) {
        ExportedNameEntry *en = &s->exported_names[i];
        switch(en->export_type) {
        case EXPORTED_NAME_NORMAL:
            {
                JSVarRef *var_ref = en->u.var_ref;
                if (!var_ref) {
                    js_resolve_export_throw_error(ctx, JS_RESOLVE_RES_CIRCULAR,
                                                  m, en->export_name);
                    goto fail;
                }
                pr = add_property(ctx, p, en->export_name,
                                  JS_PROP_ENUMERABLE | JS_PROP_WRITABLE |
                                  JS_PROP_VARREF);
                if (!pr)
                    goto fail;
                JS_REF_COUNT(var_ref)++;
                pr->u.var_ref = var_ref;
            }
            break;
        case EXPORTED_NAME_NS:
            /* the exported namespace must be created on demand */
            if (JS_DefineAutoInitProperty(ctx, obj,
                                          en->export_name,
                                          JS_AUTOINIT_ID_MODULE_NS,
                                          en->u.module, JS_PROP_ENUMERABLE | JS_PROP_WRITABLE) < 0)
                goto fail;
            break;
        default:
            break;
        }
    }

    js_free(ctx, s->exported_names);

    JS_DefinePropertyValue(ctx, obj, JS_ATOM_Symbol_toStringTag,
                           JS_AtomToString(ctx, JS_ATOM_Module),
                           0);

    p->extensible = false;
    return obj;
 fail:
    js_free(ctx, s->exported_names);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

JSValue JS_GetModuleNamespace(JSContext *ctx, JSModuleDef *m)
{
    if (JS_IsUndefined(m->module_ns)) {
        JSValue val;
        val = js_build_module_ns(ctx, m);
        if (JS_IsException(val))
            return JS_EXCEPTION;
        m->module_ns = val;
    }
    return js_dup(m->module_ns);
}

#ifdef ENABLE_DUMPS // JS_DUMP_MODULE_RESOLVE
#define module_trace(ctx, ...) \
   do { \
     if (check_dump_flag(ctx->rt, JS_DUMP_MODULE_RESOLVE)) \
       printf(__VA_ARGS__); \
   } while (0)
#else
#define module_trace(...)
#endif

/* Load all the required modules for module 'm' */
/* 16.2.1.7.1 ParseModule step 10.a.ii: an `export { x }` whose LOCAL name is an IMPORTED binding is NOT a local
   export — it is an INDIRECT re-export of the original binding, so ResolveExport reaches the very binding the
   importer would. For a NAMESPACE import (`import * as ns from "m"; export { ns }`) that is the `all` form,
   which resolves to { Module: m, BindingName: namespace }: that is why two modules re-exporting the same
   namespace are ONE binding, not an ambiguity between two module-local variables. */
static void js_module_reexport_imported_bindings(JSContext *ctx, JSModuleDef *m, JSFunctionDef *fd)
{
    int i, j;

    for (i = 0; i < m->export_entries_count; i++) {
        JSExportEntry *me = &m->export_entries[i];
        if (me->export_type != JS_EXPORT_TYPE_LOCAL)
            continue;
        for (j = 0; j < m->import_entries_count; j++) {
            JSImportEntry *mi = &m->import_entries[j];
            DCHECK(mi->var_idx >= 0 && mi->var_idx < fd->closure_var_count,
                   "a module import entry names a closure var outside the module's own scope");
            if (fd->closure_var[mi->var_idx].var_name != me->local_name)
                continue;
            JS_FreeAtom(ctx, me->local_name);
            me->local_name = JS_DupAtom(ctx, mi->import_name);
            /* the `all` form is carried by the FLAG, not by the name: `import {"*" as x} from "m"; export {x}`
               re-exports a binding literally named "*" and must not become a namespace re-export. */
            me->is_namespace = mi->is_namespace;
            me->export_type = JS_EXPORT_TYPE_INDIRECT;
            me->u.req_module_idx = mi->req_module_idx;
            break;
        }
    }
}

