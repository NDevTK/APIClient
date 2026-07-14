#!/usr/bin/env bash
# Verify the qjs_opq REVERT isolates the sentry regressor: sentry should return to
# ~7 (qjs_opq was the culprit), cold_transport stays 0 (term fix kept), branchless 2.
set -u
cd /d/APIClient || exit 1
PORT="${PORT:-8765}"; BASE="http://localhost:${PORT}"; H="node testing/harness.js"
if ! curl -s -o /dev/null "${BASE}/sentry_cdn.html"; then node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 & sleep 2; fi
node engine/build.mjs cow > engine/.work/build-revert.log 2>&1
echo "BUILD=$? cerr=$(grep -ciE 'error:' engine/.work/build-revert.log)"
for fx in sentry_cdn cold_transport branchless_cold; do
  $H restart >/dev/null 2>&1
  $H goto "${BASE}/${fx}.html" >/dev/null 2>&1
  for i in $(seq 1 18); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
  thr=$($H worker "var r=self._whyRecords||[],t=0;for(const x of r)if(x&&x.phase==='deep_callmain_throw')t++;return t" 2>&1 | head -1)
  clean=$($H offscreen "var c=0;for(const x of globalStore.endpoints.values())if(!x.resolverError)c++;return c" 2>&1 | head -1)
  printf "%-14s thr=%s clean_eps=%s\n" "$fx" "$thr" "$clean"
done
echo DONE
