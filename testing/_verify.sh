#!/usr/bin/env bash
# No-build verification of the current build: per-fixture thr + eps (coverage).
set -u
PORT="${PORT:-8765}"; BASE="http://localhost:${PORT}"; H="node testing/harness.js"
if ! curl -s -o /dev/null "${BASE}/branchless_cold.html"; then node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 & sleep 2; fi
for fx in branchless_cold body_async_shape ce_lifecycle cold_transport react_effect chain_nested; do
  $H restart >/dev/null 2>&1
  $H goto "${BASE}/${fx}.html" >/dev/null 2>&1
  for i in $(seq 1 18); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
  thr=$($H worker "var r=self._whyRecords||[],t=0;for(const x of r)if(x&&x.phase==='deep_callmain_throw')t++;return t" 2>&1 | head -1)
  eps=$($H offscreen "var a=[];for(const x of globalStore.endpoints.values())a.push((x.method||'GET')+(x.path||x.url||'?'));return a.length+':'+JSON.stringify(a)" 2>&1 | head -1)
  printf "%-18s thr=%s eps=%s\n" "$fx" "$thr" "$eps"
done
echo DONE
