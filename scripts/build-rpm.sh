#!/bin/sh
# Builds the RPM (binary and source) from the committed tree, so the package matches what is in git:
#   scripts/build-rpm.sh            (also: make rpm)        output in build/rpm/RPMS and build/rpm/SRPMS:
#   cwfa (runtime and cwfa-launch), cwfa-english and cwfa-spanish (menu entries and AppStream data of each edition)
# Needs rpmbuild (rpm-build) and the build requirements of packaging/rpm/cwfa.spec (gcc, make, SDL2 and freetype headers,
# desktop-file-utils, appstream). Nothing is installed: install the result with dnf, and remove it with dnf or GNOME Software.
# rpmbuild cannot work in a directory whose name has spaces or an ampersand, so it builds in a scratch directory.
set -e
cd "$(dirname "$0")/.."
command -v rpmbuild >/dev/null 2>&1 || { echo "build-rpm: rpmbuild not found (install rpm-build)" >&2; exit 1; }
ver=$(sed -n 's/^Version:[[:space:]]*//p' packaging/rpm/cwfa.spec)
out=$PWD/build/rpm
top=$(mktemp -d "${TMPDIR:-/tmp}/cwfa-rpm.XXXXXX")
trap 'rm -rf "$top"' EXIT
mkdir -p "$top/SOURCES" "$top/SPECS" "$top/BUILD" "$top/RPMS" "$top/SRPMS" "$top/BUILDROOT"
git archive --format=tar.gz --prefix="cwfa-$ver/" -o "$top/SOURCES/cwfa-$ver.tar.gz" HEAD
cp packaging/rpm/cwfa.spec "$top/SPECS/"
rpmbuild -ba --define "_topdir $top" "$top/SPECS/cwfa.spec"
case "$out" in */build/rpm) rm -rf "$out" ;; esac
mkdir -p "$out"
cp -r "$top/RPMS" "$top/SRPMS" "$out/"
echo "build-rpm: packages in $out/RPMS (cwfa: runtime; cwfa-english, cwfa-spanish: menu entries)"
ls "$out"/RPMS/*/cwfa*.rpm | grep -v -e debuginfo -e debugsource
