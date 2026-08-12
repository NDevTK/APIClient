#!/usr/bin/env bash
# WASM_NAMES build of the leaf-in-pool fix, then dump branchless residual trap
# stacks as raw wasm-function indices (symbolize separately via _wasmnames.cjs).
set -u
PORT="${PORT:-8765}"; BASE="http://localhost:${PORT}"; H="node testing/harness.js"
if ! curl -s -o /dev/null "${BASE}/branchless_cold.html"; then node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 & sleep 2; fi
WASM_NAMES=1 node engine/build.mjs cow > engine/.work/build-names2.log 2>&1
echo "BUILD=$? cerr=$(grep -ciE 'error:' engine/.work/build-names2.log)"
$H restart >/dev/null 2>&1
$H goto "${BASE}/branchless_cold.html" >/dev/null 2>&1
for i in $(seq 1 18); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
echo "=== thr/eps ==="
$H worker "var r=self._whyRecords||[],t=0;for(const x of r)if(x&&x.phase==='deep_callmain_throw')t++;return 'thr='+t" 2>&1 | head -2
$H offscreen "var a=[];for(const x of globalStore.endpoints.values())a.push((x.method||'GET')+(x.path||x.url||'?'));return a.length+':'+JSON.stringify(a)" 2>&1 | head -2
echo "=== residual trap stacks (raw indices, top 6 frames each) ==="
$H worker "var r=self._whyRecords||[];var t=r.filter(function(x){return x&&x.phase==='deep_callmain_throw';});return JSON.stringify(t.map(function(x){return {err:x.err,frames:(''+x.stack).split('\n').slice(0,7).map(function(s){var m=s.match(/wasm-function\[(\d+)\]/);return m?+m[1]:null;}).filter(function(v){return v!==null;})};}));" 2>&1 | head -8
echo DONE
