#!/usr/bin/env bash
# Re-symbolize branchless + body_async AFTER term+opq pooled (cold_transport->0 but
# these unchanged). The OOB moved to a 3rd source. WASM_NAMES build of current source.
set -u
cd /d/APIClient || exit 1
PORT="${PORT:-8765}"; BASE="http://localhost:${PORT}"; H="node testing/harness.js"
if ! curl -s -o /dev/null "${BASE}/branchless_cold.html"; then node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 & sleep 2; fi
WASM_NAMES=1 node engine/build.mjs cow > engine/.work/build-names3.log 2>&1
echo "BUILD=$? cerr=$(grep -ciE 'error:' engine/.work/build-names3.log)"
for fx in branchless_cold body_async_shape; do
  $H restart >/dev/null 2>&1
  $H goto "${BASE}/${fx}.html" >/dev/null 2>&1
  for i in $(seq 1 18); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
  echo "=== ${fx} thr=$($H worker "var r=self._whyRecords||[],t=0;for(const x of r)if(x&&x.phase==='deep_callmain_throw')t++;return t" 2>&1 | head -1) (raw indices) ==="
  $H worker "var r=self._whyRecords||[];var t=r.filter(function(x){return x&&x.phase==='deep_callmain_throw';});return JSON.stringify(t.slice(0,2).map(function(x){return (''+x.stack).split('\n').slice(0,9).map(function(s){var m=s.match(/wasm-function\[(\d+)\]/);return m?+m[1]:null;}).filter(function(v){return v!==null;});}));" 2>&1 | head -2
done
echo DONE
