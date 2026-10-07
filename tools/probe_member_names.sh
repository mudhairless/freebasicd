#!/usr/bin/env bash
# FreeBASIC Language Server
# Copyright (C) 2026 Ebben Feagan
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Which reserved words may name a member of a TYPE/UNION/ENUM? Ask fbc.
#
#   tools/probe_member_names.sh [outdir]     # default: a mktemp dir
#
# For every word in the server's reserved-word catalog (src/language.cpp,
# kReserved) this compiles one minimal program per body kind and records whether
# fbc accepted it. The answer is data, not something to re-derive: the tables in
# src/language.cpp are generated from this script's output, and the three
# static_asserts next to them keep them sorted, disjoint, and inside the
# catalog. Re-run it after a word is added to kReserved, or after an fbc upgrade
# — the counts it prints (words probed, never field, conditional, legal enum
# name, enum-illegal) are what the tests in tests/language_checks.cpp pin, and
# the comment on the member-name tables in src/language.h quotes them. Do not
# type them into either place from memory: paste what this script prints.
#
# One-directional by construction, and it has to be: it enumerates kReserved, so
# it can only report about words the server already knows. It cannot find a word
# fbc reserves and the catalog omits — that check is a diff of the whole catalog
# against fbc's own keyword table, and src/language.h says so where the catalog
# is declared.
#
# Requires fbc (1.10.2 when the tables were generated). Exits non-zero when fbc
# is missing, so a CI job cannot silently produce an empty result, and non-zero
# when the structure or the trigger/control sets disagree (see below).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$(mktemp -d)}"
mkdir -p "$OUT"

if ! command -v fbc >/dev/null 2>&1; then
  echo "fbc not found: this probe compiles one program per reserved word" >&2
  exit 1
fi

# The catalog, straight out of the source of truth, so the probe cannot drift
# from the words the server actually treats as reserved.
mapfile -t WORDS < <(
  sed -n '/^constexpr char const \*kReserved\[\]/,/^};/p' \
    "$ROOT/src/language.cpp" |
    python3 -c "import sys,re; s=sys.stdin.read(); print('\n'.join(re.findall(r'\"([^\"]+)\"', s)))"
)

# fbc drops a bare .o next to the source it compiles, so each program gets its
# own directory and the whole lot is removed with one rm.
WORK="$OUT/work"
rm -rf "$WORK"
mkdir -p "$WORK"

# Compile one program; succeed only on a clean compile.
#
#   $1 label, $2 source text
#
# The label is printed with the verdict, so the output is a transcript that can
# be read without knowing how the programs were built.
try() {
  local label="$1" src="$2" dir="$WORK/$1"
  mkdir -p "$dir"
  printf '%s\n' "$src" >"$dir/keyword_test.bas"
  (cd "$dir" && fbc -c keyword_test.bas) >"$dir/out.txt" 2>&1
}

# A record field. `first_field` is not optional: fbc rejects an empty UDT with
# `error 256`, which would make every word look rejected for the wrong reason.
# UNION is the same grammar, so it is not probed twice — the two sets came out
# identical, which is itself worth knowing.
field_src() {
  printf 'type keyword_test\n  as integer first_field\n  as integer %s\nend type\n' "$1"
}

# An enum member: a bare name, and nothing else. `name = expr` is the only other
# legal form and it agrees with the bare one, so it is not probed twice either.
enum_src() { printf 'enum e\n  %s\nend enum\n' "$1"; }

# A record that also holds a member procedure, which is what turns the
# conditional words into fbc's `error 238`.
func_src() {
  printf 'type keyword_test\n  as integer first_field\n  as integer %s\n  declare sub go()\nend type\n' "$1"
}

# The same question asked with the other three `error 238` triggers, so the claim
# FreeBASIC.md §7 makes about all four is re-runnable rather than quoted from one
# variant. `plain` and `section` are the negative controls: a plain `Dim` field
# and an access section do NOT arm 238, and a probe that reported only the reject
# column could not tell those two apart from a trigger.
plain_src() {
  printf 'type keyword_test\n  as integer first_field\n  as integer %s\n  dim d As Integer\nend type\n' "$1"
}
static_src() {
  printf 'type keyword_test\n  as integer first_field\n  as integer %s\n  static s As Integer\nend type\n' "$1"
}
const_src() {
  printf 'type keyword_test\n  as integer first_field\n  as integer %s\n  const c = 1\nend type\n' "$1"
}
nested_src() {
  printf 'type keyword_test\n  as integer first_field\n  as integer %s\n  type inner\n    as integer q\n  end type\nend type\n' "$1"
}
section_src() {
  printf 'type keyword_test\n  as integer first_field\n  as integer %s\n  public:\nend type\n' "$1"
}

# Every variant the answer is stated over, in the order the tables read. `func` is
# the one kConditionalFieldNames is derived from; the rest confirm the other
# triggers and the two negative controls, so a change in any of them shows up as a
# disagreeing column instead of a silently stale comment.
VARIANTS=(
  "type:field_src"
  "enum:enum_src"
  "func:func_src"
  "plain:plain_src"
  "static:static_src"
  "const:const_src"
  "nested:nested_src"
  "section:section_src"
)

TSV="$OUT/member_names.tsv"
printf 'word\tkind\tverdict\tfbc\n' >"$TSV"

for w in "${WORDS[@]}"; do
  for spec in "${VARIANTS[@]}"; do
    kind="${spec%%:*}"
    fn="${spec#*:}"
    if try "$kind-$w" "$("$fn" "$w")"; then
      verdict=accept
      note=-
    else
      verdict=reject
      # The first error line, with the file/line prefix stripped: the code is
      # what distinguishes error 14 (expected identifier) from error 273
      # (expected PTR or POINTER), and both are answers about this word.
      note="$(grep -oE 'error [0-9]+: .*' "$WORK/$kind-$w/out.txt" | head -1 || true)"
    fi
    printf '%s\t%s\t%s\t%s\n' "$w" "$kind" "$verdict" "${note:--}" >>"$TSV"
  done
done

# The three shapes the tables encode, counted from the transcript so the numbers
# cannot be quoted from memory. The `func` column is the `error 238` question, so
# its rejects split into the never-legal set (which fails the name itself) and
# the conditional set (which fails only the body the name sits in).
words=$(( $(awk -F'\t' '$2=="type"' "$TSV" | wc -l) ))
never=$(awk -F'\t' '$2=="type" && $3=="reject"' "$TSV" | wc -l)
func_reject=$(awk -F'\t' '$2=="func" && $3=="reject"' "$TSV" | wc -l)
conditional=$(( func_reject - never ))
enum_illegal=$(awk -F'\t' '$2=="enum" && $3=="reject"' "$TSV" | wc -l)
enum_legal=$(( words - enum_illegal ))

cat <<SUMMARY
words probed:    $words
never field:     $never      (kNeverFieldNames)
conditional:     $conditional      (kConditionalFieldNames)
legal enum name: $enum_legal of $words      (derived, no table)
enum-illegal:    $enum_illegal      (== never + conditional)
transcript:      $TSV
SUMMARY

printf '\nThe %d that are never a field name:\n' "$never"
awk -F'\t' '$2=="type" && $3=="reject" {printf "  %-10s %s\n", $1, $4}' "$TSV"

if (( enum_illegal != never + conditional )); then
  echo "STRUCTURE DOES NOT HOLD: enum-illegal is not never + conditional." >&2
  echo "The derivation in isLegalEnumMemberName would be wrong; re-probe." >&2
  exit 1
fi

# The `error 238` claim is about the *body the name sits in*, so the three other
# triggers must refuse exactly the set a member procedure refuses, and the two
# negative controls must refuse exactly the never-legal set — a plain `Dim` field
# and an access section leave the body a plain record. Counts alone cannot tell a
# trigger from a control; this can, and it is what keeps FreeBASIC.md §7 from
# being a comment that outlives its evidence.
rejects() { awk -F'\t' -v k="$1" '$2==k && $3=="reject" {print $1}' "$TSV"; }
for trigger in func static const nested; do
  if ! diff -q <(rejects "$trigger") <(rejects func) >/dev/null; then
    echo "TRIGGER MISMATCH: a body with a $trigger does not arm error 238 for" \
      "the same words as one with a member procedure. Re-probe by hand." >&2
    exit 1
  fi
done
for control in plain section; do
  if ! diff -q <(rejects "$control") <(rejects type) >/dev/null; then
    echo "CONTROL MISMATCH: a body with $control in it refuses a different set," \
      "so it is a trigger, not a control. Re-probe by hand." >&2
    exit 1
  fi
done
printf 'triggers agree (func, static, const, nested); controls agree (plain, section)\n'

printf '\nEvery word the enum rule refuses, and what fbc said about it:\n'
awk -F'\t' '$2=="enum" && $3=="reject" {printf "  %-10s %s\n", $1, $4}' "$TSV"
