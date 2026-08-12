#!/usr/bin/env bash
# scale_survey.sh — TEST TYPE #2: productivity AT SCALE (the prioritisation requirement).
#
# moat_survey.sh tests each library in ISOLATION (restart between fixtures). This tests the
# OTHER axis the user named: "loading lots of libraries (or multiple websites) and reviewing
# analysis stays productive". Here we load MANY libraries back-to-back WITHOUT restarting, so
# each new library is analyzed WHILE the earlier ones' background deep-grinds are still running
# (they resume from feDeepDB after navigation). The scheduler (priority.js flowCmp) must keep
# the LIVE/newest work productive — a late library must NOT be STARVED behind the accumulating
# backlog. If it adds 0 endpoints under load, prioritisation has degraded into FIFO and the
# moat stops being useful at the scale a researcher actually works at.
#
# Usage:  PORT=8765 bash testing/scale_survey.sh        (WAIT=secs bounds the per-lib wait —
#         deliberately NOT full completion, so grinds OVERLAP and the backlog builds)
# Exit 0 if every library stayed productive; 1 if any was starved.
set -u
PORT="${PORT:-8765}"; BASE="http://localhost:${PORT}"; H="node testing/harness.js"
WAIT="${WAIT:-12}"
CLEAN='var c=0;for(const x of globalStore.endpoints.values())if(!x.resolverError)c++;return c'
# Distinct-endpoint SDKs; the big one (appwrite) sits LATE so it must stay productive under load.
FIX=(sdk_supabase sdk_pocketbase sdk_firebase sdk_appwrite sentry_cdn sdk_directus)

$H restart >/dev/null 2>&1
prev=0; starved=0
printf "%-3s %-16s %7s %8s  %s\n" "#" "LIBRARY" "CUMUL" "+NEW" "PRODUCTIVE?"
printf '%.0s-' {1..58}; echo
for i in "${!FIX[@]}"; do
  fx="${FIX[$i]}"
  $H goto "${BASE}/${fx}.html" >/dev/null 2>&1   # navigate WITHOUT waiting for the prior grind
  sleep "$WAIT"
  cum=$($H offscreen "$CLEAN" 2>&1 | head -1)
  [[ "$cum" =~ ^[0-9]+$ ]] || cum=$prev
  delta=$((cum - prev))
  prod="ok"; if [[ "$delta" -le 0 ]]; then prod="STARVED"; starved=$((starved+1)); fi
  printf "%-3s %-16s %7s %8s  %s\n" "$((i+1))" "$fx" "$cum" "+$delta" "$prod"
  prev=$cum
done
printf '%.0s-' {1..58}; echo
if [[ "$starved" -gt 0 ]]; then
  echo "RESULT: $starved library(ies) STARVED (added 0 under accumulating load) — productivity degraded at scale (FIFO, not prioritised)"; exit 1
fi
echo "RESULT: all $((${#FIX[@]})) libraries stayed productive under accumulating background load — cumulative cross-site moat = $prev endpoints (each new library analyzed despite the backlog)."
