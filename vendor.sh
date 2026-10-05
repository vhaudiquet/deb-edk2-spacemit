#!/bin/bash
#
# vendor.sh - sync edk2 / edk2-platforms from upstream into this repository.
#
# Usage:
#   ./vendor.sh <edk2|edk2-platforms> [ref] [url]
#
#   ref : branch, tag or commit id (default: k3-release)
#   url : upstream repository.  May also be supplied through EDK2_VENDOR_URL
#         or EDK2_PLATFORMS_VENDOR_URL.
#
# There is deliberately no built-in default URL, so that no particular
# upstream host is baked into this repository.
#
# The script clones upstream into a temporary directory, rsyncs it over
# <component>/ with --delete, strips .git and .gitmodules, and stages the
# result.  It does not commit: review the diff and commit yourself, recording
# the new upstream revision in the commit message.
#
set -euo pipefail

DEFAULT_REF=k3-release

die()  { printf 'vendor.sh: error: %s\n' "$*" >&2; exit 1; }
info() { printf '>> %s\n' "$*"; }

case "${1:-}" in
    edk2)
        component=edk2
        envvar=EDK2_VENDOR_URL
        ;;
    edk2-platforms)
        component=edk2-platforms
        envvar=EDK2_PLATFORMS_VENDOR_URL
        ;;
    *)
        printf 'usage: %s <edk2|edk2-platforms> [ref] [url]\n' "$0" >&2
        exit 2
        ;;
esac

ref="${2:-$DEFAULT_REF}"

url="${3:-}"
if [ -z "$url" ]; then
    url="$(printenv "$envvar" 2>/dev/null || true)"
fi
[ -n "$url" ] || die "no upstream URL for $component
  pass it as the third argument, or set $envvar:
    $envvar=<url> $0 $component $ref"

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || die "not inside a git repository"
cd "$repo_root"

dest="$repo_root/$component"
[ -d "$dest" ] || die "$component/ not found - run the initial vendoring first"

# Refuse to run where the component is still a submodule: rsync would write
# into a submodule working tree while the gitlink keeps shadowing it.
if git ls-files -s -- "$component" | awk '$1 == "160000" { found = 1 } END { exit !found }'; then
    die "$component is a git submodule here; vendor.sh only applies to a tree where it is vendored in-tree"
fi

# Do not overwrite anything uncommitted -- rsync --delete removes untracked
# files as well.
if [ -n "$(git status --porcelain --untracked-files=all -- "$component")" ]; then
    die "$component/ has uncommitted or untracked files; commit, stash or remove them first"
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/vendor-${component}-XXXXXX")
trap 'rm -rf "$tmp"' EXIT

info "fetching $ref from $url"
if ! git -c core.autocrlf=false clone --quiet --depth 1 --branch "$ref" \
        --single-branch "$url" "$tmp/src" 2>/dev/null; then
    # Not a branch or tag, so treat it as a commit id and fetch just that
    # commit instead of the whole history.  Fall back to a full clone only if
    # the server will not serve a commit by id.
    rm -rf "$tmp/src"
    if ! { git init -q "$tmp/src" &&
           git -C "$tmp/src" remote add origin "$url" &&
           git -C "$tmp/src" -c core.autocrlf=false fetch -q --depth 1 origin "$ref" &&
           git -C "$tmp/src" checkout -q --detach FETCH_HEAD; }; then
        rm -rf "$tmp/src"
        git -c core.autocrlf=false clone --quiet "$url" "$tmp/src"
        git -C "$tmp/src" checkout --quiet --detach "$ref"
    fi
fi
new_sha=$(git -C "$tmp/src" rev-parse HEAD)

# Nested submodules must be populated first: rsync --delete would otherwise
# treat everything under them as removed upstream.
#
# Their exit status is not a reliable success signal.  edk2 carries stale
# third-level gitlinks whose .gitmodules entries were dropped upstream
# (openssl, mipisyst, libspdm), so --recursive reports "No url found" even
# when everything that matters has been checked out.  Verify instead that
# every *declared* submodule is non-empty.
if [ -f "$tmp/src/.gitmodules" ]; then
    info "initializing nested submodules"
    git -C "$tmp/src" -c core.autocrlf=false submodule update --init --recursive --depth 1 || true

    empty=
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        [ -n "$(ls -A "$tmp/src/$p" 2>/dev/null)" ] || empty="$empty $p"
    done < <(git -C "$tmp/src" config -f .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk '{print $2}')
    [ -z "$empty" ] || die "declared submodules failed to populate:$empty -- refusing to rsync --delete with an incomplete tree"
fi

info "syncing into $component/"
# --exclude matches on basename, so '.git' also covers the nested submodule
# gitfiles, and it does not touch .gitignore / .gitattributes.
rsync -a --delete --exclude='.git' --exclude='.gitmodules' "$tmp/src/" "$dest/"
rm -f "$dest/.gitmodules"

git add -A -- "$component"

# Upstream tracks a few files that also match their own .gitignore rules
# (mbedtls/library/error.c, openssl/include/openssl/asn1_mac.h, ...).  Such
# rules only affect untracked files, so they are inert in the upstream
# repositories; but a plain 'git add -A' on a fresh import has no tracking
# history to go by and drops those files silently.  Re-add every path that
# appears in the upstream index rather than trusting the rules.
info "restoring files upstream tracks despite its own .gitignore"
git -C "$tmp/src" ls-files -z \
    | while IFS= read -r -d '' f; do
          [ -e "$dest/$f" ] && printf '%s\0' "$component/$f"
      done \
    | xargs -0 -r git add -f --

echo
echo "=== $component ==="
printf '  upstream : %s\n' "$url"
printf '  ref      : %s\n' "$ref"
printf '  revision : %s\n' "$new_sha"
printf '  subject  : %s\n' "$(git -C "$tmp/src" log -1 --format='%h %s')"
echo
git diff --cached --stat -- "$component" | tail -3
echo
echo "Review, then commit and record the revision, e.g.:"
printf '  git commit -m "vendor: sync %s to %s"\n' "$component" "${new_sha:0:12}"
