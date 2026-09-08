#!/usr/bin/env bash
set -eu
set -o pipefail 2>/dev/null || true
PG=${1:?pergrep path}
# Every ripgrep 15.2.0 logical long flag (plus documented negations/legacy aliases)
# must be accepted by the parser. --help makes this a side-effect-free parse test.
switches=(
  --binary --no-binary --block-buffered --no-block-buffered --byte-offset --no-byte-offset
  --case-sensitive --column --no-column --count --count-matches --crlf --no-crlf --debug
  --files --files-with-matches --files-without-match --fixed-strings --no-fixed-strings
  --follow --no-follow --glob-case-insensitive --no-glob-case-insensitive --heading --no-heading
  --hidden --no-hidden --ignore-case --no-ignore-case --ignore-file-case-insensitive
  --no-ignore-file-case-insensitive --include-zero --no-include-zero --invert-match --no-invert-match
  --json --no-json --line-buffered --no-line-buffered --line-number --no-line-number --line-regexp
  --no-line-regexp --max-columns-preview --no-max-columns-preview --mmap --no-mmap --multiline
  --no-multiline --multiline-dotall --no-multiline-dotall --no-config --no-ignore --ignore
  --no-ignore-dot --ignore-dot --no-ignore-exclude --ignore-exclude --no-ignore-files --ignore-files
  --no-ignore-global --ignore-global --no-ignore-messages --ignore-messages --no-ignore-parent
  --ignore-parent --no-ignore-vcs --ignore-vcs --no-messages --messages --no-require-git --require-git
  --no-unicode --unicode --null --no-null --null-data --no-null-data --one-file-system
  --no-one-file-system --only-matching --no-only-matching --passthru --no-passthru --pcre2
  --no-pcre2 --pcre2-version --pretty --quiet --no-quiet --search-zip --smart-case --no-smart-case
  --stats --no-stats --stop-on-nonmatch --no-stop-on-nonmatch --text --no-text --trace --trim
  --no-trim --type-list --unrestricted --version --vimgrep --with-filename --no-filename
  --word-regexp --no-word-regexp --auto-hybrid-regex --no-auto-hybrid-regex --no-pcre2-unicode
  --pcre2-unicode --sort-files --no-sort-files
)
for f in "${switches[@]}"; do "$PG" --help "$f" >/dev/null; done

# Value-taking flags use syntactically valid inert values.
declare -a vals=(
  '--after-context=1' '--before-context=1' '--context=1' '--color=never' '--colors=match:none'
  '--context-separator=--' '--dfa-size-limit=10M' '--encoding=utf-8' '--engine=default'
  '--field-context-separator=-' '--field-match-separator=:' '--generate=man' '--glob=*.cpp'
  '--hostname-bin=hostname' '--hyperlink-format=none' '--iglob=*.CPP' '--ignore-file=/dev/null'
  '--max-columns=100' '--max-count=1' '--max-depth=1' '--max-filesize=1M' '--path-separator=/'
  '--pre=cat' '--pre-glob=*.txt' '--regex-size-limit=10M' '--regexp=x' '--file=/dev/null'
  '--replace=x' '--sort=path' '--sortr=path' '--threads=1' '--type=rust' '--type-not=rust'
  '--type-add=foo:*.foo' '--type-clear=foo'
)
for f in "${vals[@]}"; do "$PG" --help "$f" >/dev/null; done

# Short options, with values where required.
shorts=(-a -b -c -F -h -H -I -i -l -L -n -N -o -p -P -q -s -S -u -U -v -V -w -x -z -0)
for f in "${shorts[@]}"; do "$PG" --help "$f" >/dev/null; done
"$PG" --help -A1 -B1 -C1 -d1 -Eutf-8 -ex -f/dev/null '-g*.cpp' -j1 -m1 -M100 -rx -trust -Trust >/dev/null
