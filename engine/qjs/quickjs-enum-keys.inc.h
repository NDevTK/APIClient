/* EnumerableOwnProperties' key half (§7.3.23 steps 1-3.a), as a resumable cursor. It is [[OwnPropertyKeys]]
   followed by a [[GetOwnProperty]] per key for the enumerability test — on a Proxy the `ownKeys` trap and then
   the `getOwnPropertyDescriptor` trap per key, so BOTH are the page's code and both have to be requests.
   Six C callers still ask for this by passing JS_GPN_ENUM_ONLY to a names walk, which runs those traps from C;
   each converts by driving this instead. The keys are SNAPSHOT first and each one's enumerability re-checked
   afterwards, because a trap can redefine a later key in between — the same order JSPropWalk uses, which is why
   this is one cursor rather than a second copy of that logic. */
typedef struct JSEnumKeys {
    JSValue obj;               /* the object being walked (owned) */
    JSPropertyEnum *atoms;     /* the snapshot, compacted in place down to the keys that still EXIST, each
                                  carrying the `enumerable` its descriptor reported */
    uint32_t len;              /* how many the snapshot holds */
    uint32_t kept;             /* how many have survived so far */
    uint32_t i;                /* the walk cursor */
    uint8_t phase;             /* EK_* */
    /* 7.3.23's `kind` operand. key+value reads each surviving key's VALUE right after its descriptor, so the
       page sees getOwnPropertyDescriptor(a), get(a), getOwnPropertyDescriptor(b), get(b) — interleaved, which
       is what the algorithm states. Collecting every descriptor and then every value is a DIFFERENT observable
       order, and it is the order import()'s attribute walk had. */
    uint8_t want_values;
    JSValue *vals;             /* want_values: one per survivor, indexed with atoms[] (owned) */
    JSValue cb[1];             /* the request buffer: [the object] */
} JSEnumKeys;
enum { EK_ASK_KEYS = 0, EK_GOT_KEYS, EK_ASK_DESC, EK_GOT_DESC, EK_ASK_VAL, EK_GOT_VAL, EK_DONE };

static void js_enum_keys_init(JSEnumKeys *c)
{
    c->obj = JS_UNDEFINED; c->cb[0] = JS_UNDEFINED;
    c->atoms = NULL; c->len = c->kept = c->i = 0; c->phase = EK_ASK_KEYS;
    c->want_values = 0; c->vals = NULL;
}

static void js_enum_keys_free(JSContext *ctx, JSEnumKeys *c)
{
    if (c->vals) {
        uint32_t vi;
        for (vi = 0; vi < c->kept; vi++) JS_FreeValue(ctx, c->vals[vi]);
        js_free(ctx, c->vals); c->vals = NULL;
    }
    if (c->atoms) { js_free_prop_enum(ctx, c->atoms, c->len); c->atoms = NULL; }
    JS_FreeValue(ctx, c->obj); c->obj = JS_UNDEFINED;
    c->cb[0] = JS_UNDEFINED;   /* borrowed by the requests; `obj` owns it */
    c->len = c->kept = c->i = 0;
}

/* The cursor performs the SEQUENCE — [[OwnPropertyKeys]] then one [[GetOwnProperty]] per string key — and
   reports the FACTS: which keys still exist and whether each is enumerable. It does not apply anyone's rule.
   EnumerableOwnProperties wants the enumerable ones (js_enum_keys_keep_enumerable); 14.7.5.10.2.1's for-in wants
   ALL of them, because a non-enumerable key SHADOWS an enumerable one deeper in the prototype chain and
   dropping it would enumerate a hidden property. Filtering here would have made for-in a second walk.
   `in` is the previous request's answer (UNDEFINED on entry). Returns 11 / 12 for the next request, 0 when the
   snapshot in c->atoms[0..c->kept) is the answer, -1 having thrown.
   `h` IS THE OWNING MACHINE'S HEADER AND IT IS NOT DECORATION: this cursor's [[OwnPropertyKeys]] goes through
   step_ownkeys_run like every other one, so that there is exactly ONE issuer of that request in the engine and
   a consumer added later cannot reach the internal method without whatever that wrapper performs first. The
   cursor has no header of its own — it is embedded in five different machines — so the one it borrows is the
   one whose stage the request is issued and answered at. */
static int js_enum_keys_run(JSContext *ctx, JSStepHdr *h, JSEnumKeys *c, JSValue in,
                            JSValue **out_cb, int *out_argc)
{
    for (;;) {
        if (c->phase == EK_ASK_KEYS || c->phase == EK_GOT_KEYS) {
            /* the key list arrives as an ARRAY. Turning it into an atom snapshot invokes nothing: it is a dense
               array this engine built, of Strings and Symbols the [[OwnPropertyKeys]] invariant already
               validated. Only the STRING keys are kept — every consumer of this cursor takes `key` or `value`
               kinds, which 7.3.23 restricts to String keys. */
            JSValue keys; uint32_t klen = 0, ki, kept = 0;
            int r = step_ownkeys_run(ctx, h, c->obj, in, &keys, out_cb, out_argc);
            in = JS_UNDEFINED;
            c->phase = EK_GOT_KEYS;
            if (r) return r < 0 ? -1 : r;
            if (js_get_length32(ctx, &klen, keys)) { JS_FreeValue(ctx, keys); return -1; }
            c->atoms = klen ? js_mallocz(ctx, sizeof(c->atoms[0]) * klen) : NULL;
            if (klen && !c->atoms) { JS_FreeValue(ctx, keys); return -1; }
            for (ki = 0; ki < klen; ki++) {
                JSValue kv = JS_GetPropertyUint32(ctx, keys, ki);
                JSAtom at;
                if (JS_IsException(kv)) { JS_FreeValue(ctx, keys); c->len = kept; return -1; }
                if (JS_IsSymbol(kv)) { JS_FreeValue(ctx, kv); continue; }
                at = JS_ValueToAtom(ctx, kv);
                JS_FreeValue(ctx, kv);
                if (at == JS_ATOM_NULL) { JS_FreeValue(ctx, keys); c->len = kept; return -1; }
                c->atoms[kept].atom = at;
                c->atoms[kept].is_enumerable = false;
                kept++;
            }
            JS_FreeValue(ctx, keys);
            c->len = kept; c->i = 0; c->kept = 0;
            /* one slot per SURVIVING key, allocated once that count is known rather than at the raw key count —
               `len` is then the block's extent under its own name, which is what lets the ownership declaration
               state it. Symbols are dropped above, so the two differ whenever the object has any. */
            if (c->want_values && c->len) {
                c->vals = js_mallocz(ctx, sizeof(c->vals[0]) * c->len);
                if (!c->vals) return -1;
            }
            c->phase = EK_ASK_DESC;
            continue;
        }
        if (c->phase == EK_ASK_DESC) {
            if (c->i >= c->len) { c->phase = EK_DONE; JS_FreeValue(ctx, in); return 0; }
            JS_FreeValue(ctx, in); in = JS_UNDEFINED;
            c->phase = EK_GOT_DESC;
            c->cb[0] = c->obj;
            *out_cb = c->cb; *out_argc = (int)c->atoms[c->i].atom;
            return 12;
        }
        if (c->phase == EK_ASK_VAL) {
            JS_FreeValue(ctx, in); in = JS_UNDEFINED;
            c->phase = EK_GOT_VAL;
            c->cb[0] = c->obj;
            *out_cb = c->cb; *out_argc = (int)c->atoms[c->kept - 1].atom;
            return 6;
        }
        if (c->phase == EK_GOT_VAL) {
            if (JS_IsException(in)) return -1;
            c->vals[c->kept - 1] = in;
            in = JS_UNDEFINED;
            c->phase = EK_ASK_DESC;
            continue;
        }
        DCHECK(c->phase == EK_GOT_DESC, "the enumerable-keys cursor resumed in no phase");
        {
            /* the descriptor object, or undefined when the property is gone between the snapshot and here.
               Reading `enumerable` off it invokes nothing: every GP_GETOWNPROP delivery rebuilds the record
               through js_desc_to_object, which the walk asserts rather than assumes. */
            JSValue desc = in;
            int enumerable;
            in = JS_UNDEFINED;
            if (JS_IsException(desc)) return -1;
            /* undefined = the property is GONE between the snapshot and the read. It is not a key of the object
               under any consumer's rule, so this is the one drop the cursor itself makes. */
            if (JS_IsUndefined(desc)) {
                JS_FreeAtom(ctx, c->atoms[c->i].atom);
                c->atoms[c->i].atom = JS_ATOM_NULL;
                c->i++; c->phase = EK_ASK_DESC;
                continue;
            }
            enumerable = js_desc_object_is_enumerable(ctx, desc);
            JS_FreeValue(ctx, desc);
            if (enumerable < 0) return -1;
            /* keep it, compacting in place: the survivors are atoms[0..kept). */
            if (c->kept != c->i) {
                c->atoms[c->kept] = c->atoms[c->i];
                c->atoms[c->i].atom = JS_ATOM_NULL;
            }
            c->atoms[c->kept].is_enumerable = (enumerable != 0);
            c->kept++;
            c->i++;
            /* 7.3.23 step 3.a.ii.2: kind key+value reads the value HERE, between this key's descriptor and the
               next key's. `enumerable` gates it, because a non-enumerable key's value is never read. */
            if (c->want_values && enumerable) { c->phase = EK_ASK_VAL; continue; }
            if (c->want_values) c->vals[c->kept - 1] = JS_UNDEFINED;
            c->phase = EK_ASK_DESC;
        }
        continue;
    }
}

/* §7.3.23 EnumerableOwnProperties' rule, applied to a COMPLETED cursor: drop the keys whose descriptor said
   not enumerable. Its consumers hand the surviving allocation on, so the drop compacts in place and shrinks
   `kept` rather than allocating a second list. */
static void js_enum_keys_keep_enumerable(JSContext *ctx, JSEnumKeys *c)
{
    uint32_t i, kept = 0;
    DCHECK(c->phase == EK_DONE, "the enumerable-only filter ran on a cursor that has not finished its walk");
    for (i = 0; i < c->kept; i++) {
        if (c->atoms[i].is_enumerable) {
            if (kept != i) {
                c->atoms[kept] = c->atoms[i]; c->atoms[i].atom = JS_ATOM_NULL;
                if (c->vals) { c->vals[kept] = c->vals[i]; c->vals[i] = JS_UNDEFINED; }
            }
            kept++;
        } else {
            JS_FreeAtom(ctx, c->atoms[i].atom);
            c->atoms[i].atom = JS_ATOM_NULL;
            if (c->vals) { JS_FreeValue(ctx, c->vals[i]); c->vals[i] = JS_UNDEFINED; }
        }
    }
    c->kept = kept;
}

