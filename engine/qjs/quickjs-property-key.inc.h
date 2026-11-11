/* val must be a symbol */
static JSAtom js_symbol_to_atom(JSContext *ctx, JSValueConst val)
{
    JSAtomStruct *p = JS_VALUE_GET_PTR(val);
    return js_get_atom_index(ctx->rt, p);
}

/* return JS_ATOM_NULL in case of exception */
static JSAtom JS_ValueToAtomInternalAt(JSContext *ctx, JSValueConst val,
                                       int flags, const char *file, int line)
{
    JSAtom atom;
    uint32_t tag;
    tag = JS_VALUE_GET_TAG(val);
    if (tag == JS_TAG_INT &&
        (uint32_t)JS_VALUE_GET_INT(val) <= JS_ATOM_MAX_INT) {
        /* fast path for integer values */
        atom = __JS_AtomFromUInt32(JS_VALUE_GET_INT(val));
    } else if (tag == JS_TAG_SYMBOL) {
        JSAtomStruct *p = JS_VALUE_GET_PTR(val);
        atom = JS_DupAtom(ctx, js_get_atom_index(ctx->rt, p));
    } else {
        JSValue str;
        str = JS_ToPropertyKeyInternal(ctx, val, flags, file, line);
        if (JS_IsException(str))
            return JS_ATOM_NULL;
        if (JS_VALUE_GET_TAG(str) == JS_TAG_SYMBOL) {
            atom = js_symbol_to_atom(ctx, str);
        } else {
            atom = JS_NewAtomStr(ctx, JS_VALUE_GET_STRING(str));
        }
    }
    return atom;
}

JSAtom JS_ValueToAtomAt(JSContext *ctx, JSValueConst val, const char *file, int line)
{
    return JS_ValueToAtomInternalAt(ctx, val, /*flags*/0, file, line);
}

/* AN IDENTIFIER REFERENCE'S NAME IS INTERNED, NEVER CONVERTED — it lives beside the coercion family above
   precisely because it must not be one of them. §6.2.5 The Reference Record Specification Type, Table 8,
   says [[ReferencedName]] is "Always a String if [[Base]] value is an Environment Record", and only a
   PROPERTY reference may still hold a value ToPropertyKey has not seen — §6.2.5.1 IsPropertyReference
   ( refRecord ) returns false for BOTH an Environment Record base and an unresolvable one, which is the whole
   of what OP_get_ref_value / OP_put_ref_value are emitted for (get_lvalue reaches them only from
   OP_scope_get_var; a member lvalue becomes OP_get_field / OP_get_array_el instead). So §6.2.5.5 GetValue
   ( refRecord ) step 6 and §6.2.5.6 PutValue ( refRecord, value ) steps 2.c and 6 hand the name straight to
   GetBindingValue / Set / SetMutableBinding, and there is NO ToPropertyKey step anywhere on this path.
   (Both this comment and the crash below cited GetValue's `step 5`, and §6.2.5.5 step 5 is "Assert: base
   is an Environment Record" — it hands the name nowhere. Step 6 is the GetBindingValue that does, and it
   is PutValue's own `6` in this same sentence, for this same role. The citation audit reported NOTHING at
   either site, and the reason is NOT NAMED HERE because it was probed and not found: a flatly impossible
   `step 97` planted here raises no finding either, on one line or two, with or without the argument list
   standing between the term and the step. So the step channel is silent about this site for a cause that
   is still open, and a reader who needs that cause probes the tool rather than trusting this sentence.
   What named the defect was the asymmetry against the counterpart beside it, doing the same job at `6`.)
   A JS_ValueToAtom here was therefore not a redundant conversion but a wrong one, twice over. SPEC-wrong,
   because ToPropertyKey on an object calls the page's toString at a point the algorithm performs no coercion
   at all, so a conforming engine cannot produce that observable. And DIAGNOSTICALLY wrong, because when a
   Function object did reach this slot the abort landed one frame down in JS_ToStringInternal, whose `@WHY`
   accurately describes a coercion site that needs routing to the trampoline — and this site must never be
   routed, because it must never be REACHED. A crash naming a real mechanism in the wrong file is a plausible
   diagnosis, which is worse than silence: it is followed.
   THE PAIR'S WHOLE TYPE CONTRACT IS ASSERTED, NOT JUST THE NAME, because the shape defect this is here to
   catch shifts the operands rather than corrupting one. Every producer pushes a base and then a bytecode
   atom's value — OP_make_var_ref, OP_make_var_ref_ref, OP_make_loc_ref, OP_make_arg_ref and OP_with_make_ref
   all push JS_AtomToValue of the atom they read from `pc`, and resolve_scope_var's named-function-expression
   dummy pushes OP_push_atom_value — over a base that is the binding OBJECT modelling the Environment Record,
   or `undefined` for Table 8's unresolvable. Reading both slots is what tells a wrong NAME from a pair the
   assignment forms' shaping (OP_insert3 / OP_perm4 / OP_rot3l, and optimize_scope_make_ref's in-place rewrite
   of that very triple) left standing one slot out of place. */
static JSAtom js_referenced_name_atom(JSContext *ctx, JSValueConst base,
                                      JSValueConst name)
{
    int ntag = JS_VALUE_GET_TAG(name);
    int btag = JS_VALUE_GET_TAG(base);

    if (likely((ntag == JS_TAG_STRING || ntag == JS_TAG_SYMBOL) &&
               (btag == JS_TAG_OBJECT || btag == JS_TAG_UNDEFINED))) {
        if (ntag == JS_TAG_STRING)
            return JS_NewAtomStr(ctx, JS_VALUE_GET_STRING(js_dup(name)));
        return JS_DupAtom(ctx, js_get_atom_index(ctx->rt,
                                                 (JSAtomStruct *)JS_VALUE_GET_PTR(name)));
    }
#if APICLIENT_DEV
    {
        char why[3072];
        char frames[2048];
        char bdesc[96], ndesc[96];
        char bbuf[ATOM_GET_STR_BUF_SIZE], nbuf[ATOM_GET_STR_BUF_SIZE];
        const char *cn;

        if (btag == JS_TAG_OBJECT) {
            cn = JS_AtomGetStr(ctx, bbuf, sizeof(bbuf),
                               ctx->rt->class_array[JS_VALUE_GET_OBJ(base)->class_id].class_name);
            snprintf(bdesc, sizeof bdesc, "an object of class `%.40s`", cn ? cn : "(unnamed)");
        } else {
            snprintf(bdesc, sizeof bdesc, "a non-object with tag %d", btag);
        }
        if (ntag == JS_TAG_OBJECT) {
            cn = JS_AtomGetStr(ctx, nbuf, sizeof(nbuf),
                               ctx->rt->class_array[JS_VALUE_GET_OBJ(name)->class_id].class_name);
            snprintf(ndesc, sizeof ndesc, "an object of class `%.40s`", cn ? cn : "(unnamed)");
        } else {
            snprintf(ndesc, sizeof ndesc, "a non-key primitive with tag %d", ntag);
        }
        js_why_backtrace(ctx, frames, sizeof frames);
        snprintf(why, sizeof why,
                 "AN IDENTIFIER REFERENCE'S OPERAND PAIR IS NOT A REFERENCE: its base slot holds %s and its "
                 "name slot holds %s. §6.2.5 The Reference Record Specification Type, Table 8, requires "
                 "[[ReferencedName]] to be a String whenever [[Base]] is an Environment Record, and "
                 "§6.2.5.1 IsPropertyReference ( refRecord ) is false for both an Environment Record base and "
                 "an unresolvable one — which is every reference OP_get_ref_value / OP_put_ref_value are "
                 "emitted for. DO NOT MAKE THIS SITE CONVERT: §6.2.5.5 GetValue ( refRecord ) step 6 and "
                 "§6.2.5.6 PutValue ( refRecord, value ) steps 2.c and 6 perform no ToPropertyKey, so a "
                 "coercion here would call the page's toString where the spec calls nothing, and it is what "
                 "used to send this crash one frame down into JS_ToStringInternal — a `@WHY` about routing a "
                 "coercion site, in the wrong file, for an operand that should never have existed. FIX THE "
                 "PRODUCER. The pair is pushed by OP_make_var_ref, OP_make_var_ref_ref, OP_make_loc_ref, "
                 "OP_make_arg_ref, OP_with_make_ref (each: a base, then JS_AtomToValue of the atom at `pc`) "
                 "or by resolve_scope_var's named-function-expression dummy (OP_object / OP_define_field / "
                 "OP_push_atom_value), and it is then reshaped by the assignment forms' OP_insert3 / "
                 "OP_perm4 / OP_rot3l and by optimize_scope_make_ref's in-place rewrite of that triple. A "
                 "base that is neither an object nor undefined, or a name that is neither a String nor a "
                 "Symbol, means one of those left the operand stack a slot out of place — read the two "
                 "descriptions above to tell which slot moved. The page's own frame: %s",
                 bdesc, ndesc, frames);
        DFAIL(why);
    }
#endif
    /* Release owns nothing here — both operands stay the caller's — so the only thing this must not do is
       return a null atom with no exception pending: every caller reads that as `goto exception`. */
    JS_ThrowTypeError(ctx, "an identifier reference's name is not a property key");
    return JS_ATOM_NULL;
}

