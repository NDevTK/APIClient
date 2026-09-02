/* THE ENUMERATOR-NAMING CONVENTION core/idl_args.h OWNS, READ ONCE.
 *
 * The engine states a Web IDL type as an `IdlArgType` enumerator, and it states it by a CONVENTION rather than
 * by a table: the enumerator is the IDL type's own spelling, upper-cased, with spaces as underscores, and an
 * extended attribute or a nullable wrapper appended as a suffix. Two audits need to go the other way — from
 * the IDL type a spec declares to the enumerator the engine would have to write for it — argtypegate.mjs at an
 * ARGUMENT position and dicttypegate.mjs at a DICTIONARY MEMBER. That is one convention asked twice, so it is
 * held here once.
 *
 * CLAUDE.md: "an auditor DERIVES the rule it checks from the code that owns it, NEVER restates it — a restated
 * rule is a second copy, and the one that drifts is the copy nobody runs against reality." A table mapping
 * `unsigned long` to IDL_UNSIGNED_LONG would be that copy. What this composes is what the convention PREDICTS;
 * whether the header declares such an enumerator is asked of the header (idl_argdecl.mjs's `contract`), so a
 * name this composes that the enum does not have is UNPLACEABLE — a statement about the audit's reach and
 * never about the engine.
 *
 * IT IS A FACTORY AND NOT A MODULE-LEVEL BUILD, for the reason §Browser-half gives about per-realm facts: the
 * corpus index is an argument, so two callers cannot silently share one built from whichever of them loaded
 * first, and a caller holding a filtered or older index gets an answer about THAT index.
 */

/* THE ONLY EXTENDED ATTRIBUTES THE CONVENTION SPELLS. §3.3.6 [EnforceRange] and §3.3.3 [Clamp] REPLACE the
   conversion, so idl_args.h makes each its own enumerator rather than a flag and the convention appends a
   suffix. Every OTHER extended attribute on a type changes the conversion in a way no enumerator name states —
   [LegacyNullToEmptyString] is the one this tree carries, and idl_setter_id declares it as a separate
   `null_to_empty` PARAMETER rather than as a type — so a position carrying one is UNPLACEABLE and judged
   against nothing. Reporting those as type mismatches is how an auditor manufactures work. */
export const SPELLED_EXT = new Set(["EnforceRange", "Clamp"]);

/* ---- what KIND of thing a spec type name is, read off the corpus's own declarations -----------------------
 *
 * A `callback interface` is NOT an interface and idl_args.h declares them apart — §3.2.16 Callback interface
 * types opens "IDL callback interface type values are represented by JavaScript Object values (including
 * function objects)", so it accepts a callable where §3.2.15 Interface types' brand test does not, which is
 * exactly the difference between `addEventListener(t, function(){})` working and throwing.
 * (The number this sentence carried when it lived in argtypegate.mjs was §3.2.19, which is Callback FUNCTION
 * types — the right claim under the wrong heading, corrected here against the spec's own text.) An audit that
 * folded the two reported the engine's correct IDL_CALLBACK_INTERFACE_NULLABLE as a defect at every
 * EventListener and NodeFilter position it saw, which is the kind of finding that gets an auditor muted. */
export function typeConvention(idl) {
  /* `enums` carries the VALUES and not only the names, because §3.2.18 says the value list IS the type — an
     audit comparing the engine's declared list against it needs them, and a name-only set could not. */
  const enums = new Map(), callbacks = new Set(), callbackIfaces = new Set(), typedefs = new Map();
  for (const n of idl.declarations) {
    if (n.type === "enum" && n.name) enums.set(n.name, (n.values || []).map((v) => v.value));
    else if (n.type === "callback" && n.name) callbacks.add(n.name);
    else if (n.type === "callback interface" && n.name) callbackIfaces.add(n.name);
    else if (n.type === "typedef" && n.name) typedefs.set(n.name, n.idlType);
  }

  /* Web IDL §2.11 Typedefs is pure abbreviation — "This new name is not exposed by language bindings; it is
     purely used as a shorthand for referencing the type in the IDL." — so a declaration states the type the
     typedef names and the audit must see through it. Bounded by the number of typedefs the corpus has: §2.11
     says "The Type must not be the identifier of the same or another typedef", and a corpus of a hundred specs
     is not a thing to trust on that.
     THE CITATION THIS COMMENT CARRIED WHEN IT LIVED IN argtypegate.mjs WAS WRONG ON BOTH AXES, and it is
     written out here rather than quietly corrected because the shape is the one CLAUDE.md warns about: it read
     `§2.4 typedefs are pure abbreviation — "an alternative way to refer to a type"`. §2.4 is Callback
     interfaces, and that sentence appears NOWHERE in Web IDL — a fabricated quotation, which is the citation
     failure a reader trusts most and verifies least, because a quote beside a number reads as removing the
     need to open the spec at all. citegen.mjs's quotation channel is what surfaced it. */
  function resolveTypedef(t, depth = 0) {
    if (!t || depth > 8) return t;
    if (typeof t.idlType === "string" && typedefs.has(t.idlType)) {
      const to = typedefs.get(t.idlType);
      /* THE FIELDS ARE NAMED AND THE NODE IS NOT SPREAD, and that is a correctness fix rather than a style.
         webidl2's `Type` keeps `generic`, `nullable`, `union` and `idlType` as PROTOTYPE ACCESSORS — its own
         enumerable properties are `type` and `extAttrs` and nothing else — so `{ ...to }` produces a node with
         NO TYPE IN IT. Every caller then read `typeof t.idlType === "string"` as false, composed no
         enumerator, and reported the position UNPLACEABLE: not a wrong answer, an ABSENT one, for every
         typedef in the platform at once (`DOMHighResTimeStamp`, `BodyInit`, `EventHandler`, …). §2.4 makes a
         typedef "an alternative way to refer to a type", so a reader that loses the type at the alias has
         stopped reading the declaration rather than seen through it — and it is silent, because an
         unplaceable row is exactly what an honest reach limit looks like.
         A typedef's own nullability composes with the reference's. */
      return resolveTypedef({ type: to.type, generic: to.generic, union: to.union, idlType: to.idlType,
                              nullable: to.nullable || t.nullable,
                              extAttrs: [...(to.extAttrs || []), ...(t.extAttrs || [])] }, depth + 1);
    }
    return t;
  }

  const extNames = (argNode, t) =>
    [...(t.extAttrs || []), ...(argNode.extAttrs || [])].map((a) => a.name);

  /* THE ENUMERATOR THE CONVENTION PREDICTS for one IDL type node, or null where the convention has nothing to
     say. The node is whatever CARRIES the type and its extended attributes — webidl2's `argument` node at a
     position, its `field` node at a dictionary member — and the two are read identically because §3.2's
     conversions are per TYPE and know nothing about where the type was written. */
  function enumeratorFor(argNode) {
    const t = resolveTypedef(argNode.idlType);
    if (!t) return null;
    if (t.union || t.generic) return null;         /* the union and generic enumerators are per-member: §3.2.25 */
    const base = typeof t.idlType === "string" ? t.idlType : null;
    if (!base) return null;
    const ext = extNames(argNode, t);
    if (ext.some((n) => !SPELLED_EXT.has(n))) return null;
    let stem;
    if (enums.has(base)) stem = "IDL_ENUM";
    else if (callbacks.has(base)) stem = "IDL_CALLBACK";
    else if (callbackIfaces.has(base)) stem = "IDL_CALLBACK_INTERFACE";
    else if (idl.dictByName.has(base)) stem = "IDL_DICT";
    else if (idl.byName.has(base)) stem = "IDL_INTERFACE";
    else stem = "IDL_" + base.toUpperCase().replace(/\s+/g, "_");
    if (ext.includes("EnforceRange")) stem += "_ENFORCE";
    else if (ext.includes("Clamp")) stem += "_CLAMP";
    if (t.nullable) stem += "_NULLABLE";
    return stem;
  }

  return { enums, callbacks, callbackIfaces, typedefs, resolveTypedef, extNames, enumeratorFor };
}
