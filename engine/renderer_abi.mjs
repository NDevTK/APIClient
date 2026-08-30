/* `content.mojom.Renderer` AS A NODE DRIVER SEES IT — the ONE declaration, the ONE placement, and the ONE
 * built artifact they are checked against.
 *
 * WHY IT EXISTS, AND THE FAILURE IT IS ANSWERING. `qjs_init` takes the fifteen facts a document arrival
 * carries, and every one of them is stated by the party that HAS it. There are five callers: the extension's
 * `renderer.html` (checked at bind by `rendererImpl`), `bridge.js` and `renderer_host_gate.mjs` (both checked
 * on send by `mojo.js`'s `checkValues`), and the two Node drivers that `ccall` the entry RAW — this repository's
 * `engine/route.mjs` and `engine/solvergate.mjs`. The raw pair go through no transport, so nothing checked
 * them at either end, and each one carried its own array of values in DECLARATION ORDER. That array has now
 * gone short three times, one parameter apart each time:
 *   - HTML §7.1.7's inherited policy container, whose CSP list was passed in the `headers` slot;
 *   - Permissions Policy §9.5's container answer;
 *   - and HTML §3.1.3 "Ancestor origins"' internal ancestor origin objects list, which is what this module was
 *     written for: `solvergate.mjs` stayed at fourteen operands, the fifteenth arrived as emscripten's
 *     zero-fill, and `navigable_root_ancestor_origins` aborted EVERY document under EVERY schedule — the whole
 *     solver differential, forty runs of forty, answering nothing while looking like one document's bug.
 * Emscripten's own wrapper is one-sided about exactly this: too MANY operands assert, too FEW are zero-filled
 * in silence, so the short direction — a wider engine driven by a narrower caller — has no reporter at all.
 *
 * SO THE LIST IS NOT WRITTEN DOWN HERE EITHER. `mojo.js`'s `abiOperands` walks the interface's own parameter
 * declarations and mints through the host's callback, refusing a declared parameter with no value (a driver
 * OLDER than the interface) and a value no parameter declares (one NEWER than it, or a misspelling). What this
 * module adds is the third skew neither end can see alone: the BUILT ARTIFACT's own declared arity, read out
 * of the glue, so a driver that agrees with the interface and disagrees with the wasm says so by name instead
 * of dying inside emscripten with a number.
 *
 * WHAT IS DELIBERATELY NOT ROUTED THROUGH HERE. `renderer.html`'s `callAbi` walks the SAME declaration with
 * the same `abiPlacement`, so it cannot go short — its operand count is derived, not kept. What it has that a
 * Node driver does not is a pointer-lifetime policy (a `retained` pointer lives for the life of the frame, a
 * transient one is freed when the call returns), which is why the minting stays the host's and only the walk
 * is shared.
 */
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readFileSync } from 'node:fs';
import { createContext, runInContext } from 'node:vm';

const EXT_DIR = join(dirname(fileURLToPath(import.meta.url)), '..', 'extension');

/* THE BUILT ARTIFACT, NAMED ONCE. Both Node drivers load this module and both check their operands against
   what it declares, so the path is one fact rather than a string each of them spells. */
export const GLUE_PATH = join(EXT_DIR, 'lib', 'qjs', 'qjs.mjs');

/* THE INTERFACE, BY ITS OWN BYTES. `extension/mojom.js` is the ONE description of what may cross (its own
   first paragraph: "an interface only exists if both ends agree on it").
   AN ISOLATED CONTEXT, NOT THIS REALM. `renderer_host_gate.mjs` loads these same three files with
   `runInThisContext` because it is IMPERSONATING the browser process and wants their globals; a wasm driver
   wants one declaration, so evaluating them over its own global would install `mojo`, `DCHECK` and `CHECK`
   into a realm whose fixtures do not expect them — a driver silently gaining the extension's assert machinery
   is a difference between what it runs and what production runs. The load order is ast-worker.html's, because
   mojo.js asserts through check.js and mojom.js declares through mojo.js. */
export const MOJO = (() => {
  const sandbox = { console };
  sandbox.self = sandbox;
  sandbox.globalThis = sandbox;
  createContext(sandbox);
  for (const f of ['check.js', 'mojo.js', 'mojom.js'])
    runInContext(readFileSync(join(EXT_DIR, f), 'utf8'), sandbox, { filename: join(EXT_DIR, f) });
  return sandbox.mojo;
})();

export const RENDERER = MOJO.interfaceOf('content.mojom.Renderer');

/* WHAT THE GLUE SAYS THE ENTRY ACCEPTS — READ FROM ITS SOURCE, NOT FROM THE FUNCTION OBJECT. `nargs` is a
   closure variable inside `createExportWrapper`'s returned arrow, so `String(M._qjs_init)` does not contain
   it; a check written that way parses nothing, matches nothing and passes always, which is the vacuous check
   this whole neighbourhood exists to prevent. The declaration is in the emitted text, which is where
   `assertEngineAbiPairing` already reads it from, so this asks the same artifact the same question. */
function glueArity(entry) {
  const src = readFileSync(GLUE_PATH, 'utf8');
  const m = src.match(
    new RegExp(`createExportWrapper\\("${entry}",\\s*wasmExports\\["${entry}"\\],\\s*(\\d+)\\)`));
  const n = Number((m || [])[1]);
  if (!Number.isFinite(n))
    throw new Error(`renderer_abi.mjs could not read \`${entry}\`'s declared arity out of ${GLUE_PATH} — the ` +
      "glue's shape changed, and an unanswerable check must say so rather than pass.");
  return n;
}

/* ONE ABI CALL'S OPERANDS, FOR A DRIVER THAT `ccall`s THE ENTRY RAW.
 *   `method` — the mojom method's own name (`Init`), which is what declares the parameters;
 *   `entry`  — the C symbol (`qjs_init`), which is the one fact a mojom does not carry: an interface declares
 *              methods, not symbols in a wasm module, so the binding is stated at the call site exactly as
 *              `renderer.html`'s `ABI` table states it;
 *   `values` — keyed by the declaration's own parameter names, never positional;
 *   `cs`     — the driver's string→pointer mint, into the module it booted.
 *
 * THE PLACEMENT IS FIXED HERE RATHER THAN LEFT TO EACH DRIVER, and that is the difference between this and
 * `mojo.js`'s `abiOperands`, which takes a minting callback because the extension's renderer has a pointer
 * LIFETIME policy to express (a `retained` pointer lives for the life of the frame, a transient one is freed
 * when the call returns). A Node driver has none: it mints into a module it tears down whole, so every
 * pointer is the frame's either way, and a per-driver `if (pl.form === …)` would be the same form switch
 * written twice — one of them able to answer a form the other does not.
 * A `bytes` placement is already the PAIR the C entry takes (a pointer and a LENGTH, because a length
 * recovered with `strlen` would end at a 0x00 the sequence may legitimately contain), built by the caller
 * because only the caller knows what it wrote into linear memory; a `string` is one pointer; anything else
 * CRASHES, because an invented operand count reads every later argument of the call one slot early.
 *
 * The arity cross-check is performed HERE rather than left to the caller, because a caller that can forget it
 * is a caller that will: it is the one of the three skews whose evidence lives outside both lists. */
export function abiOperands(method, entry, values, cs) {
  const m = RENDERER.methods.find((x) => x.name === method);

  if (m === undefined)
    throw new Error(`renderer_abi.mjs was asked to place \`content.mojom.Renderer.${method}\`, which the ` +
      'interface declares no method of — a driver naming a method that does not exist is one that would ' +
      'otherwise place nothing and call the entry with no arguments at all.');
  const operands = MOJO.abiOperands(m, values, (v, pl, p) => {
    if (pl.form === 'bytes') {
      if (!Array.isArray(v) || v.length !== 2)
        throw new Error(`renderer_abi.mjs was handed \`${p.name}\` for \`${entry}\` as something other than ` +
          'the (pointer, length) pair its declared `' + p.type + '` places as — the caller writes the bytes ' +
          'into linear memory, so the pair is the caller\'s to state and a single value here is a length ' +
          'this module would have to invent.');
      return v;
    }
    if (pl.form === 'string') return [cs(v)];
    throw new Error(`content.mojom.Renderer.${method} declares \`${p.name}\` as \`${p.type}\`, whose ` +
      `placement form is \`${pl.form}\`, and no Node driver performs one — \`mojo.js\`'s abiPlacement is ` +
      'where a form is derived and this is where one is carried out, so a form it answers and this cannot ' +
      'is half a placement, and a guessed operand count is a wrong call rather than a missing one.');
  });
  const want = glueArity(entry);

  if (operands.length !== want)
    throw new Error(`renderer_abi.mjs places ${operands.length} operand(s) into \`${entry}\` — walked off the ` +
      `${m.params.length} parameter(s) content.mojom.Renderer.${method} declares — and the built glue ` +
      `declares ${want}. One side of the ABI is older than the other, and emscripten cannot tell you which: ` +
      'its wrapper asserts only on too MANY (too few are zero-filled silently), and its message names a ' +
      'number rather than a parameter. Regenerate with `node engine/build.mjs`; ' +
      'extension/lib/qjs/qjs.mjs.build.json carries the revision the glue was built from.');
  return operands;
}
