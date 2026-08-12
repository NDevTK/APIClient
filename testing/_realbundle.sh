#!/usr/bin/env bash
# Decide term-arena value by MEASUREMENT: do real CDN bundles trap (qjs_term
# hazard) AND lose @H? Baselines (moat_survey): sentry_cdn=4, sdk_supabase=27,
# sdk_directus=42(gap). thr>0 + clean<baseline => the hazard loses real @H.
set -u
PORT="${PORT:-8765}"; BASE="http://localhost:${PORT}"; H="node testing/harness.js"
if ! curl -s -o /dev/null "${BASE}/sentry_cdn.html"; then node testing/fixtures_server.cjs > testing/_fixsrv.out 2>&1 & sleep 2; fi
# harness health
echo "health: $($H restart 2>&1 | head -1)"
for fx in sentry_cdn sdk_supabase sdk_directus sdk_pocketbase; do
  $H restart >/dev/null 2>&1
  $H goto "${BASE}/${fx}.html" >/dev/null 2>&1
  for i in $(seq 1 20); do st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"'); case "$st" in *complete*|*stalled*) break;; esac; sleep 3; done
  thr=$($H worker "var r=self._whyRecords||[],t=0;for(const x of r)if(x&&x.phase==='deep_callmain_throw')t++;return t" 2>&1 | head -1)
  rec=$($H worker "var r=self._whyRecords||[],t=0;for(const x of r)if(x&&x.phase==='deep_recycle')t++;return t" 2>&1 | head -1)
  clean=$($H offscreen "var c=0;for(const x of globalStore.endpoints.values())if(!x.resolverError)c++;return c" 2>&1 | head -1)
  printf "%-16s thr=%s rec=%s clean_eps=%s\n" "$fx" "$thr" "$rec" "$clean"
done
echo DONE
