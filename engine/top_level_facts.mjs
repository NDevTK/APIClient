/* HTML §7.5.1 "Shared document creation infrastructure"'s ELEVEN REMAINING FACTS, FOR A DOCUMENT THAT IS ITS
 * OWN TOP-LEVEL TRAVERSABLE — stated ONCE, for every host this engine is driven through.
 *
 * WHY IT EXISTS. `qjs_init` takes fifteen facts and four of them are per-run (the bytes, the address, the
 * document's name, the response's header block). The other ELEVEN are the same answers for every document
 * nothing embeds, and they were written out at every site that seats one — the LIST rather than a count of
 * it, because the list is what a reader can act on and the count is what they would quote onward:
 * `engine/trusted.mjs`'s `topLevelFacts`, `engine/one_document.mjs`'s `facts` and `engine/layout_cost.mjs`'s
 * record literal (the native `--abi` channel's ordered record); `engine/pagecensus.mjs`'s and
 * `engine/solvergate.mjs`'s `abiOperands` records (the wasm entry's named one); and `engine/route.mjs`'s
 * parameter DEFAULTS, which is the one this diff leaves and is the residual at the bottom of this banner.
 * `one_document.mjs` carried a landed residual naming this module as its next diff, in its own words: "the
 * eleven facts in one module both this file and `trusted.mjs` read, so there is one statement of them and no
 * copy to drift — which is possible only because they are pure data and need none of the zone's fetching,
 * and is therefore a smaller diff than importing the zone would be."
 *
 * AND THE COPY PROPAGATES ITS OWN JUSTIFICATION, WHICH IS WHY THE RESIDUAL UNDER-COUNTED ITS OWN POPULATION.
 * It named TWO files; there were six, and the third was found only by grepping for the ARGUMENT rather than
 * for the values. `layout_cost.mjs` said in as many words that its eleven were "engine/one_document.mjs's
 * `topLevelFacts`, copied for the reason that file states about copying them from engine/trusted.mjs" — a
 * retired argument inherited at one remove, from a file that had inherited it at one remove, each hop
 * carrying the previous one's reasoning forward without re-deriving it. The reasoning was sound and its
 * conclusion never followed: it establishes only that the facts must not come from THE ZONE, and this module
 * is not the zone.
 *
 * AND THE COPY IS NOT THE WHOLE OF WHAT IT COSTS, WHICH IS WHY THIS MODULE SPANS BOTH HOSTS RATHER THAN THE
 * TWO NATIVE DRIVERS THE RESIDUAL NAMES. CLAUDE.md's §Testing makes the ONE experiment nobody has run "the
 * same page on the host that CAN preempt", and that comparison is only a comparison if the two hosts seat the
 * SAME DOCUMENT: a §8.1.3.5 secure-context answer, an inherited policy or a §7.1.5 flag set that differs
 * between them is a difference in the SUBJECT, and every census row below it would be a reading of two
 * different pages reported as one host against another. Every site above was a chance for that, and nothing
 * anywhere compared them. So the facts are stated here and RENDERED twice — named for the wasm
 * entry, ordered for the native channel — from one statement, which is what makes the two renderings unable to
 * disagree.
 *
 * WHAT EACH OF THE ELEVEN IS, AND WHY EVERY ONE IS A POSITIVE STATEMENT RATHER THAN A BLANK. This paragraph is
 * `engine/trusted.mjs`'s, moved rather than copied: it was the fullest statement of the set in the tree and
 * leaving it where ONE of its consumers could read it is the same defect as leaving the values there.
 *   · §8.1.3.1 "Environments"' TOP-LEVEL CREATION URL is this document's own address. §8.1.3.5 reads it to
 *     decide whether the document is a SECURE CONTEXT, and Web IDL §3.3.13's members exist in that realm or do
 *     not by that answer, so a wrong one is a different set of globals.
 *   · §7.1.7 "Policy containers"' inherited container is EMPTY IN BOTH HALVES — §7.1.7 clones a CREATOR's and
 *     a top-level traversable has none. Two halves because CSP §2.2 "Policies" makes a CSP list "a struct
 *     consisting of policies (a list of policies) and a self-origin" and §2.2.2 "Parse response's Content
 *     Security Policies" states the second from OUTSIDE the policy bytes, so it cannot be recovered from them.
 *   · §7.1.4 "Cross-origin embedder policies"' item of that same container has NO empty spelling: §7.1.7 gives
 *     every container one, so a container with no creator states that section's own initial value. The two
 *     values are §7.1.4's token strings, and main.c refuses one naming none of the three rather than reading
 *     it as a default.
 *   · §7.3.1.3 "Child navigables" defines "is a child navigable" as "its parent is non-null", so `u` —
 *     core/frame/remote_object.h's undefined — is the positive statement that this navigable has none.
 *   · Permissions Policy §9.5 "Create a Permissions Policy for a navigable" is given "null or an element
 *     (container) and an origin"; `null` is that grammar's word for the first, and nothing embeds this. It is
 *     a SEPARATE statement from the parent above it for the reason §7.3.1.3 defines the two links separately:
 *     a parent is a navigable and a container is the ELEMENT that presents it.
 *   · HTML §3.1.3 "Ancestor origins"' INTERNAL ANCESTOR ORIGIN OBJECTS LIST is `none` by the same sentence
 *     read one algorithm along — a THIRD statement about the same navigable and not a derivation of the two
 *     above it. An EMPTY FIELD is a different claim: it is the positive assertion that the Document is at the
 *     top of its own tree, which is what a host that stopped writing the field would silently tell a framed
 *     document.
 *   · HTML §7.1.5 "Sandboxing"'s CREATION SANDBOXING FLAG SET is EMPTY by that sentence read one algorithm
 *     further: the section fills a top-level browsing context's set from its POPUP sandboxing flag set, which
 *     is empty when the context is created and which only §7.3.1.7 "Navigable target names"'s rules for
 *     choosing a navigable ever populate — and nothing chose this one. `none` is that grammar's word for the
 *     empty set, stated rather than left blank because a navigable either carries flags or carries none and
 *     both are facts a host states.
 *
 * THE ORDER IS WALKED OFF THE DECLARATION AND IS NOT WRITTEN DOWN HERE. `content.mojom.Renderer.Init` is the
 * one description of what may cross, and `engine/renderer_abi.mjs` already exists because the operand list
 * went short THREE TIMES between the two drivers that keep one — a parameter added in the middle, and every
 * later value read one slot early in silence. The native channel has exactly that hazard with none of that
 * repair: its record is a TAB-joined array in declaration order, `abi_take` refuses an absent field but
 * nothing refuses a SHORT one written by a driver older than the interface, and a value in the wrong slot
 * parses. So the eleven names come from `Init`'s own parameters from `topLevelUrl` onward, and a parameter
 * inserted among them moves both renderings together or refuses both.
 *
 * IMPORTING `renderer_abi.mjs` COSTS A NATIVE DRIVER NOTHING, WHICH IS THE ONE THING THAT MAKES THIS MODULE
 * REACHABLE FROM BOTH HOSTS. That module reads `extension/{check.js,mojo.js,mojom.js}` into a vm context at
 * import and reads the BUILT GLUE only inside `abiOperands`, so a process that never places a wasm operand
 * never opens the artifact — which is what `one_document.mjs`'s own residual requires of its replacement
 * ("pure data and need none of the zone's fetching").
 *
 * NAMED RESIDUAL — `engine/route.mjs`. NOT COVERED: that driver states the same eleven as PARAMETER DEFAULTS
 * on `makeEngine`, because it provisions CHILD documents and must be able to pass a creator's real values, so
 * its copy is a default rather than a constant and converting it changes a signature. WHAT THE NEXT DIFF
 * BUILDS: `makeEngine` takes the eleven as one RECORD rather than as eleven trailing parameters, defaulted
 * from `topLevelFacts(url)` as a whole, so a peer's values and a top-level document's come from one shape.
 * HOW ITS ABSENCE WOULD SHOW: a value changed here and not there seats a document driven through
 * `route.mjs` under a different §7.1.4 embedder policy or §7.1.5 flag set from the same document driven
 * through any other host, and the two runs' findings differ with nothing naming the document as the
 * difference. */
import { RENDERER } from './renderer_abi.mjs';

/* THE PARAMETER THE ELEVEN BEGIN AT. Stated as a NAME rather than as an index, because an index is an ordinal
   over a set that grows and this module exists because that set grew three times. */
const FIRST = 'topLevelUrl';

const INIT = RENDERER.methods.find((m) => m.name === 'Init');
if (INIT === undefined)
  throw new Error('top_level_facts.mjs: `content.mojom.Renderer` declares no `Init` method — the eleven facts ' +
                  'below are that method\'s trailing parameters and there is nothing to walk them off, so ' +
                  'every host driven from here would seat a document on values nothing had ordered.');

/* THE NAMES, IN THE DECLARATION'S ORDER. */
export const TOP_LEVEL_FACT_NAMES = (() => {
  const i = INIT.params.findIndex((p) => p.name === FIRST);
  if (i < 0)
    throw new Error(`top_level_facts.mjs: \`content.mojom.Renderer.Init\` declares no \`${FIRST}\` parameter — ` +
                    'the eleven facts are the tail of that declaration beginning at it, so a renamed or ' +
                    'removed parameter is an interface this module can no longer find its own subject in.');
  return Object.freeze(INIT.params.slice(i).map((p) => p.name));
})();

/* THE ONE STATEMENT. Every value is a string and every one is the answer the paragraph above argues for; the
   only per-run member is the address, which is the caller's. */
function statement(url) {
  return {
    topLevelUrl: url,
    inheritedCsp: '',
    inheritedCspSelfOrigin: '',
    inheritedCoep: 'unsafe-none',
    inheritedCoepEndpoint: '',
    inheritedCoepReportOnly: 'unsafe-none',
    inheritedCoepReportOnlyEndpoint: '',
    parentNavigable: 'u',
    containerPolicy: 'null',
    ancestorOrigins: 'none',
    creationSandboxFlags: 'none',
  };
}

/* BOTH DIRECTIONS OF THE SKEW, REFUSED HERE — which is `abiOperands`' pair of checks, owed at this composer
   because the NATIVE rendering below never goes through that function and therefore through neither of them.
   A declared parameter this statement has no value for is a module OLDER than the interface; a key the
   interface declares no parameter of is one NEWER than it, or a misspelling. Each is silent on its own: the
   first shifts every later field of the native record one slot, and the second crosses a value to a reader
   that never looks at it. */
function checked(url) {
  if (typeof url !== 'string' || url === '')
    throw new Error('top_level_facts.mjs: a top-level document was composed with no ADDRESS — §8.1.3.1 gives ' +
                    'every environment a top-level creation URL, `abi_main` refuses an empty one, and every ' +
                    'relative URL the page builds resolves against it, so an absent one is a document seated ' +
                    'nowhere rather than one seated badly.');
  const v = statement(url);
  const have = Object.keys(v);
  for (const name of TOP_LEVEL_FACT_NAMES)
    if (!Object.prototype.hasOwnProperty.call(v, name))
      throw new Error(`top_level_facts.mjs states no value for \`${name}\`, which ` +
                      '`content.mojom.Renderer.Init` declares — this module is OLDER than the interface, and ' +
                      'the native channel would join its record one field short: `abi_take` refuses an ABSENT ' +
                      'field and nothing refuses a short record, so every field after the gap would be read ' +
                      'one slot early and arrive looking like a value.');
  for (const name of have)
    if (!TOP_LEVEL_FACT_NAMES.includes(name))
      throw new Error(`top_level_facts.mjs states \`${name}\`, which \`content.mojom.Renderer.Init\` declares ` +
                      'no parameter of among its trailing eleven — this module is NEWER than the interface, ' +
                      'or the name is misspelled, and either way the value crosses to a reader that never ' +
                      'looks at it while the parameter it was meant for goes unstated.');
  for (const name of have)
    if (typeof v[name] !== 'string')
      throw new Error(`top_level_facts.mjs states \`${name}\` as a ${typeof v[name]} — every one of these ` +
                      'eleven reaches the ABI as a C string, so a non-string is a value that would arrive ' +
                      'having been stringified by whichever host happened to compose it.');
  return v;
}

/* THE WASM ENTRY'S SHAPE — keyed by the declaration's own parameter names, to be spread into an
   `abiOperands("Init", "qjs_init", …)` record beside the four per-run facts that host states itself. */
export function topLevelFacts(url) { return checked(url); }

/* THE NATIVE `--abi` CHANNEL'S SHAPE — the same eleven, in the declaration's order, with that channel's own
   encoding applied.
 *
 * THE ENCODING IS A FACT OF THE CHANNEL AND NOT OF THE INTERFACE, WHICH IS WHY IT IS STATED HERE AND NOT
 * DERIVED. `test_forced.c`'s `abi_main` takes exactly one of the eleven through `abi_bytes` — the inherited
 * CSP list — and its own comment says why: "a raw CSP header field value may contain HTAB (RFC 9110 §5.5
 * "Field Values") and this channel splits on one — every other field below is a token, an origin
 * serialization or a grammar that asserts it has no tab". The interface declares all eleven `string`, so
 * nothing in the declaration distinguishes them and a walk over it could not find this. It is one sentence
 * with one reader, which is the whole point of the module.
 *
 * AND THE ASSERT IS THE CHEAP HALF: a value that is not base64 reaches the child's `abi_bytes` as a `DCHECKF`
 * naming a grammar the two ends do not share, one process away from the composer that wrote it. */
const NATIVE_BASE64 = new Set(['inheritedCsp']);

export function topLevelFactFields(url) {
  const v = checked(url);
  const fields = TOP_LEVEL_FACT_NAMES.map((name) =>
    NATIVE_BASE64.has(name) ? Buffer.from(v[name]).toString('base64') : v[name]);
  /* NO FIELD MAY CONTAIN THE SEPARATOR, asserted where the record is composed rather than discovered where it
     is split. `abi_take` finds the NEXT tab, so a value carrying one becomes two fields and every field after
     it is read one slot early — the identical skew the two checks above refuse for the arity, arriving through
     a VALUE instead of through a count. The one field that could legitimately carry one is the CSP list, and
     it is the one this channel base64s. */
  for (let i = 0; i < fields.length; i++)
    if (fields[i].includes('\t') || fields[i].includes('\n'))
      throw new Error(`top_level_facts.mjs: the value for \`${TOP_LEVEL_FACT_NAMES[i]}\` carries a TAB or a ` +
                      'NEWLINE, and the native `--abi` record is one line of tab-separated fields — so this ' +
                      'value would be split into two fields and every fact after it read one slot early, ' +
                      'which `abi_take` cannot see because the record it receives is well-formed.');
  return fields;
}
