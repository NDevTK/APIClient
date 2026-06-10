#!/usr/bin/env bash
# moat_survey.sh — REAL-BUNDLE netdiff regression oracle.
#
# WHY THIS EXISTS (acting on a design critique): the synthetic fixtures all PASS
# while real minified bundles fail on emergent complexity — so synthetic
# fixtures give false confidence. This drives the REAL CDN bundles through the
# REAL harness + netdiff and flags drift vs a baseline, making real-bundle
# coverage a first-class, repeatable check (not a one-off manual survey).
#
# Usage:  bash testing/moat_survey.sh            # uses :8765 (fixtures_server.cjs must be up)
#         PORT=8000 bash testing/moat_survey.sh  # via the python server
# Exit 0 if no REGRESSION (clean < baseline on a non-gap fixture); 1 otherwise.
set -u
PORT="${PORT:-8765}"
BASE="http://localhost:${PORT}"
H="node testing/harness.js"

# fixture | baseline_clean | kind(direct|gap) | note
FIXTURES=(
  "sdk_supabase|23|direct|REST backend"
  "sdk_pocketbase|30|direct|REST backend, full admin surface logged-out"
  "sdk_firebase|13|direct|firebase modular+compat auth"
  "sentry_cdn|4|direct|envelope POST /api/0/envelope/"
  "esm_cdn_main2|1|direct|ESM multi-import transitive (needs :8765 deps)"
  "prune_helper_gate|2|direct|value-spread cold helper-picker"
  "sdk_appwrite|5|gap|shared client.call helper host-retry not reached"
  "sdk_algolia|0|gap|transporter -> pluggable requester host-retry"
  "sdk_directus|3|gap|composable; await-fix got auth, content still 0"
)

printf "%-22s %6s %6s %7s %8s %7s  %s\n" "FIXTURE" "CLEAN" "RE" "UNUSED" "RECV" "STATUS" "NOTE"
printf '%.0s-' {1..98}; echo
regressions=0
for row in "${FIXTURES[@]}"; do
  IFS='|' read -r fx base kind note <<< "$row"
  $H restart >/dev/null 2>&1
  $H goto "${BASE}/${fx}.html" >/dev/null 2>&1
  st=""
  for i in $(seq 1 16); do
    st=$($H learnstate 2>&1 | grep -oE '"state": *"[^"]*"')
    case "$st" in *complete*|*stalled*) break;; esac
    sleep 3
  done
  clean=$($H offscreen "var c=0;for(const x of globalStore.endpoints.values())if(!x.resolverError)c++;return c" 2>&1 | head -1)
  re=$($H offscreen "var c=0;for(const x of globalStore.endpoints.values())if(x.resolverError)c++;return c" 2>&1 | head -1)
  unused=$($H netdiff --unused 2>&1 | grep -oE '"unusedCount": *[0-9]+' | grep -oE '[0-9]+' | head -1)
  unused="${unused:-?}"
  # receiver-coverage frontier (gsRecv/gsDrv off the drive-trace): of the async
  # __awaiter/generator API methods driven, how many resolved a CONCRETE receiver vs
  # opaque `this`. A low ratio = a cold/logged-in API surface stuck opaque (the moat gap).
  recv=$($H offscreen "var v=[..._lastGrindStatsByDoc.values()].sort(function(a,b){return (b.ts||0)-(a.ts||0)})[0]; return v?(v.gsRecv+'/'+v.gsDrv):'-'" 2>&1 | head -1 | tr -d '"')
  recv="${recv:--}"
  # status
  status="ok"
  if ! [[ "$clean" =~ ^[0-9]+$ ]]; then status="ERR"; clean="${clean:0:5}"; fi
  if [[ "$status" == "ok" ]]; then
    if [[ "$kind" == "gap" ]]; then
      status="GAP"
      [[ "$clean" -gt "$base" ]] && status="GAP+"   # improved past the known gap baseline = good news
    elif [[ "$clean" -lt "$base" ]]; then
      status="REGRESS"; regressions=$((regressions+1))
    fi
  fi
  printf "%-22s %6s %6s %7s %8s %7s  %s\n" "$fx" "$clean" "$re" "$unused" "$recv" "$status" "$note"
done
printf '%.0s-' {1..90}; echo
if [[ "$regressions" -gt 0 ]]; then echo "RESULT: $regressions REGRESSION(s) — a direct-fetch fixture dropped below baseline"; exit 1; fi
echo "RESULT: no regressions. GAP rows are the known real-bundle shared-helper/composable gaps (see memory)."
