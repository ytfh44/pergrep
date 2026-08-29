#!/usr/bin/env bash
set -eu
set -o pipefail 2>/dev/null || true
PG="$1"; T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
fail(){ printf 'FAIL: %s\n' "$*" >&2; exit 1; }
eq(){ [[ "$1" == "$2" ]] || fail "$3: expected <$2>, got <$1>"; }
contains(){ [[ "$1" == *"$2"* ]] || fail "$3: missing <$2> in <$1>"; }
not_contains(){ [[ "$1" != *"$2"* ]] || fail "$3: unexpected <$2> in <$1>"; }

# Locate a real Python. On Windows the WindowsApps "python3" stub opens the
# Microsoft Store instead of running, so fall back to `python`.
PY=""
for cand in python3 python; do
  if command -v "$cand" >/dev/null 2>&1 && "$cand" --version >/dev/null 2>&1; then
    PY="$cand"; break
  fi
done
[ -n "$PY" ] || fail "no working python interpreter found"

# Git for Windows / MSYS environment: some Unix-isms (executable shell
# scripts, sed as a spawnable command) are unavailable, so those cases are
# skipped.
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;;
  *) IS_WINDOWS=0 ;;
esac

mkdir -p "$T/basic"
printf 'alpha beta\nbeta gamma\n' > "$T/basic/a.txt"
printf 'delta alpha\n' > "$T/basic/b.txt"
out="$($PG -F -n alpha "$T/basic")"
contains "$out" 'a.txt:1:alpha beta' basic-a
contains "$out" 'b.txt:1:delta alpha' basic-b

# stdin is the haystack when no path is supplied and stdin is piped.
out="$(printf 'zero\nneedle one\n' | $PG -n needle)"
eq "$out" '2:needle one' stdin-search

# -f - consumes stdin for patterns and still searches the explicit path.
out="$(printf 'alpha\n' | $PG -F -f - "$T/basic/a.txt")"
eq "$out" 'alpha beta' stdin-pattern-file

# UTF-16 BOM auto-detection and explicit UTF-16LE decoding.
"$PY" - "$T/utf16.txt" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(('hello Ωmega\n').encode('utf-16'))
PY
out="$($PG 'Ωmega' "$T/utf16.txt")"
eq "$out" 'hello Ωmega' utf16-auto
out="$($PG -E utf-16le 'Ωmega' "$T/utf16.txt")"
eq "$out" 'hello Ωmega' utf16-explicit

# Nested ignore precedence: child whitelist wins over parent ignore.
mkdir -p "$T/ign/sub" "$T/ign/.git"
printf '*.log\n' > "$T/ign/.gitignore"
printf '!keep.log\n' > "$T/ign/sub/.ignore"
printf 'needle\n' > "$T/ign/drop.log"
printf 'needle\n' > "$T/ign/sub/keep.log"
out="$($PG needle "$T/ign")"
contains "$out" 'sub/keep.log:needle' ignore-whitelist
not_contains "$out" 'drop.log' ignore-parent

# Explicit --ignore-file is active, but .ignore whitelist may override it.
printf '*.log\n' > "$T/ign/custom.ignore"
out="$($PG --ignore-file "$T/ign/custom.ignore" needle "$T/ign")"
contains "$out" 'sub/keep.log:needle' explicit-ignore-precedence

# Symlink traversal is opt-in. On Windows, Git's ln -s may fall back to a
# directory copy when symlinks are unavailable (no admin/developer mode), so
# verify a real symlink was created before asserting traversal behavior.
mkdir -p "$T/link/root" "$T/link/ext"
printf 'needle\n' > "$T/link/ext/a.txt"
ln -s ../ext "$T/link/root/alias"
if [ -L "$T/link/root/alias" ]; then
  out="$($PG needle "$T/link/root" 2>/dev/null || true)"
  eq "$out" '' symlink-default
  out="$($PG -L needle "$T/link/root")"
  contains "$out" 'alias/a.txt:needle' symlink-follow
fi

# --pre executes the user supplied transform and searches its stdout.
# On Windows the transformer must be a real .exe; shell scripts and sed are
# not spawnable, so these cases only run on Unix-like hosts.
if [ "$IS_WINDOWS" = "0" ]; then
printf 'hello\n' > "$T/pre.txt"
cat > "$T/upcase.sh" <<'SH'
#!/bin/sh
tr '[:lower:]' '[:upper:]' < "$1"
SH
chmod +x "$T/upcase.sh"
out="$($PG --pre "$T/upcase.sh" HELLO "$T/pre.txt")"
eq "$out" 'HELLO' pre
out="$($PG --pre "sed -n 1p" hello "$T/pre.txt")"
eq "$out" 'hello' pre-command-args
fi

# --search-zip searches decompressed content without delegating matching.
"$PY" - "$T/a.gz" <<'PY'
import gzip,sys
with gzip.open(sys.argv[1], 'wb') as f: f.write(b'zip needle\n')
PY
out="$($PG -z needle "$T/a.gz")"
eq "$out" 'zip needle' search-zip

# Type operations.
printf 'needle\n' > "$T/x.foo"
out="$($PG --type-add 'foo:*.foo' -tfoo needle "$T/x.foo")"
eq "$out" 'needle' type-add
list="$($PG --type-list)"
contains "$list" 'rust:' type-list-rust
contains "$list" 'python:' type-list-python

# Generate modes must emit usable, non-empty text.
man="$($PG --generate man)"; contains "$man" '.TH' generate-man
bashc="$($PG --generate complete-bash)"; contains "$bashc" 'complete' generate-bash

# CRLF/null-data last flag wins.
printf 'a\r\nb\r\n' > "$T/crlf.txt"
out="$($PG --null-data --crlf '^b$' "$T/crlf.txt")"
contains "$out" 'b' crlf-overrides-null

# Default engine is Unicode aware and rejects extended PCRE-like constructs.
printf 'Σίσυφος\nabab\n' > "$T/u.txt"
out="$($PG -i '^σίσυφος$' "$T/u.txt")"
eq "$out" 'Σίσυφος' unicode-case
if $PG '(ab)\1' "$T/u.txt" >/dev/null 2>&1; then fail default-backref-accepted; fi
out="$($PG -P '(ab)\1' "$T/u.txt")"
eq "$out" 'abab' pcre2-compat-backref

# Exit status 1 means no match, 2 means an error.
set +e
$PG absent "$T/basic/a.txt" >/dev/null; s=$?
$PG --definitely-not-a-real-flag >/dev/null 2>&1; e=$?
set -e
eq "$s" 1 no-match-exit
eq "$e" 2 error-exit

# More gitignore-style semantics: **, anchored paths, directory-only and toggles.
mkdir -p "$T/ign2/a/generated" "$T/ign2/build/sub" "$T/ign2/.git"
printf '**/generated/**\n/build/\n/rootonly\n' > "$T/ign2/.gitignore"
printf 'needle\n' > "$T/ign2/a/generated/x.txt"
printf 'needle\n' > "$T/ign2/build/sub/y.txt"
printf 'needle\n' > "$T/ign2/rootonly"
printf 'needle\n' > "$T/ign2/a/rootonly"
out="$($PG needle "$T/ign2")"
not_contains "$out" 'generated' ignore-double-star
not_contains "$out" 'build/sub' ignore-dir-only
eq "$out" 'a/rootonly:needle' ignore-anchor-scope
out="$($PG --no-ignore-vcs needle "$T/ign2")"
contains "$out" 'a/generated/x.txt:needle' no-ignore-vcs

# Hidden paths are skipped unless requested.
mkdir -p "$T/hid/.secret"; printf 'needle\n' > "$T/hid/.secret/x"
out="$($PG needle "$T/hid" || true)"; eq "$out" '' hidden-default
out="$($PG --hidden needle "$T/hid")"; contains "$out" '.secret/x:needle' hidden-enabled

# RIPGREP_CONFIG_PATH is prepended and CLI flags override it.
printf '%s\n' '--ignore-case' > "$T/rg.conf"
printf 'Needle\n' > "$T/config.txt"
out="$(RIPGREP_CONFIG_PATH="$T/rg.conf" $PG needle "$T/config.txt")"; eq "$out" 'Needle' config-applied
set +e; RIPGREP_CONFIG_PATH="$T/rg.conf" $PG --case-sensitive needle "$T/config.txt" >/dev/null; c=$?; set -e
eq "$c" 1 config-overridden

# Replacement supports numeric and named captures.
printf 'foo123bar\n' > "$T/repl.txt"
out="$($PG -r '$1' 'foo([0-9]+)bar' "$T/repl.txt")"; eq "$out" '123' replacement-numeric
out="$($PG -r '${digits}' 'foo(?<digits>[0-9]+)bar' "$T/repl.txt")"; eq "$out" '123' replacement-named

# Files, sorting and JSON are valid output modes.
files="$($PG --files --sort path "$T/basic")"
eq "$files" $'a.txt\nb.txt' files-sort
$PG --json alpha "$T/basic" > "$T/out.jsonl"
"$PY" - "$T/out.jsonl" <<'PY'
import json,sys
rows=[json.loads(x) for x in open(sys.argv[1],encoding='utf8')]
assert rows and rows[-1]['type']=='summary'
assert any(x['type']=='match' for x in rows)
PY

# Ignore source controls are independently switchable.
mkdir -p "$T/parentscope/child" "$T/exrepo/.git/info" "$T/dotscope" "$T/globrepo/.git" "$T/home/.config/git"
printf '*.tmp\n' > "$T/parentscope/.ignore"
printf 'needle\n' > "$T/parentscope/child/a.tmp"
out="$($PG needle "$T/parentscope/child" || true)"; eq "$out" '' ignore-parent-source
out="$($PG --no-ignore-parent needle "$T/parentscope/child")"; contains "$out" 'a.tmp:needle' no-ignore-parent
printf 'ignored.txt\n' > "$T/exrepo/.git/info/exclude"; printf 'needle\n' > "$T/exrepo/ignored.txt"
out="$($PG needle "$T/exrepo" || true)"; eq "$out" '' ignore-exclude-source
out="$($PG --no-ignore-exclude needle "$T/exrepo")"; contains "$out" 'ignored.txt:needle' no-ignore-exclude
printf '*.dot\n' > "$T/dotscope/.ignore"; printf 'needle\n' > "$T/dotscope/a.dot"
out="$($PG needle "$T/dotscope" || true)"; eq "$out" '' ignore-dot-source
out="$($PG --no-ignore-dot needle "$T/dotscope")"; contains "$out" 'a.dot:needle' no-ignore-dot
printf '*.mine\n' > "$T/manual.ignore"; printf 'needle\n' > "$T/dotscope/a.mine"
out="$($PG --ignore-file "$T/manual.ignore" needle "$T/dotscope" || true)"; not_contains "$out" 'a.mine' ignore-files-source
out="$($PG --ignore-file "$T/manual.ignore" --no-ignore-files needle "$T/dotscope")"; contains "$out" 'a.mine:needle' no-ignore-files
printf '*.global\n' > "$T/home/.config/git/ignore"; printf 'needle\n' > "$T/globrepo/a.global"
out="$(HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config" $PG needle "$T/globrepo" || true)"; eq "$out" '' ignore-global-source
out="$(HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config" $PG --no-ignore-global needle "$T/globrepo")"; contains "$out" 'a.global:needle' no-ignore-global

# VCS ignore files require repository detection unless --no-require-git is used.
mkdir -p "$T/nogit"
printf '*.skip\n' > "$T/nogit/.gitignore"; printf 'needle\n' > "$T/nogit/a.skip"
out="$($PG needle "$T/nogit")"; contains "$out" 'a.skip:needle' require-git-default
out="$($PG --no-require-git needle "$T/nogit" || true)"; eq "$out" '' no-require-git

# --one-file-system filters followed entries on another device, per search root.
# Linux-only: relies on /proc being a different device than the temp dir.
if [[ -r /proc/version ]] && [ "$IS_WINDOWS" = "0" ]; then
  mkdir -p "$T/fsroot"; ln -s /proc/version "$T/fsroot/proc-version"
  out="$($PG -L -F Linux "$T/fsroot" || true)"; contains "$out" 'proc-version:' one-filesystem-baseline
  out="$($PG -L --one-file-system -F Linux "$T/fsroot" || true)"; eq "$out" '' one-filesystem-filter
fi

# --null-data makes NUL the logical record terminator for anchors/output.
printf 'one\0hit\0three\0' > "$T/nulldata"
$PG --null-data '^hit$' "$T/nulldata" > "$T/nulldata.out"
"$PY" - "$T/nulldata.out" <<'PY'
import sys
b=open(sys.argv[1],'rb').read()
assert b == b'hit\0', b
PY

# --stop-on-nonmatch stops after the first non-matching record following a hit,
# and its conflict with --multiline is last-flag-wins.
printf 'hit\nhit\nno\nhit\n' > "$T/stop.txt"
out="$($PG --stop-on-nonmatch '^hit$' "$T/stop.txt")"
eq "$out" $'hit\nhit' stop-on-nonmatch
out="$($PG --multiline --stop-on-nonmatch '^hit$' "$T/stop.txt")"
eq "$out" $'hit\nhit' stop-overrides-multiline
out="$($PG --stop-on-nonmatch --multiline '^hit$' "$T/stop.txt")"
eq "$out" $'hit\nhit\nhit' multiline-overrides-stop

# Color is disabled/enabled according to the explicit mode. The exact palette is
# configurable, but --color=always must emit ANSI and --color=never must not.
printf 'needle\n' > "$T/color.txt"
$PG --color=always -n -F needle "$T/color.txt" > "$T/color.always"
"$PY" - "$T/color.always" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); assert b'\x1b[' in b, b
PY
$PG --color=never -n -F needle "$T/color.txt" > "$T/color.never"
"$PY" - "$T/color.never" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); assert b'\x1b[' not in b, b
PY

# Hyperlink output uses OSC 8 when explicitly requested and output is enabled.
$PG --color=always --hyperlink-format=file -H -F needle "$T/color.txt" > "$T/hyper.out"
"$PY" - "$T/hyper.out" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); assert b'\x1b]8;;' in b, b
PY

# --max-columns replaces an overlong matching line with an omission marker;
# --max-columns-preview prints a byte-limited preview instead.
printf 'AAAAAneedleBBBBB\n' > "$T/long.txt"
out="$($PG --max-columns 5 needle "$T/long.txt")"
contains "$out" '[Omitted long matching line]' max-columns-omitted
out="$($PG --max-columns 5 --max-columns-preview needle "$T/long.txt")"
contains "$out" 'AAAAA' max-columns-preview
not_contains "$out" 'needle' max-columns-preview-limited

# Text stats follow ripgrep's stable human-readable field names.
$PG --stats -F needle "$T/color.txt" > "$T/stats.out" 2> "$T/stats.err"
contains "$(cat "$T/stats.out")" '1 matches' stats-matches
contains "$(cat "$T/stats.out")" '1 matched lines' stats-lines
contains "$(cat "$T/stats.out")" '1 files contained matches' stats-files-match
contains "$(cat "$T/stats.out")" '1 files searched' stats-files-searched
contains "$(cat "$T/stats.out")" 'bytes printed' stats-bytes-printed
contains "$(cat "$T/stats.out")" 'bytes searched' stats-bytes-searched
contains "$(cat "$T/stats.out")" 'seconds spent searching' stats-search-time

# JSON output remains valid for arbitrary bytes by using base64-bearing objects.
"$PY" - "$T/jsonbytes.txt" <<'PY'
import sys
open(sys.argv[1],'wb').write(b'\xffneedle\n')
PY
$PG -a --json -F needle "$T/jsonbytes.txt" > "$T/jsonbytes.out"
"$PY" - "$T/jsonbytes.out" <<'PY'
import json,sys
rows=[json.loads(x) for x in open(sys.argv[1],'rb').read().splitlines()]
m=next(x for x in rows if x['type']=='match')
assert 'bytes' in m['data']['lines'], m
assert m['data']['submatches'][0]['match'].get('text') == 'needle', m
PY

# Stateful flag overrides follow ripgrep's last-flag-wins rules.
printf 'before\nhit\nafter\ntail\n' > "$T/order.txt"
out="$($PG -A1 --passthru '^hit$' "$T/order.txt")"
eq "$out" $'before\nhit\nafter\ntail' passthru-last-wins
out="$($PG --passthru -A1 '^hit$' "$T/order.txt")"
eq "$out" $'hit\nafter' context-last-wins
out="$($PG -A2 -C0 '^hit$' "$T/order.txt")"
eq "$out" $'hit\nafter\ntail' context-after-precedence
out="$($PG --column -F hit "$T/order.txt")"
eq "$out" '2:1:hit' column-implies-line-number
printf 'foo\nfoo bar\nfoobar\n' > "$T/boundary.txt"
out="$($PG -x -w foo "$T/boundary.txt")"
eq "$out" $'foo\nfoo bar' word-overrides-line
out="$($PG -w -x foo "$T/boundary.txt")"
eq "$out" 'foo' line-overrides-word

# --sort/--sortr implement metadata ordering, not just path ordering.
mkdir -p "$T/sortmeta"
printf 'needle old\n' > "$T/sortmeta/old.txt"
printf 'needle new\n' > "$T/sortmeta/new.txt"
touch -m -d '2020-01-01 UTC' "$T/sortmeta/old.txt"
touch -m -d '2021-01-01 UTC' "$T/sortmeta/new.txt"
out="$($PG --sort modified -F needle "$T/sortmeta")"
eq "$out" $'old.txt:needle old\nnew.txt:needle new' sort-modified
out="$($PG --sortr modified -F needle "$T/sortmeta")"
eq "$out" $'new.txt:needle new\nold.txt:needle old' sortr-modified
set +e
$PG --sort definitely-invalid -F needle "$T/sortmeta" >/dev/null 2>&1; ss=$?
set -e
eq "$ss" 2 invalid-sort

# Upstream ContextMode semantics: -A/-B override -C regardless of order.
printf 'before\nhit\na1\na2\n' > "$T/context-precedence.txt"
out="$($PG -A2 -C0 '^hit$' "$T/context-precedence.txt")"
eq "$out" $'hit\na1\na2' after-overrides-context-regardless-order
out="$($PG -C0 -A2 '^hit$' "$T/context-precedence.txt")"
eq "$out" $'hit\na1\na2' after-overrides-context-both-orders

# Search output modes are mutually exclusive and last search-mode flag wins.
printf 'hit\nhit\n' > "$T/modes.txt"
out="$($PG -l -c hit "$T/modes.txt")"; eq "$out" '2' count-overrides-files-with
out="$($PG -c -l hit "$T/modes.txt")"; eq "$out" 'modes.txt' files-with-overrides-count
# Once a non-search mode is selected, later search modes do not override it.
out="$($PG --files -l "$T/basic")"; contains "$out" 'a.txt' files-mode-resists-search-mode

# Standard line output highlights matched spans, not just numeric/path prefixes.
printf 'xxneedleyy\n' > "$T/color-line.txt"
$PG --color=always -F needle "$T/color-line.txt" > "$T/color-line.out"
"$PY" - "$T/color-line.out" <<'PY'
import sys
b=open(sys.argv[1],'rb').read()
assert b'xx' in b and b'yy' in b and b'\x1b[' in b
# The escape sequence must occur adjacent to the match, not only at a prefix.
assert b'xx\x1b[' in b, b
PY

# Human --stats is part of stdout and does not apply to file-list summary modes.
$PG --stats -F needle "$T/color.txt" > "$T/stats2.out" 2> "$T/stats2.err"
contains "$(cat "$T/stats2.out")" '1 matches' stats-stdout
not_contains "$(cat "$T/stats2.err")" '1 matches' stats-not-stderr
$PG --stats -l -F needle "$T/color.txt" > "$T/stats-mode.out"
not_contains "$(cat "$T/stats-mode.out")" 'matches' stats-ignored-files-with

# -o preserves zero-length matches.
printf 'abc\n' > "$T/zero.txt"
out="$($PG -n -o '^' "$T/zero.txt")"
eq "$out" '1:' zero-width-only-matching-line
out="$($PG -o '^' "$T/zero.txt")"
eq "$out" '' zero-width-only-matching-text
# --files honors sorting and path separator formatting.
mkdir -p "$T/filemode/sub"
printf x > "$T/filemode/sub/b.txt"; printf x > "$T/filemode/a.txt"
out="$($PG --files --sort path --path-separator='\' "$T/filemode")"
eq "$out" $'a.txt\nsub\\b.txt' files-sorted-path-separator

# Binary modes follow ripgrep's Auto / SearchAndSuppress / AsText split.
mkdir -p "$T/bin"
"$PY" - "$T/bin/hay" <<'PY'
import sys
open(sys.argv[1],'wb').write(b'before needle\n\x00\nafter needle\n')
PY
# Implicit recursive auto mode stops at NUL. A prior match is printed with warning.
$PG --no-mmap -n -F 'before needle' "$T/bin" > "$T/bin-implicit.out"
contains "$(cat "$T/bin-implicit.out")" 'hay:1:before needle' binary-implicit-before
contains "$(cat "$T/bin-implicit.out")" 'WARNING: stopped searching binary file after match' binary-implicit-warning
# A match after NUL is not observed in implicit auto mode.
set +e
$PG --no-mmap -n -F 'after needle' "$T/bin" > "$T/bin-after.out"; bs=$?
set -e
eq "$bs" 1 binary-implicit-after-exit
[[ ! -s "$T/bin-after.out" ]] || fail binary-implicit-after-output
# Explicit files use search-and-suppress semantics.
$PG --no-mmap -n -F 'before needle' "$T/bin/hay" > "$T/bin-explicit.out"
contains "$(cat "$T/bin-explicit.out")" '1:before needle' binary-explicit-before
contains "$(cat "$T/bin-explicit.out")" 'binary file matches' binary-explicit-message
# --binary gives implicit paths search-and-suppress semantics.
$PG --no-mmap --binary -n -F 'after needle' "$T/bin" > "$T/bin-force.out"
contains "$(cat "$T/bin-force.out")" 'hay: binary file matches' binary-force-message
# --text disables binary detection and prints ordinary matches after NUL.
out="$($PG --no-mmap --text -n -F 'after needle' "$T/bin")"
contains "$out" 'hay:3:after needle' binary-text-after

# Generated docs/completions cover the compatibility surface, not a tiny placeholder.
man="$($PG --generate man)"
contains "$man" '.SH OPTIONS' generate-man-options
contains "$man" '--one-file-system' generate-man-one-filesystem
bashc="$($PG --generate complete-bash)"
contains "$bashc" '--null-data' generate-bash-null-data
contains "$bashc" '--ignore-file' generate-bash-ignore-file
zshc="$($PG --generate complete-zsh)"
contains "$zshc" '--one-file-system' generate-zsh-one-filesystem

# --colors customizes observable ANSI styles, and hyperlink-format=none disables OSC 8.
printf 'needle\n' > "$T/palette.txt"
$PG --color=always --colors=match:none -F needle "$T/palette.txt" > "$T/palette-none.out"
"$PY" - "$T/palette-none.out" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); assert b'\x1b[' not in b, b
PY
$PG --color=always --colors=match:none --colors=match:fg:blue -F needle "$T/palette.txt" > "$T/palette-blue.out"
"$PY" - "$T/palette-blue.out" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); assert b'\x1b[0m\x1b[34mneedle\x1b[0m' in b, b
PY
$PG --color=always --colors=match:none --colors=match:bg:0x33,0x66,0xFF --colors=match:style:bold -F needle "$T/palette.txt" > "$T/palette-rgb.out"
"$PY" - "$T/palette-rgb.out" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); assert b'48;2;51;102;255' in b and b'1' in b, b
PY
$PG --color=always --hyperlink-format=none -H -F needle "$T/palette.txt" > "$T/hyper-none.out"
"$PY" - "$T/hyper-none.out" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); assert b'\x1b]8;;' not in b, b
PY

# line/column color types are independently configurable.
$PG --color=always --colors=match:none --colors=line:none --colors=line:fg:blue -n -F needle "$T/palette.txt" > "$T/palette-line.out"
"$PY" - "$T/palette-line.out" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); assert b.startswith(b'\x1b[0m\x1b[34m1\x1b[0m:'), b
PY
printf 'xxneedle\n' > "$T/palette-column.txt"
$PG --color=always --colors=match:none --colors=line:none --colors=column:none --colors=column:fg:cyan -n --column -F needle "$T/palette-column.txt" > "$T/palette-column.out"
"$PY" - "$T/palette-column.out" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); assert b'\x1b[0m\x1b[36m3\x1b[0m:' in b, b
PY

# --engine=invalid error exit code (2).
set +e
$PG --engine=invalid needle "$T/basic/a.txt" >/dev/null 2>&1; eng_status=$?
set -e
eq "$eng_status" 2 engine-invalid-exit

# Multiline --replace.
printf 'start\nfoo\nbar\nend\n' > "$T/multiline_repl.txt"
out="$($PG --passthru -U -r 'REPLACED' 'foo\nbar' "$T/multiline_repl.txt")"
eq "$out" $'start\nREPLACED\nend' multiline-replace

printf 'SECTION\nkey = value\nENDSECTION\n' > "$T/multiline_repl2.txt"
out="$($PG -U -r 'FOUND: $1' 'SECTION\n(.*)\nENDSECTION' "$T/multiline_repl2.txt")"
eq "$out" 'FOUND: key = value' multiline-replace-capture

# Non-UTF8 path and data in JSON output mode.
"$PY" - "$T/nonutf8.txt" <<'PY'
import sys
open(sys.argv[1],'wb').write(b'\xff\xfd\xfe needle \xfe\xff\n')
PY
$PG --json -a needle "$T/nonutf8.txt" > "$T/nonutf8.out"
"$PY" - "$T/nonutf8.out" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1], 'rb').read().splitlines()]
matches = [x for x in rows if x['type'] == 'match']
assert len(matches) > 0, "no match records in JSON output"
for m in matches:
    assert 'path' in m['data'], "missing path in match data"
    assert 'text' in m['data']['path'] or 'bytes' in m['data']['path'], "path must be formatted with json_data"
PY
# Multipattern inverted search (-v -e A -e B excludes lines matching either A or B).
printf 'foo\nbar\nbaz\nqux\n' > "$T/vmulti.txt"
out="$($PG -v -e foo -e bar "$T/vmulti.txt")"
eq "$out" $'baz\nqux' invert-match-multipattern

# Smart case fixed string matching.
printf 'Apple\napple\n' > "$T/smartcase.txt"
out="$($PG -F -S apple "$T/smartcase.txt")"
eq "$out" $'Apple\napple' smartcase-lowercase
out="$($PG -F -S Apple "$T/smartcase.txt")"
eq "$out" 'Apple' smartcase-uppercase

# Max-count limits matches per file.
printf 'one\ntwo\nthree\n' > "$T/maxcount.txt"
out="$($PG -m 2 -e '.*' "$T/maxcount.txt")"
eq "$out" $'one\ntwo' max-count-limit

if [ "$IS_WINDOWS" = "0" ]; then
  bad_path="$(printf "$T/nonutf8_\xff_path.txt")"
  printf 'needle\n' > "$bad_path"
  $PG --json needle "$bad_path" > "$T/badpath.out" 2>/dev/null || true
  if [ -s "$T/badpath.out" ]; then
    "$PY" - "$T/badpath.out" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1], 'rb').read().splitlines()]
matches = [x for x in rows if x['type'] == 'match']
if matches:
    assert 'bytes' in matches[0]['data']['path'] or 'text' in matches[0]['data']['path']
PY
  fi
fi

# BF-1 audit: iglob Unicode and double-star glob
mkdir -p "$T/bf1_iglob"
printf 'hello
' > "$T/bf1_iglob/café.txt"
printf 'hello
' > "$T/bf1_iglob/other.txt"
out="$($PG --iglob '*café*' hello "$T/bf1_iglob" 2>&1 | sort)"
contains "$out" 'café.txt:hello' bf1-iglob-lower
out="$($PG --iglob '*CAFÉ*' hello "$T/bf1_iglob" 2>&1 | sort)"
contains "$out" 'café.txt:hello' bf1-iglob-upper-unicode
mkdir -p "$T/bf1_q"
printf 'x
' > "$T/bf1_q/ab"
printf 'x
' > "$T/bf1_q/aé"
out="$($PG -g 'a?' x "$T/bf1_q" 2>&1 | sort)"
contains "$out" 'ab:x' bf1-glob-q-ascii
contains "$out" 'aé:x' bf1-glob-q-multibyte
mkdir -p "$T/bf1_star/a/b/c"
printf 'needle
' > "$T/bf1_star/a/b/c/file.txt"
printf 'needle
' > "$T/bf1_star/a/file.txt"
printf 'needle
' > "$T/bf1_star/file.txt"
out="$($PG -g '**/*.txt' needle "$T/bf1_star" 2>&1 | sort)"
contains "$out" 'a/b/c/file.txt:needle' bf1-double-star-deep
contains "$out" 'a/file.txt:needle' bf1-double-star-mid
contains "$out" 'file.txt:needle' bf1-double-star-zero
out="$($PG -g 'a/**/*.txt' needle "$T/bf1_star" 2>&1 | sort)"
contains "$out" 'a/b/c/file.txt:needle' bf1-double-star-prefix
# a/**/*.txt must not match top-level file.txt (only under a/). Check that no line starts with file.txt:
if printf '%s' "$out" | grep -q '^file\.txt:needle'; then fail "bf1-double-star-prefix-exact: top-level file.txt should not match a/**/*.txt, got <$out>"; fi
# \p{sc = Greek} with spaces via CLI
printf 'Ω
' > "$T/bf1_greek.txt"
out="$($PG '\p{sc = Greek}' "$T/bf1_greek.txt" 2>&1)"
contains "$out" 'Ω' bf1-regex-sc-spaced
# BF-3 multi-pattern invert/max-count/stats
# invert with multi-pattern is OR then invert: -v -e foo -e bar excludes lines matching either.
printf 'foo\nbar\nbaz\n' > "$T/bf3_invert.txt"
out="$($PG -v -e foo -e bar "$T/bf3_invert.txt")"
eq "$out" 'baz' bf3-invert-multipattern
# double-invert must be identity: -v alone vs multi-pattern invert comparison is covered by qgrep check, but we also check that -v with single pattern still inverts.
out="$($PG -v foo "$T/bf3_invert.txt")"
eq "$out" $'bar\nbaz' bf3-invert-single
# max-count per file: -m 2 with 5 matching lines should cap at 2 (per file, rg parity)
printf 'foo\nfoo\nfoo\nfoo\nfoo\n' > "$T/bf3_maxcount.txt"
out="$($PG -m 2 -e foo "$T/bf3_maxcount.txt")"
eq "$out" $'foo\nfoo' bf3-maxcount-perfile
# stats with multi-pattern should report correct matches sum and file count
printf 'foo bar\nbaz\nfoo\nbar baz\n' > "$T/bf3_stats.txt"
out="$($PG --stats -e foo -e bar "$T/bf3_stats.txt" 2>&1)"
contains "$out" '4 matches' bf3-stats-matches
contains "$out" '3 matched lines' bf3-stats-lines
contains "$out" '1 files contained matches' bf3-stats-files
# stats invert: should count non-matching lines, not positive matches
out="$($PG --stats -v -e foo -e bar "$T/bf3_stats.txt" 2>&1)"
contains "$out" '1 matches' bf3-stats-invert-matches
contains "$out" '1 matched lines' bf3-stats-invert-lines
# stats + max-count: truncated to max-count
out="$($PG --stats -m 1 -e foo "$T/bf3_maxcount.txt" 2>&1)"
contains "$out" '1 matches' bf3-stats-maxcount
contains "$out" '1 matched lines' bf3-stats-maxcount-lines
contains "$out" '1 files contained matches' bf3-stats-maxcount-files
