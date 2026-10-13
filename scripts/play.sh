#!/bin/sh
# Run Creative Writer or Fine Artist live in an SDL window, from a checkout.
# usage: scripts/play.sh writer|artist|es-writer|es-artist [--data-dir DIR] [cwfa options]   (e.g. --fullscreen, --scale 3)
# Where your data lives (users, saved work, WIN.INI = the programs' drive C:): ./cwfa-data in the directory you run this
# from (./cwfa-data-es for the Spanish disks); --data-dir DIR chooses another place; CWFA_HOME=DIR keeps DIR/<program>.
# --install-dir and --c-root go straight to cwfa.
# The first start also puts the program's menu entry and icons in ~/.local/share/{applications,icons} (GNOME pairs the window
# with them by its app id); CWFA_NO_DESKTOP_INTEGRATION=1 skips that.
set -e
orig=$PWD
cd "$(dirname "$0")/.."
prog=${1:-writer}; shift || true
data=
rest=
# pull --data-dir out of the arguments, keep the others (as positional parameters, so quoting survives)
n=$#
while [ $n -gt 0 ]; do
    a=$1; shift; n=$((n - 1))
    if [ "$a" = --data-dir ] && [ $n -gt 0 ]; then data=$1; shift; n=$((n - 1)); else set -- "$@" "$a"; fi
done
own_c=
for a in "$@"; do case "$a" in --install-dir|--c-root|--no-install) own_c=1 ;; esac; done
case "$prog" in
  writer) exe=reference/WRITER.EXE; deflt=cwfa-data ;;
  artist) exe=reference/ARTIST.EXE; deflt=cwfa-data ;;
  es-writer|es-artist) deflt=cwfa-data-es ;;  # the Spanish 1.0 disks, extracted under reference/ (see README)
  *) echo "usage: $0 writer|artist|es-writer|es-artist [--data-dir DIR] [cwfa options]" >&2; exit 2 ;;
esac
if [ -z "$data" ]; then
    if [ -n "${CWFA_HOME:-}" ]; then data=$CWFA_HOME/$prog; else data=$orig/$deflt; fi
fi
case "$data" in /*) ;; *) data=$orig/$data ;; esac
[ -x build/cwfa ] || make
if [ -z "$own_c" ]; then
    echo "cwfa: your data (users, saved work, WIN.INI = drive C:) is in $data" >&2
    mkdir -p "$data"
    set -- --install-dir "$data" "$@"
fi
# menu entry + icons for this program (only what changed is written; nothing for a window-less --headless run)
integrate() {
    set -- --only "$prog" --quiet
    [ -z "$own_c" ] && set -- "$@" --data-dir "$data"
    [ -n "${CWFA_ES_CW:-}" ] && set -- "$@" --es-cd "$CWFA_ES_CW"
    [ -n "${CWFA_ES_FA:-}" ] && set -- "$@" --es-fa-cd "$CWFA_ES_FA"
    scripts/install-desktop.sh "$@" >/dev/null 2>&1 || true
}
headless=; for a in "$@"; do [ "$a" = --headless ] && headless=1; done
if [ -z "${CWFA_NO_DESKTOP_INTEGRATION:-}" ] && { [ -z "$headless" ] || [ -n "${CWFA_DESKTOP_INTEGRATION:-}" ]; }; then integrate; fi
case "$prog" in
  es-writer|es-artist)
    src=${CWFA_ES_CW:-reference/es_nrg1}
    [ "$prog" = es-artist ] && src=${CWFA_ES_FA:-reference/es_fa/Microsoft Fine Artist}
    exec build/cwfa "--$prog" "$src" --scale 2 --quiet "$@" ;;
  *)
    exec build/cwfa --exe "$exe" --cd reference/cd --scale 2 --quiet "$@" ;;
esac
