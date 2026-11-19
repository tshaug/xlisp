#!/usr/bin/env bash
# Smoke test for xlisp-repl: usage  test/smoke.sh <bin-dir>
# <bin-dir> holds xlisp-repl(.exe) and the .lsp support files (cmake --install).
set -u
bin="$1"
exe="$bin/xlisp-repl"
[ -x "$exe" ] || exe="$bin/xlisp-repl.exe"
[ -x "$exe" ] || { echo "no xlisp-repl in $bin"; exit 2; }

# XLPATH overrides the default load path (the executable's directory).
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) XLPATH="$(cd "$bin" && pwd -W)" ;;
  *)                    XLPATH="$(cd "$bin" && pwd)" ;;
esac
export XLPATH

out="$(printf '%s\n' \
  '(display (+ 1 2)) (newline)' \
  '(display (* 100000 100000)) (newline)' \
  '(car 5)' \
  '(reset)' \
  '(display "recovered") (newline)' \
  '(exit)' | "$exe" 2>&1)"
status=$?
echo "$out"

fail=0
check() { echo "$out" | grep -q -- "$1" || { echo "FAIL: missing '$1'"; fail=1; }; }
check '3'
check '10000000000'          # 64-bit fixnums, also on Win64
check 'incorrect type'       # error is reported
check 'recovered'            # (reset) returns to the top level
[ "$status" -eq 0 ] || { echo "FAIL: exit status $status"; fail=1; }
[ "$fail" -eq 0 ] && echo "smoke test OK"
exit "$fail"
