/* The next link of an ordinary lookup walk. 10.1.8.1 OrdinaryGet step 2.a, 10.1.9.2 OrdinarySetWithOwnDescriptor
   step 1.a and 10.1.7.1 OrdinaryHasProperty step 3 each ask `obj.[[GetPrototypeOf]]()`, which for a class that
   computes its prototype is the class's answer, not the shape link (HTML §7.2.3.1's WindowProxy answers its
   Window's), where the class declares the walk may take it (get_prototype_walkable). Borrowed: the class holds
   what it answers, which the assert states, and a walk runs nothing between taking a link and reading it. The
   hook runs no page code (its declaration's contract), so a throw is not an answer any walk could report. */
static JSObject *js_walk_proto(JSContext *ctx, JSObject *p)
{
    const JSClassExoticMethods *em = js_exotic_getproto(ctx->rt, p);
    JSValue v;
    JSObject *q;

    if (likely(!em) || !em->get_prototype_walkable)
        return p->shape->proto;
    v = em->get_prototype(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
    CHECK(!JS_IsException(v),
          "a class's [[GetPrototypeOf]] threw inside an ordinary lookup walk — the hook is declared to run none of "
          "the page's code, so the walk has no completion to hand the throw to");
    if (JS_IsNull(v))
        return NULL;
    DCHECK(JS_IsObject(v), "a class's [[GetPrototypeOf]] answered a link that is neither an Object nor null");
    q = JS_VALUE_GET_OBJ(v);
    DCHECK(JS_REF_COUNT(q) > 1,
           "a class's [[GetPrototypeOf]] answered a prototype it does not hold — the walk keeps a bare pointer to "
           "the link, so the answer's only reference cannot be the one the walk is about to drop");
    JS_FreeValue(ctx, v);
    return q;
}

/* return -1 (exception) or true/false */
static int JS_SetPrototypeInternal(JSContext *ctx, JSValueConst obj,
                                   JSValueConst proto_val, bool throw_flag)
{
    JSObject *proto, *p, *p1;
    const JSClassExoticMethods *em;
    JSShape *sh;

    if (throw_flag) {
        if (JS_VALUE_GET_TAG(obj) == JS_TAG_NULL ||
            JS_VALUE_GET_TAG(obj) == JS_TAG_UNDEFINED)
            goto not_obj;
    } else {
        if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
            goto not_obj;
    }
    p = JS_VALUE_GET_OBJ(obj);
    if (JS_VALUE_GET_TAG(proto_val) != JS_TAG_OBJECT) {
        if (JS_VALUE_GET_TAG(proto_val) != JS_TAG_NULL) {
        not_obj:
            JS_ThrowTypeErrorNotAnObject(ctx);
            return -1;
        }
        proto = NULL;
    } else {
        proto = JS_VALUE_GET_OBJ(proto_val);
    }

    if (throw_flag && JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return true;

    if (unlikely(p->class_id == JS_CLASS_PROXY))
        return js_proxy_setPrototypeOf(ctx, obj, proto_val, throw_flag);
    /* 10.4.7.2 SetImmutablePrototype ( obj, proto ) — for a class that COMPUTES its prototype
       (JSClassExoticMethods.get_prototype) there is no slot for a set to land in, so the only accepted value is
       the one step 1's `obj.[[GetPrototypeOf]]()` already answers. It is HERE, above the shape read, because
       the shape's link is NOT that answer for such a class: for HTML §7.2.3's WindowProxy it is the reading
       realm's own surface object, so `sh->proto == proto` below would report "already equal" for the wrong
       value and, worse, let every other value be WRITTEN into a link nothing reads any more — a mutation of the
       per-flow binding's stand-in that no flow's delta captures.
       Object.prototype's own arm below is the same clause (20.1.3 makes it an immutable prototype exotic
       object) reached from a different direction, which is why the two read alike. */
    em = js_exotic_getproto(ctx->rt, p);
    if (unlikely(em != NULL) && !em->prototype_slot_settable) {
        JSValue cur = em->get_prototype(ctx, obj);   /* step 1 */
        JSObject *cur_p;
        if (unlikely(JS_IsException(cur)))
            return -1;
        /* STEP 2's SameValue IS NOT DECIDABLE WHEN STEP 1 ANSWERED AN UNKNOWN, and the pointer test below
           would decide it anyway — always FALSE, because a derived prototype is a fresh allocation and never
           the object the page handed in. That is a fabrication of exactly the kind this class's own
           [[GetPrototypeOf]] was built to stop, one internal method further on: `Object.setPrototypeOf(rec,
           Base.prototype)` over a server-injected record reports the write as refused, and the throwing form
           raises a TypeError, at a line real Chrome does not refuse at.
           IT IS THE SAME TWO-COMPLETION QUESTION 7.3.21's step 6.c ASKS — "is this unknown that object?" —
           with nothing else attached, so what belongs here is that ask and not a walk: the outcome seam over
           `cur`, two completions, `real` unstated, the true arm returning true and the false arm falling
           through to step 3. It is a DCHECK and not a residual because the answer beneath it is WRONG rather
           than narrower: there is no case this door handles correctly today. */
        DCHECK(!js_value_is_concolic(cur),
               "10.4.7.2 SetImmutablePrototype ( obj, proto ) step 2 compares `proto` against a [[Prototype]] "
               "this engine does not know, and the comparison below answers it in C — always false, so the "
               "page is told its write was refused by a value nobody observed. Ask the outcome seam over the "
               "value step 1 returned, two completions, exactly as 7.3.21's link walk asks step 6.c");
        cur_p = JS_IsNull(cur) ? NULL : JS_VALUE_GET_OBJ(cur);
        JS_FreeValue(ctx, cur);
        if (cur_p == proto)                          /* step 2: SameValue(proto, current) over Object|null */
            return true;
        if (throw_flag) {
            char cbuf[ATOM_GET_STR_BUF_SIZE];
            JS_ThrowTypeError(ctx, "the prototype of a '%s' is computed and cannot be set",
                              JS_AtomGetStrRT(ctx->rt, cbuf, sizeof(cbuf),
                                              ctx->rt->class_array[p->class_id].class_name));
            return -1;
        }
        return false;                                /* step 3 */
    }
    sh = p->shape;
    if (sh->proto == proto)
        return true;
    if (p == JS_VALUE_GET_OBJ(ctx->class_proto[JS_CLASS_OBJECT])) {
        if (throw_flag) {
            JS_ThrowTypeError(ctx, "'Immutable prototype object \'Object.prototype\' cannot have their prototype set'");
            return -1;
        }
        return false;
    }
    if (!p->extensible) {
        if (throw_flag) {
            JS_ThrowTypeError(ctx, "object is not extensible");
            return -1;
        } else {
            return false;
        }
    }
    if (proto) {
        /* check if there is a cycle */
        p1 = proto;
        do {
            if (p1 == p) {
                if (throw_flag) {
                    JS_ThrowTypeError(ctx, "circular prototype chain");
                    return -1;
                } else {
                    return false;
                }
            }
            /* Note: for Proxy objects, proto is NULL */
            p1 = p1->shape->proto;
        } while (p1 != NULL);
        js_dup(proto_val);
    }

    /* forced-exec: the object's own state, before this flow changes it. Here rather than earlier because every
       refusal above is a write that does not happen; `sh` is re-read below anyway, which it must be — a capture
       can grow the host's delta and a growth can reshape. */
    cow_capture_obj(ctx, p);
    if (js_shape_prepare_update(ctx, p, NULL))
        return -1;
    sh = p->shape;
    if (sh->proto)
        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, sh->proto));
    sh->proto = proto;
    if (proto)
        proto->is_prototype = true;
    if (p->is_prototype) {
        /* track modification of Array.prototype */
        if (unlikely(p == JS_VALUE_GET_OBJ(ctx->class_proto[JS_CLASS_ARRAY]))) {
            ctx->std_array_prototype = false;
        }
    }
    return true;
}

/* THE OBJECT'S OWN STATE, SAVED AND PUT BACK — the three halves of one COW entry (see cow_capture_obj). It is
   engine-internal because two of the three are: `extensible` is a bit on the JSObject, the prototype lives on
   the SHAPE and is an owned reference, and `u.object_data` is a union member only some classes have. The blob
   holds a reference on each owned half, exactly as the record arm's does, because the same blob is restored
   more than once — a re-apply that had given its only reference away would be putting back freed values.
   THE RESTORE IS A SLOT WRITE AND NOT THE OPERATION, which is the same distinction JS_SetOwnSlotDesc draws:
   10.4.7.1's immutable-prototype refusal, 10.1.2's extensible test and the cycle check all belong to
   [[SetPrototypeOf]], and every one of them would REFUSE inside a context switch that has no flow to throw on —
   the flow whose baseline is being put back is very often the flow that froze the object. */
typedef struct JSObjState {
    uint8_t extensible;
    uint8_t has_data;      /* the class keeps an internal slot in u.object_data — never inferred from the value */
    JSObject *proto;       /* owned (NULL is a real prototype: Object.create(null)) */
    JSValue object_data;   /* owned; JS_UNDEFINED and unread when !has_data */
} JSObjState;

void *JS_ObjStateSave(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    JSObjState *st;

    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT, "an object-state capture on something that is not an object");
    p = JS_VALUE_GET_OBJ(obj);
    DCHECK(p->class_id != JS_CLASS_PROXY,
           "a COW delta is capturing a Proxy's own state: its prototype and its extensibility are TRAPS, so "
           "reading either here would run the page's code mid-context-switch with no flow base");
    st = js_malloc(ctx, sizeof(*st));
    if (!st)
        return NULL;
    st->extensible = p->extensible;
    st->proto = p->shape->proto;
    if (st->proto)
        js_dup(JS_MKPTR(JS_TAG_OBJECT, st->proto));
    st->has_data = js_class_has_object_data(p->class_id);
    st->object_data = st->has_data ? js_dup(p->u.object_data) : JS_UNDEFINED;
    return st;
}

void JS_ObjStateRestore(JSContext *ctx, JSValueConst obj, void *blob)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    const JSObjState *st = blob;

    DCHECK(st != NULL, "an object's own state was re-applied before any unapply had recorded one — the context "
                       "switch that parked this flow did not run");
    DCHECK(st->has_data == js_class_has_object_data(p->class_id),
           "an object's captured state disagrees with its class about whether it keeps an internal slot — a "
           "JSObject does not change class, so the entry was read off a different object than it names");
    p->extensible = st->extensible;
    if (p->shape->proto != st->proto) {
        JSShape *sh;
        if (js_shape_prepare_update(ctx, p, NULL))
            CHECK_FAIL("a COW object-state restore could not reshape an object to put its PROTOTYPE back: a "
                       "dropped baseline prototype leaves one flow's __proto__ write standing for every sibling");
        sh = p->shape;
        if (st->proto)
            js_dup(JS_MKPTR(JS_TAG_OBJECT, st->proto));
        if (sh->proto)
            JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, sh->proto));
        sh->proto = st->proto;
        if (st->proto)
            st->proto->is_prototype = true;
    }
    if (st->has_data)
        set_value(ctx, &p->u.object_data, js_dup(st->object_data));
}

void JS_ObjStateFree(JSRuntime *rt, void *blob)
{
    JSObjState *st = blob;

    if (!st)
        return;
    if (st->proto)
        JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, st->proto));
    JS_FreeValueRT(rt, st->object_data);
    js_free_rt(rt, st);
}

/* AN ITERATION RECORD'S STATE — the cursor that says how far through a walk a shared iterator object is, plus
 * whatever else that walk writes into the class opaque. See JSTimeTravelHooks.iter_state for why a flow-private
 * looking object needs this at all (a snapshot fork dups the operand stack, so a JSValue is a reference and both
 * arms walk ONE record).
 *
 * THE UNION IS PER CLASS AND THE CLASS IS STORED, because the record behind `p->u` is a different struct for
 * every iterator kind and the restore has nothing else to decide by. It is re-derived from the object and
 * compared, exactly as JSObjState's `has_data` is and for the same reason: a JSObject does not change class, so
 * a blob whose class disagrees was read off a different object than it names.
 *
 * WHAT EACH ARM OWNS DIFFERS AND THE BLOB IS THE ENGINE'S EITHER WAY, because the reason that covers all of
 * them is that THE RECORD IS THE ENGINE'S: every one of these structs is private to this file, so there is no
 * caller who could hand cow_capture_host_record an offset list for one. The for-in arm has the sharper form of
 * the same argument, and it is the one that would still hold if the layouts were public — what it owns is an
 * ATOM, a counted reference no offset list of JSValues can name, so a byte copy would take a reference it never
 * counted. The array/string arm owns a JSValue, which such a list COULD have named; for that one the private
 * struct is the whole reason, which is why it is stated first.
 * EVERY OWNED HALF IS HELD FOR THE LIFE OF THE BLOB — the atom here, the source value there — which is what
 * lets the same blob restore any number of times rather than giving its only reference away on the first. */
typedef struct JSIterState {
    JSClassID class_id;
    union {
        struct {                   /* JS_CLASS_FOR_IN_ITERATOR */
            /* RESTORED — the mutable half, and the whole of it: js_for_in_candidate advances `idx` and the
               opcode's deletion check parks its candidate in `pending`. */
            uint32_t idx;
            JSAtom   pending;      /* OWNED */
            /* ASSERTED, never restored — the immutable half, written once by the construction in js_for_in_step
               and never again. Putting these back would be restoring a fact rather than a write; holding them
               is what makes a future write to any of them fail loudly at the restore instead of being dropped
               silently by every context switch. `obj_id` is an IDENTITY and not a reference: it is
               JS_VALUE_GET_PTR of a value the iterator owns and the delta entry keeps alive, it is compared and
               never dereferenced, and there is nothing in it to dup or free. */
            void    *obj_id;
            uint32_t array_length;
            uint8_t  is_array;
        } for_in;
        struct {                   /* JS_CLASS_ARRAY_ITERATOR and JS_CLASS_STRING_ITERATOR — ONE record behind
                                      two classes (JSArrayIteratorData), so one arm serves both */
            /* RESTORED — the mutable half, and BOTH of its fields are written by the same .next(): the cursor
               advances, and the walk that runs out RELEASES THE SOURCE (`JS_FreeValue(ctx, it->obj); it->obj =
               JS_UNDEFINED`, in js_array_iter_next_step and js_string_iterator_next alike).
               THAT RELEASE IS WHY `obj` IS A COUNTED REFERENCE HERE AND for_in's IS AN IDENTITY. There the
               receiver is written once and the record holds it for the iterator's whole life, so the entry
               keeping the iterator alive keeps the receiver alive and a bare pointer is enough to compare. Here
               the record DROPS its reference the moment one arm exhausts the walk — a sibling's blob holding a
               bare pointer would then be naming freed storage, and restoring it would install a dangling value
               into a live record. So the blob holds its own, for the same reason for_in's atom is dup'd: one
               entry is restored on every switch for the life of the flow. */
            JSValue obj;           /* OWNED */
            int64_t idx;
            /* ASSERTED, never restored — the immutable half. `kind` is written by the two constructions
               (js_create_array_iterator and js_string_iterator_create_step) and by nothing else, so putting it
               back would be restoring a fact rather than a write; holding it is what makes a future write to it
               fail loudly at the restore instead of being dropped by every context switch. */
            JSIteratorKindEnum kind;
        } array_iter;
    } u;
} JSIterState;

void *JS_IterStateSave(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    JSIterState *st;

    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT, "an iteration-state capture on something that is not an object");
    p = JS_VALUE_GET_OBJ(obj);
    /* zeroed, so the release arm of the DFAIL below hands back a COHERENT blob rather than an uninitialised
       union: JS_ATOM_NULL is 0, which is a valid unowned atom, and Restore's own default then does nothing —
       leaving exactly the behaviour this hook did not yet have rather than a restore of garbage. */
    st = js_mallocz(ctx, sizeof(*st));
    if (!st)
        return NULL;
    st->class_id = p->class_id;
    switch (p->class_id) {
    case JS_CLASS_FOR_IN_ITERATOR: {
        const JSForInIterator *it = p->u.for_in_iterator;
        st->u.for_in.idx = it->idx;
        st->u.for_in.pending = JS_DupAtom(ctx, it->pending);
        st->u.for_in.obj_id = JS_VALUE_GET_PTR(it->obj);
        st->u.for_in.array_length = it->array_length;
        st->u.for_in.is_array = it->is_array;
        break;
    }
    case JS_CLASS_ARRAY_ITERATOR:
    case JS_CLASS_STRING_ITERATOR: {
        const JSArrayIteratorData *it = p->u.array_iterator_data;
        st->u.array_iter.obj = js_dup(it->obj);
        st->u.array_iter.idx = it->idx;
        st->u.array_iter.kind = it->kind;
        break;
    }
    default:
        DFAIL("an iteration record reached the time-travel capture with no arm here — this class's opaque holds "
              "a cursor now, so build its arm in JS_IterStateSave, JS_IterStateRestore and JS_IterStateFree "
              "together, holding a counted reference on anything its walk owns");
        break;
    }
    return st;
}

void JS_IterStateRestore(JSContext *ctx, JSValueConst obj, void *blob)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    const JSIterState *st = blob;

    DCHECK(st != NULL, "an iterator's state was re-applied before any unapply had recorded one — the context "
                       "switch that parked this flow did not run");
    DCHECK(st->class_id == p->class_id,
           "an iteration record's captured state disagrees with its object about its CLASS — a JSObject does "
           "not change class, so the entry was read off a different object than it names");
    switch (p->class_id) {
    case JS_CLASS_FOR_IN_ITERATOR: {
        JSForInIterator *it = p->u.for_in_iterator;
        DCHECK(JS_VALUE_GET_PTR(it->obj) == st->u.for_in.obj_id
               && it->array_length == st->u.for_in.array_length
               && it->is_array == (bool)st->u.for_in.is_array,
               "a for-in iterator's IMMUTABLE half moved under a COW swap — its receiver, its array length or "
               "its array-ness is written after construction now, so this blob saves a field the swap then "
               "drops on every context switch; restore it here as well as assert it");
        it->idx = st->u.for_in.idx;
        /* the record's own reference goes, the blob's is DUP'd in: the blob keeps its own, which is what lets
           one entry restore on every switch for the life of the flow. */
        JS_FreeAtom(ctx, it->pending);
        it->pending = JS_DupAtom(ctx, st->u.for_in.pending);
        break;
    }
    case JS_CLASS_ARRAY_ITERATOR:
    case JS_CLASS_STRING_ITERATOR: {
        JSArrayIteratorData *it = p->u.array_iterator_data;
        DCHECK(it->kind == st->u.array_iter.kind,
               "an array or string iterator's IMMUTABLE half moved under a COW swap — its `kind` is written "
               "after construction now, so this blob saves a field the swap then drops on every context "
               "switch; restore it here as well as assert it");
        it->idx = st->u.array_iter.idx;
        /* the record's own reference goes and the blob's is DUP'd in — the blob keeps its own, which is what
           lets one entry restore on every switch. An EXHAUSTED walk's saved `obj` is JS_UNDEFINED, and both
           halves of that are free. */
        set_value(ctx, &it->obj, js_dup(st->u.array_iter.obj));
        break;
    }
    default:
        DFAIL("an iteration record's state is being restored for a class JS_IterStateSave has no arm for — the "
              "save accepted this class, so the two switches have drifted apart");
        break;
    }
}

void JS_IterStateFree(JSRuntime *rt, void *blob)
{
    JSIterState *st = blob;

    if (!st)
        return;
    switch (st->class_id) {
    case JS_CLASS_FOR_IN_ITERATOR:
        JS_FreeAtomRT(rt, st->u.for_in.pending);
        break;
    case JS_CLASS_ARRAY_ITERATOR:
    case JS_CLASS_STRING_ITERATOR:
        JS_FreeValueRT(rt, st->u.array_iter.obj);
        break;
    default:
        DFAIL("an iteration record's state is being freed for a class JS_IterStateSave has no arm for — the "
              "save accepted this class, so anything its arm dup'd is leaking here");
        break;
    }
    js_free_rt(rt, st);
}

/* return -1 (exception) or true/false */
int JS_SetPrototype(JSContext *ctx, JSValueConst obj, JSValueConst proto_val)
{
    return JS_SetPrototypeInternal(ctx, obj, proto_val, true);
}

/* Only works for primitive types, otherwise return JS_NULL. */
static JSValueConst JS_GetPrototypePrimitive(JSContext *ctx, JSValueConst val)
{
    JSValue ret;
    switch(JS_VALUE_GET_NORM_TAG(val)) {
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        ret = ctx->class_proto[JS_CLASS_BIG_INT];
        break;
    case JS_TAG_INT:
    case JS_TAG_FLOAT64:
        ret = ctx->class_proto[JS_CLASS_NUMBER];
        break;
    case JS_TAG_BOOL:
        ret = ctx->class_proto[JS_CLASS_BOOLEAN];
        break;
    case JS_TAG_STRING:
    case JS_TAG_STRING_ROPE:
        ret = ctx->class_proto[JS_CLASS_STRING];
        break;
    case JS_TAG_SYMBOL:
        ret = ctx->class_proto[JS_CLASS_SYMBOL];
        break;
    case JS_TAG_OBJECT:
    case JS_TAG_NULL:
    case JS_TAG_UNDEFINED:
    default:
        ret = JS_NULL;
        break;
    }
    return ret;
}

/* Return an Object, JS_NULL or JS_EXCEPTION in case of Proxy object.
   THIS IS [[GetPrototypeOf]] AND NOT "the object's prototype link", and the difference is the whole reason the
   exotic hook is consulted here: a class may COMPUTE its answer (JSClassExoticMethods.get_prototype), in which
   case the shape's link is not what the internal method returns and for HTML §7.2.3's WindowProxy is not even
   an object of the right document.
   The ordinary property walks ask the same hook through js_walk_proto for a class that declares
   get_prototype_walkable, so such a class is answered one way by every route. */
JSValue JS_GetPrototypeSlot(JSContext *ctx, JSValueConst obj)
{
    JSObject *proto;

    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT, "a prototype slot was asked of a value that is not an object");
    proto = JS_VALUE_GET_OBJ(obj)->shape->proto;
    return proto ? js_dup(JS_MKPTR(JS_TAG_OBJECT, proto)) : JS_NULL;
}

/* Web IDL §3.9.7 named property visibility algorithm step 4's walk: whether an object of `obj`'s prototype chain,
   which is not `obj`, has an own property `atom`. 1 or 0 when every link answers [[GetOwnProperty]] and
   [[GetPrototypeOf]] from its own slots, so asking before step 1 is unobservable; -1 at the first link that does
   not (a Proxy, a class with its own [[GetOwnProperty]], a computed prototype a walk may not take). Runs no page
   code. */
int JS_PrototypeChainHasOwnProperty(JSContext *ctx, JSValueConst obj, JSAtom atom)
{
    JSObject *p;

    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT,
           "a named property's visibility was asked of a value that is not an object");
    for (p = JS_VALUE_GET_OBJ(obj);;) {
        const JSClassExoticMethods *gp = js_exotic_getproto(ctx->rt, p);

        if (gp && !gp->get_prototype_walkable) return -1;
        p = js_walk_proto(ctx, p);
        if (!p) return 0;
        if (p->class_id == JS_CLASS_PROXY) return -1;   /* before is_exotic: a proxy IS exotic */
        if (p->is_exotic) {
            const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;

            if (em && (em->get_own_property || em->get_own_property_step)) return -1;
        }
        if (find_own_property1(p, atom)) return 1;
    }
}

JSValue JS_GetPrototype(JSContext *ctx, JSValueConst obj)
{
    JSValue val;
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT) {
        JSObject *p;
        const JSClassExoticMethods *em;
        p = JS_VALUE_GET_OBJ(obj);
        if (unlikely(p->class_id == JS_CLASS_PROXY)) {
            val = js_proxy_getPrototypeOf(ctx, obj);
        } else if (unlikely((em = js_exotic_getproto(ctx->rt, p)) != NULL)) {
            val = em->get_prototype(ctx, obj);
            DCHECK(JS_IsException(val) || JS_IsNull(val) || JS_IsObject(val),
                   "a class's [[GetPrototypeOf]] answered something that is neither an Object nor null — "
                   "10.1.1's [[Prototype]] is one or the other, and every consumer here either walks the "
                   "value as a chain link or hands it to a page as one");
        } else {
            p = p->shape->proto;
            if (!p)
                val = JS_NULL;
            else
                val = js_dup(JS_MKPTR(JS_TAG_OBJECT, p));
        }
    } else {
        val = js_dup(JS_GetPrototypePrimitive(ctx, obj));
    }
    return val;
}

static JSValue JS_GetPrototypeFree(JSContext *ctx, JSValue obj)
{
    JSValue obj1;
    obj1 = JS_GetPrototype(ctx, obj);
    JS_FreeValue(ctx, obj);
    return obj1;
}

