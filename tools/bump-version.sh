#!/bin/bash
# Bump the project version in PKGBUILD (pkgver/pkgrel) and meson.build.
# Prints the new version on stdout.
#
#   tools/bump-version.sh patch|minor|major
set -euo pipefail

bump="${1:?usage: bump-version.sh patch|minor|major}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pkgbuild="$root/PKGBUILD"
meson="$root/meson.build"

current="$(sed -n 's/^pkgver=//p' "$pkgbuild")"
[[ "$current" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "error: cannot parse pkgver '$current'" >&2
    exit 1
}

IFS=. read -r major minor patch <<<"$current"
case "$bump" in
    major) major=$((major + 1)); minor=0; patch=0 ;;
    minor) minor=$((minor + 1)); patch=0 ;;
    patch) patch=$((patch + 1)) ;;
    *) echo "error: unknown bump '$bump'" >&2; exit 1 ;;
esac
next="$major.$minor.$patch"

sed -i "s/^pkgver=.*/pkgver=$next/" "$pkgbuild"
sed -i "s/^pkgrel=.*/pkgrel=1/" "$pkgbuild"
sed -i "s/^  version: '.*',$/  version: '$next',/" "$meson"

echo "$next"
