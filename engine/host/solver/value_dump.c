/* THE JSON DUMP OF A JSValue — see value_dump.h for what this is NOT and why that distinction is the whole of
 * its licence to exist.
 *
 * THE WALK IS AN EXPLICIT STACK AND NOT C RECURSION, for §25.5.4's own machine's reason stated one file over:
 * "a recursive walk's position is a stack and not a number". The value being dumped is a PAGE-BUILT graph, so
 * its depth is the page's to choose, and a C recursion over one is a C-stack limit — which §C-stack makes
 * impossible by construction everywhere else in this engine and which no cap here may re-introduce. The stack
 * grows by doubling and aborts on OOM; there is no depth at which it stops.
 *
 * EVERY REFUSAL IS A `DFAIL` AND NEVER A WRITTEN VALUE. A kind this cannot express is §Offensive-programming's
 * second category — a capability that should exist and does not — so it names what to build at the site. The
 * one thing it may never do is write a plausible datum: a `0` for a value nobody produced is the defect
 * testing/render_diff.js's whole comparator is built to make impossible, and an instrument that commits it
 * reports a MEASUREMENT where there was none. */
#include "solver/value_dump.h"
#include "solver/concolic.h"
#include "core/json_buf.h"
#include "check.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ONE CONTAINER BEING WALKED. `holder` is OWNED (the walk dups what it descends into, so a page write that
   drops the parent mid-walk cannot free it underneath us — this runs on the host's own time, but the rule is
   the same one every other walk in this engine keeps and costs one refcount).
   AN OBJECT AND AN ARRAY ARE ONE FRAME WITH TWO READERS rather than two frame kinds, because the only thing
   that differs is where the NEXT key comes from: an object's from its own enumerable string keys, an array's
   from the index. `tab` is NULL exactly when `alen` is not -1, and that pair is asserted rather than described. */
typedef struct {
    JSValue         holder;
    JSPropertyEnum *tab;     /* an object's own enumerable string keys, OWNED; NULL for an array */
    uint32_t        n;       /* how many of them */
    /* THE CURSOR IS WIDER THAN AN ARRAY INDEX ON PURPOSE. §10.4.2.2 ArrayCreate caps an Array's length at
       2**32 - 1, so a `uint32_t` cursor over the LAST possible element wraps to 0 on its increment and the
       walk never terminates — a §NO-BOUNDS-shaped defect reached by choosing a type rather than by writing a
       cap, and unreachable today only because that array does not fit in memory. `int64_t` cannot wrap inside
       the range the standard permits. */
    int64_t         i;       /* the cursor into them, or into the array's indices */
    int64_t         alen;    /* an array's `length`; -1 when this frame is an object */
} DumpFrame;

typedef struct { DumpFrame *f; int sp, cap; } DumpStack;

/* THE EXAMPLE, TAKEN — `concolic_is(v) ? concolic_example(ctx, v) : JS_DupValue(ctx, v)`, this engine's
   routine spelling, with the one thing a general walk has to add: an unknown that carries NO example is a
   state the engine is legitimately in (the value depends on something nothing has determined), so it is
   REPORTED and never asserted on. `*pshape` is that unknown's display shape, borrowed from `v` and valid
   while the container holding it is alive — which it is, because the frame owns it. */
static JSValue dump_resolve(JSContext *ctx, JSValueConst v, const char **pshape)
{
    JSValue ex;

    *pshape = NULL;
    if (!concolic_is(v))
        return JS_DupValue(ctx, v);
    ex = concolic_example(ctx, v);
    if (JS_IsUndefined(ex)) {
        *pshape = concolic_shape_c(v);
        DCHECK(*pshape != NULL,
               "an unknown with no example answered no display shape either — concolic_shape_c answers NULL "
               "only for a value that is not concolic, and concolic_is has just said this one is, so the two "
               "predicates disagree about one value and this dump would have nothing at all to state");
        return JS_UNDEFINED;
    }
    /* AN EXAMPLE IS CONCRETE BY DEFINITION, so a chain is not a case to loop over — it is two mechanisms
       disagreeing. A resolve written as a loop would hide that and answer the innermost example as if the
       outer unknown had carried it. */
    DCHECK(!concolic_is(ex),
           "an unknown's EXAMPLE is itself an unknown — an example is the CONCRETE value the run computed "
           "beside the domain (solver/concolic.h), so a concolic one is a value minted where a concrete one "
           "was owed, and every consumer of this example is being handed a second domain to narrow");
    return ex;
}

/* A NUMBER, IN §25.5.4.2 SerializeJSONProperty STEP 9'S OWN SPELLING: "If value is finite, return
   ! ToString(value)" and otherwise `"null"`. ToString is what JSON uses, so it is what this uses — a
   `snprintf` here would write a representation the other side of a comparison never produces, which is a
   divergence manufactured by the instrument. It runs no page code: ToString of a Number is §6.1.6.1.20
   Number::toString and reaches no [[Get]] and no call.
   `null` FOR A NON-FINITE IS THE STANDARD'S ANSWER AND NOT A DEFAULT THIS FILE PICKED, and a consumer that
   tests for a Number reads it as an absence with the same machinery it reads every other one. */
static void dump_number(JSContext *ctx, JsonBuf *b, JSValueConst v)
{
    double d;
    const char *s;
    int r;

    /* PERFORMED ON ITS OWN LINE AND NOT INSIDE THE DCHECK'S CONDITION. check.h compiles a DCHECK's condition
       to `sizeof(cond)` in release — never evaluated — so a conversion written there would leave `d`
       UNINITIALISED in exactly the build that cannot report it, and the finiteness test below would be a
       branch on a stack slot. Every out-parameter call in this file is on its own line for that reason. */
    r = JS_ToFloat64(ctx, &d, v);
    DCHECK(r == 0,
           "ToNumber failed on a value this walk had already established is a Number — the two predicates "
           "disagree, and the digits that would be written are a conversion nobody performed");
    if (!isfinite(d)) { json_buf_raw(b, "null"); return; }
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL, "value_dump: OOM writing a number — the artifact this dump is composing is lost at the "
                     "one line that would have reported it");
    json_buf_raw(b, s);
    JS_FreeCString(ctx, s);
}


static void dump_push(JSContext *ctx, DumpStack *st, JsonBuf *b, JSValue holder)
{
    DumpFrame *f;

    if (st->sp == st->cap) {
        int cap = st->cap ? st->cap * 2 : 16;
        DumpFrame *g = realloc(st->f, (size_t)cap * sizeof *g);
        CHECK(g != NULL, "value_dump: OOM growing the walk's frame stack — the artifact being composed is lost "
                         "with it, and there is no depth at which this walk is allowed to stop instead");
        st->f = g;
        st->cap = cap;
    }
    f = &st->f[st->sp++];
    f->holder = holder;   /* OWNED from here; the pop releases it */
    f->tab = NULL;
    f->n = f->i = 0;
    f->alen = -1;
    if (JS_IsArray(holder)) {
        /* §25.5.4.6 SerializeJSONArray WALKS THE INDICES AND NOT THE OWN KEYS, which is not a preference: an
           own-key enumeration SKIPS a hole, so `[1,,3]` would come out as a two-element array and every later
           element would be joined against the wrong one. The standard writes `null` for the hole, which is
           what reading index by index and finding no own slot produces here.
           `JS_GetLength` IS A [[Get]] AND IS STILL NOT PAGE CODE HERE, which is worth stating because
           everything else in this file refuses one: an Array's `length` is its own non-configurable DATA
           property, so the read cannot reach a getter and cannot reach the prototype — and a Proxy, whose
           `get` trap WOULD be page code, has already been refused by name. */
        int lr = JS_GetLength(ctx, holder, &f->alen);
        DCHECK(lr == 0,
               "an Array answered no `length` — JS_GetLength reads the array's own length slot and this value "
               "has just been established to be one, so the two disagree about the same object");
        json_buf_raw(b, "[");
    } else {
        /* OWN, ENUMERABLE, STRING-KEYED, IN THE OBJECT'S OWN ORDER — §25.5.4.5 SerializeJSONObject's
           EnumerableOwnProperties(value, key), reached at its steps 5 and 6. A symbol key is not expressible in JSON and the standard does
           not walk one; a non-enumerable slot is not the page's data. */
        int er = JS_GetOwnPropertyNames(ctx, &f->tab, &f->n, holder,
                                        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);
        DCHECK(er == 0,
               "the own-property enumeration of a plain object failed — it allocates and it walks a shape, and "
               "a Proxy (whose enumeration is the `ownKeys` TRAP) has already been refused above, so there is "
               "no page code here that could have thrown");
        json_buf_raw(b, "{");
    }
}

static void dump_pop(JSContext *ctx, DumpStack *st, JsonBuf *b)
{
    DumpFrame *f = &st->f[--st->sp];

    json_buf_raw(b, f->alen >= 0 ? "]" : "}");
    if (f->tab) JS_FreePropertyEnum(ctx, f->tab, f->n);
    JS_FreeValue(ctx, f->holder);
}

/* THE RELEASE ARM OF EVERY REFUSAL BELOW, DESIGNED RATHER THAN FALLEN THROUGH. check.h compiles a `DFAIL` to
   `((void)0)` outside a dev build, so an arm that only aborts writes NOTHING in release — and a JSON object
   with a key and no value is not a narrower artifact, it is a text the consumer's parser rejects, which is the
   one failure that cannot be attributed to anything. So the refusal is also a WRITTEN, STATED ABSENCE, in the
   same spelling an exampleless unknown takes: a JSON STRING, which every consumer that tests for a Number
   sorts into its own column with a reason. The two builds then differ in whether the engine STOPS, never in
   what the artifact says. */
static void dump_refuse(JsonBuf *b, const char *what)
{
    json_buf_raw(b, "\"@WHY value_dump refused: ");
    /* THE KIND NAME IS THIS FILE'S OWN LITERAL and carries no quote, backslash or control byte, which is why
       it may be written into an already-open string rather than through json_buf_str — that entry writes its
       own quotes and there is no second opening quote to pair with. A caller passing anything computed would
       be passing text this line cannot escape, which is why every caller below passes a literal. */
    json_buf_raw(b, what);
    json_buf_raw(b, "\"");
}

/* ONE VALUE WRITTEN, OR ONE CONTAINER ENTERED. `v` is BORROWED. Returns 1 when a frame was pushed. */
static int dump_value(JSContext *ctx, DumpStack *st, JsonBuf *b, JSValueConst v)
{
    const char *shape;
    JSValue r = dump_resolve(ctx, v, &shape);

    if (shape != NULL) {
        /* THE STATED ABSENCE. Not `0`, not `null`, not an omitted member: a JSON STRING holding the unknown's
           own display shape, so a consumer testing for a Number sorts it into its own column WITH A REASON
           naming the source it was waiting on. This is the one thing an instrument owes that a producer of
           measurements does not — saying which of its outputs is not a measurement. */
        json_buf_str(b, shape);
        JS_FreeValue(ctx, r);
        return 0;
    }
    if (JS_IsNull(r) || JS_IsUndefined(r)) {
        /* §25.5.4.2 step 12's `Return undefined` is OMITTED by the object walk and written as `null` by
           the array walk; this is the array/member position that already decided to write something, so both
           arrive here as JSON `null`. An object member holding `undefined` is dropped at the caller below,
           which is where the standard drops it. */
        json_buf_raw(b, "null");
    } else if (JS_IsBool(r)) {
        json_buf_raw(b, JS_ToBool(ctx, r) ? "true" : "false");
    } else if (JS_IsNumber(r)) {
        dump_number(ctx, b, r);
    } else if (JS_IsString(r)) {
        const char *s = JS_ToCString(ctx, r);
        CHECK(s != NULL, "value_dump: OOM reading a string — the artifact being composed is lost with it");
        json_buf_str(b, s);
        JS_FreeCString(ctx, s);
    } else if (JS_IsProxy(r)) {
        /* A PROXY'S EVERY OWN-PROPERTY QUERY IS A TRAP, which is the page's code, and this walk runs on the
           host's own time with no flow base under it — the exact activation quickjs.h's
           JS_GetOwnPropertyNoUserCode aborts for. Refused HERE rather than at the enumeration so the message
           names the artifact rather than a failed allocation. */
        DFAIL("a Proxy reached the JSON dump — its `ownKeys` and `getOwnPropertyDescriptor` traps are the "
              "page's own code, and this walk has no flow base to run one on. A dump that must express one "
              "is a dump that has to be a scheduler flow: queue it as a program and read its completion, "
              "which is what solver/engine.c's DYN_VALUE_DUMP row already is");
        dump_refuse(b, "a Proxy, whose own-property query is the page's own trap");
    } else if (JS_IsSymbol(r)) {
        DFAIL("a Symbol reached the JSON dump — §25.5.4.2 SerializeJSONProperty has no arm that writes one "
              "and JSON has no syntax for it, so the value being dumped is not JSON-expressible. Build the "
              "producer so it does not put one in, or state in the artifact's own contract what a Symbol "
              "member means there");
        dump_refuse(b, "a Symbol, which JSON has no syntax for");
    } else if (JS_IsBigInt(r)) {
        DFAIL("a BigInt reached the JSON dump — §25.5.4.2 SerializeJSONProperty step 10 THROWS a TypeError "
              "for one, so there is no representation to write. A producer that needs to carry one carries "
              "its decimal STRING, which is what every wire format that has met this does");
        dump_refuse(b, "a BigInt, which SerializeJSONProperty step 9 throws for");
    } else if (JS_IsFunction(ctx, r)) {
        DFAIL("a function reached the JSON dump — §25.5.4.2 step 11's condition excludes a CALLABLE object, "
              "so one falls to step 12's `Return undefined`, which the object walk drops and the array walk "
              "writes as null — and either would be this dump silently "
              "answering about a member it cannot express. A producer whose artifact holds a function is "
              "producing something other than data");
        dump_refuse(b, "a function, which is code and not data");
    } else if (JS_IsObject(r)) {
        /* EVERY OTHER OBJECT IS WALKED AS DATA, which is where this dump and §25.5.4 part company on purpose:
           JSON.stringify would call a Date's or a Response's `toJSON` first, and calling one is the page's
           code. A Date therefore comes out as `{}` here — its state is an internal slot and not a member —
           and that is an honest account of what a dump of DATA can say about it, where a `toJSON` call would
           be this file becoming the algorithm quickjs.c deleted. */
        dump_push(ctx, st, b, r);   /* OWNERSHIP MOVES ONTO THE FRAME */
        return 1;
    } else {
        DFAIL("a value of a kind this JSON dump has no arm for reached it — every kind §25.5.4.2 names is "
              "answered above and a Proxy, a Symbol, a BigInt and a function are each refused BY NAME, so "
              "this is a value class the dump was never taught. Add its arm, or establish that the producer "
              "cannot make one");
        dump_refuse(b, "a value class this dump has no arm for");
    }
    JS_FreeValue(ctx, r);
    return 0;
}

char *value_dump_json(JSContext *ctx, JSValueConst v)
{
    JsonBuf b = { 0 };
    DumpStack st = { NULL, 0, 0 };

    DCHECK(ctx != NULL, "a JSON dump was asked with no realm — every read it performs is that realm's");

    if (!dump_value(ctx, &st, &b, v)) {
        DCHECK(st.sp == 0, "a leaf write left a frame on the walk's stack");
        free(st.f);
        return json_buf_take(&b);
    }

    while (st.sp > 0) {
        DumpFrame *f = &st.f[st.sp - 1];

        DCHECK((f->tab == NULL) == (f->alen >= 0),
               "a walk frame is neither an object nor an array — the key source and the length are one "
               "statement about which it is, so a frame holding both or neither has been written by "
               "something other than dump_push");

        if (f->alen >= 0) {
            JSValue el;
            JSAtom at;
            int got;

            if (f->i >= f->alen) { dump_pop(ctx, &st, &b); continue; }
            if (f->i) json_buf_raw(&b, ",");
            at = JS_NewAtomUInt32(ctx, (uint32_t)f->i);
            DCHECK(at != JS_ATOM_NULL, "an array index could not be interned");
            got = JS_GetOwnSlot(ctx, &el, f->holder, at);
            JS_FreeAtom(ctx, at);
            DCHECK(got >= 0,
                   "the own-slot read of an array element failed — JS_GetOwnSlot performs no [[Get]] and "
                   "refuses an accessor rather than calling one, so there is no page code here that could "
                   "have thrown and a failure is this engine disagreeing with itself about the slot");
            f->i++;
            /* A HOLE IS §25.5.4.6 SerializeJSONArray step 8.b's `null` — the standard's own answer for an
               index the array does not hold,
               and the reason this walk reads indices instead of own keys. */
            if (got == 0) { json_buf_raw(&b, "null"); continue; }
            if (dump_value(ctx, &st, &b, el)) { JS_FreeValue(ctx, el); continue; }
            JS_FreeValue(ctx, el);
            continue;
        }

        {
            JSValue val;
            const char *key;
            int got;

            if (f->i >= (int64_t)f->n) { dump_pop(ctx, &st, &b); continue; }
            got = JS_GetOwnSlot(ctx, &val, f->holder, f->tab[(size_t)f->i].atom);
            DCHECK(got >= 0,
                   "the own-slot read of an enumerated key failed — the key came from this same object's own "
                   "enumeration one moment ago and JS_GetOwnSlot runs no page code, so the enumeration and "
                   "the read disagree about a slot nothing could have changed in between");
            /* AN ACCESSOR ANSWERS 0 HERE, WHICH IS JS_GetOwnSlot'S CONTRACT ("an accessor is refused on this
               side") AND NOT AN ABSENCE, so the two must not be summed. A page-built artifact holding a
               getter is a value whose members are the page's CODE, and reading one is exactly the [[Get]]
               this file exists not to perform. */
            DCHECK(got == 1,
                   "an enumerated own key has no data slot — JS_GetOwnSlot answers 0 for an ACCESSOR as well "
                   "as for an absent slot, and the key was enumerated a moment ago, so this member is a "
                   "getter. Reading it is the page's own code, which this dump may not run: the artifact's "
                   "producer must hand over data, or the dump must become a scheduler flow");
            key = JS_AtomToCString(ctx, f->tab[(size_t)f->i].atom);
            CHECK(key != NULL, "value_dump: OOM reading a member name");
            f->i++;
            /* §25.5.4.5 SerializeJSONObject step 8.b: a member whose serialization is `undefined` is NOT
               written at all — the key goes with it. That is how an absent member stays absent instead of
               becoming a `null` a consumer would read as an answer. */
            if (JS_IsUndefined(val) && !concolic_is(val)) {
                JS_FreeCString(ctx, key);
                JS_FreeValue(ctx, val);
                continue;
            }
            /* THE COMMA IS DECIDED BY WHAT HAS BEEN WRITTEN AND NOT BY THE CURSOR, because the skip above
               advances the cursor without writing anything — a comma keyed on `i` would emit a leading one
               after a dropped first member. `n` holding the count of members ALREADY written is that fact. */
            if (b.n && b.b[b.n - 1] != '{') json_buf_raw(&b, ",");
            json_buf_str(&b, key);
            json_buf_raw(&b, ":");
            JS_FreeCString(ctx, key);
            if (dump_value(ctx, &st, &b, val)) { JS_FreeValue(ctx, val); continue; }
            JS_FreeValue(ctx, val);
            continue;
        }
    }

    free(st.f);
    return json_buf_take(&b);
}
