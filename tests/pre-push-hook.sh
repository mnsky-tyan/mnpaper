#!/bin/sh
# Pre-push guard: the version identity of exactly what is being published must
# hold. Catches the class of bug where the exe's version resource and the number
# the code compares against the feed drift apart - a wrong number still compiles,
# so nothing else fails.
#
# Not part of the repository (hooks are never cloned or pushed); install with:
#   cp tests/pre-push-hook.sh .git/hooks/pre-push && chmod +x .git/hooks/pre-push
#
# Bypass deliberately with `git push --no-verify` if you know why.

root=$(git rev-parse --show-toplevel)
guard="$root/tests/release_guard.py"
[ -f "$guard" ] || exit 0          # nothing to check in a clone without the internal tests
command -v python3 >/dev/null 2>&1 || exit 0

fail=0
while read -r local_ref local_sha remote_ref remote_sha; do
    case "$local_sha" in 0000000000000000000000000000000000000000) continue ;; esac
    echo "release guard: $remote_ref <- ${local_sha%"${local_sha#??????}"}"
    # A push that carries version.txt is a release push: run the feed rules in
    # strict mode so a version.h bump without the matching version.txt (or a
    # pin typo) fails HERE instead of publishing a feed the exe will refuse.
    # check_feed is local-only, so this is safe before the release exists.
    # The published-asset and deployed-exe halves still cannot run until after
    # `gh release create` - they stay manual, and the note below says so.
    strict=""
    if git diff --name-only "$remote_sha..$local_sha" 2>/dev/null | grep -q "^version.txt$"; then
        strict="--release"
        echo "release guard: push carries version.txt - strict feed rules apply"
        echo "release guard: NOTE after publishing, still run: release_guard.py --verify-asset and --deployed <exe>"
    fi
    python3 "$guard" --commit "$local_sha" $strict || fail=1
done
exit $fail
