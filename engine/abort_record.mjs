/* The abort a run's output records, and its identity.
 *
 * Both hosts write an abort the same way: check.h's `@WHY`/`@E {"phase":"assert","cond":…,"at":…,"reason":…}`
 * record, or engine/qjs/quickjs-check.h's prose `@WHY <msg> (<file>:<line>)`, and both abort on the next
 * statement. Under emscripten `abort()` delivers no signal: it throws `RuntimeError: Aborted(...)` and node exits
 * 1, which is also the fixture's exit code for an incomplete table, so the record is the witness and the exit
 * status is not. Matched at a line start and only in those two shapes, so output quoting the token cannot
 * manufacture one. */

const ABORT_WITNESS = /^@(?:WHY|E) (?:\{"phase":"assert"|.*\([^()]*:\d+\)$)/m;
const EMSCRIPTEN_ABORT = "Aborted(native code called abort())";
/* V8's own words for a wasm trap on an out-of-bounds access, the event a native host reports as SIGSEGV. */
const WASM_MEMORY_FAULT = "RuntimeError: memory access out of bounds";

/* The whole line of the first abort record in `out`, or null. The line is taken from the match's position
   and not its extent: the JSON alternative matches only a prefix. A raw abort with no record above it is
   still an abort, reported as one that names nothing to fix. */
export function abortRecord(out) {
  const m = out.match(ABORT_WITNESS);
  if (m) return out.slice(m.index).split("\n", 1)[0];
  if (out.includes(EMSCRIPTEN_ABORT))
    return `${EMSCRIPTEN_ABORT} — with NO @WHY/@E line above it, so this abort came from a raw abort()/assert ` +
           `rather than from check.h, and it carries no message naming what to fix`;
  return null;
}

/* The identity of the crash a run ended in, comparable across hosts, or null when it ended in none.
   An assertion's identity is its macro tag, its site and its condition text: the site is root-relative on
   both hosts (`-ffile-prefix-map` on both compilers) and the condition is source text, while the reason may
   carry runtime values such as addresses that legitimately differ between a native and a wasm heap. A
   signal names itself; `SIGXCPU` and `SIGKILL` are the budget and the backstop, which end a run without a
   crash. `signal` is the child's terminating signal as the spawn reported it, or null. */
export function crashIdentity(out, signal) {
  const rec = abortRecord(out);
  if (rec !== null) {
    const json = rec.match(/^(@WHY|@E) (\{.*\})$/);
    if (json) {
      const r = JSON.parse(json[2]);
      if (typeof r.at !== "string" || typeof r.cond !== "string")
        throw new Error(`check.h's assertion record carries no \`at\` or \`cond\` string: ${rec}`);
      return `${json[1]} ${r.at} ${r.cond}`;
    }
    const prose = rec.match(/^(@WHY|@E) .*\(([^()]*:\d+)\)$/);
    if (prose) return `${prose[1]} ${prose[2]}`;
    return "abort() with no assertion record";
  }
  if (signal === "SIGABRT") return "abort() with no assertion record";
  if (signal === "SIGSEGV" || signal === "SIGBUS" || out.includes(WASM_MEMORY_FAULT)) return "memory fault";
  if (signal && signal !== "SIGXCPU" && signal !== "SIGKILL") return signal;
  return null;
}
