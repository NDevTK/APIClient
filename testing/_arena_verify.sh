#!/usr/bin/env bash
# Verify the per-flow arena on the live Chrome harness. The arena routes drive-transient
# allocations above the persistent heap so the COW revert never touches dlmalloc's allocator
# state. Primary signal: branchless_cold (fork-snapshot fixture) goes thr=2 -> 0. Guards:
# cold_transport stays thr=0 (term fix), sentry_cdn no real-bundle regression. Does NOT
# rebuild (build is run separately) -- run AFTER node engine/build.mjs cow succeeds.
set -u
cd /d/APIClient || exit 1
PORT="${PORT:-8765}"; BASE="http://localhost:${PORT}"; H="node testing/harness.js"
if ! curl -s -o /dev/null "${BASE}/sentry_cdn.html"; then node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 & sleep 2; fi
for fx in branchless_cold cold_transport sentry_cdn; do
  $H restart >/dev/null 2>&1
  $H goto "${BASE}/${fx}.html" >/dev/null 2>&1
  st=""
  for i in $(seq 1 30); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
  thr=$($H worker "var r=self._whyRecords||[],t=0;for(const x of r)if(x&&x.phase==='deep_callmain_throw')t++;return t" 2>&1 | head -1)
  sat=$($H worker "var r=self._whyRecords||[],t=0;for(const x of r)if(x&&x.phase==='cow_saturated')t++;return t" 2>&1 | head -1)
  clean=$($H offscreen "var c=0;for(const x of globalStore.endpoints.values())if(!x.resolverError)c++;return c" 2>&1 | head -1)
  printf "%-15s thr=%-4s cow_saturated=%-4s clean_eps=%-4s  (%s)\n" "$fx" "$thr" "$sat" "$clean" "$st"
done
echo DONE
