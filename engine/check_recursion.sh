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

UNITS="$Q/quickjs.c $Q/libregexp.c $Q/libunicode.c $Q/dtoa.c
$H/solver/cow.c $H/solver/engine.c $H/solver/flow.c $H/solver/decide.c
$H/solver/concolic.c $H/solver/endpoint.c $H/solver/solve.c
$H/solver/dom_cow.c $H/solver/attr_shadow.c
$H/browser/core/loader/document_scripts.c $H/test_forced.c"

# WHERE LEXBOR IS, asked of the tree rather than written down twice. This path was hardcoded to a location the
# build stopped using, so every compile failed on a missing header and the audit had been UNRUNNABLE — which is
# the exact failure its own header warns about: a checker that silently covers a fraction of the program is
# worse than none, because its zero is believed. It had stopped covering all of it.
LEXBOR="$ROOT/engine/.work/lexbor-src/source"
if [ ! -d "$LEXBOR" ]; then
  echo "check_recursion: lexbor headers not found at $LEXBOR — run 'node engine/build.mjs lexbor' first" >&2
  exit 1
fi

n=0
LLS=""
for f in $UNITS; do
  b=$(basename "$f" .c)
  clang -O0 -w -S -emit-llvm -DNDEBUG -D_GNU_SOURCE -DCONFIG_VERSION='"t262"' -DAPICLIENT_DEV=1 \
        -I"$Q" -I"$H" -I"$H/browser" -I"$LEXBOR" "$f" -o "$OUT/$b.ll"
  LLS="$LLS $OUT/$b.ll"
  n=$((n + 1))
done
llvm-link -S $LLS -o "$OUT/all.ll"
exec node "$ROOT/engine/check_recursion.mjs" "$OUT/all.ll" --units "$n" "$@"
