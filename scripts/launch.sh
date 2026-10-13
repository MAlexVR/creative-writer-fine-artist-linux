#!/bin/sh
# Launcher inside the portable package: runs Creative Writer or Fine Artist from the CD the user supplies.
#   cwfa-portable.run writer|artist --cd CD_DIR_OR_ISO [--home DIR] [cwfa options...]
# CD_DIR_OR_ISO is the CD's files as a directory, or the .ISO image (extracted once with 7z, bsdtar or isoinfo).
# Where your data lives (users, saved documents and paintings, WIN.INI = the programs' drive C:): by default ./cwfa-data in
# the directory you run this from (./cwfa-data-es for the Spanish disks); --data-dir DIR chooses another place and
# CWFA_HOME=DIR keeps DIR/<writer|artist>. --install-dir and --c-root go straight to cwfa.
# The first start of a program also puts its menu entry and icons in ~/.local/share/{applications,icons} (GNOME pairs the
# window with them by its app id, so the window shows the program's icon); CWFA_NO_DESKTOP_INTEGRATION=1 skips that.
# Nothing of the CD is part of this package. --exe PATH overrides the program file (default <cd>/SETUP/<NAME>.EX_,
# which on the CD is the plain Win16 executable). Everything after the options goes to cwfa (see cwfa without
# arguments): --scale N, --headless, --frames N, --screenshot PATH, ...
# Installed (cwfa-install, or the RPM) the same script is "cwfa-launch": the program may be writer, artist, es-writer or
# es-artist, --cd is optional (the media are remembered in ~/.config/cwfa/media.conf: asked once, in a dialog when there is a
# desktop, else on the terminal), --user-data keeps drive C: in ~/.local/share/cwfa/data (data-es for the Spanish editions),
# --configure sets the media again, --install / --uninstall run the installer.
set -e
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
usage() {
    cat >&2 <<EOF
usage: $0 writer|artist|es-writer|es-artist [--cd CD_DIR_OR_ISO] [--data-dir DIR | --user-data] [--exe PATH] [cwfa options...]
       $0 --configure [--cd CD] [--es-cd DIR] [--es-fa-cd DIR]      remember where the media are
       $0 --install [...] | --uninstall [--purge] [--yes]          install into your home directory / remove it again
  writer|artist   the program to run (es-writer, es-artist: the Spanish editions)
  --cd PATH       the CD's files (a directory) or the CD image (.ISO); or set CWFA_CD; installed, it is remembered
  --user-data     keep drive C: in ~/.local/share/cwfa/data (data-es for the Spanish editions)
  --data-dir DIR  where the programs keep their drive C: (users, saved work, WIN.INI); default ./cwfa-data in the current
                  directory (./cwfa-data-es for the Spanish disks), or \$CWFA_HOME/<program> when CWFA_HOME is set
                  (--home is the old name of --data-dir)
  --fullscreen    start in fullscreen; F11 or Alt+Enter toggles it any time (more: --integer-scale, --fixed-size)
  --exe PATH      the Win16 program file (default <cd>/SETUP/WRITER.EX_ or ARTIST.EX_)
EOF
    exit 2
}
from_package() {   # a system package has no user-level installer: its package manager installs and removes it
    echo "cwfa: this copy was installed by a package: install and remove it with your package manager (sudo dnf remove cwfa, or GNOME Software)" >&2
    exit 2
}
prog=; cd_arg=${CWFA_CD:-}; home=; exe=; desktop=; user_data=; configure=; es_cd_arg=; es_fa_arg=; lang=en
while [ $# -gt 0 ]; do
    case "$1" in
        writer|artist) [ -z "$prog" ] || usage; prog=$1; shift ;;
        es-writer|es-artist) [ -z "$prog" ] || usage; prog=${1#es-}; lang=es; shift ;;
        --user-data) user_data=1; shift ;;
        --configure) configure=1; shift ;;
        --install) shift; [ -x "$here/cwfa-install" ] || from_package; exec "$here/cwfa-install" "$@" ;;
        --uninstall) shift; [ -x "$here/cwfa-install" ] || from_package; exec "$here/cwfa-install" --uninstall "$@" ;;
        --es-cd) [ $# -ge 2 ] || usage; es_cd_arg=$2; shift 2 ;;
        --es-fa-cd) [ $# -ge 2 ] || usage; es_fa_arg=$2; shift 2 ;;
        --cd) [ $# -ge 2 ] || usage; cd_arg=$2; shift 2 ;;
        --home|--data-dir) [ $# -ge 2 ] || usage; home=$2; shift 2 ;;
        --exe) [ $# -ge 2 ] || usage; exe=$2; shift 2 ;;
        --install-desktop) desktop=install; shift ;;
        --remove-desktop) desktop=remove; shift ;;
        -h|--help) usage ;;
        *) break ;;
    esac
done
if [ "$desktop" = remove ]; then exec "$here/cwfa-install-desktop" --remove; fi
data=${XDG_DATA_HOME:-$HOME/.local/share}/cwfa    # the extracted CD image cache and, with --user-data, drive C:
conf=${XDG_CONFIG_HOME:-$HOME/.config}/cwfa
installed=; [ -n "${CWFA_PACKAGE:-}" ] || installed=1   # not started through the portable .run: media are remembered

# --- remembered media (installed mode): ~/.config/cwfa/media.conf with cd=, es_cw=, es_fa= ---
conf_get() { sed -n "s/^$1=//p" "$conf/media.conf" 2>/dev/null | head -n 1; }
conf_set() {
    mkdir -p "$conf"; touch "$conf/media.conf"
    grep -v "^$1=" "$conf/media.conf" > "$conf/media.conf.new" || true
    printf '%s=%s\n' "$1" "$2" >> "$conf/media.conf.new"
    mv "$conf/media.conf.new" "$conf/media.conf"
}
absp() {
    if [ -d "$1" ]; then (cd "$1" && pwd -P)
    elif [ -f "$1" ]; then printf '%s/%s\n' "$(cd "$(dirname "$1")" && pwd -P)" "$(basename "$1")"
    else return 1
    fi
}
# a path from the user: a dialog when there is a desktop and zenity, else the terminal; empty when neither
ask_path() {
    if [ -z "${CWFA_NO_DIALOG:-}" ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; } && command -v zenity >/dev/null 2>&1; then
        if zenity --question --title "Creative Writer & Fine Artist" --ok-label "Disc image (.ISO)" --cancel-label "Folder" \
                  --text "$1\n\nIs it a disc image (.ISO) or a folder with the disc's files?" 2>/dev/null; then
            zenity --file-selection --title "$1" --file-filter='Disc images | *.iso *.ISO' 2>/dev/null || true
        else
            zenity --file-selection --directory --title "$1" 2>/dev/null || true
        fi
    elif [ -t 0 ]; then
        printf '%s\n(the folder with the disc files, or the .ISO; empty to skip): ' "$1" >&2
        read -r p || p=
        printf '%s\n' "$p"
    fi
}
# ask_media KEY LABEL: asks for one medium and remembers it; returns 1 when none was given
ask_media() {
    p=$(ask_path "Where is the $2?") || p=
    [ -n "$p" ] || return 1
    p=$(absp "$p") || { echo "cwfa: $p does not exist" >&2; return 1; }
    conf_set "$1" "$p"
}
# refresh the launcher entries of every program whose medium is known (installed mode)
refresh_entries() {
    c=$(conf_get cd); [ -d "$c" ] || c=
    e=$(conf_get es_cw); f=$(conf_get es_fa)
    LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$here/cwfa-install-desktop" --cwfa "$here/bin/cwfa" \
        --launcher "$here/cwfa-launch" --media-conf --cd "$c" --es-cd "$e" --es-fa-cd "$f" --quiet >/dev/null 2>&1 || true
}
if [ -n "$configure" ]; then
    [ -z "$cd_arg" ] || cd_arg=$(absp "$cd_arg") || { echo "cwfa: no such path" >&2; exit 1; }
    [ -z "$es_cd_arg" ] || es_cd_arg=$(absp "$es_cd_arg") || { echo "cwfa: no such path" >&2; exit 1; }
    [ -z "$es_fa_arg" ] || es_fa_arg=$(absp "$es_fa_arg") || { echo "cwfa: no such path" >&2; exit 1; }
    if [ -z "$cd_arg$es_cd_arg$es_fa_arg" ]; then
        ask_media cd "English Creative Writer / Fine Artist CD" || true
        ask_media es_cw "Spanish Creative Writer disks (optional)" || true
        ask_media es_fa "Spanish Fine Artist disks (optional)" || true
    fi
    [ -z "$cd_arg" ] || conf_set cd "$cd_arg"
    [ -z "$es_cd_arg" ] || conf_set es_cw "$es_cd_arg"
    [ -z "$es_fa_arg" ] || conf_set es_fa "$es_fa_arg"
    [ -f "$conf/media.conf" ] || { echo "cwfa: nothing configured" >&2; exit 1; }
    refresh_entries
    echo "cwfa: media remembered in $conf/media.conf"
    exit 0
fi
[ -n "$prog" ] || [ -n "$desktop" ] || usage
if [ -n "$installed" ] && [ -n "$prog" ]; then
    case "$lang$prog" in
        enwriter|enartist) key=cd; label="English Creative Writer / Fine Artist CD" ;;
        eswriter) key=es_cw; label="Spanish Creative Writer disks" ;;
        esartist) key=es_fa; label="Spanish Fine Artist disks" ;;
    esac
    if [ -n "$cd_arg" ]; then
        cd_arg=$(absp "$cd_arg") || { echo "cwfa: $cd_arg is neither a directory nor a file" >&2; exit 1; }
        [ "$(conf_get "$key")" = "$cd_arg" ] || conf_set "$key" "$cd_arg"
    else
        cd_arg=$(conf_get "$key")
        if [ -z "$cd_arg" ] || [ ! -e "$cd_arg" ]; then
            ask_media "$key" "$label" || true
            cd_arg=$(conf_get "$key")
        fi
        [ -n "$cd_arg" ] && [ -e "$cd_arg" ] || { echo "cwfa: the $label is not configured: run  cwfa-launch --configure  (or give --cd PATH)" >&2; exit 2; }
    fi
fi
[ -n "$cd_arg" ] || { echo "cwfa: the CD is not part of this package: give --cd (a directory or the .ISO)" >&2; usage; }
# an explicit place for drive C: on the cwfa command line wins over the default
own_c=
for a in "$@"; do case "$a" in --install-dir|--c-root|--no-install) own_c=1 ;; esac; done

cd_dir=$cd_arg
if [ -f "$cd_arg" ]; then
    # an image: extract it once, keyed by name and size
    key=$(basename "$cd_arg" | tr -c 'A-Za-z0-9._-' _)-$(stat -c %s "$cd_arg")
    cd_dir=$data/cd/$key
    if [ ! -f "$cd_dir/.complete" ]; then
        echo "cwfa: extracting $cd_arg to $cd_dir (once)" >&2
        mkdir -p "$cd_dir"
        if command -v 7z >/dev/null 2>&1; then 7z x -y "-o$cd_dir" "$cd_arg" >/dev/null
        elif command -v bsdtar >/dev/null 2>&1; then bsdtar -xf "$cd_arg" -C "$cd_dir"
        elif command -v isoinfo >/dev/null 2>&1; then
            # no recursive extractor in isoinfo: list the files and extract one by one
            isoinfo -R -i "$cd_arg" -f 2>/dev/null | while IFS= read -r f; do
                case "$f" in */) mkdir -p "$cd_dir$f" ;; *) mkdir -p "$cd_dir$(dirname "$f")"; isoinfo -R -i "$cd_arg" -x "$f" > "$cd_dir$f" ;; esac
            done
        else
            echo "cwfa: cannot read the image: install 7zip (7z), bsdtar or cdrkit (isoinfo), or extract it yourself and pass the directory" >&2
            exit 1
        fi
        : > "$cd_dir/.complete"
    fi
elif [ ! -d "$cd_arg" ]; then
    echo "cwfa: $cd_arg is neither a directory nor a file" >&2
    exit 1
fi

# --install-desktop: application-menu entries and icons for this CD (the .run file itself is what the entries start)
if [ "$desktop" = install ]; then
    pkg=${CWFA_PACKAGE:-$0}
    cd_dir=$(cd "$cd_dir" && pwd)                     # the entries must not depend on the current directory
    if ls "$cd_dir" | grep -qi '^INSTALAR\.LST$' || ls "$cd_dir" | grep -qiE '^(disco|d)[0-9]+$'; then
        case "$(basename "$cd_dir")" in *[Ff]ine*|*FA*) which=--es-fa-cd ;; *) which=--es-cd ;; esac
        exec "$here/cwfa-install-desktop" --cwfa "$here/bin/cwfa" --launcher "$pkg" --cd "" "$which" "$cd_dir"
    fi
    exec "$here/cwfa-install-desktop" --cwfa "$here/bin/cwfa" --launcher "$pkg" --cd "$cd_dir" --es-cd "" --es-fa-cd ""
fi

# integrate ID MEDIA_OPTION MEDIA_DIR: the menu entry and icons of the program being started (only what changed is written,
# nothing for a window-less --headless run unless CWFA_DESKTOP_INTEGRATION=1, and a failure never stops the launch)
headless=; for a in "$@"; do [ "$a" = --headless ] && headless=1; done
integrate() {
    id=$1; mopt=$2; media=$(cd "$3" 2>/dev/null && pwd) || return 0
    [ -z "${CWFA_NO_DESKTOP_INTEGRATION:-}" ] || return 0
    [ -z "$headless" ] || [ -n "${CWFA_DESKTOP_INTEGRATION:-}" ] || return 0
    if [ -n "$installed" ]; then
        # installed: entries start "cwfa-launch <program> --user-data" and the media come from media.conf; a package that
        # ships the .desktop file itself (the RPM) only gets its icons made
        c=$(conf_get cd); [ -d "$c" ] || c=
        e=$(conf_get es_cw); [ -d "$e" ] || e=
        f=$(conf_get es_fa); [ -d "$f" ] || f=
        case "$mopt" in --cd) c=$media ;; --es-cd) e=$media ;; --es-fa-cd) f=$media ;; esac
        set -- --cwfa "$here/bin/cwfa" --launcher "$here/cwfa-launch" --media-conf --cd "$c" --es-cd "$e" --es-fa-cd "$f" --only "$id" --quiet
        [ ! -f "/usr/share/applications/cwfa-$id.desktop" ] || set -- "$@" --icons-only
        LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$here/cwfa-install-desktop" "$@" >/dev/null 2>&1 || true
        return 0
    fi
    set -- --cwfa "$here/bin/cwfa" --launcher "${CWFA_PACKAGE:-$0}" --cd "" --es-cd "" --es-fa-cd "" --only "$id" --quiet
    [ -z "$own_c" ] && set -- "$@" --data-dir "$home"
    LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$here/cwfa-install-desktop" "$@" "$mopt" "$media" >/dev/null 2>&1 || true
}

# Spanish (1.0) disks: a directory with INSTALAR.LST, or a directory of DISCOn / dN disk directories. The
# programs are KWAJ-compressed there and cwfa expands everything from the media's own setup script.
# data_dir DEFAULT_NAME USER_NAME: the drive C: directory (absolute), announced on stderr
data_dir() {
    if [ -z "$home" ]; then
        if [ -n "$user_data" ]; then home=$data/$2
        elif [ -n "${CWFA_HOME:-}" ]; then home=$CWFA_HOME/$prog; else home=$PWD/$1; fi
    fi
    case "$home" in /*) ;; *) home=$PWD/$home ;; esac
    echo "cwfa: your data (users, saved work, WIN.INI = drive C:) is in $home" >&2
}
if [ -z "$exe" ] && [ -d "$cd_dir" ] && { ls "$cd_dir" | grep -qi '^INSTALAR\.LST$' || ls "$cd_dir" | grep -qiE '^(disco|d)[0-9]+$'; }; then
    export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    if [ -n "$own_c" ]; then
        case "$prog" in artist) integrate es-artist --es-fa-cd "$cd_dir" ;; *) integrate es-writer --es-cd "$cd_dir" ;; esac
        exec "$here/bin/cwfa" "--es-$prog" "$cd_dir" --scale "${CWFA_SCALE:-2}" --quiet "$@"
    fi
    data_dir cwfa-data-es data-es
    mkdir -p "$home"
    case "$prog" in artist) integrate es-artist --es-fa-cd "$cd_dir" ;; *) integrate es-writer --es-cd "$cd_dir" ;; esac
    exec "$here/bin/cwfa" "--es-$prog" "$cd_dir" --install-dir "$home" --scale "${CWFA_SCALE:-2}" --quiet "$@"
fi

if [ -z "$exe" ]; then
    case "$prog" in writer) name=WRITER ;; artist) name=ARTIST ;; esac
    for cand in "$cd_dir/SETUP/$name.EX_" "$cd_dir/SETUP/$name.EXE" "$cd_dir/setup/$name.ex_" "$cd_dir/setup/$name.exe"; do
        [ -f "$cand" ] && { exe=$cand; break; }
    done
    [ -n "$exe" ] || { echo "cwfa: $name.EX_ not found under $cd_dir/SETUP: is this the Creative Writer & Fine Artist CD?" >&2; exit 1; }
fi
magic=$(head -c 4 "$exe" | od -An -c | tr -d ' ')
case "$magic" in
    MZ*) ;;
    SZDD) echo "cwfa: $exe is compressed (SZDD): expand it first (for example with msexpand) and pass it with --exe" >&2; exit 1 ;;
    *) echo "cwfa: $exe is not a Windows program" >&2; exit 1 ;;
esac

export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ -n "$own_c" ]; then
    integrate "$prog" --cd "$cd_dir"
    exec "$here/bin/cwfa" --exe "$exe" --cd "$cd_dir" --scale "${CWFA_SCALE:-2}" --quiet "$@"
fi
data_dir cwfa-data data
mkdir -p "$home"
integrate "$prog" --cd "$cd_dir"
exec "$here/bin/cwfa" --exe "$exe" --cd "$cd_dir" --install-dir "$home" --scale "${CWFA_SCALE:-2}" --quiet "$@"
