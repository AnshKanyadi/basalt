#!/usr/bin/env sh
# Vendored-tree integrity, entirely offline.
#
# Recomputes the git tree hash of the vendored GoogleTest directory and requires
# it to equal the hash recorded in that directory's VERSION file.
#
# A PIN NOTHING VERIFIES IS A COMMENT. third_party/googletest/VERSION has
# recorded a tag, a commit and a tree hash since the tree was vendored, and
# until this script existed nothing ever compared them to the bytes on disk. A
# local edit to a vendored dependency -- a debug printf, a patched header, a
# botched merge -- would have survived every lane and surfaced weeks later as a
# build that behaves differently here than anywhere else.
#
# THIS CHECK MAKES NO NETWORK CALL AND MUST NEVER ACQUIRE ONE. It answers "is
# this tree the tree we recorded", which is a question about local consistency.
# The provenance question it does NOT answer -- are these bytes the bytes
# upstream published -- needs the network, is answered once when a dependency is
# vendored or re-pinned, and is deliberately not a lane: a lane that fetches
# fails in exactly the situation where "it builds from a clean clone" matters.
#
# The recomputation is isolated from the developer's git configuration
# (GIT_CONFIG_GLOBAL and GIT_CONFIG_SYSTEM are neutralized) because a global
# core.autocrlf or a global excludes file would otherwise change the answer, and
# a check whose result depends on who is running it is not a check.
#
# usage: vendor-check.sh [vendored-dir]
#   The directory argument exists so an induced failure can run this exact code
#   against a scratch copy. Default: third_party/googletest.
set -eu

dir=${1:-third_party/googletest}
meta=$dir/VERSION

if [ ! -d "$dir" ]; then
  printf '\n  FAIL  vendored directory missing: %s\n\n' "$dir"
  exit 1
fi
if [ ! -f "$meta" ]; then
  printf '\n  FAIL  provenance record missing: %s\n\n' "$meta"
  exit 1
fi

recorded=$(sed -n 's/^tree: *//p' "$meta" | head -1)
commit=$(sed -n 's/^commit: *//p' "$meta" | head -1)
tag=$(sed -n 's/^tag: *//p' "$meta" | head -1)
excluded=$(sed -n 's/^exclude-from-hash: *//p' "$meta" | head -1)

if [ -z "$recorded" ]; then
  printf '\n  FAIL  %s records no "tree:" hash to check against.\n\n' "$meta"
  exit 1
fi
# THE EXCLUSION SET IS ONE HARD-CODED NAME ON PURPOSE. Widening it is how a
# vendored tree stops being the upstream tree quietly: every name added is a
# file the equality no longer covers.
if [ "$excluded" != "VERSION" ]; then
  printf '\n  FAIL  %s: exclude-from-hash must be exactly "VERSION", got "%s".\n\n' "$meta" "$excluded"
  exit 1
fi

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT INT TERM

GIT_CONFIG_GLOBAL=/dev/null
GIT_CONFIG_SYSTEM=/dev/null
export GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM

git init -q "$scratch/probe"
tar cf - -C "$dir" . | (cd "$scratch/probe" && tar xf -)
rm -f "$scratch/probe/VERSION"

# -f because the vendored tree carries its own .gitignore, and an ignored file
# that upstream tracks must still be hashed or the equality is against a subset.
( cd "$scratch/probe" && git add -A -f . >/dev/null )
computed=$( cd "$scratch/probe" && git write-tree )
files=$( cd "$scratch/probe" && git ls-files | wc -l | tr -d ' ' )

printf '\n  vendored-tree integrity (offline)\n'
printf '  ----------------------------------------------------------\n'
printf '   dir      : %s\n' "$dir"
printf '   tag      : %s\n' "$tag"
printf '   commit   : %s\n' "$commit"
printf '   files    : %s (excluding VERSION)\n' "$files"
printf '   recorded : %s\n' "$recorded"
printf '   computed : %s\n' "$computed"

if [ "$computed" != "$recorded" ]; then
  printf '  ----------------------------------------------------------\n'
  printf '   FAIL  the vendored tree is not the tree that was recorded.\n\n'
  printf '  Something under %s changed since vendoring.\n' "$dir"
  printf '  DO NOT UPDATE THE RECORDED HASH TO MATCH THE TREE. The hash is the\n'
  printf '  claim; the tree is what is being checked against it. Restore the tree,\n'
  printf '  or re-vendor deliberately and record the new commit as a new pin.\n\n'
  exit 1
fi

printf '  ----------------------------------------------------------\n'
printf '   ok  tree matches its recorded hash\n\n'
