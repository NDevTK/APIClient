static uint32_t js_string_obj_get_length(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    JSString *p1;
    uint32_t len = 0;

    /* This is a class exotic method: obj class_id is JS_CLASS_STRING */
    p = JS_VALUE_GET_OBJ(obj);
    if (JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_STRING) {
        p1 = JS_VALUE_GET_STRING(p->u.object_data);
        len = p1->len;
    }
    return len;
}

static int num_keys_cmp(const void *p1, const void *p2, void *opaque)
{
    JSContext *ctx = opaque;
    JSAtom atom1 = ((const JSPropertyEnum *)p1)->atom;
    JSAtom atom2 = ((const JSPropertyEnum *)p2)->atom;
    uint32_t v1, v2;
    bool atom1_is_integer, atom2_is_integer;

    atom1_is_integer = JS_AtomIsArrayIndex(ctx, &v1, atom1);
    atom2_is_integer = JS_AtomIsArrayIndex(ctx, &v2, atom2);
    DCHECK(atom1_is_integer && atom2_is_integer, "atom1_is_integer && atom2_is_integer");
    if (v1 < v2)
        return -1;
    else if (v1 == v2)
        return 0;
    else
        return 1;
}

static void js_free_prop_enum(JSContext *ctx, JSPropertyEnum *tab, uint32_t len)
{
    uint32_t i;
    if (tab) {
        for(i = 0; i < len; i++)
            JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);
    }
}

/* return < 0 in case if exception, 0 if OK. ptab and its atoms must
   be freed by the user. */
static int __exception JS_GetOwnPropertyNamesInternal(JSContext *ctx,
                                                      JSPropertyEnum **ptab,
                                                      uint32_t *plen,
                                                      JSObject *p, int flags)
{
    int i, j;
    JSShape *sh;
    JSShapeProperty *prs;
    JSPropertyEnum *tab_atom, *tab_exotic;
    JSAtom atom;
    uint32_t num_keys_count, str_keys_count, sym_keys_count, atom_count;
    uint32_t num_index, str_index, sym_index, exotic_count, exotic_keys_count;
    bool is_enumerable, num_sorted;
    uint32_t num_key;
    JSAtomKindEnum kind;

    /* clear pointer for consistency in case of failure */
    *ptab = NULL;
    *plen = 0;

    /* compute the number of returned properties */
    num_keys_count = 0;
    str_keys_count = 0;
    sym_keys_count = 0;
    exotic_keys_count = 0;
    exotic_count = 0;
    tab_exotic = NULL;
    sh = p->shape;
    for(i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
        atom = prs->atom;
        if (atom != JS_ATOM_NULL) {
            is_enumerable = ((prs->flags & JS_PROP_ENUMERABLE) != 0);
            kind = JS_AtomGetKind(ctx, atom);
            if ((!(flags & JS_GPN_ENUM_ONLY) || is_enumerable) &&
                ((flags >> kind) & 1) != 0) {
                /* need to raise an exception in case of the module
                   name space (implicit GetOwnProperty) */
                if (unlikely((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) &&
                    (flags & (JS_GPN_SET_ENUM | JS_GPN_ENUM_ONLY))) {
                    JSVarRef *var_ref = p->prop[i].u.var_ref;
                    if (unlikely(JS_IsUninitialized(*var_ref->pvalue))) {
                        JS_ThrowReferenceErrorUninitialized(ctx, prs->atom);
                        return -1;
                    }
                }
                if (JS_AtomIsArrayIndex(ctx, &num_key, atom)) {
                    num_keys_count++;
                } else if (kind == JS_ATOM_KIND_STRING) {
                    str_keys_count++;
                } else {
                    sym_keys_count++;
                }
            }
        }
    }

    if (p->is_exotic) {
        if (p->fast_array) {
            if (flags & JS_GPN_STRING_MASK) {
                num_keys_count += p->u.array.count;
            }
        } else if (p->class_id == JS_CLASS_STRING) {
            if (flags & JS_GPN_STRING_MASK) {
                num_keys_count += js_string_obj_get_length(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
            }
        } else {
            const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
            if (em && em->get_own_property_names && !(flags & JS_GPN_NO_EXOTIC)) {
                if (em->get_own_property_names(ctx, &tab_exotic, &exotic_count,
                                               JS_MKPTR(JS_TAG_OBJECT, p)))
                    return -1;
                for(i = 0; i < exotic_count; i++) {
                    atom = tab_exotic[i].atom;
                    kind = JS_AtomGetKind(ctx, atom);
                    if (((flags >> kind) & 1) != 0) {
                        is_enumerable = false;
                        if (flags & (JS_GPN_SET_ENUM | JS_GPN_ENUM_ONLY)) {
                            int desc_flags, res;
                            /* set the "is_enumerable" field if necessary */
                            res = JS_GetOwnPropertyFlagsInternal(ctx, &desc_flags, p, atom);
                            if (res < 0) {
                                js_free_prop_enum(ctx, tab_exotic, exotic_count);
                                return -1;
                            }
                            if (res) {
                                is_enumerable =
                                    ((desc_flags & JS_PROP_ENUMERABLE) != 0);
                            }
                            tab_exotic[i].is_enumerable = is_enumerable;
                        }
                        if (!(flags & JS_GPN_ENUM_ONLY) || is_enumerable) {
                            exotic_keys_count++;
                        }
                    }
                }
            }
        }
    }

    /* fill them */

    atom_count = num_keys_count + str_keys_count + sym_keys_count + exotic_keys_count;
    /* avoid allocating 0 bytes */
    tab_atom = js_malloc(ctx, sizeof(tab_atom[0]) * max_int(atom_count, 1));
    if (!tab_atom) {
        js_free_prop_enum(ctx, tab_exotic, exotic_count);
        return -1;
    }

    num_index = 0;
    str_index = num_keys_count;
    sym_index = str_index + str_keys_count;

    num_sorted = true;
    sh = p->shape;
    for(i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
        atom = prs->atom;
        if (atom != JS_ATOM_NULL) {
            is_enumerable = ((prs->flags & JS_PROP_ENUMERABLE) != 0);
            kind = JS_AtomGetKind(ctx, atom);
            if ((!(flags & JS_GPN_ENUM_ONLY) || is_enumerable) &&
                ((flags >> kind) & 1) != 0) {
                if (JS_AtomIsArrayIndex(ctx, &num_key, atom)) {
                    j = num_index++;
                    num_sorted = false;
                } else if (kind == JS_ATOM_KIND_STRING) {
                    j = str_index++;
                } else {
                    j = sym_index++;
                }
                tab_atom[j].atom = JS_DupAtom(ctx, atom);
                tab_atom[j].is_enumerable = is_enumerable;
            }
        }
    }

    if (p->is_exotic) {
        int len;
        if (p->fast_array) {
            if (flags & JS_GPN_STRING_MASK) {
                len = p->u.array.count;
                goto add_array_keys;
            }
        } else if (p->class_id == JS_CLASS_STRING) {
            if (flags & JS_GPN_STRING_MASK) {
                len = js_string_obj_get_length(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
            add_array_keys:
                for(i = 0; i < len; i++) {
                    tab_atom[num_index].atom = __JS_AtomFromUInt32(i);
                    if (tab_atom[num_index].atom == JS_ATOM_NULL) {
                        js_free_prop_enum(ctx, tab_atom, num_index);
                        return -1;
                    }
                    tab_atom[num_index].is_enumerable = true;
                    num_index++;
                }
            }
        } else {
            /* Note: exotic keys are not reordered and comes after the object own properties. */
            for(i = 0; i < exotic_count; i++) {
                atom = tab_exotic[i].atom;
                is_enumerable = tab_exotic[i].is_enumerable;
                kind = JS_AtomGetKind(ctx, atom);
                if ((!(flags & JS_GPN_ENUM_ONLY) || is_enumerable) &&
                    ((flags >> kind) & 1) != 0) {
                    tab_atom[sym_index].atom = atom;
                    tab_atom[sym_index].is_enumerable = is_enumerable;
                    sym_index++;
                } else {
                    JS_FreeAtom(ctx, atom);
                }
            }
            js_free(ctx, tab_exotic);
        }
    }

    DCHECK(num_index == num_keys_count, "num_index == num_keys_count");
    DCHECK(str_index == num_keys_count + str_keys_count, "str_index == num_keys_count + str_keys_count");
    DCHECK(sym_index == atom_count, "sym_index == atom_count");

    if (num_keys_count != 0 && !num_sorted) {
        rqsort(tab_atom, num_keys_count, sizeof(tab_atom[0]), num_keys_cmp,
               ctx);
    }
    *ptab = tab_atom;
    *plen = atom_count;
    return 0;
}

/* `keys`, a class's [[OwnPropertyKeys]] step's Array (CONSUMED), with `obj`'s ordinary own String and Symbol keys
   appended after it: the order a legacy platform object's internal method lists them in (Web IDL §3.9.6
   [[OwnPropertyKeys]], whose class keys come first). Owned; JS_EXCEPTION on failure. */
static int JS_CreateDataPropertyUint32(JSContext *ctx, JSValueConst this_obj,
                                       int64_t idx, JSValue val, int flags);
static JSValue js_ownkeys_append_ordinary(JSContext *ctx, JSValueConst obj, JSValue keys)
{
    JSPropertyEnum *tab = NULL;
    uint32_t len = 0, i, at;

    DCHECK(JS_IsArray(keys), "a class's [[OwnPropertyKeys]] step answered something other than an Array");
    if (js_get_length32(ctx, &at, keys) ||
        JS_GetOwnPropertyNamesInternal(ctx, &tab, &len, JS_VALUE_GET_OBJ(obj),
                                       JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_NO_EXOTIC)) {
        JS_FreeValue(ctx, keys);
        return JS_EXCEPTION;
    }
    for (i = 0; i < len; i++) {
        JSValue kv = JS_AtomToValue(ctx, tab[i].atom);
        if (JS_IsException(kv) || JS_CreateDataPropertyUint32(ctx, keys, at + i, kv, 0) < 0) {
            js_free_prop_enum(ctx, tab, len);
            JS_FreeValue(ctx, keys);
            return JS_EXCEPTION;
        }
    }
    js_free_prop_enum(ctx, tab, len);
    return keys;
}

int JS_GetOwnPropertyNames(JSContext *ctx, JSPropertyEnum **ptab,
                           uint32_t *plen, JSValueConst obj, int flags)
{
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
        JS_ThrowTypeErrorNotAnObject(ctx);
        return -1;
    }
    return JS_GetOwnPropertyNamesInternal(ctx, ptab, plen,
                                          JS_VALUE_GET_OBJ(obj), flags);
}

/* Return -1 if exception,
   false if the property does not exist, true if it exists. If true is
   returned, the property descriptor 'desc' is filled present. */
static int JS_GetOwnPropertyInternal2(JSContext *ctx, JSPropertyDescriptor *desc,
                                      JSObject *p, JSAtom prop, int *pflags, bool as_slot)
{
    JSShapeProperty *prs;
    JSProperty *pr;
    int flags_only = (desc == NULL && pflags != NULL);

retry:
    prs = find_own_property(&pr, p, prop);
    if (prs) {
        if (desc) {
            desc->flags = prs->flags & JS_PROP_C_W_E;
            desc->getter = JS_UNDEFINED;
            desc->setter = JS_UNDEFINED;
            desc->value = JS_UNDEFINED;
            if (unlikely(prs->flags & JS_PROP_TMASK)) {
                if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
                    desc->flags |= JS_PROP_GETSET;
                    if (pr->u.getset.getter)
                        desc->getter = js_dup(JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter));
                    if (pr->u.getset.setter)
                        desc->setter = js_dup(JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.setter));
                } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
                    JSValue val = *pr->u.var_ref->pvalue;
                    /* THE TDZ CHECK IS THE OPERATION'S, NOT THE SLOT'S — the same distinction `as_slot` draws
                       at delete_property and at the length write. 9.4.2's ReferenceError belongs to
                       [[GetOwnProperty]] on a module namespace whose export has not been initialised; the COW
                       capture is asking what the STORAGE holds, and uninitialised is a state that storage can
                       be in and that the slot write puts straight back. Throwing here answered the capture
                       with -1, which it read as "the slot is absent" while leaving a live ReferenceError
                       belonging to no flow — reachable by deleting a module-namespace export in TDZ. */
                    if (unlikely(JS_IsUninitialized(val)) && !as_slot) {
                        JS_ThrowReferenceErrorUninitialized(ctx, prs->atom);
                        return -1;
                    }
                    desc->value = js_dup(val);
                } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
                    /* Instantiate property and retry */
                    if (JS_AutoInitProperty(ctx, p, prop, pr, prs))
                        return -1;
                    goto retry;
                }
            } else {
                desc->value = js_dup(pr->u.value);
            }
        } else {
            if (pflags) {
                *pflags = prs->flags & JS_PROP_C_W_E;
                if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET)
                    *pflags |= JS_PROP_GETSET;
            }
            /* for consistency, send the exception even if desc is NULL */
            if (unlikely((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF)) {
                if (unlikely(JS_IsUninitialized(*pr->u.var_ref->pvalue)) && !as_slot) {
                    JS_ThrowReferenceErrorUninitialized(ctx, prs->atom);
                    return -1;
                }
            } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
                /* nothing to do: delay instantiation until actual value and/or attributes are read */
            }
        }
        return true;
    }
    if (p->is_exotic) {
        if (p->fast_array) {
            /* specific case for fast arrays */
            if (__JS_AtomIsTaggedInt(prop)) {
                uint32_t idx;
                idx = __JS_AtomToUInt32(prop);
                if (idx < p->u.array.count) {
                    if (desc) {
                        desc->flags = JS_PROP_WRITABLE | JS_PROP_ENUMERABLE |
                            JS_PROP_CONFIGURABLE;
                        desc->getter = JS_UNDEFINED;
                        desc->setter = JS_UNDEFINED;
                        /* an element of a fast array in range is a slot, so it is read as one, not through a
                           second [[Get]] that would find the same slot */
                        bool got = js_get_fast_array_element(ctx, p, idx, &desc->value);
                        DCHECK(got, "a fast array's in-range element has no slot to read");
                        (void)got;
                    } else if (flags_only) {
                        *pflags = JS_PROP_WRITABLE | JS_PROP_ENUMERABLE |
                            JS_PROP_CONFIGURABLE;
                    }
                    return true;
                }
            }
        } else if (!as_slot) {
            /* A class's [[GetOwnProperty]] hook composes its answer at the query (a named or indexed property,
               a string's index, an example's member) over storage that is not this object's shape, so a slot
               read never asks it: a composed descriptor recorded as a slot would be written back as a real
               own property, shadowing the hook. delete_property's `as_slot` draws the same line. */
            const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
            if (em && em->get_own_property) {
                if (flags_only) {
                    /* Call the exotic handler with a temporary desc,
                       extract flags, and free the values. For Proxy
                       objects, the JS trap runs regardless, so the
                       dup+free overhead is negligible. */
                    JSPropertyDescriptor d;
                    int ret = em->get_own_property(ctx, &d,
                                                   JS_MKPTR(JS_TAG_OBJECT, p), prop);
                    if (ret > 0) {
                        *pflags = d.flags;
                        js_free_desc(ctx, &d);
                    }
                    return ret;
                }
                return em->get_own_property(ctx, desc,
                                            JS_MKPTR(JS_TAG_OBJECT, p), prop);
            }
        }
    }
    return false;
}

static int JS_GetOwnPropertyInternal(JSContext *ctx, JSPropertyDescriptor *desc,
                                     JSObject *p, JSAtom prop)
{
    return JS_GetOwnPropertyInternal2(ctx, desc, p, prop, NULL, /*as_slot*/false);
}

/* Same as JS_GetOwnPropertyInternal but only returns flags, not
   value/getter/setter. Avoids unnecessary js_dup() for callers that
   only need the property flags. */
static int JS_GetOwnPropertyFlagsInternal(JSContext *ctx, int *pflags,
                                          JSObject *p, JSAtom prop)
{
    return JS_GetOwnPropertyInternal2(ctx, NULL, p, prop, pflags, /*as_slot*/false);
}

/* The internal method itself, for a class that STANDS IN for another object — see quickjs.h. The assertion is
   the whole of what it adds over the internal entry: a class that stands in performs the operation on a target
   it did not choose, so "does this run the page's code" is a question about the TARGET and has to be asked
   where the target arrives, not where the stand-in was written. */
int JS_GetOwnPropertyNoUserCode(JSContext *ctx, JSPropertyDescriptor *desc,
                                JSValueConst obj, JSAtom prop)
{
    JSObject *p;
    const JSClassExoticMethods *em;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
        JS_ThrowTypeErrorNotAnObject(ctx);
        return -1;
    }
    p = JS_VALUE_GET_OBJ(obj);
    em = p->is_exotic ? ctx->rt->class_array[p->class_id].exotic : NULL;
    DCHECK(p->class_id != JS_CLASS_PROXY,
           "a C-side [[GetOwnProperty]] was forwarded onto a Proxy — its own-property query IS the page's "
           "`getOwnPropertyDescriptor` trap, and there is no flow base here to run one on; route that caller "
           "onto the keyed entry's GP_GETOWNPROP");
    DCHECK(!em || !em->get_own_property || em->get_own_property_no_user_code,
           "a C-side [[GetOwnProperty]] was forwarded onto a class that has not declared its hook free of the "
           "page's code — declare get_own_property_no_user_code where that hook is defined, or route the "
           "caller onto the keyed entry's GP_GETOWNPROP");
    return JS_GetOwnPropertyInternal(ctx, desc, p, prop);
}

/* The COW delta's own-property read: a SLOT's whole state, never an operation.
   A delta is captured and swapped by the SCHEDULER — cow_capture runs inside a write hook, and unapply/apply run
   between two flows — so anything here that could reach the page's code would run it with no flow base and in
   the middle of a context switch. A PROXY could: its every own-property query is a trap, and that stays a refusal.
   AN ACCESSOR NO LONGER DOES, and that is the whole of this change. It used to abort here, because what an entry
   held was a VALUE: §6.2.6 makes a property descriptor one record with two shapes — {[[Value]],[[Writable]]}
   XOR {[[Get]],[[Set]]}, both carrying [[Enumerable]] and [[Configurable]] — and half a record cannot say which
   shape a slot is in, so an accessor could only be refused and an attribute change could not be recorded at all.
   Reading the descriptor is what closes both, and it runs no page code: the getter and setter are handed over as
   FUNCTION OBJECTS, exactly as [[GetOwnProperty]] hands them to `Object.getOwnPropertyDescriptor`, and neither
   is called here or at the restore.
   1 = own property (*pd owned: flags, and value XOR getter/setter), 0 = absent, -1 = threw. */
bool JS_ClassComposesOwnProperty(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;
    const JSClassExoticMethods *em;

    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) return false;
    p = JS_VALUE_GET_OBJ(obj);
    if (!p->is_exotic || p->fast_array || p->class_id == JS_CLASS_PROXY) return false;
    em = ctx->rt->class_array[p->class_id].exotic;
    return em && em->get_own_property;
}

int JS_GetOwnSlotDesc(JSContext *ctx, JSPropertyDescriptor *pd, JSValueConst obj, JSAtom prop)
{
    JSObject *p;
    int has;

    pd->flags = 0;
    pd->value = JS_UNDEFINED;
    pd->getter = JS_UNDEFINED;
    pd->setter = JS_UNDEFINED;
    if (JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT) {
        JS_ThrowTypeErrorNotAnObject(ctx);
        return -1;
    }
    p = JS_VALUE_GET_OBJ(obj);
    /* A stand-in's slot is the object it stands in for (JSClassExoticMethods.forwarded_object), the one its
       writes land on: HTML §7.2.3's WindowProxy holds no storage of its own for a Window's member. */
    if (p->is_exotic && !p->fast_array) {
        const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
        if (em && em->forwarded_object) {
            JSValueConst fwd = em->forwarded_object(ctx, obj, prop);
            if (!JS_IsUninitialized(fwd)) {
                DCHECK(JS_VALUE_GET_TAG(fwd) == JS_TAG_OBJECT,
                       "a stand-in forwarded a slot read to something that is not an object");
                p = JS_VALUE_GET_OBJ(fwd);
            }
        }
    }
    DCHECK(p->class_id != JS_CLASS_PROXY,
           "a COW delta holds a Proxy: capturing or swapping it would run the page's traps mid-context-switch");
    /* The read twin refuses what the write twin refuses, and this one fires at the CAPTURE rather than at the
       swap — cow_capture_hook reads through here before it appends the entry, so a typed-array index routed to
       cow_capture instead of cow_capture_bytes aborts where it was written down. See JS_SetOwnSlotDesc. */
    DCHECK(!(is_typed_array(p->class_id) && __JS_AtomIsTaggedInt(prop)),
           "a COW delta is capturing a typed-array element as a property SLOT: its storage is the buffer's "
           "BYTES, which no JSValue round-trips (a NaN payload normalises), so route this capture through "
           "cow_capture_bytes");
    has = JS_GetOwnPropertyInternal2(ctx, pd, p, prop, NULL, /*as_slot*/true);
    /* WHAT IS LEFT OF -1 IS AN ALLOCATION, and the caller has nowhere to put one. The non-object case returned
       above; the Proxy and typed-array cases are refused above; and the TDZ ReferenceError is the OPERATION's,
       which `as_slot` does not ask for. So a failure here is JS_AutoInitProperty's or a string index's, and
       both are OOM — which the write twin already treats as fatal for the same reason: a capture that does not
       happen leaks one flow's write into every sibling, and there is no flow here to throw on. */
    CHECK(has >= 0, "a COW slot read could not answer what a slot holds — the only remaining failure is an "
                    "allocation, and a lost baseline read leaks one flow's state into every sibling");
    if (has == 0)
        return 0;
    DCHECK(!(pd->flags & JS_PROP_GETSET) ||
           ((JS_IsObject(pd->getter) || JS_IsUndefined(pd->getter)) &&
            (JS_IsObject(pd->setter) || JS_IsUndefined(pd->setter))),
           "an accessor slot answered with a half that is neither a function object nor absent — the write twin "
           "installs those two halves as shape storage and has nothing else to put there");
    return 1;
}

/* THE VALUE PROJECTION of the read above, for the ~90 component readers that reach a slot THEY created — an
   idl_slots record, a listener map, an IndexedDB key path's next step. They want the stored value and nothing
   else, and for them an ACCESSOR is still a refusal, but for a different reason than the COW round trip's used
   to be: a component's own internal slot is one nothing else can define, so an accessor there means the read
   walked onto an object that is not the one it thought it had. That is a statement about the CALLER, which is
   why it lives on this side of the projection and not inside the descriptor read.
   1 = own data property (*pval owned), 0 = absent, -1 = threw. */
int JS_GetOwnSlot(JSContext *ctx, JSValue *pval, JSValueConst obj, JSAtom prop)
{
    JSPropertyDescriptor pd;
    int has;

    *pval = JS_UNDEFINED;
    has = JS_GetOwnSlotDesc(ctx, &pd, obj, prop);
    if (has <= 0)
        return has;
    if (pd.flags & JS_PROP_GETSET) {
        JS_FreeValue(ctx, pd.getter);
        JS_FreeValue(ctx, pd.setter);
        DFAIL("an own-slot VALUE read landed on an ACCESSOR: the slot this component created holds a stored "
              "value, so the object reached here is not the one the caller named — and calling the getter to "
              "find out would be the page's code from C with no flow base");
        return 0;
    }
    *pval = pd.value;
    return 1;
}

/* THE ACCESSOR PAIR, INSTALLED AS SHAPE STORAGE — the getset half of the slot write, taking the descriptor's two
   references and leaving it holding neither. An ABSENT half is a NULL pointer in the shape, which is exactly
   what the read twin answers as JS_UNDEFINED, so the round trip is closed on both halves rather than on one. */
static void js_set_own_slot_getset(JSContext *ctx, JSProperty *pr, JSPropertyDescriptor *pd)
{
    if (pr->u.getset.getter)
        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter));
    if (pr->u.getset.setter)
        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.setter));
    pr->u.getset.getter = JS_IsObject(pd->getter) ? JS_VALUE_GET_OBJ(pd->getter) : NULL;
    pr->u.getset.setter = JS_IsObject(pd->setter) ? JS_VALUE_GET_OBJ(pd->setter) : NULL;
    pd->getter = JS_UNDEFINED;   /* the descriptor's references have transferred into the shape */
    pd->setter = JS_UNDEFINED;
}

/* THE WRITE TWIN OF JS_GetOwnSlotDesc — a SLOT's whole state PUT BACK, never an operation, and it CANNOT FAIL.
 *
 * The read side has drawn this line since it was written and the write side never did: cow_restore_base put a
 * captured baseline value back with JS_SetProperty, a [[Set]], which walks the prototype chain, calls a setter,
 * consults `writable` and `extensible`, and REFUSES by THROWING — inside a context switch that has no flow base
 * to run an exception on. Three shapes reached it and each left a pending TypeError belonging to no flow, which
 * the next exception check read as its own: a slot the flow narrowed with a define (Object.freeze,
 * `{writable:false}`); a slot the flow DELETED off a non-extensible object, which a [[Set]] may not re-create;
 * and a SPURIOUS capture whose write then threw — `Math.PI = 1` inside a flow captures the slot, fails the
 * write, and comes back here to put back a value the slot never stopped holding. None of the three can arise
 * now, because none of the three questions is asked: the scheduler is not the page, and a swap writes storage.
 *
 * AND IT IS NOT A DEFINE EITHER, WHICH IS THE SAME SENTENCE ONE LAYER OUT. [[DefineOwnProperty]] would look
 * like the operation that "puts a descriptor back", and 10.1.6.3 ValidateAndApplyPropertyDescriptor is exactly
 * what makes it the wrong one: step 4 REFUSES every change to a non-configurable slot, and a slot the flow made
 * non-configurable is precisely the slot this write exists to widen back. `Object.defineProperty(shared,"x",{})`
 * defaults C/W/E to false, so the commonest define a page makes is one no define can undo. A restore that can
 * be refused is a restore that leaves a pending TypeError belonging to no flow — and JS_DefineProperty would
 * also run the exotic define hook and, on a Proxy, the page's own trap. So this writes STORAGE: the shape's
 * flag word and the property's union, asking nothing.
 *
 * IT MIRRORS EVERY KIND ITS TWIN ANSWERS, which is what makes the round trip closed rather than nearly closed:
 * a data slot, an ACCESSOR (its getter/setter pair, and the data<->accessor conversion in either direction — a
 * §6.2.6 descriptor is one record with two shapes and a slot can be restored across them), a VARREF (a
 * mapped-`arguments` element or a module-namespace export, whose storage IS a closure cell), an AUTOINIT
 * (instantiated on the way, exactly as the read instantiates it), a fast-array element, the array `length` PAIR
 * (through array_set_length_slot, so the elements above it go with it — a raw prop[0] write would leave a dense
 * count the length no longer describes), and a typed-array element, which is the one refusal left because its
 * storage is the BUFFER's bytes and the byte entry owns them.
 * THE ATTRIBUTES ARE PART OF THE STATE, not a second operation on it: C/W/E come off the descriptor and land in
 * the shape here, which is what makes a flow's `Object.freeze` on shared state something the swap can take back.
 *
 * Consumes the descriptor's values. There is no status to return: a slot write is refused by nothing, and the
 * only way it can not happen is an allocation failure, which is a CHECK at the origin rather than a code the
 * caller might default. */
void JS_SetOwnSlotDesc(JSContext *ctx, JSValueConst obj, JSAtom prop, JSPropertyDescriptor *pd)
{
    JSShapeProperty *prs;
    JSProperty *pr;
    JSObject *p;
    uint32_t idx;
    bool want_getset = (pd->flags & JS_PROP_GETSET) != 0;

    DCHECK(JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT,
           "a COW delta holds a slot on something that is not an object");
    DCHECK((pd->flags & ~(JS_PROP_C_W_E | JS_PROP_GETSET)) == 0,
           "a COW slot restore was handed flags outside the §6.2.6 descriptor: the read twin answers C/W/E and "
           "the accessor bit, and an internal bit written from here would name storage the entry never read");
    DCHECK(!want_getset || (JS_IsUndefined(pd->value) && !(pd->flags & JS_PROP_WRITABLE)),
           "a COW slot restore holds BOTH descriptor shapes at once — §6.2.6 makes them exclusive, so one of "
           "the two was written by something that did not read it back off a slot");
    p = JS_VALUE_GET_OBJ(obj);
    DCHECK(p->class_id != JS_CLASS_PROXY,
           "a COW delta holds a Proxy: capturing or swapping it would run the page's traps mid-context-switch");

 retry:
    prs = find_own_property(&pr, p, prop);
    if (prs) {
        switch (prs->flags & JS_PROP_TMASK) {
        case JS_PROP_GETSET:
            if (want_getset) {
                js_set_own_slot_getset(ctx, pr, pd);
            } else {
                /* ACCESSOR -> DATA. The slot the flow left is an accessor and the state being put back is a
                   value, so the pair is dropped and the union becomes a value; the shape's kind bits go with
                   it, and the C/W/E tail below then writes the descriptor's own attributes over them. */
                if (js_shape_prepare_update(ctx, p, &prs))
                    CHECK_FAIL("a COW slot restore could not reshape an accessor slot back into a data slot");
                if (pr->u.getset.getter)
                    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.getter));
                if (pr->u.getset.setter)
                    JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, pr->u.getset.setter));
                prs->flags &= ~(JS_PROP_TMASK | JS_PROP_WRITABLE);
                pr->u.value = pd->value;
                pd->value = JS_UNDEFINED;
            }
            break;
        case JS_PROP_VARREF:
            /* the slot's storage is the CELL, which is what the read twin read through. A module namespace's
               export is write-protected to the PAGE and this is not the page: the entry exists because
               delete_property captures before it learns the slot is non-configurable, and putting the same
               value back through the cell is the no-op that spurious capture asks for. */
            DCHECK(!want_getset,
                   "a COW slot restore is putting an ACCESSOR over a VARREF slot: the cell IS the storage and "
                   "an accessor has none, so the two sides of this entry name different slots");
            set_value(ctx, pr->u.var_ref->pvalue, pd->value);
            pd->value = JS_UNDEFINED;
            break;
        case JS_PROP_AUTOINIT:
            /* instantiate and retry — the read twin does exactly this, so by the time an entry exists the slot
               is a data slot; a lazy intrinsic that a flow has not yet touched can still be one here. */
            if (JS_AutoInitProperty(ctx, p, prop, pr, prs))
                CHECK_FAIL("a COW slot restore could not instantiate an AUTOINIT slot: a dropped baseline "
                           "write leaks one flow's state into every sibling");
            goto retry;
        default:
            if (want_getset) {
                /* DATA -> ACCESSOR: the mirror of the conversion above, and the reason the entry is one record
                   rather than a value plus a flag — a slot can be restored across the two shapes in either
                   direction, and neither direction is the "normal" one. */
                DCHECK(!(prs->flags & JS_PROP_LENGTH),
                       "a COW slot restore is putting an ACCESSOR over an Array's `length`: 10.4.2 makes that "
                       "slot non-configurable, so no define can have turned it into one and the entry that "
                       "says otherwise was not read off this slot");
                if (js_shape_prepare_update(ctx, p, &prs))
                    CHECK_FAIL("a COW slot restore could not reshape a data slot into an accessor slot");
                JS_FreeValue(ctx, pr->u.value);
                prs->flags = (prs->flags & ~(JS_PROP_TMASK | JS_PROP_WRITABLE)) | JS_PROP_GETSET;
                pr->u.getset.getter = NULL;
                pr->u.getset.setter = NULL;
                js_set_own_slot_getset(ctx, pr, pd);
            } else if (prs->flags & JS_PROP_LENGTH) {
                /* the array `length` is a PAIR — the stored number and the elements it bounds — so it is put
                   back through the same slot write that removes them; a raw prop[0] store would leave a dense
                   count the length no longer describes. The value came off the read twin, which answers this
                   slot as the number the slot STORES (an int, or a double above INT32_MAX), so the conversion
                   below runs no page code. */
                uint32_t len, reached;
                DCHECK(p->class_id == JS_CLASS_ARRAY,
                       "a JS_PROP_LENGTH slot on a class that is not Array");
                DCHECK(JS_IsNumber(pd->value),
                       "an Array length slot restore was handed something that is not the number the read twin "
                       "answers with — a coercion here would be the page's valueOf with no flow under it");
                JS_ToUint32(ctx, &len, pd->value);
                JS_FreeValue(ctx, pd->value);
                pd->value = JS_UNDEFINED;
                reached = array_set_length_slot(ctx, p, len, /*as_slot*/true);   /* the length actually REACHED */
                DCHECK(reached == len,
                       "an Array length slot restore did not reach the length it was given — the SLOT shrink "
                       "removes an index whatever its attributes, so nothing can block it");
                (void)reached;
            } else {
                set_value(ctx, &pr->u.value, pd->value);
                pd->value = JS_UNDEFINED;
            }
            break;
        }
        /* THE ATTRIBUTES, LAST AND FROM THE SAME RECORD. The payload write above can RESHAPE the object — both
           conversions clone the shape, and the length shrink deletes indices — so `prs` is re-found rather than
           carried across it, which is the bug a single stale pointer would have written into the shape. */
        prs = find_own_property(&pr, p, prop);
        DCHECK(prs != NULL,
               "a COW slot restore lost the slot it had just written into — the payload write removed the "
               "property whose attributes were about to be put back");
        if (js_update_property_flags(ctx, p, &prs,
                                     (prs->flags & ~JS_PROP_C_W_E) | (pd->flags & JS_PROP_C_W_E)))
            CHECK_FAIL("a COW slot restore could not reshape the object to put a slot's ATTRIBUTES back: a "
                       "baseline descriptor left narrowed leaks one flow's freeze into every sibling");
        goto done;
    }

    if (p->fast_array && __JS_AtomIsTaggedInt(prop)) {
        idx = __JS_AtomToUInt32(prop);
        DCHECK(!want_getset,
               "a COW slot restore is putting an ACCESSOR into a dense array element: making an index an "
               "accessor converts the array to a slow one, so no dense element can ever have been read as one");
        if (p->class_id == JS_CLASS_MAPPED_ARGUMENTS) {
            DCHECK(idx < p->u.array.count,
                   "a mapped-arguments element slot outside the dense part: the mapped part is fixed at "
                   "creation and a delete converts the object to a slow array first");
            set_value(ctx, p->u.array.u.var_refs[idx]->pvalue, pd->value);
            pd->value = JS_UNDEFINED;
            goto done;
        }
        if (is_typed_array(p->class_id)) {
            /* DELETED: the typed-array element restore. It re-issued JS_SetPropertyValue to put the NUMBER the
               read twin answered back into the element, and it was the second half of a unit that is wrong: a
               typed array's elements are BYTES in an ArrayBuffer, not slots, and a JSValue cannot carry them
               back. Under JS_NAN_BOXING — every 32-bit build, and the wasm one is 32-bit — __JS_NewFloat64
               NORMALISES a NaN, so a Float64Array element the page stored with a payload came back canonical and
               the resume was not byte-identical; a DataView's write has no element to name at all; and two views
               over one buffer named one storage under two keys the delta could not know were the same.
               The bytes are captured on the BUFFER now (cow_capture_bytes / JSTimeTravelHooks.buf_state), and no
               element entry is created for a typed array at all: cow_capture ROUTES a typed array's numeric
               index to the bytes, so every site that captures a slot is covered by that one decision. Reaching
               here means a capture went around cow_capture and made a slot entry for storage the byte entry
               owns; the read twin refuses the same thing one step earlier, at the capture. */
            DFAIL("a COW delta holds a typed-array element as a property SLOT: its storage is the buffer's "
                  "BYTES and the byte entry owns it, so route the capture that made this entry through "
                  "cow_capture_bytes");
            goto done;
        }
        DCHECK((pd->flags & JS_PROP_C_W_E) == JS_PROP_C_W_E,
               "a COW slot restore is putting NARROWED attributes onto a dense array element: a dense element "
               "is always C/W/E and narrowing one converts the array to a slow one, so the descriptor and the "
               "storage disagree about which object this is");
        if (idx >= p->u.array.count && array_grow_dense_to(ctx, p, idx))
            CHECK_FAIL("a COW slot restore could not grow an array's dense part back over the tail this flow "
                       "truncated: a dropped baseline element leaks one flow's state into every sibling");
        set_fast_array_element(ctx, p, idx, pd->value);
        pd->value = JS_UNDEFINED;
        goto done;
    }

    /* THE SLOT IS ABSENT: this flow DELETED a baseline one and the restore has to put it back. Not
       JS_CreateProperty, which is [[DefineOwnProperty]]'s create half — it consults `extensible`, runs the
       exotic define_own_property hook, and refuses a typed array's numeric index, and the first of those is
       precisely the `Object.preventExtensions(o); delete o.x` that used to throw mid-switch.
       THE ATTRIBUTES COME OFF THE DESCRIPTOR, which is what this line could not do while an entry held only a
       value: it created every re-instated slot C_W_E, so a baseline `{writable:false}` or a non-enumerable
       method came back writable and enumerable and no sibling could tell. */
    DCHECK(!(p->class_id == JS_CLASS_STRING && __JS_AtomIsTaggedInt(prop) &&
             JS_VALUE_GET_TAG(p->u.object_data) == JS_TAG_STRING &&
             __JS_AtomToUInt32(prop) < JS_VALUE_GET_STRING(p->u.object_data)->len),
           "a COW delta holds a String object's index slot: the read twin answers it out of the string's own "
           "characters, which are non-writable and non-configurable, so no flow can have written or deleted one");
    pr = add_property(ctx, p, prop, pd->flags & (JS_PROP_C_W_E | JS_PROP_GETSET));
    CHECK(pr, "a COW slot restore could not re-create a slot this flow deleted: a dropped baseline write leaks "
              "one flow's state into every sibling");
    if (want_getset) {
        pr->u.getset.getter = NULL;
        pr->u.getset.setter = NULL;
        js_set_own_slot_getset(ctx, pr, pd);
    } else {
        pr->u.value = pd->value;
        pd->value = JS_UNDEFINED;
    }

 done:
    /* THE DESCRIPTOR IS CONSUMED, and what is freed here is the half the slot did not take — a data restore's
       empty accessor pair, an accessor restore's empty value, and everything a DFAIL'd arm still held. Each arm
       above hands its own field over by clearing it, so this frees exactly what nothing adopted. */
    JS_FreeValue(ctx, pd->value);
    JS_FreeValue(ctx, pd->getter);
    JS_FreeValue(ctx, pd->setter);
    pd->value = JS_UNDEFINED;
    pd->getter = JS_UNDEFINED;
    pd->setter = JS_UNDEFINED;
}

void JS_FreePropertyEnum(JSContext *ctx, JSPropertyEnum *tab,
                         uint32_t len)
{
    js_free_prop_enum(ctx, tab, len);
}

/* return -1 if exception (Proxy object only) or true/false */
int JS_IsExtensible(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;

    if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT))
        return false;
    p = JS_VALUE_GET_OBJ(obj);
    if (unlikely(p->class_id == JS_CLASS_PROXY)) {
        /* ONE assert for all seven C-side call sites, placed where the trap actually starts rather than at each
           caller: js_proxy_isExtensible runs the page's `isExtensible` handler, so reaching it from C is the
           page's code with no flow base. The routed answer is a GP_ISEXT request; a caller that lands here is
           one that still has to route, and it says which one instead of hanging. */
        DCHECK(0, "a C-side IsExtensible reached a Proxy — its isExtensible trap would run with no flow base; "
                  "route that caller onto the GP_ISEXT request");
        return js_proxy_isExtensible(ctx, obj);
    } else {
        return p->extensible;
    }
}

/* return -1 if exception (Proxy object only) or true/false */
/* 10.4.5.9 IsTypedArrayFixedLength(O). Step 1: an AUTO length (a length-tracking view) is not fixed. Step 3: a
   view over a RESIZABLE, non-shared buffer is not fixed either, however its own length was written — the buffer
   can shrink under it. A growable SharedArrayBuffer is excluded by step 3's second test, so only step 1 can
   answer for one. */
static bool typed_array_is_fixed_length(JSObject *p)
{
    JSTypedArray *ta = p->u.typed_array;
    JSArrayBuffer *abuf = ta->buffer->u.array_buffer;
    if (ta->track_rab) return false;
    return !(array_buffer_is_resizable(abuf) && !abuf->shared);
}

int JS_PreventExtensions(JSContext *ctx, JSValueConst obj)
{
    JSObject *p;

    if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT))
        return false;
    p = JS_VALUE_GET_OBJ(obj);
    if (unlikely(p->class_id == JS_CLASS_PROXY))
        return js_proxy_preventExtensions(ctx, obj);
    /* 10.4.5.4 step 2: a TypedArray whose length is not fixed REFUSES. The extensibility methods behave as if
       the array had a fixed set of index properties, and it does not — so the refusal is the whole of the
       exotic behaviour, and every caller inherits it: Object.preventExtensions and Object.seal turn the false
       into 7.3.15's TypeError, Reflect.preventExtensions reports it. */
    if (unlikely(is_typed_array(p->class_id)) && !typed_array_is_fixed_length(p))
        return false;
    /* THE CLASS'S OWN ANSWER, where it has one — see JSClassExoticMethods.prevent_extensions. Web IDL §3.9.5
       [[PreventExtensions]] refuses for every legacy platform object, and there was no route by which a class
       could say so, so `Object.freeze` on one of them succeeded. Asked BEFORE the capture, because a refusal
       changes nothing and a delta entry for a write that did not happen is a baseline nobody's unapply owes. */
    if (unlikely(p->is_exotic)) {
        const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
        if (em && em->prevent_extensions) {
            int r = em->prevent_extensions(ctx, obj);
            if (r <= 0)
                return r;
        }
    }
    cow_capture_obj(ctx, p);   /* forced-exec: `Object.freeze(shared)` in one flow was permanent for every sibling */
    p->extensible = false;
    return true;
}

/* return -1 if exception otherwise true or false */
int JS_HasProperty(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSObject *p;
    int ret;
    JSValue obj1;

    if (unlikely(JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT))
        return false;
    p = JS_VALUE_GET_OBJ(obj);
    for(;;) {
        if (p->is_exotic) {
            const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
            if (em && em->has_property) {
                /* has_property can free the prototype */
                obj1 = js_dup(JS_MKPTR(JS_TAG_OBJECT, p));
                ret = em->has_property(ctx, obj1, prop);
                JS_FreeValue(ctx, obj1);
                return ret;
            }
        }
        /* ASSERTED, not claimed. This is a C-side [[GetOwnProperty]] on every level of the chain, so a Proxy
           anywhere on it runs the page's getOwnPropertyDescriptor trap with no flow base. The routed
           [[HasProperty]] is a GP_HAS request; a caller reaching HERE with a proxied receiver is one that
           still has to route, and this is where it says so rather than hanging.
           WHICH `x in y` IT WAS is in the message, for the reason the read and write arms both state at
           length: this is one line reachable from every C membership test in the tree, and "route that caller"
           with no object in it is an instruction over hundreds of spellings. The proxy is named by the level
           of the chain it sits at, which is what distinguishes a proxied RECEIVER from a proxied prototype —
           a difference that decides whether the fix is at the caller or at whoever installed the prototype. */
#if APICLIENT_DEV
        if (unlikely(p->class_id == JS_CLASS_PROXY)) {
            char pbuf[ATOM_GET_STR_BUF_SIZE], rbuf[ATOM_GET_STR_BUF_SIZE];
            char rtag[128], frames[2048], why[4096];
            JSObject *robj = JS_VALUE_GET_OBJ(obj);

            js_why_backtrace(ctx, frames, sizeof frames);
            snprintf(why, sizeof why,
                     "A membership test for `%s` walked onto a Proxy from C. The test started on %s / %s and "
                     "the Proxy is %s the receiver. §7.3.11 HasProperty ( obj, propertyKey ) is `Return ? "
                     "obj.[[HasProperty]](propertyKey)`, and a Proxy's is §10.5.7 [[HasProperty]] ( "
                     "propertyKey ), whose step 7 is `Let boolTrapResult be ToBoolean(? Call(trap, handler, "
                     "<< target, propertyKey >>))` and whose step 5 already reaches the page through "
                     "`GetMethod(handler, \"has\")` "
                     "— the page's function, and a C activation has no flow base under it. ROUTE THE CALLER "
                     "onto the GP_HAS request, which is the same one the `in` opcode issues. %s",
                     JS_AtomGetStr(ctx, pbuf, sizeof(pbuf), prop),
                     JS_AtomGetStr(ctx, rbuf, sizeof(rbuf),
                                   ctx->rt->class_array[robj->class_id].class_name),
                     js_why_class_string(ctx, robj, rtag, sizeof rtag),
                     p == robj ? "ITSELF" : "on its PROTOTYPE CHAIN, not",
                     frames);
            DFAIL(why);
        }
#endif
        /* JS_GetOwnPropertyInternal can free the prototype */
        js_dup(JS_MKPTR(JS_TAG_OBJECT, p));
        ret = JS_GetOwnPropertyInternal(ctx, NULL, p, prop);
        JS_FreeValue(ctx, JS_MKPTR(JS_TAG_OBJECT, p));
        if (ret != 0)
            return ret;
        if (is_typed_array(p->class_id)) {
            ret = JS_AtomIsNumericIndex(ctx, prop);
            if (ret != 0) {
                if (ret < 0)
                    return -1;
                return false;
            }
        }
        p = js_walk_proto(ctx, p);
        if (!p)
            break;
    }
    return false;
}

