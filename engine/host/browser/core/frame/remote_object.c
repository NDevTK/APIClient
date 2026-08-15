/* A REFERENCE TO AN OBJECT IN ANOTHER AGENT — and the one place this engine deviates from HTML's agent model
 * on purpose, so it is worth stating exactly what the deviation is.
 *
 * HTML puts same-origin documents in ONE AGENT CLUSTER, and a browser puts an agent cluster in one heap. That
 * is why `iframe.contentDocument.body` is a POINTER in Chrome and why the spec can write it as a plain
 * property read: the two documents share a heap, so there is nothing to cross. Blink only grows a remote form
 * — RemoteDOMWindow, RemoteFrame — at the CROSS-ORIGIN boundary, and at that boundary the only objects that
 * ever cross are WindowProxy and Location, because cross-origin access exposes nothing else.
 *
 * SECURITY.md puts every DOCUMENT in its own instance, same-origin included, and CLAUDE.md forbids
 * co-locating same-origin documents to avoid the transport. So this engine has same-origin documents in
 * different agents, which HTML does not, and the consequence lands exactly here: a same-origin cross-document
 * read returns an OBJECT, and an object cannot cross as bytes. What crosses is its NAME.
 *
 * WHY IT IS A PROXY. The read has to SUSPEND — the object is in another instance and the answer is not
 * available in this turn — and an exotic [[Get]] hook cannot: it is a C function that must return a value.
 * There is exactly one read shape this interpreter resolves at the OPERATOR SITE and drives on the trampoline,
 * and it is a Proxy trap (see the C-stack rules: every CALL/READ/WRITE/MEMBERSHIP/DELETE through a Proxy is
 * re-dispatched there). So a reference is a Proxy whose traps are STEP MACHINES, and `remote.title` parks the
 * flow at its own call site exactly as an await does.
 *
 * IDENTITY HOLDS ON BOTH SIDES, and neither half is optional. The exporting agent mints ONE id per object, so
 * two asks for the same object answer with the same name; the importing agent keeps ONE reference per (doc,
 * id), so two answers with the same name are the same object here. A page writes `w.document === w.document`
 * and both halves are what makes it true. It holds in BOTH DIRECTIONS for the same reason: an argument that is
 * a local object is EXPORTED rather than serialized, a reference passed as an argument re-emits the name it
 * already carries rather than being lent as a proxy, and a name that comes home resolves to the ORIGINAL —
 * which is what makes `peer.f(document) ; document === theArgumentItSawBack` true.
 *
 * THE FOUR INTERNAL METHODS A REFERENCE PERFORMS ARE ONE MACHINE. [[Get]], [[Set]], [[Delete]] and [[Call]]
 * differ in which operands they encode and nothing else: the park, the world that travels with the question,
 * the transport and the identity are one mechanism, and four copies of it would be four things to get wrong
 * once each. The peer runs a PROGRAM for every one of them (a C activation has no flow base under it, and an
 * IDL accessor or a page's setter is the page's code), and every operand reaches that program through a SLOT
 * rather than being spliced into its text — a property name spliced into a program is a property name read as
 * code.
 *
 * THE ECMASCRIPT POST-CHECKS ARE NOT WRITTEN HERE, AND THAT IS THE POINT. 10.5.8/9/10's invariants run on the
 * TRAP'S RESULT against the proxy's TARGET, and the interpreter's routed keyed entry already performs them for
 * every proxy — so a `set` that reported success against a non-configurable non-writable target property, or a
 * `deleteProperty` that reported a non-configurable property deleted, throws exactly as it does for a page's
 * own handler. That is one implementation and not two. It is also why the target is an EMPTY EXTENSIBLE object
 * (see ref_mint): an empty extensible target constrains nothing, so the peer's answer is never checked against
 * a guess this agent made about a heap it cannot see.
 *
 * WHAT IS NOT BUILT YET is written as a crash rather than a silence, and the list is now the OTHER internal
 * methods: [[GetPrototypeOf]] / [[SetPrototypeOf]] / [[IsExtensible]] / [[PreventExtensions]] /
 * [[GetOwnProperty]] / [[DefineOwnProperty]] / [[HasProperty]] / [[OwnPropertyKeys]] / [[Construct]]. Each
 * needs the same two halves the four built ones needed — a peer-side program under the asking flow's world,
 * and an encoding for whatever the operation carries (a descriptor is a RECORD, not a value) — and each aborts
 * naming itself rather than being answered out of the empty target, which would report a cross-agent object as
 * having no properties, no prototype and no keys.
 *
 * Three more absences, each a crash at its own site rather than a line here. A SYMBOL key is not "unnameable"
 * — it is three encodings nobody has written (well-known, registered, unique), and remote_object_encode says
 * so. A [[Get]] or [[Set]] whose RECEIVER is not the reference itself needs the peer to ask BACK, and this
 * transport runs one way. And an ABRUPT completion has no field in the answer's grammar, so a peer's throw is
 * the peer's crash rather than an `undefined` delivered to a `try`/`catch` that then never runs — the assert
 * for that one is at the peer, in wpt_runner.c, because that is where the completion is produced. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/frame/remote_object.h"
#include "core/idl_args.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/world.h"

/* ---- THIS AGENT AS AN EXPORTER --------------------------------------------------------------------------- */

typedef struct { JSValue v; } Export;
static Export  *g_exports;
static uint32_t g_exports_n, g_exports_cap;

/* THIS AGENT LENDING ONE OF ITS OBJECTS to a peer: a stable id, minted ONCE per object. Stable because the
   peer's reference has to have the identity a page compares — `w.document === w.document` is false the moment
   two asks mint two ids. The table holds a reference, so an exported object outlives whatever else drops it.
   Reached only from the encoder, which is the only way an object ever crosses. */
static uint32_t remote_object_export(JSContext *ctx, JSValueConst v)
{
    uint32_t i;

    DCHECK(JS_IsObject(v), "something that is not an object was exported to another agent — a primitive crosses "
                           "as its value and needs no name");
    /* ONE ID PER OBJECT. A linear scan, because the table is one entry per object a peer has actually asked
       for — the set of objects that cross an agent boundary, not the heap. */
    for (i = 0; i < g_exports_n; i++)
        if (JS_VALUE_GET_PTR(g_exports[i].v) == JS_VALUE_GET_PTR(v)) return i + 1;
    if (g_exports_n == g_exports_cap) {
        uint32_t cap = g_exports_cap ? g_exports_cap * 2 : 16;
        Export *g = realloc(g_exports, cap * sizeof *g);
        CHECK(g != NULL, "remote object: OOM exporting an object — a dropped export is a reference the peer "
                         "holds that names nothing, and every read through it would fail");
        g_exports = g;
        g_exports_cap = cap;
    }
    /* HELD, not borrowed: the peer's reference outlives whatever else in this agent points at the object, and
       an export the collector took away would leave that reference naming freed memory. */
    g_exports[g_exports_n].v = JS_DupValue(ctx, v);
    return ++g_exports_n;
}

JSValueConst remote_object_by_id(uint32_t id)
{
    if (id == 0 || id > g_exports_n) return JS_UNDEFINED;
    return g_exports[id - 1].v;
}

/* ---- THIS AGENT AS AN IMPORTER --------------------------------------------------------------------------- */

/* The (doc, id) a reference names, kept on the PROXY'S TARGET. Not on the handler, and the difference is not
   stylistic: a trap is passed (target, property, receiver) and the target is argv[0] every time, whereas what
   `this` is inside a trap depends on how the interpreter dispatched it. Reading the identity off an argument
   the contract guarantees is the only version that cannot be wrong.
   `callable` is what the peer said about the object, and it rides HERE rather than being inferred because
   nothing in this heap can look at the object to ask. */
typedef struct { uint32_t doc, id; uint8_t callable, constructor; } RefData;

/* TWO TARGET CLASSES FOR ONE KIND OF REFERENCE, because JS_NewProxy reads `JS_IsFunction(target)` and
   `JS_IsConstructor(target)` ONCE, at mint, and a Proxy over a non-callable target has no [[Call]] at all — its
   `apply` trap can never fire and `typeof` answers "object". So a reference to a peer's function is built over
   a target whose class declares a `call`, and the constructor bit is set on top of that for a `c`. The class's
   own call handler is unreachable by construction (a Proxy's target is not script-reachable) and says so. */
static JSClassID g_ref_class, g_ref_fn_class;
static JSValue   g_refs = JS_UNDEFINED;   /* "<doc>:<id>" -> the one reference for it (owned) */

/* WHICH INTERNAL METHOD a reference is performing — the machine's magic, and the index of its verb on the
   wire. One enumeration, so a verb and the operands that follow it cannot be declared apart. */
enum { REF_OP_GET, REF_OP_SET, REF_OP_DELETE, REF_OP_APPLY, REF_OP_N };
static const char *const REF_VERB[REF_OP_N] = { "get", "set", "delete", "apply" };
/* How many operands each trap is passed — 10.5.8's (target, key, receiver), 10.5.9's (target, key, value,
   receiver), 10.5.10's (target, key) and 10.5.12's (target, thisArg, argArray). Declared beside the verb
   because the machine reads operands BY POSITION and a count stated anywhere else is a count that can drift. */
static const int REF_ARGC[REF_OP_N] = { 3, 4, 2, 3 };
static int g_ref_stepid[REF_OP_N] = { -1, -1, -1, -1 };

static JSValue ref_mint(JSContext *ctx, uint32_t doc, uint32_t id, bool callable, bool constructor);

/* The reference identity behind a proxy's TARGET (what a trap is handed as argv[0]). */
static RefData *ref_of_target(JSValueConst target)
{
    RefData *r = JS_GetOpaque(target, g_ref_class);
    return r ? r : JS_GetOpaque(target, g_ref_fn_class);
}

/* The reference identity behind the PROXY itself (what a page holds, and what a trap's receiver is). */
static RefData *ref_of_proxy(JSContext *ctx, JSValueConst proxy)
{
    JSValue t;
    RefData *r;

    if (!JS_IsProxy(proxy)) return NULL;
    /* JS_GetProxyTarget DUPS, and this is a query — so the reference it takes is released here. */
    t = JS_GetProxyTarget(ctx, proxy);
    r = ref_of_target(t);
    JS_FreeValue(ctx, t);
    return r;
}

bool remote_object_is(JSContext *ctx, JSValueConst v) { return ref_of_proxy(ctx, v) != NULL; }

uint32_t remote_object_doc(JSContext *ctx, JSValueConst v)
{
    RefData *r = ref_of_proxy(ctx, v);
    DCHECK(r != NULL, "the document of something that is not a cross-agent reference was asked for");
    return r->doc;
}

uint32_t remote_object_id(JSContext *ctx, JSValueConst v)
{
    RefData *r = ref_of_proxy(ctx, v);
    DCHECK(r != NULL, "the id of something that is not a cross-agent reference was asked for");
    return r->id;
}

/* ---- WHAT CROSSES, AND HOW IT CARRIES ITS TYPE ----------------------------------------------------------- */

char *remote_object_encode(JSContext *ctx, JSValueConst v)
{
    char *out;

    if (JS_IsUndefined(v)) return strdup("u");
    if (JS_IsNull(v))      return strdup("N");
    if (JS_IsBool(v))      return strdup(JS_ToBool(ctx, v) ? "b1" : "b0");
    if (JS_IsNumber(v)) {
        double d = 0;
        out = malloc(40);
        CHECK(out != NULL, "remote object: OOM encoding a number for another agent");
        JS_ToFloat64(ctx, &d, v);
        snprintf(out, 40, "n%.17g", d);
        return out;
    }
    if (JS_IsString(v)) {
        size_t len = 0, cap, n;
        const char *t = JS_ToCStringLen(ctx, &len, v);
        CHECK(t != NULL, "remote object: OOM encoding a string for another agent");
        cap = JS_Base64EncodedSize(len) + 2;
        out = malloc(cap);
        CHECK(out != NULL, "remote object: OOM encoding a string for another agent");
        out[0] = 's';
        n = JS_Base64Encode(out + 1, cap - 1, (const uint8_t *)t, len);
        CHECK(n > 0 || len == 0, "the base64 buffer was sized wrong for a cross-agent string");
        out[1 + n] = 0;
        JS_FreeCString(ctx, t);
        return out;
    }
    if (JS_IsObject(v)) {
        /* AN OBJECT CROSSES AS ITS NAME, AND THE NAME SAYS WHOSE. A value that is ALREADY a reference re-emits
           the name it arrived with — exporting the proxy instead would make a round trip a proxy of a proxy,
           and the page's `===` would answer false about an object that never moved. */
        RefData *r = ref_of_proxy(ctx, v);
        uint32_t doc = r ? r->doc : world_local_doc();
        uint32_t id  = r ? r->id  : remote_object_export(ctx, v);
        const char *doc_name = world_doc_name(doc);
        char tag = JS_IsConstructor(ctx, v) ? 'c' : JS_IsFunction(ctx, v) ? 'f' : 'o';
        size_t cap = strlen(doc_name) + 32;

        DCHECK(!strchr(doc_name, '\t') && !strchr(doc_name, '\n'),
               "a document name carries a tab or a newline — these records are one tab-separated line, so the "
               "peer would read one field as two and the id would come back as zero");
        out = malloc(cap);
        CHECK(out != NULL, "remote object: OOM naming an object for another agent");
        snprintf(out, cap, "%c%s:%u", tag, doc_name, id);
        return out;
    }
    DCHECK(!JS_IsSymbol(v),
           "a SYMBOL crossed an agent boundary and this protocol has no encoding for one. It is not that a "
           "symbol cannot cross — it is three encodings nobody has written: a WELL-KNOWN symbol crosses as its "
           "@@name and is resolved to the receiving agent's own, a REGISTERED one as its Symbol.for key, and a "
           "unique one as an export like any other value with identity");
    DFAIL("a value crossed an agent boundary that this protocol does not name — a BigInt is the primitive that "
          "is left, and its text form is not built");
    return strdup("u");
}

JSValue remote_object_decode(JSContext *ctx, const char *text)
{
    DCHECK(text != NULL && *text, "a cross-agent record carried an empty field where a typed value belongs");
    switch (text[0]) {
    case 'u': return JS_UNDEFINED;
    case 'N': return JS_NULL;
    case 'b': return JS_NewBool(ctx, text[1] == '1');
    case 'n': return JS_NewFloat64(ctx, strtod(text + 1, NULL));
    case 's': {
        size_t n = strlen(text + 1), cap = JS_Base64DecodedMax(n) + 1, len;
        uint8_t *bytes = malloc(cap);
        JSValue s;
        int err = 0;
        CHECK(bytes != NULL, "remote object: OOM decoding a string from another agent");
        len = JS_Base64Decode(bytes, cap, text + 1, n, &err);
        DCHECK(!err, "a cross-agent string field is not base64 — the record was written by something that does "
                     "not share this file's grammar");
        s = JS_NewStringLen(ctx, (const char *)bytes, len);
        free(bytes);
        return s;
    }
    case 'o': case 'f': case 'c': {
        const char *colon = strrchr(text + 1, ':');
        char *doc_name;
        uint32_t doc, id;

        DCHECK(colon != NULL, "a cross-agent object name carried no document — an id alone means nothing "
                              "outside the agent that minted it, and resolving it here would answer with "
                              "whatever this agent happens to have lent under that number");
        if (!colon) return JS_UNDEFINED;
        doc_name = strndup(text + 1, (size_t)(colon - (text + 1)));
        CHECK(doc_name != NULL, "remote object: OOM reading a cross-agent object name");
        doc = world_doc_intern(doc_name);
        free(doc_name);
        id = (uint32_t)strtoul(colon + 1, NULL, 10);
        /* A NAME THAT CAME HOME. The object is in THIS heap and is reached directly: minting a reference for it
           would route a read back out to a peer for a question this agent is the answer to, and would make
           `x === theSameXComingBack` false. */
        if (doc == world_local_doc()) {
            JSValueConst held = remote_object_by_id(id);
            DCHECK(!JS_IsUndefined(held),
                   "a peer named an object of THIS agent that was never lent — the name it used was minted "
                   "somewhere else, or the export table was lost between the lend and the return");
            return JS_DupValue(ctx, held);
        }
        DCHECK(!world_doc_hosted(doc),
               "a cross-agent name resolved to a document THIS agent holds under a foreign name — one document "
               "would then have two identities, and an object of it would answer `===` against itself as false");
        return ref_mint(ctx, doc, id, text[0] != 'o', text[0] == 'c');
    }
    default: break;
    }
    DFAIL("a cross-agent record carried a typed value under a tag this protocol does not name");
    return JS_UNDEFINED;
}

/* ---- THE ONE MACHINE THE FOUR BUILT INTERNAL METHODS ARE ------------------------------------------------- */

/* WHERE THIS MACHINE RESTS. A cross-instance operation is ONE operation — the internal method, performed by
   the instance that owns the object — and the wait for that peer is a sub-sequence inside it rather than a step
   of its own. Each of the four names its OWN algorithm, because a parked flow says which one it is inside. */
#define REF_GET_STAGES(X) \
    X(REF_GET_ASK = IDL_STEP_FIRST, \
      "ECMA-262 10.1.8 [[Get]], performed in the instance that owns the object (the flow suspends on the read " \
      "and resumes with what the peer's own program answered)")
#define REF_SET_STAGES(X) \
    X(REF_SET_ASK = IDL_STEP_FIRST, \
      "ECMA-262 10.1.9 [[Set]], performed in the instance that owns the object under the ASKING flow's world " \
      "(the flow suspends on the write and resumes with the boolean the peer's own program answered)")
#define REF_DEL_STAGES(X) \
    X(REF_DEL_ASK = IDL_STEP_FIRST, \
      "ECMA-262 10.1.10 [[Delete]], performed in the instance that owns the object under the ASKING flow's " \
      "world (the flow suspends and resumes with the boolean the peer's own program answered)")
#define REF_APPLY_STAGES(X) \
    X(REF_APPLY_ASK = IDL_STEP_FIRST, \
      "ECMA-262 10.2.1 [[Call]], performed in the instance that owns the function under the ASKING flow's " \
      "world (the flow suspends at the call site and resumes with the peer's completion)")
enum { REF_GET_STAGES(JS_STEP_STAGE_ENUM) };
enum { REF_SET_STAGES(JS_STEP_STAGE_ENUM) };
enum { REF_DEL_STAGES(JS_STEP_STAGE_ENUM) };
enum { REF_APPLY_STAGES(JS_STEP_STAGE_ENUM) };
static const char *const REF_GET_STEPS[]   = { REF_GET_STAGES(JS_STEP_STAGE_LABEL) NULL };
static const char *const REF_SET_STEPS[]   = { REF_SET_STAGES(JS_STEP_STAGE_LABEL) NULL };
static const char *const REF_DEL_STEPS[]   = { REF_DEL_STAGES(JS_STEP_STAGE_LABEL) NULL };
static const char *const REF_APPLY_STEPS[] = { REF_APPLY_STAGES(JS_STEP_STAGE_LABEL) NULL };

/* §7.2.5.1's read, one agent further out, and its three siblings:
     object.get    <doc> <world+ancestry> <id> <key>
     object.set    <doc> <world+ancestry> <id> <key> <value>
     object.delete <doc> <world+ancestry> <id> <key>
     object.apply  <doc> <world+ancestry> <id> <thisArg> <arg>*
   The world travels for the same reason it travels with a WindowProxy read — the answer is only true in it, and
   for a WRITE it is stronger than that: two arms of a fork writing through one reference are two contradictory
   timelines, and a peer that performed both against one baseline would fabricate a third neither arm was in. Its
   ancestry travels too, so the peer can materialize its segment by forking the nearest ancestor it holds.
   The vector is written by world_serialize and by nothing else: two spellings of it are two peers materializing
   different segments for one flow. */
typedef struct { uint32_t req; } RefOpState;

static void ref_op_visit(JSContext *ctx, void *st, JSStepVisit *v) { (void)ctx; (void)st; (void)v; }

/* A RECORD BEING BUILT. Growable, because an argument list has no length this file may cap — a bound on how
   many arguments may cross is a bound on which calls this engine can make. */
typedef struct { char *s; size_t n, cap; } Rec;

static void rec_add(Rec *r, const char *t)
{
    size_t l = strlen(t);
    if (r->n + l + 1 > r->cap) {
        while (r->cap < r->n + l + 1) r->cap = r->cap ? r->cap * 2 : 256;
        r->s = realloc(r->s, r->cap);
        CHECK(r->s != NULL, "remote object: OOM building a cross-agent request — a dropped request parks its "
                            "flow on a question nothing is going to answer");
    }
    memcpy(r->s + r->n, t, l + 1);
    r->n += l;
}

/* Append one operand in the one grammar, and release the encoder's record. */
static void rec_add_value(Rec *r, JSContext *ctx, JSValueConst v)
{
    char *enc = remote_object_encode(ctx, v);
    rec_add(r, "\t");
    rec_add(r, enc);
    free(enc);
}

/* A capability this operation needs and does not have: named at the site, thrown in a release build where
   there is nothing to fall back to. Always returns JS_STEP_ABRUPT so the call site reads as one statement. */
static int ref_unbuilt_here(JSContext *ctx, const char *what)
{
    JS_ThrowInternalError(ctx, "%s", what);
    return JS_STEP_ABRUPT;
}

static int ref_op_step(JSContext *ctx, JSStepHdr *hdr, void *st, int argc, JSValueConst *argv,
                       JSValue cb_result, JSValue *presult, JSValue **out_cb, int *out_argc)
{
    RefOpState *s = st;
    JSValueConst answer;
    int op = idl_step_magic(hdr);

    (void)out_cb; (void)out_argc;
    JS_FreeValue(ctx, cb_result);
    DCHECK(op >= 0 && op < REF_OP_N, "a cross-agent trap ran under a magic this component does not name");

    if (s->req == 0) {
        RefData *r;
        Flow *f = flow_running();
        Rec rec = { NULL, 0, 0 };
        char world[1024], idbuf[32];

        DCHECK(argc >= REF_ARGC[op],
               "a cross-agent trap ran with fewer operands than its internal method is passed — the machine "
               "reads them by position, so a short vector would read the receiver out of the value's slot");
        if (argc < REF_ARGC[op]) return ref_unbuilt_here(ctx, "a cross-agent trap ran with too few operands");
        DCHECK(f != NULL, "a cross-agent operation was issued outside a flow — there would be nothing to "
                          "suspend and no world to perform it in");
        /* THE TARGET IS THE IDENTITY, and it is argv[0] by the trap's own contract. */
        r = ref_of_target(argv[0]);
        DCHECK(r != NULL, "a cross-agent trap ran with a target that is not a reference — the trap and the "
                          "target it was built with have been separated");
        if (!r) return ref_unbuilt_here(ctx, "a cross-agent trap lost its target");

        /* EVERY VALIDATION BEFORE THE FIRST ALLOCATION, so a refusal has nothing to unwind. */
        if (op != REF_OP_APPLY && !JS_IsString(argv[1])) {
            DCHECK(JS_IsSymbol(argv[1]), "a cross-agent keyed operation was handed a key that is neither a "
                                         "string nor a symbol — a property key is one of the two");
            DFAIL("a SYMBOL-keyed operation on a cross-agent reference. The old answer here was `undefined`, "
                  "justified by a symbol being unique to the agent that minted it — which is false for the two "
                  "kinds a page actually uses: `remote[Symbol.iterator]` and `remote[Symbol.toPrimitive]` name "
                  "WELL-KNOWN symbols, which every agent has its own of and which cross as their @@name, and a "
                  "registered one crosses as its Symbol.for key. Build the symbol encoding in "
                  "remote_object_encode; a unique symbol is the residue and is an export like any other value "
                  "with identity");
            return ref_unbuilt_here(ctx, "a symbol-keyed operation on a cross-agent reference is not built");
        }
        /* 10.5.8 step 9 / 10.5.9 step 9 hand the trap the RECEIVER, and it is the reference itself for every
           ordinary `remote.x` / `remote.x = v`. When it is anything else — `Object.create(remote).x = v`,
           `Reflect.set(remote, k, v, other)` — the peer's OrdinarySetWithOwnDescriptor would run
           CreateDataProperty on an object in THIS agent, so the receiver has to cross and the peer has to be
           able to ask BACK. That is the reverse direction of this transport and it does not exist. */
        if (op == REF_OP_GET || op == REF_OP_SET) {
            RefData *rr = ref_of_proxy(ctx, argv[op == REF_OP_SET ? 3 : 2]);
            if (!rr || rr->doc != r->doc || rr->id != r->id) {
                DFAIL("a cross-agent [[Get]]/[[Set]] whose RECEIVER is not the reference itself — the peer's "
                      "algorithm ends on the receiver (10.1.9.2 defines the property THERE), so the operation "
                      "would land on an object in this agent. Build the reverse direction: the peer must be "
                      "able to issue a request back to the agent that asked, which is a second route the "
                      "trusted zone owns and this transport has only one of");
                return ref_unbuilt_here(ctx, "a cross-agent operation with a foreign receiver is not built");
            }
        }
        if (op == REF_OP_APPLY) {
            DCHECK(JS_IsArray(argv[2]),
                   "the apply trap's argument list is not an Array — 10.5.12 step 7 builds it with "
                   "CreateArrayFromList, so it is a plain array of the engine's own and reading it runs no "
                   "page code");
        }

        rec_add(&rec, "object.");
        rec_add(&rec, REF_VERB[op]);
        rec_add(&rec, "\t");
        rec_add(&rec, world_doc_name(r->doc));
        rec_add(&rec, "\t");
        world_serialize(f->world, world, sizeof world);
        rec_add(&rec, world);
        snprintf(idbuf, sizeof idbuf, "\t%u", r->id);
        rec_add(&rec, idbuf);

        if (op == REF_OP_APPLY) {
            /* thisArg, then the argument list — flattened, because each element is a value in the one grammar
               and a nested list would be a second one. */
            JSValue len = JS_GetPropertyStr(ctx, argv[2], "length");
            uint32_t n = 0, i;
            JS_ToUint32(ctx, &n, len);
            JS_FreeValue(ctx, len);
            rec_add_value(&rec, ctx, argv[1]);
            for (i = 0; i < n; i++) {
                JSValue e = JS_GetPropertyUint32(ctx, argv[2], i);
                rec_add_value(&rec, ctx, e);
                JS_FreeValue(ctx, e);
            }
        } else {
            rec_add_value(&rec, ctx, argv[1]);
            if (op == REF_OP_SET) rec_add_value(&rec, ctx, argv[2]);
        }

        s->req = engine_host_request(ctx, rec.s);
        free(rec.s);
        return JS_STEP_YIELD;   /* park; siblings run until the peer answers */
    }
    if (!engine_host_answered(s->req, &answer))
        return JS_STEP_YIELD;
    *presult = engine_host_take(ctx, s->req);
    s->req = 0;
    /* 10.5.9 / 10.5.10 ToBoolean the trap's result, so a non-boolean here is not a page's doing — it is the
       peer having answered a [[Set]] or a [[Delete]] with something that is not the operation's completion. */
    DCHECK(!(op == REF_OP_SET || op == REF_OP_DELETE) || JS_IsBool(*presult),
           "a peer answered a cross-agent [[Set]] or [[Delete]] with something that is not a boolean — those "
           "internal methods complete with the operation's success, and anything else means the peer ran a "
           "program other than the one the operation names");
    return JS_STEP_DONE;
}

static const IdlStepDecl REF_GET_DECL   = { ref_op_step, sizeof(RefOpState), ref_op_visit, NULL,
                                            "ECMA-262 10.1.8 [[Get]] across an instance boundary",
                                            REF_GET_STEPS };
static const IdlStepDecl REF_SET_DECL   = { ref_op_step, sizeof(RefOpState), ref_op_visit, NULL,
                                            "ECMA-262 10.1.9 [[Set]] across an instance boundary",
                                            REF_SET_STEPS };
static const IdlStepDecl REF_DEL_DECL   = { ref_op_step, sizeof(RefOpState), ref_op_visit, NULL,
                                            "ECMA-262 10.1.10 [[Delete]] across an instance boundary",
                                            REF_DEL_STEPS };
static const IdlStepDecl REF_APPLY_DECL = { ref_op_step, sizeof(RefOpState), ref_op_visit, NULL,
                                            "ECMA-262 10.2.1 [[Call]] across an instance boundary",
                                            REF_APPLY_STEPS };

/* ---- AND THE INTERNAL METHODS THAT ARE NOT BUILT --------------------------------------------------------- */

/* Every one of them would otherwise be answered OUT OF THE EMPTY TARGET, which is a silent lie about the peer's
   object: no prototype, no keys, no own properties, extensible. A trap that aborts naming the operation is the
   honest form, and the name is what the next person builds. */
typedef struct { const char *trap; const char *what; } RefUnbuilt;
static const RefUnbuilt REF_UNBUILT[] = {
    { "getPrototypeOf",
      "a [[GetPrototypeOf]] through a cross-agent reference — build the peer-side program and answer with the "
      "prototype AS A NAME (it is an object, so it is an export like any other); answering from the empty "
      "target reports the peer's object as having no prototype" },
    { "setPrototypeOf",
      "a [[SetPrototypeOf]] through a cross-agent reference — build the peer-side program; its operand is an "
      "object and crosses as a name, and it runs under the asking flow's world like every other write" },
    { "isExtensible",
      "an [[IsExtensible]] through a cross-agent reference — build the peer-side program; the empty target "
      "answers `true` about a peer object that may be sealed, and 10.5.3's invariant then makes the lie the "
      "engine's own" },
    { "preventExtensions",
      "a [[PreventExtensions]] through a cross-agent reference — build the peer-side program; it must actually "
      "seal the peer's object, because 10.5.4 checks the claim against the target" },
    { "getOwnPropertyDescriptor",
      "a [[GetOwnProperty]] through a cross-agent reference — build the peer-side program AND the encoding for "
      "a DESCRIPTOR, which is a record and not a value: four booleans plus a value, or plus two functions that "
      "are themselves objects crossing as names" },
    { "defineProperty",
      "a [[DefineOwnProperty]] through a cross-agent reference — the descriptor encoding above, in the other "
      "direction, under the asking flow's world" },
    { "has",
      "a [[HasProperty]] through a cross-agent reference — build the peer-side program; `k in remote` is "
      "answered `false` out of the empty target today, which is a claim about a heap this agent cannot see" },
    { "ownKeys",
      "an [[OwnPropertyKeys]] through a cross-agent reference — build the peer-side program and an encoding "
      "for a LIST of keys; the empty target reports the peer's object as having none, and 10.5.11's invariant "
      "then enforces that emptiness" },
    { "construct",
      "a [[Construct]] through a cross-agent reference — build the peer-side program: it takes the same "
      "argument list [[Call]] takes and additionally a NEW TARGET, which is an object and crosses as a name" },
};
#define REF_UNBUILT_N ((int)(sizeof REF_UNBUILT / sizeof REF_UNBUILT[0]))

static JSValue ref_unbuilt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
    (void)this_val; (void)argc; (void)argv;
    DCHECK(magic >= 0 && magic < REF_UNBUILT_N, "a cross-agent trap ran under a magic that names no operation");
    DFAIL(REF_UNBUILT[magic].what);
    /* In RELEASE the DFAIL compiles out and there is deliberately nothing to fall back to: failing visibly is
       right where answering out of the empty target is not. */
    JS_ThrowInternalError(ctx, "%s", REF_UNBUILT[magic].what);
    return JS_EXCEPTION;
}

/* ---- MINTING A REFERENCE --------------------------------------------------------------------------------- */

static JSValue ref_mint(JSContext *ctx, uint32_t doc, uint32_t id, bool callable, bool constructor)
{
    char key[64];
    JSValue found, handler, target, proxy;
    RefData *r;
    int i;

    DCHECK(g_ref_class != 0, "a cross-agent reference was minted before remote_object_init ran");
    DCHECK(doc != 0 && doc != world_local_doc(),
           "a cross-agent reference was minted for THIS agent's own document — an object in this heap is "
           "reached directly, and routing it would suspend a flow on a question this agent is the answer to");
    DCHECK(id != 0, "a cross-agent reference was minted with no id — it would name nothing in the peer");
    DCHECK(!constructor || callable, "a cross-agent reference claims [[Construct]] without [[Call]] — a "
                                     "constructor is a function, so the peer named something impossible");

    /* ONE REFERENCE PER (doc, id): the other half of identity. The exporter mints one id per object; without
       this the same id would arrive twice as two objects and `w.document === w.document` would be false on
       this side instead of the other. */
    snprintf(key, sizeof key, "%u:%u", doc, id);
    found = JS_GetPropertyStr(ctx, g_refs, key);
    if (JS_IsObject(found)) {
        RefData *had = ref_of_proxy(ctx, found);
        (void)had;
        DCHECK(had != NULL, "the reference table held something that is not a reference under a (doc, id) key");
        DCHECK(had == NULL || (had->callable == (uint8_t)callable && had->constructor == (uint8_t)constructor),
               "one object of a peer arrived twice with two different callabilities — an object does not stop "
               "being a function, so either the peer minted one id for two objects or the kind byte was lost");
        return found;
    }
    JS_FreeValue(ctx, found);

    handler = JS_NewObject(ctx);
    CHECK(!JS_IsException(handler), "a cross-agent reference's handler could not be allocated");
    JS_SetPropertyStr(ctx, handler, "get", idl_step_function(ctx, "get", 3, g_ref_stepid[REF_OP_GET]));
    JS_SetPropertyStr(ctx, handler, "set", idl_step_function(ctx, "set", 4, g_ref_stepid[REF_OP_SET]));
    JS_SetPropertyStr(ctx, handler, "deleteProperty",
                      idl_step_function(ctx, "deleteProperty", 2, g_ref_stepid[REF_OP_DELETE]));
    JS_SetPropertyStr(ctx, handler, "apply", idl_step_function(ctx, "apply", 3, g_ref_stepid[REF_OP_APPLY]));
    for (i = 0; i < REF_UNBUILT_N; i++)
        JS_SetPropertyStr(ctx, handler, REF_UNBUILT[i].trap,
                          JS_NewCFunction2(ctx, (JSCFunction *)ref_unbuilt, REF_UNBUILT[i].trap, 2,
                                           JS_CFUNC_generic_magic, i));

    /* AN EMPTY EXTENSIBLE TARGET, which is what makes the reference fully virtual: a Proxy's invariants are
       stated against its target, and an extensible target with no own properties constrains no trap's result.
       A target carrying a property would make the peer's answers checkable against this agent's guesses. It
       carries the (doc, id) as an internal slot instead, which is not a property and constrains nothing —
       and it carries CALLABILITY in its CLASS, because JS_NewProxy reads that once and a reference over a
       plain target has no [[Call]] at all. */
    target = JS_NewObjectClass(ctx, callable ? g_ref_fn_class : g_ref_class);
    CHECK(!JS_IsException(target), "a cross-agent reference's target could not be allocated");
    r = calloc(1, sizeof *r);
    CHECK(r != NULL, "remote object: OOM building a cross-agent reference");
    r->doc = doc;
    r->id = id;
    r->callable = (uint8_t)callable;
    r->constructor = (uint8_t)constructor;
    JS_SetOpaque(target, r);
    if (constructor) JS_SetConstructorBit(ctx, target, true);
    DCHECK(JS_IsExtensible(ctx, target) == 1,
           "a cross-agent reference's target is not extensible — every proxy invariant is stated against the "
           "target, so a non-extensible one would start checking the peer's answers against this empty shape");
    proxy = JS_NewProxy(ctx, target, handler);
    JS_FreeValue(ctx, target);
    JS_FreeValue(ctx, handler);
    CHECK(!JS_IsException(proxy), "a cross-agent reference could not be allocated");
    DCHECK(JS_IsFunction(ctx, proxy) == callable,
           "a cross-agent reference to a peer's function is not callable — JS_NewProxy reads the target's "
           "callability once, so its `apply` trap would never fire and `typeof` would answer \"object\"");
    JS_SetPropertyStr(ctx, g_refs, key, JS_DupValue(ctx, proxy));
    return proxy;
}

static void ref_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    RefData *r = JS_GetAnyOpaque(val, &id);
    (void)rt;
    DCHECK(id == g_ref_class || id == g_ref_fn_class,
           "the cross-agent reference finalizer ran on an object of another class");
    free(r);
}

/* A reference's TARGET is not script-reachable — a Proxy does not expose it — so this is the callability
   declaration and never a call. */
static JSValue ref_target_call(JSContext *ctx, JSValueConst func_obj, JSValueConst this_val,
                               int argc, JSValueConst *argv, int flags)
{
    (void)func_obj; (void)this_val; (void)argc; (void)argv; (void)flags;
    DFAIL("the TARGET of a cross-agent reference was called directly — a Proxy's target is not reachable from "
          "script, so this class's `call` exists only to make JS_NewProxy see a function; every call goes "
          "through the apply trap");
    JS_ThrowInternalError(ctx, "a cross-agent reference's target is not callable");
    return JS_EXCEPTION;
}

void remote_object_init(JSContext *ctx)
{
    JSClassDef d  = { "CrossAgentReference", .finalizer = ref_finalizer };
    JSClassDef df = { "CrossAgentFunctionReference", .finalizer = ref_finalizer, .call = ref_target_call };
    JSRuntime *rt = JS_GetRuntime(ctx);
    /* THE TRAP'S ARGUMENTS, declared so the machine PASSES them: a declaration of none converts none and hands
       the body none, and the property name is the second — the trap would then read a name it was never given.
       They are `any` because a trap's arguments are whatever the operator site had. */
    static const IdlArgType TRAP_ARGS[4] = { IDL_ANY, IDL_ANY, IDL_ANY, IDL_ANY };

    DCHECK(g_ref_class == 0, "remote_object_init ran twice — one instance is one agent");
    JS_NewClassID(rt, &g_ref_class);
    CHECK(JS_NewClass(rt, g_ref_class, &d) == 0, "the cross-agent reference class could not be registered");
    JS_NewClassID(rt, &g_ref_fn_class);
    CHECK(JS_NewClass(rt, g_ref_fn_class, &df) == 0,
          "the cross-agent function-reference class could not be registered");

    g_ref_stepid[REF_OP_GET]    = idl_method_id_step(ctx, TRAP_ARGS, 3, NULL, 0, &REF_GET_DECL, REF_OP_GET);
    g_ref_stepid[REF_OP_SET]    = idl_method_id_step(ctx, TRAP_ARGS, 4, NULL, 0, &REF_SET_DECL, REF_OP_SET);
    g_ref_stepid[REF_OP_DELETE] = idl_method_id_step(ctx, TRAP_ARGS, 2, NULL, 0, &REF_DEL_DECL, REF_OP_DELETE);
    g_ref_stepid[REF_OP_APPLY]  = idl_method_id_step(ctx, TRAP_ARGS, 3, NULL, 0, &REF_APPLY_DECL, REF_OP_APPLY);

    g_refs = JS_NewObject(ctx);
    CHECK(!JS_IsException(g_refs), "the cross-agent reference table could not be allocated");
}

void remote_object_free(JSContext *ctx)
{
    uint32_t i;
    int k;
    for (i = 0; i < g_exports_n; i++) JS_FreeValue(ctx, g_exports[i].v);
    free(g_exports);
    g_exports = NULL;
    g_exports_n = g_exports_cap = 0;
    JS_FreeValue(ctx, g_refs);
    g_refs = JS_UNDEFINED;
    g_ref_class = g_ref_fn_class = 0;
    for (k = 0; k < REF_OP_N; k++) g_ref_stepid[k] = -1;
}
