/* WHICH HALF OF THE DOCUMENT IS RUNNING — the SESSION-VARIANT half (an inline `<script>`, re-rendered for
 * every request) or the SESSION-INVARIANT one (a fetched subresource, the same bytes for every visitor). See
 * JS_EVAL_FLAG_INLINE_SCRIPT for why those are the two halves.
 *
 * ASKED OF THE BYTECODE ON THE STACK, never of a flag set around an evaluation: this engine's programs suspend
 * at any depth, interleave with siblings and resume in another session, and an inline script's function can be
 * called back by the bundle an hour of scheduler time later, so the only thing that still knows which script a
 * piece of work belongs to is the CODE PERFORMING IT. A call root is skipped (it names the function it is
 * about to call, not an activation of it) and so is any frame with no bytecode — a C builtin ALLOCATING on the
 * page's behalf is doing it FOR the page code beneath it, which is the frame that answers, and that is what
 * makes an inline script's `JSON.parse` a document-built record. No JS frame at all means the work is the
 * host's own C, which is neither half.
 *
 * IT ANSWERS WHICH HALF IS RUNNING AND NOTHING ELSE, which is the whole of what it is FOR and the whole of what
 * it may be asked. Its one caller is js_document_container_grant, at a site that already knows a record's extent
 * is being written; asking it at an ALLOCATION instead — where nothing knows what is being built — is the
 * proxy that put every platform object on the injection channel. */
static bool js_running_code_is_inline_script(JSRuntime *rt)
{
    JSStackFrame *sf;

    for (sf = rt->current_stack_frame; sf != NULL; sf = sf->prev_frame) {
        JSFunctionBytecode *b;
        if (sf->is_call_root)
            continue;
        b = JS_GetFunctionBytecode(sf->cur_func);
        if (b != NULL)
            return b->from_inline_script;
    }
    return false;
}

/* THE ONE GRANT OF INJECTION-CHANNEL MEMBERSHIP — the RULE that decides whether a record is on it, stated
 * positively and asked at the moment the fact exists.
 *
 * THE RULE: a record is on the server's injection channel iff THE DOCUMENT'S OWN BYTES WROTE ITS EXTENT. That
 * is the fact the whole channel rests on — `window.gon={}` followed by the two of the twenty-three fields THIS
 * visitor is entitled to is a member list the SERVER chose against THIS VISITOR'S CREDENTIALS, so a field the
 * bundle reads and the record does not hold is unknown rather than `undefined`. Everything else in this
 * mechanism (the reachability walk, the key rule, the two read arms) narrows that set; nothing widens it.
 *
 * WHY MEMBERSHIP IS GRANTED BY THE OPERATION AND NOT INFERRED FROM THE ALLOCATION. It used to be inferred, by
 * the object allocator, from `class_id == JS_CLASS_OBJECT` plus "an inline script is somewhere on the stack" —
 * and that is a PROXY for the rule rather than the rule. It answers the same for `window.gon={}` and for
 * `var e = new Event("go")`, because a platform constructor allocating an ordinary object on an inline
 * script's behalf looks exactly like an object literal from the stack: this engine's platform objects ARE
 * ordinary objects (Web IDL §2.12 "Objects implementing interfaces" lets an implementation choose, and an
 * ordinary object with a Symbol-keyed slot record is what the components chose), and a step-machine
 * constructor pushes no C frame at all, so neither the class nor the frame chain can tell the two apart.
 *
 * AND A PLATFORM OBJECT ON THIS CHANNEL IS SILENTLY WRONG, WHICH IS WHY THE DIRECTION MATTERS. Its extent is
 * not the document's to choose: Web IDL §3.8 "Platform objects implementing interfaces" — "A JavaScript value
 * value is a platform object if value is an Object and if value has a [[PrimaryInterface]] internal slot" —
 * makes the member list the INTERFACE'S, so a missing member has the answer §10.1.8.1 OrdinaryGet ( O, P,
 * Receiver ) step 2.b gives it and minting an unknown there is the engine failing to know its own state. Worse
 * for this project specifically: a member of an interface this engine HAS NOT BUILT YET must be honestly
 * absent, because that absence is the forcing function that names the component to write — an unknown in its
 * place lets a flow run past a missing capability and report a surface it never reached, which is the same
 * loss this whole file exists to prevent, arriving through the other end of the channel.
 *
 * SO THE GRANT IS A POSITIVE ACT BY THE OPERATION THAT WROTE THE EXTENT, and every one of them is a LITERAL
 * OF THE DOCUMENT'S OWN TEXT: the OBJECT-LITERAL opcode and the LIST opcode running inline-script bytecode,
 * and the two container cases of a `JSON.parse` of text an inline script handed it (a server dump rendered
 * into the page as `window.__STATE__=JSON.parse("…")` is the same fact as the same dump written as a
 * literal). Each is the document's bytes deciding a member list. Nothing else grants, so every platform
 * object, every Web IDL dictionary and every record a C builtin composes is off the channel BY CONSTRUCTION —
 * including the one the next platform component adds, which is the point: an exclusion rule would have had to
 * be REMEMBERED there, and the failure of forgetting it is silent.
 *
 * AND A LIST IS SUCH A CONTAINER, WHICH THIS GRANT REFUSED FOR A REASON THAT BELONGS TO A DIFFERENT QUESTION.
 * `window.__STATE__={"users":[{"id":1}]}` is ONE literal and ONE server's choice, and the element record in it
 * is granted here in its own right — but the ARRAY between them was not, and the walk below descends only
 * through granted containers, so it stopped at `users` and every field of every element answered §10.1.8.1
 * OrdinaryGet ( O, P, Receiver ) step 2.b's `undefined`. That is the DOMINANT shape of the thing this channel
 * exists for — `__NEXT_DATA__`, `__NUXT__`, `__INITIAL_STATE__` and `gon` are trees of records inside lists —
 * and it was lost to ONE BIT ANSWERING TWO QUESTIONS. They are: DID THE DOCUMENT'S BYTES WRITE THIS
 * CONTAINER'S EXTENT, which decides whether the walk may DESCEND through it and name what is inside, and which
 * a list answers YES; and IS A MEMBER THIS CONTAINER DOES NOT HOLD THE SERVER'S SILENCE, which decides whether
 * a READ of it is asked of the channel, and which a list answers NO. So `doc_built` is the first question and
 * js_object_is_document_record asks the second on top of it.
 *
 * WHAT A LIST GRANT CAN COST IS A NAME AND NEVER AN ANSWER, which is why its precision matters less than this
 * one's and is worth stating rather than leaving to be re-derived. OP_array_from serves the array literal, a
 * spread call's argument list and a destructuring rest, and only the first is unambiguously the document
 * writing a list; the other two copy an extent from a SOURCE. But descent publishes nothing this grant has not
 * already admitted in its own right, so a list granted too generously can only cause a record that IS on the
 * channel to be named by the path it was reached at. `[...document.querySelectorAll("a")]` in an inline script
 * is granted and publishes nothing at all: a platform object is not a container this grants.
 *
 * WHAT THAT NARROWING COSTS, NAMED RATHER THAN DISCOVERED. A record an inline script builds some OTHER way —
 * `Object.create(null)`, `new Config()`, `Object.fromEntries(…)` — is no longer on the channel, so a read that
 * misses on it answers `undefined` and a gate over it does not fork. That is a LOST FORK, which the WFQ makes
 * recoverable and an absent @H example makes visible; the other direction is a member answered with an unknown
 * on an object that has a real answer, which throws nothing and is read as state. Both defects that ended a
 * document this session were the second direction, so where this rule is uncertain it refuses.
 *
 * `v` is borrowed. The container KIND is ASSERTED and not filtered: every grant site builds the container
 * itself, so anything else here is a further site that has not read this comment. */
static void js_document_container_grant(JSContext *ctx, JSValueConst v)
{
    JSObject *p;

    /* No host has declared the channel: every conformance runner pays one bit test at the grant sites and
       nothing else — the frame walk this used to do per ALLOCATION is gone. */
    if (likely(g_concolic.publish == NULL))
        return;
    if (!js_running_code_is_inline_script(ctx->rt))
        return;
    DCHECK(JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT,
           "the injection channel was offered membership for something that is not an object — a grant names "
           "the CONTAINER whose extent was just written, and a primitive has no extent to have been chosen");
    p = JS_VALUE_GET_OBJ(v);
    DCHECK(p->class_id == JS_CLASS_OBJECT || p->class_id == JS_CLASS_ARRAY,
           "a grant named something that is neither a record nor a list. A Function or a Date is not a "
           "container of members at all, and a PLATFORM object's extent is Web IDL §3.8's rather than the "
           "document's — so a further grant site has been added for something whose member list the "
           "document's bytes did not write");
    DCHECK(!p->doc_namespace,
           "a container was granted channel membership AFTER the walk had already published it — the two bits "
           "only ever go 0 -> 1 and the publication composes the path this container's members are reported "
           "under, so a grant arriving second means the walk published something it had not yet been told was "
           "a document container");
    p->doc_built = true;
}

/* DID THE DOCUMENT'S OWN BYTES WRITE THIS CONTAINER'S EXTENT — the first of the channel's two questions, and
   the one the WALK asks, because it decides whether a container may be DESCENDED through and what is inside it
   NAMED. The assert is the half that makes it a contract rather than an accessor: membership is handed out by
   js_document_container_grant alone, so every consumer is entitled to the grant's own precondition and a bit
   that arrived some other way says so HERE, at a reader, rather than turning up as a member of a namespace
   that answers a Web IDL member with an unknown. */
static bool js_object_is_document_container(JSObject *p)
{
    DCHECK(!p->doc_built || p->class_id == JS_CLASS_OBJECT || p->class_id == JS_CLASS_ARRAY,
           "a container on the injection channel is neither a record nor a list — js_document_container_grant "
           "is the only thing that grants membership and it asserts the container kind at the grant, so this "
           "bit was set by something that is not the grant");
    return p->doc_built;
}

/* IS A MEMBER THIS CONTAINER DOES NOT HOLD THE SERVER'S SILENCE — the SECOND question, asked by BOTH arms that
   read and by the interpreter fast paths that decide whether a hit may be answered inline. That is the same
   treatment the KEY rule got one question along, and for the same reason: the key rule arrived on one arm and
   the arm without it answered every well-known symbol with a callable unknown, so a rule that decides an
   answer belongs in one function under every site that needs the answer, not in a condition each site spells
   for itself.
   A LIST ANSWERS NO, and that is the whole of the difference between the two questions. ECMAScript §10.4.2
   Array Exotic Objects: "The value of the "length" property is numerically greater than the name of every own
   property whose name is an array index" — so the document that wrote `{"users":[{"id":1}]}` STATED how many
   elements the list has, and `users[7]` is answered by the list rather than left unsaid by the server. Its
   `length` is the same fact and is equally the document's, so a hit on it is the program's constant and not
   this visitor's state. A record has no such self-description: `gon={}` says nothing whatever about the
   twenty-three fields the bundle reads, which is why the channel exists at all. */
static bool js_object_is_document_record(JSObject *p)
{
    return js_object_is_document_container(p) && p->class_id == JS_CLASS_OBJECT;
}

/* ONE MEMBER OF A CONTAINER THE DOCUMENT WROTE, offered to the walk below. Returns false only for the
   worklist's own allocation failure, which is the one thing that ends the walk early.
   IT IS ONE FUNCTION BECAUSE A CONTAINER HAS TWO KINDS OF MEMBER AND EXACTLY ONE RULE. A record's fields and a
   slow list's holes live in the SHAPE; a fast list's elements live in `u.array` and have no shape property at
   all — two storages, and every question below (the key rule, the global, membership, the mark, the
   publication order) is identical for both. Asked once per storage at the caller, this rule would be the KEY
   RULE's own defect one level up: a rule stated twice is a rule one of the two sites is added without.
   THE KEY RULE, ASKED WHERE MEMBERSHIP IS DECIDED. The two read arms ask it of the key being READ; this asks
   it of the key a container is REACHED BY, which is the prior question — whether the container is on the
   channel at all. The difference in what skipping it costs is the whole reason it belongs here: a mis-named
   member answers one read wrongly, while a record published through a key no server can write becomes a
   namespace, after which BOTH arms apply the key rule perfectly correctly to that record's own string-keyed
   fields and every one of them answers with an unknown. Nothing then looks like a channel bug.
   An INTERNAL SLOT — ECMAScript §6.1.7.2 Object Internal Methods and Internal Slots, which Web IDL §3.8
   "Platform objects implementing interfaces" makes a platform object's brand — is exactly that shape in this
   engine: a record of named fields hung off a PRIVATE SYMBOL (engine/host/browser/core/idl_slots.h). So a walk
   that descends through a symbol turns every platform object an inline script leaves on the global into a
   namespace. (The number this paragraph used to carry, §3.7.3, is "Interface prototype object" and says
   nothing about slots — a citation nobody could look up read as one nobody needed to.)
   Measured: `var e = new Event("go")` published its slot record, so `e.type` and `e.eventPhase` answered with
   unknowns spelled `{e.eventSlots.type}` (a provenance composed out of a symbol's DESCRIPTION, which is
   neither unique nor a name), `e.defaultPrevented` answered TRUE on an event nothing had cancelled, and DOM
   §2.7 Interface EventTarget's dispatchEvent(event) method step 1 — "If event's dispatch flag is set, or if
   its initialized flag is not set, then throw an InvalidStateError DOMException" — threw on the FIRST dispatch
   of a freshly constructed event. A symbol is not a name a server published a member under, and it is not a
   name this walk may reach one through either. */
static bool js_publish_document_member(JSContext *ctx, JSObject ***work, int *n, int *cap,
                                       JSObject *parent, JSObject *global, JSAtom name, JSValueConst v)
{
    JSObject *c;

    if (!JS_AtomIsPublishedName(ctx->rt, name))
        return true;
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT)
        return true;
    c = JS_VALUE_GET_OBJ(v);
    /* `window.self`, `window.top` and a page's own `window.app=window` all put the global object inside the
       graph; nothing granted it channel membership, so the test beside this already excludes it, and this says
       so where a reader would otherwise have to reason it out. */
    if (c == global || !js_object_is_document_container(c) || c->doc_namespace)
        return true;
    if (*n == *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        JSObject **w = js_realloc(ctx, *work, sizeof(*w) * (size_t)ncap);
        /* OUT OF MEMORY LEAVES A SHALLOWER NAMESPACE, NOT A BROKEN ONE: nothing below has been marked or
           published, so the engine's mark and the host's registry still agree exactly, and the next miss walks
           again. The exception is dropped because this is a READ's bookkeeping, not the read — the property
           answer owes the page nothing about it. */
        if (!w) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return false;
        }
        *work = w;
        *cap = ncap;
    }
    c->doc_namespace = true;
    g_concolic.publish(ctx, JS_MKPTR(JS_TAG_OBJECT, parent), name, v);
    (*work)[(*n)++] = c;
    return true;
}

/* THE DOCUMENT'S PUBLISHED NAMESPACE — every container whose EXTENT THE DOCUMENT'S OWN BYTES WROTE
 * (js_document_container_grant is what decides that, and it is the only thing that does) that is REACHABLE
 * FROM THE GLOBAL OBJECT through such containers, named by the path it is reachable at. That is the one
 * channel a server has for injecting per-visitor state into a bundle it ships to everybody unchanged, and
 * every half of the sentence is load-bearing.
 *
 * WHY THIS IS NOT "EVERY MISSING PROPERTY". §10.1.8.1 OrdinaryGet ( obj, propertyKey, receiver ) step 2.b's
 * `undefined` is the right answer almost everywhere, because almost everywhere the PROGRAM decided the extent:
 * `({}).foo` is undefined and the code that built the object is the code that says so. It is the wrong answer
 * exactly where the extent was decided by the SERVER — `window.gon={}` followed by the two of the twenty-three
 * fields this visitor is entitled to — and a record an inline script builds and keeps to itself is not on that
 * channel: the text that decided its extent is text this run holds, so nothing about it is unknown.
 *
 * WHY IT IS ASKED OF THE HEAP AND NOT OF THE STORE. `window.gon={}`, `var gon={}`, `gon={}`,
 * `Object.assign(window,{gon:{}})` and a getter that returns the record are one fact and five operations, and
 * a question asked at the operations is a question the sixth spelling is added without — the engine's own
 * global-var store does not even reach §10.1.9.2. So the operations are not watched at all: the RESULT is, by
 * a walk from the global object, and a record that was never attached to it is never published however it was
 * built.
 *
 * WHY IT DESCENDS ONLY THROUGH CONTAINERS THE DOCUMENT'S BYTES WROTE. A server dump is a tree written as one
 * literal (`window.__STATE__={"user":{"id":1}}`), so a one-level rule would leave every nested record answering
 * `undefined` for fields a logged-in visitor's tree carried. But the global reaches the whole heap, so the
 * descent has to stop somewhere, and the honest stop is the document's own tree: a bundle record that happens
 * to contain a document record is reached by a path the BUNDLE composed, not by a name the server published.
 * That also keeps the walk proportional to what the document injected rather than to the heap.
 *
 * AND A LIST IS ONE OF THOSE CONTAINERS, which is a fact about the DESCENT and not about the ask —
 * js_object_is_document_record is where the two questions part. A server's tree is records inside lists inside
 * records (`{"users":[{"id":1}]}`), so a walk that stops at every list stops at the first one and leaves the
 * whole subtree under it answering `undefined`. It descends through a list's ELEMENTS, which is the same
 * member question its fields ask and is asked by the same function.
 *
 * THE WALK IS ITERATIVE because a server's tree is as deep as the server chose and the C stack is a non-limit
 * in this engine, and the PUBLICATION MARK is its own visited set, so a cycle terminates and no container is
 * published twice — including a list that holds itself, which is one `JSON.parse` reviver away. Parents are
 * published before their children, which is what lets the host compose a child's path out of its parent's.
 *
 * THE WORKLIST HOLDS RAW `JSObject *` AND THAT IS AN ARGUMENT, not an oversight: js_trigger_gc is called from
 * exactly one place in this file — the OBJECT allocator — and nothing this walk or the host's hook does
 * allocates an object (the hook dups the global, spells an atom into a C string, and mallocs), so no
 * collection can run between pushing a record and popping it. A step added here that allocates an object
 * breaks that and must root the worklist instead.
 *
 * WHEN IT RUNS: on a read that MISSES on a document-built record that is not yet published — see the tail of
 * JS_GetPropertyInternal. Re-walking on a miss that finds nothing is the cost of not caching a NEGATIVE: a
 * record the bundle attaches to the global later must start answering as a namespace then, and a remembered
 * "not a namespace" would be a fact that silently expires. */
static void js_publish_document_namespace(JSContext *ctx)
{
    JSObject **work = NULL;
    JSObject *global = JS_VALUE_GET_OBJ(ctx->global_obj);
    JSObject *p = global;
    int n = 0, cap = 0;

    for (;;) {
        JSShape *sh = p->shape;
        JSShapeProperty *prs;
        int i;

        for (i = 0, prs = get_shape_prop(sh); i < sh->prop_count; i++, prs++) {
            if (prs->atom == JS_ATOM_NULL)
                continue;
            /* An accessor, a var ref or an autoinit slot holds no container here: reading it RUNS something,
               and what it would answer is not a value the document wrote into this tree. Asked at this
               storage and not in the member rule because it is a fact about the SHAPE — a fast list's
               elements have no flags to ask it of. */
            if ((prs->flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
                continue;
            if (!js_publish_document_member(ctx, &work, &n, &cap, p, global, prs->atom, p->prop[i].u.value))
                goto done;
        }
        /* THE SECOND STORAGE. A list the document wrote holds its elements in `u.array` while it is dense, and
           in the SHAPE the moment a hole or a non-index key converts it — so both loops are the same list on
           two different days and the shape loop above already covers the converted one.
           THE CONTAINER KIND IS ASSERTED HERE RATHER THAN FILTERED FOR, because `fast_array` is also set for
           `arguments` and for every typed array and a test that quietly skipped those would be a filter past
           a broken invariant: nothing but the global and a GRANTED container is ever on this worklist, and the
           grant admits only a record or a list. A day this fires is a day something else was granted. */
        DCHECK(p == global || js_object_is_document_container(p),
               "the publication walk reached an object the grant never admitted — every entry on the worklist "
               "is a container js_document_container_grant named, so a stranger here means the descent test "
               "and the grant disagree about what is on the channel");
        if (p->fast_array) {
            uint32_t k;

            DCHECK(p->class_id == JS_CLASS_ARRAY,
                   "a container on the injection channel keeps element storage and is not a list — "
                   "`arguments` and every typed array also carry `fast_array`, and neither is a container "
                   "this channel grants");
            /* §10.4.2 Array Exotic Objects bounds the index: `count` is the element storage's own length,
               which the struct declares as <= 2^31-1, so every index is a tagged-integer atom and no atom is
               interned — which is what keeps this walk's raw-`JSObject *` worklist sound. */
            DCHECK(p->u.array.count <= JS_ATOM_MAX_INT,
                   "a document-written list is longer than the tagged-integer atom space, so its elements "
                   "cannot be named without interning an atom — which this walk's unrooted worklist forbids");
            for (k = 0; k < p->u.array.count; k++)
                if (!js_publish_document_member(ctx, &work, &n, &cap, p, global,
                                                __JS_AtomFromUInt32(k), p->u.array.u.values[k]))
                    goto done;
        }
        if (n == 0)
            break;
        p = work[--n];
    }
done:
    js_free(ctx, work);
}

/* IS THIS KEY A NAME A SERVER PUBLISHED A MEMBER UNDER? The publication channel is a document writing a RECORD
   OF FIELDS — `window.gon={current_user_id:7}`, a JSON dump rendered into the page — so its keys are strings
   and array indices. A SYMBOL is not on that channel and must not be read as if it were: its path would be
   composed out of a description that is neither unique nor a name (`Symbol()` twice spells one path for two
   keys), and the well-known ones are the engine's own protocol, so answering @@toPrimitive or @@iterator with
   an unknown replaces a slot the interpreter is about to CALL with a value that is not callable.
   THAT LAST SENTENCE WAS TRUE OF THE HIT ARM AND OF NOTHING ELSE, AND THE MISS ARM COST THE COERCION SURFACE
   FOR IT. This predicate arrived with the HIT half of the channel and was asked only there, while the MISS
   half — older, and the arm every well-known-symbol read actually takes, since the engine's protocol keys are
   exactly the ones no object holds — asked nothing. §7.1.1 ToPrimitive ( input [ , preferredType ] ) step 1.a
   is `? GetMethod(input, %Symbol.toPrimitive%)`, which falls off the end of the prototype chain on EVERY
   object; on the global and on any published record the channel then answered with a CALLABLE unknown, step
   1.b.iv called it, and step 1.b.vi ("If result is not an Object, return result. Throw a TypeError") threw.
   The reproduction is two tokens wide and worth keeping: `var b={}; 1 & b` and `1 & globalThis` both died
   with `TypeError: toPrimitive`, so EVERY coercion of EVERY object a document reached through a global ended
   that document — while the inline `1 & {}` beside it was fine, which is why the failure read as an operator
   bug and sent readers into the coercion trampoline rather than into a property read.
   So the ASK is one function per arm and the KEY RULE is one function under both (js_absent_ask /
   js_present_ask below) AND under the WALK that decides membership (js_publish_document_namespace above,
   where the same omission cost the whole internal-slot surface rather than one member), and it is EXPORTED
   because the host is the channel's other end: absent.c composes a PATH out of this key, so it asserts the
   rule rather than trusting it — a third base added to the ask later
   (the §4.12.1 data block the miss arm's comment names) then carries the rule for free, and a route that
   bypasses the ask fires in the host instead of arriving as a plausible member of a namespace. */
bool JS_AtomIsPublishedName(JSRuntime *rt, JSAtom atom)
{
    if (__JS_AtomIsTaggedInt(atom))
        return true;
    return rt->atom_array[atom]->atom_type == JS_ATOM_TYPE_STRING;
}

/* WHICH OF THE TWO KEY KINDS THE RULE ABOVE ADMITS — see quickjs.h for why a host may not answer this from the
   key's spelling. It is the engine's own canonicalisation and not a test over characters: an array index is
   interned AS an integer atom (JS_NewAtom folds a canonical numeric string into one), so "0" and 0 are one
   key and "01" is a name, and the tag is that decision already made. */
bool JS_AtomIsIndexName(JSAtom atom)
{
    return __JS_AtomIsTaggedInt(atom);
}

