#!/bin/sh
# Adds Creative Writer / Fine Artist to the GNOME (or any freedesktop) application menu with their own icons:
# ~/.local/share/applications/cwfa-<program>.desktop and ~/.local/share/icons/hicolor/<N>x<N>/apps/cwfa-<program>.png
# for N = 32 (the program's own pixels), 64, 128 and 256 (whole-number scaling, so they stay sharp).
# The windows carry the same app id (cwfa-writer, cwfa-artist, cwfa-es-writer, cwfa-es-artist), which is how the shell
# pairs a running window with its launcher and icon (on Wayland the window icon cannot be set by the program itself).
# Nothing outside those two directories is touched; --remove deletes what this script wrote.
#   scripts/install-desktop.sh [--cd ENGLISH_CD_DIR] [--es-cd SPANISH_CW_MEDIA] [--es-fa-cd SPANISH_FA_MEDIA]
#                              [--launcher CMD] [--cwfa BIN] [--only writer|artist|es-writer|es-artist] [--data-dir DIR]
#                              [--media-conf] [--icons auto|original|plain] [--icons-only] [--quiet] [--remove]
# play.sh and the portable package run it by themselves every time a program starts (--only that program, --quiet,
# --data-dir the data directory of this run): it only writes what changed, so a normal start costs a few file checks.
# CWFA_NO_DESKTOP_INTEGRATION=1 stops them from doing it.
# --media-conf is the installed mode (cwfa-install): the entries start "LAUNCHER <program> --user-data" and the launcher finds
# the media in ~/.config/cwfa/media.conf; Creative Writer and Fine Artist always get an entry (their icons are made as soon
# as a medium is known), the Spanish ones only when Spanish media are given.
# --icons: the English CD edition draws a CD on its program icons; the Spanish editions carry the same artwork without it.
# "plain" (what "auto" means whenever Spanish media are given) takes the icons of every program from the Spanish medium when
# there is one, "original" always takes the icon of the program that is launched.
# --icons-only writes the icons and leaves the .desktop files alone (a package that ships them).
# From a checkout the defaults are reference/cd, reference/es_nrg1 and "reference/es_fa/Microsoft Fine Artist" and the
# launcher is scripts/play.sh; the portable package calls this through "cwfa-portable.run --install-desktop --cd ...".
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cwfa=$root/build/cwfa; launcher=$here/play.sh
cd_en=; cd_es=; cd_fa=; remove=; only=; datadir=; quiet=; media_conf=; style=auto; icons_only=; PLAIN_DIR=; PLAIN_FLAG=; SRC=
[ -d "$root/reference/cd" ] && cd_en=$root/reference/cd
[ -d "$root/reference/es_nrg1" ] && cd_es=$root/reference/es_nrg1
[ -d "$root/reference/es_fa/Microsoft Fine Artist" ] && cd_fa="$root/reference/es_fa/Microsoft Fine Artist"
usage() {
    echo "usage: $0 [--cd DIR] [--es-cd DIR] [--es-fa-cd DIR] [--launcher CMD] [--cwfa BIN] [--only PROGRAM] [--data-dir DIR] [--media-conf] [--icons auto|original|plain] [--icons-only] [--quiet] [--remove]" >&2
    exit 2
}
while [ $# -gt 0 ]; do
    case "$1" in
        --cd) cd_en=$2; shift 2 ;;
        --es-cd) cd_es=$2; shift 2 ;;
        --es-fa-cd) cd_fa=$2; shift 2 ;;
        --launcher) launcher=$2; shift 2 ;;
        --cwfa) cwfa=$2; shift 2 ;;
        --only) only=$2; shift 2 ;;
        --data-dir) datadir=$2; shift 2 ;;
        --media-conf) media_conf=1; shift ;;
        --icons) style=$2; shift 2 ;;
        --icons-only) icons_only=1; shift ;;
        --quiet) quiet=1; shift ;;
        --remove) remove=1; shift ;;
        *) usage ;;
    esac
done
case "$style" in auto|original|plain) ;; *) usage ;; esac
data=${XDG_DATA_HOME:-$HOME/.local/share}
apps=$data/applications
hicolor=$data/icons/hicolor
sizes="32 64 128 256"
names="writer artist es-writer es-artist"
refresh_caches() {
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1 || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t "$hicolor" >/dev/null 2>&1 || true
}
if [ -n "$remove" ]; then
    for n in $names; do
        rm -f "$apps/cwfa-$n.desktop" "$hicolor/.cwfa-icons-$n"
        for z in $sizes; do rm -f "$hicolor/${z}x${z}/apps/cwfa-$n.png"; done
    done
    refresh_caches
    [ -n "$quiet" ] || echo "removed the cwfa launchers from $apps"
    exit 0
fi
[ -x "$cwfa" ] || { echo "install-desktop: $cwfa not found (run make)" >&2; exit 1; }
mkdir -p "$apps"
quote() { printf '"%s"' "$(printf '%s' "$1" | sed 's/[\\"$`]/\\&/g')"; }   # one Exec argument
pass_cd=1; case "$(basename "$launcher")" in play.sh) pass_cd= ;; esac
[ -z "$media_conf" ] || pass_cd=
made=; changed=
# entry ID TITLE [CWFA_OPTION...] : the options name the program whose own icon is exported (none: no medium yet); $SRC is the
# media directory of the program (its --cd in the Exec line of a package entry); $PLAIN_FLAG $PLAIN_DIR name the Spanish
# medium whose plain artwork may be used instead
entry() {
    id=$1; title=$2; shift 2
    [ -z "$only" ] || [ "$only" = "$id" ] || return 0
    prog=$id; [ -n "$pass_cd" ] && prog=${id#es-}      # the package launcher tells the Spanish disks apart by their files
    if [ -n "$media_conf" ]; then
        exec_line=$(quote "$launcher")" $id --user-data"
    else
        exec_line=$(quote "$launcher")" $prog"
        [ -n "$pass_cd" ] && exec_line="$exec_line --cd "$(quote "$SRC")
        # the data directory of a launcher started from the menu: the one of this run, else not the (unknown) current directory
        exec_line="$exec_line --data-dir "$(quote "${datadir:-$data/cwfa/$id}")
    fi
    if [ -z "$icons_only" ]; then
        want=$(cat <<DESK
[Desktop Entry]
Type=Application
Name=$title
Comment=Microsoft Creative Writer and Fine Artist (1995), native runtime
Exec=$exec_line
Icon=cwfa-$id
Terminal=false
Categories=Education;
StartupWMClass=cwfa-$id
DESK
)
        if [ "$(cat "$apps/cwfa-$id.desktop" 2>/dev/null)" != "$want" ]; then
            printf '%s\n' "$want" > "$apps/cwfa-$id.desktop"
            changed=1
        fi
    fi
    # which program's pixels: a plain (disc-free) Spanish icon when it can be had and is wanted, else the program's own
    kind=original
    if [ "$style" != original ] && [ -n "$PLAIN_DIR" ]; then set -- "$PLAIN_FLAG" "$PLAIN_DIR"; kind=plain; fi
    [ -n "${1:-}" ] || { made="$made $id"; return 0; }          # no medium yet: the icons come with the first start
    missing=
    for z in $sizes; do [ -s "$hicolor/${z}x${z}/apps/cwfa-$id.png" ] || missing=1; done
    [ "$(cat "$hicolor/.cwfa-icons-$id" 2>/dev/null)" = "$kind" ] || missing=1
    if [ -n "$missing" ]; then
        tmp=$(mktemp -d "${TMPDIR:-/tmp}/cwfa-icon.XXXXXX")
        for z in $sizes; do
            mkdir -p "$hicolor/${z}x${z}/apps"
            "$cwfa" "$@" --quiet --install-dir "$tmp/c" --export-icon "$tmp/$z.png" --export-icon-size "$z" 2>/dev/null &&
                mv "$tmp/$z.png" "$hicolor/${z}x${z}/apps/cwfa-$id.png" || echo "install-desktop: no ${z}x$z icon for $id" >&2
        done
        rm -r "$tmp"
        printf '%s\n' "$kind" > "$hicolor/.cwfa-icons-$id"
        changed=1
    fi
    made="$made $id"
}
# Spanish only (installed mode, Spanish media known, no English disc): the Spanish programs are listed as plain "Creative Writer" and
# "Fine Artist", no English entry is made, and an English entry a package ships system-wide is hidden for this user by an
# override of the same name (Hidden=true) that goes away as soon as an English disc is configured again
spanish_only=; es_suffix=" (Español)"
if [ -n "$media_conf" ] && [ -z "$cd_en" ] && { [ -n "$cd_es" ] || [ -n "$cd_fa" ]; }; then spanish_only=1; es_suffix=; fi
system_entry() {
    oldifs=$IFS; IFS=:
    for d in ${XDG_DATA_DIRS:-/usr/local/share:/usr/share}; do
        [ -f "$d/applications/cwfa-$1.desktop" ] && { IFS=$oldifs; return 0; }
    done
    IFS=$oldifs; return 1
}
if [ -n "$media_conf" ]; then
    for id in writer artist; do
        f=$apps/cwfa-$id.desktop
        if [ -n "$spanish_only" ]; then
            if system_entry "$id"; then
                case $id in writer) t="Creative Writer" ;; *) t="Fine Artist" ;; esac
                want=$(printf '[Desktop Entry]\nType=Application\nName=%s\nHidden=true' "$t")
                if [ "$(cat "$f" 2>/dev/null)" != "$want" ]; then printf '%s\n' "$want" > "$f"; changed=1; fi
            elif [ -e "$f" ]; then rm -f "$f"; changed=1; fi
            for z in $sizes; do
                if [ -e "$hicolor/${z}x${z}/apps/cwfa-$id.png" ]; then rm -f "$hicolor/${z}x${z}/apps/cwfa-$id.png"; changed=1; fi
            done
            rm -f "$hicolor/.cwfa-icons-$id"
        elif grep -qx 'Hidden=true' "$f" 2>/dev/null; then
            rm -f "$f"; changed=1
        fi
    done
fi
# the English programs: an entry always in the installed mode, otherwise only with their CD
if [ -z "$spanish_only" ] && { [ -n "$cd_en" ] || [ -n "$media_conf" ]; }; then
    if [ -n "$cd_en" ]; then
        SRC=$cd_en PLAIN_FLAG=--es-writer PLAIN_DIR=$cd_es entry writer "Creative Writer" --exe "$cd_en/SETUP/WRITER.EX_" --cd "$cd_en"
        SRC=$cd_en PLAIN_FLAG=--es-artist PLAIN_DIR=$cd_fa entry artist "Fine Artist" --exe "$cd_en/SETUP/ARTIST.EX_" --cd "$cd_en"
    else
        SRC= PLAIN_FLAG=--es-writer PLAIN_DIR=$cd_es entry writer "Creative Writer"
        SRC= PLAIN_FLAG=--es-artist PLAIN_DIR=$cd_fa entry artist "Fine Artist"
    fi
fi
[ -z "$cd_es" ] || SRC=$cd_es PLAIN_DIR= entry es-writer "Creative Writer$es_suffix" --es-writer "$cd_es"
[ -z "$cd_fa" ] || SRC=$cd_fa PLAIN_DIR= entry es-artist "Fine Artist$es_suffix" --es-artist "$cd_fa"
[ -z "$changed" ] || refresh_caches
[ -n "$quiet" ] || echo "installed:${made:- nothing (no media given)}  ($apps, $hicolor)"
