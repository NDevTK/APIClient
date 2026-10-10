/* CLAUDE.md size budget.
 *
 * CLAUDE.md is injected into every agent at spawn, so its size is paid on every spawn. It holds rules only; the
 * history behind them is in docs/claude-record.md, which agents read on demand. This refuses a CLAUDE.md over
 * the budget.
 *
 *   node engine/mdgate.mjs [--md <path>]        exit 0 within budget, 1 over it, 2 usage
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const BUDGET_BYTES = 64 * 1024;

const argv = process.argv.slice(2);
let md = join(ROOT, "CLAUDE.md");
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === "--md" && argv[i + 1]) { md = argv[++i]; continue; }
  console.error("usage: node engine/mdgate.mjs [--md <path>]");
  process.exit(2);
}

const bytes = Buffer.byteLength(readFileSync(md, "utf8"), "utf8");
if (bytes > BUDGET_BYTES) {
  console.log(`[md-gate] REFUSED — ${md} is ${bytes} bytes against a budget of ${BUDGET_BYTES}. ` +
              "Move incident history to docs/claude-record.md or a commit message; keep one rule sentence.");
  process.exit(1);
}
console.log(`[md-gate] PASS — ${md} is ${bytes} of ${BUDGET_BYTES} bytes`);
