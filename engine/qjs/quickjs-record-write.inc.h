/* §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ) step 3, "If IsDataDescriptor(propertyDesc) is true,
   return propertyDesc.[[Value]]" — ON A RECORD THE DOCUMENT PUBLISHED, where returning it CONCRETELY is the
   same loss the miss path exists to prevent.
   The tail of this function answers a MISS on such a record symbolically, because a field the server did not
   write for this visitor is unknown rather than `undefined`. A field it DID write is unknown in exactly the
   same way and for the same reason: `window.__FLAGS={admin:false}` is what the server renders for a
   logged-OUT visitor, and `false` is a fact about this session rather than about the program, so deciding
   `if (__FLAGS.admin)` with it deletes the admin arm and every endpoint behind it. That is §solver's trust
   boundary stated for the case it names: a loaded config is opaque for control flow and still carries its
   loaded value as the EXAMPLE.
   ONE BASE, AND IT IS NOT THE MISS PATH'S. The miss asks `base == global || published`, and on a HIT the
   global's answer is normally a BUILTIN — so reusing that predicate would turn `parseInt`, `Object` and every
   intrinsic the page touches into an unknown. A published record's own slots are the server's, and its
   prototype's are Object.prototype's, so the holder is asked and not the receiver.
   WHAT IS OUT OF REACH, NAMED SO IT IS NOT MISTAKEN FOR A DECISION: a server-written SCALAR global,
   `window.isAdmin=false`. Channel membership is a fact about a RECORD — js_document_container_grant grants it to
   the thing whose extent the document's bytes wrote — and a scalar is not a record, so there is nothing here
   to ask about. The missing piece is a mark on that STORE, not a wider predicate here, and widening this one
   to reach it would have to catch every builtin on the global to do it. */
/* THE PROGRAM'S OWN WRITES TO A PUBLISHED RECORD, LATCHED PER MEMBER. A [[Set]] by the page's code on a member of a
   document record records the host's current unit (JSConcolicHooks.unit_seq) under a private name on the record,
   so the latch is a property write the flow's delta captures like the member write it describes, and no
   reflection of the page sees it. A literal's own definition is not a [[Set]] and latches nothing: the served
   value is what the assumption is about. */
static JSAtom js_record_write_key(JSContext *ctx)
{
    JSRuntime *rt = ctx->rt;

    if (rt->record_write_key == JS_ATOM_NULL) {
        JSAtom desc = JS_NewAtom(ctx, "recordWrites");
        JSValue sym;

        CHECK(desc != JS_ATOM_NULL, "OOM naming the published-record write latch");
        sym = JS_NewSymbolFromAtom(ctx, desc, JS_ATOM_TYPE_PRIVATE);
        JS_FreeAtom(ctx, desc);
        CHECK(!JS_IsException(sym), "OOM minting the published-record write latch's private name");
        rt->record_write_key = JS_ValueToAtom(ctx, sym);
        JS_FreeValue(ctx, sym);
    }
    return rt->record_write_key;
}

static void js_record_write_note(JSContext *ctx, JSValueConst obj, JSAtom prop)
{
    JSAtom key;
    JSValue latch;

    if (likely(g_concolic.unit_seq == NULL) || JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT ||
        !js_object_is_document_record(JS_VALUE_GET_OBJ(obj)) || !JS_AtomIsPublishedName(ctx->rt, prop))
        return;
    key = js_record_write_key(ctx);
    latch = JS_GetProperty(ctx, obj, key);
    CHECK(!JS_IsException(latch), "reading a published record's write latch failed");
    if (!JS_IsObject(latch)) {
        JS_FreeValue(ctx, latch);
        latch = JS_NewObjectProto(ctx, JS_NULL);
        CHECK(!JS_IsException(latch), "OOM allocating a published record's write latch");
        CHECK(JS_DefinePropertyValue(ctx, obj, key, js_dup(latch), JS_PROP_WRITABLE) >= 0,
              "attaching a published record's write latch failed");
    }
    CHECK(JS_DefinePropertyValue(ctx, latch, prop, JS_NewFloat64(ctx, (double)g_concolic.unit_seq()),
                                 JS_PROP_C_W_E) >= 0,
          "latching a published record member's program write failed");
    JS_FreeValue(ctx, latch);
}

int JS_RecordMemberWriteSeq(JSContext *ctx, JSValueConst holder, JSAtom prop, uint64_t *seq)
{
    JSValue latch, v;
    double d = 0;

    DCHECK(seq != NULL, "a member's write latch was asked for with nowhere to put it");
    if (ctx->rt->record_write_key == JS_ATOM_NULL || JS_VALUE_GET_TAG(holder) != JS_TAG_OBJECT)
        return 0;
    latch = JS_GetProperty(ctx, holder, ctx->rt->record_write_key);
    if (!JS_IsObject(latch)) {
        JS_FreeValue(ctx, latch);
        return 0;
    }
    v = JS_GetProperty(ctx, latch, prop);
    JS_FreeValue(ctx, latch);
    if (!JS_IsNumber(v)) {
        JS_FreeValue(ctx, v);
        return 0;
    }
    JS_ToFloat64(ctx, &d, v);
    *seq = (uint64_t)d;
    return 1;
}

