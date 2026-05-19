#!/bin/sh
# scripts/bump.sh — one-line release: bump → build everything → tag → publish.
#
# Usage:
#   scripts/bump.sh 0.2.0          # full release (github + aur)
#   scripts/bump.sh 0.2.0 --dry    # everything except `git push`, `gh release`, aur push
#   scripts/bump.sh 0.2.0 --no-aur # skip the AUR sync
#
# Flow:
#   1. Verify the working tree is clean (no uncommitted changes outside CMakeLists).
#   2. Rewrite `project(agentty VERSION X.Y.Z ...)` in CMakeLists.txt.
#   3. `cmake --build build -j` (sanity: the new version still compiles).
#   4. git commit "release: vX.Y.Z" + git tag vX.Y.Z.
#   5. scripts/release.sh --tag vX.Y.Z (builds every artifact + uploads via gh).
#   6. Sync AUR repo: pull aur@aur.archlinux.org:agentty-bin.git into dist/aur/,
#      replace PKGBUILD + regenerate .SRCINFO, commit + push.
#   7. git push origin master --tags.
#
# Single source of truth: `CMakeLists.txt`. Everything downstream — User-Agent
# strings baked into the binary, deb/rpm/arch/scoop/homebrew/AUR manifests,
# install.sh's `--version v…` resolver, the release tag itself — derives
# from that one line.

set -eu

NEW_VERSION=${1:-}
DRY=0
DO_AUR=1
[ $# -gt 0 ] && shift
while [ $# -gt 0 ]; do
    case "$1" in
        --dry)    DRY=1 ;;
        --no-aur) DO_AUR=0 ;;
        *) echo "bump.sh: unknown flag: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ -z "$NEW_VERSION" ] || ! echo "$NEW_VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "usage: bump.sh <major.minor.patch> [--dry] [--no-aur]" >&2
    echo "  e.g. bump.sh 0.2.0" >&2
    exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

# ---- ui ----------------------------------------------------------------------
hr()   { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m\xe2\x9c\x93\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m\xe2\x9c\x97\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 1. tree clean -----------------------------------------------------------
hr "1/7  preflight"
current=$(sed -nE 's/.*project\(agentty VERSION ([0-9.]+).*/\1/p' CMakeLists.txt | head -1)
[ -n "$current" ] || err "could not read current VERSION from CMakeLists.txt"
info "$current  ->  $NEW_VERSION"

# Allow changes confined to CMakeLists.txt (in case bump.sh is rerun); reject
# anything else dirty so we don't accidentally tag a half-finished feature.
dirty=$(git status --porcelain | grep -v ' CMakeLists.txt$' | grep -v '^?? ' || true)
[ -z "$dirty" ] || err "uncommitted changes present outside CMakeLists.txt:
$dirty
commit or stash them first."

[ "$current" != "$NEW_VERSION" ] || err "version already $NEW_VERSION — nothing to bump"

# ---- 2. rewrite CMakeLists.txt ----------------------------------------------
hr "2/7  bump CMakeLists.txt"
sed -i -E "s/(project\(agentty VERSION )[0-9.]+/\1$NEW_VERSION/" CMakeLists.txt
grep -E "^project\(agentty VERSION $NEW_VERSION" CMakeLists.txt >/dev/null \
    || err "sed rewrite failed"
ok "project(agentty VERSION $NEW_VERSION ...)"

# ---- 3. compile sanity-check ------------------------------------------------
hr "3/7  build (sanity)"
if [ -d build ]; then
    cmake --build build -j10 >/dev/null
    ok "rebuild green"
    actual=$("$root/build/agentty" --version | awk '{print $2}')
    [ "$actual" = "$NEW_VERSION" ] || err "binary reports $actual, expected $NEW_VERSION"
    ok "binary --version reports $actual"
else
    info "no build/ directory — skipping local rebuild sanity check"
fi

# ---- 4. commit + tag --------------------------------------------------------
hr "4/7  commit + tag"
git add CMakeLists.txt
git commit -m "release: v$NEW_VERSION" >/dev/null
ok "committed"
git tag "v$NEW_VERSION"
ok "tagged v$NEW_VERSION"

# ---- 5. release.sh ----------------------------------------------------------
hr "5/7  build + upload artifacts"
if [ "$DRY" -eq 1 ]; then
    info "--dry: skipping release.sh upload, building only"
    "$root/scripts/release.sh"
else
    "$root/scripts/release.sh" --tag "v$NEW_VERSION"
fi

# ---- 6. AUR sync ------------------------------------------------------------
hr "6/7  AUR (agentty-bin)"
if [ "$DO_AUR" -eq 0 ]; then
    info "--no-aur: skipping AUR push"
elif ! command -v makepkg >/dev/null 2>&1; then
    info "makepkg not installed — skipping AUR (rerun on an Arch host to publish)"
else
    aur_dir="$root/dist/aur/agentty-bin"
    # Idempotent: clone if missing, otherwise fetch + reset to remote master
    # so we never push diverged history (release.sh might have re-staged it).
    if [ ! -d "$aur_dir/.git" ]; then
        info "cloning ssh://aur@aur.archlinux.org/agentty-bin.git"
        mkdir -p "$(dirname "$aur_dir")"
        if ! git clone -q ssh://aur@aur.archlinux.org/agentty-bin.git "$aur_dir" 2>/dev/null; then
            info "clone failed — initializing fresh repo (first-time publish)"
            rm -rf "$aur_dir"
            git init -q -b master "$aur_dir"
            ( cd "$aur_dir" && git remote add origin ssh://aur@aur.archlinux.org/agentty-bin.git )
        fi
    else
        info "refreshing existing AUR checkout"
        ( cd "$aur_dir" \
            && git fetch -q origin master 2>/dev/null \
            && git reset -q --hard origin/master 2>/dev/null ) || true
    fi

    # PKGBUILD with real sha256s was written by release.sh into dist/packaging/.
    cp "$root/dist/packaging/PKGBUILD" "$aur_dir/PKGBUILD"
    ( cd "$aur_dir" && makepkg --printsrcinfo > .SRCINFO )

    if [ -z "$(cd "$aur_dir" && git status --porcelain)" ]; then
        info "AUR already up-to-date for v$NEW_VERSION"
    else
        ( cd "$aur_dir" \
            && git add PKGBUILD .SRCINFO \
            && git -c user.name="$(git config user.name)" \
                   -c user.email="$(git config user.email)" \
                   commit -q -m "agentty-bin $NEW_VERSION-1" )
        if [ "$DRY" -eq 1 ]; then
            info "--dry: skipping aur git push (staged at $aur_dir)"
        else
            ( cd "$aur_dir" && git push -q origin master )
            ok "https://aur.archlinux.org/packages/agentty-bin (v$NEW_VERSION-1)"
        fi
    fi
fi

# ---- 7. push ----------------------------------------------------------------
hr "7/7  push github"
if [ "$DRY" -eq 1 ]; then
    info "--dry: skipping git push"
else
    git push origin master --tags
    ok "pushed master + tags"
fi

hr "done — v$NEW_VERSION"
info "github: https://github.com/1ay1/agentty/releases/tag/v$NEW_VERSION"
[ "$DO_AUR" -eq 1 ] && info "aur:    https://aur.archlinux.org/packages/agentty-bin"
