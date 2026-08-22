#!/usr/bin/env bash
# DIRECT trap measurement (stop hypothesizing): dump every deep_callmain_throw's
# err + culprit + loc + stack for branchless. SAME culprit 4x = one bad orphan;
# VARYING culprit = systemic free-list corruption. The err string distinguishes
# OOB (wild ptr) vs Aborted() (assert) vs Cannot-enlarge (OOM).
set -u
PORT="${PORT:-8765}"
BASE="http://localhost:${PORT}"
H="node testing/harness.js"
if ! curl -s -o /dev/null "${BASE}/branchless_cold.html"; then
  node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 &
  sleep 2
fi
$H restart >/dev/null 2>&1
$H goto "${BASE}/branchless_cold.html" >/dev/null 2>&1
for i in $(seq 1 18); do
  st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"')
  case "$st" in *complete*|*stalled*) break;; esac
  sleep 3
done
echo "=== THROWS (err | culprit | loc) ==="
$H worker "var r=self._whyRecords||[];var t=r.filter(function(x){return x&&x.phase==='deep_callmain_throw';});return JSON.stringify(t.map(function(x){return {err:x.err,culprit:x.culprit,loc:x.loc||x.culpritLoc};}),null,1)" 2>&1 | head -60
echo "=== first throw STACK ==="
$H worker "var r=self._whyRecords||[];var t=r.filter(function(x){return x&&x.phase==='deep_callmain_throw';})[0];return t?(''+t.stack).slice(0,700):'none'" 2>&1 | head -30
echo "=== recycle reasons ==="
$H worker "var r=self._whyRecords||[];var c={};for(const x of r){if(x&&x.phase==='deep_recycle'){var k=x.reason||'?';c[k]=(c[k]||0)+1;}}return JSON.stringify(c)" 2>&1 | head -5
echo "DONE"
