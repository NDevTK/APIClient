/* WEB IDL'S INDEXED PROPERTY GETTER — what `list[0]`, `nodes[i]` and `attrs[0]` all are — AND THE FOUR OTHER
 * INTERNAL METHODS §3.9 OVERRIDES ALONGSIDE IT.
 *
 * THE READ WAS NEVER THE WHOLE OF IT, AND A HALF-IMPLEMENTED LEGACY PLATFORM OBJECT IS NOT A NARROWER ONE — it
 * is a DIFFERENT object, because §3.9's methods only agree with each other when all of them are there. With
 * just [[GetOwnProperty]], `list[0]` correctly answered a non-writable descriptor (so an in-range assignment
 * was already refused) while `list[99] = "x"` created a REAL OWN PROPERTY that shadowed the getter at 99 for
 * the rest of the object's life, `delete list[0]` answered `true` with the property still there, and
 * `Object.freeze(list)` succeeded on an object `Object.isFrozen` then called unfrozen. Four answers about one
 * object, three of them wrong, none of them wrong in a way a read could reveal. So the file owns the write side
 * too: §3.9.3 [[DefineOwnProperty]], §3.9.4 [[Delete]] and §3.9.5 [[PreventExtensions]] beside §3.9.1
 * [[GetOwnProperty]] and §3.9.6 [[OwnPropertyKeys]] — five of the SIX the spec names, and §3.9.2 [[Set]] is the
 * sixth, which needs no hook of its own because its every arm is one of those: §3.9.1's descriptor decides the
 * in-range write and §3.9.3's define decides the out-of-range one (see the note above the define below).
 *
 * WHY IT IS A MECHANISM AND NOT A PROPERTY SNAPSHOT. Four interfaces in the DOM declare one — DOMTokenList,
 * NodeList, HTMLCollection, NamedNodeMap — and every one of them is a VIEW over state that changes underneath
 * it. Writing `0`, `1`, `2` onto the object as real properties would answer today's question and go stale the
 * moment the class attribute or the child list changed, which is worse than the absence: a page reading
 * `list[0]` after `list.add('x')` would get the old answer with nothing to indicate it was wrong.
 *
 * SO THE LOOKUP IS THE ALGORITHM. Web IDL §3.9 says [[GetOwnProperty]] on such an object first consults the
 * OWN properties, then, for an array-index property name, the supported property indices. quickjs's exotic
 * class hook is that seam exactly — it runs only after the ordinary own-property lookup misses, which is the
 * ordering the spec states and not something this file has to arrange.
 *
 * ONE MECHANISM, FOUR BACKINGS. What differs between the interfaces is only how many indices they support and
 * what is at one, so that is what an interface declares; everything else — the index parse, the descriptor,
 * the key enumeration, `in`, and @@iterator — is written once here. Four copies would be four chances to
 * disagree about what `list[-1]` or `list['01']` means, and the answer to both is "not an index".
 *
 * ITERATION IS THE SPEC'S OWN ANSWER: §3.7.9 Iterable declarations' define the iteration methods, step 1.1
 * gives an interface with an indexed getter %Array.prototype.values% as its @@iterator. Not a lookalike — the actual function, which works because the
 * object is array-like by construction. `for (const c of el.classList)` is ordinary code that had nothing. */
#include <string.h>
#include <stdlib.h>

#include "check.h"
#include "quickjs.h"
#include <stdio.h>
#include "core/idl_indexed.h"

static JSClassID g_class;
static int       g_ready;

/* WEB IDL §3.9.7 Abstract operations' "array index" TEST IS THE ENGINE'S OWN, and asking it of the ATOM rather
   than of the atom's TEXT is what makes that possible. JS_AtomIsIndex is quickjs's JS_AtomIsArrayIndex: a
   canonical decimal in [0, 2^32−2] — `is_num_string` refuses a leading zero, a sign, a fraction and anything
   that is not the string ToString would produce, and the `val != -1` beside it is §3.9.7's own step 7, "If
   index ≥ 2**32 − 1, then return false."
   A HAND-ROLLED DECIMAL PARSE STOOD HERE AND AGREED ON EVERY ONE OF THOSE, which is the argument for deleting
   it rather than for keeping it: THREE of §3.9's internal methods ask this question now, and a second spelling
   is a second chance for them to disagree about `"01"`. quickjs.h says so at JS_AtomIsIndex's own declaration.
   IT ALSO ANSWERS THE SYMBOL, WHICH THE TEXT PARSE COULD NOT. JS_AtomIsIndex tests the atom's TYPE, so a
   Symbol key is not an index. JS_AtomToCString on a symbol atom returns its DESCRIPTION (__JS_AtomToValue with
   force_string), so the parse it fed was handed `"0"` for `Symbol("0")` and answered TRUE — `list[Symbol("0")]`
   returned element 0 and had a property descriptor, where §3.9.7's own algorithm opens "If P is not a String,
   then return false." The same description reached the NAMED getter below for every other symbol.

   THE OTHER HALF OF THAT SENTENCE, asked where the named getter is. §3.9.3 [[DefineOwnProperty]] step 2 states
   the condition outright — "P is a String" — and §3.9.1's named arm inherits it, because §3.9.7's named
   property visibility algorithm step 1 asks whether P is a SUPPORTED PROPERTY NAME and an interface's supported
   property names are strings; a Symbol therefore fails step 1 before anything else is consulted. quickjs has no
   exported "is this atom a symbol", and JS_AtomToValue answers it by handing back the Symbol itself for a symbol
   atom and a String for a string one — which is what core/storage/storage.c asks the same question with, for
   the same section. */
static bool atom_is_string(JSContext *ctx, JSAtom prop)
{
    JSValue v = JS_AtomToValue(ctx, prop);
    bool s = JS_IsString(v);

    JS_FreeValue(ctx, v);
    return s;
}

/* §3.9'S ONE QUESTION ABOUT A WRITE TO AN INDEX: does the interface declare an INDEXED PROPERTY SETTER? Three
 * of §3.9's internal methods read it and none of them may answer it differently — §3.9.1
 * [[GetOwnProperty]]'s LegacyPlatformObjectGetOwnProperty step 1.2.7 sets desc.[[Writable]] from it ("If O
 * implements an interface with an indexed property setter, then set desc.[[Writable]] to true, otherwise set
 * it to false."),
 * §3.9.2 [[Set]] step 1.1 decides from it whether to invoke one, and §3.9.3 [[DefineOwnProperty]] step 1.2
 * refuses on it. So it is answered ONCE, here, and read three times.
 *
 * NAMED RESIDUAL — the code below is CORRECT for every interface that reaches it and NARROWER than §3.9.3.
 *  WHAT IS NOT COVERED: an interface that DECLARES an indexed property setter, spelled
 *    `setter undefined (unsigned long index, T value)` in its IDL. A grep of that spelling over the checked-out
 *    @webref IDL finds nine, in three standards: HTML §2.6.4.3 The HTMLOptionsCollection interface and §4.10.7
 *    The select element (the INTERFACE section — §15.5.16 carries the same title for the rendering), SVG's five
 *    list interfaces, and CSS Typed OM's CSSUnparsedValue and CSSTransformValue. This engine builds none of
 *    them, which is why this answers `false` for every decl rather than reading a field that no decl in the
 *    tree would write — a field with no writer is the shape CLAUDE.md's defaulted-field rule is about.
 *  WHAT THE NEXT DIFF BUILDS: a `setter` callback on IdlIndexedDecl, this predicate returning `d->setter !=
 *    NULL`, and §3.9.7's invoke an indexed property setter steps (ToUint32 on P, the `creating` flag, the
 *    conversion of V to the type of the operation's second argument) called from step 1.3 of the define arm
 *    below, where the DCHECK there stands today.
 *  HOW ITS ABSENCE WOULD SHOW: on the day such an interface is added and this still answers false,
 *    `select.options[0] = new Option()` is refused instead of replacing the option, and
 *    `Object.getOwnPropertyDescriptor(select.options, "0").writable` reads false where a browser reads true. */
static bool has_indexed_setter(const IdlIndexedDecl *d)
{
    (void)d;
    return false;
}

/* THE PER-OBJECT STATE. The decl alone used to be the opaque, which was right while an indexed object had
   nothing of its own; an index cache is per-collection, so the object needs a place to keep one. The scratch
   is allocated in the same block and handed out by idl_indexed_cache. */
typedef struct { const IdlIndexedDecl *decl; } IdlIndexedObj;

static const IdlIndexedDecl *decl_of(JSValueConst obj)
{
    IdlIndexedObj *o = JS_GetOpaque(obj, g_class);
    return o ? o->decl : NULL;
}

void *idl_indexed_cache(JSValueConst obj)
{
    IdlIndexedObj *o = JS_GetOpaque(obj, g_class);
    if (!o || !o->decl->cache_size) return NULL;
    return (char *)o + sizeof(IdlIndexedObj);
}

static void idl_indexed_finalizer(JSRuntime *rt, JSValue val)
{
    IdlIndexedObj *o = JS_GetOpaque(val, g_class);
    js_free_rt(rt, o);
}

/* THE DECL A CLASS RESOLVED IS A STATIC PER-INTERFACE CONSTANT, and every one of §3.9's algorithms below
   dereferences it. NULL is the one legitimate answer and it means "not an indexed interface"; a decl that is
   present and incomplete is a class that built one wrong, and the ADDRESS the reader needs is the INTERFACE
   rather than the C line — three of these functions are reached from two classes' hooks and from quickjs's own
   internal-method dispatch, so a `__FILE__`/`__LINE__` threaded from the caller would name one of five
   forwarding functions and tell nobody which interface to open. `interface_name` is that address, which is
   what idl_indexed.h means by "for the assertions, and only for them".
   IT IS A MACRO AND NOT A CALL, so the file:line it stamps is the internal method that was performing the
   operation rather than one shared checker — §AN-ASSERT-THAT-NAMES-A-REMEDY's rule, at the scale this file has
   it. Every use is guarded by an earlier `if (!d)`, which is the NULL contract itself and not a second test. */
#define DCHECK_DECL(d)                                                                                        \
    DCHECKF((d)->interface_name && (d)->length && (d)->item,                                                  \
            "an indexed interface's decl reached one of Web IDL §3.9's internal methods without an interface " \
            "name, a length or an item — the decl is a static per-interface constant, so the class that "      \
            "resolved this one built it incomplete (interface_name=%s)",                                       \
            (d)->interface_name ? (d)->interface_name : "(unnamed)")

int idl_indexed_own_property(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop,
                             const IdlIndexedDecl *d)
{
    uint32_t i;
    JSValue v;

    if (!d) return 0;
    DCHECK_DECL(d);
    if (JS_AtomIsIndex(ctx, &i, prop)) {
        /* §3.9.7's LegacyPlatformObjectGetOwnProperty step 1.2: a SUPPORTED property index, or nothing —
           JS_UNDEFINED past the end is "not a supported property index" and falls through to `return 0`. */
        v = (i < d->length(ctx, obj)) ? d->item(ctx, obj, i) : JS_UNDEFINED;
    } else if (d->named && atom_is_string(ctx, prop)) {
        /* Step 2. THE STRING TEST IS THE SPEC'S OWN CONDITION, quoted from §3.9.3 [[DefineOwnProperty]] step 2
           where it is written out ("If O supports named properties, O does not implement an interface with the
           [Global] extended attribute, P is a String, and P is not an unforgeable property name of O, then:"):
           a Symbol is never a supported property NAME, and without the test the atom's own DESCRIPTION was
           what the named getter was handed. */
        const char *s = JS_AtomToCString(ctx, prop);
        if (!s) return -1;
        v = d->named(ctx, obj, s);
        JS_FreeCString(ctx, s);
    } else {
        return 0;
    }
    if (JS_IsUndefined(v)) return 0;   /* not a supported index or name: the ordinary lookup continues */
    if (!desc) { JS_FreeValue(ctx, v); return 1; }
    /* A VALUE, never an accessor. The engine's C read path asserts against an exotic accessor because a getter
       there would be page code reached from inside a property read; an indexed getter has none by definition.
       [[Writable]] IS §3.9.1'S OWN SENTENCE and it is asked rather than assumed — see has_indexed_setter, which
       is the same answer §3.9.3's define arm below refuses on, so the descriptor a page reads and the define it
       then attempts cannot disagree. */
    desc->flags = (has_indexed_setter(d) ? JS_PROP_WRITABLE : 0) | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
    desc->value = v;
    desc->getter = JS_UNDEFINED;
    desc->setter = JS_UNDEFINED;
    return 1;
}

/* WEB IDL §3.9.6 [[OwnPropertyKeys]] — its step 2, which appends §2.5.6.1 Indexed properties' SUPPORTED
   PROPERTY INDICES "in ascending numerical order". That is what Object.keys, spread and for-in see, and the
   ascending order is what makes `Object.keys(list)` read like an array's; quickjs merges these with the
   object's ordinary keys, which is steps 4 and 5.
   STEP 3, THE SUPPORTED PROPERTY NAMES, IS NOT APPENDED, and it is the same missing piece the delete below
   names: step 3 takes only the names VISIBLE under §3.9.7's named property visibility algorithm, which this
   file does not have. So `Object.getOwnPropertyNames(el.children)` omits a named property a browser lists,
   for the two decls that carry a named getter and for no other. */
int idl_indexed_own_property_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj,
                                   const IdlIndexedDecl *d)
{
    JSPropertyEnum *tab;
    uint32_t n, i;

    if (!d) { *ptab = NULL; *plen = 0; return 0; }
    n = d->length(ctx, obj);
    tab = n ? js_malloc(ctx, sizeof(*tab) * n) : NULL;
    if (n && !tab) return -1;
    for (i = 0; i < n; i++) {
        char buf[16];
        snprintf(buf, sizeof buf, "%u", i);
        tab[i].is_enumerable = 1;
        tab[i].atom = JS_NewAtom(ctx, buf);
        if (tab[i].atom == JS_ATOM_NULL) {
            while (i--) JS_FreeAtom(ctx, tab[i].atom);
            js_free(ctx, tab);
            return -1;
        }
    }
    *ptab = tab;
    *plen = n;
    return 0;
}

/* WEB IDL §3.9.3 [[DefineOwnProperty]] — STEP 1, WHICH IS THE WHOLE OF WHAT A WRITE TO AN INDEX IS HERE.
 *
 * IT REFUSES EVERY ARRAY INDEX, IN RANGE OR NOT, and that is the spec rather than a simplification: step 1 asks
 * only "does O support indexed properties and is P an array index", never whether P is a SUPPORTED index. That
 * is exactly where it differs from §3.9.1, which asks the second question — and the difference is the defect
 * this method exists to close. An in-range write was already refused without any hook, because §3.9.1's
 * descriptor is non-writable and ECMAScript 10.1.9.2 OrdinarySetWithOwnDescriptor step 2.a returns false on a
 * non-writable data descriptor. An out-of-range write reached none of that: no own descriptor, nothing on the
 * prototype chain, so step 2.d.ii's CreateDataProperty ran [[DefineOwnProperty]] on the receiver, which without
 * this hook is the ordinary define. `list[99] = "x"` therefore CREATED a real own property at "99" — and it
 * outlived the reason: a later `list.add(...)` growing the collection past 99 would keep answering `list[99]`
 * with the string somebody had assigned, for ever, because an own property shadows the indexed getter.
 * `Object.defineProperty(list, "0", …)` arrived at the same door and got the same wrong answer.
 *
 * A NON-INDEX KEY IS ORDINARY, and this hook REPLACES the ordinary path rather than preceding it, so the
 * ordinary path is re-entered explicitly with the exotic step suppressed. Forgetting that would not break
 * `list[0]`; it would break every property this engine and every page define on one of these objects. That
 * fall-through is also where §3.9.3's step 2 would go for the two decls that carry a NAMED getter, and it does
 * not go there yet — see the note above idl_indexed_delete_property, which the same absence covers. */
int idl_indexed_define_own_property(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                                    JSValueConst getter, JSValueConst setter, int flags,
                                    const IdlIndexedDecl *d)
{
    uint32_t i;

    if (d) {
        DCHECK_DECL(d);
        if (JS_AtomIsIndex(ctx, &i, prop)) {
            /* Step 1.1, "If the result of calling IsDataDescriptor(Desc) is false, then return false." A
               §6.2.6 descriptor is a data descriptor when it has [[Value]] or [[Writable]]; an accessor and a
               generic one both fail here. It is written even though step 1.2 refuses everything today, because
               the day has_indexed_setter answers true this is the step that still stands. */
            if (!(flags & (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE)))
                return JS_RefuseOrThrowTypeError(ctx, flags,
                                                 "an indexed property of a legacy platform object cannot be "
                                                 "defined from a descriptor that is not a data descriptor");
            /* Step 1.2. The DCHECK is has_indexed_setter's residual made into a crash: the moment that
               predicate can answer true, step 1.3 — §3.9.7's invoke an indexed property setter — must exist,
               and this refusal must stop being the answer. */
            DCHECKF(!has_indexed_setter(d),
                    "%s declares an INDEXED PROPERTY SETTER and Web IDL §3.9.3 step 1.3 is not built — the "
                    "define must invoke it through §3.9.7's invoke an indexed property setter (ToUint32 on P, "
                    "the `creating` flag, the conversion of V to the setter's second argument type) instead of "
                    "refusing here", d->interface_name);
            return JS_RefuseOrThrowTypeError(ctx, flags,
                                             "this interface declares no indexed property setter, so an "
                                             "indexed property of it cannot be defined or assigned");
        }
    }
    return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
}

/* WEB IDL §3.9.4 [[Delete]] — STEP 1, the array-index arm, whose three sub-steps read "Let index be the result
 * of calling ! ToUint32(P).", "If index is not a supported property index, then return true." and "Return
 * false."
 *
 * SO A SUPPORTED INDEX IS UNDELETABLE AND SAYS SO, which is the half that was missing: with no hook quickjs's
 * delete_property missed the object's own shape, found no exotic deleter, and fell through to its "not found"
 * tail — `true`. `delete list[0]` therefore reported success while the property was still there on the very
 * next read, and `'use strict'; delete list[0]` completed instead of throwing.
 * PAST THE END the answer is `true`, and it was true before by accident. That is not a licence to leave it to
 * the fall-through: it is the same question asked of the same `length`, and a second route to it is what
 * §3.9.1 and §3.9.3 would then be one edit away from disagreeing with.
 *
 * STEP 2, THE NAMED-PROPERTY ARM, IS NOT HERE, and it is the one thing this pair does not answer. It needs
 * §3.9.7's named property visibility algorithm — including the prototype walk that makes `delete coll.item`
 * (the method is on the prototype, so the named property is invisible) a different question from
 * `delete coll.someElementId`. This engine already has that algorithm, written for Storage in
 * core/storage/storage.c, and the diff that answers step 2 is the one that lifts it out to be shared rather
 * than a third copy — the same algorithm §3.9.1's step 2 above is missing, which is why the two are one piece
 * of work and not two. Until then a named property on the two decls that have one (an HTMLCollection's and a
 * NamedNodeMap's) answers `delete` with `true` where a browser answers `false`. */
int idl_indexed_delete_property(JSContext *ctx, JSValueConst obj, JSAtom prop, const IdlIndexedDecl *d)
{
    uint32_t i;

    if (!d) return true;
    DCHECK_DECL(d);
    if (JS_AtomIsIndex(ctx, &i, prop))
        return i >= d->length(ctx, obj);
    /* Reached only after quickjs's ordinary own-property scan on this object missed, so "not an index" is
       "there is nothing of this object's own to delete", which §3.9.4's step 4 answers with true. */
    return true;
}

/* WEB IDL §3.9.5 [[PreventExtensions]] — "return false", and its note says why in one sentence: "this keeps
 * legacy platform objects extensible by making [[PreventExtensions]] fail for them."
 *
 * IT IS THE THIRD ANSWER TO ONE QUESTION and that is what makes it belong beside the two above rather than in
 * a later diff. §3.9.3 refuses to define an index and §3.9.4 refuses to delete one, both because the object's
 * property set is the collection's and not the page's; a successful freeze would have said the opposite about
 * the same object — that its property set is now fixed — while the collection went on changing underneath it.
 * `Object.freeze(el.classList)` used to succeed and `Object.isFrozen` then answered false anyway, because
 * §3.9.1's descriptors are configurable and 7.3.16 TestIntegrityLevel reads them: two engine answers about one
 * object that could not both be true. A browser throws at the freeze, which is 7.3.15 SetIntegrityLevel step 2
 * seeing this false and 20.1.2.6 Object.freeze ( obj ) turning it into a TypeError. */
static int idl_indexed_prevent_extensions(JSContext *ctx, JSValueConst obj)
{
    const IdlIndexedDecl *d = decl_of(obj);

    (void)ctx;
    DCHECK(d != NULL, "an object of the indexed-property class has no decl at a prevent-extensions");
    return 0;
}

/* THIS FILE'S OWN CLASS asks the algorithms above about the decl IT stores. A class that keeps its objects'
   state elsewhere — core/css/css_rule.c, whose CSSKeyframesRule is a rule first — resolves the decl its own way
   and calls the same functions, so there is one algorithm and two ways of finding the decl.
   THE DECL IS NEVER ABSENT ON AN OBJECT OF *THIS* CLASS, which is a different statement from the NULL contract
   the shared algorithms carry for their other caller: idl_indexed_new is the only thing that builds one and it
   CHECKs the allocation that holds the decl in the same call, so a NULL here is an object of this class that
   some other route created. Asserted in each hook rather than inside decl_of, because decl_of is also
   idl_indexed_cache's reader and that one legitimately answers for a decl with no cache. */
static int idl_indexed_get_own(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom prop)
{
    const IdlIndexedDecl *d = decl_of(obj);

    DCHECK(d != NULL, "an object of the indexed-property class has no decl — idl_indexed_new is the only thing "
                      "that builds one and it CHECKs the allocation carrying it, so this object reached the "
                      "class by another route");
    return idl_indexed_own_property(ctx, desc, obj, prop, d);
}

static int idl_indexed_own_names(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj)
{
    const IdlIndexedDecl *d = decl_of(obj);

    DCHECK(d != NULL, "an object of the indexed-property class has no decl at its key enumeration");
    return idl_indexed_own_property_names(ctx, ptab, plen, obj, d);
}

static int idl_indexed_define_own(JSContext *ctx, JSValueConst obj, JSAtom prop, JSValueConst val,
                                  JSValueConst getter, JSValueConst setter, int flags)
{
    const IdlIndexedDecl *d = decl_of(obj);

    DCHECK(d != NULL, "an object of the indexed-property class has no decl at a define — §3.9.3's array-index "
                      "arm would then fall through to the ordinary define and create the shadowing own "
                      "property it exists to refuse");
    return idl_indexed_define_own_property(ctx, obj, prop, val, getter, setter, flags, d);
}

static int idl_indexed_delete(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    const IdlIndexedDecl *d = decl_of(obj);

    DCHECK(d != NULL, "an object of the indexed-property class has no decl at a delete — §3.9.4's array-index "
                      "arm would then answer true for a supported index");
    return idl_indexed_delete_property(ctx, obj, prop, d);
}

/* THERE IS NO EXOTIC [[HasProperty]], AND THAT IS THE SPEC, NOT AN OMISSION. Web IDL §3.9 overrides exactly
   six internal methods on a legacy platform object — §3.9.1 [[GetOwnProperty]], §3.9.2 [[Set]], §3.9.3
   [[DefineOwnProperty]], §3.9.4 [[Delete]], §3.9.5 [[PreventExtensions]] and §3.9.6 [[OwnPropertyKeys]], which
   §3.9's own opening paragraphs name in exactly that set — and [[HasProperty]] is not among them, so `x in list` is
   OrdinaryHasProperty: this object's own lookup (which IS the indexed getter above, reached through
   [[GetOwnProperty]]) and then THE PROTOTYPE CHAIN.
   A hook here truncates that chain, because quickjs's contract for `has_property` is that the exotic answers
   definitively — it returns the exotic's answer and never walks the prototype. So one existed and
   `'length' in el.children` was FALSE while `el.children.length` answered 1, and `'item' in list`, `'add' in
   classList` and `'getNamedItem' in el.attributes` were false the same way. testharness's assert_array_equals
   opens with `"length" in actual`, so every collection assertion in the corpus failed its precondition and
   reported the collection as "not an array" — a hundred subtests whose real message was that `in` was wrong. */
static JSClassExoticMethods g_exotic = {
    .get_own_property = idl_indexed_get_own,
    .get_own_property_names = idl_indexed_own_names,
    .define_own_property = idl_indexed_define_own,
    .delete_property = idl_indexed_delete,
    .prevent_extensions = idl_indexed_prevent_extensions,
    /* The lookup above is an index parse and a read of the component's own state. There is no accessor in a
       Web IDL indexed property getter by construction, which is what lets the engine's accessor walk run it
       from C instead of routing it onto the trampoline. */
    .get_own_property_no_user_code = true,
};

static JSClassDef g_class_def = {
    "IndexedProperties", idl_indexed_finalizer, NULL, NULL, &g_exotic
};

void idl_indexed_init(JSContext *ctx)
{
    DCHECK(!g_ready, "idl_indexed_init ran twice — one instance is one document");
    g_class = 0;
    JS_NewClassID(JS_GetRuntime(ctx), &g_class);
    CHECK(JS_NewClass(JS_GetRuntime(ctx), g_class, &g_class_def) == 0,
          "the indexed-property class could not be registered");
    g_ready = 1;
}

void idl_indexed_free(JSRuntime *rt)
{
    (void)rt;
    g_ready = 0;
}

JSValue idl_indexed_new(JSContext *ctx, JSValueConst proto, const IdlIndexedDecl *decl)
{
    JSValue obj;

    DCHECK(g_ready, "an indexed-property object was built before its class was registered");
    DCHECK(decl && decl->length && decl->item, "an indexed interface must answer both length and item");
    obj = JS_NewObjectProtoClass(ctx, proto, g_class);
    if (JS_IsException(obj)) return obj;
    {
        /* The decl is BORROWED, and that is why it is a static per-interface constant: it must outlive every
           object. The CACHE is per object and zeroed, which is what "nothing cached yet" is spelled as. */
        IdlIndexedObj *o = js_mallocz(ctx, sizeof(IdlIndexedObj) + decl->cache_size);
        CHECK(o != NULL, "an indexed-property object could not allocate its per-object state");
        o->decl = decl;
        JS_SetOpaque(obj, o);
    }
    return obj;
}

void idl_indexed_install_iterable(JSContext *ctx, JSValueConst proto)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue arr = JS_GetPropertyStr(ctx, global, "Array");
    JSValue ap = JS_GetPropertyStr(ctx, arr, "prototype");
    JSValue values = JS_GetPropertyStr(ctx, ap, "values");

    DCHECK(JS_IsFunction(ctx, values), "Array.prototype.values is missing — §3.7.9 step 1.1 names it as the "
                                       "@@iterator an interface with an indexed getter is given, so there is "
                                       "nothing to install in its place");
    /* THE ACTUAL FUNCTION, not a lookalike: §3.7.9 step 1.1 states %Array.prototype.values%, and it works because the
       object is array-like by construction. A private copy would be a second array iterator to keep in step. */
    {
        JSValue sym_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
        JSValue it = JS_GetPropertyStr(ctx, sym_ctor, "iterator");
        JSAtom a = JS_ValueToAtom(ctx, it);
        CHECK(a != JS_ATOM_NULL, "@@iterator could not be reached");
        JS_DefinePropertyValue(ctx, (JSValue)proto, a, JS_DupValue(ctx, values),
                               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, it);
        JS_FreeValue(ctx, sym_ctor);
    }
    JS_FreeValue(ctx, values);
    JS_FreeValue(ctx, ap);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, global);
}

/* THE VALUE-ITERATOR MEMBERS ARE A DIFFERENT CLAUSE, AND THEY ARE NOT EVERY INDEXED INTERFACE'S. §3.7.9's
   step 1.1 gives
   @@iterator to any interface with an indexed getter and an integer `length`; `entries`, `keys`, `values` and
   `forEach` are its step 1.2 and come from an `iterable<V>` DECLARATION, which NodeList and DOMTokenList carry and HTMLCollection and
   NamedNodeMap do not. Installing all four unconditionally put `paragraphs.forEach` on an HTMLCollection, which
   WPT asserts is absent — so the interface that declares the iterable is the one that calls this. */
void idl_indexed_install_value_iterator(JSContext *ctx, JSValueConst proto)
{
    static const char *const NAMES[] = { "values", "keys", "entries", "forEach" };
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue arr = JS_GetPropertyStr(ctx, global, "Array");
    JSValue ap = JS_GetPropertyStr(ctx, arr, "prototype");
    unsigned k;

    for (k = 0; k < sizeof(NAMES) / sizeof(NAMES[0]); k++) {
        JSValue f = JS_GetPropertyStr(ctx, ap, NAMES[k]);
        DCHECK(JS_IsFunction(ctx, f), "an Array.prototype iterator member named by §3.7.9 step 1.2 is missing");
        JS_SetPropertyStr(ctx, (JSValue)proto, NAMES[k], f);
    }
    JS_FreeValue(ctx, ap);
    JS_FreeValue(ctx, arr);
    JS_FreeValue(ctx, global);
}
