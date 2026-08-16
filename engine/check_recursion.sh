#!/bin/sh
# Build the WHOLE program to one LLVM module and check it for C recursion.
#
# The unit list mirrors engine/build.mjs. A unit that does not COMPILE is a hard error here, not a skip: the
# checker's number is only worth anything over the whole program, and the previous version silently covered
# quickjs.c alone — which hid libregexp's recursive-descent pattern parser, among five other cycles.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
Q="$ROOT/engine/qjs"
H="$ROOT/engine/host"
# The first argument is the IR directory ONLY when it is not a flag; every flag is passed through to the
# checker. Reading a `--list-blob` as a directory name is how this script tried to mkdir it.
case "${1:-}" in
  -*|"") OUT="$ROOT/engine/.recursion-ir" ;;
  *)     OUT="$1"; shift ;;
esac
mkdir -p "$OUT"

# THE UNIT LIST IS ASKED FOR, NOT COPIED. This was a hand-written list that said it mirrored engine/build.mjs
# and had drifted to FIFTEEN of the program's forty-four: every browser component — the DOM tree walks, the HTML
# serialiser, custom elements, fetch, Headers — was outside the check, so a clean report was a report about
# quickjs and the solver and nothing else. That is exactly the failure this file's own header warns about, in
# this file, about this list. There is one list now and it lives where the program is defined.
UNITS=$(node "$ROOT/engine/build.mjs" --list-sources)
if [ -z "$UNITS" ]; then
  echo "check_recursion: engine/build.mjs --list-sources answered nothing — the unit list is the program" >&2
  exit 1
fi

# WHERE LEXBOR IS, asked of the tree rather than written down twice. This path was hardcoded to a location the
# build stopped using, so every compile failed on a missing header and the audit had been UNRUNNABLE — which is
# the exact failure its own header warns about: a checker that silently covers a fraction of the program is
# worse than none, because its zero is believed. It had stopped covering all of it.
LEXBOR="$ROOT/engine/.work/lexbor-src/source"
if [ ! -d "$LEXBOR" ]; then
  echo "check_recursion: lexbor headers not found at $LEXBOR — run 'node engine/build.mjs lexbor' first" >&2
  exit 1
fi

# THE IR FILE IS NAMED AFTER THE UNIT'S PATH, NOT ITS BASENAME, and that is a correctness fix rather than
# tidiness. Two units in this program are called main.c — engine/host/main.c is the renderer's ABI entry and
# engine/host/browser_process/main.c is the browser process's — so under `basename` the second compile
# OVERWROTE the first's .ll, `llvm-link` was handed the same file twice, and `n` still counted two. The
# coverage check compares that count against build.mjs's list, so it would have PASSED over a module missing a
# translation unit: exactly the "silently covers a fraction of the program, and its zero is believed" failure
# this file's own header warns about, arriving through the filename.
# -I$H/browser_process is the browser process's own root, the way -I$H/browser is the renderer's: its
# components include each other as "network/corb.h".
n=0
LLS=""
for f in $UNITS; do
  b=$(printf '%s' "${f#$ROOT/}" | tr '/.' '__')
  clang -O0 -w -S -emit-llvm -DNDEBUG -D_GNU_SOURCE -DCONFIG_VERSION='"t262"' -DAPICLIENT_DEV=1 \
        -I"$Q" -I"$H" -I"$H/browser" -I"$H/browser_process" -I"$LEXBOR" "$f" -o "$OUT/$b.ll"
  LLS="$LLS $OUT/$b.ll"
  n=$((n + 1))
done
llvm-link -S $LLS -o "$OUT/all.ll"
exec node "$ROOT/engine/check_recursion.mjs" "$OUT/all.ll" --units "$n" "$@"
