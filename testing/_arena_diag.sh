#!/usr/bin/env bash
# Diagnose why the arena did NOT clear branchless_cold (thr=2 unchanged). Re-run with the
# WASM_NAMES build + arena-activity probe. Answers: (1) is the arena active and capturing
# allocations (cow_stats arena=1, arenaUsedKB>0)?  (2) WHERE is the trap now (symbolize the
# deep_callmain_throw .stack frames). Measure, do not reason.
set -u
cd /d/APIClient || exit 1
PORT="${PORT:-8765}"; BASE="http://localhost:${PORT}"; H="node testing/harness.js"
if ! curl -s -o /dev/null "${BASE}/branchless_cold.html"; then node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 & sleep 2; fi
$H restart >/dev/null 2>&1
$H goto "${BASE}/branchless_cold.html" >/dev/null 2>&1
st=""
for i in $(seq 1 30); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
echo "state=$st"
echo "=== cow_stats (arena active? arenaUsedKB>0 => capturing drive allocs) ==="
$H worker "var r=self._whyRecords||[];return JSON.stringify((r.filter(function(x){return x&&x.phase==='cow_stats';})).slice(-6))" 2>&1 | head -3
echo "=== deep_callmain_throw stacks (wasm-function[N] frames to symbolize) ==="
$H worker "var r=self._whyRecords||[];return (r.filter(function(x){return x&&x.phase==='deep_callmain_throw';}).map(function(x){return x.stack||x.err||JSON.stringify(x);})).join('\n====\n')" 2>&1 | head -50
echo DONE
