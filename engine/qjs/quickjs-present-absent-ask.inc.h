static bool js_present_ask(JSContext *ctx, JSObject *holder, JSAtom prop, JSValueConst held)
{
    if (likely(g_concolic.present == NULL) || !js_object_is_document_record(holder))
        return false;
    /* An OBJECT-valued member is not asked, and that is the channel's own shape rather than a carve-out: a
       record hanging off a published record is PUBLISHED IN ITS OWN RIGHT by the same walk, so its members are
       already asked one level down, and its address is what absent.c's registry is keyed by. Minting a fresh
       unknown per read would answer `gon.user === gon.user` false, hide that registry's key behind a value it
       never filed, and wrap the concolics this engine mints in a second layer of themselves. */
    if (JS_VALUE_GET_TAG(held) == JS_TAG_OBJECT)
        return false;
    if (!JS_AtomIsPublishedName(ctx->rt, prop))
        return false;
    /* AND THE PUBLICATION IS ASKED HERE, WHICH THE MISS PATH'S COPY OF THIS COULD LEAVE TO SOMEBODY ELSE AND
       THIS ONE CANNOT. The walk runs lazily, off a read, and until this arm existed the only read that ran it
       was one that MISSED — which the case this hook is for never performs: `window.__FLAGS={admin:false}`
       followed by `if (__FLAGS.admin)` resolves the global by an own-slot HIT and the member by an own-slot
       HIT, so nothing on that program's path ever falls off a prototype chain and the record would stay
       unpublished for ever while every one of its members read concretely. The walk is entered here for the
       same reason and at the same cost the miss path states: publication is what says whether a record the
       document BUILT is on the server's channel or is merely an object an inline script made and kept, and a
       remembered "not a namespace" is a negative that silently expires the moment the bundle attaches it.
       It is asked LAST — after the cheap bit test, the primitive test and the name test — so a record that
       could not be answered anyway never pays for it. */
    if (!holder->doc_namespace)
        js_publish_document_namespace(ctx);
    return holder->doc_namespace;
}

/* §10.1.8.1 OrdinaryGet ( O, P, Receiver ) step 2.b — "If parent is null, return undefined" — AND THE TWO
   BASES FOR WHICH THAT undefined WOULD BE A FABRICATION. The MISS half of the same channel js_present_ask
   answers the HIT half of, written as its twin so the two cannot come apart: the key rule, the base rule and
   the lazy publication are each stated ONCE for both, and a base added here is a base the key rule already
   covers. It used to be an `if` inside JS_GetPropertyInternal's tail, which is exactly how it came to be the
   arm with no key rule in it — see JS_AtomIsPublishedName for what that cost.
   THE GLOBAL OBJECT, because a miss there is the same question a bare unresolved name asks, reached
   through `window.` instead: server-injected app state is written onto the global, so `window.__FLAGS` and
   `__FLAGS` are one read spelled two ways.
   AND A RECORD THE DOCUMENT PUBLISHED into that namespace, because a server does not only write globals it
   may leave out entirely — far more often it writes `window.gon={}` and then the handful of fields THIS
   visitor is entitled to, and every field the bundle reads and the server did not write is exactly the
   same unknown wearing a present parent. Restricting the rule to the global was not a narrower reading of
   it: `gon.*`, `__INITIAL_STATE__.*`, `__NUXT__.*` and `__APOLLO_STATE__.*` are all records whose FIELDS
   are read off a parent the document did write.
   WHAT IS STILL OUT OF REACH, NAMED SO IT IS NOT MISTAKEN FOR A DECISION: a server that ships its state as
   a `<script type="application/json">` DATA BLOCK — HTML §4.12.1 "The script element": "Setting the
   attribute to any other value means that the script is a data block, which is not processed by the user
   agent, but instead by author script or other tools" — and lets the BUNDLE parse it. That record is
   built by bundle code out of document bytes, so nothing here marks it — the document-built bit is set
   from the code that is running, and a `JSON.parse` in the bundle is bundle code however server-rendered
   its input was. Reaching it needs the DOCUMENT'S OWN TEXT to be a provenance a value can carry from the
   DOM through the parse, which is a different mechanism from this one and not a wider setting of it.
   AND IT DOES NOT ASK WHO IS READING, which is the one narrowing that looks principled and is not. It is
   tempting to say a channel needs both ends — a session-VARIANT producer and a session-INVARIANT consumer
   — and to answer `undefined` when an inline script reads a record its own document published. But the
   fact the answer turns on is WHOSE CHOICE THE EXTENT WAS, and that was the server's whichever half of the
   document reads it: `window.__FLAGS={theme}` followed on the next line by `if(__FLAGS.admin)` is a gate
   over state the server would have written for a logged-in visitor, and suppressing it buries the admin
   code exactly as answering `undefined` on the global did. A reader test would also make the mechanism's
   own fixtures unreachable — a one-document fixture has no external half — which is the tell that it is a
   cost heuristic wearing an argument. §solver: err toward MORE exploration; the WFQ starves the arm that
   emits nothing.
   Returns the unknown, or JS_UNINITIALIZED for "this read is not on the channel" — never JS_UNDEFINED, which
   is a legitimate ANSWER a host may compose and would be indistinguishable from a refusal. */
static JSValue js_absent_ask(JSContext *ctx, JSValueConst obj, JSAtom prop, JSConcolicAbsentAnswer answer)
{
    JSObject *base;
    bool ask;

    if (likely(g_concolic.absent == NULL) || JS_VALUE_GET_TAG(obj) != JS_TAG_OBJECT)
        return JS_UNINITIALIZED;
    /* THE KEY RULE, ASKED BEFORE THE BASE AND FOR THE SAME REASON THE HIT ARM ASKS IT: a well-known symbol is
       the ENGINE's protocol and not the server's channel, and it is the key that misses on EVERY object, so
       this arm is the one that meets it. Cheap, and it is what makes a third base free. */
    if (!JS_AtomIsPublishedName(ctx->rt, prop))
        return JS_UNINITIALIZED;
    base = JS_VALUE_GET_OBJ(obj);
    ask = JS_IsGlobalReceiver(ctx, obj);
    if (!ask && js_object_is_document_record(base)) {
        /* A record whose extent the document's bytes wrote: publication is what says whether it is on the
           CHANNEL or is merely an object an inline script happened to make and keep, and it is asked here
           rather than at the store for the reason js_publish_document_namespace gives. */
        if (!base->doc_namespace)
            js_publish_document_namespace(ctx);
        ask = base->doc_namespace;
    }
    if (!ask)
        return JS_UNINITIALIZED;
    return g_concolic.absent(ctx, obj, prop, answer);
}

/* THE SAME MISS, REACHED BY AN OPERATION THAT PERFORMS NO [[Get]] — and therefore a RECORDING and never an ask.
   ECMAScript §13.5.3 The typeof Operator's §13.5.3.1 Runtime Semantics: Evaluation step 2.a is "If
   IsUnresolvableReference(value) is true, return "undefined".", which settles the operator BEFORE step 2.b's
   `Set value to ? GetValue(value)` — so there is no read to answer, and a host that answered one would make
   step 2.b run and the operator say "object" for a name every browser says "undefined" for. The arm below
   therefore calls this and then pushes exactly what it pushed before.
   IT IS A FUNCTION AND NOT AN `if` AT THE ARM for the reason js_absent_ask is one: the conditions under which
   this file speaks to that host — the hook being installed, and the key rule — are ONE thing to keep in step,
   and a second spelling of them at a call site is a second thing to keep in step. What it does NOT share with
   the ask is the BASE test, and that is not an omission: an unresolvable Reference is a miss whose last link
   WAS the global record, which the arm has already established by reaching it, so there is no base here to
   ask about.
   THE KEY RULE IS ASSERTED RATHER THAN TESTED, which is the one place this differs from js_absent_ask and is a
   fact about the operand rather than a relaxation. The ask answers for a key any [[Get]] can carry, including
   a well-known symbol, which is why it filters; this one is reached only from OP_get_var_undef, whose atom is
   an IDENTIFIER's — §12.7 Names and Keywords gives no IdentifierStart that is a decimal digit, so it is
   neither a symbol nor the canonical numeric string §6.1.7 The Object Type makes a tagged integer atom of.
   A filter here would silently drop a route that should never exist; the assert names it. */
static void js_absent_note_unresolved(JSContext *ctx, JSAtom prop, JSConcolicAbsentOp op)
{
    if (likely(g_concolic.absent_unresolved == NULL))
        return;
    /* THE KEY RULE IS PER OPERATION AND THE TWO ARMS ARE NOT ONE RULE WITH TWO SPELLINGS. What is shared — the
       hook being installed, and the census this speaks to — stays in ONE function for the reason the paragraph
       above gives; what is NOT shared is who chose the key, and that is the difference between an assert and a
       filter rather than a matter of strictness. CLAUDE.md §WHOSE-BYTES-STATE-THE-VALUE: a DCHECK may only ever
       stand on a value this codebase computed, and exactly ONE member's key is one however many members the
       enum grows — which is why the arms are keyed on that member and not on the others. */
    if (op == JS_CONCOLIC_ABSENT_TYPEOF) {
        DCHECK(JS_AtomIsPublishedName(ctx->rt, prop),
               "a `typeof` on an unresolvable name reached the injected-state channel with a key the channel "
               "cannot NAME — this arm is entered only from OP_get_var_undef, whose atom is an identifier's and "
               "therefore a string atom by the grammar, so a symbol or an index here is an opcode routed to the "
               "typeof arm that is not a typeof");
    } else {
        /* AND EVERY OTHER MEMBER'S KEY IS THE PAGE'S, so the same predicate is a FILTER here. §13.10.1's is
           `? ToPropertyKey(leftValue)` and §28.1.8 "Reflect.has ( target, key )"'s step 2 is
           `? ToPropertyKey(key)`; both run the PAGE's own @@toPrimitive over the PAGE's own operand,
           so a symbol is input and not an invariant: `Symbol.iterator in window` is a line any bundle may
           write and a symbol here is that line arriving, never a routing defect. Filtered, it leaves the
           census counting what it can NAME — which is what its own denominator says it counts — and the
           operation's answer is untouched either way. Asserted, it would be a page-held abort switch for the
           whole engine, reached from a feature detect.
           THE ARMS USED TO BE WRITTEN THE OTHER WAY ROUND — `in` named, everything else asserting — and that
           is REWRITTEN rather than deleted because the polarity is the load-bearing half and the retired form
           is the one a reader re-derives from a two-member enum. Keyed on the arm that FILTERS, a member added
           to JSConcolicAbsentOp inherits the ASSERT, which is the page-held abort switch; keyed on the arm
           that ASSERTS, it inherits the FILTER, which records less and crashes nothing. The property being
           asserted is "the key came from the grammar", and exactly ONE member has it. */
        if (!JS_AtomIsPublishedName(ctx->rt, prop))
            return;
    }
    g_concolic.absent_unresolved(ctx, prop, op);
}

/* THE [[HasProperty]] SPELLINGS OF ONE FEATURE DETECT, AND THE ONES THAT REACH NEITHER OF THE OTHER TWO ARMS.
   `window.EventSource` is a [[Get]] that misses the whole chain and asks js_absent_ask; `typeof EventSource`
   is an unresolvable Reference the arm above records. `"EventSource" in window` is ECMAScript §13.10.1
   "Runtime Semantics: Evaluation"'s `RelationalExpression : RelationalExpression in ShiftExpression`, whose
   last step is "Return ? HasProperty(rightValue, ? ToPropertyKey(leftValue))" — §7.3.11
   "HasProperty ( obj, propertyKey )", which performs no [[Get]] anywhere in it — so it reaches no read hook,
   throws nothing, and answers `false` for a name a standard owns and this realm has none of. One question
   about one name, asked three ways, and the census could see two of them.
   IT IS RECORDED AT THE OPERATOR'S PLACEMENT AND NOT AT THE [[HasProperty]] MISS, which is the whole of why
   this is a separate function rather than a line in the generic request. `gp_op = GP_HAS` on the global object
   is ALSO what every identifier resolution issues — the with-machinery raises one for OP_get_var,
   OP_get_var_undef, OP_put_var and the OP_with_* forms — so a recording site down there would count every
   unresolved identifier a second time and the census's three-arm identity would fire on the first document.
   The two are already told apart by the request's own `gp_outer_kind`: identifier resolution is CONT_WITH_HAS
   and a bytecode operator is CONT_OP_KEYED, so `do_opkeyed_place` is a place identifier resolution cannot
   reach. What the placement could NOT say by itself is WHICH operator, since `delete` arrives there with the
   same pop, the same push and, in sloppy code, the same throw_on_false — which is what JSOpKeyed's `op` is.
   `base` and `answer` are the operation's own operand and its own result, both BORROWED.
   AND `op` IS THE CALLER'S TO STATE, WHICH IS WHAT LETS A SECOND SPELLING SHARE THIS FUNCTION RATHER THAN GET
   A SECOND COPY OF ITS TWO GATES. `Reflect.has(window, "X")` is §28.1.8 "Reflect.has ( target, key )"
   step 3's "Return ? target.[[HasProperty]](propertyKey)" — the SAME §7.3.11 the `in` operator ends at, one
   call further out — so the question, the gates and the row are identical and only the ROUTE differs: `in` is a
   bytecode operator recorded at its placement, and `Reflect.has` is a step machine's request, which that
   placement structurally cannot see (CONT_OP_KEYED against CONT_STEP). Two routes, one recorder, and the
   operation named by the party that performed it — the rule JSConcolicAddOp and JSConcolicEqOp already carry,
   for the reason CLAUDE.md §A-FIX-OF-THE-FORM-"X-IS-NOT-HOW-TO-ASK-Q" gives: where a codebase has a canonical
   spelling of a question, a second site ROUTES to it rather than writing a second correct answer.
   WHAT THIS FUNCTION MAY NOT SERVE IS §7.3.12 HasOwnProperty, and that is a fact about the GATES rather than
   about the route — see the residual in engine/host/solver/absent.c. Its "answer is false" gate establishes
   CHAIN-absence for every member that reaches here, which is what the census's owed arm means by a name this
   realm does not answer; own-absence establishes no such thing, because this engine puts Window's inherited
   members on a prototype exactly as Web IDL requires. */
static void js_absent_note_has(JSContext *ctx, JSValueConst base, JSAtom prop, JSValueConst answer,
                               JSConcolicAbsentOp op)
{
    /* THE NAME IS BOUND: not a miss, and nothing to say about it. Read through JS_ToBool rather than off the
       tag because §10.5.7 step 8 hands a Proxy's `has` trap result through ToBoolean, so a trap anywhere on a
       chain is free to have returned something else and the engine's own normalisation is the answer. */
    if (JS_ToBool(ctx, answer))
        return;
    /* AND THE BASE DECIDES WHOSE MISS IT IS. The census's owed arm is about names a standard puts ON THE
       GLOBAL OBJECT, so `"Node" in gon` is a field of an app record that happens to be spelled like an
       interface and suppressing or counting it would be the same defect the read hook's own `is_global` test
       exists to prevent. `window`, `self` and `globalThis` are the realm's [[GlobalThisValue]] and a bare name's
       base is the global object, so JS_IsGlobalReceiver's two objects cover every spelling of the receiver. */
    if (!JS_IsGlobalReceiver(ctx, base))
        return;
    js_absent_note_unresolved(ctx, prop, op);
}

