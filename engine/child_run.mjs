/* ONE BUDGETED CHILD RUN — the spawn engine/build.mjs makes for every program it runs, in one place, so the
 * serial driver (`runChild`) and the parallel one (`runChildren`, a fixture document per process) measure
 * their children identically.
 *
 * The child runs under RLIMIT_CPU and writes its output to a file. `spawnSync` reaps it before returning, so
 * the cutime/cstime delta around the call is that child's CPU and no other's. A parent running several
 * children at once cannot take that delta (its reaps interleave), so it runs this file as a program per
 * child: `node engine/child_run.mjs <spec-json>` makes the same spawn and prints the outcome as one JSON line,
 * and each child keeps a reaper of its own. */

import { spawnSync } from "node:child_process";
import { openSync, closeSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { childCpuSeconds, childCpuDelta } from "./gate_cpu.mjs";

/* THE BUDGET IS INSTALLED BY THE ONE PROGRAM THAT CAN INSTALL IT — node has no setrlimit — and the child is
   `exec`'d so the shell is REPLACED rather than left as a layer between the reporter and the signal it
   reads. A budget that silently failed to install would be the defaulted-field defect wearing a rlimit: the
   run would look measured and be unmeasured, so the failure to set it is a LOUD marker and a distinct exit
   code, never a silent continuation. `$0` is the wrapper's name, `$1`/`$2` the two limits, `$3` the program. */
export const CPU_BUDGET_SH =
  'ulimit -H -t "$1" 2>/dev/null; ulimit -S -t "$2" || { echo "@BUDGET-NOT-INSTALLED"; exit 125; }; ' +
  'p="$3"; shift 3; exec "$p" "$@"';
export const BUDGET_NOT_INSTALLED = "@BUDGET-NOT-INSTALLED";

/* The kernel's count of processes its out-of-memory killer took inside this process's memory cgroup (v2
   `memory.events`, v1 `memory.oom_control`), or null when no cgroup file can be read. A SIGKILL is the OOM
   killer's or anybody's, and the rise of this counter across a run is what tells the two apart. Null is an
   absence: the counter was not readable, which says nothing about whether a kill happened. */
export function cgroupOomKills() {
  let rel = null;
  try {
    for (const line of readFileSync("/proc/self/cgroup", "utf8").split("\n")) {
      const [, ctrl, path] = line.split(":");
      if (ctrl === "memory" || (ctrl === "" && rel === null)) rel = path;
    }
  } catch { return null; }
  if (rel === null) return null;
  for (const f of [`/sys/fs/cgroup${rel}/memory.events`, `/sys/fs/cgroup/memory${rel}/memory.oom_control`]) {
    let text;
    try { text = readFileSync(f, "utf8"); } catch { continue; }
    const m = /^oom_kill (\d+)$/m.exec(text);
    if (m) return Number(m[1]);
  }
  return null;
}

/* Runs `prog args` under the two CPU limits and the wall-clock backstop, output to `log`. Returns the fields
   build.mjs's `runOutcome` reads: status, signal, error (its `code` only), cpuSeconds, wallSeconds, and
   oomKills, the cgroup's OOM-kill count's rise across the run (null when the counter is unreadable). */
export function spawnBudgeted({ slug, prog, args, log, hardS, softS, deadlockMs }) {
  for (const [k, v] of Object.entries({ slug, prog, log }))
    if (typeof v !== "string" || !v) throw new Error(`[child_run] spec field \`${k}\` is ${JSON.stringify(v)}`);
  for (const [k, v] of Object.entries({ hardS, softS, deadlockMs }))
    if (!(Number.isInteger(v) && v > 0)) throw new Error(`[child_run] spec field \`${k}\` is ${JSON.stringify(v)}`);
  if (!Array.isArray(args)) throw new Error(`[child_run] spec field \`args\` is ${JSON.stringify(args)}`);
  const fd = openSync(log, "w");
  const oomBefore = cgroupOomKills();
  const cpuBefore = childCpuSeconds();
  const wallBefore = Date.now();
  let t;
  try {
    t = spawnSync("/bin/sh",
                  ["-c", CPU_BUDGET_SH, `apiclient-build:${slug}`, String(hardS), String(softS), prog, ...args],
                  { stdio: ["inherit", fd, fd], shell: false, timeout: deadlockMs });
  }
  finally { closeSync(fd); }
  const cpuAfter = childCpuSeconds();
  const oomAfter = cgroupOomKills();
  return { status: t.status, signal: t.signal, error: t.error ? { code: t.error.code, message: t.error.message } : null,
           cpuSeconds: childCpuDelta(cpuBefore, cpuAfter), wallSeconds: (Date.now() - wallBefore) / 1000,
           oomKills: oomBefore === null || oomAfter === null ? null : oomAfter - oomBefore };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  if (process.argv.length !== 3) throw new Error("usage: node engine/child_run.mjs <spec-json>");
  process.stdout.write(JSON.stringify(spawnBudgeted(JSON.parse(process.argv[2]))) + "\n");
}
