/* THE WEB IDL INDEX — @webref/idl, parsed once, flattened once, read by every gate that needs to know what the
 * PLATFORM provides.
 *
 * It was inline in idlgen.mjs, and the day a second consumer needed the same answer that placement became the
 * defect CLAUDE.md names for a build's source list: a second hand-written copy is right on the day it is
 * written and silently wrong on the day the first one learns something. There is exactly one thing this
 * question can be answered from — the W3C-curated corpus browsers are written against — so there is exactly one
 * reader of it here, and a consumer that wants "is `redirected` a member of Response" gets the SAME answer as
 * the consumer that wants "which members of Response does the engine still owe".
 *
 * WHAT IT FLATTENS, AND WHY EACH IS NOT OPTIONAL. An interface's member list as spelled in its own declaration
 * is not its member list: `inheritance` puts the base's members on the prototype chain, an `includes` statement
 * puts a mixin's members on the interface itself, a `partial interface` in another spec adds more, and an
 * `iterable<>`/`maplike<>`/`setlike<>` declaration defines named members (§3.7.9-§3.7.12) that appear nowhere
 * in the members array. A consumer diffing against the unflattened list reports members that are present and
 * misses members that are not — in both directions, which is the false-COMPLETE this whole family of gates
 * exists to remove.
 *
 * AND IT CARRIES THE DICTIONARIES, WHICH ARE THE HALF OF THE PLATFORM MADE ENTIRELY OF THE THING A MEMBER-NAME
 * INDEX LOOKS AT. An interface is a name with members hanging off it, so an index that collects the name and
 * skips a member reports a gap; a DICTIONARY is nothing BUT its member list, so an index that does not collect
 * the kind at all reports nothing in either direction — the surface is invisible rather than incomplete, which
 * is the excluded-check shape one level above the members. It matters because a dictionary is how a page hands
 * the platform its instructions (`{once: true}`, `{signal}`, `{credentials: "include"}`), so a member nothing
 * reads is a page instruction SILENTLY IGNORED: nothing is absent, no stub is there to point at, and the page's
 * own behaviour diverges with nothing to say so. §3.2.17 makes the same absence a CONVERSION that never ran —
 * a getter with a side effect the page expected, a `toString` that should have thrown.
 *
 * THE DECLARATIONS ARE HANDED BACK RAW as well as merged, because the two answer different questions and the
 * merged map cannot answer the second. `byName` keeps the FIRST node seen for a name and folds later members
 * into it, so an interface first met as a `partial` (which carries no extended attributes) loses the real
 * declaration's `[Exposed=Window]`. A consumer asking about extended attributes must therefore walk the
 * declarations; a consumer asking about members reads the merged map. Both are here, from one parse. */
import { listAll } from "@webref/idl";
import { parse } from "webidl2";

/* Web IDL §3.7.9-§3.7.12: the members an iteration declaration defines but does not spell out. `for (const [k,
   v] of headers)` is how half the code that touches one is written, and a member list without them reports an
   interface COMPLETE while every one of them is missing.
   §3.7.10's async set is NOT §3.7.9's: a pair async declaration defines `entries`, `keys` and `values`, a value
   one defines `values` alone, and NEITHER defines `forEach` — an async iterable has no synchronous walk to hand
   a callback to, so expecting one would report a member the spec does not define. */
export function iterationMembers(node) {
  const out = [];
  for (const m of node.members) {
    if (m.type === "async_iterable") {
      out.push("values");
      if (m.idlType && m.idlType.length === 2) out.push("entries", "keys");
      continue;
    }
    if (m.type !== "iterable" && m.type !== "maplike" && m.type !== "setlike") continue;
    const pair = m.type === "maplike" || (m.idlType && m.idlType.length === 2);
    out.push("forEach", "values");
    if (pair) out.push("entries", "keys");
    if (m.type === "maplike" || m.type === "setlike") out.push("has");
    if (m.type === "maplike") out.push("get");
  }
  return out;
}

export async function loadIdl() {
  const all = await listAll();
  /* Every top-level declaration of the whole corpus, in spec order, kept so a consumer that needs the
     UNMERGED node (extended attributes, per-declaration exposure) has it without a second parse. */
  const declarations = [];
  for (const spec of Object.values(all)) {
    let ast;
    try { ast = parse(await spec.text()); } catch { continue; }
    declarations.push(...ast);
  }

  const byName = new Map();
  const inheritanceOf = new Map();
  const includes = [];
  /* WEB IDL §2.7 Dictionaries — KEPT IN THEIR OWN MAP, never folded into `byName`. A dictionary is not an
     interface and the two questions this index answers about a name are different in kind: an interface has an
     interface object and a prototype a component INSTALLS members on, and a dictionary has neither — §2.7
     defines one as "an ordered map data type with a fixed, ordered set of entries" and an operation taking one
     "will perform a one-time conversion from the given JavaScript value", so there is no object for
     `instanceof` to test and nothing for the install audit to diff against. Sharing one map would make that
     confusion silent in the worst direction: `byName.has(x)` is what idlgen asks to decide whether an
     interface tag names something the corpus declares, so a tag misspelt as a dictionary's name would read as
     KNOWN, and `members()` — which collects `attribute`/`operation`/`const` and never `field` — would answer
     the EMPTY list for it, which is a false COMPLETE minted by this index. */
  const dictByName = new Map();
  const dictInheritanceOf = new Map();
  for (const n of declarations) {
    if (n.type === "dictionary" && n.name) {
      /* §2.7: "the IDL for dictionaries can be split into multiple parts by using partial dictionary
         definitions … All of the members that appear on each of the partial dictionary definitions are
         considered to be members of the dictionary itself." Merged the same way an interface's partials are,
         and for the same reason: a consumer diffing against one declaration reports members that are present. */
      if (n.inheritance) dictInheritanceOf.set(n.name, n.inheritance);
      const prev = dictByName.get(n.name);
      if (prev) prev.members.push(...n.members);
      else dictByName.set(n.name, n);
      continue;
    }
    /* A CALLBACK INTERFACE IS COLLECTED. Web IDL §3.7.1 gives one a callback interface OBJECT carrying its
       constants — `NodeFilter.SHOW_ELEMENT` is a real property of a real object a page reads.
       A NAMESPACE IS COLLECTED. §3.13 gives a namespace a real object on the global whose properties are its
       operations and attributes — `console.log`, `CSS.supports`. Skipping either kind means a consumer diffs
       against an EMPTY member list and reads COMPLETE against nothing at all. */
    if ((n.type === "interface" || n.type === "interface mixin" || n.type === "callback interface" ||
         n.type === "namespace") && n.name) {
      if (n.inheritance) inheritanceOf.set(n.name, n.inheritance);
      const prev = byName.get(n.name);
      if (prev) prev.members.push(...n.members);
      else byName.set(n.name, n);
    } else if (n.type === "includes") includes.push(n);
  }
  for (const inc of includes) {
    const host = byName.get(inc.target), mixin = byName.get(inc.includes);
    if (host && mixin) host.members.push(...mixin.members);
  }

  function flatten(name, seen = new Set()) {
    const node = byName.get(name);
    if (!node || seen.has(name)) return [];
    seen.add(name);
    const base = inheritanceOf.get(name);
    return [...(base ? flatten(base, seen) : []), ...node.members];
  }

  /* The member NAMES of one interface, flattened. §3.7.5's constants and the STATIC members count: a static
     lives on the interface object rather than the prototype, which changes where a component installs it and
     nothing about whether a page can read it. */
  function members(name) {
    const out = [], seen = new Set();
    const add = (n) => { if (n && !seen.has(n)) { seen.add(n); out.push(n); } };
    for (const m of flatten(name)) {
      if (m.type === "attribute") add(m.name);
      else if (m.type === "operation") add(m.name);
      else if (m.type === "const") add(m.name);
    }
    for (const m of flatten(name)) {
      if (m.type === "iterable" || m.type === "maplike" || m.type === "setlike" || m.type === "async_iterable")
        for (const n of iterationMembers({ members: [m] })) add(n);
    }
    return out;
  }

  /* §2.7's inherited dictionaries, LEAST DERIVED FIRST — "the set includes the dictionary E that D inherits
     from and all of E's inherited dictionaries", which §3.2.17 step 3 then orders "from least to most
     derived". `seen` is not defensive tidiness: §2.7 says a dictionary "must not be declared such that its
     inheritance hierarchy has a cycle", and the corpus is a thousand declarations from a hundred specs, so an
     index that loops on a malformed one hangs the gate instead of reporting it. */
  function dictChain(name) {
    const out = [], seen = new Set();
    for (let n = name; n && !seen.has(n); n = dictInheritanceOf.get(n)) { seen.add(n); out.unshift(n); }
    return out;
  }

  /* A DICTIONARY'S MEMBERS IN §3.2.17's READ ORDER, WHICH IS THE ORDER A PAGE OBSERVES. This is not a
     presentation detail and it is not a sorted name list: §3.2.17 Dictionary types' ES-to-IDL conversion reads
     each member with `? Get(jsDict, key)`, so a page with a getter on two members sees which one ran first, and
     §2.7 fixes that order as "inherited dictionary members are ordered before non-inherited members, and the
     dictionary members on the one dictionary definition (including any partial dictionary definitions) are
     ordered lexicographically by the Unicode codepoints that comprise their identifiers".
     JS compares strings by UTF-16 code unit and §2.7 says code POINT; Web IDL identifiers are ASCII
     (`[_-]?[A-Za-z][0-9A-Z_a-z-]*`), so the two orders coincide and there is nothing to keep in step — the same
     argument idlgen.mjs already makes for handing a sorted table to bsearch.
     `level` is which dictionary in the chain declares the member, 0 for the least-derived — the same number
     core/idl_args.h's IdlDictMember declares, so the two sides of a dictionary audit state one fact.
     A member's DEFAULT is carried as `hasDefault` and never as the value: §3.2.17 step 4.1.5 makes a defaulted
     member EXIST on the converted dictionary even where the page wrote nothing, which is a THIRD state beside
     present and absent, and whether the engine spells that default correctly is a question about the value that
     a name list cannot hold — it belongs to whoever compares the two, not to this index.
     `extAttrs` IS THE FIELD'S OWN AND NOT ITS TYPE'S, and the difference is the whole reason it is carried
     here rather than being read off `idlType` by a consumer. webidl2 puts §3.3.6 [EnforceRange] and §3.3.3
     [Clamp] on the FIELD node for a dictionary member — `[EnforceRange] unsigned long count` parses with the
     attribute on the member and an EMPTY `extAttrs` on the type — while an ARGUMENT carries them either side.
     A consumer reading only the type therefore sees a bare `unsigned long`, which is a DIFFERENT declared
     conversion — §3.3.6 [EnforceRange] replaces the modulo wrap §3.2.4.6 unsigned long performs with a throw —
     so dropping this field does not lose an annotation: it silently changes the type the member is compared
     against. */
  function dictMembers(name) {
    const out = [];
    const chain = dictChain(name);
    for (let level = 0; level < chain.length; level++) {
      const node = dictByName.get(chain[level]);
      if (!node) continue;
      const own = node.members.filter((m) => m.type === "field" && m.name);
      own.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
      for (const m of own)
        out.push({ name: m.name, required: !!m.required, hasDefault: m.default != null,
                   level, declaredBy: chain[level], idlType: m.idlType, extAttrs: m.extAttrs || [] });
    }
    return out;
  }

  /* EVERY DICTIONARY A DECLARED TYPE CAN REACH. The type an operation declares for an argument is where a
     dictionary is NAMED and the only place it is: §2.7's conversion has no interface object and no prototype,
     so nothing about the value says which dictionary it is. A dictionary therefore hides one level down as
     often as it appears at the top — `(AddEventListenerOptions or boolean)` is a union, `optional D options`
     is nullable-free but `sequence<(DOMString or SanitizerElementNamespace)>` puts one inside a sequence
     inside a union — and a walk that reads only `idlType.idlType` finds the first and misses the rest, which
     is a false COMPLETE for every member whose options bag is spelled that way. */
  function dictionaryTypesIn(t, out = new Set()) {
    if (!t) return out;
    if (Array.isArray(t)) { for (const x of t) dictionaryTypesIn(x, out); return out; }
    if (typeof t.idlType === "string") { if (dictByName.has(t.idlType)) out.add(t.idlType); return out; }
    return dictionaryTypesIn(t.idlType, out);
  }

  return { declarations, byName, inheritanceOf, flatten, members,
           dictByName, dictInheritanceOf, dictChain, dictMembers, dictionaryTypesIn };
}

/* ONE READER FOR AN EXTENDED ATTRIBUTE'S RIGHT-HAND SIDE, because Web IDL gives every one of them the SAME
 * two forms and this index used to answer that one question three different ways.
 *
 * Web IDL §2.14 Extended attributes is where the forms are given, and the ones that name identifiers are
 * `ExtendedAttributeIdent` (`[Global=Window]`) and `ExtendedAttributeIdentList` (`[Exposed=(Window,Worker)]`),
 * beside `ExtendedAttributeWildcard` (`[Exposed=*]`), which names no identifier and is not the absence of one.
 * `ExtendedAttributeNamedArgList` is the shape `[LegacyFactoryFunction=Image(optional unsigned long w)]` takes,
 * and webidl2 hands ITS rhs back as an `identifier` with the argument list carried separately — which is the
 * one non-obvious fact this reader depends on and the reason source 3 below needs no special case. Checked by
 * parsing one declaration of each form rather than by reading the grammar, because what this function must
 * agree with is the PARSER's rendering of §2.14 and not §2.14 itself.
 * (An earlier draft of this paragraph cited §2.12 and said there were four forms. §2.12 is "Objects
 * implementing interfaces" and there are six. Both were written from recollection and both were wrong, which
 * is the whole of why a number and a section go into a comment only after they have been fetched.)
 * `windowGlobals` below had a hand-rolled reader per SOURCE: one inside `exposedOnWindow` that
 * open-coded the string/array split, and one inside `legacyFactoryNames` that accepted `typeof rhs.value ===
 * "string"` and would therefore have DROPPED an identifier list had one ever been written. That is two right
 * answers to one question, which is the shape CLAUDE.md records as the one that drifts — and it is not a
 * hypothetical here: it is exactly WHY the fourth source below was missing. A source whose reader is spelled
 * at the source is a source somebody has to remember to spell, and §3.4.11's identifiers were the one nobody
 * did. Routing all four through one spelling is the root fix; adding a fifth hand-rolled reader would have
 * reproduced the cause while closing the symptom.
 *
 * IT LIVES HERE AND NOT IN idlgen.mjs, WHICH IS THE ONLY PLACE IT CAN LIVE. idlgen.mjs imports this file, so a
 * helper kept there is unreachable from the index that needs it and the choice is between moving it up and
 * writing a second copy. The same argument this file's own header makes for the corpus parse makes it for the
 * corpus's extended attributes: there is exactly one thing this question can be answered from, so there is
 * exactly one reader of it.
 *
 * NULL IS NOT THE EMPTY LIST AND NEITHER IS EXPOSED_STAR, and collapsing any pair of the three loses a fact a
 * caller decides on: `null` is "the construct carries no such attribute at all", which §3.3.7 answers by
 * walking outward rather than by defaulting; `[]` is "it carries one with no right-hand side", which §3.3.8
 * [Global] reads as naming the interface's own identifier; and EXPOSED_STAR is `*`, which is every global
 * rather than none. */
export const EXPOSED_STAR = Symbol("[Exposed=*]");
export const rhsNames = (ext) => {
  if (!ext) return null;
  if (!ext.rhs) return [];                                       // a bare [Global] names the interface itself
  if (ext.rhs.type === "*") return EXPOSED_STAR;
  if (ext.rhs.type === "identifier") return [ext.rhs.value];
  if (ext.rhs.type === "identifier-list") return ext.rhs.value.map((v) => (v && v.value !== undefined ? v.value : v));
  return null;
};
export const extOf = (node, name) => (node && node.extAttrs || []).find((a) => a.name === name);

/* EVERY GLOBAL NAME WEB IDL EXPOSES ON WINDOW. FOUR sources, all spec text, and each was missing from an
 * earlier answer to this question with a measured cost:
 *   1. Every interface / namespace / callback interface DECLARED `[Exposed=Window]` (or `[Exposed=*]`). Web IDL
 *      §3.7 Interfaces puts its interface OBJECT on the global, so `Node`, `Element`, `DOMException`,
 *      `WebSocket` are properties of Window that Window's own member list does not mention.
 *   2. Every member of the Window interface itself, flattened through its mixins — `fetch`, `setTimeout`,
 *      `onload`, `location`.
 *   3. §3.7.2 Legacy factory functions: `[LegacyFactoryFunction=Image(…)]` puts `Image` on the global as a
 *      built-in function object under a name that is neither the interface's nor a member of Window. The corpus
 *      has three — Image, Option, Audio — and each is a name a page reaches for by writing `new Image()`.
 *   4. §3.4.11 [LegacyWindowAlias]'s IDENTIFIERS, which is the same fact one legacy form over and was the one
 *      this list was written without. Web IDL §3.7 Interfaces: "If the [LegacyWindowAlias] extended attribute
 *      was specified on an exposed interface, then for each identifier in [LegacyWindowAlias]'s identifiers
 *      there exists a corresponding property on the Window global object. The name of the property is the given
 *      identifier, and its value is a reference to the interface object for the interface." The corpus declares
 *      five across four interfaces — `webkitURL` for URL, and `SVGPoint`, `SVGRect`, `SVGMatrix`,
 *      `WebKitCSSMatrix` for the geometry trio — and `webkitURL` is the ordinary blob-URL shim, which is to say
 *      the single most frequently feature-detected name in this whole population.
 *
 * WHAT ITS ABSENCE COST, WHICH IS NOT THE COST THE OTHER THREE HAD. Sources 1-3 decide whether a name reaches
 * browser/platform_names.h, and solver/absent.c's read hook does NOT throw on a name it finds there — it
 * LEAVES THE READ ALONE, so ordinary ECMAScript semantics run and `if (window.webkitURL)` takes §10.1.8.1
 * OrdinaryGet ( O, P, Receiver ) step 2.b's `undefined` arm. A name the table is MISSING takes the other arm:
 * the hook mints an example-free concolic under the source identity of server-injected app state, and the
 * guard FORKS. So the loss was not a missing throw. It was a fabricated world — an arm in which this engine
 * pretends to hold an API it has not built, whose very next member call has nothing to reach, which is the
 * mirror of the honest-absence silence and is the direction CLAUDE.md rates worse, because it manufactures a
 * flow that dies one line later instead of one that quietly does not run.
 *
 * TAKEN ONLY WHERE THE INTERFACE CARRYING IT IS ITSELF EXPOSED ON WINDOW — one gate for sources 3 and 4 alike,
 * and it is the spec's own gate for both: §3.8 Platform objects implementing interfaces step 3.2 puts a
 * factory function wherever its interface goes, and §3.7's alias sentence quoted above says "on an exposed
 * interface". §3.4.11 additionally requires Window to be in that exposure set ("The [LegacyWindowAlias]
 * extended attribute must not be specified on an interface that does not include the Window interface in its
 * exposure set"), so for an alias the gate is redundant with the conformance requirement rather than a
 * narrowing of it.
 *
 * AND THE ALIAS'S EXPOSURE SET IS NOT ITS INTERFACE'S — A DISTINCTION THAT IS LOAD-BEARING ONE FILE OVER AND
 * INERT HERE, SO IT IS NAMED RATHER THAN COPIED. idlgen.mjs's IDL_EXPOSURE rows carry a per-name SET, and
 * there the alias row must be Window ALONE: three of the four aliased interfaces are `[Exposed=(Window,Worker)]`,
 * so a row spelled from the interface's own set would put `SVGPoint` and its siblings in every worker realm.
 * THIS table asks a single Window-only membership question, so the set never appears in the answer and the
 * three `(Window,Worker)` interfaces pass the gate on their Window half exactly as they should. Copying the
 * IDL_EXPOSURE line's shape here would be answering a question this table does not ask. */
const exposedOnWindow = (node) => {
  const v = rhsNames(extOf(node, "Exposed"));
  if (v === null) return false;                 // no [Exposed] at all: not a global constructor
  if (v === EXPOSED_STAR) return true;          // [Exposed=*]
  return v.includes("Window");
};

/* §3.7.2's names. `rhsNames` is what reads them now: the reader that stood here required the rhs value to be a
   string, which is `identifier` and silently not `identifier-list`. No corpus declaration takes the list form
   today, so this is not a behaviour change — it is the removal of a second answer to a question that already
   had one, and of the way this function would have gone quietly wrong on the day one did. */
const legacyFactoryNames = (node) => {
  const v = rhsNames(extOf(node, "LegacyFactoryFunction"));
  return v === null || v === EXPOSED_STAR ? [] : v;
};

/* §3.4.11's identifiers, read through the same one reader. A bare `[LegacyWindowAlias]` gives `[]` and a `*`
   gives EXPOSED_STAR; neither names an identifier, so neither can name a property, and both yield nothing here
   rather than an empty name on the global. §3.4.11's own conformance requirements — the rhs form, at most one
   attribute per interface, Window in the exposure set — are CHECKED where a row is EMITTED from them, in
   idlgen.mjs's IDL_EXPOSURE derivation, because a violated one there changes what a row CLAIMS; this index
   answers a membership question that a violation can only ever under-answer, and staying silent is the sound
   direction for it. */
const legacyWindowAliasNames = (node) => {
  const v = rhsNames(extOf(node, "LegacyWindowAlias"));
  return v === null || v === EXPOSED_STAR ? [] : v;
};

/* Walk the DECLARATIONS, not the merged map: `byName` keeps the first node it saw for a name and folds later
   members into it, so an interface first met as a `partial` (which carries no [Exposed]) loses the real
   declaration's extended attributes — that is why Element and HTMLElement went missing from a table built off
   the map. A name is a global if ANY of its declarations says [Exposed=Window]. */
export function windowGlobals(idl) {
  const out = new Set();
  for (const n of idl.declarations) {
    if (!((n.type === "interface" || n.type === "namespace" || n.type === "callback interface")
          && n.name && exposedOnWindow(n)))
      continue;
    out.add(n.name);
    for (const f of legacyFactoryNames(n)) out.add(f);
    for (const a of legacyWindowAliasNames(n)) out.add(a);
  }
  for (const m of idl.members("Window")) out.add(m);
  return out;
}
