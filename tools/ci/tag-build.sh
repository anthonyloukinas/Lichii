#!/usr/bin/env bash
# usage: tools/ci/tag-build.sh [--remote <name>] [--branch <name>] [--floor <N>] [<commit>]
# Tags <commit> (default: the remote's tip of --branch) as the next build-N and prints how to
# publish it. It never pushes. N is one more than the highest build-N tag on the remote, and at
# least --floor (default 1000, which keeps the numbers clear of the upstream build-N series).
set -euo pipefail

branch=dev
floor=1000
remote=""
commit=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --remote) remote="${2:?--remote needs a name}"; shift 2 ;;
    --branch) branch="${2:?--branch needs a name}"; shift 2 ;;
    --floor) floor="${2:?--floor needs a number}"; shift 2 ;;
    -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) commit="$1"; shift ;;
  esac
done
[[ "$floor" =~ ^[1-9][0-9]*$ ]] || { echo "--floor must be a positive number" >&2; exit 2; }

# The remote the release branch tracks, so the tag lands next to the branch it was cut from.
if [[ -z "$remote" ]]; then
  remote=$(git config "branch.$branch.remote" || true)
  [[ -n "$remote" ]] || { echo "branch $branch tracks no remote; pass --remote" >&2; exit 2; }
fi

git fetch --quiet --no-tags "$remote" "+refs/heads/$branch:refs/remotes/$remote/$branch"
tip=$(git rev-parse --verify "refs/remotes/$remote/$branch^{commit}")
sha=$(git rev-parse -q --verify "${commit:-$tip}^{commit}") || { echo "not a commit: $commit" >&2; exit 2; }
if ! git merge-base --is-ancestor "$sha" "$tip"; then
  echo "warning: ${sha:0:10} is not on $remote/$branch; the release would contain unpromoted commits" >&2
fi

remote_tags=$(git ls-remote --tags --refs "$remote" 'build-*' | sed 's,.*refs/tags/,,')
highest=$(grep -E '^build-[1-9][0-9]*$' <<< "$remote_tags" | sed 's/^build-//' | sort -n | tail -n1 || true)
next=$(( ${highest:-0} + 1 ))
(( next >= floor )) || next=$floor
tag="build-$next"

# Remote tags already on this commit (the commit was published before).
while read -r tsha tref; do
  if [[ "$tsha" == "$sha" ]]; then
    tref=${tref#refs/tags/}
    echo "note: ${sha:0:10} is already tagged ${tref%^\{\}} on $remote" >&2
  fi
done < <(git ls-remote --tags "$remote" 'build-*')

if existing=$(git rev-parse -q --verify "refs/tags/$tag^{commit}"); then
  if [[ "$existing" != "$sha" ]]; then
    echo "local tag $tag already points to ${existing:0:10}; remove it with: git tag -d $tag" >&2
    exit 1
  fi
else
  git tag -a "$tag" -m "Lichii $branch build $next" "$sha"
fi

repo=$(git remote get-url "$remote" | sed -E 's,^(https://github\.com/|git@github\.com:),,; s,\.git$,,')
echo "Tagged $tag at ${sha:0:10} ($(git log -1 --format=%s "$sha"))"
echo
echo "Publish it (the release workflow runs on the tag push):"
echo "  git push $remote refs/tags/$tag"
echo
echo "Or let the workflow create the tag on the remote; runs on $branch reuse its compiler caches:"
echo "  gh workflow run release-binaries.yml -R $repo --ref $branch -f tag=$tag -f commit=$sha"
