/* THE C-DRIVES-JS PATH IN ToPrimitive IS GONE — the body that ran @@toPrimitive / valueOf / toString through
   JS_CallFree has been DELETED, not left behind an assert. It was the last of the three functions whose mutual
   reachability manufactures the interpreter cycle (engine/recgate.mjs --why=JS_CallInternal), and a superseded
   system kept as a fallback is what lets an unrouted site stay unrouted.
   THE COVERAGE CLAIM THAT STOOD HERE — that the assert was "silent over the whole corpus" — WAS A CLAIM ABOUT
   A CORPUS OF FIXTURES, AND REAL PAGES FALSIFIED IT: driven over thirty signed-out product surfaces in Chrome,
   two of them (excalidraw, mastodon) abort on exactly this line. Silence under test262 and WPT proves only that
   those corpora do not reach the unrouted edges; the routing is unfinished and the number of remaining sites is
   not something this comment knows. So the assert now NAMES the site it fires at, which is the difference
   between an instruction and an actionable one — see the message below.
   The trampolined form is JSToPrim (operand and machine mode); the operator dispatch, the key coercions and
   every PRIMARGS builtin route through it. Two rounds of that conversion are in the git history and both are
   worth knowing when a new site turns up: round one was BigInt.prototype.toString coercing its radix from C,
   fixed by declaring it PRIMARGS; round two was Reflect.set onto a typed array, fixed in
   ta_atom_write_needs_toprim's receiver test rather than by adding a coercion path.

   WHAT REMAINS IS NOT A COERCION AT ALL. Both arms are the cases 7.1.1 answers without running anything:
   a value that is ALREADY primitive is its own result, and a concolic stands for unknown EXTERNAL INPUT —
   primitive in the page, wearing an object only because the solver needs a carrier with an exotic [[Get]], so
   reading @@toPrimitive off it would get a DERIVED concolic back and call it, throwing "toPrimitive is not a
   function" inside an expression the page never wrote.
   A real object reaching here is a call site that runs user code from C with no flow base under it, which is a
   capability that does not exist rather than a case to handle: route it to the trampoline.
   NEITHER A BYTE CONSUMER NOR A CONVERTER CAN REACH THIS LINE ANY MORE, and that is why the message below
   offers neither "a host component that only wants BYTES asks for them at its own edge" nor "this is a
   JS_ToString called straight from C" as a remedy. Both of those arrivals die one frame EARLIER, at a
   boundary that can name the EDGE while this one can only name the CLASS: js_force_tostring for every
   JS_ToCString spelling, and JS_ToStringInternal's object arm for every JS_ToString / JS_ToStringFree /
   JS_ToPropertyKey / JS_ValueToAtom spelling — each of which is a macro that captured its caller's
   __FILE__/__LINE__ (quickjs.h). The reader has no stack (a real page reports the message and nothing else),
   so a boundary that can only name the VALUE sends them to search every edge that could have taken it.
   HINT STRING therefore cannot arrive here at all: §7.1.19 ToString ( arg ) step 10 is its ONE caller and it
   now asserts before the hop. What still arrives HERE is the NUMERIC half — an operator's or a builtin's
   ToNumber/ToNumeric-side coercion (§7.1.4 ToNumber ( arg ), 13.15.3's `+`, a relational or a loose equality)
   called straight from C without being routed to the trampoline. §7.1.15 ToBigInt ( arg ) is NOT among them
   any more: it asserts one frame earlier, in JS_ToBigIntFreeAt's object arm, which names the asking file:line
   exactly as the two string-side boundaries above do. (It was listed here as "7.1.13 ToBigInt", and §7.1.13 is
   ToUint8 ( arg ) — a number that sent a reader to the wrong operation.) */
static JSValue JS_ToPrimitiveFree(JSContext *ctx, JSValue val, int hint)
{
    if (JS_VALUE_GET_TAG(val) != JS_TAG_OBJECT)
        return val;
    /* An unknown is its own primitive under every hint. That is exact for a primitive, and the two hints give
       one string for structured or JSON data under the realm's own prototype methods (§7.1.1.1
       OrdinaryToPrimitive's two method orders agree there; §21.4.4.45 makes a Date's default the string hint).
       Named residual. Not covered: an unknown that is an Object with page code behind §7.1.1 (its own or an
       inherited @@toPrimitive, valueOf or toString), which a driven orphan's argument or receiver can be, since
       its never-run caller could pass one; there the hint picks which method answers.
       Next diff builds: such an unknown's ToPrimitive answers the derived unknown that §7.1.1 step 1.b.iv's Call
       (or OrdinaryToPrimitive's) returns, its identity naming the hint, so `x + ""` concatenates that and
       solver/concolic.c's str_of_record names its property key from it rather than from `x`.
       Absence shows as: no derived value's shape or identity anywhere names a ToPrimitive hint. */
    if (js_value_is_concolic(val))
        return val;
#if APICLIENT_DEV
    {
        /* WHICH SITE. The instruction below names a mechanism, and for the whole of its life it could not name
           the CALL SITE it was instructing anyone to route — a `@WHY` whose remedy is "route this site" and
           whose text is identical wherever it fires is a crash nobody can act on, which is the twin of the
           stale-DFAIL failure: accurate about the spec, silent about this tree. Two of a thirty-site corpus
           aborted here and the report could say no more than the file:line of THIS function.
           The innermost frame is the page's call INTO the unrouted builtin or host edge, which is the file:line
           to fix; the CLASS is what tells a Date reaching an unconverted operator from a plain object handed to
           a DOM member. */
        char why[4096];
        char frames[2048];
        char cbuf[ATOM_GET_STR_BUF_SIZE];
        const char *cname;
        int th = hint & ~HINT_FORCE_ORDINARY;   /* the caller's hint may carry 7.1.1.1's entry bit */

        cname = JS_AtomGetStr(ctx, cbuf, sizeof(cbuf),
                              ctx->rt->class_array[JS_VALUE_GET_OBJ(val)->class_id].class_name);
        js_why_backtrace(ctx, frames, sizeof frames);
        snprintf(why, sizeof why,
                 "§7.1.1 ToPrimitive ( input [ , preferredType ] ) reached with a REAL object (class `%.40s`, "
                 "hint %s) from a C activation with no flow base under it. Its step 1.a is GetMethod(input, "
                 "%%Symbol.toPrimitive%%) and §7.1.1.1 OrdinaryToPrimitive ( obj, hint ) step 3 CALLS the "
                 "page's valueOf/toString, so this is not a conversion the boundary can perform — the "
                 "C-drives-JS body was DELETED, not left behind an assert. ROUTE THE CALL SITE to the "
                 "trampoline: a builtin declares PRIMARGS(mask, hint, nargs) at its JSTrampStepDef so "
                 "js_primargs_step coerces the argument as a TOPRIMITIVE step; an operator asks "
                 "js_toprim_operand and goes to do_toprim_tramp with a tp_resume_at of its own; a step machine "
                 "already on the chain sets tp_outer/tp_value and delivers the primitive as its next step's "
                 "result. NEITHER A BYTE CONSUMER NOR A CONVERTER can be the culprit — every JS_ToCString "
                 "spelling asserts one frame earlier at js_force_tostring and every JS_ToString / "
                 "JS_ToPropertyKey / JS_ValueToAtom spelling asserts one frame earlier in "
                 "JS_ToStringInternal's object arm, both of which name the asking file:line — and hint string "
                 "has no other caller, so this is the NUMERIC side: a ToNumber / ToNumeric / `+` / relational "
                 "/ loose-equality coercion performed from C. "
                 "The innermost frame here is the page's call into the site that has not been routed: %s",
                 cname ? cname : "(unnamed class)",
                 th == HINT_STRING ? "string" : th == HINT_NUMBER ? "number" : "default",
                 frames);
        DFAIL(why);
    }
#endif
    /* The release path owns `val`: DFAIL is compiled out there, so without this the operand's reference is the
       one thing the TypeError does not take with it. */
    JS_FreeValue(ctx, val);
    return JS_ThrowTypeError(ctx, "toPrimitive");
}

static JSValue JS_ToPrimitive(JSContext *ctx, JSValueConst val, int hint)
{
    return JS_ToPrimitiveFree(ctx, js_dup(val), hint);
}

