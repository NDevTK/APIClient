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
 * EXCEPT A NAVIGABLE, WHICH CROSSES AS ITS IDENTITY AND NOT AS A NAME. A WindowProxy — and the Window global,
 * which is the same navigable spelled the other way — is the one object both agents can independently BUILD,
 * because a navigable belongs to the agent that created it and only its ACTIVE DOCUMENT lives in the peer. So
 * it crosses as the five facts that decide which navigable it is (nav_encode) and is resolved on the far side
 * to that agent's own WindowProxy, never lent as `o<doc>:<gen>:<id>` — see the encoder for what a reference proxy
 * would silently do to postMessage, to §7.2.1's cross-origin filter and to `w[0] === w.frames[0]`.
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
 * AN ANSWER IS A COMPLETION AND NOT A VALUE, which is the other half of "what crosses is text and it carries
 * its type": the peer performs every operation by running a program, and a program either returns or THROWS.
 * The completion type rides in front of the value (remote_completion_encode), the thrown value crosses by the
 * ordinary rules — an Error is an object, so it crosses as a NAME — and the asking flow's step machine RAISES
 * it at the call site that parked, so a page's `try`/`catch` around a cross-agent operation runs its handler.
 *
 * A SYMBOL KEY CROSSES, and it is three encodings rather than one because a symbol is three kinds of thing. A
 * WELL-KNOWN symbol is a distinct value in every agent denoting the same slot, so it crosses as its
 * [[Description]] and resolves to the receiving agent's own — which is what keeps `"" + remote` (a
 * Get(remote, @@toPrimitive) on the first line that logs a reference) from taking the engine down. A
 * REGISTERED symbol IS its Symbol.for key. A UNIQUE symbol is identity and nothing else, needs an export like
 * an object, and is the one remote_object_encode still aborts on.
 *
 * ONE absence is left at its own site: a [[Get]] or [[Set]] whose RECEIVER is not the reference itself. The
 * peer's algorithm ends on the receiver, so the peer would have to perform an internal method BACK in the
 * agent that asked — and that is not one more record, it is a second direction the channel does not have. The
 * DFAIL in ref_op_step names the three things it needs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "quickjs.h"
#include "core/agent_state.h"
#include "core/frame/remote_object.h"
#include "core/frame/window.h"
#include "core/frame/window_proxy.h"
#include "core/idl_args.h"
#include "solver/engine.h"
#include "solver/flow.h"
#include "solver/world.h"

/* ---- THIS AGENT AS AN EXPORTER --------------------------------------------------------------------------- */

/* THIS COMPONENT'S NAME, spelled once — core/platform.c's row and every slot it declares to
   core/agent_state.h. Neither spelling is checked by the compiler, and a mismatch is not a weaker check but a
   check that is never RUN, sitting behind a message about a different repair entirely. */
#define RO_COMPONENT "remote_object"

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

JSValueConst remote_object_by_id(uint32_t session, uint32_t id)
{
    /* A NAME THIS DOCUMENT LENT IN AN EARLIER SESSION. The id is an index into a table that died with that
       session, and this one mints from 1 again, so the name is IN RANGE and names a different object. It is
       refused — the whole reason the generation is on the wire is that an old name must be unresolvable rather
       than wrongly resolvable — and the refusal names the capability, because a peer that legitimately holds a
       reference across a park is the ordinary case: Level-1 eviction gives up ONE document's engine while the
       instance reading from it stays exactly where it was. */
    if (session != world_session()) {
        DFAIL("a peer performed an operation through a reference this document lent in an EARLIER SESSION — "
              "the export table died with that session and this one mints its ids from 1 again, so the name "
              "resolves in range to an unrelated object and `===` answers wrong. Build what carries an export "
              "across a park: an exported object is state the RECIPE has to name (solver/cold.h), or the park "
              "must tell each peer that its references into this document are gone — the second is the EXPORT "
              "sibling of the world death the park already announces (world_session_gone, solver/world.h), and "
              "it is a different record because a world names a TIMELINE and an export names an OBJECT");
        return JS_UNDEFINED;
    }
    if (id == 0 || id > g_exports_n) return JS_UNDEFINED;
    return g_exports[id - 1].v;
}

/* ---- THIS AGENT AS AN IMPORTER --------------------------------------------------------------------------- */

/* The (doc, session, id) a reference names, kept on the PROXY'S TARGET. Not on the handler, and the difference
   is not stylistic: a trap is passed (target, property, receiver) and the target is argv[0] every time, whereas
   what `this` is inside a trap depends on how the interpreter dispatched it. Reading the identity off an
   argument the contract guarantees is the only version that cannot be wrong.
   `session` is the exporting agent's GENERATION and is part of the name, not metadata about it: an id is an
   index into ONE session's export table (remote_object.h), so a reference that kept only (doc, id) would ask
   the resumed instance of that document for an object of the session that ended and be answered by whatever
   the new table happens to hold at that index.
   `callable` is what the peer said about the object, and it rides HERE rather than being inferred because
   nothing in this heap can look at the object to ask. */
typedef struct { uint32_t doc, session, id; uint8_t callable, constructor; } RefData;

/* TWO TARGET CLASSES FOR ONE KIND OF REFERENCE, because JS_NewProxy reads `JS_IsFunction(target)` and
   `JS_IsConstructor(target)` ONCE, at mint, and a Proxy over a non-callable target has no [[Call]] at all — its
   `apply` trap can never fire and `typeof` answers "object". So a reference to a peer's function is built over
   a target whose class declares a `call`, and the constructor bit is set on top of that for a `c`. The class's
   own call handler is unreachable by construction (a Proxy's target is not script-reachable) and says so. */
static JSClassID g_ref_class, g_ref_fn_class;
static JSValue   g_refs = JS_UNDEFINED;   /* "<doc>:<session>:<id>" -> the one reference for it (owned) */

/* WHICH INTERNAL METHOD a reference is performing — the machine's magic, and the index of its verb on the
   wire. One enumeration, so a verb and the operands that follow it cannot be declared apart. */
enum { REF_OP_GET, REF_OP_SET, REF_OP_DELETE, REF_OP_APPLY, REF_OP_N };
static const char *const REF_VERB[REF_OP_N] = { "get", "set", "delete", "apply" };
/* How many operands each trap is passed — 10.5.8's (target, key, receiver), 10.5.9's (target, key, value,
   receiver), 10.5.10's (target, key) and 10.5.12's (target, thisArg, argArray). Declared beside the verb
   because the machine reads operands BY POSITION and a count stated anywhere else is a count that can drift. */
static const int REF_ARGC[REF_OP_N] = { 3, 4, 2, 3 };
/* WHAT EACH POOL ENTRY IS, for core/agent_state.h's assert — declared HERE, beside the verb and the operand
   count, because it is the third thing that is per-op and a fourth list somewhere else is a fourth list that
   can drift. The reader of the `@WHY` is standing at a teardown that already ran, so the entry is named by the
   internal method it performs rather than by the index of the array it sits in. */
static const char *const REF_STEP_WHAT[REF_OP_N] = {
    "the machine performing ECMA-262 10.5.8 [[Get]] ( propertyKey, receiver ) across an agent boundary",
    "the machine performing ECMA-262 10.5.9 [[Set]] ( propertyKey, value, receiver ) across an agent boundary",
    "the machine performing ECMA-262 10.5.10 [[Delete]] ( propertyKey ) across an agent boundary",
    "the machine performing ECMA-262 10.5.12 [[Call]] ( thisArg, argList ) across an agent boundary",
};
static int g_ref_stepid[REF_OP_N] = { -1, -1, -1, -1 };

static JSValue ref_mint(JSContext *ctx, uint32_t doc, uint32_t session, uint32_t id,
                        bool callable, bool constructor);

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

/* ---- THE WELL-KNOWN SYMBOLS OF THIS AGENT ---------------------------------------------------------------- */

/* 6.1.5's Table 1, READ OUT OF THIS AGENT'S OWN %Symbol% INTRINSIC rather than listed here. A hand-copied
   table is wrong in both directions and both have happened to lists like it: the spec's grows (asyncDispose
   and dispose are recent), so a list would abort on a symbol the engine has, and an engine that lacks one
   would let the list claim a symbol nothing can resolve. What is well-known is exactly "an own property of
   %Symbol% whose value is a symbol", which is what the table below collects.
   CAPTURED BEFORE THE DOCUMENT'S SCRIPTS RUN, in remote_object_init, for the reason the peer captures
   %Reflect.set% there: `Symbol` is an ordinary writable global binding, so a page's `Symbol = {iterator: 1}`
   would otherwise decide what a cross-agent key means. Held for the agent's life, because a well-known symbol
   is agent-wide — every realm of one agent shares it, which is why one capture answers for all of them. */
typedef struct { char *desc; JSValue sym; } WellKnown;
static WellKnown *g_wk;
static int        g_wk_n;

static int wk_by_value(JSValueConst v)
{
    int i;
    for (i = 0; i < g_wk_n; i++)
        if (JS_VALUE_GET_PTR(g_wk[i].sym) == JS_VALUE_GET_PTR(v)) return i;
    return -1;
}

static int wk_by_desc(const char *desc)
{
    int i;
    for (i = 0; i < g_wk_n; i++)
        if (!strcmp(g_wk[i].desc, desc)) return i;
    return -1;
}

static void wk_capture(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue sym_ctor = JS_GetPropertyStr(ctx, global, "Symbol");
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;

    JS_FreeValue(ctx, global);
    CHECK(JS_IsObject(sym_ctor),
          "remote object: this realm has no %Symbol% — a cross-agent key that is a well-known symbol is "
          "resolved against it, and a realm without one can encode none of them");
    /* NOT ENUM_ONLY: 6.1.5's members of %Symbol% are non-enumerable, and asking for the enumerable ones
       returns a table with no well-known symbol in it at all. */
    CHECK(JS_GetOwnPropertyNames(ctx, &tab, &n, sym_ctor, JS_GPN_STRING_MASK) == 0,
          "remote object: %Symbol%'s own properties could not be read");
    for (i = 0; i < n; i++) {
        JSValue member = JS_GetProperty(ctx, sym_ctor, tab[i].atom);
        JSAtom a;
        const char *desc;
        size_t dlen = 0;

        if (!JS_IsSymbol(member)) { JS_FreeValue(ctx, member); continue; }
        /* THE NAME ON THE WIRE IS THE SYMBOL'S OWN [[Description]], not the property it hangs off — those
           agree today ("Symbol.iterator" on `Symbol.iterator`) and the description is the one the spec's Table
           1 states, so reading it from the value keeps the wire name a fact about the symbol. */
        a = JS_ValueToAtom(ctx, member);
        desc = JS_AtomToCStringLen(ctx, &dlen, a);
        CHECK(desc != NULL, "remote object: OOM reading a well-known symbol's description");
        DCHECK(dlen > 0 && strlen(desc) == dlen && !strchr(desc, '\t') && !strchr(desc, '\n'),
               "a well-known symbol's [[Description]] is empty or carries a tab, a newline or a NUL — these "
               "records are one tab-separated line and the peer would read one field as two");
        g_wk = realloc(g_wk, (size_t)(g_wk_n + 1) * sizeof *g_wk);
        CHECK(g_wk != NULL, "remote object: OOM recording this agent's well-known symbols");
        g_wk[g_wk_n].desc = strdup(desc);
        CHECK(g_wk[g_wk_n].desc != NULL, "remote object: OOM recording a well-known symbol's description");
        g_wk[g_wk_n].sym = member;   /* held: the table outlives whatever else names it */
        g_wk_n++;
        JS_FreeCString(ctx, desc);
        JS_FreeAtom(ctx, a);
    }
    JS_FreePropertyEnum(ctx, tab, n);
    JS_FreeValue(ctx, sym_ctor);
    DCHECK(g_wk_n > 0, "this agent's %Symbol% carries no well-known symbol — 6.1.5 defines fifteen and the "
                       "engine's own iterators need @@iterator, so the capture read something that is not it");
}

/* IS THIS SYMBOL IN THE GLOBAL REGISTRY, and under what key? 20.4.2.2's registry is keyed BY the description,
   and `JS_NewSymbol(…, is_global)` is the same lookup `Symbol.for` performs — so the symbol the registry holds
   for this symbol's own description IS this symbol exactly when this symbol is registered. It is decided
   without running any of the page's code, which a `Symbol.keyFor` call would be. The registered symbol the
   lookup may CREATE is unobservable: the registry cannot be enumerated, and `Symbol.for(k)` would mint the
   same one on demand. */
static bool sym_registered_key(JSContext *ctx, JSValueConst v, char **pkey, size_t *pkeylen)
{
    JSAtom a = JS_ValueToAtom(ctx, v);
    size_t len = 0;
    const char *desc = JS_AtomToCStringLen(ctx, &len, a);
    JSValue reg;
    bool same;

    CHECK(desc != NULL, "remote object: OOM reading a symbol's description");
    /* ASKED BEFORE THE LOOKUP, because the lookup itself cannot see past a NUL: JS_NewSymbol names a registry
       key by a C STRING, so `Symbol.for("a\0b")` would be compared against the registry's entry for "a" and
       report itself UNIQUE — a wrong answer wearing the right shape. The description is where the truncation
       happens, so this is where it is asserted. */
    DCHECK(strlen(desc) == len,
           "a symbol's description carries a NUL, and the registry is keyed by a C string here — a REGISTERED "
           "symbol whose Symbol.for key holds one cannot be told from a unique symbol, so it would cross as "
           "the wrong kind or not at all. Reach the registry by string-and-length to close it");
    reg = JS_NewSymbol(ctx, desc, true);
    CHECK(!JS_IsException(reg), "remote object: OOM asking the symbol registry for a key");
    same = JS_VALUE_GET_PTR(reg) == JS_VALUE_GET_PTR(v);
    if (same) {
        *pkey = malloc(len + 1);
        CHECK(*pkey != NULL, "remote object: OOM copying a registered symbol's key");
        memcpy(*pkey, desc, len + 1);
        *pkeylen = len;
    }
    JS_FreeValue(ctx, reg);
    JS_FreeCString(ctx, desc);
    JS_FreeAtom(ctx, a);
    return same;
}

/* ---- WHAT CROSSES, AND HOW IT CARRIES ITS TYPE ----------------------------------------------------------- */

/* `tag` + base64 of `len` bytes — the one form every page-chosen text crosses in, because these records are
   tab-separated and a property name, a written value and a registry key may each hold a tab or a newline. */
static char *enc_b64(char tag, const char *bytes, size_t len)
{
    size_t cap = JS_Base64EncodedSize(len) + 2, n;
    char *out = malloc(cap);

    CHECK(out != NULL, "remote object: OOM encoding text for another agent");
    out[0] = tag;
    n = JS_Base64Encode(out + 1, cap - 1, (const uint8_t *)bytes, len);
    CHECK(n > 0 || len == 0, "the base64 buffer was sized wrong for a cross-agent string");
    out[1 + n] = 0;
    return out;
}

/* …and back. Returns malloc'd bytes with a NUL after them; `*plen` is the real length, which is not
   strlen(bytes) when the text carried one. */
static char *dec_b64(const char *b64, size_t *plen)
{
    size_t n = strlen(b64), cap = JS_Base64DecodedMax(n) + 1;
    char *bytes = malloc(cap);
    int err = 0;

    CHECK(bytes != NULL, "remote object: OOM decoding text from another agent");
    *plen = JS_Base64Decode((uint8_t *)bytes, cap, b64, n, &err);
    DCHECK(!err, "a cross-agent text field is not base64 — the record was written by something that does not "
                 "share this file's grammar");
    bytes[*plen] = 0;
    return bytes;
}

/* ---- A NAVIGABLE CROSSES AS ITS IDENTITY ----------------------------------------------------------------- */

/* ITS OWN TAG, BESIDE 'o'/'f'/'c' AND NOT ONE OF THEM, because a navigable is not an object this agent may
   lend — see remote_object_encode for what an `o<doc>:<gen>:<id>` would silently do to one. */
#define NAV_TAG 'W'

/* WHAT THE IDENTITY IS, and every field of it is load-bearing:
     DOC     — WHICH navigable. Documents cross by NAME (a `uint32_t doc` is this instance's handle into its
               own table and means a different document in the peer's), and the name is what the receiving
               side resolves FIRST: a name it hosts is its OWN WindowProxy, never a remote one.
     ORIGIN  — the active document's principal, serialized. §7.2.1's whole surface is a filter over it, and a
               proxy minted without one would answer every cross-origin read out of a principal this agent
               invented. "null" is a real answer and resolves to a FRESH opaque origin here, which is correct:
               an opaque origin is same origin with nothing, and two Documents sharing ONE opaque origin share
               an agent cluster and so are never on two sides of this line.
     NAME    — the BROWSING CONTEXT's name, which §7.3.3's named access matches against.
     PARENT  — the parent navigable's DOCUMENT NAME, and this is the field whose absence is invisible:
               window_proxy_for_document takes a parent, and a navigable minted from a bare name answers
               `parent === self` and reports itself a top-level traversable — so `w[0].parent === w` would be
               false and `w[0].close()` would try to close a frame.
     OPENER  — §7.2.2.4's, by document name, for the same reason: `opener` is on §7.2.1.3.1's cross-origin list,
               so a proxy minted with a hardcoded null answers a READABLE member with a value nothing computed.
   EVERY FIELD IS BASE64 AND TERMINATED BY '.', not separated by one. Terminated, because a field that is
   EMPTY (a top-level navigable's parent) then reads as one rather than as a missing separator; base64,
   because a document name is "<parent>.<n>" and is full of the character doing the separating — '.' is
   outside base64's alphabet, which is what makes a dotted record able to carry dotted names at all. */
enum { NAV_DOC, NAV_ORIGIN, NAV_NAME, NAV_PARENT, NAV_OPENER, NAV_FIELD_N };

/* The DOCUMENT NAME of the navigable a parent or opener slot holds, or "" when it holds none. */
static const char *nav_slot_doc(JSValueConst v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) return "";
    DCHECK(window_proxy_is(v),
           "a navigable's parent or opener slot holds something that is not a WindowProxy. Both slots are "
           "NAVIGABLES — window_proxy.c keeps them un-mapped for exactly this reason, and the member accessors "
           "are what turn this document's own navigable into the Window a page compares against — so a value "
           "here that is not one has no document name to cross under and no navigable to name");
    if (!window_proxy_is(v)) return "";
    return world_doc_name(window_proxy_doc(v));
}

/* One base64 field, appended at `*pn` and terminated by '.'. */
static void nav_put(char *out, size_t cap, size_t *pn, const char *s)
{
    size_t len = strlen(s);
    size_t n;

    CHECK(*pn + JS_Base64EncodedSize(len) + 2 <= cap,
          "remote object: a navigable's identity did not fit the record sized for it — a truncated identity "
          "names a DIFFERENT document, and the peer would resolve it to a navigable that is not the one meant");
    n = JS_Base64Encode(out + *pn, cap - *pn, (const uint8_t *)s, len);
    CHECK(n > 0 || len == 0, "the base64 buffer was sized wrong for a field of a navigable's identity");
    *pn += n;
    out[(*pn)++] = '.';
    out[*pn] = 0;
}

static char *nav_encode(JSContext *ctx, JSValueConst nav)
{
    JSValue parent = window_proxy_parent_navigable(ctx, nav);
    JSValue opener = window_proxy_opener_navigable(ctx, nav);
    const char *f[NAV_FIELD_N];
    size_t cap = 2, n = 1;
    char *out;
    int i;

    f[NAV_DOC]    = world_doc_name(window_proxy_doc(nav));
    f[NAV_ORIGIN] = origin_serialized(window_proxy_origin(nav));
    f[NAV_NAME]   = window_proxy_name(nav);
    f[NAV_PARENT] = nav_slot_doc(parent);
    f[NAV_OPENER] = nav_slot_doc(opener);
    for (i = 0; i < NAV_FIELD_N; i++) {
        DCHECK(f[i] != NULL,
               "a field of a navigable's identity is absent where the record has a place for it — every one of "
               "the five is a computed fact about the navigable, so an absent one is a read that failed rather "
               "than a navigable that has none, and \"\" is how having none is stated");
        cap += JS_Base64EncodedSize(f[i] ? strlen(f[i]) : 0) + 2;
    }
    out = malloc(cap);
    CHECK(out != NULL, "remote object: OOM naming a navigable for another agent");
    out[0] = NAV_TAG;
    out[1] = 0;
    for (i = 0; i < NAV_FIELD_N; i++) nav_put(out, cap, &n, f[i] ? f[i] : "");
    JS_FreeValue(ctx, parent);
    JS_FreeValue(ctx, opener);
    return out;
}

/* A parent or opener slot of an ARRIVING identity: the navigable that document name names, which this agent
   must already hold. `none` is what the slot's absence means — JS_UNDEFINED for a parent (§7.2.2.4 makes a
   top-level navigable's `parent` itself) and JS_NULL for an opener. */
static JSValue nav_slot_resolve(JSContext *ctx, const char *doc_name, JSValue none)
{
    JSValue w;

    if (!*doc_name) return none;
    w = window_proxy_of_document(ctx, world_doc_intern(doc_name));
    DCHECK(!JS_IsUndefined(w),
           "a navigable's identity named a PARENT or OPENER navigable this agent holds no WindowProxy for. One "
           "document name is enough to RESOLVE a navigable already known here and not enough to MINT one — a "
           "minted parent needs its own origin, name, parent and opener, which is the identity of a second "
           "navigable. So the CHAIN has to cross: an identity whose parent or opener is unknown here carries "
           "that navigable's identity too, nearest first, exactly as a world carries its ancestry and for the "
           "same reason (the far end holds some prefix of it and no more)");
    if (JS_IsUndefined(w)) return none;
    return w;
}

static JSValue nav_decode(JSContext *ctx, const char *text)
{
    char *f[NAV_FIELD_N] = {0};
    const char *p = text + 1;
    JSValue parent, opener, out;
    uint32_t doc;
    int i;

    for (i = 0; i < NAV_FIELD_N; i++) {
        const char *dot = strchr(p, '.');
        char *slice;
        size_t len = 0;

        DCHECK(dot != NULL,
               "a navigable's identity ran out of fields — every field is TERMINATED by a '.', so a record "
               "that ends early was truncated in transit, and resolving the part that arrived would name a "
               "different navigable rather than fail");
        if (!dot) { while (i-- > 0) free(f[i]); return JS_UNDEFINED; }
        slice = strndup(p, (size_t)(dot - p));
        CHECK(slice != NULL, "remote object: OOM reading a navigable's identity");
        f[i] = dec_b64(slice, &len);
        free(slice);
        /* EVERY FIELD IS READ AS A C STRING from here on — world_doc_intern keys the document table by one,
           origin_parse parses one, and the browsing-context name is stored as one — so a NUL inside a field is
           a different document, a different principal or a different name, silently. The base64 carries it
           faithfully, which is exactly why the truncation has to be caught where the bytes are still counted. */
        DCHECK(strlen(f[i]) == len,
               "a field of a navigable's identity carries a NUL, and every consumer of it reads a C string — a "
               "document name that stops early names a DIFFERENT navigable, an origin that does resolves to a "
               "different principal, and both answer as though nothing was lost");
        p = dot + 1;
    }
    DCHECK(*p == 0,
           "a navigable's identity carried MORE than the fields this agent reads — the two sides disagree "
           "about what a navigable IS, and the fields this one ignored are the ones the disagreement is in");

    doc = world_doc_intern(f[NAV_DOC]);
    parent = nav_slot_resolve(ctx, f[NAV_PARENT], JS_UNDEFINED);
    opener = nav_slot_resolve(ctx, f[NAV_OPENER], JS_NULL);
    /* THE ONE DOOR: a name this agent HOSTS resolves to its own WindowProxy, a name it has already seen
       resolves to the proxy it minted then, and only a name it has never held is minted now. */
    out = window_proxy_for_document(ctx, doc, origin_parse(f[NAV_ORIGIN]), f[NAV_NAME], parent, opener);
    CHECK(!JS_IsException(out), "remote object: a navigable's WindowProxy could not be allocated");
    JS_FreeValue(ctx, parent);
    JS_FreeValue(ctx, opener);
    for (i = 0; i < NAV_FIELD_N; i++) free(f[i]);
    return out;
}

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
        size_t len = 0;
        const char *t = JS_ToCStringLen(ctx, &len, v);
        CHECK(t != NULL, "remote object: OOM encoding a string for another agent");
        out = enc_b64('s', t, len);
        JS_FreeCString(ctx, t);
        return out;
    }
    if (JS_IsSymbol(v)) {
        /* A WELL-KNOWN SYMBOL IS A DIFFERENT VALUE IN EVERY AGENT AND THE SAME SLOT IN ALL OF THEM, so it
           crosses as the name of the slot. This is what makes `"" + remote` possible at all: a string
           coercion performs Get(remote, @@toPrimitive), which is a symbol-keyed read on the first line of
           code that so much as logs a cross-agent object. */
        int k;
        char *key = NULL;
        size_t keylen = 0;

        DCHECK(g_wk_n > 0,
               "a symbol crossed an agent boundary before this agent captured its well-known symbols — "
               "remote_object_init has not run, and every well-known key would be reported as a unique symbol "
               "by a table that simply has nothing in it");
        k = wk_by_value(v);
        if (k >= 0) {
            size_t cap = strlen(g_wk[k].desc) + 2;
            out = malloc(cap);
            CHECK(out != NULL, "remote object: OOM naming a well-known symbol for another agent");
            snprintf(out, cap, "w%s", g_wk[k].desc);
            return out;
        }
        /* A REGISTERED SYMBOL IS ITS KEY. 20.4.2.2 makes `Symbol.for(k)` the agent's ONE symbol for k, so the
           key is the whole of the identity and the peer's own registry answers with the peer's own symbol. */
        if (sym_registered_key(ctx, v, &key, &keylen)) {
            out = enc_b64('g', key, keylen);
            free(key);
            return out;
        }
        DFAIL("a UNIQUE symbol crossed an agent boundary. Unlike a well-known symbol (which denotes the same "
              "slot in every agent) and a registered one (which IS its Symbol.for key), a unique symbol is "
              "identity and nothing else, so it crosses the way an object does: EXPORT it — an id minted once "
              "per symbol in this agent's export table, carried as `<document>:<id>` — and give the importing "
              "agent a per-(doc, gen, id) LOCAL symbol standing for it, with the pairing remembered in both "
              "directions so a name that comes home resolves to the original symbol and `k === k` holds on "
              "both sides. That local stand-in is a symbol and not a Proxy, which is why it is a mechanism of "
              "its own rather than a call to ref_mint");
        return strdup("u");
    }
    /* A NAVIGABLE IS NOT AN OBJECT THIS AGENT MAY LEND, and it is stopped ABOVE the generic export rather than
     * inside it, because everything below this line is right for a Document, an Error or a page's own object
     * and wrong for a window in exactly the way that leaves no trace.
     *
     * WHAT WOULD HAPPEN OTHERWISE. `otherW[0]`'s peer-side program answers with a WindowProxy (§7.2.2.2), and
     * the export below would name it `o<doc>:<gen>:<id>` — so the asking agent would build a REFERENCE PROXY where a
     * WindowProxy belongs. Every consequence of that is silent: `w[0].postMessage(…)` would park a flow on an
     * `object.apply` instead of running §7.2.1's postMessage; `w[0] === w.frames[0]` would be false about one
     * navigable; §7.2.1's cross-origin FILTER would not run at all, because a reference proxy forwards every
     * key to the peer and the whole point of the WindowProxy surface is that thirteen names are answerable and
     * the rest are a SecurityError. The two spellings of a window are one navigable (window_proxy.c says so
     * about `parent`, `top` and `closed`), so the Window GLOBAL is refused here on the same line as its proxy —
     * `remote.defaultView` reaches this with the peer's global and would lend the same wrong thing.
     *
     * WHAT CROSSES INSTEAD — and it is nav_encode above, the way `navigable.create` and a routed message's
     * sender already crossed one (navigable.c, window_message.c each build a remote proxy from one): the five
     * facts that decide WHICH navigable it is. The receiving side resolves the DOCUMENT NAME first — a name
     * this agent hosts is its OWN WindowProxy, never a remote one — and only a name it holds no proxy for at
     * all is minted, once, because `w[0] === w[0]` is a page-visible identity and this file already keeps one
     * reference per (doc, gen, id) for exactly that reason. THE KEEPING IS window_proxy.c's, because a navigable's
     * proxy is the navigable's, and its row BORROWS: core/dom/document.h refuses an OWNING table keyed by
     * document, and correctly, since a navigable is created per `open()` per flow and owning rows would root
     * every one the frontier ever made. A borrowed row is cleared by proxy_finalizer, so the table is the set
     * of remote navigables still alive and keeps nothing alive itself. */
    {
        JSValueConst nav = window_proxy_navigable_of(ctx, v);   /* the two spellings, resolved in one place */

        if (!JS_IsUndefined(nav)) return nav_encode(ctx, nav);
        /* AND THE THIRD SPELLING IS NOT ONE. An instance is an origin-keyed agent CLUSTER, so a Window global
           of ANOTHER realm of this agent can reach here — and it is a navigable this file cannot name, because
           window_proxy_navigable_of resolves a global against the realm it was handed. Crossing it as an
           object would lend that navigable as `o<doc>:<gen>:<id>`, which is the whole failure above. */
        DCHECK(!window_is(v),
               "a Window global of another realm of THIS agent was about to cross an agent boundary as a "
               "generic object reference. It is a navigable and must cross as its IDENTITY, but the mapping "
               "from a global to its navigable is asked of the realm the encoder was handed, and this value "
               "belongs to a different one. Reach the navigable from the WINDOW rather than from the realm — "
               "the global's own realm is what document_window_proxy is keyed on — or encode in the realm the "
               "value came out of, which is what flow_answer_perform already does for the value it answers");
    }
    if (JS_IsObject(v)) {
        /* AN OBJECT CROSSES AS ITS NAME, AND THE NAME SAYS WHOSE. A value that is ALREADY a reference re-emits
           the name it arrived with — exporting the proxy instead would make a round trip a proxy of a proxy,
           and the page's `===` would answer false about an object that never moved. */
        RefData *r = ref_of_proxy(ctx, v);
        uint32_t doc = r ? r->doc : world_local_doc();
        /* THE GENERATION IS THE EXPORTING SESSION'S — this one's for an object of this heap, and the one the
           reference ARRIVED with for a name that is going home. It is world.h's, not a counter of this file's:
           the export table and the world registry are the same session, and two counters for one fact are two
           answers to it. */
        uint32_t session = r ? r->session : world_session();
        uint32_t id  = r ? r->id  : remote_object_export(ctx, v);
        const char *doc_name = world_doc_name(doc);
        char tag = JS_IsConstructor(ctx, v) ? 'c' : JS_IsFunction(ctx, v) ? 'f' : 'o';
        size_t cap = strlen(doc_name) + 40;

        DCHECK(!strchr(doc_name, '\t') && !strchr(doc_name, '\n'),
               "a document name carries a tab or a newline — these records are one tab-separated line, so the "
               "peer would read one field as two and the id would come back as zero");
        out = malloc(cap);
        CHECK(out != NULL, "remote object: OOM naming an object for another agent");
        snprintf(out, cap, "%c%s:%u:%u", tag, doc_name, session, id);
        return out;
    }
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
        size_t len = 0;
        char *bytes = dec_b64(text + 1, &len);
        JSValue s = JS_NewStringLen(ctx, bytes, len);
        free(bytes);
        return s;
    }
    /* A WELL-KNOWN SYMBOL RESOLVES TO THIS AGENT'S OWN — the name denotes the SLOT, and answering with a
       symbol minted here would be a value that keys nothing the peer meant. */
    case 'w': {
        int k = wk_by_desc(text + 1);
        DCHECK(k >= 0, "a peer named a well-known symbol this agent does not have — 6.1.5's table is read out "
                       "of each agent's own %Symbol%, so the two engines disagree about which symbols exist "
                       "and there is no slot here for the one the peer meant");
        if (k < 0) return JS_UNDEFINED;
        return JS_DupValue(ctx, g_wk[k].sym);
    }
    /* A REGISTERED SYMBOL IS LOOKED UP IN THIS AGENT'S OWN REGISTRY, which is what 20.4.2.2 makes it: the key
       decides the symbol, so `Symbol.for(k)` here and there name one another's. */
    case 'g': {
        size_t len = 0;
        char *key = dec_b64(text + 1, &len);
        JSValue s;
        DCHECK(strlen(key) == len,
               "a registered symbol's key carries a NUL — JS_NewSymbol names a key by a C string, so this "
               "agent would look up a shorter key, which is a different symbol");
        s = JS_NewSymbol(ctx, key, true);
        free(key);
        CHECK(!JS_IsException(s), "remote object: OOM resolving a registered symbol from another agent");
        return s;
    }
    /* A NAVIGABLE, WHICH IS AN IDENTITY AND NEVER A REFERENCE — see nav_decode and the block above the
       encoder's own refusal to lend one. */
    case NAV_TAG: return nav_decode(ctx, text);
    case 'o': case 'f': case 'c': {
        const char *colon = strrchr(text + 1, ':'), *gen;
        char *doc_name;
        uint32_t doc, session, id;

        DCHECK(colon != NULL, "a cross-agent object name carried no document — an id alone means nothing "
                              "outside the agent that minted it, and resolving it here would answer with "
                              "whatever this agent happens to have lent under that number");
        if (!colon) return JS_UNDEFINED;
        id = (uint32_t)strtoul(colon + 1, NULL, 10);
        /* AND THE GENERATION IMMEDIATELY BEFORE IT. Scanned backwards rather than forwards for the same reason
           the serial is taken from the LAST colon: a document name is "<parent>.<n>" over a root the host may
           name anything, so only the two separators at the end are known to be the ones. */
        for (gen = colon; gen > text + 1 && gen[-1] != ':'; gen--)
            ;
        DCHECK(gen > text + 1,
               "a cross-agent object name carried no GENERATION — an export id is an index into ONE session's "
               "table, so a name without one names a different object in every session of the document that "
               "lent it, and this agent would resolve it against whichever session is running now");
        if (gen <= text + 1) return JS_UNDEFINED;
        session = (uint32_t)strtoul(gen, NULL, 10);
        doc_name = strndup(text + 1, (size_t)(gen - 1 - (text + 1)));
        CHECK(doc_name != NULL, "remote object: OOM reading a cross-agent object name");
        doc = world_doc_intern(doc_name);
        free(doc_name);
        /* A NAME THAT CAME HOME. The object is in THIS heap and is reached directly: minting a reference for it
           would route a read back out to a peer for a question this agent is the answer to, and would make
           `x === theSameXComingBack` false. Home is (document AND session): a name of an earlier session of
           this document is refused by remote_object_by_id rather than resolved against the live table. */
        if (doc == world_local_doc()) {
            JSValueConst held = remote_object_by_id(session, id);
            DCHECK(!JS_IsUndefined(held),
                   "a peer named an object of THIS agent that was never lent — the name it used was minted "
                   "somewhere else, or the export table was lost between the lend and the return");
            return JS_DupValue(ctx, held);
        }
        DCHECK(!world_doc_hosted(doc),
               "a cross-agent name resolved to a document THIS agent holds under a foreign name — one document "
               "would then have two identities, and an object of it would answer `===` against itself as false");
        return ref_mint(ctx, doc, session, id, text[0] != 'o', text[0] == 'c');
    }
    default: break;
    }
    DFAIL("a cross-agent record carried a typed value under a tag this protocol does not name");
    return JS_UNDEFINED;
}

/* ---- AND WHAT AN ANSWER IS: A COMPLETION ----------------------------------------------------------------- */

/* THE TYPE IN FRONT OF THE VALUE. Neither character is a value tag, so a value record read as a completion —
   or a completion read as a value — crashes at the decoder instead of resolving a peer's throw to a `u`. */
#define REMOTE_COMPLETION_NORMAL_TAG '.'
#define REMOTE_COMPLETION_THROW_TAG  '!'

char *remote_completion_encode(JSContext *ctx, int completion, JSValueConst v)
{
    char *value = remote_object_encode(ctx, v);
    size_t cap = strlen(value) + 2;
    char *out = malloc(cap);

    DCHECK(completion == ENGINE_COMPLETION_NORMAL || completion == ENGINE_COMPLETION_THROW,
           "a cross-agent answer was encoded under a completion type ECMA-262 6.2.4 does not have — an "
           "operation performed in another instance returns or throws, and nothing crosses a call site");
    CHECK(out != NULL, "remote object: OOM encoding a completion for another agent");
    out[0] = completion == ENGINE_COMPLETION_THROW ? REMOTE_COMPLETION_THROW_TAG
                                                   : REMOTE_COMPLETION_NORMAL_TAG;
    memcpy(out + 1, value, cap - 1);
    free(value);
    return out;
}

JSValue remote_completion_decode(JSContext *ctx, const char *text, int *pcompletion)
{
    DCHECK(pcompletion != NULL,
           "a cross-agent completion was decoded with nowhere to put its TYPE — the type is the half that says "
           "whether the value is a result or a throw, and a caller that cannot receive it will deliver a throw "
           "as a value");
    CHECK(text != NULL, "remote object: a cross-agent answer arrived as no text at all");
    DCHECK(*text == REMOTE_COMPLETION_NORMAL_TAG || *text == REMOTE_COMPLETION_THROW_TAG,
           "a cross-agent answer did not begin with a completion type — every answer is a completion record, "
           "so a record that starts with a value tag was written by something that does not share this grammar");
    *pcompletion = *text == REMOTE_COMPLETION_THROW_TAG ? ENGINE_COMPLETION_THROW : ENGINE_COMPLETION_NORMAL;
    return remote_object_decode(ctx, text + 1);
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

/* §7.2.1's read, one agent further out, and its three siblings:
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
        /* 6.1.7: A PROPERTY KEY IS A STRING OR A SYMBOL, and both cross — the encoder owns which of the three
           symbol encodings a symbol takes, so there is nothing for this site to decide. */
        DCHECK(op == REF_OP_APPLY || JS_IsString(argv[1]) || JS_IsSymbol(argv[1]),
               "a cross-agent keyed operation was handed a key that is neither a string nor a symbol — a "
               "property key is one of the two, and the trap is handed the key the operator site already "
               "converted");
        /* 10.5.8 step 9 / 10.5.9 step 9 hand the trap the RECEIVER, and it is the reference itself for every
           ordinary `remote.x` / `remote.x = v`. When it is anything else — `Object.create(remote).x = v`,
           `Reflect.set(remote, k, v, other)` — the peer's OrdinarySetWithOwnDescriptor ends on the RECEIVER
           (10.1.9.2 step 3 does CreateDataProperty on it), which is an object in THIS agent. */
        if (op == REF_OP_GET || op == REF_OP_SET) {
            RefData *rr = ref_of_proxy(ctx, argv[op == REF_OP_SET ? 3 : 2]);
            if (!rr || rr->doc != r->doc || rr->id != r->id) {
                DFAIL("a cross-agent [[Get]]/[[Set]] whose RECEIVER is not the reference itself. 10.1.9.2 ends "
                      "on the receiver, so the peer — mid-operation, inside the program that is answering this "
                      "request — would have to perform an internal method on an object of the agent that ASKED. "
                      "That is a request in the reverse direction, and it is not one more record on this "
                      "channel: (1) the channel is a synchronous call-and-answer, one question outstanding, so "
                      "the asking host is BLOCKED reading the answer line and cannot answer anything while it "
                      "waits — both directions have to become asynchronous, id-multiplexed records rather than "
                      "a write followed by a read; (2) the asking agent must answer BY RUNNING A PROGRAM under "
                      "the peer's flow's world, which it can only do from its own pump, so the peer's flow has "
                      "to PARK on a request of its own at 10.1.9.2 step 3 — the answering side needs the same "
                      "suspend the asking side has, which is what makes it a flow rather than a callback; and "
                      "(3) the peer's serve loop installs ONE world's segment per turn and holds it across the "
                      "answer, so a half-answered operation parked mid-turn needs the world switch to belong to "
                      "the parked operation instead of to the loop. Build those three and the receiver crosses "
                      "as a name like any other object");
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
        /* THE OBJECT'S NAME IS (GENERATION, ID), the same pair the value grammar puts after the document. An
           id alone is an index into whichever of that document's sessions is running when the record lands. */
        snprintf(idbuf, sizeof idbuf, "\t%u:%u", r->session, r->id);
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
    /* THE PEER'S COMPLETION, RAISED WHERE THE OPERATION WAS WRITTEN. The peer performs the internal method by
       running a program — an IDL accessor, a page's setter, a page's function — and every one of those can
       throw. The throw belongs at THIS call site, so the page's own `try { remote.x = 1 } catch (e)` around
       the operation runs its handler exactly as it would for a local object, and the reference stops being the
       one kind of object whose failures are silently `undefined`. */
    {
        int r = engine_host_take_completion(ctx, s->req, presult);
        s->req = 0;
        if (r != JS_STEP_DONE) return r;
    }
    /* 10.5.9 / 10.5.10 ToBoolean the trap's result, so a non-boolean here is not a page's doing — it is the
       peer having answered a [[Set]] or a [[Delete]] with something that is not the operation's completion.
       It is asked of a NORMAL completion only: a throw completion's value is the thrown value, and 10.5.9's
       boolean does not exist on a path that did not complete. */
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

static JSValue ref_mint(JSContext *ctx, uint32_t doc, uint32_t session, uint32_t id,
                        bool callable, bool constructor)
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

    /* ONE REFERENCE PER (doc, session, id): the other half of identity. The exporter mints one id per object;
       without this the same id would arrive twice as two objects and `w.document === w.document` would be
       false on this side instead of the other. The generation is in the key because it is in the NAME — two
       sessions of one document both mint id 1, and a key without it would answer the second session's name
       with the first session's proxy, which is the importing half of the same defect. */
    snprintf(key, sizeof key, "%u:%u:%u", doc, session, id);
    found = JS_GetPropertyStr(ctx, g_refs, key);
    if (JS_IsObject(found)) {
        RefData *had = ref_of_proxy(ctx, found);
        (void)had;
        DCHECK(had != NULL, "the reference table held something that is not a reference under a (doc, gen, id) key");
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
       carries the (doc, gen, id) as an internal slot instead, which is not a property and constrains nothing —
       and it carries CALLABILITY in its CLASS, because JS_NewProxy reads that once and a reference over a
       plain target has no [[Call]] at all. */
    target = JS_NewObjectClass(ctx, callable ? g_ref_fn_class : g_ref_class);
    CHECK(!JS_IsException(target), "a cross-agent reference's target could not be allocated");
    r = calloc(1, sizeof *r);
    CHECK(r != NULL, "remote object: OOM building a cross-agent reference");
    r->doc = doc;
    r->session = session;
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

/* THE CLASS THIS RAN FOR IS NOT ASKED, AND THE ASSERT THAT ASKED IT WAS GUARANTEED TO FIRE. It compared `id`
   against g_ref_class and g_ref_fn_class, both of which remote_object_free sets to 0 — and that release runs
   BEFORE the collection which finalizes the page's object graph, so ANY reference a peer's document still held
   at teardown reached this line with two zeroes to compare against and aborted on a `@WHY` about a defect that
   was not there. Reading the opaque with JS_GetAnyOpaque was already right; the assert beside it was the whole
   bug, which is why the rule is "a finalizer reads NO static its own release resets" and not "use
   JS_GetAnyOpaque" (see core/agent_state.h). Nothing is lost by dropping it: the collector dispatches a
   finalizer THROUGH the class whose JSClassDef names it, so the two reference classes are the only callers
   there are, and the record is the same for both. */
static void ref_finalizer(JSRuntime *rt, JSValue val)
{
    JSClassID id = 0;
    RefData *r = JS_GetAnyOpaque(val, &id);
    (void)rt; (void)id;
    /* ref_mint attaches the identity with nothing between JS_NewObjectClass and JS_SetOpaque that allocates on
       the JS heap or returns, so a reference target with no identity has never existed. */
    DCHECK(r != NULL, "a cross-agent reference's target was finalized with no (doc, session, id) — ref_mint "
                      "attaches one before the target can reach a proxy or a collection");
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
    int k;
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

    /* THIS AGENT'S WELL-KNOWN SYMBOLS, taken from its own %Symbol% BEFORE any document script runs — the same
       moment and the same reason the peer captures %Reflect.set%. A page that replaces the `Symbol` binding
       cannot make a cross-agent `remote[Symbol.iterator]` name a different slot. */
    wk_capture(ctx);

    /* WHAT THIS COMPONENT HOLDS FOR THE AGENT, DECLARED — core/agent_state.h. The release below already gave
       every one of these back; declaring them is what makes that CHECKABLE, and the row pairing in
       core/platform.c reads the declaration as the other half of the release column. The two counts beside
       g_exports and g_wk are not declared: a count left set beside a null table is a NULL dereference at the
       first read, which is loud, while a handle left set is the silence that header is about. */
    agent_state_class(RO_COMPONENT, &g_ref_class,
                      "the CrossAgentReference target class, and the declaration latch");
    agent_state_class(RO_COMPONENT, &g_ref_fn_class, "the CrossAgentFunctionReference target class");
    agent_state_value(RO_COMPONENT, &g_refs, "the (doc, session, id) -> reference table this agent imported");
    agent_state_ptr(RO_COMPONENT, &g_exports, "this session's export table — the objects this agent has lent");
    agent_state_ptr(RO_COMPONENT, &g_wk, "ECMA-262 6.1.5 Table 1's well-known symbols, captured from this "
                                         "agent's own %Symbol% before any page script ran");
    for (k = 0; k < REF_OP_N; k++)
        agent_state_id(RO_COMPONENT, &g_ref_stepid[k], REF_STEP_WHAT[k]);
}

/* THE AGENT'S HALF, UNDONE — core/platform.h's third column, and it takes the RUNTIME because that is what an
   agent is. It took a JSContext until this diff, and that signature is the whole of what kept this component
   off the column: all three hosts wrote `remote_object_free; window_proxy_free; remote_location_free;` by
   hand, which releases the BASE whose §7.2.3 prototype every reference minted here chains to BEFORE the two
   components built over it. JS_FreeValueRT reaches the same values JS_FreeValue did — JS_FreeValue IS
   JS_FreeValueRT(ctx->rt, v). */
void remote_object_free(JSRuntime *rt)
{
    uint32_t i;
    int k;

    DCHECK(g_ref_class != 0,
           "the cross-agent reference surface was released in an agent that never declared it — "
           "remote_object_init is a row on core/platform.c's declare column, so reaching here without it is a "
           "teardown of a browser that was never brought up");
    /* AN EXPORT ID IS AN IDENTITY IN THIS SESSION'S TABLE, AND A PARKED INSTANCE COMES BACK AS A NEW ONE — so
       the name carries the SESSION and this call has nothing left to refuse. It used to abort a park taken
       while any export was live, which was the right crash for as long as `o<doc>:<id>` was all a peer had:
       the resumed session mints ids from 1 again under a document name that is stable by requirement, so a
       surviving peer's name resolved IN RANGE to an unrelated object with `===` answering wrong and nothing
       saying so. The refusal is deleted rather than kept beside the fix, because it is a CAP on Level-1
       eviction — it forbids exactly the case that eviction is for, one document's engine leaving memory while
       the instance reading from it stays — and because what it was guarding is now impossible: a name from an
       ended session is REFUSED at the point it is used (remote_object_by_id), which is where the reader
       actually is and what names the capability still to build. */
    for (i = 0; i < g_exports_n; i++) JS_FreeValueRT(rt, g_exports[i].v);
    free(g_exports);
    g_exports = NULL;
    g_exports_n = g_exports_cap = 0;
    /* THE WELL-KNOWN TABLE HOLDS A REFERENCE PER SYMBOL and a string per description — both are this file's,
       and the runtime's own leak walk counts the symbol if this does not. */
    for (k = 0; k < g_wk_n; k++) { JS_FreeValueRT(rt, g_wk[k].sym); free(g_wk[k].desc); }
    free(g_wk);
    g_wk = NULL;
    g_wk_n = 0;
    JS_FreeValueRT(rt, g_refs);
    g_refs = JS_UNDEFINED;
    /* THE TWO CLASS IDS COME BACK, and ref_finalizer therefore reads neither: the collection that finalizes a
       reference target runs AFTER this call, so a class id is the one thing that cannot identify one by then.
       See core/agent_state.h. */
    g_ref_class = g_ref_fn_class = 0;
    for (k = 0; k < REF_OP_N; k++) g_ref_stepid[k] = -1;
}
