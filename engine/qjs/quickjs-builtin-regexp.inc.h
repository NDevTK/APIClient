/* RegExp */

static void js_regexp_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSRegExp *re = &p->u.regexp;
    /* JS_NewObjectFromShape initialises both to NULL, so an UNINITIALISED RegExp is a state of the class and
       the finalizer has to agree with that. It only became reachable when the constructor became a machine:
       22.2.4.1 creates the object in step 7 and compiles in step 8, so a throw or an abandon between them frees
       one. The C body assigned both the instant it created the object, which is why this was never hit. */
    if (re->bytecode) JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_STRING, re->bytecode));
    if (re->pattern)  JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_STRING, re->pattern));
    if (re->realm)    JS_FreeContext(re->realm);
}

/* [[Realm]] is a strong reference to a context, and a context reaches its own %RegExp% instances, so the pair
   is a cycle. Every other holder of a realm is marked (a C function's, a bytecode function's); this class had
   no mark function at all because it had nothing to mark until now. */
static void js_regexp_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    if (p->u.regexp.realm)
        mark_func(rt, &p->u.regexp.realm->header);
}

/* 22.2.3.3 RegExpInitialize steps 5-11: `flags` (undefined, or a String) is read into LRE flags, refusing a
   code unit outside "dgimsuvy" or one given twice with a SyntaxError, and u with v, which 22.2.3.4
   ParsePattern step 1 refuses. 0, or -1 with the exception pending. */
static int js_regexp_parse_flags(JSContext *ctx, JSValueConst flags, int *pre_flags)
{
    const char *str;
    int re_flags, mask;
    size_t i, len;

    re_flags = 0;
    if (!JS_IsUndefined(flags)) {
        str = JS_ToCStringLen(ctx, &len, flags);
        if (!str)
            return -1;
        /* XXX: re_flags = LRE_FLAG_OCTAL unless strict mode? */
        for (i = 0; i < len; i++) {
            switch(str[i]) {
            case 'd':
                mask = LRE_FLAG_INDICES;
                break;
            case 'g':
                mask = LRE_FLAG_GLOBAL;
                break;
            case 'i':
                mask = LRE_FLAG_IGNORECASE;
                break;
            case 'm':
                mask = LRE_FLAG_MULTILINE;
                break;
            case 's':
                mask = LRE_FLAG_DOTALL;
                break;
            case 'u':
                mask = LRE_FLAG_UNICODE;
                break;
            case 'v':
                mask = LRE_FLAG_UNICODE_SETS;
                break;
            case 'y':
                mask = LRE_FLAG_STICKY;
                break;
            default:
                goto bad_flags;
            }
            if ((re_flags & mask) != 0) {
            bad_flags:
                JS_FreeCString(ctx, str);
                return (JS_ThrowSyntaxError(ctx, "invalid regular expression flags"), -1);
            }
            re_flags |= mask;
        }
        JS_FreeCString(ctx, str);
    }

    if (re_flags & LRE_FLAG_UNICODE)
        if (re_flags & LRE_FLAG_UNICODE_SETS)
            return (JS_ThrowSyntaxError(ctx, "invalid regular expression flags"), -1);
    *pre_flags = re_flags;
    return 0;
}

/* Begin compiling the String `pattern` (steps 12-13: under u or v it is read as code points, so it must be
   UTF-8 rather than CESU-8). NULL with the exception pending. The caller owns the compile. */
static REParseState *js_regexp_compile_begin(JSContext *ctx, JSValueConst pattern, int re_flags, bool can_yield)
{
    const char *str;
    size_t len;
    REParseState *c;

    DCHECK(JS_IsString(pattern), "a regexp compile began over a pattern RegExpInitialize step 2 did not coerce");
    str = JS_ToCStringLen2(ctx, &len, pattern, !(re_flags & (LRE_FLAG_UNICODE | LRE_FLAG_UNICODE_SETS)));
    if (!str)
        return NULL;
    c = lre_compile_begin(str, len, re_flags, can_yield, ctx);
    JS_FreeCString(ctx, str);
    if (!c)
        JS_ThrowOutOfMemory(ctx);
    return c;
}

/* One stint of steps 14-22: 0 with *pbc holding the program as an indirect String, -1 with the SyntaxError
   pending, or LRE_RET_YIELD. The compile stays the caller's to end on every answer. */
static int js_regexp_compile_run(JSContext *ctx, REParseState *c, JSValue *pbc)
{
    uint8_t *re_bytecode_buf;
    int re_bytecode_len, r;
    JSValue ret;

    r = lre_compile_step(c);
    if (r == LRE_RET_YIELD)
        return r;
    if (r < 0)
        return (JS_ThrowSyntaxError(ctx, "%s", lre_compile_error(c)), -1);
    re_bytecode_buf = lre_compile_take(c, &re_bytecode_len);
    ret = js_new_string8_len(ctx, (void *)&re_bytecode_buf,
                             sizeof(re_bytecode_buf));
    if (JS_IsException(ret)) {
        js_free(ctx, re_bytecode_buf);
        return -1;
    }
    {
        JSString *p = JS_VALUE_GET_STRING(ret);
        p->kind = JS_STRING_KIND_INDIRECT;
        p->len = re_bytecode_len;
    }
    *pbc = ret;
    return 0;
}

/* A compile as a stage of a machine. The compile is a cache in *pcomp: a fork or a cold park sheds it and the
   next entry begins it again from the pattern the machine holds. 0 with *pbc set, -1 thrown, JS_STEP_YIELD to
   be re-entered. */
static int step_regexp_compile_run(JSContext *ctx, void **pcomp, JSValueConst pattern, int re_flags,
                                   JSValue *pbc)
{
    int r;

    if (!*pcomp) {
        *pcomp = js_regexp_compile_begin(ctx, pattern, re_flags, true);
        if (!*pcomp)
            return -1;
    }
    r = js_regexp_compile_run(ctx, *pcomp, pbc);
    if (r == LRE_RET_YIELD) {
        g_regexp_compile_rests++;
        return JS_STEP_YIELD;
    }
    lre_compile_end(*pcomp);
    *pcomp = NULL;
    return r;
}

static void js_regexp_compile_cache_destroy(JSContext *ctx, void *p)
{
    (void)ctx;
    lre_compile_end(p);
}

/* The script parse's regexp literal (ctx->compile_regexp): flags and pattern are already Strings, so nothing
   here runs page code. The parse steps the compile with js_regexp_compile_run and ends it. */
static REParseState *js_compile_regexp(JSContext *ctx, JSValueConst pattern, JSValueConst flags, bool can_yield)
{
    int re_flags;

    if (js_regexp_parse_flags(ctx, flags, &re_flags) < 0)
        return NULL;
    return js_regexp_compile_begin(ctx, pattern, re_flags, can_yield);
}

/* create a RegExp object from a string containing the RegExp bytecode
   and the source pattern */
static JSValue js_regexp_constructor_internal(JSContext *ctx, JSValueConst ctor,
                                              JSValue pattern, JSValue bc)
{
    JSValue obj;
    JSObject *p;
    JSRegExp *re;
    JSProperty prop;

    /* sanity check */
    if (JS_VALUE_GET_TAG(bc) != JS_TAG_STRING ||
        JS_VALUE_GET_TAG(pattern) != JS_TAG_STRING) {
        JS_ThrowTypeError(ctx, "string expected");
        goto fail;
    }
    prop.u.value = js_int32(0); // lastIndex
    if (ctx->regexp_shape && JS_IsUndefined(ctor)) {
        obj = JS_NewObjectFromShape(ctx, js_dup_shape(ctx->regexp_shape),
                                    JS_CLASS_REGEXP, &prop);
        if (JS_IsException(obj))
            goto fail;
    } else {
        obj = js_create_from_ctor(ctx, ctor, JS_CLASS_REGEXP);
        if (JS_IsException(obj))
            goto fail;
        JS_DefinePropertyValue(ctx, obj, JS_ATOM_lastIndex, prop.u.value,
                               JS_PROP_WRITABLE);
    }
    p = JS_VALUE_GET_OBJ(obj);
    re = &p->u.regexp;
    re->pattern = JS_VALUE_GET_STRING(pattern);
    re->bytecode = JS_VALUE_GET_STRING(bc);
    /* the ctor-less form is %RegExp% building its own instance (a literal, or @@split's internal splitter) */
    re->legacy_features_enabled = JS_IsUndefined(ctor);
    return obj;
fail:
    JS_FreeValue(ctx, bc);
    JS_FreeValue(ctx, pattern);
    return JS_EXCEPTION;
}

static JSRegExp *js_get_regexp(JSContext *ctx, JSValueConst obj,
                               bool throw_error)
{
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        if (p->class_id == JS_CLASS_REGEXP) {
            /* the half-built state the finalizer tolerates must never ESCAPE its constructor: every consumer
               reached through here reads the bytecode. */
            DCHECK(p->u.regexp.pattern && p->u.regexp.bytecode,
                   "a RegExp object was observed before its source was compiled");
            return &p->u.regexp;
        }
    }
    if (throw_error) {
        JS_ThrowTypeErrorInvalidClass(ctx, JS_CLASS_REGEXP);
    }
    return NULL;
}

/* return < 0 if exception or true/false */
static int js_is_regexp(JSContext *ctx, JSValueConst obj)
{
    JSValue m;

    if (!JS_IsObject(obj))
        return false;
    m = JS_GetProperty(ctx, obj, JS_ATOM_Symbol_match);
    if (JS_IsException(m))
        return -1;
    if (!JS_IsUndefined(m))
        return JS_ToBoolFree(ctx, m);
    return js_get_regexp(ctx, obj, false) != NULL;
}

/* 22.2.4.1 RegExp ( pattern, flags ) — a step machine. js_regexp_constructor ran all of it from C, so a
   `@@match`, `constructor`, `source` or `flags` accessor, a `prototype` trap on a NewTarget, or a `toString` on
   the pattern or the flags, each with a loop in it, had no flow base and aborted at its back-edge.
   It also created the object LAST, after both ToStrings. Step 7's RegExpAlloc — which reads
   `? Get(newTarget, "prototype")` — comes BEFORE RegExpInitialize's coercions in step 8, and
   `Reflect.construct(RegExp, [p, f], nt)` with accessors on all three observes the difference. */
/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). The IsRegExp stage
   was numbered 9 - out of sequence, appended after the rest - which is the workaround the single declaration
   removes: a parked machine holds its stage's LABEL, so the list is in the standard's order. */
#define RECTOR_STAGES(X) \
    X(RECTOR_ENTRY,    "22.2.4.1, entered: a non-object pattern settles step 1 with no read at all") \
    X(RECTOR_ISREGEXP, "22.2.4.1 step 1 -> 7.2.6 IsRegExp step 2 (matcher is Get(argument, %Symbol.match%); " \
                       "only undefined falls back to the [[RegExpMatcher]] slot)") \
    X(RECTOR_CTOR,     "22.2.4.1 steps 2-6 (a plain call takes the active function object as newTarget and, " \
                       "for a RegExp pattern with no flags, reads Get(pattern, \"constructor\") to decide step " \
                       "2.b.ii; P and F otherwise come from the slots or the arguments)") \
    X(RECTOR_SOURCE,   "22.2.4.1 step 5.a (P is Get(pattern, \"source\"))") \
    X(RECTOR_FLAGS,    "22.2.4.1 steps 5.b-5.c (F is flags, or Get(pattern, \"flags\") when it is undefined)") \
    X(RECTOR_ALLOC,    "22.2.4.1 step 7 -> 22.2.3.2 RegExpAlloc (OrdinaryCreateFromConstructor(newTarget, " \
                       "\"%RegExp.prototype%\"), whose Get(newTarget, \"prototype\") runs here; lastIndex is " \
                       "defined after it)") \
    X(RECTOR_PATSTR,   "22.2.4.1 step 8 -> 22.2.3.3 RegExpInitialize steps 1-2 (P is ToString(pattern), or the " \
                       "empty String when it is undefined)") \
    X(RECTOR_FLAGSTR,  "22.2.4.1 step 8 -> 22.2.3.3 RegExpInitialize steps 3-11 (F is ToString(flags), or the " \
                       "empty String when it is undefined, and its code units are validated)") \
    X(RECTOR_COMPILE,  "22.2.4.1 step 8 -> 22.2.3.3 RegExpInitialize steps 12-22 (ParsePattern parses P, resting " \
                       "between Terms, and CompilePattern builds the matcher the object adopts)")
enum { RECTOR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_re_ctor_steps[] = { RECTOR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_re_ctor_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReCtor *s = st;
    JSValueConst pat = step_arg(&s->hdr, 0);
    JSValue str;
    JSRegExp *re;
    int r;

    if (s->hdr.stage == RECTOR_ENTRY) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->pat = JS_UNDEFINED; s->flg = JS_UNDEFINED; s->obj = JS_UNDEFINED;
        s->bc = JS_UNDEFINED; s->result = JS_UNDEFINED; s->ntgt = JS_UNDEFINED;
        s->comp = NULL; s->re_flags = 0;
        s->patIsRe = 0;
        s->haveFlags = !JS_IsUndefined(step_arg(&s->hdr, 1));
        /* 22.2.4.1 OVER AN UNKNOWN PATTERN. A pattern this engine was never given cannot be compiled — there is
           no matcher to build and no bytecode to hold — so the constructor answers with an unknown derived from
           the source. `new RegExp(userInput)` is how a bundle builds a filter out of a parameter, and it ended
           the document at the coercion below. */
        {
            JSValue c = js_concolic_derive(ctx, step_arg(&s->hdr, 0), "RegExp", JS_UNDEFINED);
            if (!JS_IsUninitialized(c)) { s->result = c; return 0; }
        }
        /* A non-object is never a RegExp and reads nothing; anything else asks. The read is its OWN stage
           because this one opens by discarding cb_result — a suspension inside it would land back here and
           throw away the very value the read produced. */
        s->hdr.stage = JS_IsObject(pat) ? RECTOR_ISREGEXP : RECTOR_CTOR;
    }
    if (s->hdr.stage == RECTOR_ISREGEXP) {
        /* step 1, 7.2.6 IsRegExp: `? Get(pattern, @@match)` decides it unless undefined, when the
           [[RegExpMatcher]] slot does. */
        JSValue m;
        r = step_getprop_run(ctx, &s->hdr, pat, JS_ATOM_Symbol_match, cb_result, &m, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (JS_IsUndefined(m))
            s->patIsRe = (js_get_regexp(ctx, pat, false) != NULL);
        else
            s->patIsRe = JS_ToBool(ctx, m);
        JS_FreeValue(ctx, m);
        s->hdr.stage = RECTOR_CTOR;
    }
    if (s->hdr.stage == RECTOR_CTOR) {
        /* step 2: called as a FUNCTION. `RegExp(re)` with no flags hands back `re` itself when its own
           `constructor` is this very RegExp — and that read is the page's. */
        if (JS_IsUndefined(s->hdr.this_val)) {
            /* step 2.a: NewTarget is the active function object — which the header already captured as the
               callee, so there is no interpreter frame to ask (and inside a machine there would be none). */
            if (JS_IsUndefined(s->ntgt))
                s->ntgt = js_dup(s->hdr.func_obj);
            if (s->patIsRe && !s->haveFlags) {
                JSValue ctor;
                r = step_getprop_run(ctx, &s->hdr, pat, JS_ATOM_constructor, cb_result, &ctor, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r < 0 ? -1 : r;
                r = js_same_value(ctx, ctor, s->ntgt);
                JS_FreeValue(ctx, ctor);
                if (r) { s->result = js_dup(pat); return 0; }
            }
        } else {
            s->ntgt = js_dup(s->hdr.this_val);
        }
        /* steps 4-6: where P and F come from. A real RegExp yields its own compiled source, which is why the
           unflagged case can reuse the bytecode outright — no observable step either way. */
        re = js_get_regexp(ctx, pat, false);
        if (re) {
            s->pat = js_dup(JS_MKPTR(JS_TAG_STRING, re->pattern));
            if (!s->haveFlags) s->bc = js_dup(JS_MKPTR(JS_TAG_STRING, re->bytecode));
            else s->flg = js_dup(step_arg(&s->hdr, 1));
            s->hdr.stage = RECTOR_ALLOC;
        } else if (s->patIsRe) {
            s->hdr.stage = RECTOR_SOURCE;
        } else {
            s->pat = js_dup(pat);
            s->flg = js_dup(step_arg(&s->hdr, 1));
            s->hdr.stage = RECTOR_ALLOC;
        }
    }
    if (s->hdr.stage == RECTOR_SOURCE) {
        r = step_getprop_run(ctx, &s->hdr, pat, JS_ATOM_source, cb_result, &s->pat, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = RECTOR_FLAGS;
    }
    if (s->hdr.stage == RECTOR_FLAGS) {
        if (s->haveFlags) {
            s->flg = js_dup(step_arg(&s->hdr, 1));
        } else {
            r = step_getprop_run(ctx, &s->hdr, pat, JS_ATOM_flags, cb_result, &s->flg, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
        }
        s->hdr.stage = RECTOR_ALLOC;
    }
    if (s->hdr.stage == RECTOR_ALLOC) {
        /* step 7, RegExpAlloc: `? Get(newTarget, "prototype")`, BEFORE step 8's coercions. The shape fast path
           is only taken when there is no NewTarget to ask, so it reads nothing either. */
        if (JS_IsUndefined(s->ntgt) && ctx->regexp_shape) {
            JSProperty prop;
            prop.u.value = js_int32(0);   /* lastIndex */
            s->obj = JS_NewObjectFromShape(ctx, js_dup_shape(ctx->regexp_shape), JS_CLASS_REGEXP, &prop);
            if (JS_IsException(s->obj)) { s->obj = JS_UNDEFINED; return -1; }
        } else {
            r = step_create_from_ctor_run(ctx, &s->hdr, s->ntgt, JS_CLASS_REGEXP, cb_result, &s->obj,
                                          out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            if (JS_DefinePropertyValue(ctx, s->obj, JS_ATOM_lastIndex, js_int32(0), JS_PROP_WRITABLE) < 0)
                return -1;
        }
        s->hdr.stage = RECTOR_PATSTR;
    }
    if (s->hdr.stage == RECTOR_PATSTR) {
        /* step 8, RegExpInitialize step 1: `? ToString(P)`, or "" when P is undefined */
        if (JS_IsUndefined(s->pat)) {
            s->pat = js_empty_string(ctx->rt);
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        } else {
            r = step_tostring_run(ctx, &s->hdr, s->pat, cb_result, &str, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->pat);
            s->pat = str;
        }
        s->hdr.stage = RECTOR_FLAGSTR;
    }
    if (s->hdr.stage == RECTOR_FLAGSTR) {
        /* RegExpInitialize steps 3-11: `? ToString(F)`, then the flags are read. A reused matcher needs neither. */
        if (!JS_IsUndefined(s->bc)) {
            JS_FreeValue(ctx, cb_result);
        } else {
            if (!JS_IsUndefined(s->flg)) {
                r = step_tostring_run(ctx, &s->hdr, s->flg, cb_result, &str, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r < 0 ? -1 : r;
                JS_FreeValue(ctx, s->flg);
                s->flg = str;
            } else {
                JS_FreeValue(ctx, cb_result);
            }
            if (js_regexp_parse_flags(ctx, s->flg, &s->re_flags) < 0)
                return -1;
        }
        cb_result = JS_UNDEFINED;
        s->hdr.stage = RECTOR_COMPILE;
    }
    DCHECK(s->hdr.stage == RECTOR_COMPILE, "the RegExp machine resumed in an unknown stage");
    /* steps 12-22: the parse rests between Terms; a re-entry from that rest is delivered undefined */
    JS_FreeValue(ctx, cb_result);
    if (JS_IsUndefined(s->bc)) {
        r = step_regexp_compile_run(ctx, &s->comp, s->pat, s->re_flags, &s->bc);
        if (r) return r;
    }
    DCHECK(JS_VALUE_GET_TAG(s->bc) == JS_TAG_STRING && JS_VALUE_GET_TAG(s->pat) == JS_TAG_STRING,
           "the RegExp machine reached its initialisation without a compiled source");
    {
        JSObject *p = JS_VALUE_GET_OBJ(s->obj);
        p->u.regexp.pattern = JS_VALUE_GET_STRING(s->pat);
        p->u.regexp.bytecode = JS_VALUE_GET_STRING(s->bc);
        /* [[LegacyFeaturesEnabled]] is true only when %RegExp% ITSELF is the NewTarget. A subclass instance
           must not be able to write the intrinsic's static slots — `class R extends RegExp {}` exists in
           real bundles, and the proposal's whole safety argument is that it cannot reach `RegExp.$1`. */
        p->u.regexp.legacy_features_enabled = js_same_value(ctx, s->ntgt, ctx->regexp_ctor);
        s->pat = JS_UNDEFINED;   /* the object adopts both */
        s->bc = JS_UNDEFINED;
        s->result = s->obj;
        s->obj = JS_UNDEFINED;
    }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_re_ctor_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReCtor *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->pat);
    v->val(ctx, &s->flg);
    v->val(ctx, &s->obj);
    v->val(ctx, &s->bc);
    v->val(ctx, &s->ntgt);
    v->cache(ctx, &s->comp, js_regexp_compile_cache_destroy);
}

static JSValue js_re_ctor_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReCtor *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* B.2.4.1 RegExp.prototype.compile as a STEP MACHINE. RegExpInitialize coerces BOTH of its arguments —
   `? ToString(pattern)` then `? ToString(flags)` — and for an OBJECT argument each is the page's
   @@toPrimitive/valueOf/toString. The C body ran the first with JS_ToString and the second inside
   js_compile_regexp's JS_ToCStringLen, so `re.compile({toString(){ while(x){} }})` had no flow base. Compiling
   the pattern and writing the two slots invokes nothing, and `lastIndex` is a non-configurable data property on
   every RegExp instance, so its Set cannot be an accessor either. */
typedef struct JSRegExpCompile {
    JSStepHdr hdr;      /* MUST be first */
    JSValue result;     /* owned: the receiver */
    JSValue pattern;    /* the coerced source, held across the FLAGS coercion and the compile (owned) */
    void *comp;         /* the compile in flight (REParseState), a cache: shed on fork, begun again */
    int re_flags;       /* F read into LRE flags, held across the compile's rests */
} JSRegExpCompile;
_Static_assert(offsetof(JSRegExpCompile, hdr) == 0, "JSStepHdr must be first in JSRegExpCompile");

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). */
#define RCOMPILE_STAGES(X) \
    X(RC_ENTRY,   "B.2.4.1 steps 1-3 (O is the this value; RequireInternalSlot(O, [[RegExpMatcher]]); the " \
                  "proposal-regexp-legacy-features guards; P and F from a RegExp pattern's own slots or from " \
                  "the arguments)") \
    X(RC_PATTERN, "B.2.4.1 step 4 -> 22.2.3.3 RegExpInitialize steps 1-2 (P is ToString(pattern))") \
    X(RC_FLAGS,   "B.2.4.1 step 4 -> 22.2.3.3 RegExpInitialize steps 3-11 (F is ToString(flags), or the " \
                  "empty String when it is undefined, and its code units are validated)") \
    X(RC_COMPILE, "B.2.4.1 step 4 -> 22.2.3.3 RegExpInitialize steps 12-16 and 19-22 (ParsePattern parses P, " \
                  "resting between Terms, and CompilePattern builds the matcher)") \
    X(RC_BUILD,   "B.2.4.1 step 4 -> 22.2.3.3 RegExpInitialize steps 17-18 and 23 (the receiver is re-validated, " \
                  "its slots are rewritten and Set(obj, \"lastIndex\", +0, true) runs)")
enum { RCOMPILE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_regexp_compile_steps[] = { RCOMPILE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_regexp_compile_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRegExpCompile *m = st;
    JSValueConst this_val = m->hdr.this_val;
    JSValueConst pattern1 = step_arg(&m->hdr, 0), flags1 = step_arg(&m->hdr, 1);
    JSValue bc = JS_UNDEFINED, flags = JS_UNDEFINED;
    JSRegExp *re, *re1;
    int r;

    if (m->hdr.stage == RC_ENTRY) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        m->result = JS_UNDEFINED; m->pattern = JS_UNDEFINED;
        m->comp = NULL; m->re_flags = 0;
        /* every validation the spec performs before either coercion */
        re = js_get_regexp(ctx, this_val, true);
        if (!re) return -1;
        /* proposal-regexp-legacy-features rewrites this method's opening. compile REWRITES an existing
           object's pattern, so both of the proposal's guards protect the same thing the flag does: a page must
           not be able to take a regexp the intrinsic trusts and make it match something of its own choosing.
           A subclass instance never had [[LegacyFeaturesEnabled]], and an instance from ANOTHER realm has it
           relative to that realm's %RegExp%, not this one's — which is why the flag alone is not the test and
           [[Realm]] exists.
           "The current Realm Record" is the CALLEE's, not the caller's: `other.RegExp.prototype.compile.call(re)`
           must throw other.TypeError, and `ctx` inside a builtin is already the function's realm. */
        if (!re->legacy_features_enabled)
            return (JS_ThrowTypeError(ctx, "RegExp.prototype.compile requires a regexp %%RegExp%% itself "
                                           "constructed"), -1);
        if (re->realm != ctx)
            return (JS_ThrowTypeError(ctx, "RegExp.prototype.compile requires a regexp from the same realm"), -1);
        re1 = js_get_regexp(ctx, pattern1, false);
        if (re1) {
            /* step 2: a RegExp source is copied whole, so nothing is coerced at all. */
            if (!JS_IsUndefined(flags1))
                return (JS_ThrowTypeError(ctx, "flags must be undefined"), -1);
            m->pattern = js_dup(JS_MKPTR(JS_TAG_STRING, re1->pattern));
            bc = js_dup(JS_MKPTR(JS_TAG_STRING, re1->bytecode));
            m->hdr.stage = RC_BUILD;
            goto assign;
        }
        if (JS_IsUndefined(pattern1)) {
            m->pattern = js_empty_string(ctx->rt);
            m->hdr.stage = RC_FLAGS;
        } else {
            m->hdr.stage = RC_PATTERN;
        }
    }
    if (m->hdr.stage == RC_PATTERN) {
        r = step_tostring_run(ctx, &m->hdr, pattern1, cb_result, &m->pattern, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r;
        m->hdr.stage = RC_FLAGS;
    }
    if (m->hdr.stage == RC_FLAGS) {
        if (JS_IsUndefined(flags1)) {
            JS_FreeValue(ctx, cb_result);
        } else {
            r = step_tostring_run(ctx, &m->hdr, flags1, cb_result, &flags, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r;
        }
        r = js_regexp_parse_flags(ctx, flags, &m->re_flags);
        JS_FreeValue(ctx, flags);
        if (r < 0) return -1;
        cb_result = JS_UNDEFINED;
        m->hdr.stage = RC_COMPILE;
    }
    if (m->hdr.stage == RC_COMPILE) {
        /* the coercions are spent; the compile reads the held string, invokes nothing, and rests between
           Terms, a re-entry from which is delivered undefined */
        JS_FreeValue(ctx, cb_result);
        r = step_regexp_compile_run(ctx, &m->comp, m->pattern, m->re_flags, &bc);
        if (r) return r;
        m->hdr.stage = RC_BUILD;
    }
assign:
    DCHECK(m->hdr.stage == RC_BUILD, "RegExp.prototype.compile resumed in an unknown stage");
    /* the receiver is re-validated: a `toString` above is the page's code and can have been handed a different
       object entirely, and js_get_regexp is the only thing that says this one still has the slots. */
    re = js_get_regexp(ctx, this_val, true);
    if (!re) { JS_FreeValue(ctx, bc); return -1; }
    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_STRING, re->pattern));
    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_STRING, re->bytecode));
    re->pattern = JS_VALUE_GET_STRING(m->pattern);   /* ownership moves into the slots */
    re->bytecode = JS_VALUE_GET_STRING(bc);
    m->pattern = JS_UNDEFINED;
    if (JS_SetProperty(ctx, this_val, JS_ATOM_lastIndex, js_int32(0)) < 0)
        return -1;
    m->result = js_dup(this_val);
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The receiver it answers with, and the coerced source it holds
   from the pattern's ToString across the FLAGS coercion — which is the page's code, so a fork can land between
   the two. */
static void js_regexp_compile_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRegExpCompile *m = st;
    v->val(ctx, &m->result);
    v->val(ctx, &m->pattern);
    v->cache(ctx, &m->comp, js_regexp_compile_cache_destroy);
}

static JSValue js_regexp_compile_fini(JSContext *ctx, void *st, bool take_result)
{
    JSRegExpCompile *m = st;
    JSValue r = take_result ? m->result : JS_UNDEFINED;
    if (take_result) m->result = JS_UNDEFINED;
    return r;
}

static const JSTrampStepDef js_regexp_compile_def = {
    sizeof(JSRegExpCompile), js_regexp_compile_step, js_regexp_compile_fini, 0, .visit = js_regexp_compile_visit,
    .algorithm = "B.2.4.1 RegExp.prototype.compile", .steps = js_regexp_compile_steps };

/* Write `c` as an escape if it is a LineTerminator, and say whether it did. The two-code-unit ones keep the
   spelling a reader expects; U+2028 and U+2029 have no short form and take the six-unit one. */
static bool js_regexp_escape_line_terminator(StringBuffer *b, int c)
{
    switch (c) {
    case '\n':   string_buffer_puts8(b, "\\n"); return true;
    case '\r':   string_buffer_puts8(b, "\\r"); return true;
    case 0x2028: string_buffer_puts8(b, "\\u2028"); return true;
    case 0x2029: string_buffer_puts8(b, "\\u2029"); return true;
    default:     return false;
    }
}

static JSValue js_regexp_get_source(JSContext *ctx, JSValueConst this_val)
{
    JSRegExp *re;
    JSString *p;
    StringBuffer b_s, *b = &b_s;
    int i, n, c, c2, bra;

    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);

    if (js_same_value(ctx, this_val, ctx->class_proto[JS_CLASS_REGEXP]))
        goto empty_regex;

    re = js_get_regexp(ctx, this_val, true);
    if (!re)
        return JS_EXCEPTION;

    p = re->pattern;

    if (p->len == 0) {
    empty_regex:
        return js_new_string8(ctx, "(?:)");
    }
    string_buffer_init2(ctx, b, p->len, p->is_wide_char);

    /* 22.2.6.13.1 EscapeRegExpPattern: the result must be re-readable as the body of a RegularExpressionLiteral
       with the same meaning. A RegularExpressionLiteral admits no LineTerminator ANYWHERE — not at the top
       level, not inside a class, and not after a backslash — so all four of them are escaped, and `/` is
       escaped outside a class (inside one it is an ordinary RegularExpressionClassChar).

       Two of the three were missing. U+2028 and U+2029 are LineTerminators and went through raw, so
       `new RegExp("\u2028").source` produced a string that cannot be parsed back. And the backslash arm copied
       the next code unit VERBATIM, so an escaped line terminator stayed raw as well: `\<LF>` came out as a
       backslash and a real newline. Neither is expressible as the (c, c2) pair this loop used to emit, which is
       what the pair cost — an escape here is up to six code units, so the emit says so. */
    bra = 0;
    for (i = 0, n = p->len; i < n;) {
        c2 = -1;
        switch (c = string_get(p, i++)) {
        case '\\':
            if (i < n) {
                c2 = string_get(p, i++);
                /* `\<LineTerminator>` is an IdentityEscape for that terminator — the same character the raw
                   spelling denotes — so the PAIR is re-escaped as one, and the backslash is not re-emitted.
                   Emitting it as well would produce `\\u2028`, which reads as a literal backslash. */
                if (js_regexp_escape_line_terminator(b, c2))
                    continue;
            }
            break;
        case ']':
            bra = 0;
            break;
        case '[':
            if (!bra) {
                if (i < n && string_get(p, i) == ']')
                    c2 = string_get(p, i++);
                bra = 1;
            }
            break;
        case '/':
            if (!bra) {
                c = '\\';
                c2 = '/';
            }
            break;
        default:
            if (js_regexp_escape_line_terminator(b, c))
                continue;
            break;
        }
        string_buffer_putc16(b, c);
        if (c2 >= 0)
            string_buffer_putc16(b, c2);
    }
    return string_buffer_end(b);
}

static JSValue js_regexp_get_flag(JSContext *ctx, JSValueConst this_val, int mask)
{
    JSRegExp *re;
    int flags;

    if (JS_VALUE_GET_TAG(this_val) != JS_TAG_OBJECT)
        return JS_ThrowTypeErrorNotAnObject(ctx);

    re = js_get_regexp(ctx, this_val, false);
    if (!re) {
        if (js_same_value(ctx, this_val, ctx->class_proto[JS_CLASS_REGEXP]))
            return JS_UNDEFINED;
        else
            return JS_ThrowTypeErrorInvalidClass(ctx, JS_CLASS_REGEXP);
    }

    flags = lre_get_flags(str8(re->bytecode));
    return js_bool(flags & mask);
}

/* 22.2.6.4 get RegExp.prototype.flags, AS THE SPEC NUMBERS IT. Its eight reads are eight SEPARATE steps of the
   algorithm, in a fixed order the page can observe, so they are eight stages and not one walk with a private
   cursor — a flow parked on the `unicode` getter names that getter, and the ToBoolean beside each read invokes
   nothing, which is why each stage carries its `if` with it.
   The stage IS the index: js_regexp_flag_names[stage - 1] is the flag this stage reads, so the list below and
   that table cannot disagree without the DCHECK in the loop saying so. */
#define REFL_STAGES(X) \
    X(REFL_RECV,        "22.2.6.4 steps 1-3 (R is the this value; the not-an-Object TypeError; codeUnits)") \
    X(REFL_HASINDICES,  "22.2.6.4 steps 4-5 (hasIndices is ToBoolean(Get(R, \"hasIndices\")); append \"d\")") \
    X(REFL_GLOBAL,      "22.2.6.4 steps 6-7 (global is ToBoolean(Get(R, \"global\")); append \"g\")") \
    X(REFL_IGNORECASE,  "22.2.6.4 steps 8-9 (ignoreCase is ToBoolean(Get(R, \"ignoreCase\")); append \"i\")") \
    X(REFL_MULTILINE,   "22.2.6.4 steps 10-11 (multiline is ToBoolean(Get(R, \"multiline\")); append \"m\")") \
    X(REFL_DOTALL,      "22.2.6.4 steps 12-13 (dotAll is ToBoolean(Get(R, \"dotAll\")); append \"s\")") \
    X(REFL_UNICODE,     "22.2.6.4 steps 14-15 (unicode is ToBoolean(Get(R, \"unicode\")); append \"u\")") \
    X(REFL_UNICODESETS, "22.2.6.4 steps 16-17 (unicodeSets is ToBoolean(Get(R, \"unicodeSets\")); append \"v\")") \
    X(REFL_STICKY,      "22.2.6.4 steps 18-19 (sticky is ToBoolean(Get(R, \"sticky\")); append \"y\")") \
    X(REFL_RESULT,      "22.2.6.4 step 20 (the String whose code units are codeUnits)")
enum { REFL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_regexp_flags_steps[] = { REFL_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_regexp_flags_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRegExpFlags *s = st;
    int r;

    if (s->hdr.stage == REFL_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->result = JS_UNDEFINED;
        s->n = 0;
        /* 22.2.6.4 step 2: a non-object receiver is a TypeError before any Get happens. */
        if (!JS_IsObject(s->hdr.this_val)) {
            JS_ThrowTypeErrorNotAnObject(ctx);
            return -1;
        }
        s->hdr.stage = REFL_HASINDICES;
    }
    while (s->hdr.stage < REFL_RESULT) {
        int i = s->hdr.stage - REFL_HASINDICES, b;
        DCHECK(i >= 0 && i < 8, "the flags getter rested at a stage that names no flag");
        JSValue got = JS_UNDEFINED;
        JSAtom a = JS_NewAtom(ctx, js_regexp_flag_names[i]);
        if (unlikely(a == JS_ATOM_NULL)) { JS_FreeValue(ctx, cb_result); return -1; }
        /* interning the name runs nothing; the GET is the page's code and is the request. */
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, a, cb_result, &got, out_cb, out_argc);
        JS_FreeAtom(ctx, a);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        b = JS_ToBoolFree(ctx, got);   /* ToBoolean invokes nothing */
        if (unlikely(b < 0)) return -1;
        if (b)
            s->buf[s->n++] = js_regexp_flag_chars[i];
        s->hdr.stage++;
    }
    JS_FreeValue(ctx, cb_result);
    s->result = s->n ? js_new_string8_len(ctx, s->buf, s->n) : js_empty_string(ctx->rt);
    if (unlikely(JS_IsException(s->result))) { s->result = JS_UNDEFINED; return -1; }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Each flag is a Get the page can define an accessor for. */
static void js_regexp_flags_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRegExpFlags *s = st;
    v->val(ctx, &s->result);
}

static JSValue js_regexp_flags_fini(JSContext *ctx, void *st, bool take_result)
{
    JSRegExpFlags *s = st;
    JSValue r = take_result ? js_dup(s->result) : JS_UNDEFINED;
    return r;
}

/* 22.2.6.17 RegExp.prototype.toString, AS THE SPEC NUMBERS IT. Two stages may not name one step, so the Get and
   the ToString that consumes it each say which half they are — the machine rests at both, and a resume that
   could not tell them apart would re-read the value it just got. */
#define RETS_STAGES(X) \
    X(RETS_RECV,    "22.2.6.17 steps 1-2 (R is the this value; the not-an-Object TypeError)") \
    X(RETS_SOURCE,  "22.2.6.17 step 3 (Get(R, \"source\"))") \
    X(RETS_PATTERN, "22.2.6.17 step 3 (pattern is ToString of it)") \
    X(RETS_FLAGS,   "22.2.6.17 step 4 (Get(R, \"flags\"))") \
    X(RETS_FLAGSTR, "22.2.6.17 step 4 (flags is ToString of it)") \
    X(RETS_RESULT,  "22.2.6.17 steps 5-6 (the string-concatenation of \"/\", pattern, \"/\" and flags)")
enum { RETS_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_regexp_tostring_steps[] = { RETS_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_regexp_tostring_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRegExpToString *s = st;
    int r;

    if (s->hdr.stage == RETS_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->result = JS_UNDEFINED;
        s->pattern = JS_UNDEFINED;
        s->flags = JS_UNDEFINED;
        if (!JS_IsObject(s->hdr.this_val)) {   /* step 2 */
            JS_ThrowTypeErrorNotAnObject(ctx);
            return -1;
        }
        s->hdr.stage = RETS_SOURCE;
    }
    if (s->hdr.stage == RETS_SOURCE) {
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_source, cb_result, &s->pattern,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = RETS_PATTERN;
    }
    if (s->hdr.stage == RETS_PATTERN) {
        JSValue str = JS_UNDEFINED;
        r = step_tostring_run(ctx, &s->hdr, s->pattern, cb_result, &str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->pattern);
        s->pattern = str;
        s->hdr.stage = RETS_FLAGS;
    }
    if (s->hdr.stage == RETS_FLAGS) {
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_flags, cb_result, &s->flags,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = RETS_FLAGSTR;
    }
    if (s->hdr.stage == RETS_FLAGSTR) {
        JSValue str = JS_UNDEFINED;
        r = step_tostring_run(ctx, &s->hdr, s->flags, cb_result, &str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->flags);
        s->flags = str;
        s->hdr.stage = RETS_RESULT;
    }
    JS_FreeValue(ctx, cb_result);
    {   /* step 5: assembling the three pieces invokes nothing. */
        StringBuffer b_s, *b = &b_s;
        string_buffer_init(ctx, b, 0);
        string_buffer_putc8(b, '/');
        if (string_buffer_concat_value(b, s->pattern)) { string_buffer_free(b); return -1; }
        string_buffer_putc8(b, '/');
        if (string_buffer_concat_value(b, s->flags)) { string_buffer_free(b); return -1; }
        s->result = string_buffer_end(b);
        if (unlikely(JS_IsException(s->result))) { s->result = JS_UNDEFINED; return -1; }
    }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_regexp_tostring_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRegExpToString *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->pattern);
    v->val(ctx, &s->flags);
}

static JSValue js_regexp_tostring_fini(JSContext *ctx, void *st, bool take_result)
{
    JSRegExpToString *s = st;
    JSValue r = take_result ? js_dup(s->result) : JS_UNDEFINED;
    return r;
}

/* Should a running match give the thread back at its next back-edge? It REPLACES lre_check_timeout, which
   asked the embedder's interrupt handler every 10000 opcodes and TRUNCATED the match — a watchdog, which is a
   bound. The scheduler answers instead, and the match parks and resumes at the exact opcode. */
/* A regexp back-edge is a unit of engine work, as a dispatch is: retired before the policy is asked, so a
   backtracking pattern spends the slice's budget like any other stretch of the flow's path. */
int lre_want_yield(void *opaque)
{
    (void)opaque;
    DCHECK_PRICED("an assert's condition or a dev build's second derivation ran a regexp match — its back-edges "
                  "are work and a rest a release build does not take. Compare the bytes in C");
    FLOW_WORK_RETIRE();
    return g_flow_control.preempt != NULL && g_flow_control.preempt(JS_PREEMPT_BACKEDGE);
}

/* A compile's input is host work in the opcode unit, as the script tokenizer's is (js_parse_count_bytes), and
   its rest point asks the policy the matcher's back-edge asks. Named residual. Not covered: the price per
   pattern byte is the script tokenizer's rate, not one read off a census of the regexp parser's own CPU.
   Next diff builds: a regexp-compile kind beside the script parse in the thread work census, priced from it.
   Absence shows as: the census's per-unit CPU on regexp-heavy steps drifting from its per-opcode price. */
int lre_compile_poll(void *opaque, size_t bytes, bool ask)
{
    (void)opaque;
    DCHECK_PRICED("an assert's condition or a dev build's second derivation compiled a regexp — its pattern bytes "
                  "are work a release build does not retire. Check the compiled program, not a second compile");
    g_regexp_compile_bytes += bytes;
    /* A literal's compile inside a script's compile stint is that compile's work, as its tokenizer's is. */
    if (g_compile_stint) {
        js_compile_charge((uint64_t)bytes * g_flow_control.parse_units_per_byte);
    } else {
        g_flow_work_retired += (uint64_t)bytes * g_flow_control.parse_units_per_byte;
        js_flow_stand_retire((uint64_t)bytes * g_flow_control.parse_units_per_byte);
    }
    return ask && g_flow_control.preempt != NULL && g_flow_control.preempt(JS_PREEMPT_BACKEDGE);
}

void *lre_realloc(void *opaque, void *ptr, size_t size)
{
    JSContext *ctx = opaque;
    /* No JS exception is raised here */
    return js_realloc_rt(ctx->rt, ptr, size);
}

static JSValue js_regexp_escape(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    StringBuffer b_s, *b = &b_s;
    JSValue str, ret;
    JSString *p;
    uint32_t c, i;
    int idx;
    char s[16];

    if (!JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "not a string");
    if (JS_VALUE_GET_TAG(argv[0]) == JS_TAG_STRING_ROPE) {
        str = js_linearize_string_rope(ctx, argv[0]);
        if (JS_IsException(str))
            return JS_EXCEPTION;
    } else {
        str = js_dup(argv[0]);
    }
    p = JS_VALUE_GET_STRING(str);
    string_buffer_init2(ctx, b, 0, p->is_wide_char);
    /* 22.2.2.2 step 3 is `StringToCodePoints(S)`, so the walk is over CODE POINTS. It was over code UNITS, and
       the difference is exactly a valid surrogate PAIR: as two units each looks like a lone surrogate and got
       \u-escaped by EncodeForRegExpEscape step 5, so `RegExp.escape("\u{10000}")` answered "\ud800\udc00"
       where the spec answers the character itself. A LONE surrogate is still a code point in the surrogate
       range and is still escaped there — that test does not change, it just now asks about a code point. */
    for (idx = 0; idx < (int)p->len; ) {
        i = (uint32_t)idx;
        c = (uint32_t)string_getc(p, &idx);
        if (c < 33) {
            if (c >= 9 && c <= 13) {
                string_buffer_putc8(b, '\\');
                string_buffer_putc8(b, "tnvfr"[c - 9]);
            } else {
                goto hex2;
            }
        } else if (c < 128) {
            if ((c >= '0' && c <= '9')
             || (c >= 'A' && c <= 'Z')
             || (c >= 'a' && c <= 'z')) {
                if (i == 0)
                    goto hex2;
            } else if (strchr(",-=<>#&!%:;@~'`\"", c)) {
                goto hex2;
            } else if (c != '_') {
                string_buffer_putc8(b, '\\');
            }
            string_buffer_putc8(b, c);
        } else if (c < 256) {
        hex2:
            snprintf(s, sizeof(s), "\\x%02x", c);
            string_buffer_puts8(b, s);
        } else if (is_surrogate(c) || lre_is_white_space(c) || c == 0xFEFF) {
            /* step 5.c-d: a code point above the BMP would be TWO \u escapes, but nothing above the BMP
               reaches here — a supplementary code point is neither a surrogate nor whitespace. */
            snprintf(s, sizeof(s), "\\u%04x", c);
            string_buffer_puts8(b, s);
        } else {
            string_buffer_putc(b, c);   /* step 6: UTF16EncodeCodePoint, which re-pairs a supplementary one */
        }
    }
    ret = string_buffer_end(b);
    JS_FreeValue(ctx, str);
    return ret;
}

/* 22.2.7.2 steps 17-34: the match RESULT. Every object it touches — the result array, `groups`, `indices` and
   their entries — is created here, so every define is on a fresh ordinary object and reaches no page code at
   all. That is why this half stays a plain C body while the three property accesses around it became requests.
   `str_val` is CONSUMED (it becomes the result's `input`), and `bytecode` is BORROWED from the state, which
   holds a reference for exactly this call. */
/* InvalidateLegacyRegExpStaticProperties: every slot goes back to `empty`, so every getter throws until a
   legacy regexp matches again. A NON-legacy regexp (a subclass instance, or one whose prototype was swapped)
   matching is what triggers it — the proposal's point being that a page cannot use a subclass to smuggle its
   own values into an intrinsic every other script can read. */
static void js_regexp_legacy_invalidate(JSContext *ctx)
{
    int i;
    for (i = 0; i < RE_LEGACY_COUNT; i++) {
        JS_FreeValue(ctx, ctx->regexp_legacy[i]);
        ctx->regexp_legacy[i] = JS_UNINITIALIZED;
    }
}

static void js_regexp_legacy_set(JSContext *ctx, int slot, JSValue v)
{
    JS_FreeValue(ctx, ctx->regexp_legacy[slot]);
    ctx->regexp_legacy[slot] = v;
}

/* UpdateLegacyRegExpStaticProperties. `capture` holds the match's byte pointers into str_buf, which is what
   the caller already has; every slot is a substring of S, so this allocates strings and runs nothing. */
static int js_regexp_legacy_update(JSContext *ctx, JSValueConst str_val, uint8_t **capture,
                                   int capture_count, const uint8_t *str_buf, int shift)
{
    JSString *str = JS_VALUE_GET_STRING(str_val);
    int i, start, end, last_paren = -1;
    JSValue v;

    start = (int)((capture[0] - str_buf) >> shift);
    end = (int)((capture[1] - str_buf) >> shift);

    js_regexp_legacy_set(ctx, RE_LEGACY_INPUT, js_dup(str_val));
    v = js_sub_string(ctx, str, start, end);
    if (JS_IsException(v)) return -1;
    js_regexp_legacy_set(ctx, RE_LEGACY_LAST_MATCH, v);
    v = js_sub_string(ctx, str, 0, start);
    if (JS_IsException(v)) return -1;
    js_regexp_legacy_set(ctx, RE_LEGACY_LEFT_CONTEXT, v);
    v = js_sub_string(ctx, str, end, str->len);
    if (JS_IsException(v)) return -1;
    js_regexp_legacy_set(ctx, RE_LEGACY_RIGHT_CONTEXT, v);

    /* $1..$9 are the first nine captures; a group that did not participate is the empty string, not
       undefined — these are the legacy accessors, and they predate `groups`. */
    for (i = 1; i <= 9; i++) {
        if (i < capture_count && capture[2 * i] && capture[2 * i + 1]) {
            int cs = (int)((capture[2 * i] - str_buf) >> shift);
            int ce = (int)((capture[2 * i + 1] - str_buf) >> shift);
            v = js_sub_string(ctx, str, cs, ce);
            if (JS_IsException(v)) return -1;
        } else {
            v = JS_AtomToString(ctx, JS_ATOM_empty_string);
        }
        js_regexp_legacy_set(ctx, RE_LEGACY_PAREN1 + i - 1, v);
    }
    /* [[RegExpLastParen]] is the LAST capture that participated, over all of them and not just the first
       nine — `lastParen` on a pattern with twelve groups reports the twelfth. */
    for (i = 1; i < capture_count; i++) {
        if (capture[2 * i] && capture[2 * i + 1])
            last_paren = i;
    }
    if (last_paren > 0) {
        int cs = (int)((capture[2 * last_paren] - str_buf) >> shift);
        int ce = (int)((capture[2 * last_paren + 1] - str_buf) >> shift);
        v = js_sub_string(ctx, str, cs, ce);
        if (JS_IsException(v)) return -1;
    } else {
        v = JS_AtomToString(ctx, JS_ATOM_empty_string);
    }
    js_regexp_legacy_set(ctx, RE_LEGACY_LAST_PAREN, v);
    return 0;
}

static JSValue js_regexp_build_result(JSContext *ctx, JSString *bytecode, JSValue str_val,
                                      uint8_t **capture)
{
    int capture_count, shift, index, i, re_flags, prop_flags;
    JSString *str;
    JSValue t, ret, obj, groups;
    JSValue indices, indices_groups;
    uint8_t *re_bytecode;
    uint8_t *str_buf;
    const char *group_name_ptr;
    JSAtom group_name;
    JSProperty props[4]; // length, index, input, groups, in that order

    ret = JS_EXCEPTION;
    obj = JS_NULL;
    groups = JS_UNDEFINED;
    indices = JS_UNDEFINED;
    indices_groups = JS_UNDEFINED;
    group_name = JS_ATOM_NULL;

    re_bytecode = str8(bytecode);
    re_flags = lre_get_flags(re_bytecode);
    capture_count = lre_get_capture_count(re_bytecode);
    str = JS_VALUE_GET_STRING(str_val);
    shift = str->is_wide_char;
    str_buf = str8(str);

    group_name_ptr = lre_get_groupnames(re_bytecode);
    if (group_name_ptr) {
        groups = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(groups))
            goto fail;
    }
    if (re_flags & LRE_FLAG_INDICES) {
        indices = JS_NewArray(ctx);
        if (JS_IsException(indices))
            goto fail;
        if (group_name_ptr) {
            indices_groups = JS_NewObjectProto(ctx, JS_NULL);
            if (JS_IsException(indices_groups))
                goto fail;
        }
    }
    index = (capture[0] - str_buf) >> shift;
    props[0].u.value = js_int32(capture_count); // length
    props[1].u.value = js_int32(index);         // index
    props[2].u.value = str_val;                 // input
    props[3].u.value = js_dup(groups);          // groups
    str_val = JS_UNDEFINED;
    obj = JS_NewObjectFromShape(ctx, js_dup_shape(ctx->regexp_result_shape),
                                JS_CLASS_ARRAY, props);
    if (JS_IsException(obj))
        goto fail;
    prop_flags = JS_PROP_C_W_E | JS_PROP_THROW;
    for(i = 0; i < capture_count; i++) {
        uint8_t **match = &capture[2 * i];
        int start = -1;
        int end = -1;

        if (group_name_ptr && i > 0) {
            if (*group_name_ptr) {
                /* XXX: slow, should create a shape when the regexp is
                   compiled */
                group_name = JS_NewAtom(ctx, group_name_ptr);
                if (group_name == JS_ATOM_NULL)
                    goto fail;
            }
            group_name_ptr += strlen(group_name_ptr) + LRE_GROUP_NAME_TRAILER_LEN;
        }

        if (match[0] && match[1]) {
            start = (match[0] - str_buf) >> shift;
            end = (match[1] - str_buf) >> shift;
        }

        if (!JS_IsUndefined(indices)) {
            JSValue val = JS_UNDEFINED;
            if (start != -1) {
                val = JS_NewArray(ctx);
                if (JS_IsException(val))
                    goto fail;
                if (JS_DefinePropertyValueUint32(ctx, val, 0,
                                                 js_int32(start),
                                                 prop_flags) < 0) {
                    JS_FreeValue(ctx, val);
                    goto fail;
                }
                if (JS_DefinePropertyValueUint32(ctx, val, 1,
                                                 js_int32(end),
                                                 prop_flags) < 0) {
                    JS_FreeValue(ctx, val);
                    goto fail;
                }
            }
            if (group_name != JS_ATOM_NULL &&
                !JS_IsUndefined(indices_groups)) {
                /* For duplicate named groups, only the alternative that
                   actually matched (non-undefined) wins; a later
                   undefined alternative must not clobber it. */
                if (!JS_IsUndefined(val) ||
                    !JS_HasProperty(ctx, indices_groups, group_name)) {
                    if (JS_DefinePropertyValue(ctx, indices_groups,
                                               group_name, js_dup(val),
                                               prop_flags) < 0) {
                        JS_FreeValue(ctx, val);
                        goto fail;
                    }
                }
            }
            if (JS_DefinePropertyValueUint32(ctx, indices, i, val,
                                             prop_flags) < 0) {
                goto fail;
            }
        }

        JSValue val = JS_UNDEFINED;
        if (start != -1) {
            val = js_sub_string(ctx, str, start, end);
            if (JS_IsException(val))
                goto fail;
        }

        if (group_name != JS_ATOM_NULL) {
            /* duplicate named groups: matched alternative wins (see above) */
            if (!JS_IsUndefined(val) ||
                !JS_HasProperty(ctx, groups, group_name)) {
                if (JS_DefinePropertyValue(ctx, groups, group_name,
                                           js_dup(val), prop_flags) < 0) {
                    JS_FreeValue(ctx, val);
                    goto fail;
                }
            }
            JS_FreeAtom(ctx, group_name);
            group_name = JS_ATOM_NULL;
        }

        if (JS_DefinePropertyValueUint32(ctx, obj, i, val, prop_flags) < 0)
            goto fail;
    }

    if (!JS_IsUndefined(indices)) {
        t = indices_groups, indices_groups = JS_UNDEFINED;
        if (JS_DefinePropertyValue(ctx, indices, JS_ATOM_groups,
                                   t, prop_flags) < 0) {
            goto fail;
        }
        t = indices, indices = JS_UNDEFINED;
        if (JS_DefinePropertyValue(ctx, obj, JS_ATOM_indices,
                                   t, prop_flags) < 0) {
            goto fail;
        }
    }
    ret = obj;
    obj = JS_UNDEFINED;
fail:
    JS_FreeAtom(ctx, group_name);
    JS_FreeValue(ctx, indices_groups);
    JS_FreeValue(ctx, indices);
    JS_FreeValue(ctx, str_val);
    JS_FreeValue(ctx, groups);
    JS_FreeValue(ctx, obj);
    return ret;
}

/* 22.2.6.2 RegExp.prototype.exec and the 22.2.7.2 RegExpBuiltinExec it tail-calls, AS THE SPEC NUMBERS THEM.
   The stages were 0,1,2,3,4 and a 40 named REX_MATCH — internally consistent and externally meaningless, so a
   flow parked here could be described only as "stage 40 of something". Each stage rests at a written step, and
   the labels are what the driver asserts against and what a parked machine reports.
   ONE LIST, EXPANDED TWICE. The constants and the labels were two lists side by side, which is the drift
   JSTrampStepDef.steps describes: renumber one and every stage after it silently names a different step, and a
   flow parked at that number resumes at the wrong one. The out-of-sequence REX_MATCH = 40 was the workaround
   for exactly that, and this removes the need for it — a stage cannot move without its label moving with it. */
#define REX_STAGES(X) \
    X(REX_REQUIRE,       "22.2.6.2 step 2 (RequireInternalSlot)") \
    X(REX_TOSTRING,      "22.2.6.2 step 3 (S is ToString(string))") \
    X(REX_GET_LASTINDEX, "22.2.7.2 step 2 (Get(R, \"lastIndex\"))") \
    X(REX_TOLENGTH,      "22.2.7.2 step 2 (ToLength of lastIndex)") \
    X(REX_FLAGS,         "22.2.7.2 steps 3-4 (flags decide global/sticky)") \
    X(REX_MATCH,         "22.2.7.2 steps 5-12 (the matcher)") \
    X(REX_SET_LASTINDEX, "22.2.7.2 steps 13-15 (Set(R, \"lastIndex\"))") \
    X(REX_RESULT,        "22.2.7.2 steps 16-28 (build the match result)") \
    X(REX_DOMAIN,        "22.2.7.2 steps 5-12 (the matcher) over every String the unknown subject's domain admits")
enum { REX_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_regexp_exec_steps[] = { REX_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §22.2.7.2 RegExpBuiltinExec over an unknown subject, for a regexp that is neither global nor sticky (whose
   lastIndex the algorithm then neither reads into the match nor writes): seeds the matcher from index 0 over
   every String the subject's domain admits (JSConcolicHooks.string_domain). 1 where seeded, 0 where the regexp
   or the subject states nothing it can run over, -1 on an allocation refused. */
static int js_regexp_exec_domain_begin(JSContext *ctx, JSRegExpExec *s)
{
    JSRegExp *re = js_get_regexp(ctx, s->hdr.this_val, true);
    const uint8_t *bc;
    uint64_t ascii[2];
    JSString *p;
    int wide = 0, alloc, r;

    if (!re) return -1;
    bc = str8(re->bytecode);
    if (lre_get_flags(bc) & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY)) return 0;
    if (!g_concolic.string_domain(ctx, step_arg(&s->hdr, 0), &s->str_val, ascii, &wide)) return 0;
    s->bytecode = js_dup(JS_MKPTR(JS_TAG_STRING, re->bytecode));
    bc = str8(JS_VALUE_GET_STRING(s->bytecode));
    alloc = lre_get_alloc_count(bc);
    if (alloc > 0) {
        s->capture = js_malloc(ctx, sizeof(s->capture[0]) * (size_t)alloc);
        if (!s->capture) return -1;
        s->cap_alloc = alloc;
    }
    p = JS_VALUE_GET_STRING(s->str_val);
    r = lre_exec_begin_prefix(&s->ec, s->capture, bc, str8(p), p->len, p->is_wide_char, ascii, wide != 0, ctx);
    if (r == LRE_RET_MEMORY_ERROR) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (r == LRE_RET_UNDECIDED) return 0;
    DCHECK(r == 0, "lre_exec_begin_prefix answered a value its contract does not name");
    s->matching = 1;
    return 1;
}

/* One slice of that matcher: where no admitted String matches, steps 12.a-b return null in every world. */
static int js_regexp_exec_domain_step(JSContext *ctx, JSRegExpExec *s)
{
    int rc = lre_exec_step(&s->ec);

    if (rc == LRE_RET_YIELD)
        return JS_STEP_YIELD;
    lre_exec_end(&s->ec);
    s->matching = 0;
    if (rc == LRE_RET_MEMORY_ERROR) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    CHECK(rc == 0 || rc == 1 || rc == LRE_RET_UNDECIDED, "corrupted bytecode in a matcher over a domain");
    if (rc == 0) {
        JS_FreeValue(ctx, s->result);
        s->result = JS_NULL;
    }
    return 0;
}

static int js_regexp_exec_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSRegExpExec *s = st;
    JSRegExp *re;
    int r;

    if (s->hdr.stage == REX_REQUIRE) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->result = JS_UNDEFINED;
        s->str_val = JS_UNDEFINED;
        s->li_val = JS_UNDEFINED;
        s->bytecode = JS_UNDEFINED;
        s->capture = NULL;
        s->cap_alloc = 0;
        s->last_index = 0;
        s->rc = 0;
        s->set_pending = 0;
        s->matching = 0;
        if (!js_get_regexp(ctx, s->hdr.this_val, true))   /* 22.2.6.2 steps 1-2: RequireInternalSlot */
            return -1;
        s->hdr.stage = REX_TOSTRING;
    }
    if (s->hdr.stage == REX_TOSTRING) {
        /* 22.2.6.2 OVER UNKNOWN INPUT, answered HERE because the ToString below owes C a real string and can
           only crash on a concolic — which is what `/token=(\w+)/.exec(document.cookie)` did, and that is what
           reading a cookie looks like in every real bundle. Matching a pattern against input this engine was
           never given has no knowable result, so the operator produces an unknown DERIVED from the source: the
           `m ? m[1] : ...` after it then FORKS both arms instead of aborting the document, and the group read
           off it stays tied to the source a later sink solves for. */
        {
            JSValue ex = js_concolic_operand(ctx, step_arg(&s->hdr, 0));
            if (!JS_IsUninitialized(ex)) {
                /* The result is derived with no example; where no String the subject's domain admits matches
                   (js_regexp_exec_domain_begin), it is null in every world instead. */
                JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
                JS_FreeValue(ctx, ex);
                s->result = js_concolic_derive(ctx, step_arg(&s->hdr, 0), "RegExp.exec", JS_UNDEFINED);
                r = js_regexp_exec_domain_begin(ctx, s);
                if (r <= 0) return r;
                s->hdr.stage = REX_DOMAIN;
            }
        }
        if (s->hdr.stage == REX_TOSTRING) {
            /* 22.2.6.2 step 3: S is ? ToString(string), and it precedes every step of RegExpBuiltinExec. */
            r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->str_val, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = REX_GET_LASTINDEX;
        }
    }
    if (s->hdr.stage == REX_DOMAIN) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        return js_regexp_exec_domain_step(ctx, s);
    }
    if (s->hdr.stage == REX_GET_LASTINDEX) {
        /* step 3: Get(R, "lastIndex") — an accessor if the page redefined it. */
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_lastIndex, cb_result, &s->li_val,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = REX_TOLENGTH;
    }
    if (s->hdr.stage == REX_TOLENGTH) {
        /* step 4: ToLength on whatever that Get produced — a valueOf when it is an object. */
        r = step_tolength_run(ctx, &s->hdr, s->li_val, cb_result, &s->last_index, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = REX_FLAGS;
    }
    if (s->hdr.stage == REX_FLAGS) {
        /* steps 5-12: the matcher. The regexp's internal slots are re-read HERE and not before, because the two
           requests above ran the page's code and it may have called `compile()`; the bytecode this run uses is
           then HELD, so a `lastIndex` setter recompiling mid-run cannot free it under the result build. */
        const uint8_t *re_bytecode;
        JSString *str;
        int re_flags, alloc_count, shift;
        uint8_t *str_buf;

        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        re = js_get_regexp(ctx, s->hdr.this_val, true);
        if (!re) return -1;
        /* the matcher this run uses, HELD as a value so a `lastIndex` setter calling compile() cannot free it
           under the result build below — the C body kept it as a bare pointer in a local across that Set. */
        s->bytecode = js_dup(JS_MKPTR(JS_TAG_STRING, re->bytecode));
        re_bytecode = str8(JS_VALUE_GET_STRING(s->bytecode));
        re_flags = lre_get_flags(re_bytecode);
        if ((re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY)) == 0)
            s->last_index = 0;
        str = JS_VALUE_GET_STRING(s->str_val);
        /* The register-based executor writes capture positions AND temporary
           registers into the capture buffer; it must be sized by alloc_count
           (= capture_count*2 + register_count), not capture_count*2. Sizing it
           by capture_count*2 silently overflows the heap on any regexp that
           uses registers (i.e. most non-trivial patterns). */
        alloc_count = lre_get_alloc_count(re_bytecode);
        if (alloc_count > 0) {
            s->capture = js_malloc(ctx, sizeof(s->capture[0]) * alloc_count);
            if (!s->capture) return -1;
            s->cap_alloc = alloc_count;
        }
        shift = str->is_wide_char;
        str_buf = str8(str);
        if (s->last_index > str->len) {
            s->rc = 2;
        } else {
            lre_exec_begin(&s->ec, s->capture, re_bytecode,
                           str_buf, s->last_index, str->len, shift, ctx);
            s->matching = 1;
            s->rc = LRE_RET_YIELD;
        }
        s->hdr.stage = REX_MATCH;
    }
    if (s->hdr.stage == REX_MATCH) {
        /* THE MATCH. It runs one slice per entry and parks between them, so `/(a+)+b/.exec("a".repeat(40))` —
           the ReDoS shape, exponential in the input — is ordinary preemptible work rather than a thread the
           engine has given away. A yield is an exact position (program counter, input cursor, backtracking
           stack), never a re-match. */
        const uint8_t *re_bytecode;
        JSString *str;
        int re_flags, shift;
        uint8_t *str_buf;

        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        if (s->matching) {
            s->rc = lre_exec_step(&s->ec);
            if (s->rc == LRE_RET_YIELD)
                return 22;
            lre_exec_end(&s->ec);
            s->matching = 0;
        }
        /* re-derived rather than parked: both sources are HELD by this machine, so they cannot have moved. */
        re_bytecode = str8(JS_VALUE_GET_STRING(s->bytecode));
        re_flags = lre_get_flags(re_bytecode);
        str = JS_VALUE_GET_STRING(s->str_val);
        shift = str->is_wide_char;
        str_buf = str8(str);
        if (s->rc < 0) {
            switch(s->rc) {
            case LRE_RET_MEMORY_ERROR:
                JS_ThrowInternalError(ctx, "out of memory in regexp execution");
                break;
            case LRE_RET_BYTECODE_ERROR:
                JS_ThrowInternalError(ctx, "corrupted bytecode in regexp execution");
                break;
            default:
                abort();
            }
            return -1;
        }
        if (s->rc == 1) {
            /* RegExpBuiltinExec, proposal-regexp-legacy-features: a match by a regexp %RegExp% itself built
               updates the intrinsic's static slots; a match by anything else INVALIDATES them. Done here
               rather than in the result build because a `lastIndex` setter runs between the two and could
               observe the difference. */
            JSObject *rp = JS_VALUE_GET_OBJ(s->hdr.this_val);
            if (rp->u.regexp.legacy_features_enabled) {
                if (js_regexp_legacy_update(ctx, s->str_val, s->capture,
                                            lre_get_capture_count(re_bytecode), str_buf, shift) < 0)
                    return -1;
            } else {
                js_regexp_legacy_invalidate(ctx);
            }
        }
        if (s->rc != 1) {
            /* steps 12.a / 15: no match — reset lastIndex when the regexp is stateful or the start was past
               the end of the string. */
            if (s->rc == 2 || (re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY))) {
                s->set_pending = 1;
                s->set_val = 0;
            }
        } else if (re_flags & (LRE_FLAG_GLOBAL | LRE_FLAG_STICKY)) {
            /* step 18: a match on a stateful regexp advances lastIndex to the match end. */
            s->set_pending = 1;
            s->set_val = (int32_t)((s->capture[1] - str_buf) >> shift);
        }
        s->hdr.stage = REX_SET_LASTINDEX;
    }
    if (s->hdr.stage == REX_SET_LASTINDEX) {
        if (s->set_pending) {
            /* Set(R, "lastIndex", …, true) — a setter if the page redefined it, and a throw if it is a
               non-writable data property. */
            JSValue v = js_int32(s->set_val);
            r = step_setprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_lastIndex, v, cb_result,
                                 out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->set_pending = 0;
        }
        s->hdr.stage = REX_RESULT;
    }
    JS_FreeValue(ctx, cb_result);
    if (s->rc != 1) {
        s->result = JS_NULL;      /* steps 12.b / 15: no match */
        return 0;
    }
    {   /* steps 17-34, on objects this call creates: no page code, so it stays one C body. */
        JSValue str_val = s->str_val;
        s->str_val = JS_UNDEFINED;               /* CONSUMED as the result's `input` */
        s->result = js_regexp_build_result(ctx, JS_VALUE_GET_STRING(s->bytecode), str_val, s->capture);
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return -1; }
    }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The three references, the compiled matcher it pinned for the
   duration of the run, the capture array — and, ONLY while a match is in flight, the match itself. `matching`
   is that condition: before stage 4 and after the match answers, `ec` holds nothing, which is why the
   ownership is an `if` around the visit rather than something a list could state.
   ORDER MATTERS between the last two: the capture block is visited first so the context can be re-pointed at
   whichever copy it now belongs to. The context's remaining pointers aim into the subject string and the
   bytecode, and both are visited above, so both arms keep them alive. */
static void js_regexp_exec_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSRegExpExec *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->str_val);
    v->val(ctx, &s->li_val);
    v->val(ctx, &s->bytecode);
    v->buf(ctx, (void **)&s->capture, sizeof(uint8_t *) * (size_t)s->cap_alloc);
    if (s->matching)
        v->reexec(ctx, &s->ec, s->capture);
}

static JSValue js_regexp_exec_fini(JSContext *ctx, void *st, bool take_result)
{
    JSRegExpExec *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* DELETED: JS_RegExpDelete. It matched a regexp against a string and spliced out the matches, from C, in
   a loop — and it had NO CALLERS. Left in place it was a second copy of RegExpBuiltinExec that a reader would
   take for a live path, and the only remaining lre_exec call site outside the exec machine, which is what the
   suspendable executor has to convert. Dead code that LOOKS load-bearing is worse than none. */

enum { EX_PH_START = 0, EX_PH_CALL };

/* 22.2.7.1 RegExpExec(R, S) as a sub-sequence. Two of its steps are the page's code — step 3's
   `? Get(R, "exec")` and step 5's `? Call(exec, R, «S»)` when that is callable — and JS_RegExpExec reached both
   from C, so a patched `exec` with a loop in it had no flow base and every consumer of RegExpExec drove it to
   completion. The caller lends a 3-slot buffer because a CALL request is [this, fn, arg] and the header's own
   request buffer is two wide; the method itself rides h->coerce, which is what owns it across the call.
   0 = *pout is filled, 3/6 = the caller must return that step code, -1 = threw. */
static int step_reexec_run(JSContext *ctx, JSStepHdr *h, JSValueConst r, JSValueConst str, JSValue *buf,
                           JSValue in, JSValue *pout, JSValue **out_cb, int *out_argc)
{
    if (h->exec_phase == EX_PH_START) {
        int rc = step_getprop_run(ctx, h, r, JS_ATOM_exec, in, &h->coerce, out_cb, out_argc);
        if (rc) return rc < 0 ? -1 : rc;
        /* EVERY callable exec is CALLED, the built-in included. The arm that ran the built-in's body here
           instead was defended by "with a STRING subject there is nothing left in it to coerce" — measured
           false: RegExpBuiltinExec's Get(lastIndex), its ToLength and its Set(lastIndex) are the page's code
           whatever the subject is. It was the recognizer shape besides: a pointer comparison selecting a C
           fallback, which is what made those three unroutable. */
        if (JS_IsFunction(ctx, h->coerce)) {
            buf[0] = (JSValue)r;        /* this */
            buf[1] = h->coerce;         /* a borrowed view of what the header owns */
            buf[2] = (JSValue)str;
            *out_cb = buf; *out_argc = 1;
            h->exec_phase = EX_PH_CALL;
            return 3;
        }
        /* 22.2.7.1 step 4: exec is not callable, so RegExpBuiltinExec is performed DIRECTLY — and with it a
           step machine there is no body to run from here. The engine holds that algorithm as a callable it
           never exposes, so "perform the algorithm" is the same CALL request as every other spelling. */
        JS_FreeValue(ctx, h->coerce);
        h->coerce = js_dup(ctx->regexp_builtin_exec);
        buf[0] = (JSValue)r;
        buf[1] = h->coerce;
        buf[2] = (JSValue)str;
        *out_cb = buf; *out_argc = 1;
        h->exec_phase = EX_PH_CALL;
        return 3;
    }
    DCHECK(h->exec_phase == EX_PH_CALL, "RegExpExec resumed in an unknown phase");
    JS_FreeValue(ctx, h->coerce);
    h->coerce = JS_UNDEFINED;
    h->exec_phase = EX_PH_START;
    /* step 5.c: the result must be an Object or Null, checked before anyone reads a capture off it */
    if (!JS_IsObject(in) && !JS_IsNull(in)) {
        JS_FreeValue(ctx, in);
        JS_ThrowTypeError(ctx, "RegExp exec method must return an object or null");
        return -1;
    }
    *pout = in;
    return 0;
}

/* DELETED: js_regexp_test's body. `test` was already a step machine, but one whose body ran RegExpExec from
   C — and it skipped 22.2.6.16 step 2's "R must be an Object" besides. It is the @@search machine's arg-0 mode
   now, which is the same four steps with the lastIndex bracket left off. */

/* 22.2.6.8 RegExp.prototype [ @@match ] ( string ) — a step machine. js_regexp_Symbol_match ran the whole of it
   from C, including an UNBOUNDED RegExpExec loop that calls a patched `exec`, so a loop anywhere in it had no
   flow base and aborted at its back-edge. */
/* 22.2.6.8 RegExp.prototype [ @@match ], AS THE SPEC NUMBERS IT. Two stages may not name one step, so the Get
   and the ToString that consumes it each say which half they are — the machine rests at both. */
#define MATCH_STAGES(X) \
    X(MATCH_RECV,    "22.2.6.8 steps 1-2 (rx is the this value; the not-an-Object TypeError)") \
    X(MATCH_STR,     "22.2.6.8 step 3 (S is ToString(string))") \
    X(MATCH_FLAGS,   "22.2.6.8 step 4 (Get(rx, \"flags\"))") \
    X(MATCH_FLAGSTR, "22.2.6.8 steps 4-6.a (flags is ToString of it; whether it contains \"g\"; fullUnicode)") \
    X(MATCH_ONE,     "22.2.6.8 step 5.a (return RegExpExec(rx, S) — the non-global answer)") \
    X(MATCH_RESET,   "22.2.6.8 steps 6.b-d (Set(rx, \"lastIndex\", +0, true); A is ArrayCreate(0); n is 0)") \
    X(MATCH_EXEC,    "22.2.6.8 step 6.e.i (result is RegExpExec(rx, S))") \
    X(MATCH_M0,      "22.2.6.8 step 6.e.iii.1 (Get(result, \"0\"))") \
    X(MATCH_MSTR,    "22.2.6.8 steps 6.e.iii.1-4 (matchStr is ToString of it; CreateDataPropertyOrThrow(A, ToString(n), matchStr); n is n + 1)") \
    X(MATCH_LASTIDX, "22.2.6.8 step 6.e.iii.3.a (thisIndex is ToLength(Get(rx, \"lastIndex\")))") \
    X(MATCH_ADVANCE, "22.2.6.8 steps 6.e.iii.3.b-c (nextIndex is AdvanceStringIndex(S, thisIndex, fullUnicode); Set(rx, \"lastIndex\", nextIndex, true))")
enum { MATCH_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_re_match_steps[] = { MATCH_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_re_match_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReMatch *s = st;
    JSValue str;
    int r;

    if (s->hdr.stage == MATCH_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->rx = JS_UNDEFINED; s->str = JS_UNDEFINED; s->flags = JS_UNDEFINED;
        s->arr = JS_UNDEFINED; s->mres = JS_UNDEFINED; s->m0 = JS_UNDEFINED;
        s->n = 0; s->thisIndex = 0; s->is_global = 0; s->fullUnicode = 0;
        if (!JS_IsObject(s->hdr.this_val)) { JS_ThrowTypeErrorNotAnObject(ctx); return -1; }
        s->rx = js_dup(s->hdr.this_val);
        s->hdr.stage = MATCH_STR;
    }
    if (s->hdr.stage == MATCH_STR) {
        r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = MATCH_FLAGS;
    }
    if (s->hdr.stage == MATCH_FLAGS) {
        r = step_getprop_run(ctx, &s->hdr, s->rx, JS_ATOM_flags, cb_result, &s->flags, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = MATCH_FLAGSTR;
    }
    if (s->hdr.stage == MATCH_FLAGSTR) {
        JSString *fp;
        r = step_tostring_run(ctx, &s->hdr, s->flags, cb_result, &str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->flags);
        s->flags = str;
        fp = JS_VALUE_GET_STRING(s->flags);
        s->is_global = (-1 != string_indexof_char(fp, 'g', 0));
        /* the 'v' flag implies full Unicode, like 'u' */
        s->fullUnicode = s->is_global && (string_indexof_char(fp, 'u', 0) >= 0 ||
                                          string_indexof_char(fp, 'v', 0) >= 0);
        JS_FreeValue(ctx, s->flags); s->flags = JS_UNDEFINED;
        s->hdr.stage = s->is_global ? MATCH_RESET : MATCH_ONE;
    }
    if (s->hdr.stage == MATCH_ONE) {
        /* step 5.a: not global — ONE RegExpExec, and its result IS the answer */
        r = step_reexec_run(ctx, &s->hdr, s->rx, s->str, s->cbx, cb_result, &s->arr, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        return 0;
    }
    if (s->hdr.stage == MATCH_RESET) {
        /* step 6.b: `? Set(rx, "lastIndex", +0, true)` */
        r = step_setprop_run(ctx, &s->hdr, s->rx, JS_ATOM_lastIndex, js_int32(0), cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->arr = JS_NewArray(ctx);   /* step 6.c: ArrayCreate(0) — engine-private, so filling it runs nothing */
        if (JS_IsException(s->arr)) { s->arr = JS_UNDEFINED; return -1; }
        s->hdr.stage = MATCH_EXEC;
    }
    for (;;) {
        if (s->hdr.stage == MATCH_EXEC) {
            r = step_reexec_run(ctx, &s->hdr, s->rx, s->str, s->cbx, cb_result, &s->mres, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            if (JS_IsNull(s->mres)) {
                /* step 6.e.ii: no matches at all is null, not an empty array */
                JS_FreeValue(ctx, s->mres); s->mres = JS_UNDEFINED;
                if (s->n == 0) { JS_FreeValue(ctx, s->arr); s->arr = JS_NULL; }
                return 0;
            }
            s->hdr.stage = MATCH_M0;
        }
        if (s->hdr.stage == MATCH_M0) {
            r = step_getidx_run(ctx, &s->hdr, s->mres, 0, cb_result, &s->m0, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = MATCH_MSTR;
        }
        if (s->hdr.stage == MATCH_MSTR) {
            int empty;
            r = step_tostring_run(ctx, &s->hdr, s->m0, cb_result, &str, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->m0); s->m0 = JS_UNDEFINED;
            JS_FreeValue(ctx, s->mres); s->mres = JS_UNDEFINED;
            empty = JS_IsEmptyString(str);
            /* step 6.e.iv: `!` CreateDataPropertyOrThrow(A, ToString(n), matchStr) — A is engine-private */
            if (JS_DefinePropertyValueInt64(ctx, s->arr, s->n, str, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                return -1;
            s->n++;
            if (!empty) { s->hdr.stage = MATCH_EXEC; continue; }
            s->hdr.stage = MATCH_LASTIDX;
        }
        if (s->hdr.stage == MATCH_LASTIDX) {
            /* an EMPTY match must always advance at least one char, or the loop never terminates */
            r = step_getlen_run(ctx, &s->hdr, s->rx, JS_ATOM_lastIndex, cb_result, &s->thisIndex,
                                out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = MATCH_ADVANCE;
        }
        r = step_setprop_run(ctx, &s->hdr, s->rx, JS_ATOM_lastIndex,
                             js_int64(string_advance_index(JS_VALUE_GET_STRING(s->str), s->thisIndex,
                                                           s->fullUnicode)),
                             cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = MATCH_EXEC;
    }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). @@match holds the regexp, the subject and each match across an `exec` the page can override. */
static void js_re_match_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReMatch *s = st;
    v->val(ctx, &s->arr);
    v->val(ctx, &s->rx);
    v->val(ctx, &s->str);
    v->val(ctx, &s->flags);
    v->val(ctx, &s->mres);
    v->val(ctx, &s->m0);
}

static JSValue js_re_match_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReMatch *s = st;
    JSValue r = take_result ? s->arr : JS_UNDEFINED;
    if (take_result) s->arr = JS_UNDEFINED;
    return r;
}

typedef struct JSRegExpStringIteratorData {
    JSValue iterating_regexp;
    JSValue iterated_string;
    bool global;
    bool unicode;
    int done;
} JSRegExpStringIteratorData;

static void js_regexp_string_iterator_finalizer(JSRuntime *rt,
                                                JSValueConst val)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSRegExpStringIteratorData *it = p->u.regexp_string_iterator_data;
    if (it) {
        JS_FreeValueRT(rt, it->iterating_regexp);
        JS_FreeValueRT(rt, it->iterated_string);
        js_free_rt(rt, it);
    }
}

static void js_regexp_string_iterator_mark(JSRuntime *rt, JSValueConst val,
                                           JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSRegExpStringIteratorData *it = p->u.regexp_string_iterator_data;
    if (it) {
        JS_MarkValue(rt, it->iterating_regexp, mark_func);
        JS_MarkValue(rt, it->iterated_string, mark_func);
    }
}

/* 22.2.9.2.1 %RegExpStringIteratorPrototype%.next. Every step of it is the page's code: RegExpExec (which is a
   Get of `exec` and then a call), `Get(match, 0)` and its ToString, and — for a GLOBAL regexp that matched the
   empty string — `Get(R,"lastIndex")`, its ToLength and the `Set` that advances it. js_regexp_string_iterator_next
   did all of them from C through JS_RegExpExec, which is why that helper existed at all; with RegExpBuiltinExec a
   step machine there is nothing for it to call, and it is deleted with this conversion.
   It also stops being a JS_CFUNC_iterator_next: that cproto hands the engine (value, *pdone) so a for-of can skip
   building the result object, and a machine cannot answer through an out-parameter. It builds the
   IteratorResult itself, which is what the spec says `next` returns. */
/* Steps 1-8 read only the iterator's own slots and step 14 builds an object out of nothing the page can reach,
   so those two ranges are one stage each and the labels say which range. Everything between them is one of the
   page's operations and is its own rest point. */
#define RESTRITER_STAGES(X) \
    X(RSI_RECV,    "22.2.9.2.1 steps 1-8 (the RegExp String Iterator slots; the done answer)") \
    X(RSI_EXEC,    "22.2.9.2.1 step 9 (match is RegExpExec(R, S))") \
    X(RSI_GET0,    "22.2.9.2.1 step 12 (Get(match, \"0\"))") \
    X(RSI_STR,     "22.2.9.2.1 step 12 (matchStr is ToString of it)") \
    X(RSI_LASTIDX, "22.2.9.2.1 step 13.a (thisIndex is ToLength(Get(R, \"lastIndex\")))") \
    X(RSI_ADVANCE, "22.2.9.2.1 steps 13.b-c (nextIndex is AdvanceStringIndex(S, thisIndex, fullUnicode); " \
                   "Set(R, \"lastIndex\", nextIndex, true))") \
    X(RSI_RESULT,  "22.2.9.2.1 steps 11.b and 14 (CreateIteratorResultObject(match, false))")
enum { RESTRITER_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_re_str_iter_steps[] = { RESTRITER_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_re_str_iter_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReStrIter *s = st;
    JSRegExpStringIteratorData *it;
    int r;

    it = JS_GetOpaque2(ctx, s->hdr.this_val, JS_CLASS_REGEXP_STRING_ITERATOR);
    if (!it) { JS_FreeValue(ctx, cb_result); return -1; }

    if (s->hdr.stage == RSI_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->result = JS_UNDEFINED;
        s->match = JS_UNDEFINED;
        s->matchStr = JS_UNDEFINED;
        s->thisIndex = 0;
        if (it->done)
            goto done_result;
        s->hdr.stage = RSI_EXEC;
    }
    if (s->hdr.stage == RSI_EXEC) {
        r = step_reexec_run(ctx, &s->hdr, it->iterating_regexp, it->iterated_string, s->exec_buf,
                            cb_result, &s->match, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (JS_IsNull(s->match)) {
            it->done = true;
            goto done_result;
        }
        if (!it->global) {
            it->done = true;
            s->hdr.stage = RSI_RESULT;      /* a non-global match yields once and is finished */
        } else {
            s->hdr.stage = RSI_GET0;
        }
    }
    if (s->hdr.stage == RSI_GET0) {
        r = step_getidx_run(ctx, &s->hdr, s->match, 0, cb_result, &s->matchStr, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = RSI_STR;
    }
    if (s->hdr.stage == RSI_STR) {
        JSValue str = JS_UNDEFINED;
        r = step_tostring_run(ctx, &s->hdr, s->matchStr, cb_result, &str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->matchStr);
        s->matchStr = str;
        /* only an EMPTY match has to advance lastIndex, or the loop never terminates */
        s->hdr.stage = JS_IsEmptyString(s->matchStr) ? RSI_LASTIDX : RSI_RESULT;
    }
    if (s->hdr.stage == RSI_LASTIDX) {
        r = step_getlen_run(ctx, &s->hdr, it->iterating_regexp, JS_ATOM_lastIndex, cb_result, &s->thisIndex,
                            out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = RSI_ADVANCE;
    }
    if (s->hdr.stage == RSI_ADVANCE) {
        int64_t next = string_advance_index(JS_VALUE_GET_STRING(it->iterated_string), s->thisIndex, it->unicode);
        r = step_setprop_run(ctx, &s->hdr, it->iterating_regexp, JS_ATOM_lastIndex, js_int64(next),
                             cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = RSI_RESULT;
    }
    JS_FreeValue(ctx, cb_result);
    {   /* building the IteratorResult runs nothing */
        JSValue m = s->match;
        s->match = JS_UNDEFINED;
        s->result = js_create_iterator_result(ctx, m, false);
        if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return -1; }
    }
    return 0;

done_result:
    JS_FreeValue(ctx, cb_result);
    s->result = js_create_iterator_result(ctx, JS_UNDEFINED, true);
    if (JS_IsException(s->result)) { s->result = JS_UNDEFINED; return -1; }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_re_str_iter_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReStrIter *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->match);
    v->val(ctx, &s->matchStr);
}

static JSValue js_re_str_iter_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReStrIter *s = st;
    JSValue r = take_result ? js_dup(s->result) : JS_UNDEFINED;
    return r;
}

/* 22.2.6.9 RegExp.prototype [ @@matchAll ] ( string ) — a step machine. js_regexp_Symbol_matchAll ran all of it
   from C: ToString(string), SpeciesConstructor's `constructor` and `@@species` reads, `? Get(R,"flags")` and
   its ToString, `? Construct(C, «R, flags»)`, and the lastIndex read and write. The Construct in particular
   reached the DFAIL once RegExp itself became a step machine — a C frame cannot drive one. */
/* 22.2.6.9 RegExp.prototype [ @@matchAll ], AS THE SPEC NUMBERS IT. */
#define MALL_STAGES(X) \
    X(MALL_RECV,      "22.2.6.9 steps 1-2 (R is the this value; the not-an-Object TypeError)") \
    X(MALL_STR,       "22.2.6.9 step 3 (S is ToString(string))") \
    X(MALL_CTOR,      "22.2.6.9 step 4 (C is SpeciesConstructor(R, %RegExp%))") \
    X(MALL_FLAGS,     "22.2.6.9 step 5 (Get(R, \"flags\"))") \
    X(MALL_FLAGSTR,   "22.2.6.9 step 5 (flags is ToString of what that Get produced)") \
    X(MALL_CONSTRUCT, "22.2.6.9 step 6 (matcher is Construct(C, <<R, flags>>) — the dispatch)") \
    X(MALL_MATCHER,   "22.2.6.9 step 6 (the constructed matcher arrives)") \
    X(MALL_LASTIDX,   "22.2.6.9 step 7 (lastIndex is ToLength(Get(R, \"lastIndex\")))") \
    X(MALL_SET,       "22.2.6.9 steps 8-11 (Set(matcher, \"lastIndex\", lastIndex, true); global; fullUnicode; CreateRegExpStringIterator)")
enum { MALL_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_re_matchall_steps[] = { MALL_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_re_matchall_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReMatchAll *s = st;
    JSValue str;
    int r;

    if (s->hdr.stage == MALL_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->str = JS_UNDEFINED; s->ctor = JS_UNDEFINED; s->flags = JS_UNDEFINED;
        s->matcher = JS_UNDEFINED; s->result = JS_UNDEFINED; s->lastIndex = 0;
        if (!JS_IsObject(s->hdr.this_val)) { JS_ThrowTypeErrorNotAnObject(ctx); return -1; }
        s->hdr.stage = MALL_STR;
    }
    if (s->hdr.stage == MALL_STR) {
        r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = MALL_CTOR;
    }
    if (s->hdr.stage == MALL_CTOR) {
        r = step_speciesctor_run(ctx, &s->hdr, s->hdr.this_val, ctx->regexp_ctor, cb_result, &s->ctor,
                                 out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = MALL_FLAGS;
    }
    if (s->hdr.stage == MALL_FLAGS) {
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_flags, cb_result, &s->flags,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = MALL_FLAGSTR;
    }
    if (s->hdr.stage == MALL_FLAGSTR) {
        r = step_tostring_run(ctx, &s->hdr, s->flags, cb_result, &str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->flags);
        s->flags = str;
        s->hdr.stage = MALL_CONSTRUCT;
    }
    if (s->hdr.stage == MALL_CONSTRUCT) {
        /* step 5: `? Construct(C, «R, flags»)`. Every slot is a borrowed view of what the state or its header
           owns, so the request buffer has nothing to release. */
        s->cb[0] = s->ctor;
        s->cb[1] = s->hdr.this_val;
        s->cb[2] = s->flags;
        *out_cb = s->cb; *out_argc = 2;
        s->hdr.stage = MALL_MATCHER;
        return 4;
    }
    if (s->hdr.stage == MALL_MATCHER) {
        s->matcher = cb_result; cb_result = JS_UNDEFINED;
        s->hdr.stage = MALL_LASTIDX;
    }
    if (s->hdr.stage == MALL_LASTIDX) {
        /* step 6: `? ToLength(? Get(R, "lastIndex"))` — off the RECEIVER, not the matcher */
        r = step_getlen_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_lastIndex, cb_result, &s->lastIndex,
                            out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = MALL_SET;
    }
    /* step 7: `? Set(matcher, "lastIndex", lastIndex, true)` */
    r = step_setprop_run(ctx, &s->hdr, s->matcher, JS_ATOM_lastIndex, js_int64(s->lastIndex), cb_result,
                         out_cb, out_argc);
    cb_result = JS_UNDEFINED;
    if (r) return r < 0 ? -1 : r;
    {   /* step 10: CreateRegExpStringIterator — an engine-private object, so it runs nothing */
        JSRegExpStringIteratorData *it;
        JSString *fp = JS_VALUE_GET_STRING(s->flags);
        JSValue iter = JS_NewObjectClass(ctx, JS_CLASS_REGEXP_STRING_ITERATOR);
        if (JS_IsException(iter)) return -1;
        it = js_malloc(ctx, sizeof(*it));
        if (!it) { JS_FreeValue(ctx, iter); return -1; }
        it->iterating_regexp = s->matcher;   /* the iterator adopts both */
        it->iterated_string = s->str;
        s->matcher = JS_UNDEFINED;
        s->str = JS_UNDEFINED;
        it->global = string_indexof_char(fp, 'g', 0) >= 0;
        it->unicode = string_indexof_char(fp, 'u', 0) >= 0 || string_indexof_char(fp, 'v', 0) >= 0;
        it->done = false;
        JS_SetOpaqueInternal(iter, it);
        s->result = iter;
    }
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). cb is the Construct request's buffer, borrowing ctor, the
   receiver and flags from where they already live. */
static void js_re_matchall_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReMatchAll *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->str);
    v->val(ctx, &s->ctor);
    v->val(ctx, &s->flags);
    v->val(ctx, &s->matcher);
}

static JSValue js_re_matchall_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReMatchAll *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

typedef struct ValueBuffer {
    JSContext *ctx;
    JSValue *arr;
    JSValue def[4];
    int len;
    int size;
    int error_status;
} ValueBuffer;

static int value_buffer_init(JSContext *ctx, ValueBuffer *b)
{
    b->ctx = ctx;
    b->len = 0;
    b->size = 4;
    b->error_status = 0;
    b->arr = b->def;
    return 0;
}

static void value_buffer_free(ValueBuffer *b)
{
    while (b->len > 0)
        JS_FreeValue(b->ctx, b->arr[--b->len]);
    if (b->arr != b->def)
        js_free(b->ctx, b->arr);
    b->arr = b->def;
    b->size = 4;
}

static int value_buffer_append(ValueBuffer *b, JSValue val)
{
    if (b->error_status)
        return -1;

    if (b->len >= b->size) {
        int new_size = (b->len + (b->len >> 1) + 31) & ~16;
        size_t slack;
        JSValue *new_arr;

        if (b->arr == b->def) {
            new_arr = js_realloc2(b->ctx, NULL, sizeof(*b->arr) * new_size, &slack);
            if (new_arr)
                memcpy(new_arr, b->def, sizeof b->def);
        } else {
            new_arr = js_realloc2(b->ctx, b->arr, sizeof(*b->arr) * new_size, &slack);
        }
        if (!new_arr) {
            value_buffer_free(b);
            JS_FreeValue(b->ctx, val);
            b->error_status = -1;
            return -1;
        }
        new_size += slack / sizeof(*new_arr);
        b->arr = new_arr;
        b->size = new_size;
    }
    b->arr[b->len++] = val;
    return 0;
}

static int js_is_standard_regexp(JSContext *ctx, JSValueConst rx)
{
    JSValue val;
    int res;

    val = JS_GetProperty(ctx, rx, JS_ATOM_constructor);
    if (JS_IsException(val))
        return -1;
    // rx.constructor === RegExp
    res = js_same_value(ctx, val, ctx->regexp_ctor);
    JS_FreeValue(ctx, val);
    if (res) {
        val = JS_GetProperty(ctx, rx, JS_ATOM_exec);
        if (JS_IsException(val))
            return -1;
        // rx.exec === RE_exec
        /* the BUILT-IN exec, which is a step machine: its identity is the definition it names, not a
           c_function pointer the JS_CFUNC_step cproto does not carry. */
        res = tramp_step_def_of(val) == &js_regexp_exec_def;
        JS_FreeValue(ctx, val);
    }
    return res;
}

/* 22.2.6.11 step 14 — phase 2, one match per entry. EVERY read here is off an object the page produced (a
   patched `exec` returns whatever it likes), so LengthOfArrayLike, `? Get(result,"0")` and its ToString,
   `? Get(result,"index")`, each `? Get(result, n)` and its ToString, `? Get(result,"groups")` and the ToString
   of the replacer's own return are all its code. They ran from C.
   The C also read "groups" BEFORE the captures. Step 14.i runs the capture loop and step 14.j reads "groups"
   after it, which a getter on either observes; the stages below are in the spec's order.
   Which stage is which step is REP_STAGES; every one of them names the step it rests at. */
static void js_re_rep_free_cb(JSContext *ctx, struct JSReRep *s);
static int js_re_rep_splice(JSContext *ctx, JSReRep *s, JSValue rep_str)
{
    JSString *sp = JS_VALUE_GET_STRING(s->str);
    /* a custom regexp can hand back a BACKWARD index; the substitution is then ignored (never rewinds) */
    if (s->position >= s->nextSourcePosition) {
        string_buffer_concat(&s->b, sp, s->nextSourcePosition, s->position);
        string_buffer_concat_value(&s->b, rep_str);
        s->nextSourcePosition = s->position + JS_VALUE_GET_STRING(s->matched)->len;
    }
    JS_FreeValue(ctx, rep_str);
    s->j++;
    s->hdr.stage = REP_LEN;   /* step 15's next element */
    return 0;
}

static int js_re_rep_step(JSContext *ctx, struct JSReRep *s, JSValue cb_result,
                          JSValue **out_cb, int *out_argc)
{
    JSString *sp = JS_VALUE_GET_STRING(s->str);
    JSValueConst result;
    JSValue str;
    int r;

    if (s->hdr.stage == REP_CALL) {
        s->repres = cb_result; cb_result = JS_UNDEFINED;
        js_re_rep_free_cb(ctx, s);
        s->hdr.stage = REP_REPSTR;
    }
    if (s->hdr.stage == REP_REPSTR) {
        r = step_tostring_run(ctx, &s->hdr, s->repres, cb_result, &str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->repres); s->repres = JS_UNDEFINED;
        js_re_rep_splice(ctx, s, str);
    }
    for (;;) {
        if (s->hdr.stage == REP_LEN) {
            int64_t rlen;
            if (s->j >= s->nresults) { JS_FreeValue(ctx, cb_result); break; }
            r = step_length_run(ctx, &s->hdr, s->results[s->j], cb_result, &rlen, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->nCaptures = rlen > 1 ? rlen - 1 : REP_RECV;
            s->hdr.stage = REP_MATCH0;
        }
        result = s->results[s->j];
        if (s->hdr.stage == REP_MATCH0) {
            r = step_getidx_run(ctx, &s->hdr, result, 0, cb_result, &s->m0, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = REP_MATCHED;
        }
        if (s->hdr.stage == REP_MATCHED) {
            r = step_tostring_run(ctx, &s->hdr, s->m0, cb_result, &str, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->m0); s->m0 = JS_UNDEFINED;
            JS_FreeValue(ctx, s->matched);
            s->matched = str;
            s->hdr.stage = REP_POSITION;
        }
        if (s->hdr.stage == REP_POSITION) {
            r = step_getlen_run(ctx, &s->hdr, result, JS_ATOM_index, cb_result, &s->position, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            if (s->position > sp->len) s->position = sp->len;
            else if (s->position < 0) s->position = 0;
            /* the call buffer IS the spec's captures List: [this, fn, matched, caps..., position, string,
               groups?]. It is allocated once nCaptures is known and filled as each capture lands, so an abandon
               mid-loop releases exactly what has been gathered (js_re_rep_free_cb reads cb_nargs). */
            js_re_rep_free_cb(ctx, s);
            s->cb_buf = js_malloc(ctx, sizeof(JSValue) * (size_t)(2 + s->nCaptures + 4));
            if (!s->cb_buf) return -1;
            s->cb_buf[0] = JS_UNDEFINED;               /* thisArgument */
            s->cb_buf[1] = js_dup(s->rep);
            s->cb_buf[2] = js_dup(s->matched);
            s->cb_nargs = 1;
            s->n = 1;
            s->hdr.stage = REP_CAP;
        }
        while (s->hdr.stage == REP_CAP || s->hdr.stage == REP_CAPSTR) {
            if (s->hdr.stage == REP_CAP) {
                if (s->n > s->nCaptures) { s->hdr.stage = REP_GROUPS; break; }
                r = step_getidx_run(ctx, &s->hdr, result, s->n, cb_result, &s->cap, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r < 0 ? -1 : r;
                s->hdr.stage = REP_CAPSTR;
            }
            if (!JS_IsUndefined(s->cap)) {
                r = step_tostring_run(ctx, &s->hdr, s->cap, cb_result, &str, out_cb, out_argc);
                cb_result = JS_UNDEFINED;
                if (r) return r < 0 ? -1 : r;
                JS_FreeValue(ctx, s->cap);
                s->cap = str;
            }
            s->cb_buf[2 + s->cb_nargs++] = s->cap;     /* the buffer adopts it */
            s->cap = JS_UNDEFINED;
            s->n++;
            s->hdr.stage = REP_CAP;
        }
        if (s->hdr.stage == REP_GROUPS) {
            r = step_getprop_run(ctx, &s->hdr, result, JS_ATOM_groups, cb_result, &s->named, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = REP_ARGS;
        }
        /* STAGE-GUARDED, and it has to be: the substitution below can now SUSPEND, and on the way back in
           these three statements would run a second time — appending past cb_buf and freeing the answer that
           was just delivered. They were unconditional because nothing after them could re-enter. */
        if (s->hdr.stage == REP_ARGS) {
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            s->cb_buf[2 + s->cb_nargs++] = js_int32(s->position);
            s->cb_buf[2 + s->cb_nargs++] = js_dup(s->str);
            if (s->functional) {
                /* step 14.k: Call(replaceValue, undefined, «matched, ...captures, position, string, groups?»).
                   The spec builds a LIST, not an array — the JS array a C path materializes is never
                   observable, so the args go straight into the call buffer. */
                if (!JS_IsUndefined(s->named)) {
                    s->cb_buf[2 + s->cb_nargs++] = s->named;   /* ownership moves into the buffer */
                    s->named = JS_UNDEFINED;
                }
                s->hdr.stage = REP_CALL;
                *out_cb = s->cb_buf; *out_argc = s->cb_nargs;
                return 3;
            }
            /* step 14.l: a NON-callable replacement is the same walk with a different substitution SOURCE —
               GetSubstitution instead of a callback — so it runs HERE, in the one machine, and continues to the
               next match. It is not a separate no-callback body: that duplicate walk is deleted.
               The captures array and the ToObject'd `groups` move onto the MACHINE because the walk parks on
               `groups[name]`; as C locals they were live only until the C frame returned. */
            {
                int64_t gn;
                s->gs_tab = JS_NewArray(ctx);
                if (JS_IsException(s->gs_tab)) { s->gs_tab = JS_UNDEFINED; return -1; }
                for (gn = 0; gn <= s->nCaptures; gn++) {
                    if (JS_DefinePropertyValueInt64(ctx, s->gs_tab, gn, js_dup(s->cb_buf[2 + gn]),
                                                    JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                        return -1;
                }
                s->gs_named = JS_IsUndefined(s->named) ? JS_UNDEFINED : JS_ToObject(ctx, s->named);
                if (JS_IsException(s->gs_named)) { s->gs_named = JS_UNDEFINED; return -1; }
                JS_FreeValue(ctx, s->named); s->named = JS_UNDEFINED;
            }
            s->gs.stage = GS_START;
            s->gs.position = s->position;
            s->hdr.stage = REP_SUBST;
        }
        if (s->hdr.stage == REP_SUBST) {
            JSValue rep_str = JS_UNDEFINED;
            int gsr;
            gsr = step_getsubst_run(ctx, &s->hdr, &s->gs, s->matched, s->str, s->gs_tab,
                                    s->gs_named, s->rep, cb_result, &rep_str, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (gsr) return gsr < 0 ? -1 : gsr;
            JS_FreeValue(ctx, s->gs_tab);   s->gs_tab = JS_UNDEFINED;
            JS_FreeValue(ctx, s->gs_named); s->gs_named = JS_UNDEFINED;
            s->hdr.stage = REP_LEN;
            js_re_rep_splice(ctx, s, rep_str);
        }
    }
    string_buffer_concat(&s->b, sp, s->nextSourcePosition, sp->len);
    s->result = string_buffer_end(&s->b);
    if (JS_IsException(s->result))
        return -1;
    return 0;
}

static void js_re_rep_free_cb(JSContext *ctx, struct JSReRep *s)
{
    int i;
    if (!s->cb_buf)
        return;
    JS_FreeValue(ctx, s->cb_buf[1]);
    for (i = 0; i < s->cb_nargs; i++)
        JS_FreeValue(ctx, s->cb_buf[2 + i]);
    js_free(ctx, s->cb_buf);
    s->cb_buf = NULL; s->cb_nargs = 0;
}

/* RegExp.prototype[@@replace] IS this step machine — JS_CFUNC_STEP_DEF, no recognizer, no C entry to choose
   against. Stages 0, 10, 20 and 2-4 are 22.2.6.11 steps 2-8 IN THE SPEC'S ORDER — ToString(string), then the
   replacer's ToString, then `flags`, then the lastIndex reset. The C read `flags` FIRST, which
   `{ get flags(){...} }` with a throwing subject observes. 11-15 collect every match; 30 onwards is phase 2.
   DELETED: js_re_rep_prologue and js_re_rep_collect. They ran the whole of phase 1 from C — `? Get(rx,"flags")`
   and its ToString, `? Set(rx,"lastIndex",0)`, then an UNBOUNDED JS_RegExpExec loop whose every turn could call
   a patched `exec`, read `[0]` off whatever that returned, ToString it, and re-read and re-write `lastIndex`.
   A comment here used to justify that by saying a recognizer required a STANDARD regexp, so no user code ran
   before the first callback. The recognizer is gone, so the justification was stale and the loop was a
   drive-to-completion inside a converted machine: `re.exec = function(){ while(...){} }` aborted at its
   back-edge with "its C entry never became a flow base". */

static int js_re_rep_vstep(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReRep *s = st;
    JSValueConst repArg = step_arg(&s->hdr, 1);
    int r;

    if (s->hdr.stage == REP_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        if (!JS_IsObject(s->hdr.this_val)) { JS_ThrowTypeErrorNotAnObject(ctx); return -1; }
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds, and a buffer
           initialised late is freed with a NULL ctx. */
        string_buffer_init(ctx, &s->b, 0);
        s->flags = JS_UNDEFINED; s->mres = JS_UNDEFINED; s->m0 = JS_UNDEFINED;
        s->cap = JS_UNDEFINED; s->named = JS_UNDEFINED; s->repres = JS_UNDEFINED;
        s->gs_tab = JS_UNDEFINED; s->gs_named = JS_UNDEFINED;
        s->gs.pending = JS_UNDEFINED; s->gs.name_atom = JS_ATOM_NULL; s->gs.active = 0;
        s->rx = js_dup(s->hdr.this_val);
        s->hdr.stage = REP_TOSTRING;
    }
    if (s->hdr.stage == REP_TOSTRING) {
        /* step 3: `? ToString(string)` — BEFORE the replacer's coercion and before `flags` */
        r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = REP_REPLACER;
    }
    if (s->hdr.stage == REP_REPLACER) {
        /* steps 5-6: ONE machine, both replacer kinds — a callable drives step-code 3, anything else is
           ToString'd HERE, which is where the spec coerces it: before `flags` is read, not after the matches
           are collected. */
        s->functional = JS_IsFunction(ctx, repArg);
        if (s->functional) {
            s->rep = js_dup(repArg);
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        } else {
            r = step_tostring_run(ctx, &s->hdr, repArg, cb_result, &s->rep, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) { if (r < 0) s->rep = JS_UNDEFINED; return r < 0 ? -1 : r; }
        }
        s->hdr.stage = REP_FLAGS;
    }
    if (s->hdr.stage == REP_FLAGS) {
        /* step 3: `? Get(rx, "flags")`. RegExp.prototype.flags is an accessor that reads six more, so on a
           subclass this is the page's code several times over. */
        r = step_getprop_run(ctx, &s->hdr, s->rx, JS_ATOM_flags, cb_result, &s->flags, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = REP_FLAGSTR;
    }
    if (s->hdr.stage == REP_FLAGSTR) {
        JSValue fstr;
        JSString *fp;
        r = step_tostring_run(ctx, &s->hdr, s->flags, cb_result, &fstr, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->flags);
        s->flags = fstr;
        fp = JS_VALUE_GET_STRING(s->flags);
        s->is_global = (-1 != string_indexof_char(fp, 'g', 0));
        /* the 'v' flag implies full Unicode, like 'u' */
        s->fullUnicode = s->is_global && (string_indexof_char(fp, 'u', 0) >= 0 ||
                                          string_indexof_char(fp, 'v', 0) >= 0);
        JS_FreeValue(ctx, s->flags); s->flags = JS_UNDEFINED;
        s->hdr.stage = s->is_global ? REP_RESET : REP_EXEC;
    }
    if (s->hdr.stage == REP_RESET) {
        /* step 4.b: `? Set(rx, "lastIndex", 0, true)` */
        r = step_setprop_run(ctx, &s->hdr, s->rx, JS_ATOM_lastIndex, js_int32(0), cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = REP_EXEC;
    }
    /* step 5 onwards: collect EVERY match. Unbounded by construction — a patched `exec` decides when it ends —
       so each turn is stages, never a C loop. */
    while (s->hdr.stage >= REP_EXEC && s->hdr.stage <= REP_ADVANCE) {
        if (s->hdr.stage == REP_EXEC) {
            r = step_reexec_run(ctx, &s->hdr, s->rx, s->str, s->cbx, cb_result, &s->mres, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            if (JS_IsNull(s->mres)) { JS_FreeValue(ctx, s->mres); s->mres = JS_UNDEFINED; s->hdr.stage = REP_LEN; break; }
            if (s->nresults == s->rcap) {
                JSValue *nr;
                int ncap = s->rcap ? s->rcap * 2 : 8;
                nr = js_realloc(ctx, s->results, sizeof(JSValue) * ncap);
                if (!nr) { JS_ThrowOutOfMemory(ctx); return -1; }
                s->results = nr; s->rcap = ncap;
            }
            s->results[s->nresults++] = s->mres;   /* the array adopts it */
            s->mres = JS_UNDEFINED;
            if (!s->is_global) { s->hdr.stage = REP_LEN; break; }
            s->hdr.stage = REP_M0;
        }
        if (s->hdr.stage == REP_M0) {
            r = step_getidx_run(ctx, &s->hdr, s->results[s->nresults - 1], 0, cb_result, &s->m0,
                                out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = REP_MSTR;
        }
        if (s->hdr.stage == REP_MSTR) {
            JSValue mstr;
            int empty;
            r = step_tostring_run(ctx, &s->hdr, s->m0, cb_result, &mstr, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            JS_FreeValue(ctx, s->m0); s->m0 = JS_UNDEFINED;
            /* an EMPTY match must always advance at least one char, or the collection never terminates */
            empty = JS_IsEmptyString(mstr);
            JS_FreeValue(ctx, mstr);
            s->hdr.stage = empty ? REP_LASTIDX : REP_EXEC;
            continue;
        }
        if (s->hdr.stage == REP_LASTIDX) {
            r = step_getlen_run(ctx, &s->hdr, s->rx, JS_ATOM_lastIndex, cb_result, &s->thisIndex,
                                out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = REP_ADVANCE;
        }
        r = step_setprop_run(ctx, &s->hdr, s->rx, JS_ATOM_lastIndex,
                             js_int64(string_advance_index(JS_VALUE_GET_STRING(s->str), s->thisIndex,
                                                           s->fullUnicode)),
                             cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = REP_EXEC;
    }
    DCHECK(s->hdr.stage >= REP_LEN, "the @@replace machine entered phase 2 from an unexpected stage");
    return js_re_rep_step(ctx, s, cb_result, out_cb, out_argc);
}

static JSValue js_re_rep_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReRep *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Two ARRAYS and an accumulator, which is why this one needed
   the sub-object operations rather than a list of values:
     - `results` is phase 1's collected matches, `nresults` live in a `rcap` block;
     - `cb_buf` IS the spec's captures List, [this, fn, matched, caps…, position, string, groups?], allocated
       once nCaptures is known and filled as each capture lands. Slot 0 is the callback's `this`, a plain
       UNDEFINED that holds no reference, so the whole block visits uniformly — 2 + cb_nargs live in a block
       sized for the worst case;
     - `b` is the accumulator, and it needs no take_result guard: string_buffer_end NULLs the buffer on every
       path that consumed it, so the release is right whether the result was produced or not, and a NULL ctx is
       the prologue having thrown before it was built.
   `cbx` is RegExpExec's request buffer and borrows; the substitution's own three fields are declared once, by
   js_getsubst_visit, because str.replace holds the same walk. */
static void js_re_rep_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReRep *s = st;
    v->val(ctx, &s->result);
    v->strbuf(ctx, &s->b);
    js_getsubst_visit(ctx, &s->gs, v);
    v->val(ctx, &s->gs_tab);
    v->val(ctx, &s->gs_named);
    v->array(ctx, (void **)&s->results, sizeof(JSValue), s->nresults, s->rcap, js_step_visit_value_elem);
    v->array(ctx, (void **)&s->cb_buf, sizeof(JSValue), 2 + s->cb_nargs,
             (int)(2 + s->nCaptures + 4), js_step_visit_value_elem);
    v->val(ctx, &s->rx);
    v->val(ctx, &s->str);
    v->val(ctx, &s->rep);
    v->val(ctx, &s->matched);
    v->val(ctx, &s->flags);
    v->val(ctx, &s->mres);
    v->val(ctx, &s->m0);
    v->val(ctx, &s->cap);
    v->val(ctx, &s->named);
    v->val(ctx, &s->repres);
}



/* The NO-CALLBACK path of 22.2.6.11 only: a non-function replacer, reached solely from js_re_rep_init. The
   functional walk that used to live here is gone — that one is js_re_rep_step, and there is no longer a second
   copy for a recognizer to choose against. */

/* 22.2.6.16 RegExp.prototype.test and 22.2.6.12 RegExp.prototype [ @@search ] ( string ) — ONE machine.
   js_regexp_Symbol_search ran every step from C: ToString(string), `? Get(rx,"lastIndex")`, the reset,
   RegExpExec (whose `exec` the page can patch), the re-read, the restore and `? Get(result,"index")`.
   `test` is the same walk with the four lastIndex stages skipped — it saves no lastIndex — which is what
   hdr.arg selects, and nothing else. */
/* 22.2.6.12 RegExp.prototype [ @@search ] and 22.2.6.16 RegExp.prototype.test are ONE machine, so ONE stage
   list expanded twice with each algorithm's own step text — see ACB_STAGES.
   TEST'S ARRAY IS SHORTER, and that is the declaration doing its job: test has no lastIndex to save, restore or
   re-read, so its algorithm simply has no step at those stages, and the two it cannot skip past say so rather
   than borrowing @@search's numbers. A rest past the end of its array is a stage past the end, which
   step_stage_check names; the DCHECK below is what catches the two in the middle. */
#define RES_STAGES(X, RECV, STR, PREV, RESET, EXEC) \
    X(RES_RECV,  RECV) \
    X(RES_STR,   STR) \
    X(RES_PREV,  PREV) \
    X(RES_RESET, RESET) \
    X(RES_EXEC,  EXEC)
/* The four @@search reaches and test does not. A SEPARATE list, so test's array simply ENDS after the exec and
   a rest in one of them is a stage past its end — which step_stage_check names on its own. */
#define RES_SEARCH_STAGES(X, CUR, RESTORE, NUL, INDEX) \
    X(RES_CUR,     CUR) \
    X(RES_RESTORE, RESTORE) \
    X(RES_NULL,    NUL) \
    X(RES_INDEX,   INDEX)
enum { RES_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0, 0, 0)
       RES_SEARCH_STAGES(JS_STEP_STAGE_ENUM, 0, 0, 0, 0) };
static const char *const js_re_search_steps[] = {
    RES_STAGES(JS_STEP_STAGE_LABEL,
        "22.2.6.12 steps 1-2 (rx is the this value; the not-an-Object TypeError)",
        "22.2.6.12 step 3 (S is ToString(string))",
        "22.2.6.12 step 4 (previousLastIndex is Get(rx, \"lastIndex\"))",
        "22.2.6.12 step 5.a (Set(rx, \"lastIndex\", +0, true) — skipped when it is already +0)",
        "22.2.6.12 step 6 (result is RegExpExec(rx, S))")
    RES_SEARCH_STAGES(JS_STEP_STAGE_LABEL,
        "22.2.6.12 step 7 (currentLastIndex is Get(rx, \"lastIndex\"))",
        "22.2.6.12 step 8.a (Set(rx, \"lastIndex\", previousLastIndex, true))",
        "22.2.6.12 step 9 (if result is null, return -1)",
        "22.2.6.12 step 10 (return Get(result, \"index\"))")
    NULL };
static const char *const js_re_test_steps[] = {
    RES_STAGES(JS_STEP_STAGE_LABEL,
        "22.2.6.16 steps 1-2 (R is the this value; the not-an-Object TypeError)",
        "22.2.6.16 step 3 (string is ToString(S))",
        "22.2.6.16 — no step: test does not read lastIndex, so @@search's step 4 has no counterpart here",
        "22.2.6.16 — no step: test does not write lastIndex, so @@search's step 5.a has no counterpart here",
        "22.2.6.16 steps 4-5 (match is RegExpExec(R, string); return whether it is not null)")
    NULL };

static int js_re_search_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReSearch *s = st;
    int search = (s->hdr.arg != 0);
    JSValue cur;
    int r;

    /* test's algorithm has no lastIndex steps at all, so its steps array ends at RES_EXEC. A stage past the end
       is what step_stage_check names; the ones IN the middle are this, and there is nothing else that could
       report a machine resting at a step of the other algorithm. */
    DCHECK(search || s->hdr.stage <= RES_EXEC, "RegExp.prototype.test rested in a stage only @@search has");
    if (s->hdr.stage == RES_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->str = JS_UNDEFINED; s->prevLast = JS_UNDEFINED; s->res = JS_UNDEFINED; s->result = JS_UNDEFINED;
        s->pred = JS_UNDEFINED;
        if (!JS_IsObject(s->hdr.this_val)) { JS_ThrowTypeErrorNotAnObject(ctx); return -1; }
        s->hdr.stage = RES_STR;
    }
    if (s->hdr.stage == RES_STR && g_concolic.is(step_arg(&s->hdr, 0))) {
        /* test's and @@search's step 3 over an unknown that is a String in every world (its domain says so):
           §7.1.17 ToString returns a String argument itself, so RegExpExec runs over the unknown, whose
           RegExpBuiltinExec decides from the subject's domain (REX_DOMAIN). */
        JSValue pre = JS_UNDEFINED;
        uint64_t ascii[2];
        int wide = 0;

        if (g_concolic.string_domain(ctx, step_arg(&s->hdr, 0), &pre, ascii, &wide)) {
            JS_FreeValue(ctx, pre);
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            s->str = js_dup(step_arg(&s->hdr, 0));
            s->hdr.stage = RES_EXEC;
        }
    }
    if (s->hdr.stage == RES_STR) {
        r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = search ? RES_PREV : RES_EXEC;
    }
    if (s->hdr.stage == RES_PREV) {
        /* step 4: `? Get(rx, "lastIndex")` — saved so the search leaves no trace */
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_lastIndex, cb_result, &s->prevLast,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* step 5: the reset is SKIPPED when it is already +0, so a `lastIndex` setter does not see a write */
        s->hdr.stage = js_same_value(ctx, s->prevLast, js_int32(0)) ? RES_EXEC : RES_RESET;
    }
    if (s->hdr.stage == RES_RESET) {
        r = step_setprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_lastIndex, js_int32(0), cb_result,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = RES_EXEC;
    }
    if (s->hdr.stage == RES_EXEC) {
        r = step_reexec_run(ctx, &s->hdr, s->hdr.this_val, s->str, s->cbx, cb_result, &s->res,
                            out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (!search) {   /* test steps 4-5 */
            /* An unknown match is a predicate of the match, `match !== null`, through the hook that owns
               §7.2.14 over an unknown. */
            if (g_concolic.is(s->res)) {
                JSValue pair[2] = { js_dup(s->res), JS_NULL };

                if (!g_concolic.cmp(ctx, pair + 2, /*is_neq*/true, JS_CONCOLIC_EQ_STRICT))
                    DFAIL("the concolic comparison hook declined an unknown RegExpExec result");
                s->result = pair[0];
                return 0;
            }
            s->result = js_bool(!JS_IsNull(s->res));
            return 0;
        }
        s->hdr.stage = RES_CUR;
    }
    if (s->hdr.stage == RES_CUR) {
        /* step 7: `? Get(rx, "lastIndex")` again — the exec moved it, or the page's exec did */
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_lastIndex, cb_result, &cur,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        /* step 8: restore, and again only when it actually changed */
        r = js_same_value(ctx, cur, s->prevLast);
        JS_FreeValue(ctx, cur);
        s->hdr.stage = r ? RES_NULL : RES_RESTORE;
    }
    if (s->hdr.stage == RES_RESTORE) {
        /* BORROWED, like every other value handed to a keyed write: the request buffer takes no reference and
           the machine holds `prevLast` across the write in its own slot. A `js_dup` here created a reference
           nobody ever freed — one per @@search whose lastIndex was an OBJECT — and because that object is
           reachable from the page's own graph the leaked reference pinned the entire runtime: 383 objects
           reported at teardown for a four-line fixture. */
        r = step_setprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_lastIndex, s->prevLast, cb_result,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = RES_NULL;
    }
    if (s->hdr.stage == RES_NULL) {
        /* step 9: no match is -1, and nothing more is read */
        if (JS_IsNull(s->res)) { JS_FreeValue(ctx, cb_result); s->result = js_int32(-1); return 0; }
        /* An unknown match is null in some worlds and not in others: the step's two completions are the arms of
           `result !== null`, through the hook that owns §7.2.14 over an unknown. */
        if (g_concolic.is(s->res)) {
            int some = 1;

            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
            if (JS_IsUndefined(s->pred)) {
                JSValue pair[2] = { js_dup(s->res), JS_NULL };

                if (!g_concolic.cmp(ctx, pair + 2, /*is_neq*/true, JS_CONCOLIC_EQ_STRICT))
                    DFAIL("the concolic comparison hook declined an unknown RegExpExec result");
                s->pred = pair[0];
            }
            r = step_tobool_run(ctx, &s->hdr, s->pred, "22.2.6.12 step 9 (if result is null, return -1)", &some);
            if (r) return r < 0 ? -1 : r;
            if (!some) { s->result = js_int32(-1); return 0; }
        }
        s->hdr.stage = RES_INDEX;
    }
    /* step 10: `? Get(result, "index")` — off whatever the exec produced */
    r = step_getprop_run(ctx, &s->hdr, s->res, JS_ATOM_index, cb_result, &s->result, out_cb, out_argc);
    cb_result = JS_UNDEFINED;
    if (r) return r < 0 ? -1 : r;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). cbx is RegExpExec's request buffer, a BORROWED view of the
   receiver, the header's `exec` and this state's `str` — it is not visited, or the clone would over-count what
   those three already are. `prevLast` is the saved lastIndex, which the restore hands back BORROWED for the
   same reason. */
static void js_re_search_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReSearch *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->str);
    v->val(ctx, &s->prevLast);
    v->val(ctx, &s->res);
    v->val(ctx, &s->pred);
}

static JSValue js_re_search_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReSearch *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* 22.2.6.14 RegExp.prototype [ @@split ] ( string, limit ) — a step machine. js_regexp_Symbol_split ran all of
   it from C: ToString(string), SpeciesConstructor's two reads, `? Get(rx,"flags")` and its ToString,
   `? Construct(C, «rx, newFlags»)`, ToUint32(limit), and then an unbounded walk that writes `lastIndex`, calls
   a patchable `exec`, re-reads `lastIndex` and reads every capture off the result.
   It also ToString'd each capture before appending it. Step 13.c.iii.8.b appends nextCapture AS IS — only a
   patched `exec` can produce a non-string one, which is exactly the case that was being coerced away.
*/
/* 22.2.6.14 RegExp.prototype [ @@split ], AS THE SPEC NUMBERS IT. The Repeat is step 17: this machine's
   comments called it step 13, which is where it stood two editions ago, and a stage that cites a step number
   nobody can look up is the same defect as a private counter. The tail stage was 20, out of the run it belongs
   to; it is 14 now and cannot drift, because it moves with its label. */
#define RESPLIT_STAGES(X) \
    X(SPLIT_RECV,      "22.2.6.14 steps 1-2 (rx is the this value; the not-an-Object TypeError)") \
    X(SPLIT_STR,       "22.2.6.14 step 3 (S is ToString(string))") \
    X(SPLIT_CTOR,      "22.2.6.14 step 4 (C is SpeciesConstructor(rx, %RegExp%))") \
    X(SPLIT_FLAGS,     "22.2.6.14 step 5 (Get(rx, \"flags\"))") \
    X(SPLIT_FLAGSTR,   "22.2.6.14 steps 5-7 (flags is ToString of it; unicodeMatching; newFlags is flags with \"y\")") \
    X(SPLIT_CONSTRUCT, "22.2.6.14 step 8 (splitter is Construct(C, <<rx, newFlags>>) — the dispatch)") \
    X(SPLIT_SPLITTER,  "22.2.6.14 steps 8-10 (the constructed splitter arrives; A is ArrayCreate(0); lengthA is 0)") \
    X(SPLIT_LIMIT,     "22.2.6.14 steps 11-16 (lim is ToUint32(limit); the lim 0 return; size; p is 0; q is p)") \
    X(SPLIT_EMPTY,     "22.2.6.14 step 13.a (z is RegExpExec(splitter, S) — the empty-subject case)") \
    X(SPLIT_SETIDX,    "22.2.6.14 step 17.a (Set(splitter, \"lastIndex\", q, true))") \
    X(SPLIT_EXEC,      "22.2.6.14 step 17.b (z is RegExpExec(splitter, S))") \
    X(SPLIT_END,       "22.2.6.14 steps 17.d.i-iv.5 (e is ToLength(Get(splitter, \"lastIndex\")), clamped; T is appended; p is e)") \
    X(SPLIT_NCAPS,     "22.2.6.14 steps 17.d.iv.6-8 (numberOfCaptures is LengthOfArrayLike(z), less one; i is 1)") \
    X(SPLIT_CAP,       "22.2.6.14 step 17.d.iv.9.a (nextCapture is Get(z, ToString(i)))") \
    X(SPLIT_TAIL,      "22.2.6.14 steps 18-20 (T is the substring of S from p to size; append it; return A)")
enum { RESPLIT_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_re_split_steps[] = { RESPLIT_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_re_split_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSReSplit *s = st;
    JSString *strp;
    JSValue sub;
    int r;

    if (s->hdr.stage == SPLIT_RECV) {
        JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        /* FIRST, before anything that can throw: the teardown frees exactly what the state holds. */
        s->str = JS_UNDEFINED; s->ctor = JS_UNDEFINED; s->flags = JS_UNDEFINED;
        s->splitter = JS_UNDEFINED; s->arr = JS_UNDEFINED; s->z = JS_UNDEFINED; s->cap = JS_UNDEFINED;
        s->lim = 0xffffffff; s->size = 0; s->p = 0; s->q = 0;
        s->lengthA = 0; s->e = 0; s->nCaps = 0; s->i = 0; s->unicodeMatching = 0;
        if (!JS_IsObject(s->hdr.this_val)) { JS_ThrowTypeErrorNotAnObject(ctx); return -1; }
        s->hdr.stage = SPLIT_STR;
    }
    if (s->hdr.stage == SPLIT_STR) {
        r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = SPLIT_CTOR;
    }
    if (s->hdr.stage == SPLIT_CTOR) {
        r = step_speciesctor_run(ctx, &s->hdr, s->hdr.this_val, ctx->regexp_ctor, cb_result, &s->ctor,
                                 out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = SPLIT_FLAGS;
    }
    if (s->hdr.stage == SPLIT_FLAGS) {
        r = step_getprop_run(ctx, &s->hdr, s->hdr.this_val, JS_ATOM_flags, cb_result, &s->flags,
                             out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = SPLIT_FLAGSTR;
    }
    if (s->hdr.stage == SPLIT_FLAGSTR) {
        JSValue fs;
        JSString *fp;
        r = step_tostring_run(ctx, &s->hdr, s->flags, cb_result, &fs, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        JS_FreeValue(ctx, s->flags);
        s->flags = fs;
        fp = JS_VALUE_GET_STRING(s->flags);
        /* the 'v' flag implies full Unicode, like 'u' */
        s->unicodeMatching = string_indexof_char(fp, 'u', 0) >= 0 || string_indexof_char(fp, 'v', 0) >= 0;
        /* step 7: the splitter is always STICKY, so the walk controls the position through lastIndex alone */
        if (string_indexof_char(fp, 'y', 0) < 0) {
            s->flags = JS_ConcatString3(ctx, "", s->flags, "y");
            if (JS_IsException(s->flags)) { s->flags = JS_UNDEFINED; return -1; }
        }
        s->hdr.stage = SPLIT_CONSTRUCT;
    }
    if (s->hdr.stage == SPLIT_CONSTRUCT) {
        s->cb[0] = s->ctor;
        s->cb[1] = s->hdr.this_val;
        s->cb[2] = s->flags;
        *out_cb = s->cb; *out_argc = 2;
        s->hdr.stage = SPLIT_SPLITTER;
        return 4;
    }
    if (s->hdr.stage == SPLIT_SPLITTER) {
        s->splitter = cb_result; cb_result = JS_UNDEFINED;
        s->arr = JS_NewArray(ctx);   /* step 9: engine-private, so filling it runs nothing */
        if (JS_IsException(s->arr)) { s->arr = JS_UNDEFINED; return -1; }
        s->hdr.stage = SPLIT_LIMIT;
    }
    if (s->hdr.stage == SPLIT_LIMIT) {
        /* step 11: ToUint32(limit) — a `valueOf` runs there */
        if (!JS_IsUndefined(step_arg(&s->hdr, 1))) {
            int lim32;
            r = step_toint32_run(ctx, &s->hdr, step_arg(&s->hdr, 1), cb_result, &lim32, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->lim = (uint32_t)lim32;
            if (s->lim == 0) return 0;   /* step 12 */
        } else {
            JS_FreeValue(ctx, cb_result); cb_result = JS_UNDEFINED;
        }
        s->size = JS_VALUE_GET_STRING(s->str)->len;
        s->hdr.stage = s->size == 0 ? SPLIT_EMPTY : SPLIT_SETIDX;
    }
    if (s->hdr.stage == SPLIT_EMPTY) {
        /* step 13: an EMPTY subject either matches (and yields nothing) or yields the subject itself */
        r = step_reexec_run(ctx, &s->hdr, s->splitter, s->str, s->cbx, cb_result, &s->z, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        if (!JS_IsNull(s->z)) return 0;
        JS_FreeValue(ctx, s->z); s->z = JS_UNDEFINED;
        s->hdr.stage = SPLIT_TAIL;
    }
    while (s->hdr.stage >= SPLIT_SETIDX && s->hdr.stage <= SPLIT_CAP) {
        strp = JS_VALUE_GET_STRING(s->str);
        if (s->hdr.stage == SPLIT_SETIDX) {
            if (s->q >= s->size) { s->hdr.stage = SPLIT_TAIL; break; }
            /* step 17.a: `? Set(splitter, "lastIndex", q, true)` */
            r = step_setprop_run(ctx, &s->hdr, s->splitter, JS_ATOM_lastIndex, js_int32(s->q), cb_result,
                                 out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->hdr.stage = SPLIT_EXEC;
        }
        if (s->hdr.stage == SPLIT_EXEC) {
            JS_FreeValue(ctx, s->z); s->z = JS_UNDEFINED;
            r = step_reexec_run(ctx, &s->hdr, s->splitter, s->str, s->cbx, cb_result, &s->z, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            if (JS_IsNull(s->z)) {
                s->q = string_advance_index(strp, s->q, s->unicodeMatching);
                s->hdr.stage = SPLIT_SETIDX;
                continue;
            }
            s->hdr.stage = SPLIT_END;
        }
        if (s->hdr.stage == SPLIT_END) {
            /* step 17.d.i: `? ToLength(? Get(splitter, "lastIndex"))` */
            r = step_getlen_run(ctx, &s->hdr, s->splitter, JS_ATOM_lastIndex, cb_result, &s->e,
                                out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            if (s->e > s->size) s->e = s->size;
            if (s->e == s->p) {
                s->q = string_advance_index(strp, s->q, s->unicodeMatching);
                s->hdr.stage = SPLIT_SETIDX;
                continue;
            }
            sub = js_sub_string(ctx, strp, s->p, s->q);
            if (JS_IsException(sub)) return -1;
            if (JS_DefinePropertyValueInt64(ctx, s->arr, s->lengthA++, sub, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
                return -1;
            if (s->lengthA == s->lim) return 0;
            s->p = (uint32_t)s->e;
            s->hdr.stage = SPLIT_NCAPS;
        }
        if (s->hdr.stage == SPLIT_NCAPS) {
            /* step 17.d.iv.6: `? LengthOfArrayLike(z)` — z is whatever a patched exec produced */
            r = step_length_run(ctx, &s->hdr, s->z, cb_result, &s->nCaps, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            s->i = 1;
            s->hdr.stage = SPLIT_CAP;
        }
        while (s->i < s->nCaps) {
            /* step 17.d.iv.9.a: `? Get(z, ToString(i))`, appended AS IS — the spec does not coerce a capture */
            r = step_getidx_run(ctx, &s->hdr, s->z, s->i, cb_result, &s->cap, out_cb, out_argc);
            cb_result = JS_UNDEFINED;
            if (r) return r < 0 ? -1 : r;
            r = JS_DefinePropertyValueInt64(ctx, s->arr, s->lengthA++, s->cap, JS_PROP_C_W_E | JS_PROP_THROW);
            s->cap = JS_UNDEFINED;   /* the define consumed it either way */
            if (r < 0) return -1;
            if (s->lengthA == s->lim) return 0;
            s->i++;
        }
        s->q = s->p;
        s->hdr.stage = SPLIT_SETIDX;
    }
    /* steps 18-19: whatever is left after the last match */
    JS_FreeValue(ctx, cb_result);
    DCHECK(s->hdr.stage == SPLIT_TAIL, "the @@split walk left its loop in an unexpected stage");
    strp = JS_VALUE_GET_STRING(s->str);
    if (s->p > s->size) s->p = s->size;
    sub = js_sub_string(ctx, strp, s->p, s->size);
    if (JS_IsException(sub)) return -1;
    if (JS_DefinePropertyValueInt64(ctx, s->arr, s->lengthA++, sub, JS_PROP_C_W_E | JS_PROP_THROW) < 0)
        return -1;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The result array is `arr` rather than a `result` field —
   @@split builds its answer as it goes — and the two request buffers borrow. */
static void js_re_split_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReSplit *s = st;
    v->val(ctx, &s->arr);
    v->val(ctx, &s->str);
    v->val(ctx, &s->ctor);
    v->val(ctx, &s->flags);
    v->val(ctx, &s->splitter);
    v->val(ctx, &s->z);
    v->val(ctx, &s->cap);
}

static JSValue js_re_split_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReSplit *s = st;
    JSValue r = take_result ? s->arr : JS_UNDEFINED;
    if (take_result) s->arr = JS_UNDEFINED;
    return r;
}

/* The RegExp Legacy Features proposal's static accessors. Every one is `if SameValue(this, %RegExp%) is false,
   throw a TypeError` followed by an internal-slot read — no user code, so a plain C getter is the whole of it.
   `empty` is JS_UNINITIALIZED and is also a TypeError: a getter that answered `undefined` would be reporting
   the last match of a regexp the page is not allowed to observe. */
static JSValue js_regexp_get_legacy_magic(JSContext *ctx, JSValueConst this_val, int slot)
{
    if (!js_same_value(ctx, this_val, ctx->regexp_ctor))
        return JS_ThrowTypeError(ctx, "RegExp legacy static properties are only readable on %%RegExp%% itself");
    if (JS_IsUninitialized(ctx->regexp_legacy[slot]))
        return JS_ThrowTypeError(ctx, "RegExp legacy static property is not available");
    return js_dup(ctx->regexp_legacy[slot]);
}

/* JS_CGETSET_STEP_DEF's getter half is the plain (no-magic) shape, so `input` and `$_` need this one line. */
static JSValue js_regexp_get_input(JSContext *ctx, JSValueConst this_val)
{
    return js_regexp_get_legacy_magic(ctx, this_val, RE_LEGACY_INPUT);
}

/* ONE list expanded twice, so a renumber carries its label with it (JSTrampStepDef.steps). The standard has no
   section number for these: regexp-legacy-features is not in the published edition, so the algorithm is named
   and its steps are the proposal's own (see B64OP_STAGES). */
#define RESETIN_STAGES(X) \
    X(RESETIN_ENTRY,    "proposal-regexp-legacy-features `set RegExp.input` step 1 (the receiver must be " \
                        "%RegExp% itself)") \
    X(RESETIN_TOSTRING, "proposal-regexp-legacy-features `set RegExp.input` step 2 (the value is ToString'd " \
                        "before it is stored)")
enum { RESETIN_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_regexp_set_input_steps[] = { RESETIN_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_regexp_set_input_step(JSContext *ctx, void *st, JSValue cb_result,
                                    JSValue **out_cb, int *out_argc)
{
    JSReSetInput *s = st;
    int r;

    if (s->hdr.stage == RESETIN_ENTRY) {
        s->hdr.stage = RESETIN_TOSTRING;
        s->result = JS_UNDEFINED;
        s->str = JS_UNDEFINED;
        if (!js_same_value(ctx, s->hdr.this_val, ctx->regexp_ctor)) {
            JS_FreeValue(ctx, cb_result);
            JS_ThrowTypeError(ctx, "RegExp legacy static properties are only writable on %%RegExp%% itself");
            return -1;
        }
    }
    r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->str, out_cb, out_argc);
    if (r) return r < 0 ? -1 : r;
    js_regexp_legacy_set(ctx, RE_LEGACY_INPUT, s->str);
    s->str = JS_UNDEFINED;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_regexp_set_input_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSReSetInput *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->str);
}

static JSValue js_regexp_set_input_fini(JSContext *ctx, void *st, bool take_result)
{
    JSReSetInput *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static const JSCFunctionListEntry js_regexp_funcs[] = {
    JS_CFUNC_DEF("escape", 1, js_regexp_escape ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
    /* proposal-regexp-legacy-features. Both spellings of each are the SAME pair of accessor functions in every
       engine that ships this, and the aliases are what real bundles actually use. */
    JS_CGETSET_STEP_DEF("input", js_regexp_get_input, STEPDEF_REGEXP_SET_INPUT ),
    JS_CGETSET_STEP_DEF("$_", js_regexp_get_input, STEPDEF_REGEXP_SET_INPUT ),
    JS_CGETSET_MAGIC_DEF("lastMatch", js_regexp_get_legacy_magic, NULL, RE_LEGACY_LAST_MATCH ),
    JS_CGETSET_MAGIC_DEF("$&", js_regexp_get_legacy_magic, NULL, RE_LEGACY_LAST_MATCH ),
    JS_CGETSET_MAGIC_DEF("lastParen", js_regexp_get_legacy_magic, NULL, RE_LEGACY_LAST_PAREN ),
    JS_CGETSET_MAGIC_DEF("$+", js_regexp_get_legacy_magic, NULL, RE_LEGACY_LAST_PAREN ),
    JS_CGETSET_MAGIC_DEF("leftContext", js_regexp_get_legacy_magic, NULL, RE_LEGACY_LEFT_CONTEXT ),
    JS_CGETSET_MAGIC_DEF("$`", js_regexp_get_legacy_magic, NULL, RE_LEGACY_LEFT_CONTEXT ),
    JS_CGETSET_MAGIC_DEF("rightContext", js_regexp_get_legacy_magic, NULL, RE_LEGACY_RIGHT_CONTEXT ),
    JS_CGETSET_MAGIC_DEF("$'", js_regexp_get_legacy_magic, NULL, RE_LEGACY_RIGHT_CONTEXT ),
    JS_CGETSET_MAGIC_DEF("$1", js_regexp_get_legacy_magic, NULL, RE_LEGACY_PAREN1 + 0 ),
    JS_CGETSET_MAGIC_DEF("$2", js_regexp_get_legacy_magic, NULL, RE_LEGACY_PAREN1 + 1 ),
    JS_CGETSET_MAGIC_DEF("$3", js_regexp_get_legacy_magic, NULL, RE_LEGACY_PAREN1 + 2 ),
    JS_CGETSET_MAGIC_DEF("$4", js_regexp_get_legacy_magic, NULL, RE_LEGACY_PAREN1 + 3 ),
    JS_CGETSET_MAGIC_DEF("$5", js_regexp_get_legacy_magic, NULL, RE_LEGACY_PAREN1 + 4 ),
    JS_CGETSET_MAGIC_DEF("$6", js_regexp_get_legacy_magic, NULL, RE_LEGACY_PAREN1 + 5 ),
    JS_CGETSET_MAGIC_DEF("$7", js_regexp_get_legacy_magic, NULL, RE_LEGACY_PAREN1 + 6 ),
    JS_CGETSET_MAGIC_DEF("$8", js_regexp_get_legacy_magic, NULL, RE_LEGACY_PAREN1 + 7 ),
    JS_CGETSET_MAGIC_DEF("$9", js_regexp_get_legacy_magic, NULL, RE_LEGACY_PAREN1 + 8 ),
};

static const JSCFunctionListEntry js_regexp_proto_funcs[] = {
    JS_CGETSET_DEF("source", js_regexp_get_source, NULL ),
    JS_CGETSET_MAGIC_DEF("global", js_regexp_get_flag, NULL, LRE_FLAG_GLOBAL ),
    JS_CGETSET_MAGIC_DEF("ignoreCase", js_regexp_get_flag, NULL, LRE_FLAG_IGNORECASE ),
    JS_CGETSET_MAGIC_DEF("multiline", js_regexp_get_flag, NULL, LRE_FLAG_MULTILINE ),
    JS_CGETSET_MAGIC_DEF("dotAll", js_regexp_get_flag, NULL, LRE_FLAG_DOTALL ),
    JS_CGETSET_MAGIC_DEF("unicode", js_regexp_get_flag, NULL, LRE_FLAG_UNICODE ),
    JS_CGETSET_MAGIC_DEF("unicodeSets", js_regexp_get_flag, NULL, LRE_FLAG_UNICODE_SETS ),
    JS_CGETSET_MAGIC_DEF("sticky", js_regexp_get_flag, NULL, LRE_FLAG_STICKY ),
    JS_CGETSET_MAGIC_DEF("hasIndices", js_regexp_get_flag, NULL, LRE_FLAG_INDICES ),
    JS_CGETSET_STEP_BOTH_DEF("flags", STEPDEF_REGEXP_FLAGS, -1 ),
    JS_CFUNC_STEP_DEF("exec", 1, STEPDEF_REGEXP_EXEC ),
    JS_CFUNC_STEP_DEF("compile", 2, STEPDEF_REGEXP_COMPILE ),
    JS_CFUNC_STEP_DEF("test", 1, STEPDEF_REGEXP_TEST ),
    JS_CFUNC_STEP_DEF("toString", 0, STEPDEF_REGEXP_TOSTRING ),
    JS_CFUNC_STEP_DEF("[Symbol.replace]", 2, STEPDEF_RE_REPLACE ),
    JS_CFUNC_STEP_DEF("[Symbol.match]", 1, STEPDEF_RE_MATCH ),
    JS_CFUNC_STEP_DEF("[Symbol.matchAll]", 1, STEPDEF_RE_MATCHALL ),
    JS_CFUNC_STEP_DEF("[Symbol.search]", 1, STEPDEF_RE_SEARCH ),
    JS_CFUNC_STEP_DEF("[Symbol.split]", 2, STEPDEF_RE_SPLIT ),
};

static const JSCFunctionListEntry js_regexp_string_iterator_proto_funcs[] = {
    JS_CFUNC_STEP_DEF("next", 0, STEPDEF_RE_STR_ITER_NEXT ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "RegExp String Iterator", JS_PROP_CONFIGURABLE ),
};

void JS_AddIntrinsicRegExpCompiler(JSContext *ctx)
{
    ctx->compile_regexp = js_compile_regexp;
}

int JS_AddIntrinsicRegExp(JSContext *ctx)
{
    JSValue proto, obj;

    proto = ctx->class_proto[JS_CLASS_REGEXP] = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, proto, js_regexp_proto_funcs,
                                   countof(js_regexp_proto_funcs))) {
        return -1;
    }
    /* 22.2.7.1 step 4 performs RegExpBuiltinExec DIRECTLY, with no function object to call. With exec a step
       machine there is nothing to run from C, so the engine holds the algorithm as a callable of its own and
       never exposes it — "perform the algorithm" becomes the same CALL request as every other spelling. */
    ctx->regexp_builtin_exec = JS_NewCFunctionMagic(ctx, NULL, "exec", 1, JS_CFUNC_step, STEPDEF_REGEXP_EXEC);
    if (JS_IsException(ctx->regexp_builtin_exec))
        return -1;
    /* Registered as a STEP machine: cproto JS_CFUNC_step_ctor with the definition id in magic, so both
       `RegExp(x)` and `new RegExp(x)` reach do_step_tramp. The function pointer is unused on that cproto.
       _2 CONSUMES the reference, leaving `obj` borrowed from the global — exactly what
       JS_NewGlobalCConstructor returns, which is why the code below dups rather than owning it. */
    obj = JS_NewCFunction3(ctx, NULL, "RegExp", 2, JS_CFUNC_step_ctor, STEPDEF_RE_CTOR,
                           ctx->function_proto, 0);
    if (JS_IsException(obj))
        return -1;
    JS_NewGlobalCConstructor2(ctx, obj, "RegExp", proto);
    ctx->regexp_ctor = js_dup(obj);
    if (JS_SetPropertyFunctionList(ctx, obj, js_regexp_funcs,
                                   countof(js_regexp_funcs))) {
        return -1;
    }
    static const JSShapeProperty regexp_props[] = {
        {.atom=JS_ATOM_lastIndex, .flags=JS_PROP_WRITABLE},
    };
    static const JSShapeProperty regexp_result_props[] = {
        {.atom=JS_ATOM_length, .flags=JS_PROP_WRITABLE|JS_PROP_LENGTH},
        {.atom=JS_ATOM_index,  .flags=JS_PROP_C_W_E},
        {.atom=JS_ATOM_input,  .flags=JS_PROP_C_W_E},
        {.atom=JS_ATOM_groups, .flags=JS_PROP_C_W_E},
    };
    if (js_new_shape_with(ctx, &ctx->regexp_shape, proto,
                          countof(regexp_props), regexp_props)) {
        return -1;
    }
    if (js_new_shape_with(ctx, &ctx->regexp_result_shape,
                          ctx->class_proto[JS_CLASS_ARRAY],
                          countof(regexp_result_props), regexp_result_props)) {
        return -1;
    }
    proto = ctx->class_proto[JS_CLASS_REGEXP_STRING_ITERATOR] =
        JS_NewObjectProto(ctx, ctx->class_proto[JS_CLASS_ITERATOR]);
    if (JS_IsException(proto))
        return -1;
    if (JS_SetPropertyFunctionList(ctx, ctx->class_proto[JS_CLASS_REGEXP_STRING_ITERATOR],
                                   js_regexp_string_iterator_proto_funcs,
                                   countof(js_regexp_string_iterator_proto_funcs))) {
        return -1;
    }
    JS_AddIntrinsicRegExpCompiler(ctx);
    return 0;
}

