/* JSON */


static void json_parse_record_init_obj(JSContext *ctx, JSONParseRecord *pr, JSValueConst val)
{
    pr->value = js_dup(val);
    pr->u.obj.count = 0;
    pr->u.obj.size = 0;
    pr->u.obj.entries = NULL;
    pr->u.obj.hash_table = NULL;
    pr->u.obj.hash_size = 0;
}

static void json_parse_record_init_array(JSContext *ctx, JSONParseRecord *pr, JSValueConst val)
{
    pr->value = js_dup(val);
    pr->u.array.count = 0;
    pr->u.array.size = 0;
    pr->u.array.elements = NULL;
}

static void json_parse_record_init_primitive(JSContext *ctx, JSONParseRecord *pr, JSValueConst val,
                                             uint32_t source_pos, uint32_t source_len)
{
    pr->value = js_dup(val);
    pr->u.primitive.source_pos = source_pos;
    pr->u.primitive.source_len = source_len;
}

static int json_parse_record_resize_hash(JSContext *ctx, JSONParseRecordObject *po, uint32_t new_hash_size)
{
    uint32_t i, h, *new_hash_table;
    JSONParseRecordEntry *e;

    new_hash_table = js_malloc(ctx, sizeof(new_hash_table[0]) * new_hash_size);
    if (!new_hash_table)
        return -1;
    js_free(ctx, po->hash_table);
    po->hash_table = new_hash_table;
    po->hash_size = new_hash_size;

    for(i = 0; i < po->hash_size; i++) {
        po->hash_table[i] = -1;
    }
    for(i = 0; i < po->count; i++) {
        e = &po->entries[i];
        h = e->atom & (po->hash_size - 1);
        e->hash_next = po->hash_table[h];
        po->hash_table[h] = i;
    }
    return 0;
}

static JSONParseRecord *json_parse_record_add(JSContext *ctx, JSONParseRecord *pr, JSAtom key)
{
    JSONParseRecordObject *po = &pr->u.obj;
    JSONParseRecordEntry *e;
    JSONParseRecord *pr1;
    uint32_t h;

    if (js_resize_array(ctx, (void **)&po->entries, sizeof(po->entries[0]),
                        &po->size, po->count + 1)) {
        return NULL;
    }
    /* switch to hash table when going over size threshold */
    if (po->count >= 8 && (po->count + 1) > po->hash_size) {
        int hash_bits = 32 - clz32(po->count);
        if (json_parse_record_resize_hash(ctx, po, 1 << hash_bits))
            return NULL;
    }

    e = &po->entries[po->count++];
    e->atom = JS_DupAtom(ctx, key);
    pr1 = &e->parse_record;
    pr1->value = JS_UNDEFINED;
    if (po->hash_size != 0) {
        h = key & (po->hash_size - 1);
        e->hash_next = po->hash_table[h];
        po->hash_table[h] = po->count - 1;
    }
    return pr1;
}

static JSONParseRecord *json_parse_record_find(JSONParseRecord *pr, JSAtom key)
{
    JSONParseRecordObject *po = &pr->u.obj;
    JSONParseRecordEntry *e;
    uint32_t h, i;

    if (po->hash_size == 0) {
        /* BACKWARD, because a JSON object may repeat a member name and the LAST one is what the constructed
           object holds -- that member's text is the source of the value the reviver is handed. The hashed path
           below already answered that way (entries are chained newest-first), so a forward scan here made
           `{"b":2,"b":1,"b":4}` lose its source while the same object with nine more members kept it. */
        for(i = po->count; i-- > 0; ) {
            if (po->entries[i].atom == key)
                return &po->entries[i].parse_record;
        }
    } else {
        h = key & (po->hash_size - 1);
        i = po->hash_table[h];
        while (i != -1) {
            e = &po->entries[i];
            if (e->atom == key)
                return &e->parse_record;
            i = e->hash_next;
        }
    }
    return NULL;
}

/* The record tree is exactly as deep as the JSON that built it, so this walk had the SAME page-controlled depth
   json_parse_value did — and flattening only the parser made things WORSE, not better: the parse then accepted
   200k levels and this SEGFAULTED freeing them, where before the parser's stack-overflow guard had rejected the
   input early. Flattening one half of a recursive pair converts a wrong-but-safe RangeError into a crash. Both
   halves or neither.
   POST-ORDER over an explicit stack: a child record lives INSIDE its parent's array, so the parent's arrays can
   only be released once every child is done. */
typedef struct JSONFreeFrame {
    JSONParseRecord *pr;
    int i;                  /* the next child to visit */
    uint8_t kind;           /* 0 = leaf, 1 = array, 2 = object */
} JSONFreeFrame;

static void json_free_parse_record(JSContext *ctx, JSONParseRecord *pr)
{
    JSONFreeFrame inline_st[32], *st = inline_st;
    int sp = 0, st_size = (int)countof(inline_st);
    bool st_heap = false;

    if (!pr)
        return;
    for (;;) {
        /* PUSH pr */
        if (sp >= st_size) {
            int n = st_size * 2;
            JSONFreeFrame *ns;
            if (st_heap) {
                ns = js_realloc(ctx, st, sizeof(*ns) * (size_t)n);
            } else {
                ns = js_malloc(ctx, sizeof(*ns) * (size_t)n);
                if (ns) memcpy(ns, st, sizeof(*ns) * (size_t)st_size);
            }
            /* a TEARDOWN that cannot allocate cannot leave the tree half-released: there is no completion to
               fall back to, which is what CHECK is for. */
            CHECK(ns != NULL, "the JSON parse-record teardown could not grow its walk stack");
            st = ns; st_size = n; st_heap = true;
        }
        st[sp].pr = pr;
        st[sp].i = 0;
        st[sp].kind = 0;
        if (JS_IsObject(pr->value))
            st[sp].kind = js_is_array(ctx, pr->value) ? 1 : 2;
        sp++;

        for (;;) {
            JSONFreeFrame *f = &st[sp - 1];
            if (f->kind == 1 && f->i < f->pr->u.array.count) {
                pr = &f->pr->u.array.elements[f->i++];
                break;                                  /* descend */
            }
            if (f->kind == 2 && f->i < f->pr->u.obj.count) {
                JS_FreeAtom(ctx, f->pr->u.obj.entries[f->i].atom);
                pr = &f->pr->u.obj.entries[f->i].parse_record;
                f->i++;
                break;                                  /* descend */
            }
            /* every child is done, so the node's own storage goes */
            if (f->kind == 1) {
                js_free(ctx, f->pr->u.array.elements);
            } else if (f->kind == 2) {
                js_free(ctx, f->pr->u.obj.entries);
                js_free(ctx, f->pr->u.obj.hash_table);
            }
            JS_FreeValue(ctx, f->pr->value);
            f->pr->value = JS_UNDEFINED; /* fail safe: a second release of the same node is a no-op */
            if (--sp == 0) {
                if (st_heap) js_free(ctx, st);
                return;
            }
        }
    }
}

/* 'pr' can be NULL */

/* ONE STEP of the parse — one completed VALUE. 0 = done (p->val is the result), 1 = MORE (a value completed
   and the parse is offering the thread; call again to continue at the exact point), -1 = threw. The MORE is
   unconditional: this machine offers, and whichever driver took it decides whether anything parks (see the
   back-edge below). */
static int json_parse_step(JSParseState *s, JSONParse *p)
{
    JSContext *ctx = s->ctx;
    /* LOADED into locals and STORED back at each of the three exits. The state is the caller's — that is what
       makes the parse parkable — but the hot loop should not read through a pointer on every token, and naming
       them as locals also keeps `val` from colliding with s->token.val. */
    JSONFrame *st;
    int sp, st_size;
    JSValue val;
    JSONParseRecord *pr_cur, *pr = p->pr_root;
    JSONFrame *top;
    JSONParseRecord *pr1;
    int ret;

    if (p->resume == JPR_START) {
        st = NULL; sp = 0; st_size = 0; val = JS_NULL; pr_cur = pr;
    } else {
        st = p->st; sp = p->sp; st_size = p->st_size; val = p->val; pr_cur = p->pr_cur;
    }
#define JSON_SAVE() do { p->st = st; p->sp = sp; p->st_size = st_size; p->val = val; p->pr_cur = pr_cur; } while (0)
    switch (p->resume) {
    case JPR_DONE_VALUE:   goto value_done_resumed;
    case JPR_ARRAY_ELEMENT:goto array_element;
    case JPR_OBJECT_KEY:   goto object_key;
    default:               break;   /* JPR_START / JPR_VALUE fall into parse_value below */
    }

#define JSON_PUSH_FRAME(container_, is_array_)                                          \
    do {                                                                                \
        if (js_resize_array(ctx, (void **)&st, sizeof(st[0]), &st_size, sp + 1)) {       \
            JS_FreeValue(ctx, (container_));                                            \
            goto fail;                                                                  \
        }                                                                               \
        top = &st[sp++];                                                                \
        top->container = (container_);                                                  \
        top->pr = pr_cur;                                                               \
        top->idx = 0;                                                                   \
        top->prop_name = JS_ATOM_NULL;                                                  \
        top->is_array = (is_array_);                                                    \
    } while (0)

 parse_value:
    if (pr_cur)
        pr_cur->value = JS_UNDEFINED;

    switch(s->token.val) {
    case '{':
        {
            JSValue obj;
            if (json_next_token(s))
                goto fail;
            obj = JS_NewObject(ctx);
            if (JS_IsException(obj))
                goto fail;
            /* A GRANT OF INJECTION-CHANNEL MEMBERSHIP — see js_document_container_grant.
               A server dump rendered into the page as `window.__STATE__=JSON.parse("…")` decided its member
               list in the DOCUMENT'S OWN BYTES exactly as the same dump written as an object literal does, so
               it is the same fact and gets the same grant; the text is what chose the extent, and this is the
               operation that turns that text into a record. Granted PER CONTAINER as the parse builds it, so
               the whole tree is covered without a second walk over the result — and without an ambient
               "a parse is running" flag, which a reviver that parks would carry into another flow.
               js_document_container_grant asks whether the code that called for this parse is the session-variant
               half of the page, so a BUNDLE `JSON.parse` of a fetched config is not on this channel. */
            js_document_container_grant(ctx, obj);
            if (pr_cur)
                json_parse_record_init_obj(ctx, pr_cur, obj);
            JSON_PUSH_FRAME(obj, 0);
            if (s->token.val == '}') {
                if (json_next_token(s))
                    goto fail;
                goto container_done;
            }
            goto object_key;
        }
    case '[':
        {
            JSValue arr;
            if (json_next_token(s))
                goto fail;
            arr = JS_NewArray(ctx);
            if (JS_IsException(arr))
                goto fail;
            /* THE LIST HALF OF THE SAME GRANT, and it is granted here for the identical reason the object
               case above is: the container the text names is the container the walk must be able to descend
               through, and a dump's records live inside its lists. Skipping it would leave the object grant
               above answering for a tree the walk cannot reach past the first `[`. */
            js_document_container_grant(ctx, arr);
            if (pr_cur)
                json_parse_record_init_array(ctx, pr_cur, arr);
            JSON_PUSH_FRAME(arr, 1);
            if (s->token.val == ']') {
                if (json_next_token(s))
                    goto fail;
                goto container_done;
            }
            goto array_element;
        }
    case TOK_STRING:
        val = js_dup(s->token.u.str.str);
        goto primitive;
    case TOK_NUMBER:
        val = s->token.u.num.val;
        goto primitive;
    case TOK_IDENT:
        if (s->token.u.ident.atom == JS_ATOM_false ||
            s->token.u.ident.atom == JS_ATOM_true) {
            val = js_bool(s->token.u.ident.atom == JS_ATOM_true);
        } else if (s->token.u.ident.atom == JS_ATOM_null) {
            val = JS_NULL;
        } else {
            goto def_token;
        }
        goto primitive;
    default:
    def_token:
        if (s->token.val == TOK_EOF) {
            js_parse_error(s, "Unexpected end of JSON input");
        } else {
            js_parse_error(s, "unexpected token: '%.*s'",
                           (int)(s->buf_ptr - s->token.ptr), s->token.ptr);
        }
        goto fail;
    }

 primitive:
    if (pr_cur) {
        json_parse_record_init_primitive(ctx, pr_cur, val,
                                         s->token.ptr - s->buf_start,
                                         s->buf_ptr - s->token.ptr);
    }
    if (json_next_token(s))
        goto fail;
    /* fall through: a completed value belongs to the frame below it, if there is one */

 value_done:
    /* THE BACK-EDGE, once per completed value: a value is the unit of progress, so the offer belongs here
       rather than on the character loop.
       IT OFFERS; IT DOES NOT DECIDE — and that is the correction this line carries. It used to ask
       g_flow_control.preempt ITSELF and return MORE only when the answer was yes, which is a POLICY
       consultation made inside a machine that cannot act on the answer, and both halves of that were wrong.
       On the FLOW path the policy was asked twice per yielded value: once here, and again by the interpreter
       at this machine's `st == 22`, which is the ask that actually parks — one budget consulted from two
       places, which §scheduler forbids for exactly the reason it forbids two schedulers.
       On the EMBEDDER path there is no flow at all. JS_ParseJSON_internal drives this same step from a C
       activation on the HOST's own time — the shipped ABI parses the trusted zone's reply record with it
       BETWEEN two scheduler steps (engine/host/main.c's qjs_provide, and engine_provide's own req2proto
       learning under it) — where the scheduler holds no slice, nothing can park, and the answer is discarded
       by a `while (r > 0)` that simply re-enters. Asking a flow's scheduling policy from there is asking about
       a flow nobody is running, and solver/quantum.c's slice assertion is what said so: every reply the
       shipped extension has ever delivered aborted at it.
       So the machine yields at every completed value and the ONE consultation belongs to whichever driver
       took the MORE — the interpreter asks the preempt hook and parks, the embedder's loop re-enters. That is
       the contract every other step machine already has: quickjs-step.h's JS_STEP_YIELD, "when nobody is
       waiting it is re-entered immediately, which costs one predicted call per iteration". Which is why this
       is a DELETION and not a guard: a guard would be a second predicate deciding which of two behaviours a
       builtin gets. This site paraphrased that sentence INSIDE QUOTATION MARKS and named no file to check it
       against — the words were "per back-edge" and "a machine may ask", and the second of those is the very
       claim the paragraph it was quoted from refuses. */
    p->resume = JPR_DONE_VALUE;
    JSON_SAVE();
    return 1;
 value_done_resumed:
    /* the RESUME lands BELOW the offer. Landing on it would re-offer the instant the flow came back, and a
       driver that keeps taking the offer would never consume a token — a live-lock, not a slow parse. A
       back-edge belongs before the work of an iteration, and a resume belongs after it. */
    if (sp == 0) {
        js_free(ctx, st);
        st = NULL;
        p->resume = JPR_START;
        JSON_SAVE();
        return 0;
    }
    top = &st[sp - 1];
    if (top->is_array) {
        ret = JS_DefinePropertyValueUint32(ctx, top->container, top->idx++, val, JS_PROP_C_W_E);
        val = JS_NULL;
        if (ret < 0)
            goto fail;
        if (s->token.val == ']') {
            if (json_next_token(s))
                goto fail;
            goto container_done;
        }
        if (s->token.val != ',') {
            json_parse_error(s, s->token.ptr, "Expected ',' or ']' after array element");
            goto fail;
        }
        if (json_next_token(s))
            goto fail;
        p->resume = JPR_ARRAY_ELEMENT;
        goto array_element;
    }
    ret = JS_DefinePropertyValue(ctx, top->container, top->prop_name, val, JS_PROP_C_W_E);
    val = JS_NULL;
    JS_FreeAtom(ctx, top->prop_name);
    top->prop_name = JS_ATOM_NULL;
    if (ret < 0)
        goto fail;
    if (s->token.val == '}') {
        if (json_next_token(s))
            goto fail;
        goto container_done;
    }
    if (s->token.val != ',') {
        json_parse_error(s, s->token.ptr, "Expected ',' or '}' after property value");
        goto fail;
    }
    if (json_next_token(s))
        goto fail;
    p->resume = JPR_OBJECT_KEY;
    goto object_key;

 array_element:
    top = &st[sp - 1];
    if (top->pr) {
        if (js_resize_array(ctx, (void **)&top->pr->u.array.elements,
                            sizeof(top->pr->u.array.elements[0]),
                            &top->pr->u.array.size, top->pr->u.array.count + 1))
            goto fail;
        pr1 = &top->pr->u.array.elements[top->pr->u.array.count++];
        pr1->value = JS_UNDEFINED;
    } else {
        pr1 = NULL;
    }
    pr_cur = pr1;
    p->resume = JPR_VALUE;
    goto parse_value;

 object_key:
    top = &st[sp - 1];
    if (s->token.val != TOK_STRING) {
        json_parse_error(s, s->token.ptr, "Expected property name or '}'");
        goto fail;
    }
    top->prop_name = JS_ValueToAtom(ctx, s->token.u.str.str);
    if (top->prop_name == JS_ATOM_NULL)
        goto fail;
    if (json_next_token(s))
        goto fail;
    if (s->token.val != ':') {
        json_parse_error(s, s->token.ptr, "Expected ':' after property name");
        goto fail;
    }
    if (json_next_token(s))
        goto fail;
    if (top->pr) {
        pr1 = json_parse_record_add(ctx, top->pr, top->prop_name);
        if (!pr1)
            goto fail;
    } else {
        pr1 = NULL;
    }
    pr_cur = pr1;
    p->resume = JPR_VALUE;
    goto parse_value;

 container_done:
    /* the frame is complete: it becomes the VALUE the frame below it was waiting for. */
    top = &st[--sp];
    val = top->container;
    top->container = JS_UNDEFINED;
    DCHECK(top->prop_name == JS_ATOM_NULL, "a finished JSON object frame still holds a key");
    goto value_done;

 fail:
    /* the ROOT record owns the whole tree — json_free_parse_record clears each value as it goes, so a child
       already released by a nested failure is a no-op the second time, which is what the recursive version
       relied on too. */
    json_free_parse_record(ctx, pr);
    JS_FreeValue(ctx, val);
    while (sp > 0) {
        top = &st[--sp];
        JS_FreeValue(ctx, top->container);
        JS_FreeAtom(ctx, top->prop_name);
    }
    js_free(ctx, st);
    st = NULL;
    val = JS_NULL;
    p->resume = JPR_START;
    JSON_SAVE();
    return -1;
#undef JSON_PUSH_FRAME
#undef JSON_SAVE
}

/* The C-API driver. JS_ParseJSON is an embedder entry with no flow to park into, so it TAKES every offer back:
   the step yields once per completed value and this loop re-enters it at that exact value, which is the same
   one implementation the JS-visible JSON.parse drives — that one hands the MORE to the interpreter, which asks
   the scheduler whether to park. That is the split the async generator already documents between its tramp
   entry and its resume-as-flow driver; there is no second parser.
   AND THIS DRIVER ASKS THE SCHEDULER NOTHING, which is the property that makes it safe for a host to call on
   its own time between two scheduler steps. It used to ask by proxy — the step consulted the preempt hook
   itself — so an embedder parsing a reply record with no slice open was consulting the budget of a flow it was
   not running. The offer is unconditional now and this loop is the answer to it. */
static JSValue JS_ParseJSON_internal(JSContext *ctx, const char *buf, size_t buf_len,
                                     const char *filename, JSONParseRecord *pr)
{
    JSParseState s1, *s = &s1;
    JSONParse p1, *p = &p1;
    JSValue val = JS_UNDEFINED;
    int r;

    js_parse_init(ctx, s, buf, buf_len, filename, 1);
    p->resume = JPR_START;
    p->pr_root = pr;
    if (json_next_token(s))
        goto fail;
    do {
        r = json_parse_step(s, p);
    } while (r > 0);
    if (r < 0)
        goto fail;
    val = p->val;
    p->val = JS_NULL;
    if (s->token.val != TOK_EOF) {
        if (js_parse_error(s, "unexpected data at the end")) {
            json_free_parse_record(ctx, pr);
            goto fail;
        }
    }
    return val;
 fail:
    JS_FreeValue(ctx, val);
    free_token(s, &s->token);
    return JS_EXCEPTION;
}

/* 'buf' must be zero terminated i.e. buf[buf_len] = '\0'. A flow runs exactly while an activation is on the
   runtime's stack: the base JS_FlowResume installs stays installed on the host's time between steps, when the
   trusted zone's entries parse their records. */
JSValue JS_ParseJSON(JSContext *ctx, const char *buf, size_t buf_len, const char *filename)
{
    if (ctx->rt->current_stack_frame != NULL)
        DFAIL("JS_ParseJSON was called from inside an activation, so the parse cannot rest; a platform parse of "
              "the page's input is Infra §6's call of %JSON.parse% made as a request (core/infra_json.h), and "
              "the engine's own record is JS_ParseJSONRecord");
    return JS_ParseJSON_internal(ctx, buf, buf_len, filename, NULL);
}

JSValue JS_ParseJSONRecord(JSContext *ctx, const char *buf, size_t buf_len, const char *filename)
{
    return JS_ParseJSON_internal(ctx, buf, buf_len, filename, NULL);
}

/* DELETED: internalize_json_property. It was JSON.parse's recursive C reviver walker, and it had been
   unreachable since js_json_reviver_step replaced it — its only remaining caller was its own recursion. Left in
   place it read a value with JS_GetProperty, enumerated with JS_GPN_ENUM_ONLY and invoked the reviver with
   JS_Call, all from C: a reader would have taken it for a live path that runs three of the page's operations
   with no flow base, and the enum-only ratchet counted it as a consumer still to convert. Dead code that LOOKS
   load-bearing is worse than none. */

/* ---- Suspendable JSON.parse reviver (explicit-stack post-order walk; see JSJsonReviver) ---- */
static int jr_push(JSContext *ctx, JSJsonReviver *s) {
    if (s->sp >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        JRFrame *ns = js_realloc(ctx, s->stack, (size_t)nc * sizeof(JRFrame));
        if (!ns) return -1;
        s->stack = ns; s->cap = nc;
    }
    JRFrame *f = &s->stack[s->sp++];
    f->holder = JS_UNDEFINED; f->val = JS_UNDEFINED; f->context = JS_UNDEFINED;
    f->name = JS_ATOM_NULL; f->name_val = JS_UNDEFINED; f->atoms = NULL;
    f->len = 0; f->i = 0; f->is_array = 0; f->fpr = NULL; f->vpr = NULL; f->phase = 0; f->ek = NULL;
    f->applied = JS_UNDEFINED; f->apply_name = JS_ATOM_NULL; f->apply_del = 0;
    return 0;
}
/* descend a HOLDER's parse-record to the child `name` whose value is `cval`, mirroring internalize's pr walk. */
static JSONParseRecord *jr_child_pr(JSContext *ctx, JSONParseRecord *holder_pr, JSAtom name, JSValueConst cval) {
    JSONParseRecord *pr = holder_pr;
    if (!pr) return NULL;
    if (js_is_array(ctx, pr->value)) {
        if (__JS_AtomIsTaggedInt(name)) {
            uint32_t idx = __JS_AtomToUInt32(name);
            pr = (idx < (uint32_t)pr->u.array.count) ? &pr->u.array.elements[idx] : NULL;
        } else pr = NULL;
    } else {
        pr = json_parse_record_find(pr, name);
    }
    if (pr && !js_same_value(ctx, pr->value, cval)) pr = NULL;
    return pr;
}
/* JSON.parse as a STEP builtin. The reviver walk is js_json_reviver_step; the NO-reviver case has no JS re-entry
   at all, but it still PARSES, and a parse is unbounded work — so both cases run through JP_PARSE and both can
   park. internalize_json_property — the recursive C walker that called the reviver through JS_Call — is
   deleted, so there is one walk, not two.

   §25.5.2 steps 2-3: START the parse. The tokenizer (jps) and the frame stack (jp) live ON THE MACHINE from here
   until js_json_parse_finish or the teardown (js_json_reviver_visit), which is the whole point: every
   completed value in between is a place the flow can park with its position in the text intact. */

/* PUT THE TOKENIZER AND THE FRAME STACK ON THE MACHINE, over `text_str`, with `pr_root` the reviver's record or
   NULL when nothing is collecting one. Both parses this machine can run go through it — §25.5.2 step 2's parse
   of the text the algorithm returns a value for, and JP_PROBE's parse of an unknown source's EXAMPLE — because
   they are ONE parse driven from one place, differing only in what they do with the value at the end. A second
   copy of these six lines is how the probe would come to accept a text the real parse rejects. */
static int js_json_parse_state_start(JSContext *ctx, JSJsonReviver *s, JSONParseRecord *pr_root)
{
    size_t len;

    /* text_str is ALREADY step 1's string, so nothing here can run user code or fail on a coercion. */
    DCHECK(JS_IsString(s->text_str), "the JSON parse was handed a text that step 1's ToString had not produced");
    DCHECK(!s->parsing && s->text == NULL, "a JSON parse was started on a machine already holding one");
    s->text = JS_ToCStringLen(ctx, &len, s->text_str);
    if (!s->text)
        return -1;
    /* A C string is the bytes of a String it holds a reference to (js_free_cstring), so the machine owns that
       reference as a value: a deep-fork clone takes a second one, where a shared C string was freed by each arm
       while the other's tokenizer still pointed into it. */
    s->text_val = JS_MKPTR(JS_TAG_STRING, (JSString *)s->text - 1);
    js_parse_init(ctx, &s->jps, s->text, len, "<input>", 1);
    s->jp.resume = JPR_START;
    s->jp.pr_root = pr_root;
    s->parsing = 1;
    if (json_next_token(&s->jps))
        return -1;
    return 0;
}

static int js_json_parse_begin(JSContext *ctx, JSJsonReviver *s)
{
    JSValueConst reviver = step_arg(&s->hdr, 1);
    JSONParseRecord *pr1 = NULL;
    int i;

    if (JS_IsFunction(ctx, reviver)) {
        s->reviver = reviver;
        for (i = 0; i < 5; i++) s->cb_args[i] = JS_UNDEFINED;
        s->pr = js_mallocz(ctx, sizeof(JSONParseRecord));
        if (s->pr) s->pr->ref_count = 1;   /* this machine's reference; a deep-fork clone takes another */
        if (!s->pr) return -1;
        s->root = JS_NewObject(ctx);
        if (JS_IsException(s->root)) return -1;
        json_parse_record_init_obj(ctx, s->pr, s->root);
        pr1 = json_parse_record_add(ctx, s->pr, JS_ATOM_empty_string);
        if (!pr1) return -1;
    } else {
        s->early = 1;   /* no reviver: once the parse is done there is no JS re-entry at all */
    }
    return js_json_parse_state_start(ctx, s, pr1);
}

/* The parse reported DONE: take its value, enforce the EOF, and seed the reviver walk from it. */
static int js_json_parse_finish(JSContext *ctx, JSJsonReviver *s)
{
    JSValue val = s->jp.val;
    s->jp.val = JS_NULL;
    s->parsing = 0;
    if (s->jps.token.val != TOK_EOF) {
        js_parse_error(&s->jps, "unexpected data at the end");
        /* the record's own value goes to UNDEFINED, which is what keeps the reviver's root record from
           releasing this subtree a second time. */
        json_free_parse_record(ctx, s->jp.pr_root);
        JS_FreeValue(ctx, val);
        free_token(&s->jps, &s->jps.token);
        return -1;
    }
    if (s->early) {
        s->result = val;
        return 0;
    }
    if (JS_DefinePropertyValue(ctx, s->root, JS_ATOM_empty_string, val, JS_PROP_C_W_E) < 0)
        return -1;
    if (jr_push(ctx, s) < 0)                            /* the root frame: holder={""}, name="", record=pr */
        return -1;
    s->stack[0].holder = s->root;
    s->stack[0].name = JS_DupAtom(ctx, JS_ATOM_empty_string);
    s->stack[0].fpr = s->pr;
    return 0;
}



/* The reviver walk's phase ceiling — 0..6, where 5 and 6 are the parked proxy-trap requests. Named so the
   DFS invariant below cannot fall behind the phases the walk actually uses. */
#define JR_PHASE_MAX 6

/* THE STAGES OF §25.5.2 JSON.parse ( text [ , reviver ] ).
   A STAGE THAT BUILDS STATE GUARDS ITS BUILDING WITH A PHASE, never with the stage alone. A stage is where a
   resumption LANDS, so a resume re-enters the stage's first line; a builder that ran again there would free the
   incoming cb_result a second time (which is what the base64 machine did) and overwrite the fields it had
   already filled. Every coercing prologue in this file guards its own with str_phase, and JP_PROBE below guards
   its own with probe_phase for the same reason and in the same shape.
   Steps 2-8 are one stage because they are one operation to this engine — the parse — and it is the stage that
   YIELDS, so a resume falls straight back into json_parse_step at the character it stopped on. */
/* THE FIRST STAGE IS A REST POINT AND THAT IS WHY IT IS A STAGE. `text` may be UNKNOWN EXTERNAL INPUT, and
   over such a text §25.5.2 JSON.parse ( text [ , reviver ] ) has two feasible completions — its step 2's
   normal completion and the SyntaxError of §25.5.2.1 ParseJSON ( text ) step 1 — so the machine rests here on
   an OUTCOME FORK while the substrate decides which one this flow is and snapshots a sibling for the other.
   Every ordinary JSON.parse passes straight through it without resting, exactly as a string argument passes
   through JP_TOSTRING without one. */
/* …AND THE STAGE AHEAD OF IT IS THE ONE THAT ANSWERS THE FORK'S QUESTION, WHICH IS A PARSE AND THEREFORE WORK.
   Deciding WHICH completion a session carrying the real bytes reaches is not a comparison and not a shape test:
   §25.5.2.1 ParseJSON ( text ) step 1 states the condition as "if StringToCodePoints(text) is not a valid JSON
   text as specified in ECMA-404", so the only thing that answers it is the ECMA-404 grammar, and the only
   honest way to ask that grammar is to RUN THE PARSER — §Solver-half's rule that an example propagates because
   the engine runs the real op, and §JS-engine-encoding-builtin's ban on hand-rolling JSON, are the same
   sentence read from two directions.
   SO THE ANSWER COSTS A PARSE OF THE EXAMPLE, AND A PARSE IS UNBOUNDED WORK. That is exactly the reason it is
   a STAGE and not a C span inside the ask: it runs on this machine's own tokenizer and frame stack, so it
   YIELDS at every completed value like JP_PARSE does and a flow can park inside it with its position in the
   example intact. Driven to completion from inside the ask instead, it was one span the scheduler could not
   preempt — a document whose unknown text carries a large learned reply body as its example bought a
   cooperative quantum's worth of overrun for one declaration. */
#define JSONPARSE_STAGES(X) \
    X(JP_PROBE,    "§25.5.2 JSON.parse step 2 (ParseJSON over the unknown text's EXAMPLE: which completion a " \
                   "session carrying the real bytes reaches)") \
    X(JP_UNKNOWN,  "§25.5.2 JSON.parse step 1 (text is unknown external input: both completions feasible)") \
    X(JP_TOSTRING, "§25.5.2 JSON.parse step 1 (jsonString is ToString(text))") \
    X(JP_PARSE,    "§25.5.2 JSON.parse steps 2-8 (parseResult is ParseJSON(jsonString), unfiltered its value)") \
    X(JP_WALK,     "§25.5.2 JSON.parse step 9 (InternalizeJSONProperty(root, rootName, reviver, snapshot))")
enum { JSONPARSE_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_json_parse_steps[] = { JSONPARSE_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_json_reviver_step(JSContext *ctx, JSJsonReviver *s, JSValue res, JSValueConst out_args[3]);
static void js_json_reviver_visit(JSContext *ctx, void *st, JSStepVisit *v);

/* CAN A JSON TEXT BEGIN WITH THIS CHARACTER? What a JSON text IS is no longer ECMAScript's to say:
   ECMAScript §25.5.2.1 "ParseJSON ( text )" step 1 hands the whole question to another standard — "If
   StringToCodePoints(text) is not a valid JSON text as specified in ECMA-404, throw a SyntaxError
   exception" — and ECMA-404 allows whitespace either side of the single value a text is, so the first
   character is whitespace or the first character of a value. 0 means the domain guarantees nothing about
   it, and then every completion stays open.
   The EMPTY string is not a JSON text either, which is what lets a DECLARED PREFIX answer for the whole domain
   rather than only for the values that carry it: `location.hash` is "" or "#" followed by the fragment, and
   neither of those is a JSON text.

   THIS NUMBER'S RETIREMENT IS NOT A RENUMBER, WHICH IS WHY IT IS RECORDED SEPARATELY FROM THE ONE AT
   JP_OUTCOME. It stood as `25.5.1's grammar`, naming a production `JSONText : JSONValue`. ECMAScript no
   longer states that grammar ANYWHERE — the clause has no JSON grammar section and the word
   "insignificant" does not occur in it — so the rule did not move to a new number here, it left the
   standard, and the citation had to be re-aimed at the step that DELEGATES rather than at a section that
   restates. ECMA-404 is a standard this tree indexes no corpus for, so nothing here can check a section
   number in it and none is written. Read off the standard's text rather than recalled. */
static bool json_text_may_start_with(int c)
{
    if (c == 0)
        return true;
    switch (c) {
    case ' ': case '\t': case '\n': case '\r':      /* ECMA-404's whitespace, either side of the value */
    case '{': case '[': case '"': case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
    case 't': case 'f': case 'n':                   /* true / false / null */
        return true;
    }
    return false;
}

/* §25.5.2.1 ParseJSON ( text ) step 1's SyntaxError over a text this engine does not have. It is a REAL
   throw — the page's own `catch` runs and everything behind it is reached, which is the whole reason this
   completion is an arm rather than a value. The message invents no position, because there is no text to have
   one in. */
static int js_json_throw_unknown(JSContext *ctx)
{
    JS_ThrowSyntaxError(ctx, "unexpected token in JSON");
    return -1;
}

/* THE TWO COMPLETIONS THIS MACHINE DECLARES FEASIBLE OVER AN UNKNOWN TEXT. 0 is the VALUE — ECMAScript
   §25.5.2 JSON.parse ( text [ , reviver ] ) step 2's normal completion — and 1 is the THROW, §25.5.2.1
   ParseJSON ( text ) step 1's SyntaxError. The value comes first because outcome 0 is what a run with no
   forking policy takes, and a candidate re-fire on its way to a sink must not be diverted into a throw it did
   not choose.
   THE NUMBER MOVED AND THE TITLE IS WHY THIS IS STILL FINDABLE. Comments elsewhere in this machine give
   JSON.parse the number an earlier edition had, and that number now belongs to §25.5.1 JSON.isRawJSON ( obj );
   the parse's SyntaxError also moved out of JSON.parse's own step list into the ParseJSON abstract operation.
   Verified against the standard's text rather than recalled — and the same edition that renumbered these is
   the one this fork already implements, since `JSON.rawJSON` is built here. */
enum { JP_OUTCOME_VALUE = 0, JP_OUTCOME_THROW = 1 };

/* THE PROBE'S CURSOR. PROBE_PH_START is what a js_mallocz'd state already reads as — "nothing recorded" — so a
   completion slot nobody wrote can never be mistaken for the claim that a real session parses this text. */
enum { PROBE_PH_START = 0, PROBE_PH_PARSE, PROBE_PH_DONE };

/* THE MODEL'S OWN THROW, TAKEN OUT OF THE CONTEXT — or propagated when it is not §25.5.2.1 ParseJSON ( text )
 * step 1's SyntaxError.
 *
 * A FAILURE THAT IS NOT THAT SyntaxError IS NOT A COMPLETION OF ANYTHING. The parse's only defined abrupt
 * completion is that SyntaxError; anything else this parser can raise is an allocation failure, and turning
 * one into "the real session throws" would be a claim about the page's value made out of this process's memory
 * pressure. The SyntaxError itself belongs to the MODEL run and may not be left standing in the context — the
 * same rule js_concolic_derive states, for the same reason: a stale pending exception is read by the next
 * thing that asks for one.
 * Returns 1 when it took a SyntaxError, 0 when it left something else pending for the caller to propagate. */
static int json_probe_take_syntax_error(JSContext *ctx)
{
    JSValue e, proto;
    bool syn;

    DCHECK(JS_HasException(ctx), "the JSON probe parse failed and left no completion value behind");
    e = JS_GetException(ctx);
    proto = JS_GetPrototype(ctx, e);
    syn = js_same_value(ctx, proto, ctx->native_error_proto[JS_SYNTAX_ERROR]);
    JS_FreeValue(ctx, proto);
    if (!syn) {
        JS_Throw(ctx, e);
        return 0;
    }
    JS_FreeValue(ctx, e);
    return 1;
}

/* THE PROBE IS OVER: release everything it built and leave the machine owning NOTHING.
 * It is not the teardown of a parse the flow was PARKED inside (js_json_reviver_visit's release); this releases
 * one that RAN TO ITS ANSWER, so its frame stack is already gone (json_parse_step frees it on both its terminating
 * returns) and what is left is the completed value, the token and the source text. Emptying the machine is the
 * point rather than a tidy-up: the ask one stage below snapshots the SIBLING, and its DCHECK says in as many
 * words that the snapshot has to be taken before the machine owns anything. */
static void json_probe_end(JSContext *ctx, JSJsonReviver *s)
{
    DCHECK(s->jp.st == NULL && s->jp.sp == 0,
           "the JSON probe finished with parse frames still standing — json_parse_step frees its own stack on "
           "both terminating returns, so a live frame here is a third exit nothing releases");
    JS_FreeValue(ctx, s->jp.val);
    s->jp.val = JS_NULL;
    free_token(&s->jps, &s->jps.token);
    s->parsing = 0;
    s->text = NULL;
    JS_FreeValue(ctx, s->text_val);
    s->text_val = JS_UNDEFINED;
    JS_FreeValue(ctx, s->text_str);
    s->text_str = JS_UNDEFINED;
}

static int js_json_parse_vstep(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSJsonReviver *s = st;
    int r;
    if (s->hdr.stage == JP_PROBE) {
        JSValueConst text = step_arg(&s->hdr, 0);
        if (s->probe_phase == PROBE_PH_START) {
            /* EVERY owned field before the first thing that can throw — and on THIS stage the first thing that
               can throw is the probe parse itself, so the emptying is the first thing the machine does. The
               failure path tears the state down through fini, which frees exactly what the state holds, so a
               field left unwritten is read as whatever the allocation happened to contain.
               THE EMPTYING LIVES HERE AND NOT AT THE ASK BECAUSE THE PROBE RUNS FIRST AND OWNS THINGS. It used
               to sit one stage below, where it was reached before anything had been built; a stage that starts
               a parse ahead of it would have had its tokenizer, its source text and its example overwritten
               with empty values while the allocations they named went unreleased. */
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            s->root = JS_UNDEFINED; s->result = JS_UNDEFINED; s->text_str = JS_UNDEFINED;
            s->unknown = JS_UNDEFINED;
            s->early = 0; s->text = NULL; s->text_val = JS_UNDEFINED;
            s->pr = NULL; s->stack = NULL; s->sp = 0; s->cap = 0;
            s->parsing = 0;
            s->probe_real = JS_OUTCOME_REAL_UNSTATED;
            s->probe_phase = PROBE_PH_DONE;   /* …unless the branch below finds an example to run */
            if (g_concolic.is(text)) {
                /* A NON-STRING EXAMPLE IS "CANNOT SAY", AND IT IS THE SAME LINE THE PARSE ARM DRAWS. §25.5.2
                   JSON.parse ( text [ , reviver ] ) step 1 is `? ToString(text)`, so an example that is not
                   already a String would have to be coerced before the parse — and the parse arm below refuses
                   exactly that (it takes the example only `if (JS_IsString(example))`). Declaring a completion
                   from a coercion the arm would not perform would make the declaration and the arm two answers
                   to one question. */
                JSValue example = g_concolic.example(ctx, text);
                if (JS_IsString(example)) {
                    s->text_str = example;
                    s->probe_phase = PROBE_PH_PARSE;
                    /* NO PARSE RECORD AND NO REVIVER: the probe wants the COMPLETION, not the value, and
                       §25.5.2 step 9's InternalizeJSONProperty runs on the arm the fork keeps rather than on
                       the question that chooses it. Running the reviver here would call the page's function
                       once per node for a walk no completion of the algorithm performs. */
                    if (js_json_parse_state_start(ctx, s, NULL)) {
                        /* the tokenizer refused the very first token — §25.5.2.1 ParseJSON ( text ) step 1's
                           SyntaxError, reached before json_parse_step ever runs, and a completion of the probe
                           like any other. */
                        if (!json_probe_take_syntax_error(ctx)) {
                            json_probe_end(ctx, s);
                            return -1;
                        }
                        s->probe_real = JP_OUTCOME_THROW;
                        json_probe_end(ctx, s);
                        s->probe_phase = PROBE_PH_DONE;
                    }
                } else {
                    JS_FreeValue(ctx, example);
                }
            }
        }
        if (s->probe_phase == PROBE_PH_PARSE) {
            /* THE REST POINT, one offer per completed value — the same back-edge JP_PARSE hands the scheduler,
               because it is the same parser stepped the same way. A YIELD returns with the stage still JP_PROBE
               and this phase still PROBE_PH_PARSE, so the resume falls straight back in here and continues at
               the exact character it stopped on. */
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            r = json_parse_step(&s->jps, &s->jp);
            if (r > 0)
                return JS_STEP_YIELD;
            if (r < 0) {
                if (!json_probe_take_syntax_error(ctx)) {
                    json_probe_end(ctx, s);
                    return -1;
                }
                s->probe_real = JP_OUTCOME_THROW;
            } else {
                /* §25.5.2.1 ParseJSON ( text ) step 1 asks whether the WHOLE text is a valid JSON text as
                   specified in ECMA-404, so a parse that completed a value and left data behind it has not
                   answered yes — `{}x` is the same SyntaxError as `x`. It is recorded rather than raised: the
                   probe reports a completion, and the only throw the page may see is the one the fork's THROW
                   arm raises for itself. */
                s->probe_real = (s->jps.token.val == TOK_EOF) ? JP_OUTCOME_VALUE : JP_OUTCOME_THROW;
            }
            json_probe_end(ctx, s);
            s->probe_phase = PROBE_PH_DONE;
        }
        DCHECK(s->probe_phase == PROBE_PH_DONE,
               "JSON.parse left its probe stage without recording a completion — the ask one stage below reads "
               "that slot, and a phase short of DONE is the one state in which it holds nothing");
        s->hdr.stage = JP_UNKNOWN;
    }
    if (s->hdr.stage == JP_UNKNOWN) {
        JSValueConst text = step_arg(&s->hdr, 0);
        if (s->hdr.fork_phase == FORK_PH_ASK) {
            /* The SIBLING's snapshot is taken AT the ask, so it re-enters here rather than at JP_PROBE and its
               `cb_result` is the driver's, owed a release exactly as the first entry's was. What it does NOT
               have to redo is the emptying: the clone was taken with the machine owning nothing, which is what
               this asserts rather than assumes. */
            JS_FreeValue(ctx, cb_result);
            cb_result = JS_UNDEFINED;
            DCHECK(JS_IsUndefined(s->root) && JS_IsUndefined(s->result) && JS_IsUndefined(s->text_str)
                   && JS_IsUndefined(s->unknown) && !s->early && s->text == NULL && s->pr == NULL
                   && s->stack == NULL && s->sp == 0 && s->cap == 0 && !s->parsing,
                   "JSON.parse reached its outcome fork owning state — the probe one stage above releases what "
                   "it built precisely so the sibling's snapshot is taken of a machine that owns nothing");
        }
        if (g_concolic.is(text)) {
            int arm, real = JS_OUTCOME_REAL_UNSTATED;
            if (!json_text_may_start_with(g_concolic.lead(text))) {
                /* THE PARSE ARM IS INFEASIBLE and is pruned rather than forked. The component that owns this
                   source declares what the browser's delivery guarantees — a fragment is empty or begins with
                   `#` — and neither an empty text nor one beginning with `#` is a JSON text, so §25.5.2.1
                   ParseJSON ( text ) step 1 throws for EVERY value the domain permits. That is V8's answer to
                   `JSON.parse(location.hash)` and it is reached here the same way V8 reaches it: by the
                   grammar, not by a special case.
                   THE DOMAIN AND THE EXAMPLE ARE TWO WITNESSES TO ONE FACT, so they are checked against each
                   other where both are in hand. The example is a member of the domain, so a domain that admits
                   no JSON text cannot carry an example the REAL parser accepted — and if it does, the source's
                   component is declaring a prefix its own deliveries do not honour, which would silently prune
                   an arm the page really reaches. */
                DCHECK(s->probe_real != JP_OUTCOME_VALUE,
                       "a source declared a leading character no JSON text may begin with, and the REAL parser "
                       "then accepted that same source's example — the domain and the example disagree, and "
                       "the domain is about to delete a completion the example proves feasible");
                JS_FreeValue(ctx, cb_result);
                return js_json_throw_unknown(ctx);
            }
            /* THE DECLARATION, MADE ON THE ENTRY THAT ASKS. JP_OUTCOME_VALUE / JP_OUTCOME_THROW above name the
               two completions and the JP_PROBE stage recorded which one the REAL codec reaches on this source's
               example — so the flow that carries the example keeps that arm and the sibling holding the other
               is FORCED, which is what an outcome fork previously could not say about the requests built behind
               it.
               ONLY ON THE ASK, because that is the only entry a declaration is read on: step_fork_run consults
               `real` in FORK_PH_ASK and the FORK_PH_ANSWERED re-entry is the DELIVERY of a decision already
               taken. Reading it there would state a completion into a slot with no reader.
               THE SIBLING INHERITS THE RECORDING RATHER THAN REPEATING IT. Its snapshot is taken AT the ask, so
               it re-enters here with the probe already DONE and its recorded completion copied — which is the
               same answer over the same example, computed once instead of once per arm. */
            if (s->hdr.fork_phase == FORK_PH_ASK)
                real = s->probe_real;
            r = step_fork_run(ctx, &s->hdr, text, "JSON.parse", 2, real, &arm);
            if (r) { JS_FreeValue(ctx, cb_result); return r; }
            if (arm == JP_OUTCOME_THROW) {
                /* the THROW completion, raised for real so the page's own `catch` runs and whatever is behind
                   it is reached — which is the whole reason this is an arm and not a value. */
                JS_FreeValue(ctx, cb_result);
                return js_json_throw_unknown(ctx);
            }
        }
        s->hdr.stage = JP_TOSTRING;
        if (g_concolic.is(text)) {
            /* THE PARSE COMPLETION, over a text this engine does not have. What it DOES have is the source's
               EXAMPLE — the concrete fragment this document was loaded with — and §solver's rule is that an
               example propagates by RUNNING THE REAL OPERATION on it, so the example becomes this machine's
               text and the ordinary parse (and the ordinary reviver walk) runs on it. The value handed back to
               the page is that result carried by an unknown derived from `text`; fini does that in one place.
               With no example there is nothing concrete to parse, and the completion is an unknown with no
               example — which is honest, not a stub: the domain is "some JSON value" and nothing narrows it.

               NAMED RESIDUAL — THE VALUE ARM PARSES THE EXAMPLE A SECOND TIME. JP_PROBE has already run the
               real parser over these exact bytes to declare the completion, and this arm runs it again to
               produce the value, so a flow on the value arm pays two parses of one example. Both are parkable,
               so nothing here is a cap; what it costs is CPU. WHAT THE NEXT DIFF BUILDS is the handover: the
               probe keeps its parsed value and its parse record instead of releasing them in json_probe_end,
               and this arm adopts them — which requires js_json_reviver_visit to declare the tokenizer and the
               frame stack it does not name today, because the ask between the two clones the machine for the
               sibling and a memcpy'd `text` would be freed twice. HOW ITS ABSENCE SHOWS: the parse time for an
               unknown text's example is exactly double the parse time for the same bytes handed to JSON.parse
               concretely — measurable on a document whose unknown text carries a large learned reply body. */
            JSValue example = g_concolic.example(ctx, text);
            s->unknown = js_dup(text);
            if (JS_IsString(example)) {
                s->text_str = example;
                s->hdr.stage = JP_PARSE;
                if (js_json_parse_begin(ctx, s))
                    return -1;
            } else {
                JS_FreeValue(ctx, example);
                if (JS_IsFunction(ctx, step_arg(&s->hdr, 1)))
                    DFAIL("JSON.parse over unknown text WITH a reviver and no example to parse — §25.5.2 step 9 "
                          "calls the reviver once per node of a structure this arm does not have, so its calls "
                          "and their side effects would silently not happen; build the unknown-structure walk");
                s->early = 1;   /* nothing to parse and no walk: the completion is the derived unknown */
                return 0;
            }
        }
    }
    if (s->hdr.stage == JP_TOSTRING) {
        /* §25.5.2 step 1: `? ToString(text)`. JS_ToCStringLen ran it from C, so `JSON.parse({toString(){…}})` had
           its toString driven with no flow base. */
        r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->text_str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r;
        s->hdr.stage = JP_PARSE;
        if (js_json_parse_begin(ctx, s))
            return -1;
    }
    if (s->hdr.stage == JP_PARSE) {
        /* THE PARSE, one completed value per entry. A YIELD returns with the stage still JP_PARSE, so the
           resume falls straight back in here and json_parse_step continues at the exact character it stopped
           on — the tokenizer's position and the frame stack are fields of this machine, not C locals. */
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        r = json_parse_step(&s->jps, &s->jp);
        if (r > 0)
            return 22;
        if (r < 0) {
            s->parsing = 0;
            free_token(&s->jps, &s->jps.token);
            if (!JS_IsUndefined(s->unknown)) {
                /* THE EXAMPLE CONTRADICTS THE ARM THIS FLOW IS. This flow is the PARSE completion of an
                   unknown text; the concrete example the source carries happens not to be a JSON text, which
                   says something about the example and nothing about the arm — §solver's rule is exactly that:
                   the example marks the real arm primary and the arm it contradicts DROPS it rather than the
                   arm. So the completion stays a parse, with an unknown that has no example. The throw
                   belonged to the model run and is taken out of the context, never left for the next code that
                   asks for a pending exception. */
                DCHECK(JS_HasException(ctx), "the JSON parse failed and left no completion value behind");
                JS_FreeValue(ctx, JS_GetException(ctx));
                if (!s->early)
                    DFAIL("JSON.parse over unknown text WITH a reviver whose example is not a JSON text — the "
                          "parse arm has no structure for §25.5.2 step 9 to walk, so the reviver's calls and "
                          "their side effects would silently not happen; build the unknown-structure walk");
                JS_FreeValue(ctx, s->text_str);
                s->text_str = JS_UNDEFINED;
                s->result = JS_UNDEFINED;
                return 0;
            }
            return -1;
        }
        if (js_json_parse_finish(ctx, s))
            return -1;
        s->hdr.stage = JP_WALK;
        if (s->early)
            return 0;
    }
    r = js_json_reviver_step(ctx, s, cb_result, (JSValueConst *)&s->cb_args[2]);
    if (r < 0) return -1;
    if (r == 0) return 0;
    while (r == 2) {
        /* the frame just ENTERED the key walk, which has no request of its own yet; run the cursor's first step
           rather than returning a code the driver has no meaning for. */
        r = js_json_reviver_step(ctx, s, JS_UNDEFINED, (JSValueConst *)&s->cb_args[2]);
        if (r < 0) return -1;
        if (r == 0) return 0;
    }
    if (r == 5 || r == 6 || r == 10 || r == 11 || r == 12 || r == 15) {
        /* an operation the walk owes: §25.5.2.4's `Get(holder, name)`, LengthOfArrayLike's read and its
           ToLength, the apply's CreateDataProperty or bare [[Delete]], or the enumerable-key cursor's
           [[OwnPropertyKeys]] / [[GetOwnProperty]]. The machine relays the requester's own buffer, which
           holds the operands across the
           request. Every code the walk can produce is listed, and one it cannot answer for falls through to the
           reviver CALL below with an undefined callee — so the list is asserted rather than assumed. */
        *out_cb = s->ek_cb; *out_argc = s->ek_argc;
        return r;
    }
    DCHECK(r == 1, "the JSON reviver walk produced a step code its driver does not relay");
    /* cb_args[0] (the holder, i.e. `this`) is set by the step itself; cb_args[1] is the reviver, which the caller
       supplies — omitting it left the driver calling an undefined callee, so the reviver was never invoked. */
    s->cb_args[1] = s->reviver;
    *out_cb = s->cb_args; *out_argc = 3;
    return 3;
}

static JSValue js_json_parse_vfini(JSContext *ctx, void *st, bool take_result)
{
    JSJsonReviver *s = st;
    /* TAKEN BEFORE THE DISCHARGE, which is what frees this field. The derivation below is the last thing
       §25.5.2 does on this arm and it needs the source that named it. */
    JSValue unknown = s->unknown;
    JSValue r;
    s->unknown = JS_UNDEFINED;
    /* `text_val`, `text_str`, a parse still in flight, the walk's frame stack and `result` are all declared
       (js_json_reviver_visit), and every one of them is left intact for the discharge — an exception or a fork
       arriving from inside one of the walk's requests is released through that list. */
    r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    if (!JS_IsUndefined(unknown)) {
        /* THE COMPLETION OF THE PARSE ARM IS AN UNKNOWN DERIVED FROM THE TEXT, carrying whatever the REAL
           codec made of that source's example. Not the bare parsed value: the page is holding something it
           read out of `location.hash`, so a branch on it must still fork and a sink fed from it must still
           solve for that source. Not a bare unknown either — that would throw away the one concrete thing the
           engine ran the real operation to get. */
        if (take_result) {
            /* `early` is set exactly when no reviver is callable, and then step 4 returns ParseJSON's value. */
            r = s->early ? g_concolic.json_parse(ctx, unknown, r) : js_concolic_derive(ctx, unknown, "JSON.parse", r);
        }
        JS_FreeValue(ctx, unknown);
    }
    return r;
}

/* Drive the DFS until the next reviver call is needed (return 1, out_args=[name,val,context]) or done (0).
   `res` (owned) is the reviver's result for the just-completed node, or JS_UNDEFINED on the first call.

   THE SUB-STEP NUMBERS IN THIS WALK MOVED WITH THE SECTION, AND NOT BY THE SAME OFFSET AS THE SECTION.
   Every `25.5.1.1` here named InternalizeJSONProperty, which is
   ECMAScript §25.5.2.4 "InternalizeJSONProperty ( holder, name, reviver, parseRecord )". Renumbering the
   SECTION and keeping the
   sub-numbers would have certified four wrong ones: the object arm's key read was `3.c` and is `5.c.i`, the
   array arm's length read was `2.b.i` and is `5.b.ii`, and the write-back pair was `2.b.ii.3/4` and
   `2.c.ii.2/3` and is `5.b.iv.4.a/5.a` and `5.c.ii.3.a/4.a` — the algorithm gained a parse-record arm ahead
   of the object test, so what was step 2 is step 5. Only `step 1`, the `Get`, is where it was. The
   OPERANDS were renamed with it (`val`/`P` are now `value`/`name`/`propertyKey`), and so was the operation
   the key read calls: EnumerableOwnPropertyNames is now §7.3.23 EnumerableOwnProperties ( obj, kind ).
   Counted against the section's own list with nesting depth tracked, not by a flat count of items. */
static int js_json_reviver_step(JSContext *ctx, JSJsonReviver *s, JSValue res, JSValueConst out_args[3]) {
    if (s->sp > 0 && s->stack[s->sp - 1].phase == 3) {
        /* the enumerable-key walk is in flight: `res` is the last request's answer, not a reviver result. */
        JRFrame *f = &s->stack[s->sp - 1];
        JSValue *ekcb = NULL; int ekargc = 0;
        int r = js_enum_keys_run(ctx, &s->hdr, f->ek, res, &ekcb, &ekargc);
        if (r > 0) { s->ek_cb = ekcb; s->ek_argc = ekargc; return r; }
        if (r < 0) return -1;
        /* the survivors become the frame's key list; the cursor's allocation is handed over whole. §25.5.2.4
           step 5.c.i is EnumerableOwnProperties, so the non-enumerable ones go first. */
        js_enum_keys_keep_enumerable(ctx, f->ek);
        f->atoms = f->ek->atoms; f->len = f->ek->kept;
        f->ek->atoms = NULL; f->ek->len = 0;
        js_enum_keys_free(ctx, f->ek);
        js_free(ctx, f->ek); f->ek = NULL;
        f->phase = 1;
        res = JS_UNDEFINED;
    }
    if (s->sp > 0 && (s->stack[s->sp - 1].phase == 4 || s->stack[s->sp - 1].phase == 6)) {
        /* a read landed: `res` is the value or the raw `length`, not a reviver result. The frame's own arm below
           consumes it. */
    } else if (s->sp > 0 && s->stack[s->sp - 1].phase == 5) {
        /* the apply's own request answered; the arm below re-enters the sub-sequence with it. */
    } else if (s->sp > 0 && s->stack[s->sp - 1].phase == 2) {   /* a node's reviver just returned `res` */
        JRFrame *f = &s->stack[s->sp - 1];
        JSAtom fname = f->name;                          /* keep for the parent apply */
        if (f->atoms) js_free_prop_enum(ctx, f->atoms, (uint32_t)f->len);
        JS_FreeValue(ctx, f->name_val);
        JS_FreeValue(ctx, f->val);
        JS_FreeValue(ctx, f->context);
        s->sp--;
        if (s->sp == 0) { JS_FreeAtom(ctx, fname); s->result = res; return 0; }   /* root: done */
        {
            /* §25.5.2.4 steps 5.b.iv.4.a/5.a (and 5.c.ii.3.a/4.a): `? value.[[Delete]](propertyKey)` or
               `? CreateDataProperty(value, propertyKey, newElement)`. Both are the page's code the moment
               `value` is a Proxy or carries a non-configurable/non-writable property — and a reviver runs
               BOTTOM-UP with `this` bound to
               the holder, so it can plant a proxy on the very object its parent is about to write back into.
               JS_DeleteProperty / JS_DefinePropertyValue ran both from C. The apply is now a parked
               sub-sequence, and the frame owns the operands across it because the machine's C locals are gone
               when it resumes. */
            JRFrame *p = &s->stack[s->sp - 1];
            p->apply_del = JS_IsUndefined(res);
            if (p->apply_del) JS_FreeValue(ctx, res);
            else p->applied = res;                       /* owned until the define has been issued */
            p->apply_name = fname;                       /* owned; the sub-sequence dups it */
            p->phase = 5;
            res = JS_UNDEFINED;
        }
    } else {
        JS_FreeValue(ctx, res);                          /* JS_UNDEFINED on the first call */
    }
    if (s->sp > 0 && s->stack[s->sp - 1].phase == 5) {
        JRFrame *p = &s->stack[s->sp - 1];
        int r = p->apply_del
            ? step_delprop_run(ctx, &s->hdr, p->val, p->apply_name, res, &s->ek_cb, &s->ek_argc)
            : step_defprop_run(ctx, &s->hdr, p->val, p->apply_name, p->applied, false, res,
                               &s->ek_cb, &s->ek_argc);
        if (r > 0) return r;
        if (r < 0) return -1;
        JS_FreeAtom(ctx, p->apply_name); p->apply_name = JS_ATOM_NULL;
        JS_FreeValue(ctx, p->applied); p->applied = JS_UNDEFINED;
        p->phase = 1;
        p->i++;
        res = JS_UNDEFINED;
    }
    while (s->sp > 0) {
        JRFrame *f = &s->stack[s->sp - 1];
        /* DFS invariants: a live frame stack within its allocation, a valid phase, and (once past init) a child
           cursor that never runs past the key count. A violation is a walk-state bug; crash at its origin.
           THE PHASE BOUND WAS STALE. It read `phase <= 4` while the walk had grown phases 5 and 6 for the
           parked proxy-trap requests, so a revoked-proxy revive tripped an invariant that was simply out of
           date — the walk was correct and the assertion about it was not. It was a plain assert, compiled out
           by the NDEBUG the harness builds with, so it never had a chance to say so. The bound is a named
           constant now: a seventh phase updates one place, and the DCHECK cannot silently fall behind it. */
        DCHECK(s->sp <= s->cap && f->phase <= JR_PHASE_MAX, "s->sp <= s->cap && f->phase <= JR_PHASE_MAX");
        DCHECK(f->phase == 0 || f->phase == 4 || f->i <= f->len, "f->phase == 0 || f->phase == 4 || f->i <= f->len");
        if (f->phase == 0) {
            /* §25.5.2.4 step 1: `Let value be ? Get(holder, name)`. The holder is parser-built at the root, but a
               reviver returns arbitrary values into it, so by the time a deeper frame reads one the holder can be
               a Proxy — its `get` trap, run from C by JS_GetProperty with no flow base. A request now, parked in
               phase 4. */
            s->ek_cb = &f->holder;   /* borrowed: the frame holds it across the request */
            s->ek_argc = (int)f->name;
            f->phase = 4;
            return 6;
        }
        if (f->phase == 4) {
            f->val = res; res = JS_UNDEFINED;
            if (JS_IsException(f->val)) { f->val = JS_UNDEFINED; return -1; }
            f->vpr = jr_child_pr(ctx, f->fpr, f->name, f->val);   /* val's own parse record */
            f->context = JS_NewObject(ctx);
            if (JS_IsException(f->context)) return -1;
            if (JS_IsObject(f->val)) {
                f->is_array = js_is_array(ctx, f->val);
                if (f->is_array < 0) return -1;
                if (f->is_array) {
                    /* §25.5.2.4 step 5.b.ii: `? LengthOfArrayLike(value)`, which is
                       `? ToLength(? Get(value, "length"))`. js_get_length32 ran BOTH halves from C — the read
                       is a Proxy `get` trap or an accessor, and
                       the ToLength coerces whatever it produced — and it truncated to 32 bits where ToLength
                       reaches 2^53-1. The shared sub-sequence performs both. */
                    f->phase = 6;
                    res = JS_UNDEFINED;
                    goto array_len;
                }
                else {
                    /* §25.5.2.4 step 5.c.i: `? EnumerableOwnProperties(value, key)`. It was JS_GPN_ENUM_ONLY from
                       C, which runs a Proxy's `ownKeys` and per-key `getOwnPropertyDescriptor` traps with no flow
                       base — reachable, because a reviver runs bottom-up with `this` bound to the holder and can
                       plant a proxy on a sibling key this walk has not reached yet. It is the shared cursor now:
                       the frame parks in phase 3 and the driver relays its requests. */
                    f->ek = js_mallocz(ctx, sizeof(*f->ek));
                    if (!f->ek) return -1;
                    js_enum_keys_init(f->ek);
                    f->ek->obj = js_dup(f->val);
                    f->phase = 3;
                    return 2;   /* the driver re-enters with the cursor's request */
                }
            } else if (f->vpr) {   /* primitive with a source record -> context.source */
                JSValue src = JS_NewStringLen(ctx, s->text + f->vpr->u.primitive.source_pos, f->vpr->u.primitive.source_len);
                if (JS_IsException(src)) return -1;
                /* f->context is this walk's own fresh object, so its define is not the page's code */
                if (JS_DefinePropertyValue(ctx, f->context, JS_ATOM_source, src, JS_PROP_C_W_E) < 0) return -1;
            }
            f->phase = 1;
        }
        if (f->phase == 6) {
        array_len:
            {
                int r = step_length_run(ctx, &s->hdr, f->val, res, &f->len, &s->ek_cb, &s->ek_argc);
                res = JS_UNDEFINED;
                if (r > 0) return r;
                if (r < 0) return -1;
                f->phase = 1;
            }
        }
        if (f->i < f->len) {                             /* descend into child i */
            JSAtom prop;
            if (f->is_array) { prop = JS_NewAtomInt64(ctx, f->i); if (prop == JS_ATOM_NULL) return -1; }
            else prop = JS_DupAtom(ctx, f->atoms[f->i].atom);
            JSValueConst pval = f->val; JSONParseRecord *pvpr = f->vpr;   /* snapshot before push may realloc s->stack */
            if (jr_push(ctx, s) < 0) { JS_FreeAtom(ctx, prop); return -1; }
            JRFrame *child = &s->stack[s->sp - 1];
            child->holder = pval; child->name = prop; child->fpr = pvpr;   /* child's holder-record = this val's record */
            continue;
        }
        /* children done -> call the reviver on this node (SUSPEND) */
        f->name_val = JS_AtomToValue(ctx, f->name);
        if (JS_IsException(f->name_val)) return -1;
        s->cb_args[0] = f->holder;                       /* this = holder (borrowed) */
        out_args[0] = f->name_val; out_args[1] = f->val; out_args[2] = f->context;
        f->phase = 2;
        return 1;
    }
    return 0;
}
/* WHAT AN ENUMERABLE-KEY CURSOR OWNS: a frame holds one while phase 3's key walk is in flight. */
static void js_enum_keys_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    JSEnumKeys *ek = elem;
    v->val(ctx, &ek->obj);
    v->props(ctx, &ek->atoms, ek->len);
    /* the key+VALUE form's collected values. They were missing from this list, which no consumer noticed while
       every machine reading the declaration used the key-only form — import()'s attribute walk is the one that
       wants values, and converting its teardown to the declaration leaked 390 objects per corpus run the moment
       it did. cb is the request buffer and borrows `obj`. */
    v->array(ctx, (void **)&ek->vals, sizeof(JSValue), (int)ek->kept, (int)ek->len, js_step_visit_value_elem);
}

/* WHAT ONE WALK FRAME OWNS. `holder` is borrowed from its parent's `val`, and fpr/vpr are borrowed into the
   parse record — which the machine SHARES rather than copies, so they stay valid in a clone. Everything else
   is the frame's, including a cursor parked mid-request: the exception that tears a frame down can arrive from
   inside any of its twelve requests, and so can a fork. */
static void js_jrframe_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    JRFrame *f = elem;
    v->array(ctx, (void **)&f->ek, sizeof(JSEnumKeys), 1, 1, js_enum_keys_visit);
    v->props(ctx, &f->atoms, (uint32_t)f->len);
    v->val(ctx, &f->name_val);
    v->val(ctx, &f->val);
    v->val(ctx, &f->context);
    v->val(ctx, &f->applied);
    v->atom(ctx, &f->apply_name);
    v->atom(ctx, &f->name);
}

static void json_destroy_parse_record(JSContext *ctx, void *p)
{
    json_free_parse_record(ctx, (JSONParseRecord *)p);
    js_free(ctx, p);
}

/* A parse frame owns the container it builds and the key whose value is being parsed; its record is borrowed
   from the shared tree. */
static void js_json_frame_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    JSONFrame *f = elem;
    v->val(ctx, &f->container);
    v->atom(ctx, &f->prop_name);
}
/* The tokenizer's current token owns what free_token releases, by the token's kind. */
static void js_json_token_visit(JSContext *ctx, JSToken *t, JSStepVisit *v)
{
    switch (t->val) {
    case TOK_NUMBER: v->val(ctx, &t->u.num.val); break;
    case TOK_STRING: case TOK_TEMPLATE: v->val(ctx, &t->u.str.str); break;
    case TOK_REGEXP: v->val(ctx, &t->u.regexp.body); v->val(ctx, &t->u.regexp.flags); break;
    case TOK_IDENT: case TOK_PRIVATE_NAME: v->atom(ctx, &t->u.ident.atom); break;
    default:
        if (t->val >= TOK_FIRST_KEYWORD && t->val <= TOK_LAST_KEYWORD) v->atom(ctx, &t->u.ident.atom);
        break;
    }
}
/* A cursor into the record tree being copied: the original record it names, and the slot to re-aim. */
typedef struct JSONRecordCursor {
    const JSONParseRecord *from;
    void **slot;
} JSONRecordCursor;
static int json_record_cursor_cmp(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)((const JSONRecordCursor *)a)->from, y = (uintptr_t)((const JSONRecordCursor *)b)->from;
    return x < y ? -1 : x > y;
}
/* JSStepTreeOps.clone for a reviver's record tree the parse is still appending to: a deep copy, each array
   at its record's own capacity so the parse goes on appending in place, and every cursor re-aimed at the
   copy of the record it named. The walk is an explicit stack, since the tree is as deep as the JSON. */
static void *json_parse_record_tree_clone(JSContext *ctx, void *root, void **cursors[], int ncursors)
{
    typedef struct { const JSONParseRecord *src; JSONParseRecord *dst; } Pair;
    JSONRecordCursor *map = NULL;
    Pair *st = NULL;
    int sp = 0, st_size = 0, i, named = 0, hit = 0;
    JSONParseRecord *out = js_malloc(ctx, sizeof(*out));

    CHECK(out != NULL, "JSON.parse: OOM copying a reviver's parse-record tree for a fork");
    if (ncursors > 0) {
        map = js_malloc(ctx, sizeof(*map) * (size_t)ncursors);
        CHECK(map != NULL, "JSON.parse: OOM mapping the cursors into a reviver's parse-record tree");
        for (i = 0; i < ncursors; i++) {
            map[i].from = *cursors[i];
            map[i].slot = cursors[i];
            named += map[i].from != NULL;
        }
        qsort(map, (size_t)ncursors, sizeof(*map), json_record_cursor_cmp);
    }
    CHECK(!js_resize_array(ctx, (void **)&st, sizeof(*st), &st_size, 1),
          "JSON.parse: OOM walking a reviver's parse-record tree for a fork");
    st[sp].src = root; st[sp].dst = out; sp++;
    while (sp > 0) {
        const JSONParseRecord *a = st[--sp].src;
        JSONParseRecord *b = st[sp].dst;
        int lo = 0, hi = ncursors, n;

        *b = *a;
        b->value = js_dup(a->value);
        /* Every cursor naming `a`: the map is sorted, so they are one run found by a lower bound. */
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if ((uintptr_t)map[mid].from < (uintptr_t)a) lo = mid + 1; else hi = mid;
        }
        for (; lo < ncursors && map[lo].from == a; lo++, hit++)
            *map[lo].slot = b;
        if (!JS_IsObject(a->value))
            continue;
        if (js_is_array(ctx, a->value)) {
            n = a->u.array.count;
            b->u.array.elements = NULL;
            if (a->u.array.size > 0) {
                b->u.array.elements = js_malloc(ctx, sizeof(*a->u.array.elements) * (size_t)a->u.array.size);
                CHECK(b->u.array.elements != NULL, "JSON.parse: OOM copying a parse record's elements");
            }
            CHECK(!js_resize_array(ctx, (void **)&st, sizeof(*st), &st_size, sp + n),
                  "JSON.parse: OOM walking a reviver's parse-record tree for a fork");
            for (i = 0; i < n; i++) {
                st[sp].src = &a->u.array.elements[i]; st[sp].dst = &b->u.array.elements[i]; sp++;
            }
        } else {
            const JSONParseRecordObject *po = &a->u.obj;
            JSONParseRecordObject *qo = &b->u.obj;
            n = po->count;
            qo->entries = NULL;
            qo->hash_table = NULL;
            if (po->size > 0) {
                qo->entries = js_malloc(ctx, sizeof(*po->entries) * (size_t)po->size);
                CHECK(qo->entries != NULL, "JSON.parse: OOM copying a parse record's members");
            }
            if (po->hash_size > 0) {
                qo->hash_table = js_malloc(ctx, sizeof(*po->hash_table) * po->hash_size);
                CHECK(qo->hash_table != NULL, "JSON.parse: OOM copying a parse record's member hash");
                memcpy(qo->hash_table, po->hash_table, sizeof(*po->hash_table) * po->hash_size);
            }
            CHECK(!js_resize_array(ctx, (void **)&st, sizeof(*st), &st_size, sp + n),
                  "JSON.parse: OOM walking a reviver's parse-record tree for a fork");
            for (i = 0; i < n; i++) {
                qo->entries[i].atom = JS_DupAtom(ctx, po->entries[i].atom);
                qo->entries[i].hash_next = po->entries[i].hash_next;
                st[sp].src = &po->entries[i].parse_record; st[sp].dst = &qo->entries[i].parse_record; sp++;
            }
        }
    }
    js_free(ctx, st);
    js_free(ctx, map);
    DCHECKF(hit == named, "a fork copied a reviver's parse-record tree and %d of its %d cursors named no record "
            "in it — every frame's record, `pr_cur` and `pr_root` stand inside the tree the parse is building",
            named - hit, named);
    out->ref_count = 1;
    return out;
}
static const JSStepTreeOps json_parse_record_tree_ops = {
    json_parse_record_tree_clone, json_destroy_parse_record
};
/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). The reviver is the page's code called once per node, so a
   concolic branch inside it forks the walk and each arm revives its own subtree from there — its own frame
   stack, its own holders, its own result. The parse record is SHARED by reference for the reason stated at its
   refcount, and `text` is the bytes of `text_val`, of which each arm holds its own reference. */
static void js_json_reviver_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSJsonReviver *s = st;
    /* A reviver's record tree the parse is still appending to is the machine's own (`tree`), not shared: its
       cursors are each open frame's record, `pr_cur` and `pr_root`. A frame's record is read into `recs` before
       the frame stack is visited and written back after, the order in which every consumer finds both the
       tree and the stack it visits live. */
    bool building = s->parsing && s->pr != NULL;
    JSONParseRecord *recs_inline[16], **recs = recs_inline;
    void **cur_inline[18], ***cur = cur_inline;
    int i, nf = building ? s->jp.sp : 0;

    DCHECK(!building || (s->pr->ref_count == 1 && s->sp == 0),
           "a reviver's record tree is shared, or its walk has begun, while the parse is still building it — "
           "the tree is shared only once the parse is done, and the walk starts there");
    if (nf > 16) {
        recs = js_malloc(ctx, sizeof(*recs) * (size_t)nf);
        cur = js_malloc(ctx, sizeof(*cur) * (size_t)(nf + 2));
        CHECK(recs != NULL && cur != NULL, "JSON.parse: OOM naming a deep parse's records for its declaration");
    }
    if (building) {
        for (i = 0; i < nf; i++) { recs[i] = s->jp.st[i].pr; cur[i] = (void **)&recs[i]; }
        cur[nf] = (void **)&s->jp.pr_cur;
        cur[nf + 1] = (void **)&s->jp.pr_root;
        v->tree(ctx, (void **)&s->pr, cur, nf + 2, &json_parse_record_tree_ops);
    }
    /* A parse the flow is parked inside (`parsing`): its frame stack, the value flowing into the frame below,
       and the current token. Every completed value is a rest point, so a fork can land between any two of
       them, and an undeclared frame stack was one allocation two arms freed. */
    if (s->parsing) {
        v->array(ctx, (void **)&s->jp.st, sizeof(JSONFrame), s->jp.sp, s->jp.st_size, js_json_frame_visit);
        v->val(ctx, &s->jp.val);
        js_json_token_visit(ctx, &s->jps.token, v);
    }
    if (building && s->jp.st != NULL)
        for (i = 0; i < nf; i++) s->jp.st[i].pr = recs[i];
    if (recs != recs_inline) { js_free(ctx, recs); js_free(ctx, cur); }
    v->val(ctx, &s->text_val);
    v->array(ctx, (void **)&s->stack, sizeof(JRFrame), s->sp, s->cap, js_jrframe_visit);
    if (!building)
        v->shared(ctx, (void **)&s->pr, s->pr ? &s->pr->ref_count : NULL, json_destroy_parse_record);
    v->val(ctx, &s->root);
    v->val(ctx, &s->result);
    v->val(ctx, &s->text_str);
    v->val(ctx, &s->unknown);
}

/* The JSON.parse recognizer is DELETED — it existed only to pick the suspendable walk over
   internalize_json_property, and that walker is gone. */

/* DELETED: js_json_parse's C body. Its reviver branch called internalize_json_property — a recursive C walker
   that invoked the reviver through JS_Call — which was a second implementation of the walk that
   js_json_reviver_step already performs suspendably. Its no-reviver branch had no callback at all and now lives
   in js_json_parse_vinit. JSON.parse IS the step machine (STEPDEF_JSON_PARSE). */

static JSValue js_json_isRawJSON(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JSValueConst obj = argv[0];
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        JSObject *p = JS_VALUE_GET_OBJ(obj);
        return js_bool(p->class_id == JS_CLASS_RAWJSON);
    } else {
        return JS_FALSE;
    }
}

static bool is_valid_raw_json_char(int c)
{
    return ((c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' ||
            c == '"');
}

/* Steps 2-8 run none of the page's code: the validation is this engine's parser over a String it already holds,
   and the object it builds is not reachable by the page until it is returned. */
/* The standard has no section number for this one: json-parse-with-source is not in the published edition, so
   the algorithm is named and its steps are the proposal's own — 25.5.4 is a section that does not exist, and a
   number invented here would be a claim about the standard rather than a reference to it (see B64OP_STAGES). */
#define JSONRAW_STAGES(X) \
    X(JSONRAW_TOSTRING, "proposal-json-parse-with-source JSON.rawJSON step 1 (jsonString is ToString(text))") \
    X(JSONRAW_BUILD,    "proposal-json-parse-with-source JSON.rawJSON steps 2-8 (jsonString is a valid JSON " \
                        "text; obj carries it, frozen)")
enum { JSONRAW_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_json_raw_steps[] = { JSONRAW_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_json_raw_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSJsonRaw *s = st;
    JSString *p;
    JSValue res, obj;
    int r;

    if (s->hdr.stage == JSONRAW_TOSTRING) {
        if (s->hdr.str_phase == STR_PH_START) { s->str = JS_UNDEFINED; s->result = JS_UNDEFINED; }
        r = step_tostring_run(ctx, &s->hdr, step_arg(&s->hdr, 0), cb_result, &s->str, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r) return r < 0 ? -1 : r;
        s->hdr.stage = JSONRAW_BUILD;
    }
    JS_FreeValue(ctx, cb_result);

    p = JS_VALUE_GET_STRING(s->str);
    if (p->len == 0 ||
        !is_valid_raw_json_char(string_get(p, 0)) ||
        !is_valid_raw_json_char(string_get(p, p->len - 1)))
        goto syntax_error;
    /* rawJSON only validates: no reviver, so this is the plain parse — JS_ParseJSON_internal is the one parser,
       reached directly rather than through a builtin entry that no longer exists.
       Named residual. Not covered: the validation runs to its end inside this step, over the page's text (a
       primitive, so one token, which a long string literal makes long). Next diff: the parse on this machine's
       own JSONParse state, yielding as JP_PARSE does. Its absence shows as a step overrunning the slice at a
       JSON.rawJSON of a string more than a quantum long. */
    {
        const char *cs; size_t cl;
        cs = JS_ToCStringLen(ctx, &cl, s->str);
        res = cs ? JS_ParseJSON_internal(ctx, cs, cl, "<input>", NULL) : JS_EXCEPTION;
        if (cs) JS_FreeCString(ctx, cs);
    }
    if (JS_IsException(res)) {
    syntax_error:
        JS_ThrowSyntaxError(ctx, "invalid rawJSON string");
        return -1;
    }
    JS_FreeValue(ctx, res);

    obj = JS_NewObjectProtoClass(ctx, JS_NULL, JS_CLASS_RAWJSON);
    if (JS_IsException(obj))
        return -1;
    /* the define CONSUMES the string, so the state must stop owning it or fini would free it twice */
    if (JS_DefinePropertyValue(ctx, obj, JS_ATOM_rawJSON, s->str, JS_PROP_ENUMERABLE) < 0) {
        s->str = JS_UNDEFINED;
        JS_FreeValue(ctx, obj);
        return -1;
    }
    s->str = JS_UNDEFINED;
    JS_PreventExtensions(ctx, obj);
    s->result = obj;
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). */
static void js_json_raw_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSJsonRaw *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->str);
}

static JSValue js_json_raw_fini(JSContext *ctx, void *st, bool take_result)
{
    JSJsonRaw *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static JSValue JS_ToQuotedStringFree(JSContext *ctx, JSValue val) {
    JSValue r = JS_ToQuotedString(ctx, val);
    JS_FreeValue(ctx, val);
    return r;
}

JSValue JS_QuoteJSONString(JSContext *ctx, JSValueConst value)
{
    DCHECK(JS_IsString(value), "QuoteJSONString was handed a non-String — §25.5.4.3 takes a String, and "
                               "JS_ToQuotedString's ToString would run the page's code on an object");
    return JS_ToQuotedString(ctx, value);
}

static int sj_push(JSContext *ctx, JSJsonStr *s)
{
    JSSJFrame *f;
    if (s->sp >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        JSSJFrame *ns = js_realloc(ctx, s->frames, (size_t)nc * sizeof(JSSJFrame));
        if (!ns) return -1;
        s->frames = ns; s->cap = nc;
    }
    f = &s->frames[s->sp++];
    memset(f, 0, sizeof(*f));
    f->holder = JS_UNDEFINED; f->key = JS_UNDEFINED; f->val = JS_UNDEFINED;
    f->indent = JS_UNDEFINED; f->indent1 = JS_UNDEFINED;
    f->sep = JS_UNDEFINED; f->sep1 = JS_UNDEFINED;
    f->key_atom = JS_ATOM_NULL;
    f->phase = SJ_GET;
    return 0;
}

/* Release the top frame. The cycle-detection pop rides here rather than at the one normal-completion site
   because the walk is a DFS: frames leave in reverse order of arrival, which is exactly the order a stack
   truncation needs, and a frame torn down MID-WALK owes the same pop as one that finished. */
/* WHAT ONE SERIALIZE LEVEL OWNS. `pushed` is NOT here: popping the cycle-detection stack is the algorithm
   UNWINDING, not a reference being released, and a clone must copy the frame without unwinding anything. */
static void js_sj_frame_visit(JSContext *ctx, void *elem, JSStepVisit *v)
{
    JSSJFrame *f = elem;
    v->array(ctx, (void **)&f->ek, sizeof(JSEnumKeys), 1, 1, js_enum_keys_visit);
    v->array(ctx, (void **)&f->keys, sizeof(JSAtom), (int)f->nkeys, (int)f->nkeys, js_step_visit_atom_elem);
    v->atom(ctx, &f->key_atom);
    v->val(ctx, &f->holder); v->val(ctx, &f->key); v->val(ctx, &f->val);
    v->val(ctx, &f->indent); v->val(ctx, &f->indent1);
    v->val(ctx, &f->sep); v->val(ctx, &f->sep1);
}

static void sj_pop(JSContext *ctx, JSJsonStr *s)
{
    JSSJFrame *f = &s->frames[--s->sp];
    if (f->pushed) js_array_private_pop(ctx, s->stack);   /* the unwind, which the declaration deliberately omits */
    js_sj_frame_visit(ctx, f, (JSStepVisit *)&js_step_visit_free);
}

/* AN UNKNOWN IN A SERIALIZATION POSITION, TAKEN BEFORE ANYTHING BRANCHES ON ITS TYPE.
 *
 * §25.5.4.2 SerializeJSONProperty ( state, key, holder ) decides everything by TYPE — step 2 "If value is an
 * Object or value is a BigInt", step 4 "If value is an Object", step 11 "If value is an Object and
 * IsCallable(value) is false" — and §solver's rule for `typeof` over unknown input is that the engine must not
 * answer from the host object's REPRESENTATION. It answered from it here, and the representation says
 * "function": the value class installs a `.call` handler so a page can write `document.cookie.indexOf(...)`,
 * which makes JS_IsFunction true, which makes step 11 false and step 12 "Return undefined" the answer. So an
 * unknown MEMBER was dropped from the object entirely and an unknown ROOT made JSON.stringify return
 * undefined — the taint deleted, silently, with a plausible concrete string left in its place for
 * `fetch(url, { body: JSON.stringify(cfg) })` to send.
 *
 * CLAUDE.md's §C-stack names this builtin and this branch by hand, and the answer it states is to yield the
 * opaque ITSELF — taint preserved, never a de-tainting placeholder. A JSON text is a String, so yielding the
 * opaque means the RESULT is an unknown String carrying this operand's source — which
 * is what sj_finish builds — and the walk continues over this unknown's own EXAMPLE so that the derived value
 * carries the JSON the concrete really serializes to. That is §solver's only sound way for an example to
 * propagate: the engine RUNS the real operation on the concrete. It runs it ONCE, in this same walk, rather
 * than in a second pass — a second pass would re-run the page's `toJSON`, its replacer and every accessor a
 * second time, and their side effects with them.
 *
 * `pv` is the settled value slot (a frame's, or the prologue's `space`), owned by the caller and REPLACED.
 * It answers nothing back: every caller's next act is the spec step it was about to take, which now reads a
 * real value whatever this did, and a return value no caller reads is a second contract to keep correct. */
static void sj_take_unknown(JSContext *ctx, JSJsonStr *s, JSValue *pv)
{
    JSValue ex = js_concolic_operand(ctx, *pv);   /* UNINITIALIZED when *pv is not unknown input */

    if (JS_IsUninitialized(ex))
        return;
    DCHECK(!(g_concolic.is(ex)),
           "an unknown's EXAMPLE is itself unknown external input — an example rides the concrete value the "
           "interpreter really produced, so a producer has attached a derivation where a datum belongs, and "
           "the walk below would take it as a serializable object");
    if (JS_IsUndefined(s->unknown))
        s->unknown = js_dup(*pv);
    if (JS_IsUndefined(ex))
        s->exampleless = 1;
    JS_FreeValue(ctx, *pv);
    *pv = ex;
}

/* THE RESULT, ONCE THE WALK HAS PRODUCED ITS TEXT. `text` is what the serialization built with every unknown
   standing on its own example, and it is CONSUMED. With no unknown met it IS the answer; with one, the answer
   is the derived unknown String and `text` is its example — or no example at all, when some unknown had no
   concrete to contribute and the text is therefore a string no run of this page produces. */
static JSValue sj_finish(JSContext *ctx, JSJsonStr *s, JSValue text)
{
    JSValue c;

    if (JS_IsUndefined(s->unknown))
        return text;
    if (s->exampleless) {
        JS_FreeValue(ctx, text);
        text = JS_UNDEFINED;
    }
    c = js_concolic_derive(ctx, s->unknown, "JSON.stringify", text);
    DCHECK(!JS_IsUninitialized(c),
           "the concolic derivation declined an operand this machine had already recognised as unknown "
           "external input — the value semantics were installed without the builtin-derivation hook");
    JS_FreeValue(ctx, s->unknown);
    s->unknown = JS_UNDEFINED;
    return c;
}

/* state.[[PropertyList]]'s append, §25.5.4 JSON.stringify ( value [ , replacer [ , space ] ] ) step 5.b.ii.4.7
   — "If item is not undefined and propertyList does not contain item, then" — so the key joins unless an equal
   one is already there. Comparing ATOMS performs that SameValue on strings once, at interning, instead of per
   element. `atom` is CONSUMED either way. */
static int sj_plist_add(JSContext *ctx, JSJsonStr *s, JSAtom atom)
{
    uint32_t k;
    for (k = 0; k < s->nplist; k++) {
        if (s->plist[k] == atom) { JS_FreeAtom(ctx, atom); return 0; }
    }
    if (s->nplist >= s->nplist_cap) {
        uint32_t nc = s->nplist_cap ? s->nplist_cap * 2 : 8;
        JSAtom *na = js_realloc(ctx, s->plist, (size_t)nc * sizeof(JSAtom));
        if (!na) { JS_FreeAtom(ctx, atom); return -1; }
        s->plist = na; s->nplist_cap = nc;
    }
    s->plist[s->nplist++] = atom;
    return 0;
}

/* §25.5.4 JSON.stringify ( value [ , replacer [ , space ] ] ) steps 5-9. `in` is the previous request's answer,
   owned. 0 = the prologue is complete, a step code = the caller must return it, -1 = threw.
   NAMED RESIDUAL — AN UNKNOWN `replacer` IS TAKEN AS THE CALLABLE ARM AND THE ARRAY ARM IS NOT EXPLORED.
   WHAT IS NOT COVERED: step 5's two arms, 5.a "If IsCallable(replacer) is true" and 5.b's IsArray, are a TYPE
   dispatch over a value whose type is unknown; JS_IsFunction answers true off this value class's `.call`
   handler, so the callable arm is DECIDED. WHAT THE NEXT DIFF BUILDS: a fork over step 5's arms, so the
   PropertyList world is a sibling flow of the replacer-function world. WHY THIS IS NARROW AND NOT WRONG: the
   arm taken calls the unknown at every node, each call answers with a derivation of it (SJ_REPLACER_CALLED
   takes that at `unwrap:`), so the result is an unknown String carrying the replacer's own source either way —
   what the unexplored arm would change is only which concrete example the text carries. HOW ITS ABSENCE WOULD
   SHOW: `JSON.stringify(cfg, unknownReplacer)` reports an @H body shape with no example where the array arm
   would have produced one built out of the named members. */
static int js_json_str_prologue(JSContext *ctx, JSJsonStr *s, JSValue in, JSValue **out_cb, int *out_argc)
{
    JSValueConst replacer = step_arg(&s->hdr, 1);
    int r;

    for (;;) {
        switch (s->pro) {
        case SJP_START:
            JS_FreeValue(ctx, in); in = JS_UNDEFINED;
            s->space = js_dup(step_arg(&s->hdr, 2));
            if (JS_IsFunction(ctx, replacer)) { s->pro = SJP_SPACE; continue; }
            r = js_is_array(ctx, replacer);
            if (r < 0) return -1;
            if (!r) { s->pro = SJP_SPACE; continue; }
            /* PropertyList exists from here on even if the array turns out to be empty — an empty one
               serializes every object as {} — so the allocation, not the count, is what records it. */
            s->nplist = 0;
            s->nplist_cap = 4;
            s->plist = js_mallocz(ctx, sizeof(JSAtom) * s->nplist_cap);
            if (!s->plist) return -1;
            s->pro = SJP_RLEN;
            continue;

        case SJP_RLEN:
            /* `? LengthOfArrayLike(replacer)` — a `length` accessor or a Proxy `get` trap. */
            r = step_length_run(ctx, &s->hdr, replacer, in, &s->pn, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r != 0) return r;
            s->pi = 0;
            s->pro = SJP_RGET;
            continue;

        case SJP_RGET: {
            JSValue v;
            if (s->hdr.get_phase == GET_PH_START) {
                JSAtom at;
                JS_FreeValue(ctx, in); in = JS_UNDEFINED;
                if (s->pi >= s->pn) { s->pro = SJP_SPACE; continue; }
                at = JS_NewAtomInt64(ctx, s->pi);
                if (at == JS_ATOM_NULL) return -1;
                return step_getprop_begin(ctx, &s->hdr, replacer, at, out_cb, out_argc);
            }
            /* the READ's own completion, taken from the request rather than sniffed off the value it wrote:
               an abrupt one writes nothing, so a JS_IsException on an unwritten local was reading a slot the
               request had never filled. */
            if (step_getprop_done(ctx, &s->hdr, JS_ATOM_NULL, in, &v) < 0) { in = JS_UNDEFINED; return -1; }
            in = JS_UNDEFINED;
            s->pi++;
            /* §25.5.4 steps 5.b.ii.4.4 to 5.b.ii.4.6: a String or a Number is the item; an Object with
               [[StringData]] or [[NumberData]] is ToString'd, which is the page's code; anything else is
               skipped. */
            if (JS_IsString(v) || JS_IsNumber(v)) {
                JSAtom at;
                v = JS_ToStringFree(ctx, v);   /* a string or a primitive number: nothing to run */
                if (JS_IsException(v)) return -1;
                at = JS_ValueToAtom(ctx, v);
                JS_FreeValue(ctx, v);
                if (at == JS_ATOM_NULL) return -1;
                if (sj_plist_add(ctx, s, at) < 0) return -1;
                continue;
            }
            if (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT) {
                JSObject *p = JS_VALUE_GET_OBJ(v);
                if (p->class_id == JS_CLASS_STRING || p->class_id == JS_CLASS_NUMBER) {
                    s->pv = v;
                    s->pro = SJP_RSTR;
                    continue;
                }
            }
            JS_FreeValue(ctx, v);
            continue;
        }

        case SJP_RSTR: {
            JSValue str;
            JSAtom at;
            r = step_tostring_run(ctx, &s->hdr, s->pv, in, &str, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r != 0) return r;
            JS_FreeValue(ctx, s->pv); s->pv = JS_UNDEFINED;
            at = JS_ValueToAtom(ctx, str);
            JS_FreeValue(ctx, str);
            if (at == JS_ATOM_NULL) return -1;
            if (sj_plist_add(ctx, s, at) < 0) return -1;
            s->pro = SJP_RGET;
            continue;
        }

        case SJP_SPACE:
            /* §25.5.4 step 6: a wrapper `space` is unwrapped, which runs the page's toString/valueOf. Its
               "If space is an Object" and steps 7-8's "If space is a Number" / "is a String" are the same TYPE
               dispatch SerializeJSONProperty makes, so an unknown `space` is taken here for the same reason —
               left to fall through, it matched neither and the gap silently became the empty String, which is
               a DECIDED answer to how much whitespace an unknown asks for. */
            sj_take_unknown(ctx, s, &s->space);
            if (JS_VALUE_GET_TAG(s->space) == JS_TAG_OBJECT) {
                JSObject *p = JS_VALUE_GET_OBJ(s->space);
                if (p->class_id == JS_CLASS_NUMBER) { s->pro = SJP_SPACE_NUM; continue; }
                if (p->class_id == JS_CLASS_STRING) {
                    JSValue str;
                    r = step_tostring_run(ctx, &s->hdr, s->space, in, &str, out_cb, out_argc);
                    in = JS_UNDEFINED;
                    if (r != 0) return r;
                    JS_FreeValue(ctx, s->space);
                    s->space = str;
                }
            }
            JS_FreeValue(ctx, in); in = JS_UNDEFINED;
            s->pro = SJP_GAP;
            continue;

        case SJP_SPACE_NUM: {
            JSValue prim;
            r = step_toprim_run(ctx, &s->hdr, s->space, HINT_NUMBER, in, &prim, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r != 0) return r;
            JS_FreeValue(ctx, s->space);
            /* THE CONVERSION BOUNDARY BELOW OWES C A REAL NUMBER, so an unknown reaching it aborts the
               document — see JS_ToNumberInternal's object arm. It cannot: §7.1.1 ToPrimitive is a step machine
               whose delivery gate admits a coercion method's result only where that result is not an Object,
               and this value class IS an Object,
               so a `valueOf` answering with unknown input is rejected as non-primitive and the walk ends in
               §7.1.1.1 OrdinaryToPrimitive's TypeError instead. Asserted rather than assumed, because the day
               that gate learns about this class is the day this line starts receiving one. */
            DCHECK(!(g_concolic.is(prim)),
                   "§25.5.4 step 6.a's ToNumber was handed unknown external input by §7.1.1 ToPrimitive — the "
                   "conversion boundary owes C a real number, so this needs a derived answer here and not a "
                   "conversion");
            s->space = JS_ToNumberFree(ctx, prim);
            if (JS_IsException(s->space)) { s->space = JS_UNDEFINED; return -1; }
            s->pro = SJP_GAP;
            continue;
        }

        default:
            DCHECK(s->pro == SJP_GAP, "the JSON.stringify prologue resumed in no phase");
            JS_FreeValue(ctx, in);
            /* §25.5.4 steps 7-9. `space` is a primitive by now, so nothing here runs. */
            if (JS_IsNumber(s->space)) {
                int n;
                if (JS_ToInt32Clamp(ctx, &n, s->space, 0, 10, 0)) return -1;
                s->gap = JS_NewStringLen(ctx, "          ", n);
            } else if (JS_IsString(s->space)) {
                JSString *p;
                /* JS_IsString IS TRUE FOR A ROPE, and JS_VALUE_GET_STRING would hand back the JSStringRope as a
                   JSString — `p->len` would then be a rope field read as a length and js_sub_string would cut
                   ten units out of a header. `JSON.stringify(v, null, "aaaa…" + "bbbb…")` is all it takes, and
                   the two structs are near enough that nothing downstream complains. JS_ToStringFree flattens
                   and is the identity on an already-flat string, the same way Symbol.for takes its key. */
                if (JS_VALUE_GET_TAG(s->space) == JS_TAG_STRING_ROPE) {
                    s->space = JS_ToStringFree(ctx, s->space);
                    if (JS_IsException(s->space)) { s->space = JS_UNDEFINED; return -1; }
                }
                DCHECK(JS_VALUE_GET_TAG(s->space) == JS_TAG_STRING,
                       "JSON.stringify's gap is about to read a JSString out of a value that is not one");
                p = JS_VALUE_GET_STRING(s->space);
                s->gap = js_sub_string(ctx, p, 0, min_int(p->len, 10));
            } else {
                s->gap = js_dup(s->empty);
            }
            return JS_IsException(s->gap) ? -1 : 0;
        }
    }
}

/* A container node's separator strings and its place on the cycle stack — §25.5.4.6 SerializeJSONArray
   ( state, value ) / §25.5.4.5 SerializeJSONObject ( state, value ) steps 2 and 4. Nothing here runs the page's
   code, so it is one block rather than a phase. */
static int sj_open_container(JSContext *ctx, JSJsonStr *s, JSSJFrame *f)
{
    f->indent1 = JS_ConcatString(ctx, js_dup(f->indent), js_dup(s->gap));
    if (JS_IsException(f->indent1)) { f->indent1 = JS_UNDEFINED; return -1; }
    if (!JS_IsEmptyString(s->gap)) {
        f->sep = JS_ConcatString3(ctx, "\n", js_dup(f->indent1), "");
        if (JS_IsException(f->sep)) { f->sep = JS_UNDEFINED; return -1; }
        f->sep1 = js_new_string8(ctx, " ");
        if (JS_IsException(f->sep1)) { f->sep1 = JS_UNDEFINED; return -1; }
    } else {
        f->sep = js_dup(s->empty);
        f->sep1 = js_dup(s->empty);
    }
    if (js_array_private_push(ctx, s->stack, f->val) < 0) return -1;
    f->pushed = 1;
    return 0;
}

/* §25.5.4.2 SerializeJSONProperty steps 5-10 on a value that is not a serializable object: write it. `val` is
   CONSUMED. */
static int sj_write_primitive(JSContext *ctx, JSJsonStr *s, JSValue val)
{
    switch (JS_VALUE_GET_NORM_TAG(val)) {
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        val = JS_ToQuotedStringFree(ctx, val);
        if (JS_IsException(val)) return -1;
        break;
    case JS_TAG_FLOAT64:
        if (!isfinite(JS_VALUE_GET_FLOAT64(val))) val = JS_NULL;
        break;
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        JS_FreeValue(ctx, val);
        JS_ThrowTypeError(ctx, "BigInt are forbidden in JSON.stringify");
        return -1;
    default:
        DCHECK(JS_VALUE_GET_NORM_TAG(val) == JS_TAG_INT || JS_VALUE_GET_NORM_TAG(val) == JS_TAG_BOOL
               || JS_VALUE_GET_NORM_TAG(val) == JS_TAG_NULL,
               "a value SerializeJSONProperty said writes nothing reached the primitive writer");
        break;
    }
    return string_buffer_concat_value_free(&s->b, val) < 0 ? -1 : 0;
}

/* Does SerializeJSONProperty write anything for this value? §25.5.4.2's steps 5-11 answer it, and step 12's
   "Return undefined." is the whole of the no. A callable object and a Symbol are the two kinds that reach it. */
static int sj_writes(JSContext *ctx, JSValueConst val)
{
    switch (JS_VALUE_GET_NORM_TAG(val)) {
    case JS_TAG_OBJECT:
        /* AND UNKNOWN EXTERNAL INPUT IS NOT ONE OF THEM, which is what sj_take_unknown guarantees two phases
           earlier. Step 11's IsCallable is answered from the OBJECT, and this value class carries a `.call`
           handler so a page can write `x.indexOf(...)` over an unknown — so an unknown arriving here answers
           "callable", takes step 12, and is dropped from the output with its taint. That is the bug this
           assert makes impossible to reintroduce: a new route into the walk that forgets the take would be
           silent, and silence here is a deleted attacker source. */
        DCHECK(!(g_concolic.is(val)),
               "unknown external input reached §25.5.4.2 SerializeJSONProperty's type dispatch — step 11's "
               "IsCallable answers TRUE for this value class's carrier, so the member would be dropped and its "
               "provenance with it; the value is taken at sj_take_unknown before any step branches on it");
        return !JS_IsFunction(ctx, val);
    case JS_TAG_STRING: case JS_TAG_STRING_ROPE: case JS_TAG_INT: case JS_TAG_FLOAT64:
    case JS_TAG_BOOL: case JS_TAG_NULL: case JS_TAG_SHORT_BIG_INT: case JS_TAG_BIG_INT:
        return 1;
    default:
        return 0;
    }
}

/* Drive the walk until the next request (return its step code) or completion (0). `in` (owned) is the previous
   request's answer, JS_UNDEFINED on entry. */
static int js_json_str_walk(JSContext *ctx, JSJsonStr *s, JSValue in, JSValue **out_cb, int *out_argc)
{
    int r;

    while (s->sp > 0) {
        JSSJFrame *f = &s->frames[s->sp - 1];
        /* DFS invariants: a live frame within its allocation, a valid phase, and a child cursor that never runs
           past the child count. A violation is a walk-state bug; crash where it is born. */
        DCHECK(s->sp <= s->cap, "the JSON.stringify frame stack ran past its allocation");
        DCHECK(f->phase <= SJ_LOOP, "a JSON.stringify frame resumed in no phase");

        switch (f->phase) {
        case SJ_GET:
            /* §25.5.4.2 SerializeJSONProperty step 1: "Let value be ? Get(holder, key)." The root's holder is
               the engine-built wrapper, but every deeper one is the page's object — an accessor, or a Proxy
               whose `get` trap the C body ran with no flow base. */
            f->phase = SJ_GOT;
            return step_getprop_begin(ctx, &s->hdr, f->holder, JS_DupAtom(ctx, f->key_atom), out_cb, out_argc);

        case SJ_GOT:
            if (step_getprop_done(ctx, &s->hdr, f->key_atom, in, &f->val) < 0) { in = JS_UNDEFINED; return -1; }
            in = JS_UNDEFINED;
            /* BEFORE STEP 2, because step 2 is the first thing that branches on the value's TYPE — and reading
               `toJSON` off an unknown is not a harmless miss: this value class answers every read with a
               derivation, so the read minted a `{src}.toJSON` injection identity the page never wrote and the
               call over it recorded a `toJSON(key)` string predicate the page never performed. */
            sj_take_unknown(ctx, s, &f->val);
            /* step 2: "If value is an Object or value is a BigInt, then" 2.a "Let toJSON be
               ? GetV(value, "toJSON")." */
            if (JS_IsObject(f->val) || JS_IsBigInt(f->val)) {
                f->phase = SJ_TOJSON_GOT;
                return step_getprop_begin(ctx, &s->hdr, f->val, JS_DupAtom(ctx, JS_ATOM_toJSON),
                                          out_cb, out_argc);
            }
            f->phase = SJ_REPLACER_CALLED;
            goto replacer;

        case SJ_TOJSON_GOT: {
            JSValue fn;
            if (step_getprop_done(ctx, &s->hdr, JS_ATOM_toJSON, in, &fn) < 0) { in = JS_UNDEFINED; return -1; }
            in = JS_UNDEFINED;
            if (!JS_IsFunction(ctx, fn)) {
                JS_FreeValue(ctx, fn);
                f->phase = SJ_REPLACER_CALLED;
                goto replacer;
            }
            /* step 2.b.i: "Set value to ? Call(toJSON, value, « key »)." The machine holds the callee across
               the call; the receiver and the key belong to the frame, which outlives it. */
            s->pv = fn;
            s->cb_args[0] = f->val;   /* borrowed: the frame holds the receiver */
            s->cb_args[1] = s->pv;
            s->cb_args[2] = f->key;
            f->phase = SJ_TOJSON_CALLED;
            *out_cb = s->cb_args; *out_argc = 1;
            return 3;
        }

        case SJ_TOJSON_CALLED:
            JS_FreeValue(ctx, s->pv); s->pv = JS_UNDEFINED;
            JS_FreeValue(ctx, f->val);
            f->val = in;
            in = JS_UNDEFINED;
            f->phase = SJ_REPLACER_CALLED;
            goto replacer;

        replacer:
            /* step 3: "If state.[[ReplacerFunction]] is not undefined, then" 3.a "Set value to
               ? Call(state.[[ReplacerFunction]], holder, « key, value »)." Arriving here with the phase already
               set is what makes the no-replacer case a fallthrough rather than a second copy of step 4. */
            if (JS_IsFunction(ctx, step_arg(&s->hdr, 1))) {
                s->cb_args[0] = f->holder;                       /* borrowed: the frame holds it */
                s->cb_args[1] = (JSValue)step_arg(&s->hdr, 1);   /* borrowed: the header holds it */
                s->cb_args[2] = f->key;
                s->cb_args[3] = f->val;
                *out_cb = s->cb_args; *out_argc = 2;
                return 3;
            }
            goto unwrap;

        case SJ_REPLACER_CALLED:
            JS_FreeValue(ctx, f->val);
            f->val = in;
            in = JS_UNDEFINED;
            goto unwrap;

        unwrap:
            /* THE OTHER TWO WAYS AN UNKNOWN ARRIVES IN A VALUE POSITION: the page's own `toJSON` ANSWERED with
               one (step 2.b.i), or its replacer did (step 3.a). Both callbacks RAN — their emits and side
               effects have already happened, which is what §C-stack requires — and only the type dispatch
               below is short-circuited. Taken here rather than at each of the two arms because both converge
               on this label, and step 4 is the first thing past them that reads the value's class. */
            sj_take_unknown(ctx, s, &f->val);
            /* step 4: "If value is an Object, then" — unwrap a RawJSON/Number/String/Boolean/BigInt wrapper
               (4.a to 4.e). The [[NumberData]] and [[StringData]] arms run the page's code. */
            if (JS_VALUE_GET_TAG(f->val) == JS_TAG_OBJECT) {
                JSObject *p = JS_VALUE_GET_OBJ(f->val);
                if (p->class_id == JS_CLASS_STRING) { f->phase = SJ_UNWRAP_STR; continue; }
                if (p->class_id == JS_CLASS_NUMBER) { f->phase = SJ_UNWRAP_NUM; continue; }
                if (p->class_id == JS_CLASS_BOOLEAN || p->class_id == JS_CLASS_BIG_INT) {
                    set_value(ctx, &f->val, js_dup(p->u.object_data));
                } else if (p->class_id == JS_CLASS_RAWJSON) {
                    /* step 4.a.i: "Let rawJSON be ! Get(value, "rawJSON")." JSON.rawJSON's object is
                       null-prototyped, of a class only that builtin creates, and its one property is a
                       non-configurable non-writable data property — so this read reaches no accessor and no
                       trap, which is what the `!` in that step asserts. Asserted rather than assumed. */
                    JSValue raw;
                    DCHECK(!js_read_is_page_code(ctx, f->val, JS_ATOM_rawJSON),
                           "a rawJSON object's own property became reachable page code — route the read");
                    raw = JS_GetProperty(ctx, f->val, JS_ATOM_rawJSON);
                    if (JS_IsException(raw)) return -1;
                    JS_FreeValue(ctx, f->val);
                    f->val = raw;
                    f->raw = 1;
                }
            }
            f->phase = SJ_PREAMBLE;
            continue;

        case SJ_UNWRAP_STR: {
            JSValue str;
            r = step_tostring_run(ctx, &s->hdr, f->val, in, &str, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r != 0) return r;
            JS_FreeValue(ctx, f->val);
            f->val = str;
            f->phase = SJ_PREAMBLE;
            continue;
        }

        case SJ_UNWRAP_NUM: {
            JSValue prim;
            r = step_toprim_run(ctx, &s->hdr, f->val, HINT_NUMBER, in, &prim, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r != 0) return r;
            JS_FreeValue(ctx, f->val);
            /* THE SAME BOUNDARY THE PROLOGUE'S ToNumber ASSERTS AT, for the same reason and by the same
               argument: §7.1.1 ToPrimitive's step machine delivers only a result that is not an Object, and
               this value class IS an Object, so a `valueOf` on a [[NumberData]] wrapper that answers with
               unknown input is rejected as non-primitive and §7.1.1.1 OrdinaryToPrimitive throws instead of
               handing one here. A conversion here would abort the document. */
            DCHECK(!(g_concolic.is(prim)),
                   "§25.5.4.2 step 4.b's ToNumber was handed unknown external input by §7.1.1 ToPrimitive — the "
                   "conversion boundary owes C a real number, so this needs a derived answer here and not a "
                   "conversion");
            f->val = JS_ToNumberFree(ctx, prim);
            if (JS_IsException(f->val)) { f->val = JS_UNDEFINED; return -1; }
            f->phase = SJ_PREAMBLE;
            continue;
        }

        case SJ_PREAMBLE: {
            /* The value is settled, so whether this node writes anything is finally known — and THAT decides
               the separator, the quoted key and, for an array element, the literal `null` that stands in for an
               unserializable one (§25.5.4.6 SerializeJSONArray step 8.b). A parent cannot make that decision,
               which is why the preamble belongs to the child. */
            int writes = sj_writes(ctx, f->val);
            JSSJFrame *par = (s->sp >= 2) ? &s->frames[s->sp - 2] : NULL;
            if (!par) {
                /* the root: an unserializable value is JSON.stringify's undefined result — and an unknown
                   root's result is the derived unknown, whose example is that same undefined (no String, so
                   no example), never a concrete `undefined` that would state the page produces one. */
                if (!writes) { sj_pop(ctx, s); s->result = sj_finish(ctx, s, JS_UNDEFINED); return 0; }
            } else if (par->is_array) {
                if (par->i > 0) string_buffer_putc8(&s->b, ',');
                string_buffer_concat_value(&s->b, par->sep);
                if (!writes) {
                    /* §25.5.4.6 SerializeJSONArray step 8.b.i: "Append "null" to partial." */
                    if (string_buffer_concat_value_free(&s->b, JS_NULL) < 0) return -1;
                    goto child_done;
                }
            } else {
                JSValue q;
                if (!writes) goto child_done;
                if (par->has_content) string_buffer_putc8(&s->b, ',');
                string_buffer_concat_value(&s->b, par->sep);
                q = JS_ToQuotedString(ctx, f->key);
                if (JS_IsException(q)) return -1;
                string_buffer_concat_value_free(&s->b, q);
                string_buffer_putc8(&s->b, ':');
                string_buffer_concat_value(&s->b, par->sep1);
                par->has_content = 1;
            }
            if (JS_VALUE_GET_TAG(f->val) != JS_TAG_OBJECT) {
                JSValue v = f->val;
                f->val = JS_UNDEFINED;
                /* a rawJSON payload IS the serialization: its text is spliced in unquoted, which is the whole
                   point of the proposal and the one value that skips §25.5.4.2 step 8's
                   §25.5.4.3 QuoteJSONString ( value ). */
                if (f->raw) {
                    if (string_buffer_concat_value_free(&s->b, v) < 0) return -1;
                } else if (sj_write_primitive(ctx, s, v) < 0) {
                    return -1;
                }
                goto child_done;
            }
            /* step 11: "If value is an Object and IsCallable(value) is false, then" — a serializable object.
               §25.5.4.6 SerializeJSONArray / §25.5.4.5 SerializeJSONObject open here. */
            if (js_internal_array_includes(ctx, s->stack, f->val)) {
                JS_ThrowTypeError(ctx, "circular reference");
                return -1;
            }
            r = js_is_array(ctx, f->val);
            if (r < 0) return -1;
            f->is_array = r;
            if (sj_open_container(ctx, s, f) < 0) return -1;
            f->i = 0;
            if (f->is_array) { f->phase = SJ_ARR_LEN; continue; }
            string_buffer_putc8(&s->b, '{');
            if (s->plist) {
                /* §25.5.4.5 SerializeJSONObject step 5: "If state.[[PropertyList]] is not undefined, then"
                   5.a "Let keys be state.[[PropertyList]]." — it replaces the enumeration entirely. */
                f->plist = 1;
                f->nkeys = s->nplist;
                f->phase = SJ_LOOP;
                continue;
            }
            /* §25.5.4.5 step 6.a: "Let keys be ? EnumerableOwnProperties(value, key)." — the `ownKeys`
               trap and one `getOwnPropertyDescriptor` per key, both of which the C body ran with no flow
               base. This site cited `step 3`, which is "Let stepback be state.[[Indent]]" — a number IN
               RANGE for a 13-step algorithm, so the step channel had nothing to fail on and only the
               sibling at SJ_KEYS, which already cited 6.a, disagreed with it. */
            f->ek = js_mallocz(ctx, sizeof(*f->ek));
            if (!f->ek) return -1;
            js_enum_keys_init(f->ek);
            f->ek->obj = js_dup(f->val);
            f->phase = SJ_KEYS;
            continue;
        }

        case SJ_ARR_LEN:
            r = step_length_run(ctx, &s->hdr, f->val, in, &f->len, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r != 0) return r;
            string_buffer_putc8(&s->b, '[');
            f->phase = SJ_LOOP;
            continue;

        case SJ_KEYS: {
            uint32_t k;
            r = js_enum_keys_run(ctx, &s->hdr, f->ek, in, out_cb, out_argc);
            in = JS_UNDEFINED;
            if (r != 0) return r;
            /* §25.5.4.5 step 6.a is "Let keys be ? EnumerableOwnProperties(value, key)." — the ENUMERABLE
               half is this filter and not the key walk that produced the list. */
            js_enum_keys_keep_enumerable(ctx, f->ek);
            f->keys = js_malloc(ctx, sizeof(JSAtom) * (f->ek->kept ? f->ek->kept : 1));
            if (!f->keys) return -1;
            /* the survivors MOVE to the frame; the cursor keeps its array only to free what it dropped. */
            for (k = 0; k < f->ek->kept; k++) {
                f->keys[k] = f->ek->atoms[k].atom;
                f->ek->atoms[k].atom = JS_ATOM_NULL;
            }
            f->nkeys = f->ek->kept;
            js_enum_keys_free(ctx, f->ek);
            js_free(ctx, f->ek); f->ek = NULL;
            f->phase = SJ_LOOP;
            continue;
        }

        case SJ_LOOP: {
            JSAtom at;
            int64_t nchild = f->is_array ? f->len : (int64_t)f->nkeys;
            if (f->i >= nchild) {
                /* the closing bracket — §25.5.4.6 SerializeJSONArray steps 9-10 / §25.5.4.5 SerializeJSONObject
                   steps 9-10, whose 10.b builds the pretty-printed form out of state.[[Indent]] */
                bool any = f->is_array ? (f->len > 0) : (f->has_content != 0);
                if (any && !JS_IsEmptyString(s->gap)) {
                    string_buffer_putc8(&s->b, '\n');
                    string_buffer_concat_value(&s->b, f->indent);
                }
                string_buffer_putc8(&s->b, f->is_array ? ']' : '}');
                goto child_done;
            }
            if (f->is_array) {
                at = JS_NewAtomInt64(ctx, f->i);
                if (at == JS_ATOM_NULL) return -1;
            } else {
                at = JS_DupAtom(ctx, f->plist ? s->plist[f->i] : f->keys[f->i]);
            }
            {
                JSValueConst pval = f->val;        /* snapshot: the push may realloc the frame array */
                JSValueConst pind = f->indent1;
                JSValue kv = JS_AtomToValue(ctx, at);
                JSSJFrame *child;
                if (JS_IsException(kv)) { JS_FreeAtom(ctx, at); return -1; }
                if (sj_push(ctx, s) < 0) { JS_FreeAtom(ctx, at); JS_FreeValue(ctx, kv); return -1; }
                child = &s->frames[s->sp - 1];
                child->holder = js_dup(pval);
                child->key = kv;
                child->key_atom = at;
                child->indent = js_dup(pind);
            }
            continue;
        }

        child_done:
            /* this node is fully written. The root's completion is the whole result. */
            sj_pop(ctx, s);
            if (s->sp == 0) {
                s->result = string_buffer_end(&s->b);
                s->b_live = 0;
                if (JS_IsException(s->result)) return -1;
                /* §25.5.4 step 13's result. The text is what the walk built with every unknown standing on its
                   own example, so where one was met the answer is the DERIVED unknown String and this text is
                   its example — the JSON the concrete really serializes to, produced by running the real
                   algorithm rather than by predicting it. */
                s->result = sj_finish(ctx, s, s->result);
                return 0;
            }
            s->frames[s->sp - 1].i++;
            continue;
        }
    }
    DFAIL("the JSON.stringify walk ran out of frames without producing a result");
    return -1;
}

/* THE STAGES OF §25.5.4 JSON.stringify ( value [ , replacer [ , space ] ] ). Two of them carry a sub-cursor
   rather than splitting further, and each is a sub-sequence of ONE step in the sense JSStepHdr's len_phase is:
   `pro` walks the replacer array's element reads inside step 5.b.ii.4, and the FRAME STACK walks §25.5.4.2
   SerializeJSONProperty's recursion inside step 13 — a recursive walk's position is a stack and not a number,
   so the stage says which step the machine is inside and the frames say where in it. Both are visited, so both
   park and fork with the machine.
   A LABEL HERE IS HALF THE IDENTITY OF A DERIVED UNKNOWN (js_step_unknown_result composes
   "<algorithm> @ <label>"), so these strings are not decoration: two stages sharing one would let a branch over
   one coercion decide a branch over another. */
#define JSONSTR_STAGES(X) \
    X(JSONSTR_STATE,    "25.5.4 steps 1-4 and 10-12 (stack, indent, PropertyList and ReplacerFunction begin " \
                        "empty; the `{ \"\": value }` wrapper and the JSON Serialization Record)") \
    X(JSONSTR_PROLOGUE, "25.5.4 steps 5-9 (ReplacerFunction or PropertyList from replacer; gap from space)") \
    X(JSONSTR_WALK,     "25.5.4 step 13 (SerializeJSONProperty(state, the empty String, wrapper))")
enum { JSONSTR_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const js_json_str_steps[] = { JSONSTR_STAGES(JS_STEP_STAGE_LABEL) NULL };

static int js_json_str_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSJsonStr *s = st;
    int r;

    /* steps 10-12's wrapper is built HERE, before steps 5-9: OrdinaryObjectCreate and CreateDataPropertyOrThrow
       on an object no page code can reach run nothing and are observable nowhere, and building it with the rest
       of the state is what lets the walk have ONE entry rather than an entry and a root special case. */
    if (s->hdr.stage == JSONSTR_STATE) {
        JSValue wrapper;
        JSSJFrame *root;
        s->hdr.stage = JSONSTR_PROLOGUE;
        JS_FreeValue(ctx, cb_result);
        cb_result = JS_UNDEFINED;
        s->stack = JS_UNDEFINED; s->gap = JS_UNDEFINED; s->result = JS_UNDEFINED;
        s->pv = JS_UNDEFINED; s->space = JS_UNDEFINED;
        /* js_mallocz ZEROES the state and a zeroed JSValue is tag 0 (int 0), not UNDEFINED — so the unknown
           this serialization stands on is placed explicitly like every other owned slot above. */
        s->unknown = JS_UNDEFINED;
        s->empty = js_empty_string(ctx->rt);
        string_buffer_init(ctx, &s->b, 0);
        s->b_live = 1;
        s->stack = JS_NewArray(ctx);
        if (JS_IsException(s->stack)) { s->stack = JS_UNDEFINED; return -1; }
        /* §25.5.4 steps 10-11: "Let wrapper be OrdinaryObjectCreate(%Object.prototype%)." then "Perform
           ! CreateDataPropertyOrThrow(wrapper, the empty String, value)." — the root holder is `{ "": value }`,
           engine-built. Its Get is a request like every other one, so the walk has one entry rather than an
           entry and a special case for the root. */
        wrapper = JS_NewObject(ctx);
        if (JS_IsException(wrapper)) return -1;
        if (JS_DefinePropertyValue(ctx, wrapper, JS_ATOM_empty_string,
                                   js_dup(step_arg(&s->hdr, 0)), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, wrapper);
            return -1;
        }
        if (sj_push(ctx, s) < 0) { JS_FreeValue(ctx, wrapper); return -1; }
        root = &s->frames[0];
        root->holder = wrapper;
        root->key = js_dup(s->empty);
        root->key_atom = JS_DupAtom(ctx, JS_ATOM_empty_string);
        root->indent = js_dup(s->empty);
    }
    if (s->hdr.stage == JSONSTR_PROLOGUE) {
        r = js_json_str_prologue(ctx, s, cb_result, out_cb, out_argc);
        cb_result = JS_UNDEFINED;
        if (r != 0) return r;
        s->hdr.stage = JSONSTR_WALK;
    }
    DCHECK(s->hdr.stage == JSONSTR_WALK, "JSON.stringify resumed in no stage");
    return js_json_str_walk(ctx, s, cb_result, out_cb, out_argc);
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). `toJSON` and an array replacer are the page's code called at
   every node, so a fork lands with the level stack live and each arm serializes its own remainder into its own
   accumulator. The cycle-detection array is SHARED by reference on purpose: its push and pop go through the
   ordinary property path, so the per-flow COW delta isolates it the way it isolates any other shared array.
   cb_args are borrowed views of the current frame's holder, key and value. */
static void js_json_str_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSJsonStr *s = st;
    v->array(ctx, (void **)&s->frames, sizeof(JSSJFrame), s->sp, s->cap, js_sj_frame_visit);
    v->array(ctx, (void **)&s->plist, sizeof(JSAtom), (int)s->nplist, (int)s->nplist_cap, js_step_visit_atom_elem);
    if (s->b_live) v->strbuf(ctx, &s->b);
    v->val(ctx, &s->stack);
    v->val(ctx, &s->gap);
    v->val(ctx, &s->empty);
    v->val(ctx, &s->pv);
    v->val(ctx, &s->space);
    v->val(ctx, &s->unknown);
    v->val(ctx, &s->result);
}

static JSValue js_json_str_fini(JSContext *ctx, void *st, bool take_result)
{
    JSJsonStr *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    /* the UNWIND first, while the cycle stack is still alive: sj_pop pops it for a frame that pushed. Only
       then does the declaration release everything, the frame array included. */
    while (s->sp > 0) sj_pop(ctx, s);
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

/* DELETED: JS_JSONStringify, js_json_check, js_json_to_str and JSONStringifyContext. The public C entry was a
   SECOND implementation of this algorithm — the one that drove toJSON, the replacer, every read and the proxy
   traps from C — and keeping it beside the machine is exactly the dual-system seam these conversions exist to
   close. quickjs.h loses the declaration with it. */

static const JSCFunctionListEntry js_json_funcs[] = {
    JS_CFUNC_DEF("isRawJSON", 1, js_json_isRawJSON ),
    JS_CFUNC_STEP_DEF("parse", 2, STEPDEF_JSON_PARSE ),
    JS_CFUNC_STEP_DEF("rawJSON", 1, STEPDEF_JSON_RAWJSON ),
    JS_CFUNC_STEP_DEF("stringify", 3, STEPDEF_JSON_STRINGIFY ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "JSON", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_json_obj[] = {
    JS_OBJECT_DEF("JSON", js_json_funcs, countof(js_json_funcs), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE ),
};

static const JSClassShortDef js_json_class_def[] = {
    { JS_ATOM_Object, NULL, NULL }, /* JS_CLASS_RAWJSON */
};

int JS_AddIntrinsicJSON(JSContext *ctx)
{
    if (!JS_IsRegisteredClass(ctx->rt, JS_CLASS_RAWJSON)) {
        if (init_class_range(ctx->rt, js_json_class_def, JS_CLASS_RAWJSON,
                             countof(js_json_class_def)) < 0)
            return -1;
    }
    /* add JSON as autoinit object */
    return JS_SetPropertyFunctionList(ctx, ctx->global_obj, js_json_obj, countof(js_json_obj));
}

