#!/usr/bin/env bash
# Real forked-V8 build, run inside WSL Ubuntu (Linux — depot_tools
# works cleanly here). No sudo: V8's gclient hooks download their own
# clang + sysroot, so use_sysroot makes the build self-contained.
# Builds in the WSL ext4 home (fast); the patch + final binary are
# read/written under /mnt/d (the repo) so the host sees them.
set -euo pipefail

PIN="${V8_REF:-465b551501481f0c7629bdd168a681c24dc60e1b}"
REPO=/mnt/d/APIClient/engine
WORK="$HOME/.v8forced"
DEPOT="$WORK/depot_tools"
V8="$WORK/v8"
OUT=out/forced
export PATH="$DEPOT:$PATH"
export DEPOT_TOOLS_UPDATE=1
export PYTHONDONTWRITEBYTECODE=1

mkdir -p "$WORK"
[ -d "$DEPOT/.git" ] || git clone --depth 1 \
  https://chromium.googlesource.com/chromium/tools/depot_tools.git "$DEPOT"

cd "$WORK"
if [ ! -d "$V8/.git" ]; then
  gclient config --spec 'solutions=[{"name":"v8","url":"https://chromium.googlesource.com/v8/v8.git","managed":False,"custom_deps":{},"deps_file":"DEPS"}]'
  gclient sync --no-history --revision "v8@${PIN}" --shallow -D
else
  ( cd "$V8" && git fetch --depth 1 origin "$PIN" && git checkout "$PIN" )
  gclient sync --no-history --revision "v8@${PIN}" --shallow -D
fi

cd "$V8"
git stash -u >/dev/null 2>&1 || true   # clean tree so patches apply
for p in "$REPO"/patches/*.patch; do
  echo "[build-wsl] git apply $p"
  git apply --whitespace=nowarn "$p"
done

gn gen "$OUT" --args='is_debug=false target_cpu="x64" v8_enable_sandbox=true is_component_build=false v8_monolithic=false v8_jitless=true use_sysroot=true is_clang=true symbol_level=1 use_remoteexec=false treat_warnings_as_errors=false'
ninja -C "$OUT" d8

mkdir -p "$REPO/.work"
cp -f "$V8/$OUT/d8" "$REPO/.work/d8-linux"
"$V8/$OUT/d8" -e 'print("D8_OK " + (1+1))'
echo "[build-wsl] DONE -> $REPO/.work/d8-linux"
