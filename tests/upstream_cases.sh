#!/usr/bin/env bash
set -euo pipefail
PG=${1:?pergrep path}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail(){ echo "FAIL: $1" >&2; exit 1; }
eq(){ [[ "$1" == "$2" ]] || { printf 'FAIL: %s\nexpected <%s>\ngot <%s>\n' "$3" "$2" "$1" >&2; exit 1; }; }
contains(){ [[ "$1" == *"$2"* ]] || fail "$3"; }

# Locate a real Python; on Windows the WindowsApps "python3" stub opens the
# Microsoft Store instead of running.
PY=""
for cand in python3 python; do
  if command -v "$cand" >/dev/null 2>&1 && "$cand" --version >/dev/null 2>&1; then
    PY="$cand"; break
  fi
done
[ -n "$PY" ] || fail "no working python interpreter found"

cat >"$T/sherlock" <<'TXT'
For the Doctor Watsons of this world, as opposed to the Sherlock
Holmeses, success in the province of detective work must always
be, to a very large extent, the result of luck. Sherlock Holmes
can extract a clew from a wisp of straw or a flake of cigar ash;
but Doctor Watson has to have it taken out for him and dusted,
TXT

# Derived from ripgrep's public integration tests (feature.rs/multiline.rs).
out="$($PG -S sherlock "$T/sherlock")"
eq "$out" $'For the Doctor Watsons of this world, as opposed to the Sherlock\nbe, to a very large extent, the result of luck. Sherlock Holmes' smart-case
out="$($PG -o --column -n Sherlock "$T/sherlock")"
eq "$out" $'1:57:Sherlock\n3:49:Sherlock' only-matching-column

# Pattern file is OR, preserving line-oriented output.
printf 'Sherlock\nHolmes\n' >"$T/pat"
out="$($PG -f "$T/pat" "$T/sherlock")"
contains "$out" 'For the Doctor Watsons' pattern-file-1
contains "$out" 'Holmeses, success' pattern-file-2

# NUL path termination in files-with-matches.
raw="$T/null.out"; $PG --null --files-with-matches Sherlock "$T/sherlock" >"$raw"
"$PY" - "$raw" <<'PY'
import sys
b=open(sys.argv[1],'rb').read()
assert b.endswith(b'\0') and b'Sherlock' not in b
PY

# Multiline: literal newline requires -U; dotall is independent.
printf 'xxx\nabc\ndefxxxabc\ndefxxx\nxxx\n' >"$T/multi"
set +e; $PG $'abc\ndef' "$T/multi" >/dev/null 2>&1; rc=$?; set -e
eq "$rc" 1 no-multiline-cross
out="$($PG -n -U $'abc\ndef' "$T/multi")"
eq "$out" $'2:abc\n3:defxxxabc\n4:defxxx' multiline-span
set +e; $PG -U 'abc.+def' "$T/multi" >/dev/null; rc=$?; set -e
eq "$rc" 1 dot-no-newline
out="$($PG -U --multiline-dotall 'abc.+def' "$T/multi")"
contains "$out" 'abc' dotall

# Vimgrep and context formatting.
out="$($PG --vimgrep Sherlock "$T/sherlock")"
eq "$out" $'sherlock:1:57:For the Doctor Watsons of this world, as opposed to the Sherlock\nsherlock:3:49:be, to a very large extent, the result of luck. Sherlock Holmes' vimgrep
out="$($PG -n -C1 'detective work' "$T/sherlock")"
eq "$out" $'1-For the Doctor Watsons of this world, as opposed to the Sherlock\n2:Holmeses, success in the province of detective work must always\n3-be, to a very large extent, the result of luck. Sherlock Holmes' context

# Count modes distinguish lines from individual matches.
printf 'x x\nx\nnone\n' >"$T/count"
out="$($PG -c x "$T/count")"; eq "$out" '2' count-lines
out="$($PG --count-matches x "$T/count")"; eq "$out" '3' count-matches
out="$($PG -m1 x "$T/count")"; eq "$out" 'x x' max-count-lines

# Invert is line complement, not file complement.
out="$($PG -v x "$T/count")"; eq "$out" 'none' invert-lines
