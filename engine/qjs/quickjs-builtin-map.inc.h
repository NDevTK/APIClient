/* Set/Map/WeakSet/WeakMap */

#define MAGIC_SET (1 << 0)
#define MAGIC_WEAK (1 << 1)

/* 24.1.1.1 step 3 / 24.2.1.1 step 3: the [[MapData]] / [[SetData]] slot. It is not the page's code and it is not
   the object's creation either, so it is its own function — the interpreter's consume arm creates the object from a
   ROUTED `prototype` read and then needs exactly this. -1 = threw. */
static int js_map_state_init(JSContext *ctx, JSValueConst obj, int magic)
{
    JSMapState *s = js_mallocz(ctx, sizeof(*s));
    if (!s)
        return -1;
    init_list_head(&s->records);
    s->is_weak = ((magic & MAGIC_WEAK) != 0);
    JS_SetOpaqueInternal(unsafe_unconst(obj), s);
    s->hash_size = 1;
    s->hash_table = js_malloc(ctx, sizeof(s->hash_table[0]) * s->hash_size);
    if (!s->hash_table)
        return -1;   /* the state is on the object now; the object's finalizer owns it */
    init_list_head(&s->hash_table[0]);
    s->record_count_threshold = 4;
    return 0;
}

/* An ENGINE-OWNED Map/Set: Map.groupBy's container, the Set-method results, an internal materialisation. There
   is no NewTarget, so 24.1.1.1 step 2 degenerates to OrdinaryObjectCreate over the intrinsic prototype and reads
   NOTHING — which is why this is a plain function and the CONSTRUCTOR is a machine. */
static JSValue js_map_new(JSContext *ctx, int magic)
{
    JSValue obj = JS_NewObjectClass(ctx, JS_CLASS_MAP + magic);
    if (JS_IsException(obj))
        return obj;
    if (js_map_state_init(ctx, obj, magic) < 0) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    return obj;
}

static int js_map_ctor_precheck(JSContext *ctx, const JSStepHdr *h)
{
    /* 24.1.1.1 step 1: the CALL form is rejected before anything is created, and therefore before the
       `prototype` read the create performs. */
    if (JS_IsUndefined(h->this_val)) {
        JS_ThrowTypeError(ctx, "constructor requires 'new'");
        return -1;
    }
    return 0;
}

/* 24.1.1.1 / 24.2.1.1 / 24.3.1.1 / 24.4.1.1 after step 2. Step 2 is OrdinaryCreateFromConstructor, whose
   `Get(newTarget, "prototype")` is the page's code — a getter, or a Proxy trap — and js_map_constructor
   performed it with js_create_from_ctor from its C entry, so `Reflect.construct(Map, [], proxiedCtor)` aborted
   at js_proxy_get's DFAIL. Step 3 (the data slot) and step 4 (return, for a nullish source) invoke nothing,
   which is what the declaration asserts. Steps 5-6 are the WALK and belong to the consume machine. */
static JSValue js_map_ctor_body(JSContext *ctx, JSValueConst obj_, int argc, JSValueConst *argv, int magic)
{
    JSValue obj = unsafe_unconst(obj_);
    JSValueConst arr;

    if (js_map_state_init(ctx, obj, magic) < 0)
        goto fail;

    arr = JS_UNDEFINED;
    if (argc > 0)
        arr = argv[0];
    if (!JS_IsUndefined(arr) && !JS_IsNull(arr)) {
        /* DELETED: the iteration loop. new Map/Set(iterable) over any callable @@iterator is driven by the
           ONE consume machine (ITERCONS_MAP / ITERCONS_SET), which acquires on the tramp, reads `set`/`add`
           there, and performs IfAbruptCloseIterator itself. This loop drove .next and the adder from C, where a
           coroutine cannot suspend. An iterable reaching here means a call site was not routed.
           WHAT WENT WITH IT THIS TIME: the `adder` read, the iterator locals and the fail-path IteratorClose.
           All three sat DOWNSTREAM of the DFAIL — the adder read ran first, but only on a path that then aborts
           — so they were the deleted acquire's residue, kept alive by a `fail:` label whose iterator could never
           be an object. 24.1.1.1 step 6's `? Get(map, "set")` is the page's code and belongs to the machine that
           can suspend in it, which is where it already lives; a second copy here is the legacy twin the ban is
           about, and being unreachable is not being absent. It was also the last inline JS_IteratorClose left in
           a builtin. */
        DFAIL("new Map/Set reached its C entry with a non-nullish source - route that construct shape onto the "
              "consume machine, which acquires on the chain and throws for a non-iterable itself");
        JS_ThrowTypeError(ctx, "new Map/Set: unrouted construct shape (no off-tramp implementation exists)");
        goto fail;
    }
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

/* XXX: could normalize strings to speed up comparison */
static JSValue map_normalize_key(JSContext *ctx, JSValue key)
{
    uint32_t tag = JS_VALUE_GET_TAG(key);
    // convert -0.0 to +0.0
    // not a leak; |key| and return value are not heap-allocated
    if (JS_TAG_IS_FLOAT64(tag) && JS_VALUE_GET_FLOAT64(key) == 0.0)
        return js_int32(0);
    return key;
}

static JSValueConst map_normalize_key_const(JSContext *ctx, JSValueConst key)
{
    return safe_const(map_normalize_key(ctx, unsafe_unconst(key)));
}

/* XXX: better hash ? */
static uint32_t map_hash_key(JSContext *ctx, JSValueConst key)
{
    uint32_t tag = JS_VALUE_GET_NORM_TAG(key);
    uint32_t h;
    double d;
    JSFloat64Union u;
    JSBigInt *r;

    switch(tag) {
    case JS_TAG_BOOL:
        h = JS_VALUE_GET_INT(key);
        break;
    case JS_TAG_STRING:
        h = hash_string(JS_VALUE_GET_STRING(key), 0);
        break;
    case JS_TAG_STRING_ROPE:
        h = hash_string_rope(key, 0);
        break;
    case JS_TAG_OBJECT:
    case JS_TAG_SYMBOL:
        h = (uintptr_t)JS_VALUE_GET_PTR(key) * 3163;
        break;
    case JS_TAG_INT:
        d = JS_VALUE_GET_INT(key);
        goto hash_float64;
    case JS_TAG_SHORT_BIG_INT:
        d = JS_VALUE_GET_SHORT_BIG_INT(key);
        goto hash_float64;
    case JS_TAG_BIG_INT:
        r = JS_VALUE_GET_PTR(key);
        h = hash_string8((void *)r->tab, r->len * sizeof(*r->tab), 0);
        break;
    case JS_TAG_FLOAT64:
        d = JS_VALUE_GET_FLOAT64(key);
        /* normalize the NaN */
        if (isnan(d))
            d = NAN;
    hash_float64:
        u.d = d;
        h = (u.u32[0] ^ u.u32[1]) * 3163;
        return h ^= JS_TAG_FLOAT64;
    default:
        h = 0;
        break;
    }
    h ^= tag;
    return h;
}

static JSMapRecord *map_find_record(JSContext *ctx, JSMapState *s,
                                    JSValueConst key)
{
    struct list_head *el;
    JSMapRecord *mr;
    uint32_t h;
    h = map_hash_key(ctx, key) & (s->hash_size - 1);
    list_for_each(el, &s->hash_table[h]) {
        mr = list_entry(el, JSMapRecord, hash_link);
        if (js_same_value_zero(ctx, mr->key, key))
            return mr;
    }
    return NULL;
}

static void map_hash_resize(JSContext *ctx, JSMapState *s)
{
    uint32_t new_hash_size, i, h;
    struct list_head *new_hash_table, *el;
    JSMapRecord *mr;

    /* XXX: no reporting of memory allocation failure */
    if (s->hash_size == 1)
        new_hash_size = 4;
    else
        new_hash_size = s->hash_size * 2;
    new_hash_table = js_realloc(ctx, s->hash_table,
                                sizeof(new_hash_table[0]) * new_hash_size);
    if (!new_hash_table)
        return;

    for(i = 0; i < new_hash_size; i++)
        init_list_head(&new_hash_table[i]);

    list_for_each(el, &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        if (!mr->empty) {
            h = map_hash_key(ctx, mr->key) & (new_hash_size - 1);
            list_add_tail(&mr->hash_link, &new_hash_table[h]);
        }
    }
    s->hash_table = new_hash_table;
    s->hash_size = new_hash_size;
    s->record_count_threshold = new_hash_size * 2;
}

static JSWeakRefRecord **get_first_weak_ref(JSValueConst key)
{
        switch (JS_VALUE_GET_TAG(key)) {
        case JS_TAG_OBJECT:
            {
                JSObject *p = JS_VALUE_GET_OBJ(key);
                return &p->first_weak_ref;
            }
            break;
        case JS_TAG_SYMBOL:
            {
                JSAtomStruct *p = JS_VALUE_GET_PTR(key);
                return &p->first_weak_ref;
            }
            break;
        default:
            abort();
        }
        return NULL; // pacify compiler
}

/* WHERE A RECORD SITS IS PART OF ITS STATE, because a Set/Map iterates in INSERTION order and that order is
   observable. So the two questions below are the read and the write half of one fact, and they exist because a
   COW unapply that re-added a deleted record at the TAIL rebuilt a different collection: a flow's
   `m.delete('a')` on ['a','b','c'] came back as ['b','c','a'], and a clear() came back reversed.
   The unit is the LIVE record: a zombie (mr->empty, kept alive by an iterator that still points at it) is not
   iterated and so is not a position. A list has no index, so both halves are O(position) — which is O(1) for
   the two shapes that actually delete in bulk (clear(), and `for (k of m.keys()) m.delete(k)`, both of which
   remove the head), and paid only on a captured mutation of a SHARED collection. */
static int map_record_pos(const JSMapState *s, const JSMapRecord *mr)
{
    struct list_head *el;
    int pos = 0;

    list_for_each(el, &s->records) {
        const JSMapRecord *r = list_entry(el, JSMapRecord, link);
        if (r == mr)
            return pos;
        pos += !r->empty;
    }
    DFAIL("a Set/Map record's position was asked of a collection the record is not in");
    return JS_MAP_POS_TAIL;
}

/* The node a record must be inserted BEFORE so that exactly `pos` live records precede it. JS_MAP_POS_TAIL — the
   ordinary add, and the replay of one — is the list head, which list_add_tail reads as "append". A `pos` past
   the end is the same append, and it is not a silent clamp: it is the only answer a shorter collection has, and
   the DCHECK in JS_MapAddRecord is what says a delta may not ask for one. */
static struct list_head *map_pos_before(JSMapState *s, int pos)
{
    struct list_head *el;
    int n = 0;

    if (pos == JS_MAP_POS_TAIL)
        return &s->records;
    list_for_each(el, &s->records) {
        const JSMapRecord *r = list_entry(el, JSMapRecord, link);
        if (r->empty)
            continue;
        if (n == pos)
            return el;
        n++;
    }
    return &s->records;
}

/* `pos` is where the record must land — JS_MAP_POS_TAIL for every ordinary add (10.1's append), or the number of
   live records that must precede it, which only the COW restore of a DELETED record asks for. */
static JSMapRecord *map_add_record(JSContext *ctx, JSMapState *s,
                                   JSValueConst key, int pos)
{
    uint32_t h;
    JSMapRecord *mr;

    mr = js_malloc(ctx, sizeof(*mr));
    if (!mr)
        return NULL;
    mr->ref_count = 1;
    mr->map = s;
    mr->empty = false;
    if (s->is_weak) {
        JSWeakRefRecord *wr = js_malloc(ctx, sizeof(*wr));
        if (!wr) {
            js_free(ctx, mr);
            return NULL;
        }
        wr->kind = JS_WEAK_REF_KIND_MAP;
        wr->u.map_record = mr;
        insert_weakref_record(key, wr);
        mr->key = unsafe_unconst(key);
    } else {
        mr->key = js_dup(key);
    }
    h = map_hash_key(ctx, key) & (s->hash_size - 1);
    list_add_tail(&mr->hash_link, &s->hash_table[h]);
    list_add_tail(&mr->link, map_pos_before(s, pos));
    s->record_count++;
    if (s->record_count >= s->record_count_threshold) {
        map_hash_resize(ctx, s);
    }
    return mr;
}

/* Remove the weak reference from the object weak
   reference list. we don't use a doubly linked list to
   save space, assuming a given object has few weak
       references to it */
static void delete_map_weak_ref(JSRuntime *rt, JSMapRecord *mr)
{
    JSWeakRefRecord **pwr, *wr;

    pwr = get_first_weak_ref(mr->key);
    for(;;) {
        wr = *pwr;
        DCHECK(wr != NULL, "wr != NULL");
        if (wr->kind == JS_WEAK_REF_KIND_MAP && wr->u.map_record == mr)
            break;
        pwr = &wr->next_weak_ref;
    }
    *pwr = wr->next_weak_ref;
    js_free_rt(rt, wr);
}

static void map_delete_record(JSRuntime *rt, JSMapState *s, JSMapRecord *mr)
{
    if (mr->empty)
        return;
    list_del(&mr->hash_link);
    if (s->is_weak) {
        delete_map_weak_ref(rt, mr);
    } else {
        JS_FreeValueRT(rt, mr->key);
    }
    JS_FreeValueRT(rt, mr->value);
    if (--mr->ref_count == 0) {
        list_del(&mr->link);
        js_free_rt(rt, mr);
    } else {
        /* keep a zombie record for iterators */
        mr->empty = true;
        mr->key = JS_UNDEFINED;
        mr->value = JS_UNDEFINED;
    }
    s->record_count--;
}

static void map_decref_record(JSRuntime *rt, JSMapRecord *mr)
{
    if (--mr->ref_count == 0) {
        /* the record can be safely removed */
        DCHECK(mr->empty, "mr->empty");
        list_del(&mr->link);
        js_free_rt(rt, mr);
    }
}

static JSValue js_map_set(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key, value;
    int is_set;

    if (!s)
        return JS_EXCEPTION;
    is_set = (magic & MAGIC_SET);
    key = map_normalize_key_const(ctx, argv[0]);
    if (s->is_weak && !is_valid_weakref_target(key))
        return JS_ThrowTypeError(ctx, "invalid value used as %s key", is_set ? "WeakSet" : "WeakMap");
    if (is_set)
        value = JS_UNDEFINED;
    else
        value = argv[1];
    mr = map_find_record(ctx, s, key);
    if (mr) {
        /* OVERWRITE of an existing record (Map.set of a present key): capture the old value so a snapshot-forked
           sibling restores it on unapply. A Set re-add is a no-op (same value) — nothing to capture.
           NO POSITION: an overwrite does not move the record, so its inverse creates nothing and has nothing to
           place — which is why this does not pay map_record_pos's walk. */
        if (g_time_travel.map_mutate && !is_set)
            g_time_travel.map_mutate(ctx, this_val, key, mr->value, value, JS_MAP_MUTATE_OVERWRITE,
                                     JS_MAP_POS_TAIL);
        JS_FreeValue(ctx, mr->value);
    } else {
        mr = map_add_record(ctx, s, key, JS_MAP_POS_TAIL);
        if (!mr)
            return JS_EXCEPTION;
        /* a genuinely NEW record on a shared Set/Map: capture it per-flow so a snapshot-forked sibling stays
           isolated (unapply deletes it, apply re-adds).
           WEAK COLLECTIONS COME THROUGH HERE TOO, AND THE GUARD THAT KEPT THEM OUT SAID "Weak collections are
           never snapshot-shared this way" — a claim about this tree with no derivation under it, and false for
           the commonest weak collection on the web: core-js's internal-state module does `new WeakMap` while
           the polyfill chunk runs, which is BOOT, and a boot creation is baseline by construction
           (§State-isolation). So every forked arm wrote into ONE uncaptured map. What the capture may not do is
           dup the key — see JSCowWeakRef. */
        if (g_time_travel.map_add)
            g_time_travel.map_add(ctx, this_val, key, value);
    }
    mr->value = js_dup(value);
    return js_dup(this_val);
}

/* Per-flow Set/Map COW record primitives (JS_MapAddRecord / JS_MapDeleteRecord) — see quickjs.h. Manipulate the
   internal JSMapState record directly so cow apply/unapply bypass any JS-level add/set/delete override. */
/* THERE IS NO STATUS TO DROP, and that is the change rather than a simplification. Both of these answered -1
   for two different things — "the delta names something that is not a Set/Map", which is a broken invariant of
   the engine's own logic, and "map_add_record's js_malloc failed", which is an OOM inside a CONTEXT SWITCH —
   and the two COW callers looked at neither. So a failed re-add left the baseline record silently missing AND a
   pending InternalError belonging to no flow, which the next thing to check for an exception read as its own.
   The invariant is asserted where it is decidable and the allocation is a CHECK (a dropped baseline record
   corrupts the frontier in release too), so there is nothing left for a caller to ignore. */
static JSMapState *js_map_cow_state(JSValueConst obj)
{
    JSObject *p;
    JSMapState *s;

    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT,
           "a COW delta holds a Set/Map record entry whose collection is not an object");
    p = JS_VALUE_GET_OBJ(obj);
    DCHECK(p->class_id == JS_CLASS_MAP || p->class_id == JS_CLASS_SET ||
           p->class_id == JS_CLASS_WEAKMAP || p->class_id == JS_CLASS_WEAKSET,
           "a COW delta holds a Set/Map record entry on an object that is none of the four collection classes "
           "— so the entry was read off a different object than the one it names");
    /* THE WEAK ARMS ARE CAPTURED NOW, AND THIS SENTENCE USED TO EXCLUDE THEM: it said a weak collection "is
       never captured (its records are not iterated and its keys do not survive a park)". Both halves are true
       and neither is a reason. NOT ITERATED is why `pos` is meaningless for a weak record, not why the record
       is unobservable — `has`/`get` observe it, so a sibling arm sees a key it never set. KEYS DO NOT SURVIVE A
       PARK is a fact about the KEY's lifetime that the delta now states instead of assuming: it holds a
       JSCowWeakRef, which goes dead with the key and makes every operation over it a no-op. What the exclusion
       actually cost is in cow.h's residual; what it takes to end it is JSCowWeakRef's banner. */
    s = p->u.opaque;
    DCHECK(s != NULL, "a COW delta holds a record entry for a Set/Map with no state — the collection was torn "
                      "down while a parked flow still named it");
    return s;
}
void JS_MapAddRecord(JSContext *ctx, JSValueConst obj, JSValueConst key, JSValueConst val, int pos)
{
    JSMapState *s = js_map_cow_state(obj);
    JSMapRecord *mr;
    JSValueConst nkey;

    DCHECK(pos == JS_MAP_POS_TAIL || pos >= 0, "a Set/Map record restore was given a position that is neither "
                                               "an append nor a count of the records preceding it");
    DCHECK(!s->is_weak || pos == JS_MAP_POS_TAIL,
           "a WEAK collection's record restore was given a position. A weak collection is not iterable, so the "
           "order of its records is unobservable and there is no place to put one back — the capture states "
           "that by carrying JS_MAP_POS_TAIL, and a number here is a caller that read the position off a "
           "strong record's arm");
    nkey = map_normalize_key_const(ctx, key);
    mr = map_find_record(ctx, s, nkey);
    if (mr) {
        /* ALREADY IN ITS PLACE — an overwrite writes a value into a record the collection already holds, and a
           record does not move when its value changes, so `pos` is not consulted. That is why an OVERWRITE
           entry has no position to carry. */
        JS_FreeValue(ctx, mr->value);
    } else {
        DCHECK(pos == JS_MAP_POS_TAIL || (uint32_t)pos <= s->record_count,
               "a Set/Map record restore names a position past the end of the collection — the entries invert "
               "in reverse, so everything that was removed after this record has already been put back and the "
               "position it was captured at must exist");
        mr = map_add_record(ctx, s, nkey, pos);
        CHECK(mr, "OOM re-adding a Set/Map record during a context switch — the baseline record would be gone "
                  "for every flow with nothing left that could put it back");
    }
    mr->value = js_dup(val);
}
void JS_MapDeleteRecord(JSContext *ctx, JSValueConst obj, JSValueConst key)
{
    JSMapState *s = js_map_cow_state(obj);
    JSMapRecord *mr = map_find_record(ctx, s, map_normalize_key_const(ctx, key));

    if (mr)
        map_delete_record(ctx->rt, s, mr);
}

/* THE DELTA'S HANDLE ON A WEAK RECORD — see JSCowWeakRef for why a dup of the key is not one. Registered on the
   key exactly as map_add_record's is_weak arm registers a record, so the two die together and by the same walk.
   The key is BORROWED (uncounted) and the value is dup'd but reachable only through the key. */
JSCowWeakRef *JS_NewCowWeakRef(JSContext *ctx, JSValueConst key, JSValueConst v_base, JSValueConst v_cur)
{
    JSCowWeakRef *w;
    JSWeakRefRecord *wr;

    DCHECK(is_valid_weakref_target(key),
           "a COW weak handle was asked for on a key no weak collection can hold — the only caller is the "
           "capture of a record the collection ALREADY made, and that add refused an invalid target first");
    w = js_malloc(ctx, sizeof(*w));
    if (!w)
        return NULL;
    wr = js_malloc(ctx, sizeof(*wr));
    if (!wr) {
        js_free(ctx, w);
        return NULL;
    }
    w->ref_count = 1;
    w->live = true;
    w->key = unsafe_unconst(key);   /* borrowed, like JSMapRecord.key */
    w->v_base = js_dup(v_base);
    w->v_cur = js_dup(v_cur);
    wr->kind = JS_WEAK_REF_KIND_COW;
    wr->u.cow_ref = w;
    insert_weakref_record(key, wr);
    return w;
}

/* Release one holder. The LAST one unlinks the key's record where the key is still alive, and where it is not
   there is nothing to unlink: reset_weak_ref took the record out of the list and freed it, which is what `live`
   false records. That asymmetry is the whole contract, and it is why the flag is read here rather than the key
   being tested for a tag. */
void JS_FreeCowWeakRef(JSRuntime *rt, JSCowWeakRef *w)
{
    JSWeakRefRecord **pwr, *wr;

    if (!w)
        return;
    DCHECK(w->ref_count > 0, "a COW weak handle was released more times than it was held");
    if (--w->ref_count != 0)
        return;
    if (w->live) {
        pwr = get_first_weak_ref(w->key);
        for (;;) {
            wr = *pwr;
            DCHECK(wr != NULL,
                   "a LIVE COW weak handle is not on its key's weak-ref list — the mint is the only writer of "
                   "`live` true and it inserts the record in the same call, so the two have parted company");
            if (wr->kind == JS_WEAK_REF_KIND_COW && wr->u.cow_ref == w)
                break;
            pwr = &wr->next_weak_ref;
        }
        *pwr = wr->next_weak_ref;
        js_free_rt(rt, wr);
        JS_FreeValueRT(rt, w->v_base);
        JS_FreeValueRT(rt, w->v_cur);
    } else {
        DCHECK(JS_IsUndefined(w->v_base) && JS_IsUndefined(w->v_cur) && JS_IsUndefined(w->key),
               "a NEUTRALISED COW weak handle still names a key or a value — reset_weak_ref clears both, so "
               "something else wrote the cell after the sweep took its key");
    }
    js_free_rt(rt, w);
}

/* Has the key outlived the delta's need of it? Every operation over a dead cell is a no-op, and that is exact
   rather than lossy — a dead weak key is unreachable, so neither restoring its record nor removing it is
   observable by anything that could still ask. */
int JS_CowWeakRefLive(const JSCowWeakRef *w) { return w != NULL && w->live; }

/* The capture side's own question — see quickjs.h. It reads the STATE's flag rather than comparing class ids,
   because `is_weak` is the field every arm of this file branches on and a second spelling of it here is the
   two-copies shape that drifts. */
int JS_IsWeakCollection(JSValueConst obj)
{
    JSObject *p;
    JSMapState *s;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return 0;
    p = JS_VALUE_GET_OBJ(obj);
    if (p->class_id != JS_CLASS_MAP && p->class_id != JS_CLASS_SET &&
        p->class_id != JS_CLASS_WEAKMAP && p->class_id != JS_CLASS_WEAKSET)
        return 0;
    s = p->u.opaque;
    return s != NULL && s->is_weak;
}
JSValueConst JS_CowWeakRefKey(const JSCowWeakRef *w)
{
    DCHECK(w != NULL && w->live, "a neutralised COW weak handle was asked for its key");
    return w->key;
}
JSValueConst JS_CowWeakRefBase(const JSCowWeakRef *w)
{
    DCHECK(w != NULL && w->live, "a neutralised COW weak handle was asked for the value its unapply writes");
    return w->v_base;
}
JSValueConst JS_CowWeakRefCur(const JSCowWeakRef *w)
{
    DCHECK(w != NULL && w->live, "a neutralised COW weak handle was asked for the value its apply writes");
    return w->v_cur;
}

static JSValue js_map_get(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key_const(ctx, argv[0]);
    mr = map_find_record(ctx, s, key);
    if (!mr)
        return JS_UNDEFINED;
    else
        return js_dup(mr->value);
}

/* getOrInsert / getOrInsertComputed, as ONE machine. The computed form's callbackfn is the page's code AND the
   operation continues into the map afterwards — a continuation-holding builtin, which js_map_getOrInsert drove
   with JS_Call from its C entry. Everything else in it (the record walk, the add, the weak-target test) invokes
   nothing.
   FOUR algorithms; the plain forms never reach the CALL stage, so their arrays end one stage earlier and a plain
   machine resting there is a stage past its end, which step_stage_check names on its own. */
#define MAPUPSERT_STAGES(X, ENTRY) X(MAPUPSERT_ENTRY, ENTRY)
#define MAPUPSERT_CALL_STAGE(X, CALL) X(MAPUPSERT_CALL, CALL)
enum { MAPUPSERT_STAGES(JS_STEP_STAGE_ENUM, 0) MAPUPSERT_CALL_STAGE(JS_STEP_STAGE_ENUM, 0) };
static const char *const js_map_upsert_steps[] = {
    MAPUPSERT_STAGES(JS_STEP_STAGE_LABEL,
        "24.1.3.7 steps 1-4 (M has [[MapData]]; key is canonicalized; the record walk)")
    NULL };
static const char *const js_map_upsert_comp_steps[] = {
    MAPUPSERT_STAGES(JS_STEP_STAGE_LABEL,
        "24.1.3.8 steps 1-5 (M has [[MapData]]; IsCallable(callbackfn); key is canonicalized; the record walk)")
    MAPUPSERT_CALL_STAGE(JS_STEP_STAGE_LABEL,
        "24.1.3.8 step 6 (value is Call(callbackfn, undefined, <<key>>))")
    NULL };
static const char *const js_weakmap_upsert_steps[] = {
    MAPUPSERT_STAGES(JS_STEP_STAGE_LABEL,
        "24.3.3.4 steps 1-4 (M has [[WeakMapData]]; key is a valid WeakMap key; the record walk)")
    NULL };
static const char *const js_weakmap_upsert_comp_steps[] = {
    MAPUPSERT_STAGES(JS_STEP_STAGE_LABEL,
        "24.3.3.5 steps 1-5 (M has [[WeakMapData]]; IsCallable(callbackfn); a valid key; the record walk)")
    MAPUPSERT_CALL_STAGE(JS_STEP_STAGE_LABEL,
        "24.3.3.5 step 6 (value is Call(callbackfn, undefined, <<key>>))")
    NULL };
_Static_assert(offsetof(struct JSMapUpsert, hdr) == 0, "JSStepHdr must be first in JSMapUpsert");

static int js_map_upsert_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    struct JSMapUpsert *s = st;
    bool computed = s->hdr.arg & 1;
    JSClassID class_id = (JSClassID)(s->hdr.arg >> 1);
    /* the state is re-derived at every entry: the CALLBACK can have added, deleted or rehashed records, so a
       JSMapState * or a JSMapRecord * held across it is a pointer into a table that has moved. */
    JSMapState *ms = JS_GetOpaque2(ctx, s->hdr.this_val, class_id);
    JSValueConst key;
    JSMapRecord *mr;
    JSValue value;

    if (!ms) { JS_FreeValue(ctx, cb_result); return -1; }
    key = map_normalize_key_const(ctx, step_arg(&s->hdr, 0));

    if (s->hdr.stage == MAPUPSERT_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        s->result = JS_UNDEFINED;
        s->cb_args[0] = JS_UNDEFINED; s->cb_args[1] = JS_UNDEFINED; s->cb_args[2] = JS_UNDEFINED;
        if (computed && check_function(ctx, step_arg(&s->hdr, 1)))
            return -1;
        if (ms->is_weak && !is_valid_weakref_target(key)) {
            JS_ThrowTypeError(ctx, "invalid value used as WeakMap key");
            return -1;
        }
        mr = map_find_record(ctx, ms, key);
        if (mr) { s->result = js_dup(mr->value); return 0; }
        if (computed) {
            s->hdr.stage = MAPUPSERT_CALL;
            s->cb_args[1] = js_dup(step_arg(&s->hdr, 1));
            s->cb_args[2] = js_dup(key);
            *out_cb = s->cb_args; *out_argc = 1;
            return 3;   /* CALL */
        }
        value = js_dup(step_arg(&s->hdr, 1));
    } else {
        /* the callbackfn returned. It may have inserted the key itself, and the spec's step 7.a SETS that
           record's value — it does not remove and re-append it. The C body deleted and re-added, which moves the
           entry to the END of a Map's insertion order, so
           `m.set("a",1); m.getOrInsertComputed("b", () => { m.set("b",0); m.set("c",3); return 2 })` iterated
           a, c, b where the standard iterates a, b, c. Only naming the stage after step 6 made the step after
           it something to read. The table is looked up again because the callback may have rehashed it. */
        DCHECK(s->hdr.stage == MAPUPSERT_CALL, "the map upsert resumed in no stage");
        value = cb_result;
        mr = map_find_record(ctx, ms, key);
        if (mr) {
            JS_FreeValue(ctx, mr->value);
            mr->value = value;
            s->result = js_dup(value);
            return 0;
        }
    }
    mr = map_add_record(ctx, ms, key, JS_MAP_POS_TAIL);
    if (!mr) { JS_FreeValue(ctx, value); return -1; }
    mr->value = value;
    s->result = js_dup(value);
    return 0;
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). It had none, so a concolic branch inside the callbackfn would
   have hit the fork's DCHECK rather than forking: two arms of a flow standing in one getOrInsertComputed each
   need their own callback operands and their own result. */
static void js_map_upsert_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    struct JSMapUpsert *s = st;
    v->val(ctx, &s->result);
    v->val(ctx, &s->cb_args[0]);
    v->val(ctx, &s->cb_args[1]);
    v->val(ctx, &s->cb_args[2]);
}

static JSValue js_map_upsert_fini(JSContext *ctx, void *st, bool take_result)
{
    struct JSMapUpsert *s = st;
    JSValue r = take_result ? s->result : JS_UNDEFINED;
    if (take_result) s->result = JS_UNDEFINED;
    return r;
}

static JSValue js_map_has(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key_const(ctx, argv[0]);
    mr = map_find_record(ctx, s, key);
    return js_bool(mr != NULL);
}

static JSValue js_map_delete(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    JSMapRecord *mr;
    JSValueConst key;

    if (!s)
        return JS_EXCEPTION;
    key = map_normalize_key_const(ctx, argv[0]);
    mr = map_find_record(ctx, s, key);
    if (!mr)
        return JS_FALSE;
    /* DELETE of a present record on a shared Set/Map: capture the key+old value AND ITS POSITION, so a
       snapshot-forked sibling re-adds it where it was (a Set's value is UNDEFINED, harmless). Without the
       position the unapply appended, and `m.delete('a')` on ['a','b','c'] came back as ['b','c','a'] — a
       different collection, since iteration order is insertion order and observable. */
    /* A WEAK record carries NO POSITION and must not be asked for one: map_record_pos walks the record list to
       count what precedes it, and for a weak collection that number is unobservable (it is not iterable) and
       the walk is O(records) for nothing. JS_MAP_POS_TAIL is the positive statement of that, and JS_MapAddRecord
       asserts a weak restore never arrives carrying anything else. */
    if (g_time_travel.map_mutate)
        g_time_travel.map_mutate(ctx, this_val, key, mr->value, JS_UNDEFINED, JS_MAP_MUTATE_DELETE,
                                 s->is_weak ? JS_MAP_POS_TAIL : map_record_pos(s, mr));
    map_delete_record(ctx->rt, s, mr);
    return JS_TRUE;
}

static JSValue js_map_clear(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    struct list_head *el, *el1;
    JSMapRecord *mr;

    if (!s)
        return JS_EXCEPTION;
    /* `clear` is defined on Map and Set only — a weak collection has no such method — so the guard js_map_delete
       carries for its own weak spellings is here a statement about the engine instead. */
    DCHECK(!s->is_weak, "Set/Map.prototype.clear ran on a WEAK collection");
    list_for_each_safe(el, el1, &s->records) {
        mr = list_entry(el, JSMapRecord, link);
        if (mr->empty)
            continue;   /* a zombie an iterator still points at: already deleted, nothing to capture */
        /* forced-exec TIME-TRAVEL: A TRUNCATION IS THE ONE MUTATION THAT DESTROYS N RECORDS PER CALL, so it is
           the one whose absence from the delta costs the most — and this one captured NOTHING at all, three
           functions below the delete that captures every single record. `sharedMap.clear()` in one flow emptied
           the collection for every sibling, permanently. Each removal is the same reversible DELETE js_map_delete
           records, taken in list order, so each is at position 0 by the time it is removed and the reverse-order
           inversion rebuilds the collection in its original order. */
        if (g_time_travel.map_mutate) {
            uint32_t n = s->record_count;
            g_time_travel.map_mutate(ctx, this_val, mr->key, mr->value, JS_UNDEFINED,
                                     JS_MAP_MUTATE_DELETE, map_record_pos(s, mr));
            DCHECK(s->record_count == n && !mr->empty,
                   "the collection changed while its cleared records were being captured — a capture may grow "
                   "the host's delta and a growth may release a PARKED flow, which is why it may never revert "
                   "the running flow's heap; `el1` is a node this walk is holding across that call");
        }
        map_delete_record(ctx->rt, s, mr);
    }
    return JS_UNDEFINED;
}

static JSValue js_map_get_size(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSMapState *s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    if (!s)
        return JS_EXCEPTION;
    return js_uint32(s->record_count);
}

/* 24.1.3.5 Map.prototype.forEach / 24.2.4.7 Set.prototype.forEach. The callback is the page's code and the loop
   drove it with JS_Call from C, so a callback containing a loop preempted in an activation with no flow base —
   and between them Map and Set were the whole of their directories' residual drive count.
   The traversal is unchanged, including the part that is not obvious: the record is LOCKED across the call and
   the cursor advanced only afterwards, because the callback is allowed to delete it (and to add entries, which
   the spec then requires this loop to visit). What changes is that "across the call" now means across a
   SUSPENSION — so the locked record and the cursor live on the state, and fini releases the lock, because an
   abandoned machine is the one path a C local could not have covered. */
/* TWO algorithms, one machine: the walk is the same and a Set passes its key twice. ONE stage list expanded once
   per algorithm; see AFIND_STAGES. */
#define MAPFOREACH_STAGES(X, ENTRY, CALL) \
    X(MAPFOREACH_ENTRY, ENTRY) \
    X(MAPFOREACH_CALL,  CALL)
enum { MAPFOREACH_STAGES(JS_STEP_STAGE_ENUM, 0, 0) };
static const char *const js_map_foreach_steps[] = {
    MAPFOREACH_STAGES(JS_STEP_STAGE_LABEL,
        "24.1.3.5 steps 1-6 (M has [[MapData]]; IsCallable(callbackfn); entries; index is 0)",
        "24.1.3.5 step 7.c.i (Call(callbackfn, thisArg, <<e.[[Value]], e.[[Key]], M>>))")
    NULL };
static const char *const js_set_foreach_steps[] = {
    MAPFOREACH_STAGES(JS_STEP_STAGE_LABEL,
        "24.2.4.7 steps 1-6 (S has [[SetData]]; IsCallable(callbackfn); entries; index is 0)",
        "24.2.4.7 step 7.b.i (Call(callbackfn, thisArg, <<e, e, S>>))")
    NULL };

static int js_map_foreach_step(JSContext *ctx, void *st, JSValue cb_result, JSValue **out_cb, int *out_argc)
{
    JSMapForEach *m = st;
    int magic = m->hdr.arg;
    JSMapState *s;

    if (m->hdr.stage == MAPFOREACH_ENTRY) {
        JS_FreeValue(ctx, cb_result);
        m->hdr.stage = MAPFOREACH_CALL;
        s = JS_GetOpaque2(ctx, m->hdr.this_val, JS_CLASS_MAP + magic);
        if (!s)
            return -1;
        if (check_function(ctx, step_arg(&m->hdr, 0)))
            return -1;
        m->s = (struct JSMapState *)s;
        m->el = s->records.next;
        m->cb[0] = js_dup(step_arg(&m->hdr, 1));   /* thisArg — constant, dup'd once */
        m->cb[1] = js_dup(step_arg(&m->hdr, 0));   /* the callback */
        m->cb[4] = js_dup(m->hdr.this_val);        /* the collection, the third argument */
    } else {
        /* the callback RETURNED. A throw never arrives here — forEach catches nothing, so the driver unwinds
           and fini releases the lock. Advance then unlock, in that order: the lock is what keeps el->next
           readable after a callback that deleted this record. */
        JS_FreeValue(ctx, cb_result);
        DCHECK(m->lock != NULL, "a forEach callback returned with no locked record");
        m->el = m->el->next;
        map_decref_record(ctx->rt, (JSMapRecord *)m->lock);
        m->lock = NULL;
        JS_FreeValue(ctx, m->cb[2]); m->cb[2] = JS_UNDEFINED;
        JS_FreeValue(ctx, m->cb[3]); m->cb[3] = JS_UNDEFINED;
    }

    s = (JSMapState *)m->s;
    while (m->el != &s->records) {
        JSMapRecord *mr = list_entry(m->el, JSMapRecord, link);
        if (mr->empty) {
            m->el = m->el->next;
            continue;
        }
        mr->ref_count++;                       /* LOCK for the duration of the call */
        m->lock = (struct JSMapRecord *)mr;
        /* dup'd because the callback may delete the record: both values must outlive it */
        m->cb[3] = js_dup(mr->key);
        m->cb[2] = magic ? js_dup(mr->key) : js_dup(mr->value);   /* a Set passes the key twice */
        *out_cb = m->cb; *out_argc = 3;
        return 3;
    }
    return 0;
}

/* The two halves of the record LOCK, defined here where the record type is complete. */
static void js_step_visit_dup_maprec(JSContext *ctx, struct JSMapRecord **slot)
{
    (void)ctx;
    if (*slot) ((JSMapRecord *)*slot)->ref_count++;
}
static void js_step_visit_free_maprec(JSContext *ctx, struct JSMapRecord **slot)
{
    if (*slot) { map_decref_record(ctx->rt, (JSMapRecord *)*slot); *slot = NULL; }
}

/* WHAT THIS MACHINE OWNS (JSTrampStepDef.visit). Map.forEach and Set.forEach drive the page's callback once
   per record, so a concolic branch inside one forks the walk — and each arm then stands on the SAME record
   with its own lock, which is what stops the first to finish freeing what the second is still reading. `el` is
   the cursor into the collection's record list, kept alive by exactly that lock; `s` is borrowed, the receiver
   on the header holds the collection. */
static void js_map_foreach_visit(JSContext *ctx, void *st, JSStepVisit *v)
{
    JSMapForEach *m = st;
    int i;
    v->maprec(ctx, (struct JSMapRecord **)&m->lock);
    for (i = 0; i < 5; i++) v->val(ctx, &m->cb[i]);
}


static const JSTrampStepDef js_map_foreach_def = {
    sizeof(JSMapForEach), js_map_foreach_step, NULL, 0, .visit = js_map_foreach_visit,
    .algorithm = "24.1.3.5 Map.prototype.forEach",
    .steps = js_map_foreach_steps
};
static const JSTrampStepDef js_set_foreach_def = {
    sizeof(JSMapForEach), js_map_foreach_step, NULL, MAGIC_SET, .visit = js_map_foreach_visit,
    .algorithm = "24.2.4.7 Set.prototype.forEach",
    .steps = js_set_foreach_steps
};


static void js_map_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSObject *p;
    JSMapState *s;
    struct list_head *el, *el1;
    JSMapRecord *mr;

    p = JS_VALUE_GET_OBJ(val);
    s = p->u.map_state;
    if (s) {
        /* if the object is deleted we are sure that no iterator is
           using it */
        list_for_each_safe(el, el1, &s->records) {
            mr = list_entry(el, JSMapRecord, link);
            if (!mr->empty) {
                if (s->is_weak)
                    delete_map_weak_ref(rt, mr);
                else
                    JS_FreeValueRT(rt, mr->key);
                JS_FreeValueRT(rt, mr->value);
            }
            js_free_rt(rt, mr);
        }
        js_free_rt(rt, s->hash_table);
        js_free_rt(rt, s);
    }
}

static void js_map_mark(JSRuntime *rt, JSValueConst val,
                        JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSMapState *s;
    struct list_head *el;
    JSMapRecord *mr;

    s = p->u.map_state;
    if (s) {
        DCHECK(!s->is_weak, "!s->is_weak");
        list_for_each(el, &s->records) {
            mr = list_entry(el, JSMapRecord, link);
            JS_MarkValue(rt, mr->key, mark_func);
            JS_MarkValue(rt, mr->value, mark_func);
        }
    }
}

/* Map Iterator */

typedef struct JSMapIteratorData {
    JSValue obj;
    JSIteratorKindEnum kind;
    JSMapRecord *cur_record;
} JSMapIteratorData;

static void js_map_iterator_finalizer(JSRuntime *rt, JSValueConst val)
{
    JSObject *p;
    JSMapIteratorData *it;

    p = JS_VALUE_GET_OBJ(val);
    it = p->u.map_iterator_data;
    if (it) {
        /* During the GC sweep phase the Map finalizer may be
           called before the Map iterator finalizer */
        if (JS_IsLiveObject(rt, it->obj) && it->cur_record) {
            map_decref_record(rt, it->cur_record);
        }
        JS_FreeValueRT(rt, it->obj);
        js_free_rt(rt, it);
    }
}

static void js_map_iterator_mark(JSRuntime *rt, JSValueConst val,
                                 JS_MarkFunc *mark_func)
{
    JSObject *p = JS_VALUE_GET_OBJ(val);
    JSMapIteratorData *it;
    it = p->u.map_iterator_data;
    if (it) {
        /* the record is already marked by the object */
        JS_MarkValue(rt, it->obj, mark_func);
    }
}

static JSValue js_create_map_iterator(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv, int magic)
{
    JSIteratorKindEnum kind;
    JSMapState *s;
    JSMapIteratorData *it;
    JSValue enum_obj;

    kind = magic >> 2;
    magic &= 3;
    s = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP + magic);
    if (!s)
        return JS_EXCEPTION;
    enum_obj = JS_NewObjectClass(ctx, JS_CLASS_MAP_ITERATOR + magic);
    if (JS_IsException(enum_obj))
        goto fail;
    it = js_malloc(ctx, sizeof(*it));
    if (!it) {
        JS_FreeValue(ctx, enum_obj);
        goto fail;
    }
    it->obj = js_dup(this_val);
    it->kind = kind;
    it->cur_record = NULL;
    JS_SetOpaqueInternal(enum_obj, it);
    return enum_obj;
 fail:
    return JS_EXCEPTION;
}

static JSValue js_map_iterator_next(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv,
                                    int *pdone, int magic)
{
    JSMapIteratorData *it;
    JSMapState *s;
    JSMapRecord *mr;
    struct list_head *el;

    it = JS_GetOpaque2(ctx, this_val, JS_CLASS_MAP_ITERATOR + magic);
    if (!it) {
        *pdone = false;
        return JS_EXCEPTION;
    }
    if (JS_IsUndefined(it->obj))
        goto done;
    s = JS_GetOpaque(it->obj, JS_CLASS_MAP + magic);
    DCHECK(s != NULL, "s != NULL");
    if (!it->cur_record) {
        el = s->records.next;
    } else {
        mr = it->cur_record;
        el = mr->link.next;
        map_decref_record(ctx->rt, mr); /* the record can be freed here */
    }
    for(;;) {
        if (el == &s->records) {
            /* no more record  */
            it->cur_record = NULL;
            JS_FreeValue(ctx, it->obj);
            it->obj = JS_UNDEFINED;
        done:
            /* end of enumeration */
            *pdone = true;
            return JS_UNDEFINED;
        }
        mr = list_entry(el, JSMapRecord, link);
        if (!mr->empty)
            break;
        /* get the next record */
        el = mr->link.next;
    }

    /* lock the record so that it won't be freed */
    mr->ref_count++;
    it->cur_record = mr;
    *pdone = false;

    if (it->kind == JS_ITERATOR_KIND_KEY) {
        return js_dup(mr->key);
    } else {
        JSValueConst args[2];
        args[0] = mr->key;
        if (magic)
            args[1] = mr->key;
        else
            args[1] = mr->value;
        if (it->kind == JS_ITERATOR_KIND_VALUE) {
            return js_dup(args[1]);
        } else {
            return js_create_array(ctx, 2, args);
        }
    }
}

static JSValue js_map_read(BCReaderState *s, int magic)
{
    JSContext *ctx = s->ctx;
    JSValue obj, rv, argv[2];
    uint32_t i, prop_count;

    argv[0] = JS_UNDEFINED;
    argv[1] = JS_UNDEFINED;
    (void)argv; (void)rv; (void)i;
    obj = js_map_new(ctx, magic);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (BC_add_object_ref(s, obj))
        goto fail;
    if (bc_get_leb128(s, &prop_count))
        goto fail;
    /* The records are a FRAME: a key and (for a map) a value are each a whole nested read, and the key has to
       be held across the value's. */
    if (bcr_push(s) < 0)
        goto fail;
    {
        BCRFrame *f = &s->r_stack[s->r_sp - 1];
        f->kind = BCR_MAP;
        f->obj = obj;
        f->len = prop_count;
        f->flag = (magic & MAGIC_SET) != 0;
        f->u1 = magic;
    }
    return JS_UNDEFINED;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static int js_map_write(BCWriterState *s, struct JSMapState *map_state,
                        int magic)
{
    struct list_head *el;
    JSMapRecord *mr;

    bc_put_leb128(s, map_state ? map_state->record_count : 0);
    /* The records are a QUEUED walk for the same reason the array's elements are: a key or value can be an
       object whose own write pushes further work, and that subtree must finish before the next record. */
    if (map_state && !list_empty(&map_state->records)) {
        BCWItem it;
        memset(&it, 0, sizeof(it));
        it.kind = BCW_MAPENT;
        it.map = map_state;
        it.mr = list_entry(map_state->records.next, JSMapRecord, link);
        it.flag = (magic & MAGIC_SET) != 0;   /* a set record is key-only */
        if (bcw_push(s, it))
            return -1;
    }
    (void)el; (void)mr;
    return 0;
}

static JSValue JS_ReadMap(BCReaderState *s)
{
    return js_map_read(s, 0);
}

static JSValue JS_ReadSet(BCReaderState *s)
{
    return js_map_read(s, MAGIC_SET);
}

static int JS_WriteMap(BCWriterState *s, struct JSMapState *map_state)
{
    return js_map_write(s, map_state, 0);
}

static int JS_WriteSet(BCWriterState *s, struct JSMapState *map_state)
{
    return js_map_write(s, map_state, MAGIC_SET);
}




/* DELETED: js_set_isSupersetOf. The whole operation is do_setop_consume_tramp — the setlike's props read in spec
   order, THIS's records copied straight into the result (the spec forbids calling .add), and keys() folded in by
   the shared acquire so a generator `*keys()` runs on the tramp. This body still had a LIVE JS_IteratorNext loop,
   reachable only through the recognizer's declines; with the walk declared at the definition no pointer names
   it, and js_call_c_function's JS_CFUNC_consume arm is the backstop for a call site that was never routed. */



/* DELETED: js_set_symmetricDifference. The whole operation is do_setop_consume_tramp — the setlike's props read in spec
   order, THIS's records copied straight into the result (the spec forbids calling .add), and keys() folded in by
   the shared acquire so a generator `*keys()` runs on the tramp. This body still had a LIVE JS_IteratorNext loop,
   reachable only through the recognizer's declines; with the walk declared at the definition no pointer names
   it, and js_call_c_function's JS_CFUNC_consume arm is the backstop for a call site that was never routed. */

/* DELETED: js_set_union. The whole operation is do_setop_consume_tramp — the setlike's props read in spec
   order, THIS's records copied straight into the result (the spec forbids calling .add), and keys() folded in by
   the shared acquire so a generator `*keys()` runs on the tramp. This body still had a LIVE JS_IteratorNext loop,
   reachable only through the recognizer's declines; with the walk declared at the definition no pointer names
   it, and js_call_c_function's JS_CFUNC_consume arm is the backstop for a call site that was never routed. */

static const JSCFunctionListEntry js_map_funcs[] = {
    JS_CFUNC_CONSUME_DEF("groupBy", 2, ITERCONS_GROUPBY_BASE + GROUPBY_COLLECTION ),
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static const JSCFunctionListEntry js_set_funcs[] = {
    JS_CGETSET_DEF("[Symbol.species]", js_get_this, NULL ),
};

static const JSCFunctionListEntry js_map_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("set", 2, js_map_set, 0 ),
    JS_CFUNC_MAGIC_DEF("get", 1, js_map_get, 0 ),
    JS_CFUNC_STEP_DEF("getOrInsert", 2, STEPDEF_MAP_UPSERT ),
    JS_CFUNC_STEP_DEF("getOrInsertComputed", 2, STEPDEF_MAP_UPSERT_COMPUTED ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, 0 ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, 0 ),
    JS_CFUNC_MAGIC_DEF("clear", 0, js_map_clear, 0 ),
    JS_CGETSET_MAGIC_DEF("size", js_map_get_size, NULL, 0),
    JS_CFUNC_STEP_DEF("forEach", 1, STEPDEF_MAP_FOREACH ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_map_iterator, (JS_ITERATOR_KIND_VALUE << 2) | 0 ),
    JS_CFUNC_MAGIC_DEF("keys", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY << 2) | 0 ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY_AND_VALUE << 2) | 0 ),
    JS_ALIAS_DEF("[Symbol.iterator]", "entries" ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Map", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_map_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_map_iterator_next, 0 ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Map Iterator", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_set_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, js_map_set, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, MAGIC_SET ),
    JS_CFUNC_MAGIC_DEF("clear", 0, js_map_clear, MAGIC_SET ),
    JS_CGETSET_MAGIC_DEF("size", js_map_get_size, NULL, MAGIC_SET ),
    JS_CFUNC_STEP_DEF("forEach", 1, STEPDEF_SET_FOREACH ),
    JS_CFUNC_CONSUME_DEF("isDisjointFrom", 1, ITERCONS_SETOP_BASE + SETOP_DISJOINT ),
    JS_CFUNC_CONSUME_DEF("isSubsetOf", 1, ITERCONS_SETOP_BASE + SETOP_SUBSET ),
    JS_CFUNC_CONSUME_DEF("isSupersetOf", 1, ITERCONS_SETOP_BASE + SETOP_SUPERSET ),
    JS_CFUNC_CONSUME_DEF("intersection", 1, ITERCONS_SETOP_BASE + SETOP_INTERSECT ),
    JS_CFUNC_CONSUME_DEF("difference", 1, ITERCONS_SETOP_BASE + SETOP_DIFF ),
    JS_CFUNC_CONSUME_DEF("symmetricDifference", 1, ITERCONS_SETOP_BASE + SETOP_SYMDIFF ),
    JS_CFUNC_CONSUME_DEF("union", 1, ITERCONS_SETOP_BASE + SETOP_UNION ),
    JS_CFUNC_MAGIC_DEF("values", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY << 2) | MAGIC_SET ),
    JS_ALIAS_DEF("keys", "values" ),
    JS_ALIAS_DEF("[Symbol.iterator]", "values" ),
    JS_CFUNC_MAGIC_DEF("entries", 0, js_create_map_iterator, (JS_ITERATOR_KIND_KEY_AND_VALUE << 2) | MAGIC_SET ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Set", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_set_iterator_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_map_iterator_next, MAGIC_SET ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Set Iterator", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_weak_map_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("set", 2, js_map_set, MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("get", 1, js_map_get, MAGIC_WEAK ),
    JS_CFUNC_STEP_DEF("getOrInsert", 2, STEPDEF_WEAKMAP_UPSERT ),
    JS_CFUNC_STEP_DEF("getOrInsertComputed", 2, STEPDEF_WEAKMAP_UPSERT_COMPUTED ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, MAGIC_WEAK ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WeakMap", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry js_weak_set_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("add", 1, js_map_set, MAGIC_SET | MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("has", 1, js_map_has, MAGIC_SET | MAGIC_WEAK ),
    JS_CFUNC_MAGIC_DEF("delete", 1, js_map_delete, MAGIC_SET | MAGIC_WEAK ),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "WeakSet", JS_PROP_CONFIGURABLE ),
};

static const JSCFunctionListEntry * const js_map_proto_funcs_ptr[6] = {
    js_map_proto_funcs,
    js_set_proto_funcs,
    js_weak_map_proto_funcs,
    js_weak_set_proto_funcs,
    js_map_iterator_proto_funcs,
    js_set_iterator_proto_funcs,
};

static const uint8_t js_map_proto_funcs_count[6] = {
    countof(js_map_proto_funcs),
    countof(js_set_proto_funcs),
    countof(js_weak_map_proto_funcs),
    countof(js_weak_set_proto_funcs),
    countof(js_map_iterator_proto_funcs),
    countof(js_set_iterator_proto_funcs),
};

int JS_AddIntrinsicMapSet(JSContext *ctx)
{
    int i;
    JSValue obj1;
    char buf[ATOM_GET_STR_BUF_SIZE];
    for(i = 0; i < 4; i++) {
        const char *name = JS_AtomGetStr(ctx, buf, sizeof(buf),
                                         JS_ATOM_Map + i);
        int class_id = JS_CLASS_MAP + i;
        const JSCFunctionListEntry *ctor_funcs;
        int n_ctor_funcs;
        if (class_id == JS_CLASS_MAP) {
            ctor_funcs = js_map_funcs;
            n_ctor_funcs = countof(js_map_funcs);
        } else if (class_id == JS_CLASS_SET) {
            ctor_funcs = js_set_funcs;
            n_ctor_funcs = countof(js_set_funcs);
        } else {
            ctor_funcs = NULL;
            n_ctor_funcs = 0;
        }
        obj1 = JS_NewCConstructor(ctx, class_id, name,
                                  NULL, 0, JS_CFUNC_step_ctor, STEPDEF_MAP_CTOR + i,
                                  JS_UNDEFINED,
                                  ctor_funcs, n_ctor_funcs,
                                  js_map_proto_funcs_ptr[i], js_map_proto_funcs_count[i],
                                  0);
        if (JS_IsException(obj1))
            return -1;
        /* 24.1.1.1 step 6 onwards is a WALK of the iterable, and it declares it here rather than being
           recognised by address at the construct convergence point. */
        js_declare_native_machine(obj1, ITERCONS_SETMAP_CTOR_BASE + i);
        JS_FreeValue(ctx, obj1);
    }

    for(i = 0; i < 2; i++) {
        ctx->class_proto[JS_CLASS_MAP_ITERATOR + i] =
            JS_NewObjectProtoList(ctx, ctx->class_proto[JS_CLASS_ITERATOR],
                                  js_map_proto_funcs_ptr[i + 4],
                                  js_map_proto_funcs_count[i + 4]);
        if (JS_IsException(ctx->class_proto[JS_CLASS_MAP_ITERATOR + i]))
            return -1;
    }
    return 0;
}

