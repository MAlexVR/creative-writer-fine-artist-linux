#!/bin/sh
# Run the SPANISH Creative Writer or Fine Artist headless with an --inject script; shot:PATH.ppm steps are
# converted to PNG next to the PPM (needs ImageMagick) when PATH ends in .ppm.
# usage: scripts/headless_es.sh writer|artist INSTALL_DIR "steps" [frames] [extra cwfa options]
#   CWFA_ES_CW (default reference/es_nrg1) and CWFA_ES_FA (default "reference/es_fa/Microsoft Fine Artist")
#   name the extracted Spanish media. In the steps, $LOGIN_W expands to the Creative Writer first-time login
#   for "José Peña Núñez" up to the pseudonym page, and $LOGIN_A is not defined (explore with plain steps).
set -e
cd "$(dirname "$0")/.."
prog=$1; inst=$2; steps=$3; frames=${4:-6000}; shift 4 2>/dev/null || shift $#
LOGIN_W="wait:600;move:365,263;wait:30;click:365,263;wait:800;move:245,128;wait:10;click:245,128;wait:20;type:José;key:TAB;wait:20;type:Peña Núñez;wait:20;move:331,217;wait:10;click:331,217;wait:1500;move:320,231;wait:10;click:320,231;wait:2500;move:350,230;wait:10;click:350,230;wait:7000;move:332,185;wait:10;click:332,185;wait:6000"
steps=$(printf '%s' "$steps" | sed "s|\\\$LOGIN_W|$LOGIN_W|g")
case "$prog" in
  writer) opt=--es-writer; src=${CWFA_ES_CW:-reference/es_nrg1} ;;
  artist) opt=--es-artist; src=${CWFA_ES_FA:-reference/es_fa/Microsoft Fine Artist} ;;
  *) echo "usage: $0 writer|artist INSTALL_DIR steps [frames] [options]" >&2; exit 2 ;;
esac
[ -x build/cwfa ] || make
# CWFA_ES_LOG=FILE keeps cwfa's diagnostics (unimplemented API calls are logged there) instead of --quiet
if [ -n "${CWFA_ES_LOG:-}" ]; then q=; log=$CWFA_ES_LOG; else q=--quiet; log=/dev/null; fi
build/cwfa $opt "$src" --headless $q --frames "$frames" --max-steps 12000000000 \
  --install-dir "$inst" --inject "$steps" "$@" >/dev/null 2>"$log" || true
for ppm in $(printf '%s' "$steps" | tr ';' '\n' | sed -n 's/^shot://p' | grep '\.ppm$'); do
  [ -f "$ppm" ] && magick "$ppm" "${ppm%.ppm}.png" 2>/dev/null || true
done
