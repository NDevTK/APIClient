/* THE INTERFACE'S OWN NAME, FOR AN ASSERT THAT WOULD OTHERWISE NAME A HUNDRED COMPONENTS AT ONCE.
   The accessor assertions below identify a read by the RECEIVER's class and the HOLDER's class, and that
   argument holds only while a component has a JSClassID of its own. It does not hold for the shape a Web IDL
   interface normally has here: an interface prototype object is an ORDINARY object, so its class is `Object`,
   and every interface built on a shared substrate shares the substrate's class on the instance side too. The
   measured case is Web IDL §3.9 Legacy platform objects' indexed getter — one `IndexedProperties` class backs
   ten interfaces (FileList, DOMStringList, NodeList, HTMLCollection, NamedNodeMap, DOMTokenList, DOMRectList,
   MediaList, CSSRuleList, StyleSheetList), each of which puts `length` on its own prototype — so a C read of
   `length` on any of them printed receiver `IndexedProperties`, holder `Object`, and ten unrelated components
   were one indistinguishable line. That is precisely the defect the `name`-on-four-objects comment below
   describes, reproduced one layer along inside the message written to prevent it.
   SO THE CLASS STRING IS ASKED, BECAUSE WEB IDL PUTS THE IDENTITY THERE. §3 JavaScript binding states that an
   object with a class string "must, at the time it is created, have a property whose name is the
   %Symbol.toStringTag% symbol with PropertyDescriptor{[[Writable]]: false, [[Enumerable]]: false,
   [[Configurable]]: true, [[Value]]: classString}", and §3.7.3 Interface prototype object states that "the
   class string of an interface prototype object is the interface's qualified name". So the holder of an IDL
   accessor carries the name of the one component that installed it, and the receiver's prototype carries the
   name of the interface the instance implements.
   IT RUNS NO PAGE CODE, AND EVERY CLAUSE HERE IS WHAT MAKES THAT TRUE rather than a hope. This is an abort
   path: a diagnostic that reached the page would re-enter the very walk that is aborting, and the crash would
   then report a state its own reporting created. So the slot is read the way the assert below already tells
   its reader to read one — off the shape, never through a [[Get]]. find_own_property is a hash lookup in the
   shape and calls nothing; a Proxy holds no own properties at all, so one answers "absent" here without its
   trap running; a property that is not a plain data slot (an accessor, a var_ref, an autoinit) is REFUSED and
   said so rather than instantiated or called; and the walk is ONE step to the prototype and no further,
   because every interface prototype on a chain carries its OWN tag and a second step would report an ancestor
   interface's name as this object's — a wrong answer, not a partial one.
   AND THE DESCRIPTOR IS CHECKED, BECAUSE THE PROPERTY IS [[Configurable]]. A page can redefine it, which would
   make this an authoritative-sounding wrong address — the failure this project calls worse than none. §3's
   descriptor is exactly { configurable } with writable and enumerable false, so a tag whose flags are not that
   one is reported as redefined and the reader knows to trust the file:line and the class names instead.
   DEV-ONLY, like every one of its callers: it exists to give an abort an address, and a release build has no
   abort to give one to. */
#if APICLIENT_DEV
static const char *js_why_class_string(JSContext *ctx, JSObject *p, char *buf, size_t n)
{
    JSShapeProperty *prs;
    JSProperty *pr;
    JSString *str;
    uint32_t i;
    int step;

    (void)ctx;
    if (!p)
        return "(not an object)";
    for (step = 0; step < 2; step++, p = p->shape->proto) {
        if (!p)
            return "(no %Symbol.toStringTag% on it or its prototype)";
        prs = find_own_property(&pr, p, JS_ATOM_Symbol_toStringTag);
        if (!prs)
            continue;
        if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
            return "(its %Symbol.toStringTag% is not a data property — NOT READ, because reading one from an "
                   "abort path would run the page's code)";
        if (JS_VALUE_GET_TAG(pr->u.value) != JS_TAG_STRING)
            return "(its %Symbol.toStringTag% is not a plain string value, so it is not a Web IDL class "
                   "string — a rope is refused here too rather than flattened, because flattening allocates "
                   "on a path that is already aborting)";
        str = JS_VALUE_GET_STRING(pr->u.value);
        if (str->is_wide_char)
            return "(its %Symbol.toStringTag% is not ASCII, so it is not a Web IDL qualified name)";
        /* string_get, not a direct str8 index: a NORMAL, a SLICE and an INDIRECT string all answer through it
           and none of them allocates or calls anything. */
        for (i = 0; i < str->len && i + 1 < n; i++)
            buf[i] = (char)string_get(str, (int)i);
        buf[i] = '\0';
        /* §3's descriptor is { [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }. */
        if ((prs->flags & (JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE)) != JS_PROP_CONFIGURABLE)
            snprintf(buf + i, n - i, " [REDEFINED — not §3's class-string descriptor, so this name is the "
                                     "page's claim rather than the engine's]");
        return buf;
    }
    return "(no %Symbol.toStringTag% on it or its prototype)";
}
#endif /* APICLIENT_DEV — js_why_class_string */

/* THE ONE PLACE A READ THAT REACHES THE PAGE'S CODE IS DISCOVERED, so `file`/`line` are the READER's — captured
   by the macro over every public spelling (see the contract above JS_GetPropertyAt in quickjs.h). They are read
   only by the two assertions in the accessor arms, which a release build compiles out; the parameters stay in
   the signature there so ONE argument list serves both builds.
   The INTERPRETER's own routed reads pass js_read_site_routed, naming their file, and their __LINE__: reaching an
   accessor from one of those is not an unrouted caller at all but the read opcode's tramp_accessor_getter walk
   disagreeing with this one about a single object, which is a different bug with a different fix — and the
   line still says WHICH opcode disagreed, which is the half of the address that is still worth having.
   Stating that as a positive value rather than a NULL is the point: a hole here would be indistinguishable
   from a caller written against the raw entry, which is the defect a defaulted field always is. */
#define JS_READ_SITE_ROUTED_TAG "the interpreter's own routed read at "
#define js_read_site_routed (JS_READ_SITE_ROUTED_TAG __FILE__)
static inline bool js_read_site_is_routed(const char *file)
{
    return strncmp(file, JS_READ_SITE_ROUTED_TAG, sizeof(JS_READ_SITE_ROUTED_TAG) - 1) == 0;
}
static JSValue JS_GetPropertyInternal(JSContext *ctx, JSValueConst obj,
                                      JSAtom prop, JSValueConst this_obj,
                                      bool throw_ref_error,
                                      const char *file, int line)
{
    JSObject *p;
    JSProperty *pr;
    JSShapeProperty *prs;
    uint32_t tag;

    (void)file; (void)line;
    DCHECK(!g_compile_stint,
           "a [[Get]] ran inside a compile stint — a compile reads no state of any world; the parse product is "
           "built from the source bytes alone");
    DCHECK(file != NULL,
           "a property read reached JS_GetPropertyInternal with no call site — every public spelling is a macro "
           "that captures __FILE__/__LINE__ and every internal caller passes js_read_site_routed, so a NULL "
           "means a caller was written against the raw entry and the assertions in the accessor arms below "
           "would name nothing");
    tag = JS_VALUE_GET_TAG(obj);
    if (unlikely(tag != JS_TAG_OBJECT)) {
        switch(tag) {
        case JS_TAG_NULL:
            return JS_ThrowTypeErrorAtom(ctx, "cannot read property '%s' of null", prop);
        case JS_TAG_UNDEFINED:
            return JS_ThrowTypeErrorAtom(ctx, "cannot read property '%s' of undefined", prop);
        case JS_TAG_EXCEPTION:
            return JS_EXCEPTION;
        case JS_TAG_STRING:
            {
                JSString *p1 = JS_VALUE_GET_STRING(obj);
                if (__JS_AtomIsTaggedInt(prop)) {
                    uint32_t idx, ch;
                    idx = __JS_AtomToUInt32(prop);
                    if (idx < p1->len) {
                        ch = string_get(p1, idx);
                        return js_new_string_char(ctx, ch);
                    }
                } else if (prop == JS_ATOM_length) {
                    return js_int32(p1->len);
                }
            }
            break;
        case JS_TAG_STRING_ROPE:
            {
                JSStringRope *r = JS_VALUE_GET_STRING_ROPE(obj);
                if (__JS_AtomIsTaggedInt(prop)) {
                    uint32_t idx, ch;
                    idx = __JS_AtomToUInt32(prop);
                    if (idx < r->len) {
                        ch = string_rope_get(obj, idx);
                        return js_new_string_char(ctx, ch);
                    }
                } else if (prop == JS_ATOM_length) {
                    return js_int32(r->len);
                }
            }
            break;
        default:
            break;
        }
        /* cannot raise an exception */
        p = JS_VALUE_GET_OBJ(JS_GetPrototypePrimitive(ctx, obj));
        if (!p)
            return JS_UNDEFINED;
    } else {
        p = JS_VALUE_GET_OBJ(obj);
    }

    for(;;) {
        prs = find_own_property(&pr, p, prop);
        if (prs) {
            /* found */
            if (unlikely(prs->flags & JS_PROP_TMASK)) {
                if ((prs->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
                    if (unlikely(!pr->u.getset.getter)) {
                        return JS_UNDEFINED;
                    } else if (js_cfunc_no_user_code(pr->u.getset.getter)) {
                        /* A GETTER THAT DECLARED IT RUNS NONE OF THE PAGE'S CODE, answered where it is asked.
                           THIS IS NOT THE DELETED FALLBACK COMING BACK, and the test that separates them is
                           mechanical: delete the arm below and this one is still needed, because the read must
                           produce a value and nothing else will produce it. It is one of two algorithms the
                           callee kind distinguishes, not a softer path chosen when the hard one is awkward.
                           WHAT THE ARM BELOW IS ABOUT DOES NOT EXIST HERE. Routing exists so that a body which
                           runs the page gets a flow base to park on; a body that runs none has no continuation
                           to hold, no back-edge to preempt and no page frame to suspend — and the routed path
                           would reach THIS SAME C function through THIS SAME js_call_c_function, so routing it
                           is not a stronger answer, it is the identical call with a tramp frame around it. An
                           abort here fires where nothing is wrong, and a crash that fires where nothing is
                           wrong is how the next reader learns to distrust the ones that matter.
                           THE CLASS CALL HOOK, NOT JS_CallInternal. The interpreter is not entered for a C
                           callee and saying so is worth a line: JS_CallInternal's own non-bytecode arm does
                           exactly this dispatch, and going through it would put JS_GetPropertyInternal back
                           into the interpreter cycle that deleting the old JS_CallFree removed. The hook still
                           owns everything that makes the call correct — §3.7 answers a per-realm fact from the
                           callee's OWN realm, which js_call_c_function does with `ctx = p->u.cfunc.realm`, and
                           it pushes the JSStackFrame a backtrace needs.
                           THE RECEIVER IS `this_obj`, which is step 7's `receiver` and not `obj`: the getter is
                           normally found on a PROTOTYPE and the value belongs to the instance the read was made
                           on. NOTHING IS RAISED HERE: the declaration is checked inside js_call_c_function, for
                           the length of the body and for every way of reaching it, so this arm has one job. */
                        JSObject *g = pr->u.getset.getter;
                        DCHECK(ctx->rt->class_array[g->class_id].call == js_call_c_function,
                               "a declared no-user-code getter is not dispatched by js_call_c_function — the "
                               "declaration asserts a C-FUNCTION callee, so a different call hook here means "
                               "the class table and JS_DeclareCFunctionNoUserCode disagree about what it is");
                        return js_call_c_function(ctx, JS_MKPTR(JS_TAG_OBJECT, g), this_obj, 0, NULL, 0);
                    } else {
                        /* THE GETTER RUNS PAGE CODE, and running it from here would run it in a C activation
                           with no flow base — a loop in the body drives to completion instead of parking. The
                           read OPCODES route it instead: tramp_accessor_getter finds the same getter this walk
                           finds, for ANY callable, and calls it on the tramp chain as a 0-arg method call.
                           The JS_CallFree that used to be here was measured over the whole corpus behind a
                           DCHECK and never reached, so it is deleted rather than left as a fallback. */
                        /* NAME THE PROPERTY *AND WHOSE*. The atom alone is not enough to identify the read, and
                           that shortfall cost a real session: `name` is an accessor on FOUR unrelated things in
                           this engine — Window (§7.1's `window.name`), DOMException, Attr, and any page object
                           whose config the browser half reads — so four completely different bugs printed the
                           identical line, and the search the atom was added to prevent happened anyway, one
                           layer in. The RECEIVER'S CLASS plus the HOLDER'S CLASS narrow it to one component: a
                           `name` on JS_CLASS_DOMEXCEPTION is an error-report path, on the global it is an
                           attacker-source read, on JS_CLASS_OBJECT it is a page config.
                           The two classes are separate because the getter is normally found on a PROTOTYPE, so
                           the holder names the interface and the receiver names the instance the read was
                           actually made on — and when they differ, that difference is the identification. And
                           WHICH-OF-THE-TWO is DIAGNOSTIC ONLY: tramp_accessor_getter walks own slot and
                           prototypes with one rule and routes either the same way, so nothing about the route
                           turns on it. It is the identification that turns on it, which is why both are named.
                           AND THE TWO CLASSES ARE NOT ENOUGH, WHICH IS THE SAME DEFECT ONE LAYER ALONG. A Web
                           IDL interface prototype object is an ORDINARY object, so its class is `Object` for
                           every interface in the engine, and an instance built on a shared substrate carries
                           the substrate's class rather than its own — so the pair narrows to one component
                           only where a component has a JSClassID to itself. Measured on §3.9 Legacy platform
                           objects' indexed getter: ONE `IndexedProperties` class backs ten interfaces, each
                           with `length` on its own `Object`-classed prototype, so a C read of `length` on any
                           of them printed one identical line for ten unrelated files. The CLASS STRING is what
                           separates them — see js_why_class_string, which reads §3's %Symbol.toStringTag% off
                           the shape and runs none of the page's code.
                           AND THE CALLER IS IN THE REPORT NOW, WHICH IT COULD HAVE BEEN ALL ALONG. The line
                           that stood here said it could not be — "the wasm stack is function indices" — and
                           that is a claim about the NATIVE stack, which nothing here was ever going to walk.
                           The two addresses that matter are both reachable: the C site travels with the request
                           (quickjs.h's macro over every public spelling), and js_why_backtrace renders the JS
                           frames without running a byte of the page's code. A crash whose remedy is "route this
                           site" and whose text is identical wherever it fires is one nobody can act on. */
#if APICLIENT_DEV
                        {
                            char pbuf[ATOM_GET_STR_BUF_SIZE], hbuf[ATOM_GET_STR_BUF_SIZE];
                            char rbuf[ATOM_GET_STR_BUF_SIZE], why[4096];
                            char rtag[128], htag[128];
                            char frames[2048], site[320];
                            JSRuntime *rt0 = ctx->rt;
                            const char *rcls = "a primitive";
                            bool routed = js_read_site_is_routed(file);
                            JSObject *robj = JS_VALUE_GET_TAG(obj) == JS_TAG_OBJECT
                                             ? JS_VALUE_GET_OBJ(obj) : NULL;
                            if (robj)
                                rcls = JS_AtomGetStr(ctx, rbuf, sizeof(rbuf),
                                                     rt0->class_array[robj->class_id].class_name);
                            snprintf(site, sizeof site, "%s:%d", file, line);
                            js_why_backtrace(ctx, frames, sizeof frames);
                            snprintf(why, sizeof why,
                                     "A property READ reached a getter for `%s` on %s / %s (the accessor is "
                                     "held by %s / %s), asked for by the C READER at %s. §10.1.8.1 OrdinaryGet "
                                     "( obj, propertyKey, receiver ) step 7 is `Return ? Call(getter, "
                                     "receiver)` — the "
                                     "page's function, and a C activation has no flow base under it, so a loop "
                                     "in that body drives to completion instead of parking. This is a "
                                     "capability this engine does not have rather than a case to handle; the "
                                     "C-drives-JS body was DELETED, not left behind an assert. %s The page's "
                                     "call into this reader: %s",
                                     JS_AtomGetStr(ctx, pbuf, sizeof(pbuf), prop), rcls,
                                     js_why_class_string(ctx, robj, rtag, sizeof rtag),
                                     JS_AtomGetStr(ctx, hbuf, sizeof(hbuf),
                                                   rt0->class_array[p->class_id].class_name),
                                     js_why_class_string(ctx, p, htag, sizeof htag),
                                     site,
                                     routed
                                     ? "SO THIS IS NOT AN UNROUTED READER: the opcode already asked "
                                       "tramp_accessor_getter and was told there was no accessor, so that walk "
                                       "and this one disagree about THIS object — fix the walk, not a call site."
                                     : "FIX IT AT THAT FILE:LINE, not here. ASK FIRST WHETHER THE SITE SHOULD "
                                       "BE READING AT ALL: an engine-owned array's element is the engine's own "
                                       "storage and a host DIAGNOSTIC must reach no page code at all, so both "
                                       "read the slot off the shape rather than performing a [[Get]]. If the "
                                       "site GENUINELY needs the property, ROUTE IT: a step machine already on "
                                       "the chain issues the keyed request (gp_op = GP_GET, gp_outer_kind = "
                                       "CONT_STEP) and takes the value as its next step's result, which is the "
                                       "same request Reflect.get issues and the same one the read opcodes make.",
                                     frames);
                            DFAIL(why);
                        }
#endif
                        return JS_ThrowTypeError(ctx, "getter");
                    }
                } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_VARREF) {
                    JSValue val = *pr->u.var_ref->pvalue;
                    if (unlikely(JS_IsUninitialized(val)))
                        return JS_ThrowReferenceErrorUninitialized(ctx, prs->atom);
                    return js_dup(val);
                } else if ((prs->flags & JS_PROP_TMASK) == JS_PROP_AUTOINIT) {
                    /* Instantiate property and retry */
                    if (JS_AutoInitProperty(ctx, p, prop, pr, prs))
                        return JS_EXCEPTION;
                    continue;
                }
            } else {
                /* THE HIT HALF of the two this read answers — see js_present_ask for which base is asked and
                   why it is not the miss path's. `pr->u.value` is BORROWED across the hook: the slot holds the
                   reference and `obj` holds `p`, and the hook allocates but runs none of the page's code, so
                   nothing can drop either. */
                if (unlikely(js_present_ask(ctx, p, prop, pr->u.value))) {
                    JSValue s = g_concolic.present(ctx, JS_MKPTR(JS_TAG_OBJECT, p), prop, pr->u.value);
                    if (!JS_IsUninitialized(s))
                        return s;
                }
                return js_dup(pr->u.value);
            }
        }
        if (unlikely(p->is_exotic)) {
            /* exotic behaviors */
            if (p->fast_array) {
                if (__JS_AtomIsTaggedInt(prop)) {
                    uint32_t idx = __JS_AtomToUInt32(prop);
                    if (idx < p->u.array.count) {
                        /* THE ELEMENT READ ITSELF, not the general indexed entry that finds its way to it.
                           JS_GetPropertyUint32 -> JS_GetPropertyInt64 tries js_get_fast_array_element and falls
                           back to JS_GetProperty, which is this function — so the ordinary read was in a cycle
                           with itself to answer a case already resolved two lines up: p is a fast array and idx
                           is in range, so the element read succeeds and that fallback is unreachable from here.
                           Same defect as the other "general operation, fixed answer" sites, and the DCHECK is
                           the claim: every class that sets fast_array is one js_get_fast_array_element
                           switches on. */
                        JSValue el;
                        if (js_get_fast_array_element(ctx, p, idx, &el))
                            return el;
                        DFAIL("a fast_array class in range did not answer its own element read — add it to "
                              "js_get_fast_array_element rather than routing the read back through [[Get]]");
                        return JS_UNDEFINED;
                    } else if (is_typed_array(p->class_id)) {
                        return js_ta_get_past_example(ctx, p, idx);
                    }
                } else if (is_typed_array(p->class_id)) {
                    int ret;
                    ret = JS_AtomIsNumericIndex(ctx, prop);
                    if (ret != 0) {
                        if (ret < 0)
                            return JS_EXCEPTION;
                        return JS_UNDEFINED;
                    }
                }
            } else {
                const JSClassExoticMethods *em = ctx->rt->class_array[p->class_id].exotic;
                if (em) {
                    if (em->get_property) {
                        JSValue obj1, retval;
                        /* XXX: should pass throw_ref_error */
                        /* Note: if 'p' is a prototype, it can be
                           freed in the called function */
                        obj1 = js_dup(JS_MKPTR(JS_TAG_OBJECT, p));
                        retval = em->get_property(ctx, obj1, prop, this_obj);
                        JS_FreeValue(ctx, obj1);
                        return retval;
                    }
                    if (em->get_own_property) {
                        JSPropertyDescriptor desc;
                        int ret;
                        JSValue obj1;

                        /* Note: if 'p' is a prototype, it can be
                           freed in the called function */
                        obj1 = js_dup(JS_MKPTR(JS_TAG_OBJECT, p));
                        ret = em->get_own_property(ctx, &desc, obj1, prop);
                        JS_FreeValue(ctx, obj1);
                        if (ret < 0)
                            return JS_EXCEPTION;
                        if (ret) {
                            if (desc.flags & JS_PROP_GETSET) {
                                JS_FreeValue(ctx, desc.setter);
                                JS_FreeValue(ctx, desc.getter);
                                /* THE READ OPCODES ROUTE THIS ONE — tramp_accessor_getter asks the class for
                                   its descriptor and hands the getter to the tramp chain, exactly as it hands
                                   over a shape slot's. So an accessor arriving HERE is a read performed from
                                   C, with no flow base to run the getter on, and it is that CALLER that has an
                                   unbuilt route rather than the class: HTML §7.2.3.5's same-origin WindowProxy
                                   answers with the other document's Window's accessors on purpose. AND THAT
                                   CALLER IS NAMED, for the reason the shape-slot arm above states at length —
                                   "route it" with no address in it is an instruction over hundreds of
                                   spellings, and the site travels with the request precisely so this one can
                                   spend it. */
#if APICLIENT_DEV
                                {
                                    char why[4096], frames[2048], site[320];
                                    char pbuf[ATOM_GET_STR_BUF_SIZE], hbuf[ATOM_GET_STR_BUF_SIZE];
                                    char htag[128];
                                    bool routed = js_read_site_is_routed(file);
                                    snprintf(site, sizeof site, "%s:%d", file, line);
                                    js_why_backtrace(ctx, frames, sizeof frames);
                                    /* THE CLASS STRING IS HERE FOR THE SAME REASON IT IS IN THE SHAPE-SLOT ARM,
                                       and this arm needs it MORE: an exotic class is a SUBSTRATE, so one class
                                       name here stands for every interface built on it by construction rather
                                       than by accident. */
                                    snprintf(why, sizeof why,
                                             "An exotic [[GetOwnProperty]] answered `%s` on %s / %s with an "
                                             "ACCESSOR and the read is being performed from C, asked for by "
                                             "the C READER at %s. §10.1.8.1 OrdinaryGet ( obj, propertyKey, "
                                             "receiver ) step 7 calls that getter and a C activation has no "
                                             "flow base to call it on. tramp_accessor_getter asks the class "
                                             "for the same descriptor and hands the getter to the tramp "
                                             "chain, so the CLASS is not what is missing — the reader is. "
                                             "%s The page's call into this reader: %s",
                                             JS_AtomGetStr(ctx, pbuf, sizeof(pbuf), prop),
                                             JS_AtomGetStr(ctx, hbuf, sizeof(hbuf),
                                                           ctx->rt->class_array[p->class_id].class_name),
                                             js_why_class_string(ctx, p, htag, sizeof htag),
                                             site,
                                             routed
                                             ? "SO THIS IS NOT AN UNROUTED READER: the opcode already asked "
                                               "tramp_accessor_getter and was told there was no accessor, so "
                                               "that walk and this one disagree about THIS object — fix the "
                                               "walk, not a call site."
                                             : "ROUTE IT: a step machine already on the chain issues the "
                                               "keyed request (gp_op = GP_GET, gp_outer_kind = CONT_STEP) and "
                                               "takes the value as its next step's result.",
                                             frames);
                                    DFAIL(why);
                                }
#endif
                                return JS_ThrowTypeError(ctx, "getter");
                            } else {
                                return desc.value;
                            }
                        }
                    }
                }
            }
        }
        p = js_walk_proto(ctx, p);
        if (!p)
            break;
    }
    if (unlikely(throw_ref_error)) {
        return JS_ThrowReferenceErrorNotDefined(ctx, prop);
    }
    /* §10.1.8.1 OrdinaryGet ( O, P, Receiver ) step 2.b, and the two bases on which that `undefined` is a
       fabrication — js_absent_ask, which is the HIT arm's twin and shares its key rule. */
    {
        /* asked of the RECEIVER: a walk resumed at a prototype (a step [[GetOwnProperty]]'s miss) is still the
           receiver's read */
        JSValue a = js_absent_ask(ctx, this_obj, prop, JS_CONCOLIC_ABSENT_ANSWERS_UNDEFINED);
        if (!JS_IsUninitialized(a))
            return a;
    }
    return JS_UNDEFINED;
}

JSValue JS_GetPropertyAt(JSContext *ctx, JSValueConst this_obj, JSAtom prop,
                         const char *file, int line)
{
    return JS_GetPropertyInternal(ctx, this_obj, prop, this_obj, false, file, line);
}

