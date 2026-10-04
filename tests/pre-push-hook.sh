#!/bin/sh
# Pre-push guard: the version identity of exactly what is being published must
# hold. Catches the class of bug where the exe's version resource and the number
# the code compares against the feed drift apart - a wrong number still compiles,
# so nothing else fails.
#
# Tracked in the repository since 1ac3908 (a clone gets this file); install with:
#   cp tests/pre-push-hook.sh .git/hooks/pre-push && chmod +x .git/hooks/pre-push
#
# Bypass deliberately with `git push --no-verify` if you know why.

root=$(git rev-parse --show-toplevel)
guard="$root/tests/release_guard.py"
[ -f "$guard" ] || exit 0          # nothing to check in a clone without the internal tests
if ! command -v python3 >/dev/null 2>&1; then
    echo "release guard: NOTE python3 not found - the version guard is DISABLED for this push"
    exit 0
fi

fail=0
# local_ref is the git pre-push protocol field; unused on purpose.
# shellcheck disable=SC2034
while read -r local_ref local_sha remote_ref remote_sha; do
    case "$local_sha" in 0000000000000000000000000000000000000000) continue ;; esac
    echo "release guard: $remote_ref <- ${local_sha%"${local_sha#??????}"}"
    # Strict feed rules fire ONLY when the push's diff carries version.txt:
    # a version.h bump without the matching version.txt (or a pin typo) fails
    # HERE instead of publishing a feed the exe will refuse. check_feed is
    # local-only, so this is safe before the release exists. Two known edges,
    # stated so nobody expects more of this hook than it does (2026-10-04
    # review): (1) a push that DELETES version.txt, or a tree with no
    # version.txt at all, is a NOTE in the guard, not a failure - the
    # bump-then-feed two-commit flow makes stricter keying wrong; (2) on a
    # NEW branch or tag the remote sha is all zeros, so the diff below is
    # fatal and strict mode would silently degrade - hence the explicit
    # zero-sha branch: a fresh ref gets the full-tree version.txt test.
    # The published-asset and deployed-exe halves still cannot run until
    # after `gh release create` - they stay manual, and the note below says so.
    strict=""
    if [ "$remote_sha" = "0000000000000000000000000000000000000000" ]; then
        # new branch or tag: no remote side to diff against; test the tree
        if git ls-tree -r --name-only "$local_sha" | grep -q "^version.txt$"; then
            strict="--release"
            echo "release guard: new ref carries version.txt - strict feed rules apply"
            echo "release guard: NOTE after publishing, still run: release_guard.py --verify-asset and --deployed <exe>"
        fi
    else
        # one diff per push: the output answers "carries version.txt", the
        # exit status answers "diffable at all" (2026-10-05 review)
        diff_files=$(git diff --name-only "$remote_sha..$local_sha" 2>/dev/null)
        diff_ok=$?
        if printf '%s\n' "$diff_files" | grep -q "^version.txt$"; then
            strict="--release"
            echo "release guard: push carries version.txt - strict feed rules apply"
            echo "release guard: NOTE after publishing, still run: release_guard.py --verify-asset and --deployed <exe>"
        elif [ "$diff_ok" -ne 0 ]; then
            echo "release guard: NOTE could not diff $remote_sha..$local_sha; strict feed rules skipped"
        fi
    fi
    python3 "$guard" --commit "$local_sha" $strict || fail=1
done
exit $fail
