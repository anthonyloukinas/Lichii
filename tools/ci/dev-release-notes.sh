#!/usr/bin/env bash
# usage: dev-release-notes.sh <output.md>
# Env: REPO TAG SHA, optional PREV_TAG (previous build-N release), LINK_TAG (release whose assets
# the links point at, default TAG), UPSTREAM_REPO + UPSTREAM_BRANCH + UPSTREAM_REF (a fetched ref of
# the parent's default branch), ASSET_DIR (default dist). Run inside a checkout with full commit history.
set -euo pipefail

out="${1:?usage: dev-release-notes.sh <output.md>}"
repo="${REPO:?REPO is required}"
tag="${TAG:?TAG is required}"
sha=$(git rev-parse --verify "${SHA:?SHA is required}^{commit}")
prev="${PREV_TAG:-}"
dir="${ASSET_DIR:-dist}"
base_url="https://github.com/${repo}/releases/download/${LINK_TAG:-$tag}"
max=100

link() { if [[ -e "$dir/$2" ]]; then printf '[%s](%s/%s)' "$1" "$base_url" "$2"; fi; return 0; }
join() { if [[ -n "$1" && -n "$2" ]]; then printf '%s · %s' "$1" "$2"; else printf '%s%s' "$1" "$2"; fi; }
commits() {
  local total
  total=$(git rev-list --count --no-merges "$@")
  git log --no-merges -n "$max" --pretty='- %s (`%h`)' "$@"
  if (( total > max )); then printf -- '- ... and %d more\n' "$((total - max))"; fi
}

upstream_base=""
if [[ -n "${UPSTREAM_REF:-}" ]] && git rev-parse -q --verify "${UPSTREAM_REF}^{commit}" > /dev/null; then
  upstream_base=$(git merge-base "$sha" "$UPSTREAM_REF" || true)
fi

{
  printf 'Lichii %s, built from [`%s`](https://github.com/%s/commit/%s).\n\n' \
    "${tag/build-/dev build }" "${sha:0:10}" "$repo" "$sha"
  if [[ -n "$upstream_base" ]]; then
    behind=$(git rev-list --count --no-merges "$sha..$UPSTREAM_REF")
    printf 'Based on upstream %s `%s` at [`%s`](https://github.com/%s/commit/%s) (%s).' \
      "${UPSTREAM_REPO:-?}" "${UPSTREAM_BRANCH:-?}" "${upstream_base:0:10}" "${UPSTREAM_REPO:-?}" "$upstream_base" \
      "$(git log -1 --format='%s, %cs' "$upstream_base")"
    if (( behind > 0 )); then printf ' Upstream has %d newer commits that are not in this build.' "$behind"; fi
    printf '\n\n'
  fi
  printf 'This is a test build: settings, caches and saves may change between builds. Keep a copy of your user folder.\n\n'

  w_std=$(link 'zip' lichii-windows-x86_64.zip)
  w_pgo=$(link 'zip' lichii-windows-x86_64-pgo.zip)
  if [[ -n "$w_std$w_pgo" ]]; then
    printf '## Windows\n\nExtract the zip and run `lichii.exe` (`eden-cli.exe` is the command-line version).\n\n'
    printf '| Build | Download | Notes |\n| --- | --- | --- |\n'
    [[ -z "$w_std" ]] || printf '| Standard (MSVC) | %s | Needs the Visual C++ 2015-2022 redistributable |\n' "$w_std"
    [[ -z "$w_pgo" ]] || printf '| PGO (MinGW clang) | %s | Profile-guided build, bundles its own runtime DLLs |\n' "$w_pgo"
    printf '\n'
  fi

  l_std=$(join "$(link AppImage lichii-linux-x86_64.AppImage)" "$(link tar.gz lichii-linux-x86_64.tar.gz)")
  l_pgo=$(join "$(link AppImage lichii-linux-x86_64-pgo.AppImage)" "$(link tar.gz lichii-linux-x86_64-pgo.tar.gz)")
  if [[ -n "$l_std$l_pgo" ]]; then
    printf '## Linux (x86_64)\n\nThe AppImage is self-contained (glibc 2.39 or newer: Ubuntu 24.04, Fedora 40, Debian 13, Arch): `chmod +x` it and run it. The tar.gz uses the system Qt 6 and libraries.\n\n'
    printf '| Build | Download |\n| --- | --- |\n'
    [[ -z "$l_std" ]] || printf '| Standard (GCC) | %s |\n' "$l_std"
    [[ -z "$l_pgo" ]] || printf '| PGO (clang) | %s |\n' "$l_pgo"
    printf '\n'
  fi

  if [[ -n "$prev" ]] && git rev-parse -q --verify "$prev^{commit}" > /dev/null; then
    printf '## Changes since %s\n\n[Full comparison](https://github.com/%s/compare/%s...%s)\n\n' \
      "$prev" "$repo" "$prev" "$tag"
    if [[ -n "$upstream_base" ]]; then
      printf '### This fork\n\n'
      commits "$prev..$sha" --not "$UPSTREAM_REF"
      printf '\n### Merged from upstream\n\n'
      commits "$prev..$upstream_base"
    else
      commits "$prev..$sha"
    fi
  elif [[ -n "$upstream_base" ]]; then
    printf '## Commits on top of upstream\n\n'
    commits "$upstream_base..$sha"
  else
    printf '## Latest commits\n\n'
    max=30 commits "$sha"
  fi
  printf '\n'
} > "$out"
