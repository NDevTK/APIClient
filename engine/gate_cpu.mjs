/* THE CPU A SPAWNED CHILD ACTUALLY CONSUMED — ONE READER, because there were THREE and TWO OF THEM READ THE
   WRONG FIELDS, and the wrong reading was printed as a measurement inside the very sentence that argues for
   measuring the right thing. §Testing: "a measurement that a loaded machine can falsify is not a measurement",
   and the fix it names is CPU actually consumed. This file is that meter.
   WHAT WENT WRONG, kept because the defect SHAPE is permanent even though the coordinates are not. Two drivers
   carried a hand-copied helper that indexed /proc/self/stat at `v[11]`/`v[12]` under a comment reading
   "fields 16,17: cutime, cstime". Those indices are fields 14 and 15 — `utime`/`stime`, THIS process's own CPU
   — so the number every verdict below rested on was the CPU consumed by the PARENT while it sat in `waitpid`,
   which is approximately zero by construction. It read 0.0 for a child the KERNEL had just killed for spending
   sixty seconds, and the message printed both numbers side by side with equal confidence and drew its
   conclusion from the one that contradicted the signal. Reproduced: a child under `ulimit -S -t 1` dies on
   SIGXCPU having burned 0.99 s; `v[11]+v[12]` reports 0.01 s and `v[13]+v[14]` reports 0.99 s.
   SO CALLERS NAME THE FIELD NUMBER proc(5) GIVES IT, NEVER AN ARRAY INDEX. The comment and the code stated
   different numbers and nothing in the program could compare them; here they are the same token, so the
   mismatch that caused this is not expressible. */

import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";

/* THE UNITS, ASKED RATHER THAN ASSUMED. USER_HZ is 100 on every Linux this project has run on, and writing 100
   here would be a number nobody fetched — the same thing as a spec section quoted from memory. An answer that
   is not a positive integer THROWS rather than being defaulted to the number that is usually right. */
let CLK_TCK = null;
export function clockTicks() {
  if (CLK_TCK === null) {
    const g = spawnSync("getconf", ["CLK_TCK"], { encoding: "utf8" });
    const n = g.status === 0 ? Number((g.stdout || "").trim()) : NaN;
    if (!Number.isInteger(n) || n <= 0)
      throw new Error(`[gate_cpu] \`getconf CLK_TCK\` did not answer a positive integer (status ${g.status}, ` +
                      `stdout ${JSON.stringify(g.stdout)}) — /proc/self/stat reports child CPU in those units ` +
                      "and this meter will not guess them.");
    CLK_TCK = n;
  }
  return CLK_TCK;
}

/* proc(5) numbers the stat fields from 1; the first two are `pid` and `comm`, and comm is parenthesised and may
   itself contain a space or a `)`, so the record is split after its LAST `)` and what follows begins at field 3.
   THE ANCHOR IS ASSERTED, not assumed, and it is chosen to be structural rather than environmental: field 3 is
   `state`, a SINGLE character from the kernel's own set, and field 20 is `num_threads`, which is at least 1 for
   a process that is running enough to be read. If index 0 really is field 3 then `n - 3` really is field n for
   every n, by construction — so these two witnesses are the whole of what can go wrong with the arithmetic, and
   a comm that ends in a `)` mid-record cannot satisfy both. */
const PROC_STATES = "RSDZTtWXxKPI";
function statFieldReader() {
  let s;
  try { s = readFileSync("/proc/self/stat", "utf8"); } catch { return null; }
  const close = s.lastIndexOf(")");
  if (close < 0) return null;
  const v = s.slice(close + 2).split(" ");
  const at = (n) => Number(v[n - 3]);
  if (v.length < 18)
    throw new Error(`[gate_cpu] /proc/self/stat carried only ${v.length} fields past comm, so field 17 ` +
                    "(cstime) is not in the record this meter parsed. The parse landed somewhere that is not " +
                    "the start of field 3.");
  if (v[0].length !== 1 || !PROC_STATES.includes(v[0]) || !(at(20) >= 1))
    throw new Error(`[gate_cpu] the field past comm is ${JSON.stringify(v[0])} with num_threads ` +
                    `${JSON.stringify(v[17])} — proc(5) field 3 is a single-character state and field 20 is a ` +
                    "positive thread count, so this parse is NOT anchored at field 3 and every field number " +
                    "below it would name the wrong column. That is exactly how this meter reported a parent's " +
                    "own idle CPU as a killed child's sixty seconds.");
  return at;
}

/* THE CPU THIS PROCESS'S REAPED CHILDREN ACTUALLY CONSUMED — the kernel's own accounting. `cutime`/`cstime`
   (fields 16 and 17) accumulate the CPU of children this process has WAITED FOR, and `spawnSync` reaps before
   it returns, so the delta across one such call IS that child's CPU: no wrapper, no accounting a kill can
   destroy, and nothing a loaded box can move.
   A HOST WITHOUT IT ANSWERS `null`, WHICH IS A POSITIVE STATEMENT AND NOT A ZERO. Every consumer prints "not
   measured" rather than a plausible 0.0 s: a zero CPU reading is precisely the evidence a deadlock verdict
   rests on, so a host that cannot measure must never be able to manufacture one. The BUDGET is unaffected —
   the kernel enforces the rlimit whether or not this file can read the meter. */
export function childCpuSeconds() {
  const at = statFieldReader();
  if (at === null) return null;
  const cutime = at(16), cstime = at(17);
  if (!Number.isFinite(cutime) || !Number.isFinite(cstime))
    throw new Error("[gate_cpu] /proc/self/stat did not carry numeric cutime/cstime at fields 16/17 " +
                    `(${JSON.stringify([cutime, cstime])}) — the child-CPU meter every verdict below is ` +
                    "measured in reads them, and a non-number there must be fixed rather than absorbed.");
  return (cutime + cstime) / clockTicks();
}

/* THE SUBTRACTION IS OWNED HERE BECAUSE JAVASCRIPT WILL DO IT WRONG FOR FREE. `null - null` is `0` and
   `after - null` is `after`: an ABSENT measurement becomes the one number that means "this process was asleep",
   which is the defaulted-field defect arriving through an arithmetic operator rather than through a `||`. An
   unmeasurable pair is `null` and stays `null` all the way to the message. */
export const childCpuDelta = (before, after) =>
  before === null || after === null ? null : after - before;

/* HOW AN ABSENT NUMBER PRINTS. Never `0.0`, never omitted — a reader who is told nothing was measured can go
   and find out why, and a reader shown a zero cannot tell that from a process that consumed nothing. */
export const cpuText = (v, digits = 1) =>
  v === null ? "not measured on this host (no /proc/self/stat child accounting)" : `${v.toFixed(digits)} s`;
