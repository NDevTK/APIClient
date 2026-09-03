/* THE UNION-ARM AUDIT — the sixth axis, and the one every other instrument in this tree excludes BY NAME.
 *
 *   node engine/uniongate.mjs              audit every union-typed argument position the engine installs
 *   node engine/uniongate.mjs --all        print every partition class, not only the ones a finding names
 *   node engine/uniongate.mjs --reach      also print what could not be joined, and why
 *
 * WHAT IT CHECKS. idlgen.mjs asks which members EXIST. WPT's idlharness reads `.length`, which is ARITY.
 * argtypegate.mjs asks whether a position's declared TYPE matches the IDL, and dicttypegate.mjs asks the same
 * of a dictionary member. rettypegate.mjs asks about return types. Every one of those composes an enumerator
 * NAME from the spec type through idl_typename.mjs's convention — and that convention says, in its own code,
 * `if (t.union || t.generic) return null;`. So a position whose IDL type is a UNION is UNPLACEABLE to all of
 * them: counted, printed under a reach band, and judged by nothing. This asks the one question that survives
 * that exclusion.
 *
 * THE QUESTION IS A PIGEONHOLE AND THAT IS WHY IT NEEDS NO ENGINE-SIDE DECLARATION OF ARMS. Web IDL §3.2.25
 * Union types is a fixed twenty-step ladder, so for any union it defines a TOTAL FUNCTION from the value
 * shapes its steps can tell apart to the arm each one takes. This engine spells a union as ONE `IdlArgType`
 * enumerator whose arm test is hand-written C at a single site — one test, therefore exactly ONE such
 * function. So: compute §3.2.25's function for every union the platform declares at a position, group the
 * positions by the enumerator declared there, and any enumerator standing over two DIFFERENT functions is a
 * single test being asked to answer two questions. That is decidable from the IDL and the declaration alone;
 * nothing here reads, parses or restates the arm test itself, which is exactly what a static auditor cannot
 * do and must not pretend to.
 *
 * WHAT IT CANNOT SEE, because a checker trusted past its evidence is worse than none:
 *
 *   - IT NEVER CHECKS WHETHER AN ARM TEST IS RIGHT. An enumerator declared at ONE union, or at several unions
 *     that share a partition, is CONSISTENT here whatever its C says — and the defect that motivated this file
 *     was exactly that: `IDL_BOOL_OR_DICT`'s test read `!JS_IsObject(a)`, which answers §3.2.25 steps 11/12/18
 *     and not step 4 ("If V is null or undefined, then: If types includes a dictionary type, then return the
 *     result of converting V to that dictionary type."), so an omitted argument took the boolean arm and
 *     `el.scrollIntoView()` scrolled to the wrong end. This audit would have said CONSISTENT. Deciding an arm
 *     TEST needs the real conversion run over the real values — a differential, not a scan — and the
 *     per-enumerator partition table this prints is what such a fixture's rows are written from.
 *   - IT ASKS ARGUMENT POSITIONS AND NOT DICTIONARY MEMBERS. The argument join is exact (install → step id →
 *     declaration); the dictionary-member one is not — dicttypegate.mjs reports its own as "candidates predict
 *     {…} over N dictionaries" — and grouping by enumerator over an ambiguous join would put a position under
 *     a partition that may not be its own. Those rows stay in dicttypegate's silent band and say so there.
 *   - ITS DOMAIN IS COMPLETE OVER WHAT THE LADDER OBSERVES, AND OVER NOTHING ELSE. §3.2.25 reads V through
 *     its JavaScript type, platform-object-ness, four internal slots, IsCallable and GetMethod at two property
 *     keys; the domain is every base an ECMAScript constructor produces crossed with every assignment of those
 *     two keys, so a shape a page can build and this table cannot name would have to be one the standard's own
 *     steps cannot name either. The one corner a human still states is the platform object's step-5-to-10
 *     facts, which no ECMAScript value can witness — the residual is at that declaration, with what would show
 *     it.
 *   - IT READS THE CORPUS `idl_installed.mjs` IS POINTED AT, and a member installed outside it is not seen.
 *   - AN OVERLOADED MEMBER IS NOT JUDGED. §3.6 gives one member several signatures and this engine declares
 *     one entry; which signature a declaration is meant to be is a question about the member's algorithm.
 *   - A UNION WHOSE ARMS DIFFER ONLY IN DESTINATION IS NOT A FINDING. `(AddEventListenerOptions or boolean)`
 *     and `(boolean or ScrollIntoViewOptions)` have the SAME §3.2.25 partition and the engine spells them as
 *     two rows on purpose, because DOM §2.7 Interface EventTarget's flatten options folds the boolean into a
 *     member where CSSOM VIEW §6 Extensions to the Element Interface reads it directly. Two enumerators over
 *     one partition are printed as an observation and never judged; one enumerator over two partitions is the
 *     finding, and the two are not symmetric.
 *
 * IT IS NOT A BUILD GATE, for argtypegate.mjs's reason: it reads source text joined to a corpus that moves
 * under it. It prints; the human decides. It exits 0.
 */
import { fileURLToPath } from "node:url";
import { dirname, join, relative } from "node:path";
import { loadEnvironment, installedMembers } from "./idl_installed.mjs";
import { loadIdl } from "./idl_members.mjs";
import { contract, declarationIndex } from "./idl_argdecl.mjs";
import { typeConvention } from "./idl_typename.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);
const HOST = join(HERE, "host");
const CORE = join(HOST, "browser/core");

const ALL = process.argv.includes("--all");
const REACH = process.argv.includes("--reach");

const C = contract(join(CORE, "idl_args.h"));
const idl = await loadIdl();
const { enums, callbacks, callbackIfaces, resolveTypedef } = typeConvention(idl);

/* ---- Web IDL §2.13 Types' own category lists ---------------------------------------------------------------
 *
 * Quoted from §2.13 rather than recalled, because §3.2.25's clauses are written in these words and a wrong
 * membership silently moves an arm: "The following types are known as integer types: byte, octet, short,
 * unsigned short, long, unsigned long, long long and unsigned long long. The following types are known as
 * numeric types: the integer types, float, unrestricted float, double and unrestricted double." and "The
 * string types are DOMString, all enumeration types, ByteString and USVString." and "The typed array types are
 * Int8Array, Int16Array, Int32Array, Uint8Array, Uint16Array, Uint32Array, Uint8ClampedArray, BigInt64Array,
 * BigUint64Array, Float16Array, Float32Array, and Float64Array."
 *
 * AN ENUMERATION IS A STRING TYPE AND THAT CLAUSE IS LOAD-BEARING: without it §3.2.25 step 15 does not fire
 * for `(CompositeOperationOrAuto or sequence<CompositeOperationOrAuto>)` and every string a page passes reads
 * as step 20's TypeError. It is read off the corpus's own `enum` declarations rather than listed. */
const NUMERIC = new Set(["byte", "octet", "short", "unsigned short", "long", "unsigned long", "long long",
  "unsigned long long", "float", "unrestricted float", "double", "unrestricted double"]);
const TYPED_ARRAY = new Set(["Int8Array", "Int16Array", "Int32Array", "Uint8Array", "Uint16Array",
  "Uint32Array", "Uint8ClampedArray", "BigInt64Array", "BigUint64Array", "Float16Array", "Float32Array",
  "Float64Array"]);
const STRING = new Set(["DOMString", "ByteString", "USVString"]);

/* The CLAUSE of §3.2.25 a flattened member type answers to. Arms are named by clause and never by the type's
   own identifier, because two unions whose only difference is WHICH interface they brand are one question
   (`(Node or DOMString)` and `(Request or USVString)` are both "an interface, else a string") and the engine
   answers both with one row whose interface is declared beside the position. */
function armOf(t) {
  if (t.generic === "sequence") return "sequence";
  if (t.generic === "async_sequence") return "async sequence";
  if (t.generic === "FrozenArray") return "frozen array";
  if (t.generic === "record") return "record";
  if (t.generic) return `generic<${t.generic}>`;
  const n = t.idlType;
  if (typeof n !== "string") return "?";
  if (n === "undefined") return "undefined";
  if (n === "object") return "object";
  if (n === "boolean") return "boolean";
  if (n === "bigint") return "bigint";
  if (n === "any") return "any";
  if (n === "ArrayBuffer") return "ArrayBuffer";
  if (n === "SharedArrayBuffer") return "SharedArrayBuffer";
  if (n === "DataView") return "DataView";
  if (TYPED_ARRAY.has(n)) return "typed array";
  if (NUMERIC.has(n)) return "numeric";
  if (STRING.has(n) || enums.has(n)) return "string";
  if (callbacks.has(n)) return "callback function";
  if (callbackIfaces.has(n)) return "callback interface";
  if (idl.dictByName.has(n)) return "dictionary";
  if (idl.byName.has(n)) return "interface";
  return `unresolved<${n}>`;
}

/* Web IDL §2.13.32 Union types — "The flattened member types of a union type, possibly annotated, is a set of
   types determined as follows", whose loop strips an annotated type's and a nullable type's inner type and
   splices a nested union's own flattened members. */
function flattenedMemberTypes(t, out = []) {
  for (const u0 of t.idlType) {
    const u = resolveTypedef(u0);
    if (u.union) flattenedMemberTypes(u, out);
    else out.push(u);
  }
  return out;
}
/* §2.13.32 "The number of nullable member types of a union type", which its own following paragraph uses to
   decide whether the union includes a nullable type. */
function nullableMemberCount(t) {
  let n = 0;
  for (const u0 of t.idlType) {
    const u = resolveTypedef(u0);
    if (u.nullable) n++;
    if (u.union) n += nullableMemberCount(u);
  }
  return n;
}
/* §2.13.32 "A type includes undefined if: the type is undefined, or the type is a nullable type and its inner
   type includes undefined, or the type is an annotated type and its inner type includes undefined, or the type
   is a union type and one of its member types includes undefined." */
function includesUndefined(t) {
  if (t.union) return t.idlType.some((u) => includesUndefined(resolveTypedef(u)));
  return t.idlType === "undefined";
}
/* §2.13.32 "A type includes a nullable type if: the type is a nullable type, or the type is an annotated type
   and its inner type is a nullable type, or the type is a union type and its number of nullable member types
   is 1."
   THE UNION'S OWN `?` IS THE FIRST CLAUSE AND IT IS THE HALF A READER DROPS. `(HTMLScriptElement or
   SVGScriptElement)?` has ZERO nullable MEMBER types, so a check written as `nullableMemberCount(t) === 1`
   alone answers false and sends null to step 20's TypeError where the spec sends it to step 2's IDL null. */
const includesNullable = (t) => !!t.nullable || nullableMemberCount(t) === 1;

/* ---- the value shapes §3.2.25's twenty steps can tell apart -----------------------------------------------
 *
 * The domain is the set of shapes the ladder can DISTINGUISH, and every step tests exactly one of five things
 * about V: its JavaScript type, whether it is a platform object, one internal slot, IsCallable, or the answer
 * of ECMAScript's GetMethod at one property key. Two shapes are separated here exactly when some step
 * separates them, so a partition over this domain IS Web IDL §3.2.25 Union types' function and not a sample of
 * it. That is a claim about COMPLETENESS as much as about correctness, and the completeness half is what the
 * cross product below is for.
 *
 * A SHAPE'S NAME IS A LABEL AND NEVER A FACT — WHICH IS THE ONLY REASON THIS DOMAIN CAN BE TRUSTED. The list
 * below used to be bare strings, and every step read them by comparing the string: `V === "Object @@iterator"`
 * WAS the question "does V have %Symbol.iterator%". That spells a claim about JavaScript into a name, and the
 * claim it makes about every OTHER object shape is the silent one — each name that is not the iterator one
 * asserts, by not being it, that its shape has no iteration method. Two of those assertions were FALSE. A
 * String object inherits `%Symbol.iterator%` from String.prototype and a typed array inherits it from
 * %TypedArray%.prototype, so both reach step 11.2's `? GetMethod(V, %Symbol.iterator%)` and get a method back;
 * the name-comparison sent both past 11.2 and 11.3 to step 15's string arm. Web IDL §3.2.25 carries exactly ONE
 * String-object carve-out and it is step 11.1.1's — "If types does not include a string type or V does not
 * have a [[StringData]] internal slot, then" — under the ASYNC sequence clause alone. Steps 11.2 and 11.3 have
 * no such condition, so a name-derived model of iteration disagrees with the ladder at the two shapes whose
 * iteration comes from a prototype rather than from their own construction.
 *
 * SO EVERY FACT IS DERIVED BY RUNNING THE OPERATION THE STEP NAMES, ON A REAL VALUE, IN THIS REALM. Node is a
 * conforming ECMAScript implementation and Web IDL §3.2.25's steps are ECMAScript operations, so the
 * implementation is
 * the artifact that OWNS these answers — the same reason idlgen reads the real `.idl` through webidl2 rather
 * than a table of member names somebody typed. `String object -> sequence` is then not a special case anybody
 * wrote; it is what `new String("")[%Symbol.iterator%]` answers. The one shape that CANNOT be derived is the
 * platform object, which has no witness outside a browser, and it is declared as one below with its reason.
 *
 * AND THE SHAPES A PAGE CAN BUILD ARE NOT THE SHAPES ECMASCRIPT'S CONSTRUCTORS PRODUCE, WHICH IS THE HALF
 * DERIVING THE FACTS DOES NOT CLOSE. Running the real operation on a real value makes every ROW right; it says
 * nothing about which rows the table should HAVE, and a domain built by listing one value per constructor
 * silently asserts that a page hands Web IDL only the shapes a constructor emitted. In this project's threat
 * model that is precisely the wrong assumption: the values reaching a Web IDL entry point are the page's, and
 * the page writes properties. So the domain is a CROSS PRODUCT, and its two axes are the standard's own
 * distinction rather than a taxonomy invented here. Steps 5-10 ask what V IS — "V is a platform object",
 * "V has an [[ArrayBufferData]] internal slot", IsCallable(V) — and no ECMAScript operation adds an internal
 * slot to a value or takes one away, so those facts are fixed by the constructor that made it. Steps 11.1.1.1,
 * 11.1.1.3, 11.2.1 and 11.3.1 ask `? GetMethod(V, P)`, which is an ordinary [[Get]] and therefore answers
 * whatever was last written at P on V or on its prototype chain. GetMethod's keys are the axes a page moves a
 * value along, and they are the ONLY ones — so the domain is { every base a constructor produces } x { every
 * assignment of those keys }, which is derived from the key list and grows with it rather than being widened
 * by hand.
 *
 * THE CROSSINGS ARE REACHABLE, WHICH IS NOT AUTOMATIC AND IS THE REASON THIS IS A DOMAIN AND NOT A PADDING.
 * §3.2.25 is ORDERED, so a decoration read at step 11 is invisible for a value whose brand step already
 * returned — but steps 5-10 each hold exactly two sub-steps and FALL THROUGH when the union names neither of
 * them, and a union that names no `object` arm and no arm for V's own brand is the ordinary case rather than
 * the exotic one. `(DOMString or sequence<DOMString>)` is such a union: an ArrayBuffer carrying
 * `%Symbol.iterator%` falls past step 6 and takes step 11.2's sequence arm where an undecorated one takes
 * step 15's string arm, and a typed array whose `%Symbol.iterator%` has been masked does the reverse. Both
 * values are two lines of page script. A crossing the ladder CANNOT observe costs nothing either, because it
 * produces a fact vector some other shape already has and the dedup below removes it — so the cross product
 * cannot manufacture a partition, only reveal one.
 *
 * The domain's own duplicate check is what enforces that, and it is not a formality: a widening that adds a
 * shape the ladder cannot separate from an existing one is a domain naming one value shape twice, and it
 * aborts here rather than padding every partition key with a duplicated column. */

/* The brand checks Web IDL §3.2.25 Union types' steps 6-9 name, reached through the ECMAScript accessors that
   OWN them rather than
   restated: each getter below begins by requiring the very internal slot its step asks about, so a throw from
   it and an absent slot are one fact and not two, and the ArrayBuffer/SharedArrayBuffer pair splits
   on IsSharedArrayBuffer exactly as steps 6 and 7 do because that is what those two getters disagree about.
   %TypedArray%.prototype[%Symbol.toStringTag%] RETURNS undefined rather than throwing for a value with no
   [[TypedArrayName]], which is why the probe reads the result and does not merely survive the call. */
const SLOT = {
  arrayBuffer: Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength").get,
  sharedArrayBuffer: Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, "byteLength").get,
  dataView: Object.getOwnPropertyDescriptor(DataView.prototype, "byteLength").get,
  typedArray: Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Int8Array.prototype), Symbol.toStringTag).get,
  stringData: String.prototype.valueOf,
};
const hasSlot = (get, v) => { try { return get.call(v) !== undefined; } catch { return false; } };

/* THE PROPERTY KEYS THE LADDER READS THROUGH GetMethod, each named beside the sub-step that reads it. This is
   the whole of what §3.2.25 observes about V that is not its JavaScript type, an internal slot, IsCallable or
   platform-object-ness — so it is also the whole of what a page can change about a value's arm without
   changing what the value is. The cross product below is generated from this list, which is why adding a key
   here widens the domain instead of requiring the domain to be widened. */
const GET_METHOD_KEYS = [
  ["asyncIterator", Symbol.asyncIterator], /* step 11.1.1.1 */
  ["iterator", Symbol.iterator],           /* steps 11.1.1.3, 11.2.1, 11.3.1 */
];

/* ECMAScript's GetMethod(V, P) — the operation Web IDL §3.2.25 steps 11.1.1.1, 11.1.1.3, 11.2.1 and 11.3.1 all
   name, and not a
   plain [[Get]]. undefined and null are both "there is no method"; anything else that is not callable is a
   TypeError. The domain carries no shape for that third outcome — no union arm is ever chosen by a throw — so a
   witness that reached it would be a shape this partition cannot express, and it aborts here rather than
   quietly answering as if the method were absent. */
function getMethod(v, sym, who) {
  const m = v[sym];
  if (m === undefined || m === null) return false;
  if (typeof m !== "function")
    throw new Error(`the witness for the \`${who}\` shape carries a non-callable ${String(sym)}, so Web IDL `
      + "§3.2.25's "
      + "GetMethod would throw a TypeError — an outcome this partition's arms cannot name");
  return true;
}

/* The facts of one witness, each answered by the operation its step names. `platform` is the ONE fact no
   ECMAScript value can state, so it is passed in rather than probed. */
function factsOf(who, v, platform) {
  const object = (typeof v === "object" && v !== null) || typeof v === "function";
  const f = {
    kind: v === null ? "null" : typeof v,      /* steps 1, 2, 4, 12, 13 and 14 ask only this */
    object,                                    /* step 11's own condition */
    platform,                                  /* step 5 */
    callable: typeof v === "function",         /* step 10's IsCallable(V) */
    arrayBuffer: object && hasSlot(SLOT.arrayBuffer, v),              /* step 6 */
    sharedArrayBuffer: object && hasSlot(SLOT.sharedArrayBuffer, v),  /* step 7 */
    dataView: object && hasSlot(SLOT.dataView, v),                    /* step 8 */
    typedArray: object && hasSlot(SLOT.typedArray, v),                /* step 9 */
    stringData: object && hasSlot(SLOT.stringData, v),                /* step 11.1.1 */
  };
  for (const [fact, key] of GET_METHOD_KEYS) f[fact] = object && getMethod(v, key, who);
  return f;
}

/* THE PLATFORM OBJECT IS DECLARED AND NOT DERIVED, because no ECMAScript value is one: "platform object" is
   Web IDL's own notion and a witness for it exists only inside a browser. Its facts are therefore the one
   place in this domain where a human states an answer, and they are stated here so that the narrowness is
   READABLE rather than implied by a name — which is exactly the defect the rest of this section removes. Only
   its step-5-to-10 facts are stated: the GetMethod facts are varied over it by the cross product like every
   other object base, because a page assigns `%Symbol.iterator%` to a platform object with the same one line
   it assigns it to anything else, and because an interface declaring `iterable<>`, `maplike<>`, `setlike<>` or
   `async_iterable` carries one already. The ladder reads GetMethod and nothing else there, so it cannot tell
   those two apart and this domain does not model them apart.
   RESIDUAL — WHAT IS NOT COVERED: the platform object is ONE declared fact vector, so an implementation whose
   platform objects answer a step-5-to-10 probe differently from this declaration — IsCallable true, or a brand
   slot present — has a shape this domain does not name. WHAT THE NEXT DIFF BUILDS: nothing, until such a shape
   is shown to exist; Web IDL's own steps 6 to 9 are the exhaustive brand list and a platform object holding
   one of those slots would be a value the engine constructs, so the evidence would come from `idl_args.c`'s
   own conversions rather than from this file. HOW ITS ABSENCE WOULD SHOW: a union naming both an interface arm
   and a buffer arm would be printed here as sending every platform object to the interface arm, and a
   differential written from that row would expect the interface conversion for a platform object the engine
   converts to a buffer. */
const PLATFORM_OBJECT_FACTS = { kind: "object", object: true, platform: true, callable: false,
  arrayBuffer: false, sharedArrayBuffer: false, dataView: false, typedArray: false, stringData: false,
  iterator: false, asyncIterator: false };

/* The bases, in the order the table prints — one THUNK per base, because each assignment of the GetMethod keys
   needs its own value to write them on and a shared one would carry the previous assignment's properties. */
const BASES = [
  ["undefined", () => undefined], ["null", () => null],
  ["platform object", () => PLATFORM_OBJECT_FACTS],
  ["ArrayBuffer", () => new ArrayBuffer(0)],
  ["SharedArrayBuffer", () => new SharedArrayBuffer(0)],
  ["DataView", () => new DataView(new ArrayBuffer(0))],
  ["typed array", () => new Uint8Array(0)],
  ["callable", () => function () {}],
  ["Object", () => ({})],
  ["String object", () => new String("")],
  ["boolean", () => true], ["number", () => 0], ["bigint", () => 0n], ["string", () => ""],
  ["symbol", () => Symbol("uniongate")],
];

/* Every assignment of the GetMethod keys, generated from the key list rather than enumerated, so the count is
   2^|GET_METHOD_KEYS| by construction and a key added above cannot be forgotten here. */
let ASSIGNMENTS = [[]];
for (const [, key] of GET_METHOD_KEYS)
  ASSIGNMENTS = ASSIGNMENTS.flatMap((s) => [s, [...s, key]]);

/* Writing an assignment onto a base. Both directions are page-reachable and both are used: an own data
   property holding a function makes GetMethod answer where the base had nothing, and an own data property
   holding `undefined` makes it answer nothing where the base inherited a method from its prototype — which is
   the only way this domain reaches a typed array or a String object that does NOT take step 11.2's sequence
   arm. Nothing else about the value is touched, and the facts recorded are re-probed rather than intended. */
function decorate(v, present) {
  for (const [, key] of GET_METHOD_KEYS)
    Object.defineProperty(v, key, { value: present.includes(key) ? function () {} : undefined,
      writable: true, configurable: true });
  return v;
}

/* The declared platform-object row must state EVERY fact the probes derive, or a fact the ladder reads would
   answer `undefined` there and the table would be silent about a step rather than wrong about it. */
{
  const derived = Object.keys(factsOf("Object", {}, false)).sort().join(",");
  const declared = Object.keys(PLATFORM_OBJECT_FACTS).sort().join(",");
  if (derived !== declared)
    throw new Error(`the declared platform-object facts state {${declared}} while Web IDL §3.2.25's steps are `
      + `probed for {${derived}}, so that row answers a step this domain reads`);
}

const SHAPES = [];
const FACTS = {};
/* A shape's printed name is rendered from the facts the probes RETURNED, never from the assignment that built
   the witness, so a name cannot state an iteration the ladder would not find — and a base whose prototype
   already supplies a method is named for what GetMethod answers rather than for how it came to answer. */
const nameOf = (base, f) =>
  base + (f.iterator ? " +@@iterator" : "") + (f.asyncIterator ? " +@@asyncIterator" : "");

for (const [base, make] of BASES) {
  const bare = make();
  const bareFacts = (bare === PLATFORM_OBJECT_FACTS) ? bare : factsOf(base, bare, false);
  /* Step 11's own condition gates every GetMethod §3.2.25 makes, so a value that is not an Object is never
     asked at these keys: it has exactly one shape and the cross product does not run over it. */
  if (!bareFacts.object) { SHAPES.push(base); FACTS[base] = bareFacts; continue; }
  for (const present of ASSIGNMENTS) {
    const f = (bare === PLATFORM_OBJECT_FACTS)
      ? { ...PLATFORM_OBJECT_FACTS, asyncIterator: present.includes(Symbol.asyncIterator),
          iterator: present.includes(Symbol.iterator) }
      : factsOf(base, decorate(make(), present), false);
    /* The witness is re-probed and the probe must agree with what was written on it. This is what the two
       hand-written exclusivity checks it replaces were reaching for, and it is stronger: it holds every base
       against every assignment rather than one pair of witnesses against each other. The two ways a base can
       fail to carry an assignment abort at their own origins and neither can produce a silently wrong row —
       a base that cannot take the write at all (non-extensible, or the key non-configurable) throws from
       `Object.defineProperty` at the write, and a base that ANSWERS past what was written (a Proxy whose get
       trap ignores the property table) is this check, which names the base and the key. */
    for (const [fact, key] of GET_METHOD_KEYS)
      if (f[fact] !== present.includes(key))
        throw new Error(`the \`${base}\` witness was written ${String(key)} = `
          + `${present.includes(key) ? "a function" : "undefined"} and Web IDL §3.2.25's GetMethod then `
          + `answered ${f[fact]}, so this base cannot carry the assignments the domain is built from`);
    const name = nameOf(base, f);
    SHAPES.push(name);
    FACTS[name] = f;
  }
}

{
  /* Two shapes with the same facts are one shape: the ladder cannot tell them apart, so printing both states a
     distinction that does not exist and pads the partition key with a duplicated column. This is also what
     keeps the cross product honest — a crossing no step of §3.2.25 can observe is removed here rather than
     manufacturing a partition, and a base whose brand facts duplicate another's is caught the same way. */
  const seen = new Map();
  for (const name of SHAPES) {
    const f = FACTS[name];
    const k = JSON.stringify(Object.keys(f).sort().map((key) => [key, f[key]]));
    if (seen.has(k))
      throw new Error(`the \`${name}\` and \`${seen.get(k)}\` shapes have identical facts, so no step of `
        + "Web IDL §3.2.25 separates them and this domain names one value shape twice");
    seen.set(k, name);
  }
}
const SHAPE_PAD = Math.max(...SHAPES.map((s) => s.length));


/* §3.2.25 Union types, read in the standard's own step order. Its twenty top-level steps are parameterised by
   THREE facts the IDL states and not by one: whether the union type includes undefined (step 1), whether it
   includes a nullable type (step 2), and its flattened member types (step 3 onward). The first two are §2.13.32
   predicates over the union AS DECLARED — flattening strips the very `?` step 2 asks about — so a partition
   computed from the flattened set alone gets both of the ladder's first two steps wrong. */
function partition(t) {
  const arms = new Set(flattenedMemberTypes(t).map(armOf));
  const has = (a) => arms.has(a);
  const undef = includesUndefined(t), nullable = includesNullable(t);

  /* Steps 15-20 — the tail every shape falls to when no clause above it matched. */
  const tail = () => has("string") ? "string"                                          /* step 15 */
    : has("numeric") && has("bigint") ? "numeric-or-bigint"                            /* step 16 */
    : has("numeric") ? "numeric"                                                       /* step 17 */
    : has("boolean") ? "boolean"                                                       /* step 18 */
    : has("bigint") ? "bigint"                                                         /* step 19 */
    : "TypeError";                                                                     /* step 20 */

  /* Each clause reads the FACT its step tests, never the shape's name — see the domain's own banner for why a
     name comparison answered two of them wrongly. Every sub-step pair falls THROUGH when the union names
     neither arm, exactly as the standard's nested lists do. */
  const of = (V) => {
    const f = FACTS[V];
    if (undef && f.kind === "undefined") return "undefined";                           /* step 1 */
    if (nullable && (f.kind === "null" || f.kind === "undefined")) return "null";       /* step 2 */
    if (f.kind === "null" || f.kind === "undefined")                                   /* step 4 */
      return has("dictionary") ? "dictionary" : tail();
    if (f.platform) {                                                                  /* step 5 */
      if (has("interface")) return "interface";
      if (has("object")) return "object";
    }
    if (f.arrayBuffer) {                                                               /* step 6 */
      if (has("ArrayBuffer")) return "ArrayBuffer";
      if (has("object")) return "object";
    }
    if (f.sharedArrayBuffer) {                                                         /* step 7 */
      if (has("SharedArrayBuffer")) return "SharedArrayBuffer";
      if (has("object")) return "object";
    }
    if (f.dataView) {                                                                  /* step 8 */
      if (has("DataView")) return "DataView";
      if (has("object")) return "object";
    }
    if (f.typedArray) {                                                                /* step 9 */
      if (has("typed array")) return "typed array";
      if (has("object")) return "object";
    }
    if (f.callable) {                                                                  /* step 10 */
      if (has("callback function")) return "callback function";
      if (has("object")) return "object";
    }
    if (f.object) {                                                                    /* step 11 */
      /* 11.1.1 — "If types does not include a string type or V does not have a [[StringData]] internal slot,
         then" — and its four steps take @@asyncIterator first and @@iterator second, so either iteration
         method answers the async sequence arm. This is Web IDL §3.2.25's ONLY String-object carve-out. */
      if (has("async sequence") && !(has("string") && f.stringData)
          && (f.asyncIterator || f.iterator)) return "async sequence";
      /* 11.2 and 11.3 read @@iterator ONLY and carry NO String-object condition, so an object carrying
         @@asyncIterator alone falls past both while a String object — which inherits @@iterator — does not. */
      if (has("sequence") && f.iterator) return "sequence";                            /* step 11.2 */
      if (has("frozen array") && f.iterator) return "frozen array";                    /* step 11.3 */
      if (has("dictionary")) return "dictionary";                                      /* step 11.4 */
      if (has("record")) return "record";                                              /* step 11.5 */
      if (has("callback interface")) return "callback interface";                      /* step 11.6 */
      if (has("object")) return "object";                                              /* step 11.7 */
    }
    if (f.kind === "boolean" && has("boolean")) return "boolean";                      /* step 12 */
    if (f.kind === "number" && has("numeric")) return "numeric";                       /* step 13 */
    if (f.kind === "bigint" && has("bigint")) return "bigint";                         /* step 14 */
    return tail();
  };
  const map = {};
  for (const V of SHAPES) map[V] = of(V);
  return { map, key: SHAPES.map((V) => map[V]).join("|") };
}

const spell = (t) => t.union ? "(" + t.idlType.map(spell).join(" or ") + ")" + (t.nullable ? "?" : "")
  : t.generic ? `${t.generic}<${t.idlType.map(spell).join(", ")}>` + (t.nullable ? "?" : "")
  : String(t.idlType) + (t.nullable ? "?" : "");

/* ---- the join ---------------------------------------------------------------------------------------------- */
const env = loadEnvironment(HOST);
const cFiles = [...env.sources.keys()].filter((p) => p.endsWith(".c"));
const { declarationFor } = declarationIndex(cFiles, env, C);
const world = installedMembers(cFiles, env);

/* enumerator -> { unions: Map(partitionKey -> {spell, map, sites[]}), plain: Map(specType -> sites[]) } */
const byEnumerator = new Map();
const reach = [];
let positions = 0, unionPositions = 0;

for (const rec of world.records) {
  if (!rec.ifaces || rec.ifaces.length !== 1) continue;
  const iface = rec.ifaces[0];
  if (!idl.byName.get(iface)) continue;
  const ops = idl.flatten(iface).filter((m) => m.type === "operation" && m.name === rec.name);
  if (ops.length !== 1) continue;                 /* absent-from-spec is idlgen's; overloaded is not judged */
  const j = declarationFor(rec.file, rec.line);
  if (j.unjoined) { reach.push({ who: `${iface}.${rec.name}`, why: j.unjoined }); continue; }
  const decl = j.decl;
  if (!decl.types) { reach.push({ who: `${iface}.${rec.name}`,
    why: "the declaration's type list is an expression this cannot resolve to a named array" }); continue; }

  (ops[0].arguments || []).forEach((a, i) => {
    const declared = decl.types[i];
    if (!declared) return;                        /* an undeclared tail position is argtypegate's ARITY finding */
    const t = resolveTypedef(a.idlType);
    if (!t) return;
    positions++;
    if (!byEnumerator.has(declared)) byEnumerator.set(declared, { unions: new Map(), plain: new Map() });
    const e = byEnumerator.get(declared);
    const site = `${relative(ROOT, decl.file)}:${decl.line}  ${iface}.${rec.name} position ${i} (\`${a.name}\`)`;
    if (!t.union) {
      const s = spell(t);
      if (!e.plain.has(s)) e.plain.set(s, []);
      e.plain.get(s).push(site);
      return;
    }
    unionPositions++;
    const p = partition(t);
    if (!e.unions.has(p.key)) e.unions.set(p.key, { spells: new Set(), map: p.map, sites: [] });
    const u = e.unions.get(p.key);
    u.spells.add(spell(t));
    u.sites.push(site);
  });
}

/* ---- the findings ------------------------------------------------------------------------------------------ */
const showMap = (map, indent) =>
  SHAPES.map((V) => `${indent}${V.padEnd(SHAPE_PAD)} -> ${map[V]}`).join("\n");

/* HOW MANY ARMS A PARTITION ACTUALLY HAS, which is what decides whether a union NEEDS a union row. A partition
   whose only outcomes are one arm and §3.2.25 step 20's TypeError (and step 2's IDL null, which every nullable
   type answers) is a BRAND TEST and nothing more — `(Element or Text)` and a plain `Element` position are the
   same function over these shapes, differing only in which brand the position states, which is
   idl_arg_iface's question and not this one. Two or more substantive arms is where a plain type must lose one. */
const armCount = (map) => new Set(SHAPES.map((V) => map[V])
  .filter((a) => a !== "TypeError" && a !== "null" && a !== "undefined")).size;

/* `IDL_ANY` IS NOT AN ARM TEST AND MUST NOT BE GROUPED AS ONE. idl_args.h declares it as the type at which
   "NOTHING IS ASKED AND NOTHING IS COERCED", so a union position carrying it runs no §3.2.25 at all — every
   arm is unresolved and the body receives whatever the page passed. Reporting that as "one test answering four
   questions" would be false about the mechanism and would bury the enumerators that DO test. It is named here
   as one constant with the header's own reason, never as a table of which rows are unions. */
const NO_CONVERSION = "IDL_ANY";

const split = [], collapsed = [], unconverted = [];
for (const [enumerator, e] of byEnumerator) {
  if (!e.unions.size) continue;
  if (enumerator === NO_CONVERSION) { unconverted.push([enumerator, e]); continue; }
  if (e.unions.size > 1) split.push([enumerator, e]);
  if (e.plain.size && [...e.unions.values()].some((u) => armCount(u.map) >= 2))
    collapsed.push([enumerator, e]);
}

console.log(`union-arm audit — ${positions} joined argument position(s), ${unionPositions} of them a union type`);
console.log(`${byEnumerator.size} enumerator(s) declared at a joined position; `
  + `${[...byEnumerator.values()].filter((e) => e.unions.size).length} of them at a union\n`);

const band = (rows, title, blurb) => {
  console.log(`${title}: ${rows.length}`);
  if (rows.length) console.log(blurb);
};

band(split, "SPLIT", `
An enumerator is ONE arm test written at ONE site, so it computes ONE function from §3.2.25's value shapes to
arms. Each of these is declared at positions whose unions demand DIFFERENT functions, so the test answers at
most one of them and the other position is converted by a rule its IDL does not state.`);
for (const [enumerator, e] of split) {
  console.log(`\n  ${enumerator} — ${e.unions.size} distinct §3.2.25 partitions`);
  let n = 0;
  for (const [, u] of e.unions) {
    console.log(`    partition ${++n}: ${[...u.spells].join(" , ")}`);
    for (const s of u.sites) console.log(`        ${s}`);
    console.log(showMap(u.map, "        "));
  }
  const differing = SHAPES.filter((V) => new Set([...e.unions.values()].map((u) => u.map[V])).size > 1);
  console.log(`    the shapes the partitions disagree about: ${differing.join(", ")}`);
}

console.log();
band(collapsed, "COLLAPSED", `
An enumerator declared BOTH at a union position and at a position whose IDL type is not a union. §3.2.25 does
not run for the second, so one of the two is being converted by the other's rule — and where the non-union
type is one arm of the union, the remaining arms are unreachable at that position by construction.`);
for (const [enumerator, e] of collapsed) {
  console.log(`\n  ${enumerator}`);
  for (const [, u] of e.unions) {
    if (armCount(u.map) < 2) continue;
    console.log(`    union    ${[...u.spells].join(" , ")}   (${armCount(u.map)} substantive arms)`);
    for (const s of u.sites) console.log(`        ${s}`);
    console.log(showMap(u.map, "        "));
  }
  /* The non-union side is counted and sampled, not listed: `IDL_INTERFACE` stands at scores of ordinary
     positions and printing them all buries the union the band is about. */
  const plainTotal = [...e.plain.values()].reduce((n, s) => n + s.length, 0);
  console.log(`    also declared at ${plainTotal} NON-union position(s) of `
    + `${e.plain.size} type(s): ${[...e.plain.keys()].join(", ")}`);
  for (const [s, sites] of e.plain) console.log(`        e.g. \`${s}\`  ${sites[0]}`);
}

console.log();
band(unconverted, "UNCONVERTED", `
A union position declared \`${NO_CONVERSION}\` — the type idl_args.h documents as the one where "NOTHING IS ASKED
AND NOTHING IS COERCED". §3.2.25 never runs, so no arm is resolved and no arm's TypeError is thrown; the body
receives whatever the page passed. Counted apart from SPLIT because the mechanism differs: SPLIT is one test
answering two questions, this is no test at all.`);
for (const [, e] of unconverted)
  for (const [, u] of e.unions) {
    console.log(`\n    ${[...u.spells].join(" , ")}   (${armCount(u.map)} substantive arms)`);
    for (const s of u.sites) console.log(`        ${s}`);
  }

/* ---- the observations, which are NOT findings ------------------------------------------------------------- */
const classes = new Map();
for (const [enumerator, e] of byEnumerator) {
  if (enumerator === NO_CONVERSION) continue;   /* not an arm test, so not an answer to a partition */
  for (const [key, u] of e.unions) {
    if (!classes.has(key)) classes.set(key, { map: u.map, spells: new Set(), enums: new Set() });
    const c = classes.get(key);
    for (const s of u.spells) c.spells.add(s);
    c.enums.add(enumerator);
  }
}
const shared = [...classes.values()].filter((c) => c.enums.size > 1);
console.log(`\n\nPARTITION CLASSES: ${classes.size} over ${unionPositions} union position(s)`);
console.log(`  ${shared.length} class(es) answered by more than one enumerator — NOT judged. Two rows over one`);
console.log("  partition is how this engine spells two unions whose arms differ in DESTINATION rather than in");
console.log("  the test that picks them, which the header states with the example it was written for.");
for (const c of shared)
  console.log(`    { ${[...c.enums].join(", ")} }  <-  ${[...c.spells].join(" , ")}`);

if (ALL) {
  console.log("\n\nEVERY PARTITION — the arm map a differential's rows are written from, per union:");
  for (const [, c] of classes) {
    console.log(`\n  ${[...c.spells].join(" , ")}   [${[...c.enums].join(", ")}]`);
    console.log(showMap(c.map, "      "));
  }
}

console.log(`\nREACH: ${reach.length} installed operation(s) not joined to a declaration`
  + (REACH ? "" : "   (--reach to list)"));
if (REACH) for (const r of reach) console.log(`  ${r.who}: ${r.why}`);

console.log("\nThis audit REPORTS and exits 0 — see the header for what it cannot see, and in particular for");
console.log("why an enumerator standing over ONE partition is CONSISTENT here whatever its arm test says.");
