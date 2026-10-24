#!/bin/bash
# Provision a fresh container for this repository: the toolchains a build needs and what a live run needs.
# The cloud environment's setup script runs it as root; every step checks its own result first, so running it
# again changes nothing. A step that cannot be met exits nonzero and names itself.
#
#   bash engine/provision.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# The emscripten release the shipped wasm is built with. github.com is not reachable from this environment, so
# emsdk comes from Chromium's mirror of its repository; its installer downloads from storage.googleapis.com.
EMSDK_VERSION=6.0.13
EMSDK_MIRROR=https://chromium.googlesource.com/external/github.com/emscripten-core/emsdk
EMSDK="$ROOT/engine/.work/emsdk"

step() { echo "[provision] $*"; }

# 1. Node packages, exactly as the lockfile states (the gates parse with @babel/parser).
if [ ! -d node_modules/@babel/parser ]; then
  step "npm ci"
  npm ci --no-audit --no-fund
fi

# 2. System packages: ICU for the native Intl host, certutil for the browser's trust store, unzip for Chrome.
need=()
dpkg -s libicu-dev >/dev/null 2>&1 || need+=(libicu-dev)
command -v certutil >/dev/null 2>&1 || need+=(libnss3-tools)
command -v unzip >/dev/null 2>&1 || need+=(unzip)
if [ ${#need[@]} -gt 0 ]; then
  step "apt-get install ${need[*]}"
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${need[@]}"
fi

# 3. emsdk, pinned, so every container builds the wasm with the same compiler.
if [ ! -x "$EMSDK/emsdk" ]; then
  step "clone emsdk from $EMSDK_MIRROR"
  mkdir -p "$(dirname "$EMSDK")"
  git clone --depth 1 "$EMSDK_MIRROR" "$EMSDK"
fi
if ! grep -qx "\"$EMSDK_VERSION\"" "$EMSDK/upstream/emscripten/emscripten-version.txt" 2>/dev/null; then
  step "emsdk install $EMSDK_VERSION"
  "$EMSDK/emsdk" install "$EMSDK_VERSION"
  "$EMSDK/emsdk" activate "$EMSDK_VERSION"
fi

# 4. Chrome for Testing at the harness's version, the unprivileged account its sandbox runs under, and the
#    agent proxy's CA in the store that browser reads.
bash testing/chrome_for_testing_setup.sh
id chromerun >/dev/null 2>&1 || { step "useradd chromerun"; useradd -m chromerun; }
bash testing/browser_ca_setup.sh

# 5. test262, the corpus the language gates read, at the commit the submodule pins, from Chromium's mirror.
if [ -z "$(ls -A engine/qjs/test262 2>/dev/null)" ]; then
  step "test262 submodule"
  git -c url."https://chromium.googlesource.com/external/github.com/tc39/test262".insteadOf=https://github.com/tc39/test262 \
      -c submodule."engine/qjs/test262".update=checkout submodule update --init --depth 1 engine/qjs/test262
fi

# 6. This checkout: the tracked pre-push hook engine/publish.mjs requires, and lane worktrees kept out of status.
git config core.hooksPath "$ROOT/engine/githooks"
grep -qxF '.claude/worktrees/' .git/info/exclude 2>/dev/null || echo '.claude/worktrees/' >> .git/info/exclude

step "done: emcc $("$EMSDK/upstream/emscripten/emcc" --version 2>/dev/null | head -1 | sed 's/^.*) //'), node $(node --version)"
