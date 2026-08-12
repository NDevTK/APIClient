#!/usr/bin/env bash
# COW-soundness gate: symbolize body_async's ACTUAL trap (only branchless done) +
# a fast real-bundle leaf-pool regression signal (sentry_cdn baseline 4).
set -u
cd /d/APIClient || exit 1
PORT="${PORT:-8765}"; BASE="http://localhost:${PORT}"; H="node testing/harness.js"
if ! curl -s -o /dev/null "${BASE}/body_async_shape.html"; then node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 & sleep 2; fi
$H restart >/dev/null 2>&1
$H goto "${BASE}/sentry_cdn.html" >/dev/null 2>&1
for i in $(seq 1 18); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
echo "sentry_cdn clean_eps (baseline 4): $($H offscreen "var c=0;for(const x of globalStore.endpoints.values())if(!x.resolverError)c++;return c" 2>&1 | head -1)"
$H restart >/dev/null 2>&1
$H goto "${BASE}/body_async_shape.html" >/dev/null 2>&1
for i in $(seq 1 18); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
echo "body_async thr: $($H worker "var r=self._whyRecords||[],t=0;for(const x of r)if(x&&x.phase==='deep_callmain_throw')t++;return t" 2>&1 | head -1)"
echo "body_async eps: $($H offscreen "var c=0;for(const x of globalStore.endpoints.values())if(!x.resolverError)c++;return c" 2>&1 | head -1)"
echo "=== body_async trap stacks (raw indices, top 8) ==="
$H worker "var r=self._whyRecords||[];var t=r.filter(function(x){return x&&x.phase==='deep_callmain_throw';});return JSON.stringify(t.map(function(x){return (''+x.stack).split('\n').slice(0,8).map(function(s){var m=s.match(/wasm-function\[(\d+)\]/);return m?+m[1]:null;}).filter(function(v){return v!==null;});}));" 2>&1 | head -3
echo DONE
