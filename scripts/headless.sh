#!/bin/sh
# Run a program headless with an --inject script and keep the shot:PATH.ppm files as PNG.
# usage: scripts/headless.sh writer|artist INSTALL_DIR "inject steps" [frames] [extra cwfa options]
# The login steps for a new user "Ana" are available as $LOGIN in the steps (see scripts/login.inc).
set -e
cd "$(dirname "$0")/.."
prog=$1; inst=$2; steps=$3; frames=${4:-3000}; shift 4 2>/dev/null || shift $#
case "$prog" in
  writer) exe=reference/WRITER.EXE ;;
  artist) exe=reference/ARTIST.EXE ;;
  *) echo "usage: $0 writer|artist INSTALL_DIR steps [frames] [options]" >&2; exit 2 ;;
esac
[ -x build/cwfa ] || make
exec build/cwfa --exe "$exe" --cd "${CWFA_CD_ROOT:-reference/cd}" --headless --frames "$frames" \
  --max-steps 12000000000 --install-dir "$inst" --inject "$steps" "$@"
